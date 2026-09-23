#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "profile/profile_service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>

#include <sqlite3.h>

namespace {

class TempRoot {
 public:
  TempRoot() {
    path_ = std::filesystem::temp_directory_path() /
            ("xrm-profile-i10-" + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TempRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct ProfileFixture {
  TempRoot root;
  std::unique_ptr<xrm::Store> store;
  std::unique_ptr<xrm::ProfileService> service;
  xrm::ProfileSettings profile;

  ProfileFixture() {
    auto opened = xrm::Store::open(root.path() / "manager.sqlite");
    REQUIRE(opened);
    store = std::move(*opened);
    service = std::make_unique<xrm::ProfileService>(*store, root.path() / "profiles");
    auto created = service->create({"home", "core-system", "local", {}});
    REQUIRE(created);
    profile = std::move(*created);
  }
};

}  // namespace

TEST_CASE("MOD-02/03 all native modules can be independently read, edited and toggled", "[MOD-02][MOD-03]") {
  ProfileFixture fixture;
  const auto views = fixture.service->modules(fixture.profile.id);
  REQUIRE(views);
  REQUIRE(views->size() == 15);

  const std::string stats_text = "// keep this comment\n{\"stats\":{},\"unknown\":[1,true]}\n";
  auto before = fixture.service->module(fixture.profile.id, "stats");
  REQUIRE(before);
  REQUIRE_FALSE(before->document);

  xrm::ModuleEdit edit{"stats", {std::nullopt}, stats_text};
  REQUIRE(fixture.service->save_module(fixture.profile.id, edit));
  REQUIRE(fixture.service->include_module(fixture.profile.id, "stats", true));
  auto after = fixture.service->module(fixture.profile.id, "stats");
  REQUIRE(after);
  REQUIRE(after->setting.included);
  REQUIRE(after->document);
  CHECK(after->document->source_text == stats_text);
  CHECK(after->document->filename == "08_stats.json");

  REQUIRE(fixture.service->include_module(fixture.profile.id, "stats", false));
  after = fixture.service->module(fixture.profile.id, "stats");
  REQUIRE(after);
  CHECK_FALSE(after->setting.included);
  REQUIRE(after->document);
  CHECK(after->document->source_text == stats_text);
}

TEST_CASE("MOD-02 each of the fifteen module files saves its original text independently", "[MOD-02]") {
  struct Expected { const char* key; const char* filename; };
  constexpr std::array expected{
      Expected{"log", "00_log.json"}, Expected{"dns", "01_dns.json"},
      Expected{"inbounds", "02_inbounds.json"}, Expected{"outbounds", "03_outbounds.json"},
      Expected{"routing", "04_routing.json"}, Expected{"observatory", "05_observatory.json"},
      Expected{"api", "06_api.json"}, Expected{"metrics", "07_metrics.json"},
      Expected{"stats", "08_stats.json"}, Expected{"policy", "09_policy.json"},
      Expected{"fakedns", "10_fakedns.json"}, Expected{"geodata", "11_geodata.json"},
      Expected{"env", "12_env.json"}, Expected{"version", "13_version.json"},
      Expected{"burstObservatory", "14_burstObservatory.json"},
  };
  ProfileFixture fixture;
  for (const auto& item : expected) {
    INFO(item.key);
    auto before = fixture.service->modules(fixture.profile.id);
    REQUIRE(before);
    const std::string raw = "// " + std::string(item.key) + " source\n{\"unknown\":[null,7]}\n";
    const auto current = fixture.service->module(fixture.profile.id, item.key);
    REQUIRE(current);
    const auto expected_text = current->document
        ? std::optional<std::string>(current->document->source_text) : std::nullopt;
    CHECK(current->setting.filename == item.filename);
    CHECK(current->editor_support == "native");
    REQUIRE(fixture.service->save_module(fixture.profile.id,
        {item.key, {expected_text}, raw}));
    REQUIRE(fixture.service->include_module(fixture.profile.id, item.key, true));
    auto enabled = fixture.service->module(fixture.profile.id, item.key);
    REQUIRE(enabled);
    CHECK(enabled->setting.included);
    REQUIRE(fixture.service->include_module(fixture.profile.id, item.key, false));
    for (const auto& previous : *before) {
      const auto actual = fixture.service->module(fixture.profile.id, previous.descriptor.key);
      REQUIRE(actual);
      if (previous.descriptor.key == item.key) {
        REQUIRE(actual->document);
        CHECK(actual->document->source_text == raw);
        CHECK_FALSE(actual->setting.included);
      } else {
        CHECK(actual->document.has_value() == previous.document.has_value());
        if (actual->document && previous.document) {
          CHECK(actual->document->filename == previous.document->filename);
          CHECK(actual->document->source_text == previous.document->source_text);
        }
      }
    }
  }
}

TEST_CASE("MOD-04 raw edits preserve malformed JSONC bytes", "[MOD-04]") {
  ProfileFixture fixture;
  const std::string source = "{\n  // comment with https://example.invalid/path\n  \"dns\": [],\n  \"unknown\": {\"n\": 1}\n}\n";
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"dns", {std::nullopt}, source}));
  const auto result = fixture.service->module(fixture.profile.id, "dns");
  REQUIRE(result);
  REQUIRE(result->document);
  CHECK(result->document->source_text == source);
}

TEST_CASE("MOD-05 module conflict checks only the edited file", "[MOD-05]") {
  ProfileFixture fixture;
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"policy", {std::nullopt}, "{\"policy\":{\"levels\":{}}}\n"}));
  auto opened = fixture.service->module(fixture.profile.id, "policy");
  REQUIRE(opened);
  REQUIRE(opened->document);
  const auto base = opened->document->source_text;
  const auto profile_dir = fixture.root.path() / "profiles" / fixture.profile.id;
  {
    std::ofstream external(profile_dir / "modules" / "09_policy.json", std::ios::binary | std::ios::trunc);
    external << "{\"policy\":{\"system\":{\"statsOutboundUplink\":true}}}\n";
  }
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"dns", {std::nullopt}, "{\"dns\":[]}\n"}));
  const auto conflict = fixture.service->save_module(fixture.profile.id,
      {"policy", {base}, "{\"policy\":{}}\n"});
  REQUIRE_FALSE(conflict);
  CHECK(conflict.error().code == xrm::ErrorCode::conflict);
  CHECK(read(profile_dir / "modules" / "09_policy.json") ==
        "{\"policy\":{\"system\":{\"statsOutboundUplink\":true}}}\n");
}

TEST_CASE("MOD-08 module filenames cannot collide, escape profile or use node tail slot", "[MOD-08]") {
  ProfileFixture fixture;
  const std::string dns_source = "{\"dns\":[],\"untouched\":true}\n";
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"dns", {std::nullopt}, dns_source}));
  REQUIRE(fixture.service->rename_module_file(fixture.profile.id, "dns", "custom/dns-source.json"));
  const auto renamed = fixture.service->module(fixture.profile.id, "dns");
  REQUIRE(renamed);
  REQUIRE(renamed->document);
  CHECK(renamed->document->filename == "custom/dns-source.json");
  CHECK(renamed->document->source_text == dns_source);
  CHECK_FALSE(std::filesystem::exists(fixture.root.path() / "profiles" / fixture.profile.id /
                                      "modules" / "01_dns.json"));
  CHECK_FALSE(fixture.service->rename_module_file(fixture.profile.id, "log", "custom/dns-source.json"));
  CHECK_FALSE(fixture.service->rename_module_file(fixture.profile.id, "dns", "00_log.json"));
  CHECK_FALSE(fixture.service->rename_module_file(fixture.profile.id, "dns", "../outside.json"));
  CHECK_FALSE(fixture.service->rename_module_file(fixture.profile.id, "outbounds", "90_nodes_tail.json"));
  auto profile = fixture.service->get(fixture.profile.id);
  REQUIRE(profile);
  const auto profile_file = fixture.root.path() / "profiles" / fixture.profile.id / "profile.json";
  const auto profile_base = read(profile_file);
  for (auto& setting : profile->modules) {
    if (setting.key == "dns") setting.filename = "00_log.json";
  }
  const auto duplicate = fixture.service->save(fixture.profile.id, { {profile_base}, *profile });
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == xrm::ErrorCode::conflict);
}

TEST_CASE("MOD-07 Profile environment persists separately from Xray env module", "[MOD-07]") {
  TempRoot root;
  xrm::Id id;
  {
    auto opened = xrm::Store::open(root.path() / "manager.sqlite");
    REQUIRE(opened);
    xrm::ProfileService profiles(**opened, root.path() / "profiles");
    auto created = profiles.create({"launch context", "core-system", "local", {}});
    REQUIRE(created);
    auto settings = *created;
    settings.environment.cwd = "/tmp/xrm cwd";
    settings.environment.process_environment = {{"XDG_CONFIG_HOME", "/tmp/profile-config"}};
    settings.environment.asset_dir = "/tmp/xray-assets";
    settings.selected = xrm::Target{xrm::TargetKind::node, "node-uuid"};
    settings.append_selected_default = false;
    const auto profile_file = root.path() / "profiles" / settings.id / "profile.json";
    const auto base = read(profile_file);
    REQUIRE(profiles.save(settings.id, {{base}, settings}));
    id = settings.id;
  }
  auto reopened = xrm::Store::open(root.path() / "manager.sqlite");
  REQUIRE(reopened);
  xrm::ProfileService profiles(**reopened, root.path() / "profiles");
  auto loaded = profiles.get(id);
  REQUIRE(loaded);
  CHECK(loaded->environment.cwd == "/tmp/xrm cwd");
  CHECK(loaded->environment.process_environment.at("XDG_CONFIG_HOME") == "/tmp/profile-config");
  CHECK(loaded->environment.asset_dir == std::filesystem::path("/tmp/xray-assets"));
  REQUIRE(loaded->selected);
  CHECK(loaded->selected->kind == xrm::TargetKind::node);
  CHECK_FALSE(loaded->append_selected_default);
}

TEST_CASE("MOD-07 Xray env and version modules remain native files and are not synthesized", "[MOD-07]") {
  ProfileFixture fixture;
  const std::string env_text = "{\"env\":{\"XRAY_LOCATION_ASSET\":\"/custom/assets\"}}\n";
  const std::string version_text = "{\"version\":{\"min\":\"1.8.0\"}}\n";
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"env", {std::nullopt}, env_text}));
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"version", {std::nullopt}, version_text}));
  const auto env = fixture.service->module(fixture.profile.id, "env");
  const auto version = fixture.service->module(fixture.profile.id, "version");
  REQUIRE(env);
  REQUIRE(version);
  REQUIRE(env->document);
  REQUIRE(version->document);
  CHECK(env->document->filename == "12_env.json");
  CHECK(env->document->source_text == env_text);
  CHECK(version->document->filename == "13_version.json");
  CHECK(version->document->source_text == version_text);
  CHECK(fixture.profile.environment.process_environment.empty());
  CHECK_FALSE(fixture.profile.environment.cwd == "/custom/assets");
}

TEST_CASE("Profile clone copies source modules without copying database inventory", "[DB-05]") {
  ProfileFixture fixture;
  const std::string custom = "{\"routing\":{\"rules\":[{\"domain\":[\"domain:example.test\"]}]}}\n";
  const auto original = fixture.service->module(fixture.profile.id, "routing");
  REQUIRE(original);
  REQUIRE(original->document);
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"routing", {original->document->source_text}, custom}));
  auto cloned = fixture.service->clone(fixture.profile.id, "copy");
  REQUIRE(cloned);
  CHECK(cloned->id != fixture.profile.id);
  CHECK(cloned->name == "copy");
  const auto copy = fixture.service->module(cloned->id, "routing");
  REQUIRE(copy);
  REQUIRE(copy->document);
  CHECK(copy->document->source_text == custom);
}

TEST_CASE("Profile create/list/show/remove lifecycle uses the registered file path", "[DB-05]") {
  TempRoot root;
  auto opened = xrm::Store::open(root.path() / "manager.sqlite");
  REQUIRE(opened);
  xrm::ProfileService profiles(**opened, root.path() / "profiles");
  auto created = profiles.create({"work", "core-system", "local", {}});
  REQUIRE(created);
  const auto directory = root.path() / "profiles" / created->id;
  CHECK(std::filesystem::is_regular_file(directory / "profile.json"));
  auto listed = profiles.list();
  REQUIRE(listed);
  REQUIRE(listed->size() == 1);
  CHECK(listed->front().id == created->id);
  auto shown = profiles.get(created->id);
  REQUIRE(shown);
  CHECK(shown->name == "work");
  REQUIRE(profiles.remove(created->id));
  CHECK_FALSE(std::filesystem::exists(directory));
  CHECK_FALSE(profiles.get(created->id));
  listed = profiles.list();
  REQUIRE(listed);
  CHECK(listed->empty());
}

TEST_CASE("DB-05 Profile and source module text live in files, not SQLite mirrors", "[DB-05]") {
  TempRoot root;
  xrm::Id id;
  {
    auto opened = xrm::Store::open(root.path() / "manager.sqlite");
    REQUIRE(opened);
    xrm::ProfileService profiles(**opened, root.path() / "profiles");
    auto created = profiles.create({"home", "core-system", "local", {}});
    REQUIRE(created);
    id = created->id;
    REQUIRE(profiles.save_module(id, {"fakedns", {std::nullopt}, "[ {\"ipPool\":\"198.18.0.0/15\"} ]\n"}));
    REQUIRE(profiles.include_module(id, "fakedns", true));
  }

  {
    std::ofstream external(root.path() / "profiles" / id / "modules" / "10_fakedns.json",
                           std::ios::binary | std::ios::trunc);
    external << "[ {\"ipPool\":\"203.0.113.0/24\",\"external\":true} ]\n";
  }
  auto reopened = xrm::Store::open(root.path() / "manager.sqlite");
  REQUIRE(reopened);
  xrm::ProfileService profiles(**reopened, root.path() / "profiles");
  auto snapshot = profiles.snapshot(id);
  REQUIRE(snapshot);
  CHECK(snapshot->profile.id == id);
  const auto fakedns = std::find_if(snapshot->modules.begin(), snapshot->modules.end(),
      [](const xrm::ModuleDocument& doc) { return doc.key == "fakedns"; });
  REQUIRE(fakedns != snapshot->modules.end());
  CHECK(fakedns->source_text == "[ {\"ipPool\":\"203.0.113.0/24\",\"external\":true} ]\n");
  sqlite3* raw_database = nullptr;
  REQUIRE(sqlite3_open((root.path() / "manager.sqlite").c_str(), &raw_database) == SQLITE_OK);
  sqlite3_stmt* query = nullptr;
  REQUIRE(sqlite3_prepare_v2(raw_database,
      "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name", -1, &query, nullptr) == SQLITE_OK);
  std::set<std::string> tables;
  while (sqlite3_step(query) == SQLITE_ROW) {
    tables.emplace(reinterpret_cast<const char*>(sqlite3_column_text(query, 0)));
  }
  sqlite3_finalize(query);
  sqlite3_close(raw_database);
  CHECK(tables == std::set<std::string>{"cores", "nodes", "profiles"});
}

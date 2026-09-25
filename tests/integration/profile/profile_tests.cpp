#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "profile/forms.hpp"
#include "profile/profile_service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
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

std::optional<std::string> module_base(xrm::ProfileService& service,
                                       const xrm::Id& id, std::string_view key) {
  auto view = service.module(id, key);
  if (!view) throw std::runtime_error(view.error().message);
  return view->document ? std::optional(view->document->source_text) : std::nullopt;
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

TEST_CASE("local first-use Profile provides a loopback SOCKS entrance", "[PROFILE][first-use]") {
  ProfileFixture fixture;
  auto inbound = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(inbound);
  REQUIRE(inbound->setting.included);
  REQUIRE(inbound->document);
  const auto source = xrm::Json::parse(inbound->document->source_text);
  REQUIRE(source.contains("inbounds"));
  REQUIRE(source.at("inbounds").is_array());
  REQUIRE(source.at("inbounds").size() == 1);
  const auto& entrance = source.at("inbounds").front();
  CHECK(entrance.value("listen", "") == "127.0.0.1");
  CHECK(entrance.value("port", 0) == 10808);
  CHECK(entrance.value("protocol", "") == "socks");
  CHECK(entrance.at("settings").value("auth", "") == "noauth");
}

TEST_CASE("UI-11 local template can be saved before a core is registered and offers all native examples", "[PROFILE][UI-11]") {
  TempRoot root;
  auto opened = xrm::Store::open(root.path() / "manager.sqlite");
  REQUIRE(opened);
  xrm::ProfileService service(**opened, root.path() / "profiles");
  auto created = service.create({"No core yet", "", "local", {}});
  REQUIRE(created);
  CHECK(created->core_id.empty());
  REQUIRE(created->enabled_source_ids);
  CHECK(created->enabled_source_ids->empty());
  REQUIRE(created->modules.size() == 15);
  const std::set<std::string> expected_enabled{"log", "dns", "inbounds", "outbounds", "routing"};
  for (const auto& module : created->modules) {
    INFO(module.key);
    CHECK(module.included == expected_enabled.contains(module.key));
    auto view = service.module(created->id, module.key);
    REQUIRE(view);
    REQUIRE(view->document);
    const auto parsed = xrm::Json::parse(view->document->source_text);
    CHECK(parsed.contains(module.key));
    CHECK_FALSE(view->document->source_text.find("placeholder") != std::string::npos);
  }
}

TEST_CASE("UI-11 legacy Profile without source binding remains all-sources and new binding round trips", "[PROFILE][UI-11]") {
  ProfileFixture fixture;
  auto file = fixture.root.path() / "profiles" / fixture.profile.id / "profile.json";
  auto legacy = xrm::Json::parse(read(file));
  legacy.erase("enabled_source_ids");
  {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << legacy.dump(2) << '\n';
  }
  auto loaded = fixture.service->get(fixture.profile.id);
  REQUIRE(loaded);
  CHECK_FALSE(loaded->enabled_source_ids);

  loaded->enabled_source_ids = std::vector<xrm::Id>{"source-A", "source-B"};
  const auto base = read(file);
  REQUIRE(fixture.service->save(fixture.profile.id, {{base}, *loaded}));
  auto reopened = fixture.service->get(fixture.profile.id);
  REQUIRE(reopened);
  REQUIRE(reopened->enabled_source_ids);
  CHECK(*reopened->enabled_source_ids == std::vector<xrm::Id>{"source-A", "source-B"});
  auto cloned = fixture.service->clone(fixture.profile.id, "Office");
  REQUIRE(cloned);
  REQUIRE(cloned->enabled_source_ids);
  CHECK(*cloned->enabled_source_ids == *reopened->enabled_source_ids);
}

TEST_CASE("an existing empty-inbounds Profile needs an explicit source edit and include", "[PROFILE][legacy-inbound]") {
  ProfileFixture fixture;
  auto existing = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(existing);
  REQUIRE(existing->document);
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      xrm::ModuleEdit{"inbounds", {existing->document->source_text}, "{\"inbounds\": []}\n"}));
  REQUIRE(fixture.service->include_module(fixture.profile.id, "inbounds", false));

  const auto before = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(before);
  REQUIRE(before->document);
  CHECK_FALSE(before->setting.included);
  CHECK(xrm::Json::parse(before->document->source_text)["inbounds"].empty());

  // Merely opening/planning the action must not persist a listening socket.
  const xrm::Json proposed = {
      {"tag", "local-socks"}, {"listen", "127.0.0.1"}, {"port", 10808},
      {"protocol", "socks"}, {"settings", {{"auth", "noauth"}, {"udp", true}}}};
  auto edit = xrm::edit_native_list(*before->document,
      {xrm::CollectionKind::inbounds, xrm::ListAction::insert, 0, std::nullopt, proposed});
  REQUIRE(edit);
  auto unchanged = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(unchanged);
  REQUIRE(unchanged->document);
  CHECK(unchanged->document->source_text == before->document->source_text);
  CHECK_FALSE(unchanged->setting.included);

  REQUIRE(fixture.service->save_module(fixture.profile.id, *edit));
  auto saved = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(saved);
  REQUIRE(saved->document);
  CHECK_FALSE(saved->setting.included);
  const auto body = xrm::Json::parse(saved->document->source_text);
  REQUIRE(body["inbounds"].size() == 1);
  CHECK(body["inbounds"][0] == proposed);

  REQUIRE(fixture.service->include_module(fixture.profile.id, "inbounds", true));
  auto included = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(included);
  CHECK(included->setting.included);
  REQUIRE(included->document);
  CHECK(xrm::Json::parse(included->document->source_text)["inbounds"][0] == proposed);
}

TEST_CASE("an included legacy empty-inbounds Profile remains empty until the user saves", "[PROFILE][legacy-inbound]") {
  ProfileFixture fixture;
  auto original = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(original);
  REQUIRE(original->document);
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      xrm::ModuleEdit{"inbounds", {original->document->source_text}, "{\"inbounds\": []}\n"}));

  auto legacy = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(legacy);
  REQUIRE(legacy->document);
  REQUIRE(legacy->setting.included);
  REQUIRE(xrm::Json::parse(legacy->document->source_text)["inbounds"].empty());

  const xrm::Json proposed = {
      {"tag", "local-socks"}, {"listen", "127.0.0.1"}, {"port", 10808},
      {"protocol", "socks"}, {"settings", {{"auth", "noauth"}, {"udp", true}}}};
  auto preview = xrm::edit_native_list(*legacy->document,
      {xrm::CollectionKind::inbounds, xrm::ListAction::insert, 0, std::nullopt, proposed});
  REQUIRE(preview);
  auto before_confirmation = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(before_confirmation);
  REQUIRE(before_confirmation->document);
  CHECK(before_confirmation->document->source_text == legacy->document->source_text);

  REQUIRE(fixture.service->save_module(fixture.profile.id, *preview));
  auto saved = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(saved);
  REQUIRE(saved->document);
  CHECK(saved->setting.included);
  CHECK(xrm::Json::parse(saved->document->source_text)["inbounds"][0] == proposed);
}

TEST_CASE("custom inbound is preserved while inspecting a Profile for a local entrance", "[PROFILE][custom-inbound]") {
  ProfileFixture fixture;
  const std::string original = R"({"inbounds":[{"tag":"my-server","listen":"127.0.0.1","port":14001,"protocol":"vless","settings":{"clients":[],"unknown":"kept"},"vendor":{"flag":true}}]})";
  auto before = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(before);
  REQUIRE(before->document);
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      xrm::ModuleEdit{"inbounds", {before->document->source_text}, original}));

  auto inspected = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(inspected);
  REQUIRE(inspected->document);
  REQUIRE(inspected->setting.included);
  const auto parsed = xrm::Json::parse(inspected->document->source_text);
  REQUIRE(parsed["inbounds"].size() == 1);
  CHECK(parsed["inbounds"][0]["protocol"] == "vless");
  CHECK(parsed["inbounds"][0]["vendor"]["flag"] == true);

  auto after = fixture.service->module(fixture.profile.id, "inbounds");
  REQUIRE(after);
  REQUIRE(after->document);
  CHECK(after->document->source_text == original);
  CHECK(after->setting.included);
}

TEST_CASE("MOD-02/03 all native modules can be independently read, edited and toggled", "[MOD-02][MOD-03]") {
  ProfileFixture fixture;
  const auto views = fixture.service->modules(fixture.profile.id);
  REQUIRE(views);
  REQUIRE(views->size() == 15);

  const std::string stats_text = "// keep this comment\n{\"stats\":{},\"unknown\":[1,true]}\n";
  auto before = fixture.service->module(fixture.profile.id, "stats");
  REQUIRE(before);
  REQUIRE(before->document);
  CHECK_FALSE(before->setting.included);

  xrm::ModuleEdit edit{"stats", {before->document->source_text}, stats_text};
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
      {"dns", {module_base(*fixture.service, fixture.profile.id, "dns")}, source}));
  const auto result = fixture.service->module(fixture.profile.id, "dns");
  REQUIRE(result);
  REQUIRE(result->document);
  CHECK(result->document->source_text == source);
}

TEST_CASE("MOD-05 module conflict checks only the edited file", "[MOD-05]") {
  ProfileFixture fixture;
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"policy", {module_base(*fixture.service, fixture.profile.id, "policy")}, "{\"policy\":{\"levels\":{}}}\n"}));
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
      {"dns", {module_base(*fixture.service, fixture.profile.id, "dns")}, "{\"dns\":[]}\n"}));
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
      {"dns", {module_base(*fixture.service, fixture.profile.id, "dns")}, dns_source}));
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
      {"env", {module_base(*fixture.service, fixture.profile.id, "env")}, env_text}));
  REQUIRE(fixture.service->save_module(fixture.profile.id,
      {"version", {module_base(*fixture.service, fixture.profile.id, "version")}, version_text}));
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
    REQUIRE(profiles.save_module(id, {"fakedns", {module_base(profiles, id, "fakedns")}, "[ {\"ipPool\":\"198.18.0.0/15\"} ]\n"}));
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
  CHECK(tables == std::set<std::string>{"cores", "nodes", "profiles", "program_settings",
                                        "runtime_state", "subscriptions"});
  CHECK_FALSE(tables.contains("modules"));
}

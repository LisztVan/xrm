#include <catch2/catch_test_macros.hpp>

#include "contract/dtos.hpp"
#include "infra/store.hpp"

#include <filesystem>
#include <chrono>
#include <string>

namespace {

class TempDatabase {
 public:
  TempDatabase() {
    path_ = std::filesystem::temp_directory_path() /
            ("xrm-store-test-" + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++sequence_) + ".sqlite");
  }
  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(path_.string() + "-wal", ignored);
    std::filesystem::remove(path_.string() + "-shm", ignored);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  inline static std::uint64_t sequence_ = 0;
};

xrm::Node node(std::string id, std::string name, std::string secret) {
  return {std::move(id), std::nullopt, std::move(name), std::nullopt,
          {"blue", "manual"}, true, false,
          xrm::RawOutbound{xrm::Json{{"protocol", "vless"}, {"password", std::move(secret)},
                                     {"port", 443}, {"enabled", true}}}};
}

}  // namespace

TEST_CASE("DB-01 manual node inventory survives reopening SQLite", "[DB-01]") {
  TempDatabase database;
  {
    auto opened = xrm::Store::open(database.path());
    REQUIRE(opened);
    REQUIRE((*opened)->put_node(node("id-a", "same name", "secret-a")));
    REQUIRE((*opened)->put_node(node("id-b", "same name", "secret-b")));
    const auto page = (*opened)->list_nodes({}, {0, 10});
    REQUIRE(page);
    REQUIRE(page->total == 2);
    REQUIRE(page->items[0].id != page->items[1].id);
  }
  auto reopened = xrm::Store::open(database.path());
  REQUIRE(reopened);
  auto found = (*reopened)->get_node("id-b");
  REQUIRE(found);
  REQUIRE(std::get<xrm::RawOutbound>(found->settings).value.at("password") == "secret-b");
  REQUIRE(found->labels == std::vector<std::string>{"blue", "manual"});

  const auto summary = xrm::summarize_node(*found);
  const auto dto = xrm::Json(summary).dump();
  REQUIRE(dto.find("secret-b") == std::string::npos);
  REQUIRE(dto.find("password") == std::string::npos);
}

TEST_CASE("Store reports missing IDs and applies bounded inventory filters", "[DB-01]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path());
  REQUIRE(opened);
  REQUIRE((*opened)->put_node(node("id-a", "first", "secret-a")));
  REQUIRE((*opened)->put_node(node("id-b", "second", "secret-b")));

  xrm::NodeQuery query;
  query.text = "cond";
  const auto page = (*opened)->list_nodes(query, {0, 1});
  REQUIRE(page);
  REQUIRE(page->total == 1);
  REQUIRE(page->items.front().original_name == "second");
  REQUIRE_FALSE((*opened)->get_node("missing"));
}

TEST_CASE("Node storage JSON preserves nullable and unknown native values", "[CT-02]") {
  const xrm::Node source = node("id-1", "raw", "fixture-secret");
  const auto encoded = xrm::Json(source);
  const auto decoded = encoded.get<xrm::Node>();
  REQUIRE(decoded.id == source.id);
  REQUIRE(decoded.source_id == source.source_id);
  REQUIRE(std::get<xrm::RawOutbound>(decoded.settings).value ==
          std::get<xrm::RawOutbound>(source.settings).value);
  REQUIRE(encoded.at("source_id").is_null());
}

TEST_CASE("DB-01 core registrations and Profile paths are persistent indexes", "[DB-01]") {
  TempDatabase database;
  {
    auto opened = xrm::Store::open(database.path());
    REQUIRE(opened);
    REQUIRE((*opened)->put_core({"core-1", "system", "/usr/bin/xray", "Xray fixture", false}));
    REQUIRE((*opened)->register_profile("profile-1", "/tmp/xrm-profile-fixture"));
  }
  auto reopened = xrm::Store::open(database.path());
  REQUIRE(reopened);
  const auto cores = (*reopened)->cores();
  REQUIRE(cores);
  REQUIRE(cores->size() == 1);
  REQUIRE(cores->front().path == "/usr/bin/xray");
  const auto path = (*reopened)->profile_path("profile-1");
  REQUIRE(path);
  REQUIRE(*path == "/tmp/xrm-profile-fixture");
  REQUIRE((*reopened)->profile_ids() == std::vector<xrm::Id>{"profile-1"});
}

TEST_CASE("XRM program settings persist without touching Profile or core state", "[SETTINGS]") {
  TempDatabase database;
  {
    auto opened = xrm::Store::open(database.path());
    REQUIRE(opened);
    REQUIRE((*opened)->put_core({"core-1", "system", "/usr/bin/xray", "fixture", false}));
    REQUIRE((*opened)->register_profile("profile-1", "/tmp/xrm-settings-fixture"));
    REQUIRE((*opened)->save_runtime(xrm::RuntimePersistent{.desired_running = true}));
    auto defaults = (*opened)->load_program_settings();
    REQUIRE(defaults);
    REQUIRE(defaults->at("appearance").at("theme") == "default");
    REQUIRE(defaults->at("appearance").at("icon_mode") == "unicode");
    REQUIRE(defaults->at("editor").at("argv") == xrm::Json::array());
    auto updated = (*opened)->update_program_settings(xrm::Json{
        {"appearance", {{"theme", "dark"}, {"reduce_motion", true}}},
        {"editor", {{"argv", xrm::Json::array({"/usr/bin/vi", "-n"})}}}});
    REQUIRE(updated);
    REQUIRE(updated->at("appearance").at("theme") == "dark");
    REQUIRE(updated->at("appearance").at("reduce_motion") == true);
    REQUIRE(updated->at("appearance").at("icon_mode") == "unicode");
    REQUIRE(updated->at("editor").at("argv") == xrm::Json::array({"/usr/bin/vi", "-n"}));
  }
  auto reopened = xrm::Store::open(database.path());
  REQUIRE(reopened);
  auto persisted = (*reopened)->load_program_settings();
  REQUIRE(persisted);
  REQUIRE(persisted->at("appearance").at("theme") == "dark");
  REQUIRE(persisted->at("editor").at("argv") == xrm::Json::array({"/usr/bin/vi", "-n"}));
  REQUIRE((*reopened)->get_core("core-1"));
  REQUIRE((*reopened)->profile_path("profile-1"));
  const auto runtime = (*reopened)->load_runtime();
  REQUIRE(runtime);
  REQUIRE(runtime->desired_running);
}

TEST_CASE("Program settings reject unknown and malformed patches without partial writes", "[SETTINGS]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path());
  REQUIRE(opened);
  const auto original = (*opened)->load_program_settings();
  REQUIRE(original);
  REQUIRE_FALSE((*opened)->update_program_settings(xrm::Json{{"appearance", {{"theme", "dark"},
                                                              {"unknown", "unexpected"}}}}));
  REQUIRE_FALSE((*opened)->update_program_settings(xrm::Json{{"editor", {{"argv", "sh -c arbitrary"}}}}));
  REQUIRE_FALSE((*opened)->update_program_settings(xrm::Json{{"appearance", {{"color_mode", "bad"}}}}));
  REQUIRE_FALSE((*opened)->update_program_settings(xrm::Json{{"general", {{"imaginary", true}}}}));
  const auto unchanged = (*opened)->load_program_settings();
  REQUIRE(unchanged);
  REQUIRE(*unchanged == *original);
}

TEST_CASE("P4 test defaults persist separately from Xray native modules", "[P4-SETTINGS]") {
  TempDatabase database;
  auto opened = xrm::Store::open(database.path()); REQUIRE(opened);
  const auto saved = (*opened)->update_program_settings(xrm::Json{{"tests", {
      {"default_url", "http://127.0.0.1:18080/ping"}, {"timeout_ms", 4000},
      {"concurrency", 2}, {"max_body_bytes", 1048576}}}});
  REQUIRE(saved);
  CHECK(saved->at("tests").at("default_url") == "http://127.0.0.1:18080/ping");
  CHECK_FALSE((*opened)->update_program_settings(xrm::Json{{"tests", {
      {"concurrency", 1000}}}}));
  opened->reset();
  auto reopened = xrm::Store::open(database.path()); REQUIRE(reopened);
  auto persisted = (*reopened)->load_program_settings(); REQUIRE(persisted);
  CHECK(persisted->at("tests").at("max_body_bytes") == 1048576);
}

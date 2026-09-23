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
          xrm::Json{{"protocol", "vless"}, {"password", std::move(secret)},
                    {"port", 443}, {"enabled", true}}};
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
  REQUIRE(found->outbound.at("password") == "secret-b");
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
  REQUIRE(decoded.outbound == source.outbound);
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

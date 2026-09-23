#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "profile/forms.hpp"
#include "profile/profile_service.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace {
using namespace xrm;

struct TempRoot {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-module-actions-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  TempRoot() { std::filesystem::create_directories(path); }
  ~TempRoot() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Fixture {
  TempRoot root;
  std::unique_ptr<Store> store;
  std::unique_ptr<ProfileService> profiles;
  ProfileSettings profile;

  Fixture() {
    auto opened = Store::open(root.path / "manager.sqlite");
    REQUIRE(opened);
    store = std::move(*opened);
    profiles = std::make_unique<ProfileService>(*store, root.path / "profiles");
    auto created = profiles->create({"forms", "core-system", "local", {}});
    REQUIRE(created);
    profile = *created;
    REQUIRE(profiles->save_module(profile.id, {"policy", {std::nullopt},
      R"({"policy":{"levels":{"0":{"connIdle":60,"unknownLevel":true}},"system":{"statsOutboundUplink":false,"otherSystemField":{"keep":7}},"extraPolicyField":9}})"}));
    REQUIRE(profiles->save_module(profile.id, {"stats", {std::nullopt}, R"({"stats":{}})"}));
    REQUIRE(profiles->save_module(profile.id, {"metrics", {std::nullopt}, R"({"metrics":{"listen":"127.0.0.1:9555","unknown":true}})"}));
  }
};

}  // namespace

TEST_CASE("FORM-03 traffic-statistics plan is explicit, native, and preserves policy data", "[FORM-03]") {
  Fixture fixture;
  const auto before = fixture.profiles->snapshot(fixture.profile.id);
  REQUIRE(before);
  const auto original_policy = read(fixture.root.path / "profiles" / fixture.profile.id / "modules" / "09_policy.json");
  auto plan = plan_traffic_stats(*before, true, "127.0.0.1:9556");
  REQUIRE(plan);
  REQUIRE(plan->changes.size() == 3);
  CHECK(plan->changes[0].edit.key == "stats");
  CHECK(plan->changes[1].edit.key == "policy");
  CHECK(plan->changes[2].edit.key == "metrics");
  CHECK(plan->changes[0].include_after_save);
  CHECK(plan->changes[1].include_after_save);
  CHECK(plan->changes[2].include_after_save);
  CHECK(plan->changes[1].edit.text.find("otherSystemField") != std::string::npos);
  CHECK(plan->changes[1].edit.text.find("unknownLevel") != std::string::npos);
  CHECK(plan->changes[1].edit.text.find("extraPolicyField") != std::string::npos);
  CHECK(plan->changes[2].edit.text.find("127.0.0.1:9556") != std::string::npos);
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" / "09_policy.json") == original_policy);

  const auto saved = save_module_changes(*fixture.profiles, fixture.profile.id, *plan);
  REQUIRE_FALSE(saved.failure);
  CHECK(saved.saved == std::vector<std::string>{"stats", "policy", "metrics"});
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" / "09_policy.json").find("otherSystemField") != std::string::npos);
  CHECK_FALSE((fixture.profiles->get(fixture.profile.id))->selected.has_value());

  const auto current = fixture.profiles->snapshot(fixture.profile.id);
  REQUIRE(current);
  auto disabled = plan_traffic_stats(*current, false, "127.0.0.1:9556");
  REQUIRE(disabled);
  REQUIRE(disabled->changes.size() == 3);
  for (const auto& change : disabled->changes) CHECK_FALSE(change.include_after_save);
}

TEST_CASE("FORM-03 failed later native write reports exactly the saved prefix", "[FORM-03]") {
  Fixture fixture;
  auto snapshot = fixture.profiles->snapshot(fixture.profile.id);
  REQUIRE(snapshot);
  auto plan = plan_traffic_stats(*snapshot, true, "127.0.0.1:9556");
  REQUIRE(plan);

  const auto profile_dir = fixture.root.path / "profiles" / fixture.profile.id;
  const auto metrics_path = profile_dir / "modules" / "07_metrics.json";
  std::filesystem::remove(metrics_path);
  std::filesystem::create_directory(metrics_path);

  const auto saved = save_module_changes(*fixture.profiles, fixture.profile.id, *plan);
  REQUIRE(saved.failure);
  CHECK(saved.saved == std::vector<std::string>{"stats", "policy"});
  CHECK(saved.failure->subject == std::optional<std::string>{"metrics"});
  CHECK(read(profile_dir / "modules" / "08_stats.json").find("stats") != std::string::npos);
  CHECK(read(profile_dir / "modules" / "09_policy.json").find("statsOutboundUplink") != std::string::npos);
  CHECK(std::filesystem::is_directory(metrics_path));
  CHECK_FALSE((fixture.profiles->get(fixture.profile.id))->selected.has_value());
}

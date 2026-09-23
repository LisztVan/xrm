#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "infra/http.hpp"
#include "infra/process.hpp"
#include "profile/forms.hpp"
#include "profile/profile_service.hpp"
#include "server/api_router.hpp"
#include "server/application.hpp"
#include "subscription/subscription_service.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <uv.h>

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

struct ApplicationFixture {
  TempRoot root;
  uv_loop_t loop{};
  std::unique_ptr<Store> store;
  std::unique_ptr<ProcessService> processes;
  std::unique_ptr<HttpClient> http;
  std::unique_ptr<SubscriptionService> subscriptions;
  std::unique_ptr<Application> application;
  bool loop_initialized = false;

  ApplicationFixture() {
    auto opened = Store::open(root.path / "manager.sqlite");
    REQUIRE(opened);
    store = std::move(*opened);
    REQUIRE(uv_loop_init(&loop) == 0);
    loop_initialized = true;
    processes = std::make_unique<ProcessService>(loop);
    http = std::make_unique<HttpClient>(loop);
    subscriptions = std::make_unique<SubscriptionService>(*store, *http);
    application = std::make_unique<Application>(*store, *processes, *http, loop,
        root.path, "test", "boot-test", *subscriptions);
  }

  ~ApplicationFixture() {
    if (application) {
      bool drained = false;
      application->shutdown([&] { drained = true; });
      while (!drained) uv_run(&loop, UV_RUN_ONCE);
      application.reset();
    }
    subscriptions.reset();
    if (http) http->shutdown([] {});
    http.reset();
    if (processes) processes->shutdown([] {});
    processes.reset();
    if (loop_initialized) {
      uv_run(&loop, UV_RUN_DEFAULT);
      (void)uv_loop_close(&loop);
    }
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

TEST_CASE("FORM-03 Application RPC enables native traffic statistics and preserves unrelated settings",
          "[FORM-03][module.change_plan][module.save_changes]") {
  ApplicationFixture fixture;
  auto profile = fixture.application->profiles().create({"form-rpc", "core-system", "local", {}});
  REQUIRE(profile);
  REQUIRE(fixture.application->profiles().save_module(profile->id, {"policy", {std::nullopt},
      R"({"policy":{"levels":{"0":{"connIdle":61,"customLevel":{"keep":"level"}}},"system":{"statsOutboundUplink":false,"otherSystemField":{"keep":7}},"extraPolicyField":9}})"}));
  REQUIRE(fixture.application->profiles().save_module(profile->id, {"stats", {std::nullopt},
      R"({"stats":{}})"}));
  REQUIRE(fixture.application->profiles().save_module(profile->id, {"metrics", {std::nullopt},
      R"({"metrics":{"listen":"127.0.0.1:9555","customMetricField":{"keep":true}}})"}));

  ApiRouter router(*fixture.application);
  std::optional<RpcResponse> plan_response;
  router.dispatch("form-test-client", RpcRequest{1, 1, "module.change_plan",
      Json{{"profile_id", profile->id}, {"action", "enable_traffic_statistics"},
           {"arguments", Json{{"metrics_listen", "127.0.0.1:9556"}}}}},
      [&](RpcResponse response) { plan_response = std::move(response); });
  REQUIRE(plan_response);
  REQUIRE_FALSE(plan_response->error.has_value());
  REQUIRE(plan_response->result);
  const auto& plan = *plan_response->result;
  REQUIRE(plan.at("changes").size() == 3);
  CHECK(plan.at("changes").at(0).at("key") == "stats");
  CHECK(plan.at("changes").at(1).at("key") == "policy");
  CHECK(plan.at("changes").at(2).at("key") == "metrics");
  CHECK(plan.at("changes").at(1).at("text").get<std::string>().find("customLevel") != std::string::npos);
  CHECK(plan.at("changes").at(1).at("text").get<std::string>().find("otherSystemField") != std::string::npos);
  CHECK(plan.at("changes").at(2).at("text").get<std::string>().find("customMetricField") != std::string::npos);

  std::optional<RpcResponse> save_response;
  router.dispatch("form-test-client", RpcRequest{1, 2, "module.save_changes",
      Json{{"profile_id", profile->id}, {"plan", plan}}},
      [&](RpcResponse response) { save_response = std::move(response); });
  REQUIRE(save_response);
  REQUIRE_FALSE(save_response->error.has_value());
  REQUIRE(save_response->result);
  const auto& saved_result = *save_response->result;
  CHECK(saved_result.at("changed_modules") == Json::array({"stats", "policy", "metrics"}));
  CHECK(saved_result.at("failure").is_null());

  const auto module_dir = fixture.root.path / "profiles" / profile->id / "modules";
  const auto stats = Json::parse(read(module_dir / "08_stats.json"));
  const auto policy = Json::parse(read(module_dir / "09_policy.json")).at("policy");
  const auto metrics = Json::parse(read(module_dir / "07_metrics.json")).at("metrics");
  CHECK(stats == (Json{{"stats", Json::object()}}));
  CHECK(policy.at("levels").at("0").at("connIdle") == 61);
  CHECK(policy.at("levels").at("0").at("customLevel").at("keep") == "level");
  CHECK(policy.at("system").at("otherSystemField").at("keep") == 7);
  CHECK(policy.at("system").at("statsInboundUplink") == true);
  CHECK(policy.at("system").at("statsInboundDownlink") == true);
  CHECK(policy.at("system").at("statsOutboundUplink") == true);
  CHECK(policy.at("system").at("statsOutboundDownlink") == true);
  CHECK(policy.at("extraPolicyField") == 9);
  CHECK(metrics.at("listen") == "127.0.0.1:9556");
  CHECK(metrics.at("customMetricField").at("keep") == true);
  CHECK_FALSE(fixture.application->profiles().get(profile->id)->selected.has_value());
  CHECK(fixture.application->core_manager().status().state == CoreState::stopped);
}

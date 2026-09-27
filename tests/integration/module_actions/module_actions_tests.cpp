// XRM: Tests module actions in the client, daemon, persistence, and Xray
// integration suite.

#include <catch2/catch_test_macros.hpp>

#include "infra/http.hpp"
#include "infra/process.hpp"
#include "infra/store.hpp"
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
#include <set>
#include <thread>
#include <uv.h>

namespace {
using namespace xrm;

struct TempRoot {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xrm-module-actions-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempRoot() { std::filesystem::create_directories(path); }
  ~TempRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::string read(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::optional<std::string> module_base(ProfileService &profiles,
                                       const Id &profile_id,
                                       std::string_view key) {
  auto module = profiles.module(profile_id, key);
  REQUIRE(module);
  return module->document ? std::optional(module->document->source_text)
                          : std::nullopt;
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
    REQUIRE(profiles->save_module(
        profile.id,
        {"policy",
         {module_base(*profiles, profile.id, "policy")},
         R"({"policy":{"levels":{"0":{"connIdle":60,"unknownLevel":true}},"system":{"statsOutboundUplink":false,"otherSystemField":{"keep":7}},"extraPolicyField":9}})"}));
    REQUIRE(profiles->save_module(
        profile.id, {"stats",
                     {module_base(*profiles, profile.id, "stats")},
                     R"({"stats":{}})"}));
    REQUIRE(profiles->save_module(
        profile.id,
        {"metrics",
         {module_base(*profiles, profile.id, "metrics")},
         R"({"metrics":{"listen":"127.0.0.1:9555","unknown":true}})"}));
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
                                                root.path, "test", "boot-test",
                                                *subscriptions);
  }

  ~ApplicationFixture() {
    if (application) {
      bool drained = false;
      application->shutdown([&] { drained = true; });
      while (!drained)
        uv_run(&loop, UV_RUN_ONCE);
      application.reset();
    }
    subscriptions.reset();
    if (http)
      http->shutdown([] {});
    http.reset();
    if (processes)
      processes->shutdown([] {});
    processes.reset();
    if (loop_initialized) {
      uv_run(&loop, UV_RUN_DEFAULT);
      (void)uv_loop_close(&loop);
    }
  }
};

Node source_node(Id id, Id source_id) {
  StreamNode stream;
  stream.endpoint = {"127.0.0.1", 443};
  std::get<VlessSettings>(stream.protocol).id =
      "11111111-1111-4111-8111-111111111111";
  return {
      std::move(id), std::move(source_id), "example",   std::nullopt, {}, true,
      false,         std::move(stream),    std::nullopt};
}

void save_sources(ApplicationFixture &fixture, const ProfileSettings &original,
                  std::optional<std::vector<Id>> sources) {
  auto profile = fixture.application->profiles().get(original.id);
  REQUIRE(profile);
  profile->enabled_source_ids = std::move(sources);
  const auto file =
      fixture.root.path / "profiles" / original.id / "profile.json";
  REQUIRE(fixture.application->profiles().save(original.id,
                                               {{read(file)}, *profile}));
}

} // namespace

TEST_CASE("A rejected core switch restores the Profile's saved core",
          "[core][lifecycle]") {
  ApplicationFixture fixture;
  const CoreInstall original{"core-a", "original", XRM_TEST_FAKE_CORE,
                             "fixture", false};
  const CoreInstall replacement{"core-b", "replacement", XRM_TEST_FAKE_CORE,
                                "fixture", false};
  REQUIRE(fixture.store->put_core(original));
  REQUIRE(fixture.store->put_core(replacement));
  auto profile = fixture.application->profiles().create(
      {"core-switch", original.id, "local", {}});
  REQUIRE(profile);

  const auto confdir = fixture.root.path / "running-confdir";
  std::filesystem::create_directories(confdir);
  RunRecord running;
  running.run_id = "initial-run";
  running.profile_id = profile->id;
  running.core_id = original.id;
  running.spec = {original.path,
                  confdir,
                  fixture.root.path,
                  {{"XRM_FAKE_CORE_RUN_HANG", "1"}}};
  bool activated = false;
  REQUIRE(fixture.application->core_manager().activate(
      {ReplayRequest{running, original}, ActivationMode::start_or_replace},
      [&](Result<ActivationResult> result) {
        CHECK(result);
        activated = true;
      }));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (!activated && std::chrono::steady_clock::now() < deadline) {
    uv_run(&fixture.loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(activated);
  const auto status = fixture.application->core_manager().status();
  REQUIRE(status.active);

  REQUIRE(fixture.application->core_manager().activate(
      {ReplayRequest{*status.active, original},
       ActivationMode::start_or_replace},
      [](Result<ActivationResult>) {}));
  REQUIRE(fixture.application->core_manager().busy());

  bool completed = false;
  std::optional<AppError> failure;
  fixture.application->use_core(profile->id, replacement.id,
                                [&](Result<ActivationResult> result) {
                                  completed = true;
                                  if (!result)
                                    failure = result.error();
                                });
  REQUIRE(completed);
  REQUIRE(failure);
  CHECK(failure->code == ErrorCode::busy);
  const auto saved = fixture.application->profiles().get(profile->id);
  REQUIRE(saved);
  CHECK(saved->core_id == original.id);

  bool stopped = false;
  REQUIRE(fixture.application->core_manager().stop(
      [&](Result<ActivationResult>) { stopped = true; }));
  const auto stop_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (!stopped && std::chrono::steady_clock::now() < stop_deadline) {
    uv_run(&fixture.loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(stopped);
}

TEST_CASE(
    "A confirmed Stop with a stale run identity never changes core intent",
    "[runtime][stop-confirmation]") {
  ApplicationFixture fixture;
  ApiRouter router(*fixture.application);
  std::optional<RpcResponse> response;
  router.dispatch(
      "stop-confirmation-client",
      RpcRequest{1, 1, "connection.disconnect",
                 Json{{"expected_run_id", "run-from-confirmation"}}},
      [&](RpcResponse value) { response = std::move(value); });
  REQUIRE(response);
  REQUIRE_FALSE(response->ok);
  REQUIRE(response->error);
  CHECK(response->error->code == ErrorCode::conflict);
  CHECK(response->error->subject == "run-from-confirmation");
  const auto status = fixture.application->core_manager().status();
  CHECK_FALSE(status.active);
  CHECK_FALSE(status.desired_running);

  response.reset();
  router.dispatch("stop-confirmation-client",
                  RpcRequest{1, 2, "connection.disconnect",
                             Json{{"expected_run_id", nullptr}}},
                  [&](RpcResponse value) { response = std::move(value); });
  REQUIRE(response);
  CHECK(response->ok);
}

TEST_CASE("Subscription URL syntax is checked before storing a source",
          "[subscription][url-validation]") {
  ApplicationFixture fixture;
  ApiRouter router(*fixture.application);
  std::optional<RpcResponse> response;
  router.dispatch("source-client",
                  RpcRequest{1, 1, "subscription.add",
                             Json{{"name", "Broken"}, {"url", "not-a-url"}}},
                  [&](RpcResponse value) { response = std::move(value); });
  REQUIRE(response);
  REQUIRE_FALSE(response->ok);
  REQUIRE(response->error);
  CHECK(response->error->code == ErrorCode::invalid_argument);
  CHECK(response->error->subject == "url");
  const auto sources = fixture.store->list_sources();
  REQUIRE(sources);
  CHECK(sources->empty());

  response.reset();
  router.dispatch(
      "source-client",
      RpcRequest{1, 2, "subscription.add",
                 Json{{"name", "Valid"},
                      {"url", "http://127.0.0.1:9/fixture?token=private"}}},
      [&](RpcResponse value) { response = std::move(value); });
  REQUIRE(response);
  REQUIRE(response->ok);
  CHECK(response->result->contains("id"));
}

TEST_CASE("Profile source binding scopes connection inputs without copying the "
          "global inventory",
          "[PROFILE][source-binding]") {
  ApplicationFixture fixture;
  auto home = fixture.application->profiles().create({"Home", "", "local", {}});
  auto office =
      fixture.application->profiles().create({"Office", "", "local", {}});
  REQUIRE(home);
  REQUIRE(office);
  REQUIRE(fixture.store->put_node(source_node("node-a", "source-a")));
  REQUIRE(fixture.store->put_node(source_node("node-b", "source-b")));
  auto local = source_node("node-local", "source-a");
  local.source_id.reset();
  REQUIRE(fixture.store->put_node(local));

  save_sources(fixture, *home, std::vector<Id>{"source-a"});
  save_sources(fixture, *office, std::vector<Id>{"source-b"});
  auto home_input = fixture.application->build_input(home->id);
  auto office_input = fixture.application->build_input(office->id);
  REQUIRE(home_input);
  REQUIRE(office_input);
  auto ids = [](const BuildInput &input) {
    std::set<Id> result;
    for (const auto &node : input.nodes)
      result.insert(node.id);
    return result;
  };
  CHECK(ids(*home_input) == std::set<Id>{"node-a", "node-local"});
  CHECK(ids(*office_input) == std::set<Id>{"node-b", "node-local"});
  CHECK_FALSE(fixture.application->select(home->id,
                                          Target{TargetKind::node, "node-b"}));
  REQUIRE(fixture.application->select(home->id,
                                      Target{TargetKind::node, "node-a"}));
  CHECK_FALSE(fixture.application->select(office->id,
                                          Target{TargetKind::node, "node-a"}));
  REQUIRE(fixture.application->select(office->id,
                                      Target{TargetKind::node, "node-b"}));
  CHECK(fixture.store->get_node("node-a"));
  CHECK(fixture.store->get_node("node-b"));

  save_sources(fixture, *home, std::vector<Id>{});
  CHECK_FALSE(fixture.application->select(home->id,
                                          Target{TargetKind::node, "node-a"}));
  REQUIRE(fixture.application->select(home->id,
                                      Target{TargetKind::node, "node-local"}));
  auto office_after = fixture.application->build_input(office->id);
  REQUIRE(office_after);
  CHECK(ids(*office_after) == std::set<Id>{"node-b", "node-local"});

  // The legacy no-binding state must retain the pre-U0 all-sources behavior.
  save_sources(fixture, *home, std::nullopt);
  auto legacy_input = fixture.application->build_input(home->id);
  REQUIRE(legacy_input);
  CHECK(ids(*legacy_input) == std::set<Id>{"node-a", "node-b", "node-local"});
  REQUIRE(fixture.application->select(home->id,
                                      Target{TargetKind::node, "node-b"}));
}

TEST_CASE("FORM-03 traffic-statistics plan is explicit, native, and preserves "
          "policy data",
          "[FORM-03]") {
  Fixture fixture;
  const auto before = fixture.profiles->snapshot(fixture.profile.id);
  REQUIRE(before);
  const auto original_policy =
      read(fixture.root.path / "profiles" / fixture.profile.id / "modules" /
           "09_policy.json");
  auto plan = plan_traffic_stats(*before, true, "127.0.0.1:9556");
  REQUIRE(plan);
  REQUIRE(plan->changes.size() == 3);
  CHECK(plan->changes[0].edit.key == "stats");
  CHECK(plan->changes[1].edit.key == "policy");
  CHECK(plan->changes[2].edit.key == "metrics");
  CHECK(plan->changes[0].include_after_save);
  CHECK(plan->changes[1].include_after_save);
  CHECK(plan->changes[2].include_after_save);
  CHECK(plan->changes[1].edit.text.find("otherSystemField") !=
        std::string::npos);
  CHECK(plan->changes[1].edit.text.find("unknownLevel") != std::string::npos);
  CHECK(plan->changes[1].edit.text.find("extraPolicyField") !=
        std::string::npos);
  CHECK(plan->changes[2].edit.text.find("127.0.0.1:9556") != std::string::npos);
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" /
             "09_policy.json") == original_policy);

  const auto saved =
      save_module_changes(*fixture.profiles, fixture.profile.id, *plan);
  REQUIRE_FALSE(saved.failure);
  CHECK(saved.saved == std::vector<std::string>{"stats", "policy", "metrics"});
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" /
             "09_policy.json")
            .find("otherSystemField") != std::string::npos);
  CHECK_FALSE(
      (fixture.profiles->get(fixture.profile.id))->selected.has_value());

  const auto current = fixture.profiles->snapshot(fixture.profile.id);
  REQUIRE(current);
  auto disabled = plan_traffic_stats(*current, false, "127.0.0.1:9556");
  REQUIRE(disabled);
  REQUIRE(disabled->changes.size() == 3);
  for (const auto &change : disabled->changes)
    CHECK_FALSE(change.include_after_save);
}

TEST_CASE("FORM-03 failed later native write reports exactly the saved prefix",
          "[FORM-03]") {
  Fixture fixture;
  auto snapshot = fixture.profiles->snapshot(fixture.profile.id);
  REQUIRE(snapshot);
  auto plan = plan_traffic_stats(*snapshot, true, "127.0.0.1:9556");
  REQUIRE(plan);

  const auto profile_dir = fixture.root.path / "profiles" / fixture.profile.id;
  const auto metrics_path = profile_dir / "modules" / "07_metrics.json";
  std::filesystem::remove(metrics_path);
  std::filesystem::create_directory(metrics_path);

  const auto saved =
      save_module_changes(*fixture.profiles, fixture.profile.id, *plan);
  REQUIRE(saved.failure);
  CHECK(saved.saved == std::vector<std::string>{"stats", "policy"});
  CHECK(saved.failure->subject == std::optional<std::string>{"metrics"});
  CHECK(read(profile_dir / "modules" / "08_stats.json").find("stats") !=
        std::string::npos);
  CHECK(read(profile_dir / "modules" / "09_policy.json")
            .find("statsOutboundUplink") != std::string::npos);
  CHECK(std::filesystem::is_directory(metrics_path));
  CHECK_FALSE(
      (fixture.profiles->get(fixture.profile.id))->selected.has_value());
}

TEST_CASE("FORM-03 Application RPC enables native traffic statistics and "
          "preserves unrelated settings",
          "[FORM-03][module.change_plan][module.save_changes]") {
  ApplicationFixture fixture;
  auto profile = fixture.application->profiles().create(
      {"form-rpc", "core-system", "local", {}});
  REQUIRE(profile);
  REQUIRE(fixture.application->profiles().save_module(
      profile->id,
      {"policy",
       {module_base(fixture.application->profiles(), profile->id, "policy")},
       R"({"policy":{"levels":{"0":{"connIdle":61,"customLevel":{"keep":"level"}}},"system":{"statsOutboundUplink":false,"otherSystemField":{"keep":7}},"extraPolicyField":9}})"}));
  REQUIRE(fixture.application->profiles().save_module(
      profile->id,
      {"stats",
       {module_base(fixture.application->profiles(), profile->id, "stats")},
       R"({"stats":{}})"}));
  REQUIRE(fixture.application->profiles().save_module(
      profile->id,
      {"metrics",
       {module_base(fixture.application->profiles(), profile->id, "metrics")},
       R"({"metrics":{"listen":"127.0.0.1:9555","customMetricField":{"keep":true}}})"}));

  ApiRouter router(*fixture.application);
  std::optional<RpcResponse> plan_response;
  router.dispatch(
      "form-test-client",
      RpcRequest{
          1, 1, "module.change_plan",
          Json{{"profile_id", profile->id},
               {"action", "enable_traffic_statistics"},
               {"arguments", Json{{"metrics_listen", "127.0.0.1:9556"}}}}},
      [&](RpcResponse response) { plan_response = std::move(response); });
  REQUIRE(plan_response);
  REQUIRE_FALSE(plan_response->error.has_value());
  REQUIRE(plan_response->result);
  const auto &plan = *plan_response->result;
  REQUIRE(plan.at("changes").size() == 3);
  CHECK(plan.at("changes").at(0).at("key") == "stats");
  CHECK(plan.at("changes").at(1).at("key") == "policy");
  CHECK(plan.at("changes").at(2).at("key") == "metrics");
  CHECK(plan.at("changes").at(1).at("text").get<std::string>().find(
            "customLevel") != std::string::npos);
  CHECK(plan.at("changes").at(1).at("text").get<std::string>().find(
            "otherSystemField") != std::string::npos);
  CHECK(plan.at("changes").at(2).at("text").get<std::string>().find(
            "customMetricField") != std::string::npos);

  std::optional<RpcResponse> save_response;
  router.dispatch(
      "form-test-client",
      RpcRequest{1, 2, "module.save_changes",
                 Json{{"profile_id", profile->id}, {"plan", plan}}},
      [&](RpcResponse response) { save_response = std::move(response); });
  REQUIRE(save_response);
  REQUIRE_FALSE(save_response->error.has_value());
  REQUIRE(save_response->result);
  const auto &saved_result = *save_response->result;
  CHECK(saved_result.at("changed_modules") ==
        Json::array({"stats", "policy", "metrics"}));
  CHECK(saved_result.at("failure").is_null());

  const auto module_dir =
      fixture.root.path / "profiles" / profile->id / "modules";
  const auto stats = Json::parse(read(module_dir / "08_stats.json"));
  const auto policy =
      Json::parse(read(module_dir / "09_policy.json")).at("policy");
  const auto metrics =
      Json::parse(read(module_dir / "07_metrics.json")).at("metrics");
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
  CHECK_FALSE(
      fixture.application->profiles().get(profile->id)->selected.has_value());
  CHECK(fixture.application->core_manager().status().state ==
        CoreState::stopped);
}

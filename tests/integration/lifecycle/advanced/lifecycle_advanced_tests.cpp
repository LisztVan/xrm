#include <catch2/catch_test_macros.hpp>

#include "infra/process.hpp"
#include "infra/store.hpp"
#include "xray/core_manager.hpp"

#include <sqlite3.h>
#include <uv.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace {
using namespace xrm;

struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-lifecycle-advanced-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

struct Loop {
  uv_loop_t loop{};
  Loop() { REQUIRE(uv_loop_init(&loop) == 0); }
  ~Loop() { uv_loop_close(&loop); }
};

struct Services {
  TempDirectory temp;
  Loop event_loop;
  std::unique_ptr<Store> store;
  ProcessService process{event_loop.loop};
  RuntimeWriter writer{temp.path};
  std::unique_ptr<CoreManager> manager;
  explicit Services(std::function<void(LifecycleOptions::Phase)> phase_observer = {},
                    Milliseconds process_observation = std::chrono::milliseconds(10)) {
    auto opened = Store::open(temp.path / "state.sqlite");
    REQUIRE(opened);
    store = std::move(*opened);
    manager = std::make_unique<CoreManager>(event_loop.loop, *store, process, writer,
        LifecycleOptions{std::chrono::seconds(1), std::chrono::milliseconds(100),
                         process_observation, {std::chrono::milliseconds(10),
                         std::chrono::milliseconds(10), std::chrono::milliseconds(10)},
                         std::move(phase_observer)});
  }
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

BuildRequest request(const TempDirectory& temp, std::string profile,
                     std::map<std::string, std::string> environment) {
  BuildInput input;
  input.settings.profile.id = profile;
  input.settings.profile.name = profile;
  input.settings.profile.core_id = "fake";
  input.settings.profile.environment.cwd = temp.path;
  input.settings.profile.environment.process_environment = std::move(environment);
  input.settings.profile.modules.push_back({"log", "00_log.json", true});
  input.settings.modules.push_back({"log", "00_log.json", "{\"log\":{\"loglevel\":\"warning\"}}"});
  CoreInstall core{"fake", "fake", XRM_TEST_FAKE_CORE, "Xray fixture", false};
  return BuildRequest{std::move(input), std::move(core)};
}

ActivationRequest activation(BuildRequest build) {
  return ActivationRequest{ActivationInput{std::move(build)}, ActivationMode::start_or_replace};
}

void add_runtime_save_failure(const std::filesystem::path& database,
                              const std::string& predicate) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(database.c_str(), &db) == SQLITE_OK);
  const std::string sql =
      "CREATE TRIGGER reject_runtime BEFORE INSERT ON runtime_state "
      "WHEN " + predicate + " BEGIN SELECT RAISE(ABORT, 'injected runtime save failure'); END;"
      "CREATE TRIGGER reject_runtime_update BEFORE UPDATE ON runtime_state "
      "WHEN " + predicate + " BEGIN SELECT RAISE(ABORT, 'injected runtime save failure'); END;";
  char* error = nullptr;
  const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  const std::string message = error ? error : "";
  sqlite3_free(error);
  sqlite3_close(db);
  INFO(message);
  REQUIRE(result == SQLITE_OK);
}

}  // namespace

TEST_CASE("LIFE-07 failed RunRecord persistence stops candidate and restores previous input",
          "[LIFE-07][lifecycle-advanced]") {
  Services services;
  bool complete = false;
  REQUIRE(services.manager->activate(activation(request(services.temp, "A",
      {{"XRM_FAKE_CORE_RUN_HANG", "1"}})), [&](auto result) {
    REQUIRE(result);
    REQUIRE(result->effect == ActivationEffect::running_new);
    add_runtime_save_failure(services.temp.path / "state.sqlite",
        "instr(NEW.document_json, '\"profile_id\":\"B\"') > 0");
    REQUIRE(services.manager->activate(activation(request(services.temp, "B",
        {{"XRM_FAKE_CORE_RUN_HANG", "1"}})), [&](auto replaced) {
      REQUIRE(replaced);
      CHECK(replaced->effect == ActivationEffect::restored_old);
      CHECK(replaced->attempt_error);
      REQUIRE(replaced->active);
      CHECK(replaced->active->profile_id == "A");
      CHECK(services.manager->status().active->profile_id == "A");
      auto stop = services.manager->stop([&](auto stopped) {
        REQUIRE(stopped);
        complete = true;
      });
      REQUIRE(stop);
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(complete);
}

TEST_CASE("LIFE-07 a failed persisted stop intent is reported while the owned core still stops",
          "[LIFE-07][lifecycle-advanced]") {
  Services services;
  bool complete = false;
  REQUIRE(services.manager->activate(activation(request(services.temp, "A",
      {{"XRM_FAKE_CORE_RUN_HANG", "1"}})), [&](auto result) {
    REQUIRE(result);
    add_runtime_save_failure(services.temp.path / "state.sqlite", "1");
    REQUIRE(services.manager->stop([&](auto stopped) {
      REQUIRE(stopped);
      CHECK(stopped->effect == ActivationEffect::stopped);
      CHECK(stopped->attempt_error);
      CHECK_FALSE(services.manager->status().desired_running);
      CHECK(services.manager->status().state == CoreState::stopped);
      complete = true;
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(complete);
}

TEST_CASE("LIFE-09 concurrent activation is rejected as busy rather than queued",
          "[LIFE-09][lifecycle-advanced]") {
  Services services;
  bool complete = false;
  const auto first = services.manager->activate(activation(request(services.temp, "A",
      {{"XRM_FAKE_CORE_CHECK_HANG", "1"}, {"XRM_FAKE_CORE_RUN_HANG", "1"}})),
      [&](auto result) {
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::cancelled);
        complete = true;
      });
  REQUIRE(first);
  const auto second = services.manager->activate(activation(request(services.temp, "B", {})),
      [](auto) {});
  REQUIRE_FALSE(second);
  CHECK(second.error().code == ErrorCode::busy);
  auto cancel = services.manager->stop([&](auto stopped) {
    REQUIRE(stopped);
    complete = true;
  });
  REQUIRE(cancel);
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(complete);
}

TEST_CASE("LIFE-04 stop deterministically preempts every lifecycle phase",
          "[LIFE-04][lifecycle-advanced]") {
  using Phase = LifecycleOptions::Phase;
  const std::array phases{Phase::prepare, Phase::checking, Phase::stopping_old,
                          Phase::starting_new, Phase::restoring_old, Phase::retry_wait};
  for (const auto target : phases) {
    DYNAMIC_SECTION("phase " << static_cast<int>(target)) {
      std::unique_ptr<Services> services;
      bool stop_requested = false;
      bool stopped = false;
      services = std::make_unique<Services>([&](Phase phase) {
        if (phase != target || stop_requested) return;
        stop_requested = true;
        REQUIRE(services->manager->stop([&](auto result) {
          REQUIRE(result);
          CHECK(result->effect == ActivationEffect::stopped);
          stopped = true;
        }));
      }, target == Phase::restoring_old ? std::chrono::milliseconds(500)
                                        : std::chrono::milliseconds(10));
      const auto run_count = services->temp.path / "run-count";
      auto launch_a = [&] {
        return services->manager->activate(activation(request(services->temp, "A", {
            {"XRM_FAKE_CORE_RUN_HANG", "1"},
            {"XRM_FAKE_CORE_RUN_COUNT", run_count.string()}})), [&](auto result) {
          REQUIRE(result);
          if (target == Phase::stopping_old || target == Phase::restoring_old) {
            std::map<std::string, std::string> environment{
                {"XRM_FAKE_CORE_RUN_COUNT", run_count.string()}};
            if (target == Phase::restoring_old)
              environment.emplace("XRM_FAKE_CORE_RUN_EXIT", "23");
            else
              environment.emplace("XRM_FAKE_CORE_RUN_HANG", "1");
            REQUIRE(services->manager->activate(activation(request(services->temp, "B",
                std::move(environment))), [&](auto replacement) {
              CHECK_FALSE(replacement);
              if (replacement) {
                CHECK(replacement->effect != ActivationEffect::restored_old);
                REQUIRE(services->manager->stop([&](auto cleanup) { REQUIRE(cleanup); }));
              } else {
                CHECK(replacement.error().code == ErrorCode::cancelled);
              }
            }));
          } else if (target == Phase::retry_wait) {
            const auto pid = services->manager->status().pid;
            REQUIRE(pid);
            REQUIRE(::kill(*pid, SIGKILL) == 0);
          }
        });
      };

      if (target == Phase::prepare || target == Phase::checking || target == Phase::starting_new) {
        std::map<std::string, std::string> environment{
            {"XRM_FAKE_CORE_RUN_HANG", "1"},
            {"XRM_FAKE_CORE_RUN_COUNT", run_count.string()}};
        if (target == Phase::checking)
          environment.emplace("XRM_FAKE_CORE_CHECK_HANG", "1");
        REQUIRE(services->manager->activate(activation(request(services->temp, "A",
            std::move(environment))), [](auto result) {
          REQUIRE_FALSE(result);
          CHECK(result.error().code == ErrorCode::cancelled);
        }));
      } else {
        REQUIRE(launch_a());
      }
      uv_run(&services->event_loop.loop, UV_RUN_DEFAULT);
      CHECK(stop_requested);
      CHECK(stopped);
      CHECK_FALSE(services->manager->status().desired_running);
      CHECK(services->manager->status().state == CoreState::stopped);
    }
  }
}

TEST_CASE("LIFE-05 replay uses the previously successful runtime directory unchanged",
          "[LIFE-05][LIFE-08][LIFE-10][lifecycle-advanced]") {
  Services services;
  std::unique_ptr<CoreManager> replay_manager;
  const auto run_argv = services.temp.path / "run.argv";
  bool complete = false;
  REQUIRE(services.manager->activate(activation(request(services.temp, "saved-A",
      {{"XRM_FAKE_CORE_RUN_HANG", "1"}, {"XRM_FAKE_CORE_RUN_ARGV", run_argv.string()}})),
      [&](auto started) {
        REQUIRE(started);
        const auto saved = services.manager->status().active;
        REQUIRE(saved);
        const auto old_confdir = saved->spec.confdir;
        const auto old_file = old_confdir / "00_log.json";
        const auto bytes = read_file(old_file);
        REQUIRE_FALSE(bytes.empty());
        REQUIRE(services.manager->stop([&, saved_record = *saved, old_confdir,
                                        old_file, bytes](auto stopped) {
          REQUIRE(stopped);
          RuntimePersistent replay_state;
          replay_state.desired_running = true;
          replay_state.active = saved_record;
          REQUIRE(services.store->save_runtime(replay_state));
          replay_manager = std::make_unique<CoreManager>(services.event_loop.loop,
              *services.store, services.process, services.writer,
              LifecycleOptions{std::chrono::seconds(1), std::chrono::milliseconds(100),
                               std::chrono::milliseconds(10)});
          const auto replacement_binary = services.temp.path / "replacement-xray";
          std::filesystem::copy_file(XRM_TEST_FAKE_CORE, replacement_binary,
                                     std::filesystem::copy_options::overwrite_existing);
          std::filesystem::permissions(replacement_binary,
              std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
              std::filesystem::perms::owner_write | std::filesystem::perms::group_exec |
              std::filesystem::perms::group_read | std::filesystem::perms::others_exec |
              std::filesystem::perms::others_read);
          CoreInstall replacement{"replacement", "replacement", replacement_binary,
                                  "Xray fixture", false};
          auto replay = ActivationRequest{
              ActivationInput{ReplayRequest{saved_record, replacement}},
              ActivationMode::preserve_running_state};
          REQUIRE(replay_manager->activate(std::move(replay),
              [&, old_confdir, old_file, bytes, replacement_binary](auto result) {
            REQUIRE(result);
            REQUIRE(result->active);
            CHECK(result->active->spec.confdir == old_confdir);
            CHECK(result->active->spec.binary == replacement_binary);
            CHECK(read_file(old_file) == bytes);
            CHECK(read_file(run_argv).find(old_confdir.string()) != std::string::npos);
            CHECK(result->active->core_id == "replacement");
            REQUIRE(replay_manager->stop([&](auto stop_result) {
              REQUIRE(stop_result);
              complete = true;
            }));
          }));
        }));
      }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(complete);
}

TEST_CASE("LIFE-11 failed candidate and unusable old runtime report failure without replacing old files",
          "[LIFE-11][lifecycle-advanced]") {
  Services services;
  const auto old_binary = services.temp.path / "old-xray";
  std::filesystem::copy_file(XRM_TEST_FAKE_CORE, old_binary,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::permissions(old_binary,
      std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
      std::filesystem::perms::owner_write | std::filesystem::perms::group_exec |
      std::filesystem::perms::group_read | std::filesystem::perms::others_exec |
      std::filesystem::perms::others_read);
  auto old = request(services.temp, "A", {{"XRM_FAKE_CORE_RUN_HANG", "1"}});
  old.core.path = old_binary;
  bool complete = false;
  REQUIRE(services.manager->activate(activation(std::move(old)), [&](auto started) {
    REQUIRE(started);
    const auto active = services.manager->status().active;
    REQUIRE(active);
    const auto source_file = active->spec.confdir / "00_log.json";
    const auto source_bytes = read_file(source_file);
    REQUIRE_FALSE(source_bytes.empty());
    REQUIRE(std::filesystem::remove(old_binary));
    {
      std::ofstream unusable(old_binary, std::ios::binary);
      unusable << "#!/bin/sh\nexit 27\n";
      REQUIRE(unusable.good());
    }
    std::filesystem::permissions(old_binary, std::filesystem::perms::owner_exec |
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    auto replacement = request(services.temp, "B", {{"XRM_FAKE_CORE_RUN_EXIT", "23"}});
    REQUIRE(services.manager->activate(activation(std::move(replacement)),
        [&, source_file, source_bytes](auto result) {
      REQUIRE_FALSE(result);
      CHECK(services.manager->status().state == CoreState::failed);
      CHECK_FALSE(services.manager->status().active);
      CHECK(read_file(source_file) == source_bytes);
      auto persisted = services.store->load_runtime();
      REQUIRE(persisted);
      CHECK_FALSE(persisted->desired_running);
      CHECK_FALSE(persisted->active);
      complete = true;
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(complete);
}

TEST_CASE("LIFE-10 server recovery replays the persisted confdir and normal stop clears run intent",
          "[LIFE-10][lifecycle-advanced]") {
  Services services;
  REQUIRE(services.store->put_core(CoreInstall{"fake", "fake", XRM_TEST_FAKE_CORE,
                                               "Xray fixture", false}));
  const auto run_count = services.temp.path / "run-count";
  std::unique_ptr<CoreManager> restarted_manager;
  bool complete = false;
  REQUIRE(services.manager->activate(activation(request(services.temp, "saved-A", {
      {"XRM_FAKE_CORE_RUN_HANG", "1"}, {"XRM_FAKE_CORE_RUN_COUNT", run_count.string()}})),
      [&](auto result) {
        REQUIRE(result);
        const auto active = services.manager->status().active;
        REQUIRE(active);
        const auto confdir = active->spec.confdir;
        const auto config = confdir / "00_log.json";
        const auto bytes = read_file(config);
        REQUIRE_FALSE(bytes.empty());
        REQUIRE(services.manager->stop([&, saved_record = *active,
                                        confdir, config, bytes](auto stopped) {
          REQUIRE(stopped);
          RuntimePersistent restart_state;
          restart_state.desired_running = true;
          restart_state.active = saved_record;
          REQUIRE(services.store->save_runtime(restart_state));
          restarted_manager = std::make_unique<CoreManager>(services.event_loop.loop,
              *services.store, services.process, services.writer,
              LifecycleOptions{std::chrono::seconds(1), std::chrono::milliseconds(100),
                               std::chrono::milliseconds(10)});
          REQUIRE(restarted_manager->restore_after_server_start([&, confdir, config, bytes](auto restored) {
            REQUIRE(restored);
            REQUIRE(restored->active);
            CHECK(restored->active->spec.confdir == confdir);
            CHECK(read_file(config) == bytes);
            const auto launches = read_file(run_count);
            CHECK(std::count(launches.begin(), launches.end(), '\n') == 2);
            REQUIRE(restarted_manager->stop([&](auto final_stop) {
              REQUIRE(final_stop);
              auto persisted = services.store->load_runtime();
              REQUIRE(persisted);
              CHECK_FALSE(persisted->desired_running);
              CHECK_FALSE(persisted->active);
              complete = true;
            }));
          }));
        }));
      }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(complete);
}

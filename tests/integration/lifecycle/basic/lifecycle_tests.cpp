#include <catch2/catch_test_macros.hpp>

#include "infra/process.hpp"
#include "infra/store.hpp"
#include "server/log_buffer.hpp"
#include "xray/core_manager.hpp"

#include <uv.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

namespace {
using namespace xrm;

struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
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
  explicit Services(Milliseconds process_observation = std::chrono::milliseconds(20)) {
    auto opened = Store::open(temp.path / "state.sqlite");
    REQUIRE(opened);
    store = std::move(*opened);
    manager = std::make_unique<CoreManager>(event_loop.loop, *store, process, writer,
        LifecycleOptions{std::chrono::seconds(1), std::chrono::milliseconds(100), process_observation});
  }
};
BuildRequest request(const TempDirectory& temp, std::string profile,
                     std::map<std::string, std::string> environment) {
  BuildInput input;
  input.settings.profile.id = profile;
  input.settings.profile.name = profile;
  input.settings.profile.core_id = "fake";
  input.settings.profile.environment.cwd = temp.path;
  input.settings.profile.environment.process_environment = std::move(environment);
  CoreInstall core{"fake", "fake", XRM_TEST_FAKE_CORE, "Xray fixture", false};
  return BuildRequest{std::move(input), std::move(core)};
}
ActivationRequest activation(BuildRequest build) {
  return ActivationRequest{ActivationInput{std::move(build)}, ActivationMode::start_or_replace};
}
}  // namespace

TEST_CASE("core output EOF finishes incomplete log tails after check and run", "[LIFE][log-eof]") {
  Services services;
  LogBuffer logs;
  services.manager->on_output([&](LogStream stream, std::string bytes) {
    logs.append(stream, bytes);
  });
  services.manager->on_output_closed([&](LogStream stream) { logs.finish(stream); });
  auto build = request(services.temp, "log-eof", {{"XRM_FAKE_CORE_RUN_HANG", "1"},
      {"XRM_FAKE_CORE_STDOUT_HEX", "7461696c3ae4b8"}});
  bool stopped = false;
  REQUIRE(services.manager->activate(activation(std::move(build)), [&](auto result) {
    REQUIRE(result);
    REQUIRE(services.manager->stop([&](auto stop_result) {
      REQUIRE(stop_result);
      stopped = true;
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  REQUIRE(stopped);
  std::string output;
  for (const auto &entry : logs.read_after(0, 65536).entries)
    if (entry.stream == LogStream::core_stdout) output += entry.text;
  CHECK(output == "tail:\\xE4\\xB8tail:\\xE4\\xB8");
}

TEST_CASE("LIFE-01 checks and starts the same candidate and persists active RunRecord", "[LIFE-01]") {
  Services services;
  const auto check_argv = services.temp.path / "check.argv";
  const auto run_argv = services.temp.path / "run.argv";
  const auto check_cwd = services.temp.path / "check.cwd";
  const auto run_cwd = services.temp.path / "run.cwd";
  auto build = request(services.temp, "profile-a", {{"XRM_FAKE_CORE_RUN_HANG", "1"},
                                                       {"XRM_FAKE_CORE_STDOUT", "core-out"},
                                                       {"XRM_FAKE_CORE_STDERR", "core-err"},
                                                       {"XRM_FAKE_CORE_CHECK_ARGV", check_argv.string()},
                                                       {"XRM_FAKE_CORE_RUN_ARGV", run_argv.string()},
                                                       {"XRM_FAKE_CORE_CHECK_CWD", check_cwd.string()},
                                                       {"XRM_FAKE_CORE_RUN_CWD", run_cwd.string()}});
  LogBuffer output_logs;
  services.manager->on_output([&](LogStream stream, std::string bytes) {
    output_logs.append(stream, bytes);
  });
  bool start_ok = false;
  bool stop_ok = false;
  REQUIRE(services.manager->activate(activation(std::move(build)), [&](auto result) {
    REQUIRE(result);
    REQUIRE(result->effect == ActivationEffect::running_new);
    start_ok = true;
    const auto status = services.manager->status();
    REQUIRE(status.state == CoreState::running);
    REQUIRE(status.health == RuntimeHealth::process_only);
    REQUIRE(status.active);
    auto saved_active = services.store->load_runtime();
    REQUIRE(saved_active);
    REQUIRE(saved_active->desired_running);
    REQUIRE(saved_active->active);
    CHECK(saved_active->active->spec.confdir == status.active->spec.confdir);
    auto stopped = services.manager->stop([&](auto stop_result) {
      REQUIRE(stop_result);
      REQUIRE(stop_result->effect == ActivationEffect::stopped);
      stop_ok = true;
    });
    REQUIRE(stopped);
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  REQUIRE(start_ok);
  REQUIRE(stop_ok);
  auto saved = services.store->load_runtime();
  REQUIRE(saved);
  CHECK_FALSE(saved->desired_running);
  CHECK_FALSE(saved->active);
  const auto candidate = read_file(check_argv);
  CHECK(candidate.find("1\trun\n2\t-test\n3\t-confdir\n") != std::string::npos);
  CHECK(read_file(run_argv).find("1\trun\n2\t-confdir\n") != std::string::npos);
  CHECK(read_file(check_cwd) == services.temp.path.string());
  CHECK(read_file(run_cwd) == services.temp.path.string());
  const auto logs = output_logs.read_after(0, 1024);
  CHECK(std::any_of(logs.entries.begin(), logs.entries.end(), [](const auto& entry) {
    return entry.stream == LogStream::core_stdout && entry.text.find("core-out") != std::string::npos;
  }));
  CHECK(std::any_of(logs.entries.begin(), logs.entries.end(), [](const auto& entry) {
    return entry.stream == LogStream::core_stderr && entry.text.find("core-err") != std::string::npos;
  }));
}

TEST_CASE("LIFE-02 a failed candidate check leaves the previous core untouched", "[LIFE-02]") {
  Services services;
  auto start_a = request(services.temp, "A", {{"XRM_FAKE_CORE_RUN_HANG", "1"}});
  bool completed = false;
  std::optional<int> original_pid;
  REQUIRE(services.manager->activate(activation(std::move(start_a)), [&](auto result_a) {
    REQUIRE(result_a);
    original_pid = services.manager->status().pid;
    REQUIRE(original_pid);
    auto start_b = request(services.temp, "B", {{"XRM_FAKE_CORE_RUN_HANG", "1"},
                                                 {"XRM_FAKE_CORE_CHECK_EXIT", "9"}});
    REQUIRE(services.manager->activate(activation(std::move(start_b)), [&](auto result_b) {
      REQUIRE_FALSE(result_b);
      const auto status = services.manager->status();
      CHECK(status.state == CoreState::running);
      CHECK(status.active->profile_id == "A");
      CHECK(status.pid == original_pid);
      auto stop = services.manager->stop([&](auto result) { REQUIRE(result); completed = true; });
      REQUIRE(stop);
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(completed);
}

TEST_CASE("LIFE-03 a failed replacement restores the previous input once", "[LIFE-03]") {
  // The short-lived failing fixture must remain observable when sanitizer
  // instrumentation slows process startup.
  Services services(std::chrono::milliseconds(250));
  const auto run_count = services.temp.path / "run-count";
  auto start_a = request(services.temp, "A", {{"XRM_FAKE_CORE_RUN_HANG", "1"},
                                               {"XRM_FAKE_CORE_RUN_COUNT", run_count.string()}});
  bool completed = false;
  REQUIRE(services.manager->activate(activation(std::move(start_a)), [&](auto result_a) {
    REQUIRE(result_a);
    auto start_b = request(services.temp, "B", {{"XRM_FAKE_CORE_RUN_EXIT", "23"},
                                                 {"XRM_FAKE_CORE_RUN_COUNT", run_count.string()}});
    REQUIRE(services.manager->activate(activation(std::move(start_b)), [&](auto result_b) {
      REQUIRE(result_b);
      CHECK(result_b->effect == ActivationEffect::restored_old);
      CHECK(result_b->active);
      CHECK(result_b->active->profile_id == "A");
      CHECK(result_b->attempt_error);
      auto stop = services.manager->stop([&](auto result) { REQUIRE(result); completed = true; });
      REQUIRE(stop);
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(completed);
  const auto count = read_file(run_count);
  CHECK(std::count(count.begin(), count.end(), '\n') == 3);
}

TEST_CASE("P2 inventory removal retains the active confdir until a successful switch releases it") {
  Services services;
  Node source_node;
  source_node.id = "inventory-A";
  source_node.source_id = "source-A";
  source_node.original_name = "A";
  source_node.settings = RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
  REQUIRE(services.store->put_node(source_node));
  bool completed = false;
  auto start_a = request(services.temp, "A", {{"XRM_FAKE_CORE_RUN_HANG", "1"}});
  start_a.input.nodes.push_back(source_node);
  start_a.input.settings.profile.additional_node_ids.push_back(source_node.id);
  start_a.input.settings.profile.modules.push_back({"outbounds", "03_outbounds.json", true});
  start_a.input.settings.modules.push_back({"outbounds", "03_outbounds.json", R"({"outbounds":[]})"});
  start_a.input.settings.profile.modules.push_back({"log", "00_log.json", true});
  start_a.input.settings.modules.push_back({"log", "00_log.json", R"({"log":{"loglevel":"warning"}})"});
  REQUIRE(services.manager->activate(activation(std::move(start_a)), [&](auto started) {
    REQUIRE(started);
    auto running = services.manager->status();
    REQUIRE(running.active);
    REQUIRE(running.pid);
    const auto old_record = *running.active;
    REQUIRE(old_record.index.nodes.size() == 1);
    CHECK(old_record.index.nodes.front().id == source_node.id);
    CHECK(old_record.index.nodes.front().display_name == "A");
    const auto old_file = old_record.spec.confdir / "00_log.json";
    const auto bytes = read_file(old_file);
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(services.store->erase_node(source_node.id));
    CHECK_FALSE(services.store->get_node(source_node.id));
    auto unchanged = services.manager->status();
    CHECK(unchanged.pid == running.pid);
    CHECK(unchanged.active->spec.confdir == old_record.spec.confdir);
    CHECK(unchanged.active->index.nodes.front().display_name == "A");
    CHECK(read_file(old_file) == bytes);

    auto start_b = request(services.temp, "B", {{"XRM_FAKE_CORE_RUN_HANG", "1"}});
    REQUIRE(services.manager->activate(activation(std::move(start_b)), [&, old_record](auto replaced) {
      REQUIRE(replaced);
      REQUIRE(replaced->effect == ActivationEffect::running_new);
      CHECK(replaced->active->profile_id == "B");
      CHECK_FALSE(std::filesystem::exists(old_record.spec.confdir));
      const auto stored = services.store->load_runtime();
      REQUIRE(stored);
      CHECK_FALSE(stored->previous);
      REQUIRE(services.manager->stop([&](auto stopped) {
        REQUIRE(stopped);
        completed = true;
      }));
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(completed);
}

TEST_CASE("P2 failed replacement preserves rollback confdir and discards the failed candidate") {
  Services services(std::chrono::milliseconds(250));
  bool completed = false;
  auto start_a = request(services.temp, "A", {{"XRM_FAKE_CORE_RUN_HANG", "1"}});
  start_a.input.settings.profile.modules.push_back({"log", "00_log.json", true});
  start_a.input.settings.modules.push_back({"log", "00_log.json", R"({"log":{"loglevel":"warning"}})"});
  REQUIRE(services.manager->activate(activation(std::move(start_a)), [&](auto started) {
    REQUIRE(started);
    const auto old = services.manager->status().active;
    REQUIRE(old);
    const auto old_file = old->spec.confdir / "00_log.json";
    const auto bytes = read_file(old_file);
    auto start_b = request(services.temp, "B", {{"XRM_FAKE_CORE_RUN_EXIT", "23"}});
    REQUIRE(services.manager->activate(activation(std::move(start_b)), [&, old, bytes, old_file](auto replaced) {
      REQUIRE(replaced);
      CHECK(replaced->effect == ActivationEffect::restored_old);
      CHECK(replaced->active->spec.confdir == old->spec.confdir);
      CHECK(read_file(old_file) == bytes);
      std::size_t dirs = 0;
      for (const auto& entry : std::filesystem::directory_iterator(
               services.temp.path / "runtime" / "candidate"))
        if (entry.is_directory()) ++dirs;
      CHECK(dirs == 1);
      REQUIRE(services.manager->stop([&](auto stopped) { REQUIRE(stopped); completed = true; }));
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(completed);
}

TEST_CASE("P2 failed same-directory core replay cannot discard its rollback input") {
  Services services(std::chrono::milliseconds(250));
  bool completed = false;
  auto start_a = request(services.temp, "A", {{"XRM_FAKE_CORE_RUN_HANG", "1"}});
  start_a.input.settings.profile.modules.push_back({"log", "00_log.json", true});
  start_a.input.settings.modules.push_back({"log", "00_log.json", R"({"log":{"loglevel":"warning"}})"});
  REQUIRE(services.manager->activate(activation(std::move(start_a)), [&](auto started) {
    REQUIRE(started);
    const auto active = services.manager->status().active;
    REQUIRE(active);
    const auto file = active->spec.confdir / "00_log.json";
    const auto bytes = read_file(file);
    REQUIRE_FALSE(bytes.empty());
    auto replay_record = *active;
    replay_record.spec.environment.erase("XRM_FAKE_CORE_RUN_HANG");
    replay_record.spec.environment["XRM_FAKE_CORE_RUN_EXIT"] = "23";
    const CoreInstall replay_core{"replay", "replay", XRM_TEST_FAKE_CORE, "fixture", false};
    REQUIRE(services.manager->activate(
        ActivationRequest{ActivationInput{ReplayRequest{replay_record, replay_core}},
                          ActivationMode::preserve_running_state},
        [&, active, file, bytes](auto result) {
          REQUIRE(result);
          CHECK(result->effect == ActivationEffect::restored_old);
          CHECK(result->active->run_id == active->run_id);
          CHECK(read_file(file) == bytes);
          REQUIRE(services.manager->stop([&](auto stopped) {
            REQUIRE(stopped);
            completed = true;
          }));
        }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(completed);
}

TEST_CASE("LIFE-06 process-only health never claims remote readiness", "[LIFE-06]") {
  Services services;
  auto build = request(services.temp, "offline", {{"XRM_FAKE_CORE_RUN_HANG", "1"}});
  bool completed = false;
  REQUIRE(services.manager->activate(activation(std::move(build)), [&](auto result) {
    REQUIRE(result);
    CHECK(services.manager->status().health == RuntimeHealth::process_only);
    CHECK(services.manager->status().state == CoreState::running);
    auto stop = services.manager->stop([&](auto stop_result) { REQUIRE(stop_result); completed = true; });
    REQUIRE(stop);
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(completed);
}

TEST_CASE("LOG-01 preserves stream and cursor while bounding retained chunks", "[LOG-01]") {
  LogBuffer logs;
  logs.append(LogStream::core_stdout, std::string(3 * 1024 * 1024, 'a'));
  logs.append(LogStream::core_stderr, "error\n");
  logs.append(LogStream::core_stdout, std::string(3 * 1024 * 1024, 'b'));
  CHECK(logs.bytes_retained() <= 4 * 1024 * 1024);
  CHECK(logs.read_after(0, 32 * 1024).truncated);
  auto page = logs.read_after(192, 32 * 1024);
  REQUIRE_FALSE(page.entries.empty());
  CHECK(page.entries.front().stream == LogStream::core_stderr);
  auto next_page = logs.read_after(page.next_cursor, 32 * 1024);
  REQUIRE_FALSE(next_page.entries.empty());
  CHECK(next_page.entries.front().stream == LogStream::core_stdout);
  CHECK(page.next_cursor < logs.cursor());
}

TEST_CASE("PROC-04 stopping the managed core leaves an unrelated process untouched", "[PROC-04]") {
  const pid_t unrelated = ::fork();
  REQUIRE(unrelated >= 0);
  if (unrelated == 0) {
    ::execl("/bin/sleep", "sleep", "30", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  Services services;
  auto build = request(services.temp, "managed", {{"XRM_FAKE_CORE_RUN_HANG", "1"}});
  bool stopped = false;
  REQUIRE(services.manager->activate(activation(std::move(build)), [&](auto result) {
    REQUIRE(result);
    REQUIRE(services.manager->stop([&](auto stop_result) {
      REQUIRE(stop_result);
      stopped = true;
    }));
  }));
  uv_run(&services.event_loop.loop, UV_RUN_DEFAULT);
  CHECK(stopped);
  CHECK(::kill(unrelated, 0) == 0);
  ::kill(unrelated, SIGTERM);
  int status = 0;
  REQUIRE(::waitpid(unrelated, &status, 0) == unrelated);
  CHECK(WIFSIGNALED(status));
}

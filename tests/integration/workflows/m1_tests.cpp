// XRM: Exercises CLI-to-daemon workflows, including saved Source and runtime
// boundaries.

#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/common.hpp"
#include "model/profile.hpp"
#include "xray/codecs/codec.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {
using namespace xrm;

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xrm-m1-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
struct Captured {
  int code = -1;
  std::string out;
  std::string err;
  bool timed_out = false;
};
std::string read_file(const std::filesystem::path &file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}
Captured run(const std::filesystem::path &executable,
             const std::filesystem::path &root,
             const std::vector<std::string> &arguments) {
  static unsigned sequence = 0;
  const auto out_path = root / ("out-" + std::to_string(++sequence));
  const auto err_path = root / ("err-" + std::to_string(sequence));
  std::vector<std::string> storage{executable.string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &value : storage)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                                  argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0)
    return {};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(out_path), read_file(err_path), false};
    if (result < 0)
      break;
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path), true};
}
class Server {
public:
  explicit Server(TempDirectory &temp)
      : temp_(&temp), socket_(temp.path / "xrmd.sock"),
        db_(temp.path / "state.sqlite") {}
  bool start() {
    std::vector<std::string> values{XRM_TEST_SERVER, "--foreground",
                                    "--socket",      socket_.string(),
                                    "--database",    db_.string()};
    std::vector<char *> argv;
    for (auto &value : values)
      argv.push_back(value.data());
    argv.push_back(nullptr);
    std::array<char *, 1> environment{nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                     (temp_->path / "xrmd.stderr").c_str(),
                                     O_CREAT | O_TRUNC | O_WRONLY, 0600);
    const int status = posix_spawn(&pid_, XRM_TEST_SERVER, &actions, nullptr,
                                   argv.data(), environment.data());
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0)
      return false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      const auto value = socket_.string();
      std::copy(value.begin(), value.end(), address.sun_path);
      if (::connect(fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) == 0) {
        ::close(fd);
        return true;
      }
      ::close(fd);
      ::poll(nullptr, 0, 10);
    }
    return false;
  }
  Captured cli(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(),
                     {"--socket", socket_.string(), "--json"});
    return run(XRM_TEST_MANAGER, temp_->path, arguments);
  }
  Captured cli_text(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), {"--socket", socket_.string()});
    return run(XRM_TEST_MANAGER, temp_->path, arguments);
  }
  bool stop() {
    if (pid_ < 0)
      return true;
    ::kill(pid_, SIGTERM);
    int status = 0;
    for (int attempt = 0; attempt < 500; ++attempt) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return WIFEXITED(status);
      }
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return false;
  }
  int observed_exit() {
    if (pid_ < 0)
      return -1;
    int status = 0;
    const auto waited = ::waitpid(pid_, &status, WNOHANG);
    if (waited != pid_)
      return -1;
    pid_ = -1;
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
  }
  ~Server() {
    if (pid_ >= 0)
      stop();
  }

private:
  TempDirectory *temp_;
  std::filesystem::path socket_;
  std::filesystem::path db_;
  pid_t pid_ = -1;
};
Json parsed(const Captured &result) {
  if (result.code != 0 || result.timed_out)
    return {};
  try {
    return Json::parse(result.out);
  } catch (...) {
    return {};
  }
}

TEST_CASE("historical node preview is read-only and confirmation removes only "
          "selected safe rows",
          "[HISTORY-01]") {
  TempDirectory temp;
  auto store = Store::open(temp.path / "state.sqlite");
  REQUIRE(store);
  Subscription source;
  source.id = "history-source";
  source.name = "fixture";
  source.url = "http://127.0.0.1/fixture";
  REQUIRE((*store)->put_source(source));
  Node current;
  current.id = "current";
  current.source_id = source.id;
  current.original_name = "same name";
  current.settings =
      RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
  REQUIRE((*store)->put_node(current));
  Node safe = current;
  safe.id = "safe-history";
  safe.source_missing = true;
  REQUIRE((*store)->put_node(safe));
  Node second = safe;
  second.id = "second-history";
  REQUIRE((*store)->put_node(second));
  Node renamed = safe;
  renamed.id = "different-name";
  renamed.original_name = "another label";
  REQUIRE((*store)->put_node(renamed));
  Node customized = safe;
  customized.id = "user-alias";
  customized.alias = "my alias";
  REQUIRE((*store)->put_node(customized));
  Node referenced = safe;
  referenced.id = "selected-history";
  referenced.source_missing = false;
  REQUIRE((*store)->put_node(referenced));
  store->reset();

  Server server(temp);
  REQUIRE(server.start());
  const auto core = parsed(server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "history core"}));
  REQUIRE(core.contains("id"));
  const auto profile =
      parsed(server.cli({"profile", "create", "--name", "history profile",
                         "--core", core.at("id").get<std::string>()}));
  REQUIRE(profile.contains("id"));
  REQUIRE(server
              .cli({"profile", "sources", "set",
                    profile.at("id").get<std::string>(), "--source", source.id})
              .code == 0);
  REQUIRE(server
              .cli({"select", "node", profile.at("id").get<std::string>(),
                    referenced.id})
              .code == 0);
  auto update_store = Store::open(temp.path / "state.sqlite");
  REQUIRE(update_store);
  referenced.source_missing = true;
  REQUIRE((*update_store)->put_node(referenced));
  update_store->reset();
  const auto running_profile =
      parsed(server.cli({"profile", "create", "--name", "running profile",
                         "--core", core.at("id").get<std::string>()}));
  REQUIRE(running_profile.contains("id"));
  REQUIRE(server
              .cli({"profile", "sources", "set",
                    running_profile.at("id").get<std::string>(), "--source",
                    source.id})
              .code == 0);
  REQUIRE(server
              .cli({"connect", running_profile.at("id").get<std::string>(),
                    "--node", current.id})
              .code == 0);
  const auto runtime_before = parsed(server.cli({"status"}));
  REQUIRE(runtime_before.value("state", "") == "running");
  const auto confdir =
      runtime_before.at("active").at("confdir").get<std::string>();
  std::map<std::string, std::string> runtime_files;
  for (const auto &file : std::filesystem::directory_iterator(confdir))
    runtime_files.emplace(file.path().filename().string(),
                          read_file(file.path()));
  const auto preview_result =
      server.cli({"node", "history", "preview", source.id});
  REQUIRE(preview_result.code == 0);
  const auto preview = parsed(preview_result);
  REQUIRE(preview.value("current_count", 0) == 1);
  REQUIRE(preview.value("history_count", 0) == 5);
  REQUIRE(preview.at("candidates").size() == 5);
  const auto token = preview.at("token").get<std::string>();
  REQUIRE_FALSE(token.empty());
  auto classify = [&](std::string_view id) -> Json {
    for (const auto &item : preview.at("candidates"))
      if (item.value("id", "") == id)
        return item;
    return Json::object();
  };
  CHECK(classify("safe-history").value("eligible", false));
  CHECK(classify("second-history").value("eligible", false));
  CHECK_FALSE(classify("different-name").value("eligible", true));
  CHECK_FALSE(classify("user-alias").value("eligible", true));
  CHECK_FALSE(classify("selected-history").value("eligible", true));
  CHECK_FALSE(classify("selected-history").at("references").empty());
  const auto human_preview =
      server.cli_text({"node", "history", "preview", source.id});
  REQUIRE(human_preview.code == 0);
  CHECK(human_preview.out.find("blocked: user_attributes_differ") !=
        std::string::npos);
  CHECK(human_preview.out.find("referenced by Profile history profile") !=
        std::string::npos);
  CHECK(parsed(server.cli({"node", "history", "preview", source.id}))
            .at("token") == token);
  CHECK(server
            .cli({"node", "history", "confirm", source.id, "--id",
                  "safe-history", "--token", token})
            .code != 0); // No --yes means cancel.
  CHECK(parsed(server.cli({"node", "history", "preview", source.id}))
            .value("history_count", 0) == 5);
  const auto confirmed =
      server.cli({"node", "history", "confirm", source.id, "--id",
                  "safe-history", "--token", token, "--yes"});
  REQUIRE(confirmed.code == 0);
  CHECK(parsed(confirmed).value("removed", 0) == 1);
  const auto after =
      parsed(server.cli({"node", "history", "preview", source.id}));
  CHECK(after.value("current_count", 0) == 1);
  CHECK(after.value("history_count", 0) == 4);
  const auto runtime_after = parsed(server.cli({"status"}));
  INFO("runtime before=" << runtime_before.dump()
                         << " after=" << runtime_after.dump());
  CHECK(runtime_after.at("pid") == runtime_before.at("pid"));
  CHECK(runtime_after.at("active").at("confdir") ==
        runtime_before.at("active").at("confdir"));
  std::map<std::string, std::string> files_after;
  for (const auto &file : std::filesystem::directory_iterator(confdir))
    files_after.emplace(file.path().filename().string(),
                        read_file(file.path()));
  CHECK(files_after == runtime_files);
  CHECK(server
            .cli({"node", "history", "confirm", source.id, "--id",
                  "second-history", "--token", token, "--yes"})
            .code != 0); // Stale preview.
  CHECK(parsed(server.cli({"node", "history", "preview", source.id}))
            .value("history_count", 0) == 4);
  CHECK(server
            .cli({"node", "history", "confirm", source.id, "--id",
                  "selected-history", "--token",
                  after.at("token").get<std::string>(), "--yes"})
            .code != 0);
  auto changed_store = Store::open(temp.path / "state.sqlite");
  REQUIRE(changed_store);
  source.name = "fixture settings changed";
  REQUIRE((*changed_store)->put_source(source));
  CHECK(server
            .cli({"node", "history", "confirm", source.id, "--id",
                  "second-history", "--token",
                  after.at("token").get<std::string>(), "--yes"})
            .code != 0);
  const auto after_source_change =
      parsed(server.cli({"node", "history", "preview", source.id}));
  second.labels = {"changed-after-preview"};
  REQUIRE((*changed_store)->put_node(second));
  changed_store->reset();
  CHECK(server
            .cli({"node", "history", "confirm", source.id, "--id",
                  "second-history", "--token",
                  after_source_change.at("token").get<std::string>(), "--yes"})
            .code != 0);
  auto live_store = Store::open(temp.path / "state.sqlite");
  REQUIRE(live_store);
  REQUIRE((*live_store)->erase_node(current.id));
  live_store->reset();
  const auto removed_status = parsed(server.cli({"status"}));
  REQUIRE(removed_status.at("state") == "running");
  CHECK(removed_status.at("active").at("selection_display_name") ==
        current.original_name);
  CHECK_FALSE(
      removed_status.at("active").at("selection_in_inventory").get<bool>());
  CHECK(removed_status.at("active").at("confdir") ==
        runtime_before.at("active").at("confdir"));
  CHECK(server.cli({"disconnect"}).code == 0);
}
struct TuiProcess {
  pid_t pid = -1;
  int master = -1;
};
TuiProcess start_tui(const std::filesystem::path &root,
                     const std::filesystem::path &socket,
                     const std::string &profile) {
  int master = -1, slave = -1;
  if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0)
    return {};
  termios terminal{};
  if (::tcgetattr(slave, &terminal) == 0) {
    // Keep Ctrl-S available to the TUI editor instead of letting the PTY's
    // XON/XOFF line discipline consume it as terminal flow control.
    terminal.c_iflag &= static_cast<tcflag_t>(~IXON);
    (void)::tcsetattr(slave, TCSANOW, &terminal);
  }
  winsize size{40, 140, 0, 0};
  ::ioctl(slave, TIOCSWINSZ, &size);
  std::vector<std::string> values{XRM_TEST_MANAGER, "--socket",
                                  socket.string(),  "tui",
                                  "--profile",      profile};
  std::vector<char *> argv;
  for (auto &value : values)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  pid_t pid = -1;
  const int status = posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr,
                                 argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(slave);
  if (status != 0) {
    ::close(master);
    return {};
  }
  (void)root;
  return {pid, master};
}
std::string read_tui(int master, std::chrono::milliseconds duration) {
  std::string captured;
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{master, POLLIN, 0};
    if (::poll(&descriptor, 1, 20) <= 0)
      continue;
    char bytes[4096];
    const auto count = ::read(master, bytes, sizeof(bytes));
    if (count > 0)
      captured.append(bytes, static_cast<std::size_t>(count));
  }
  return captured;
}
void send_tui(int master, std::string_view key) {
  (void)::write(master, key.data(), key.size());
  ::poll(nullptr, 0, 250);
}
} // namespace

TEST_CASE("runtime stats CLI and TUI report unavailable without fabricating "
          "zero counters",
          "[CLI-03][UI-03][stats]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "stats fixture"});
  REQUIRE(core.code == 0);
  auto profile =
      server.cli({"profile", "create", "--name", "stats-profile", "--core",
                  parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  auto node = server.cli(
      {"node", "add", "--name", "stats-direct", "--outbound-json",
       R"({"protocol":"freedom","settings":{},"tag":"stats-direct"})"});
  REQUIRE(node.code == 0);
  REQUIRE(server
              .cli({"connect", parsed(profile).at("id").get<std::string>(),
                    "--node", parsed(node).at("id").get<std::string>()})
              .code == 0);

  const auto unavailable = server.cli({"stats"});
  REQUIRE(unavailable.code == 0);
  const auto stats_json = parsed(unavailable);
  CHECK(stats_json.at("availability") == "unavailable");
  CHECK(stats_json.at("missing_conditions").is_array());
  CHECK(std::find(stats_json.at("missing_conditions").begin(),
                  stats_json.at("missing_conditions").end(),
                  "stats") != stats_json.at("missing_conditions").end());
  CHECK(std::find(stats_json.at("missing_conditions").begin(),
                  stats_json.at("missing_conditions").end(),
                  "policy") != stats_json.at("missing_conditions").end());
  CHECK(std::find(stats_json.at("missing_conditions").begin(),
                  stats_json.at("missing_conditions").end(),
                  "metrics") != stats_json.at("missing_conditions").end());

  const auto text_stats = server.cli_text({"stats"});
  REQUIRE(text_stats.code == 0);
  CHECK(text_stats.out.find("metrics: unavailable") != std::string::npos);
  CHECK(text_stats.out.find("stats") != std::string::npos);
  CHECK(text_stats.out.find("0 B/s") == std::string::npos);
  CHECK(text_stats.out.find("0 ms") == std::string::npos);

  auto tui = start_tui(temp.path, temp.path / "xrmd.sock",
                       parsed(profile).at("id").get<std::string>());
  REQUIRE(tui.pid > 0);
  REQUIRE(tui.master >= 0);
  (void)read_tui(tui.master, std::chrono::milliseconds(300));
  send_tui(
      tui.master,
      "\t"); // Exercise TopNav rather than removed numeric direct navigation.
  (void)read_tui(tui.master, std::chrono::milliseconds(120));
  send_tui(tui.master, "\x1b[Z"); // Return to Overview.
  const auto screen = read_tui(tui.master, std::chrono::milliseconds(300));
  CHECK(screen.find("Statistics") != std::string::npos);
  CHECK(screen.find("unavailable") != std::string::npos);
  send_tui(tui.master, "q");
  int status = 0;
  for (int attempt = 0;
       attempt < 100 && ::waitpid(tui.pid, &status, WNOHANG) == 0; ++attempt)
    ::poll(nullptr, 0, 20);
  if (::waitpid(tui.pid, &status, WNOHANG) == 0) {
    ::kill(tui.pid, SIGKILL);
    ::waitpid(tui.pid, &status, 0);
  }
  ::close(tui.master);
  REQUIRE(WIFEXITED(status));
  REQUIRE(server.stop());
}

TEST_CASE("Overview statistics keep missing metrics and latency explicitly "
          "unavailable",
          "[UI-03][stats-view]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "stats view fixture"});
  REQUIRE(core.code == 0);
  auto profile =
      server.cli({"profile", "create", "--name", "stats-view", "--core",
                  parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  auto node = server.cli(
      {"node", "add", "--name", "stats-view-direct", "--outbound-json",
       R"({"protocol":"freedom","settings":{},"tag":"stats-view-direct"})"});
  REQUIRE(node.code == 0);
  REQUIRE(server
              .cli({"connect", parsed(profile).at("id").get<std::string>(),
                    "--node", parsed(node).at("id").get<std::string>()})
              .code == 0);
  auto tui = start_tui(temp.path, temp.path / "xrmd.sock",
                       parsed(profile).at("id").get<std::string>());
  REQUIRE(tui.pid > 0);
  REQUIRE(tui.master >= 0);
  (void)read_tui(tui.master, std::chrono::milliseconds(300));
  send_tui(tui.master, "\t");
  (void)read_tui(tui.master, std::chrono::milliseconds(120));
  send_tui(tui.master, "\x1b[Z");
  const auto screen = read_tui(tui.master, std::chrono::milliseconds(300));
  CHECK(screen.find("Runtime") != std::string::npos);
  CHECK(screen.find("ACTUAL") != std::string::npos);
  CHECK(screen.find("Statistics") != std::string::npos);
  CHECK(screen.find("Traffic: unavailable") != std::string::npos);
  CHECK(screen.find("latency: unmeasured") != std::string::npos);
  CHECK(screen.find("0 B/s") == std::string::npos);
  CHECK(screen.find("0 ms") == std::string::npos);
  send_tui(tui.master, "q");
  int status = 0;
  for (int attempt = 0;
       attempt < 100 && ::waitpid(tui.pid, &status, WNOHANG) == 0; ++attempt)
    ::poll(nullptr, 0, 20);
  if (::waitpid(tui.pid, &status, WNOHANG) == 0) {
    ::kill(tui.pid, SIGKILL);
    ::waitpid(tui.pid, &status, 0);
  }
  ::close(tui.master);
  REQUIRE(WIFEXITED(status));
  REQUIRE(server.stop());
}

TEST_CASE(
    "stats --watch keeps a UDS session alive until it sends an explicit stop",
    "[CLI-03][stats-watch]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli({"core", "register", XRM_TEST_FAKE_CORE, "--name",
                          "stats watch fixture"});
  REQUIRE(core.code == 0);
  auto profile =
      server.cli({"profile", "create", "--name", "stats-watch", "--core",
                  parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);

  int output_pipe[2]{};
  REQUIRE(::pipe(output_pipe) == 0);
  const int flags = ::fcntl(output_pipe[0], F_GETFL, 0);
  REQUIRE(flags >= 0);
  REQUIRE(::fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0);
  std::vector<std::string> args{XRM_TEST_MANAGER, "--socket",
                                (temp.path / "xrmd.sock").string(), "stats",
                                "--watch"};
  std::vector<char *> argv;
  for (auto &value : args)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
  REQUIRE(posix_spawn_file_actions_adddup2(&actions, output_pipe[1],
                                           STDOUT_FILENO) == 0);
  REQUIRE(posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                           O_WRONLY, 0) == 0);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr,
                                  argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(output_pipe[1]);
  REQUIRE(spawned == 0);

  std::string output;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{output_pipe[0], POLLIN, 0};
    if (::poll(&descriptor, 1, 50) > 0 && (descriptor.revents & POLLIN) != 0) {
      char bytes[4096];
      const auto count = ::read(output_pipe[0], bytes, sizeof(bytes));
      if (count > 0)
        output.append(bytes, static_cast<std::size_t>(count));
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid)
      break;
  }
  CHECK(output.find("metrics: unavailable") != std::string::npos);
  REQUIRE(::kill(pid, SIGINT) == 0);
  int status = 0;
  for (int attempt = 0; attempt < 300 && ::waitpid(pid, &status, WNOHANG) == 0;
       ++attempt) {
    pollfd descriptor{output_pipe[0], POLLIN, 0};
    if (::poll(&descriptor, 1, 10) > 0 && (descriptor.revents & POLLIN) != 0) {
      char bytes[4096];
      const auto count = ::read(output_pipe[0], bytes, sizeof(bytes));
      if (count > 0)
        output.append(bytes, static_cast<std::size_t>(count));
    }
  }
  if (::waitpid(pid, &status, WNOHANG) == 0) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  }
  ::close(output_pipe[0]);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  const auto first_sample = output.find("metrics: unavailable");
  REQUIRE(first_sample != std::string::npos);
  CHECK(output.find("metrics: unavailable", first_sample + 1) !=
        std::string::npos);
  REQUIRE(server.stop());
}

TEST_CASE("stats --watch emits JSON before exit and reports daemon loss",
          "[CLI-UX][stats-watch]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  const auto out = temp.path / "watch.out";
  const auto err = temp.path / "watch.err";
  std::vector<std::string> args{
      XRM_TEST_MANAGER, "--socket", (temp.path / "xrmd.sock").string(),
      "--json",         "stats",    "--watch"};
  std::vector<char *> argv;
  for (auto &value : args)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
  REQUIRE(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out.c_str(),
                                           O_CREAT | O_TRUNC | O_WRONLY,
                                           0600) == 0);
  REQUIRE(posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err.c_str(),
                                           O_CREAT | O_TRUNC | O_WRONLY,
                                           0600) == 0);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr,
                                  argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  REQUIRE(spawned == 0);
  std::string live_output;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    live_output = read_file(out);
    if (live_output.find('\n') != std::string::npos)
      break;
    ::poll(nullptr, 0, 20);
  }
  INFO("watch output before shutdown: " << live_output);
  CHECK(live_output.find('\n') != std::string::npos);
  int status = 0;
  const auto before_stop = ::waitpid(pid, &status, WNOHANG);
  CHECK(before_stop == 0);
  REQUIRE(server.stop());
  bool exited = before_stop == pid;
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (exited)
      break;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      exited = true;
      break;
    }
    ::poll(nullptr, 0, 10);
  }
  if (!exited && before_stop == 0) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  }
  REQUIRE(exited);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 3);
  const auto output = read_file(out);
  const auto first = output.substr(0, output.find('\n'));
  CHECK(Json::parse(first).value("availability", "") == "unavailable");
  const auto error = read_file(err);
  INFO("watch stderr: " << error);
  CHECK(Json::parse(error).at("error").value("code", "") == "io_error");
}

TEST_CASE("FORM-03 traffic statistics convenience action saves only its native "
          "modules",
          "[FORM-03][CLI-02][CLI-03]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "forms fixture"});
  INFO("fake core registration: " << core.out << core.err);
  REQUIRE(core.code == 0);
  auto profile =
      server.cli({"profile", "create", "--name", "statistics-form", "--core",
                  parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  const auto profile_id = parsed(profile).at("id").get<std::string>();
  REQUIRE(
      server
          .cli(
              {"config", "edit", profile_id, "policy", "--text",
               R"({"policy":{"levels":{"0":{"handshake":3,"customLevel":"preserve"}},"system":{"statsOutboundUplink":false,"customSystem":{"keep":true}},"extension":{"keep":7}}})"})
          .code == 0);
  REQUIRE(
      server
          .cli(
              {"config", "edit", profile_id, "metrics", "--text",
               R"({"metrics":{"listen":"127.0.0.1:9100","customMetric":{"keep":"yes"}}})"})
          .code == 0);

  const auto action = server.cli({"config", "statistics", "enable", profile_id,
                                  "--metrics-listen", "127.0.0.1:9101"});
  REQUIRE(action.code == 0);
  const auto action_json = parsed(action);
  REQUIRE(action_json.at("changed_modules").is_array());
  CHECK(action_json.at("changed_modules") ==
        Json::array({"stats", "policy", "metrics"}));
  CHECK(action_json.at("planned_modules") ==
        Json::array({"stats", "policy", "metrics"}));

  const auto policy_result =
      server.cli({"config", "show", profile_id, "--module-key", "policy"});
  const auto metrics_result =
      server.cli({"config", "show", profile_id, "--module-key", "metrics"});
  const auto stats_result =
      server.cli({"config", "show", profile_id, "--module-key", "stats"});
  INFO("stats show output: " << stats_result.out << stats_result.err);
  REQUIRE(policy_result.code == 0);
  REQUIRE(metrics_result.code == 0);
  REQUIRE(stats_result.code == 0);
  const auto policy =
      Json::parse(parsed(policy_result).at("text").get<std::string>())
          .at("policy");
  CHECK(policy.at("levels").at("0").at("handshake") == 3);
  CHECK(policy.at("levels").at("0").at("customLevel") == "preserve");
  CHECK(policy.at("system").at("customSystem").at("keep") == true);
  CHECK(policy.at("system").at("statsOutboundUplink") == true);
  CHECK(policy.at("extension").at("keep") == 7);
  const auto metrics =
      Json::parse(parsed(metrics_result).at("text").get<std::string>())
          .at("metrics");
  CHECK(metrics.at("listen") == "127.0.0.1:9101");
  CHECK(metrics.at("customMetric").at("keep") == "yes");
  CHECK(Json::parse(parsed(stats_result).at("text").get<std::string>())
            .at("stats")
            .is_object());
  CHECK(parsed(server.cli({"status"})).at("state") == "stopped");
  REQUIRE(server.stop());
}

TEST_CASE(
    "E2E-02 manual node through Profile, modules, connect, logs, and stop",
    "[E2E-02][CLI-02][RPC-03]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "loopback fixture"});
  REQUIRE(core.code == 0);
  const auto core_id = parsed(core).at("id").get<std::string>();
  auto created = server.cli(
      {"profile", "create", "--name", "local-test", "--core", core_id});
  REQUIRE(created.code == 0);
  const auto profile_id = parsed(created).at("id").get<std::string>();
  auto node =
      server.cli({"node", "add", "--name", "manual-direct", "--outbound-json",
                  R"({"protocol":"freedom","settings":{},"tag":"manual"})"});
  REQUIRE(node.code == 0);
  const auto node_id = parsed(node).at("id").get<std::string>();
  auto modules = server.cli({"config", "modules", profile_id});
  REQUIRE(modules.code == 0);
  REQUIRE(parsed(modules).is_array());
  REQUIRE(parsed(modules).size() == 15);
  auto selected = server.cli({"select", "node", profile_id, node_id});
  REQUIRE(selected.code == 0);
  REQUIRE(parsed(server.cli({"status"})).at("state") == "stopped");
  auto applied = server.cli({"config", "apply", profile_id});
  REQUIRE(applied.code == 0);
  REQUIRE(parsed(applied).at("effect") == "checked_only");
  REQUIRE(parsed(server.cli({"status"})).at("state") == "stopped");
  auto rendered = server.cli({"config", "render", profile_id});
  REQUIRE(rendered.code == 0);
  const auto rendered_json = parsed(rendered);
  REQUIRE(rendered_json.at("files").is_array());
  REQUIRE(std::any_of(
      rendered_json.at("files").begin(), rendered_json.at("files").end(),
      [](const auto &file) { return file == "90_nodes_tail.json"; }));
  auto checked = server.cli({"config", "check", profile_id});
  REQUIRE(checked.code == 0);
  auto connected = server.cli({"connect", profile_id});
  REQUIRE(connected.code == 0);
  auto status = server.cli({"status"});
  REQUIRE(status.code == 0);
  REQUIRE(parsed(status).at("state") == "running");
  auto logs = server.cli({"logs"});
  REQUIRE(logs.code == 0);
  REQUIRE(parsed(logs).contains("entries"));
  auto disconnected = server.cli({"disconnect"});
  REQUIRE(disconnected.code == 0);
  REQUIRE(parsed(disconnected).at("effect") == "stopped");
  REQUIRE(parsed(server.cli({"status"})).at("state") == "stopped");
  auto unknown_operation =
      server.cli({"operation", "get", "unknown-from-this-boot"});
  CHECK(unknown_operation.code != 0);
  REQUIRE(server.stop());
}

TEST_CASE("UI-09 CLI Profile references resolve exact names and UUIDs without "
          "guessing",
          "[UI-09][CLI-Profile-resolver]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "resolver fixture"});
  REQUIRE(core.code == 0);
  const auto core_id = parsed(core).at("id").get<std::string>();

  auto home =
      server.cli({"profile", "create", "--name", "home", "--core", core_id});
  REQUIRE(home.code == 0);
  const auto home_id = parsed(home).at("id").get<std::string>();

  // A unique exact human name and the stable UUID must address the same
  // Profile.
  auto by_name = server.cli({"profile", "show", "home"});
  REQUIRE(by_name.code == 0);
  CHECK(parsed(by_name).at("id") == home_id);
  auto by_uuid = server.cli({"profile", "show", home_id});
  REQUIRE(by_uuid.code == 0);
  CHECK(parsed(by_uuid).at("id") == home_id);
  CHECK(server.cli({"profile", "show", "hom"}).code != 0);

  // Duplicate names are legal data, so the CLI must reject ambiguity instead of
  // choosing whichever row profile.list happened to return first.
  auto duplicate =
      server.cli({"profile", "create", "--name", "home", "--core", core_id});
  REQUIRE(duplicate.code == 0);
  auto ambiguous = server.cli({"profile", "show", "home"});
  CHECK(ambiguous.code != 0);
  CHECK((ambiguous.err.find("ambiguous") != std::string::npos ||
         ambiguous.err.find("歧义") != std::string::npos ||
         ambiguous.err.find("重名") != std::string::npos ||
         ambiguous.err.find("不唯一") != std::string::npos));

  auto missing = server.cli({"profile", "show", "does-not-exist"});
  CHECK(missing.code != 0);
  CHECK((missing.err.find("not_found") != std::string::npos ||
         missing.err.find("找不到") != std::string::npos));
  REQUIRE(server.stop());
}

TEST_CASE("CLI human Profile and runtime receipts preserve machine results and "
          "saved scope",
          "[CLI-UX]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  const auto core = parsed(server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "UX core"}));
  REQUIRE(core.contains("id"));
  const auto core_id = core.at("id").get<std::string>();

  const auto created = server.cli_text(
      {"profile", "create", "--name", "UX Home", "--core", core_id});
  REQUIRE(created.code == 0);
  CHECK(created.err.empty());
  CHECK(created.out.find("Created Profile UX Home [") != std::string::npos);
  CHECK(created.out.find("no core started") != std::string::npos);
  const auto profiles = parsed(server.cli({"profile", "list"}));
  REQUIRE(profiles.is_array());
  REQUIRE(profiles.size() == 1);
  const auto profile_id = profiles[0].at("id").get<std::string>();
  CHECK(created.out.find(profile_id) != std::string::npos);
  const auto listed = server.cli_text({"profile", "list"});
  CHECK(listed.out.find(profile_id) != std::string::npos);

  const auto copy =
      server.cli_text({"profile", "clone", "UX Home", "--name", "UX Copy"});
  REQUIRE(copy.code == 0);
  CHECK(copy.out.find("Copied Profile UX Copy [") != std::string::npos);
  const auto cloned = parsed(server.cli({"profile", "show", "UX Copy"}));
  REQUIRE(cloned.contains("id"));
  const auto copy_id = cloned.at("id").get<std::string>();
  CHECK(copy_id != profile_id);
  const auto renamed =
      server.cli_text({"profile", "rename", copy_id, "--name", "UX Renamed"});
  REQUIRE(renamed.code == 0);
  CHECK(renamed.out.find("Renamed Profile") != std::string::npos);
  CHECK(parsed(server.cli({"profile", "show", "UX Renamed"})).at("id") ==
        copy_id);
  const auto removed = server.cli_text({"profile", "remove", "UX Renamed"});
  REQUIRE(removed.code == 0);
  CHECK(removed.out.find("Removed Profile") != std::string::npos);
  CHECK(server.cli({"profile", "show", copy_id}).code != 0);

  const auto source =
      parsed(server.cli({"sub", "add", "--name", "Private source", "--url",
                         "http://user:secret@127.0.0.1:1/sub", "--paused"}));
  REQUIRE(source.contains("id"));
  const auto source_id = source.at("id").get<std::string>();
  const auto source_human = server.cli_text({"sub", "show", source_id});
  CHECK(source_human.code == 0);
  CHECK(source_human.out.find(source_id) != std::string::npos);
  CHECK(source_human.out.find("secret") == std::string::npos);
  CHECK(parsed(server.cli({"sub", "show", source_id}))
            .at("subscription")
            .at("url") == "http://user:secret@127.0.0.1:1/sub");
  const auto private_node = parsed(server.cli(
      {"node", "add", "--name", "Private node", "--outbound-json",
       R"({"protocol":"vless","settings":{"vnext":[{"address":"example.invalid","port":443,"users":[{"id":"secret-id","encryption":"none"}]}]}})"}));
  REQUIRE(private_node.contains("id"));
  const auto private_id = private_node.at("id").get<std::string>();
  const auto node_human = server.cli_text({"node", "show", private_id});
  CHECK(node_human.code == 0);
  CHECK(node_human.out.find(private_id) != std::string::npos);
  CHECK(node_human.out.find("secret-id") == std::string::npos);
  CHECK(parsed(server.cli({"node", "show", private_id}))
            .dump()
            .find("secret-id") != std::string::npos);

  const auto node = parsed(server.cli(
      {"node", "add", "--name", "UX direct", "--outbound-json",
       R"({"protocol":"freedom","settings":{},"tag":"ux-direct"})"}));
  REQUIRE(node.contains("id"));
  const auto node_id = node.at("id").get<std::string>();
  REQUIRE(server.cli({"select", "node", profile_id, node_id}).code == 0);
  const auto stopped_json = server.cli({"status"});
  REQUIRE(stopped_json.code == 0);
  REQUIRE_FALSE(stopped_json.out.empty());
  CHECK(stopped_json.out.find('\n') == stopped_json.out.size() - 1);
  CHECK(parsed(stopped_json).at("saved_target").is_null());
  const auto stopped_text = server.cli_text({"status"});
  CHECK(stopped_text.out.find("saved target: scope is running Profile only") !=
        std::string::npos);
  const auto detail = server.cli_text({"profile", "show", "UX Home"});
  CHECK(detail.out.find("Saved target: node " + node_id) != std::string::npos);
  CHECK(detail.out.find("process_environment") == std::string::npos);
  CHECK_FALSE(parsed(server.cli({"profile", "show", "UX Home"}))
                  .contains("source_text"));

  const auto connected = server.cli_text({"connect", "UX Home"});
  INFO(connected.out << connected.err);
  REQUIRE(connected.code == 0);
  CHECK(connected.out.find(profile_id) != std::string::npos);
  CHECK(connected.out.find("traffic unverified") != std::string::npos);
  CHECK(parsed(server.cli({"status"})).at("state") == "running");
  const auto disconnected = server.cli_text({"disconnect"});
  REQUIRE(disconnected.code == 0);
  CHECK(disconnected.out.find(
            "Core is stopped; saved Profile settings unchanged") !=
        std::string::npos);

  const auto missing = server.cli_text({"profile", "show", "missing"});
  CHECK(missing.code != 0);
  CHECK(missing.out.empty());
  CHECK(missing.err.find("Error: xrm profile show — Profile missing") !=
        std::string::npos);
  CHECK(missing.err.find("Reason:") != std::string::npos);
  CHECK(missing.err.find("Recovery:") != std::string::npos);
  CHECK(missing.err.find("Diagnostic (original):") == std::string::npos);
  const auto verbose_missing =
      server.cli_text({"--verbose", "profile", "show", "missing"});
  CHECK(verbose_missing.code != 0);
  CHECK(verbose_missing.err.find("Diagnostic (original):") !=
        std::string::npos);
  CHECK(missing.err.find("profile.get [missing]") == std::string::npos);
  CHECK(missing.err.find((temp.path / "xrmd.sock").string()) !=
        std::string::npos);
  const auto missing_json = server.cli({"profile", "show", "missing"});
  CHECK(missing_json.code != 0);
  CHECK(missing_json.out.empty());
  CHECK(Json::parse(missing_json.err).contains("error"));
  REQUIRE(server.stop());
}

TEST_CASE("E2E-05 installing/using a registered core stays separate from "
          "applying module edits",
          "[E2E-05][CLI-03]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  const auto original_core = parsed(server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "original core"}));
  REQUIRE(original_core.contains("id"));
  const auto profile =
      parsed(server.cli({"profile", "create", "--name", "core-use-profile",
                         "--core", original_core.at("id").get<std::string>()}));
  REQUIRE(profile.contains("id"));
  const auto node = parsed(server.cli(
      {"node", "add", "--name", "core-use-direct", "--outbound-json",
       R"({"protocol":"freedom","settings":{},"tag":"core-use-direct"})"}));
  REQUIRE(node.contains("id"));
  REQUIRE(server
              .cli({"select", "node", profile.at("id").get<std::string>(),
                    node.at("id").get<std::string>()})
              .code == 0);
  const auto connected =
      server.cli({"connect", profile.at("id").get<std::string>()});
  INFO("initial connection: " << connected.out << connected.err);
  REQUIRE(connected.code == 0);
  const auto before = parsed(server.cli({"status"}));
  INFO("status before core switch: " << before.dump());
  REQUIRE(before.at("state") == "running");
  const auto confdir = before.at("active").at("confdir").get<std::string>();
  const auto original_files = std::filesystem::directory_iterator(confdir);
  std::map<std::string, std::string> bytes_before;
  for (const auto &entry : original_files)
    bytes_before.emplace(entry.path().filename().string(),
                         read_file(entry.path()));

  const auto module_call =
      server.cli({"config", "show", profile.at("id").get<std::string>(),
                  "--module-key", "log"});
  INFO("module read result: " << module_call.out << module_call.err);
  const auto module_before = parsed(module_call);
  INFO("source log module: " << module_before.dump());
  const auto module_text = module_before.at("text").get<std::string>();
  auto module = Json::parse(module_text);
  module["log"]["loglevel"] = "warning";
  REQUIRE(server
              .cli({"config", "edit", profile.at("id").get<std::string>(),
                    "log", "--text", module.dump()})
              .code == 0);

  const auto second_core = parsed(server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "replacement core"}));
  REQUIRE(second_core.contains("id"));
  const auto used =
      server.cli({"core", "use", profile.at("id").get<std::string>(),
                  second_core.at("id").get<std::string>()});
  INFO("core use output: " << used.out << used.err);
  REQUIRE(used.code == 0);
  const auto after = parsed(server.cli({"status"}));
  CHECK(after.at("state") == "running");
  CHECK(after.at("active").at("core_id") == second_core.at("id"));
  CHECK(after.at("active").at("confdir") == confdir);
  CHECK(after.at("pid") != before.at("pid"));
  for (const auto &entry : std::filesystem::directory_iterator(confdir))
    CHECK(bytes_before.at(entry.path().filename().string()) ==
          read_file(entry.path()));
  CHECK(parsed(server.cli(
                   {"profile", "show", profile.at("id").get<std::string>()}))
            .at("core_id") == second_core.at("id"));
  REQUIRE(server.cli({"core", "stop"}).code == 0);
  REQUIRE(server.stop());
}

TEST_CASE(
    "CLI-03 dedicated advanced module forms save their own native source files",
    "[CLI-03][FORM-01][FORM-05][MOD-07]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  const auto core = parsed(server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "forms core"}));
  const auto profile =
      parsed(server.cli({"profile", "create", "--name", "forms-profile",
                         "--core", core.at("id").get<std::string>()}));
  const auto id = profile.at("id").get<std::string>();

  const auto env_edit =
      server.cli({"config", "env", "set", id, "XRM_NATIVE_TEST", "kept"});
  INFO("native env form: " << env_edit.out << env_edit.err);
  CHECK(env_edit.code == 0);
  auto env = Json::parse(
      parsed(server.cli({"config", "show", id, "--module-key", "env"}))
          .at("text")
          .get<std::string>());
  CHECK(env.at("env").at("XRM_NATIVE_TEST") == "kept");
  CHECK(server
            .cli({"config", "version", "range", id, "--min", "26.0.0", "--max",
                  "27.0.0"})
            .code == 0);
  auto version = Json::parse(
      parsed(server.cli({"config", "show", id, "--module-key", "version"}))
          .at("text")
          .get<std::string>());
  CHECK(version.at("version").at("min") == "26.0.0");
  CHECK(version.at("version").at("max") == "27.0.0");
  CHECK(server.cli({"config", "geodata", "schedule", id, "0 0 1 * *"}).code ==
        0);
  auto geodata = Json::parse(
      parsed(server.cli({"config", "show", id, "--module-key", "geodata"}))
          .at("text")
          .get<std::string>());
  CHECK(geodata.at("geodata").at("cron") == "0 0 1 * *");
  CHECK(server
            .cli({"config", "api", "services", id, "HandlerService",
                  "RoutingService"})
            .code == 0);
  auto api = Json::parse(
      parsed(server.cli({"config", "show", id, "--module-key", "api"}))
          .at("text")
          .get<std::string>());
  CHECK(api.at("api").at("services") ==
        Json::array({"HandlerService", "RoutingService"}));
  CHECK(
      server.cli({"config", "metrics", "listen", id, "127.0.0.1:19999"}).code ==
      0);
  auto metrics = Json::parse(
      parsed(server.cli({"config", "show", id, "--module-key", "metrics"}))
          .at("text")
          .get<std::string>());
  CHECK(metrics.at("metrics").at("listen") == "127.0.0.1:19999");
  CHECK(server.cli({"config", "log", "level", id, "warning"}).code == 0);
  auto log = Json::parse(
      parsed(server.cli({"config", "show", id, "--module-key", "log"}))
          .at("text")
          .get<std::string>());
  CHECK(log.at("log").at("loglevel") == "warning");
  REQUIRE(server.stop());
}

TEST_CASE(
    "E2E-04 managed manual group is saved, selected, and assembled through CLI",
    "[E2E-04][CLI-02][CLI-03]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "group fixture"});
  REQUIRE(core.code == 0);
  auto profile =
      server.cli({"profile", "create", "--name", "group-profile", "--core",
                  parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  const auto profile_id = parsed(profile).at("id").get<std::string>();
  auto first =
      server.cli({"node", "add", "--name", "group-a", "--outbound-json",
                  R"({"protocol":"freedom","settings":{},"tag":"group-a"})"});
  auto second =
      server.cli({"node", "add", "--name", "group-b", "--outbound-json",
                  R"({"protocol":"freedom","settings":{},"tag":"group-b"})"});
  REQUIRE(first.code == 0);
  REQUIRE(second.code == 0);
  auto added = server.cli({"group", "add", profile_id, "--name", "Pinned",
                           "--tag", "pinned", "--mode", "manual", "--members",
                           parsed(first).at("id").get<std::string>(),
                           parsed(second).at("id").get<std::string>()});
  INFO(added.err);
  REQUIRE(added.code == 0);
  const auto group_id = parsed(added).at("id").get<std::string>();
  auto listed = server.cli({"group", "list", profile_id});
  REQUIRE(listed.code == 0);
  REQUIRE(parsed(listed).size() == 1);
  auto shown = server.cli({"group", "show", profile_id, group_id});
  REQUIRE(shown.code == 0);
  auto group_document = parsed(shown);
  group_document["name"] = "Pinned edited";
  auto edited = server.cli(
      {"group", "edit", profile_id, "--json", group_document.dump()});
  REQUIRE(edited.code == 0);
  CHECK(
      parsed(server.cli({"group", "show", profile_id, group_id})).at("name") ==
      "Pinned edited");
  auto select_member = server.cli({"group", "select", profile_id, group_id,
                                   parsed(second).at("id").get<std::string>()});
  REQUIRE(select_member.code == 0);
  auto select_target = server.cli({"select", "group", profile_id, group_id});
  REQUIRE(select_target.code == 0);
  auto rendered = server.cli({"config", "render", profile_id});
  REQUIRE(rendered.code == 0);
  const auto directory = parsed(rendered).at("confdir").get<std::string>();
  const auto routing =
      read_file(std::filesystem::path(directory) / "04_routing.json");
  const auto outbounds =
      read_file(std::filesystem::path(directory) / "03_outbounds.json");
  const auto tail =
      read_file(std::filesystem::path(directory) / "90_nodes_tail.json");
  CHECK(routing.find("pinned") != std::string::npos);
  CHECK(outbounds.find("pinned") == std::string::npos);
  CHECK(tail.find(node_tag(parsed(first).at("id").get<std::string>())) !=
        std::string::npos);
  CHECK(tail.find(node_tag(parsed(second).at("id").get<std::string>())) !=
        std::string::npos);
  auto connected = server.cli({"connect", profile_id});
  REQUIRE(connected.code == 0);
  auto status = parsed(server.cli({"status"}));
  REQUIRE(status.at("active").at("selection").at("kind") == "group");
  CHECK(status.at("saved_target").at("id_or_tag") == group_id);
  CHECK(status.at("actual_target").at("id_or_tag") == group_id);
  auto no_api =
      server.cli({"group", "override", "--runtime", "--clear", "pinned"});
  CHECK(no_api.code != 0);
  CHECK(parsed(server.cli({"profile", "show", profile_id}))
            .at("selected")
            .at("id_or_tag") == group_id);
  auto stopped = server.cli({"disconnect"});
  REQUIRE(stopped.code == 0);
  auto restarted = server.cli({"connect", profile_id});
  REQUIRE(restarted.code == 0);
  CHECK(parsed(server.cli({"status"}))
            .at("active")
            .at("selection")
            .at("id_or_tag") == group_id);
  REQUIRE(server.cli({"disconnect"}).code == 0);
  REQUIRE(server.stop());
}

TEST_CASE("UI-04 TUI module page renders all native module keys and UI-03 "
          "exposes saved Groups",
          "[UI-04][UI-03]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "tui fixture"});
  REQUIRE(core.code == 0);
  auto profile =
      server.cli({"profile", "create", "--name", "tui-profile", "--core",
                  parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  auto node =
      server.cli({"node", "add", "--name", "tui-group-node", "--outbound-json",
                  R"({"protocol":"freedom","settings":{}})"});
  REQUIRE(node.code == 0);
  auto group = server.cli(
      {"group", "add", parsed(profile).at("id").get<std::string>(), "--name",
       "TUI policy", "--tag", "tui-policy", "--mode", "manual", "--members",
       parsed(node).at("id").get<std::string>()});
  REQUIRE(group.code == 0);

  int master = -1;
  int slave = -1;
  REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
  winsize size{40, 140, 0, 0};
  REQUIRE(::ioctl(slave, TIOCSWINSZ, &size) == 0);
  std::vector<std::string> arguments{
      XRM_TEST_MANAGER,
      "--socket",
      (temp.path / "xrmd.sock").string(),
      "tui",
      "--profile",
      parsed(profile).at("id").get<std::string>()};
  std::vector<char *> argv;
  for (auto &value : arguments)
    argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr,
                                  argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(slave);
  REQUIRE(spawned == 0);
  std::string screen = read_tui(master, std::chrono::milliseconds(500));
  const auto press = [&](std::string_view key) {
    REQUIRE(::write(master, key.data(), key.size()) ==
            static_cast<ssize_t>(key.size()));
    screen += read_tui(master, std::chrono::milliseconds(120));
  };
  press("\t");
  press("\t"); // Overview -> Modules TopNav.
  press("\r"); // SecondaryNav, no editor yet.
  for (int i = 0; i < 15; ++i)
    press("\t"); // Visit every native module tab.
  press("\x1b"); // Modules -> TopNav.
  press("\t");
  press("\t"); // Nodes.
  press("\r"); // Node categories.
  press("\t");
  press("\t"); // Groups category.
  press("\r"); // Group list Content.
  press("\x1b");
  press("\x1b"); // Back to TopNav.
  press("\t");
  press("\t"); // Config.
  press("\r");
  press("\r"); // Core Content.
  press("\x1b");
  press("\x1b"); // TopNav before q.
  press("q");
  int child_status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    char bytes[4096];
    const auto count = ::read(master, bytes, sizeof(bytes));
    if (count > 0)
      screen.append(bytes, static_cast<std::size_t>(count));
    const auto waited = ::waitpid(pid, &child_status, WNOHANG);
    if (waited == pid)
      break;
    ::poll(nullptr, 0, 10);
  }
  ::close(master);
  if (::waitpid(pid, &child_status, WNOHANG) == 0) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &child_status, 0);
  }
  REQUIRE(WIFEXITED(child_status));
  REQUIRE(WEXITSTATUS(child_status) == 0);
  for (const auto *key :
       {"env", "log", "api", "dns", "routing", "policy", "inbounds",
        "outbounds", "stats", "fakedns", "metrics", "observatory",
        "burstObservatory", "geodata", "version"})
    CHECK(screen.find(key) != std::string::npos);
  CHECK(screen.find("TUI policy") != std::string::npos);
  CHECK(screen.find("Config") != std::string::npos);
  CHECK(screen.find("Core") != std::string::npos);
  CHECK((screen.find("registered") != std::string::npos ||
         screen.find("managed") != std::string::npos));
  REQUIRE(server.stop());
}

TEST_CASE("UI-01 text keys do not fire global actions and TUI exit leaves core "
          "running",
          "[UI-01]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli(
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "ui action fixture"});
  REQUIRE(core.code == 0);
  auto profile =
      server.cli({"profile", "create", "--name", "ui-actions", "--core",
                  parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  auto node =
      server.cli({"node", "add", "--name", "manual-ui", "--outbound-json",
                  R"({"protocol":"freedom","settings":{}})"});
  REQUIRE(node.code == 0);

  auto tui = start_tui(temp.path, temp.path / "xrmd.sock",
                       parsed(profile).at("id").get<std::string>());
  REQUIRE(tui.pid > 0);
  REQUIRE(tui.master >= 0);
  (void)read_tui(tui.master, std::chrono::milliseconds(300));
  const auto tap = [&](std::string_view key) {
    send_tui(tui.master, key);
    return read_tui(tui.master, std::chrono::milliseconds(120));
  };
  (void)tap("\t");
  (void)tap("\r"); // Profiles Content.
  (void)tap("n");  // Shared multi-field Profile form.
  send_tui(tui.master, "qe ?");
  const auto draft_screen =
      read_tui(tui.master, std::chrono::milliseconds(120));
  CHECK(draft_screen.find("qe ?") != std::string::npos);
  int status = 0;
  CHECK(::waitpid(tui.pid, &status, WNOHANG) == 0);
  (void)tap("\x1b"); // Dirty form confirmation.
  (void)tap("\t");
  (void)tap("\r"); // Discard; Profile was not created.
  REQUIRE(parsed(server.cli({"profile", "list"})).size() == 1);
  (void)tap("\x1b"); // Profiles -> TopNav.
  for (int i = 0; i < 3; ++i)
    (void)tap("\t"); // Nodes TopNav.
  (void)tap("\r");
  (void)tap("\t");
  (void)tap("\r"); // Local Content.
  const auto details = tap("\r");
  CHECK(details.find("manual-ui") != std::string::npos);
  CHECK(details.find("Details") != std::string::npos);
  (void)tap("\x1b"); // Close read-only node details before actions.
  (void)tap("s");
  (void)tap("c");
  const auto running_screen = read_tui(tui.master, std::chrono::seconds(3));
  CHECK(running_screen.find("Connect ui-actions") != std::string::npos);
  CHECK(::waitpid(tui.pid, &status, WNOHANG) == 0);
  (void)tap("\x1b");
  (void)tap("\x1b"); // q belongs only to TopNav.
  (void)tap("q");
  for (int attempt = 0;
       attempt < 100 && ::waitpid(tui.pid, &status, WNOHANG) == 0; ++attempt)
    ::poll(nullptr, 0, 20);
  if (::waitpid(tui.pid, &status, WNOHANG) == 0) {
    ::kill(tui.pid, SIGKILL);
    ::waitpid(tui.pid, &status, 0);
  }
  ::close(tui.master);
  REQUIRE(WIFEXITED(status));
  const auto modules = parsed(server.cli(
      {"config", "modules", parsed(profile).at("id").get<std::string>()}));
  const auto env =
      std::find_if(modules.begin(), modules.end(), [](const auto &item) {
        return item.value("key", "") == "env";
      });
  REQUIRE(env != modules.end());
  CHECK_FALSE(env->value("included", true));
  auto runtime = server.cli({"status"});
  REQUIRE(runtime.code == 0);
  CHECK(parsed(runtime).at("state") == "running");
  CHECK(parsed(runtime).at("active").at("profile_id") ==
        parsed(profile).at("id"));
  const auto stopped = server.cli({"disconnect"});
  INFO(stopped.err);
  INFO(server.cli({"server", "info"}).err);
  INFO("server exit status: " << server.observed_exit());
  REQUIRE(stopped.code == 0);
  REQUIRE(server.stop());
}

TEST_CASE(
    "UI-04 advanced node form preserves typed subobjects and core selection",
    "[UI-04][M4]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli({"core", "register", XRM_TEST_FAKE_CORE, "--name",
                          "typed node editor fixture"});
  REQUIRE(core.code == 0);
  auto profile =
      server.cli({"profile", "create", "--name", "typed-node-ui", "--core",
                  parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  auto store = Store::open(temp.path / "state.sqlite");
  REQUIRE(store);
  auto typed = parse_vless_uri("vless://"
                               "00000000-0000-4000-8000-000000000099@127.0.0.1:"
                               "443?encryption=none#typed-ui-node");
  REQUIRE(typed);
  typed->node.id = "typed-ui-node-id";
  REQUIRE((*store)->put_node(typed->node));

  auto tui = start_tui(temp.path, temp.path / "xrmd.sock",
                       parsed(profile).at("id").get<std::string>());
  REQUIRE(tui.pid > 0);
  (void)read_tui(tui.master, std::chrono::milliseconds(250));
  const auto tap = [&](std::string_view key) {
    send_tui(tui.master, key);
    return read_tui(tui.master, std::chrono::milliseconds(120));
  };
  for (int i = 0; i < 4; ++i)
    (void)tap("\t"); // Nodes TopNav.
  (void)tap("\r");
  (void)tap("\t");
  (void)tap("\r");                // Local Content.
  const auto details = tap("\r"); // Open highlighted Local node.
  CHECK(details.find("typed-ui-node") != std::string::npos);
  CHECK(details.find("Details") != std::string::npos);
  (void)tap("\x1b"); // Close details; J acts on the selected node in Content.
  const auto editor = tap("J");
  INFO("typed node editor screen: " << editor);
  CHECK(editor.find("Node.settings (advanced JSON)") != std::string::npos);
  CHECK(editor.find("Name") != std::string::npos);
  const auto saved = tap("\x13");
  INFO("typed node save screen: " << saved);
  CHECK(saved.find("Node settings saved") != std::string::npos);
  (void)tap("\x1b");
  (void)tap("\x1b"); // SecondaryNav -> TopNav.
  (void)tap("q");
  int status = 0;
  for (int attempt = 0;
       attempt < 100 && ::waitpid(tui.pid, &status, WNOHANG) == 0; ++attempt)
    ::poll(nullptr, 0, 20);
  if (::waitpid(tui.pid, &status, WNOHANG) == 0) {
    ::kill(tui.pid, SIGKILL);
    ::waitpid(tui.pid, &status, 0);
  }
  ::close(tui.master);
  REQUIRE(WIFEXITED(status));
  const auto saved_node =
      parsed(server.cli({"node", "show", "typed-ui-node-id"}));
  CHECK(saved_node.at("settings").at("transport").at("kind") == "raw");

  auto core_tui = start_tui(temp.path, temp.path / "xrmd.sock",
                            parsed(profile).at("id").get<std::string>());
  REQUIRE(core_tui.pid > 0);
  (void)read_tui(core_tui.master, std::chrono::milliseconds(250));
  const auto core_tap = [&](std::string_view key) {
    send_tui(core_tui.master, key);
    return read_tui(core_tui.master, std::chrono::milliseconds(120));
  };
  for (int i = 0; i < 6; ++i)
    (void)core_tap("\t"); // Config.
  (void)core_tap("\r");
  const auto core_page = core_tap("\r"); // Core Content.
  CHECK(core_page.find("Core / release") != std::string::npos);
  const auto core_details = core_tap("\r");
  CHECK(core_details.find("Details") != std::string::npos);
  CHECK(core_details.find("typed node editor fixture") != std::string::npos);
  CHECK(core_details.find("reported_version:") != std::string::npos);
  (void)core_tap("\x1b");
  const auto core_help = core_tap("?");
  CHECK(core_help.find("Use core in viewed Profile") != std::string::npos);
  (void)core_tap("\x1b");
  send_tui(core_tui.master, "u");
  std::string use_result;
  for (int attempt = 0; attempt < 8; ++attempt) {
    use_result += read_tui(core_tui.master, std::chrono::milliseconds(250));
    if (use_result.find("Use core: completed") != std::string::npos)
      break;
    send_tui(core_tui.master, "r");
  }
  INFO("TUI core use result: " << use_result);
  CHECK(use_result.find("Use core: completed") != std::string::npos);
  (void)core_tap("\x1b");
  (void)core_tap("\x1b");
  (void)core_tap("q");
  status = 0;
  for (int attempt = 0;
       attempt < 100 && ::waitpid(core_tui.pid, &status, WNOHANG) == 0;
       ++attempt)
    ::poll(nullptr, 0, 20);
  if (::waitpid(core_tui.pid, &status, WNOHANG) == 0) {
    ::kill(core_tui.pid, SIGKILL);
    ::waitpid(core_tui.pid, &status, 0);
  }
  ::close(core_tui.master);
  REQUIRE(WIFEXITED(status));
  const auto profile_after_use = parsed(server.cli(
      {"profile", "show", parsed(profile).at("id").get<std::string>()}));
  CHECK(profile_after_use.at("core_id") == parsed(core).at("id"));
  REQUIRE(server.stop());
}

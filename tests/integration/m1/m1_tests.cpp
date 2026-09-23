#include <catch2/catch_test_macros.hpp>

#include "model/common.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {
using namespace xrm;

struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-m1-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
struct Captured { int code = -1; std::string out; std::string err; bool timed_out = false; };
std::string read_file(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
Captured run(const std::filesystem::path& executable, const std::filesystem::path& root,
             const std::vector<std::string>& arguments) {
  static unsigned sequence = 0;
  const auto out_path = root / ("out-" + std::to_string(++sequence));
  const auto err_path = root / ("err-" + std::to_string(sequence));
  std::vector<std::string> storage{executable.string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& value : storage) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) return {};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
                               read_file(out_path), read_file(err_path), false};
    if (result < 0) break;
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path), true};
}
class Server {
 public:
  explicit Server(TempDirectory& temp) : temp_(&temp), socket_(temp.path / "xrmd.sock"), db_(temp.path / "state.sqlite") {}
  bool start() {
    std::vector<std::string> values{XRM_TEST_SERVER, "--foreground", "--socket", socket_.string(), "--database", db_.string()};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    std::array<char*, 1> environment{nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, (temp_->path / "xrmd.stderr").c_str(),
                                     O_CREAT | O_TRUNC | O_WRONLY, 0600);
    const int status = posix_spawn(&pid_, XRM_TEST_SERVER, &actions, nullptr, argv.data(), environment.data());
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      const auto value = socket_.string();
      std::copy(value.begin(), value.end(), address.sun_path);
      if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) { ::close(fd); return true; }
      ::close(fd);
      ::poll(nullptr, 0, 10);
    }
    return false;
  }
  Captured cli(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), {"--socket", socket_.string(), "--json"});
    return run(XRM_TEST_MANAGER, temp_->path, arguments);
  }
  bool stop() {
    if (pid_ < 0) return true;
    ::kill(pid_, SIGTERM);
    int status = 0;
    for (int attempt = 0; attempt < 500; ++attempt) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return WIFEXITED(status); }
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return false;
  }
  int observed_exit() {
    if (pid_ < 0) return -1;
    int status = 0;
    const auto waited = ::waitpid(pid_, &status, WNOHANG);
    if (waited != pid_) return -1;
    pid_ = -1;
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
  }
  ~Server() { if (pid_ >= 0) stop(); }
 private:
  TempDirectory* temp_;
  std::filesystem::path socket_;
  std::filesystem::path db_;
  pid_t pid_ = -1;
};
Json parsed(const Captured& result) {
  if (result.code != 0 || result.timed_out) return {};
  try { return Json::parse(result.out); } catch (...) { return {}; }
}
struct TuiProcess { pid_t pid = -1; int master = -1; };
TuiProcess start_tui(const std::filesystem::path& root, const std::filesystem::path& socket,
                     const std::string& profile) {
  int master = -1, slave = -1;
  if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return {};
  winsize size{40, 140, 0, 0};
  ::ioctl(slave, TIOCSWINSZ, &size);
  std::vector<std::string> values{XRM_TEST_MANAGER, "--socket", socket.string(), "tui", "--profile", profile};
  std::vector<char*> argv;
  for (auto& value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  pid_t pid = -1;
  const int status = posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(slave);
  if (status != 0) { ::close(master); return {}; }
  (void)root;
  return {pid, master};
}
std::string read_tui(int master, std::chrono::milliseconds duration) {
  std::string captured;
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{master, POLLIN, 0};
    if (::poll(&descriptor, 1, 20) <= 0) continue;
    char bytes[4096];
    const auto count = ::read(master, bytes, sizeof(bytes));
    if (count > 0) captured.append(bytes, static_cast<std::size_t>(count));
  }
  return captured;
}
void send_tui(int master, std::string_view key) {
  (void)::write(master, key.data(), key.size());
  ::poll(nullptr, 0, 250);
}
}  // namespace

TEST_CASE("E2E-02 manual node through Profile, modules, connect, logs, and stop", "[E2E-02][CLI-02][RPC-03]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli({"core", "register", XRM_TEST_FAKE_CORE, "--name", "loopback fixture"});
  REQUIRE(core.code == 0);
  const auto core_id = parsed(core).at("id").get<std::string>();
  auto created = server.cli({"profile", "create", "--name", "local-test", "--core", core_id});
  REQUIRE(created.code == 0);
  const auto profile_id = parsed(created).at("id").get<std::string>();
  auto node = server.cli({"node", "add", "--name", "manual-direct", "--outbound-json",
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
  REQUIRE(std::any_of(rendered_json.at("files").begin(), rendered_json.at("files").end(),
                      [](const auto& file) { return file == "90_nodes_tail.json"; }));
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
  auto unknown_operation = server.cli({"operation", "get", "unknown-from-this-boot"});
  CHECK(unknown_operation.code != 0);
  REQUIRE(server.stop());
}

TEST_CASE("UI-04 TUI module page renders all native module keys", "[UI-04]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli({"core", "register", XRM_TEST_FAKE_CORE, "--name", "tui fixture"});
  REQUIRE(core.code == 0);
  auto profile = server.cli({"profile", "create", "--name", "tui-profile", "--core",
                             parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);

  int master = -1;
  int slave = -1;
  REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
  winsize size{40, 140, 0, 0};
  REQUIRE(::ioctl(slave, TIOCSWINSZ, &size) == 0);
  std::vector<std::string> arguments{XRM_TEST_MANAGER, "--socket", (temp.path / "xrmd.sock").string(),
      "tui", "--profile", parsed(profile).at("id").get<std::string>()};
  std::vector<char*> argv;
  for (auto& value : arguments) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(slave);
  REQUIRE(spawned == 0);
  ::poll(nullptr, 0, 500);
  REQUIRE(::write(master, "3", 1) == 1);
  ::poll(nullptr, 0, 500);
  REQUIRE(::write(master, "q", 1) == 1);
  std::string screen;
  int child_status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    char bytes[4096];
    const auto count = ::read(master, bytes, sizeof(bytes));
    if (count > 0) screen.append(bytes, static_cast<std::size_t>(count));
    const auto waited = ::waitpid(pid, &child_status, WNOHANG);
    if (waited == pid) break;
    ::poll(nullptr, 0, 10);
  }
  ::close(master);
  if (::waitpid(pid, &child_status, WNOHANG) == 0) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &child_status, 0);
  }
  REQUIRE(WIFEXITED(child_status));
  REQUIRE(WEXITSTATUS(child_status) == 0);
  for (const auto* key : {"env", "log", "api", "dns", "routing", "policy", "inbounds",
                           "outbounds", "stats", "fakedns", "metrics", "observatory",
                           "burstObservatory", "geodata", "version"})
    CHECK(screen.find(key) != std::string::npos);
  REQUIRE(server.stop());
}

TEST_CASE("UI-01 text keys do not fire global actions and TUI exit leaves core running", "[UI-01]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto core = server.cli({"core", "register", XRM_TEST_FAKE_CORE, "--name", "ui action fixture"});
  REQUIRE(core.code == 0);
  auto profile = server.cli({"profile", "create", "--name", "ui-actions", "--core",
                             parsed(core).at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  auto node = server.cli({"node", "add", "--name", "manual-ui", "--outbound-json",
                          R"({"protocol":"freedom","settings":{}})"});
  REQUIRE(node.code == 0);

  auto tui = start_tui(temp.path, temp.path / "xrmd.sock", parsed(profile).at("id").get<std::string>());
  REQUIRE(tui.pid > 0);
  REQUIRE(tui.master >= 0);
  (void)read_tui(tui.master, std::chrono::milliseconds(300));
  send_tui(tui.master, "3");
  send_tui(tui.master, "\r");
  send_tui(tui.master, "qe ");
  int status = 0;
  CHECK(::waitpid(tui.pid, &status, WNOHANG) == 0);
  send_tui(tui.master, "\x11");
  send_tui(tui.master, "2");
  send_tui(tui.master, "\r");
  const auto details = read_tui(tui.master, std::chrono::milliseconds(300));
  CHECK(details.find("manual-ui") != std::string::npos);
  send_tui(tui.master, "\x11");
  send_tui(tui.master, "s");
  send_tui(tui.master, "c");
  const auto running_screen = read_tui(tui.master, std::chrono::seconds(3));
  CHECK(running_screen.find("connected") != std::string::npos);
  CHECK(::waitpid(tui.pid, &status, WNOHANG) == 0);
  send_tui(tui.master, "q");
  for (int attempt = 0; attempt < 100 && ::waitpid(tui.pid, &status, WNOHANG) == 0; ++attempt)
    ::poll(nullptr, 0, 20);
  if (::waitpid(tui.pid, &status, WNOHANG) == 0) {
    ::kill(tui.pid, SIGKILL);
    ::waitpid(tui.pid, &status, 0);
  }
  ::close(tui.master);
  REQUIRE(WIFEXITED(status));
  const auto modules = parsed(server.cli({"config", "modules", parsed(profile).at("id").get<std::string>()}));
  const auto env = std::find_if(modules.begin(), modules.end(), [](const auto& item) { return item.value("key", "") == "env"; });
  REQUIRE(env != modules.end());
  CHECK_FALSE(env->value("included", true));
  auto runtime = server.cli({"status"});
  REQUIRE(runtime.code == 0);
  CHECK(parsed(runtime).at("state") == "running");
  CHECK(parsed(runtime).at("active").at("profile_id") == parsed(profile).at("id"));
  const auto stopped = server.cli({"disconnect"});
  INFO(stopped.err);
  INFO(server.cli({"server", "info"}).err);
  INFO("server exit status: " << server.observed_exit());
  REQUIRE(stopped.code == 0);
  REQUIRE(server.stop());
}

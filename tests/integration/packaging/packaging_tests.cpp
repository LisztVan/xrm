#include <catch2/catch_test_macros.hpp>

#include "model/common.hpp"

#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern char** environ;

namespace {
using namespace std::chrono_literals;

struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-packaging-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string> child_environment(const std::vector<std::string>& overrides = {}) {
  std::vector<std::string> values;
  for (char** item = environ; item && *item; ++item) {
    const std::string entry(*item);
    const auto separator = entry.find('=');
    const auto name = entry.substr(0, separator);
    if (std::none_of(overrides.begin(), overrides.end(), [&](const auto& replacement) {
          return replacement.substr(0, replacement.find('=')) == name;
        })) values.push_back(entry);
  }
  values.insert(values.end(), overrides.begin(), overrides.end());
  return values;
}

struct Child {
  pid_t pid = -1;
  Child() = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  ~Child() { stop(); }

  bool spawn(const std::filesystem::path& executable,
             const std::vector<std::string>& arguments,
             const std::vector<std::string>& env = {}) {
    std::vector<std::string> storage{executable.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);
    auto environment = child_environment(env);
    std::vector<char*> envp;
    for (auto& value : environment) envp.push_back(value.data());
    envp.push_back(nullptr);
    return posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), envp.data()) == 0;
  }

  bool stop() {
    if (pid < 0) return true;
    if (::kill(pid, SIGTERM) != 0 && errno == ESRCH) { pid = -1; return true; }
    int status = 0;
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        pid = -1;
        return WIFEXITED(status) || (WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
      }
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    pid = -1;
    return false;
  }
};

struct CommandResult { int code = -1; std::string out; std::string err; };

CommandResult run_manager(const std::filesystem::path& temp,
                          const std::filesystem::path& socket,
                          std::vector<std::string> args) {
  static unsigned int sequence = 0;
  const auto output = temp / ("manager-" + std::to_string(++sequence) + ".out");
  const auto errors = temp / ("manager-" + std::to_string(sequence) + ".err");
  std::vector<std::string> storage{XRM_TEST_MANAGER, "--socket", socket.string(), "--json"};
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& value : storage) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
  REQUIRE(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output.c_str(),
      O_CREAT | O_TRUNC | O_WRONLY, 0600) == 0);
  REQUIRE(posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, errors.c_str(),
      O_CREAT | O_TRUNC | O_WRONLY, 0600) == 0);
  pid_t pid = -1;
  const auto spawn_status = posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_status != 0) return {};
  int status = 0;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(output), read_file(errors)};
    }
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(output), read_file(errors)};
}

bool socket_accepts(const std::filesystem::path& socket) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto name = socket.string();
  if (name.size() >= sizeof(address.sun_path)) { ::close(fd); return false; }
  std::copy(name.begin(), name.end(), address.sun_path);
  const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  ::close(fd);
  return connected;
}

bool process_gone(pid_t pid) {
  for (int attempt = 0; attempt < 300; ++attempt) {
    if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
    ::poll(nullptr, 0, 10);
  }
  return false;
}

xrm::Json parse_json(const CommandResult& result) {
  if (result.code != 0) return {};
  try { return xrm::Json::parse(result.out); } catch (...) { return {}; }
}
}  // namespace

TEST_CASE("PKG-01 systemd unit owns xrmd's cgroup and states service paths explicitly",
          "[PKG-01][packaging]") {
  const auto unit_path = std::filesystem::path(XRM_TEST_SOURCE_DIR) /
      "packaging/systemd/xrmd.service";
  const auto unit = read_file(unit_path);
  REQUIRE_FALSE(unit.empty());
  CHECK(unit.find("Type=simple") != std::string::npos);
  CHECK(unit.find("ExecStart=/usr/bin/xrmd --foreground --socket /run/xrm/xrmd.sock --database /var/lib/xrm/server.sqlite") != std::string::npos);
  CHECK(unit.find("StateDirectory=xrm") != std::string::npos);
  CHECK(unit.find("RuntimeDirectory=xrm") != std::string::npos);
  CHECK(unit.find("Restart=on-failure") != std::string::npos);
  CHECK(unit.find("RestartSec=") != std::string::npos);
  CHECK(unit.find("KillMode=control-group") != std::string::npos);
  CHECK(unit.find("TimeoutStopSec=") != std::string::npos);
  CHECK(unit.find("UMask=0077") != std::string::npos);
  CHECK(unit.find("xray.service") == std::string::npos);
}

TEST_CASE("LIFE-10 xrmd shutdown reaps only its managed Xray child, not an independent Xray",
          "[LIFE-10][PKG-01][packaging]") {
  TempDirectory temp;
  const auto socket = temp.path / "run" / "xrmd.sock";
  const auto database = temp.path / "state" / "server.sqlite";
  Child unrelated;
  REQUIRE(unrelated.spawn(XRM_TEST_FAKE_CORE, {"run", "-confdir", "/dev/null"},
                          {"XRM_FAKE_CORE_RUN_HANG=1"}));
  const auto unrelated_pid = unrelated.pid;

  Child daemon;
  REQUIRE(daemon.spawn(XRM_TEST_SERVER,
      {"--foreground", "--socket", socket.string(), "--database", database.string()},
      {"XRM_FAKE_CORE_RUN_HANG=1"}));
  bool listening = false;
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (socket_accepts(socket)) { listening = true; break; }
    int status = 0;
    if (::waitpid(daemon.pid, &status, WNOHANG) == daemon.pid) { daemon.pid = -1; break; }
    ::poll(nullptr, 0, 10);
  }
  REQUIRE(listening);

  const auto registered = run_manager(temp.path, socket,
      {"core", "register", XRM_TEST_FAKE_CORE, "--name", "owned-child-fixture"});
  REQUIRE(registered.code == 0);
  const auto core = parse_json(registered);
  REQUIRE(core.contains("id"));
  const auto profile = run_manager(temp.path, socket,
      {"profile", "create", "--name", "systemd-cleanup", "--core", core.at("id").get<std::string>()});
  REQUIRE(profile.code == 0);
  const auto profile_id = parse_json(profile).at("id").get<std::string>();
  const auto node = run_manager(temp.path, socket, {"node", "add", "--name", "direct",
      "--outbound-json", R"({"protocol":"freedom","settings":{},"tag":"direct"})"});
  REQUIRE(node.code == 0);
  const auto node_id = parse_json(node).at("id").get<std::string>();
  REQUIRE(run_manager(temp.path, socket, {"select", "node", profile_id, node_id}).code == 0);
  const auto connected = run_manager(temp.path, socket, {"connect", profile_id});
  REQUIRE(connected.code == 0);
  const auto status = run_manager(temp.path, socket, {"status"});
  REQUIRE(status.code == 0);
  const auto daemon_status = parse_json(status);
  REQUIRE(daemon_status.at("state") == "running");
  REQUIRE(daemon_status.at("pid").is_number_integer());
  const auto managed_pid = static_cast<pid_t>(daemon_status.at("pid").get<int>());
  REQUIRE(managed_pid > 0);
  CHECK(::kill(managed_pid, 0) == 0);

  REQUIRE(daemon.stop());
  CHECK(process_gone(managed_pid));
  CHECK_FALSE(socket_accepts(socket));
  CHECK(::kill(unrelated_pid, 0) == 0);
  CHECK(unrelated.stop());
}

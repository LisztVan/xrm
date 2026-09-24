#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "model/common.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {
using xrm::Json;
using namespace std::chrono_literals;

struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-e2e06-" + std::to_string(
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

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Captured run(const std::filesystem::path& executable,
             const std::filesystem::path& root,
             const std::vector<std::string>& arguments,
             std::chrono::seconds timeout = 20s) {
  static unsigned sequence = 0;
  const auto suffix = std::to_string(++sequence);
  const auto out_path = root / ("client-" + suffix + ".out");
  const auto err_path = root / ("client-" + suffix + ".err");
  std::vector<std::string> storage{executable.string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& value : storage) argv.push_back(value.data());
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
  if (spawned != 0) return {};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(out_path), read_file(err_path), false};
    }
    if (waited < 0) break;
    (void)::poll(nullptr, 0, 10);
  }
  (void)::kill(pid, SIGKILL);
  (void)::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path), true};
}

std::filesystem::path configured_xray() {
  const char* value = std::getenv("XRM_TEST_XRAY");
  if (value == nullptr || *value == '\0') return {};
  return value;
}

Json parsed(const Captured& result) {
  if (result.code != 0 || result.timed_out) return {};
  try {
    return Json::parse(result.out);
  } catch (...) {
    return {};
  }
}

std::map<std::string, std::string> directory_bytes(const std::filesystem::path& path) {
  std::map<std::string, std::string> result;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (entry.is_regular_file())
      result.emplace(entry.path().filename().string(), read_file(entry.path()));
  }
  return result;
}

class SubscriptionFixture {
 public:
  SubscriptionFixture() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return;
    int enabled = 1;
    (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(fd_, 8) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/nodes";
    thread_ = std::thread([this] { serve(); });
  }
  ~SubscriptionFixture() {
    running_ = false;
    if (fd_ >= 0) {
      (void)::shutdown(fd_, SHUT_RDWR);
      (void)::close(fd_);
      fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
  }
  bool valid() const { return fd_ >= 0; }
  const std::string& url() const { return url_; }
  std::size_t requests() const { return requests_.load(); }
  void set(std::string body) {
    std::lock_guard lock(mutex_);
    body_ = std::move(body);
  }

 private:
  void serve() {
    while (running_) {
      pollfd ready{fd_, POLLIN, 0};
      if (::poll(&ready, 1, 100) <= 0) continue;
      const int peer = ::accept(fd_, nullptr, nullptr);
      if (peer < 0) continue;
      ++requests_;
      char request[2048];
      (void)::recv(peer, request, sizeof(request), 0);
      std::string body;
      {
        std::lock_guard lock(mutex_);
        body = body_;
      }
      const auto response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
          "Connection: close\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\n\r\n" + body;
      std::size_t sent = 0;
      while (sent < response.size()) {
        const auto count = ::send(peer, response.data() + sent,
                                  response.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
      }
      ::close(peer);
    }
  }

  int fd_ = -1;
  std::atomic<bool> running_{true};
  std::atomic<std::size_t> requests_{0};
  std::string url_;
  std::string body_;
  std::mutex mutex_;
  std::thread thread_;
};

class Server {
 public:
  explicit Server(TempDirectory& temp)
      : root_(temp.path), socket_(root_ / "xrmd.sock"), database_(root_ / "state.sqlite") {}
  bool start() {
    std::vector<std::string> values{XRM_TEST_SERVER, "--foreground", "--socket",
                                    socket_.string(), "--database", database_.string()};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    if (posix_spawn(&pid_, XRM_TEST_SERVER, nullptr, nullptr, argv.data(), environ) != 0)
      return false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) return false;
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      const auto socket_path = socket_.string();
      if (socket_path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return false;
      }
      std::copy(socket_path.begin(), socket_path.end(), address.sun_path);
      const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
      ::close(fd);
      if (connected) return true;
      (void)::poll(nullptr, 0, 10);
    }
    return false;
  }
  Captured cli(std::vector<std::string> arguments) const {
    arguments.insert(arguments.begin(), {"--socket", socket_.string(), "--json"});
    return run(XRM_TEST_MANAGER, root_, arguments);
  }
  Captured cli_text(std::vector<std::string> arguments) const {
    arguments.insert(arguments.begin(), {"--socket", socket_.string()});
    return run(XRM_TEST_MANAGER, root_, arguments);
  }
  const std::filesystem::path& socket_path() const { return socket_; }
  const std::filesystem::path& database_path() const { return database_; }
  bool stop() {
    if (pid_ < 0) return true;
    (void)::kill(pid_, SIGTERM);
    int status = 0;
    for (int attempt = 0; attempt < 500; ++attempt) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return WIFEXITED(status);
      }
      (void)::poll(nullptr, 0, 10);
    }
    (void)::kill(pid_, SIGKILL);
    (void)::waitpid(pid_, &status, 0);
    pid_ = -1;
    return false;
  }
  ~Server() { (void)stop(); }

 private:
  std::filesystem::path root_;
  std::filesystem::path socket_;
  std::filesystem::path database_;
  pid_t pid_ = -1;
};

struct TuiProcess {
  pid_t pid = -1;
  int master = -1;
};

TuiProcess start_tui(const std::filesystem::path& socket, const std::string& profile) {
  int master = -1;
  int slave = -1;
  if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return {};
  termios terminal{};
  if (::tcgetattr(slave, &terminal) == 0) {
    terminal.c_iflag &= static_cast<tcflag_t>(~IXON);
    (void)::tcsetattr(slave, TCSANOW, &terminal);
  }
  winsize size{24, 80, 0, 0};
  (void)::ioctl(slave, TIOCSWINSZ, &size);
  std::vector<std::string> storage{XRM_TEST_MANAGER, "--socket", socket.string(),
                                   "tui", "--profile", profile};
  std::vector<char*> argv;
  for (auto& value : storage) argv.push_back(value.data());
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
  if (status != 0) {
    ::close(master);
    return {};
  }
  return {pid, master};
}

std::string drain_tui(int master, std::chrono::milliseconds duration) {
  std::string content;
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd ready{master, POLLIN, 0};
    if (::poll(&ready, 1, 20) <= 0) continue;
    char buffer[8192];
    const auto count = ::read(master, buffer, sizeof(buffer));
    if (count > 0) content.append(buffer, static_cast<std::size_t>(count));
  }
  return content;
}

bool write_tui(int master, std::string_view bytes) {
  return ::write(master, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
}

bool stop_tui(TuiProcess& tui) {
  if (tui.pid < 0) return true;
  (void)write_tui(tui.master, "q");
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::waitpid(tui.pid, &status, WNOHANG) == tui.pid) {
      tui.pid = -1;
      ::close(tui.master);
      tui.master = -1;
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    (void)drain_tui(tui.master, 20ms);
  }
  (void)::kill(tui.pid, SIGKILL);
  (void)::waitpid(tui.pid, &status, 0);
  tui.pid = -1;
  ::close(tui.master);
  tui.master = -1;
  return false;
}

struct TuiGuard {
  TuiProcess& tui;
  ~TuiGuard() {
    if (tui.pid >= 0) (void)stop_tui(tui);
    else if (tui.master >= 0) {
      ::close(tui.master);
      tui.master = -1;
    }
  }
};

std::string vless_link(std::string_view uuid, std::string_view name) {
  return "vless://" + std::string(uuid) + "@127.0.0.1:9?encryption=none#" +
         std::string(name);
}

}  // namespace

int main(int argc, char** argv) {
  const auto xray = configured_xray();
  if (xray.empty() || !xray.is_absolute() || !std::filesystem::is_regular_file(xray) ||
      ::access(xray.c_str(), X_OK) != 0) {
    std::cerr << "XRM_TEST_XRAY must point to the fixed v26.9.9 executable; skipping E2E-06\n";
    return 77;
  }
  const auto version = run(xray, std::filesystem::temp_directory_path(), {"version"});
  std::cout << "E2E-06 core: " << xray << "\n"
            << "Command: \"" << xray.string() << "\" version\n"
            << version.out << version.err;
  if (version.code != 0 || version.timed_out || version.out.find("26.9.9") == std::string::npos ||
      version.out.find("52a412d") == std::string::npos) {
    std::cerr << "E2E-06 requires Xray 26.9.9 commit 52a412d\n";
    return 1;
  }
  return Catch::Session().run(argc, argv);
}

TEST_CASE("E2E-06 two Manager clients complete the inventory-to-running lifecycle",
          "[E2E-06][real_core]") {
  const auto xray = configured_xray();
  TempDirectory temp;
  SubscriptionFixture subscription;
  REQUIRE(subscription.valid());
  subscription.set(vless_link("11111111-1111-4111-8111-111111111111", "Subscription A"));

  Server server(temp);
  REQUIRE(server.start());
  const auto core = parsed(server.cli({"core", "register", xray.string(), "--name", "fixed v26.9.9"}));
  INFO("core register: " << core.dump());
  REQUIRE(core.contains("id"));
  const auto profile = parsed(server.cli({"profile", "create", "--name", "release-e2e",
      "--core", core.at("id").get<std::string>()}));
  REQUIRE(profile.contains("id"));
  const auto profile_id = profile.at("id").get<std::string>();

  const auto source = parsed(server.cli({"sub", "add", "--name", "release fixture",
                                         "--url", subscription.url()}));
  INFO("subscription add: " << source.dump());
  REQUIRE(source.contains("id"));
  const auto source_id = source.at("id").get<std::string>();
  const auto first_update = server.cli({"sub", "update", source_id});
  INFO("subscription update A: " << first_update.out << first_update.err);
  REQUIRE(first_update.code == 0);
  REQUIRE(subscription.requests() == 1);
  const auto nodes_a = parsed(server.cli({"node", "list"})).at("items");
  REQUIRE(nodes_a.size() == 1);
  const auto node_a_id = nodes_a.at(0).at("id").get<std::string>();

  // Client B remains attached to the same server while CLI client A continues to issue RPC.
  auto tui = start_tui(server.socket_path(), profile_id);
  TuiGuard tui_guard{tui};
  REQUIRE(tui.pid > 0);
  REQUIRE(tui.master >= 0);
  const auto initial_screen = drain_tui(tui.master, 700ms);
  INFO("TUI initial frame bytes=" << initial_screen.size());
  REQUIRE(::kill(tui.pid, 0) == 0);

  auto log_edit = server.cli({"config", "log", "level", profile_id, "error"});
  INFO("native module edit: " << log_edit.out << log_edit.err);
  REQUIRE(log_edit.code == 0);
  const auto saved_log = parsed(server.cli({"config", "show", profile_id, "--module-key", "log"}));
  REQUIRE(saved_log.contains("text"));
  CHECK(Json::parse(saved_log.at("text").get<std::string>()).at("log").at("loglevel") == "error");
  const auto group = parsed(server.cli({"group", "add", profile_id, "--name", "release-manual",
      "--tag", "release-manual", "--mode", "manual", "--members", node_a_id,
      "--strategy", "roundRobin"}));
  INFO("group add: " << group.dump());
  REQUIRE(group.contains("id"));
  const auto group_id = group.at("id").get<std::string>();
  REQUIRE(server.cli({"group", "select", profile_id, group_id, node_a_id}).code == 0);
  REQUIRE(server.cli({"select", "group", profile_id, group_id}).code == 0);
  const auto rendered_a = parsed(server.cli({"config", "render", profile_id}));
  INFO("render A: " << rendered_a.dump());
  REQUIRE(rendered_a.contains("confdir"));
  const auto preview_a = std::filesystem::path(rendered_a.at("confdir").get<std::string>());
  INFO("routing A: " << read_file(preview_a / "04_routing.json"));
  INFO("outbounds A: " << read_file(preview_a / "03_outbounds.json"));
  INFO("tail A: " << read_file(preview_a / "90_nodes_tail.json"));
  const auto check_a = run(xray, temp.path,
      {"run", "-test", "-confdir", preview_a.string()});
  INFO("direct xray run -test -confdir stdout: " << check_a.out);
  INFO("direct xray run -test -confdir stderr: " << check_a.err);
  CHECK(check_a.code == 0);
  const auto connect_a = server.cli({"connect", profile_id});
  INFO("connect A: " << connect_a.out << connect_a.err);
  REQUIRE(connect_a.code == 0);

  const auto active_a = parsed(server.cli({"status"}));
  INFO("running status A: " << active_a.dump());
  REQUIRE(active_a.at("state") == "running");
  const auto pid_a = active_a.at("pid");
  const auto confdir_a = active_a.at("active").at("confdir").get<std::string>();
  const auto runtime_a = directory_bytes(confdir_a);
  REQUIRE_FALSE(runtime_a.empty());
  CHECK(std::filesystem::exists(server.database_path()));
  CHECK_FALSE(std::filesystem::exists(temp.path / "manager.sqlite"));
  CHECK(server.cli({"group", "list", profile_id}).code == 0);
  CHECK(server.cli({"logs"}).code == 0);
  const auto stats = server.cli({"stats"});
  CHECK(stats.code == 0);
  CHECK((stats.out.find("unavailable") != std::string::npos ||
         stats.out.find("available") != std::string::npos));

  // Refresh changes only the inventory. The live process and its file bytes are held as evidence.
  subscription.set(vless_link("22222222-2222-4222-8222-222222222222", "Subscription B"));
  const auto refresh_b = server.cli({"sub", "update", source_id});
  INFO("subscription update B: " << refresh_b.out << refresh_b.err);
  REQUIRE(refresh_b.code == 0);
  REQUIRE(subscription.requests() == 2);
  const auto nodes_b = parsed(server.cli({"node", "list"})).at("items");
  REQUIRE(nodes_b.size() == 2);
  const auto node_b = std::find_if(nodes_b.begin(), nodes_b.end(), [](const Json& item) {
    return item.value("name", "") == "Subscription B" && !item.value("source_missing", false);
  });
  REQUIRE(node_b != nodes_b.end());
  const auto node_b_id = node_b->at("id").get<std::string>();
  const auto unchanged = parsed(server.cli({"status"}));
  CHECK(unchanged.at("pid") == pid_a);
  CHECK(unchanged.at("active").at("confdir") == confdir_a);
  CHECK(directory_bytes(confdir_a) == runtime_a);
  const auto saved_group = parsed(server.cli({"group", "show", profile_id, group_id}));
  CHECK(saved_group.at("selected_node_id") == node_a_id);

  // Only an explicit user choice and connect may publish the newly refreshed node.
  REQUIRE(server.cli({"select", "node", profile_id, node_b_id}).code == 0);
  const auto connect_b = server.cli({"connect", profile_id});
  INFO("explicit connect B: " << connect_b.out << connect_b.err);
  REQUIRE(connect_b.code == 0);
  const auto active_b = parsed(server.cli({"status"}));
  REQUIRE(active_b.at("state") == "running");
  CHECK(active_b.at("pid") != pid_a);
  CHECK(active_b.at("saved_target").at("id_or_tag") == node_b_id);
  CHECK(active_b.at("actual_target").at("id_or_tag") == node_b_id);
  const auto confdir_b = std::filesystem::path(active_b.at("active").at("confdir").get<std::string>());
  const auto runtime_b = directory_bytes(confdir_b);
  REQUIRE(runtime_b.contains("90_nodes_tail.json"));
  CHECK(runtime_b.at("90_nodes_tail.json").find("22222222-2222-4222-8222-222222222222") !=
        std::string::npos);

  // Closing the interactive Manager is not a proxy for stopping the Server-owned Xray.
  CHECK(stop_tui(tui));
  const auto after_tui_exit = parsed(server.cli({"status"}));
  CHECK(after_tui_exit.at("state") == "running");
  CHECK(after_tui_exit.at("pid") == active_b.at("pid"));
  REQUIRE(server.cli({"disconnect"}).code == 0);
  CHECK(parsed(server.cli({"status"})).at("state") == "stopped");

  // Reconnect a new Manager after a Server restart; durable profile/node/source state is shared.
  REQUIRE(server.stop());
  REQUIRE(server.start());
  const auto recovered_status = parsed(server.cli({"status"}));
  CHECK(recovered_status.at("state") == "stopped");
  CHECK(parsed(server.cli({"profile", "show", profile_id})).at("selected").at("id_or_tag") == node_b_id);
  const auto recovered_nodes = parsed(server.cli({"node", "list"})).at("items");
  CHECK(recovered_nodes.size() == 2);
  CHECK(parsed(server.cli({"sub", "list"})).size() == 1);
  CHECK(subscription.requests() == 2);  // No write replay or auto-refresh on reconnect.
  CHECK(std::filesystem::exists(server.database_path()));
  auto reconnected_tui = start_tui(server.socket_path(), profile_id);
  TuiGuard reconnected_tui_guard{reconnected_tui};
  REQUIRE(reconnected_tui.pid > 0);
  CHECK(::kill(reconnected_tui.pid, 0) == 0);
  CHECK(stop_tui(reconnected_tui));
  CHECK(subscription.requests() == 2);
  REQUIRE(server.stop());
}

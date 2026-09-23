#include <catch2/catch_test_macros.hpp>

#include "model/common.hpp"

#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <chrono>
#include <array>
#include <algorithm>
#include <atomic>
#include <thread>
#include <map>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern char** environ;

namespace {
struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-m2-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
struct Captured { int code = -1; std::string out; std::string err; };
std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
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
  const auto spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) return {};
  int status = 0;
  for (int attempt = 0; attempt < 1500; ++attempt) {
    if (::waitpid(pid, &status, WNOHANG) == pid)
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), read_file(out_path), read_file(err_path)};
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path)};
}
class Server {
 public:
  explicit Server(TempDirectory& temp) : root_(temp.path), socket_(root_ / "xrmd.sock") {}
  bool start() {
    std::vector<std::string> values{XRM_TEST_SERVER, "--foreground", "--socket", socket_.string(),
                                    "--database", (root_ / "state.sqlite").string()};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    std::array<char*, 1> environment{nullptr};
    const auto status = posix_spawn(&pid_, XRM_TEST_SERVER, nullptr, nullptr, argv.data(), environ);
    if (status != 0) return false;
    for (int attempt = 0; attempt < 500; ++attempt) {
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
  Captured cli(std::vector<std::string> args) {
    args.insert(args.begin(), {"--socket", socket_.string(), "--json"});
    return run(XRM_TEST_MANAGER, root_, args);
  }
  const std::filesystem::path& socket_path() const { return socket_; }
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
  ~Server() {
    (void)stop();
  }
 private:
  std::filesystem::path root_;
  std::filesystem::path socket_;
  pid_t pid_ = -1;
};
xrm::Json parsed(const Captured& result) {
  if (result.code != 0) return {};
  try { return xrm::Json::parse(result.out); } catch (...) { return {}; }
}

int send_rpc_without_reading(const std::filesystem::path& socket, const xrm::Json& request) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = socket.string();
  if (path.size() >= sizeof(address.sun_path)) { ::close(fd); return -1; }
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return -1; }
  const auto frame = request.dump() + "\n";
  const auto count = ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
  if (count != static_cast<ssize_t>(frame.size())) { ::close(fd); return -1; }
  return fd;
}

class SubscriptionHttpFixture {
 public:
  SubscriptionHttpFixture() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return;
    int enabled = 1;
    (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd_, 8) != 0) {
      ::close(fd_); fd_ = -1; return;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size) != 0) { ::close(fd_); fd_ = -1; return; }
    url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/nodes";
    thread_ = std::thread([this] { serve(); });
  }
  ~SubscriptionHttpFixture() {
    running_ = false;
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    if (thread_.joinable()) thread_.join();
  }
  bool valid() const { return fd_ >= 0; }
  const std::string& url() const { return url_; }
  std::size_t requests() const { return requests_.load(); }
  void set(std::string body, int status = 200) {
    std::lock_guard lock(mutex_);
    body_ = std::move(body);
    status_ = status;
  }
 private:
  void serve() {
    while (running_) {
      pollfd readiness{fd_, POLLIN, 0};
      if (::poll(&readiness, 1, 100) <= 0) continue;
      const int peer = ::accept(fd_, nullptr, nullptr);
      if (peer < 0) continue;
      ++requests_;
      char request[4096];
      (void)::recv(peer, request, sizeof(request), 0);
      std::string body;
      int status;
      {
        std::lock_guard lock(mutex_);
        body = body_;
        status = status_;
      }
      const auto reason = status == 200 ? "OK" : "Service Unavailable";
      const auto response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
          "Content-Type: text/plain; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
          std::to_string(body.size()) + "\r\n\r\n" + body;
      std::size_t sent = 0;
      while (sent < response.size()) {
        const auto count = ::send(peer, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
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
  std::string body_ = "";
  int status_ = 200;
  std::mutex mutex_;
  std::thread thread_;
};

std::map<std::string, std::string> directory_bytes(const std::filesystem::path& path) {
  std::map<std::string, std::string> files;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    if (!entry.is_regular_file()) continue;
    files.emplace(entry.path().filename().string(), read_file(entry.path()));
  }
  return files;
}

}  // namespace

TEST_CASE("CLI-01 subscription add/list/show/edit/remove and node ownership actions", "[CLI-01][DB-03]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto added = server.cli({"sub", "add", "--name", "fixture", "--url", "http://127.0.0.1:1/sub"});
  REQUIRE(added.code == 0);
  const auto source_id = parsed(added).at("id").get<std::string>();
  auto list = parsed(server.cli({"sub", "list"}));
  REQUIRE(list.is_array());
  REQUIRE(list.size() == 1);
  CHECK_FALSE(list[0].contains("url"));
  CHECK(list[0].at("name") == "fixture");
  CHECK(list[0].contains("last_attempt"));
  CHECK(list[0].contains("last_success"));
  CHECK(list[0].contains("unsupported_count"));
  CHECK(list[0].contains("malformed_count"));
  auto detail = parsed(server.cli({"sub", "show", source_id}));
  CHECK(detail.at("subscription").at("url") == "http://127.0.0.1:1/sub");
  auto edited = server.cli({"sub", "edit", source_id, "--name", "changed"});
  REQUIRE(edited.code == 0);
  auto shown = parsed(server.cli({"sub", "show", source_id}));
  CHECK(shown.at("subscription").at("name") == "changed");

  auto node = parsed(server.cli({"node", "add", "--name", "manual", "--outbound-json",
      R"({"protocol":"vless","settings":{"vnext":[{"address":"node.example","port":443,"users":[{"id":"secret-id","encryption":"none"}]}]}})"}));
  REQUIRE(node.contains("id"));
  auto copied = server.cli({"node", "copy", node.at("id").get<std::string>(), "--name", "manual copy"});
  REQUIRE(copied.code == 0);
  CHECK(parsed(copied).at("id") != node.at("id"));
  auto disabled = server.cli({"node", "disable", parsed(copied).at("id").get<std::string>()});
  REQUIRE(disabled.code == 0);
  CHECK(parsed(server.cli({"node", "show", parsed(copied).at("id").get<std::string>()}))
            .at("settings").dump().find("secret-id") != std::string::npos);
  REQUIRE(server.cli({"sub", "remove", source_id, "--keep-nodes"}).code == 0);
}

TEST_CASE("RPC-04 an accepted subscription refresh is queried, never replayed", "[RPC-04]") {
  TempDirectory temp;
  SubscriptionHttpFixture fixture;
  REQUIRE(fixture.valid());
  fixture.set("vless://33333333-3333-4333-8333-333333333333@rpc.invalid:443?encryption=none#RPC");
  Server server(temp);
  REQUIRE(server.start());
  auto source = parsed(server.cli({"sub", "add", "--name", "rpc fixture", "--url", fixture.url(), "--paused"}));
  REQUIRE(source.contains("id"));
  const auto info = parsed(server.cli({"server", "info"}));
  REQUIRE(info.contains("boot_id"));
  const auto request = xrm::Json{{"v", 1}, {"id", 71}, {"method", "subscription.refresh"},
      {"params", {{"source_ids", xrm::Json::array({source.at("id")})}}}};
  const int client_fd = send_rpc_without_reading(server.socket_path(), request);
  REQUIRE(client_fd >= 0);
  const auto request_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (fixture.requests() == 0 && std::chrono::steady_clock::now() < request_deadline) ::poll(nullptr, 0, 10);
  REQUIRE(fixture.requests() == 1);
  ::shutdown(client_fd, SHUT_RDWR);
  ::close(client_fd);
  const auto operation = info.at("boot_id").get<std::string>() + ":1";
  xrm::Json status;
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto result = parsed(server.cli({"operation", "get", operation}));
    if (result.is_object()) status = std::move(result);
    if (status.value("status", "") == "succeeded" || status.value("status", "") == "failed") break;
    ::poll(nullptr, 0, 20);
  }
  REQUIRE(status.value("status", "") == "succeeded");
  CHECK(fixture.requests() == 1);
  CHECK(parsed(server.cli({"node", "list"})).at("total") == 1);
  REQUIRE(server.stop());
}

TEST_CASE("TUI exposes separate Subscriptions and Nodes views with missing source state", "[UI-02]") {
  TempDirectory temp;
  Server server(temp);
  REQUIRE(server.start());
  auto added = server.cli({"sub", "add", "--name", "fixture", "--url", "http://127.0.0.1:1/sub"});
  REQUIRE(added.code == 0);
  CHECK(parsed(server.cli({"node", "list"})).at("total") == 0);
  int master = -1, slave = -1;
  REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
  winsize size{24, 80, 0, 0};
  REQUIRE(::ioctl(slave, TIOCSWINSZ, &size) == 0);
  std::vector<std::string> values{XRM_TEST_MANAGER, "--socket", (temp.path / "xrmd.sock").string(), "tui"};
  std::vector<char*> argv;
  for (auto& value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  pid_t tui = -1;
  const auto spawned = posix_spawn(&tui, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(slave);
  REQUIRE(spawned == 0);
  ::poll(nullptr, 0, 250);
  (void)::write(master, "5", 1);
  std::string screen;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
  while (std::chrono::steady_clock::now() < until) {
    pollfd fd{master, POLLIN, 0};
    if (::poll(&fd, 1, 20) > 0) { char bytes[2048]; const auto n = ::read(master, bytes, sizeof(bytes)); if (n > 0) screen.append(bytes, static_cast<std::size_t>(n)); }
  }
  (void)::write(master, "q", 1);
  int status = 0;
  for (int i = 0; i < 100 && ::waitpid(tui, &status, WNOHANG) == 0; ++i) ::poll(nullptr, 0, 10);
  if (::waitpid(tui, &status, WNOHANG) == 0) { ::kill(tui, SIGKILL); ::waitpid(tui, &status, 0); }
  ::close(master);
  REQUIRE(WIFEXITED(status));
  CHECK(screen.find("Subscriptions") != std::string::npos);
  CHECK(screen.find("fixture") != std::string::npos);
}

TEST_CASE("G2/E2E-03 refresh changes inventory only until an explicit connect", "[E2E-03][SUB-02][SUB-06][RPC-04]") {
  TempDirectory temp;
  SubscriptionHttpFixture fixture;
  REQUIRE(fixture.valid());
  const auto fake_core = temp.path / "fake-xray";
  {
    std::ofstream script(fake_core);
    script << "#!/bin/sh\n"
        << "if [ \"$1\" = run ]; then\n"
        << "  if [ \"$2\" = -test ]; then printf '%s\\n' \"$*\" >> '" << (temp.path / "check.argv").string() << "';\n"
        << "  else printf '%s\\n' \"$*\" >> '" << (temp.path / "run.argv").string() << "'; fi\n"
        << "fi\nexec '" << XRM_TEST_FAKE_CORE << "' \"$@\"\n";
  }
  REQUIRE(::chmod(fake_core.c_str(), 0700) == 0);
  Server server(temp);
  REQUIRE(server.start());
  const auto link = [](std::string uuid, std::string host, std::string name) {
    return "vless://" + uuid + "@" + host + ":443?encryption=none#" + name;
  };
  const auto uuid_a = "11111111-1111-4111-8111-111111111111";
  const auto uuid_b = "22222222-2222-4222-8222-222222222222";
  fixture.set(link(uuid_a, "a.invalid", "A"));
  auto core = parsed(server.cli({"core", "register", fake_core.string(), "--name", "G2 fake core"}));
  REQUIRE(core.contains("id"));
  auto profile = parsed(server.cli({"profile", "create", "--name", "G2 profile", "--core", core.at("id").get<std::string>()}));
  REQUIRE(profile.contains("id"));
  const auto profile_id = profile.at("id").get<std::string>();
  auto source = parsed(server.cli({"sub", "add", "--name", "loopback subscription", "--url", fixture.url()}));
  REQUIRE(source.contains("id"));
  const auto source_id = source.at("id").get<std::string>();
  REQUIRE(server.cli({"sub", "update", source_id}).code == 0);
  auto nodes_a = parsed(server.cli({"node", "list"})).at("items");
  REQUIRE(nodes_a.size() == 1);
  const auto id_a = nodes_a.front().at("id").get<std::string>();
  REQUIRE(server.cli({"node", "alias", id_a, "A preserved alias"}).code == 0);
  REQUIRE(server.cli({"node", "labels", id_a, "--values", "lab-one,lab-two"}).code == 0);
  REQUIRE(server.cli({"select", "node", profile_id, id_a}).code == 0);
  REQUIRE(server.cli({"connect", profile_id}).code == 0);
  auto active_before = parsed(server.cli({"status"}));
  REQUIRE(active_before.at("state") == "running");
  const auto pid_before = active_before.at("pid");
  const auto confdir_before = std::filesystem::path(active_before.at("active").at("confdir").get<std::string>());
  const auto files_before = directory_bytes(confdir_before);
  const auto check_before = read_file(temp.path / "check.argv");
  const auto run_before = read_file(temp.path / "run.argv");
  REQUIRE_FALSE(files_before.empty());
  REQUIRE_FALSE(check_before.empty());
  REQUIRE_FALSE(run_before.empty());

  fixture.set(link(uuid_b, "b.invalid", "B"));
  auto updated = parsed(server.cli({"sub", "update", source_id}));
  REQUIRE(updated.contains("completed"));
  REQUIRE(updated.at("failed").empty());
  auto nodes_b = parsed(server.cli({"node", "list"})).at("items");
  REQUIRE(nodes_b.size() == 2);
  const auto node_b = std::find_if(nodes_b.begin(), nodes_b.end(), [](const auto& node) {
    return node.value("name", "") == "B" && !node.value("source_missing", false);
  });
  REQUIRE(node_b != nodes_b.end());
  const auto id_b = node_b->at("id").get<std::string>();
  auto missing_a = std::find_if(nodes_b.begin(), nodes_b.end(), [&](const auto& node) { return node.at("id") == id_a; });
  REQUIRE(missing_a != nodes_b.end());
  CHECK(missing_a->at("source_missing") == true);
  CHECK(missing_a->at("name") == "A preserved alias");
  CHECK(missing_a->at("labels") == xrm::Json::array({"lab-one", "lab-two"}));
  auto active_after_refresh = parsed(server.cli({"status"}));
  CHECK(active_after_refresh.at("pid") == pid_before);
  CHECK(active_after_refresh.at("active").at("confdir") == confdir_before.string());
  CHECK(directory_bytes(confdir_before) == files_before);
  CHECK(read_file(temp.path / "check.argv") == check_before);
  CHECK(read_file(temp.path / "run.argv") == run_before);
  auto saved_profile = parsed(server.cli({"profile", "show", profile_id}));
  CHECK(saved_profile.at("selected").at("id_or_tag") == id_a);
  const auto old_tail = files_before.find("90_nodes_tail.json");
  REQUIRE(old_tail != files_before.end());
  CHECK(old_tail->second.find(uuid_a) != std::string::npos);

  auto stale_connect = server.cli({"connect", profile_id});
  CHECK(stale_connect.code != 0);
  CHECK(parsed(server.cli({"status"})).at("pid") == pid_before);
  auto copied = parsed(server.cli({"node", "copy", id_a, "--name", "A manual copy"}));
  REQUIRE(copied.contains("id"));
  CHECK(copied.at("id") != id_a);
  auto copied_detail = parsed(server.cli({"node", "show", copied.at("id").get<std::string>()}));
  CHECK(copied_detail.at("source_id").is_null());
  CHECK(copied_detail.at("settings").at("protocol").at("id") == uuid_a);

  REQUIRE(server.cli({"select", "node", profile_id, id_b}).code == 0);
  REQUIRE(server.cli({"connect", profile_id}).code == 0);
  auto active_after_connect = parsed(server.cli({"status"}));
  REQUIRE(active_after_connect.at("state") == "running");
  CHECK(active_after_connect.at("pid") != pid_before);
  const auto b_runtime = directory_bytes(active_after_connect.at("active").at("confdir").get<std::string>());
  const auto tail = b_runtime.find("90_nodes_tail.json");
  REQUIRE(tail != b_runtime.end());
  CHECK(tail->second.find(uuid_b) != std::string::npos);

  fixture.set("<html>partial listing</html>\n" + link(uuid_b, "b.invalid", "B") + "\nvmess://not-supported", 200);
  // HTML is intentionally a failed parse; the next valid partial response verifies the
  // accepted-node/partial-state behavior without allowing missing-node cleanup.
  auto failed_cmd = server.cli({"sub", "update", source_id});
  CHECK(failed_cmd.code == 4);
  auto failed = xrm::Json::parse(failed_cmd.out);
  REQUIRE(failed.contains("failed"));
  CHECK_FALSE(failed.at("failed").empty());
  CHECK(parsed(server.cli({"node", "list"})).at("total") == 3);

  fixture.set(link(uuid_b, "b.invalid", "B") + "\nvmess://unsupported-protocol");
  auto partial_cmd = server.cli({"sub", "update", source_id});
  CHECK(partial_cmd.code == 4);
  auto partial = xrm::Json::parse(partial_cmd.out);
  REQUIRE(partial.at("completed").size() == 1);
  CHECK(partial.at("completed")[0].at("partial") == true);
  const auto after_partial = parsed(server.cli({"node", "list"})).at("items");
  auto still_missing_a = std::find_if(after_partial.begin(), after_partial.end(), [&](const auto& node) { return node.at("id") == id_a; });
  REQUIRE(still_missing_a != after_partial.end());
  CHECK(still_missing_a->at("source_missing") == true);
  auto source_rows = parsed(server.cli({"sub", "list"}));
  CHECK(source_rows[0].at("last_result") == "partial");
  CHECK(source_rows[0].at("unsupported_count") == 1);

  fixture.set(link(uuid_a, "a.invalid", "A"));
  REQUIRE(server.cli({"sub", "update", source_id}).code == 0);
  auto missing_b = parsed(server.cli({"node", "list"})).at("items");
  auto b_row = std::find_if(missing_b.begin(), missing_b.end(), [&](const auto& node) { return node.at("id") == id_b; });
  REQUIRE(b_row != missing_b.end());
  CHECK(b_row->at("source_missing") == true);
  auto manual_after_refresh = parsed(server.cli({"node", "show", copied.at("id").get<std::string>()}));
  CHECK(manual_after_refresh.at("source_id").is_null());
  CHECK(manual_after_refresh.at("settings").at("protocol").at("id") == uuid_a);
  auto selected_missing = server.cli({"connect", profile_id});
  CHECK(selected_missing.code != 0);
  CHECK(parsed(server.cli({"status"})).at("state") == "running");
  REQUIRE(server.cli({"disconnect"}).code == 0);
  REQUIRE(server.stop());
}

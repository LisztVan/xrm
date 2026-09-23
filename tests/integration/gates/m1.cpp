#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/profile.hpp"
#include "xray/codecs/codec.hpp"
#include "xray/runtime_assembler.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;

namespace {
using namespace std::chrono_literals;
using namespace xrm;

std::filesystem::path real_xray() {
  if (const char* value = std::getenv("XRM_TEST_XRAY"); value && *value)
    return std::filesystem::path(value);
#ifdef XRM_TEST_XRAY_CONFIGURED
  return XRM_TEST_XRAY_CONFIGURED;
#else
  return {};
#endif
}

struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-real-core-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

struct Captured { int code = -1; std::string out; std::string err; bool timed_out = false; };

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Captured run_command(const std::filesystem::path& executable,
                     const std::vector<std::string>& arguments,
                     const std::filesystem::path& root,
                     std::chrono::seconds timeout = 20s) {
  static std::atomic<unsigned> sequence{0};
  const auto number = sequence.fetch_add(1);
  const auto out_path = root / ("command-" + std::to_string(number) + ".out");
  const auto err_path = root / ("command-" + std::to_string(number) + ".err");
  std::vector<std::string> values{executable.string()};
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) return {};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(out_path), read_file(err_path), false};
    if (waited < 0) break;
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path), true};
}

class OwnedProcess {
 public:
  bool start(const std::filesystem::path& executable,
             std::vector<std::string> arguments,
             const std::filesystem::path& root,
             bool inherit_environment = true) {
    executable_ = executable;
    static std::atomic<unsigned> sequence{0};
    const auto number = sequence.fetch_add(1);
    out_ = root / ("process-" + std::to_string(number) + ".out");
    err_ = root / ("process-" + std::to_string(number) + ".err");
    std::vector<std::string> values{executable.string()};
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    std::array<char*, 1> empty_environment{nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    const int spawned = posix_spawn(&pid_, executable.c_str(), &actions, nullptr, argv.data(),
                                    inherit_environment ? environ : empty_environment.data());
    posix_spawn_file_actions_destroy(&actions);
    return spawned == 0;
  }

  bool stop() {
    if (pid_ < 0) return true;
    ::kill(pid_, SIGTERM);
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
      }
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return false;
  }

  std::string stderr_text() const { return read_file(err_); }
  std::string stdout_text() const { return read_file(out_); }
  ~OwnedProcess() { (void)stop(); }
 private:
  std::filesystem::path executable_;
  std::filesystem::path out_;
  std::filesystem::path err_;
  pid_t pid_ = -1;
};

std::optional<std::uint16_t> free_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return std::nullopt;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  socklen_t size = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  const auto port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}

bool can_connect(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  timeval timeout{0, 200000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  ::close(fd);
  return connected;
}

bool wait_for_port(std::uint16_t port) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (can_connect(port)) return true;
    ::poll(nullptr, 0, 20);
  }
  return false;
}

class LoopbackHttp {
 public:
  bool start(std::string marker, int requests = 1) {
    marker_ = std::move(marker);
    remaining_ = requests;
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd_, 8) != 0) {
      ::close(fd_); fd_ = -1; return false;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
      ::close(fd_); fd_ = -1; return false;
    }
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] { serve(); });
    return true;
  }
  std::uint16_t port() const { return port_; }
  ~LoopbackHttp() {
    stopping_.store(true);
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    if (worker_.joinable()) worker_.join();
  }
 private:
  void serve() {
    while (!stopping_.load() && remaining_ > 0) {
      pollfd descriptor{fd_, POLLIN, 0};
      if (::poll(&descriptor, 1, 10000) <= 0) break;
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0) continue;
      timeval timeout{3, 0};
      ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      char request[2048];
      (void)::recv(client, request, sizeof(request), 0);
      const auto response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(marker_.size()) +
          "\r\nConnection: close\r\n\r\n" + marker_;
      (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
      --remaining_;
    }
  }
  int fd_ = -1;
  std::uint16_t port_ = 0;
  std::string marker_;
  int remaining_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
};

bool send_all(int fd, std::string_view bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const auto count = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
    if (count <= 0) return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

bool recv_exact(int fd, char* bytes, std::size_t size) {
  std::size_t received = 0;
  while (received < size) {
    const auto count = ::recv(fd, bytes + received, size - received, 0);
    if (count <= 0) return false;
    received += static_cast<std::size_t>(count);
  }
  return true;
}

std::string socks_http_request(std::uint16_t proxy_port, std::uint16_t destination_port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  timeval timeout{4, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in proxy{};
  proxy.sin_family = AF_INET;
  proxy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  proxy.sin_port = htons(proxy_port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&proxy), sizeof(proxy)) != 0) { ::close(fd); return {}; }
  const char hello[] = {5, 1, 0};
  char selection[2]{};
  if (!send_all(fd, std::string_view(hello, sizeof(hello))) || !recv_exact(fd, selection, sizeof(selection)) ||
      selection[0] != 5 || selection[1] != 0) { ::close(fd); return {}; }
  const std::array<unsigned char, 10> request{
      5, 1, 0, 1, 127, 0, 0, 1,
      static_cast<unsigned char>(destination_port >> 8), static_cast<unsigned char>(destination_port & 0xff)};
  if (!send_all(fd, std::string_view(reinterpret_cast<const char*>(request.data()), request.size()))) {
    ::close(fd); return {};
  }
  char reply[4]{};
  if (!recv_exact(fd, reply, sizeof(reply)) || reply[0] != 5 || reply[1] != 0) { ::close(fd); return {}; }
  std::size_t remainder = reply[3] == 1 ? 6 : reply[3] == 4 ? 18 : 0;
  if (reply[3] == 3) {
    unsigned char length = 0;
    if (!recv_exact(fd, reinterpret_cast<char*>(&length), 1)) { ::close(fd); return {}; }
    remainder = static_cast<std::size_t>(length) + 2;
  }
  std::array<char, 258> ignored{};
  if (remainder == 0 || !recv_exact(fd, ignored.data(), remainder)) { ::close(fd); return {}; }
  if (!send_all(fd, "GET /g1 HTTP/1.1\r\nHost: loopback\r\nConnection: close\r\n\r\n")) {
    ::close(fd); return {};
  }
  std::string response;
  std::array<char, 1024> chunk{};
  for (;;) {
    const auto count = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (count <= 0) break;
    response.append(chunk.data(), static_cast<std::size_t>(count));
  }
  ::close(fd);
  return response;
}

class XrmServer {
 public:
  explicit XrmServer(TempDirectory& temp) : root_(temp.path), socket_(temp.path / "xrmd.sock"), db_(temp.path / "state.sqlite") {}
  bool start() {
    std::vector<std::string> values{XRM_TEST_SERVER, "--foreground", "--socket", socket_.string(), "--database", db_.string()};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    std::array<char*, 1> environment{nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, (root_ / "xrmd.err").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    const int spawned = posix_spawn(&pid_, XRM_TEST_SERVER, &actions, nullptr, argv.data(), environment.data());
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      const auto path = socket_.string();
      std::copy(path.begin(), path.end(), address.sun_path);
      const bool ready = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
      ::close(fd);
      if (ready) return true;
      ::poll(nullptr, 0, 10);
    }
    return false;
  }
  Captured cli(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), {"--socket", socket_.string(), "--json"});
    return run_command(XRM_TEST_MANAGER, arguments, root_);
  }
  std::filesystem::path database() const { return db_; }
  Json json(const Captured& result) const {
    if (result.code != 0 || result.timed_out) return {};
    try { return Json::parse(result.out); } catch (...) { return {}; }
  }
  bool stop() {
    if (pid_ < 0) return true;
    ::kill(pid_, SIGTERM);
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) { pid_ = -1; return WIFEXITED(status); }
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return false;
  }
  ~XrmServer() { (void)stop(); }
 private:
  std::filesystem::path root_;
  std::filesystem::path socket_;
  std::filesystem::path db_;
  pid_t pid_ = -1;
};

Json inbound_config(std::uint16_t port) {
  return Json{{"inbounds", Json::array({Json{
      {"listen", "127.0.0.1"}, {"port", port}, {"protocol", "socks"},
      {"tag", "g1-socks"}, {"settings", Json{{"auth", "noauth"}, {"udp", false}}}}})}};
}

bool edit_module(XrmServer& server, const std::string& profile, const std::string& key, const Json& document) {
  auto result = server.cli({"config", "edit", profile, key, "--text", document.dump()});
  return result.code == 0;
}

std::optional<std::string> setup_xrm(XrmServer& server, const std::filesystem::path& xray) {
  auto core = server.cli({"core", "register", xray.string(), "--name", "G1 pinned real-core fixture"});
  if (core.code != 0) return std::nullopt;
  auto profile = server.cli({"profile", "create", "--name", "G1 loopback", "--core",
      server.json(core).value("id", "")});
  if (profile.code != 0) return std::nullopt;
  return server.json(profile).value("id", "");
}

Json routing_with_static_direct(std::uint16_t direct_port) {
  return Json{{"routing", Json{{"domainStrategy", "AsIs"}, {"rules", Json::array({Json{
      {"type", "field"}, {"inboundTag", Json::array({"g1-socks"})},
      {"port", std::to_string(direct_port)}, {"outboundTag", "direct"}}})}}}};
}

bool assert_xray_check(const std::filesystem::path& xray, const std::filesystem::path& dir,
                       const std::filesystem::path& logs) {
  auto checked = run_command(xray, {"run", "-test", "-confdir", dir.string()}, logs);
  INFO("xray check stderr: " << checked.err);
  return !checked.timed_out && checked.code == 0;
}

bool run_vless_flow(bool also_export) {
  const auto xray = real_xray();
  TempDirectory temp;
  const auto socks_port = free_loopback_port();
  const auto vless_port = free_loopback_port();
  REQUIRE(socks_port.has_value());
  REQUIRE(vless_port.has_value());
  REQUIRE(*socks_port != *vless_port);

  const auto uuid = std::string("00000000-0000-4000-8000-000000000001");
  const auto http_marker = also_export ? "XRM-G1-RC10-UNIQUE-3a91c8" : "XRM-G1-RC02-UNIQUE-87d20f";
  LoopbackHttp http;
  REQUIRE(http.start(http_marker, also_export ? 2 : 1));

  const auto vless_config = temp.path / "vless-server.json";
  const Json server_config{
      {"log", Json{{"loglevel", "warning"}}},
      {"inbounds", Json::array({Json{{"listen", "127.0.0.1"}, {"port", *vless_port},
          {"protocol", "vless"}, {"tag", "g1-vless-server"},
          {"settings", Json{{"clients", Json::array({Json{{"id", uuid}, {"email", "g1-local"}}})},
                             {"decryption", "none"}}}}})},
      {"outbounds", Json::array({Json{{"protocol", "freedom"}, {"tag", "direct"},
          {"settings", Json{{"finalRules", Json::array({Json{{"action", "allow"}, {"network", "tcp"},
              {"port", std::to_string(http.port())}, {"ip", Json::array({"127.0.0.0/8"})}}})}}}}})}};
  { std::ofstream output(vless_config); output << server_config.dump(2) << '\n'; }

  OwnedProcess vless_server;
  REQUIRE(vless_server.start(xray, {"run", "-c", vless_config.string()}, temp.path, false));
  REQUIRE(wait_for_port(*vless_port));

  XrmServer server(temp);
  REQUIRE(server.start());
  const auto profile = setup_xrm(server, xray);
  REQUIRE(profile.has_value());
  REQUIRE(edit_module(server, *profile, "inbounds", inbound_config(*socks_port)));

  const auto uri = "vless://" + uuid + "@127.0.0.1:" + std::to_string(*vless_port) +
                   "?encryption=none&type=tcp#G1-local-vless";
  auto decoded = parse_vless_uri(uri);
  REQUIRE(decoded.has_value());
  decoded->node.id = "g1-vless-node";
  INFO("decoded stream reaches M1 Node storage and RuntimeAssembler");
  auto store = Store::open(server.database());
  REQUIRE(store.has_value());
  auto stored = (*store)->put_node(decoded->node);
  if (!stored) INFO("typed-node store error: " << stored.error().message);
  REQUIRE(stored.has_value());
  auto reloaded = (*store)->get_node(decoded->node.id);
  REQUIRE(reloaded.has_value());
  REQUIRE(std::holds_alternative<StreamNode>(reloaded->settings));
  store->reset();

  auto selected = server.cli({"select", "node", *profile, decoded->node.id});
  REQUIRE(selected.code == 0);
  auto connected = server.cli({"connect", *profile});
  INFO(connected.err);
  REQUIRE(connected.code == 0);
  const auto status = server.json(server.cli({"status"}));
  REQUIRE(status.value("state", "") == "running");
  const auto confdir = std::filesystem::path(status.at("active").at("confdir").get<std::string>());
  REQUIRE(std::filesystem::exists(confdir / "03_outbounds.json"));
  REQUIRE(std::filesystem::exists(confdir / "90_nodes_tail.json"));
  REQUIRE_FALSE(std::filesystem::exists(confdir / "config.json"));
  const auto outbound = Json::parse(read_file(confdir / "90_nodes_tail.json"));
  REQUIRE(outbound.at("outbounds").at(0).at("protocol") == "vless");
  REQUIRE(outbound.at("outbounds").at(0).at("settings").at("vnext").at(0).at("address") == "127.0.0.1");

  const auto first_response = socks_http_request(*socks_port, http.port());
  INFO("first proxied response: " << first_response);
  INFO("managed core logs: " << server.cli({"logs"}).out);
  INFO("VLESS peer stdout: " << vless_server.stdout_text());
  INFO("VLESS peer stderr: " << vless_server.stderr_text());
  INFO("generated VLESS outbound: " << read_file(confdir / "90_nodes_tail.json"));
  CHECK(first_response.find(http_marker) != std::string::npos);
  if (!also_export) {
    auto disconnected = server.cli({"disconnect"});
    REQUIRE(disconnected.code == 0);
    REQUIRE(server.stop());
    REQUIRE(vless_server.stop());
    return true;
  }

  const auto export_dir = temp.path / "exported-confdir";
  auto exported = server.cli({"config", "export", *profile, "--source", "runtime", "--output", export_dir.string()});
  REQUIRE(exported.code == 0);
  auto disconnected = server.cli({"disconnect"});
  REQUIRE(disconnected.code == 0);
  REQUIRE(server.json(server.cli({"status"})).value("state", "") == "stopped");
  REQUIRE(assert_xray_check(xray, export_dir, temp.path));

  OwnedProcess standalone;
  REQUIRE(standalone.start(xray, {"run", "-confdir", export_dir.string()}, temp.path, false));
  REQUIRE(wait_for_port(*socks_port));
  const auto exported_response = socks_http_request(*socks_port, http.port());
  CHECK(exported_response.find(http_marker) != std::string::npos);
  INFO("standalone Xray stderr: " << standalone.stderr_text());
  REQUIRE(standalone.stop());
  REQUIRE(server.stop());
  REQUIRE(vless_server.stop());
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto xray = real_xray();
  if (xray.empty()) {
#if defined(XRM_REQUIRE_REAL_XRAY) && XRM_REQUIRE_REAL_XRAY
    std::cerr << "XRM_TEST_XRAY is required for real_core tests\n";
    return 1;
#else
    std::cerr << "SKIP: set XRM_TEST_XRAY to a fixed v26.9.9 Xray binary\n";
    return 77;
#endif
  }
  std::error_code ec;
  if (!xray.is_absolute() || !std::filesystem::is_regular_file(xray, ec) || ::access(xray.c_str(), X_OK) != 0) {
    std::cerr << "XRM_TEST_XRAY must name an absolute executable file: " << xray << '\n';
    return 1;
  }
  const auto version = run_command(xray, {"version"}, std::filesystem::temp_directory_path());
  std::cout << "Real-core binary: " << xray << '\n' << version.out << version.err;
  if (version.code != 0 || version.timed_out || version.out.find("26.9.9") == std::string::npos) {
    std::cerr << "real_core tests require Xray-core v26.9.9\n";
    return 1;
  }
  return Catch::Session().run(argc, argv);
}

TEST_CASE("RC-01 real Xray loads static modules and generated tail from separate files", "[real_core][RC-01]") {
  const auto xray = real_xray();
  TempDirectory temp;
  const auto socks_port = free_loopback_port();
  const auto static_port = free_loopback_port();
  const auto tail_port = free_loopback_port();
  REQUIRE(socks_port.has_value());
  REQUIRE(static_port.has_value());
  REQUIRE(tail_port.has_value());
  REQUIRE(*socks_port != *static_port);
  REQUIRE(*socks_port != *tail_port);
  REQUIRE(*static_port != *tail_port);
  LoopbackHttp static_http;
  LoopbackHttp tail_http;
  REQUIRE(static_http.start("XRM-G1-RC01-STATIC-6d330e"));
  REQUIRE(tail_http.start("XRM-G1-RC01-TAIL-70b41a"));

  XrmServer server(temp);
  REQUIRE(server.start());
  const auto profile = setup_xrm(server, xray);
  REQUIRE(profile.has_value());
  REQUIRE(edit_module(server, *profile, "inbounds", inbound_config(*socks_port)));
  REQUIRE(edit_module(server, *profile, "routing", routing_with_static_direct(static_http.port())));
  auto node = server.cli({"node", "add", "--name", "g1-tail-freedom", "--outbound-json",
                          R"({"protocol":"freedom","settings":{}})"});
  REQUIRE(node.code == 0);
  const auto node_id = server.json(node).at("id").get<std::string>();
  REQUIRE(server.cli({"select", "node", *profile, node_id}).code == 0);
  const auto rendered = server.json(server.cli({"config", "render", *profile}));
  REQUIRE(rendered.contains("confdir"));
  const auto preview = std::filesystem::path(rendered.at("confdir").get<std::string>());
  const auto direct_check = run_command(xray, {"run", "-test", "-confdir", preview.string()}, temp.path);
  INFO("generated routing: " << read_file(preview / "04_routing.json"));
  INFO("direct run -test stdout: " << direct_check.out);
  INFO("direct run -test stderr: " << direct_check.err);
  REQUIRE_FALSE(direct_check.timed_out);
  REQUIRE(direct_check.code == 0);
  const auto connected = server.cli({"connect", *profile});
  INFO(connected.err);
  REQUIRE(connected.code == 0);
  const auto status = server.json(server.cli({"status"}));
  REQUIRE(status.value("state", "") == "running");
  const auto confdir = std::filesystem::path(status.at("active").at("confdir").get<std::string>());
  REQUIRE(std::filesystem::exists(confdir / "00_log.json"));
  REQUIRE(std::filesystem::exists(confdir / "02_inbounds.json"));
  REQUIRE(std::filesystem::exists(confdir / "03_outbounds.json"));
  REQUIRE(std::filesystem::exists(confdir / "04_routing.json"));
  REQUIRE(std::filesystem::exists(confdir / "90_nodes_tail.json"));
  REQUIRE_FALSE(std::filesystem::exists(confdir / "config.json"));
  const auto static_outbounds = Json::parse(read_file(confdir / "03_outbounds.json")).at("outbounds");
  REQUIRE(std::any_of(static_outbounds.begin(), static_outbounds.end(), [](const Json& value) {
    return value.value("tag", "") == "direct" && value.value("protocol", "") == "freedom";
  }));
  const auto tail = Json::parse(read_file(confdir / "90_nodes_tail.json")).at("outbounds");
  REQUIRE(tail.size() == 1);
  REQUIRE(tail.at(0).at("tag") == node_tag(node_id));
  REQUIRE(tail.at(0).at("protocol") == "freedom");
  const auto routing = Json::parse(read_file(confdir / "04_routing.json")).at("routing").at("rules");
  REQUIRE(routing.size() == 2);
  REQUIRE(routing.at(1).at("outboundTag") == node_tag(node_id));

  CHECK(socks_http_request(*socks_port, static_http.port()).find("XRM-G1-RC01-STATIC-6d330e") != std::string::npos);
  CHECK(socks_http_request(*socks_port, tail_http.port()).find("XRM-G1-RC01-TAIL-70b41a") != std::string::npos);
  auto disconnected = server.cli({"disconnect"});
  REQUIRE(disconnected.code == 0);
  REQUIRE(server.stop());
}

TEST_CASE("RC-02 VLESS Codec node traverses XRM and carries loopback traffic", "[real_core][RC-02]") {
  run_vless_flow(false);
}

TEST_CASE("RC-10 exported runtime files run independently without xrmd", "[real_core][RC-10]") {
  run_vless_flow(true);
}

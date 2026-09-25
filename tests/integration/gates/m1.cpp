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
#include <pty.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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
  int served() const { return served_.load(); }
  ~LoopbackHttp() {
    stopping_.store(true);
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    if (worker_.joinable()) worker_.join();
  }
 private:
  void serve() {
    while (!stopping_.load() && remaining_ > 0) {
      pollfd descriptor{fd_, POLLIN, 0};
      const int ready = ::poll(&descriptor, 1, 1000);
      if (ready == 0) continue;
      if (ready < 0) {
        if (errno == EINTR) continue;
        break;
      }
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0) continue;
      timeval timeout{3, 0};
      ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      char request[2048];
      (void)::recv(client, request, sizeof(request), 0);
      const auto response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(marker_.size()) +
          "\r\nConnection: close\r\n\r\n" + marker_;
      (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
      ++served_;
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
      --remaining_;
    }
  }
  int fd_ = -1;
  std::uint16_t port_ = 0;
  std::string marker_;
  int remaining_ = 0;
  std::atomic<int> served_{0};
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

bool wait_for_socks(std::uint16_t port) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in proxy{};
      proxy.sin_family = AF_INET;
      proxy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      proxy.sin_port = htons(port);
      if (::connect(fd, reinterpret_cast<sockaddr*>(&proxy), sizeof(proxy)) == 0) {
        const char hello[] = {5, 1, 0};
        char selection[2]{};
        const bool ready = send_all(fd, std::string_view(hello, sizeof(hello))) &&
                           recv_exact(fd, selection, sizeof(selection)) &&
                           selection[0] == 5 && selection[1] == 0;
        ::close(fd);
        if (ready) return true;
      } else {
        ::close(fd);
      }
    }
    ::poll(nullptr, 0, 20);
  }
  return false;
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

std::string wait_for_socks_marker(std::uint16_t proxy_port, std::uint16_t destination_port,
                                  std::string_view marker) {
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  std::string response;
  while (std::chrono::steady_clock::now() < deadline) {
    response = socks_http_request(proxy_port, destination_port);
    if (response.find(marker) != std::string::npos) return response;
    ::poll(nullptr, 0, 100);
  }
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
  const std::filesystem::path& socket_path() const { return socket_; }
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

class LoopbackSubscription {
 public:
  explicit LoopbackSubscription(std::string body) : body_(std::move(body)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return;
    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd_, 8) != 0) {
      ::close(fd_); fd_ = -1; return;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
      ::close(fd_); fd_ = -1; return;
    }
    url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/nodes";
    worker_ = std::thread([this] { serve(); });
  }
  ~LoopbackSubscription() {
    stopping_ = true;
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    if (worker_.joinable()) worker_.join();
  }
  bool valid() const { return fd_ >= 0; }
  const std::string& url() const { return url_; }
  int requests() const { return requests_.load(); }
 private:
  void serve() {
    while (!stopping_.load()) {
      pollfd ready{fd_, POLLIN, 0};
      if (::poll(&ready, 1, 100) <= 0) continue;
      const int peer = ::accept(fd_, nullptr, nullptr);
      if (peer < 0) continue;
      ++requests_;
      std::array<char, 2048> request{};
      (void)::recv(peer, request.data(), request.size(), 0);
      const auto response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: " +
          std::to_string(body_.size()) + "\r\n\r\n" + body_;
      (void)::send(peer, response.data(), response.size(), MSG_NOSIGNAL);
      ::close(peer);
    }
  }
  int fd_ = -1;
  std::string url_;
  std::string body_;
  std::atomic_bool stopping_{false};
  std::atomic_int requests_{0};
  std::thread worker_;
};

struct TuiProcess {
  pid_t pid = -1;
  int master = -1;
  int exit_status = -1;
};

TuiProcess start_tui_without_profile(const std::filesystem::path& socket,
                                     const std::filesystem::path& visual,
                                     const std::string& profile = {}) {
  int master = -1, slave = -1;
  if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return {};
  winsize size{24, 80, 0, 0};
  (void)::ioctl(slave, TIOCSWINSZ, &size);
  std::vector<std::string> args{XRM_TEST_MANAGER, "--socket", socket.string(), "tui"};
  if (!profile.empty()) { args.push_back("--profile"); args.push_back(profile); }
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  std::vector<std::string> environment;
  for (char** item = environ; item && *item; ++item) {
    const std::string value(*item);
    if (value.starts_with("VISUAL=")) continue;
    environment.push_back(value);
  }
  environment.push_back("VISUAL=" + visual.string());
  std::vector<char*> envp;
  for (auto& value : environment) envp.push_back(value.data());
  envp.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  pid_t pid = -1;
  const int result = ::posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  ::close(slave);
  if (result != 0) { ::close(master); return {}; }
  return {pid, master};
}

bool stop_tui(TuiProcess& tui) {
  if (tui.pid < 0) return true;
  (void)::write(tui.master, "q", 1);
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::waitpid(tui.pid, &status, WNOHANG) == tui.pid) {
      tui.pid = -1;
      tui.exit_status = status;
      ::close(tui.master); tui.master = -1;
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    ::poll(nullptr, 0, 10);
  }
  ::kill(tui.pid, SIGKILL);
  ::waitpid(tui.pid, &status, 0);
  tui.pid = -1;
  ::close(tui.master); tui.master = -1;
  return false;
}

struct TuiProcessGuard {
  TuiProcess& tui;
  ~TuiProcessGuard() { if (tui.pid >= 0) (void)stop_tui(tui); }
};

void tui_keys(TuiProcess& tui, std::string_view keys) {
  (void)::write(tui.master, keys.data(), keys.size());
  ::poll(nullptr, 0, 300);
}

std::string tui_read(TuiProcess& tui, std::chrono::milliseconds duration = 300ms) {
  std::string result;
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd ready{tui.master, POLLIN, 0};
    if (::poll(&ready, 1, 10) <= 0) continue;
    std::array<char, 8192> bytes{};
    const auto count = ::read(tui.master, bytes.data(), bytes.size());
    if (count > 0) result.append(bytes.data(), static_cast<std::size_t>(count));
  }
  return result;
}

std::string tui_wait_for(TuiProcess& tui, std::string_view needle,
                        std::chrono::milliseconds timeout = 5s) {
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    output += tui_read(tui, 100ms);
    if (output.find(needle) != std::string::npos) break;
  }
  return output;
}

Json inbound_config(std::uint16_t port) {
  return Json{{"inbounds", Json::array({Json{
      {"listen", "127.0.0.1"}, {"port", port}, {"protocol", "socks"},
      {"tag", "g1-socks"}, {"settings", Json{{"auth", "noauth"}, {"udp", false}}}}})}};
}

bool edit_module(XrmServer& server, const std::string& profile, const std::string& key, const Json& document) {
  auto result = server.cli({"config", "edit", profile, key, "--text", document.dump()});
  if (result.code != 0) std::cerr << "edit " << key << " failed: " << result.err << '\n';
  return result.code == 0;
}

bool enable_module(XrmServer& server, const std::string& profile, const std::string& key) {
  const auto result = server.cli({"config", "enable", profile, key});
  if (result.code != 0) std::cerr << "enable " << key << " failed: " << result.err << '\n';
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

bool run_vless_flow(bool also_export, std::string transport = "tcp") {
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
  Json inbound{{"listen", "127.0.0.1"}, {"port", *vless_port},
      {"protocol", "vless"}, {"tag", "g1-vless-server"},
      {"settings", Json{{"clients", Json::array({Json{{"id", uuid}, {"email", "g1-local"}}})},
                         {"decryption", "none"}}}};
  if (transport == "ws")
    inbound["streamSettings"] = Json{{"network", "ws"}, {"wsSettings", Json{{"path", "/repair-ws"}}}};
  else if (transport == "xhttp")
    inbound["streamSettings"] = Json{{"network", "xhttp"}, {"xhttpSettings", Json{{"path", "/repair-xhttp"}}}};
  Json server_config{
      {"log", Json{{"loglevel", "warning"}}},
      {"inbounds", Json::array({std::move(inbound)})},
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

  auto uri = "vless://" + uuid + "@127.0.0.1:" + std::to_string(*vless_port) +
      "?encryption=none&type=" + transport;
  if (transport == "ws") uri += "&path=%2Frepair-ws";
  if (transport == "xhttp") uri += "&path=%2Frepair-xhttp";
  uri += "#G1-local-vless-" + transport;
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
    REQUIRE(wait_for_socks(*socks_port));
  const auto exported_response = socks_http_request(*socks_port, http.port());
  CHECK(exported_response.find(http_marker) != std::string::npos);
  INFO("standalone Xray stderr: " << standalone.stderr_text());
  REQUIRE(standalone.stop());
  REQUIRE(server.stop());
  REQUIRE(vless_server.stop());
  return true;
}

bool run_runtime_override_flow(const std::filesystem::path& xray, bool exercise_override) {
  TempDirectory temp;
  const auto socks_port = free_loopback_port();
  const auto native_port = free_loopback_port();
  const auto native_b_port = free_loopback_port();
  const auto api_port = free_loopback_port();
  const auto server_a_port = free_loopback_port();
  const auto server_b_port = free_loopback_port();
  REQUIRE((socks_port && native_port && native_b_port && api_port && server_a_port && server_b_port));
  LoopbackHttp http_a, http_b;
  REQUIRE(http_a.start("XRM-G3-RC06-MARKER-A", 100));
  REQUIRE(http_b.start("XRM-G3-RC06-MARKER-B", 100));
  const std::string uuid_a = "00000000-0000-4000-8000-000000000011";
  const std::string uuid_b = "00000000-0000-4000-8000-000000000012";
  const auto server_config = [&](std::uint16_t listen_port, const std::string& uuid,
                                 std::uint16_t destination_port) {
    Json config{{"log", Json{{"loglevel", "warning"}}}};
    config["inbounds"] = Json::array({Json{{"listen", "127.0.0.1"}, {"port", listen_port},
        {"protocol", "vless"}, {"tag", "rc06-server"},
        {"settings", Json{{"clients", Json::array({Json{{"id", uuid}}})}, {"decryption", "none"}}}}});
    config["outbounds"] = Json::array({Json{{"protocol", "freedom"}, {"tag", "direct"},
        {"settings", Json{{"finalRules", Json::array({Json{{"action", "allow"}, {"network", "tcp"},
          {"port", std::to_string(destination_port)}, {"ip", Json::array({"127.0.0.0/8"})}}})}}}}});
    return config;
  };
  const auto config_a = temp.path / "server-a.json";
  const auto config_b = temp.path / "server-b.json";
  { std::ofstream file(config_a); file << server_config(*server_a_port, uuid_a, http_a.port()).dump(2); }
  { std::ofstream file(config_b); file << server_config(*server_b_port, uuid_b, http_b.port()).dump(2); }
  OwnedProcess server_a, server_b;
  REQUIRE(server_a.start(xray, {"run", "-c", config_a.string()}, temp.path, false));
  REQUIRE(server_b.start(xray, {"run", "-c", config_b.string()}, temp.path, false));
  REQUIRE(wait_for_port(*server_a_port));
  REQUIRE(wait_for_port(*server_b_port));

  XrmServer server(temp);
  REQUIRE(server.start());
  const auto profile = setup_xrm(server, xray);
  REQUIRE(profile);
  REQUIRE(edit_module(server, *profile, "api", Json{{"api", Json{{"tag", "api"},
      {"services", Json::array({"HandlerService", "StatsService", "ObservatoryService", "RoutingService"})}}}}));
  REQUIRE(enable_module(server, *profile, "inbounds"));
  REQUIRE(enable_module(server, *profile, "routing"));
  REQUIRE(enable_module(server, *profile, "api"));
  REQUIRE(edit_module(server, *profile, "observatory", Json{{"observatory", Json{
      {"subjectSelector", Json::array({"xrm.node.g3-rc06-a", "xrm.node.g3-rc06-b"})},
      {"probeUrl", "http://127.0.0.1/"}, {"probeInterval", "5s"}}}}));
  REQUIRE(enable_module(server, *profile, "observatory"));
  REQUIRE(edit_module(server, *profile, "inbounds", Json{{"inbounds", Json::array({
      Json{{"listen", "127.0.0.1"}, {"port", *socks_port}, {"protocol", "socks"},
           {"tag", "g3-socks"}, {"settings", Json{{"auth", "noauth"}, {"udp", false}}}},
      Json{{"listen", "127.0.0.1"}, {"port", *native_port}, {"protocol", "socks"},
           {"tag", "g3-native-socks"}, {"settings", Json{{"auth", "noauth"}, {"udp", false}}}},
      Json{{"listen", "127.0.0.1"}, {"port", *native_b_port}, {"protocol", "socks"},
           {"tag", "g3-native-b-socks"}, {"settings", Json{{"auth", "noauth"}, {"udp", false}}}},
      Json{{"listen", "127.0.0.1"}, {"port", *api_port}, {"protocol", "dokodemo-door"},
           {"tag", "api"}, {"settings", Json{{"address", "127.0.0.1"}}}}
  })}}));
  REQUIRE(edit_module(server, *profile, "routing", Json{{"routing", Json{
      {"balancers", Json::array({Json{{"tag", "g3-static"},
          {"selector", Json::array({node_tag("g3-rc06-a")})},
          {"strategy", Json{{"type", "random"}}}}})},
      {"rules", Json::array({
      Json{{"type", "field"}, {"inboundTag", Json::array({"api"})}, {"outboundTag", "api"}},
      Json{{"type", "field"}, {"inboundTag", Json::array({"g3-native-socks"})}, {"outboundTag", node_tag("g3-rc06-a")}},
      Json{{"type", "field"}, {"inboundTag", Json::array({"g3-native-b-socks"})}, {"outboundTag", node_tag("g3-rc06-b")}},
      Json{{"type", "field"}, {"inboundTag", Json::array({"g3-socks"})}, {"balancerTag", "g3-group"}}})}}}}));
  const auto uri_a = "vless://" + uuid_a + "@127.0.0.1:" + std::to_string(*server_a_port) + "?encryption=none&type=tcp#exit-a";
  const auto uri_b = "vless://" + uuid_b + "@127.0.0.1:" + std::to_string(*server_b_port) + "?encryption=none&type=tcp#exit-b";
  auto node_a = parse_vless_uri(uri_a), node_b = parse_vless_uri(uri_b);
  REQUIRE((node_a && node_b));
  node_a->node.id = "g3-rc06-a"; node_b->node.id = "g3-rc06-b";
  auto store = Store::open(server.database());
  REQUIRE(store);
  REQUIRE((*store)->put_node(node_a->node)); REQUIRE((*store)->put_node(node_b->node));
  store->reset();
  const auto tag_b = node_tag("g3-rc06-b");
  auto group = server.cli({"group", "add", *profile, "--name", "runtime-group", "--tag", "g3-group",
      "--mode", "manual", "--strategy", "random", "--members", "g3-rc06-a", "g3-rc06-b"});
  REQUIRE(group.code == 0);
  const auto group_id = server.json(group).value("id", "");
  REQUIRE(server.cli({"group", "select", *profile, group_id, "g3-rc06-a"}).code == 0);
  auto automatic = server.cli({"group", "add", *profile, "--name", "automatic-single-node",
      "--tag", "g3-auto", "--mode", "automatic", "--strategy", "roundRobin",
      "--members", "g3-rc06-a"});
  REQUIRE(automatic.code == 0);
  const auto automatic_id = server.json(automatic).value("id", "");
  REQUIRE(server.cli({"select", "group", *profile, automatic_id}).code == 0);
  const auto auto_candidate = server.json(server.cli({"config", "render", *profile}));
  const auto auto_check = run_command(xray, {"run", "-test", "-confdir", auto_candidate.at("confdir").get<std::string>()}, temp.path);
  INFO("automatic config check stdout=" << auto_check.out << " stderr=" << auto_check.err);
  const auto automatic_connect = server.cli({"connect", *profile});
  INFO("automatic group connect: " << automatic_connect.out << automatic_connect.err);
  REQUIRE(automatic_connect.code == 0);
  const auto auto_response = socks_http_request(*socks_port, http_a.port());
  INFO("automatic group response: " << auto_response);
  CHECK(auto_response.find("XRM-G3-RC06-MARKER-A") != std::string::npos);
  REQUIRE(server.cli({"disconnect"}).code == 0);
  REQUIRE(server.cli({"select", "group", *profile, group_id}).code == 0);
  const auto candidate = server.json(server.cli({"config", "render", *profile}));
  REQUIRE(candidate.contains("confdir"));
  const auto candidate_dir = std::filesystem::path(candidate.at("confdir").get<std::string>());
  INFO("RC-06 generated routing: " << read_file(candidate_dir / "04_routing.json"));
  INFO("RC-06 generated API: " << read_file(candidate_dir / "06_api.json"));
  auto direct_check = run_command(xray, {"run", "-test", "-confdir", candidate.at("confdir").get<std::string>()}, temp.path);
  INFO("RC-06 candidate stdout: " << direct_check.out);
  INFO("RC-06 candidate stderr: " << direct_check.err);
  REQUIRE(direct_check.code == 0);
  const auto preview_routing = Json::parse(read_file(candidate_dir / "04_routing.json")).at("routing");
  CHECK(preview_routing.at("balancers").size() == 2);
  CHECK(preview_routing.at("rules").at(1).at("outboundTag") == node_tag("g3-rc06-a"));
  auto connect_result = server.cli({"connect", *profile});
  INFO("connect result: " << connect_result.out << connect_result.err);
  REQUIRE(connect_result.code == 0);
  REQUIRE(wait_for_socks(*socks_port));
  const auto before = server.json(server.cli({"status"}));
  const auto confdir = std::filesystem::path(before.at("active").at("confdir").get<std::string>());
  const auto routing_before = read_file(confdir / "04_routing.json");
  const auto tail_before = read_file(confdir / "90_nodes_tail.json");
  CHECK(socks_http_request(*native_port, http_a.port()).find("XRM-G3-RC06-MARKER-A") != std::string::npos);
  CHECK(socks_http_request(*native_b_port, http_b.port()).find("XRM-G3-RC06-MARKER-B") != std::string::npos);
  const auto balancer_info = run_command(xray, {"api", "bi", "--server=127.0.0.1:" + std::to_string(*api_port), "-t", "3", "--json", "g3-group"}, temp.path);
  INFO("balancer info exit=" << balancer_info.code << " stdout=" << balancer_info.out << " stderr=" << balancer_info.err);
  REQUIRE(balancer_info.code == 0);
  const auto baseline = Json::parse(balancer_info.out).at("balancer");
  CHECK(baseline.at("override").empty());
  CHECK(baseline.at("principleTarget").at("tag").at(0) == node_tag("g3-rc06-a"));
  const auto manual_response = socks_http_request(*socks_port, http_a.port());
  INFO("manual saved-A group response: " << manual_response);
  CHECK(manual_response.find("XRM-G3-RC06-MARKER-A") != std::string::npos);
  if (exercise_override) {
    auto override_a = server.cli({"group", "override", "--runtime", "g3-group", node_tag("g3-rc06-a")});
    REQUIRE(override_a.code == 0);
    CHECK(server.json(override_a).value("state", "") == "confirmed");
    const auto override_a_info = run_command(xray, {"api", "bi", "--server=127.0.0.1:" + std::to_string(*api_port), "--json", "g3-group"}, temp.path);
    REQUIRE(override_a_info.code == 0);
    CHECK(Json::parse(override_a_info.out).at("balancer").at("override").at("target") == node_tag("g3-rc06-a"));
    CHECK(socks_http_request(*socks_port, http_a.port()).find("XRM-G3-RC06-MARKER-A") != std::string::npos);
  }
    REQUIRE(server.cli({"group", "select", *profile, group_id, "g3-rc06-b"}).code == 0);
    CHECK(server.json(server.cli({"group", "show", *profile, group_id})).at("selected_node_id") == "g3-rc06-b");
    REQUIRE(server.cli({"disconnect"}).code == 0);
    const auto selected_b = server.json(server.cli({"config", "render", *profile}));
    const auto selected_b_confdir = std::filesystem::path(selected_b.at("confdir").get<std::string>());
    const auto selected_b_routing = Json::parse(read_file(selected_b_confdir / "04_routing.json")).at("routing");
    const auto selected_b_balancer = std::find_if(selected_b_routing.at("balancers").begin(), selected_b_routing.at("balancers").end(),
        [](const Json& value) { return value.value("tag", "") == "g3-group"; });
    REQUIRE(selected_b_balancer != selected_b_routing.at("balancers").end());
    REQUIRE(selected_b_balancer->at("selector").size() == 1);
    CHECK(selected_b_balancer->at("selector").at(0) == tag_b);
    REQUIRE(server.cli({"connect", *profile}).code == 0);
    const auto status_b = server.json(server.cli({"status"}));
    const auto runtime_b = std::filesystem::path(status_b.at("active").at("confdir").get<std::string>());
    INFO("B selected runtime routing: " << read_file(runtime_b / "04_routing.json"));
    const auto actual_b = run_command(xray, {"api", "bi", "--server=127.0.0.1:" + std::to_string(*api_port), "--json", "g3-group"}, temp.path);
    INFO("B selected balancer info: " << actual_b.out);
    const auto response_b = socks_http_request(*socks_port, http_b.port());
    INFO("B selected HTTP response: " << response_b);
    CHECK(response_b.find("XRM-G3-RC06-MARKER-B") != std::string::npos);
  REQUIRE(server.cli({"group", "select", *profile, group_id, "g3-rc06-a"}).code == 0);
  REQUIRE(server.cli({"disconnect"}).code == 0);
  REQUIRE(server.cli({"connect", *profile}).code == 0);
  REQUIRE(wait_for_socks(*socks_port));
  if (exercise_override) {
    const auto override_baseline = server.json(server.cli({"status"}));
    const auto override_confdir = std::filesystem::path(override_baseline.at("active").at("confdir").get<std::string>());
    const auto override_routing = read_file(override_confdir / "04_routing.json");
    const auto override_tail = read_file(override_confdir / "90_nodes_tail.json");
    const auto source_routing_path = temp.path / "profiles" / *profile / "modules" / "04_routing.json";
    const auto source_groups_path = temp.path / "profiles" / *profile / "bindings" / "groups.json";
    const auto source_routing = read_file(source_routing_path);
    const auto source_groups = read_file(source_groups_path);
    auto overridden = server.cli({"group", "override", "--runtime", "g3-group", tag_b});
    INFO("override response: " << overridden.out << overridden.err);
    REQUIRE(overridden.code == 0);
    REQUIRE(server.json(overridden).value("state", "") == "confirmed");
    const auto after = server.json(server.cli({"status"}));
    CHECK(after.at("pid") == override_baseline.at("pid"));
    CHECK(after.at("active").at("confdir") == override_baseline.at("active").at("confdir"));
    CHECK(read_file(override_confdir / "04_routing.json") == override_routing);
    CHECK(read_file(override_confdir / "90_nodes_tail.json") == override_tail);
    CHECK(read_file(source_routing_path) == source_routing);
    CHECK(read_file(source_groups_path) == source_groups);
    CHECK(server.json(server.cli({"profile", "show", *profile})).at("selected").at("id_or_tag") == group_id);
    CHECK(server.json(server.cli({"group", "show", *profile, group_id})).at("selected_node_id") == "g3-rc06-a");
    const auto override_b_info = run_command(xray, {"api", "bi", "--server=127.0.0.1:" + std::to_string(*api_port), "--json", "g3-group"}, temp.path);
    REQUIRE(override_b_info.code == 0);
    CHECK(Json::parse(override_b_info.out).at("balancer").at("override").at("target") == tag_b);
    CHECK(socks_http_request(*socks_port, http_b.port()).find("XRM-G3-RC06-MARKER-B") != std::string::npos);
    CHECK(server.json(server.cli({"status"})).value("runtime_override", "") == tag_b);
    auto clear = server.cli({"group", "override", "--clear", "--runtime", "g3-group"});
    REQUIRE(clear.code == 0);
    const auto cleared = run_command(xray, {"api", "bi", "--server=127.0.0.1:" + std::to_string(*api_port), "--json", "g3-group"}, temp.path);
    REQUIRE(cleared.code == 0);
    CHECK(Json::parse(cleared.out).at("balancer").at("override").empty());
  }
  REQUIRE(server.cli({"disconnect"}).code == 0);
  REQUIRE(server.cli({"connect", *profile}).code == 0);
  REQUIRE(wait_for_socks(*socks_port));
  CHECK(server.json(server.cli({"status"})).value("override_state", "") == "none");
  if (exercise_override) {
    const auto final_status = server.json(server.cli({"status"}));
    const auto final_dir = std::filesystem::path(final_status.at("active").at("confdir").get<std::string>());
    INFO("restart runtime routing: " << read_file(final_dir / "04_routing.json"));
    const auto restart_info = run_command(xray, {"api", "bi", "--server=127.0.0.1:" + std::to_string(*api_port), "--json", "g3-group"}, temp.path);
    INFO("restart balancer info: " << restart_info.out);
    const auto final_response = wait_for_socks_marker(*socks_port, http_a.port(), "XRM-G3-RC06-MARKER-A");
    INFO("restart saved-selection response: " << final_response);
    INFO("marker A HTTP requests served: " << http_a.served());
    INFO("VLESS server A output: " << server_a.stdout_text() << server_a.stderr_text());
    INFO("XRM core logs after restart: " << server.cli({"logs", "--limit", "65536"}).out);
    CHECK(final_response.find("XRM-G3-RC06-MARKER-A") != std::string::npos);
  }
  REQUIRE(server.cli({"disconnect"}).code == 0);
  REQUIRE(server.stop());
  REQUIRE(server_a.stop()); REQUIRE(server_b.stop());
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

TEST_CASE("VLESS WS and XHTTP transports carry loopback traffic through XRM", "[real_core][product-repair][VLESS-WS-XHTTP]") {
  REQUIRE(run_vless_flow(false, "ws"));
  REQUIRE(run_vless_flow(false, "xhttp"));
}

TEST_CASE("RC-10 exported runtime files run independently without xrmd", "[real_core][RC-10]") {
  run_vless_flow(true);
}

TEST_CASE("RC-06 runtime API override changes only current Xray group selection", "[real_core][RC-06]") {
  REQUIRE(run_runtime_override_flow(real_xray(), true));
}

TEST_CASE("RC-03 native routing and manual groups send real loopback traffic", "[real_core][RC-03]") {
  REQUIRE(run_runtime_override_flow(real_xray(), false));
}

TEST_CASE("E2E-07 cold-start human flow begins in TUI and carries real VLESS traffic", "[real_core][E2E-07]") {
  const auto xray = real_xray();
  if (xray.empty() || !xray.is_absolute() || !std::filesystem::is_regular_file(xray) ||
      ::access(xray.c_str(), X_OK) != 0) {
    SKIP("E2E-07 requires XRM_TEST_XRAY fixed v26.9.9");
  }

  TempDirectory temp;
  const auto vless_port = free_loopback_port();
  const auto socks_port = free_loopback_port();
  const auto metrics_port = free_loopback_port();
  REQUIRE(vless_port.has_value());
  REQUIRE(socks_port.has_value());
  REQUIRE(metrics_port.has_value());
  REQUIRE(*vless_port != *socks_port);
  REQUIRE(*metrics_port != *socks_port);
  REQUIRE(*metrics_port != *vless_port);
  constexpr std::string_view uuid = "10000000-0000-4000-8000-000000000007";
  constexpr std::string_view marker = "XRM-E2E-07-TUI-COLD-START-MARKER-61f2";
  LoopbackHttp http;
  REQUIRE(http.start(std::string(marker), 8));

  const auto peer_config = temp.path / "vless-peer.json";
  const Json peer{{"log", Json{{"loglevel", "warning"}}},
      {"inbounds", Json::array({Json{{"listen", "127.0.0.1"}, {"port", *vless_port},
          {"protocol", "vless"}, {"tag", "e2e07-vless-peer"},
          {"settings", Json{{"clients", Json::array({Json{{"id", uuid}}})}, {"decryption", "none"}}}}})},
      {"outbounds", Json::array({Json{{"protocol", "freedom"}, {"tag", "direct"},
          {"settings", Json{{"finalRules", Json::array({Json{{"action", "allow"}, {"network", "tcp"},
              {"port", std::to_string(http.port())}, {"ip", Json::array({"127.0.0.0/8"})}}})}}}}})}};
  { std::ofstream output(peer_config); REQUIRE(output.good()); output << peer.dump(2) << '\n'; }
  OwnedProcess peer_process;
  REQUIRE(peer_process.start(xray, {"run", "-c", peer_config.string()}, temp.path));
  REQUIRE(wait_for_port(*vless_port));

  const auto uri = "vless://" + std::string(uuid) + "@127.0.0.1:" +
      std::to_string(*vless_port) + "?encryption=none&type=tcp#Cold-start-VLESS";
  LoopbackSubscription subscription(uri + "\n");
  REQUIRE(subscription.valid());
  XrmServer server(temp);
  REQUIRE(server.start());

  const auto editor = temp.path / "test-editor";
  {
    std::ofstream output(editor);
    REQUIRE(output.good());
    output << "#!/bin/sh\nprintf '%s' '{\"inbounds\":[{\"listen\":\"127.0.0.1\",\"port\":"
           << *socks_port << ",\"protocol\":\"socks\",\"tag\":\"e2e07-socks\","
              "\"settings\":{\"auth\":\"noauth\",\"udp\":false}}]}' > \"$1\"\n";
  }
  REQUIRE(::chmod(editor.c_str(), 0700) == 0);

  auto tui = start_tui_without_profile(server.socket_path(), editor);
  TuiProcessGuard tui_guard{tui};
  REQUIRE(tui.pid > 0);
  const auto initial = tui_wait_for(tui, "Overview", 5s);
  CHECK(initial.find("Overview") != std::string::npos);

  // Global inventory is established entirely through the no-profile TUI.
  tui_keys(tui, "4a");
  tui_keys(tui, "local VLESS fixture\r");
  tui_keys(tui, subscription.url() + "\r");
  tui_keys(tui, "u");
  CHECK(subscription.requests() >= 1);
  const auto update_result = tui_wait_for(tui, "Subscription update complete", 8s);
  INFO("TUI subscription completion: " << update_result);
  REQUIRE(update_result.find("Subscription update complete") != std::string::npos);

  tui_keys(tui, "a");
  REQUIRE(tui_wait_for(tui, "Subscription name", 5s).find("Subscription name") != std::string::npos);
  tui_keys(tui, "backup VLESS fixture\r");
  REQUIRE(tui_wait_for(tui, "Subscription URL", 5s).find("Subscription URL") != std::string::npos);
  tui_keys(tui, subscription.url() + "\r");
  const auto backup_added = tui_wait_for(tui, "Subscription saved globally", 5s);
  INFO("Backup subscription added: " << backup_added);
  REQUIRE(backup_added.find("Subscription saved globally") != std::string::npos);
  const auto global_sources = server.json(server.cli({"sub", "list"}));
  REQUIRE(global_sources.size() == 2);
  const auto source_id = [&](std::string_view name) {
    const auto found = std::find_if(global_sources.begin(), global_sources.end(),
        [&](const Json& item) { return item.value("name", "") == name; });
    return found == global_sources.end() ? std::string{} : found->value("id", "");
  };
  const auto source_a = source_id("local VLESS fixture");
  const auto source_b = source_id("backup VLESS fixture");
  REQUIRE_FALSE(source_a.empty());
  REQUIRE_FALSE(source_b.empty());
  REQUIRE(source_a != source_b);
  const auto source_index = [&](std::string_view id) {
    const auto found = std::find_if(global_sources.begin(), global_sources.end(),
        [&](const Json& item) { return item.value("id", "") == id; });
    return static_cast<std::size_t>(found - global_sources.begin());
  };
  const auto point_to_source = [&](std::string_view id) {
    tui_keys(tui, "\x1b[A\x1b[A");  // ArrowUp saturates at the first row.
    if (source_index(id) == 1) tui_keys(tui, "\x1b[B");
  };
  point_to_source(source_b);
  tui_keys(tui, "u");
  REQUIRE(tui_wait_for(tui, "Subscription update complete", 8s).find(
      "Subscription update complete") != std::string::npos);

  // Register the fixed core and create a named Profile in the same TUI session.
  tui_keys(tui, "7a");
  tui_keys(tui, xray.string() + "\r");
  tui_keys(tui, "2n");
  REQUIRE(tui_wait_for(tui, "Create Profile · choose a template", 5s).find(
      "Create Profile · choose a template") != std::string::npos);
  tui_keys(tui, "1"); // Desktop, then name; TUN remains an explicit separate choice.
  tui_keys(tui, "Cold Start\r");
  tui_keys(tui, "4");
  point_to_source(source_a);
  REQUIRE(tui_wait_for(tui, "Selected: local VLESS fixture", 5s).find(
      "Selected: local VLESS fixture") != std::string::npos);
  tui_keys(tui, "b");  // Only source A is used by this Profile.
  REQUIRE(tui_wait_for(tui, "Source included in viewed Profile", 5s).find(
      "Source included in viewed Profile") != std::string::npos);
  tui_keys(tui, "5");
  auto node_screen = tui_wait_for(tui, "Cold-start-VLESS", 5s);
  CHECK(node_screen.find("Cold-start-VLESS") != std::string::npos);
  tui_keys(tui, "s");
  const auto profile_rows = server.json(server.cli({"profile", "list"}));
  REQUIRE(profile_rows.is_array());
  REQUIRE(profile_rows.size() == 1);
  const auto saved_profile = server.json(server.cli({"profile", "show", profile_rows.front().value("id", "")}));
  INFO("saved selection after TUI node action: " << saved_profile.value("selected", Json(nullptr)).dump());
  REQUIRE(saved_profile.value("selected", Json(nullptr)).is_object());
  REQUIRE(saved_profile.at("selected").value("kind", "") == "node");
  REQUIRE(saved_profile.at("enabled_source_ids") == Json::array({source_a}));

  // Configure the local client listener via Modules -> Inbounds -> external
  // editor, then explicitly save the source document.
  tui_keys(tui, "3\r\t\t\r\r");  // Modules → inbounds tab → content → native editor.
  REQUIRE(tui_wait_for(tui, "Native module editor", 5s).find("Native module editor") != std::string::npos);
  tui_keys(tui, "v");
  tui_keys(tui, "\x13");
  const auto after_module_save = tui_read(tui);
  INFO("after module save: " << after_module_save);
  tui_keys(tui, "c");

  Json running;
  const auto running_deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < running_deadline) {
    const auto result = server.cli({"status"});
    running = server.json(result);
    if (running.value("state", "") == "running" || running.value("state", "") == "failed") break;
    ::poll(nullptr, 0, 40);
  }
  INFO("running status: " << running.dump());
  REQUIRE(running.value("state", "") == "running");
  const auto tui_connected = tui_wait_for(tui, "Xray core running", 8s);
  CHECK(tui_connected.find("Xray core running") != std::string::npos);
  REQUIRE(wait_for_socks(*socks_port));
  const auto proxied = wait_for_socks_marker(*socks_port, http.port(), marker);
  INFO("loopback VLESS response: " << proxied);
  CHECK(proxied.find(marker) != std::string::npos);
  REQUIRE(http.served() >= 1);

  // The same cold-start TUI session can create a typed Local node and then
  // choose it as a Group outlet. Neither client config is handwritten here.
  tui_keys(tui, "5a1");
  for (const auto& field : {std::string("vless"), std::string("Local VLESS"),
                            std::string("127.0.0.1"), std::to_string(*vless_port),
                            std::string(uuid), std::string("raw"), std::string("none")})
    tui_keys(tui, field + "\r");
  const auto local_screen = tui_wait_for(tui, "Manual node saved to Local", 5s);
  INFO("Local node TUI screen: " << local_screen);
  REQUIRE(local_screen.find("Local VLESS") != std::string::npos);
  const auto first_run = server.json(server.cli({"status"})).at("active").value("run_id", "");
  tui_keys(tui, "sc");
  Json local_running;
  const auto local_deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < local_deadline) {
    local_running = server.json(server.cli({"status"}));
    if (local_running.value("state", "") == "running" &&
        local_running.at("active").value("run_id", "") != first_run) break;
    ::poll(nullptr, 0, 40);
  }
  INFO("Local node connection status: " << local_running.dump());
  REQUIRE(local_running.value("state", "") == "running");
  REQUIRE(local_running.at("active").value("run_id", "") != first_run);
  const auto local_response = wait_for_socks_marker(*socks_port, http.port(), marker);
  INFO("Local typed node loopback response: " << local_response);
  CHECK(local_response.find(marker) != std::string::npos);

  tui_keys(tui, "a4");
  for (const auto& field : {"Local group", "automatic", "random", "local"})
    tui_keys(tui, std::string(field) + "\r");
  const auto group_screen = tui_wait_for(tui, "Group saved as a routing balancer", 5s);
  INFO("Group TUI screen: " << group_screen);
  REQUIRE(group_screen.find("Local group") != std::string::npos);
  const auto local_run_id = local_running.at("active").value("run_id", "");
  tui_keys(tui, "sc");
  Json group_running;
  const auto group_deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < group_deadline) {
    group_running = server.json(server.cli({"status"}));
    if (group_running.value("state", "") == "running" &&
        group_running.at("active").value("run_id", "") != local_run_id) break;
    ::poll(nullptr, 0, 40);
  }
  INFO("Group connection status: " << group_running.dump());
  REQUIRE(group_running.value("state", "") == "running");
  REQUIRE(group_running.at("active").value("run_id", "") != local_run_id);
  const auto group_response = wait_for_socks_marker(*socks_port, http.port(), marker);
  INFO("Local Group loopback response: " << group_response);
  CHECK(group_response.find(marker) != std::string::npos);
  const auto group_confdir = std::filesystem::path(group_running.at("active").at("confdir").get<std::string>());
  CHECK(read_file(group_confdir / "04_routing.json").find("xrm-group-") != std::string::npos);
  const auto local_inventory = server.json(server.cli({"node", "list", "--limit", "20"}));
  auto local_row = std::find_if(local_inventory.at("items").begin(), local_inventory.at("items").end(),
      [](const Json& item) { return item.value("name", "") == "Local VLESS"; });
  REQUIRE(local_row != local_inventory.at("items").end());
  CHECK(read_file(group_confdir / "90_nodes_tail.json").find(
      node_tag(local_row->value("id", ""))) != std::string::npos);

  // Enable real native counters through the product action, then explicitly
  // reconnect. Overview must consume the Server's sampled Xray counters.
  const auto home_id = profile_rows.front().value("id", "");
  const auto stats_action = server.cli({"config", "statistics", "enable", home_id,
      "--metrics-listen", "127.0.0.1:" + std::to_string(*metrics_port)});
  INFO("native statistics action: " << stats_action.out << stats_action.err);
  REQUIRE(stats_action.code == 0);
  CHECK(server.json(server.cli({"status"})).value("pid", Json(nullptr)) ==
      group_running.value("pid", Json(nullptr)));
  const auto stats_connect = server.cli({"connect", home_id});
  INFO("statistics connect: " << stats_connect.out << stats_connect.err);
  REQUIRE(stats_connect.code == 0);
  Json stats_running;
  const auto stats_deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < stats_deadline) {
    stats_running = server.json(server.cli({"status"}));
    if (stats_running.value("state", "") == "running" &&
        stats_running.at("active").value("run_id", "") !=
            group_running.at("active").value("run_id", "")) break;
    ::poll(nullptr, 0, 40);
  }
  REQUIRE(stats_running.value("state", "") == "running");
  REQUIRE(stats_running.at("active").value("run_id", "") !=
      group_running.at("active").value("run_id", ""));
  REQUIRE(wait_for_port(*metrics_port));
  CHECK(wait_for_socks_marker(*socks_port, http.port(), marker).find(marker) != std::string::npos);
  INFO("statistics modules: " << server.cli({"config", "modules", home_id}).out);
  INFO("statistics RPC: " << server.cli({"stats"}).out);
  tui_keys(tui, "1");
  std::string overview_stats;
  const auto overview_deadline = std::chrono::steady_clock::now() + 12s;
  while (std::chrono::steady_clock::now() < overview_deadline) {
    overview_stats += tui_read(tui, 500ms);
    if (overview_stats.find("Cumulative inbound/") != std::string::npos ||
        overview_stats.find("Cumulative outbound/") != std::string::npos ||
        overview_stats.find("Cumulative user/") != std::string::npos) break;
  }
  INFO("Overview real statistics screen: " << overview_stats);
  CHECK((overview_stats.find("Cumulative inbound/") != std::string::npos ||
        overview_stats.find("Cumulative outbound/") != std::string::npos ||
        overview_stats.find("Cumulative user/") != std::string::npos));

  // Viewing another Profile and binding the second source must not switch the
  // one running Xray process or duplicate the global node inventory.
  const auto before_office = stats_running;
  tui_keys(tui, "2n");
  REQUIRE(tui_wait_for(tui, "Create Profile · choose a template", 5s).find(
      "Create Profile · choose a template") != std::string::npos);
  tui_keys(tui, "1");
  const auto office_prompt = tui_wait_for(tui, "New desktop Profile name", 5s);
  INFO("Office creation prompt: " << office_prompt);
  REQUIRE(office_prompt.find("New desktop Profile name") != std::string::npos);
  tui_keys(tui, "Office\r");
  const auto office_created = tui_wait_for(tui, "Profile selected for editing", 5s);
  INFO("Office created screen: " << office_created);
  REQUIRE(office_created.find("Profile selected for editing") != std::string::npos);
  tui_keys(tui, "4");
  point_to_source(source_b);
  REQUIRE(tui_wait_for(tui, "Selected: backup VLESS fixture", 5s).find(
      "Selected: backup VLESS fixture") != std::string::npos);
  tui_keys(tui, "b");
  REQUIRE(tui_wait_for(tui, "Source included in viewed Profile", 5s).find(
      "Source included in viewed Profile") != std::string::npos);
  const auto profiles_after_office = server.json(server.cli({"profile", "list"}));
  REQUIRE(profiles_after_office.size() == 2);
  const auto office_row = std::find_if(profiles_after_office.begin(), profiles_after_office.end(),
      [](const Json& item) { return item.value("name", "") == "Office"; });
  REQUIRE(office_row != profiles_after_office.end());
  const auto office_id = office_row->at("id").get<std::string>();
  const auto office = server.json(server.cli({"profile", "show", office_id}));
  REQUIRE(office.at("enabled_source_ids") == Json::array({source_b}));
  tui_keys(tui, "5");
  REQUIRE(tui_wait_for(tui, "Cold-start-VLESS", 5s).find("Cold-start-VLESS") != std::string::npos);
  const auto while_viewing_office = server.json(server.cli({"status"}));
  CHECK(while_viewing_office.value("pid", Json(nullptr)) == before_office.value("pid", Json(nullptr)));
  CHECK(while_viewing_office.at("active").value("confdir", "") ==
      before_office.at("active").value("confdir", ""));
  CHECK(while_viewing_office.at("active").value("profile_id", "") ==
      before_office.at("active").value("profile_id", ""));
  tui_keys(tui, "2\x1b[A\r");  // Return to the Home viewing context.

  // Open and save a disabled native example without applying it.
  const auto before_edit = stats_running;
  tui_keys(tui, "3f");
  REQUIRE(tui_wait_for(tui, "Find native module key", 5s).find("Find native module key") != std::string::npos);
  tui_keys(tui, "api\r\r\r"); // Disabled api module: select tab, enter content, open draft.
  auto disabled = tui_read(tui);
  CHECK(disabled.find("Native module editor") != std::string::npos);
  tui_keys(tui, "\x13");
  const auto after_edit = server.json(server.cli({"status"}));
  CHECK(after_edit.value("pid", Json(nullptr)) == before_edit.value("pid", Json(nullptr)));
  CHECK(after_edit.at("active").value("run_id", "") == before_edit.at("active").value("run_id", ""));
  CHECK(after_edit.at("active").value("confdir", "") == before_edit.at("active").value("confdir", ""));
  CHECK(after_edit.value("state", "") == "running");
  const auto save_feedback = tui_read(tui);
  INFO("disabled module save feedback: " << save_feedback);
  CHECK(save_feedback.find("saved source text") != std::string::npos);

  const auto tui_stopped = stop_tui(tui);
  INFO("TUI stopped=" << tui_stopped << " exit_status=" << tui.exit_status << " pid=" << tui.pid);
  REQUIRE(tui_stopped);
  const auto after_exit = server.json(server.cli({"status"}));
  CHECK(after_exit.value("state", "") == "running");
  CHECK(after_exit.at("active").value("profile_id", "") == before_edit.at("active").value("profile_id", ""));

  // An existing Profile with an empty inbound source must remain untouched
  // until its local-use entry is explicitly confirmed in the real TUI.
  const auto legacy = server.json(server.cli({"profile", "create", "--name", "Existing Empty Inbounds",
      "--core", profile_rows.front().value("core_id", "")}));
  REQUIRE(legacy.contains("id"));
  const auto legacy_id = legacy.at("id").get<std::string>();
  REQUIRE(server.cli({"config", "edit", legacy_id, "inbounds", "--text", "{\"inbounds\":[]}"}).code == 0);
  auto legacy_tui = start_tui_without_profile(server.socket_path(), editor, legacy_id);
  TuiProcessGuard legacy_guard{legacy_tui};
  REQUIRE(legacy_tui.pid > 0);
  const auto missing_entry = tui_wait_for(legacy_tui, "no local SOCKS entry", 5s);
  INFO("existing Profile before confirmed repair: " << missing_entry);
  REQUIRE(missing_entry.find("no local SOCKS entry") != std::string::npos);
  tui_keys(legacy_tui, "i");
  const auto confirmation = tui_wait_for(legacy_tui, "Add local proxy entry", 5s);
  REQUIRE(confirmation.find("127.0.0.1:10808") != std::string::npos);
  tui_keys(legacy_tui, "y");
  const auto saved_entry = tui_wait_for(legacy_tui, "Local SOCKS 127.0.0.1:10808 saved", 5s);
  INFO("existing Profile entry saved: " << saved_entry);
  REQUIRE(saved_entry.find("Local SOCKS 127.0.0.1:10808 saved") != std::string::npos);
  tui_keys(legacy_tui, "4b");
  tui_keys(legacy_tui, "5s");
  tui_keys(legacy_tui, "c");
  Json legacy_running;
  const auto legacy_deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < legacy_deadline) {
    legacy_running = server.json(server.cli({"status"}));
    if (legacy_running.value("state", "") == "running" &&
        legacy_running.at("active").value("profile_id", "") == legacy_id) break;
    ::poll(nullptr, 0, 40);
  }
  INFO("existing Profile runtime: " << legacy_running.dump());
  REQUIRE(legacy_running.value("state", "") == "running");
  REQUIRE(legacy_running.at("active").value("profile_id", "") == legacy_id);
  REQUIRE(wait_for_socks(10808));
  const auto legacy_marker = wait_for_socks_marker(10808, http.port(), marker);
  INFO("existing Profile loopback marker: " << legacy_marker);
  REQUIRE(legacy_marker.find(marker) != std::string::npos);
  const auto legacy_feedback = tui_wait_for(legacy_tui, "Xray core running", 8s);
  INFO("existing Profile TUI connect feedback: " << legacy_feedback);
  REQUIRE(legacy_feedback.find("Xray core running") != std::string::npos);
  REQUIRE(stop_tui(legacy_tui));
  CHECK(server.json(server.cli({"status"})).value("state", "") == "running");
  CHECK(server.cli({"disconnect"}).code == 0);
  CHECK(server.stop());
}

TEST_CASE("TUI manual Group selection A then B reaches the corresponding real Xray exit",
          "[real_core][GROUP-TUI-E2E]") {
  const auto xray = real_xray();
  if (xray.empty() || !xray.is_absolute() || !std::filesystem::is_regular_file(xray) ||
      ::access(xray.c_str(), X_OK) != 0)
    SKIP("GROUP-TUI-E2E requires XRM_TEST_XRAY fixed v26.9.9");

  TempDirectory temp;
  const auto peer_a_port = free_loopback_port();
  const auto peer_b_port = free_loopback_port();
  const auto socks_port = free_loopback_port();
  REQUIRE((peer_a_port && peer_b_port && socks_port));
  LoopbackHttp http_a, http_b;
  REQUIRE(http_a.start("XRM-TUI-GROUP-EXIT-A-9d31", 8));
  REQUIRE(http_b.start("XRM-TUI-GROUP-EXIT-B-2c74", 8));
  constexpr std::string_view uuid_a = "30000000-0000-4000-8000-000000000001";
  constexpr std::string_view uuid_b = "30000000-0000-4000-8000-000000000002";
  const auto start_peer = [&](std::string_view name, std::uint16_t listen_port,
                              std::string_view uuid, std::uint16_t destination_port,
                              OwnedProcess& process) {
    const Json config{{"log", Json{{"loglevel", "warning"}}},
        {"inbounds", Json::array({Json{{"listen", "127.0.0.1"}, {"port", listen_port},
            {"protocol", "vless"}, {"tag", std::string(name)},
            {"settings", Json{{"clients", Json::array({Json{{"id", uuid}}})}, {"decryption", "none"}}}}})},
        {"outbounds", Json::array({Json{{"protocol", "freedom"}, {"tag", "direct"},
            {"settings", Json{{"finalRules", Json::array({Json{{"action", "allow"}, {"network", "tcp"},
                {"port", std::to_string(destination_port)}, {"ip", Json::array({"127.0.0.0/8"})}}})}}}}})}};
    const auto path = temp.path / (std::string(name) + ".json");
    { std::ofstream output(path); REQUIRE(output.good()); output << config.dump(2) << '\n'; }
    REQUIRE(process.start(xray, {"run", "-c", path.string()}, temp.path));
    REQUIRE(wait_for_port(listen_port));
  };
  OwnedProcess peer_a, peer_b;
  start_peer("group-peer-a", *peer_a_port, uuid_a, http_a.port(), peer_a);
  start_peer("group-peer-b", *peer_b_port, uuid_b, http_b.port(), peer_b);

  XrmServer server(temp);
  REQUIRE(server.start());
  auto tui = start_tui_without_profile(server.socket_path(), "/bin/true");
  TuiProcessGuard tui_guard{tui};
  REQUIRE(tui.pid > 0);
  REQUIRE(tui_wait_for(tui, "Overview", 5s).find("Overview") != std::string::npos);
  tui_keys(tui, "7a");
  tui_keys(tui, xray.string() + "\r");
  tui_keys(tui, "2n1TUI Group A-B\r");
  REQUIRE(tui_wait_for(tui, "Profile selected for editing", 5s).find("TUI Group A-B") != std::string::npos);
  const auto profiles = server.json(server.cli({"profile", "list"}));
  REQUIRE(profiles.is_array());
  REQUIRE(profiles.size() == 1);
  const auto profile_id = profiles.front().value("id", "");
  // Only the disposable listener port is set outside the UI. The Group,
  // members, saved target and both connects below all go through real TUI keys.
  REQUIRE(edit_module(server, profile_id, "inbounds", inbound_config(*socks_port)));
  REQUIRE(enable_module(server, profile_id, "inbounds"));

  const auto add_local = [&](std::string_view name, std::uint16_t port, std::string_view uuid) {
    tui_keys(tui, "5a1");
    for (const auto& field : {std::string("vless"), std::string(name), std::string("127.0.0.1"),
                               std::to_string(port), std::string(uuid), std::string("raw"), std::string("none")})
      tui_keys(tui, field + "\r");
    REQUIRE(tui_wait_for(tui, "Manual node saved to Local", 5s).find("Manual node saved to Local") != std::string::npos);
    const auto current = server.json(server.cli({"node", "list", "--limit", "20"}));
    REQUIRE(std::any_of(current.at("items").begin(), current.at("items").end(),
        [&](const Json& row) { return row.value("name", "") == name; }));
  };
  add_local("Exit A", *peer_a_port, uuid_a);
  add_local("Exit B", *peer_b_port, uuid_b);
  const auto inventory = server.json(server.cli({"node", "list", "--limit", "20"}));
  REQUIRE(inventory.at("items").size() == 2);
  const auto find_id = [&](std::string_view name) {
    const auto found = std::find_if(inventory.at("items").begin(), inventory.at("items").end(),
        [&](const Json& row) { return row.value("name", "") == name; });
    return found == inventory.at("items").end() ? std::string{} : found->value("id", "");
  };
  const auto id_a = find_id("Exit A"), id_b = find_id("Exit B");
  REQUIRE_FALSE(id_a.empty());
  REQUIRE_FALSE(id_b.empty());

  tui_keys(tui, "/Exit A\r");
  REQUIRE(tui_wait_for(tui, "Search: Exit A", 5s).find("Exit A") != std::string::npos);
  tui_keys(tui, "a4");
  // Manual mode fixes the underlying strategy to random and goes directly to
  // the member scope prompt; unlike automatic mode it has no strategy prompt.
  for (const auto& field : {"TUI manual exits", "manual", "local"})
    tui_keys(tui, std::string(field) + "\r");
  const auto group_screen = tui_wait_for(tui, "Group saved as a routing balancer", 5s);
  INFO("manual Group creation screen: " << group_screen.substr(group_screen.size() > 1200 ? group_screen.size() - 1200 : 0));
  REQUIRE(group_screen.find("TUI manual exits") != std::string::npos);
  const auto groups = server.json(server.cli({"group", "list", profile_id}));
  REQUIRE(groups.size() == 1);
  const auto group_id = groups.front().value("id", "");
  REQUIRE_FALSE(group_id.empty());
  REQUIRE(groups.front().at("members").value("include_local", false));
  REQUIRE(groups.front().value("selection_mode", "") == "manual");
  REQUIRE(groups.front().value("selected_node_id", "") == id_a);

  tui_keys(tui, "sc");
  Json running_a;
  const auto deadline_a = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < deadline_a) {
    running_a = server.json(server.cli({"status"}));
    if (running_a.value("state", "") == "running") break;
    ::poll(nullptr, 0, 40);
  }
  INFO("TUI Group A runtime: " << running_a.dump());
  REQUIRE(running_a.value("state", "") == "running");
  REQUIRE(running_a.at("active").at("selection").value("kind", "") == "group");
  REQUIRE(running_a.at("active").at("selection").value("id_or_tag", "") == group_id);
  REQUIRE(wait_for_socks(*socks_port));
  const auto route_selector = [&](const Json& runtime) {
    const auto path = std::filesystem::path(runtime.at("active").value("confdir", "")) / "04_routing.json";
    const auto routing = Json::parse(read_file(path)).at("routing");
    const auto& balancers = routing.at("balancers");
    const auto found = std::find_if(balancers.begin(), balancers.end(), [&](const Json& value) {
      return value.value("tag", "") == groups.front().at("balancer").value("tag", "");
    });
    return found == balancers.end() ? Json(nullptr) : found->at("selector");
  };
  REQUIRE(route_selector(running_a) == Json::array({node_tag(id_a)}));
  const auto marker_a = wait_for_socks_marker(*socks_port, http_a.port(), "XRM-TUI-GROUP-EXIT-A-9d31");
  INFO("TUI selected A traffic: " << marker_a);
  REQUIRE(marker_a.find("XRM-TUI-GROUP-EXIT-A-9d31") != std::string::npos);
  const auto run_a = running_a.at("active").value("run_id", "");
  // The daemon can report running before the Manager consumes the operation's
  // completion event. Wait for the actual UI feedback before starting another
  // Connect, especially under ASan.
  REQUIRE(tui_wait_for(tui, "Xray core running", 8s).find("Xray core running") != std::string::npos);

  // Group -> choose member -> Nodes -> B -> save member -> Group -> connect.
  tui_keys(tui, "m");
  REQUIRE(tui_wait_for(tui, "choose a candidate on Nodes and press m", 5s).find("Nodes") != std::string::npos);
  // The earlier A search is deliberately retained for this category. Change
  // the visible filter through the normal Nodes search before choosing B.
  tui_keys(tui, "/Exit B\r");
  REQUIRE(tui_wait_for(tui, "Search: Exit B", 5s).find("Exit B") != std::string::npos);
  tui_keys(tui, "m");
  const auto selected_b = tui_wait_for(tui, "manual group member saved", 5s);
  INFO("manual member B save screen: " << selected_b.substr(selected_b.size() > 1200 ? selected_b.size() - 1200 : 0));
  REQUIRE(selected_b.find("manual group member saved") != std::string::npos);
  REQUIRE(server.json(server.cli({"group", "show", profile_id, group_id})).value("selected_node_id", "") == id_b);
  tui_keys(tui, "g");
  REQUIRE(tui_wait_for(tui, "Group selection is persistent", 5s).find("TUI manual exits") != std::string::npos);
  tui_keys(tui, "c");
  REQUIRE(tui_wait_for(tui, "Connect started", 5s).find("Connect started") != std::string::npos);
  Json running_b;
  const auto deadline_b = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < deadline_b) {
    running_b = server.json(server.cli({"status"}));
    if (running_b.value("state", "") == "running" &&
        running_b.at("active").value("run_id", "") != run_a) break;
    ::poll(nullptr, 0, 40);
  }
  INFO("TUI Group B runtime: " << running_b.dump());
  REQUIRE(running_b.value("state", "") == "running");
  REQUIRE(running_b.at("active").value("run_id", "") != run_a);
  REQUIRE(route_selector(running_b) == Json::array({node_tag(id_b)}));
  const auto marker_b = wait_for_socks_marker(*socks_port, http_b.port(), "XRM-TUI-GROUP-EXIT-B-2c74");
  INFO("TUI selected B traffic: " << marker_b);
  REQUIRE(marker_b.find("XRM-TUI-GROUP-EXIT-B-2c74") != std::string::npos);
  REQUIRE(http_a.served() >= 1);
  REQUIRE(http_b.served() >= 1);
  REQUIRE(tui_wait_for(tui, "Xray core running", 8s).find("Xray core running") != std::string::npos);
  REQUIRE(stop_tui(tui));
  CHECK(server.json(server.cli({"status"})).value("state", "") == "running");
  CHECK(server.cli({"disconnect"}).code == 0);
  CHECK(server.stop());
}

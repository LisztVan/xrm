#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/common.hpp"
#include <ftxui/screen/string.hpp>

#include <pty.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <spawn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <iostream>
#include <utility>
#include <vector>

extern char** environ;

namespace {
using namespace xrm;
struct TempDir {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-i52-tui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct Child {
  pid_t pid = -1;
  int master = -1;
  Child() = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  Child(Child&& other) noexcept : pid(std::exchange(other.pid, -1)), master(std::exchange(other.master, -1)) {}
  Child& operator=(Child&& other) noexcept {
    if (this == &other) return *this;
    if (pid > 0) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
    if (master >= 0) ::close(master);
    pid = std::exchange(other.pid, -1);
    master = std::exchange(other.master, -1);
    return *this;
  }
  bool wait(std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (std::chrono::steady_clock::now() < until) {
      if (::waitpid(pid, &status, WNOHANG) == pid) { pid = -1; return WIFEXITED(status) && WEXITSTATUS(status) == 0; }
      // A full PTY output buffer can block FTXUI's render thread before it
      // handles the queued quit key. Drain frames while waiting for exit.
      pollfd output{master, POLLIN, 0};
      if (master >= 0 && ::poll(&output, 1, 10) > 0 && (output.revents & POLLIN)) {
        char discard[8192];
        (void)::read(master, discard, sizeof(discard));
      }
    }
    return false;
  }
  ~Child() {
    if (pid > 0) { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
    if (master >= 0) ::close(master);
  }
};

// tmux holds the current 80x24 terminal grid, including styling. PTY read
// buffers below include old redraws, so they cannot prove a row is visible now.
bool visibly_emphasized(std::string_view styled_grid, std::string_view token);

class CapturedTui {
 public:
  explicit CapturedTui(const std::filesystem::path& socket,
                       const std::vector<std::string>& environment = {})
      : tmux_socket_("xrm-acceptance-" + std::to_string(::getpid()) + "-" +
                     std::to_string(sequence_.fetch_add(1))) {
    const auto quote = [](const std::string& value) {
      std::string result = "'";
      for (char c : value) { if (c == '\'') result += "'\\''"; else result += c; }
      return result + "'";
    };
    const auto root = socket.parent_path();
    std::string launch = "exec env";
    for (const auto& name : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"})
      launch += " " + quote(std::string(name) + "=" + (root / name).string());
    for (const auto& value : environment) launch += " " + quote(value);
    launch += " " + quote(XRM_TEST_MANAGER) + " --socket " + quote(socket.string()) + " tui";
    started_ = invoke({"new-session", "-d", "-s", "xrm", "-x", "80", "-y", "24", launch}).first == 0;
  }
  CapturedTui(const CapturedTui&) = delete;
  CapturedTui& operator=(const CapturedTui&) = delete;
  ~CapturedTui() { if (started_) (void)invoke({"kill-server"}); }
  bool started() const { return started_; }
  bool type(std::string_view value) const {
    const bool sent = invoke({"send-keys", "-t", "xrm", "-l", "--", std::string(value)}).first == 0;
    ::poll(nullptr, 0, 60); return sent;
  }
  bool key(std::string_view value) const {
    const bool sent = invoke({"send-keys", "-t", "xrm", std::string(value)}).first == 0;
    ::poll(nullptr, 0, value == "Escape" ? 130 : 45); return sent;
  }
  bool repeat_key(std::string_view value, int count) const {
    const bool sent = invoke({"send-keys", "-t", "xrm", "-N", std::to_string(count), std::string(value)}).first == 0;
    ::poll(nullptr, 0, 60); return sent;
  }
  bool resize(int width, int height) const {
    return invoke({"resize-window", "-t", "xrm", "-x", std::to_string(width),
                   "-y", std::to_string(height)}).first == 0;
  }
  bool top_page(std::string_view title) const {
    constexpr std::array<std::string_view, 7> names{
        "Overview", "Profiles", "Modules", "Subscriptions", "Nodes", "Logs", "Config"};
    (void)wait_for([&](std::string_view screen) {
      return std::any_of(names.begin(), names.end(), [&](std::string_view name) {
        return screen.find("[" + std::string(name) + "]") != std::string_view::npos;
      });
    });
    for (int i = 0; i < 3 && grid().find("TopNav") == std::string::npos; ++i) {
      if (!key("Escape")) return false;
      (void)wait_for([](std::string_view screen) { return screen.find("TopNav") != std::string_view::npos; });
    }
    const auto current = grid();
    auto from = names.size(), to = names.size();
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (current.find("[" + std::string(names[i]) + "]") != std::string::npos) from = i;
      if (names[i] == title) to = i;
    }
    if (from == names.size() || to == names.size()) return false;
    for (std::size_t i = 0; i < (to + names.size() - from) % names.size(); ++i)
      if (!key("Tab")) return false;
    return wait_for([&](std::string_view screen) {
      return screen.find("[" + std::string(title) + "]") != std::string_view::npos;
    }).find("[" + std::string(title) + "]") != std::string::npos;
  }
  bool secondary_section(std::string_view name) const {
    if (grid().find("TopNav") != std::string::npos && !key("Enter")) return false;
    (void)wait_for([](std::string_view screen) { return screen.find("SecondaryNav") != std::string_view::npos; });
    if (grid().find("SecondaryNav") == std::string::npos) return false;
    for (int i = 0; i < 32; ++i) {
      const auto before = grid();
      if (visibly_emphasized(grid(true), " " + std::string(name))) return true;
      if (!key("Tab")) return false;
      (void)wait_for([&](std::string_view current) { return current != before; },
                     std::chrono::seconds(1));
    }
    return false;
  }
  bool content() const {
    (void)wait_for([](std::string_view screen) { return screen.find("TopNav") != std::string_view::npos || screen.find("SecondaryNav") != std::string_view::npos || screen.find("Content") != std::string_view::npos; });
    if (grid().find("TopNav") != std::string::npos && !key("Enter")) return false;
    auto current = wait_for([](std::string_view screen) {
      return screen.find("SecondaryNav") != std::string_view::npos ||
             screen.find("Content") != std::string_view::npos;
    });
    if (current.find("SecondaryNav") != std::string::npos) {
      if (!key("Enter")) return false;
      current = wait_for([](std::string_view screen) { return screen.find("Content") != std::string_view::npos; });
    }
    return current.find("Content") != std::string::npos;
  }
  std::string grid(bool styled = false) const {
    auto args = std::vector<std::string>{"capture-pane", "-p", "-t", "xrm"};
    if (styled) args.push_back("-e");
    return invoke(args).second;
  }
  std::string wait_for(const std::function<bool(std::string_view)>& expected,
                       std::chrono::milliseconds timeout = std::chrono::seconds(8)) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string current;
    do {
      current = grid();
      if (expected(current)) return current;
      ::poll(nullptr, 0, 30);
    } while (std::chrono::steady_clock::now() < deadline);
    return current;
  }

 private:
  std::pair<int, std::string> invoke(const std::vector<std::string>& arguments) const {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return {-1, {}};
    std::vector<std::string> values{"tmux", "-L", tmux_socket_, "-f", "/dev/null"};
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    pid_t child = -1;
    const int launched = ::posix_spawnp(&child, "tmux", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    std::string output;
    if (launched == 0) {
      char bytes[4096];
      ssize_t count;
      while ((count = ::read(pipe_fds[0], bytes, sizeof(bytes))) > 0)
        output.append(bytes, static_cast<std::size_t>(count));
    }
    ::close(pipe_fds[0]);
    if (launched != 0) return {-1, std::move(output)};
    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) return {-1, std::move(output)};
    return {WEXITSTATUS(status), std::move(output)};
  }
  static inline std::atomic<unsigned> sequence_{0};
  std::string tmux_socket_;
  bool started_ = false;
};

std::string read_screen(int fd, std::chrono::milliseconds wait = std::chrono::milliseconds(180)) {
  std::string result;
  const auto until = std::chrono::steady_clock::now() + wait;
  while (std::chrono::steady_clock::now() < until) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 10) <= 0) continue;
    char bytes[8192];
    const auto count = ::read(fd, bytes, sizeof(bytes));
    if (count <= 0) break;
    result.append(bytes, static_cast<std::size_t>(count));
  }
  return result;
}
void keys(int fd, std::string_view value) {
  REQUIRE(::write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size()));
  ::poll(nullptr, 0, 120);
}
// Drive the public layered keyboard route. Callers state the page currently
// shown, so this never reintroduces a hidden numeric test-only shortcut.
void navigate_top(int fd, int from, int to) {
  const int tabs = (to + 7 - from) % 7;
  for (int i = 0; i < 4; ++i) {
    keys(fd, "\x1b");
    if (i != 3 || tabs != 0) (void)read_screen(fd, std::chrono::milliseconds(80));
    ::poll(nullptr, 0, 250); // Let the terminal disambiguate Esc from Alt+key.
  }
  for (int i = 0; i < tabs; ++i) {
    keys(fd, "\t");
    if (i + 1 != tabs) (void)read_screen(fd, std::chrono::milliseconds(80));
  }
}
void navigate_content(int fd, int from, int to) {
  navigate_top(fd, from, to);
  (void)read_screen(fd, std::chrono::milliseconds(80));
  keys(fd, "\r");
  if (to == 2 || to == 4 || to == 6) {
    (void)read_screen(fd, std::chrono::milliseconds(80));
    keys(fd, "\r");
  }
}

class Daemon {
 public:
  explicit Daemon(const std::filesystem::path& path) : socket_(path / "xrmd.sock"), db_(path / "state.sqlite") {}
  bool start(const std::vector<std::string>& environment = {}) {
    std::vector<std::string> args{XRM_TEST_SERVER, "--foreground", "--socket", socket_.string(), "--database", db_.string()};
    std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
    auto env = child_environment(environment);
    if (::posix_spawn(&pid_, XRM_TEST_SERVER, nullptr, nullptr, argv.data(), env.data()) != 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un addr{}; addr.sun_family = AF_UNIX;
      const auto name = socket_.string(); std::copy(name.begin(), name.end(), addr.sun_path);
      const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
      ::close(fd);
      if (ok) return true;
      ::poll(nullptr, 0, 10);
    }
    return false;
  }
  const std::filesystem::path& socket() const { return socket_; }
  void stop() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGTERM);
    ::waitpid(pid_, nullptr, 0);
    pid_ = -1;
  }
  ~Daemon() { stop(); }
 private:
  std::vector<char*> child_environment(const std::vector<std::string>& additions) {
    std::vector<std::string> values;
    for (char** item = environ; item && *item; ++item) {
      const std::string current(*item);
      const bool replaced = std::any_of(additions.begin(), additions.end(), [&](const auto& addition) {
        const auto equals = addition.find('=');
        return equals != std::string::npos && current.starts_with(addition.substr(0, equals + 1));
      });
      if (!replaced) values.push_back(current);
    }
    values.insert(values.end(), additions.begin(), additions.end());
    std::vector<char*> result;
    result.reserve(values.size() + 1);
    for (auto& value : values) result.push_back(value.data());
    result.push_back(nullptr);
    // Keep the backing strings alive for posix_spawn's duration.
    environment_storage_ = std::move(values);
    result.clear();
    for (auto& value : environment_storage_) result.push_back(value.data());
    result.push_back(nullptr);
    return result;
  }
  std::filesystem::path socket_, db_;
  std::vector<std::string> environment_storage_;
  pid_t pid_ = -1;
};

std::string cli(const std::filesystem::path& socket, std::vector<std::string> args) {
  auto out = std::filesystem::temp_directory_path() / ("xrm-i52-cli-" + std::to_string(getpid()));
  std::vector<std::string> values{XRM_TEST_MANAGER, "--socket", socket.string(), "--json"};
  values.insert(values.end(), args.begin(), args.end());
  std::vector<char*> argv; for (auto& value : values) argv.push_back(value.data()); argv.push_back(nullptr);
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1; const int rc = ::posix_spawn(&pid, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc) return {};
  int status = 0;
  for (int i = 0; i < 1000; ++i) {
    if (::waitpid(pid, &status, WNOHANG) == pid) break;
    ::poll(nullptr, 0, 10);
  }
  if (::waitpid(pid, &status, WNOHANG) == 0) { ::kill(pid, SIGKILL); ::waitpid(pid, &status, 0); }
  std::ifstream input(out, std::ios::binary);
  std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  std::error_code ec; std::filesystem::remove(out, ec);
  return text;
}

Json raw_rpc(const std::filesystem::path& socket, std::string method, Json params) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return Json::object();
  sockaddr_un address{}; address.sun_family = AF_UNIX;
  const auto path = socket.string();
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return Json::object(); }
  const auto frame = Json{{"v", 1}, {"id", 1}, {"method", std::move(method)}, {"params", std::move(params)}}.dump() + "\n";
  std::size_t sent = 0;
  while (sent < frame.size()) {
    const auto count = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
    if (count <= 0) { ::close(fd); return Json::object(); }
    sent += static_cast<std::size_t>(count);
  }
  std::string response;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline && response.find('\n') == std::string::npos) {
    pollfd readiness{fd, POLLIN, 0};
    if (::poll(&readiness, 1, 50) <= 0) continue;
    char bytes[4096];
    const auto count = ::recv(fd, bytes, sizeof(bytes), 0);
    if (count <= 0) break;
    response.append(bytes, static_cast<std::size_t>(count));
  }
  ::close(fd);
  try { return Json::parse(response.substr(0, response.find('\n'))); }
  catch (...) { return Json::object(); }
}

void set_profile_process_environment(const std::filesystem::path& socket,
                                     const std::filesystem::path& data_root,
                                     const std::string& profile_id,
                                     Json environment) {
  const auto path = data_root / "profiles" / profile_id / "profile.json";
  std::ifstream input(path, std::ios::binary);
  const std::string original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  auto profile = Json::parse(cli(socket, {"profile", "show", profile_id}));
  profile["environment"]["process_environment"] = std::move(environment);
  const auto response = raw_rpc(socket, "profile.save", Json{
      {"profile_id", profile_id}, {"settings", std::move(profile)}, {"expected_text", original}});
  REQUIRE(response.value("ok", false));
}

Child start_tui(const std::filesystem::path& socket, const std::string& profile,
                const std::vector<std::string>& environment = {}) {
  Child child;
  int slave = -1;
  if (::openpty(&child.master, &slave, nullptr, nullptr, nullptr) != 0) return child;
  winsize size{24, 80, 0, 0}; (void)::ioctl(slave, TIOCSWINSZ, &size);
  std::vector<std::string> args{XRM_TEST_MANAGER, "--socket", socket.string(), "tui"};
  if (!profile.empty()) { args.push_back("--profile"); args.push_back(profile); }
  std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  std::vector<std::string> env_values;
  for (char** item = environ; item && *item; ++item) {
    const std::string current(*item);
    const bool replaced = std::any_of(environment.begin(), environment.end(), [&](const auto& addition) {
      const auto equals = addition.find('=');
      return equals != std::string::npos && current.starts_with(addition.substr(0, equals + 1));
    });
    if (!replaced) env_values.push_back(current);
  }
  env_values.insert(env_values.end(), environment.begin(), environment.end());
  std::vector<char*> envp;
  for (auto& value : env_values) envp.push_back(value.data());
  envp.push_back(nullptr);
  const int rc = ::posix_spawn(&child.pid, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions); ::close(slave);
  if (rc != 0) { child.pid = -1; ::close(child.master); child.master = -1; }
  return child;
}

class LocalSubscriptionServer {
 public:
  explicit LocalSubscriptionServer(std::string body,
                                   std::chrono::milliseconds delay = std::chrono::milliseconds(0),
                                   int status = 200)
      : body_(std::move(body)), delay_(delay), status_(status) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return;
    int reuse = 1; (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd_, 4) != 0) { ::close(fd_); fd_ = -1; return; }
    socklen_t length = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) { ::close(fd_); fd_ = -1; return; }
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] {
      while (!stop_.load()) {
        pollfd ready{fd_, POLLIN, 0};
        if (::poll(&ready, 1, 50) <= 0) continue;
        const int client = ::accept(fd_, nullptr, nullptr);
        if (client < 0) continue;
        char request[4096]; (void)::recv(client, request, sizeof(request), 0);
        if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
        const auto response = "HTTP/1.1 " + std::to_string(status_) + (status_ == 200 ? " OK\r\n" : " Forbidden\r\n") +
            "Content-Type: text/plain\r\nConnection: close\r\nContent-Length: " +
            std::to_string(body_.size()) + "\r\n\r\n" + body_;
        std::size_t sent = 0;
        while (sent < response.size()) {
          const auto count = ::send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
          if (count <= 0) break;
          sent += static_cast<std::size_t>(count);
        }
        ::close(client);
      }
    });
  }
  LocalSubscriptionServer(const LocalSubscriptionServer&) = delete;
  LocalSubscriptionServer& operator=(const LocalSubscriptionServer&) = delete;
  ~LocalSubscriptionServer() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    if (fd_ >= 0) ::close(fd_);
  }
  bool valid() const { return fd_ >= 0; }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/nodes"; }
 private:
  int fd_ = -1;
  unsigned short port_ = 0;
  std::string body_;
  std::chrono::milliseconds delay_;
  int status_ = 200;
  std::atomic_bool stop_{false};
  std::thread worker_;
};

bool valid_utf8(std::string_view value) {
  for (std::size_t i = 0; i < value.size();) {
    const auto c = static_cast<unsigned char>(value[i]);
    std::size_t n = c < 0x80 ? 1 : (c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0);
    if (!n || i + n > value.size()) return false;
    for (std::size_t j = 1; j < n; ++j) if ((static_cast<unsigned char>(value[i+j]) & 0xc0) != 0x80) return false;
    i += n;
  }
  return true;
}
std::string terminal_text(std::string_view raw) {
  std::string text;
  for (std::size_t index = 0; index < raw.size();) {
    if (raw[index] == '\0') { ++index; continue; }
    if (raw[index] == '\x1b') {
      ++index;
      if (index < raw.size() && raw[index] == '[') {
        ++index;
        while (index < raw.size()) {
          const auto byte = static_cast<unsigned char>(raw[index++]);
          if (byte >= 0x40 && byte <= 0x7e) break;
        }
      } else if (index < raw.size() && raw[index] == 'P') {
        ++index;
        while (index < raw.size() && !(raw[index] == '\x1b' && index + 1 < raw.size() && raw[index + 1] == '\\')) ++index;
        index = std::min(raw.size(), index + 2);
      } else if (index < raw.size()) ++index;
      continue;
    }
    if (raw[index] != '\r') text.push_back(raw[index]);
    ++index;
  }
  return text;
}
template <typename Predicate>
std::string wait_screen_text(int fd, Predicate ready,
                             std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
  std::string result;
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < until) {
    result += terminal_text(read_screen(fd, std::chrono::milliseconds(50)));
    if (ready(result)) break;
  }
  return result;
}
std::optional<unsigned> log_view_top(std::string_view screen) {
  constexpr std::string_view prefix = "Viewport top: XRM_FAKE_CORE_LOG_";
  const auto at = screen.rfind(prefix);
  if (at == std::string_view::npos) return std::nullopt;
  const auto digits = screen.substr(at + prefix.size(), 4);
  try { return static_cast<unsigned>(std::stoul(std::string(digits))); }
  catch (...) { return std::nullopt; }
}

bool visible_selected_row(std::string_view grid, std::string_view name) {
  for (std::size_t first = 0; first < grid.size();) {
    const auto last = grid.find('\n', first);
    const auto line = grid.substr(first, last == std::string_view::npos ? last : last - first);
    if ((line.find("> ") != std::string_view::npos || line.find("▸") != std::string_view::npos) && line.find(name) != std::string_view::npos)
      return true;
    if (last == std::string_view::npos) break;
    first = last + 1;
  }
  return false;
}

bool visibly_emphasized(std::string_view styled_grid, std::string_view label) {
  const auto at = styled_grid.find(label);
  if (at == std::string_view::npos) return false;
  bool inverted = false;
  bool bold = false;
  for (std::size_t position = 0; position < at;) {
    const auto start = styled_grid.find("\x1b[", position);
    if (start == std::string_view::npos || start >= at) break;
    const auto end = styled_grid.find('m', start + 2);
    if (end == std::string_view::npos || end >= at) break;
    const auto parameters = styled_grid.substr(start + 2, end - start - 2);
    std::size_t first = 0;
    while (first <= parameters.size()) {
      const auto next = parameters.find(';', first);
      const auto part = parameters.substr(first, next == std::string_view::npos ? next : next - first);
      if (part.empty() || part == "0" || part == "27") inverted = false;
      if (part == "7") inverted = true;
      if (part.empty() || part == "0" || part == "22") bold = false;
      if (part == "1") bold = true;
      if (next == std::string_view::npos) break;
      first = next + 1;
    }
    position = end + 1;
  }
  return inverted || bold;
}
void expect_text(const CapturedTui& tui, std::string_view text) {
  const auto grid = tui.wait_for([&](std::string_view current) { return current.find(text) != std::string_view::npos; });
  INFO("expected=" << text << "\n" << grid); REQUIRE(grid.find(text) != std::string::npos);
}
// Receipts are transient overlays. Verify their durable history through the
// public Help action instead of depending on a toast surviving PTY polling.
void expect_notification(const CapturedTui& tui, std::string_view message) {
  REQUIRE(tui.key("F1")); expect_text(tui, "Help & actions");
  REQUIRE(tui.key("Home"));
  bool selected = false;
  for (int i = 0; i < 40; ++i) {
    if (visible_selected_row(tui.grid(), "Notification history")) { selected = true; break; }
    REQUIRE(tui.key("Down"));
  }
  REQUIRE(selected); REQUIRE(tui.key("Enter"));
  REQUIRE(tui.key("End")); // The newest receipt can be below the dialog viewport.
  expect_text(tui, message); REQUIRE(tui.key("Escape"));
}
void replace_input(const CapturedTui& tui, std::string_view value, int max_chars = 512) {
  REQUIRE(tui.key("Home")); REQUIRE(tui.repeat_key("Delete", max_chars)); REQUIRE(tui.type(value));
}
void edit_module(const CapturedTui& tui, std::string_view module) {
  REQUIRE(tui.top_page("Profiles")); REQUIRE(tui.content()); REQUIRE(tui.type("e"));
  expect_text(tui, "Module Edit enabled");
  REQUIRE(tui.top_page("Modules")); REQUIRE(tui.secondary_section(module)); REQUIRE(tui.content());
  expect_text(tui, "Source editable");
}
} // namespace

TEST_CASE("TUI navigation shows one matching selected tab in the current terminal grid", "[UI-NAV][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  const std::array<std::pair<std::string_view, std::string_view>, 7> pages{{
      {"Overview", "Runtime"}, {"Profiles", "Profiles"},
      {"Modules", "View a Profile"}, {"Subscriptions", "No subscriptions"},
      {"Nodes", "No nodes"}, {"Logs", "Following"}, {"Config", "Config"},
  }};
  for (std::size_t index = 0; index < pages.size(); ++index) {
    const auto& [title, body] = pages[index];
    REQUIRE(tui.top_page(title));
    const auto label = "[" + std::string(title) + "]";
    const auto grid = tui.wait_for([&](std::string_view value) {
      return value.find(label) != std::string_view::npos && value.find(body) != std::string_view::npos;
    });
    INFO("page=" << index + 1 << " grid=" << grid);
    REQUIRE(grid.find(label) != std::string::npos);
    REQUIRE(grid.find(body) != std::string::npos);
    CHECK(visibly_emphasized(tui.grid(true), label));
  }
  REQUIRE(tui.key("Tab"));
  const auto wrapped = tui.wait_for([](std::string_view grid) {
    return grid.find("[Overview]") != std::string_view::npos;
  });
  CHECK(wrapped.find("Runtime") != std::string::npos);
  REQUIRE(tui.key("BTab"));
  const auto reversed = tui.wait_for([](std::string_view grid) {
    return grid.find("[Config]") != std::string_view::npos;
  });
  CHECK(reversed.find("Config") != std::string::npos);
  REQUIRE(tui.resize(50, 24));
  const auto narrow = tui.wait_for([](std::string_view grid) {
    return grid.find("Terminal too small") != std::string_view::npos;
  });
  CHECK(narrow.find("Terminal too small") != std::string::npos);
  REQUIRE(tui.resize(80, 24));
  REQUIRE(tui.wait_for([](std::string_view grid) {
    return grid.find("[Config]") != std::string_view::npos;
  }).find("[Config]") != std::string::npos);
  REQUIRE(tui.secondary_section("Appearance"));
  const auto appearance = tui.wait_for([](std::string_view grid) {
    return grid.find("Appearance") != std::string_view::npos &&
           grid.find("theme") != std::string_view::npos;
  });
  CHECK(appearance.find("[Config]") != std::string::npos);
  REQUIRE(tui.top_page("Nodes"));
  REQUIRE(tui.secondary_section("Groups"));
  const auto groups = tui.wait_for([](std::string_view grid) {
    return grid.find("[Nodes]") != std::string_view::npos &&
           grid.find("Groups") != std::string_view::npos;
  });
  CHECK(groups.find("[Nodes]") != std::string::npos);
  REQUIRE(tui.resize(80, 24));
  REQUIRE(tui.type("q"));
}

TEST_CASE("P1 layered Tab enters secondary navigation and keeps both bars at the top", "[P1-FOCUS][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.top_page("Nodes"));
  auto grid = tui.wait_for([](std::string_view value) {
    return value.find("[Nodes]") != std::string_view::npos &&
           value.find("TopNav") != std::string_view::npos;
  });
  REQUIRE(grid.find("TopNav") != std::string::npos);
  REQUIRE(tui.key("Enter"));
  grid = tui.wait_for([](std::string_view value) {
    return value.find("SecondaryNav") != std::string_view::npos;
  });
  REQUIRE(grid.find("[Nodes]") != std::string::npos);
  REQUIRE(tui.key("Tab"));
  grid = tui.wait_for([](std::string_view value) {
    return value.find("SecondaryNav") != std::string_view::npos &&
           value.find("[Nodes]") != std::string_view::npos &&
           value.find("Local") != std::string_view::npos;
  });
  CHECK(grid.find("[Nodes]") != std::string::npos);
  REQUIRE(tui.key("Enter"));
  grid = tui.wait_for([](std::string_view value) {
    return value.find("Content") != std::string_view::npos;
  });
  REQUIRE(grid.find("[Nodes]") != std::string::npos);
  REQUIRE(tui.key("Escape"));
  CHECK(tui.wait_for([](std::string_view value) {
    return value.find("SecondaryNav") != std::string_view::npos;
  }).find("SecondaryNav") != std::string::npos);
  REQUIRE(tui.key("Escape"));
  CHECK(tui.wait_for([](std::string_view value) {
    return value.find("TopNav") != std::string_view::npos;
  }).find("TopNav") != std::string::npos);
}

TEST_CASE("P1 terminal below 80x24 protects and restores the current session", "[P1-SIZE][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.top_page("Nodes"));
  REQUIRE(tui.wait_for([](std::string_view value) {
    return value.find("[Nodes]") != std::string_view::npos;
  }).find("[Nodes]") != std::string::npos);
  REQUIRE(tui.resize(70, 20));
  const auto small = tui.wait_for([](std::string_view value) {
    return value.find("Terminal too small") != std::string_view::npos &&
           value.find("70x20") != std::string_view::npos;
  });
  INFO("small grid: " << small);
  REQUIRE(small.find("Terminal too small") != std::string::npos);
  REQUIRE(tui.resize(120, 35));
  const auto restored = tui.wait_for([](std::string_view value) {
    return value.find("[Nodes]") != std::string_view::npos &&
           value.find("Terminal too small") == std::string_view::npos;
  });
  INFO("restored grid: " << restored);
  CHECK(restored.find("[Nodes]") != std::string::npos);
}

TEST_CASE("P1 node table header and cells share display columns", "[P1-TABLE][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto added = cli(daemon.socket(), {"node", "add", "--name", "Short",
      "--outbound-json", R"({"protocol":"freedom","settings":{}})"});
  REQUIRE_FALSE(added.empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.top_page("Nodes"));
  REQUIRE(tui.secondary_section("Local"));
  const auto grid = tui.wait_for([](std::string_view value) {
    return value.find("Node") != std::string_view::npos &&
           value.find("Short") != std::string_view::npos;
  });
  const auto header_start = grid.rfind('\n', grid.find("Protocol"));
  const auto row_start = grid.rfind('\n', grid.find("Short"));
  REQUIRE(header_start != std::string::npos);
  REQUIRE(row_start != std::string::npos);
  const auto name_column = ftxui::string_width(std::string_view(grid).substr(
      header_start + 1, grid.find("Node", header_start) - header_start - 1));
  const auto value_column = ftxui::string_width(std::string_view(grid).substr(
      row_start + 1, grid.find("Short", row_start) - row_start - 1));
  CHECK(name_column == value_column);

  REQUIRE_FALSE(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE,
                                     "--name", "table-core"}).empty());
  REQUIRE(tui.top_page("Config")); REQUIRE(tui.content()); REQUIRE(tui.type("r"));
  const auto core_grid = tui.wait_for([](std::string_view value) {
    return value.find("table-core") != std::string_view::npos &&
           value.find("registered") != std::string_view::npos &&
           value.find("history in Help") == std::string_view::npos;
  });
  INFO(core_grid);
  const auto core_line_start = core_grid.rfind('\n', core_grid.find("table-core"));
  REQUIRE(core_line_start != std::string::npos);
  const auto core_line_end = core_grid.find('\n', core_line_start + 1);
  const auto core_line = core_grid.substr(core_line_start + 1, core_line_end - core_line_start - 1);
  const auto origin_at = core_line.find("registered");
  REQUIRE(origin_at != std::string::npos);
  CHECK((origin_at > 0 && core_line[origin_at - 1] == ' ')); // Long version must not bleed into State.
}

TEST_CASE("P3 Profile creation edits name core and preset in one form", "[P3-TEMPLATE][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.top_page("Profiles")); REQUIRE(tui.content()); REQUIRE(tui.type("n"));
  expect_text(tui, "new Profile");
  REQUIRE(tui.type("Custom Draft")); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Tab"));
  REQUIRE(tui.key("Home")); REQUIRE(tui.key("Down")); REQUIRE(tui.key("Enter"));
  expect_text(tui, "Custom (all modules disabled)"); REQUIRE(tui.key("C-s"));
  expect_text(tui, "Custom Draft saved");
  const auto profiles = Json::parse(cli(daemon.socket(), {"profile", "list"})); REQUIRE(profiles.size() == 1);
  const auto modules = Json::parse(cli(daemon.socket(), {"config", "modules", profiles.front().at("id").get<std::string>()}));
  REQUIRE(modules.size() == 15);
  CHECK(std::all_of(modules.begin(), modules.end(), [](const Json& item) { return !item.value("included", false); }));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("P3 Modules secondary tabs show the selected native source without opening an editor", "[P3-MODULE-TABS][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto created = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Desk"})); REQUIRE(created.contains("id"));
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Modules"));
  expect_text(tui, "Xray logging"); REQUIRE(tui.secondary_section("dns"));
  expect_text(tui, "DNS servers"); expect_text(tui, "Source read-only");
  CHECK(visibly_emphasized(tui.grid(true), " dns "));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("P3 module key selector keeps a far-right native tab visible at 80 columns", "[P3-MODULE-NAV][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  REQUIRE_FALSE(cli(daemon.socket(), {"profile", "create", "--name", "Desk"}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Modules"));
  REQUIRE(tui.secondary_section("burstObservatory"));
  expect_text(tui, "Burst observatory");
  CHECK(visibly_emphasized(tui.grid(true), " burstObservatory"));
  REQUIRE(tui.resize(120, 40)); CHECK(visibly_emphasized(tui.grid(true), " burstObservatory"));
}

TEST_CASE("P3 Modules runtime toggle does not silently render or start a stopped Profile", "[P3-RUNTIME][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  REQUIRE_FALSE(cli(daemon.socket(), {"profile", "create", "--name", "Desk"}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Modules")); REQUIRE(tui.content());
  REQUIRE(tui.type("v")); expect_text(tui, "Runtime belongs to none");
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("P3 semantic module form changes the same unsaved native draft", "[P3-STRUCT][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto created = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Desk"}));
  const auto id = created.at("id").get<std::string>();
  const std::string original = R"({"log":{"loglevel":"warning","iterationFlag":true}})";
  REQUIRE_FALSE(cli(daemon.socket(), {"config", "edit", id, "log", "--text", original}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); edit_module(tui, "log");
  REQUIRE(tui.key("Enter")); expect_text(tui, "loglevel");
  REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Home")); REQUIRE(tui.repeat_key("Down", 2)); REQUIRE(tui.key("Enter"));
  REQUIRE(tui.key("C-s")); expect_text(tui, "Unsaved *");
  CHECK(Json::parse(cli(daemon.socket(), {"config", "show", id, "--module-key", "log", "--source", "source"})).at("text") == original);
  REQUIRE(tui.key("F2")); expect_text(tui, "iterationFlag");
  REQUIRE(tui.key("C-s")); expect_notification(tui, "Saved Desk / log");
  const auto saved = Json::parse(cli(daemon.socket(), {"config", "show", id, "--module-key", "log", "--source", "source"}));
  const auto body = Json::parse(saved.at("text").get<std::string>());
  CHECK(body.at("log").at("iterationFlag") == true); CHECK(body.at("log").at("loglevel") == "info");
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("P5 routing preset is previewed before saving native routing source", "[P5-PRESET-UI][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Desk"}));
  const auto id = profile.at("id").get<std::string>();
  auto source = [&] { return Json::parse(cli(daemon.socket(), {"config", "show", id, "--module-key", "routing", "--source", "source"})).at("text").get<std::string>(); };
  const auto before = source();
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); edit_module(tui, "routing");
  auto preview = [&] {
    REQUIRE(tui.type("p")); expect_text(tui, "Routing preset");
    REQUIRE(tui.key("Home")); REQUIRE(tui.repeat_key("Down", 3)); REQUIRE(tui.key("Enter")); REQUIRE(tui.key("C-s"));
    expect_text(tui, "Review routing changes");
  };
  preview(); CHECK(source() == before); REQUIRE(tui.key("Escape")); CHECK(source() == before);
  preview(); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Enter")); expect_text(tui, "Routing preset saved");
  const auto saved = source(); CHECK(saved != before); CHECK(Json::parse(saved).at("routing").at("rules").is_array());
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("P5 Config Geodata adds a shared resource without changing the running core", "[P5-GEODATA-UI][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Config")); REQUIRE(tui.secondary_section("Geodata")); REQUIRE(tui.content());
  REQUIRE(tui.type("a")); expect_text(tui, "Add Geodata resource");
  REQUIRE(tui.type("China IP")); REQUIRE(tui.repeat_key("Tab", 4)); REQUIRE(tui.type("http://127.0.0.1:9/geoip.dat"));
  REQUIRE(tui.key("C-s")); expect_text(tui, "China IP saved");
  const auto resources = raw_rpc(daemon.socket(), "geodata.list", Json::object());
  REQUIRE(resources.value("ok", false)); REQUIRE(resources.at("result").size() == 1);
  CHECK(resources.at("result").front().value("name", "") == "China IP");
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("P5 Config Geodata validates and binds a local asset from the real TUI", "[P5-GEODATA-UI][TUI][real_core]") {
  const char* binary = std::getenv("XRM_TEST_XRAY");
  if (!binary || !std::filesystem::exists(binary)) SKIP("fixed Xray unavailable");
  auto varint = [](std::size_t value) {
    std::string out;
    while (value >= 128) { out.push_back(static_cast<char>((value & 0x7f) | 0x80)); value >>= 7; }
    out.push_back(static_cast<char>(value));
    return out;
  };
  auto field = [&](int number, std::string bytes) {
    return varint(static_cast<std::size_t>((number << 3) | 2)) + varint(bytes.size()) + bytes;
  };
  const auto cidr = field(1, std::string{"\x7f\0\0\0", 4}) + "\x10\x08";
  TempDir temp;
  const auto asset = temp.path / "geoip.dat";
  { std::ofstream output(asset, std::ios::binary);
    output << field(1, field(1, "XRM-TEST") + field(2, cidr)); }
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto core = Json::parse(cli(daemon.socket(), {"core", "register", binary, "--name", "fixed-geodata"}));
  REQUIRE(core.contains("id"));
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "geo-ui",
      "--core", core.at("id").get<std::string>()}));
  REQUIRE(profile.contains("id"));
CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Config")); REQUIRE(tui.secondary_section("Geodata")); REQUIRE(tui.content());
  REQUIRE(tui.type("a")); expect_text(tui, "Add Geodata resource"); REQUIRE(tui.type("Geo local")); REQUIRE(tui.repeat_key("Tab", 3));
  REQUIRE(tui.key("Down")); REQUIRE(tui.key("Enter")); REQUIRE(tui.key("Tab")); REQUIRE(tui.type(asset.string())); REQUIRE(tui.key("C-s")); expect_text(tui, "Geo local saved");
  REQUIRE(tui.type("u")); expect_text(tui, "Update resource: completed"); REQUIRE(tui.type("b")); expect_text(tui, "Geodata for geo-ui");
  REQUIRE(tui.key("Space")); REQUIRE(tui.key("C-s")); expect_text(tui, "bindings saved");
  const auto shown = Json::parse(cli(daemon.socket(), {"profile", "show", profile.at("id").get<std::string>()}));
  CHECK(shown.at("geodata_resource_ids").contains("geoip.dat"));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");

}

TEST_CASE("P4 TCP test is an explicit RPC operation and leaves runtime stopped", "[P4-RPC][TUI]") {
  TempDir temp;
  LocalSubscriptionServer endpoint("marker");
  REQUIRE(endpoint.valid());
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto endpoint_url = endpoint.url();
  const auto colon = endpoint_url.find_last_of(':');
  auto uri = "vless://00000000-0000-4000-8000-000000000111@127.0.0.1:" +
      endpoint_url.substr(colon + 1, endpoint_url.find('/', colon) - colon - 1) +
      "?type=tcp&security=none#Test";
  const auto node = Json::parse(cli(daemon.socket(), {"node", "add", "--uri", uri}));
  REQUIRE(node.contains("id"));
  const auto started = raw_rpc(daemon.socket(), "node.test.start",
      Json{{"node_id", node.at("id")}, {"kind", "tcp"}, {"timeout_ms", 3000}});
  REQUIRE(started.value("ok", false));
  REQUIRE(started.at("result").contains("operation_id"));
  const auto operation_id = started.at("result").at("operation_id").get<std::string>();
  Json completed;
  for (int i = 0; i < 100; ++i) {
    completed = raw_rpc(daemon.socket(), "operation.get", Json{{"operation_id", operation_id}});
    if (completed.value("ok", false) &&
        completed.at("result").value("status", "") != "running") break;
    ::poll(nullptr, 0, 20);
  }
  REQUIRE(completed.value("ok", false));
  CHECK(completed.at("result").value("status", "") == "succeeded");
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("P4 and P5 resources live in Config secondary navigation", "[P4-CONFIG-TABS][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.top_page("Config"));
  const auto grid = tui.wait_for([](std::string_view value) {
    return value.find("Geodata") != std::string_view::npos &&
           value.find("Tests") != std::string_view::npos &&
           value.find("General") != std::string_view::npos;
  });
  CHECK(grid.find("Core") != std::string::npos);
  CHECK(grid.find("Geodata") != std::string::npos);
  CHECK(grid.find("Tests") != std::string::npos);
}

TEST_CASE("Nodes Add exposes a real Local form and Group action", "[UI-LOCAL][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  REQUIRE_FALSE(cli(daemon.socket(), {"profile", "create", "--name", "Objects"}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.content());
  REQUIRE(tui.type("a")); expect_text(tui, "Add node"); expect_text(tui, "Name"); expect_text(tui, "Protocol");
  REQUIRE(tui.key("Escape")); REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.secondary_section("Groups")); REQUIRE(tui.content());
  REQUIRE(tui.type("a")); expect_text(tui, "Add Group"); expect_text(tui, "Selection mode");
  CHECK(Json::parse(cli(daemon.socket(), {"node", "list"})).value("total", 0) == 0);
}

TEST_CASE("Config Core removes only an unused managed install", "[UI-CORE][CLI]") {
  TempDir temp;
  const auto managed_path = temp.path / "cores" / "v-test" / "xray";
  std::filesystem::create_directories(managed_path.parent_path());
  { std::ofstream output(managed_path); REQUIRE(output.good()); output << "test binary"; }
  auto store = Store::open(temp.path / "state.sqlite"); REQUIRE(store);
  REQUIRE((*store)->put_core(CoreInstall{"managed-v-test", "v-test", managed_path,
                                         "Xray test", true}));
  store->reset();
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "uses-core",
                                                    "--core", "managed-v-test"}));
  REQUIRE(profile.contains("id"));
  {
    CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
    REQUIRE(tui.top_page("Config"));
    const auto grid = tui.wait_for([](std::string_view screen) {
      return screen.find("View uses-core") != std::string_view::npos &&
             screen.find("managed") != std::string_view::npos;
    });
    CHECK(grid.find("managed") != std::string::npos);
    REQUIRE(tui.type("q"));
  }
  CHECK(cli(daemon.socket(), {"core", "remove", "managed-v-test"}).empty());
  CHECK(std::filesystem::exists(managed_path));
  REQUIRE_FALSE(cli(daemon.socket(), {"profile", "show", profile.at("id").get<std::string>()}).empty());
  REQUIRE_FALSE(cli(daemon.socket(), {"profile", "list"}).empty());
  // The Profile reference must be removed through the regular API first.
  CHECK(raw_rpc(daemon.socket(), "profile.remove",
      Json{{"profile_id", profile.at("id").get<std::string>()}}).value("ok", false));
  const auto removed = cli(daemon.socket(), {"core", "remove", "managed-v-test"});
  CHECK_FALSE(removed.empty());
  CHECK_FALSE(std::filesystem::exists(managed_path));
  const auto listed = Json::parse(cli(daemon.socket(), {"core", "list"}));
  CHECK(listed.empty());
}

TEST_CASE("Config appearance and editor preferences persist across Manager sessions without Xray edits",
          "[UI-CONFIG][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Home"}));
  const auto id = profile.at("id").get<std::string>();
  const auto before = raw_rpc(daemon.socket(), "module.get", {{"profile_id", id}, {"module", "routing"}});
  {
    CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Config"));
    REQUIRE(tui.secondary_section("Appearance")); REQUIRE(tui.content()); REQUIRE(tui.type("e")); expect_text(tui, "XRM appearance");
    REQUIRE(tui.key("Home")); REQUIRE(tui.key("Down")); REQUIRE(tui.key("Enter")); REQUIRE(tui.key("C-s")); expect_text(tui, "appearance saved");
    REQUIRE(tui.top_page("Config")); REQUIRE(tui.secondary_section("Editor")); REQUIRE(tui.content()); REQUIRE(tui.type("e"));
    expect_text(tui, "External editor"); REQUIRE(tui.type("/bin/true")); REQUIRE(tui.key("C-s")); expect_text(tui, "External editor saved");
  }
  const auto saved = raw_rpc(daemon.socket(), "settings.get", Json::object()); REQUIRE(saved.value("ok", false));
  CHECK(saved["result"]["appearance"]["theme"] == "dark"); CHECK(saved["result"]["editor"]["argv"] == Json::array({"/bin/true"}));
  CHECK(raw_rpc(daemon.socket(), "module.get", {{"profile_id", id}, {"module", "routing"}})["result"]["text"] == before["result"]["text"]);
  CapturedTui reopened(daemon.socket()); REQUIRE(reopened.started()); REQUIRE(reopened.top_page("Config")); REQUIRE(reopened.secondary_section("Appearance"));
  expect_text(reopened, "XRM Slate"); REQUIRE(reopened.content()); REQUIRE(reopened.type("e")); expect_text(reopened, "XRM appearance");
  REQUIRE(reopened.key("Down")); REQUIRE(reopened.key("Enter")); REQUIRE(reopened.key("Escape")); expect_text(reopened, "Unsaved form");
  REQUIRE(reopened.key("Tab")); REQUIRE(reopened.key("Enter"));
  CHECK(raw_rpc(daemon.socket(), "settings.get", Json::object())["result"]["appearance"]["theme"] == "dark");
  REQUIRE(reopened.top_page("Config")); REQUIRE(reopened.secondary_section("Editor")); expect_text(reopened, "/bin/true");
}

TEST_CASE("UI-12 local VLESS form saves a manual node and Group as a Profile outlet", "[UI-12][TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Profiles")); REQUIRE(tui.content()); REQUIRE(tui.type("n"));
  expect_text(tui, "new Profile"); REQUIRE(tui.type("Home")); REQUIRE(tui.key("C-s")); expect_text(tui, "Home saved");
  REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.secondary_section("Local")); REQUIRE(tui.content()); REQUIRE(tui.type("a")); expect_text(tui, "Add node");
  REQUIRE(tui.type("Local fixture")); REQUIRE(tui.repeat_key("Tab", 2)); replace_input(tui, "127.0.0.1"); REQUIRE(tui.key("Tab")); replace_input(tui, "19001");
  REQUIRE(tui.key("Tab")); REQUIRE(tui.type("00000000-0000-4000-8000-000000000111")); REQUIRE(tui.key("C-s")); expect_text(tui, "Local fixture saved");
  const auto listed = Json::parse(cli(daemon.socket(), {"node", "list", "--limit", "20"})); REQUIRE(listed.value("total", 0) == 1);
  const auto node_id = listed["items"][0].at("id").get<std::string>(); CHECK(listed["items"][0]["source_id"].is_null());
  REQUIRE(tui.type("/")); expect_text(tui, "Filter nodes"); REQUIRE(tui.type("Local")); REQUIRE(tui.key("C-s")); expect_text(tui, "Local fixture");
  REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.secondary_section("Groups")); REQUIRE(tui.content()); REQUIRE(tui.type("a")); expect_text(tui, "Add Group");
  REQUIRE(tui.type("Local group")); REQUIRE(tui.repeat_key("Tab", 4)); REQUIRE(tui.key("Space")); REQUIRE(tui.key("C-s")); expect_text(tui, "Local group saved");
  const auto profiles = Json::parse(cli(daemon.socket(), {"profile", "list"})); REQUIRE(profiles.size() == 1);
  const auto profile_id = profiles[0].at("id").get<std::string>();
  const auto groups = Json::parse(cli(daemon.socket(), {"group", "list", profile_id})); REQUIRE(groups.size() == 1);
  CHECK(groups[0].value("name", "") == "Local group"); CHECK(groups[0]["members"]["node_ids"] == Json::array({node_id}));
  REQUIRE(tui.type("s")); expect_text(tui, "Target saved");
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "show", profile_id}));
  CHECK(profile.at("selected").at("kind") == "group"); CHECK(profile.at("selected").at("id_or_tag") == groups[0].at("id"));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("TUI history preview cancels and confirms selected safe records", "[HISTORY-TUI]") {
  TempDir temp;
  auto store = Store::open(temp.path / "state.sqlite");
  REQUIRE(store);
  Subscription source;
  source.id = "history-source";
  source.name = "History Fixture";
  source.url = "http://127.0.0.1/fixture";
  REQUIRE((*store)->put_source(source));
  Node current;
  current.id = "current";
  current.source_id = source.id;
  current.original_name = "Current Node";
  current.settings = RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
  REQUIRE((*store)->put_node(current));
  Node old = current;
  old.id = "old-safe";
  old.source_missing = true;
  REQUIRE((*store)->put_node(old));
  Node different = old;
  different.id = "old-different";
  different.original_name = "Different Name";
  REQUIRE((*store)->put_node(different));
  store->reset();
  Daemon daemon(temp.path); REQUIRE(daemon.start());
CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Subscriptions")); REQUIRE(tui.content()); expect_text(tui, "History Fixture");
  REQUIRE(tui.type("h")); expect_text(tui, "History cleanup");
  CHECK(Json::parse(cli(daemon.socket(), {"node", "history", "preview", source.id})).value("history_count", 0) == 2);
  REQUIRE(tui.key("Escape"));
  CHECK(Json::parse(cli(daemon.socket(), {"node", "history", "preview", source.id})).value("history_count", 0) == 2);
  REQUIRE(tui.type("h")); expect_text(tui, "History cleanup"); REQUIRE(tui.key("Space"));
  REQUIRE(tui.key("Escape")); expect_text(tui, "Unsaved form"); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Enter"));
  CHECK(Json::parse(cli(daemon.socket(), {"node", "history", "preview", source.id})).value("history_count", 0) == 2);
  REQUIRE(tui.type("h")); expect_text(tui, "History cleanup"); REQUIRE(tui.key("Space")); REQUIRE(tui.key("C-s")); expect_text(tui, "History cleaned");
  const auto after = Json::parse(cli(daemon.socket(), {"node", "history", "preview", source.id}));
  CHECK(after.value("current_count", 0) == 1); CHECK(after.value("history_count", 0) == 1);
  CHECK(after.at("candidates").front().value("id", "") == "old-different");

}

TEST_CASE("TUI native inbound Add requires Edit and preserves a disabled module", "[PROFILE-LOCAL-TUI][port10808]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Legacy"}));
  const auto id = profile.at("id").get<std::string>();

  const std::string original = R"({"inbounds":[]})";
  REQUIRE_FALSE(cli(daemon.socket(), {"config", "edit", id, "inbounds", "--text", original}).empty());
  REQUIRE_FALSE(cli(daemon.socket(), {"config", "disable", id, "inbounds"}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.content()); REQUIRE(tui.type("i"));
  expect_text(tui, "Source read-only"); REQUIRE(tui.key("Enter")); expect_text(tui, "Inbound listeners");
  REQUIRE(tui.type("a"));
  CHECK(raw_rpc(daemon.socket(), "module.get", {{"profile_id", id}, {"module", "inbounds"}})["result"]["text"] == original);
  REQUIRE(tui.key("Escape"));

  edit_module(tui, "inbounds"); REQUIRE(tui.key("Enter")); expect_text(tui, "Inbound listeners");
  REQUIRE(tui.type("a")); expect_text(tui, "protocol"); REQUIRE(tui.key("Space")); REQUIRE(tui.key("Tab")); REQUIRE(tui.type("local-socks")); expect_text(tui, "local-socks"); REQUIRE(tui.key("Tab"));
  REQUIRE(tui.key("Home")); REQUIRE(tui.repeat_key("Down", 4)); REQUIRE(tui.key("Enter")); expect_text(tui, "socks ›");
  REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Space")); REQUIRE(tui.key("Tab")); REQUIRE(tui.type("127.0.0.1")); expect_text(tui, "127.0.0.1"); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Space")); REQUIRE(tui.key("Tab")); REQUIRE(tui.type("10808")); expect_text(tui, "10808");
  REQUIRE(tui.key("C-s")); REQUIRE(tui.key("Escape")); expect_text(tui, "Unsaved *");
  REQUIRE(tui.key("C-s")); expect_notification(tui, "Saved Legacy / inbounds");

  const auto after = raw_rpc(daemon.socket(), "module.get", {{"profile_id", id}, {"module", "inbounds"}}); REQUIRE(after.value("ok", false));
  CHECK_FALSE(after["result"].value("included", true));
  const auto body = Json::parse(after["result"]["text"].get<std::string>());
  REQUIRE(body.at("inbounds").size() == 1); CHECK(body["inbounds"][0]["protocol"] == "socks");
  CHECK(body.at("inbounds").at(0).at("listen") == "127.0.0.1"); CHECK(body.at("inbounds").at(0).at("port") == 10808);
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("TUI local-use entry keeps an existing custom inbound intact", "[PROFILE-LOCAL-TUI]") {
TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Legacy"}));
  const auto id = profile.at("id").get<std::string>();

  const std::string custom = R"({"inbounds":[{"tag":"my-server","listen":"127.0.0.1","port":14001,"protocol":"vless","settings":{"clients":[],"unknown":"kept"},"vendor":{"flag":true}}]})";
  REQUIRE_FALSE(cli(daemon.socket(), {"config", "edit", id, "inbounds", "--text", custom}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());

  edit_module(tui, "inbounds"); REQUIRE(tui.key("Enter")); expect_text(tui, "Inbound listeners");
  REQUIRE(tui.type("a")); expect_text(tui, "protocol"); REQUIRE(tui.key("Space")); REQUIRE(tui.key("Tab")); REQUIRE(tui.type("local-socks")); expect_text(tui, "local-socks"); REQUIRE(tui.key("Tab"));
  REQUIRE(tui.key("Home")); REQUIRE(tui.repeat_key("Down", 4)); REQUIRE(tui.key("Enter")); expect_text(tui, "socks ›");
  REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Space")); REQUIRE(tui.key("Tab")); REQUIRE(tui.type("127.0.0.1")); expect_text(tui, "127.0.0.1"); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Space")); REQUIRE(tui.key("Tab")); REQUIRE(tui.type("10808")); expect_text(tui, "10808");
  REQUIRE(tui.key("C-s")); REQUIRE(tui.key("Escape")); expect_text(tui, "Unsaved *");
  REQUIRE(tui.key("C-s")); expect_notification(tui, "Saved Legacy / inbounds");

  const auto changed = raw_rpc(daemon.socket(), "module.get", {{"profile_id", id}, {"module", "inbounds"}}); REQUIRE(changed.value("ok", false));
  const auto body = Json::parse(changed["result"]["text"].get<std::string>()); REQUIRE(body["inbounds"].size() == 2);
  CHECK(body["inbounds"][0] == Json::parse(custom)["inbounds"][0]); CHECK(body["inbounds"][1]["protocol"] == "socks");
  CHECK_FALSE(raw_rpc(daemon.socket(), "module.save", {{"profile_id", id}, {"module", "inbounds"}, {"text", "{\"inbounds\":[]}"}, {"expected_absent", true}}).value("ok", true));
  CHECK(raw_rpc(daemon.socket(), "module.get", {{"profile_id", id}, {"module", "inbounds"}})["result"]["text"] == changed["result"]["text"]);
}

TEST_CASE("TUI node viewport follows the selected row across a 1205-node subscription", "[UI-SCROLL][TUI]") {
  TempDir temp;
  std::string payload;
  for (unsigned index = 0; index < 1205; ++index) {
    std::ostringstream suffix;
    suffix << std::setw(12) << std::setfill('0') << index + 1;
    std::ostringstream name;
    name << "Node-" << std::setw(4) << std::setfill('0') << index;
    if (index == 5) name << "-中文";
    if (index == 6) name << "-🚀";
    payload += "vless://00000000-0000-4000-8000-" + suffix.str() +
               "@127.0.0.1:" + std::to_string(20000 + index) +
               "?type=tcp&security=none#" + name.str() + "\n";
  }
  LocalSubscriptionServer subscription(payload);
  REQUIRE(subscription.valid());
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto added = Json::parse(cli(daemon.socket(), {"sub", "add", "--name", "scroll fixture",
                                                 "--url", subscription.url()}));
  REQUIRE(added.contains("id"));
  const auto updated = cli(daemon.socket(), {"sub", "update", added.at("id").get<std::string>()});
  INFO("subscription update result: " << updated);
  REQUIRE_FALSE(updated.empty());
  const auto inventory = Json::parse(cli(daemon.socket(), {"node", "list", "--offset", "1200", "--limit", "100"}));
  REQUIRE(inventory.value("total", 0) == 1205);
const auto last_name = inventory.at("items").back().value("name", "");
  const auto first_name = Json::parse(cli(daemon.socket(), {"node", "list", "--limit", "1"}))["items"][0].value("name", "");
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.content());
  expect_text(tui, first_name); REQUIRE(tui.key("End")); expect_text(tui, last_name);
  CHECK(visible_selected_row(tui.grid(), last_name));
  REQUIRE(tui.resize(60, 20)); expect_text(tui, "Terminal too small"); REQUIRE(tui.resize(100, 32)); expect_text(tui, last_name);
  CHECK(visible_selected_row(tui.grid(), last_name));
  REQUIRE(tui.key("PPage"));
  const auto previous = tui.wait_for([&](std::string_view text) { return !visible_selected_row(text, last_name); });
  CHECK_FALSE(visible_selected_row(previous, last_name));
  REQUIRE(tui.key("Home")); expect_text(tui, first_name); CHECK(visible_selected_row(tui.grid(), first_name));
  REQUIRE(tui.key("Up")); CHECK(visible_selected_row(tui.grid(), first_name));
  REQUIRE(tui.key("NPage"));
  CHECK_FALSE(visible_selected_row(tui.wait_for([&](std::string_view text) { return !visible_selected_row(text, first_name); }), first_name));
  REQUIRE(tui.key("End")); expect_text(tui, last_name);
  REQUIRE(tui.top_page("Subscriptions")); REQUIRE(tui.content()); REQUIRE(tui.type("u")); expect_text(tui, "completed");
  REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.content()); expect_text(tui, last_name); CHECK(visible_selected_row(tui.grid(), last_name));
  REQUIRE(tui.type("/")); expect_text(tui, "Filter nodes"); REQUIRE(tui.type(last_name)); REQUIRE(tui.key("C-s")); expect_text(tui, last_name);
  const auto filtered = tui.grid(); CHECK(valid_utf8(filtered)); CHECK_FALSE(visible_selected_row(filtered, first_name)); CHECK(visible_selected_row(tui.grid(), last_name));

}

TEST_CASE("80x24 TUI keyboard flow retains UTF-8 names and q is ordinary filter input", "[UI-01][I52]") {
  TempDir temp;
  auto store = Store::open(temp.path / "state.sqlite"); REQUIRE(store);
  for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{
      {"ascii", "alpha-node"}, {"cn", "中文节点"}, {"emoji", "🚀-node"},
      {"long", "long-" + std::string(110, 'x')}}) {
    Node n; n.id = id; n.original_name = name; n.settings = RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
    REQUIRE((*store)->put_node(n));
  }
  store->reset();
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto core_text = cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE, "--name", "fixture"});
  REQUIRE(core_text.find("fixture") != std::string::npos);
  // The JSON interface is used for stable IDs; registration output is parsed via a local JSON object.
  auto core_json = Json::parse(core_text);
  const auto profile_text = cli(daemon.socket(), {"profile", "create", "--name", "tui", "--core", core_json.at("id").get<std::string>()});
  const auto profile = Json::parse(profile_text).at("id").get<std::string>();
CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); expect_text(tui, "F1");
  REQUIRE(tui.key("F1")); expect_text(tui, "Help"); expect_text(tui, "Tab"); REQUIRE(tui.key("Escape"));
  REQUIRE(tui.key("Tab")); expect_text(tui, "[Profiles]"); REQUIRE(tui.key("BTab")); expect_text(tui, "[Overview]");
  REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.secondary_section("Local")); REQUIRE(tui.content()); expect_text(tui, "alpha-node");
  REQUIRE(tui.type("/")); expect_text(tui, "Filter nodes"); REQUIRE(tui.type("q e u p ? "));
  expect_text(tui, "q e u p ?"); REQUIRE(tui.key("F1")); expect_text(tui, "Help & actions"); REQUIRE(tui.key("Escape"));
  replace_input(tui, "中文"); REQUIRE(tui.key("C-s")); expect_text(tui, "中文节点"); CHECK(valid_utf8(tui.grid()));
  REQUIRE(tui.type("/")); expect_text(tui, "Filter nodes"); replace_input(tui, "node"); REQUIRE(tui.key("C-s")); expect_text(tui, "🚀-node"); CHECK(valid_utf8(tui.grid()));
  REQUIRE(tui.key("End")); REQUIRE(tui.type("r"));
  const auto refreshed = tui.wait_for([](std::string_view screen) { return visible_selected_row(screen, "🚀-node"); });
  CHECK(visible_selected_row(refreshed, "🚀-node")); CHECK(valid_utf8(refreshed));
  REQUIRE(tui.type("/")); expect_text(tui, "Filter nodes"); replace_input(tui, "long"); REQUIRE(tui.key("C-s")); expect_text(tui, "long-"); CHECK(valid_utf8(tui.grid()));
  REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.type("q"));

}

// These cases drive current terminal grids and the component contract. The
// original source/runtime/process invariants remain; sequential prompts and
// old debug footer strings are deliberately not part of the new interface.
namespace {
Json rpc_value(const Daemon& daemon, std::string method, Json params = Json::object()) {
  auto response = raw_rpc(daemon.socket(), std::move(method), std::move(params));
  INFO(response.dump()); REQUIRE(response.value("ok", false)); return response.at("result");
}
std::string core_profile(const Daemon& daemon, const std::string& name) {
  auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE, "--name", "fixture"}));
  auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", name, "--core", core.at("id").get<std::string>()}));
  return profile.at("id").get<std::string>();
}
Json wait_state(const Daemon& daemon, std::string_view state) {
  Json status;
  for (int i=0;i<160;++i) { status=rpc_value(daemon,"runtime.status");if(status.value("state","")==state)return status;::poll(nullptr,0,50); }
  INFO(status.dump()); REQUIRE(status.value("state","")==state); return status;
}
std::string direct_node(const Daemon& daemon, const std::string& name) {
  return rpc_value(daemon,"node.add",{{"name",name},{"outbound",{{"protocol","freedom"},{"settings",Json::object()}}}}).at("id").get<std::string>();
}
void select_direct(const Daemon& daemon,const std::string& profile,const std::string& node) {
  rpc_value(daemon,"connection.select",{{"profile_id",profile},{"target",{{"kind","node"},{"id_or_tag",node}}}});
}
void discard_dialog(const CapturedTui& tui) {
  REQUIRE(tui.key("Escape"));expect_text(tui,"Unsaved");REQUIRE(tui.key("Tab"));REQUIRE(tui.key("Enter"));
}
std::optional<unsigned> first_log(std::string_view grid) {
  auto pos=grid.find("XRM_FAKE_CORE_LOG_");if(pos==std::string_view::npos)return {};
  return unsigned(std::stoul(std::string(grid.substr(pos+18,4))));
}
void add_subscription_ui(const CapturedTui& tui,const std::string& name,const std::string& url) {
  REQUIRE(tui.top_page("Subscriptions"));REQUIRE(tui.content());REQUIRE(tui.type("a"));expect_text(tui,"Add subscription");
  REQUIRE(tui.type(name));REQUIRE(tui.key("Tab"));REQUIRE(tui.type(url));REQUIRE(tui.key("C-s"));expect_text(tui,"Subscription "+name+" saved");
}
}

TEST_CASE("UI-02/T25 logs expose pause and resume actions on the actual TUI", "[UI-02][T25][I52]") {
  TempDir temp;Daemon daemon(temp.path);REQUIRE(daemon.start());const auto id=core_profile(daemon,"logs");
  select_direct(daemon,id,direct_node(daemon,"logs"));
  set_profile_process_environment(daemon.socket(),temp.path,id,{{"XRM_FAKE_CORE_LOG_COUNT","10000"},{"XRM_FAKE_CORE_LOG_INTERVAL_MS","10"}});
  rpc_value(daemon,"connection.connect",{{"profile_id",id}});wait_state(daemon,"running");
  {
    CapturedTui tui(daemon.socket());REQUIRE(tui.started());REQUIRE(tui.top_page("Logs"));REQUIRE(tui.content());expect_text(tui,"XRM_FAKE_CORE_LOG_");expect_text(tui,"Following");
    REQUIRE(tui.key("F1"));expect_text(tui,"Pause reading");REQUIRE(tui.key("Escape"));
    auto first=first_log(tui.grid());REQUIRE(first);auto followed=tui.wait_for([&](std::string_view g){auto v=first_log(g);return v&&*v>*first;});REQUIRE(first_log(followed)>first);
    REQUIRE(tui.type("p"));expect_text(tui,"Paused");auto paused=first_log(tui.grid());REQUIRE(paused);
    ::poll(nullptr,0,1200);CHECK(first_log(tui.grid())==paused);CHECK(tui.grid().find("new ")!=std::string::npos);
    REQUIRE(tui.key("PPage"));CHECK(first_log(tui.grid())<paused);REQUIRE(tui.type("p"));
    auto resumed=tui.wait_for([&](std::string_view g){return g.find("Following")!=std::string_view::npos&&first_log(g)>paused;});CHECK(first_log(resumed)>paused);
    CHECK(resumed.find("Viewport top:")==std::string::npos);REQUIRE(tui.top_page("Logs"));REQUIRE(tui.type("q"));
  }
  CHECK(rpc_value(daemon,"runtime.status").value("state","")=="running");
}

TEST_CASE("UI-03 overview distinguishes the saved target from the running target", "[UI-03][I52]") {
  TempDir temp;Daemon daemon(temp.path);REQUIRE(daemon.start());const auto id=core_profile(daemon,"targets");
  const auto a=direct_node(daemon,"Target A"), b=direct_node(daemon,"Target B");select_direct(daemon,id,a);rpc_value(daemon,"connection.connect",{{"profile_id",id}});auto before=wait_state(daemon,"running");select_direct(daemon,id,b);
  CapturedTui tui(daemon.socket());REQUIRE(tui.started());expect_text(tui,"Actual target");expect_text(tui,"Node · Target A");expect_text(tui,"Saved target");expect_text(tui,"Node · Target B");
  {auto store=Store::open(temp.path/"state.sqlite");REQUIRE(store);REQUIRE((*store)->erase_node(a));}
  REQUIRE(tui.content());REQUIRE(tui.type("r"));expect_text(tui,"running snapshot; removed from source");
  REQUIRE(tui.top_page("Modules"));REQUIRE(tui.secondary_section("log"));REQUIRE(tui.content());REQUIRE(tui.type("v"));expect_text(tui,"Runtime read-only");
  REQUIRE(tui.type("i"));expect_text(tui,"00_log.json");REQUIRE(tui.key("Escape"));REQUIRE(tui.top_page("Modules"));REQUIRE(tui.type("q"));
  const auto after=rpc_value(daemon,"runtime.status");CHECK(after.at("pid")==before.at("pid"));CHECK(after.at("active").at("selection").value("id_or_tag","")==a);
}

TEST_CASE("disabled native modules without source open safely and remain disabled", "[product-repair][UI-04]") {
  TempDir temp;Daemon daemon(temp.path);REQUIRE(daemon.start());const auto profile=rpc_value(daemon,"profile.create",{{"name","empty"},{"preset","custom"}});const auto id=profile.value("id","");
  const auto modules=rpc_value(daemon,"module.list",{{"profile_id",id}});REQUIRE(modules.size()==15);
  CapturedTui tui(daemon.socket());REQUIRE(tui.started());
  for(const auto& module:modules){REQUIRE(tui.top_page("Modules"));REQUIRE(tui.secondary_section(module.value("key","")));REQUIRE(tui.content());expect_text(tui,"Source read-only");
    REQUIRE(tui.key("F2"));expect_text(tui,"JSON");REQUIRE(tui.type("qeux"));REQUIRE(tui.key("C-s"));
    CHECK(rpc_value(daemon,"module.get",{{"profile_id",id},{"module",module.at("key")}}).at("text")==module.at("text"));
    if(module.at("text").is_null()){CHECK_FALSE(module.value("included",false));CHECK_FALSE(std::filesystem::exists(temp.path/"profiles"/id/"modules"/module.at("runtime_file").get<std::string>()));}
  }
  CHECK(rpc_value(daemon,"module.list",{{"profile_id",id}})==modules);
}

TEST_CASE("first use from xrm tui needs no Profile ID and completes local subscription to connect", "[product-repair][UI-01][E2E-06][TUI-SUB-01][UI-R01][UI-R06]") {
  TempDir temp;LocalSubscriptionServer subscription("vless://00000000-0000-4000-8000-000000000111@127.0.0.1:9?type=tcp&security=none#TUI-first-node\n",std::chrono::milliseconds(350));REQUIRE(subscription.valid());
  Daemon daemon(temp.path);REQUIRE(daemon.start());CapturedTui tui(daemon.socket());REQUIRE(tui.started());expect_text(tui,"Viewing: none");
  add_subscription_ui(tui,"local fixture",subscription.url());REQUIRE(tui.type("u"));expect_text(tui,"Update local fixture: completed");CHECK(rpc_value(daemon,"profile.list").empty());CHECK(rpc_value(daemon,"runtime.status").value("state","")=="stopped");
  REQUIRE(tui.top_page("Nodes"));expect_text(tui,"TUI-first-node");
  REQUIRE(tui.top_page("Config"));REQUIRE(tui.content());REQUIRE(tui.type("a"));expect_text(tui,"Register Xray core");REQUIRE(tui.type("fixture"));REQUIRE(tui.key("Tab"));REQUIRE(tui.type(XRM_TEST_FAKE_CORE));REQUIRE(tui.key("C-s"));expect_text(tui,"Registered fixture");
  REQUIRE(tui.top_page("Profiles"));REQUIRE(tui.content());REQUIRE(tui.type("n"));expect_text(tui,"new Profile");REQUIRE(tui.type("first profile"));REQUIRE(tui.key("Tab"));REQUIRE(tui.key("Down"));REQUIRE(tui.key("Enter"));REQUIRE(tui.key("C-s"));expect_text(tui,"Profile first profile saved");
  // New Profile can be viewed without implicitly selecting a runtime target.
  REQUIRE(tui.type("u"));expect_text(tui,"Next: first profi");
  REQUIRE(tui.top_page("Subscriptions"));REQUIRE(tui.content());REQUIRE(tui.type("b"));expect_text(tui,"Sources for first profile");REQUIRE(tui.key("Space"));REQUIRE(tui.key("C-s"));expect_text(tui,"Source bindings saved");
  auto rows=rpc_value(daemon,"profile.list");REQUIRE(rows.size()==1);const auto id=rows[0].value("id","");
  const auto run=(temp.path/"run.argv").string(),check=(temp.path/"check.argv").string();set_profile_process_environment(daemon.socket(),temp.path,id,{{"XRM_FAKE_CORE_RUN_ARGV",run},{"XRM_FAKE_CORE_CHECK_ARGV",check}});
  CHECK(rpc_value(daemon,"profile.get",{{"profile_id",id}}).at("selected").is_null());
  REQUIRE(tui.top_page("Nodes"));REQUIRE(tui.content());expect_text(tui,"TUI-first-node");REQUIRE(tui.type("c"));auto connected=wait_state(daemon,"running");CHECK(connected.at("active").at("selection").value("kind","")=="node");CHECK(rpc_value(daemon,"profile.get",{{"profile_id",id}}).at("selected").is_object());
  REQUIRE(tui.top_page("Overview")); REQUIRE(tui.content()); REQUIRE(tui.type("d"));
  expect_text(tui,"Stop core"); REQUIRE(tui.key("BTab")); REQUIRE(tui.key("Enter"));
  wait_state(daemon,"stopped");
  std::cout << "Overview after server stopped, before Manager completion:\n" << tui.grid();
  // A terminal lifecycle action finishes when the Manager consumes its result;
  // observing the server state alone can precede that completion event. Keep
  // the live page open: notification history is an intentionally static view.
  expect_text(tui,"Stop first profile: completed");
  std::cout << "Overview after Manager stop completion:\n" << tui.grid();
  REQUIRE(tui.type("c")); const auto restarted = wait_state(daemon,"running");
  CHECK(restarted.at("active").at("run_id") != connected.at("active").at("run_id"));
  CHECK(std::filesystem::exists(run)); CHECK(std::filesystem::exists(check));
  REQUIRE(tui.top_page("Logs"));REQUIRE(tui.top_page("Overview"));expect_text(tui,"Statistics");REQUIRE(tui.type("q"));CHECK(rpc_value(daemon,"runtime.status").value("state","")=="running");rpc_value(daemon,"connection.disconnect");wait_state(daemon,"stopped");
}

TEST_CASE("TUI-SUB-02 failed refresh is visible and keeps the prior node inventory", "[TUI-SUB-02][UI-R01][UI-R06]") {
  TempDir temp;LocalSubscriptionServer subscription("unavailable\n",std::chrono::milliseconds(350),403);REQUIRE(subscription.valid());Daemon daemon(temp.path);REQUIRE(daemon.start());const auto node=direct_node(daemon,"kept-node");
  CapturedTui tui(daemon.socket());REQUIRE(tui.started());add_subscription_ui(tui,"403 fixture",subscription.url());REQUIRE(tui.type("u"));expect_text(tui,"403");
  REQUIRE(tui.top_page("Nodes"));REQUIRE(tui.secondary_section("Local"));expect_text(tui,"kept-node");CHECK(rpc_value(daemon,"node.get",{{"id",node}}).value("original_name","")=="kept-node");CHECK(rpc_value(daemon,"runtime.status").value("state","")=="stopped");
}

TEST_CASE("TUI-SUB-03 partial refresh shows imported, unsupported and malformed counts", "[TUI-SUB-03][UI-R01]") {
  TempDir temp;LocalSubscriptionServer subscription("vless://00000000-0000-4000-8000-000000000111@127.0.0.1:9?type=tcp&security=none#partial-node\nanytls://opaque\nnot-a-uri\n",std::chrono::milliseconds(350));REQUIRE(subscription.valid());Daemon daemon(temp.path);REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket());REQUIRE(tui.started());add_subscription_ui(tui,"partial",subscription.url());REQUIRE(tui.type("u"));expect_text(tui,"Partial: inspect");REQUIRE(tui.key("Enter"));REQUIRE(tui.type("F"));expect_text(tui,"unsupported_count: 1");expect_text(tui,"malformed_count: 1");REQUIRE(tui.key("Escape"));
  REQUIRE(tui.top_page("Nodes"));expect_text(tui,"partial-node");auto rows=rpc_value(daemon,"subscription.list");REQUIRE(rows.size()==1);CHECK(rows[0].value("node_count",0)==1);CHECK(rows[0].value("unsupported_count",0)==1);CHECK(rows[0].value("malformed_count",0)==1);
}

TEST_CASE("UI-06/UI-07 zero-profile startup and Profile management use names", "[UI-06][UI-07][TUI]") {
  TempDir temp;Daemon daemon(temp.path);REQUIRE(daemon.start());CapturedTui tui(daemon.socket());REQUIRE(tui.started());expect_text(tui,"Viewing: none");
  REQUIRE(tui.top_page("Subscriptions"));expect_text(tui,"No subscriptions");REQUIRE(tui.top_page("Nodes"));REQUIRE(tui.secondary_section("Groups"));expect_text(tui,"View a Profile");REQUIRE(tui.top_page("Modules"));expect_text(tui,"View a Profile");
  REQUIRE(tui.top_page("Overview"));REQUIRE(tui.content());REQUIRE(tui.type("c"));expect_text(tui,"Use is empty");CHECK(rpc_value(daemon,"profile.list").empty());
  REQUIRE(tui.top_page("Profiles"));REQUIRE(tui.content());REQUIRE(tui.type("n"));expect_text(tui,"new Profile");REQUIRE(tui.type("Home"));REQUIRE(tui.key("C-s"));expect_text(tui,"Profile Home saved");
  REQUIRE(tui.type("R"));expect_text(tui,"rename Profile");replace_input(tui,"Router",20);REQUIRE(tui.key("C-s"));expect_text(tui,"Profile Router saved");
  REQUIRE(tui.type("c"));expect_text(tui,"copy Profile");replace_input(tui,"Test",30);REQUIRE(tui.key("C-s"));expect_text(tui,"Profile Test saved");
  auto rows=rpc_value(daemon,"profile.list");REQUIRE(rows.size()==2);REQUIRE(tui.key("Home"));for(auto&row:rows){if(row.value("name","")=="Test")break;REQUIRE(tui.key("Down"));}
  REQUIRE(tui.type("D"));expect_text(tui,"Delete Profile");REQUIRE(tui.key("BTab"));REQUIRE(tui.key("Enter"));expect_text(tui,"Delete Test saved");auto remaining=rpc_value(daemon,"profile.list");REQUIRE(remaining.size()==1);CHECK(remaining[0].value("name","")=="Router");CHECK(rpc_value(daemon,"runtime.status").value("state","")=="stopped");
}

TEST_CASE("UI-07 Profile management is name-oriented and browsing keeps the running Profile", "[UI-07][UI-08][UX-01][TUI]") {
  TempDir temp;Daemon daemon(temp.path);REQUIRE(daemon.start());const auto home=core_profile(daemon,"Home");const auto core=rpc_value(daemon,"profile.get",{{"profile_id",home}}).value("core_id","");const auto office=rpc_value(daemon,"profile.create",{{"name","Office"},{"core_id",core}}).value("id","");const auto node=direct_node(daemon,"direct");select_direct(daemon,home,node);select_direct(daemon,office,node);rpc_value(daemon,"connection.connect",{{"profile_id",home}});const auto before=wait_state(daemon,"running");
  CapturedTui tui(daemon.socket());REQUIRE(tui.started());expect_text(tui,"[Profiles]");expect_text(tui,"Home");expect_text(tui,"Office");REQUIRE(tui.content());
  auto rows=rpc_value(daemon,"profile.list");REQUIRE(tui.key("Home"));for(auto&r:rows){if(r.value("id","")==office)break;REQUIRE(tui.key("Down"));}
  REQUIRE(tui.key("Enter"));expect_text(tui,"View Office");expect_text(tui,"Running: Home");REQUIRE(tui.type("e"));expect_text(tui,"Module Edit: Office");CHECK(tui.grid().find("Edit Profile")==std::string::npos);
  REQUIRE(tui.type("u"));expect_text(tui,"Next: Office");REQUIRE(tui.type("u"));expect_text(tui,"Next: none");const auto unchanged=rpc_value(daemon,"runtime.status");CHECK(unchanged.at("pid")==before.at("pid"));CHECK(unchanged.at("active").at("confdir")==before.at("active").at("confdir"));CHECK(unchanged.at("active").at("selection")==before.at("active").at("selection"));
  REQUIRE(tui.type("u"));REQUIRE(tui.top_page("Overview"));REQUIRE(tui.content());REQUIRE(tui.type("c"));
  auto switched=tui.wait_for([&](std::string_view g){auto status=rpc_value(daemon,"runtime.status");return status.contains("active")&&status["active"].is_object()&&status["active"].value("profile_id","")==office&&g.find("Running: Office")!=std::string_view::npos;});CHECK(switched.find("Running: Office")!=std::string::npos);
  CHECK(rpc_value(daemon,"runtime.status").value("state","")=="running");
}

TEST_CASE("UI-05 opens a configured external editor from the native module draft", "[UI-05][I52]") {
  TempDir temp;Daemon daemon(temp.path);REQUIRE(daemon.start());const auto id=core_profile(daemon,"editor");
  auto source=[&]{return rpc_value(daemon,"module.get",{{"profile_id",id},{"module","log"}}).at("text").get<std::string>();};
  for(const auto& mode:{"output","success","nonzero","cancel","invalid","unset","conflict"}) {
    CAPTURE(mode);rpc_value(daemon,"module.save",{{"profile_id",id},{"module","log"},{"text","{\"log\":{\"loglevel\":\"info\"}}\n"}});const auto before=source();const auto marker=(temp.path/(std::string(mode)+".marker")).string();
    std::string command=std::string(XRM_TEST_FAKE_EDITOR)+" --mode "+(std::string(mode)=="conflict"?"output":mode)+" --marker "+marker;
    CapturedTui tui(daemon.socket(),{std::string("VISUAL=")+(std::string(mode)=="success"||std::string(mode)=="unset"?"":command),std::string("EDITOR=")+(std::string(mode)=="success"?command:std::string(mode)=="unset"?"":"/usr/bin/false")});REQUIRE(tui.started());edit_module(tui,"log");REQUIRE(tui.key("F2"));
    if(std::string(mode)=="nonzero"||std::string(mode)=="cancel")REQUIRE(tui.type("DRAFT_KEEP"));
    REQUIRE(tui.key("F6"));
    if(std::string(mode)=="unset"){expect_notification(tui,"Configure an editor");CHECK(source()==before);continue;}
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!std::filesystem::exists(marker)&&std::chrono::steady_clock::now()<until)::poll(nullptr,0,20);REQUIRE(std::filesystem::exists(marker));
    if(std::string(mode)=="nonzero"||std::string(mode)=="cancel"){expect_notification(tui,std::string(mode)=="cancel"?"External editor cancelled":"External editor failed");expect_text(tui,"DRAFT_KEEP");CHECK(source()==before);discard_dialog(tui);continue;}
    if(std::string(mode)=="invalid"){expect_text(tui,"invalid JSON");REQUIRE(tui.key("C-s"));expect_text(tui,"Invalid JSON line 1");expect_text(tui,"{ invalid JSON");CHECK(source()==before);discard_dialog(tui);continue;}
    expect_text(tui,"warning");expect_text(tui,"Unsaved *");CHECK(source()==before);
    if(std::string(mode)=="conflict"){rpc_value(daemon,"module.save",{{"profile_id",id},{"module","log"},{"text","{\"log\":{\"loglevel\":\"error\"}}\n"}});REQUIRE(tui.key("C-s"));expect_text(tui,"source file changed");expect_text(tui,"warning");CHECK(source().find("error")!=std::string::npos);discard_dialog(tui);}
    else {REQUIRE(tui.key("C-s"));expect_notification(tui,"Saved editor / log");CHECK(source().find("warning")!=std::string::npos);}
    CHECK(rpc_value(daemon,"runtime.status").value("state","")=="stopped");
  }
}

TEST_CASE("T25 pause keeps a bounded log viewport and reports server buffer truncation", "[T25][I52]") {
  TempDir temp;Daemon daemon(temp.path);REQUIRE(daemon.start());const auto id=core_profile(daemon,"truncation");select_direct(daemon,id,direct_node(daemon,"truncate"));set_profile_process_environment(daemon.socket(),temp.path,id,{{"XRM_FAKE_CORE_STDOUT_BYTES","5000000"}});
  CapturedTui tui(daemon.socket());REQUIRE(tui.started());REQUIRE(tui.top_page("Logs"));REQUIRE(tui.content());REQUIRE(tui.type("p"));expect_text(tui,"Paused");rpc_value(daemon,"connection.connect",{{"profile_id",id}});wait_state(daemon,"running");
  const auto grid=tui.wait_for([](std::string_view g){return g.find("server history incomplete")!=std::string_view::npos;},std::chrono::seconds(20));INFO(grid);CHECK(grid.find("server history incomplete")!=std::string::npos);CHECK(grid.find("Paused")!=std::string::npos);CHECK(grid.find("F1 Help")!=std::string::npos);CHECK(grid.find("Viewport top:")==std::string::npos);
  rpc_value(daemon,"connection.disconnect");wait_state(daemon,"stopped");
}

TEST_CASE("UX-02/03/04/05/06 focus layers route only the current keyboard scope", "[UX-02][UX-03][UX-04][UX-05][UX-06]") {
  TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto created = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Home"}));
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); expect_text(tui, "TopNav");
  REQUIRE(tui.type("3[7puecs")); CHECK(visibly_emphasized(tui.grid(true), "[Overview]"));
  REQUIRE(tui.top_page("Profiles")); REQUIRE(tui.content()); REQUIRE(tui.key("Enter"));
  expect_text(tui, "View Home"); CHECK(tui.grid().find("Next: Home") != std::string::npos);
  CHECK(tui.grid().find("Module Edit: none") != std::string::npos);
  REQUIRE(tui.top_page("Modules")); REQUIRE(tui.secondary_section("routing"));
  REQUIRE(tui.type("p")); CHECK(tui.grid().find("Keep custom native") == std::string::npos);
  REQUIRE(tui.content()); REQUIRE(tui.type("p")); expect_text(tui, "Source read-only");
  CHECK(tui.grid().find("Keep custom native") == std::string::npos);
  edit_module(tui, "routing"); REQUIRE(tui.type("p")); expect_text(tui, "Keep custom native");
  REQUIRE(tui.key("Escape"));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("UX-07/09 module views use one full-width draft and F2 never saves", "[UX-07][UX-09]") {
  TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Home"}));
  const auto id = profile.at("id").get<std::string>();
  const auto original = raw_rpc(daemon.socket(), "module.get", Json{{"profile_id", id}, {"module", "log"}}).at("result").at("text");
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); edit_module(tui, "log");
  REQUIRE(tui.key("Enter")); expect_text(tui, "loglevel");
  REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Home")); REQUIRE(tui.repeat_key("Down", 2)); REQUIRE(tui.key("Enter"));
  REQUIRE(tui.key("C-s")); expect_text(tui, "Unsaved *"); REQUIRE(tui.key("F2")); expect_text(tui, "\"info\"");
  CHECK(raw_rpc(daemon.socket(), "module.get", Json{{"profile_id", id}, {"module", "log"}}).at("result").at("text") == original);
  REQUIRE(tui.key("C-Home")); REQUIRE(tui.repeat_key("Delete", 4096));
  REQUIRE(tui.type(R"({"log":{"loglevel":"debug","extension":{"kept":true}}})"));
  REQUIRE(tui.key("F2")); expect_text(tui, "Visual"); REQUIRE(tui.key("Enter")); expect_text(tui, "debug"); REQUIRE(tui.key("Escape"));
  REQUIRE(tui.key("C-s")); expect_notification(tui, "Saved Home / log");
  const auto saved = Json::parse(raw_rpc(daemon.socket(), "module.get", Json{{"profile_id", id}, {"module", "log"}}).at("result").at("text").get<std::string>());
  CHECK(saved["log"]["loglevel"] == "debug"); CHECK(saved["log"]["extension"]["kept"] == true);
}

TEST_CASE("UX-08 invalid JSON stays in the same draft until repaired", "[UX-08][TUI]") {
  TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Draft"})); const auto id=profile.at("id").get<std::string>();
  const auto original=raw_rpc(daemon.socket(), "module.get", Json{{"profile_id",id},{"module","log"}}).at("result").at("text");
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); edit_module(tui,"log");
  REQUIRE(tui.key("F2")); REQUIRE(tui.type("x")); REQUIRE(tui.key("C-s")); expect_text(tui,"Invalid JSON");
  REQUIRE(tui.key("F2")); expect_text(tui,"JSON"); expect_text(tui,"Unsaved *");
  CHECK(tui.grid().find("line 1")!=std::string::npos);
  CHECK(raw_rpc(daemon.socket(),"module.get",Json{{"profile_id",id},{"module","log"}}).at("result").at("text")==original);
  REQUIRE(tui.key("BSpace")); REQUIRE(tui.key("F2")); expect_text(tui,"Xray logging");
}

TEST_CASE("UX-07 leaving an unsaved module draft requires an explicit choice", "[UX-07][TUI]") {
  TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile=Json::parse(cli(daemon.socket(),{"profile","create","--name","Draft boundary"})); const auto id=profile.at("id").get<std::string>();
  const auto original=raw_rpc(daemon.socket(),"module.get",Json{{"profile_id",id},{"module","log"}}).at("result").at("text");
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); edit_module(tui,"log"); REQUIRE(tui.key("F2")); REQUIRE(tui.type(" "));
  REQUIRE(tui.key("Escape")); expect_text(tui,"Unsaved module"); REQUIRE(tui.repeat_key("Tab",2)); REQUIRE(tui.key("Enter")); expect_text(tui,"Unsaved *");
  REQUIRE(tui.key("Escape")); expect_text(tui,"Unsaved module"); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Enter")); expect_text(tui,"SecondaryNav");
  CHECK(tui.grid().find("Unsaved *")==std::string::npos); REQUIRE(tui.secondary_section("dns"));
  CHECK(raw_rpc(daemon.socket(),"module.get",Json{{"profile_id",id},{"module","log"}}).at("result").at("text")==original);
}

TEST_CASE("UX-09 formatting compact Source and switching read-only Runtime never saves", "[UX-09][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE,
      "--name", "preview core"}));
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Compact",
      "--core", core.at("id").get<std::string>()}));
  const auto id = profile.at("id").get<std::string>();
  const std::string compact = R"({"log":{"loglevel":"warning","vendor":{"kept":true}}})";
  REQUIRE_FALSE(cli(daemon.socket(), {"config", "edit", id, "log", "--text", compact}).empty());
  const auto node = Json::parse(cli(daemon.socket(), {"node", "add", "--name", "preview direct",
      "--outbound-json", R"({"protocol":"freedom","settings":{},"tag":"preview-direct"})"}));
  REQUIRE_FALSE(cli(daemon.socket(), {"select", "node", id, node.at("id").get<std::string>()}).empty());
  REQUIRE_FALSE(cli(daemon.socket(), {"connect", id}).empty());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (std::chrono::steady_clock::now() < deadline &&
         Json::parse(cli(daemon.socket(), {"status"})).value("state", "") != "running")
    ::poll(nullptr, 0, 30);
  REQUIRE(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "running");
  const auto pid=Json::parse(cli(daemon.socket(),{"status"})).at("pid");
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.top_page("Modules")); REQUIRE(tui.content()); REQUIRE(tui.key("F2")); expect_text(tui,"\"vendor\"");
  CHECK(tui.grid().find("Unsaved *")==std::string::npos); REQUIRE(tui.key("F2")); expect_text(tui,"Xray logging");
  REQUIRE(tui.type("v")); expect_text(tui,"Runtime read-only"); REQUIRE(tui.key("F2")); REQUIRE(tui.type("x")); REQUIRE(tui.key("C-s"));
  CHECK(tui.grid().find("Unsaved *")==std::string::npos);
  CHECK(raw_rpc(daemon.socket(),"module.get",Json{{"profile_id",id},{"module","log"}}).at("result").at("text").get<std::string>()==compact);
  const auto status=Json::parse(cli(daemon.socket(),{"status"})); CHECK(status.value("state","")=="running"); CHECK(status.at("pid")==pid);
}

TEST_CASE("UX-INT node JSON and Group field errors keep the bound PTY draft", "[UX-INT][I-11][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Draft"}));
  const auto profile_id = profile.at("id").get<std::string>();
  const auto uri = "vless://00000000-0000-4000-8000-000000000222@127.0.0.1:12345?type=tcp&security=none#Local";
  const auto node = Json::parse(cli(daemon.socket(), {"node", "add", "--uri", uri}));
  const auto node_id = node.at("id").get<std::string>();
  const auto node_before = raw_rpc(daemon.socket(), "node.get", Json{{"id", node_id}})
      .at("result").at("settings");
  {
    CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.secondary_section("Local")); REQUIRE(tui.content());
    REQUIRE(tui.type("J")); expect_text(tui,"Advanced node"); REQUIRE(tui.key("Tab")); REQUIRE(tui.type("x")); REQUIRE(tui.key("C-s")); expect_text(tui,"Invalid JSON object");
    CHECK(raw_rpc(daemon.socket(),"node.get",Json{{"id",node_id}}).at("result").at("settings")==node_before);
    REQUIRE(tui.key("Escape")); expect_text(tui,"Unsaved form"); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Enter"));
  }
  const auto local = Json::parse(cli(daemon.socket(), {"node", "add", "--name", "group-local",
      "--outbound-json", R"({"protocol":"freedom","settings":{},"tag":"group-local"})"}));
  const auto group = Json::parse(cli(daemon.socket(), {"group", "add", profile_id,
      "--name", "Pinned", "--tag", "pinned", "--mode", "manual", "--members",
      local.at("id").get<std::string>()}));
  const auto group_id = group.at("id").get<std::string>();
  const auto group_before = raw_rpc(daemon.socket(), "group.get",
      Json{{"profile_id", profile_id}, {"group_id", group_id}}).at("result");
  {
    CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Nodes")); REQUIRE(tui.secondary_section("Groups")); REQUIRE(tui.content());
    REQUIRE(tui.type("e")); expect_text(tui,"Edit Group"); replace_input(tui,""); REQUIRE(tui.key("C-s")); expect_text(tui,"Required");
    CHECK(raw_rpc(daemon.socket(),"group.get",Json{{"profile_id",profile_id},{"group_id",group_id}}).at("result")==group_before);
    REQUIRE(tui.type("Repaired draft")); expect_text(tui,"Repaired draft");
  }
}

TEST_CASE("UX-INT Profile and Subscription editors save their own sources", "[UX-INT][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Home"}));
  const auto profile_id = profile.at("id").get<std::string>();
  const auto source = Json::parse(cli(daemon.socket(), {"sub", "add", "--name", "Fixture",
      "--url", "http://127.0.0.1:1/fixture"}));
  const auto source_id = source.at("id").get<std::string>();
  REQUIRE(!cli(daemon.socket(), {"profile", "sources", "set", profile_id, "--source", source_id}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Profiles")); REQUIRE(tui.content());
  REQUIRE(tui.type("m")); expect_text(tui,"metadata Profile"); replace_input(tui,"Home renamed"); REQUIRE(tui.key("C-s")); expect_text(tui,"Home renamed saved;");
  CHECK(Json::parse(cli(daemon.socket(),{"profile","show",profile_id})).value("name","")=="Home renamed");
  REQUIRE(tui.type("m")); expect_text(tui,"metadata Profile"); replace_input(tui,"Stale draft");
  REQUIRE_FALSE(cli(daemon.socket(),{"profile","rename",profile_id,"--name","External name"}).empty()); REQUIRE(tui.key("C-s"));
  expect_text(tui,"Stale draft"); expect_text(tui,"changed");
  CHECK(Json::parse(cli(daemon.socket(),{"profile","show",profile_id})).value("name","")=="External name");
  REQUIRE(tui.key("Escape")); expect_text(tui,"Unsaved form"); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Enter"));
  REQUIRE_FALSE(cli(daemon.socket(),{"profile","rename",profile_id,"--name","Home renamed"}).empty());
  REQUIRE(tui.top_page("Subscriptions")); REQUIRE(tui.content()); REQUIRE(tui.type("e")); expect_text(tui,"Edit subscription");
  replace_input(tui,"Fixture renamed"); REQUIRE(tui.key("C-s")); expect_text(tui,"Settings changed");
  auto updated=Json::parse(cli(daemon.socket(),{"sub","show",source_id})).at("subscription"); CHECK(updated.value("name","")=="Fixture renamed"); CHECK(updated.value("auto_refresh",false));
  REQUIRE(tui.type("e")); replace_input(tui,"Stale subscription"); REQUIRE_FALSE(cli(daemon.socket(),{"sub","edit",source_id,"--name","External source"}).empty()); REQUIRE(tui.key("C-s"));
  expect_text(tui,"changed outside this form"); expect_text(tui,"Stale subscription");
  CHECK(Json::parse(cli(daemon.socket(),{"sub","show",source_id})).at("subscription").value("name","")=="External source");
  REQUIRE(tui.key("Escape")); expect_text(tui,"Unsaved form"); REQUIRE(tui.key("Tab")); REQUIRE(tui.key("Enter"));
  REQUIRE_FALSE(cli(daemon.socket(),{"sub","edit",source_id,"--name","Fixture renamed"}).empty()); REQUIRE(tui.type("r")); REQUIRE(tui.type("t")); expect_text(tui,"Auto refresh");
  CHECK_FALSE(Json::parse(cli(daemon.socket(),{"sub","show",source_id})).at("subscription").value("auto_refresh",true));
  const auto other=Json::parse(cli(daemon.socket(),{"profile","create","--name","Office"}));
  REQUIRE_FALSE(cli(daemon.socket(),{"profile","sources","set",other.at("id").get<std::string>(),"--source",source_id}).empty());
  REQUIRE(tui.type("r")); REQUIRE(tui.type("D")); expect_text(tui,"Remove Fixture renamed"); expect_text(tui,"Office"); expect_text(tui,"Home renamed");
  REQUIRE(tui.key("Escape")); CHECK(Json::parse(cli(daemon.socket(),{"sub","show",source_id})).at("subscription").value("name","")=="Fixture renamed");
  REQUIRE(tui.type("D")); REQUIRE(tui.key("C-s")); expect_text(tui,"Subscription removed");
  const auto remaining=Json::parse(cli(daemon.socket(),{"sub","list"})); CHECK(std::none_of(remaining.begin(),remaining.end(),[&](const Json& source){return source.value("id","")==source_id;}));
  CHECK(Json::parse(cli(daemon.socket(),{"status"})).value("state","")=="stopped");
}

TEST_CASE("UX-INT offline inventory is unknown and cached data recovers in place", "[UX-INT][TUI]") {
  TempDir temp; Daemon daemon(temp.path);
  { CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); expect_text(tui,"unavailable"); CHECK(tui.grid().find("0 nodes")==std::string::npos);
    REQUIRE(daemon.start()); REQUIRE(tui.content()); REQUIRE(tui.type("r")); expect_text(tui,"xrmd ready"); expect_text(tui,"0 nodes"); }
  REQUIRE_FALSE(cli(daemon.socket(),{"profile","create","--name","Cached Home"}).empty());
  { CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.top_page("Profiles")); REQUIRE(tui.content()); expect_text(tui,"Cached Home");
    daemon.stop(); REQUIRE(tui.type("r")); expect_text(tui,"cached"); expect_text(tui,"Cached Home"); expect_text(tui,"Content");
    REQUIRE(daemon.start()); REQUIRE(tui.type("r")); expect_text(tui,"xrmd ready"); expect_text(tui,"Cached Home"); expect_text(tui,"Content"); }
  CHECK(Json::parse(cli(daemon.socket(),{"status"})).value("state","")=="stopped");
}

TEST_CASE("UX-INT a dirty native draft survives same-session resize and exposes full paths", "[UX-INT][TUI]") {
  TempDir temp; Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile=Json::parse(cli(daemon.socket(),{"profile","create","--name","Resize"})); const auto id=profile.at("id").get<std::string>();
  const auto original=raw_rpc(daemon.socket(),"module.get",Json{{"profile_id",id},{"module","log"}}).at("result").at("text");
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started()); REQUIRE(tui.content()); REQUIRE(tui.type("f")); expect_text(tui,"No running confdir"); REQUIRE(tui.key("Escape"));
  edit_module(tui,"log"); REQUIRE(tui.type("i")); expect_text(tui,"Source:"); expect_text(tui,"Runtime:"); REQUIRE(tui.key("Escape"));
  REQUIRE(tui.key("F2")); REQUIRE(tui.type("x")); expect_text(tui,"Unsaved *");
  REQUIRE(tui.resize(120,40)); expect_text(tui,"Unsaved *"); REQUIRE(tui.resize(70,20)); expect_text(tui,"Terminal too small");
  REQUIRE(tui.resize(80,24)); expect_text(tui,"Unsaved *"); REQUIRE(tui.key("BSpace")); REQUIRE(tui.key("F2")); expect_text(tui,"Xray logging");
  CHECK(raw_rpc(daemon.socket(),"module.get",Json{{"profile_id",id},{"module","log"}}).at("result").at("text")==original);
}

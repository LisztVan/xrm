#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/common.hpp"

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
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
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
      ::poll(nullptr, 0, 10);
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
class CapturedTui {
 public:
  explicit CapturedTui(const std::filesystem::path& socket)
      : tmux_socket_("xrm-acceptance-" + std::to_string(::getpid()) + "-" +
                     std::to_string(sequence_.fetch_add(1))) {
    const auto launch = "exec " + std::string(XRM_TEST_MANAGER) + " --socket " +
                        socket.string() + " tui";
    started_ = invoke({"new-session", "-d", "-s", "xrm", "-x", "80", "-y", "24", launch}).first == 0;
  }
  CapturedTui(const CapturedTui&) = delete;
  CapturedTui& operator=(const CapturedTui&) = delete;
  ~CapturedTui() { if (started_) (void)invoke({"kill-server"}); }
  bool started() const { return started_; }
  bool type(std::string_view value) const {
    return invoke({"send-keys", "-t", "xrm", "-l", std::string(value)}).first == 0;
  }
  bool key(std::string_view value) const {
    return invoke({"send-keys", "-t", "xrm", std::string(value)}).first == 0;
  }
  bool resize(int width, int height) const {
    return invoke({"resize-window", "-t", "xrm", "-x", std::to_string(width),
                   "-y", std::to_string(height)}).first == 0;
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
  ~Daemon() { if (pid_ > 0) { ::kill(pid_, SIGTERM); ::waitpid(pid_, nullptr, 0); } }
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
    if (line.find("▸") != std::string_view::npos && line.find(name) != std::string_view::npos)
      return true;
    if (last == std::string_view::npos) break;
    first = last + 1;
  }
  return false;
}

bool visibly_inverted(std::string_view styled_grid, std::string_view label) {
  const auto at = styled_grid.find(label);
  if (at == std::string_view::npos) return false;
  bool inverted = false;
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
      if (next == std::string_view::npos) break;
      first = next + 1;
    }
    position = end + 1;
  }
  return inverted;
}
} // namespace

TEST_CASE("TUI navigation shows one matching selected tab in the current terminal grid", "[UI-NAV][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  const std::array<std::pair<std::string_view, std::string_view>, 7> pages{{
      {"Overview", "XRM Dashboard"}, {"Profiles", "Profiles"},
      {"Modules", "Native Modules"}, {"Subscriptions", "Subscriptions —"},
      {"Nodes", "Nodes —"}, {"Logs", "Logs —"}, {"Config", "Config"},
  }};
  for (std::size_t index = 0; index < pages.size(); ++index) {
    REQUIRE(tui.type(std::to_string(index + 1)));
    const auto& [title, body] = pages[index];
    const auto label = "[" + std::to_string(index + 1) + " " + std::string(title) + "]";
    const auto grid = tui.wait_for([&](std::string_view value) {
      return value.find("Current: " + std::string(title)) != std::string_view::npos &&
             value.find(label) != std::string_view::npos && value.find(body) != std::string_view::npos;
    });
    INFO("page=" << index + 1 << " grid=" << grid);
    REQUIRE(grid.find("Current: " + std::string(title)) != std::string::npos);
    REQUIRE(grid.find(label) != std::string::npos);
    REQUIRE(grid.find(body) != std::string::npos);
    CHECK(visibly_inverted(tui.grid(true), label));
  }
  REQUIRE(tui.key("Tab"));
  const auto wrapped = tui.wait_for([](std::string_view grid) {
    return grid.find("Current: Overview") != std::string_view::npos &&
           grid.find("[1 Overview]") != std::string_view::npos;
  });
  CHECK(wrapped.find("XRM Dashboard") != std::string::npos);
  REQUIRE(tui.key("BTab"));
  const auto reversed = tui.wait_for([](std::string_view grid) {
    return grid.find("Current: Config") != std::string_view::npos &&
           grid.find("[7 Config]") != std::string_view::npos;
  });
  CHECK(reversed.find("Config") != std::string::npos);
  REQUIRE(tui.resize(50, 24));
  const auto narrow = tui.wait_for([](std::string_view grid) {
    return grid.find("Current: Config") != std::string_view::npos &&
           grid.find("[7 Config]") != std::string_view::npos;
  });
  CHECK(narrow.find("[7 Config]") != std::string::npos);
  REQUIRE(tui.type("]"));
  const auto appearance = tui.wait_for([](std::string_view grid) {
    return grid.find("[Appearance]") != std::string_view::npos &&
           grid.find("Appearance settings") != std::string_view::npos;
  });
  CHECK(appearance.find("Current: Config") != std::string::npos);
  REQUIRE(tui.type("5"));
  REQUIRE(tui.type("g"));
  const auto groups = tui.wait_for([](std::string_view grid) {
    return grid.find("[5 Nodes]") != std::string_view::npos &&
           grid.find("Groups") != std::string_view::npos;
  });
  CHECK(groups.find("Current: Nodes") != std::string::npos);
  REQUIRE(tui.resize(80, 24));
  REQUIRE(tui.type("q"));
}

TEST_CASE("Nodes Add exposes a real Local form and Group action", "[UI-LOCAL][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.type("5a"));
  const auto add = tui.wait_for([](std::string_view grid) {
    return grid.find("Add outlet") != std::string_view::npos &&
           grid.find("Manual node") != std::string_view::npos &&
           grid.find("Group") != std::string_view::npos;
  });
  REQUIRE(add.find("Add outlet") != std::string::npos);
  REQUIRE(tui.type("1"));
  const auto protocol = tui.wait_for([](std::string_view grid) {
    return grid.find("Protocol") != std::string_view::npos &&
           grid.find("vless") != std::string_view::npos;
  });
  CHECK(protocol.find("vless") != std::string::npos);
  REQUIRE(tui.key("Escape"));
  REQUIRE(tui.type("q"));
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
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Home"}));
  const auto profile_id = profile.value("id", "");
  REQUIRE_FALSE(profile_id.empty());
  const auto before = raw_rpc(daemon.socket(), "module.get",
      Json{{"profile_id", profile_id}, {"module", "routing"}});
  REQUIRE(before.value("ok", false));
  {
    CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
    REQUIRE(tui.type("7]"));
    REQUIRE(tui.wait_for([](std::string_view grid) {
      return grid.find("[Appearance]") != std::string_view::npos;
    }).find("Appearance settings") != std::string::npos);
    REQUIRE(tui.type("ts"));
    REQUIRE(tui.wait_for([](std::string_view grid) {
      return grid.find("Appearance saved") != std::string_view::npos;
    }).find("Appearance saved") != std::string::npos);
    REQUIRE(tui.type("]e"));
    REQUIRE(tui.type("[\"/bin/true\"]")); REQUIRE(tui.key("Enter"));
    REQUIRE(tui.wait_for([](std::string_view grid) {
      return grid.find("Editor argv saved") != std::string_view::npos;
    }).find("Editor argv saved") != std::string::npos);
    REQUIRE(tui.type("q"));
  }
  const auto saved = raw_rpc(daemon.socket(), "settings.get", Json::object());
  REQUIRE(saved.value("ok", false));
  CHECK(saved.at("result").at("appearance").at("theme") == "dark");
  CHECK(saved.at("result").at("editor").at("argv") == Json::array({"/bin/true"}));
  const auto after = raw_rpc(daemon.socket(), "module.get",
      Json{{"profile_id", profile_id}, {"module", "routing"}});
  REQUIRE(after.value("ok", false));
  CHECK(after.at("result").at("text") == before.at("result").at("text"));
  CapturedTui reopened(daemon.socket()); REQUIRE(reopened.started());
  REQUIRE(reopened.type("7]"));
  CHECK(reopened.wait_for([](std::string_view grid) {
    return grid.find("Theme [t] \"dark\"") != std::string_view::npos;
  }).find("Theme [t] \"dark\"") != std::string::npos);
  REQUIRE(reopened.type("t"));
  REQUIRE(reopened.wait_for([](std::string_view grid) {
    return grid.find("Previewing unsaved appearance") != std::string_view::npos;
  }).find("Previewing unsaved appearance") != std::string::npos);
  REQUIRE(reopened.key("Escape"));
  CHECK(reopened.wait_for([](std::string_view grid) {
    return grid.find("Theme [t] \"dark\"") != std::string_view::npos &&
           grid.find("Appearance preview cancelled") != std::string_view::npos;
  }).find("Theme [t] \"dark\"") != std::string::npos);
  REQUIRE(reopened.type("]"));
  CHECK(reopened.wait_for([](std::string_view grid) {
    return grid.find("Explicit editor argv: [\"/bin/true\"]") != std::string_view::npos;
  }).find("Explicit editor argv: [\"/bin/true\"]") != std::string::npos);
  REQUIRE(reopened.type("q"));
}

TEST_CASE("UI-12 local VLESS form saves a manual node and Group as a Profile outlet", "[UI-12][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.type("2n"));
  REQUIRE(tui.type("Home")); REQUIRE(tui.key("Enter"));
  REQUIRE(tui.wait_for([](std::string_view grid) {
    return grid.find("Profile selected for editing") != std::string_view::npos;
  }).find("Home") != std::string::npos);
  REQUIRE(tui.type("5a1"));
  for (const auto& value : {"vless", "Local fixture", "127.0.0.1", "19001",
                             "00000000-0000-4000-8000-000000000111", "raw", "none"}) {
    REQUIRE(tui.type(value)); REQUIRE(tui.key("Enter"));
  }
  auto grid = tui.wait_for([](std::string_view screen) {
    return screen.find("Manual node saved to Local") != std::string_view::npos &&
           screen.find("Local fixture") != std::string_view::npos;
  });
  REQUIRE(grid.find("Local fixture") != std::string::npos);
  REQUIRE(tui.type("/Local")); REQUIRE(tui.key("Enter"));
  REQUIRE(tui.wait_for([](std::string_view screen) {
    return screen.find("Search: Local") != std::string_view::npos;
  }).find("Local fixture") != std::string::npos);
  REQUIRE(tui.type("["));
  CHECK(tui.wait_for([](std::string_view screen) {
    return screen.find("[All Subscriptions]") != std::string_view::npos;
  }).find("Search: —") != std::string::npos);
  REQUIRE(tui.type("]"));
  CHECK(tui.wait_for([](std::string_view screen) {
    return screen.find("[Local]") != std::string_view::npos;
  }).find("Search: Local") != std::string::npos);
  const auto listed = Json::parse(cli(daemon.socket(), {"node", "list", "--limit", "20"}));
  REQUIRE(listed.value("total", 0) == 1);
  const auto node_id = listed.at("items").at(0).value("id", "");
  REQUIRE_FALSE(node_id.empty());
  CHECK(listed.at("items").at(0).at("source_id").is_null());
  REQUIRE(tui.type("a4"));
  for (const auto& value : {"Local group", "automatic", "random", "local"}) {
    REQUIRE(tui.type(value)); REQUIRE(tui.key("Enter"));
  }
  grid = tui.wait_for([](std::string_view screen) {
    return screen.find("Group saved as a routing balancer") != std::string_view::npos &&
           screen.find("Local group") != std::string_view::npos;
  });
  REQUIRE(grid.find("Local group") != std::string::npos);
  const auto profiles = Json::parse(cli(daemon.socket(), {"profile", "list"}));
  REQUIRE(profiles.size() == 1);
  const auto profile_id = profiles.at(0).value("id", "");
  const auto groups = Json::parse(cli(daemon.socket(), {"group", "list", profile_id}));
  REQUIRE(groups.size() == 1);
  CHECK(groups.at(0).value("name", "") == "Local group");
  CHECK(groups.at(0).at("members").value("include_local", false));
  REQUIRE(tui.type("3"));
  grid = tui.wait_for([](std::string_view screen) {
    return screen.find("routing") != std::string_view::npos;
  });
  CHECK(grid.find("routing") != std::string::npos);
  REQUIRE(tui.type("q"));
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
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.type("4"));
  REQUIRE(tui.wait_for([](std::string_view grid) {
    return grid.find("[4 Subscriptions]") != std::string_view::npos &&
           grid.find("History Fixture") != std::string_view::npos;
  }).find("History Fixture") != std::string::npos);
  REQUIRE(tui.type("h"));
  auto grid = tui.wait_for([](std::string_view value) {
    return value.find("History cleanup") != std::string_view::npos &&
           value.find("current 1") != std::string_view::npos &&
           value.find("historical 2") != std::string_view::npos;
  });
  INFO("history preview grid: " << grid);
  REQUIRE(grid.find("History cleanup") != std::string::npos);
  CHECK(Json::parse(cli(daemon.socket(), {"node", "history", "preview", source.id})).value("history_count", 0) == 2);
  REQUIRE(tui.key("Escape"));
  CHECK(Json::parse(cli(daemon.socket(), {"node", "history", "preview", source.id})).value("history_count", 0) == 2);
  REQUIRE(tui.type("h"));
  grid = tui.wait_for([](std::string_view value) { return value.find("History cleanup") != std::string_view::npos; });
  REQUIRE(grid.find("History cleanup") != std::string::npos);
  // The fixture rows are ordered by stable ID; the distinct-name row is blocked.
  REQUIRE(tui.key("Down"));
  REQUIRE(tui.key("Space"));
  REQUIRE(tui.type("c"));
  grid = tui.wait_for([](std::string_view value) { return value.find("Confirm deletion") != std::string_view::npos; });
  REQUIRE(grid.find("Confirm deletion") != std::string::npos);
  REQUIRE(tui.key("Escape"));
  CHECK(Json::parse(cli(daemon.socket(), {"node", "history", "preview", source.id})).value("history_count", 0) == 2);
  REQUIRE(tui.type("c"));
  REQUIRE(tui.type("y"));
  grid = tui.wait_for([](std::string_view value) {
    return value.find("Removed 1 selected historical records") != std::string_view::npos;
  });
  INFO("history result grid: " << grid);
  REQUIRE(grid.find("Removed 1 selected historical records") != std::string::npos);
  const auto after = Json::parse(cli(daemon.socket(), {"node", "history", "preview", source.id}));
  CHECK(after.value("current_count", 0) == 1);
  CHECK(after.value("history_count", 0) == 1);
  CHECK(after.at("candidates").front().value("id", "") == "old-different");
}

TEST_CASE("TUI offers confirmed local SOCKS repair for an existing empty-inbounds Profile", "[PROFILE-LOCAL-TUI][port10808]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE,
                                                      "--name", "legacy fixture"}));
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Legacy",
                                                         "--core", core.at("id").get<std::string>()}));
  const auto id = profile.at("id").get<std::string>();
  REQUIRE_FALSE(cli(daemon.socket(), {"config", "edit", id, "inbounds", "--text", "{\"inbounds\":[]}"}).empty());
  REQUIRE_FALSE(cli(daemon.socket(), {"config", "disable", id, "inbounds"}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  auto grid = tui.wait_for([](std::string_view value) {
    return value.find("Current: Overview") != std::string_view::npos &&
           value.find("inbounds disabled") != std::string_view::npos;
  });
  INFO("legacy profile before repair: " << grid);
  REQUIRE(grid.find("inbounds disabled") != std::string::npos);
  const int occupied = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(occupied >= 0);
  const int reuse = 1;
  REQUIRE(::setsockopt(occupied, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
  sockaddr_in occupied_address{};
  occupied_address.sin_family = AF_INET;
  occupied_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  occupied_address.sin_port = htons(10808);
  REQUIRE(::bind(occupied, reinterpret_cast<const sockaddr*>(&occupied_address), sizeof(occupied_address)) == 0);
  REQUIRE(::listen(occupied, 1) == 0);
  REQUIRE(tui.type("i"));
  grid = tui.wait_for([](std::string_view value) {
    return value.find("127.0.0.1:10808 is currently unavailable") != std::string_view::npos;
  });
  REQUIRE(grid.find("127.0.0.1:10808 is currently unavailable") != std::string::npos);
  ::close(occupied);
  REQUIRE(tui.type("i"));
  grid = tui.wait_for([](std::string_view value) {
    return value.find("Add local proxy entry") != std::string_view::npos &&
           value.find("127.0.0.1:10808") != std::string_view::npos;
  });
  REQUIRE(grid.find("Add local proxy entry") != std::string::npos);
  REQUIRE(tui.key("Escape"));
  auto before = raw_rpc(daemon.socket(), "module.get", Json{{"profile_id", id}, {"module", "inbounds"}});
  REQUIRE(before.value("ok", false));
  CHECK(before["result"].at("text") == "{\"inbounds\":[]}");
  REQUIRE(tui.type("i"));
  REQUIRE(tui.wait_for([](std::string_view value) {
    return value.find("Add local proxy entry") != std::string_view::npos;
  }).find("Add local proxy entry") != std::string::npos);
  REQUIRE(tui.type("y"));
  grid = tui.wait_for([](std::string_view value) {
    return value.find("enable inbounds in Modules") != std::string_view::npos;
  });
  INFO("legacy profile after confirmed source save: " << grid);
  REQUIRE(grid.find("enable inbounds in Modules") != std::string::npos);
  auto after = raw_rpc(daemon.socket(), "module.get", Json{{"profile_id", id}, {"module", "inbounds"}});
  REQUIRE(after.value("ok", false));
  CHECK_FALSE(after["result"].value("included", true));
  const auto body = Json::parse(after["result"].at("text").get<std::string>());
  REQUIRE(body.at("inbounds").size() == 1);
  CHECK(body["inbounds"][0].value("protocol", "") == "socks");
  CHECK(body["inbounds"][0].value("listen", "") == "127.0.0.1");
  CHECK(body["inbounds"][0].value("port", 0) == 10808);
}

TEST_CASE("TUI local-use entry keeps an existing custom inbound intact", "[PROFILE-LOCAL-TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  const auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE,
                                                      "--name", "custom fixture"}));
  const auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Custom",
                                                         "--core", core.at("id").get<std::string>()}));
  const auto id = profile.at("id").get<std::string>();
  const auto custom = R"({"inbounds":[{"tag":"my-server","listen":"127.0.0.1","port":14001,"protocol":"vless","settings":{"clients":[],"unknown":"kept"},"vendor":{"flag":true}}]})";
  REQUIRE_FALSE(cli(daemon.socket(), {"config", "edit", id, "inbounds", "--text", custom}).empty());
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.wait_for([](std::string_view value) {
    return value.find("no local SOCKS entry") != std::string_view::npos;
  }).find("no local SOCKS entry") != std::string::npos);
  REQUIRE(tui.type("i"));
  REQUIRE(tui.wait_for([](std::string_view value) {
    return value.find("Existing inbounds are preserved") != std::string_view::npos;
  }).find("Existing inbounds are preserved") != std::string::npos);
  REQUIRE(tui.type("y"));
  REQUIRE(tui.wait_for([](std::string_view value) {
    return value.find("Local SOCKS 127.0.0.1:10808 saved") != std::string_view::npos;
  }).find("Local SOCKS 127.0.0.1:10808 saved") != std::string::npos);
  auto changed = raw_rpc(daemon.socket(), "module.get", Json{{"profile_id", id}, {"module", "inbounds"}});
  REQUIRE(changed.value("ok", false));
  const auto body = Json::parse(changed["result"].at("text").get<std::string>());
  REQUIRE(body.at("inbounds").size() == 2);
  CHECK(body["inbounds"][0] == Json::parse(custom).at("inbounds")[0]);
  CHECK(body["inbounds"][1].value("protocol", "") == "socks");
  const auto stale_creation = raw_rpc(daemon.socket(), "module.save", Json{
      {"profile_id", id}, {"module", "inbounds"}, {"text", "{\"inbounds\":[]}"},
      {"expected_absent", true}});
  CHECK_FALSE(stale_creation.value("ok", true));
  const auto after_rejected = raw_rpc(daemon.socket(), "module.get", Json{{"profile_id", id}, {"module", "inbounds"}});
  CHECK(after_rejected.at("result").at("text") == changed.at("result").at("text"));
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
  CapturedTui tui(daemon.socket()); REQUIRE(tui.started());
  REQUIRE(tui.type("5"));
  auto grid = tui.wait_for([](std::string_view value) {
    return value.find("[5 Nodes]") != std::string_view::npos &&
           value.find("1205 current") != std::string_view::npos;
  }, std::chrono::seconds(15));
  REQUIRE(grid.find("1205 current") != std::string::npos);
  REQUIRE(tui.key("End"));
  grid = tui.wait_for([](std::string_view value) { return visible_selected_row(value, "Node-1204"); });
  INFO("current terminal grid at End: " << grid);
  REQUIRE(visible_selected_row(grid, "Node-1204"));
  CHECK(grid.find("[5 Nodes]") != std::string::npos);
  CHECK(visibly_inverted(tui.grid(true), "Node-1204"));
  REQUIRE(tui.resize(60, 20));
  grid = tui.wait_for([](std::string_view value) { return visible_selected_row(value, "Node-1204"); });
  REQUIRE(visible_selected_row(grid, "Node-1204"));
  REQUIRE(tui.resize(100, 32));
  grid = tui.wait_for([](std::string_view value) { return visible_selected_row(value, "Node-1204"); });
  REQUIRE(visible_selected_row(grid, "Node-1204"));
  REQUIRE(tui.key("PPage"));
  grid = tui.wait_for([](std::string_view value) { return visible_selected_row(value, "Node-1194"); });
  CHECK(visible_selected_row(grid, "Node-1194"));
  REQUIRE(tui.key("Home"));
  grid = tui.wait_for([](std::string_view value) { return visible_selected_row(value, "Node-0000"); });
  CHECK(visible_selected_row(grid, "Node-0000"));
  REQUIRE(tui.key("Up"));
  CHECK(visible_selected_row(tui.wait_for([](std::string_view value) {
    return visible_selected_row(value, "Node-0000");
  }), "Node-0000"));
  for (int count = 0; count < 20; ++count) REQUIRE(tui.key("Down"));
  grid = tui.wait_for([](std::string_view value) { return visible_selected_row(value, "Node-0020"); });
  CHECK(visible_selected_row(grid, "Node-0020"));
  REQUIRE(tui.key("Home"));
  REQUIRE(tui.key("NPage"));
  grid = tui.wait_for([](std::string_view value) { return visible_selected_row(value, "Node-0010"); });
  CHECK(visible_selected_row(grid, "Node-0010"));
  REQUIRE(tui.key("End"));
  REQUIRE(visible_selected_row(tui.wait_for([](std::string_view value) {
    return visible_selected_row(value, "Node-1204");
  }), "Node-1204"));
  REQUIRE(tui.type("4"));
  REQUIRE(tui.wait_for([](std::string_view value) { return value.find("[4 Subscriptions]") != std::string_view::npos; })
              .find("[4 Subscriptions]") != std::string::npos);
  REQUIRE(tui.type("u"));
  const auto refreshed = tui.wait_for([](std::string_view value) {
    return value.find("[4 Subscriptions]") != std::string_view::npos &&
           value.find("success") != std::string_view::npos;
  }, std::chrono::seconds(15));
  CHECK(refreshed.find("success") != std::string::npos);
  REQUIRE(tui.type("5"));
  grid = tui.wait_for([](std::string_view value) { return visible_selected_row(value, "Node-1204"); });
  CHECK(visible_selected_row(grid, "Node-1204"));
  CHECK(valid_utf8(grid));
  REQUIRE(tui.type("/"));
  REQUIRE(tui.type("Node-1204"));
  REQUIRE(tui.key("Enter"));
  grid = tui.wait_for([](std::string_view value) {
    return value.find("showing 1") != std::string_view::npos &&
           visible_selected_row(value, "Node-1204");
  });
  CHECK(grid.find("showing 1") != std::string::npos);
  CHECK(visible_selected_row(grid, "Node-1204"));
  REQUIRE(tui.type("q"));
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
  auto tui = start_tui(daemon.socket(), profile); REQUIRE(tui.pid > 0);
  auto initial = read_screen(tui.master, std::chrono::milliseconds(1500));
  int initial_status = 0;
  const auto initial_wait = ::waitpid(tui.pid, &initial_status, WNOHANG);
  INFO("initial PTY bytes=" << initial.size() << " screen=" << initial << " child_wait=" << initial_wait
       << " child_status=" << initial_status);
  CHECK(initial.find("? Help") != std::string::npos);
  keys(tui.master, "?");
  const auto help = read_screen(tui.master);
  INFO("help PTY bytes=" << help.size() << " screen=" << help);
  CHECK(help.find("Help") != std::string::npos);
  CHECK(help.find("Tab") != std::string::npos);
  keys(tui.master, "\x1b");
  keys(tui.master, "\t");
  const auto tabbed = read_screen(tui.master);
  CHECK(tabbed.find("Current: Profiles") != std::string::npos);
  keys(tui.master, "\x1b[Z");
  const auto shifted = read_screen(tui.master);
  INFO("shift-tab PTY bytes=" << shifted.size() << " screen=" << shifted);
  CHECK(shifted.find("Overview") != std::string::npos);
  keys(tui.master, "5");
  keys(tui.master, "]");
  auto first = read_screen(tui.master);
  INFO("nodes initial screen=" << first);
  CHECK(first.find("alpha-node") != std::string::npos);
  CHECK(valid_utf8(first));
  keys(tui.master, "/qe ");
  auto filter_input = read_screen(tui.master);
  int status = 0;
  CHECK(::waitpid(tui.pid, &status, WNOHANG) == 0);
  CHECK(filter_input.find("Filter nodes: qe ") != std::string::npos);
  keys(tui.master, "\x1b");
  keys(tui.master, "/中文");
  keys(tui.master, "\r");
  auto chinese = read_screen(tui.master);
  INFO("Chinese node screen=" << chinese);
  CHECK(chinese.find("中文节点") != std::string::npos);
  CHECK(valid_utf8(chinese));
  keys(tui.master, "/node");
  keys(tui.master, "\r");
  auto emoji = read_screen(tui.master);
  INFO("emoji node screen=" << emoji);
  CHECK(emoji.find("🚀-node") != std::string::npos);
  CHECK(valid_utf8(emoji));
  keys(tui.master, "/node");
  keys(tui.master, "\r");
  keys(tui.master, "\x1b[B");
  keys(tui.master, "r");
  const auto refreshed = read_screen(tui.master, std::chrono::milliseconds(500));
  INFO("selection after refresh=" << refreshed);
  CHECK(refreshed.find("▸ ·  🚀-node") != std::string::npos);
  CHECK(valid_utf8(refreshed));
  keys(tui.master, "/long");
  keys(tui.master, "\r");
  auto long_name = read_screen(tui.master);
  INFO("long name screen=" << long_name);
  CHECK(long_name.find("long-") != std::string::npos);
  CHECK(valid_utf8(long_name));
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
}

TEST_CASE("UI-02/T25 logs expose pause and resume actions on the actual TUI", "[UI-02][T25][I52]") {
  TempDir temp;
  auto store = Store::open(temp.path / "state.sqlite"); REQUIRE(store);
  Node node; node.id = "log-node"; node.original_name = "logs";
  node.settings = RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
  REQUIRE((*store)->put_node(node));
  store->reset();
  Daemon daemon(temp.path);
  REQUIRE(daemon.start());
  auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE, "--name", "log fixture"}));
  auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "logs", "--core", core.at("id").get<std::string>()}));
  REQUIRE(!profile.value("id", "").empty());
  set_profile_process_environment(daemon.socket(), temp.path, profile.at("id").get<std::string>(), Json{
      {"XRM_FAKE_CORE_STDOUT", "BOOT_LOG_MARKER"},
      {"XRM_FAKE_CORE_LOG_COUNT", "1000"},
      {"XRM_FAKE_CORE_LOG_INTERVAL_MS", "10"}});
  REQUIRE(!cli(daemon.socket(), {"select", "node", profile.at("id").get<std::string>(), "log-node"}).empty());
  const auto connect_result = cli(daemon.socket(), {"connect", profile.at("id").get<std::string>()});
  REQUIRE(!connect_result.empty());
  bool running = false;
  const auto running_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!running && std::chrono::steady_clock::now() < running_deadline) {
    const auto status = cli(daemon.socket(), {"status"});
    if (!status.empty()) running = Json::parse(status).value("state", "") == "running";
    if (!running) ::poll(nullptr, 0, 20);
  }
  REQUIRE(running);

  auto tui = start_tui(daemon.socket(), profile.at("id").get<std::string>());
  REQUIRE(tui.pid > 0);
  read_screen(tui.master, std::chrono::milliseconds(700));
  keys(tui.master, "6");
  auto logs = terminal_text(read_screen(tui.master, std::chrono::milliseconds(1000)));
  CHECK(logs.find("Logs — FOLLOW") != std::string::npos);
  keys(tui.master, "?");
  const auto help = terminal_text(read_screen(tui.master));
  CHECK(help.find("Logs: p pause/resume") != std::string::npos);
  keys(tui.master, "\x1b");
  read_screen(tui.master);
  const auto followed_top = log_view_top(logs);
  REQUIRE(followed_top.has_value());
  CHECK(logs.find("XRM_FAKE_CORE_LOG_") != std::string::npos);
  auto followed_again = terminal_text(read_screen(tui.master, std::chrono::milliseconds(500)));
  const auto followed_top_again = log_view_top(followed_again);
  REQUIRE(followed_top_again.has_value());
  CHECK(*followed_top_again > *followed_top);
  keys(tui.master, "p");
  auto paused = terminal_text(read_screen(tui.master));
  CHECK(paused.find("PAUSED") != std::string::npos);
  const auto paused_top = log_view_top(paused);
  REQUIRE(paused_top.has_value());
  auto background = terminal_text(read_screen(tui.master, std::chrono::milliseconds(600)));
  CHECK(background.find("new logs:") != std::string::npos);
  CHECK(log_view_top(background) == paused_top);
  keys(tui.master, "\x1b[A");
  auto history = terminal_text(read_screen(tui.master));
  CHECK(log_view_top(history).value_or(*paused_top) < *paused_top);
  keys(tui.master, "p");
  auto resumed = wait_screen_text(tui.master, [&](const std::string& text) {
    const auto top = log_view_top(text);
    return text.find("FOLLOW") != std::string::npos && top && *top > *paused_top;
  });
  CHECK(resumed.find("FOLLOW") != std::string::npos);
  const auto resumed_top = log_view_top(resumed);
  REQUIRE(resumed_top.has_value());
  CHECK(*resumed_top > *paused_top);
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
  const auto after_exit = Json::parse(cli(daemon.socket(), {"status"}));
  CHECK(after_exit.value("state", "") == "running");
}

TEST_CASE("UI-03 overview distinguishes the saved target from the running target", "[UI-03][I52]") {
  TempDir temp;
  auto store = Store::open(temp.path / "state.sqlite"); REQUIRE(store);
  for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{
      {"target-a", "Target A"}, {"target-b", "Target B"}}) {
    Node node; node.id = id; node.original_name = name;
    node.settings = RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
    REQUIRE((*store)->put_node(node));
  }
  store->reset();
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE, "--name", "target fixture"}));
  auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "targets", "--core", core.at("id").get<std::string>()}));
  const auto profile_id = profile.at("id").get<std::string>();
  REQUIRE(!cli(daemon.socket(), {"select", "node", profile_id, "target-a"}).empty());
  REQUIRE(!cli(daemon.socket(), {"connect", profile_id}).empty());
  bool running = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!running && std::chrono::steady_clock::now() < deadline) {
    const auto status = cli(daemon.socket(), {"status"});
    if (!status.empty()) running = Json::parse(status).value("state", "") == "running";
    if (!running) ::poll(nullptr, 0, 20);
  }
  REQUIRE(running);
  REQUIRE(!cli(daemon.socket(), {"select", "node", profile_id, "target-b"}).empty());

  auto tui = start_tui(daemon.socket(), profile_id); REQUIRE(tui.pid > 0);
  const auto overview = terminal_text(read_screen(tui.master, std::chrono::milliseconds(900)));
  INFO("target dashboard: " << overview);
  CHECK(overview.find("Saved target: node: Target B") != std::string::npos);
  CHECK(overview.find("Actual target: node: Target A") != std::string::npos);
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
  const auto status = Json::parse(cli(daemon.socket(), {"status"}));
  CHECK(status.value("state", "") == "running");
  CHECK(status.at("active").at("selection").value("id_or_tag", "") == "target-a");
}

TEST_CASE("disabled native modules without source open safely and remain disabled", "[product-repair][UI-04]") {
  TempDir temp;
  auto store = Store::open(temp.path / "state.sqlite"); REQUIRE(store);
  store->reset();
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE, "--name", "empty module fixture"}));
  auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "empty", "--core", core.at("id").get<std::string>()}));
  const auto profile_id = profile.at("id").get<std::string>();
  auto modules = Json::parse(cli(daemon.socket(), {"config", "modules", profile_id}));
  REQUIRE(modules.size() == 15);
  const auto profile_root = temp.path / "profiles" / profile_id / "modules";

  auto tui = start_tui(daemon.socket(), profile_id);
  REQUIRE(tui.pid > 0);
  read_screen(tui.master, std::chrono::milliseconds(600));
  keys(tui.master, "3");
  read_screen(tui.master);
  for (std::size_t i = 0; i < modules.size(); ++i) {
    if (i != 0) keys(tui.master, "\x1b[B");
    keys(tui.master, "\r");
    const auto opened = terminal_text(read_screen(tui.master, std::chrono::milliseconds(120)));
    int status = 0;
    const auto child_state = ::waitpid(tui.pid, &status, WNOHANG);
    INFO("module=" << modules[i].value("key", "") << " screen=" << opened << " waitpid=" << child_state << " status=" << status);
    CHECK(child_state == 0);
    CHECK(opened.find("Native module editor") != std::string::npos);
    if (modules[i].at("text").is_null()) {
      CHECK(opened.find("Not configured yet") != std::string::npos);
      CHECK_FALSE(modules[i].value("included", false));
      CHECK_FALSE(std::filesystem::exists(profile_root / modules[i].at("runtime_file").get<std::string>()));
    }
    keys(tui.master, "\x1b");
    read_screen(tui.master);
  }
  keys(tui.master, "\x1b");
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
}

TEST_CASE("first use from xrm tui needs no Profile ID and completes local subscription to connect", "[product-repair][UI-01][E2E-06][TUI-SUB-01][UI-R01][UI-R06]") {
  TempDir temp;
  const std::string node_uri = "vless://00000000-0000-4000-8000-000000000111@127.0.0.1:9?type=tcp&security=none#TUI-first-node";
  LocalSubscriptionServer subscription(node_uri + "\n", std::chrono::milliseconds(650));
  REQUIRE(subscription.valid());
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto tui = start_tui(daemon.socket(), ""); REQUIRE(tui.pid > 0);
  auto overview = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("First use") != std::string::npos;
  });
  CHECK(overview.find("First use") != std::string::npos);
  CHECK(overview.find("First use") != std::string::npos);

  keys(tui.master, "4");
  auto subscriptions_page = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Subscriptions") != std::string::npos;
  });
  CHECK(subscriptions_page.find("Subscriptions") != std::string::npos);
  keys(tui.master, "a");
  auto add_modal = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Subscription name") != std::string::npos;
  });
  CHECK(add_modal.find("Subscription name") != std::string::npos);
  CHECK(add_modal.find("Subscriptions") != std::string::npos);
  keys(tui.master, "local fixture\r");
  keys(tui.master, subscription.url() + "\r");
  auto subscription_added = terminal_text(read_screen(tui.master));
  CHECK((subscription_added.find("Subscription URL") != std::string::npos ||
         subscription_added.find("subscription saved") != std::string::npos));
  keys(tui.master, "u");
  auto updating = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Contacting xrmd") != std::string::npos;
  });
  CHECK(updating.find("Contacting xrmd") != std::string::npos);
  auto refreshed = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Subscription update complete") != std::string::npos &&
        text.find("1 current") != std::string::npos;
  }, std::chrono::seconds(5));
  CHECK(refreshed.find("Subscription update complete") != std::string::npos);
  CHECK(refreshed.find("1 current") != std::string::npos);
  // Completion must be delivered while this page remains open; no extra key,
  // manual refresh, reconnect, or runtime file generation is needed.
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
  CHECK(Json::parse(cli(daemon.socket(), {"profile", "list"})).empty());
  keys(tui.master, "5");
  auto node_screen = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("TUI-first-node") != std::string::npos;
  }, std::chrono::seconds(5));
  CHECK(node_screen.find("TUI-first-node") != std::string::npos);
  keys(tui.master, "5");
  node_screen = terminal_text(read_screen(tui.master));
  CHECK(node_screen.find("TUI-first-node") != std::string::npos);

  keys(tui.master, "7a");
  keys(tui.master, std::string(XRM_TEST_FAKE_CORE) + "\r");
  auto registered = terminal_text(read_screen(tui.master));
  CHECK(registered.find("existing Xray registered") != std::string::npos);

  keys(tui.master, "2n");
  keys(tui.master, "first profile\r");
  auto created = terminal_text(read_screen(tui.master));
  CHECK(created.find("Profile selected for editing") != std::string::npos);
  CHECK(created.find("first profile") != std::string::npos);

  // A subscription added before any Profile remains global. Explicitly bind
  // it to the newly created Profile before selecting its outlet.
  keys(tui.master, "4b");
  CHECK(terminal_text(read_screen(tui.master)).find("Source included in") != std::string::npos);
  keys(tui.master, "5");
  CHECK(terminal_text(read_screen(tui.master)).find("TUI-first-node") != std::string::npos);
  const auto profile_inventory = Json::parse(cli(daemon.socket(), {"profile", "list"}));
  REQUIRE(profile_inventory.is_array());
  REQUIRE(profile_inventory.size() == 1);
  const auto profile_id = profile_inventory.front().value("id", "");
  const auto core_run_argv = (temp.path / "first-use-core-run.argv").string();
  const auto core_check_argv = (temp.path / "first-use-core-check.argv").string();
  set_profile_process_environment(daemon.socket(), temp.path, profile_id,
      Json{{"XRM_FAKE_CORE_RUN_ARGV", core_run_argv}, {"XRM_FAKE_CORE_CHECK_ARGV", core_check_argv}});

  // Connecting the highlighted inventory row must use that row even when the
  // Profile did not previously have a saved selection. The old TUI sent only
  // profile_id and the assembler then failed for a missing default target.
  CHECK(Json::parse(cli(daemon.socket(), {"profile", "show", profile_id}))
            .value("selected", Json(nullptr)).is_null());
  keys(tui.master, "c");
  Json direct_status;
  const auto direct_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < direct_deadline) {
    const auto output = cli(daemon.socket(), {"status"});
    if (!output.empty()) direct_status = Json::parse(output);
    if (direct_status.value("state", "") == "running" || direct_status.value("state", "") == "failed") break;
    ::poll(nullptr, 0, 50);
  }
  INFO("direct node Connect status: " << direct_status.dump());
  REQUIRE(direct_status.value("state", "") == "running");
  REQUIRE((direct_status.contains("active") && direct_status.at("active").is_object()));
  CHECK(direct_status.at("active").at("selection").value("kind", "") == "node");
  CHECK(Json::parse(cli(daemon.socket(), {"profile", "show", profile_id}))
            .value("selected", Json(nullptr)).is_object());
  (void)wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Xray core running") != std::string::npos;
  }, std::chrono::seconds(5));
  keys(tui.master, "d");
  const auto stopped_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < stopped_deadline &&
         Json::parse(cli(daemon.socket(), {"status"})).value("state", "") != "stopped")
    ::poll(nullptr, 0, 50);
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");

  keys(tui.master, "s");
  const auto saved_profile = Json::parse(cli(daemon.socket(), {"profile", "show", profile_id}));
  INFO("saved profile selection: " << saved_profile.value("selected", Json(nullptr)).dump());
  REQUIRE(saved_profile.value("selected", Json(nullptr)).is_object());
  keys(tui.master, "1");
  const auto overview_before_connect = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Overview") != std::string::npos;
  });
  CHECK(overview_before_connect.find("Overview") != std::string::npos);
  keys(tui.master, "c");
  const auto connect_started = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Connect…") != std::string::npos ||
           text.find("Running Profile: first profile") != std::string::npos;
  }, std::chrono::seconds(5));
  INFO("connect feedback: " << connect_started);
  CHECK((connect_started.find("Connect…") != std::string::npos ||
         connect_started.find("Running Profile: first profile") != std::string::npos));
  Json status;
  const auto connected_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < connected_deadline) {
    auto text = cli(daemon.socket(), {"status"});
    if (!text.empty()) status = Json::parse(text);
    if (status.value("state", "") == "running" || status.value("state", "") == "failed") break;
    ::poll(nullptr, 0, 50);
  }
  INFO("status: " << status.dump());
  CHECK(status.value("state", "") == "running");
  CHECK(std::filesystem::exists(core_check_argv));
  CHECK(std::filesystem::exists(core_run_argv));
  REQUIRE((status.contains("active") && status.at("active").is_object()));
  CHECK(status.at("active").at("selection").value("kind", "") == "node");
  keys(tui.master, "r");
  CHECK(terminal_text(read_screen(tui.master)).find("Profile") != std::string::npos);

  keys(tui.master, "6");
  CHECK(terminal_text(read_screen(tui.master)).find("Logs") != std::string::npos);
  keys(tui.master, "1");
  CHECK(terminal_text(read_screen(tui.master)).find("Statistics") != std::string::npos);
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "running");
  CHECK(!cli(daemon.socket(), {"disconnect"}).empty());
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
}

TEST_CASE("TUI-SUB-02 failed refresh is visible and keeps the prior node inventory", "[TUI-SUB-02][UI-R01][UI-R06]") {
  TempDir temp;
  auto store = Store::open(temp.path / "state.sqlite"); REQUIRE(store);
  Node old; old.id = "preexisting-manual"; old.original_name = "kept-node";
  old.settings = RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
  REQUIRE((*store)->put_node(old));
  store->reset();
  LocalSubscriptionServer subscription("unavailable\n", std::chrono::milliseconds(350), 403);
  REQUIRE(subscription.valid());
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto tui = start_tui(daemon.socket(), ""); REQUIRE(tui.pid > 0);
  read_screen(tui.master, std::chrono::milliseconds(500));
  keys(tui.master, "4a");
  keys(tui.master, "403 fixture\r");
  keys(tui.master, subscription.url() + "\r");
  read_screen(tui.master);
  keys(tui.master, "u");
  const auto updating = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Contacting xrmd") != std::string::npos;
  });
  CHECK(updating.find("Contacting xrmd") != std::string::npos);
  keys(tui.master, "5");
  const auto result = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("HTTP 403") != std::string::npos;
  }, std::chrono::seconds(5));
  INFO("failed refresh screen: " << result);
  CHECK(result.find("HTTP 403") != std::string::npos);
  // Manual inventory is deliberately under Local, not All Subscriptions.
  keys(tui.master, "]]");
  const auto local = terminal_text(read_screen(tui.master));
  CHECK(local.find("kept-node") != std::string::npos);
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
}

TEST_CASE("TUI-SUB-03 partial refresh shows imported, unsupported and malformed counts", "[TUI-SUB-03][UI-R01]") {
  TempDir temp;
  const std::string body =
      "vless://00000000-0000-4000-8000-000000000111@127.0.0.1:9?type=tcp&security=none#partial-node\n"
      "anytls://opaque\nnot-a-uri\n";
  LocalSubscriptionServer subscription(body, std::chrono::milliseconds(350));
  REQUIRE(subscription.valid());
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto tui = start_tui(daemon.socket(), ""); REQUIRE(tui.pid > 0);
  read_screen(tui.master, std::chrono::milliseconds(500));
  keys(tui.master, "4a");
  keys(tui.master, "partial fixture\r");
  keys(tui.master, subscription.url() + "\r");
  read_screen(tui.master);
  keys(tui.master, "u");
  const auto updating = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("Contacting xrmd") != std::string::npos;
  });
  CHECK(updating.find("Contacting xrmd") != std::string::npos);
  keys(tui.master, "5");
  const auto result = wait_screen_text(tui.master, [](const std::string& text) {
    return text.find("partial-node") != std::string::npos &&
        text.find("Subscription update partial") != std::string::npos;
  }, std::chrono::seconds(5));
  CHECK(result.find("partial-node") != std::string::npos);
  CHECK(result.find("Subscription update partial") != std::string::npos);
  CHECK(result.find("1 unsupported") != std::string::npos);
  CHECK(result.find("1 malformed") != std::string::npos);
  keys(tui.master, "4");
  keys(tui.master, "\r");
  const auto row = terminal_text(read_screen(tui.master));
  CHECK(row.find("Unsupported: 1") != std::string::npos);
  CHECK(row.find("malformed: 1") != std::string::npos);
  keys(tui.master, "\x1b");
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
}

TEST_CASE("UI-06/UI-07 zero-profile startup and Profile management use names", "[UI-06][UI-07][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto tui = start_tui(daemon.socket(), ""); REQUIRE(tui.pid > 0);
  (void)read_screen(tui.master, std::chrono::milliseconds(1500));
  keys(tui.master, "1");

  auto overview = terminal_text(read_screen(tui.master, std::chrono::milliseconds(500)));
  CHECK(overview.find("Viewing Profile") != std::string::npos);
  CHECK(overview.find("No profile selected") != std::string::npos);

  keys(tui.master, "4");
  auto subscriptions = terminal_text(read_screen(tui.master));
  CHECK(subscriptions.find("Subscriptions") != std::string::npos);
  CHECK(subscriptions.find("No subscriptions yet") != std::string::npos);

  keys(tui.master, "5");
  auto nodes = terminal_text(read_screen(tui.master));
  CHECK(nodes.find("Nodes") != std::string::npos);

  keys(tui.master, "7");
  auto cores = terminal_text(read_screen(tui.master));
  CHECK(cores.find("Core") != std::string::npos);

  keys(tui.master, "2");
  auto profiles = terminal_text(read_screen(tui.master));
  CHECK(profiles.find("Profiles") != std::string::npos);
  CHECK(profiles.find("No Profile") != std::string::npos);

  keys(tui.master, "5g");
  auto groups = terminal_text(read_screen(tui.master));
  CHECK(groups.find("No profile selected") != std::string::npos);
  CHECK(groups.find("Create a profile to use this feature") != std::string::npos);

  keys(tui.master, "3");
  auto modules = terminal_text(read_screen(tui.master));
  CHECK(modules.find("No profile selected") != std::string::npos);
  CHECK(modules.find("Create a profile to use this feature") != std::string::npos);

  keys(tui.master, "c");
  CHECK(terminal_text(read_screen(tui.master)).find("Create Default Profile") != std::string::npos);
  keys(tui.master, "\x1b");

  keys(tui.master, "7a");
  keys(tui.master, std::string(XRM_TEST_FAKE_CORE) + "\r");
  CHECK(terminal_text(read_screen(tui.master)).find("existing Xray registered") != std::string::npos);
  keys(tui.master, "2n");
  keys(tui.master, "Home\r");
  CHECK(terminal_text(read_screen(tui.master)).find("Home") != std::string::npos);
  keys(tui.master, "r");
  keys(tui.master, "Router\r");
  CHECK(terminal_text(read_screen(tui.master)).find("Router") != std::string::npos);
  keys(tui.master, "c");
  keys(tui.master, "Test\r");
  CHECK(terminal_text(read_screen(tui.master)).find("Test") != std::string::npos);
  keys(tui.master, "d");
  CHECK(terminal_text(read_screen(tui.master)).find("Profile removed") != std::string::npos);

  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
  const auto remaining = Json::parse(cli(daemon.socket(), {"profile", "list"}));
  REQUIRE(remaining.is_array());
  REQUIRE(remaining.size() == 1);
  CHECK(remaining.front().value("name", "") == "Router");
}

TEST_CASE("UI-07 Profile management is name-oriented and browsing keeps the running Profile", "[UI-07][UI-08][TUI]") {
  TempDir temp;
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE, "--name", "profile UX core"}));
  auto home = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Home", "--core", core.at("id").get<std::string>()}));
  auto office = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "Office", "--core", core.at("id").get<std::string>()}));
  const auto home_id = home.at("id").get<std::string>();
  const auto office_id = office.at("id").get<std::string>();
  auto node = Json::parse(cli(daemon.socket(), {"node", "add", "--name", "profile UX direct",
      "--outbound-json", R"({"protocol":"freedom","settings":{},"tag":"profile-ux-direct"})"}));
  const auto node_id = node.at("id").get<std::string>();
  REQUIRE(!cli(daemon.socket(), {"select", "node", home_id, node_id}).empty());
  REQUIRE(!cli(daemon.socket(), {"select", "node", office_id, node_id}).empty());
  REQUIRE(!cli(daemon.socket(), {"connect", home_id}).empty());
  const auto profile_rows = Json::parse(cli(daemon.socket(), {"profile", "list"}));
  REQUIRE(profile_rows.is_array());
  const auto home_row = std::find_if(profile_rows.begin(), profile_rows.end(), [&](const Json& row) { return row.value("id", "") == home_id; });
  const auto office_row = std::find_if(profile_rows.begin(), profile_rows.end(), [&](const Json& row) { return row.value("id", "") == office_id; });
  REQUIRE(home_row != profile_rows.end());
  REQUIRE(office_row != profile_rows.end());
  const auto home_index = static_cast<std::size_t>(home_row - profile_rows.begin());
  const auto office_index = static_cast<std::size_t>(office_row - profile_rows.begin());
  auto picker = start_tui(daemon.socket(), ""); REQUIRE(picker.pid > 0);
  const auto picker_screen = terminal_text(read_screen(picker.master, std::chrono::milliseconds(1200)));
  CHECK(picker_screen.find("Profiles") != std::string::npos);
  CHECK(picker_screen.find("Home") != std::string::npos);
  CHECK(picker_screen.find("Office") != std::string::npos);
  CHECK(picker_screen.find("editing") == std::string::npos);
  int picker_status = 0;
  CHECK(::waitpid(picker.pid, &picker_status, WNOHANG) == 0);
  keys(picker.master, "q");
  REQUIRE(picker.wait(std::chrono::seconds(3)));

  auto tui = start_tui(daemon.socket(), "Home"); REQUIRE(tui.pid > 0);
  (void)read_screen(tui.master, std::chrono::milliseconds(1200));
  Json before;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto response = cli(daemon.socket(), {"status"});
    if (!response.empty()) before = Json::parse(response);
    if (before.value("state", "") == "running" || before.value("state", "") == "failed") break;
    ::poll(nullptr, 0, 30);
  }
  REQUIRE(before.value("state", "") == "running");

  keys(tui.master, "2");
  const auto profile_page = terminal_text(read_screen(tui.master));
  CHECK(profile_page.find("Home") != std::string::npos);
  CHECK(profile_page.find("Office") != std::string::npos);
  CHECK(profile_page.find(home_id) == std::string::npos);
  CHECK(profile_page.find(office_id) == std::string::npos);

  std::string move_to_office;
  if (office_index > home_index)
    for (std::size_t i = home_index; i < office_index; ++i) move_to_office += "\x1b[B";
  else
    for (std::size_t i = office_index; i < home_index; ++i) move_to_office += "\x1b[A";
  move_to_office += "\r";
  keys(tui.master, move_to_office);
  const auto switched = terminal_text(read_screen(tui.master));
  CHECK(switched.find("Viewing: Office") != std::string::npos);
  CHECK(switched.find("Running: Home") != std::string::npos);
  const auto after = Json::parse(cli(daemon.socket(), {"status"}));
  CHECK(after.at("pid") == before.at("pid"));
  CHECK(after.at("active").at("confdir") == before.at("active").at("confdir"));
  CHECK(after.at("active").at("selection") == before.at("active").at("selection"));

  keys(tui.master, "1c");
  Json office_status;
  const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < connect_deadline) {
    const auto response = cli(daemon.socket(), {"status"});
    if (!response.empty()) office_status = Json::parse(response);
    if (office_status.contains("active") && office_status.at("active").is_object() &&
        office_status.at("active").value("profile_id", "") == office_id) break;
    ::poll(nullptr, 0, 30);
  }
  CHECK((office_status.contains("active") && office_status.at("active").is_object()));
  if (office_status.contains("active") && office_status.at("active").is_object())
    CHECK(office_status.at("active").value("profile_id", "") == office_id);
  keys(tui.master, "q");
  const auto after_quit_screen = terminal_text(read_screen(tui.master, std::chrono::milliseconds(400)));
  INFO("screen after q=" << after_quit_screen);
  CHECK(tui.wait(std::chrono::seconds(3)));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "running");
  (void)cli(daemon.socket(), {"disconnect"});
}

TEST_CASE("UI-05 opens a configured external editor from the native module draft", "[UI-05][I52]") {
  TempDir temp;
  auto store = Store::open(temp.path / "state.sqlite"); REQUIRE(store);
  store->reset();
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE, "--name", "editor fixture"}));
  auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "editor", "--core", core.at("id").get<std::string>()}));
  const auto profile_id = profile.at("id").get<std::string>();
  const auto module_text = [&](std::string_view expected) {
    const auto response = Json::parse(cli(daemon.socket(), {"config", "show", profile_id,
        "--module-key", "log", "--source", "source"}));
    return response.at("text").get<std::string>().find(expected) != std::string::npos;
  };
  const auto launch_editor = [&](std::string mode, std::string suffix) {
    const auto marker = (temp.path / ("editor-" + mode + ".txt")).string();
    auto tui = start_tui(daemon.socket(), profile_id,
        {std::string("VISUAL=") + XRM_TEST_FAKE_EDITOR + " --mode " + mode + " --marker " + marker,
         "EDITOR=/usr/bin/false"});
    REQUIRE(tui.pid > 0);
    const auto overview = wait_screen_text(tui.master, [](const std::string& text) {
      return text.find("Overview") != std::string::npos;
    });
    REQUIRE(overview.find("Overview") != std::string::npos);
    keys(tui.master, "3\r");
    const auto opened = wait_screen_text(tui.master, [](const std::string& text) {
      return text.find("Native module editor") != std::string::npos;
    });
    REQUIRE(opened.find("Native module editor") != std::string::npos);
    if (!suffix.empty()) keys(tui.master, suffix);
    keys(tui.master, "v");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < until)
      ::poll(nullptr, 0, 10);
    REQUIRE(std::filesystem::exists(marker));
    keys(tui.master, "\x1b[C");
    auto result = wait_screen_text(tui.master, [&](const std::string& text) {
      if (mode == "output") return text.find("[unsaved]") != std::string::npos && text.find("warning") != std::string::npos;
      if (mode == "nonzero") return text.find("external editor failed; draft kept") != std::string::npos;
      if (mode == "cancel") return text.find("external editor cancelled; draft kept") != std::string::npos;
      if (mode == "invalid") return text.find("invalid JSON retained") != std::string::npos;
      return true;
    });
    return std::pair{std::move(tui), std::move(result)};
  };

  const auto before = Json::parse(cli(daemon.socket(), {"config", "show", profile_id,
      "--module-key", "log", "--source", "source"})).at("text").get<std::string>();
  auto [normal_tui, normal] = launch_editor("output", "");
  CHECK(normal.find("FAKE_EDITOR_TERMINAL_OUTPUT") != std::string::npos);
  CHECK(normal.find("[unsaved]") != std::string::npos);
  CHECK(module_text(before));
  CHECK(normal.find("warning") != std::string::npos);
  keys(normal_tui.master, "\x13");
  const auto saved = terminal_text(read_screen(normal_tui.master));
  CHECK(saved.find("runtime inclusion is unchanged") != std::string::npos);
  CHECK(module_text("warning"));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
  keys(normal_tui.master, "\x1b"); keys(normal_tui.master, "q");
  CHECK(normal_tui.wait(std::chrono::seconds(3)));

  // VISUAL takes precedence; with it unset, EDITOR is used directly via argv.
  const auto fallback_marker = (temp.path / "editor-fallback.txt").string();
  auto fallback_tui = start_tui(daemon.socket(), profile_id,
      {"VISUAL=", std::string("EDITOR=") + XRM_TEST_FAKE_EDITOR + " --mode success --marker " + fallback_marker});
  REQUIRE(fallback_tui.pid > 0);
  wait_screen_text(fallback_tui.master, [](const std::string& text) {
    return text.find("Overview") != std::string::npos;
  });
  keys(fallback_tui.master, "3\r");
  REQUIRE(wait_screen_text(fallback_tui.master, [](const std::string& text) {
    return text.find("Native module editor") != std::string::npos;
  }).find("Native module editor") != std::string::npos);
  keys(fallback_tui.master, "v");
  const auto fallback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!std::filesystem::exists(fallback_marker) && std::chrono::steady_clock::now() < fallback_deadline)
    ::poll(nullptr, 0, 10);
  CHECK(std::filesystem::exists(fallback_marker));
  keys(fallback_tui.master, "\x1b"); keys(fallback_tui.master, "q");
  CHECK(fallback_tui.wait(std::chrono::seconds(3)));

  auto unset_tui = start_tui(daemon.socket(), profile_id, {"VISUAL=", "EDITOR="});
  REQUIRE(unset_tui.pid > 0);
  wait_screen_text(unset_tui.master, [](const std::string& text) {
    return text.find("Overview") != std::string::npos;
  });
  keys(unset_tui.master, "3\r");
  REQUIRE(wait_screen_text(unset_tui.master, [](const std::string& text) {
    return text.find("Native module editor") != std::string::npos;
  }).find("Native module editor") != std::string::npos);
  keys(unset_tui.master, "v"); keys(unset_tui.master, "\x1b[C");
  const auto no_editor = wait_screen_text(unset_tui.master, [](const std::string& text) {
    return text.find("VISUAL/EDITOR") != std::string::npos;
  });
  CHECK(no_editor.find("VISUAL/EDITOR") != std::string::npos);
  keys(unset_tui.master, "\x1b"); keys(unset_tui.master, "q");
  CHECK(unset_tui.wait(std::chrono::seconds(3)));

  auto [failed_tui, failed] = launch_editor("nonzero", "DRAFT_NONZERO");
  CHECK(failed.find("external editor failed; draft kept") != std::string::npos);
  CHECK(failed.find("DRAFT_NONZERO") != std::string::npos);
  keys(failed_tui.master, "\x1b"); keys(failed_tui.master, "q");
  CHECK(failed_tui.wait(std::chrono::seconds(3)));

  auto [cancel_tui, cancelled] = launch_editor("cancel", "DRAFT_CANCEL");
  CHECK(cancelled.find("external editor cancelled; draft kept") != std::string::npos);
  CHECK(cancelled.find("DRAFT_CANCEL") != std::string::npos);
  keys(cancel_tui.master, "\x1b"); keys(cancel_tui.master, "q");
  CHECK(cancel_tui.wait(std::chrono::seconds(3)));

  auto [invalid_tui, invalid] = launch_editor("invalid", "");
  CHECK(invalid.find("invalid JSON retained") != std::string::npos);
  CHECK(invalid.find("invalid JSON") != std::string::npos);
  keys(invalid_tui.master, "\x1b"); keys(invalid_tui.master, "q");
  CHECK(invalid_tui.wait(std::chrono::seconds(3)));

  // A second client changes the source after the editor has loaded its base.
  REQUIRE(cli(daemon.socket(), {"config", "edit", profile_id, "log", "--text",
      "{\"log\":{\"loglevel\":\"info\"}}\n"}).find("saved") != std::string::npos);
  const auto marker = (temp.path / "editor-conflict.txt").string();
  auto conflict_tui = start_tui(daemon.socket(), profile_id,
      {std::string("VISUAL=") + XRM_TEST_FAKE_EDITOR + " --mode output --marker " + marker, "EDITOR="});
  REQUIRE(conflict_tui.pid > 0);
  wait_screen_text(conflict_tui.master, [](const std::string& text) {
    return text.find("Overview") != std::string::npos;
  });
  keys(conflict_tui.master, "3\r");
  REQUIRE(wait_screen_text(conflict_tui.master, [](const std::string& text) {
    return text.find("Native module editor") != std::string::npos;
  }).find("Native module editor") != std::string::npos);
  keys(conflict_tui.master, "v");
  const auto conflict_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < conflict_deadline)
    ::poll(nullptr, 0, 10);
  REQUIRE(std::filesystem::exists(marker));
  keys(conflict_tui.master, "\x1b[C");
  (void)wait_screen_text(conflict_tui.master, [](const std::string& text) {
    return text.find("[unsaved]") != std::string::npos;
  });
  REQUIRE(cli(daemon.socket(), {"config", "edit", profile_id, "log", "--text",
      "{\"log\":{\"loglevel\":\"error\"}}\n"}).find("saved") != std::string::npos);
  keys(conflict_tui.master, "\x13");
  keys(conflict_tui.master, "\x1b[C");
  const auto conflict = wait_screen_text(conflict_tui.master, [](const std::string& text) {
    return text.find("文件在读取后已被外部修改") != std::string::npos;
  });
  CHECK(conflict.find("文件在读取后已被外部修改") != std::string::npos);
  CHECK(conflict.find("warning") != std::string::npos);
  CHECK(module_text("error"));
  keys(conflict_tui.master, "\x1b"); keys(conflict_tui.master, "q");
  CHECK(conflict_tui.wait(std::chrono::seconds(3)));
}

TEST_CASE("T25 pause keeps a bounded log viewport and reports server buffer truncation", "[T25][I52]") {
  TempDir temp;
  auto store = Store::open(temp.path / "state.sqlite"); REQUIRE(store);
  Node node; node.id = "truncate-node"; node.original_name = "truncate logs";
  node.settings = RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
  REQUIRE((*store)->put_node(node));
  store->reset();
  Daemon daemon(temp.path); REQUIRE(daemon.start());
  auto core = Json::parse(cli(daemon.socket(), {"core", "register", XRM_TEST_FAKE_CORE, "--name", "truncation fixture"}));
  auto profile = Json::parse(cli(daemon.socket(), {"profile", "create", "--name", "truncation", "--core", core.at("id").get<std::string>()}));
  const auto profile_id = profile.at("id").get<std::string>();
  set_profile_process_environment(daemon.socket(), temp.path, profile_id,
      Json{{"XRM_FAKE_CORE_STDOUT_BYTES", "5000000"}});
  REQUIRE(!cli(daemon.socket(), {"select", "node", profile_id, "truncate-node"}).empty());
  auto tui = start_tui(daemon.socket(), profile_id); REQUIRE(tui.pid > 0);
  read_screen(tui.master, std::chrono::milliseconds(700));
  keys(tui.master, "6");
  CHECK(terminal_text(read_screen(tui.master)).find("Logs — FOLLOW") != std::string::npos);
  keys(tui.master, "p");
  CHECK(terminal_text(read_screen(tui.master)).find("PAUSED") != std::string::npos);
  const auto started = cli(daemon.socket(), {"connect", profile_id});
  CHECK(!started.empty());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::string observed;
  while (std::chrono::steady_clock::now() < deadline && observed.find("older logs dropped / truncated") == std::string::npos) {
    const auto chunk = terminal_text(read_screen(tui.master, std::chrono::milliseconds(200)));
    observed += chunk;
  }
  CHECK(observed.find("older logs dropped / truncated") != std::string::npos);
  CHECK(observed.find("PAUSED") != std::string::npos);
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
  const auto stop = cli(daemon.socket(), {"disconnect"});
  CHECK(!stop.empty());
}

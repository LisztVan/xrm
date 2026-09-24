#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/common.hpp"

#include <pty.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <spawn.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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
  termios attrs{};
  if (::tcgetattr(slave, &attrs) == 0) { attrs.c_iflag &= static_cast<tcflag_t>(~IXON); (void)::tcsetattr(slave, TCSANOW, &attrs); }
  std::vector<std::string> args{XRM_TEST_MANAGER, "--socket", socket.string(), "tui", "--profile", profile};
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
std::optional<unsigned> log_view_top(std::string_view screen) {
  constexpr std::string_view prefix = "Viewport top: XRM_FAKE_CORE_LOG_";
  const auto at = screen.find(prefix);
  if (at == std::string_view::npos) return std::nullopt;
  const auto digits = screen.substr(at + prefix.size(), 4);
  try { return static_cast<unsigned>(std::stoul(std::string(digits))); }
  catch (...) { return std::nullopt; }
}
} // namespace

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
  CHECK(initial.find("? help") != std::string::npos);
  keys(tui.master, "?");
  const auto help = read_screen(tui.master);
  INFO("help PTY bytes=" << help.size() << " screen=" << help);
  CHECK(help.find("Help") != std::string::npos);
  CHECK(help.find("Tab") != std::string::npos);
  keys(tui.master, "\x1b");
  keys(tui.master, "\t");
  const auto tabbed = read_screen(tui.master);
  CHECK(tabbed.find("Nodes") != std::string::npos);
  keys(tui.master, "\x1b[Z");
  const auto shifted = read_screen(tui.master);
  INFO("shift-tab PTY bytes=" << shifted.size() << " screen=" << shifted);
  CHECK(shifted.find("Overview") != std::string::npos);
  keys(tui.master, "2");
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
  CHECK(refreshed.find("> 🚀-node") != std::string::npos);
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
  keys(tui.master, "\t\t\t");
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
  auto resumed = terminal_text(read_screen(tui.master));
  CHECK(resumed.find("FOLLOW") != std::string::npos);
  CHECK(log_view_top(resumed).value_or(0) >= *paused_top);
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
  CHECK(overview.find("Saved target: node:target-b") != std::string::npos);
  CHECK(overview.find("Applied target: target-a") != std::string::npos);
  keys(tui.master, "q");
  CHECK(tui.wait(std::chrono::seconds(3)));
  const auto status = Json::parse(cli(daemon.socket(), {"status"}));
  CHECK(status.value("state", "") == "running");
  CHECK(status.at("active").at("selection").value("id_or_tag", "") == "target-a");
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
    read_screen(tui.master, std::chrono::milliseconds(400));
    keys(tui.master, "3\r");
    const auto opened = terminal_text(read_screen(tui.master));
    REQUIRE(opened.find("Native module editor") != std::string::npos);
    if (!suffix.empty()) keys(tui.master, suffix);
    keys(tui.master, "v");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < until)
      ::poll(nullptr, 0, 10);
    REQUIRE(std::filesystem::exists(marker));
    keys(tui.master, "\x1b[C");
    auto result = terminal_text(read_screen(tui.master, std::chrono::milliseconds(300)));
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
  CHECK(saved.find("not applied to running core") != std::string::npos);
  CHECK(module_text("warning"));
  CHECK(Json::parse(cli(daemon.socket(), {"status"})).value("state", "") == "stopped");
  keys(normal_tui.master, "\x1b"); keys(normal_tui.master, "q");
  CHECK(normal_tui.wait(std::chrono::seconds(3)));

  // VISUAL takes precedence; with it unset, EDITOR is used directly via argv.
  const auto fallback_marker = (temp.path / "editor-fallback.txt").string();
  auto fallback_tui = start_tui(daemon.socket(), profile_id,
      {"VISUAL=", std::string("EDITOR=") + XRM_TEST_FAKE_EDITOR + " --mode success --marker " + fallback_marker});
  REQUIRE(fallback_tui.pid > 0);
  read_screen(fallback_tui.master, std::chrono::milliseconds(400));
  keys(fallback_tui.master, "3\r");
  REQUIRE(terminal_text(read_screen(fallback_tui.master)).find("Native module editor") != std::string::npos);
  keys(fallback_tui.master, "v");
  const auto fallback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!std::filesystem::exists(fallback_marker) && std::chrono::steady_clock::now() < fallback_deadline)
    ::poll(nullptr, 0, 10);
  CHECK(std::filesystem::exists(fallback_marker));
  keys(fallback_tui.master, "\x1b"); keys(fallback_tui.master, "q");
  CHECK(fallback_tui.wait(std::chrono::seconds(3)));

  auto unset_tui = start_tui(daemon.socket(), profile_id, {"VISUAL=", "EDITOR="});
  REQUIRE(unset_tui.pid > 0);
  read_screen(unset_tui.master, std::chrono::milliseconds(400));
  keys(unset_tui.master, "3\r");
  REQUIRE(terminal_text(read_screen(unset_tui.master)).find("Native module editor") != std::string::npos);
  keys(unset_tui.master, "v"); keys(unset_tui.master, "\x1b[C");
  const auto no_editor = terminal_text(read_screen(unset_tui.master));
  CHECK(no_editor.find("set VISUAL or EDITOR") != std::string::npos);
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
  read_screen(conflict_tui.master, std::chrono::milliseconds(400));
  keys(conflict_tui.master, "3\r");
  REQUIRE(terminal_text(read_screen(conflict_tui.master)).find("Native module editor") != std::string::npos);
  keys(conflict_tui.master, "v");
  const auto conflict_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < conflict_deadline)
    ::poll(nullptr, 0, 10);
  REQUIRE(std::filesystem::exists(marker));
  keys(conflict_tui.master, "\x1b[C");
  (void)terminal_text(read_screen(conflict_tui.master));
  REQUIRE(cli(daemon.socket(), {"config", "edit", profile_id, "log", "--text",
      "{\"log\":{\"loglevel\":\"error\"}}\n"}).find("saved") != std::string::npos);
  keys(conflict_tui.master, "\x13");
  keys(conflict_tui.master, "\x1b[C");
  const auto conflict = terminal_text(read_screen(conflict_tui.master));
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
  keys(tui.master, "\t\t\t");
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

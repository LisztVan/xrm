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
  bool start() {
    std::vector<std::string> args{XRM_TEST_SERVER, "--foreground", "--socket", socket_.string(), "--database", db_.string()};
    std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
    if (::posix_spawn(&pid_, XRM_TEST_SERVER, nullptr, nullptr, argv.data(), environ) != 0) return false;
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
  std::filesystem::path socket_, db_;
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

Child start_tui(const std::filesystem::path& socket, const std::string& profile) {
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
  const int rc = ::posix_spawn(&child.pid, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), environ);
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

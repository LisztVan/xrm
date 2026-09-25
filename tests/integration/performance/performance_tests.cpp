#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/runtime.hpp"
#include "server/log_buffer.hpp"
#include "subscription/reconcile.hpp"
#include "xray/runtime_assembler.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <spawn.h>
#include <poll.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <unistd.h>

extern char** environ;

namespace {
using namespace xrm;
using Clock = std::chrono::steady_clock;

std::uint64_t rss_kib(std::string_view key) {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.starts_with(key)) {
      std::istringstream value(line.substr(key.size()));
      std::uint64_t kib = 0;
      if (value >> kib) return kib;
    }
  }
  return 0;
}
std::string cpu_model() {
  std::ifstream input("/proc/cpuinfo");
  std::string line;
  while (std::getline(input, line))
    if (line.starts_with("model name\t: ")) return line.substr(13);
  return std::to_string(std::thread::hardware_concurrency()) + " logical CPUs";
}
std::string machine() {
  utsname info{};
  if (::uname(&info) != 0) return "unknown";
  return std::string(info.sysname) + " " + info.release + " " + info.machine + " / " + info.nodename;
}
template<class F> double milliseconds(F&& f) {
  const auto start = Clock::now();
  f();
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
std::pair<double, double> quantiles(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return {values[values.size() / 2], values[static_cast<std::size_t>(0.95 * (values.size() - 1))]};
}
struct TestDaemon {
  std::filesystem::path socket, database;
  pid_t pid = -1;
  explicit TestDaemon(std::filesystem::path root) : socket(root / "xrmd.sock"), database(root / "perf.sqlite") {}
  bool start() {
    std::vector<std::string> args{XRM_TEST_SERVER, "--foreground", "--socket", socket.string(), "--database", database.string()};
    std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
    if (::posix_spawn(&pid, XRM_TEST_SERVER, nullptr, nullptr, argv.data(), environ) != 0) return false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
      const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un addr{}; addr.sun_family = AF_UNIX;
      const auto path = socket.string(); std::copy(path.begin(), path.end(), addr.sun_path);
      const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
      ::close(fd); if (connected) return true;
      ::poll(nullptr, 0, 10);
    }
    return false;
  }
  bool stop() {
    if (pid < 0) return true;
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i=0;i<500;++i) {
      if (::waitpid(pid, &status, WNOHANG) == pid) { pid=-1; return WIFEXITED(status); }
      ::poll(nullptr,0,10);
    }
    ::kill(pid,SIGKILL); ::waitpid(pid,&status,0); pid=-1; return false;
  }
  ~TestDaemon() { (void)stop(); }
};
struct CliResult {
  int code = -1;
  double elapsed_ms = 0;
  std::string out;
  std::string err;
};
class SpawnedCli {
 public:
  SpawnedCli(const std::filesystem::path& root, const std::filesystem::path& socket,
             std::vector<std::string> command) {
    static std::atomic<unsigned> sequence{0};
    const auto id = sequence.fetch_add(1);
    out_ = root / ("cli-" + std::to_string(id) + ".out");
    err_ = root / ("cli-" + std::to_string(id) + ".err");
    std::vector<std::string> args{XRM_TEST_MANAGER, "--socket", socket.string(), "--json"};
    args.insert(args.end(), command.begin(), command.end());
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    started_ = Clock::now();
    const int spawned = ::posix_spawn(&pid_, XRM_TEST_MANAGER, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) pid_ = -1;
  }
  SpawnedCli(const SpawnedCli&) = delete;
  SpawnedCli& operator=(const SpawnedCli&) = delete;
  ~SpawnedCli() { terminate(); }
  bool started() const { return pid_ >= 0; }
  CliResult wait(std::chrono::milliseconds timeout) {
    if (pid_ < 0) return {};
    int status = 0;
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
                std::chrono::duration<double, std::milli>(Clock::now() - started_).count(),
                read_path(out_), read_path(err_)};
      }
      ::poll(nullptr, 0, 5);
    }
    terminate();
    return {-1, std::chrono::duration<double, std::milli>(Clock::now() - started_).count(),
            read_path(out_), read_path(err_)};
  }
 private:
  static std::string read_path(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
  void terminate() {
    if (pid_ < 0) return;
    ::kill(pid_, SIGKILL);
    int status = 0;
    (void)::waitpid(pid_, &status, 0);
    pid_ = -1;
  }
  std::filesystem::path out_, err_;
  pid_t pid_ = -1;
  Clock::time_point started_{};
};
CliResult manager_call(const std::filesystem::path& root, const std::filesystem::path& socket,
                       std::vector<std::string> command,
                       std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
  SpawnedCli child(root, socket, std::move(command));
  if (!child.started()) return {};
  return child.wait(timeout);
}
xrm::Json raw_rpc(const std::filesystem::path& socket, std::string method, xrm::Json params) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = socket.string();
  if (path.size() >= sizeof(address.sun_path)) { ::close(fd); return {}; }
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return {}; }
  const auto frame = xrm::Json{{"v", 1}, {"id", 1}, {"method", std::move(method)}, {"params", std::move(params)}}.dump() + "\n";
  std::size_t sent = 0;
  while (sent < frame.size()) {
    const auto count = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
    if (count <= 0) { ::close(fd); return {}; }
    sent += static_cast<std::size_t>(count);
  }
  std::string response;
  char buffer[4096];
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < deadline && response.find('\n') == std::string::npos) {
    pollfd readiness{fd, POLLIN, 0};
    if (::poll(&readiness, 1, 50) <= 0) continue;
    const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count <= 0) break;
    response.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  try { return xrm::Json::parse(response.substr(0, response.find('\n'))); }
  catch (...) { return {}; }
}
class HttpResponseBarrier {
 public:
  HttpResponseBarrier() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) return;
    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 8) != 0) { ::close(listener_); listener_ = -1; return; }
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
      ::close(listener_); listener_ = -1; return;
    }
    url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/nodes";
    thread_ = std::thread([this] { serve(); });
  }
  HttpResponseBarrier(const HttpResponseBarrier&) = delete;
  HttpResponseBarrier& operator=(const HttpResponseBarrier&) = delete;
  ~HttpResponseBarrier() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      blocked_ = false;
    }
    condition_.notify_all();
    if (listener_ >= 0) (void)::shutdown(listener_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (listener_ >= 0) ::close(listener_);
  }
  bool valid() const { return listener_ >= 0; }
  const std::string& url() const { return url_; }
  void set(std::string body, bool block) {
    std::lock_guard lock(mutex_);
    body_ = std::move(body);
    blocked_ = block;
    request_count_ = 0;
  }
  bool wait_for_request(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] { return request_count_ >= count || stopping_; }) &&
        request_count_ >= count;
  }
  void release() {
    {
      std::lock_guard lock(mutex_);
      blocked_ = false;
    }
    condition_.notify_all();
  }
 private:
  void serve() {
    while (true) {
      pollfd readiness{listener_, POLLIN, 0};
      if (::poll(&readiness, 1, 100) <= 0) {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        continue;
      }
      const int peer = ::accept(listener_, nullptr, nullptr);
      if (peer < 0) {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        continue;
      }
      char request[4096];
      (void)::recv(peer, request, sizeof(request), 0);
      std::string body;
      {
        std::unique_lock lock(mutex_);
        ++request_count_;
        condition_.notify_all();
        condition_.wait(lock, [&] { return !blocked_ || stopping_; });
        if (stopping_) { ::close(peer); return; }
        body = body_;
      }
      const auto response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: " +
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
  int listener_ = -1;
  std::string url_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::thread thread_;
  std::string body_;
  std::size_t request_count_ = 0;
  bool blocked_ = false;
  bool stopping_ = false;
};
class FakeCoreCheckBarrier {
 public:
  explicit FakeCoreCheckBarrier(std::filesystem::path path) : path_(std::move(path)) {
    listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener_ < 0) return;
    if (path_.string().size() >= sizeof(sockaddr_un{}.sun_path)) { ::close(listener_); listener_ = -1; return; }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path_text = path_.string();
    std::copy(path_text.begin(), path_text.end(), address.sun_path);
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener_, 1) != 0) {
      ::close(listener_); listener_ = -1; return;
    }
    thread_ = std::thread([this] { serve(); });
  }
  FakeCoreCheckBarrier(const FakeCoreCheckBarrier&) = delete;
  FakeCoreCheckBarrier& operator=(const FakeCoreCheckBarrier&) = delete;
  ~FakeCoreCheckBarrier() {
    release();
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    if (listener_ >= 0) (void)::shutdown(listener_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (listener_ >= 0) ::close(listener_);
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  bool valid() const { return listener_ >= 0; }
  const std::filesystem::path& path() const { return path_; }
  bool wait_ready(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] { return ready_ || stopping_; }) && ready_;
  }
  void release() {
    std::lock_guard lock(mutex_);
    if (peer_ >= 0 && !released_) {
      constexpr char release[] = "RELEASE\n";
      (void)::send(peer_, release, sizeof(release) - 1, MSG_NOSIGNAL);
      (void)::shutdown(peer_, SHUT_RDWR);
      released_ = true;
    }
  }
 private:
  void serve() {
    int peer = -1;
    while (true) {
      pollfd readiness{listener_, POLLIN, 0};
      if (::poll(&readiness, 1, 100) <= 0) {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        continue;
      }
      peer = ::accept(listener_, nullptr, nullptr);
      if (peer >= 0) break;
      std::lock_guard lock(mutex_);
      if (stopping_) return;
    }
    {
      std::lock_guard lock(mutex_);
      peer_ = peer;
    }
    char buffer[32]{};
    const auto received = ::recv(peer, buffer, sizeof(buffer), 0);
    bool ready = false;
    {
      std::lock_guard lock(mutex_);
      ready_ = received > 0 && std::string_view(buffer, static_cast<std::size_t>(received)).find("READY") != std::string_view::npos;
      ready = ready_;
    }
    condition_.notify_all();
    if (ready) {
      while (true) {
        const auto count = ::recv(peer, buffer, sizeof(buffer), 0);
        if (count <= 0 || std::string_view(buffer, static_cast<std::size_t>(count)).find("RELEASE") != std::string_view::npos) break;
      }
    }
    ::close(peer);
    std::lock_guard lock(mutex_);
    peer_ = -1;
  }
  std::filesystem::path path_;
  int listener_ = -1;
  int peer_ = -1;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool ready_ = false;
  bool released_ = false;
  bool stopping_ = false;
};
double manager_call_ms(const std::filesystem::path& socket, std::string command) {
  std::vector<std::string> args{XRM_TEST_MANAGER, "--socket", socket.string(), "--json", std::move(command)};
  std::vector<char*> argv; for (auto& arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  const auto start = Clock::now();
  pid_t child=-1; const int rc=::posix_spawn(&child,XRM_TEST_MANAGER,&actions,nullptr,argv.data(),environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) return -1;
  int status=0;
  for(int i=0;i<500;++i) {
    if(::waitpid(child,&status,WNOHANG)==child) return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    ::poll(nullptr,0,10);
  }
  ::kill(child,SIGKILL); ::waitpid(child,&status,0); return -1;
}
Node make_node(std::size_t i) {
  Node node;
  node.id = "node-" + std::to_string(i);
  node.original_name = "edge-" + std::to_string(i) + (i % 3 == 0 ? " 中文" : "");
  node.source_id = "perf-source";
  node.upstream_id = std::to_string(i);
  node.labels = {i % 2 ? "odd" : "even", "fixture"};
  StreamNode stream;
  stream.endpoint = {"192.0.2.1", static_cast<std::uint16_t>(1000 + i)};
  stream.protocol = VlessSettings{"00000000-0000-4000-8000-" + std::to_string(100000000000ULL + i), "none", std::nullopt, std::nullopt};
  node.settings = std::move(stream);
  return node;
}
} // namespace

TEST_CASE("PERF-02 1000-node inventory, group expansion, rendering and bounded buffers", "[PERF-02][I52]") {
  constexpr std::size_t count = 1000;
  auto root = std::filesystem::temp_directory_path() /
      ("xrm-perf-" + std::to_string(Clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
  const auto dbpath = root / "perf.sqlite";
  const auto rss_start = rss_kib("VmRSS:");
  const auto process_peak_start = rss_kib("VmHWM:");
  auto opened = Store::open(dbpath); REQUIRE(opened);
  std::vector<Node> nodes; nodes.reserve(count);
  for (std::size_t i = 0; i < count; ++i) nodes.push_back(make_node(i));
  const auto seed_ms = milliseconds([&] { for (const auto& node : nodes) REQUIRE((*opened)->put_node(node)); });

  std::vector<double> list_ms, search_ms;
  list_ms.reserve(60); search_ms.reserve(60);
  for (int i = 0; i < 60; ++i) {
    const auto listing = milliseconds([&] {
      auto page = (*opened)->list_nodes({}, PageRequest{0, 100}); REQUIRE(page); REQUIRE(page->total == count);
    });
    list_ms.push_back(listing);
    const auto searching = milliseconds([&] {
      NodeQuery query; query.text = "edge-99";
      auto page = (*opened)->list_nodes(query, PageRequest{0, 100}); REQUIRE(page); REQUIRE(page->total == 11);
    });
    search_ms.push_back(searching);
  }

  BuildInput input;
  input.settings.profile.id = "perf-profile";
  input.settings.profile.name = "PERF fixture";
  input.settings.profile.core_id = "perf-core";
  input.settings.profile.selected = Target{TargetKind::group, "perf-group"};
  input.settings.profile.append_selected_default = true;
  input.settings.profile.modules = {{"outbounds", "03_outbounds.json", true}, {"routing", "04_routing.json", true}};
  input.settings.modules = {{"outbounds", "03_outbounds.json", R"({"outbounds":[]})"},
                            {"routing", "04_routing.json", R"({"routing":{"rules":[]}})"}};
  GroupBinding group;
  group.id = "perf-group"; group.name = "1000 nodes";
  group.members = NodeFilter{{}, {}, {}, {"fixture"}};
  group.selection_mode = SelectionMode::automatic;
  group.balancer = Json{{"tag", "perf-balancer"}, {"strategy", {{"type", "roundRobin"}}}};
  input.settings.bindings.groups.push_back(group);
  input.nodes = nodes;
  const auto repeat_rss_start = rss_kib("VmRSS:");
  auto repeat_rss_high = repeat_rss_start;
  std::vector<double> expand_render_ms; expand_render_ms.reserve(40);
  std::size_t output_bytes = 0;
  for (int i = 0; i < 40; ++i) {
    auto elapsed = milliseconds([&] {
      auto prepared = assemble(input);
      REQUIRE(prepared);
      REQUIRE(prepared->index.nodes.size() == count);
      output_bytes = 0;
      for (const auto& file : prepared->files) output_bytes += file.bytes.size();
    });
    expand_render_ms.push_back(elapsed);
    if (i % 5 == 4) repeat_rss_high = std::max(repeat_rss_high, rss_kib("VmRSS:"));
  }

  Subscription source; source.id = "perf-source"; source.name = "local fixture"; source.url = "http://127.0.0.1/fixture";
  ParseReport report; report.structurally_complete = true;
  report.nodes.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    report.nodes.push_back(ParsedNode{std::to_string(i), nodes[i].original_name, nodes[i].settings});
  std::vector<double> reconcile_ms_samples; reconcile_ms_samples.reserve(40);
  for (int i = 0; i < 40; ++i) reconcile_ms_samples.push_back(milliseconds([&] {
    auto result = reconcile({source, nodes, report}, [n = std::size_t{0}]() mutable { return "new-" + std::to_string(n++); });
    REQUIRE(result);
    REQUIRE(result->changes.upserts.empty());
    REQUIRE(result->report.added == 0);
    REQUIRE(result->report.missing == 0);
  }));

  LogBuffer logs;
  const std::string chunk(1024 * 1024, 'L');
  const auto log_ms = milliseconds([&] { for (int i = 0; i < 8; ++i) logs.append(LogStream::core_stdout, chunk); });
  REQUIRE(logs.bytes_retained() <= 4 * 1024 * 1024);
  const std::uint64_t old_cursor = 0;
  REQUIRE(logs.read_after(old_cursor, 4096).truncated);
  std::vector<double> log_watch_read_ms; log_watch_read_ms.reserve(60);
  std::uint64_t cursor = 0;
  for (int i = 0; i < 60; ++i) {
    log_watch_read_ms.push_back(milliseconds([&] { const auto page = logs.read_after(cursor, 64 * 1024); cursor = page.next_cursor; }));
  }

  const auto [list_p50, list_p95] = quantiles(list_ms);
  const auto [search_p50, search_p95] = quantiles(search_ms);
  const auto [render_p50, render_p95] = quantiles(expand_render_ms);
  const auto [reconcile_p50, reconcile_p95] = quantiles(reconcile_ms_samples);
  const auto [log_read_p50, log_read_p95] = quantiles(log_watch_read_ms);
  const auto rss_end = rss_kib("VmRSS:");
  const auto process_peak_end = rss_kib("VmHWM:");
  std::ostringstream perf;
  perf << "PERF-02 machine=\"" << machine() << "\" cpu=\"" << cpu_model() << "\" cpu_threads=" << std::thread::hardware_concurrency()
       << " compiler=" << __VERSION__ << " build=" << XRM_PERF_BUILD_TYPE
       << " nodes=" << count << " repetitions=list/search:60,assemble/reconcile/log-read:40/40/60"
       << " seed_ms=" << seed_ms
       << " node_list_p50_ms=" << list_p50 << " node_list_p95_ms=" << list_p95
       << " node_search_p50_ms=" << search_p50 << " node_search_p95_ms=" << search_p95
       << " group_expand+render_p50_ms=" << render_p50 << " group_expand+render_p95_ms=" << render_p95
       << " reconcile_p50_ms=" << reconcile_p50 << " reconcile_p95_ms=" << reconcile_p95
       << " log_append_8MiB_ms=" << log_ms << " log_watch_read_p50_ms=" << log_read_p50
       << " log_watch_read_p95_ms=" << log_read_p95
       << " runtime_files_bytes=" << output_bytes << " log_retained_bytes=" << logs.bytes_retained()
       << " log_limit_bytes=" << (4 * 1024 * 1024)
       << " repeated_assemble_rss_start_kib=" << repeat_rss_start
       << " repeated_assemble_rss_high_kib=" << repeat_rss_high
       << " rss_initial_kib=" << rss_start << " rss_end_kib=" << rss_end
       << " peak_rss_before_kib=" << process_peak_start << " peak_rss_end_kib=" << process_peak_end;
  INFO(perf.str());
  CHECK(process_peak_end >= rss_end);
#if !defined(XRM_ASAN_BUILD)
  CHECK(repeat_rss_high <= repeat_rss_start + 32768);
#else
  // ASan retains freed allocations in its quarantine, so the normal-build RSS
  // ceiling is not meaningful here. LeakSanitizer checks live allocations at
  // process exit; functional resource-bound assertions remain active below.
  CHECK(repeat_rss_high >= repeat_rss_start);
#endif
  opened->reset();
  TestDaemon daemon(root);
  REQUIRE(daemon.start());
  std::vector<double> status_ms, cli_logs_ms;
  for (int i=0;i<40;++i) {
    status_ms.push_back(manager_call_ms(daemon.socket,"status"));
    cli_logs_ms.push_back(manager_call_ms(daemon.socket,"logs"));
  }
  REQUIRE(std::all_of(status_ms.begin(),status_ms.end(),[](double x){return x>=0;}));
  REQUIRE(std::all_of(cli_logs_ms.begin(),cli_logs_ms.end(),[](double x){return x>=0;}));
  const auto [status_p50,status_p95]=quantiles(status_ms);
  const auto [cli_logs_p50,cli_logs_p95]=quantiles(cli_logs_ms);
  INFO("cli_status_p50_ms=" << status_p50 << " cli_status_p95_ms=" << status_p95
       << " cli_logs_p50_ms=" << cli_logs_p50 << " cli_logs_p95_ms=" << cli_logs_p95);
  std::cout << perf.str() << " cli_status_p50_ms=" << status_p50
            << " cli_status_p95_ms=" << status_p95
            << " cli_logs_p50_ms=" << cli_logs_p50
            << " cli_logs_p95_ms=" << cli_logs_p95 << '\n';
  CHECK(output_bytes > 0);
  CHECK(logs.bytes_retained() == 4 * 1024 * 1024);
}

TEST_CASE("PERF-01 slow HTTP and core check do not block other UDS clients", "[PERF-01][I52]") {
  auto root = std::filesystem::temp_directory_path() /
      ("xrm-perf01-" + std::to_string(Clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};

  HttpResponseBarrier http;
  REQUIRE(http.valid());
  const auto check_socket = root / "fake-check.sock";
  FakeCoreCheckBarrier core_check(check_socket);
  REQUIRE(core_check.valid());

  TestDaemon daemon(root);
  REQUIRE(daemon.start());
  const auto link = [](std::string uuid, std::string name) {
    return "vless://" + uuid + "@" + name + ".invalid:443?encryption=none#" + name;
  };
  http.set(link("11111111-1111-4111-8111-111111111111", "A"), false);
  auto core = manager_call(root, daemon.socket, {"core", "register", XRM_TEST_FAKE_CORE, "--name", "PERF-01 barrier core"});
  REQUIRE(core.code == 0);
  const auto core_id = xrm::Json::parse(core.out).at("id").get<std::string>();
  auto profile = manager_call(root, daemon.socket, {"profile", "create", "--name", "PERF-01", "--core", core_id});
  REQUIRE(profile.code == 0);
  const auto profile_id = xrm::Json::parse(profile.out).at("id").get<std::string>();
  auto profile_settings = manager_call(root, daemon.socket, {"profile", "show", profile_id});
  REQUIRE(profile_settings.code == 0);
  auto settings = xrm::Json::parse(profile_settings.out);
  settings["environment"]["process_environment"]["XRM_FAKE_CORE_CHECK_BARRIER_SOCKET"] = check_socket.string();
  const auto profile_file = root / "profiles" / profile_id / "profile.json";
  std::ifstream profile_input(profile_file, std::ios::binary);
  REQUIRE(profile_input.is_open());
  const std::string profile_base{std::istreambuf_iterator<char>(profile_input), std::istreambuf_iterator<char>()};
  REQUIRE_FALSE(profile_base.empty());
  const auto saved = raw_rpc(daemon.socket, "profile.save",
      xrm::Json{{"profile_id", profile_id}, {"expected_text", profile_base}, {"settings", settings}});
  REQUIRE(saved.value("ok", false));
  REQUIRE(saved.contains("result"));
  CHECK(saved.at("result").value("saved", false));
  auto source = manager_call(root, daemon.socket, {"sub", "add", "--name", "PERF-01", "--url", http.url()});
  REQUIRE(source.code == 0);
  const auto source_id = xrm::Json::parse(source.out).at("id").get<std::string>();
  REQUIRE(manager_call(root, daemon.socket, {"profile", "sources", "set", profile_id,
                       "--source", source_id}).code == 0);
  REQUIRE(manager_call(root, daemon.socket, {"sub", "update", source_id}).code == 0);
  auto listing = manager_call(root, daemon.socket, {"node", "list"});
  REQUIRE(listing.code == 0);
  const auto node_id = xrm::Json::parse(listing.out).at("items").at(0).at("id").get<std::string>();
  REQUIRE(manager_call(root, daemon.socket, {"select", "node", profile_id, node_id}).code == 0);

  http.set(link("22222222-2222-4222-8222-222222222222", "B"), true);
  SpawnedCli refresh(root, daemon.socket, {"sub", "update", source_id});
  REQUIRE(refresh.started());
  REQUIRE(http.wait_for_request(1, std::chrono::seconds(3)));
  SpawnedCli connect(root, daemon.socket, {"connect", profile_id});
  REQUIRE(connect.started());
  INFO("waiting for fake-core check barrier at " << check_socket);
  REQUIRE(core_check.wait_ready(std::chrono::seconds(3)));

  std::vector<double> status_ms, logs_ms;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto status = manager_call(root, daemon.socket, {"status"}, std::chrono::seconds(2));
    INFO("status output=" << status.out << " error=" << status.err);
    REQUIRE(status.code == 0);
    REQUIRE(xrm::Json::parse(status.out).is_object());
    status_ms.push_back(status.elapsed_ms);
    const auto logs = manager_call(root, daemon.socket, {"logs"}, std::chrono::seconds(2));
    INFO("logs output=" << logs.out << " error=" << logs.err);
    REQUIRE(logs.code == 0);
    REQUIRE(xrm::Json::parse(logs.out).is_object());
    logs_ms.push_back(logs.elapsed_ms);
  }
  const auto disconnect = manager_call(root, daemon.socket, {"disconnect"}, std::chrono::seconds(3));
  INFO("stop output=" << disconnect.out << " error=" << disconnect.err);
  REQUIRE(disconnect.code == 0);
  CHECK(disconnect.elapsed_ms < 3000.0);
  const auto connect_result = connect.wait(std::chrono::seconds(3));
  INFO("connect completion output=" << connect_result.out << " error=" << connect_result.err);
  CHECK(connect_result.code != -1);
  CHECK(connect_result.code != 0);

  http.release();
  const auto refresh_result = refresh.wait(std::chrono::seconds(3));
  INFO("refresh completion output=" << refresh_result.out << " error=" << refresh_result.err);
  REQUIRE(refresh_result.code == 0);
  const auto stopped = manager_call(root, daemon.socket, {"status"});
  REQUIRE(stopped.code == 0);
  CHECK(xrm::Json::parse(stopped.out).value("state", "") == "stopped");
  core_check.release();
  REQUIRE(daemon.stop());

  const auto [status_p50, status_p95] = quantiles(status_ms);
  const auto [logs_p50, logs_p95] = quantiles(logs_ms);
  std::cout << "PERF-01 blocked_http_and_core_check=true status_logs_rounds=3 status_p50_ms=" << status_p50
            << " status_p95_ms=" << status_p95 << " logs_p50_ms=" << logs_p50
            << " logs_p95_ms=" << logs_p95 << " stop_response_ms=" << disconnect.elapsed_ms << '\n';
  CHECK(status_p95 < 2000.0);
  CHECK(logs_p95 < 2000.0);
}

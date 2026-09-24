#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/runtime.hpp"
#include "server/log_buffer.hpp"
#include "subscription/reconcile.hpp"
#include "xray/runtime_assembler.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <spawn.h>
#include <poll.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
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

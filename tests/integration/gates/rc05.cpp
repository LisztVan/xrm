#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "profile/profile_service.hpp"
#include "xray/codecs/codec.hpp"
#include "xray/metrics.hpp"
#include "xray/runtime_assembler.hpp"
#include "xray/runtime_writer.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {
using namespace std::chrono_literals;
using namespace xrm;

std::filesystem::path real_xray() {
  if (const char* path = std::getenv("XRM_TEST_XRAY"); path && *path) return path;
#ifdef XRM_TEST_XRAY_CONFIGURED
  return XRM_TEST_XRAY_CONFIGURED;
#else
  return {};
#endif
}

struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-rc05-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct CommandResult { int code = -1; std::string out; std::string err; };

CommandResult run_command(const std::filesystem::path& executable,
                          const std::vector<std::string>& arguments,
                          const std::filesystem::path& root,
                          std::chrono::seconds timeout = 15s) {
  static std::atomic<unsigned> sequence{0};
  const auto number = sequence.fetch_add(1);
  const auto stdout_path = root / ("command-" + std::to_string(number) + ".out");
  const auto stderr_path = root / ("command-" + std::to_string(number) + ".err");
  std::vector<std::string> values{executable.string()};
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) return {};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::waitpid(pid, &status, WNOHANG) == pid)
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(stdout_path), read_file(stderr_path)};
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(stdout_path), read_file(stderr_path)};
}

std::optional<std::uint16_t> free_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return std::nullopt;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

bool wait_for_port(std::uint16_t port, std::chrono::seconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    if (connected) return true;
    ::poll(nullptr, 0, 20);
  }
  return false;
}

std::string get_debug_vars(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return {};
  }
  const std::string request = "GET /debug/vars HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) < 0) {
    ::close(fd);
    return {};
  }
  std::string response;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (count <= 0) break;
    response.append(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(fd);
  const auto separator = response.find("\r\n\r\n");
  if (separator == std::string::npos) return response;
  auto body = response.substr(separator + 4);
  if (response.substr(0, separator).find("Transfer-Encoding: chunked") == std::string::npos &&
      response.substr(0, separator).find("transfer-encoding: chunked") == std::string::npos)
    return body;
  std::string decoded;
  std::size_t cursor = 0;
  while (cursor < body.size()) {
    const auto line_end = body.find("\r\n", cursor);
    if (line_end == std::string::npos) return {};
    std::size_t chunk_size = 0;
    try { chunk_size = std::stoul(body.substr(cursor, line_end - cursor), nullptr, 16); }
    catch (...) { return {}; }
    cursor = line_end + 2;
    if (chunk_size == 0) return decoded;
    if (cursor + chunk_size + 2 > body.size()) return {};
    decoded.append(body, cursor, chunk_size);
    cursor += chunk_size + 2;
  }
  return {};
}

class ControlledProbe {
 public:
  bool start() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) return false;
    int enabled = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 16) != 0) return false;
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] { serve(); });
    return true;
  }

  std::uint16_t port() const { return port_; }
  std::size_t requests() const { return requests_.load(); }

  ~ControlledProbe() {
    stopping_.store(true);
    if (listener_ >= 0) {
      ::shutdown(listener_, SHUT_RDWR);
      ::close(listener_);
      listener_ = -1;
    }
    if (worker_.joinable()) worker_.join();
  }

 private:
  void serve() {
    while (!stopping_.load()) {
      pollfd descriptor{listener_, POLLIN, 0};
      if (::poll(&descriptor, 1, 100) <= 0) continue;
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) continue;
      timeval timeout{2, 0};
      ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      char request[2048]{};
      (void)::recv(client, request, sizeof(request), 0);
      constexpr std::string_view response =
          "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      std::size_t sent = 0;
      while (sent < response.size()) {
        const auto count = ::send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
      }
      requests_.fetch_add(1);
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }
  }

  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<std::size_t> requests_{0};
  std::atomic<bool> stopping_{false};
  std::thread worker_;
};

class XrayProcess {
 public:
  bool start(const std::filesystem::path& xray, const std::filesystem::path& confdir,
             const std::filesystem::path& root) {
    static std::atomic<unsigned> sequence{0};
    const auto number = sequence.fetch_add(1);
    stdout_path_ = root / ("xray-" + std::to_string(number) + ".out");
    stderr_path_ = root / ("xray-" + std::to_string(number) + ".err");
    std::vector<std::string> values{xray.string(), "run", "-confdir", confdir.string()};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    const int status = posix_spawn(&pid_, xray.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    return status == 0;
  }

  bool stop() {
    if (pid_ < 0) return true;
    ::kill(pid_, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int status = 0;
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

  std::string stderr_text() const { return read_file(stderr_path_); }
  ~XrayProcess() { (void)stop(); }

 private:
  pid_t pid_ = -1;
  std::filesystem::path stdout_path_;
  std::filesystem::path stderr_path_;
};

bool save_module(ProfileService& profiles, const Id& profile, std::string key, Json document) {
  auto previous = profiles.module(profile, key);
  if (!previous) return false;
  const auto base = previous->document
      ? std::optional<std::string>(previous->document->source_text) : std::nullopt;
  if (!profiles.save_module(profile, {key, {base}, document.dump(2) + "\n"})) return false;
  return static_cast<bool>(profiles.include_module(profile, key, true));
}

Result<PreparedDirectory> assemble_profile(ProfileService& profiles, Store& store,
                                           RuntimeWriter& writer, const Id& profile,
                                           const Id& directory_id) {
  auto snapshot = profiles.snapshot(profile);
  if (!snapshot) return std::unexpected(snapshot.error());
  auto groups = profiles.groups(profile);
  auto observation = profiles.observation(profile);
  auto nodes = store.all_nodes();
  if (!groups) return std::unexpected(groups.error());
  if (!observation) return std::unexpected(observation.error());
  if (!nodes) return std::unexpected(nodes.error());
  snapshot->bindings.groups = std::move(*groups);
  snapshot->bindings.observation = std::move(*observation);
  auto files = assemble(BuildInput{std::move(*snapshot), std::move(*nodes)});
  if (!files) return std::unexpected(files.error());
  return writer.materialize(*files, DirectoryPurpose::candidate, directory_id);
}

void describe_handle(uv_handle_t* handle, void* opaque) {
  auto* output = static_cast<std::string*>(opaque);
  *output += std::string(uv_handle_type_name(uv_handle_get_type(handle))) +
      " active=" + (uv_is_active(handle) ? "yes" : "no") +
      " closing=" + (uv_is_closing(handle) ? "yes" : "no") + "; ";
}

bool run_observation_case(const std::filesystem::path& xray, std::string_view module_key) {
  TempDirectory temp;
  ControlledProbe probe;
  REQUIRE(probe.start());
  const auto metrics_port = free_loopback_port();
  REQUIRE(metrics_port.has_value());

  auto store = Store::open(temp.path / "state.sqlite");
  REQUIRE(store);
  ProfileService profiles(**store, temp.path / "profiles");
  auto profile = profiles.create({"RC-05 controlled " + std::string(module_key), "test-core", "local", {}});
  REQUIRE(profile);

  constexpr std::string_view managed_id = "rc05-probe-node";
  auto raw_node = parse_native_outbound(Json{{"protocol", "freedom"}, {"settings", Json::object()}},
                                        std::string(managed_id), "RC-05 local freedom");
  REQUIRE(raw_node);
  REQUIRE((*store)->put_node(*raw_node));
  auto profile_settings = profiles.get(profile->id);
  REQUIRE(profile_settings);
  profile_settings->selected = Target{TargetKind::node, std::string(managed_id)};
  const auto profile_path = temp.path / "profiles" / profile->id / "profile.json";
  REQUIRE(profiles.save(profile->id, {{read_file(profile_path)}, *profile_settings}));

  REQUIRE(save_module(profiles, profile->id, "outbounds", Json{{"outbounds", Json::array()}}));
  REQUIRE(save_module(profiles, profile->id, "metrics",
      Json{{"metrics", Json{{"listen", "127.0.0.1:" + std::to_string(*metrics_port)}}}}));
  const auto probe_url = "http://127.0.0.1:" + std::to_string(probe.port()) + "/controlled-probe";
  REQUIRE(save_module(profiles, profile->id, "observatory", Json{{"observatory", Json{
      {"subjectSelector", Json::array()}, {"probeUrl", probe_url},
      {"probeInterval", "250ms"}, {"rc05NativeField", "keep-observatory"}}}}));
  REQUIRE(save_module(profiles, profile->id, "burstObservatory", Json{{"burstObservatory", Json{
      {"subjectSelector", Json::array()},
      {"pingConfig", Json{{"destination", probe_url}, {"connectivity", probe_url},
          {"interval", "250ms"}, {"sampling", 2}, {"timeout", "1s"}, {"httpMethod", "GET"},
          {"rc05NativeField", "keep-burst"}}}}}}));

  REQUIRE(profiles.save_observation(profile->id, {std::nullopt},
      ObservationBinding{false, {std::string(managed_id)}}));
  REQUIRE(profiles.choose_observer(profile->id, std::string(module_key)));
  const auto opposite = module_key == "observatory" ? "burstObservatory" : "observatory";
  const auto selected = profiles.module(profile->id, module_key);
  const auto inactive = profiles.module(profile->id, opposite);
  REQUIRE(selected);
  REQUIRE(inactive);
  REQUIRE(selected->setting.included);
  CHECK_FALSE(inactive->setting.included);
  REQUIRE(selected->document);
  CHECK_FALSE(inactive->document == std::nullopt);

  RuntimeWriter writer(temp.path / "manager-runtime");
  auto runtime = assemble_profile(profiles, **store, writer, profile->id,
      "rc05-" + std::string(module_key));
  INFO("assemble failed: " << (runtime ? "none" : runtime.error().message));
  REQUIRE(runtime);
  const auto confdir = runtime->confdir;
  const auto active_filename = module_key == "observatory" ? "05_observatory.json" : "14_burstObservatory.json";
  const auto opposite_filename = module_key == "observatory" ? "14_burstObservatory.json" : "05_observatory.json";
  REQUIRE(std::filesystem::exists(confdir / active_filename));
  CHECK_FALSE(std::filesystem::exists(confdir / opposite_filename));

  const auto active_root = Json::parse(read_file(confdir / active_filename));
  REQUIRE(active_root.contains(module_key));
  CHECK(std::find(active_root.at(module_key).at("subjectSelector").begin(),
                  active_root.at(module_key).at("subjectSelector").end(), node_tag(managed_id)) !=
        active_root.at(module_key).at("subjectSelector").end());
  if (module_key == "observatory") {
    CHECK(active_root.at("observatory").at("probeUrl") == probe_url);
    CHECK(active_root.at("observatory").at("probeInterval") == "250ms");
    CHECK(active_root.at("observatory").at("rc05NativeField") == "keep-observatory");
  } else {
    CHECK(active_root.at("burstObservatory").at("pingConfig").at("destination") == probe_url);
    CHECK(active_root.at("burstObservatory").at("pingConfig").at("interval") == "250ms");
    CHECK(active_root.at("burstObservatory").at("pingConfig").at("sampling") == 2);
    CHECK(active_root.at("burstObservatory").at("pingConfig").at("rc05NativeField") == "keep-burst");
  }
  const auto outbounds = Json::parse(read_file(confdir / "90_nodes_tail.json")).at("outbounds");
  REQUIRE(outbounds.size() == 1);
  CHECK(outbounds.front().at("tag") == node_tag(managed_id));

  auto checked = run_command(xray, {"run", "-test", "-confdir", confdir.string()}, temp.path);
  INFO("real Xray check stdout: " << checked.out << " stderr: " << checked.err);
  REQUIRE(checked.code == 0);
  XrayProcess core;
  REQUIRE(core.start(xray, confdir, temp.path));
  REQUIRE(wait_for_port(*metrics_port));
  const auto real_vars = Json::parse(get_debug_vars(*metrics_port));
  REQUIRE(real_vars.contains("observatory"));
  REQUIRE(real_vars.at("observatory").contains(node_tag(managed_id)));
  INFO("real Xray observatory expvar: " << real_vars.at("observatory").at(node_tag(managed_id)).dump());
  CHECK(real_vars.at("observatory").at(node_tag(managed_id)).at("alive") == true);
  if (module_key == "burstObservatory") {
    REQUIRE(real_vars.at("observatory").at(node_tag(managed_id)).contains("health_ping"));
    CHECK(real_vars.at("observatory").at(node_tag(managed_id)).at("health_ping").at("all").get<std::uint64_t>() > 0);
  } else {
    CHECK(real_vars.at("observatory").at(node_tag(managed_id)).contains("last_try_time"));
    CHECK(real_vars.at("observatory").at(node_tag(managed_id)).contains("last_seen_time"));
  }

  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  bool got_managed_observation = false;
  {
    HttpClient http(loop);
    {
      MetricsClient metrics(loop, http);
      RunRecord record;
      record.run_id = "rc05-" + std::string(module_key);
      record.profile_id = profile->id;
      const auto endpoint = "http://127.0.0.1:" + std::to_string(*metrics_port) + "/debug/vars";
      record.metrics_endpoint = endpoint;
      metrics.attach(record, endpoint);
      metrics.set_viewer_count(1);
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (std::chrono::steady_clock::now() < deadline) {
        uv_run(&loop, UV_RUN_NOWAIT);
        const auto view = metrics.view();
        if (view.sample && std::any_of(view.sample->observations.begin(), view.sample->observations.end(),
            [&](const NodeObservation& observation) {
              return observation.tag == node_tag(managed_id) && observation.alive.value_or(false);
            })) {
          got_managed_observation = true;
          break;
        }
        std::this_thread::sleep_for(2ms);
      }
      metrics.set_viewer_count(0);
      const auto until = std::chrono::steady_clock::now() + 150ms;
      while (std::chrono::steady_clock::now() < until) {
        uv_run(&loop, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(2ms);
      }
    }
    bool drained = false;
    http.shutdown([&] { drained = true; });
    while (!drained) uv_run(&loop, UV_RUN_DEFAULT);
  }
  std::string open_handles;
  uv_walk(&loop, describe_handle, &open_handles);
  INFO("remaining libuv handles: " << open_handles);
  uv_run(&loop, UV_RUN_DEFAULT);
  REQUIRE(uv_loop_close(&loop) == 0);
  CHECK(probe.requests() > 0);
  INFO("real controlled probe requests: " << probe.requests());
  INFO("real Xray stderr: " << core.stderr_text());
  CHECK(got_managed_observation);
  REQUIRE(core.stop());
  return true;
}

}  // namespace

TEST_CASE("RC-05 fixed Xray probes managed tags through both native observatories",
          "[real_core][RC-05]") {
  const auto xray = real_xray();
  if (xray.empty() || !std::filesystem::exists(xray))
    SKIP("set XRM_TEST_XRAY to the fixed real Xray binary");
  const auto version = run_command(xray, {"version"}, std::filesystem::temp_directory_path());
  INFO("Xray version output: " << version.out);
  REQUIRE(version.code == 0);
  REQUIRE(version.out.find("26.9.9") != std::string::npos);
  REQUIRE(version.out.find("52a412d") != std::string::npos);
  REQUIRE(run_observation_case(xray, "observatory"));
  REQUIRE(run_observation_case(xray, "burstObservatory"));
}

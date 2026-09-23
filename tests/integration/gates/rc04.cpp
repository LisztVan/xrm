#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "profile/forms.hpp"
#include "profile/profile_service.hpp"
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
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;

namespace {
using namespace std::chrono_literals;
using namespace xrm;

std::filesystem::path test_xray() {
  if (const char* path = std::getenv("XRM_TEST_XRAY"); path && *path) return path;
#ifdef XRM_TEST_XRAY_CONFIGURED
  return XRM_TEST_XRAY_CONFIGURED;
#else
  return {};
#endif
}

class TempDirectory {
 public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
        ("xrm-rc04-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
  const std::filesystem::path& path() const { return path_; }
 private:
  std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct CommandResult { int code = -1; std::string out; std::string err; };

CommandResult run_command(const std::filesystem::path& executable,
                          const std::vector<std::string>& arguments,
                          const std::filesystem::path& root,
                          std::chrono::seconds timeout = 20s) {
  static std::atomic<unsigned> sequence{0};
  const auto number = sequence.fetch_add(1);
  const auto out_path = root / ("cmd-" + std::to_string(number) + ".out");
  const auto err_path = root / ("cmd-" + std::to_string(number) + ".err");
  std::vector<std::string> values{executable.string()};
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) return {};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
              read_file(out_path), read_file(err_path)};
    }
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path)};
}

class OwnedProcess {
 public:
  bool start(const std::filesystem::path& executable,
             const std::filesystem::path& confdir,
             const std::filesystem::path& root) {
    static std::atomic<unsigned> sequence{0};
    const auto number = sequence.fetch_add(1);
    out_ = root / ("xray-" + std::to_string(number) + ".out");
    err_ = root / ("xray-" + std::to_string(number) + ".err");
    std::vector<std::string> values{executable.string(), "run", "-confdir", confdir.string()};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_.c_str(),
                                     O_CREAT | O_TRUNC | O_WRONLY, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_.c_str(),
                                     O_CREAT | O_TRUNC | O_WRONLY, 0600);
    const int status = posix_spawn(&pid_, executable.c_str(), &actions, nullptr, argv.data(), environ);
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
  ~OwnedProcess() { (void)stop(); }
 private:
  std::filesystem::path out_;
  std::filesystem::path err_;
  pid_t pid_ = -1;
};

std::optional<std::uint16_t> free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return std::nullopt;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  const auto result = ntohs(address.sin_port);
  ::close(fd);
  return result;
}

bool wait_port(std::uint16_t port) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const bool ready = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    if (ready) return true;
    ::poll(nullptr, 0, 20);
  }
  return false;
}

class HttpMarker {
 public:
  bool start() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) return false;
    int reuse = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 8) != 0) return false;
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] { serve(); });
    return true;
  }
  std::uint16_t port() const { return port_; }
  ~HttpMarker() {
    stopping_.store(true);
    if (listener_ >= 0) { ::shutdown(listener_, SHUT_RDWR); ::close(listener_); listener_ = -1; }
    if (worker_.joinable()) worker_.join();
  }
 private:
  void serve() {
    while (!stopping_.load()) {
      pollfd pfd{listener_, POLLIN, 0};
      if (::poll(&pfd, 1, 100) <= 0) continue;
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) continue;
      timeval timeout{3, 0};
      ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      char request[2048];
      (void)::recv(client, request, sizeof(request), 0);
      constexpr std::string_view marker = "XRM-RC04-TRAFFIC-9b27a1";
      const auto response = "HTTP/1.1 200 OK\r\nContent-Length: " +
          std::to_string(marker.size()) + "\r\nConnection: close\r\n\r\n" + std::string(marker);
      std::size_t sent = 0;
      while (sent < response.size()) {
        const auto amount = ::send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
        if (amount <= 0) break;
        sent += static_cast<std::size_t>(amount);
      }
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }
  }
  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
};

bool send_all(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (count <= 0) return false;
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return true;
}

bool recv_exact(int fd, char* bytes, std::size_t count) {
  std::size_t received = 0;
  while (received < count) {
    const auto amount = ::recv(fd, bytes + received, count - received, 0);
    if (amount <= 0) return false;
    received += static_cast<std::size_t>(amount);
  }
  return true;
}

std::optional<std::string> get_http_body(std::uint16_t port, std::string_view path) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return std::nullopt;
  timeval timeout{3, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  const auto request = "GET " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (!send_all(fd, request)) { ::close(fd); return std::nullopt; }
  std::string response;
  std::array<char, 8192> chunk{};
  for (;;) {
    const auto amount = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (amount <= 0) break;
    response.append(chunk.data(), static_cast<std::size_t>(amount));
  }
  ::close(fd);
  const auto separator = response.find("\r\n\r\n");
  if (separator == std::string::npos || !response.starts_with("HTTP/1.1 200")) return std::nullopt;
  const auto body = response.substr(separator + 4);
  std::string decoded;
  std::size_t cursor = 0;
  for (;;) {
    const auto line_end = body.find("\r\n", cursor);
    if (line_end == std::string::npos) return std::nullopt;
    std::size_t chunk_size = 0;
    try { chunk_size = std::stoul(body.substr(cursor, line_end - cursor), nullptr, 16); }
    catch (...) { return std::nullopt; }
    cursor = line_end + 2;
    if (chunk_size == 0) return decoded;
    if (chunk_size > body.size() - cursor || body.size() - cursor - chunk_size < 2 ||
        body.compare(cursor + chunk_size, 2, "\r\n") != 0) return std::nullopt;
    decoded.append(body, cursor, chunk_size);
    cursor += chunk_size + 2;
  }
}

std::string request_through_socks(std::uint16_t socks_port, std::uint16_t target_port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  timeval timeout{4, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in proxy{};
  proxy.sin_family = AF_INET;
  proxy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  proxy.sin_port = htons(socks_port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&proxy), sizeof(proxy)) != 0) { ::close(fd); return {}; }
  const char greeting[]{5, 1, 0};
  char selection[2]{};
  if (!send_all(fd, {greeting, sizeof(greeting)}) || !recv_exact(fd, selection, sizeof(selection)) ||
      selection[0] != 5 || selection[1] != 0) { ::close(fd); return {}; }
  const std::array<unsigned char, 10> request{
      5, 1, 0, 1, 127, 0, 0, 1,
      static_cast<unsigned char>(target_port >> 8), static_cast<unsigned char>(target_port & 0xff)};
  if (!send_all(fd, {reinterpret_cast<const char*>(request.data()), request.size()})) { ::close(fd); return {}; }
  char response[4]{};
  if (!recv_exact(fd, response, sizeof(response)) || response[1] != 0) { ::close(fd); return {}; }
  std::size_t address_remainder = response[3] == 1 ? 6 : response[3] == 4 ? 18 : 0;
  if (response[3] == 3) {
    unsigned char length = 0;
    if (!recv_exact(fd, reinterpret_cast<char*>(&length), 1)) { ::close(fd); return {}; }
    address_remainder = static_cast<std::size_t>(length) + 2;
  }
  std::array<char, 258> ignored{};
  if (address_remainder == 0 || !recv_exact(fd, ignored.data(), address_remainder) ||
      !send_all(fd, "GET /rc04 HTTP/1.1\r\nHost: loopback\r\nConnection: close\r\n\r\n")) {
    ::close(fd); return {};
  }
  std::string body;
  std::array<char, 2048> chunk{};
  for (;;) {
    const auto count = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (count <= 0) break;
    body.append(chunk.data(), static_cast<std::size_t>(count));
  }
  ::close(fd);
  return body;
}

void run_loop_for(uv_loop_t& loop, std::chrono::milliseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until) {
    uv_run(&loop, UV_RUN_NOWAIT);
    std::this_thread::sleep_for(1ms);
  }
}

bool wait_for_samples(uv_loop_t& loop, const MetricsClient& metrics,
                      std::size_t& samples, std::size_t wanted,
                      std::chrono::seconds timeout = 7s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline && samples < wanted) run_loop_for(loop, 10ms);
  return samples >= wanted;
}

bool positive_metric_counter(const Json& value) {
  if (value.is_number_integer() || value.is_number_unsigned()) {
    try { return value.get<std::int64_t>() > 0; } catch (...) { return value.get<std::uint64_t>() > 0; }
  }
  if (value.is_object()) {
    for (const auto& [key, child] : value.items()) {
      (void)key;
      if (positive_metric_counter(child)) return true;
    }
  } else if (value.is_array()) {
    for (const auto& child : value) if (positive_metric_counter(child)) return true;
  }
  return false;
}

void describe_handle(uv_handle_t* handle, void* opaque) {
  auto* output = static_cast<std::string*>(opaque);
  *output += std::string(uv_handle_type_name(uv_handle_get_type(handle))) +
      " active=" + (uv_is_active(handle) ? "yes" : "no") +
      " closing=" + (uv_is_closing(handle) ? "yes" : "no") + "; ";
}

bool save_module(ProfileService& profiles, const Id& profile, const std::string& key,
                 const Json& value) {
  auto existing = profiles.module(profile, key);
  if (!existing) return false;
  const auto base = existing->document
      ? std::optional<std::string>(existing->document->source_text) : std::nullopt;
  if (!profiles.save_module(profile, {key, {base}, value.dump(2) + "\n"})) return false;
  return static_cast<bool>(profiles.include_module(profile, key, true));
}

Result<PreparedDirectory> make_runtime(ProfileService& profiles, const Id& profile,
                                       RuntimeWriter& writer, const Id& directory_id) {
  auto snapshot = profiles.snapshot(profile);
  if (!snapshot) return std::unexpected(snapshot.error());
  auto prepared = assemble(BuildInput{*snapshot, {}});
  if (!prepared) return std::unexpected(prepared.error());
  auto directory = writer.materialize(*prepared, DirectoryPurpose::candidate, directory_id);
  if (!directory) return std::unexpected(directory.error());
  return *directory;
}

}  // namespace

TEST_CASE("RC-04 fixed Xray exports real stats and MetricsClient computes rates",
          "[real_core][RC-04]") {
  const auto xray = test_xray();
  if (xray.empty() || !std::filesystem::exists(xray)) SKIP("set XRM_TEST_XRAY to the fixed real Xray binary");
  const auto version = run_command(xray, {"version"}, std::filesystem::temp_directory_path());
  INFO("xray version stdout: " << version.out);
  REQUIRE(version.code == 0);
  REQUIRE(version.out.find("26.9.9") != std::string::npos);
  REQUIRE(version.out.find("52a412d") != std::string::npos);

  TempDirectory temp;
  auto database = Store::open(temp.path() / "state.sqlite");
  REQUIRE(database);
  ProfileService profiles(**database, temp.path() / "profiles");
  auto profile = profiles.create({"RC-04 metrics", "test-core", "local", {}});
  REQUIRE(profile);
  auto profile_settings = profiles.get(profile->id);
  REQUIRE(profile_settings);
  profile_settings->append_selected_default = false;
  const auto profile_path = temp.path() / "profiles" / profile->id / "profile.json";
  REQUIRE(profiles.save(profile->id, {{read_file(profile_path)}, *profile_settings}));
  REQUIRE(profiles.save_module(profile->id, {"stats", {std::nullopt}, R"({"stats":{}})"}));
  REQUIRE(profiles.save_module(profile->id, {"policy", {std::nullopt},
      R"({"policy":{"levels":{"0":{"connIdle":71,"rc04Keep":"level"}},"system":{"other":"preserved"}}})"}));
  REQUIRE(profiles.save_module(profile->id, {"metrics", {std::nullopt},
      R"({"metrics":{"unrelatedField":"preserved"}})"}));
  auto socks_port = free_port();
  auto metrics_port = free_port();
  REQUIRE(socks_port);
  REQUIRE(metrics_port);
  REQUIRE(save_module(profiles, profile->id, "inbounds", Json{{"inbounds", Json::array({Json{
      {"listen", "127.0.0.1"}, {"port", *socks_port}, {"protocol", "socks"},
      {"tag", "rc04-socks"}, {"settings", Json{{"auth", "noauth"}, {"udp", false}}}}})}}));
  auto snapshot = profiles.snapshot(profile->id);
  REQUIRE(snapshot);
  auto plan = plan_traffic_stats(*snapshot, true,
                                 "127.0.0.1:" + std::to_string(*metrics_port));
  REQUIRE(plan);
  const auto saved = save_module_changes(profiles, profile->id, *plan);
  INFO("saving native statistics modules failed: "
       << (saved.failure ? saved.failure->message : "no failure")
       << " subject=" << (saved.failure ? saved.failure->subject.value_or("") : ""));
  REQUIRE_FALSE(saved.failure.has_value());
  CHECK(saved.saved == std::vector<std::string>{"stats", "policy", "metrics"});
  const auto stats_setting = profiles.module(profile->id, "stats");
  const auto policy_setting = profiles.module(profile->id, "policy");
  const auto metrics_setting = profiles.module(profile->id, "metrics");
  REQUIRE(stats_setting);
  REQUIRE(policy_setting);
  REQUIRE(metrics_setting);
  CHECK(stats_setting->setting.included);
  CHECK(policy_setting->setting.included);
  CHECK(metrics_setting->setting.included);
  const auto source_snapshot = profiles.snapshot(profile->id);
  REQUIRE(source_snapshot);
  const auto source_policy = std::find_if(source_snapshot->modules.begin(), source_snapshot->modules.end(),
      [](const auto& document) { return document.key == "policy"; });
  REQUIRE(source_policy != source_snapshot->modules.end());
  const auto policy_json = Json::parse(source_policy->source_text).at("policy");
  CHECK(policy_json.at("levels").at("0").at("rc04Keep") == "level");
  CHECK(policy_json.at("system").at("other") == "preserved");
  const auto source_metrics = std::find_if(source_snapshot->modules.begin(), source_snapshot->modules.end(),
      [](const auto& document) { return document.key == "metrics"; });
  REQUIRE(source_metrics != source_snapshot->modules.end());
  CHECK(Json::parse(source_metrics->source_text).at("metrics").at("unrelatedField") == "preserved");

  RuntimeWriter writer(temp.path() / "runtime");
  auto runtime = make_runtime(profiles, profile->id, writer, "rc04-first");
  const std::string assembly_error = runtime
      ? "none" : runtime.error().message + " subject=" + runtime.error().subject.value_or("");
  INFO("runtime assembly error: " << assembly_error);
  REQUIRE(runtime);
  const auto checked = run_command(xray, {"run", "-test", "-confdir", runtime->confdir.string()}, temp.path());
  INFO("real xray -test stderr: " << checked.err);
  REQUIRE(checked.code == 0);
  HttpMarker marker;
  REQUIRE(marker.start());

  OwnedProcess core;
  REQUIRE(core.start(xray, runtime->confdir, temp.path()));
  REQUIRE(wait_port(*socks_port));
  REQUIRE(wait_port(*metrics_port));
  const auto endpoint = "http://127.0.0.1:" + std::to_string(*metrics_port) + "/debug/vars";

  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    {
      MetricsClient metrics(loop, http);
      RunRecord record;
      record.run_id = "rc04-generation-1";
      record.profile_id = profile->id;
      record.metrics_endpoint = endpoint;
      metrics.attach(record, endpoint);
      std::size_t samples = 0;
      metrics.on_sample([&](const MetricsSample&) { ++samples; });
      metrics.set_viewer_count(1);
      REQUIRE(wait_for_samples(loop, metrics, samples, 1));

      for (int i = 0; i < 4; ++i) {
        const auto response = request_through_socks(*socks_port, marker.port());
        INFO("loopback request response: " << response);
        REQUIRE(response.find("XRM-RC04-TRAFFIC-9b27a1") != std::string::npos);
      }

      const auto raw = get_http_body(*metrics_port, "/debug/vars");
      REQUIRE(raw);
      INFO("actual Xray /debug/vars stdout: " << *raw);
      const auto metrics_json = Json::parse(*raw);
      REQUIRE(metrics_json.contains("stats"));
      REQUIRE(positive_metric_counter(metrics_json.at("stats")));

      REQUIRE(wait_for_samples(loop, metrics, samples, 2));
      auto view = metrics.view();
      REQUIRE(view.availability == MetricAvailability::available);
      REQUIRE(view.sample);
      CHECK(view.sample->run_id == record.run_id);
      REQUIRE_FALSE(view.sample->traffic.empty());
      CHECK(std::any_of(view.sample->traffic.begin(), view.sample->traffic.end(), [](const auto& counter) {
        return (counter.uplink && *counter.uplink > 0) || (counter.downlink && *counter.downlink > 0);
      }));
      REQUIRE_FALSE(view.rates.empty());
      CHECK(std::any_of(view.rates.begin(), view.rates.end(), [](const auto& rate) {
        return (rate.uplink_bytes_per_second && *rate.uplink_bytes_per_second >= 0) ||
               (rate.downlink_bytes_per_second && *rate.downlink_bytes_per_second >= 0);
      }));

      REQUIRE(core.stop());
      record.run_id = "rc04-generation-2";
      metrics.attach(record, endpoint);
      REQUIRE(core.start(xray, runtime->confdir, temp.path()));
      REQUIRE(wait_port(*metrics_port));
      REQUIRE(wait_for_samples(loop, metrics, samples, 3));
      view = metrics.view();
      REQUIRE(view.sample);
      CHECK(view.sample->run_id == record.run_id);
      CHECK(view.rates.empty());
      metrics.stop();
    }

    {
      auto unavailable_profile = profiles.create({"RC-04 unavailable", "test-core", "local", {}});
      REQUIRE(unavailable_profile);
      const auto unavailable_modules = profiles.modules(unavailable_profile->id);
      REQUIRE(unavailable_modules);
      for (const auto key : {"stats", "policy", "metrics"}) {
        const auto setting = std::find_if(unavailable_modules->begin(), unavailable_modules->end(),
            [&](const auto& module) { return module.descriptor.key == key; });
        REQUIRE(setting != unavailable_modules->end());
        CHECK_FALSE(setting->setting.included);
      }
      RunRecord unavailable_run;
      unavailable_run.run_id = "rc04-no-stats";
      unavailable_run.profile_id = unavailable_profile->id;
      MetricsClient unavailable(loop, http);
      unavailable.attach(unavailable_run, std::nullopt);
      CHECK(unavailable.view().availability == MetricAvailability::unavailable);
      CHECK_FALSE(unavailable.view().sample.has_value());
      CHECK(unavailable.view().rates.empty());
      unavailable.stop();
    }
    bool http_drained = false;
    http.shutdown([&] { http_drained = true; });
    while (!http_drained) uv_run(&loop, UV_RUN_DEFAULT);
  }
  REQUIRE(core.stop());
  std::string open_handles;
  uv_walk(&loop, describe_handle, &open_handles);
  INFO("remaining libuv handles: " << open_handles);
  uv_run(&loop, UV_RUN_DEFAULT);
  REQUIRE(uv_loop_close(&loop) == 0);
}

/// @file
/// @brief Exercises native routing presets against the fixed Xray executable.

#include "infra/files.hpp"
#include "infra/store.hpp"
#include "server/application.hpp"
#include "xray/codecs/codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uv.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char **environ;
using namespace xrm;
using namespace std::chrono_literals;

namespace {

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xrm-p5-preset-real-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::string bytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool send_all(int fd, std::string_view text) {
  std::size_t sent = 0;
  while (sent < text.size()) {
    const auto count =
        ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
    if (count <= 0)
      return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<std::uint16_t> free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return std::nullopt;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return {};
  }
  socklen_t length = sizeof(address);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
  ::close(fd);
  return ntohs(address.sin_port);
}

bool wait_port(std::uint16_t port) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const bool ready = ::connect(fd, reinterpret_cast<sockaddr *>(&address),
                                 sizeof(address)) == 0;
    ::close(fd);
    if (ready)
      return true;
    (void)::poll(nullptr, 0, 20);
  }
  return false;
}

struct Process {
  pid_t pid = -1;
  std::filesystem::path stderr_file;
  bool start(const std::filesystem::path &executable,
             std::vector<std::string> arguments,
             const std::filesystem::path &root,
             std::optional<std::filesystem::path> asset_dir = std::nullopt) {
    static unsigned sequence = 0;
    stderr_file = root / ("xray-" + std::to_string(++sequence) + ".err");
    std::vector<std::string> storage{executable.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    for (auto &item : storage)
      argv.push_back(item.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    if (asset_dir) {
      for (char **entry = environ; *entry != nullptr; ++entry)
        if (!std::string_view(*entry).starts_with("XRAY_LOCATION_ASSET="))
          environment.emplace_back(*entry);
      environment.push_back("XRAY_LOCATION_ASSET=" + asset_dir->string());
    }
    std::vector<char *> envp;
    for (auto &item : environment)
      envp.push_back(item.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                     stderr_file.c_str(),
                                     O_CREAT | O_TRUNC | O_WRONLY, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
    const int status =
        posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(),
                    asset_dir ? envp.data() : environ);
    posix_spawn_file_actions_destroy(&actions);
    return status == 0;
  }
  void stop() {
    if (pid <= 0)
      return;
    ::kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        pid = -1;
        return;
      }
      (void)::poll(nullptr, 0, 20);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    pid = -1;
  }
  ~Process() { stop(); }
};

class Marker {
public:
  bool start(std::string marker) {
    marker_ = std::move(marker);
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0)
      return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 16) != 0)
      return false;
    socklen_t length = sizeof(address);
    ::getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length);
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] {
      while (!stopping_) {
        pollfd pfd{listener_, POLLIN, 0};
        if (::poll(&pfd, 1, 100) <= 0)
          continue;
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0)
          continue;
        timeval timeout{3, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
        char request[1024];
        (void)::recv(client, request, sizeof(request), 0);
        const auto response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                              std::to_string(marker_.size()) +
                              "\r\nConnection: close\r\n\r\n" + marker_;
        (void)send_all(client, response);
        ::close(client);
      }
    });
    return true;
  }
  std::uint16_t port() const { return port_; }
  ~Marker() {
    stopping_ = true;
    if (listener_ >= 0) {
      ::shutdown(listener_, SHUT_RDWR);
      ::close(listener_);
    }
    if (worker_.joinable())
      worker_.join();
  }

private:
  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
  std::string marker_;
};

std::string socks_get(std::uint16_t socks_port, std::string_view host,
                      std::uint16_t destination) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(socks_port);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return {};
  }
  timeval timeout{3, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  const std::string greeting{"\x05\x01\x00", 3};
  if (!send_all(fd, greeting)) {
    ::close(fd);
    return {};
  }
  char reply[512]{};
  if (::recv(fd, reply, 2, MSG_WAITALL) != 2 || reply[1] != 0) {
    ::close(fd);
    return {};
  }
  std::string connect{"\x05\x01\x00\x03", 4};
  connect.push_back(static_cast<char>(host.size()));
  connect += host;
  connect.push_back(static_cast<char>(destination >> 8));
  connect.push_back(static_cast<char>(destination & 0xff));
  if (!send_all(fd, connect) || ::recv(fd, reply, 10, 0) <= 0 ||
      reply[1] != 0) {
    ::close(fd);
    return {};
  }
  const auto request = "GET /preset HTTP/1.1\r\nHost: " + std::string(host) +
                       "\r\nConnection: close\r\n\r\n";
  if (!send_all(fd, request)) {
    ::close(fd);
    return {};
  }
  std::string response;
  for (;;) {
    const auto size = ::recv(fd, reply, sizeof(reply), 0);
    if (size <= 0)
      break;
    response.append(reply, static_cast<std::size_t>(size));
  }
  ::close(fd);
  return response;
}

bool socks_udp_443_is_blocked(std::uint16_t socks_port,
                              const std::filesystem::path &access_file) {
  const int control = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in proxy{};
  proxy.sin_family = AF_INET;
  proxy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  proxy.sin_port = htons(socks_port);
  if (::connect(control, reinterpret_cast<sockaddr *>(&proxy), sizeof(proxy)) !=
      0) {
    ::close(control);
    return false;
  }
  timeval timeout{2, 0};
  ::setsockopt(control, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char reply[32]{};
  if (!send_all(control, std::string{"\x05\x01\x00", 3}) ||
      ::recv(control, reply, 2, MSG_WAITALL) != 2 || reply[1] != 0 ||
      !send_all(control, std::string{"\x05\x03\x00\x01\x7f\0\0\x01\0\0", 10}) ||
      ::recv(control, reply, 10, MSG_WAITALL) != 10 || reply[1] != 0 ||
      reply[3] != 1) {
    ::close(control);
    return false;
  }
  const auto udp_port =
      static_cast<std::uint16_t>((static_cast<unsigned char>(reply[8]) << 8) |
                                 static_cast<unsigned char>(reply[9]));
  const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in relay{};
  relay.sin_family = AF_INET;
  relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  relay.sin_port = htons(udp_port);
  const auto baseline = bytes(access_file).size();
  const std::string packet =
      std::string{"\0\0\0\x01\x7f\0\0\x01\x01\xbb", 10} + "P5-UDP-BLOCK";
  const auto sent =
      ::sendto(udp, packet.data(), packet.size(), 0,
               reinterpret_cast<sockaddr *>(&relay), sizeof(relay));
  bool routed_to_block = false;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (sent == static_cast<ssize_t>(packet.size()) &&
         std::chrono::steady_clock::now() < deadline) {
    const auto log = bytes(access_file);
    if (log.size() > baseline &&
        log.substr(baseline).find("[local-socks -> block]") !=
            std::string::npos) {
      routed_to_block = true;
      break;
    }
    (void)::poll(nullptr, 0, 20);
  }
  ::close(udp);
  ::close(control);
  return routed_to_block;
}

std::string varint(std::uint64_t value) {
  std::string result;
  while (value > 127) {
    result.push_back(static_cast<char>((value & 127) | 128));
    value >>= 7;
  }
  result.push_back(static_cast<char>(value));
  return result;
}
std::string field(unsigned number, std::string_view value) {
  return varint(number << 3 | 2) + varint(value.size()) + std::string(value);
}
std::string integer(unsigned number, std::uint64_t value) {
  return varint(number << 3) + varint(value);
}
std::string geoip() {
  std::string result;
  const auto cidr = field(1, std::string{"\x7f\0\0\0", 4}) + integer(2, 8);
  for (std::string_view label : {"PRIVATE", "CN", "FACEBOOK", "FASTLY",
                                 "GOOGLE", "NETFLIX", "TELEGRAM", "TWITTER"})
    result += field(1, field(1, label) + field(2, cidr));
  return result;
}
std::string geosite() {
  std::string result;
  for (const auto &[label, name] :
       std::array<std::pair<std::string_view, std::string_view>, 6>{
           {{"PRIVATE", "local.private.test"},
            {"CN", "cn.test"},
            {"GOOGLE", "google.test"},
            {"GFW", "gfw.test"},
            {"GREATFIRE", "greatfire.test"},
            {"OTHER", "other.test"}}})
    result +=
        field(1, field(1, label) + field(2, integer(1, 3) + field(2, name)));
  return result;
}

void save_module(ProfileService &profiles, const Id &profile_id,
                 std::string_view key, Json value) {
  auto module = profiles.module(profile_id, key);
  REQUIRE(module);
  REQUIRE(module->document);
  REQUIRE(profiles.save_module(profile_id,
                               ModuleEdit{std::string(key),
                                          {module->document->source_text},
                                          value.dump(2) + "\n"}));
}

} // namespace

TEST_CASE("P5 fixed Xray routes preset direct, proxy and block through native "
          "multi-file confdir",
          "[real_core]") {
  const char *configured = std::getenv("XRM_TEST_XRAY");
  if (!configured || !std::filesystem::exists(configured))
    SKIP("fixed Xray unavailable");
  const std::filesystem::path xray{configured};
  TempDirectory temp;
  Marker direct, proxy;
  REQUIRE(direct.start("P5-DIRECT-MARKER"));
  REQUIRE(proxy.start("P5-PROXY-MARKER"));
  const auto socks_port = free_port(), vless_port = free_port();
  REQUIRE(socks_port);
  REQUIRE(vless_port);
  constexpr std::string_view uuid = "00000000-0000-4000-8000-000000000055";
  Json peer{{"log", {{"loglevel", "info"}}},
            {"inbounds",
             Json::array({Json{{"listen", "127.0.0.1"},
                               {"port", *vless_port},
                               {"protocol", "vless"},
                               {"settings",
                                {{"clients", Json::array({Json{{"id", uuid}}})},
                                 {"decryption", "none"}}}}})}};
  peer["outbounds"] = Json::array({Json{
      {"tag", "redirect"},
      {"protocol", "freedom"},
      {"settings",
       Json{{"redirect", "127.0.0.1:" + std::to_string(proxy.port())},
            {"finalRules",
             Json::array({Json{{"action", "allow"},
                               {"network", "tcp"},
                               {"port", std::to_string(proxy.port())},
                               {"ip", Json::array({"127.0.0.0/8"})}}})}}}}});
  const auto peer_file = temp.path / "peer.json";
  {
    std::ofstream out(peer_file);
    out << peer.dump(2);
  }
  Process peer_process;
  REQUIRE(
      peer_process.start(xray, {"run", "-c", peer_file.string()}, temp.path));
  INFO(bytes(peer_process.stderr_file));
  REQUIRE(wait_port(*vless_port));

  const auto ip_file = temp.path / "geoip.dat",
             site_file = temp.path / "geosite.dat";
  {
    std::ofstream out(ip_file, std::ios::binary);
    out << geoip();
  }
  {
    std::ofstream out(site_file, std::ios::binary);
    out << geosite();
  }
  auto store = Store::open(temp.path / "state.sqlite");
  REQUIRE(store);
  REQUIRE(
      (*store)->put_core(CoreInstall{"fixed", "fixed", xray, "26.9.9", false}));
  for (const auto &[id, kind, file, labels] : std::array{
           std::tuple{"ip", GeoAssetKind::geoip, ip_file,
                      std::set<std::string>{"private", "cn", "facebook",
                                            "fastly", "google", "netflix",
                                            "telegram", "twitter"}},
           std::tuple{"site", GeoAssetKind::geosite, site_file,
                      std::set<std::string>{"private", "cn", "google", "gfw",
                                            "greatfire", "other"}}}) {
    GeodataResourceRecord record;
    record.id = id;
    record.name = id;
    record.kind = kind;
    record.file_name = file.filename().string();
    record.local_path = file;
    REQUIRE((*store)->put_geodata_resource(record));
    REQUIRE((*store)->commit_geodata_resource_result(
        id, 1,
        GeodataAssetVersion{id, kind, record.file_name,
                            "fixture-" + std::string(id), temp.path, file,
                            labels, std::filesystem::file_size(file)},
        1, "success"));
  }
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    ProcessService processes(loop);
    SubscriptionService subscriptions(**store, http);
    Application app(**store, processes, http, loop, temp.path / "data", "test",
                    "boot", subscriptions);
    auto profile = app.profiles().create({"Preset", "fixed", "local", {}});
    REQUIRE(profile);
    auto parsed = parse_vless_uri(std::string("vless://") + std::string(uuid) +
                                  "@127.0.0.1:" + std::to_string(*vless_port) +
                                  "?encryption=none&type=tcp#P5-proxy");
    REQUIRE(parsed);
    parsed->node.id = "p5-proxy-node";
    REQUIRE((*store)->put_node(parsed->node));
    REQUIRE(app.select(profile->id, Target{TargetKind::node, parsed->node.id}));
    REQUIRE(app.set_profile_geodata(
        profile->id, {{"geoip.dat", "ip"}, {"geosite.dat", "site"}}));
    save_module(
        app.profiles(), profile->id, "inbounds",
        Json{{"inbounds",
              Json::array(
                  {Json{{"listen", "127.0.0.1"},
                        {"port", *socks_port},
                        {"protocol", "socks"},
                        {"tag", "local-socks"},
                        {"settings", {{"auth", "noauth"}, {"udp", true}}}}})}});
    const auto access = (temp.path / "access.log").string();
    save_module(app.profiles(), profile->id, "log",
                Json{{"log", {{"loglevel", "info"}, {"access", access}}}});
    save_module(
        app.profiles(), profile->id, "dns",
        Json{{"dns",
              {{"hosts",
                {{"other.test", "127.0.0.1"}, {"cn.test", "127.0.0.1"}}},
               {"servers", Json::array({"localhost"})}}}});
    save_module(
        app.profiles(), profile->id, "outbounds",
        Json{{"outbounds",
              Json::array(
                  {Json{{"tag", "direct"},
                        {"protocol", "freedom"},
                        {"settings", {{"domainStrategy", "UseIP"}}}},
                   Json{{"tag", "block"}, {"protocol", "blackhole"}}})}});

    for (std::string_view preset : {"direct_only", "bypass_mainland",
                                    "blacklist_proxy", "global_proxy"}) {
      auto plan = app.plan_routing_preset(profile->id, preset);
      INFO("preset=" << preset
                     << (plan ? "" : " plan error=" + plan.error().message));
      REQUIRE(plan);
      REQUIRE(app.save_routing_preset(
          profile->id, preset, plan->at("expected_text").get<std::string>()));
      auto rendered = app.render(profile->id);
      INFO("preset=" << preset
                     << (rendered
                             ? ""
                             : " render error=" + rendered.error().message));
      REQUIRE(rendered);
      REQUIRE(std::filesystem::exists(rendered->confdir / "04_routing.json"));
      REQUIRE(std::filesystem::exists(rendered->confdir / "geoip.dat"));
      REQUIRE(std::filesystem::exists(rendered->confdir / "geosite.dat"));
      Process check;
      REQUIRE(check.start(
          xray, {"run", "-test", "-confdir", rendered->confdir.string()},
          temp.path, rendered->confdir));
      int check_status = 0;
      REQUIRE(::waitpid(check.pid, &check_status, 0) == check.pid);
      check.pid = -1;
      INFO("xray -test: " << bytes(check.stderr_file));
      REQUIRE(WIFEXITED(check_status));
      REQUIRE(WEXITSTATUS(check_status) == 0);
      Process client;
      REQUIRE(client.start(xray,
                           {"run", "-confdir", rendered->confdir.string()},
                           temp.path, rendered->confdir));
      INFO("client startup: " << bytes(client.stderr_file));
      REQUIRE(wait_port(*socks_port));
      const auto local = socks_get(*socks_port, "127.0.0.1", direct.port());
      INFO("local response: " << local
                              << " stderr=" << bytes(client.stderr_file));
      CHECK(local.find("P5-DIRECT-MARKER") != std::string::npos);
      if (preset != "direct_only") {
        const auto remote =
            socks_get(*socks_port, "google.test", direct.port());
        INFO("proxy response: "
             << remote << " client stderr=" << bytes(client.stderr_file)
             << " peer stderr=" << bytes(peer_process.stderr_file));
        REQUIRE(remote.find("P5-PROXY-MARKER") != std::string::npos);
        INFO("UDP/443 access log: " << bytes(access));
        CHECK(socks_udp_443_is_blocked(*socks_port, access));
      }
      if (preset == "bypass_mainland" || preset == "blacklist_proxy" ||
          preset == "global_proxy") {
        const auto fallback =
            socks_get(*socks_port, "other.test", direct.port());
        INFO("fallback response: " << fallback
                                   << " client=" << bytes(client.stderr_file));
        if (preset == "blacklist_proxy")
          CHECK(fallback.find("P5-DIRECT-MARKER") != std::string::npos);
        else
          CHECK(fallback.find("P5-PROXY-MARKER") != std::string::npos);
        const auto mainland = socks_get(*socks_port, "cn.test", direct.port());
        INFO("mainland response: " << mainland
                                   << " client=" << bytes(client.stderr_file));
        if (preset == "global_proxy")
          CHECK(mainland.find("P5-PROXY-MARKER") != std::string::npos);
        else
          CHECK(mainland.find("P5-DIRECT-MARKER") != std::string::npos);
      }
      // UDP/443 has a native blackhole rule in every proxy preset; the real
      // process must accept the corresponding routing module, not just a JSON
      // assertion.
      const auto routing =
          Json::parse(bytes(rendered->confdir / "04_routing.json"))
              .at("routing");
      CHECK(routing.at("rules").is_array());
      if (preset != "direct_only") {
        CHECK(routing.at("rules").at(0).at("outboundTag") == "block");
        CHECK(routing.at("rules").at(0).at("port") == "443");
      }
      client.stop();
    }
  }
  uv_run(&loop, UV_RUN_NOWAIT);
  REQUIRE(uv_loop_close(&loop) == 0);
}

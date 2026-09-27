// XRM: Tests rc08 in the client, daemon, persistence, and Xray integration
// suite.

#include <catch2/catch_test_macros.hpp>

#include "infra/store.hpp"
#include "model/module_catalog.hpp"
#include "xray/codecs/codec.hpp"
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern char **environ;

namespace {
using namespace std::chrono_literals;
using namespace xrm;

std::filesystem::path real_xray() {
  if (const char *value = std::getenv("XRM_TEST_XRAY"); value && *value)
    return std::filesystem::path(value);
#ifdef XRM_TEST_XRAY_CONFIGURED
  return XRM_TEST_XRAY_CONFIGURED;
#else
  return {};
#endif
}

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("xrm-rc08-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct Captured {
  int code = -1;
  std::string out;
  std::string err;
  bool timed_out = false;
};

Captured run_command(const std::filesystem::path &executable,
                     const std::vector<std::string> &arguments,
                     const std::filesystem::path &root,
                     std::chrono::seconds timeout = 20s) {
  static std::atomic<unsigned> sequence{0};
  const auto index = sequence.fetch_add(1);
  const auto out_path = root / ("command-" + std::to_string(index) + ".out");
  const auto err_path = root / ("command-" + std::to_string(index) + ".err");
  std::vector<std::string> values{executable.string()};
  values.insert(values.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &value : values)
    argv.push_back(value.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0600);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                                  argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0)
    return {};

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      const int code = WIFEXITED(status)     ? WEXITSTATUS(status)
                       : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                             : -1;
      return {code, read_file(out_path), read_file(err_path), false};
    }
    if (waited < 0)
      break;
    ::poll(nullptr, 0, 10);
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return {-1, read_file(out_path), read_file(err_path), true};
}

std::string base64(std::string_view input) {
  constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve((input.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < input.size(); i += 3) {
    const auto a = static_cast<unsigned char>(input[i]);
    const auto b =
        i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
    const auto c =
        i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
    encoded.push_back(alphabet[a >> 2]);
    encoded.push_back(alphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
    encoded.push_back(
        i + 1 < input.size() ? alphabet[((b & 0x0fU) << 2U) | (c >> 6U)] : '=');
    encoded.push_back(i + 2 < input.size() ? alphabet[c & 0x3fU] : '=');
  }
  return encoded;
}

std::string vmess_uri() {
  const Json share{{"v", "2"},
                   {"ps", "RC08 VMess"},
                   {"add", "127.0.0.1"},
                   {"port", "443"},
                   {"id", "00000000-0000-4000-8000-000000000081"},
                   {"aid", "0"},
                   {"scy", "auto"},
                   {"net", "tcp"},
                   {"type", "none"},
                   {"tls", ""}};
  return "vmess://" + base64(share.dump());
}

Result<DecodedNode> parse_vmess(std::string_view uri) {
  return parse_vmess_uri(uri);
}
Result<DecodedNode> parse_trojan(std::string_view uri) {
  return parse_trojan_uri(uri);
}
Result<DecodedNode> parse_shadowsocks(std::string_view uri) {
  return parse_shadowsocks_uri(uri);
}
Result<DecodedNode> parse_hysteria2(std::string_view uri) {
  return parse_hysteria2_uri(uri);
}

std::optional<std::uint16_t> free_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return std::nullopt;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return std::nullopt;
  }
  socklen_t size = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  const auto port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}

bool check_typed_uri(const std::filesystem::path &xray, TempDirectory &temp,
                     std::string_view protocol, std::string_view uri,
                     Result<DecodedNode> (*parse)(std::string_view)) {
  constexpr std::string_view node_id = "rc08-protocol-node";
  auto decoded = parse(uri);
  INFO("parse result: " << (decoded ? "success" : decoded.error().message));
  REQUIRE(decoded.has_value());
  decoded->node.id = std::string(node_id);
  REQUIRE(std::holds_alternative<StreamNode>(decoded->node.settings));

  auto store = Store::open(temp.path / "nodes.sqlite");
  REQUIRE(store.has_value());
  REQUIRE((*store)->put_node(decoded->node).has_value());
  auto reloaded = (*store)->get_node(std::string(node_id));
  REQUIRE(reloaded.has_value());
  REQUIRE(std::holds_alternative<StreamNode>(reloaded->settings));

  const auto inbound_port = free_loopback_port();
  REQUIRE(inbound_port.has_value());

  ProfileSnapshot snapshot;
  snapshot.profile.id = "rc08-profile";
  snapshot.profile.name = "RC-08 " + std::string(protocol);
  snapshot.profile.core_id = "rc08-fixed-xray";
  snapshot.profile.selected = Target{TargetKind::node, std::string(node_id)};
  snapshot.profile.append_selected_default = true;
  snapshot.profile.modules = {{"inbounds", "02_inbounds.json", true},
                              {"outbounds", "03_outbounds.json", true},
                              {"routing", "04_routing.json", true}};
  snapshot.modules = {
      {"inbounds", "02_inbounds.json",
       Json{{"inbounds",
             Json::array({Json{
                 {"listen", "127.0.0.1"},
                 {"port", *inbound_port},
                 {"protocol", "socks"},
                 {"tag", "rc08-socks"},
                 {"settings", Json{{"auth", "noauth"}, {"udp", false}}}}})}}
           .dump()},
      {"outbounds", "03_outbounds.json", R"({"outbounds":[]})"},
      {"routing", "04_routing.json", R"({"routing":{"rules":[]}})"}};

  auto prepared = assemble(BuildInput{std::move(snapshot), {*reloaded}});
  INFO("RuntimeAssembler error: " << (prepared ? "none"
                                               : prepared.error().message));
  REQUIRE(prepared.has_value());
  const auto tail =
      std::find_if(prepared->files.begin(), prepared->files.end(),
                   [](const OutputFile &file) {
                     return file.relative_name == "90_nodes_tail.json";
                   });
  REQUIRE(tail != prepared->files.end());
  const auto tail_json = Json::parse(tail->bytes);
  REQUIRE(tail_json.at("outbounds").size() == 1);
  CHECK(tail_json.at("outbounds").at(0).at("protocol") == protocol);

  RuntimeWriter writer(temp.path / "runtime-data");
  auto directory = writer.materialize(*prepared, DirectoryPurpose::candidate,
                                      "rc08-" + std::string(protocol));
  REQUIRE(directory.has_value());
  CHECK_FALSE(std::filesystem::exists(directory->confdir / "config.json"));
  INFO("generated node tail: "
       << read_file(directory->confdir / "90_nodes_tail.json"));

  const auto checked = run_command(
      xray, {"run", "-test", "-confdir", directory->confdir.string()},
      temp.path);
  INFO("Xray stdout: " << checked.out);
  INFO("Xray stderr: " << checked.err);
  INFO("Xray timed out: " << checked.timed_out);
  CHECK_FALSE(checked.timed_out);
  CHECK(checked.code == 0);
  return checked.code == 0 && !checked.timed_out;
}

bool require_fixed_xray(const std::filesystem::path &xray,
                        const std::filesystem::path &temp) {
  if (xray.empty() || !std::filesystem::exists(xray))
    SKIP("set XRM_TEST_XRAY to Xray v26.9.9");
  const auto version = run_command(xray, {"version"}, temp);
  INFO("Xray version stdout: " << version.out);
  INFO("Xray version stderr: " << version.err);
  REQUIRE(version.code == 0);
  REQUIRE(version.out.find("26.9.9") != std::string::npos);
  REQUIRE(version.out.find("52a412d") != std::string::npos);
  return true;
}

} // namespace

TEST_CASE("RC-08 VMess URI becomes an Xray v26.9.9 outbound through Store and "
          "RuntimeAssembler",
          "[real_core][RC-08]") {
  const auto xray = real_xray();
  TempDirectory temp;
  REQUIRE(require_fixed_xray(xray, temp.path));
  REQUIRE(check_typed_uri(xray, temp, "vmess", vmess_uri(), parse_vmess));
}

TEST_CASE("RC-08 Trojan URI becomes an Xray v26.9.9 outbound through Store and "
          "RuntimeAssembler",
          "[real_core][RC-08]") {
  const auto xray = real_xray();
  TempDirectory temp;
  REQUIRE(require_fixed_xray(xray, temp.path));
  REQUIRE(check_typed_uri(
      xray, temp, "trojan",
      "trojan://rc08-secret@127.0.0.1:443?security=none#rc08-trojan",
      parse_trojan));
}

TEST_CASE("RC-08 Shadowsocks URI becomes an Xray v26.9.9 outbound through "
          "Store and RuntimeAssembler",
          "[real_core][RC-08]") {
  const auto xray = real_xray();
  TempDirectory temp;
  REQUIRE(require_fixed_xray(xray, temp.path));
  REQUIRE(check_typed_uri(
      xray, temp, "shadowsocks",
      "ss://YWVzLTI1Ni1nY206cmMwOC1zZWNyZXQ@127.0.0.1:443#rc08-shadowsocks",
      parse_shadowsocks));
}

TEST_CASE("RC-08 Hysteria2 URI becomes an Xray v26.9.9 outbound through Store "
          "and RuntimeAssembler",
          "[real_core][RC-08]") {
  const auto xray = real_xray();
  TempDirectory temp;
  REQUIRE(require_fixed_xray(xray, temp.path));
  REQUIRE(check_typed_uri(
      xray, temp, "hysteria",
      "hy2://rc08-secret@127.0.0.1:443?sni=localhost#rc08-hysteria2",
      parse_hysteria2));
}

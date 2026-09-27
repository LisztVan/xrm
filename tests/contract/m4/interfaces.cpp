// XRM: Checks public interfaces at compile time.

#include "contract/dtos.hpp"
#include "contract/methods.hpp"
#include "profile/forms.hpp"
#include "xray/codecs/codec.hpp"
#include "xray/installer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(xrm::StreamNode{}.protocol),
                             xrm::ProtocolSettings>);
static_assert(std::is_same_v<decltype(xrm::parse_vmess_uri(std::string_view{})),
                             xrm::Result<xrm::DecodedNode>>);
static_assert(
    std::is_same_v<decltype(xrm::parse_trojan_uri(std::string_view{})),
                   xrm::Result<xrm::DecodedNode>>);
static_assert(
    std::is_same_v<decltype(xrm::parse_shadowsocks_uri(std::string_view{})),
                   xrm::Result<xrm::DecodedNode>>);
static_assert(
    std::is_same_v<decltype(xrm::parse_hysteria2_uri(std::string_view{})),
                   xrm::Result<xrm::DecodedNode>>);
static_assert(std::is_same_v<decltype(xrm::edit_node_transport(
                                 std::declval<const xrm::Node &>(),
                                 std::declval<xrm::Transport>())),
                             xrm::Result<xrm::Node>>);
static_assert(std::is_same_v<decltype(xrm::edit_node_security(
                                 std::declval<const xrm::Node &>(),
                                 std::declval<xrm::Security>())),
                             xrm::Result<xrm::Node>>);
static_assert(
    std::is_same_v<
        decltype(std::declval<xrm::CoreInstaller &>().releases(
            false,
            std::declval<xrm::Completion<std::vector<xrm::CoreRelease>>>())),
        xrm::Result<xrm::Id>>);
static_assert(
    std::is_same_v<decltype(std::declval<xrm::CoreInstaller &>().install(
                       std::declval<xrm::InstallRequest>(),
                       std::declval<xrm::Completion<xrm::CoreInstall>>())),
                   xrm::Result<xrm::Id>>);

TEST_CASE(
    "M4 protocol settings preserve a tagged typed model through Node JSON") {
  xrm::Node node;
  node.id = "node-vmess";
  node.original_name = "VMess";
  xrm::StreamNode stream;
  stream.endpoint = {"2001:db8::1", 443};
  stream.protocol = xrm::VmessSettings{"uuid", "auto", 0};
  stream.transport = xrm::WebSocketTransport{"/ws", {{"Host", "edge.example"}}};
  stream.security = xrm::TlsSecurity{
      "edge.example", "chrome", {"h2"}, xrm::Json{{"allowInsecure", false}}};
  node.settings = stream;

  const auto encoded = xrm::Json(node);
  REQUIRE(encoded.at("settings").at("protocol").at("kind") == "vmess");
  REQUIRE(encoded.at("settings").at("transport").at("kind") == "websocket");
  const auto decoded = encoded.get<xrm::Node>();
  const auto &roundtrip = std::get<xrm::StreamNode>(decoded.settings);
  REQUIRE(std::get<xrm::VmessSettings>(roundtrip.protocol).security == "auto");
  REQUIRE(std::get<xrm::WebSocketTransport>(roundtrip.transport)
              .headers.at("Host") == "edge.example");
  REQUIRE(std::get<xrm::TlsSecurity>(roundtrip.security)
              .options.at("allowInsecure") == false);
}

TEST_CASE("M4 every protocol variant round-trips its credentials without "
          "changing kind") {
  const std::vector<xrm::ProtocolSettings> protocols{
      xrm::VlessSettings{"id-vless", "none", "vision", 7},
      xrm::VmessSettings{"id-vmess", "chacha20-poly1305", 3},
      xrm::TrojanSettings{"trojan-secret"},
      xrm::ShadowsocksSettings{"aes-128-gcm", "ss-secret"},
      xrm::Hysteria2Settings{"hy-secret"}};
  const std::vector<std::string> kinds{"vless", "vmess", "trojan",
                                       "shadowsocks", "hysteria2"};
  for (std::size_t i = 0; i < protocols.size(); ++i) {
    xrm::Node node;
    node.id = "typed-" + kinds[i];
    node.settings = xrm::StreamNode{{"node.example", 443}, protocols[i]};
    const auto encoded = xrm::Json(node);
    REQUIRE(encoded.at("settings").at("protocol").at("kind") == kinds[i]);
    const auto decoded = encoded.get<xrm::Node>();
    REQUIRE(xrm::Json(decoded).at("settings") == encoded.at("settings"));
  }
}

TEST_CASE("M4 release metadata and install request have stable JSON DTOs") {
  const xrm::CoreRelease release{
      "v26.9.9",
      1790000000000LL,
      false,
      {{"Xray-linux-64.zip", "https://releases.example/Xray-linux-64.zip",
        "linux", "amd64", "0123456789abcdef"}}};
  const auto encoded = xrm::Json(release);
  REQUIRE(encoded.at("tag") == "v26.9.9");
  REQUIRE(encoded.at("prerelease") == false);
  REQUIRE(encoded.at("assets").at(0).at("architecture") == "amd64");
  REQUIRE(encoded.get<xrm::CoreRelease>().assets.at(0).sha256 ==
          "0123456789abcdef");

  const auto request =
      xrm::Json(xrm::InstallRequest{release, "Xray-linux-64.zip", false});
  REQUIRE(request.at("asset_name") == "Xray-linux-64.zip");
  REQUIRE(request.at("allow_unverified_checksum") == false);
  REQUIRE(xrm::methods::core_releases == "core.releases");
  REQUIRE(xrm::methods::core_install == "core.install");
}

TEST_CASE("M4 core use RPC binds an explicit Profile and registered core") {
  const xrm::CoreUseRequestDto request{"profile-local", "core-managed"};
  const auto encoded = xrm::Json(request);
  CHECK(encoded == xrm::Json{{"profile_id", "profile-local"},
                             {"core_id", "core-managed"}});
  const auto decoded = encoded.get<xrm::CoreUseRequestDto>();
  CHECK(decoded.profile_id == request.profile_id);
  CHECK(decoded.core_id == request.core_id);
  CHECK(xrm::methods::core_use == "core.use");
}

TEST_CASE("M4 typed node form RPC keeps each native subobject distinct") {
  const xrm::NodeFormEditDto request{
      "node-id", "security",
      xrm::Json{{"kind", "tls"},
                {"server_name", "edge.example"},
                {"fingerprint", nullptr},
                {"alpn", xrm::Json::array()},
                {"options", xrm::Json::object()}}};
  const auto encoded = xrm::Json(request);
  CHECK(encoded.at("field") == "security");
  CHECK(encoded.at("value").at("kind") == "tls");
  CHECK(encoded.get<xrm::NodeFormEditDto>().value == request.value);
  CHECK(xrm::methods::node_form_edit == "node.form_edit");
}

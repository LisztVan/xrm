// XRM: Tests trojan in the domain and editor behavior suite.

#include "xray/codecs/codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

xrm::Json load_fixture(std::string_view name) {
#ifndef XRM_CODEC_FIXTURE_ROOT
#define XRM_CODEC_FIXTURE_ROOT "tests/fixtures/codecs"
#endif
  std::ifstream input(std::string(XRM_CODEC_FIXTURE_ROOT) + "/trojan/" +
                      std::string(name));
  if (!input)
    throw std::runtime_error("cannot open Trojan codec fixture " +
                             std::string(name));
  xrm::Json fixture;
  input >> fixture;
  return fixture;
}

} // namespace

TEST_CASE("CODEC-04 Trojan URI preserves credentials, IPv6, TLS, transport and "
          "name") {
  const auto fixture = load_fixture("tls_ws_ipv6.json");
  const auto parsed =
      xrm::parse_trojan_uri(fixture.at("uri").get<std::string>());
  REQUIRE(parsed.has_value());
  CHECK(parsed->node.original_name == fixture.at("name").get<std::string>());
  const auto &stream = std::get<xrm::StreamNode>(parsed->node.settings);
  CHECK(stream.endpoint.address == "2001:db8::4");
  CHECK(stream.endpoint.port == 443);
  REQUIRE(std::holds_alternative<xrm::TrojanSettings>(stream.protocol));
  CHECK(std::get<xrm::TrojanSettings>(stream.protocol).password ==
        "pa:ss@word");
  REQUIRE(std::holds_alternative<xrm::WebSocketTransport>(stream.transport));
  const auto &ws = std::get<xrm::WebSocketTransport>(stream.transport);
  CHECK(ws.path == "/trojan");
  CHECK(ws.headers.at("Host") == "cdn.example");
  REQUIRE(std::holds_alternative<xrm::TlsSecurity>(stream.security));
  const auto &tls = std::get<xrm::TlsSecurity>(stream.security);
  CHECK(tls.server_name == "front.example");
  CHECK(tls.fingerprint == "chrome");
  CHECK(tls.alpn == std::vector<std::string>{"h2", "http/1.1"});
  CHECK(fixture.at("expected_outbound").at("protocol") == "trojan");
}

TEST_CASE("CODEC-04 Trojan supports approved transport forms and rejects "
          "Trojan flow") {
  for (const auto *type : {"raw", "grpc", "httpupgrade", "xhttp"}) {
    const std::string uri =
        std::string("trojan://secret@edge.example:443?type=") + type;
    const auto parsed = xrm::parse_trojan_uri(uri);
    REQUIRE(parsed.has_value());
    const auto &stream = std::get<xrm::StreamNode>(parsed->node.settings);
    if (std::string_view(type) == "raw")
      CHECK(std::holds_alternative<xrm::RawTransport>(stream.transport));
    if (std::string_view(type) == "grpc")
      CHECK(std::holds_alternative<xrm::GrpcTransport>(stream.transport));
    if (std::string_view(type) == "httpupgrade")
      CHECK(
          std::holds_alternative<xrm::HttpUpgradeTransport>(stream.transport));
    if (std::string_view(type) == "xhttp")
      CHECK(std::holds_alternative<xrm::XHttpTransport>(stream.transport));
  }
  const auto flow = xrm::parse_trojan_uri(
      "trojan://secret@edge.example:443?flow=xtls-rprx-vision");
  REQUIRE_FALSE(flow.has_value());
  CHECK(flow.error().code == xrm::ErrorCode::unsupported);
  CHECK(flow.error().subject == "flow");
}

TEST_CASE("CODEC-04 Trojan rejects unknown connection parameters and malformed "
          "authorities") {
  for (const auto *uri :
       {"trojan://secret@edge.example:443?plugin=obfs",
        "trojan://secret@edge.example:443?security=tls&allowInsecure=1",
        "trojan://"
        "secret@edge.example:443?type=ws&serviceName=wrong-transport"}) {
    const auto result = xrm::parse_trojan_uri(uri);
    REQUIRE_FALSE(result.has_value());
  }
  const auto unknown =
      xrm::parse_trojan_uri("trojan://secret@edge.example:443?headerType=http");
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().code == xrm::ErrorCode::unsupported);
  CHECK(unknown.error().subject == "headerType");

  for (const auto *uri :
       {"trojan://secret@edge.example:0", "trojan://secret@2001:db8::4:443",
        "trojan://@edge.example:443"}) {
    const auto result = xrm::parse_trojan_uri(uri);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == xrm::ErrorCode::invalid_argument);
  }
  const auto security = xrm::parse_trojan_uri(
      "trojan://secret@edge.example:443?security=unknown");
  REQUIRE_FALSE(security.has_value());
  CHECK(security.error().code == xrm::ErrorCode::unsupported);
}

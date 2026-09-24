#include "xray/codecs/codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

xrm::Json load_fixture(std::string_view name) {
#ifndef XRM_CODEC_FIXTURE_ROOT
#define XRM_CODEC_FIXTURE_ROOT "tests/fixtures/codecs"
#endif
  std::ifstream input(std::string(XRM_CODEC_FIXTURE_ROOT) + "/vmess/" + std::string(name));
  if (!input) throw std::runtime_error("cannot open VMess fixture " + std::string(name));
  xrm::Json fixture;
  input >> fixture;
  return fixture;
}

void check_fixture(std::string_view name) {
  const auto fixture = load_fixture(name);
  const auto decoded = xrm::parse_vmess_uri(fixture.at("uri").get<std::string>());
  REQUIRE(decoded.has_value());
  CHECK(decoded->node.original_name == fixture.at("name").get<std::string>());
  REQUIRE(std::holds_alternative<xrm::StreamNode>(decoded->node.settings));
  const auto& stream = std::get<xrm::StreamNode>(decoded->node.settings);
  const auto& expected = fixture.at("outbound");
  const auto& expected_server = expected.at("settings").at("vnext").at(0);
  const auto& expected_user = expected_server.at("users").at(0);
  CHECK(stream.endpoint.address == expected_server.at("address").get<std::string>());
  CHECK(stream.endpoint.port == expected_server.at("port").get<std::uint16_t>());
  REQUIRE(std::holds_alternative<xrm::VmessSettings>(stream.protocol));
  const auto& protocol = std::get<xrm::VmessSettings>(stream.protocol);
  CHECK(protocol.id == expected_user.at("id").get<std::string>());
  CHECK(protocol.security == expected_user.at("security").get<std::string>());
  CHECK(stream.security.index() == (expected.at("streamSettings").at("security") == "tls" ? 1U : 0U));
  if (expected.at("streamSettings").at("security") == "tls") {
    const auto& tls = std::get<xrm::TlsSecurity>(stream.security);
    const auto& expected_tls = expected.at("streamSettings").at("tlsSettings");
    CHECK(tls.server_name == expected_tls.at("serverName").get<std::string>());
    CHECK(tls.fingerprint == expected_tls.at("fingerprint").get<std::string>());
    CHECK(tls.alpn == expected_tls.at("alpn").get<std::vector<std::string>>());
  }
  const auto& stream_fixture = expected.at("streamSettings");
  if (stream_fixture.at("network") == "websocket") {
    REQUIRE(std::holds_alternative<xrm::WebSocketTransport>(stream.transport));
    const auto& ws = std::get<xrm::WebSocketTransport>(stream.transport);
    CHECK(ws.path == stream_fixture.at("wsSettings").at("path").get<std::string>());
    CHECK(ws.headers.at("Host") == stream_fixture.at("wsSettings").at("headers").at("Host").get<std::string>());
  } else {
    CHECK(std::holds_alternative<xrm::RawTransport>(stream.transport));
  }
}

}  // namespace

TEST_CASE("CODEC-03 VMess share formats map to native Xray outbound fields") {
  check_fixture("ws_tls.json");
  check_fixture("tcp.json");
}

TEST_CASE("CODEC-03 VMess base64 accepts URL-safe alphabet and omitted padding") {
  const auto parsed = xrm::parse_vmess_uri(
      "vmess://eyJ2IjoiMiIsInBzIjoi8J-nqiIsImFkZCI6ImV4YW1wbGUub3JnIiwicG9ydCI6IjQ0MyIsImlkIjoiMTExMTExMTEtMTExMS00MTExLTgxMTEtMTExMTExMTExMTExIiwiYWlkIjoiMCIsInNjeSI6ImF1dG8iLCJuZXQiOiJ0Y3AiLCJ0bHMiOiIifQ");
  REQUIRE(parsed.has_value());
  CHECK(parsed->node.original_name == "🧪");
  REQUIRE(std::holds_alternative<xrm::StreamNode>(parsed->node.settings));
  CHECK(std::get<xrm::StreamNode>(parsed->node.settings).endpoint.address == "example.org");
}

TEST_CASE("CODEC-03 VMess rejects altered legacy semantics and unknown fields") {
  const auto aid = xrm::parse_vmess_uri(
      "vmess://eyJ2IjoiMiIsInBzIjoiYSIsImFkZCI6ImV4YW1wbGUub3JnIiwicG9ydCI6IjQ0MyIsImlkIjoiMTExMTExMTEtMTExMS00MTExLTgxMTEtMTExMTExMTExMTExIiwiYWlkIjoiMSIsInNjeSI6ImF1dG8iLCJuZXQiOiJ0Y3AiLCJ0bHMiOiIifQ==");
  REQUIRE_FALSE(aid.has_value());
  CHECK(aid.error().code == xrm::ErrorCode::unsupported);
  CHECK(aid.error().subject == "aid");

  const auto unknown = xrm::parse_vmess_uri(
      "vmess://eyJ2IjoiMiIsInBzIjoiYSIsImFkZCI6ImV4YW1wbGUub3JnIiwicG9ydCI6IjQ0MyIsImlkIjoiMTExMTExMTEtMTExMS00MTExLTgxMTEtMTExMTExMTExMTExIiwiYWlkIjoiMCIsInNjeSI6ImF1dG8iLCJuZXQiOiJ0Y3AiLCJ0bHMiOiIiLCJhbHRUeXBlIjoid2Vic29ja2V0In0=");
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().code == xrm::ErrorCode::unsupported);
  CHECK(unknown.error().subject == "altType");
}

TEST_CASE("CODEC-03 VMess malformed JSON and invalid values are rejected") {
  for (const auto uri : {"vmess://%%%", "vmess://e30=", "vmess://eyJhZGQiOiJhIiwicG9ydCI6MCwiaWQiOiJpZCIsImFpZCI6MCwic2N5IjoiYXV0byIsIm5ldCI6InRjcCJ9"}) {
    const auto parsed = xrm::parse_vmess_uri(uri);
    CHECK_FALSE(parsed.has_value());
  }
}

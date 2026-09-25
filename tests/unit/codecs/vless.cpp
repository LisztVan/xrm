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
  std::ifstream input(std::string(XRM_CODEC_FIXTURE_ROOT) + "/vless/" + std::string(name));
  if (!input) throw std::runtime_error("cannot open codec fixture " + std::string(name));
  xrm::Json fixture;
  input >> fixture;
  return fixture;
}

void check_fixture(std::string_view name) {
  const auto fixture = load_fixture(name);
  const auto decoded = xrm::parse_vless_uri(fixture.at("uri").get<std::string>());
  REQUIRE(decoded.has_value());
  CHECK(decoded->node.original_name == fixture.at("name").get<std::string>());
  const auto actual = xrm::write_outbound(decoded->node, "managed-node");
  REQUIRE(actual.has_value());
  CHECK(*actual == fixture.at("outbound"));
}

}  // namespace

TEST_CASE("CODEC-01 VLESS URI maps typed streams into native Xray outbound fields") {
  check_fixture("raw.json");
  check_fixture("tls.json");
  check_fixture("reality_vision.json");
  check_fixture("xhttp_extra.json");
}

TEST_CASE("CODEC-02 URI components are decoded once and malformed input is located") {
  const auto ipv6 = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@[2001:db8::1]:443?"
      "type=ws&security=tls&path=%2Fa%2Bb#hello%20%E4%B8%96%E7%95%8C");
  REQUIRE(ipv6.has_value());
  CHECK(ipv6->node.original_name == "hello 世界");
  const auto& stream = std::get<xrm::StreamNode>(ipv6->node.settings);
  CHECK(stream.endpoint.address == "2001:db8::1");
  CHECK(stream.endpoint.port == 443);
  CHECK(std::get<xrm::WebSocketTransport>(stream.transport).path == "/a+b");

  const auto reality = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?"
      "security=reality&pbk=YWJjZA&sid=01#no-padding");
  REQUIRE(reality.has_value());
  CHECK(std::get<xrm::RealitySecurity>(std::get<xrm::StreamNode>(reality->node.settings).security)
            .password == "YWJjZA");

  for (const auto* uri : {
           "vless://00000000-0000-4000-8000-000000000001@example.org:0",
           "vless://00000000-0000-4000-8000-000000000001@example.org:notaport",
           "vless://00000000-0000-4000-8000-000000000001@example.org:443?path=%2",
           "vless://00000000-0000-4000-8000-000000000001@example.org:443\r\n"}) {
    const auto result = xrm::parse_vless_uri(uri);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == xrm::ErrorCode::invalid_argument);
  }
}

TEST_CASE("CODEC-02 duplicate parameters produce a notice and ambiguous values fail") {
  auto duplicate = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?"
      "security=tls&security=tls");
  REQUIRE(duplicate.has_value());
  REQUIRE(duplicate->notices.size() == 1);
  CHECK(duplicate->notices.front().code == "duplicate_parameter");

  auto conflicting = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?"
      "security=tls&security=reality");
  REQUIRE_FALSE(conflicting.has_value());
  CHECK(conflicting.error().code == xrm::ErrorCode::invalid_argument);
  CHECK(conflicting.error().subject == "security");
}

TEST_CASE("CODEC-08 unknown connection semantics are not silently discarded") {
  const auto result = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?"
      "type=tcp&security=none&headerType=http");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == xrm::ErrorCode::unsupported);
  CHECK(result.error().subject == "headerType");

  const auto misplaced = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?"
      "type=tcp&security=none&path=%2Fmust-not-disappear");
  REQUIRE_FALSE(misplaced.has_value());
  CHECK(misplaced.error().subject == "path");
}

TEST_CASE("VLESS imports tolerate known neutral defaults across RAW, WS and XHTTP") {
  const auto id = "00000000-0000-4000-8000-000000000001";
  const auto raw = xrm::parse_vless_uri(std::string("vless://") + id +
      "@raw.invalid:443?type=tcp&security=reality&pbk=YWJjZA&sid=01&insecure=true&headerType=none&quicSecurity=none#raw");
  REQUIRE(raw.has_value());
  CHECK(std::holds_alternative<xrm::RawTransport>(std::get<xrm::StreamNode>(raw->node.settings).transport));

  const auto reality_tls_false = xrm::parse_vless_uri(std::string("vless://") + id +
      "@reality.invalid:443?type=raw&security=reality&pbk=YWJjZA&sid=01&insecure=false&headerType=none&quicSecurity=none#reality");
  REQUIRE(reality_tls_false.has_value());

  const auto ws = xrm::parse_vless_uri(std::string("vless://") + id +
      "@ws.invalid:443?type=ws&security=tls&insecure=false&headerType=none&quicSecurity=none&ech=YWJjKysvPQ%3D%3D&host=front.invalid&path=%2Fsocket#ws");
  REQUIRE(ws.has_value());
  const auto ws_outbound = xrm::write_outbound(ws->node, "ws");
  REQUIRE(ws_outbound.has_value());
  CHECK(ws_outbound->at("streamSettings").at("network") == "websocket");
  CHECK(ws_outbound->at("streamSettings").at("wsSettings").at("path") == "/socket");
  CHECK(ws_outbound->at("streamSettings").at("tlsSettings").at("echConfigList") == "YWJjKysvPQ==");
  CHECK_FALSE(ws_outbound->at("streamSettings").at("tlsSettings").value("allowInsecure", false));
  const auto encoded_node = xrm::Json(ws->node);
  const auto restored_node = encoded_node.get<xrm::Node>();
  const auto restored_outbound = xrm::write_outbound(restored_node, "ws-roundtrip");
  REQUIRE(restored_outbound.has_value());
  CHECK(restored_outbound->at("streamSettings").at("tlsSettings").at("echConfigList") == "YWJjKysvPQ==");

  const auto xhttp = xrm::parse_vless_uri(std::string("vless://") + id +
      "@xhttp.invalid:443?type=xhttp&security=tls&insecure=false&headerType=none&quicSecurity=none&ech=YWJjKysvPQ%3D%3D&host=origin.invalid&path=%2Fedge#xhttp");
  REQUIRE(xhttp.has_value());
  const auto xhttp_outbound = xrm::write_outbound(xhttp->node, "xhttp");
  REQUIRE(xhttp_outbound.has_value());
  CHECK(xhttp_outbound->at("streamSettings").at("network") == "xhttp");
  CHECK(xhttp_outbound->at("streamSettings").at("xhttpSettings").at("host") == "origin.invalid");
  CHECK(xhttp_outbound->at("streamSettings").at("tlsSettings").at("echConfigList") == "YWJjKysvPQ==");

  // The reported eight-item shape is 4 TCP/REALITY, 3 WS/TLS, and 1 XHTTP/TLS.
  // Endpoint, UUID and ECH credentials are synthetic because the complete source URIs were not supplied.
  for (const auto* insecure_value : {"false", "false", "false", "true"}) {
    const auto uri = std::string("vless://") + id +
        "@tcp.invalid:443?type=tcp&security=reality&pbk=YWJjZA&sid=01&insecure=" + insecure_value +
        "&headerType=none&quicSecurity=none";
    CHECK(xrm::parse_vless_uri(uri).has_value());
  }
  for (const auto* path : {"%2Fone", "%2Ftwo", "%2Fthree"}) {
    const auto uri = std::string("vless://") + id +
        "@ws.invalid:443?type=ws&security=tls&insecure=false&headerType=none&quicSecurity=none&ech=YWJjKysvPQ%3D%3D&host=front.invalid&path=" + path;
    const auto decoded = xrm::parse_vless_uri(uri);
    REQUIRE(decoded.has_value());
    const auto stream = xrm::write_outbound(decoded->node, "ws-matrix");
    REQUIRE(stream.has_value());
    CHECK(stream->at("streamSettings").at("tlsSettings").at("echConfigList") == "YWJjKysvPQ==");
  }
}

TEST_CASE("VLESS TLS insecure=true is not silently converted to trust bypass") {
  const auto result = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?type=ws&security=tls&insecure=true");
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == xrm::ErrorCode::unsupported);
  CHECK(result.error().subject == "insecure");
}

TEST_CASE("VLESS gRPC mode is preserved as native multiMode") {
  const auto parsed = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?"
      "type=grpc&security=tls&mode=multi&serviceName=edge&authority=front.example#grpc");
  REQUIRE(parsed.has_value());
  const auto outbound = xrm::write_outbound(parsed->node, "grpc-multi");
  REQUIRE(outbound.has_value());
  CHECK(outbound->at("streamSettings").at("grpcSettings").at("multiMode") == true);
  const auto roundtrip = xrm::Json(parsed->node).get<xrm::Node>();
  const auto again = xrm::write_outbound(roundtrip, "grpc-multi-roundtrip");
  REQUIRE(again.has_value());
  CHECK(again->at("streamSettings").at("grpcSettings").at("multiMode") == true);

  constexpr auto id = "00000000-0000-4000-8000-000000000001";
  for (const auto mode : {"multi", "gun"}) {
    const auto tcp = xrm::parse_vless_uri(std::string("vless://") + id +
        "@tcp.invalid:443?type=tcp&mode=" + mode);
    REQUIRE(tcp.has_value());
    CHECK(std::holds_alternative<xrm::RawTransport>(std::get<xrm::StreamNode>(tcp->node.settings).transport));
    const auto tcp_outbound = xrm::write_outbound(tcp->node, "tcp-mode-residue");
    REQUIRE(tcp_outbound.has_value());
    const auto tcp_stream = tcp_outbound->at("streamSettings");
    CHECK(tcp_stream.at("network") == "raw");
    CHECK_FALSE(tcp_stream.dump().find("multiMode") != std::string::npos);
    CHECK_FALSE(tcp_stream.dump().find("xhttpSettings") != std::string::npos);
    CHECK_FALSE(tcp_stream.dump().find("grpcSettings") != std::string::npos);

    const auto raw = xrm::parse_vless_uri(std::string("vless://") + id +
        "@raw.invalid:443?type=raw&mode=" + mode);
    REQUIRE(raw.has_value());
    const auto raw_outbound = xrm::write_outbound(raw->node, "raw-mode-residue");
    REQUIRE(raw_outbound.has_value());
    CHECK(raw_outbound->at("streamSettings").at("network") == "raw");
    CHECK_FALSE(raw_outbound->at("streamSettings").dump().find("multiMode") != std::string::npos);
  }

  // mode is not a globally ignored parameter: WS has no matching mode contract.
  const auto ws_mismatch = xrm::parse_vless_uri(std::string("vless://") + id +
      "@ws.invalid:443?type=ws&mode=multi");
  REQUIRE_FALSE(ws_mismatch.has_value());
  CHECK(ws_mismatch.error().subject == "mode");

  const auto xhttp = xrm::parse_vless_uri(std::string("vless://") + id +
      "@xhttp.invalid:443?type=xhttp&mode=stream-up");
  REQUIRE(xhttp.has_value());
  const auto xhttp_outbound = xrm::write_outbound(xhttp->node, "xhttp-mode");
  REQUIRE(xhttp_outbound.has_value());
  CHECK(xhttp_outbound->at("streamSettings").at("xhttpSettings").at("mode") == "stream-up");

  const auto unsupported = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?type=grpc&mode=guna");
  REQUIRE_FALSE(unsupported.has_value());
  CHECK(unsupported.error().code == xrm::ErrorCode::unsupported);

  const auto raw_unsupported = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?type=tcp&mode=packet-up");
  REQUIRE_FALSE(raw_unsupported.has_value());
  CHECK(raw_unsupported.error().code == xrm::ErrorCode::unsupported);
}

TEST_CASE("VLESS security aliases and non-neutral legacy fields keep explicit semantics") {
  const auto alias = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?security=tls&sni=front.example&servername=front.example&ech=");
  REQUIRE(alias.has_value());
  const auto outbound = xrm::write_outbound(alias->node, "alias");
  REQUIRE(outbound.has_value());
  CHECK(outbound->at("streamSettings").at("tlsSettings").at("serverName") == "front.example");
  CHECK(outbound->at("streamSettings").at("tlsSettings").at("echConfigList") == "");

  const auto conflict = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?security=tls&sni=one&servername=two");
  REQUIRE_FALSE(conflict.has_value());
  CHECK(conflict.error().code == xrm::ErrorCode::invalid_argument);
  CHECK(conflict.error().subject == "servername");

  const auto unsupported_quic_security = xrm::parse_vless_uri(
      "vless://00000000-0000-4000-8000-000000000001@example.org:443?type=ws&quicSecurity=aes-128-gcm");
  REQUIRE_FALSE(unsupported_quic_security.has_value());
  CHECK(unsupported_quic_security.error().code == xrm::ErrorCode::unsupported);
  CHECK(unsupported_quic_security.error().subject == "quicSecurity");
}

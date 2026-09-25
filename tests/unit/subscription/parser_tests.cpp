#include "subscription/parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {
constexpr const char* vless =
    "vless://00000000-0000-4000-8000-000000000001@node.example:443?type=ws&security=tls&path=%2Fa%2Bb#First%20%E4%B8%96%E7%95%8C";
}

TEST_CASE("SUB-01 recognizes URI lists and one standard or URL-safe Base64 wrapper") {
  const auto plain = xrm::parse_subscription(std::string("\xEF\xBB\xBF  ") + vless + "\r\n\n" + vless);
  REQUIRE(plain.has_value());
  REQUIRE(plain->nodes.size() == 2);
  CHECK(plain->nodes[0].name == "First 世界");
  CHECK(plain->nodes[1].name == "First 世界");
  CHECK(plain->structurally_complete);

  const auto standard = xrm::parse_subscription("dmxlc3M6Ly8wMDAwMDAwMC0wMDAwLTQwMDAtODAwMC0wMDAwMDAwMDAwMDFAbm9kZS5leGFtcGxlOjQ0Mw");
  REQUIRE(standard.has_value());
  REQUIRE(standard->nodes.size() == 1);

  const auto urlsafe = xrm::parse_subscription("dmxlc3M6Ly8wMDAwMDAwMC0wMDAwLTQwMDAtODAwMC0wMDAwMDAwMDAwMDFAWzIwMDE6ZGI4OjoxXTo0NDM_dHlwZT13cyZwYXRoPWFiYytkZWYvZ2hp");
  REQUIRE(urlsafe.has_value());
  REQUIRE(urlsafe->nodes.size() == 1);
  const auto& stream = std::get<xrm::StreamNode>(urlsafe->nodes[0].settings);
  CHECK(std::get<xrm::WebSocketTransport>(stream.transport).path == "abc+def/ghi");
}

TEST_CASE("SUB-01 separates malformed, unsupported, HTML, and deep Base64 input") {
  const auto report = xrm::parse_subscription(
      "vless://00000000-0000-4000-8000-000000000001@node.example:443\n"
      "socks://opaque\n"
      "vless://not-a-valid-node");
  REQUIRE(report.has_value());
  REQUIRE(report->nodes.size() == 1);
  REQUIRE(report->issues.size() == 2);
  CHECK(report->issues[0].kind == xrm::IssueKind::unsupported);
  CHECK(report->issues[1].kind == xrm::IssueKind::malformed);
  CHECK_FALSE(report->structurally_complete);

  const auto html = xrm::parse_subscription("\xEF\xBB\xBF<!doctype html><html><title>login</title></html>");
  REQUIRE(html.has_value());
  CHECK(html->nodes.empty());
  CHECK_FALSE(html->structurally_complete);
  REQUIRE(html->issues.size() == 1);
  CHECK(html->issues[0].kind == xrm::IssueKind::malformed);

  const auto deep = xrm::parse_subscription("ZG1...=");
  REQUIRE(deep.has_value());
  CHECK(deep->nodes.empty());
  CHECK_FALSE(deep->structurally_complete);

  const auto double_encoded =
      xrm::parse_subscription("ZG14bGMzTTZMeTh3TURBd01EQXdNQzB3TURBd0xUUXdNREF0T0RBd01DMHdNREF3TURBd01EQXdNREZBWlhoaGJYQnNaUzV2Y21jNk5EUXo=");
  REQUIRE(double_encoded.has_value());
  CHECK(double_encoded->nodes.empty());
  CHECK_FALSE(double_encoded->structurally_complete);
}

TEST_CASE("subscription malformed VLESS items retain a safe field-level diagnostic") {
  const auto report = xrm::parse_subscription(
      "vless://00000000-0000-4000-8000-000000000001@node.example:443?security=tls&insecure=maybe");
  REQUIRE(report.has_value());
  REQUIRE(report->nodes.empty());
  REQUIRE(report->issues.size() == 1);
  CHECK(report->issues.front().kind == xrm::IssueKind::malformed);
  CHECK(report->issues.front().message.find("insecure must be true, false, 1 or 0") != std::string::npos);
  CHECK(report->issues.front().message.find("maybe") == std::string::npos);
}

TEST_CASE("real VLESS TCP subscription shape tolerates an inapplicable gRPC mode parameter") {
  // The saved real subscription returns VLESS TCP/REALITY items with mode=multi.
  // mode is defined for gRPC, so it has no TCP setting to map and must not make
  // otherwise complete TCP nodes disappear from the inventory.
  const auto report = xrm::parse_subscription(
      "vless://00000000-0000-4000-8000-000000000001@node.example:443?"
      "type=tcp&security=reality&pbk=YWJjZA&sid=01&mode=multi&flow=xtls-rprx-vision"
      "&sni=front.example&fp=chrome#node");
  REQUIRE(report.has_value());
  REQUIRE(report->nodes.size() == 1);
  CHECK(report->issues.empty());
  CHECK(report->structurally_complete);
  const auto& stream = std::get<xrm::StreamNode>(report->nodes.front().settings);
  CHECK(std::holds_alternative<xrm::RawTransport>(stream.transport));
  CHECK(std::get<xrm::VlessSettings>(stream.protocol).flow == "xtls-rprx-vision");
}

TEST_CASE("CODEC-02 subscription share URI boundary keeps IPv6 and decodes URI components once") {
  const auto parsed = xrm::parse_share_uri(
      "vless://00000000-0000-4000-8000-000000000001@[2001:db8::1]:443?"
      "type=ws&security=tls&path=%2Fa%2Bb#hello%20%E4%B8%96%E7%95%8C");
  REQUIRE(parsed.has_value());
  CHECK(parsed->name == "hello 世界");
  const auto& stream = std::get<xrm::StreamNode>(parsed->settings);
  CHECK(stream.endpoint.address == "2001:db8::1");
  CHECK(std::get<xrm::WebSocketTransport>(stream.transport).path == "/a+b");
}

TEST_CASE("CODEC-07 subscription accepts complete native outbound objects and collections only") {
  const auto single = xrm::parse_subscription(
      R"({"protocol":"freedom","settings":{},"tag":"direct-like"})");
  REQUIRE(single.has_value());
  REQUIRE(single->nodes.size() == 1);
  CHECK(single->nodes[0].name == "direct-like");
  CHECK(std::holds_alternative<xrm::RawOutbound>(single->nodes[0].settings));

  const auto collection = xrm::parse_subscription(
      R"({"outbounds":[{"protocol":"blackhole","settings":{"response":{"type":"http"}},"tag":"blocked"}]})");
  REQUIRE(collection.has_value());
  REQUIRE(collection->nodes.size() == 1);
  CHECK(collection->nodes[0].name == "blocked");

  const auto not_outbound = xrm::parse_subscription(
      R"({"routing":{"rules":[]},"dns":{"servers":[]}})");
  REQUIRE(not_outbound.has_value());
  CHECK(not_outbound->nodes.empty());
  CHECK_FALSE(not_outbound->structurally_complete);

  const auto full_config = xrm::parse_subscription(
      R"({"outbounds":[{"protocol":"freedom","settings":{}}],"routing":{"rules":[]}})");
  REQUIRE(full_config.has_value());
  CHECK(full_config->nodes.empty());
  CHECK_FALSE(full_config->structurally_complete);
}

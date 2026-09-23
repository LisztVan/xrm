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

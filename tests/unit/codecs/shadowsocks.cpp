/// @file
/// @brief Checks supported Shadowsocks URI credential and encoding forms.

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
  std::ifstream input(std::string(XRM_CODEC_FIXTURE_ROOT) + "/shadowsocks/" +
                      std::string(name));
  if (!input)
    throw std::runtime_error("cannot open Shadowsocks codec fixture " +
                             std::string(name));
  xrm::Json fixture;
  input >> fixture;
  return fixture;
}

void check_credentials(const xrm::DecodedNode &parsed, std::string_view method,
                       std::string_view password) {
  const auto &stream = std::get<xrm::StreamNode>(parsed.node.settings);
  REQUIRE(std::holds_alternative<xrm::ShadowsocksSettings>(stream.protocol));
  const auto &settings = std::get<xrm::ShadowsocksSettings>(stream.protocol);
  CHECK(settings.method == method);
  CHECK(settings.password == password);
}

} // namespace

TEST_CASE("CODEC-05 SIP002 URL-safe Base64 userinfo preserves method, password "
          "and endpoint") {
  const auto fixture = load_fixture("sip002.json");
  const auto parsed =
      xrm::parse_shadowsocks_uri(fixture.at("uri").get<std::string>());
  REQUIRE(parsed.has_value());
  CHECK(parsed->node.original_name == fixture.at("name").get<std::string>());
  const auto &stream = std::get<xrm::StreamNode>(parsed->node.settings);
  CHECK(stream.endpoint.address == "ss.example");
  CHECK(stream.endpoint.port == 8388);
  check_credentials(*parsed, "aes-128-gcm", "p@ss:word");
  CHECK(fixture.at("expected_outbound").at("protocol") == "shadowsocks");
}

TEST_CASE("CODEC-05 legacy whole-authority Base64 form is decoded once") {
  const auto fixture = load_fixture("legacy.json");
  const auto parsed =
      xrm::parse_shadowsocks_uri(fixture.at("uri").get<std::string>());
  REQUIRE(parsed.has_value());
  CHECK(parsed->node.original_name == "legacy");
  const auto &stream = std::get<xrm::StreamNode>(parsed->node.settings);
  CHECK(stream.endpoint.address == "example.org");
  CHECK(stream.endpoint.port == 8443);
  check_credentials(*parsed, "aes-256-gcm", "pass");
}

TEST_CASE("CODEC-05 Shadowsocks unsupported plugins and methods never become "
          "usable nodes") {
  for (const auto *uri :
       {"ss://"
        "YWVzLTEyOC1nY206cGFzcw@ss.example:8388?plugin=obfs-local%3Bobfs%"
        "3Dhttp",
        "ss://YWVzLTEyOC1nY206cGFzcw@ss.example:8388?plugin=unsupported"}) {
    const auto result = xrm::parse_shadowsocks_uri(uri);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == xrm::ErrorCode::unsupported);
    CHECK(result.error().subject == "plugin");
  }
  const auto method = xrm::parse_shadowsocks_uri(
      "ss://bm90LWEtY2lwaGVyOnBhc3M@ss.example:8388");
  REQUIRE_FALSE(method.has_value());
  CHECK(method.error().code == xrm::ErrorCode::unsupported);
  CHECK(method.error().subject == "method");
}

TEST_CASE(
    "CODEC-05 malformed Shadowsocks encodings and endpoints fail explicitly") {
  for (const auto *uri : {"ss://%%%bad@ss.example:8388",
                          "ss://YWVzLTEyOC1nY206cGFzcw@ss.example:0",
                          "ss://YWVzLTEyOC1nY206cGFzcw@[2001:db8::1]:bad",
                          "ss://YWVzLTEyOC1nY206cGFzcw@ss.example:8388?x=1"}) {
    const auto result = xrm::parse_shadowsocks_uri(uri);
    REQUIRE_FALSE(result.has_value());
  }
}

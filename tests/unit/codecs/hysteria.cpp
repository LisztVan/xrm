/// @file
/// @brief Checks accepted Hysteria2 URI fields and explicit rejection of
/// unsupported options.

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
  std::ifstream input(std::string(XRM_CODEC_FIXTURE_ROOT) + "/hysteria/" +
                      std::string(name));
  if (!input)
    throw std::runtime_error("cannot open codec fixture " + std::string(name));
  xrm::Json fixture;
  input >> fixture;
  return fixture;
}

void check_fixture(std::string_view filename) {
  const auto fixture = load_fixture(filename);
  const auto decoded =
      xrm::parse_hysteria2_uri(fixture.at("uri").get<std::string>());
  REQUIRE(decoded.has_value());
  CHECK(decoded->node.original_name == fixture.at("name").get<std::string>());
  const auto *stream = std::get_if<xrm::StreamNode>(&decoded->node.settings);
  REQUIRE(stream != nullptr);
  CHECK(stream->endpoint.address == fixture.at("address").get<std::string>());
  CHECK(stream->endpoint.port == fixture.at("port").get<std::uint16_t>());
  const auto *protocol = std::get_if<xrm::Hysteria2Settings>(&stream->protocol);
  REQUIRE(protocol != nullptr);
  CHECK(protocol->auth == fixture.at("auth").get<std::string>());
  const auto *tls = std::get_if<xrm::TlsSecurity>(&stream->security);
  REQUIRE(tls != nullptr);
  if (fixture.contains("server_name")) {
    CHECK(tls->server_name == fixture.at("server_name").get<std::string>());
  } else {
    CHECK_FALSE(tls->server_name.has_value());
  }
  if (fixture.contains("allow_insecure")) {
    CHECK(tls->options.at("allowInsecure") == fixture.at("allow_insecure"));
  } else {
    CHECK_FALSE(tls->options.contains("allowInsecure"));
  }
  const auto *transport =
      std::get_if<xrm::HysteriaTransport>(&stream->transport);
  REQUIRE(transport != nullptr);
  CHECK(transport->options == fixture.at("transport"));
}

} // namespace

TEST_CASE("CODEC-06 Hysteria2 URI maps only the pinned Xray Hysteria fields") {
  check_fixture("ipv6_tls.json");
  check_fixture("default_port.json");
}

TEST_CASE("CODEC-06 unsupported Hysteria2 share options are explicit") {
  for (const auto &[option, subject] :
       {std::pair{"obfs=salamander", "obfs"},
        std::pair{"obfs-password=secret", "obfs-password"},
        std::pair{"pinSHA256=abcd", "pinSHA256"},
        std::pair{"upmbps=20", "upmbps"}, std::pair{"downmbps=100", "downmbps"},
        std::pair{"ech=AAE=", "ech"}}) {
    const std::string uri =
        std::string("hysteria2://auth@example.net:443?") + option;
    const auto result = xrm::parse_hysteria2_uri(uri);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == xrm::ErrorCode::unsupported);
    CHECK(result.error().subject == subject);
  }

  const auto port_hopping =
      xrm::parse_hysteria2_uri("hy2://auth@example.net:443,8443");
  REQUIRE_FALSE(port_hopping.has_value());
  CHECK(port_hopping.error().code == xrm::ErrorCode::unsupported);
  CHECK(port_hopping.error().subject == "port");
}

TEST_CASE("CODEC-06 Hysteria2 URI encoding and explicit Xray transport "
          "settings are checked") {
  const auto decoded =
      xrm::parse_hysteria2_uri("hysteria2://"
                               "pa%3Ass%40word@example.net?sni=front.example&"
                               "insecure=0&udpIdleTimeout=2#n%20ame");
  REQUIRE(decoded.has_value());
  CHECK(decoded->node.original_name == "n ame");
  const auto &stream = std::get<xrm::StreamNode>(decoded->node.settings);
  CHECK(std::get<xrm::Hysteria2Settings>(stream.protocol).auth == "pa:ss@word");
  const auto &tls = std::get<xrm::TlsSecurity>(stream.security);
  CHECK(tls.server_name == "front.example");
  CHECK_FALSE(tls.options.contains("allowInsecure"));
  CHECK(std::get<xrm::HysteriaTransport>(stream.transport).options ==
        xrm::Json{{"version", 2}, {"udpIdleTimeout", 2}});

  const auto duplicate =
      xrm::parse_hysteria2_uri("hy2://auth@example.net?insecure=1&insecure=1");
  REQUIRE(duplicate.has_value());
  REQUIRE(duplicate->notices.size() == 1);
  CHECK(duplicate->notices.front().code == "duplicate_parameter");
  CHECK(duplicate->notices.front().message == "insecure");

  for (const auto *uri :
       {"hy2://auth@example.net:0", "hy2://auth@example.net:65536",
        "hy2://auth@example.net?insecure=yes",
        "hy2://auth@example.net?udpIdleTimeout=1",
        "hy2://auth@example.net?udpIdleTimeout=601",
        "hy2://auth@example.net?udpIdleTimeout=one",
        "hy2://auth@example.net?udpIdleTimeout=2&udpIdleTimeout=3",
        "hy2://auth@example.net?sni=%2", "hy2://auth@example.net\r\n"}) {
    const auto result = xrm::parse_hysteria2_uri(uri);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == xrm::ErrorCode::invalid_argument);
  }
}

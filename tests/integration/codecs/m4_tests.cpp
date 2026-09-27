/// @file
/// @brief Exercises supported URI codecs through subscription import and native
/// runtime assembly.

#include <catch2/catch_test_macros.hpp>

#include "contract/dtos.hpp"
#include "subscription/parser.hpp"
#include "xray/codecs/codec.hpp"

#include <fstream>
#include <iterator>
#include <tuple>

namespace {

xrm::Json fixture(std::string_view relative) {
  std::ifstream input(std::filesystem::path(XRM_M4_FIXTURE_DIR) / relative);
  REQUIRE(input.good());
  return xrm::Json::parse(input);
}

xrm::Result<xrm::Json> compose(std::string_view uri) {
  auto parsed = xrm::parse_share_uri(uri);
  if (!parsed)
    return std::unexpected(parsed.error());
  xrm::Node node;
  node.id = "codec-gate";
  node.original_name = parsed->name;
  node.settings = std::move(parsed->settings);
  return xrm::write_outbound(node, "managed-node");
}

} // namespace

TEST_CASE("CODEC-08 all frozen typed URI codecs compose into pinned Xray "
          "outbound shapes") {
  for (const auto &[relative, uri_key, outbound_key] :
       {std::tuple{"vmess/ws_tls.json", "uri", "outbound"},
        std::tuple{"trojan/tls_ws_ipv6.json", "uri", "expected_outbound"},
        std::tuple{"shadowsocks/sip002.json", "uri", "expected_outbound"}}) {
    const auto sample = fixture(relative);
    const auto output = compose(sample.at(uri_key).get<std::string>());
    REQUIRE(output);
    CHECK(*output == sample.at(outbound_key));
  }

  const auto sample = fixture("hysteria/ipv6_tls.json");
  const auto output = compose(sample.at("uri").get<std::string>());
  REQUIRE(output);
  CHECK(output->at("protocol") == "hysteria");
  CHECK(output->at("settings") ==
        xrm::Json{{"version", 2}, {"address", "2001:db8::25"}, {"port", 8443}});
  CHECK(output->at("streamSettings").at("network") == "hysteria");
  CHECK(output->at("streamSettings").at("hysteriaSettings") ==
        xrm::Json{
            {"version", 2}, {"auth", "token@part"}, {"udpIdleTimeout", 90}});
  CHECK(output->at("streamSettings").at("tlsSettings").at("serverName") ==
        "front.example");
}

TEST_CASE("CODEC-08 subscription import dispatches supported M4 schemes and "
          "reports unsupported explicitly") {
  const auto vmess = fixture("vmess/tcp.json").at("uri").get<std::string>();
  const auto trojan =
      fixture("trojan/tls_ws_ipv6.json").at("uri").get<std::string>();
  const auto shadowsocks =
      fixture("shadowsocks/sip002.json").at("uri").get<std::string>();
  const auto hysteria =
      fixture("hysteria/ipv6_tls.json").at("uri").get<std::string>();
  auto report = xrm::parse_subscription(vmess + "\n" + trojan + "\n" +
                                        shadowsocks + "\n" + hysteria +
                                        "\nwireguard://node.example:51820\n");
  REQUIRE(report);
  REQUIRE(report->nodes.size() == 4);
  CHECK(report->issues.size() == 1);
  CHECK(report->issues.front().kind == xrm::IssueKind::unsupported);
  CHECK(report->structurally_complete);
  CHECK(std::holds_alternative<xrm::VmessSettings>(
      std::get<xrm::StreamNode>(report->nodes[0].settings).protocol));
  CHECK(std::holds_alternative<xrm::TrojanSettings>(
      std::get<xrm::StreamNode>(report->nodes[1].settings).protocol));
  CHECK(std::holds_alternative<xrm::ShadowsocksSettings>(
      std::get<xrm::StreamNode>(report->nodes[2].settings).protocol));
  CHECK(std::holds_alternative<xrm::Hysteria2Settings>(
      std::get<xrm::StreamNode>(report->nodes[3].settings).protocol));
}

TEST_CASE("CLI-03 protocol summaries distinguish typed URI codecs from native "
          "raw-only nodes") {
  const auto vmess = xrm::parse_share_uri(
      fixture("vmess/tcp.json").at("uri").get<std::string>());
  REQUIRE(vmess);
  xrm::Node typed{
      "node-vmess", std::nullopt, vmess->name, std::nullopt,
      {},           true,         false,       std::move(vmess->settings)};
  const auto typed_summary = xrm::summarize_node(typed);
  CHECK(typed_summary.protocol == "vmess");
  CHECK(typed_summary.support ==
        "typed-uri; Xray v26.9.9 config-verified; traffic-unverified");

  xrm::Node raw{"node-raw",
                std::nullopt,
                "raw",
                std::nullopt,
                {},
                true,
                false,
                xrm::RawOutbound{xrm::Json{{"protocol", "freedom"},
                                           {"settings", xrm::Json::object()}}}};
  const auto raw_summary = xrm::summarize_node(raw);
  CHECK(raw_summary.protocol == "freedom");
  CHECK(raw_summary.support == "raw-only");
}

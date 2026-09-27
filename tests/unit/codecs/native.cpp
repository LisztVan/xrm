// XRM: Tests native in the domain and editor behavior suite.

#include "xray/codecs/codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

xrm::Json load_native_fixture(std::string_view name) {
#ifndef XRM_CODEC_FIXTURE_ROOT
#define XRM_CODEC_FIXTURE_ROOT "tests/fixtures/codecs"
#endif
  std::ifstream input(std::string(XRM_CODEC_FIXTURE_ROOT) + "/native/" +
                      std::string(name));
  if (!input)
    throw std::runtime_error("cannot open native codec fixture " +
                             std::string(name));
  xrm::Json fixture;
  input >> fixture;
  return fixture;
}

} // namespace

TEST_CASE("CODEC-07 native outbound import preserves complete unknown nested "
          "values") {
  const auto source = load_native_fixture("complete_outbound.json");

  const auto imported =
      xrm::parse_native_outbound(source, "node-id", "Native edge");
  REQUIRE(imported.has_value());
  CHECK(imported->original_name == "Native edge");
  const auto written = xrm::write_outbound(*imported, "managed-node");
  REQUIRE(written.has_value());
  auto expected = source;
  expected["tag"] = "managed-node";
  CHECK(*written == expected);
}

TEST_CASE(
    "CODEC-07 a config collection is not mistaken for one native outbound") {
  const auto source = load_native_fixture("config_collection.json");
  const auto imported = xrm::parse_native_outbound(source, "node-id");
  REQUIRE_FALSE(imported.has_value());
  CHECK(imported.error().code == xrm::ErrorCode::invalid_argument);
}

TEST_CASE(
    "CODEC-07 standalone import rejects a dangling outbound proxy reference") {
  const xrm::Json source = {
      {"tag", "proxy"},
      {"protocol", "freedom"},
      {"settings", xrm::Json::object()},
      {"proxySettings", {{"tag", "another-managed-node"}}}};
  const auto imported = xrm::parse_native_outbound(source, "node-id");
  REQUIRE_FALSE(imported.has_value());
  CHECK(imported.error().code == xrm::ErrorCode::unsupported);
  CHECK(imported.error().subject == "proxySettings.tag");
}

TEST_CASE("CODEC-08 old top-level reverse and global transport modules are not "
          "outbounds") {
  for (const auto *source : {R"({"reverse":{"bridges":[]}})",
                             R"({"transport":{"kcpSettings":{}}})"}) {
    const auto imported =
        xrm::parse_native_outbound(xrm::Json::parse(source), "node-id");
    REQUIRE_FALSE(imported.has_value());
    CHECK(imported.error().code == xrm::ErrorCode::invalid_argument);
  }
}

TEST_CASE(
    "CODEC-07 typed security options cannot duplicate strongly typed fields") {
  xrm::Node node;
  xrm::StreamNode stream;
  stream.endpoint = {"edge.invalid", 443};
  std::get<xrm::VlessSettings>(stream.protocol).id =
      "00000000-0000-4000-8000-000000000001";
  xrm::TlsSecurity tls;
  tls.server_name = "front.invalid";
  tls.options["serverName"] = "conflicting.invalid";
  stream.security = tls;
  node.settings = stream;

  const auto written = xrm::write_outbound(node, "managed-node");
  REQUIRE_FALSE(written.has_value());
  CHECK(written.error().code == xrm::ErrorCode::invalid_argument);
  CHECK(written.error().subject == "security.options.serverName");
}

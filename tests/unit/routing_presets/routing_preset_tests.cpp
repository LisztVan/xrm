#include "geodata/resource_validation.hpp"
#include "profile/routing_presets.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace xrm;

namespace {

ModuleDocument existing_routing() {
  return {"routing", "04_routing.json",
          R"({"routing":{"domainStrategy":"IPIfNonMatch","vendor":{"keep":7},"balancers":[{"tag":"static","selector":["some-tag"]}],"rules":[{"type":"field","inboundTag":["api"],"outboundTag":"api"},{"type":"field","domain":["domain:old.example"],"outboundTag":"direct"}]}})"};
}

RoutingPresetRequest request(RoutingPreset preset) {
  RoutingPresetRequest result;
  result.preset = preset;
  result.routing = existing_routing();
  result.proxy = RoutingProxyTarget{"balancerTag", "chosen-group"};
  result.outbound_tags = {"direct", "block", "api"};
  result.balancer_tags = {"chosen-group", "static"};
  result.protected_inbound_tags = {"api"};
  result.geoip_labels = {"private", "cn", "facebook", "fastly", "google", "netflix", "telegram", "twitter"};
  result.geosite_labels = {"private", "cn", "google", "gfw", "greatfire"};
  return result;
}

}  // namespace

TEST_CASE("P5 preset plan replaces rules but preserves API route, balancers and unknown fields") {
  auto planned = plan_routing_preset(request(RoutingPreset::bypass_mainland));
  REQUIRE(planned);
  REQUIRE(planned->changed);
  CHECK(planned->edit.base.expected_text == existing_routing().source_text);
  const Json after = Json::parse(planned->edit.text);
  CHECK(after["routing"]["domainStrategy"] == "IPIfNonMatch");
  CHECK(after["routing"]["vendor"]["keep"] == 7);
  CHECK(after["routing"]["balancers"].size() == 1);
  CHECK(after["routing"]["rules"][0]["inboundTag"] == Json::array({"api"}));
  CHECK(after["routing"]["rules"][1]["network"] == "udp");
  CHECK(after["routing"]["rules"].back()["balancerTag"] == "chosen-group");
  CHECK(planned->required_geoip_labels.contains("cn"));
  CHECK(planned->required_geosite_labels.contains("google"));

  auto repeated = request(RoutingPreset::bypass_mainland);
  repeated.routing.source_text = planned->edit.text;
  auto second = plan_routing_preset(repeated);
  REQUIRE(second);
  CHECK_FALSE(second->changed);
  CHECK(second->edit.text == repeated.routing.source_text);
}

TEST_CASE("P5 preset plan refuses missing proxy and geodata dependencies") {
  auto missing_proxy = request(RoutingPreset::bypass_mainland);
  missing_proxy.proxy.reset();
  REQUIRE_FALSE(plan_routing_preset(missing_proxy));
  auto missing_geoip = request(RoutingPreset::bypass_mainland);
  missing_geoip.geoip_labels.erase("cn");
  REQUIRE_FALSE(plan_routing_preset(missing_geoip));
  auto missing_balancer = request(RoutingPreset::bypass_mainland);
  missing_balancer.balancer_tags.clear();
  REQUIRE_FALSE(plan_routing_preset(missing_balancer));
}

TEST_CASE("P5 direct preset needs no proxy or geodata and custom leaves source untouched") {
  auto direct = request(RoutingPreset::direct_only);
  direct.proxy.reset();
  direct.geoip_labels.clear();
  direct.geosite_labels.clear();
  auto result = plan_routing_preset(direct);
  REQUIRE(result);
  CHECK(result->required_geoip_labels.empty());
  CHECK(result->required_geosite_labels.empty());
  CHECK(Json::parse(result->edit.text)["routing"]["rules"].back()["outboundTag"] == "direct");

  auto custom = request(RoutingPreset::custom_native);
  auto unchanged = plan_routing_preset(custom);
  REQUIRE(unchanged);
  CHECK_FALSE(unchanged->changed);
  CHECK(unchanged->edit.text == custom.routing.source_text);
}

TEST_CASE("P5 blacklist and global keep their first-match policy explicit") {
  auto black = plan_routing_preset(request(RoutingPreset::blacklist_proxy));
  REQUIRE(black);
  const auto black_rules = Json::parse(black->edit.text)["routing"]["rules"];
  CHECK(black_rules[1]["network"] == "udp");
  CHECK(black_rules[2]["domain"] == Json::array({"geosite:google"}));
  CHECK(black_rules.back()["outboundTag"] == "direct");
  CHECK(black->required_geoip_labels.contains("telegram"));
  CHECK(black->required_geosite_labels.contains("gfw"));

  auto global = plan_routing_preset(request(RoutingPreset::global_proxy));
  REQUIRE(global);
  const auto global_rules = Json::parse(global->edit.text)["routing"]["rules"];
  CHECK(global_rules[1]["network"] == "udp");
  CHECK(global_rules[2]["ip"] == Json::array({"geoip:private"}));
  CHECK(global_rules.back()["balancerTag"] == "chosen-group");
  CHECK_FALSE(global->required_geoip_labels.contains("cn"));
}

TEST_CASE("P5 geodata validation fixture is type-specific and rejects traversal") {
  REQUIRE_FALSE(geodata_validation_files(GeoAssetKind::geoip, "../geoip.dat", {"cn"}));
  REQUIRE_FALSE(geodata_validation_files(GeoAssetKind::geosite, "/tmp/geosite.dat", {"cn"}));
  REQUIRE_FALSE(geodata_validation_files(GeoAssetKind::geoip, "geosite.dat", {}));

  auto ip = geodata_validation_files(GeoAssetKind::geoip, "geoip.dat", {"cn"});
  REQUIRE(ip);
  REQUIRE(ip->size() == 1);
  const Json ip_config = Json::parse(ip->front().bytes);
  CHECK(ip_config["routing"]["rules"][0]["ip"] == Json::array({"ext:geoip.dat:cn"}));

  auto site = geodata_validation_files(GeoAssetKind::geosite, "geosite.dat", {"cn"});
  REQUIRE(site);
  const Json site_config = Json::parse(site->front().bytes);
  CHECK(site_config["routing"]["rules"][0]["domain"] == Json::array({"ext:geosite.dat:cn"}));
  CHECK(geodata_validation_files(GeoAssetKind::geosite, "geosite.dat",
                                  {"category-scholar-!cn"}));
}

TEST_CASE("P5 geodata inspection rejects empty and wrong-type bytes") {
  CHECK_FALSE(inspect_geodata_bytes(GeoAssetKind::geoip, ""));
  CHECK_FALSE(inspect_geodata_bytes(GeoAssetKind::geosite, std::string{"\x0a\x01\x00", 3}));
  // A valid protobuf envelope with a code but a Domain payload is not GeoIP.
  const std::string site_bytes = "\x0a\x11\x0a\x08XRM-TEST\x12\x05\x08\x03\x12\x01x";
  CHECK_FALSE(inspect_geodata_bytes(GeoAssetKind::geoip, site_bytes));
}

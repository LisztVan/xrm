#include <catch2/catch_test_macros.hpp>

#include "profile/forms.hpp"
#include "xray/codecs/codec.hpp"

using namespace xrm;

namespace {
ModuleDocument module(std::string key, Json root) {
  return {std::move(key), "module.json", root.dump()};
}

Json parsed(const ModuleEdit& edit) { return Json::parse(edit.text); }

Node typed_node() {
  Node node;
  node.id = "node-form-1";
  node.original_name = "fixture";
  StreamNode stream;
  stream.endpoint = {"127.0.0.1", 443};
  stream.protocol = VlessSettings{"11111111-1111-4111-8111-111111111111", "none", std::nullopt,
                                 std::nullopt};
  node.settings = std::move(stream);
  return node;
}
}  // namespace

TEST_CASE("FORM-01 advanced module fields update only native-owned keys", "[FORM-01][M4]") {
  const auto api = set_api_services(
      module("api", Json{{"api", Json{{"tag", "api"}, {"unknown", Json{{"kept", 1}}}}},
                           {"outside", 9}}),
      {"HandlerService"});
  REQUIRE(api);
  CHECK(parsed(*api)["api"]["services"] == Json::array({"HandlerService"}));
  CHECK(parsed(*api)["api"]["unknown"] == Json{{"kept", 1}});
  CHECK(parsed(*api)["outside"] == 9);

  const auto metrics = set_metrics_listener(
      module("metrics", Json{{"metrics", Json{{"listen", "127.0.0.1:9000"},
                                                {"unknown", Json{{"enabled", false}}}}}}),
      "127.0.0.1:9555");
  REQUIRE(metrics);
  CHECK(parsed(*metrics)["metrics"]["listen"] == "127.0.0.1:9555");
  CHECK(parsed(*metrics)["metrics"]["unknown"]["enabled"] == false);
}

TEST_CASE("MOD-07 env and version stay as native modules without changing process defaults", "[MOD-07][M4]") {
  const auto env = put_env(module("env", {{"env", {{"EXISTING", "same"}, {"KEEP", 7}}}}),
                           "XRM_FIXTURE", "native value");
  REQUIRE(env);
  CHECK(parsed(*env)["env"]["XRM_FIXTURE"] == "native value");
  CHECK(parsed(*env)["env"]["EXISTING"] == "same");
  CHECK(parsed(*env)["env"]["KEEP"] == 7);
  CHECK_FALSE(parsed(*env).contains("processEnvironment"));

  const auto version = set_version_range(
      module("version", {{"version", {{"unknown", true}}}}), std::nullopt, std::nullopt);
  REQUIRE(version);
  CHECK_FALSE(parsed(*version)["version"].contains("min"));
  CHECK_FALSE(parsed(*version)["version"].contains("max"));
  CHECK(parsed(*version)["version"]["unknown"] == true);
}

TEST_CASE("FORM-05 geodata forms edit resource references only", "[FORM-05][M4]") {
  const auto edited = replace_geodata_assets(
      module("geodata", {{"geodata", {{"downloadDetour", "proxy"},
                                        {"assets", Json::array({{{"path", "geoip.dat"},
                                                                  {"kind", "geoip"},
                                                                  {"vendor", "kept"}}})}}}}),
      {{{"path", "geoip.dat"}, {"kind", "geoip"}, {"vendor", "kept"}},
       {{"path", "geosite.dat"}, {"kind", "geosite"}}});
  REQUIRE(edited);
  CHECK(parsed(*edited)["geodata"]["assets"].size() == 2);
  CHECK(parsed(*edited)["geodata"]["assets"][0]["vendor"] == "kept");
  CHECK(parsed(*edited)["geodata"]["downloadDetour"] == "proxy");
  CHECK(parsed(*edited).dump().find("dat-bytes") == std::string::npos);

  const auto schedule = set_geodata_schedule(module("geodata", {{"geodata", {{"assets", Json::array({"geoip.dat"})},
                                                                                {"unknown", 3}}}}),
                                              "0 */6 * * *");
  REQUIRE(schedule);
  CHECK(parsed(*schedule)["geodata"]["cron"] == "0 */6 * * *");
  CHECK(parsed(*schedule)["geodata"]["assets"][0] == "geoip.dat");
  CHECK(parsed(*schedule)["geodata"]["unknown"] == 3);
}

TEST_CASE("UI-04 typed node sub-editors preserve identity and write fields to Xray native paths", "[UI-04][M4]") {
  auto node = typed_node();
  const auto id = node.id;

  auto transport = edit_node_transport(node, WebSocketTransport{"/ws", {{"Host", "edge.test"}}});
  REQUIRE(transport);
  auto security = edit_node_security(*transport, TlsSecurity{"sni.test", "chrome", {"h2"},
      Json{{"allowInsecure", false}, {"privateExtension", "preserve"}}});
  REQUIRE(security);
  auto sockopt = edit_node_sockopt(*security, Json{{"mark", 7}, {"custom", true}});
  REQUIRE(sockopt);
  auto finalmask = edit_node_finalmask(*sockopt, Json{{"enabled", true}, {"vendor", "keep"}});
  REQUIRE(finalmask);
  auto mux = edit_node_mux(*finalmask, Json{{"enabled", true}, {"concurrency", 4}, {"unknown", 8}});
  REQUIRE(mux);
  CHECK(mux->id == id);
  CHECK(std::get<StreamNode>(mux->settings).endpoint.port == 443);

  const auto outbound = write_outbound(*mux, "node.fixture");
  REQUIRE(outbound);
  CHECK((*outbound)["streamSettings"]["network"] == "websocket");
  CHECK((*outbound)["streamSettings"]["wsSettings"]["path"] == "/ws");
  CHECK((*outbound)["streamSettings"]["tlsSettings"]["serverName"] == "sni.test");
  CHECK((*outbound)["streamSettings"]["tlsSettings"]["privateExtension"] == "preserve");
  CHECK((*outbound)["streamSettings"]["sockopt"]["mark"] == 7);
  CHECK((*outbound)["streamSettings"]["finalmask"]["vendor"] == "keep");
  CHECK((*outbound)["mux"]["enabled"] == true);
  CHECK((*outbound)["mux"]["unknown"] == 8);
  CHECK_FALSE(outbound->contains("tls"));
  CHECK_FALSE(outbound->contains("transport"));
  CHECK_FALSE(outbound->contains("finalmask"));

  const auto reality = edit_node_security(node, RealitySecurity{
      "reality.sni", "chrome", "public-key", "0102", "/", Json{{"xrmExtension", 7}},
      "mldsa-verification"});
  REQUIRE(reality);
  const auto reality_outbound = write_outbound(*reality, "node.reality");
  REQUIRE(reality_outbound);
  CHECK((*reality_outbound)["streamSettings"]["security"] == "reality");
  CHECK((*reality_outbound)["streamSettings"]["realitySettings"]["serverName"] == "reality.sni");
  CHECK((*reality_outbound)["streamSettings"]["realitySettings"]["publicKey"] == "public-key");
  CHECK((*reality_outbound)["streamSettings"]["realitySettings"]["xrmExtension"] == 7);
  const auto& reality_json = (*reality_outbound)["streamSettings"]["realitySettings"];
  CHECK(reality_json.contains("mldsa65Verify"));
  if (reality_json.contains("mldsa65Verify"))
    CHECK(reality_json["mldsa65Verify"] == "mldsa-verification");
  CHECK(std::holds_alternative<RawTransport>(std::get<StreamNode>(node.settings).transport));
}

TEST_CASE("FORM-02 typed sub-editors reject raw nodes and wrong native object shapes", "[FORM-02][M4]") {
  Node raw;
  raw.id = "raw";
  raw.settings = RawOutbound{Json{{"protocol", "custom"}, {"unknown", 1}}};
  CHECK_FALSE(edit_node_transport(raw, RawTransport{}));
  CHECK_FALSE(edit_node_security(raw, NoSecurity{}));

  const auto node = typed_node();
  CHECK_FALSE(edit_node_sockopt(node, Json::array({1, 2})));
  CHECK_FALSE(edit_node_finalmask(node, "not-object"));
  CHECK_FALSE(edit_node_mux(node, Json::array()));

  auto hysteria = node;
  std::get<StreamNode>(hysteria.settings).protocol = Hysteria2Settings{"auth"};
  CHECK_FALSE(edit_node_transport(hysteria, WebSocketTransport{}));
  const auto native_transport = edit_node_transport(hysteria, HysteriaTransport{Json{{"version", 2}}});
  REQUIRE(native_transport);
  CHECK(std::holds_alternative<HysteriaTransport>(std::get<StreamNode>(native_transport->settings).transport));
}

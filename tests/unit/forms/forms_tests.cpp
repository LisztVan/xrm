#include <catch2/catch_test_macros.hpp>

#include "profile/forms.hpp"

using namespace xrm;

namespace {
ModuleDocument doc(std::string key, Json body) {
  return {std::move(key), "module.json", body.dump()};
}

Json body(const ModuleEdit& edit) { return Json::parse(edit.text); }
}

TEST_CASE("FORM-01 common forms update only owned native fields", "[FORM-01]") {
  auto log = set_log_level(doc("log", {{"log", {{"loglevel", "old"}, {"access", "old.log"}, {"extra", 7}}}}), "warning");
  REQUIRE(log);
  CHECK(body(*log)["log"]["loglevel"] == "warning");
  CHECK(body(*log)["log"]["access"] == "old.log");
  CHECK(body(*log)["log"]["extra"] == 7);

  auto dns = replace_dns_servers(doc("dns", {{"dns", {{"servers", Json::array({{{"address", "old"}, {"unknown", 8}}})}, {"queryStrategy", "UseIP"}, {"other", 9}}}, {"extra", "kept"}}), {{{"address", "new"}, {"unknown", 11}}});
  REQUIRE(dns);
  CHECK(body(*dns)["dns"]["servers"][0]["unknown"] == 11);
  CHECK(body(*dns)["dns"]["queryStrategy"] == "UseIP");
  CHECK(body(*dns)["dns"]["other"] == 9);
  CHECK(body(*dns)["extra"] == "kept");

  auto hosts = replace_dns_hosts(doc("dns", {{"dns", {{"servers", Json::array({"one"})}, {"hosts", {{"old", "127.0.0.1"}}}, {"unknown", 1}}}}), {{"new", "127.0.0.2"}});
  REQUIRE(hosts);
  CHECK(body(*hosts)["dns"]["hosts"]["new"] == "127.0.0.2");
  CHECK(body(*hosts)["dns"]["servers"][0] == "one");
  CHECK(body(*hosts)["dns"]["unknown"] == 1);

  auto api = set_api_services(doc("api", {{"api", {{"tag", "api"}, {"unknown", 2}}}}), {"HandlerService", "LoggerService"});
  REQUIRE(api);
  CHECK(body(*api)["api"]["services"] == Json::array({"HandlerService", "LoggerService"}));
  CHECK(body(*api)["api"]["unknown"] == 2);

  auto metrics = set_metrics_listener(doc("metrics", {{"metrics", {{"listen", "127.0.0.1:1"}, {"unknown", true}}}}), "127.0.0.1:9555");
  REQUIRE(metrics);
  CHECK(body(*metrics)["metrics"]["listen"] == "127.0.0.1:9555");
  CHECK(body(*metrics)["metrics"]["unknown"] == true);

  auto policy = set_policy_counters(doc("policy", {{"policy", {{"levels", {{"0", {{"connIdle", 60}}}}}, {"system", {{"statsInboundUplink", false}, {"keep", "yes"}}}, {"unknown", 7}}}}), true, false, false, true);
  REQUIRE(policy);
  CHECK(body(*policy)["policy"]["system"]["statsInboundUplink"] == true);
  CHECK(body(*policy)["policy"]["system"]["statsInboundDownlink"] == false);
  CHECK(body(*policy)["policy"]["system"]["statsOutboundUplink"] == false);
  CHECK(body(*policy)["policy"]["system"]["statsOutboundDownlink"] == true);
  CHECK(body(*policy)["policy"]["system"]["keep"] == "yes");
  CHECK(body(*policy)["policy"]["levels"]["0"]["connIdle"] == 60);
  CHECK(body(*policy)["policy"]["unknown"] == 7);

  auto fake_object = replace_fakedns_pools(doc("fakedns", {{"fakedns", {{"old", 1}}}, {"extra", true}}), {{"ipPool", "198.18.0.0/15"}});
  REQUIRE(fake_object);
  CHECK(body(*fake_object)["fakedns"].is_object());
  CHECK(body(*fake_object)["fakedns"]["ipPool"] == "198.18.0.0/15");
  CHECK(body(*fake_object)["extra"] == true);
  auto fake_array = replace_fakedns_pools(doc("fakedns", {{"fakedns", Json::array()}}), Json::array({{{"ipPool", "198.18.0.0/15"}}}));
  REQUIRE(fake_array);
  CHECK(body(*fake_array)["fakedns"].is_array());

  auto env = put_env(doc("env", {{"env", {{"OLD", "value"}}}, {"outside", 1}}), "X", "y");
  REQUIRE(env);
  CHECK(body(*env)["env"]["OLD"] == "value");
  CHECK(body(*env)["env"]["X"] == "y");
  CHECK(body(*env)["outside"] == 1);
  auto erased_env = erase_env(doc("env", {{"env", {{"OLD", "value"}, {"KEEP", "yes"}}}}), "OLD");
  REQUIRE(erased_env);
  CHECK_FALSE(body(*erased_env)["env"].contains("OLD"));
  CHECK(body(*erased_env)["env"]["KEEP"] == "yes");

  auto version = set_version_range(doc("version", {{"version", {{"min", "1.0"}, {"unknown", 4}}}}), "2.0", std::nullopt);
  REQUIRE(version);
  CHECK(body(*version)["version"]["min"] == "2.0");
  CHECK_FALSE(body(*version)["version"].contains("max"));
  CHECK(body(*version)["version"]["unknown"] == 4);

  auto geo = replace_geodata_assets(doc("geodata", {{"geodata", {{"downloadDetour", "proxy"}, {"unknown", 3}}}}), {{{"path", "geo.dat"}}});
  REQUIRE(geo);
  CHECK(body(*geo)["geodata"]["assets"][0]["path"] == "geo.dat");
  CHECK(body(*geo)["geodata"]["downloadDetour"] == "proxy");
  CHECK(body(*geo)["geodata"]["unknown"] == 3);
  auto schedule = set_geodata_schedule(doc("geodata", {{"geodata", {{"assets", Json::array({"old"})}}}}), "0 */6 * * *");
  REQUIRE(schedule);
  CHECK(body(*schedule)["geodata"]["cron"] == "0 */6 * * *");
  CHECK(body(*schedule)["geodata"]["assets"][0] == "old");

  auto observer = set_observatory_probe(doc("observatory", {{"observatory", {{"probeUrl", "old"}, {"subjectSelector", Json::array({"xrm.node.a"})}, {"unknown", 6}}}}), "http://127.0.0.1/p", "30s", true);
  REQUIRE(observer);
  CHECK(body(*observer)["observatory"]["probeUrl"] == "http://127.0.0.1/p");
  CHECK(body(*observer)["observatory"]["probeInterval"] == "30s");
  CHECK(body(*observer)["observatory"]["enableConcurrency"] == true);
  CHECK(body(*observer)["observatory"]["subjectSelector"][0] == "xrm.node.a");
  CHECK(body(*observer)["observatory"]["unknown"] == 6);
  auto burst = replace_burst_ping_config(doc("burstObservatory", {{"burstObservatory", {{"pingConfig", {{"old", true}}}, {"unknown", 5}}}}), {{"destination", "127.0.0.1:80"}});
  REQUIRE(burst);
  CHECK(body(*burst).contains("burstObservatory"));
  CHECK(body(*burst)["burstObservatory"]["pingConfig"]["destination"] == "127.0.0.1:80");
  CHECK(body(*burst)["burstObservatory"]["unknown"] == 5);
  CHECK_FALSE(set_observatory_probe(doc("burstObservatory", {{"burstObservatory", {{"pingConfig", {{"keep", true}}}}}}),
                                   "http://127.0.0.1/", "30s", true));
}

TEST_CASE("FORM-06 native target edit and list move preserve route matching fields", "[FORM-06]") {
  auto routing = doc("routing", {{"routing", {{"rules", Json::array({{{"domain", Json::array({"domain:keep.test"})}, {"port", "443"}, {"unknown", {{"keep", true}}}, {"outboundTag", "old"}}, {{"network", "udp"}}})}, {"domainStrategy", "IPIfNonMatch"}}}});
  auto targeted = set_rule_target(Json::parse(routing.source_text)["routing"]["rules"][0], {TargetKind::group, "group-1"}, "xrm.group.group-1");
  REQUIRE(targeted);
  CHECK(targeted->contains("balancerTag"));
  CHECK((*targeted)["balancerTag"] == "xrm.group.group-1");
  CHECK_FALSE(targeted->contains("outboundTag"));
  CHECK((*targeted)["domain"][0] == "domain:keep.test");
  CHECK((*targeted)["port"] == "443");
  CHECK((*targeted)["unknown"]["keep"] == true);

  auto moved = edit_native_list(routing, {CollectionKind::route_rules, ListAction::move, 0, 1, std::nullopt});
  REQUIRE(moved);
  const auto result = body(*moved);
  CHECK(result["routing"]["rules"][0]["network"] == "udp");
  CHECK(result["routing"]["rules"][1]["unknown"]["keep"] == true);
  CHECK(result["routing"]["domainStrategy"] == "IPIfNonMatch");
}

TEST_CASE("FORM-07 list operations retain complete inbound and outbound objects", "[FORM-07]") {
  auto outbounds = doc("outbounds", {{"outbounds", Json::array({{{"tag", "direct"}, {"protocol", "freedom"}, {"settings", {{"domainStrategy", "UseIP"}, {"unknown", 8}}}, {"streamSettings", {{"network", "tcp"}, {"security", "tls"}, {"tlsSettings", {{"serverName", "keep.test"}}}}}, {"mux", {{"enabled", true}, {"unknown", 4}}}, {"vendor", {{"x", 1}}}}})}, {"extra", true}});
  auto inserted = edit_native_list(outbounds, {CollectionKind::outbounds, ListAction::insert, 1, std::nullopt, Json{{"tag", "copy"}, {"protocol", "freedom"}, {"settings", {{"unknown", 8}}}, {"streamSettings", {{"security", "reality"}, {"realitySettings", {{"publicKey", "kept"}}}}}, {"mux", {{"enabled", true}}}, {"vendor", {{"x", 1}}}}});
  REQUIRE(inserted);
  CHECK(body(*inserted)["outbounds"].size() == 2);
  CHECK(body(*inserted)["outbounds"][1]["streamSettings"]["realitySettings"]["publicKey"] == "kept");
  CHECK(body(*inserted)["outbounds"][1]["mux"]["enabled"] == true);
  CHECK(body(*inserted)["outbounds"][1]["vendor"]["x"] == 1);
  CHECK(body(*inserted)["extra"] == true);

  auto inbound = doc("inbounds", {{"inbounds", Json::array({{{"tag", "socks"}, {"protocol", "socks"}, {"settings", {{"udp", true}, {"unknown", 3}}}, {"sniffing", {{"enabled", true}, {"destOverride", Json::array({"http"})}, {"unknown", "keep"}}}, {"streamSettings", {{"network", "tcp"}, {"sockopt", {{"mark", 3}}}, {"finalmask", {{"enabled", true}}}}}}})}});
  auto replacement = edit_native_list(inbound, {CollectionKind::inbounds, ListAction::replace, 0, std::nullopt, Json{{"tag", "http-in"}, {"protocol", "http"}, {"settings", {{"timeout", 10}, {"unknown", 3}}}, {"sniffing", {{"enabled", true}, {"unknown", "keep"}}}, {"streamSettings", {{"network", "tcp"}, {"sockopt", {{"mark", 3}}}, {"finalmask", {{"enabled", true}}}}}}});
  REQUIRE(replacement);
  CHECK(body(*replacement)["inbounds"][0]["sniffing"]["unknown"] == "keep");
  CHECK(body(*replacement)["inbounds"][0]["streamSettings"]["sockopt"]["mark"] == 3);
  CHECK(body(*replacement)["inbounds"][0]["streamSettings"]["finalmask"]["enabled"] == true);
}

TEST_CASE("FORM-01 safe structured forms reject comments and malformed JSON with native editor guidance", "[FORM-01]") {
  const auto result = set_log_level({"log", "00_log.json", "{ /* comment */ \"log\": {} }"}, "warning");
  REQUIRE_FALSE(result);
  CHECK(result.error().hint.has_value());
}

TEST_CASE("P3 structured native edits keep one module draft and native types", "[P3][FORM-01]") {
  const auto original = doc("dns", {{"dns", {{"servers", Json::array({"one", "two"})},
      {"hosts", {{"old.test", "127.0.0.1"}}}, {"unknown", {{"nested", true}}}}},
      {"outside", 7}});
  auto added = edit_json_draft(original, {JsonEditAction::set, "/dns/hosts/new.test", "127.0.0.2"});
  REQUIRE(added);
  CHECK(added->base.expected_text == original.source_text);
  CHECK(body(*added)["dns"]["unknown"]["nested"] == true);
  CHECK(body(*added)["outside"] == 7);
  auto bool_edit = edit_json_draft({"dns", "module.json", added->text},
      {JsonEditAction::set, "/dns/unknown/nested", false});
  REQUIRE(bool_edit);
  CHECK(body(*bool_edit)["dns"]["unknown"]["nested"].is_boolean());
  CHECK(body(*bool_edit)["dns"]["unknown"]["nested"] == false);
  auto rename = edit_json_draft({"dns", "module.json", bool_edit->text},
      {JsonEditAction::rename_key, "/dns/hosts/old.test", {}, "renamed.test"});
  REQUIRE(rename);
  CHECK(body(*rename)["dns"]["hosts"]["renamed.test"] == "127.0.0.1");
  CHECK_FALSE(body(*rename)["dns"]["hosts"].contains("old.test"));
  auto removed = edit_json_draft({"dns", "module.json", rename->text},
      {JsonEditAction::erase, "/dns/hosts/new.test"});
  REQUIRE(removed);
  CHECK_FALSE(body(*removed)["dns"]["hosts"].contains("new.test"));
  CHECK(body(*removed)["dns"]["unknown"]["nested"] == false);
}

TEST_CASE("P3 structured array actions keep route order and reject invalid drafts", "[P3][FORM-06]") {
  const auto routing = doc("routing", {{"routing", {{"rules", Json::array({
      {{"outboundTag", "a"}, {"unknown", 1}}, {{"outboundTag", "b"}, {"unknown", 2}}})},
      {"balancers", Json::array({{{"tag", "static"}, {"selector", Json::array({"a"})}}})},
      {"domainStrategy", "AsIs"}}}});
  auto copied = edit_json_draft(routing,
      {JsonEditAction::array_copy, "/routing/rules", {}, {}, 0, 2});
  REQUIRE(copied);
  CHECK(body(*copied)["routing"]["rules"].size() == 3);
  auto moved = edit_json_draft({"routing", "module.json", copied->text},
      {JsonEditAction::array_move, "/routing/rules", {}, {}, 2, 0});
  REQUIRE(moved);
  CHECK(body(*moved)["routing"]["rules"][0]["outboundTag"] == "a");
  CHECK(body(*moved)["routing"]["rules"][2]["outboundTag"] == "b");
  CHECK(body(*moved)["routing"]["balancers"][0]["tag"] == "static");
  auto inserted = edit_json_draft({"routing", "module.json", moved->text},
      {JsonEditAction::array_insert, "/routing/rules", Json{{"outboundTag", "c"}}, {}, 1});
  REQUIRE(inserted);
  CHECK(body(*inserted)["routing"]["rules"][1]["outboundTag"] == "c");
  auto erased = edit_json_draft({"routing", "module.json", inserted->text},
      {JsonEditAction::array_erase, "/routing/rules", {}, {}, 1});
  REQUIRE(erased);
  CHECK(body(*erased)["routing"]["rules"].size() == 3);
  CHECK_FALSE(edit_json_draft({"routing", "module.json", "{broken"},
      {JsonEditAction::set, "/routing/domainStrategy", "IPIfNonMatch"}));
  CHECK_FALSE(edit_json_draft(routing,
      {JsonEditAction::erase, "/routing"}));
  CHECK_FALSE(edit_json_draft(routing,
      {JsonEditAction::set, "/outside", 9}));
  const auto inbounds = doc("inbounds", {{"inbounds", Json::array()}});
  auto inbound_insert = edit_json_draft(inbounds, {JsonEditAction::array_insert,
      "/inbounds", Json{{"tag", "local"}, {"protocol", "socks"}}, {}, 0});
  REQUIRE(inbound_insert);
  CHECK(body(*inbound_insert)["inbounds"][0]["tag"] == "local");
}

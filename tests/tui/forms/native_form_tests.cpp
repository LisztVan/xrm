#include "tui/forms/native_form.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <ftxui/screen/screen.hpp>

using nlohmann::json;
using namespace xrm::tui;
using namespace xrm::tui::forms;
using ftxui::Event;

namespace {
std::size_t field(const std::shared_ptr<ui::Form>& form, const std::string& label) {
  for (std::size_t i = 0; i < form->fields().size(); ++i)
    if (form->fields()[i].label == label) return i;
  throw std::runtime_error("Missing form field: " + label);
}
std::string render(ftxui::Component component, int width = 80, int height = 24) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, component->Render());
  return screen.ToString();
}
struct Host {
  std::string draft;
  std::vector<ftxui::Component> dialogs;
  int changes{};
  NativeFormOptions options(std::string module, bool readonly = false) {
    return {std::move(module), &draft, readonly, {{"direct", "Direct"}, {"proxy", "Proxy"}}, {{"auto", "Auto group"}}, {{"socks-in", "SOCKS"}},
      [this](ftxui::Component dialog) { dialogs.push_back(std::move(dialog)); },
      [this] { if (!dialogs.empty()) dialogs.pop_back(); }, [this] { ++changes; }};
  }
  void event(Event event) {
    REQUIRE_FALSE(dialogs.empty());
    const auto current = dialogs.back(); // callbacks may pop their own overlay
    current->OnEvent(event);
  }
  std::shared_ptr<ui::Form> form() {
    REQUIRE_FALSE(dialogs.empty());
    auto result = std::dynamic_pointer_cast<ui::Form>(dialogs.back());
    REQUIRE(result);
    return result;
  }
};
}

TEST_CASE("Native semantic forms preserve unknown fields and absent defaults", "[tui][native-form]") {
  Host host;
  for (const auto& kind : NativeFormKinds()) {
    INFO(kind);
    json original = {{"futureExtension", {1, false, "opaque"}}};
    if (kind == "dns-server") original["address"] = "localhost";
    if (kind == "routing-rule") original["outboundTag"] = "direct";
    json result;
    auto form = MakeObjectForm(kind, original, host.options(kind), [&](const json& value) -> std::optional<std::string> { result = value; return {}; }, [] {});
    REQUIRE_FALSE(form->fields().empty());
    CHECK(form->submit());
    CHECK(result == original);
    CHECK_FALSE(render(form).empty());
  }
}

TEST_CASE("DNS string and object representations are deliberate", "[tui][native-form]") {
  Host host;
  json result;
  auto accept = [&](const json& value) -> std::optional<std::string> { result = value; return {}; };
  auto form = MakeObjectForm("dns-server", "localhost", host.options("dns"), accept, [] {});
  form->set_value(field(form, "Address"), "tcp://127.0.0.1");
  REQUIRE(form->submit());
  CHECK(result == "tcp://127.0.0.1");
  form->set_value(field(form, "port"), "5353");
  CHECK_FALSE(form->submit());
  CHECK(form->value(field(form, "port")) == "5353");
  form->set_value(field(form, "Native representation"), "object");
  REQUIRE(form->submit());
  CHECK(result == json{{"address", "tcp://127.0.0.1"}, {"port", 5353}});

  auto object = MakeObjectForm("dns-server", json{{"address", "localhost"}, {"future", 42}}, host.options("dns"), accept, [] {});
  object->set_value(field(object, "Address"), "127.0.0.1");
  REQUIRE(object->submit());
  CHECK(result.at("future") == 42);
  object->set_value(field(object, "port"), "65536");
  CHECK_FALSE(object->submit());
  CHECK(object->value(field(object, "Address")) == "127.0.0.1");
}

TEST_CASE("Native optional booleans preserve unset and explicit defaults", "[tui][native-form]") {
  Host host; json result;
  auto form = MakeObjectForm("dns", json{{"disableCache", false}, {"tag", "dns"}}, host.options("dns"), [&](const json& value) -> std::optional<std::string> { result = value; return {}; }, [] {});
  CHECK(form->fields()[field(form, "disableCache")].kind == ui::FieldKind::OptionalCheckbox);
  form->set_value(field(form, "tag"), "edited");
  REQUIRE(form->submit());
  CHECK(result.at("disableCache") == false);
  CHECK_FALSE(result.contains("serveStale"));
  form->set_value(field(form, "serveStale"), "false");
  REQUIRE(form->submit());
  CHECK(result.at("serveStale") == false);
  form->set_value(field(form, "disableCache"), "");
  REQUIRE(form->submit());
  CHECK_FALSE(result.contains("disableCache"));
}

TEST_CASE("Legal alternative native types and aliases survive another field edit", "[tui][native-form]") {
  Host host; json result;
  const auto accept = [&](const json& value) -> std::optional<std::string> { result = value; return {}; };
  auto inbound = MakeObjectForm("inbound", json{{"port", "1000-1002"}, {"tag", "in"}}, host.options("inbounds"), accept, [] {});
  inbound->set_value(field(inbound, "tag"), "renamed");
  REQUIRE(inbound->submit()); CHECK(result.at("port") == "1000-1002");
  auto dns = MakeObjectForm("dns-server", json{{"address", "localhost"}, {"clientIP", "127.0.0.1"}}, host.options("dns"), accept, [] {});
  dns->set_value(field(dns, "clientIp"), "127.0.0.2");
  REQUIRE(dns->submit()); CHECK(result.at("clientIP") == "127.0.0.2"); CHECK_FALSE(result.contains("clientIp"));
  auto stream = MakeObjectForm("stream", json{{"network", "tcp"}, {"security", "none"}}, host.options("outbounds"), accept, [] {});
  stream->set_value(field(stream, "security"), "tls");
  REQUIRE(stream->submit()); CHECK(result.at("network") == "tcp"); CHECK_FALSE(result.contains("method"));
  auto rule = MakeObjectForm("routing-rule", json{{"outboundTag", "direct"}, {"network", json::array({"tcp", "udp"})}}, host.options("routing"), accept, [] {});
  rule->set_value(field(rule, "Rule label"), "keep network type");
  REQUIRE(rule->submit()); CHECK(result.at("network").is_array());
}

TEST_CASE("Routing semantic fields use selectors and preserve unknown legacy targets", "[tui][native-form]") {
  Host host; json result;
  auto form = MakeObjectForm("routing-rule", json{{"outboundTag", "native-unlisted"}, {"domain", {"domain:example.test"}}, {"future", 17}}, host.options("routing"), [&](const json& value) -> std::optional<std::string> { result = value; return {}; }, [] {});
  CHECK(form->fields()[field(form, "outboundTag")].kind == ui::FieldKind::Select);
  CHECK(form->fields()[field(form, "domain")].kind == ui::FieldKind::StringList);
  form->set_value(field(form, "Rule label"), "renamed");
  REQUIRE(form->submit()); CHECK(result.at("outboundTag") == "native-unlisted"); CHECK(result.at("future") == 17);
  form->set_value(field(form, "outboundTag"), "");
  CHECK_FALSE(form->submit());
  form->set_value(field(form, "balancerTag"), "auto");
  REQUIRE(form->submit()); CHECK_FALSE(result.contains("outboundTag")); CHECK(result.at("balancerTag") == "auto");
}

TEST_CASE("Readonly native forms block submit and keep text", "[tui][native-form]") {
  Host host; bool accepted = false;
  auto form = MakeObjectForm("dns-server", json{{"address", "localhost"}}, host.options("dns", true), [&](const json&) -> std::optional<std::string> { accepted = true; return {}; }, [] {});
  form->set_value(field(form, "Address"), "127.0.0.1");
  CHECK_FALSE(form->submit()); CHECK_FALSE(accepted);
}

TEST_CASE("DNS visual collection modifies only the shared draft in native order", "[tui][native-form]") {
  Host host;
  host.draft = R"({"dns":{"servers":["localhost",{"address":"127.0.0.1","future":true}],"hosts":{"loop.test":"127.0.0.1"}},"untouched":42})";
  auto visual = MakeNativeForm(host.options("dns"));
  const auto baseline = host.draft;
  CHECK(render(visual).find("DNS servers") != std::string::npos);
  visual->OnEvent(Event::Return);
  REQUIRE(host.dialogs.size() == 1);
  host.event(Event::Character("a"));
  auto form = host.form();
  form->set_value(field(form, "Address"), "tcp://127.0.0.2");
  form->set_value(field(form, "Native representation"), "string");
  CHECK(host.draft == baseline);
  REQUIRE(form->submit());
  CHECK(json::parse(host.draft)["dns"]["servers"].at(2) == "tcp://127.0.0.2");
  CHECK(json::parse(host.draft)["untouched"] == 42);
  CHECK(json::parse(host.draft)["dns"]["hosts"]["loop.test"] == "127.0.0.1");
  host.event(Event::Character("-"));
  CHECK(json::parse(host.draft)["dns"]["servers"].at(1) == "localhost");
  host.event(Event::Character("c"));
  CHECK(json::parse(host.draft)["dns"]["servers"].size() == 4);
  CHECK(host.changes == 3);
}

TEST_CASE("Collection view and readonly controls cannot mutate modules", "[tui][native-form]") {
  for (bool readonly : {false, true}) {
    Host host; host.draft = R"({"dns":{"servers":["localhost"]}})";
    const auto baseline = host.draft;
    auto visual = MakeNativeForm(host.options("dns", readonly));
    visual->OnEvent(Event::Return);
    host.event(Event::Return);
    auto form = host.form();
    form->set_value(field(form, "Address"), "127.0.0.1");
    CHECK_FALSE(form->submit()); CHECK(host.draft == baseline);
  }
}

TEST_CASE("All fifteen module forms open without generating source content", "[tui][native-form]") {
  for (const auto* module : {"env", "log", "api", "dns", "routing", "policy", "inbounds", "outbounds", "stats", "fakedns", "metrics", "observatory", "burstObservatory", "geodata", "version"}) {
    INFO(module); Host host; host.draft = "{}";
    auto visual = MakeNativeForm(host.options(module));
    CHECK_FALSE(render(visual).empty());
    visual->OnEvent(Event::Return);
    CHECK(host.dialogs.size() == 1);
    CHECK(host.draft == "{}"); CHECK(host.changes == 0);
  }
}

TEST_CASE("Unsupported existing field types remain readonly and do not block unrelated edits", "[tui][native-form]") {
  Host host; json result;
  const json original{{"address", "localhost"}, {"port", "future-port-type"}, {"domains", json::array({"", "domain:example.test"})}, {"serveExpiredTTL", nullptr},
                      {"timeoutMs", std::numeric_limits<std::uint64_t>::max()}};
  auto form = MakeObjectForm("dns-server", original, host.options("dns"), [&](const json& value) -> std::optional<std::string> { result = value; return {}; }, [] {});
  CHECK(form->fields()[field(form, "port")].readonly);
  CHECK(form->fields()[field(form, "domains")].readonly);
  CHECK(form->fields()[field(form, "timeoutMs")].readonly);
  form->set_value(field(form, "Address"), "127.0.0.1");
  REQUIRE(form->submit());
  CHECK(result.at("port") == original.at("port"));
  CHECK(result.at("domains") == original.at("domains"));
  CHECK(result.at("serveExpiredTTL").is_null());
  CHECK(result.at("timeoutMs") == original.at("timeoutMs"));
}

TEST_CASE("Native optional scalar and collection presence is independent of value", "[tui][native-form][remediation]") {
  Host host;
  json result;
  auto accept = [&](const json& value) -> std::optional<std::string> { result = value; return {}; };
  auto form = MakeObjectForm("dns", json::object(), host.options("dns"), accept, [] {});
  const auto tag = field(form, "tag");
  const auto ttl = field(form, "serveExpiredTTL");
  CHECK_FALSE(form->present(tag));
  form->set_present(tag, true);
  REQUIRE(form->submit());
  CHECK(result.at("tag") == "");
  CHECK_FALSE(result.contains("serveExpiredTTL"));
  form->set_value(ttl, "0");
  REQUIRE(form->submit());
  CHECK(result.at("serveExpiredTTL") == 0);
  form->set_present(tag, false);
  REQUIRE(form->submit());
  CHECK_FALSE(result.contains("tag"));

  auto observe = MakeObjectForm("observatory", json::object(), host.options("observatory"), accept, [] {});
  observe->set_present(field(observe, "subjectSelector"), true);
  REQUIRE(observe->submit());
  CHECK(result.at("subjectSelector") == json::array());
  observe->set_present(field(observe, "subjectSelector"), false);
  REQUIRE(observe->submit());
  CHECK_FALSE(result.contains("subjectSelector"));

  auto api = MakeObjectForm("api", json{{"tag", "control"}}, host.options("api"), accept, [] {});
  api->set_present(field(api, "services"), true);
  REQUIRE(api->submit());
  CHECK(result.at("services") == json::array());
  api->set_present(field(api, "services"), false);
  REQUIRE(api->submit());
  CHECK_FALSE(result.contains("services"));
}

TEST_CASE("Unnamed routing rule has a discoverable target and retains native paths", "[tui][native-form][remediation]") {
  Host host;
  const json original{{"domain", {"domain:example.test"}}, {"outboundTag", "direct"},
                      {"future/key~", {{"secret", "never-display-this"}}}};
  json result;
  auto form = MakeObjectForm("routing-rule", original, host.options("routing"),
      [&](const json& value) -> std::optional<std::string> { result = value; return {}; }, [] {});
  CHECK(form->fields()[field(form, "Rule label")].optional);
  CHECK_FALSE(form->present(field(form, "Rule label")));
  REQUIRE(form->submit());
  CHECK(result == original);
  CHECK_FALSE(result.contains("ruleTag"));
  CHECK(form->context().find("/future~1key~0") != std::string::npos);
  CHECK(form->context().find("never-display-this") == std::string::npos);
  for (const auto width : {80, 120, 180}) {
    form->focus_field(field(form, "Rule label"));
    const auto screen = render(form, width, width == 80 ? 24 : 40);
    CHECK(screen.find("Match conditions -> Target") != std::string::npos);
    form->focus_field(field(form, "outboundTag"));
    CHECK(render(form, width, width == 80 ? 24 : 40).find("outboundTag") != std::string::npos);
  }
  form->set_value(field(form, "outboundTag"), "");
  CHECK_FALSE(form->submit());
  CHECK(form->value(field(form, "domain")) == "domain:example.test");
  form->set_value(field(form, "balancerTag"), "auto");
  REQUIRE(form->submit());
  CHECK(result.at("balancerTag") == "auto");
  CHECK_FALSE(result.contains("ruleTag"));
  CHECK(result.at("future/key~") == original.at("future/key~"));
}

TEST_CASE("Native map edits preserve zero and allow explicit field removal", "[tui][native-form][remediation]") {
  Host host;
  host.draft = R"({"policy":{"levels":{"0":{"handshake":0,"bufferSize":0,"statsUserUplink":false,"future":[]}}}})";
  auto visual = MakeNativeForm(host.options("policy"));
  visual->OnEvent(Event::Return);
  host.event(Event::Character("e"));
  auto form = host.form();
  form->set_value(field(form, "User level"), "1");
  form->set_present(field(form, "handshake"), false);
  REQUIRE(form->submit());
  const auto result = json::parse(host.draft).at("policy").at("levels");
  CHECK_FALSE(result.contains("0"));
  CHECK_FALSE(result.at("1").contains("handshake"));
  CHECK(result.at("1").at("bufferSize") == 0);
  CHECK(result.at("1").at("statsUserUplink") == false);
  CHECK(result.at("1").at("future") == json::array());
}

TEST_CASE("All native module entry points stay nonmutating in readonly mode", "[tui][native-form][remediation]") {
  for (const auto* module : {"env", "log", "api", "dns", "routing", "policy", "inbounds", "outbounds", "stats", "fakedns", "metrics", "observatory", "burstObservatory", "geodata", "version"}) {
    INFO(module);
    Host host;
    host.draft = "{}";
    auto visual = MakeNativeForm(host.options(module, true));
    visual->OnEvent(Event::Return);
    REQUIRE(host.dialogs.size() == 1);
    host.event(Event::Character("a"));
    host.event(Event::Character("e"));
    host.event(Event::Special(std::string(1, '\x13')));
    CHECK(host.draft == "{}");
    CHECK(host.changes == 0);
    if (const auto form = std::dynamic_pointer_cast<ui::Form>(host.dialogs.back())) {
      CHECK(form->readonly());
      CHECK_FALSE(form->submit());
    }
  }
}

TEST_CASE("Native scalar modules accept basic edits while preserving unknown fields", "[tui][native-form][remediation]") {
  struct Example { const char* module; const char* label; const char* value; const char* pointer; };
  for (const auto& example : std::vector<Example>{
      {"log", "Access destination", "none", "/log/access"},
      {"api", "tag", "control", "/api/tag"},
      {"metrics", "Listen address:port", "127.0.0.1:12000", "/metrics/listen"},
      {"observatory", "Probe interval", "30s", "/observatory/probeInterval"},
      {"burstObservatory", "Probe URL", "https://example.test/probe", "/burstObservatory/pingConfig/destination"},
      {"geodata", "Reload schedule", "0 0 * * *", "/geodata/cron"},
      {"version", "Minimum Xray version", "26.9.9", "/version/min"}}) {
    INFO(example.module);
    Host host;
    const json original{{example.module, {{"futureNative", json::array({false, 0, ""})}}}, {"untouched", 17}};
    host.draft = original.dump();
    auto visual = MakeNativeForm(host.options(example.module));
    visual->OnEvent(Event::Return);
    auto form = host.form();
    form->set_value(field(form, example.label), example.value);
    CHECK(json::parse(host.draft) == original);
    REQUIRE(form->submit());
    auto expected = original;
    expected[json::json_pointer(example.pointer)] = example.value;
    CHECK(json::parse(host.draft) == expected);
    CHECK(host.changes == 1);
  }
}

TEST_CASE("Native object collections preserve order through copy move remove and add", "[tui][native-form][remediation]") {
  struct Example {
    const char* module;
    const char* pointer;
    int section;
    json first, second;
    const char* label;
    const char* added;
  };
  for (const auto& example : std::vector<Example>{
      {"dns", "/dns/servers", 0, {{"address", "localhost"}}, {{"address", "1.1.1.1"}}, "Address", "8.8.8.8"},
      {"routing", "/routing/rules", 0, {{"domain", {"domain:one.test"}}, {"outboundTag", "direct"}}, {{"domain", {"domain:two.test"}}, {"outboundTag", "direct"}}, "outboundTag", "direct"},
      {"routing", "/routing/balancers", 1, {{"tag", "one"}}, {{"tag", "two"}}, "tag", "three"},
      {"inbounds", "/inbounds", 0, {{"tag", "one"}, {"protocol", "socks"}}, {{"tag", "two"}, {"protocol", "socks"}}, "tag", "three"},
      {"outbounds", "/outbounds", 0, {{"tag", "one"}, {"protocol", "freedom"}}, {{"tag", "two"}, {"protocol", "freedom"}}, "tag", "three"},
      {"fakedns", "/fakedns", 0, {{"ipPool", "198.18.0.0/16"}}, {{"ipPool", "198.19.0.0/16"}}, "Fake IP pool (CIDR)", "198.18.0.0/15"},
      {"geodata", "/geodata/assets", 1, {{"file", "one.dat"}}, {{"file", "two.dat"}}, "Existing asset filename", "three.dat"}}) {
    INFO(example.module << example.pointer);
    Host host;
    json initial{{"untouched", {false, 0, ""}}};
    const json::json_pointer path(example.pointer);
    initial[path] = json::array({example.first, example.second});
    host.draft = initial.dump();
    auto visual = MakeNativeForm(host.options(example.module));
    for (int i = 0; i < example.section; ++i)
      visual->OnEvent(Event::ArrowDown);
    visual->OnEvent(Event::Return);
    host.event(Event::Character("c"));
    CHECK(json::parse(host.draft).at(path).size() == 3);
    host.event(Event::Character("-"));
    host.event(Event::Character("-"));
    CHECK(json::parse(host.draft).at(path) == json::array({example.first, example.second, example.first}));
    host.event(Event::Character("D"));
    REQUIRE(host.dialogs.size() == 2);
    host.event(Event::Return);
    CHECK(json::parse(host.draft).at(path).size() == 3); // Safe default cancels.
    host.event(Event::Character("D"));
    REQUIRE(host.dialogs.size() == 2);
    host.event(Event::TabReverse); // Explicitly focus Confirm.
    host.event(Event::Return);
    CHECK(json::parse(host.draft).at(path) == json::array({example.first, example.second}));
    host.event(Event::Character("a"));
    auto form = host.form();
    form->set_value(field(form, example.label), example.added);
    REQUIRE(form->submit());
    const auto result = json::parse(host.draft);
    CHECK(result.at(path).size() == 3);
    CHECK(result.at(path).at(0) == example.first);
    CHECK(result.at(path).at(1) == example.second);
    CHECK(result.at("untouched") == initial.at("untouched"));
  }
}

TEST_CASE("Native environment and hosts maps retain values on rename collision", "[tui][native-form][remediation]") {
  for (const bool hosts : {false, true}) {
    Host host;
    const std::string module = hosts ? "dns" : "env";
    const json::json_pointer path(hosts ? "/dns/hosts" : "/env");
    json original;
    original[path] = {{"first", hosts ? json::array({"127.0.0.1"}) : json("")},
                      {"second", "127.0.0.2"}};
    host.draft = original.dump();
    auto visual = MakeNativeForm(host.options(module));
    if (hosts) {
      visual->OnEvent(Event::ArrowDown);
      visual->OnEvent(Event::ArrowDown);
    }
    visual->OnEvent(Event::Return);
    host.event(Event::Character("e"));
    auto form = host.form();
    form->set_value(0, "second");
    CHECK_FALSE(form->submit());
    CHECK(form->value(0) == "second");
    CHECK(json::parse(host.draft) == original);
    form->set_value(0, "renamed");
    REQUIRE(form->submit());
    auto expected = original;
    expected[path]["renamed"] = original.at(path).at("first");
    expected[path].erase("first");
    CHECK(json::parse(host.draft) == expected);
  }
}

TEST_CASE("Fake DNS native object form keeps object representation and zero", "[tui][native-form][remediation]") {
  Host host;
  host.draft = R"({"fakedns":{"ipPool":"198.18.0.0/15","poolSize":42,"future":[]}})";
  auto visual = MakeNativeForm(host.options("fakedns"));
  visual->OnEvent(Event::Return);
  auto form = host.form();
  form->set_value(field(form, "poolSize"), "0");
  REQUIRE(form->submit());
  const auto result = json::parse(host.draft).at("fakedns");
  CHECK(result.is_object());
  CHECK(result.at("poolSize") == 0);
  CHECK(result.at("future") == json::array());
}

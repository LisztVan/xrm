// XRM: Tests assembler in the domain and editor behavior suite.

#include "model/module_catalog.hpp"
#include "xray/codecs/codec.hpp"
#include "xray/runtime_assembler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <map>

namespace {
using namespace xrm;

constexpr std::array<std::pair<std::string_view, std::string_view>, 15> modules{
    {
        {"env", "12_env.json"},
        {"log", "00_log.json"},
        {"api", "06_api.json"},
        {"dns", "01_dns.json"},
        {"routing", "04_routing.json"},
        {"policy", "09_policy.json"},
        {"inbounds", "02_inbounds.json"},
        {"outbounds", "03_outbounds.json"},
        {"stats", "08_stats.json"},
        {"fakedns", "10_fakedns.json"},
        {"metrics", "07_metrics.json"},
        {"observatory", "05_observatory.json"},
        {"burstObservatory", "14_burstObservatory.json"},
        {"geodata", "11_geodata.json"},
        {"version", "13_version.json"},
    }};

BuildInput base_input() {
  BuildInput input;
  input.settings.profile.id = "p1";
  input.settings.profile.name = "test";
  input.settings.profile.core_id = "core1";
  return input;
}

void enable(BuildInput &input, std::string key, std::string filename,
            std::string text) {
  input.settings.profile.modules.push_back(ModuleSetting{key, filename, true});
  input.settings.modules.push_back(
      ModuleDocument{std::move(key), std::move(filename), std::move(text)});
}

const OutputFile *file(const PreparedFiles &prepared,
                       std::string_view filename) {
  const auto found = std::find_if(
      prepared.files.begin(), prepared.files.end(),
      [&](const auto &entry) { return entry.relative_name == filename; });
  return found == prepared.files.end() ? nullptr : &*found;
}

Node raw_node(std::string id, std::string tag) {
  Node node;
  node.id = std::move(id);
  node.original_name = "fixture";
  node.settings =
      RawOutbound{Json{{"protocol", "freedom"}, {"settings", Json::object()}}};
  auto &outbound = std::get<RawOutbound>(node.settings).value;
  outbound["tag"] = std::move(tag);
  return node;
}
} // namespace

TEST_CASE("each native module emits its own original wrapper",
          "[unit][ASM-01]") {
  for (const auto &[key, filename] : modules) {
    auto input = base_input();
    Json value = key == "stats" ? Json::object() : Json{{"marker", key}};
    if (key == "outbounds")
      value = Json::array({Json{{"tag", "static"}, {"protocol", "freedom"}}});
    if (key == "routing")
      value = Json{{"rules", Json::array()}, {"balancers", Json::array()}};
    const Json wrapper{{std::string(key), value}};
    enable(input, std::string(key), std::string(filename), wrapper.dump());
    auto built = assemble(input);
    INFO("module: " << key);
    if (!built)
      INFO("error: " << built.error().message);
    REQUIRE(built);
    const auto *output = file(*built, filename);
    REQUIRE(output);
    CHECK(Json::parse(output->bytes) == wrapper);
    CHECK(built->files.size() == 1);
  }
}

TEST_CASE("observability roots are mutually exclusive native modules",
          "[unit][ASM-01]") {
  for (const auto &[key, filename] :
       std::array<std::pair<std::string_view, std::string_view>, 2>{
           {{"observatory", "05_observatory.json"},
            {"burstObservatory", "14_burstObservatory.json"}}}) {
    auto input = base_input();
    const Json wrapper{
        {std::string(key), Json{{"subjectSelector", Json::array({"static"})}}}};
    enable(input, std::string(key), std::string(filename), wrapper.dump());
    auto built = assemble(input);
    REQUIRE(built);
    REQUIRE(built->files.size() == 1);
    CHECK(Json::parse(built->files.front().bytes) == wrapper);
  }
}

TEST_CASE("a module edit only changes that module output", "[unit][ASM-02]") {
  auto input = base_input();
  enable(input, "log", "00_log.json", R"({"log":{"loglevel":"warning"}})");
  enable(input, "dns", "01_dns.json", R"({"dns":{"servers":["localhost"]}})");
  auto first = assemble(input);
  REQUIRE(first);
  input.settings.modules.front().source_text =
      R"({"log":{"loglevel":"debug"}})";
  auto second = assemble(input);
  REQUIRE(second);
  REQUIRE(first->files.size() == 2);
  REQUIRE(second->files.size() == 2);
  CHECK(file(*first, "01_dns.json")->bytes ==
        file(*second, "01_dns.json")->bytes);
  CHECK(file(*first, "00_log.json")->bytes !=
        file(*second, "00_log.json")->bytes);
  CHECK(input.settings.modules.front().source_text ==
        R"({"log":{"loglevel":"debug"}})");
}

TEST_CASE("only selected and explicitly additional nodes enter the nodes tail",
          "[unit][ASM-03]") {
  auto input = base_input();
  enable(input, "outbounds", "03_outbounds.json",
         R"({"outbounds":[{"tag":"direct","protocol":"freedom"}]})");
  enable(
      input, "routing", "04_routing.json",
      R"({"routing":{"rules":[{"type":"field","outboundTag":"direct"}],"balancers":[],"unknown":true}})");
  input.settings.profile.selected = Target{TargetKind::node, "n1"};
  input.settings.profile.additional_node_ids = {"n2", "n1"};
  input.settings.profile.append_selected_default = true;
  input.nodes = {raw_node("n1", "one"), raw_node("n2", "two"),
                 raw_node("unused", "unused")};
  auto built = assemble(input);
  REQUIRE(built);
  REQUIRE(file(*built, "90_nodes_tail.json"));
  const auto tail = Json::parse(file(*built, "90_nodes_tail.json")->bytes);
  REQUIRE(tail.at("outbounds").size() == 2);
  CHECK(built->index.nodes.size() == 2);
  CHECK(built->index.nodes[0].id == "n1");
  CHECK(built->index.nodes[1].id == "n2");
  const auto routing =
      Json::parse(file(*built, "04_routing.json")->bytes).at("routing");
  CHECK(routing.at("rules").size() == 2);
  CHECK(routing.at("rules").back().at("outboundTag") == node_tag("n1"));
  CHECK(routing.at("rules").back().at("network") == "tcp,udp");
  CHECK(routing.at("unknown") == true);
  CHECK_FALSE(tail.dump().find("unused") != std::string::npos);
}

TEST_CASE("native JSON types and nested outbound fields are retained",
          "[unit][ASM-06]") {
  auto input = base_input();
  enable(input, "stats", "08_stats.json", R"({"stats":{}})");
  enable(input, "fakedns", "10_fakedns.json",
         R"({"fakedns":[{"ipPool":"198.18.0.0/15"}]})");
  enable(input, "outbounds", "03_outbounds.json", R"({"outbounds":[]})");
  Node n = raw_node("n1", "ignored");
  std::get<RawOutbound>(n.settings).value =
      Json{{"protocol", "vless"},
           {"settings",
            Json{{"vnext", Json::array({Json{{"address", "127.0.0.1"}}})}}},
           {"streamSettings",
            Json{{"network", "raw"},
                 {"security", "tls"},
                 {"tlsSettings", Json{{"serverName", "localhost"}}}}},
           {"mux", Json{{"enabled", true}}},
           {"sockopt", Json{{"mark", 7}}},
           {"finalmask", Json{{"enabled", true}}}};
  input.nodes.push_back(n);
  input.settings.profile.selected = Target{TargetKind::node, "n1"};
  auto built = assemble(input);
  REQUIRE(built);
  CHECK(Json::parse(file(*built, "08_stats.json")->bytes).at("stats").empty());
  CHECK(Json::parse(file(*built, "10_fakedns.json")->bytes)
            .at("fakedns")
            .is_array());
  const auto outbound = Json::parse(file(*built, "90_nodes_tail.json")->bytes)
                            .at("outbounds")
                            .front();
  CHECK(outbound.at("streamSettings").at("tlsSettings").at("serverName") ==
        "localhost");
  CHECK(outbound.at("mux").at("enabled") == true);
  CHECK(outbound.at("sockopt").at("mark") == 7);
  CHECK(outbound.at("finalmask").at("enabled") == true);
  CHECK_FALSE(outbound.contains("tls"));
  CHECK_FALSE(outbound.contains("groups"));
  CHECK_FALSE(outbound.contains("sniffing"));
}

TEST_CASE("required disabled module prerequisites fail without enabling them",
          "[unit][ASM-07]") {
  auto input = base_input();
  input.settings.profile.selected = Target{TargetKind::node, "n1"};
  input.nodes.push_back(raw_node("n1", "node"));
  input.settings.profile.modules.push_back(
      ModuleSetting{"outbounds", "03_outbounds.json", false});
  auto missing_outbounds = assemble(input);
  CHECK_FALSE(missing_outbounds);

  input.settings.profile.modules = {
      ModuleSetting{"routing", "04_routing.json", false}};
  input.settings.profile.append_selected_default = true;
  auto missing_routing = assemble(input);
  CHECK_FALSE(missing_routing);

  input.settings.profile.selected.reset();
  input.settings.profile.append_selected_default = false;
  auto disabled_unused = assemble(input);
  REQUIRE(disabled_unused);
  CHECK(disabled_unused->files.empty());
}

TEST_CASE("assembly is deterministic, in-memory, and does not mutate inputs",
          "[unit][ASM-08]") {
  auto input = base_input();
  enable(input, "stats", "08_stats.json", R"({"stats":{}})");
  const auto before = input.settings.modules.front().source_text;
  auto first = assemble(input);
  auto second = assemble(input);
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(first->files.size() == second->files.size());
  for (std::size_t i = 0; i < first->files.size(); ++i) {
    CHECK(first->files[i].relative_name == second->files[i].relative_name);
    CHECK(first->files[i].bytes == second->files[i].bytes);
  }
  CHECK(input.settings.modules.front().source_text == before);
}

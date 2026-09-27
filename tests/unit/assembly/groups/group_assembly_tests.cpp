/// @file
/// @brief Checks selector matching and dynamic group expansion into routing.

#include "model/module_catalog.hpp"
#include "xray/codecs/codec.hpp"
#include "xray/runtime_assembler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {
using namespace xrm;

BuildInput base_input() {
  BuildInput input;
  input.settings.profile.id = "p1";
  input.settings.profile.name = "groups";
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

Node raw_node(std::string id, std::string name, std::string protocol = "vless",
              std::optional<std::string> source = {},
              std::vector<std::string> labels = {}) {
  Node node;
  node.id = std::move(id);
  node.original_name = std::move(name);
  node.source_id = std::move(source);
  node.labels = std::move(labels);
  node.settings =
      RawOutbound{Json{{"protocol", protocol}, {"settings", Json::object()}}};
  return node;
}

GroupBinding group(std::string id, std::string tag, NodeSelector selector,
                   SelectionMode mode = SelectionMode::automatic,
                   std::optional<Id> selected = {}) {
  return GroupBinding{std::move(id),
                      "fixture",
                      std::move(selector),
                      mode,
                      std::move(selected),
                      Json{{"tag", std::move(tag)},
                           {"strategy", Json{{"type", "leastPing"}}},
                           {"custom", "preserved"}}};
}
} // namespace

TEST_CASE("GROUP-01 selectors combine fields with AND and values within a "
          "field with OR",
          "[unit][GROUP-01]") {
  auto input = base_input();
  enable(input, "outbounds", "03_outbounds.json", R"({"outbounds":[]})");
  enable(
      input, "routing", "04_routing.json",
      R"({"routing":{"rules":[{"type":"field","balancerTag":"managed"}],"balancers":[]}})");
  NodeFilter filter;
  filter.sources = {"s1", "s2"};
  filter.protocols = {"vless", "trojan"};
  filter.name_contains = {"west", "east"};
  filter.labels = {"fast", "blue"};
  input.settings.bindings.groups.push_back(group("g1", "managed", filter));
  input.nodes = {raw_node("a", "west edge", "vless", "s1", {"fast"}),
                 raw_node("b", "east edge", "trojan", "s2", {"blue"}),
                 raw_node("c", "west edge", "vless", "s3", {"fast"}),
                 raw_node("d", "west edge", "vless", "s1", {"slow"}),
                 raw_node("e", "north edge", "vless", "s1", {"fast"}),
                 raw_node("f", "west disabled", "vless", "s1", {"fast"}),
                 raw_node("g", "west missing", "vless", "s1", {"fast"})};
  input.nodes[5].enabled = false;
  input.nodes[6].source_missing = true;
  auto built = assemble(input);
  REQUIRE(built);
  REQUIRE(built->index.groups.size() == 1);
  CHECK(built->index.groups.front().candidate_ids == std::vector<Id>{"a", "b"});
  REQUIRE(file(*built, "90_nodes_tail.json"));
  const auto tail =
      Json::parse(file(*built, "90_nodes_tail.json")->bytes).at("outbounds");
  REQUIRE(tail.size() == 2);
  CHECK(tail[0].at("tag") == node_tag("a"));
  CHECK(tail[1].at("tag") == node_tag("b"));
}

TEST_CASE("GROUP-02 manual group has a stable singleton selector while loading "
          "candidates",
          "[unit][GROUP-02]") {
  auto input = base_input();
  enable(input, "outbounds", "03_outbounds.json", R"({"outbounds":[]})");
  enable(input, "routing", "04_routing.json",
         R"({"routing":{"rules":[],"balancers":[]}})");
  input.settings.profile.selected = Target{TargetKind::group, "g1"};
  input.settings.profile.append_selected_default = true;
  input.settings.bindings.groups.push_back(group("g1", "managed",
                                                 ExplicitMembers{{"a", "b"}},
                                                 SelectionMode::manual, "b"));
  input.nodes = {raw_node("b", "B"), raw_node("a", "A")};
  auto first = assemble(input);
  auto second = assemble(input);
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(first->index.groups.size() == 1);
  CHECK(first->index.groups.front().candidate_ids == std::vector<Id>{"a", "b"});
  CHECK(first->index.groups.front().selected_node_id == "b");
  auto routing =
      Json::parse(file(*first, "04_routing.json")->bytes).at("routing");
  REQUIRE(routing.at("balancers").size() == 1);
  CHECK(routing.at("balancers")[0].at("selector") ==
        Json::array({node_tag("b")}));
  CHECK(routing.at("balancers")[0].at("strategy").at("type") == "leastPing");
  CHECK_FALSE(routing.at("balancers")[0].at("strategy").at("type") == "manual");
  CHECK(routing.at("rules").back().at("balancerTag") == "managed");
  CHECK(file(*first, "90_nodes_tail.json")->bytes ==
        file(*second, "90_nodes_tail.json")->bytes);
  CHECK(file(*first, "04_routing.json")->bytes ==
        file(*second, "04_routing.json")->bytes);
}

TEST_CASE("GROUP-03 dynamic balancers coexist with native balancers and "
          "conflict by exact tag",
          "[unit][GROUP-03]") {
  auto input = base_input();
  enable(input, "outbounds", "03_outbounds.json", R"({"outbounds":[]})");
  enable(
      input, "routing", "04_routing.json",
      R"({"routing":{"rules":[],"balancers":[{"tag":"native","selector":["x"],"unknown":17}],"domainStrategy":"IPIfNonMatch"}})");
  input.settings.profile.selected = Target{TargetKind::group, "g1"};
  input.settings.bindings.groups.push_back(
      group("g1", "managed", ExplicitMembers{{"a"}}));
  input.settings.bindings.groups.push_back(
      group("g2", "native", ExplicitMembers{{"a"}}));
  input.nodes.push_back(raw_node("a", "A"));
  auto built = assemble(input);
  REQUIRE(built);
  const auto routing =
      Json::parse(file(*built, "04_routing.json")->bytes).at("routing");
  REQUIRE(routing.at("balancers").size() == 2);
  CHECK(routing.at("balancers")[0].at("unknown") == 17);
  CHECK(routing.at("balancers")[1].at("tag") == "managed");
  CHECK(routing.at("domainStrategy") == "IPIfNonMatch");
  CHECK(built->index.static_balancer_tags ==
        std::vector<std::string>{"native"});

  input.settings.bindings.groups[0].balancer["tag"] = "native";
  auto conflict = assemble(input);
  REQUIRE_FALSE(conflict);
  CHECK(conflict.error().code == ErrorCode::conflict);
}

TEST_CASE("GROUP-04 unused empty groups are inert but referenced and invalid "
          "manual groups fail",
          "[unit][GROUP-04]") {
  auto input = base_input();
  enable(input, "outbounds", "03_outbounds.json", R"({"outbounds":[]})");
  enable(input, "routing", "04_routing.json",
         R"({"routing":{"rules":[],"balancers":[]}})");
  input.settings.bindings.groups.push_back(
      group("empty", "empty-tag", ExplicitMembers{}));
  CHECK(assemble(input));

  input.settings.profile.selected = Target{TargetKind::group, "empty"};
  input.settings.profile.append_selected_default = true;
  auto used_empty = assemble(input);
  REQUIRE_FALSE(used_empty);

  input.settings.bindings.groups[0] =
      group("manual", "manual-tag", ExplicitMembers{{"a", "b"}},
            SelectionMode::manual, "missing");
  input.settings.profile.selected = Target{TargetKind::group, "manual"};
  input.nodes = {raw_node("a", "A"), raw_node("b", "B")};
  auto bad_manual = assemble(input);
  REQUIRE_FALSE(bad_manual);
  CHECK((bad_manual.error().code == ErrorCode::not_found ||
         bad_manual.error().code == ErrorCode::invalid_argument));

  input.settings.bindings.groups[0].members = ExplicitMembers{{"gone"}};
  input.settings.bindings.groups[0].selected_node_id = "gone";
  auto missing_member = assemble(input);
  REQUIRE_FALSE(missing_member);
}

TEST_CASE("GROUP-05 default selection preserves rule order and only appends "
          "when requested",
          "[unit][GROUP-05]") {
  auto input = base_input();
  enable(input, "outbounds", "03_outbounds.json", R"({"outbounds":[]})");
  enable(
      input, "routing", "04_routing.json",
      R"({"routing":{"rules":[{"type":"field","outboundTag":"first"},{"type":"field","outboundTag":"catchall","network":"tcp,udp"}],"balancers":[],"unknown":true}})");
  input.settings.bindings.groups.push_back(
      group("g1", "managed", ExplicitMembers{{"a"}}));
  input.settings.profile.selected = Target{TargetKind::group, "g1"};
  input.nodes.push_back(raw_node("a", "A"));
  auto no_append = assemble(input);
  REQUIRE(no_append);
  auto original =
      Json::parse(file(*no_append, "04_routing.json")->bytes).at("routing");
  CHECK(original.at("rules").size() == 2);
  CHECK(original.at("rules")[1].at("outboundTag") == "catchall");

  input.settings.profile.append_selected_default = true;
  auto appended = assemble(input);
  REQUIRE(appended);
  auto changed =
      Json::parse(file(*appended, "04_routing.json")->bytes).at("routing");
  REQUIRE(changed.at("rules").size() == 3);
  CHECK(changed.at("rules")[0].at("outboundTag") == "first");
  CHECK(changed.at("rules")[1].at("outboundTag") == "catchall");
  CHECK(changed.at("rules")[2].at("balancerTag") == "managed");
  CHECK(changed.at("rules")[2].at("network") == "tcp,udp");
  CHECK(changed.at("unknown") == true);
}

TEST_CASE("ASM-03 and ASM-05 observation supplement deduplicates group and "
          "explicit tags",
          "[unit][ASM-03][ASM-05]") {
  auto input = base_input();
  enable(input, "outbounds", "03_outbounds.json", R"({"outbounds":[]})");
  enable(
      input, "routing", "04_routing.json",
      R"({"routing":{"rules":[{"type":"field","balancerTag":"managed"}],"balancers":[]}})");
  enable(
      input, "observatory", "05_observatory.json",
      R"({"observatory":{"subjectSelector":["static", "node-a"],"probeUrl":"http://127.0.0.1/probe","probeInterval":"1m","unknown":{"keep":true}}})");
  input.settings.bindings.groups.push_back(
      group("g1", "managed", ExplicitMembers{{"a", "b"}}));
  input.settings.bindings.observation.append_used_group_members = true;
  input.settings.bindings.observation.additional_node_ids = {"c", "a"};
  input.nodes = {raw_node("a", "A"), raw_node("b", "B"), raw_node("c", "C"),
                 raw_node("unused", "unused")};
  auto built = assemble(input);
  REQUIRE(built);
  REQUIRE(built->index.nodes.size() == 3);
  CHECK(built->index.nodes[0].id == "a");
  CHECK(built->index.nodes[1].id == "b");
  CHECK(built->index.nodes[2].id == "c");
  const auto root = Json::parse(file(*built, "05_observatory.json")->bytes);
  CHECK(root.at("observatory").at("subjectSelector") ==
        Json::array(
            {"static", "node-a", node_tag("a"), node_tag("b"), node_tag("c")}));
  CHECK(root.at("observatory").at("probeUrl") == "http://127.0.0.1/probe");
  CHECK(root.at("observatory").at("probeInterval") == "1m");
  CHECK(root.at("observatory").at("unknown").at("keep") == true);
  CHECK_FALSE(file(*built, "14_burstObservatory.json"));
}

TEST_CASE("ASM-05 burst observatory keeps ping config and two enabled "
          "observers conflict",
          "[unit][ASM-05]") {
  auto input = base_input();
  enable(input, "outbounds", "03_outbounds.json", R"({"outbounds":[]})");
  enable(
      input, "burstObservatory", "14_burstObservatory.json",
      R"({"burstObservatory":{"pingConfig":{"destination":"127.0.0.1"},"subjectSelector":["static"]}})");
  input.settings.bindings.observation.additional_node_ids = {"a"};
  input.nodes.push_back(raw_node("a", "A"));
  auto built = assemble(input);
  REQUIRE(built);
  auto burst = Json::parse(file(*built, "14_burstObservatory.json")->bytes)
                   .at("burstObservatory");
  CHECK(burst.at("subjectSelector") == Json::array({"static", node_tag("a")}));
  CHECK(burst.at("pingConfig").at("destination") == "127.0.0.1");
  CHECK_FALSE(file(*built, "05_observatory.json"));

  enable(input, "observatory", "05_observatory.json",
         R"({"observatory":{"subjectSelector":[]}})");
  auto conflict = assemble(input);
  REQUIRE_FALSE(conflict);
  CHECK(conflict.error().code == ErrorCode::conflict);
}

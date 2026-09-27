// XRM: Checks public interfaces at compile time.

#include "contract/dtos.hpp"
#include "contract/methods.hpp"
#include "profile/forms.hpp"
#include "xray/control.hpp"
#include "xray/metrics.hpp"
#include "xray/runtime_assembler.hpp"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <utility>

static_assert(std::is_default_constructible_v<xrm::GroupBinding>);
static_assert(std::is_default_constructible_v<xrm::ManagerBindings>);
static_assert(std::is_default_constructible_v<xrm::MetricsSample>);
static_assert(!std::is_copy_constructible_v<xrm::MetricsClient>);
static_assert(
    std::is_same_v<decltype(std::declval<const xrm::MetricsClient &>().view()),
                   xrm::MetricsView>);
static_assert(std::is_same_v<
              decltype(std::declval<xrm::LifecycleOptions>().phase_observer),
              std::function<void(xrm::LifecycleOptions::Phase)>>);
static_assert(std::is_same_v<decltype(xrm::write_observation(
                                 std::declval<const xrm::ModuleDocument &>(),
                                 std::declval<const xrm::BuildInput &>(),
                                 std::declval<const xrm::RuntimeIndex &>())),
                             xrm::Result<xrm::OutputFile>>);
static_assert(std::is_same_v<decltype(xrm::plan_traffic_stats(
                                 std::declval<const xrm::ProfileSnapshot &>(),
                                 true, std::string{})),
                             xrm::Result<xrm::ModuleChangePlan>>);
static_assert(xrm::methods::group_save == "group.save");
static_assert(xrm::methods::group_override == "group.override");

TEST_CASE("M3 manager bindings have frozen selector and observation document "
          "shapes") {
  const xrm::GroupBinding group{
      "g1",
      "AUTO",
      xrm::NodeFilter{{"source1"}, {"vless"}, {"edge"}, {"work"}},
      xrm::SelectionMode::manual,
      "node1",
      xrm::Json{{"tag", "auto"},
                {"strategy", {{"type", "leastPing"}}},
                {"x-native-extension", true}}};
  const auto encoded = xrm::Json(group);
  REQUIRE(encoded.at("members").at("kind") == "filter");
  REQUIRE(encoded.at("members").at("name_contains").at(0) == "edge");
  REQUIRE(encoded.at("selection_mode") == "manual");
  REQUIRE(encoded.at("balancer").at("x-native-extension") == true);
  REQUIRE(encoded.get<xrm::GroupBinding>().balancer == group.balancer);

  const xrm::ObservationBinding observation{true, {"node2"}};
  const auto obs_json = xrm::Json(observation);
  REQUIRE(obs_json.size() == 2);
  REQUIRE(obs_json.at("append_used_group_members") == true);
  REQUIRE(obs_json.at("additional_node_ids").at(0) == "node2");
}

#include <catch2/catch_test_macros.hpp>

#include "infra/files.hpp"
#include "infra/store.hpp"
#include "profile/profile_service.hpp"
#include "xray/runtime_assembler.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace {

struct TempRoot {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-groups-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  TempRoot() { std::filesystem::create_directories(path); }
  ~TempRoot() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Fixture {
  TempRoot root;
  std::unique_ptr<xrm::Store> store;
  std::unique_ptr<xrm::ProfileService> profiles;
  xrm::ProfileSettings profile;

  Fixture() {
    auto opened = xrm::Store::open(root.path / "manager.sqlite");
    REQUIRE(opened);
    store = std::move(*opened);
    profiles = std::make_unique<xrm::ProfileService>(*store, root.path / "profiles");
    auto created = profiles->create({"home", "core-system", "local", {}});
    REQUIRE(created);
    profile = *created;
  }
};

xrm::Node node(std::string id, std::string name, bool enabled = true, bool missing = false) {
  xrm::StreamNode stream;
  stream.endpoint = {"127.0.0.1", 443};
  std::get<xrm::VlessSettings>(stream.protocol).id = "uuid";
  return {std::move(id), std::nullopt, std::move(name), std::nullopt,
          {"test", "west"}, enabled, missing, std::move(stream), std::nullopt};
}

xrm::GroupBinding group(std::string id = "group-1") {
  xrm::GroupBinding value;
  value.id = std::move(id);
  value.name = "manual group";
  value.members = xrm::ExplicitMembers{{"node-a", "node-b"}};
  value.selection_mode = xrm::SelectionMode::manual;
  value.balancer = xrm::Json{{"tag", "balancer.custom"},
                             {"strategy", {{"type", "leastPing"}}},
                             {"fallbackTag", "direct"}};
  value.balancer["unknown"] = xrm::Json::object({{"kept", true}});
  return value;
}

}  // namespace

TEST_CASE("BIND-01 group definitions and manual selection persist independently of routing", "[BIND-01]") {
  Fixture fixture;
  REQUIRE(fixture.store->put_node(node("node-a", "alpha")));
  REQUIRE(fixture.store->put_node(node("node-b", "beta")));
  const std::string routing = "{\"routing\":{\"rules\":[{\"domain\":[\"domain:kept.test\"]}],\"unknown\":{\"keep\":true}}}\n";
  auto routing_module = fixture.profiles->module(fixture.profile.id, "routing");
  REQUIRE(routing_module);
  const auto routing_base = routing_module->document
      ? std::optional<std::string>(routing_module->document->source_text) : std::nullopt;
  REQUIRE(fixture.profiles->save_module(fixture.profile.id, {"routing", {routing_base}, routing}));
  auto base = fixture.profiles->groups(fixture.profile.id);
  REQUIRE(base);
  REQUIRE(base->empty());
  auto value = group();
  REQUIRE(fixture.profiles->save_groups(fixture.profile.id, {{std::nullopt}, {value}}));
  REQUIRE(fixture.profiles->select_group_node(fixture.profile.id, "group-1", "node-b"));

  const auto group_file = fixture.root.path / "profiles" / fixture.profile.id / "bindings" / "groups.json";
  REQUIRE(std::filesystem::is_regular_file(group_file));
  const auto serialized = xrm::Json::parse(read(group_file));
  REQUIRE(serialized.size() == 1);
  CHECK(serialized[0]["members"]["node_ids"] == xrm::Json::array({"node-a", "node-b"}));
  CHECK(serialized[0]["selected_node_id"] == "node-b");
  CHECK(serialized[0]["balancer"]["tag"] == "balancer.custom");
  CHECK(serialized[0]["balancer"]["strategy"]["type"] == "leastPing");
  CHECK(serialized[0]["balancer"]["fallbackTag"] == "direct");
  CHECK(serialized[0]["balancer"]["unknown"]["kept"] == true);
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" / "04_routing.json") == routing);
  const auto stale_groups = read(group_file);
  REQUIRE(fixture.profiles->select_group_node(fixture.profile.id, "group-1", "node-a"));
  auto stale_edit = *fixture.profiles->groups(fixture.profile.id);
  stale_edit.front().name = "stale edit";
  const auto conflict = fixture.profiles->save_groups(fixture.profile.id, {{stale_groups}, stale_edit});
  REQUIRE_FALSE(conflict);
  CHECK(conflict.error().code == xrm::ErrorCode::conflict);

  fixture.profiles.reset();
  fixture.store.reset();
  auto reopened = xrm::Store::open(fixture.root.path / "manager.sqlite");
  REQUIRE(reopened);
  xrm::ProfileService profiles(**reopened, fixture.root.path / "profiles");
  auto after = profiles.groups(fixture.profile.id);
  REQUIRE(after);
  REQUIRE(after->size() == 1);
  CHECK(after->front().id == "group-1");
  CHECK(after->front().selected_node_id == "node-a");
  CHECK(after->front().balancer["unknown"]["kept"] == true);
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" / "04_routing.json") == routing);
  auto deletion_base = read(group_file);
  REQUIRE(profiles.save_groups(fixture.profile.id, {{deletion_base}, {}}));
  auto deleted = profiles.groups(fixture.profile.id);
  REQUIRE(deleted);
  CHECK(deleted->empty());
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" / "04_routing.json") == routing);
}

TEST_CASE("BIND-01 manual selection rejects non-candidates and unavailable nodes", "[BIND-01]") {
  Fixture fixture;
  REQUIRE(fixture.store->put_node(node("active", "alpha active")));
  REQUIRE(fixture.store->put_node(node("disabled", "alpha disabled", false)));
  REQUIRE(fixture.store->put_node(node("missing", "alpha missing", true, true)));
  auto value = group();
  value.members = xrm::NodeFilter{{}, {"vmess", "vless"}, {"alpha", "edge"}, {"east", "west"}};
  value.selected_node_id.reset();
  REQUIRE(fixture.profiles->save_groups(fixture.profile.id, {{std::nullopt}, {value}}));

  CHECK_FALSE(fixture.profiles->select_group_node(fixture.profile.id, "unknown", "active"));
  CHECK_FALSE(fixture.profiles->select_group_node(fixture.profile.id, "group-1", "not-a-member"));
  REQUIRE(fixture.profiles->select_group_node(fixture.profile.id, "group-1", "active"));
  CHECK_FALSE(fixture.profiles->select_group_node(fixture.profile.id, "group-1", "disabled"));
  CHECK_FALSE(fixture.profiles->select_group_node(fixture.profile.id, "group-1", "missing"));
  auto selected = fixture.profiles->groups(fixture.profile.id);
  REQUIRE(selected);
  CHECK(selected->front().selected_node_id == "active");
}

TEST_CASE("BIND-01 dynamic source selector unions local and explicit members without copying inventory",
          "[BIND-01][source-binding]") {
  Fixture fixture;
  auto from_a = node("from-a", "Subscribed A");
  from_a.source_id = "source-a";
  auto from_b = node("from-b", "Subscribed B");
  from_b.source_id = "source-b";
  auto omitted = node("omitted", "Other B");
  omitted.source_id = "source-b";
  auto local = node("local", "Local VLESS");
  REQUIRE(fixture.store->put_node(from_a));
  REQUIRE(fixture.store->put_node(from_b));
  REQUIRE(fixture.store->put_node(omitted));
  REQUIRE(fixture.store->put_node(local));

  auto value = group("source-union");
  value.selection_mode = xrm::SelectionMode::automatic;
  value.balancer["strategy"] = xrm::Json{{"type", "random"}};
  xrm::NodeFilter selector;
  selector.sources = {"source-a"};
  selector.include_local = true;
  selector.explicit_node_ids = {"from-b"};
  value.members = selector;
  REQUIRE(fixture.profiles->save_groups(fixture.profile.id, {{std::nullopt}, {value}}));

  auto persisted = fixture.profiles->groups(fixture.profile.id);
  REQUIRE(persisted);
  REQUIRE(persisted->size() == 1);
  const auto* restored = std::get_if<xrm::NodeFilter>(&persisted->front().members);
  REQUIRE(restored);
  CHECK(restored->sources == std::vector<xrm::Id>{"source-a"});
  CHECK(restored->include_local);
  CHECK(restored->explicit_node_ids == std::vector<xrm::Id>{"from-b"});

  auto snapshot = fixture.profiles->snapshot(fixture.profile.id);
  auto inventory = fixture.store->all_nodes();
  REQUIRE(snapshot);
  REQUIRE(inventory);
  snapshot->profile.selected = xrm::Target{xrm::TargetKind::group, value.id};
  snapshot->bindings.groups = *persisted;
  auto files = xrm::assemble(xrm::BuildInput{*snapshot, *inventory});
  REQUIRE(files);
  REQUIRE(files->index.groups.size() == 1);
  CHECK(files->index.groups.front().candidate_ids ==
      std::vector<xrm::Id>{"from-a", "from-b", "local"});
  CHECK(inventory->size() == 4);
  auto routing = std::find_if(files->files.begin(), files->files.end(), [](const auto& file) {
    return file.relative_name == "04_routing.json";
  });
  REQUIRE(routing != files->files.end());
  const auto root = xrm::Json::parse(routing->bytes).at("routing");
  REQUIRE(root.at("balancers").is_array());
  CHECK(root.at("balancers").back().at("selector").size() == 3);

  // An explicit local-only scope must stay dynamic without expanding to all
  // subscribed nodes. An explicit-ID-only scope must not select the rest.
  auto local_only = selector;
  local_only.sources.clear();
  local_only.explicit_node_ids.clear();
  snapshot->bindings.groups.front().members = local_only;
  auto local_files = xrm::assemble(xrm::BuildInput{*snapshot, *inventory});
  REQUIRE(local_files);
  CHECK(local_files->index.groups.front().candidate_ids == std::vector<xrm::Id>{"local"});
  auto explicit_only = selector;
  explicit_only.sources.clear();
  explicit_only.include_local = false;
  snapshot->bindings.groups.front().members = explicit_only;
  auto explicit_files = xrm::assemble(xrm::BuildInput{*snapshot, *inventory});
  REQUIRE(explicit_files);
  CHECK(explicit_files->index.groups.front().candidate_ids == std::vector<xrm::Id>{"from-b"});
}

TEST_CASE("observation binding persists only manager-owned supplement settings", "[BIND-01]") {
  Fixture fixture;
  const auto path = fixture.root.path / "profiles" / fixture.profile.id / "bindings" / "observation.json";
  auto initial = fixture.profiles->observation(fixture.profile.id);
  REQUIRE(initial);
  CHECK_FALSE(initial->append_used_group_members);
  CHECK(initial->additional_node_ids.empty());
  CHECK_FALSE(std::filesystem::exists(path));

  xrm::ObservationBinding binding{true, {"node-a", "node-b"}};
  REQUIRE(fixture.profiles->save_observation(fixture.profile.id, {std::nullopt}, binding));
  const auto raw = xrm::Json::parse(read(path));
  CHECK(raw.size() == 2);
  CHECK(raw.contains("append_used_group_members"));
  CHECK(raw.contains("additional_node_ids"));
  CHECK_FALSE(raw.contains("probeUrl"));
  fixture.profiles.reset();
  fixture.store.reset();
  auto reopened = xrm::Store::open(fixture.root.path / "manager.sqlite");
  REQUIRE(reopened);
  xrm::ProfileService profiles(**reopened, fixture.root.path / "profiles");
  auto persisted = profiles.observation(fixture.profile.id);
  REQUIRE(persisted);
  CHECK(persisted->append_used_group_members);
  CHECK(persisted->additional_node_ids == std::vector<xrm::Id>{"node-a", "node-b"});
  CHECK_FALSE(profiles.save_observation(fixture.profile.id, {std::nullopt}, {}));
}

TEST_CASE("FORM-04 choosing observatory kind preserves both native source documents", "[FORM-04]") {
  Fixture fixture;
  const std::string background = "{\"observatory\":{\"probeUrl\":\"http://127.0.0.1/p\",\"unknown\":7}}\n";
  const std::string burst = "{\"burstObservatory\":{\"pingConfig\":{\"destination\":\"127.0.0.1:80\"},\"unknown\":8}}\n";
  auto background_before = fixture.profiles->module(fixture.profile.id, "observatory");
  auto burst_before = fixture.profiles->module(fixture.profile.id, "burstObservatory");
  REQUIRE(background_before);
  REQUIRE(burst_before);
  REQUIRE(background_before->document);
  REQUIRE(burst_before->document);
  REQUIRE(fixture.profiles->save_module(fixture.profile.id,
      {"observatory", {background_before->document->source_text}, background}));
  REQUIRE(fixture.profiles->save_module(fixture.profile.id,
      {"burstObservatory", {burst_before->document->source_text}, burst}));
  REQUIRE(fixture.profiles->include_module(fixture.profile.id, "observatory", true));
  REQUIRE(fixture.profiles->include_module(fixture.profile.id, "burstObservatory", true));

  REQUIRE(fixture.profiles->choose_observer(fixture.profile.id, "burstObservatory"));
  auto current = fixture.profiles->modules(fixture.profile.id);
  REQUIRE(current);
  const auto background_view = std::find_if(current->begin(), current->end(), [](const auto& item) {
    return item.descriptor.key == "observatory";
  });
  const auto burst_view = std::find_if(current->begin(), current->end(), [](const auto& item) {
    return item.descriptor.key == "burstObservatory";
  });
  REQUIRE(background_view != current->end());
  REQUIRE(burst_view != current->end());
  CHECK_FALSE(background_view->setting.included);
  CHECK(burst_view->setting.included);
  REQUIRE(background_view->document);
  REQUIRE(burst_view->document);
  CHECK(background_view->document->source_text == background);
  CHECK(burst_view->document->source_text == burst);

  REQUIRE(fixture.profiles->choose_observer(fixture.profile.id, std::nullopt));
  current = fixture.profiles->modules(fixture.profile.id);
  REQUIRE(current);
  for (const auto& item : *current) {
    if (item.descriptor.key == "observatory" || item.descriptor.key == "burstObservatory")
      CHECK_FALSE(item.setting.included);
  }
  CHECK_FALSE(fixture.profiles->choose_observer(fixture.profile.id, "metrics"));
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" / "05_observatory.json") == background);
  CHECK(read(fixture.root.path / "profiles" / fixture.profile.id / "modules" / "14_burstObservatory.json") == burst);
}

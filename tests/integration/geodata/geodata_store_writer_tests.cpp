#include "infra/store.hpp"
#include "xray/runtime_writer.hpp"
#include "server/application.hpp"
#include "server/api_router.hpp"
#include "contract/methods.hpp"
#include "infra/files.hpp"

#include <uv.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace xrm;
using namespace std::chrono_literals;

namespace {
struct TempTree {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm-geodata-store-writer-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempTree() { std::filesystem::create_directories(path); }
  ~TempTree() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

std::string bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("P5 geodata metadata persists separately from binary with revision-safe publish") {
  TempTree temp;
  auto store = Store::open(temp.path / "state.sqlite");
  REQUIRE(store);
  GeodataResourceRecord resource;
  resource.id = "geo-cn";
  resource.name = "Chinese geoip";
  resource.kind = GeoAssetKind::geoip;
  resource.file_name = "geoip.dat";
  resource.url = "https://example.invalid/geoip.dat";
  resource.auto_update = true;
  auto created = (*store)->put_geodata_resource(resource);
  REQUIRE(created);
  CHECK(created->settings_revision == 1);
  CHECK_FALSE(created->current);

  GeodataAssetVersion version{resource.id, resource.kind, resource.file_name,
                              "sha256-first", temp.path / "cache-v1",
                              temp.path / "cache-v1/geoip.dat", {"cn"}, 17};
  REQUIRE((*store)->commit_geodata_resource_result(resource.id, 1, version, 101, "success"));
  auto published = (*store)->get_geodata_resource(resource.id);
  REQUIRE(published);
  REQUIRE(published->current);
  CHECK(published->current->version_id == "sha256-first");
  CHECK(published->last_success == 101);

  resource.name = "Renamed";
  auto changed = (*store)->put_geodata_resource(resource);
  REQUIRE(changed);
  CHECK(changed->settings_revision == 2);
  REQUIRE_FALSE((*store)->commit_geodata_resource_result(resource.id, 1, std::nullopt,
                                                         102, "failed"));
  REQUIRE((*store)->commit_geodata_resource_result(resource.id, 2, std::nullopt,
                                                   103, "failed"));
  auto retained = (*store)->get_geodata_resource(resource.id);
  REQUIRE(retained);
  REQUIRE(retained->current);
  CHECK(retained->current->version_id == "sha256-first");
  CHECK(retained->last_success == 101);
  CHECK(retained->last_attempt == 103);

  store->reset();
  auto reopened = Store::open(temp.path / "state.sqlite");
  REQUIRE(reopened);
  auto values = (*reopened)->geodata_resources();
  REQUIRE(values);
  REQUIRE(values->size() == 1);
  CHECK(values->front().current->path == version.path);
  REQUIRE((*reopened)->remove_geodata_resource(resource.id));
  CHECK((*reopened)->geodata_resources()->empty());
}

TEST_CASE("P5 runtime snapshot owns asset bytes independent of cache and exports them") {
  TempTree temp;
  const auto cache_file = temp.path / "cache/geoip.dat";
  std::filesystem::create_directories(cache_file.parent_path());
  { std::ofstream output(cache_file, std::ios::binary); output << "version-one"; }
  PreparedFiles prepared;
  prepared.files.push_back({"04_routing.json", R"({"routing":{"rules":[]}})"});
  RuntimeWriter writer(temp.path / "manager");
  const std::array assets{RuntimeWriter::AssetSnapshot{cache_file, "geoip.dat"}};
  auto runtime = writer.materialize(prepared, DirectoryPurpose::candidate, "run-one", assets);
  REQUIRE(runtime);
  CHECK(bytes(runtime->confdir / "geoip.dat") == "version-one");
  { std::ofstream output(cache_file, std::ios::binary | std::ios::trunc); output << "version-two"; }
  CHECK(bytes(runtime->confdir / "geoip.dat") == "version-one");
  const auto exported = temp.path / "export";
  REQUIRE(writer.export_files(runtime->confdir, exported));
  CHECK(bytes(exported / "geoip.dat") == "version-one");
  CHECK(bytes(exported / "04_routing.json") == prepared.files[0].bytes);
}

TEST_CASE("P5 runtime snapshot rejects missing, linked and colliding assets atomically") {
  TempTree temp;
  PreparedFiles prepared;
  prepared.files.push_back({"04_routing.json", "{}"});
  RuntimeWriter writer(temp.path / "manager");
  const auto source = temp.path / "source.dat";
  { std::ofstream output(source); output << "asset"; }
  const auto link = temp.path / "link.dat";
  std::filesystem::create_symlink(source, link);
  const std::array collision{RuntimeWriter::AssetSnapshot{source, "04_routing.json"}};
  CHECK_FALSE(writer.materialize(prepared, DirectoryPurpose::candidate, "collision", collision));
  CHECK_FALSE(std::filesystem::exists(temp.path / "manager/runtime/candidate/collision"));
  const std::array linked{RuntimeWriter::AssetSnapshot{link, "geoip.dat"}};
  CHECK_FALSE(writer.materialize(prepared, DirectoryPurpose::candidate, "linked", linked));
  CHECK_FALSE(std::filesystem::exists(temp.path / "manager/runtime/candidate/linked"));
  const std::array missing{RuntimeWriter::AssetSnapshot{temp.path / "missing.dat", "geoip.dat"}};
  CHECK_FALSE(writer.materialize(prepared, DirectoryPurpose::candidate, "missing", missing));
  CHECK_FALSE(std::filesystem::exists(temp.path / "manager/runtime/candidate/missing"));
}

TEST_CASE("P5 explicit Profile resource binding snapshots validated cache bytes on render") {
  TempTree temp;
  const auto asset = temp.path / "cache/geoip.dat";
  std::filesystem::create_directories(asset.parent_path());
  { std::ofstream output(asset, std::ios::binary); output << "verified-asset-v1"; }
  auto opened = Store::open(temp.path / "state.sqlite");
  REQUIRE(opened);
  GeodataResourceRecord resource;
  resource.id = "geo-one";
  resource.name = "GeoIP";
  resource.kind = GeoAssetKind::geoip;
  resource.file_name = "geoip.dat";
  resource.url = "https://example.invalid/geoip.dat";
  REQUIRE((*opened)->put_geodata_resource(resource));
  GeodataAssetVersion version{resource.id, resource.kind, resource.file_name, "sha-v1",
                              asset.parent_path(), asset, {"cn"}, 17};
  REQUIRE((*opened)->commit_geodata_resource_result(resource.id, 1, version, 1, "success"));
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    ProcessService processes(loop);
    SubscriptionService subscriptions(**opened, http);
    Application app(**opened, processes, http, loop, temp.path / "data", "test", "boot",
                    subscriptions);
    auto profile = app.profiles().create({"Home", "", "local", {}});
    REQUIRE(profile);
    auto profile_dir = (*opened)->profile_path(profile->id);
    REQUIRE(profile_dir);
    auto text = read_text(*profile_dir / "profile.json", 8 * 1024 * 1024);
    REQUIRE(text);
    profile->geodata_resource_ids["geoip.dat"] = resource.id;
    profile->selected = Target{TargetKind::native_outbound, "direct"};
    REQUIRE(app.profiles().save(profile->id, ProfileEdit{{*text}, *profile}));
    ApiRouter router(app);
    std::optional<RpcResponse> response;
    router.dispatch("viewer", RpcRequest{1, 7, std::string(methods::geodata_list), Json::object()},
                    [&](RpcResponse value) { response = std::move(value); });
    REQUIRE(response);
    REQUIRE(response->ok);
    REQUIRE(response->result);
    CHECK(response->result->size() == 1);
    CHECK_FALSE(response->result->at(0).contains("url"));
    response.reset();
    router.dispatch("viewer", RpcRequest{1, 8, std::string(methods::profile_geodata_set),
                    Json{{"profile_id", profile->id}, {"resources", Json{{"geoip.dat", resource.id}}}}},
                    [&](RpcResponse value) { response = std::move(value); });
    REQUIRE(response);
    CHECK(response->ok);
    auto rendered = app.render(profile->id);
    const auto render_error = rendered ? std::string{} : rendered.error().message;
    INFO(render_error);
    REQUIRE(rendered);
    CHECK(bytes(rendered->confdir / "geoip.dat") == "verified-asset-v1");
    CHECK_FALSE(app.remove_geodata_resource(resource.id));
    { std::ofstream output(asset, std::ios::trunc); output << "later-cache-update"; }
    CHECK(bytes(rendered->confdir / "geoip.dat") == "verified-asset-v1");
    auto updated = app.profiles().get(profile->id);
    REQUIRE(updated);
    auto new_text = read_text(*profile_dir / "profile.json", 8 * 1024 * 1024);
    REQUIRE(new_text);
    updated->environment.asset_dir = temp.path / "custom-assets";
    REQUIRE(app.profiles().save(profile->id, ProfileEdit{{*new_text}, *updated}));
    auto conflict = app.render(profile->id);
    CHECK_FALSE(conflict);
    if (!conflict) CHECK(conflict.error().code == ErrorCode::conflict);
    bool drained = false;
    app.shutdown([&] { drained = true; });
    while (!drained) uv_run(&loop, UV_RUN_ONCE);
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("P5 preset RPC previews native JSON and saves only after explicit confirmation") {
  TempTree temp;
  auto opened = Store::open(temp.path / "state.sqlite");
  REQUIRE(opened);
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    ProcessService processes(loop);
    SubscriptionService subscriptions(**opened, http);
    Application app(**opened, processes, http, loop, temp.path / "data", "test", "boot",
                    subscriptions);
    auto profile = app.profiles().create({"Home", "", "local", {}});
    REQUIRE(profile);
    auto routing = app.profiles().module(profile->id, "routing");
    REQUIRE(routing);
    REQUIRE(routing->document);
    const auto before = routing->document->source_text;
    auto plan = app.plan_routing_preset(profile->id, "direct_only");
    REQUIRE(plan);
    CHECK(plan->at("expected_text") == before);
    CHECK(plan->at("after").at("routing").at("rules").is_array());
    auto unchanged = app.profiles().module(profile->id, "routing");
    REQUIRE(unchanged);
    REQUIRE(unchanged->document);
    CHECK(unchanged->document->source_text == before);
    CHECK_FALSE(app.save_routing_preset(profile->id, "direct_only", "stale-base"));
    auto saved = app.save_routing_preset(profile->id, "direct_only", before);
    REQUIRE(saved);
    auto after = app.profiles().module(profile->id, "routing");
    REQUIRE(after);
    REQUIRE(after->document);
    CHECK(Json::parse(after->document->source_text) == plan->at("after"));
    bool drained = false;
    app.shutdown([&] { drained = true; });
    while (!drained) uv_run(&loop, UV_RUN_ONCE);
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(uv_loop_close(&loop) == 0);
}

TEST_CASE("P5 geodata auto-update runs only for an explicitly configured interval") {
  TempTree temp;
  const auto local_file = temp.path / "source.dat";
  // A minimal structurally valid GeoIPList; the timer test uses /bin/true only
  // for scheduler wiring. Separate real-core tests validate Xray semantics.
  const auto field = [](char tag, std::string_view value) {
    return std::string{tag, static_cast<char>(value.size())} + std::string(value);
  };
  const auto cidr = field('\x0a', std::string{"\x7f\0\0\0", 4}) + std::string{"\x10\x08", 2};
  const auto content = field('\x0a', field('\x0a', "XRM-TEST") + field('\x12', cidr));
  { std::ofstream output(local_file, std::ios::binary); output << content; }
  auto opened = Store::open(temp.path / "state.sqlite");
  REQUIRE(opened);
  REQUIRE((*opened)->put_core(CoreInstall{"validator", "fixture", "/bin/true", "test", false}));
  uv_loop_t loop{};
  REQUIRE(uv_loop_init(&loop) == 0);
  {
    HttpClient http(loop);
    ProcessService processes(loop);
    SubscriptionService subscriptions(**opened, http);
    Application app(**opened, processes, http, loop, temp.path / "data", "test", "boot",
                    subscriptions);
    GeodataResourceRecord disabled;
    disabled.id = "no-interval";
    disabled.name = "not scheduled";
    disabled.kind = GeoAssetKind::geoip;
    disabled.file_name = "geoip.dat";
    disabled.local_path = local_file;
    disabled.auto_update = true;
    disabled.validator_core_id = "validator";
    REQUIRE(app.put_geodata_resource(disabled));
    auto enabled = disabled;
    enabled.id = "with-interval";
    enabled.name = "scheduled";
    enabled.update_interval = std::chrono::seconds{1};
    REQUIRE(app.put_geodata_resource(enabled));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    bool acquired = false;
    while (std::chrono::steady_clock::now() < deadline) {
      uv_run(&loop, UV_RUN_NOWAIT);
      auto resource = app.get_geodata_resource(enabled.id);
      REQUIRE(resource);
      if (resource->current) { acquired = true; break; }
      std::this_thread::sleep_for(10ms);
    }
    CHECK(acquired);
    auto never = app.get_geodata_resource(disabled.id);
    REQUIRE(never);
    CHECK_FALSE(never->current);
    bool drained = false;
    app.shutdown([&] { drained = true; });
    while (!drained) uv_run(&loop, UV_RUN_ONCE);
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  CHECK(uv_loop_close(&loop) == 0);
}

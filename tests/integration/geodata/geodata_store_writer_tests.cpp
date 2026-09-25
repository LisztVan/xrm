#include "infra/store.hpp"
#include "xray/runtime_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>

using namespace xrm;

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

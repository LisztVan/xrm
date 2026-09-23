#include <catch2/catch_test_macros.hpp>

#include "xray/runtime_writer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace {

struct TempDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("xrm runtime writer " + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string> filenames(const std::filesystem::path& directory) {
  std::vector<std::string> result;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    result.push_back(entry.path().filename().string());
  }
  std::sort(result.begin(), result.end());
  return result;
}

xrm::PreparedFiles files(std::initializer_list<xrm::OutputFile> output) {
  xrm::PreparedFiles prepared;
  prepared.files.assign(output.begin(), output.end());
  return prepared;
}

}  // namespace

TEST_CASE("WRITE-01 materializes every FileSet into a fresh directory", "[WRITE-01]") {
  TempDirectory temp;
  xrm::RuntimeWriter writer(temp.path);

  auto first = writer.materialize(
      files({{"15_burstObservatory.json", "{\"burstObservatory\":{}}"}}),
      xrm::DirectoryPurpose::candidate, "run-a");
  REQUIRE(first);
  REQUIRE(filenames(first->confdir) == std::vector<std::string>{"15_burstObservatory.json"});

  auto second = writer.materialize(
      files({{"14_observatory.json", "{\"observatory\":{\"probeURL\":\"x\"}}"}}),
      xrm::DirectoryPurpose::candidate, "run-b");
  REQUIRE(second);
  REQUIRE(filenames(second->confdir) == std::vector<std::string>{"14_observatory.json"});
  REQUIRE(read_file(second->confdir / "14_observatory.json") ==
          "{\"observatory\":{\"probeURL\":\"x\"}}");
}

TEST_CASE("WRITE-02 candidate files are isolated from active files", "[WRITE-02]") {
  TempDirectory temp;
  xrm::RuntimeWriter writer(temp.path);
  auto active = writer.materialize(
      files({{"03_outbounds.json", "active bytes"}}),
      xrm::DirectoryPurpose::candidate, "active");
  REQUIRE(active);
  const auto active_path = active->confdir / "03_outbounds.json";
  struct stat before {};
  REQUIRE(::stat(active_path.c_str(), &before) == 0);

  auto candidate = writer.materialize(
      files({{"03_outbounds.json", "candidate bytes"}}),
      xrm::DirectoryPurpose::candidate, "candidate");
  REQUIRE(candidate);
  const auto candidate_path = candidate->confdir / "03_outbounds.json";
  REQUIRE(read_file(candidate_path) == "candidate bytes");
  struct stat candidate_stat {};
  REQUIRE(::stat(candidate_path.c_str(), &candidate_stat) == 0);
  REQUIRE((candidate_stat.st_dev != before.st_dev || candidate_stat.st_ino != before.st_ino));

  {
    std::ofstream changed(candidate_path, std::ios::binary | std::ios::trunc);
    REQUIRE(changed.good());
    changed << "changed candidate";
  }
  struct stat after {};
  REQUIRE(::stat(active_path.c_str(), &after) == 0);
  REQUIRE(after.st_dev == before.st_dev);
  REQUIRE(after.st_ino == before.st_ino);
  REQUIRE(read_file(active_path) == "active bytes");
}

TEST_CASE("WRITE-03 invalid FileSet leaves active input intact and removes partial output",
          "[WRITE-03]") {
  TempDirectory temp;
  xrm::RuntimeWriter writer(temp.path);
  auto active = writer.materialize(
      files({{"04_routing.json", "active routing"}}),
      xrm::DirectoryPurpose::candidate, "active");
  REQUIRE(active);
  const auto active_path = active->confdir / "04_routing.json";
  struct stat before {};
  REQUIRE(::stat(active_path.c_str(), &before) == 0);

  auto collision = writer.materialize(
      files({{"03_outbounds.json", "must not replace active"}}),
      xrm::DirectoryPurpose::candidate, "active");
  REQUIRE_FALSE(collision);
  REQUIRE(collision.error().code == xrm::ErrorCode::conflict);
  REQUIRE(read_file(active_path) == "active routing");

  auto invalid = writer.materialize(
      files({{"03_outbounds.json", "partial candidate"}, {"../escape.json", "escape"}}),
      xrm::DirectoryPurpose::candidate, "invalid");
  REQUIRE_FALSE(invalid);
  REQUIRE_FALSE(std::filesystem::exists(temp.path / "runtime" / "candidate" / "invalid"));
  REQUIRE_FALSE(std::filesystem::exists(temp.path / "runtime" / "candidate" / "escape.json"));
  struct stat after {};
  REQUIRE(::stat(active_path.c_str(), &after) == 0);
  REQUIRE(after.st_dev == before.st_dev);
  REQUIRE(after.st_ino == before.st_ino);
  REQUIRE(read_file(active_path) == "active routing");

  auto protected_dir = writer.materialize(
      files({{"stats.json", "{}"}}), xrm::DirectoryPurpose::candidate, "keep-run");
  REQUIRE(protected_dir);
  const std::array<xrm::Id, 1> protected_ids{"keep-run"};
  auto protected_result = writer.discard(*protected_dir, protected_ids);
  REQUIRE_FALSE(protected_result);
  REQUIRE(protected_result.error().code == xrm::ErrorCode::conflict);
  REQUIRE(std::filesystem::exists(protected_dir->confdir));
  const std::array<xrm::Id, 0> unprotected_ids{};
  REQUIRE(writer.discard(*protected_dir, unprotected_ids));
  REQUIRE_FALSE(std::filesystem::exists(protected_dir->confdir));
}

TEST_CASE("WRITE-04 exports exact multi-file contents without overwriting a destination",
          "[WRITE-04]") {
  TempDirectory temp;
  xrm::RuntimeWriter writer(temp.path / "manager-data");
  const std::string outbound =
      "{\"outbounds\":[{\"streamSettings\":{\"tlsSettings\":{\"certificates\":["
      "{\"certificateFile\":\"/external/client.crt\"}]}}}]}\n// user comment\n";
  const std::string tail = "{\"outbounds\":[{\"tag\":\"managed\"}]}";
  auto source = writer.materialize(
      files({{"03_outbounds.json", outbound}, {"90_nodes_tail.json", tail}}),
      xrm::DirectoryPurpose::preview, "preview-1");
  REQUIRE(source);

  const auto exported = temp.path / "export";
  REQUIRE(writer.export_files(source->confdir, exported));
  REQUIRE(filenames(exported) ==
          std::vector<std::string>{"03_outbounds.json", "90_nodes_tail.json"});
  REQUIRE(read_file(exported / "03_outbounds.json") == outbound);
  REQUIRE(read_file(exported / "90_nodes_tail.json") == tail);
  REQUIRE_FALSE(std::filesystem::exists(exported / "external" / "client.crt"));

  std::filesystem::create_directories(temp.path / "nonempty");
  std::ofstream(temp.path / "nonempty" / "keep.txt") << "keep";
  auto denied = writer.export_files(source->confdir, temp.path / "nonempty");
  REQUIRE_FALSE(denied);
  REQUIRE(read_file(temp.path / "nonempty" / "keep.txt") == "keep");
}

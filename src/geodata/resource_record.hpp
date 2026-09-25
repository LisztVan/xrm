#pragma once

#include "geodata/resource_validation.hpp"

#include <filesystem>
#include <optional>
#include <set>
#include <string>

namespace xrm {

struct GeodataAssetVersion {
  Id resource_id;
  GeoAssetKind kind = GeoAssetKind::geoip;
  std::string file_name;
  std::string version_id;  // SHA-256 of validated bytes.
  std::filesystem::path asset_dir;
  std::filesystem::path path;
  std::set<std::string> labels;
  std::size_t bytes = 0;
};

// Shared resource metadata only. Binary assets remain in the immutable cache.
struct GeodataResourceRecord {
  Id id;
  std::string name;
  GeoAssetKind kind = GeoAssetKind::geoip;
  std::string file_name;
  std::string url;
  std::filesystem::path local_path;
  bool auto_update = false;
  std::uint64_t settings_revision = 0;
  std::optional<GeodataAssetVersion> current;
  UnixMillis last_attempt = 0;
  UnixMillis last_success = 0;
  std::string last_result;
};

}  // namespace xrm

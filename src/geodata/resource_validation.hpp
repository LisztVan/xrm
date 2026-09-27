/// @file
/// @brief Declares geodata validation results and entry points.

#pragma once

#include "model/runtime.hpp"

#include <set>
#include <string>
#include <string_view>

namespace xrm {

enum class GeoAssetKind { geoip, geosite };

struct GeoAssetInspection {
  GeoAssetKind kind = GeoAssetKind::geoip;
  std::set<std::string> labels; // normalized lowercase, as used in Xray rules
  std::size_t entries = 0;
};

// Narrow wire inspection for v26.9.9 GeoIPList/GeoSiteList entries. This
// catches empty, truncated and wrong-type files that Xray's `run -test` may
// accept when a matching label is present; actual Xray validation is still
// required before publishing a resource version.
Result<GeoAssetInspection> inspect_geodata_bytes(GeoAssetKind,
                                                 std::string_view bytes);

// Builds a one-file, loopback-free Xray config that forces the fixed core to
// load each required label from the nominated asset. This function does not
// claim to validate bytes itself: the caller must write the FileSet to an
// isolated confdir and run the selected Xray binary with XRAY_LOCATION_ASSET
// pointing to the staged resource directory, then check its real exit status.
Result<FileSet>
geodata_validation_files(GeoAssetKind, std::string file_name,
                         const std::set<std::string> &required_labels);

} // namespace xrm

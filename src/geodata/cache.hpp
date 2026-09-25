#pragma once

#include "geodata/resource_record.hpp"
#include "infra/http.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <set>

struct uv_loop_s;

namespace xrm {

class ProcessService;

struct GeodataAcquireRequest {
  Id resource_id;
  GeoAssetKind kind = GeoAssetKind::geoip;
  std::string file_name;
  std::string url;
  std::set<std::string> required_labels;
  std::filesystem::path validator_binary;
  std::map<std::string, std::string> headers;
  DownloadRoute route;
  Milliseconds timeout{30000};
  std::size_t max_body_bytes = 64 * 1024 * 1024;
};

// Downloads and validates into a private immutable cache. The Server persists
// a successful version reference in its existing Store; this class never
// changes Profile/modules or an active RunSpec.
class GeodataCache {
 public:
  GeodataCache(HttpClient&, ProcessService&, uv_loop_s&, std::filesystem::path cache_root);
  ~GeodataCache();
  GeodataCache(const GeodataCache&) = delete;
  GeodataCache& operator=(const GeodataCache&) = delete;

  Result<Id> acquire(GeodataAcquireRequest, Completion<GeodataAssetVersion>);
  Result<void> cancel(const Id& operation_id);
  // Call and drain on the Server loop before destroying HttpClient or
  // ProcessService; outstanding network/check callbacks retain their jobs.
  void shutdown(std::function<void()> drained);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xrm

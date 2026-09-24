#pragma once

#include "model/runtime.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xrm {

class HttpClient;
class ProcessService;

struct ReleaseAsset {
  std::string name;
  std::string url;
  std::string platform;
  std::string architecture;
  std::optional<std::string> sha256;
};
struct CoreRelease {
  std::string tag;
  UnixMillis published_at = 0;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;
};
struct InstallRequest {
  CoreRelease release;
  std::string asset_name;
  bool allow_unverified_checksum = false;
};

void to_json(Json&, const ReleaseAsset&);
void from_json(const Json&, ReleaseAsset&);
void to_json(Json&, const CoreRelease&);
void from_json(const Json&, CoreRelease&);
void to_json(Json&, const InstallRequest&);
void from_json(const Json&, InstallRequest&);

class CoreInstaller {
 public:
  CoreInstaller(HttpClient&, ProcessService&, std::filesystem::path cores_root);
  ~CoreInstaller();
  CoreInstaller(const CoreInstaller&) = delete;
  CoreInstaller& operator=(const CoreInstaller&) = delete;

  Result<Id> releases(bool include_prerelease,
                      Completion<std::vector<CoreRelease>> completion);
  Result<Id> install(InstallRequest request,
                     Completion<CoreInstall> completion);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xrm

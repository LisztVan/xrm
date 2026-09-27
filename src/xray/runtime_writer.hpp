// XRM: Declares runtime confdir writing and validation.

#pragma once

#include "model/runtime.hpp"

#include <filesystem>
#include <span>
#include <string>

namespace xrm {

class RuntimeWriter {
public:
  struct AssetSnapshot {
    std::filesystem::path immutable_source;
    std::string runtime_name;
  };
  explicit RuntimeWriter(std::filesystem::path data_root);
  Result<PreparedDirectory>
  materialize(const PreparedFiles &, DirectoryPurpose, const Id &,
              std::span<const AssetSnapshot> assets = {});
  Result<void> export_files(const std::filesystem::path &confdir,
                            const std::filesystem::path &new_destination);
  Result<void> discard(const PreparedDirectory &,
                       std::span<const Id> protected_run_ids);

private:
  std::filesystem::path data_root_;
};

} // namespace xrm

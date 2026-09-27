/// @file
/// @brief Declares runtime confdir writing and validation.

#pragma once

#include "model/runtime.hpp"

#include <filesystem>
#include <span>
#include <string>

namespace xrm {

/// @brief Materializes and exports complete native confdir file sets.
///
/// This is the filesystem boundary after pure assembly. Generated output is
/// never written back into a Profile's native module Source.
class RuntimeWriter {
public:
  struct AssetSnapshot {
    std::filesystem::path immutable_source;
    std::string runtime_name;
  };
  explicit RuntimeWriter(std::filesystem::path data_root);
  /// Writes one fresh preview/candidate directory, rejecting duplicate file
  /// names and invalid asset sources before publication.
  Result<PreparedDirectory>
  materialize(const PreparedFiles &, DirectoryPurpose, const Id &,
              std::span<const AssetSnapshot> assets = {});
  Result<void> export_files(const std::filesystem::path &confdir,
                            const std::filesystem::path &new_destination);
  /// Removes only a directory owned by this writer and not protected by a
  /// live or previous run ID.
  Result<void> discard(const PreparedDirectory &,
                       std::span<const Id> protected_run_ids);

private:
  std::filesystem::path data_root_;
};

} // namespace xrm

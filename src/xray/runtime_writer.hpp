#pragma once

#include "model/runtime.hpp"

namespace xrm {

class RuntimeWriter {
 public:
  explicit RuntimeWriter(std::filesystem::path data_root);
  Result<PreparedDirectory> materialize(const PreparedFiles&, DirectoryPurpose,
                                        const Id&);
  Result<void> export_files(const std::filesystem::path& confdir,
                            const std::filesystem::path& new_destination);
  Result<void> discard(const PreparedDirectory&,
                       std::span<const Id> protected_run_ids);
};

}  // namespace xrm

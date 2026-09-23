#pragma once

#include "model/runtime.hpp"

namespace xrm {

ProcessSpec version_command(const std::filesystem::path& binary);
Result<std::string> read_core_version(const ProcessResult& result);

}  // namespace xrm

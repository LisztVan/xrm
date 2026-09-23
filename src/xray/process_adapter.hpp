#pragma once

#include "model/runtime.hpp"

namespace xrm {

ProcessSpec version_command(const std::filesystem::path& binary);
ProcessSpec check_command(const RunSpec& run);
ProcessSpec run_command(const RunSpec& run);
ProcessSpec dump_command(const RunSpec& run);
Result<std::string> read_core_version(const ProcessResult& result);

}  // namespace xrm

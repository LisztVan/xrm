#pragma once

#include "model/common.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct uv_loop_s;
using uv_loop_t = uv_loop_s;

namespace xrm {

struct CoreInstall {
  Id id;
  std::string name;
  std::filesystem::path path;
  std::string reported_version;
  bool managed = false;
};

struct ProcessSpec {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::filesystem::path cwd;
  std::map<std::string, std::string> environment;
};

using ProcessId = std::uint64_t;
struct ProcessExit {
  int exit_code = 0;
  int term_signal = 0;
};
struct ProcessResult {
  ProcessExit exit;
  std::string stdout_text;
  std::string stderr_text;
  bool output_truncated = false;
};

}  // namespace xrm

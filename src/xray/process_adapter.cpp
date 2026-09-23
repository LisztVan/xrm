#include "xray/process_adapter.hpp"

#include <algorithm>
#include <cctype>

namespace xrm {

ProcessSpec version_command(const std::filesystem::path& binary) {
  return {binary, {"version"}, {}, {}};
}

Result<std::string> read_core_version(const ProcessResult& result) {
  if (result.exit.term_signal != 0 || result.exit.exit_code != 0) {
    return std::unexpected(AppError{ErrorCode::core_check_failed,
                                    "Xray version命令未成功退出",
                                    std::nullopt,
                                    result.stderr_text.empty()
                                        ? std::nullopt
                                        : std::optional<std::string>(result.stderr_text)});
  }
  auto version = result.stdout_text;
  const auto first_line = version.find_first_of("\r\n");
  if (first_line != std::string::npos) version.resize(first_line);
  const auto first = std::find_if_not(version.begin(), version.end(),
                                      [](unsigned char ch) { return std::isspace(ch); });
  const auto last = std::find_if_not(version.rbegin(), version.rend(),
                                     [](unsigned char ch) { return std::isspace(ch); }).base();
  if (first >= last) {
    return std::unexpected(AppError{ErrorCode::core_check_failed,
                                    "Xray version命令没有返回版本文本",
                                    std::nullopt, std::nullopt});
  }
  version = std::string(first, last);
  if (version.find("Xray") == std::string::npos) {
    return std::unexpected(AppError{ErrorCode::core_check_failed,
                                    "version输出未标识Xray核心",
                                    std::nullopt, std::nullopt});
  }
  return version;
}

}  // namespace xrm

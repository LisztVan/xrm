// XRM: Declares filesystem helpers for atomic and bounded file access.

#pragma once

#include "model/profile.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace xrm {

Result<std::string> read_text(const std::filesystem::path &,
                              std::size_t max_bytes);
Result<void> replace_text(const std::filesystem::path &,
                          std::string_view bytes);
Result<void> write_if_unchanged(const std::filesystem::path &, const EditBase &,
                                std::string_view bytes);

} // namespace xrm

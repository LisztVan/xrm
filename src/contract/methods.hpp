#pragma once

#include <string_view>

namespace xrm::methods {

inline constexpr std::string_view server_info = "server.info";
inline constexpr std::string_view core_register = "core.register";
inline constexpr std::string_view core_list = "core.list";
inline constexpr std::string_view node_add = "node.add";
inline constexpr std::string_view node_list = "node.list";

}  // namespace xrm::methods

#pragma once

#include <string_view>

namespace xrm::methods {

inline constexpr std::string_view server_info = "server.info";
inline constexpr std::string_view core_register = "core.register";
inline constexpr std::string_view core_list = "core.list";
inline constexpr std::string_view node_add = "node.add";
inline constexpr std::string_view node_list = "node.list";
inline constexpr std::string_view node_get = "node.get";
inline constexpr std::string_view node_copy = "node.copy";
inline constexpr std::string_view node_set_enabled = "node.set_enabled";
inline constexpr std::string_view node_set_labels = "node.set_labels";
inline constexpr std::string_view node_set_alias = "node.set_alias";
inline constexpr std::string_view subscription_list = "subscription.list";
inline constexpr std::string_view subscription_get = "subscription.get";
inline constexpr std::string_view subscription_add = "subscription.add";
inline constexpr std::string_view subscription_replace = "subscription.replace";
inline constexpr std::string_view subscription_remove = "subscription.remove";
inline constexpr std::string_view subscription_refresh = "subscription.refresh";
inline constexpr std::string_view profile_create = "profile.create";
inline constexpr std::string_view profile_list = "profile.list";
inline constexpr std::string_view profile_get = "profile.get";
inline constexpr std::string_view profile_save = "profile.save";
inline constexpr std::string_view profile_clone = "profile.clone";
inline constexpr std::string_view profile_remove = "profile.remove";
inline constexpr std::string_view module_list = "module.list";
inline constexpr std::string_view module_get = "module.get";
inline constexpr std::string_view module_save = "module.save";
inline constexpr std::string_view module_include = "module.include";
inline constexpr std::string_view module_rename_file = "module.rename_file";
inline constexpr std::string_view connection_select = "connection.select";
inline constexpr std::string_view connection_connect = "connection.connect";
inline constexpr std::string_view connection_disconnect = "connection.disconnect";
inline constexpr std::string_view config_render = "config.render";
inline constexpr std::string_view config_check = "config.check";
inline constexpr std::string_view config_apply = "config.apply";
inline constexpr std::string_view config_dump = "config.dump";
inline constexpr std::string_view config_files = "config.files";
inline constexpr std::string_view config_export = "config.export";
inline constexpr std::string_view core_start = "core.start";
inline constexpr std::string_view core_restart = "core.restart";
inline constexpr std::string_view core_stop = "core.stop";
inline constexpr std::string_view runtime_status = "runtime.status";
inline constexpr std::string_view logs_read = "logs.read";
inline constexpr std::string_view operation_get = "operation.get";

}  // namespace xrm::methods

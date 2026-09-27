/// @file
/// @brief Registers connection, status, log, and runtime commands.

#include "cli/command_options.hpp"

#include "contract/methods.hpp"

namespace xrm::cli {

void add_runtime_commands(CLI::App &app, Request &request) {
  auto *group =
      app.add_subcommand("group", "Manage saved dynamic Xray balancers");
  auto *group_add = group->add_subcommand(
      "add", "Create a managed group with explicit node candidates");
  group_add->add_option("profile_id", request.profile_id)->required();
  group_add->add_option("--name", request.group_name)->required();
  group_add->add_option("--tag", request.group_tag)->required();
  group_add->add_option("--mode", request.group_mode)
      ->check(CLI::IsMember({"automatic", "manual"}));
  group_add
      ->add_option("--strategy", request.group_strategy,
                   "Xray strategy; leastLoad is experimental and has not been "
                   "verified against the fixed core")
      ->check(
          CLI::IsMember({"random", "roundRobin", "leastPing", "leastLoad"}));
  group_add->add_option("--members", request.group_members)->required();
  group_add->callback([&] {
    request.method = std::string(methods::group_save);
    request.params = {
        {"profile_id", request.profile_id},
        {"group",
         {{"id", ""},
          {"name", request.group_name},
          {"members",
           {{"kind", "explicit"}, {"node_ids", request.group_members}}},
          {"selection_mode", request.group_mode},
          {"selected_node_id", nullptr},
          {"balancer",
           {{"tag", request.group_tag},
            {"strategy", {{"type", request.group_strategy}}}}}}}};
  });
  auto *group_list = group->add_subcommand("list", "List groups in a Profile");
  group_list->add_option("profile_id", request.profile_id)->required();
  group_list->callback([&] {
    request.method = std::string(methods::group_list);
    request.params = {{"profile_id", request.profile_id}};
  });
  auto *group_show = group->add_subcommand("show", "Show one saved group");
  group_show->add_option("profile_id", request.profile_id)->required();
  group_show->add_option("group_id", request.group_id)->required();
  group_show->callback([&] {
    request.method = std::string(methods::group_get);
    request.params = {{"profile_id", request.profile_id},
                      {"group_id", request.group_id}};
  });
  auto *group_edit = group->add_subcommand(
      "edit", "Replace one group from its complete JSON object");
  group_edit->add_option("profile_id", request.profile_id)->required();
  group_edit->add_option("--json", request.group_json)->required();
  group_edit->callback([&] {
    request.method = std::string(methods::group_save);
    try {
      request.params = {{"profile_id", request.profile_id},
                        {"group", Json::parse(request.group_json)}};
    } catch (const Json::exception &) {
      request.params =
          Json{{"profile_id", request.profile_id}, {"group", nullptr}};
    }
  });
  auto *group_remove = group->add_subcommand("remove", "Remove a saved group");
  group_remove->add_option("profile_id", request.profile_id)->required();
  group_remove->add_option("group_id", request.group_id)->required();
  group_remove->callback([&] {
    request.method = std::string(methods::group_remove);
    request.params = {{"profile_id", request.profile_id},
                      {"group_id", request.group_id}};
  });
  auto *group_select = group->add_subcommand(
      "select", "Persist a manual group's chosen candidate");
  group_select->add_option("profile_id", request.profile_id)->required();
  group_select->add_option("group_id", request.group_id)->required();
  group_select->add_option("node_id", request.node_id)->required();
  group_select->callback([&] {
    request.method = std::string(methods::group_select);
    request.params = {{"profile_id", request.profile_id},
                      {"group_id", request.group_id},
                      {"node_id", request.node_id}};
  });
  auto *group_override = group->add_subcommand(
      "override", "Temporarily override a running Xray balancer selection");
  group_override->add_option("balancer", request.override_balancer)->required();
  group_override->add_option("outbound", request.override_outbound);
  group_override->add_flag("--runtime", request.runtime_only,
                           "Required: affects only the current running Xray");
  group_override->add_flag("--clear", request.clear_override,
                           "Clear the current runtime override");
  group_override->callback([&] {
    if (!request.runtime_only)
      throw CLI::ValidationError("group override", "--runtime is required");
    if (!request.clear_override && request.override_outbound.empty())
      throw CLI::ValidationError("group override",
                                 "outbound is required unless --clear is used");
    request.method =
        std::string(request.clear_override ? methods::group_override_clear
                                           : methods::group_override);
    request.params = {{"runtime", true},
                      {"balancer", request.override_balancer}};
    if (!request.clear_override)
      request.params["outbound"] = request.override_outbound;
  });

  auto *select = app.add_subcommand(
      "select", "Persist a node selection without connecting");
  auto *select_node =
      select->add_subcommand("node", "Select a saved node for a Profile");
  select_node->add_option("profile_id", request.profile_id)->required();
  select_node->add_option("node_id", request.target_id)->required();
  select_node->callback([&] {
    request.method = std::string(methods::connection_select);
    request.params = {
        {"profile_id", request.profile_id},
        {"target", Json{{"kind", "node"}, {"id_or_tag", request.target_id}}}};
  });
  auto *select_group = select->add_subcommand(
      "group", "Persist a group selection without connecting");
  select_group->add_option("profile_id", request.profile_id)->required();
  select_group->add_option("group_id", request.target_id)->required();
  select_group->callback([&] {
    request.method = std::string(methods::connection_select);
    request.params = {
        {"profile_id", request.profile_id},
        {"target", Json{{"kind", "group"}, {"id_or_tag", request.target_id}}}};
  });

  auto *connect =
      app.add_subcommand("connect", "Apply saved Profile settings and connect");
  connect->add_option("profile_id", request.profile_id)->required();
  connect->add_option("--node", request.target_id,
                      "Optionally persist a node selection before connecting");
  connect->add_option("--group", request.group_id,
                      "Optionally persist a group selection before connecting");
  connect->callback([&] {
    request.method = std::string(methods::connection_connect);
    request.params = {{"profile_id", request.profile_id}};
    if (!request.target_id.empty() && !request.group_id.empty())
      throw CLI::ValidationError("connect", "--node与--group不能同时指定");
    if (!request.target_id.empty())
      request.params["target"] = {{"kind", "node"},
                                  {"id_or_tag", request.target_id}};
    if (!request.group_id.empty())
      request.params["target"] = {{"kind", "group"},
                                  {"id_or_tag", request.group_id}};
  });
  auto *disconnect =
      app.add_subcommand("disconnect", "Stop the managed Xray process");
  disconnect->callback([&] {
    request.method = std::string(methods::connection_disconnect);
    request.params = Json::object();
  });
  auto *status =
      app.add_subcommand("status", "Show expected/actual core runtime state");
  status->callback([&] {
    request.method = std::string(methods::runtime_status);
    request.params = Json::object();
  });
  auto *logs =
      app.add_subcommand("logs", "Read the bounded server/core log buffer");
  logs->add_option("--cursor", request.log_cursor,
                   "Transport buffer cursor, not a log record number; 0 starts "
                   "at the oldest available chunk");
  logs->add_option(
      "--limit", request.log_limit,
      "Maximum log text bytes returned by this read (default 65536)");
  logs->callback([&] {
    request.method = std::string(methods::logs_read);
    request.params = {{"cursor", request.log_cursor},
                      {"limit", request.log_limit}};
  });
  auto *operation =
      app.add_subcommand("operation", "Inspect an asynchronous operation");
  auto *get =
      operation->add_subcommand("get", "Read an operation's final result");
  get->add_option("operation_id", request.operation_id)->required();
  get->callback([&] {
    request.method = std::string(methods::operation_get);
    request.params = {{"operation_id", request.operation_id}};
  });
}

} // namespace xrm::cli

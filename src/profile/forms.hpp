/// @file
/// @brief Declares structured Profile and module form operations.

#pragma once

#include "profile/profile_service.hpp"

namespace xrm {

enum class CollectionKind {
  inbounds,
  outbounds,
  route_rules,
  native_balancers
};
enum class ListAction { insert, replace, erase, move };
struct ListEdit {
  CollectionKind collection = CollectionKind::inbounds;
  ListAction action = ListAction::insert;
  std::size_t index = 0;
  std::optional<std::size_t> destination;
  std::optional<Json> object;
};
struct ModuleChange {
  ModuleEdit edit;
  bool include_after_save = true;
};
struct ModuleChangePlan {
  std::vector<ModuleChange> changes;
  std::vector<Notice> notices;
};
struct SaveManyResult {
  std::vector<std::string> saved;
  std::optional<AppError> failure;
};

// A structured edit of the same source text used by the native editor. The
// returned ModuleEdit keeps the caller's original text as its conflict base.
enum class JsonEditAction {
  set,
  erase,
  rename_key,
  array_insert,
  array_erase,
  array_copy,
  array_move,
  array_replace
};
struct JsonDraftEdit {
  JsonEditAction action = JsonEditAction::set;
  std::string pointer;
  std::optional<Json> value;
  std::optional<std::string> new_key;
  std::optional<std::size_t> index;
  std::optional<std::size_t> destination;
};
Result<ModuleEdit> edit_json_draft(const ModuleDocument &,
                                   const JsonDraftEdit &);

Result<ModuleEdit> set_log_level(const ModuleDocument &,
                                 std::optional<std::string>);
Result<ModuleEdit> replace_dns_servers(const ModuleDocument &,
                                       std::vector<Json>);
Result<ModuleEdit> replace_dns_hosts(const ModuleDocument &, Json hosts);
Result<ModuleEdit> set_api_services(const ModuleDocument &,
                                    std::vector<std::string>);
Result<ModuleEdit> set_metrics_listener(const ModuleDocument &,
                                        std::string listen);
Result<ModuleEdit> put_policy_level(const ModuleDocument &, std::string level,
                                    Json complete_level);
Result<ModuleEdit> set_policy_counters(const ModuleDocument &, bool in_up,
                                       bool in_down, bool out_up,
                                       bool out_down);
Result<ModuleEdit> replace_fakedns_pools(const ModuleDocument &,
                                         Json object_or_array);
Result<ModuleEdit> put_env(const ModuleDocument &, std::string key,
                           std::string value);
Result<ModuleEdit> erase_env(const ModuleDocument &, std::string key);
Result<ModuleEdit> set_version_range(const ModuleDocument &,
                                     std::optional<std::string> min,
                                     std::optional<std::string> max);
Result<ModuleEdit> replace_geodata_assets(const ModuleDocument &,
                                          std::vector<Json>);
Result<ModuleEdit> set_geodata_schedule(const ModuleDocument &,
                                        std::string cron);
Result<ModuleEdit> set_observatory_probe(const ModuleDocument &,
                                         std::string url, std::string interval,
                                         bool concurrency);
Result<ModuleEdit> replace_burst_ping_config(const ModuleDocument &,
                                             Json complete_ping_config);
Result<ModuleEdit> edit_native_list(const ModuleDocument &, const ListEdit &);
Result<Json> set_rule_target(Json original_rule, const Target &,
                             std::string_view resolved_native_tag);
Result<ModuleChangePlan> plan_traffic_stats(const ProfileSnapshot &,
                                            bool enabled,
                                            std::string metrics_listen);
SaveManyResult save_module_changes(ProfileService &, const Id &,
                                   const ModuleChangePlan &);

// Typed node editors return a changed node; callers persist it through the
// node store. They never edit an unrelated native module or outbound object.
Result<Node> edit_node_transport(const Node &, Transport);
Result<Node> edit_node_security(const Node &, Security);
Result<Node> edit_node_sockopt(const Node &, std::optional<Json>);
Result<Node> edit_node_finalmask(const Node &, std::optional<Json>);
Result<Node> edit_node_mux(const Node &, std::optional<Json>);

} // namespace xrm

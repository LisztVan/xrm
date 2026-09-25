#include "cli/command_options.hpp"
#include "contract/methods.hpp"

namespace xrm::cli {

void add_node_commands(CLI::App& app, Request& request) {
  auto* node = app.add_subcommand("node", "Manage the independent node inventory");
  auto* add = node->add_subcommand("add", "Save a manual node from native outbound JSON");
  add->add_option("--name", request.node_name, "Human readable node name")->required();
  add->add_option("--outbound-json", request.node_outbound, "Native Xray outbound object")->required();
  add->callback([&request] {
    try {
      auto outbound = Json::parse(request.node_outbound);
      if (!outbound.is_object()) throw CLI::ValidationError("--outbound-json must be an object");
      request.method = "node.add";
      request.params = Json{{"name", request.node_name}, {"outbound", std::move(outbound)}};
    } catch (const Json::exception&) {
      throw CLI::ValidationError("--outbound-json must contain valid JSON");
    }
  });

  auto* list = node->add_subcommand("list", "List saved nodes without connection secrets");
  list->add_option("--search", request.node_search, "Filter by node name");
  list->add_option("--offset", request.node_offset, "Page offset");
  list->add_option("--limit", request.node_limit, "Page size");
  list->callback([&request] {
    request.method = "node.list";
    request.params = Json{{"offset", request.node_offset}, {"limit", request.node_limit}};
    if (!request.node_search.empty()) request.params["text"] = request.node_search;
  });

  auto* show = node->add_subcommand("show", "Show one node including required connection settings");
  show->add_option("id", request.node_id)->required();
  show->callback([&request] { request.method = "node.get"; request.params = {{"id", request.node_id}}; });
  auto* edit = node->add_subcommand("edit", "Edit one typed transport/security/sockopt/finalmask/mux subobject");
  edit->add_option("id", request.node_id)->required();
  edit->add_option("field", request.module_key)->required()->check(
      CLI::IsMember({"transport", "security", "sockopt", "finalmask", "mux"}));
  edit->add_option("--json", request.node_outbound,
                   "Typed node subobject JSON; use null to clear optional fields")->required();
  edit->callback([&request] {
    Json value;
    try { value = Json::parse(request.node_outbound); }
    catch (const Json::exception&) { throw CLI::ValidationError("--json must contain valid JSON"); }
    request.method = "node.form_edit";
    request.params = {{"id", request.node_id}, {"field", request.module_key}, {"value", std::move(value)}};
  });
  auto* copy = node->add_subcommand("copy", "Copy a node as an independent manual node");
  copy->add_option("id", request.node_id)->required();
  copy->add_option("--name", request.node_name)->required();
  copy->callback([&request] { request.method = "node.copy"; request.params = {{"id", request.node_id}, {"name", request.node_name}}; });
  for (const auto& [name, enabled] : {std::pair{"enable", true}, std::pair{"disable", false}}) {
    auto* toggle = node->add_subcommand(name, enabled ? "Enable a node" : "Disable a node");
    toggle->add_option("id", request.node_id)->required();
    toggle->callback([&request, enabled] {
      request.method = "node.set_enabled";
      request.params = {{"id", request.node_id}, {"enabled", enabled}};
    });
  }
  auto* alias = node->add_subcommand("alias", "Set or clear a node display alias");
  alias->add_option("id", request.node_id)->required();
  alias->add_option("value", request.node_alias);
  alias->callback([&request] {
    request.method = "node.set_alias";
    request.params = {{"id", request.node_id}, {"alias", request.node_alias.empty() ? Json(nullptr) : Json(request.node_alias)}};
  });
  auto* labels = node->add_subcommand("labels", "Set node labels as comma-separated values");
  labels->add_option("id", request.node_id)->required();
  labels->add_option("--values", request.node_labels)->required();
  labels->callback([&request] {
    Json values = Json::array();
    std::size_t start = 0;
    while (start <= request.node_labels.size()) {
      const auto end = request.node_labels.find(',', start);
      const auto part = request.node_labels.substr(start, end == std::string::npos ? end : end - start);
      if (!part.empty()) values.push_back(part);
      if (end == std::string::npos) break;
      start = end + 1;
    }
    request.method = "node.set_labels";
    request.params = {{"id", request.node_id}, {"labels", std::move(values)}};
  });
  auto* history = node->add_subcommand("history", "Preview and confirm source-local redundant history cleanup");
  auto* preview = history->add_subcommand("preview", "Read-only history candidates; no credentials are printed");
  preview->add_option("source_id", request.subscription_id)->required();
  preview->callback([&request] {
    request.method = std::string(methods::node_history_preview);
    request.params = {{"source_id", request.subscription_id}};
  });
  auto* confirm = history->add_subcommand("confirm", "Remove only explicitly selected safe historical records");
  confirm->add_option("source_id", request.subscription_id)->required();
  confirm->add_option("--id", request.history_node_ids, "Historical node ID from preview; repeat for multiple")->required();
  confirm->add_option("--token", request.history_token, "Preview token; stale previews are rejected")->required();
  confirm->add_flag("--yes", request.history_confirmed, "Confirm deletion of the selected historical records");
  confirm->callback([&request] {
    if (!request.history_confirmed) throw CLI::ValidationError("node history confirm requires --yes");
    request.method = std::string(methods::node_history_confirm);
    request.params = {{"source_id", request.subscription_id}, {"token", request.history_token},
                      {"ids", request.history_node_ids}};
  });
}

}  // namespace xrm::cli

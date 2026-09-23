#include "cli/command_options.hpp"

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
}

}  // namespace xrm::cli

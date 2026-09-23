#include "cli/command_options.hpp"

#include "contract/methods.hpp"

namespace xrm::cli {

void add_runtime_commands(CLI::App& app, Request& request) {
  auto* select = app.add_subcommand("select", "Persist a node selection without connecting");
  auto* select_node = select->add_subcommand("node", "Select a saved node for a Profile");
  select_node->add_option("profile_id", request.profile_id)->required();
  select_node->add_option("node_id", request.target_id)->required();
  select_node->callback([&] {
    request.method = std::string(methods::connection_select);
    request.params = {{"profile_id", request.profile_id},
                      {"target", Json{{"kind", "node"}, {"id_or_tag", request.target_id}}}};
  });

  auto* connect = app.add_subcommand("connect", "Apply saved Profile settings and connect");
  connect->add_option("profile_id", request.profile_id)->required();
  connect->add_option("--node", request.target_id, "Optionally persist a node selection before connecting");
  connect->callback([&] {
    request.method = std::string(methods::connection_connect);
    request.params = {{"profile_id", request.profile_id}};
    if (!request.target_id.empty()) request.params["target"] = {{"kind", "node"}, {"id_or_tag", request.target_id}};
  });
  auto* disconnect = app.add_subcommand("disconnect", "Stop the managed Xray process");
  disconnect->callback([&] { request.method = std::string(methods::connection_disconnect); request.params = Json::object(); });
  auto* status = app.add_subcommand("status", "Show expected/actual core runtime state");
  status->callback([&] { request.method = std::string(methods::runtime_status); request.params = Json::object(); });
  auto* logs = app.add_subcommand("logs", "Read the bounded server/core log buffer");
  logs->add_option("--cursor", request.log_cursor);
  logs->add_option("--limit", request.log_limit);
  logs->callback([&] {
    request.method = std::string(methods::logs_read);
    request.params = {{"cursor", request.log_cursor}, {"limit", request.log_limit}};
  });
  auto* operation = app.add_subcommand("operation", "Inspect an asynchronous operation");
  auto* get = operation->add_subcommand("get", "Read an operation's final result");
  get->add_option("operation_id", request.operation_id)->required();
  get->callback([&] { request.method = std::string(methods::operation_get); request.params = {{"operation_id", request.operation_id}}; });
}

}  // namespace xrm::cli

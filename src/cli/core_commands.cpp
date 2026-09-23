#include "cli/command_options.hpp"

namespace xrm::cli {

void add_core_commands(CLI::App& app, Request& request) {
  auto* core = app.add_subcommand("core", "Register and inspect Xray binaries");
  auto* register_command = core->add_subcommand("register", "Register an existing Xray binary");
  register_command->add_option("path", request.core_path, "Absolute path to the Xray executable")->required();
  register_command->add_option("--name", request.core_name, "Local display name")->required();
  register_command->callback([&request] {
    request.method = "core.register";
    request.params = Json{{"path", request.core_path}, {"name", request.core_name}};
  });

  auto* list = core->add_subcommand("list", "List registered Xray binaries");
  list->callback([&request] {
    request.method = "core.list";
    request.params = Json::object();
  });
}

}  // namespace xrm::cli

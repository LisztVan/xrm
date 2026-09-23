#include "cli/command_options.hpp"
#include "cli/commands.hpp"

#include "contract/methods.hpp"
#include "contract/wire.hpp"

#include <iostream>

namespace xrm::cli {

int run_cli(int argc, char** argv) {
  Request request;
  request.socket = default_socket_path();
  CLI::App app{"xrm: Xray node inventory and core registration"};
  app.set_help_flag("-h,--help", "Show this help message");
  app.add_option("--socket", request.socket, "xrmd Unix domain socket");
  app.add_flag("--json", request.json, "Write machine readable output");
  app.fallthrough();

  auto* server = app.add_subcommand("server", "Server operations");
  auto* info = server->add_subcommand("info", "Show the connected server version");
  info->callback([&request] {
    request.method = "server.info";
    request.params = Json::object();
  });
  add_core_commands(app, request);
  add_node_commands(app, request);

  try {
    app.parse(argc, argv);
  } catch (const CLI::CallForHelp& error) {
    return app.exit(error);
  } catch (const CLI::ParseError& error) {
    if (request.json) {
      const auto app_error = AppError{ErrorCode::invalid_argument, error.what(),
                                      std::nullopt, std::nullopt};
      std::cerr << Json{{"error", error_to_json(app_error)}}.dump() << '\n';
    } else {
      std::cerr << error.what() << '\n';
    }
    return error.get_exit_code();
  }

  if (request.method.empty()) {
    std::cerr << "请选择 server info、core 或 node 命令\n";
    return 2;
  }
  return execute_request(request);
}

}  // namespace xrm::cli

namespace xrm {
int run_cli(int argc, char** argv) { return cli::run_cli(argc, argv); }
}

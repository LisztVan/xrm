#include "cli/command_options.hpp"
#include "cli/commands.hpp"
#include "cli/rpc_session.hpp"

#include "contract/methods.hpp"
#include "contract/wire.hpp"

#include <iostream>
#include <chrono>
#include <csignal>
#include <thread>

namespace xrm::cli {
namespace {
volatile std::sig_atomic_t stop_stats_watch = 0;
void request_stats_watch_stop(int) { stop_stats_watch = 1; }
}  // namespace

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
  add_subscription_commands(app, request);
  add_profile_commands(app, request);
  add_runtime_commands(app, request);
  bool stats_watch = false;
  auto* stats = app.add_subcommand("stats", "Show server-sampled Xray traffic and observations");
  stats->add_flag("--watch", stats_watch, "Refresh the shared server sample every two seconds");
  stats->callback([&request, &stats_watch] {
    request.method = std::string(methods::runtime_stats);
    request.params = {{"watch", stats_watch}};
  });
  std::string tui_profile;
  auto* tui = app.add_subcommand("tui", "Open the interactive Overview/Nodes/Native Modules/Logs interface");
  tui->add_option("--profile", tui_profile, "Initial Profile ID");
  tui->callback([&request] { request.method = "tui"; });

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
    std::cerr << "请选择 server、core、node、profile、config、connect、status、logs 或 tui 命令\n";
    return 2;
  }
  if (request.method == "tui") return run_tui(request.socket, tui_profile);
  if (request.method == methods::runtime_stats && stats_watch) {
    stop_stats_watch = 0;
    const auto old_int = std::signal(SIGINT, request_stats_watch_stop);
    const auto old_term = std::signal(SIGTERM, request_stats_watch_stop);
    RpcSession session(request.socket);
    int result = 0;
    while (!stop_stats_watch) {
      auto sample = session.call(request.method, Json{{"watch", true}});
      result = print_result(request, sample);
      if (result != 0) break;
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    (void)session.call(request.method, Json{{"watch", false}});
    session.close();
    std::signal(SIGINT, old_int);
    std::signal(SIGTERM, old_term);
    return result;
  }
  return execute_request(request);
}

}  // namespace xrm::cli

namespace xrm {
int run_cli(int argc, char** argv) { return cli::run_cli(argc, argv); }
}

// XRM: Starts the daemon with its storage, transport, and core-process
// adapters.

#include "infra/http.hpp"
#include "infra/process.hpp"
#include "infra/store.hpp"
#include "server/api_router.hpp"
#include "server/application.hpp"
#include "server/rpc_server.hpp"
#include "subscription/subscription_service.hpp"

#include <CLI/CLI.hpp>
#include <uv.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::filesystem::path default_socket() {
  if (const char *runtime = std::getenv("XDG_RUNTIME_DIR");
      runtime && *runtime) {
    return std::filesystem::path(runtime) / "xrm" / "xrmd.sock";
  }
  return std::filesystem::path("/tmp") /
         ("xrmd-" + std::to_string(static_cast<unsigned long>(::getuid())) +
          ".sock");
}

std::filesystem::path default_database() {
  if (const char *state = std::getenv("XDG_STATE_HOME"); state && *state) {
    return std::filesystem::path(state) / "xrm" / "server.sqlite";
  }
  if (const char *home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".local" / "state" / "xrm" /
           "server.sqlite";
  }
  return std::filesystem::path("/tmp") /
         ("xrm-" + std::to_string(static_cast<unsigned long>(::getuid())) +
          ".sqlite");
}

xrm::Id make_boot_id() {
  std::random_device random;
  return "boot-" + std::to_string((static_cast<std::uint64_t>(random()) << 32) |
                                  random());
}

struct Shutdown {
  uv_signal_t interrupt{};
  uv_signal_t terminate{};
  xrm::RpcServer *server = nullptr;
  xrm::ProcessService *processes = nullptr;
  xrm::Application *application = nullptr;
  xrm::HttpClient *http = nullptr;
  bool stopping = false;

  static void received(uv_signal_t *signal, int) {
    auto *self = static_cast<Shutdown *>(signal->data);
    if (self->stopping)
      return;
    self->stopping = true;
    uv_signal_stop(&self->interrupt);
    uv_signal_stop(&self->terminate);
    uv_close(reinterpret_cast<uv_handle_t *>(&self->interrupt), nullptr);
    uv_close(reinterpret_cast<uv_handle_t *>(&self->terminate), nullptr);
    self->application->shutdown([self] {
      self->http->shutdown([self] {
        self->server->close([] {});
        self->processes->shutdown([] {});
      });
    });
  }
};

} // namespace

int main(int argc, char **argv) {
  CLI::App app{"xrmd: XRM local manager server"};
  bool foreground = false;
  auto socket = default_socket();
  auto database = default_database();
  app.add_flag("--foreground", foreground, "Run attached to this terminal")
      ->required();
  app.add_option("--socket", socket, "Unix domain socket path");
  app.add_option("--database", database, "SQLite database path");
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &error) {
    return app.exit(error);
  }
  if (!foreground) {
    std::cerr << "xrmd currently requires --foreground (use a service manager "
                 "for background operation)\n";
    return 2;
  }

  ::umask(0077);
  try {
    if (!database.parent_path().empty())
      std::filesystem::create_directories(database.parent_path());
    if (!socket.parent_path().empty())
      std::filesystem::create_directories(socket.parent_path());
  } catch (const std::filesystem::filesystem_error &error) {
    std::cerr << "Cannot create server data directory: " << error.what()
              << '\n';
    return 1;
  }

  auto store = xrm::Store::open(database);
  if (!store) {
    std::cerr << store.error().message << '\n';
    return 1;
  }
  uv_loop_t loop{};
  int status = uv_loop_init(&loop);
  if (status != 0) {
    std::cerr << "Cannot initialize event loop: " << uv_strerror(status)
              << '\n';
    return 1;
  }
  xrm::ProcessService processes(loop);
  xrm::HttpClient http(loop);
  xrm::SubscriptionService subscriptions(**store, http);
  const auto data_root = database.parent_path().empty()
                             ? std::filesystem::current_path()
                             : database.parent_path();
  xrm::Application application(**store, processes, http, loop, data_root,
                               XRM_VERSION, make_boot_id(), subscriptions);
  xrm::ApiRouter router(application);
  xrm::RpcServer rpc(
      loop,
      [&router](const xrm::Id &connection_id, xrm::RpcRequest request,
                xrm::RpcReply reply) {
        router.dispatch(connection_id, std::move(request), std::move(reply));
      },
      [&application](const xrm::Id &connection_id) {
        application.release_metrics_viewer(connection_id);
      });
  auto listening = rpc.listen(socket);
  if (!listening) {
    std::cerr << listening.error().message << '\n';
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    return 1;
  }
  subscriptions.start_schedule(loop);

  application.recover([](xrm::Result<xrm::ActivationResult>) {});

  Shutdown shutdown;
  shutdown.server = &rpc;
  shutdown.processes = &processes;
  shutdown.application = &application;
  shutdown.http = &http;
  status = uv_signal_init(&loop, &shutdown.interrupt);
  const bool interrupt_initialized = status == 0;
  if (status == 0)
    status = uv_signal_init(&loop, &shutdown.terminate);
  const bool terminate_initialized = status == 0;
  if (status == 0) {
    shutdown.interrupt.data = &shutdown;
    shutdown.terminate.data = &shutdown;
    status = uv_signal_start(&shutdown.interrupt, Shutdown::received, SIGINT);
    if (status == 0)
      status =
          uv_signal_start(&shutdown.terminate, Shutdown::received, SIGTERM);
  }
  if (status != 0) {
    std::cerr << "Cannot register shutdown signals: " << uv_strerror(status)
              << '\n';
    if (interrupt_initialized)
      uv_close(reinterpret_cast<uv_handle_t *>(&shutdown.interrupt), nullptr);
    if (terminate_initialized)
      uv_close(reinterpret_cast<uv_handle_t *>(&shutdown.terminate), nullptr);
    rpc.close([] {});
    processes.shutdown([] {});
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    return 1;
  }

  uv_run(&loop, UV_RUN_DEFAULT);
  status = uv_loop_close(&loop);
  if (status != 0) {
    std::cerr << "Event loop still has active handles: " << uv_strerror(status)
              << '\n';
    return 1;
  }
  return 0;
}

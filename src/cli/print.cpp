#include "cli/command_options.hpp"

#include "client/rpc_client.hpp"
#include "contract/wire.hpp"

#include <uv.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <unistd.h>

namespace xrm::cli {
namespace {

struct CallState {
  RpcClient* client = nullptr;
  const Request* request = nullptr;
  std::optional<Result<Json>> result;
};

int exit_code_for(const AppError& error) {
  if (error.code == ErrorCode::io_error || error.code == ErrorCode::timed_out ||
      error.code == ErrorCode::cancelled) return 3;
  return 1;
}

std::string display_error(const Json& error) {
  if (!error.is_object() || !error.contains("message") || !error.at("message").is_string()) {
    return error.dump();
  }
  return error.at("message").get<std::string>();
}

}  // namespace

std::string default_socket_path() {
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
    return (std::filesystem::path(runtime) / "xrm" / "xrmd.sock").string();
  }
  return (std::filesystem::path("/tmp") /
          ("xrmd-" + std::to_string(static_cast<unsigned long>(::getuid())) + ".sock")).string();
}

int execute_request(const Request& request) {
  uv_loop_t loop{};
  const int initialized = uv_loop_init(&loop);
  if (initialized != 0) {
    return print_result(request, std::unexpected(AppError{
        ErrorCode::io_error, std::string("初始化客户端事件循环失败: ") + uv_strerror(initialized),
        std::nullopt, std::nullopt}));
  }

  RpcClient client(loop);
  CallState state{&client, &request, std::nullopt};
  auto opened = client.open(request.socket, [&state](Result<void> result) {
    if (!result) {
      state.result.emplace(std::unexpected(result.error()));
      return;
    }
    auto sent = state.client->call(state.request->method, state.request->params,
                                   [&state](Result<Json> response) {
      state.result.emplace(std::move(response));
      state.client->close();
    });
    if (!sent) {
      state.result.emplace(std::unexpected(sent.error()));
      state.client->close();
    }
  });
  if (!opened) {
    state.result.emplace(std::unexpected(opened.error()));
  } else {
    uv_run(&loop, UV_RUN_DEFAULT);
  }
  client.close();
  if (uv_loop_close(&loop) != 0 && !state.result) {
    state.result.emplace(std::unexpected(AppError{ErrorCode::io_error,
                                                   "客户端事件循环未能关闭",
                                                   std::nullopt, std::nullopt}));
  }
  if (!state.result) {
    state.result.emplace(std::unexpected(AppError{ErrorCode::result_unknown,
                                                  "IPC请求未收到结果",
                                                  std::nullopt, std::nullopt}));
  }
  return print_result(request, *state.result);
}

int print_result(const Request& request, const Result<Json>& result) {
  if (!result) {
    const auto error = error_to_json(result.error());
    if (request.json) std::cerr << Json{{"error", error}}.dump() << '\n';
    else std::cerr << display_error(error) << '\n';
    return exit_code_for(result.error());
  }
  if (request.json) {
    std::cout << result->dump() << '\n';
    return 0;
  }

  if (request.method == "server.info") {
    std::cout << "xrmd " << result->value("version", "unknown")
              << " (protocol " << result->value("protocol", 0) << ")\n";
  } else if (request.method == "core.register") {
    std::cout << "Registered " << result->value("name", "") << " "
              << result->value("reported_version", "") << " ["
              << result->value("id", "") << "]\n";
  } else if (request.method == "core.list") {
    for (const auto& core : *result) {
      std::cout << core.value("id", "") << '\t' << core.value("name", "") << '\t'
                << core.value("reported_version", "") << '\t'
                << core.value("path", "") << '\n';
    }
  } else if (request.method == "node.add") {
    std::cout << "Saved " << result->value("name", "") << " ("
              << result->value("protocol", "unknown") << ") ["
              << result->value("id", "") << "]\n";
  } else if (request.method == "node.list") {
    const auto& items = result->at("items");
    std::cout << "Nodes: " << result->value("total", 0) << '\n';
    for (const auto& node : items) {
      std::cout << node.value("id", "") << '\t' << node.value("name", "") << '\t'
                << node.value("protocol", "unknown") << '\n';
    }
  }
  return 0;
}

}  // namespace xrm::cli

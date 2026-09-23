#include "cli/command_options.hpp"

#include "client/rpc_client.hpp"
#include "contract/wire.hpp"

#include <uv.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>
#include <unistd.h>

namespace xrm::cli {
namespace {

struct CallState {
  RpcClient* client = nullptr;
  const Request* request = nullptr;
  std::optional<Result<Json>> result;
};

Result<Json> call_rpc_once(const std::string& socket, const std::string& method, const Json& params) {
  uv_loop_t loop{};
  const int initialized = uv_loop_init(&loop);
  if (initialized != 0)
    return std::unexpected(AppError{ErrorCode::io_error,
        std::string("初始化客户端事件循环失败: ") + uv_strerror(initialized), std::nullopt, std::nullopt});
  RpcClient client(loop);
  std::optional<Result<Json>> result;
  auto opened = client.open(socket, [&](Result<void> status) {
    if (!status) { result.emplace(std::unexpected(status.error())); return; }
    auto sent = client.call(method, params, [&](Result<Json> response) {
      result.emplace(std::move(response));
      client.close();
    });
    if (!sent) { result.emplace(std::unexpected(sent.error())); client.close(); }
  });
  if (!opened) result.emplace(std::unexpected(opened.error()));
  else uv_run(&loop, UV_RUN_DEFAULT);
  client.close();
  const int closed = uv_loop_close(&loop);
  if (!result) return std::unexpected(AppError{ErrorCode::result_unknown,
      "IPC请求未收到结果", std::nullopt, std::nullopt});
  if (closed != 0 && *result)
    return std::unexpected(AppError{ErrorCode::io_error, "客户端事件循环未能关闭", std::nullopt, std::nullopt});
  return std::move(*result);
}

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

int refresh_exit_code(const Request& request, const Json& result) {
  if (request.method != "subscription.refresh" || !result.is_object()) return 0;
  if (result.contains("failed") && result["failed"].is_array() && !result["failed"].empty()) return 4;
  if (result.contains("completed") && result["completed"].is_array())
    for (const auto& source : result["completed"])
      if (source.is_object() && source.value("partial", false)) return 4;
  return 0;
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
  auto result = call_rpc_once(request.socket, request.method, request.params);
  if (result && result->is_object() && result->value("status", "") == "accepted" &&
      result->contains("operation_id") && result->at("operation_id").is_string()) {
    const auto operation = result->at("operation_id").get<std::string>();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      auto polled = call_rpc_once(request.socket, "operation.get", Json{{"operation_id", operation}});
      if (!polled) { result = std::unexpected(polled.error()); break; }
      const auto status = polled->value("status", "");
      if (status == "succeeded") {
        result = polled->value("result", Json(nullptr));
        break;
      }
      if (status == "failed") {
        auto error = error_from_json(polled->at("error"));
        result = std::unexpected(error ? std::move(*error) : error.error());
        break;
      }
    }
    if (result && result->is_object() && result->value("status", "") == "accepted")
      result = std::unexpected(AppError{ErrorCode::timed_out,
          "等待Server操作超过5分钟；operation仍可继续查询", operation, std::nullopt});
  }
  return print_result(request, result);
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
    return refresh_exit_code(request, *result);
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
                << node.value("protocol", "unknown") << '\t'
                << (node.value("source_missing", false) ? "source_missing" : "available")
                << (node.value("enabled", false) ? "" : "\tdisabled") << '\n';
    }
  } else if (request.method == "subscription.list") {
    for (const auto& source : *result) {
      std::cout << source.value("id", "") << '\t' << source.value("name", "") << '\t'
          << (source.value("enabled", false) ? "enabled" : "paused") << '\t'
          << source.value("last_result", "never") << "\tnodes=" << source.value("node_count", 0)
          << "\tunsupported=" << source.value("unsupported_count", 0)
          << "\tmalformed=" << source.value("malformed_count", 0)
          << "\tlast_attempt=" << (source.contains("last_attempt") && source["last_attempt"].is_number()
                ? std::to_string(source["last_attempt"].get<std::int64_t>()) : "never")
          << "\tlast_success=" << (source.contains("last_success") && source["last_success"].is_number()
                ? std::to_string(source["last_success"].get<std::int64_t>()) : "never") << '\n';
    }
  } else if (request.method == "subscription.get") {
    std::cout << result->at("subscription").dump(2) << "\nstate: " << result->at("state").dump() << '\n';
  } else if (request.method == "subscription.refresh") {
    if (result->contains("completed")) for (const auto& source : result->at("completed"))
      std::cout << source.value("source_id", "") << " added=" << source.value("added", 0)
          << " updated=" << source.value("updated", 0) << " missing=" << source.value("missing", 0)
          << " unsupported=" << source.value("unsupported", 0) << " malformed=" << source.value("malformed", 0)
          << (source.value("partial", false) ? " partial" : "") << '\n';
    if (result->contains("failed")) for (const auto& source : result->at("failed"))
      std::cout << source.value("source_id", "") << " failed: " << source.value("error", Json::object()).value("message", "") << '\n';
  } else if (request.method == "node.get") {
    std::cout << result->dump(2) << '\n';
  } else if (request.method == "profile.list") {
    for (const auto& profile : *result)
      std::cout << profile.value("id", "") << '\t' << profile.value("name", "") << '\t'
                << profile.value("core_id", "") << '\n';
  } else if (request.method == "module.list") {
    for (const auto& module : *result)
      std::cout << module.value("key", "") << '\t' << (module.value("included", false) ? "enabled" : "disabled")
                << '\t' << module.value("source_file", "") << '\t' << module.value("runtime_file", "") << '\n';
  } else if (request.method == "module.get") {
    std::cout << "module: " << result->value("key", "") << "\nenabled: "
              << (result->value("included", false) ? "yes" : "no") << "\nsource: "
              << result->value("source_file", "") << "\nruntime: "
              << result->value("runtime_file", "") << "\n";
    if (result->contains("text") && result->at("text").is_string()) std::cout << result->at("text").get<std::string>();
  } else if (request.method == "runtime.status") {
    std::cout << "state: " << result->value("state", "unknown") << "\nhealth: "
              << result->value("health", "unknown") << "\n";
    if (result->contains("active") && result->at("active").is_object())
      std::cout << "profile: " << result->at("active").value("profile_id", "") << "\nconfdir: "
                << result->at("active").value("confdir", "") << '\n';
  } else if (request.method == "logs.read") {
    for (const auto& entry : result->at("entries"))
      std::cout << '[' << entry.value("cursor", 0) << "][" << entry.value("stream", "") << "] "
                << entry.value("text", "");
  } else if (request.method == "config.render") {
    std::cout << "confdir: " << result->value("confdir", "") << '\n';
    for (const auto& file : result->at("files")) std::cout << file.get<std::string>() << '\n';
  }
  return refresh_exit_code(request, *result);
}

}  // namespace xrm::cli

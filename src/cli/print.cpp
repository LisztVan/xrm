#include "cli/command_options.hpp"

#include "client/rpc_client.hpp"
#include "contract/wire.hpp"

#include <uv.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <cctype>
#include <thread>
#include <unistd.h>
#include <sys/utsname.h>

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

Result<Json> finish_operation(const std::string& socket, Result<Json> result) {
  if (!result || !result->is_object() || result->value("status", "") != "accepted" ||
      !result->contains("operation_id") || !result->at("operation_id").is_string()) return result;
  const auto operation = result->at("operation_id").get<std::string>();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto polled = call_rpc_once(socket, "operation.get", Json{{"operation_id", operation}});
    if (!polled) return std::unexpected(polled.error());
    const auto status = polled->value("status", "");
    if (status == "succeeded") return polled->value("result", Json(nullptr));
    if (status == "failed") {
      auto error = error_from_json(polled->at("error"));
      return std::unexpected(error ? std::move(*error) : error.error());
    }
  }
  return std::unexpected(AppError{ErrorCode::timed_out,
      "等待Server操作超过5分钟；operation仍可继续查询", std::nullopt, std::nullopt});
}

std::pair<std::string, std::string> host_platform_arch() {
  struct utsname host{};
  if (::uname(&host) != 0) return {};
  std::string platform = host.sysname;
  if (platform == "Linux") platform = "linux";
  else if (platform == "Darwin") platform = "macos";
  else if (platform == "FreeBSD") platform = "freebsd";
  else if (platform == "OpenBSD") platform = "openbsd";
  else return {};
  std::string arch = host.machine;
  if (arch == "x86_64") arch = "x86_64";
  else if (arch == "aarch64" || arch == "arm64") arch = "aarch64";
  else if (arch == "i386" || arch == "i686") arch = "x86";
  else return {};
  return {std::move(platform), std::move(arch)};
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

bool is_uuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

std::string lowercase(std::string_view value) {
  std::string result(value);
  for (auto& ch : result) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return result;
}

Result<Id> resolve_profile_ref(const std::string& socket, std::string_view input) {
  auto profiles = call_rpc_once(socket, "profile.list", Json::object());
  if (!profiles) return std::unexpected(profiles.error());
  if (!profiles->is_array())
    return std::unexpected(AppError{ErrorCode::protocol_error,
        "profile.list没有返回Profile列表", std::nullopt, std::nullopt});

  if (is_uuid(input)) {
    const auto canonical = lowercase(input);
    for (const auto& profile : *profiles) {
      if (profile.is_object() && lowercase(profile.value("id", std::string{})) == canonical)
        return profile.value("id", std::string{});
    }
  }

  std::vector<std::string> matches;
  for (const auto& profile : *profiles) {
    if (profile.is_object() && profile.value("name", std::string{}) == input &&
        profile.contains("id") && profile.at("id").is_string())
      matches.push_back(profile.at("id").get<std::string>());
  }
  if (matches.size() == 1) return matches.front();
  if (matches.size() > 1)
    return std::unexpected(AppError{ErrorCode::conflict,
        "Profile名称不唯一；请使用UUID精确指定", std::string(input), std::nullopt});
  return std::unexpected(AppError{ErrorCode::not_found,
      "找不到对应Profile（请使用精确名称或UUID）", std::string(input), std::nullopt});
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
  Json params = request.params;
  if (params.is_object() && params.contains("profile_id") && params.at("profile_id").is_string()) {
    auto profile_id = resolve_profile_ref(request.socket, params.at("profile_id").get_ref<const std::string&>());
    if (!profile_id) return print_result(request, std::unexpected(profile_id.error()));
    params["profile_id"] = std::move(*profile_id);
  }
  if (request.method == "module.form") {
    auto plan = call_rpc_once(request.socket, "module.change_plan", params);
    if (!plan) return print_result(request, std::unexpected(plan.error()));
    const auto profile_id = params.value("profile_id", std::string{});
    auto saved = call_rpc_once(request.socket, "module.save_changes",
        Json{{"profile_id", profile_id}, {"plan", *plan}});
    if (!saved) return print_result(request, std::unexpected(saved.error()));
    if (!saved->is_object())
      return print_result(request, std::unexpected(AppError{ErrorCode::protocol_error,
          "module.save_changes返回了非对象", std::nullopt, std::nullopt}));
    (*saved)["notices"] = plan->value("notices", Json::array());
    (*saved)["planned_modules"] = Json::array();
    if (plan->contains("changes") && plan->at("changes").is_array())
      for (const auto& change : plan->at("changes"))
        if (change.is_object() && change.contains("key")) (*saved)["planned_modules"].push_back(change.at("key"));
    return print_result(request, *saved);
  }
  if (request.method == "module.enable_traffic_statistics") {
    auto plan = call_rpc_once(request.socket, "module.change_plan", params);
    if (!plan) return print_result(request, std::unexpected(plan.error()));
    const auto profile_id = params.value("profile_id", std::string{});
    auto saved = call_rpc_once(request.socket, "module.save_changes",
        Json{{"profile_id", profile_id}, {"plan", *plan}});
    if (!saved) return print_result(request, std::unexpected(saved.error()));
    if (!saved->is_object())
      return print_result(request, std::unexpected(AppError{ErrorCode::protocol_error,
          "module.save_changes返回了非对象", std::nullopt, std::nullopt}));
    (*saved)["notices"] = plan->value("notices", Json::array());
    (*saved)["planned_modules"] = Json::array();
    if (plan->contains("changes") && plan->at("changes").is_array())
      for (const auto& change : plan->at("changes"))
        if (change.is_object() && change.contains("key")) (*saved)["planned_modules"].push_back(change.at("key"));
    return print_result(request, *saved);
  }
  if (request.method == "core.use.version") {
    auto cores = call_rpc_once(request.socket, "core.list", Json::object());
    if (!cores) return print_result(request, std::unexpected(cores.error()));
    std::vector<std::string> matches;
    if (cores->is_array()) for (const auto& core : *cores)
      if (core.is_object() && (core.value("id", "") == request.core_version ||
                               core.value("reported_version", "") == request.core_version))
        matches.push_back(core.value("id", ""));
    if (matches.size() != 1)
      return print_result(request, std::unexpected(AppError{matches.empty() ? ErrorCode::not_found : ErrorCode::conflict,
          matches.empty() ? "找不到已登记的核心版本或 ID" : "该版本对应多个已登记核心，请用 core ID 消歧",
          request.core_version, std::nullopt}));
    auto result = finish_operation(request.socket, call_rpc_once(request.socket, "core.use",
        Json{{"profile_id", params.value("profile_id", std::string{})}, {"core_id", matches.front()}}));
    return print_result(request, result);
  }
  if (request.method == "core.install.version") {
    auto releases = finish_operation(request.socket,
        call_rpc_once(request.socket, "core.releases", Json{{"include_prerelease", true}}));
    if (!releases) return print_result(request, std::unexpected(releases.error()));
    if (!releases->is_object() || !releases->contains("items") || !releases->at("items").is_array())
      return print_result(request, std::unexpected(AppError{ErrorCode::protocol_error,
          "core.releases未返回版本列表", std::nullopt, std::nullopt}));
    const Json* selected_release = nullptr;
    for (const auto& release : releases->at("items"))
      if (release.is_object() && release.value("tag", "") == request.core_version) {
        selected_release = &release;
        break;
      }
    if (!selected_release)
      return print_result(request, std::unexpected(AppError{ErrorCode::not_found,
          "官方发布列表中找不到所选版本", request.core_version, std::nullopt}));
    std::string asset_name = request.core_asset;
    if (asset_name.empty()) {
      const auto [platform, architecture] = host_platform_arch();
      std::vector<std::string> matching;
      for (const auto& asset : selected_release->value("assets", Json::array()))
        if (asset.value("platform", "") == platform && asset.value("architecture", "") == architecture)
          matching.push_back(asset.value("name", ""));
      if (matching.size() != 1)
        return print_result(request, std::unexpected(AppError{ErrorCode::not_found,
            "所选版本没有唯一匹配当前平台的官方资产；请传入 --asset", request.core_version, std::nullopt}));
      asset_name = matching.front();
    }
    auto installed = finish_operation(request.socket, call_rpc_once(request.socket, "core.install",
        Json{{"request", {{"release", *selected_release}, {"asset_name", asset_name},
            {"allow_unverified_checksum", request.core_allow_unverified_checksum}}}}));
    return print_result(request, installed);
  }
  auto result = call_rpc_once(request.socket, request.method, params);
  result = finish_operation(request.socket, std::move(result));
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
    const auto runtime = call_rpc_once(request.socket, "runtime.status", Json::object());
    const auto profiles = call_rpc_once(request.socket, "profile.list", Json::object());
    const std::string running_core = runtime && runtime->is_object() &&
        runtime->contains("active") && runtime->at("active").is_object()
        ? runtime->at("active").value("core_id", "") : "";
    for (const auto& core : *result) {
      bool selected = false;
      if (profiles && profiles->is_array()) for (const auto& profile : *profiles)
        selected = selected || profile.value("core_id", "") == core.value("id", "");
      std::cout << core.value("id", "") << '\t' << core.value("name", "") << '\t'
                << core.value("reported_version", "") << '\t'
                << core.value("path", "") << '\t'
                << (core.value("managed", false) ? "managed" : "system") << '\t'
                << "installed" << (selected ? ",selected" : "")
                << (running_core == core.value("id", "") ? ",running" : "") << '\n';
    }
  } else if (request.method == "core.releases") {
    if (result->contains("items") && result->at("items").is_array()) for (const auto& release : result->at("items")) {
      std::cout << release.value("tag", "") << '\t'
                << (release.value("prerelease", false) ? "prerelease" : "stable") << '\t'
                << release.value("published_at", std::int64_t{}) << '\n';
      if (release.contains("assets") && release.at("assets").is_array())
        for (const auto& asset : release.at("assets"))
          std::cout << "  " << asset.value("name", "") << '\t'
                    << asset.value("platform", "") << '/' << asset.value("architecture", "")
                    << (asset.value("sha256", Json(nullptr)).is_string() ? "\tsha256 verified" : "\tno checksum")
                    << '\n';
    }
  } else if (request.method == "core.install.version") {
    const auto installed = result->contains("install") ? result->at("install") : *result;
    std::cout << "Installed " << installed.value("reported_version", "") << " ["
              << installed.value("id", "") << "] (managed; not selected)\n";
  } else if (request.method == "core.use.version") {
    std::cout << "Profile core updated";
    if (result->contains("effect")) std::cout << " (" << result->value("effect", "unknown") << ')';
    std::cout << '\n';
  } else if (request.method == "module.form") {
    std::cout << "Saved native module fields:";
    if (result->contains("changed_modules") && result->at("changed_modules").is_array())
      for (const auto& key : result->at("changed_modules"))
        if (key.is_string()) std::cout << ' ' << key.get<std::string>();
    std::cout << '\n';
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
                << node.value("support", "raw-only") << '\t'
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
  } else if (request.method == "group.list") {
    for (const auto& group : *result)
      std::cout << group.value("id", "") << '\t' << group.value("name", "") << '\t'
          << group.value("selection_mode", "automatic") << '\t'
          << group.value("balancer", Json::object()).value("tag", "") << '\n';
  } else if (request.method == "group.get") {
    std::cout << result->dump(2) << '\n';
  } else if (request.method == "group.save") {
    std::cout << "saved group " << result->value("id", "") << '\n';
  } else if (request.method == "group.remove") {
    std::cout << "group removed\n";
  } else if (request.method == "group.select") {
    std::cout << "manual group selection saved; runtime unchanged\n";
  } else if (request.method == "group.override" || request.method == "group.override.clear") {
    std::cout << "runtime override " << result->value("state", "unknown");
    if (result->contains("actual_outbound") && result->at("actual_outbound").is_string())
      std::cout << ": " << result->at("actual_outbound").get<std::string>();
    std::cout << "\n";
  } else if (request.method == "connection.select") {
    std::cout << "selection saved; runtime unchanged\n";
  } else if (request.method == "runtime.status") {
    std::cout << "state: " << result->value("state", "unknown") << "\nhealth: "
              << result->value("health", "unknown") << "\n";
    if (result->contains("saved_target")) std::cout << "saved target: " << result->at("saved_target").dump() << '\n';
    if (result->contains("actual_target")) std::cout << "applied target: " << result->at("actual_target").dump() << '\n';
    std::cout << "runtime override: " << result->value("override_state", "unknown");
    if (result->contains("runtime_override") && result->at("runtime_override").is_string())
      std::cout << " " << result->at("runtime_override").get<std::string>();
    std::cout << '\n';
    if (result->contains("active") && result->at("active").is_object())
      std::cout << "profile: " << result->at("active").value("profile_id", "") << "\nconfdir: "
                << result->at("active").value("confdir", "") << '\n';
  } else if (request.method == "runtime.stats") {
    const auto availability = result->value("availability", "unknown");
    std::cout << "metrics: " << availability << '\n';
    if (availability != "available") {
      const auto missing_key = result->contains("missing_conditions") ? "missing_conditions" : "missing";
      if (result->contains(missing_key) && result->at(missing_key).is_array()) {
        std::cout << "unavailable conditions:";
        for (const auto& condition : result->at(missing_key))
          if (condition.is_string()) std::cout << ' ' << condition.get<std::string>();
        std::cout << '\n';
      }
    }
    if (result->contains("sample") && result->at("sample").is_object()) {
      const auto& sample = result->at("sample");
      if (sample.contains("received_at") && !sample["received_at"].is_null())
        std::cout << "sample timestamp: " << sample["received_at"].dump() << '\n';
      if (sample.contains("traffic") && sample["traffic"].is_array()) {
        for (const auto& counter : sample["traffic"]) {
          std::cout << "traffic " << counter.value("scope", "") << '/' << counter.value("tag", "")
                    << " uplink=" << (counter.contains("uplink") && !counter["uplink"].is_null()
                        ? counter["uplink"].dump() : "unavailable")
                    << " downlink=" << (counter.contains("downlink") && !counter["downlink"].is_null()
                        ? counter["downlink"].dump() : "unavailable") << '\n';
        }
      }
      if (sample.contains("observations") && sample["observations"].is_array()) {
        for (const auto& observation : sample["observations"])
          std::cout << "observation " << observation.value("tag", "") << " latency="
                    << (observation.contains("latency_ms") && !observation["latency_ms"].is_null()
                        ? observation["latency_ms"].dump() + " ms" : "unavailable") << '\n';
      }
    } else {
      std::cout << "sample: unavailable\n";
    }
    if (result->contains("rates") && result->at("rates").is_array()) {
      for (const auto& rate : result->at("rates"))
        std::cout << "rate " << rate.value("scope", "") << '/' << rate.value("tag", "")
                  << " uplink=" << (rate.contains("uplink_bytes_per_second") && !rate["uplink_bytes_per_second"].is_null()
                      ? rate["uplink_bytes_per_second"].dump() + " B/s" : "pending")
                  << " downlink=" << (rate.contains("downlink_bytes_per_second") && !rate["downlink_bytes_per_second"].is_null()
                      ? rate["downlink_bytes_per_second"].dump() + " B/s" : "pending") << '\n';
    }
  } else if (request.method == "module.enable_traffic_statistics") {
    std::cout << "changed modules:";
    if (result->contains("changed_modules") && result->at("changed_modules").is_array())
      for (const auto& module : result->at("changed_modules"))
        if (module.is_string()) std::cout << ' ' << module.get<std::string>();
    std::cout << '\n';
    if (result->contains("failure") && !result->at("failure").is_null())
      std::cout << "partial save failure: " << result->at("failure").dump() << '\n';
    if (result->contains("notices") && result->at("notices").is_array())
      for (const auto& notice : result->at("notices"))
        if (notice.is_object()) std::cout << notice.value("message", "") << '\n';
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

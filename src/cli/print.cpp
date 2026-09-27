/// @file
/// @brief Executes CLI RPC requests and formats their human/machine results.
///
/// The JSON branch keeps the wire result/error shape; presentation changes
/// belong in the human branch and must preserve stderr and exit-code behavior.

#include "cli/command_options.hpp"
#include "cli/presentation.hpp"
#include "infra/pipe_write_scope.hpp"

#include "client/human_message.hpp"
#include "client/rpc_client.hpp"
#include "contract/methods.hpp"
#include "contract/wire.hpp"

#include <uv.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

namespace xrm::cli {
namespace {

struct CallState {
  RpcClient *client = nullptr;
  const Request *request = nullptr;
  std::optional<Result<Json>> result;
};

Result<Json> call_rpc_once(const std::string &socket, const std::string &method,
                           const Json &params) {
  uv_loop_t loop{};
  const int initialized = uv_loop_init(&loop);
  if (initialized != 0)
    return std::unexpected(AppError{ErrorCode::io_error,
                                    std::string("初始化客户端事件循环失败: ") +
                                        uv_strerror(initialized),
                                    std::nullopt, std::nullopt});
  RpcClient client(loop);
  std::optional<Result<Json>> result;
  auto opened = client.open(socket, [&](Result<void> status) {
    if (!status) {
      result.emplace(std::unexpected(status.error()));
      return;
    }
    auto sent = client.call(method, params, [&](Result<Json> response) {
      result.emplace(std::move(response));
      client.close();
    });
    if (!sent) {
      result.emplace(std::unexpected(sent.error()));
      client.close();
    }
  });
  if (!opened)
    result.emplace(std::unexpected(opened.error()));
  else
    uv_run(&loop, UV_RUN_DEFAULT);
  client.close();
  const int closed = uv_loop_close(&loop);
  if (!result)
    return std::unexpected(AppError{ErrorCode::result_unknown,
                                    "IPC请求未收到结果", std::nullopt,
                                    std::nullopt});
  if (closed != 0 && *result)
    return std::unexpected(AppError{ErrorCode::io_error,
                                    "客户端事件循环未能关闭", std::nullopt,
                                    std::nullopt});
  return std::move(*result);
}

Result<Json> finish_operation(const std::string &socket, Result<Json> result) {
  // An asynchronous operation's accepted receipt is not its final result.
  // Resolve it before the one-shot CLI reports success or failure.
  if (!result || !result->is_object() ||
      result->value("status", "") != "accepted" ||
      !result->contains("operation_id") ||
      !result->at("operation_id").is_string())
    return result;
  const auto operation = result->at("operation_id").get<std::string>();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto polled = call_rpc_once(socket, "operation.get",
                                Json{{"operation_id", operation}});
    if (!polled)
      return std::unexpected(polled.error());
    const auto status = polled->value("status", "");
    if (status == "succeeded")
      return polled->value("result", Json(nullptr));
    if (status == "failed") {
      auto error = error_from_json(polled->at("error"));
      return std::unexpected(error ? std::move(*error) : error.error());
    }
  }
  return std::unexpected(AppError{
      ErrorCode::timed_out, "等待Server操作超过5分钟；operation仍可继续查询",
      operation, std::nullopt});
}

std::pair<std::string, std::string> host_platform_arch() {
  struct utsname host{};
  if (::uname(&host) != 0)
    return {};
  std::string platform = host.sysname;
  if (platform == "Linux")
    platform = "linux";
  else if (platform == "Darwin")
    platform = "macos";
  else if (platform == "FreeBSD")
    platform = "freebsd";
  else if (platform == "OpenBSD")
    platform = "openbsd";
  else
    return {};
  std::string arch = host.machine;
  if (arch == "x86_64")
    arch = "x86_64";
  else if (arch == "aarch64" || arch == "arm64")
    arch = "aarch64";
  else if (arch == "i386" || arch == "i686")
    arch = "x86";
  else
    return {};
  return {std::move(platform), std::move(arch)};
}

int exit_code_for(const AppError &error) {
  if (error.code == ErrorCode::io_error || error.code == ErrorCode::timed_out ||
      error.code == ErrorCode::cancelled)
    return 3;
  return 1;
}

void print_embedded_error(const Json &value, std::string_view action,
                          std::string_view object = {},
                          std::ostream &output = std::cout,
                          bool verbose = false, std::string_view method = {}) {
  const auto parsed = error_from_json(value);
  const auto error =
      parsed ? *parsed
             : AppError{ErrorCode::protocol_error, value.dump(), {}, {}};
  const auto message = human::error(error, action, object, method);
  output << message.title << "\nReason: " << message.reason
         << "\nRecovery: " << message.recovery << '\n';
  if (verbose)
    output << "Diagnostic (original):\n" << message.diagnostic << '\n';
}

std::string target_label(const Json &target) {
  if (!target.is_object())
    return "none";
  return target.value("kind", "unknown") + std::string(" ") +
         target.value("id_or_tag", "");
}

std::string request_module(const Request &request) {
  if (!request.module_key.empty())
    return request.module_key;
  if (request.method != "module.form")
    return {};
  const auto action = request.params.value("action", "");
  return action.substr(0, action.find('.'));
}

std::string request_subject(const Request &request) {
  // URL, headers and native JSON can contain credentials. Use identifiers only.
  if (!request.profile_id.empty())
    return "Profile " + request.profile_id +
           (request_module(request).empty() ? ""
                                            : " / " + request_module(request));
  if (!request.subscription_id.empty())
    return request.subscription_id;
  if (!request.node_id.empty())
    return request.node_id;
  if (!request.group_id.empty())
    return request.group_id;
  if (!request.operation_id.empty())
    return request.operation_id;
  if (!request.core_id.empty())
    return request.core_id;
  for (const auto *key : {"profile", "resource"})
    if (request.params.contains(key) && request.params.at(key).is_string())
      return request.params.at(key).get<std::string>();
  return {};
}

std::string shell_quote(std::string_view text) {
  std::string quoted = "'";
  for (char ch : text) {
    if (ch == '\'')
      quoted += "'\\''";
    else
      quoted += ch;
  }
  quoted += '\'';
  return quoted;
}

std::string recovery_command(const Request &request, const AppError &error,
                             human::Recovery next) {
  const auto prefix = "xrm --socket " + shell_quote(request.socket) + " ";
  if (error.code == ErrorCode::core_check_failed &&
      request.method == "config.check" && !request.profile_id.empty())
    return prefix + "--verbose config check " + shell_quote(request.profile_id);
  if (next == human::Recovery::endpoint)
    return prefix + "server info";
  if (next == human::Recovery::core)
    return prefix + "core list\nSetup help: " + prefix +
           "core register --help\nSelect core help: " + prefix +
           "core use --help";
  if (next == human::Recovery::operation && error.subject &&
      error.message.find("operation") != std::string::npos)
    return prefix + "operation get " + shell_quote(*error.subject);
  if (next == human::Recovery::source && !request.profile_id.empty() &&
      !request_module(request).empty())
    return prefix + "config show " + shell_quote(request.profile_id) + " " +
           shell_quote(request_module(request));
  if (next == human::Recovery::runtime || next == human::Recovery::operation)
    return prefix + "status";
  if (request.method.starts_with("subscription."))
    return prefix + "sub list";
  if (request.method.starts_with("node."))
    return prefix + "node list";
  if (request.method.starts_with("cli.geodata."))
    return prefix + "geodata list";
  if (request.method.starts_with("core."))
    return prefix + "core list";
  if (request.method.starts_with("group.") && !request.profile_id.empty())
    return prefix + "group list " + shell_quote(request.profile_id);
  if (!request.profile_id.empty() || request.method.starts_with("profile.") ||
      request.method.starts_with("cli.routing."))
    return prefix + "profile list";
  return prefix +
         request.command.substr(request.command.starts_with("xrm ") ? 4 : 0) +
         " --help";
}

std::string profile_identity(const Request &request, const std::string &id) {
  if (id.empty())
    return "unknown";
  const auto profile =
      call_rpc_once(request.socket, "profile.get", Json{{"profile_id", id}});
  if (profile && profile->is_object() && !profile->value("name", "").empty())
    return profile->value("name", "") + " [" + id + "]";
  return "[" + id + "] (name unavailable)";
}

bool human_color_enabled(const Request &request, const Json &result) {
  // Non-terminal and NO_COLOR output needs no extra daemon query.
  const auto *term = std::getenv("TERM");
  if (!::isatty(STDOUT_FILENO) || std::getenv("NO_COLOR") ||
      (term && std::string_view(term) == "dumb"))
    return false;
  static std::optional<std::string> saved_preference;
  if (request.method == "settings.get" || request.method == "settings.update") {
    saved_preference =
        result.value("appearance", Json::object()).value("cli_color", "auto");
  } else if (!saved_preference) {
    auto settings =
        call_rpc_once(request.socket, "settings.get", Json::object());
    saved_preference = settings && settings->is_object()
                           ? settings->value("appearance", Json::object())
                                 .value("cli_color", "auto")
                           : "auto";
  }
  if (std::getenv("NO_COLOR"))
    return false;
  return ::isatty(STDOUT_FILENO) &&
         (*saved_preference == "always" || *saved_preference == "auto");
}

std::string colored(std::string_view text, std::string_view ansi,
                    bool enabled) {
  if (!enabled)
    return std::string(text);
  return "\x1b[" + std::string(ansi) + "m" + std::string(text) + "\x1b[0m";
}

std::string diagnostic_line(std::string value) {
  value = terminal_text(std::move(value));
  while (!value.empty() && value.back() == ' ')
    value.pop_back();
  return value;
}

bool is_uuid(std::string_view value) {
  if (value.size() != 36)
    return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-')
        return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

std::string lowercase(std::string_view value) {
  std::string result(value);
  for (auto &ch : result)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return result;
}

Result<Id> resolve_profile_ref(const std::string &socket,
                               std::string_view input) {
  auto profiles = call_rpc_once(socket, "profile.list", Json::object());
  if (!profiles)
    return std::unexpected(profiles.error());
  if (!profiles->is_array())
    return std::unexpected(AppError{ErrorCode::protocol_error,
                                    "profile.list没有返回Profile列表",
                                    std::nullopt, std::nullopt});

  if (is_uuid(input)) {
    const auto canonical = lowercase(input);
    for (const auto &profile : *profiles) {
      if (profile.is_object() &&
          lowercase(profile.value("id", std::string{})) == canonical)
        return profile.value("id", std::string{});
    }
  }

  std::vector<std::string> matches;
  for (const auto &profile : *profiles) {
    if (profile.is_object() && profile.value("name", std::string{}) == input &&
        profile.contains("id") && profile.at("id").is_string())
      matches.push_back(profile.at("id").get<std::string>());
  }
  if (matches.size() == 1)
    return matches.front();
  if (matches.size() > 1)
    return std::unexpected(AppError{ErrorCode::conflict,
                                    "Profile名称不唯一；请使用UUID精确指定",
                                    std::string(input), std::nullopt});
  return std::unexpected(AppError{ErrorCode::not_found,
                                  "找不到对应Profile（请使用精确名称或UUID）",
                                  std::string(input), std::nullopt});
}

Result<Id> resolve_subscription_ref(const std::string &socket,
                                    std::string_view input) {
  auto sources = call_rpc_once(socket, "subscription.list", Json::object());
  if (!sources)
    return std::unexpected(sources.error());
  if (!sources->is_array())
    return std::unexpected(
        AppError{ErrorCode::protocol_error,
                 "subscription.list did not return a source list", std::nullopt,
                 std::nullopt});

  // Prefer an existing exact ID even if another source happens to use that ID
  // as its name.
  for (const auto &source : *sources)
    if (source.is_object() && source.value("id", std::string{}) == input)
      return source.value("id", std::string{});
  if (is_uuid(input)) {
    for (const auto &source : *sources)
      if (source.is_object() &&
          lowercase(source.value("id", std::string{})) == lowercase(input))
        return source.value("id", std::string{});
  }

  std::vector<std::string> matches;
  for (const auto &source : *sources)
    if (source.is_object() && source.value("name", std::string{}) == input &&
        source.contains("id") && source.at("id").is_string())
      matches.push_back(source.at("id").get<std::string>());
  if (matches.size() == 1)
    return matches.front();
  if (matches.size() > 1) {
    std::string ids;
    for (const auto &id : matches)
      ids += (ids.empty() ? "" : ", ") + id;
    return std::unexpected(
        AppError{ErrorCode::conflict,
                 "Subscription name is ambiguous. Use one source ID: " + ids,
                 std::string(input), std::nullopt});
  }
  return std::unexpected(AppError{
      ErrorCode::not_found,
      "No subscription source matches this exact name or ID; run xrm sub list",
      std::string(input), std::nullopt});
}

int refresh_exit_code(const Request &request, const Json &result) {
  if (request.method != "subscription.refresh" || !result.is_object())
    return 0;
  if (result.contains("failed") && result["failed"].is_array() &&
      !result["failed"].empty())
    return 4;
  if (result.contains("completed") && result["completed"].is_array())
    for (const auto &source : result["completed"])
      if (source.is_object() && source.value("partial", false))
        return 4;
  return 0;
}

} // namespace

std::string default_socket_path() {
  if (const char *runtime = std::getenv("XDG_RUNTIME_DIR");
      runtime && *runtime) {
    return (std::filesystem::path(runtime) / "xrm" / "xrmd.sock").string();
  }
  return (std::filesystem::path("/tmp") /
          ("xrmd-" + std::to_string(static_cast<unsigned long>(::getuid())) +
           ".sock"))
      .string();
}

int execute_request(const Request &request) {
  // Resolve local input/refs before RPC, but leave daemon business semantics
  // in the server. A failed file/stdin read must never send module.save.
  Json params = request.params;
  if (request.method == "module.save") {
    std::string text = request.module_text;
    if (request.module_file_specified) {
      if (request.module_file == "-") {
        text.assign(std::istreambuf_iterator<char>(std::cin),
                    std::istreambuf_iterator<char>());
        if (std::cin.bad())
          return print_result(
              request, std::unexpected(
                           AppError{ErrorCode::io_error,
                                    "Could not read complete JSON from stdin",
                                    std::nullopt, std::nullopt}));
      } else {
        std::ifstream file(request.module_file, std::ios::binary);
        if (!file)
          return print_result(request, std::unexpected(AppError{
                                           ErrorCode::io_error,
                                           "Could not open JSON input file",
                                           request.module_file, std::nullopt}));
        text.assign(std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>());
        if (file.bad())
          return print_result(
              request, std::unexpected(
                           AppError{ErrorCode::io_error,
                                    "Could not read complete JSON input file",
                                    request.module_file, std::nullopt}));
      }
    }
    try {
      const auto checked = Json::parse(text);
      (void)checked;
    } catch (const Json::parse_error &error) {
      return print_result(request, std::unexpected(AppError{
                                       ErrorCode::invalid_argument,
                                       "Invalid native module JSON at byte " +
                                           std::to_string(error.byte),
                                       request.module_key, std::nullopt}));
    }
    params["text"] = std::move(text);
  }
  if (params.is_object() && params.contains("profile_id") &&
      params.at("profile_id").is_string()) {
    auto profile_id = resolve_profile_ref(
        request.socket, params.at("profile_id").get_ref<const std::string &>());
    if (!profile_id)
      return print_result(request, std::unexpected(profile_id.error()));
    params["profile_id"] = std::move(*profile_id);
  }
  if (request.method == "subscription.get" ||
      request.method == "subscription.replace" ||
      request.method == "subscription.remove" ||
      (request.method == "subscription.refresh" &&
       !request.subscription_id.empty())) {
    auto source_id =
        resolve_subscription_ref(request.socket, request.subscription_id);
    if (!source_id)
      return print_result(request, std::unexpected(source_id.error()));
    if (request.method == "subscription.refresh")
      params["source_ids"] = Json::array({*source_id});
    else
      params["id"] = *source_id;
  }
  if (request.method == "module.form") {
    auto plan = call_rpc_once(request.socket, "module.change_plan", params);
    if (!plan)
      return print_result(request, std::unexpected(plan.error()));
    const auto profile_id = params.value("profile_id", std::string{});
    auto saved =
        call_rpc_once(request.socket, "module.save_changes",
                      Json{{"profile_id", profile_id}, {"plan", *plan}});
    if (!saved)
      return print_result(request, std::unexpected(saved.error()));
    if (!saved->is_object())
      return print_result(
          request, std::unexpected(AppError{ErrorCode::protocol_error,
                                            "module.save_changes返回了非对象",
                                            std::nullopt, std::nullopt}));
    (*saved)["notices"] = plan->value("notices", Json::array());
    (*saved)["planned_modules"] = Json::array();
    if (plan->contains("changes") && plan->at("changes").is_array())
      for (const auto &change : plan->at("changes"))
        if (change.is_object() && change.contains("key"))
          (*saved)["planned_modules"].push_back(change.at("key"));
    return print_result(request, *saved);
  }
  if (request.method == "module.enable_traffic_statistics") {
    auto plan = call_rpc_once(request.socket, "module.change_plan", params);
    if (!plan)
      return print_result(request, std::unexpected(plan.error()));
    const auto profile_id = params.value("profile_id", std::string{});
    auto saved =
        call_rpc_once(request.socket, "module.save_changes",
                      Json{{"profile_id", profile_id}, {"plan", *plan}});
    if (!saved)
      return print_result(request, std::unexpected(saved.error()));
    if (!saved->is_object())
      return print_result(
          request, std::unexpected(AppError{ErrorCode::protocol_error,
                                            "module.save_changes返回了非对象",
                                            std::nullopt, std::nullopt}));
    (*saved)["notices"] = plan->value("notices", Json::array());
    (*saved)["planned_modules"] = Json::array();
    if (plan->contains("changes") && plan->at("changes").is_array())
      for (const auto &change : plan->at("changes"))
        if (change.is_object() && change.contains("key"))
          (*saved)["planned_modules"].push_back(change.at("key"));
    return print_result(request, *saved);
  }
  if (request.method == "core.use.version") {
    auto cores = call_rpc_once(request.socket, "core.list", Json::object());
    if (!cores)
      return print_result(request, std::unexpected(cores.error()));
    std::vector<std::string> matches;
    if (cores->is_array())
      for (const auto &core : *cores)
        if (core.is_object() &&
            (core.value("id", "") == request.core_version ||
             core.value("reported_version", "") == request.core_version))
          matches.push_back(core.value("id", ""));
    if (matches.size() != 1)
      return print_result(
          request,
          std::unexpected(AppError{
              matches.empty() ? ErrorCode::not_found : ErrorCode::conflict,
              matches.empty() ? "找不到已登记的核心版本或 ID"
                              : "该版本对应多个已登记核心，请用 core ID 消歧",
              request.core_version, std::nullopt}));
    auto result = finish_operation(
        request.socket,
        call_rpc_once(
            request.socket, "core.use",
            Json{{"profile_id", params.value("profile_id", std::string{})},
                 {"core_id", matches.front()}}));
    return print_result(request, result);
  }
  if (request.method == "core.install.version") {
    auto releases = finish_operation(
        request.socket, call_rpc_once(request.socket, "core.releases",
                                      Json{{"include_prerelease", true}}));
    if (!releases)
      return print_result(request, std::unexpected(releases.error()));
    if (!releases->is_object() || !releases->contains("items") ||
        !releases->at("items").is_array())
      return print_result(
          request, std::unexpected(AppError{ErrorCode::protocol_error,
                                            "core.releases未返回版本列表",
                                            std::nullopt, std::nullopt}));
    const Json *selected_release = nullptr;
    for (const auto &release : releases->at("items"))
      if (release.is_object() &&
          release.value("tag", "") == request.core_version) {
        selected_release = &release;
        break;
      }
    if (!selected_release)
      return print_result(
          request, std::unexpected(AppError{
                       ErrorCode::not_found, "官方发布列表中找不到所选版本",
                       request.core_version, std::nullopt}));
    std::string asset_name = request.core_asset;
    if (asset_name.empty()) {
      const auto [platform, architecture] = host_platform_arch();
      std::vector<std::string> matching;
      for (const auto &asset : selected_release->value("assets", Json::array()))
        if (asset.value("platform", "") == platform &&
            asset.value("architecture", "") == architecture)
          matching.push_back(asset.value("name", ""));
      if (matching.size() != 1)
        return print_result(
            request,
            std::unexpected(AppError{
                ErrorCode::not_found,
                "所选版本没有唯一匹配当前平台的官方资产；请传入 --asset",
                request.core_version, std::nullopt}));
      asset_name = matching.front();
    }
    auto installed = finish_operation(
        request.socket,
        call_rpc_once(request.socket, "core.install",
                      Json{{"request",
                            {{"release", *selected_release},
                             {"asset_name", asset_name},
                             {"allow_unverified_checksum",
                              request.core_allow_unverified_checksum}}}}));
    return print_result(request, installed);
  }
  auto result = call_rpc_once(request.socket, request.method, params);
  result = finish_operation(request.socket, std::move(result));
  return print_result(request, result);
}

int print_watch_result(const Request &request, const Result<Json> &result) {
  // Scope EPIPE handling to stdout sampling. RPC or other write failures must
  // retain nonzero exits even when the CLI is connected to a pipeline.
  int result_code = 0;
  bool failed = false;
  int write_error = 0;
  {
    PipeWriteScope pipe_write;
    errno = 0;
    result_code = print_result(request, result);
    std::cout.flush();
    failed = !std::cout;
    if (failed)
      write_error = errno;
  }
  if (failed) {
    // A downstream reader closing stdout is a normal end to a sampling pipe.
    // Other write failures retain the existing nonzero I/O boundary.
    if (write_error == EPIPE)
      return watch_consumer_closed;
    const AppError error{ErrorCode::io_error, "Cannot write stats output",
                         std::nullopt, std::nullopt};
    if (request.json)
      std::cerr << Json{{"error", error_to_json(error)}}.dump() << '\n';
    else
      std::cerr << "stats: " << error.message << '\n';
    return 3;
  }
  return result_code;
}

int print_result(const Request &request, const Result<Json> &result) {
  // Handle machine output before any human labels, colors, or recovery text.
  // Both modes send errors to stderr and use the same error-to-exit mapping.
  if (!result) {
    const auto error = error_to_json(result.error());
    if (request.json)
      std::cerr << Json{{"error", error}}.dump() << '\n';
    else {
      const auto message = human::error(
          result.error(),
          request.command.empty() ? "XRM operation" : request.command,
          request_subject(request), request.method);
      std::cerr << "Error: " << message.title << "\nReason: " << message.reason
                << "\nRecovery: " << message.recovery << "\nNext: "
                << recovery_command(request, result.error(), message.next)
                << '\n';
      if (message.next == human::Recovery::endpoint || request.verbose)
        std::cerr << "Socket: " << request.socket << '\n';
      if (request.verbose)
        std::cerr << "Diagnostic (original):\n" << message.diagnostic << '\n';
    }
    return exit_code_for(result.error());
  }
  if (request.json) {
    std::cout << result->dump() << '\n';
    return refresh_exit_code(request, *result);
  }
  const bool color = human_color_enabled(request, *result);

  if (request.method == "server.info") {
    std::cout << colored("xrmd", "1;36", color) << ' '
              << result->value("version", "unknown") << " (protocol "
              << result->value("protocol", 0) << ")\n";
  } else if (request.method == "settings.get" ||
             request.method == "settings.update") {
    std::cout << colored("XRM appearance", "1;36", color) << '\n';
    for (const auto &[key, value] :
         result->value("appearance", Json::object()).items())
      std::cout << "  " << key << ": "
                << (value.is_string() ? value.get<std::string>() : value.dump())
                << '\n';
    std::cout << "Editor argv:";
    const auto argv =
        result->value("editor", Json::object()).value("argv", Json::array());
    if (argv.empty())
      std::cout << " VISUAL / EDITOR fallback";
    else
      for (const auto &argument : argv)
        if (argument.is_string())
          std::cout << ' ' << shell_quote(argument.get<std::string>());
    std::cout << "\nNode test defaults:\n";
    for (const auto &[key, value] :
         result->value("tests", Json::object()).items())
      std::cout << "  " << key << ": "
                << (value.is_string() ? value.get<std::string>() : value.dump())
                << '\n';
  } else if (request.method == "core.register") {
    std::cout << "Registered " << result->value("name", "") << " "
              << result->value("reported_version", "") << " ["
              << result->value("id", "") << "]\n";
  } else if (request.method == "core.list") {
    const auto runtime =
        call_rpc_once(request.socket, "runtime.status", Json::object());
    const auto profiles =
        call_rpc_once(request.socket, "profile.list", Json::object());
    const std::string running_core =
        runtime && runtime->is_object() && runtime->contains("active") &&
                runtime->at("active").is_object()
            ? runtime->at("active").value("core_id", "")
            : "";
    HumanRows core_rows;
    for (const auto &core : *result) {
      bool selected = false;
      if (profiles && profiles->is_array())
        for (const auto &profile : *profiles)
          selected =
              selected || profile.value("core_id", "") == core.value("id", "");
      if (::isatty(STDOUT_FILENO)) {
        core_rows.push_back(
            {core.value("name", ""), core.value("reported_version", ""),
             std::string(core.value("managed", false) ? "managed" : "system") +
                 (selected ? "; selected" : "") +
                 (running_core == core.value("id", "") ? "; running" : ""),
             core.value("id", ""), core.value("path", "")});
        continue;
      }
      std::cout << core.value("id", "") << '\t' << core.value("name", "")
                << '\t' << core.value("reported_version", "") << '\t'
                << core.value("path", "") << '\t'
                << (core.value("managed", false) ? "managed" : "system") << '\t'
                << "installed" << (selected ? ",selected" : "")
                << (running_core == core.value("id", "") ? ",running" : "")
                << '\n';
    }
    if (::isatty(STDOUT_FILENO))
      human_table({"NAME", "VERSION", "STATE", "ID", "PATH"}, core_rows,
                  "No registered cores", color);
  } else if (request.method == "core.releases") {
    if (result->contains("items") && result->at("items").is_array())
      for (const auto &release : result->at("items")) {
        std::cout << release.value("tag", "") << '\t'
                  << (release.value("prerelease", false) ? "prerelease"
                                                         : "stable")
                  << '\t' << release.value("published_at", std::int64_t{})
                  << '\n';
        if (release.contains("assets") && release.at("assets").is_array())
          for (const auto &asset : release.at("assets"))
            std::cout << "  " << asset.value("name", "") << '\t'
                      << asset.value("platform", "") << '/'
                      << asset.value("architecture", "")
                      << (asset.value("sha256", Json(nullptr)).is_string()
                              ? "\tchecksum available"
                              : "\tno checksum")
                      << '\n';
      }
  } else if (request.method == "core.install.version") {
    const auto installed =
        result->contains("install") ? result->at("install") : *result;
    std::cout << "Installed " << installed.value("reported_version", "") << " ["
              << installed.value("id", "") << "] (managed; not selected)\n";
  } else if (request.method == "core.use.version") {
    if (result->value("effect", "") == "checked_only")
      std::cout << "Core selection saved or already current for Profile "
                << request.profile_id << "; running instance unchanged\n";
    else if (result->value("effect", "") == "running_new")
      std::cout << "Core switched for Profile " << request.profile_id
                << "; process running, traffic unverified\n";
    else
      std::cout
          << "Core selection finished; inspect Profile and runtime status\n";
  } else if (request.method == "node.add") {
    std::cout << "Saved " << result->value("name", "") << " ("
              << result->value("protocol", "unknown") << ") ["
              << result->value("id", "") << "]\n";
  } else if (request.method == "node.list") {
    if (::isatty(STDOUT_FILENO)) {
      HumanRows rows;
      for (const auto &item : result->at("items"))
        rows.push_back(
            {item.value("name", ""), item.value("protocol", "unknown"),
             item.value("support", "raw-only"),
             std::string(item.value("enabled", false) ? "enabled"
                                                      : "disabled") +
                 (item.value("source_missing", false) ? "; source missing"
                                                      : ""),
             item.value("id", "")});
      std::cout << "Nodes  " << result->value("total", 0) << " total\n\n";
      human_table({"NAME", "PROTOCOL", "SUPPORT", "STATE", "ID"}, rows,
                  "No matching nodes", color);
      return 0;
    }
    const auto &items = result->at("items");
    std::cout << "Nodes: " << result->value("total", 0) << '\n';
    for (const auto &node : items) {
      std::cout << node.value("id", "") << '\t' << node.value("name", "")
                << '\t' << node.value("protocol", "unknown") << '\t'
                << node.value("support", "raw-only") << '\t'
                << (node.value("source_missing", false) ? "source_missing"
                                                        : "available")
                << (node.value("enabled", false) ? "" : "\tdisabled") << '\n';
    }
  } else if (request.method == methods::node_history_preview) {
    std::cout << result->value("source_name", "") << ": "
              << result->value("current_count", 0) << " current, "
              << result->value("history_count", 0) << " historical\n";
    for (const auto &item : result->value("candidates", Json::array())) {
      std::cout << (item.value("eligible", false) ? "[safe] " : "[keep] ")
                << item.value("name", "") << " [" << item.value("id", "")
                << "] -> " << item.value("keep_name", "none") << " ["
                << item.value("keep_id", "") << "] " << item.value("reason", "")
                << '\n';
      for (const auto &reference : item.value("references", Json::array()))
        if (reference.is_string())
          std::cout << "  referenced by " << reference.get<std::string>()
                    << '\n';
      for (const auto &blocker : item.value("blockers", Json::array()))
        if (blocker.is_string() && blocker != "known_reference")
          std::cout << "  blocked: " << blocker.get<std::string>() << '\n';
    }
    std::cout << "Preview token: " << result->value("token", "") << '\n';
  } else if (request.method == methods::node_history_confirm) {
    std::cout << "Removed " << result->value("removed", 0)
              << " selected historical records; runtime unchanged\n";
  } else if (request.method == "subscription.list") {
    if (::isatty(STDOUT_FILENO)) {
      HumanRows rows;
      for (const auto &item : *result)
        rows.push_back({item.value("name", ""),
                        item.value("enabled", false) ? "enabled" : "paused",
                        item.value("last_result", "never"),
                        std::to_string(item.value("node_count", 0)),
                        item.value("id", "")});
      human_table({"NAME", "REFRESH", "RESULT", "NODES", "ID"}, rows,
                  "No subscriptions", color);
      return 0;
    }
    for (const auto &source : *result) {
      std::cout
          << source.value("id", "") << '\t' << source.value("name", "") << '\t'
          << (source.value("enabled", false) ? "enabled" : "paused") << '\t'
          << source.value("last_result", "never")
          << "\tnodes=" << source.value("node_count", 0)
          << "\tunsupported=" << source.value("unsupported_count", 0)
          << "\tmalformed=" << source.value("malformed_count", 0)
          << "\tlast_attempt="
          << (source.contains("last_attempt") &&
                      source["last_attempt"].is_number()
                  ? std::to_string(source["last_attempt"].get<std::int64_t>())
                  : "never")
          << "\tlast_success="
          << (source.contains("last_success") &&
                      source["last_success"].is_number()
                  ? std::to_string(source["last_success"].get<std::int64_t>())
                  : "never")
          << '\n';
    }
  } else if (request.method == "subscription.get") {
    const auto &source = result->at("subscription");
    const auto &state = result->at("state");
    if (::isatty(STDOUT_FILENO)) {
      HumanRows fields{
          {"ID", source.value("id", "")},
          {"Auto refresh", source.value("auto_refresh", false) ? "on" : "off"},
          {"Last result", state.value("last_result", "never")}};
      if (state.contains("last_error") && state.at("last_error").is_string()) {
        fields.push_back(
            {"Failure stage", state.value("last_error_stage", "unknown")});
        fields.push_back(
            {"Last error",
             diagnostic_line(state.at("last_error").get<std::string>())});
      }
      human_details("Subscription  " + source.value("name", ""), fields, color);
      std::cout
          << "\nURL and headers: hidden; use xrm --json sub show for editing\n";
      return 0;
    }
    std::cout << "Subscription " << source.value("name", "") << " ["
              << source.value("id", "") << "]\n"
              << "Auto refresh: "
              << (source.value("auto_refresh", false) ? "on" : "off") << '\n'
              << "Last result: " << state.value("last_result", "never") << '\n';
    if (state.contains("last_error") && state.at("last_error").is_string())
      std::cout << "Failure stage: "
                << state.value("last_error_stage", "unknown") << '\n'
                << "Last error: "
                << diagnostic_line(state.at("last_error").get<std::string>())
                << '\n';
    std::cout << "URL and headers: hidden in human output; use --json sub show "
                 "for editing\n";
  } else if (request.method == "subscription.refresh") {
    if (result->contains("completed"))
      for (const auto &source : result->at("completed"))
        std::cout << source.value("source_id", "")
                  << " added=" << source.value("added", 0)
                  << " updated=" << source.value("updated", 0)
                  << " missing=" << source.value("missing", 0)
                  << " unsupported=" << source.value("unsupported", 0)
                  << " malformed=" << source.value("malformed", 0)
                  << (source.value("partial", false) ? " partial" : "") << '\n';
    if (result->contains("completed"))
      for (const auto &source : result->at("completed")) {
        if (!source.value("partial", false))
          continue;
        std::cerr << "Partial subscription update — "
                  << source.value("source_id", "") << ": "
                  << source.value("first_issue",
                                  "Some entries could not be parsed; inspect "
                                  "the source result")
                  << '\n';
      }
    if (result->contains("failed"))
      for (const auto &source : result->at("failed")) {
        std::cerr << "Error: ";
        print_embedded_error(source.value("error", Json::object()),
                             "Subscription update failed",
                             source.value("source_id", ""), std::cerr,
                             request.verbose, request.method);
        auto details =
            call_rpc_once(request.socket, "subscription.get",
                          Json{{"id", source.value("source_id", "")}});
        if (details && details->is_object() && details->contains("state") &&
            details->at("state").is_object()) {
          const auto &state = details->at("state");
          if (state.contains("last_error") &&
              state.at("last_error").is_string())
            std::cerr << "Stage: " << state.value("last_error_stage", "unknown")
                      << "\nCause: "
                      << diagnostic_line(
                             state.at("last_error").get<std::string>())
                      << '\n';
        }
      }
  } else if (request.method == "node.get") {
    const auto name =
        result->contains("alias") && result->at("alias").is_string()
            ? result->at("alias").get<std::string>()
            : result->value("original_name", "");
    std::string protocol = "unknown";
    if (result->contains("settings") && result->at("settings").is_object()) {
      const auto &settings = result->at("settings");
      if (settings.value("kind", "") == "stream" &&
          settings.contains("protocol") && settings.at("protocol").is_object())
        protocol = settings.at("protocol").value("kind", "unknown");
      else if (settings.value("kind", "") == "raw" &&
               settings.contains("value") && settings.at("value").is_object())
        protocol = settings.at("value").value("protocol", "unknown");
    }
    if (::isatty(STDOUT_FILENO)) {
      human_details(
          "Node  " + name,
          {{"ID", result->value("id", "")},
           {"Protocol", protocol},
           {"State",
            std::string(result->value("enabled", false) ? "enabled"
                                                        : "disabled") +
                (result->value("source_missing", false) ? "; source missing"
                                                        : "")},
           {"Source",
            result->contains("source_id") && result->at("source_id").is_string()
                ? result->at("source_id").get<std::string>()
                : "local"}},
          color);
      std::cout << "\nConnection settings: hidden; use xrm --json node show "
                   "for editing\n";
      return 0;
    }
    std::cout << "Node " << name << " [" << result->value("id", "") << "]\n"
              << "Protocol: " << protocol << '\n'
              << "State: "
              << (result->value("enabled", false) ? "enabled" : "disabled")
              << (result->value("source_missing", false) ? ", source missing"
                                                         : "")
              << '\n'
              << "Source: "
              << (result->contains("source_id") &&
                          result->at("source_id").is_string()
                      ? result->at("source_id").get<std::string>()
                      : "local")
              << '\n'
              << "Connection settings: hidden in human output; use --json node "
                 "show for editing\n";
  } else if (request.method == "profile.list") {
    if (::isatty(STDOUT_FILENO)) {
      HumanRows rows;
      for (const auto &item : *result)
        rows.push_back({item.value("name", ""), item.value("id", ""),
                        item.value("core_id", "").empty()
                            ? "none"
                            : item.value("core_id", "")});
      human_table({"NAME", "ID", "CORE ID"}, rows,
                  "No Profiles. Create one with xrm profile create --name Home",
                  color);
      return 0;
    }
    for (const auto &profile : *result)
      std::cout << profile.value("id", "") << '\t' << profile.value("name", "")
                << '\t' << profile.value("core_id", "") << '\n';
  } else if (request.method == "profile.get") {
    if (::isatty(STDOUT_FILENO)) {
      const auto sources = result->value("enabled_source_ids", Json(nullptr));
      human_details(
          "Profile  " + result->value("name", ""),
          {{"ID", result->value("id", "")},
           {"Core", result->value("core_id", "").empty()
                        ? "none"
                        : result->value("core_id", "")},
           {"Saved target",
            target_label(result->value("selected", Json(nullptr)))},
           {"Sources", sources.is_null() ? "all (legacy binding)"
                       : sources.is_array()
                           ? std::to_string(sources.size()) + " bound"
                           : "unknown"},
           {"Modules",
            std::to_string(result->value("modules", Json::array()).size())},
           {"Append selected",
            result->value("append_selected_default", false) ? "on" : "off"}},
          color);
      return 0;
    }
    std::cout << "Profile " << result->value("name", "") << " ["
              << result->value("id", "") << "]\n"
              << "Core: "
              << (result->value("core_id", "").empty()
                      ? "none"
                      : result->value("core_id", ""))
              << '\n'
              << "Saved target: "
              << target_label(result->value("selected", Json(nullptr))) << '\n'
              << "Sources: ";
    if (!result->contains("enabled_source_ids") ||
        result->at("enabled_source_ids").is_null())
      std::cout << "all (legacy binding)\n";
    else if (result->at("enabled_source_ids").is_array())
      std::cout << result->at("enabled_source_ids").size() << '\n';
    else
      std::cout << "unknown\n";
    std::cout << "Modules: " << result->value("modules", Json::array()).size()
              << '\n'
              << "Append selected default: "
              << (result->value("append_selected_default", false) ? "on"
                                                                  : "off")
              << '\n'
              << "Runtime: unaffected by this query\n";
  } else if (request.method == "profile.create" ||
             request.method == "profile.clone") {
    std::cout << (request.method == "profile.create" ? "Created" : "Copied")
              << " Profile " << result->value("name", "") << " ["
              << result->value("id", "") << "]; no core started\n";
  } else if (request.method == "profile.rename") {
    std::cout << "Renamed Profile " << request.profile_id << " to "
              << request.profile_name << "; running core unchanged\n";
  } else if (request.method == "profile.remove") {
    std::cout << "Removed Profile " << request.profile_id << '\n';
  } else if (request.method == "module.list") {
    if (::isatty(STDOUT_FILENO)) {
      HumanRows rows;
      const bool compact = terminal_width() < 98;
      for (const auto &item : *result) {
        std::vector<std::string> row{
            item.value("key", ""),
            item.value("included", false) ? "included" : "excluded",
            item.value("source_file", "") +
                (item.value("text", Json(nullptr)).is_null() ? " (not saved)"
                                                             : "")};
        if (compact)
          row.back() += "\n" + item.value("runtime_file", "");
        else
          row.push_back(item.value("runtime_file", ""));
        rows.push_back(std::move(row));
      }
      std::cout << "Source files are editable; runtime filenames are generated "
                   "on Connect / Apply.\n";
      human_table(compact
                      ? std::vector<std::string>{"MODULE", "INCLUDE",
                                                 "SOURCE FILE / RUNTIME FILE"}
                      : std::vector<std::string>{"MODULE", "INCLUDE",
                                                 "SOURCE FILE", "RUNTIME FILE"},
                  rows, "No module settings", color);
      return 0;
    }
    for (const auto &module : *result)
      std::cout << module.value("key", "") << '\t'
                << (module.value("included", false) ? "enabled" : "disabled")
                << '\t' << module.value("source_file", "") << '\t'
                << module.value("runtime_file", "") << '\n';
  } else if (request.method == "module.get") {
    if (::isatty(STDOUT_FILENO)) {
      human_details("Native module  " + result->value("key", ""),
                    {{"Include", result->value("included", false) ? "included"
                                                                  : "excluded"},
                     {"Source file", result->value("source_file", "")},
                     {"Runtime file", result->value("runtime_file", "")}},
                    color);
      if (result->contains("text") && result->at("text").is_string())
        std::cout << "\n" << result->at("text").get<std::string>() << '\n';
      else
        std::cout << "\nNo source file saved\n";
      return 0;
    }
    std::cout << "module: " << result->value("key", "") << "\nenabled: "
              << (result->value("included", false) ? "yes" : "no")
              << "\nsource: " << result->value("source_file", "")
              << "\nruntime: " << result->value("runtime_file", "") << "\n";
    if (result->contains("text") && result->at("text").is_string())
      std::cout << result->at("text").get<std::string>();
  } else if (request.method == "group.list") {
    if (::isatty(STDOUT_FILENO)) {
      HumanRows rows;
      for (const auto &item : *result)
        rows.push_back({item.value("name", ""),
                        item.value("selection_mode", "automatic"),
                        item.value("balancer", Json::object()).value("tag", ""),
                        item.value("id", "")});
      human_table({"NAME", "MODE", "BALANCER TAG", "ID"}, rows,
                  "No groups in this Profile", color);
      return 0;
    }
    for (const auto &group : *result)
      std::cout << group.value("id", "") << '\t' << group.value("name", "")
                << '\t' << group.value("selection_mode", "automatic") << '\t'
                << group.value("balancer", Json::object()).value("tag", "")
                << '\n';
  } else if (request.method == "group.get") {
    const auto balancer = result->value("balancer", Json::object());
    const auto members = result->value("members", Json::object());
    std::cout
        << "Group " << result->value("name", "") << " ["
        << result->value("id", "") << "]\n"
        << "Profile: " << request.profile_id << '\n'
        << "Selection mode: " << result->value("selection_mode", "automatic")
        << '\n'
        << "Native balancer: " << balancer.value("tag", "") << '\n'
        << "Strategy: "
        << balancer.value("strategy", Json::object()).value("type", "unknown")
        << '\n'
        << "Members: " << members.value("kind", "unknown") << '\n';
    for (const auto &id : members.value("node_ids", Json::array()))
      if (id.is_string())
        std::cout << "  " << id.get<std::string>() << '\n';
    if (result->contains("selected_node_id") &&
        result->at("selected_node_id").is_string())
      std::cout << "Saved candidate: "
                << result->at("selected_node_id").get<std::string>() << '\n';
  } else if (request.method == "group.save") {
    std::cout << "Saved group " << result->value("name", "") << " ["
              << result->value("id", "") << "] for Profile "
              << request.profile_id << "; runtime unchanged\n";
  } else if (request.method == "group.remove") {
    std::cout << "Removed group " << request.group_id << " from Profile "
              << request.profile_id << "; runtime unchanged\n";
  } else if (request.method == "group.select") {
    std::cout << "manual group selection saved; runtime unchanged\n";
  } else if (request.method == "group.override" ||
             request.method == "group.override.clear") {
    std::cout << "runtime override " << result->value("state", "unknown");
    if (result->contains("actual_outbound") &&
        result->at("actual_outbound").is_string())
      std::cout << ": " << result->at("actual_outbound").get<std::string>();
    std::cout << "\n";
  } else if (request.method == "connection.select") {
    std::cout << "Saved target "
              << target_label(request.params.value("target", Json(nullptr)))
              << " for Profile " << request.profile_id
              << "; runtime unchanged\n";
  } else if (request.method == "connection.connect") {
    const auto effect = result->value("effect", "unknown");
    const auto active = result->value("active", Json(nullptr));
    if (effect == "running_new" && active.is_object())
      std::cout << "Connected Profile "
                << profile_identity(request, active.value("profile_id", ""))
                << "; process running; traffic unverified\n";
    else if (effect == "restored_old" && active.is_object())
      std::cout << "Previous Profile "
                << profile_identity(request, active.value("profile_id", ""))
                << " restored after connection failure; inspect status\n";
    else
      std::cout << "Connect completed with effect: " << effect
                << "; inspect status\n";
    if (result->contains("attempt_error") &&
        result->at("attempt_error").is_object())
      print_embedded_error(result->at("attempt_error"),
                           "Connection attempt failed", request.profile_id);
  } else if (request.method == "connection.disconnect") {
    std::cout << (result->value("effect", "") == "stopped"
                      ? "Core is stopped; saved Profile settings unchanged\n"
                      : "Stop completed; inspect status for actual state\n");
  } else if (request.method == "runtime.status") {
    std::cout << colored("state:", "1;36", color) << ' '
              << result->value("state", "unknown") << '\n'
              << colored("health:", "1;36", color) << ' '
              << (result->value("health", "unknown") == "process_only"
                      ? "traffic unverified"
                      : result->value("health", "unknown"))
              << "\n";
    if (result->contains("active") && result->at("active").is_object())
      std::cout << "running profile: "
                << profile_identity(
                       request, result->at("active").value("profile_id", ""))
                << '\n';
    else
      std::cout << "running profile: none\n";
    if (result->contains("active") && result->at("active").is_object())
      std::cout << "saved target (running Profile only): "
                << target_label(result->value("saved_target", Json(nullptr)))
                << '\n';
    else
      std::cout << "saved target: scope is running Profile only; inspect "
                   "`profile show <ref>`\n";
    if (result->contains("actual_target"))
      std::cout << "applied target: "
                << (result->at("actual_target").is_null()
                        ? "no running target"
                        : target_label(result->at("actual_target")))
                << '\n';
    std::cout << "runtime override: "
              << result->value("override_state", "unknown");
    if (result->contains("runtime_override") &&
        result->at("runtime_override").is_string())
      std::cout << " " << result->at("runtime_override").get<std::string>();
    std::cout << '\n';
    if (result->contains("active") && result->at("active").is_object()) {
      std::cout << "confdir: " << result->at("active").value("confdir", "")
                << '\n';
    }
  } else if (request.method == "runtime.stats") {
    const auto availability = result->value("availability", "unknown");
    std::cout << colored("metrics:", "1;36", color) << ' ' << availability
              << '\n';
    if (availability != "available") {
      const auto missing_key = result->contains("missing_conditions")
                                   ? "missing_conditions"
                                   : "missing";
      if (result->contains(missing_key) && result->at(missing_key).is_array()) {
        std::cout << "unavailable conditions:";
        for (const auto &condition : result->at(missing_key))
          if (condition.is_string())
            std::cout << ' ' << condition.get<std::string>();
        std::cout << '\n';
      }
    }
    if (result->contains("sample") && result->at("sample").is_object()) {
      const auto &sample = result->at("sample");
      if (sample.contains("received_at") && !sample["received_at"].is_null())
        std::cout << "sample timestamp: " << sample["received_at"].dump()
                  << '\n';
      if (sample.contains("traffic") && sample["traffic"].is_array()) {
        for (const auto &counter : sample["traffic"]) {
          std::cout << "traffic " << counter.value("scope", "") << '/'
                    << counter.value("tag", "") << " uplink="
                    << (counter.contains("uplink") &&
                                !counter["uplink"].is_null()
                            ? counter["uplink"].dump()
                            : "unavailable")
                    << " downlink="
                    << (counter.contains("downlink") &&
                                !counter["downlink"].is_null()
                            ? counter["downlink"].dump()
                            : "unavailable")
                    << '\n';
        }
      }
      if (sample.contains("observations") &&
          sample["observations"].is_array()) {
        for (const auto &observation : sample["observations"])
          std::cout << "observation " << observation.value("tag", "")
                    << " latency="
                    << (observation.contains("latency_ms") &&
                                !observation["latency_ms"].is_null()
                            ? observation["latency_ms"].dump() + " ms"
                            : "unavailable")
                    << '\n';
      }
    } else {
      std::cout << "sample: unavailable\n";
    }
    if (result->contains("rates") && result->at("rates").is_array()) {
      for (const auto &rate : result->at("rates"))
        std::cout << "rate " << rate.value("scope", "") << '/'
                  << rate.value("tag", "") << " uplink="
                  << (rate.contains("uplink_bytes_per_second") &&
                              !rate["uplink_bytes_per_second"].is_null()
                          ? rate["uplink_bytes_per_second"].dump() + " B/s"
                          : "pending")
                  << " downlink="
                  << (rate.contains("downlink_bytes_per_second") &&
                              !rate["downlink_bytes_per_second"].is_null()
                          ? rate["downlink_bytes_per_second"].dump() + " B/s"
                          : "pending")
                  << '\n';
    }
  } else if (request.method == "module.enable_traffic_statistics" ||
             request.method == "module.form") {
    std::cout << "Saved native module fields for Profile " << request.profile_id
              << ":";
    if (result->contains("changed_modules") &&
        result->at("changed_modules").is_array())
      for (const auto &module : result->at("changed_modules"))
        if (module.is_string())
          std::cout << ' ' << module.get<std::string>();
    std::cout << '\n';
    if (result->contains("failure") && !result->at("failure").is_null())
      print_embedded_error(result->at("failure"),
                           "Module save did not complete", request.profile_id);
    if (result->contains("notices") && result->at("notices").is_array())
      for (const auto &notice : result->at("notices"))
        if (notice.is_object())
          std::cout << notice.value("message", "") << '\n';
  } else if (request.method == "logs.read") {
    if (::isatty(STDOUT_FILENO) && result->at("entries").empty()) {
      std::cout << "No log entries\n";
      return 0;
    }
    for (const auto &entry : result->at("entries"))
      std::cout << '[' << entry.value("cursor", 0) << "]["
                << entry.value("stream", "") << "] " << entry.value("text", "");
  } else if (request.method == "config.render") {
    std::cout << "confdir: " << result->value("confdir", "") << '\n';
    for (const auto &file : result->at("files"))
      std::cout << file.get<std::string>() << '\n';
  } else if (request.method == "profile.sources.set") {
    std::cout << "Saved source bindings for Profile " << request.profile_id
              << "; running configuration unchanged\n";
  } else if (request.method == "module.save") {
    std::cout << "Saved native module " << request.module_key << " for Profile "
              << request.profile_id << "; running configuration unchanged\n";
  } else if (request.method == "module.include") {
    std::cout << (request.params.value("included", false) ? "Included "
                                                          : "Disabled ")
              << "native module " << request.module_key << " for Profile "
              << request.profile_id << "; running configuration unchanged\n";
  } else if (request.method == "subscription.add" ||
             request.method == "subscription.replace") {
    std::cout << (request.method == "subscription.add" ? "Added" : "Updated")
              << " subscription [" << result->value("id", "")
              << "]; running configuration unchanged\n";
  } else if (request.method == "subscription.remove") {
    std::cout << "Removed subscription " << request.subscription_id
              << "; running configuration unchanged\n";
  } else if (request.method == "node.update" || request.method == "node.copy" ||
             request.method == "node.set_enabled" ||
             request.method == "node.set_alias" ||
             request.method == "node.set_labels" ||
             request.method == "node.form_edit") {
    std::cout << (request.method == "node.copy" ? "Copied" : "Saved")
              << " node [" << result->value("id", "")
              << "]; running configuration unchanged\n";
  } else if (request.method == "node.remove") {
    std::cout << "Removed node " << request.node_id
              << "; running configuration unchanged\n";
  } else if (request.method == "core.remove") {
    std::cout << "Removed managed core [" << result->value("removed", "")
              << "]\n";
  } else if (request.method == "config.export") {
    std::cout << "Exported multi-file configuration to "
              << result->value("output", "") << '\n';
  } else if (request.method == "config.check" ||
             request.method == "config.apply" ||
             request.method == "core.stop" ||
             request.method == "core.restart") {
    std::cout << (request.command.empty() ? "Xray action" : request.command)
              << " completed: " << result->value("effect", "unknown") << '\n';
  } else if (request.method == "node.test.cancel") {
    std::cout << "Cancelled node test " << request.operation_id << '\n';
  } else if (request.method == "config.files" && ::isatty(STDOUT_FILENO)) {
    if (result->contains("runtime_dir")) {
      std::cout << colored("Runtime configuration", "1;36", color) << "\n"
                << "Directory: " << result->value("runtime_dir", "") << "\n";
      for (const auto &file : result->value("files", Json::array()))
        std::cout << "\n"
                  << colored(file.value("file", ""), "1", color) << "\n"
                  << file.value("text", "") << "\n";
    } else {
      std::cout << colored("Source configuration", "1;36", color) << "\n"
                << "Source:  " << result->value("source_file", "") << "\n"
                << "Runtime: " << result->value("runtime_file", "") << "\n\n";
      if (result->contains("text") && result->at("text").is_string())
        std::cout << result->at("text").get<std::string>() << '\n';
      else
        std::cout << "No source file saved\n";
    }
  } else if ((request.method == "node.test.start" ||
              request.method == "node.test.results") &&
             ::isatty(STDOUT_FILENO)) {
    HumanRows rows;
    const auto items = result->is_array() ? *result : Json::array({*result});
    for (const auto &item : items) {
      const auto number = [&](const char *key, const char *unit) {
        return item.contains(key) && item.at(key).is_number()
                   ? item.at(key).dump() + unit
                   : "unavailable";
      };
      rows.push_back(
          {item.value("kind", "unknown"),
           item.value("status", "unknown") +
               std::string(item.value("stale", false) ? "; stale" : ""),
           number("latency_ms", " ms"), number("bytes_per_second", " B/s"),
           item.value("node_id", "")});
    }
    human_table({"TEST", "RESULT", "LATENCY", "RATE", "NODE ID"}, rows,
                "No node test results", color);
    for (const auto &item : items)
      if (!item.value("message", "").empty())
        std::cout << "\n"
                  << item.value("node_id", "") << ": "
                  << terminal_text(item.value("message", "")) << '\n';
  } else if (request.method == "operation.get" && ::isatty(STDOUT_FILENO)) {
    std::cout << colored("Operation", "1;36", color) << ' '
              << result->value("operation_id", "")
              << "\nStatus: " << result->value("status", "unknown") << '\n';
    if (result->contains("error") && result->at("error").is_object())
      print_embedded_error(result->at("error"), "Operation failed",
                           request.operation_id);
    if (result->contains("result") && !result->at("result").is_null()) {
      const auto &value = result->at("result");
      if (value.is_object())
        for (const auto &[key, field] : value.items()) {
          if (field.is_string())
            std::cout << key << ": " << terminal_text(field.get<std::string>())
                      << '\n';
          else if (field.is_primitive())
            std::cout << key << ": " << field.dump() << '\n';
          else
            std::cout << key << ": " << field.size()
                      << (field.is_array() ? " items" : " fields") << '\n';
        }
      std::cout << "Full result: xrm --json operation get "
                << shell_quote(request.operation_id) << '\n';
    }
  } else if (request.method == "config.files" ||
             request.method == "config.dump" ||
             request.method == "operation.get" ||
             request.method == "node.test.start" ||
             request.method == "node.test.results") {
    // Retain the established non-terminal and explicit native-dump contract.
    std::cout << result->dump(2) << '\n';
  }
  return refresh_exit_code(request, *result);
}

} // namespace xrm::cli

// XRM: Registers geodata management commands.

#include "cli/command_options.hpp"
#include "cli/presentation.hpp"
#include "cli/rpc_session.hpp"
#include "contract/methods.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <unistd.h>

namespace xrm::cli {
namespace {

struct GeodataOptions {
  std::string name;
  std::string kind;
  std::string file_name;
  std::string url;
  std::string local_path;
  std::string resource_ref;
  std::string core_ref;
  std::string profile_ref;
  bool auto_update = false;
  std::int64_t interval_seconds = 0;
};

Result<std::string> exact_ref(RpcSession &session, std::string_view method,
                              std::string_view reference,
                              std::string_view entity) {
  auto listed = session.call(std::string(method));
  if (!listed)
    return std::unexpected(listed.error());
  if (!listed->is_array())
    return std::unexpected(AppError{ErrorCode::protocol_error,
                                    std::string(method) + "未返回列表",
                                    std::nullopt, std::nullopt});
  // Stable IDs win over display names, including a name that happens to equal
  // another ID.
  for (const auto &item : *listed)
    if (item.is_object() && item.value("id", std::string{}) == reference)
      return item.at("id").get<std::string>();
  std::optional<std::string> matched;
  for (const auto &item : *listed) {
    if (!item.is_object() || item.value("name", std::string{}) != reference ||
        !item.contains("id") || !item.at("id").is_string())
      continue;
    if (matched)
      return std::unexpected(AppError{
          ErrorCode::conflict, std::string(entity) + "名称不唯一，请使用 ID",
          std::string(reference), std::nullopt});
    matched = item.at("id").get<std::string>();
  }
  if (matched)
    return *matched;
  return std::unexpected(
      AppError{ErrorCode::not_found,
               "找不到对应" + std::string(entity) + "（请使用精确名称或 ID）",
               std::string(reference), std::nullopt});
}

Result<Json> completed_operation(RpcSession &session, Result<Json> response) {
  if (!response || !response->is_object() ||
      response->value("status", "") != "accepted" ||
      !response->contains("operation_id") ||
      !response->at("operation_id").is_string())
    return response;
  const auto id = response->at("operation_id").get<std::string>();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto operation = session.call(std::string(methods::operation_get),
                                  Json{{"operation_id", id}});
    if (!operation)
      return std::unexpected(operation.error());
    const auto status = operation->value("status", "");
    if (status == "succeeded")
      return operation->value("result", Json(nullptr));
    if (status == "failed") {
      auto error = error_from_json(operation->at("error"));
      return std::unexpected(error ? std::move(*error) : error.error());
    }
  }
  return std::unexpected(AppError{
      ErrorCode::timed_out, "等待资源更新超过5分钟；可用 operation get 查询",
      id, std::nullopt});
}

int output(const Request &request, const Result<Json> &result) {
  if (!result || request.json)
    return print_result(request, result);
  if (request.method == "cli.geodata.list") {
    if (::isatty(STDOUT_FILENO)) {
      HumanRows rows;
      for (const auto &item : *result)
        rows.push_back({item.value("name", ""), item.value("kind", ""),
                        item.value("file_name", ""),
                        item.value("last_result", "never"),
                        item.value("id", "")});
      human_table({"NAME", "KIND", "FILE", "RESULT", "ID"}, rows,
                  "No geodata resources");
      return 0;
    }
    for (const auto &resource : *result)
      std::cout << resource.value("id", "") << '\t'
                << resource.value("name", "") << '\t'
                << resource.value("kind", "") << '\t'
                << resource.value("file_name", "") << '\t'
                << resource.value("last_result", "never") << '\n';
  } else if (request.method == "cli.geodata.add") {
    std::cout << "Registered geodata " << result->value("name", "") << " ["
              << result->value("id", "")
              << "]; download and runtime unchanged\n";
  } else if (request.method == "cli.geodata.remove") {
    std::cout << "Removed geodata " << request.params.value("resource", "")
              << '\n';
  } else if (request.method == "cli.geodata.bind") {
    std::cout << "Bound geodata " << request.params.value("resource", "")
              << " to Profile " << request.params.value("profile", "")
              << "; runtime unchanged\n";
  } else if (request.method == "cli.geodata.update") {
    std::cout << "Updated and validated geodata "
              << request.params.value("resource", "") << '\n';
  }
  return 0;
}

} // namespace

void add_geodata_commands(CLI::App &app, Request &request) {
  auto options = std::make_shared<GeodataOptions>();
  auto *geodata =
      app.add_subcommand("geodata", "Manage shared GeoIP/GeoSite resources");
  auto *list =
      geodata->add_subcommand("list", "List registered shared resources");
  list->callback([&request] { request.method = "cli.geodata.list"; });

  auto *add = geodata->add_subcommand(
      "add", "Register one resource; download is a separate update");
  add->add_option("name", options->name, "Human-readable resource name")
      ->required();
  add->add_option("kind", options->kind, "geoip or geosite")
      ->required()
      ->check(CLI::IsMember({"geoip", "geosite"}));
  add->add_option("file_name", options->file_name, "Target .dat filename")
      ->required();
  auto *url = add->add_option("--url", options->url, "HTTPS resource URL");
  auto *path = add->add_option("--file", options->local_path,
                               "Local resource .dat path");
  url->excludes(path);
  add->add_flag("--auto-update", options->auto_update,
                "Enable periodic cache updates");
  add->add_option("--interval-seconds", options->interval_seconds,
                  "Refresh interval in seconds")
      ->check(CLI::NonNegativeNumber);
  add->callback([&request, options] {
    if (options->url.empty() && options->local_path.empty())
      throw CLI::ValidationError("source", "specify --url or --file");
    request.method = "cli.geodata.add";
    request.params =
        Json{{"name", options->name},
             {"kind", options->kind},
             {"file_name", options->file_name},
             {"auto_update", options->auto_update},
             {"update_interval_seconds", options->interval_seconds}};
    if (!options->url.empty())
      request.params["url"] = options->url;
    if (!options->local_path.empty())
      request.params["local_path"] = options->local_path;
  });

  auto *update = geodata->add_subcommand(
      "update", "Fetch and validate one resource with the selected Xray");
  update
      ->add_option("resource", options->resource_ref,
                   "Exact resource name or ID")
      ->required();
  update
      ->add_option("--core", options->core_ref,
                   "Exact registered core name or ID")
      ->required();
  update->callback([&request, options] {
    request.method = "cli.geodata.update";
    request.params =
        Json{{"resource", options->resource_ref}, {"core", options->core_ref}};
  });

  auto *bind =
      geodata->add_subcommand("bind", "Bind a validated resource to a Profile");
  bind->add_option("profile", options->profile_ref, "Exact Profile name or ID")
      ->required();
  bind->add_option("resource", options->resource_ref,
                   "Exact resource name or ID")
      ->required();
  bind->callback([&request, options] {
    request.method = "cli.geodata.bind";
    request.params = Json{{"profile", options->profile_ref},
                          {"resource", options->resource_ref}};
  });

  auto *remove =
      geodata->add_subcommand("remove", "Remove an unbound resource record");
  remove
      ->add_option("resource", options->resource_ref,
                   "Exact resource name or ID")
      ->required();
  remove->callback([&request, options] {
    request.method = "cli.geodata.remove";
    request.params = Json{{"resource", options->resource_ref}};
  });
}

std::optional<int> execute_geodata_cli(const Request &request) {
  if (!request.method.starts_with("cli.geodata."))
    return std::nullopt;
  RpcSession session(request.socket);
  if (request.method == "cli.geodata.list")
    return output(request, session.call(std::string(methods::geodata_list)));
  if (request.method == "cli.geodata.add")
    return output(request, session.call(std::string(methods::geodata_put),
                                        request.params));
  auto resource = exact_ref(session, methods::geodata_list,
                            request.params.value("resource", ""), "资源");
  if (!resource)
    return output(request, std::unexpected(resource.error()));
  if (request.method == "cli.geodata.remove")
    return output(request, session.call(std::string(methods::geodata_remove),
                                        Json{{"id", *resource}}));
  if (request.method == "cli.geodata.update") {
    auto core = exact_ref(session, methods::core_list,
                          request.params.value("core", ""), "核心");
    if (!core)
      return output(request, std::unexpected(core.error()));
    return output(
        request, completed_operation(
                     session, session.call(std::string(methods::geodata_update),
                                           Json{{"id", *resource},
                                                {"core_id", *core}})));
  }
  if (request.method == "cli.geodata.bind") {
    auto profile = exact_ref(session, methods::profile_list,
                             request.params.value("profile", ""), "Profile");
    if (!profile)
      return output(request, std::unexpected(profile.error()));
    auto record = session.call(std::string(methods::geodata_get),
                               Json{{"id", *resource}});
    if (!record)
      return output(request, std::unexpected(record.error()));
    if (!record->contains("current") || record->at("current").is_null())
      return output(request, std::unexpected(AppError{
                                 ErrorCode::invalid_argument,
                                 "资源尚无已验证版本，请先执行 geodata update",
                                 *resource, std::nullopt}));
    auto profile_record = session.call(std::string(methods::profile_get),
                                       Json{{"profile_id", *profile}});
    if (!profile_record)
      return output(request, std::unexpected(profile_record.error()));
    Json resources =
        profile_record->value("geodata_resource_ids", Json::object());
    if (!resources.is_object())
      return output(request, std::unexpected(AppError{ErrorCode::protocol_error,
                                                      "Profile资源绑定不是对象",
                                                      *profile, std::nullopt}));
    resources[record->at("file_name").get<std::string>()] = *resource;
    return output(request,
                  session.call(std::string(methods::profile_geodata_set),
                               Json{{"profile_id", *profile},
                                    {"resources", std::move(resources)}}));
  }
  return output(request, std::unexpected(AppError{
                             ErrorCode::invalid_argument, "未知 geodata 命令",
                             request.method, std::nullopt}));
}

} // namespace xrm::cli

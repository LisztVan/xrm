// XRM: Registers routing preset commands.

#include "cli/command_options.hpp"
#include "cli/rpc_session.hpp"
#include "contract/methods.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>

namespace xrm::cli {
namespace {

struct PresetOptions {
  std::string profile_ref;
  std::string preset;
  std::string expected_file;
};

Result<std::string> profile_id(RpcSession &session,
                               std::string_view reference) {
  auto profiles = session.call(std::string(methods::profile_list));
  if (!profiles)
    return std::unexpected(profiles.error());
  if (!profiles->is_array())
    return std::unexpected(AppError{ErrorCode::protocol_error,
                                    "profile.list未返回列表", std::nullopt,
                                    std::nullopt});
  for (const auto &profile : *profiles)
    if (profile.is_object() && profile.value("id", std::string{}) == reference)
      return profile.at("id").get<std::string>();
  std::optional<std::string> matched;
  for (const auto &profile : *profiles) {
    if (!profile.is_object() ||
        profile.value("name", std::string{}) != reference ||
        !profile.contains("id") || !profile.at("id").is_string())
      continue;
    if (matched)
      return std::unexpected(AppError{ErrorCode::conflict,
                                      "Profile名称不唯一，请使用 ID",
                                      std::string(reference), std::nullopt});
    matched = profile.at("id").get<std::string>();
  }
  if (matched)
    return *matched;
  return std::unexpected(AppError{ErrorCode::not_found,
                                  "找不到对应Profile（请使用精确名称或 ID）",
                                  std::string(reference), std::nullopt});
}

int output(const Request &request, const Result<Json> &result) {
  if (!result || request.json)
    return print_result(request, result);
  if (request.method == "cli.routing.preset.save")
    std::cout << "Saved routing preset " << request.params.value("preset", "")
              << " for Profile " << request.params.value("profile", "")
              << "; runtime unchanged\n";
  else
    std::cout
        << result->dump(2)
        << '\n'; // Explicit native-config preview, including the save baseline.
  return 0;
}

} // namespace

void add_routing_preset_commands(CLI::App &app, Request &request) {
  auto options = std::make_shared<PresetOptions>();
  auto *routing =
      app.add_subcommand("routing", "Profile routing preset operations");
  auto *preset = routing->add_subcommand(
      "preset", "Preview and save native routing presets");
  auto *plan = preset->add_subcommand(
      "plan", "Show existing and proposed native routing JSON");
  plan->add_option("profile", options->profile_ref, "Exact Profile name or ID")
      ->required();
  plan->add_option("preset", options->preset,
                   "bypass_mainland, blacklist_proxy, global_proxy, "
                   "direct_only, custom_native")
      ->required()
      ->check(CLI::IsMember({"bypass_mainland", "blacklist_proxy",
                             "global_proxy", "direct_only", "custom_native"}));
  plan->callback([&request, options] {
    request.method = "cli.routing.preset.plan";
    request.params = {{"profile", options->profile_ref},
                      {"preset", options->preset}};
  });
  auto *save = preset->add_subcommand(
      "save", "Save the planned native routing source; does not Apply");
  save->add_option("profile", options->profile_ref, "Exact Profile name or ID")
      ->required();
  save->add_option("preset", options->preset,
                   "bypass_mainland, blacklist_proxy, global_proxy, "
                   "direct_only, custom_native")
      ->required()
      ->check(CLI::IsMember({"bypass_mainland", "blacklist_proxy",
                             "global_proxy", "direct_only", "custom_native"}));
  save->add_option(
          "--expected-file", options->expected_file,
          "File with exact expected_text from a previously reviewed plan")
      ->required();
  save->callback([&request, options] {
    request.method = "cli.routing.preset.save";
    request.params = {{"profile", options->profile_ref},
                      {"preset", options->preset},
                      {"expected_file", options->expected_file}};
  });
}

std::optional<int> execute_routing_preset_cli(const Request &request) {
  if (!request.method.starts_with("cli.routing.preset."))
    return std::nullopt;
  RpcSession session(request.socket);
  auto profile = profile_id(session, request.params.value("profile", ""));
  if (!profile)
    return output(request, std::unexpected(profile.error()));
  const auto preset = request.params.value("preset", "");
  if (request.method == "cli.routing.preset.plan")
    return output(
        request,
        session.call(std::string(methods::routing_preset_plan),
                     Json{{"profile_id", *profile}, {"preset", preset}}));
  const auto file =
      std::filesystem::path(request.params.value("expected_file", ""));
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size > 8 * 1024 * 1024)
    return output(request, std::unexpected(
                               AppError{ErrorCode::io_error,
                                        "无法读取预览基线文件，或文件超过8 MiB",
                                        file.string(), std::nullopt}));
  std::ifstream input(file, std::ios::binary);
  if (!input)
    return output(request, std::unexpected(AppError{
                               ErrorCode::io_error, "无法打开预览基线文件",
                               file.string(), std::nullopt}));
  const std::string expected_text(std::istreambuf_iterator<char>{input}, {});
  if (!input.eof() && input.fail())
    return output(request, std::unexpected(AppError{
                               ErrorCode::io_error, "读取预览基线文件失败",
                               file.string(), std::nullopt}));
  auto saved = session.call(std::string(methods::routing_preset_save),
                            Json{{"profile_id", *profile},
                                 {"preset", preset},
                                 {"expected_text", expected_text}});
  return output(request, saved);
}

} // namespace xrm::cli

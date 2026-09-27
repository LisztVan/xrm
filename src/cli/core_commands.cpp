// XRM: Registers core installation and inspection commands.

#include "cli/command_options.hpp"

namespace xrm::cli {

void add_core_commands(CLI::App &app, Request &request) {
  auto *core = app.add_subcommand("core", "Register and inspect Xray binaries");
  auto *register_command =
      core->add_subcommand("register", "Register an existing Xray binary");
  register_command
      ->add_option("path", request.core_path,
                   "Absolute path to the Xray executable")
      ->required();
  register_command
      ->add_option("--name", request.core_name, "Local display name")
      ->required();
  register_command->callback([&request] {
    request.method = "core.register";
    request.params =
        Json{{"path", request.core_path}, {"name", request.core_name}};
  });

  auto *list = core->add_subcommand("list", "List registered Xray binaries");
  list->callback([&request] {
    request.method = "core.list";
    request.params = Json::object();
  });

  auto *remove =
      core->add_subcommand("remove", "Remove an unused managed Xray install; "
                                     "system binaries are never deleted");
  remove->add_option("core_id", request.core_id)->required();
  remove->callback([&request] {
    request.method = "core.remove";
    request.params = {{"core_id", request.core_id}};
  });

  auto *releases = core->add_subcommand(
      "releases", "List official Xray releases and platform assets");
  releases->add_flag("--prerelease", request.core_include_prerelease,
                     "Include prerelease versions");
  releases->callback([&request] {
    request.method = "core.releases";
    request.params = {{"include_prerelease", request.core_include_prerelease}};
  });

  auto *install = core->add_subcommand(
      "install", "Install an explicitly selected official Xray version");
  install->add_option("version", request.core_version)->required();
  install->add_option(
      "--asset", request.core_asset,
      "Exact asset name from `xrm core releases` (defaults to this platform)");
  install->add_flag(
      "--allow-unverified-checksum", request.core_allow_unverified_checksum,
      "Explicitly permit releases without an official SHA-256 digest");
  install->callback([&request] {
    request.method = "core.install.version";
    request.params = {
        {"version", request.core_version},
        {"asset_name", request.core_asset},
        {"allow_unverified_checksum", request.core_allow_unverified_checksum}};
  });

  auto *use =
      core->add_subcommand("use", "Use an installed core for a Profile");
  use->add_option("profile_id", request.profile_id)->required();
  use->add_option("version_or_id", request.core_version)->required();
  use->callback([&request] {
    request.method = "core.use.version";
    request.params = {{"profile_id", request.profile_id},
                      {"version", request.core_version}};
  });

  auto *stop = core->add_subcommand("stop", "Stop the managed Xray process");
  stop->callback([&request] {
    request.method = "core.stop";
    request.params = Json::object();
  });
  auto *restart = core->add_subcommand(
      "restart", "Restart the active Profile using saved settings");
  restart->callback([&request] {
    request.method = "core.restart";
    request.params = Json::object();
  });
}

} // namespace xrm::cli

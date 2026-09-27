// XRM: Registers Manager appearance and editor settings commands.

#include "cli/command_options.hpp"

#include <memory>

namespace xrm::cli {

void add_settings_commands(CLI::App &app, Request &request) {
  auto *settings = app.add_subcommand(
      "settings", "Manage XRM appearance and Manager editor preferences");
  auto *show = settings->add_subcommand("show", "Show XRM program settings");
  show->callback([&request] {
    request.method = "settings.get";
    request.params = Json::object();
  });

  struct SetOptions {
    std::string key, value;
  };
  auto options = std::make_shared<SetOptions>();
  auto *set = settings->add_subcommand("set", "Set one appearance preference");
  set->add_option(
         "key", options->key,
         "theme, color_mode, density, icon_mode, reduce_motion or cli_color")
      ->required();
  set->add_option("value", options->value, "Preference value")->required();
  set->callback([&request, options] {
    Json value = options->value;
    if (options->key == "reduce_motion") {
      if (options->value != "true" && options->value != "false")
        throw CLI::ValidationError("value",
                                   "reduce_motion must be true or false");
      value = options->value == "true";
    }
    request.method = "settings.update";
    request.params = {
        {"patch", {{"appearance", {{options->key, std::move(value)}}}}}};
  });

  auto *editor = settings->add_subcommand(
      "editor", "Set or clear Manager-side external editor argv");
  auto editor_argv = std::make_shared<std::string>();
  auto *configure = editor->add_subcommand(
      "set", "Set argv as a JSON string array; no shell is used");
  configure
      ->add_option("argv_json", *editor_argv,
                   "Example: '[\"/usr/bin/vi\",\"-n\"]'")
      ->required();
  configure->callback([&request, editor_argv] {
    Json argv;
    try {
      argv = Json::parse(*editor_argv);
    } catch (const Json::exception &) {
      throw CLI::ValidationError("argv_json", "must be a JSON string array");
    }
    request.method = "settings.update";
    request.params = {{"patch", {{"editor", {{"argv", std::move(argv)}}}}}};
  });
  auto *clear = editor->add_subcommand(
      "clear", "Use Manager VISUAL/EDITOR fallback again");
  clear->callback([&request] {
    request.method = "settings.update";
    request.params = {{"patch", {{"editor", {{"argv", Json::array()}}}}}};
  });
}

} // namespace xrm::cli

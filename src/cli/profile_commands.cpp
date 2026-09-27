// XRM: Registers Profile management commands.

#include "cli/command_options.hpp"

#include "contract/methods.hpp"

namespace xrm::cli {

void add_profile_commands(CLI::App &app, Request &request) {
  auto *profile = app.add_subcommand("profile", "Manage saved Profiles");
  auto *create = profile->add_subcommand(
      "create", "Create a local Profile from the built-in preset");
  create->add_option("--name", request.profile_name)->required();
  create->add_option("--core", request.core_id,
                     "Registered core ID (may be selected later)");
  create->callback([&] {
    request.method = std::string(methods::profile_create);
    request.params = Json{{"name", request.profile_name},
                          {"core_id", request.core_id},
                          {"preset", "local"}};
  });
  auto *list = profile->add_subcommand("list", "List Profiles");
  list->callback([&] {
    request.method = std::string(methods::profile_list);
    request.params = Json::object();
  });
  auto *show = profile->add_subcommand("show", "Show Profile settings");
  show->add_option("profile_id", request.profile_id)->required();
  show->callback([&] {
    request.method = std::string(methods::profile_get);
    request.params = {{"profile_id", request.profile_id}};
  });
  auto *clone =
      profile->add_subcommand("clone", "Copy a saved Profile under a new name");
  clone->add_option("profile_id", request.profile_id)->required();
  clone->add_option("--name", request.profile_name)->required();
  clone->callback([&] {
    request.method = std::string(methods::profile_clone);
    request.params = {{"profile_id", request.profile_id},
                      {"name", request.profile_name}};
  });
  auto *rename = profile->add_subcommand("rename", "Rename a saved Profile");
  rename->add_option("profile_id", request.profile_id)->required();
  rename->add_option("--name", request.profile_name)->required();
  rename->callback([&] {
    request.method = std::string(methods::profile_rename);
    request.params = {{"profile_id", request.profile_id},
                      {"name", request.profile_name}};
  });
  auto *remove = profile->add_subcommand("remove", "Delete a saved Profile");
  remove->add_option("profile_id", request.profile_id)->required();
  remove->callback([&] {
    request.method = std::string(methods::profile_remove);
    request.params = {{"profile_id", request.profile_id}};
  });
  auto *sources = profile->add_subcommand(
      "sources", "Set the subscription sources used by one Profile");
  auto *source_set = sources->add_subcommand(
      "set", "Replace source bindings; no --source means use none");
  source_set->add_option("profile_id", request.profile_id)->required();
  source_set->add_option("--source", request.profile_source_ids,
                         "Source ID to use; repeat for multiple sources");
  source_set->callback([&] {
    request.method = std::string(methods::profile_sources_set);
    request.params = {{"profile_id", request.profile_id},
                      {"source_ids", request.profile_source_ids}};
  });

  auto *config =
      app.add_subcommand("config", "Inspect and edit native Xray modules");
  auto *modules = config->add_subcommand(
      "modules", "List all native Xray modules and their source/runtime files");
  modules->add_option("profile_id", request.profile_id)->required();
  modules->callback([&] {
    request.method = std::string(methods::module_list);
    request.params = {{"profile_id", request.profile_id}};
  });
  auto *show_module = config->add_subcommand(
      "show", "Show one source module or the generated runtime files");
  show_module->add_option("profile_id", request.profile_id)->required();
  show_module->add_option("module", request.module_key);
  show_module->add_option("--module-key", request.module_show_key,
                          "Native Xray module key, for example dns or routing");
  show_module->add_option("--source", request.source)
      ->check(CLI::IsMember({"source", "runtime"}));
  show_module->callback([&] {
    if (!request.module_show_key.empty())
      request.module_key = request.module_show_key;
    if (request.source == "source" && !request.module_key.empty()) {
      request.method = std::string(methods::module_get);
      request.params = {{"profile_id", request.profile_id},
                        {"module", request.module_key}};
    } else {
      request.method = std::string(methods::config_files);
      request.params = {{"profile_id", request.profile_id},
                        {"source", request.source}};
      if (!request.module_key.empty())
        request.params["module"] = request.module_key;
    }
  });
  auto *edit =
      config->add_subcommand("edit", "Save a module's original JSON text");
  edit->add_option("profile_id", request.profile_id)->required();
  edit->add_option("module", request.module_key)->required();
  auto *text = edit->add_option("--text", request.module_text,
                                "Native module JSON passed as an argument");
  auto *file =
      edit->add_option("--file", request.module_file,
                       "Read native module JSON from PATH, or - for stdin");
  text->excludes(file);
  edit->callback([&, text, file] {
    if (text->count() == 0 && file->count() == 0)
      throw CLI::ValidationError("config edit",
                                 "specify exactly one of --text or --file");
    request.module_file_specified = file->count() != 0;
    request.method = std::string(methods::module_save);
    request.params = {{"profile_id", request.profile_id},
                      {"module", request.module_key},
                      {"text", request.module_text}};
  });
  for (const auto &[name, included] :
       {std::pair{"enable", true}, std::pair{"disable", false}}) {
    auto *toggle = config->add_subcommand(
        name, included ? "Include a native module in runtime"
                       : "Disable runtime output for a native module");
    toggle->add_option("profile_id", request.profile_id)->required();
    toggle->add_option("module", request.module_key)->required();
    toggle->callback([&, included] {
      request.method = std::string(methods::module_include);
      request.params = {{"profile_id", request.profile_id},
                        {"module", request.module_key},
                        {"included", included}};
    });
  }
  auto *statistics = config->add_subcommand(
      "statistics", "Explicitly edit native modules for traffic statistics");
  auto *enable_statistics = statistics->add_subcommand(
      "enable", "Enable stats, policy counters and the Metrics endpoint");
  enable_statistics->add_option("profile_id", request.profile_id)->required();
  enable_statistics
      ->add_option("--metrics-listen", request.metrics_listen,
                   "Host:port to write into the native metrics module")
      ->required();
  enable_statistics->callback([&request] {
    // run_cli handles this local marker by requesting a plan first, then saving
    // exactly the listed native modules through module.save_changes.
    request.method = "module.enable_traffic_statistics";
    request.params = {
        {"profile_id", request.profile_id},
        {"action", "enable_traffic_statistics"},
        {"arguments", {{"metrics_listen", request.metrics_listen}}}};
  });

  auto *env = config->add_subcommand(
      "env", "Edit Xray's native env module, not xrmd's process environment");
  auto *env_set =
      env->add_subcommand("set", "Set one native Xray environment value");
  env_set->add_option("profile_id", request.profile_id)->required();
  env_set->add_option("key", request.form_key)->required();
  env_set->add_option("value", request.form_value)->required();
  env_set->callback([&request] {
    request.method = "module.form";
    request.params = {
        {"profile_id", request.profile_id},
        {"action", "env.set"},
        {"arguments",
         {{"key", request.form_key}, {"value", request.form_value}}}};
  });
  auto *env_unset =
      env->add_subcommand("unset", "Remove one native Xray environment value");
  env_unset->add_option("profile_id", request.profile_id)->required();
  env_unset->add_option("key", request.form_key)->required();
  env_unset->callback([&request] {
    request.method = "module.form";
    request.params = {{"profile_id", request.profile_id},
                      {"action", "env.unset"},
                      {"arguments", {{"key", request.form_key}}}};
  });

  auto *version = config->add_subcommand(
      "version", "Edit the native Xray minimum/maximum version range");
  auto *version_range = version->add_subcommand(
      "range", "Set or clear user-authored min/max fields");
  version_range->add_option("profile_id", request.profile_id)->required();
  version_range->add_option("--min", request.form_min);
  version_range->add_option("--max", request.form_max);
  version_range->callback([&request] {
    request.method = "module.form";
    request.params = {
        {"profile_id", request.profile_id},
        {"action", "version.range"},
        {"arguments",
         {{"min",
           request.form_min.empty() ? Json(nullptr) : Json(request.form_min)},
          {"max", request.form_max.empty() ? Json(nullptr)
                                           : Json(request.form_max)}}}};
  });

  auto *geodata = config->add_subcommand(
      "geodata", "Edit geodata resource references and update schedule");
  auto *geodata_schedule = geodata->add_subcommand(
      "schedule", "Set the Xray-native geodata update schedule");
  geodata_schedule->add_option("profile_id", request.profile_id)->required();
  geodata_schedule->add_option("cron", request.form_cron)->required();
  geodata_schedule->callback([&request] {
    request.method = "module.form";
    request.params = {{"profile_id", request.profile_id},
                      {"action", "geodata.schedule"},
                      {"arguments", {{"cron", request.form_cron}}}};
  });
  auto *geodata_assets = geodata->add_subcommand(
      "assets", "Replace geodata asset references from a JSON array");
  geodata_assets->add_option("profile_id", request.profile_id)->required();
  geodata_assets->add_option("--json", request.form_json)->required();
  geodata_assets->callback([&request] {
    request.method = "module.form";
    Json assets = nullptr;
    try {
      assets = Json::parse(request.form_json);
    } catch (const Json::exception &) {
    }
    request.params = {{"profile_id", request.profile_id},
                      {"action", "geodata.assets"},
                      {"arguments", {{"assets", std::move(assets)}}}};
  });

  auto *api = config->add_subcommand(
      "api",
      "Edit native Xray API services without automatically opening the API");
  auto *api_services = api->add_subcommand("services", "Set API services");
  api_services->add_option("profile_id", request.profile_id)->required();
  api_services->add_option("services", request.form_values)->required();
  api_services->callback([&request] {
    request.method = "module.form";
    request.params = {{"profile_id", request.profile_id},
                      {"action", "api.services"},
                      {"arguments", {{"services", request.form_values}}}};
  });

  auto *metrics = config->add_subcommand(
      "metrics", "Edit the native Xray Metrics listener");
  auto *metrics_listen =
      metrics->add_subcommand("listen", "Set a Metrics listen address");
  metrics_listen->add_option("profile_id", request.profile_id)->required();
  metrics_listen->add_option("listen", request.metrics_listen)->required();
  metrics_listen->callback([&request] {
    request.method = "module.form";
    request.params = {{"profile_id", request.profile_id},
                      {"action", "metrics.listen"},
                      {"arguments", {{"listen", request.metrics_listen}}}};
  });

  auto *log = config->add_subcommand("log", "Edit common native log fields");
  auto *log_level = log->add_subcommand("level", "Set native Xray log level");
  log_level->add_option("profile_id", request.profile_id)->required();
  log_level->add_option("level", request.form_value)->required();
  log_level->callback([&request] {
    request.method = "module.form";
    request.params = {{"profile_id", request.profile_id},
                      {"action", "log.level"},
                      {"arguments", {{"level", request.form_value}}}};
  });
  for (const auto &[name, method] :
       {std::pair{"render", methods::config_render},
        std::pair{"check", methods::config_check},
        std::pair{"dump", methods::config_dump},
        std::pair{"apply", methods::config_apply}}) {
    auto *action = config->add_subcommand(
        name, name == "render"  ? "Materialize a multi-file preview"
              : name == "check" ? "Run Xray's complete configuration check"
              : name == "dump"
                  ? "Show Xray diagnostic dump without writing it back"
                  : "Check saved settings while stopped, or apply them to the "
                    "running core");
    action->add_option("profile_id", request.profile_id)->required();
    action->callback([&, method] {
      request.method = std::string(method);
      request.params = {{"profile_id", request.profile_id}};
    });
  }
  auto *export_command =
      config->add_subcommand("export", "Export generated multi-file confdir");
  export_command->add_option("profile_id", request.profile_id)->required();
  export_command
      ->add_option("--source", request.export_source,
                   "Saved candidate or actual running confdir")
      ->check(CLI::IsMember({"saved", "runtime"}));
  export_command->add_option("--output", request.output_path)->required();
  export_command->callback([&] {
    request.method = std::string(methods::config_export);
    request.params = {{"profile_id", request.profile_id},
                      {"source", request.export_source},
                      {"output", request.output_path}};
  });
}

} // namespace xrm::cli

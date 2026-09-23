#include "cli/command_options.hpp"

#include "contract/methods.hpp"

namespace xrm::cli {

void add_profile_commands(CLI::App& app, Request& request) {
  auto* profile = app.add_subcommand("profile", "Manage saved Profiles");
  auto* create = profile->add_subcommand("create", "Create a local Profile from the built-in preset");
  create->add_option("--name", request.profile_name)->required();
  create->add_option("--core", request.core_id)->required();
  create->callback([&] {
    request.method = std::string(methods::profile_create);
    request.params = Json{{"name", request.profile_name}, {"core_id", request.core_id}, {"preset", "local"}};
  });
  auto* list = profile->add_subcommand("list", "List Profiles");
  list->callback([&] { request.method = std::string(methods::profile_list); request.params = Json::object(); });
  auto* show = profile->add_subcommand("show", "Show Profile settings");
  show->add_option("profile_id", request.profile_id)->required();
  show->callback([&] { request.method = std::string(methods::profile_get); request.params = {{"profile_id", request.profile_id}}; });

  auto* config = app.add_subcommand("config", "Inspect and edit native Xray modules");
  auto* modules = config->add_subcommand("modules", "List all native Xray modules and their source/runtime files");
  modules->add_option("profile_id", request.profile_id)->required();
  modules->callback([&] { request.method = std::string(methods::module_list); request.params = {{"profile_id", request.profile_id}}; });
  auto* show_module = config->add_subcommand("show", "Show one source module or the generated runtime files");
  show_module->add_option("profile_id", request.profile_id)->required();
  show_module->add_option("module", request.module_key);
  show_module->add_option("--source", request.source)->check(CLI::IsMember({"source", "runtime"}));
  show_module->callback([&] {
    if (request.source == "source" && !request.module_key.empty()) {
      request.method = std::string(methods::module_get);
      request.params = {{"profile_id", request.profile_id}, {"module", request.module_key}};
    } else {
      request.method = std::string(methods::config_files);
      request.params = {{"profile_id", request.profile_id}, {"source", request.source}};
      if (!request.module_key.empty()) request.params["module"] = request.module_key;
    }
  });
  auto* edit = config->add_subcommand("edit", "Save a module's original JSON text");
  edit->add_option("profile_id", request.profile_id)->required();
  edit->add_option("module", request.module_key)->required();
  edit->add_option("--text", request.module_text)->required();
  edit->callback([&] {
    request.method = std::string(methods::module_save);
    request.params = {{"profile_id", request.profile_id}, {"module", request.module_key}, {"text", request.module_text}};
  });
  for (const auto& [name, included] : {std::pair{"enable", true}, std::pair{"disable", false}}) {
    auto* toggle = config->add_subcommand(name, included ? "Include a native module in runtime" : "Disable runtime output for a native module");
    toggle->add_option("profile_id", request.profile_id)->required();
    toggle->add_option("module", request.module_key)->required();
    toggle->callback([&, included] {
      request.method = std::string(methods::module_include);
      request.params = {{"profile_id", request.profile_id}, {"module", request.module_key}, {"included", included}};
    });
  }
  auto* statistics = config->add_subcommand("statistics", "Explicitly edit native modules for traffic statistics");
  auto* enable_statistics = statistics->add_subcommand("enable", "Enable stats, policy counters and the Metrics endpoint");
  enable_statistics->add_option("profile_id", request.profile_id)->required();
  enable_statistics->add_option("--metrics-listen", request.metrics_listen,
      "Host:port to write into the native metrics module")->required();
  enable_statistics->callback([&request] {
    // run_cli handles this local marker by requesting a plan first, then saving
    // exactly the listed native modules through module.save_changes.
    request.method = "module.enable_traffic_statistics";
    request.params = {{"profile_id", request.profile_id},
                      {"action", "enable_traffic_statistics"},
                      {"arguments", {{"metrics_listen", request.metrics_listen}}}};
  });
  for (const auto& [name, method] : {std::pair{"render", methods::config_render},
                                     std::pair{"check", methods::config_check},
                                     std::pair{"dump", methods::config_dump},
                                     std::pair{"apply", methods::config_apply}}) {
    auto* action = config->add_subcommand(name, name == "render" ? "Materialize a multi-file preview" :
        name == "check" ? "Run Xray's complete configuration check" :
        name == "dump" ? "Show Xray diagnostic dump without writing it back" :
        "Check saved settings while stopped, or apply them to the running core");
    action->add_option("profile_id", request.profile_id)->required();
    action->callback([&, method] {
      request.method = std::string(method);
      request.params = {{"profile_id", request.profile_id}};
    });
  }
  auto* export_command = config->add_subcommand("export", "Export generated multi-file confdir");
  export_command->add_option("profile_id", request.profile_id)->required();
  export_command->add_option("--source", request.source)->check(CLI::IsMember({"saved", "runtime"}));
  export_command->add_option("--output", request.output_path)->required();
  export_command->callback([&] {
    request.method = std::string(methods::config_export);
    request.params = {{"profile_id", request.profile_id}, {"source", request.source}, {"output", request.output_path}};
  });
}

}  // namespace xrm::cli

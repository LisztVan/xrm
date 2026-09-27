// XRM: Builds context-sensitive help from the registered CLI command tree.

#pragma once

#include <CLI/CLI.hpp>
#include <cctype>
#include <map>
#include <sstream>
#include <string>

namespace xrm::cli {

class CommandHelpFormatter final : public CLI::Formatter {
public:
  std::string make_option_name(const CLI::Option *option,
                               bool positional) const override {
    if (!positional)
      return CLI::Formatter::make_option_name(option, false);
    const auto type = option->get_type_name();
    return type.substr(0, type.find(':'));
  }

  std::string make_option_opts(const CLI::Option *option) const override {
    if (!option->get_option_text().empty())
      return " " + option->get_option_text();
    if (option->get_items_expected() == 0)
      return {};
    const auto type = option->get_type_name();
    std::string out =
        option->get_positional() ? "" : " " + type.substr(0, type.find(':'));
    if (option->get_expected_max() == CLI::detail::expected_max_vector_size)
      out += " ...";
    if (option->get_required())
      out += " (required)";
    return out;
  }

  std::string make_option_desc(const CLI::Option *option) const override {
    std::string out = option->get_description();
    const auto type = option->get_type_name();
    if (const auto start = type.find(":{"); start != std::string::npos) {
      const auto end = type.find('}', start);
      auto choices = type.substr(start + 2, end - start - 2);
      for (std::size_t i = 0; (i = choices.find(',', i)) != std::string::npos;
           i += 2)
        choices.insert(i + 1, " ");
      out += " Choices: " + choices + ".";
    }
    if (!option->get_default_str().empty())
      out += " Default: " + option->get_default_str() + ".";
    if (!option->get_excludes().empty()) {
      out += " Cannot combine with";
      for (const auto *excluded : option->get_excludes())
        out += " " + excluded->get_name();
      out += ".";
    }
    if (!option->get_needs().empty()) {
      out += " Requires";
      for (const auto *required : option->get_needs())
        out += " " + required->get_name();
      out += ".";
    }
    return out;
  }

  std::string make_help(const CLI::App *app, std::string name,
                        CLI::AppFormatMode mode) const override {
    auto usage = make_usage(app, std::move(name));
    while (!usage.empty() && usage.front() == '\n')
      usage.erase(0, 1);
    while (!usage.empty() && usage.back() == '\n')
      usage.pop_back();
    std::string result = app->get_description() + "\n\nUsage: " + usage + "\n" +
                         make_positionals(app) + make_groups(app, mode) +
                         make_subcommands(app, mode);
    if (app->get_parent()) {
      const auto *root = app;
      while (root->get_parent())
        root = root->get_parent();
      result += "\nGLOBAL OPTIONS (before the command):\n";
      for (const auto *option : root->get_options()) {
        if (option == root->get_help_ptr() || option->get_name() == "--version")
          continue;
        result += make_option(option, false);
      }
    }
    result += "\n" + app->get_footer();
    while (!result.empty() && result.back() == '\n')
      result.pop_back();
    return result + "\n";
  }

  std::string make_subcommands(const CLI::App *app,
                               CLI::AppFormatMode mode) const override {
    if (app->get_parent())
      return CLI::Formatter::make_subcommands(app, mode);
    std::string result;
    for (const auto *group :
         {"Interactive", "Saved configuration", "Inventory and resources",
          "Run and observe", "XRM settings and server"}) {
      const auto children = app->get_subcommands(
          [&](const CLI::App *child) { return child->get_group() == group; });
      if (children.empty())
        continue;
      result += std::string("\n") + group + ":\n";
      for (const auto *child : children)
        result += make_subcommand(child);
    }
    return result;
  }
};

// Descriptions, parameters and child commands come from CLI11's registered
// tree. Only task examples are editorial: they never stand in for command
// registration.
inline void configure_help(CLI::App &root) {
  auto formatter = std::make_shared<CommandHelpFormatter>();
  formatter->column_width(26);
  formatter->right_column_width(52);
  const std::map<std::string, std::string> groups{
      {"tui", "Interactive"},
      {"profile", "Saved configuration"},
      {"routing", "Saved configuration"},
      {"config", "Saved configuration"},
      {"select", "Saved configuration"},
      {"group", "Saved configuration"},
      {"node", "Inventory and resources"},
      {"sub", "Inventory and resources"},
      {"core", "Inventory and resources"},
      {"geodata", "Inventory and resources"},
      {"connect", "Run and observe"},
      {"disconnect", "Run and observe"},
      {"status", "Run and observe"},
      {"logs", "Run and observe"},
      {"stats", "Run and observe"},
      {"operation", "Run and observe"},
      {"server", "XRM settings and server"},
      {"settings", "XRM settings and server"}};
  for (auto *child : root.get_subcommands([](CLI::App *) { return true; }))
    if (const auto group = groups.find(child->get_name());
        group != groups.end())
      child->group(group->second);

  const std::map<std::string, std::string> examples{
      {"", "xrm tui\nxrm profile list\nxrm profile create --name Home\nxrm "
           "config modules Home\nxrm connect Home"},
      {"server", "xrm server info"},
      {"profile", "xrm profile list\nxrm profile create --name Home\nxrm "
                  "profile show Home"},
      {"profile create", "xrm profile create --name Home"},
      {"profile show", "xrm profile show Home"},
      {"profile clone", "xrm profile clone Home --name Office"},
      {"profile rename", "xrm profile rename Home --name Office"},
      {"profile remove", "xrm profile remove Office"},
      {"profile sources", "xrm profile sources set Home"},
      {"profile sources set", "xrm profile sources set Home"},
      {"config", "xrm config modules Home\nxrm config show Home dns\nxrm "
                 "config check Home"},
      {"config modules", "xrm config modules Home"},
      {"config show",
       "xrm config show Home dns\nxrm config show Home --source runtime"},
      {"config edit",
       "xrm config edit Home dns --file ./dns.json\ncat ./dns.json | xrm "
       "config edit Home dns --file -\nxrm config edit Home dns --text "
       "'{\"dns\":{\"servers\":[\"localhost\"]}}'"},
      {"config enable", "xrm config enable Home dns"},
      {"config disable", "xrm config disable Home dns"},
      {"config statistics",
       "xrm config statistics enable Home --metrics-listen 127.0.0.1:11111"},
      {"config statistics enable",
       "xrm config statistics enable Home --metrics-listen 127.0.0.1:11111"},
      {"node", "xrm node list\nxrm node add --name Direct --outbound-json "
               "'{\"protocol\":\"freedom\",\"settings\":{}}'"},
      {"node edit", "xrm node edit NODE_ID mux --json '{\"enabled\":false}'"},
      {"node replace", "xrm node replace NODE_ID --name Direct --settings-json "
                       "'{\"kind\":\"raw\",\"value\":{\"protocol\":\"freedom\","
                       "\"settings\":{}}}'"},
      {"sub edit", "xrm sub edit SOURCE_ID --name Local"},
      {"group override", "xrm group override proxy --runtime --clear"},
      {"group edit",
       "xrm group edit Home --json "
       "'{\"id\":\"GROUP_ID\",\"name\":\"Manual\",\"members\":{\"kind\":"
       "\"explicit\",\"node_ids\":[\"NODE_ID\"]},\"selection_mode\":\"manual\","
       "\"selected_node_id\":\"NODE_ID\",\"balancer\":{\"tag\":\"proxy\","
       "\"strategy\":{\"type\":\"random\"}}}'"},
      {"node history confirm", "xrm node history confirm SOURCE_ID --id "
                               "NODE_ID --token PREVIEW_TOKEN --yes"},
      {"geodata add",
       "xrm geodata add Local geoip geoip.dat --file /tmp/geoip.dat"},
      {"config env set",
       "xrm config env set Home XRAY_LOCATION_ASSET /opt/xray"},
      {"config env unset", "xrm config env unset Home XRAY_LOCATION_ASSET"},
      {"config geodata assets", "xrm config geodata assets Home --json '[]'"},
      {"config geodata schedule",
       "xrm config geodata schedule Home '0 0 * * *'"},
      {"config api services", "xrm config api services Home StatsService"},
      {"config metrics listen",
       "xrm config metrics listen Home 127.0.0.1:11111"},
      {"config log level", "xrm config log level Home warning"},
      {"routing preset plan", "xrm routing preset plan Home direct_only"},
      {"routing preset save", "xrm routing preset save Home direct_only "
                              "--expected-file /tmp/routing-baseline.json"},
      {"node add", "xrm node add --name Direct --outbound-json "
                   "'{\"protocol\":\"freedom\",\"settings\":{}}'"},
      {"sub", "xrm sub list\nxrm sub show Local\nxrm sub add --name Local "
              "--url http://127.0.0.1:8080/sub --paused"},
      {"sub add",
       "xrm sub add --name Local --url http://127.0.0.1:8080/sub --paused"},
      {"core", "xrm core list\nxrm core register /opt/xray/xray --name Xray"},
      {"core register", "xrm core register /opt/xray/xray --name Xray"},
      {"core install", "xrm core install v26.9.9"},
      {"core use", "xrm core use Home v26.9.9"},
      {"group", "xrm group list Home"},
      {"group list", "xrm group list Home"},
      {"select", "xrm select node --help\nxrm select group --help"},
      {"connect", "xrm connect Home"},
      {"stats", "xrm stats\nxrm --json stats --watch"},
      {"logs", "xrm logs --limit 100"},
      {"settings", "xrm settings show\nxrm settings set cli_color never"},
      {"settings set", "xrm settings set cli_color never"},
      {"settings editor", "xrm settings editor set '[\"/usr/bin/vi\",\"-n\"]'"},
      {"settings editor set",
       "xrm settings editor set '[\"/usr/bin/vi\",\"-n\"]'"},
      {"geodata", "xrm geodata list\nxrm geodata add --help"}};

  const auto visit = [&](auto &&self, CLI::App &command,
                         const std::string &path) -> void {
    command.formatter(formatter);
    command.subcommand_fallthrough(false);
    for (auto *option : command.get_options()) {
      if (option->get_items_expected() > 0) {
        auto metavar = option->get_name();
        while (metavar.starts_with('-'))
          metavar.erase(0, 1);
        if (metavar == "profile_id" || metavar == "profile")
          metavar = "PROFILE";
        else if (metavar == "socket" || metavar == "file" ||
                 metavar == "output" || metavar == "path" ||
                 metavar == "expected-file")
          metavar = "PATH";
        else if (metavar == "module-key")
          metavar = "MODULE";
        else if (metavar == "text")
          metavar = "JSON_TEXT";
        else
          for (auto &c : metavar)
            c = c == '-' ? '_'
                         : static_cast<char>(
                               std::toupper(static_cast<unsigned char>(c)));
        option->type_name(metavar);
      }
      if (!option->get_required() && option->get_items_expected() > 0 &&
          option->get_name() != "--socket")
        option->capture_default_str();
      if (option->get_description().empty()) {
        const auto name = option->get_name();
        if (name == "--name")
          option->description("Human-readable object name");
        else if (name == "profile_id")
          option->description(
              "Exact Profile name or UUID (saved configuration)");
        else if (name == "module")
          option->description(
              "Native Xray module key, for example dns or routing");
        else if (name == "node_id" || name == "id")
          option->description("Stable node ID from node list");
        else if (name == "source_id")
          option->description("Stable source ID from sub list");
        else if (name == "group_id")
          option->description("Stable group ID from group list");
        else if (name == "operation_id")
          option->description("Operation ID returned by the server");
      }
    }
    std::string footer = "Examples:\n";
    if (const auto example = examples.find(path); example != examples.end()) {
      std::istringstream lines(example->second);
      for (std::string line; std::getline(lines, line);)
        footer += "  " + line + "\n";
      if (!footer.empty())
        footer.pop_back();
    } else if (command.get_subcommands([](CLI::App *) { return true; })
                   .empty()) {
      footer += "  xrm " + path;
      const std::map<std::string, std::string> values{
          {"profile_id", "Home"},     {"profile", "Home"},
          {"--name", "Example"},      {"module", "dns"},
          {"--members", "NODE_ID"},   {"--tag", "proxy"},
          {"--values", "local,test"}, {"--output", "/tmp/xrm-export"},
          {"resource", "Local"},      {"--core", "Xray"},
          {"node_id", "NODE_ID"},     {"id", "NODE_ID"},
          {"source_id", "SOURCE_ID"}, {"group_id", "GROUP_ID"},
          {"core_id", "CORE_ID"},     {"operation_id", "OPERATION_ID"}};
      for (const auto *option : command.get_options()) {
        if (!option->get_required())
          continue;
        const auto name = option->get_name();
        footer += ' ';
        if (name.starts_with('-'))
          footer += name + " ";
        const auto value = values.find(name);
        footer += value == values.end() ? "VALUE" : value->second;
      }
    } else {
      const auto children =
          command.get_subcommands([](CLI::App *) { return true; });
      footer +=
          "  xrm " + path + " " + children.front()->get_name() + " --help";
    }
    footer += "\n\n";
    if (!command.get_subcommands([](CLI::App *) { return true; }).empty())
      footer += "Next: xrm " + (path.empty() ? "" : path + " ") +
                "<command> --help\n";
    if (path.empty())
      footer += "Help is local and never connects to xrmd.\n";
    bool has_profile = false;
    bool has_input_json = false;
    for (const auto *option : command.get_options()) {
      has_profile = has_profile || option->get_name() == "profile_id" ||
                    option->get_name() == "profile";
      has_input_json = has_input_json || option->get_name() == "--json";
    }
    if (has_profile)
      footer += "Profile accepts an exact saved name or UUID.\n";
    if (path.empty())
      footer += "Saving and selecting never connect. Use connect to apply "
                "saved settings.\n"
                "Machine output: xrm --json <command>; errors use stderr.\n";
    else if (has_input_json)
      footer += "For machine output, place global --json before the command.\n";
    command.footer(std::move(footer));
    for (auto *child : command.get_subcommands([](CLI::App *) { return true; }))
      self(self, *child,
           path.empty() ? child->get_name() : path + " " + child->get_name());
  };
  visit(visit, root, "");
}

inline const CLI::App &parsed_context(const CLI::App &root) {
  const auto *current = &root;
  while (!current->get_subcommands().empty())
    current = current->get_subcommands().back();
  return *current;
}

inline std::string command_path(const CLI::App &command) {
  if (!command.get_parent())
    return "xrm";
  return command_path(*command.get_parent()) + " " + command.get_name();
}

} // namespace xrm::cli

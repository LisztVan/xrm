// XRM: Renders core, geodata, test, and Manager settings pages.

#include "client/human_message.hpp"
#include "tui/components/style.hpp"
#include "tui/shell/workspace.hpp"

#include <algorithm>
#include <limits>
#include <sys/utsname.h>

namespace xrm::tui {
namespace {

std::vector<ui::Choice> core_choices(const Json &cores) {
  std::vector<ui::Choice> choices{{"", "No validator selected"}};
  for (const auto &core : cores)
    choices.push_back(
        {core.value("id", ""),
         core.value("name", "") + " · " + core.value("reported_version", "")});
  return choices;
}

std::pair<std::string, std::string> host_target() {
  utsname machine{};
  if (uname(&machine) != 0)
    return {};
  std::string platform = machine.sysname;
  if (platform == "Linux")
    platform = "linux";
  else if (platform == "Darwin")
    platform = "macos";
  else if (platform == "FreeBSD")
    platform = "freebsd";
  else if (platform == "OpenBSD")
    platform = "openbsd";
  std::string architecture = machine.machine;
  if (architecture == "amd64")
    architecture = "x86_64";
  else if (architecture == "arm64")
    architecture = "aarch64";
  else if (architecture == "i386" || architecture == "i686")
    architecture = "x86";
  return {std::move(platform), std::move(architecture)};
}

} // namespace

void Workspace::core_form() {
  form(
      "Register Xray core",
      {text_field("Name", "", true), text_field("Executable path", "", true)},
      [this](ui::Form &f) -> std::optional<std::string> {
        auto registered =
            call("core.register", {{"name", f.value(0)}, {"path", f.value(1)}});
        if (!registered) {
          const auto message =
              human::error(registered.error(), "Register Xray core", f.value(0),
                           "core.register");
          f.set_field_error(1, message.reason + " " + message.recovery);
          return message.reason + " " + message.recovery;
        }
        releases_view_ = false;
        refresh();
        cores_table_->select_id(registered->value("id", ""));
        notify("Registered " + f.value(0) + "; no core started",
               ui::Tone::Success);
        return {};
      },
      "Register");
}

void Workspace::core_releases(bool prerelease) {
  operation("core.releases", {{"include_prerelease", prerelease}},
            "Core releases");
}

void Workspace::core_install() {
  if (!releases_view_) {
    notify("Load official releases before installing", ui::Tone::Warning);
    return;
  }
  const auto tag = cores_table_->selected_id();
  auto found = std::ranges::find_if(releases_, [&](const Json &release) {
    return release.value("tag", "") == tag;
  });
  if (found == releases_.end()) {
    notify("Select a release to install", ui::Tone::Warning);
    return;
  }
  const Json release = *found;
  const auto [platform, architecture] = host_target();
  std::vector<ui::Choice> assets{{"", "Choose a release asset"}};
  std::string preferred;
  for (const auto &asset : release.value("assets", Json::array())) {
    const auto name = asset.value("name", "");
    assets.push_back({name, name + " · " + asset.value("platform", "") + "/" +
                                asset.value("architecture", "")});
    if (asset.value("platform", "") == platform &&
        asset.value("architecture", "") == architecture)
      preferred = name;
  }
  auto version = text_field("Release", tag);
  version.readonly = true;
  auto asset = select_field("Asset", preferred, std::move(assets));
  asset.validate = [](const std::string &value) -> std::optional<std::string> {
    if (value.empty())
      return "Choose an asset";
    return {};
  };
  auto checksum = bool_field("Allow no checksum", false);
  checksum.hint = "Only enable when you explicitly accept an asset without an "
                  "official SHA-256 digest.";
  form(
      "Install Xray", {version, asset, checksum},
      [this, release, tag](ui::Form &f) -> std::optional<std::string> {
        auto result =
            call("core.install",
                 {{"request",
                   {{"release", release},
                    {"asset_name", f.value(1)},
                    {"allow_unverified_checksum", f.value(2) == "true"}}}});
        if (!result)
          return result.error().message;
        const auto id = result->value("operation_id", "");
        if (id.empty())
          return "Server accepted no operation ID; check core list before "
                 "retrying";
        operations_[id] = "Install " + tag;
        notify("Install " + tag + " accepted; core is not selected or started");
        return {};
      },
      "Install");
}

void Workspace::geodata_form(bool editing) {
  const auto id = geodata_table_->selected_id();
  Json original = Json::object();
  if (editing) {
    auto resource = call("geodata.get", {{"id", id}});
    if (!resource) {
      notify(last_error_, ui::Tone::Error);
      return;
    }
    original = *resource;
  }
  const auto url = original.value("url", "");
  auto source =
      text_field("Source URL or local path",
                 url.empty() ? original.value("local_path", "") : url, true);
  source.kind = ui::FieldKind::Secret;
  source.hint = "Source type determines whether this is a URL or a "
                "server-local .dat file.";
  std::vector<ui::Field> fields{
      text_field("Name", original.value("name", ""), true),
      select_field("Resource type", original.value("kind", "geoip"),
                   {{"geoip", "GeoIP"}, {"geosite", "GeoSite"}}),
      text_field("Runtime filename", original.value("file_name", "geoip.dat"),
                 true),
      select_field("Source type",
                   url.empty() && !original.value("local_path", "").empty()
                       ? "file"
                       : "url",
                   {{"url", "HTTP(S) URL"}, {"file", "Local file on server"}}),
      std::move(source),
      bool_field("Automatic updates", original.value("auto_update", false)),
      number_field("Interval (seconds)",
                   original.value("update_interval_seconds", 0LL), 0,
                   std::numeric_limits<long long>::max()),
      select_field("Validator Xray", original.value("validator_core_id", ""),
                   core_choices(cores_))};
  fields[6].hint = "0 disables the schedule. Positive automatic updates "
                   "require a validator core.";
  form(editing ? "Edit Geodata resource" : "Add Geodata resource",
       std::move(fields),
       [this, id, editing](ui::Form &f) -> std::optional<std::string> {
         const bool automatic = f.value(5) == "true";
         const auto interval = std::stoll(f.value(6));
         if (automatic && interval > 0 && f.value(7).empty()) {
           f.set_field_error(7,
                             "Choose a validator core for scheduled updates");
           return "Choose the Xray core used to validate automatic resource "
                  "updates";
         }
         Json params{{"name", f.value(0)},
                     {"kind", f.value(1)},
                     {"file_name", f.value(2)},
                     {"url", f.value(3) == "url" ? f.value(4) : ""},
                     {"local_path", f.value(3) == "file" ? f.value(4) : ""},
                     {"auto_update", automatic},
                     {"update_interval_seconds", interval},
                     {"validator_core_id", f.value(7)}};
         if (editing)
           params["id"] = id;
         auto saved = call("geodata.put", std::move(params));
         if (!saved)
           return saved.error().message;
         refresh();
         geodata_table_->select_id(saved->value("id", id));
         notify("Geodata " + f.value(0) + " saved; runtime unchanged",
                ui::Tone::Success);
         return {};
       });
}

void Workspace::bind_geodata() {
  if (view_.empty())
    return;
  const auto profile = view_;
  const auto bindings = profile_.value("geodata_resource_ids", Json::object());
  std::vector<ui::Choice> choices;
  std::vector<std::string> selected;
  for (const auto &resource : geodata_) {
    const auto id = resource.value("id", "");
    const auto file = resource.value("file_name", "");
    choices.push_back(
        {id, resource.value("name", "") + " · " + file +
                 (resource.value("current", Json(nullptr)).is_null()
                      ? " (not validated)"
                      : "")});
    for (const auto &bound : bindings.items())
      if (bound.value().is_string() && bound.value().get<std::string>() == id)
        selected.push_back(id);
  }
  // Keep stale references visible as unsupported existing values, so opening
  // the form cannot silently remove a binding that requires user attention.
  for (const auto &bound : bindings.items())
    if (bound.value().is_string() &&
        std::ranges::find(selected, bound.value().get<std::string>()) ==
            selected.end())
      selected.push_back(bound.value().get<std::string>());
  ui::Field resources{"Resources", ui::JoinLines(selected),
                      ui::FieldKind::MultiSelect, std::move(choices)};
  resources.hint = "One validated resource per filename. Uncheck a resource to "
                   "remove its binding.";
  form("Geodata for " + name(profile), {std::move(resources)},
       [this, profile](ui::Form &f) -> std::optional<std::string> {
         Json references = Json::object();
         for (const auto &id : ui::SplitLines(f.value(0))) {
           if (id.empty())
             continue;
           auto resource = call("geodata.get", {{"id", id}});
           if (!resource)
             return resource.error().message;
           if (resource->value("current", Json(nullptr)).is_null())
             return "Update and validate " + resource->value("name", id) +
                    " before binding it";
           const auto file = resource->value("file_name", "");
           if (references.contains(file))
             return "Choose only one resource for " + file;
           references[file] = id;
         }
         auto saved = call("profile.geodata.set", {{"profile_id", profile},
                                                   {"resources", references}});
         if (!saved)
           return saved.error().message;
         refresh();
         notify("Geodata bindings saved for " + name(profile) +
                    "; runtime unchanged",
                ui::Tone::Success);
         return {};
       });
}

void Workspace::settings_form() {
  const auto page = section();
  if (page == "Appearance") {
    const auto value = settings_.value("appearance", Json::object());
    form(
        "XRM appearance",
        {select_field("Theme", value.value("theme", "dark"),
                      {{"default", "Terminal palette"},
                       {"dark", "XRM Slate (dark)"},
                       {"light", "XRM Paper (light)"}}),
         select_field("Color mode", value.value("color_mode", "auto"),
                      {{"auto", "Automatic"},
                       {"truecolor", "True color"},
                       {"256", "256 colors"},
                       {"none", "No color"}}),
         select_field("Density", value.value("density", "comfortable"),
                      {{"comfortable", "Comfortable"}, {"compact", "Compact"}}),
         select_field("Icons", value.value("icon_mode", "unicode"),
                      {{"unicode", "Unicode"}, {"ascii", "ASCII"}}),
         bool_field("Reduce motion", value.value("reduce_motion", false)),
         select_field("CLI colors", value.value("cli_color", "auto"),
                      {{"auto", "Automatic"},
                       {"always", "On in terminals"},
                       {"never", "Never"}})},
        [this](ui::Form &f) -> std::optional<std::string> {
          auto saved = call("settings.update",
                            {{"patch",
                              {{"appearance",
                                {{"theme", f.value(0)},
                                 {"color_mode", f.value(1)},
                                 {"density", f.value(2)},
                                 {"icon_mode", f.value(3)},
                                 {"reduce_motion", f.value(4) == "true"},
                                 {"cli_color", f.value(5)}}}}}});
          if (!saved)
            return saved.error().message;
          settings_ = *saved;
          refresh_tables();
          notify("XRM appearance saved", ui::Tone::Success);
          return {};
        });
  } else if (page == "Tests") {
    const auto value = settings_.value("tests", Json::object());
    auto url = text_field("Default test URL", value.value("default_url", ""));
    url.validate = [](const std::string &text) -> std::optional<std::string> {
      if (!text.empty() && !text.starts_with("http://") &&
          !text.starts_with("https://"))
        return "Use HTTP(S), or leave empty to choose per test";
      return {};
    };
    form("Node test defaults",
         {url,
          number_field("Timeout (ms)", value.value("timeout_ms", 5000), 100,
                       60000),
          number_field("Concurrent tests", value.value("concurrency", 3), 1, 3),
          number_field("Download cap (bytes)",
                       value.value("max_body_bytes", 8 * 1024 * 1024), 1,
                       32 * 1024 * 1024)},
         [this](ui::Form &f) -> std::optional<std::string> {
           auto saved =
               call("settings.update",
                    {{"patch",
                      {{"tests",
                        {{"default_url", f.value(0)},
                         {"timeout_ms", std::stoll(f.value(1))},
                         {"concurrency", std::stoll(f.value(2))},
                         {"max_body_bytes", std::stoll(f.value(3))}}}}}});
           if (!saved)
             return saved.error().message;
           settings_ = *saved;
           refresh_tables();
           notify("Node test defaults saved; no test started",
                  ui::Tone::Success);
           return {};
         });
  } else if (page == "Editor") {
    const auto original =
        settings_.value("editor", Json::object()).value("argv", Json::array());
    std::vector<std::string> argv;
    bool embedded_newline = false;
    for (const auto &arg : original) {
      if (!arg.is_string())
        continue;
      argv.push_back(arg.get<std::string>());
      embedded_newline =
          embedded_newline || argv.back().find('\n') != std::string::npos;
    }
    const auto initial = ui::JoinLines(argv);
    ui::Field field{"Editor arguments (one per line)", initial,
                    ui::FieldKind::Multiline};
    field.hint = "First line is the executable. No shell quoting. Empty text "
                 "uses VISUAL / EDITOR. Blank argument lines are preserved.";
    form("External editor", {std::move(field)},
         [this, initial, original,
          embedded_newline](ui::Form &f) -> std::optional<std::string> {
           if (embedded_newline && f.value(0) != initial)
             return "Existing arguments contain embedded newlines; this line "
                    "editor cannot change them safely";
           const Json argv = f.value(0) == initial
                                 ? original
                                 : Json(ui::SplitLines(f.value(0)));
           auto saved = call("settings.update",
                             {{"patch", {{"editor", {{"argv", argv}}}}}});
           if (!saved)
             return saved.error().message;
           settings_ = *saved;
           refresh_tables();
           notify("External editor saved", ui::Tone::Success);
           return {};
         });
  }
}

void Workspace::local_entry() {
  if (view_.empty()) {
    notify("View a Profile before inspecting local inbounds",
           ui::Tone::Warning);
    return;
  }
  leave_draft([this] {
    page_ = 2;
    const auto items = secondary_items();
    auto entry = std::ranges::find_if(
        items, [](const auto &item) { return item.first == "inbounds"; });
    if (entry == items.end())
      return;
    secondary_ = static_cast<int>(entry - items.begin());
    selected_section_ids_[page_] = section();
    layer_ = Layer::Content;
    load_module();
    if (auto component = content())
      component->TakeFocus();
    detail_title_ = "Inbounds for " + name(view_);
    detail_ =
        "Inspect and edit local entry points in the native inbounds module";
  });
}

} // namespace xrm::tui

// XRM: Renders Profile view, Use, Edit, and metadata operations.

#include "tui/components/style.hpp"
#include "tui/shell/workspace.hpp"
namespace xrm::tui {
void Workspace::toggle_edit() {
  auto id = profiles_table_->selected_id();
  if (id.empty())
    return;
  leave_draft([this, id] {
    edit_ = edit_ == id ? "" : id;
    if (!edit_.empty()) {
      view_ = id;
      refresh_profile();
    }
    draft_ = {};
    refresh_tables();
    notify((edit_.empty() ? "Module Edit ended for "
                          : "Module Edit enabled for ") +
           name(id) + "; runtime unchanged");
  });
}
void Workspace::profile_form(std::string mode) {
  auto id = profiles_table_->selected_id();
  Json source = Json::object();
  if (mode != "new") {
    auto r = call("profile.get",
                  {{"profile_id", id}, {"include_source_text", true}});
    if (!r) {
      notify(last_error_, ui::Tone::Error);
      return;
    }
    source = *r;
  }
  std::vector<ui::Choice> choices{{"", "No core yet"}};
  for (auto &c : cores_)
    choices.push_back({c.value("id", ""), c.value("name", "") + " " +
                                              c.value("reported_version", "")});
  auto original = source.value("source_text", "");
  source.erase("source_text");
  std::vector<ui::Field> fields{
      text_field("Name",
                 mode == "copy" ? source.value("name", "") + " copy"
                                : source.value("name", ""),
                 true)};
  if (mode == "new") {
    fields.push_back(select_field("Core", "", choices));
    fields.push_back(select_field(
        "Preset", "local",
        {{"local", "Local SOCKS proxy"},
         {"custom", "Custom (all modules disabled)"},
         {"tun", "TUN draft (system effects on explicit connect)"}}));
  }
  if (mode == "metadata") {
    fields.push_back(
        select_field("Core", source.value("core_id", ""), choices));
    fields.push_back(
        bool_field("Append selected default",
                   source.value("append_selected_default", false)));
  }
  form(mode + " Profile", std::move(fields),
       [this, mode, id, source,
        original](ui::Form &f) mutable -> std::optional<std::string> {
         Result<Json> r;
         if (mode == "new")
           r = call("profile.create", {{"name", f.value(0)},
                                       {"core_id", f.value(1)},
                                       {"preset", f.value(2)}});
         else if (mode == "copy")
           r = call("profile.clone",
                    {{"profile_id", id}, {"name", f.value(0)}});
         else if (mode == "rename")
           r = call("profile.rename",
                    {{"profile_id", id}, {"name", f.value(0)}});
         else {
           source["name"] = f.value(0);
           source["core_id"] = f.value(1);
           source["append_selected_default"] = f.value(2) == "true";
           r = call("profile.save", {{"profile_id", id},
                                     {"settings", source},
                                     {"expected_text", original}});
         }
         if (!r)
           return r.error().message;
         notify("Profile " + f.value(0) + " saved; runtime unchanged",
                ui::Tone::Success);
         refresh();
         if (mode == "new" && view_.empty()) {
           view_ = r->value("id", "");
           refresh_profile();
         }
         return {};
       });
}
void Workspace::subscription_form(bool editing) {
  auto id = subscriptions_table_->selected_id();
  Json source = Json::object();
  if (editing) {
    auto r = call("subscription.get", {{"id", id}});
    if (!r) {
      notify(last_error_, ui::Tone::Error);
      return;
    }
    source = r->value("subscription", Json::object());
  }
  auto headers = source.value("headers", Json::object()).dump(2);
  std::vector<ui::Field> fields{
      text_field("Name", source.value("name", ""), true),
      text_field("URL", source.value("url", ""), true),
      text_field("User-Agent", source.value("user_agent", "")),
      bool_field("Auto refresh", source.value("auto_refresh", true)),
      number_field("Interval seconds", source.value("interval_seconds", 21600),
                   1, 31536000),
      text_field("Proxy URL",
                 source.value("route", Json::object())
                         .value("proxy_url", Json(nullptr))
                         .is_string()
                     ? source["route"]["proxy_url"].get<std::string>()
                     : "")};
  fields[1].kind = ui::FieldKind::Secret;
  fields[5].kind = ui::FieldKind::Secret;
  fields.push_back(
      {"Headers (advanced JSON)", headers, ui::FieldKind::Multiline});
  form(editing ? "Edit subscription" : "Add subscription", std::move(fields),
       [this, id, source, editing](ui::Form &f) -> std::optional<std::string> {
         if (editing) {
           auto current = call("subscription.get", {{"id", id}});
           if (!current)
             return current.error().message;
           if (current->value("subscription", Json::object()) != source)
             return "Subscription changed outside this form; draft retained";
         }
         auto headers = Json::parse(f.value(6), nullptr, false);
         if (!headers.is_object()) {
           f.set_field_error(6,
                             "Enter a JSON object of header names and values");
           return "Headers must be an object";
         }
         Json p{{"name", f.value(0)},
                {"url", f.value(1)},
                {"user_agent", f.value(2)},
                {"auto_refresh", f.value(3) == "true"},
                {"interval_seconds", std::stoll(f.value(4))},
                {"proxy_url",
                 f.value(5).empty() ? Json(nullptr) : Json(f.value(5))},
                {"headers", headers}};
         if (editing)
           p["id"] = id;
         auto r =
             call(editing ? "subscription.replace" : "subscription.add", p);
         if (!r) {
           if (r.error().subject && *r.error().subject == "url")
             f.set_field_error(1, r.error().message);
           return r.error().message;
         }
         notify("Subscription " + f.value(0) + " saved", ui::Tone::Success);
         refresh();
         return {};
       });
}
void Workspace::bind_sources() {
  if (view_.empty())
    return;
  std::vector<ui::Choice> choices;
  std::vector<std::string> selected;
  for (auto &s : subscriptions_) {
    auto id = s.value("id", "");
    choices.push_back({id, s.value("name", "")});
    if (!profile_.contains("enabled_source_ids") ||
        !profile_["enabled_source_ids"].is_array() ||
        std::ranges::find(profile_["enabled_source_ids"], Json(id)) !=
            profile_["enabled_source_ids"].end())
      selected.push_back(id);
  }
  auto profile = view_;
  form("Sources for " + name(profile),
       {{"Enabled sources", ui::JoinLines(selected), ui::FieldKind::MultiSelect,
         choices}},
       [this, profile](ui::Form &f) -> std::optional<std::string> {
         auto r = call("profile.sources.set",
                       {{"profile_id", profile},
                        {"source_ids", ui::SplitLines(f.value(0))}});
         if (!r)
           return r.error().message;
         refresh();
         notify("Source bindings saved", ui::Tone::Success);
         return {};
       });
}
void Workspace::remove_subscription() {
  auto id = subscriptions_table_->selected_id();
  if (id.empty())
    return;
  auto source = find_id(subscriptions_, id);
  std::string refs;
  for (auto &p : profiles_) {
    auto r = call("profile.get", {{"profile_id", p.value("id", "")}});
    if (!r) {
      notify("Cannot inspect references: " + last_error_, ui::Tone::Error);
      return;
    }
    auto ids = r->value("enabled_source_ids", Json(nullptr));
    if (ids.is_null() ||
        (ids.is_array() && std::ranges::find(ids, Json(id)) != ids.end()))
      refs += p.value("name", "") + " ";
  }
  auto info = text_field("References", refs);
  info.readonly = true;
  auto count = text_field("Inventory nodes",
                          scalar(source.value("node_count", Json(0))));
  count.readonly = true;
  form(
      "Remove " + source.value("name", id),
      {info, count, bool_field("Keep nodes as local", true)},
      [this, id](ui::Form &f) -> std::optional<std::string> {
        auto r = call("subscription.remove",
                      {{"id", id}, {"keep_as_manual", f.value(2) == "true"}});
        if (!r)
          return r.error().message;
        refresh();
        notify("Subscription removed", ui::Tone::Success);
        return {};
      },
      "Remove");
}
void Workspace::history() {
  auto id = subscriptions_table_->selected_id();
  auto r = call("node.history_preview", {{"source_id", id}});
  if (!r) {
    notify(last_error_, ui::Tone::Error);
    return;
  }
  auto preview = *r;
  std::vector<ui::Choice> choices;
  for (auto &n : preview.value("candidates", Json::array()))
    if (n.value("eligible", false))
      choices.push_back({n.value("id", n.value("node_id", "")),
                         n.value("name", "") + " — " + n.value("reason", "")});
  form(
      "History cleanup",
      {{"Safe candidates", "", ui::FieldKind::MultiSelect, choices}},
      [this, id, preview](ui::Form &f) -> std::optional<std::string> {
        auto ids = ui::SplitLines(f.value(0));
        if (ids.empty())
          return "Choose candidates from this preview";
        auto r =
            call("node.history_confirm", {{"source_id", id},
                                          {"token", preview.value("token", "")},
                                          {"ids", ids}});
        if (!r)
          return r.error().message;
        refresh();
        notify("History cleaned", ui::Tone::Success);
        return {};
      },
      "Remove selected");
}
} // namespace xrm::tui

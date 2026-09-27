// XRM: Coordinates TUI navigation, cached page data, drafts, and overlays.
// View/Use/Edit and the Modules draft live only in this session. Server RPCs
// own saved state; runtime.status reports actual running state separately.

#include "tui/shell/workspace.hpp"
#include "client/human_message.hpp"
#include "contract/wire.hpp"
#include "model/module_catalog.hpp"
#include "tui/components/log_viewer.hpp"
#include "tui/components/style.hpp"
#include "tui/forms/native_form.hpp"
#include <algorithm>
#include <ctime>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <sstream>
namespace xrm::tui {
using namespace ftxui;
namespace {
std::string subscription_result(const Json &source) {
  const auto result = source.value("last_result", "");
  if (result.empty())
    return "Not refreshed";
  if (result == "settings_changed")
    return "Settings changed";
  if (result == "not_modified")
    return "Unchanged";
  if (result == "success")
    return "Updated";
  if (result == "partial")
    return "Partial: inspect";
  if (result == "failed")
    return "Failed: inspect";
  return result;
}
} // namespace
ui::Field text_field(std::string label, std::string value, bool required) {
  ui::Field f{std::move(label), std::move(value)};
  if (required)
    f.validate = [](const std::string &s) -> std::optional<std::string> {
      if (s.empty())
        return "Required";
      return {};
    };
  return f;
}
ui::Field select_field(std::string l, std::string v,
                       std::vector<ui::Choice> c) {
  return {std::move(l), std::move(v), ui::FieldKind::Select, std::move(c)};
}
ui::Field bool_field(std::string l, bool v) {
  return {std::move(l), v ? "true" : "false", ui::FieldKind::Checkbox};
}
ui::Field number_field(std::string l, long long v, long long min,
                       long long max) {
  ui::Field f{std::move(l), std::to_string(v), ui::FieldKind::Number};
  f.validate = ui::Integer(min, max, false);
  return f;
}
Json find_id(const Json &rows, const std::string &id) {
  for (const auto &r : rows)
    if (r.value("id", "") == id)
      return r;
  return Json::object();
}
std::string scalar(const Json &v) {
  return v.is_string() ? v.get<std::string>() : v.is_null() ? "—" : v.dump();
}
Workspace::Workspace(std::string socket, std::string initial,
                     ScreenInteractive &screen)
    : socket_(std::move(socket)), view_(std::move(initial)), screen_(screen) {
  original_color_ = Terminal::ColorSupport();
  profiles_table_ = std::make_shared<ui::Table>(std::vector<ui::Column>{
      {"Profile", 22}, {"Core", 20}, {"Use", 6}, {"Edit", 6}, {"Run", 9}});
  subscriptions_table_ = std::make_shared<ui::Table>(std::vector<ui::Column>{
      {"Source", 24}, {"Nodes", 7}, {"Result", 15}, {"Auto", 7}, {"Bound", 9}});
  nodes_table_ =
      std::make_shared<ui::Table>(std::vector<ui::Column>{{"Node", 22},
                                                          {"Protocol", 10},
                                                          {"Enabled", 8},
                                                          {"Support", 10},
                                                          {"Saved", 6},
                                                          {"Run", 7}});
  groups_table_ = std::make_shared<ui::Table>(std::vector<ui::Column>{
      {"Group", 26}, {"Mode", 14}, {"Strategy", 15}, {"Members", 12}});
  cores_table_ = std::make_shared<ui::Table>(std::vector<ui::Column>{
      {"Core / release", 30}, {"Version", 20}, {"State", 20}});
  geodata_table_ = std::make_shared<ui::Table>(std::vector<ui::Column>{
      {"Resource", 26}, {"Kind", 12}, {"Result", 18}, {"Auto", 8}});
  settings_table_ = std::make_shared<ui::Table>(
      std::vector<ui::Column>{{"Setting", 24}, {"Value", 46}});
  logs_ = std::make_shared<ui::LogViewer>();
  for (auto c : std::vector<Component>{
           profiles_table_, subscriptions_table_, nodes_table_, groups_table_,
           cores_table_, geodata_table_, settings_table_, logs_})
    Add(c);
  refresh();
  if (view_.empty() && profiles_.size() == 1)
    view_ = profiles_[0].value("id", "");
  else if (!view_.empty() && find_id(profiles_, view_).empty()) {
    std::vector<std::string> matches;
    for (auto &p : profiles_)
      if (p.value("name", "") == view_)
        matches.push_back(p.value("id", ""));
    if (matches.size() == 1)
      view_ = matches[0];
    else
      view_.clear();
  }
  use_ = view_;
  refresh_profile();
  refresh_tables();
  if (view_.empty() && profiles_.size() > 1)
    page_ = 1;
  ticker_ = std::jthread([this](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!stop.stop_requested())
        // PostEvent writes FTXUI's unsynchronized input buffer. The ticker is
        // a different thread from terminal input, so use its locked task queue.
        screen_.Post(Event::Custom);
    }
  });
}
Workspace::~Workspace() {
  Terminal::SetColorSupport(original_color_);
  ticker_.request_stop();
  if (ticker_.joinable())
    ticker_.join();
  if (background_.valid())
    background_.wait();
}
Result<Json> Workspace::call(std::string_view method, Json p) {
  auto r = request(socket_, std::string(method), std::move(p));
  if (r)
    endpoint_connected_ = true;
  if (!r) {
    if (r.error().code == ErrorCode::io_error ||
        r.error().code == ErrorCode::result_unknown) {
      fresh_ = false;
      endpoint_connected_ = false;
    }
    const auto message = human::error(r.error(), pages_[page_], name(view_));
    last_diagnostic_ = message.diagnostic;
    last_error_ =
        message.title + ": " + message.reason + " " + message.recovery;
    r.error().message = message.reason + " " + message.recovery;
  }
  return r;
}
void Workspace::notify(std::string message, ui::Tone tone) {
  if (tone == ui::Tone::Error)
    last_error_ = message;
  toasts_.push(message, message, tone);
}
void Workspace::apply_colors() {
  auto colors =
      settings_.value("appearance", Json::object()).value("color_mode", "auto");
  Terminal::SetColorSupport(std::getenv("NO_COLOR") || colors == "none"
                                ? Terminal::Palette1
                            : colors == "256"       ? Terminal::Palette256
                            : colors == "truecolor" ? Terminal::TrueColor
                                                    : original_color_);
}
void Workspace::refresh() {
  // Keep the last successful page snapshot on partial RPC failure. fresh_
  // marks whether displayed values are current; it is not a zero value.
  bool good = true;
  auto get = [&](std::string_view method, Json &target) {
    auto r = call(method);
    if (r)
      target = *r;
    else
      good = false;
  };
  get("profile.list", profiles_);
  get("subscription.list", subscriptions_);
  for (auto &source : subscriptions_) {
    auto full = call("subscription.get", {{"id", source.value("id", "")}});
    if (full) {
      auto sub = full->value("subscription", Json::object());
      source["auto_refresh"] = sub.value("auto_refresh", true);
    } else
      good = false;
  }
  get("core.list", cores_);
  get("geodata.list", geodata_);
  get("settings.get", settings_);
  get("runtime.status", status_);
  auto n = inventory(socket_);
  if (n)
    nodes_ = *n;
  else {
    good = false;
    last_error_ = n.error().message;
  }
  fresh_ = good;
  loaded_ = loaded_ || good;
  refresh_profile();
  refresh_tables();
}
void Workspace::refresh_profile() {
  if (view_.empty()) {
    profile_ = Json::object();
    groups_ = Json::array();
    modules_ = Json::array();
    return;
  }
  auto p = call("profile.get", {{"profile_id", view_}});
  if (p)
    profile_ = *p;
  auto g = call("group.list", {{"profile_id", view_}});
  if (g)
    groups_ = *g;
  auto m = call("module.list", {{"profile_id", view_}});
  if (m)
    modules_ = *m;
}
std::string Workspace::name(const std::string &id) const {
  if (id.empty())
    return "none";
  auto p = find_id(profiles_, id);
  return p.value("name", id);
}
std::string Workspace::active_name() const {
  return status_.contains("active") && status_["active"].is_object()
             ? name(status_["active"].value("profile_id", ""))
             : "none";
}
std::vector<std::pair<std::string, std::string>>
Workspace::secondary_items() const {
  std::vector<std::pair<std::string, std::string>> items;
  if (page_ == 2)
    for (auto &m : module_catalog())
      items.emplace_back(m.key, m.key);
  if (page_ == 4) {
    items = {{"all", "Scope: All"},
             {"local", "Scope: Local"},
             {"groups", "Scope: Groups"}};
    for (auto &s : subscriptions_)
      items.emplace_back(s.value("id", ""), "Source: " + s.value("name", ""));
  }
  if (page_ == 6)
    for (const auto *s :
         {"Core", "Geodata", "Tests", "Appearance", "Editor", "General"})
      items.emplace_back(s, s);
  return items;
}
std::string Workspace::section() const {
  auto items = secondary_items();
  return items.empty()
             ? ""
             : items[std::clamp(secondary_, 0, int(items.size()) - 1)].first;
}
std::shared_ptr<ui::Table> Workspace::table() const {
  if (page_ == 1)
    return profiles_table_;
  if (page_ == 3)
    return subscriptions_table_;
  if (page_ == 4)
    return section() == "groups" ? groups_table_ : nodes_table_;
  if (page_ == 6)
    return section() == "Core"      ? cores_table_
           : section() == "Geodata" ? geodata_table_
                                    : settings_table_;
  return {};
}
Component Workspace::content() const {
  if (page_ == 2)
    return draft_.visual ? visual_ : code_;
  if (page_ == 5)
    return logs_;
  return table();
}
void Workspace::refresh_tables() {
  // Rebuild presentation from cached RPC snapshots, preserving stable row IDs
  // and the current module draft. This function does not issue write RPCs.
  // Secondary sections are identified by key: source list refreshes may
  // remove a source or shift its index while this page is open.
  if (!selected_section_ids_[page_].empty()) {
    const auto items = secondary_items();
    const auto found = std::ranges::find_if(items, [&](const auto &item) {
      return item.first == selected_section_ids_[page_];
    });
    secondary_ =
        found == items.end() ? 0 : static_cast<int>(found - items.begin());
    if (found == items.end() && !items.empty())
      selected_section_ids_[page_] = items.front().first;
  }
  auto appearance = settings_.value("appearance", Json::object());
  const auto colors = appearance.value("color_mode", "auto");
  Terminal::SetColorSupport(std::getenv("NO_COLOR") || colors == "none"
                                ? Terminal::Palette1
                            : colors == "256"       ? Terminal::Palette256
                            : colors == "truecolor" ? Terminal::TrueColor
                                                    : original_color_);
  for (auto t : {profiles_table_, subscriptions_table_, nodes_table_,
                 groups_table_, cores_table_, geodata_table_, settings_table_})
    t->presentation(appearance.value("density", "comfortable") == "compact",
                    appearance.value("icon_mode", "unicode") == "ascii");
  std::vector<ui::Row> rows;
  for (auto &p : profiles_) {
    auto id = p.value("id", "");
    auto core = find_id(cores_, p.value("core_id", ""));
    const bool running = status_.contains("active") &&
                         status_["active"].is_object() &&
                         id == status_["active"].value("profile_id", "");
    rows.push_back({id,
                    {p.value("name", ""), core.value("name", "Not set"),
                     id == use_ ? "yes" : "—", id == edit_ ? "yes" : "—",
                     fresh_ ? (running ? "running" : "—") : "unknown"}});
  }
  profiles_table_->set_rows(std::move(rows),
                            loaded_ ? "No Profiles" : "Profiles unavailable");
  rows.clear();
  for (auto &s : subscriptions_) {
    auto id = s.value("id", "");
    bool bound = !profile_.contains("enabled_source_ids") ||
                 !profile_["enabled_source_ids"].is_array() ||
                 std::ranges::find(profile_["enabled_source_ids"], Json(id)) !=
                     profile_["enabled_source_ids"].end();
    rows.push_back(
        {id,
         {s.value("name", ""), scalar(s.value("node_count", Json(nullptr))),
          subscription_result(s), s.value("auto_refresh", false) ? "on" : "off",
          view_.empty() ? "—"
          : bound       ? "yes"
                        : "no"}});
  }
  subscriptions_table_->set_rows(std::move(rows),
                                 loaded_ ? "No subscriptions"
                                         : "Subscriptions unavailable");
  rows.clear();
  std::map<std::string, int> node_name_counts;
  for (const auto &node : nodes_)
    ++node_name_counts[node.value("name", "")];
  for (auto &n : nodes_) {
    std::string source = n.contains("source_id") && n["source_id"].is_string()
                             ? n["source_id"].get<std::string>()
                             : "";
    auto sec = section();
    if (page_ == 4 &&
        ((sec == "local" && !source.empty()) ||
         (sec != "all" && sec != "local" && sec != "groups" && sec != source)))
      continue;
    if (!source.empty() && profile_.contains("enabled_source_ids") &&
        profile_["enabled_source_ids"].is_array() &&
        std::ranges::find(profile_["enabled_source_ids"], Json(source)) ==
            profile_["enabled_source_ids"].end())
      continue;
    auto label = n.value("name", "");
    if (!search_.empty() && label.find(search_) == std::string::npos)
      continue;
    if (!show_missing_ && n.value("source_missing", false))
      continue;
    auto id = n.value("id", "");
    if (node_name_counts[label] > 1) {
      const auto source_label =
          source.empty()
              ? std::string("local")
              : find_id(subscriptions_, source).value("name", source);
      label += " [" + source_label + ":" +
               id.substr(0, std::min<std::size_t>(6, id.size())) + "]";
    }
    const bool saved = profile_.contains("selected") &&
                       profile_["selected"].is_object() &&
                       profile_["selected"].value("id_or_tag", "") == id;
    const bool running = status_.contains("actual_target") &&
                         status_["actual_target"].is_object() &&
                         status_["actual_target"].value("id_or_tag", "") == id;
    rows.push_back({id,
                    {label, n.value("protocol", "native"),
                     n.value("enabled", true) ? "yes" : "no",
                     n.value("support", "unknown"), saved ? "yes" : "—",
                     fresh_ ? (running ? "yes" : "—") : "unknown"}});
  }
  nodes_table_->set_rows(std::move(rows), loaded_ ? "No nodes in this view"
                                                  : "Nodes unavailable");
  rows.clear();
  for (auto &g : groups_) {
    auto m = g.value("members", Json::object());
    auto b = g.value("balancer", Json::object());
    rows.push_back(
        {g.value("id", ""),
         {g.value("name", ""), g.value("selection_mode", ""),
          b.value("strategy", Json::object()).value("type", "random"),
          m.value("kind", "")}});
  }
  groups_table_->set_rows(std::move(rows),
                          view_.empty() ? "View a Profile to manage Groups"
                                        : "No Groups");
  rows.clear();
  if (releases_view_)
    for (auto &r : releases_)
      rows.push_back(
          {r.value("tag", ""),
           {r.value("tag", ""), r.value("name", ""),
            r.value("prerelease", false) ? "prerelease" : "release"}});
  else
    for (auto &c : cores_)
      rows.push_back({c.value("id", ""),
                      {c.value("name", ""),
                       [&] {
                         auto version = c.value("reported_version", "");
                         if (version.starts_with("Xray "))
                           version.erase(0, 5);
                         if (auto end = version.find_first_of(" \n");
                             end != std::string::npos)
                           version.resize(end);
                         return version;
                       }(),
                       c.value("managed", false) ? "managed" : "registered"}});
  cores_table_->set_rows(std::move(rows), loaded_
                                              ? "No registered cores"
                                              : "Core inventory unavailable");
  rows.clear();
  for (auto &g : geodata_)
    rows.push_back({g.value("id", ""),
                    {g.value("name", ""), g.value("kind", ""),
                     g.value("last_result", "never"),
                     g.value("auto_update", false) ? "on" : "off"}});
  geodata_table_->set_rows(std::move(rows),
                           loaded_ ? "No Geodata resources"
                                   : "Geodata inventory unavailable");
  rows.clear();
  auto sec = section();
  auto object = sec == "Appearance"
                    ? settings_.value("appearance", Json::object())
                : sec == "Editor" ? settings_.value("editor", Json::object())
                : sec == "Tests"
                    ? settings_.value("tests", Json::object())
                    : Json{{"Socket", socket_},
                           {"Configuration", "Native modules are in Modules"},
                           {"Version", "XRM 0.1.0"}};
  if (object.is_object())
    for (auto it = object.begin(); it != object.end(); ++it) {
      static const std::map<std::string, std::string> labels{
          {"theme", "Theme"},
          {"color_mode", "Color capability"},
          {"density", "Content density"},
          {"icon_mode", "Symbols"},
          {"reduce_motion", "Reduce motion"},
          {"cli_color", "CLI color"},
          {"argv", "External editor"},
          {"default_url", "Test URL"},
          {"timeout_ms", "Timeout (ms)"},
          {"concurrency", "Parallel tests"},
          {"max_body_bytes", "Response limit (bytes)"}};
      auto label = labels.contains(it.key()) ? labels.at(it.key()) : it.key();
      auto value = scalar(it.value());
      if (it.key() == "theme")
        value = value == "dark"    ? "XRM Slate"
                : value == "light" ? "XRM Paper"
                                   : "Terminal palette";
      if (it.value().is_boolean())
        value = it.value().get<bool>() ? "On" : "Off";
      if (it.key() == "argv" && it.value().is_array()) {
        value.clear();
        for (auto &arg : it.value()) {
          if (!value.empty())
            value += " ";
          value += scalar(arg);
        }
        if (value.empty())
          value = "Not configured";
      }
      if (value.empty())
        value = "Not configured";
      rows.push_back({it.key(), {label, value}});
    }
  settings_table_->set_rows(std::move(rows),
                            loaded_ ? "No settings" : "Settings unavailable");
}
void Workspace::background(std::function<Result<Json>()> job,
                           std::function<void(Result<Json>)> done) {
  // One outstanding request keeps its result paired with its completion
  // callback. poll() consumes it on the UI thread.
  if (background_.valid()) {
    notify("Another request is still pending", ui::Tone::Warning);
    return;
  }
  background_done_ = std::move(done);
  background_ = std::async(std::launch::async, std::move(job));
}
void Workspace::operation(std::string method, Json params, std::string label) {
  // A returned operation ID means accepted, not completed. poll() observes
  // the final result before reporting success or refreshing saved state.
  auto socket = socket_;
  background(
      [socket, method, params] { return request(socket, method, params); },
      [this, label](Result<Json> r) {
        if (!r) {
          const auto message = human::error(r.error(), label, name(view_));
          last_diagnostic_ = message.diagnostic;
          if (r.error().code == ErrorCode::io_error ||
              r.error().code == ErrorCode::result_unknown)
            fresh_ = false;
          notify(message.title + ": " + message.reason + " " + message.recovery,
                 ui::Tone::Error);
          return;
        }
        auto id = r->value("operation_id", "");
        if (id.empty()) {
          notify(label + " completed", ui::Tone::Success);
          refresh();
        } else {
          operations_[id] = label;
          notify(label + " started");
        }
      });
}
bool Workspace::mutate(std::string_view method, Json params,
                       std::string label) {
  auto r = call(method, std::move(params));
  if (!r) {
    notify(label + ": " + r.error().message, ui::Tone::Error);
    return false;
  }
  notify(label + " saved; runtime unchanged", ui::Tone::Success);
  refresh();
  return true;
}
void Workspace::poll() {
  toasts_.suspended(
      !overlays_.empty() ||
      (page_ == 2 && !draft_.visual && writable() && layer_ == Layer::Content));
  toasts_.expire();
  if (background_.valid() && background_.wait_for(std::chrono::seconds(0)) ==
                                 std::future_status::ready) {
    auto result = background_.get();
    auto done = std::move(background_done_);
    done(std::move(result));
  }
  if (std::chrono::steady_clock::now() < next_poll_)
    return;
  next_poll_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  for (auto it = operations_.begin(); it != operations_.end();) {
    const bool was_connected = fresh_;
    auto r = call("operation.get", {{"operation_id", it->first}});
    if (!r) {
      if (was_connected || r.error().code == ErrorCode::not_found)
        notify(it->second + ": result unconfirmed — " + last_error_,
               ui::Tone::Error);
      // Re-query an existing operation after reconnect; never replay its write.
      if (r.error().code == ErrorCode::not_found)
        it = operations_.erase(it);
      else
        ++it;
      continue;
    }
    auto state = r->value("status", "");
    if (state == "running" || state == "pending") {
      ++it;
      continue;
    }
    if (state == "failed") {
      const auto parsed = error_from_json(r->value("error", Json::object()));
      const auto message = human::error(parsed ? *parsed : parsed.error(),
                                        it->second, name(view_));
      last_diagnostic_ = message.diagnostic;
      notify(message.title + ": " + message.reason + " " + message.recovery,
             ui::Tone::Error);
    } else if (state == "cancelled")
      notify(it->second + ": cancelled", ui::Tone::Warning);
    else {
      auto result = r->value("result", Json::object());
      bool partial = result.is_object() && result.contains("failed") &&
                     !result["failed"].empty();
      std::string first_problem;
      bool any_completed = false;
      if (result.is_object() && result.contains("failed") &&
          result["failed"].is_array()) {
        for (const auto &failed : result["failed"]) {
          if (!failed.is_object())
            continue;
          auto id = failed.value("source_id", "");
          const auto source = find_id(subscriptions_, id);
          auto parsed = error_from_json(failed.value("error", Json::object()));
          const auto message =
              human::error(parsed ? *parsed : parsed.error(), "Refresh source",
                           source.value("name", id), "subscription.refresh");
          if (first_problem.empty()) {
            first_problem =
                message.title + ": " + message.reason + " " + message.recovery;
            last_diagnostic_ = message.diagnostic;
          }
        }
      }
      if (result.is_object() && result.contains("completed"))
        for (auto &c : result["completed"]) {
          any_completed = true;
          partial = partial || c.value("partial", false);
          if (first_problem.empty() && c.value("partial", false)) {
            const auto id = c.value("source_id", "");
            const auto source = find_id(subscriptions_, id);
            first_problem = source.value("name", id) + ": partial parse";
            if (c.contains("first_issue") && c["first_issue"].is_string())
              first_problem += " — " + c["first_issue"].get<std::string>();
            first_problem +=
                "; inspect the source result and retained inventory";
          }
        }
      if (result.is_object() && result.contains("node_id"))
        notify(it->second + ": " + result.value("status", "unknown") + " · " +
                   result.value("message", ""),
               result.value("status", "") == "success" ? ui::Tone::Success
                                                       : ui::Tone::Warning);
      else if (partial)
        notify(it->second + ": " + first_problem,
               any_completed ? ui::Tone::Warning : ui::Tone::Error);
      else
        notify(it->second + ": completed", ui::Tone::Success);
      if (it->second == "Core releases" && result.is_object()) {
        releases_ = result.value("items", Json::array());
        releases_view_ = true;
      }
    }
    it = operations_.erase(it);
    refresh();
  }
  if (page_ == 5)
    read_logs();
  auto runtime = call("runtime.status");
  if (runtime) {
    status_ = *runtime;
    if (!fresh_) {
      refresh();
      if (fresh_)
        notify("Connection restored; cached lists refreshed");
    }
  }
  if (page_ == 0) {
    if (!stats_reader_)
      stats_reader_ = std::make_unique<RpcSession>(socket_);
    auto s = stats_reader_->call("runtime.stats", {{"watch", true}});
    if (s)
      stats_ = *s;
    else {
      stats_reader_.reset();
      stats_["availability"] = "stale";
    }
  } else
    stats_reader_.reset();
}
void Workspace::read_logs() {
  // The cursor belongs to IPC chunks. ParseLogChunks derives viewer records;
  // neither cursor nor retained-chunk count is a log sequence number.
  auto r = call("logs.read", {{"cursor", log_cursor_}, {"limit", 65536}});
  if (!r)
    return;
  log_cursor_ = r->value("next_cursor", log_cursor_);
  log_truncated_ = log_truncated_ || r->value("truncated", false);
  for (auto &e : r->value("entries", Json::array()))
    raw_logs_.push_back(e);
  std::size_t bytes_retained = 0;
  for (auto &e : raw_logs_)
    bytes_retained += e.value("text", "").size();
  while (raw_logs_.size() > 512 || bytes_retained > 256 * 1024) {
    bytes_retained -= raw_logs_.front().value("text", "").size();
    raw_logs_.erase(raw_logs_.begin());
    log_dropped_ = true;
  }
  std::vector<ui::LogChunk> chunks;
  for (auto &e : raw_logs_)
    chunks.push_back({e.value("cursor", std::uint64_t{}),
                      e.value("stream", "core"), e.value("text", "")});
  auto records = ui::ParseLogChunks(chunks);
  log_lines_.clear();
  for (const auto &record : records)
    log_lines_.push_back(record.raw);
  logs_->set_records(std::move(records));
}
void Workspace::push(Component c) {
  toasts_.suspended(true);
  overlays_.push_back(c);
  Add(c);
  c->TakeFocus();
}
void Workspace::pop() {
  if (overlays_.empty())
    return;
  overlays_.back()->Detach();
  overlays_.pop_back();
  if (overlays_.empty())
    toasts_.suspended(false);
  if (!overlays_.empty())
    overlays_.back()->TakeFocus();
  else if (auto c = content())
    c->TakeFocus();
}
void Workspace::confirm(std::string title, std::string message,
                        std::function<void()> accept) {
  push(ui::Confirm(
      std::move(title), std::move(message),
      [this, accept] {
        pop();
        accept();
      },
      [this] { pop(); }));
}
void Workspace::form(std::string title, std::vector<ui::Field> fields,
                     ui::Form::Submit submit, std::string save) {
  auto weak = std::make_shared<std::weak_ptr<ui::Form>>();
  auto f = std::make_shared<ui::Form>(
      std::move(title), std::move(fields),
      [this, submit](ui::Form &f) -> std::optional<std::string> {
        try {
          auto error = submit(f);
          if (!error)
            pop();
          return error;
        } catch (const std::exception &e) {
          return e.what();
        }
      },
      [this, weak] { cancel_form(*weak); }, false, std::move(save));
  f->set_context(
      "Save this object to its saved settings; runtime remains unchanged.");
  *weak = f;
  push(f);
}
void Workspace::cancel_form(std::weak_ptr<ui::Form> weak) {
  auto f = weak.lock();
  if (!f)
    return;
  if (!f->dirty()) {
    pop();
    return;
  }
  auto save = [this, weak] {
    pop();
    if (auto form = weak.lock())
      form->submit();
  };
  auto discard = [this] {
    pop();
    pop();
  };
  auto resume = [this] { pop(); };
  auto buttons = Container::Horizontal(
      {ui::style::button("Save", save), ui::style::button("Discard", discard),
       ui::style::button("Continue editing", resume)});
  const auto message =
      "Unsaved changes to " + f->title() + ". " + f->context() +
      " Discard closes this form and loses these local changes.";
  auto body = Renderer(buttons, [buttons, message] {
    return vbox({paragraph(message), text(""), buttons->Render()});
  });
  push(
      ui::Dialog("Unsaved form", body, resume,
                 {{"Enter on Save", "Save", f->title(), "", save, f->context()},
                  {"Enter on Discard", "Discard", f->title(), "", discard,
                   "Lose this form's local changes."},
                  {"Esc", "Continue editing", f->title(), "", resume,
                   "Keep all local values."}}));
}
void Workspace::leave_draft(std::function<void()> next) {
  // Navigation waits for Save/Discard/Continue. A failed Save leaves the
  // dialog and draft in place, so the pending transition cannot consume it.
  if (!draft_.dirty()) {
    next();
    return;
  }
  auto save = [this, next] {
    if (save_module()) {
      pop();
      next();
    }
  };
  auto discard = [this, next] {
    draft_.text = draft_.baseline;
    pop();
    next();
  };
  auto resume = [this] { pop(); };
  const auto target = draft_.module + " — " + name(draft_.profile);
  const auto message = "Unsaved changes to " + target +
                       ". Save writes this module's Source; the running core "
                       "stays unchanged. Discard loses this module draft.";
  auto buttons = Container::Horizontal(
      {ui::style::button("Save", save), ui::style::button("Discard", discard),
       ui::style::button("Continue editing", resume)});
  auto body = Renderer(buttons, [buttons, message] {
    return vbox({paragraph(message), text(""), buttons->Render()});
  });
  push(ui::Dialog("Unsaved module", body, resume,
                  {{"Enter on Save", "Save", target, "", save,
                    "Write Source; runtime unchanged."},
                   {"Enter on Discard", "Discard", target, "", discard,
                    "Lose this module draft."},
                   {"Esc", "Continue editing", target, "", resume,
                    "Keep all local changes."}}));
}
void Workspace::help() {
  // Combine shell navigation and the active component's available actions.
  // Help executes the same callbacks as keyboard actions in this context.
  std::vector<ui::HelpEntry> entries;
  std::string context = std::string(pages_[page_]) + " · View " + name(view_);
  std::string prerequisites;
  const auto guide = [this] {
    switch (page_) {
    case 0:
      return std::string("Add nodes in Nodes; choose the next Profile in "
                         "Profiles. Connect publishes saved settings only.");
    case 1:
      return std::string(
          "Use chooses the next connection; module Edit grants writes in "
          "Modules. Connect and Stop are in Overview.");
    case 2:
      return std::string(
          "Read only? In Profiles, View this Profile and allow its module "
          "Edit; return here. This does not Use or run it.");
    case 3:
      return std::string(
          "Subscription updates change inventory only. Bind sources to a "
          "Profile here; select Nodes and Connect separately.");
    case 4:
      return std::string("Groups are under Nodes / Scope: Groups. Choose "
                         "Profile Use in Profiles; Connect in Overview.");
    case 5:
      return std::string(
          "Logs cannot stop the core. Go to Overview, then choose Stop core.");
    default:
      return std::string("XRM settings and core registration are here. Xray "
                         "native configuration is in Modules.");
    }
  };
  if (!overlays_.empty()) {
    const auto owner = overlays_.back();
    entries = ui::ContextHelp(owner);
    if (auto form = std::dynamic_pointer_cast<ui::Form>(owner)) {
      context = form->title();
      prerequisites = form->context();
      entries.push_back({"", "Editing scope and advanced fields", form->title(),
                         "", [this, form] {
                           detail_title_ = form->title();
                           detail_ = form->context();
                           full_detail();
                         }});
    }
    if (page_ == 2 && !writable())
      prerequisites += " Read only for " + name(view_) + ". " + guide();
  } else if (layer_ != Layer::Content) {
    context += layer_ == Layer::Top
                   ? " · Page navigation"
                   : " · " + (page_ == 2 ? std::string("Module selection")
                                         : std::string("Section selection"));
    entries = {
        {"Enter",
         "Open " + std::string(pages_[page_]) +
             (section().empty() ? "" : " / " + section()),
         "", "", [this] { OnEvent(Event::Return); }},
        {"Tab / Shift-Tab",
         layer_ == Layer::Top ? "Choose a page" : "Choose a module or section",
         "", ""},
        {"Esc", "Return one level", "", ""}};
    int shown = 0;
    for (const auto &task : actions()) {
      if (task.key == "N" || task.key == "r" || task.key == "F")
        continue;
      entries.push_back({"After Enter", task.label,
                         task.target == pages_[page_] ? "" : task.target,
                         task.disabled});
      if (++shown == 4)
        break;
    }
    if (layer_ == Layer::Top)
      entries.push_back({"q", "Exit TUI", "", "",
                         [this] { OnEvent(Event::Character('q')); },
                         "The running core keeps running."});
    prerequisites = guide();
  } else {
    for (auto &a : actions()) {
      auto duplicate =
          std::find_if(entries.begin(), entries.end(), [&](const auto &e) {
            return e.action == a.label &&
                   e.target == (a.target == pages_[page_] ? "" : a.target);
          });
      if (duplicate != entries.end()) {
        duplicate->key += " / " + a.key;
        continue;
      }
      entries.push_back({a.key, a.label,
                         a.target == pages_[page_] ? "" : a.target, a.disabled,
                         a.run});
    }
    for (auto entry : ui::ContextHelp(content())) {
      if (std::none_of(entries.begin(), entries.end(),
                       [&](const auto &e) { return e.key == entry.key; }))
        entries.push_back(std::move(entry));
    }
    if (page_ != 2 && page_ != 5)
      entries.push_back({"Enter", "View highlighted object", "", "",
                         [this] { view_detail(); }});
    entries.push_back({"Esc", "Return one level", "", ""});
    if (page_ == 0) {
      context += " · Connect uses " + name(use_);
      prerequisites = use_.empty()
                          ? "Choose Use in Profiles before connecting. "
                          : "Connect publishes saved settings only. ";
      if (stats_.value("availability", "unavailable") != "available")
        prerequisites +=
            "Statistics need saved stats, policy and metrics settings plus a "
            "running core. Configure in Modules, then explicitly Connect.";
    }
    if (page_ == 1)
      prerequisites =
          "Use selects the next connection Profile; Cancel Use and module Edit "
          "do not change the running core. Metadata is independent of Edit.";
    if (page_ == 2) {
      context = name(view_) + " / " + draft_.module +
                (draft_.runtime_view ? " · Runtime" : " · Source");
      prerequisites =
          writable() ? "Child forms change this draft; Save writes Source; "
                       "Connect publishes saved settings. "
          : draft_.runtime_view
              ? "Runtime is always read only. Switch to Source to edit. "
              : "Read only: in Profiles allow module Edit for " + name(view_) +
                    ". ";
      prerequisites += "Visual edits listed native fields; other fields/types "
                       "remain in JSON (F2).";
    }
    if (page_ == 5)
      prerequisites = "Pause keeps the current buffer in place; new text is "
                      "buffered. Enter shows original text and transport "
                      "details. No core lifecycle actions here.";
    prerequisites += " " + guide();
  }
  if (!endpoint_connected_)
    prerequisites += " xrmd is unavailable at " + socket_ +
                     ". Check this endpoint and start the intended test daemon "
                     "if needed; Refresh after it returns. Cached views and "
                     "local drafts remain available.";
  if (page_ == 2 && !draft_.error.empty())
    entries.push_back(
        {"", "Module validation detail", draft_.module, "", [this] {
           detail_title_ = "Module validation";
           detail_ = draft_.error;
           full_detail();
         }});
  if (!last_diagnostic_.empty())
    entries.push_back({"", "Last error diagnostic", "", "", [this] {
                         detail_title_ = "Diagnostic (original)";
                         detail_ = last_diagnostic_;
                         full_detail();
                       }});
  for (auto &entry : entries)
    if (entry.run) {
      auto run = std::move(entry.run);
      entry.run = [this, run = std::move(run)] {
        pop();
        run();
      };
    }
  push(ui::HelpOverlay(
      std::move(entries), [this] { pop(); }, context, prerequisites));
}
std::string Workspace::target_label(const Json &target) const {
  if (!target.is_object())
    return "none";
  auto id = target.value("id_or_tag", "");
  auto kind = target.value("kind", "");
  if (kind == "node") {
    auto n = find_id(nodes_, id);
    if (!n.empty())
      return "Node · " + n.value("name", id) +
             (n.value("source_missing", false) ? " (source missing)" : "");
    if (status_.contains("active") && status_["active"].is_object() &&
        status_["active"].value("selection", Json(nullptr)) == target)
      return "Node · " + status_["active"].value("selection_display_name", id) +
             " (running snapshot; removed from source)";
    return "Node · " + id + " (missing)";
  }
  if (kind == "group")
    return "Group · " + find_id(groups_, id).value("name", id);
  return kind + " · " + id;
}
Element Workspace::overview() const {
  using namespace ui::style;
  auto active = status_.value("active", Json(nullptr));
  const auto state = fresh_ ? status_.value("state", "unknown") : "unknown";
  Elements runtime{
      heading("Runtime", "ACTUAL"),
      hbox(
          {text(state) | bold |
               ink(state == "running" ? Role::Running : Role::Warning),
           text(active.is_object()
                    ? "  " +
                          [&] {
                            auto version = active.value("core_version", "");
                            if (version.starts_with("Xray "))
                              version.erase(0, 5);
                            if (auto end = version.find_first_of(" \n");
                                end != std::string::npos)
                              version.resize(end);
                            return "Xray " + version;
                          }() +
                          "  PID " + scalar(status_.value("pid", Json(nullptr)))
                    : "") |
               ink(Role::Muted)}),
      key_value("Running Profile", fresh_ ? active_name() : "unavailable"),
      key_value("Actual target", fresh_ ? target_label(status_.value(
                                              "actual_target", Json(nullptr)))
                                        : "unavailable"),
      key_value("Health", !fresh_              ? "Unknown"
                          : state == "stopped" ? "Not running"
                          : state != "running" ? "Unconfirmed"
                          : status_.value("health", "unknown") == "process_only"
                              ? "Process alive; traffic unverified"
                              : status_.value("health", "unknown"))};
  Elements saved{
      heading("Saved configuration", "VIEW " + name(view_)),
      key_value("Saved target",
                target_label(profile_.value("selected", Json(nullptr)))),
      key_value("Next Connect", use_.empty() ? "No Use Profile" : name(use_)),
      text("Saved settings apply only on Connect / Apply") | ink(Role::Muted)};
  Elements body;
  if (Terminal::Size().dimx >= 120)
    body.push_back(
        hbox({vbox(runtime) | xflex, text("    "), vbox(saved) | xflex}));
  else {
    body.push_back(vbox(runtime));
    body.push_back(text(""));
    body.push_back(vbox(saved));
  }
  if (fresh_ && state == "running" && active.is_object() &&
      active.value("profile_id", "") == view_ &&
      status_.value("override_state", "none") == "none" &&
      profile_.value("selected", Json(nullptr)) !=
          status_.value("actual_target", Json(nullptr)))
    body.push_back(
        text("Saved target changed since this run · Connect to apply") |
        ink(Role::Muted));
  if (draft_.dirty())
    body.push_back(text("* Unsaved module  " + draft_.module) |
                   ink(Role::Warning));
  if (status_.value("override_state", "none") != "none")
    body.push_back(key_value("Runtime override",
                             status_.value("override_state", "unknown")));
  if (status_.contains("error") && status_["error"].is_object())
    body.push_back(paragraph("! " + status_["error"].value("message", "")) |
                   ink(Role::Error));
  body.push_back(text(""));
  body.push_back(heading("Statistics", "OBSERVED"));
  auto sample = stats_.value("sample", Json(nullptr));
  auto observations = sample.is_object()
                          ? sample.value("observations", Json::array())
                          : Json::array();
  body.push_back(
      text("Traffic: " + stats_.value("availability", "unavailable") +
           " · latency: " + (observations.empty() ? "unmeasured" : "sampled")));
  if (sample.is_object())
    for (auto &counter : sample.value("traffic", Json::array()))
      body.push_back(
          text(counter.value("tag", "") + "  total up " +
               scalar(counter.value("uplink", Json(nullptr))) + " B · down " +
               scalar(counter.value("downlink", Json(nullptr))) + " B"));
  for (auto &observation : observations)
    body.push_back(text(observation.value("tag", "") + "  latency " +
                        scalar(observation.value("latency_ms", Json(nullptr))) +
                        " ms · alive " +
                        scalar(observation.value("alive", Json(nullptr)))));
  if (stats_.contains("missing_conditions") &&
      stats_["missing_conditions"].is_array()) {
    std::string missing;
    for (auto &condition : stats_["missing_conditions"]) {
      if (!missing.empty())
        missing += ", ";
      missing += scalar(condition);
    }
    if (!missing.empty())
      body.push_back(paragraph("Unavailable reason: " + missing) |
                     ink(Role::Muted));
  }
  if (stats_.contains("rates") && stats_["rates"].is_array())
    for (auto &rate : stats_["rates"])
      if (rate.is_object())
        body.push_back(text(
            rate.value("name", rate.value("tag", "Traffic")) + "  " + "up " +
            scalar(rate.value("uplink_bytes_per_second", Json(nullptr))) +
            " B/s · down " +
            scalar(rate.value("downlink_bytes_per_second", Json(nullptr))) +
            " B/s"));
  body.push_back(text(""));
  body.push_back(text("Inventory  " +
                      (loaded_ ? ui::Count(nodes_.size(), "node") + " · " +
                                     ui::Count(subscriptions_.size(), "source")
                               : "unavailable")));
  if (!operations_.empty())
    body.push_back(text("Pending  " + operations_.begin()->second));
  return vbox(std::move(body)) | focusPosition(0, overview_scroll_) | yframe |
         flex;
}

Component Workspace::ActiveChild() {
  if (!overlays_.empty())
    return overlays_.back();
  return layer_ == Layer::Content ? content() : nullptr;
}
Element Workspace::OnRender() {
  // ScreenInteractive capability discovery runs after Workspace construction.
  // Reapply the selected policy before every frame, including the first one.
  apply_colors();
  using namespace ui::style;
  auto dimensions = Terminal::Size();
  theme = settings_.value("appearance", Json::object()).value("theme", "dark");
  if (dimensions.dimx < 80 || dimensions.dimy < 24)
    return vbox({text("XRM · Terminal too small") | bold,
                 text(std::to_string(dimensions.dimx) + "x" +
                      std::to_string(dimensions.dimy) +
                      " available; 80x24 required"),
                 text("Your draft and focus are preserved.") |
                     ink(Role::Muted)}) |
           center | surface();
  Elements nav{text("XRM ") | bold | ink(Role::Accent)};
  for (int i = 0; i < 7; i++) {
    auto n = text(std::string(i == page_ ? "[" : " ") + pages_[i] +
                  (i == page_ ? "]" : " "));
    n = i == page_ ? n | selected(layer_ == Layer::Top) : n | ink(Role::Muted);
    nav.push_back(n);
  }
  Elements top{hbox(std::move(nav)) | panel()};
  auto subs = secondary_items();
  if (!subs.empty()) {
    std::vector<std::string> labels;
    for (const auto &item : subs)
      labels.push_back(item.second);
    top.push_back(ui::Navigation(labels, secondary_, dimensions.dimx - 2,
                                 layer_ == Layer::Secondary));
  }
  std::string title = pages_[page_], subtitle;
  switch (page_) {
  case 0:
    subtitle = "Runtime & saved configuration";
    break;
  case 1:
    subtitle = loaded_ ? ui::Count(profiles_.size(), "Profile") +
                             " / independent runtime contexts"
                       : "Unavailable";
    break;
  case 2:
    title = draft_.module.empty() ? "Modules" : draft_.module;
    subtitle = std::string(draft_.runtime_view ? "RUNTIME · READ ONLY"
                           : writable()        ? "SOURCE · EDIT"
                                               : "SOURCE · READ ONLY") +
               (draft_.visual ? " · Visual" : " · JSON") +
               (draft_.dirty() ? " · UNSAVED *" : "");
    break;
  case 3:
    subtitle = loaded_ ? ui::Count(subscriptions_.size(), "source") +
                             " / node inventory"
                       : "Unavailable";
    break;
  case 4:
    subtitle = section() == "groups" ? "Groups · selection bindings"
                                     : "Node inventory";
    if (!search_.empty())
      subtitle += "  /  Search: " + search_;
    break;
  case 5:
    subtitle = "Core output";
    break;
  case 6:
    title = "Config / " + section();
    subtitle = section() == "Core"      ? "Installed & registered Xray binaries"
               : section() == "Geodata" ? "Resource cache"
                                        : "XRM program settings";
    break;
  default:
    break;
  }
  top.push_back(heading(title, subtitle));
  Element body =
      page_ == 0 ? overview()
      : content()
          ? content()->Render()
          : vbox({text(""), text("View a Profile to inspect native modules") |
                                ink(Role::Muted)});
  if (page_ == 2 && !draft_.error.empty())
    body = vbox(
        {paragraph("! " + ui::Clip(draft_.error,
                                   std::max(40, dimensions.dimx - 4) * 3)) |
             ink(Role::Error),
         body | flex});
  if (page_ == 5 && !fresh_ && raw_logs_.empty())
    body = text("Log stream unavailable · daemon disconnected") |
           ink(Role::Warning);
  auto context_name = [&](const std::string &value) {
    const int budget = std::max(8, (dimensions.dimx - 30) / 4);
    if (string_width(value) <= budget)
      return value;
    std::string clipped;
    int used = 0;
    for (const auto &glyph : Utf8ToGlyphs(value)) {
      if (used + string_width(glyph) > budget - 1)
        break;
      clipped += glyph;
      used += string_width(glyph);
    }
    return clipped + "…";
  };
  std::string context =
      "Viewing: " + context_name(name(view_)) +
      " · Next: " + context_name(name(use_)) +
      " · Module Edit: " + context_name(name(edit_)) +
      " · Running: " + context_name(fresh_ ? active_name() : "unknown");
  std::string details = context_details();
  if (auto end = details.find('\n'); end != std::string::npos)
    details.resize(end);
  if (page_ == 2 && !draft_.module.empty())
    details = draft_.module + " · " +
              (draft_.runtime_view ? "Runtime read-only"
               : writable()        ? "Source editable"
                                   : "Source read-only") +
              " · " + (draft_.visual ? "Visual" : "JSON") +
              (draft_.dirty() ? " · Unsaved *" : "") +
              (draft_.included ? " · included" : " · excluded") +
              (draft_.expected ? "" : " · no source");
  if (page_ == 5) {
    details = !fresh_ && raw_logs_.empty()
                  ? "Log stream unavailable · disconnected"
                  : std::string(!fresh_              ? "Cached · disconnected"
                                : logs_->following() ? "Following"
                                                     : "Paused") +
                        " · " + ui::Count(logs_->record_count(), "text line");
    if (logs_->unseen())
      details +=
          " · " + ui::Count(logs_->unseen(), "new text line") + " buffered";
    if (log_dropped_)
      details += " · earlier output evicted";
    if (log_truncated_)
      details += " · server history incomplete";
  }
  std::string state =
      std::string(fresh_    ? "xrmd ready"
                  : loaded_ ? "xrmd disconnected · cached data"
                            : "xrmd unavailable") +
      " · " +
      (layer_ == Layer::Top         ? "Page navigation (TopNav)"
       : layer_ == Layer::Secondary ? "Section selection (SecondaryNav)"
                                    : "Page content (Content)");
  if (page_ == 2 && !draft_.module.empty())
    state += " · " + (draft_.runtime_view ? draft_.runtime : draft_.source);

  const int body_height = dimensions.dimy - int(top.size()) - 5;
  auto area = body | size(HEIGHT, EQUAL, body_height);
  // Notifications are below modal dialogs in the overlay stack. They never
  // consume layout rows, focus, or input, and can't hide a dialog's controls.
  if (overlays_.empty() &&
      !(page_ == 2 && !draft_.visual && writable() &&
        layer_ == Layer::Content) &&
      !toasts_.visible().empty())
    area = dbox({area, toasts_.render()});
  if (!overlays_.empty())
    area = area | dim;
  if (!overlays_.empty()) {
    auto modal =
        hbox({text(" "), overlays_.back()->Render() | xflex, text(" ")}) |
        size(WIDTH, EQUAL, std::min(100, dimensions.dimx - 6)) |
        size(HEIGHT, LESS_THAN, body_height - 2) | border | panel();
    area = dbox({area, modal | center});
  }
  auto root = vbox(
      {vbox(top), separator() | rule(), area | size(HEIGHT, EQUAL, body_height),
       separator() | rule(), ui::InfoPane(context, details, state)});
  return hbox({text(" "), root | xflex, text(" ")}) | surface() |
         size(HEIGHT, EQUAL, dimensions.dimy) |
         size(WIDTH, EQUAL, dimensions.dimx);
}
bool Workspace::OnEvent(Event event) {
  // Overlay events are consumed before page/navigation events; Esc closes one
  // local focus layer and cannot also back out of the workspace.
  if (event == Event::Custom) {
    poll();
    return true;
  }
  auto dimensions = Terminal::Size();
  if (dimensions.dimx < 80 || dimensions.dimy < 24)
    return true;
  if (event == Event::F1) {
    help();
    return true;
  }
  if (!overlays_.empty()) {
    auto overlay = overlays_.back();
    bool handled = overlay->OnEvent(event);
    if (!handled && event == Event::Escape && !overlays_.empty() &&
        overlays_.back() == overlay)
      pop();
    return true;
  }
  if (event == Event::Character("?") &&
      !(page_ == 2 && !draft_.visual && writable() &&
        layer_ == Layer::Content)) {
    help();
    return true;
  }
  if (layer_ != Layer::Content) {
    if (event == Event::Tab || event == Event::TabReverse) {
      int delta = event == Event::Tab ? 1 : -1;
      leave_draft([this, delta] {
        if (layer_ == Layer::Top) {
          if (!secondary_items().empty())
            selected_section_ids_[page_] = section();
          page_ = (page_ + delta + 7) % 7;
          secondary_ = 0;
          const auto items = secondary_items();
          for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].first == selected_section_ids_[page_])
              secondary_ = static_cast<int>(i);
          detail_.clear();
          detail_title_.clear();
        } else {
          int n = int(secondary_items().size());
          if (n) {
            secondary_ = (secondary_ + delta + n) % n;
            selected_section_ids_[page_] = section();
            detail_.clear();
            detail_title_.clear();
          }
        }
        refresh_tables();
        if (page_ == 2)
          load_module();
      });
      return true;
    }
    if (event == Event::Return) {
      if (layer_ == Layer::Top && !secondary_items().empty())
        layer_ = Layer::Secondary;
      else {
        layer_ = Layer::Content;
        if (page_ == 2)
          load_module();
        if (auto c = content())
          c->TakeFocus();
      }
      return true;
    }
    if (event == Event::Escape) {
      layer_ = Layer::Top;
      return true;
    }
    if (event == Event::Character("q") && layer_ == Layer::Top) {
      leave_draft([this] { screen_.ExitLoopClosure()(); });
      return true;
    }
    return true;
  }
  if (event == Event::Escape) {
    leave_draft([this] {
      layer_ = secondary_items().empty() ? Layer::Top : Layer::Secondary;
    });
    return true;
  }
  if (page_ == 0) {
    if (event == Event::ArrowDown || event == Event::PageDown) {
      overview_scroll_ = std::min(
          overview_scroll_ + (event == Event::PageDown ? 10 : 1), 10000);
      return true;
    }
    if (event == Event::ArrowUp || event == Event::PageUp) {
      overview_scroll_ =
          std::max(overview_scroll_ - (event == Event::PageUp ? 10 : 1), 0);
      return true;
    }
    if (event == Event::Home) {
      overview_scroll_ = 0;
      return true;
    }
  }
  if (page_ == 2) {
    for (auto &a : actions()) {
      bool match = (a.key == "F2" && event == Event::F2) ||
                   (a.key == "F6" && event == Event::F6) ||
                   (a.key == "Ctrl-S" && event == Event::Special("\x13"));
      if (match) {
        if (a.disabled.empty())
          a.run();
        else
          notify(a.disabled, ui::Tone::Warning);
        return true;
      }
    }
    if (!draft_.visual) {
      if (code_)
        code_->OnEvent(event);
      return true;
    }
  }
  if (event == Event::Return && page_ != 2) {
    view_detail();
    return true;
  }
  for (auto &a : actions())
    if (event == Event::Character(a.key)) {
      if (a.disabled.empty())
        a.run();
      else
        notify(a.label + ": " + a.disabled, ui::Tone::Warning);
      return true;
    }
  if (auto c = content())
    return c->OnEvent(event);
  return true;
}
std::string Workspace::context_details() const {
  if (page_ == 1) {
    const auto core = find_id(cores_, profile_.value("core_id", ""));
    auto summary = "View " + name(view_) + " · Core " +
                   core.value("name", "not set") + " · Saved " +
                   target_label(profile_.value("selected", Json(nullptr)));
    summary += "\nProfile ID: " + view_ + "\nSources: ";
    if (!profile_.contains("enabled_source_ids") ||
        !profile_["enabled_source_ids"].is_array())
      summary += "All sources (legacy scope)";
    else
      for (const auto &id : profile_["enabled_source_ids"])
        summary += find_id(subscriptions_, id.get<std::string>())
                       .value("name", id.get<std::string>()) +
                   "; ";
    summary += "\nModules: ";
    for (const auto &module : modules_)
      summary +=
          module.value("key", module.value("module", "")) +
          (module.value("included", false) ? " included; " : " excluded; ");
    if (profiles_table_->selected_id() != view_)
      summary += "\nHighlighted: " + name(profiles_table_->selected_id()) +
                 " (Enter changes View only)";
    return summary;
  }
  auto t = table();
  if (!t || t->selected_id().empty())
    return view_.empty() ? "No View Profile · open Help for available tasks"
                         : "View " + name(view_) +
                               (section().empty() ? "" : " · " + section());
  Json row =
      page_ == 3 ? find_id(subscriptions_, t->selected_id())
      : page_ == 4
          ? find_id(section() == "groups" ? groups_ : nodes_, t->selected_id())
      : page_ == 6 && section() == "Core" ? find_id(cores_, t->selected_id())
      : page_ == 6 && section() == "Geodata"
          ? find_id(geodata_, t->selected_id())
          : Json::object();
  if (row.empty())
    return "XRM / " + section() +
           " · preferences do not modify native Xray modules";
  std::string result = "Highlighted " + row.value("name", "");
  if (page_ == 4)
    result +=
        " · Saved " + target_label(profile_.value("selected", Json(nullptr))) +
        " · Running " +
        (fresh_ ? target_label(status_.value("actual_target", Json(nullptr)))
                : "unknown");
  if (page_ == 6)
    result += " · " + row.value("path", row.value("file_name", ""));
  if (page_ == 3)
    result += " · " + subscription_result(row) + " · " +
              scalar(row.value("node_count", Json(nullptr))) + " nodes";
  result += "\nID: " + row.value("id", "");
  if (page_ == 3) {
    auto timestamp = [](const Json &value) {
      if (!value.is_number_integer())
        return std::string("Not yet");
      const auto seconds =
          static_cast<std::time_t>(value.get<std::int64_t>() / 1000);
      std::tm utc{};
      char buffer[32]{};
      gmtime_r(&seconds, &utc);
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &utc);
      return std::string(buffer);
    };
    result += "\nLast attempt: " +
              timestamp(row.value("last_attempt", Json(nullptr))) +
              "\nLast success: " +
              timestamp(row.value("last_success", Json(nullptr)));
    if (row.contains("last_error") && row["last_error"].is_string()) {
      const auto stage = row.value("last_error_stage", "source");
      result +=
          "\nLast " + stage + " issue: " + row["last_error"].get<std::string>();
      result += "\nExisting inventory is retained after a failed refresh.";
    }
    const auto sources = profile_.value("enabled_source_ids", Json(nullptr));
    result += "\nView Profile binding (" + name(view_) + "): ";
    if (view_.empty())
      result += "No View Profile";
    else if (!sources.is_array())
      result += "Included (legacy all-source scope)";
    else
      result +=
          std::ranges::find(sources, Json(t->selected_id())) != sources.end()
              ? "Included"
              : "Not included";
    result +=
        "\nOther Profile bindings: inspect each Profile's source selection.";
  }
  if (page_ == 4 && section() != "groups") {
    const auto source = row.value("source_id", Json(nullptr));
    result +=
        "\nSource: " + (source.is_string()
                            ? find_id(subscriptions_, source.get<std::string>())
                                  .value("name", "Unavailable source")
                            : "Local inventory");
    result += std::string("\nSource present: ") +
              (row.value("source_missing", false) ? "no" : "yes");
  }
  for (const auto &key :
       {"protocol", "transport", "security", "enabled", "support",
        "reported_version", "path", "file_name", "last_result",
        "unsupported_count", "malformed_count"})
    if (row.contains(key))
      result += "\n" + std::string(key) + ": " + scalar(row[key]);
  return result;
}
void Workspace::full_detail() {
  auto value = std::make_shared<std::string>(detail_title_ + "\n" + detail_ +
                                             "\nSocket: " + socket_);
  auto c = ui::ReadOnlyText(value.get());
  push(ui::Dialog("Details", Renderer(c, [c, value] { return c->Render(); }),
                  [this] { pop(); }));
}
void Workspace::view_detail() {
  if (page_ == 1) {
    auto id = profiles_table_->selected_id();
    if (id.empty())
      return;
    leave_draft([this, id] {
      view_ = id;
      refresh_profile();
      auto p = find_id(profiles_, id);
      detail_title_ = p.value("name", id);
      detail_ =
          "Profile " + detail_title_ + " · Core " +
          find_id(cores_, profile_.value("core_id", "")).value("name", "none") +
          " · sources " +
          (profile_.contains("enabled_source_ids") &&
                   profile_["enabled_source_ids"].is_array()
               ? std::to_string(profile_["enabled_source_ids"].size())
               : "all") +
          " · ID " + id;
      refresh_tables();
    });
  } else if (page_ == 3) {
    auto s = find_id(subscriptions_, subscriptions_table_->selected_id());
    detail_title_ = s.value("name", "");
    detail_ = "Source " + detail_title_ + " · " + subscription_result(s) +
              " · nodes " + scalar(s.value("node_count", Json(nullptr))) +
              " · unsupported " +
              scalar(s.value("unsupported_count", Json(0))) + " · malformed " +
              scalar(s.value("malformed_count", Json(0))) + " · ID " +
              s.value("id", "");
    if (s.contains("last_error") && s["last_error"].is_string())
      detail_ +=
          "\nLast " + s.value("last_error_stage", "source") +
          " issue: " + s["last_error"].get<std::string>() +
          "\nExisting inventory remains available after a failed refresh.";
  } else if (page_ == 4) {
    auto row = section() == "groups"
                   ? find_id(groups_, groups_table_->selected_id())
                   : find_id(nodes_, nodes_table_->selected_id());
    if (row.empty())
      return;
    detail_title_ = row.value("name", "");
    detail_ = "Cursor " + detail_title_ + " · Saved " +
              target_label(profile_.value("selected", Json(nullptr))) +
              " · ID " + row.value("id", "");
    if (section() != "groups") {
      detail_ = context_details();
      full_detail();
    }
  } else if (page_ == 6) {
    auto row = section() == "Core"
                   ? find_id(cores_, cores_table_->selected_id())
                   : find_id(geodata_, geodata_table_->selected_id());
    if (row.empty())
      return;
    detail_title_ = row.value("name", section());
    detail_ = "Object " + detail_title_ + " · " +
              row.value("path", row.value("file_name", "")) + " · ID " +
              row.value("id", "");
    if (section() == "Core") {
      detail_ = context_details();
      full_detail();
    }
  } else if (page_ == 5) {
    if (const auto *record = logs_->selected_record()) {
      detail_title_ = "Log text line";
      detail_ =
          "Time    " +
          (record->timestamp.empty() ? "not supplied" : record->timestamp) +
          "\nTransport chunk  " + std::to_string(record->sequence) +
          "\nSource  " + record->source + "\nLevel   " +
          (record->level.empty() ? "not supplied" : record->level) +
          "\nSubsystem  " + record->subsystem + "\n\n";
      detail_ +=
          "Line within chunk  " + std::to_string(record->line + 1) + "\n\n";
      detail_ += ui::LogTextForDisplay(record->raw, true);
      full_detail();
    }
  }
}
} // namespace xrm::tui

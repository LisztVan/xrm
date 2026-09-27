// XRM: Builds executable contextual TUI Help actions.

#include "tui/shell/workspace.hpp"
namespace xrm::tui {
std::vector<Action> Workspace::actions() {
  std::vector<Action> out;
  auto add = [&](std::string key, std::string label, std::function<void()> fn,
                 std::string disabled = "", std::string target = "") {
    out.push_back({std::move(key), std::move(label),
                   target.empty() ? pages_[page_] : target, std::move(disabled),
                   std::move(fn)});
  };
  auto t = table();
  auto id = t ? t->selected_id() : "";
  auto need = id.empty() ? "Select an object" : "";
  auto need_view = view_.empty() ? "View a Profile" : "";
  if (page_ == 0) {
    add(
        "c", "Connect saved", [this] { connect(); },
        use_.empty() ? "Use is empty" : "", name(use_));
    add("d", "Stop core", [this] {
      auto status = call("runtime.status");
      if (!status) {
        notify(last_error_, ui::Tone::Error);
        return;
      }
      const auto active = status->value("active", Json(nullptr));
      const auto state = status->value("state", "unknown");
      if (state == "starting" && !active.is_object()) {
        confirm("Cancel core start",
                "Cancel the current start attempt? Saved settings and module "
                "drafts remain.",
                [this] {
                  operation("connection.disconnect",
                            {{"expected_run_id", nullptr}},
                            "Cancel core start");
                });
        return;
      }
      if (!active.is_object() || !active.contains("run_id") ||
          !active["run_id"].is_string() ||
          active["run_id"].get<std::string>().empty()) {
        notify(state == "stopped" ? "Core is already stopped"
                                  : "No confirmed running instance to stop; "
                                    "refresh runtime status",
               ui::Tone::Info);
        return;
      }
      const auto run_id = active["run_id"].get<std::string>();
      const auto profile = active.value("profile_id", "");
      confirm("Stop core",
              "Stop " + name(profile) + " (run " + run_id +
                  ")? Current connections will close. Saved settings and "
                  "module drafts remain.",
              [this, run_id, profile] {
                operation("connection.disconnect",
                          {{"expected_run_id", run_id}},
                          "Stop " + name(profile));
              });
    });
    add("f", "Runtime paths", [this] {
      detail_title_ = "Runtime";
      detail_ = status_.contains("active") && status_["active"].is_object()
                    ? status_["active"].value("confdir", "No confdir")
                    : "No running confdir";
      full_detail();
    });
    add("i", "Inspect local entry", [this] { local_entry(); }, need_view);
  }
  if (page_ == 1) {
    add(
        "u", id == use_ ? "Cancel Use" : "Use Profile",
        [this, id] {
          use_ = use_ == id ? "" : id;
          refresh_tables();
          notify((use_.empty() ? "Cancelled Use for " : "Use set to ") +
                 name(id) + "; runtime unchanged");
        },
        need, name(id));
    add(
        "e", id == edit_ ? "End module Edit" : "Allow module Edit",
        [this] { toggle_edit(); }, need, name(id));
    add("n", "New Profile", [this] { profile_form("new"); });
    add("R", "Rename Profile", [this] { profile_form("rename"); }, need);
    add("m", "Profile metadata", [this] { profile_form("metadata"); }, need);
    add("c", "Copy Profile", [this] { profile_form("copy"); }, need);
    add(
        "D", "Delete Profile",
        [this, id] {
          leave_draft([this, id] {
            confirm("Delete Profile", name(id), [this, id] {
              if (mutate("profile.remove", {{"profile_id", id}},
                         "Delete " + name(id))) {
                if (use_ == id)
                  use_.clear();
                if (edit_ == id)
                  edit_.clear();
                if (view_ == id)
                  view_.clear();
                draft_ = {};
                refresh();
              }
            });
          });
        },
        need);
  }
  if (page_ == 2) {
    add("F2", draft_.visual ? "Open JSON" : "Open Visual",
        [this] { switch_module_view(); });
    add(
        "Ctrl-S", "Save module source", [this] { save_module(); },
        writable() ? "" : "Module is read-only");
    add(
        "F6", "External editor", [this] { external_editor(); },
        writable() ? "" : "Module is read-only");
    auto ro = writable()            ? ""
              : draft_.runtime_view ? "Runtime is always read-only"
                                    : "Enable module Edit for " + name(view_);
    add(
        "I", draft_.included ? "Exclude module" : "Include module",
        [this] { include_module(); }, ro);
    add("i", "Source / runtime paths", [this] {
      detail_title_ = draft_.module;
      auto active = status_.value("active", Json(nullptr));
      const auto runtime =
          active.is_object() && active.value("profile_id", "") == draft_.profile
              ? active.value("confdir", "") + "/" + draft_.runtime
              : "Not generated for this Profile · " + draft_.runtime;
      detail_ = "Source: " + draft_.source + "\nRuntime: " + runtime +
                "\nIncluded: " + (draft_.included ? "yes" : "no") +
                "\nSource exists: " + (draft_.expected ? "yes" : "no");
      full_detail();
    });
    add("v", draft_.runtime_view ? "View Source" : "View Runtime", [this] {
      leave_draft([this] {
        if (draft_.runtime_view) {
          draft_ = {};
          load_module();
          return;
        }
        if (!status_.contains("active") || !status_["active"].is_object() ||
            status_["active"].value("profile_id", "") != view_) {
          notify("Runtime belongs to " + active_name() +
                     "; View that Profile to inspect it",
                 ui::Tone::Warning);
          return;
        }
        auto r = call("config.files",
                      {{"profile_id", view_}, {"source", "runtime"}});
        if (!r) {
          notify(last_error_, ui::Tone::Error);
          return;
        }
        std::string value;
        for (auto &f : r->value("files", Json::array()))
          if (f.value("file", "") == draft_.runtime)
            value += f.value("text", "");
        if (value.empty()) {
          notify("No generated runtime file; Connect or Preview explicitly",
                 ui::Tone::Warning);
          return;
        }
        draft_.text = value;
        draft_.baseline = value;
        draft_.runtime_view = true;
        rebuild_module();
      });
    });
    if (draft_.module == "routing")
      add("p", "Routing preset", [this] { routing_preset(); }, ro);
    add("s", "Save module source", [this] { save_module(); }, ro);
  }
  if (page_ == 3) {
    add("a", "Add subscription", [this] { subscription_form(false); });
    add("e", "Edit subscription", [this] { subscription_form(true); }, need);
    add(
        "u", "Update subscription",
        [this, id] {
          operation("subscription.refresh", {{"source_ids", Json::array({id})}},
                    "Update " + find_id(subscriptions_, id).value("name", id));
        },
        need);
    add("U", "Update all subscriptions", [this] {
      operation("subscription.refresh", Json::object(),
                "Update all subscriptions");
    });
    add(
        "t", "Pause / resume auto refresh",
        [this, id] {
          auto s = find_id(subscriptions_, id);
          mutate("subscription.replace",
                 {{"id", id}, {"auto_refresh", !s.value("auto_refresh", true)}},
                 "Auto refresh");
        },
        need);
    add("b", "Bind sources", [this] { bind_sources(); }, need_view);
    add("D", "Remove subscription", [this] { remove_subscription(); }, need);
    add("h", "History cleanup", [this] { history(); }, need);
  }
  if (page_ == 4) {
    if (section() == "groups") {
      add("a", "Add Group", [this] { group_form(false); }, need_view);
      add("e", "Edit Group", [this] { group_form(true); }, need);
      add("m", "Select manual member", [this] { group_member(); }, need);
      add(
          "D", "Remove Group",
          [this, id] {
            confirm("Remove Group", find_id(groups_, id).value("name", id),
                    [this, id] {
                      mutate("group.remove",
                             {{"profile_id", view_}, {"group_id", id}},
                             "Group removal");
                    });
          },
          need);
    } else {
      add("a", "Add node", [this] { node_form(false); });
      add("e", "Edit node", [this] { node_form(true); }, need);
      add("J", "Advanced node editor", [this] { node_advanced(); }, need);
      add("i", "Import share URI / native outbound", [this] { node_import(); });
      add("C", "Copy as local node", [this] { node_copy(); }, need);
      add("l", "Alias and labels", [this] { node_labels(); }, need);
      add(
          "D", "Remove local node",
          [this, id] {
            confirm("Remove node", find_id(nodes_, id).value("name", id),
                    [this, id] {
                      mutate("node.remove", {{"id", id}}, "Node removal");
                    });
          },
          need);
      add(
          "x", "Enable / disable node",
          [this, id] {
            auto n = find_id(nodes_, id);
            mutate("node.set_enabled",
                   {{"id", id}, {"enabled", !n.value("enabled", true)}},
                   "Node availability");
          },
          need);
      add("t", "Test node", [this] { node_test(); }, need);
      add("T", "Node test results", [this] { node_results(); }, need);
      add("k", "Cancel active node tests", [this] { cancel_node_tests(); });
      add("/", "Filter nodes", [this] {
        form(
            "Filter nodes", {text_field("Name contains", search_)},
            [this](ui::Form &f) -> std::optional<std::string> {
              search_ = f.value(0);
              refresh_tables();
              return {};
            },
            "Filter");
      });
      add("f", "Show / hide missing nodes", [this] {
        show_missing_ = !show_missing_;
        refresh_tables();
      });
    }
    add(
        "s", "Save target", [this] { save_target(false); },
        view_.empty() ? need_view : need, name(view_));
    add(
        "c", "Save & Connect highlighted node", [this] { save_target(true); },
        use_.empty()    ? "Use is empty"
        : use_ != view_ ? "Use must match View"
                        : need,
        name(use_));
  }
  if (page_ == 5) {
    add("p", logs_->following() ? "Pause reading" : "Resume following",
        [this] { logs_->set_follow(!logs_->following()); });
    add("i", "Raw log details", [this] { view_detail(); });
  }
  if (page_ == 6) {
    if (section() == "Core") {
      add("a", "Register core", [this] { core_form(); });
      add(
          "u", "Use core in viewed Profile",
          [this, id] {
            operation("core.use", {{"profile_id", view_}, {"core_id", id}},
                      "Use core");
          },
          releases_view_  ? "Select an installed core"
          : view_.empty() ? need_view
                          : need);
      add(
          "D", "Remove managed core",
          [this, id] {
            confirm("Remove core", id, [this, id] {
              mutate("core.remove", {{"core_id", id}}, "Core removal");
            });
          },
          releases_view_ ? "Select an installed core" : need);
      add("l", "Official releases", [this] { core_releases(false); });
      add("p", "Include prereleases", [this] { core_releases(true); });
      add(
          "i", "Install selected release", [this] { core_install(); },
          releases_view_ ? need : "Load releases first");
      add("o", "Installed cores", [this] {
        releases_view_ = false;
        refresh_tables();
      });
    } else if (section() == "Geodata") {
      add("a", "Add resource", [this] { geodata_form(false); });
      add("e", "Edit resource", [this] { geodata_form(true); }, need);
      add(
          "u", "Update resource",
          [this, id] {
            auto core = find_id(geodata_, id).value("validator_core_id", "");
            if (core.empty())
              core = profile_.value("core_id", "");
            if (core.empty()) {
              notify("Choose a validator core in Edit resource or View a "
                     "Profile with a core",
                     ui::Tone::Warning);
              return;
            }
            operation("geodata.update", {{"id", id}, {"core_id", core}},
                      "Update resource");
          },
          need);
      add("b", "Bind resources", [this] { bind_geodata(); }, need_view);
      add(
          "D", "Remove resource",
          [this, id] {
            confirm("Remove resource", id, [this, id] {
              mutate("geodata.remove", {{"id", id}}, "Resource removal");
            });
          },
          need);
    } else if (section() != "General")
      add("e", "Edit settings", [this] { settings_form(); });
  }
  add("N", "Notification history", [this] {
    detail_title_ = "Notifications";
    detail_.clear();
    for (auto &toast : toasts_.history())
      detail_ +=
          toast.message +
          (toast.repeats > 1
               ? " · repeated " + std::to_string(toast.repeats) + " times"
               : "") +
          "\n";
    if (detail_.empty())
      detail_ = "No notifications";
    full_detail();
  });
  add("r", "Refresh", [this] {
    notify("Refreshing " + std::string(pages_[page_]) + "…");
    refresh();
    if (page_ == 5)
      read_logs();
    notify(fresh_ ? "Refreshed " + std::string(pages_[page_])
                  : "Refresh incomplete; cached data retained. Check the "
                    "connection in Help.",
           fresh_ ? ui::Tone::Info : ui::Tone::Warning);
  });
  if (page_ != 2)
    add("F", "Full object details", [this] {
      detail_title_ = pages_[page_];
      detail_ = context_details();
      full_detail();
    });
  if (!endpoint_connected_) {
    const auto needs_service = [this](const Action &a) {
      const auto &k = a.key;
      if (page_ == 0)
        return k == "c" || k == "d";
      if (page_ == 1)
        return k == "D";
      if (page_ == 2)
        return k == "s" || k == "Ctrl-S" || k == "I" || k == "p";
      if (page_ == 3)
        return k == "u" || k == "U" || k == "t" || k == "D" || k == "h";
      if (page_ == 4)
        return k == "D" || k == "x" || k == "t" || k == "k" || k == "s" ||
               k == "c" || k == "m";
      if (page_ == 6)
        return k == "u" || k == "D" || k == "i" ||
               (section() == "Geodata" && k == "u");
      return false;
    };
    for (auto &action : out)
      if (needs_service(action))
        action.disabled =
            "xrmd is unavailable at " + socket_ +
            "; keep local drafts, then Refresh after the endpoint returns";
  }
  return out;
}
void Workspace::connect() {
  if (use_.empty()) {
    notify("Use is empty", ui::Tone::Warning);
    return;
  }
  operation("connection.connect", {{"profile_id", use_}},
            "Connect " + name(use_));
}
void Workspace::save_target(bool connect_after) {
  auto t = table();
  if (!t || t->selected_id().empty() || view_.empty())
    return;
  if (connect_after && (use_.empty() || use_ != view_)) {
    notify("Use must match View", ui::Tone::Warning);
    return;
  }
  auto r = call("connection.select",
                {{"profile_id", view_},
                 {"target",
                  {{"kind", section() == "groups" ? "group" : "node"},
                   {"id_or_tag", t->selected_id()}}}});
  if (!r) {
    notify(last_error_, ui::Tone::Error);
    return;
  }
  refresh_profile();
  refresh_tables();
  notify("Target saved for " + name(view_) + "; runtime unchanged",
         ui::Tone::Success);
  if (connect_after)
    connect();
}
} // namespace xrm::tui

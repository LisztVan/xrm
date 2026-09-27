// XRM: Owns the Modules page's single Source draft and its Visual/JSON views.
// Both editors point at draft_.text. Explicit Save actions write Source;
// Runtime stays read-only; publishing Source requires later Connect/Apply.

#include "model/module_catalog.hpp"
#include "tui/components/style.hpp"
#include "tui/forms/native_form.hpp"
#include "tui/shell/workspace.hpp"
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
namespace xrm::tui {
using namespace ftxui;
namespace {
std::string json_error(const Json::parse_error &error) {
  std::string message = error.what();
  auto position = message.find("line ");
  if (position != std::string::npos)
    message = message.substr(position);
  return "Invalid JSON " + message;
}
} // namespace

bool Workspace::writable() const {
  // Keep every write entry point tied to the same viewed Profile and draft.
  return !view_.empty() && edit_ == view_ && draft_.profile == view_ &&
         !draft_.runtime_view;
}
void Workspace::load_module() {
  if (view_.empty()) {
    draft_ = {};
    visual_.reset();
    code_.reset();
    return;
  }
  auto key = section();
  // Reuse a loaded draft for the same Profile/module during ordinary refresh.
  if (key == draft_.module && view_ == draft_.profile && !draft_.text.empty())
    return;
  auto r = call("module.get", {{"profile_id", view_}, {"module", key}});
  if (!r) {
    notify(last_error_, ui::Tone::Error);
    return;
  }
  draft_ = {};
  draft_.profile = view_;
  draft_.module = key;
  draft_.included = r->value("included", false);
  draft_.source = r->value("source_file", "");
  draft_.runtime = r->value("runtime_file", "");
  if (r->contains("text") && (*r)["text"].is_string()) {
    // expected is the server's original Source for optimistic save conflicts.
    draft_.expected = (*r)["text"].get<std::string>();
    draft_.text = *draft_.expected;
  } else
    draft_.text =
        Json{{key, (key == "inbounds" || key == "outbounds") ? Json::array()
                                                             : Json::object()}}
            .dump(2) +
        "\n";
  auto parsed = Json::parse(draft_.text, nullptr, false, true);
  if (parsed.is_discarded())
    draft_.visual = false;
  draft_.baseline = draft_.text;
  rebuild_module();
}
void Workspace::rebuild_module() {
  // Recreate controls after a mode/permission change, retaining draft_.text.
  // Visual edits update that same text; neither control writes the server.
  if (visual_)
    visual_->Detach();
  if (code_)
    code_->Detach();
  code_ = ui::TextArea(&draft_.text, !writable());
  Add(code_);
  forms::NativeFormOptions o;
  o.module = draft_.module;
  o.draft = &draft_.text;
  o.readonly = !writable();
  o.open_dialog = [this](Component c) { push(c); };
  o.close_dialog = [this] { pop(); };
  o.changed = [this] { draft_.error.clear(); };
  for (auto &n : nodes_) {
    auto source = n.value("source_id", Json(nullptr));
    if (source.is_string() && profile_.contains("enabled_source_ids") &&
        profile_["enabled_source_ids"].is_array() &&
        std::ranges::find(profile_["enabled_source_ids"], source) ==
            profile_["enabled_source_ids"].end())
      continue;
    o.outbound_tags.push_back(
        {node_tag(n.value("id", "")), n.value("name", "")});
  }
  o.open_groups = [this] {
    leave_draft([this] {
      page_ = 4;
      secondary_ = 2;
      selected_section_ids_[page_] = section();
      layer_ = Layer::Content;
      refresh_tables();
      groups_table_->TakeFocus();
    });
  };
  for (auto &g : groups_) {
    auto b = g.value("balancer", Json::object());
    if (b.contains("tag")) {
      o.bound_groups.push_back({b.value("tag", ""), g.value("name", "")});
      o.balancer_tags.push_back({b.value("tag", ""), g.value("name", "")});
    }
  }
  for (auto &m : modules_)
    if (m.contains("text") && m["text"].is_string()) {
      auto j = Json::parse(m["text"].get<std::string>(), nullptr, false, true);
      if (j.is_discarded())
        continue;
      auto k = m.value("key", "");
      if ((k == "inbounds" || k == "outbounds") && j.contains(k) &&
          j[k].is_array())
        for (auto &v : j[k])
          if (v.contains("tag") && v["tag"].is_string())
            (k == "inbounds" ? o.inbound_tags : o.outbound_tags)
                .push_back(
                    {v["tag"].get<std::string>(), v["tag"].get<std::string>()});
    }
  auto routing = Json::parse(draft_.text, nullptr, false, true);
  if (draft_.module == "routing" && !routing.is_discarded() &&
      routing.contains("routing") && routing["routing"].is_object())
    for (auto &b : routing["routing"].value("balancers", Json::array()))
      if (b.is_object() && b.contains("tag") && b["tag"].is_string())
        o.balancer_tags.push_back(
            {b["tag"].get<std::string>(), b["tag"].get<std::string>()});
  visual_ = forms::MakeNativeForm(std::move(o));
  Add(visual_);
  if (layer_ == Layer::Content)
    content()->TakeFocus();
}
bool Workspace::save_module() {
  if (!writable()) {
    notify("Modules are read-only; enable Edit for " + name(view_),
           ui::Tone::Warning);
    return false;
  }
  try {
    auto j = Json::parse(draft_.text, nullptr, true, true);
    if (!j.is_object() || !j.contains(draft_.module)) {
      draft_.error = "Expected native wrapper: " + draft_.module;
      return false;
    }
  } catch (const Json::parse_error &e) {
    draft_.error = json_error(e);
    notify(draft_.error + "; draft retained", ui::Tone::Error);
    return false;
  }
  Json params{{"profile_id", draft_.profile},
              {"module", draft_.module},
              {"text", draft_.text}};
  // Absence is a distinct baseline for a new module Source.
  if (draft_.expected)
    params["expected_text"] = *draft_.expected;
  else
    params["expected_absent"] = true;
  auto r = call("module.save", params);
  if (!r) {
    draft_.error = r.error().message;
    notify("Save " + draft_.module + ": " + draft_.error, ui::Tone::Error);
    return false;
  }
  draft_.expected = draft_.text;
  draft_.baseline = draft_.text;
  draft_.error.clear();
  notify("Saved " + name(draft_.profile) + " / " + draft_.module +
             "; runtime unchanged",
         ui::Tone::Success);
  refresh_profile();
  return true;
}
void Workspace::switch_module_view() {
  if (draft_.module.empty())
    return;
  if (!draft_.visual) {
    // An invalid JSON draft stays editable in JSON mode with its text intact.
    auto j = Json::parse(draft_.text, nullptr, false, true);
    if (j.is_discarded()) {
      try {
        [[maybe_unused]] const auto validated =
            Json::parse(draft_.text, nullptr, true, true);
      } catch (const Json::parse_error &e) {
        draft_.error = json_error(e);
      }
      notify(draft_.error + "; draft retained", ui::Tone::Error);
      return;
    }
  }
  draft_.visual = !draft_.visual;
  rebuild_module();
}
void Workspace::include_module() {
  if (!writable())
    return;
  auto r = call("module.include", {{"profile_id", draft_.profile},
                                   {"module", draft_.module},
                                   {"included", !draft_.included}});
  if (!r) {
    notify(last_error_, ui::Tone::Error);
    return;
  }
  draft_.included = !draft_.included;
  notify(draft_.module + (draft_.included ? " included" : " excluded") +
             "; runtime unchanged",
         ui::Tone::Success);
  refresh_profile();
}
void Workspace::external_editor() {
  if (!writable()) {
    notify("External editor requires module Edit", ui::Tone::Warning);
    return;
  }
  std::vector<std::string> args;
  auto editor = settings_.value("editor", Json::object());
  if (editor.contains("argv") && editor["argv"].is_array())
    args = editor["argv"].get<std::vector<std::string>>();
  if (args.empty()) {
    auto env = std::getenv("VISUAL");
    if (!env || !*env)
      env = std::getenv("EDITOR");
    if (env) {
      std::istringstream in(env);
      std::string a;
      while (in >> a)
        args.push_back(a);
    }
  }
  if (args.empty()) {
    notify("Configure an editor in Config / Editor", ui::Tone::Warning);
    return;
  }
  char pattern[] = "/tmp/xrm-rewrite-draft-XXXXXX";
  int fd = mkstemp(pattern);
  if (fd < 0) {
    notify("Cannot create editor draft", ui::Tone::Error);
    return;
  }
  close(fd);
  struct Cleanup {
    const char *path;
    ~Cleanup() { unlink(path); }
  } cleanup{pattern};
  {
    std::ofstream out(pattern);
    out << draft_.text;
  }
  args.emplace_back(pattern);
  std::vector<char *> argv;
  for (auto &a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  int result = -1;
  screen_.WithRestoredIO([&] {
    pid_t child;
    int error =
        posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ);
    if (error) {
      result = -1;
      return;
    }
    while (waitpid(child, &result, 0) < 0 && errno == EINTR) {
    }
  })();
  if (result != 0) {
    notify(result > 0 && WIFEXITED(result) && WEXITSTATUS(result) == 130
               ? "External editor cancelled; original draft retained"
               : "External editor failed; original draft retained",
           ui::Tone::Error);
    return;
  }
  // Import only after a successful editor exit; invalid JSON is still kept as
  // a local draft for repair, never silently saved or applied.
  std::ifstream in(pattern);
  draft_.text = {std::istreambuf_iterator<char>(in), {}};
  auto j = Json::parse(draft_.text, nullptr, false, true);
  if (j.is_discarded()) {
    draft_.visual = false;
    draft_.error = "External editor returned invalid JSON; draft retained";
  }
  rebuild_module();
}
void Workspace::routing_preset() {
  if (!writable() || draft_.module != "routing")
    return;
  leave_draft([this] {
    // The preview is based on saved routing Source. Pending edits must be
    // resolved before asking the server to plan and save a preset.
    form(
        "Routing preset",
        {select_field("Preset", "custom_native",
                      {{"bypass_mainland", "Bypass mainland"},
                       {"blacklist_proxy", "Blacklist proxy"},
                       {"global_proxy", "Global proxy"},
                       {"direct_only", "Direct only"},
                       {"custom_native", "Keep custom native"}})},
        [this](ui::Form &f) -> std::optional<std::string> {
          auto p = call("routing.preset.plan",
                        {{"profile_id", view_}, {"preset", f.value(0)}});
          if (!p)
            return p.error().message;
          auto plan = *p;
          auto preset = f.value(0);
          screen_.Post([this, plan, preset] {
            auto text = std::make_shared<std::string>(plan.dump(2));
            auto view = ui::ReadOnlyText(text.get());
            auto b =
                ui::style::button("Save routing source", [this, plan, preset] {
                  if (!writable()) {
                    notify("Edit is off", ui::Tone::Warning);
                    return;
                  }
                  auto r = call("routing.preset.save",
                                {{"profile_id", draft_.profile},
                                 {"preset", preset},
                                 {"expected_text",
                                  plan.value("expected_text",
                                             draft_.expected.value_or(""))}});
                  if (!r) {
                    notify(last_error_, ui::Tone::Error);
                    return;
                  }
                  pop();
                  draft_ = {};
                  load_module();
                  notify("Routing preset saved; runtime unchanged",
                         ui::Tone::Success);
                });
            auto c = Container::Vertical({view, b});
            push(ui::Dialog("Review routing changes",
                            Renderer(c, [c, text] { return c->Render(); }),
                            [this] { pop(); }));
          });
          return {};
        },
        "Preview");
  });
}
} // namespace xrm::tui

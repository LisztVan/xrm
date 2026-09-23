#include "cli/commands.hpp"

#include "client/rpc_client.hpp"
#include "contract/methods.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <uv.h>

#include <algorithm>
#include <optional>
#include <chrono>
#include <thread>
#include <utility>

namespace xrm::cli {
namespace {
using namespace ftxui;

Result<Json> rpc(const std::string& socket, std::string method, Json params = Json::object()) {
  uv_loop_t loop{};
  int status = uv_loop_init(&loop);
  if (status != 0) return std::unexpected(AppError{ErrorCode::io_error, uv_strerror(status), std::nullopt, std::nullopt});
  RpcClient client(loop);
  std::optional<Result<Json>> result;
  auto opened = client.open(socket, [&](Result<void> open_result) {
    if (!open_result) { result.emplace(std::unexpected(open_result.error())); return; }
    auto call = client.call(std::move(method), std::move(params), [&](Result<Json> response) {
      result.emplace(std::move(response));
      client.close();
    });
    if (!call) { result.emplace(std::unexpected(call.error())); client.close(); }
  });
  if (!opened) result.emplace(std::unexpected(opened.error()));
  else uv_run(&loop, UV_RUN_DEFAULT);
  client.close();
  uv_loop_close(&loop);
  if (!result) return std::unexpected(AppError{ErrorCode::result_unknown, "TUI RPC请求没有结果", std::nullopt, std::nullopt});
  return std::move(*result);
}

class TuiApp final : public ComponentBase {
 public:
  TuiApp(std::string socket, std::string profile, ScreenInteractive& screen)
      : socket_(std::move(socket)), profile_id_(std::move(profile)), screen_(&screen) {
    if (profile_id_.empty()) {
      auto profiles = rpc(socket_, std::string(methods::profile_list));
      if (profiles && profiles->is_array() && !profiles->empty()) profile_id_ = profiles->front().value("id", "");
    }
    refresh_all();
  }

  bool OnEvent(Event event) override {
    if (editing_) {
      if (event == Event::Escape || event == Event::CtrlQ) { editing_ = false; return true; }
      if (event == Event::CtrlS) { save_editor(); return true; }
      if (event == Event::ArrowLeft && edit_cursor_ > 0) --edit_cursor_;
      else if (event == Event::ArrowRight && edit_cursor_ < draft_.size()) ++edit_cursor_;
      else if (event == Event::Backspace && edit_cursor_ > 0) {
        auto erase_from = edit_cursor_ - 1;
        while (erase_from > 0 &&
               (static_cast<unsigned char>(draft_[erase_from]) & 0xc0) == 0x80) --erase_from;
        draft_.erase(erase_from, edit_cursor_ - erase_from);
        edit_cursor_ = erase_from;
      } else if (event == Event::Return) {
        draft_.insert(edit_cursor_++, "\n");
      } else if (event.is_character()) {
        const auto value = event.character();
        draft_.insert(edit_cursor_, value);
        edit_cursor_ += value.size();
      }
      return true;
    }
    if (node_detail_) {
      if (event == Event::Escape || event == Event::CtrlQ) node_detail_ = false;
      return true;
    }
    if (event == Event::Character("q") || event == Event::Escape) {
      screen_->ExitLoopClosure()();
      return true;
    }
    if (event == Event::Character("1")) page_ = 0;
    else if (event == Event::Character("2")) { page_ = 1; refresh_nodes(); }
    else if (event == Event::Character("3")) { page_ = 2; refresh_modules(); }
    else if (event == Event::Character("4")) { page_ = 3; refresh_logs(); }
    else if (event == Event::ArrowDown) selected_ = std::min(selected_ + 1, page_size() ? page_size() - 1 : 0);
    else if (event == Event::ArrowUp) selected_ = selected_ == 0 ? 0 : selected_ - 1;
    else if (event == Event::Return && page_ == 1) node_detail_ = true;
    else if (event == Event::Return && page_ == 2) open_editor();
    else if (event == Event::Character("e") && page_ == 2) toggle_module();
    else if (event == Event::Character("s") && page_ == 1) select_node();
    else if (event == Event::Character("c") && !profile_id_.empty()) connect();
    else if (event == Event::Character("d")) disconnect();
    else if (event == Event::Character("r")) refresh_all();
    return true;
  }

  Element OnRender() override {
    Elements header{
      text(" XRM  ") | bold,
      text("1 Overview   2 Nodes   3 Native Modules   4 Logs") | color(Color::Cyan),
      filler(),
      text(profile_id_.empty() ? "no Profile" : "Profile " + profile_id_) | dim,
    };
    Elements body;
    if (editing_) {
      body.push_back(text("Native module editor — " + editing_key_ + " (Ctrl-S save, Esc discard)") | bold);
      auto visible = draft_;
      visible.insert(edit_cursor_, "█");
      body.push_back(paragraph(visible) | border | flex);
    } else if (page_ == 0) {
      body.push_back(text("Overview") | bold);
      body.push_back(text("Runtime: " + status_.value("state", "unknown") +
                          "    Health: " + status_.value("health", "unknown")));
      body.push_back(text("Active Profile: " + active_profile()));
      body.push_back(text("Runtime files: " + active_confdir()));
      body.push_back(text("Expected selection: " + expected_selection()));
      body.push_back(text("Actual selection: " + actual_selection()));
      body.push_back(text("Actions: c connect saved Profile    d stop    r refresh    q exit (core continues)") | dim);
    } else if (page_ == 1) {
      if (node_detail_ && selected_ < nodes_.size()) {
        const auto& node = nodes_[selected_];
        body.push_back(text("Node details (Esc returns)") | bold);
        body.push_back(text("Name: " + node.value("name", "")));
        body.push_back(text("Protocol: " + node.value("protocol", "")));
        body.push_back(text("ID: " + node.value("id", "")));
        body.push_back(text(std::string("Enabled: ") + (node.value("enabled", false) ? "yes" : "no")));
      } else {
        body.push_back(text("Nodes — Enter details; s saves selection, c connects") | bold);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
          const auto& node = nodes_[i];
          const auto row = node.value("name", "") + "   " + node.value("protocol", "") + "   " + node.value("id", "");
          body.push_back(text((i == selected_ ? "> " : "  ") + row) | (i == selected_ ? inverted : nothing));
        }
      }
    } else if (page_ == 2) {
      body.push_back(text("Native Modules — Enter view/edit, e enable/disable. Source text is saved separately.") | bold);
      for (std::size_t i = 0; i < modules_.size(); ++i) {
        const auto& module = modules_[i];
        const auto row = module.value("key", "") + " [" + (module.value("included", false) ? "enabled" : "disabled") +
            "]  source:" + module.value("source_file", "") + "  runtime:" + module.value("runtime_file", "");
        body.push_back(text((i == selected_ ? "> " : "  ") + row) | (i == selected_ ? inverted : nothing));
      }
    } else {
      body.push_back(text("Logs — bounded cursor " + std::to_string(logs_.value("next_cursor", 0))) | bold);
      if (logs_.contains("entries")) for (const auto& entry : logs_["entries"])
        body.push_back(paragraph("[" + std::to_string(entry.value("cursor", 0)) + "][" +
            entry.value("stream", "") + "] " + entry.value("text", "")));
      body.push_back(text("r refresh from the server") | dim);
    }
    if (!message_.empty()) body.push_back(text(message_) | color(Color::Yellow));
    return vbox({hbox(std::move(header)), separator(), vbox(std::move(body)) | flex,
                 separator(), text("q quit   ↑/↓ move   c connect   d stop   r refresh   Tabs: 1–4") | dim}) | border;
  }

 private:
  std::size_t page_size() const {
    if (page_ == 1) return nodes_.size();
    if (page_ == 2) return modules_.size();
    return 0;
  }
  void refresh_all() { refresh_status(); refresh_expected(); refresh_nodes(); refresh_modules(); refresh_logs(); }
  void refresh_expected() {
    expected_selection_.clear();
    if (profile_id_.empty()) return;
    auto profile = rpc(socket_, std::string(methods::profile_get), Json{{"profile_id", profile_id_}});
    if (profile && profile->contains("selected") && profile->at("selected").is_object())
      expected_selection_ = profile->at("selected").value("id_or_tag", "");
  }
  void refresh_status() {
    auto response = rpc(socket_, std::string(methods::runtime_status));
    if (response) status_ = std::move(*response); else message_ = response.error().message;
  }
  void refresh_nodes() {
    auto response = rpc(socket_, std::string(methods::node_list), Json{{"offset", 0}, {"limit", 100}});
    nodes_.clear();
    if (response && response->contains("items")) nodes_ = response->at("items").get<std::vector<Json>>();
  }
  void refresh_modules() {
    if (profile_id_.empty()) return;
    auto response = rpc(socket_, std::string(methods::module_list), Json{{"profile_id", profile_id_}});
    modules_.clear();
    if (response && response->is_array()) modules_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, modules_.empty() ? 0 : modules_.size() - 1);
  }
  void refresh_logs() {
    auto response = rpc(socket_, std::string(methods::logs_read), Json{{"cursor", 0}, {"limit", 65536}});
    if (response) logs_ = std::move(*response);
  }
  void toggle_module() {
    if (selected_ >= modules_.size()) return;
    auto& module = modules_[selected_];
    auto result = rpc(socket_, std::string(methods::module_include), Json{
        {"profile_id", profile_id_}, {"module", module.value("key", "")},
        {"included", !module.value("included", false)}});
    if (!result) message_ = result.error().message;
    else { message_ = "saved; runtime changes on the next explicit apply/connect"; refresh_modules(); }
  }
  void open_editor() {
    if (selected_ >= modules_.size()) return;
    editing_key_ = modules_[selected_].value("key", "");
    auto result = rpc(socket_, std::string(methods::module_get),
                      Json{{"profile_id", profile_id_}, {"module", editing_key_}});
    if (!result) { message_ = result.error().message; return; }
    draft_ = result->value("text", std::string("{}\n"));
    edit_cursor_ = draft_.size();
    editing_ = true;
  }
  void save_editor() {
    auto result = rpc(socket_, std::string(methods::module_save), Json{
        {"profile_id", profile_id_}, {"module", editing_key_}, {"text", draft_}});
    if (!result) message_ = result.error().message;
    else message_ = "saved source text; not applied to running core";
    editing_ = false;
    refresh_modules();
  }
  void select_node() {
    if (nodes_.empty() || selected_ >= nodes_.size() || profile_id_.empty()) return;
    auto result = rpc(socket_, std::string(methods::connection_select), Json{
        {"profile_id", profile_id_}, {"target", Json{{"kind", "node"},
            {"id_or_tag", nodes_[selected_].value("id", "")}}}});
    if (!result) message_ = result.error().message; else { message_ = "selection saved; core unchanged"; refresh_expected(); refresh_status(); }
  }
  void connect() {
    auto result = rpc(socket_, std::string(methods::connection_connect), Json{{"profile_id", profile_id_}});
    if (!result) { message_ = result.error().message; return; }
    const auto operation = result->value("operation_id", "");
    for (int attempt = 0; attempt < 100 && !operation.empty(); ++attempt) {
      auto state = rpc(socket_, std::string(methods::operation_get), Json{{"operation_id", operation}});
      if (!state) { message_ = state.error().message; break; }
      if (state->value("status", "") == "succeeded") { message_ = "connected"; break; }
      if (state->value("status", "") == "failed") { message_ = state->at("error").value("message", "connection failed"); break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    refresh_status();
  }
  void disconnect() {
    auto result = rpc(socket_, std::string(methods::connection_disconnect));
    if (!result) { message_ = result.error().message; return; }
    const auto operation = result->value("operation_id", "");
    for (int attempt = 0; attempt < 100 && !operation.empty(); ++attempt) {
      auto state = rpc(socket_, std::string(methods::operation_get), Json{{"operation_id", operation}});
      if (state && state->value("status", "") != "running") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    message_ = "core stopped";
    refresh_status();
  }
  std::string active_profile() const {
    return status_.contains("active") && status_["active"].is_object()
        ? status_["active"].value("profile_id", "") : "—";
  }
  std::string active_confdir() const {
    return status_.contains("active") && status_["active"].is_object()
        ? status_["active"].value("confdir", "") : "—";
  }
  std::string actual_selection() const {
    if (!status_.contains("active") || !status_["active"].is_object() ||
        status_["active"]["selection"].is_null()) return "none";
    return status_["active"]["selection"].value("id_or_tag", "");
  }
  std::string expected_selection() const {
    return expected_selection_.empty() ? "none" : expected_selection_;
  }

  std::string socket_;
  std::string profile_id_;
  ScreenInteractive* screen_;
  int page_ = 0;
  std::size_t selected_ = 0;
  bool editing_ = false;
  bool node_detail_ = false;
  std::string editing_key_;
  std::string draft_;
  std::size_t edit_cursor_ = 0;
  std::vector<Json> nodes_;
  std::vector<Json> modules_;
  Json status_ = Json::object();
  Json logs_ = Json::object();
  std::string message_;
  std::string expected_selection_;
};

}  // namespace

int run_tui(const std::string& socket, const std::string& profile_id) {
  ScreenInteractive screen = ScreenInteractive::Fullscreen();
  auto app = Make<TuiApp>(socket, profile_id, screen);
  screen.Loop(app);
  return 0;
}

}  // namespace xrm::cli

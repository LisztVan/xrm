#include "cli/commands.hpp"
#include "cli/rpc_session.hpp"

#include "client/rpc_client.hpp"
#include "contract/methods.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <uv.h>
#include <sys/utsname.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <spawn.h>
#include <system_error>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace xrm::cli {
namespace {
using namespace ftxui;
constexpr std::size_t kLogViewportRows = 9;
constexpr std::size_t kTuiLogByteLimit = 256 * 1024;
constexpr std::size_t kTuiLogEntryLimit = 512;

class DraftFile {
 public:
  DraftFile() {
    std::error_code error;
    auto pattern = (std::filesystem::temp_directory_path(error) / "xrm-module-XXXXXX").string();
    if (error) return;
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int fd = ::mkstemp(writable.data());
    if (fd < 0) return;
    path_ = writable.data();
    ::close(fd);
  }
  ~DraftFile() { if (!path_.empty()) ::unlink(path_.c_str()); }
  bool valid() const { return !path_.empty(); }
  const std::string& path() const { return path_; }
  bool write(std::string_view content) const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(output);
  }
  std::optional<std::string> read() const {
    std::ifstream input(path_, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
 private:
  std::string path_;
};

std::optional<std::vector<std::string>> editor_argv(std::string_view command) {
  std::istringstream input{std::string(command)};
  std::vector<std::string> args;
  std::string arg;
  while (input >> arg) {
    if (arg.find_first_of("\"'") != std::string::npos) return std::nullopt;
    args.push_back(std::move(arg));
  }
  if (args.empty()) return std::nullopt;
  return args;
}

std::string editor_command() {
  if (const char* visual = std::getenv("VISUAL"); visual && *visual) return visual;
  if (const char* editor = std::getenv("EDITOR"); editor && *editor) return editor;
  return {};
}

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
    log_poller_ = std::thread([this] {
      while (!log_poller_stop_.load()) {
        if (log_poll_active_.load()) screen_->PostEvent(Event::Custom);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });
  }
  ~TuiApp() override {
    log_poller_stop_.store(true);
    if (log_poller_.joinable()) log_poller_.join();
    if (stats_watching_) refresh_stats(false);
  }

  bool OnEvent(Event event) override {
    if (event == Event::Custom) {
      if (page_ == 3) refresh_logs();
      return true;
    }
    if (filtering_nodes_) {
      if (event == Event::Escape) { filtering_nodes_ = false; return true; }
      if (event == Event::Return) { filtering_nodes_ = false; selected_ = 0; refresh_nodes(); return true; }
      if (event == Event::Backspace && !node_filter_.empty()) {
        auto erase_from = node_filter_.size() - 1;
        while (erase_from > 0 && (static_cast<unsigned char>(node_filter_[erase_from]) & 0xc0) == 0x80) --erase_from;
        node_filter_.erase(erase_from);
      }
      else if (event.is_character()) node_filter_ += event.character();
      return true;
    }
    if (editing_) {
      if (event == Event::Escape || event == Event::CtrlQ) {
        editing_ = false; editing_group_ = false; editing_node_ = false; return true;
      }
      if (event == Event::CtrlS) { save_editor(); return true; }
      if (event == Event::Character("v") && !editing_node_ && !editing_group_) {
        open_external_editor();
        return true;
      }
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
      if (!editing_node_ && !editing_group_ && editing_base_text_)
        editing_dirty_ = draft_ != *editing_base_text_;
      return true;
    }
    if (node_detail_) {
      if (event == Event::Escape || event == Event::CtrlQ) node_detail_ = false;
      else if (event == Event::Character("t"))
        node_field_index_ = (node_field_index_ + 1) % node_edit_fields_.size();
      else if (event == Event::Character("e")) open_node_editor();
      return true;
    }
    if (show_help_) {
      if (event == Event::Escape || event == Event::CtrlQ || event == Event::Character("?"))
        show_help_ = false;
      return true;
    }
    if (event == Event::Character("q") || event == Event::Escape) {
      stop_stats_watch();
      screen_->ExitLoopClosure()();
      return true;
    }
    if (event == Event::Character("?")) { show_help_ = true; return true; }
    if (event == Event::Tab) { navigate_page((page_ + 1) % 8); return true; }
    if (event == Event::TabReverse) { navigate_page((page_ + 7) % 8); return true; }
    if (event == Event::Character("1")) change_page(0);
    else if (event == Event::Character("2")) { change_page(1); refresh_nodes(); }
    else if (event == Event::Character("3")) { change_page(2); refresh_modules(); }
    else if (event == Event::Character("4")) { change_page(3); refresh_logs(); }
    else if (event == Event::Character("5")) { change_page(4); refresh_subscriptions(); }
    else if (event == Event::Character("6")) { change_page(5); refresh_groups(); }
    else if (event == Event::Character("7")) { change_page(6); refresh_stats(true); }
    else if (event == Event::Character("8")) { change_page(7); refresh_cores(); }
    else if (event == Event::ArrowDown && page_ == 3) {
      const auto tail = log_entries_.size() > kLogViewportRows ? log_entries_.size() - kLogViewportRows : 0;
      if (log_view_start_ < tail) ++log_view_start_;
    }
    else if (event == Event::ArrowUp && page_ == 3) {
      if (log_view_start_ > 0) --log_view_start_;
    }
    else if (event == Event::ArrowDown) selected_ = std::min(selected_ + 1, page_size() ? page_size() - 1 : 0);
    else if (event == Event::ArrowUp) selected_ = selected_ == 0 ? 0 : selected_ - 1;
    else if (event == Event::Return && page_ == 1) node_detail_ = true;
    else if (event == Event::Return && page_ == 2) open_editor();
    else if (event == Event::Return && page_ == 5) open_group_editor();
    else if (event == Event::Character("e") && page_ == 2) toggle_module();
    else if (event == Event::Character("s") && page_ == 1) select_node();
    else if (event == Event::Character("s") && page_ == 5) select_group();
    else if (event == Event::Character("m") && page_ == 5) choose_group_member();
    else if (event == Event::Character("m") && page_ == 1) select_group_member();
    else if (event == Event::Character("c") && !profile_id_.empty()) connect();
    else if (event == Event::Character("d")) disconnect();
    else if (event == Event::Character("r")) {
      if (page_ == 7) {
        if (!pending_core_operation_.empty()) poll_core_operation();
        else refresh_cores();
      } else if (page_ == 3) refresh_logs();
      else refresh_all();
      if (page_ == 6) refresh_stats(true);
    }
    else if (event == Event::Character("p") && page_ == 3) {
      logs_paused_ = !logs_paused_;
      if (!logs_paused_) {
        refresh_logs();
        scroll_logs_to_tail();
        message_ = "log follow resumed";
      } else message_ = "log view paused; Server continues collecting logs";
    }
    else if (event == Event::Character("/" ) && page_ == 1) {
      node_filter_.clear();
      filtering_nodes_ = true;
    }
    else if (event == Event::Character("u") && page_ == 4) refresh_subscription_inventory();
    else if (event == Event::Character("l") && page_ == 7) fetch_core_releases(false);
    else if (event == Event::Character("p") && page_ == 7) fetch_core_releases(true);
    else if (event == Event::Character("i") && page_ == 7) install_selected_release();
    else if (event == Event::Character("u") && page_ == 7) use_selected_core();
    return true;
  }

  Element OnRender() override {
    Elements header{
      text(" XRM  ") | bold,
      text("1 Overview   2 Nodes   3 Native Modules   4 Logs   5 Subscriptions   6 Groups   7 Stats   8 Cores") | color(Color::Cyan),
      filler(),
      text(profile_id_.empty() ? "no Profile" : "Profile " + profile_id_) | dim,
    };
    Elements body;
    if (editing_) {
      body.push_back(text("Native module editor — " + editing_key_ +
          (editing_dirty_ ? " [unsaved]" : "") +
          (editing_node_ || editing_group_ ? " (Ctrl-S Save, Esc close)" : " (Ctrl-S Save, v external editor, Esc close)")) | bold);
      if (draft_json_invalid_)
        body.push_back(text("Draft is not valid strict JSON; source can be saved, but form/runtime checks may reject it") | color(Color::Yellow));
      auto visible = draft_;
      visible.insert(edit_cursor_, "█");
      body.push_back(paragraph(visible) | border | flex);
    } else if (page_ == 0) {
      body.push_back(text("Overview") | bold);
      body.push_back(text("Runtime: " + status_.value("state", "unknown") +
                          "    Health: " + status_.value("health", "unknown")));
      body.push_back(text("Active Profile: " + active_profile()));
      body.push_back(text("Runtime files: " + active_confdir()));
      body.push_back(text("Saved target: " + expected_selection()));
      body.push_back(text("Applied target: " + actual_selection()));
      body.push_back(text("Runtime override: " + status_.value("override_state", "unknown") +
          (status_.contains("runtime_override") && status_["runtime_override"].is_string()
              ? " " + status_["runtime_override"].get<std::string>() : "")));
      body.push_back(text("Actions: c connect saved Profile    d stop    r refresh    q exit (core continues)") | dim);
    } else if (page_ == 1) {
      if (node_detail_ && selected_ < nodes_.size()) {
        const auto& node = nodes_[selected_];
        body.push_back(text("Node details (Esc returns)") | bold);
        body.push_back(text("Name: " + node.value("name", "")));
        body.push_back(text("Protocol: " + node.value("protocol", "")));
        body.push_back(text("XRM support: " + node.value("support", "raw-only")));
        body.push_back(text("ID: " + node.value("id", "")));
        body.push_back(text(std::string("Enabled: ") + (node.value("enabled", false) ? "yes" : "no")));
        body.push_back(text("Native subobject: " + node_edit_fields_[node_field_index_] +
                            " (t changes field, e edits JSON, Ctrl-S saves)") | dim);
      } else {
        body.push_back(text("Nodes — Enter details; / filter; s saves selection, c connects") | bold);
        body.push_back(text("filter: " + node_filter_) | dim);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
          const auto& node = nodes_[i];
          const auto row = node.value("name", "") + "   " + node.value("protocol", "") + " [" +
              node.value("support", "raw-only") + "]   " +
              (node.value("source_missing", false) ? "source_missing" : "available") + "   " + node.value("id", "");
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
    } else if (page_ == 3) {
      const auto mode = logs_paused_ ? "PAUSED" : "FOLLOW";
      const auto end = std::min(log_entries_.size(), log_view_start_ + kLogViewportRows);
      const std::string top = log_view_start_ < log_entries_.size()
          ? log_entries_[log_view_start_].value("text", "") : "(empty)";
      body.push_back(text("Logs — " + std::string(mode) + " cursor " + std::to_string(log_cursor_)) | bold);
      body.push_back(text("Viewport top: " + top.substr(0, 72) + "  rows " +
          std::to_string(log_view_start_ + (log_entries_.empty() ? 0 : 1)) + "-" +
          std::to_string(end) + "/" + std::to_string(log_entries_.size())) | dim);
      if (log_dropped_ || logs_.value("truncated", false))
        body.push_back(text("older logs dropped / truncated") | color(Color::Yellow));
      if (logs_paused_ && log_view_start_ < log_entries_.size() &&
          log_entries_.size() > log_view_start_ + kLogViewportRows)
        body.push_back(text("new logs: " + std::to_string(log_entries_.size() - log_view_start_ - kLogViewportRows)) | dim);
      for (std::size_t index = log_view_start_; index < end; ++index) {
        const auto& entry = log_entries_[index];
        body.push_back(paragraph("[" + std::to_string(entry.value("cursor", 0)) + "][" +
            entry.value("stream", "") + "] " + entry.value("text", "")));
      }
      body.push_back(text("p " + std::string(logs_paused_ ? "Resume" : "Pause") +
          "   ↑/↓ history   r refresh   Server buffer remains active") | dim);
    } else if (page_ == 4) {
      body.push_back(text("Subscriptions — u refresh inventory only; runtime is unchanged") | bold);
      for (std::size_t i = 0; i < subscriptions_.size(); ++i) {
        const auto& source = subscriptions_[i];
        const auto row = source.value("name", "") + " [" + source.value("last_result", "never") + "] " +
            (source.value("enabled", false) ? "enabled" : "paused") + " nodes:" +
            std::to_string(source.value("node_count", 0)) + " unsupported:" +
            std::to_string(source.value("unsupported_count", 0)) + " malformed:" +
            std::to_string(source.value("malformed_count", 0));
        body.push_back(text((i == selected_ ? "> " : "  ") + row) | (i == selected_ ? inverted : nothing));
      }
      body.push_back(text("u updates subscriptions; no connect, config render or core restart") | dim);
    } else if (page_ == 6) {
      body.push_back(text("Stats — shared server sample; no client-side rate calculation") | bold);
      body.push_back(text("Core: " + status_.value("state", "unknown")));
      body.push_back(text("Metrics: " + stats_.value("availability", "unknown")));
      if (stats_.value("availability", "unknown") != "available") {
        const auto missing_key = stats_.contains("missing_conditions") ? "missing_conditions" : "missing";
        std::string missing = "Unavailable conditions: ";
        if (stats_.contains(missing_key) && stats_[missing_key].is_array()) {
          bool first = true;
          for (const auto& condition : stats_[missing_key]) if (condition.is_string()) {
            if (!first) missing += ", ";
            missing += condition.get<std::string>();
            first = false;
          }
        } else if (stats_.contains("reason") && stats_["reason"].is_string()) {
          missing += stats_["reason"].get<std::string>();
        } else missing += "no sample";
        body.push_back(text(missing));
      }
      if (!stats_.contains("sample") || !stats_["sample"].is_object()) {
        body.push_back(text("Cumulative traffic: unavailable"));
        body.push_back(text("Current rates: unavailable"));
        body.push_back(text("Observatory latency: unavailable"));
      } else {
        const auto& sample = stats_["sample"];
        body.push_back(text("Sample timestamp: " +
            (sample.contains("received_at") && !sample["received_at"].is_null()
                ? sample["received_at"].dump() : "unavailable")));
        if (sample.contains("traffic") && sample["traffic"].is_array() && !sample["traffic"].empty()) {
          body.push_back(text("Cumulative traffic") | bold);
          for (const auto& counter : sample["traffic"])
            body.push_back(text(counter.value("scope", "") + "/" + counter.value("tag", "") +
                "  up=" + (counter.contains("uplink") && !counter["uplink"].is_null()
                    ? counter["uplink"].dump() + " B" : "unavailable") +
                "  down=" + (counter.contains("downlink") && !counter["downlink"].is_null()
                    ? counter["downlink"].dump() + " B" : "unavailable")));
        } else body.push_back(text("Cumulative traffic: unavailable"));
        if (stats_.contains("rates") && stats_["rates"].is_array() && !stats_["rates"].empty()) {
          body.push_back(text("Current rates") | bold);
          for (const auto& rate : stats_["rates"])
            body.push_back(text(rate.value("scope", "") + "/" + rate.value("tag", "") +
                "  up=" + (rate.contains("uplink_bytes_per_second") && !rate["uplink_bytes_per_second"].is_null()
                    ? rate["uplink_bytes_per_second"].dump() + " B/s" : "pending") +
                "  down=" + (rate.contains("downlink_bytes_per_second") && !rate["downlink_bytes_per_second"].is_null()
                    ? rate["downlink_bytes_per_second"].dump() + " B/s" : "pending")));
        } else body.push_back(text("Current rates: pending second sample"));
        if (sample.contains("observations") && sample["observations"].is_array() && !sample["observations"].empty()) {
          body.push_back(text("Observatory latency") | bold);
          for (const auto& observation : sample["observations"])
            body.push_back(text(observation.value("tag", "") + "  " +
                (observation.contains("latency_ms") && !observation["latency_ms"].is_null()
                    ? observation["latency_ms"].dump() + " ms" : "unavailable")));
        } else body.push_back(text("Observatory latency: unavailable"));
      }
      body.push_back(text("r refreshes the shared server sample") | dim);
    } else if (page_ == 5) {
      body.push_back(text("Groups — Enter edits native binding JSON; s saves as target; m choose member via Nodes") | bold);
      for (std::size_t i = 0; i < groups_.size(); ++i) {
        const auto& group = groups_[i];
        const auto selected_node = group.value("selected_node_id", Json(nullptr));
        const auto row = group.value("name", "") + " [" + group.value("selection_mode", "automatic") + "] tag:" +
            group.value("balancer", Json::object()).value("tag", "") + " selected:" +
            (selected_node.is_string() ? selected_node.get<std::string>() : "—");
        body.push_back(text((i == selected_ ? "> " : "  ") + row) | (i == selected_ ? inverted : nothing));
        if (i == selected_) body.push_back(text("selector: " + group.value("members", Json::object()).dump()) | dim);
      }
      body.push_back(text("Group selection is persistent; runtime override is separate") | dim);
    } else {
      body.push_back(text("Cores — system binaries are register-only; managed binaries live under XRM data") | bold);
      if (viewing_releases_) {
        body.push_back(text(std::string("Official XTLS/Xray-core releases ") +
            (include_prerelease_ ? "(stable + prerelease)" : "(stable)")) | dim);
        for (std::size_t i = 0; i < core_releases_.size(); ++i) {
          const auto& release = core_releases_[i];
          body.push_back(text((i == selected_ ? "> " : "  ") + release.value("tag", "") +
              "  published:" + std::to_string(release.value("published_at", std::int64_t{0})) +
              (release.value("prerelease", false) ? "  prerelease" : "  stable")) |
              (i == selected_ ? inverted : nothing));
          if (i == selected_ && release.contains("assets") && release["assets"].is_array())
            for (const auto& asset : release["assets"])
              body.push_back(text("    " + asset.value("name", "") + "  " +
                  asset.value("platform", "") + "/" + asset.value("architecture", "") +
                  (asset.contains("sha256") && asset["sha256"].is_string()
                      ? "  sha256" : "  no checksum")) | dim);
        }
        body.push_back(text("l stable releases   p include prereleases   i install selected release") | dim);
      } else {
        const auto running = status_.contains("active") && status_["active"].is_object()
            ? status_["active"].value("core_id", "") : "";
        auto profiles = rpc(socket_, std::string(methods::profile_list));
        for (std::size_t i = 0; i < cores_.size(); ++i) {
          const auto& core = cores_[i];
          bool selected = false;
          if (profiles && profiles->is_array()) for (const auto& profile : *profiles)
            selected = selected || profile.value("core_id", "") == core.value("id", "");
          body.push_back(text((i == selected_ ? "> " : "  ") + core.value("name", "") + "  " +
              core.value("reported_version", "") + "  " +
              (core.value("managed", false) ? "managed" : "system") + "  installed" +
              (selected ? " selected" : "") + (running == core.value("id", "") ? " running" : "")) |
              (i == selected_ ? inverted : nothing));
          body.push_back(text(core.value("path", "")) | dim);
        }
        body.push_back(text("l releases   u use selected core for current Profile") | dim);
      }
      body.push_back(text("r refresh/poll operation; install never changes the running core") | dim);
    }
    if (filtering_nodes_) body.push_back(text("Filter nodes: " + node_filter_ + "  Enter apply / Esc cancel") | color(Color::Yellow));
    if (!message_.empty()) body.push_back(text(message_) | color(Color::Yellow));
    if (show_help_) {
      return vbox({hbox(std::move(header)), separator(),
          vbox({text("Help — keyboard controls") | bold,
                text("Tab / Shift-Tab  change page"),
                text("1–8              open a page directly"),
                text("↑ / ↓            move through a list"),
                text("Enter            view or open the selected item"),
                text("/                filter Nodes; Enter applies, Esc cancels"),
                text("c                connect the saved Profile target"),
                text("d                stop the managed core"),
                text("r                refresh the current page"),
                text("Logs: p pause/resume follow; arrows browse received history"),
                text("Module editor: v open $VISUAL / $EDITOR; Ctrl-S saves source only"),
                text("q                quit this TUI; the core keeps running"),
                text("Esc or ?         close help")}) | flex,
          separator(), text("Help") | dim}) | border;
    }
    return vbox({hbox(std::move(header)), separator(), vbox(std::move(body)) | flex,
                 separator(), text("q quit   ? help   Tab/Shift-Tab pages   ↑/↓ move   c connect   d stop   r refresh   Logs: p pause/resume") | dim}) | border;
  }

 private:
  std::size_t page_size() const {
    if (page_ == 1) return nodes_.size();
    if (page_ == 2) return modules_.size();
    if (page_ == 4) return subscriptions_.size();
    if (page_ == 5) return groups_.size();
    if (page_ == 7) return viewing_releases_ ? core_releases_.size() : cores_.size();
    return 0;
  }
  void refresh_all() { refresh_status(); refresh_expected(); refresh_nodes(); refresh_modules(); refresh_logs(); refresh_subscriptions(); refresh_groups(); refresh_cores(); }
  void change_page(int page) {
    if (page_ == 6 && page != 6) stop_stats_watch();
    page_ = page;
    log_poll_active_.store(page_ == 3);
  }
  void navigate_page(int page) {
    change_page(page);
    if (page_ == 1) refresh_nodes();
    else if (page_ == 2) refresh_modules();
    else if (page_ == 3) refresh_logs();
    else if (page_ == 4) refresh_subscriptions();
    else if (page_ == 5) refresh_groups();
    else if (page_ == 6) refresh_stats(true);
    else if (page_ == 7) refresh_cores();
  }
  void stop_stats_watch() {
    if (stats_watching_) refresh_stats(false);
  }
  void refresh_stats(bool watch) {
    if (watch) {
      if (!stats_rpc_) stats_rpc_ = std::make_unique<RpcSession>(socket_);
      auto response = stats_rpc_->call(std::string(methods::runtime_stats), Json{{"watch", true}});
      if (response) stats_ = std::move(*response);
      else stats_ = Json{{"availability", "unavailable"}, {"reason", response.error().message}};
      stats_watching_ = response.has_value();
      return;
    }
    if (!stats_rpc_) return;
    auto response = stats_rpc_->call(std::string(methods::runtime_stats), Json{{"watch", false}});
    if (response) stats_ = std::move(*response);
    else stats_ = Json{{"availability", "unavailable"}, {"reason", response.error().message}};
    stats_rpc_->close();
    stats_rpc_.reset();
    stats_watching_ = false;
  }
  void refresh_expected() {
    expected_selection_.clear();
    if (profile_id_.empty()) return;
    auto profile = rpc(socket_, std::string(methods::profile_get), Json{{"profile_id", profile_id_}});
    if (profile && profile->contains("selected") && profile->at("selected").is_object()) {
      const auto& target = profile->at("selected");
      expected_selection_ = target.value("kind", "") + ":" + target.value("id_or_tag", "");
    }
  }
  void refresh_status() {
    auto response = rpc(socket_, std::string(methods::runtime_status));
    if (response) status_ = std::move(*response); else message_ = response.error().message;
  }
  void refresh_nodes() {
    std::string selected_id;
    if (selected_ < nodes_.size()) selected_id = nodes_[selected_].value("id", "");
    Json params{{"offset", 0}, {"limit", 100}};
    if (!node_filter_.empty()) params["text"] = node_filter_;
    auto response = rpc(socket_, std::string(methods::node_list), std::move(params));
    nodes_.clear();
    if (response && response->contains("items")) nodes_ = response->at("items").get<std::vector<Json>>();
    if (!selected_id.empty()) {
      auto found = std::find_if(nodes_.begin(), nodes_.end(), [&](const auto& node) { return node.value("id", "") == selected_id; });
      if (found != nodes_.end()) selected_ = static_cast<std::size_t>(found - nodes_.begin());
      else selected_ = std::min(selected_, nodes_.empty() ? 0 : nodes_.size() - 1);
    }
  }
  void refresh_subscriptions() {
    auto response = rpc(socket_, std::string(methods::subscription_list));
    subscriptions_.clear();
    if (response && response->is_array()) subscriptions_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, subscriptions_.empty() ? 0 : subscriptions_.size() - 1);
  }
  void refresh_groups() {
    if (profile_id_.empty()) return;
    auto response = rpc(socket_, std::string(methods::group_list), Json{{"profile_id", profile_id_}});
    groups_.clear();
    if (response && response->is_array()) groups_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, groups_.empty() ? 0 : groups_.size() - 1);
  }
  void refresh_cores() {
    auto response = rpc(socket_, std::string(methods::core_list));
    cores_.clear();
    if (response && response->is_array()) cores_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, cores_.empty() ? 0 : cores_.size() - 1);
    viewing_releases_ = false;
  }
  void poll_core_operation() {
    if (pending_core_operation_.empty()) return;
    auto state = rpc(socket_, std::string(methods::operation_get),
                     Json{{"operation_id", pending_core_operation_}});
    if (!state) { message_ = state.error().message; return; }
    const auto status = state->value("status", "unknown");
    if (status == "running") { message_ = pending_core_label_ + " still running; press r to check"; return; }
    if (status == "failed") {
      message_ = state->contains("error")
          ? state->at("error").value("message", "operation failed") : "operation failed";
    } else if (status == "succeeded") {
      const auto result = state->value("result", Json::object());
      if (pending_core_label_ == "release query") {
        core_releases_ = result.value("items", Json::array()).get<std::vector<Json>>();
        viewing_releases_ = true;
        selected_ = 0;
        message_ = "loaded " + std::to_string(core_releases_.size()) + " official releases";
      } else if (pending_core_label_ == "install") {
        message_ = "installed core; not selected or running";
        refresh_cores();
      } else {
        message_ = "selected core for Profile; saved runtime files were reused";
        refresh_cores();
        refresh_status();
      }
    }
    pending_core_operation_.clear();
    pending_core_label_.clear();
  }
  void start_core_operation(std::string method, Json params, std::string label) {
    if (!pending_core_operation_.empty()) {
      message_ = "core operation already pending; press r to poll";
      return;
    }
    auto accepted = rpc(socket_, std::move(method), std::move(params));
    if (!accepted) { message_ = accepted.error().message; return; }
    pending_core_operation_ = accepted->value("operation_id", "");
    pending_core_label_ = std::move(label);
    if (pending_core_operation_.empty()) { message_ = "server did not return an operation id"; return; }
    message_ = pending_core_label_ + " accepted; press r to poll";
    poll_core_operation();
  }
  void fetch_core_releases(bool include_prerelease) {
    include_prerelease_ = include_prerelease;
    start_core_operation(std::string(methods::core_releases),
        Json{{"include_prerelease", include_prerelease_}}, "release query");
  }
  void install_selected_release() {
    if (!viewing_releases_ || selected_ >= core_releases_.size()) {
      message_ = "load and select an official release first";
      return;
    }
    utsname machine{};
    if (::uname(&machine) != 0) { message_ = "cannot determine host architecture"; return; }
    std::string architecture = machine.machine;
    if (architecture == "amd64") architecture = "x86_64";
    if (architecture == "arm64") architecture = "aarch64";
#if defined(__linux__)
    const std::string platform = "linux";
#elif defined(__APPLE__)
    const std::string platform = "macos";
#elif defined(__FreeBSD__)
    const std::string platform = "freebsd";
#else
    const std::string platform = "unknown";
#endif
    const auto& release = core_releases_[selected_];
    std::string asset_name;
    if (release.contains("assets") && release["assets"].is_array())
      for (const auto& asset : release["assets"])
        if (asset.value("platform", "") == platform && asset.value("architecture", "") == architecture) {
          asset_name = asset.value("name", "");
          break;
        }
    if (asset_name.empty()) {
      message_ = "selected release has no asset for " + platform + "/" + architecture;
      return;
    }
    start_core_operation(std::string(methods::core_install), Json{{"request", Json{
        {"release", release}, {"asset_name", asset_name},
        {"allow_unverified_checksum", false}}}}, "install");
  }
  void use_selected_core() {
    if (viewing_releases_ || selected_ >= cores_.size() || profile_id_.empty()) {
      message_ = "select an installed core from the local list first";
      return;
    }
    start_core_operation(std::string(methods::core_use), Json{
        {"profile_id", profile_id_}, {"core_id", cores_[selected_].value("id", "")}}, "use");
  }
  void refresh_subscription_inventory() {
    auto result = rpc(socket_, std::string(methods::subscription_refresh));
    if (!result) message_ = result.error().message;
    else if (result->value("status", "") == "accepted" && result->contains("operation_id")) {
      const auto operation = result->at("operation_id").get<std::string>();
      message_ = "refresh running; runtime files are unchanged";
      for (int attempt = 0; attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto state = rpc(socket_, std::string(methods::operation_get), Json{{"operation_id", operation}});
        if (!state || state->value("status", "") == "running") continue;
        if (state->value("status", "") == "succeeded") {
          const auto& refresh = state->at("result");
          std::size_t updated = 0, failed = 0;
          if (refresh.contains("completed")) for (const auto& source : refresh["completed"])
            updated += source.value("added", std::size_t{0}) + source.value("updated", std::size_t{0});
          if (refresh.contains("failed")) failed = refresh["failed"].size();
          message_ = "refresh complete: changed nodes=" + std::to_string(updated) +
              " failed sources=" + std::to_string(failed) + "; runtime unchanged";
        } else message_ = "refresh did not complete successfully; runtime unchanged";
        break;
      }
    } else message_ = "refresh accepted; running core and runtime files are not changed";
    refresh_subscriptions();
  }
  void refresh_modules() {
    if (profile_id_.empty()) return;
    auto response = rpc(socket_, std::string(methods::module_list), Json{{"profile_id", profile_id_}});
    modules_.clear();
    if (response && response->is_array()) modules_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, modules_.empty() ? 0 : modules_.size() - 1);
  }
  void refresh_logs() {
    auto response = rpc(socket_, std::string(methods::logs_read), Json{{"cursor", log_cursor_}, {"limit", 65536}});
    if (!response) return;
    if (response->value("truncated", false)) log_dropped_ = true;
    if (response->contains("entries") && response->at("entries").is_array()) {
      for (const auto& entry : response->at("entries")) {
        log_bytes_ += entry.value("text", std::string{}).size();
        log_entries_.push_back(entry);
      }
    }
    log_cursor_ = response->value("next_cursor", log_cursor_);
    logs_ = std::move(*response);
    while (log_entries_.size() > kTuiLogEntryLimit || log_bytes_ > kTuiLogByteLimit) {
      if (log_entries_.empty()) break;
      log_bytes_ -= log_entries_.front().value("text", std::string{}).size();
      log_entries_.erase(log_entries_.begin());
      if (log_view_start_ > 0) --log_view_start_;
      log_dropped_ = true;
    }
    if (!logs_paused_) scroll_logs_to_tail();
  }
  void scroll_logs_to_tail() {
    log_view_start_ = log_entries_.size() > kLogViewportRows
        ? log_entries_.size() - kLogViewportRows : 0;
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
    editing_group_ = false;
    editing_node_ = false;
    auto result = rpc(socket_, std::string(methods::module_get),
                      Json{{"profile_id", profile_id_}, {"module", editing_key_}});
    if (!result) { message_ = result.error().message; return; }
    draft_ = result->value("text", std::string("{}\n"));
    editing_base_text_ = result->contains("text") && result->at("text").is_string()
        ? std::optional<std::string>(result->at("text").get<std::string>()) : std::nullopt;
    edit_cursor_ = draft_.size();
    editing_dirty_ = false;
    draft_json_invalid_ = false;
    editing_ = true;
  }
  void open_external_editor() {
    const auto command = editor_command();
    if (command.empty()) { message_ = "set VISUAL or EDITOR to open an external editor"; return; }
    auto args = editor_argv(command);
    if (!args) {
      message_ = "editor command must be an executable followed by simple unquoted arguments";
      return;
    }
    DraftFile temporary;
    if (!temporary.valid() || !temporary.write(draft_)) {
      message_ = "cannot create temporary module draft";
      return;
    }
    args->push_back(temporary.path());
    std::vector<char*> argv;
    argv.reserve(args->size() + 1);
    for (auto& arg : *args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    int spawn_result = 0;
    int wait_status = 0;
    auto invoke = [&] {
      spawn_result = ::posix_spawnp(&pid, argv.front(), nullptr, nullptr, argv.data(), environ);
      if (spawn_result != 0) return;
      pid_t waited;
      do { waited = ::waitpid(pid, &wait_status, 0); } while (waited < 0 && errno == EINTR);
      if (waited < 0) spawn_result = errno;
    };
    screen_->WithRestoredIO(invoke)();
    if (spawn_result != 0) {
      message_ = "external editor failed to start/wait: " + std::string(std::strerror(spawn_result));
      return;
    }
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
      message_ = (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 130)
          ? "external editor cancelled; draft kept"
          : "external editor failed; draft kept";
      return;
    }
    auto edited = temporary.read();
    if (!edited) { message_ = "cannot read external editor draft; current draft kept"; return; }
    if (*edited == draft_) { message_ = "external editor made no changes; draft kept"; return; }
    draft_ = std::move(*edited);
    edit_cursor_ = draft_.size();
    editing_dirty_ = !editing_base_text_ || draft_ != *editing_base_text_;
    try { const auto parsed = Json::parse(draft_); (void)parsed; draft_json_invalid_ = false; }
    catch (const Json::exception&) { draft_json_invalid_ = true; }
    message_ = draft_json_invalid_
        ? "external draft loaded; invalid JSON retained, Save is explicit"
        : "external draft loaded; unsaved until Ctrl-S";
  }
  void open_node_editor() {
    if (selected_ >= nodes_.size()) return;
    const auto id = nodes_[selected_].value("id", "");
    auto current = rpc(socket_, std::string(methods::node_get), Json{{"id", id}});
    if (!current) { message_ = current.error().message; return; }
    if (!current->contains("settings") || !current->at("settings").is_object() ||
        current->at("settings").value("kind", "") != "stream") {
      message_ = "raw-only native outbounds use the native editor";
      return;
    }
    editing_node_field_ = node_edit_fields_[node_field_index_];
    if (!current->at("settings").contains(editing_node_field_)) {
      message_ = "node subobject is unavailable";
      return;
    }
    editing_node_id_ = id;
    editing_key_ = "Node native editor — " + editing_node_field_;
    draft_ = current->at("settings").at(editing_node_field_).dump(2) + "\n";
    edit_cursor_ = draft_.size();
    editing_node_ = true;
    editing_group_ = false;
    editing_ = true;
  }
  void save_editor() {
    if (editing_node_) {
      Result<Json> saved = std::unexpected(AppError{ErrorCode::invalid_argument,
          "节点子对象必须是合法JSON", std::nullopt, std::nullopt});
      try {
        saved = rpc(socket_, std::string(methods::node_form_edit), Json{
            {"id", editing_node_id_}, {"field", editing_node_field_},
            {"value", Json::parse(draft_)}});
      } catch (const Json::exception&) {}
      if (!saved) message_ = saved.error().message;
      else message_ = "saved node " + editing_node_field_ + "; connect to apply";
      editing_ = false;
      editing_node_ = false;
      refresh_nodes();
      return;
    }
    if (editing_group_) {
      Result<Json> saved = std::unexpected(AppError{ErrorCode::invalid_argument, "组内容必须是JSON对象", std::nullopt, std::nullopt});
      try {
        auto document = Json::parse(draft_);
        saved = rpc(socket_, std::string(methods::group_save), Json{{"profile_id", profile_id_}, {"group", document}});
      } catch (const Json::exception&) {}
      if (!saved) message_ = saved.error().message;
      else message_ = "group saved; runtime changes on next explicit connect";
      editing_ = false;
      editing_group_ = false;
      refresh_groups();
      return;
    }
    auto result = rpc(socket_, std::string(methods::module_save), Json{
        {"profile_id", profile_id_}, {"module", editing_key_}, {"text", draft_},
        {"expected_text", editing_base_text_ ? Json(*editing_base_text_) : Json(nullptr)}});
    if (!result) { message_ = result.error().message + "; draft retained"; return; }
    message_ = "saved source text; not applied to running core";
    editing_ = false;
    editing_dirty_ = false;
    editing_base_text_ = draft_;
    refresh_modules();
  }
  void select_node() {
    if (nodes_.empty() || selected_ >= nodes_.size() || profile_id_.empty()) return;
    auto result = rpc(socket_, std::string(methods::connection_select), Json{
        {"profile_id", profile_id_}, {"target", Json{{"kind", "node"},
            {"id_or_tag", nodes_[selected_].value("id", "")}}}});
    if (!result) message_ = result.error().message; else { message_ = "selection saved; core unchanged"; refresh_expected(); refresh_status(); }
  }
  void open_group_editor() {
    if (selected_ >= groups_.size()) return;
    draft_ = groups_[selected_].dump(2) + "\n";
    edit_cursor_ = draft_.size();
    editing_group_ = true;
    editing_node_ = false;
    editing_key_ = "group " + groups_[selected_].value("name", "");
    editing_ = true;
  }
  void select_group() {
    if (selected_ >= groups_.size()) return;
    auto result = rpc(socket_, std::string(methods::connection_select), Json{
        {"profile_id", profile_id_}, {"target", Json{{"kind", "group"},
        {"id_or_tag", groups_[selected_].value("id", "")}}}});
    if (!result) message_ = result.error().message;
    else { message_ = "group selection saved; core unchanged"; refresh_expected(); refresh_status(); }
  }
  void choose_group_member() {
    if (selected_ >= groups_.size()) return;
    selected_group_id_ = groups_[selected_].value("id", "");
    page_ = 1;
    refresh_nodes();
    message_ = "choose a candidate on Nodes and press m";
  }
  void select_group_member() {
    if (selected_group_id_.empty() || selected_ >= nodes_.size()) return;
    auto result = rpc(socket_, std::string(methods::group_select), Json{
        {"profile_id", profile_id_}, {"group_id", selected_group_id_},
        {"node_id", nodes_[selected_].value("id", "")}});
    if (!result) message_ = result.error().message;
    else { message_ = "manual group member saved; core unchanged"; selected_group_id_.clear(); refresh_groups(); }
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
  bool filtering_nodes_ = false;
  bool node_detail_ = false;
  bool editing_group_ = false;
  bool editing_node_ = false;
  bool editing_dirty_ = false;
  bool draft_json_invalid_ = false;
  std::optional<std::string> editing_base_text_;
  std::string editing_key_;
  std::string node_filter_;
  std::string draft_;
  std::size_t edit_cursor_ = 0;
  std::vector<Json> nodes_;
  std::vector<Json> subscriptions_;
  std::vector<Json> modules_;
  Json status_ = Json::object();
  Json logs_ = Json::object();
  std::vector<Json> log_entries_;
  std::uint64_t log_cursor_ = 0;
  std::size_t log_bytes_ = 0;
  std::size_t log_view_start_ = 0;
  bool logs_paused_ = false;
  bool log_dropped_ = false;
  std::atomic_bool log_poll_active_{false};
  std::atomic_bool log_poller_stop_{false};
  std::thread log_poller_;
  Json stats_ = Json::object();
  std::unique_ptr<RpcSession> stats_rpc_;
  bool stats_watching_ = false;
  std::string message_;
  std::string expected_selection_;
  std::string selected_group_id_;
  std::string editing_node_id_;
  std::string editing_node_field_;
  std::array<std::string, 5> node_edit_fields_{"transport", "security", "sockopt", "finalmask", "mux"};
  std::size_t node_field_index_ = 0;
  std::vector<Json> groups_;
  std::vector<Json> cores_;
  std::vector<Json> core_releases_;
  std::string pending_core_operation_;
  std::string pending_core_label_;
  bool viewing_releases_ = false;
  bool include_prerelease_ = false;
  bool show_help_ = false;
};

}  // namespace

int run_tui(const std::string& socket, const std::string& profile_id) {
  ScreenInteractive screen = ScreenInteractive::Fullscreen();
  auto app = Make<TuiApp>(socket, profile_id, screen);
  screen.Loop(app);
  return 0;
}

}  // namespace xrm::cli

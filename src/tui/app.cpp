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
#include <cctype>
#include <map>
#include <optional>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
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
#include <tuple>
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
struct PageNav {
  const char* title;
  const char* help;
};
// This is the visual order, numeric key order, and Tab order.
constexpr std::array<PageNav, 9> kPages{{
    {"Overview", "c Connect   d Stop   r Refresh   ? help"},
    {"Nodes", "↑↓ PgUp/PgDn Home/End   / Search   f Removed   x Toggle   s Select   c Connect   ? help"},
    {"Modules", "↑↓ Move   Enter Edit Native   e Enable/Disable   ? help"},
    {"Logs", "↑↓ History   p Pause/Resume   r Refresh   ? help"},
    {"Subscriptions", "↑↓ Move   Enter Details   a Add   u Update   ? help"},
    {"Groups", "↑↓ Move   Enter Details   s Select   m Choose Member   ? help"},
    {"Stats", "r Refresh   ? help"},
    {"Cores", "↑↓ Move   a Register   l Releases   i Install   u Use   r Refresh   ? help"},
    {"Profiles", "↑↓ Move   Enter Open   n New   r Rename   c Clone   d Delete   ? help"},
}};

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

std::string short_utf8(std::string_view value, std::size_t max_chars) {
  std::size_t offset = 0;
  std::size_t chars = 0;
  while (offset < value.size() && chars < max_chars) {
    ++offset;
    while (offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80) ++offset;
    ++chars;
  }
  if (offset == value.size()) return std::string(value);
  return std::string(value.substr(0, offset)) + "…";
}

std::string short_update_time(const Json& source) {
  if (!source.contains("last_attempt") || !source["last_attempt"].is_number_integer())
    return "never";
  const auto seconds = static_cast<std::time_t>(source["last_attempt"].get<std::int64_t>() / 1000);
  std::tm local{};
  if (!localtime_r(&seconds, &local)) return "unknown";
  char buffer[20]{};
  if (std::strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &local) == 0) return "unknown";
  return buffer;
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

Result<std::vector<Json>> fetch_all_nodes(const std::string& socket) {
  std::vector<Json> nodes;
  std::size_t offset = 0;
  for (;;) {
    auto response = rpc(socket, std::string(methods::node_list),
                        Json{{"offset", offset}, {"limit", 500}});
    if (!response) return std::unexpected(response.error());
    if (!response->contains("items") || !response->at("items").is_array())
      return std::unexpected(AppError{ErrorCode::io_error, "node.list returned no items", std::nullopt, std::nullopt});
    auto batch = response->at("items").get<std::vector<Json>>();
    nodes.insert(nodes.end(), std::make_move_iterator(batch.begin()),
                 std::make_move_iterator(batch.end()));
    if (!response->contains("next_offset") || response->at("next_offset").is_null()) break;
    const auto next = response->at("next_offset").get<std::size_t>();
    if (next <= offset)
      return std::unexpected(AppError{ErrorCode::io_error, "node.list pagination did not advance", std::nullopt, std::nullopt});
    offset = next;
  }
  return nodes;
}

class TuiApp final : public ComponentBase {
 public:
  enum class Prompt { none, subscription_name, subscription_url, core_path, profile_name, profile_clone_name, profile_rename_name };
  enum class AsyncUiState { idle, starting, running, success, partial, failed };
  struct SubscriptionPageState {
    AsyncUiState update_state = AsyncUiState::idle;
    std::size_t spinner_phase = 0;
    std::optional<std::string> operation_error;
    std::optional<std::string> updating_source_id;
  };
  struct RuntimePageState {
    AsyncUiState operation_state = AsyncUiState::idle;
    std::string operation_label;
    std::size_t spinner_phase = 0;
  };
  struct Toast {
    enum class Level { info, success, warning, error } level = Level::info;
    std::string text;
    std::chrono::steady_clock::time_point expires;
  };
  struct TuiState {
    int page = 0;
    std::string viewed_profile_id;
    std::optional<std::string> highlighted_node_id;
    std::optional<std::string> highlighted_group_id;
    std::string node_search;
    bool log_follow = true;
    std::optional<std::string> module_draft;
    SubscriptionPageState subscriptions;
    RuntimePageState runtime;
    std::vector<Toast> toasts;
  };
  struct EditorSnapshot { std::string text; std::size_t cursor = 0; };
  struct SubscriptionRefreshEvent {
    std::optional<Json> result;
    std::optional<std::string> error;
    std::vector<Json> subscriptions;
    std::vector<Json> nodes;
    bool subscriptions_loaded = false;
    bool nodes_loaded = false;
  };
  struct RuntimeOperationEvent {
    std::string label;
    std::optional<std::string> error;
    std::optional<Json> status;
  };
  struct AsyncMailbox {
    std::mutex mutex;
    std::optional<SubscriptionRefreshEvent> subscription_refresh;
    bool subscription_running = false;
    std::vector<RuntimeOperationEvent> runtime_operations;
    std::optional<Json> logs;
  };
  struct UiEventPoster {
    std::mutex mutex;
    ScreenInteractive* screen = nullptr;
    std::atomic_bool alive{true};
  };
  struct LogPollState {
    std::atomic_bool active{false};
    std::atomic_bool stop{false};
    std::atomic_bool animation{false};
    std::atomic<std::uint64_t> cursor{0};
  };

  static void post_ui_event(const std::shared_ptr<UiEventPoster>& poster) {
    if (!poster->alive.load()) return;
    std::lock_guard lock(poster->mutex);
    if (!poster->alive.load() || !poster->screen) return;
    std::weak_ptr<UiEventPoster> weak = poster;
    poster->screen->Post([weak] {
      const auto owner = weak.lock();
      if (!owner) return;
      std::lock_guard event_lock(owner->mutex);
      if (owner->screen) owner->screen->PostEvent(Event::Custom);
    });
  }
  TuiApp(std::string socket, std::string profile, ScreenInteractive& screen)
      : socket_(std::move(socket)), state_{.viewed_profile_id = std::move(profile)}, screen_(&screen) {
    ui_poster_->screen = &screen;
    refresh_profiles();
    if (profile_id_.empty() && profiles_.size() == 1) profile_id_ = profiles_.front().value("id", "");
    else if (!profile_id_.empty()) resolve_initial_profile_ref(profile_id_);
    if (!profile_id_.empty()) selected_core_for_profile_ = profile_core_id(profile_id_);
    refresh_all();
    if (profile_id_.empty() && profiles_.size() > 1) {
      page_ = 8;
      message_ = "Choose a Profile to view; this does not change the running instance";
    }
    const auto log_socket = socket_;
    const auto log_mailbox = async_mailbox_;
    const auto log_poster = ui_poster_;
    const auto log_poll_state = log_poll_state_;
    log_poller_ = std::thread([log_socket, log_mailbox, log_poster, log_poll_state] {
      auto next_log_poll = std::chrono::steady_clock::time_point::min();
      while (!log_poll_state->stop.load()) {
        const bool logs_active = log_poll_state->active.load();
        const bool animation_active = log_poll_state->animation.load();
        const auto now = std::chrono::steady_clock::now();
        if (logs_active && now >= next_log_poll) {
          auto response = rpc(log_socket, std::string(methods::logs_read),
              Json{{"cursor", log_poll_state->cursor.load()}, {"limit", 65536}});
          next_log_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
          if (response) {
            {
              std::lock_guard lock(log_mailbox->mutex);
              log_mailbox->logs = std::move(*response);
            }
            post_ui_event(log_poster);
          }
        }
        if (animation_active) {
          post_ui_event(log_poster);
        }
        const auto delay = animation_active ? std::chrono::milliseconds(120) :
            logs_active ? std::chrono::milliseconds(200) : std::chrono::milliseconds(500);
        std::this_thread::sleep_for(delay);
      }
    });
  }
  ~TuiApp() override {
    ui_poster_->alive.store(false);
    log_poll_state_->stop.store(true);
    if (log_poller_.joinable()) log_poller_.detach();
    {
      std::lock_guard lock(ui_poster_->mutex);
      ui_poster_->screen = nullptr;
    }
    if (stats_watching_) refresh_stats(false);
  }

  bool OnEvent(Event event) override {
    if (event == Event::Custom) {
      if (state_.subscriptions.update_state == AsyncUiState::starting ||
          state_.subscriptions.update_state == AsyncUiState::running)
        ++state_.subscriptions.spinner_phase;
      if (state_.runtime.operation_state == AsyncUiState::starting ||
          state_.runtime.operation_state == AsyncUiState::running)
        ++state_.runtime.spinner_phase;
      std::optional<SubscriptionRefreshEvent> completed;
      bool subscription_running = false;
      std::vector<RuntimeOperationEvent> runtime_completed;
      std::optional<Json> logs_completed;
      {
        std::lock_guard lock(async_mailbox_->mutex);
        completed = std::move(async_mailbox_->subscription_refresh);
        async_mailbox_->subscription_refresh.reset();
        subscription_running = async_mailbox_->subscription_running;
        async_mailbox_->subscription_running = false;
        runtime_completed = std::move(async_mailbox_->runtime_operations);
        async_mailbox_->runtime_operations.clear();
        logs_completed = std::move(async_mailbox_->logs);
        async_mailbox_->logs.reset();
      }
      if (subscription_running) state_.subscriptions.update_state = AsyncUiState::running;
      if (completed) apply_subscription_refresh(std::move(*completed));
      for (auto& result : runtime_completed) apply_runtime_operation(std::move(result));
      if (logs_completed) apply_logs(std::move(*logs_completed));
      const auto now = std::chrono::steady_clock::now();
      std::erase_if(state_.toasts, [&](const Toast& toast) { return toast.expires <= now; });
      log_poll_state_->animation.store(!state_.toasts.empty());
      return true;
    }
    if (filtering_nodes_) {
      if (event == Event::Escape) { filtering_nodes_ = false; return true; }
      if (event == Event::Return) {
        filtering_nodes_ = false;
        rebuild_node_view();
        return true;
      }
      if (event == Event::Backspace && !node_filter_.empty()) {
        auto erase_from = node_filter_.size() - 1;
        while (erase_from > 0 && (static_cast<unsigned char>(node_filter_[erase_from]) & 0xc0) == 0x80) --erase_from;
        node_filter_.erase(erase_from);
      }
      else if (event.is_character()) node_filter_ += event.character();
      state_.node_search = node_filter_;
      return true;
    }
    if (editing_) {
      if (event == Event::Escape || event == Event::CtrlQ) {
        editing_ = false; editing_group_ = false; editing_node_ = false; return true;
      }
      if (event == Event::CtrlS || event == Event::F2) { save_editor(); return true; }
      if ((event == Event::Character("v") || event == Event::F6) && !editing_node_ && !editing_group_) {
        open_external_editor();
        return true;
      }
      if (event == Event::CtrlZ && !editor_undo_.empty()) {
        draft_ = std::move(editor_undo_.back().text);
        edit_cursor_ = editor_undo_.back().cursor;
        editor_undo_.pop_back();
      } else if (event == Event::ArrowLeft && edit_cursor_ > 0) edit_cursor_ = previous_utf8(draft_, edit_cursor_);
      else if (event == Event::ArrowRight && edit_cursor_ < draft_.size()) edit_cursor_ = next_utf8(draft_, edit_cursor_);
      else if (event == Event::Home) edit_cursor_ = line_start(draft_, edit_cursor_);
      else if (event == Event::End) edit_cursor_ = line_end(draft_, edit_cursor_);
      else if (event == Event::ArrowUp || event == Event::ArrowDown || event == Event::PageUp || event == Event::PageDown) {
        move_vertical(event == Event::ArrowUp || event == Event::PageUp,
                      event == Event::PageUp || event == Event::PageDown ? 8 : 1);
      } else if ((event == Event::Backspace && edit_cursor_ > 0) ||
                 (event == Event::Delete && edit_cursor_ < draft_.size()) ||
                 event == Event::Return || event.is_character()) {
        remember_editor_state();
        if (event == Event::Backspace && edit_cursor_ > 0) {
          const auto erase_from = previous_utf8(draft_, edit_cursor_);
          draft_.erase(erase_from, edit_cursor_ - erase_from);
          edit_cursor_ = erase_from;
        } else if (event == Event::Delete && edit_cursor_ < draft_.size()) {
          draft_.erase(edit_cursor_, next_utf8(draft_, edit_cursor_) - edit_cursor_);
        } else if (event == Event::Return) {
          draft_.insert(edit_cursor_++, "\n");
        } else if (event.is_character()) {
          const auto value = event.character();
          draft_.insert(edit_cursor_, value);
          edit_cursor_ += value.size();
        }
      }
      if (!editing_node_ && !editing_group_ && editing_base_text_)
        editing_dirty_ = draft_ != *editing_base_text_;
      if (!editing_node_ && !editing_group_) state_.module_draft = draft_;
      return true;
    }
    if (prompt_ != Prompt::none) {
      if (event == Event::Escape || event == Event::CtrlQ) { prompt_ = Prompt::none; prompt_text_.clear(); message_ = "cancelled"; return true; }
      if (event == Event::Return) { submit_prompt(); return true; }
      if (event == Event::Backspace) {
        if (!prompt_text_.empty()) {
          std::size_t start = prompt_text_.size() - 1;
          while (start > 0 && (static_cast<unsigned char>(prompt_text_[start]) & 0xc0) == 0x80) --start;
          prompt_text_.erase(start);
        }
        return true;
      }
      if (event.is_character()) prompt_text_ += event.character();
      return true;
    }
    if (node_detail_) {
      if (event == Event::Escape || event == Event::CtrlQ) node_detail_ = false;
      else if (event == Event::Character("t"))
        node_field_index_ = (node_field_index_ + 1) % node_edit_fields_.size();
      else if (event == Event::Character("e")) open_node_editor();
      return true;
    }
    if (subscription_detail_) {
      if (event == Event::Escape || event == Event::Return) subscription_detail_ = false;
      return true;
    }
    if (show_help_) {
      if (event == Event::Escape || event == Event::CtrlQ || event == Event::Character("?"))
        show_help_ = false;
      return true;
    }
    if (connect_profile_prompt_) {
      if (event == Event::Escape || event == Event::CtrlQ) {
        connect_profile_prompt_ = false;
        message_ = "connect cancelled";
      } else if (event == Event::Character("o")) {
        connect_profile_prompt_ = false;
        page_ = 8;
        refresh_profiles();
        message_ = "choose a Profile to view; it will not connect until you explicitly connect";
      } else if (event == Event::Character("n")) {
        connect_profile_prompt_ = false;
        if (selected_core_for_profile_.empty()) {
          page_ = 7;
          message_ = "register or select an Xray core, then create the Profile";
        } else {
          page_ = 8;
          begin_profile_prompt(false);
        }
      }
      return true;
    }
    if (event == Event::Character("q") || event == Event::Escape) {
      stop_stats_watch();
      ui_poster_->alive.store(false);
      {
        std::lock_guard lock(ui_poster_->mutex);
        ui_poster_->screen = nullptr;
      }
      screen_->Exit();
      return true;
    }
    if (event == Event::Character("?")) { show_help_ = true; return true; }
    if (page_ == 8 && event == Event::Character("n")) { begin_profile_prompt(false); return true; }
    if (page_ == 8 && event == Event::Character("c")) { begin_profile_prompt(true); return true; }
    if (page_ == 8 && event == Event::Character("r")) { begin_profile_prompt(false, true); return true; }
    if (page_ == 8 && event == Event::Character("d")) { remove_selected_profile(); return true; }
    if (event == Event::Tab) { navigate_page((page_ + 1) % kPages.size()); return true; }
    if (event == Event::TabReverse) { navigate_page((page_ + kPages.size() - 1) % kPages.size()); return true; }
    if (event.is_character() && event.character().size() == 1 &&
        event.character()[0] >= '1' && event.character()[0] <= '9') {
      navigate_page(event.character()[0] - '1');
      return true;
    }
    else if (event == Event::ArrowDown && page_ == 3) {
      const auto tail = log_entries_.size() > kLogViewportRows ? log_entries_.size() - kLogViewportRows : 0;
      if (log_view_start_ < tail) ++log_view_start_;
    }
    else if (event == Event::ArrowUp && page_ == 3) {
      if (log_view_start_ > 0) --log_view_start_;
    }
    else if (page_ == 1 && event == Event::Character("f")) {
      show_missing_nodes_ = !show_missing_nodes_;
      rebuild_node_view();
      message_ = show_missing_nodes_ ? "Showing available and source missing nodes" : "Showing current nodes only";
    }
    else if (page_ == 1 && (event == Event::PageUp || event == Event::PageDown ||
                            event == Event::Home || event == Event::End)) {
      const auto last = nodes_.empty() ? 0 : nodes_.size() - 1;
      if (event == Event::Home) selected_ = 0;
      else if (event == Event::End) selected_ = last;
      else if (event == Event::PageUp) selected_ = selected_ > 10 ? selected_ - 10 : 0;
      else selected_ = std::min(selected_ + 10, last);
      page_selections_[1] = selected_;
      remember_highlighted_item();
    }
    else if (event == Event::ArrowDown) {
      selected_ = std::min(selected_ + 1, page_size() ? page_size() - 1 : 0);
      page_selections_[static_cast<std::size_t>(page_)] = selected_;
      remember_highlighted_item();
    }
    else if (event == Event::ArrowUp) {
      selected_ = selected_ == 0 ? 0 : selected_ - 1;
      page_selections_[static_cast<std::size_t>(page_)] = selected_;
      remember_highlighted_item();
    }
    else if (event == Event::Return && page_ == 1) node_detail_ = true;
    else if (event == Event::Return && page_ == 4 && selected_ < subscriptions_.size()) subscription_detail_ = true;
    else if (event == Event::Return && page_ == 2) open_editor();
    else if (event == Event::Return && page_ == 5) open_group_editor();
    else if (event == Event::Return && page_ == 7 && selected_ < cores_.size()) {
      selected_core_for_profile_ = cores_[selected_].value("id", "");
      message_ = "core selected for new Profile; installation/use state is unchanged";
    }
    else if (event == Event::Return && page_ == 8 && selected_ < profiles_.size()) select_profile_context();
    else if (event == Event::Character("e") && page_ == 2) toggle_module();
    else if (event == Event::Character("s") && page_ == 1) select_node();
    else if (event == Event::Character("x") && page_ == 1) toggle_node_enabled();
    else if (event == Event::Character("s") && page_ == 5) select_group();
    else if (event == Event::Character("m") && page_ == 5) choose_group_member();
    else if (event == Event::Character("m") && page_ == 1) select_group_member();
    else if (event == Event::Character("c") && profile_id_.empty()) connect_profile_prompt_ = true;
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
    else if (event == Event::Character("a") && page_ == 4) begin_prompt(Prompt::subscription_name, "Subscription name");
    else if (event == Event::Character("a") && page_ == 7) begin_prompt(Prompt::core_path, "Absolute path to an existing Xray executable");
    else if (event == Event::Character("l") && page_ == 7) fetch_core_releases(false);
    else if (event == Event::Character("p") && page_ == 7) fetch_core_releases(true);
    else if (event == Event::Character("i") && page_ == 7) install_selected_release();
    else if (event == Event::Character("u") && page_ == 7) use_selected_core();
    return true;
  }

  Element OnRender() override {
    Elements tabs;
    for (std::size_t i = 0; i < kPages.size(); ++i) {
      const bool current = i == static_cast<std::size_t>(page_);
      auto label = std::to_string(i + 1) + " " + kPages[i].title;
      auto tab = text(current ? "[" + label + "] " : " " + label + " ") |
          (current ? bold : nothing) | (current ? inverted : nothing);
      if (current) tab = tab | focus;
      tabs.push_back(std::move(tab));
    }
    Elements header{
      text(" XRM · Current: " + std::string(kPages[static_cast<std::size_t>(page_)].title)) | bold,
      hbox(std::move(tabs)) | hscroll_indicator | xframe,
    };
    Elements body;
    if (editing_) {
      body.push_back(text("Native module editor — " + editing_key_ +
          (editing_dirty_ ? " [unsaved]" : "") +
          (editing_node_ || editing_group_ ? " (F2/Ctrl-S Save, Esc close)" : " (F2 Save, F6/v external editor, Esc close)")) | bold);
      if (!editing_node_ && !editing_group_) {
        body.push_back(text(editing_module_included_ ? "Runtime state: enabled" : "Runtime state: disabled (editing does not enable it)") | dim);
        if (editing_module_unconfigured_)
          body.push_back(text("Not configured yet. This is an unsaved native draft; Save creates the source file. Press e on the module list to enable it separately.") | color(Color::Yellow));
      }
      if (draft_json_invalid_)
        body.push_back(text("Draft is not valid strict JSON; source can be saved, but form/runtime checks may reject it") | color(Color::Yellow));
      auto visible = draft_;
      visible.insert(edit_cursor_, "█");
      body.push_back(paragraph(visible) | border | flex);
    } else if (page_ == 0) {
      body.push_back(text("XRM Dashboard") | bold | color(Color::Cyan));
      if (profile_id_.empty())
        body.push_back(text("First use: 8 Cores register Xray → 5 Subs add/update → 2 Nodes → 9 Profiles create") | color(Color::Yellow));
      body.push_back(text("Viewing Profile: " + (profile_id_.empty() ? "No profile selected" : profile_name(profile_id_)) +
          "   |   Running Profile: " + active_profile()) | bold);
      body.push_back(hbox({
          vbox({text("VIEWING PROFILE") | bold | color(Color::Cyan),
                text(profile_id_.empty() ? "No profile selected" : profile_name(profile_id_))}) | border | flex,
          vbox({text("RUNNING PROFILE") | bold | color(Color::Cyan), text(active_profile())}) | border | flex,
      }));
      const auto core_version = status_.contains("active") && status_["active"].is_object()
          ? status_["active"].value("core_version", "unknown") : "not running";
      body.push_back(text("CORE  " + status_.value("state", "unknown") + " · " +
          status_.value("health", "unknown") + " · " +
          core_version.substr(0, core_version.find(" ("))) | bold | color(Color::Cyan));
      body.push_back(text("LOCAL PROXY  " +
          (proxy_entries_.empty() ? "No entry in running configuration" : proxy_entries_.front())) | bold);
      if (state_.runtime.operation_state == AsyncUiState::starting ||
          state_.runtime.operation_state == AsyncUiState::running) {
        static constexpr std::array<const char*, 4> spinner{"⠋", "⠙", "⠹", "⠸"};
        body.push_back(text(std::string(spinner[state_.runtime.spinner_phase % spinner.size()]) + " " +
            state_.runtime.operation_label + "…") | color(Color::Cyan) | bold);
      }
      body.push_back(text("Saved target: " + expected_selection()) | bold);
      body.push_back(text("Actual target: " + actual_selection()) | bold);
      body.push_back(hbox({
          vbox({text("RUNTIME OVERRIDE") | bold | color(Color::Cyan),
                text(status_.value("override_state", "unknown") +
                    (status_.contains("runtime_override") && status_["runtime_override"].is_string()
                        ? " " + status_["runtime_override"].get<std::string>() : ""))}) | border | flex,
          vbox({text("INVENTORY") | bold | color(Color::Cyan),
                text(std::to_string(subscriptions_.size()) + " subscriptions · " +
                     std::to_string(node_count_) + " nodes")}) | border | flex,
      }));
      body.push_back(text("Runtime files: " + active_confdir()) | dim);
      body.push_back(text("Core running is separate from proxy listener readiness and remote connectivity") | dim);
      body.push_back(text("Pending changes: saved edits apply only on explicit Connect/Apply") | dim);
      body.push_back(text("c Connect   d Stop   r Refresh   q Exit (core continues)") | dim);
      if (profile_id_.empty()) {
        body.push_back(text("No profile selected. Subscriptions, Nodes, Cores and Profiles are available without one.") | color(Color::Yellow));
        body.push_back(text("Press c to Create Default Profile, Open Profiles, or Cancel.") | color(Color::Yellow));
      }
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
        body.push_back(text("Nodes — " + std::to_string(node_count_) + " current, " +
            std::to_string(node_missing_count_) + " source missing; showing " +
            std::to_string(nodes_.size()) + (show_missing_nodes_ ? " (all)" : " (current)")) | bold);
        body.push_back(text("Search: " + (node_filter_.empty() ? "—" : node_filter_) +
                            "   f Show removed: " + (show_missing_nodes_ ? "on" : "off")) | dim);
        body.push_back(text(screen_->dimx() < 95
            ? "   Status  Name                     Protocol / Transport    Security"
            : "   Status  Name                             Protocol / Transport    Security   Source   Latency") | bold);
        if (nodes_.empty())
          body.push_back(text("No nodes in this view. Update a subscription, clear search, or press f for removed nodes.") | dim);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
          const auto& node = nodes_[i];
          const auto id = node.value("id", "");
          const auto saved = expected_selection_ == "node:" + id;
          const auto actual = status_.contains("active") && status_["active"].is_object() &&
              status_["active"].contains("selection") && status_["active"]["selection"].is_object() &&
              status_["active"]["selection"].value("kind", "") == "node" &&
              status_["active"]["selection"].value("id_or_tag", "") == id;
          const std::string marker = actual ? "●" : saved ? "◆" : node.value("source_missing", false) ? "×" :
              node.value("enabled", true) ? "·" : "○";
          std::string source = "Manual";
          if (node.contains("source_id") && node["source_id"].is_string()) {
            source = "Subscription";
            for (const auto& item : subscriptions_)
              if (item.value("id", "") == node["source_id"].get<std::string>()) {
                source = item.value("name", "Subscription"); break;
              }
          }
          const auto compact = screen_->dimx() < 95;
          const auto name = short_utf8(node.value("name", ""), compact ? 24 : 32);
          const auto row = std::string(i == selected_ ? "▸ " : "  ") + marker + "  " + name +
              "   " + node.value("protocol", "") + "/" + node.value("transport", "—") +
              "   " + node.value("security", "—") +
              (compact ? "" : "   " + short_utf8(source, 18) + "   --");
          auto element = text(row) | (i == selected_ ? inverted : nothing);
          if (i == selected_) element = element | focus;
          body.push_back(std::move(element));
        }
        if (selected_ < nodes_.size()) {
          const auto& selected = nodes_[selected_];
          body.push_back(text("Selected: " + selected.value("name", "") +
              " · " + (selected.value("source_missing", false) ? "removed from source" :
                           selected.value("enabled", true) ? "available, untested" : "disabled") +
              " · Enter for ID and Codec support") | dim);
        }
      }
    } else if (page_ == 2) {
      body.push_back(text("Native Modules — Enter Edit Native; inside choose Open External Editor (F6). e toggles inclusion. Disabled sources remain editable; empty modules create only on Save.") | bold);
      if (profile_id_.empty()) {
        body.push_back(text("No profile selected") | color(Color::Yellow));
        body.push_back(text("Create a profile to use this feature") | color(Color::Yellow));
      }
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
      body.push_back(text("Subscriptions — inventory refresh never changes the running core") | bold);
      if (state_.subscriptions.update_state == AsyncUiState::starting ||
          state_.subscriptions.update_state == AsyncUiState::running) {
        static constexpr std::array<const char*, 4> spinner{"⠋", "⠙", "⠹", "⠸"};
        body.push_back(text(std::string(spinner[state_.subscriptions.spinner_phase % spinner.size()]) +
            (state_.subscriptions.update_state == AsyncUiState::starting
                ? " Contacting xrmd…" : " Updating subscription inventory…")) | color(Color::Cyan));
      }
      for (std::size_t i = 0; i < subscriptions_.size(); ++i) {
        const auto& source = subscriptions_[i];
        const auto row = short_utf8(source.value("name", ""), 26) + "  " +
            std::to_string(source.value("node_count", 0)) + " current  " + short_update_time(source) + "  " +
            source.value("last_result", "never") +
            ((state_.subscriptions.update_state == AsyncUiState::starting ||
              state_.subscriptions.update_state == AsyncUiState::running) &&
              state_.subscriptions.updating_source_id == source.value("id", "")
                ? "  ⠋ updating" : "");
        auto element = text((i == selected_ ? "▸ " : "  ") + row) | (i == selected_ ? inverted : nothing);
        if (i == selected_) element = element | focus;
        body.push_back(std::move(element));
      }
      if (subscriptions_.empty()) body.push_back(text("No subscriptions yet. Press a to paste a local or remote URL.") | dim);
      if (selected_ < subscriptions_.size()) {
        const auto& source = subscriptions_[selected_];
        body.push_back(text("Selected: " + source.value("name", "") + " · " +
            std::to_string(source.value("node_count", 0)) + " current nodes") | dim);
        if (subscription_detail_) {
          body.push_back(text("Last result: " + source.value("last_result", "never")));
          body.push_back(text("Last attempt: " + short_update_time(source)));
          body.push_back(text("Unsupported: " + std::to_string(source.value("unsupported_count", 0)) +
              " · malformed: " + std::to_string(source.value("malformed_count", 0))));
          if (state_.subscriptions.operation_error)
            body.push_back(paragraph("Last update error: " + *state_.subscriptions.operation_error) | color(Color::Red));
          body.push_back(text("Enter or Esc closes details; URL and credentials are hidden") | dim);
        }
      }
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
      if (profile_id_.empty()) {
        body.push_back(text("No profile selected") | color(Color::Yellow));
        body.push_back(text("Create a profile to use this feature") | color(Color::Yellow));
      }
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
    } else if (page_ == 7) {
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
        for (std::size_t i = 0; i < cores_.size(); ++i) {
          const auto& core = cores_[i];
          bool selected = false;
          for (const auto& profile : profiles_)
            selected = selected || profile.value("core_id", "") == core.value("id", "");
          body.push_back(text((i == selected_ ? "> " : "  ") + core.value("name", "") + "  " +
              core.value("reported_version", "") + "  " +
              (core.value("managed", false) ? "managed" : "system") + "  installed" +
              (selected ? " selected" : "") + (running == core.value("id", "") ? " running" : "")) |
              (i == selected_ ? inverted : nothing));
          body.push_back(text(core.value("path", "")) | dim);
        }
        body.push_back(text("a register existing xray   Enter select core for a new Profile   l releases   i install   u use for current Profile") | dim);
      }
      body.push_back(text("r refresh/poll operation; install never changes the running core") | dim);
    } else if (page_ == 8) {
      body.push_back(text("Profiles — Enter switch browsing context; n create; c copy; r rename; d delete") | bold);
      for (std::size_t i = 0; i < profiles_.size(); ++i) {
        const auto& profile = profiles_[i];
        body.push_back(text((i == selected_ ? "> " : "  ") + profile.value("name", "") +
            (profile.value("id", "") == profile_id_ ? "  editing" : "") +
            (profile.value("id", "") == active_profile_id() ? "  running" : "")) |
            (i == selected_ ? inverted : nothing));
      }
      if (profiles_.empty()) body.push_back(text("No Profile. Register/select an Xray core on Cores, then press n here.") | color(Color::Yellow));
      body.push_back(text("Profile browsing does not connect, save other drafts, or change the running instance.") | dim);
    }
    if (connect_profile_prompt_)
      body.push_back(text("No profile selected — n Create Default Profile   o Open Profiles   Esc Cancel") | color(Color::Yellow));
    if (filtering_nodes_) body.push_back(text("Filter nodes: " + node_filter_ + "  Enter apply / Esc cancel") | color(Color::Yellow));
    Elements footer_lines;
    if (!state_.toasts.empty() &&
        !(runtime_error_ && state_.toasts.back().text.starts_with("Connect failed:"))) {
      const auto& toast = state_.toasts.back();
      const auto shade = toast.level == Toast::Level::success ? Color::Green :
          toast.level == Toast::Level::warning ? Color::Yellow :
          toast.level == Toast::Level::error ? Color::Red : Color::Cyan;
      footer_lines.push_back(paragraph(toast.text) | color(shade) | bold);
    }
    if (!message_.empty() && !(runtime_error_ && message_.starts_with("Connect failed:")) &&
        (state_.toasts.empty() || state_.toasts.back().text != message_))
      footer_lines.push_back(paragraph(message_) | color(Color::Yellow));
    std::string help_bar = std::string(kPages[static_cast<std::size_t>(page_)].help) +
        "   Tab/Shift-Tab Pages   q Quit";
    if (prompt_ != Prompt::none) help_bar = "Type text   Enter Continue/Save   Esc Cancel";
    else if (editing_) help_bar = "↑↓←→ Move   F2 Save   F6 Open External Editor   Esc Close";
    if (runtime_error_)
      footer_lines.push_back(paragraph("Connection failed: " + *runtime_error_) | color(Color::Red) | bold);
    footer_lines.push_back(paragraph(help_bar) | dim);
    Element footer = vbox(std::move(footer_lines));
    auto base = vbox({vbox(std::move(header)), separator(), vbox(std::move(body)) | vscroll_indicator | yframe | flex,
                      separator(), std::move(footer)}) | border | flex;
    if (prompt_ != Prompt::none) {
      auto dialog = window(text(prompt_label_) | bold,
          vbox({text(prompt_text_ + "█") | border | size(WIDTH, LESS_THAN, 56),
                text("Enter continue/save   Esc cancel") | dim})) | center;
      return dbox({base | dim, std::move(dialog)});
    }
    if (show_help_) {
      // Keep help concise enough to fit over the current page at 80x24.
      return dbox({base | dim,
          window(text("Help") | bold,
              vbox({text("Tab / Shift-Tab  change page"),
                    text("1–9              open a page directly"),
                    text("↑ / ↓            move through a list"),
                    text("Enter            view or open selected item"),
                    text("c                connect   d stop   r refresh"),
                    text("Logs: p pause/resume; arrows browse history"),
                    text("Module editor: F6/v external editor; F2 save"),
                    text("q quit; core continues   Esc or ? close")})) | center});
    }
    return base;
  }

 private:
  std::size_t page_size() const {
    if (page_ == 1) return nodes_.size();
    if (page_ == 2) return modules_.size();
    if (page_ == 4) return subscriptions_.size();
    if (page_ == 5) return groups_.size();
    if (page_ == 7) return viewing_releases_ ? core_releases_.size() : cores_.size();
    if (page_ == 8) return profiles_.size();
    return 0;
  }
  static std::size_t previous_utf8(std::string_view text, std::size_t cursor) {
    if (cursor == 0) return 0;
    auto position = cursor - 1;
    while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xc0) == 0x80) --position;
    return position;
  }
  static std::size_t next_utf8(std::string_view text, std::size_t cursor) {
    if (cursor >= text.size()) return text.size();
    auto position = cursor + 1;
    while (position < text.size() && (static_cast<unsigned char>(text[position]) & 0xc0) == 0x80) ++position;
    return position;
  }
  static std::size_t line_start(std::string_view text, std::size_t cursor) {
    if (cursor > 0) {
      const auto newline = text.rfind('\n', cursor - 1);
      if (newline != std::string_view::npos) return newline + 1;
    }
    return 0;
  }
  static std::size_t line_end(std::string_view text, std::size_t cursor) {
    const auto newline = text.find('\n', cursor);
    return newline == std::string_view::npos ? text.size() : newline;
  }
  void remember_editor_state() {
    editor_undo_.push_back({draft_, edit_cursor_});
    if (editor_undo_.size() > 64) editor_undo_.erase(editor_undo_.begin());
  }
  void move_vertical(bool up, std::size_t lines) {
    const auto start = line_start(draft_, edit_cursor_);
    const auto column = edit_cursor_ - start;
    if (up) {
      if (start == 0) { edit_cursor_ = 0; return; }
      const auto previous_end = start - 1;
      const auto previous_start = line_start(draft_, previous_end);
      edit_cursor_ = std::min(previous_start + column, previous_end);
      for (std::size_t i = 1; i < lines && edit_cursor_ > 0; ++i) {
        const auto current_start = line_start(draft_, edit_cursor_);
        if (current_start == 0) { edit_cursor_ = 0; break; }
        const auto end = current_start - 1;
        edit_cursor_ = std::min(line_start(draft_, end) + column, end);
      }
    } else {
      auto current_end = line_end(draft_, edit_cursor_);
      edit_cursor_ = std::min(current_end + 1 + column, draft_.size());
      for (std::size_t i = 1; i < lines && edit_cursor_ < draft_.size(); ++i) {
        current_end = line_end(draft_, edit_cursor_);
        if (current_end == draft_.size()) break;
        edit_cursor_ = std::min(current_end + 1 + column, draft_.size());
      }
    }
    while (edit_cursor_ > 0 && edit_cursor_ < draft_.size() &&
           (static_cast<unsigned char>(draft_[edit_cursor_]) & 0xc0) == 0x80) --edit_cursor_;
  }
  void refresh_all() {
    const auto active_selection = selected_;
    selected_ = page_selections_[8]; refresh_profiles(); page_selections_[8] = selected_;
    refresh_status(); refresh_expected();
    selected_ = page_selections_[1]; refresh_nodes(); page_selections_[1] = selected_;
    selected_ = page_selections_[2]; refresh_modules(); page_selections_[2] = selected_;
    selected_ = page_selections_[3]; refresh_logs(); page_selections_[3] = selected_;
    selected_ = page_selections_[4]; refresh_subscriptions(); page_selections_[4] = selected_;
    selected_ = page_selections_[5]; refresh_groups(); page_selections_[5] = selected_;
    selected_ = page_selections_[7]; refresh_cores(); page_selections_[7] = selected_;
    selected_ = active_selection;
  }
  void refresh_profiles() {
    auto response = rpc(socket_, std::string(methods::profile_list));
    profiles_.clear();
    if (response && response->is_array()) profiles_ = response->get<std::vector<Json>>();
    if (!profile_id_.empty()) select_profile_row(profile_id_);
    else selected_ = std::min(selected_, profiles_.empty() ? 0 : profiles_.size() - 1);
    if (selected_core_for_profile_.empty() && cores_.size() == 1)
      selected_core_for_profile_ = cores_.front().value("id", "");
  }
  std::string profile_name(std::string_view id) const {
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const Json& profile) {
      return profile.value("id", "") == id;
    });
    return found == profiles_.end() ? std::string(id) : found->value("name", std::string(id));
  }
  void resolve_initial_profile_ref(std::string_view input) {
    const auto by_id = std::find_if(profiles_.begin(), profiles_.end(), [&](const Json& profile) {
      return profile.value("id", "") == input;
    });
    if (by_id != profiles_.end()) return;
    const auto by_name = std::find_if(profiles_.begin(), profiles_.end(), [&](const Json& profile) {
      return profile.value("name", "") == input;
    });
    if (by_name == profiles_.end()) {
      profile_id_.clear();
      message_ = "Profile not found; choose one from Profiles";
      return;
    }
    const auto duplicate = std::find_if(std::next(by_name), profiles_.end(), [&](const Json& profile) {
      return profile.value("name", "") == input;
    });
    if (duplicate != profiles_.end()) {
      profile_id_.clear();
      message_ = "Profile name is ambiguous; choose from Profiles";
      return;
    }
    profile_id_ = by_name->value("id", "");
  }
  std::string profile_core_id(std::string_view id) const {
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const Json& profile) {
      return profile.value("id", "") == id;
    });
    return found == profiles_.end() ? std::string{} : found->value("core_id", "");
  }
  std::string active_profile_id() const {
    return status_.contains("active") && status_["active"].is_object()
        ? status_["active"].value("profile_id", "") : "";
  }
  void begin_prompt(Prompt prompt, std::string label) {
    prompt_ = prompt;
    prompt_label_ = std::move(label);
    prompt_text_.clear();
  }
  void begin_profile_prompt(bool clone, bool rename = false) {
    if ((clone || rename) && selected_ >= profiles_.size()) { message_ = "select a Profile first"; return; }
    if (clone) prompt_profile_id_ = profiles_[selected_].value("id", "");
    else if (rename) prompt_profile_id_ = profiles_[selected_].value("id", "");
    if (clone) begin_prompt(Prompt::profile_clone_name, "Copy name");
    else if (rename) begin_prompt(Prompt::profile_rename_name, "New Profile name");
    else begin_prompt(Prompt::profile_name, "New Profile name");
  }
  void submit_prompt() {
    if (prompt_ == Prompt::subscription_name) {
      prompt_subscription_name_ = prompt_text_;
      if (prompt_subscription_name_.empty()) { message_ = "subscription name cannot be empty"; return; }
      begin_prompt(Prompt::subscription_url, "Subscription URL (paste it here)");
      return;
    }
    const auto value = prompt_text_;
    const auto completed = prompt_;
    prompt_ = Prompt::none;
    prompt_text_.clear();
    if (completed == Prompt::subscription_url) {
      if (value.empty()) { message_ = "subscription URL cannot be empty"; return; }
      auto added = rpc(socket_, std::string(methods::subscription_add),
          Json{{"name", prompt_subscription_name_}, {"url", value}});
      if (!added) message_ = added.error().message;
      else { message_ = "subscription saved; press u to update node inventory (runtime unchanged)"; refresh_subscriptions(); }
      prompt_subscription_name_.clear();
      return;
    }
    if (completed == Prompt::core_path) {
      if (value.empty()) { message_ = "Xray path cannot be empty"; return; }
      const auto name = std::filesystem::path(value).filename().string();
      auto added = rpc(socket_, std::string(methods::core_register), Json{{"path", value}, {"name", name}});
      if (!added) message_ = added.error().message;
      else {
        selected_core_for_profile_ = added->value("id", "");
        message_ = "existing Xray registered; it is not started until you connect";
        refresh_cores();
        refresh_profiles();
      }
      return;
    }
    if (completed == Prompt::profile_name || completed == Prompt::profile_clone_name) {
      if (value.empty()) { message_ = "Profile name cannot be empty"; return; }
      if (completed == Prompt::profile_clone_name) {
        auto created = rpc(socket_, std::string(methods::profile_clone), Json{{"profile_id", prompt_profile_id_}, {"name", value}});
        if (!created) { message_ = created.error().message; return; }
        profile_id_ = created->value("id", "");
      } else {
        if (selected_core_for_profile_.empty()) {
          message_ = "register/select an Xray first on page 8 Cores";
          return;
        }
        auto created = rpc(socket_, std::string(methods::profile_create), Json{
            {"name", value}, {"core_id", selected_core_for_profile_}, {"preset", "local"}});
        if (!created) { message_ = created.error().message; return; }
        profile_id_ = created->value("id", "");
      }
      refresh_profiles();
      select_profile_row(profile_id_);
      refresh_all();
      page_ = 8;
      select_profile_row(profile_id_);
      message_ = "Profile selected for editing; it has not been connected";
      return;
    }
    if (completed == Prompt::profile_rename_name) {
      auto renamed = rpc(socket_, std::string(methods::profile_rename),
          Json{{"profile_id", prompt_profile_id_}, {"name", value}});
      if (!renamed) message_ = renamed.error().message;
      else { message_ = "Profile renamed; running configuration unchanged"; refresh_profiles(); }
    }
  }
  void select_profile_context() {
    profile_id_ = profiles_[selected_].value("id", "");
    select_profile_row(profile_id_);
    selected_core_for_profile_ = profile_core_id(profile_id_);
    refresh_all();
    page_ = 8;
    select_profile_row(profile_id_);
    message_ = "Viewing: " + profile_name(profile_id_) + "; Running: " + active_profile() + "; runtime unchanged";
  }
  void remove_selected_profile() {
    if (selected_ >= profiles_.size()) return;
    const auto id = profiles_[selected_].value("id", "");
    if (id == active_profile_id()) {
      message_ = "cannot remove the running Profile";
      return;
    }
    auto removed = rpc(socket_, std::string(methods::profile_remove), Json{{"profile_id", id}});
    if (!removed) message_ = removed.error().message;
    else {
      const bool removed_view = id == profile_id_;
      message_ = "Profile removed";
      refresh_profiles();
      if (removed_view) {
        profile_id_.clear();
        refresh_all();
      }
    }
  }
  void select_profile_row(std::string_view id) {
    const auto found = std::find_if(profiles_.begin(), profiles_.end(), [&](const Json& profile) {
      return profile.value("id", "") == id;
    });
    if (found != profiles_.end()) selected_ = static_cast<std::size_t>(found - profiles_.begin());
  }
  void remember_highlighted_item() {
    if (page_ == 1 && selected_ < nodes_.size())
      state_.highlighted_node_id = nodes_[selected_].value("id", "");
    else if (page_ == 5 && selected_ < groups_.size())
      state_.highlighted_group_id = groups_[selected_].value("id", "");
  }
  void change_page(int page) {
    if (page_ == 6 && page != 6) stop_stats_watch();
    page_selections_[static_cast<std::size_t>(page_)] = selected_;
    page_ = page;
    selected_ = page_selections_[static_cast<std::size_t>(page_)];
    log_poll_state_->active.store(page_ == 3);
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
    else if (page_ == 8) refresh_profiles();
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
    if (response) { status_ = std::move(*response); refresh_proxy_entries(); }
    else message_ = response.error().message;
  }
  void refresh_proxy_entries() {
    proxy_entries_.clear();
    if (!status_.contains("active") || !status_["active"].is_object()) return;
    const auto profile = status_["active"].value("profile_id", "");
    auto response = rpc(socket_, std::string(methods::config_files),
        Json{{"profile_id", profile}, {"source", "runtime"}, {"module", "inbounds"}});
    if (!response || !response->contains("files") || !(*response)["files"].is_array()) return;
    for (const auto& file : (*response)["files"]) {
      if (!file.contains("text") || !file["text"].is_string()) continue;
      const auto parsed = Json::parse(file["text"].get<std::string>(), nullptr, false);
      if (parsed.is_discarded() || !parsed.contains("inbounds") || !parsed["inbounds"].is_array()) continue;
      for (const auto& inbound : parsed["inbounds"]) {
        if (!inbound.is_object() || !inbound.contains("port") || !inbound["port"].is_number_integer()) continue;
        const auto kind = inbound.value("protocol", "proxy");
        const auto listen = inbound.value("listen", "0.0.0.0");
        proxy_entries_.push_back(kind + " " + listen + ":" + std::to_string(inbound["port"].get<int>()));
      }
    }
  }
  void refresh_nodes() {
    auto response = fetch_all_nodes(socket_);
    if (!response) { message_ = "Cannot load nodes: " + response.error().message; return; }
    all_nodes_ = std::move(*response);
    rebuild_node_view();
  }
  void rebuild_node_view() {
    std::string selected_id = state_.highlighted_node_id.value_or(std::string{});
    if (selected_id.empty() && selected_ < nodes_.size()) selected_id = nodes_[selected_].value("id", "");
    node_count_ = std::count_if(all_nodes_.begin(), all_nodes_.end(), [](const Json& node) {
      return !node.value("source_missing", false);
    });
    node_missing_count_ = all_nodes_.size() - node_count_;
    std::map<std::string, std::string> source_names;
    for (const auto& source : subscriptions_)
      source_names[source.value("id", "")] = source.value("name", "");
    auto source_name = [&](const Json& node) {
      if (!node.contains("source_id") || !node["source_id"].is_string()) return std::string("Manual");
      const auto id = node["source_id"].get<std::string>();
      auto found = source_names.find(id);
      return found == source_names.end() ? std::string("Subscription") : found->second;
    };
    nodes_.clear();
    auto search = node_filter_;
    std::transform(search.begin(), search.end(), search.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& node : all_nodes_) {
      if (!show_missing_nodes_ && node.value("source_missing", false)) continue;
      auto name = node.value("name", "");
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (!search.empty() && name.find(search) == std::string::npos) continue;
      nodes_.push_back(node);
    }
    std::sort(nodes_.begin(), nodes_.end(), [&](const Json& a, const Json& b) {
      const auto ka = std::tuple{source_name(a), a.value("name", ""), a.value("id", "")};
      const auto kb = std::tuple{source_name(b), b.value("name", ""), b.value("id", "")};
      return ka < kb;
    });
    if (!selected_id.empty()) {
      auto found = std::find_if(nodes_.begin(), nodes_.end(), [&](const auto& node) { return node.value("id", "") == selected_id; });
      if (found != nodes_.end()) selected_ = static_cast<std::size_t>(found - nodes_.begin());
      else {
        selected_ = std::min(selected_, nodes_.empty() ? 0 : nodes_.size() - 1);
        if (page_ == 1) message_ = "Previously highlighted node is no longer in this view";
      }
    }
    else selected_ = std::min(selected_, nodes_.empty() ? 0 : nodes_.size() - 1);
    if (selected_ < nodes_.size()) state_.highlighted_node_id = nodes_[selected_].value("id", "");
    else state_.highlighted_node_id.reset();
  }
  void refresh_subscriptions() {
    auto response = rpc(socket_, std::string(methods::subscription_list));
    subscriptions_.clear();
    if (response && response->is_array()) subscriptions_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, subscriptions_.empty() ? 0 : subscriptions_.size() - 1);
  }
  void refresh_groups() {
    if (profile_id_.empty()) return;
    std::string selected_id = state_.highlighted_group_id.value_or(std::string{});
    if (selected_id.empty() && selected_ < groups_.size()) selected_id = groups_[selected_].value("id", "");
    auto response = rpc(socket_, std::string(methods::group_list), Json{{"profile_id", profile_id_}});
    groups_.clear();
    if (response && response->is_array()) groups_ = response->get<std::vector<Json>>();
    if (!selected_id.empty()) {
      const auto found = std::find_if(groups_.begin(), groups_.end(), [&](const Json& group) {
        return group.value("id", "") == selected_id;
      });
      if (found != groups_.end()) selected_ = static_cast<std::size_t>(found - groups_.begin());
      else selected_ = std::min(selected_, groups_.empty() ? 0 : groups_.size() - 1);
    } else selected_ = std::min(selected_, groups_.empty() ? 0 : groups_.size() - 1);
    if (selected_ < groups_.size()) state_.highlighted_group_id = groups_[selected_].value("id", "");
    else state_.highlighted_group_id.reset();
  }
  void refresh_cores() {
    auto response = rpc(socket_, std::string(methods::core_list));
    cores_.clear();
    if (response && response->is_array()) cores_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, cores_.empty() ? 0 : cores_.size() - 1);
    if (selected_core_for_profile_.empty() && cores_.size() == 1)
      selected_core_for_profile_ = cores_.front().value("id", "");
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
    if (state_.subscriptions.update_state == AsyncUiState::starting ||
        state_.subscriptions.update_state == AsyncUiState::running) {
      message_ = "subscription update is already running";
      return;
    }
    state_.subscriptions.update_state = AsyncUiState::starting;
    state_.subscriptions.spinner_phase = 0;
    state_.subscriptions.operation_error.reset();
    state_.subscriptions.updating_source_id.reset();
    message_ = "Subscription update started";
    Json refresh_params = Json::object();
    if (selected_ < subscriptions_.size()) {
      const auto& selected_source = subscriptions_[selected_];
      const auto source_id = selected_source.value("id", "");
      if (!source_id.empty()) {
        refresh_params["source_ids"] = Json::array({source_id});
        state_.subscriptions.updating_source_id = source_id;
        message_ += ": " + selected_source.value("name", "subscription");
      }
    }
    message_ += "; Xray runtime will not change";
          post_ui_event(ui_poster_);
    const auto socket = socket_;
    const auto mailbox = async_mailbox_;
    const auto poster = ui_poster_;
    std::thread([socket, mailbox, poster, refresh_params = std::move(refresh_params)] {
      SubscriptionRefreshEvent event;
      auto accepted = rpc(socket, std::string(methods::subscription_refresh), refresh_params);
      if (!accepted) {
        event.error = accepted.error().message;
      } else if (accepted->value("status", "") != "accepted" || !accepted->contains("operation_id")) {
        event.error = "xrmd did not return an operation id";
      } else {
        const auto operation = accepted->at("operation_id").get<std::string>();
        {
          std::lock_guard lock(mailbox->mutex);
          mailbox->subscription_running = true;
        }
        post_ui_event(poster);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        for (;;) {
          if (!poster->alive.load()) return;
          std::this_thread::sleep_for(std::chrono::milliseconds(180));
          if (!poster->alive.load()) return;
          post_ui_event(poster);
          auto operation_state = rpc(socket, std::string(methods::operation_get),
                                     Json{{"operation_id", operation}});
          if (!operation_state) { event.error = operation_state.error().message; break; }
          const auto status = operation_state->value("status", "unknown");
          if (status == "running") {
            if (std::chrono::steady_clock::now() < deadline) continue;
            event.error = "Subscription update timed out; xrmd may still be processing the operation";
            break;
          }
          if (status == "failed") {
            event.error = operation_state->contains("error")
                ? operation_state->at("error").value("message", "subscription update failed")
                : "subscription update failed";
          } else if (status == "succeeded") {
            event.result = operation_state->value("result", Json::object());
          } else event.error = "unexpected subscription operation status: " + status;
          break;
        }
      }
      if (!poster->alive.load()) return;

      auto source_list = rpc(socket, std::string(methods::subscription_list));
      if (source_list && source_list->is_array()) {
        event.subscriptions = source_list->get<std::vector<Json>>();
        event.subscriptions_loaded = true;
      }
      auto node_list = fetch_all_nodes(socket);
      if (node_list) {
        event.nodes = std::move(*node_list);
        event.nodes_loaded = true;
      }
      {
        std::lock_guard lock(mailbox->mutex);
        mailbox->subscription_refresh = std::move(event);
      }
      post_ui_event(poster);
    }).detach();
  }

  void apply_subscription_refresh(SubscriptionRefreshEvent event) {
    state_.subscriptions.updating_source_id.reset();
    if (event.subscriptions_loaded) subscriptions_ = std::move(event.subscriptions);
    if (event.nodes_loaded) {
      all_nodes_ = std::move(event.nodes);
      rebuild_node_view();
    }
    if (event.error) {
      state_.subscriptions.update_state = AsyncUiState::failed;
      state_.subscriptions.operation_error = event.error;
      message_ = "Subscription update failed: " + *event.error + "; old node inventory retained where applicable";
      push_toast(Toast::Level::error, message_);
    } else {
      std::size_t added = 0, updated = 0, unsupported = 0, malformed = 0;
      if (event.result) {
        for (const auto& source : event.result->value("completed", Json::array())) {
          added += source.value("added", std::size_t{0});
          updated += source.value("updated", std::size_t{0});
          unsupported += source.value("unsupported", std::size_t{0});
          malformed += source.value("malformed", std::size_t{0});
        }
        if (event.result->contains("failed") && !event.result->at("failed").empty()) {
          state_.subscriptions.update_state = AsyncUiState::partial;
          const auto& first_failure = event.result->at("failed").front();
          const auto details = first_failure.value("error", Json::object());
          message_ = "Subscription update partial: " + std::to_string(added) + " added, " +
              std::to_string(updated) + " updated; " + std::to_string(event.result->at("failed").size()) +
              " source(s) failed" +
              (details.is_object() ? ": " + details.value("message", "update failed") : std::string{}) +
              "; node inventory refreshed";
          push_toast(Toast::Level::warning, message_);
        } else if (unsupported || malformed) {
          state_.subscriptions.update_state = AsyncUiState::partial;
          message_ = "Subscription update partial: " + std::to_string(added) + " added, " +
              std::to_string(updated) + " updated, " + std::to_string(unsupported) +
              " unsupported, " + std::to_string(malformed) + " malformed";
          push_toast(Toast::Level::warning, message_);
        } else {
          state_.subscriptions.update_state = AsyncUiState::success;
          message_ = "Subscription update complete: " + std::to_string(added) + " added, " +
              std::to_string(updated) + " updated; " + std::to_string(node_count_) + " nodes available";
          push_toast(Toast::Level::success, message_);
        }
      } else {
        state_.subscriptions.update_state = AsyncUiState::partial;
        message_ = "Subscription inventory refreshed; see per-source result for details";
        push_toast(Toast::Level::info, message_);
      }
    }
  }
  void push_toast(Toast::Level level, std::string text) {
    state_.toasts.push_back(Toast{level, std::move(text),
        std::chrono::steady_clock::now() + std::chrono::seconds(level == Toast::Level::error ? 8 : 4)});
    if (state_.toasts.size() > 4) state_.toasts.erase(state_.toasts.begin());
    log_poll_state_->animation.store(true);
  }
  void start_runtime_operation(std::string method, Json params, std::string label) {
    const bool is_stop = label == "Stop";
    if (!is_stop && (stop_pending_ || state_.runtime.operation_state == AsyncUiState::starting ||
        state_.runtime.operation_state == AsyncUiState::running)) {
      message_ = "Another runtime operation is already running";
      return;
    }
    if (is_stop) {
      if (stop_pending_) {
        message_ = "Stop is already in progress";
        return;
      }
      stop_pending_ = true;
    }
    state_.runtime.operation_state = AsyncUiState::starting;
    runtime_error_.reset();
    state_.runtime.operation_label = label;
    state_.runtime.spinner_phase = 0;
    message_ = label + " started";
    post_ui_event(ui_poster_);
    const auto socket = socket_;
    const auto mailbox = async_mailbox_;
    const auto poster = ui_poster_;
    auto worker = [socket, mailbox, poster, method = std::move(method),
                                   params = std::move(params), label = std::move(label)]() mutable {
      RuntimeOperationEvent event;
      event.label = label;
      auto accepted = rpc(socket, std::move(method), std::move(params));
      if (!accepted) event.error = accepted.error().message;
      else if (accepted->value("status", "") != "accepted" || !accepted->contains("operation_id"))
        event.error = "xrmd did not return an operation id";
      else {
        const auto operation = accepted->at("operation_id").get<std::string>();
        post_ui_event(poster);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        for (;;) {
          if (!poster->alive.load()) return;
          std::this_thread::sleep_for(std::chrono::milliseconds(180));
          if (!poster->alive.load()) return;
          post_ui_event(poster);
          auto current = rpc(socket, std::string(methods::operation_get),
                             Json{{"operation_id", operation}});
          if (!current) { event.error = current.error().message; break; }
          const auto state = current->value("status", "unknown");
          if (state == "running") {
            if (std::chrono::steady_clock::now() < deadline) continue;
            event.error = "Runtime operation timed out; xrmd may still be processing it";
            break;
          }
          if (state == "failed") {
            if (current->contains("error") && current->at("error").is_object()) {
              const auto& failure = current->at("error");
              event.error = failure.value("message", "runtime operation failed");
              if (failure.contains("subject") && failure["subject"].is_string())
                *event.error += " [" + failure["subject"].get<std::string>() + "]";
              if (failure.contains("hint") && failure["hint"].is_string())
                *event.error += " — " + failure["hint"].get<std::string>();
            } else event.error = "runtime operation failed";
          }
          break;
        }
      }
      if (!poster->alive.load()) return;
      auto status = rpc(socket, std::string(methods::runtime_status));
      if (status) event.status = std::move(*status);
      {
        std::lock_guard lock(mailbox->mutex);
        mailbox->runtime_operations.push_back(std::move(event));
      }
      post_ui_event(poster);
    };
    std::thread(std::move(worker)).detach();
  }
  void apply_runtime_operation(RuntimeOperationEvent event) {
    if (event.label == "Stop") stop_pending_ = false;
    if (event.status) { status_ = std::move(*event.status); refresh_proxy_entries(); }
    if (!event.error && event.label == "Connect" && status_.value("state", "") != "running")
      event.error = "xrmd completed the operation but the core is " + status_.value("state", "unknown");
    if (event.error) {
      state_.runtime.operation_state = AsyncUiState::failed;
      runtime_error_ = *event.error;
      if (runtime_error_->find("a selected target is required") != std::string::npos)
        *runtime_error_ += " — select a node in Nodes or a group in Groups, then Connect";
      message_ = event.label + " failed: " + *event.error;
      push_toast(Toast::Level::error, message_);
    } else {
      state_.runtime.operation_state = AsyncUiState::success;
      message_ = event.label == "Connect"
          ? "Xray core running; local proxy: " +
              (proxy_entries_.empty() ? "no inbound configured" : proxy_entries_.front()) +
              "; remote route not yet verified"
          : "core stopped";
      push_toast(Toast::Level::success, message_);
    }
  }
  void refresh_modules() {
    if (profile_id_.empty()) return;
    auto response = rpc(socket_, std::string(methods::module_list), Json{{"profile_id", profile_id_}});
    modules_.clear();
    if (response && response->is_array()) modules_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, modules_.empty() ? 0 : modules_.size() - 1);
  }
  void refresh_logs() {
    if (page_ == 3) log_poll_state_->cursor.store(log_cursor_);
  }
  void apply_logs(Json response) {
    if (response.value("truncated", false)) log_dropped_ = true;
    if (response.contains("entries") && response.at("entries").is_array()) {
      for (const auto& entry : response.at("entries")) {
        log_bytes_ += entry.value("text", std::string{}).size();
        log_entries_.push_back(entry);
      }
    }
    log_cursor_ = response.value("next_cursor", log_cursor_);
    log_poll_state_->cursor.store(log_cursor_);
    logs_ = std::move(response);
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
    editing_module_included_ = result->value("included", false);
    const auto text = result->find("text");
    editing_module_unconfigured_ = text == result->end() || text->is_null();
    if (editing_module_unconfigured_) {
      draft_ = Json{{editing_key_, Json::object()}}.dump(2) + "\n";
      editing_base_text_.reset();
      message_ = "module has no source file; draft only, not enabled";
    } else if (text->is_string()) {
      draft_ = text->get<std::string>();
      editing_base_text_ = draft_;
      message_ = editing_module_included_ ? "source loaded; Save changes only" : "source loaded; disabled module remains excluded";
    } else {
      message_ = "module source response has an invalid text value";
      return;
    }
    edit_cursor_ = draft_.size();
    state_.module_draft = draft_;
    editing_dirty_ = false;
    draft_json_invalid_ = false;
    editor_undo_.clear();
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
    remember_editor_state();
    draft_ = std::move(*edited);
    state_.module_draft = draft_;
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
    message_ = "saved source text; runtime inclusion is unchanged and no connect/restart was performed";
    editing_ = false;
    editing_dirty_ = false;
    editing_module_unconfigured_ = false;
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
    if (profile_id_.empty()) { connect_profile_prompt_ = true; return; }
    Json params{{"profile_id", profile_id_}};
    if (page_ == 1) {
      if (selected_ >= nodes_.size()) { message_ = "Select an available node before Connect"; return; }
      const auto& node = nodes_[selected_];
      if (!node.value("enabled", false) || node.value("source_missing", false)) {
        message_ = "This node is disabled or missing from its source; select an available node";
        return;
      }
      params["target"] = Json{{"kind", "node"}, {"id_or_tag", node.value("id", "")}};
    }
    start_runtime_operation(std::string(methods::connection_connect),
        std::move(params), "Connect");
  }
  void toggle_node_enabled() {
    if (selected_ >= nodes_.size()) { message_ = "Select a node first"; return; }
    const auto id = nodes_[selected_].value("id", "");
    const auto enabled = !nodes_[selected_].value("enabled", true);
    auto result = rpc(socket_, std::string(methods::node_set_enabled),
        Json{{"id", id}, {"enabled", enabled}});
    if (!result) message_ = "Cannot change node: " + result.error().message;
    else {
      message_ = enabled ? "Node enabled; running core unchanged" : "Node disabled; running core unchanged";
      refresh_nodes();
    }
  }
  void disconnect() {
    start_runtime_operation(std::string(methods::connection_disconnect), Json::object(), "Stop");
  }
  std::string active_profile() const {
    return status_.contains("active") && status_["active"].is_object()
        ? profile_name(status_["active"].value("profile_id", "")) : "—";
  }
  std::string active_confdir() const {
    return status_.contains("active") && status_["active"].is_object()
        ? status_["active"].value("confdir", "") : "—";
  }
  std::string actual_selection() const {
    const Json* actual = nullptr;
    if (status_.contains("actual_target") && status_["actual_target"].is_object())
      actual = &status_["actual_target"];
    else if (status_.contains("active") && status_["active"].is_object() &&
             status_["active"].contains("selection") && status_["active"]["selection"].is_object())
      actual = &status_["active"]["selection"];
    if (!actual) return "none";
    const auto& target = *actual;
    return display_target(target.value("kind", ""), target.value("id_or_tag", ""));
  }
  std::string expected_selection() const {
    if (expected_selection_.empty()) return "none";
    const auto separator = expected_selection_.find(':');
    if (separator == std::string::npos) return expected_selection_;
    return display_target(std::string_view(expected_selection_).substr(0, separator),
                          std::string_view(expected_selection_).substr(separator + 1));
  }
  std::string display_target(std::string_view kind, std::string_view id) const {
    if (kind == "node") {
      const auto found = std::find_if(nodes_.begin(), nodes_.end(), [&](const Json& node) {
        return node.value("id", "") == id;
      });
      if (found != nodes_.end()) return "node: " + found->value("name", std::string(id));
    } else if (kind == "group") {
      const auto found = std::find_if(groups_.begin(), groups_.end(), [&](const Json& group) {
        return group.value("id", "") == id;
      });
      if (found != groups_.end()) return "group: " + found->value("name", std::string(id));
    }
    return std::string(kind) + ": " + std::string(id);
  }

  std::string socket_;
  TuiState state_;
  std::string& profile_id_ = state_.viewed_profile_id;
  std::vector<Json> profiles_;
  std::string selected_core_for_profile_;
  std::string prompt_profile_id_;
  std::string prompt_subscription_name_;
  std::string prompt_label_;
  std::string prompt_text_;
  Prompt prompt_ = Prompt::none;
  ScreenInteractive* screen_;
  int& page_ = state_.page;
  std::size_t selected_ = 0;
  std::array<std::size_t, 9> page_selections_{};
  bool editing_ = false;
  bool filtering_nodes_ = false;
  bool node_detail_ = false;
  bool subscription_detail_ = false;
  bool editing_group_ = false;
  bool editing_node_ = false;
  bool editing_dirty_ = false;
  bool editing_module_included_ = false;
  bool editing_module_unconfigured_ = false;
  bool draft_json_invalid_ = false;
  std::optional<std::string> editing_base_text_;
  std::string editing_key_;
  std::string node_filter_;
  std::string draft_;
  std::size_t edit_cursor_ = 0;
  std::vector<EditorSnapshot> editor_undo_;
  std::vector<Json> all_nodes_;
  std::vector<Json> nodes_;
  std::size_t node_count_ = 0;
  std::size_t node_missing_count_ = 0;
  bool show_missing_nodes_ = false;
  std::vector<Json> subscriptions_;
  std::vector<Json> modules_;
  Json status_ = Json::object();
  std::vector<std::string> proxy_entries_;
  std::optional<std::string> runtime_error_;
  Json logs_ = Json::object();
  std::vector<Json> log_entries_;
  std::uint64_t log_cursor_ = 0;
  std::size_t log_bytes_ = 0;
  std::size_t log_view_start_ = 0;
  bool logs_paused_ = false;
  bool log_dropped_ = false;
  std::shared_ptr<LogPollState> log_poll_state_ = std::make_shared<LogPollState>();
  std::thread log_poller_;
  std::shared_ptr<AsyncMailbox> async_mailbox_ = std::make_shared<AsyncMailbox>();
  std::shared_ptr<UiEventPoster> ui_poster_ = std::make_shared<UiEventPoster>();
  bool stop_pending_ = false;
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
  bool connect_profile_prompt_ = false;
};

}  // namespace

int run_tui(const std::string& socket, const std::string& profile_id) {
  ScreenInteractive screen = ScreenInteractive::Fullscreen();
  auto app = Make<TuiApp>(socket, profile_id, screen);
  screen.Loop(app);
  return 0;
}

}  // namespace xrm::cli

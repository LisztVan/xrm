#include "cli/commands.hpp"
#include "cli/rpc_session.hpp"

#include "client/rpc_client.hpp"
#include "contract/methods.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <uv.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <map>
#include <optional>
#include <set>
#include <chrono>
#include <cstdint>
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
  int page;
};
// This is the visual order, numeric key order, and Tab order.
constexpr std::array<PageNav, 7> kPages{{
    {"Overview", "c Connect   i Local entry   d Stop   r Refresh   ? Help", 0},
    {"Profiles", "↑↓ Move   Enter Open   n New   r Rename   c Clone   d Delete   ? Help", 8},
    {"Modules", "↑↓ Move   Enter Edit Native   e Enable/Disable   g Routing Groups   i Local entry   ? Help", 2},
    {"Subscriptions", "↑↓ Move   Enter Details   a Add   b Use in Profile   u Update   h History   ? Help", 4},
    {"Nodes", "↑↓ Move   [ ] Category   / Search   g Groups   s Select   c Connect   ? Help", 1},
    {"Logs", "↑↓ History   p Pause/Resume   r Refresh   ? Help", 3},
    {"Config", "[ ] Section   Core: a Register   l Releases   i Install   u Use   ? Help", 7},
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

std::string inbound_text_field(const Json& inbound, std::string_view key) {
  const auto field = inbound.find(std::string(key));
  return field != inbound.end() && field->is_string() ? field->get<std::string>() : "";
}

int inbound_port(const Json& inbound) {
  const auto port = inbound.find("port");
  return port != inbound.end() && port->is_number_integer() ? port->get<int>() : 0;
}

bool local_socks_port_available() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  const int reuse = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(10808);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool available = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
  ::close(fd);
  return available;
}

std::string history_reason(std::string_view code) {
  if (code == "redundant_history") return "同来源、同名称和连接配置的历史冗余";
  if (code == "different_name_or_upstream_id") return "连接相同，但名称或来源身份不同；暂不处理";
  if (code == "ambiguous_current") return "有多个当前保留对象；暂不处理";
  if (code == "removed_from_source") return "来源已移除，当前无等价保留对象";
  return std::string(code);
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
  enum class Prompt { none, subscription_name, subscription_url, subscription_use_profile, core_path, core_remove_confirm,
                      profile_name, profile_clone_name, profile_rename_name, editor_argv,
                      node_protocol, node_name, node_address, node_port, node_auth, node_ss_method,
                      node_transport, node_security, node_server_name, node_reality_key,
                      node_uri, node_raw_name, node_raw_json, node_copy_name,
                      node_replace_json, node_remove_confirm, group_name, group_mode,
                      group_strategy, group_scope, group_remove_confirm };
  struct NodeDraft {
    std::string protocol, name, address, auth, method, transport = "raw", security = "none", server_name, reality_key;
    std::uint16_t port = 0;
  };
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
    bool metrics_due = false;
  };
  struct UiEventPoster {
    std::mutex mutex;
    ScreenInteractive* screen = nullptr;
    std::atomic_bool alive{true};
  };
  struct LogPollState {
    std::atomic_bool active{false};
    std::atomic_bool overview_active{false};
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
    refresh_program_settings();
    refresh_all();
    log_poll_state_->overview_active.store(page_ == 0);
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
      auto next_metrics_event = std::chrono::steady_clock::now() + std::chrono::seconds(2);
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
        if (log_poll_state->overview_active.load() && now >= next_metrics_event) {
          {
            std::lock_guard lock(log_mailbox->mutex);
            log_mailbox->metrics_due = true;
          }
          post_ui_event(log_poster);
          next_metrics_event = now + std::chrono::seconds(2);
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
      bool metrics_due = false;
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
        metrics_due = async_mailbox_->metrics_due;
        async_mailbox_->metrics_due = false;
      }
      if (subscription_running) state_.subscriptions.update_state = AsyncUiState::running;
      if (completed) apply_subscription_refresh(std::move(*completed));
      for (auto& result : runtime_completed) apply_runtime_operation(std::move(result));
      if (logs_completed) apply_logs(std::move(*logs_completed));
      if (metrics_due && page_ == 0) refresh_stats(true);
      const auto now = std::chrono::steady_clock::now();
      std::erase_if(state_.toasts, [&](const Toast& toast) { return toast.expires <= now; });
      log_poll_state_->animation.store(!appearance_draft_.value("reduce_motion", false) &&
          !state_.toasts.empty());
      return true;
    }
    if (filtering_nodes_) {
      if (event == Event::Escape) {
        node_filter_ = node_filter_before_edit_;
        filtering_nodes_ = false;
        return true;
      }
      if (event == Event::Return) {
        filtering_nodes_ = false;
        category_searches_[node_category_] = node_filter_;
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
    if (history_open_) {
      const auto& candidates = history_preview_.at("candidates");
      if (history_confirm_) {
        if (event == Event::Character("y")) confirm_history();
        else if (event == Event::Character("n") || event == Event::Escape) {
          history_confirm_ = false;
          message_ = "history cleanup cancelled; inventory unchanged";
        }
        return true;
      }
      if (event == Event::Escape || event == Event::Character("q")) {
        history_open_ = false;
        history_selected_ids_.clear();
        message_ = "history preview closed; inventory unchanged";
      } else if (event == Event::ArrowDown && !candidates.empty()) {
        history_index_ = std::min(history_index_ + 1, candidates.size() - 1);
      } else if (event == Event::ArrowUp && history_index_ > 0) --history_index_;
      else if (event == Event::Character(" ") && history_index_ < candidates.size()) {
        const auto& item = candidates[history_index_];
        if (!item.value("eligible", false)) message_ = "Cannot select: " + item.at("blockers").dump();
        else {
          const auto id = item.value("id", "");
          if (!history_selected_ids_.erase(id)) history_selected_ids_.insert(id);
        }
      } else if (event == Event::Character("c") && !history_selected_ids_.empty()) history_confirm_ = true;
      return true;
    }
    if (node_detail_) {
      if (event == Event::Escape || event == Event::CtrlQ) node_detail_ = false;
      else if (event == Event::Character("t"))
        node_field_index_ = (node_field_index_ + 1) % node_edit_fields_.size();
      else if (event == Event::Character("e")) open_node_editor();
      else if (event == Event::Character("k") && selected_ < nodes_.size()) {
        prompt_node_id_ = nodes_[selected_].value("id", "");
        begin_prompt(Prompt::node_copy_name, "Copy as Local node — new display name");
      } else if (event == Event::Character("E") && selected_ < nodes_.size()) {
        const auto& selected = nodes_[selected_];
        if (selected.contains("source_id") && !selected["source_id"].is_null())
          message_ = "Subscription nodes are edited at their source; copy to Local first";
        else {
          prompt_node_id_ = selected.value("id", "");
          auto detail = rpc(socket_, std::string(methods::node_get), Json{{"id", prompt_node_id_}});
          if (!detail) message_ = detail.error().message;
          else {
            begin_prompt(Prompt::node_replace_json, "Replace Local settings JSON; Enter saves, Esc cancels");
            prompt_text_ = detail->at("settings").dump();
          }
        }
      } else if (event == Event::Character("D") && selected_ < nodes_.size()) {
        prompt_node_id_ = nodes_[selected_].value("id", "");
        begin_prompt(Prompt::node_remove_confirm, "Delete unreferenced Local node? type DELETE");
      }
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
    if (add_outlet_menu_) {
      if (event == Event::Escape || event == Event::Character("q")) add_outlet_menu_ = false;
      else if (event == Event::Character("1")) {
        add_outlet_menu_ = false; node_draft_ = NodeDraft{};
        begin_prompt(Prompt::node_protocol, "Protocol: vless / vmess / trojan / shadowsocks / hysteria2");
      } else if (event == Event::Character("2")) {
        add_outlet_menu_ = false; begin_prompt(Prompt::node_uri, "Paste a supported share URI");
      } else if (event == Event::Character("3")) {
        add_outlet_menu_ = false; begin_prompt(Prompt::node_raw_name, "Native outbound display name");
      } else if (event == Event::Character("4")) {
        add_outlet_menu_ = false;
        if (profile_id_.empty()) message_ = "Create or open a Profile before adding a Group";
        else begin_prompt(Prompt::group_name, "Group name");
      }
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
        page_ = 8;
        begin_profile_prompt(false);
      }
      return true;
    }
    if (local_entry_confirm_) {
      if (event == Event::Character("y")) add_local_entry();
      else if (event == Event::Character("n") || event == Event::Escape) {
        local_entry_confirm_ = false;
        message_ = "local proxy entry unchanged";
      }
      return true;
    }
    if (page_ == 7 && config_section_ == 1 && event == Event::Escape && appearance_dirty_) {
      appearance_draft_ = program_settings_.value("appearance", Json::object());
      appearance_dirty_ = false;
      message_ = "Appearance preview cancelled; saved settings unchanged";
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
    if (page_ == 7 && config_section_ == 1 && event == Event::Character("s")) {
      save_appearance(); return true;
    }
    if (page_ == 7 && config_section_ == 1 && event.is_character()) {
      const auto key = event.character();
      if (key == "t") { cycle_appearance("theme", {"default", "dark", "light"}); return true; }
      if (key == "c") { cycle_appearance("color_mode", {"auto", "truecolor", "256", "none"}); return true; }
      if (key == "d") { cycle_appearance("density", {"comfortable", "compact"}); return true; }
      if (key == "i") { cycle_appearance("icon_mode", {"unicode", "ascii"}); return true; }
      if (key == "m") {
        appearance_draft_["reduce_motion"] = !appearance_draft_.value("reduce_motion", false);
        appearance_dirty_ = true; return true;
      }
    }
    if (page_ == 7 && config_section_ == 2 && event == Event::Character("e")) {
      begin_prompt(Prompt::editor_argv, "Editor argv as JSON array, e.g. [\"/usr/bin/vi\"]"); return true;
    }
    if (page_ == 7 && config_section_ == 2 && event == Event::Character("t")) {
      test_configured_editor(); return true;
    }
    if (page_ == 8 && event == Event::Character("n")) { begin_profile_prompt(false); return true; }
    if (page_ == 8 && event == Event::Character("c")) { begin_profile_prompt(true); return true; }
    if (page_ == 8 && event == Event::Character("r")) { begin_profile_prompt(false, true); return true; }
    if (page_ == 8 && event == Event::Character("d")) { remove_selected_profile(); return true; }
    if (event == Event::Tab) { navigate_top_page((top_page_index() + 1) % kPages.size()); return true; }
    if (event == Event::TabReverse) { navigate_top_page((top_page_index() + kPages.size() - 1) % kPages.size()); return true; }
    if (event.is_character() && event.character().size() == 1 &&
        event.character()[0] >= '1' && event.character()[0] <= '7') {
      navigate_top_page(event.character()[0] - '1');
      return true;
    }
    if (page_ == 1 && event == Event::Character("g")) { navigate_page(5); return true; }
    if (page_ == 5 && event == Event::Character("n")) { node_category_ = "all"; navigate_page(1); return true; }
    if ((page_ == 1 || page_ == 5) && event == Event::Character("[")) {
      move_node_category(-1); return true;
    }
    if ((page_ == 1 || page_ == 5) && event == Event::Character("]")) {
      move_node_category(1); return true;
    }
    if (page_ == 7 && event == Event::Character("[")) {
      config_section_ = (config_section_ + 3) % 4; return true;
    }
    if (page_ == 7 && event == Event::Character("]")) {
      config_section_ = (config_section_ + 1) % 4; return true;
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
    else if (event == Event::Return && page_ == 7 && config_section_ == 0 && selected_ < cores_.size()) {
      selected_core_for_profile_ = cores_[selected_].value("id", "");
      message_ = "core selected for new Profile; installation/use state is unchanged";
    }
    else if (event == Event::Return && page_ == 8 && selected_ < profiles_.size()) select_profile_context();
    else if (event == Event::Character("e") && page_ == 2) toggle_module();
    else if (event == Event::Character("g") && page_ == 2 && selected_ < modules_.size() &&
             modules_[selected_].value("key", "") == "routing") {
      node_category_ = "groups";
      navigate_page(5);
      message_ = "Dynamic Groups share this Profile's routing output; static native balancers remain in routing source";
    }
    else if (event == Event::Character("s") && page_ == 1) select_node();
    else if (event == Event::Character("x") && page_ == 1) toggle_node_enabled();
    else if (event == Event::Character("s") && page_ == 5) select_group();
    else if (event == Event::Character("m") && page_ == 5) choose_group_member();
    else if (event == Event::Character("D") && page_ == 5 && selected_ < groups_.size()) {
      prompt_group_id_ = groups_[selected_].value("id", "");
      begin_prompt(Prompt::group_remove_confirm, "Remove this Group? type DELETE");
    }
    else if (event == Event::Character("m") && page_ == 1) select_group_member();
    else if (event == Event::Character("c") && profile_id_.empty()) connect_profile_prompt_ = true;
    else if (event == Event::Character("c") && !profile_id_.empty()) connect();
    else if (event == Event::Character("d")) disconnect();
    else if (event == Event::Character("r")) {
      if (page_ == 7 && config_section_ == 0) {
        if (!pending_core_operation_.empty()) poll_core_operation();
        else refresh_cores();
      } else if (page_ == 3) refresh_logs();
      else refresh_all();
      if (page_ == 0) refresh_stats(true);
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
      node_filter_before_edit_ = node_filter_;
      node_filter_.clear();
      filtering_nodes_ = true;
    }
    else if (event == Event::Character("u") && page_ == 4) refresh_subscription_inventory();
    else if (event == Event::Character("b") && page_ == 4) toggle_subscription_binding();
    else if (event == Event::Character("h") && page_ == 4) open_history();
    else if (event == Event::Character("i") && (page_ == 0 || page_ == 2)) inspect_local_entry();
    else if (event == Event::Character("a") && page_ == 4) begin_prompt(Prompt::subscription_name, "Subscription name");
    else if (event == Event::Character("a") && (page_ == 1 || page_ == 5)) add_outlet_menu_ = true;
    else if (event == Event::Character("a") && page_ == 7 && config_section_ == 0) begin_prompt(Prompt::core_path, "Absolute path to an existing Xray executable");
    else if (event == Event::Character("D") && page_ == 7 && config_section_ == 0 &&
             !viewing_releases_ && selected_ < cores_.size()) {
      prompt_core_id_ = cores_[selected_].value("id", "");
      begin_prompt(Prompt::core_remove_confirm, "Remove unused managed core? type DELETE");
    }
    else if (event == Event::Character("l") && page_ == 7 && config_section_ == 0) fetch_core_releases(false);
    else if (event == Event::Character("p") && page_ == 7 && config_section_ == 0) fetch_core_releases(true);
    else if (event == Event::Character("i") && page_ == 7 && config_section_ == 0) install_selected_release();
    else if (event == Event::Character("u") && page_ == 7 && config_section_ == 0) use_selected_core();
    return true;
  }

  Element OnRender() override {
    const auto theme = appearance_draft_.value("theme", "default");
    const auto accent = theme == "light" ? Color::Blue :
        theme == "dark" ? Color::Cyan : Color::Green;
    const bool use_colors = appearance_draft_.value("color_mode", "auto") != "none";
    const bool compact_density = appearance_draft_.value("density", "comfortable") == "compact";
    Elements tabs;
    for (std::size_t i = 0; i < kPages.size(); ++i) {
      const bool current = i == top_page_index();
      auto label = std::to_string(i + 1) + " " + kPages[i].title;
      auto tab = text(current ? "[" + label + "] " : " " + label + " ") |
          (current ? bold : nothing) | (current ? inverted : nothing);
      if (current && use_colors) tab = tab | color(accent);
      if (current) tab = tab | focus;
      tabs.push_back(std::move(tab));
    }
    Elements header{
      text(" XRM · Current: " + std::string(kPages[top_page_index()].title)) | bold |
          (use_colors ? color(accent) : nothing),
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
        body.push_back(text("First use: 7 Config/Core register Xray → 2 Profiles create → 4 Subs add/update → 5 Nodes") | color(Color::Yellow));
      body.push_back(text("Viewing Profile: " + (profile_id_.empty() ? "No profile selected" : profile_name(profile_id_)) +
          "   |   Running Profile: " + active_profile()) | bold);
      const auto core_version = status_.contains("active") && status_["active"].is_object()
          ? status_["active"].value("core_version", "unknown") : "not running";
      body.push_back(text("CORE  " + status_.value("state", "unknown") + " · " +
          status_.value("health", "unknown") + " · " +
          core_version.substr(0, core_version.find(" ("))) | bold | color(Color::Cyan));
      body.push_back(text("LOCAL PROXY  " +
          (proxy_entries_.empty() ? "No entry in running configuration" : proxy_entries_.front())) | bold);
      if (!profile_id_.empty())
        body.push_back(text("LOCAL-USE SETUP  " + local_entry_status_ + " · i Inspect/add") |
            color(Color::Yellow));
      if (state_.runtime.operation_state == AsyncUiState::starting ||
          state_.runtime.operation_state == AsyncUiState::running) {
        static constexpr std::array<const char*, 4> spinner{"⠋", "⠙", "⠹", "⠸"};
        body.push_back(text(std::string(spinner[state_.runtime.spinner_phase % spinner.size()]) + " " +
            state_.runtime.operation_label + "…") | color(Color::Cyan) | bold);
      }
      body.push_back(text("Saved target: " + expected_selection()) | bold);
      body.push_back(text("Actual target: " + actual_selection()) | bold);
      body.push_back(text("Runtime override: " + status_.value("override_state", "unknown") +
          (status_.contains("runtime_override") && status_["runtime_override"].is_string()
              ? " " + status_["runtime_override"].get<std::string>() : "")));
      body.push_back(text("Inventory: " + std::to_string(subscriptions_.size()) +
          " subscriptions · " + std::to_string(node_count_) + " nodes"));
      body.push_back(text("Statistics · traffic & observation") | bold | color(Color::Cyan));
      if (stats_.value("availability", "unknown") != "available" ||
          !stats_.contains("sample") || !stats_["sample"].is_object()) {
        body.push_back(text("Traffic: unavailable · latency: unmeasured · sample: " +
            stats_.value("availability", "unknown")) | color(Color::Yellow));
      } else {
        const auto& sample = stats_["sample"];
        body.push_back(text("Sample: " +
            (sample.contains("received_at") ? sample["received_at"].dump() : "pending")) | dim);
        if (sample.contains("traffic") && sample["traffic"].is_array() &&
            !sample["traffic"].empty()) {
          const auto& counters = sample["traffic"];
          const auto counter = std::find_if(counters.begin(), counters.end(), [](const Json& item) {
            return (item.contains("uplink") && item["uplink"].is_number_unsigned() &&
                    item["uplink"].get<std::uint64_t>() > 0) ||
                   (item.contains("downlink") && item["downlink"].is_number_unsigned() &&
                    item["downlink"].get<std::uint64_t>() > 0);
          });
          const auto& shown = counter == counters.end() ? counters.front() : *counter;
          body.push_back(text("Cumulative " + shown.value("scope", "") + "/" +
              shown.value("tag", "") + "  ↓ " +
              (shown.contains("downlink") && !shown["downlink"].is_null()
                  ? shown["downlink"].dump() + " B" : "unavailable") + "  ↑ " +
              (shown.contains("uplink") && !shown["uplink"].is_null()
                  ? shown["uplink"].dump() + " B" : "unavailable")));
        } else body.push_back(text("Cumulative traffic: unavailable"));
        if (stats_.contains("rates") && stats_["rates"].is_array() && !stats_["rates"].empty()) {
          const auto& rate = stats_["rates"].front();
          body.push_back(text("↓ " + (rate.contains("downlink_bytes_per_second") && !rate["downlink_bytes_per_second"].is_null()
                  ? rate["downlink_bytes_per_second"].dump() + " B/s" : "pending") +
              "   ↑ " + (rate.contains("uplink_bytes_per_second") && !rate["uplink_bytes_per_second"].is_null()
                  ? rate["uplink_bytes_per_second"].dump() + " B/s" : "pending")));
        } else body.push_back(text("Rates: pending second valid sample"));
        if (sample.contains("observations") && sample["observations"].is_array() &&
            !sample["observations"].empty()) {
          const auto& observation = sample["observations"].front();
          body.push_back(text("Latency: " +
              (observation.contains("latency_ms") && !observation["latency_ms"].is_null()
                  ? observation["latency_ms"].dump() + " ms" : "unavailable")));
        } else body.push_back(text("Latency: unmeasured"));
      }
      body.push_back(text("Runtime files: " + active_confdir()) | dim);
      if (!compact_density) {
        body.push_back(text("Core running is separate from proxy listener readiness and remote connectivity") | dim);
        body.push_back(text("Pending changes: saved edits apply only on explicit Connect/Apply") | dim);
      }
      body.push_back(text("c Connect   d Stop   r Refresh   q Exit (core continues)") | dim);
      if (profile_id_.empty()) {
        body.push_back(text("No profile selected. Subscriptions, Nodes, Cores and Profiles are available without one.") | color(Color::Yellow));
        body.push_back(text("Press c to Create Default Profile, Open Profiles, or Cancel.") | color(Color::Yellow));
      }
    } else if (page_ == 1) {
      body.push_back(node_category_tabs());
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
        body.push_back(text("k Copy to Local   E Replace Local settings   D Delete Local (confirmation)") | dim);
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
      const bool ascii_icons = appearance_draft_.value("icon_mode", "unicode") == "ascii";
      const std::string marker = actual ? (ascii_icons ? "*" : "●") :
          saved ? (ascii_icons ? "+" : "◆") :
          node.value("source_missing", false) ? (ascii_icons ? "x" : "×") :
          node.value("enabled", true) ? (ascii_icons ? "." : "·") :
          (ascii_icons ? "o" : "○");
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
      if (!compact_density)
        body.push_back(text("routing contains native rules/static balancers; select routing and press g to edit dynamic Groups in Nodes") | dim);
      if (profile_id_.empty()) {
        body.push_back(text("No profile selected") | color(Color::Yellow));
        body.push_back(text("Create a profile to use this feature") | color(Color::Yellow));
      } else {
        body.push_back(text("i Inspect local proxy entry; any change requires confirmation") | dim);
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
            "  auto:" + (source.value("auto_refresh", false) ? "on" : "off") +
            "  profile:" + (profile_id_.empty() ? "—" :
                source_enabled_for_view(source.value("id", "")) ? "on" : "off") +
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
      body.push_back(text("b Toggle use for viewed Profile; auto-refresh is a separate source setting") | dim);
      body.push_back(text("h Preview historical/duplicate nodes for the selected source (no changes)") | dim);
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
      body.push_back(node_category_tabs());
      body.push_back(text("Groups — Enter edits binding; s saves as target; m chooses manual member; D removes") | bold);
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
      static constexpr std::array<const char*, 4> sections{"Core", "Appearance", "Editor", "General"};
      body.push_back(text("Config — XRM settings and Xray core resources") | bold);
      Elements section_tabs;
      for (std::size_t i = 0; i < sections.size(); ++i)
        section_tabs.push_back(text(i == config_section_ ? "[" + std::string(sections[i]) + "] " :
            " " + std::string(sections[i]) + " ") |
            (i == config_section_ ? inverted : nothing));
      body.push_back(hbox(std::move(section_tabs)) | xframe);
      if (config_section_ != 0) {
        body.push_back(text(std::string(sections[config_section_]) + " settings") | bold);
        if (config_section_ == 1) {
          const auto field = [&](std::string_view key) {
            if (appearance_draft_.contains(std::string(key)))
              return appearance_draft_.at(std::string(key)).dump();
            return std::string("unavailable");
          };
          body.push_back(text("Theme [t] " + field("theme") + "   Color mode [c] " + field("color_mode")));
          body.push_back(text("Density [d] " + field("density") + "   Icons [i] " + field("icon_mode")));
          body.push_back(text("Reduce motion [m] " + field("reduce_motion")));
          body.push_back(text(appearance_dirty_ ? "Previewing unsaved appearance · s Save · Esc Cancel" :
              "t/c/d/i/m Preview   s Save   [ ] Section") |
              (appearance_dirty_ ? color(Color::Yellow) : dim));
          body.push_back(text("Font family and size remain terminal settings; XRM does not change them") | dim);
        } else if (config_section_ == 2) {
          const auto argv = program_settings_.value("editor", Json::object()).value("argv", Json::array());
          body.push_back(text("Explicit editor argv: " + (argv.empty() ? "not configured" : argv.dump())));
          body.push_back(text("Manager VISUAL: " + std::string(std::getenv("VISUAL") ? std::getenv("VISUAL") : "not set")));
          body.push_back(text("Manager EDITOR: " + std::string(std::getenv("EDITOR") ? std::getenv("EDITOR") : "not set")));
          body.push_back(text("Effective source: " + std::string(!argv.empty() ? "saved argv" :
              std::getenv("VISUAL") && *std::getenv("VISUAL") ? "VISUAL" :
              std::getenv("EDITOR") && *std::getenv("EDITOR") ? "EDITOR" : "unconfigured")));
          body.push_back(text("e Set argv (JSON array)   t Test with a temporary draft; Manager host executes it") | dim);
        } else {
          body.push_back(text("Server socket: " + socket_));
          body.push_back(text("Server: " + status_.value("state", "unknown")));
          body.push_back(text("Only existing adjustable XRM preferences are shown; Xray modules remain under Modules") | dim);
        }
        body.push_back(text("[ / ] Change Config section") | dim);
      } else {
      body.push_back(text("Core — system binaries are register-only; managed binaries live under XRM data") | bold);
      body.push_back(text("Viewing Profile: " +
          (profile_id_.empty() ? std::string("none — open Profiles before choosing a core") :
              profile_name(profile_id_)) +
          " · u Use selected core for this Profile") | dim);
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
          const bool selected_here = !profile_id_.empty() &&
              profile_core_id(profile_id_) == core.value("id", "");
          const bool selected_elsewhere = std::any_of(profiles_.begin(), profiles_.end(),
              [&](const Json& profile) {
                return profile.value("id", "") != profile_id_ &&
                    profile.value("core_id", "") == core.value("id", "");
              });
          body.push_back(text((i == selected_ ? "> " : "  ") + core.value("name", "") + "  " +
              core.value("reported_version", "") + "  " +
              (core.value("managed", false) ? "managed" : "system") + "  installed" +
              (selected_here ? " selected for viewed Profile" :
                  selected_elsewhere ? " used by another Profile" : "") +
              (running == core.value("id", "") ? " running" : "")) |
              (i == selected_ ? inverted : nothing));
          body.push_back(text(core.value("path", "")) | dim);
        }
        body.push_back(text("a register   Enter choose for new Profile   l releases   i install   u use here   D remove unused managed") | dim);
      }
      body.push_back(text("r refresh/poll operation; install never changes the running core") | dim);
      }
    } else if (page_ == 8) {
      body.push_back(text("Profiles — Enter switch browsing context; n create; c copy; r rename; d delete") | bold);
      for (std::size_t i = 0; i < profiles_.size(); ++i) {
        const auto& profile = profiles_[i];
        body.push_back(text((i == selected_ ? "> " : "  ") + profile.value("name", "") +
            (profile.value("id", "") == profile_id_ ? "  editing" : "") +
            (profile.value("id", "") == active_profile_id() ? "  running" : "")) |
            (i == selected_ ? inverted : nothing));
      }
      if (selected_ < profiles_.size()) {
        const auto& profile = profiles_[selected_];
        const auto core_id = profile.value("core_id", "");
        std::string core_name = "Not selected — Config → Core";
        for (const auto& core : cores_)
          if (core.value("id", "") == core_id) { core_name = core.value("name", "Xray"); break; }
        body.push_back(text("Plan: standard local proxy · Core: " + core_name) | bold);
        if (profile.value("id", "") == profile_id_) {
          const auto bound = selected_profile_sources();
          body.push_back(text("Sources used here: " + std::to_string(bound.size()) +
              " · Native modules: " + std::to_string(modules_.size()) +
              " · one Server / one production Xray process"));
        } else body.push_back(text("Enter to open this Profile and inspect its sources/modules") | dim);
      }
      if (profiles_.empty()) body.push_back(text("No Profile. Press n for a local template; choose a core in Config → Core before Connect.") | color(Color::Yellow));
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
    std::string help_bar = std::string(kPages[top_page_index()].help) +
        "   Tab/Shift-Tab Pages   q Quit";
    if (page_ == 5) help_bar = "Nodes/Groups: ↑↓ Move   Enter Edit   s Select   m Choose Member   n Nodes   ? Help";
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
                    text("1–7              open a page directly"),
                    text("↑ / ↓            move through a list"),
                    text("Enter            view or open selected item"),
                    text("c                connect   d stop   r refresh"),
                    text("Logs: p pause/resume; arrows browse history"),
                    text("Module editor: F6/v external editor; F2 save"),
                    text("q quit; core continues   Esc or ? close")})) | center});
    }
    if (history_open_) {
      Elements rows{text("History preview · " + history_preview_.value("source_name", "") +
          " · current " + std::to_string(history_preview_.value("current_count", 0)) +
          " · historical " + std::to_string(history_preview_.value("history_count", 0))) | bold,
          text("Space selects safe records; blocked records stay untouched") | dim};
      const auto& candidates = history_preview_.at("candidates");
      const auto visible = static_cast<std::size_t>(std::max(3, std::min(8, screen_->dimy() - 18)));
      const auto start = history_index_ < visible ? 0 : history_index_ - visible + 1;
      const auto end = std::min(candidates.size(), start + visible);
      rows.push_back(text("Candidates " + std::to_string(start + (candidates.empty() ? 0 : 1)) +
          "-" + std::to_string(end) + "/" + std::to_string(candidates.size())) | dim);
      for (std::size_t i = start; i < end; ++i) {
        const auto& item = candidates[i];
        const auto id = item.value("id", "");
        const auto marker = history_selected_ids_.contains(id) ? "[x] " : "[ ] ";
        auto line = text(std::string(i == history_index_ ? "▸ " : "  ") + marker +
            short_utf8(item.value("name", ""), 28) + " → keep " +
            short_utf8(item.value("keep_name", "none"), 24) +
            (item.value("eligible", false) ? " · 可处理" : " · 暂不处理"));
        if (i == history_index_) line = line | inverted | focus;
        rows.push_back(std::move(line));
      }
      if (history_index_ < candidates.size()) {
        const auto& item = candidates[history_index_];
        rows.push_back(paragraph("Source: " + item.value("source_name", "") +
            " · record: historical/source_missing · keep: " + item.value("keep_status", "none")));
        rows.push_back(paragraph("Reason: " + history_reason(item.value("reason", ""))));
        rows.push_back(paragraph("Blockers: " + item.value("blockers", Json::array()).dump() +
            " · references: " + item.value("references", Json::array()).dump()));
      }
      rows.push_back(text("↑↓ Move   Space Select   c Confirm selected   Esc Cancel") | dim);
      auto dialog = window(text("History cleanup") | bold, vbox(std::move(rows)) |
          size(WIDTH, LESS_THAN, std::max(40, screen_->dimx() - 4)) |
          size(HEIGHT, LESS_THAN, std::max(8, screen_->dimy() - 4))) | center;
      if (history_confirm_)
        dialog = window(text("Confirm deletion") | bold,
            vbox({text("Remove " + std::to_string(history_selected_ids_.size()) +
                       " selected safe historical records?"),
                  text("No profile, module, or running core will change."),
                  text("y Confirm   n/Esc Cancel") | dim})) | center;
      return dbox({base | dim, std::move(dialog)});
    }
    if (local_entry_confirm_) return dbox({base | dim,
        window(text("Add local proxy entry") | bold,
            vbox({text("Profile: " + profile_name(profile_id_)),
                  text("Append SOCKS 127.0.0.1:10808 to modules/02_inbounds.json"),
                  text("Existing inbounds are preserved; no system proxy/DNS changes."),
                  text("Save source only; explicit Connect/Apply required."),
                  text("y Confirm   n/Esc Cancel") | dim})) | center});
    if (add_outlet_menu_) return dbox({base | dim,
        window(text("Add outlet") | bold,
            vbox({text("1 Manual node — typed protocol form"),
                  text("2 Import share URI"),
                  text("3 Custom native Xray outbound"),
                  text("4 Group — routing balancer binding"),
                  text("Esc Cancel") | dim})) | center});
    return base;
  }

 private:
  std::size_t top_page_index() const {
    if (page_ == 5) return 4;  // Groups are a Nodes category.
    if (page_ == 6) return 0;  // Legacy Stats content belongs to Overview.
    for (std::size_t i = 0; i < kPages.size(); ++i)
      if (kPages[i].page == page_) return i;
    return 0;
  }
  void navigate_top_page(std::size_t index) {
    if (index < kPages.size()) navigate_page(kPages[index].page);
  }
  bool source_enabled_for_view(std::string_view id) const {
    if (profile_id_.empty() || !profile_settings_.is_object() ||
        !profile_settings_.contains("enabled_source_ids") ||
        profile_settings_["enabled_source_ids"].is_null()) return true;
    const auto& enabled = profile_settings_["enabled_source_ids"];
    if (!enabled.is_array()) return false;
    return std::any_of(enabled.begin(), enabled.end(), [&](const Json& value) {
      return value.is_string() && value.get<std::string>() == id;
    });
  }
  std::vector<std::pair<std::string, std::string>> node_categories() const {
    std::vector<std::pair<std::string, std::string>> items{{"all", "All Subscriptions"}};
    for (const auto& source : subscriptions_)
      items.emplace_back("source:" + source.value("id", ""), source.value("name", "Subscription"));
    items.emplace_back("local", "Local");
    items.emplace_back("groups", "Groups");
    return items;
  }
  Element node_category_tabs() const {
    Elements tabs;
    for (const auto& [key, label] : node_categories()) {
      auto item = text(key == node_category_ ? "[" + short_utf8(label, 18) + "] " :
          " " + short_utf8(label, 18) + " ");
      if (key == node_category_) item = item | inverted;
      tabs.push_back(std::move(item));
    }
    return hbox(std::move(tabs)) | xframe;
  }
  void move_node_category(int direction) {
    if (page_ == 1 && selected_ < nodes_.size())
      category_highlights_[node_category_] = nodes_[selected_].value("id", "");
    category_searches_[node_category_] = node_filter_;
    const auto categories = node_categories();
    auto found = std::find_if(categories.begin(), categories.end(), [&](const auto& item) {
      return item.first == node_category_;
    });
    const auto index = found == categories.end() ? 0 : static_cast<std::size_t>(found - categories.begin());
    const auto next = direction < 0 ? (index + categories.size() - 1) % categories.size() :
        (index + 1) % categories.size();
    node_category_ = categories[next].first;
    node_filter_ = category_searches_.contains(node_category_) ?
        category_searches_[node_category_] : std::string{};
    state_.highlighted_node_id = category_highlights_.contains(node_category_) ?
        std::optional<std::string>(category_highlights_[node_category_]) : std::nullopt;
    selected_ = 0;
    navigate_page(node_category_ == "groups" ? 5 : 1);
  }
  std::size_t page_size() const {
    if (page_ == 1) return nodes_.size();
    if (page_ == 2) return modules_.size();
    if (page_ == 4) return subscriptions_.size();
    if (page_ == 5) return groups_.size();
    if (page_ == 7) return config_section_ == 0 ?
        (viewing_releases_ ? core_releases_.size() : cores_.size()) : 0;
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
  void refresh_program_settings() {
    auto response = rpc(socket_, std::string(methods::settings_get));
    if (!response || !response->is_object()) {
      message_ = response ? "Cannot read XRM settings" : response.error().message;
      return;
    }
    program_settings_ = std::move(*response);
    if (!appearance_dirty_) appearance_draft_ = program_settings_.value("appearance", Json::object());
  }
  void cycle_appearance(std::string_view key, std::initializer_list<const char*> values) {
    if (!appearance_draft_.is_object()) appearance_draft_ = Json::object();
    const std::vector<std::string> choices(values.begin(), values.end());
    const auto current = appearance_draft_.value(std::string(key), choices.front());
    auto found = std::find(choices.begin(), choices.end(), current);
    const auto next = found == choices.end() || std::next(found) == choices.end() ?
        choices.front() : *std::next(found);
    appearance_draft_[std::string(key)] = next;
    appearance_dirty_ = true;
  }
  void save_appearance() {
    if (!appearance_dirty_) { message_ = "Appearance is unchanged"; return; }
    auto result = rpc(socket_, std::string(methods::settings_update),
        Json{{"patch", {{"appearance", appearance_draft_}}}});
    if (!result) { message_ = "Appearance save failed: " + result.error().message; return; }
    program_settings_ = std::move(*result);
    appearance_draft_ = program_settings_.value("appearance", Json::object());
    appearance_dirty_ = false;
    message_ = "Appearance saved; Xray configuration and running core unchanged";
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
  void save_prompted_subscription(bool use_for_profile) {
    auto added = rpc(socket_, std::string(methods::subscription_add),
        Json{{"name", prompt_subscription_name_}, {"url", prompt_subscription_url_}});
    prompt_subscription_name_.clear();
    prompt_subscription_url_.clear();
    if (!added) { message_ = "Subscription add failed: " + added.error().message; return; }
    const auto id = added->value("id", "");
    refresh_subscriptions();
    if (use_for_profile && !profile_id_.empty()) {
      auto ids = selected_profile_sources();
      ids.push_back(id);
      auto saved = rpc(socket_, std::string(methods::profile_sources_set),
          Json{{"profile_id", profile_id_}, {"source_ids", ids}});
      if (!saved) { message_ = "Source added globally, but Profile binding failed: " + saved.error().message; return; }
      refresh_expected();
    }
    message_ = use_for_profile ?
        "Subscription saved and used by viewed Profile; press u to update inventory" :
        "Subscription saved globally; press u to update inventory";
  }
  std::vector<Id> selected_profile_sources() const {
    std::vector<Id> ids;
    if (profile_settings_.contains("enabled_source_ids") &&
        profile_settings_["enabled_source_ids"].is_array()) {
      for (const auto& item : profile_settings_["enabled_source_ids"])
        if (item.is_string()) ids.push_back(item.get<std::string>());
    } else {
      for (const auto& source : subscriptions_) ids.push_back(source.value("id", ""));
    }
    ids.erase(std::remove(ids.begin(), ids.end(), ""), ids.end());
    return ids;
  }
  void toggle_subscription_binding() {
    if (profile_id_.empty()) { message_ = "Create or open a Profile before binding a source"; return; }
    if (selected_ >= subscriptions_.size()) { message_ = "Select a source first"; return; }
    const auto id = subscriptions_[selected_].value("id", "");
    auto ids = selected_profile_sources();
    const bool was_enabled = std::find(ids.begin(), ids.end(), id) != ids.end();
    if (was_enabled) ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    else ids.push_back(id);
    auto saved = rpc(socket_, std::string(methods::profile_sources_set),
        Json{{"profile_id", profile_id_}, {"source_ids", ids}});
    if (!saved) { message_ = "Profile source binding failed: " + saved.error().message; return; }
    refresh_expected();
    rebuild_node_view();
    message_ = std::string(was_enabled ? "Source excluded from" : "Source included in") +
        " viewed Profile; running files and PID are unchanged until Connect/Apply";
  }
  void finish_manual_node() {
    Json protocol{{"kind", node_draft_.protocol}};
    if (node_draft_.protocol == "vless")
      protocol.update(Json{{"id", node_draft_.auth}, {"encryption", nullptr}, {"flow", nullptr}, {"level", nullptr}});
    else if (node_draft_.protocol == "vmess")
      protocol.update(Json{{"id", node_draft_.auth}, {"security", nullptr}, {"level", nullptr}});
    else if (node_draft_.protocol == "shadowsocks")
      protocol.update(Json{{"method", node_draft_.method}, {"password", node_draft_.auth}});
    else protocol[node_draft_.protocol == "trojan" ? "password" : "auth"] = node_draft_.auth;
    Json transport{{"kind", node_draft_.transport}};
    if (node_draft_.transport == "websocket") transport.update(Json{{"path", nullptr}, {"headers", Json::object()}});
    else if (node_draft_.transport == "xhttp")
      transport.update(Json{{"host", nullptr}, {"path", nullptr}, {"mode", nullptr}, {"extra", nullptr}});
    else if (node_draft_.transport == "grpc")
      transport.update(Json{{"service_name", nullptr}, {"authority", nullptr}, {"multi_mode", false}});
    else if (node_draft_.transport == "httpupgrade")
      transport.update(Json{{"host", nullptr}, {"path", nullptr}});
    else if (node_draft_.transport == "hysteria") transport["options"] = Json::object();
    Json security{{"kind", node_draft_.security}};
    const Json server_name = node_draft_.server_name.empty() ? Json(nullptr) : Json(node_draft_.server_name);
    if (node_draft_.security == "tls")
      security.update(Json{{"server_name", server_name}, {"fingerprint", nullptr},
          {"alpn", Json::array()}, {"options", Json::object()}});
    else if (node_draft_.security == "reality")
      security.update(Json{{"server_name", server_name}, {"fingerprint", nullptr},
          {"password", node_draft_.reality_key}, {"short_id", nullptr},
          {"spider_x", nullptr}, {"mldsa65_verify", nullptr}, {"options", Json::object()}});
    Json settings{{"kind", "stream"},
        {"endpoint", {{"address", node_draft_.address}, {"port", node_draft_.port}}},
        {"protocol", std::move(protocol)}, {"transport", std::move(transport)},
        {"security", std::move(security)}, {"sockopt", nullptr}, {"finalmask", nullptr}, {"mux", nullptr}};
    auto added = rpc(socket_, std::string(methods::node_add),
        Json{{"name", node_draft_.name}, {"settings", std::move(settings)}});
    if (!added) { message_ = "Manual node was not saved: " + added.error().message; return; }
    node_category_ = "local";
    navigate_page(1);
    message_ = "Manual node saved to Local; protocol UUID and XRM Node ID are separate";
  }
  void save_group_from_scope(std::string_view scope) {
    if (profile_id_.empty()) { message_ = "Open a Profile before saving a Group"; return; }
    Json members{{"kind", "filter"}, {"sources", Json::array()},
        {"explicit_node_ids", Json::array()}, {"include_local", false},
        {"protocols", Json::array()}, {"name_contains", Json::array()}, {"labels", Json::array()}};
    const bool all = scope == "all" || scope.empty();
    bool local = false;
    bool selected = false;
    if (!all) {
      std::string remaining(scope);
      std::replace(remaining.begin(), remaining.end(), '+', ',');
      std::istringstream input(remaining);
      std::string token;
      while (std::getline(input, token, ',')) {
        const auto first = token.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        const auto last = token.find_last_not_of(' ');
        token = token.substr(first, last - first + 1);
        if (token == "local") { local = true; continue; }
        if (token == "selected") { selected = true; continue; }
        auto found = std::find_if(subscriptions_.begin(), subscriptions_.end(), [&](const Json& item) {
          return item.value("name", "") == token || item.value("id", "") == token;
        });
        if (found == subscriptions_.end() || !source_enabled_for_view(found->value("id", ""))) {
          message_ = "Group member source is not enabled for this Profile: " + token;
          return;
        }
        members["sources"].push_back(found->value("id", ""));
      }
      if (selected && state_.highlighted_node_id)
        members["explicit_node_ids"].push_back(*state_.highlighted_node_id);
      else if (selected) { message_ = "Highlight a Node before using selected in a Group"; return; }
      members["include_local"] = local;
      if (members["sources"].empty() &&
          !members["include_local"].get<bool>() && members["explicit_node_ids"].empty()) {
        message_ = "Choose all, a source name, local, or selected"; return;
      }
    }
    const auto tag = "xrm-group-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (group_draft_mode_ == "manual" && !state_.highlighted_node_id) {
      message_ = "For a manual Group, highlight a candidate Node first, then Add → Group";
      return;
    }
    Json group{{"id", ""}, {"name", group_draft_name_}, {"members", std::move(members)},
        {"selection_mode", group_draft_mode_},
        {"selected_node_id", group_draft_mode_ == "manual" ? Json(*state_.highlighted_node_id) : Json(nullptr)},
        {"balancer", {{"tag", tag}, {"strategy", {{"type", group_draft_strategy_}}}}}};
    auto saved = rpc(socket_, std::string(methods::group_save),
        Json{{"profile_id", profile_id_}, {"group", std::move(group)}});
    if (!saved) { message_ = "Group save failed: " + saved.error().message; return; }
    node_category_ = "groups";
    navigate_page(5);
    message_ = "Group saved as a routing balancer; select/connect explicitly to apply";
    if (group_draft_strategy_ == "leastLoad") message_ += " · leastLoad is experimental/unverified";
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
      prompt_subscription_url_ = value;
      if (!profile_id_.empty()) {
        begin_prompt(Prompt::subscription_use_profile, "Use this source for " + profile_name(profile_id_) + "? y/n (default y)");
        return;
      }
      save_prompted_subscription(false);
      return;
    }
    if (completed == Prompt::subscription_use_profile) {
      if (value != "" && value != "y" && value != "Y" && value != "n" && value != "N") {
        message_ = "Type y or n"; begin_prompt(Prompt::subscription_use_profile, "Use for viewed Profile? y/n"); return;
      }
      save_prompted_subscription(value.empty() || value == "y" || value == "Y");
      return;
    }
    if (completed == Prompt::node_protocol) {
      if (value != "vless" && value != "vmess" && value != "trojan" &&
          value != "shadowsocks" && value != "hysteria2") {
        message_ = "Choose vless, vmess, trojan, shadowsocks or hysteria2"; return;
      }
      node_draft_.protocol = value;
      begin_prompt(Prompt::node_name, "Display name (not the protocol UUID)"); return;
    }
    if (completed == Prompt::node_name) {
      if (value.empty()) { message_ = "Node name is required"; return; }
      node_draft_.name = value;
      begin_prompt(Prompt::node_address, "Server address or IP"); return;
    }
    if (completed == Prompt::node_address) {
      if (value.empty()) { message_ = "Server address is required"; return; }
      node_draft_.address = value;
      begin_prompt(Prompt::node_port, "Server port (1–65535)"); return;
    }
    if (completed == Prompt::node_port) {
      unsigned port = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          port == 0 || port > 65535) { message_ = "Port must be 1–65535"; return; }
      node_draft_.port = static_cast<std::uint16_t>(port);
      begin_prompt(node_draft_.protocol == "shadowsocks" ? Prompt::node_ss_method : Prompt::node_auth,
          node_draft_.protocol == "vless" || node_draft_.protocol == "vmess" ?
              "Protocol UUID (not XRM node ID)" : "Authentication password/token");
      return;
    }
    if (completed == Prompt::node_ss_method) {
      if (value.empty()) { message_ = "Shadowsocks method is required"; return; }
      node_draft_.method = value;
      begin_prompt(Prompt::node_auth, "Shadowsocks password"); return;
    }
    if (completed == Prompt::node_auth) {
      if (value.empty()) { message_ = "Authentication is required"; return; }
      node_draft_.auth = value;
      begin_prompt(Prompt::node_transport,
          "Transport: raw / websocket / xhttp / grpc / httpupgrade / hysteria"); return;
    }
    if (completed == Prompt::node_transport) {
      if (value != "raw" && value != "websocket" && value != "xhttp" &&
          value != "grpc" && value != "httpupgrade" && value != "hysteria") {
        message_ = "Unsupported transport; use a supported value or native outbound"; return;
      }
      node_draft_.transport = value;
      begin_prompt(Prompt::node_security, "Security: none / tls / reality"); return;
    }
    if (completed == Prompt::node_security) {
      if (value != "none" && value != "tls" && value != "reality") {
        message_ = "Security must be none, tls or reality"; return;
      }
      node_draft_.security = value;
      if (value == "none") finish_manual_node();
      else begin_prompt(Prompt::node_server_name, "TLS/REALITY server name (may be empty)");
      return;
    }
    if (completed == Prompt::node_server_name) {
      node_draft_.server_name = value;
      if (node_draft_.security == "reality")
        begin_prompt(Prompt::node_reality_key, "REALITY public key");
      else finish_manual_node();
      return;
    }
    if (completed == Prompt::node_reality_key) {
      if (value.empty()) { message_ = "REALITY public key is required"; return; }
      node_draft_.reality_key = value;
      finish_manual_node(); return;
    }
    if (completed == Prompt::node_copy_name) {
      if (value.empty()) { message_ = "Copy name is required"; return; }
      auto copied = rpc(socket_, std::string(methods::node_copy),
          Json{{"id", prompt_node_id_}, {"name", value}});
      if (!copied) message_ = "Node copy failed: " + copied.error().message;
      else { node_detail_ = false; node_category_ = "local"; navigate_page(1);
        message_ = "Independent Local copy saved; subscription refresh cannot overwrite it"; }
      return;
    }
    if (completed == Prompt::node_replace_json) {
      auto settings = Json::parse(value, nullptr, false);
      if (!settings.is_object()) { message_ = "Node settings must be a JSON object; nothing was saved"; return; }
      auto found = std::find_if(nodes_.begin(), nodes_.end(), [&](const Json& node) {
        return node.value("id", "") == prompt_node_id_;
      });
      if (found == nodes_.end()) { message_ = "Node changed; refresh and try again"; return; }
      auto updated = rpc(socket_, std::string(methods::node_update),
          Json{{"id", prompt_node_id_}, {"name", found->value("name", "")},
               {"settings", std::move(settings)}});
      if (!updated) message_ = "Local node edit failed: " + updated.error().message;
      else { node_detail_ = false; refresh_nodes(); message_ = "Local node saved; Connect remains explicit"; }
      return;
    }
    if (completed == Prompt::node_remove_confirm) {
      if (value != "DELETE") { message_ = "Delete cancelled; node unchanged"; return; }
      auto removed = rpc(socket_, std::string(methods::node_remove), Json{{"id", prompt_node_id_}});
      if (!removed) message_ = "Node not removed: " + removed.error().message;
      else { node_detail_ = false; refresh_nodes(); message_ = "Unreferenced Local node removed"; }
      return;
    }
    if (completed == Prompt::node_uri) {
      if (value.empty()) { message_ = "Share URI is required"; return; }
      auto added = rpc(socket_, std::string(methods::node_add), Json{{"uri", value}});
      if (!added) message_ = "Import failed: " + added.error().message;
      else { node_category_ = "local"; navigate_page(1); message_ = "Node imported to Local; no connection started"; }
      return;
    }
    if (completed == Prompt::node_raw_name) {
      if (value.empty()) { message_ = "Display name is required"; return; }
      raw_node_name_ = value;
      begin_prompt(Prompt::node_raw_json, "Native outbound JSON object (single paste)"); return;
    }
    if (completed == Prompt::node_raw_json) {
      auto outbound = Json::parse(value, nullptr, false);
      if (!outbound.is_object()) { message_ = "Native outbound must be a JSON object"; return; }
      auto added = rpc(socket_, std::string(methods::node_add),
          Json{{"name", raw_node_name_}, {"outbound", outbound}});
      if (!added) message_ = "Native outbound save failed: " + added.error().message;
      else { node_category_ = "local"; navigate_page(1); message_ = "Native outbound saved to Local; config verification is separate"; }
      return;
    }
    if (completed == Prompt::group_name) {
      if (value.empty()) { message_ = "Group name is required"; return; }
      group_draft_name_ = value;
      begin_prompt(Prompt::group_mode, "Selection: automatic / manual (manual uses highlighted Node)"); return;
    }
    if (completed == Prompt::group_mode) {
      if (value != "automatic" && value != "manual") {
        message_ = "Selection mode must be automatic or manual"; return;
      }
      group_draft_mode_ = value;
      if (value == "manual") {
        group_draft_strategy_ = "random";
        begin_prompt(Prompt::group_scope,
            "Members: all, local, selected, or comma-separated source names [+local/+selected]");
        return;
      }
      begin_prompt(Prompt::group_strategy,
          "Strategy: random / roundRobin / leastPing / leastLoad (experimental)"); return;
    }
    if (completed == Prompt::group_strategy) {
      if (value != "random" && value != "roundRobin" && value != "leastPing" && value != "leastLoad") {
        message_ = "Choose a listed Xray strategy"; return;
      }
      group_draft_strategy_ = value;
      begin_prompt(Prompt::group_scope,
          "Members: all, local, selected, or comma-separated source names [+local/+selected]"); return;
    }
    if (completed == Prompt::group_scope) { save_group_from_scope(value); return; }
    if (completed == Prompt::group_remove_confirm) {
      if (value != "DELETE") { message_ = "Group removal cancelled"; return; }
      auto removed = rpc(socket_, std::string(methods::group_remove),
          Json{{"profile_id", profile_id_}, {"group_id", prompt_group_id_}});
      if (!removed) message_ = "Group removal failed: " + removed.error().message;
      else { refresh_groups(); message_ = "Group removed; running configuration unchanged until Connect/Apply"; }
      return;
    }
    if (completed == Prompt::core_remove_confirm) {
      if (value != "DELETE") { message_ = "Core removal cancelled"; return; }
      auto removed = rpc(socket_, std::string(methods::core_remove), Json{{"core_id", prompt_core_id_}});
      if (!removed) message_ = "Core not removed: " + removed.error().message;
      else { refresh_cores(); message_ = "Unused managed core removed; running core unchanged"; }
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
    if (completed == Prompt::editor_argv) {
      Json argv = Json::parse(value, nullptr, false);
      if (!argv.is_array() || !std::all_of(argv.begin(), argv.end(), [](const Json& item) {
            return item.is_string() && !item.get<std::string>().empty();
          })) {
        message_ = "Editor argv must be a JSON array of non-empty strings; no shell is used";
        return;
      }
      auto saved = rpc(socket_, std::string(methods::settings_update),
          Json{{"patch", {{"editor", {{"argv", argv}}}}}});
      if (!saved) message_ = "Editor setting failed: " + saved.error().message;
      else { program_settings_ = std::move(*saved); message_ = "Editor argv saved for this Manager host"; }
      return;
    }
    if (completed == Prompt::profile_name || completed == Prompt::profile_clone_name) {
      if (value.empty()) { message_ = "Profile name cannot be empty"; return; }
      if (completed == Prompt::profile_clone_name) {
        auto created = rpc(socket_, std::string(methods::profile_clone), Json{{"profile_id", prompt_profile_id_}, {"name", value}});
        if (!created) { message_ = created.error().message; return; }
        profile_id_ = created->value("id", "");
      } else {
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
    if (page_ == 1 && selected_ < nodes_.size()) {
      state_.highlighted_node_id = nodes_[selected_].value("id", "");
      category_highlights_[node_category_] = *state_.highlighted_node_id;
    } else if (page_ == 5 && selected_ < groups_.size())
      state_.highlighted_group_id = groups_[selected_].value("id", "");
  }
  void change_page(int page) {
    if (page_ == 0 && page != 0) stop_stats_watch();
    page_selections_[static_cast<std::size_t>(page_)] = selected_;
    page_ = page;
    selected_ = page_selections_[static_cast<std::size_t>(page_)];
    log_poll_state_->active.store(page_ == 3);
    log_poll_state_->overview_active.store(page_ == 0);
  }
  void navigate_page(int page) {
    if (page_ == 1 || page_ == 5) category_searches_[node_category_] = node_filter_;
    change_page(page);
    if (page_ == 1 && node_category_ == "groups") node_category_ = "all";
    if (page_ == 5) node_category_ = "groups";
    if (page_ == 1 || page_ == 5)
      node_filter_ = category_searches_.contains(node_category_) ?
          category_searches_[node_category_] : std::string{};
    if (page_ == 1) refresh_nodes();
    else if (page_ == 2) refresh_modules();
    else if (page_ == 3) refresh_logs();
    else if (page_ == 4) refresh_subscriptions();
    else if (page_ == 5) refresh_groups();
    else if (page_ == 0) refresh_stats(true);
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
    profile_settings_ = Json::object();
    if (profile_id_.empty()) return;
    auto profile = rpc(socket_, std::string(methods::profile_get), Json{{"profile_id", profile_id_}});
    if (profile && profile->is_object()) profile_settings_ = *profile;
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
      const auto source = node.contains("source_id") && node["source_id"].is_string()
          ? node["source_id"].get<std::string>() : std::string{};
      if (node_category_ == "local" && !source.empty()) continue;
      if (node_category_ == "all" && (source.empty() || !source_enabled_for_view(source))) continue;
      if (node_category_.starts_with("source:") && source != node_category_.substr(7)) continue;
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
  void open_history() {
    if (selected_ >= subscriptions_.size()) { message_ = "Select a subscription first"; return; }
    const auto source_id = subscriptions_[selected_].value("id", "");
    auto preview = rpc(socket_, std::string(methods::node_history_preview), Json{{"source_id", source_id}});
    if (!preview) { message_ = "History preview failed: " + preview.error().message; return; }
    history_preview_ = std::move(*preview);
    history_source_id_ = source_id;
    history_index_ = 0;
    history_selected_ids_.clear();
    history_open_ = true;
    history_confirm_ = false;
  }
  void confirm_history() {
    Json ids = Json::array();
    for (const auto& id : history_selected_ids_) ids.push_back(id);
    auto result = rpc(socket_, std::string(methods::node_history_confirm),
        Json{{"source_id", history_source_id_}, {"token", history_preview_.at("token")}, {"ids", ids}});
    history_confirm_ = false;
    if (!result) {
      message_ = "History cleanup failed: " + result.error().message + "; reopen preview";
      history_open_ = false;
      return;
    }
    const auto removed = result->value("removed", 0);
    history_open_ = false;
    history_selected_ids_.clear();
    message_ = "Removed " + std::to_string(removed) + " selected historical records; running core unchanged";
    refresh_subscriptions();
    refresh_nodes();
  }
  void inspect_local_entry() {
    if (profile_id_.empty()) { message_ = "Select or create a Profile first"; return; }
    auto view = rpc(socket_, std::string(methods::module_get),
        Json{{"profile_id", profile_id_}, {"module", "inbounds"}});
    if (!view) { message_ = "Cannot inspect inbounds: " + view.error().message; return; }
    local_entry_included_ = view->value("included", false);
    local_entry_base_text_.reset();
    Json module{{"inbounds", Json::array()}};
    if (view->contains("text") && (*view)["text"].is_string()) {
      local_entry_base_text_ = (*view)["text"].get<std::string>();
      module = Json::parse(*local_entry_base_text_, nullptr, false);
      if (module.is_discarded() || !module.is_object() || !module.contains("inbounds") ||
          !module["inbounds"].is_array()) {
        message_ = "Inbounds source is not an editable native array; open Modules → inbounds";
        return;
      }
    }
    for (const auto& inbound : module["inbounds"]) {
      if (!inbound.is_object()) continue;
      const auto listen = inbound_text_field(inbound, "listen");
      const auto port = inbound_port(inbound);
      if (port == 10808 || inbound_text_field(inbound, "tag") == "local-socks") {
        message_ = "Port 10808 or tag local-socks already configured; inspect Modules → inbounds. "
            "No source changed.";
        return;
      }
      if (inbound_text_field(inbound, "protocol") == "socks" &&
          (listen == "127.0.0.1" || listen == "::1" || listen == "0.0.0.0" ||
           listen == "::" || listen.empty())) {
        message_ = local_entry_included_
            ? "This Profile already has a SOCKS entry; inspect Modules → inbounds"
            : "Local SOCKS entry exists but inbounds is disabled; enable it in Modules";
        return;
      }
    }
    if (!local_socks_port_available()) {
      message_ = "127.0.0.1:10808 is currently unavailable; inspect Modules → inbounds "
          "and choose a free port. No source changed.";
      return;
    }
    local_entry_proposed_ = std::move(module);
    local_entry_proposed_["inbounds"].push_back(Json{
        {"tag", "local-socks"}, {"listen", "127.0.0.1"}, {"port", 10808},
        {"protocol", "socks"}, {"settings", Json{{"auth", "noauth"}, {"udp", true}}}});
    local_entry_confirm_ = true;
  }
  void add_local_entry() {
    local_entry_confirm_ = false;
    auto result = rpc(socket_, std::string(methods::module_save),
        Json{{"profile_id", profile_id_}, {"module", "inbounds"},
             {"text", local_entry_proposed_.dump(2) + "\n"},
             {"expected_text", local_entry_base_text_ ? Json(*local_entry_base_text_) : Json(nullptr)},
             {"expected_absent", !local_entry_base_text_}});
    if (!result) {
      message_ = "Local entry not saved: " + result.error().message + "; inspect inbounds and retry";
      return;
    }
    message_ = local_entry_included_
        ? "Local SOCKS 127.0.0.1:10808 saved; explicitly Connect/Apply to use it"
        : "Local SOCKS 127.0.0.1:10808 saved; enable inbounds in Modules, then Connect";
    refresh_modules();
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
    log_poll_state_->animation.store(!appearance_draft_.value("reduce_motion", false));
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
    if (profile_id_.empty()) { local_entry_status_ = "no Profile"; return; }
    auto response = rpc(socket_, std::string(methods::module_list), Json{{"profile_id", profile_id_}});
    modules_.clear();
    if (response && response->is_array()) modules_ = response->get<std::vector<Json>>();
    selected_ = std::min(selected_, modules_.empty() ? 0 : modules_.size() - 1);
    local_entry_status_ = "no local SOCKS entry (custom/server Profiles may be intentional)";
    for (const auto& module : modules_) {
      if (module.value("key", "") != "inbounds") continue;
      if (!module.value("included", false)) {
        local_entry_status_ = "inbounds disabled";
        break;
      }
      if (!module.contains("text") || !module["text"].is_string()) break;
      auto source = Json::parse(module["text"].get<std::string>(), nullptr, false);
      if (source.is_discarded() || !source.is_object() || !source.contains("inbounds") ||
          !source["inbounds"].is_array()) {
        local_entry_status_ = "inbounds source needs native inspection";
        break;
      }
      for (const auto& inbound : source["inbounds"]) {
        if (!inbound.is_object()) continue;
        const auto listen = inbound_text_field(inbound, "listen");
        if (inbound_text_field(inbound, "protocol") == "socks" &&
            (listen == "127.0.0.1" || listen == "::1" || listen == "0.0.0.0" ||
             listen == "::" || listen.empty())) {
          local_entry_status_ = "SOCKS " + (listen.empty() ? "default-listen" : listen) + ":" +
              std::to_string(inbound_port(inbound)) + " configured";
          break;
        }
      }
      break;
    }
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
    std::optional<std::vector<std::string>> args;
    const auto editor = program_settings_.value("editor", Json::object());
    if (editor.contains("argv") && editor["argv"].is_array() && !editor["argv"].empty()) {
      try { args = editor["argv"].get<std::vector<std::string>>(); }
      catch (const Json::exception&) { message_ = "saved editor argv is invalid"; return; }
    } else {
      const auto command = editor_command();
      if (command.empty()) {
        message_ = "set Config → Editor or VISUAL/EDITOR on the Manager host";
        return;
      }
      args = editor_argv(command);
    }
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
  void test_configured_editor() {
    const auto old_draft = draft_;
    const auto old_cursor = edit_cursor_;
    const auto old_base = editing_base_text_;
    const auto old_dirty = editing_dirty_;
    const auto old_invalid = draft_json_invalid_;
    const auto old_module_draft = state_.module_draft;
    draft_ = "XRM temporary editor test draft\n";
    edit_cursor_ = draft_.size();
    editing_base_text_ = draft_;
    open_external_editor();
    const auto editor_result = message_;
    draft_ = old_draft;
    edit_cursor_ = old_cursor;
    editing_base_text_ = old_base;
    editing_dirty_ = old_dirty;
    draft_json_invalid_ = old_invalid;
    state_.module_draft = old_module_draft;
    message_ = "Temporary editor test: " + editor_result + "; no Xray module was saved";
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
      const auto found = std::find_if(all_nodes_.begin(), all_nodes_.end(), [&](const Json& node) {
        return node.value("id", "") == id;
      });
      if (found != all_nodes_.end()) return "node: " + found->value("name", std::string(id));
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
  Json program_settings_ = Json::object();
  Json appearance_draft_ = Json::object();
  bool appearance_dirty_ = false;
  std::string prompt_profile_id_;
  std::string prompt_subscription_name_;
  std::string prompt_subscription_url_;
  bool add_outlet_menu_ = false;
  NodeDraft node_draft_;
  std::string raw_node_name_;
  std::string prompt_node_id_;
  std::string group_draft_name_;
  std::string group_draft_mode_ = "automatic";
  std::string group_draft_strategy_ = "random";
  std::string prompt_group_id_;
  std::string prompt_core_id_;
  std::string prompt_label_;
  std::string prompt_text_;
  Prompt prompt_ = Prompt::none;
  ScreenInteractive* screen_;
  int& page_ = state_.page;
  std::size_t config_section_ = 0;
  std::size_t selected_ = 0;
  std::array<std::size_t, 9> page_selections_{};
  bool editing_ = false;
  bool filtering_nodes_ = false;
  bool node_detail_ = false;
  bool subscription_detail_ = false;
  bool history_open_ = false;
  bool history_confirm_ = false;
  Json history_preview_ = Json::object();
  std::string history_source_id_;
  std::set<std::string> history_selected_ids_;
  std::size_t history_index_ = 0;
  bool local_entry_confirm_ = false;
  bool local_entry_included_ = false;
  Json local_entry_proposed_ = Json::object();
  std::optional<std::string> local_entry_base_text_;
  bool editing_group_ = false;
  bool editing_node_ = false;
  bool editing_dirty_ = false;
  bool editing_module_included_ = false;
  bool editing_module_unconfigured_ = false;
  bool draft_json_invalid_ = false;
  std::optional<std::string> editing_base_text_;
  std::string editing_key_;
  std::string node_filter_;
  std::string node_filter_before_edit_;
  std::string draft_;
  std::size_t edit_cursor_ = 0;
  std::vector<EditorSnapshot> editor_undo_;
  std::vector<Json> all_nodes_;
  std::vector<Json> nodes_;
  Json profile_settings_ = Json::object();
  std::string node_category_ = "all";
  std::map<std::string, std::string> category_highlights_;
  std::map<std::string, std::string> category_searches_;
  std::size_t node_count_ = 0;
  std::size_t node_missing_count_ = 0;
  bool show_missing_nodes_ = false;
  std::vector<Json> subscriptions_;
  std::vector<Json> modules_;
  std::string local_entry_status_ = "not inspected";
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

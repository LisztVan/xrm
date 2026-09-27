#pragma once
#include "client/rpc_session.hpp"
#include "tui/adapters/rpc.hpp"
#include "tui/components/widgets.hpp"
#include <array>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>
#include <future>
#include <map>
#include <thread>
namespace xrm::tui {
struct DraftSession {
  std::string profile, module, text, baseline, source, runtime, error;
  std::optional<std::string> expected;
  bool included = false, visual = true, runtime_view = false;
  bool dirty() const { return text != baseline; }
};
struct Action {
  std::string key, label, target, disabled;
  std::function<void()> run;
};
class Workspace final : public ftxui::ComponentBase {
public:
  Workspace(std::string socket, std::string initial_profile,
            ftxui::ScreenInteractive &screen);
  ~Workspace() override;
  ftxui::Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  ftxui::Component ActiveChild() override;
  void SetActiveChild(ftxui::ComponentBase *) override {}

private:
  enum class Layer { Top, Secondary, Content };
  std::string socket_, view_, use_, edit_, detail_title_, detail_, last_error_,
      search_, last_diagnostic_;
  ftxui::ScreenInteractive &screen_;
  int page_ = 0, secondary_ = 0, overview_scroll_ = 0;
  std::array<std::string, 7> selected_section_ids_{};
  Layer layer_ = Layer::Top;
  Json profiles_ = Json::array(), subscriptions_ = Json::array(),
       nodes_ = Json::array(), groups_ = Json::array(), cores_ = Json::array(),
       geodata_ = Json::array(), modules_ = Json::array();
  Json profile_ = Json::object(), settings_ = Json::object(),
       status_ = Json::object(), stats_ = Json::object(),
       releases_ = Json::array();
  bool loaded_ = false, fresh_ = false, endpoint_connected_ = false,
       show_missing_ = false,
       releases_view_ = false;
  std::shared_ptr<ui::Table> profiles_table_, subscriptions_table_,
      nodes_table_, groups_table_, cores_table_, geodata_table_,
      settings_table_;
  std::shared_ptr<ui::LogViewer> logs_;
  std::vector<std::string> log_lines_;
  Json raw_logs_ = Json::array();
  std::uint64_t log_cursor_ = 0;
  bool log_dropped_ = false, log_truncated_ = false;
  DraftSession draft_;
  ftxui::Component visual_, code_;
  std::vector<ftxui::Component> overlays_;
  ui::ToastHost toasts_;
  std::unique_ptr<RpcSession> stats_reader_;
  ftxui::Terminal::Color original_color_;
  std::jthread ticker_;
  std::future<Result<Json>> background_;
  std::function<void(Result<Json>)> background_done_;
  std::map<std::string, std::string> operations_;
  std::chrono::steady_clock::time_point next_poll_{};
  static constexpr std::array<const char *, 7> pages_{
      "Overview", "Profiles", "Modules", "Subscriptions",
      "Nodes",    "Logs",     "Config"};
  Result<Json> call(std::string_view method, Json params = Json::object());
  void notify(std::string text, ui::Tone tone = ui::Tone::Info);
  void apply_colors();
  void refresh();
  void refresh_profile();
  void refresh_tables();
  void poll();
  void read_logs();
  void background(std::function<Result<Json>()> job,
                  std::function<void(Result<Json>)> done);
  void operation(std::string method, Json params, std::string label);
  bool mutate(std::string_view method, Json params, std::string label);
  std::string name(const std::string &id) const;
  std::string active_name() const;
  std::string target_label(const Json &target) const;
  std::vector<std::pair<std::string, std::string>> secondary_items() const;
  std::string section() const;
  std::shared_ptr<ui::Table> table() const;
  ftxui::Component content() const;
  ftxui::Element overview() const;
  void view_detail();
  void full_detail();
  std::string context_details() const;
  std::vector<Action> actions();
  void help();
  void push(ftxui::Component c);
  void pop();
  void form(std::string title, std::vector<ui::Field> fields,
            ui::Form::Submit submit, std::string save = "Save");
  void cancel_form(std::weak_ptr<ui::Form> form);
  void confirm(std::string title, std::string message,
               std::function<void()> accept);
  void leave_draft(std::function<void()> continuation);
  bool writable() const;
  void load_module();
  void rebuild_module();
  bool save_module();
  void include_module();
  void switch_module_view();
  void external_editor();
  void routing_preset();
  void profile_form(std::string mode);
  void toggle_edit();
  void subscription_form(bool edit);
  void remove_subscription();
  void bind_sources();
  void history();
  void node_results();
  void cancel_node_tests();
  void node_form(bool edit);
  void node_advanced();
  void node_import();
  void node_copy();
  void node_labels();
  void node_test();
  void group_form(bool edit);
  void group_member();
  void save_target(bool connect);
  void core_form();
  void core_releases(bool prerelease);
  void core_install();
  void geodata_form(bool edit);
  void bind_geodata();
  void settings_form();
  void connect();
  void local_entry();
};
ui::Field text_field(std::string label, std::string value = "",
                     bool required = false);
ui::Field select_field(std::string label, std::string value,
                       std::vector<ui::Choice> choices);
ui::Field bool_field(std::string label, bool value);
ui::Field number_field(std::string label, long long value, long long min,
                       long long max);
Json find_id(const Json &rows, const std::string &id);
std::string scalar(const Json &value);
} // namespace xrm::tui

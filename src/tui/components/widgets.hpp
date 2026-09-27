/// @file
/// @brief Declares shared TUI controls and their focus and value states.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace xrm::tui::ui {
using ftxui::Component;
using ftxui::Element;
using Validator =
    std::function<std::optional<std::string>(const std::string &)>;
struct Choice {
  std::string value;
  std::string label;
};
struct HelpEntry {
  // A disabled action has a reason but must not run. consequence describes the
  // saved/runtime effect shown beside the executable action in Help.
  std::string key, action, target, disabled_reason;
  std::function<void()> run;
  std::string consequence;
};
enum class FieldKind {
  Text,
  Secret,
  Number,
  Multiline,
  Select,
  Checkbox,
  MultiSelect,
  StringList,
  OptionalCheckbox
};
/// One form draft field with independent native presence and value.
struct Field {
  // optional/present model native omission separately from an empty, false,
  // or zero value. Controls edit value/present; Form submits their snapshot.
  std::string label;
  std::string value;
  FieldKind kind = FieldKind::Text;
  std::vector<Choice> choices;
  std::string hint;
  Validator validate;
  bool readonly = false;
  bool disabled = false;
  bool optional = false;
  bool present = true;
  std::string section;
  // MultiSelect values are newline-separated stable choice IDs.
};

Component TextInput(std::string *value, bool readonly = false,
                    bool secret = false, bool multiline = false,
                    bool disabled = false);
Component TextArea(std::string *value, bool readonly = false);
Component ReadOnlyText(std::string *value);
Component Select(std::string *value, std::vector<Choice> choices,
                 bool readonly = false);
Component MultiSelect(std::string *value, std::vector<Choice> choices,
                      bool readonly = false);
Component Toggle(std::string *value, bool readonly = false);
Component StringList(std::string *value, bool readonly = false,
                     std::string label = "Entries");
std::string Clip(const std::string &value, int width);
std::string Count(std::size_t count, const std::string &singular,
                  const std::string &plural = "");
Element Navigation(const std::vector<std::string> &labels, int selected,
                   int width, bool focused);
Validator Integer(std::optional<long long> min = {},
                  std::optional<long long> max = {}, bool optional = true);

/// @brief Validates caller-supplied fields and invokes one submit callback.
///
/// Controls mutate only this form's draft. On validation failure, the form
/// keeps values and focus so the user can repair the relevant field.
class Form final : public ftxui::ComponentBase,
                   public std::enable_shared_from_this<Form> {
public:
  // Controls edit local values. Submit is the only path that asks the caller
  // to persist them; field validation leaves the draft and focus intact.
  using Submit = std::function<std::optional<std::string>(Form &)>;
  Form(std::string title, std::vector<Field> fields, Submit submit,
       std::function<void()> cancel, bool readonly = false,
       std::string save_label = "Save");
  Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  const std::string &value(std::size_t index) const;
  void set_value(std::size_t index, std::string value);
  const std::vector<Field> &fields() const { return fields_; }
  bool present(std::size_t index) const { return fields_.at(index).present; }
  void set_present(std::size_t index, bool present) {
    fields_.at(index).present = present;
  }
  const std::string &title() const { return title_; }
  const std::string &save_label() const { return save_label_; }
  const std::string &context() const { return context_; }
  void set_context(std::string context) { context_ = std::move(context); }
  void cancel() {
    auto keep = weak_from_this().lock();
    if (cancel_)
      cancel_();
  }
  void set_error(std::string error);
  void set_field_error(std::size_t index, std::string error,
                       std::vector<std::size_t> related = {});
  const std::string &error() const { return error_; }
  bool dirty() const;
  bool readonly() const { return readonly_; }
  bool submit();
  void focus_field(std::size_t index);

private:
  std::string title_, error_, save_label_, context_;
  std::vector<bool> baseline_present_;
  std::vector<Field> fields_;
  std::vector<std::string> baseline_, errors_;
  std::vector<std::size_t> related_error_fields_;
  std::vector<Component> controls_;
  Component body_, buttons_;
  Submit submit_;
  std::function<void()> cancel_;
  bool readonly_;
};

struct Row {
  std::string id;
  std::vector<std::string> cells;
};
struct Column {
  std::string label;
  int width = 20;
};
/// @brief Displays rows while keeping the selected object ID across refresh.
class Table final : public ftxui::ComponentBase {
public:
  explicit Table(std::vector<Column> columns = {},
                 std::function<void(const std::string &)> activate = {});
  // Rows may refresh while focus stays here; selection follows a stable ID.
  void set_rows(std::vector<Row> rows, std::string empty = "No entries");
  const std::string &selected_id() const;
  std::string selected_summary() const;
  int selected_index() const { return selected_; }
  std::size_t row_count() const { return rows_.size(); }
  void select_id(const std::string &id);
  void presentation(bool compact, bool ascii) {
    compact_ = compact;
    ascii_ = ascii;
  }
  Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  bool Focusable() const override { return true; }

private:
  std::vector<Column> columns_;
  std::vector<int> rendered_widths_;
  ftxui::Box box_;
  std::vector<Row> rows_;
  std::vector<std::string> labels_;
  std::string empty_ = "No entries", no_id_;
  int selected_ = 0;
  bool compact_ = false, ascii_ = true;
  Component menu_;
  std::function<void(const std::string &)> activate_;
};

struct TreeNode {
  std::string id, label;
  std::vector<TreeNode> children;
  bool expanded = true;
};
class SectionTree final : public ftxui::ComponentBase {
public:
  explicit SectionTree(std::vector<TreeNode> nodes,
                       std::function<void(const std::string &)> activate = {});
  Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  std::string selected_id() const;

private:
  void rebuild();
  std::vector<TreeNode> nodes_;
  std::vector<TreeNode *> visible_;
  std::shared_ptr<Table> table_;
};

struct CollectionActions {
  std::function<void()> add;
  std::function<void(const std::string &)> view, edit, copy, remove;
  std::function<void(const std::string &, int)> move;
};
/// @brief Applies collection actions to the stable selected row identity.
class CollectionEditor final : public ftxui::ComponentBase {
public:
  // Action callbacks receive stable row IDs. Tab onto the button row keeps
  // the selected collection item instead of selecting by a transient index.
  CollectionEditor(std::string title, std::vector<Column> columns,
                   CollectionActions actions, bool readonly = false);
  void set_rows(std::vector<Row> rows);
  Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  std::shared_ptr<Table> table() const { return table_; }
  std::vector<HelpEntry> help() const;

private:
  std::string title_;
  std::shared_ptr<Table> table_;
  Component controls_;
  CollectionActions actions_;
  std::string notice_;
  bool readonly_;
};

Component Dialog(std::string title, Component content,
                 std::function<void()> close,
                 std::vector<HelpEntry> actions = {});
Component Confirm(std::string title, std::string message,
                  std::function<void()> accept, std::function<void()> cancel);
std::vector<HelpEntry> ContextHelp(const Component &component);
Component HelpOverlay(std::vector<HelpEntry> entries,
                      std::function<void()> close, std::string context = "",
                      std::string prerequisites = "");

enum class Tone { Info, Success, Warning, Error };
struct Toast {
  std::string key, message;
  Tone tone;
  std::chrono::steady_clock::time_point expires;
  int repeats = 1;
};
class ToastHost {
public:
  using Clock = std::chrono::steady_clock;
  void push(std::string key, std::string message, Tone tone = Tone::Info,
            Clock::time_point now = Clock::now());
  void expire(Clock::time_point now = Clock::now());
  void suspended(bool value, Clock::time_point now = Clock::now());
  Element render() const;
  const std::deque<Toast> &visible() const { return visible_; }
  const std::deque<Toast> &history() const { return history_; }

private:
  std::deque<Toast> visible_, history_;
  std::optional<Clock::time_point> suspended_at_;
};

// Sequence is the daemon's chunk cursor, not an invented log timestamp/ID.
struct LogChunk {
  std::uint64_t sequence = 0;
  std::string source, text;
};
struct LogRecord {
  std::uint64_t sequence = 0;
  std::size_t line = 0;
  std::string timestamp, source, level, subsystem, message, raw;
  bool partial = false;
  bool operator==(const LogRecord &) const = default;
};
std::vector<LogRecord> ParseLogChunks(const std::vector<LogChunk> &chunks);

class LogViewer final : public ftxui::ComponentBase {
public:
  void set_lines(std::vector<std::string> lines);
  void set_records(std::vector<LogRecord> records);
  void set_follow(bool follow);
  bool following() const { return follow_; }
  std::size_t unseen() const { return unseen_; }
  std::size_t record_count() const { return records_.size(); }
  const LogRecord *selected_record() const;
  Element OnRender() override;
  bool OnEvent(ftxui::Event event) override;
  bool Focusable() const override { return true; }

private:
  std::vector<LogRecord> records_, incoming_;
  int cursor_ = 0;
  bool follow_ = true;
  std::size_t unseen_ = 0;
};
Element InfoPane(std::string object, std::string details, std::string status);
std::vector<std::string> SplitLines(const std::string &value);
std::string JoinLines(const std::vector<std::string> &values);
} // namespace xrm::tui::ui

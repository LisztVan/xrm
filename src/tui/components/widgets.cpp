// XRM: Implements shared TUI controls, forms, tables, dialogs, and toasts.
// Controls own focus/rendering, but edit caller-owned values; page code owns
// business state and persistence. Keep key handling consistent across pages.

#include "tui/components/widgets.hpp"
#include "tui/components/style.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <utility>

#include <ftxui/component/component_options.hpp>
#include <ftxui/screen/string.hpp>

namespace xrm::tui::ui {
using namespace ftxui;
std::string Clip(const std::string &value, int width) {
  if (string_width(value) <= width)
    return value;
  std::string result;
  int used = 0;
  for (const auto &glyph : Utf8ToGlyphs(value)) {
    const int cells = string_width(glyph);
    if (used + cells > width - 1)
      break;
    result += glyph;
    used += cells;
  }
  return result + "…";
}
std::string Count(std::size_t count, const std::string &singular,
                  const std::string &plural) {
  return std::to_string(count) + " " +
         (count == 1       ? singular
          : plural.empty() ? singular + "s"
                           : plural);
}
Element Navigation(const std::vector<std::string> &labels, int selected,
                   int width, bool focused) {
  if (labels.empty())
    return text("");
  selected = std::clamp(selected, 0, int(labels.size()) - 1);
  const auto position =
      std::to_string(selected + 1) + "/" + std::to_string(labels.size());
  const int budget = std::max(1, width - int(position.size()) - 8);
  int first = selected, last = selected;
  int used = std::min(budget, string_width(labels[selected]) + 2);
  while (first > 0 && used + string_width(labels[first - 1]) + 2 <= budget) {
    --first;
    used += string_width(labels[first]) + 2;
  }
  while (last + 1 < int(labels.size()) &&
         used + string_width(labels[last + 1]) + 2 <= budget) {
    ++last;
    used += string_width(labels[last]) + 2;
  }
  Elements tabs{text(first ? "‹ " : "  ") | style::ink(style::Role::Accent)};
  for (int i = first; i <= last; ++i) {
    auto item = text(" " + Clip(labels[i], budget - 2) + " ");
    tabs.push_back(i == selected ? item | style::selected(focused)
                                 : item | style::ink(style::Role::Muted));
  }
  tabs.push_back(filler());
  tabs.push_back(text(last + 1 < int(labels.size()) ? " › " : "   ") |
                 style::ink(style::Role::Accent));
  tabs.push_back(text(position) | style::ink(style::Role::Muted));
  return hbox(std::move(tabs)) | size(WIDTH, EQUAL, width);
}
namespace {
bool motion(const Event &e) {
  return e == Event::ArrowLeft || e == Event::ArrowRight ||
         e == Event::ArrowUp || e == Event::ArrowDown || e == Event::Home ||
         e == Event::End || e == Event::ArrowLeftCtrl ||
         e == Event::ArrowRightCtrl || e == Event::PageUp ||
         e == Event::PageDown;
}
Component action_button(std::string label, std::function<void()> action,
                        std::function<bool()> enabled) {
  return Maybe(style::button(std::move(label),
                             [action, enabled] {
                               if (enabled())
                                 action();
                             }),
               enabled);
}
class InputControl final : public ComponentBase {
  // FTXUI Input owns character editing. This wrapper only reserves global
  // navigation keys and adapts cursor movement/focus for forms.
public:
  InputControl(std::string *value, bool readonly, bool secret, bool multiline,
               bool disabled)
      : value_(value), readonly_(readonly), multiline_(multiline),
        disabled_(disabled), secret_(secret), hidden_(secret),
        cursor_(multiline ? 0 : static_cast<int>(value->size())) {
    InputOption options;
    options.multiline = multiline;
    options.password = &hidden_;
    options.placeholder = readonly   ? "Not set (read only)"
                          : disabled ? "Unavailable"
                                     : "Enter value";
    options.cursor_position = &cursor_;
    options.transform = [](InputState state) {
      return state.element | (state.focused ? style::ink(style::Role::Focus)
                                            : style::ink(style::Role::Text));
    };
    input_ = Input(value, options);
    Add(input_);
  }
  Element OnRender() override {
    auto result = input_->Render();
    if (multiline_)
      result = result | frame | flex;
    if (readonly_ || disabled_)
      result = result | style::ink(style::Role::Disabled);
    return hbox({text(readonly_ ? "  " : "[ "), result | xflex,
                 text(readonly_ ? "  " : " ]")}) |
           style::panel();
  }
  bool Focusable() const override { return !disabled_; }
  bool OnEvent(Event event) override {
    if (disabled_)
      return false;
    if (secret_ && event == Event::Special("\x12")) {
      hidden_ = !hidden_;
      return true;
    }
    if (event == Event::Tab || event == Event::TabReverse ||
        event == Event::Escape || event == Event::F1)
      return false;
    cursor_ = std::clamp(cursor_, 0, static_cast<int>(value_->size()));
    // FTXUI 7's Home/End move to document boundaries. Adapt only the cursor,
    // retaining its UTF-8 insertion/deletion and vertical movement
    // implementation.
    if (event == Event::Home) {
      const auto pos =
          cursor_ ? value_->rfind('\n', static_cast<std::size_t>(cursor_ - 1))
                  : std::string::npos;
      cursor_ = pos == std::string::npos ? 0 : static_cast<int>(pos + 1);
      return true;
    }
    if (event == Event::End) {
      const auto pos = value_->find('\n', static_cast<std::size_t>(cursor_));
      cursor_ =
          static_cast<int>(pos == std::string::npos ? value_->size() : pos);
      return true;
    }
    if (event == Event::Special("\x1b[1;5H")) {
      cursor_ = 0;
      return true;
    }
    if (event == Event::Special("\x1b[1;5F")) {
      cursor_ = static_cast<int>(value_->size());
      return true;
    }
    if (readonly_ && !motion(event) && !event.is_mouse())
      return true;
    if (!multiline_ && (event == Event::ArrowUp || event == Event::ArrowDown ||
                        event == Event::Return))
      return true;
    if (input_->OnEvent(event))
      return true;
    return motion(event) || event.is_character() || event == Event::Return ||
           event == Event::Backspace || event == Event::Delete;
  }

private:
  std::string *value_;
  bool readonly_, multiline_, disabled_, secret_, hidden_;
  int cursor_ = 0;
  Component input_;
};
class SelectControl final : public ComponentBase {
  // selected_ and *value_ change together on arrows; Enter consumes focus
  // without a second commit that could accidentally submit the parent form.
public:
  SelectControl(std::string *value, std::vector<Choice> choices, bool readonly)
      : value_(value), choices_(std::move(choices)), readonly_(readonly) {
    auto found = std::find_if(choices_.begin(), choices_.end(),
                              [&](const auto &c) { return c.value == *value; });
    if (found == choices_.end() && (!value->empty() || !choices_.empty())) {
      choices_.insert(
          choices_.begin(),
          Choice{*value, value->empty()
                             ? "(not set)"
                             : "Unsupported existing value: " + *value});
    } else {
      selected_ = static_cast<int>(found - choices_.begin());
    }
    for (const auto &choice : choices_)
      labels_.push_back(choice.label);
    auto option = RadioboxOption::Simple();
    focused_choice_ = selected_;
    option.focused_entry = &focused_choice_;
    option.transform = [](const EntryState &state) {
      auto item =
          text(std::string(state.state ? "(o) " : "( ) ") + state.label);
      return state.focused ? item | style::selected(true) : item;
    };
    menu_ = Radiobox(&labels_, &selected_, option);
    Add(menu_);
  }
  Element OnRender() override {
    if (choices_.empty()) {
      auto empty =
          text("No choices available · check the required source or target");
      return Focused() ? empty | style::selected(true) | focus
                       : empty | style::ink(style::Role::Disabled);
    }
    auto result = text(
        std::string(readonly_ || choices_.size() == 1 ? "  " : "‹ ") +
        labels_[selected_] + (readonly_ || choices_.size() == 1 ? "" : " ›") +
        (choices_.size() == 1 && !readonly_ ? " (only option)" : ""));
    return Focused() ? result | style::selected(true) | focus
                     : result | style::ink(style::Role::Accent);
  }
  bool OnEvent(Event event) override {
    if (event == Event::Tab || event == Event::TabReverse ||
        event == Event::Escape || event == Event::F1)
      return false;
    if (readonly_ || choices_.empty())
      return true;
    int next = selected_;
    if (event == Event::ArrowRight || event == Event::ArrowDown)
      next = std::min(next + 1, static_cast<int>(choices_.size()) - 1);
    else if (event == Event::ArrowLeft || event == Event::ArrowUp)
      next = std::max(next - 1, 0);
    else if (event == Event::Home)
      next = 0;
    else if (event == Event::End)
      next = static_cast<int>(choices_.size()) - 1;
    selected_ = focused_choice_ = next;
    *value_ = choices_[selected_].value;
    // The visible option is already the form value. Enter never commits a
    // second hidden candidate or reaches the parent form button.
    return true;
  }

private:
  std::string *value_;
  std::vector<Choice> choices_;
  std::vector<std::string> labels_;
  bool readonly_;
  int selected_ = 0, focused_choice_ = 0;
  Component menu_;
};
class ToggleControl final : public ComponentBase {
public:
  ToggleControl(std::string *value, bool readonly)
      : value_(value), readonly_(readonly), checked_(*value == "true") {
    child_ = Checkbox("Enabled", &checked_, style::checkbox());
    Add(child_);
  }
  Element OnRender() override { return child_->Render(); }
  bool OnEvent(Event event) override {
    if (event == Event::Tab || event == Event::TabReverse ||
        event == Event::Escape || event == Event::F1)
      return false;
    if (readonly_ || event == Event::Return)
      return true;
    const auto result = child_->OnEvent(event);
    if (result)
      *value_ = checked_ ? "true" : "false";
    return result;
  }

private:
  std::string *value_;
  bool readonly_, checked_;
  Component child_;
};
class OptionalValueControl final : public ComponentBase {
public:
  OptionalValueControl(Field *field, Component value, bool readonly)
      : field_(field), value_(std::move(value)), readonly_(readonly) {
    present_ = Checkbox("Set value", &field_->present, style::checkbox());
    body_ = Container::Vertical({present_, value_});
    Add(body_);
  }
  Element OnRender() override {
    auto state = field_->present ? text("Explicit value")
                                 : text("Not set · field omitted");
    return vbox(
        {readonly_ ? state | style::ink(style::Role::Muted)
                   : hbox({present_->Render(), text("  "),
                           state | style::ink(style::Role::Muted)}),
         field_->present
             ? value_->Render()
             : text("  Enable Set value to edit; draft value retained") |
                   style::ink(style::Role::Disabled)});
  }
  bool OnEvent(Event event) override {
    if (event == Event::F1 || event == Event::Escape)
      return false;
    if (event == Event::Tab || event == Event::TabReverse) {
      if (readonly_)
        return false;
      if (body_->ActiveChild() == present_ && event == Event::Tab &&
          field_->present) {
        value_->TakeFocus();
        return true;
      }
      if (body_->ActiveChild() == value_) {
        if (event == Event::TabReverse) {
          present_->TakeFocus();
          return true;
        }
      }
      return false;
    }
    if (readonly_)
      return motion(event) ? value_->OnEvent(event) : true;
    if (event == Event::Return)
      return true;
    if (!field_->present) {
      present_->TakeFocus();
      return event == Event::Character(' ') ? present_->OnEvent(event) : true;
    }
    return body_->OnEvent(event) || event.is_character();
  }

private:
  Field *field_;
  Component value_, present_, body_;
  bool readonly_;
};
class MultiSelectControl final : public ComponentBase {
public:
  MultiSelectControl(std::string *value, std::vector<Choice> choices,
                     bool readonly)
      : value_(value), choices_(std::move(choices)), readonly_(readonly) {
    const auto selected = SplitLines(*value);
    for (const auto &id : selected) {
      if (!id.empty() &&
          std::none_of(choices_.begin(), choices_.end(),
                       [&](const auto &c) { return c.value == id; }))
        choices_.push_back({id, "Unsupported existing value: " + id});
    }
    for (const auto &c : choices_) {
      checked_.push_back(
          std::make_unique<bool>(std::find(selected.begin(), selected.end(),
                                           c.value) != selected.end()));
    }
    Components children;
    for (std::size_t i = 0; i < choices_.size(); ++i)
      children.push_back(
          Checkbox(choices_[i].label, checked_[i].get(), style::checkbox()));
    body_ = Container::Vertical(children);
    Add(body_);
  }
  Element OnRender() override {
    return body_->Render() | vscroll_indicator | frame |
           size(HEIGHT, LESS_THAN, 6);
  }
  bool OnEvent(Event event) override {
    // Tab leaves the multi-select as one field; arrows move among its choices.
    if (event == Event::Tab || event == Event::TabReverse ||
        event == Event::Escape || event == Event::F1)
      return false;
    if (event == Event::Return)
      return true;
    if (readonly_ && event != Event::ArrowUp && event != Event::ArrowDown)
      return true;
    std::vector<bool> before;
    for (const auto &checked : checked_)
      before.push_back(*checked);
    const auto result = body_->OnEvent(event);
    bool changed = false;
    for (std::size_t i = 0; i < checked_.size(); ++i)
      changed |= before[i] != *checked_[i];
    if (!readonly_ && changed) {
      auto ids = SplitLines(*value_);
      std::erase_if(ids, [&](const auto &id) {
        for (std::size_t i = 0; i < choices_.size(); ++i)
          if (choices_[i].value == id)
            return !*checked_[i];
        return false;
      });
      for (std::size_t i = 0; i < choices_.size(); ++i)
        if (*checked_[i] &&
            std::find(ids.begin(), ids.end(), choices_[i].value) == ids.end())
          ids.push_back(choices_[i].value);
      *value_ = JoinLines(ids);
    }
    return result || motion(event) || event.is_character();
  }

private:
  std::string *value_;
  std::vector<Choice> choices_;
  std::vector<std::unique_ptr<bool>> checked_;
  bool readonly_;
  Component body_;
};
class StringListControl final : public ComponentBase {
public:
  StringListControl(std::string *value, bool readonly, std::string label)
      : value_(value), readonly_(readonly), label_(std::move(label)) {
    table_ = std::make_shared<Table>(
        std::vector<Column>{{label_, 44}}, [this](const std::string &id) {
          const auto values = SplitLines(*value_);
          const auto index = static_cast<std::size_t>(std::stoul(id));
          if (index < values.size()) {
            edit_ = values[index];
            editing_ = static_cast<int>(index);
            input_->TakeFocus();
          }
        });
    input_ = TextInput(&edit_, readonly_);
    Components buttons;
    buttons.push_back(style::button("Add", [this] {
      if (edit_.empty()) {
        error_ = "Enter a value before adding an entry";
        return;
      }
      error_.clear();
      auto values = SplitLines(*value_);
      values.push_back(edit_);
      *value_ = JoinLines(values);
      refresh();
      edit_.clear();
      editing_ = -1;
      focus_input_after_event_ = true;
    }));
    buttons.push_back(action_button(
        "Update",
        [this] {
          if (edit_.empty()) {
            error_ = "Enter a value before updating this entry";
            return;
          }
          error_.clear();
          const auto index = editing_;
          auto values = SplitLines(*value_);
          if (index >= 0 && index < static_cast<int>(values.size()) &&
              !edit_.empty()) {
            values[static_cast<std::size_t>(index)] = edit_;
            *value_ = JoinLines(values);
            refresh();
          }
        },
        [this] {
          return editing_ >= 0 && editing_ < int(table_->row_count());
        }));
    buttons.push_back(action_button(
        "Delete",
        [this] {
          const auto index = table_->selected_index();
          auto values = SplitLines(*value_);
          if (index >= 0 && index < static_cast<int>(values.size())) {
            values.erase(values.begin() + index);
            *value_ = JoinLines(values);
            editing_ = -1;
            edit_.clear();
            refresh();
          }
        },
        [this] { return table_->row_count() > 0; }));
    buttons.push_back(action_button(
        "Up", [this] { move(-1); },
        [this] { return table_->selected_index() > 0; }));
    buttons.push_back(action_button(
        "Down", [this] { move(1); },
        [this] {
          return table_->selected_index() + 1 < int(table_->row_count());
        }));
    buttons_ = Container::Horizontal(buttons);
    body_ = readonly_ ? Container::Vertical({table_})
                      : Container::Vertical({table_, input_, buttons_});
    Add(body_);
    refresh();
  }
  Element OnRender() override {
    if (readonly_)
      return table_->Render() | size(HEIGHT, LESS_THAN, 5);
    return vbox(
        {table_->Render() | size(HEIGHT, EQUAL, 4),
         hbox({text(editing_ < 0 ? "New " + label_ + ": "
                                 : "Edit " + label_ + " #" +
                                       std::to_string(editing_ + 1) + ": "),
               input_->Render() | xflex}),
         buttons_->Render(),
         text(error_.empty() ? "" : "! " + error_) |
             style::ink(style::Role::Error)});
  }
  bool OnEvent(Event event) override {
    if (event == Event::Tab || event == Event::TabReverse) {
      if (readonly_)
        return false;
      const int direction = event == Event::Tab ? 1 : -1;
      const int current = body_->ActiveChild()->Index();
      if (current == 2) {
        for (int button = buttons_->ActiveChild()->Index() + direction;
             button >= 0 && button < int(buttons_->ChildCount());
             button += direction)
          if (buttons_->ChildAt(button)->Focusable()) {
            buttons_->ChildAt(button)->TakeFocus();
            return true;
          }
      }
      const int next = current + direction;
      if (next < 0 || next > 2)
        return false;
      body_->ChildAt(static_cast<std::size_t>(next))->TakeFocus();
      if (next == 2)
        buttons_->ChildAt(direction > 0 ? 0 : buttons_->ChildCount() - 1)
            ->TakeFocus();
      return true;
    }
    if (event == Event::Escape || event == Event::F1)
      return false;
    const bool handled = body_->OnEvent(event);
    if (focus_input_after_event_) {
      focus_input_after_event_ = false;
      input_->TakeFocus();
    }
    return handled || event.is_character() || motion(event);
  }

private:
  void refresh() {
    std::vector<Row> rows;
    const auto values = SplitLines(*value_);
    for (std::size_t i = 0; i < values.size(); ++i)
      rows.push_back({std::to_string(i), {values[i]}});
    table_->set_rows(std::move(rows));
  }
  void move(int delta) {
    auto values = SplitLines(*value_);
    const int index = table_->selected_index(), target = index + delta;
    if (target < 0 || target >= static_cast<int>(values.size()))
      return;
    std::swap(values[static_cast<std::size_t>(index)],
              values[static_cast<std::size_t>(target)]);
    *value_ = JoinLines(values);
    if (editing_ == index)
      editing_ = target;
    else if (editing_ == target)
      editing_ = index;
    refresh();
    table_->select_id(std::to_string(target));
  }
  std::string *value_;
  std::string error_;
  std::string edit_, label_;
  bool readonly_;
  bool focus_input_after_event_ = false;
  int editing_ = -1;
  std::shared_ptr<Table> table_;
  Component input_, buttons_, body_;
};
std::string safe_text(std::string line) {
  for (char &ch : line)
    if (static_cast<unsigned char>(ch) < 32 && ch != '\t')
      ch = ' ';
  return line;
}
} // namespace

std::vector<std::string> SplitLines(const std::string &value) {
  std::vector<std::string> result;
  std::size_t start = 0;
  if (value.empty())
    return result;
  while (start <= value.size()) {
    auto end = value.find('\n', start);
    result.push_back(
        value.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return result;
}
std::string JoinLines(const std::vector<std::string> &values) {
  std::string result;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i)
      result += '\n';
    result += values[i];
  }
  return result;
}
Component TextInput(std::string *value, bool readonly, bool secret,
                    bool multiline, bool disabled) {
  return std::make_shared<InputControl>(value, readonly, secret, multiline,
                                        disabled);
}
Component TextArea(std::string *value, bool readonly) {
  return TextInput(value, readonly, false, true);
}
Component ReadOnlyText(std::string *value) { return TextArea(value, true); }
Component Select(std::string *value, std::vector<Choice> choices,
                 bool readonly) {
  return std::make_shared<SelectControl>(value, std::move(choices), readonly);
}
Component MultiSelect(std::string *value, std::vector<Choice> choices,
                      bool readonly) {
  return std::make_shared<MultiSelectControl>(value, std::move(choices),
                                              readonly);
}
Component Toggle(std::string *value, bool readonly) {
  return std::make_shared<ToggleControl>(value, readonly);
}
Component StringList(std::string *value, bool readonly, std::string label) {
  return std::make_shared<StringListControl>(value, readonly, std::move(label));
}
Validator Integer(std::optional<long long> min, std::optional<long long> max,
                  bool optional) {
  return [=](const std::string &value) -> std::optional<std::string> {
    if (value.empty() && optional)
      return {};
    long long number{};
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size())
      return "Enter a whole number";
    if (min && number < *min)
      return "Minimum: " + std::to_string(*min);
    if (max && number > *max)
      return "Maximum: " + std::to_string(*max);
    return {};
  };
}

Form::Form(std::string title, std::vector<Field> fields, Submit submit,
           std::function<void()> cancel, bool readonly, std::string save_label)
    : title_(std::move(title)), fields_(std::move(fields)),
      submit_(std::move(submit)), cancel_(std::move(cancel)),
      readonly_(readonly) {
  // Field values and presence are the form draft. Baselines are retained only
  // for dirty checks and updated after a successful submit.
  save_label_ = save_label;
  errors_.resize(fields_.size());
  for (auto &field : fields_) {
    baseline_.push_back(field.value);
    baseline_present_.push_back(field.present);
    const bool ro = readonly_ || field.readonly || field.disabled;
    Component control;
    switch (field.kind) {
    case FieldKind::Select:
      control = Select(&field.value, field.choices, ro);
      break;
    case FieldKind::MultiSelect:
      control = MultiSelect(&field.value, field.choices, ro);
      break;
    case FieldKind::Checkbox:
      control = Toggle(&field.value, ro);
      break;
    case FieldKind::OptionalCheckbox:
      control = Select(&field.value,
                       {{"", "Not set · field omitted"},
                        {"true", "True"},
                        {"false", "False"}},
                       ro);
      break;
    case FieldKind::StringList:
      control = StringList(&field.value, ro, field.label);
      break;
    default:
      control = TextInput(&field.value, ro, field.kind == FieldKind::Secret,
                          field.kind == FieldKind::Multiline, field.disabled);
    }
    if (field.optional)
      control = std::make_shared<OptionalValueControl>(&field, control, ro);
    controls_.push_back(control);
  }
  Components buttons;
  if (!readonly_)
    buttons.push_back(
        style::button(std::move(save_label), [this] { this->submit(); }));
  buttons.push_back(style::button(readonly_ ? "Close" : "Cancel", [this] {
    if (cancel_)
      cancel_();
  }));
  buttons_ = Container::Horizontal(buttons);
  Components children = controls_;
  children.push_back(buttons_);
  body_ = Container::Vertical(children);
  Add(body_);
}
const std::string &Form::value(std::size_t index) const {
  return fields_.at(index).value;
}
void Form::set_value(std::size_t index, std::string value) {
  if (!value.empty())
    fields_.at(index).present = true;
  fields_.at(index).value = std::move(value);
}
void Form::set_error(std::string error) { error_ = std::move(error); }
void Form::set_field_error(std::size_t index, std::string error,
                           std::vector<std::size_t> related) {
  if (index >= errors_.size())
    return;
  errors_[index] = std::move(error);
  related_error_fields_ = std::move(related);
  related_error_fields_.push_back(index);
  focus_field(index);
}
bool Form::dirty() const {
  for (std::size_t i = 0; i < fields_.size(); ++i)
    if (fields_[i].value != baseline_[i] ||
        fields_[i].present != baseline_present_[i])
      return true;
  return false;
}
void Form::focus_field(std::size_t index) { controls_.at(index)->TakeFocus(); }
bool Form::submit() {
  // A deferred Save button callback has no event-dispatch owner. Successful
  // submission may detach the form, so retain it through the baseline update.
  const auto keep_alive = weak_from_this().lock();
  if (readonly_)
    return false;
  error_.clear();
  related_error_fields_.clear();
  bool valid = true;
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    errors_[i].clear();
    auto validate = fields_[i].validate;
    if (!validate && fields_[i].kind == FieldKind::Number)
      validate = Integer();
    if (validate && !fields_[i].disabled &&
        (!fields_[i].optional || fields_[i].present)) {
      if (auto error = validate(fields_[i].value)) {
        errors_[i] = *error;
        if (valid)
          focus_field(i);
        valid = false;
      }
    }
  }
  if (!valid)
    return false;
  if (submit_)
    if (auto error = submit_(*this)) {
      if (related_error_fields_.empty())
        error_ = *error;
      return false;
    }
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    baseline_[i] = fields_[i].value;
    baseline_present_[i] = fields_[i].present;
  }
  return true;
}
Element Form::OnRender() {
  Elements rows;
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    const auto &field = fields_[i];
    const bool focused = controls_[i]->Focused();
    const bool changed =
        field.value != baseline_[i] || field.present != baseline_present_[i];
    if (!field.section.empty())
      rows.push_back(text(field.section) | bold |
                     style::ink(style::Role::Accent));
    const auto label_text = std::string(focused ? "> " : "  ") + field.label +
                            (changed ? " *" : "") +
                            (field.disabled ? " [disabled]" : "");
    auto label = text(label_text);
    if (focused)
      label = label | bold;
    auto control = controls_[i]->Render();
    const bool block = field.kind == FieldKind::StringList ||
                       field.kind == FieldKind::Multiline ||
                       field.kind == FieldKind::MultiSelect;
    if (field.kind == FieldKind::Multiline)
      control = control | size(HEIGHT, EQUAL, field.optional ? 5 : 4);
    auto row = block ? vbox({label, hbox({text("  "), control | xflex})})
                     : hbox({label | size(WIDTH, EQUAL, 26), control | xflex});
    if (field.readonly && !readonly_)
      row = vbox({row, text("  Read only") | style::ink(style::Role::Muted)});
    if (!errors_[i].empty())
      row = vbox({row, paragraph("  ! " + field.label + ": " + errors_[i]) |
                           style::ink(style::Role::Error)});
    else if (!field.hint.empty() ||
             (field.kind == FieldKind::Secret && !readonly_))
      row = vbox({row, paragraph("  " + field.hint +
                                 (field.kind == FieldKind::Secret && !readonly_
                                      ? " Hidden · Ctrl-R reveals/hides the "
                                        "real value without changing it."
                                      : "")) |
                           style::ink(style::Role::Muted)});
    rows.push_back(row);
    rows.push_back(text(""));
  }
  auto submit_error = error_.empty() ? text("") | size(HEIGHT, EQUAL, 0)
                                     : paragraph("! " + error_) |
                                           style::ink(style::Role::Error);
  const auto active = body_->ActiveChild();
  const auto position = active && active != buttons_
                            ? " · field " +
                                  std::to_string(active->Index() + 1) + "/" +
                                  std::to_string(fields_.size())
                            : "";
  return vbox({style::heading(title_, std::string(readonly_ ? "READ ONLY"
                                                  : dirty() ? "UNSAVED *"
                                                            : "FORM") +
                                          position),
               separator() | style::rule(),
               vbox(std::move(rows)) | vscroll_indicator | yframe | flex,
               submit_error, separator() | style::rule(), buttons_->Render()});
}
bool Form::OnEvent(Event event) {
  if (event == Event::F1)
    return false;
  if (event == Event::Escape) {
    if (cancel_)
      cancel_();
    return true;
  }
  if (event == Event::Special(std::string(1, '\x13'))) {
    submit();
    return true;
  }
  if (event == Event::Tab || event == Event::TabReverse) {
    // Each field is one stop, including multi-choice fields and the button row.
    int index = body_->ActiveChild() ? body_->ActiveChild()->Index() : 0;
    const int count = static_cast<int>(controls_.size()) + 1;
    if (index < count - 1 &&
        (fields_[static_cast<std::size_t>(index)].kind ==
             FieldKind::StringList ||
         fields_[static_cast<std::size_t>(index)].optional) &&
        controls_[static_cast<std::size_t>(index)]->OnEvent(event))
      return true;
    if (index == count - 1) {
      const int target =
          buttons_->ActiveChild()->Index() + (event == Event::Tab ? 1 : -1);
      if (target >= 0 && target < static_cast<int>(buttons_->ChildCount())) {
        buttons_->ChildAt(static_cast<std::size_t>(target))->TakeFocus();
        return true;
      }
    }
    for (int n = 0; n < count; ++n) {
      index = (index + (event == Event::Tab ? 1 : count - 1)) % count;
      auto child = index == count - 1 ? buttons_ : controls_[index];
      if (child->Focusable()) {
        child->TakeFocus();
        if (index == count - 1)
          buttons_
              ->ChildAt(event == Event::Tab ? 0 : buttons_->ChildCount() - 1)
              ->TakeFocus();
        break;
      }
    }
    return true;
  }
  std::vector<std::string> before;
  std::vector<bool> present_before;
  before.reserve(fields_.size());
  present_before.reserve(fields_.size());
  for (const auto &field : fields_) {
    before.push_back(field.value);
    present_before.push_back(field.present);
  }
  body_->OnEvent(event);
  for (std::size_t i = 0; i < fields_.size(); ++i) {
    if (fields_[i].value == before[i] &&
        fields_[i].present == present_before[i])
      continue;
    // A changed field has no old validation result. Keep unrelated errors
    // until that field is actually corrected or the form is submitted again.
    errors_[i].clear();
    if (std::find(related_error_fields_.begin(), related_error_fields_.end(),
                  i) != related_error_fields_.end()) {
      for (auto field : related_error_fields_)
        errors_[field].clear();
      related_error_fields_.clear();
    }
    error_.clear();
  }
  return true; // Modal form input never reaches background page actions.
}

Table::Table(std::vector<Column> columns,
             std::function<void(const std::string &)> activate)
    : columns_(std::move(columns)), activate_(std::move(activate)) {
  MenuOption options = MenuOption::Vertical();
  options.entries_option.transform = [this](const EntryState &state) {
    const auto index = static_cast<std::size_t>(state.index);
    Elements cells;
    cells.push_back(text(state.active ? (ascii_ ? "> " : "▸ ") : "  "));
    if (index < rows_.size()) {
      for (std::size_t i = 0; i < rows_[index].cells.size(); ++i) {
        const int width = i < rendered_widths_.size() ? rendered_widths_[i]
                          : i < columns_.size()       ? columns_[i].width
                                                      : 24;
        cells.push_back(columns_.empty()
                            ? text(rows_[index].cells[i]) | xflex
                            : text(Clip(rows_[index].cells[i], width)) |
                                  size(WIDTH, EQUAL, width));
        if (!columns_.empty())
          cells.push_back(text(" "));
      }
    }
    auto row = hbox(std::move(cells));
    if (state.active)
      row = row | style::selected(state.focused) | focus;
    return row;
  };
  options.on_enter = [this] {
    if (activate_ && !selected_id().empty())
      activate_(selected_id());
  };
  menu_ = Menu(&labels_, &selected_, options);
  Add(menu_);
}
void Table::set_rows(std::vector<Row> rows, std::string empty) {
  // Preserve selection by stable object ID across refresh/reordering, not by
  // a row index that may now name another object.
  const std::string keep = selected_id();
  rows_ = std::move(rows);
  labels_.clear();
  for (const auto &row : rows_)
    labels_.push_back(row.id);
  empty_ = std::move(empty);
  selected_ =
      std::clamp(selected_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
  select_id(keep);
}
const std::string &Table::selected_id() const {
  return selected_ >= 0 && selected_ < static_cast<int>(rows_.size())
             ? rows_[selected_].id
             : no_id_;
}
std::string Table::selected_summary() const {
  if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size()))
    return {};
  const auto &row = rows_[static_cast<std::size_t>(selected_)];
  std::string result;
  for (const auto &cell : row.cells) {
    if (cell.empty())
      continue;
    if (!result.empty())
      result += " · ";
    result += cell;
    if (result.size() > 96)
      break;
  }
  return Clip(result.empty() ? row.id : result, 96);
}
void Table::select_id(const std::string &id) {
  for (std::size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].id == id) {
      selected_ = static_cast<int>(i);
      break;
    }
}
Element Table::OnRender() {
  rendered_widths_.clear();
  int used = 3; // row marker and scroll indicator
  for (const auto &column : columns_) {
    rendered_widths_.push_back(column.width);
    used += column.width + 1;
  }
  int spare = std::max(0, box_.x_max - box_.x_min + 1 - used);
  for (std::size_t i = 0; i < columns_.size() && spare > 0; ++i) {
    int needed = string_width(columns_[i].label);
    for (const auto &row : rows_)
      if (i < row.cells.size())
        needed = std::max(needed, string_width(row.cells[i]));
    const int extra = std::min(
        spare, std::max(0, std::min(64, needed) - rendered_widths_[i]));
    rendered_widths_[i] += extra;
    spare -= extra;
  }
  Elements result;
  if (!columns_.empty()) {
    Elements columns{text("  ")};
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      const auto &column = columns_[i];
      columns.push_back(text(column.label) | bold |
                        style::ink(style::Role::Text) |
                        size(WIDTH, EQUAL, rendered_widths_[i]));
      columns.push_back(text(" "));
    }
    result.push_back(hbox(std::move(columns)));
    if (!compact_)
      result.push_back(separator() | style::rule());
  }
  result.push_back(
      rows_.empty()
          ? vbox({text(""), paragraph(empty_) | style::ink(style::Role::Muted)})
          : menu_->Render() | vscroll_indicator | frame | flex);
  return vbox(std::move(result)) | flex | reflect(box_);
}
bool Table::OnEvent(Event event) {
  if (rows_.empty())
    return motion(event) || event == Event::Return;
  return menu_->OnEvent(event);
}

SectionTree::SectionTree(std::vector<TreeNode> nodes,
                         std::function<void(const std::string &)> activate)
    : nodes_(std::move(nodes)),
      table_(
          std::make_shared<Table>(std::vector<Column>{}, std::move(activate))) {
  Add(table_);
  rebuild();
}
void SectionTree::rebuild() {
  visible_.clear();
  std::vector<Row> rows;
  const auto walk = [&](auto &&self, std::vector<TreeNode> &nodes,
                        int depth) -> void {
    for (auto &node : nodes) {
      visible_.push_back(&node);
      rows.push_back({node.id,
                      {std::string(depth * 2, ' ') +
                       (node.children.empty() ? "  "
                        : node.expanded       ? "- "
                                              : "+ ") +
                       node.label}});
      if (node.expanded)
        self(self, node.children, depth + 1);
    }
  };
  walk(walk, nodes_, 0);
  table_->set_rows(std::move(rows));
}
Element SectionTree::OnRender() { return table_->Render(); }
std::string SectionTree::selected_id() const { return table_->selected_id(); }
bool SectionTree::OnEvent(Event event) {
  if ((event == Event::ArrowLeft || event == Event::ArrowRight) &&
      !visible_.empty()) {
    auto *node = visible_[static_cast<std::size_t>(table_->selected_index())];
    node->expanded = event == Event::ArrowRight;
    rebuild();
    return true;
  }
  return table_->OnEvent(event);
}

CollectionEditor::CollectionEditor(std::string title,
                                   std::vector<Column> columns,
                                   CollectionActions actions, bool readonly)
    : title_(std::move(title)), actions_(std::move(actions)),
      readonly_(readonly) {
  table_ = std::make_shared<Table>(std::move(columns), actions_.view);
  Components buttons;
  const auto selected = [this](const auto &fn) {
    if (fn && !table_->selected_id().empty())
      fn(table_->selected_id());
  };
  if (actions_.view)
    buttons.push_back(action_button(
        "View", [this, selected] { selected(actions_.view); },
        [this] { return table_->row_count() > 0; }));
  if (!readonly_) {
    if (actions_.add)
      buttons.push_back(style::button("Add", actions_.add));
    if (actions_.edit)
      buttons.push_back(action_button(
          "Edit", [this, selected] { selected(actions_.edit); },
          [this] { return table_->row_count() > 0; }));
    if (actions_.copy)
      buttons.push_back(action_button(
          "Copy", [this, selected] { selected(actions_.copy); },
          [this] { return table_->row_count() > 0; }));
    if (actions_.remove)
      buttons.push_back(action_button(
          "Delete", [this, selected] { selected(actions_.remove); },
          [this] { return table_->row_count() > 0; }));
    if (actions_.move) {
      buttons.push_back(action_button(
          "Up",
          [this] {
            if (!table_->selected_id().empty())
              actions_.move(table_->selected_id(), -1);
          },
          [this] { return table_->selected_index() > 0; }));
      buttons.push_back(action_button(
          "Down",
          [this] {
            if (!table_->selected_id().empty())
              actions_.move(table_->selected_id(), 1);
          },
          [this] {
            return table_->selected_index() + 1 < int(table_->row_count());
          }));
    }
  }
  controls_ = Container::Horizontal(buttons);
  Add(Container::Vertical({table_, controls_}));
}
void CollectionEditor::set_rows(std::vector<Row> rows) {
  table_->set_rows(std::move(rows));
}
Element CollectionEditor::OnRender() {
  const bool rows_focused = table_->Focused();
  const auto selection = table_->selected_summary();
  return vbox(
      {style::heading(title_, readonly_ ? "READ ONLY" : "DRAFT COLLECTION"),
       text(rows_focused ? "Focus: rows · Up/Down choose · Tab for actions"
                         : "Focus: actions · Shift-Tab for rows") |
           style::ink(style::Role::Muted),
       table_->Render() | flex,
       text(selection.empty() ? "Selected: none" : "Selected: " + selection) |
           style::ink(style::Role::Muted),
       text(notice_) | style::ink(style::Role::Muted), controls_->Render()});
}
bool CollectionEditor::OnEvent(Event event) {
  // Moving focus to the action buttons does not change table selection. Their
  // callbacks use that still-visible selected ID.
  if (event == Event::Tab && table_->Focused() && controls_->ChildCount() > 0) {
    controls_->ChildAt(0)->TakeFocus();
    return true;
  }
  if (event == Event::TabReverse && controls_->Focused()) {
    table_->TakeFocus();
    return true;
  }
  if (event == Event::Special("\x13")) {
    notice_ = readonly_
                  ? "Read only; no draft change to accept"
                  : "Collection changes already live in module draft; close "
                    "this list, then save Source in the module workspace";
    return true;
  }
  if ((event == Event::ArrowUp || event == Event::ArrowDown) &&
      !table_->Focused()) {
    table_->TakeFocus();
    return table_->OnEvent(event);
  }
  if (!readonly_) {
    const auto id = table_->selected_id();
    if (event == Event::Character('a') && actions_.add) {
      actions_.add();
      return true;
    }
    if (!id.empty()) {
      if (event == Event::Character('e') && actions_.edit) {
        actions_.edit(id);
        return true;
      }
      if (event == Event::Character('c') && actions_.copy) {
        actions_.copy(id);
        return true;
      }
      if (event == Event::Character('D') && actions_.remove) {
        actions_.remove(id);
        return true;
      }
      if (event == Event::Character('+') && actions_.move &&
          table_->selected_index() > 0) {
        actions_.move(id, -1);
        return true;
      }
      if (event == Event::Character('-') && actions_.move &&
          table_->selected_index() + 1 < int(table_->row_count())) {
        actions_.move(id, 1);
        return true;
      }
    }
  }
  return ComponentBase::OnEvent(event);
}

std::vector<HelpEntry> CollectionEditor::help() const {
  std::vector<HelpEntry> result;
  const auto id = table_->selected_id();
  auto add = [&](std::string key, std::string label, std::function<void()> run,
                 std::string reason = "") {
    result.push_back(
        {std::move(key), std::move(label), title_, std::move(reason),
         std::move(run),
         "Changes stay in the parent draft until its Save action."});
  };
  if (actions_.view)
    add(
        "Enter", "View entry", [fn = actions_.view, id] { fn(id); },
        id.empty() ? "Choose an entry" : "");
  if (!readonly_) {
    if (actions_.add)
      add("a", "Add entry", actions_.add);
    if (actions_.edit)
      add(
          "e", "Edit entry", [fn = actions_.edit, id] { fn(id); },
          id.empty() ? "Choose an entry" : "");
    if (actions_.copy)
      add(
          "c", "Copy entry", [fn = actions_.copy, id] { fn(id); },
          id.empty() ? "Choose an entry" : "");
    if (actions_.remove)
      add(
          "D", "Delete entry", [fn = actions_.remove, id] { fn(id); },
          id.empty() ? "Choose an entry" : "");
    if (actions_.move) {
      add(
          "+", "Move entry up", [fn = actions_.move, id] { fn(id, -1); },
          table_->selected_index() > 0 ? "" : "Already first");
      add(
          "-", "Move entry down", [fn = actions_.move, id] { fn(id, 1); },
          table_->selected_index() + 1 < int(table_->row_count())
              ? ""
              : "Already last");
    }
  }
  result.push_back(
      {"Tab / Shift-Tab", "Move between collection and buttons", title_, ""});
  result.push_back({"Up / Down", "Choose entry", title_, ""});
  return result;
}

namespace {
class DialogControl final : public ComponentBase {
public:
  DialogControl(std::string title, Component content,
                std::function<void()> close, std::vector<HelpEntry> actions)
      : title_(std::move(title)), content_(std::move(content)),
        close_(std::move(close)), actions_(std::move(actions)) {
    Add(content_);
  }
  Element OnRender() override {
    return vbox({style::heading(title_), separator() | style::rule(),
                 content_->Render() | flex});
  }
  bool OnEvent(Event event) override {
    if (event == Event::F1)
      return false;
    if (event == Event::Escape) {
      if (close_)
        close_();
      return true;
    }
    content_->OnEvent(event);
    return true;
  }
  std::vector<HelpEntry> help() const {
    auto entries = actions_;
    if (entries.empty())
      entries.push_back({"Esc", "Close dialog", title_, "", close_});
    entries.push_back({"Tab / Shift-Tab", "Choose a control", title_, ""});
    entries.push_back({"Enter", "Activate selected control", title_, ""});
    return entries;
  }

private:
  std::string title_;
  Component content_;
  std::function<void()> close_;
  std::vector<HelpEntry> actions_;
};
} // namespace
std::vector<HelpEntry> ContextHelp(const Component &component) {
  if (!component)
    return {};
  if (const auto dialog = std::dynamic_pointer_cast<DialogControl>(component))
    return dialog->help();
  if (const auto collection =
          std::dynamic_pointer_cast<CollectionEditor>(component))
    return collection->help();
  if (const auto form = std::dynamic_pointer_cast<Form>(component)) {
    std::vector<HelpEntry> result{
        {"Tab / Shift-Tab", "Move between fields and buttons", "Current form",
         ""},
        {"Arrows / Home / End",
         "Move cursor or choose visible option immediately", "Focused control",
         ""},
        {"Ctrl-Home / Ctrl-End", "Document start / end", "Multiline text", ""},
        {"Space", "Toggle focused checkbox or presence", "Focused control", ""},
        {"Enter",
         "Insert newline in multiline text / activate button; no checkbox "
         "change",
         "Focused control", ""},
        {"Ctrl-S", "Save / accept form", "Current draft",
         form->readonly() ? "Read only" : ""},
        {"Esc", "Cancel / close form", "Current draft", ""},
        {"q e u p ? Space", "Text in input fields", "No background action",
         ""}};
    const auto body = form->ActiveChild();
    const auto field = body ? body->ActiveChild() : Component{};
    if (field && field->Index() < static_cast<int>(form->fields().size())) {
      const auto &definition =
          form->fields()[static_cast<std::size_t>(field->Index())];
      if (definition.kind == FieldKind::Secret)
        result.push_back({"Ctrl-R", "Reveal / hide field", definition.label, "",
                          [form] { form->OnEvent(Event::Special("\x12")); },
                          "Display the real editable value; its stored content "
                          "is unchanged."});
      if (definition.kind == FieldKind::StringList)
        result.push_back(
            {"Add / Update / Delete / Up / Down",
             "Activate the labelled list button with Enter", definition.label,
             form->readonly() || definition.readonly ? "Read only" : ""});
      if (definition.kind == FieldKind::OptionalCheckbox)
        result.push_back(
            {"Left / Right / Up / Down", "Choose omitted, true or false now",
             definition.label,
             form->readonly() || definition.readonly ? "Read only" : ""});
    }
    for (std::size_t i = 0; i < form->fields().size(); ++i)
      if (!form->fields()[i].section.empty())
        result.push_back({"Jump", "Go to " + form->fields()[i].section,
                          form->title(), "",
                          [form, i] { form->focus_field(i); },
                          "The same object draft remains open."});
    for (auto &entry : result) {
      if (entry.key == "Ctrl-S") {
        entry.action = form->save_label();
        entry.target = form->title();
        entry.run = [form] { form->submit(); };
        entry.consequence = form->context();
      } else if (entry.key == "Esc") {
        entry.target = form->title();
        entry.run = [form] { form->cancel(); };
        entry.consequence =
            "Changed values require Save, Discard or Continue editing.";
      }
    }
    return result;
  }
  if (std::dynamic_pointer_cast<Table>(component))
    return {{"Arrows / PgUp / PgDn", "Browse rows; Home / End jump",
             "Current list", ""}};
  if (std::dynamic_pointer_cast<LogViewer>(component))
    return {{"p", "Pause / follow", "Logs", ""},
            {"Up / Down / PageUp / PageDown", "Browse history", "Logs", ""},
            {"End", "Resume follow", "Logs", ""}};
  if (std::dynamic_pointer_cast<SectionTree>(component))
    return {{"Up / Down", "Choose section", "Native sections", ""},
            {"Left / Right", "Collapse / expand", "Hierarchical sections", ""},
            {"Enter", "Open section", "Selected section", "",
             [component] { component->OnEvent(Event::Return); }}};
  if (std::dynamic_pointer_cast<InputControl>(component))
    return {{"Arrows / Home / End", "Move cursor", "Text", ""},
            {"Ctrl-Home / Ctrl-End", "Document start / end", "Text", ""},
            {"Esc", "Close one layer", "Editor", ""}};
  for (std::size_t index = 0; index < component->ChildCount(); ++index) {
    auto result = ContextHelp(component->ChildAt(index));
    if (!result.empty())
      return result;
  }
  return {};
}

Component Dialog(std::string title, Component content,
                 std::function<void()> close, std::vector<HelpEntry> actions) {
  return std::make_shared<DialogControl>(std::move(title), std::move(content),
                                         std::move(close), std::move(actions));
}
Component Confirm(std::string title, std::string message,
                  std::function<void()> accept, std::function<void()> cancel) {
  auto confirm = style::button("Confirm", accept);
  auto safe = style::button("Cancel", cancel);
  auto buttons = Container::Horizontal({confirm, safe});
  safe->TakeFocus();
  auto body = Renderer(buttons, [message, buttons] {
    return vbox({paragraph(message), text(""), buttons->Render()});
  });
  std::vector<HelpEntry> actions{
      {"Enter on Cancel", "Cancel", title, "", cancel,
       "Return without confirming."},
      {"Enter on Confirm", "Confirm", title, "", accept, message},
      {"Esc", "Cancel", title, "", cancel, "Return without confirming."}};
  return Dialog(std::move(title), body, cancel, std::move(actions));
}
Component HelpOverlay(std::vector<HelpEntry> entries,
                      std::function<void()> close, std::string context,
                      std::string prerequisites) {
  auto selected = std::make_shared<int>(0);
  auto choices = std::make_shared<std::vector<HelpEntry>>();
  auto labels = std::make_shared<std::vector<std::string>>();
  auto instructions = std::make_shared<Elements>();
  for (auto &entry : entries) {
    if (entry.run) {
      labels->push_back(entry.action);
      choices->push_back(std::move(entry));
    } else
      instructions->push_back(
          hbox({paragraph(entry.key) | size(WIDTH, EQUAL, 24) |
                    style::ink(style::Role::Accent),
                paragraph(entry.action +
                          (entry.disabled_reason.empty()
                               ? ""
                               : " · Unavailable: " + entry.disabled_reason)) |
                    xflex}));
  }
  auto options = MenuOption::Vertical();
  options.entries_option.transform = [choices](const EntryState &state) {
    const auto &entry = choices->at(state.index);
    auto row = hbox({text(state.active ? "> " : "  "),
                     text(Clip(entry.key, 16)) | size(WIDTH, EQUAL, 16) |
                         style::ink(style::Role::Accent),
                     text(entry.action +
                          (entry.target.empty() ? "" : " · " + entry.target)) |
                         xflex});
    if (!entry.disabled_reason.empty())
      row = row | style::ink(style::Role::Disabled);
    return state.active ? row | style::selected(state.focused) | focus : row;
  };
  options.on_enter = [choices, selected] {
    if (*selected >= 0 && *selected < int(choices->size())) {
      auto run = choices->at(*selected).run;
      if (run && choices->at(*selected).disabled_reason.empty())
        run();
    }
  };
  auto menu = Menu(labels.get(), selected.get(), options);
  auto body = Renderer(menu, [=] {
    (void)labels; // Own the list referenced by Menu across renders and events.
    const bool compact = Terminal::Size().dimy < 30;
    Elements rows{paragraph(context) | bold |
                  size(HEIGHT, LESS_THAN, compact ? 2 : 4)};
    if (!prerequisites.empty())
      rows.push_back(
          paragraph(Clip(prerequisites,
                         std::max(40, Terminal::Size().dimx - 12) * 2)) |
          style::ink(style::Role::Muted) |
          size(HEIGHT, LESS_THAN, compact ? 2 : 4));
    rows.push_back(
        text("Tasks · arrows choose; Enter executes available task") |
        style::ink(style::Role::Muted));
    rows.push_back(menu->Render() | vscroll_indicator | frame | flex);
    if (!choices->empty() && !choices->at(*selected).disabled_reason.empty())
      rows.push_back(
          paragraph("Unavailable: " + choices->at(*selected).disabled_reason) |
          style::ink(style::Role::Warning) |
          size(HEIGHT, LESS_THAN, compact ? 2 : 4));
    else if (!choices->empty() &&
             choices->at(*selected).consequence != prerequisites &&
             !choices->at(*selected).consequence.empty())
      rows.push_back(
          paragraph(Clip(choices->at(*selected).consequence,
                         std::max(40, Terminal::Size().dimx - 12) * 2)) |
          style::ink(style::Role::Muted) |
          size(HEIGHT, LESS_THAN, compact ? 2 : 4));
    rows.push_back(separator() | style::rule());
    if (compact)
      rows.push_back(
          text("Tab moves · Space toggles · Enter activates · Esc closes") |
          style::ink(style::Role::Muted));
    else {
      rows.push_back(text("Navigation and input") | bold);
      rows.push_back(vbox(*instructions) | size(HEIGHT, LESS_THAN, 7));
    }
    rows.push_back(text("Esc closes Help and restores your place") |
                   style::ink(style::Role::Muted));
    return vbox(std::move(rows));
  });
  return Dialog("Help & actions", body, std::move(close));
}

void ToastHost::push(std::string key, std::string message, Tone tone,
                     Clock::time_point now) {
  // Toasts are ephemeral overlays. History keeps prior notifications without
  // forcing them into the page document flow.
  expire(now);
  if (suspended_at_)
    now = *suspended_at_;
  const auto duration = tone == Tone::Error     ? std::chrono::seconds(8)
                        : tone == Tone::Warning ? std::chrono::seconds(5)
                                                : std::chrono::seconds(3);
  for (auto &toast : visible_) {
    if (toast.key == key && toast.message == message) {
      ++toast.repeats;
      for (auto it = history_.rbegin(); it != history_.rend(); ++it)
        if (it->key == key && it->message == message) {
          ++it->repeats;
          break;
        }
      toast.expires = now + duration;
      return;
    }
  }
  Toast toast{std::move(key), std::move(message), tone, now + duration};
  visible_.push_back(toast);
  history_.push_back(std::move(toast));
  while (visible_.size() > 3)
    visible_.pop_front();
  while (history_.size() > 30)
    history_.pop_front();
}
void ToastHost::suspended(bool value, Clock::time_point now) {
  // Modal dialogs pause the visible lifetime instead of expiring hidden toast
  // messages while the user cannot see them.
  if (value && !suspended_at_)
    suspended_at_ = now;
  else if (!value && suspended_at_) {
    for (auto &toast : visible_)
      toast.expires += now - *suspended_at_;
    suspended_at_.reset();
  }
}
void ToastHost::expire(Clock::time_point now) {
  if (suspended_at_)
    return;
  std::erase_if(visible_,
                [&](const auto &toast) { return toast.expires <= now; });
}
Element ToastHost::render() const {
  Elements cards;
  const bool compact = Terminal::Size().dimy < 30;
  const auto begin = compact && visible_.size() > 1 ? visible_.size() - 1 : 0;
  for (std::size_t index = begin; index < visible_.size(); ++index) {
    const auto &toast = visible_[index];
    const std::string label = toast.tone == Tone::Error     ? "! Error"
                              : toast.tone == Tone::Warning ? "! Warning"
                              : toast.tone == Tone::Success ? "+ Success"
                                                            : "i Info";
    const auto role = toast.tone == Tone::Error     ? style::Role::Error
                      : toast.tone == Tone::Warning ? style::Role::Warning
                      : toast.tone == Tone::Success ? style::Role::Success
                                                    : style::Role::Accent;
    auto glyphs = Utf8ToGlyphs(safe_text(toast.message));
    std::string message;
    for (std::size_t i = 0;
         i < std::min<std::size_t>(glyphs.size(), compact ? 120 : 180); ++i)
      message += glyphs[i];
    if (glyphs.size() > (compact ? 120u : 180u))
      message += "...";
    if (toast.repeats > 1)
      message += " · repeated " + std::to_string(toast.repeats) + " times";
    cards.push_back(
        hbox({text(" "),
              vbox({text(label + (begin ? " · history in Help" : "")) | bold |
                        style::ink(role),
                    paragraph(message)}) |
                  size(WIDTH, EQUAL, 32),
              text(" ")}) |
        border | style::panel());
  }
  return vbox(
      {hbox({filler(), hbox({text(" "), vbox(std::move(cards))}), text(" ")}),
       filler()});
}

Element InfoPane(std::string object, std::string details, std::string status) {
  return vbox({text(Clip(object, Terminal::Size().dimx - 2)) |
                   style::ink(style::Role::Muted),
               text(Clip(details, Terminal::Size().dimx - 2)),
               hbox({text(Clip(status, Terminal::Size().dimx - 22)) | xflex |
                         style::ink(style::Role::Muted),
                     text("F1 Help & actions") |
                         style::ink(style::Role::Accent)})}) |
         size(HEIGHT, EQUAL, 3);
}
} // namespace xrm::tui::ui

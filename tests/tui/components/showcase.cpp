// XRM: Runs a standalone component showcase for terminal inspection.

#include "tui/components/widgets.hpp"

#include <atomic>
#include <chrono>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>

using namespace ftxui;
using namespace xrm::tui::ui;
namespace {
struct FlowControl {
  termios saved{};
  bool active = tcgetattr(STDIN_FILENO, &saved) == 0;
  FlowControl() {
    if (active) {
      auto next = saved;
      next.c_iflag &= ~IXON;
      tcsetattr(STDIN_FILENO, TCSANOW, &next);
    }
  }
  ~FlowControl() {
    if (active)
      tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  }
};
} // namespace
int main() {
  FlowControl flow;
  auto screen = ScreenInteractive::Fullscreen();
  ToastHost toasts;
  Component overlay;
  std::string info = "Demo data; no daemon or filesystem writes";
  std::vector<Field> fields{
      {"Name", "中文 sample"},
      {"Secret", "demo-secret", FieldKind::Secret},
      {"Port",
       "443",
       FieldKind::Number,
       {},
       "1–65535",
       Integer(1, 65535, false)},
      {"Strategy",
       "random",
       FieldKind::Select,
       {{"random", "Random"}, {"leastPing", "Least latency"}}},
      {"Enabled", "", FieldKind::OptionalCheckbox},
      {"Members",
       "b",
       FieldKind::MultiSelect,
       {{"a", "Node A"}, {"b", "Node B"}, {"c", "Node C"}}},
      {"Domains", "example.test\nlocal.test", FieldKind::StringList},
      {"Description", "First line\nSecond line", FieldKind::Multiline},
      {"Read only", "Retained value", FieldKind::Text, {}, "", {}, true},
      {"Disabled", "Unavailable", FieldKind::Text, {}, "", {}, false, true}};
  auto form = std::make_shared<Form>(
      "Multi-field resource", fields,
      [&](Form &) -> std::optional<std::string> {
        toasts.push("save", "Demo resource saved", Tone::Success);
        return {};
      },
      [&] {
        overlay = Confirm("Discard draft?", "Close showcase?",
                          screen.ExitLoopClosure(), [&] { overlay.reset(); });
      });
  auto table = std::make_shared<Table>(
      std::vector<Column>{{"Name", 28}, {"Status", 16}, {"Origin", 20}},
      [&](const std::string &id) { info = "Viewed object " + id; });
  std::vector<Row> rows;
  for (int i = 1; i <= 60; ++i)
    rows.push_back(
        {std::to_string(i),
         {"Resource " + std::to_string(i), "ready", "isolated demo"}});
  table->set_rows(std::move(rows));
  auto tree = std::make_shared<SectionTree>(
      std::vector<TreeNode>{
          {"routing",
           "Routing",
           {{"rules", "Rules", {}}, {"balancers", "Balancers", {}}}},
          {"policy",
           "Policy",
           {{"levels", "Levels", {}}, {"system", "System", {}}}}},
      [&](const auto &id) { info = "Viewed section " + id; });
  auto logs = std::make_shared<LogViewer>();
  std::vector<std::string> records;
  for (int i = 0; i < 80; ++i)
    records.push_back("12:34:56 [Info] Example log record " +
                      std::to_string(i));
  records.push_back(
      "[Warning] Multiple lines\ncontinued without changing source severity");
  logs->set_lines(records);
  std::string document =
      "{\n  \"example\": \"Edit 中文 normally\",\n  \"list\": [1, 2, 3]\n}";
  auto editor = TextArea(&document);
  int page = 0;
  std::vector<std::string> tabs{"Form", "Table", "Tree", "Logs", "Editor"};
  auto navigation = Menu(&tabs, &page, MenuOption::Horizontal());
  auto content = Container::Tab({form, table, tree, logs, editor}, &page);
  auto body = Container::Vertical({navigation, content});
  bool in_content = false;
  auto shell = Renderer(body, [&] {
    const auto dimensions = Terminal::Size();
    Element element;
    if (dimensions.dimx < 80 || dimensions.dimy < 24) {
      element = vbox({text("Resize to at least 80 × 24"),
                      text("Draft and focus are retained")}) |
                center;
    } else {
      element = vbox(
          {navigation->Render(), separator(),
           dbox({content->Render() | flex, toasts.render()}) | flex,
           InfoPane(tabs[page], info, in_content ? "Content" : "Navigation")});
    }
    if (overlay)
      element = dbox({element, overlay->Render()});
    return element;
  });
  auto root = CatchEvent(shell, [&](Event event) {
    toasts.expire();
    if (event == Event::Custom)
      return true;
    if (event == Event::F1) {
      auto previous = overlay;
      overlay = HelpOverlay(
          {{"F1", "Help", "All fields; ? remains editable text", ""},
           {"Tab / Shift-Tab", "Move focus", "Current form or navigation", ""},
           {"Home / End", "Line start / end", "Text editor", ""},
           {"Ctrl-Home / Ctrl-End", "Document start / end", "Text editor", ""},
           {"Ctrl-S", "Save form", "Validation retains all values", ""},
           {"Esc", "Close one layer", "Dialog / content", ""},
           {"t", "Show toast", "Navigation", ""},
           {"q", "Exit", "Navigation", ""}},
          [&, previous] { overlay = previous; });
      return true;
    }
    if (overlay) {
      auto current = overlay;
      current->OnEvent(event);
      return true;
    }
    if (!in_content) {
      if (event == Event::Character('q')) {
        screen.Exit();
        return true;
      }
      if (event == Event::Character('t')) {
        toasts.push("demo", "Demo notification; focus is unchanged",
                    Tone::Info);
        return true;
      }
      if (event == Event::Return) {
        in_content = true;
        content->TakeFocus();
        return true;
      }
      if (event == Event::Tab || event == Event::TabReverse) {
        page = (page + (event == Event::Tab ? 1 : 4)) % 5;
        return true;
      }
      return true;
    }
    if (event == Event::Escape) {
      in_content = false;
      navigation->TakeFocus();
      return true;
    }
    content->OnEvent(event);
    return true;
  });
  std::jthread clock([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      screen.PostEvent(Event::Custom);
    }
  });
  screen.Loop(root);
  clock.request_stop();
  return 0;
}

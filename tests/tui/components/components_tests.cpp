#include "tui/components/widgets.hpp"
#include "tui/components/style.hpp"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/screen/screen.hpp>

using namespace xrm::tui::ui;
using ftxui::Event;
namespace {
std::string frame(ftxui::Component component, int width = 80, int height = 24) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, component->Render());
  return screen.ToString();
}
}
TEST_CASE("component input owns cursor and printable shortcuts", "[tui][components]") {
  std::string value = "甲乙ab";
  auto input = TextInput(&value);
  input->OnEvent(Event::End);
  input->OnEvent(Event::ArrowLeft);
  input->OnEvent(Event::Character("中"));
  CHECK(value == "甲乙a中b");
  input->OnEvent(Event::Backspace);
  CHECK(value == "甲乙ab");
  input->OnEvent(Event::Home);
  input->OnEvent(Event::Delete);
  CHECK(value == "乙ab");
  for (auto c : std::string("qeucdp? ")) CHECK(input->OnEvent(Event::Character(c)));
  CHECK(value == "qeucdp? 乙ab");
  CHECK(input->OnEvent(Event::ArrowUp));
  CHECK(input->OnEvent(Event::ArrowDown));
  CHECK_FALSE(input->OnEvent(Event::F1));
}
TEST_CASE("editing an existing scalar starts at its end", "[tui][components]") {
  std::string value = "existing";
  auto input = TextInput(&value);
  input->OnEvent(Event::Character("!"));
  CHECK(value == "existing!");
  input->OnEvent(Event::Backspace);
  CHECK(value == "existing");
}
TEST_CASE("multiline input keeps line navigation and newline semantics", "[tui][components]") {
  std::string value = "first\nsecond";
  auto input = TextArea(&value);
  input->OnEvent(Event::Special("\x1b[1;5F"));
  input->OnEvent(Event::Home);
  input->OnEvent(Event::Character("X"));
  CHECK(value == "first\nXsecond");
  input->OnEvent(Event::ArrowUp);
  input->OnEvent(Event::End);
  input->OnEvent(Event::Return);
  CHECK(value == "first\n\nXsecond");
  const auto before = value;
  auto readonly = TextArea(&value, true);
  readonly->OnEvent(Event::Delete);
  readonly->OnEvent(Event::Character("oops"));
  readonly->OnEvent(Event::Return);
  CHECK(value == before);
}
TEST_CASE("form cycles all fields and buttons; invalid save preserves draft", "[tui][components]") {
  int saves = 0;
  std::vector<Field> fields{{"Name", "alpha"}, {"Port", "80", FieldKind::Number}};
  fields[1].validate = Integer(1, 65535, false);
  auto form = std::make_shared<Form>("Resource", fields, [&](Form&) -> std::optional<std::string> { ++saves; return "Server disconnected"; }, [] {});
  form->OnEvent(Event::End);
  form->OnEvent(Event::Character(" beta"));
  form->OnEvent(Event::Tab);
  form->OnEvent(Event::End);
  form->OnEvent(Event::Character("x"));
  CHECK_FALSE(form->submit());
  CHECK(saves == 0);
  CHECK(form->value(0) == "alpha beta");
  CHECK(form->value(1) == "80x");
  CHECK(frame(form).find("Enter a whole number") != std::string::npos);
  form->OnEvent(Event::Backspace);
  CHECK_FALSE(form->submit());
  CHECK(saves == 1);
  CHECK(form->dirty());
  CHECK(form->error() == "Server disconnected");
  form->focus_field(0);
  form->OnEvent(Event::Tab); // Port
  form->OnEvent(Event::Tab); // Save
  form->OnEvent(Event::Tab); // Cancel
  form->OnEvent(Event::Tab); // Name, no trap in buttons
  form->OnEvent(Event::End);
  form->OnEvent(Event::Character("!"));
  CHECK(form->value(0) == "alpha beta!");
}
TEST_CASE("form survives being closed by its save callback", "[tui][components]") {
  std::shared_ptr<Form> owner;
  std::weak_ptr<Form> weak;
  owner = std::make_shared<Form>(
      "Appearance", std::vector<Field>{{"Theme", "dark"}},
      [&](Form& form) -> std::optional<std::string> {
        CHECK(form.value(0) == "light");
        owner.reset(); // Successful save removes the last modal owner.
        REQUIRE_FALSE(weak.expired());
        return {};
      }, [] {});
  weak = owner;
  owner->set_value(0, "light");
  CHECK(owner->submit());
  CHECK(weak.expired()); // The submission guard does not retain a closed form.
}

TEST_CASE("select keeps displayed option equal to form draft immediately", "[tui][components]") {
  std::string value = "future";
  auto select = Select(&value, {{"a", "Option A"}, {"b", "Option B"}});
  CHECK(frame(select).find("Unsupported existing value: future") != std::string::npos);
  CHECK(value == "future");
  select->OnEvent(Event::ArrowDown);
  CHECK(value == "a");
  CHECK(frame(select).find("Option A") != std::string::npos);
  select->OnEvent(Event::ArrowRight);
  CHECK(value == "b");
  select->OnEvent(Event::Return);
  CHECK(value == "b");
  select->OnEvent(Event::ArrowLeft);
  CHECK(value == "a");
  auto readonly = Select(&value, {{"a", "Option A"}, {"b", "Option B"}}, true);
  readonly->OnEvent(Event::ArrowDown);
  CHECK(value == "a");
}
TEST_CASE("optional input visibly omits absent value and accepts the next normal Tab", "[tui][components]") {
  Field optional{"Optional address", "", FieldKind::Text};
  optional.optional = true;
  optional.present = false;
  auto form = std::make_shared<Form>("Optional address", std::vector<Field>{optional},
      [](Form &) -> std::optional<std::string> { return {}; }, [] {});
  form->focus_field(0);
  CHECK(frame(form).find("Not set · field omitted") != std::string::npos);
  form->OnEvent(Event::Character("x"));
  CHECK_FALSE(form->present(0));
  CHECK(form->value(0).empty());
  form->OnEvent(Event::Character(' '));
  CHECK(form->present(0));
  form->OnEvent(Event::Tab);
  form->OnEvent(Event::Character("abc"));
  CHECK(form->value(0) == "abc");
  form->OnEvent(Event::Home);
  form->OnEvent(Event::Character("!"));
  form->OnEvent(Event::End);
  form->OnEvent(Event::Character("?"));
  CHECK(form->value(0) == "!abc?");
  form->OnEvent(Event::TabReverse);
  form->OnEvent(Event::Character(' '));
  CHECK_FALSE(form->present(0));
  CHECK(form->value(0) == "!abc?");
  form->OnEvent(Event::Character(' '));
  CHECK(form->present(0));
  CHECK(form->value(0) == "!abc?");
}
TEST_CASE("checkbox Enter is inert and Space changes only its draft value", "[tui][components]") {
  std::string value = "false";
  auto toggle = Toggle(&value);
  CHECK(toggle->OnEvent(Event::Return));
  CHECK(value == "false");
  CHECK(toggle->OnEvent(Event::Character(' ')));
  CHECK(value == "true");
}
TEST_CASE("multi select uses stable identities and keeps unknown members", "[tui][components]") {
  std::string value = "old\n1";
  auto multi = MultiSelect(&value, {{"1", "Same name"}, {"2", "Same name"}});
  multi->OnEvent(Event::ArrowDown);
  multi->OnEvent(Event::Character(' '));
  CHECK(SplitLines(value) == std::vector<std::string>{"old", "1", "2"});
}
TEST_CASE("table retains selection by identity across reordering", "[tui][components]") {
  auto table = std::make_shared<Table>(std::vector<Column>{{"Name", 20}, {"State", 12}});
  table->set_rows({{"a", {"Alpha", "ready"}}, {"b", {"Beta", "ready"}}});
  table->OnEvent(Event::ArrowDown);
  CHECK(table->selected_id() == "b");
  table->set_rows({{"b", {"Renamed Beta", "ready"}}, {"a", {"Alpha", "ready"}}});
  CHECK(table->selected_id() == "b");
  CHECK(table->selected_index() == 0);
  CHECK(frame(table).find("Name") != std::string::npos);
  table->set_rows({}, "Offline; retry to load");
  CHECK(table->selected_id().empty());
  CHECK(frame(table).find("Offline; retry to load") != std::string::npos);
}
TEST_CASE("collection keeps second object when Tab moves to its actions", "[tui][components]") {
  std::string edited;
  int added = 0, viewed = 0;
  CollectionActions actions;
  actions.edit = [&](const std::string& id) { edited = id; };
  actions.add = [&] { ++added; };
  actions.view = [&](const std::string& id) { if (id == "second") ++viewed; };
  auto collection = std::make_shared<CollectionEditor>(
      "Objects", std::vector<Column>{{"Name", 30}}, actions);
  collection->set_rows({{"first", {"First"}}, {"second", {"Second"}}});
  collection->table()->TakeFocus();
  collection->OnEvent(Event::ArrowDown);
  REQUIRE(collection->table()->selected_id() == "second");
  collection->OnEvent(Event::Tab);
  CHECK(collection->table()->selected_id() == "second");
  CHECK_FALSE(collection->table()->Focused());
  collection->OnEvent(Event::Return);
  CHECK(viewed == 1);
  collection->OnEvent(Event::Tab);
  collection->OnEvent(Event::Return);
  CHECK(added == 1);
  collection->OnEvent(Event::Tab);
  collection->OnEvent(Event::Return);
  CHECK(edited == "second");
  collection->OnEvent(Event::TabReverse);
  CHECK(collection->table()->Focused());
  CHECK(collection->table()->selected_id() == "second");
}
TEST_CASE("collection readonly prevents all mutation shortcuts", "[tui][components]") {
  int writes = 0, views = 0;
  CollectionActions actions;
  actions.add = [&] { ++writes; };
  actions.edit = actions.copy = actions.remove = [&](const std::string&) { ++writes; };
  actions.move = [&](const std::string&, int) { ++writes; };
  actions.view = [&](const std::string&) { ++views; };
  auto collection = std::make_shared<CollectionEditor>("Objects", std::vector<Column>{{"Name", 30}}, actions, true);
  collection->set_rows({{"one", {"First"}}});
  for (const auto c : std::string("aecD+-")) collection->OnEvent(Event::Character(c));
  collection->OnEvent(Event::Return);
  CHECK(writes == 0);
  CHECK(views == 1);
}
TEST_CASE("string list supports edits and ordering in a single form", "[tui][components]") {
  std::string value = "one\ntwo";
  auto list = StringList(&value);
  list->OnEvent(Event::Return); // Load first into Value input.
  list->OnEvent(Event::End);
  list->OnEvent(Event::Character("!"));
  list->OnEvent(Event::Tab); // Add
  list->OnEvent(Event::Return);
  CHECK(value == "one\ntwo\none!");
  CHECK(frame(list).find("New Entries:") != std::string::npos);
  list->OnEvent(Event::TabReverse); // Row
  list->OnEvent(Event::Return); // Load first and focus its Value input.
  list->OnEvent(Event::End);
  list->OnEvent(Event::Character("!"));
  list->OnEvent(Event::Tab); // Add
  list->OnEvent(Event::Tab); // Update
  list->OnEvent(Event::Return);
  CHECK(value == "one!\ntwo\none!");
  list->OnEvent(Event::Tab); // Delete
  list->OnEvent(Event::Return);
  CHECK(value == "two\none!");
  list->OnEvent(Event::Tab); // Up
  list->OnEvent(Event::Tab); // Down
  list->OnEvent(Event::Return);
  CHECK(value == "one!\ntwo");
}
TEST_CASE("destructive confirmation defaults to Cancel", "[tui][components]") {
  int accepted = 0, cancelled = 0;
  auto dialog = Confirm("Remove", "Remove selected object", [&] { ++accepted; }, [&] { ++cancelled; });
  dialog->OnEvent(Event::Return);
  CHECK(accepted == 0);
  CHECK(cancelled == 1);
  dialog->OnEvent(Event::TabReverse);
  dialog->OnEvent(Event::Return);
  CHECK(accepted == 1);
}
TEST_CASE("dialog consumes Escape once and preserves underlying input", "[tui][components]") {
  std::string value = "draft";
  auto input = TextInput(&value);
  input->OnEvent(Event::End);
  int closed = 0;
  auto help = HelpOverlay({{"F1", "Help", "Input", ""}}, [&] { ++closed; });
  help->OnEvent(Event::Character("q"));
  help->OnEvent(Event::Escape);
  CHECK(closed == 1);
  input->OnEvent(Event::Character("!"));
  CHECK(value == "draft!");
}
TEST_CASE("toast expiration deduplication and queue are bounded", "[tui][components]") {
  ToastHost host;
  const auto now = ToastHost::Clock::time_point{};
  host.push("op", "Subscription updated", Tone::Success, now);
  host.push("op", "Subscription updated", Tone::Success, now);
  CHECK(host.visible().size() == 1);
  CHECK(host.visible().front().repeats == 2);
  host.expire(now + std::chrono::seconds(3));
  CHECK(host.visible().empty());
  for (int i = 0; i < 40; ++i) host.push(std::to_string(i), "Failure", Tone::Error, now);
  CHECK(host.visible().size() == 3);
  CHECK(host.history().size() == 30);
  host.expire(now + std::chrono::seconds(7));
  CHECK(host.visible().size() == 3);
  host.expire(now + std::chrono::seconds(8));
  CHECK(host.visible().empty());
}
TEST_CASE("log pause retains history and strips terminal controls", "[tui][components]") {
  auto logs = std::make_shared<LogViewer>();
  const auto plain_log_frame = [&] {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(24));
    ftxui::Render(screen, logs->Render());
    std::string cells;
    for (int y = 0; y < 24; ++y) {
      for (int x = 0; x < 80; ++x)
        cells += screen.at(x, y).empty() ? " " : screen.at(x, y);
      cells += '\n';
    }
    return cells;
  };
  logs->set_lines({"first", "second", "third"});
  logs->OnEvent(Event::ArrowUp);
  CHECK_FALSE(logs->following());
  logs->set_lines({"first", "second", "third", "new\ncontinued"});
  CHECK(logs->unseen() == 1);
  CHECK(frame(logs).find("continued") == std::string::npos);
  logs->OnEvent(Event::End);
  // The gutter and message have different styles; assert adjacent screen
  // cells, rather than requiring their ANSI serialization to be contiguous.
  CHECK(plain_log_frame().find("| continued") != std::string::npos);
  logs->OnEvent(Event::ArrowUp);
  logs->set_lines({"\x1b]52;malicious clipboard\a"});
  CHECK(frame(logs).find('\a') == std::string::npos);
  logs->OnEvent(Event::End);
  CHECK(logs->following());
  CHECK(logs->unseen() == 0);
}
TEST_CASE("component renders mixed Unicode and survives constrained widths", "[tui][components]") {
  auto form = std::make_shared<Form>("Unicode", std::vector<Field>{{"名称", "中文 é العربية 👩‍💻"}, {"Disabled", "value", FieldKind::Text, {}, "", {}, false, true}}, Form::Submit{}, [] {});
  for (int width : {40, 80, 120, 180}) {
    const auto output = frame(form, width);
    INFO("width=" << width << "\n" << output);
    CHECK(output.find("名称") != std::string::npos);
    // At 40 columns the focused input scrolls horizontally to its end;
    // wider terminals should expose the unchanged beginning as well.
    if (width >= 80) CHECK(output.find("中文") != std::string::npos);
    else CHECK(output.find("👩‍💻") != std::string::npos);
    CHECK(output.find("[disabled]") != std::string::npos);
  }
}
TEST_CASE("optional checkbox distinguishes unset false and true", "[tui][components]") {
  auto form = std::make_shared<Form>("Native Boolean", std::vector<Field>{{"Flag", "", FieldKind::OptionalCheckbox}}, Form::Submit{}, [] {});
  CHECK(form->value(0).empty());
  form->OnEvent(Event::ArrowDown);
  form->OnEvent(Event::Return);
  CHECK(form->value(0) == "true");
  form->OnEvent(Event::ArrowDown);
  form->OnEvent(Event::Return);
  CHECK(form->value(0) == "false");
  form->OnEvent(Event::Home);
  form->OnEvent(Event::Return);
  CHECK(form->value(0).empty());
  auto readonly = std::make_shared<Form>("Native Boolean", std::vector<Field>{{"Flag", "false", FieldKind::OptionalCheckbox}}, Form::Submit{}, [] {}, true);
  readonly->OnEvent(Event::Character(' '));
  CHECK(readonly->value(0) == "false");
  CHECK_FALSE(readonly->submit());
}
TEST_CASE("tree collapse is separate from activation", "[tui][components]") {
  int activations = 0;
  auto tree = std::make_shared<SectionTree>(std::vector<TreeNode>{{"parent", "Parent", {{"child", "Child", {}}}}}, [&](const auto&) { ++activations; });
  CHECK(frame(tree).find("Child") != std::string::npos);
  tree->OnEvent(Event::ArrowLeft);
  CHECK(frame(tree).find("Child") == std::string::npos);
  CHECK(activations == 0);
  CHECK(tree->selected_id() == "parent");
  tree->OnEvent(Event::ArrowRight);
  tree->OnEvent(Event::ArrowDown);
  tree->OnEvent(Event::Return);
  CHECK(tree->selected_id() == "child");
  CHECK(activations == 1);
}
TEST_CASE("line values retain empty entries where representation is unambiguous", "[tui][components]") {
  CHECK(JoinLines({"", "foo", ""}) == "\nfoo\n");
  CHECK(SplitLines("\nfoo\n") == std::vector<std::string>{"", "foo", ""});
  CHECK(SplitLines("").empty());
}
TEST_CASE("collection Help derives available actions through overlay wrappers", "[tui][components]") {
  CollectionActions actions;
  actions.add = [] {};
  actions.edit = [](const std::string&) {};
  actions.move = [](const std::string&, int) {};
  auto collection = std::make_shared<CollectionEditor>("DNS servers", std::vector<Column>{{"Address", 30}}, actions, true);
  collection->set_rows({{"one", {"1.1.1.1"}}});
  auto overlay = ftxui::Renderer(collection, [collection] { return collection->Render(); });
  const auto help = ContextHelp(overlay);
  const auto edit = std::find_if(help.begin(), help.end(), [](const HelpEntry& entry) { return entry.key == "e"; });
  CHECK(edit == help.end()); // Read-only actions are not executable Help entries.
  CHECK(std::none_of(help.begin(), help.end(), [](const HelpEntry& entry) { return entry.key == "c"; }));
}

TEST_CASE("select starts keyboard focus on the saved choice", "[tui][components]") {
  std::string value = "b";
  auto select = Select(&value, {{"a", "First"}, {"b", "Saved"}, {"c", "Next"}});
  frame(select);
  select->OnEvent(Event::ArrowDown);
  select->OnEvent(Event::Character(' '));
  CHECK(value == "c");
}

TEST_CASE("review Help owns labels and executes exactly the selected action", "[tui][review]") {
  int first = 0, second = 0, closed = 0;
  auto help = HelpOverlay({{"a", "First", "", "", [&]{ ++first; }},
                           {"b", "Second", "", "", [&]{ ++second; }},
                           {"x", "Unavailable", "", "Requires Edit", [&]{ FAIL("disabled action"); }}},
                          [&]{ ++closed; }, "Current object");
  for (int i = 0; i < 3; ++i) CHECK(frame(help).find("First") != std::string::npos);
  CHECK(first == 0);
  CHECK(second == 0);
  help->OnEvent(Event::ArrowDown);
  help->OnEvent(Event::Return);
  CHECK(first == 0);
  CHECK(second == 1);
  help->OnEvent(Event::Escape);
  CHECK(closed == 1);
}
TEST_CASE("optional scalar presence is independent of empty and zero", "[tui][review]") {
  Field field{"Port", "0", FieldKind::Number};
  field.optional = true; field.present = false;
  field.validate = Integer(0,65535,false);
  int writes = 0;
  auto form = std::make_shared<Form>("Native port", std::vector<Field>{field},
      [&](Form&) -> std::optional<std::string> { ++writes; return {}; }, []{});
  CHECK_FALSE(form->present(0));
  CHECK(form->value(0) == "0");
  CHECK_FALSE(form->dirty());
  form->OnEvent(Event::Character(' '));
  CHECK(form->present(0));
  CHECK(form->dirty());
  CHECK(form->submit());
  CHECK(writes == 1);
  form->set_value(0, "");
  CHECK(form->present(0));
  CHECK_FALSE(form->submit());
  CHECK(writes == 1);
  form->set_present(0,false);
  CHECK(form->submit());
  CHECK(writes == 2);
}
TEST_CASE("toast modal suspension preserves lifetime and repeat history", "[tui][review]") {
  ToastHost host;
  const auto now = ToastHost::Clock::time_point{};
  host.push("one", "Saved", Tone::Success, now);
  host.push("one", "Saved", Tone::Success, now);
  CHECK(host.history().back().repeats == 2);
  host.suspended(true,now + std::chrono::seconds(1));
  host.push("two", "Warning", Tone::Warning, now + std::chrono::seconds(10));
  host.expire(now + std::chrono::seconds(20));
  CHECK(host.visible().size() == 2);
  host.suspended(false,now + std::chrono::seconds(20));
  host.expire(now + std::chrono::seconds(21));
  CHECK(host.visible().size() == 2);
  host.expire(now + std::chrono::seconds(30));
  CHECK(host.visible().empty());
}
TEST_CASE("theme and overlay clear all background cells", "[tui][review]") {
  using namespace ftxui;
  const auto old_theme = style::theme;
  const auto old_color = Terminal::ColorSupport();
  const char* no_color = std::getenv("NO_COLOR");
  const std::optional<std::string> old_no_color = no_color ? std::optional<std::string>(no_color) : std::nullopt;
  unsetenv("NO_COLOR");
  Terminal::SetColorSupport(Terminal::TrueColor);
  for (const auto& theme : {"dark", "light"}) {
    style::theme = theme;
    auto screen = Screen::Create(Dimension::Fixed(80),Dimension::Fixed(24));
    auto base = paragraph(std::string(1800,'X')) | flex;
    auto modal = text("Dialog") | size(WIDTH,EQUAL,30) | size(HEIGHT,EQUAL,8) | border | style::panel();
    Render(screen, dbox({base,modal | center}) | style::surface());
    CHECK(screen.PixelAt(79,23).background_color == (style::theme == "light" ? Color::RGB(243,246,249) : Color::RGB(19,26,35)));
    CHECK(screen.at(40,12) == " ");
    CHECK(screen.PixelAt(40,12).background_color == (style::theme == "light" ? Color::RGB(229,235,242) : Color::RGB(27,38,51)));
  }
  style::theme = old_theme;
  Terminal::SetColorSupport(old_color);
  if (old_no_color) setenv("NO_COLOR",old_no_color->c_str(),1); else unsetenv("NO_COLOR");
}
TEST_CASE("navigation keeps selected identity and explicit overflow", "[tui][review]") {
  using namespace ftxui;
  std::vector<std::string> labels{"log","dns","inbounds","outbounds","routing","observatory","api","metrics","stats","policy","fakedns","geodata","env","version","burstObservatory"};
  for (int width : {78,118,178}) for (int selected = 0; selected < int(labels.size()); ++selected) {
    auto screen = Screen::Create(Dimension::Fixed(width),Dimension::Fixed(1));
    Render(screen, Navigation(labels,selected,width,true));
    std::string plain;
    for (int x=0;x<width;++x) plain += screen.at(x,0);
    CHECK(plain.find(labels[selected]) != std::string::npos);
    CHECK(plain.find(std::to_string(selected+1)+"/15") != std::string::npos);
  }
}

TEST_CASE("wide tables use available cells for long object identity", "[tui][review]") {
  auto table = std::make_shared<Table>(std::vector<Column>{{"Name",20},{"State",10}});
  table->set_rows({{"id",{"Long meaningful object name with complete identity","ready"}}});
  frame(table,120); // The first layout establishes the actual parent allocation.
  CHECK(frame(table,120).find("Long meaningful object name with complete identity") != std::string::npos);
}

/// @file
/// @brief Checks log parsing, stream boundaries, wrapped records, and viewer
/// chrome.

#include "tui/components/log_viewer.hpp"
#include "tui/components/widgets.hpp"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

using namespace xrm::tui::ui;
using ftxui::Event;

namespace {
std::string log_screen(const std::shared_ptr<LogViewer> &logs, int width,
                       int height) {
  ftxui::Terminal::SetFallbackSize({width, height});
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, logs->Render());
  return screen.ToString();
}
} // namespace

TEST_CASE("log chunks preserve stream boundaries and native facts",
          "[tui][logs]") {
  const auto records = ParseLogChunks({
      {10, "stdout", "2026/09/26 16:02:03.123 [Warn"},
      {11, "stderr", "unstructured warning word\n"},
      {12, "stdout",
       "ing] [1234] app/dns: failed to lookup example.test\nnext"},
      {13, "stdout", " line\n"},
  });
  REQUIRE(records.size() == 3);
  CHECK(records[0].sequence == 10);
  CHECK(records[0].timestamp == "2026/09/26 16:02:03.123");
  CHECK(records[0].level == "Warning");
  CHECK(records[0].subsystem == "app/dns");
  CHECK(records[0].message == "[1234] failed to lookup example.test");
  CHECK(records[0].raw == "2026/09/26 16:02:03.123 [Warning] [1234] app/dns: "
                          "failed to lookup example.test");
  CHECK(records[1].source == "stderr");
  CHECK(records[1].timestamp.empty());
  CHECK(records[1].level.empty());
  CHECK(records[1].message == "unstructured warning word");
  CHECK(records[2].message == "next line");
  CHECK_FALSE(records[2].partial);
}

TEST_CASE("logs keep partial lines and unknown prefixes literal",
          "[tui][logs]") {
  const auto records = ParseLogChunks({
      {1, "stdout", "[something] api/server: retained\n[Error] incomplete"},
      {2, "stderr", "plain text"},
  });
  REQUIRE(records.size() == 3);
  CHECK(records[0].level.empty());
  CHECK(records[0].subsystem.empty());
  CHECK(records[0].message == "[something] api/server: retained");
  CHECK(records[1].level == "Error");
  CHECK(records[1].partial);
  CHECK(records[2].partial);
}

TEST_CASE(
    "paused log records remain stable across completion and ring rotation",
    "[tui][logs]") {
  auto logs = std::make_shared<LogViewer>();
  logs->set_records(ParseLogChunks({{1, "stdout", "first\npartial"}}));
  logs->set_follow(false);
  logs->set_records(ParseLogChunks(
      {{1, "stdout", "first\npartial"}, {2, "stdout", " complete\nnew\n"}}));
  REQUIRE(logs->selected_record());
  CHECK(logs->selected_record()->message == "partial");
  CHECK(logs->unseen() == 1);
  logs->set_records(ParseLogChunks({{3, "stdout", "rotated\n"}}));
  CHECK(logs->selected_record()->message == "partial");
  CHECK(logs->unseen() == 2);
  logs->OnEvent(Event::End);
  CHECK(logs->following());
  CHECK(logs->unseen() == 0);
  CHECK(logs->selected_record()->message == "rotated");
}

TEST_CASE("logs align wrapped messages at compact and wide sizes",
          "[tui][logs]") {
  auto logs = std::make_shared<LogViewer>();
  logs->set_records(ParseLogChunks(
      {{7, "stderr",
        "2026/09/26 16:02:03 [Error] app/dns: " + std::string(160, 'x') +
            " END-OF-MESSAGE\n"}}));
  for (auto [width, height] : {std::pair{80, 24}, {120, 40}, {180, 45}}) {
    const auto rendered = log_screen(logs, width, height);
    CHECK(rendered.find("TIME") != std::string::npos);
    CHECK(rendered.find("CURSOR") == std::string::npos);
    CHECK(rendered.find("#7") == std::string::npos);
    CHECK(rendered.find("16:02:03") != std::string::npos);
    CHECK(rendered.find("Error") != std::string::npos);
    CHECK(rendered.find("END-OF-MESSAGE") != std::string::npos);
    CHECK(rendered.find("FOLLOW") == std::string::npos);
    CHECK(rendered.find("PAUSED") == std::string::npos);
    CHECK(rendered.find("F1") == std::string::npos);
  }
  ftxui::Terminal::SetFallbackSize({80, 24});
}

TEST_CASE("logs display controls without executing terminal sequences",
          "[tui][logs]") {
  auto logs = std::make_shared<LogViewer>();
  logs->set_lines({"\x1b]52;clipboard\a\t中文 é"});
  const auto rendered = log_screen(logs, 80, 24);
  CHECK(rendered.find("^[]52;clipboard^G") != std::string::npos);
  CHECK(rendered.find('\a') == std::string::npos);
  CHECK(rendered.find("中文 é") != std::string::npos);
}

TEST_CASE("log metadata has gutters and compact wrapped rows align",
          "[tui][logs]") {
  auto logs = std::make_shared<LogViewer>();
  logs->set_records(ParseLogChunks(
      {{123456789, "core_stderr",
        "2026/09/26 16:02:03 [Error] proxy/vless: START continuation "
        "continuation continuation continuation continuation continuation "
        "continuation continuation\n"}}));
  ftxui::Terminal::SetFallbackSize({80, 24});
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                      ftxui::Dimension::Fixed(24));
  ftxui::Render(screen, logs->Render());
  std::vector<std::string> rows;
  for (int y = 0; y < 24; ++y) {
    std::string row;
    for (int x = 0; x < 80; ++x)
      row += screen.at(x, y).empty() ? " " : screen.at(x, y);
    rows.push_back(std::move(row));
  }
  CHECK(rows[2].substr(9, 9) == "stderr   ");
  CHECK(rows[2][8] == ' ');
  CHECK(rows[2][17] == ' ');
  CHECK(rows[2][25] == ' ');
  CHECK(rows[2].find("proxy/vless") == 28);
  CHECK(rows[3].substr(26, 2) == "| ");
  CHECK(rows[3].find("continuation") == 28);
  CHECK(rows[4].substr(26, 2) == "| ");
  CHECK(rows[4].find("continuation") == 28);
  REQUIRE(logs->selected_record());
  CHECK(logs->selected_record()->source == "core_stderr");
  CHECK(logs->selected_record()->sequence == 123456789);
}

TEST_CASE("log UTF8 and CRLF survive chunk boundaries with exact raw text",
          "[tui][logs]") {
  const std::string chinese = "中文";
  const auto records = ParseLogChunks({{1, "stdout", chinese.substr(0, 2)},
                                       {2, "stderr", "other\n"},
                                       {3, "stdout", chinese.substr(2) + "\r"},
                                       {4, "stdout", "\nnext\n"}});
  REQUIRE(records.size() == 3);
  CHECK(records[0].message == chinese);
  CHECK(records[0].raw == chinese + "\r");
  CHECK(records[0].timestamp.empty());
  CHECK(records[0].level.empty());
  CHECK_FALSE(records[0].partial);
}

TEST_CASE("lost log transport spans do not invent joined records",
          "[tui][logs]") {
  const auto records = ParseLogChunks(
      {{1, "stdout", "before-gap"}, {4, "stdout", "after-gap\n"}});
  REQUIRE(records.size() == 2);
  CHECK(records[0].message == "before-gap");
  CHECK(records[0].partial);
  CHECK(records[1].message == "after-gap");
}

TEST_CASE("log display escapes ANSI OSC C1 and malformed byte tails",
          "[tui][logs]") {
  const std::string raw = "\x1b[31mred\x1b[0m\x1b]52;c;DATA\a\xc2\x9b";
  const auto visible = LogTextForDisplay(raw);
  CHECK(visible == "^[[31mred^[[0m^[]52;c;DATA^G\\u009B");
  CHECK(visible.find('\x1b') == std::string::npos);
  CHECK(LogTextForDisplay("\xe4\xb8") == "\\xE4\\xB8");
  CHECK(LogTextForDisplay("first\nsecond", true) == "first\nsecond");
  CHECK(LogTextForDisplay("中文 é") == "中文 é");
}

TEST_CASE(
    "wide log subsystem truncation is explicit and raw detail remains complete",
    "[tui][logs]") {
  auto logs = std::make_shared<LogViewer>();
  const std::string subsystem = "transport/internet/tcp/long-subsystem-name";
  logs->set_records(ParseLogChunks(
      {{1, "core_stdout", "[Warning] " + subsystem + ": message\n"}}));
  const auto rendered = log_screen(logs, 120, 40);
  CHECK(rendered.find("…") != std::string::npos);
  CHECK(rendered.find("Warning") != std::string::npos);
  CHECK(rendered.find("CURSOR") == std::string::npos);
  REQUIRE(logs->selected_record());
  CHECK(logs->selected_record()->subsystem == subsystem);
  ftxui::Terminal::SetFallbackSize({80, 24});
}

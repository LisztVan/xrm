// XRM: Defines shared TUI color, emphasis, and layout tokens.

#pragma once

#include <cstdlib>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <string>

// Presentation only. Theme follows the existing XRM appearance setting; no
// business state or terminal font assumptions belong in these tokens.
namespace xrm::tui::ui::style {
enum class Role {
  Text,
  Muted,
  Subtle,
  Accent,
  Focus,
  Success,
  Warning,
  Error,
  Running,
  Disabled
};
inline std::string theme = "dark";
inline bool mono() {
  const auto *term = std::getenv("TERM");
  return std::getenv("NO_COLOR") || (term && std::string(term) == "dumb") ||
         ftxui::Terminal::ColorSupport() == ftxui::Terminal::Palette1;
}
inline ftxui::Color shade(Role role) {
  using ftxui::Color;
  const bool light = theme == "light";
  switch (role) {
  case Role::Text:
    return light ? Color::RGB(32, 43, 56) : Color::RGB(220, 227, 235);
  case Role::Muted:
  case Role::Disabled:
    return light ? Color::RGB(82, 98, 117) : Color::RGB(146, 163, 183);
  case Role::Subtle:
    return light ? Color::RGB(132, 145, 160) : Color::RGB(66, 83, 104);
  case Role::Accent:
  case Role::Focus:
    return light ? Color::RGB(0, 93, 143) : Color::RGB(115, 194, 230);
  case Role::Success:
  case Role::Running:
    return light ? Color::RGB(23, 112, 80) : Color::RGB(130, 201, 164);
  case Role::Warning:
    return light ? Color::RGB(142, 88, 16) : Color::RGB(228, 187, 111);
  case Role::Error:
    return light ? Color::RGB(170, 49, 61) : Color::RGB(242, 140, 145);
  }
  return Color::Default;
}
inline ftxui::Decorator ink(Role role) {
  return [role](ftxui::Element e) {
    if (mono())
      return role == Role::Muted || role == Role::Disabled ||
                     role == Role::Subtle
                 ? e | ftxui::dim
                 : e;
    if (theme == "default" && role == Role::Text)
      return e;
    return e | ftxui::color(shade(role));
  };
}
// Clear first, then apply the theme, then render children. This writes blank
// cells too and prevents terminal background erasure or lower layers leaking.
inline ftxui::Decorator paint(bool raised) {
  return [raised](ftxui::Element e) {
    if (mono() || theme == "default")
      return e | ftxui::clear_under;
    const auto background = theme == "light"
                                ? (raised ? ftxui::Color::RGB(229, 235, 242)
                                          : ftxui::Color::RGB(243, 246, 249))
                                : (raised ? ftxui::Color::RGB(27, 38, 51)
                                          : ftxui::Color::RGB(19, 26, 35));
    return e | ftxui::bgcolor(background) | ink(Role::Text) |
           ftxui::clear_under;
  };
}
inline ftxui::Decorator surface() { return paint(false); }
inline ftxui::Decorator panel() { return paint(true); }
inline ftxui::Decorator selected(bool focused) {
  return [focused](ftxui::Element e) {
    if (!focused)
      return e | ftxui::bold | ink(Role::Accent);
    if (mono() || theme == "default")
      return e | ftxui::inverted | ftxui::bold;
    return e | ftxui::bold | ink(Role::Focus) |
           ftxui::bgcolor(theme == "light" ? ftxui::Color::RGB(211, 229, 240)
                                           : ftxui::Color::RGB(35, 55, 73));
  };
}
inline ftxui::Decorator rule() { return ink(Role::Subtle); }
inline ftxui::Element heading(std::string title, std::string meta = "") {
  using namespace ftxui;
  return hbox({text(std::move(title)) | bold,
               text("  " + meta) | ink(Role::Muted) | xflex});
}
inline ftxui::Element key_value(std::string label, std::string value,
                                int width = 18) {
  using namespace ftxui;
  return hbox(
      {text(std::move(label)) | ink(Role::Muted) | size(WIDTH, EQUAL, width),
       paragraph(std::move(value)) | xflex});
}
inline ftxui::Component button(std::string label,
                               std::function<void()> action) {
  auto option = ftxui::ButtonOption::Simple();
  option.transform = [](const ftxui::EntryState &s) {
    auto e = ftxui::text(" [ " + s.label + " ] ");
    return s.focused ? e | selected(true) : e | ink(Role::Accent);
  };
  return ftxui::Button(std::move(label), std::move(action), option);
}
inline ftxui::CheckboxOption checkbox() {
  auto option = ftxui::CheckboxOption::Simple();
  option.transform = [](const ftxui::EntryState &s) {
    auto e = ftxui::text(std::string(s.state ? "[x] " : "[ ] ") + s.label);
    return s.focused ? e | selected(true) : e;
  };
  return option;
}
} // namespace xrm::tui::ui::style

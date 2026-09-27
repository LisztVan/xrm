// XRM: Declares the log viewer model and rendering helpers.

#pragma once

#include <string>
#include <string_view>

namespace xrm::tui::ui {
// Display only: raw LogRecord data remains unchanged. Invalid UTF-8 and C1
// controls are escaped; C0/DEL use caret notation. No terminal sequences run.
std::string LogTextForDisplay(std::string_view text, bool multiline = false);
} // namespace xrm::tui::ui

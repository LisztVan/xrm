// XRM: Creates the seven-page terminal application and its event loop.

#include "cli/commands.hpp"
#include "tui/shell/workspace.hpp"
#include <termios.h>
#include <unistd.h>
namespace xrm::cli {
int run_tui(const std::string &socket, const std::string &profile_id) {
  // Ctrl-S belongs to the form while the TUI is active. Restore the caller's
  // flow-control settings on every normal/exception exit.
  struct TerminalMode {
    termios original{};
    bool valid = false;
    TerminalMode() {
      valid = tcgetattr(STDIN_FILENO, &original) == 0;
      if (valid) {
        auto mode = original;
        mode.c_iflag &= ~(IXON | IXOFF);
        tcsetattr(STDIN_FILENO, TCSANOW, &mode);
      }
    }
    ~TerminalMode() {
      if (valid)
        tcsetattr(STDIN_FILENO, TCSANOW, &original);
    }
  } terminal;
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  auto workspace = ftxui::Make<xrm::tui::Workspace>(socket, profile_id, screen);
  screen.Loop(workspace);
  return 0;
}
} // namespace xrm::cli

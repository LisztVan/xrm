# I52 interactive terminal checklist

This is a manual record template, not evidence of completion. The automated PTY case is in `acceptance_tests.cpp`.

## Environment

- Terminal emulator and version: NOT RECORDED
- Size fixed at 80x24: NOT RUN
- Mouse disabled / keyboard-only: NOT RUN
- Locale (`locale`): NOT RECORDED
- Current core running before test: NOT RECORDED

## Required walkthrough

- [ ] Move among Overview, Nodes, Modules, Logs, Subscriptions, Groups, Stats, and Cores with Tab and Shift-Tab; `?` opens help and Escape closes it.
- [ ] Navigate with arrows and Enter; Escape returns from detail/editor/help without stopping the core; `q` exits the TUI and the same core PID remains running.
- [ ] Browse fixture nodes named ASCII, Chinese, emoji, and >80-column long name. Verify rendered glyphs are intact and selection remains on the same node ID after refresh.
- [ ] Start node search. Type `q`, `e`, and space; verify those characters enter the search input and do not quit/edit/enable/connect. Apply and cancel search.
- [ ] While a second client adds nodes and refreshes lists, verify current focus, selected ID, scroll position, and unsaved edit draft are not displaced.
- [ ] Logs: pause viewing, append enough new logs to move the tail, resume, and verify the prior viewport is not forced to bottom.
- [ ] Start a core on target A, save target B, make B fail `connect`, and verify Overview simultaneously shows saved=B and actual=A. If API override is available, verify it is shown separately without replacing either saved or actual state.
- [ ] If an external editor action is exposed: test normal exit, nonzero exit, cancellation, conflict, invalid JSON, re-edit after each failure, terminal restoration, and that save does not connect/restart.

## Results

Status: NOT RUN in the agent environment. Automated PTY execution was blocked before TUI startup by the sandbox's Unix-socket bind denial. The repository currently exposes an embedded TUI JSON editor; no external-editor launcher was found, so the final bullet is not applicable to a reachable product action at this baseline and remains NOT IMPLEMENTED/NOT RUN. Record tester/date/build before treating any checkbox as evidence.

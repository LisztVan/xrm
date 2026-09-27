# TUI event-buffer crash repair — 2026-09-27

The preceding [full build-reorganization run](../build-reorg/README.md) failed test #368 (`UX-07/09 module views use one full-width draft and F2 never saves`) when `xrm` received SIGSEGV. The PTY test had entered the native module JSON editor, sent a large Delete/input burst, and switched with F2. The [sanitized crash stack](../build-reorg/ctest-ux0709-crash-summary.txt) stops in FTXUI `Event` move construction during `MultiReceiverBuffer<Event>::Push`, called from `TerminalInputParser::Add` on the terminal thread.

`Workspace` also sent `Event::Custom` every 100 ms from its ticker thread with `screen_.PostEvent`. In the retained FTXUI 7.0.1 source, `PostEvent` writes directly to that `MultiReceiverBuffer`, which has no lock; terminal input pushes to the same buffer. This is a concrete concurrent mutation of one deque, consistent with the stack. The ticker now uses `screen_.Post(Event::Custom)`, whose task queue is mutex-protected and delivers the same event on the UI loop. The sole product change is [Workspace ticker](../../../src/tui/shell/workspace.cpp); it does not change refresh cadence, navigation, drafts, or core behavior.

Verification on the post-repair [source and binary identity](identity.txt):

- Offline `real-core` configure and full build passed: [configure](configure.log), [build](build.log).
- The exact failed CTest case passed **20 consecutive isolated runs**: [focused output](focused.log).
- An isolated real PTY Config workflow switched light, default, and dark themes and completed its form/read-only checks: [run output](config-pty.log), [screens and steps](config-pty/steps.json). This does not prove the older user-reported theme-switch SIGSEGV had the same trigger.
- `ctest --preset real-core -j4 --output-on-failure` passed **377/377, 0 failed**, with the pinned Xray 26.9.9 real-core cases executed rather than skipped: [complete output](ctest-full.log).

The exact `/tmp/xrm-user-db-before-g5-remediation.sqlite` backup and any sidecars were deleted at the user's request; the path is absent. All tests used their own temporary HOME/XDG, socket, database, ports and tmux server. No project-owned `xrm`, `xrmd` or Xray process remained after verification. The generated `out/real-core` tree was removed during the subsequent project cleanup; the recorded hashes identify the verified binaries, and `.deps/` remains available for an offline rebuild. This engineering result does not mark G5 PASS.

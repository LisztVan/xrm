# G5 RC unresolved watchlist

These four reports remain **unreproduced and open** at the RC baseline. They are neither fixed nor G5 PASS. Product code is frozen until a matching user G5 reproduction or another permitted exception supplies a concrete trigger.

| Item | Current evidence | Reproduction details to capture if seen again |
| --- | --- | --- |
| Rapid Esc reopen | Earlier report C described a form reopening after rapid Esc. The later isolated single-key replay did not reproduce it; that replay does not clear the original report. | Exact page, object, focused field/overlay, clean or dirty draft, terminal and key byte sequence with timing, screen before/after. Check actual form state rather than a stale cell fragment. |
| Dirty draft navigation lock | Earlier report C described navigation becoming locked with a draft. Later clean/dirty replay did not reproduce the same condition. | Page and component, draft contents/validation state, focused layer, Save/Discard/Continue action, exact keys and resulting screen/state. |
| Profile Enter unexpected page transition | Earlier report C described Enter on a Profile opening Modules unexpectedly. Current isolated navigation replay did not reproduce it. | Profile row, TopNav/SecondaryNav/Content focus, any overlay, exact Enter sequence and page/focus before/after. |
| Theme switch SIGSEGV | User observed `scripts/tui-lab.py --build build/component-rewrite` exit with SIGSEGV during theme switching. The Python `CalledProcessError` gives signal 11 but no native stack or exact switch path; later isolated dark/light/default attempts did not reproduce it. | Exact Config/Appearance or CLI path, starting and chosen theme, terminal/size, focus and keys, binary SHA, core dump or native backtrace, and whether the crash occurs before or after redraw. |

Source of prior disposition: [usability-repair-execution.md](usability-repair-execution.md). Do not close an item because a different environment or interaction sequence passes. Do not make speculative product changes under the RC freeze.

## Build reorganization verification observation

The 2026-09-27 clean `real-core` CTest run observed a separate TUI SIGSEGV in `UX-07/09 module views use one full-width draft and F2 never saves`, after switching a native module draft between JSON and Visual. It is **not** the theme-switch report above. The failing run and stack are in [build reorganization evidence](evidence/build-reorg/README.md). A subsequent investigation identified concurrent writes to FTXUI's unlocked event buffer from terminal input and Workspace's ticker; the ticker now uses FTXUI's locked task queue. The exact case passed 20 consecutive runs, and the post-repair full CTest passed 377/377; see the [repair evidence](evidence/tui-crash-fix/README.md). This observed race is repaired for the known path, while the older theme-switch report remains separately unreproduced and G5 remains unsigned.

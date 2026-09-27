# Native forms and terminal acceptance migration evidence

The old PTY cases exercised removed sequential Prompts and generic key/type/value JSON trees. They now drive the actual FTXUI forms, semantic module sections, native collections, Edit gate, and explicit dialog buttons. No business case was deleted or marked skip for the rewrite.

Preserved invariants: Profile/node/Group persistence, read-only Modules without Edit, source vs draft vs Runtime, unknown native fields and object types, disabled inbound preservation, order and selection, no implicit core start, running PID unchanged during browsing, source conflict rejection with retained fields, failed input remaining editable, source-specific subscription changes, offline cache recovery, Unicode/filter keyboard ownership, 80x24 layout and resize recovery.

Changed UI requirements: Edit toggles module editing; metadata uses m. Enter inspects and never starts a sequential prompt. Native collections own add/edit/copy/delete/reorder. Tab navigates labelled fields; selector arrow moves focus and Enter selects. Ctrl-S submits one form. Child native forms update a shared module draft, which needs separate module Ctrl-S to save. Dirty forms use actual Save/Discard/Continue buttons. Local SOCKS capability is via native inbounds Add and explicit Include; no legacy port10808 precheck is claimed. Root explicitly authorized this replacement path.

Harness corrections were evidence-driven: CapturedTui waits for startup layer before entering content; literal text sends wait for FTXUI event consumption; no backend assertion races an unsent mutation. Selected far-right tab ANSI lacks trailing space in tmux output (captured under far-tab/), so helper matches the label without assuming trailing whitespace. After an out-of-TUI core registration, refresh is explicit. Target save waits for its own notification rather than a previous Group saved notification. Toast checks respect visible paragraph wrapping. Match actual capitalized Required and [Overview] selected labels; startup single-Profile Use follows observed current session initialization.

Product findings retained by tests: invalid JSON error line/column was clipped behind a long parser prefix at 80 columns (root repaired concise error toast); Geodata update omitted core_id despite a bound real core (reported to root, pending recheck).

Runs before final rebuild:
- native forms: 10 cases, 226 assertions PASS. Real FTXUI events, draft values checked, not a PTY substitute.
- front initial migrated PTY batch: 17 cases, 13 pass, 4 fail from harness timing/stale-message/refresh assumptions above.
- tail initial migrated PTY batch: 9 cases, 4 pass, 5 fail; actual JSON location visibility plus harness startup/text matching above.
- extra PTY/RPC batch: 7 cases, 6 pass, 1 skipped because incorrect environment name supplied. Explicit rerun with XRM_TEST_XRAY fixed-v26.9.9 exposed real Geodata missing core_id; no core PASS claimed for the skipped run.

Final rechecks are recorded below. G5 remains user acceptance, not automatically PASS.

Recheck after harness fixes and concise JSON errors:
- Tail: 9 cases, 8 passed. The remaining Subscription conflict case exposed a real reusable Form defect: callback errors were appended inside the scrolling field area and invisible while focus remained on Name. The test still requires visible conflict feedback, retained dirty input, and unchanged backend settings.
- Front exposed a real reusable Table defect: at 80 columns, a long Core Version filled its entire cell and touched the State text (`...Pregistered`). The existing spacing assertion remains in place; this is not treated as acceptable truncation.

## Final recheck

Both shared-component defects were fixed in the product. The focused latest-binary run passed **3 cases / 141 assertions**: Core table column separation, the complete Profile/Subscription edit–external conflict–discard–auto-refresh–reference-aware removal chain, and Unicode/filter/help keyboard ownership.

Combined evidence covers **all 23 front cases and all 9 tail cases assigned to this migration**. This is coverage across the recorded initial full batches plus focused affected-case rechecks, not a claim that a single final 32-case invocation was run. The real Geodata case used `XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray`, validated an isolated loopback GeoIP asset, bound it to a Profile, and kept runtime stopped. It passed after `core_id` was supplied to the existing backend API.

Actual commands and local logs:

```sh
build/component-rewrite/tests/tui/forms/xrm_tui_forms_tests
# 10 cases, 226 assertions passed; build/ui-research/native-form-final.log

XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray \
  build/component-rewrite/tests/tui/acceptance/xrm_tui_acceptance_tests \
  '[UI-NAV],[P1-FOCUS],[P1-SIZE],[P1-TABLE],[P3-TEMPLATE],[P3-MODULE-TABS],[P3-MODULE-NAV],[P3-RUNTIME],[P3-STRUCT],[P5-PRESET-UI],[P5-GEODATA-UI],[P4-RPC],[P4-CONFIG-TABS],[UI-LOCAL],[UI-CORE],[UI-CONFIG],[UI-12],[HISTORY-TUI],[PROFILE-LOCAL-TUI],[UI-SCROLL],"80x24 TUI keyboard flow*"'
# 23 cases: 22 passed, one Table spacing failure subsequently fixed/rechecked.
# build/ui-research/front-acceptance-final.log

build/component-rewrite/tests/tui/acceptance/xrm_tui_acceptance_tests \
  '[UX-02],[UX-07],[UX-08],[UX-09],[UX-INT]'
# 9 cases: 8 passed, one Form conflict visibility failure subsequently fixed/rechecked.
# build/ui-research/tail-acceptance-final.log

build/component-rewrite/tests/tui/acceptance/xrm_tui_acceptance_tests \
  '[P1-TABLE],UX-INT Profile and Subscription editors save their own sources,80x24 TUI keyboard flow retains UTF-8 names and q is ordinary filter input'
# 3 cases, 141 assertions passed; build/ui-research/focused-acceptance-recheck.log
```

The success claims above concern actual PTY interaction plus source/runtime assertions. They do not approve G5 or replace the user's visual acceptance. The middle acceptance cases and the rewrite terminal screenshot/core-flow suite are recorded separately by the integration owner.

# G5 RC: E2E-07 root cause and Nodes Enter

This investigation used only E2E-07 for the original failure. The newly reported Nodes Enter behavior was reproduced separately in a disposable daemon/tmux fixture. No other interaction redesign was made, and this report does not mark G5 PASS.

## Locked starting point

HEAD was `561dede3c2e8e9e38a39d07a52076c9f08e3eeb0`. Before edits, `xrm` was `4e4fb1c5…`, `xrmd` `103af026…`, and `xrm_real_core_tests` `e81cc324…` (full hashes in [pre-hashes.txt](evidence/g5-rc/e2e07-investigation/pre-hashes.txt)). The product source manifest hash was `0fb1d79b…`. [Pre-investigation status](evidence/g5-rc/e2e07-investigation/pre-status.txt), [full tracked diff](evidence/g5-rc/e2e07-investigation/pre-diff.patch), and [test-driver diff](evidence/g5-rc/e2e07-investigation/test-driver-diff.patch) preserve this uncommitted baseline. `docs/g5-rc-baseline.md` had described the previous test-driver correction but lacked the corrected test binary hash; that metadata was added before investigation.

## E2E-07: what actually failed

**Classification of the failing assertion: TEST-DRIVER-BUG.** The test helper assumed `Modules` always entered its first secondary item and tabbed six positions to `api`. The current TUI correctly retains the last selected module, `stats` (9/15); six tabs from there wrapped back to `stats`. The [original diagnostic replay](evidence/g5-rc/e2e07-investigation/trace-repro.log) visibly shows `stats / SOURCE / EDIT / JSON` and `UNSAVED *` after the ASCII space, while the assertion read the unchanged **api** Source. The input chain was not silently dropping whitespace.

The test helper now reads the actual `N/15` secondary position, tabs to the target catalog index, and checks the resulting module title before editing. It still requires byte-level Source change, semantic equality, unchanged inclusion, and unchanged running PID/run ID/confdir. The final E2E fixture seeds a compact API Source containing an ordered services array and unknown `futureNative` object, then requires the saved Source to equal **exactly one leading ASCII space plus the original bytes**. It checks the unknown boolean/integer/array values and disk Source. This strengthens rather than relaxes the assertion.

The more detailed replay used an isolated Unix socket proxy to capture the actual requests. [Raw wire](evidence/g5-rc/e2e07-investigation/rpc-wire.log) and [decoded API save payloads](evidence/g5-rc/e2e07-investigation/api-save-payloads.json) show `module.save` with `module=api`, the original `expected_text`, and `text` beginning with a space. A second edit changed the native `api.tag` string from `api` to `api-g5`; the next request carried that value and retained the services array. The proxy and visible-edit diagnostic were removed from the final test driver.

| Layer | Direct evidence | Conclusion |
| --- | --- | --- |
| Keyboard → TextArea | [Stage screens](evidence/g5-rc/e2e07-investigation/editor-stages.json) show `api / JSON` then `UNSAVED *` after Space; the visible string edit also appears in the TextArea. | ASCII Space and ordinary string characters reached the editor. |
| Editor → module draft | The pre-save screen shows the changed characters. `save_module()` sends `draft_.text` directly as the RPC `text` field. | The exact in-memory buffer was not introspected; its saved bytes are observed at the next boundary. |
| Serialization → RPC | Decoded `module.save(api)` payload begins with `0x20`; the second payload contains `"tag": "api-g5"`. | No save-time JSON pretty-printer removed the edit. |
| Server → disk → reload | In [the stage trace](evidence/g5-rc/e2e07-investigation/editor-stages.json), `module.get`/CLI Source and the source file match after each save. `ProfileService::save_module()` passes the request text to `write_if_unchanged()`. | The persisted Source matched the request, and reload returned it. |
| Assertion | The old helper opened `stats` but compared `api`. With correct selection, the byte-difference assertion passed. | Wrong test context caused the original failure. |

The format-preservation rule is explicit: [requirements.md](requirements.md) says text editing preserves original text; [architecture.md](architecture.md) says advanced text read/save does not reorder a whole JSON file; [xrm-interaction-spec.md](xrm-interaction-spec.md) says one DraftSession retains original text/baseline while Visual preserves unknown fields, legal types, and array order. This is **Textual preservation**, not an unspecified formatting preference.

The trace also exposed a distinct, same-path **PRODUCT-BUG**: `load_module()` replaced valid Source text with `Json::dump(2)` before any user edit. The pre-fix stage trace shows an 88-byte Source becoming 107 bytes after adding one space because opening had already reformatted it. The only product change in this path removes that load-time formatting; invalid JSON still selects JSON view, and Visual changes may still format their target draft under the existing contract. The final E2E check proves a compact Source remains byte-identical apart from the user's space. The test case's original failure remains classified as TEST-DRIVER-BUG.

## User G5 report: Nodes Content Enter

**Classification: PRODUCT-BUG.** In the isolated [before screen](evidence/g5-rc/nodes-enter-before/before-enter-80x24.txt), Nodes / All / Content highlighted a real local node; the [after-Enter screen](evidence/g5-rc/nodes-enter-before/after-enter-80x24.txt) was unchanged. `view_detail()` assembled an internal string but did not display it. The narrow fix opens the existing read-only Details dialog for node rows, using the existing node context summary. [After-Enter](evidence/g5-rc/nodes-enter-after/after-enter-80x24.txt) shows source, protocol, enabled state, support, and ID; [after Esc](evidence/g5-rc/nodes-enter-after/after-esc-80x24.txt) returns to the same Content selection. [Fixture trace](evidence/g5-rc/nodes-enter-after/trace.json) confirms the runtime state did not change. Group Enter behavior was left untouched.

## Final gate and limits

The product fixes changed the RC source manifest, so [the RC baseline](g5-rc-baseline.md) now records new product and binary hashes. The focused E2E-07 and four related editor/module tests passed. The real Nodes Enter fixture passed at 80×24, and the two affected existing UI tests passed after their drivers closed the new detail dialog before later actions. The final full CTest command, with the fixed Xray 26.9.9, completed **375/375 PASS, 0 failed, 0 skipped**; [full log](evidence/g5-rc/ctest-full-final-recheck.log).

One preceding full run on the same product tree had an isolated RC-09 failure before Catch entered its test case: the fixed-core `version` preflight returned no version text in 0.01 seconds. RC-09 passed alone (62 assertions) and in the final full run. Concurrent preflights use the same `/tmp/command-0.out` capture filename, which may explain that intermittent result, but the exact process interleaving was not captured. [Failed run](evidence/g5-rc/ctest-full-final.log) remains available; no RC-09 product or test-infrastructure code was changed.

The diagnostic screens and `module.save` request prove the typed ASCII space reached the saved API Source, and a visible string-value edit also survived save and reload. The final E2E asserts exact Source bytes and parsed JSON, while existing shared-draft/F2 tests passed; this is evidence for the relevant JSON/Visual path rather than a claim that every editor key combination was exhaustively tested. The four older unreproduced watchlist items remain open. This investigation does not mark G5 PASS.

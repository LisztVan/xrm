# XRM Usability Test — Part A: Black-box human simulation

Environment: `scripts/tui-lab.py --build build/component-rewrite` in isolated tmux server (`-L xrm-lab`), real PTY, 120×40 default. Loopback subscription fixture, local xray 26.9.9 binary registered by hand. No source read during this phase.

## 1. Persona A journey (first-time user)

First launch (Overview):
- Saw "View Lab Home · Use Lab Home · Edit none · Run none" — four unexplained state words. Could not tell what distinguishes View/Use/Edit/Run. (HUMAN-01)
- "Saved settings apply only on Connect / Apply" was the single most useful sentence on screen.
- Right arrow in top nav: nothing. Tab: changed page. Inconsistent affordance. (HUMAN-02)
- Enter on the Profiles row: nothing visible (View was already that profile — zero feedback). (HUMAN-03)
- Guessed 'e'/'u' on Profiles — worked (Edit/Use toggles) but only by luck; toasts ("Module Edit enabled… runtime unchanged") were the first explanation of the model.
- Needed F1 to learn Tab/Shift-Tab = within level, Enter = descend, Esc = ascend. Core navigation model only documented in Help. (HUMAN-04)
- Module strip (log/dns/…) — could not discover how to switch modules without learning the level model. (HUMAN-05)
- Quit: 'q' from content = silent no-op; only works from TopNav. (HUMAN-06)

## 2. Persona B journey (Xray/Linux expert, new to XRM)

- DNS module: recognized native names (dns.servers path shown, "localhost", queryStrategy). Field hints ("IP, DNS URL, localhost or fakedns", "Default 53; URL port applies for DOH/DOQL", "Match: domains (any entry)") read naturally.
- Routing rule form: 18 fields, native names, "Match conditions -> Target" structure — the mental model of "building a rule" mostly held. BUT the select-commit trap (HUMAN-09) and checkbox-Enter trap (HUMAN-10) broke flow even for an expert.
- Puzzled by a "webhook (advanced JSON)" field inside a routing rule; then a raw nlohmann exception for it. (HUMAN-11)
- CLI: bare `xrm` help is excellent (grouped, examples, "Help is local", "Saving and selecting never connect"). Found `profile list` in 2 steps. Confused by UUID-first list output and node-as-UUID in `status`. (HUMAN-22)

## 3. Persona C journey (experienced XRM user)

- High-frequency flow (Tab page → Enter → letter actions → Ctrl-S/'s') is fast once the model is internalized. Toasts "X saved; runtime unchanged" give good closure.
- Costs that remain: Tab-counting to reach form buttons (13–15 Tabs in DNS/routing forms, count varies with sub-collection buttons — unreliable); every page switch from content requires 1–2 Esc + exact Tab count, overshoot common; module switching requires SecondaryNav focus.
- 'N' notification history is the recovery net for faded toasts — good.
- Keyboard-only reorder works (+/-) but invisible entry selection makes it risky. (HUMAN-15)

## 4. Task completion table

| Task | Done | Time-to-first-correct-action | Wrong actions | Help opens | Confidence (1-5) |
|---|---|---|---|---|---|
| 1 Profile CRUD+use | yes | fast ('n' guessed 2nd) | 'c'=copy not create; Ctrl-A≠select-all | 1 | 4 |
| 2 Read-only→Edit | yes | ~4 min | 'a' silent; form-level help has no path | 3 | 3 |
| 3 DNS visual | yes | slow | Tab skipped enabled input; keystrokes swallowed; stale validation | 2 | 3 |
| 4 Routing rule | yes (2nd attempt) | very slow | uncommitted select dropped; Enter toggled webhook; Enter on [Cancel] destroyed draft | 3 | 2 |
| 5 Subscription | yes | fast (F1 list clear) | invalid URL saved silently; URL masked in edit; no failure reason shown | 1 | 4 |
| 6 Group | yes | fast | none major | 1 | 4 |
| 7 Run (connect/status/stop) | yes | medium | 'c' on Profiles = copy (not connect) | 2 | 4 |
| 8 Logs | yes | fast | — | 1 | 4 |
| 9 Help per context | yes | — | truncated text; ConfirmConfirm bug | — | 3 |
| Input controls | yes | — | End key dead in sub-form input; swallowed typing | — | 3 |
| Chaos (daemon down/mid-save) | yes | — | — | — | 4 |
| Resize | yes | — | 80-col tree debris | — | 4 |
| CLI discovery | yes | fast | — | 0 | 4 |
| Wrong paths | n/a | — | silent no-ops everywhere | — | 2 |

## 5. All FRICTION / FAIL observations

ID: HUMAN-01 — Persona A — Overview/always
- Before: expected screen to explain the product. Actual: View/Use/Edit/Run state quartet unexplained on screen; model only documented inside F1 on Profiles ("Use selects the next connection Profile…").
- Recovery: opened Help on Profiles → explanation exists but buried. Result: FRICTION, High, State model.

ID: HUMAN-02 — A — TopNav
- Right/Left arrows do nothing in nav; only Tab/Shift-Tab. Expected arrows (standard). FRICTION, Medium, Navigation.

ID: HUMAN-03 — A — Profiles content
- Enter on already-viewed row = zero feedback. FRICTION, Low, Feedback.

ID: HUMAN-04 — A — Global
- Level model (TopNav/SecondaryNav/Content) must be learned from Help; status-bar level labels are jargon. Esc ascends exactly one level; page depth varies per page (Modules/Nodes/Config have secondary nav) → constant over/undershoot. FRICTION, High, Navigation. Evidence: multiple overshoots (Subscriptions instead of Nodes; Modules strip instead of page).

ID: HUMAN-05 — A — Modules
- Module switching undiscoverable without level knowledge. FRICTION, High, Discoverability.

ID: HUMAN-06 — A — Global
- 'q' quits only from TopNav; silent elsewhere. FRICTION, Medium, Navigation.

ID: HUMAN-07 — A — Profiles
- 'c' = copy (expected create; create='n'). On Overview 'c' = Connect. Same letter, opposite-domain meanings per page. FRICTION, Medium, Terminology/Discoverability.

ID: HUMAN-08 — A/B — Select controls
- ‹ › glyph invites Left/Right; only Up/Down cycles; "(choose)" hint appears only after first change. FRICTION, High, Form.

ID: HUMAN-09 — B — Select controls (routing outboundTag, profile Core)
- FAIL: Up/Down only *offers* a candidate ("(choose)"); Enter commits. Uncommitted choice silently dropped → validation "Target: select an outbound or balancer" while the screen visibly showed an outbound chosen. No hint anywhere that Enter commits. FAIL, Blocker, Form.

ID: HUMAN-10 — B — Checkboxes
- Enter toggles checkboxes. Stray Enter (user aiming at buttons) enabled "webhook (advanced JSON)" with empty content → blocking parse error in a field the user never intended. FAIL, High, Form.

ID: HUMAN-11 — B — Validation errors
- Raw library exception shown: "[json.exception.parse_error.101] parse error at line 1, column 1: at…" — truncated, untranslated, no recovery. Also stale: message persisted after fixing value/unchecking (DNS port "Maximum: 65535" after entering 853; webhook error after unchecking). FAIL, High, Error recovery/Feedback.

ID: HUMAN-12 — A/B — Optional-field inputs
- Text input under an unchecked "[ ] Set value" looks editable but silently swallows typed characters (DNS port 5353 lost; Access destination path lost). Tab skips the input even after enabling; Right arrow is the hidden way in. FAIL, High, Form.

ID: HUMAN-13 — B — Sub-collection add
- After adding an entry (domain example.com), the "New domain:" input keeps the text → duplicate risk. FRICTION, Low, Form.

ID: HUMAN-14 — A — Dirty-form discard (CORRECTED after source review)
- Original black-box note: "[ Cancel ] button discarded instantly with no guard → lost routing rule." Source review (native_form.cpp guarded_cancel) shows ALL Cancel paths route through the same dirty guard, and the rule was actually in the parent draft all along. The real defects were HUMAN-16 (stale "No entries" render after apply) plus focus invisibility — I could not tell the apply had succeeded. Downgraded: guard exists and works; the black-box experience of "I lost my work" is itself evidence of HUMAN-16/HUMAN-19 severity.

ID: HUMAN-15 — B/C — Collection selection invisible
- Entry selection in module preview/collection has no persistent highlight; Up/Down move an invisible cursor; Enter/D act on an unknown entry (delete confirm says only "entry 2", not the value). FAIL, High, Feedback/State model.

ID: HUMAN-16 — B — Collection/module view sync
- After applying a sub-form, the module preview showed "No entries" while the draft actually contained the rule (stale render, corrected on reopen). Directly caused a perceived data-loss event (see HUMAN-14 correction). FAIL, High, Feedback.

ID: HUMAN-17 — A — Read-only module recovery
- Inside a read-only form, F1 explains draft/save but NOT how to enable Edit; must Esc out to module level where help says "in Profiles allow module Edit". Blocked action ('a' in read-only) = silent. FRICTION, High, Error recovery/Discoverability.

ID: HUMAN-18 — A — Help text truncated
- Form help body cut off mid-word ("Native pat…") with no scroll affordance. "Enter on ConfirmConfirm" / "DiscardDiscard" duplicated-word rendering bugs. FRICTION, Medium, Visual/Help.

ID: HUMAN-19 — A — Two "Content" contexts
- Module entry-level and section-level both labeled "Content" in status bar; Ctrl-S works in one, silently not in the other. FAIL, Medium, State model/Feedback.

ID: HUMAN-20 — A — Unbound keys silent
- Every unbound key ('a' on Overview, 'u' on Nodes/Logs, 'a' read-only) is a silent no-op. No "nothing here, see F1" affordance. FRICTION, Medium, Feedback.

ID: HUMAN-21 — B — Subscription URL masked
- Edit form shows URL as ••••••••• (non-secret field); list/details never show the URL at all → cannot inspect or partially fix my own subscription URL. FAIL, High, Form/Information.

ID: HUMAN-22 — B — CLI human output
- `profile list` leads with raw UUIDs; core shown as UUID; `status` shows target node as UUID not name. `--json` error message is Chinese ("找不到对应Profile（请使用精确名称或UUID）") — unusable message for scripts; code field fine. FRICTION, Medium, CLI.

ID: HUMAN-23 — A/B — Chinese diagnostics in English UI
- TUI "Last error diagnostic": "连接xrmd失败: no such file or directory". CLI daemon-down diagnostic same. Mixed language + raw errno. FRICTION, High, Terminology.

ID: HUMAN-24 — B — Subscription failure has no reason
- Update of invalid-URL source: list shows "Failed", warning toast "partially completed; inspect source result", F details show counts/ID/socket but never the actual error (why it failed). FAIL, High, Error recovery.

ID: HUMAN-25 — A — Stop core has no confirmation
- Overview 'd' instantly stopped the running core (dropped traffic) — while profile/group/subscription deletes all confirm. FAIL, High, Error recovery (dangerous op).

ID: HUMAN-26 — A — Duplicate nodes indistinguishable
- Two "Loopback fixture" rows (same name/protocol/support) with no source/origin column. FRICTION, Medium, Information hierarchy.

ID: HUMAN-27 — B — Routing target shows internal ID
- Rule list shows target as "xrm.node.3aa6e88d-ed47-4afb-81ee-3fec9bf6a1e3" instead of node name. FRICTION, Medium, Terminology/Information.

ID: HUMAN-28 — A — 80×24 left-tree debris
- At 80×24 the module section tree renders as 1-char vertical letter fragments. 60×18 shows a clean "Terminal too small … draft and focus preserved" guard (excellent, and the promise held). FRICTION, Low, Visual.

ID: HUMAN-29 — A — Logs
- "core: Xray 26.9.9 started" classified Warning (misleading severity). Pre-start lines show TIME "-" (looks like missing data). Detail dialog has jargon fields (Transport chunk/Line within chunk) + irrelevant Socket line. Help says "No core lifecycle actions here" (good) but not where they are. FRICTION, Low-Medium, Logs.

ID: HUMAN-30 — A — Toast-only outcomes
- Some results (incl. one "! Error") appear only as 3-second toasts; recoverable via 'N' history (good) but 'N' is per-context and undiscoverable. FRICTION, Medium, Feedback.

ID: HUMAN-31 — B — Tab-to-button distance
- DNS server form: 14 Tabs from field 2 to buttons; routing: 13–15, count varies because sub-collection buttons join tab order. Ctrl-End scrolls but doesn't move focus. Ctrl-S works in simple forms but not module sub-forms. FRICTION, High, Efficiency/Form.

ID: HUMAN-32 — A — End key dead in sub-form input
- In routing sub-form port input, End did nothing (cursor stayed at 0 → "8535353" prepend); Home worked elsewhere. FAIL, Medium, Form.

ID: HUMAN-33 — B — Long select options overflow
- Core select option "Lab core Xray 26.9.9 (Xray, Penetrates Everything.) 52a412d (go1.27.1 linux/amd64)" overflows field width. FRICTION, Low, Visual.

ID: HUMAN-34 — A — Nodes sub-tab indicator desync
- Sub-tab showed "1/4" while content displayed Groups (3/4). FRICTION, Low, Visual.

## 6. Wrong paths taken (and product response)

| Wrong expectation | Where | Product response |
|---|---|---|
| Stop core in Logs | Logs | Help: "No core lifecycle actions here" — best case, still no pointer where |
| Connect from Profiles | Profiles | 'c' opens copy dialog (misleading same-letter) |
| Enable Edit in Modules | Modules | read-only form help: no path; module-level help: has path |
| Add node from Overview | Overview | 'a' silent no-op |
| Use profile from Nodes | Nodes | 'u' silent no-op |
| Xray modules under Config | Config | Config = core/geodata/appearance…; Xray modules live under "Modules" — naming split must be learned |

## 7. Recovery behavior (positive evidence)

- Disconnect mid-use: "xrmd disconnected · cached data", "Run unknown", no zeroing; auto-reconnect on daemon return; save retry succeeds. Excellent.
- Save while disconnected: persistent banner + toast with actionable text ("Check the socket path… Confirm the result before retrying a write"), draft preserved.
- Invalid JSON save: blocked with located error ("Invalid JSON line 12, column 4…"), draft preserved.
- Required-field validation inline ("! Name: Required"), form retained.
- Unsaved-guard modal on Esc: Save/Discard/Continue, default Save.
- Delete confirmations name the target; subscription remove shows references + node count + keep-nodes option.
- Daemon-down CLI: Error/Reason/Recovery/Next structure with concrete next command; exit codes differentiated (1 not_found, 3 endpoint, 106 usage, 109 unknown-arg).
- Resize: dirty draft/focus/selection survive 80×24→60×18→180×48; explicit too-small guard.

## 8. Screenshots / terminal evidence

Key captures quoted inline above; raw session was tmux `-L xrm-lab` (torn down). Notable verbatim strings: "Module Edit enabled for Lab Home; runtime unchanged", "Read only: in Profiles allow module Edit for Renamed A", "! Target: select an outbound or balancer", "[json.exception.parse_error.101] parse error at line 1, column 1", "连接xrmd失败: no such file or directory", "XRM · Terminal too small / 60x18 available; 80x24 required / Your draft and focus are preserved", "xrmd disconnected · cached data".

## 9. CLI discovery transcript (condensed)

```
xrm                          → grouped help + examples + behavior notes (EXIT 0)   [excellent]
xrm profile list             → UUID\tname\tcoreUUID                                 [works, UUID-heavy]
xrm unknown                  → "not expected: unknown" + root help (EXIT 109)
xrm profile                  → profile help (EXIT 0)
xrm profile create           → "--name is required" (EXIT 106)
xrm profile show does-not-exist → Error/Reason/Recovery/Next + 中文 diagnostic (EXIT 1)
xrm connect does-not-exist   → same structure (EXIT 1)
xrm status (dead socket)     → Reason/Recovery/Next + "连接xrmd失败" (EXIT 3)
xrm --json profile list      → clean JSON array
xrm --json profile show X    → {"error":{"code":"not_found","message":"找不到对应Profile…"}}
xrm profile list | cat/grep  → tab-separated, no ANSI, clean
xrm stats --watch | head     → exits cleanly, no SIGPIPE hang
xrm connect 'Renamed A'      → "Connected Profile Renamed A [uuid]; process running; traffic unverified"
xrm status                   → running; target node shown as UUID
xrm disconnect               → "Stopped running core; saved Profile settings unchanged"
```
Human finds profiles in 2 commands; connect by name works. Main CLI issues: Chinese messages, UUID-first human output.

## 10. Things I could not determine

1. The "粘贴的文本 (1)(3).txt / 粘贴的文本 (2).txt" strings flashed once near the routing domain field (transient toast?) — content unrecoverable; possibly a paste-to-file feature triggered by tmux's input delivery. Unverified.
2. One "! Error" toast after a DNS save faded before reading; not clearly identifiable in 'N' history.
3. Real-IME Chinese input (only direct UTF-8 delivery tested — ENVIRONMENT LIMITATION).
4. Whether [ Cancel ]-button instant-discard vs Esc-guard is intentional asymmetry.
5. Why the routing rule preview showed "No entries" after a successful apply (stale render vs lost-and-regained draft).

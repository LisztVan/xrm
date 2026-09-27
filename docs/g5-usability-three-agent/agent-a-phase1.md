# Phase 1 — Black-box usability evidence

## Environment
- Unique tmux server: `xrm-usability-a`, session `lab`, initial size 120x40.
- Started with exact user-provided command inside the PTY: `python3 scripts/tui-lab.py --build build/component-rewrite`.
- Lab visibly started with one Profile (`Lab Home`), one node, one source, stopped runtime.
- No docs/source/tests/reviews read.
- A prior `xrmux` exploration was detected as pre-existing/concurrently mutated and is excluded from all findings.

## Persona A — first launch/free exploration
- First impression: Xray runtime/profile dashboard. Top navigation and `[Overview]` location are clear.
- Current focus is weakly conveyed only by footer word `TopNav`; selected content uses a triangle.
- Tab naturally changes top page in the clean run; Enter moves from TopNav to Content, but only footer changes.
- Help is easy to find (`F1 Help & actions`); quit is not visible until Help.
- Overview warning `Saved target differs from the running target` appears while stopped and initially feels alarming.
- Footer `View Lab Home · Use Lab Home · Edit none · Run none` initially looks partly like available actions rather than four independent states.

## TASK 1 — Profile
Plan: discover from F1, then create/view/use/cancel use/rename/copy/delete.

### Evidence and reactions
1. TopNav `Tab` -> Profiles; `Enter` -> Content. Screen content stayed identical; only footer focus changed.
2. Content `Enter` on Lab Home produced no visible change because it was already Viewed. Could not distinguish no-op from success.
3. F1 explained: Use selects next connection Profile; Cancel Use and module Edit do not affect running core; Metadata is independent. Actions are discoverable only in Help (`u/e/n/R/m/c/D/...`).
4. Create (`n`): form clearly differentiated TextInput (`[ ]`) and selects (`‹ ›`); `UNSAVED *` dirty state was clear. After Tab through fields, button focus was not visibly distinguishable. Save succeeded with toast `Profile First Profile saved; runtime unchanged`.
5. View new Profile: Up+Enter changed footer to `View First Profile`; low-salience but confirmable.
6. Use: F1 default action + Enter. Row and footer changed; toast `Use set to First Profile; runtime unchanged`. Model became clear.
7. Cancel Use: `u`; row/footer changed to `Use none`; toast explicitly said runtime unchanged.
8. Rename: form prefilled. Tester expected Ctrl-A select-all, but it behaved as Home, resulting in unintended `Renamed AFirst Profile`. This is recoverable but input convention was not explained.
9. Copy: form prefilled sensible `... copy`; save succeeded and runtime unchanged.
10. Delete: target name shown, but consequence text absent and no visible focus between `[ Confirm ] [ Cancel ]`. Enter activated Confirm (dangerous default). After deletion, list selected Lab Home but global footer remained `View Renamed AFirst Profile`, a Profile that no longer exists.

### Metrics
- completion: yes
- time to first correct action: ~1s for page navigation; ~20s to discover management actions via F1
- conceptual actions: 30 (including navigation/form operations; text characters excluded)
- wrong actions: 1 (Ctrl-A expectation)
- Help opens: 2
- page transitions: 1
- errors: 1 unintended rename; recovery possible but not performed
- recovery success: partial
- confidence: create 5/5, view 4/5, use/cancel 5/5, rename 2/5, copy 5/5, delete 3/5

### Candidate observations
- FRICTION / Discoverability / Medium: Profile management actions exist only in Help; page has no visible action strip.
- FRICTION / Visual / Medium: focus changes between TopNav/Content and form buttons are not visibly salient.
- FRICTION / Form / Low: Ctrl-A follows terminal Home behavior, not GUI select-all; no convention reminder.
- FAIL / Safety & state model / High: destructive confirmation has no visible focused button and Enter confirms; deletion leaves View pointing to deleted object.

Evidence captures: `/tmp/xrm-usability-a-task1-*.txt`.

## TASK 2 — Module READ ONLY → Edit
Plan: enter Modules with Edit off, try to change DNS, use only contextual Help, then recover.

### Evidence
1. Modules prominently showed `SOURCE · READ ONLY · Visual`, `Read only` beside DNS servers, and `modules/01_dns.json`. Status recognition was immediate.
2. DNS servers opened a `READ ONLY` collection with only View/Done. No Add/Edit/Delete was offered.
3. F1 in this dialog only said `Enter View entry` and `Changes stay in the parent draft until its Save action`; it did **not** explain how to enable Edit or where to go. In read-only context, the parent-draft sentence was confusing.
4. Recovery was possible only because TASK 1's Profiles Help had incidentally disclosed `e Allow module Edit`. Required closing Help/dialog, backing through Content and SecondaryNav, returning to Profiles, entering Content, enabling Edit, and navigating back to Modules/dns.
5. Enable feedback was strong: Profile row `Edit yes`, footer `Edit Lab Home`, toast `Module Edit enabled for Lab Home; runtime unchanged`. Back in DNS, state became `SOURCE · EDIT · Visual` and `Draft`.
6. Rapid multi-key navigation attempted once earlier only applied the first visible state change; paced key input worked. This will be retested under Persona C before treating as a product finding.

### Metrics
- completion: yes
- time to first correct action: immediate recognition of read-only; recovery path not discoverable from current Help
- conceptual actions: 18
- wrong actions: 1 rapid sequence (possibly timing-related)
- Help opens: 1
- page transitions: 4 (Modules→Profiles→Modules plus focus levels)
- error count: 0 product errors
- recovery success: partially (only via prior incidental knowledge)
- confidence: 5/5 after returning to `SOURCE · EDIT`

### Observation
ID: HUMAN-02
Persona: A
Task: Module Read Only → Edit
Screen: Modules / DNS servers
Before action: expected Help to explain how to leave READ ONLY.
Action: F1.
Actual: Help explained View and parent draft, but no Edit-enabling path.
Human reaction: stuck in the correct feature area and forced to remember a shortcut from another page.
Recovery: left Modules, returned to Profiles, used `e`, navigated back.
Could recover: Partially
Result: FAIL
Severity: High
Type: Discoverability / Navigation / State model
Evidence: `/tmp/xrm-usability-a-task2-readonly-help.txt`, `...-edit-enabled.txt`, `...-edit-dns.txt`.

## Persona B — TASK 3 DNS Visual
- Collection UI felt like managing DNS servers: table plus View/Add/Edit/Copy/Delete; reorder appeared contextually as Up/Down.
- Add form exposed 15 native fields at once. Presence checkboxes explicitly said `field omitted`, which is precise, but the experience felt like manipulating a configuration structure for a basic server addition.
- Form F1 was excellent: explained Ctrl-S `Add to draft`, object draft vs module Source vs Connect runtime, cancel scope, native path, and document navigation.
- Add succeeded; `UNSAVED *` appeared at module level.
- Collection focus became invisible/stuck after Help/return: row retained `>` but Down and `e` did nothing. Closing and reopening the collection restored keyboard operation. This made the tester unable to know whether focus was on rows or buttons.
- Modify exposed an input convention failure: Ctrl-U did not clear the field; new value prefixed old (`8.8.8.81.1.1.1`). No address validation prevented the malformed value entering draft.
- Delete confirmation was strong: explicitly said draft only; Source/runtime unchanged.
- Reorder via `-` worked and selection followed the moved object.
- Visual→JSON showed the same order/values; F2 back preserved draft. JSON outer array was recognizable but mildly surprising to an Xray user expecting a root object.
- Module Ctrl-S saved Source with toast `runtime unchanged`.

Metrics: completion yes; actions ~31; wrong actions 4; Help opens 2; page transitions 0; errors 1 malformed edit; recovery partial; confidence 4/5. Reorder yes. Visual/JSON mental model retained.

Key findings:
- HUMAN-03, FRICTION, High, Form/Focus: collection row marker remains while shortcuts/arrows stop acting; no visible button focus; recover by close/reopen.
- HUMAN-04, FRICTION, Medium, Form: common line-edit clearing expectations fail and malformed DNS value is accepted into draft; input preserved but correcting requires manual deletion.
- HUMAN-05, FRICTION, Medium, Information hierarchy: basic DNS Add presents 15 native fields and nested collection controls; expert can proceed via Ctrl-S after Help, novice faces high density.

Evidence: `/tmp/xrm-usability-a-task3-*.txt`.

## Persona B — TASK 4 Routing
- Routing overview cleanly separated ordered rules, native balancers, and domain strategy; native path was visible.
- Empty collection used human task columns `Rule / match` and `Target`.
- Add form contained 18 fields. It explicitly grouped `Match conditions -> Target`, clarified ruleTag is not a match, and described domain/IP/protocol meanings.
- Domain presence toggle and collection worked, but the New domain input retained text after Add, creating duplicate-add risk.
- Protocol used finite checkboxes (http/tls/quic/bittorrent), not keyword typing, and explained sniffing dependency.
- Common navigation shortcuts failed inside nested collection focus: Ctrl-End and PgDn did not move to document end; Tab count included hidden composite stops. Reaching fields required many key presses and field numbers did not map to key count.
- Submit validation preserved all input and clearly said `Target: select an outbound or balancer`, but did not focus/scroll to target.
- F1 said `Tab to Target: outboundTag / balancerTag`; actual traversal from field 17 through field 18 to buttons exposed no target choices in the current lab state. No disabled reason or prerequisite/navigation advice was shown. Rule creation was blocked.
- Dirty-cancel dialog explained Add-to-draft vs Discard vs Continue and all persistence effects very well, but button focus remained invisible. Tab+Enter successfully discarded.

Metrics: completion no; rule add blocked, therefore modify/delete/reorder not testable; actions ~50 (mostly Tab); wrong paths 3; Help opens 1; error count 1 validation; recovery yes via discard; confidence 1/5.

HUMAN-06: Persona B, Routing add, Form/Navigation, FAIL High. Human understands missing target but cannot find or select it; product does not say that no targets exist or where to create one. Evidence `/tmp/xrm-usability-a-task4-validation.txt`.
HUMAN-07: Persona B, Routing long form, Efficiency/Form, FRICTION High. Nested controls trap document navigation and require opaque repeated Tab traversal.

## Persona B — TASK 5 Subscription
- List hierarchy was strong: Source/Nodes/Result/Auto/Bound visible. F1 disclosed every required action.
- Add form used clear field types and sensible defaults. `not-a-url` was accepted and saved; failure was deferred to Update.
- Update preserved selection and existing inventory and did not let toasts steal focus. Failure feedback was only `partially completed; inspect source result`; list showed `Failed` with no reason.
- Enter/View gave no visible detail. `F Full object details` showed UUID, temporary daemon Socket, raw field names and binding state, but omitted URL and actual download error. Recovery remained impossible from details.
- Edit preserved Name but masked URL as bullets inside the edit form. User could not inspect or locally correct the failed URL and had to replace it blind; Home+Delete using remembered length worked.
- Saving changed settings to explicit `Settings changed`; Update against loopback failed without clearing data.
- Pause/resume toggled Auto on/off, preserved focus, and said runtime unchanged.
- Bind used an excellent finite MultiSelect with source names and Profile title; Space + Ctrl-S worked.
- Remove form showed referencing Profile, inventory count, and Keep nodes as local before removal; Ctrl-S removed successfully.

Metrics: completion yes; actions ~25; wrong actions 1; Help opens 1; errors 2 intentional failures; recovery partial (could replace URL, could not diagnose cause); confidence 4/5 for CRUD, 1/5 for failure diagnosis.

HUMAN-08: FAIL High Error recovery — update failure has no concrete cause/action; Full Details prioritizes UUID/socket/internal keys over the URL/error.
HUMAN-09: FAIL High Form — subscription URL is masked in the edit form, preventing inspection/correction and making accidental credential replacement likely.
HUMAN-10: FRICTION Medium Validation — syntactically invalid URL is saved and only later becomes generic update failure.
Evidence: `/tmp/xrm-usability-a-task5-*.txt`.

## Persona B — TASK 6 Node / Group
- Nodes secondary navigation made Groups discoverable as `Scope: Groups`; Saved/Run columns on nodes supported target-state distinction.
- Empty Groups screen had no inline Add affordance; F1 was required.
- Group form used finite Select/MultiSelect/Checkbox controls for mode, strategy, scope, nodes, sources, manual member, protocols. Invalid typed `x` did not alter Select or trigger a page shortcut.
- Empty Name validation was inline and preserved draft, but `Name: Required` remained after a valid name was typed until resubmission.
- Strategy looked selectable (`‹ Random ›`) but Right/Left/Enter/Space produced no feedback; perhaps only one option existed, but no disabled/only-one explanation appeared.
- Node MultiSelect worked with Space; edit retained true selections. Save succeeded.
- Group summary `Members: explicit` omitted member count/names, reducing scan value.

Metrics: completion yes; actions ~19; wrong actions 4 while guessing Select; Help opens 1; errors 1 intentional; recovery yes; confidence 4/5.
HUMAN-11: FRICTION Medium Form/Feedback — Select affordance suggests changeability but common keys do nothing and no disabled reason is shown.
HUMAN-12: FRICTION Low Form — corrected required-field error remains stale.
Evidence: `/tmp/xrm-usability-a-task6-*.txt`.

## Persona C — TASK 7 Run
- Natural first stop Profiles: Use worked, but Profile Help had no Connect and no pointer to Overview.
- Overview Help clearly stated Connect uses Lab Home, publishes saved settings only, stats prerequisites, and offered Connect/Stop together.
- Connect failed because Core not set. Error was highly actionable: choose registered core/register binary; runtime remained stopped.
- Config defaulted to Core and showed `No registered cores`; F1 exposed Register core.
- Register form gave no lab test-core path or discovery assistance. Safe `/bin/true` was rejected with misleading message `Xray rejected the candidate configuration... correct the saved configuration`, diagnosing invalid binary as configuration error.
- Without reading docs/source or using a real Xray, the test core could not be discovered. Connect/status running target/log generation/stats/Stop after a real run were not safely testable.
Result: FAIL/ENVIRONMENT LIMITATION for end-to-end run. Completion no. Confidence 1/5.

## Persona C — TASK 8 Logs
- Empty page clearly said Core output, No log entries, `Following · 0 text lines`.
- F1 explained Pause buffers new text, Enter shows original/transport details, and explicitly `No core lifecycle actions here`; this is excellent wrong-path guidance.
- Pause and End resume updated footer correctly.
- No core could be started, so scroll/detail/error prioritization, sequence/source/level usefulness, buffered new logs, cache after disconnect could not be evaluated. `xrm logs` CLI on empty buffer returned empty stdout exit0 with no `no logs` marker.
Result: partial / environment limitation.

## TASK 9 Help synthesis
- Profiles: explains View/Use/Edit side effects and metadata distinction; actions complete. Strong, but basic CRUD is only discoverable here.
- Modules content/Form: excellent three-level draft/Source/runtime explanation, native path, cancel scope, shortcuts. Strong.
- Modules READ ONLY collection: fails to explain how/where to enable Edit; says changes stay in parent draft despite no changes possible.
- Nodes Groups: complete actions but no inline empty-state Add.
- Logs: excellent pause/live and no-lifecycle explanation.
- Dialogs: persistence consequences are usually excellent; focused button is visually indistinguishable.
- Offline startup: F1 gives no xrmd start/socket/retry guidance. Last diagnostic only shows Chinese errno + socket.

## Resize
Same TUI process with selected Test Group and dirty `XTest Group` Form:
- 120x40: full background + form.
- 80x24: form remained usable; background columns disappeared/covered.
- 60x18: dedicated `Terminal too small`, reports 60x18 and requires 80x24, promises draft/focus preserved.
- return 80x24 and 180x48: dirty value, field1, object, focus preserved.
- 180x48: more background/list width, but form fixed width and mostly additional whitespace.
Result: PASS; evidence `/tmp/xrm-usability-a-resize-*.txt`.

## Input controls
- TextInput exact ASCII result after sequence: `^abcdefghijkXnop$`; cursor, BSpace/Delete/Home/End correct. q/e/u/c/d/p/? stayed text.
- Direct UTF-8 bytes rendered `测试中文输入ABC` with correct alignment. Real IME composition was not available: ENVIRONMENT LIMITATION.
- TextArea JSON: Enter/Up/Down/Home/End/Ctrl-Home/Ctrl-End all verified via markers (`H{`, prior-line `U`, `^Dqeucdp?$T`); global shortcut letters stayed text. Unsaved discard confirmation worked.
- Select: Strategy presented arrows but common keys produced no change/disabled reason.
- Checkbox/MultiSelect/CollectionEditor: representative operations worked; collection focus ambiguity documented in HUMAN-03.
Evidence `/tmp/xrm-usability-a-input-*.txt`.

## Chaos / error recovery
- Daemon disappears with data: list and selection preserved; Run becomes unknown; footer `xrmd disconnected · cached data`. PASS.
- Disconnected Help lists write actions but no disabled reason/reconnect guidance; Refresh silently no-ops. FAIL Medium.
- Daemon returns: auto reconnect, toast `Connection restored; cached lists refreshed`, selection preserved. PASS.
- Disconnected save in dirty Group Form: all input/selections/focus/UNSAVED preserved; inline actionable endpoint error; cached/unknown background; auto reconnect retained draft. PASS.
- Startup without daemon: Overview correctly shows unknown/unavailable/stale and Inventory unavailable, not zero. F1 has no start/retry/socket guidance; Last diagnostic exposes Chinese errno + socket only. FAIL High.
Evidence `/tmp/xrm-usability-a-daemon-*.txt`, `...-disconnected-save.txt`, `/tmp/xrm-usability-offline-*.txt`.

## CLI discovery transcript
1. `xrm` -> grouped root help, examples, next step; exit0 stdout. Explicitly says help local, saving/selecting do not connect, JSON/errors channels.
2. `xrm profile` -> subcommands/examples; exit0.
3. `xrm profile list` -> two rows, no headers, UUID first; target found after 3 commands / 2 Help screens.
4. `profile show Lab Home` accepts name but outputs Profile UUID and saved target node UUID instead of node display name.
5. `node` then `node list` discoverable; output again UUID-first/no headers.
6. `config` clearly separates native module operations and examples. `config edit` only supports inline `--text`; no stdin/file, awkward for large JSON. `config modules/show` work with name.
7. status/connect/logs/stats/disconnect: connect error gives exact core list/register/use next commands. Empty logs is silent success. Disconnect while already stopped says `Stopped running core`, misleading.

CLI errors:
- unknown exit109, full root help on stderr (actionable but long).
- profile create missing name exit106, concise required/example.
- missing Profile/Node/Core exit1 with Reason/Recovery/Next, but appends socket path, Chinese original diagnostic, and UUID wording.
- daemon unavailable exit3 with good Reason/Recovery/Next but raw `no such file or directory` + Chinese diagnostic.

Pipelines:
- human status piped to cat: no ANSI, both exit0.
- grep works but UUID pollutes rows.
- `--json status`: valid single-line JSON object, exit0.
- human and JSON `stats --watch | head`: lines flush, JSON is one valid object per line, but producer treats closed stdout as io_error, writes stderr and exits3 instead of normal SIGPIPE behavior.
- Ctrl-C in tmux PTY was quiet except `^C`; wrapper could not reliably capture child exit code (environment limitation).

CLI findings:
HUMAN-13 Medium CLI/Information hierarchy: UUID-first unheaded human lists and UUID targets undermine name-first UX.
HUMAN-14 Medium CLI/Error recovery: errors append internal socket/raw mixed-language diagnostics after otherwise excellent recovery text.
HUMAN-15 High CLI/Pipeline: watch treats downstream close/SIGPIPE as error (exit3/stderr), breaking `| head`.
HUMAN-16 Low CLI/Feedback: empty logs silent exit0; disconnect while stopped claims it stopped a core.
HUMAN-17 Medium CLI/Efficiency: config edit lacks stdin/file input for module JSON.

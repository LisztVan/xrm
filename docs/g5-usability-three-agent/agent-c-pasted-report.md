<!-- 用户消息中粘贴的 Agent C 完整输出；与 agent-c-part-a-blackbox.md 并存，未改写原文。 -->

Black-box phase complete; lab torn down. Below is the report. Phase‑2 source review was **not** performed (only `docs/xrm-interaction-spec.md` was opened, for classification) — Part C says so explicitly.

---

# Part A — Black-box human simulation

Environment (all isolated, then destroyed): lab from `scripts/tui-lab.py --build build/component-rewrite`, socket `/tmp/xrm-component-lab-fk0d8bxa/run/xrmd.sock`, own DB, own HOME/XDG, real `xray` v26.9.9 registered manually. PTY via dedicated tmux server `-L xrmux` (no terminal MCP was loaded in this host). No production socket/subscription/TUN touched.

**Environment blocker found before testing:** `/tmp` (16 GB tmpfs) was **100 % full** — all labs failed with `OSError: [Errno 28] No space left on device` and `tui-lab.py` exited 120 with an empty record file. I reclaimed ~13 GB from days-old orphan build trees in `/tmp` (no process held them). Without that, the isolated lab cannot start at all.

## 1. Persona A journey — first-time user, no docs, no source

**First screen (verbatim):**
```
 XRM [Overview] Profiles  Modules  Subscriptions  Nodes  Logs  Config
 Overview  Runtime & saved configuration
 Runtime  ACTUAL                    Saved configuration  VIEW Lab Home
 stopped                            Saved target      Node · Loopback direct
 Running Profile   none             Next Connect      Lab Home
 Actual target     none             Saved settings apply only on Connect / Apply
 Health            Not running
 ! Saved target differs from the running target
 Statistics  OBSERVED
 Traffic: unavailable · latency: unmeasured
 Unavailable reason: core not running
 Inventory  1 node · 1 source
 View Lab Home · Use Lab Home · Edit none · Run none
 View Lab Home
 xrmd ready · TopNav                              F1 Help & actions
```

Think-aloud, in order:

- "It's a status dashboard. Fine. `Use Lab Home` / `Edit none` / `Run none` looks like an action bar — **which key does which?** Nothing says."
- EXPECT: Right arrow moves the tab strip → OBSERVATION: **nothing happens**. (A horizontal tab bar that ignores Left/Right.)
- EXPECT: Tab moves focus inside the page → OBSERVATION: *the page changed to Profiles*. Accident, not intent.
- EXPECT: Enter on the `▸ Lab Home` row opens the profile → OBSERVATION: **the whole app jumped to the Modules page.** "Did I change the profile? Did I leave Profiles?"
- Press `Down` in Modules → module changed log→dns, but I could not tell whether that was the module strip or the inner list.
- 8 probes later I still could not name a key for any action. Pressed F1 (advertised on every screen).
- F1 at Overview/Modules level shows a *2-item action menu*, not documentation: `Enter Open Modules / log`, `q Exit TUI`, plus a keyless dangling row `Last error diagnostic`. **It never mentions page switching, the module strip, Use/Edit/Run, or the focus levels.**
- Only by descending (Enter, Enter) and pressing **F1 a second time** did the real action set appear (`u e n R m c D N r F Enter`). *Create/rename/copy/delete were invisible until then.*

Persona A verdict: the product has a real action model, but it is **two F1 presses and one invisible focus level deep**, and the only always-visible hint (the footer line) is phrased as actions while being status.

## 2. Persona B journey — knows Xray/Linux, first time on XRM

I know Xray, DNS, routing, JSON, TUI conventions. What I could not infer from the product:

- **`View` ≠ `Enter`-opens-object.** `Enter` on a Profile row updates the footer's `View X`; on a first press it navigated to Modules; on another it did not. I could not predict which.
- **`Edit` is not a profile-metadata editor.** The contract and the UI agree, but the *label* invites that reading; I only learned it means "this Profile may write module source" from the dialog help sentence.
- **Read-only recovery is not in Modules.** With `SOURCE · READ ONLY` I had no in-page path; enabling edit lives on *Profiles* (`e`), a different page, undocumented on Modules. Pure tribal knowledge.
- **Module strip = `Tab` at the SecondaryNav level.** This is the single biggest expert-blocker: I could read `1/15` and `›` but no key I tried (Left, Right, Up, Down, `p`, `n`, `-`, `]`, `[`, `>`, `<`, `j`,`k`,`h`,`l`,`g`,`G`,`w`,`b`, letters a–z, `C-Left/Right`, `S-Left/Right`, `PgUp/PgDn`, `Home/End`) moved it in the state I was in. It finally moved by luck when I mashed `BTab` for page switching while focus was in SecondaryNav.
- **CLI/TUI naming collision.** `xrm config modules` edits Xray modules; the TUI `Config` page is Core/Geodata/Tests/Appearance/Editor/General. An expert types `config` for the wrong thing on both sides.
- **Subscription refs are UUIDs while Profile refs are names.** `xrm sub show "Local fixture"` → `找不到对应Profile/订阅来源不存在` although `sub list` prints that exact name.

Persona B verdict: the *domain* model (modules as native keys, draft→Source→runtime, confdir) is sound and legible once found. The **navigation and mode model is not inferable**; it requires memorising an internal three-level vocabulary (`TopNav`/`SecondaryNav`/`Content`) that only appears as a 10-character token in the bottom-right corner.

## 3. Persona C journey — experienced user

- **Page switching is O(n).** There is no direct jump (contract: no digits/`[]`/arrows). From Subscriptions to Config: 3 `Tab`s. From Modules to Profiles: 2 `BTab`s. I repeatedly wrote loops to reach a page.
- **The level you are in silently redefines `Tab`.** Same key = next page / next module / next field / next scope, depending on the invisible level. To change a module I must first `Esc` to SecondaryNav; to change a page I must first `Esc` to TopNav. `Esc` itself did not always do that (below).
- **`Esc` is unreliable.** On the routing rule form, alternating presses produced closed → open → closed → open. Twice I ended with focus stuck in `Content` where `Tab`/`BTab`/`Esc` all did nothing, and I had to abandon the TUI session.
- **Drafts break navigation.** With an unsaved routing rule draft I could not get back to TopNav by any key sequence I tried; a fresh TUI session was the only recovery.
- **High-frequency cost:** adding one DNS server = `n a` + type + `Ctrl-S` (good) but the collection editor's row cursor never moved, so a second server could never be edited/deleted/reordered.
- CLI is fast and scriptable; TUI is correct but expensive in keystrokes for routine actions.

## 4. Task completion table

| # | Task | Completion | First correct action | Actions (approx) | Wrong actions | Help opens | Page hops | Errors | Recovery | Confidence |
|---|---|---|---|---|---|---|---|---|---|---|
| 1a | Create Profile | yes | ~90 s (required 2 help levels) | ~14 | 3 | 2 | 0 | 0 | yes | 4/5 |
| 1b | View Profile | **ambiguous** | unclear (Enter semantics vary) | 3 | 2 | 1 | 1 | 0 | partial | 2/5 |
| 1c | Use Profile | yes | ~20 s | 3 | 1 | 1 | 0 | 0 | yes | 5/5 |
| 1d | Cancel Use | yes | ~5 s | 1 | 0 | 0 | 0 | 0 | yes | 4/5 |
| 1e | Rename | yes | ~15 s (needs Help) | 12 | 1 | 1 | 0 | 0 | yes | 5/5 |
| 1f | Copy | yes | ~10 s | 4 | 0 | 1 | 0 | 0 | yes | 5/5 |
| 1g | Delete | yes | ~15 s | 3 | 0 | 1 | 0 | 0 | yes | 4/5 |
| 2 | Read-only → Edit | partial | found Edit only from Profiles | ~30 | many | 3 | 2 | 0 | partial | 2/5 |
| 3 | DNS visual add/edit/delete/reorder | **partial** | reached DNS only by luck | ~40 | many | 4 | — | 0 | partial | 2/5 |
| 3b | Visual→JSON→Visual | yes (F2) | fast | 3 | 0 | 1 | 0 | 0 | yes | 5/5 |
| 4 | Routing rule add | **no** (draft left unresolved) | form opened | ~35 | many | 2 | 1 | 0 | no | 1/5 |
| 5a | Subscription add | yes | ~25 s | 6 | 0 | 1 | 0 | 0 | yes | 5/5 |
| 5b | Update (failure) | yes, poorly explained | `u` | 1 | 0 | 0 | 0 | 1 | no (no cause shown) | 2/5 |
| 5c | Pause/resume source | yes | `t` | 1 | 0 | 1 | 0 | 0 | n/a | 4/5 |
| 5d | Bind source to Profile | **not tested** | — | — | — | — | — | — | — | — |
| 5e | Remove source | **not tested** | — | — | — | — | — | — | — | — |
| 6 | Group create/modify | **not tested** (Groups = a Nodes *scope*, not a page) | — | — | — | — | — | — | — | — |
| 7 | Run/Connect | yes | ~60 s | 5 | 3 | 2 | 2 | 0 | yes | 4/5 |
| 7b | Stop | found in Help (`d`) | — | 0 | 0 | 1 | 0 | 0 | yes | 3/5 |
| 8 | Logs scroll/pause/resume | yes | ~25 s | 5 | 1 | 1 | 0 | 0 | yes | 5/5 |
| 9 | Help on Profiles/Modules/Form/Nodes/Logs/Dialog | yes | — | — | — | 8 | — | — | — | 3/5 (thin at top levels) |
| 10 | CLI discover Profile list | yes | 1 command | 2 | 1 | 1 | — | 0 | yes | 5/5 |
| 11 | CLI error paths | yes | — | 6 | 0 | 3 | — | 6 | yes | 4/5 |
| 12 | CLI scripts/pipes/SIGPIPE | yes | — | 5 | 0 | 0 | — | 0 | yes | 5/5 |
| 13 | Resize 120→80→60→180 | yes | — | 4 | 0 | 0 | 0 | 0 | yes | 5/5 |
| C1 | daemon not running (CLI) | yes | — | 1 | 0 | 0 | — | 1 | yes | 5/5 |
| C2 | daemon disappears (TUI) | yes | — | 0 | 0 | 0 | 0 | 0 | yes | 5/5 |
| C3 | daemon reconnect | yes (automatic) | — | 0 | 0 | 0 | 0 | 0 | yes | 5/5 |
| C4 | Save failure in a form | **not tested** | — | — | — | — | — | — | — | — |
| C5 | Invalid JSON in F2 | **not tested** | — | — | — | — | — | — | — | — |
| C6 | Dangerous op confirmation | yes | — | 2 | 0 | 1 | 0 | 0 | yes | 3/5 |

## 5. All FRICTION / FAIL observations

**HUMAN-01** — Persona A/B/C · Tab · Screen: TopNav
Before: "Right arrow, or Tab, moves the tab bar." · Action: `Right` (nothing), `Tab` (page changes)
Actual: `Right`/`Left` do nothing in TopNav; `Tab`/`Shift-Tab` change pages.
Reaction: confused — the nav is drawn as a horizontal strip but arrows are inert.
Recovery: accidental discovery of `Tab`. Could recover: Yes. Result: **FRICTION** · Severity **High** · Type: Navigation.
Evidence: `Right` → header still `[Overview]`, no repaint; next `Tab` → `Overview [Profiles]`.

**HUMAN-02** — Persona A/B/C · any page · the footer line
Before: "`Use Lab Home` is the key that uses the profile." · Action: `u` on Profiles while cursor was on `Test One`
Actual: footer read `View Test One · Use Lab Home · …`; the same screen's Help read `Use Profile · Test One`.
Reaction: "which object will `u` hit?" — stopped and re-opened Help before acting.
Recovery: Help clarified. Could recover: Yes. Result: **FRICTION** · Severity **High** · Type: State model / Information hierarchy.
Evidence: cursor `▸ Test One` + footer `View Test One · Use Lab Home` simultaneously.

**HUMAN-03** — Persona A · Modules · enabling edit
Before: "`SOURCE · READ ONLY` — something must let me edit here." · Action: `F1` at Modules TopNav and SecondaryNav, `Enter`, `Tab`, arrows, `p`, letters
Actual: no Help level on Modules mentions read-only or how to enable writing; only the Profiles page has `e Allow module Edit`.
Reaction: "I have no way out of read-only from here." Had to leave the page and remember a key from another page.
Recovery: returned to Profiles, `F1`, then `e`. Could recover: Partially. Result: **FAIL (recovery path)** · Severity **High** · Type: Discoverability / Error recovery.
Evidence: Modules help = `Enter Open Modules / log`, `q Exit TUI`, `Last error diagnostic`; no Edit entry anywhere.

**HUMAN-04** — Persona A/B/C · Modules · switching modules
Before: "`1/15` and `›` mean I can reach other modules." · Action: Left/Right/Up/Down, `p`, `n`, `-`, `]`,`[`,`<`,`>`, `j k h l g G w b`, a–z, `C-Left/Right`, `S-Left/Right`, `PgUp/PgDn`, `Home/End`
Actual: **none** of these moved the strip in the state I was in; Help documents no key for it either. It only moved when I pressed `Tab`/`BTab` while the breadcrumb happened to read `SecondaryNav`.
Reaction: repeatedly stuck; twice considered the feature absent.
Recovery: found it by accident; continued by counting `Tab`s. Could recover: Partially. Result: **FAIL** · Severity **Blocker** · Type: Discoverability / Navigation.
Evidence: 15 consecutive keys with `mod=1/15` unchanged; then `Tab` → `9/15` while region=`SecondaryNav`. Help's only relevant line is the ambiguous `Tab / Shift-Tab  Choose within this navigation level`.

**HUMAN-05** — Persona A/B/C · Modules · the invisible level
Before: "`Tab` moves to the next page." · Action: `Tab` with focus in SecondaryNav
Actual: the *module* changed, not the page.
Reaction: lost track of where I was; could not predict `Tab`.
Recovery: read the bottom-right token `TopNav`/`SecondaryNav`/`Content` and reverse-engineered the rule. Could recover: Partially. Result: **FRICTION** · Severity **High** · Type: State model / Terminology.
Evidence: region `SecondaryNav · modules/07_metrics.json`; `Tab` → `08_stats.json` → `09_policy.json`; page unchanged.

**HUMAN-06** — Persona A/B/C · `Esc`
Before: "`Esc` closes the form." · Action: `Esc` ×4 on the routing-rule form
Actual: closed → **reopened** → closed → reopened, deterministically.
Reaction: afraid of the key; stopped using it to escape forms.
Recovery: re-closed with an odd number of presses. Could recover: Partially. Result: **FAIL** · Severity **High** · Type: Navigation / Form.
Evidence: `Esc#1 → "Domain strategy"`, `Esc#2 → "Doma│"`, `Esc#3 → "Domain strategy"`, `Esc#4 → "Doma│"`.

**HUMAN-07** — Persona B/C · unsaved draft
Before: "`Esc` / `Tab` will leave." · Action: 5×`Esc`, 6×`Tab` with an unsaved routing rule
Actual: region stayed `Content · modules/04_routing.json`; no key left the page.
Reaction: believed I was stuck; had no explanation of why.
Recovery: started a second TUI session. Could recover: Partially. Result: **FAIL** · Severity **High** · Type: Navigation / State model.
Evidence: region unchanged across 5 `Esc`; `page=[Modules]` across 6 `Tab`.

**HUMAN-08** — Persona A · Profiles · Enter semantics
Before: "Enter opens the highlighted profile." · Action: `Enter` on the row
Actual: first time it switched to Modules; later it only updated the footer (`View Test One`), staying on Profiles.
Reaction: could not predict the result.
Recovery: none needed, but confidence 2/5. Could recover: Yes. Result: **FRICTION** · Severity **Medium** · Type: State model.
Evidence: `Enter` → `page=[Modules]` (first session) vs `Enter` → `page=[Profiles] region=Content`, footer `View Test One`.

**HUMAN-09** — Persona A/B/C · Profile list
Before: "`View Lab Home · Core not set · Saved Node · Loopback direct` describes the row I highlighted." · Action: `Up` to `Test One`
Actual: the detail line kept naming Lab Home while `▸` was on `Test One`.
Reaction: stopped before pressing `r`/`F`, unsure of the target.
Recovery: Help showed the real target. Could recover: Yes. Result: **FRICTION** · Severity **Medium** · Type: Information hierarchy.
Evidence: `▸ Test One` + `View Lab Home · Core not set · Saved Node · Loopback direct`. (Note: on Subscriptions the same line *does* follow the highlight: `Highlighted Bad source · Failed · 0 nodes` — the behaviour differs per page.)

**HUMAN-10** — Persona A/B/C · Delete
Before: "`D` → some dialog; Enter should be safe if I did not mean it." · Action: `D`, inspect focus, `Enter`
Actual: dialog shows only the name; **`[Confirm]` is focused by default**; `Enter` deletes immediately. No "cannot be undone", no reference count.
Reaction: careful; a novice holding Enter after a typo would delete data.
Recovery: n/a. Could recover: n/a. Result: **FRICTION** · Severity **Medium** · Type: Feedback / State model.
Evidence: colored capture shows `[ Confirm ]` with the active background before any `Tab`.

**HUMAN-11** — Persona A/B/C · collection editor (DNS servers)
Before: "Up/Down moves the row cursor (`>`), Help says so." · Action: `Down`, `BTab`+`Down`, `-` (Move entry down)
Actual: cursor stayed on row 1 with two rows present; only row 1 was ever actionable. `-` did nothing.
Reaction: could not edit/delete/reorder the second server.
Recovery: none found. Could recover: No. Result: **FAIL** · Severity **High** · Type: Form.
Evidence: after `a`+`Ctrl-S`, rows `1 localhost`, `2 1.1.1.1`; `Down`/`BTab`+`Down`/`-` → `> 1` unchanged.

**HUMAN-12** — Persona A/B/C · reorder
Before: "I will move `localhost` down." · Action: `-`
Actual: nothing; Help lists `- Move entry down` but no move-up counterpart and no `[Up]` button (only `[ Down ]`).
Reaction: asymmetric, undiscoverable, and inert in practice.
Recovery: none. Could recover: No. Result: **FAIL** · Severity **Medium** · Type: Discoverability / Form.

**HUMAN-13** — Persona A/B · module strip overflow
Before: "`›` means there is more; something scrolls it." · Action: reach `15/15`
Actual: it *does* scroll (`‹ routing … version burstObservatory  15/15`) — good — but nothing tells you the key, and at rest the last two of 15 modules are simply invisible.
Reaction: for a long time I believed only 13 modules existed.
Recovery: accident. Could recover: Partially. Result: **FRICTION** · Severity **Medium** · Type: Discoverability.

**HUMAN-14** — Persona A · terms
Before: "`TopNav`, `SecondaryNav`, `Content` are probably something technical." · Action: none
Actual: they appear in the bottom-right on every screen with no definition anywhere, including Help.
Reaction: ignored them; only later realised they are the key to the whole navigation model.
Recovery: n/a. Result: **FRICTION** · Severity **Medium** · Type: Terminology.

**HUMAN-15** — Persona A/B/C · Help thinness by level
Before: "F1 will tell me what I can do here." · Action: `F1` at Nodes SecondaryNav / Modules SecondaryNav
Actual: Nodes SecondaryNav help = only `Enter Open Nodes / all`. Config SecondaryNav = only `Enter Open Config / Core`. The useful list requires descending one more level.
Reaction: "help says there is one thing I can do — that cannot be right."
Recovery: `Enter` then `F1` again. Could recover: Yes. Result: **FRICTION** · Severity **Medium** · Type: Discoverability / Information hierarchy.

**HUMAN-16** — Persona A/B/C · dangling Help rows
Before: "each Help row has a key." · Action: read Help
Actual: `Last error diagnostic` and `Editing scope and advanced fields · DNS server` appear as action rows **with no key and no action**.
Reaction: thinks a key is missing / a rendering bug.
Result: **FRICTION** · Severity **Low** · Type: Visual.

**HUMAN-17** — Persona A · dialog titles
Before: "`EDIT` means editing something existing." · Action: `n` (New Profile) / DNS `Add`
Actual: `new Profile  EDIT · field 1/3`; `DNS server  EDIT · field 1/15`; the submit button reads `Add to draft`.
Reaction: unsure whether I was creating or modifying.
Result: **FRICTION** · Severity **Low** · Type: Terminology.

**HUMAN-18** — Persona A/B · overlay rendering
Before: "the dialog is the foreground." · Action: open a form / Help over Logs
Actual: underlying content is not cleared — log text and `> DNS` bleed into the margins and between the dialog borders.
Reaction: hard to read; looks broken.
Result: **FRICTION** · Severity **Medium** · Type: Visual.

**HUMAN-19** — Persona A/B/C · subscription update failure
Before: "Warning tells me the cause." · Action: `u` on a URL pointing at `127.0.0.1:9`
Actual: toast `! Warning  Update Bad source: partially completed; inspect source result`; row `Failed`; `F` details show `last_result: failed` and **no reason at all**; no recovery action offered.
Reaction: "inspect what? where?" — could not learn the cause anywhere in the TUI.
Recovery: none. Could recover: No (in TUI). Result: **FAIL** · Severity **High** · Type: Error recovery.
Evidence: Details panel = ID / Last attempt / Last success / bindings / `enabled: true` / `last_result: failed` / `unsupported_count: 0` / `malformed_count: 0` / Socket. No error text.

**HUMAN-20** — Persona B/C · CLI list formats
Before: "`profile list`, `core list`, `sub list`, `node list` should look alike." · Action: run all four
Actual: `profile list` has no header and dumps UUID first; `core list` prints **nothing at all** (exit 0) when empty; `node list` prints `Nodes: 2` + rows; `sub list` prints a UUID plus `last_attempt=1790476342198` epoch millis.
Reaction: `core list` looked broken; epoch millis unreadable.
Could recover: Yes. Result: **FRICTION** · Severity **Medium** · Type: CLI / Visual.
Evidence: `core list` → `EXIT=0`, empty stdout+stderr.

**HUMAN-21** — Persona B/C · CLI identifier conventions
Before: "the name in the list is the ref." · Action: `xrm sub show "Local fixture"`, `xrm sub update "Bad source"`
Actual: `订阅来源不存在` / exit 1 and exit 4; only the UUID works, while `profile show "Lab Home"` works with a name.
Reaction: the error claims the object does not exist although it is printed by `sub list`.
Recovery: copy the UUID. Could recover: Yes (after guessing). Result: **FRICTION** · Severity **High** · Type: CLI / Terminology.

**HUMAN-22** — Persona B/C · CLI error language
Before: "everything is English." · Action: any failing command
Actual: every error ends with `Diagnostic (original): 找不到对应Profile（请使用精确名称或UUID）` / `连接xrmd失败: …` / `核心不存在` / `订阅来源不存在`.
Reaction: needs translation; in a `| grep` pipeline the mixed script is noise.
Result: **FRICTION** · Severity **Medium** · Type: CLI / Terminology.

**HUMAN-23** — Persona B/C · CLI misleading classification
Before: "download failure will say so." · Action: `xrm sub update <bad-source-uuid>`
Actual: `Subscription update failed — Bad source` + `Reason: The requested object or a required reference was not found.` + `Recovery: Refresh the relevant list and use an existing name or ID.` — and the whole payload goes to **stdout** (stderr empty), exit 4.
Reaction: the reason/recovery are wrong for the failure, and stdout pollution breaks the pipeline contract.
Result: **FAIL** · Severity **High** · Type: CLI / Error recovery.

**HUMAN-24** — Persona B/C · CLI help gaps
Before: "`--cursor` / `--limit` have descriptions." · Action: `xrm logs --help`
Actual: `--cursor CURSOR   Default: 0.` and `--limit LIMIT   Default: 65536.` — description empty; no `--follow`, no `--tail`, no `--level`.
Result: **FRICTION** · Severity: Low · Type: CLI.

**HUMAN-25** — Persona A/B/C · `status`
Before: "`status` answers 'what will run'." · Action: `xrm status`
Actual: `saved target: scope is running Profile only; inspect \`profile show <ref>\`` — a pointer instead of an answer.
Result: **FRICTION** · Severity: Low · Type: CLI / Information hierarchy.

**HUMAN-26** — Persona A/B/C · Overview after Connect
Before: "after a successful Connect, no warning." · Action: `c`
Actual: running/PID/profile/target all correct, **but `! Saved target differs from the running target` is still shown**, with `Saved target none`.
Reaction: alarming and contradictory — I just connected.
Result: **FRICTION** · Severity **Medium** · Type: State model / Feedback.

**HUMAN-27** — Persona A/B/C · Overview statistics reason
Before: "`Unavailable reason` explains itself." · Action: read Overview
Actual: `Unavailable reason: stats, policy, metrics` — a bare list of module names.
Reaction: reads like "these are missing" rather than "these modules are disabled".
Result: **FRICTION** · Severity **Low** · Type: Terminology.

**HUMAN-28** — Persona A/B/C · Logs
Before: "scannable table." · Action: read logs
Actual: TIME column shows `-` for untimed lines, long messages wrap with a bare `|` whose continuation sits under the SOURCE column and looks like a new record; `core: Xray 26.9.9 started` is emitted at **Warning** level; TIME is `HH:MM:SS` here but `2026-09-27 02:44:20 UTC` in Details.
Reaction: hard to tell rows from continuations; spurious warnings for a healthy start.
Result: **FRICTION** · Severity **Medium** · Type: Visual / Information hierarchy.

**HUMAN-29** — Persona C · daemon disconnected, footer
Before: "the footer gives the recovery path." · Action: kill `xrmd`
Actual: `xrmd disconnected · cached data · Content` — cached/disconnected is **excellent**, but no recovery hint is shown (Help has `r Refresh`).
Result: **FRICTION** · Severity: Low · Type: Error recovery. (Everything else about this path passed.)

**HUMAN-30** — Persona B/C · CLI `config` vs TUI `Config`
Before: "Config configures Xray." · Action: open TUI `Config`
Actual: `Core Geodata Tests Appearance Editor General` — no native modules; native modules live in the TUI `Modules` page and in CLI `xrm config modules`, while TUI `Config` corresponds to CLI `core`/`geodata`/`settings`.
Result: **FRICTION** · Severity **Medium** · Type: Terminology / Information architecture.

**HUMAN-31** — Persona A/B · Node list keys
Before: "short letters map to obvious actions." · Action: F1 on Nodes content
Actual: `D Remove local node`, `x Enable/disable`, `i Import share URI`, `l Alias and labels`, `k Cancel active node tests`, `C Copy as local node`, `J Advanced node editor`, `t Test node`, `T Node test results`, `/ Filter`. Same letters mean different things on Modules (`i` = paths, `e` = Edit-mode-ish) and Profiles (`u` = Use; on Subscriptions `u` = Update).
Result: **FRICTION** · Severity **Medium** · Type: Discoverability / Terminology.

**HUMAN-32** — Persona A/B/C · collection row actions duplicated 3 ways
Before: "one way to add a DNS server." · Action: inspect the collection
Actual: `[View][Add][Edit][Copy][Delete][Down][Done]` buttons **and** hidden keys `Enter e a c D -`, while the module-level footer/action bar shows a different set.
Reaction: unsure which set is authoritative.
Result: **FRICTION** · Severity: Low · Type: Information hierarchy.

**PASS observations (explicitly not friction):** resize incl. a real `Terminal too small — 60x18 available; 80x24 required. Your draft and focus are preserved.` guard and draft/focus survival through 5 resizes; daemon-down `cached · disconnected` labelling and automatic reconnect; text-field isolation (`q e u c d p ?` typed into `Name` inserted literally, TUI stayed alive); `Home/End/Backspace/Delete` correct; visual→native-JSON round-trip (`{"address":"1.1.1.1"}` with omitted optionals); module `UNSAVED *` dirty indicator clearing after `s`; `Ctrl-S` submit; startup help text `Saving and selecting never connect.`; CLI `connect` error listing `Recovery` + `Next` + `Setup help` + `Select core help`; SIGPIPE (`PIPESTATUS=141`), `| cat`, `--json` all clean.

## 6. Wrong paths taken

- Logs → looked for Stop: absent; Help explicitly says `No core lifecycle actions here` ✅ handled.
- Profiles → looked for Connect: not there; found via Help on Overview (`c`) after 2 page hops — no in-page redirect ❌.
- Modules → looked for "enable edit": no path ❌ (HUMAN-03).
- Overview → looked for "add node": `Inventory 1 node · 1 source` is a read-only summary with no link ❌.
- Nodes → looked for "Profile Use": `Saved`/`Run` columns exist but the action is on Profiles; no redirect ❌.
- Config → looked for Xray Modules: 6 app-setting tabs, no pointer to the Modules page ❌.
- Expecting `Left/Right` on a tab bar, number-key page jumps, `?` for help (it typed a literal `?` — correct per contract, but I had no other guess).

## 7. Recovery behaviour

| Situation | Recovered? | How | Cost |
|---|---|---|---|
| Read-only module | Partially | leave page → Profiles → `e` → return | 2 page hops, memorised key |
| Unknown module-switch key | Partially | accidental `Tab` in SecondaryNav | ~15 failed keys |
| `Esc` reopens form | Partially | odd number of presses | loss of trust in `Esc` |
| Unsaved-draft navigation lock | No (in-session) | new TUI session | session state lost |
| Subscription download failure cause | No | not available anywhere in TUI | — |
| Collection row 2 | No | none | feature unusable |
| daemon down | Yes | automatic reconnect | zero |
| Link error paths | Yes | structured Error/Reason/Recovery/Next | zero |
| Too-small terminal | Yes | explicit guard message | zero |
| CLI missing arg / bad arg | Yes | usage + specific missing item | zero |

## 8. Terminal evidence (verbatim)

Read-only gating with no in-page explanation:
```
 dns  SOURCE · READ ONLY · Visual
 │ Modules · View Lab Home
 │ Actions · choose with arrows, Enter to execute
 │ > Enter           View entry · Inbound listeners
 │ Changes stay in the parent draft until its Save action.
```
Same page after Edit was enabled on *Profiles*: `log  SOURCE · EDIT · Visual`, badge `Draft`, footer `log · Source editable · Visual · included`.

Focus-level token that redefines `Tab`:
```
 xrmd ready · SecondaryNav · modules/07_metrics.json        F1 Help & actions
 Tab -> 9/15 region=[SecondaryNav] | stats
 Tab -> 10/15 region=[SecondaryNav] | policy
```
`Esc` alternating on a form: `Domain strategy` → `Doma│` → `Domain strategy` → `Doma│`.

40-column module strip at `15/15` (`‹`/`›` paging does exist):
```
 ‹  routing  observatory  api  metrics  stats  policy  fakedns  geodata  env  version  burstObservatory           15/15
```

Daemon-down (good):
```
 Cached · disconnected · 28 text lines
 xrmd disconnected · cached data · Content
```

Too-small guard (good):
```
            XRM · Terminal too small
            60x18 available; 80x24 required
            Your draft and focus are preserved.
```

## 9. CLI discovery transcript (actual)

```
xrm                                   → full grouped help, EXIT=0
xrm --help                            → identical, EXIT=0
xrm profile                           → subcommand help, EXIT=0
xrm profile list                      → "<uuid>\t<name>\t" ×2, EXIT=0
xrm profile create                    → "xrm profile create: --name is required" + usage, EXIT=106, stderr
xrm unknown                           → "The following argument was not expected: unknown" + full root help, EXIT=109, stderr
xrm profile show does-not-exist       → Error/Reason/Recovery/Next/Socket/Diagnostic, EXIT=1, stderr
xrm connect does-not-exist            → same shape + Setup help + Select core help, EXIT=1, stderr
xrm --socket <bad> status             → "endpoint is unavailable", EXIT=3, stderr
xrm logs                              → empty, EXIT=0  (empty buffer is silent)
xrm core list                         → empty, EXIT=0  (empty registry is silent)
xrm group list                        → "xrm group list: profile_id is required", EXIT=106
xrm sub show "Local fixture"          → 订阅来源不存在, EXIT=1
xrm sub show <uuid>                   → ok; "URL and headers: hidden in human output; use --json sub show for editing"
xrm sub update <bad>                  → reason/recovery = not-found template, payload on STDOUT, EXIT=4
xrm stats --watch | head -3           → PIPESTATUS=141 (SIGPIPE handled), "metrics: unavailable / sample: unavailable"
xrm --json profile list               → [{"core_id":"","id":"…","name":"…"},…] on stdout
xrm profile <TAB-less> discovery      → 2 levels max; found everything in ≤3 help calls
```
Help calls needed to find a feature: root → group → leaf = 2–3, all readable. CLI discoverability is good; **CLI output shape is not.**

## 10. Things I could not determine

1. Whether `Up/Down` failing to move the collection row cursor is focus (I was in the button row) or a defect — `BTab`+`Down` also failed, but I could not rule out a key-delivery quirk.
2. Whether the `Esc` open/close alternation is deterministic under hand input or an artefact.
3. Whether `Enter` on a Profile row is *intended* to navigate to Modules (contract says `Enter View 更新底部`, i.e. footer only).
4. Whether `! Saved target differs from the running target` is a bug or a deliberate unresolved warning.
5. Module-scope: Groups as Node `Scope` (contract says Nodes has 来源/Local/Groups); I never created/modified a Group, so "constrained-value vs free-text" (Task 6) is **untested**.
6. Subscription bind-to-Profile, remove-with-references, history cleanup: **untested**.
7. Form save failure / invalid JSON in F2 / single-file edit conflict: **untested** (the contract promises they preserve text+cursor).
8. CLI JSON error schema, `--json` on errors, non-TTY ANSI behaviour: only partially tested.
9. TUI `Appearance`/`Editor` settings pages: not opened.
10. Whether Edit being **session-scoped** (Edit Lab Home in session 1, `Edit none` in sessions 2–4) confuses users who reopen the TUI — observed, not evaluated with users.
11. IME/CJK input: not testable through tmux `send-keys` → **ENVIRONMENT LIMITATION**, not a PASS.

---

# Part B — Root causes (ranked)

**ROOT-01 — The focus level is invisible, but it redefines the primary navigation key.**
Affected: HUMAN-01, 02, 04, 05, 07, 14, 15 (Module switch, page switch, Enter, Esc).
Evidence: same `Tab` press produced `[Profiles]→[Modules]` at one moment and `dns→inbounds` at another; the only signal is a 10-character token (`TopNav`/`SecondaryNav`/`Content`) in the bottom-right, undefined in Help.
Why humans struggle: `Tab` is the one key every TUI user trusts. Making it mode-dependent without a visible mode costs the user their mental model; the failure is silent — the wrong thing changes and looks plausible.
Frequency: every session, every page with a SecondaryNav (Modules, Nodes, Config). Severity: **Blocker**.

**ROOT-02 — Capability discovery is a two-level F1 descent.**
Affected: HUMAN-03, 15, 16, 17, 31, 32; Tasks 1, 2, 4.
Evidence: Profiles TopNav help = `{Enter, q}`; Profiles Content help = 11 actions incl. New/Rename/Copy/Delete. Nodes/Config SecondaryNav help = 1 entry each.
Why humans struggle: the first Help press returns a near-empty menu, so users conclude the page is inert and stop. The richest information sits behind a key press they have no reason to try again.
Frequency: any first use of any page. Severity: **High**.

**ROOT-03 — Status is written as imperative action text.**
Affected: HUMAN-02, 09, 26, 27, 30.
Evidence: `View Test One · Use Lab Home · Edit none · Run none` while `▸ Test One`; `Saved target differs…` after a successful Connect; `Unavailable reason: stats, policy, metrics`.
Why humans struggle: "Use Lab Home" reads as "press this to use Lab Home". Users cannot separate *what the app resolved* from *what the app will do*.
Frequency: constant (every screen). Severity: **High**.

**ROOT-04 — Cross-page object ownership is not signposted.**
Affected: HUMAN-03, 30, 31 + all wrong-path tests except Logs.
Evidence: Edit gate lives on Profiles only; Connect only on Overview; Groups only as a Nodes scope; native modules not in Config; no link or hint on the page where the user looks.
Why humans struggle: the natural question "where do I enable X" has no in-page answer, so the user must guess a different page.
Frequency: high. Severity: **High**.

**ROOT-05 — Failure is reported without cause or next step in the TUI.**
Affected: HUMAN-19, 29.
Evidence: `partially completed; inspect source result` + Details panel with `last_result: failed` and no reason; and the CLI variant mislabels it as "object not found" while writing to stdout.
Why humans struggle: a failure the user cannot diagnose from the product is indistinguishable from a bug.
Frequency: any download/RPC failure. Severity: **High**.

**ROOT-06 — Modal editing affordances are inconsistent within one dialog.**
Affected: HUMAN-11, 12, 32, 17.
Evidence: `[View][Add][Edit][Copy][Delete][Down][Done]` + hidden keys `Enter/a/e/c/D/-`; cursor unmovable; move-down only; `EDIT` in an "Add" title; `Enter` submits at the bottom of a 15-field form.
Why humans struggle: three parallel interaction models for one object class means the user cannot build transferable muscle memory.
Frequency: every collection/editor. Severity: **High**.

**ROOT-07 — Unsaved drafts block navigation without saying so.**
Affected: HUMAN-07.
Evidence: 5×`Esc` and 6×`Tab` left region/content unchanged with an unsaved routing rule; a fresh TUI session was the only recovery.
Why humans struggle: no exit, no reason, no offered Save/Discard/Continue — the contract says a draft-carrying Edit change must offer Save/Discard/Continue.
Frequency: after any unsaved edit. Severity: **High**.

**ROOT-08 — Identifiers and list shapes are inconsistent across sibling commands.**
Affected: HUMAN-20, 21, 23, 25.
Evidence: Profiles accept names, subscriptions demand UUIDs; four list commands have three formats; `core list` is silent when empty; epoch millis in `sub list` but a readable timestamp in the TUI Details.
Why humans struggle: the ref that the list *shows* is not the ref the commands *accept*, and the error blames a missing object.
Frequency: every CLI scripting/search task. Severity: **Medium-High**.

**ROOT-09 — CLI human output leaks raw internals by default.**
Affected: HUMAN-22, 23, 24, 25.
Evidence: `Diagnostic (original): 订阅来源不存在`; `Profile core updated (checked_only)`; `profile_id is required`; empty option descriptions; `error:""`-style `"core_id":""` in JSON.
Why humans struggle: internal names, another language, and unlabelled status tokens force translation before the user can act.
Frequency: any error, several successes. Severity: **Medium**.

**ROOT-10 — Overlays do not clear the layer beneath.**
Affected: HUMAN-18, 28.
Evidence: log text and `> DNS` visible between dialog borders and in the margins; 40-column log lines wrapping into a bare `|` continuation under the SOURCE column; `core: Xray … started` at Warning.
Why humans struggle: the surface the user is reading is visually noisy and severity colouring misleads triage.
Frequency: every overlay on a content-dense page. Severity: **Medium**.

---

# Part C — Spec / implementation analysis

**Scope caveat:** I read only `docs/xrm-interaction-spec.md`. I did **not** read product source, RPC definitions or tests, so the classes below are behavioural/contract-based, not source-confirmed. `C`/`D` items are labelled *candidate*.

- **intentional tradeoff, but a real usability problem (B)** — `Tab/Shift-Tab 只切当前层`, `无数字、[] 或方向键跨主导航捷径`, TopNav/SecondaryNav/Content naming. HUMAN-01/05/14 are the price of this rule. The contract is self-consistent; the outcome is still a user who cannot predict `Tab` or jump to a page.
- **contract-induced (B)** — footer as status phrased with verbs (HUMAN-02/26); `Edit` as TUI-session state (HUMAN-03's "why did my edit mode vanish"); subscription result wording `partially completed` (HUMAN-19); raw Diagnostics reachable by default (HUMAN-22); CLI error text taking `命令/任务、对象、原因和恢复建议` from a shared template, which is why a download failure is labelled "not found" (HUMAN-23).
- **terminology flaw (A)** — `TopNav`/`SecondaryNav`/`Content` exposed to users; `EDIT` as the title of create forms; `EDIT` as both "module write mode" and "form open"; `enabled` (CLI) vs `included` (TUI) for the same state; CLI `config` vs TUI `Config`.
- **information-architecture flaw (A)** — the only always-visible action surface is a status line; every actionable list needs an extra descent to be explained; the Overview's `Inventory`/`Statistics` blocks are dead ends.
- **visual/component flaw (A)** — opaque overlay requirement (`整矩形不透明`) is not met in practice: background text shows around and between dialog borders.
- **candidate implementation bug (C)** — `Esc` alternating closed/open on a form against `Esc 只退一层` / `Esc 不穿透`; unsaved-draft navigation lock against `关闭或切换前必须处理草稿（保存、放弃、继续）`; collection row cursor not moving against `Up/Down Choose entry` + `无选中项…不可执行` and DNS `增改删/重排`; move-down without move-up against DNS `重排`; `[Confirm]`-focused destructive dialog while `危险操作` is guarded by `确认`; `失败保留旧缓存并标 stale/unavailable` is met for the daemon case but no *reason* is retained for a failed refresh; failed `sub update` payload on stdout against `错误 stderr、结果 stdout`; `Saved target differs…` after a successful Connect against `实际核心/目标、保存目标`.
- **candidate discoverability gap (D)** — module-strip key; per-level Help thinness; `p`/`End`-style bindings discoverable only inside Content Help.
- **contract violation of Help completeness (C, candidate)** — `必要前置条件与保存层级在 Help 说明区显示`: on Modules with `READ ONLY`, no Help level stated the Edit precondition.
- **environment limitation (F)** — CJK/IME input via tmux; and the fact that the isolated test environment could not start at all until I freed 13 GB of tmpfs.

---

# Part D — Highest-return fixes (ranked by friction removed per fix)

1. **Make the focus level visible and make `Tab` truthful.** Render the level as a labelled chip in the bottom bar (`Page navigation` / `Module selection` / `Fields`), and/or put the level in the Help header. Kills ROOT-01 for every page at once. *(Does not require new navigation semantics — only disclosing the existing ones.)*
2. **Make Help non-empty at every level.** At TopNav/SecondaryNav, list the actions that exist one level down, greyed with the reason (`Press Enter to open Modules / log`), and always state the current precondition (`Read only — enable module Edit on Profiles with e`). Kills ROOT-02 + the Modules read-only recovery gap.
3. **Split the footer into two labelled lines: `Watching:` and `Will run:`**, keeping the verbs out of it (`Watching Test One · Next connect Lab Home · Edit off · Not running`). Kills ROOT-03 including the post-Connect contradiction.
4. **Show the failure reason in the TUI.** One line in the toast (`failed: connection refused (127.0.0.1:9)`) plus a reason row in `F` Details and a `Retry` action. Kills ROOT-05.
5. **Fix the draft/navigation contract.** On `Esc`/page change with an unsaved draft, open the promised `Save / Discard / Continue` dialog instead of silently holding the user. Also fix the `Esc` alternation. Kills ROOT-07 + HUMAN-06.
6. **Make one collection interaction model work.** Row cursor reachable, Up and Down reorder actions, and either buttons or keys — not both. Kills ROOT-06 and unblocks DNS/routing editing.
7. **Unify list output.** Header row + stable column order for all four list commands, names first and UUIDs last, `Nothing to show` for empty lists, human timestamps in human mode, JSON only in `--json`. Kills ROOT-08/09 (half).
8. **Accept names wherever the list shows names** (`sub show/update`, `group`), and make the not-found error say "expected an ID from `sub list`, got a name". Kills HUMAN-21.
9. **Stop emitting the raw Chinese diagnostic by default**; move it behind `--verbose` / `Diagnostic (original)` only on request, and translate the reason/recovery templates so a download failure is not described as "not found". Kills HUMAN-22/23.
10. **Make overlays opaque and add list/log column discipline.** Fill the dialog rectangle with the background colour; give log continuations a proper hanging indent and a distinct marker; demote `core: … started` out of Warning. Kills ROOT-10.

---

# Part E — Human acceptance recommendation

## **Not ready for human G5**

Reasons, in order of weight:

1. **A first-time user cannot discover how to change modules.** 15 keys/gestures failed; Help documents no key. `dns`/`routing` are unreachable except by luck. This alone blocks a G5 session, because Tasks 2–4 depend on it.
2. **Create/rename/copy/delete exist only behind two F1 presses.** The first Help press returns a 2-item menu, which reads as "nothing here".
3. **`Esc` is not trustworthy** (observed open/close alternation) and an unsaved draft can lock navigation with no message and no offered Save/Discard.
4. **Read-only has no in-page recovery path**, contradicting the contract's requirement that preconditions appear in Help.
5. **The footer contradicts the rest of the screen** (stale object names; post-Connect "saved target differs"), so a user cannot trust the app's own statement of what it will do.
6. **A failed subscription refresh shows no cause anywhere in the TUI**, and the CLI variant mislabels it as "not found" on stdout.

What is already good and should not be regressed while fixing the above: daemon-down handling (`Cached · disconnected`, data preserved, automatic reconnect), the too-small-terminal guard with draft/focus preservation, text-field key isolation, the `UNSAVED *` dirty indicator, visual→native-JSON round-tripping with unknown-field preservation, draft→Source→Connect layering, the root/group/leaf CLI help with examples, and the CLI's `Error / Reason / Recovery / Next` error shape.

Two process notes for whoever runs the human G5: (a) the isolated lab could not start at all until 13 GB was freed from a 100 %-full `/tmp` tmpfs — that will hit a human tester too; (b) I could not verify CJK/IME input through a tmux PTY, so that must be marked SKIP rather than PASS.

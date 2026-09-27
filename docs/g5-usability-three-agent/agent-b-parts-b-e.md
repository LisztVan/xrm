<!-- 用户消息中粘贴的 Agent B Parts B–E；Part A 原稿另存于 agent-b-part-a-blackbox.md。以下为原文。 -->

# Part B — Root causes (by impact)

**ROOT-01 — Select/checkbox/input controls don't behave like their affordances** (Blocker)
- Affected: HUMAN-08, 09, 10, 12, 13, 32, 31; tasks 1, 3, 4, 6, 7.
- Evidence: `‹ ›` select ignores Left/Right; Up/Down only *offers* ("(choose)"), Enter silently commits — uncommitted choice dropped → validation error contradicting the visible screen. Enter also toggles checkboxes → stray Enter enabled `webhook (advanced JSON)` → blocking raw parse error. Unchecked optional inputs render like live inputs but swallow keystrokes (`OptionalValueControl::OnEvent` force-refocuses the checkbox and eats characters). End key dead in a sub-form input. 13–15 Tabs to reach form buttons; sub-collection buttons make the count unstable.
- Why humans struggle: three different meanings of Enter (commit select / toggle checkbox / newline) selected by invisible focus state; typed input can vanish without a trace.
- Frequency: every non-trivial form. Severity: Blocker (caused my only "lost work" experience and the longest task time).

**ROOT-02 — Silent no-ops and invisible state** (High)
- Affected: HUMAN-03, 15, 16, 19, 20, 34.
- Evidence: unbound keys ignored silently everywhere; collection entry selection has no persistent highlight (delete confirm says "entry 2", not the value); two different contexts both labeled "Content" — Ctrl-S works in one, silently not the other; module preview showed "No entries" after a successful apply (stale render); sub-tab indicator desynced from content.
- Why: users build mental models from feedback; silence + stale views teach wrong models.
- Frequency: constant. Severity: High.

**ROOT-03 — Contract-mandated navigation model is expensive to learn** (High, contract-driven)
- Affected: HUMAN-02, 04, 05, 06; all wrong-path rows.
- Evidence: TopNav→SecondaryNav→Content with Tab-only-in-level, Enter-descend, Esc-one-level is per spec; arrows deliberately disabled. Result: repeated over/undershoot even after I "knew" the model; module switching and quit (q only at TopNav) undiscoverable without Help.
- Why: depth varies per page; nothing on screen teaches levels; status-bar "TopNav/Content" is jargon.
- Frequency: every navigation. Severity: High (but partially intentional tradeoff — see Part C).

**ROOT-04 — Error/diagnostic quality** (High)
- Affected: HUMAN-11, 23, 22, 24, 29, 30.
- Evidence: raw `[json.exception.parse_error.101]…` truncated; stale validation messages survive the fix; Chinese diagnostics hardcoded (`rpc_client.cpp` "连接xrmd失败", `cli/print.cpp` "找不到对应Profile") leak into English UI and `--json` messages; subscription "Failed" has no reason anywhere; "Xray started" classified Warning; some errors exist only as 8s toasts.
- Frequency: every failure path. Severity: High.

**ROOT-05 — Identity display instead of names** (Medium)
- Affected: HUMAN-21, 22, 26, 27, 33.
- Evidence: routing target shown as `xrm.node.3aa6e88d-…`; CLI `profile list`/`status` lead with UUIDs; two identical "Loopback fixture" nodes indistinguishable; subscription URL masked in edit (and viewable nowhere) though it's not a credential.
- Severity: Medium.

**ROOT-06 — Missing guardrails on consequential actions** (Medium)
- Affected: HUMAN-25, 10.
- Evidence: Overview `d` stops the running core instantly (no confirm) while all deletes confirm; stray Enter toggling advanced checkboxes creates blocking errors in untouched fields. (Note: Cancel-button discard is guarded — corrected after source review.)
- Severity: Medium-High.

**ROOT-07 — Onboarding: the product doesn't explain its own state model** (Medium)
- Affected: HUMAN-01, 17, 18.
- Evidence: View/Use/Edit/Run only explained inside Profiles F1; read-only form's F1 omits the unlock path (module-level F1 has it — contract requires prerequisites in Help); help text truncated ("Native pat…"); "ConfirmConfirm"/"DiscardDiscard" rendering bugs.
- Severity: Medium.

---

# Part C — Spec/implementation analysis

| Finding | Class | Note |
|---|---|---|
| Select Enter-commit + silent drop (HUMAN-09) | C — implementation | `SelectControl` writes value only from committed radiobox entry; spec requires Select to be "可辨认" — the (choose) state is not self-explanatory |
| Swallowed input when unchecked (HUMAN-12) | C — implementation | `OptionalValueControl::OnEvent` eats characters; violates "可辨载体" intent |
| Enter toggles checkbox accidents (HUMAN-10) | B/C — FTXUI default, no guardrail | spec doesn't address |
| Raw exception + stale validation (HUMAN-11) | C — implementation | spec: errors in place + human-readable; revalidation missing on change |
| Stale "No entries" after apply (HUMAN-16) | C — implementation | render not invalidated |
| Two "Content" contexts, Ctrl-S dead at entry level (HUMAN-19) | C — implementation | spec: "保存使用…Ctrl-S" uniformly |
| Unbound keys silent (HUMAN-20) | B — interaction design flaw | contract has no feedback requirement for unbound keys |
| Level navigation cost (HUMAN-02/04/05/06) | B — contract-mandated | per spec verbatim; still real friction — needs on-screen teaching, not redesign |
| Chinese diagnostics (HUMAN-23/22) | C at origin, surfaced by contract | messages hardcoded in `rpc_client.cpp`/`cli/print.cpp`; contract's "原始诊断可达" faithfully surfaces them |
| Subscription failure without reason (HUMAN-24) | B — information architecture | details dialog lacks the error field |
| Stop without confirm (HUMAN-25) | B — intentional? contract silent | spec only says "Stop 只操作 Server 自己实例" — no confirm required; usability issue stands |
| Masked URL in edit (HUMAN-21) | C — over-applied masking | spec: 编辑/导出使用真实值 |
| UUID/internal-ID display (HUMAN-22/27) | C/B — name resolution missing in human output | |
| Read-only form help lacks unlock path (HUMAN-17) | C — implementation | spec: "必要前置条件…在 Help 说明区显示" |
| Help truncation, ConfirmConfirm (HUMAN-18) | C — implementation bugs | |
| TIME "-" in logs, toast durations, 'N' history (HUMAN-29/30) | E/B — per contract ("无时间不补采样时间") | adequate given 'N', but 'N' discoverability = D |
| Level-model state quartet unexplained (HUMAN-01) | D — discoverability within contract | model is mandated; onboarding is missing |
| Duplicate node rows (HUMAN-26) | B — design gap | no origin column |
| 80×24 tree debris (HUMAN-28) | C — implementation | spec: 80×24 minimum operable |
| End key dead in sub-form (HUMAN-32) | C — implementation | spec: Home/End belong to control |
| Input not cleared after collection add (HUMAN-13) | C — implementation | |
| IME Chinese | F — environment limitation | direct UTF-8 works |

---

# Part D — Highest-return fixes (max 10, ordered by friction eliminated)

1. **Make the Select commit itself**: write the value on arrow-move (or show "Enter to apply" inline). Kills the worst contradiction error (HUMAN-09) across every form in the product.
2. **Block or visibly disable untouched optional inputs**: grey the box + ignore-with-hint instead of swallowing keystrokes; put the input in Tab order only when enabled (HUMAN-12).
3. **Revalidate on change**: clear/refresh inline errors when the field value changes (HUMAN-11 stale errors).
4. **Invalidate module preview after sub-form apply**: the "No entries" stale render manufactured a data-loss scare (HUMAN-16/14).
5. **Add "press Enter to commit / Space toggles" hints to form footer or field hints**, and stop Enter from toggling advanced-JSON checkboxes when the user is clearly heading for buttons (HUMAN-10).
6. **Translate diagnostics at source** (rpc_client/cli messages) and add failure reason to subscription details + F dialog (HUMAN-23/24).
7. **Resolve names in all human output**: node names in routing list/status, core names in `profile list`; show subscription URL in edit/details (HUMAN-21/22/26/27).
8. **One keystroke feedback rule**: any unbound key → status hint "no action here — F1 for actions"; add selection highlight to collection entries (HUMAN-20/15).
9. **Confirm before Stop core** (or make it undoably easy to reconnect) (HUMAN-25).
10. **Teach the model on screen**: one-line "Tab pages · Enter into · Esc back" hint on first run + put the Edit-unlock path into the read-only form's F1 (HUMAN-01/04/17).

---

# Part E — Human acceptance recommendation

**Not ready for human G5.**

Reasons (all evidenced in Part A):
1. One **Blocker** (ROOT-01): the select-commit model makes a routine routing rule require insider knowledge and produces validation errors that contradict the visible screen; Enter has three invisible meanings; text can be silently swallowed.
2. Multiple **High** defects that a human G5 would hit within minutes: stale "No entries" after apply (perceived data loss), stale validation messages, silent Ctrl-S at entry level, read-only unlock path absent where needed.
3. The product's **recovery spine is genuinely strong** (disconnect handling, draft preservation, unsaved guards, CLI error structure, resize guard, notification history) — the gaps are concentrated in form controls and feedback consistency, not architecture.
4. Environment caveats: no real-IME test; one unexplained transient "粘贴的文本" anomaly; these don't change the recommendation.

Recommend fixing D-1 through D-5 (all form-control/feedback issues) before asking a human to repeat G5; those five eliminate the majority of observed friction without touching the contracted navigation model.

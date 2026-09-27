# Phase 2 — Spec/implementation classification

## Confirmed implementation defects / contract misses (C)
1. Module READ ONLY recovery disappears inside CollectionEditor Help. `Workspace::help()` uses only overlay `ContextHelp` when an overlay exists (`src/tui/shell/workspace.cpp` around 627), while the module prerequisite `Read only: in Profiles allow module Edit...` is added only in the non-overlay Content branch. Collection Help (`src/tui/components/widgets.cpp` around 894) knows readonly but emits only View/navigation and generic parent-draft text. Violates required prerequisites/disabled reasons.
2. Disconnected Help exposes writes as executable. `Workspace::actions()` (`src/tui/shell/actions.cpp`) gates by selection/View/Use but not `fresh_`; refresh has no user result. Violates unavailable action disabled reasons.
3. Subscription batch error is discarded from human recovery. `Workspace::poll()` around 484 reduces a succeeded batch with `failed[]` to `partially completed; inspect source result`; subscription summary API omits the failed diagnostic, while Details only prints `last_error` if present. The user can never inspect the promised reason.
4. Core registration error is misclassified. `human_message.hpp` maps every `core_check_failed` to candidate configuration correction, including validating `/bin/true` during `core.register`; screen points at saved config instead of invalid executable.
5. Routing target validation is a form-level submit error and does not focus/scroll to outboundTag/balancerTag. Contract requires adjacent recoverable field errors. Fields actually exist at positions 5/6 (`native_form.cpp` ~238); tester's claim that no control existed was incorrect, but validation recovery remains poor.
6. Offline startup Help has no daemon/socket/retry action. TopNav Help is generic and the only diagnostic exposes raw errno/socket. This conflicts with designed error states/next action.

## Interaction-contract/design usability costs (B)
1. Basic action discovery intentionally lives only in executable F1 Help. Profiles CRUD, empty Groups Add, Edit prerequisites and Connect are absent from the page. This satisfies the contract but repeatedly forces Help and creates wrong-page dead ends.
2. Edit only in Profiles and Connect only in Overview create cross-page dependencies. The contract defines this IA; missing cross-page wayfinding makes it costly.
3. Complete native forms show 15–18 outer fields plus composite controls. The contract favors complete labeled forms over sequential prompts, but common DNS/routing tasks have high traversal cost.
4. Original diagnostics are required to remain reachable; current CLI prints socket + mixed-language raw diagnostic by default rather than behind a detail/verbose path. Contract-compatible, poor hierarchy.
5. stdout consumer closure is intentionally treated as io_error/exit3 (`print.cpp` ~365 and execution evidence). It distinguishes daemon disconnect from EPIPE as required, but makes common `watch | head` noisy/non-success under pipefail.

## Discoverability gaps with existing capability (D)
1. Subscription URL is not lost or replaced by bullets. Secret Input stores the real value and Ctrl-R toggles visibility (`widgets.cpp` ~93); field-focused F1 adds `Ctrl-R Reveal / hide` (`~970`). Black-box tester did not focus URL then open Help. Product capability exists, but the edit field itself gives no visible reveal hint.
2. Routing outbound/balancer target controls existed; tester overshot them in the long form. Help named them, but no direct jump.
3. TTY CLI tables exist; black-box shell commands were non-TTY, so UUID-first TSV is the intentional pipe contract (`print.cpp` profile/node list branches), not human TTY UX.

## Tester misunderstandings / sufficient existing behavior (E)
- Ctrl-A and Ctrl-U are not promised; Help documents Home/End and standard control navigation. Their failure is not a product defect.
- DNS semantic validation is intentionally bounded; saving an address-like string to draft is not equivalent to successful core check.
- Collection row `>` is selection, not button focus. Actual focus uses styled background/inverse. After Add, button focus can persist and Tab cycles it; closing/reopening merely reset focus.
- Deleting the Profile copy did not leave View on a deleted object; View remained on the original similarly named Profile.
- Non-TTY UUID-first output preserves machine compatibility by contract.

## Environment limitations (F)
- Exact lab command omitted `--core`; docs reveal optional `--core /tmp/xrm-g1-xray-v26.9.9/xray`, but reading docs was prohibited in phase 1. Real Run/Logs could not safely be completed.
- tmux plain captures strip ANSI/inverse styling. Claims that focused buttons were visually indistinguishable are not valid product findings without ANSI/current terminal evidence.
- UTF-8 bytes were tested, not OS IME composition.
- Most CLI discovery commands were captured non-TTY; TTY human table quality was not black-box tested. Execution records/source confirm a different TTY presentation, but that is phase-2 evidence.
- No log entries meant 3–5 minute scroll/detail/error scanning could not be evaluated.

## Valid product-level observations not cancelled by compliance
- Warning `Saved target differs from running target` while stopped overstates an expected state difference.
- Select displays horizontal chevrons while the underlying Radiobox interaction is Up/Down; common Left/Right/Enter/Space attempts did nothing.
- Corrected field validation remains visible until next submit.
- Subscription failure diagnosis, offline recovery and disconnected action gating are major real defects.
- Empty logs has silent CLI success; disconnect while already stopped reports `Stopped running core`.
- Inline-only `config edit --text` is inefficient for large module JSON.

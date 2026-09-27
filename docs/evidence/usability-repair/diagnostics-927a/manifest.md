# UXR-07/08/09 diagnostic backend evidence

Baseline at start: Git HEAD `561dede3c2e8e9e38a39d07a52076c9f08e3eeb0`; shared `build/component-rewrite` binaries were from different code versions, so they were not used here. This work used the unique `build/usability-diagnostics-927a` directory. Final `xrmd` SHA256: `604fedf6ed6d72b752866c584c22884e53350bfb34759ffc55111b84cf07135f`.

The replay command was `python3 docs/evidence/usability-repair/diagnostics-927a/live_rpc.py`. It created its own temporary HOME, XDG directories, Unix socket and SQLite database; launched that `xrmd` plus a loopback HTTP fixture on an ephemeral port; invoked protocol-v1 IPC directly; then removed only its own temporary directory. The observed response excerpts are in `live_rpc.json`. No core was started, no existing daemon or production data was accessed.

Observed facts: invalid source URL is rejected before storing; `/bin/true` registration fails as an invalid Xray binary; config checking a Profile without a selected core fails as a missing core. A successful source refresh has one node. HTTP 503 persists `last_error` and `last_error_stage=http` while retaining that node and its last successful refresh; the reason survives a daemon restart. Partial input returns `first_issue` and persists a parse-stage cause. An HTML response fails without clearing the old node. A later clean refresh clears the previous error. Connection refusal produces a bounded, NUL-free, credential-free download error. An obsolete `expected_run_id` returns conflict without changing runtime status.

Focused checks after the final source edit:

| Executable | Result |
| --- | --- |
| `xrm_http_tests` | 111 assertions, 3 cases passed |
| `xrm_subscription_tests` | 301 assertions, 23 cases passed |
| `xrm_subscription_integration_tests` | 493 assertions, 14 cases passed |
| `xrm_human_message_tests` | 34 assertions, 7 cases passed |
| `xrm_contract_tests` | 90 assertions, 11 cases passed |
| `xrm_module_actions_tests` | 141 assertions, 6 cases passed |

The exact stdout is saved beside this manifest. `git diff --check` passed for the modified tracked backend and test files. The live replay does not exercise TUI presentation, CLI pipe behavior, or an invalid native configuration with a real core; those belong to the integration/terminal checks in the parent run.

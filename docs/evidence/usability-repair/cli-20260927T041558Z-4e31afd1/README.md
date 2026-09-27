# CLI UXR-12/13 evidence

This run used an isolated `xrmd` socket, SQLite database, HOME/XDG directories and loopback HTTP fixture created by `tests/cli/usability_repair_test.py`. It registered the fixed Xray v26.9.9 binary and did not start a proxy core or touch the user's daemon. The run traces preserve command arguments, stdout, stderr, and individual pipeline process exits. Test fixture names and URLs are synthetic.

## Version boundary

| Artifact | SHA-256 |
| --- | --- |
| Baseline `build/component-rewrite/xrm` | `2c8bb9dfc5e41d0e54eb9c15dacdca9d964daf98dd4565b65b95f2a91bb74df5` |
| Final `build/component-rewrite/xrm` | `22e80bb56943dea34114ea514103e0b5c529cd8aaad63a6f4f8e7eed5644ddb9` |
| Final `build/usability-diagnostics-927a/xrmd` | `604fedf6ed6d72b752866c584c22884e53350bfb34759ffc55111b84cf07135f` |
| Fixed Xray v26.9.9 | `c4ae6798c38e0e5343b192406746333cd0ba7ff3eb984f8c4b9939dcb68c3f8a` |

The original reports' watch producer exit values 0/141/3 came from different contexts. In this workspace's baseline, `stats --watch | head -n 1` made the producer exit **3** with an `io_error` diagnostic. The final implementation returns **0** on confirmed stdout EPIPE only. IPC disconnect and other stdout write errors remain **3**. See `baseline.json` and `uxr-final3/trace.json` for observed streams and process codes.

## Reproduction

```sh
python3 tests/cli/usability_repair_test.py \
  --xrm build/component-rewrite/xrm \
  --xrmd build/usability-diagnostics-927a/xrmd \
  --xray /tmp/xrm-g1-xray-v26.9.9/xray \
  --evidence docs/evidence/usability-repair/cli-20260927T041558Z-4e31afd1/uxr-final3
python3 tests/cli/cli_rewrite_test.py \
  --xrm build/component-rewrite/xrm \
  --xrmd build/usability-diagnostics-927a/xrmd \
  --xray /tmp/xrm-g1-xray-v26.9.9/xray \
  --evidence docs/evidence/usability-repair/cli-20260927T041558Z-4e31afd1/cli-regression-final.json
```

Observed: **66** isolated CLI observations and **277** existing R10/R11 CLI scenarios passed. `python3 -m py_compile tests/cli/usability_repair_test.py tests/cli/cli_rewrite_test.py` and `git diff --check -- src/cli tests/cli docs/usage.md` passed.

## Original issue disposition

| Original report | Result and evidence |
| --- | --- |
| A final HUMAN-12/14/15; C HUMAN-20/21/24/25/26/30/31 | CLI human help/error/receipt tightened; exact subscription name or ID works, duplicate names give candidate IDs, and `config edit --file PATH/-` reads a complete validated document before RPC. TTY and non-TTY outputs, `--json`, Source preservation, and bad-input rollback are in `uxr-final3/trace.json`. Existing `--text` remains valid. |
| A final HUMAN-13; C HUMAN-23 | Watch stdout consumer close, normal `cat`, SIGINT, daemon disconnect, and `/dev/full` distinguished with separate stdout/stderr/process codes. Both `pipefail` settings and human/JSON watch tested in `uxr-final3/trace.json`. |
| B HUMAN-22/23 and related status/help observations | TTY list/show/status/log help and non-TTY machine columns were checked; empty logs uses a TTY-only message. The R10/R11 output compatibility suite is `cli-regression-final.json`. |
| Subscription partial/all-failed diagnostics | Human success output stays on stdout; failure and persisted stage/cause go to stderr. Partial parse issue is shown without URI credentials. JSON aggregate result stays on its compatible stream with exit 4; old nodes remain after failed refresh. |

The conservative `core use` and `disconnect` receipts describe observed state without claiming a transition when the server provides no changed/no-op fact. Showing a literal “already stopped” would require an authoritative server result; this task did not invent one. G5 acceptance remains with the user.

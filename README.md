# XRM

XRM is a C++23 Xray-only terminal manager. `xrmd` owns configuration and the
core process; `xrm` provides the CLI and TUI. See [requirements](docs/requirements.md),
[architecture](docs/architecture.md), and the [interaction contract](docs/xrm-interaction-spec.md).

## Repository layout

| Path | Responsibility |
| --- | --- |
| `apps/` | The two executable entry points |
| `src/model`, `src/contract`, `src/profile`, `src/subscription`, `src/geodata`, `src/xray` | Data contracts and domain logic |
| `src/infra`, `src/server` | Persistence, I/O, daemon and owned core lifecycle |
| `src/client`, `src/cli`, `src/tui` | RPC client and terminal frontends |
| `cmake/` | Options, dependency acquisition and target graph |
| `tests/` | Contract, unit, integration and terminal tests |
| `presets/`, `packaging/`, `scripts/` | Native configuration presets, deployment and isolated labs |
| `docs/` | Product specifications, build instructions, evidence and historical inputs |
| `.deps/`, `out/` | Ignored dependency cache and generated build outputs |

Build commands and the target compilation graph are in [docs/build.md](docs/build.md).

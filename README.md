# XRM

**A terminal manager for Xray-core on Linux.** XRM keeps subscription and
manual nodes in an independent inventory. Each Profile holds saved settings,
bindings, and Xray-native module files. A separate Xray process starts when you
explicitly connect. The CLI and seven-page TUI show saved choices alongside
what is actually running.

XRM is an active development project written in C++23 and built from source.
It consists of `xrm` (the CLI and TUI) and `xrmd` (the local server that owns
the core process).
[Xray-core](https://github.com/XTLS/Xray-core) is a separate executable.
The source is available on [Codeberg](https://codeberg.org/lisztvan/xrm) and
[GitHub](https://github.com/LisztVan/xrm).
Detailed first-use instructions are also available in the
[Chinese user guide](docs/usage.md).

## What it does

- Imports subscription and manual nodes into a shared library. Refreshing a
  subscription updates inventory without rebuilding or restarting the core.
- Manages all 15 supported native Xray top-level modules per Profile. The TUI
  offers structured Visual forms and a JSON editor over the same draft; advanced
  fields remain available in native JSON.
- Saves node or group choices separately from the active run. Connect builds a
  fresh multi-file Xray `confdir` from saved settings and checks it before
  replacing the owned core process.
- Provides contextual TUI Help, human-readable CLI output, `--json` for scripts,
  and status, logs, node tests, and available core statistics.

## Build and run

You need Linux, CMake **3.28+**, Ninja, a C++23 compiler, and development
packages for SQLite3, libcurl, OpenSSL, and zlib. On the first configure,
CMake downloads the remaining pinned source dependencies into `.deps/sources/`.
The build does not download or start Xray-core.

```sh
git clone https://github.com/LisztVan/xrm.git
cd xrm
cmake --preset dev
cmake --build --preset dev -j4 --target xrm xrmd
```

Run the server in one terminal:

```sh
./out/dev/xrmd --foreground
```

Open the TUI in another terminal:

```sh
./out/dev/xrm tui
```

The TUI opens even if you have not created a Profile or registered a core yet.
It uses the same per-user Unix socket as `xrmd` by default. For an isolated
socket and database, see the [user guide](docs/usage.md).

To make a first connection in the TUI:

1. In **Config → Core**, register an existing Xray binary or explicitly install
   a release.
2. Add a subscription in **Subscriptions** or a manual node in **Nodes**.
3. Create a **Profile**, set it as **Use**, choose its core and any subscription
   source bindings, and inspect its native configuration in **Modules**.
4. Save a node or group as the target, then explicitly **Connect**.

`Tab` and `Shift-Tab` move within the current navigation layer; `Enter` enters
the next layer or views the focused object, and `Esc` goes back. Press `F1` for
actions available in the current context. **View** is the Profile being
inspected; **Use** chooses a Profile for the next connection; **Edit** allows
changes to that Profile's modules. These TUI session modes do not change the
running core by themselves.

The CLI provides explicit commands for saved settings and runtime state:

```sh
./out/dev/xrm --help
./out/dev/xrm server info
./out/dev/xrm core list
./out/dev/xrm profile list
./out/dev/xrm status
./out/dev/xrm --json status
```

See the [user guide](docs/usage.md) for a complete CLI connection workflow,
core registration, native module editing, node tests, and optional systemd
deployment.

## Saved state and running state

```text
subscription / manual nodes ──> node library (SQLite)
Profile settings + native modules + bindings ──> saved Source
                                              └─ explicit Connect / Apply
                                                 └─ fresh Xray confdir ──> Xray-core
```

| Action | Effect |
| --- | --- |
| Refresh a subscription | Updates node inventory and source status only |
| Save a module or select a target | Updates saved settings; the active run stays as it is |
| Preview or check a Profile | Builds separate preview files; does not start the core |
| Connect or apply | Uses saved settings to prepare and start a checked run |
| Exit the TUI | Leaves the server-owned core running |

The server controls only Xray processes it starts. Runtime files are generated
results, not a second editable copy of Profile Source. Status distinguishes the
saved target from the target used by the active run. **Source** is saved Profile
and module configuration; **Runtime** is generated for a specific run. See the
[architecture](docs/architecture.md) for the storage and process boundaries.

## Develop and test

```sh
cmake --build --preset dev -j4
ctest --preset dev -j4
```

Real-Xray integration tests require a separately supplied, fixed Xray binary;
without it, conditional tests report skips. The [build guide](docs/build.md)
explains the offline, release, sanitizer, and real-core presets, plus formatting
and static analysis.

| Path | Responsibility |
| --- | --- |
| `apps/` | `xrm` and `xrmd` entry points |
| `src/model`, `src/contract`, `src/profile` | Domain data, RPC contract, saved Profiles and native modules |
| `src/subscription`, `src/geodata`, `src/xray` | Inventory refresh, assets, runtime assembly and core lifecycle |
| `src/infra`, `src/server`, `src/client` | Storage, I/O, daemon services and RPC client |
| `src/cli`, `src/tui` | Independent terminal frontends |
| `cmake/`, `CMakePresets.json` | Dependency and build configuration |
| `presets/`, `packaging/`, `scripts/` | Native presets, deployment files and isolated labs |
| `tests/` | Contract, unit, integration, CLI and terminal tests |
| `docs/` | Usage, design contracts, architecture and build instructions |

The detailed product documents are in Chinese: read the
[requirements](docs/requirements.md) and
[interaction contract](docs/xrm-interaction-spec.md). The
[module form coverage](docs/module-form-coverage.md) identifies which native
fields have Visual controls and which use JSON.

## Help and feedback

For a reproducible problem, open an issue on
[GitHub](https://github.com/LisztVan/xrm/issues) or
[Codeberg](https://codeberg.org/lisztvan/xrm/issues). Include the commit or
build identifier, the command or TUI steps, and the observed error; remove
credentials from any logs or configuration excerpts before sharing them.

## License

XRM is licensed under the [MIT License](LICENSE). Bundled third-party skills
retain their own copyright notices and license files.

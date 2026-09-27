# Building XRM

The build uses CMake 3.28 or newer, Ninja, a C++23 compiler, SQLite3, CURL,
OpenSSL and zlib. FetchContent keeps libarchive, libuv, nlohmann/json, CLI11,
FTXUI and Catch2 **source trees** in `.deps/sources/`; every preset compiles its
own objects in `out/<preset>/`. The dependency cache is independent of build
outputs, so removing `out/` does not require another download.

## Build order

`CMakeLists.txt` loads `cmake/Options.cmake`, `cmake/Dependencies.cmake`, and
`cmake/Targets.cmake`, then registers tests. The target links express this
dependency order; Ninja compiles independent branches concurrently.

| Stage | Targets | Depends on |
| --- | --- | --- |
| Dependencies | libarchive, libuv, nlohmann/json, CLI11, FTXUI, Catch2 when testing | System libraries and `.deps/sources/` |
| Domain | `xrm_model`, then `xrm_contract` and `xrm_logic` | nlohmann/json |
| Backends | `xrm_server_lib`, `xrm_client_lib` | Domain targets and I/O libraries |
| Terminal UI | `xrm_tui_components`, `xrm_tui_forms`, `xrm_tui_frontend`, `xrm_cli_frontend` | FTXUI, domain and client targets |
| Programs | `xrmd`, `xrm` | Server or frontend targets respectively |
| Tests | `xrm_fake_core`, `xrm_fake_editor`, contract/unit/integration/TUI binaries | Relevant production targets and fixtures |

For a first checkout, let CMake populate `.deps/sources/`:

```sh
cmake --preset dev
cmake --build --preset dev -j4 --target xrm xrmd
cmake --build --preset dev -j4
ctest --preset dev -j4
```

When the cached dependency sources already exist, `dev-offline` uses them
without network access and shares `out/dev` with `dev`:

```sh
cmake --preset dev-offline
cmake --build --preset dev-offline -j4
ctest --preset dev-offline -j4
```

`release` builds the two programs without test targets in `out/release`.
`asan` builds a separate sanitizer tree in `out/asan`.

The optional `real-core` preset requires the pinned executable at
`.deps/tools/xray-26.9.9/xray`. It builds in `out/real-core` and passes that
binary to the real-Xray tests:

```sh
cmake --preset real-core
cmake --build --preset real-core -j4
ctest --preset real-core -j4
```

Without that pinned core, use `dev`/`dev-offline`; tests marked as conditional
real-core tests report their configured skip rather than proving Xray behavior.
For another fixed core path, set `XRM_TEST_XRAY` to its absolute executable
path at configure time and for CTest.

The integration suites are organized by purpose: `bootstrap`, `inventory`,
`codecs`, `workflows`, and `real_core` replace the old milestone directory
names. Existing test case names and executable target names are retained.

The clean build and test record for this reorganization is in
[build-reorg evidence](evidence/build-reorg/README.md). Its generated `out/`
trees were removed after verification; the reusable `.deps/` cache remains.

## C++ formatting and static analysis

Project C++ uses the LLVM style in [`.clang-format`](../.clang-format). The
commands below assume `clang-format` and `clang-tidy` are on `PATH`; this setup
was checked with LLVM 21.
Format only the project's own files, never the fetched dependency sources:

```sh
rg --files -0 apps src tests -g '*.cpp' -g '*.hpp' |
  xargs -0 clang-format -i --style=file
```

Run `clang-tidy` with [`.clang-tidy`](../.clang-tidy) and the generated compile
commands. With GCC 16, CMake adds GCC-only dependency-scanner flags to the
database; the helper removes only those flags from a separate analysis copy:

```sh
cmake --preset dev-offline
python3 scripts/clang-tidy-compdb.py out/dev/compile_commands.json out/dev/tidy
clang-tidy -p out/dev/tidy src/profile/module_actions.cpp
rg --files -0 apps src -g '*.cpp' |
  xargs -0 -P 4 -n 1 clang-tidy -p out/dev/tidy --quiet
```

The compile database and analysis copy live under ignored `out/`; they are
regenerated after a clean build or a changed CMake configuration. Tidy findings
still need review against XRM's behavior and data boundaries before applying a
fix.

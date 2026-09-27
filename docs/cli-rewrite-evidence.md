# CLI 组件重写配套验收记录

日期：2026-09-26。R10/R11 为工程检查；G5 仍待用户人工复验。

## 实现与边界

- `src/cli/help.hpp` 使用 CLI11 实际注册树的参数、说明、子命令与分组；编辑性示例只是补充。根与裸分组帮助输出 stdout、退出 0、完全离线。错误给最深已解析层的帮助并保留 CLI11 退出码；`--json` 错误只给 stderr 单行原 schema。
- 关闭相邻子命令自动回退，`config show Home stats` 正确指向原生 stats 模块。根级 connect/disconnect 等既有入口保留。根 `--json` 必须放命令前，避免与既有叶级 JSON 输入选项混淆。
- TTY 列表增加表头和空提示；非 TTY 继续既有完整 ID 与空列表管道行为。组、设置和 Geodata 的普通结果改为领域摘要与保存回执；明确请求的原生配置预览/导出保留真实数据。JSON 成功与错误 schema 未改。
- watch 每条立即 flush；仅在当前线程的 RPC/stdout 写出期间暂时阻塞并消费新 SIGPIPE，让实际 I/O 失败返回 3；不安装进程级 SIG_IGN。真实回归先观察到 daemon 关闭时 exit -13 无错误，再修正并验证 daemon 断开与消费者关闭是两种明确错误。
- 旧根级“请选择 … 命令”fallback 删除；没有另一套手写命令目录。已有工作区 CLI 修改保持。

## 参考及采用内容

锁定源码：`build/dev/_deps/cli11-src/include/CLI/Version.hpp` 确认为 2.5.0，实际核对 App::help、get_subcommands、subcommand_fallthrough、Option::get_name、Formatter 的源实现。

[CLI11 子命令](https://cliutils.github.io/CLI11/book-subcommands.html)、[帮助格式](https://cliutils.github.io/CLI11/book-formatting.html)用于树与 formatter；在线文档版本更新，最终只调用锁定 2.5.0 API。[clig.dev](https://clig.dev/)用于本地帮助、stdout/stderr、非交互原则；[GitHub repo 帮助](https://cli.github.com/manual/gh_repo)与[Docker container 帮助](https://docs.docker.com/reference/cli/docker/container/)用于当前分组、简明任务描述和下一级发现。没有新增命令框架、网络依赖或安装 GitHub/Docker。

## 实际验证

构建：独立 `build/cli-rewrite`，源缓存来自 `build/dev/_deps`，`cmake --build build/cli-rewrite --target xrm xrmd -j 4`。

```sh
python3 tests/cli/cli_rewrite_test.py \
  --xrm build/cli-rewrite/xrm --xrmd build/cli-rewrite/xrmd \
  --xray /tmp/xrm-g1-xray-v26.9.9/xray \
  --evidence build/cli-rewrite/evidence.json
```

测试使用临时 HOME/XDG、socket、数据库与配置目录，真实 xrmd，PTY 表格捕获，无生产地址。递归发现 108 个实际帮助路径，并把所有非 TUI 示例在不存在的 socket 上运行以确认解析合法、无写副作用。覆盖人读/JSON 空结果、列表/详情、创建/选择/模块保存、失败、真实核心连接/停止、watch flush/Ctrl-C/daemon 断开/消费者关闭。测试结束只回收本次进程与临时目录。

结果：250 个实际捕获场景通过。这是覆盖记录数量，不是 TUI 可用性或 G5 的替代证据。完整 stdout/stderr/退出码存于 `build/cli-rewrite/evidence.json`。可重复的测试脚本在仓库；集成测试名 `cli_rewrite_r10_r11`。

真实核心：Xray 26.9.9，commit 52a412d，SHA256 `c4ae6798c38e0e5343b192406746333cd0ba7ff3eb984f8c4b9939dcb68c3f8a`。成功连接配置无入站监听、无 TUN、无远端请求；此处证明真实核心进程启动/停止，不声称网络流量已验证。

## 实际输出样本

### `xrm `

Exit: 0

stdout:
```text
Manage Xray nodes, native configurations and the independent core

Usage: xrm [OPTIONS] [SUBCOMMAND]


OPTIONS:
  -h,   --help            Show this help message 
        --socket TEXT [/tmp/xrm-cli-rewrite-i493xgq2/runtime/xrm/xrmd.sock]  
                          xrmd Unix domain socket 
        --json            Write machine readable output 

XRM settings and server:
  server                  Server operations 
  settings                Manage XRM appearance and Manager editor preferences 

Inventory and resources:
  core                    Register and inspect Xray binaries 
  node                    Manage the independent node inventory 
  sub                     Manage subscription sources and refresh node inventory 
  geodata                 Manage shared GeoIP/GeoSite resources 

Saved configuration:
  profile                 Manage saved Profiles 
  config                  Inspect and edit native Xray modules 
  group                   Manage saved dynamic Xray balancers 
  select                  Persist a node selection without connecting 
  routing                 Profile routing preset operations 

Run and observe:
  connect                 Apply saved Profile settings and connect 
  disconnect              Stop the managed Xray process 
  status                  Show expected/actual core runtime state 
  logs                    Read the bounded server/core log buffer 
  operation               Inspect an asynchronous operation 
  stats                   Show server-sampled Xray traffic and observations 

Interactive:
  tui                     Open the seven-page interactive terminal application 

Examples:
  xrm tui
xrm profile list
xrm profile create --name Home
xrm config modules Home
xrm connect Home

Next: xrm <command> --help
Help is local and never connects to xrmd.
Replace Home and *_ID with your saved object names/IDs.
Profile arguments accept exact names or UUIDs. Saves do not connect.
Machine output: xrm --json <command>; errors use stderr.
Place global --json before the command when a leaf accepts JSON input.
```

stderr:
```text
(empty)
```

### `xrm server`

Exit: 0

stdout:
```text
Server operations

Usage: xrm server [OPTIONS] [SUBCOMMAND]


OPTIONS:
  -h,   --help            Show this help message 

SUBCOMMANDS:
  info                    Show the connected server version 

GLOBAL OPTIONS (before the command):
        --socket TEXT [/tmp/xrm-cli-rewrite-i493xgq2/runtime/xrm/xrmd.sock]  
                          xrmd Unix domain socket 
        --json            Write machine readable output 

Examples:
  xrm server info

Next: xrm server <command> --help
Replace Home and *_ID with your saved object names/IDs.
Profile arguments accept exact names or UUIDs. Saves do not connect.
Machine output: xrm --json <command>; errors use stderr.
Place global --json before the command when a leaf accepts JSON input.
```

stderr:
```text
(empty)
```

### `xrm profile create`

Exit: 106

stdout:
```text
(empty)
```

stderr:
```text
xrm profile create: --name is required

Create a local Profile from the built-in preset

Usage: xrm profile create [OPTIONS]


OPTIONS:
  -h,   --help            Show this help message 
        --name TEXT REQUIRED 
                          Human-readable object name 
        --core TEXT       Registered core ID (may be selected later) 

GLOBAL OPTIONS (before the command):
        --socket TEXT [/tmp/xrm-cli-rewrite-i493xgq2/runtime/xrm/xrmd.sock]  
                          xrmd Unix domain socket 
        --json            Write machine readable output 

Examples:
  xrm profile create --name Home

Replace Home and *_ID with your saved object names/IDs.
Profile arguments accept exact names or UUIDs. Saves do not connect.
Machine output: xrm --json <command>; errors use stderr.
Place global --json before the command when a leaf accepts JSON input.
```

### `xrm --json profile create`

Exit: 106

stdout:
```text
(empty)
```

stderr:
```text
{"error":{"code":"invalid_argument","message":"--name is required"}}
```

### `xrm profile create --name Home`

Exit: 0

stdout:
```text
Created Profile Home [677fd128-055a-4a6f-8904-b99b5125cc18]; no core started
```

stderr:
```text
(empty)
```

### `xrm connect Home`

Exit: 0

stdout:
```text
Connected Profile [677fd128-055a-4a6f-8904-b99b5125cc18]; process running; traffic unverified
```

stderr:
```text
(empty)
```

### `xrm disconnect`

Exit: 0

stdout:
```text
Stopped running core; saved Profile settings unchanged
```

stderr:
```text
(empty)
```

### watch flush + Ctrl-C

Exit: 0

```text
stdout:
{"availability":"unavailable","missing_conditions":["core not running"],"rates":[],"received_at":null,"run_id":null,"sample":null}
{"availability":"unavailable","missing_conditions":["core not running"],"rates":[],"received_at":null,"run_id":null,"sample":null}
stderr:
(empty)
```

### watch stdout consumer closed

Exit: 3

```text
stdout:
{"availability":"unavailable","missing_conditions":["core not running"],"rates":[],"received_at":null,"run_id":null,"sample":null}
stderr:
{"error":{"code":"io_error","message":"Cannot write stats output; stdout consumer closed or write failed"}}
```

### watch daemon disconnected

Exit: 3

```text
stdout:
{"availability":"unavailable","missing_conditions":["core not running"],"rates":[],"received_at":null,"run_id":null,"sample":null}
stderr:
{"error":{"code":"io_error","message":"发送IPC请求失败: broken pipe"}}
```

## 集成验证

主集成 `build/component-rewrite/xrm` / `xrmd` 已运行同一脚本和固定真实核心，通过 250 个捕获场景（包括离线 `--version` 输出 `xrm 0.1.0`）。最后当前产物再次通过同一脚本；最终捕获保存于 [CLI 原始输出](evidence/component-rewrite/cli-output.json)。所有 TUI 界面验收仍单独记录，不由此代签。

独立产物 xrm SHA256：`9623a844ece14b2871b8d2edeb6c40823f13ae59ae5f31d30b5f6954a346c4d8`。

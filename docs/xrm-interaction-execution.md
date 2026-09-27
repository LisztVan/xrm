# XRM 交互重设计执行与验收记录

> 下文为上一轮历史结果，不能证明当前组件重写已经通过。当前执行见 [组件重写记录](component-ui-rewrite-execution.md)，有效语义只见 [交互契约](xrm-interaction-spec.md)。

本记录针对 `561dede3c2e8e9e38a39d07a52076c9f08e3eeb0` 工作树。[旧审计文件](history/reviews/xrm-interaction-audit.md) 保留为旧基线；交互规则以 [xrm-interaction-spec.md](xrm-interaction-spec.md) 为准。以下“通过”仅指列出的自动检查和隔离终端操作，G5 用户人工验收仍待执行。

## 工具、来源和生效状态

| 项目 | 固定来源和实际位置 | 已核实 | 限制/回退 |
|---|---|---|---|
| 终端设计 Skill | `pageton/tui-design-skill` commit `ec54092ea3a47fb3aafe3abc3363bc5e911d1a14`；`.agents/skills/tui-design/` 含 SKILL、引用和 LICENSE | 已读取正文、交互/状态/布局/兼容/测试引用；当前宿主的可用 Skill 列表已发现 `tui-design`，本轮按其终端原则检查布局与真实 PTY | 不采用其其他语言框架示例；删除该项目目录即可回退 |
| 项目 Skill | `.agents/skills/xrm-interaction-design/SKILL.md`；`AGENTS.md` 仅添加触发与唯一契约引用 | 已读取正文；当前宿主已发现 `xrm-interaction-design`，本轮修改前实际加载 | 官方 `skill-creator` 验证脚本缺少 PyYAML，未声称其通过；frontmatter/正文已人工核对 |
| 终端 MCP | `bnomei/tmux-mcp` v0.6.0 / commit `c4762ea36b1a288a95ba5cc125154fae65a60ad5`；`.agents/bin/tmux-mcp-rs` 和原项目许可证 `.agents/bin/tmux-mcp.LICENSE` | 发行包 SHA-256 `6d2042b74bb1f35ef6e274a15d72a99a621fcd178c26367ed076dfd92dba786d`，二进制 SHA-256 `8349453d919665a9923f2673231f4d46de25f7495b3c4640d3306c9c4e84b031`；`--version` 为 0.6.0；直接 stdio JSON-RPC 已调用 initialize、tools/list、list/create-session、list-panes、send-keys、capture-pane、resize-pane 等；隔离真实操作见下 | 项目配置已写 `.codex/config.toml`、`.codex/tmux-mcp.toml`，仅允许 `/tmp/xrm-interaction-tmux-mcp.sock` 和 `xrm-interaction` 会话。**当前 Agent 工具列表尚未重新加载这个 MCP**，所以宿主原生发现待重启/重新信任项目后验证；本轮用该二进制直接走 MCP 协议完成操作。删除这两个项目配置和二进制即可回退，不改用户配置 |
| CLI 参考 | [clig.dev](https://clig.dev/) | 用于人读/机器输出、错误、管道与非交互设计；实际命令和退出码以 XRM 合同为准 | 无额外运行依赖 |

本轮未运行远程安装脚本；没有安装新的产品运行依赖，也未改安全审批策略。`CMakeLists.txt` 仍固定 FTXUI v7.0.1、CLI11 v2.5.0，并复用当前项目的源码缓存。初始 `HEAD` 与审计基线一致；当时仅有六份用户未跟踪文档，均未覆盖或清理。构建在 `build/interaction-baseline/`，没有拿 `build/dev/` 的旧程序代替当前工作树。

## 实现与旧问题追踪

| 锚点 | 新行为和位置 | 主要独立证据 |
|---|---|---|
| I-01 | `src/tui/app.cpp` 的当前页动作表同时产生执行与帮助；删除跨页通用 c/d。Logs 中 c/d 只保留日志视图 | MCP 当前格 Logs 仅提示 p/r；c/d 前后 `runtime.status` 的 `state=running`、PID、active 对象一致；TUI 焦点/生命周期用例 |
| I-02 | `src/cli/print.cpp` 对 Profile 创建、选择、连接、停止等输出已确认回执；错误继续 stderr/非零 | 隔离 CLI stdout/stderr/码样本；M1 `[CLI-UX]` |
| I-03 | TUI 的库存加载状态与 daemon 可达性分开；离线未知、旧缓存过期，r 手动重试，不进入假首用 | `[UX-INT]` PTY 断开/恢复用例；当前格和焦点断言 |
| I-04 | Subscriptions 进入真实编辑、暂停/恢复和带多 Profile 引用预览的移除流程；来源自身保存，不发布运行文件 | `[UX-INT]` PTY；原本地 HTTP fixture 完整链和失败更新用例 |
| I-05 | Profile `e` 打开绑定 ID 的名称、核心、默认补充选项草稿；`profile.get` 仅显式 opt-in 从同一次文件读取返回设置与源正文，`profile.save` 带 expected_text 检查单文件冲突；View/Use/Edit 不隐式连接 | `[UX-INT]` 编辑、外部改动冲突与 RPC 源核对；原 UI-07 分离用例 |
| I-06 | Nodes 明示 Cursor、Saved 和 Running；Nodes `c` 命名 Save & Connect，失败后核实保存结果 | TUI 节点/Overview 用例；MCP 与 CLI 的保存/运行事实 |
| I-07 | 80×24 保留完整 Ctrl-S；Modules `i`、Overview `f` 打开完整文件/运行路径；同一 TUI 进程 resize 留稿 | MCP 当前格、ANSI 与尺寸；`[UX-INT]` resize 用例 |
| I-08 | 普通 `status` 在无活动 Profile 时声明 saved target 的作用域，Profile 详情可查保存目标；机器 JSON 字段/类型未改 | 隔离 CLI 连接前/后样本；M1 `[CLI-UX]` |
| I-09 | `docs/requirements.md`、`docs/usage.md` 和 [G5 手册](tui-manual-acceptance.md) 统一分层导航，移除旧数字/`[]` 指令 | 文档核对、TUI 焦点测试 |
| I-11 | 节点/组保存失败留在原 JSON 稿、绑定对象和焦点；Esc 先处理未保存选择 | `[UX-INT][I-11]` 真实 PTY 非法 JSON 与已保存源核对 |

审计没有 I-10，不补造。代码还调整了 CLI Profile clone/rename/remove 的已有 RPC 入口、人读详情的凭据隐藏、Subscription URL 在普通详情的隐藏；显式 JSON/编辑/导出继续返回真实授权内容。额外发现并修复 `stats --watch` 在管道输出未实时刷新、daemon 断开时因 SIGPIPE 无错误退出的问题。原生 15 模块、未知字段、单模块源文件、节点库存、confdir 多文件、显式连接边界和 `--json` 单行机器接口保留。

## 当前源码与自动检查

二进制：`build/interaction-baseline/xrm`、`xrmd` 和同目录测试程序，由当前工作树独立构建。CMake 缓存记录 Ninja、Debug、源码目录本仓库、依赖缓存 `build/dev/_deps`；构建命令 `cmake --build build/interaction-baseline -j4` 通过。`git diff --check` 通过；没有提交、推送、生产操作。

| 实际命令 | 结果与覆盖 |
|---|---|
| `build/interaction-baseline/tests/integration/m1/xrm_m1_tests --reporter compact` | **PASS**：15 用例、410 断言；含 CLI 人读/JSON、watch 实时输出/Ctrl-C/daemon 断连、完整 Profile/模块/节点/运行链 |
| `build/interaction-baseline/tests/tui/acceptance/xrm_tui_acceptance_tests '[UX-INT]' --reporter compact` | **PASS**：4 用例、145 断言；I-11、Profile/Subscription 外部编辑冲突、断连恢复、同进程 resize；最终二进制又由完整 `[TUI]` 覆盖 |
| `XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray TMPDIR=/tmp/xrm-interaction-realcore build/interaction-baseline/tests/tui/acceptance/xrm_tui_acceptance_tests '[TUI]' --reporter compact` | **PASS**：27 用例、781 断言，含真实核心 TUI 用例；之后仅把 Nodes 帮助中的 `c` 标签补上 cursor，最终标签又由 MCP 80×24 当前格核实 |
| `build/interaction-baseline/tests/integration/profile/xrm_profile_tests --reporter compact` | **PASS**：19 用例、1467 断言；Profile 读取/保存边界 |
| `XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray TMPDIR=/tmp/xrm-interaction-realcore ctest --test-dir build/interaction-baseline -R '^(RC-04|RC-05|RC-07|E2E-06)$' --output-on-failure` | **PASS**：4/4。使用官方 v26.9.9 发布资产的本地副本，zip SHA-256 `1eb9175d0f0a8f8149c9230a7fc5ae66ce332ed20a53155ce61fe62e3f58b7df`，xray 二进制 SHA-256 `c4ae6798c38e0e5343b192406746333cd0ba7ff3eb984f8c4b9939dcb68c3f8a`，版本实际报告 26.9.9；隔离 loopback，含真实配置检查和请求 |

固定 Xray 资产摘要与 [官方 v26.9.9 发布页](https://github.com/XTLS/Xray-core/releases/tag/v26.9.9) 的发布资产相核对。测试未访问真实订阅、未启动 TUN、未改系统网络。全仓 293 项、ASAN、生产环境、所有外部编辑器失败方式和真人可用性没有运行；不能从上述检查推断它们通过。

中间失败及处置：早期 `[UX-INT]` 暴露 Profiles `r` 被重命名占用，已改为 `r` 刷新、`R` 重命名；路径弹层关闭的 PTY 断言改为等待当前格关闭后再输入。后续两次全套回归分别碰到旧首用文案断言及将新弹层检查误插入旧测试的测试问题，已更新正确可见文案并移到目标用例；最终全套通过。CLI watch 的断线测试曾观察到 SIGPIPE 无 stderr，属于实际产品缺陷，已修复并由最终 M1 全套通过。没有删除失败用例或放宽业务断言。

## 真实终端操作与当前格

Agent 通过 `tmux-mcp-rs` 的 stdio `tools/call` 建立 `xrm-interaction`，在专用 tmux socket 和临时 HOME/XDG、xrmd socket、SQLite、confdir、fake-core 下操作实际 `xrm tui`。轨迹：80×24 TopNav → Profiles Content/Edit → Subscriptions Content → Nodes/Local Content → Overview c 连接保存目标 → f 看完整 confdir → Esc → Logs Content 按 c/d → Modules/log Source JSON 输入非法 `x` → 120×40 → 70×20 → 80×24 → Esc 放弃稿 → Overview d 停止 → 顶层 q 退出。使用 MCP 发 Enter/Esc/Tab/F2、文字键和 capture-pane；Ctrl-S、Shift-Tab 与 resize-pane 还经工具协议能力探测，实际草稿保存键由 PTY 回归执行。resize-window 的尺寸由同一 tmux server 辅助命令调整，前后没有重启 TUI。tmux ANSI `capture-pane -e` 在模块草稿当前格记录 8 个 SGR 序列；下面是**真实纯文本当前格摘录**，不是像素截图或设计示意：

```text
80×24 Profiles/Editor：
│> MCP Home  using
│Plan: standard local proxy · Core: MCP fake core
│Sources used here: 0 · Native modules: 15
│       ╭Edit Profile MCP Home────────────────────────────────────────╮
│       │> Name: MCP Home                                             │
│       │  Core: MCP fake core                                        │
│       │  Append selected default: on                                │
│       │↑↓ Field  Enter Edit  ←→ Core  Space Toggle  Ctrl-S Save  Esc│
```

```text
80×24 Subscriptions 空库存与 Nodes/Local：
│No subscriptions yet. Press a to paste a local or remote URL.
│Enter Details  a Add  e Edit  u Update  D Remove  Esc  ?
│ Nodes — 1 current, 0 source missing; showing 1 (current)
│▸  ◆ Save  MCP loopback direct           freedom/—              —
│Cursor: MCP loopback direct · available, untested
│Enter Details  / Search  s Save target  c Save & Connect cursor  Esc  ?
```

```text
80×24 Overview，运行后：
│View: MCP Home   Use: MCP Home   Run: MCP Home
│CORE  running · process_only · Xray 26.9.9
│Saved target: node: MCP loopback direct
│Actual target: node: MCP loopback direct
│Runtime files: …/run-1790419850259-1 · f Full path
│Xray core running; socks 127.0.0.1:10808; remote unverified
```

```text
80×24，Overview f 弹层把同一完整 confdir 分为连续两行：
│Confdir:
│/tmp/xrm-terminal-walkthrough-owpv_5ti/state/runtime/candidate/
│run-1790419850259-1
│Generated only by explicit Connect/Apply
```

```text
80×24 Logs Content，在此按 c/d 后：
│Logs — FOLLOW cursor 0
│p Pause   ↑/↓ history   r refresh   Server buffer remains active
│Xray core running; socks 127.0.0.1:10808; remote unverified
│↑↓ Move  p Pause/Resume  r Refresh  Esc  ?
```

```text
120×40 Modules/log JSON 未保存稿：
│[log]  dns  inbounds  outbounds  routing  observatory  api  metrics  stats  policy  fakedns  geodata  env  version  bu│
││  "log": {
││    "loglevel": "warning"
││x█
│Invalid JSON at 6:1; text draft retained: [json.exception.parse_error.101] parse error at line 6, column 1: synt…
│Focus: Content · log included Source JSON [unsaved] · 00_log.json
│JSON: arrows Move  type Edit  F2 Visual  Ctrl-S Save  Esc

70×20 同一进程：Terminal too small / 70x20 available; 80x24 required
恢复 80×24：同一 `x█` 稿、Content 焦点和 Ctrl-S Save 仍可见
```

真实运行事实：Logs 的 c/d 前后 `state=running` 且 PID、active 对象一致；Overview d 后 `state=stopped`、屏幕 `core stopped`。测试 daemon 以本次 PID 退出 0，MCP 只清理 `xrm-interaction` 会话，临时目录自动回收。

## CLI 实际 stdout / stderr / 退出码

同样是隔离 fake-core、非 TTY（另测 Profile list TTY），没有访问真实用户资料。以下 ID 为一次临时运行产生，可引用且没有截断；长路径省略仅在本记录中用于阅读。

| 操作 | exit | stdout | stderr |
|---|---:|---|---|
| `profile list` 空结果，非 TTY | 0 | 空 | 空 |
| `--json profile list` 空结果 | 0 | `[]\n` | 空 |
| `profile create --name 'Human Home' --core <id>` | 0 | `Created Profile Human Home [cccf144c-2e24-4ca5-931e-cd2223fc01ee]; no core started` | 空 |
| `select node <profile-id> <node-id>` | 0 | `selection saved; runtime unchanged` | 空 |
| `status`，连接前 | 0 | `state: stopped`；`saved target: scope is running Profile only; inspect \`profile show <ref>\`` | 空 |
| `connect 'Human Home'` | 0 | `Connected Profile [cccf144c-2e24-4ca5-931e-cd2223fc01ee]; process running; traffic unverified` | 空 |
| `disconnect` | 0 | `Stopped running core; saved Profile settings unchanged` | 空 |
| `--json stats --watch`，SIGINT | 0 | 一行 `availability:"unavailable"` 的 JSON 记录，无标题/转义 | 空 |
| `--json stats --watch`，daemon 中途退出 | 3 | 在进程仍运行时已输出一行 JSON 样本；断线后无伪成功记录 | `{"error":{"code":"io_error","message":"发送IPC请求失败: broken pipe"}}` |
| `status`，不存在的隔离 socket | 3 | 空 | `runtime.status: 连接xrmd失败: no such file or directory`，随后实际 socket 和重试命令 |

TTY `profile list` 一行含完整 Profile ID、名称、核心 ID；`--json profile list` 仍为单行数组，`--json status` 停止时仍保留原 `active:null`、`saved_target:null` 字段。`profile.get` 的源正文只在编辑器显式请求时增加，不向旧默认 JSON 响应添加字段。

## 验收范围和待办

T1/T2/T4/T5/T6/T8/T9/T10/T11 的关键路径由上述 PTY、CLI、RPC、文件与原有回归共同覆盖；T7 的本地 HTTP 添加/更新/失败保留与本轮编辑/暂停/移除预览已有自动证据。T3 的非法 JSON 和实际失败留稿已覆盖关键编辑器，但不同错误种类对每个编辑器的排列组合未全量自动运行；T12 使用真实 Xray 覆盖配置接受度与 loopback 请求，不能推断远端连通或生产权限。`subscription.replace` 仍是读取当前 revision 后提交的边界检查，没有新建 Server 原子 compare-and-swap；若在读取和提交之间发生外部并发写入，该窄窗口未验证为无覆盖。

G5 状态：**待用户人工复验**。按 [G5 手册](tui-manual-acceptance.md) 在用户终端走查并记录结果；自动化 PASS、MCP 操作和真实核心测试均不代签 G5。MCP 的 Codex 宿主发现仍待重载；本轮产品与终端验证使用实际二进制协议完成。

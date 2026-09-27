# XRM 人机交互现状审计

审计日期：2026-09-26（Asia/Shanghai）。基线：`master`，`561dede3c2e8e9e38a39d07a52076c9f08e3eeb0`。本报告供下一阶段设计、开发 Agent 独立使用；**它记录当前行为和已确认约束，不是一份新界面方案**。

## 1. 审计摘要与证据覆盖范围

**结论。** 当前 `xrmd` 的节点、订阅、Profile、15 个原生模块、运行、日志、统计等接口已有相当广的覆盖。CLI 和 TUI 都能完成隔离测试中的“登记测试核心 → 创建 Profile → 保存节点选择 → 连接 → 查看状态 → 停止”链。TUI 的三层焦点和 Modules 单视图草稿也已有可运行实现。但“入口存在”不等于交互闭环已经稳固：Logs 页存在未提示的连接/停止键，CLI 若干成功操作无任何普通输出，daemon 不可达时 TUI 把未知库存画成空库存。Profile 的 `Edit` 目前主要是一个会话标签，订阅编辑/移除仍须离开 TUI。具体证据和影响见第 8 节。

证据标签：**【运行验证】**为本轮实际进程、当前终端格或命令结果；**【代码证据】**为当前源码及测试确认而本轮未直接操作的行为；**【文档约定】**为设计要求；**【待验证】**为缺乏足够证据的场景。源码行号对应上述 commit 和本轮未改动的产品源码。

覆盖范围：

| 项目 | 本轮证据 |
|---|---|
| 仓库/文档 | 阅读 `AGENTS.md`、`docs/requirements.md`、`architecture.md`、`product-repair-spec.md`、`xrm-tui-interaction-repair.md`、`usage.md`、`testing-standard.md`、`tui-manual-acceptance.md`、验收记录；核对源码与构建目标。【代码证据/文档约定】 |
| 实际 CLI | 用绝对临时 socket、数据库执行空列表、参数错误、not found、核心登记、Profile 创建、节点添加/选择、模块列表、连接/停止、状态、统计、日志、`--json`、无 TTY `--watch`/Ctrl-C。【运行验证】 |
| 实际 TUI | 独立临时 daemon + tmux PTY，读取的是 `capture-pane` **当前屏幕**，不是 stdout 重绘流；记录 80×24、120×40、70×20，以及 Profiles、Modules、Logs、断连画面。【运行验证】 |
| 测试 | 焦点/草稿/首用/订阅失败/大列表/中文与日志定向 TUI 测试通过；CLI-01 两例通过；真实核心 E2E-06 SKIP。命令及数量见第 7 节。【运行验证】 |
| 界限 | 未执行生产配置、真实订阅、真实 Xray 流量或全量测试；未运行新构建。现有 `build/dev` 二进制时间晚于所查 `src/tui/app.cpp`，但这不等于干净构建证明。【待验证】 |

现有验收状态仍是 **G5 FAIL / 待用户人工复验**，不能由本轮 fake-core、PTY 或源码审计改写：`AGENTS.md:3`，`docs/product-repair-spec.md:5-21`，`docs/release-acceptance.md:5-22`，`docs/tui-manual-acceptance.md:1-3`。【文档约定】

## 2. 仓库、技术栈、运行环境与 Agent/工具环境

### 2.1 仓库和程序

【运行验证】Linux Debian 主机 `debian-work`，内核 `7.2.7+deb14-amd64`、x86_64；shell `/bin/bash`，`TERM=xterm-256color`，`COLORTERM=truecolor`。CMake 4.3.4、GCC 16.2.0、tmux 3.7c、`script` 可执行。当前分支/commit 见页首。开始及结束时原有未跟踪文件为 `docs/codex-iteration-prompt.md`、`docs/codex-repair-task.md`、`docs/product-repair-spec.md`、`docs/xrm-tui-interaction-repair.md`；本轮没有覆盖它们。报告文件为本轮唯一新增项目文件。

`CMakeLists.txt:1-6,64-88,155-175` 确认 C++23、项目版本 0.1.0；CLI11 v2.5.0、FTXUI v7.0.1、nlohmann/json v3.12.0、libuv v1.51.0、Catch2 v3.7.1 由固定 FetchContent 标签指定。这是**仓库声明版本**，本轮没有联网核验发布。Server 用 libuv/Unix domain socket；`src/client/rpc_client.cpp` 与 `src/server/rpc_server.cpp` 承载线协议，`src/contract/methods.hpp:7-85` 列出方法，`src/server/api_router.cpp:700-1162` 实际派发。`xrm` 主入口在 `apps/xrm_main.cpp:3`，`xrmd` 在 `apps/xrmd_main.cpp:21-40,72-89,93-138`。CLI 命令树在 `src/cli/commands.cpp:20-53` 和同目录各 `*_commands.cpp`，格式化在 `src/cli/print.cpp:282-516`；TUI 页面、会话状态、事件和绘制集中于 `src/tui/app.cpp:244-4085`，表格裁剪在 `src/tui/table_view.hpp:16-102`。

【运行验证】已有 `build/dev/xrm`、`build/dev/xrmd` 和测试二进制；`./build/dev/xrm --help`、`./build/dev/xrmd --help` 可运行。仓库标准命令见 `CMakePresets.json`：`cmake --preset dev`、`cmake --build --preset dev`、`ctest --preset dev`；本轮只运行现有测试二进制及 `ctest --test-dir build/dev` 的定向用例。隔离 Server 实际以 `xrmd --foreground --socket <临时>/run/xrmd.sock --database <临时>/state/server.sqlite` 启动，程序将数据根派生为数据库父目录（`apps/xrmd_main.cpp:93-119`）。隔离 TUI 显式 `xrm --socket <同一临时socket> tui`；核心登记仅使用仓库 `build/dev/tests/xrm_fake_core`。**临时 HOME/XDG 之外又核对并显式指定了 socket、数据库和由数据库派生的 runtime confdir**；运行记录中的 confdir 均在该临时 state/runtime 内。本轮测试 daemon 与 fake-core 已停止，临时数据目录已由脚本清理。【运行验证】不以 fake-core 宣称真实网络代理或 Xray 原生配置可用。

测试实际目录包括 `tests/contract`、`tests/unit`、`tests/integration`、`tests/tui/acceptance`；真实核心相关用例在 integration/release、geodata 等目录并以 `real_core` 标签标识，而非独立 `tests/real_core` 目录。`ctest --test-dir build/dev -N` 列出 293 项。TUI 验收文件 `tests/tui/acceptance/acceptance_tests.cpp:80-165,369-425` 使用 tmux/PTY、独立 socket、当前格捕获和按键；其中焦点 `:2378-2426`、草稿 `:2428-2602`、首用 `:1730-1887`、大列表 `:1378-1472`。CLI、IPC、运行及真实核心用例分布在 `tests/integration`；测试写法约定见 `docs/testing-standard.md:24-75,241-268,438-463`。

### 2.2 Agent 与可用工具盘点

【运行验证】本轮运行于本地 Codex API Agent 会话；宿主标识本会话 Agent 为 Codex/GPT-6，但未暴露可核实的 Codex 客户端安装版本，`codex` 命令不可执行。用户目录 `~/.codex/version.json` 的 `latest_version=0.157.1` 只是更新缓存，**不等于当前版本**。用户级配置位置 `~/.codex/config.toml`、技能根 `~/.codex/skills`、插件缓存 `~/.codex/plugins/cache`；项目级 `.codex/` 与 `.agents/` 目录存在但为空。未读取认证文件。可见系统 Skills 有 imagegen、openai-docs、plugin-creator、skill-creator、skill-installer 等；插件缓存含 figma、plugin-management、openai-templates。这里只盘点，无安装、调用或选型。

`~/.codex/config.toml` 中有 mcp-ssh、playwright 的配置项；本会话工具清单**实际暴露**相应 MCP 方法及若干 Codex Apps/Figma 方法，但本轮没有调用这些外部服务，所以连接成功与权限仍【待验证】。不能把“配置存在”写成外部服务已可用。PTY/按键注入/屏幕读取/resize 能力则已通过本地 `tmux send-keys`、`capture-pane`、`resize-window` **实际调用**；轮询当前格可等待 UI 状态。没有使用终端图像截图工具，本文只附真实字符格摘录。Agent、源码、构建与被测进程均在 `debian-work` 本机；SSH 插件配置不是本轮连接路径，也没有文件同步。

## 3. 已确认设计约束与当前实现偏差

权威边界为本轮用户明确约束、`AGENTS.md:5-45,81-93`、`docs/requirements.md:37-53,283-303`、`docs/architecture.md` 和较新的 `docs/xrm-tui-interaction-repair.md:21-95,198-260`。【文档约定】产品仍是 C++23/Xray-only 的 Manager+Server：独立节点库、Profile 原生 `modules/`、bindings、生成的多文件运行 confdir、独立 Xray 进程。保存不连接，订阅刷新不发布运行文件，退出 TUI 不停止核心。TUI 的焦点层级、Enter/Esc 消费、View/Use/Edit、底部信息、单视图 F2、routing Source Content 内的 `p` 是本轮验收约束。

| 约束或历史说法 | 当前证据/偏差 |
|---|---|
| 顶层七页；二级 Tab 仅 Tab/Shift-Tab，数字与 `[]` 不做导航 | `src/tui/app.cpp:61-75,801-843` 当前按层消费；焦点 PTY 用例通过。【运行验证/代码证据】但旧 `docs/requirements.md:283-285` 仍写“数字快捷键一致”，`docs/tui-manual-acceptance.md:7` 仍教 `1–7`/`[]`；它们与本轮用户约束、`docs/xrm-tui-interaction-repair.md:84-95` 冲突，属于文档过期，不能按旧键验收。【文档偏差】 |
| View、Use、Edit、保存、运行各有独立含义 | Profiles Enter/u/e 实测未启动核心；`src/tui/app.cpp:2549-2573` 有不同字段。【运行验证】但 `e` 只置 `editing_profile_id` 与状态文案；该字段实际仅在行标签/清理处读取，没有 Profile 草稿或独立保存动作，详见 I-05。【代码证据】 |
| Content 命令按页面/上下文有效 | 非 Content 被 `src/tui/app.cpp:843` 阻挡；但 Content 页有未提示的全局 `c/d` 运行分支，Logs 实测触发连接/停止，详见 I-01。【运行验证】 |
| 源、草稿、已保存、运行实际配置分开 | 模块 Visual→JSON 改动未进入 Server source；Esc 出现选择弹窗；fake-core 运行 confdir 独立生成。【运行验证】仍须注意 TUI 状态词与 CLI `status` 的作用域问题（I-06/I-08）。 |
| 80×24 可用，关键提示在底部 | 80×24 真实格可操作，70×20 保护提示可恢复；但长 footer 被省略号截断，Visual 草稿时 `Ctrl-S` 只显示 `Ctrl-…`，详见 I-07。【运行验证】 |
| R06 的 15 个原生顶层模块统一入口 | `config modules` 实测 15 项；TUI Modules 二级导航遍历 `modules_`，可打开 disabled+无 source 的用例已跑。运行前后是否都经真实 Xray 接受本轮未验证。【运行验证/待验证】 |
| 订阅来源可正常管理 | TUI 有添加、更新、绑定、历史入口；CLI/Server 另有编辑、暂停/恢复、移除，TUI 缺这些入口，详见 I-04。【代码证据】 |

## 4. 后端能力—用户任务—CLI/TUI 入口映射

“完整”只评价表中用户任务是否能由该入口走到结果；不要求每个内部 RPC 都上屏。RPC 合同见 `src/contract/methods.hpp:7-85`，派发见 `src/server/api_router.cpp:700-1162`。CLI 命令均取自实际 `--help` 与源注册；TUI 取自 `src/tui/app.cpp:887-1010,1014-1800`。**R**=本轮运行验证，**C**=只看代码/已有测试。

| 用户任务 | 后端能力 | CLI 入口 | TUI 入口 | 完整性/缺口 | 证据 |
|---|---|---|---|---|---|
| 登记/选择/安装核心 | core.register/list/release/install/use/remove | `core register/list/releases/install/use/remove` | Config→Core：a/l/p/i/u/D | 登记并指定给测试 Profile 可完成 R；官方安装未实际下载 C | `api_router.cpp:618-700`；`app.cpp:999-1010,1525-1575` |
| 启动/连接/停止/重启 | connection.connect/disconnect、core.start/restart/stop、operation.get | `connect/disconnect/core stop/restart/operation get` | Overview/Nodes 的 c，部分 Content 的 c/d；异步通知 | 测试核心链可完成 R；TUI 全局 c/d 隐藏副作用 I-01；CLI 成功空输出 I-02 | `api_router.cpp:1000-1036,1139-1160`；`app.cpp:971-974,3289-3382` |
| 导入、刷新、查看订阅 | subscription.add/list/get/refresh | `sub add/list/show/update` | Subscriptions a/u/Enter | 隔离首用、失败和部分刷新测试通过 R；不会改运行核心 | `src/server/api_router.cpp:420-539`；`src/contract/methods.hpp:31-36`；`app.cpp:1326-1379,3077-3211`；`acceptance_tests.cpp:1730-1976` |
| 编辑/暂停/移除已有订阅 | subscription.replace/remove | `sub edit/remove` | 无相应动作 | CLI 可做；TUI 任务中断，I-04 C | `subscription_commands.cpp:37-83`；`app.cpp:887-1010` |
| 管理节点库存/测试 | node.add/list/get/edit/copy/remove/test | `node add/list/show/edit/replace/copy/remove/enable/disable/alias/labels/test*` | Nodes→各来源/Local、详情、测试、添加表单 | 基本添加/浏览/测试入口有；大列表与中文用例通过 R；“Selected”词义冲突 I-06 | `methods.hpp:16-30`；`app.cpp:1187-1267`；`acceptance_tests.cpp:1378-1472` |
| 创建、查看、使用 Profile | profile.create/list/get、session context | `profile create/list/show`，后续命令显式 Profile ref | Profiles n/Enter/u，默认单 Profile；多 Profile picker | View/Use 测试核心不启动 R；CLI create 空输出 I-02 | `api_router.cpp:700-728`；`app.cpp:367-382,2549-2573` |
| 复制/重命名/删除及编辑 Profile | profile.clone/rename/remove/save | CLI 无 clone/rename/remove/save 子命令 | Profiles c/r/d/e；Config→Core、Modules 等分散编辑 | TUI 可完成单项；CLI 命令树不覆盖这些管理动作 C；Edit 无独立编辑稿 I-05 | `api_router.cpp:728-768`；`profile_commands.cpp:7-28`；`app.cpp:887-896,2023-2029,2549-2593` |
| 为 Profile 指定订阅、目标和组 | profile.sources.set、connection.select、group.* | `profile sources set`、`select node/group`、`group add/list/show/edit/remove/select/override` | Subscriptions b；Nodes s/m/Groups | 选择与 Connect 分开 R；TUI组入口有 C；临时 override 合理地主要在 CLI | `methods.hpp:41,53,68-75`；`app.cpp:960-972,3823-3878` |
| 查看、编辑、启停原生模块 | module.list/get/save/include | `config modules/show/edit/enable/disable` 和若干专用表单 | Modules 二级 15 key；Enter、F2、Ctrl-S、e、v、F6 | 源编辑/草稿可完成 R；运行文件只在显式连接/应用生成；80列提示裁剪 I-07 | `api_router.cpp:816-900`；`app.cpp:1268-1304,3490-3525,3776-3820` |
| 预览/校验/导出/应用配置 | config.render/check/dump/files/export/apply | `config render/check/dump/show/export/apply` | Modules Source/运行文件预览、连接；无专门 check/dump/export 入口 | CLI 可用 C；TUI 的运行前错误走连接结果，不能在页内单独 check/export；这些也可合理保留为高级 CLI | `api_router.cpp:1013-1110`；`profile_commands.cpp:30-79`；`app.cpp:3418-3453` |
| 路由预设和共享 Geodata | routing.preset.plan/save；geodata.* | `routing preset plan/save`、`geodata` | Modules→routing Source `p`；Config→Geodata | 路由 p 作用域代码正确；预设完整保存链现有测试 C；真实资源环境未本轮实测 | `api_router.cpp:576-618,796-815`；`app.cpp:623-658,951-959` |
| 查看状态、统计、日志和恢复结果 | runtime.status/stats、logs.read、operation.get | `status/stats [--watch]/logs/operation get` | Overview、Logs、底部通知 | 状态/未启用统计可见 R；Logs 隐藏运行键 I-01；daemon 不可达显示误导 I-03；CLI 作用域 I-08 | `api_router.cpp:1111-1144`；`print.cpp:442-516`；`app.cpp:1077-1186,1305-1325` |
| XRM 外观、编辑器、测试参数 | settings.get/update | `settings show/set/editor`，节点测试选项 | Config→Appearance/Editor/Tests | 设置与 Xray 模块分离 C；Appearance 取消/保存代码明确 | `app.cpp:844-870,1490-1523,1941-1968` |

其中 `operation.get`、`runtime.metrics`、`module.change_plan` 等有内部/高级用途，**不据此推断每个都缺少 TUI 按钮**。Server 能力与产品任务的取舍须按真实用户链判断。

## 5. TUI 页面、状态、焦点与按键模型

### 5.1 当前页与可见信息

【代码证据，部分由第 7 节 PTY 复核】顶层固定顺序 `Overview / Profiles / Modules / Subscriptions / Nodes / Logs / Config`（`src/tui/app.cpp:61-75`）；内部页码不是顶栏顺序，Profiles 为 8、Groups 为 5（`:1805-1816`）。二级：Modules 是 15 个原生 key；Nodes 是“全部订阅、各来源、Local、Groups”（`:1828-1844`）；Config 是 Core、Geodata、Tests、Appearance、Editor、General。Overview、Profiles、Subscriptions、Logs 无可见二级层。

| 页 | 主体当前内容与入口 | 空/失败/编辑表现 |
|---|---|---|
| Overview | 查看 Viewing/Using/Running Profile，核心、代理入口、保存/实际目标、override、库存数、统计和图表；c/d/r/i | 无 Profile 提示首用；无统计显示 unavailable/pending；断连时会显示“0 库存/CORE unknown”，见 I-03。绘制 `:1077-1186`。 |
| Profiles | 行高亮、名称、核心、来源数；Enter View、u Use、e Edit、n/c/r/d 管理 | 多 Profile 无有效选择时启动在 picker；行标签 using/edit context/running；没有独立完整 Profile 编辑稿。绘制 `:1577-1601`，动作 `:887-896,2549-2593`。 |
| Modules | 顶部第二行模块 key；主体单一 JSON 或 Visual；底部 key、included、Source/Runtime、路径、草稿状态；Enter 编辑、F2、v、e、F6、Ctrl-S | disabled/无 source 可打开临时草稿但不自动 enable；非法 JSON 留在文本模式；未保存退出弹选择；Runtime 只读。绘制 `:1268-1304,1609-1658`，事件 `:514-607`。 |
| Subscriptions | 名称、节点数、上次更新/结果、自动刷新、Profile 绑定；Enter 详情、a/u/b/h | 空时提示 a；异步 spinner、partial/failed 计数与错误；编辑、删除缺入口。绘制 `:1326-1379`，动作 `:995-999`。 |
| Nodes / Groups | 共享库存、搜索、来源分类、状态 Save/Run、测试结果；详情及本地节点/组入口 | 空列表有下一步提示；节点按 ID 保持高亮；大列表 viewport、中文宽度由 `TableView` 与测试覆盖。节点/组 JSON 编辑保存失败后会退出编辑，见 I-11。绘制 `:1187-1267`，表格 `table_view.hpp:78-102`。 |
| Logs | FOLLOW/PAUSED、cursor、viewport、dropped/truncated、日志正文；p/r/上下 | 空时显示 `(empty)`；实际隐藏 c/d 运行操作未在页内帮助出现。绘制 `:1305-1325`。 |
| Config | Core 表/版本/来源/状态及登记安装；Geodata；Tests；Appearance 草稿；Editor argv；General socket | 设置与原生模块分区；长路径可挤出；核心安装通过异步 operation 轮询。绘制 `:1440-1575`，动作 `:844-886,999-1010`。 |

主要局部层：文本 Prompt（订阅、核心、Profile、节点、组、Geodata、结构字段等），模块 Visual/JSON 内编辑，节点详情，订阅详情，帮助、历史清理、路由预设和差异、首用 Connect、未保存草稿选择等弹窗。事件在 `src/tui/app.cpp:493-795` 按活跃局部层优先处理，再到三层焦点；渲染在 `:1662-1800`。这不是一般 FTXUI Menu 默认事件链，主组件 `OnEvent` 自行决定是否消费。

所有顶级页从 TopNav 用 Tab/Shift-Tab 到达，Enter 按是否存在二级导航进入 SecondaryNav 或 Content；Content 的 Esc 一次退到二级/顶层，二级 Esc 退顶层，TopNav 的 q 才退出。焦点名称和当前对象/源路径绘制在底部，选中行另以反白/箭头标记（`app.cpp:801-843,1603-1661`）。加载状态不是统一模型：订阅更新有 spinner/partial/failed，运行操作有 starting/failed toast，Logs 的空缓冲为 `(empty)`，统计用 unavailable/pending，Core 安装可按 r 轮询；Server 整体断开时 Overview 的空/未知语义见 I-03。模块 Ctrl-S 保存错误会留在编辑器并保留 draft（`app.cpp:3805-3813`）；节点/组错误路径则不同（I-11）。以上是当前行为，不等于所有页面均有完整加载、断连和恢复界面。

### 5.2 真实按键模型和状态作用域

| 当前上下文 | 键 | 实际行为/副作用 | 消费与提示 |
|---|---|---|---|
| TopNav | Tab/Shift-Tab | 七页循环；仅改显示页 | `:807-824` 消费；PTY 焦点用例通过；数字/`[]` 不切页。 |
| TopNav | Enter | 有二级页则 SecondaryNav，否则 Content | `:825-829` 消费；本轮 Profiles 一次 Enter 只到 Content。 |
| SecondaryNav | Tab/Shift-Tab、Enter、Esc | 切模块/来源/Config 子页；Enter 到 Content；Esc 回 TopNav | `:801-843` 消费；本轮 Modules 一次 Enter 未打开编辑。 |
| Top/SecondaryNav | c/d/u/e/s/p、方向键 | 不执行隐藏内容命令 | `:843` 消费；定向 UX 测试通过。 |
| Profiles Content | 上下、Enter、u、e | 行高亮；View；Use 会同时 View；Edit 会 View 并设编辑标签。三者本轮未改运行 PID | `:887-896,945,2549-2573`；footer 提示在 80 列末端裁剪。 |
| Modules Source Content | Enter/F2/e/v/p | Enter 开草稿；F2 切单视图；e 保存 included 切换；v 切运行预览；p **只在 routing Source** 打开预设 | `:935-959`；F2/草稿本轮实测；e 与 Ctrl-S 均不连接。 |
| 模块编辑/弹窗 | Ctrl-S、F2、Esc、普通字符 | Ctrl-S 保存 Source；F2 Visual/JSON 共用 `draft_`；非法 JSON 留文本与错误；Esc 未保存出现 Save/Discard/Continue | `:514-607,1668-1672,3776-3820`；本轮改 `warning→info` 后源仍 `warning`。 |
| Logs Content | p、上下、r | 暂停/浏览/刷新，页内与底部提示一致 | `:899-905,974-984`；测试覆盖暂停。 |
| Logs Content | c、d | **c 连接，d 停止**；页内和 footer 均未提示 | `:971-974` 的跨页分支；本轮真实 PTY/RPC 状态实测，I-01。 |
| TextInput/帮助/详情 | 字符、Esc | 字符归所属控件；Esc 关当前层 | `:493-795`；输入框中 q/e/空格相关测试通过。 |
| 小于 80×24 | 任意键 | 显示保护页，事件不交给隐藏页 | `:489-492,1014-1022`；70×20→80×24 本轮实测恢复。 |

状态并非天然相同：`selected_` 是当前行索引，`state_.highlighted_node_id` 用于刷新后的节点高亮；`profile_id_` 别名指向 `state_.viewed_profile_id`；`state_.used_profile_id` 是本 TUI 会话后续 Connect 的方案；`state_.editing_profile_id` 是 Profile 行标签；`draft_`/`editing_dirty_` 是模块编辑；已保存来源由 `module.get/profile.get` 取回；运行实际值由 `runtime.status.active` 和运行 confdir 给出（`app.cpp:287-301,1948-1949,2683-2697,3948-4058`）。用户看到的“Selected:”在 Nodes 中却指向光标高亮，详 I-06。

## 6. CLI 命令结构、交互与输出行为

【运行验证】根帮助为 `xrm [OPTIONS] [SUBCOMMAND]`，全局 `--socket`、`--json`、`-h/--help`；根下有 `server/core/node/sub/profile/config/group/select/connect/disconnect/status/logs/operation/geodata/routing/settings/stats/tui`。CLI11 接收显式参数；`profile show/connect/config ...` 可按**精确 UUID 或唯一名称**解析，重名报歧义（`src/cli/print.cpp:139-163`）；节点和来源多用 ID。没有当前目录驱动的隐式 Profile；`xrmd` 默认 socket 由 `XDG_RUNTIME_DIR` 或 `/tmp/xrmd-UID.sock` 决定，数据库由 `XDG_STATE_HOME`/`HOME` 决定（`apps/xrmd_main.cpp:23-40`），因此审计一律显式 `--socket`。外部编辑器 `VISUAL/EDITOR` 只影响 TUI Manager 编辑器，不代表 CLI 提示输入。

【代码证据】主 CLI 无交互式问答：必需参数由 CLI11 报错；长 RPC operation 默认轮询最终结果，最长五分钟（`print.cpp:54-72`）；`stats --watch` 每两秒取样、SIGINT/SIGTERM 停止（`commands.cpp:76-109`）。机器模式成功输出单行 JSON；失败 JSON 写 stderr 并按错误返回非零（`print.cpp:282-293`）。普通模式依方法定制；未列分支的成功方法返回 0 但可能一字不输出（I-02）。颜色自动模式仅在 stdout 是 TTY 时启用（`print.cpp:104-118`）；本轮未直接在 CLI 交互 TTY 下观察颜色。

实际输出样本（路径/UUID 用 `<临时>`、`<id>` 替换；各行其余文字为实际结果；均为 **非 TTY**，stdout/stderr 在探针中分开捕获）：

| 命令/前置 | stdout | stderr | 码/变更 |
|---|---|---|---|
| `xrm --socket <临时>/run/xrmd.sock status`，daemon 未启动 | 空 | `连接xrmd失败: no such file or directory` | 3；无变更 |
| `profile create`，缺 `--name` | 空 | `--name is required` | 106；无变更 |
| `config show Office log --source source` | 空 | `The following arguments were not expected: source --source` | 109；无变更；将选项提前可用 |
| `profile show missing` | 空 | `找不到对应Profile（请使用精确名称或UUID）` | 1；无变更 |
| `--json profile show missing` | 空 | `{"error":{"code":"not_found","message":"找不到对应Profile（请使用精确名称或UUID）","subject":"missing"}}` | 1；无变更 |
| `profile list`，空库 | 空 | 空 | 0；无变更 |
| `--json profile list`，空库 | `[]` | 空 | 0；无变更 |
| `profile list`，创建两方案后 | `<id>\t审计 Home\t<core-id>`、`<id>\tOffice\t<core-id>` | 空 | 0；标识稳定可引用 |
| `node list`，1 个虚构本地节点 | `Nodes: 1`，随后 `<id>\t审计节点 长名称 A\tfreedom\traw-only\tavailable` | 空 | 0；名称未乱码 |
| `select node <Home-id> <node-id>` | `selection saved; runtime unchanged` | 空 | 0；保存选择，无核心 |
| `profile create --name Human-only --core <core-id>` | **空** | 空 | 0；实际新建，须另查 list 才知 ID |
| `connect '审计 Home'` | **空** | 空 | 0；fake-core 已运行，`status` 才显示 |
| `disconnect` | **空** | 空 | 0；测试核心停止 |
| `stats`，运行但统计相关模块未启用 | `metrics: unavailable\nunavailable conditions: stats policy metrics\nsample: unavailable` | 空 | 0；正确区分 unavailable 与零 |
| `logs --limit 3`，空缓冲 | 空 | 空 | 0；与失败/无日志需靠上下文区分 |

`--json status` 在**停止但已对 Home 保存节点选择**时实际为 `{"active":null,"actual_target":null,"saved_target":null,"state":"stopped",…}`；连接成功后变为 `state:"running"`，`active.profile_id` 指向 Home、`active.confdir` 为 `<临时>/state/runtime/candidate/run-…`、`saved_target` 与 `actual_target` 都为同一虚构节点、`pid` 非空。这里的 `saved_target:null` 是 runtime.status 仅查**活动 Profile**所致（`src/server/application.cpp:1259-1263`），不能据此说 Home 没有保存节点；详 I-08。

【运行验证】`--json stats --watch` 的 stdout 被管道捕获时两秒产生两行独立 JSON，stderr 为空；发送 SIGINT 后退出码 0。此行为适合逐行流式解析，但命令帮助只描述“每两秒刷新”，未提供机器 schema 稳定性保证。本轮没有在非交互环境触发确认等待；源码 CLI11 参数路径未发现交互读取 stdin 的主流程。`--help` 与本轮所用参数错误一致；未逐项对全部子命令执行错误矩阵。

完整命令树摘要见附录 A。CLI 与 TUI 的可见称呼有差异：CLI `sub` 对应 TUI `Subscriptions`，CLI `config` 是原生模块操作而 TUI `Config` 是核心/XRM 设置（原生配置在 `Modules`）；CLI `select node` 明确“只保存”，TUI Nodes 的行提示“Selected”实际上是高亮，`s` 才保存。前者是既有命令兼容性，后者影响状态理解。

## 7. 代表性真实操作记录

以下每次探针都用新建的 `/tmp/xrm-audit-XXXX/`，`XDG_RUNTIME_DIR`/`XDG_STATE_HOME`/`XDG_CONFIG_HOME`/`HOME` 指向其子目录，并在 `--socket`/`--database` 明确传入路径；Server 的 runtime confdir 落在对应临时 state 下。虚构节点用原生 `freedom` outbound 和 `audit-direct` tag，无真实订阅、TUN 或外部网络。TUI 在独立 tmux PTY 中，字符画均为 `capture-pane -p` 的**当前格摘录**，仅省略空白行、边框长填充和动态 ID；不是伪造截图。所有程序最后退出、临时目录清除。【运行验证】

**T1：Profiles 焦点、查看、使用、编辑。** 先通过隔离 CLI 注册 fake-core，建立“审计 Home”“Office”，分别有已保存节点选择；启动 `xrm tui` 时因多 Profile 未浏览过，初屏为 Profiles/TopNav，`Use:none`、`Run:—`。按 `Enter` 到 Content，仍未 View；再 `Enter` 查看 Home；`Down` 高亮 Office 时底部仍 `View: 审计 Home · Use: none`；`u` 后 `View: Office · Use: Office · Run: —`；`e` 后 Office 行出现 `edit context`。CLI `--json status` 前后均 `state:"stopped",active:null,pid:null`。这些动作未隐式 Connect。当前格摘录：

```text
│ Overview [Profiles]  Modules  Subscriptions  Nodes  Logs  Config             │
│  审计 Home                                                                   │
│> Office  using  edit context                                                 │
│Plan: standard local proxy · Core: Audit fake core                            │
│Editing Profile Office; rename/core/module changes save independently; u…     │
│Focus: Content · View: Office · Use: Office · Run: —                          │
```

**T2：Modules 单视图和草稿。** 从 Profiles Content `Esc → Tab → Enter → Enter` 到 Modules/log Content。前两个 Enter 分别进 SecondaryNav/Content，第三个才开 Source Visual。按 `Down → Space → F2` 将 `loglevel` 从 warning 切为 info，再看 JSON：`"loglevel": "info"`，footer 为 `[unsaved]`；同时 CLI `--json config show --module-key log --source source Office` 的 source text 仍是 `warning`。按 `Esc` 出现 `Save / Discard / Continue`，按 `d` 恢复 `warning`；无连接或保存。关键当前格：

```text
│[log]  dns  inbounds  outbounds  routing  observatory  api  metrics  stats  po│
││  "log": {                                                                 │ │
││    "loglevel": "info"                                                     │ │
│Visual draft changed; Ctrl-S saves Source; runtime unchanged                  │
│Focus: Content · log included Source JSON [unsaved] · modules/00_log.jso…     │
│JSON: arrows Move  type Edit  F2 Visual  F6 Editor  Ctrl-S Save  Esc          │
```

在 80×24 的 Visual 编辑画面，帮助行实际末尾为 `Ctrl-…`；文件路径在上例被截成 `modules/00_log.jso…`。120×40 可见更多模块 key 和完整 footer；70×20 显示 `Terminal too small / 70x20 available; 80x24 required`，恢复 80×24 后仍在同一 Modules/log 页面。本次 resize 发生在丢弃后；“未保存草稿在 resize 后一定保留”仅由现有 `[P1-SIZE]` 等测试和代码支持，未由这段手动轨迹单独复核。

**T3：Logs 页未提示的运行键。** 从 Modules 回 TopNav，Tab 到 Logs，Enter 进入 Content。当前格只列 `p Pause / ↑↓ history / r refresh`：

```text
│ Overview  Profiles  Modules  Subscriptions  Nodes [Logs]  Config             │
│Logs — FOLLOW cursor 0                                                        │
│Viewport top: (empty)  rows 0-0/0                                             │
│p Pause   ↑/↓ history   r refresh   Server buffer remains active              │
│Focus: Content · View: Office · Use: Office · Run: —                          │
│↑↓ History   p Pause/Resume   r Refresh   Esc Back   ? Help                   │
```

此时按 `c`，footer 变 `Connect started`，隔离 `--json status` 观察到 `state:"starting"`；随后按 `d`，footer 先出现 `Connection failed: 连接请求已被停止` 和 `core stopped`，`status` 变 `state:"stopped"`。这验证了操作确实越过 Logs 的可见动作范围；停止优先使尚在启动的连接取消。只操作本轮的 fake-core。

**T4：CLI 完整测试进程链与观测。** 隔离 CLI `core register <fake>`、`profile create --name '审计 Home' --core <id>`、`node add --outbound-json '{"protocol":"freedom","settings":{},"tag":"audit-direct"}'`、`select node <home-id> <node-id>` 后，`connect '审计 Home'` 返回 0、普通 stdout/stderr 均空。随后 `--json status` 为 `running`，有 PID、Home Profile、临时 `runtime/candidate/run-…` confdir、保存/实际节点一致；`stats` 明确为 unavailable（stats/policy/metrics 未启用）；`disconnect` 后测试 daemon 正常退出。这只证明 CLI→IPC→Server→多文件→**fake-core 的 argv/进程**链，不证明真实 Xray 解析或网络连通。

**T4b：TUI 首用到运行的现有 PTY 测试。** 本轮通过的 `[product-repair][UI-01][E2E-06][TUI-SUB-01]` 用例使用独立本机订阅 HTTP fixture（虚构 VLESS 指向 127.0.0.1:9）和 fake-core。实际测试输入顺序为：空 Profile 直接 `xrm tui` → Subscriptions `a` 填名称/本机 URL → `u` 更新并见 1 个节点 → Nodes 查看 → Config/Core `a` 登记测试核心 → Profiles `n` 创建并选用方案 → Subscriptions `b` 绑定 → Nodes 高亮 `c` 连接 → CLI/RPC 查 `running` 和已保存节点 → `d` 停止 → `s` 保存节点选择 → Overview `c` 再连接 → 查看 Logs/Statistics → `q` 退出后核心仍运行 → 测试清理时 `disconnect`。测试还断言订阅更新后核心仍停止，且 fake-core check/run argv 记录存在（`tests/tui/acceptance/acceptance_tests.cpp:1730-1885`）。该用例 PASS 是本轮**实际测试结果**；它的部分屏幕断言使用 PTY 字节流，不能把测试内部 `read_screen` 当作本报告的最终当前格截图。最终当前格的独立观察由 T1–T3 提供。

**T5：daemon 不可达与非交互。** 尚未启动 daemon 的 `status` 返回 3/stderr `连接xrmd失败: no such file or directory`；停止后再启动 TUI 指向同一已消失的临时 socket，界面仍可打开，但主体展示 `First use`、`Inventory: 0 subscriptions · 0 nodes`、`CORE unknown`，底部只有 `Cannot load nodes: 连接xrmd失败…`，没有清楚区分“服务断连，库存未知”与“确实空库”。`--json stats --watch` 在重定向 stdout 时输出两行 JSON；SIGINT 退出 0，无 stderr。CLI 的空列表/成功无输出详第 6 节。

**现有测试本轮命令和结果。**

```text
./build/dev/tests/tui/acceptance/xrm_tui_acceptance_tests '[UX-02],[UX-03],[UX-04],[UX-05],[UX-06],[UX-07],[UX-08],[UX-09],[UI-07],[UI-08],[UX-01]' --reporter compact
→ 7 test cases / 300 assertions PASS
./build/dev/tests/tui/acceptance/xrm_tui_acceptance_tests '[product-repair],[TUI-SUB-02],[TUI-SUB-03],[P1-SIZE]' --reporter compact
→ 5 test cases / 299 assertions PASS
./build/dev/tests/tui/acceptance/xrm_tui_acceptance_tests '[UI-SCROLL],[UI-01],[UI-02]' --reporter compact
→ 4 test cases / 297 assertions PASS
ctest --test-dir build/dev -R '^(CLI-01 server unavailable is an error on stderr, not false success|CLI-01 subscription add/list/show/edit/remove and node ownership actions|E2E-06)$' --output-on-failure
→ CLI-01 两例 PASS；E2E-06 SKIP（未提供真实 Xray）
```

前三组第一次在默认沙箱中试跑时均在 `daemon.start()` 失败：`绑定Unix Socket失败: operation not permitted`，exit 42；经批准在允许测试自建 socket 的环境重跑，才得到上面的 PASS。这个首轮失败是环境限制，**不是产品 TUI 回归**。没有运行全量 293 项、ASAN、systemd、真实核心或生产数据验收。

## 8. 已确认问题清单及根因分组

严重度表示对用户任务的影响；所有“已确认”项都有具体触发与结果。相同根因的其他表象放在同一项。未证实风险放第 10 节。

### 8.1 焦点/事件分发与动作语义

**I-01｜高｜Logs 等内容页存在未提示的全局连接/停止键。** 任务：查看日志、在当前页安全操作。触发：隔离 Profile 已 Use 且有保存节点，`Logs → Content → c → d`。实际：`c` 开始连接，`d` 停止/取消核心；Logs 主体和 footer 均仅提示 p/方向键/r。原因是 `src/tui/app.cpp:971-974` 在多个页面特定分支之后仍处理通用 `c/d`，而帮助在 `:1604-1624` 取 Logs 页动作。违反本轮“Content 动作必须按页面、当前上下文生效”“运行动作显式表达”的约束（`docs/xrm-tui-interaction-repair.md:21-27,103-122`）。证据：【运行验证】T3 的当前格、`status starting→stopped`；【代码证据】上述行。绕过：避免在 Logs 等非运行业务页按 c/d，使用 CLI `connect/disconnect` 或明确的 Overview/Nodes 入口；用户无理由仅凭当前页提示知道该绕过。此项覆盖同一根因在其他 Content 页可能出现的表象，未逐页实测所有页。

**I-05｜中｜Profiles 的 Edit 显示编辑上下文，但没有由其拥有的编辑流程。** 任务：用户查看 B 后明确编辑 B、保存，并知晓何时结束编辑。触发：Profiles Content 高亮 Office 按 `e`。实际：行加 `edit context`、footer 提示 “Editing Profile Office”，仍是同一列表；`state_.editing_profile_id` 除显示/清理外没有作为修改门槛、草稿归属或保存目标使用。重命名 `r`、核心选择 Config→Core、模块编辑各自独立，`e` 本身不打开设置/字段或草稿。与用户本轮 “e/Edit 建立编辑上下文；编辑草稿与查看/使用分离” 的预期存在语义落差，可能让用户以为后续操作处在单一 Profile 编辑事务中。证据：【运行验证】T1 `e` 画面和停止状态；【代码证据】`app.cpp:287-300,887-896,1577-1599,2549-2573`，全文件 `editing_profile_id` 引用仅限赋值/标签/清理。绕过：直接到具体页完成每个操作，不依赖该标签；这仍要求用户理解分散保存边界。此项不声称后端缺少 Profile.save：RPC 已有。

**I-06｜中｜Nodes 把光标高亮称为“Selected”，与保存选择共用词。** 任务：确认当前要用哪个节点、哪个节点已保存、哪个在运行。触发：Nodes Content 用上下键移动而不按 `s`。实际代码用 `selected_` 绘制表格高亮和 `Selected: <name>`（`app.cpp:1253-1265`），另用 State 列 `◆ Save`/`● Run` 和 `s` 写入持久选择（`:1206-1219,960,3823-3828`）。前者其实是“光标所在项”，后者才是“已保存目标”；同词提高误操作和误读风险，尤其当前行与 Save/Run 不同时。【代码证据】；本轮未单独捕获 Nodes 该状态的 PTY 当前格。【待验证】用户在不同状态下是否能通过 State 列充分消歧。绕过：查看 State 列及 Overview 的 Saved/Actual。

### 8.2 任务流程/前后端能力衔接

**I-04｜中｜TUI 无法编辑、暂停/恢复或移除已有订阅。** 任务：修正 URL/名称、暂停自动刷新、删除来源。触发：已有来源进入 Subscriptions Content。实际可见/实现动作只有详情、添加、更新、Profile 绑定、历史清理；`subscription.replace/remove` Server 已有，CLI `sub edit/remove` 可用（`src/cli/subscription_commands.cpp:37-83`），TUI `app.cpp:995-999,1326-1379` 无对应入口或 Prompt。违反“管理全局订阅仍能完成正常人工任务”的任务完成条件；此缺口在**前端**，不是后端接口不足。【代码证据】。绕过：退出 TUI 用 CLI，获取 source ID 后操作；若 URL 含秘密，不应将其放入审计输出。本轮没有在 TUI 按键证实“无法通过隐藏键”之外的所有可能入口，因此结论仅限当前事件分支和可见菜单完整检索。

### 8.3 信息呈现与错误恢复

**I-02｜高｜CLI 多个成功操作在普通模式完全静默。** 任务：脚本/人工创建方案、连接/停止并确认实际结果。触发：隔离 `profile create --name Human-only --core <id>`、`connect '审计 Home'`、`disconnect`，无 `--json`。实际均 exit 0、stdout/stderr 空；`profile create` 不给新 ID，`connect` 不给实际 Profile/核心/目标/confdir 或健康限制，用户需要再发 `list/status`。根因：`src/cli/print.cpp:282-516` 的普通模式没有 profile.create、connection.connect、connection.disconnect 等结果分支，默认只返回退出码；JSON 模式则打印结果。违反“操作成功后能知道改变了什么”的任务完成条件。【运行验证】第 6、7 节；【代码证据】`print.cpp:282-293,408-516`。绕过：加 `--json` 或紧跟 `profile list/status`；不能据空白本身判断是哪一个对象已生效。

**I-03｜高｜daemon 不可达时，TUI 的“空库存/首用”与“未知”混在一起。** 任务：断连后判断是否需要启动 Server、库存是否真的为空。触发：`xrm --socket <已消失临时socket> tui`。实际仍画 Overview 的 “First use”、`Inventory: 0 subscriptions · 0 nodes`、`CORE unknown`，页尾才有 `Cannot load nodes: 连接xrmd失败…`；没有清楚把整个页标成不可用或指出启动 `xrmd` 的动作/地址。源码 `refresh_profiles` 在 RPC 错误时清空 `profiles_`，`refresh_modules` 也清空，`refresh_status` 保留旧状态并写通用消息（`app.cpp:1970-1975,2694-2697,3384-3389`）。违反 `docs/requirements.md:89` 对 Server 不可达的说明义务；用户可能把暂时断连误判为空库并尝试首用。证据：【运行验证】T5 当前格；【代码证据】上述行。绕过：CLI `status` 对该 socket 明确返回 3；再启动正确测试/生产 Server 后刷新。未测在已有 TUI 会话中 daemon 突然断开后所有页的状态。

**I-11｜高｜节点/组 JSON 编辑在保存失败后仍关闭，当前输入不可恢复。** 任务：修正本地节点的原生子对象或 Group JSON，遇到无效文本/保存错误后继续修正。触发：Nodes 详情按 `e` 或 Groups 按 Enter 打开 JSON 编辑，输入非法 JSON，然后 Ctrl-S；Server 拒绝也走同一分支。实际 `save_editor()` 构造失败结果并写错误消息，但随后**无条件**设置 `editing_=false`、清除节点/组编辑标志并刷新列表；重新打开取回已保存对象，当前编辑文本不再可达（`src/tui/app.cpp:3776-3803`）。相对地模块保存失败在 `:3809-3813` 保留草稿。普通 Esc 对节点/组也不走未保存确认（`:542-551` 只保护模块）。违反本轮“保存失败后用户输入保留、Esc 一次只关闭当前层且未保存稿不被静默丢弃”的任务条件。证据：【代码证据】上述明确分支；本轮未做此特定 PTY 输入序列，【待验证】实际屏幕文案和重开后的文本。绕过：编辑前自行复制文本，失败后重新输入；不能把这视为正常恢复路径。

**I-07｜中｜80×24 最低尺寸下关键 footer 被截断。** 任务：在 Modules 编辑时看清保存键、原生源文件和状态。触发：80×24、Modules/log Visual 编辑或 JSON 未保存。实际 Visual 帮助末尾是 `Ctrl-…`，JSON 未保存时文件路径为 `modules/00_log.jso…`；`src/tui/app.cpp:1631-1658` 把整句/路径经 `short_utf8` 截至终端宽度，行内无后续展开。违背“全局状态、路径、模式、通知与快捷键帮助在底部且关键内容可见”的本轮约束；新用户无法仅凭 Visual 当前格辨认 Ctrl-S。【运行验证】T2；【代码证据】`app.cpp:1625-1658`。绕过：切到 JSON 模式、扩大终端或查帮助/文档；本轮未验证帮助弹窗在该模式是否完整包含保存键。

**I-08｜中｜CLI 全局 `status` 的 saved target 作用域不明显。** 任务：只保存节点选择后确认它是否仍在。触发：Home 中 `select node` 成功，核心未运行，再执行 `status`。实际 `saved_target:null`，随后从同一 Profile 成功连接到所选节点，证明先前的保存并未丢失；Server `runtime_status()` 仅在 `status.active` 存在时才取该 Profile 的 saved target（`src/server/application.cpp:1259-1263`）。CLI 默认状态又打印 `saved target: null` 而不说明“仅针对运行 Profile”（`src/cli/print.cpp:442-454`）。这不是选择丢失，而是无 Profile 参数的状态作用域；会使“保存但尚未连接”的用户误读。证据：【运行验证】T4 连接前/后 JSON；【代码证据】上述行。绕过：`profile show <ref>`；运行后 `status` 会有活动 Profile 的保存目标。后端的全局 status 设计限制了 CLI 此处可直接给出的上下文。

### 8.4 文档/测试证据

**I-09｜中｜人工验收脚本仍教已被删除的导航键。** 任务：让用户按正确路径复验 G5。触发：照 `docs/tui-manual-acceptance.md:7` 按 `1–7`/`[]` 切页；当前实现已在 TopNav/SecondaryNav 消费这些键且不导航（`app.cpp:807-843`，UX 焦点测试）。`docs/requirements.md:285` 同样保留数字快捷键说法，但本轮用户和 `docs/xrm-tui-interaction-repair.md:84-95` 明确取消。实际结果是验收者可能把按键无效误判成新缺陷，或遗漏新层级路径。证据：【文档约定/代码证据】，本轮未按旧清单做逐项人工演练。绕过：按本轮约束和第 5 节 Tab/Enter/Esc 路径。此项是现状偏差；本轮按授权不修改规范或测试。

问题根因汇总：I-01 属于 Content 内仍有跨页运行分支；I-05/I-06 属于状态名称与实际数据/动作未对齐；I-04 属于前端任务入口缺口；I-02/I-07/I-08 属于结果/作用域/提示表达不足；I-03 属于错误与空状态混合；I-11 属于失败后草稿生命周期不一致；I-09 属于旧验收文本未同步。**未将框架替换、配色、边框喜好或尚未复现的焦点穿透列为缺陷。**

## 9. 必须保留的能力、兼容性与重构边界

- 【文档约定+代码证据】保留 C++23、Xray-only、Manager/Server、独立 Xray 进程和 `xray run -confdir` 多文件输出。节点与订阅库存独立于 Profile 模块和运行目录；`modules/` 原生 wrapper 与 bindings 分开；15 个根键完整保留，未知字段/原生类型不能因前端简化而丢失（`AGENTS.md:5-45`，`docs/requirements.md:37-53`）。
- 【运行验证】保留 CLI 的显式 Profile ref、精确名称/UUID、`--socket`、`--json`、可引用 ID、非交互退出码与 stderr 错误；`select` 只保存、`connect` 才应用；脚本可使用现有命令树及 `--json` 单行结果。仓库有 `tests/integration` 中的 CLI-01/CLI-03 和 E2E 调用，**证明仓库测试依赖这些参数与输出**；无法证明外部用户脚本的具体使用量，故改动命令兼容性需谨慎。
- 【运行验证/代码证据】保留 TUI 七页、Nodes 来源/Local/Groups、Config 核心与设置、Overview 真实统计、Logs 暂停/有界缓冲、Modules 15 key、单视图 Visual/JSON 共用草稿、未保存确认、外部编辑器、Profile View/Use/Run 分离、80×24 基础可用。现有表格 `TableView` 用 FTXUI 的 Unicode glyph 宽度计算（`src/tui/table_view.hpp:78-102`），可复用；现有 ID 保持和 viewport 测试可复用。
- 【代码证据】TUI/CLI 都直连 RpcClient/Server；不要为了共用命令处理器让 TUI 退化为 CLI 的交互模型。现有 RPC 对订阅编辑/移除、Profile clone/rename/remove、模块变更、config check 等已有能力；I-04/I-05 主要是前端表达/流程问题。I-08 的无活动 Profile 状态作用域则涉及现有 `runtime.status` 合同，后续需决定怎样展示，不要把 `null` 误作数据丢失。
- 【运行验证】fake-core 可继续用于 argv、进程和失败路径；真实 Xray 流量、配置接受度及 G5 用户人工复验不能由 fake-core 代签。现有 `tests/tui/acceptance/acceptance_tests.cpp` 的 tmux 当前格与真实 RPC 检查，适合作为后续回归基座。
- 本轮**不提出新设计稿、命令树、框架迁移或后端重写**。下一阶段验收至少要用隔离 PTY 检查每层键路由、在不同页面按 c/d 的实际副作用、View/Use/Edit 与保存/运行的状态、断连与空库的区别、CLI 成功/失败的 stdout/stderr/码、80×24 和宽屏当前格，并用真实 Xray 补足代理流量与完整配置检查。此处是验收条件，不是实现方案。

## 10. 未验证事项与后续需要的工具能力

【待验证】没有真实 Xray 二进制作为本轮固定输入：`command -v xray` 无输出，定向 `ctest` 的 E2E-06 明确 SKIP。因此不能确认真实核心的 Xray 配置解析、端口/权限、代理流量、远端连通、完整故障恢复和运行文件热切换。没有请求真实订阅、下载官方核心、启用 TUN、改变系统网络或接触生产 daemon。

【待验证】未对所有 TUI 页面/所有弹窗制作逐键交叉矩阵；特别是 I-11 的节点/组保存失败实际屏幕、Profile 编辑未保存时切目标、CLI/TUI 同一来源编辑冲突、后台响应抢焦点、TUI 会话中 Server 突然断开后恢复、外部编辑器的各种退出码、长 URL/长路径、CLI 大量结果、屏幕阅读器、真实交互 TTY 颜色/管道格式、机器 JSON schema 长期兼容性，仍需定向环境与输入。现有测试对其中若干情形有覆盖，但本报告不把未本轮跑过的用例写成 PASS。大列表 1205 节点与 80×24 中文/emoji 测试本轮已跑，但没有人工滚动全部 1205 行。

后续工具能力需求（**只列能力，不选型或安装**）：可建立完全隔离的临时 socket/数据库/confdir/测试 daemon；能在真实 PTY 注入 Tab/Shift-Tab/F 键/Ctrl-S、读取**当前格和样式**、调整 70×20/80×24/120×40/更宽尺寸、等待指定 UI 状态、同时查询 CLI/RPC 和文件/进程事实；能在固定真实 Xray 与 loopback fixture 下验证代理请求，同时不触碰用户实例；能分别捕获 stdout/stderr/退出码与 TTY/非 TTY 输出。本机 tmux 与现有测试框架已证明其中大部分终端能力可用；终端像素截图和外部 MCP 连接尚未验证。设计 Agent 不应假定 Figma/浏览器工具能取代 PTY 的最终屏幕与进程观察。

### 附录 A：当前 CLI 命令树（`--help` 核对）

```text
xrm [--socket PATH] [--json]
  server info
  core register | list | remove | releases | install | use | stop | restart
  node add | list | show | edit | replace | copy | remove | enable | disable
       | alias | labels | history preview/confirm | test | test-results | test-cancel
  sub add | list | show | edit | update | remove
  profile create | list | show | sources set
  config modules | show | edit | enable | disable | statistics enable
         | env set/unset | version range | geodata schedule/assets
         | api services | metrics listen | log level
         | render | check | dump | apply | export
  group add | list | show | edit | remove | select | override
  select node | group
  connect | disconnect | status | logs | operation get
  geodata list | add | update | bind | remove
  routing preset plan | save
  settings show | set | editor set/clear
  stats [--watch]
  tui [--profile ID]
```

此树是现有程序输出的子命令名称压缩记法，不是建议的新语法；具体位置参数与选项以每个 `--help` 和 `src/cli/*_commands.cpp` 为准。实测 `config show --module-key log --source source Office` 成功，而 `config show Office log --source source` 返回 109。帮助显示 `[OPTIONS] profile_id [module]`，因此这里记录参数位置行为与实测错误，不单独列为违反既定要求的缺陷。

### 附录 B：复现保护与命令记录

探针文件在 `/tmp/xrm_audit_probe.py`，只写独立临时目录，含明确 `--socket`/`--database`、测试 fake-core、tmux 会话、finally 清理；它是本轮临时产物，**不属于项目源码/测试**。报告已内嵌关键输出，阅读者不必依赖该临时文件。没有创建新的 Skill/MCP，未修改产品代码、现有配置、测试和规范。测试失败/通过的实际命令已在第 7 节列明；现有构建目录只被读取和执行。

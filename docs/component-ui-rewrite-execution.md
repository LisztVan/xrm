# 组件界面重写执行记录

状态：组件重写、七页迁移、CLI 配套和下列工程验证已完成。G5 仍待用户人工复验，未标记 PASS。

基线为 master / 561dede3c2e8e9e38a39d07a52076c9f08e3eeb0。开始时存在已修改的 TUI/CLI/后端/测试/文档及未跟踪 Skill/MCP 文件。本轮保护它们，tracked.patch、untracked.tar.gz、status.txt 与旧 app.cpp 保存在 build/component-rewrite-baseline/，没有 reset/clean/提交/推送。源码替换前先读旧能力、RPC 与原生保存边界。

用户未能提供五张原截图，明确要求自行启动与截图。本轮按提示词九项反馈实施，并自行保存隔离终端当前格；不会声称看过缺失原图。

项目 Skill 与 tui-design Skill 正文已读取，宿主已发现两项 Skill。tmux MCP 在当前宿主已发现，首次 list_sessions 报专用 socket 未启动，随后 create_session/list_windows/list_panes/execute_command/capture_pane/get_command_result 均通过宿主真实调用成功（与上一轮“宿主未加载”不同）。最终体验脚本 tracked command f67312d0-dbc8-48f9-bc6f-1eaf5692be8e 经侧通道确认退出 0；沿用现有配置，不安装插件。所有产品验证使用独立实例和临时 HOME/XDG/socket/database/confdir，真实订阅/TUN/生产服务不在范围。

## 实现归属与组件复用

| 层 | 文件 | 实际调用 |
|---|---|---|
| 启动 | src/tui/app.cpp | FTXUI屏幕与终端IXON恢复，组装Workspace |
| Shell/状态/Actions | src/tui/shell/* | 七页导航、单一焦点层、nullable Use/Edit、draft、overlay、共用Help动作 |
| 输入/选择/表单 | src/tui/components/widgets.* | Profile/订阅/节点/Group/Core/Geodata/XRM设置/原生对象同屏表单；有限选择不用手写词 |
| 表格/列表 | ui::Table | Profiles、Subscriptions、Nodes、Groups、Core、Geodata、Settings；稳定ID保留光标 |
| 树/集合 | SectionTree/CollectionEditor/StringList | 15模块语义段落；DNS servers/hosts、routing rules/balancers、入/出站及原生数组 |
| 多行/只读 | TextArea/ReadOnlyText | Modules JSON、节点高级编辑、headers、Editor argv、Help和完整详情 |
| 对话/确认 | Form/Dialog/Confirm/HelpOverlay | 全页面modal；表单错误保留值；保存/放弃/继续 |
| 信息/通知 | InfoPane/ToastHost | Shell底部身份/状态；右上角非焦点有界过期Toast |
| 日志 | LogViewer | Logs正文，后端chunk按流组装，底部follow/pause/drop/truncate状态 |
| 原生映射 | src/tui/forms/native_form.* | 共享Draft文本，无RPC；覆盖范围见 module-form-coverage.md |
| RPC薄适配 | src/tui/adapters/rpc.* | 复用原有RpcClient、分页库存读取，不调用CLI handler |

## 删除与保留

旧 src/tui/app.cpp 的 TuiApp 页面绘制、Prompt枚举/submit_prompt、字节光标/退格、通用JSON结构树、旧footer、布尔弹窗事件链已由新组件和Shell替换。不会在生产保留旧/新开关。旧文件仅在忽略的本地回退基线中；旧 `src/tui/table_view.hpp` 已删除且无构建/源码引用。最终 `rg` 核验 `src/tui` 无 `Prompt`、`submit_prompt`、`TuiApp` 或旧表格引用，页面不再处理退格、字符拼接或编辑光标。

保留 xrmd、节点库存、Profile/module源、原生FileSet/confdir、独立核心和现有业务API；未迁移数据库。Profile旧修复、CLI机器合同与单文件CAS继续使用。Source保存不发布运行配置，Use取消不停止核心。

## 研究依据

FTXUI v7.0.1 本地锁定源码决定 Input/Menu/Container/Checkbox/Radiobox API，官网用于组件组合说明；不换框架或重写字符输入。CLI11 v2.5.0 与 clig、GitHub CLI、Docker 采用真实树分级帮助、上下文诊断和流分工，具体证据见 cli-rewrite-evidence.md。Xray全量官方文档和固定v26.9.9源码用于原生表单字段，不将滚动文档视为所有版本保证；详细字段映射见 module-form-coverage.md。

## 构建与证据

集成产物目录 build/component-rewrite，由当前工作树独立 CMake/Ninja Debug 构建，复用 build/dev/_deps 的锁定源码缓存。最终屏幕、按键与数据结果见 [证据索引](evidence/component-rewrite/README.md)，前轮执行记录不替代本轮证据。


### 已替换的旧能力映射

| 原实现/路径 | 最终替代 | 保留的用户任务 |
|---|---|---|
| app.cpp 的 TuiApp 巨型页面、跨页分支、旧事件链 | Workspace + Actions + 七页组件组合 | 全部导航/上下文/任务，只有一套生产实现 |
| Prompt 枚举、submit_prompt、逐字段 Enter | 同一 Form / Field / Button | Profile、订阅、节点、Group、核心、资源、设置的添加与修改 |
| 页面字符追加、字节退格和自制 JSON 光标 | FTXUI Input 及小型行首尾适配 | 中文/多行/光标编辑；不新造输入引擎 |
| 通用 JSON key/type/value Visual 树 | NativeForm 固定原生语义分区与 CollectionEditor | 15 模块及真实原生嵌套路径；高级边界见覆盖矩阵 |
| table_view.hpp | 公共 Table / FTXUI Menu | 稳定 ID 选择、分页库存、长列表、固定列与独立间隔 |
| 旧 footer 动作长条/逐页帮助 | InfoPane + 同源 Action Help | 底部详情/身份，F1 局部帮助，N 有界通知历史 |
| 持久底部通知和混杂日志绘制 | ToastHost / LogViewer | 右上角过期通知；日志正文与底部状态、暂停/跟随 |
| 旧 local-use 特殊确认入口 | Overview → Modules/inbounds 原生集合 | 保留查看/创建本地 SOCKS 的能力；不保留旧固定 10808 预检弹窗，核心仍作真实检查与启动错误报告 |
| CLI 单句根 fallback | CLI11 真实树 formatter 和当前层诊断 | 根/分组/叶发现、参数说明与真实示例 |

基础组件完全不依赖 RPC 方法/Profile/Xray。Shell 维护会话身份和一个模块稿件；页面直接消费已有 RpcClient。持久统计连接从 CLI 私有 header 移至 client/rpc_session.hpp 供两端共用，CLI header 仅转发；SIGPIPE 处理限定当前线程写边界，不全局忽略信号。后台订阅/安装/测试最终结果仍来自 daemon operation。

### 实际验证

以下均使用 `build/component-rewrite` 的独立 Debug 产物。最后实际构建 `xrm / xrmd / xrm_tui_components_tests / xrm_tui_forms_tests / xrm_component_showcase / xrm_tui_acceptance_tests / xrm_m1_tests / xrm_real_core_tests / xrm_release_e2e_tests` 成功；`git diff --check` 无问题。

| 检查 | 实际结果与证据 |
|---|---|
| 组件展示与测试 | FTXUI 真实事件、80×24 展示；单/多行、中文、只读、未知枚举、表单错误、集合、Toast、日志、窄屏通过。独立 Demo 不作为页面验收替代 |
| 原生表单测试 | 15 模块只读打开、未知字段/类型/未设置值、DNS/routing 表单与集合回归通过 |
| 42 个旧终端验收迁移 | 全部场景有分批运行与受影响复验的通过证据，**没有声称单次最终全量调用**。23 前半、9 尾部详见 [迁移说明](tui-acceptance-migration.md)；10 中段最终批次 9 通过，外部编辑器错误通知换行匹配修正后该例单独通过。未跳过业务测试换取通过 |
| 终端主流程脚本 | `python3 tests/tui/rewrite/terminal_regression.py --build build/component-rewrite --output build/component-rewrite/terminal-final` 通过；22 组当前屏幕与完整轨迹。Use 取消、Edit/只读、CRUD/顺序、失败留稿、通知出现/消失、日志、15 模块和同进程缩放 |
| Config 真实 PTY | `python3 tests/tui/rewrite/config_pty.py --build-dir build/component-rewrite --xray /tmp/xrm-g1-xray-v26.9.9/xray --evidence build/component-rewrite/config-pty-final` 通过；Core/Geodata/Tests/Appearance/Editor/只读链的 21 个操作记录 |
| CLI R10/R11 | `python3 tests/cli/cli_rewrite_test.py --xrm build/component-rewrite/xrm --xrmd build/component-rewrite/xrmd --xray /tmp/xrm-g1-xray-v26.9.9/xray --evidence build/component-rewrite/cli-final.json` 通过；实际捕获 250 场景，含根/分组/叶、输出、watch flush/Ctrl-C、消费者关闭、daemon 断开。见 [样本](cli-rewrite-evidence.md) |
| M1 集成 | `build/component-rewrite/tests/integration/m1/xrm_m1_tests --reporter compact` 全部 15 用例通过，含机器接口/原生多模块/新 UI 与统计读取。见 [原始结果](evidence/component-rewrite/m1-final.txt) |
| 固定真实 Xray 门禁 | `XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray ctest --test-dir build/component-rewrite -R '^(RC-04|RC-05|RC-07|E2E-06)$' --output-on-failure -j1` 四项通过，无 SKIP |
| 真实 TUI→核心→流量 | `python3 tests/tui/rewrite/real_core_terminal.py --build build/component-rewrite --xray /tmp/xrm-g1-xray-v26.9.9/xray --output build/component-rewrite/real-terminal` 通过。TUI DNS/routing 表单保存前后检查未发布运行文件；明确 Connect 后多文件 confdir，curl 经 SOCKS 得到 loopback 响应；退出 TUI 后相同核心 PID 仍运行，然后只停止隔离实例 |

固定核心实际版本为 26.9.9，SHA-256 `c4ae6798c38e0e5343b192406746333cd0ba7ff3eb984f8c4b9939dcb68c3f8a`。各产物摘要与真实流量回执见 [证据目录](evidence/component-rewrite/README.md)。这里的通过只涉及固定输入与 loopback，不声称所有协议或公网远端连接已验证。

回归实际发现并修复了：组件 ActiveChild 焦点、内容高度挤走底部、按 chunk 读日志、暂停后 frame 自动居中漂移、表格长版本粘连下一列、长表单服务端错误滚到不可见区域、无效 JSON 行列被长错误前缀遮住、Geodata validator core 回退、订阅部分导入计数字段、非 Overview 页运行状态刷新。日志暂停保留有界快照，新数据单独接收，恢复跟随才发布到该视口。

### 体验与明确边界

```sh
python3 scripts/tui-lab.py --build build/component-rewrite \
  --core /tmp/xrm-g1-xray-v26.9.9/xray \
  --record /tmp/xrm-component-lab.json
```

已通过当前宿主 MCP 实际启动、捕获并正常退出。可省略 core 参数后在 Config 登记。脚本只清理自身临时环境，测试回归保留的 `/tmp/xrm-rewrite-ui-*` 目录在各 trace 的 cleanup 中有完整身份；没有触碰生产数据或宽泛杀进程。

- Visual 为明确维护的原生语义表单，不是全 Xray 动态 schema。协议专属复杂 settings/finalmask 等明确标记为高级 JSON；未知合法类型保留，详见 [覆盖矩阵](module-form-coverage.md)。
- 应用内选区、剪贴板、撤销栈、完整 BiDi 排版未实现；复制使用终端选择，多行字符/粘贴交给 FTXUI。并非声称全 Unicode 排版验收；边界见 [输入记录](component-rewrite-terminal.md)。
- Source 文件名由 API 返回为 Profile 相对路径；详情完整显示该路径。实际 Runtime 路径来自 Server confdir，不猜测远端文件位置。
- 本轮未下载公网发行资产，也未改变 TUN/系统网络/真实订阅；发行版/安装入口使用现有 Server API，fixture 与既有回归覆盖其合同。
- 所有七页已迁移；G5 仍由用户按 [人工清单](tui-manual-acceptance.md) 复验，不由工程测试代签。

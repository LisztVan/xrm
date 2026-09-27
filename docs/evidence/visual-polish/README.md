# Visual polish · current-screen evidence

状态：实现与工程验证完成；**G5 仍待用户体验复验**。

所有 TUI 屏幕来自真实 `build/component-rewrite/xrm` + 隔离 `xrmd`，由 tmux 捕获当前终端格。`.txt` 为当前格，`.ansi` 保留真实 SGR，`.html` 按当前格呈现，PNG 是对该格呈现的截图，**不是 mockup，也不是 GUI 终端窗口的桌面截图**。截图使用普通 DejaVu Sans Mono，验证无需 Nerd Font 的阅读基线；字体建议另见设计文档。tmux MCP 拒绝了自选 socket，改用直接 tmux 命令，仍保持每次测试独立 socket。

七页视觉 fixture 使用 fake-core **仅产生进程日志**；名称 Process fixture 已明确显示。没有用它证明代理流量。真实 Xray 流量证据另列。所有 URL 为 loopback，无生产订阅、TUN 或系统网络修改。

## 七页与三种尺寸

| 页面 | 80×24 | 120×40 | 180×48 |
|---|---|---|---|
| Overview | [PNG](80x24-overview.png) · [当前格](80x24-overview.txt) | [PNG](120x40-overview.png) · [当前格](120x40-overview.txt) | [PNG](180x48-overview.png) · [当前格](180x48-overview.txt) |
| Profiles | [PNG](80x24-profiles.png) · [当前格](80x24-profiles.txt) | [PNG](120x40-profiles.png) · [当前格](120x40-profiles.txt) | [PNG](180x48-profiles.png) · [当前格](180x48-profiles.txt) |
| Modules | [PNG](80x24-modules.png) · [当前格](80x24-modules.txt) | [PNG](120x40-modules.png) · [当前格](120x40-modules.txt) | [PNG](180x48-modules.png) · [当前格](180x48-modules.txt) |
| Subscriptions | [PNG](80x24-subscriptions.png) · [当前格](80x24-subscriptions.txt) | [PNG](120x40-subscriptions.png) · [当前格](120x40-subscriptions.txt) | [PNG](180x48-subscriptions.png) · [当前格](180x48-subscriptions.txt) |
| Nodes | [PNG](80x24-nodes.png) · [当前格](80x24-nodes.txt) | [PNG](120x40-nodes.png) · [当前格](120x40-nodes.txt) | [PNG](180x48-nodes.png) · [当前格](180x48-nodes.txt) |
| Logs | [PNG](80x24-logs.png) · [当前格](80x24-logs.txt) | [PNG](120x40-logs.png) · [当前格](120x40-logs.txt) | [PNG](180x48-logs.png) · [当前格](180x48-logs.txt) |
| Config | [PNG](80x24-config.png) · [当前格](80x24-config.txt) | [PNG](120x40-config.png) · [当前格](120x40-config.txt) | [PNG](180x48-config.png) · [当前格](180x48-config.txt) |

## 交互与状态

- [字段验证](form-validation-80x24.png)、[集合输入](form-string-list-80x24.png)、[有限选项](form-select-80x24.png)、[选项改变](form-select-changed-80x24.png)。未聚焦的复杂字段显示摘要，聚焦后展开；验证失败就地保留稿件。
- [Visual](module-visual-updated-80x24.png)、[同一份 JSON 稿](module-json-80x24.png)。原生未知字段 futureNative 保留；保存前后运行文件摘要相同。
- [Help](help-form-80x24.png)、[未保存确认](dialog-unsaved-80x24.png)、[Toast](toast-over-editor-80x24.png)、[Toast 消失](toast-expired-80x24.png)。80×24 只展示最新通知，其余保留于历史；高屏最多三条。
- [120×40 稿件](dirty-resize-120x40.png)、[180×48 稿件](dirty-resize-180x48.png)、[60×18 保护视图](dirty-resize-60x18.txt)、[回到 80×24](dirty-resize-80x24.txt)。同一进程中稿件与焦点保留。
- [日志原始记录](logs-record-detail-80x24.png)、[256 色](logs-256-80x24.png)、[无色](logs-none-80x24.png)、[Paper](logs-paper-80x24.png)、[daemon 断开](disconnected-80x24.png)。断开时 Run 为 unknown，已有记录明确属于缓存状态。
- [完整按键与 RPC 事实](trace.json)、[业务回归轨迹](regression-trace.json)、[Config 操作轨迹](config-steps.json)。

## 逐页 self-critique

| 页面 | 首眼目标与最终判断 | 看屏后修正 / 保留边界 |
|---|---|---|
| Overview | 先看实际运行，再看保存目标；窄屏上下、宽屏并列 | 去掉 process_only 内部枚举与冗长版本 banner；缺少统计条件合并成一行；未知值不当作零。没有数据时留白，不画虚假仪表 |
| Profiles | Profile 名称是主体，Session 列区分 Use/Edit/Run；Cursor 与 View 分开 | 长值有省略标记；宽屏加入选中 Profile 的核心/保存目标预览。只有当前 View 的已读取目标可显示，其他 Profile 明确提示 View to inspect，不伪造 none。少量对象自然留下空间，不添加无关说明填充 |
| Modules | 先识别原生键、Source/Runtime、Edit/Read-only；Visual 展示领域结构 | DNS/routing 加入真实字段预览；导航与详情响应式分布，集合用专属列。复杂/未知字段仍到 JSON，未声称全 Xray schema 表单 |
| Subscriptions | 来源、节点数量、刷新结果、Auto、Bound 可横向扫读 | 未刷新显示 Not refreshed，避免空白状态；Toast 在窄屏限制为最新一条，主体与背景焦点不移动 |
| Nodes | 名称、协议、可用性、Saved/Run 分列 | 来源切换留在二级导航，Groups 保留独立出口；宽屏提供选中对象预览，列表不暴露完整凭据 |
| Logs | 正文最亮；TIME/IPC CURSOR/SOURCE 弱化，Warning/Error 可扫到 | 实际修复来源贴住级别、续行错位和过暗 metadata；宽屏独立 subsystem，窄屏正文前缀弱化。未知行不伪造级别，长 token 换行，原始记录可查看 |
| Config | 当前子页明确属于 XRM，Core/Geodata 仍是各自库存 | 表格版本仅显示版本号；Appearance 字段改人读标签。表单复杂字段未聚焦时摘要化，整页复用同一组件，不变成另一套设置界面 |

跨页结论：主导航、标题、列表表头、选中条、底部状态和浮层遵循同一语言；无常驻动作长条，无卡片堆叠。低数据量 fixture 的空白是剩余工作区，真实长库存使用原滚动模型。宽屏预览不更改 View，不产生 RPC 写入。仅在 focused 控件展示复杂选项，使 80×24 有效空间优先用于当前任务。

## 验证命令与实际结果

```sh
cmake --build build/component-rewrite --target xrm xrmd xrm_tui_components_tests xrm_tui_forms_tests -j4
build/component-rewrite/tests/tui/components/xrm_tui_components_tests --reporter compact
build/component-rewrite/tests/tui/forms/xrm_tui_forms_tests --reporter compact
build/component-rewrite/tests/integration/store/xrm_store_tests '[SETTINGS]' --reporter compact
python3 tests/tui/rewrite/visual_polish.py --output docs/evidence/visual-polish
python3 tests/tui/rewrite/terminal_regression.py --build build/component-rewrite --output build/component-rewrite/polish-regression
python3 tests/tui/rewrite/config_pty.py --build-dir build/component-rewrite --xray /tmp/xrm-g1-xray-v26.9.9/xray --evidence build/component-rewrite/polish-config
python3 tests/tui/rewrite/real_core_terminal.py --build build/component-rewrite --xray /tmp/xrm-g1-xray-v26.9.9/xray --output build/component-rewrite/polish-real
python3 tests/cli/cli_rewrite_test.py --xrm build/component-rewrite/xrm --xrmd build/component-rewrite/xrmd --xray /tmp/xrm-g1-xray-v26.9.9/xray --evidence docs/evidence/visual-polish/cli.json
```

组件 23 cases / 152 assertions、原生表单 10 / 226、设置 2 / 29 全部通过；原始结果见 [组件结果](components-tests.txt)、[原生表单结果](native-form-tests.txt)、[设置结果](settings-tests.txt)。终端主流程、Config 与真实 Xray 流程均通过。CLI 264 个场景通过，含 `--json`/pipe、帮助树、错误与 watch 退出路径；见 [CLI 报告](../../cli-visual-polish.md) 和 [原始输出](cli.json)。后续 TUI-only 改动未改变 CLI 实现，因此复用其验证。

旧测试仅更新与新 presentation 不兼容的定位：DNS 集合现在有 Address 列、Overview 标题已重做、Toast 允许跨行、新装主题为 dark。业务状态、源文件、运行文件及真实流量断言未放宽。

[真实核心证据](real-core-trace.json) 验证 TUI DNS/routing 表单保存、保存不发布运行文件、显式 Connect 产生多文件 confdir、curl 经 SOCKS 获得 loopback 响应、退出 TUI 后同一核心 PID 存活。固定核心为 26.9.9，SHA-256 `c4ae6798c38e0e5343b192406746333cd0ba7ff3eb984f8c4b9939dcb68c3f8a`。不扩展为公网/所有协议/TUN 验证。

## 设计选择与仍需主观判断的部分

选择 Slate developer console 的比较与完整 token/state 规则见 [视觉系统](../../tui-visual-design.md)。仅剩两项偏好值得用户体验判断：默认密度是否要进一步偏紧凑，以及 Slate 与 Paper 哪个更适合长时间使用。它们已有 Appearance 选项，不阻塞当前交付。Maple Mono NF CN 是独立终端配置建议，本机未安装该字体，未伪称做过该字体的实际渲染验证。

隔离体验：

```sh
python3 scripts/tui-lab.py --build build/component-rewrite --core /tmp/xrm-g1-xray-v26.9.9/xray
```

无自动 Connect；实验脚本退出时清理其自己的 daemon/临时目录。常规 `xrm tui` 退出不会停止核心。

## 本轮文件范围

- `src/tui/components/style.hpp`、`widgets.cpp/.hpp`、`log_viewer.cpp`：主题 tokens、统一组件、表单折叠摘要、上下文 Help、响应式 Toast、结构化日志。
- `src/tui/shell/workspace.cpp`、`actions.cpp`：七页布局、导航/状态层次、宽屏检查区、原始日志详情、会话名按显示宽度限长。页面的按钮接同一 style；数据和运行操作仍走原 API。
- `src/tui/forms/native_form.cpp`：原生 Visual 领域预览和集合列；`pages/config.cpp`：主题选项命名。
- `src/infra/store.cpp`：只将新建设置的默认主题设为 dark；已有保存主题不被迁移或覆盖。
- `src/cli/presentation.hpp`、`help.hpp`、`print.cpp`、`geodata_commands.cpp`：CLI 人读呈现。
- 对应组件/日志/设置测试与 `tests/tui/rewrite/visual_polish.py`，以及帮助定位的 PTY 脚本更新。没有新增运行依赖。

[最终构建摘要](artifacts.json) 记录最终产物；各运行 trace 自带实际测试时的产物摘要。CLI/真实核心较早的证据仅复用于其未改变的业务与前端路径。未执行整个仓库的全量测试，不声称所有协议或所有终端模拟器都已验证。

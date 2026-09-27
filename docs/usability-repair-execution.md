# G5 可用性修复追踪与实机证据

本文件记录 2026-09-27 的 UXR-01～13 实施。唯一现行交互契约是 [xrm-interaction-spec.md](xrm-interaction-spec.md)；七份审查输入保留原貌，不据此把 G5 标成 PASS。`docs/g5-usability-three-agent/` 中实际存在 `README.md`，用户所列的 `README(1).md` 不存在；本轮读的是该目录的 `README.md`。其中 B/C 编号交叉处均以原始报告为准。

## 基线和隔离范围

工作树起始 commit `561dede3c2e8e9e38a39d07a52076c9f08e3eeb0`，已有大量未提交和未跟踪的 TUI/CLI 重写文件；没有 reset、clean、commit 或 push。本轮起始 `build/component-rewrite/xrm` SHA-256 `2c8bb9dfc5e41d0e54eb9c15dacdca9d964daf98dd4565b65b95f2a91bb74df5`，`xrmd` `251f3e1102b32a47e6bfe7e58b9481f7ea3c9fb23ae96ad4e42353c803ee210c`；工作树后续构建均重新链接并记录 SHA。固定 Xray v26.9.9 路径 `/tmp/xrm-g1-xray-v26.9.9/xray`，SHA-256 `c4ae6798c38e0e5343b192406746333cd0ba7ff3eb984f8c4b9939dcb68c3f8a`。本轮所有回放使用新建 temp HOME/XDG、数据库、UDS、loopback HTTP/端口与独立 tmux socket；没有使用报告中的共享会话或清理他人的 `/tmp`。

最终构建 SHA-256：`xrm` `4e4fb1c523be27ef3090d154c350bca763ddd343bb99926c7c26ffa87625e9f9`，`xrmd` `103af026302ada08e53507f631ade1ebd4f3394f0d03432d059a662cbc0f7c16`。固定核心与起始时相同；最终真实 tmux 证据与完整 CTest 均使用这组最终构建。

本轮主证据目录为 [uxr-20260927T041541Z-885dda](evidence/usability-repair/uxr-20260927T041541Z-885dda)。起始单键轨迹 `baseline-tui` 记录 Profile Enter 仅改变 View、clean Esc 关闭表单一次、dirty Esc 打开 Unsaved 后下一次 Esc 返回原表单；C 的相反轨迹在此环境未复现，不能因此认定原报告无效。最初尝试的旧回归脚本假设 Enter 默认确认删除、可选字段启用后立即输入，与本轮新合同冲突；只更新了这些有意改变的步骤，没有放宽 Source/运行断言。

## 根因、处理和边界

| 编号 | 处理结果 | 当前依据与限制 |
|---|---|---|
| UXR-01 | **部分修复；争议轨迹未复现**。表单/浮层事件由最上层消费，局部草稿有 Save/Discard/Continue；集合 Ctrl-S 明示只接受父稿、模块 Ctrl-S 才保存 Source。 | 组件回归、独立 tmux 单键基线及七页终端回归；C 的快速 Esc 重开、带稿锁死、Profile Enter 跳页须按原环境继续定点复验，不能写作已修复。 |
| UXR-02 | **已修复**。公共 Select 可见值即表单草稿值；可选字段的 presence 与输入分开，启用后 Tab 进入值；Checkbox/MultiSelect 仅 Space 改值；StringList 新增后清空缓冲并聚焦输入；已有值从末尾编辑，长值仅在输入控件内水平滚动。 | 公共组件回归与 DNS 真实 PTY 表单；黑盒报告的地址前缀插入摩擦据此修复，不会提前保存 Source 或应用运行。 |
| UXR-03 | **已修复并保留原生边界**。DNS/规则表单分区、Help 字段跳转；routing 目标组错误聚焦且修改关联字段清除旧错；无效 JSON 留稿显示位置。 | Native form 回归与终端操作。`webhook`、未知字段和合法类型未删除；没有创造核心默认值。 |
| UXR-04 | **黑盒发现残留后已修复**。集合行/动作焦点、所选摘要、上下方向与边界、删除目标摘要统一；第二项 Tab 不再重置为第一项，Add/Apply 刷新同一父稿预览，子层不直接写 Source。 | 独立黑盒 `94/95` 是修复前失败；修复后的组件回归和 `final-terminal-current/07b`/`07c` 为受指导实机复验。索引仅为 UI 映射，不写入原生 JSON。 |
| UXR-05 | **已修复，仍需盲测可发现性**。各层 Help 给任务/前提、只读恢复与跨页路径，禁用项说明原因；80×24 Help 首屏保留可执行任务。 | 终端 Help 截屏与回归；没有恢复另一套导航或常驻热键。 |
| UXR-06 | **已修复并补实机证据**。区分 endpoint 可达与聚合缓存新鲜度；远端写/生命周期动作禁用，本地 Help/草稿可继续；Refresh 有开始/结果反馈。 | `final-review-terminal-2` 的 `failed-save-keeps-draft`、`reconnected-draft-not-replayed` 当前格和轨迹；未知运行值不伪装为空。 |
| UXR-07 | **已修复**。订阅保留 `last_error`/阶段和部分解析首因，失败不清旧节点；HTTP 失败原因不再含 NUL；URL 添加/修改预检语法，TUI 字段就地报错，真实 URL 可显隐和局部编辑。 | 隔离 IPC+HTTP 回放、持久化重启、CLI/TUI 回归；解析首因不包含 URL token。 |
| UXR-08 | **黑盒发现残留后已修复**。统一人读适配按方法/对象/错误码分流：core.register 非 Xray 与 config.check 失败不同，订阅 HTTP 与对象不存在不同；Stop 后 runtime 文件不存在不再误报 Profile 丢失；原始诊断另可查。 | 适配单测、独立固定核心 CLI/RPC 及盲测 `90/93` 前后对照；wire 码/结构保持。 |
| UXR-09 | **已修复**。页脚分列 Viewing/Next/Module Edit/Running；仅相同 Profile、无 override 且正在运行时比较 Saved/Running；节点同名补来源与短 ID；二级选择按 key 跨页保留。 | 终端当前格与 RPC 状态核对；Use/Edit 仍仅 TUI 会话。 |
| UXR-10 | **已修复**。破坏性 Confirm 默认 Cancel；TUI Stop 先核实实际状态，确认绑定 `expected_run_id`，过期拒绝且不误停新实例。 | native form/组件测试、隔离服务器 stale run 回放、TUI 真实停止流程；CLI 仍非交互。 |
| UXR-11 | **已修复并保留日志事实**。Help/Toast/Dialog 小屏绘制、SectionTree 纵向单视图、日志续行和原始详情、通知历史。 | 80×24/120×40/180×48 实际 ANSI 当前格、resize、日志与主题切换回归；原始 Warning、无时间和 chunk 语义不改写。 |
| UXR-12 | **已修复**。订阅全部失败及部分完成分别在 stdout/stderr 表达；stdout EPIPE 在 watch 时干净退出，IPC/非 EPIPE 仍报错。 | 独立 CLI fixture 记录 producer/consumer/pipefail 各退出码与流。 |
| UXR-13 | **已修复**。订阅精确唯一名称/ID，重名拒绝猜测；config edit 接受互斥 `--text`/`--file PATH` 与显式 `--file -`；TTY 帮助/空结果/回执人读，非 TTY TSV 与 JSON 保持。 | 独立 CLI 真实实例、旧 R10/R11 回归。 |

## 原报告逐项映射

下表编号均在各自文件内局部有效；“映射”给出上表处理结果，纠错或建议不被当成独立已复现缺陷。

| 原文件 | 原条目 | 本轮映射 / 处置 |
|---|---|---|
| `agent-a-final-report.md` | HUMAN-01/02 | UXR-05/06；只读恢复及离线 Help。 |
| 同上 | HUMAN-03/04 | UXR-03；长表单定位/校验，保留 phase2 对已有 Routing target 的更正。 |
| 同上 | HUMAN-05/06 | UXR-07；URL 安全编辑和订阅失败原因。 |
| 同上 | HUMAN-07 | UXR-02；公共控件。 |
| 同上 | HUMAN-08 | UXR-08；错误上下文。 |
| 同上 | HUMAN-09/10 | UXR-06/08；离线与错误恢复。 |
| 同上 | HUMAN-11 | UXR-09；Saved/Running 作用域。 |
| 同上 | HUMAN-12 | UXR-08/13；CLI 真实输出按 TTY/非 TTY 分别验收。 |
| 同上 | HUMAN-13 | UXR-12；原 watch 分歧由独立管道进程码复验。 |
| 同上 | HUMAN-14 | UXR-13；输入与人读格式。 |
| 同上 | HUMAN-15 | UXR-10/13；危险操作与 CLI 界面。 |
| `agent-b-part-a-blackbox.md`、`agent-b-parts-b-e.md` | HUMAN-01～07 | UXR-05/09；保留三层导航，只修说明、作用域、no-op 反馈。 |
| 同上 | HUMAN-08/09/10/12/32 | UXR-02；选择、可选值、Checkbox、子表单 End 按原焦点复验。 |
| 同上 | HUMAN-11 | UXR-03/08；表单定位与错误归因。 |
| 同上 | HUMAN-13/15/16 | UXR-04；集合目标、预览、重排。 |
| 同上 | HUMAN-14 | B 已撤销“Cancel 无保护丢稿”；UXR-01/04 只验现有 guard 和预览。 |
| 同上 | HUMAN-17/18/19/20 | UXR-01/04/05/06/11；只读、焦点、Help 与通知，不逐未知键骚扰。 |
| 同上 | HUMAN-21/24 | UXR-07；URL 与订阅结果。 |
| 同上 | HUMAN-22/23 | UXR-08/09/13；TTY/非 TTY、错误与状态。 |
| 同上 | HUMAN-25～27 | UXR-09/10；确认、作用域与名称。 |
| 同上 | HUMAN-28/29/30/33 | UXR-11；先核对原始日志级别，不为美观改写。 |
| 同上 | HUMAN-31/34 | UXR-01/03/09；表单和焦点、状态。 |
| `agent-c-pasted-report.md` | HUMAN-01～05、13～16 | UXR-05/06/09；可发现性与只读恢复，保留导航。 |
| 同上 | HUMAN-06/07/08 | UXR-01；当前独立基线未复现，仍是待复验疑点。 |
| 同上 | HUMAN-09/10/11/12 | UXR-04/09/10；身份、集合第二项与确认。 |
| 同上 | HUMAN-17/18/19 | UXR-03/07/09/11；表单、状态和订阅 URL。 |
| 同上 | HUMAN-20～24 | UXR-07/08/12/13；其中 C HUMAN-23 是 CLI 失败误报/输出流，不误记成 B 同号。 |
| 同上 | HUMAN-25～32 | UXR-04/05/06/09/11/13；运行作用域、Stats 前提、日志、术语和共享动作路径。 |
| `agent-a-phase1.md` 与 `agent-a-phase2.md` | 第一阶段观察及第二阶段更正 | A 的多数 CLI 调用为非 TTY；Ctrl-A/Ctrl-U 未承诺；URL 编辑持有真实值；删除观察未证明 View 指向删除对象；plain tmux 抓取不足以证明焦点样式。对应误判不采纳为新功能；其余控件实证仍按 UXR 回归。 |

## 验证索引与尚需复验

工程组件：`xrm_tui_components_tests` 最终 39 case/338 assertions，`xrm_tui_forms_tests` 18 case/594 assertions，human-message 8 case/38 assertions。最终完整 [CTest 日志](evidence/usability-repair/uxr-20260927T041541Z-885dda/ctest-full-final-clean.log) 375 项中 350 passed、25 skipped、0 failed（163.65 秒）。黑盒反馈后的首次完整回归曾有两项失败：把已有值光标放末尾时误影响多行 JSON 和只读订阅详情；限定为单行输入后，两项定点和最终完整并发回归均通过，失败过程保留在 `ctest-full-after-input.log`。25 个跳过项需要 CMake 的 `XRM_TEST_XRAY`，且其中有固定端口旧用例，本轮没有为取得绿色数字而让它们与独立端口要求冲突。固定 v26.9.9 Xray 已通过独立随机 loopback 实机链。服务端诊断的独立构建、6 组测试及真实 IPC/HTTP 回放见 [diagnostics-927a/manifest.md](evidence/usability-repair/diagnostics-927a/manifest.md)。CLI 66 次独立观察和旧 CLI 277 场景见 [CLI README](evidence/usability-repair/cli-20260927T041558Z-4e31afd1/README.md)。七页 tmux 当前格/ANSI/HTML、按键、Source/运行文件哈希见主证据目录的 `final-terminal-current`；固定真实 Xray 链、主题与 Config 专项分别见 `final-real-core-3`、`final-visual-3`、`final-config-2`。这些证据不等于真人盲测，也不证明互联网流量。

当前已知尚未在相同条件复现的是 C 的 Esc 重开/带稿锁死/Profile Enter 跳页；无法以另一个 clean Esc PASS 宣告它们已修。固定真实核心无效原生 inbound 的错误归因见 `final-core-errors/trace.json`，离线 dirty 草稿见 `final-review-terminal-2`，集合第二项黑盒操作见 `blind-operator`。原始报告中的建议与撤销误判未被偷偷当成必改缺陷：不恢复常驻快捷键面板，不合并 Use/Edit/Run，不让 TUI 会话选择持久化，不把 IPC chunk 游标伪造为日志记录号，不将 Ctrl-A/Ctrl-U、真实 IME、完整注释保真承诺为本轮能力。

## V01～V19 当前验收矩阵

以下“通过”只覆盖所列的实际条件；组件断言、PTY 当前格、IPC/Source/runtime 分别给出不同层面的依据。`final-*`、`baseline-tui`、`blind-operator` 均位于本轮主证据目录；每个终端目录保存 `trace.json` 及相应 `.txt`、`.ansi` 或 `.html` 当前格。

| ID | 当前结果 | 实际操作与证据 |
|---|---|---|
| V01 | 通过 | Select 切换立即更改 draft、Tab/Ctrl-S 提交由组件与 native form 回归覆盖；Profile core、routing target 和 Group 使用同一控件；独立 Group 补测实际切 Manual、勾选节点和选择 Manual member，保存后重开及 CLI 回读一致。`final-terminal-current/trace.json` 的 Visual/JSON/Source 与运行哈希核对。 |
| V02 | 通过 | 公共可选字段 Space 启用、Tab 输入、Home/End、关闭/重启用与 omitted/显式值序列化由组件/native form 回归覆盖；真实 inbound PTY 两个旧用例核对中间字段值。 |
| V03 | 通过 | Checkbox Enter 不变、Space 改值，输入框 `q/e/u/c/d/p/?` 被控件消费，多行 Enter 换行；组件回归和完整 CTest 的 TUI 输入用例。 |
| V04 | 通过 | routing 缺目标就地报错并可修正；无效 JSON 留稿，取消/修正后的其它字段保留；`final-terminal-current/05-dns-invalid-form-keeps-values.*` 和 `blind-operator` 当前格。 |
| V05 | 修复后通过；黑盒原缺陷单列 | [独立黑盒报告](evidence/usability-repair/uxr-20260927T041541Z-885dda/blind-operator/operator-report.md) 的 `94/95` 确证第二项 Tab 误跳第一项。修复公共 CollectionEditor 后，`final-terminal-current/07b-dns-second-row-before-tab.*`/`07c-dns-second-row-after-tab.*` 显示焦点进入 actions 且保持第二项；组件回归还核对 Enter 真正编辑第二项、Shift-Tab 返回。native collection 与真实终端另覆盖 copy/move/remove/boundary。此复验由修复者执行，不冒充第二次盲测。 |
| V06 | 通过 | 子层 Apply 更新父稿、Visual/JSON 同一 draft、显式保存前 Source 不变、保存不改 Running；`final-terminal-current/trace.json`、`final-real-core-3/trace.json` 中 Source/运行文件哈希与屏幕。 |
| V07 | 部分；原 C 轨迹未复现 | `baseline-tui/trace.json` 逐键记录 clean/dirty Esc、Confirm/Help/继续；组件测试最上层消费事件。C 的快速 Esc 重开、带稿锁死和 Profile Enter 跳页缺少原相同环境条件，不能由本轮另一条 PASS 抵消。 |
| V08 | 通过；盲测结论另记 | `final-review-terminal-2` 七页、表单、Dialog 的 Help 当前格；只读前提和跨页路径在 80×24 首屏可见，未添常驻动作面板。`blind-operator` 自行用 Help 找到 Edit 和模块路径。 |
| V09 | 通过 | `final-review-terminal-2/failed-save-keeps-draft.*`、`reconnected-draft-not-replayed.*`：断线保存失败保稿、恢复后不自动重放；首次离线 Help/缓存当前格和 Refresh 反馈同目录。 |
| V10 | 通过 | `diagnostics-927a/live_rpc.json` 隔离 HTTP 503/部分解析/重启后 last_error/旧节点保留；组件/HTTP 回归覆盖 URL 无效、连接拒绝和超时；CLI 全失败与部分成功的流见 CLI trace。 |
| V11 | 通过 | `final-review-terminal-2/url-masked.*`、`url-revealed-synthetic.*` 使用虚构 token；显隐不写稿，局部编辑/取消/保存在同轨迹；普通列表与通知不回显凭据。 |
| V12 | 通过 | `final-core-errors/trace.json` 独立固定 Xray 实测非核心可执行、未选核心、真实 Xray 拒绝原生 inbound 三种不同原因；`--verbose` 原文及 `--json` 错误码逐项记录。盲测还发现 Stop 后 `config show --source runtime` 把无运行文件误说成 Profile 丢失；人读适配已修，固定核心隔离 CLI 用例核对错误、已保存 Profile 仍在及 wire code 不变。 |
| V13 | 通过 | `final-review-terminal-2/view-edit-a-highlight-use-b-running-a.*` 与 `final-terminal-current/03-use-cancelled-core-running.*` 核对不同 Profile 作用域；同名节点加来源/短 ID；真实 Group 成员由 `blind-operator/group-followup` 的 TUI 回读、CLI `group show` 与 `node list` 中相同 ID 核对；该补测没有 Connect，未声称运行 Group。 |
| V14 | 通过 | Confirm 默认 Cancel、Esc 一次消费、显式确认在组件与 TUI 测试；真实 Stop 当前格见 `final-real-core-3/06-real-xray-stopped.*`；`diagnostics-927a/live_rpc.json` 的过期 run_id 被拒绝且新实例仍在。 |
| V15 | 通过 | `final-review-terminal-2` 保存四 Toast/通知历史、表单遮挡、Help/Dialog 与 120×40→80×24→60×18→80×24→180×48 的原始当前格；`final-visual-3` 七页在三种尺寸和主题下有 ANSI/HTML。 |
| V16 | 通过 | `final-terminal-current/17-logs-paused.*`、`final-real-core-3/04-real-xray-logs.*`/`05-real-xray-log-detail.*` 与日志组件回归；真实和合成来源明确，未知时间、级别、chunk 语义不伪造。 |
| V17 | 通过 | [CLI 独立证据](evidence/usability-repair/cli-20260927T041558Z-4e31afd1/README.md) 逐进程记录 TTY/非 TTY/JSON、`watch | head`、真 IPC 断开、SIGINT、`/dev/full` 的 stdout/stderr/退出码。 |
| V18 | 通过 | 同一 CLI 独立证据记录唯一名称、重名拒绝、ID、`--text`/`--file PATH`/`--file -` 与坏输入；保存文件哈希核对无部分写入。 |
| V19 | 独立黑盒主链完成；三处摩擦已修并受指导复验 | [独立操作报告](evidence/usability-repair/uxr-20260927T041541Z-885dda/blind-operator/operator-report.md) 保存只依赖界面/Help 的逐键轨迹、12 次 TUI Help、17 次 CLI Help、ANSI/当前格、stdout/stderr/各进程码。它完成 Edit、DNS 第二项取消/排序、routing 缺目标修正、订阅失败恢复、daemon 恢复、固定 Xray Connect/Logs/Stats 入口/Stop、CLI ref/file/watch；新的独立 lab 后续补测 Group 新建、成员身份和保存作用域；发现 V02 旧值光标、V05 第二项 Tab 和 V12 停止态错误，均随后修复。Stats 因未启用 stats/policy/metrics 而明确显示不可用，盲测未取得数值；Group 补测与第二项修复复验均作为后续受指导任务单列；此轮不能宣称独立黑盒全绿或 G5 PASS。 |

**未复现：** 原报告 C 的快速 Esc 重开、带稿锁死、Profile Enter 跳页在本轮基线条件未出现；用户先前报告的主题切换 SIGSEGV 在当前隔离构建的 dark/light/default 与 Config PTY 操作中也未复现，原崩溃只有进程退出回溯而无可定位的栈帧；黑盒快速组合 `Esc Tab Tab` 曾使 Help 延迟关闭，单独 Esc 后恢复，证据在操作报告 `03`，没有把它包装为已修。**受阻/未覆盖：** 真实 IME 候选窗、盲测的 Stats 数值（fixture 未启用 stats/policy/metrics）、互联网代理稳定性、完整 CTest 中需固定 Xray 的 25 个条件 SKIP。新黑盒仍有轻度摩擦：Group 的 F 详情只显示 ID/摘要，成员需重开 Edit 表单或用 CLI `group show` 查看；这一点未在本轮扩大为另一套详情设计。固定核心测试只在隔离 loopback 上证明本轮相关 Xray 原生配置链；原始误判和建议的“不采纳”原因见上表及原文件映射。未提交、未推送，G5 仍由用户验收。

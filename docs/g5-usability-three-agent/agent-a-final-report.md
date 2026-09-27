# XRM 独立可用性测试报告

## 测试边界与方法

- 黑盒阶段没有读取文档、规范、源码、测试或既有评审。
- 有效会话：独占 tmux server `xrm-usability-a`，120×40 启动；命令为 `python3 scripts/tui-lab.py --build build/component-rewrite`。
- lab 使用临时 socket/database/HOME/XDG；只停止本轮精确 PID，没有访问真实订阅、真实 Xray、TUN、防火墙或用户数据。
- 最初误连到预先存在且被并发修改的 `xrmux`；发现无操作却出现 Profile 后，立即判定污染。该会话全部观察作废，不进入本报告。
- 有效黑盒记录先保存到 `/tmp/xrm-usability-a/phase1.md`，之后才读取 `docs/xrm-interaction-spec.md`、项目 Skill、当前源码与 execution records。第二阶段记录在 `/tmp/xrm-usability-a/phase2.md`。
- 测试完成后已关闭本轮 TUI/tmux 和精确启动的 daemon；证据文本保留。

# Part A — Black-box human simulation

## 1. Persona A journey — 第一次使用 XRM

### 第一眼

- 第一眼像 Xray 的运行与 Profile 状态面板。
- `[Overview]`、`stopped`、Saved target、Next Connect、Inventory 很容易看到。
- `F1 Help & actions` 容易发现；退出方式要进入 Help 才知道。
- footer `View / Use / Edit / Run` 最初像动作与状态混合，实际操作后才理解为四个独立状态。
- stopped 时仍显示 `! Saved target differs from the running target`，把预期差异表现成警报，造成不必要紧张。

### Profile journey

- Tab→Profiles、Enter→Content 的分层导航可学会，但进入 Content 主要靠 footer 的 `TopNav/Content` 变化提示。
- Profile CRUD、Use、Edit 全部只在 F1 中发现；页面没有动作入口。
- Create 表单控件类型清楚，`UNSAVED *` 清楚；保存 Toast 明确 `runtime unchanged`。
- View 只改变底部 `View ...`；若当前已经 View，Enter 完全无反馈。
- Use/Cancel Use 的行状态、footer 和 Toast 一致，且明确不会改变运行核心。这是本轮最清楚的状态反馈之一。
- Copy 默认名称合理；Delete 显示目标名称。
- Ctrl-A/Ctrl-U 的预期失败属于测试者误用，不算产品缺陷；产品只承诺 Home/End 等键。

### READ ONLY → Edit journey

- Modules 的 `SOURCE · READ ONLY · Visual`、原生键、源路径非常清楚。
- DNS servers 只读集合只有 View/Done，不能编辑的原因一眼可见。
- 在只读集合内打开 F1，Help 只解释 View 和 parent draft，没有说明“回 Profiles 开启 Edit”。从当前页面无法恢复。
- 只能依赖此前 Profiles Help 偶然看到的 `e Allow module Edit`，退出集合、退出 Modules、回 Profiles、开启 Edit，再回 Modules。
- 开启 Edit 后 Profile 行、footer、Toast 和 Modules `SOURCE · EDIT` 四处一致，模型变得清楚。

## 2. Persona B journey — 熟悉 Linux/Xray，首次使用 XRM

### DNS

- 集合页像“管理 DNS servers”：表格 + View/Add/Edit/Copy/Delete/排序。
- Add 表单一次显示 15 个原生字段；presence/omitted 表达准确，但简单添加 `1.1.1.1` 也像在填写配置结构。
- Form Help 很强：解释 object draft → module Source → Connect runtime，并给 Ctrl-S。
- Add/Edit/Delete/Reorder 均可完成；删除确认清楚说明只改 draft，Source/runtime 不变。
- Visual→JSON→Visual 保持数据与顺序，心智连续。
- 输入集合后文本仍留在 New domain 输入框，存在重复 Add 风险。

### Routing

- Overview 把 ordered rules、native balancers、domain strategy 分开，信息架构正确。
- Routing rule 表单有 18 个外层字段和多个复合控件；domain/ip/protocol 等原生语义解释清楚，protocol 使用有限 checkbox。
- Tab 成本很高，field 计数不等于按键数；Ctrl-End/PgDn 在复合控件内部没有达到预期。
- 缺 Target 时错误保留输入并说清 `select an outbound or balancer`，但错误没有把焦点/视口送到 target。测试者在长表单里错过了实际存在的 outboundTag/balancerTag。
- 黑盒任务未完成；第二阶段确认目标控件存在，所以“功能不存在”不是结论，真正问题是错误恢复与长表单导航。

### Subscription

- 列表 Source/Nodes/Result/Auto/Bound 很清楚；Help 完整覆盖 Add/Edit/Update/Pause/Bind/Remove。
- `not-a-url` 可以直接保存，失败被推迟到 Update。
- Update 保留来源、焦点和旧库存；Toast 不抢焦点。
- 失败只显示 `partially completed; inspect source result`。Enter 无详情；F Details 显示 UUID、socket、last_result 等内部信息，却不显示 URL 或具体下载错误。
- Edit 表单默认掩码 URL；黑盒测试者不知道如何揭示，只能盲改。第二阶段确认 Ctrl-R 可揭示真实值，但它只在 URL 获得焦点后通过 F1 才可发现。
- Pause/Resume、Bind MultiSelect、Remove 引用/节点处置都清楚且可完成。

### Node / Group

- Nodes 二级导航中 `Scope: Groups` 可发现。
- 空 Groups 页面没有 inline Add，仍需 F1。
- Group 表单对 mode/strategy/scope/nodes/sources/protocol 使用有限控件，不要求手打关键词。
- 空 Name 错误就地且保留输入；修正后错误直到再次提交仍存在。
- Select 显示 `‹ Random ›`，自然暗示 Left/Right，但 Left/Right/Enter/Space 没反馈；Help 泛称 Arrows。
- MultiSelect、保存、再次编辑保留值均正常。

## 3. Persona C journey — 熟练用户

### 高频导航与错路

- Profiles 找不到 Connect；F1 也没有“去 Overview Connect”的指路。
- Overview Help 中 Connect/Stop、saved-only 和 stats 前置条件非常清楚。
- Logs Help 明确说 `No core lifecycle actions here`，对“在 Logs 找 Stop”的错路帮助最好。
- Nodes/Config 的 Help 不会把“Profile Use / Xray Modules”错路引回正确页。
- Nodes 二级范围离开再回来会回到 All；熟练用户若没看 1/4，容易把 Group edit 误操作为 node edit。

### Run / Logs

- Connect 缺 Core 时错误非常可操作：选择注册 core，必要时 register binary。
- Config 默认 Core，空状态与 Register action 可发现。
- `/bin/true` 被拒绝时，错误错误地说“Xray rejected candidate configuration / correct saved configuration”，把无效 binary 诊断成配置问题。
- 黑盒命令未提供 test core，且禁止查文档/源码、禁止启动真实 Xray；因此不能安全完成真实 Connect、运行状态、真实日志、统计和运行后 Stop。
- Logs 的空状态、Following/Pause/End 恢复与 Help 文案可验证；3–5 分钟滚动、metadata、warning/error 定位、pause 后新增日志等无法评价。

### Resize

同一进程、同一 Test Group、dirty `XTest Group`：

- 120×40：完整背景与 Form。
- 80×24：Form 可用，背景信息减少。
- 60×18：专用 `Terminal too small`，准确报告尺寸和 80×24 要求，并说明 draft/focus preserved。
- 返回 80×24、再到 180×48：dirty、field 1、对象、选择均保留。
- 180×48：主要增加背景/空白，Form 固定宽；不坏，但利用率有限。

### 输入控件

- TextInput 指定 ASCII 序列最终得到 `^abcdefghijkXnop$`，Left/Right/BSpace/Delete/Home/End 正常。
- 输入 `qeucdp?` 不触发全局动作。
- UTF-8 `测试中文输入ABC` 显示和宽度正常；没有真实 IME composition，不能宣称 IME PASS。
- TextArea 的 Enter/Up/Down/Home/End/Ctrl-Home/Ctrl-End 均通过标记验证；快捷键字符仍是正文。
- Checkbox/MultiSelect/CollectionEditor 均有代表性操作；Select affordance 问题见上。

### Chaos

- daemon 使用中消失：数据保留，Run=unknown，footer `xrmd disconnected · cached data`；优秀。
- daemon 返回：自动恢复、刷新缓存、保留选择；优秀。
- dirty Form 断线保存：输入、焦点、dirty、所有选择保留；错误就地可理解；重连后稿件仍在；优秀。
- 但断线 F1 仍把 Add/Edit/Delete/Save&Connect 显示成可执行动作，没有 disabled reason；Refresh 无反馈。
- daemon 未启动首屏正确显示 unknown/unavailable/stale，而不是 0；但 F1 不告诉如何启动/连接 xrmd。Last diagnostic 只有中文 errno 与 socket。

## 4. Task completion table

| Task | Completion | First correct action | Actions | Wrong | Help | Page transitions | Errors | Recovery | Confidence |
|---|---|---:|---:|---:|---:|---:|---:|---|---:|
| 1 Profile | Yes | ~1s | ~30 | 1 | 2 | 1 | 1 user input mistake | Partial | 4/5 |
| 2 READ ONLY→Edit | Yes | immediate recognition; recovery not discoverable | ~18 | 0 | 1 | 4 | 0 | Partial, prior knowledge needed | 5/5 after return |
| 3 DNS | Yes | ~2s | ~31 | 3 | 2 | 0 | 1 malformed draft | Yes | 4/5 |
| 4 Routing | No | ~2s | ~50 | 3 | 1 | 0 | 1 validation | Discard only | 1/5 |
| 5 Subscription | Yes | ~2s | ~25 | 1 | 1 | 0 | 2 intentional update failures | CRUD yes; diagnosis no | 4/5 CRUD, 1/5 error |
| 6 Node/Group | Yes | ~4s | ~19 | 4 key guesses | 1 | 0 | 1 intentional validation | Yes | 4/5 |
| 7 Run | No | ~3s | ~20 | 1 wrong page | 3 | 3 | 2 core/register errors | Partial | 1/5 |
| 8 Logs | Partial | immediate | 4 | 0 | 1 | 0 | 0 | N/A | 2/5 |
| 9 Help | Partial/varied | F1 always visible | — | — | many contexts | — | — | varies | 3/5 |
| Resize | Yes | immediate | 5 resizes | 0 | 0 | 0 | 0 | Yes | 5/5 |
| CLI Profile discovery | Yes | command 3 | 3 commands | 0 | 2 help levels | — | 0 | Yes | 4/5 |

## 5. All valid FRICTION / FAIL observations

### HUMAN-01
- Persona: A/B/C
- Task: 基础动作发现与错路
- Screen: Profiles / Groups / Overview / Nodes / Config
- Before action: 基础 CRUD/Connect/Add 应在当前任务附近可发现，或错页给方向。
- Action: 浏览页面后打开 F1。
- Actual: 动作集中在 F1；很多错页没有“去哪里”的提示。
- Human reaction: 可完成，但频繁依赖 Help 和记忆页面归属。
- Recovery: F1 + 返回顶层 + 找另一页。
- Could recover: Partially
- Result: FRICTION
- Severity: High
- Type: Discoverability / Navigation
- Evidence: Profile、Groups、Overview、Nodes、Config Help 终端轨迹。

### HUMAN-02
- Persona: A
- Task: Module READ ONLY→Edit
- Screen: Modules / DNS servers / Help
- Before action: Help 应说明怎样解除 READ ONLY。
- Action: 在只读 collection 内 F1。
- Actual: 只解释 View/parent draft，没有 Profiles/Edit 路径。
- Human reaction: 在正确功能区仍被卡住。
- Recovery: 依赖先前偶然知识离开 Modules。
- Could recover: Partially
- Result: FAIL
- Severity: High
- Type: Discoverability / Navigation / State model
- Evidence: `/tmp/xrm-usability-a-task2-readonly-help.txt`。

### HUMAN-03
- Persona: B
- Task: DNS / Routing forms
- Screen: 15/18 field native forms
- Before action: 常用字段应先完成任务，高级项不妨碍导航。
- Action: Add DNS / Add routing rule。
- Actual: 完整字段和复合控件一次展开；Tab 成本高，field count 不等于 key count。
- Human reaction: 像操作配置结构，容易失去位置。
- Recovery: F1、Ctrl-S、反复 Tab。
- Could recover: DNS Yes; Routing Partially
- Result: FRICTION
- Severity: High
- Type: Form / Efficiency / Information hierarchy
- Evidence: `task3-add-form.txt`, `task4-add-form.txt`。

### HUMAN-04
- Persona: B
- Task: Routing validation
- Screen: Routing rule form
- Before action: 缺 target 的错误应定位 target。
- Action: Ctrl-S。
- Actual: 底部 form-level 错误；焦点仍在 domain，未跳转 target。
- Human reaction: 知道缺什么，不知道到哪里修。
- Recovery: 长距离 Tab；测试未完成。
- Could recover: Partially
- Result: FAIL
- Severity: High
- Type: Error recovery / Form
- Evidence: `/tmp/xrm-usability-a-task4-validation.txt`。

### HUMAN-05
- Persona: B
- Task: Subscription update failure
- Screen: Subscriptions / Details
- Before action: 失败应说明 URL/网络/HTTP/解析原因和下一步。
- Action: Update、Enter、F Details。
- Actual: `partially completed; inspect source result`；Details 只有 UUID/socket/internal keys，没有具体错误。
- Human reaction: 无法诊断或有把握恢复。
- Recovery: 盲改 URL 重试。
- Could recover: No（原因不可得）
- Result: FAIL
- Severity: High
- Type: Error recovery / Information hierarchy
- Evidence: `task5-update-fail.txt`, `task5-failure-detail.txt`。

### HUMAN-06
- Persona: B
- Task: Edit failed subscription
- Screen: Edit subscription
- Before action: 授权编辑应能看到真实 URL，或明显提供 reveal。
- Action: e。
- Actual: URL 显示 bullets；页面无 reveal hint。能力存在于 field-focused F1/Ctrl-R，但未自然发现。
- Human reaction: 不敢局部修改，担心保存 bullets。
- Recovery: 记住原长度后整段替换。
- Could recover: Partially
- Result: FRICTION
- Severity: Medium
- Type: Discoverability / Form
- Evidence: 黑盒终端轨迹；第二阶段 `widgets.cpp` Ctrl-R。

### HUMAN-07
- Persona: B
- Task: Group Select
- Screen: Add/Edit Group
- Before action: `‹ Random ›` 应支持 Left/Right 或明确按键。
- Action: Left/Right/Enter/Space。
- Actual: 无反馈；实际 Radiobox 主要用 Up/Down。
- Human reaction: 怀疑控件禁用或只有一个值。
- Recovery: 保留默认。
- Could recover: Partially
- Result: FRICTION
- Severity: Medium
- Type: Form / Feedback
- Evidence: `/tmp/xrm-usability-a-task6-group-form.txt` 与终端轨迹。

### HUMAN-08
- Persona: C
- Task: Register core
- Screen: Config / Register Xray core
- Before action: 非 Xray executable 应报 binary/version validation error。
- Action: 注册 `/bin/true`。
- Actual: 提示修正 saved candidate configuration。
- Human reaction: 被引导去改无关配置。
- Recovery: 无；放弃表单。
- Could recover: No
- Result: FAIL
- Severity: High
- Type: Error recovery / Terminology
- Evidence: Task 7 terminal screen。

### HUMAN-09
- Persona: A/C
- Task: daemon unavailable startup
- Screen: Overview / Help / Last diagnostic
- Before action: Help 应告诉如何启动/检查 xrmd。
- Action: F1 → Last diagnostic。
- Actual: 状态 unknown/unavailable 表达正确；Help 无恢复动作，diagnostic 只有中文 errno + socket。
- Human reaction: 知道服务不可达，不知道下一步。
- Recovery: 普通用户无法从产品恢复。
- Could recover: No
- Result: FAIL
- Severity: High
- Type: Error recovery / Help
- Evidence: `/tmp/xrm-usability-offline-start.txt`, `offline-help.txt`。

### HUMAN-10
- Persona: C
- Task: daemon disappears
- Screen: Nodes cached state / Help
- Before action: 写动作应 disabled 并说明重连；Refresh 应反馈结果。
- Action: F1、r。
- Actual: 写动作仍像可执行；r 无反馈。
- Human reaction: 容易误以为离线也能保存，或反复按 Refresh。
- Recovery: daemon 返回后自动恢复。
- Could recover: Partially
- Result: FRICTION
- Severity: Medium
- Type: State model / Feedback / Error recovery
- Evidence: `daemon-disappeared.txt`, `daemon-reconnected.txt`。

### HUMAN-11
- Persona: A
- Task: First launch
- Screen: Overview stopped
- Before action: stopped 时 Saved target 与 Actual none 是正常。
- Action: 观察首屏。
- Actual: `! Saved target differs from the running target`。
- Human reaction: 误以为配置异常或需要修复。
- Recovery: 后续 Help/操作才理解。
- Could recover: Yes
- Result: FRICTION
- Severity: Medium
- Type: State model / Feedback
- Evidence: 初始 Overview capture。

### HUMAN-12
- Persona: C
- Task: CLI errors
- Command: missing Profile/Node/Core、daemon unavailable
- Before action: 默认 stderr 应先给任务原因和恢复，原始诊断按需查看。
- Action: 执行错误命令。
- Actual: Reason/Recovery/Next 很好，但默认追加 socket、中文 wire 原文、errno、UUID wording。
- Human reaction: 可恢复，但信息层级退化且中英混杂。
- Recovery: 复制 Next 命令。
- Could recover: Yes
- Result: FRICTION
- Severity: Medium
- Type: CLI / Information hierarchy / Terminology
- Evidence: CLI transcript in phase1 report。

### HUMAN-13
- Persona: C / script user
- Task: Pipeline
- Command: `xrm [--json] stats --watch | head`
- Before action: head 关闭后 producer 安静结束或按正常 SIGPIPE 行为退出。
- Action: head 读取 2/4 行。
- Actual: 数据及时 flush，JSON 每行合法；producer stderr 报 io_error 并 exit3。
- Human reaction: 常见采样 pipeline 在 pipefail 下失败。
- Recovery: 忽略 exit/stderr 或改脚本。
- Could recover: Yes
- Result: FRICTION
- Severity: Medium
- Type: CLI / Efficiency
- Evidence: CLI pipeline transcript。

### HUMAN-14
- Persona: C / script user
- Task: Batch config edit
- Command: `xrm config edit --help`
- Before action: 大 JSON 可从 stdin/file 输入。
- Action: 查看叶帮助。
- Actual: 只支持 inline `--text JSON_TEXT`。
- Human reaction: 长 JSON shell quoting 笨拙。
- Recovery: 自行命令替换/转义。
- Could recover: Yes
- Result: FRICTION
- Severity: Medium
- Type: CLI / Efficiency
- Evidence: CLI help transcript。

### HUMAN-15
- Persona: C
- Task: CLI logs/stop
- Command: `xrm logs`, `xrm disconnect` while stopped
- Before action: 空日志应可区分；已停止 Stop 应报告 no-op。
- Action: 执行命令。
- Actual: logs 空 stdout exit0；disconnect 说 `Stopped running core`。
- Human reaction: 难以区分空结果/异常；误以为 Stop 刚产生影响。
- Recovery: 再查 status。
- Could recover: Yes
- Result: FRICTION
- Severity: Low
- Type: CLI / Feedback
- Evidence: CLI transcript。

## 6. Wrong paths taken

| Wrong path | Product response | Result |
|---|---|---|
| Profiles 找 Connect | Help 无 Connect，也不指向 Overview | Navigation friction |
| Modules 内找 Edit | Page-level Help 有前置条件；collection-level Help 丢失 | FAIL in tested context |
| Logs 找 Stop | Help 明确 no lifecycle actions | Good recovery |
| Nodes 找 Profile Use | Help 只有 Saved/Connect，未指向 Profiles | Friction |
| Overview 找 Add node | Help 无动作/无 Nodes 指路 | Friction |
| Config 找 Xray Modules | Config 二级结构无 Modules，也无跳转提示 | Friction |
| Nodes 返回 Groups | 二级范围重置 All，误入 node edit | Recoverable friction |

## 7. Recovery behavior

**表现好：** validation/断线保存留稿；subscription update 不清旧库存；daemon cached/unknown；自动重连；resize 保持 dirty/focus；删除/移除 dialog 通常解释 Source/runtime 后果。

**表现差：** subscription 最终错误不可达；offline Help 无下一步；disconnected Refresh 静默；routing 错误不定位字段；core registration 错误分类错误；跨页依赖需要用户自己记忆。

## 8. Screenshots / terminal evidence

没有生成像素截图；使用真实 PTY 当前屏幕文本，不用 stdout 重绘历史代替当前格。

- Black-box log: `/tmp/xrm-usability-a/phase1.md`
- Spec/source analysis: `/tmp/xrm-usability-a/phase2.md`
- Resize: `/tmp/xrm-usability-a-resize-{120x40,80x24,60x18,80x24-return,180x48}.txt`
- Offline: `/tmp/xrm-usability-offline-{start,help}.txt`
- Daemon: `/tmp/xrm-usability-a-daemon-{disappeared,reconnected}.txt`
- Disconnected save: `/tmp/xrm-usability-a-disconnected-save.txt`
- Input: `/tmp/xrm-usability-a-input-{text,chinese,textarea}.txt`
- Task evidence: `/tmp/xrm-usability-a-task{1..6}-*.txt`

注意：plain tmux capture 不保留 ANSI/inverse，不能用这些文本证明颜色/按钮焦点不可见。

## 9. CLI discovery transcript

```text
xrm
→ root groups + examples + Next
xrm profile
→ create/list/show/clone/rename/remove/sources
xrm profile list
→ non-TTY TSV, UUID first
xrm profile show 'Lab Home'
→ accepted readable name
xrm node → xrm node list
xrm config → xrm config edit --help → modules/show
xrm status / connect / logs / stats / disconnect
```

Profile 列表发现需要 3 条命令、2 层 Help。根帮助和叶帮助总体优秀。黑盒大多数 CLI 命令由非-TTY capture 执行，因此 UUID-first TSV 属于 pipeline contract，不能当成人类 TTY 缺陷。TTY human table 在第一阶段未独立评价。

## 10. Things I could not determine

- 真实 Core connect/run/stop、实际 Running target、真实流量。
- Logs 3–5 分钟滚动、warning/error 扫读、raw detail、pause 时新增数据、真实 metadata。
- stats 有真实采样时的可读性。
- OS IME composition、候选窗、粘贴/选区。
- ANSI 颜色/反显下的按钮焦点质量。
- CLI TTY 表格的人类可读性（非 TTY 与 watch PTY 已测）。
- 真实订阅网络；只用了无效和 loopback-failure URL。

# Part B — Root causes

## ROOT-01 — F1-only action model lacks cross-page wayfinding
- Affected: Profile CRUD, READ ONLY→Edit, Connect, empty Groups, all wrong-path tests.
- Evidence: HUMAN-01/02; Help screens.
- Why humans struggle: 用户必须先知道动作存在、知道要按 F1、再记住动作属于哪一页。走错页时多数没有“去哪里”。
- Frequency: Very high
- Severity: High

## ROOT-02 — Complete native forms overwhelm navigation and recovery
- Affected: DNS, Routing, Select, composite collections, validation.
- Evidence: HUMAN-03/04/07.
- Why: 15–18 outer fields、复合焦点站、通用 Tab 和 form-level error 使常见任务与高级字段竞争注意力。
- Frequency: High on module edits
- Severity: High

## ROOT-03 — Human error layer drops task-specific facts, then exposes internals
- Affected: subscription failure, core register, offline TUI, CLI errors.
- Evidence: HUMAN-05/08/09/12.
- Why: generic mapping replaces concrete reason；Details/default stderr 再用 UUID/socket/raw language 填充，却不提供真正恢复信息。
- Frequency: Every significant failure
- Severity: High

## ROOT-04 — Disconnected state rendering is mature, action availability is not
- Affected: daemon startup/disappearance/refresh/write actions.
- Evidence: HUMAN-09/10 and disconnected-save PASS.
- Why: data layer正确保留 cache/unknown，action/help 层却没有把 `fresh_` 作为 disabled prerequisite。
- Frequency: Every disconnect
- Severity: High

## ROOT-05 — Hidden capabilities and misleading affordances
- Affected: URL reveal, Select, focus/context controls.
- Evidence: HUMAN-06/07.
- Why: capability exists but only在 field-focused Help；visual glyph 和实际 keys 不一致。
- Frequency: Common forms
- Severity: Medium

## ROOT-06 — CLI machine compatibility is strong, common shell ergonomics remain rough
- Affected: watch/head, config edit, raw diagnostic, no-op/empty feedback.
- Evidence: HUMAN-12–15.
- Why: machine schema/TSV preserved correctly, but consumer-close policy、inline JSON、default diagnostics optimize explicit contracts over ordinary shell habits.
- Frequency: Medium
- Severity: Medium

## ROOT-07 — Expected target differences are presented as warnings
- Affected: first launch/stopped Overview, status wording.
- Evidence: HUMAN-11.
- Why: Saved/Actual distinction is correct, but severity does not account for stopped state.
- Frequency: Every stopped Profile with saved target
- Severity: Medium

# Part C — Spec/implementation analysis

| Finding | Classification | Basis |
|---|---|---|
| READ ONLY collection Help lacks Edit path | C implementation violation | Overlay Help bypasses module prerequisites; contract requires prerequisites/disabled reasons. |
| Disconnected write actions enabled | C implementation violation | actions() does not gate on daemon freshness. |
| Subscription batch error unavailable | C implementation bug | operation result contains failed error, TUI reduces it to generic partial and summary does not persist it. |
| Core register error says config invalid | C implementation bug | all `core_check_failed` share one connection-oriented human mapping. |
| Routing error not adjacent/focused | C implementation/component flaw | validator returns form-level string instead of target field error. |
| Offline Help no recovery | C implementation/Help gap | state is correct; next action absent. |
| Basic actions only in F1 | B contract-induced usability issue | Sole interaction contract intentionally removes action strips and centralizes executable Help. |
| Edit in Profiles / Connect in Overview | B information architecture tradeoff | Contractual page ownership, but weak wayfinding causes friction. |
| 15/18-field forms | B interaction design flaw | Contract requires complete native semantic forms; current presentation lacks progressive grouping/jump. |
| URL bullets | D discoverability | Real value retained; Ctrl-R reveal exists only in field-focused Help. |
| Routing target “missing” | E tester misunderstanding + C recovery issue | Controls existed at fields 5/6; tester overshot; validation still failed to guide. |
| Ctrl-A/Ctrl-U expectations | E tester misunderstanding | Not promised; terminal help documents supported keys. |
| Button focus invisible | F evidence limitation | Plain capture stripped ANSI/inverse; source renders focused buttons distinctly. |
| UUID-first profile/node output | E intentional pipeline behavior | Commands were non-TTY; contract preserves ID-first TSV. |
| Core/Logs end-to-end unavailable | F environment limitation | lab launched without optional fixed core; real core prohibited. |
| watch consumer-close exit3 | B intentional tradeoff | Execution record explicitly tests it; still poor `| head` ergonomics. |
| CLI raw diagnostic default | B terminology/information hierarchy flaw | Contract requires reachability, not necessarily default prominence. |

自动化记录中的 PASS 不取消这些结果。尤其：现有证据覆盖“Help 能打开”和“URL 能揭示”，但没有证明第一次用户能发现；覆盖 disconnected state，但没有证明 Help 会禁用动作；覆盖 subscription operation envelope，但没有证明失败原因对人可见。

# Part D — Highest-return fixes

1. **让 Help 直接解决当前阻塞。** READ ONLY collection 内提供可执行“Go to Profiles / Enable Edit for X”；Profiles 无 Connect 时指向 Overview；wrong-page Help 给目标页。一次修复消灭最多 discoverability/navigation friction。
2. **让 action availability 感知连接状态。** offline/disconnected 时写动作保留在 Help 但 disabled，显示原因和重连条件；Refresh 必须 Toast 成功/失败。
3. **统一任务级错误保真。** subscription batch failed 的具体 URL/network/HTTP/parse error进入 row/detail/notification；默认不丢失原始 cause。
4. **把 validation 定位到字段。** routing target 错误绑定 outboundTag/balancerTag，自动滚动/聚焦；其他复合字段同样处理。
5. **给原生长表单提供常用/目标/高级分区跳转或折叠。** 不删字段、不改 JSON 模型；让 DNS address 和 routing match→target 在少数动作内完成。
6. **按操作分类 human errors。** core.register 的 binary validation 与 config check 分开；无效 URL 在保存前给就地 syntax error；恢复建议指向真实对象。
7. **把秘密字段揭示能力放在字段旁。** 例如 `URL [hidden · Ctrl-R reveal]`；授权 edit 仍持有真实值，避免把 bullets 当数据。
8. **修正 Select affordance。** 支持 Left/Right 循环，或把 glyph/Help 改为 Up/Down；单一选项明确 disabled/only choice；修正值后及时清旧错误。
9. **降低“正常差异”的告警级别。** stopped 时把 Saved target/Actual none 描述为 `Next Connect`，不用 `! differs`；disconnect already stopped 输出 `Already stopped; saved settings unchanged`。
10. **改善 shell 边界。** consumer EPIPE 对 `| head` 安静退出；原始诊断移到 `--verbose`/diagnostic 命令；`config edit` 增加 stdin/file 输入，同时保持现有 TSV/JSON schema。

# Part E — Human acceptance recommendation

## Not ready for human G5

原因：

1. READ ONLY 的实际阻塞场景中，Help 无法告诉用户如何继续。
2. subscription 下载失败的具体原因在人类界面中不可达，错误无法恢复。
3. daemon 不可达时虽然状态表示优秀，但 Help/动作可用性与恢复路径不完整。
4. routing 常见规则在长表单中的 validation recovery 仍会让领域用户卡住。
5. core registration 错误会把用户引向错误修复对象。
6. 本轮因无固定 test core，真实 Run/Logs/Stats 主链没有得到独立黑盒人类证据。

这不是 G5 判定，只是是否适合进入人工 G5 的建议。建议先修复 ROOT-01、ROOT-03、ROOT-04 和 routing target 定位，再用带固定隔离 core 的 lab 重跑 Run/Logs 主链。

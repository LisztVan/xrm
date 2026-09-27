# XRM 交互契约

状态：2026-09-27 可用性修复阶段的唯一有效交互契约。产品与原生配置边界仍见 requirements.md、architecture.md、AGENTS.md。旧审计及旧执行结果是历史证据，不定义当前交互。G5 仍待用户人工复验。

## 状态与保存边界

Cursor（列表高亮）、View（查看 Profile）、Use（后续运行目标，可为空）、Edit（唯一允许模块写入的 Profile，可为空）、Draft（对象/源基线绑定）、Saved（Server 设置）、Running（Server 实际实例）分别建模。Unknown 不显示成零，失败保留旧缓存并标 stale/unavailable。运行中不等于流量可用。

Profiles 的 u 对光标 P 切换 Use：原来不是 P 则设 P，原来是 P 则清空。取消不停止核心、不清保存目标、不关闭 Edit。需要 Use 的动作无目标时禁用，不能回退到 View/Cursor/Run。

Profiles 的 e 是模块编辑模式开关。开启 P 时同时把 View 设为 P，不改变 Use/Run；关闭或切换前必须处理草稿（保存、放弃、继续）。保存失败保留模式/目标/稿件。Profile 元数据管理使用独立的 Metadata 操作，正常创建、改名、复制、删除不受模块 Edit 限制。

Modules 的所有写入口共用 `Edit == View && Source` 门槛：Visual 字段、集合增删改复制重排、JSON、Include、预设、外部编辑器导入、保存均遵守。Runtime 永远只读。查看空/停用模块不创建源或启用。此门槛仅属 TUI 会话，CLI 的显式写动作不要求 TUI 开 Edit。

一个模块一个 DraftSession，保存原始文本/基线、合法 JSON 与错误位置；Visual 只修改负责字段，保留未知字段、合法类型及数组顺序。F2 在同一全宽工作区切 Visual/JSON，非法 JSON 保留文本/光标并阻止切回 Visual。子表单确定只更新父稿；保存模块才写 Source；显式 Connect/Apply 才发布运行文件。保存、订阅更新、安装核心、退出 TUI 都不隐式运行。

## 导航、焦点和输入

顶层固定 Overview、Profiles、Modules、Subscriptions、Nodes、Logs、Config。Modules、Nodes、Config 有二级导航。TopNav → SecondaryNav（若有）→ Content：Tab/Shift-Tab 只切当前层，Enter 进入并消费，Esc 只退一层。TopNav q 退出。无数字、[] 或方向键跨主导航捷径。

表单/对话框建立局部焦点域。Tab/Shift-Tab 在字段与按钮间移动；已有单行值的编辑光标默认在末尾，长值在窄屏由输入控件自身水平滚动，字段标签仍可见。文本框方向键、Home/End、Backspace/Delete 属于控件，单行上下不泄漏，多行 Enter 换行。q/e/u/p/?/空格是文本，不能触发背景动作。FTXUI Input 的字符处理为唯一基础，页面不得自行拼字符、退格或管理编辑光标。

Select 上方向键与 Left/Right 立即切换当前**表单草稿值**；失焦和 Ctrl-S 无第二次隐形提交，Enter 消费但不保存。Checkbox/MultiSelect 使用 Space 改值，Enter 不改值也不触发表单提交。可选值未设置时显示“未设置/字段省略”，Space 启用 presence 后下一次 Tab 进入值输入；关闭 presence 只影响原生序列化，暂存候选值可留在本地草稿。行列表 Enter 查看，按钮 Enter 执行，三层导航 Enter 进入下一层；这些按焦点区分。

F1 从任何上下文打开统一 Help 并保留原控件/光标；? 仅在非文本上下文作为别名。Help/确认 Esc 不穿透。保存使用明确 Save/Cancel 按钮与 Ctrl-S，终端期间关闭 IXON 并在退出恢复。选区/复制/粘贴支持范围必须记录，不能声称不存在的能力。

## 组件与布局

所有页面复用稳定生命周期的 Input、TextArea、Select、Checkbox/MultiSelect、List/Table、Tree/Sections、CollectionEditor、Form、Dialog、Help、Toast、LogViewer、InfoPane。组件不依赖 RPC/Profile/Xray。Render 无写 RPC、无稿件修改或任务启动。

顶部为导航；主体为业务列表/表单/日志；底部为查看对象详情、模式、状态及路径，只有一个轻量 F1 Help & actions 入口。常驻快捷键长条、列表后附加的 Plan/Core/Sources 说明、正文里的调试游标全部删除。Help 是可操作的上下文菜单：F1 只打开，方向键选择、Enter 执行一次并消费、Esc 关闭恢复原焦点。动作、键位、目标、启用条件和后果来自同一真实 Action 定义；别名合并，不可执行项不伪装成可执行操作。必要前置条件与保存层级在 Help 说明区显示。输入中的 ? 仍是文字，不恢复常驻动作栏或右侧动作/Inspection。

二级导航显示完整当前项、左右续项及当前位置/总数，Tab/Shift-Tab遍历顺序不变；Nodes范围与来源分组标识。所有尺寸对象详情归底部，长详情通过只读详情层访问；View身份不跟随高亮改变。路径与名称按显示宽度带省略标记截断，完整值可达。

80×24 为最小可操作尺寸，120×40 与更宽尺寸约束列宽与阅读宽度；底部预算最多 4 行，长详情用受控展开/滚动对话框。小尺寸保护视图恢复后保留稿件/焦点/滚动。颜色仅增强语义，不要求 Nerd Font。

通知独立叠加于内容右上角，不挤动布局、不抢焦点；信息/成功 3 秒、警告 5 秒、错误 8 秒；最多同时 3 条，重复合并；少于 30 行时仅展示最新一条，其余保留在通知历史。浮层由Shell统一单边框、背景和居中定位，整矩形不透明；Paper及深色覆盖空白与resize新增单元格。Dialog/Help高于通知；模态或可写JSON输入获得焦点时延后通知并暂停过期，离开后继续显示；重复次数注明含义、历史入口在Help。错误仍保留于相关表单/对象状态。订阅只在后台最终结果确认后报告成功/部分失败/失败，超时结果未知不可自动重放。

## 页面及动作合同

| 页面 | 内容和操作 | 保存/运行边界 |
|---|---|---|
| Overview | 实际核心/目标、保存目标、统计/资源、Connect saved、Stop、完整运行路径 | Connect 必须有 Use；Stop 只操作 Server 自己实例 |
| Profiles | Table；Enter View 更新底部；u Use 开关；e Edit 开关；n 新建、c 复制、R 改名、m 元数据、D 删除 | Profile 管理不依赖 Edit，不隐式连接 |
| Modules | 15 原生键，Source/Runtime、Visual/JSON、Include、预设、外部编辑器 | 所有写入口受同一 Edit 门槛，单文件保存与冲突留稿 |
| Subscriptions | 有限列宽 Table；添加/查看/编辑/更新/批量更新/绑定/暂停/移除及历史清理 | 刷新只改库存；移除显示多 Profile 引用与节点处置 |
| Nodes | 来源/Local/Groups；搜索、详情、增删改复制导入、启停、测试、保存目标 | Cursor/Saved/Running 分开；Save & Connect 先保存再连接，失败分别说明 |
| Logs | 正文仅日志；跟随/暂停/历史/原始详情 | 无 c/d 生命周期动作；时间/级别/来源保持原事实 |
| Config | Core、Geodata、Tests、Appearance、Editor、General；相同 Form/Table | XRM 设置不进 Xray 模块；安装/登记不启用 |

空输入在未聚焦时仍有可辨载体。字段标签、可选性、说明及就地错误相邻；Select 固定布局并可辨认，屏幕上显示的选项就是表单草稿值。无选项须写明缺少的引用，仅一项应明确说明；未知已有枚举保留。可选布尔用未设置/true/false一个选择器；可选文本/数值/集合的 presence 与值分离，absent/false/0/空字符串/空集合不合并。集合标签对应领域，无选中项与首尾越界动作不可执行；增改只更新父稿，模块 Source Save 另行执行。

CollectionEditor 的 Tab 从当前行进入动作按钮时必须保留该行身份；按钮上的 Enter 操作的仍是屏幕所示 Selected 对象。Shift-Tab 返回行也保留身份。停止时请求 Runtime 文件应说明当前没有运行配置，同时给出查看已保存 Source 与状态的路径，不能暗示 Profile 已删除；CLI JSON 错误码保持原样。

添加和修改同一对象共用多字段表单，可以返回任何字段；枚举选择、布尔开关、引用按稳定 ID 选择、成员多选，不输入有限关键词。错误就地显示并保留所有字段，取消有改动时统一 Save/Discard/Continue。

DNS servers 是字符串/对象原生集合，完整查看/增改删/重排；hosts 保留。Routing rules 是有序规则集合，支持查看/增改复制删/重排，按匹配条件、目标、高级项组织；不造 enabled/type 等便利原生字段。高级未覆盖字段明确由 JSON 管理并往返保留。所有 15 模块和原有嵌套功能在覆盖矩阵逐项列出，不用 DNS/routing 代替全部。

Logs 保留原始消息，未知格式不伪造级别；长行/多行清楚续行，不假定 IPC chunk 是记录。终端控制字符受控显示。跟随、暂停、新增、丢弃/截断、连接状态属于底部，暂停不被新日志拉回。完整原始时间/来源/路径可查看。传输chunk游标只在技术详情，不冒充记录号；无时间不补采样时间，dropped/truncated布尔不伪造数量。断连正文保留缓存，底部明确Cached/Disconnected并提供恢复路径。

## CLI 合同

CLI11 实际注册树生成根、分组与叶帮助。裸 xrm/分组打印当前层 Usage、子命令、说明和真实示例，帮助/版本不访问 daemon、不创建数据。叶缺参报具体缺项并给该层 Usage，未知子命令给当前层合法项，不自动纠错执行。显式 -h/--help 正常退出；解析错误非零，JSON 错误只给原机器格式，不混普通帮助。

保留现有命令/参数、--socket、显式 Profile ref、--json schema/完整 ID/退出码；默认非交互。订阅 ref 接受精确唯一名称或 ID；重名时拒绝猜测并列出 ID。`config edit` 接受既有 `--text` 或新增 `--file PATH`（`--file -` 显式读取 stdin），两者互斥，读取失败不得写入。人读列表/详情按任务组织，普通输出隐藏凭据，授权编辑/导出使用真实值。空列表非 TTY 保留既有管道合同。人读错误使用命令/任务、对象、原因和恢复建议；原始诊断可达，wire JSON 错误 schema 与 message 保持。TUI Use/Edit 是会话态，不在 CLI 持久化伪造。错误 stderr、结果 stdout；非 TTY 无动画/ANSI。订阅批量部分完成逐来源报告成功与失败，单一来源全失败返回非零且不在 stdout 输出成功回执。成功只报告已确认事实；启动不等于代理连通。watch 及时 flush，Ctrl-C 正常退出；只有 stdout 消费者关闭导致的 EPIPE 允许干净退出 0，不吞 IPC 断开或其他写错误，各进程退出码分别核验。

TUI 的破坏性确认默认聚焦 Cancel；Enter 不经显式切焦不能删除或停止。Stop 显示当前运行对象和作用，确认时绑定 Server 当前 run_id（尚在启动时绑定“当前无 active”），若实例已变化则拒绝；取消不触发核心动作。CLI Stop 仍为显式非交互命令。

## 验证与交付

按重写提示词 R1–R12 执行组件事件/状态、当前屏幕与样式、隔离 PTY 用户链；核查实际草稿/源/RPC/运行事实。组件 Demo 通过不代表页面完成，断言数量不代表可用性。提供组件复用矩阵、模块覆盖矩阵、旧实现删除清单、真实屏幕/操作记录、CLI 样本及隔离体验入口。所有试验独立 HOME/XDG、socket、数据库、confdir、端口；不访问生产订阅或 TUN。真实核心缺失记录 SKIP。G5 仅由用户人工复验。

补充（审查实测）：主题颜色策略在每帧绘制前应用，包含FTXUI能力探测后的首屏；stopped不能由默认health值推导出Process alive。首次Logs未连接没有缓存，不显示Cached或推测日志数；确有缓存则明确标旧数据。JSON验证错误持久显示在编辑区旁，完整诊断由Help可达。

# 双评审修复记录

状态：公共组件、生产页面与 CLI 修复及工程回归已完成；G5仍待用户。唯一合同为 `xrm-interaction-spec.md`；原报告保持原样。

## 基线与证据

branch/commit、未提交工作树、二进制SHA256及报告/旧证据时间见 [baseline](evidence/review-remediation/baseline.json)。当前构建 `build/component-rewrite`；基线构建无待编译项。上一轮Save生命周期修复已在二进制，不能把旧崩溃作为当前缺陷。A/B都主要看实现方捕获，无独立实机运行，评分不作验收证据。

宿主已发现并实际调用tmux MCP create_session，但新隔离socket被allowlist拒绝；本轮改用已授权的隔离tmux CLI，未宣称MCP终端执行成功。Playwright宿主调用成功，仅将实际ANSI当前格重渲染为PNG，非原生终端像素截图。

## 两份报告去重追踪矩阵

原分级仅原审查意见；当前严重程度以复现后果说明，未验证不升级。重复根因/章节/复审条目共用一行，V01–V15在后表独立交付。当前分级指本轮复现时的程度，修复后关闭；P0不能仅沿用原报告评分。

| 包 | 原报告位置 | 原分级 | 问题/建议 | 当前判断/状态 | 当前分级、根因/决策 | 实现/测试/限制 |
|---|---|---|---|---|---|---|
| R1 | A ROOT-04/P0-01; B ROOT-02/P1-1 | P0/P1 | 二级导航完整身份、续项与位置 | 已修复 | 修复前P1（发现性，非不可达）；当前80列Tab可达全部15项；末项可达不等于可发现 | Navigation完整窗口/位置；Scope与Source显式标签；底部摘要+Help完整详情。`ui/nav-*`、`screens-final/*`；组件导航全宽回归。 |
| R4 | A ROOT-07/P0-02; A §8 | P0 | Paper空白cell主题残缺 | 旧归因未证实；当前补证据 | 证据不足，不定P0；旧PNG存在深色空白，但旧capture丢弃行尾空格，不能归因产品；本轮-N保留格并补真彩背景验证 | Shell单一模态边界；surface/panel清除下层并绘制所有cell；Toast计时暂停/历史repeat；JSON编辑期间延后。`screens-final`、`ui/*dialog*`、`editor`、`components-final.txt`。 |
| R2 | A ROOT-02/P1-01/P1-10; B ROOT-01/P1-4 | P1 | 动作发现、大小写及别名重复 | 已修复 | 修复前P1；基线Help仅静态键表；改同源可执行Help，不恢复动作条 | HelpOverlay保留Menu数据所有权，动作来自真实Action/ContextHelp并执行同一回调；合并别名、上下文前置条件，Form/Dialog也可执行。`ui/help-*`、`ui/trace.json`、`modules-help-final`、`real-core`。 |
| R3 | A ROOT-03/P1-02/P1-03; B §10/P2-4 | P1/P2 | 空输入、字段界限和集合空状态 | 已修复 | 修复前P1；Input空白、StringList无语义Value、五按钮 | Field presence、可见Input、固定Select、StringList语义标签/选择身份/非法动作边界、字段附近错误；全部生产Form共用。`components-final.txt`、`native-forms.txt`、`terminal-final`、`editor`。 |
| R3 | A P1-04; A/B §10 | P1 | 可选bool三件套、absent/0/空值 | 已修复 | 修复前P1；共享可选字段模型，保留原生presence | Field presence、可见Input、固定Select、StringList语义标签/选择身份/非法动作边界、字段附近错误；全部生产Form共用。`components-final.txt`、`native-forms.txt`、`terminal-final`、`editor`。 |
| R5 | A ROOT-06/P1-05; B §7 | P1 | Session/Target多状态token串 | 已修复 | 修复前P1；改固定标记与清楚字段，保持状态分别来源 | Profile Use/Edit/Run与Node Saved/Run独立列；View底部详情；已知数量单复数；缓存/恢复明确，operation断线后仅查询原ID，不重放写入。`ui/trace.json`、`logs/trace.json`、`screens-final`。 |
| R1 | A P1-06; B ROOT-02 | P1 | Nodes范围与同名来源混同 | 已修复 | 修复前P1；单一二级模型加范围/来源标签 | Navigation完整窗口/位置；Scope与Source显式标签；底部摘要+Help完整详情。`ui/nav-*`、`screens-final/*`；组件导航全宽回归。 |
| R4 | A P1-07; B P1-3/P2-5 | P1 | Dialog双边框、模态/未保存后果 | 已修复 | 修复前P1；当前Unsaved捕获；Shell唯一边界 | Shell单一模态边界；surface/panel清除下层并绘制所有cell；Toast计时暂停/历史repeat；JSON编辑期间延后。`screens-final`、`ui/*dialog*`、`editor`、`components-final.txt`。 |
| R4 | A P1-08; B ROOT-04/P1-3 | P1 | Toast穿透/遮挡/恢复 | 已修复 | 修复前P1；旧证据非当前证明；检查当前合成与遮挡计时 | Shell单一模态边界；surface/panel清除下层并绘制所有cell；Toast计时暂停/历史repeat；JSON编辑期间延后。`screens-final`、`ui/*dialog*`、`editor`、`components-final.txt`。 |
| R5 | A P1-09; B §9/§18.3 | P1 | 离线、缓存、恢复和保稿 | 已修复 | 修复前P1；基线footer弱且FOLLOW与离线并列 | Profile Use/Edit/Run与Node Saved/Run独立列；View底部详情；已知数量单复数；缓存/恢复明确，operation断线后仅查询原ID，不重放写入。`ui/trace.json`、`logs/trace.json`、`screens-final`。 |
| R8 | A P1-11; B ROOT-03/P1-2 | P1 | 人读错误内部词/混合语言/恢复 | 已修复 | 修复前P1；保留wire message；适配受控人读说明与原始诊断 | human_message仅适配人读；命令树help/metavar、回执name+ID、source/runtime、null按语义；JSON原错误保留。`cli.json`、`cli-80-columns.txt`/120/180，真实watch/JSON/TSV/退出码。 |
| R5/R8 | A P1-12; B §13 | P1 | 跨界面术语与Use/Edit作用域 | 已修复呈现；不合并领域状态 | 修复前P1；enabled/support/available、select/Use不可合并；CLI不持久化Use/Edit | Profile Use/Edit/Run与Node Saved/Run独立列；View底部详情；已知数量单复数；缓存/恢复明确，operation断线后仅查询原ID，不重放写入。`ui/trace.json`、`logs/trace.json`、`screens-final`。 |
| R1 | A P1-13/P2-1; B ROOT-04/05/P2-1/P3-5 | P1/P2 | 底部详情消失/右栏、长路径截断 | 已修复 | 修复前P1；基线实现120才右栏；改底部摘要与完整详情入口 | Navigation完整窗口/位置；Scope与Source显式标签；底部摘要+Help完整详情。`ui/nav-*`、`screens-final/*`；组件导航全宽回归。 |
| R7 | A P2-2; B P3-6/§11 | P2/P3 | ruleTag必填与标签 | 未复现；补证据 | 证据不足，不定P0；当前无必填validator；星号是脏标记。补可省略回归 | 模块覆盖见[module-form-coverage.md](module-form-coverage.md)；原生可选presence、路由分组、未知路径/类型保留，JSON边界明确。`modules`、`modules-help-final`、`native-forms.txt`、`real-core`。 |
| R5 | A P2-3; B P3-1 | P2/P3 | 单复数 | 已修复 | 修复前P2/Polish；共享数量格式 | Profile Use/Edit/Run与Node Saved/Run独立列；View底部详情；已知数量单复数；缓存/恢复明确，operation断线后仅查询原ID，不重放写入。`ui/trace.json`、`logs/trace.json`、`screens-final`。 |
| R5 | A P2-4; B §7 | P2 | Draft/unsaved词汇 | 已修复 | 修复前P2/Polish；统一Unsaved呈现，Draft领域对象保留 | Profile Use/Edit/Run与Node Saved/Run独立列；View底部详情；已知数量单复数；缓存/恢复明确，operation断线后仅查询原ID，不重放写入。`ui/trace.json`、`logs/trace.json`、`screens-final`。 |
| R5 | A P2-5; B §9 | P2 | Use/Edit切换反馈 | 已修复 | 修复前P2/Polish；会话变更通知说明目标与runtime unchanged | Profile Use/Edit/Run与Node Saved/Run独立列；View底部详情；已知数量单复数；缓存/恢复明确，operation断线后仅查询原ID，不重放写入。`ui/trace.json`、`logs/trace.json`、`screens-final`。 |
| R4 | A P2-6; A/B §8 | P2 | 列头/元数据对比 | 已修复 | 修复前P2/Polish；列头提升强调；NO_COLOR不依赖颜色 | Shell单一模态边界；surface/panel清除下层并绘制所有cell；Toast计时暂停/历史repeat；JSON编辑期间延后。`screens-final`、`ui/*dialog*`、`editor`、`components-final.txt`。 |
| R6 | A ROOT-05/P2-7/P2-8; B ROOT-06/P2-3/P3-4 | P2 | 传输游标冒充序号/时间/布尔数量 | 已修复 | 修复前P2/Polish；去正文chunk列，技术详情保留；不伪造时间/计数 | 日志传输chunk只进技术详情；按原文换行，视觉续行单独引导；缺时间为-；不把dropped布尔写成数量。UTF-8跨chunk及EOF修复见LogBuffer/Process与组件测试；`logs`、`real-core`。 |
| R4/R2 | A P2-9; A/B §10 | P2 | 重复通知次数与历史入口 | 已修复 | 修复前P2/Polish；次数有标签，Help历史可执行 | Shell单一模态边界；surface/panel清除下层并绘制所有cell；Toast计时暂停/历史repeat；JSON编辑期间延后。`screens-final`、`ui/*dialog*`、`editor`、`components-final.txt`。 |
| R1 | A P2-10 | P2 | included与导航焦点混用 | 已修复 | 修复前P2/Polish；included留模块状态；选中只表导航 | Navigation完整窗口/位置；Scope与Source显式标签；底部摘要+Help完整详情。`ui/nav-*`、`screens-final/*`；组件导航全宽回归。 |
| R2/R7 | A P2-11; B §9/§11 | P2 | 模块只读门槛与保存层级 | 已修复 | 修复前P2/Polish；Help明确Profiles Edit、Source/Runtime、父稿→保存→运行 | HelpOverlay保留Menu数据所有权，动作来自真实Action/ContextHelp并执行同一回调；合并别名、上下文前置条件，Form/Dialog也可执行。`ui/help-*`、`ui/trace.json`、`modules-help-final`、`real-core`。 |
| R8 | A P2-12 | P2 | config modules源/运行文件重复 | 已修复 | 修复前P2/Polish；不同路径对象不能合并；核对人读标签 | human_message仅适配人读；命令树help/metavar、回执name+ID、source/runtime、null按语义；JSON原错误保留。`cli.json`、`cli-80-columns.txt`/120/180，真实watch/JSON/TSV/退出码。 |
| R2/R5 | A P2-13; B P2-2 | P2 | Overview统计不可用原因与下一步 | 已修复 | 修复前P2/Polish；Help真实路径；不自动开stats或虚构指标 | HelpOverlay保留Menu数据所有权，动作来自真实Action/ContextHelp并执行同一回调；合并别名、上下文前置条件，Form/Dialog也可执行。`ui/help-*`、`ui/trace.json`、`modules-help-final`、`real-core`。 |
| R6 | A P2-14 | P2 | Logs滚动条孤立 | 已修复 | 修复前P2/Polish；检查当前布局与真实长日志 | 日志传输chunk只进技术详情；按原文换行，视觉续行单独引导；缺时间为-；不把dropped布尔写成数量。UTF-8跨chunk及EOF修复见LogBuffer/Process与组件测试；`logs`、`real-core`。 |
| R5 | A ROOT-01/P2-15; B ROOT-05/P2-1 | P2 | 宽屏信息量/七页任务结构 | 已修复 | 修复前P2/Polish；代表数据验证；不为填空白造数据 | Profile Use/Edit/Run与Node Saved/Run独立列；View底部详情；已知数量单复数；缓存/恢复明确，operation断线后仅查询原ID，不重放写入。`ui/trace.json`、`logs/trace.json`、`screens-final`。 |
| R8 | A P3-1/P3-2/P3-3; B P3-3 | P3 | CLI metavar、validator语法、实现理由/重复尾注 | 已修复 | 修复前P2/Polish；真实CLI11树格式，保留参数语法 | human_message仅适配人读；命令树help/metavar、回执name+ID、source/runtime、null按语义；JSON原错误保留。`cli.json`、`cli-80-columns.txt`/120/180，真实watch/JSON/TSV/退出码。 |
| R8 | A P3-4; B P2-6/§13 | P3/P2 | CLI空行/null/名称+ID/回执 | 已修复 | 修复前P2/Polish；按作用域解释null，成功附加查询失败不反报失败 | human_message仅适配人读；命令树help/metavar、回执name+ID、source/runtime、null按语义；JSON原错误保留。`cli.json`、`cli-80-columns.txt`/120/180，真实watch/JSON/TSV/退出码。 |
| R2 | A P3-5/P3-6; B P3-2 | P3 | Help对齐/混杂scope说明 | 已修复 | 修复前P2/Polish；可执行任务与输入说明分组，单一入口 | HelpOverlay保留Menu数据所有权，动作来自真实Action/ContextHelp并执行同一回调；合并别名、上下文前置条件，Form/Dialog也可执行。`ui/help-*`、`ui/trace.json`、`modules-help-final`、`real-core`。 |
| R4 | A P3-7 | P3 | Toast Done符号 | 设计建议不采纳 | 不定级（建议）；符号作为冗余辅助保留；标题直接Success/Info/Warning/Error | 按本行原因不采纳；实际替代方案见相应R包和V表。 |
| R9 | A P3-8 | P3 | Process fixture样例名 | 设计建议不采纳 | 不定级（建议）；属于隔离测试命名，不是生产伪造状态 | 按本行原因不采纳；实际替代方案见相应R包和V表。 |
| R9 | B P1-5; A §0/§18.3 | 存疑P1 | 订阅URL掩码与真实编辑 | 掩码疑点已核实；新增合法揭示 | 修复前P2/Polish；合成URL实测，区分密码控件与取证处理 | 本轮真实tmux当前格、按键与RPC/文件事实见下方V表；不把旧证据或缺图当功能结论。 |
| R7 | A §11.3/§18.4; B §11 | 证据疑点 | 15模块真实覆盖/高级边界 | 当前已有实现，补齐证据 | 证据核验项；源码已有15模块真实Form/Collection；需本轮逐模块终端补证 | 模块覆盖见[module-form-coverage.md](module-form-coverage.md)；原生可选presence、路由分组、未知路径/类型保留，JSON边界明确。`modules`、`modules-help-final`、`native-forms.txt`、`real-core`。 |
| R9 | A §14/§18.C; B §14/§18.9 | 证据疑点 | CJK/组合字符/长文本/输入键 | PTY已验证；OS IME受阻 | 证据核验项；PTY可证明接收编辑显示；OS IME候选窗不在工具环境 | 本轮真实tmux当前格、按键与RPC/文件事实见下方V表；不把旧证据或缺图当功能结论。 |
| R9 | A §18.2/3/E; B §18.1 | 证据疑点 | Group成员、节点有限字段、测试结果 | 当前已修复并补证据 | 证据核验项；核对生产Form及错误留稿 | 本轮真实tmux当前格、按键与RPC/文件事实见下方V表；不把旧证据或缺图当功能结论。 |
| R9 | A §18.6/G; B §0/§18.4 | 证据疑点 | 连续4条通知、焦点、时序、resize | 当前已修复并补证据 | 证据核验项；可控时钟组件测试+实际终端轨迹 | 本轮真实tmux当前格、按键与RPC/文件事实见下方V表；不把旧证据或缺图当功能结论。 |
| R9 | A §18.7/F; B §18.8 | 证据疑点 | 真实Xray日志与chunk保真 | 当前已修复并补证据 | 证据核验项；固定v26.9.9 loopback；fake仅故障/拆分fixture | 本轮真实tmux当前格、按键与RPC/文件事实见下方V表；不把旧证据或缺图当功能结论。 |
| R9 | A §18.8/9; B §18.3 | 证据疑点 | 断连/引用丢失/下载失败/check失败 | 当前已修复并补证据 | 证据核验项；独立实例，不重放写入，保留原故障诊断 | 本轮真实tmux当前格、按键与RPC/文件事实见下方V表；不把旧证据或缺图当功能结论。 |
| R9 | A §18.10/I; B §18.2 | 证据疑点 | 七页三尺寸、全主题、resize压力 | 当前已修复并补证据 | 证据核验项；本轮二进制ANSI当前格与可信重渲染 | 本轮真实tmux当前格、按键与RPC/文件事实见下方V表；不把旧证据或缺图当功能结论。 |
| R9 | A §18.A/B/D/H; B §18.1/6/7/10 | 证据疑点 | 完整任务与Help驱动、CLI机器兼容 | 当前已修复并补证据 | 证据核验项；界面操作+RPC/Source/confdir事实核对 | 本轮真实tmux当前格、按键与RPC/文件事实见下方V表；不把旧证据或缺图当功能结论。 |
| R2 | A §16.1; B §16.1/5 | 建议 | 常驻动作/右侧操作区 | 设计建议不采纳 | 不定级（建议）；与本轮明确边界冲突，以可执行Help解决发现与执行 | 按本行原因不采纳；实际替代方案见相应R包和V表。 |
| R1 | A §9.5/§16.2 | 建议 | 跨主导航数字/方向捷径或多行导航 | 设计建议不采纳 | 不定级（建议）；保持焦点层与Tab遍历；稳定单行完整标签窗口 | 按本行原因不采纳；实际替代方案见相应R包和V表。 |
| R3/R6 | A §16.3/5 | 建议 | 统一三态/丢弃0/改游标为记录号 | 设计建议不采纳 | 不定级（建议）；不把不同原生类型或布尔/游标伪装成新语义 | 按本行原因不采纳；实际替代方案见相应R包和V表。 |
| R4 | A §16.4 | 建议 | 删除Paper | 设计建议不采纳 | 不定级（建议）；修整屏背景，保留已提供主题 | 按本行原因不采纳；实际替代方案见相应R包和V表。 |
| R7 | A §11.5 | 建议 | 显示未覆盖N字段 | 设计建议不采纳 | 不定级（建议）；只显示实际检测路径，不从对象总数编造高级字段数 | 按本行原因不采纳；实际替代方案见相应R包和V表。 |
| R6 | A §12 | 证据缺口 | Logs搜索/过滤/级别计数 | 设计建议不采纳 | 不定级（建议）；无既定功能承诺，不因缺截图扩张功能 | 按本行原因不采纳；实际替代方案见相应R包和V表。 |

## 验证交付

| 验收 | 本轮结果 | 证据/剩余限制 |
|---|---|---|
| V01 | 已实际验证 | `ui/nav-*`三尺寸双向全部15项、`ui/nodes-local-*`同名范围/来源；`screens-final`长标签；无跨焦点捷径。 |
| V02 | 已实际验证 | `ui/help-*`七页+Form/Dialog；Help执行Profile完整链，`real-core`执行模块/Connect/Stop；组件回调恰一次，F1前后runtime状态相同。 |
| V03 | 已实际验证；IME待人工 | `components-final.txt`与`ui/form-cjk-edited`，`terminal-final`非法字段留稿、Home/End/Left/Right/Delete/Backspace；空/readonly/disabled；`?qeup`不穿透。 |
| V04 | 已实际验证 | `native-forms.txt`absent/0/false/空字符串/空数组/未知枚举/原有类型；`modules`三尺寸只读与无修改接受Source字节不变。 |
| V05 | 已实际验证 | `terminal-final`DNS/routing新增修改复制删重排及错误留稿；StringList稳定编辑身份/空值错误/边界按钮，native集合源码序列断言。 |
| V06 | 已实际验证 | `ui/four-toast-events`、历史/重复、dirty Dialog、三主题浮层；可控时间测试暂停计时；`editor`JSON编辑延后Toast/就地error/出现消失；`logs`日志背景详情。 |
| V07 | 已实际验证 | `screens-final`63页屏+各主题Help/Form/Dialog/Toast；`compatibility`显式TERM/COLORTERM/NO_COLOR与ANSI色码；启动探测覆盖策略已修复。 |
| V08 | 已实际验证 | `ui/view-edit-a-highlight-use-b-running-a`、取消Use/切Edit/ProfileCRUD；`terminal-final`不同Profile只读/保存；RPC PID/源文件/confdir事实不变。 |
| V09 | 已实际验证，包括提交后丢回包 | `compatibility/first-no-daemon`；`ui/failed-save-keeps-draft`→reconnected不重放；`logs`有缓冲断线/恢复；保留operation ID仅查询。`unconfirmed-final`代理将真实module.save成功回包丢弃：核对源已写入、TUI保稿、恢复不重放；另丢operation.get回包并核对重复查询同一ID。代理不伪造业务成功。 |
| V10 | 已实际验证 | `logs`固定真实Xray三尺寸、pause新增、原文与缓存；组件/后端log/process/lifecycle覆盖chunk/UTF8/控制字符/真实EOF，保留协议边界。 |
| V11 | 已实际验证 | 15项逐条[module覆盖表](module-form-coverage.md)，`modules`141屏+`modules-help-final`6屏；`real-core`DNS/routing完整保存→显式运行；ruleTag optional非必填。 |
| V12 | 已实际验证 | `ui/url-masked`/`url-revealed-synthetic`及真实URL核对；`terminal-final`Group成员多选、节点有限选择/非法留稿；`config`资源与Core表单。 |
| V13 | 已实际验证 | `cli.json`捕获每个stdout/stderr/exit，根/组/叶、缺参/对象/核心/断线、JSON/TSV/watch/pipe；CLI真实Xray显式运行。 |
| V14 | 已实际验证 | `ui/dirty-resize-*`同进程120→80→60→80→180、同一脏值/保存边界；`screens-final`18 Profiles/41 nodes/12 sources及长名；既有1205节点测试另复验。 |
| V15 | 已实际验证主链 | `ui`Help Profile管理；`terminal-final`订阅生命周期/Group/节点；`real-core`Help DNS/routing→Save→真实SOCKS/HTTP loopback→Logs/stats→退出保PID→Help Stop。G5仍不代签。 |

已另外核验原用户的主题切换 SIGSEGV：基线包含的Form::submit生命周期保护保留；本轮Help标签也改为明确所有权。真实Config回归从Save按钮依次切Paper/Terminal/Slate并重新打开表单，全部正常；这两类生命周期修复不会改Use/Edit或后端设置边界。

## 构建与回归命令记录

- `cmake --build build/component-rewrite -j 4` 全目标构建完成；随后按实际修复增量编译。最终二进制/源哈希和各证据阶段身份见 [manifest.json](evidence/review-remediation/manifest.json)。工作树没有提交、reset或清理原有变更。
- 首轮 `XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray ctest --test-dir build/component-rewrite --output-on-failure -j 1` 实际执行361项，333通过、28失败，[原始输出](evidence/review-remediation/ctest-final.txt)保留。失败包括旧逐字段Prompt/旧页面字符串与样式假设，以及本轮实测补修的JSON持久错误；不能把首轮描述为全绿。
- 旧测试迁移保留了RPC、源文件、confdir、PID、真实流量和未知值断言。PTY修复只涉及raw输入/持续drain、按实际UI完成信号等待及Toast换行；未删除复杂任务。42项TUI acceptance逐项复验见`acceptance-rerun-{2,3,4}.txt`与`acceptance-firstuse-final.txt`；两个真实完整gate见`gate-e2e07-final.txt`和`gate-remigration-third.txt`。
- 最终单次全量复验 **362/362通过，0失败，709.21秒**，包括固定真实Xray集成链；完整输出为 [ctest-verification-final.txt](evidence/review-remediation/ctest-verification-final.txt)。末次宽表头文件变动后，收尾发现原生表单测试与showcase两个目标仍待重链接；全量运行结束后已补建，并独立重跑全部18项原生表单测试通过。生产xrm/xrmd未改变，最终Ninja检查无待构建项。
- `build/component-rewrite/tests/tui/components/xrm_tui_components_tests --reporter compact`通过；34用例覆盖事件/生命周期/浮层计时/全部背景cell/宽表/日志，原始输出`components-final.txt`。`build/component-rewrite/tests/tui/forms/xrm_tui_forms_tests --reporter compact`18用例通过，原始输出`native-forms.txt`。这些只支持各自覆盖范围，不替代屏幕/真实任务。
- `python3 tests/cli/cli_rewrite_test.py --xrm build/component-rewrite/xrm --xrmd build/component-rewrite/xrmd --xray /tmp/xrm-g1-xray-v26.9.9/xray --evidence docs/evidence/review-remediation/cli.json`通过274个捕获场景；每项有stdout/stderr/exit，不以条数代替接口核对。
- 真实Config PTY脚本通过Core路径失败/登记/选择、Tests非法值留稿、Editor多行、三个主题从Save按钮保存、Geodata失效/验证/绑定/移除，以及本地入站只读不写入。`config/steps.json`为实际按键和当前屏。
- `git diff --check`通过。环境没有可用clang-format；没有为排版安装新依赖或改写整个旧工作树。

## 公共根因与生产接入

- `widgets.hpp/cpp` 为七页共用输入/选择/表单/集合/表格/导航/Help/Dialog/Toast；`style.hpp` 为唯一颜色与不透明背景实现。宽表按实际分配空间和数据长度展开列（上限64格），80列仍明确省略；不是给各页固定增加空格。
- `workspace.cpp/actions.cpp` 负责焦点层、同源动作Help、底部状态、模态栈和UI消息；没有右侧Inspection、常驻动作条或第二套动作执行条件。模态Help先关闭再执行一次原回调，返回原组件保留输入焦点。Help的Menu标签生命周期现由组件持有，修复首次模块取证暴露的悬空引用。
- `native_form.cpp` 只映射受控原生字段，不引入新schema/配置镜像。`src/tui/pages/*` 的Profile、订阅、节点、Group、Core、Geodata、设置均接入相同Form。模块JSON验证错误现持续显示在编辑器附近，完整诊断由Help打开。
- `log_viewer.cpp` 专用布局保留，未退化通用表格；`server/log_buffer.*` 修复实际UTF-8传输边界，`infra/process.*`传递真实EOF。未修改日志RPC形状，没有造全局记录号、采样时间或丢弃数。
- `client/human_message.hpp`由CLI/TUI共用受控原因与恢复说明；原始errno/远端诊断保留，机器wire不翻译。CLI先前未设值的`config export`默认source修为独立saved默认，显式runtime及JSON合同保留。
- 订阅绑定只使用当前完整View Profile的来源数据。`profile.list`只有摘要，不据它猜测所有Profile绑定。Source存在/Included、节点Enabled/Support/测试结果、TUI Use/Edit各自保留来源和作用域。

## 屏幕复看与迭代

方向继续使用统一的Slate developer console：业务数据主导、克制分隔、底部状态、浮层承载任务。没有把审查建议转换成第二次架构重写。

本轮先后看过实际ANSI重渲染图片，发现并修正了Help上下文挤压动作、模态重复边界、只读字段噪声、宽屏名称仍过早截断、JSON错误仅靠Toast、启动颜色探测覆盖策略、stopped仍写Process alive及首次Logs错误标Cached/0。取证自身也修复两点：tmux `capture-pane -N`保留空白cell；resize等待应用重绘而不是仅等待tmux几何更新。旧Paper深色空白的单张重渲染图不再作为产品故障的独立证明。

| 页面 | 本轮观察与最终取舍 | 仍有的合理边界 |
|---|---|---|
| Overview | Actual/Saved/Observed三组扫读清楚，Use控制Next Connect，未测流量和缺统计不显示0；Help提供真实模块前置条件 | 无统计时没有曲线，必须明确配置与连接 |
| Profiles | 独立Use/Edit/Run列，底部始终跟View而非高亮；18个Profile验证滚动与长名，宽屏按内容扩列 | 80列长名省略，Help完整详情可达 |
| Modules | 全宽领域工作区，宽屏内部树与字段分栏；15键双向可达；父稿/Source/runtime三层说明在Help首屏 | 一项数据保留空白，复杂字段仍用同一JSON稿，不伪造字段计数 |
| Subscriptions | 结果/数量/自动更新/当前View绑定分列；详情分开真实尝试/成功时间，完整URL只在授权编辑揭示 | 未查询其他Profile全文时不推断其绑定；默认摘要不显示凭据 |
| Nodes | Scope与Source显式区分，Enabled/Support/Saved/Run互不合并；41节点验证列表与长名 | raw-only是真实支持维度；Group使用独立成员表单 |
| Logs | 已亲看真实Xray三尺寸。80列续行很多但metadata和正文对齐，120/180独立subsystem，Warning可扫；chrome在底部 | stream只有stdout/stderr等原身份，不宣称跨进程可靠重组；原文详情可滚动 |
| Config | 核心/资源列表与程序设置Form共用组件；Paper、Slate与Terminal palette切换同一进程保存，不再崩溃 | 字体由终端渲染，Maple Mono配置可选，不假称TUI强制换字体 |

屏幕入口：[截图索引](evidence/review-remediation/index.html)，每页三尺寸和三个主题均可点击原图；真实Xray日志、浮层与保存未确认另列。该索引不是产品mockup。

## 证据性质与版本

所有运行均使用独立HOME/XDG/database/socket/tmux/confdir和loopback fixture；fixture及清理PID记在各`trace.json`。`screens-final`使用18 Profiles、41 nodes、12 subscriptions，来源自动刷新关闭。PNG为对应真实当前格ANSI经HTML/浏览器渲染（faint按65%前景近似，不降低背景透明度；实际终端的faint强度可能不同），保留`.txt/.ansi/.html`原件；不是设计mockup，也不是原生终端模拟器像素截图。

每份trace独立记录实际xrm SHA。修复过程中的`before`、`iteration-1`、`modules-attempt1`、`terminal-attempt*`和兼容性`*-attempt1`保持其旧身份。全模块141屏后只改变公共Help上下文次序、可选字段提示和宽表布局；模块逻辑验证仍有效，受影响Help和完整任务另有本轮补验。不能将这些旧阶段SHA改写为最终SHA。

## 保留的限制与未采纳意见

1. G5仍由用户验收；OS IME候选窗、具体终端字体回退与屏幕阅读器体验需要人工环境。PTY的CJK/组合字符检查只证明接收、编辑和显示。
2. tmux MCP新隔离socket受allowlist限制；实际操作改用授权tmux CLI，未绕过MCP守卫或宣称MCP调用成功。
3. 日志协议不包含跨进程日志事件身份。非法字节在文本RPC中明确转义供审计，不宣称可以无歧义逆转任意二进制日志；后端仍保留原始字节缓冲，不执行ANSI/OSC。
4. 未采用右侧动作面板、常驻快捷键、数字跨页导航、删Paper、把chunk改名记录号、全局持久化Use/Edit、原生默认值自动写入、虚构高级字段数。这些与本轮合同或真实数据来源冲突。
5. CLI既有`module.save_changes`逐项结果的部分失败可返回成功的RPC envelope/退出码；本轮保留机器契约，人读明确失败项，不擅自修改退出码。
6. 首轮全ctest中`xrm_component_rewrite_terminal`一次tmux pane提前退出，后续独立正式终端链和CTest复跑未复现；原失败记录保留，不把它解释为已确认的文案断言错误。原用户theme SIGSEGV所涉Form回调生命周期已修复，有关闭最后owner单测及真实三主题Save按钮回归；不将一次未重现等同于证明所有崩溃绝无可能。

## 留给用户的少量主观判断

- 80列的Form单行间距是否偏宽松；当前优先清楚分辨字段和就地验证，长表单通过位置指示与滚动继续。
- Slate与Paper的个人对比度偏好，以及本地Maple Mono/普通等宽字体的实际阅读体验。功能语义不依赖颜色或Nerd Font。

这些不阻塞已完成的工程修复，也不由自动评分替用户作答。

## 复验入口

```sh
cmake --build build/component-rewrite -j 4
XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray ctest --test-dir build/component-rewrite --output-on-failure -j 1
python3 tests/tui/rewrite/review_remediation.py --output /tmp/xrm-review-ui
python3 tests/tui/rewrite/review_remediation.py --screens-only --output /tmp/xrm-review-screens
python3 tests/tui/rewrite/review_remediation.py --editor-only --output /tmp/xrm-review-editor
python3 tests/tui/rewrite/unconfirmed_review.py --output /tmp/xrm-review-unconfirmed
python3 tests/tui/rewrite/compatibility_review.py --build build/component-rewrite --output /tmp/xrm-review-compatibility
python3 tests/tui/rewrite/module_review.py --build build/component-rewrite --output /tmp/xrm-review-modules
env -u NO_COLOR python3 tests/tui/rewrite/log_review.py --build build/component-rewrite --xray /tmp/xrm-g1-xray-v26.9.9/xray --output /tmp/xrm-review-logs
env -u NO_COLOR python3 tests/tui/rewrite/real_core_terminal.py --build build/component-rewrite --xray /tmp/xrm-g1-xray-v26.9.9/xray --output /tmp/xrm-review-real-core
python3 tests/tui/rewrite/config_pty.py --build-dir build/component-rewrite --xray /tmp/xrm-g1-xray-v26.9.9/xray --evidence /tmp/xrm-review-config
python3 scripts/tui-lab.py --build build/component-rewrite --core /tmp/xrm-g1-xray-v26.9.9/xray --record /tmp/xrm-review-lab.json
```

审查最小入口为本记录、唯一交互契约、模块覆盖表、人工G5清单和`evidence/review-remediation`。核心路径必须是本机真实固定版本，缺失时真实核心项目为SKIP而非PASS。Lab不自动启动核心。

# 三份独立可用性报告：交叉汇总

本目录归档三名 Agent 的报告，并按**现象而非 HUMAN/ROOT 编号**合并结论；各报告的编号只在各自文件内有效。此处是复验前的工程评估，**不是 G5 人工验收**。三份报告都建议 **Not ready for human G5**；现行验收状态仍以项目文档和用户人工复验为准。

## 原稿索引与证据边界

| 来源 | 归档文件 | 范围 |
|---|---|---|
| A：独立黑盒 + 第二阶段源码核对 | [最终报告](agent-a-final-report.md)、[第一阶段原始记录](agent-a-phase1.md)、[第二阶段核对](agent-a-phase2.md) | 曾误连共享 tmux，已明确排除污染会话；有效会话未带固定 core，真实 Run/Logs/Stats 主链未验证。源码核对可纠正黑盒误判。 |
| B：另一隔离 lab | [Part A 黑盒原稿](agent-b-part-a-blackbox.md)、[Parts B–E 原文](agent-b-parts-b-e.md) | Part A 原稿来自 `/tmp/xrm-lab/part-a-blackbox.md`；B–E 逐字提取自用户提供的输出。包含真实 Xray v26.9.9 隔离运行记录；其源码判断限于所报告的核对，不等于本轮重新审查。 |
| C：第三名 Agent | [用户提供的完整报告原文](agent-c-pasted-report.md) | 未提供本地报告文件；此处逐字保存用户消息中的 A–E。它明确说**没有进行第二阶段源码审查**，只读了交互契约；“实现 bug”仅为行为推断。隔离 lab 使用手工登记的 Xray。 |

原文中的 HUMAN-01 等是**各 Agent 私有编号**，不要跨文件同号引用为同一问题。B 的 Part A 与 Parts B–E 分开存放；C 的原文中 Part A 与 B–E 在同一文件。A 的原始截图/终端抓取路径仍是 `/tmp`，这里归档的是报告文本，**未归档每一张终端抓取**。三份报告并非同一个 lab、同一操作序列或同等源码审查程度；本汇总没有再次运行产品测试。

## 合并结论：先处理可重复的阻塞点

| 主题（交叉引用） | 综合判断 / 最小下一步 |
|---|---|
| **只读 Modules 缺 Edit 恢复路径**（A HUMAN-02；B HUMAN-03；C HUMAN-17） | 三方都在只读集合/表单的 F1 遇到“不能改但不告诉去 Profiles 开 Edit”。A 第二阶段核对为 overlay Help 遮蔽了前置条件；优先让当前上下文 Help 说清目标 Profile、前置条件和跨页路径，不恢复旧逐字段 Prompt。 |
| **订阅更新失败不暴露具体原因**（A HUMAN-05；B HUMAN-24；C HUMAN-19） | 三方一致：`Failed`/`partially completed`/Details 不能指出网络、URL、HTTP 或解析原因；A 核对 batch 结果到人读界面丢失诊断。保留失败来源与旧节点，并让结果/详情显示任务级错误及下一步。B 另报 CLI 此错误误判 not-found 且写 stdout，需单独复现。 |
| **导航层与 F1 动作模型不易发现**（A HUMAN-01；B HUMAN-01/04/05/20；C HUMAN-01/04/05/14/15） | Tab 分层、Enter 进入、Esc 返回是[现行契约](../xrm-interaction-spec.md)明确语义；不能以测试者想用箭头/数字键就直接推翻。应在现有焦点层和 Help 内讲清 Tab 的目标及跨页任务归属，避免非文本上下文无反馈。B 所报 Esc 交替开/关、带稿无法离开是更严重的**未核定行为**，与 A/C 报告的 dirty guard 成功并存，须按准确焦点/事件序列复现。 |
| **原生长表单、Select 与可选值**（A HUMAN-03/04/07；B HUMAN-08–12/15/16/31/32；C HUMAN-06/07/11/12） | 三方都感到 15–18 字段和复合焦点成本高。A 确认 routing target 控件存在（不是缺功能），错误不定位字段；B 记录候选未提交、可选输入吞字、checkbox Enter 误切、错误残留、预览不刷新等情形，C 另报集合行无法操作、Esc/带稿焦点问题。优先按真实控件事件复测并修复，保留原生字段、未知字段和草稿→Source→运行边界。 |
| **断线数据正确、Help/动作与错误恢复滞后**（A HUMAN-09/10；B HUMAN-29；C daemon 测试） | 三方称 cached/disconnected、留稿、自动重连表现良好；A 源码核对指出断线写动作的 Help 未按连接状态禁用。离线入口需提供 xrmd/连接恢复建议，Refresh 给明确反馈；不应把“状态保留正常”误当“离线操作可用”。 |
| **人类错误和对象名层级**（A HUMAN-06/08/12；B HUMAN-21–23/27；C HUMAN-20–23） | A 实测 `/bin/true` 注册失败被错误归因为 saved config；B/C 记录中文原始诊断、UUID/内部 tag 优先展示、URL 掩码缺提示。A 核对 URL 真实值与 Ctrl-R reveal 已存在，只是难发现；非 TTY UUID-first TSV 也属现有管道契约，不能直接改为 TTY 风格。先修正任务级归因、可见名称/可达详情，不改 wire JSON schema、明确的退出码和已有凭据边界。 |
| **Stop/删除等后果与视觉反馈**（B HUMAN-14–16/18/25/28；C HUMAN-10/11/12/18） | B/C 对删除默认焦点、Stop 无确认、集合选择与覆盖层有不同观察；B 源码复核撤销了“Cancel 直接丢弃草稿”的黑盒判断。危险动作的默认焦点/提示及过期预览应针对可复现事件修复，不把“感觉丢稿”写成已证实数据丢失。 |
| **CLI 脚本体验**（A HUMAN-13/14；B HUMAN-22/23；C HUMAN-20–24） | root/leaf Help 与 JSON/结构化错误总体可用；大 JSON 仅 inline、列表不一致、订阅 ref 约定和管道错误仍有摩擦。`stats --watch | head` 在 A 中报 io_error/exit3，在 B/C 中分别表现安静结束 / SIGPIPE 141；不能合并为一个已证实的统一行为，先在相同构建和 pipefail/TTY 条件下重测。 |

## 优先顺序与复验

1. **恢复可操作性**：只读 Edit 前置条件与 F1 跨页指路；订阅失败的真实原因；断线动作可用性/恢复说明。三份均支持，A 有源码定位。
2. **修正表单的实际阻塞**：Select 提交状态与提示、可选输入事件、checkbox 意外切换、路由 target 定位、变更后的错误与预览刷新；以受影响控件的事件序列和草稿/Source 事实验证，不先重做导航合同。
3. **消除错误指引**：core 注册失败归因、脱敏 URL 的显式揭示、可读名称/诊断层级。其他视觉/CLI 建议按可重现成本分批处理，避免修改机器格式。
4. **补足验收证据**：使用隔离 socket/DB/HOME/XDG、固定测试 core 和 loopback，复验 Profile→Modules→Subscription→Nodes/Group→Connect→Logs/Stats 的实际链路。A 缺核心主链，B 有部分任务（Group、订阅绑定等）未测，C 没有源码审查；真实 IME 均未由 tmux 输入证明。80×24、焦点/带稿退出以及不同 pipe 环境的分歧需精确复测。

**证据不能相互抵消**：B 的 collection 第二行无法选中与 A/C 能操作是不同会话/焦点条件，不宣布全局功能缺失；B 的 `Esc` 锁住与 A/C 的取消保护通过也是待复现分歧。A 对 Ctrl-A/Ctrl-U、非 TTY UUID-first、URL reveal、routing target 和 tmux 无反显的更正优先用于解释 A 自身记录，不自动推翻 B/C 的独立现象。这里没有修改或宣布修好任何产品缺陷，也没有替用户把 G5 标记为 PASS。

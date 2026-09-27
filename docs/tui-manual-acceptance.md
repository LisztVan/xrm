# G5 人工验收：组件重写

状态：**待用户人工复验，未标记 PASS**。工程测试、PTY、MCP、真实核心回归只证明记录中的场景。唯一有效交互规则见 [交互契约](xrm-interaction-spec.md)，实际覆盖见 [模块表单矩阵](module-form-coverage.md)。

## 隔离体验入口

先按 [构建说明](build.md) 编译 `real-core` preset，再启动隔离实例：

```sh
python3 scripts/tui-lab.py --build out/real-core \
  --core .deps/tools/xray-26.9.9/xray \
  --record /tmp/xrm-component-lab.json
```

核心参数可省略，随后在 Config → Core 登记自己的测试核心。脚本创建临时 HOME/XDG、数据库、socket、Profile、虚构节点和 loopback 订阅，不自动连接。记录文件保存本次路径与进程身份；正常退出只清理自身 daemon、HTTP fixture 和临时数据，退出 TUI 本身不会停止生产实例。不要把该命令指向生产数据、真实订阅或 TUN。

在另一个终端查看记录中的 socket，所有 CLI 操作显式加 `--socket <该路径>`。截图同时保留终端尺寸、动作、对象和源/运行结果。用于连接的固定核心应先核实版本与摘要；没有真实核心时流量项记 SKIP。

## R1–R12 走查

1. [ ] **复用与全部页面**：Overview、Profiles、Modules、Subscriptions、Nodes、Logs、Config 七页均可到达。Profile、订阅、Group、Core、Geodata、设置与原生对象使用一致表单/选择器；不是仅有组件 Demo。
2. [ ] **输入**：添加和修改同一个对象均显示多个字段。中文和英文混合文本中用左右/Home/End/Backspace/Delete 修改中间字符；多行框上下移动，Enter 换行。Tab/Shift-Tab 返回首字段。输入 `q u e p ?` 不触发背景动作；错误时文本和焦点仍保留。Ctrl-S 可用且退出后终端流控恢复。
3. [ ] **焦点与 Help**：Tab 只切当前导航层；Enter 进入；Esc 仅返回一层；数字/方括号不跳页。表单 F1 → Help → Esc 回到原字段和光标，输入框里的 `?` 是文本。主界面只有一个 F1 Help 入口；F1 本身不执行，方向键选择任务、Enter 执行一次，Esc 恢复位置。不可执行动作不列入操作区，前置条件在帮助上下文说明。
4. [ ] **Use / Edit**：Profiles `u` 选择 P，再次 `u` 取消，不停止核心或改变保存目标。Use 为空时 Connect 禁用。`e` 开启 P 的模块编辑并将 View 指向 P，不改变 Use/Run；Q 未开 Edit 时模块只读。Profile 新建/改名/复制/删除仍允许。关闭/切换 Edit 的未保存稿有 Save/Discard/Continue，保存失败不丢模式、身份、文本。
5. [ ] **底部详情**：Profiles Enter 后 Core/Sources 等说明仅在底部，列表行不插入说明。高亮/View/Use/Edit/Run 不同对象可辨。`F` 展开完整详情；Modules `i` 查看完整路径。80×24 的底部不淹没正文，宽屏订阅列宽不过度扩张。
6. [ ] **Toast**：loopback 订阅更新完成后通知在内容右上角，3 秒后普通通知消失；列表位置、焦点与输入不变。403、混合合法/不支持/损坏条目能看到失败/部分成功。错误信息过期后仍能从对象/表单或通知历史找回；重复错误与队列不会堆满屏幕。
7. [ ] **Logs**：正文只含日志，底部显示 Following/Paused、新增、丢弃/截断与连接状态。长行、多行、控制字符可辨。暂停后新日志不拉走历史位置，恢复跟随有效。c/d 不触发核心生命周期；原始详情保留来源事实。
8. [ ] **Visual**：完全不用 JSON 完成 DNS servers 与 routing rules 的增查改复制删/重排。新增/修改同一个表单，可返回第一个字段，非法端口保留输入。Group 类型/策略是选择器，成员是多选；原生静态 balancer 与 Group bindings 不混写。
9. [ ] **原生边界**：15 模块逐个打开，disabled/无 Source 不落盘、不自动启用。Visual→JSON→Visual 是同一草稿；未知字段、合法其他类型、数组顺序保留。非法 JSON 留稿并给位置。Edit 关闭时集合、文本、Include、预设、外部编辑器均不能写；Runtime 永远只读。保存只改所属源，不重生成 confdir、不启动。
10. [ ] **CLI 帮助**：无 daemon 时运行裸 `xrm`、`xrm server`、`xrm profile`、`xrm config`、叶命令 `--help`、缺参数和未知子命令。每层为实际命令树的局部帮助与可执行示例；帮助不创建数据；错误非零。
11. [ ] **CLI 输出**：列表/详情/空结果/创建/保存/连接/停止/失败分别查看人读、管道与 `--json`。JSON 不混帮助/ANSI，普通输出不泄漏凭据；watch 及时 flush，Ctrl-C、daemon 断开、消费者关管道行为明确。
12. [ ] **完整闭环与尺寸**：空库存 → 本地订阅 → 登记核心 → Profile → 明确绑定来源/Use → 节点/Group → 保存/连接 → Logs/统计。带稿在同一进程 120×40→80×24→60×18 保护→80×24→180×48，检查焦点与稿件。固定真实 Xray+loopback 检查配置与流量；不要把 fake-core 或 `-test` 当远端连通。

## 其他已有能力

- [ ] Subscriptions 修改/暂停/恢复/绑定/删除，多 Profile 引用和保留本地节点；历史清理先预览安全候选再明确选择。
- [ ] Nodes Local typed/高级导入、复制、显示名/标签、启停、测试结果/取消；Save target 不运行，Save & Connect 是明确的两步结果。
- [ ] Config Core 登记/发行版/资产选择安装/选择/删除；Geodata 添加/修改/验证更新/绑定/移除；Tests/Appearance/Editor 设置持久且不改 Xray 模块。
- [ ] 外部编辑器正常、无配置、取消、非零、非法 JSON、外部冲突均保留适当草稿。内嵌编辑边界见 [输入记录](component-rewrite-terminal.md)，本轮未提供应用内撤销栈或剪贴板选区。

记录实际结果、屏幕、异常和复现步骤。所有复选框由人工执行后填写，工程测试不代填。

## 双审查后的重点复验

- [ ] Slate / Paper / Terminal palette 在七页、空白格、Help、单边框 Dialog、Toast 与 resize 后背景一致。16/256/truecolor/无色仍可辨焦点。
- [ ] 二级导航正反遍历15模块，完整选中名称和位置、左右续项清楚；Nodes 的 Scope: Local 与 Source: Local 可区分。
- [ ] 可选值区分未设置、false、0、空字符串和空集合；未知枚举保持，不因打开表单生成默认值。
- [ ] 用 Help 完成 Profile 新建/查看/Use/取消/元数据；Form 和 Dialog 的 Help 同样可以保存、取消、确认。
- [ ] 连续4条通知及重复次数可从历史核对；模态和可写 JSON 输入期间不遮挡，回到主区后再显示。
- [ ] 合成 URL 默认掩码，Ctrl-R 或字段 Help 合法揭示；修改后真实值正确，未改动不把掩码保存。
- [ ] 首次无服务、已有缓存后断开、保存结果未确认、重连时草稿保留，没有自动重放写入。
- [ ] OS 输入法候选窗与本地终端字体、输入法组合由人工补验；PTY Unicode 输入不替代该项。

本轮工程证据与两报告逐项处理见 [集中记录](design-review-remediation.md)。以上仍为用户 G5 清单，工程记录不代勾。

# XRM 独立黑盒操作报告

本轮只读了 `blind-fixture-card.md`，其后只依靠产品 TUI、`xrm`/`xrmd` 内置 Help 和隔离 lab 操作。没有读源码、修复报告、测试脚本或其他 Agent 结论，也没有改源码或构建产物。终端画面来自独立 tmux socket `s` 的 `capture-pane -p -e`（`*.ansi`）和无 ANSI 当前格（`*.grid.txt`）。当前 lab 见 `lab.json`：socket `/tmp/xrm-component-lab-e6ojxmpm/run/xrmd.sock`，Profile `Lab Home`，来源 `Local fixture`。最初一次 bootstrap 使用另一独立 lab，仅保存了 `00-overview.*`，随即 Ctrl-C 退出并清理；下述操作全部发生在 `01-overview.*` 所属的当前 lab。

## 结果与事实

| 用户任务 | 黑盒结果 | 直接证据 |
| --- | --- | --- |
| 只读恢复 Edit | Modules Help 指向 Profiles；`e` 后 `Module Edit: Lab Home`，运行仍为 none | `05-help-modules.*`、`10-help-profile-content.*`、`11-enable-edit.*`、`12-modules-edit.*` |
| 新增 DNS、操作第二项、取消、排序、保存 | 增加 `127.0.0.1`；选第二项进入编辑、修改后 Discard，原值保留；`+` 将新项移至第一；Ctrl-S 保存 Source，runtime unchanged | `19`、`20`、`23`–`27` 对应画面；`81-config-dns-source.stdout` |
| routing 缺目标并修正 | 未设目标的 Ctrl-S 定位 `outboundTag: Choose an outbound or balancer`；选择 `Loopback direct` 并设 `network=tcp`，去掉误设的空 domain，保存 | `32-routing-missing-target.*`、`34-routing-clean-draft.*`、`35-routing-saved.*`、`82-config-routing-source.stdout` |
| 失败订阅诊断与恢复 | URL 指向本机未监听的 9 端口后更新失败；Details 显示连接失败并提示保留已有 1 个节点；恢复 lab URL 后更新成功，节点数仍为 1 | `45-sub-refused.*`、`46-sub-failure-detail.*`、`47-sub-recovered.*`、`78-sub-show-json.stdout` |
| daemon 不可用及恢复 | 暂停原 lab daemon 后 TUI 后来显示 `xrmd disconnected · cached data`、Running unknown；CLI 8 秒无返回。原进程随后已是 zombie，原 socket 留存；用 `xrmd --foreground` 在本 lab 原 socket/数据库上重新启动，CLI status 与 TUI Refresh 成功 | `49`、`51`–`56` 对应画面/CLI；`50-daemon-paused-cli-status.exit` |
| 连接、Logs、Stats、明确 Stop | TUI `c` 后 Xray 26.9.9 运行，实际目标为 Loopback direct；Logs 显示 Xray 从 confdir 逐个读取 `00_log`、`01_dns`、`02_inbounds`、`03_outbounds`、`04_routing`、`90_nodes_tail`；Stats 可查看但数值不可用，界面列出缺少 stats/policy/metrics；`d`、确认后 stopped | `60-connect-result.*`、`61-logs.*`、`62-stats-running.*`、`63-stop.*`、`64-stopped.*`、`88-final-status.stdout` |
| CLI 来源名称/ID 查询 | `sub list` 给出 ID，`sub show 'Local fixture'` 与 ID 均成功，`--json sub show` 返回真实本地 URL；均 exit 0 | `75`–`78` 的 `.stdout/.stderr/.exit` |
| CLI 文件配置提交 | `config show` 先显示 loglevel warning；把 `log-info.json` 交给 `config edit Lab Home log --file ...`，返回 Source 已保存且运行未变；再次 show 为 info，`config check Lab Home` 为 checked_only，均 exit 0 | `80`、`83`–`86` 的 `.stdout/.stderr/.exit` |
| CLI watch 管道 | `xrm --json stats --watch \| head -n 2` 得到两条 JSON 样本，xrm/head 状态 `0 0`，stderr 空；因已 Stop，样本均为 `core not running` | `87-watch.stdout`、`87-watch.stderr`、`87-watch.pipe-status`、`87-watch.exit` |

CLI 还能查询已保存的 Profile：`93-profile-show.stdout` 给出当前核心、保存的节点目标、1 个来源、15 个模块。`79-config-modules.stdout` 列出全部 15 个模块及源/运行文件。`81`、`82` 查询的 Source 内容与 TUI 保存一致。CLI 仅在 Stop 后尝试 `config show Lab Home --source runtime`，exit 1；因此本轮没有取得活动运行文件的 CLI dump。实际运行由 TUI Runtime 与 Xray Logs 支持。文件提交的 loglevel `info` 只做了 Source 保存和 `config check`，没有再 Connect 发布到运行文件。

## 实际按键轨迹

`Tab×N`、`Backspace×N` 表示实际连续发送 N 次；这里记录按键与观察，不把工具等待时间算作人的思考时间。每组编号对应同名前缀截图。

1. 启动当前 lab 后 `F1` 看 Overview Help（`02`）；第一次快速 `Esc Tab Tab` 没关掉 Help，只把 Help 任务选择移到 `q`（`03`）。分开发 `Esc`，再 `Tab Tab` 到只读 Modules（`04`）。
2. Modules `F1`（`05`）指出 Profiles 路径；`Esc`、`Shift-Tab` 到 Profiles（`06`），`F1`（`07`）；`Esc`、`Enter` 进入内容焦点（`08`），再 `Enter` 无新详情（`09`），`F1`（`10`）显示 `e Allow module Edit`；`Esc e` 成功（`11`）。
3. `Esc Tab` 返回 Modules（`12`）；`Enter F1` 看模块选择 Help（`13`）；`Esc Tab Enter` 到 dns 内容（`14`），`F1`（`15`）；`Esc Enter` 打开 DNS servers 集合（`16`）；`Tab Tab Enter` 打开 Add 表单（`17`）；输入 `127.0.0.1`，`F1`（`18`），`Esc Ctrl-S` 加入模块草稿（`19`）。
4. 集合里 `Shift-Tab Down` 选第二项（`20`）；试 `Tab` 去 actions 时选择回到第一项；`Down Enter` 打开第二项只读详情（`21`），`Esc` 回集合。再试 `Tab` 同样回第一项，`F1`（`22`）找到直接动作键；`Esc Down e` 打开第二项编辑（`23`）。
5. 第二项表单光标开始在字符串开头；试 `Backspace×9` 再输入 `127.0.0.2`，结果是前缀插入、表单变为错误值。`Esc` 打开未保存确认（`24`），`Tab Enter` 选 Discard，第二项仍为原 `127.0.0.1`（`25`）。在仍选第二项时按 `+`，它上移为第一项（`26`）；`Esc Ctrl-S` 保存 dns Source（`27`）。
6. `Esc Tab×3 Enter` 到 routing（`28`）；`Enter a` 打开规则 Add（`29`、`30`）。`Tab Space` 启用 domain，但在数组区输入 `domain:example.invalid` 没进入 New domain；`F1` 看表单 Help（`31`）。用 `Esc` 回表单后尝试 Tab/Shift-Tab/Down/Enter/Right 定位字段，`Ctrl-S` 触发缺目标错误，焦点跳到 outboundTag（`32`）。
7. `Right` 选 `Loopback direct`；`Shift-Tab Right` 设 `network=tcp`；`Shift-Tab×2 Space` 没关闭当时的空 domain，`Ctrl-S` 暂时加入含空 domain 的草稿（`33`）。按 `e` 重新编辑，`Tab Space` 明确关闭 domain，`Ctrl-S` 得到只含 `network=tcp` 的规则（`34`）；`Esc Ctrl-S` 保存 routing Source（`35`）。
8. `Esc Esc Tab Enter` 到 Subscriptions 内容（`36`），`F1`（`37`）；`Esc u` 初次更新成功、1 节点（`38`）；`Enter` 没显示更多，`F` 打开详情（`40`）；`Esc e Tab Ctrl-R` 揭示本地 URL（`41`、`42`）。`End Backspace×5` 把 `/nodes` 改成 `/missing`，`Ctrl-S u` 后仍成功（`43`、`44`），说明这个 fixture 路径没有制造失败。
9. `e Tab Ctrl-R End Backspace×30` 输入 `http://127.0.0.1:9/nodes`，`Ctrl-S u` 得到 `Failed: inspect` 且 1 个旧节点保留（`45`）；`F` 看失败原因（`46`）。`Esc e Tab Ctrl-R End Backspace×24` 恢复 `http://127.0.0.1:32903/nodes`，`Ctrl-S u`，更新成功（`47`）。
10. 对当前 lab.json 所列 daemon PID `1197715` 发 `SIGSTOP`；TUI 内容页按 `F1` 后画面暂未反应（`49`），CLI `status` 在操作员的 8 秒边界仍无退出。发 `SIGCONT` 后看到 Help 中的断连说明和缓存状态（`51`）；`Esc r` 当时仍刷新不完整（`52`）。`ps` 观察原进程为 zombie，CLI status 返回 exit 3（`53`）。读 `xrmd --help`（`54`）后，移除仅属于本 lab 的 stale socket，并在同一 tmux server 新窗口启动 `xrmd --foreground --socket ... --database ...`；CLI status exit 0（`55`），TUI `r` 恢复 `xrmd ready`（`56`）。
11. `Esc Shift-Tab×3 Enter` 到 Overview 内容（`57`），`F1`（`58`）；`Esc c` 开始连接（`59`），完成后显示 Xray 26.9.9、运行 Profile 与实际目标（`60`）。`Esc Tab×5` 到 Logs（`61`）；`Shift-Tab×5 Enter` 回 Overview 看 Stats（`62`）。`d` 出现 Stop 确认且默认在 Cancel（`63`）；`Shift-Tab Enter` 明确确认，随后 stopped（`64`）。
12. 后续为复核集合键盘路径，从 Overview `Esc Tab×2 Enter` 进入 Modules 时保留此前 routing 选择；盲按 `Tab Enter Enter` 错进 observatory 表单，`Esc Esc Shift-Tab×4 Enter` 回 DNS。`Enter Down` 选第二项（`94`）；连续三次 `Tab` 在两行之间循环，始终显示 `Focus: rows`，第一次后截图（`95`）。`Esc` 退出集合（`96`），未改任何值。

TUI `F1` 共按 **12 次**：`02,05,07,10,13,15,18,22,31,37,49/51,58`。其中 daemon 暂停时一次延迟到恢复后呈现。CLI/daemon 的 `--help` 共调用 **17 次**，包含为寻找 watch 入口而看的 node、operation Help，以及一次重复的 server Help。Help 原始输出在 `48`、`54`、`65`–`74`、`85`、`91`–`92`、`97` 的文件里；第一次 `server --help` 仅在终端工具输出观察，`97` 为再次保存的同一 Help。

## 体验问题与边界

- **集合 Tab 导航与屏幕提示不符。** 两项 DNS servers 集合提示 `Tab for actions`，但在第二项按 Tab 后跳到第一项且焦点仍为 rows；继续 Tab 在两项间循环。`94` 与 `95` 是可复现前后画面。这会让试图按可见按钮编辑/排序第二项的用户作用到错误对象；本轮靠 `F1` 给出的直接动作键 `e`、`+` 完成任务。
- **停止后的 runtime 查询报错指向错误。** `xrm config show Lab Home --source runtime` 在 Stop 后 exit 1，错误说 Profile Lab Home 或引用不存在，而紧接着 `profile show Lab Home` exit 0。这可能使用户误以为 Profile 丢失；本轮未在运行中执行该 CLI 查询，所以只确认停止态的提示不准确（`90`、`93`）。
- **daemon 暂停实验有恢复成本。** SIGSTOP 后 CLI 8 秒内没有退出；TUI 后来只显示缓存和 unknown。原 daemon 在 SIGCONT 后实际已是 zombie，单纯刷新不能恢复；重新启动服务后 TUI/CLI 都恢复。不能据此断言一般断网或正常 daemon 重启也会如此，实验只覆盖了本次 SIGSTOP/进程退出路径。
- **统计数值未取得。** 运行时 UI 明确提示缺少 stats、policy、metrics；Stop 后 watch 管道返回 `core not running`。本任务完成了 Stats 入口与不可用原因的查看，没有真实流量采样或互联网连通性证据。固定 loopback freedom outbound 的 Connect 仅证明此 lab 的核心启动与配置加载。

结束前查询的最终状态为 stopped；来源 URL 已恢复，DNS/routing 和 CLI log Source 已保存。之后操作员向 `daemon-restarted.pid` 所记的手动 daemon 发 SIGTERM，从 TUI 顶层按 `q` 退出；lab 的 `/tmp/xrm-component-lab-e6ojxmpm` 已由启动器清理，独立 tmux server 也已结束。以上运行和 Source 事实均以退出前保存的证据为准。

## 补测：Nodes / Group（新的独立 lab）

此节是主 Agent 后续明确要求的补测，不属于上面的首次盲测。仅用产品界面与 Help；证据在 `group-followup/`。新 tmux socket 为 `g`，新 lab 是 `/tmp/xrm-component-lab-yl6o5vb4`，其 socket、数据库、随机订阅端口和对象 ID 均见新的 `group-followup/lab.json`，与首次 lab 不同。

从 Overview 发送 `Tab×4` 到 Nodes（`02-nodes.*`）；`Enter Tab×2` 进入 `Scope: Groups`，空列表见 `03-groups-section.*`。再 `Enter F1`，Help 明确列出 `a Add Group`，并说明 Group 位于 Nodes 下、Connect 在 Overview（`04-groups-help.*`）。`Esc a` 打开 Group 表单，`F1` 明确提示 Save 只写保存设置，runtime 不变（`05`、`06`）。随后输入名称 `Lab Local Group`，`Tab Right` 把 Selection mode 设为 Manual，`Tab×3 Space` 勾选 `Loopback direct`，`Tab×3 Right` 把 Manual member 设为该节点；保存前的表单见 `07-group-composed.*`。`Ctrl-S` 成功，列表为 `manual / random / explicit`，页脚仍显示 Running none（`08-group-saved.*`）。

保存后 `F` 得到 Group ID `69fb5656-0494-4a80-a1d1-f7f0ed5a0e5e`（`09-group-details.*`）；`Esc e` 重开编辑表单，回读值为 `Nodes: [x] Loopback direct`、`[ ] Loopback fixture`、`Manual member: Loopback direct`，来源未勾选（`18-group-edit-reopen.*`）。CLI `group list Lab Home` 找到此 ID（`12`），`group show Lab Home <ID>` 和 `--json group show` 均 exit 0，返回 `Profile: Lab Home`、`members.kind=explicit`、唯一 `node_ids=[bbd903b5-935e-40a6-ab70-d5614fa078c1]`、同一 `selected_node_id`（`15`、`16`）。`node list` 把该 ID 对应到 `Loopback direct`（`17`）。`config show Lab Home routing` 的原生 Source 仍只有 `domainStrategy: AsIs` 与空 rules（`19`），`status` 为 stopped（`13`、`26`）。这些查询支持 Group 作为 `Lab Home` 的保存绑定，保存本身没有发布运行配置；本补测没有 Connect。所有 CLI 调用的 stdout、stderr、退出码分别保存在相应前缀文件。

此次 Group 补测按 `F1` **2 次**（`04`、`06`），CLI Help **2 次**（`10-cli-group-help.*`、`11-cli-group-show-help.*`）。Group 的 `F` 详情只显示 ID 和摘要，没有直接列成员；进入 `e` 回读表单或用 CLI `group show` 才能确认成员。

### 受指导的第二项 Tab 复验

此项是已知问题的定向复验，**不计入首次盲测**。在同一新 lab 中，从 Group 编辑表单 `Esc×3` 回顶层，`Shift-Tab×2 Enter` 到 Modules；发现模块只读后，`Esc Shift-Tab Enter e` 在 Profiles 启用 Edit。再 `Esc Tab Enter Tab Enter Enter` 到 DNS servers 集合；`Tab Tab Enter` 新增 `127.0.0.1`，输入后 `Ctrl-S` 仅放入模块草稿。`Shift-Tab Down` 选第二项（`20-guided-tab-before.*`），然后单按 `Tab`：这次焦点移到 actions，第二项仍被选中（`21-guided-tab-after.*`）。这与首次盲测 `94/95` 的表现不同，说明当前新 lab 所用二进制在该路径下可以保留第二项选择；不能把受指导复验改写为首次盲测结果。

定向复验的 DNS 改动没有写入 Source：`Esc Esc` 打开未保存提示（`22`），`Tab Enter` 选 Discard，列表回到单个 `localhost`（`23`）；CLI `config show Lab Home dns` 验证 Source 仍只有 `localhost`（`24`）。最后再查询 `group show` 与 `status`（`25`、`26`），Group 仍保存、runtime 仍 stopped。

补测结束后从 TUI 顶层按 `q` 退出；新的 `/tmp/xrm-component-lab-yl6o5vb4` 已由启动器清理，tmux server `g` 已结束。Group 的保存事实以上述退出前的 TUI/CLI 证据为准。

# v1 Release Candidate 验收（G5）

日期：2026-09-24。本文记录最终集成代码的实际证据；状态只使用 `PASS`、`FAIL`、`SKIP`、`NOT RUN`、`NOT IMPLEMENTED`。T 项映射依据 `docs/testing-standard.md` TS-07，不把代码存在或局部覆盖当作通过。

## 验收环境与总结果

- 主机：`Linux 7.2.7+deb14-amd64 x86_64 / debian-work`；Intel Core i7-14700F；GCC 16.2.0；Debug dev 与 ASan presets。
- 固定真实核心：`/tmp/xrm-g1-xray-v26.9.9/xray`，输出 `Xray 26.9.9 (Xray, Penetrates Everything.) 52a412d (go1.27.1 linux/amd64)`。固定发布二进制在仓库外准备，SHA-256 `1eb9175d0f0a8f8149c9230a7fc5ae66ce332ed20a53155ce61fe62e3f58b7df`；没有改动系统 Xray。
- 最终 dev 干净重配/构建成功；`TMPDIR=/home/lisztzy/.cache/xrm-tmp XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray ctest --preset dev --output-on-failure`：**178/178 PASS，0 FAIL，0 SKIP**。其中 `real_core` **11/11 PASS**，RC-01～RC-10 与 E2E-06 均执行。
- 最终 ASan 干净重配/构建成功；同样环境下 `ctest --preset asan --output-on-failure`：**178/178 PASS，0 FAIL，0 SKIP**。其中 `real_core` **11/11 PASS**。ASan PERF-02 执行相同负载和产品缓冲区/功能断言；Sanitizer allocator RSS 不套用 Debug 产品 RSS 上限，原因与证据见 [acceptance-notes.md](acceptance-notes.md)。
- 固定核心命令为 `xray run -test -confdir <test-confdir>`、`xray run -confdir <test-confdir>` 以及 RC-01～RC-10/E2E-06 gate 所调用的同一 v26.9.9 binary；没有公网节点，核心行为用例均 loopback。CTest 最终 real-core label 两套构建均无 skip。
- systemd unit 静态验证及 `xrmd` 子进程归属测试通过；此环境未确认可安全隔离的 systemd 实机测试环境，未启动/停止宿主 systemd unit，故 systemd 实机清理 **NOT RUN**。不要将 PKG-01 的静态/归属证据描述为 systemd 运行态测试。
- release candidate 总门：**FAIL / incomplete**。T25 的日志暂停操作未实现，外部编辑器和完整人工小终端流程也没有验收入口/证据。没有为 M5 新增这类产品功能。

## I50 / I51 / I52

| Task | 状态 | 证据 | 限制 |
|---|---|---|---|
| I50 | PASS（实机环境限制已披露） | `xrm_packaging_tests [packaging]` 2 cases / 48 assertions；PKG-01 unit 静态策略、LIFE-10 验证 xrmd 退出只清理自己拥有的 Xray child、socket 关闭且无关 Xray 存活。 | 没有隔离 systemd 环境，实机启停 NOT RUN。 |
| I51 | PASS | RC-01～RC-10 与 E2E-06 使用固定 v26.9.9 最终代码，11/11 PASS；E2E-06 包含两个 Manager 客户端共用一个 Server，订阅刷新 A→B 时运行 PID、confdir 和所有文件字节不变，显式选择并 connect 后才采用 B，TUI 退出不停止 core，Server 重启后持久状态一致。 | VMess/Trojan/Shadowsocks/Hysteria2 保持 traffic-unverified。 |
| I52 | PARTIAL | 80×24 PTY 自动验收 1 case / 44 assertions；PERF-01 41 assertions；PERF-02 1000 节点真实测量，dev/ASan 均 PASS。 | T25 中日志暂停未实现；外部编辑器及完整人工/小终端流程、saved-vs-actual 页面验收未完成。 |
| G5 | FAIL / incomplete | 其余自动化、真实核心回归和逐项证据已记录。 | T25 未满足，因此不宣布 v1 release candidate 完成。 |

## T01–T37 最终追踪

| ID | 状态 | 测试与实际证据 |
|---|---|---|
| T01 | PASS | E2E-01、E2E-06、UI-01；CLI/TUI 共用 Server，TUI 退出后实际 core 保持运行。 |
| T02 | PASS | SUB-02、SUB-06、E2E-03；订阅更新库存不写运行目录。 |
| T03 | PASS | SUB-03、HTTP-03；失败/HTML/空和部分损坏结果有分别处理。 |
| T04 | PASS | SUB-04、GROUP-04；身份匹配及缺失已选目标失败处理。 |
| T05 | PASS | MOD-02、ASM-02；模块源编辑按目标文件保存，测试逐模块确认。 |
| T06 | PASS | GROUP-01/02、BIND-01、E2E-04；组可筛选、手选并保存。 |
| T07 | PASS | ASM-03、GROUP-05、RC-03；动态组与原生路由/真实出口流量共存。 |
| T08 | PASS | ASM-01/08、RC-01；真实 Xray 多文件 confdir 检查与运行，无单一汇总配置。 |
| T09 | PASS | MOD-01、ASM-01、MOD-08；15 个原生根项文件归属和输出均覆盖。 |
| T10 | PASS | ASM-04、FORM-03、GROUP-03、RC-04；完整 routing 和 policy/stats 输出不互相覆盖。 |
| T11 | PASS | MOD-02、ASM-02；模块修改只影响所属运行文件。 |
| T12 | PASS | MOD-03、WRITE-01；停用模块不残留在新运行目录。 |
| T13 | PASS | LIFE-01、RC-01、RC-02；真实核心检查、启动及协议流量均验证。 |
| T14 | PASS | LIFE-05、CLI-02、E2E-06；仅保存后显式 connect 才应用；refresh 本身不应用。 |
| T15 | PASS | LIFE-02/03/11；检查失败保留旧 core，候选启动失败恢复一次。 |
| T16 | PASS | LIFE-04、SUB-06；stop phase matrix 与刷新/重试停止意图测试通过。 |
| T17 | PASS | GROUP-02、BIND-01、LIFE-05、RC-03；普通节点/手选组选择持久且重启后有效。 |
| T18 | PASS | CTRL-01、RC-06；runtime override/clear 真实流量验证，不修改永久选择。 |
| T19 | PASS | WRITE-04、RC-10、E2E-05；导出目录脱离 xrmd 可由相同 Xray 直接读取/运行。 |
| T20 | PASS | MOD-04、FORM-02/06；未知字段保留，编辑规范化范围限目标模块。 |
| T21 | PASS | LIFE-06、UI-03；无 API/Metrics 时 process-only 和 unavailable 状态，不伪造统计。 |
| T22 | PASS | MET-01/02、LOG-02；速率重置/未知值、共享采样、慢 UDS 日志消费者有界测试通过。 |
| T23 | PASS | WRITE-02、LIFE-03/11；活动输入与 Profile 源分离，回退不覆盖用户源模块。 |
| T24 | PASS | INS-01、LIFE-08、E2E-05；系统/托管 core 区分；install 不等于 use/running。 |
| T25 | NOT IMPLEMENTED | 自动 PTY 覆盖 80×24、中文/emoji 字符边界和输入焦点的部分行为；但日志页没有暂停交互，外部编辑器场景及完整人工核心流程未提供/未验证。此项阻塞 G5。 |
| T26 | PASS | HTTP-02、PERF-01、RPC-04；HTTP/core-check barrier 下其他 UDS CLI status/logs 仍响应，stop 在约 106 ms 完成，写请求不会重放。 |
| T27 | PASS | CT-01、RC-01/08；固定真实 Xray 实际运行，所有协议 fixture 脱敏，real_core 最终无 skip。 |
| T28 | PASS | E2E-06、CLI-01；两个客户端端到端流程及状态一致性通过。 |
| T29 | PASS | MOD-01/02/03、ASM-01/02、UI-04、RC-07；15 项可列出、编辑、启停、预览/输出；有专用表单与 native editor 的范围如实区分，不声称 15 套完整表单。 |
| T30 | PASS | MOD-07、RC-09；进程 environment、Xray env 和 version 配置分离；固定 core 的 version 约束与资源路径实测。 |
| T31 | PASS | MOD-06、RC-07；stats `{}`、FakeDNS 对象/数组、大小写原生 burstObservatory 在合法配置上下文验证。 |
| T32 | PASS | FORM-02、ASM-06、UI-04；sniffing/TLS/REALITY/mux 写在原生所属 inbound/outbound/Node 字段，无伪顶层文件。 |
| T33 | PASS | GROUP-03、FORM-06、ASM-04/05；原生 route/balancer 与动态组共存，policy/routing 未知字段保留。 |
| T34 | PASS | FORM-04、ASM-05、RC-05；观测类型互斥，binding 只补范围、不覆盖用户 probe 参数。 |
| T35 | PASS | FORM-03、RC-04；便利操作分别保存 stats/policy/metrics，真实 Xray counters 可读，用户 policy 字段保留。 |
| T36 | PASS | FORM-05；geodata 管理 resource references/config，不把 .dat 内容写进 JSON；无第二资源更新器。 |
| T37 | PASS | CODEC-08、ASM-06；旧 reverse/global transport 不误作模块或 outbound，保持原生嵌套归属。 |

## 协议支持等级

| 协议 | URI/输入 | typed 编辑 | Xray 配置 | 真实流量 |
|---|---|---|---|---|
| VLESS | PASS | PASS | v26.9.9 PASS | **loopback-verified**（RC-02/03/06/E2E） |
| VMess | PASS | PASS | v26.9.9 配置验证 PASS | **traffic-unverified** |
| Trojan | PASS | PASS | v26.9.9 配置验证 PASS | **traffic-unverified** |
| Shadowsocks | PASS | PASS | v26.9.9 配置验证 PASS；未支持 plugin 明确 unsupported | **traffic-unverified** |
| Hysteria2 | PASS（仅冻结字段） | PASS | v26.9.9 配置验证 PASS；不可映射参数明确 unsupported | **traffic-unverified** |

`leastPing = verified`（RC-05 loopback 受控探测）；`leastLoad = experimental / unverified`，未提升为稳定支持。VMess/Trojan/SS/HY2 不因能解析或 `-test` 成功而宣传为真实流量已验证。

## 原生 15 模块矩阵（RC-07）

| 原生模块 | 状态 | 验收要点 |
|---|---|---|
| env | PASS | 独立 `12_env.json` 与进程启动 environment 区分，RC-09。 |
| log | PASS | 原生 `log` 文件在合法上下文检查并真实启动。 |
| api | PASS | 原生 API 文件在合法上下文检查；RC-06 用真实 API binary 控制。 |
| dns | PASS | 原生 DNS 文件真实核心检查，unknown 字段保留。 |
| routing | PASS | 完整 `04_routing.json`，RC-03/06 真实规则/组与流量。 |
| policy | PASS | levels/system 保留；RC-04 真实 counters。 |
| inbounds | PASS | 原生 inbound 上下文及 loopback traffic fixtures。 |
| outbounds | PASS | 静态对象与 `90_nodes_tail.json`，RC-01～03/08。 |
| stats | PASS | `{}` 保留且实际 counters 验证，RC-04。 |
| fakedns | PASS | 原生对象与数组形态分别保留并在合法上下文验证。 |
| metrics | PASS | 原生 endpoint 与真实 Xray expvar counters，RC-04。 |
| observatory | PASS | 独立原生根项、受控 probe 和 managed tags，RC-05。 |
| burstObservatory | PASS | 大小写及独立文件，原生 pingConfig 保留，RC-05/07。 |
| geodata | PASS | 原生资源配置/路径检查；数据文件不嵌入 JSON，RC-07/09。 |
| version | PASS | 独立原生文件，固定核心接受/拒绝行为，RC-09。 |

RC-07 是逐模块合法上下文，不是 15 项全部启用的单一巨大配置；专用 forms 的实际范围见 CLI/TUI 和 `docs/usage.md`，无专用表单处使用 native editor。

## TUI 与性能验收

- PTY 自动测试：80×24 terminal，中文、emoji、长节点名；Tab/Shift-Tab、上下、Enter、`?`、`/`、`q/e/space` 输入焦点和 UTF-8 显示检查。1 case / 44 assertions PASS。emoji 通过带 ASCII 后缀的筛选验证显示，未声称验证终端输入法发送 emoji 的方式。
- NOT RUN：无鼠标完整人工 connect 流程、后台刷新焦点和 ID 保持、编辑稿抗刷新、日志滚动暂停、saved target 与实际恢复目标的屏幕并列确认。退出 TUI 不停止 Xray 由真实 E2E-06 PASS 覆盖。
- 外部编辑器：产品目前使用内嵌原生 JSON 编辑，不存在外部编辑器启动入口；成功/非零/取消/冲突/无效 JSON/终端恢复场景 **NOT IMPLEMENTED / NOT RUN**。未为验收临时添加产品功能。
- PERF-01：本机 HTTP barrier 与 fake-core check UDS barrier 同时阻塞；CLI UDS clients 重复 status/logs 并 stop；41 assertions PASS。实测一个最终 dev 样本：status p50/p95 5.34/5.34 ms，logs 5.19/5.19 ms，stop 106.4 ms。ASan 对应测试 PASS。
- PERF-02：1000 个虚构节点、1 Profile、1 core；SQLite list/search、组展开/组装渲染、来源 reconcile、CLI status/logs 和 4 MiB bounded logs。dev 单机 p50/p95：node list 57.36/59.17 ms，search 56.60/58.25 ms，assembly/render 35.74/36.61 ms，reconcile 62.83/63.19 ms，log page 0.00211/0.00229 ms。测得初始/结束 RSS 5840/16184 KiB，seed 后 8 MiB log append 0.908 ms，FileSet 504143 B，日志保留上限 4194304 B。ASan 负载运行通过，但 RSS 不与 dev 比作产品内存限制。有限重复测量不证明长期无泄漏。

## 部署、help 与范围检查

- `packaging/systemd/xrmd.service`：`Type=simple`，`ExecStart=/usr/bin/xrmd --foreground`，明确 system identity/data/runtime paths，`Restart=on-failure`、`RestartSec`、`KillMode=control-group`、停止超时及 `UMask=0077`；未添加 `xray.service`。安装 unit 不 enable/start；文档没有 sudo/setcap/防火墙/DNS 自动操作。
- `docs/usage.md` 覆盖首用、daemon/core/subscription/profile/modules/groups/connect/status/logs/stats/stop/restart/install/use/export 与常见错误；命令名称以已实现 CLI 为准。
- `build/dev/xrm --help`、`build/dev/xrmd --help`，以及 core/node/sub/profile/config/group/select/connect/disconnect/status/logs/stats/tui、`config statistics`、`core use` 的 `--help` 均执行成功。
- `git grep -n -E 'TODO|FIXME|NOT IMPLEMENTED|placeholder|return fixed success|fixed success' -- src apps packaging` 未找到用户可达的假成功或产品 stub；匹配为空。已知 TUI 不足均为显式未实现/未运行，不是隐藏成功状态。
- 默认路径检查：未发现自动 sudo、setcap、systemd enable、系统 DNS/路由/防火墙修改、pkill、覆盖 `/usr/bin/xray` 或自动升级运行 core；systemd unit 只管理 xrmd，由 xrmd 拥有自己的 Xray 子进程。

## 未满足项与 v1 外事项

阻塞 G5 的具体项是 T25：没有日志暂停交互，且外部编辑器与完整人工 TUI 验收未完成。其余环境缺口是 systemd 实机运行未执行；unit 静态审查和进程归属集成测试已完成，缺口已明确披露。

Future / Not in v1：VMess/Trojan/Shadowsocks/Hysteria2 loopback traffic peer 验证；leastLoad 稳定性/真实行为验证；systemd 隔离环境实机验收；TUI 日志暂停和完整外部编辑器流程；长期 RSS 斜率。没有实现 M6 或新增产品范围。

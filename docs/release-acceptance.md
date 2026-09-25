# v1 Release Candidate 验收（G5）

日期：2026-09-25。本文记录最终集成代码的实际证据；状态只使用 `PASS`、`FAIL`、`SKIP`、`NOT RUN`、`NOT IMPLEMENTED`。T 项映射依据 `docs/testing-standard.md` TS-07，不把代码存在或局部覆盖当作通过。

## 验收环境与总结果

- 主机：`Linux 7.2.7+deb14-amd64 x86_64 / debian-work`；Intel Core i7-14700F；GCC 16.2.0；Debug dev 与 ASan presets。
- 固定真实核心：`/tmp/xrm-g1-xray-v26.9.9/xray`，输出 `Xray 26.9.9 (Xray, Penetrates Everything.) 52a412d (go1.27.1 linux/amd64)`。固定发布二进制在仓库外准备，SHA-256 `1eb9175d0f0a8f8149c9230a7fc5ae66ce332ed20a53155ce61fe62e3f58b7df`；没有改动系统 Xray。
- 本轮最终 dev build 成功；完整回归：**214/214 PASS，0 FAIL，0 SKIP**，其中 `real_core` **13/13 PASS**，包括 RC-01～RC-10、VLESS-WS-XHTTP、E2E-06 和扩展后的 E2E-07。命令为 `TMPDIR=/home/lisztzy/.cache/xrm-tmp XRM_TEST_XRAY=/tmp/xrm-g1-xray-v26.9.9/xray ctest --preset dev --output-on-failure`。
- 最终 ASan 构建与完整回归：**214/214 PASS，0 FAIL，0 SKIP**，其中 `real_core` **13/13 PASS**，总耗时 416.61 秒。此前针对短命 fake-core 的观察窗、PTY 目标帧及 PROC-03 fixture 的修正没有降低行为断言；sanitizer instrumentation 下的 RSS 不作为 Debug 产品上限。
- 固定核心命令为 `xray run -test -confdir <test-confdir>`、`xray run -confdir <test-confdir>` 以及 RC-01～RC-10/E2E-06 gate 所调用的同一 v26.9.9 binary；没有公网节点，核心行为用例均 loopback。CTest 最终 real-core label 两套构建均无 skip。
- systemd unit 静态验证及 `xrmd` 子进程归属测试通过；此环境未确认可安全隔离的 systemd 实机测试环境，未启动/停止宿主 systemd unit，故 systemd 实机清理 **NOT RUN**。不要将 PKG-01 的静态/归属证据描述为 systemd 运行态测试。
- release candidate 总门：**G5 FAIL，待用户复验**。此前人工操作已明确发现导航、滚动、订阅历史库存和现有 Profile 本地入口问题；这些不能由旧自动测试抵消。本轮加入只读历史预览/确认及旧 Profile 本地入口提示，隔离副本与 PTY 定向验证已覆盖，但生产数据未处理、完整人工主流程尚未由用户重新验收，因此不能声明 G5 PASS。

## I50 / I51 / I52

| Task | 状态 | 证据 | 限制 |
|---|---|---|---|
| I50 | PASS（实机环境限制已披露） | `xrm_packaging_tests [packaging]` 2 cases / 48 assertions；PKG-01 unit 静态策略、LIFE-10 验证 xrmd 退出只清理自己拥有的 Xray child、socket 关闭且无关 Xray 存活。 | 没有隔离 systemd 环境，实机启停 NOT RUN。 |
| I51 | PASS | RC-01～RC-10、VLESS-WS-XHTTP、E2E-06 与 E2E-07 使用固定 v26.9.9，13/13 real_core PASS；E2E-06 包含两个 Manager 客户端共用一个 Server，订阅刷新 A→B 时运行 PID、confdir 和所有文件字节不变，显式选择并 connect 后才采用 B，TUI 退出不停止 core，Server 重启后持久状态一致。 | VMess/Trojan/Shadowsocks/Hysteria2 保持 traffic-unverified。 |
| I52 | PARTIAL | UI-01～UI-09、E2E-07、PERF-01/02 自动验收覆盖完成；真实 80×24 PTY 自动化与固定 v26.9.9 loopback cold-start 流程已通过。 | 用户人工重跑完整 TUI 冷启动流程尚未发生；人工 checklist 结果留空待操作者填写。 |
| G5 | FAIL | 用户人工操作明确发现旧库存和已有 Profile 的入口缺口；本轮定向修复不能反向抹去失败记录。 | 新入口完成开发者隔离验收后，仍须按 [tui-manual-acceptance.md](tui-manual-acceptance.md) 由用户复验。systemd 实机限制仍为 NOT RUN，且不要求宿主机操作。 |

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
| T25 | NOT RUN | UI-01～UI-09 的相关自动 PTY 测试通过，包含 log pause/resume、disabled module、无 Profile、Profiles 管理、saved/actual target 和 external editor conflict。冻结标准要求的完整人工验收尚未执行。 |
| T26 | PASS | HTTP-02、PERF-01、RPC-04；HTTP/core-check barrier 下其他 UDS CLI status/logs 仍响应，stop 在约 106 ms 完成，写请求不会重放。 |
| T27 | PASS | CT-01、RC-01/08；固定真实 Xray 实际运行，所有协议 fixture 脱敏，real_core 最终无 skip。 |
| T28 | PASS | E2E-06/07、CLI-01、UI-09；两个客户端端到端流程及 Profile 名称/UUID 寻址通过。 |
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

- PTY 自动验收：真实 80×24 pseudo-terminal。UI-01～UI-09 对应测试和 E2E-07 均通过：覆盖中文、emoji、长名称、键盘导航、零 Profile global pages、Profile create/rename/clone/delete、name-oriented picker、viewed ≠ running、saved target ≠ actual target、日志 follow/pause/历史/resume、Server buffer 淘汰提示、external editor 和退出后核心继续运行。新增 PTY 检查已在 dev 与 ASan 全量中通过。emoji 验证显示与 UTF-8 保持，不声称覆盖终端输入法。
- UI-05 外部编辑器：真实 TUI PTY 与测试 fake-editor 子进程覆盖 `$VISUAL` 优先、`$EDITOR` fallback、均未设置时明确错误、正常 0、nonzero、取消、无效 JSON 草稿保留、另一 CLI 并发修改同一文件后触发 conflict；源文件未被覆盖。fake-editor 只存在于测试 support，生产代码不识别测试模式。
- Logs Pause：真实 TUI `p` 暂停/恢复；暂停期后台日志仍到达，viewport 稳定，上下键可翻历史；Server 环形 buffer 淘汰旧日志后显示 `older logs dropped / truncated`；Resume 跟随 tail；`q` 后 CLI `status` 仍显示 core running。UI-02/T25 与 5 MB 日志淘汰 PTY 用例均 PASS。
- 人工复验 **NOT RUN**：自动化完成后，本轮尚无用户复测记录；更新后的 [tui-manual-acceptance.md](tui-manual-acceptance.md) 是待执行 checklist。saved target 与 actual target 的人工观察、真实小终端交互及对用户订阅节点的实际刷新结果仍待用户确认。
- 旧库存整理（开发者隔离操作）：一致性备份的隔离副本初始为 49 当前 + 52 `source_missing`。应用预览将其中 46 条归为无冲突的冗余历史，6 条因名称/上游身份不同不自动处理；这不是“把历史清零”。真实 TUI 预览后取消保持 52，明确确认一条后为 51。将来源切到缓存响应的本机回放 URL 时，首次响应与备份时的库存并非同一快照，发生一次库存变化；重新建立相同响应基线并再确认一条后为 49 当前 + 51 历史。此后同一响应本机重放 10 次及 Server 重启后再次刷新均报告 `added=0, updated=0, missing=0`，总数 100 和 ID 集合摘要稳定。生产订阅和数据库未改。已知引用和用户属性冲突均默认阻止确认，不展示认证数据。开发者操作不等于用户人工复验。
- 已有 Profile 本地入口：PTY 覆盖旧空 `inbounds` 的确认/取消和禁用模块仍需单独启用，也覆盖自定义入站保留；扩展 E2E-07 使用固定 Xray 26.9.9 从旧空入站 Profile 的 TUI `i` 确认、TUI 选节点与 Connect，到 127.0.0.1:10808 SOCKS 收到真实 VLESS loopback marker。保存源模块本身不 Connect，退出 TUI 后核心仍运行。
- PERF-01：本机 HTTP barrier 与 fake-core check UDS barrier 同时阻塞；CLI UDS clients 重复 status/logs 并 stop；41 assertions PASS。实测一个最终 dev 样本：status p50/p95 5.34/5.34 ms，logs 5.19/5.19 ms，stop 106.4 ms。ASan 对应测试 PASS。
- PERF-02：1000 个虚构节点、1 Profile、1 core；SQLite list/search、组展开/组装渲染、来源 reconcile、CLI status/logs 和 4 MiB bounded logs。dev 单机 p50/p95：node list 57.36/59.17 ms，search 56.60/58.25 ms，assembly/render 35.74/36.61 ms，reconcile 62.83/63.19 ms，log page 0.00211/0.00229 ms。测得初始/结束 RSS 5840/16184 KiB，seed 后 8 MiB log append 0.908 ms，FileSet 504143 B，日志保留上限 4194304 B。ASan 负载运行通过，但 RSS 不与 dev 比作产品内存限制。有限重复测量不证明长期无泄漏。

## 部署、help 与范围检查

- `packaging/systemd/xrmd.service`：`Type=simple`，`ExecStart=/usr/bin/xrmd --foreground`，明确 system identity/data/runtime paths，`Restart=on-failure`、`RestartSec`、`KillMode=control-group`、停止超时及 `UMask=0077`；未添加 `xray.service`。安装 unit 不 enable/start；文档没有 sudo/setcap/防火墙/DNS 自动操作。
- `docs/usage.md` 覆盖首用、daemon/core/subscription/profile/modules/groups/connect/status/logs/stats/stop/restart/install/use/export 与常见错误；命令名称以已实现 CLI 为准。
- `build/dev/xrm --help`、`build/dev/xrmd --help`，以及 core/node/sub/profile/config/group/select/connect/disconnect/status/logs/stats/tui、`config statistics`、`core use` 的 `--help` 均执行成功。
- `git grep -n -E 'TODO|FIXME|NOT IMPLEMENTED|placeholder|return fixed success|fixed success' -- src apps packaging` 未找到用户可达的假成功或产品 stub；匹配为空。已知 TUI 不足均为显式未实现/未运行，不是隐藏成功状态。
- 默认路径检查：未发现自动 sudo、setcap、systemd enable、系统 DNS/路由/防火墙修改、pkill、覆盖 `/usr/bin/xray` 或自动升级运行 core；systemd unit 只管理 xrmd，由 xrmd 拥有自己的 Xray 子进程。

## 未满足项与 v1 外事项

本轮修复：VLESS Codec 接受 Xray v26.9.9 下已确认可中性映射的 headerType=none、quicSecurity=none、insecure=false，并将 TLS ECH 参数映射到 streamSettings；不支持 TLS insecure=true 等无法保真映射的输入仍明确拒绝。VLESS 的 RAW/TCP、WS、XHTTP 脱敏形状有 codec 测试，WS/XHTTP 另有固定核心 loopback 测试。未取得用户私有订阅的原始 URI，因此不能声称复现每条实际 URI。

本轮还修复无 source 的 disabled module 打开时 JSON null 转字符串导致 SIGABRT；现在先建立内存草稿，用户显式保存后创建 source，启用仍是单独操作。新增无需 `--profile` 的 TUI 首用流程：订阅添加/更新、节点浏览、登记已有 core、创建/选择 Profile、保存节点选择并 connect；保存不会自动连接，TUI 退出不会 stop core。外部编辑器有 F6/v 明确入口并继续只修改草稿。

G5 当前为 FAIL；旧库存只在隔离副本中预览和选择性处理，用户生产库存尚未处理，已有 Profile 本地入口需用户明确确认。完整人工 TUI 复验完成前不能作最终判定。systemd 实机运行仍未执行；unit 静态审查和进程归属集成测试已完成，环境限制已披露且本轮不触碰宿主服务。Future / Not in v1：VMess/Trojan/Shadowsocks/Hysteria2 loopback traffic peer 验证；leastLoad 稳定性/真实行为验证；systemd 隔离环境实机验收；长期 RSS 斜率。没有实现 M6 或新增产品范围。

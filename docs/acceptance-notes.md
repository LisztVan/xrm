# v1 交互与性能验收记录（I52）

日期：2026-09-24。此记录区分自动 PTY、性能实测与必须人工执行的项目；未观察到的行为不记为通过。

## 环境与命令

验收 worktree 基线为 `940c78c`，另集成主 Agent 的 TUI 修复 `8a1bdd4`（帮助/Tab 导航）和 `35cdf87`（新搜索重置节点过滤）。I52 测试目标使用独立的临时构建目录，复用仓库已下载依赖：

```sh
cmake -S /tmp/xrm-m5-i52 -B /tmp/xrm-i52-build \
  -DCMAKE_BUILD_TYPE=Debug -DXRM_BUILD_TESTING=ON \
  -DFETCHCONTENT_BASE_DIR=/home/lisztzy/Projects/xrm/build/dev/_deps \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build /tmp/xrm-i52-build -j4 \
  --target xrm_tui_acceptance_tests xrm_performance_tests
```

构建通过。本 agent 的执行环境禁止测试子进程绑定 Unix domain socket：xrmd 输出 `绑定Unix Socket失败: operation not permitted`。未经修改的既有用例 `build/dev/tests/integration/m1/xrm_m1_tests '[UI-01]'` 同样在 `server.start()` 处失败。主 Agent 在可运行 UDS 的环境完成了修正后的 PTY 运行：`xrm_tui_acceptance_tests '[I52]'`，1 case / 44 assertions PASS，exit 0。之前的失败来自用例在已提交搜索之后误发全局 Escape 退出键；现已移除该按键。性能目标也由主 Agent 在可运行 UDS 的环境通过，待把最终输出的全部测量值写入本记录。

## 自动 PTY 覆盖

`tests/tui/acceptance/acceptance_tests.cpp` 创建 80x24 PTY，并准备 ASCII、中文、emoji 和长名称的临时节点。验证 `?` help、Tab/Shift-Tab 页面导航、上下/Enter、中文搜索、emoji 节点显示（以 ASCII 后缀过滤，避免依赖终端输入法如何向 FTXUI 注入 emoji 事件）、过滤输入的 `q`/`e`/空格不会触发全局动作，以及截取屏幕文本仍为合法 UTF-8。长名称断言已补；修正键序后的复跑结果待主 Agent确认。

状态：**PASS（自动 PTY）**。修正后由主 Agent 在可绑定 UDS 的环境运行，1 case / 44 assertions，exit 0。覆盖范围以本节前述项目为准；这不替代下表中的人工/未实现交互验收。

以下依赖实际手动操作或当前产品并未暴露对应交互，未声称完成人工验收：

| 项目 | 状态 | 说明 |
|---|---|---|
| 无鼠标完成核心连接流程 / 80x24 可达性 | NOT RUN | 自动用例只覆盖节点浏览/搜索/按键；尚未做完整人工 connect 流程。 |
| 后台刷新不抢焦点、按 ID 保持选择 | NOT RUN | 未做可控后台刷新与当前焦点的端到端 PTY 检查。 |
| 编辑稿不被后台刷新覆盖 | NOT RUN | 未获得真实 TUI 多客户端并行编辑验收。 |
| 日志暂停后保持滚动位置 | NOT IMPLEMENTED / NOT RUN | 当前 TUI 日志页只有显式 `r` 刷新，没有暂停/自动订阅交互；未添加产品功能。 |
| 外部编辑器成功、非零退出、取消、冲突、无效 JSON、终端恢复 | NOT IMPLEMENTED / NOT RUN | 当前仓库未发现外部编辑器启动入口；TUI 使用内嵌原生 JSON 编辑器。没有用手工测试替代缺失入口。 |
| saved target B 与 actual A、runtime override 的屏幕并列展示 | NOT RUN | 源码中 Overview 有 saved/applied/override 字段；既有状态/API 与真实 override gate 属于 M3 覆盖，但本轮没有得到小终端屏幕验收证据。 |
| 退出 TUI 不停止核心 | NOT RUN（本轮） | 既有 M1 `[UI-01]` 用例覆盖此行为；本轮因 Server 无法绑定 UDS 未能复跑。 |

## PERF-02：1000 节点测量

测试目标 `xrm_performance_tests` 使用临时 SQLite 与虚构节点，不访问公网。数据为 1000 个有不同 endpoint/upstream ID 的节点、一个筛选组。重复次数：Store 列表/搜索各 60 次，组展开+组装渲染 40 次，reconcile 40 次，日志读取 60 次。RSS 取 `/proc/self/status` 的 `VmRSS` / `VmHWM`；4 MiB 日志上限直接检查 `LogBuffer::bytes_retained()` 与旧 cursor `truncated`。

该次 Debug 测量输出（单次主机结果，不作为跨机绝对门槛）：

| 路径 | p50 | p95 |
|---|---:|---:|
| SQLite node list（100条分页） | 57.53 ms | 58.23 ms |
| SQLite node search（`edge-99`） | 56.95 ms | 57.68 ms |
| group expansion + runtime assemble/render | 35.93 ms | 36.77 ms |
| 来源内 reconcile（1000节点） | 63.28 ms | 63.81 ms |
| 日志 page read | 0.0021 ms | 0.0024 ms |

其它观测：seed 86.7 ms；8 MiB 日志追加约 0.98 ms；FileSet 合计 504143 字节；保留日志 4194304 字节（正好达到 4 MiB 上限）。机器标识 `Linux 7.2.7+deb14-amd64 x86_64 / debian-work`，CPU `Intel(R) Core(TM) i7-14700F`，GCC 16.2.0，Debug，报告 28 个逻辑线程。测试进程 `VmRSS` 初始/结束为 5836/16300 KiB；重复组装 40 次的区间 `VmRSS` 为 10952 KiB 起、最高 13496 KiB；`VmHWM` 初始/结束为 5904/16300 KiB。现使用同一个 `/proc/self/status` 的 VmRSS/VmHWM 字段，没有将历史 `getrusage` 峰值冒充当前 RSS。重复组装区间的增长上限为 32 MiB，并断言结束 VmHWM 不低于 VmRSS。

CLI status/logs 的真实 UDS 调用循环已经由主 Agent 在 socket-capable 环境运行通过；`performance_tests.cpp` 现在会在成功时把完整单行测量记录直接打印到测试 stdout。最终测量数字仍待从该次 stdout 抄录到上表/记录。reconcile/assemble 和缓冲区数据是产品真实逻辑，但不等价于 TUI 绘制全链路延迟。

Metrics 多 watcher 与无人 watcher 停止高频采样已有独立 `[MET-02]` 集成测试，覆盖两个 viewer 共用一个采样器、最后一个 viewer 离开后不再轮询；I52 专项未重复计时此行为。性能测试当前未测长期持续 RSS 斜率或每节点线程数，不能由有限重复次数推断无泄漏。

## PERF-01 与故障/编辑器验收

PERF-01 要求用同步屏障同时阻塞本机 HTTP 下载和 core check，并由第二真实 UDS 客户端重复调用 status/logs/stop。本轮尚未添加完整的组合屏障测试，也未取得 UDS 测试环境，因此状态为 **NOT RUN**。已有 HTTP-02、LIFE-04 与日志/Metrics 用例各自覆盖的边界不能替代此组合门。

外部编辑器、暂停日志、saved/actual 屏幕差异以及长期重复操作的 RSS 斜率是本轮剩余验收项目。这里仅记录现状，不新增 M5 以外产品功能。

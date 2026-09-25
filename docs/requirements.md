# XRM 需求文档

**状态：首版实现与验收记录见 `release-acceptance.md`；人工验收失败后的修复仍在进行 · 整理日期：2026-09-24**
**形态：Manager（CLI/TUI）+ Server（daemon）+ 独立 Xray-core**  
**语言：C++23 · 首发平台：Linux x86_64 / aarch64**

> 产品定位是完整的终端代理管理软件：**订阅入库 → 管理 Xray 原生模块 → 选择节点或策略组 → 按原生模块输出多文件配置 → 启动独立核心 → 查看日志和统计**。DNS 只是一个模块示例，不是产品的配置能力边界。不是单纯的文件启动器，也不是通用配置编译平台。
>
> 本文定义使用行为与验收范围；实现结构见 [architecture.md](architecture.md)，编码 Agent 约束见 [AGENTS.md](../AGENTS.md)。命令、数据结构、默认值和里程碑均为设计约定，不表示已有实现。外部事实标记 `[Xnn]`，来源列在架构文档末尾。

## 1. 已确定的产品边界

| 事项 | 要求 |
|---|---|
| 唯一代理核心 | Project X 的 Xray-core，不增加其他核心后端 |
| 对接方式 | 调用独立 `xray` 程序，不依赖 libXray，不嵌入 Go runtime |
| Manager | `xrm`，提供 CLI 和 TUI，共用同一套 Server 接口 |
| Server | `xrmd`，负责订阅、节点、配置、运行准备及它创建的核心子进程 |
| 节点保存 | 订阅节点和手工节点独立入库；不是某个配置文件的临时缓存 |
| 配置管理 | 以官方原生顶层模块及其子对象为边界；模块可分别查看、编辑、保存、启用和输出；范围见 R06 |
| 运行输出 | 原生顶层模块分别写对应文件；嵌套功能写回所属模块，不按每个 UI 页面虚构顶层配置；最终以 `-confdir` 运行 |
| 禁止的捷径 | 不把所有模块合成单一 `config.json`；不把刷新订阅直接等同于改运行配置 |
| 实例范围 | 一个 Server、多套 Profile、同时最多一个生产 Xray 实例 |
| 远程使用 | 本机 Unix Socket；远程通过 SSH 使用终端，不建设远程管理平台 |
| 通用性 | 来源、分组、DNS、规则、端口、目录均可由使用者决定，不绑定某个机场或路由器 |

**模块命名、字段名称和层级沿用 Xray，不用 `router` 代替 `routing`、不用 `observer` 代替 `observatory`，也不生成顶层 `groups`、`tls`、`sniffing`。方便用户的页面可以聚合多个模块，但不能改变原生归属。**[X10][X11][X12][X13]

v2rayN、v2rayNG、xrat 仅用于说明“节点、设置、连接、日志/统计”的操作体验，不据此引入它们的全部功能、核心或实现方式。

### 1.1 不在当前范围内

多核心抽象、插件平台、Web UI、多租户、远程 HTTP 控制面、节点扫描平台、计费面板、任意脚本执行、复杂配置 DSL、编译器 passes、全 Xray schema 验证器、通用热重载 diff 引擎、无限快照和配置历史。

不默认修改系统 DNS、防火墙、路由或系统代理。用户在入站等模块显式启用 Xray 自身的 TUN/透明代理行为时，程序应说明其系统影响，但不另建整机网络管理器。

## 2. 三类数据与基本术语

| 概念 | 含义 | 保存位置与修改方式 |
|---|---|---|
| 节点库 | 可供使用的代理节点及其来源、名称、连接参数 | SQLite；通过订阅/节点功能更新 |
| 原生模块配置 | Xray 官方定义的顶层配置及其完整子对象 | Profile 的 `modules/` 文件；保存原生字段和层级 |
| 管理器绑定 | 动态组成员筛选、默认选择如何接入路由、附加观测节点范围 | `bindings/` 普通设置文件；只在运行准备时解析，不进入核心配置 |
| 当前选择 | 本次希望使用的默认节点/组，以及各手选组的选择 | 作为公开可见的用户设置保存，不是隐藏 API 状态 |
| 运行配置 | 根据上述内容准备的完整 Xray 文件集合 | Server 的运行目录；只含原生核心配置 |
| Profile | 一套模块配置、组、选择和运行环境的集合 | 不是单纯的目录别名，也不把全部内容塞进一个配置字段 |
| 运行记录 | 当前进程、实际核心、实际文件目录和已应用选择 | Server 保存必要元信息；不与尚未应用的设置混为一谈 |

**用户设置与运行配置有明确分工。** 修改设置不立即影响连接；“连接/应用”才准备运行文件并交给核心。选定节点和分组中的节点参数来自节点库，不复制进 DNS、路由等用户设置。

原生源文件统一采用 `{ "原生模块名": 原生值 }`，包括 routing、观测，不用自定义 RoutingSettings/ObservationSettings 替代其完整原生结构。辅助信息放在 `bindings/`：动态组成员、选择和自动补充观测范围没有 Xray 顶层键，不混入模块正文。

源模块可能引用尚未从节点库输出的 tag，且还没有包含动态组的最终展开结果，所以只有生成的运行 `confs/` 保证是完整的核心输入。界面区分“原生模块设置”“管理器绑定”“运行文件”，不要求用户直接运行源设置目录。

## 3. 用户主流程

### 3.1 获取与管理节点

添加订阅 → 获取 → 解析 → 保存节点 → 浏览、筛选和查看参数。用户也可以直接导入分享链接或创建手工节点。

导入/刷新到此结束：**不生成节点出站文件，不写运行目录，不重启核心。** 节点数量变化会显示“库存已更新；应用后使用新节点”。

### 3.2 配置使用方式

创建或选择 Profile，在原生模块目录中管理所有需要的配置。节点选择、分组、路由、观测和统计提供方便的功能视图；每项操作显示实际属于哪个原生模块和哪个文件。常用操作应有实际 CLI/TUI 功能，不能只用“打开编辑器”代替全部配置管理。

高级设置允许查看或编辑对应模块的原生字段。软件不要求给 Xray 每个高级字段制作表单，也不能无声丢弃尚未出现在表单中的字段。

### 3.3 选择并连接

用户选择一个节点或策略组作为默认目标，再执行连接。也可以只保存选择，稍后连接。

默认目标只决定未被更具体规则处理的流量；用户已有的直连、阻止和其他组分流仍有效。“选择一个节点”不等于强行把所有规则变成全局代理。

### 3.4 运行与调整

连接后查看日志、核心状态、流量与观测结果。可以重新选择并连接、应用设置、停止或重启。退出 TUI 或断开 SSH 不停止核心。

首版普通切换允许通过检查后重启实现。软件要清楚提示连接可能中断，不为追求无重启而先建设复杂 API 同步系统。

## 4. 功能要求

### R01 · Manager / Server 与后台运行

两个产品程序属于同一个项目和发布包。Manager 负责交互、编辑稿与展示；业务修改经过 Server。Server 不处理用户实际代理流量。

CLI 与 TUI 是两个独立交互前端。CLI 是面向 shell、SSH、Agent 和自动化的 command-oriented 前端：使用显式参数、便于脚本调用、无状态或弱状态。TUI 是长驻、GUI-like、面向人工操作的交互前端，维护自己的 UI session context；正常流程不得要求用户输入内部 UUID、RPC 参数或 CLI 前置参数。

两者共享 xrmd business API、DTO、业务语义和持久化数据，但不要求共享命令结构、导航、上下文、页面流程或参数输入方式。**“CLI/TUI 共用业务接口”不得解释为“共用交互模型”。** Server 不可达时说明连接位置和启动方法，不偷偷启动服务或提权。生产后台部署由 systemd 托管 Server，避免另一个 `xray.service` 同时管理同一实例。

### R02 · 订阅管理

支持新增、查看、编辑、删除、手动更新、批量更新和定时更新。来源包含 URL、请求头、User-Agent、更新周期和明确的下载方式。默认直接下载；指定代理失败则报错，不暗中改为直连。

默认定时刷新周期可设为 6 小时，允许关闭。**定时刷新只更新节点库，本方案的首版不自动发布到运行配置。** 删除来源时可明确选择将节点保留为手工节点，不自动改变正在运行的实例。

HTTP 失败、HTML 错误页面、无法识别正文或零个有效节点，保留上次有效库存。部分条目损坏时展示部分成功，保留未能确认移除的旧节点；不能把失败结果当成空订阅。明确不支持的协议单独报告。

### R03 · 节点库

节点支持列表、搜索、来源/协议/标签过滤、详情、手工增删改、导入、复制和启用/禁用。节点有独立 ID，名称或排序不是身份。

订阅刷新在来源内做确定性匹配，不跨来源模糊去重。订阅节点的参数由来源维护；用户可改别名、标签和启用状态。需要固定修改连接参数时，复制为手工节点。

被用户选择或引用的节点消失时保留“来源已移除”提示，不把原选择偷偷指向另一个同名节点。当前核心仍可继续使用已经加载的旧运行文件；下一次连接需用户处理无效引用。

### R04 · 节点参数和输入格式

C++ 自己维护协议、传输、安全参数及分享链接解析。首个闭环实现 VLESS 常用参数；完整首版逐步覆盖 VMess、Trojan、Shadowsocks 和目标 Xray 实际支持的 Hysteria2 常用格式。

输入先覆盖单个 URI、明文 URI 列表、Base64 URI 列表、原生单 outbound / outbounds 集合。Clash/Mihomo YAML 节点导入可以后续加入，不把其全配置转换作为前置条件。

常用字段强类型；复杂扩展允许原生 JSON。未知且影响连接的参数不能删掉后报告完全支持。原生高级 outbound 可保存，但管理器不得据此声称已适配所有对应分享链接与编辑表单。

VLESS URI 必须按字段语义识别。`headerType=none` 与非 QUIC 传输上的 `quicSecurity=none` 是兼容性中性默认值；`insecure=false/0` 不得变成跳过证书验证；`ech` 在 TLS 上写入原生 `tlsSettings.echConfigList`，按 URI percent-decode 一次并保留 `+ / =`。TLS `insecure=true` 在目标核心不能安全表达时明确报告待修正，不生成已移除的信任绕过字段。其他未知连接参数需指出具体参数，不能把已识别的节点协议全部混报为 malformed。

### R05 · Profile 和原生模块管理

Profile 支持创建、选择、复制、重命名、查看和删除。内部 Profile ID 是稳定 UUID，用于持久化、RPC、引用和精确寻址；人机交互优先显示/使用 Profile 名称，TUI 使用列表/选择器，不要求用户记忆 UUID。CLI 的 Profile ref 可为 UUID 或名称：UUID 精确存在时优先；否则按名称精确匹配，唯一名称成功、无匹配为 not_found、同名多项返回 ambiguous conflict；禁止模糊匹配或自动选择第一个。多个 Profile 共享独立节点库，不复制订阅数据。Profile 的运行目标可以是一个节点、一个组或用户明确配置的原生路由方案；手写服务端配置不必为了启动而伪造一个订阅节点。

每个原生模块具有统一的基本操作：**列出与状态、启用/停用、查看、编辑、保存、恢复本次未保存编辑、预览对应运行文件**。停用表示本轮不输出该模块，保留源设置；这是管理器行为，不向每个原生对象随意插入 `enabled`。

Enter 打开 disabled 模块不要求其先有源文件，也不自动启用。无源模块展示未配置状态与原生编辑/创建草稿；已有 disabled 源保持可读可编辑但不参与运行。创建模块正文与启用模块是两个明确保存动作。

界面中的停用准确含义是“不输出这段配置”，并不保证核心对应能力完全关闭；省略某些配置后核心仍可能采用默认行为。`outbounds` 的停用覆盖其静态与生成节点分片。若本次管理器选择明确需要一个已停用模块，提示启用或调整选择，不静默恢复该模块。

常用字段和对象有专用操作；其余字段由模块级原生编辑覆盖。读写、导出和组装不能因为表单未适配而丢掉未知字段。对象/数组/字符串等类型按该模块规范保留，不能把所有模块值都强制当成对象。

源文件允许外部编辑。Server 不用数据库中的镜像恢复它们。保存一个模块只改该模块；显式跨模块功能一次列明相关修改，不触碰其余内容。文本编辑保留原文；表单保存若会规范化当前文件的排版/注释，先说明，不建设自研 JSONC/CST 编辑器。

### R06 · Xray 原生模块清单与文件归属

#### R06.1 顶层模块

以下以本次核对的官方配置总览所列 **15 个顶层配置项**为覆盖清单。它们是配置管理的正式对象，不是“DNS 之外以后再说”的附加功能。文件名和编号是产品建议，不是核心要求。[X10]

所有模块均须有统一的模块管理入口；专用表单按里程碑逐步实施。未启用模块不生成文件，不需要每份 Profile 同时启用全部模块。

| 原生顶层项 | 主要管理内容 / 最小交互 | 原生源文件 → 运行文件 | 归属注意事项 |
|---|---|---|---|
| `env` | 核心配置环境变量的键值编辑 | `modules/12_env.json` → `12_env.json` | 与启动前进程环境分开；值按原生字符串映射保存 |
| `log` | 日志级别、目的地和原生附加字段 | `modules/00_log.json` → `00_log.json` | 是核心日志配置，不是 Server 日志设置 |
| `api` | 原生服务列表、tag、listen 等 | `modules/06_api.json` → `06_api.json` | 与 Manager–Server 的 UDS 协议无关 |
| `dns` | hosts、服务器列表、解析策略和各服务器原生字段 | `modules/01_dns.json` → `01_dns.json` | 不是 DNS 入站、DNS outbound 或宿主机 DNS |
| `routing` | 全部原生路由选项、有序 `rules`、原生 `balancers` | `modules/04_routing.json` → `04_routing.json` | 动态组展开也只能汇入这一个 routing 对象 |
| `policy` | 用户等级 `levels` 与 `system` 策略 | `modules/09_policy.json` → `09_policy.json` | 不仅是流量统计开关；其余超时/策略字段保留 |
| `inbounds` | 入站列表、协议 settings、streamSettings、sniffing | `modules/02_inbounds.json` → `02_inbounds.json` | 单个入站的子对象不拆成全局模块 |
| `outbounds` | 原生静态出站、选用节点、settings、streamSettings、mux 等 | `modules/03_outbounds.json` → `03_outbounds.json`；节点库 → `90_nodes_tail.json` | 同一个原生数组模块，允许静态/生成两份分片；一个模块负责人协调 |
| `stats` | 启用统计组件；保留原生空对象 | `modules/08_stats.json` → `08_stats.json` | `{ "stats": {} }` 与不输出模块不同；不要因 empty 而删除 |
| `fakedns` | FakeIP 池、容量、多池对象/数组 | `modules/10_fakedns.json` → `10_fakedns.json` | 与 dns、sniffing 分别管理；接入时可显式联动 |
| `metrics` | 指标导出配置及原生监听设置 | `modules/07_metrics.json` → `07_metrics.json` | 不替代 stats/policy，也不是 GUI 数据库 |
| `observatory` | 后台观测的目标范围与探测参数 | `modules/05_observatory.json` → `05_observatory.json` | 一种原生实现，不使用虚构的 `observer` 根键 |
| `burstObservatory` | 突发观测的范围与 `pingConfig` | `modules/14_burstObservatory.json` → `14_burstObservatory.json` | 独立根键和文件，不把它包进 observatory 对象 |
| `geodata` | 原生资源更新计划、assets 等 | `modules/11_geodata.json` → `11_geodata.json` | 配置文件与 `.dat` 资源不同；不复制核心的定时更新器 |
| `version` | 此配置允许的核心 `min` / `max` | `modules/13_version.json` → `13_version.json` | 不是管理器版本、安装核心版本或数据库 schema_version |

根键拼写按官方文档，例如 `fakedns`、`burstObservatory`；不从 C++ 成员名、中文名称或旧样例猜测大小写。FakeDNS 可为对象或数组；Stats 的空对象有启用意义。[X10][X14][X07]

**完整模块覆盖，不等于全量字段表单、任意组合都能运行或“所有核心版本都兼容”。** 每项提供可用管理与无损字段保留，核心行为以目标二进制为准。新根项可在独立原生模块文件中接入；不为此建立插件/跨版本能力平台。

#### R06.2 子对象也要对接，但保留其原生层级

| 用户看到的功能 | 必须写入的位置 | 不应生成的伪顶层文件/键 |
|---|---|---|
| 路由规则 / 策略组 | `routing.rules` / `routing.balancers` | 顶层 `rules`、`groups`、`balancers` |
| 用户等级与系统策略 | `policy.levels` / `policy.system` | 把每个 level 或统计开关写成另一份半个 policy |
| RAW / XHTTP / WS / gRPC 等传输 | 对应入/出站的 `streamSettings` | 全局 `transport` / `xhttp` |
| TLS / REALITY | 对应入/出站 `streamSettings.tlsSettings` / `realitySettings` | 顶层 `tls`、`reality` |
| Socket / FinalMask | 对应入/出站 `streamSettings.sockopt` / `finalmask` | 顶层 `sockopt`、`finalmask` |
| 嗅探 | 入站 `sniffing`；其他协议专有位置按对应文档 | 通用顶层 `sniffing` |
| Mux / XUDP | 出站 `mux` 的原生字段 | 顶层 `mux`、`xudp` |
| TUN / Tunnel / DNS 出站 | 对应 `inbounds[]` 或 `outbounds[]` 中的协议配置 | 顶层 `tun`、`tproxy`、`dns-out` |

协议特有 settings、VLESS 的反向功能等继续由所属入/出站管理。不能因为文档侧栏有一个功能页面，就推断它是一个顶层根键。[X11][X12][X13][X17]

当前文档已废弃旧全局反向代理；v26.9.9 拒绝旧顶层 `reverse`，全局 `transport` 也不再是可用的现代传输配置入口。导入旧内容可以展示并报告，不无声丢弃、伪造迁移或作为新 Profile 的默认输出。[X02][X17][X18]

#### R06.3 模块、分片和页面不是一回事

原生顶层模块是责任单位；文件是持久化/输出单位；页面是操作入口。通常一个顶层项对应一个文件，但 `outbounds` 可以按核心支持的数组规则分为静态与节点分片。传输表单可能出现在节点页，却必须更新这个节点将来所属的 outbound，而不是新建全局 transport。

一个对象型原生模块只有一个输出负责人。`routing.rules` 与 `routing.balancers` 汇入同一个文件，`policy.levels` 与 `policy.system` 同理。不能在两个文件写各半个对象并期待深合并。`inbounds/outbounds` 的 tag 和追加规则交给核心，禁止自研通用 deep-merge。[X01][X02]

#### R06.4 功能联动不是模块合并

“启用流量统计”可以一次帮助修改 stats、policy、metrics/API，但要列明实际改动，分别保存/输出这些模块；不能产生自定义顶层 telemetry。“FakeDNS 接入”可提示 fakedns、dns、相关入站 sniffing、必要 routing/outbounds 的关系，不能把它们塞进一个 DNS 文件，也不能擅自改宿主网络。

普通配置对两种观测采用二选一；关闭/切换时移除旧运行文件，保留源设置。这是本产品明确选择的交互约定，不能声称已经验证所有双观测组合。[X05]

模块覆盖的验收不只测 DNS：至少覆盖每一个顶层项的读写、启停输出，以及入/出站子对象正确归属。具体见 T29–T37。

### R07 · 策略组

支持手动选择、最低延迟、随机、轮询，以及后续完成目标版本验证的 leastLoad 高级模式。自动选择由 Xray 执行，不由 C++ 重新实现测速和选路。

成员可按明确节点列表或简单来源/协议/名称/标签条件选择。不同条件为 AND，同一条件多个值为 OR；首版不做任意表达式语言、嵌套组或组图框架。

手选组的选择是公开、可保存的用户配置。改变成员或选择只改 `bindings/groups.json`；其中原生 balancer 字段保持 Xray 的 `strategy`、`fallbackTag` 等名称，连接/应用时填成员并汇入 routing。缺失成员和空组要有实际提示，不能静默回退直连。

原生 `fallbackTag` 不等于按节点顺序故障转移，最低延迟不等于带容差的 Mihomo url-test；UI 不使用误导性的等价名称。[X04]

### R08 · 路由、默认选择和入/出站

原生路由源文件保存完整 routing 对象，使用真实的 `rules`、`outboundTag`、`balancerTag` 等，不把用户文件改成自定义 `match + target` schema。CLI/TUI 的节点/组选择器在保存规则时将选择落实为稳定 tag；高级用户直接管理原生规则。

当前默认选择通过单独可见的管理器绑定接入：local 预设开启“追加默认路由”，连接时在用户规则后追加当前目标规则；高级用户可关闭，完全由原生 routing/首出站决定。界面说明更早的用户规则优先，不能自动排序、删除 catch-all 或把所有流量改成全局代理。

入/出站模块均支持对象列表、添加、复制、删除、顺序/标签管理和高级设置。常用协议提供表单，传输、安全、嗅探、Mux 等在对应对象内编辑。节点库与静态 outbounds 是不同来源，界面清楚标出；不得对同一节点参数维持两个可写主副本。

静态 outbounds 不限于 direct/block：用户可配置 DNS、loopback、原生代理和其他核心支持的出站。它们保留原生 tag 和顺序；选用节点生成到 `90_nodes_tail.json`，不因订阅顺序改变首出站。需要复杂原生链式引用时可显式包含所需节点，不展开任意依赖图。[X01][X12]

### R09 · 原生模块的语义边界与关联

`dns` 是解析模块；DNS outbound 是出站协议；`fakedns` 是单独的地址池配置。管理器分别提供入口，允许显式的接入向导协调，但不把相似名称合并为一个不透明“DNS 引擎”。[X14][X19]

`policy` 编辑覆盖 levels/system 本身，不把它缩减成统计页的隐藏开关。Stats 负责启用统计组件、Policy 控制相关计数、Metrics/API 提供读取渠道；修改其中一项不代表其他项已经启用。页面把配置状态与实际数据可用性分开。[X07]

`observatory` 与 `burstObservatory` 保持各自原生正文。自动补充组候选的观测范围存为明确绑定，运行时与用户已写的 subjectSelector 合并去重，不覆盖探测参数，也不在 C++ 重做健康检查或最优选择。leastLoad 等组合按目标核心实测。[X05]

`geodata` 管理配置，不等于把资源二进制嵌入 JSON；首次资源准备由用户明确选择。启用核心自己的更新后不启动第二个竞争更新任务。[X15]

`env` 在配置读取后由核心应用。核心启动前就需生效的环境变量放在 Profile 的进程启动设置，不误写进 env 文件并声称有效。不得将这些变量设置到 Server 本身。[X16]

`version` 是可选原生模块，允许用户编辑 min/max；XRM 不为追求“版本一致”自动给全部配置加限制，不把 XRM 包版本写进去。兼容检查最终交给核心，研发记录单独注明实际测试核心版本。[X10]

所有未适配的原生字段可以保留并预览；这不等于已经实现其专用 UI 或验证了连接行为。需要运行的原生配置不应受分享链接 parser 支持范围限制。

### R10 · 选择、连接、保存和应用

| 动作 | 保存内容 | 运行效果 |
|---|---|---|
| `sub update` | 更新节点库 | 不生成配置、不重启 |
| 模块保存 | 更新该模块源文件 | 不重启 |
| `select` / 手选组保存选择 | 保存明确可见的节点/组选择 | 不改变当前进程 |
| `connect` | 可同时保存本次选择 | 组装当前已保存设置，检查并启动/切换 |
| `config render` | 不改源设置 | 生成独立预览目录，不启动 |
| `config check` | 不改源设置 | 生成预览并让 Xray 检查一次 |
| `config apply` | 不改变用户设置 | 正在运行则检查后切换；停止时准备并检查预览，保持停止 |
| `core start/restart` | 不改源设置 | 使用当前已保存设置和节点库重新准备文件，不使用未保存编辑稿 |
| `core stop` | 保存停止意图 | 停止，不被刷新/恢复重新拉起 |
| `group override --runtime` | 不改永久选择 | 可选高级临时覆盖；普通选择不依赖它 |

UI 默认提供“选用并连接”，不要求用户手改 tag 或分别执行多个底层命令。连接会应用当前 Profile 已保存的配置；若有其他待应用模块，一次提示其范围即可。

保存失败、连接失败、切换后恢复旧实例是不同结果。连接失败时可以保留刚保存的目标以便修复，但界面必须同时显示“希望使用的目标”和“仍在运行的旧目标”，不能谎称已切换。

### R11 · 多文件运行准备

只有连接、应用或显式 render/check/export 才将节点参数写入生成目录。节点范围包括当前默认目标、规则引用节点、相关组候选和用户明确要求的观测对象，不默认导出全部订阅。

生成的 `confs/` 不含 ID 索引、数据库记录、任务状态、组筛选表达式等管理器元数据。每个文件采用 Xray 原生字段；模块合并由核心完成。`-dump` 只用于诊断，不作为新的配置保存格式。[X01]

修改任一原生模块时，不改变无关模块源文件及运行内容。准备另一份候选目录时可以复制无变化文件；不要求零字节拷贝，不因此建设增量编译系统。禁用、重命名或移除模块后，新运行目录必须没有旧文件残留。

运行文件是可查看、可导出的产物，不是另一份双向同步的用户设置。编辑界面应引导修改对应源模块；需要独立修改可导出后自行使用。

### R12 · 核心生命周期和失败恢复

独立核心由 Server 创建、收集 stdout/stderr、停止并回收。只操作自己持有的进程句柄，不扫描接管旧 PID，不使用 `pkill`。

连接先准备候选文件并执行一次 Xray `-test`，失败不停止旧实例。检查通过后停止旧进程，启动同一份候选目录。实际启动失败时只恢复上一份成功运行配置一次。[X03]

`-test` 不是联网验收，也不保证端口和权限。启动状态、管理接口可达性、节点健康分开显示。普通启动不以访问某个公网网址成功为前提。

手动 stop 的意图优先于所有延迟启动和重试。自动崩溃恢复使用最近成功运行目录，不趁机发布用户正在编辑的新设置。回退不覆盖用户的模块源文件，也不回滚网络、证书或外部资产。

### R13 · 核心版本与更新

支持系统核心路径登记、读取版本和选择。系统核心不覆盖、不删除、不代替包管理器更新。

后续可安装官方发布到管理器私有目录，区分 stable/prerelease；安装不等于启用。默认不追 `main`、不自动升级运行实例、不引入 Go 构建链。

切换核心版本默认测试当前已运行文件集合，避免同时发布未应用的模块修改；用户需要一起变更时明确连接/应用。跟进最新版本需要记录实际测试标签，不把某个固定版本永久称为“最新”。

### R14 · 状态、日志和统计

展示 Server 连接、核心版本/PID、活动 Profile、已保存选择、正在使用的选择、实际运行目录、待应用模块、最后结果。

实时页使用核心提供的指标和观测结果。区别延迟、传输速率、累计流量和来源报告的额度；未观测不显示 0 ms，统计不可用不显示假零。核心重启后重置速率差分基线。

多客户端共享采样；默认有人查看时每 2 秒刷新，无查看者不高频采样。日志有界、可暂停自动滚动，分清 Server 和 Core 输出。

### R15 · CLI/TUI 人体工程学

TUI 顶层导航固定为 **Overview、Profiles、Modules、Subscriptions、Nodes、Logs、Config**，顺序和数字快捷键一致。Groups 是 Nodes 的特殊出口分类，Stats 汇入 Overview，核心管理进入 Config → Core；Config 另有 Appearance、Editor、General 二级页。Profile 是一级可管理对象。Overview 持续显示 Viewing Profile、Running Profile、Saved Target、Actual Target、Runtime Override、Core/version/state、订阅与节点计数、真实统计和 pending changes。Viewing Profile 可以不同于 Running Profile；浏览或切换 viewed Profile 不连接、不改变运行 PID/目录/目标，只有明确 Connect/Apply 才改变运行实例。当前高亮节点不得冒充 Actual Target。

Profile 是一套上网方案：引用核心、原生模块、组、保存出口、运行环境和启用的共享订阅来源。来源/节点与核心仍全局共享，不复制入 Profile。来源的自动刷新开关不同于“此 Profile 使用来源”；新 Profile 明确选择来源，旧 Profile 缺失该字段时保持此前全部来源可见的行为。来源绑定只在下一次显式连接/应用时改变运行范围，失效的保存出口必须报错，不自动换节点。创建标准本地代理 Profile 时，一次性生成覆盖 15 原生模块的可编辑样例/说明，默认只启用有效基础组合，不自动接管系统网络或启用全部高级模块；没有登记核心时仍可先保存方案。

Nodes 二级分类为全部订阅、各来源、Local、Groups，按当前 Profile 的来源绑定筛选日常订阅视图；无 Profile 时仍可浏览共享库存。Local 可用表单创建已支持的 typed 节点，也可通过原生 outbound 高级入口添加。Group 仍用既有 GroupBinding 和 routing.balancers，不成为协议节点；Modules → routing 显示相同动态组的只读对应关系并导向同一组编辑入口。

Config 只管理 XRM 设置与核心资源：Core、Appearance、Editor、General。程序设置存数据库，主题与编辑器不写 Xray 原生文件。安装核心不等于选择；为 Profile 选择核心不等于切换当前运行实例。外部编辑器由 Manager 按显式设置、VISUAL、EDITOR 的顺序选择并在 Manager 主机运行。

原生模块页必须列出 R06 全部项目，显示原生键名、中文名称、included 状态、源/运行文件和表单/原生编辑支持。Nodes 的组分类、传输子视图和 Overview 的统计区域只是方便操作/查看的入口，不创建平行配置体系。模块 Enter 对 disabled 或无源模块也可用：有源时打开其草稿；无源时打开未配置草稿但不落盘、不启用；只有明确 Save 创建源，Enable 是独立操作；included 但无源、无效草稿等状态均须明确展示。

直接运行 `xrm tui` 即可进入，不需要 `--profile`。Profile 数量为 0 时正常进入，Overview、Profiles、Subscriptions、Nodes、Config → Core 可用；Modules、Groups 分类和 Connect 显示 “No profile selected / Create a profile to use this feature” 等明确空状态。1 个 Profile 时自动作为 viewed Profile，但不自动 connect；多个 Profile 恢复最近有效的 viewed Profile 或显示 picker，不能随意取第一个。TUI 的 Profile 页面支持 list/create/open/rename/clone/delete，全程通过可读名称/选择器完成人工操作，不要求 UUID。上下文切换不连接、不自动应用。

节点库是全局共享的；订阅和节点操作不依赖 Profile。Connect 在无 Profile 时于 TUI 内提供 Create Default Profile、Open Profiles、Cancel 选择，不要求退出后使用 CLI。复杂原生文本编辑器应显眼提供 “Edit Native” 和 “Open External Editor”；内嵌编辑提供多行、Unicode 光标、删除、撤销、清楚的保存/取消键。

Enter 查看；明确的“选用并连接”才执行连接。Tab/Shift-Tab、方向键、Esc、搜索和帮助可用；输入框里的字符不触发全局命令。退出 TUI 不停止核心。

80×24、中文、无鼠标可操作。刷新不打断正在编辑的内容，不把列表选中项跳到第一行。操作完成显示对象、结果和实际运行影响，不堆叠多次确认。

CLI 支持非交互、JSON 输出和明确退出码；进度写 stderr。长操作默认等待结果，`--no-wait` 只表示已接受。客户端断线不自动重发写请求。

### R16 · 正确性与复杂度边界

保留正常 TLS 校验、本机文件/socket 权限、输入大小与超时、原子替换单个源文件、合理进程退出处理和一次完整核心检查。它们用于避免具体故障，不建设安全平台。

Server 串行提交业务修改；同一时间只执行一个运行切换，冲突提示忙而不是积压多次重启。编辑同一个源文件时做一次局部覆盖检查；不做全局版本树、内容寻址、全规则一致性审查。

## 5. 拟议 CLI 首用路径

下列命令是设计接口，尚无实现。

```bash
xrmd --foreground
xrm core register /usr/bin/xray --name system
xrm sub add airport-a --url-stdin
xrm sub update airport-a
xrm node list

xrm profile create home --preset local --core system
# 这些都是同一套原生模块接口，不是 DNS 特例
xrm config modules --profile home
xrm config edit routing --profile home
xrm config edit policy --profile home
xrm config edit fakedns --profile home
xrm config edit geodata --profile home
xrm config edit burstObservatory --profile home
xrm group add AUTO --profile home --mode least-ping --source airport-a
xrm select --profile home --group AUTO
xrm connect --profile home

xrm status
xrm logs --follow
xrm stats --watch
xrm tui
```

没有订阅时可以导入手工节点完成相同闭环。预设是一次性创建的普通模块文件，没有模板继承；缺少可用节点时明确提示选择节点或明确直连。

| 命令族 | 主要操作 |
|---|---|
| `server` | info、status |
| `sub` | add、list、show、edit、update、remove |
| `node` | list、show、add、import、edit、copy、enable、disable、remove |
| `profile` | create、list、show、clone、rename、remove |
| `config` | modules、files、show、edit、enable、disable、render、check、dump、apply、export、rollback；模块参数使用原生键名 |
| `group` | add、list、show、edit、remove、select；后续 override/clear --runtime |
| `core` | register、list、use、start、stop、restart；后续 releases/install/remove |
| 连接与观察 | select、connect、disconnect、status、logs、stats、tui |

`disconnect` 与 `core stop` 同义，不建立第二种停止语义。`config files` 必须能区分 source/runtime。正常 `select` 保存；`connect --node/--group` 合并选择与连接动作。

## 6. 验收场景

| 编号 | 必须证明的行为 |
|---|---|
| T01 | Manager/Server 分离；CLI/TUI 操作同一数据，退出界面不停止核心 |
| T02 | 订阅首次导入与刷新只改变节点库存，不创建或修改运行节点文件 |
| T03 | 下载失败、错误页面、空结果保留旧节点；部分损坏可解释 |
| T04 | 节点改名/重排不错误改绑；缺失的已选节点不自动变成另一个节点 |
| T05 | 对所有顶层模块参数化测试：保存仅改变目标源文件；无关源字节不变，不只测试 DNS |
| T06 | 分组可创建、筛选、手选和保存，不只是只读文件展示 |
| T07 | 选择单节点或组后连接，加载范围正确；默认选择不覆盖其他路由规则 |
| T08 | 输出保留多个模块文件；没有汇总运行用的大 JSON |
| T09 | R06 清单中全部顶层模块各有正确输出路径；管理器绑定不混入原生正文 |
| T10 | 路由与组只输出一个完整 routing；stats/policy 不拆成相互覆盖的片段 |
| T11 | 修改任一非联动模块后，只对应运行文件内容变化；候选复制不重格式化无关文件 |
| T12 | 禁用/删除模块后，下一运行目录没有残留文件继续生效 |
| T13 | 对 runtime confdir 使用真实 Xray 检查和启动；合并语义不是自研 deep-merge |
| T14 | 未保存编辑不应用；已保存设置在显式 connect/start/restart 时重新组装 |
| T15 | 配置检查失败不停止旧核心；启动失败只恢复一次，并报告真实目标 |
| T16 | stop 抢占运行切换和重试；定时更新不能恢复用户已停止的核心 |
| T17 | 普通选择持久保存；Server/核心重启不会依赖隐藏的临时 API 覆盖 |
| T18 | 明确 runtime override 不改永久选择；新实例按保存设置启动 |
| T19 | 程序生成文件可独立导出成多文件目录，同环境下由 Xray 直接读取 |
| T20 | 读取/文本编辑原生高级内容不丢未知字段；表单规范化仅针对目标模块 |
| T21 | 没有 API/Metrics 时能管理进程，状态不伪装成已有统计 |
| T22 | 流速计数重置、未探测延迟、慢日志消费者等显示正确且有界 |
| T23 | 运行目录和用户设置分开；回退不覆盖正在编辑的模块 |
| T24 | 系统核心只用不改；核心安装、选择和正在运行的版本分别显示 |
| T25 | 小终端、中文、编辑焦点、选用并连接、日志暂停可以实际操作 |
| T26 | UI 响应不因下载或等待子进程阻塞；断线不重放写操作 |
| T27 | 所有协议 fixture 脱敏；没有真实 Xray 时标记 SKIP，不冒充已验证 |
| T28 | 阶段交付有可运行用户闭环，无框架空壳、假状态和未经执行的测试声明 |
| T29 | R06 的 15 个顶层项都可列出、编辑、启停、预览并独立输出；支持表单与原生编辑的范围如实标注 |
| T30 | env/version 有独立文件；配置后环境不冒充启动前环境；不自动加版本限制 |
| T31 | stats 空对象不丢失，fakedns 对象/数组保留，burstObservatory 的大小写及独立文件正确 |
| T32 | 改入站 sniffing 只影响所属入站；改节点 TLS/REALITY/Mux 只改节点库和后续节点分片，无伪顶层键 |
| T33 | routing 与 policy 的未知原生选项保留；原生 balancers 与动态组展开共存但无同 tag 重复写入 |
| T34 | 普通观测类型切换删除旧运行模块，不同时遗留两种；用户原生探测参数不被自动范围覆盖 |
| T35 | 显式统计联动分别更新 stats/policy/metrics 或 API，Policy 其他用户等级字段不变 |
| T36 | geodata 文件不包含 .dat 字节；同一资源不被两个后台更新器争抢 |
| T37 | 旧 reverse/global transport 不作为当前模块误生成；不把嵌套 reverse 等任意挪到顶层 |

## 7. 实施阶段

每次只授权一个阶段中的具体闭环，不要求 Agent 一次实现全部产品。

| 阶段 | 用户闭环 | 暂不实施 |
|---|---|---|
| M0 | xrm/xrmd、UDS、状态、节点库基础、手工节点保存与列表、核心登记 | 不创建全业务空接口，不做订阅和核心安装 |
| M1 | Profile、15 项原生模块的统一管理/原生编辑入口、单节点连接、多文件检查/启停/日志、最小 TUI | 不要求15套复杂专用表单；不做全部协议和热切换 |
| M2 | URI/Base64 订阅、节点刷新与匹配、编辑/复制节点、节点页可连接 | 不做订阅直接写配置、整套 Clash 转换 |
| M3 | 入/出站与 routing/组、两种观测、policy/stats、DNS/FakeDNS 等常用操作，选择组与失败恢复 | 不做嵌套组、全字段 schema、永久 API 同步框架 |
| M4 | 常用 Codec、env/version/geodata/API/metrics 的配置交互完善、子对象编辑、核心安装/切换 | 不增加多核心、自动脚本、Web 控制台或网络接管平台 |
| M5 | systemd 打包、真实核心回归、交互验收、发布文档 | 不临时扩大协议/平台承诺 |

效率目标以千级节点、少量 Profile、单核心为参考；先保证正常交互及时响应、没有无界增长，再用测量解决瓶颈。不要先建设专用内存池、无锁容器或增量编译框架。

## 8. 交给编码 Agent 的起始任务

```text
阅读 AGENTS.md、docs/requirements.md 和 docs/architecture.md。
本次仅实施 M0：Manager/Server 通信、核心登记、节点独立保存和列表。
检查并复用仓库已有代码，不另建第二套服务或存储。
订阅不写配置；后续按 Xray 原生模块生成运行 confdir，子对象保留原生归属。
不能预建单 JSON 编译器；不将动态绑定字段混入原生模块。
交付真实 CLI 闭环和对应测试，报告执行结果，完成后停止。
```

本套文档只描述当前讨论方案，不包含版本迁移、历史变更或实施完成声明。

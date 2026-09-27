# 原生模块 Visual 覆盖与数据边界

本矩阵对应 `src/tui/forms/native_form.cpp` 的固定原生表单；所有 15 个模块有入口。它不表示所有 Xray 字段已有专门控件。产品交互以 [唯一交互契约](xrm-interaction-spec.md) 为准，G5 仍待用户复验。

Visual 的主体是原生语义分区，Enter 打开集合或多字段表单。集合的 Enter 是只读查看；Add/Edit 使用同一个 Form；Copy/Delete/Up/Down 是明确集合操作。子表单的 **Add to draft** 只改当前模块草稿，Modules 的 Ctrl-S 才通过 daemon 保存源文件；Connect/Apply 是另一个运行发布动作。Edit 关闭或 Runtime 视图时，同一只读条件传给全部子控件和提交回调。

## 字段和控件

`S` = TextInput（自由字符串），`N` = NumberInput（整数），`B` = 未设置/显式 true/显式 false 单选择器，`E` = Select，`M` = MultiSelect，`L` = StringList（列表 + 输入 + 增改删/重排），`A` = 明确标注的高级 TextArea/JSON。秘密使用 SecretInput。可选文本、数字、集合和高级 JSON 使用独立 presence；未设置、显式空字符串、0、false、空数组不会互相代替。枚举自带未设置选项，布尔自带三态，不叠加第二个 presence 控件。未设置字段不因打开、切换或提交其他字段而写入默认值。

| 顶层模块 | Visual 分区、实际字段与类型 | 集合操作 | 高级 JSON 范围 |
|---|---|---|---|
| `env` | Xray 进程环境：变量名 S、值 S | map 增查改复制删；键可重命名 | 非字符串已有值保留；并非 xrmd 环境 |
| `log` | access/error S；loglevel E；dnsLog B；maskAddress S | 单对象 | 未覆盖字段；maskAddress 允许原生自定义格式 |
| `api` | tag/listen S；services M | 服务多选 | 未知服务值显示已有值并保留 |
| `dns` | queryStrategy E；clientIp/tag S；disableCache/serveStale/disableFallback/disableFallbackIfMatch/enableParallelQuery/useSystemHosts B；serveExpiredTTL N | servers 增查改复制删/重排；hosts map 增查改复制删 | 复杂或未来类型明确留在 JSON |
| `routing` | domainStrategy E；rules 和 balancers 分区 | 两个原生集合完整增查改复制删/重排 | attrs/webhook、balancer strategy.settings 使用 A；规则未知字段保留 |
| `policy` | levels 按用户等级键管理；system 四个统计开关 B | levels map 增查改复制删，等级键 N | 保留每个 level/system 的未知字段 |
| `inbounds` | tag/listen S、protocol E、port 原生整数或字符串 | 增查改复制删/重排；已有连接进入原生嵌套分区 | 协议专属 settings、未表单化 transport 设置 A |
| `outbounds` | tag/sendThrough S、protocol/targetStrategy E | 增查改复制删/重排（首项顺序保留） | 协议专属 settings、proxySettings 等 A；动态库存出站仍由原组装边界生成 |
| `stats` | 无原生参数；可明确将 `{}` 放入草稿；Include 独立管理 | 无伪造字段 | 不因空对象而移除 |
| `fakedns` | ipPool S、poolSize N | 原生数组使用完整集合；已有对象保持对象表单 | 不自动把单对象变数组；更改表示形式使用 JSON |
| `metrics` | tag/listen S | 单对象 | 未知字段保留 |
| `observatory` | subjectSelector L、probeUrl/probeInterval S、enableConcurrency B | 前缀列表 | 未知字段保留 |
| `burstObservatory` | subjectSelector L；pingConfig 下 destination/connectivity/interval/timeout/httpMethod S、sampling N | 前缀列表 | 未知 pingConfig 字段保留；不改大小写 |
| `geodata` | cron S、outbound E；assets 每项 url/file S | assets 完整集合 | 仅 Xray 原生热重载模块；不冒充 XRM 缓存资源管理 |
| `version` | min/max S | 单对象 | 表达 Xray 版本约束，不是管理器版本 |

### 每模块操作与只读入口

以下是代码实际入口，非按截图数量推断。所有行都使用同一 `Edit == View && Source` 门槛；Runtime 和 Edit 关闭时只能查看。模块级包含/排除与对象字段相互独立；整模块的原生表示变更通过同稿 JSON。`删字段` 指显式切到未设置，不是把 0/false/空字符串当删除。

| 模块 | 查看/新增/修改/删除 | 排序 | 未知/可选字段与只读 |
|---|---|---|---|
| [env](evidence/review-remediation/modules/80x24-env-object-edit.txt) | 变量 map 查看、增加、编辑、复制、重命名、删除 | map 无人工排序语义 | 值为空字符串可保留；已有非字符串只读保留；摘要不显示值 |
| [log](evidence/review-remediation/modules/80x24-log-object-edit.txt) | 日志对象查看、设置/修改/删字段 | 不适用 | 可选文本/布尔/枚举；未知键与不兼容类型保留 |
| [api](evidence/review-remediation/modules/80x24-api-object-edit.txt) | API 对象查看、设置/修改/删字段，services 多选 | services 原顺序保留，不提供语义无效的排序按钮 | services 未知已有值保留；空数组与未设置分开 |
| [dns](evidence/review-remediation/modules/80x24-dns-object-edit.txt) | query/cache 对象；servers 增查改复制删；hosts map 增查改复制重命名删 | servers 上下移；hosts 不排序 | 地址字符串/对象维持表示；同稿未知字段/空数组保留 |
| [routing](evidence/review-remediation/modules/80x24-routing-object-edit.txt) | strategy 对象；rules/balancers 增查改复制删 | 两个数组均保留顺序并可上下移 | ruleTag 可省略；已有未知目标保留；动态 bindings 是独立只读出口 |
| [policy](evidence/review-remediation/modules/80x24-policy-object-edit.txt) | system 对象；levels map 增查改复制重命名删；每个 level 设置/删字段 | map 不排序 | 数值 0/布尔 false 保留；未覆盖 level/system 字段保留 |
| [inbounds](evidence/review-remediation/modules/80x24-inbounds-object-edit.txt) | listener 数组增查改复制删；已有对象进入 basic/stream/TLS/REALITY/sockopt/sniffing 等分区 | 上下移 | 端口数字/范围字符串保留；复杂协议 settings 使用高级 JSON |
| [outbounds](evidence/review-remediation/modules/80x24-outbounds-object-edit.txt) | static outbound 数组增查改复制删；已有对象进入 basic/stream/security/mux 等分区 | 上下移；首项语义不变 | 节点动态尾片仍由组装器负责；静态未知字段保留 |
| [stats](evidence/review-remediation/modules/80x24-stats-object-edit.txt) | 查看；显式将空对象加入草稿 | 不适用 | 无伪造参数；Include 独立；删除整个对象使用 JSON |
| [fakedns](evidence/review-remediation/modules/80x24-fakedns-object-edit.txt) | 原生数组增查改复制删；已有单对象直接 Form | 数组上下移；对象不适用 | 不自动互转数组/对象；poolSize 0 与未设置分开 |
| [metrics](evidence/review-remediation/modules/80x24-metrics-object-edit.txt) | endpoint 对象查看、设置/修改/删字段 | 不适用 | 不自动建立监听；未知字段保留 |
| [observatory](evidence/review-remediation/modules/80x24-observatory-object-edit.txt) | 对象与 subjectSelector 字符串集合增查改删 | subjectSelector 可上下移，原有顺序保留 | probeURL 别名保留；enableConcurrency false 不丢 |
| [burstObservatory](evidence/review-remediation/modules/80x24-burstObservatory-object-edit.txt) | 对象；subjectSelector 集合及 pingConfig 多字段 Form | subjectSelector 可上下移 | pingConfig 未知成员、sampling 0 保留；空数组不删除 |
| [geodata](evidence/review-remediation/modules/80x24-geodata-object-edit.txt) | reload 对象；assets 增查改复制删 | assets 上下移 | 不运行下载任务；原生资源检查仍属核心 |
| [version](evidence/review-remediation/modules/80x24-version-object-edit.txt) | min/max 对象查看、设置/修改/删字段 | 不适用 | min/max 空字符串可显式保存；不是管理器版本 |

上述集合的 Enter 是只读对象查看，Edit 是同一对象的多字段表单；只读模式不显示/执行写操作。没有把 Config → Tests 当作第 16 个原生模块。

### DNS 服务器和 hosts

服务器 Form 同时展示 address S、port N、domains/expectedIPs/unexpectedIPs L、queryStrategy E、skipFallback/finalQuery/disableCache/serveStale B、timeoutMs/serveExpiredTTL N、clientIp/tag S、原生表示 E。原生表示选择“Address only (string)”或“Server settings (object)”；添加对象字段时必须明确使用对象表示，不能为了转换丢弃已有字段。

hosts 的 Form 是域名匹配 S、单地址/地址数组 E、地址列表 L；重命名碰撞报错并留稿。服务器集合的顺序不会自动排序。

### Routing 规则与原生 balancer

规则 Form 包含 ruleTag S、domain/ip/sourceIP/localIP/user/process L、port/sourcePort/localPort/vlessRoute（保留整数/字符串）、network E、inboundTag/protocol M、outboundTag/balancerTag E，以及 attrs/webhook A。首屏明确 `Match conditions -> Target`；常见匹配后立即出现 Target 分组，其余匹配字段在 Additional match conditions，随后 Advanced。字段间为原生 AND；同一数组内为原生匹配列表。目标选项来自宿主提供的 Profile 范围引用，标签是提交值，可读名称只是显示。未知已有引用显式标记，不默认替换。新提交需有目标，修改目标时不能同时选 outbound 与 balancer；已有二者同时指定的原生对象保留核心的 outbound 优先行为。

`ruleTag` 是可选原生名字。审查 A 的 `Rule label *` 在当前实现中是**已修改字段**标记，不是必填校验；原实现没有 ruleTag validator，本轮不宣称修复了不存在的必填限制。现在标明可选及原生路径，并以无 ruleTag 的完整规则提交/JSON 保真回归核验。官方 [Routing RuleObject](https://xtls.github.io/config/routing.html#ruleobject) 明确其可选；[v26.9.9 router.go](https://github.com/XTLS/Xray-core/blob/v26.9.9/infra/conf/router.go) 对 ruleTag 直接赋值，要求的是 outboundTag 或 balancerTag。

balancer Form 是 tag S、selector L（出站标签前缀）、fallbackTag E、strategy.type E、strategy.settings A。可选策略 random/roundRobin/leastPing/leastLoad；后两者的观测依赖显示在字段说明。动态 Group 仍是 Nodes → Groups 的独立 bindings 表单，不生成原生顶层 groups。routing Visual 的只读 bindings 分支显示 Group 名称与生成 tag，进入时导向同一个 Nodes/Groups 入口，并先处理未保存模块稿。

### 入出站的真实嵌套路径

| 原生归属 | 专用表单字段 | 未专门表单化部分 |
|---|---|---|
| `*.streamSettings` | method/security E、address S、port N | raw/xhttp/kcp/grpc/ws/httpupgrade/hysteria settings A |
| `*.streamSettings.tlsSettings` | serverName/verifyPeerCertByName/fingerprint/minVersion/maxVersion/pinnedPeerCertSha256/echConfigList/cipherSuites/masterKeyLog S；alpn/curvePreferences L；rejectUnknownSni/disableSystemRoot/enableSessionResumption B；echServerKeys SecretInput | certificates、echSockopt、已弃用 allowInsecure 及其他字段 A |
| `*.streamSettings.realitySettings` | 客户端 serverName/fingerprint/shortId/spiderX/mldsa65Verify S、password SecretInput；服务端 target S、serverNames/shortIds L、privateKey/mldsa65Seed SecretInput、xver/maxTimeDiff N、minClientVer/maxClientVer S、show B | limitFallbackUpload/Download、数字型 target 等合法其他类型 A |
| `*.streamSettings.sockopt` | mark/tcpKeepAliveIdle/tcpKeepAliveInterval/tcpUserTimeout/tcpMaxSeg/tcpWindowClamp N；tproxy/domainStrategy/dialerProxy E；interface/tcpCongestion S；acceptProxyProtocol/v6only/tcpMptcp B | tcpFastOpen（bool/number）、happyEyeballs/customSockopt 显式 A，其余字段保留 |
| `*.streamSettings.finalmask` | 明确 Advanced TextArea | 完整原生对象；没有重新发明 mask 表达式 |
| `inbounds[].sniffing` | enabled/metadataOnly/routeOnly B；destOverride M；domainsExcluded/ipsExcluded L | 未知字段保留 |
| `outbounds[].mux` | enabled B、concurrency/xudpConcurrency N、xudpProxyUDP443 E | 未知字段保留 |

TLS fingerprint 文档还允许 uTLS 原生变量名，所以保留自由字符串，而不是限制为几个浏览器名称。所有协议 settings 的专属字段仍通过高级原生 JSON 编辑；本轮没有实现全 Xray 协议动态表单系统。

## 官方依据、条件与默认值

实际读取了 [官方全量文档](https://xtls.github.io/llms-full.txt)，对应模块章节，以及 [Xray-core v26.9.9 infra/conf](https://github.com/XTLS/Xray-core/tree/v26.9.9/infra/conf) 固定标签源码。源码下载缓存放在构建目录 `build/ui-research/`，不引入产品依赖。下列是表单实际采用的边界，不代替核心配置检查。

| 范围 | 字段/允许值/条件与默认依据 | 固定版本核对 |
|---|---|---|
| 根键 | 15 个模块；env 为 string map，version 为 min/max 字符串 | `xray.go`、`version.go`；env 定义在核心配置源码范围，未设置会保留 |
| DNS | queryStrategy：UseIP/UseIPv4/UseIPv6/UseSystem；服务器策略继承并受全局限制；port 缺省 53，DoH/DoQL 用 URL 端口；缓存过期 TTL 只在相关缓存开启时生效 | `dns.go` 的 NameServerConfig/DNSConfig；clientIp 是地址字符串（滚动文档尾部一处写成数组，与源码/示例不同，UI 按固定源码字符串；已有其他类型保留） |
| Routing | domainStrategy：AsIs/IPIfNonMatch/IPOnDemand；network：tcp/udp/tcp,udp；protocol：http/tls/quic/bittorrent；匹配条件 AND；首个命中规则获胜 | `router.go`；不添加规则 enabled/type；新增目标用已有对象选择器 |
| Balancer | random/roundRobin/leastPing/leastLoad，默认 random；leastPing/leastLoad 需观测覆盖 | `router.go`；strategy.settings 保留原生对象 |
| Log/API | loglevel 五值，缺省 warning；API 支持 Handler/Logger/Stats/Routing/Reflection/ObservatoryService | `log.go`、`api.go`；后者固定源码完整服务分支 |
| Policy | handshake 60、connIdle 300、uplinkOnly/downlinkOnly 1 秒；bufferSize 默认随 CPU 架构，不能强写同一默认；统计开关可显式 false | `policy.go` |
| Metrics | tag/listen，不能同时缺失才由核心报错；UI 不在空模块浏览时制造默认 listener | `metrics.go` |
| Observatory | subjectSelector 为前缀；probeInterval 为 duration；Burst interval 默认 1m、core 最小 10s、sampling 默认 10、timeout 默认 5s、httpMethod 默认 HEAD | `observatory.go`、`router_strategy.go` 的固定标签定义；未设置与显式值分开 |
| Inbound/Outbound | inbound port 允许 number/范围/env:string；outbound 首项有意义；protocol 从当前官方入出站列表选择 | `xray.go`；保存未覆盖类型，原生检查负责协议条件 |
| Transport/Security | method 的七种官方值，security none/tls/reality；REALITY 适用 raw/xhttp/grpc；TLS 相关条件在原生文档中 | `transport_internet.go`、`transport_security.go`；legacy network 别名保留 |
| Sniffing/Mux | destOverride 的四种值；metadataOnly 只激活 fakedns；routeOnly 需 destOverride；Mux 0/负数有原生含义；UDP443 reject/allow/skip | `xray.go`；表单保留数值语义，不把负数变成布尔 |
| Socket | tproxy off/redirect/tproxy；domainStrategy 使用固定原生策略集合；tcpFastOpen 是 bool/number | `transport_sockopt.go`，多类型字段高级编辑 |
| Geodata | cron 五段；assets.url HTTPS、file 是资源目录内已有文件；outbound 是原生 tag | `geodata.go`；下载/权限检查仍由对应实际边界负责 |
| FakeDNS/version | FakeDNS 接受对象或数组；stats `{}` 必须保留；版本 min/max 可空且采用 x.y.z | `fakedns.go`、`version.go`、`xray.go` |

实际保留原字段别名：clientIP/clientIp、expectIPs/expectedIPs、domains/domain、source/sourceIP、network/method、probeURL/probeUrl、tcpcongestion/tcpCongestion、V6Only/v6only、dest/target、publicKey/password。已有别名会在原位置编辑，不生成第二个同义字段。

## 保留与限制

- 固定表单仅修改值确实改变的受控字段，其余对象成员、合法 JSON 类型及数组顺序保留。未修改提交不规范化文本；发生 Visual 修改时整个**目标模块草稿**格式化为 JSON，不能保留注释排版。
- 未识别字段仍在同一模块 JSON 草稿；不丢弃。字段遇到不支持的原生类型或超出 NumberInput 的整数范围显示只读说明，必须在 JSON 模式显式编辑。已有包含空字符串或换行的原生字符串数组也如此；StringList 使用换行文本桥接，不能假装区分 `[]` 与 `[""]`。
- Form Help 的 context 列出实际存在但不由当前常规字段编辑的原生路径，只显示路径、不显示敏感值、不用对象成员总数冒充“高级字段数量”。路径可能属于另一专用原生分区，也可能需要 JSON，帮助明确区分这个范围。
- 已核对的高级边界包括 routing.attrs/webhook 与 strategy.settings、TLS certificates/echSockopt、协议 settings、transport 专属设置、finalmask、sockopt.tcpFastOpen/happyEyeballs/customSockopt，以及已有不兼容类型。固定源码还接受 routing.rules[].localOS，但本轮访问的官方全量文档没有该字段说明，继续留在 JSON，不猜测可选值或增加 schema。
- 无 source 或 disabled 模块浏览不会创建文件或启用。stats 放入空对象也是显式草稿动作，最终保存与 Include 仍独立。
- 字段范围检查只处理已确认的局部输入。表单不是 Xray 全语义验证器；原生 check、监听端口、资源存在、远端连通是不同证据。

## 实际验证

2026-09-27 实际运行 `build/component-rewrite/tests/tui/forms/xrm_tui_forms_tests --reporter compact`，18 个用例、559 个断言通过，见 [原始结果](evidence/review-remediation/native-forms.txt)。覆盖所有 15 模块打开和只读无写入、每种固定对象表单未知字段/默认值往返、DNS 表示/错误留稿/别名、routing 未命名规则/目标分组可达、可选值 absent/0/false/空字符串/空数组、policy 字段移除、7 种集合复制/移动/删除/新增、env/hosts 重命名碰撞和 FakeDNS 单对象保真。

上述测试操作真实 FTXUI Component 事件并检查同一草稿 JSON，不代表核心接受所有配置。真实终端脚本为 `tests/tui/rewrite/module_review.py`；其同轮执行结果与真实 Xray loopback 由 [集中记录](design-review-remediation.md) 承载，旧 [验收迁移记录](tui-acceptance-migration.md) 仅作历史，不替代本轮运行。

本轮真实模块取证实际执行：

```sh
python3 tests/tui/rewrite/module_review.py --build build/component-rewrite \
  --output docs/evidence/review-remediation/modules
```

结果 PASS：[操作与隔离身份记录](evidence/review-remediation/modules/trace.json) 包含 141 次当前屏捕获。每个模块在 80×24、120×40、180×48 均实际进入 Visual、只读对象、可编辑对象；上表模块名链接 80 列编辑入口，同目录 `<尺寸>-<模块>-visual-readonly` / `-object-readonly` / `-object-edit` 为对应 `.txt`、`.ansi`、`.html`。HTML 是真实终端格重渲染，不是原生终端像素截图。

模块 fixture 全部 excluded，没有登记或启动核心；无改动接受子表单后，15 个 Source 的原文字节、included 标记与运行文件保持不变。本取证不证明这些示例配置均可启动/连通，也不替代 DNS/routing 实际修改及真实核心 loopback 验证。第一次尝试在共享 Help 生命周期缺陷处中止，原轨迹保留于 `modules-attempt1`；修复后上面的完整新进程运行通过。

最终小幅迭代仅调整 Form Help 的上下文次序：80 列先显示“Add to draft → 父稿、Module Save → Source、Connect/Apply → runtime”，再显示匹配和高级路径；完整范围从 Help 的 Editing scope 动作可达。共享 Form 同时补空 StringList 提交错误与局部只读标签。15 模块全量屏对应 `eb44f948cf3a…` 构建，不把其身份改写成最终构建；受影响的 6 张 Help 屏已用最终构建重取到 [modules-help-final](evidence/review-remediation/modules-help-final/trace.json)，并重跑 [完整终端链](evidence/review-remediation/terminal-final/trace.json) 与上面的 18/559 表单测试。两轮分别保存二进制 SHA-256、临时目录与清理结果。

# XRM 测试标准与接口先行开发流程

**状态：实施验收基线 · 日期：2026-09-23 · 本文不代表已有测试通过**

依据：[需求T01–T37](requirements.md)、[总体架构](architecture.md)、[实施接口](implementation-plan.md)、[任务DAG](task-dag.md)与[AGENTS.md](../AGENTS.md)。本文规定先测什么、用什么环境、什么才算通过。它不是让LLM自行选择是否遵守的建议清单。

## TS-01　接口先行、测试先行的准确含义

每个任务按以下顺序：

1. **读契约与确认输入输出。** 确认任务的IF接口、R/T要求、文件owner；不要重新定义一套模型。
2. **声明与骨架。** 提交当前任务所需声明，足够使调用方和测试编译。实现未知时可短期明确抛出未实现异常/返回失败；不得返回固定成功。
3. **写行为测试并跑RED。** 测试要因为目标行为尚未实现而失败，不是因为拼错头文件、测试没被发现或缺依赖失败。
4. **具体实现。** 在约定文件中完成行为；不改断言来迁就错误实现。
5. **GREEN与必要重构。** 测试通过后做局部去重复，保持对外契约不变。
6. **集成。** 用真实相邻模块和CLI/IPC运行本阶段用户流程，再交接。

这是**逐任务TDD**，不是先建全产品空壳。私有辅助函数不需要先逐个冻结。测试调用真实公开接口，不额外创建一套业务测试API。实际I/O使用临时DB/目录、测试服务与fake-core；只有I/O边界/时间来源需要替代，纯逻辑直接测试。

### RED/GREEN交付记录

保留实际命令、退出码和关键断言结果即可，不建设证据数据库。测试先出现的commit或清楚的执行记录足够。修复用例的编译错误是准备工作，不算RED证据。未来未实施功能不能被skip后计为当前完成。

## TS-02　测试层次与分工

| 层次 | 测什么 | 使用的真实部分 | 可以替代的部分 |
|---|---|---|---|
| Contract | 共享类型、签名、DTO、帧结构 | 真头文件与编解码 | 无业务假成功 |
| Unit | Codec、匹配、表单、scope、各Writer、FileSet、速率 | 真纯函数 | 固定ID/固定时间、虚构数据 |
| Integration | Store、HTTP、文件、进程、生命周期、CLI/IPC | 真SQLite/文件/UDS/libuv/应用层 | 本机HTTP、测试fake-core、特定失败注入 |
| Real Core | 原生配置实际被Xray解释与执行 | 固定发布版Xray和loopbackpeer | 外部互联网改成本机服务 |
| TUI | 动作语义、焦点、中文、小屏、终端恢复 | 真actions/FTXUI及部分真实Server | 测试输入事件、少量人工验收 |

不能只对mock验证“调用了一次某函数”就声称用户功能通过；也不需要为每个private函数建立mock。测试对象是可观察行为和模块边界。

## TS-03　测试目录、目标与运行命令

```text
tests/
  contract/                  # 编译契约、wire/DTO
  unit/                      # 纯函数
  integration/               # 真DB/文件/IPC/Process和完整应用
  real_core/                 # 真xray的多文件与协议/行为
  tui/                       # UI动作和人工清单
  fixtures/                  # 脱敏输入、独立预期输出
  support/                   # TempDir、loopbackHTTP、fake-core等测试专用工具
```

推荐少量目标：`xrm_contract_tests`、`xrm_unit_tests`、`xrm_integration_tests`、`xrm_real_core_tests`、`xrm_tui_tests`、`xrm_fake_core`。源文件分目录不代表要建几十个测试框架。任务可用Catch2标签选择自己的案例。

项目建立后必须提供：

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure

# 任务自己的用例示例，二进制实际输出路径由preset固定
./build/dev/tests/xrm_unit_tests '[ASM-02]'

cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

这些是将来代码仓库的构建约定；当前交付只有Markdown，并没有可以执行这些命令的C++项目。执行时报告实际生成器、编译器、架构、依赖版本，不照抄本文就声称已运行。

## TS-04　真实核心与SKIP不能混淆

通过显式 `-DXRM_TEST_XRAY=/absolute/path/to/xray` 指定真实核心；测试读取 `xray version` 并记录。固定研究基线为v26.9.9，后续新增版本应独立记录，不能使用“latest”代替版本信息。

```bash
cmake --preset dev \
  -DXRM_TEST_XRAY=/absolute/path/to/xray \
  -DXRM_REQUIRE_REAL_XRAY=ON
cmake --build --preset dev
ctest --preset dev -L real_core --output-on-failure
```

- 普通本地开发没指定核心：real_core测试在CTest层显示**SKIPPED**，不能标PASSED。使用明确返回码77并设`SKIP_RETURN_CODE=77`；不能只打印一行“skip”后exit0。[S-CTEST]
- 开启`XRM_REQUIRE_REAL_XRAY=ON`而核心缺失/不可执行：配置或测试失败，不允许全skip绿灯。
- 明确指定了核心但版本/行为不满足本次承诺：是失败或明确未支持能力，不是“环境不方便所以skip”。
- Catch2内部使用SKIP时要保留机器报告中的skipped数量；单靠整个测试进程exit0不能声称每个用例都通过。[S-CATCH]
- G1/G3/G4/G5需要真实核心证据；fake-core只能完成进程和故障逻辑验收。

普通测试不访问用户订阅、GitHub、公网204、系统DNS配置或上传TUN配置。真实网络行为使用loopbackpeer和临时目录；特权网络功能不在无授权测试中开启。

## TS-05　测试工具的最小范围

### fake-core

仅放在tests/support，编译为独立测试程序，不能成为生产后端。理解测试所需的`version`、`run -test -confdir`、`run -confdir`及已适配api子命令：记录argv/cwd/env，按场景退出、保持运行、输出日志或拒绝TERM。

fake-core **不实现Xray JSON解释器**。它读取测试场景决定失败，不证明任何真实协议合法。场景和控制地址只通过测试环境传入；产品代码不识别`XRM_FAKE_*`变量，也不增加生产`--test-mode`开关。

为竞态用例提供一个本机Unix Socket控制通道：helper报告到达check/start/ready/exit相位，测试收到后发送继续/失败指令。等待以事件和有界deadline实现，不依靠`sleep(1)`碰运气。协议仅服务fixture，不能复用为产品任务系统。

### 文件与SQLite故障

用临时目录、显式冲突路径、第二SQLite连接制造短期锁/事务失败，以及仅限FileOps/Store入口的测试故障点。不通过修改全盘权限、填满宿主磁盘或损坏真实用户数据制造错误。不为故障注入给所有类加接口层。

### 预期数据

expected fixture由接口契约与固定核心文档人工写出；不能调用被测Writer/Catalog/Reconcile生成expected再与自身比较。fixture中的UUID/域名/密码全是虚构的。来源URL和节点秘密不得从聊天附件复制到仓库。

本文的ID是测试族标识，可含多条TestCase，并带[M0]、[M1]等阶段标签。同一测试族在后续阶段扩充已授权案例：例如M1的ASM-03先测单节点范围，M3再测组与观测并集；M1的UI-04先测15项统一入口，M3/M4再测专用表单。未来案例不能以skip占位冒充实现，最终G5须覆盖本目录全部承诺。所有TestCase使用稳定ID标签，与DAG任务卡对应。每次默认运行当前任务和直接依赖的相关用例；阶段门再运行跨模块回归，不要求每改一行都测试整个互联网。

## TS-06　详细测试目录

表中“禁止假通过”也是审查条件；部分架构规则需要源代码/链接依赖审查，不能假装一个黑盒断言足以证明内部没有单JSON中间层。

### 契约与编译

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **CT-01** · contract | **头文件自包含及跨目标依赖**：按IF-00至当期接口分别include；仅链接允许的目标 | 声明可编译；Assembler返回Result<PreparedFiles>；Manager不能链接Store/CoreManager | 通过包含一个万能头隐藏循环依赖；提前空实现所有阶段 |
| **CT-02** · contract | **DTO序列化和错误约定**：四种Target、Node variants、null/missing、枚举字符串、错误响应 | round-trip必要字段和秘密分级正确；未知枚举/必需字段缺失报具体错误 | 把null当默认节点；枚举输出不稳定整数；***回存 |

### 库存与持久化

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **DB-01** · integration | **手工节点真实存储**：临时SQLite，创建两个同名不同ID节点，关闭后重新打开 | 节点数/ID/参数/别名保留，列表筛选分页正确 | 只用内存map替代SQLite；名称当ID |
| **DB-02** · integration | **来源事务原子性与过期结果**：更新多节点时制造SQL失败；下载期间改来源revision/删除来源 | 无半个库存提交；过期结果不覆盖新请求或复活来源 | 把HTTP放事务中；失败吞掉继续提交 |
| **DB-03** · integration | **订阅参数归属与手工复制**：源节点编辑别名/labels/enabled；尝试改连接参数；复制 | 允许元数据；拒绝原地改来源参数；复制新ID且清来源标识 | 两个对象共用可写参数；复制仍被刷新覆盖 |
| **DB-05** · integration | **配置主数据不镜像进库**：创建Profile及模块并外部改文件；重新读取 | SQLite只有位置索引；模块/组从文件读取；运行产物不回写源 | 恢复数据库旧DNS/路由；三个数据层互相同步 |

### 进程与IPC

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **PROC-01** · integration | **argv/cwd/env及父环境隔离**：测试路径带空格；参数含空格、引号、分号；子环境不同于Server | fake-core记录精确argv元素/cwd/env；无shell副作用；Server环境不变 | 用拼接shell命令执行；修改全局setenv |
| **PROC-02** · integration | **非零退出、超时与SIGTERM/KILL**：fake短命命令成功/非零/阻塞；生产实例忽略TERM | 返回真实退出/信号；deadline与grace结束后回收；回调一次 | sleep后假装退出；重复回调；超时当成功 |
| **PROC-03** · integration | **输出与回收**：分块stdout/stderr、长行、大输出、快速退出、未完成工作时shutdown | 流不混淆；截断明示；无悬空uv回调/句柄与无界缓冲 | Server析构后回调；stdout错误串拼成成功JSON |
| **PROC-04** · integration | **只停止自己的进程**：由测试另外启动无关进程；停止被管理核心 | 仅持有句柄对应实例退出；检查进程退出不改变生产状态 | pkill、扫描PID接管、历史PID杀进程 |
| **RPC-01** · integration | **分帧与关联**：一帧多次write、多帧单次write、文本含转义换行、超长帧 | 按id返回完整消息；保留原文；超限错误不截断保存 | 一次read等于一帧；错误字段被当成功 |
| **RPC-02** · integration | **连接/协议失败**：Server不在、协议主版本不支持、EOF、半个JSON后断线 | 明确连接位置/错误；未完成请求结束；不自动启动Server | 静默降级直接读库；重连反复发送写请求 |
| **RPC-03** · integration | **长操作接受与终态**：connect返回accepted后延迟成功/失败/回退；Server重启 | accepted不等于完成；最终错误/实际effect正确；旧operation unknown | op已接受就显示已连接；任务结果永久伪造恢复 |
| **RPC-04** · integration | **断线不重放副作用**：服务已接受写请求但响应前断线，再连接查询 | 操作最多按已接受的那次执行；客户端查询事实不重发 | 重复添加订阅/连接；将未知结果填成成功 |

### 原生模块

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **MOD-01** · unit | **完整catalog**：独立测试表列15个key/filename，不调用被测catalog生成expected | 15项名字、大小写、默认路径、列表入口全部一致 | 只测DNS；实现表自己当预期表 |
| **MOD-02** · integration | **每个模块的原文保存**：对15项分别创建含注释/未知字段/特殊类型的源文，编辑一个 | 只有目标文件字节变化；其他文件逐字节不变；无run文件新写入 | 保存后全Profile重新dump；源设置被写SQLite |
| **MOD-03** · integration | **启停与未实现字段**：逐个停用模块再render；再启用保存内容 | 停用保留源，不输出运行文件；不因绑定需要而默默启用 | 旧运行文件残留；默认给15模块全填空对象 |
| **MOD-04** · integration | **原文/表单差异**：未完成JSON、带注释JSON、未知子字段；高级文本编辑与表单编辑 | 文本编辑原样保存；表单不丢未知值且说明规范化；运行必要解析失败可定位 | regex删注释；URL //被删除；保存失败清空稿件 |
| **MOD-05** · integration | **局部编辑冲突**：打开policy后外部修改同文件再save；只修改另一个文件再save | 同文件返回conflict并保留两侧；无关文件变化不阻塞 | 全局revision触发无关冲突；强行回写旧原文 |
| **MOD-06** · unit | **特殊原生类型**：stats={}；fakedns对象和数组；burstObservatory正确大小写 | 空stats仍输出；FakeDNS类型原样保留；burst单独根项 | 统一删除empty；所有原生值强制对象；observer伪键 |
| **MOD-07** · unit | **env/version生效层级**：配置env字符串映射、version边界；独立Profile进程env | 独立文件；不自动加版本限制；env不污染Server，不代替启动定位参数 | 把XRM版本写version；用配置env寻找尚未加载的confdir |
| **MOD-08** · integration | **文件映射冲突**：两模块同filename、越界路径、outbounds节点分片保留名冲突 | 保存/组装边界给出冲突；不覆盖已有另一模块 | 任意重命名用户模块以掩盖冲突 |

### 模块表单

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **FORM-01** · unit | **原生模块常用操作覆盖**：逐项执行IP-06函数：log、dns、api、metrics、policy、fakedns、geodata、env、version、两种观测 | 只更新约定原生字段；source wrapper保持；API/UI实际可调用 | 以原文编辑器声称全部专用表单完成 |
| **FORM-02** · unit | **子对象正确归属**：分别编辑inbound.sniffing、streamSettings TLS/REALITY/sockopt/finalmask、outbound.mux、Node transport | 参数在所属对象；节点编辑只改库，下一次仅节点分片变化 | 生成顶层tls、sniffing、mux、transport；修改其他节点 |
| **FORM-03** · integration | **统计显式联动与部分失败**：已有policy.levels及未知system字段；计划改stats/policy/metrics；第二文件写失败 | 计划列明文件；保留其他policy字段；已保存/失败清单真实；不自动连接 | 把几模块合为telemetry；中途失败报告全部成功 |
| **FORM-04** · integration | **切换观测类型**：保存两种源设置，从observatory切到burst再关闭 | 只profile included变化，源参数保留；下一份只输出选择的一项 | 两种根键都残留；把pingConfig写进observatory |
| **FORM-05** · integration | **Geodata与实际资源分离**：编辑assets/cron；数据目录含dat；纯模块保存和render | JSON中不含dat二进制；不触发下载或第二个定时器 | 保存即联网；覆盖/usr/share包管理器资源 |
| **FORM-06** · unit | **规则编辑与原生保留**：规则含多个match条件和未知字段；UI选择Node/Group作为target；移动/替换一条 | 保存原生outboundTag/balancerTag；原条件不丢；顺序按操作变化 | 保存为match+target私有格式；全局规则自动排序 |
| **FORM-07** · unit | **入出站列表与完整对象**：增删复制移动原生数组项，包含高级settings、tls和mux | 只目标父模块变化；复制显式新tag；未知子项保留；索引冲突可解释 | 替换同tag半个outbound期待核心deep-merge |

### Codec与来源

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **CODEC-01** · unit | **VLESS常用映射**：批准fixture分别覆盖RAW/TLS/REALITY/Vision/XHTTP及extra | 逐字段对独立expected outbound比较；传输安全归属正确 | 只断言protocol=vless或outbounds长度1 |
| **CODEC-02** · unit | **URI编码边界**：IPv6 authority、百分号、+、Unicode fragment、缺padding、CRLF、无效端口、重复参数 | 逐字段解码一次；错误可定位；重复参数按约定处理并有notice | 整条先decode；凭据进异常/日志；递归无限Base64 |
| **CODEC-03** · unit | **VMess分享格式**：批准的新URI/旧Base64 JSON正反例 | 输出固定核心接受的参数，unsupported有原因 | 把旧alterId等不支持语义静默保留或删除后冒称支持 |
| **CODEC-04** · unit | **Trojan参数**：password含保留字符、TLS/transport、IPv6等 | 认证和传输映射正确；未知影响连接项报告 | 默认禁用TLS验证来通测 |
| **CODEC-05** · unit | **SS/SIP002输入**：合法userinfo编码、方法密码、插件需求与错误案例 | 密码无损、方法保留；不支持插件明确失败 | 删plugin参数后声称节点可用 |
| **CODEC-06** · unit | **Hysteria2到Xray**：hy2/hysteria2的批准常用参数、超范围参数 | 以Xray原生协议/传输结构写出；字段有规范fixture | 启动额外Hysteria核心；其他客户端支持就假设Xray支持 |
| **CODEC-07** · unit | **原生outbound入口**：单outbound/集合、未知嵌套字段、自定义tag、有跨节点引用 | 保存RawOutbound未知内容；输出按管理节点tag；复杂引用边界明确 | 把整个来源DNS/routing当节点导入；丢字段重建有限模型 |
| **CODEC-08** · unit | **不支持与废弃字段**：已移除/影响语义未知URI参数及旧顶层reverse/global transport样例 | 不作为当前模块生成；报告明确；源原文不被擅自迁移 | -test容忍未知字段就宣称已支持 |
| **HTTP-01** · integration | **HTTP正常与TLS/代理路径**：本机HTTP与测试TLS服务；直连/显式代理；代理不可用 | 证书默认验证；自定义CA仅测试环境显式配置；代理失败不直连 | 为了测试设置CURLOPT_SSL_VERIFYPEER=0 |
| **HTTP-02** · integration | **资源边界**：慢响应、超正文上限、取消、并发4请求、shutdown | 超时/取消可辨；最多3实际下载；loop仍可答status；完成回调一次 | 无界线程/内存；后台回调UAF |
| **HTTP-03** · integration | **HTTP状态与缓存**：404/500/重定向/200错误HTML/304有无本地成功缓存 | 下载状态、正文识别分开；不把304无缓存当有效首次导入 | TCP成功就提交空节点 |
| **SUB-01** · unit | **订阅容器识别**：明文URI、标准/URLsafe Base64、BOM/CRLF、HTML、深层编码 | 最多一次包装解码；合法nodes与issues完整报告 | 不断解码直到猜中；将错误页面当节点 |
| **SUB-02** · integration | **刷新只影响库存**：先运行一个实例并记录run文件/PID；更新含新节点的来源 | Node库新增，run文件字节/PID/已应用选择不变；无core check调用 | source update写90_nodes_tail或触发重启 |
| **SUB-03** · integration | **部分损坏与零有效节点**：混合valid/malformed/unsupported；后续全空/HTML | 部分成功不按未出现删除旧节点；零有效保留旧库存；结果有分类 | 损坏列表清空旧节点；只返回成功数量 |
| **SUB-04** · unit | **稳定匹配与歧义**：来源重排、改名、同名不同端点、认证变化、两来源相同名字 | 保留确定匹配ID/用户元数据；歧义新增；不跨来源合并 | 取第一个同名节点；凭据hash作为公开ID |
| **SUB-05** · integration | **进行中删除和改来源**：HTTP被测试屏障暂停；删除/编辑来源；释放HTTP | 旧结果不重建来源或覆盖新settings；报告冲突/取消 | 旧回调无条件写入Store |
| **SUB-06** · integration | **定时刷新与停止意图**：核心明确停止，timer到期；重复手动刷新同来源 | 只一份来源请求；更新库但不拉起核心，不产生运行目录 | 默认自动apply；定时器覆盖用户stop |

### 组与多文件组装

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **GROUP-01** · unit | **筛选真值表**：显式节点和动态来源/协议/名称/标签组合，disabled/missing | 条件间AND、同条件OR；查询排除不可用；结果稳定 | 猜地区、模糊跨来源、运行时测速选路 |
| **GROUP-02** · unit | **手选组singleton输出**：已保存绑定的固定fixture：候选A/B，selected=B | selector只有B；可加载候选但不随机分担；重复组装结果一致 | 依赖启动后bo恢复B；默默选第一个 |
| **BIND-01** · integration | **组和选择独立持久化**：通过ProfileService保存/删除/选择组，关闭并重新读取；原生routing源同时存在 | groups.json保存成员与选择；routing源字节不变；选择重开后仍在；不连接核心 | 在库中又保存groups镜像；必须先调用bo才算保存 |
| **GROUP-03** · unit | **原生与动态balancer共存**：静态native组、动态组；再制造同tag冲突 | 保留原组/未知参数，新组追加一次；冲突报错 | 覆盖原组/改名掩盖冲突 |
| **GROUP-04** · unit | **空组与缺失选择**：未使用空组、使用空组、ExplicitMembers缺项、selected不属于成员 | 未使用组不阻塞；实际使用错误清楚；不回退直连 | 全部组必过度校验；自动修成DIRECT |
| **GROUP-05** · unit | **默认选择不是全局强制**：多条原规则含先前catch-all；append_selected_default切换 | 只在启用时追加默认规则，原规则順序/目标不变 | 将所有流量改选定出口；自动挪catch-all |
| **ASM-01** · unit | **全部原生模块的FileSet**：15项独立参数化启用；观测使用两个互斥案例 | 每项正确文件/key/value，binding文件不输出；不是单config.json | 依赖同一被测catalog计算expected；误同时启用两种观测 |
| **ASM-02** · unit | **局部变化局部输出**：基准输入分别只改log/dns/policy/version/geodata等一项 | 文件集合中仅所属内容变化；源文件全部不被assemble写入 | 全局序列化重格式化无关模块 |
| **ASM-03** · unit | **实际节点范围**：默认目标、规则精确管理tag、使用组、显式观测与additional节点交叠 | 并集每ID一次；不导出无引用库存；缺失引用报错 | 订阅全部节点无条件导出；扫描任意依赖图 |
| **ASM-04** · unit | **一个完整routing**：原生rules/options/balancers与动态组和默认目标 | 仅04_routing.json一个完整routing；原未知字段保留 | 规则和组分别半个routing文件；运行元数据混入 |
| **ASM-05** · unit | **观测范围补充**：原生selector已有静态值和重复管理tag；选择不同观测模块 | 用户原序在前+必要tag去重；probe/pingConfig不变；只输出当前根项 | C++重新做测速；覆盖整份观测参数 |
| **ASM-06** · unit | **子对象与原生类型**：FakeDNS数组、stats空对象、节点mux与stream子对象 | 保留值类型和归属；无顶层tls/groups/sniffing/observer | 统一empty清理、误顶层拆分 |
| **ASM-07** · unit | **禁用模块前提**：停用outbounds但选节点；停用routing却要绑定；禁用无引用模块 | 必要前提报具体错误；无关停用直接不输出 | 偷偷启用模块或为安全改成block |
| **ASM-08** · unit | **纯度与输出契约**：内存BuildInput，确定ID；两次assemble | 确定性FileSet、无网络/DB/Process链接，无源文件写入 | 先生成complete_config再拆回文件；结构审查必须识别 |

### 文件与生命周期

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **WRITE-01** · integration | **新目录无旧文件**：第一次含burst，第二次改observatory并删其他模块 | 第二目录只含新FileSet，没有旧JSON残留 | 复用旧confs但忘记删除文件 |
| **WRITE-02** · integration | **candidate隔离**：建立active后准备candidate，修改candidate测试文件 | active字节与inode内容不变；无可写硬链接共享 | 测试A启动B；隐藏共用可写inode |
| **WRITE-03** · integration | **物化失败不伤旧输入**：中途文件写失败/目标冲突 | 候选标记失败并清理，active不变，错误真实 | 部分目录被标可启动；覆盖原目录 |
| **WRITE-04** · integration | **多文件导出**：FileSet含注释和原生扩展，另有外部asset路径 | 导出文件名/内容保留，依赖另说明，不用dump；默认不覆盖非空目录 | 暗中打包外部私钥；导出大JSON |
| **LIFE-01** · integration | **成功连接与同输入检查启动**：选择A，捕获check/run日志与RunRecord | 每次连接一次check；binary/cwd/env/confdir一致；成功后记录active | 检查源目录后另生成运行；spawn即宣布联网成功 |
| **LIFE-02** · integration | **check失败旧进程不动**：运行A，B的fake check非零 | A仍原PID；source中B选择可保留但actual仍A | 先停A再检查；把选中B当已连接B |
| **LIFE-03** · integration | **start失败一次恢复**：B check成功但run立刻失败，A可恢复 | 只尝试A一次；operation失败且effect=restored_old；源不变 | 无限恢复；恢复成功把B报告成功 |
| **LIFE-04** · integration | **stop抢占所有阶段**：用屏障分别停在prepare/check/stop-old/start-new/restore/retry，再发stop | 停止意图false；之后无任何启动；请求终态明确；当前句柄回收 | 延迟回调/定时器再次拉起；只测试running阶段 |
| **LIFE-05** · integration | **已保存/未保存/自动恢复输入**：保存DNS B并选择B，但当前A；另有TUI未保存C；显式restart/自动崩溃 | 显式重启用已保存B；C不混入；自动恢复用最近成功原目录 | 所有重启都用数据库旧稿；恢复顺带发布新编辑 |
| **LIFE-06** · integration | **无API与远端离线**：没有API/Metrics；普通进程存活；公网不可达 | process_only可运行；核心健康与网络健康分开 | 为ready check注入API或依赖gstatic成功 |
| **LIFE-07** · integration | **持久化失败与停止失败**：候选启动后写RunRecord失败；停止意图写失败 | 前者按有限旧输入恢复并报告；后者仍停进程并提示意图未保存 | 吞错误回成功；建立隐式第二数据库 |
| **LIFE-08** · integration | **核心切换只换二进制**：active A；源已保存未应用B；切新core | 以A已运行目录测试/启动新core，不引入B；系统路径不改；部分保存失败报告准确 | core use顺带应用全部设置 |
| **LIFE-09** · integration | **并行连接与旧回调**：一次connect未结束又发connect；旧进程晚到退出事件 | 第二请求busy，不排队；旧回调不把新PID标failed | 两个生产core、无限连接队列 |
| **LIFE-10** · integration | **Server重启和进程归属**：正常关闭/正式systemd清理；保存stop后重启；另有独立xray | 自己子进程回收；停止意图保留；不接管无关PID | 前台SIGKILL场景假宣称完全清理；pkill |
| **LIFE-11** · integration | **旧输入也无法恢复**：候选启动失败，旧文件/外部证书/程序不可用 | 最终failed和实际原因；不覆盖用户修复中的源文件 | 声称回滚整个系统/资产；无穷重试 |

### 统计与安装

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **MET-01** · unit | **速率和未知值**：两次计数正常/下降、run_id变化、0或负elapsed、缺延迟/缺计数 | 按单调时间差算；重置或未知=null，不产生负流速/假0ms | 不同时间/不同run混算；用户额度当本地流量 |
| **MET-02** · integration | **多人共享采样和旧响应**：两个watcher，关闭其一/全部；旧run请求晚到 | 同一tick只一次采样，全部离开停采；旧响应丢弃 | 每UI启动轮询器；旧数据覆盖新核心 |
| **CTRL-01** · integration | **临时API argv和数据边界**：对已运行组override/clear，记录source/profile文件 | 官方argv正确；仅当前core受影响；源/永久选择不变 | 把临时覆盖当持久选择；自动补节点/API |
| **CTRL-02** · integration | **API超时结果不明**：bo超时；bi可确认或也失败 | 只查询一次；confirmed或unknown；无无限重试写命令 | 超时等于未生效或成功 |
| **LOG-01** · integration | **有界分块日志**：长行分块、stdout/stderr错开、超过4MiB、历史游标 | 来源清楚、截断标记、cursor可恢复、内存有界 | 整行无限缓存；stdout/stderr误合并 |
| **LOG-02** · integration | **慢客户端不阻塞核心**：一个日志连接不读，另一个继续status/stop | 丢弃旧日志/断开慢端明示；其他操作持续可用 | 为保全日志阻塞loop或无限队列 |
| **INS-01** · integration | **系统核心与已安装核心区分**：注册现有binary、安装不use、删除仍引用版本/系统版本 | 登记不改文件；安装不启用；引用和system受保护 | 更新覆盖/usr/bin/xray；安装后自动重启 |
| **INS-02** · integration | **发布/下载/归档正确性**：本机官方形状release JSON、好坏摘要、归档../与符号链接、缺架构资产 | 选择显式版本/架构；仅期望文件；坏包失败清理 | latest永久等于最新；任意归档路径展开 |
| **INS-03** · integration | **安装失败不改变运行**：下载中断、解包失败、binary version失败 | 旧core/active/Profile不变，错误说明阶段 | 回退到Go构建或改其他运行核心 |

### 终端与端到端

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **CLI-01** · integration | **非交互契约**：成功、失败、notfound、busy、partial、unknown、accepted命令 | 退出码与结果一致；JSON stdout无进度/颜色；错误stderr | accepted当最终成功；需要模拟TUI才能脚本化 |
| **CLI-02** · integration | **选用并连接语义**：select不connect，connect --node，停止时apply，disconnect | 保存不运行；合并动作明确；停止apply不拉起；disconnect=stop | 一个命令同名多语义；未保存草稿混入 |
| **CLI-03** · integration | **模块与文件来源展示**：列全模块、show source/runtime、render/dump/export | 明确源/运行/绑定；导出是多文件；show原文不改成***再保存 | dump当可无损回写配置；源与运行混称 |
| **UI-01** · tui | **键盘动作与退出**：输入框键入q/e/空格；Enter选中；明确连接；q退出 | 文本不触发动作；Enter详情；退出只断客户端，core仍运行 | 查看即切节点；退出顺便stop |
| **UI-02** · tui | **中文、小屏与列表稳定**：80×24、中英文emoji节点、滚动中刷新、来源更新 | UTF-8不按字节切断；焦点/ID/滚动保留；无鼠标可操作 | 刷新跳首行、按字节截断、窄屏按钮不可达 |
| **UI-03** · tui | **期望与实际状态**：选B失败恢复A；有未应用设置；缺API/metric样本 | 分别显示saved B/actual A，提示未知/待应用，不捏造状态 | 把当前高亮项当实际出口 |
| **UI-04** · tui | **全部模块与真实编辑能力**：打开15模块，操作常用表单及高级原文；传输子编辑器 | 标原生键/位置/源运行文件；功能真实可保存；未支持表单如实标记 | DNS之外杂项占位；一个原文编辑器冒充全表单 |
| **UI-05** · tui | **外部编辑器与终端恢复**：编辑器成功/非零/冲突/取消，随后TUI继续 | 草稿保留可修复；终端恢复；保存不自动连接 | 编辑器退出就重启；异常后终端坏掉 |
| **UI-06** · tui | **zero-profile startup**：空 data root 启动 Server 后直接 `xrm tui`，不传 `--profile` | 成功进入；Subscriptions/Nodes/Cores/Profiles 可用；Groups/Modules 显示创建 Profile 的 empty state；不要求 UUID | 退出、崩溃或要求先用 CLI 创建 Profile |
| **UI-07** · tui | **Profile management in TUI**：全程不输入 UUID，create Home、rename Router、clone Test、open/switch、delete Test | 通过 Profile 一级页面按名称/选择器完成；删除/切换不隐式 connect | UUID 成为正常操作门槛；浏览造成运行切换 |
| **UI-08** · tui | **viewed vs running Profile**：Home 正在运行，切换 viewing 到 Office，再明确 connect Office | 浏览阶段 PID、runtime dir、Actual Target 不变；显式 connect 后运行 Profile 才切换 | 把 viewed/highlighted target 当作 running/actual |
| **UI-09** · integration | **CLI exact Profile ref resolution**：`--profile home` 唯一名称、重名、合法 UUID | 唯一名称及 UUID 精确成功；重名 conflict/ambiguous；零匹配 not_found；不 fuzzy/不取首项 | fuzzy match 或任意选择同名 Profile |
| **E2E-01** · integration | **M0两进程闭环**：真实xrmd+xrm、临时DB、测试核心version、手工节点 | 登记/保存/列表/重启持久化均经UDS | 绕过Server直接改库取得假成功 |
| **E2E-02** · integration | **M1模块到连接**：一个手工节点、15模块管理、select、connect、logs、stop | 保存->FileSet->core argv->状态闭环；无大JSON | 只测库/生成器不运行xrm命令 |
| **E2E-03** · integration | **M2来源到实际选用**：本机订阅更新后查看节点，选择并连接，后续再次更新 | 更新不运行，connect才采用新库存；部分结果可解释 | 依赖机场在线才能验收 |
| **E2E-04** · integration | **M3分组、原生规则和状态**：动态组+静态规则+观测/统计，修改选择并应用 | 仅相关文件变；用户规则保留；统计来自core；回退目标真实 | 关闭原生规则方便实现；合为一个文件 |
| **E2E-05** · integration | **M4导出与高级模块**：原生高级模块+节点库选择，导出生成confdir | 同样核心/cwd/env下可独立运行；外部资源另说明 | 依赖Server隐含patch才能运行 |
| **E2E-06** · integration | **多客户端与完整首版流程**：CLI与TUI同时连Server，保存、更新、连接、停止、重连 | 单写与实际状态一致；无写重放；完整工作流可重复 | 客户端另存业务模型、多个核心争抢 |
| **E2E-07** · real_core | **cold-start human workflow**：空 data root；从 `xrm tui` 添加本机订阅、更新、创建 Profile、选择 VLESS 节点、connect、真实 loopback 流量、打开 disabled module 并保存、退出 | 不预建 Profile、不传 Profile UUID、不直接改 DB/CLI 代做前置步骤；固定真实 Xray 流量成功；退出 TUI 后 core 仍运行 | fake-core、手工预置业务状态、仅 `-test` 或 JSON 断言 |

### 真实核心

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **RC-01** · real_core | **原生多文件加载与tail**：固定发布版；临时loopback入站；独立原生文件包含同tag覆盖和tail案例 | 真实核心加载顺序/端口/出口行为符合所选版本；不是C++merge | fake-core或仅比较JSON代替真实加载 |
| **RC-02** · real_core | **VLESS客户端/服务端本机连通**：同一固定xray程序分别作测试peer和管理实例；loopback服务返回标记 | 通过管理器导入->输出->启动的客户端收到标记；有实际数据经过协议 | 只-test；真实公网机场；把未联网当网络成功 |
| **RC-03** · real_core | **原生路由+静态/动态组选择**：可区分响应的两个本机出口，固定规则与手选/自动组 | 请求实际到相应出口；singleton选择重启仍正确；原路由未丢 | 只检查selector字符串而不发请求 |
| **RC-04** · real_core | **流量统计与Metrics**：明确启用stats/policy/metrics，发送已知本机流量 | 读取真实非空计数；未开启对应项时显示不可用；比较增量非绝对相等 | 用自增测试计数假装真实统计 |
| **RC-05** · real_core | **观测实现与leastLoad边界**：本机受控探测URL；两种观测分别测试；leastPing/leastLoad依支持承诺测试 | 真实结果符合接口；未验证组合不标supported；不同时遗留两种模块 | 公网204可达作为唯一证据；一律宣称最优算法支持 |
| **RC-06** · real_core | **临时API覆盖**：真实RoutingService查询/override/clear并重启 | 按固定核心官方JSON/argv；临时效果不写源，重启回保存选择 | 解析人类表格；永久选择靠补发API |
| **RC-07** · real_core | **原生模块类型与上下文**：对15项分别建立合法上下文fixture；stats={}、FakeDNS多池、policy级别等 | 相应配置被真正识别；有行为的关键模块再以行为断言验证 | 所有模块强行塞一个fixture；-test忽略字段也当功能成功 |
| **RC-08** · real_core | **声称支持的协议映射**：VMess/Trojan/SS/HY2最小本机peer配置，及新传输字段的目标版样例 | 至少config conformance；声称连通支持的常用协议还要loopback流量 | 仅Raw JSON能保存就称typed/URI支持；一协议覆盖其他协议 |
| **RC-09** · real_core | **env/version与进程上下文**：不同启动前env、原生env、min/max范围、资源路径；隔离子进程 | 读取时机/版本限制按核心验证；Server环境未改变；失败可定位 | 改变Server全局env以通过；把两种版本混同 |
| **RC-10** · real_core | **多文件导出独立运行**：导出当前FileSet，停止管理实例，再用相同程序/env/cwd运行导出目录 | 无Server时同样可访问本机测试服务，文件名内容保留 | dump重写；端口冲突时偷偷kill其他进程 |

### 打包与性能

| ID / 层次 | 场景与输入 | 必须断言 | 禁止假通过 |
|---|---|---|---|
| **PKG-01** · integration | **systemd正式部署**：检查unit的ExecStart/KillMode/明确目录；具备隔离systemd环境时启动停止 | 正式模式清理自己的cgroup；不开另一个xray.service；命令可部署 | 未经授权操作宿主服务；静态unit检查冒充实机部署 |
| **PERF-01** · integration | **异步操作下的可响应性**：下载与检查被屏障延迟，另一客户端持续status/stop | loop可处理请求，UI立即给反馈；无等待网络导致整个界面卡住 | 用总下载速度代替交互响应；用长sleep规避竞态 |
| **PERF-02** · integration | **千级节点与有界资源**：1000虚构节点、少量Profile、重复render/刷新、慢消费者 | 记录机器/版本/p50/p95/峰值RSS；无持续线性泄漏与无限日志；无查看者无频繁轮询 | 未测量就加无锁容器/内存池；把环境差异藏进结果 |

## TS-07　需求T01–T37完整追踪

该表引用冻结需求的原编号，不另定义一套产品验收编号。每行至少有测试/审查证据；不是把本表打勾就代表用例已经运行。

| 需求验收 | 本文测试 | 验收时补充 |
|---|---|---|
| T01 | E2E-01、E2E-06、E2E-07、UI-01、UI-06 | 含实际TUI操作 |
| T02 | SUB-02、SUB-06 | 接口行为与副作用同时检查 |
| T03 | SUB-03、HTTP-03 | 接口行为与副作用同时检查 |
| T04 | SUB-04、GROUP-04 | 接口行为与副作用同时检查 |
| T05 | MOD-02、ASM-02 | 接口行为与副作用同时检查 |
| T06 | GROUP-01、GROUP-02、BIND-01、E2E-04 | 接口行为与副作用同时检查 |
| T07 | ASM-03、GROUP-05、RC-03 | 真实核心必需 |
| T08 | ASM-01、ASM-08、RC-01 | 真实核心必需 |
| T09 | MOD-01、ASM-01、MOD-08 | 接口行为与副作用同时检查 |
| T10 | ASM-04、FORM-03、GROUP-03 | 接口行为与副作用同时检查 |
| T11 | ASM-02、MOD-02 | 接口行为与副作用同时检查 |
| T12 | MOD-03、WRITE-01 | 接口行为与副作用同时检查 |
| T13 | RC-01、RC-02、LIFE-01 | 真实核心必需 |
| T14 | LIFE-05、CLI-02 | 接口行为与副作用同时检查 |
| T15 | LIFE-02、LIFE-03、LIFE-11 | 接口行为与副作用同时检查 |
| T16 | LIFE-04、SUB-06 | 接口行为与副作用同时检查 |
| T17 | GROUP-02、BIND-01、LIFE-05、RC-03 | 真实核心必需 |
| T18 | CTRL-01、RC-06 | 真实核心必需 |
| T19 | WRITE-04、RC-10、E2E-05 | 真实核心必需 |
| T20 | MOD-04、FORM-02、FORM-06 | 接口行为与副作用同时检查 |
| T21 | LIFE-06、UI-03 | 含实际TUI操作 |
| T22 | MET-01、MET-02、LOG-02 | 接口行为与副作用同时检查 |
| T23 | WRITE-02、LIFE-03、LIFE-11 | 接口行为与副作用同时检查 |
| T24 | INS-01、LIFE-08 | 接口行为与副作用同时检查 |
| T25 | UI-01、UI-02、UI-03、UI-05、UI-06、UI-07、UI-08 | 含实际TUI操作 |
| T26 | HTTP-02、PERF-01、RPC-04 | 接口行为与副作用同时检查 |
| T27 | RC-01、RC-08、CT-01 | 真实核心必需；核对SKIP/证据真实性，不只依赖断言 |
| T28 | E2E-06、E2E-07、CLI-01、UI-09 | 接口行为与副作用同时检查；核对SKIP/证据真实性，不只依赖断言 |
| T29 | MOD-01、MOD-02、MOD-03、ASM-01、UI-04 | 含实际TUI操作 |
| T30 | MOD-07、RC-09 | 真实核心必需 |
| T31 | MOD-06、RC-07 | 真实核心必需 |
| T32 | FORM-02、ASM-06 | 接口行为与副作用同时检查 |
| T33 | GROUP-03、FORM-06、ASM-04 | 接口行为与副作用同时检查 |
| T34 | FORM-04、ASM-05、WRITE-01 | 接口行为与副作用同时检查 |
| T35 | FORM-03、RC-04 | 真实核心必需 |
| T36 | FORM-05 | 接口行为与副作用同时检查 |
| T37 | CODEC-08、ASM-06 | 接口行为与副作用同时检查 |

## TS-08　15个原生模块的参数化验收矩阵

expected清单直接写在测试fixture中，与产品catalog独立维护。每一项至少运行：list/get；文本保存；表单或明确高级入口；included开关；预览；文件路径/根键；未修改字段保留。不要拿一次包含全部模块的大fixture代替这些测试，尤其两种观测本就互斥。

| 原生key | 运行文件 | 特别需要证明 |
|---|---|---|
| log | 00_log.json | 只改日志自身；尊重用户文件日志目的地 |
| dns | 01_dns.json | 服务器/hosts未知字段保留；不等同DNS outbound |
| inbounds | 02_inbounds.json | sniffing/streamSettings归对应入站；顺序/tag保留 |
| outbounds | 03_outbounds.json及90_nodes_tail.json | 静态和节点同属原生数组；静态首出站保留；mux归出站 |
| routing | 04_routing.json | rules与balancers在一个对象；原规则先于追加默认规则 |
| observatory | 05_observatory.json | 只增必要subjectSelector，原探测参数保留 |
| api | 06_api.json | 服务列表保留；没有时仍可运行；不是管理器UDS |
| metrics | 07_metrics.json | 仅配置不等于统计已开启；不自动插入所有API |
| stats | 08_stats.json | 空对象不能删掉；省略与空对象区分 |
| policy | 09_policy.json | levels/system完整；统计编辑不清除其他策略 |
| fakedns | 10_fakedns.json | 对象和数组；不与dns混成私有模型 |
| geodata | 11_geodata.json | 计划与dat分离；无第二个定时下载器 |
| env | 12_env.json | 字符串值；核心配置后生效与启动前环境分开 |
| version | 13_version.json | 用户min/max；不注入XRM版本或自动限制 |
| burstObservatory | 14_burstObservatory.json | 大小写正确；pingConfig保留；切换无残留 |

另外拒绝把旧reverse/global transport以及页面名groups/tls/sniffing作为新根项输出。新合法核心根项的支持需要明确描述和对应测试，不能用维护白名单为由丢用户原生内容。

## TS-09　几个关键测试写法示例

以下是实施时要编写的测试形式，依赖tests/support中的fixture辅助函数，不是已经可编译执行的测试文件。辅助函数只构造固定数据，不调用被测算法生成预期结果。

### 9.1 改任何一个独立模块，而非只改DNS

```cpp
TEST_CASE("single module change has local output", "[unit][ASM-02]") {
    for (const auto& example : fixtures::independent_module_edits()) {
        INFO(example.key);
        auto before_input = fixtures::saved_profile_with_one_node();
        auto before = assemble(before_input);
        REQUIRE(before.has_value());

        auto after_input = before_input;
        fixtures::replace_module_text(after_input, example.key, example.new_text);
        auto after = assemble(after_input);
        REQUIRE(after.has_value());

        // expected_changed_filename来自独立fixture，不调用catalog猜预期。
        CHECK(fixtures::changed_file_names(before->files, after->files)
              == std::vector<std::string>{example.expected_changed_filename});
    }
}
```

需要覆盖log/policy/geodata/version等，不只一个dns。磁盘源文件保护由MOD-02的真实文件测试证明，这个纯函数案例不伪造磁盘状态。表单联动另有允许变化文件集合，不能用这条断言阻止明确的跨模块功能。

### 9.2 空Stats值不能被通用empty判断删掉

```cpp
TEST_CASE("empty stats object is emitted", "[unit][MOD-06]") {
    ModuleDocument doc{"stats", "08_stats.json", "{\"stats\":{}}\n"};
    auto file = emit_native_module(doc);
    REQUIRE(file.has_value());
    CHECK(file->relative_name == "08_stats.json");
    CHECK(file->bytes == doc.source_text);
}
```

测试要验证原字节与空对象存在，而不是只检查JSON非空。

### 9.3 订阅更新不应接触运行配置

```text
Arrange:
  用真实Server连接节点A；记录PID与运行目录中全部文件字节。
  本机HTTP来源下一轮返回节点B。
Act:
  经xrm sub update调用刷新，等operation终态。
Assert:
  库中可查到B；旧运行PID和文件字节完全未变；
  fake-core或进程事件记录中没有新的check/run。
```

这里不能用MockRuntimeWriter验证“没调用write”代替真实目录和子进程检查。

### 9.4 停止抢占要有屏障，不靠运气

```text
for phase in prepare, checking, stopping_old, starting_new, restoring_old, retry_wait:
  在测试fixture屏障停在指定phase。
  从第二个真实Manager发送core stop。
  释放屏障，让全部已排队回调运行。
  断言没有后续spawn、desired_running=false、受控子进程已回收。
```

要同时确认旧迟到exit回调不会把另一个新实例标failed。产品代码不得为测试添加“跳过停止逻辑”开关。

## TS-10　测试接口、Mock与断言的上限

**应使用：** 真纯函数、真实SQLite临时库、真实临时文件、真实UDS、真实libuv Process+fake-core、真实Xray本机peer；少量可控制时间/ID/I/O失败的测试接点。

**不应使用：** 每个类一套mock接口、测试专用业务RPC、用全部Mock拼出伪E2E、期望输出直接由产品生成器写出、每条assert都按当前代码行为临时决定。

无须100%行覆盖作为产品门槛。必须覆盖公开契约、核心正向路径和任务直接相关失败分支；重要行为缺失不能用高覆盖率抵消。为观察private数据加friend应有具体原因，优先用公开结果/真实副作用。

静态架构审查核对：Manager不链接Server逻辑；Assembler不含I/O依赖；订阅服务不持有RuntimeWriter/CoreManager；模块原生字段不镜像数据库；没有complete_config中间对象；没有为15模块机械创建多层服务。

## TS-11　可用性与性能验收方式

对TUI至少实际操作一次：80×24窗口、中文和emoji节点名、全键盘导航、编辑器退出恢复、已保存/未保存提示、更新列表不丢焦点、连接失败显示实际旧目标、退出不停止core。

人工失败后的产品修复需保留可复现回归：

- TUI 对全部15个模块逐项实际按 Enter；覆盖 disabled+无源、disabled+有源、enabled+有源、无效正文；确认进程存活、正文状态可见、未因打开/保存草稿自动启用或改运行文件。
- 80×24 PTY 使用 `xrm tui` 无 `--profile`，在 UI 添加本机订阅 fixture 并更新；观察节点、通过 Profile 页创建/选择方案、选节点并 connect。不得在测试前创建 Profile 或向命令传 ID。
- VLESS 使用固定脱敏 4 TCP/REALITY（含 `insecure=true/false`）、3 WS/TLS、1 XHTTP/TLS 形态：检查 neutral defaults、ECH 的 `+ / =` 一次解码、Node JSON roundtrip、多文件出站。完整原 URI 不可得时明确说明合成端点与凭据。
- 固定 v26.9.9 对 RAW、WS、XHTTP 各自使用 loopback peer 和唯一 marker 做真实流量验收；不得用 `-test` 或 JSON 断言替代流量。
- 普通 PTY terminal mode 验证 Ctrl-S/F2 保存、外部编辑器返回草稿、失败恢复与 conflict；测试 harness 不得为了单独按键而关闭终端流控。

性能使用固定虚构负载：1000节点、少量Profile、单核心，记录机器/构建类型与p50/p95、峰值RSS和日志上限。优先判断是否阻塞loop、是否持续泄漏、无查看者是否仍高频轮询。不要把不同机器的绝对毫秒数当同一硬门槛；测得瓶颈后只修具体路径。不能以性能为由删错误检查或引入未需求的无锁框架。

正式systemd清理由隔离环境验证；没有这样的环境只记录静态unit检查已做、实测未做。未经用户授权不得启停宿主服务。这个环境性缺口必须在发布验收清楚注明。

## TS-12　阶段门与结果表达

| 门 | 必需重点 | 不足以通过的证据 |
|---|---|---|
| G0 | 两进程CLI、真实Store与Process/IPC | 只有头文件和unit成功 |
| G1 | 全15模块通用入口、单节点多文件连接、真实Xray loopback | 只有fake-core启动 |
| G2 | 本机订阅入库/刷新不改运行配置、失败保留、可连接新选择 | 只解析一行URI |
| G3 | 动态组/原生路由/观测/统计与故障stop矩阵 | 只看JSON里有balancer字段 |
| G4 | 完整首版协议/高级模块入口与实际支持范围 | 所有复杂功能都标Raw就算支持 |
| G5 | T01–T37证据、实际TUI、打包、完整真实核心回归 | task DAG全打勾、编译绿、测试全SKIP |

报告区分PASSED、FAILED、SKIPPED、NOT IMPLEMENTED、NOT RUN。SKIP不等于失败，但也不满足依赖该证据的门。RED是开发中有意的行为失败，不是交付时可以保留的最终状态。

建议结果表字段：测试ID、任务ID、实际命令、环境/版本、退出码、状态、关键断言/证据位置、限制。不要伪造完整日志，也不需要存储逐次测试数据库。

## TS-13　测试发现规范矛盾时

依据顺序是用户确认需求 → 冻结架构 → 实施契约 → 测试具体样例。外部核心接口出现实测不一致时，记录固定tag、最小配置/命令、实际结果和影响范围。只能修正有依据的契约/测试问题，不能把实际功能失败改成“预期失败”后宣布支持。

依赖版本不同导致的API变更采用最小适配；不得连带升级所有库或改变Manager/Server边界。任务外错误单独报告，不默认扩大修复范围。

## TS-14　依据与完成声明的边界

- 产品验收来自随包 requirements.md 的T01–T37和architecture.md；本文件给出实施测试细化，不另起产品需求。
- [S-CTEST] [CMake SKIP_RETURN_CODE](https://cmake.org/cmake/help/latest/prop_test/SKIP_RETURN_CODE.html)：跳过返回码在CTest层显式标记。
- [S-CATCH] [Catch2 runtime skip](https://catch2-temp.readthedocs.io/en/latest/skipping-passing-failing.html)：被跳过的用例需保留其状态，不能用exit0掩盖。
- [S-CORE] [固定基线run.go](https://raw.githubusercontent.com/XTLS/Xray-core/v26.9.9/main/run.go)：-test与真正启动分开。
- 原生模块、文件合并、路由和字段依据见 implementation-plan.md 的来源表与原架构资料表。对每个未来新字段在相应任务中补充固定源码/文档fixture依据。

当前交付只制定测试标准与任务，**没有运行本节中的C++/Xray测试，也没有产生PASSED证据**。实现之后必须实际执行，不能把本文的预期结果当作测试报告。

# XRM 架构文档

**状态：当前首版实现与人工验收状态见 `release-acceptance.md`；产品修复批次进行中 · 整理日期：2026-09-24**
**需求：[requirements.md](requirements.md) · 执行约束：[AGENTS.md](../AGENTS.md)**

> 本方案采用：**独立节点库 + Xray 原生模块文件 + 少量独立管理绑定 + 按原生边界输出多文件 + 独立核心进程**。Manager/Server 不变。不是只为 DNS 做特例，不以自定义模块模型替代 Xray 配置，也不退回只有文件树和启动按钮的工具。
>
> 本文为明确实现边界而选定一套方案；类名、源设置结构、路径和命令均为产品设计，不是 Xray 自带接口。核心事实以 `[Xnn]` 标记。代码片段仅说明接口和数据形状，没有声称已经编译或运行。

## 1. 总体结构与职责

```text
用户 / 脚本 / Agent
       |
       v
xrm Manager
  CLI + TUI + RpcClient
       |
       | 本机 UDS / JSON Lines
       v
xrmd Server
  Application
    ├── SubscriptionService ──> NodeStore
    ├── ProfileService ──────> 原生模块文件、分组绑定、当前选择
    ├── RuntimeAssembler ───> FileSet：各功能自己的 Xray 文件
    └── CoreManager ────────> 独立 xray-core 子进程
            ├── ProcessAdapter
            ├── XrayControl（可选 API）
            └── MetricsClient
```

| 部分 | 负责 | 不负责 |
|---|---|---|
| Manager | 命令、表单、筛选、临时编辑稿、状态显示 | 不直接访问生产数据库、不启动核心、不生成另一套配置 |
| Server | 节点、来源、模块保存、选择、运行准备、子进程和日志 | 不承载实际代理流量 |
| Xray-core | 读取原生配置、实际代理、DNS、路由、观测与核心统计 | 不被当作普通 URI 订阅解析器 |

两个产品可执行文件是 `xrm` 和 `xrmd`；核心来自用户指定或管理器安装的独立 Xray 程序。CLI 与 TUI 共用同一应用接口，不各自实现节点选择和配置生成。

## 2. 三类数据分别保存，不做双向同步

### 2.1 选定的目录布局

```text
data_root/
├── manager.db                    # 来源、节点、Profile位置索引、核心、运行指针
├── profiles/<profile-id>/
│   ├── profile.json              # 当前选择、核心、启动上下文、模块文件名/启用状态
│   ├── modules/                  # Xray原生顶层块；不放管理器自定义根键
│   │   ├── 00_log.json
│   │   ├── 01_dns.json
│   │   ├── 02_inbounds.json
│   │   ├── 03_outbounds.json      # 静态原生出站，不限于direct/block
│   │   ├── 04_routing.json        # 完整原生routing；保留用户原生balancers
│   │   ├── 05_observatory.json
│   │   ├── 06_api.json
│   │   ├── 07_metrics.json
│   │   ├── 08_stats.json
│   │   ├── 09_policy.json
│   │   ├── 10_fakedns.json
│   │   ├── 11_geodata.json
│   │   ├── 12_env.json
│   │   ├── 13_version.json
│   │   └── 14_burstObservatory.json
│   └── bindings/                 # 管理器辅助输入；绝不直接给core扫描
│       ├── groups.json           # 动态成员/手选 + 原生balancer模板
│       └── observation.json      # 是否把本次组/节点补入观测范围
├── previews/<profile-id>/confs/  # 显式render/check/dump的临时多文件
├── runs/<run-id>/confs/           # 只含本轮启用的原生模块文件
│   ├── 00_log.json
│   ├── 01_dns.json
│   ├── ...                       # 与modules同名；对象型模块只输出一次
│   ├── 04_routing.json           # 原生routing + 已展开动态组 + 可选默认规则
│   ├── ...
│   └── 90_nodes_tail.json        # 节点库中本轮需要的outbounds
├── cores/<release>/xray
├── assets/                       # .dat/证书等实际文件，和模块JSON不同
├── cache/subscriptions/
└── logs/
```

完整文件映射以需求 R06 为准。编号是默认排布，不代表初始化/模块依赖顺序；核心读取与构建由 Xray 决定。未启用模块不出现在运行目录，源模块可以保留以便以后启用。

通常一个原生顶层项有一个文件；outbounds 是明确例外，静态条目与节点库条目按核心数组语义分为两个分片，并由一个出站输出负责人协调顺序、tag 和范围。[X01]

`runs/<run-id>/confs` 在状态页和 config command 中显示。run-id 只是目录名，不是内容寻址或构建锁。用户模式使用 XDG，系统模式可指定 /var/lib/xrm 与 /run/xrm；路径均属 Server 主机。

### 2.2 权威来源

| 内容 | 唯一编辑来源 |
|---|---|
| 节点与订阅请求设置 | SQLite |
| Profile 名称、核心选择、启用的来源、当前默认节点/组、运行环境 | `profile.json` |
| XRM 主题、编辑器和已有程序偏好 | SQLite 程序设置；不写入 Xray 模块 |
| 所有原生顶层块和子对象 | 各 `modules/` 源文件 |
| 动态组定义、成员筛选、手选值及附加观测范围 | `bindings/`；仅此处保存相应管理绑定 |
| 当前实际运行的配置 | 已成功运行的文件集合及 RunRecord，只作结果记录 |
| 实时流量、延迟、临时 API override | 核心反馈和 Server 内存，不作为永久配置 |

SQLite 的 `profiles` 表只登记 ID 与文件位置，不再复制整个 Profile 的设置 JSON。列表读取文件并可缓存；缓存不是另一个可写主数据。

TUI 使用七个顶层页：Overview、Profiles、Modules、Subscriptions、Nodes、Logs、Config。Nodes 二级页包含订阅来源、Local 和 Groups；Overview 复用现有 MetricsClient 显示统计；Config 二级页为 Core、Appearance、Editor、General。页面合并不合并 Server 职责：GroupBinding 仍在 Profile 绑定文件，核心安装记录仍在数据库，Xray 原生模块仍只在 `modules/`。来源全局存储，Profile 只保存来源 ID 绑定。旧 Profile 缺失来源绑定字段视为沿用旧行为（全部来源可见），新建 Profile 使用显式空集合直到用户选择来源。

模块源文件与运行文件不是需要持续同步的两份真相：前者是输入，后者是连接时的输出。运行目录不监视后反向写回用户设置。用户需要编辑运行产物时先导出副本，独立使用。

### 2.3 原生层、绑定层、运行层

**原生层：** 每个 ModuleDocument 保存该顶层项的完整原生 wrapper。例如 routing 文件始终是 `{"routing": {...}}`，policy 始终是 `{"policy": {...}}`。原生规则直接使用 outboundTag/balancerTag，不能改成持久化的 `{"match":...,"target":...}`。

**绑定层：** 核心本身不认识的 source_id、NodeSelector、当前节点选择、动态成员、自动观测范围，放在 Profile 或 bindings。动态组模板里的原生部分仍使用真实的 tag/strategy/fallbackTag 等；成员筛选不伪装成原生 selector。

**运行层：** 每个 Writer 保留原生块的未修改字段，只在自己负责的已知位置加入本轮绑定结果。普通模块直接复制源字节；routing 加入动态 balancers 和可选默认规则；outbounds 输出必要节点；观测可附加本轮 tags。绑定文件本身不进入 confs。

原生层可有用户自己写的静态 balancer，与动态绑定组以不同 tag 共存；不能同一个 tag 在两处同时可写。传输设置等属于入/出站，不建立第二套全局原生模块。

节点库参数不复制到 DNS、路由或组；只在显式连接/预览时输出到节点分片。源模块的 tag 可能引用待生成节点，因此不能把源目录和已展开运行目录混称为一套无需准备就可运行的配置。

### 2.4 “原生模块”的准确定义

当前配置总览列出 env、log、api、dns、routing、policy、inbounds、outbounds、stats、fakedns、metrics、observatory、burstObservatory、geodata、version 共15项。[X10]

这些是配置单位，不意味着15个C++类、15个daemon服务或15套RPC。用一个普通模块描述列表提供名称/默认文件映射，共用模块读写接口；只对需要实际组装的模块写专门函数。

子对象按其路径归属：routing.rules/balancers，policy.levels/system，inbounds[].sniffing，inbounds/outbounds[].streamSettings，outbounds[].mux 等。修改子对象调用所属模块或节点操作，不发出伪顶层 tls、transport、sniffing。[X11][X12][X13]

本次维护的是当前原生清单，不是永远冻结的白名单。新文档根项可通过简单增加描述/明确原生文件入口保留并输出，未适配的交互标明高级编辑；不以解析成功或 -test 成功充当新特性已经实际生效的证据。

## 3. 核心数据模型

普通结构体和值类型即可，不为每个对象同时建立 Manager、Repository、Factory 和抽象接口。

### 3.1 节点与来源

```cpp
struct Node {
    std::string id;
    std::optional<std::string> source_id;
    std::string original_name;
    std::optional<std::string> alias;
    std::vector<std::string> labels;
    bool enabled = true;
    bool source_missing = false;
    NodeSettings settings;
};

using NodeSettings = std::variant<StreamNode, WireGuardNode, RawOutbound>;
```

StreamNode 组合 Endpoint、协议、Transport、Security；不定义 VlessRealityXhttpNode 这种组合类。常用字段强类型，未设置字段用 optional；XHTTP extra、复杂 socket 参数等保留原生 JSON。

WireGuard 等不适合单端点 stream 模型的类型独立表达，或先通过 RawOutbound 接入。RawOutbound 是明确的原生节点表示，不与 typed 参数同时竞争写同一字段。

来源保存 ID、名称、URL、请求头、周期、下载方式、最近结果和可选来源报告额度。节点详情与普通列表分别返回需要的数据，不以 `***` 作为真实密码的占位存储规则。

### 3.2 Profile、原生模块与管理绑定

```cpp
struct ModuleDocument {
    std::string key;                  // 官方根键，保留大小写
    std::filesystem::path filename;
    std::string source_text;          // 原生顶层包装对象原文；未修改时可直接输出
};

struct ModuleSetting {
    std::string key;
    std::filesystem::path filename;
    bool included = false;            // XRM 设置，不写成模块内部 enabled
};

struct Target {
    TargetKind kind;                  // node / group / native_outbound / native_balancer
    std::string id_or_tag;
};

struct ProfileSettings {
    std::string name;
    std::string core_id;
    std::optional<Target> selected;
    bool append_selected_default = false; // local 预设显式开启
    RunEnvironment environment;
    std::vector<ModuleSetting> modules;
    std::vector<std::string> additional_node_ids;
};

struct GroupBinding {
    std::string id;
    std::string name;
    NodeSelector members;
    SelectionMode selection_mode;    // automatic / manual：管理器语义
    std::optional<std::string> selected_node_id;
    Json balancer;                    // 原生 tag、strategy、fallbackTag 及未知扩展
};
```

`GroupBinding.balancer` 不保存第二份 selector：动态 selector 来自 members 或手选值；原生 balancer 的 tag 在创建组时给定稳定值。自动策略直接保存在原生 `strategy.type`，不同时在 C++ 枚举与 JSON 中持久化两份策略名称。手选是管理操作，不给 Xray 发 `strategy.type="select"`。

GroupBinding 只保存到 bindings/groups.json。它拥有这个生成 balancer 的原生参数和动态成员；相同 tag 不允许又放入 modules/04_routing.json 的静态 balancers。模板不包含节点密码，不复制节点配置。

路由操作的 UI DTO 可以暂时使用 Target，保存时即写成原生 rule 的 outboundTag/balancerTag；DTO 不是另一种磁盘路由语言。已有原生 rules 和 balancers 直接保留，Writer 不把它们拆成自定义 Rule 数组再丢掉其他字段。

Profile.selected 代表默认节点/组；手选组内部选择代表该组的具体节点，概念不同但都可见。选择原生固定路由方案时可以关闭 append_selected_default 并不设 selected；手写服务端配置也可以使用模块系统运行。

### 3.3 原生值的表示

ModuleDocument 的正文是含相应原生顶层键的对象，原生值允许对象、数组等对应类型。文本编辑不强制 Json.dump；表单按需解析为同一个 DOM 并只改目标字段。不要为所有核心字段手写 struct，不给已保存原文再配一份持久化 typed 镜像。

具体语义不得弄反：缺省模块表示不输出；stats 的 `{}` 应保留；fakedns 可以是对象或数组；env 的值是字符串；version 的值是核心版本边界对象。[X07][X14][X16][X10]

启用/停用和文件名映射在 ModuleSetting，源模块正文不带 XRM 的 enabled/source_id/schema_version 等属性。嵌套对象的管理开关若无原生对应项，也只能保留在管理设置或以明确删除/恢复对象表达，不能编造核心字段。

### 3.4 观测绑定

bindings/observation.json 只保存“是否自动补充本次需要观测的组成员/节点”和额外明确节点选择，不重复保存 probeInterval、probeUrl、pingConfig 等原生参数。

用户手写的 subjectSelector 仍是静态范围；启用自动补充时，将它与本轮生成 tags 合并去重，保留用户顺序在前。不用“自动值优先覆盖整个模块”的规则。关闭补充后原生范围完整保留。

普通产品界面让 observatory/burstObservatory 二选一；源文件分别保存，运行仅输出启用的一份。切换实现的设置变化可显示两个模块，不将两种原生结构映射成一个不透明 observation.json。

## 4. 订阅链：只负责库存

```text
HTTP 获取
   -> 识别 URI/Base64/原生节点 JSON
   -> Codec 解析
   -> NodeStore 更新
   -> 返回新增/更新/缺失/不支持/损坏统计
```

SubscriptionService 不持有 RuntimeWriter，不调用 ProfileService 改 DNS、组或运行文件。刷新操作的文件副作用最多是来源响应缓存，不是 Xray 配置。

每个来源一次短数据库事务，网络请求不包在事务里。暂停定时更新不禁用已有节点；节点是否参与组由 enabled、source_missing 和组条件决定。

### 4.1 解析与字段维护

先判断已知文本格式，再最多一次 Base64 解码识别；处理 BOM、CRLF、标准/URL-safe Base64 和缺省 padding。不递归试解码，不把 HTML 登录页当节点。

结构化 URI 先拆 scheme、authority、query、fragment 再逐部分解码。IPv6、认证信息、百分号、加号和重复参数在 Codec 测试覆盖。每种协议的输入解析与 Xray 输出放在同一模块，共享传输/安全字段函数。

普通程序错误与不支持参数分开报告。不能为“兼容”默默删除会改变连接或证书验证语义的参数。

### 4.2 节点 ID 与刷新

首次入库分配随机稳定 ID。来源内部优先使用可靠唯一来源 ID；没有时以明确协议/端点/认证身份组合匹配，再考虑两边均唯一的原名与协议。歧义时新增，不用模糊算法猜。

显示名变化、来源排序变化不应直接改变 ID。认证信息只参与必要的内存比较，不写日志、不作为公开 hash ID。

完整成功刷新可将不再出现的节点标为 source_missing；仍被引用的记录保留以说明问题。部分损坏不据此删除旧节点。当前运行文件在下一次应用前不受库存清理影响。

手工节点的增删改只修改库。编辑订阅参数通过“复制为手工节点”完成，避免维护多层来源覆盖规则。

## 5. 模块配置管理与编辑

### 5.1 每个原生模块具有相同的管理契约

ProfileService 提供 module.list/get/save/enable/disable，标识使用原生 key，不以界面中文标题或文件名猜模块。描述信息至少有原生 key、名称、源路径、运行路径、启用状态和编辑支持范围。

统一契约覆盖 R06 全部15项，不只实现 DNS 然后把其他功能留在杂项 JSON。只有需要专用交互时增加表单函数，通用管理无需每项一个 Service 类。

保存原生模块只改对应 modules 文件；保存动态组只改 bindings/groups.json；改变当前选择只改 profile.json。保存不调用核心、不导出节点、不重启。

原生 routing 文件保持真实路由选项以及 `rules`、`balancers`；原生 policy 保持 `levels`、`system`。UI 中“选节点作为规则目标”是编辑原生 tag 的便利操作，不把源文件换成管理器自定义 target schema。

新 Profile 的预设是一份普通模块和少量绑定设置的一次性创建；不远程继承、不持续覆盖。原生块默认来自明确预设/用户选择，不为所有模块自动填一个空对象。不能把“模块在列表中可用”误实现为“默认全部启用”。

### 5.2 常用表单与高级原文共存

入/出站对象管理、协议/传输子对象、路由规则与组、用户等级策略、DNS/FakeDNS、观测、env键值、version边界、geodata资源计划、API服务及统计设置，应有相应可用的 CLI/TUI 入口。常用操作逐步提供表单，其余保留完整模块级原生编辑，不能宣称一份通用原文编辑器等于所有表单已经完成。

高级编辑针对同一模块文件，允许原生未知字段。文本读取/保存不做整份 JSON 重排。常用表单解析模块对象并只更新其负责的字段，保留未知值；若保存会移除该文件注释或规范化排版，首次明确告知并允许继续用文本编辑。

不要求首版实现无损 JSONC AST/CST，不用正则改复杂 JSON，不为了保留注释写一套编辑器。无法可靠展示表单的内容保留原文，显示具体限制。

### 5.3 文件冲突和外部编辑

用户可以直接编辑 Profile 的模块源文件。Server 不用数据库“修复”它们。读取时保存本文件基线，写回时只比对当前文件；冲突保留稿件，不三方自动合并。

设置文件采用同目录临时文件 + rename 写回。Manager 通过 Server 保存，Server 元数据仍是单写；外部源编辑是支持的例外，不意味着运行产物要反向同步。

不会在每次刷新页面执行全配置比较或核心检查。明确打开/重载/连接时读取最新保存内容；文件监听若后来加入，也只通知变化，不自动应用。

## 6. 多文件 RuntimeAssembler

### 6.1 接口形状

```cpp
struct BuildInput {
    ProfileSettings profile;
    std::vector<ModuleDocument> modules;
    ManagerBindings bindings;  // 普通组/默认选择/观测绑定，不是另一套核心配置结构
    std::vector<Node> nodes;
};

struct OutputFile {
    std::filesystem::path relative_name;
    std::string bytes;
};

struct PreparedFiles {
    std::vector<OutputFile> files;
    RuntimeIndex index;  // 已加载节点/组映射，用于状态；不放进 confs
};

Result<PreparedFiles> assemble(const BuildInput& input);
```

Assembler 无网络、数据库、UI 或进程操作。它返回 **FileSet，不返回 Json complete_config**。不得先生成整份配置再“按顶层键切回文件”作为长期实现，这会重新引入一个全量配置权威对象。

实际实现是一组普通函数：emit_native_module、write_inbounds、write_outbounds、write_routing、write_observation 等，由一个薄函数协调。函数不必机械变成类，更不需要注册型插件框架。

### 6.2 节点范围和 tag

准备输入时先确定默认选择、原生规则中的已知管理tag、被使用的组及显式观测节点范围。节点集合取这些引用的并集；同一节点写一次。手选组可加载该组候选，选择仍由 singleton selector 决定，不意味着所有候选都在分担流量。

不扫描任意 raw JSON 自动推导复杂依赖图。RawOutbound 有额外链式目标时由用户显式加入所需节点/基础出站；不支持的自动推导要说明。

生成节点 tag 使用 `xrm.node.<完整ID>`，组使用 `xrm.group.<完整ID>`，固定格式，不与同范围生成项互为前缀。显示名独立。Xray 的 selector 是前缀匹配，固定完整 tag 可用于明确成员集合。[X04]

### 6.3 完整原生模块映射与唯一输出责任

完整的原生键→文件表见需求 R06。实现保留一个简单静态描述表/普通数组来支持名称和文件默认值；不建动态注册、插件、schema下载或多核心能力系统。多个模块可以共用相同原文输出函数。

| 输出函数/职责 | 输入 | 本轮允许写的文件/位置 |
|---|---|---|
| 原生块输出 | log、dns、api、metrics、stats、policy、fakedns、geodata、env、version 等原生正文 | 对应的单一模块文件；没有绑定时可直接复制字节 |
| InboundsWriter | 原生入站列表及本次明确选择的辅助对象 | `02_inbounds.json`；保留 settings/streamSettings/sniffing 的嵌套 |
| OutboundsWriter | 静态原生出站 + 本轮节点集合 | `03_outbounds.json` 与 `90_nodes_tail.json`；同一逻辑模块协调两分片 |
| RoutingWriter | 完整原生 routing + 动态组绑定 + 默认选择 | 仅 `04_routing.json`；不再写第二个 routing |
| ObservationWriter | 两种原生观测正文中本轮启用项 + 补充范围绑定 | `05_observatory.json` 或 `14_burstObservatory.json`，根键不互换 |

这些是普通函数责任，不要求为每行建立 Writer 类。Policy 没有绑定时就是原生块复制；统计操作已在源设置中显式改 policy，不在组装期从 metrics 开关偷偷再重写 policy。

运行输出不含 `profile.json`、bindings、数据库 ID 对象或 NodeSelector。未知原生字段保留，未启用模块不输出。`stats` 空对象与缺省模块不能被统一的“删除空值优化”弄混。

输出前按 ModuleSetting 的 included 决定模块是否参与；`outbounds` 的设置共同控制静态和节点两份分片。不能因为存在组绑定就偷偷生成用户已经停用的 routing。绑定或当前选择需要已停用模块时返回一条明确提示。这只处理管理器自身的生成前提，不分析核心所有隐含默认行为。

### 6.4 RoutingWriter：原生对象 + 有限绑定

1. 以源模块中完整的 routing 对象为基础，保留所有原生路由选项、未知字段、原有 `rules` 和 `balancers`。
2. 将用户在规则编辑器选出的目标保存为稳定的原生 tag；组装不重建一个自定义 Rule schema。
3. 对本次使用的 GroupBinding，保留原生 balancer 模板字段，只填动态 selector，追加到 balancers。静态与生成 tag 冲突直接报告，不覆盖、改名或吞掉原组。
4. `append_selected_default=true` 时，在原有规则之后生成一条当前选择的默认规则；false 时不增加规则，也不强迫原生 routing 使用默认选择。
5. 输出一次完整 routing。用户先前的 catch-all 仍先匹配；不自动移动它，预览能看到追加规则的位置。

普通手选组为单元素 selector，其原生策略使用确定的单候选实现（例如创建手选模式时保存 random）。不向核心发虚构的 manual/select 类型，不把 fallbackTag 当成有序故障转移。

自动组由原生 strategy 决定。NodeSelector 只用于把来源/标签等管理条件展开为节点 tag，不自己实现运行时健康探测或选路。[X04]

原生固定 balancer 直接保留。需要使用节点库而原生 selector 范围无法被简单明确引用识别时，用户可设置 additional_node_ids；不扫描任意JSON来推导无限链式图。引用缺失的管理节点必须提示，但不对未知原生机制做臆测修复。

### 6.5 对象模块不任意拆分；数组模块显式分片

目标版本对 routing、policy 等整体覆盖，不能假设跨文件深合并。因此规则页和组页共同归 RoutingWriter，policy的system和levels共同归Policy模块；API辅助路由也通过这一个routing输出位置产生。[X01][X02]

outbounds 数组有核心定义的 tag/前后插语义。静态内容保持在03_outbounds，生成节点在90_nodes_tail；前者可以包含DNS、loopback等任意原生静态出站，而非仅direct/block。两份文件不代表两个“出站配置系统”。[X01][X12]

节点库对象只有在运行准备时由Codec写成outbound；节点的TLS、REALITY、XHTTP、sockopt、finalmask、mux跟随该节点输出，不能被放进全局传输文件。用户入站中的这些子对象则原样留在入站文件。[X11][X12][X13]

同名配置页面可能涵盖多个原生位置；UI映射不是核心schema。一份叫transport.json的文件本身并非技术禁忌，但它不能包含已移除的顶层transport或只写半个同tag outbound期待深合并。默认布局不制造这种误解。[X02][X18]

### 6.6 原生模块关联：只做明确的小型协作

**观测与组。** 原生参数从对应模块正文读；补充目标来自绑定。当前选择的观测模块只在subjectSelector追加实际所需tags，其他字段不变。普通产品二选一，切换时旧根项不输出。leastLoad等组合以核心实测为依据。[X05]

**Stats、Policy、Metrics与API。** “启用流量统计”是显式的应用层便捷动作，列出需要改动的原生模块，分别写入其源文件。Policy的原有levels和其他system字段不丢失。用户也可逐模块编辑。组装时不再计算一份隐藏统计配置，开启Metrics不等于计数已开启。[X07]

**FakeDNS。** 地址池是fakedns；DNS选择逻辑是dns；恢复目标的sniffing属于相关inbound；DNS outbound属于outbounds；分流属于routing。便捷接入只改列明的位置，仍分别输出；不以一键接入为由修改宿主机防火墙或DNS。[X14][X19]

**Geodata。** geodata文件保存计划和资源条目；.dat在单独资源目录。首次准备是明确操作，运行期启用核心更新时XRM不做同一文件的第二个更新器。回退运行文件不保证回退外部.dat内容。[X15]

**Env。** 原生env写入12_env.json，由核心读完配置后应用；RunSpec.process_environment是启动前的独立进程环境。XRM不执行原生env里的字符串、不写入Server环境、不尝试用env块选择它自己尚未读取的confdir。[X16]

**Version。** version写入13_version.json，min/max来自用户，默认不自动产生版本限制。CoreInstall的实际版本和管理器数据格式版本另外记录，禁止用“校验一致”覆盖原生version。[X10]

跨模块操作用几次明确字段修改即可，列明结果，失败时不宣称全部已保存。不建设一个任意JSON补丁平台、依赖DAG或自动配置修复器。

### 6.7 只改相关内容，不建设增量编译框架

小规模设置允许每次调用全部 Writer 得到本轮文件集合。对于不变模块，可保留原文字节或复用上一输出；Writer 不统一重排所有文件。

例如只改log级别、policy某项、geodata计划或version范围，各自只改所属源文件和运行文件内容；同样的原则适用于全部模块而非DNS特例。显式联动只允许列出的相关模块改变。候选复制其他文件相同字节不算改用户配置。

不要为了省几次 JSON 序列化引入依赖 DAG、文件哈希树或缓存一致性系统。用“文件名 + 字节内容”的普通比较产生变化列表足够。

## 7. 运行目录与启动准备

### 7.1 RuntimeWriter 与 Assembler 分开

RuntimeWriter 将 FileSet 写到新候选目录；Assembler 只处理数据。候选目录与正在运行目录物理分离，不能边写多个文件边让新核心读取半份配置。

**不要用可写硬链接把候选和 active 的文件相连。** 无关模块可普通复制；不需要数据库—文件—进程的分布式事务。

每份候选目录仅包含本次文件清单，禁用或改名模块不会遗留旧 JSON。临时文件、备份、状态记录一律放在 confs 之外，因为核心会按目录读取配置。[X03]

### 7.2 三种准备场景

| 操作 | 使用的位置 | 是否改当前运行 |
|---|---|---|
| render/check/dump | previews 下独立目录 | 不改 |
| connect/apply/start/restart | 新候选 runs 目录 | 检查通过后才切换 |
| 自动恢复/rollback | 最近成功或上一份 runs 目录 | 直接恢复该份已保存输入，不重建节点 |

最多保留当前、上一份和正在准备的候选；预览也只保留有限一份。不添加配置版本仓库。

导出保存多文件名和内容，绝不通过 `-dump` 代替原生文件导出。导出目录可在相同核心和必要资源下独立运行；证书、规则数据、cwd/env 等外部依赖另行说明，不暗中打包宿主机秘密。

## 8. 连接和生命周期

### 8.1 一条主要运行路径

```text
收集本次已保存输入
        ↓
RuntimeAssembler -> FileSet
        ↓
RuntimeWriter -> candidate/confs
        ↓
xray run -test -confdir candidate/confs
        ↓
停止旧核心（存在时）
        ↓
xray run -confdir candidate/confs
        ↓
确认进程状态 / 可选管理探测
        ↓
保存本次 RunRecord
```

核心启动后仍使用这个已经检查的目录，不切回源设置目录，不再次从今天的节点库临时生成。`check` 和 `run` 的 binary/cwd/env/asset_dir 一致。[X03]

本轮开始收集的输入构成一个普通不可变副本。之后的编辑属于下一轮，状态提示待应用即可；不因另一页保存了日志级别而建设全局 revision 重试体系。

### 8.2 选择是用户设置，不是运行时猜测

`select` 只保存 Profile 选择；`group select` 只保存某个手选组。`connect --node/--group` 是“保存该选择 + 执行连接”的便捷操作。

连接使用当前已保存模块，因此也会应用其他已保存编辑；UI 一次列出待应用模块。未保存的 TUI 编辑稿不会自动混入。

普通切换在首版使用重新生成相关内容后重启的路径。这保证永久选择与下次启动一致，不依赖 API 成功后又补写数据库和快照的复杂链条。API 临时覆盖另有明确高级动作。

### 8.3 RunRecord 与状态

```cpp
struct RunRecord {
    std::string profile_id;
    RunSpec spec;                    // binary、运行 confdir、cwd、env
    std::optional<Target> applied_selection;
    RuntimeIndex index;              // 已加载节点/组；只供展示和临时控制
    std::chrono::system_clock::time_point started_at;
};
```

RunRecord 是结果记录，不反向覆盖 Profile。状态使用 stopped/starting/running/stopping/failed，加 desired_running 和一个当前操作，不创建状态机框架。

显示“已保存选择”“当前运行选择”“待应用模块”；临时 override 存在时额外标记，不以缓存值冒充核心当前状态。

### 8.4 错误处理与回退

检查失败：旧进程和 active 指针不动。实际启动失败：清理候选并尝试恢复 old 一次。恢复成功仍报告“连接失败，已恢复原实例”，不能显示新目标已连接。

`-test` 在正常 server.Start 前结束，因此不能证明端口绑定、权限或远端连通。[X03] 没有 API 的配置可以按进程存活观察窗报告 process_only；有 API 时检查实际配置的服务，不能为探测方便要求用户启用全部服务。

失败不删除用户刚保存的设置；它们保留以便修复，运行记录仍如实指向旧输入。无法保存运行记录时报告错误，按同一条有限恢复路径处理；不追加二阶段提交系统。

### 8.5 stop 与自动恢复

stop 更新 desired_running=false 并停止自己的子进程。停止意图保存失败仍尽力停进程并报告限制。主动停止、主动替换旧实例不触发崩溃重试。

正常停止先 SIGTERM，默认 5 秒后再 SIGKILL。意外退出最多按 1、3、10 秒尝试三次；用户 stop 后所有后续启动回调都应失效。自动恢复使用最近成功运行目录，不应用源模块新编辑。

核心版本选择与安装独立。对正在运行的 Profile 执行版本切换时，拿已运行文件集合测试新核心，不顺带加入库存/设置的新变更；成功才更新选择。外部包管理器替换系统二进制后，管理器不保证还能恢复旧版本。

### 8.6 部署与进程归属

Server 用 libuv 持有实际子进程句柄，生产、check 和 api 子进程区分。回调携带本次句柄/运行身份，避免旧进程退出回调误伤新实例。

正式后台部署由 systemd 托管，使用 `KillMode=control-group` 清理子进程。前台开发模式不宣称父进程被强杀后一定完成清理；当前范围不额外加入第三个 watchdog 或提权代理。[X09]

## 9. 核心程序、API 与 Metrics

ProcessAdapter 是构造核心命令的唯一位置：

```text
<binary> version
<binary> run -test -confdir <runtime-directory>
<binary> run -dump -confdir <runtime-directory>
<binary> run -confdir <runtime-directory>
<binary> api <已经适配的子命令和参数>
```

argv 直接执行，不拼 shell。清理会意外选择另一配置位置的继承变量，保留用户明确的资源环境。核心配置的 env 不用于取代启动前必需的文件定位环境。[X03]

低频控制采用官方 `xray api` 命令，避免首版引入 C++ gRPC/Protobuf 构建链。具体参数集中核对目标核心 help；不解析人类表格作为稳定机器协议。[X06]

`group override --runtime` 只影响当前进程，不写 Profile，不在下次启动补发。目标必须已加载；API 超时只查询一次实际结果，不能确认则显示未知。普通“选用并连接”不通过这个入口偷换为临时选择。

Metrics 请求本地 `/debug/vars`，从核心取得统计/观测；缺数据说明原因，不伪造。有人查看时默认每 2 秒采样，多客户端共享结果，无查看者停止轮询；速率按单调时间和计数差计算，核心重启/计数下降时清基线。[X07]

日志保持有界，区分 Server 和 Core。用户日志模块写文件时尊重该设置，不为接入日志页擅自改配置；可明确显示“核心日志位于某文件”。

## 10. Server API 与 CLI/TUI

### 10.1 IPC

AF_UNIX stream + UTF-8 JSON Lines。一个请求带版本、ID、method 和 params；响应带结构化 result 或 error。处理半包、多包和换行转义，不假设一次 read 就是一条消息。

```json
{"v":1,"id":17,"method":"module.get","params":{"profile_id":"home-id","module":"policy"}}
```

```json
{"v":1,"id":18,"method":"connection.connect","params":{"profile_id":"home-id","target":{"kind":"group","id":"auto-id"}}}
```

只需要同包协议主版本，不做多代协商系统。大列表分页，单帧大小有普通上限；超限返回错误，不能截断正文后当完整模块保存。

### 10.2 接口责任

| 接口族 | 作用 |
|---|---|
| subscription.* | 请求设置、刷新、来源状态；无运行文件副作用 |
| node.* | 独立节点管理 |
| profile.* | 创建、读取、复制、元设置和选择 |
| module.* | 全部原生模块的list/get/save/enable/disable、原文、原生键和文件定位；子对象操作仍归其父模块 |
| group.* | 候选、模式、手选设置；与 routing 管理分开 |
| config.* | 多文件预览、检查、应用、导出、回退 |
| connection.select/connect/disconnect | 明确的用户连接动作 |
| core.* | 二进制和进程管理 |
| runtime.status/logs/stats | 实际状态与观测 |
| operation.get/events.subscribe | 有界长任务结果和刷新通知 |

长任务返回 operation_id，CLI 默认等最终结果。客户端断线不取消已接受操作、不重发写请求。操作记录只保留最近少量于内存；Server 重启后未知结果明确说明，不构建任务回放日志。

### 10.3 TUI 页面与源文件联动

CLI 和 TUI 是两个兄弟前端，直接通过 RpcClient 消费同一 Server API/DTO；共享业务接口不意味着共享交互模型。CLI 保持显式 command/argument 流程，TUI 保持长驻页面和本地会话上下文。TUI 不得构造 CLI argv 或调用 CLI command handler 实现业务；可复用 DTO、小型纯 formatter、selector/validation helper。

TUI 本地只维护会话状态，例如页面、`viewed_profile_id`、高亮对象 ID、搜索词、日志 follow 状态与当前 ModuleDraft。它不持有权威 NodeStore、ProfileSettings 副本、RuntimeStatus 或 running_profile_id；运行状态始终来自 xrmd。`viewed_profile_id` 属于 TUI session，`running_profile_id`/实际目标由 Server 提供。查看/切换 Profile 只改变 UI 上下文，不连接或变更运行实例。

总览持续显示 Viewing Profile、Running Profile、Saved Target、Actual Target、Runtime Override、Core/version/state、订阅/节点数量和 pending changes。节点页提供搜索、详情、编辑、复制和“选用并连接”。组页可真正修改成员与模式，不只是读取核心 tag 列表。

原生模块页使用完整清单，不只有DNS/路由几个入口；每行显示中文名称、精确原生键、启用状态、源文件、运行文件、表单/原生编辑支持。高级编辑针对所属ModuleDocument，不存在另一套TUI保存库。

`module.get` 的文档正文可以为空（JSON `null`），这是合法的“无源文件”状态，不得在 Manager 中假定为 string。Enter 对此状态展示未配置并提供未保存原生 wrapper 草稿；保存只创建源文件，不改变 included。disabled 但有源的模块仍可查看/编辑，禁止为打开编辑器而自动启用。

导航可将15项视觉分为“基础/运行约束（env、version、log、api）”“连接/选路（inbounds、outbounds、routing、policy）”“解析/资源（dns、fakedns、geodata）”“观测/统计（observatory、burstObservatory、stats、metrics）”，但分区不变成新的运行根对象。

`xrm tui` 的 Profile ID 参数是可选定位捷径而非启动依赖，普通启动为 `xrm tui`。无 Profile 状态保留 Overview、Subscriptions、Nodes、Cores、Profiles；Groups、Modules、Connect 显示明确 empty state。一个 Profile 时仅自动设为 viewed context，不自动连接；多个时恢复最近有效上下文或显示 picker，不能默认取第一项。Profiles 页面通过列表/选择器完成创建、打开、重命名、克隆、删除，按名称交互且不要求 UUID。节点/订阅是全局数据，可在无 Profile 时管理；无 Profile Connect 时由 UI 提供 Create Default Profile / Open Profiles / Cancel。

`Profile.id` 稳定 UUID 是内部持久化、RPC、引用和精确寻址身份。CLI 的统一 resolver 先尝试合法且存在的 UUID，再做 Profile 名称 exact match；唯一返回 ID，零结果 not_found，重复名称 conflict/ambiguous，禁止 fuzzy match 和自动取首项。所有 CLI `--profile`/Profile ref 均使用同一 resolver；TUI 正常交互使用名称与选择器。VLESS URI 的兼容字段在现有 Codec 中归一化为 Node/原生子对象，不新增第二套参数存储。

节点页的传输/安全/Mux编辑保存Node；入站页的同名子编辑器保存对应inbound；组页编辑bindings，原生固定组在routing中编辑。相同的子对象表单可以复用代码，不共享或覆盖不同对象的数据。

便利操作给出“将修改 stats、policy、metrics”而不是模糊的“优化配置”。未启用API/Metrics时可配置或查看提示，不为填满状态页无声启用全部模块。

文本输入不触发全局动作，Enter 默认查看；明确连接动作才可能中断网络。q 退出 UI 不停止核心。列表按 ID 保持选择，后台刷新不覆盖编辑稿；日志暂停滚动后不抢位置。

CLI 默认普通文本；`--json` 输出结构化结果到 stdout，进度与错误到 stderr。`config show --source` 与 `--runtime` 分清；`config export --runtime --output DIR` 导出多个原生文件，而不是 dump。

## 11. 存储、并发与技术选择

### 11.1 SQLite 的职责

建议表：subscriptions、nodes、profiles（ID/位置索引）、cores、runtime。来源与节点是主数据；模块与组不复制入表。使用 SQLite C API、prepared statements 和短事务，不引入 ORM 或数据库连接池。

迁移仅在真正改变已实现持久格式时写顺序步骤；讨论文档不预建升级平台。实时指标和持续日志不逐条写数据库。

### 11.2 并发模型

一个 libuv loop 线程持有业务状态和数据库连接。[X08]少量 HTTP 下载通过 libcurl easy + libuv 有界工作执行，默认最多三个并发；完成后回到 loop 提交。CPU 较重解析/写出使用不可变输入，不让工作线程持有可变 Store。

同一时刻一个运行切换；后到的 connect/apply 返回忙，不排队积累过期连接。stop 可优先取消后续启动。编辑保存只做文件级冲突检查；运行准备截取输入后，新编辑留待下次应用。

不用 Actor、事件总线、自建线程池、通用工作流或跨全部对象的版本比较系统。

### 11.3 依赖

| 功能 | 选定依赖 |
|---|---|
| 语言/构建 | C++23、CMake + Ninja |
| CLI/TUI | CLI11、FTXUI |
| JSON | nlohmann/json，同一套 DOM；不用它重排未编辑文本 |
| HTTP | libcurl |
| 事件、进程、UDS | libuv |
| 节点与管理存储 | SQLite C API |
| 测试 | Catch2 + CTest |
| 核心安装解包/摘要 | 对应阶段再引入现成库，不提前扩大依赖 |

外部 I/O 使用 RAII 和明确所有权。std::expected 用于解析/I/O 边界，不强迫每个内部函数层层包装。不要每层 catch，也不要吞异常返回空列表当成功。

## 12. 代码组织

```text
apps/
  xrm_main.cpp
  xrmd_main.cpp
src/
  model/                  # Node、ProfileSettings、ModuleDocument、GroupBinding
  contract/               # IPC DTO 和错误码
  client/                 # RpcClient
  cli/
  tui/
  server/                 # Application、ApiRouter、简单 Operation
  subscription/           # 获取、解析、节点库存更新
  profile/                # ProfileService、原生模块与bindings保存
  xray/
    codecs/               # 每种协议的 URI 解析与 outbound 写出
    writers/              # 原生块通用输出 + routing/outbounds/观测等少量专用函数
    runtime_assembler.cpp # 只返回文件集合
    runtime_writer.cpp    # 将集合物化成 confdir
    process_adapter.cpp
    core_manager.cpp
    control.cpp
    metrics.cpp
    installer.cpp         # 到实际安装功能阶段再加入
  infra/                  # HTTP、Process、Store、文件操作
presets/
tests/
docs/
```

主要业务依赖：UI → RpcClient → Server；Subscription → NodeStore；ProfileService → 源文件；Assembler → 模型/JSON；RuntimeWriter/CoreManager → 文件与进程。

这些目录表示职责，不是第一天创建空壳的清单。不为每个 Writer 创建抽象基类，不为每张表建四层架构，不另做一个单文件生成器与多文件生成器并行维护。

## 13. 基本防护与明确上限

本机目录/socket 使用合理权限，订阅 HTTPS 正常验证，HTTP/进程有超时，输出和日志有大小限制。默认消息上限可从 8 MiB、订阅正文 16 MiB、日志内存 4 MiB 起步，并在一个地方设置；它们不是配额平台。

普通日志/列表不打印订阅 URL、UUID、密码。编辑器和导出属于明确读取秘密的操作，返回真实内容并设置合理权限，不能让 `***` 被保存成凭据。

不添加 RBAC、密钥保险库、数据库加密、自动证书轮换或完整沙箱。原生配置可能启用 TUN、DNS 劫持或公开监听，启动前说明实际模式；不把“管理器没执行防火墙命令”当作“核心不会改变系统网络”的保证。

## 14. 测试和维护

### 14.1 四类足够的测试

- Unit：Codec 映射、节点匹配、组筛选、目标解析、各 Writer、文件归属。
- Integration：真实 xrm → UDS → xrmd，临时库/目录，fake-core 记录 argv 并模拟失败。
- Real Core：指定发布版的多文件检查、loopback 连通、routing/balancers 共存、tail 次序、统计与可选 API。
- TUI：常用动作与最小人工交互验收，覆盖中文、小终端、焦点和保存/连接提示。

真实核心不可用标记 SKIP，不用 fake-core 冒充协议测试。测试不使用用户真实订阅，不启动上传配置里的 TUN，不更改开发主机 DNS/路由。

### 14.2 对本方案最关键的回归

导入节点不写confs；对R06全部顶层项参数化测试读写/启停/文件映射；组选择持久保存；原生路由与动态组不互相覆盖；禁用模块无遗留；检查失败旧进程不动；重连用保存设置，自动恢复不发布新编辑。

特别回归：stats空对象、fakedns对象和数组、burstObservatory大小写/独立文件、env生效阶段、version不被自动改写、Policy完整levels/system保留、Geodata资源与配置分离。对子对象测试位置：sniffing只能在所属入站；TLS/REALITY/sockopt/finalmask跟随streamSettings；Mux跟随出站；不产生伪顶层配置。

一个普通fixture静态目录不需要启用全部模块才能测试FileSet；有实际行为的模块用针对性真实核心用例。不通过全网探测或用户路由器配置扩大测试副作用。

模块之间只维护必要引用和输出归属。新字段通常只修改所属 Codec/Writer、表单及对应样例，不重构节点库和整个 Profile。不得为添加一个 XHTTP 参数开发全量能力矩阵。

### 14.3 构建与 Agent 范围

实际建立项目后提供 dev/asan CMake presets、compile_commands.json。只报告执行过的构建/测试；不修改验收条件来消除失败，不自动进入下一阶段。

每次任务做一个用户闭环，按需求 M0–M5 推进。当前阶段允许缺少未来功能，但不允许拿空接口或固定返回的在线状态冒充已实现。

## 15. 资料与核查范围

研究基线保留 Xray-core **v26.9.9**，只作为固定接口参照，不称其永久最新。本次重新读取官方完整文档和配置总览核对原生模块，并参考下列固定标签实现处理合并/弃用边界；未编译XRM、未执行核心联网验收。后续对接新版时记录确切版本，在对应测试中确认，不根据 main 自动扩大产品承诺。

| ID | 一手来源 | 用于确认的边界 |
|---|---|---|
| X00 | [官方完整文档](https://xtls.github.io/llms-full.txt) | 配置字段先读相关原生章节，不以聊天示例代替规范 |
| X01 | [多文件配置](https://xtls.github.io/config/features/multiple.html) | confdir、顺序、tag 覆盖、tail、dump 的诊断用途 |
| X02 | [v26.9.9 infra/conf/xray.go](https://raw.githubusercontent.com/XTLS/Xray-core/v26.9.9/infra/conf/xray.go) | Override 对 RouterConfig、Policy 等的替换；组与规则需共同输出 |
| X03 | [v26.9.9 main/run.go](https://raw.githubusercontent.com/XTLS/Xray-core/v26.9.9/main/run.go) | version/run、test 与 Start 分离、confdir/环境、进程退出 |
| X04 | [Routing](https://xtls.github.io/config/routing.html) | 目标、balancers、selector、strategy、fallback，不照搬 Mihomo 语义 |
| X05 | [Observatory](https://xtls.github.io/config/observatory.html) | background/burst、观测范围、策略与观测关系 |
| X06 | [API](https://xtls.github.io/config/api.html) | listen、RoutingService、官方 API 命令调用 |
| X07 | [Metrics](https://xtls.github.io/config/metrics.html) · [Stats](https://xtls.github.io/config/stats.html) · [Policy](https://xtls.github.io/config/policy.html) | 指标、计数开关、模块间明确关联 |
| X08 | [libuv Process](https://docs.libuv.org/en/v1.x/process.html) · [Thread pool](https://docs.libuv.org/en/v1.x/threadpool.html) | 选定进程与后台工作实现接口，实施时按安装版本核对 |
| X09 | [systemd.kill 官方手册源码](https://raw.githubusercontent.com/systemd/systemd/main/man/systemd.kill.xml) | 正式部署的 cgroup 清理范围，打包时核对 |
| X10 | [Xray配置总览](https://xtls.github.io/config/) | 本次原生 15 项清单及 version 含义 |
| X11 | [入站代理](https://xtls.github.io/config/inbound.html) | InboundObject、settings/streamSettings/sniffing归属 |
| X12 | [出站代理](https://xtls.github.io/config/outbound.html) | OutboundObject、Mux/XUDP、静态出站与默认顺序 |
| X13 | [传输配置](https://xtls.github.io/config/transport.html) | streamSettings、传输、安全、finalmask/sockopt的原生层级 |
| X14 | [FakeDNS](https://xtls.github.io/config/fakedns.html) | fakedns对象/数组及DNS/嗅探关系 |
| X15 | [Geodata](https://xtls.github.io/config/geodata.html) | 配置、已有资产、更新和热重载范围 |
| X16 | [环境变量](https://xtls.github.io/config/env.html) | env与进程启动环境的生效阶段和字符串值 |
| X17 | [旧版反向代理](https://xtls.github.io/config/reverse.html) · [VLESS反向示例](https://xtls.github.io/document/level-2/vless_reverse.html) | 旧顶层reverse已废弃；新功能归协议settings |
| X18 | [传输配置](https://xtls.github.io/config/transport.html) · [v26.9.9 xray.go](https://raw.githubusercontent.com/XTLS/Xray-core/v26.9.9/infra/conf/xray.go) | 核心旧入口与加载实现；全局transport不作为新模块生成 |
| X19 | [DNS出站](https://xtls.github.io/config/outbounds/dns.html) | 顶层dns解析模块与DNS outbound不是同一对象 |

用户提供的多文件配置包仅作为模块拆分风格参考；其中端口、DNS、节点前缀和规则不成为产品固定默认值。本文产品数据结构和用户操作语义是方案选择，不冒称来源仓库的既有实现。

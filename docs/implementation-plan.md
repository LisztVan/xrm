# XRM 实现方案与接口契约

**状态：实现与阶段门完成情况见 `release-acceptance.md`；人工验收失败后的修复按 `product-repair-spec.md` 执行**
**日期：2026-09-23 · C++23 · Linux 优先 · Manager/Server · Xray-only**

本文将已确认的[需求](requirements.md)和[架构](architecture.md)落实为文件职责、跨模块接口、调用顺序和交付边界。与[任务 DAG](task-dag.md)、[测试标准](testing-standard.md)共同使用。原三份基线文档随包保留原文；其中“讨论稿”是原文件中的状态文字，用户后续确认已将其确定为本次实施依据。

## IP-00　文档地位与实施原则

1. 产品行为以需求 R01–R16、验收 T01–T37 为准；总体分层以 architecture.md 为准。本文件只细化未确定的接口和局部实施选择，不恢复已否定的方案。
2. `AGENTS.md` 继续有效。其禁止的“任务 DAG”指**产品内的通用运行工作流引擎**；本包的开发任务 DAG 是供主 Agent 调度编码任务的静态文档，不能实现进 xrmd。
3. 本文代码块是**公开头文件契约**，省略 include、命名空间包裹、私有成员和函数实现。它们不是已编译产品。类型均位于 `namespace xrm`；接口中的 `Result` 等定义见下文。
4. 公开契约按里程碑落地。文档先写全，不等于第一天创建所有未来头文件、空类、数据库表、RPC 或菜单。
5. 一个任务先实现声明和可编译骨架，写会因行为缺失而失败的测试，再写实现。私有函数由负责 Agent 按最简单的方式安排，不逐个预定。
6. 已批准契约由主 Agent 维护；模块 Agent 不修改共享签名来适配自己的实现。真实阻碍用“具体签名、原因、受影响调用者/测试、最小修正”提出，不扩大成架构重做。
7. 没有源码仓库作为本次输入；以下是从零实施的组织方案。执行时先检查实际仓库，复用已有工作；不要为吻合目录图搬动无关文件。

### 必须不变的四条数据流

```text
订阅 -> 解析 -> 节点库                         （不写运行文件）
原生模块编辑 -> 对应 modules 文件             （不连接）
组/选择编辑 -> 对应 bindings/profile 文件      （不连接）
连接/预览 -> 不可变输入 -> FileSet -> confdir  （不是单 config.json）
```

原生配置不链接 libXray、不嵌入 Go；实际解释、检查与执行交给独立 Xray 程序。配置文件合并不由 C++ 模拟。[B-R R01、R05–R12；B-A §2、§6–§9]

## IP-01　工程目录、目标和依赖

```text
CMakeLists.txt                    # 主 Agent / 集成任务独占
CMakePresets.json                 # dev、asan
cmake/
apps/
  xrm_main.cpp                    # Manager 组合根
  xrmd_main.cpp                   # Server 组合根
src/
  model/
    common.hpp node.hpp profile.hpp subscription.hpp runtime.hpp
    json_models.cpp              # 管理器数据的序列化，不处理 Xray 配置合并
    module_catalog.hpp/.cpp
  contract/
    wire.hpp/.cpp                # envelope、错误、编码
    dtos.hpp                     # RPC 对外 DTO；不暴露数据库/uv句柄
    methods.hpp                  # 只登记已经进入实施阶段的方法
  infra/
    files.hpp/.cpp store.hpp/.cpp process.hpp/.cpp http.hpp/.cpp
    limits.hpp
  client/
    rpc_client.hpp/.cpp
  server/
    rpc_server.hpp/.cpp api_router.hpp/.cpp
    application.hpp/.cpp operations.hpp/.cpp
  subscription/
    parser.hpp/.cpp reconcile.hpp/.cpp subscription_service.hpp/.cpp
  profile/
    profile_service.hpp/.cpp
    groups.cpp                   # 同一 ProfileService 的绑定操作实现
    forms.hpp/.cpp               # 常用对象/字段修改，纯函数
    advanced_forms.cpp           # M4 才增加的高级模块便捷操作
  xray/
    codecs/codec.hpp codec.cpp stream.cpp
    codecs/vless.cpp vmess.cpp trojan.cpp shadowsocks.cpp hysteria.cpp
    writers/native.cpp outbounds.cpp routing.cpp observation.cpp
    runtime_assembler.hpp/.cpp runtime_writer.hpp/.cpp
    process_adapter.hpp/.cpp core_manager.hpp/.cpp
    control.hpp/.cpp metrics.hpp/.cpp installer.hpp/.cpp
  cli/
    commands.hpp/.cpp node_commands.cpp profile_commands.cpp
    subscription_commands.cpp runtime_commands.cpp core_commands.cpp
    print.cpp
  tui/
    app.hpp/.cpp state.hpp actions.hpp/.cpp
    nodes.cpp subscriptions.cpp modules.cpp groups.cpp overview.cpp logs.cpp
presets/local/
tests/
  contract/ unit/ integration/ real_core/ tui/ fixtures/ support/
packaging/systemd/
docs/
```

`.hpp/.cpp` 表示一对文件，不是文件名中包含斜线。未到对应任务，不创建该文件。

### 链接边界

只用少量内部目标：

| CMake 目标 | 内容 | 允许依赖 | 禁止依赖 |
|---|---|---|---|
| `xrm_model` | 值类型、序列化、模块描述表、错误 | nlohmann/json、标准库 | Store、网络、进程、UI |
| `xrm_contract` | IPC envelope、DTO 编码 | xrm_model | Server 业务实现 |
| `xrm_logic` | Codec、解析/匹配、表单修改、Assembler/Writer纯函数 | xrm_model | 数据库、libuv、libcurl、UI |
| `xrm_server_lib` | Server、持久化、HTTP、Process、RuntimeWriter、CoreManager | 前述目标、SQLite、libcurl、libuv | FTXUI、CLI11 |
| `xrm_client_lib` | RpcClient | xrm_contract、libuv | Store、CoreManager、Assembler |
| `xrm` | CLI/TUI 组合 | xrm_client_lib、CLI11、FTXUI | xrm_server_lib |
| `xrmd` | 后台服务组合 | xrm_server_lib | CLI/TUI |

测试按需要链接目标，不为每个目录建一个 SDK。`ProcessAdapter` 构造命令的纯函数可编入 xrm_logic；进程执行属于 Server。

**编译约定：** CMake + Ninja、`cxx_std_23`、`compile_commands.json`、合理编译警告；默认不联网拉取任意最新版依赖。实际使用的编译器和库版本在构建测试报告记录，不在文档中伪造已验证版本矩阵。核心下载阶段才引入 libarchive 与 OpenSSL Crypto。

## IP-02　共享类型及错误（IF-00）

`src/model/common.hpp`：

```cpp
using Id = std::string;                    // 随机UUID；不是显示名或凭据hash
using Json = nlohmann::json;
using Milliseconds = std::chrono::milliseconds;
using UnixMillis = std::int64_t;           // 持久时间，UTC毫秒

enum class ErrorCode {
    invalid_argument, not_found, conflict, busy, unsupported,
    io_error, storage_error, network_error, timed_out, cancelled,
    core_check_failed, core_start_failed, core_control_failed,
    protocol_error, too_large, result_unknown
};
struct AppError {
    ErrorCode code;
    std::string message;
    std::optional<std::string> subject;     // ID/模块键/相对文件名，不放凭据
    std::optional<std::string> hint;
};
template<class T> using Result = std::expected<T, AppError>;
template<class T> using Completion = std::function<void(Result<T>)>;

struct PageRequest { std::size_t offset = 0; std::size_t limit = 100; };
template<class T> struct Page {
    std::vector<T> items;
    std::size_t total;
    std::optional<std::size_t> next_offset;
};
struct Notice { std::string code; std::string message; };
```

- `Id` 由应用/存储创建，传进纯函数。普通新 ID 用现成随机源产生 UUID；不引入 ID 服务器。
- 对外枚举序列化为文档中的字符串，不直接输出底层整数。Path 传输为 UTF-8 字符串；Server 文件路径与客户端导出路径明确区分。
- 日志用不含秘密的 `AppError`；核心 stderr 是独立诊断流，标记来源并尽力脱敏，不宣称能识别所有秘密。
- `Result<void>` 表示同步边界已完成。业务“部分成功”用业务报告，不把它伪装成 `Result` 整体失败或空数据。
- 异步返回 `Result<Id>` 的接口：返回 error 表示未接受、回调不执行；返回 ID 表示已接受，最终完成回调**恰好一次**，且不会在接受函数返回前内联执行。
- 回调由 Server loop 线程执行。传入值由服务复制/移动持有；不把 RPC 输入 buffer 的 `string_view` 留到异步阶段。工作线程只处理不可变输入。

### 值类型的头文件归属（避免纯逻辑依赖I/O头文件）

| 头文件 | 集中定义 |
|---|---|
| model/common.hpp | Id、Json、Result、AppError、Page、Notice |
| model/node.hpp | Endpoint/协议/传输/安全、Node、NodeQuery、NodeChanges |
| model/subscription.hpp | Subscription、状态、ParsedNode/ParseReport、RefreshReport、SourceCommit |
| model/profile.hpp | 模块、Profile、组/观测绑定、Target、ProfileSnapshot；ProfileCreate/Summary/Edit、BindingsEdit/ModuleView |
| model/runtime.hpp | CoreInstall、BuildInput、FileSet/PreparedFiles、RuntimeIndex、ProcessSpec/Exit/Result、RunSpec、RunRecord、RuntimePersistent/Status、激活输入输出 |
| contract/dtos.hpp | 只含IPC展示/摘要DTO和编解码声明；引用上述值类型，不引用服务实现 |

类型在后文按使用场景展示，不表示都定义在展示位置的服务头文件。特别是Reconcile不能为了NodeChanges包含Store头文件，ProcessAdapter不能为了ProcessSpec包含ProcessService。服务类只前向声明其具体依赖；不以一个万能头掩盖依赖环。

## IP-03　节点和订阅模型（IF-01）

节点类型在 `model/node.hpp`，源码模型只供节点库与 Codec 使用，不是全部 Xray 原生配置的替代 schema。

```cpp
struct Endpoint { std::string address; std::uint16_t port; };
struct VlessSettings {
    std::string id;
    std::optional<std::string> encryption;
    std::optional<std::string> flow;
    std::optional<std::uint32_t> level;
};
struct VmessSettings {
    std::string id;
    std::optional<std::string> security;
    std::optional<std::uint32_t> level;
};
struct TrojanSettings { std::string password; };
struct ShadowsocksSettings { std::string method; std::string password; };
struct Hysteria2Settings { std::string auth; }; // 内部名；输出字段按固定核心文档映射
using ProtocolSettings = std::variant<VlessSettings, VmessSettings,
    TrojanSettings, ShadowsocksSettings, Hysteria2Settings>;

struct RawTransport {};
struct XHttpTransport {
    std::optional<std::string> host, path, mode;
    std::optional<Json> extra;
};
struct WebSocketTransport {
    std::optional<std::string> path;
    std::map<std::string, std::string> headers;
};
struct GrpcTransport { std::optional<std::string> service_name, authority; };
struct HttpUpgradeTransport { std::optional<std::string> host, path; };
struct HysteriaTransport { Json options; }; // 仅对应原生Hysteria传输参数，不允许任意顶层覆盖
using Transport = std::variant<RawTransport, XHttpTransport,
    WebSocketTransport, GrpcTransport, HttpUpgradeTransport, HysteriaTransport>;

struct NoSecurity {};
struct TlsSecurity {
    std::optional<std::string> server_name, fingerprint;
    std::vector<std::string> alpn;
    Json options = Json::object();       // 仅TLS原生子对象的高级字段
};
struct RealitySecurity {
    std::optional<std::string> server_name, fingerprint;
    std::string password;
    std::optional<std::string> short_id, spider_x, mldsa65_verify;
    Json options = Json::object();
};
using Security = std::variant<NoSecurity, TlsSecurity, RealitySecurity>;
struct StreamNode {
    Endpoint endpoint;
    ProtocolSettings protocol;
    Transport transport;
    Security security;
    std::optional<Json> sockopt, finalmask, mux;
};
struct RawOutbound { Json value; };       // 自包含原生outbound；tag由节点身份赋值
using NodeSettings = std::variant<StreamNode, RawOutbound>;
struct Node {
    Id id;
    std::optional<Id> source_id;
    std::optional<std::string> upstream_id; // 来源提供的可靠唯一ID；没有则null
    std::string original_name;
    std::optional<std::string> alias;
    std::vector<std::string> labels;
    bool enabled = true;
    bool source_missing = false;
    NodeSettings settings;
};
struct NodeChanges { std::vector<Node> upserts; std::vector<Id> erase_ids; };
struct NodeQuery {
    std::optional<Id> source_id;
    std::optional<std::string> protocol, text;
    std::vector<std::string> labels;
    bool include_missing = false;
};
```

**分阶段落实：** M0/M1 只声明当期用到的 VLESS、RAW、NoSecurity、RawOutbound 等分支；C2/C4 契约任务才扩充后续 variant。上面的集合是首版完成后的接口边界，不要求 M0 引用未实现的 Hysteria 函数。WireGuard/HTTP/SOCKS 高级节点可先通过 RawOutbound；不为未要求的完整表单新建空类型。

`options` 不得包含已强类型拥有的同义字段；遇到重复归属返回 invalid_argument，不做“最后谁覆盖谁”。手写完整节点需要超出 typed 模型时明确转换为 RawOutbound，不维持两份可写参数。`allowInsecure` 等旧字段是否移除应按当前固定核心和原生文档处理，不能凭模型默认值改变验证语义。

序列化约定：NodeSettings 带 `kind: stream|raw`；protocol/transport/security 各有自己的 kind；这是**数据库/IPC格式**，不写入 Xray JSON。高级原生字段留在对应 options/RawOutbound。所有生成字段映射由 Codec 的独立测试样例指定，样例同时记录官方字段依据；不能由生成器反向产生自己的expected。[B-A §3.1、§4；X-CODEC]

`model/subscription.hpp`：

```cpp
struct DownloadRoute {
    std::optional<std::string> proxy_url; // null=明确直连；有值则失败不回退
};
struct Subscription {
    Id id;
    std::string name, url, user_agent = "Xray";
    std::map<std::string, std::string> headers;
    bool auto_refresh = true;
    std::chrono::seconds interval{21600};
    DownloadRoute route;
    std::uint64_t settings_revision = 0; // 仅本来源请求设置变化，防旧URL结果覆盖
};
struct SubscriptionState {
    std::optional<UnixMillis> last_attempt, last_success;
    std::optional<std::string> etag, last_modified;
    std::string last_result;
    std::optional<std::string> reported_usage;
};
struct ParsedNode {
    std::optional<std::string> upstream_id;
    std::string name;
    NodeSettings settings;
};
enum class IssueKind { malformed, unsupported, warning };
struct ParseIssue {
    std::size_t item_index;
    IssueKind kind;
    std::string code, message;            // 不包含原URI和密码
};
struct ParseReport {
    std::vector<ParsedNode> nodes;
    std::vector<ParseIssue> issues;
    bool structurally_complete = false;
};
struct RefreshReport {
    Id source_id;
    std::size_t added=0, updated=0, missing=0, malformed=0, unsupported=0;
    bool changed=false, partial=false;
    std::vector<Notice> notices;
};
struct SourceCommit {
    std::uint64_t expected_settings_revision;
    NodeChanges changes;
    SubscriptionState state;
    RefreshReport report;
};
```

## IP-04　Profile、原生模块、绑定模型（IF-02）

`model/profile.hpp`：

```cpp
struct ModuleDescriptor {
    std::string key, label, default_filename;
    bool dedicated_form_available = false; // 按实际实现更新，不作协议能力保证
};
struct ModuleDocument {
    std::string key;
    std::filesystem::path filename;
    std::string source_text;
};
struct ModuleSetting {
    std::string key;
    std::filesystem::path filename;
    bool included = false;
};
enum class TargetKind { node, group, native_outbound, native_balancer };
struct Target { TargetKind kind; std::string id_or_tag; };
struct RunEnvironment {
    std::filesystem::path cwd;
    std::map<std::string, std::string> process_environment;
    std::optional<std::filesystem::path> asset_dir;
    std::optional<std::string> api_endpoint, metrics_endpoint;
};
struct ProfileSettings {
    Id id;
    std::string name;
    Id core_id;
    std::optional<Target> selected;
    bool append_selected_default = false;
    RunEnvironment environment;
    std::vector<ModuleSetting> modules;
    std::vector<Id> additional_node_ids;
    std::optional<std::vector<Id>> enabled_source_ids; // null=旧Profile兼容全部来源；空=新Profile未绑定来源
};
struct ExplicitMembers { std::vector<Id> node_ids; };
struct NodeFilter {
    std::vector<Id> sources;              // 空=全部来源，含手工节点
    std::vector<std::string> protocols;
    std::vector<std::string> name_contains;
    std::vector<std::string> labels;
};
using NodeSelector = std::variant<ExplicitMembers, NodeFilter>;
enum class SelectionMode { automatic, manual };
struct GroupBinding {
    Id id;
    std::string name;
    NodeSelector members;
    SelectionMode selection_mode = SelectionMode::automatic;
    std::optional<Id> selected_node_id;
    Json balancer;                       // tag/strategy/fallbackTag等；禁止selector
};
struct ObservationBinding {
    bool append_used_group_members = false;
    std::vector<Id> additional_node_ids;
};
struct ManagerBindings {
    std::vector<GroupBinding> groups;
    ObservationBinding observation;
};
struct ProfileSnapshot {
    ProfileSettings profile;
    std::vector<ModuleDocument> modules;
    ManagerBindings bindings;
};
struct EditBase { std::optional<std::string> expected_text; };
struct ModuleEdit {
    std::string key;
    EditBase base;                       // null仅指文件应不存在，不表示无条件覆盖
    std::string text;
};
struct ModuleView {
    ModuleDescriptor descriptor;
    ModuleSetting setting;
    std::optional<ModuleDocument> document;
    std::string editor_support;          // native / form_and_native
};
```

### 保存位置固定

- Node/Subscription：SQLite。Profile ID→目录索引也在 SQLite。
- ProfileSettings：`profiles/<id>/profile.json`。不能再复制一份到数据库。
- ModuleDocument：`profiles/<id>/modules/<filename>`，只含一个相应原生 wrapper。
- GroupBinding：`bindings/groups.json`；ObservationBinding：`bindings/observation.json`。
- 所有保存接口局部原子写入、单文件冲突检查。原文保存可以保存暂时未完成的 JSON；不在每次保存时启动 Xray。
- module.get/read 返回原文，不先去注释、不把密码替换成星号；列表不含原文秘密。
- 正文结构检查发生于表单编辑需要解析、运行组装需要该模块时；原文编辑不被表单解析器绑架。采用库的注释解析支持，不自写正则去注释。[X-JSON]

### 管理文件的序列化形状

类型按同名snake_case字段保存，optional无值用null；Target编码为 `{"kind":"node","id_or_tag":"UUID"}`，不在不同入口混用id/tag/value三个名字。NodeSelector编码为 `{"kind":"explicit","node_ids":[...]}` 或 `{"kind":"filter","sources":[],"protocols":[],"name_contains":[],"labels":[]}`。

`profile.json`保存一份ProfileSettings，modules为ModuleSetting数组；`groups.json`顶层是GroupBinding数组；`observation.json`顶层是一个ObservationBinding对象。没有bindings文件时分别按空数组/关闭自动补充的绑定读取，不会创建额外运行模块。原生 `modules/*.json` 则始终使用核心wrapper，不使用这些管理器结构。

组绑定的最小例子（ID为文档虚构示例）：

```json
[
  {
    "id": "6da51d67-bd9b-4eb3-9f3b-091a2f609c10",
    "name": "AUTO",
    "members": {"kind": "filter", "sources": [], "protocols": [], "name_contains": [], "labels": []},
    "selection_mode": "automatic",
    "selected_node_id": null,
    "balancer": {"tag": "xrm.group.6da51d67-bd9b-4eb3-9f3b-091a2f609c10", "strategy": {"type": "leastPing"}}
  }
]
```

这个例子是XRM绑定格式，不是直接交给核心的文件。Selector条件不得复制进runtime。别名只影响展示；名称筛选使用original_name，UTF-8字符串做明确包含比较，不从emoji/地区名猜地理位置。

### 15 项模块目录（IF-03）

`module_catalog()` 返回固定普通数组；不动态加载 schema、不建立插件注册器。

| key | 默认源/运行文件名 | 处理 |
|---|---|---|
| log | 00_log.json | 原文复制 |
| dns | 01_dns.json | 原文复制 |
| inbounds | 02_inbounds.json | 原文复制；子对象在源编辑阶段改 |
| outbounds | 03_outbounds.json | 静态原文 + 可选90_nodes_tail.json |
| routing | 04_routing.json | 需要绑定展开时局部组装，否则原文复制 |
| observatory | 05_observatory.json | 可选补充subjectSelector |
| api | 06_api.json | 原文复制 |
| metrics | 07_metrics.json | 原文复制 |
| stats | 08_stats.json | 原文复制，空对象必须保留 |
| policy | 09_policy.json | 原文复制，levels/system完整保留 |
| fakedns | 10_fakedns.json | 原文复制，对象/数组都保留 |
| geodata | 11_geodata.json | 原文复制，不打包dat、不再做更新器 |
| env | 12_env.json | 原文复制，与进程env分离 |
| version | 13_version.json | 原文复制，不自动添加min/max |
| burstObservatory | 14_burstObservatory.json | 与observatory二选一输出 |

文件名配置允许用户明确修改，但不改变 key；相同输出名返回冲突。普通对象模块只能一个输出文件。模块停用保留源文件，下一次 FileSet 中不出现它。清单依据已冻结 R06；新增核心根项只增加一个描述和其测试，不顺便改整体架构。[B-R R06；X-MODULES]

```cpp
std::span<const ModuleDescriptor> module_catalog();
const ModuleDescriptor* find_module(std::string_view key);
std::string node_tag(std::string_view node_id);
std::string group_tag(std::string_view group_id);
```

节点 tag 是 `xrm.node.<完整UUID>`，组默认 tag 是 `xrm.group.<完整UUID>`。静态原生 tag 保留；生成范围与静态 tag 重复则报告，不重命名用户配置。

## IP-05　存储与文件接口（IF-04）

### 5.1 Store：一个具体类，不按表叠四层

`infra/store.hpp`：构造成功只表示持有句柄，打开失败通过工厂返回。私有实现使用 SQLite C API；所有方法只在 Server loop 线程调用。

```cpp
// 以下CoreInstall定义在model/runtime.hpp，不在Store私有区域。
struct CoreInstall {
    Id id;
    std::string name;
    std::filesystem::path path;
    std::string reported_version;
    bool managed = false;
};
class Store {
public:
    static Result<std::unique_ptr<Store>> open(std::filesystem::path database);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Result<Page<Node>> list_nodes(const NodeQuery&, PageRequest) const;
    Result<Node> get_node(const Id&) const;
    Result<std::vector<Node>> all_nodes() const;
    Result<void> put_node(const Node&);
    Result<void> erase_node(const Id&);

    Result<std::vector<Subscription>> list_sources() const;
    Result<Subscription> get_source(const Id&) const;
    Result<SubscriptionState> get_source_state(const Id&) const;
    Result<void> put_source(const Subscription&);
    Result<void> remove_source(const Id&, bool keep_as_manual);
    Result<void> commit_source(const Id&, const SourceCommit&); // 单个短事务

    Result<void> register_profile(const Id&, const std::filesystem::path&);
    Result<std::filesystem::path> profile_path(const Id&) const;
    Result<std::vector<Id>> profile_ids() const;
    Result<void> unregister_profile(const Id&);

    Result<std::vector<CoreInstall>> cores() const;
    Result<CoreInstall> get_core(const Id&) const;
    Result<void> put_core(const CoreInstall&);
    Result<void> remove_core(const Id&);

    // runtime类型见IF-09。不存在记录时返回空optional，不伪造运行状态。
    Result<RuntimePersistent> load_runtime() const;
    Result<void> save_runtime(const RuntimePersistent&);
};
```

`NodeStore` 是 Store 中节点相关方法的职责名称，不再另建一层同样的数据仓库。节点增删的用户限制在 Application/订阅服务处理：订阅参数不能原地手改；Node raw put 不是直接暴露的 RPC。

Store 不启动网络、不扫描 Profile JSON 找引用、不把数据写进 runs。没有实现过的历史格式不写迁移脚本。`remove_core` 的“仍被引用不可删除”由应用检查实际 profile/runtime，Store 不遍历任意 JSON。

### 5.2 文件操作与 ProfileService

`infra/files.hpp` 的少量基础函数：

```cpp
Result<std::string> read_text(const std::filesystem::path&, std::size_t max_bytes);
Result<void> replace_text(const std::filesystem::path&, std::string_view bytes);
Result<void> write_if_unchanged(const std::filesystem::path&, const EditBase&,
                              std::string_view bytes);
```

替换在目标同目录创建临时文件，再 rename；不跨文件系统假装原子。权限与失败行为集中在这里，不让每个模块自己写一套。不能用可写硬链接共享 candidate 与 active。外部编辑者不受互斥锁控制，不宣称完整多文件事务。

`profile/profile_service.hpp`：

```cpp
struct ProfileCreate {
    std::string name;
    Id core_id;
    std::string preset;                  // 首版local；不是远程模板URL
    RunEnvironment environment;
};
struct ProfileSummary { Id id; std::string name; Id core_id; };
struct ProfileEdit { EditBase base; ProfileSettings value; };
struct BindingsEdit { EditBase base; std::vector<GroupBinding> value; };
class ProfileService {
public:
    ProfileService(Store&, std::filesystem::path profiles_root);
    Result<ProfileSettings> create(const ProfileCreate&);
    Result<std::vector<ProfileSummary>> list() const;
    Result<ProfileSettings> get(const Id&) const;
    Result<void> save(const Id&, const ProfileEdit&);
    Result<ProfileSettings> clone(const Id&, std::string new_name);
    Result<void> remove(const Id&);       // 活动Profile由Application拒绝移除

    Result<std::vector<ModuleView>> modules(const Id&) const;
    Result<ModuleView> module(const Id&, std::string_view key) const;
    Result<void> save_module(const Id&, const ModuleEdit&);
    Result<void> include_module(const Id&, std::string_view key, bool included);
    Result<void> rename_module_file(const Id&, std::string_view key,
                                   const std::filesystem::path& relative_name);

    Result<std::vector<GroupBinding>> groups(const Id&) const;
    Result<void> save_groups(const Id&, const BindingsEdit&);
    Result<ObservationBinding> observation(const Id&) const;
    Result<void> save_observation(const Id&, const EditBase&, const ObservationBinding&);
    Result<void> select(const Id&, std::optional<Target>);
    Result<void> select_group_node(const Id&, const Id& group_id, const Id& node_id);
    Result<void> choose_observer(const Id&, std::optional<std::string> native_key);

    Result<ProfileSnapshot> snapshot(const Id&) const;
};
```

- `select` 更新当前 profile.json 的 selected；不持有 CoreManager，也不组装 FileSet。
- `select_group_node` 只改 groups.json 对应绑定；候选合法性由应用依据当时节点库存检查，不自动连接。
- `choose_observer` 在同一 profile.json 中改两个 ModuleSetting 的 included，保留各自源文件；一次原子替换即可。
- `snapshot` 重新读所需源文件。未保存 TUI 草稿不在输入中；禁用模块即使正文暂时损坏也不阻塞连接。
- `remove/clone` 不复制/删除全局节点库。create 用临时目录完成 Profile 文件后登记数据库，失败不留下看似完整的 Profile；不要求跨库/目录两阶段提交。
- `include_module` 启用没有源内容的模块时提示先配置；显式 `stats` 启用动作可以创建原生空对象，不能对其他模块机械创建空配置。

## IP-06　常用模块编辑：同一来源，少量纯函数（IF-05）

`profile/forms.hpp`：表单和 CLI 均调用 Server 的相同编辑动作；函数读取一个 ModuleDocument，返回一次 ModuleEdit，不写文件、启动进程或生成其他模块。

```cpp
Result<ModuleEdit> set_log_level(const ModuleDocument&, std::optional<std::string>);
Result<ModuleEdit> replace_dns_servers(const ModuleDocument&, std::vector<Json>);
Result<ModuleEdit> replace_dns_hosts(const ModuleDocument&, Json hosts);
Result<ModuleEdit> set_api_services(const ModuleDocument&, std::vector<std::string>);
Result<ModuleEdit> set_metrics_listener(const ModuleDocument&, std::string listen);
Result<ModuleEdit> put_policy_level(const ModuleDocument&, std::string level,
                                   Json complete_level);
Result<ModuleEdit> set_policy_counters(const ModuleDocument&, bool in_up, bool in_down,
                                      bool out_up, bool out_down);
Result<ModuleEdit> replace_fakedns_pools(const ModuleDocument&, Json object_or_array);
Result<ModuleEdit> put_env(const ModuleDocument&, std::string key, std::string value);
Result<ModuleEdit> erase_env(const ModuleDocument&, std::string key);
Result<ModuleEdit> set_version_range(const ModuleDocument&, std::optional<std::string> min,
                                    std::optional<std::string> max);
Result<ModuleEdit> replace_geodata_assets(const ModuleDocument&, std::vector<Json>);
Result<ModuleEdit> set_geodata_schedule(const ModuleDocument&, std::string cron);
Result<ModuleEdit> set_observatory_probe(const ModuleDocument&, std::string url,
                                       std::string interval, bool concurrency);
Result<ModuleEdit> replace_burst_ping_config(const ModuleDocument&, Json complete_ping_config);
```

完整子对象替换（如 policy level、pingConfig）是明确的编辑对象，不是任意 JSON Patch 服务。UI 先读原对象再更改用户实际编辑的字段，保留其余未知值；服务端保留对象外的原生字段。对完整替换可能删除用户字段的操作，界面显示范围。

列表/子对象操作使用一个限定集合，而非路径执行语言：

```cpp
enum class CollectionKind { inbounds, outbounds, route_rules, native_balancers };
enum class ListAction { insert, replace, erase, move };
struct ListEdit {
    CollectionKind collection;
    ListAction action;
    std::size_t index;
    std::optional<std::size_t> destination;
    std::optional<Json> object;           // 原生完整项；没有管理器伪根键
};
Result<ModuleEdit> edit_native_list(const ModuleDocument&, const ListEdit&);
Result<Json> set_rule_target(Json original_rule, const Target&,
                            std::string_view resolved_native_tag);
```

规则保存直接采用 outboundTag/balancerTag，并删除另一种相互竞争的目标字段，保留原始匹配条件，不持久化 `match+target`。不能给未命名入站虚构可寻址 ID；列表操作使用索引并带整文件基线，重排后基线失效则提示重载。insert的index允许0..size；replace/erase为0..size-1；move的destination是移动完成后目标索引，范围0..size-1。无关参数不允许被静默采用；索引含义在CLI帮助中保持一致。

入/出站 TLS、REALITY、Mux、sniffing 编辑器复用纯字段表单，但必须返回所属完整原生对象：节点库编辑通过 node.replace 保存，静态入/出站通过上述列表替换保存，二者不共享可写数据。

**显式跨模块便利操作：**

```cpp
struct ModuleChange { ModuleEdit edit; bool include_after_save = true; };
struct ModuleChangePlan { std::vector<ModuleChange> changes; std::vector<Notice> notices; };
struct SaveManyResult { std::vector<std::string> saved; std::optional<AppError> failure; };
Result<ModuleChangePlan> plan_traffic_stats(const ProfileSnapshot&, bool enabled,
                                          std::string metrics_listen);
SaveManyResult save_module_changes(ProfileService&, const Id&, const ModuleChangePlan&);
```

这不是通用事务或补丁系统。plan 只列明 stats/policy/metrics 的已确认字段；保存前检查这些文件的基线，之后按明确顺序写，各步骤有失败清单。中间失败不连接、不声称全部成功、不自动覆盖外部新编辑。开启 API 与 Metrics 是不同用户选择，不能两者一律强制启用。FakeDNS 首版提供分别可用的原生模块/入站编辑及关联提示，不增加自动宿主网络改写向导。[B-R R06.4、R09；X-STATS]

## IP-07　HTTP、解析、刷新：只提交节点库存（IF-06）

`infra/http.hpp`：

```cpp
struct HttpRequest {
    std::string url;
    std::map<std::string,std::string> headers;
    std::string user_agent;
    DownloadRoute route;
    Milliseconds timeout{30000};
    std::size_t max_body_bytes = 16 * 1024 * 1024;
};
struct HttpResponse {
    long status;
    std::map<std::string,std::string> headers; // 规范化header名称，不改值
    std::string body;
};
class HttpClient {
public:
    explicit HttpClient(uv_loop_t& loop);
    ~HttpClient();
    Result<Id> fetch(HttpRequest, Completion<HttpResponse>);
    Result<void> cancel(const Id& request_id);
    void shutdown(std::function<void()> drained);
};
```

libcurl easy 在有界后台任务执行；默认三个并发。取消通过库的进度/中止入口，结果回 loop。关闭时取消请求并等待工作回调排空，再释放引用对象。HTTP status 不因 TCP 成功而视为有效订阅；正文上限针对解压后字节。下载请求不执行订阅提供的脚本。

`xray/codecs/codec.hpp`、`subscription/parser.hpp`、`reconcile.hpp`：

```cpp
Result<ParsedNode> parse_share_uri(std::string_view uri);
Result<ParseReport> parse_subscription(std::string_view body);
Result<Json> write_outbound(const Node&, std::string_view tag);

struct ReconcileInput {
    Subscription source;
    std::vector<Node> old_nodes;          // 只属于这个来源
    ParseReport parsed;
};
struct ReconcileResult { NodeChanges changes; RefreshReport report; };
Result<ReconcileResult> reconcile(const ReconcileInput&,
                                const std::function<Id()>& make_id);
```

`reconcile` 是纯逻辑；注入一个普通 ID 函数只为确定性测试，不需要 IdProvider 服务。匹配按冻结架构确定性一对一完成。为使用可靠来源 ID，Node 的来源元数据可保存 `upstream_id`；它是管理字段，不进入 Xray JSON。所有 missing 记录只保留最后一份参数与状态，不记录每次刷新历史。

`subscription/subscription_service.hpp`：

```cpp
struct BatchRefreshResult {
    std::vector<RefreshReport> completed;
    std::vector<std::pair<Id,AppError>> failed;
};
class SubscriptionService {
public:
    SubscriptionService(Store&, HttpClient&);
    Result<Id> refresh(std::vector<Id> source_ids, Completion<BatchRefreshResult>);
    void start_schedule(uv_loop_t&);
    void stop_schedule();
};
```

同一来源已有请求时复用该请求结果，不同时下载两份；不同来源可并发。同一批结束后返回每个来源的结果，不自动 apply。调度器只是一个 timer + 下一次到期计算，不是持久任务 DAG。

### 刷新成功/失败契约

| 情况 | 库存 | 报告 |
|---|---|---|
| 2xx有效列表且至少一节点 | 单个来源事务提交 | added/updated/missing数量 |
| malformed条目 | 可提交成功解析项；不标记未出现旧项为missing | partial，损坏数与条目位置 |
| 可识别但不支持协议 | 不伪造节点；保持报告中的unsupported数量 | 结构完整性与协议支持分开 |
| 0有效节点、HTML、正文损坏、HTTP/超时 | 保留旧节点 | 明确失败 |
| 304且有成功状态/库存缓存 | 不替换节点 | not_modified |
| 304但无成功缓存 | 不当成首次成功 | 错误或一次不带条件头重取，限一次 |
| 下载期间来源URL/headers已被修改或来源已删除 | 不提交过期请求结果 | conflict/cancelled，不能重建已删除来源 |

正常更新不会生成 outbounds 文件，也不会为了显示“更新成功”触发核心检查。刷新时不得覆盖 alias/labels/enabled。订阅参数只能通过复制为手工节点后编辑；复制使用新ID并清除 source_id/upstream_id/source_missing。

## IP-08　多文件组装与落盘（IF-07、IF-08）

### 8.1 可在不同 Agent 之间对接的公共数据

`model/runtime.hpp`、`xray/runtime_assembler.hpp`：

```cpp
struct NodeRuntimeEntry { Id id; std::string tag; std::string display_name; };
struct GroupRuntimeEntry {
    Id id;
    std::string tag;
    std::vector<Id> candidate_ids;
    std::optional<Id> selected_node_id;
};
struct RuntimeIndex {
    std::vector<NodeRuntimeEntry> nodes;
    std::vector<GroupRuntimeEntry> groups;
    std::vector<std::string> static_outbound_tags, static_balancer_tags;
};
struct BuildInput {
    ProfileSnapshot settings;
    std::vector<Node> nodes;              // 当次库存值拷贝，不是Store引用
};
struct OutputFile { std::filesystem::path relative_name; std::string bytes; };
using FileSet = std::vector<OutputFile>;
struct PreparedFiles {
    FileSet files;
    RuntimeIndex index;
    std::vector<Notice> notices;
};
Result<PreparedFiles> assemble(const BuildInput&);
```

`assemble` 没有 I/O，不读取环境、不分配全局ID、不读库、不启动核心。相同输入得到相同文件名、字节和节点顺序。**返回 FileSet，不出现 `Json whole_config`。**

### 8.2 Writer 的直接接口

```cpp
Result<OutputFile> emit_native_module(const ModuleDocument&);
Result<RuntimeIndex> resolve_scope(const BuildInput&);
Result<std::vector<OutputFile>> write_outbounds(const BuildInput&, const RuntimeIndex&);
Result<OutputFile> write_routing(const ModuleDocument&, const BuildInput&,
                                 const RuntimeIndex&);
Result<OutputFile> write_observation(const ModuleDocument&, const BuildInput&,
                                     const RuntimeIndex&);
```

这些是公开于内部 logic 目标的函数，不是15个抽象 Writer。没有额外绑定/修改的原生模块走 emit，原字节保留。`write_routing` 或观测实际添加字段时可以规范化**运行副本**，源文件始终不变。不修改的运行模块不因为同次连接而全局格式化。

### 8.3 范围计算固定规则

1. 读取 included 的原生 routing 中 `rules[].outboundTag`、`rules[].balancerTag`；只识别与本地管理节点/绑定组**精确匹配**的 tag。其他原生 tag 保留并交核心处理。
2. `append_selected_default=true` 时默认选择必须存在，并加入相应节点/绑定组；关闭时不追加规则，也不强制需要 selected。
3. 加入 `additional_node_ids` 和观测绑定的明确节点。默认只扩展实际被选择/规则引用的动态组；未使用的空组不阻止运行。
4. 动态组的来源/协议/名称/标签筛选：字段间AND、字段内OR。ExplicitMembers 的缺失项明确报错；查询过滤自然排除 disabled/missing 节点。实际被引用的空组报错。
5. 普通手选组加载其候选以供观察，但原生 selector 仅包含所选节点。手选值不在候选集合时提示修正，不偷选第一个。
6. 生成组模板里 `fallbackTag` 精确引用管理节点时将该节点加入一次。不要递归展开任意原生JSON或嵌套组图；复杂原生链依靠显式 additional_node_ids。
7. 节点去重按 ID，生成顺序按 ID；源路由/静态数组的原有顺序不变。完整静态 balancer 的宽前缀不自动解释成“导出所有订阅”，用户用显式附加节点完成。
8. 必需管理节点不存在/被禁用/source_missing，则组装失败。未选用节点无须逐个检查连接合法性。
9. 要生成节点但 outbounds 模块停用、要展开组/追加规则但 routing 停用，返回有对象名的前提错误，不静默启用。
10. 已启用的两个观测模块同时存在时报告选择冲突；选择器页面通过 choose_observer 处理二选一。

没有选中节点、没有管理器追加默认路由的原生服务端/直连 Profile 可以运行，不强迫制造节点库记录。

### 8.4 模块输出规则

| 模块 | 允许的组装修改 | 必须保留 |
|---|---|---|
| routing | 追加已使用动态balancers、可选追加默认规则 | 原规则顺序、原生balancers、未知路由选项 |
| outbounds | 静态文件保留；生成已解析范围的90_nodes_tail.json | 静态首出站及tag；节点传输/TLS/REALITY/mux子结构 |
| observatory / burstObservatory | 绑定开启时在subjectSelector末尾补充本次tags并去重 | 静态selector在前、全部探测参数、正确根键 |
| 其他全部模块 | 没有隐式组装改写；复制模块字节 | 字段、值类型、空stats对象、子对象层级 |

默认规则使用对应目标的原生 outboundTag 或 balancerTag，覆盖TCP/UDP，位于原规则末尾；不能挪走用户原catch-all。动态组冲突不覆盖静态balancer。手选模式使用已保存的单候选原生策略，不输出 `select/manual` 核心策略。[X-ROUTING]

**原生分片约束：** routing/policy 等对象不拆成两份半对象；静态与生成outbounds按原生数组规则分片，生成文件带tail。核心最终合并，不自研deep-merge。编号不是C++初始化顺序。[X-MULTI]

### 8.5 RuntimeWriter：只负责文件集合

`xray/runtime_writer.hpp`：

```cpp
enum class DirectoryPurpose { preview, candidate };
struct PreparedDirectory {
    Id id;
    DirectoryPurpose purpose;
    std::filesystem::path confdir;
    RuntimeIndex index;
};
class RuntimeWriter {
public:
    explicit RuntimeWriter(std::filesystem::path data_root);
    Result<PreparedDirectory> materialize(const PreparedFiles&, DirectoryPurpose, const Id&);
    Result<void> export_files(const std::filesystem::path& confdir,
                              const std::filesystem::path& new_destination);
    Result<void> discard(const PreparedDirectory&, std::span<const Id> protected_run_ids);
};
```

- materialize 使用新目录；生成过程中不修改当前active。失败清理不完整candidate。
- 路径仅允许相对普通文件名，无 `..` 越界、无重名；不把profile/bindings/index放进confs。
- 不沿用旧目录内容，因此禁用模块无残留。不用硬链接共享可写inode。
- 检查和启动指向 materialize 返回的**同一目录**；不能检查A、启动B。
- 当前/上一份/候选/有限预览的保留规则由 CoreManager/Application 提供引用集合；discard核对目录在自有根下且ID不在protected_run_ids，Writer不根据mtime猜哪份是active。清理使用当时的引用集合，不因后台任务开始时的旧集合删除新active。
- export 是文件集合复制，不运行dump再保存。目标非空需明确覆盖授权；默认报冲突。导出不偷偷打包证书与dat。

## IP-09　进程、核心适配与生命周期（IF-09、IF-10）

### 9.1 基础进程接口

`infra/process.hpp`：

```cpp
struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments;   // 不包含argv[0]，由Process统一添加
    std::filesystem::path cwd;
    std::map<std::string,std::string> environment;
};
using ProcessId = std::uint64_t;          // Server内代次，不是可持久化PID
struct ProcessExit { int exit_code; int term_signal; };
struct ProcessResult {
    ProcessExit exit;
    std::string stdout_text, stderr_text;
    bool output_truncated = false;
};
struct ProcessCallbacks {
    std::function<void(ProcessId, std::string)> stdout_chunk;
    std::function<void(ProcessId, std::string)> stderr_chunk;
    std::function<void(ProcessId, ProcessExit)> exited;
};
class ProcessService {
public:
    explicit ProcessService(uv_loop_t&);
    ~ProcessService();
    Result<ProcessId> spawn(ProcessSpec, ProcessCallbacks);
    Result<Id> execute(ProcessSpec, Milliseconds timeout, Completion<ProcessResult>);
    Result<void> terminate(ProcessId, Milliseconds grace);
    Result<void> cancel_command(const Id& command_id);
    std::optional<int> pid(ProcessId) const;
    void shutdown(std::function<void()> drained);
};
```

生产进程 spawn 没有“健康”承诺，返回表示操作系统已创建进程。输出按chunk回调可能拆行，日志模块负责拼行并限制单行大小。exited 处理并释放进程句柄；不能在 exit callback 之外凭数字PID杀其他进程。短命命令 execute 捕获输出并设置deadline，非零exit仍带ProcessResult，由适配器判断含义。所有输出有界，管道过长不无限存储。

ProcessId 在本次Server内唯一，旧回调不能改变新核心状态。shutdown 先停止子进程、回收uv句柄与工作回调，最后允许Application销毁；不能在析构后回调悬空对象。[X-UV]

### 9.2 唯一核心命令构造位置

`xray/process_adapter.hpp`：

```cpp
struct RunSpec {
    std::filesystem::path binary, confdir, cwd;
    std::map<std::string,std::string> environment; // 此次check/run共用的进程环境
};
ProcessSpec version_command(const std::filesystem::path& binary);
ProcessSpec check_command(const RunSpec&);
ProcessSpec run_command(const RunSpec&);
ProcessSpec dump_command(const RunSpec&);
Result<std::string> read_core_version(const ProcessResult&);
```

输出argv分别为 `version`、`run -test -confdir DIR`、`run -confdir DIR`、`run -dump -confdir DIR`。路径中空格不是shell转义问题，作为一个argv元素传入。

环境在运行准备时从约定的继承环境加Profile显式设置建立一次，清理会引入另一配置位置的变量；资源目录映射按核心文档。check与run使用相同有效环境。模块内env不修改Server环境，也不替代启动前设置。运行记录标明实际context，外部环境/证书变化不在回退保证内。

`-test` 在真正 server.Start 之前结束；退出0不能证明端口绑定或远端连通。`-dump`用于诊断，不能当作可编辑的原模块备份。[X-RUN]

### 9.3 运行类型与记录

`model/runtime.hpp`：

```cpp
enum class LogStream { server, core_stdout, core_stderr };
enum class CoreState { stopped, starting, running, stopping, failed };
enum class RuntimeHealth { process_only, control_ready, control_unavailable };
struct RunRecord {
    Id run_id, profile_id, core_id;
    RunSpec spec;
    std::optional<Target> applied_selection;
    RuntimeIndex index;
    std::optional<std::string> api_endpoint, metrics_endpoint; // 本轮启动上下文，replay沿用
    UnixMillis started_at;
};
struct RuntimePersistent {
    bool desired_running = false;
    std::optional<RunRecord> active, previous;
};
struct RuntimeStatus {
    CoreState state;
    bool desired_running;
    std::optional<int> pid;
    std::optional<RunRecord> active;
    RuntimeHealth health;
    std::optional<AppError> last_error;
};
struct BuildRequest { BuildInput input; CoreInstall core; };
struct ReplayRequest { RunRecord record; CoreInstall core; };
using ActivationInput = std::variant<BuildRequest, ReplayRequest>;
enum class ActivationMode { start_or_replace, check_only, preserve_running_state };
struct ActivationRequest { ActivationInput input; ActivationMode mode; };
enum class ActivationEffect { checked_only, running_new, restored_old, stopped, unknown };
struct ActivationResult {
    ActivationEffect effect;
    std::optional<RunRecord> active;
    std::optional<AppError> attempt_error;
};
struct LifecycleOptions {
    Milliseconds check_timeout{30000};
    Milliseconds stop_grace{5000};
    Milliseconds process_observation{2000};
    std::array<Milliseconds,3> retry_delay{Milliseconds{1000},Milliseconds{3000},Milliseconds{10000}};
};
```

`restored_old` 是**连接请求失败的结果**，不是新连接成功。API外层提供实际作用和错误，不让CLI仅因有active就退出0。`CoreInstall`、RunSpec、RuntimePersistent 属于model/runtime.hpp；在IF-04提前展示CoreInstall仅为易读，不把它定义在Store私有区域。

### 9.4 CoreManager 公共接口

```cpp
class CoreManager {
public:
    CoreManager(uv_loop_t&, Store&, ProcessService&, RuntimeWriter&, LifecycleOptions);
    bool busy() const;
    RuntimeStatus status() const;
    void on_output(std::function<void(LogStream, std::string)>); // 组合根接到LogBuffer
    Result<Id> activate(ActivationRequest, Completion<ActivationResult>);
    Result<Id> stop(Completion<ActivationResult>);
    Result<Id> restore_after_server_start(Completion<ActivationResult>);
    void shutdown(std::function<void()> drained);
};
```

CoreManager 可以调用纯assemble与RuntimeWriter，但不读ProfileService、不获取订阅、不访问UI。Application先取得本轮保存设置和节点值，再交给它。大型assemble/write在libuv后台工作中运行，完成后回loop控制进程与保存RunRecord。

**单个CoreManager即一个运行互斥入口。** activate接受后马上标记preparing；第二次activate返回busy，不排队。stop允许抢占。预览check可以使用独立检查进程，不占生产CoreHandle；必须有自己的有界命令slot。

### 9.5 顺序固定，不引入事务框架

1. 从请求中取得本轮不可变输入。后续保存属于下一轮；不建全局model_epoch重试。
2. 新构建调用assemble并物化新candidate；replay复用旧完整目录，禁止重建旧节点。
3. 使用候选的核心/cwd/env执行一次check。失败结束，旧进程/active不变。
4. check_only，或preserve_running_state且原本停止：返回预览/检查结果，不启动、不将预览伪装为成功运行。
5. 准备切换时保存old，正常停止old，创建新进程。没有API可用时以观察窗内存活报告process_only；有显式端点才做相应有限探测，不能自动插入StatsService。
6. 启动成功后保存RuntimePersistent：old为previous，candidate为active。成功保存后才报告完整运行成功。
7. 启动或记录保存失败：停止candidate，使用old原目录恢复**一次**；结果返回restored_old并保留attempt_error。不能重建当前Profile作为“回退”。
8. 不修改用户源文件和保存的选择。保存B但连接失败恢复A时，UI显示“希望B，实际A”。
9. 普通连接允许重启中断，不先实现所有动态API热重载。原生启动日志写文件时不为了GUI可见而重写模块。

### 9.6 stop、取消、恢复与核心切换

- stop先尽力保存desired_running=false，再标记当前操作禁止任何后续启动，停止正在启动/运行的本次句柄；保存失败仍停止并报告限制。
- preparing后台工作不能被线程强杀，但其完成后只清理candidate，不启动；checking可取消短命check；old已停止时不再恢复old；已提交新active时保留记录但停止运行。
- 每个启动/重试回调都检查该操作的取消标志和当前代次。标志只属于CoreManager，不在多个服务各放一套。
- 用户主动stop、正常替换old不触发崩溃重试；意外退出最多三次，连续稳定运行60秒后可重置次数。自动恢复按active目录，不发布新模块设置或订阅。
- Server正常退出保留用户运行意图；系统服务停止通过cgroup清理。重启时读取active/desired，不接管历史PID。
- 显式rollback取previous；原来停止则只检查/登记目标不拉起核心，原来运行则恢复。它不改源设置；后续普通connect仍组装最新保存内容。
- core use正在运行时用ReplayRequest检查原文件+新程序；不同时应用新设置。成功后仅更新对应Profile的core_id，不覆盖其他已保存字段。若这一步保存失败，返回“核心已切换、选择未保存”的部分结果并明确当前实际版本，不能宣称下次仍使用新版本；用户可重新保存。这里不为跨文件和进程强造原子保证。

最后一点是对基线未细分故障的局部处理约定：不隐藏部分成功，也不在用户源文件并发变更时强行回写。正常成功路径仍为“运行成功后才保存核心选择”。

## IP-10　控制、指标、日志与核心安装（IF-11）

### 10.1 可选API控制

`xray/control.hpp`：

```cpp
struct ControlTarget { RunSpec run; std::string endpoint; Milliseconds timeout{3000}; };
struct OverrideResult { bool confirmed; std::optional<std::string> actual_outbound; };
class XrayControl {
public:
    explicit XrayControl(ProcessService&);
    Result<Id> balancer_info(ControlTarget, std::string balancer_tag, Completion<Json>);
    Result<Id> override_outbound(ControlTarget, std::string balancer_tag,
                                std::string outbound_tag, Completion<OverrideResult>);
    Result<Id> clear_override(ControlTarget, std::string balancer_tag,
                             Completion<OverrideResult>);
};
```

只供明确的runtime动作；普通select/connect不用它。adapter集中核对当前核心`api bo/bi`，参数放在位置参数前，使用结构化输出。超时最多查询一次bi，无法确认返回result_unknown；不反复重试写命令。Override不保存Profile、不补写run配置，不恢复为下次启动的隐含选择。[X-CONTROL]

RuntimeIndex 可附带已知静态outbound/balancer tag列表供显示和检查；这是由原生模块读取的可丢弃索引，不是另一份配置数据库。无法解析的高级原生控制情况报告不支持该操作，不妨碍原生配置运行。

### 10.2 指标和有界日志

`xray/metrics.hpp`：

```cpp
struct TrafficCounter {
    std::string scope, tag;
    std::optional<std::uint64_t> uplink, downlink;
};
struct NodeObservation {
    std::string tag;
    std::optional<bool> alive;
    std::optional<double> latency_ms;
};
struct MetricsSample {
    Id run_id;
    UnixMillis received_at;
    std::vector<TrafficCounter> traffic;
    std::vector<NodeObservation> observations;
};
struct TrafficRate {
    std::string scope, tag;
    std::optional<double> uplink_bytes_per_second, downlink_bytes_per_second;
};
Result<MetricsSample> parse_metrics(const Json&, const Id& run_id, UnixMillis);
std::vector<TrafficRate> compute_rates(const MetricsSample& previous,
                                      const MetricsSample& current,
                                      std::chrono::duration<double> elapsed);
class MetricsClient {
public:
    MetricsClient(uv_loop_t&, HttpClient&);
    void attach(const RunRecord&, std::optional<std::string> endpoint);
    void set_viewer_count(std::size_t);
    std::optional<MetricsSample> latest() const;
    void on_sample(std::function<void(const MetricsSample&)>);
    void stop();
};
```

单调时钟计算elapsed；响应关联run_id，旧进程延迟到达的HTTP结果不能污染新实例。无观察值不填0ms，无计数不填0字节。restart/计数下降/非正elapsed时对应速率unknown。一次采样供所有客户端；无查看者停止高频请求。Metrics内容按真实核心fixture读取，不以“JSON可解析”代替字段语义验证。[X-METRICS]

日志的跨模块值类型放在model/runtime.hpp，缓冲类在server/log_buffer.hpp/.cpp（M1创建）：

```cpp
struct LogEntry {
    std::uint64_t sequence;
    UnixMillis received_at;
    LogStream stream;
    std::string text;
    bool line_truncated = false;
};
struct LogRead {
    Id boot_id;
    std::vector<LogEntry> entries;
    std::uint64_t next_cursor;
    bool truncated;
};
class LogBuffer {
public:
    explicit LogBuffer(Id boot_id, std::size_t max_bytes);
    void append(LogStream, std::string_view chunk);
    LogRead read(std::optional<LogStream>, std::uint64_t cursor, std::size_t limit) const;
};
```

CoreManager只暴露on_output回调，不依赖Server的LogBuffer类；组合根把核心输出接入缓冲。LogBuffer与CoreManager在同一loop调用，不再建日志线程。游标表示“最后读到的sequence”，读取其后的记录；超出已保留窗口/boot_id变化要报告截断或重置，不能丢记录而伪装连续。

内存默认4MiB，单行16KiB；逐stream拼行，未结束尾行在进程退出时以最后一个换行chunk刷新，不能无限等待换行而丢最后诊断。超出截断明确标记，不写SQLite。客户端没有消费时不积累无限输出队列。

### 10.3 核心登记与安装

系统核心登记：Application执行version_command，成功得到实际版本后写CoreInstall；不覆盖、不删除程序。登记不启动生产代理。

M4的 `xray/installer.hpp`：

```cpp
struct ReleaseAsset { std::string name, url; std::optional<std::string> sha256; };
struct CoreRelease {
    std::string tag;
    UnixMillis published_at;
    bool prerelease;
    std::vector<ReleaseAsset> assets;
};
struct InstallRequest { CoreRelease release; std::string asset_name; };
class CoreInstaller {
public:
    CoreInstaller(HttpClient&, ProcessService&, std::filesystem::path cores_root);
    Result<Id> releases(bool include_prerelease, Completion<std::vector<CoreRelease>>);
    Result<Id> install(InstallRequest, Completion<CoreInstall>);
};
```

安装只接受官方release中明确选择的资产；HTTPS下载、现成摘要与归档库、仅解出期望核心文件、防归档路径越界，执行version再登记。没有可用官方摘要时如实报告核查缺项，不伪造“签名已验证”；是否继续是显式安装策略，不添加一套密钥信任平台。安装不等于core use。系统核心remove永远不是删除系统文件。普通离线测试使用本机HTTP/归档fixture，不请求GitHub。

## IP-11　Application 与 IPC（IF-12、IF-13）

### 11.1 Application 是流程协调，不是所有逻辑的大类

`server/application.hpp`：

```cpp
class Application {
public:
    Application(Store&, ProfileService&, SubscriptionService&, CoreManager&,
                ProcessService&, RuntimeWriter&);
    Result<Node> add_node(std::string name, NodeSettings);
    Result<void> replace_node(const Id&, const Node& edited);
    Result<Node> copy_node(const Id&, std::string new_name);
    Result<void> remove_node(const Id&);
    Result<Id> import_nodes(std::string content, Completion<std::vector<Id>>);
    Result<Id> register_core(std::filesystem::path binary, std::string name,
                             Completion<CoreInstall>);
    Result<Id> use_core(const Id& profile_id, const Id& core_id,
                       Completion<ActivationResult>);
    Result<Id> connect(const Id& profile_id, std::optional<Target> new_selection,
                       Completion<ActivationResult>);
    Result<Id> apply(const Id& profile_id, Completion<ActivationResult>);
    Result<Id> render(const Id& profile_id, Completion<PreparedDirectory>);
    Result<Id> check(const Id& profile_id, Completion<ProcessResult>);
    Result<Id> dump(const Id& profile_id, Completion<ProcessResult>);
    Result<Id> disconnect(Completion<ActivationResult>);
    Result<RuntimeStatus> status() const;
};
```

add_node/copy_node由应用分配新ID；replace_node保持原ID和来源归属，并检查订阅节点只改别名、标签和enabled。remove_node只更新库存，不擅自修改Profile/routing；缺失引用在下一次连接明确报错。核心use与文件保存失败的边界按IP-09处理。

列表、普通增删改可由ApiRouter调用明确所属的服务，不为透传层层增加同名函数。连接路径：检查busy → 可选保存选择 → ProfileService.snapshot + Store.all_nodes + CoreInstall → CoreManager.activate。只有Application知道这个用户动作需要哪些数据；TUI不复制该逻辑。

`new_selection`为空表示不修改现有选择；清除选择通过独立select请求表达，不能把“参数没传”和“明确清除”混淆。选择保存成功而连接失败，返回结果必须显示保存已成功、运行未切换。busy应在保存便捷选择前检查，避免用户点击被拒绝却更改了选择。

### 11.2 IPC envelope

同包协议主版本1、UTF-8 JSON Lines、AF_UNIX stream。Manager发起客户端请求ID，与长操作ID分开。异常、失败或缺字段不回固定成功。

```json
{"v":1,"id":17,"method":"module.get","params":{"profile_id":"p1","module":"policy"}}
```

```json
{"v":1,"id":17,"ok":true,"result":{"module":"policy","filename":"09_policy.json","included":true,"text":"{\"policy\":{}}"}}
```

```json
{"v":1,"id":18,"ok":false,"error":{"code":"conflict","message":"policy已被另一个编辑器修改","subject":"policy","hint":"重载后合并你的修改"}}
```

```json
{"v":1,"id":19,"ok":true,"result":{"status":"accepted","operation_id":"boot-7:12"}}
```

`operation.get`返回 `accepted|running|succeeded|partial|failed|cancelled|unknown`、phase、result/error。恢复了旧核心时operation为failed，result.effect=restored_old，并有原错误；不能用succeeded表达。

### 11.3 最小可实现的传输接口

```cpp
struct RpcRequest { int v; std::uint64_t id; std::string method; Json params; };
struct RpcResponse {
    int v; std::uint64_t id; bool ok;
    std::optional<Json> result;
    std::optional<AppError> error;
};
Result<RpcRequest> decode_request(std::string_view line);
std::string encode_response(const RpcResponse&);  // 含一个末尾换行

class RpcClient {
public:
    explicit RpcClient(uv_loop_t&);
    Result<Id> open(std::filesystem::path socket, Completion<void>);
    Result<std::uint64_t> call(std::string method, Json params, Completion<Json>);
    void on_event(std::function<void(std::string topic, Json payload)>);
    void close();
};
using RpcReply = std::function<void(RpcResponse)>;
using RpcHandler = std::function<void(const Id&, RpcRequest, RpcReply)>;
class ApiRouter {
public:
    ApiRouter(Application&, Store&, ProfileService&, SubscriptionService&, CoreManager&);
    void dispatch(const Id& connection_id, RpcRequest,
                  RpcReply reply);
};
class RpcServer {
public:
    RpcServer(uv_loop_t&, RpcHandler);
    Result<void> listen(const std::filesystem::path& socket);
    void publish(std::string topic, Json data);
    void on_viewer_count(std::function<void(std::string topic, std::size_t)>);
    void close(std::function<void()> drained);
};
```

RpcServer只依赖RpcHandler函数契约；组合根用lambda转发ApiRouter.dispatch。传输测试可以传测试handler，因此I02不依赖I03的Application，也不必创建生产假业务实现。

RpcClient.open是异步连接：回调成功后才能call，连接未完成不得将写请求隐藏排队。RpcServer维护events.subscribe的连接主题集合并在断线时移除；publish只向相应主题的查看者通知，on_viewer_count供MetricsClient管理采样。ApiRouter对普通请求恰好回复一次，长操作先回accepted、终态经operation/events查询，不能重复发第二个同ID响应。

ApiRouter把schema边界检查与方法路由集中在服务入口，再调用对应函数；不把各业务类变成独立RPC服务。`wire.cpp`仅编解码帧，不知道Store。每帧上限8MiB、列表默认100行；文本超限必须报错，不截断后保存。处理半包、粘包、换行转义、EOF、重复响应与断连。

读写请求都不得自动重放。CLI等待可用同一loop驱动，TUI采用通信线程将结果转成UI事件。断开后已被Server接受的操作继续，客户端重连查状态和operation，不再发送connect。

### 11.4 必需方法清单与参数

下表是跨Agent共享的wire契约；具体枚举按IF-00字符串编码。`expected_text`是编辑本文件时拿到的原文（null=期望不存在），不是全局revision或hash树。

| method | params主要字段 | result | 所属/副作用 |
|---|---|---|---|
| server.info | 无 | version,protocol,boot_id | 只读 |
| subscription.list/get | page / id | summary page / 完整编辑对象 | Store，list不含URL秘密 |
| subscription.add/replace/remove | 定义 / id+定义 / id+keep_as_manual | id/完成 | 只改来源/库存 |
| subscription.refresh | source_ids或all | operation_id | SubscriptionService；最终BatchRefreshResult |
| node.list/get | query,page / id | summary page / 完整Node | Store |
| node.add/import/replace/copy/remove | Node草稿/正文/id+草稿/id/id | id或IDs | Application执行所有权规则；不写confs |
| node.set_enabled/set_labels/set_alias | id+对应值 | 完成 | 只节点库 |
| profile.create/list/get/clone/remove | 定义/无/id/id+name/id | Profile或列表 | ProfileService；无节点库副本 |
| profile.save | id,expected_text,settings | 完成 | 只profile.json |
| module.list/get | profile_id / profile_id,module | ModuleView(s) | 全15项同接口 |
| module.save | profile_id,module,expected_text,text | saved_only | 只该源文件，可保存中间稿 |
| module.include/rename_file | profile_id,module,included / filename | 完成 | 只对应元设置；不启核心 |
| module.form_edit | profile_id,module,expected_text,action,value | saved_only+notice | action闭集对应IF-05，不是任意JSON Patch |
| module.change_plan | profile_id,action,arguments | ModuleChangePlan | 明确跨模块便利动作的预览 |
| module.save_changes | profile_id,plan | SaveManyResult | 逐文件保存，失败可部分完成 |
| group.list/get/save/remove | profile_id及id/expected_text+完整groups | groups/完成 | 只bindings/groups.json |
| group.select | profile_id,group_id,node_id | saved_only | 永久选择，不调用API |
| observation.save | profile_id,expected_text,binding | 完成 | 只bindings/observation.json |
| observation.choose | profile_id,module或null | 完成 | profile.json中两种观测的included |
| connection.select | profile_id,target或null | saved_only | 只selected |
| connection.connect | profile_id,target可省略 | operation_id | 保存可选选择并连接 |
| connection.disconnect | 无 | operation_id | 与core.stop一致 |
| config.render/check/dump | profile_id | operation_id | 新preview，不改active |
| config.apply | profile_id | operation_id | 当前运行则切换，停止则check_only |
| config.export | profile_id,source(saved或runtime),output | files/export result | 保持多文件；不是dump |
| config.files/show | profile_id,source,module可选 | paths/text | 明确源与运行 |
| config.rollback | 无 | operation_id | 旧run，源不变 |
| core.register/list/use | path+name/无/core_id,profile_id | CoreInstall(s)/operation | 系统程序不改；use不发布新模块 |
| core.start/restart/stop | profile_id可选/无/无 | operation_id | start/restart重新准备保存设置 |
| core.releases/install/remove | prerelease/版本资产/id | 列表/op/完成 | M4才实现 |
| group.override/clear | profile_id,group_tag,node_tag,runtime:true | operation_id | 临时控制，不保存选项 |
| runtime.status | 无 | 期望/实际选择、状态、运行目录、dirty标记 | 不生成配置 |
| runtime.stats | 无或watch | latest或unavailable | 不伪造零值 |
| logs.read | stream,cursor,limit | entries,next_cursor,truncated | 有界 |
| operation.get | operation_id | 操作最终状态/当前阶段 | 无任务重播 |
| events.subscribe | topics数组 | subscribed | 仅通知，不持久事件总线 |

导出路径解释：本地Manager导出默认由Manager在自己的输出目录写Server返回的FileSet，若内容过大可逐文件读取；Server的显式导出路径是Server主机路径并在CLI标明，不能混用二者。不提供任意系统文件读写平台。

### 11.5 任务和状态

`server/operations.hpp/.cpp`只管理用户操作状态，不执行或调度业务：

```cpp
enum class OperationState { accepted, running, succeeded, partial, failed, cancelled, unknown };
struct OperationView {
    Id id;
    std::string kind, phase;
    OperationState state;
    std::optional<Json> result;
    std::optional<AppError> error;
};
class Operations {
public:
    explicit Operations(Id boot_id);
    void accepted(Id id, std::string kind);
    void phase(const Id&, std::string phase);
    void finish(const Id&, OperationState terminal,
                std::optional<Json> result, std::optional<AppError> error);
    OperationView get(const Id&) const;
};
```

OperationView是contract/dtos.hpp中的展示类型；Operations是Server具体容器，不被CoreManager/Assembler包含。ApiRouter把已接受的业务ID登记，完成回调finish；本期实现不提供实时细阶段时可显示running，不能捏造阶段进度。并行长期操作上限沿用已有槽位，保留全部未完成记录及最多64条近期终态；未知ID返回unknown。只有业务完成才标终态，不能凭计时器填成功。

操作ID带boot_id，重启后unknown而不是假成功。CoreManager的内部change id可作为相应operation_id登记，不再为同一变化建立多层调度任务。HTTP子请求ID仍是I/O标识，不暴露为用户配置任务。

UI dirty：保存成功后标记相关模块/选择/节点输入待应用；连接时记录本轮输入摘要用于展示即可。不要状态轮询时调用assemble或xray check。后续外部编辑在读/重载/连接时识别，观察到后通知；没持续扫描时允许显示“未检查外部变化”，不谎称磁盘必然无变化。

## IP-12　CLI/TUI 的实现接点（IF-14）

CLI11只负责参数解析、调用RpcClient、等待结果与渲染。FTXUI 是独立的 long-running interactive frontend，通过 RpcClient 直接调用 Server API；它只保存焦点、过滤、选中ID、编辑草稿、viewed Profile 等 UI session state 和最新 Server 快照。Server没有终端组件，Manager没有生产Store。CLI command handler 不是 TUI 的业务层；TUI 不构造 CLI argv 再调用 CLI handler。两者共享 RpcClient/API/DTO/业务语义，不共享 command structure、navigation、context、page flow 或 input model。

最小会话状态示意：

```cpp
struct TuiState {
    Page page;
    std::optional<Id> viewed_profile_id;
    std::optional<Id> highlighted_node_id;
    std::optional<Id> highlighted_group_id;
    std::string node_search;
    bool log_follow;
    std::optional<ModuleDraft> module_draft;
};
```

该状态不得成为第二份业务数据源：Profile/Node/Runtime 状态需要时通过 RPC 读取；`running_profile_id` 与实际目标只由 Server 状态返回。一个 Profile 可自动成为 viewed context；多个 Profile 恢复已知有效的本地 context 或显示 picker，绝不任取首项。查看/切换 viewed Profile 不触发 connect/apply。

CLI Profile ref 统一通过 `resolve_profile_ref(std::string_view)` 解析：先确认合法 UUID 且记录存在；否则 Profile name exact match。唯一命中返回稳定 ID，零命中 not_found，重名 conflict/ambiguous；禁止 fuzzy matching。所有带 Profile 参数的 CLI command 共用该 resolver。内部 ID 仍用于 RPC 和存储，TUI 主交互使用可读名称/选择器。

```cpp
struct ConnectAction { Id profile_id; std::optional<Target> target; };
struct SaveModuleAction { Id profile_id; ModuleEdit edit; };
struct SelectAction { Id profile_id; std::optional<Target> target; };
// 其余动作按当期RPC直接映射，不建立动态命令插件。
class ClientActions {
public:
    explicit ClientActions(RpcClient&);
    void connect(const ConnectAction&, Completion<Json>);
    void select(const SelectAction&, Completion<Json>);
    void save_module(const SaveModuleAction&, Completion<Json>);
};
```

这一层只对真正被多个视图共用的复合等待/提示动作封装，不为每个RPC再机械透传两层。所有Server返回错误都保留code，UI不靠匹配中文文字做流程判断。

### 页面与最小交互对应

当前前端定型为七个顶层页，依次为 Overview、Profiles、Modules、Subscriptions、Nodes、Logs、Config。Groups 属于 Nodes 特殊出口分类，统计属于 Overview，Core 属于 Config → Core；Config 另有 Appearance、Editor、General。顶层与二级导航均有可见焦点，标题、顺序、数字键来自一份导航定义。CLI 命令结构不因 TUI 合并而改写。

Profile 的 `enabled_source_ids` 仅保存来源引用，绝不复制节点。旧文件没有此字段按全部来源兼容，新建标准本地模板设为空集合；显式禁用来源不影响其它 Profile 或当前运行文件。保存目标若超出该 Profile 的来源范围，下一次连接给出明确错误而不自动改绑。标准模板一次性复制 15 个模块的可编辑示例与说明，只有可直接启动的基础模块默认启用；未登记 core 的方案允许保存但不能连接。

程序 Appearance/Editor/General 设置只在数据库中保存，编辑器 argv 在 Manager 主机运行并优先于该主机 VISUAL/EDITOR。Core 页面复用既有登记、安装和 Profile 核心选择 RPC，不创建第二个生命周期 owner。Nodes 的 Local 表单调用现有节点保存接口；Groups 分类调用现有组绑定接口，并在 Modules → routing 提供同一绑定的只读入口，不创建第二份组数据。

| 页面 | 至少可做的真实动作 | 保存位置/调用 |
|---|---|---|
| 节点 | 查找、详情、复制、编辑、启停、选用并连接 | node.* / connection.connect |
| 订阅 | 加/改/删、更新、查看部分结果 | subscription.* |
| 原生模块 | 15项列表、included、原文编辑、当期常用表单、预览 | module.* / config.* |
| 入/出站子视图 | 原生列表操作、sniffing/streamSettings/mux所属对象编辑 | module.form_edit或node.replace |
| 组 | 成员筛选、模式、手选值、选择组连接 | group.* / connection.* |
| 总览 | 期望与实际、当前Profile、核心、待应用、连接/停止 | runtime.status / core.* |
| Profile | 可读列表、创建、复制、重命名、删除、上下文选择 | profile.list/create/clone/rename/remove |
| TUI session | Viewing Profile、焦点/搜索/选中ID、ModuleDraft | 仅本地交互状态；业务事实从 RPC 读取 |
| 日志/统计 | 查看、暂停滚动、来源标签、未知/过期提示 | logs/read/events、runtime.stats |

原生模块“可高级编辑”和“已有专用表单”分别标记。M1全部模块通用入口必须可用；不能用假按钮冒充M3/M4专用编辑完成。

Enter查看，连接是明确动作；输入框字符不触发全局快捷键；q只退出UI；编辑返回保存/丢弃/返回；列表刷新保持ID与滚动；80×24不硬挤多栏。外部编辑器用argv启动，支持editor_argv，不使用shell命令替换。`xrm tui` 不要求 Profile ID；零 Profile 时可操作 Overview/Subscriptions/Nodes/Cores/Profiles，Profile 专属页面显示创建引导；一个 Profile 只自动成为 viewed context，多个 Profile 恢复有效 context 或弹出选择器。模块正文 `null` 表示源文件不存在，Manager 显示未配置草稿而不写盘或启用；disabled 不妨碍 Enter 打开。

CLI错误/进度写stderr；JSON结果写stdout，不混色彩。采用退出码：0完成或按要求accepted；1失败；2参数；3Server/协议不可用；4未找到；5busy/conflict；6partial；7unknown；130中断等待。中断等待不等于取消Server已接受写操作。

## IP-13　异步、所有权与关停顺序

1. Server组合根持有Store、ProfileService、ProcessService、HttpClient、SubscriptionService、RuntimeWriter、CoreManager、ApiRouter/RpcServer。具体成员/unique_ptr与构造顺序决定生命周期，不用服务定位器。
2. 一个loop串行提交状态；HTTP/parse/assemble大工作可使用libuv工作池，完成后回loop。工作项拥有自己的数据副本；不从worker访问Store。
3. 所有异步完成都通过约定Completion一次交付。取消不销毁仍被worker访问的对象；不捕获请求栈局部变量引用。
4. stop优先与关停不需要新线程/Actor；在CoreManager操作上下文里置取消标志，worker完成时按标志清理。
5. Server shutdown：停止新IPC写入和订阅timer → 取消HTTP/等待已接受工作 → CoreManager停止生产及检查/API子进程 → 排空uv回调/关闭socket → 销毁业务对象与Store。
6. 正式systemd部署使用control-group清理子进程。前台调试被强杀后的孤儿进程保证依赖部署方式；不为此增加第三个watchdog。
7. UI通信线程只将数据事件投递给UI线程，不从网络回调修改FTXUI组件树。终端退出恢复状态，后台服务继续运行。

构造和关停必须有测试；否则接口“看起来解耦”但异步悬空仍会破坏稳定性。测试注入只放在I/O/时钟入口；纯函数直接测，不为每个类提供IMock接口。

## IP-14　必须配套的接口契约测试

细节见 testing-standard.md，下表保证每个跨Agent边界有可独立调用的验收入口。

| 契约 | 主要文件/函数 | 测试族 | 不能只测什么 |
|---|---|---|---|
| IF-00 | common、wire错误字符串 | CT-01、CT-02 | 只测编译成功 |
| IF-01 | Node序列化、Codec | CT-02、DB-01、CODEC-01～08 | 只测试解析数量 |
| IF-02/03 | Profile、15项catalog | MOD-01～08、BIND-01 | 只测DNS |
| IF-04 | Store、文件保存 | DB-01～03、DB-05、MOD-02、MOD-04 | 用map模拟SQLite就当持久化完成 |
| IF-05 | forms、跨模块保存 | FORM-01～07 | 删除未知字段后只比较已知字段 |
| IF-06 | fetch/parse/reconcile/refresh | HTTP-01～03、SUB-01～06 | 请求200就等于成功 |
| IF-07/08 | assemble/FileSet/materialize | ASM-01～08、GROUP-01～05、WRITE-01～04 | 生成一个大JSON再切文件冒充设计 |
| IF-09/10 | Process/CoreManager | PROC-01～04、LIFE-01～11、RC-01～03 | fake-core代替真实核心 |
| IF-11 | API/Metrics/Installer | CTRL-01～02、MET-01～02、LOG-01～02、INS-01～03 | 缺数据统一补零 |
| IF-12/13 | Application、IPC | RPC-01～04、E2E-01～06 | 只测handler函数不经socket |
| IF-14 | CLI/TUI actions | CLI-01～03、UI-01～05 | 只测菜单能画出 |

## IP-15　局部决定与不确定项如何处理

本文件将下列空白确定为实施细节：异步接受/完成语义、手选组范围规则、字节级编辑冲突、连接部分成功表达、CLI退出码、CoreManager直接接受值快照、CMake少量内部目标。这些是XRM设计决定，不冒充上游行为。

仍依赖外部实现验证的部分：具体新协议URI字段、核心对特定观测/策略组合的行为、Metrics/CLI JSON具体形状、特权入站平台条件。对应任务必须读取固定标签文档/源码并建立真实核心测试；未验证时明确标注，不用推测填补。若确实与冻结需求冲突，停止受影响功能并提交具体差异；无关模块可继续。

**禁止自行扩大：** 全量Xray语义模型、运行时任务DAG、任意模板/脚本、所有字段动态表单、跨版本转换器、持久事件回放、全局文件哈希树、自动改系统网络。真实局部问题用局部检查与测试处理。

## IP-16　阅读、实施与核查依据

### 本包来源

- **[B-R]** [requirements.md](requirements.md)：已确认原生模块方案，尤其R02–R16、T01–T37、M0–M5。
- **[B-A]** [architecture.md](architecture.md)：节点/模块/绑定分工、FileSet、运行目录与Server协作。
- **[B-G]** [AGENTS.md](../AGENTS.md)：执行范围、无libXray、不过度工程、测试真实性。

基线原文未改。本文件是其工程细化，不把旧版纯confdir启动器或单JSON生成器带回项目。

### 外部接口的补充核对（非产品需求来源）

| 引用 | 一手资料 | 用途 |
|---|---|---|
| X-MODULES | [Xray配置总览](https://xtls.github.io/config/) | 原生根项与层级 |
| X-MULTI | [官方完整文档的多文件章节](https://xtls.github.io/llms-full.txt) · [多文件](https://xtls.github.io/config/features/multiple.html) | confdir与tail；不自研合并 |
| X-ROUTING | [原生Routing](https://xtls.github.io/config/routing.html) | selector/规则/策略；不照搬Mihomo |
| X-RUN | [v26.9.9 run.go](https://raw.githubusercontent.com/XTLS/Xray-core/v26.9.9/main/run.go) | test与实际启动分开，argv基线 |
| X-CONTROL | [v26.9.9 balancer_override.go](https://raw.githubusercontent.com/XTLS/Xray-core/v26.9.9/main/commands/all/api/balancer_override.go) · [balancer_info.go](https://raw.githubusercontent.com/XTLS/Xray-core/v26.9.9/main/commands/all/api/balancer_info.go) | API命令集中适配 |
| X-CODEC | [官方完整文档](https://xtls.github.io/llms-full.txt) · 基线架构§15资料 | Codec字段与原生位置；实现逐项建立fixture |
| X-STATS | [Stats](https://xtls.github.io/config/stats.html) · [Policy](https://xtls.github.io/config/policy.html) | 显式模块联动 |
| X-METRICS | [Metrics](https://xtls.github.io/config/metrics.html) | 读取核心事实，不伪造指标 |
| X-UV | [libuv Process](https://docs.libuv.org/en/v1.x/process.html) | 进程/exit/句柄生命周期 |
| X-JSON | [nlohmann::json parse](https://json.nlohmann.me/api/basic_json/parse/) | 需要DOM时采用现成注释解析，不正则改原文 |

研究接口沿用v26.9.9作为确定参照，不声称它永远最新；实现时记录实际核心和依赖版本。本文只完成了资料和设计核对，不代表任何代码、协议或性能已经通过测试。

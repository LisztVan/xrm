# XRM 首次使用与部署

XRM 由 `xrm` Manager、`xrmd` Server 和独立的 Xray-core 组成。Manager 通过 Unix socket 与一个 Server 交互；Server 独占它启动的 Xray 子进程。本指南按普通用户模式说明首用流程，最后单列可选 systemd 系统服务部署。

从源码使用时，先按 [构建说明](build.md) 编译；开发版可执行文件位于 `out/dev/`。以下命令假设 `xrm` 和 `xrmd` 已加入 `PATH`，或从该目录调用。

## 普通用户模式

推荐先以普通用户运行，TUN/TPROXY 等需要额外权限的功能不是启动 XRM 的前提。

```sh
xrmd --foreground
```

在另一个终端可直接运行 `xrm tui`，无需预先创建 Profile 或提供 UUID；也可运行 `xrm` 查看实际命令树的根级帮助；`xrm server`、`xrm profile` 等分组显示该层帮助，任意命令的 `--help` 展示参数。帮助不连接 daemon。默认数据库位于 `$XDG_STATE_HOME/xrm/server.sqlite`（未设置时为 `~/.local/state/xrm/server.sqlite`），默认 socket 位于 `$XDG_RUNTIME_DIR/xrm/xrmd.sock`；没有 `XDG_RUNTIME_DIR` 时，socket 回退为 `/tmp/xrmd-<uid>.sock`。数据库所在目录同时作为 Profile、运行文件、托管核心和资源的 data root。也可以显式传入 `--database` 与 `--socket`，便于隔离实例：

```sh
mkdir -p "$HOME/.local/state/xrm" "$HOME/.cache/xrm/run"
xrmd --foreground \
  --database "$HOME/.local/state/xrm/server.sqlite" \
  --socket "$HOME/.cache/xrm/run/xrmd.sock"
xrm --socket "$HOME/.cache/xrm/run/xrmd.sock" server info
```

`xrmd` 是前台进程，建议由终端会话管理器或用户级服务管理器负责其生命周期；不要把它放到无人管理的后台。正常收到 SIGINT/SIGTERM 时，Server 会停止自己管理的核心。不要使用 `pkill xray`。

## 首次设置和连接

以下命令中的 `<id>` 都是前一步命令实际返回的 ID。订阅示例 URL 应替换成你有权使用的地址；普通测试不要请求真实订阅。

1. 登记已有核心，或显式安装官方发布版本：

   ```sh
   xrm core register /absolute/path/to/xray --name local-xray
   xrm core releases
   xrm core install v26.9.9
   xrm core list
   ```

   `core install` 只安装到 XRM 私有 cores 目录，不会选用或启动它。登记系统核心同样只记录路径，不会覆盖该文件。

2. 添加订阅并刷新节点库存：

   ```sh
   xrm sub add --name personal --url 'https://example.invalid/subscription'
   xrm sub list
   xrm sub update <exact-source-name-or-id>
   xrm node list
   ```

   刷新只更新库存与来源状态，不修改 modules、不生成 runtime confdir，也不会重启核心。需要订阅代理时必须显式指定 `--proxy`；代理失败不会回退直连。
   `sub show/edit/update/remove` 接受来源的精确唯一名称或 ID；重名时使用 `sub list` 中的 ID。更新全部失败时人读诊断写 stderr；部分成功的结果留在 stdout、失败原因写 stderr，退出码为 4。`--json` 保留完整聚合结果在 stdout。

3. 创建 Profile：

   ```sh
   xrm profile create --name home --core <core-id>
   xrm profile list
   xrm profile show <profile-id>
   xrm profile sources set <profile-id> --source <source-id>
   xrm config modules <profile-id>
   ```

   新 Profile 先不绑定全局来源；`profile sources set` 用明确的来源 ID 列表替换当前绑定，不传 `--source` 表示不使用任何订阅。旧 Profile 若没有此字段，继续沿用“使用全部来源”的兼容语义。Profile 可暂不选核心而保存，但 Connect 前必须在 Config → Core 或 CLI 中指定可用核心。CLI `profile create` 目前使用标准本地样例；TUI 创建时在同一表单选择 Local SOCKS proxy、TUN 或 Custom。Local SOCKS proxy 默认启用 log、dns、inbounds、outbounds、routing，其余模块有可编辑的禁用样例；Custom 的原生模块初始不启用。TUN 创建的是待检查的原生入站草稿，不会仅因创建方案就接管系统网络。

4. 按需配置原生模块。模块正文仍是 Xray 原生 JSON，未知字段通过原生编辑保留；`config show --source` 查看已保存设置，`config show --runtime` 查看当前活动运行文件：

   ```sh
   xrm config show <profile-id> routing --source source
   xrm config edit <profile-id> routing --file ./routing.json
   xrm config enable <profile-id> stats
   xrm config render <profile-id>
   xrm config check <profile-id>
   ```

   仅在明确保存后，设置才会进入之后的组装。`render` 是预览，`check` 使用当前已保存配置检查核心，不会启动它。
   `config edit` 也接受 `--file -` 从 stdin 读取，原有 `--text JSON_TEXT` 保持可用；两种输入互斥。文件或 JSON 有误时不会发送保存请求。

5. 可选创建手选/自动策略组，或直接选择单节点。手选组须从候选节点中保存成员选择：

   ```sh
   xrm group add <profile-id> --name preferred --tag preferred \
     --mode manual --strategy roundRobin --members <node-id-1>,<node-id-2>
   xrm group list <profile-id>
   xrm group select <profile-id> <group-id> <node-id>
   xrm select group <profile-id> <group-id>
   # 或：xrm select node <profile-id> <node-id>
   ```

6. 明确连接所选目标：

   ```sh
   xrm connect <profile-id>
   xrm status
   xrm stats
   xrm logs
   ```

   保存的目标与实际运行目标分别显示。运行配置是 Xray 原生多文件 confdir；XRM 不以单个合成 `config.json` 作为运行输入。
   `xrm stats --watch | head` 在下游关闭 stdout 时安静结束；xrmd 断线或其他输出写入失败仍返回非零。默认错误只显示任务、原因和恢复方式；需要原始诊断时显式加 `--verbose`。`--json` 的错误仍按原格式写 stderr。

## 常见运行操作

```sh
xrm core restart
xrm core stop
xrm disconnect
xrm status
xrm logs
xrm stats --watch
xrm config show <profile-id> --source source
xrm config show <profile-id> --source runtime
xrm config export <profile-id> --source runtime --output ./exported-confdir
```

`disconnect`/`core stop` 是显式停止。退出 TUI 不会停止核心。导出运行目录后，可在相同 Xray 二进制、工作目录、环境变量和外部资源路径条件下独立运行；导出不会打包证书/私钥等外部资源。

无 Profile 的 TUI 首用：从 `xrm tui` 启动，在顶层用 Tab 选 Subscriptions、Enter 进入内容，按 `a` 在同一多字段表单填写名称和订阅 URL、`u` 更新库存；回顶层选 Config、Enter 进入二级导航，在 Core 按 Enter 进入内容，再按 `a` 登记已有 Xray；回顶层选 Profiles、Enter 进入内容，按 `n` 在同一表单选择 Local SOCKS proxy 模板并填写名称；回 Subscriptions 选中来源按 `b`，明确将该来源用于当前 Profile；进入 Nodes 的二级分类和内容，选择节点并按 `s` 保存目标，或按 `c` 保存光标节点并连接。订阅可先于 Profile 管理；保存目标不自动连接。唯一 Profile 会自动进入浏览上下文；切换浏览不影响正在运行的 Profile。

顶层固定 `Overview / Profiles / Modules / Subscriptions / Nodes / Logs / Config` 七页。两级 Tab 都在顶端；`Tab` / `Shift-Tab` 只移动**当前焦点层**，`Enter` 从顶层进入二级导航、再进入内容，`Esc` 退一层。顶层没有数字直达或方向键切页；`?` 或 F1 看当前焦点帮助。文本输入和弹窗只接收自己的按键。只有焦点在顶层时 `q` 退出 TUI，且不停止核心。Profiles 内容区 `Enter` 查看并更新底部详情；`u` 切换 Use，再对同一 Profile 按 `u` 取消 Use，不停止运行；`e` 开启/关闭该 Profile 的模块 Edit，开启时将 View 设为该 Profile，不改变 Use 或 Run。`R` 改名、`m` 管理元数据、`n` 新建、`c` 复制、`D` 删除，这些 Profile 管理不要求开启 Edit。关闭或切换 Edit 时先处理未保存草稿；保存失败保留目标和输入。Subscriptions 内容区 `e` 编辑名称、URL 与自动刷新，`t` 暂停/恢复自动刷新，`D` 预览引用和节点处置后移除。Nodes 的二级分类包括全部订阅、各来源、Local、Groups；内容区上下键、PageUp/PageDown、Home/End 浏览。Config 二级页为 Core、Geodata、Tests、Appearance、Editor、General。`N` 查看有界通知历史；Nodes `T` 查看所选节点测试结果，`k` 请求取消本会话的活动测试。80×24 是正常布局下限，缩得更小时显示保护提示，放大后恢复草稿与选择。

Modules 顶部第二行是 15 个原生模块 key：在二级导航按 Tab 切模块，Enter 进入内容。F2 在同一全宽工作区切 Visual/JSON；`v` 切 Source/Runtime；`i` 查看源和运行路径。Visual 按原生语义显示 DNS servers/hosts、routing rules/balancers、Policy levels/system 等分区，Enter 打开对应表单或集合。集合中 Enter 只读查看，Add/Edit 使用相同多字段表单，Copy/Delete/Up/Down 管理有序对象；DNS 服务器可显式选择原生字符串或对象表示，规则目标用当前 Profile 对象选择器。

子表单的 **Add to draft** 只更新同一模块草稿。返回模块内容后 Ctrl-S 保存目标源文件；保存不 Connect/Apply。只有 **Edit 对象与 View 相同且为 Source** 时，字段、集合、JSON、Include（`I`）、预设、外部编辑器和保存才可写。Runtime 永远只读；停用或不存在 Source 的模块仍可查看，打开不会启用或生成文件。F6 打开外部编辑器。无效 JSON 保留原文并阻止切回 Visual，修正后可往返；未知字段、已有原生类型、数组顺序保留。Visual 实际修改会规范化当前模块文本，不承诺保留注释排版。专用字段与高级 JSON 边界见 [模块表单覆盖矩阵](module-form-coverage.md)。

表单中的 Tab/Shift-Tab 在当前控件间切换，方向键、Home/End 和删除键由输入控件处理，多行 Enter 是换行。F1 在表单中同样打开 Help 并保留输入焦点；文本中的 `?` 是字符。所有常驻快捷键长条已移入 Help。当前对象说明、路径和模式在底部信息区；临时成功/失败反馈在右上角浮出并自动消失，错误仍可在相关表单或通知历史中查看。

Overview 显示实际运行、保存目标和 Server 统计；没有采样或重启后的间隔明确显示不可用，不画成零流量。Logs 页按 `p` 暂停/恢复自动跟随；暂停只影响本地视口，Server 仍继续接收有界日志，可用上下键查看历史。若缓冲淘汰了旧日志，页面会提示 `older logs dropped / truncated`。

命令行的程序偏好独立于 Xray 原生配置：`xrm settings show` 查看，`xrm settings set theme dark` 修改外观，`xrm settings editor set '["/usr/bin/vi"]'` 设置外部编辑器。脚本需要机器可读输出时使用现有 `--json`，不会混入终端配色。核心资源仍用 `xrm core ...`，原生模块仍用 `xrm config ...`；这三类设置不会相互覆盖。

新订阅的**完整成功**刷新会按来源内连接身份去重，并把已消失条目从当前节点库存真实移除；下载失败、部分解析失败不据缺席删除，Local 节点不受订阅刷新影响。订阅响应中的明确说明项可在来源详情中查看，不按“可连接节点”累计。若旧库存仍有历史重复项，可在 Subscriptions 页选中来源按 `h` 预览候选；在多选表单中勾选可清理候选，审阅后通过 Remove selected 明确提交，Esc 取消。CLI 仍有 `xrm node history preview SOURCE_ID` 和带预览 token 的 `xrm node history confirm`。这是针对旧库存的审阅入口，不应把新订阅的正常刷新当成永久保留 `source_missing` 的机制。不要在生产数据上执行未经查看的批量清理。

移除节点不会为旧运行实例即时改配置：已经启动的 Xray 使用其不可变多文件运行目录和 RunRecord，当前 PID/文件不因订阅更新变化。Overview 仍可解释正在使用的旧目标；下一次明确 Connect/Apply 从最新库存重新组装。已删除节点不再作为新连接候选；保存的选择若引用失效，会明确报错而非自动直连或改绑。

### 显式节点测试

在 Nodes 内容区高亮一个节点按 `t`，在多字段表单的测试类型选择器中选择 ICMP、TCP、真实代理 URL 或限量下载，并按需填写 URL/限额。CLI 使用相同 Server 测试能力，例如：

```sh
xrm node test <node-id> --kind icmp
xrm node test <node-id> --kind tcp
xrm node test <node-id> --kind url --profile home --url 'http://127.0.0.1:<test-port>/marker'
xrm node test <node-id> --kind download --profile home --url 'http://127.0.0.1:<test-port>/bytes' --max-bytes 1048576
xrm operation get <returned-operation-id>
xrm node test-results
xrm node test-cancel <operation-id>
```

ICMP 只测节点主机的回声响应；本机没有 `ping` 或缺权限时会如实报告，不代表节点离线。TCP 只测地址/端口建连，纯 UDP/QUIC 不适用；TCP 成功**不证明代理认证正确**。URL/下载测试使用所选 Profile 的核心另起受控临时 Xray，测试请求走其 loopback SOCKS 入口；它们不会切换生产出口，也不会回退直接访问目标。下载还受正文大小和超时约束，不是自动批量测所有节点。目标 URL 优先使用本次 `--url`，否则使用当前 Profile 已启用观测模块的目标，再否则使用 Config → Tests 的默认 URL；`burstObservatory.pingConfig.connectivity` 不是代理测试目标。Observatory 本身是后台 HTTPing，不是 ICMP、下载或即时测试 RPC。测试结果记录类型、目标、采样时间和成功/失败/不适用/取消；节点参数后来变化时旧结果会标记过期。测试需要真实流量、可能消耗订阅额度；本指南中的 loopback URL 只是本机测试示例，绝不自动测试你的真实订阅。

### Geodata 与分流预设

共享资源在 `Config → Geodata` 管理：`a` 登记 GeoIP/GeoSite 的 URL 或本地 `.dat`，`u` 下载/校验并形成缓存版本；通过已登记的 Xray 验证后，`b` 才把所选文件绑定到**当前浏览的 Profile**。`e` 修改资源及自动更新设置，`D` 只移除未被绑定的资源元信息。Profile 绑定的是共享缓存的资源版本，不复制订阅节点；缓存更新不会改当前运行 FileSet，明确 Connect/Apply 才取得新版本。原生 `modules/geodata` 与进程启动前资源路径是不同层，不能用编辑 Xray `env` 模块代替资产绑定。资源缺失、类型或预设需要的标签不匹配时，不要把配置检查通过误当分流已经生效。

对应的命令式入口可用于脚本。名称必须精确且唯一，也可直接使用 ID；`add` 仅登记，`update` 才下载/校验，`bind` 需要已验证版本。`--json` 输出不含颜色或界面装饰。

```sh
xrm geodata list
xrm geodata add My-GeoIP geoip geoip.dat --url 'https://example.invalid/geoip.dat'
# 或：xrm geodata add Local-GeoIP geoip geoip.dat --file /absolute/path/geoip.dat
xrm geodata update My-GeoIP --core local-xray
xrm geodata bind home My-GeoIP
xrm geodata remove My-GeoIP  # 仅限未被 Profile 引用的资源
```

在当前 Profile 的 Modules 顶层按 Enter 进入二级导航，用 Tab 选 `routing`，再按 Enter 进入 Source 内容，先为当前 View Profile 开启模块 Edit，再按 `p` 从选择器选择“绕过大陆 / 黑名单代理 / Global / 全直连 / 自定义原生”。先看弹窗中的依赖、**当前原生 routing JSON 与拟保存 JSON 的差异**；Save routing source 才保存到该 Profile 的 routing 源模块，Esc 取消。TopNav、SecondaryNav、其他模块和 Runtime 预览都不会触发该操作。前几种预设需要相应已验证的 GeoIP/GeoSite 标签及可用的出口；缺资源时应先回 Geodata 处理。重复选择相同未改动预设不会叠加规则。保存只改变源文件，必须再明确 Connect/Apply 才改变当前 Xray；已有手写未知字段与静态 balancer 不应被预设覆盖。Global 的名称也不表示毫无例外：其局域网和 UDP/443 等行为以预览中的原生规则为准。

CLI 的对应操作是先预览，再明确保存。`plan` 返回 `before`、`after`、依赖和精确原文基线 `expected_text`；审阅结果后把基线存入文件，`save --expected-file` 会在源文件已变化时拒绝，不会静默重做一份新计划：

```sh
xrm --json routing preset plan home bypass_mainland > /tmp/xrm-routing-plan.json
# 审阅 /tmp/xrm-routing-plan.json 中的 before、after 和所需资源
jq -j .expected_text /tmp/xrm-routing-plan.json > /tmp/xrm-routing-base.json
xrm routing preset save home bypass_mainland --expected-file /tmp/xrm-routing-base.json
```

`jq -j` 不追加换行，避免改变基线字节。可用预设标识为 `bypass_mainland`、`blacklist_proxy`、`global_proxy`、`direct_only`、`custom_native`。保存只修改原生 `routing` 源模块，不是运行配置应用动作；之后根据需要显式 `xrm config apply home` 或 `xrm connect home`。

分流行为参考 [v2rayN 固定提交 e1cb99c](https://github.com/2dust/v2rayN/tree/e1cb99cd6ec1c134ea8260814d2a27e8b0ab862e) 的初始化与预设语义；XRM 独立生成原生规则，未复制其源码或样例文件。v2rayN 的许可证为 [GPL-3.0](https://raw.githubusercontent.com/2dust/v2rayN/e1cb99cd6ec1c134ea8260814d2a27e8b0ab862e/LICENSE)。用户自行选择 Geodata 来源，须遵守该资源自身的许可证与更新约束。

已有 Profile 若没有本地代理入口，在 Overview 按 `i` 进入所查看 Profile 的 `inbounds` 模块；Modules 的 `i` 显示完整 Source/Runtime 路径。先为该 Profile 开启模块 Edit，然后用入站集合的 Add 表单设置 tag、protocol、listen、port；协议高级设置位于该连接的 settings 分区。查看与新建草稿都不会自动启用、保存或启动，原本禁用的 `inbounds` 仍需显式 Include，保存后再明确 Connect/Apply。选择 loopback 监听与本次可用端口；XRM 不修改系统代理、DNS 或路由。

在 Modules 内容区按 F2 在 Visual/JSON 间切换；当前 Profile 的 Source 已开启 Edit 时，按 F6 可打开外部编辑器。顺序是 `Config → Editor` 中保存的 argv、Manager 主机的 `$VISUAL`、`$EDITOR`；程序直接 spawn，不经 shell。编辑器只改临时草稿，返回 TUI 后仍须按 Ctrl-S 才保存模块源文件；保存不 connect/restart。未配置时会提示；非零退出保留草稿；源文件冲突也保留草稿，不覆盖外部修改。人工验收步骤见 [tui-manual-acceptance.md](tui-manual-acceptance.md)。

切换核心是显式动作：

```sh
xrm core install v26.9.9
xrm core use <profile-id> v26.9.9
xrm core list
```

安装不等于 use，use 也不会自动应用未运行的源配置。运行过程中 use 按当前生命周期语义用实际运行文件测试所选核心。

常见错误先检查 `xrm server info`、`xrm status`、`xrm logs`、`xrm core list` 和 `xrm profile show <profile-id>`。配置检查失败不会停止旧实例；新核心启动失败时管理器最多恢复旧运行输入一次。订阅下载失败、HTML 页面或无法识别正文不会清空旧节点。若所选来源节点已缺失，XRM 会报告选择失效，不会静默改绑或直连。

## 可选：systemd 系统 daemon

systemd unit 位于 `packaging/systemd/xrmd.service`，面向发行包将二进制安装到 `/usr/bin/xrmd` 的 Linux 系统。系统模式使用 `/var/lib/xrm/server.sqlite`（StateDirectory `/var/lib/xrm`）和 `/run/xrm/xrmd.sock`（RuntimeDirectory `/run/xrm`）；Profile、运行文件、托管核心和资源均相对数据库 data root 保存。服务账号 `xrm` 由部署者创建，服务以该账号运行；RPC socket 当前为 daemon UID 私有，系统模式下 Manager 也应使用该服务身份。通常桌面/单用户使用更适合普通用户模式，不需要 root。

示例安装步骤需要部署者显式执行，unit 安装本身不会 enable 或 start 服务：

```sh
sudo useradd --system --user-group --home-dir /var/lib/xrm --shell /usr/sbin/nologin xrm
sudo install -D -m 0644 packaging/systemd/xrmd.service /etc/systemd/system/xrmd.service
sudo systemctl daemon-reload
sudo systemctl enable xrmd.service
sudo systemctl start xrmd.service
sudo systemctl status xrmd.service
```

由于 RPC socket 对 daemon UID 私有，Manager 必须以服务账号运行并显式指定 socket，例如 `sudo -u xrm /usr/bin/xrm --socket /run/xrm/xrmd.sock status`。该系统部署模式不要求日常用户都以 root 身份运行 XRM；单用户场景应优先使用前面的普通用户模式。

`enable` 只配置后续启动时启用，`start` 才启动本次服务；可分别执行 `disable` 和 `stop`。升级 unit 文件后需 `daemon-reload`，部署者决定何时 restart。unit 只启动 `xrmd`；`KillMode=control-group` 让 systemd 在停止服务时清理这个服务 cgroup 中的 Server 与其 Xray 子进程，不存在并行管理同一核心的 `xray.service`。

TUN/TPROXY 等特权配置不是默认 systemd 能力。部署者应先确认配置确实需要特权，再自行选择服务身份或为服务配置最小必要 capability；XRM 不自动 sudo、setcap、改防火墙、路由、NetworkManager 或宿主 DNS。systemd unit 的静态检查与普通进程归属测试不能替代隔离 systemd 实机启动/停止测试；当前验收环境未运行隔离 systemd 实机测试。

组件重写的隔离体验入口、屏幕与验证记录见 [执行记录](component-ui-rewrite-execution.md)。

### Visual polish

TUI 默认使用 XRM Slate 深色主题，Config → Appearance 可选 XRM Paper 或 Terminal palette。字体由终端提供；可选的 Maple Mono NF CN 独立配置见 [视觉系统](tui-visual-design.md)。七页三尺寸的真实当前格、截图与逐页自评见 [证据索引](evidence/visual-polish/README.md)。

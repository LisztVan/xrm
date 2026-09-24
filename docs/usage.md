# XRM 首次使用与部署

XRM 由 `xrm` Manager、`xrmd` Server 和独立的 Xray-core 组成。Manager 通过 Unix socket 与一个 Server 交互；Server 独占它启动的 Xray 子进程。本指南按普通用户模式说明首用流程，最后单列可选 systemd 系统服务部署。

## 普通用户模式

推荐先以普通用户运行，TUN/TPROXY 等需要额外权限的功能不是启动 XRM 的前提。

```sh
xrmd --foreground
```

在另一个终端运行 `xrm`。默认数据库位于 `$XDG_STATE_HOME/xrm/server.sqlite`（未设置时为 `~/.local/state/xrm/server.sqlite`），默认 socket 位于 `$XDG_RUNTIME_DIR/xrm/xrmd.sock`；没有 `XDG_RUNTIME_DIR` 时，socket 回退为 `/tmp/xrmd-<uid>.sock`。数据库所在目录同时作为 Profile、运行文件、托管核心和资源的 data root。也可以显式传入 `--database` 与 `--socket`，便于隔离实例：

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
   xrm sub update <source-id>
   xrm node list
   ```

   刷新只更新库存与来源状态，不修改 modules、不生成 runtime confdir，也不会重启核心。需要订阅代理时必须显式指定 `--proxy`；代理失败不会回退直连。

3. 创建 Profile：

   ```sh
   xrm profile create --name home --core <core-id>
   xrm profile list
   xrm profile show <profile-id>
   xrm config modules <profile-id>
   ```

4. 按需配置原生模块。模块正文仍是 Xray 原生 JSON，未知字段通过原生编辑保留；`config show --source` 查看已保存设置，`config show --runtime` 查看当前活动运行文件：

   ```sh
   xrm config show <profile-id> routing --source source
   xrm config edit <profile-id> routing --text '{"routing":{"rules":[]}}'
   xrm config enable <profile-id> stats
   xrm config render <profile-id>
   xrm config check <profile-id>
   ```

   仅在明确保存后，设置才会进入之后的组装。`render` 是预览，`check` 使用当前已保存配置检查核心，不会启动它。

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

TUI 使用 Tab / Shift-Tab 或 `1`–`8` 切换页面，`?` 查看按键帮助，`q` 仅退出界面。Logs 页按 `p` 暂停/恢复自动跟随；暂停只影响本地视口，Server 仍继续接收有界日志，可用上下键查看已收到的历史。若缓冲淘汰了旧日志，页面会提示 `older logs dropped / truncated`。

在 Native Modules 页按 Enter 打开原生文本草稿，按 `v` 可用 `$VISUAL`（优先）或 `$EDITOR` 打开外部编辑器。变量须指定 executable 和可选的简单、未加引号参数；程序直接 spawn，不经 shell。编辑器只改临时草稿，返回 TUI 后仍须按 Ctrl-S 才保存模块源文件；保存不 connect/restart。缺少两个变量会明确报错。编辑器非零退出时保留 TUI 草稿；源文件在编辑期间被别的客户端修改时，保存会报告冲突并保留草稿，不覆盖外部修改。人工验收步骤见 [tui-manual-acceptance.md](tui-manual-acceptance.md)。

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

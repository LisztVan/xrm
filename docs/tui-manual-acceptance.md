# TUI 人工验收清单（G5）

状态：**待用户按新七页工作流复验；开发者 PTY/真实核心验收不能代签 G5。**

建议用隔离数据目录、本机订阅 fixture 和固定 Xray v26.9.9，在 80×24 终端完成。不要使用生产订阅做破坏性测试，也不要修改宿主 DNS、路由或系统代理。先启动 `./build/dev/xrmd --foreground`，再运行 `./build/dev/xrm tui`；若已有同一用户的 daemon，不要启动第二份。TUI 不需要 Profile UUID。

顶层按 `1`–`7` 或 Tab/Shift-Tab 切换：Overview、Profiles、Modules、Subscriptions、Nodes、Logs、Config。Nodes 用 `[`/`]` 切换来源、Local、Groups；Config 用 `[`/`]` 切换 Core、Appearance、Editor、General。`?` 显示当前页帮助。

1. [ ] 从空数据目录直接进入 TUI，确认恰好七页，当前页高亮与正文一致；缩到 80×24 后仍看得见当前分类。
2. [ ] Config → Core 登记本机已有 Xray；在 Profiles 创建 Home 并选择该核心。查看 15 个原生模块的标准样例与默认启用摘要；只浏览 Profile 不启动核心。
3. [ ] Modules 打开 disabled 且无 source 的模块，确认有未配置草稿而不自动创建或启用；修改一个模块并保存，确认不自动 Connect/Apply。
4. [ ] Subscriptions 添加 A/B，更新并确认数量；在 Home 只启用 A。切到 Nodes，确认“全部订阅”只含 A，单来源与 Local/Groups 分类可达。重复更新不应凭空增加节点。
5. [ ] 新建 Office，仅启用 B。切换 Viewing Profile 后，Overview 的 Running Profile、PID、实际出口均不变；节点库仍是共享库存。
6. [ ] 回 Home，选择 A 的一个可连接节点并明确 Connect。确认 Overview 显示本地代理入口、保存目标和实际目标；通过配置了该本地代理的测试请求验证真实流量。
7. [ ] Nodes → Local 用表单新增 VLESS，确认协议 UUID 与内部节点 ID 不混淆；保存、选用并连接。再创建 Group（订阅成员或 Local 成员），选用并连接，确认可访问同一本机测试服务。
8. [ ] Modules → routing 能进入同一个动态 Group；原生 routing 规则仍可查看。Overview 显示可用的真实统计；未启用统计时显示 unavailable，而不是假零。顶层没有独立 Stats 页。
9. [ ] Logs 按 `p` 暂停跟随，产生新日志后视口不跳底；上下查看，再按 `p` 恢复。日志被有界缓冲淘汰时应显示 dropped/truncated。
10. [ ] 在 Nodes 搜索框输入 `q`、`e`、空格，确认不会触发全局快捷键；中文、emoji、长名称在滚动和缩窗后仍完整可辨。
11. [ ] Config → Appearance 预览、取消再保存主题；Config → Editor 保存外部编辑器 argv。重启 Manager 后设置仍在，核心 PID、原生模块和运行文件不会因此变化。
12. [ ] Modules 使用外部编辑器打开草稿，正常返回后先确认 source 未变，再明确保存。再测一次编辑器非零退出，确认草稿和终端状态保留；由第二客户端修改同一模块时，保存冲突不可静默覆盖。
13. [ ] Config → Core 检查 installed、Profile selected、running 是不同状态；登记/安装核心不自动切换当前运行核心。不要覆盖系统 Xray。
14. [ ] 按 `q` 退出 TUI，再用 `./build/dev/xrm status` 确认核心仍运行；若使用隔离数据目录，结束后只停止本次自己启动的进程。

人工记录（由实际操作者填写）：

- 操作者 / 日期：
- 终端与尺寸：
- 结果：PASS / FAIL（失败时记录当前页面、按键和可见错误）：
- 额外观察：

# TUI 人工验收清单（G5）

状态：**G5 READY FOR HUMAN RE-VALIDATION — 自动化回归已通过；尚无人工执行记录，不代表 PASS**

目标终端：80×24，真实 CLI / Server / 本机 loopback Xray。预计 10–15 分钟。请使用一次性 Profile 和测试节点，不要使用生产配置。修复后的起点是直接运行 `xrm tui`，不传 Profile UUID；若从仓库运行则用 `build/dev/xrm tui`。

在第一个终端从仓库启动 `./build/dev/xrmd --foreground`（若同一用户的 daemon 已运行，不要再启动第二份）。第二个终端进入仓库，执行 `export VISUAL=nano`、`stty cols 80 rows 24`，再直接运行 `./build/dev/xrm tui`，不带 `--profile`。保留第二个 CLI 会话供冲突检查和最终 `./build/dev/xrm status` 使用。先在 TUI 的 `5 Subs` 页按 `a` 添加你自己的测试订阅，按 `u` 更新；`8 Cores` 页按 `a` 登记已有 Xray 路径；`9 Profiles` 页按 `n` 创建有名字的 Profile。随后在 `2 Nodes` 选择节点按 `s` 保存，在 Overview 按 `c` 连接。不要抄写或手工输入 Profile UUID。若需确认真实 loopback 连接，请登记本机已有 Xray；勿使用公网节点或修改系统网络。

依次执行并记录结果：

1. [ ] 直接 `xrm tui` 启动（无 `--profile`）；确认能打开、提供首用说明，并在有唯一 Profile 时显示可读 Profile 名称。Overview 显示 saved target、actual target、PID/runtime 状态。
2. [ ] 按 `2` 打开 Nodes；确认中文、emoji 和长节点名没有被 UTF-8 字节截断。
3. [ ] 用 Tab / Shift-Tab 在 Overview、Nodes、Groups、Native Modules、Logs、Stats 间切换；再用数字快捷键直达页面，全程仅用键盘。
4. [ ] 在 Nodes 按 `/` 搜索并选择一项；等待后台刷新，确认焦点和当前节点 ID 没跳到其他项。
5. [ ] 在搜索输入框输入 `q`、`e` 和空格；确认它们只是输入内容，不触发退出或模块动作；按 Esc 退出搜索。
6. [ ] 在第二个 CLI 会话修改节点库存并在 TUI 刷新；确认当前 ID 保持。若手边有故障注入环境，可另验证 saved target 为 B、actual running target 恢复为 A 时 Overview 同时如实显示二者。
7. [ ] 打开 Logs（`4`），确认新日志到来时 FOLLOW 自动跟随底部；按 `p` 暂停，再通过真实核心产生新日志，确认 viewport 不跳动。
8. [ ] Pause 状态用上下键查看已有历史；如果服务端环形日志有淘汰，确认出现 `older logs dropped / truncated` 提示。
9. [ ] 再按 `p` Resume，确认回到最新位置且之后仍自动跟随。
10. [ ] 打开 Native Modules（`3`），对至少一个 disabled 且无 source 的模块按 Enter；确认无崩溃、显示“Not configured yet”，Esc 后 source 仍不存在且没有自动启用。再选 `log` 模块，Enter 后确认页面明确显示 `F2 Save, F6/v external editor`；按 `F6`（或 `v`）打开外部编辑器。修改 `loglevel`，正常退出；确认回到 TUI 后显示未保存草稿。
11. [ ] 不按 F2/Ctrl-S 前，使用第二个 CLI 查看 source 并确认源文件未变；回到 TUI 按 F2（或 Ctrl-S）保存，确认只保存源模块，没有自动 connect/restart。
12. [ ] 对相同模块重开外部编辑器做一次冲突验证：外部编辑器返回后、TUI Save 前，用第二个 CLI 修改同一源模块；再按 F2/Ctrl-S，确认冲突报错、CLI 修改未被覆盖、草稿保留且 TUI 仍可操作。
13. [ ] 另开一次 TUI 会话，将 `VISUAL` 设为会非零退出的命令（例如 `false`），按 `v`；确认错误清楚显示、草稿保留、terminal 恢复。再使用有效编辑器正常退出，确认输入、绘制和光标正常。
14. [ ] 每次退出 TUI 都按 `q`；随后在 CLI 执行 `xrm status`，确认核心仍运行。退出 TUI 不应 stop core。

人工记录（由操作者填写）：

- 操作者 / 日期：
- 终端与尺寸：
- 结果：PASS / FAIL（若失败请记录复现步骤）：
- 额外观察：

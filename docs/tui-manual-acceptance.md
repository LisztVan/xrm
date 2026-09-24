# TUI 人工验收清单（G5）

状态：**READY FOR MANUAL ACCEPTANCE — 尚未由人工执行，不代表 PASS**

目标终端：80×24，真实 CLI / Server / 本机 loopback Xray。预计 10–15 分钟。请使用一次性 Profile 和测试节点，不要使用生产配置。

准备一个已登记的本机 Xray、一次性 Profile 和至少一个可连接的 loopback 节点；启动 daemon 并让该 Profile 的核心处于 running。运行前设置真实编辑器，例如 `export VISUAL=nano`。在第二个 shell 保留同一 Server 的 CLI 会话。然后执行 `stty cols 80 rows 24` 并启动 `xrm tui --profile <profile-id>`。

依次执行并记录结果：

1. [ ] 在 80×24 窗口启动 TUI；确认 Overview 显示 saved target、actual target、PID/runtime 状态。
2. [ ] 按 `2` 打开 Nodes；确认中文、emoji 和长节点名没有被 UTF-8 字节截断。
3. [ ] 用 Tab / Shift-Tab 在 Overview、Nodes、Groups、Native Modules、Logs、Stats 间切换；再用数字快捷键直达页面，全程仅用键盘。
4. [ ] 在 Nodes 按 `/` 搜索并选择一项；等待后台刷新，确认焦点和当前节点 ID 没跳到其他项。
5. [ ] 在搜索输入框输入 `q`、`e` 和空格；确认它们只是输入内容，不触发退出或模块动作；按 Esc 退出搜索。
6. [ ] 在第二个 CLI 会话修改节点库存并在 TUI 刷新；确认当前 ID 保持。若手边有故障注入环境，可另验证 saved target 为 B、actual running target 恢复为 A 时 Overview 同时如实显示二者。
7. [ ] 打开 Logs（`4`），确认新日志到来时 FOLLOW 自动跟随底部；按 `p` 暂停，再通过真实核心产生新日志，确认 viewport 不跳动。
8. [ ] Pause 状态用上下键查看已有历史；如果服务端环形日志有淘汰，确认出现 `older logs dropped / truncated` 提示。
9. [ ] 再按 `p` Resume，确认回到最新位置且之后仍自动跟随。
10. [ ] 打开 Native Modules（`3`），选择 `log` 模块（必要时用上下键），Enter 后按 `v` 启动外部编辑器。修改 `loglevel`，正常退出编辑器；确认回到 TUI 后显示未保存草稿。
11. [ ] 不按 Ctrl-S 前，使用第二个 CLI 查看 source 并确认源文件未变；回到 TUI 按 Ctrl-S 保存，确认只保存源模块，没有自动 connect/restart。
12. [ ] 对相同模块重开外部编辑器做一次冲突验证：外部编辑器返回后、TUI Save 前，用第二个 CLI 修改同一源模块；再按 Ctrl-S，确认冲突报错、CLI 修改未被覆盖、草稿保留且 TUI 仍可操作。
13. [ ] 另开一次 TUI 会话，将 `VISUAL` 设为会非零退出的命令（例如 `false`），按 `v`；确认错误清楚显示、草稿保留、terminal 恢复。再使用有效编辑器正常退出，确认输入、绘制和光标正常。
14. [ ] 每次退出 TUI 都按 `q`；随后在 CLI 执行 `xrm status`，确认核心仍运行。退出 TUI 不应 stop core。

人工记录（由操作者填写）：

- 操作者 / 日期：
- 终端与尺寸：
- 结果：PASS / FAIL（若失败请记录复现步骤）：
- 额外观察：

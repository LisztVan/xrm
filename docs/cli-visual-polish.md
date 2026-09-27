# CLI visual polish

本轮沿用 TUI 的克制工具语言，但 CLI 保持一次性、可复制的文本输出，不模拟全屏应用。

## 实现

- `src/cli/presentation.hpp`：统一终端列表与详情；名称先于标识，固定两格列间距，无表格外框。用已依赖的 FTXUI 计算 Unicode 显示宽度。长值折行，UUID 不省略。
- `src/cli/print.cpp`、`geodata_commands.cpp`：Profiles、Nodes、Subscriptions、Modules、Groups、Core、Geodata 使用同一表格；Profile/Node/Subscription/Module 详情统一标签对齐。窄屏 Profiles 转为名称与字段记录；Modules 在 80 列将 Source/Runtime 路径上下排列，在 120/180 列分别成列。
- `src/cli/help.hpp`：根帮助按交互入口、保存配置、资源、运行观测、程序设置组织；内容仍取自 CLI11 注册树。示例逐行缩进，描述换行，不在无 Profile 参数的叶帮助重复 Profile 说明，不在每个叶帮助打印版本选项、默认 socket 路径等无关信息。
- TTY 中的配置文件查看展示文件名与原生文本；测试结果展示结果、时延、速率与节点；operation 展示状态与结果摘要。显式 `config dump` 保持原生诊断输出。
- 颜色只增强表头；`NO_COLOR`、`TERM=dumb` 与非 TTY 不产生 ANSI。非 TTY 列表、完整 ID、原有 RPC 输出、`--json` schema、退出码与 stdout/stderr 边界保留。

## 实际观察与修正

真实 PTY 80/120/180 列逐一检查 Profiles、Nodes、Modules；另检查根/分组/叶帮助与空测试结果。第一轮 80 列 Modules 的 Source 路径出现单字符续行，已改为两行路径列，第二轮观察确认 15 个原生模块均清楚区分 Source/Runtime，未截断路径。

窄屏的完整 ID 与高密度不可同时最大化：Profiles 选择可复制的完整 ID；宽屏恢复表格。CLI 长原生 JSON 的换行仍由终端/用户选择的 pager 决定，不改写用户配置文本。

## 验证

```sh
cmake --build build/component-rewrite --target xrm -j 3
python3 tests/cli/cli_rewrite_test.py \
  --xrm build/component-rewrite/xrm \
  --xrmd build/component-rewrite/xrmd \
  --xray /tmp/xrm-g1-xray-v26.9.9/xray \
  --evidence docs/evidence/visual-polish/cli.json
```

测试覆盖真实 CLI 的全部帮助树与示例、解析错误、缺少参数、JSON 错误、pipe TSV、80/120/180 PTY、隔离 Server、保存不运行、真实 Xray 26.9.9 connect/stop、watch flush/Ctrl-C、消费者关闭及 daemon 断开。使用临时 HOME/XDG/socket/database；不使用真实订阅、不设置入口端口、不发起代理流量。

最终执行结果：构建成功；264 个捕获场景通过，含真实核心路径，未以 fake-core 替代。

原始 stdout/stderr、尺寸和退出码见 [CLI 证据](evidence/visual-polish/cli.json)。本轮不将自动验证标成 G5 人工验收通过。

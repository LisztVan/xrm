# 组件与真实终端验证记录

本记录对应组件重写的工程验证；G5 仍待用户人工复验。终端格来自实际 `xrm` / 组件展示程序，不是设计稿。隔离测试仅操作自己创建的 socket、数据库、HOME/XDG 与 tmux 会话。

## 可重复入口

```sh
cmake --build build/component-rewrite --target xrm xrmd xrm_fake_core xrm_tui_components_tests xrm_component_showcase
build/component-rewrite/tests/tui/components/xrm_tui_components_tests
build/component-rewrite/tests/tui/components/xrm_component_showcase
python3 tests/tui/rewrite/terminal_regression.py \
  --build build/component-rewrite \
  --output /tmp/xrm-rewrite-terminal-evidence
```

回归脚本启动独立 daemon、loopback HTTP 订阅 fixture 和专用 tmux socket；结束后关闭自身进程。数据与证据目录保留在脚本输出的 `/tmp/xrm-rewrite-ui-*`，可检查后按该完整路径清理。不得使用 `pkill` 或清理其他 socket。脚本记录二进制摘要、daemon/终端 PID、按键轨迹、当前屏幕格、ANSI 样式、RPC 状态和原生源数据；每个保存边界均检查运行文件没有暗中发布。

脚本的 fake core 仅用于进程与日志行为，不能证明 Xray 配置语义或流量。真实核心结果由执行记录另列，不用此测试替代。

## 组件采用依据与输入边界

已核对项目锁定的 FTXUI `v7.0.1`，实现采用 `Input`、`Checkbox`、`Radiobox`、`Menu`、`Button`、`Container`，组合成 XRM 的表单、集合、树、对话框与日志视口。依据：[FTXUI 官方组件文档](https://arthursonzogni.com/FTXUI/doc/)、[v7.0.1 options](https://github.com/ArthurSonzogni/FTXUI/blob/v7.0.1/include/ftxui/component/component_options.hpp) 与本地锁定源码 `input.cpp` / `container.cpp`。网页文档标示 5.0.0，实际 API 以锁定源码核验，未升级依赖。

FTXUI 7 的 Home/End 默认为文档首尾。本组件只适配光标位置，使 Home/End 到当前行首尾，Ctrl-Home/Ctrl-End 到文档首尾；字符插入、UTF-8 删除、左右与上下移动仍交给 FTXUI。单行上下键由控件消费。Enter 在多行框插入换行，单行框不触发表单提交；保存使用按钮或 Ctrl-S。

本轮没有开发应用内选区、剪贴板、撤销栈或完整 BiDi 引擎。复制由终端自身选择功能完成；粘贴文本经终端输入交给 FTXUI。中文字符和混合文本实际验证；组合字符、emoji 序列遵循 FTXUI 自身的字素边界，阿拉伯文字保持逻辑顺序，显示整形取决于终端。不能把普通 UTF-8 可编辑误写为全 Unicode 排版已验收。

可选布尔使用复选框和 `Use default`，显式区分未设置、false、true。枚举选择器保留未知旧值，并明确显示不受当前选择集合支持。多选维护稳定身份和原数组次序，方向键不修改值。StringList 支持 Add/Update/Delete/Up/Down；`[]` 与单元素空字符串数组无法通过空文本区分，原生表单须保留该类数据并转高级 JSON 编辑，不默默转换。

表单组件将数据与控件稳定持有，字段校验失败定位字段并保留全部输入；服务端失败保留错误与草稿。只读控件仍允许光标和阅读滚动；所有改变值的事件由同一个输入包装禁止。Toast 不在焦点树中，普通/警告/错误分别 3/5/8 秒，最多三个同时显示、三十条内存历史，重复通知合并。

## 已执行的组件验证

`xrm_tui_components_tests` 覆盖普通/多行/中文输入、Home/End、方向键和文本快捷键、字段错误与服务端错误保留、按钮焦点不陷入循环、未知枚举、多选身份、表格稳定选择、只读集合、字符串集合 CRUD/重排、Help 返回、Toast 时钟/容量、日志暂停/控制字符、窄宽屏、可选布尔、树折叠。

独立展示程序已在实际 80×24 tmux 中验证：`Enter → End → Left → Z → Home → Right → 中` 正常插入；F1 Help 打开后 Esc 返回原输入；`?` 插入文本；端口输入 `443x` 后 Ctrl-S 留在同一多字段表单，显示 `Enter a whole number` 并保留 Name/Port；删除 `x` 后保存成功。原始当前格保存于 `/tmp/xrm-components-ui-20260926`。该展示程序不访问 daemon，不能单独证明页面集成。

## 页面集成证据

完整记录见 [真实屏幕与轨迹](evidence/component-rewrite/README.md) 和 [统一执行记录](component-ui-rewrite-execution.md)。终端主流程22组屏幕、Config21个操作、真实Xray DNS/routing表单→SOCKS loopback均已通过。日志实际增量验证发现FTXUI frame在暂停后追加时会居中移位，已改为有界冻结快照；新日志计数增长但可见历史不变，恢复跟随再切到最新数据。长表单服务端错误固定在滚动区域外，长列保留独立间隔。

PNG 从实际当前格的ANSI捕获呈现，附TXT/ANSI/HTML和实际按键；没有生成假截图。G5 人工结果仍未填写。

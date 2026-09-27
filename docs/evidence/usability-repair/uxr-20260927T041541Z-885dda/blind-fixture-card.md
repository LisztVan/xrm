# 独立操作员 fixture 卡（不含操作答案）

本卡只供新上下文黑盒复验。不要阅读源码、修复报告、提示词或既有测试脚本。所有操作只在你创建的隔离 lab 内进行，不访问真实订阅、生产数据库或系统网络。

- 当前二进制：`/home/lisztzy/Projects/xrm/build/component-rewrite/xrm` 与同目录 `xrmd`。
- 固定 Xray：`/tmp/xrm-g1-xray-v26.9.9/xray`。使用它登记和运行本地测试实例；启动 lab 不会自动连接核心。
- 启动入口：`python3 /home/lisztzy/Projects/xrm/scripts/tui-lab.py --build /home/lisztzy/Projects/xrm/build/component-rewrite --core /tmp/xrm-g1-xray-v26.9.9/xray --record <你自己的证据目录>/lab.json`。请用自己唯一的 tmux server/socket 运行，保存 `tmux capture-pane -e` 与当前格。
- lab 创建 `Lab Home` Profile、`Loopback direct` 本地节点和 `Local fixture` 订阅来源。`lab.json` 给出独立 socket、数据库、daemon PID、订阅 URL 和对象 ID；由 lab 选择随机本地 HTTP 端口。
- `Loopback direct` 是 loopback 内的原生 freedom outbound。允许在此 lab 中 Connect/Stop；不要假定它证明互联网代理连通性。
- lab 退出时会销毁自己的数据。退出前保存所需截图、CLI stdout/stderr/exit code 和 Source/运行状态查询。

用户任务：仅靠产品界面和 Help 找到从只读恢复 Edit 的路径；新增 DNS 服务器与 routing 规则并修正一次缺目标错误；操作集合第二项、排序、取消及保存；诊断并修正一次失败订阅；在 daemon 暂时不可用时判断可做的事并在恢复后继续；连接固定核心并查看 Logs/Stats 后明确 Stop；通过 CLI 按名称/ID 查来源、提交文件配置、采样 watch 管道。记录每一步的实际按键、错路、Help 次数、当前格和结果。没有完成的任务按实记录。

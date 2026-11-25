# 智能体性能基线（Stage 4）

本文记录智能体页与核心层在**本机**上的可复现测量，给后续改动的回归对照用。数字都是"同一台机器、同一次构建"下的量级，不是承诺值。方法写在每节末尾，换机器/换构建类型要重测并在下面追加一行日期。

## 1. 核心层

测量入口：`verify_agent`（`tests/core_modules/AgentPerformanceTests.cpp`），输出带 `[perf]` 前缀的行。构建：`build/verify`（Release，MinGW 13.1，Qt 6.10.1），Windows 11 x64。

| 项 | 内容 | 2026-11-19 |
| --- | --- | --- |
| `session-save-2000` | 2000 条消息（每条约 45 字）保存一次，含快照 | 18 ms |
| `session-load-2000` | 同上读回 | 11 ms |
| `session-encrypt-2000` | 开启整会话 DPAPI 加密（写 `.enc`、删明文正文） | 46 ms |
| `session-decrypt-2000` | 解密读回 2000 条 | 45 ms |
| `context-build-2000` | 2000 条历史按 32768 预算裁剪一次 | < 1 ms |
| `estimate-tokens-200x16k` | 200 段 16 KB 文本的 token 估算 | 6 ms |

量法：`QElapsedTimer` 包住一次调用，含必要的文件写入/读取；不含进程启动、不含界面渲染。跑法：

```powershell
./build/verify/tests/verify_agent.exe | Select-String '\[perf\]'
```

## 2. 智能体页

测量入口：`ui_check_agent`（`tests/ui_check/modules/UiAgentCheck.cpp`），offscreen 平台，`build/verify` Release。

| 项 | 内容 | 2026-11-19 |
| --- | --- | --- |
| `agent-page-2000-messages` | 一次性 `setMessages(2000)` 并处理一轮事件（2026-11-19，未窗口化） | 3820 ms |
| `agent-page-2000-messages` | 同上（2026-11-25，只摆末尾 200 条） | 56 ms |
| `agent-page-2000-windowed` | 窗口化后实际创建的 `AgentMessageView` 个数 | 200 个 |

量法：先建好 2000 条 `AgentView::Message`，`QElapsedTimer` 包住 `setMessages` + 一次 `processEvents()`。这一项量的是"极端长会话一次全量重摆"的代价，不是滚动帧率；日常会话切回来走的是同一路径，所以它是上界。

已知：3820 ms 出现在 offscreen 首次布局（含 2000 个 `AgentMessageView` 的创建与 Markdown 渲染），说明长会话一次性重摆偏慢。现在的做法是窗口化：`AgentPage` 只渲染末尾 200 条（`kMessageWindow`），更早的点顶部「显示更早的 N 条消息」再补一页，追加新消息时回收最老的一页视图；2000 条一次 `setMessages` 降到 56 ms，视图数 200。量法与上面同一条用例（`ui_check_agent`，offscreen，`build/verification-mingw13` Release），只多了一个窗口化后的视图计数打印。限制：窗口外的消息不参与查找/跳转，真机长会话的手感仍留在 [AGENT](../../features/AGENT.md) §13.3 的人工项里。

## 3. 未测量

| 项 | 原因 |
| --- | --- |
| 首开智能体页到可输入的时长 | 需要真窗口 + 计时钩子，本轮只做了消息重摆 |
| 流式刷新连续帧 | 需要真实模型流或节流夹具 |
| 工具大输出（16 KB）在消息流里的渲染 | 与 2000 条消息同一路径，未单列 |
| 加密会话在真机磁盘上的读写 | 本机 SSD，未覆盖慢盘 |

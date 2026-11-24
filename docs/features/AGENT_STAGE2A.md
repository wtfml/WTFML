# 智能体 Stage 2A：stdio MCP

本文给开发者和测试看 Stage 2A 的现行行为、代码位置、盘上格式、实现边界和仍需人工验收的项（G2A）。设计约束见 [AGENT.md](AGENT.md) §10 与 D7，聊天底座见 [AGENT_STAGE1A.md](AGENT_STAGE1A.md)，文件工具见 [AGENT_STAGE1B.md](AGENT_STAGE1B.md)，维护工作流见 [AGENT_STAGE2B.md](AGENT_STAGE2B.md)，安全边界见 [SECURITY_BOUNDARIES.md](SECURITY_BOUNDARIES.md)。

## 1. 当前能力

- 只做 **stdio** MCP：不自动下载/安装服务器，不把 `npx` / `uvx` 当默认；可执行文件必须是绝对路径（不解析 `PATH`）。
- 每个服务器一条配置：名称、可执行文件、参数数组、工作目录、环境变量**白名单**（只记名字）、启用开关、三档超时。
- 握手：`initialize`（客户端提议 `2024-11-05`，以服务器返回的为准，低于 `2024-11-05` 报错）→ `notifications/initialized` → `tools/list`（支持 `cursor`，最多 10 页）→ 就绪。
- 服务器反向请求：`ping` 直接回结果，其它方法回 `-32601`；`notifications/tools/list_changed` 触发重列。
- 每个 MCP 工具**逐次审批**，即便服务器声明 `readOnlyHint`；工具线上名是 `mcp__<serverId>__<tool>`，非法字符换下划线，冲突拒绝。
- 工具结果是异步的：`AgentLoop` 进入"执行中"等结果，不阻塞 UI、不开嵌套事件循环；结果回来自动续跑这一轮。
- 结果进上下文前：遮凭据、16 KB 截断、标 `untrusted=true`；`isError` 映射成工具错误。
- 服务器 stdout 只走协议；stderr 进脱敏日志；垃圾行限 50 条，超了终止会话。
- 进程用 Job Object 管；关窗/停用/超时都清理整棵进程树；崩溃、超时都**不自动重启**。
- UI：API 设置页多一颗「MCP 服务器…」，页面里列出服务器、状态、身份摘要、工具数，可添加/编辑/删除/连接/断开，页顶固定写明"该服务器以当前 Windows 用户权限运行"。

## 2. 确认闸（D7）

- 落盘 `<agent 数据目录>/mcp/servers.json` 与 `mcp/confirmations.json`；确认记录含 `serverId + configHash + 可执行文件身份 + confirmedAt + purpose`。
- `configHash` 覆盖命令、参数、工作目录、环境变量名、启用开关、三档超时；任一项变了旧确认失效。
- 可执行文件身份含绝对路径、SHA-256、大小、mtime、卷序列 + 文件 ID（取不到时留空）。路径不变但内容/大小/mtime/文件 ID 变了 → 旧确认失效。
- 文件不存在、读不动、算不出身份 → **Blocked**，连"确认"按钮都不生效。
- 首次启动要确认；配置修改后要确认；**手动重连每次都重新确认**（即使配置和身份都没变）；不做自动重连。
- 确认卡展示：服务器名、可执行文件身份全文、参数、工作目录（空则 agent 数据目录）、环境变量白名单（空则"不继承"）、以及"本机用户权限"警告。
- 启动确认与工具审批互相独立：确认只代表"允许起这个进程"，每次工具调用还要单独批。

## 3. 代码位置

| 模块 | 文件 |
| --- | --- |
| 配置/身份/确认闸 | `src/agent/McpTrustStore.*`、`McpTypes.*` |
| JSON-RPC 帧 | `src/agent/McpProtocol.*` |
| 进程与握手 | `src/agent/McpClient.*` |
| 生命周期与工具汇总 | `src/agent/McpManager.*` |
| 工具审批与异步交付 | `src/agent/AgentMcpToolEngine.*` |
| 异步工具回调 | `AgentTool.hpp`（`AgentToolSink`）、`AgentLoop.*`（`Pending` 与 `onAsyncToolDone`）、`AgentCompositeToolEngine.*` |
| 界面 | `ui/AgentMcpPage.*`、`ui/AgentSettingsPage.*`（入口按钮） |
| 接线 | `src/controllers/AgentController.*`（`openMcpSettings` / `confirmAndStart` / `onMcp*`） |

## 4. 盘上格式与上限

```text
agent/mcp/servers.json         # {"schema":1,"servers":[{id,name,command,arguments[],workingDirectory,environmentNames[],enabled,三档超时}]}
agent/mcp/confirmations.json   # {"schema":1,"confirmations":[{serverId,configHash,identity{...},confirmedAt,purpose}]}
```

| 项 | 上限 |
| --- | --- |
| MCP 单行 | 8 MB（`McpLimits::kMaxLineBytes`） |
| 同时挂起请求 | 32 |
| `tools/list` 分页 | 10 页 |
| 单服务器工具数 | 200 |
| stdout 垃圾行 | 50 条 |
| 参数个数 / 单个参数长度 | 32 / 1024 字符 |
| 工作目录长度 | 1024 字符 |
| 超时（握手/列工具/调用） | 1–300 秒，默认 15/15/120 |

两份 JSON 都不含秘密值：环境变量只存名字，值不进配置文件（Stage 2A 只透传父进程同名变量）。

## 5. 实现边界

- 只支持 stdio；HTTP/SSE、采样（sampling）、roots、resource/prompt 都没有实现，服务器发这些请求会收到 `-32601`。
- 不自动重启、不做健康检查；服务器崩了状态是 `failed`，要用户手动重连并重新确认。
- 单次调用超时按配置终止**整个服务器进程**（随后必须手动重连+重新确认），不是只放弃这一次调用。
- 环境变量值不落盘：需要秘密值的服务器目前只能靠父进程环境里已有的同名变量。
- MCP 工具不进入工作流的技能白名单，因此在体检/崩溃急救等模式里不可用；只有自由对话模式能调。
- 工具结果只在本地脱敏与截断，不做二次外发确认（这一次调用的审批已覆盖外发）。
- 不做 `content` 里的图片/音频解码，只记 MIME 类型。
- 服务器工具名冲突只报错，不自动改名。

## 6. 自动覆盖

| 套件 | 本次结果 |
| --- | --- |
| `verify_agent`（含 MCP 三组） | 641 项通过 |
| `ui_check_agent` | 72 项通过 |
| `mw_check_agent` | 74 项通过 |

MCP 相关用例：

- `tests/core_modules/AgentMcpProtocolTests.cpp`：合法/非法帧、8 MB 上限、CRLF、通知不带 id、`ping`/未知方法应答、工具线上名清洗与截断、`configHash` 随参数/工作目录/启用开关变化。
- `tests/core_modules/AgentMcpTrustTests.cpp`：身份计算与一致性、相对路径/相对工作目录/参数过多被拒、首次启动要确认、配置变化失效、可执行文件被替换失效、文件删除直接 Blocked、重连必确认、确认记录落盘可读回、删服务器清记录、未连接时工具调用被拒。
- `tests/core_modules/AgentMcpClientTests.cpp`：用 `verify_agent --mcp-fake-server` 起真子进程，覆盖握手、分页合并（3 个工具）、stderr 脱敏、逐次审批与拒绝、批准后异步结果、结果里的口令遮挡、`isError` 映射、断开后拒绝调用、stdout 垃圾行不断连、崩溃不自动重启、重连要重新确认。
- `tests/core_modules/AgentLoopTests.cpp`：`Pending` 异步工具（进入执行中 → sink 回来完成；取消后迟到结果丢弃）。
- `tests/ui_check/modules/UiAgentCheck.cpp`：MCP 页权限提示、服务器行/身份/状态、连接/断开按钮、空态、API 设置页入口。
- `tests/mw_check/modules/MwAgentCheck.cpp`：真窗口里 MCP 管理器接线、没服务器时不起进程、未知 MCP 工具被拒。

## 7. 待人工测试（G2A）

- [ ] 用真实 stdio MCP 服务器（Node 与 Python 各一）走一遍：添加配置 → 首次启动的确认卡是否展示路径/SHA-256/参数/工作目录/环境变量白名单与"本机用户权限"；取消后确认进程没起。
- [ ] 配置修改（改一个参数、改工作目录、改环境变量名、改超时）后连接：确认卡必须重新出现。
- [ ] 运行中把可执行文件换成另一个版本（路径不变）：断开 → 连接时应提示"可执行文件变了"并要重新确认；不确认则不起进程。
- [ ] 断开后用「重新连接」：每次都要确认（即使什么都没改）。
- [ ] 服务器崩溃/被手动杀掉：状态显示失败、不自动重启；重连前仍要确认；任务管理器里没有残留子进程（Job Object）。
- [ ] 工具调用：审批卡显示参数；拒绝时不产生调用；批准后结果回灌，工具卡显示"外部 MCP / 不可信"。
- [ ] 服务器 stderr 里带口令形态的内容：MCP 页日志里是遮过的。
- [ ] 服务器输错协议/吐垃圾行：会话终止并给出人话，不卡死、不无限重试。
- [ ] 单次调用超时（把调用超时调到最小值 + 一个慢工具）：请求以超时错误结束、服务器进程被清理、状态变失败。
- [ ] 关闭启动器时有服务器在跑：进程树被清理，没有孤儿进程。
- [ ] 两个服务器导出同名工具：第二个被拒并说明冲突。
- [ ] MCP 工具在「崩溃急救」等模式里不可用（被技能白名单挡住），在自由对话里可用。

## 8. 未覆盖

| 项 | 状态 |
| --- | --- |
| HTTP/SSE MCP | 未实现（Stage 3 再议） |
| sampling / roots / resources / prompts | 未实现，回 `-32601` |
| 环境变量秘密值存储 | 未实现，只透传父进程同名变量 |
| 结果中的图片/音频内容 | 只记 MIME 类型，不解码 |
| 服务器工具注解（`readOnlyHint`）信任 | 不采用，一律逐次审批 |
| 真机高 DPI / 读屏 | 未覆盖 |
| 长时间运行的服务器内存/句柄泄漏 | 未测量 |

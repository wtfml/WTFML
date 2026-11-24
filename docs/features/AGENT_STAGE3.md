# 智能体 Stage 3：联网与固定诊断命令

本文给开发者和测试看 Stage 3 的现行行为、代码位置、盘上配置、实现边界和仍需人工验收的项（G3）。设计约束见 [AGENT.md](AGENT.md) §9.5，安全边界见 [SECURITY_BOUNDARIES.md](SECURITY_BOUNDARIES.md) §4.7，Stage 3/4 的验收清单单独放在 [AGENT_STAGE3_4_ACCEPTANCE.md](AGENT_STAGE3_4_ACCEPTANCE.md)。前面几段：[Stage 1A](AGENT_STAGE1A.md)、[Stage 1B](AGENT_STAGE1B.md)、[Stage 2B](AGENT_STAGE2B.md)、[Stage 2A](AGENT_STAGE2A.md)。

## 1. 当前能力

### 1.1 联网工具

- `web.fetch`：只抓 `http/https` 公网地址。
  - 拒绝 `file:`/`data:`/`ftp:` 等 scheme、userinfo、URL 片段、空 host。
  - 解析目标 host 的**全部** A/AAAA，任何一个落在 loopback / 私网 / 链路本地 / 组播 / 保留段 / 未指定（含 `::ffff:` 映射的 v4）就整条拒绝。
  - **连接已校验的 IP**：URL 的 host 换成 IP，`Host` 头与 TLS 主机名校验仍用原 host（`QNetworkRequest::setPeerVerifyName`）。
  - 手动跟随重定向，最多 5 跳；**每一跳重新解析 + 全地址校验 + pinned-IP 连接**，不复用上一跳结果。
  - 强制直连：`QNetworkAccessManager` 明确设 `NoProxy`，不继承系统代理/环境变量代理。
  - 只接受文本类 Content-Type（`text/*`、JSON、XML 家族），其它直接拒绝；响应上限 2 MB，超出截断。
  - HTML 转纯文本（去 script/style/标签、解常见实体、压缩空行），再遮凭据，正文最多 200000 字符，超出截断。
- `web.search`：用设置里的 **SearXNG** 实例（`<endpoint>/search?...&format=json`）搜索，最多返回 20 条（标题、URL、摘要），摘要同样遮凭据。没配地址时工具直接拒绝。
- 两个工具都**逐次审批**（搜索词/URL 会发给第三方），审批卡写目标 host 与完整 URL；结果是不可信内容（`untrusted: true`）。

### 1.2 固定诊断命令（`command.diagnose`）

- 默认**关闭**；设置页「联网与诊断」里打开后才可用。
- 只开放编译进程序的固定 spec：`java-version`、`where-java`、`ping <host>`、`nslookup <host>`、`netstat -ano`、`ipconfig /all`。
- `host` 参数只接受域名/IP 字面量（只允许 `[A-Za-z0-9.:_-]`，不能以 `-` 或 `.` 开头），不拼 shell 字符串，也不经过 shell。
- 每次执行都审批，审批卡写完整命令行；进程用 QProcess + Job Object（`KILL_ON_JOB_CLOSE`）管，超时/取消清理整棵进程树。
- 输出合并 stdout/stderr，上限 256 KB，结果遮凭据、标 `untrusted`；退出码原样返回。
- `command.run`（任意可执行文件）**没有实现**，也不出现在工具清单里。

## 2. 配置

| 项 | 存放 | 说明 |
| --- | --- | --- |
| SearXNG 地址 | `QSettings` `Agent/WebSearchEndpoint` | 必须 http/https；空 = 联网搜索不可用 |
| 诊断命令开关 | `QSettings` `Agent/DiagnoseCommands` | 默认 `false`；打开后每次执行仍要审批 |

两项都在 API 设置页的「联网与诊断」组里；保存时地址不是 http/https 会被拒。

## 3. 代码位置

| 模块 | 文件 |
| --- | --- |
| 地址策略/HTML 转文本 | `src/agent/AgentWebPolicy.*` |
| 传输接口 | `src/agent/AgentWebTransport.hpp` |
| Qt 实现（直连 + pinned-IP） | `src/agent/QtAgentWebTransport.*` |
| 联网工具 | `src/agent/AgentWebTools.*` |
| 固定诊断命令 | `src/agent/AgentCommandTools.*` |
| 设置 | `src/agent/AgentSettings.*`、`ui/AgentSettingsPage.*` |
| 接线 | `src/controllers/AgentController.*`（合成引擎里多两个子引擎） |

## 4. 上限与超时

| 项 | 上限 |
| --- | --- |
| 响应体 | 2 MB（超出截断并标记） |
| 转文本后正文字符 | 200000 |
| 重定向 | 5 跳 |
| 单次请求超时 | 20 秒（`AgentWebPolicy::kTimeoutMs`） |
| 搜索词长度 | 400 字符 |
| URL 长度 | 2048 字符 |
| 命令输出 | 256 KB |
| 命令超时 | 20–30 秒（按 spec） |
| 同时执行的命令 | 1 条 |

## 5. 实现边界

- 搜索只支持 SearXNG（JSON 输出）。Brave / Tavily 没实现；不抓搜索引擎结果页 HTML 充当搜索。
- 联网工具只在「自由对话」模式可用：维护工作流的技能白名单里没有 `web.*` / `command.diagnose`。
- 不提供"本会话允许联网"：每次调用都审批（比设计里允许的会话级放行更严）。
- 传输层只做 GET；不下载二进制、不跟随到内网、不执行 JS、不解析 Cookie 以外的凭据内容。
- 诊断命令只在 PATH 上找可执行文件；找不到 `java` 时给明确错误，不自动改用实例里配置的 Java 路径。
- 命令输出不做本机路径投影（它本来就是给用户看的诊断输出），但会遮凭据；工具卡与结果是不可信内容。
- 没有 `command.run`、没有资源搜索/安装工具、没有 HTTP/SSE MCP。

## 6. 自动覆盖

| 套件 | 本次结果 |
| --- | --- |
| `verify_agent` | 749 项通过 |
| `ui_check_agent` | 75 项通过 |
| `mw_check_agent` | 76 项通过 |

Stage 3 用例：

- `tests/core_modules/AgentWebPolicyTests.cpp`：scheme/userinfo/片段拒绝；v4 各特殊段、v6 loopback/ULA/link-local/组播/文档段、`::ffff:` 映射；多地址混私网整条拒绝；pinned URL 保留端口/路径/查询；Content-Type 白名单；HTML 去 script/style；截断。
- `tests/core_modules/AgentWebToolsTests.cpp`（假传输）：审批前不发请求、拒绝不抓取、pinned-IP 与 Host/TLS 主机名、私网阻断、逐跳重新解析、重定向到私网被拒、重定向上限、非文本类型被拒、结果脱敏与截断、搜索 JSON 解析与摘要脱敏、没配 SearXNG 时拒绝。
- `tests/core_modules/AgentCommandToolsTests.cpp`：默认关闭、白名单外命令拒绝、缺 host/元字符/多余参数拒绝、逐次审批、拒绝不执行、真跑 `where-java` 有结果、失效审批拒绝。
- `tests/ui_check/modules/UiAgentCheck.cpp`：设置页 SearXNG 地址与诊断命令开关。
- `tests/mw_check/modules/MwAgentCheck.cpp`：真窗口里诊断命令默认关闭、`web.fetch` 要审批。

## 7. 待人工测试

见 [AGENT_STAGE3_4_ACCEPTANCE.md](AGENT_STAGE3_4_ACCEPTANCE.md) 的 G3 一节（DNS 多地址/重绑定、系统代理已配置仍直连、证书主机名校验、真实搜索与真实命令）。

## 8. 未覆盖

| 项 | 状态 |
| --- | --- |
| Brave / Tavily 搜索 provider | 未实现 |
| 资源搜索/安装/更新工具 | 未实现 |
| HTTP/SSE MCP | 未实现 |
| `command.run`（任意可执行文件） | 未开放 |
| 代理支持（有意不做） | 代理可能绕过 pinned-IP 校验，第一版强制直连 |
| 真机读屏 / 高 DPI | 未覆盖 |

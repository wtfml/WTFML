# 智能体（Agent）

本文写智能体现行行为、接口与硬约束，给改智能体代码和做验收的人看。设计决策（D1–D11）仍是硬约定，见 §2；安全边界细节另见 [SECURITY_BOUNDARIES](SECURITY_BOUNDARIES.md)，测试入口与超时见 [TESTING](../dev/TESTING.md)，性能数字见 [perf-notes](../dev/perf-notes/AGENT_STAGE4.md)。

## 1. 定位与边界

启动器内置的**实例维护助手**：观察实例事实、解释异常、给出计划，用户批准后执行受控操作。不做通用代码助手，不做无人值守维护，不执行模型生成的任意脚本。

- 能做的：锁定实例内读目录/读文本/写配置白名单；跑启动诊断与崩溃诊断；看日志结尾与崩溃报告；改启动器设置（白名单键）；启停模组；建/恢复维护备份；只读检查升级并按需打开现有向导；联网抓取/搜索；固定诊断命令；stdio MCP。
- 不做的：任意命令（`command.run`）、资源安装工具、HTTP/SSE MCP、后台自动维护、跨用户数据。
- 不配置智能体也能正常用启动器其它功能；智能体故障不阻断启动。

## 2. 设计约定（不可随手改）

| ID | 约定 |
| --- | --- |
| D1 | 导航顺序 主页/管理/下载/设置/智能体，启动默认主页 |
| D2 | 会话是启动器级全局目录 `%LOCALAPPDATA%\WTFML\agent`，默认明文 JSON，与游戏文件夹解耦 |
| D3 | 原生 loop + 受控工具；技能编译进程序，不从远程下载 |
| D4 | 用户看到的是"模式"（技能），Stage 1 只有自由对话 |
| D5 | 只读自动；写/删/升级/命令/外部 MCP 逐次确认；备份创建也要审批；没有"全自动" |
| D6 | 只做 OpenAI-compatible 子集 + 枚举化适配，不接受任意 header/URL 模板/脚本 |
| D7 | MCP 只做 stdio，单独验收；首次启动/改配置/重连都确认；文件身份变了重新确认；不自动重连；工具逐次审批 |
| D8 | 首次实例工具调用锁定实例；之后不跟主页切换；工具不接受模型传的实例 id |
| D9 | 第一版只开放固定诊断命令 + 参数白名单，默认关闭；任意可执行文件未开放 |
| D10 | 删除会话不删维护备份；备份带 `owner/source/sourceSessionId`；只清 `owner=agent` |
| D11 | 外发按内容判定：自动读取脱敏日志/崩溃报告/实例与目录元数据；配置/完整文件名/完整实例清单逐次确认；凭据与二进制硬阻断 |

## 3. 架构与依赖

`ui/Agent*` → `AgentController` → `src/agent/`（loop / context / session / provider / policy / tools）→ 现有网络与业务层。

- `ui/Agent*` 只 include `ui/` 与值类型，不 include `src/`。
- `src/agent/` 不 include `ui/` 或 `MainWindow`；业务能力只经 `AgentLauncherHost` 接口接入（实现在 `src/controllers/AgentLauncherHostImpl.*`）。
- 网络只走 `AgentModelClient`；凭据只走 `AgentSecretStore`；会话落盘只走 `AgentSessionStore`；路径只从 `AgentPaths` / `AppPaths` 取。
- UI 更新在主线程；网络异步；同一会话最多 1 个模型请求、1 个工具；异步工具有代次，取消后旧回调丢弃。

## 4. 界面

- 智能体页双栏：左侧会话栏（新建/搜索/分组/重命名/固定/删除），右侧聊天区。
- 顶行：标题、API 状态胶囊、实例胶囊、`⋯`（重命名/固定/加密存储/导出/删除）、API 设置。
- 输入岛上方一行**模式胶囊**：自由对话 / 实例体检 / 崩溃急救 / 版本升级 / 模组维护（`agentSkill_<id>`）；下面一行工作流状态（模式/阶段/计划步数/证据数），空串收起。
- 消息区：用户气泡、助手 Markdown、推理折叠、错误/中断、工具卡、审批卡（计划卡复用审批卡）。
- API 设置页另有「MCP 服务器…」入口与「联网与诊断」组（SearXNG 地址、允许诊断命令开关，默认关）。
- 主题、尺寸、无障碍沿用现有 Theme 令牌与 `objectName`。

## 5. API 与网络安全

### 5.1 Provider 兼容子集

`POST <base>/chat/completions`；支持 `messages`、`tools`、`tool_choice:auto`、流式 SSE 与非流式 JSON；解析 `delta.content`、`reasoning_content`、`tool_calls`、`finish_reason`、`usage`、`data: [DONE]`。适配项只允许枚举：

| 适配项 | 取值 | 默认 |
| --- | --- | --- |
| `authMode` | bearer / api-key-header / none | bearer |
| `stream` | on / off | on |
| `maxTokensField` | max_tokens / max_completion_tokens | max_tokens |
| `systemRole` | system / developer | system |
| `reasoningEcho` | display-only / send-back | display-only |
| `temperature` | send / omit | send |
| `toolChoice` | auto / omit | auto |

工具线上名必须匹配 `^[A-Za-z0-9_-]+$`；内部 `fs.read` 映射为 `fs_read`。错误分类：`NoConfiguration / Unauthorized / RateLimited / Quota / Network / Protocol / ContextOverflow / ToolError / EgressApprovalRequired / EgressBlocked / ApprovalDenied / Cancelled / Internal`；401/403/配额不重试，首字节前网络/429/5xx 最多 2 次（1 s / 4 s，尊重 Retry-After），流开始后失败保留部分文本不自动重发。

### 5.2 凭据

API Key 只落 Windows 凭据管理器（`WTFML/Agent/<profileId>`），主路径失败用 DPAPI 后备（QSettings 里的密文）；两条都失败拒绝保存并保留旧值。界面默认星号遮挡、不回显；Key 不进日志、会话、错误、导出或 URL。

### 5.3 网络边界

Base URL 只接受 `https://`；loopback `http://` 需显式勾选并持续提示；拒绝 userinfo、片段、查询串。不跟随重定向、不忽略 TLS 错误；响应 16 MB、SSE 单行 1 MB。

### 5.6 模型上下文的数据外发范围（D11）

#### 5.6.2 默认边界

| 数据类型 | 默认行为 |
| --- | --- |
| 脱敏日志、崩溃报告、锁定实例元数据、必要目录元数据、实例总数 | 自动读取；首次使用提示「可能发送给当前模型服务」 |
| 配置、完整文件名清单、其他文本、完整实例清单 | 逐次确认；确认后只发脱敏片段；没有会话级目录放行 |
| 凭据文件/凭据存储、二进制 | 硬阻断 |
| 严格模式 | 自动读取改成逐次确认；硬阻断不变 |

#### 5.6.2.1 当前脱敏方案

`AgentRedaction` 以内容为主、文件名为辅：键名按 `_`、`-`、`.` 与驼峰切词根，命中 `password/passwd/pwd/secret/token/credential(s)/cookie/csrf/session/apikey` 或相邻 `api/key`、`private/key`、`access/key` 时把值换成 `<redacted>`；值少于 6 字符不遮；固定形态覆盖 `Authorization: Bearer …`、`Bearer …`、`sk-…`、JWT、`AKIA…`、PEM 私钥块。凭据文件名按 dotenv 家族、证书后缀、`credential/secret/token/apikey` 词根判定。日志与崩溃报告里的本机绝对路径换成 `<path>`；诊断结果经字段白名单投影，不带 Java 路径、数据根、原始 stack trace。

限制：词根表之外的自造字段名靠固定令牌形态兜底；裸 Windows 用户名、启动参数回显与硬件指纹仍可能出现在日志片段里。

## 6. 会话、上下文与存储

### 6.1 存储位置

`%LOCALAPPDATA%\WTFML\agent`，与游戏文件夹解耦；缓存与断点续传在 `%LOCALAPPDATA%\WTFML\cache`。测试用 `QCoreApplication::setProperty("wtfml.testAgentDataDir", <临时目录>)` 隔离。旧版三层路径（`…\WTFML\WTFML\…`）在启动时一次性迁移：新位置已有内容就不动，旧目录只在搬空后 rmdir。

```text
agent/
  sessions/<uuid>.json | .enc        正文（消息/summary/usage/pendingApproval）
  sessions/<uuid>.meta.json          标题/时间/固定/加密/skill/instanceLock/哈希/删除状态
  sessions/.recovery/<uuid>/         稳定点快照（最多 3 份）
  artifacts/<uuid>/<id>.enc + .meta.json   模型可读 artifact（DPAPI，meta 只有大小/哈希）
  backups/maintenance/<id>/original.enc + meta.json   维护备份（owner/source/sourceSessionId）
  mcp/servers.json, mcp/confirmations.json            MCP 配置与确认记录
  logs/
```

### 6.2 元数据与消息

消息状态 `complete/streaming/interrupted/error/tool_pending/awaiting_approval`；审批只存请求、不存「已批准」，重启后作废。正文 8 MB / 2000 条上限。

### 6.3 原子写、快照与损坏隔离

正文先写、元数据后写，`QSaveFile` 原子提交；稳定点保留最近 3 份快照；正文损坏改名隔离，不当空会话覆盖；启动时 `streaming` 恢复为 `interrupted`。

### 6.4 artifacts

模型可读 artifact 只存脱敏内容并 DPAPI 加密；本地原始存档 id 不在模型可见空间；`artifact.read` 每次重新脱敏；大文本 artifact 只存这次读取的窗口。

### 6.5 多实例隔离与会话锁定

首次实例工具调用锁定当前实例并先落盘；锁定后不接受模型传入的实例 id；换游戏文件夹/实例删除返回 `InstanceUnavailable`；显式切换需确认。

### 6.6 导出、备份、删除

删除会话删正文/快照/artifacts/元数据，不删维护备份；活动会话先取消/等待，文件提交阶段拒绝强行中断；`owner=launcher/user/shared` 只删引用；来源会话删除后备份仍可恢复。

### 6.7 整会话加密

可选 DPAPI 加密（`.enc`），标题与时间仍明文；加密失败保留明文并报错，解密失败不回退明文；加密只保护落盘，发送给模型的内容不变。

## 7. 上下文管理

### 7.1 环境快照

每轮带只读快照：启动器版本、当前游戏根显示名、锁定实例摘要、最近启动/异常摘要、外发策略与工具集版本、会话预算。未锁定实例不假装已确定。

### 7.2 预算

保守估算 token（CJK 1、ASCII 0.3、其它 1），预算 = `contextWindow − maxOutput − 512`；优先级：系统提示 → 工具 schema → 环境快照 → 最近完整轮次 → 工具结果摘要 → 更早摘要 → 省略标记；永远保留最后一条用户消息与进行中的 assistant/tool 配对。

### 7.3 压缩与摘要

确定性裁剪并插入「（更早的 N 条消息已省略）」；不做 LLM 摘要；原文仍在会话文件里。

### 7.4 消息转换

内部消息转 OpenAI：system、user/assistant 文本、assistant `tool_calls`、tool 结果带 `tool_call_id`；工具结果是 JSON：`ok`、`summary`、`data`、`error?`，`data.evidence[]` 是证据。

## 8. 运行时

### 8.1 状态机

`Idle → Preparing → Requesting → AwaitingApproval / ExecutingTool → Completed / Failed / Cancelled`。同会话不允许并行 send；工具串行；单轮最多 8 步工具。

### 8.2 一轮的步骤

校验配置/密钥/实例 → 构建上下文 → 流式接收 → 合并文本/推理/工具调用 → 无工具则完成；有工具则校验名称/参数/风险 → 审批/执行 → 结果回灌继续。

### 8.3 重试与取消

首字节前网络/429/5xx 最多 2 次；流开始后失败不自动重发。取消要 abort、清定时器、丢弃半截 tool_calls、停止工具、持久化部分消息、恢复可发送状态。

### 8.4 持久化时机

用户消息后、assistant 完成/中断后、每个工具结果后、审批创建/决定后、重命名/固定/删除后保存；流式每 2 秒最多一次。

### 8.5 异步工具

MCP、联网、诊断命令是异步的：引擎先返回 `Status::Pending`，结果通过 `AgentToolSink` 回来，Loop 保持"执行中"继续等，不阻塞 UI、不开嵌套事件循环；取消或换代后迟到的结果丢弃。

## 9. 工具

### 9.1 工具接口

每个工具声明名称、风险等级、JSON Schema、超时与幂等性；未声明风险按 `ExternalMcp` 逐次审批；参数校验失败不执行；实例由会话锁定提供，参数里没有实例 id；`additionalProperties=false`。

### 9.2 风险与审批矩阵

只读自动；完整实例清单、完整模组文件名、配置读取、设置写、模组启停、备份创建与恢复逐次审批；命令与外部 MCP 逐次审批；没有会话级目录放行。

### 9.3 文件工具（5 个，只作用于锁定实例）

| 工具 | 审批 | 关键上限 |
| --- | --- | --- |
| `fs.list` | 自动 | 只回目录元数据（条目/文件/目录/字节/截断），最多 2000 项，**不返回文件名** |
| `fs.listNames` | 每次 | 完整文件名，最多 2000 项（默认 2000） |
| `fs.read` | 自动（配置/其他文本要确认） | 默认 256 KB、最大 1 MB；二进制/凭据拒绝；返回 sha256 + `fileIdentityToken`；>16 KB 转模型 artifact |
| `fs.write` | 每次 | 只写 `config/**`、`defaultconfigs/**`、`options.txt`，≤1 MB；必须带旧哈希与身份 token；写前备份、原子替换、写后回读 |
| `artifact.read` | 自动 | 单次 ≤1 MB（默认 64 KB）；每次重新脱敏；本地原始存档不可读 |

路径校验是字符串拒绝（绝对路径、`..`、盘符、UNC、ADS、保留名、尾点/空格）+ Win32 句柄级最终路径/文件 ID/reparse 核对；受保护目录（`versions/libraries/assets/java/servers`）原始写一律拒绝。

### 9.4 启动器工具（19 个，均经 `AgentLauncherHost`）

只读自动：

| 工具 | 关键上限 / 行为 |
| --- | --- |
| `instance.list` | 默认只给总数；`detail=full` 列名称/版本/加载器并逐次确认 |
| `instance.context` | 锁定实例元数据；未锁定回 `needsInstanceLock` |
| `diagnostics.startup` | `StartupDiagnosis::analyze()` + 字段投影；空结论报错 |
| `diagnostics.crash` | 最近异常退出事实 + 规则提示；正常退出报错 |
| `logs.tail` | ≤500 行（默认 120），读盘 ≤256 KB；实例 `logs/latest.log` → 数据根 `latest.log` → `wtfml.log`；路径换 `<path>` |
| `crash.list` | 实例 `crash-reports/` 优先 + 数据根兜底，同名去重，≤20 份；每份读头 8 KB；`detail=full` 要确认 |
| `instance.integrity` | 8 秒预算，超时回部分结果；只给分组计数 |
| `mods.list` | 默认给启用/停用数；`detail=full` 列文件名并逐次确认，≤2000 |
| `settings.get` | 白名单键的非路径字段（`java.path` 只写不读） |
| `java.list` / `memory.suggest` | 主版本列表 / 推荐 Xmx、Xms |
| `backup.list` | 维护备份 + 启动器 zip（zip 只列，`restorable=false`），≤200 条 |
| `upgrade.inspect` | 候选目标 + 兼容性结论，≤40 个目标 |

写 / 高风险（逐次审批）：

| 工具 | 关键上限 / 行为 |
| --- | --- |
| `settings.set` | 白名单键：`memory.auto/xmx/xms`、`java.autoSelect`、`java.path`、`args.extraJvm`、`launch.afterLaunch/showGameLog/processPriority`；审批卡写旧值→新值；Xmx 512–65536、Xms 256–65536 |
| `mods.setEnabled` | 只改扩展名 `.jar` ↔ `.jar.disabled`；目标已存在拒绝 |
| `backup.create` | 范围 config/mods/options/servers；单文件 ≤8 MB、总量 ≤32 MB、≤400 文件；`owner=agent` 写 `backups/maintenance` |
| `backup.restore` | 恢复前先做快照；路径越界/绝对路径拒绝；只恢复 `owner=agent` |

只开页面：`upgrade.openWizard`、`launcher.openPage`（实例设置 / 日志 / 备份 / 完整性）。

#### 9.4.1 诊断结果字段白名单

`diagnostics.startup`、`diagnostics.crash`、`instance.integrity` 自动执行，但结果先经 `AgentDiagnosticProjection`：只留实例名/版本/加载器、Java 主版本、Xmx/Xms、模组数、退出码、崩溃分类、OOM/缺前置等布尔事实、发现的 severity/code/title/hint 与证据 id；Java 路径、数据根、日志/崩溃报告绝对路径、账户、环境变量、原始 stack trace 一律剔除。空结果不报成功。

### 9.5 联网与固定诊断命令

| 工具 | 审批 | 关键上限 |
| --- | --- | --- |
| `web.fetch` | 每次 | 只 http/https；全部 A/AAAA 必须公网（拒绝 loopback/私网/链路本地/组播/保留，含 `::ffff:`）；连接已校验 IP，Host 与 TLS 主机名仍用原 host；重定向 ≤5 跳且每跳重检；强制直连（不继承系统代理）；文本类 Content-Type；响应 2 MB、正文 20 万字符；超时 20 s |
| `web.search` | 每次 | 只支持 SearXNG（设置里填地址）；查询 ≤400 字符；≤20 条结果；摘要遮凭据 |
| `command.diagnose` | 每次 | **默认关闭**；spec 固定：`java -version`(20 s)、`where java`(15 s)、`ping -n 4 <host>`、`nslookup <host>`、`netstat -ano`、`ipconfig /all`（30 s）；host 只接受 `[A-Za-z0-9.:_-]{1,253}`；输出 ≤256 KB；Job Object 清理进程树 |

#### 9.5.1 命令 spec

命令定义编译进程序（`AgentCommandSpecs::all()`），声明可执行文件、固定参数、可选 host、超时；参数不拼 shell 字符串；每次执行审批并显示完整命令行；`command.run`（任意可执行文件）不存在。

### 9.6 证据

工具结果带 `evidence: [{id, kind, source, capturedAt, sha256?, excerpt?}]`，id 从 `E1` 起；最终报告按 id 引用；无证据必须写"无法确认"并说明还需要什么。

## 10. MCP（stdio，单独验收）

### 10.1 配置与确认闸

服务器条目：名称、绝对命令、参数数组、工作目录、环境变量**名**、启用开关、三档超时（默认 15/15/120 s，1–300 s）。不自动下载/安装，不把 `npx`/`uvx` 当默认。

- 身份：解析后的绝对路径 + SHA-256 + 大小 + mtime + 卷序列/文件 ID；`configHash` 覆盖命令、参数、工作目录、环境变量名、启用开关与超时。
- 首次启动、配置修改、手动重连都要确认（重连即使没改也要确认）；文件缺失/读不动/算不出身份直接 Blocked；身份或 `configHash` 变化旧确认失效。
- 确认卡展示路径/SHA-256/参数/工作目录/环境变量白名单，并写明「以当前 Windows 用户权限运行」。确认与工具审批相互独立。

### 10.2 协议

JSON-RPC 2.0、stdio、换行分隔。`initialize`（提议 `2024-11-05`，以服务器返回为准，低于则报错）→ `notifications/initialized` → `tools/list`（cursor 分页 ≤10 页）→ `tools/call`；支持 `notifications/tools/list_changed`；服务器反向请求只回 `ping`，其它 `-32601`。

上限：单行 8 MB、同时挂起 32、单服务器 200 工具、stdout 垃圾行 50。stdout 只走协议，stderr 进脱敏日志；进程用 Job Object（`KILL_ON_JOB_CLOSE`）管；崩溃/超时/EOF 都让 pending 以错误结束，**不自动重启**。

### 10.3 审批与信任

工具名 `mcp__<serverId>__<tool>`（≤64 字符，非法字符换 `_`，冲突拒绝）。所有 MCP 工具逐次审批，不信任服务器声明的只读注解。结果标 `untrusted`、遮凭据、内联 16 KB 截断；`isError` 映射成工具错误。

### 10.4 测试

`verify_agent --mcp-fake-server` 是假服务器入口（同一可执行文件的隐藏模式），覆盖握手、分页、审批拒绝、异步结果、垃圾行、崩溃不重启、重连确认。

## 11. 技能与工作流

### 11.1 技能（编译进程序）

| 技能 | 模式 | 只读 | 要求计划 | 必须碰过 |
| --- | --- | --- | --- | --- |
| `general` | 自由对话 | 否 | 否 | — |
| `inspection` | 实例体检 | 是 | 否 | `diagnostics.startup` |
| `crash-rescue` | 崩溃急救 | 否 | 是 | `diagnostics.crash` |
| `upgrade` | 版本升级 / 移植 | 否 | 是 | `upgrade.inspect` |
| `mods` | 模组维护 | 否 | 是 | `mods.list` |

技能只能缩小工具集合，改不了审批/外发策略；模式记在会话 `skill` 字段；换模式作废旧计划、证据保留。联网与诊断命令只在自由对话可用。

### 11.2 工作流

- **实例体检**：锁定实例 → 启动诊断 → 日志/模组/完整性 → 分级结论 + 证据 + 下一步选项；不写盘。
- **崩溃急救**：收集异常退出事实、日志结尾、最新崩溃报告 → 按现有规则分类（OOM、Java 版本不匹配、缺前置、文件损坏、配置错误，不猜具体模组）→ 计划 → 逐步执行（每步单独审批、失败即停）→ 验证 → 报告写回退方式与未解决项。
- **版本升级 / 移植**：`upgrade.inspect` 只读检查 + 解释风险；执行一律打开现有升级向导。
- **模组维护**：列清单 → 按现有规则查依赖（不按文件名猜）→ 启停/备份。

### 11.3 工作流状态机

```
Collect → AwaitPlanApproval → Execute*（每步审批）→ Verify → Report
                    ↘ 用户拒绝 → Collect        ↘ 某步失败 → Failed（写工具全锁）
```

- `workflow.submitPlan`：结构化 `{goal(≤500), steps[{tool, arguments, reason(≤300), verify}]}`；本地校验工具在白名单、参数过 schema、只读不许写、至少一个修复步与一个验证步（验证步必须只读）、步数 ≤20。
- 计划按数组顺序执行；不是当前这一步的写工具被拒绝并点名该做哪一步；批准计划 ≠ 批准写入。
- `workflow.finish`：`report(≤8000)` + `unresolved(≤2000)`；本地核对计划已批准、每步有结果、跑过验证步、必需工具碰过、至少一条证据；缺一项拒绝，失败过不能报完成。

### 11.4 核心提示词

提示词全部编译在 `src/agent/AgentSkill.cpp`（技能 `systemPrompt`），控制器每轮只追加一行「当前工作流状态（本地记账，不要复述给用户）：…」。共享规则原文：

> 没有用工具读到的文件、日志或实例信息，不要假装读过；不知道就说不确定。任何凭据、令牌、账户信息都不要索要或复述。每条结论引用证据 id（E1、E2…）；拿不到证据就写“无法确认”并说明还需要什么。

各技能在此基础上加各自的只读/计划/报告要求；工具描述与参数 schema 在各自引擎的 `specs()` 里（文件工具 `AgentToolRegistry.cpp`、启动器 `AgentLauncherTools.cpp`、工作流 `AgentWorkflowEngine.cpp`、联网 `AgentWebTools.cpp`、命令 `AgentCommandTools.cpp`）。系统提示作为 `messages` 第一条发出，role 按 `systemRole` 适配。

## 12. 防御性约束与上限

- 外部输入（模型 JSON/SSE、MCP、会话 JSON、工具参数、文件/日志/网页）全部按不可信解析，有大小/深度/条目/长度上限；解析失败返回错误，不抛异常穿过边界。
- 路径只用"实例 id + 相对路径"接口；覆盖/删除必须有哈希或对象身份；并发替换当可捕获错误处理。
- 异步请求带 requestId + sessionId + generation；删除会话前先取消；不用嵌套事件循环；关窗有界退出。
- 密钥、令牌、密码、Cookie 不进日志/会话/错误/导出；外发按 D11。

| 项 | 上限 |
| --- | --- |
| 用户单条消息 / assistant 单条 | 64 KB / 512 KB |
| 单次 API 响应 / SSE 单行 | 16 MB / 1 MB |
| 单条工具参数 / 单条工具内联结果 | 64 KB / 16 KB |
| 单会话正文 / 消息条数 / 快照 | 8 MB / 2000 条 / 3 份 |
| 单轮工具步数 / 单会话总数 | 8 / 40（总数按会话累计，见 §14 未覆盖） |
| 重试 / 模型请求总时长 | 2 次 / 10 分钟（可取消） |
| 工具超时 | 文件 30 s、MCP 120 s、命令 60 s、联网 20 s |
| 目录列表 / `fs.read` / `fs.write` | 2000 项 / 256 KB（最大 1 MB）/ 1 MB |
| MCP 单行 / 命令输出 | 8 MB / 256 KB |

## 13. 测试

### 13.1 入口

```powershell
./build/verification-mingw13/tests/verify_agent.exe            # 核心域（CTest wtfml.core-agent）
./build/verification-mingw13/tests/ui_check/ui_check_agent.exe # 页面（wtfml.ui-agent）
./build/verification-mingw13/tests/mw_check/mw_check_agent.exe # 真主窗口（wtfml.main-window-agent）
pwsh tools/check-agent-release.ps1                             # 发布检查
```

不联网、不写真实用户目录；每个用例独立 `QTemporaryDir` + 隔离 QSettings；`AgentPaths` 用 `wtfml.testAgentDataDir` 重定向。

### 13.2 覆盖

- 协议与客户端：SSE/分块边界、适配枚举、错误体保留、重试/取消、本地假服务器。
- 会话与凭据：元数据/正文往返、损坏隔离、快照、整会话加密、Credential Manager + DPAPI 后备、脱敏。
- 工具与安全：路径攻击矩阵、句柄级校验、审批矩阵、D11 边界表、artifact 分离、实例锁、备份生命周期、诊断投影、工作流闸门、MCP 协议/确认闸/假服务器、联网地址策略（假传输）、诊断命令。
- UI/集成：页面几何与状态、模式胶囊、工具卡/审批卡、真窗口里只读模式拒绝写、MCP 管理器接线、实例目录日志/崩溃报告读取。
- 性能基线：2000 条会话读写/加密、上下文裁剪、智能体页 2000 条消息重摆（数字见 perf-notes）。

### 13.3 待人工验证（核心）

真机/真网/真实例能做的、自动化覆盖不到的部分：

| # | 项 | 期望 |
| --- | --- | --- |
| A1 | 真实模型服务：流式、工具调用、401/429/断网、代理、TLS 错误 | 每类错误有人话与下一步，不伪造成功 |
| A2 | 真实实例：锁定 A 后主页切 B、体检、崩溃急救计划/拒绝/跳步/失败锁写 | 只操作锁定实例；失败后写工具全锁 |
| A3 | 配置写与回退、模组启停、备份创建/恢复、来源会话删除后恢复 | 可拒绝、可回退，备份在 `owner=agent` 下 |
| A4 | 真实 MCP（Node/Python）：首次启动、改配置、替换可执行文件、重连、崩溃清理 | 每次都确认；无孤儿进程 |
| A5 | 真实联网：私网/多 A 记录/重定向到内网、系统代理已配仍直连、证书主机名不匹配 | 全部拒绝或直连，不静默降级 |
| A6 | 真实诊断命令：默认关闭、审批、取消/超时后进程树清理 | 无孤儿进程，输出截断可见 |
| A7 | 凭据与会话：Credential Manager 主路径、DPAPI 后备、换 Windows 用户后解密失败 | 明确报错，不静默当空会话 |
| A8 | 发布物在干净机器启动；高 DPI 与读屏基础检查 | 智能体页可打开、无重叠、关键控件有名称 |

限制：A1–A8 需要真机与真实账号/服务，记录写进 [ACCEPTANCE_LOG](../archive/ACCEPTANCE_LOG.md)（一行：日期｜做了什么｜结论｜意外发现）。

## 14. 现状、未覆盖与剩余计划

已实现：聊天底座、文件工具、启动器工具、工作流、stdio MCP、联网与固定诊断命令、模式与证据、发布检查脚本。核心计数与最近一次自动化结果见 [TEST_RUN_HISTORY](../archive/TEST_RUN_HISTORY.md)。

未覆盖：

| 项 | 状态 |
| --- | --- |
| 单会话工具调用总数（设计 40） | 未实现（只有单轮 8 步） |
| artifact 单会话配额（设计 64 MB） | 未实现，不拒绝 |
| 联网工具的"本会话允许" | 审批卡按钮文案有，联网引擎未实现会话放行 |
| 日志里的裸用户名/启动参数/硬件指纹 | 只替换路径形态 |
| 资源搜索/安装/更新工具、Brave/Tavily、HTTP/SSE MCP、`command.run` | 未实现（见 [ROADMAP](../ROADMAP.md)） |
| 高 DPI/读屏/长路径/换用户解密/视觉套件 | 人工项，见 §13.3 |
| 长会话一次重摆（2000 条约 3.8 s） | 未优化，见 perf-notes |

剩余计划只在 [ROADMAP](../ROADMAP.md) 维护：G1B 复测、网络错误验收、A2–A8 中未做的人工项、Stage 3 剩余工具、性能与长会话渲染。

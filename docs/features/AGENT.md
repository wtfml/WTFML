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
| D10 | 删除会话不删备份；备份带来源实例与 `sourceSessionId`；智能体备份并入启动器备份（#129），在实例概览页「从快照恢复…」里与快照一起列出、可还原 |
| D11 | 外发按内容判定：自动读取脱敏日志/崩溃报告/实例与目录元数据；配置/完整文件名/完整实例清单逐次确认；凭据与二进制硬阻断 |
| D12 | Auto-safe（2026-11-24 产品确认）：用户显式打开后，**一次批准一个有范围、有时效的事务**，事务内按已批准计划执行写/启动/提交，不再逐次确认；范围来自计划与 `repair.begin`（工具、文件、设置键、模组、项目号），最长 60 分钟或 20 次启动；`java.path`、`args.extraJvm`、`saves/`、删除、`backup.restore`、`instance.repairCore`、别的工区一律拒绝；默认关闭、不随会话记忆；每次事务结束写审计。这是 D5 的唯一例外，其它路径仍逐次确认 |

## 3. 架构与依赖

`ui/Agent*` → `AgentController` → `src/agent/`（loop / context / session / provider / policy / tools）→ 现有网络与业务层。

- `ui/Agent*` 只 include `ui/` 与值类型，不 include `src/`。
- `src/agent/` 不 include `ui/` 或 `MainWindow`；业务能力只经宿主接口接入：`AgentLauncherHost`（诊断/设置/模组/备份）、`AgentLaunchHost`（测试启动）、`AgentRepairHost`（建工作区/提交回滚）、`AgentModHost`（平台查询/下载/核心修复），实现都在 `src/controllers/AgentLauncherHostImpl.*` 与 `AgentModPlatform.*`。
- 网络只走 `AgentModelClient`；凭据只走 `AgentSecretStore`；会话落盘只走 `AgentSessionStore`；路径只从 `AgentPaths` / `AppPaths` 取。
- UI 更新在主线程；网络异步；同一会话最多 1 个模型请求、1 个工具；异步工具有代次，取消后旧回调丢弃。

## 4. 界面

- 智能体页双栏：左侧会话栏（新建/搜索/分组/重命名/固定/删除），右侧聊天区。
- 顶行：标题、API 状态胶囊、实例胶囊、`⋯`（重命名/固定/加密存储/导出/删除）、API 设置。
- 输入岛上方一行**模式胶囊**：自由对话 / 实例体检 / 崩溃急救 / 版本升级 / 模组维护（`agentSkill_<id>`）；下面一行工作流状态（模式/阶段/计划步数/证据数），空串收起。
- 消息区：用户气泡、助手 Markdown、推理折叠、错误/中断、工具卡、审批卡（计划卡复用审批卡）。滚动跟随只在用户本来就贴着底部时生效：上翻阅读后，流式正文/工具卡/提示的更新不再把画面拽回最下方，折叠或展开思考过程同理；`Ctrl+End`（对话视图有焦点时）或滚轮回到最底部恢复跟随。
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
  repair-cases/<uuid>.json            本地修复病例库（默认关闭，只存结构化字段）
  backups/maintenance/<id>/original.enc + meta.json   旧维护备份（#129 起冻结：只读，不写不删）
  mcp/servers.json, mcp/confirmations.json            MCP 配置与确认记录
  logs/
```

### 6.2 元数据与消息

消息状态 `complete/streaming/interrupted/error/tool_pending/awaiting_approval`；审批只存请求、不存「已批准」，重启后作废。正文 8 MB / 2000 条上限。

### 6.3 原子写、快照与损坏隔离

正文先写、元数据后写，`QSaveFile` 原子提交；稳定点保留最近 3 份快照；正文损坏改名隔离，不当空会话覆盖；启动时 `streaming` 恢复为 `interrupted`。

### 6.4 artifacts

模型可读 artifact 只存脱敏内容并 DPAPI 加密；本地原始存档 id 不在模型可见空间；`artifact.read` 每次重新脱敏；大文本 artifact 只存这次读取的窗口。单会话配额 64 MB，按会话目录里密文的总字节算：写之前先算，超了就拒绝新的 artifact（`artifact.read` 不受影响，删会话一并清掉）。

### 6.5 多实例隔离与会话锁定

首次实例工具调用锁定当前实例并先落盘；锁定后不接受模型传入的实例 id；换游戏文件夹/实例删除返回 `InstanceUnavailable`；显式切换需确认。

### 6.6 导出、备份、删除

删除会话删正文/快照/artifacts/元数据，不删备份；活动会话先取消/等待，文件提交阶段拒绝强行中断；`owner=launcher/user/shared` 只删引用；来源会话删除后备份仍可恢复。

智能体备份（#129）写进当前游戏文件夹的 `cache/backups/files/<id>/`：`wtfml-backup.json`（`kind=files`，记来源实例、`trigger`、会话 id 与标题、逐文件大小与 sha1）+ `files/<相对路径>`，不加密，目录先写暂存再改名发布。

- 列表：实例概览页「从快照恢复…」把这个实例的智能体备份与 zip 快照按时间混排，行内写明来源与范围；`fs.write` 的写前备份按会话归一行，选中即撤销这次会话的修改（每个文件取会话里最早那份原文）。
- 还原只覆盖列出的文件，其他文件与备份后新增的文件不动；路径逐段校验、途经链接拒绝；先给被覆盖的文件做一份恢复前备份，写回或核对失败就把已写的改回、新建的删掉。运行中、正被复制/安装/还原或有备份任务时拒绝。
- 保留：每个实例最多 50 份、合计 1 GB，从最旧的删；最新 5 份不受大小限制，当前会话的不删；只清这一类，不碰 zip 与旧库。
- 写进修复事务工作区的 `fs.write` 不备份（那是副本）。
- 旧库 `agent/backups/maintenance` 照旧解密列出与还原，旧条目出现在当前游戏文件夹同名实例下；恢复前备份一律写新格式。

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

只读自动；完整实例清单、完整模组文件名、配置读取、设置写、模组启停、备份创建与恢复逐次审批；命令与外部 MCP 逐次审批。联网工具（`web.search` / `web.fetch`）额外提供“本会话允许此来源”：点过之后同一个 host 在这个会话里不再逐次弹审批，换 host、换会话、重启后都要重新确认。唯一另一处例外是 D12 Auto-safe：用户显式打开且计划已批准后，计划范围内的写/启动/提交不再逐次确认，范围外与硬禁止项照旧。

### 9.3 文件工具（5 个，只作用于锁定实例）

| 工具 | 审批 | 关键上限 |
| --- | --- | --- |
| `fs.list` | 自动 | 只回目录元数据（条目/文件/目录/字节/截断），最多 2000 项，**不返回文件名** |
| `fs.listNames` | 每次 | 完整文件名，最多 2000 项（默认 2000） |
| `fs.read` | 自动（配置/其他文本要确认） | 默认 256 KB、最大 1 MB；二进制/凭据拒绝；返回 sha256 + `fileIdentityToken`；>16 KB 转模型 artifact |
| `fs.write` | 每次 | 只写 `config/**`、`defaultconfigs/**`、`options.txt`，≤1 MB；必须带旧哈希与身份 token；写前备份、原子替换、写后回读 |
| `artifact.read` | 自动 | 单次 ≤1 MB（默认 64 KB）；每次重新脱敏；本地原始存档不可读 |

路径校验是字符串拒绝（绝对路径、`..`、盘符、UNC、ADS、保留名、尾点/空格）+ Win32 句柄级最终路径/文件 ID/reparse 核对；受保护目录（`versions/libraries/assets/java/servers`）原始写一律拒绝。

### 9.4 启动器工具（28 个，均经 `AgentLauncherHost` / `AgentModHost` / `AgentLaunchHost`）

分两部分：基础表（下面两张表）+ Stage 5 的六个专节——§9.4.2 测试启动、§9.4.3 修复事务、§9.4.4 崩溃快照与日志检索、§9.4.5 模组兼容与文件修复、§9.4.6 依赖感知二分、§9.4.7 Auto-safe 事务授权。

只读自动：

| 工具 | 关键上限 / 行为 |
| --- | --- |
| `instance.list` | 默认只给总数；`detail=full` 列名称/版本/加载器并逐次确认 |
| `instance.context` | 锁定实例元数据；未锁定回 `needsInstanceLock` |
| `diagnostics.startup` | `StartupDiagnosis::analyze()` + 字段投影；空结论报错 |
| `diagnostics.crash` | 最近异常退出事实 + 规则提示；正常退出报错 |
| `logs.tail` | ≤500 行（默认 120），读盘 ≤256 KB；实例 `logs/latest.log` → 数据根 `latest.log` → `wtfml.log`；路径换 `<path>` |
| `crash.list` | 实例 `crash-reports/` 优先 + 数据根兜底，同名去重，≤20 份；每份读头 8 KB；`detail=full` 要确认 |
| `diagnostics.crashSnapshot` | 解析最新一份崩溃报告（或日志尾）：环境、崩溃阶段、异常链、模组嫌疑、命中规则、建议验证；报告 8 MB、输出 16 KB、10 s 预算；额外给 `signature`（结构字段拼的症状键，可直接喂给 `repair.begin`）与 `priorCases`（开启病例库时同签名的历史动作按本地成功率排前 3 条） |
| `logs.search` | 字面量（大小写不敏感），带行号与前后文；≤200 条，结果 ≤24 KB 字符，扫描 6 s 预算；路径换 `<path>` |
| `logs.readRange` | 行号从 1 开始；≤500 行、≤256 KB；起点越界回空表并标 `truncated` |
| `crash.readFull` | 读整份报告（≤8 MB）；只回分段目录与每段摘要，原文脱敏后进 artifact；输出 ≤24 KB |
| `mods.analyzeCompatibility` | 读每个 JAR 的本地声明对照实例：加载器不匹配、版本不兼容、缺前置、重复、损坏；`offline=false` 时经确认后把 SHA-1 发给 Modrinth 补平台结论 |
| `mods.verifyFiles` | 逐个检查 `mods/` 下 JAR 的 zip 结构、大小、SHA-1；`platform=true` 时经同一份确认按哈希找回候选 |
| `instance.integrity` | 8 秒预算，超时回部分结果；只给分组计数 |
| `mods.list` | 默认给启用/停用数；`detail=full` 列文件名并逐次确认，≤2000 |
| `settings.get` | 白名单键的非路径字段（`java.path` 只写不读） |
| `java.list` / `memory.suggest` | 主版本列表 / 推荐 Xmx、Xms |
| `backup.list` | 智能体备份（新格式 + 旧库）+ 启动器 zip（zip 只列，`restorable=false`），带 `instanceId`，zip ≤200 条 |
| `upgrade.inspect` | 候选目标 + 兼容性结论，≤40 个目标 |

写 / 高风险（逐次审批）：

| 工具 | 关键上限 / 行为 |
| --- | --- |
| `settings.set` | 白名单键：`memory.auto/xmx/xms`、`java.autoSelect`、`java.path`、`args.extraJvm`、`launch.afterLaunch/showGameLog/processPriority`；审批卡写旧值→新值；Xmx 512–65536、Xms 256–65536 |
| `mods.setEnabled` | 只改扩展名 `.jar` ↔ `.jar.disabled`；目标已存在拒绝 |
| `backup.create` | 范围 config/mods/options/servers；单文件 ≤8 MB、总量 ≤32 MB、≤400 文件；写进 `cache/backups/files` |
| `backup.restore` | 只恢复会话锁定实例的智能体备份（别的实例、启动器 zip 直接拒绝）；实例忙时拒绝；恢复前先做备份，失败回滚；路径越界/绝对路径/链接拒绝 |
| `mods.installFix` | 只接受 `projectId`/`versionId`/`expectedSha1`；先在暂存目录下载并校验大小与 SHA-1，全部通过才写进事务工作区；可补齐版本声明的必需前置；没有活动事务时拒绝 |
| `instance.repairCore` | 复用 `VersionManager::repairVersionFiles()` 重下客户端 jar、库和资源索引；每次审批；事务活动期间拒绝 |
| `mods.bisect` | 在事务工作区里反复启停模组并启动游戏，做依赖感知 delta debugging；默认 8 次启动（最多 20）、45 分钟；结束后恢复 mods/config/options 基线 |

只开页面：`upgrade.openWizard`、`launcher.openPage`（实例设置 / 日志 / 备份 / 完整性）。

#### 9.4.1 诊断结果字段白名单

`diagnostics.startup`、`diagnostics.crash`、`instance.integrity` 自动执行，但结果先经 `AgentDiagnosticProjection`：只留实例名/版本/加载器、Java 主版本、Xmx/Xms、模组数、退出码、崩溃分类、OOM/缺前置等布尔事实、发现的 severity/code/title/hint 与证据 id；Java 路径、数据根、日志/崩溃报告绝对路径、账户、环境变量、原始 stack trace 一律剔除。空结果不报成功。

#### 9.4.2 测试启动（Stage 5.1，`AgentLaunchHost`）

`instance.testLaunch` 是异步工具：每次独立调用先审批，批准后由 `AgentTestLaunchRunner` 在隔离工作区启动一次游戏，等里程碑、崩溃或超时。

| 项 | 行为 |
| --- | --- |
| 工作区 | 默认 `<游戏根>/cache/agent-workspaces/<id>/`；用 `VersionManager::copyInstanceFrom()` 复制实例，登记与日志在 `%LOCALAPPDATA%/WTFML/agent/workspaces/`；原实例在启动期间不被写 |
| 启动参数 | 复用 `LauncherManager::generateJvmArguments()` 与启动器的资产/库路径；离线、固定测试身份 `WTFML-Test`、不刷新令牌、不连服务器、不下载 Java |
| 里程碑 | `process_started` / `main_menu` / `world_loaded`；标记在 `assets/agent/knowledge/launch_milestones.json`（读不到时判定一律 false，宁可超时）；另看进程退出码、新崩溃报告与 `hs_err_pid` |
| 上限 | 超时 1–600 s，观察窗口 0–60 s，重复 1–3 次；每个启动器窗口一次只允许一个测试启动；进程树用 Job Object 清理 |
| 只进副本世界 | `quickPlay.world` 只接受工作区 `saves/` 里已有的世界；源存档不参与启动 |
| 拒绝 | 实例正在运行、缺 Java、`-javaagent`/`-agentlib`/`-Xbootclasspath` 等能加载本地代码的 JVM 参数、工作区路径不匹配 |
| 结果 | `processStarted`、`exitCode`、`startupPhase`、`milestoneReached`、`crashed`、`timedOut`、`newCrashReport`、`durationMs`、日志引用与证据 id；工作区默认保留，日志留在本地；自动清理尚未实现 |

#### 9.4.3 修复事务（Stage 5.2，`AgentRepairHost`）

| 工具 | 行为 | 审批 |
| --- | --- | --- |
| `repair.begin` | 复制隔离工作区、记录源实例基线；返回 `transactionId` / `workspaceId`；可选 `symptomKey`（从 `diagnostics.crashSnapshot` 的 `signature` 抄来，只允许 `[A-Za-z0-9_.:+\|-]`、≤160 字符） | 每次 |
| `repair.diff` | 比较工作区与基线，列新增/修改/删除、哈希和配置预览 | 自动 |
| `repair.verify` | 在事务工作区调 `instance.testLaunch`，把里程碑/崩溃/超时写进事务 | 事务内自动 |
| `repair.finish` | `commit` 先核对源文件哈希、备份、原子重放差异；`rollback` 只丢弃工作区 | commit 每次；rollback 自动 |
| `repair.status` | 列活动/中断事务、状态、验证结果；不返回本机路径 | 自动 |

- 范围：`mods`、`config`、`defaultconfigs`、`options`、`settings`；不提交世界/存档。
- 事务内写工具（`fs.write`、`mods.setEnabled`、`settings.set`）带 `workspaceId` 只改工作区；不带 `workspaceId` 的直接写和 `backup.restore` 在事务活动期间拒绝。
- `settings.set` 在事务内只记旧值/新值，`repair.finish(commit)` 时才通过启动器设置写回；旧值不一致就拒绝提交。
- 提交前逐个核对源文件 SHA-256 等于基线；源文件被外部修改就拒绝并列出文件。提交失败按已应用文件备份回滚，回滚失败保留现场。
- `workflow.finish` 检测到未收尾事务时拒绝报完成；`repair.verify` 可作为计划的验证步，`repair.finish` 可在失败后回滚。
- 病例库（默认关闭，设置页「记住修复病例」）：`repair.verify` 把「症状键、范围/设置/改动类别标签、验证结果」写进 `agent/repair-cases/`，`repair.finish` 只把结果改成 `committed` / `rolled_back`。同签名同动作合并成一条（尝试 +1、成功 +1 看验证），本地按拉普拉斯平滑成功率 + 最近使用排序；相对路径明细只存本地，发给模型的 `priorCases` 只有动作类别与结果。

#### 9.4.4 崩溃快照与日志检索（Stage 5.3）

规则不写在提示词和 C++ 里：崩溃签名在 `assets/agent/knowledge/crash_rules.json`，启动里程碑标记在 `launch_milestones.json`，字段与置信度算法见该目录的 `README.md`。`AgentKnowledgeRules` 负责加载与校验（坏规则跳过并记 `problems`）；读不到规则时快照只给事实，`matchedRules` 为空。

| 工具 | 读什么 | 关键行为 |
| --- | --- | --- |
| `diagnostics.crashSnapshot` | 最新崩溃报告；没有报告就用日志尾（512 KB） | `phase`、`suggestedAction` 取最高优先级命中规则；`suspects` 按证据分档；`knowledgeVersion` + `knowledgeProblems` 标明用的是哪版规则 |
| `logs.search` | 实例 `logs/latest.log` → 数据根 `latest.log` → `wtfml.log`（`source=auto`/`instance`/`shared`/`launcher`） | 只做字面量匹配；同一份日志的行索引按文件大小 + 修改时间缓存 |
| `logs.readRange` | 同上 | 用行偏移表直接定位，不把整份日志读进内存 |
| `crash.readFull` | `crash.list` 列出的某一份报告 | 原文换掉本机路径后进模型可读 artifact，再用 `artifact.read` 按偏移取；分段摘要用来定位读哪一段 |

嫌疑模组的角色与置信度由证据类型决定，不由模型或规则自说：

| 证据 | `role` | `confidence` |
| --- | --- | --- |
| `Mixin apply for mod X failed`，或 mod id 出现在失败区栈帧里 | `candidate` | 规则基础值 + 0.12 |
| 崩溃文本点名的 mixin 配置能由 JAR 声明归属到某个模组 | `candidate` | 规则基础值 + 0.05 |
| 只在报告头 `Suspected Mods`、模组清单或规则 `suspects` 里出现 | `related` | ≤0.4 |
| mixin 配置读不到归属 | `related`（`modId=unknown`） | ≤0.4 |

上限：置信度封顶 0.95，不返回 1.0；`matchedRules` ≤8、嫌疑 ≤8、`causeChain` ≤8、建议验证 ≤8；证据 `id`/`kind`/`source`/`capturedAt`/`sha256`/`excerpt`，摘录 ≤400 字符且已脱敏。mixin 配置归属只读 JAR 里的声明（`fabric.mod.json`、`quilt.mod.json`、`META-INF/mods.toml`、`META-INF/neoforge.mods.toml`、`META-INF/MANIFEST.MF`），读不到就标 `unknown`，不按文件名猜。

限制：日志搜索在调用线程上顺序扫描，100 MB 实测见 [PERFORMANCE](../dev/PERFORMANCE.md)；超过 6 s 预算回部分结果并标 `truncated`。正则搜索未开放。

日志与崩溃报告进模型、进 artifact 前统一过 `AgentDiagnosticProjection::scrubLogText()`：先遮凭据（`AgentRedaction`），再把本机绝对路径换成 `<path>`，然后把启动参数里的 `--accessToken` / `--uuid` / `--username` / `--xuid` / `--clientid` / `--userProperties` 的值、MAC 地址、Windows 用户 SID、`HWID` / `MachineGuid` / `serial` 这类硬件标识字段换成占位符，最后把本机账户名（`USERNAME` / `USER` / 主目录最后一段；短于 4 个字符的不动）整词换成 `<user>`。显卡/驱动/音频设备型号不在剔除范围：它们是判断驱动问题的证据。

#### 9.4.5 模组兼容与文件修复（Stage 5.4）

本地声明解析在 `src/operations/ModDeclarations`（升级评估与智能体共用一份，不读界面、不读盘），判定在 `src/agent/AgentModCompat`，平台查询、下载与核心文件修复经 `AgentModHost`（`src/controllers/AgentModPlatform` + `DownloadManager` + `VersionManager`）。

| 工具 | 平台/网络 | 写盘 | 审批 |
| --- | --- | --- | --- |
| `mods.analyzeCompatibility` | 默认询问平台；`offline=true` 只做本地 | 不写 | 本地自动；**首次平台查询**按实例确认一次，严格模式每次 |
| `mods.verifyFiles` | 只有 `platform=true` 才查 | 不写 | 本地自动；平台查询用同一份确认 |
| `mods.installFix` | 解析下载地址 + 下载（只接 Modrinth） | 只写事务工作区 | 每次 |
| `instance.repairCore` | 官方源重下 | 写共享 `libraries/`、`assets/`、客户端 jar | 每次 |

平台查询的边界：只把 SHA-1 发给 Modrinth（**不发文件名**），每批 ≤100、每次 ≤2000 个哈希、总时限 180 s；结果按哈希由平台侧的地址与版本元数据解释。严格模式下每次都确认；非严格模式同一实例在同一会话里确认一次。平台只接 Modrinth，CurseForge 不参与；必要前置在 Modrinth 找不到时整批停止，不按名称搜索替代品。

问题类型：

| 类型 | 判据 |
| --- | --- |
| `missing_dependency` | 声明要求的 required 前置没有启用的文件；只有 `.jar.disabled` 时在 `detail` 里点名 |
| `incompatible_version` | 声明的游戏/加载器/Java 范围不满足，或平台记录的这一版不支持当前实例 |
| `loader_mismatch` | 声明（或平台记录）的加载器与实例不一致；Quilt 实例允许 Fabric 模组 |
| `duplicate_mod` | 同一个 mod id 出现在多个文件里（都启用是 problem，含停用文件是 warning） |
| `corrupt_file` | 读不到条目表、zip 坏、文件大小为 0、复制后哈希不符 |
| `unknown` | 没有可识别声明、读不到 mod id、范围无法解析、平台没认出哈希（平台查询没问成时**不**逐文件报这个） |

`mods.installFix` 的写入顺序：解析目标 → 下载到 `agent/staging/<事务>-<uuid>/` → 校验大小与 SHA-1 → 复制到工作区 `versions/<id>/mods/` 的临时名 → 再校验一次哈希 → 改名落盘。任何一步失败都清掉这次复制的文件，工作区回到调用前；单文件 ≤100 MB、单事务 ≤500 MB、≤10 个文件。目标文件名、下载地址与哈希全部来自平台记录，不接受任意 URL。

限制：平台只覆盖 Modrinth 上认得出的文件；`repairCore` 不保留旧副本，失败按官方哈希重下恢复。

#### 9.4.6 依赖感知二分（Stage 5.5）

`mods.bisect` 在工具内部完成多轮启动，不让模型逐步调 `mods.setEnabled` / `instance.testLaunch`（也绕开单轮 8 次工具调用）。纯状态机在 `src/agent/AgentModBisect`，启动、重置工作区与清理在现场层（`AgentModToolEngine`）。

| 项 | 行为 |
| --- | --- |
| 前置 | 必须有活动事务与登记工作区；没有就拒绝。每次计划单独审批 |
| 候选 | 工作区 `mods/` 里启用的模组；加载器/API 前置（`fabric-api`、`forge`、`neoforge`、`quilt_loader` 等）始终启用，任何被保留模组的 required 前置跟着启用（`respectDependencies=false` 可关） |
| 优先 | `prioritizedCandidates`（一般来自 `diagnostics.crashSnapshot` 的嫌疑）排最前，先单独试一轮；其余按最近修改时间、文件名排序 |
| 每轮 | 按集合改 `.jar` ↔ `.jar.disabled`、把 `config/` 与 `options.txt` 恢复到二分开始时的快照、清掉 `logs/`、`crash-reports/`、`hs_err_pid*`，再启动一次（里程碑 `main_menu`，超时 10–600 s） |
| 判定 | 进程崩溃或出现新崩溃报告算"复现"；到主菜单算"不复现"；进程没起来算基础设施失败，连续 3 次就停 |
| 结果 | `single` / `pair` / `set` / `not_found`（停用全部候选仍崩）/ `not_reproducible`（基线不崩）/ `inconclusive`（确认不一致或连续启动失败）；`culprits` 只列责任模组，`minimalSet` 含被依赖带进来的前置 |
| 收尾 | 恢复模组启停与 `config/`、`options.txt`，删掉快照目录；只留每轮证据（目的、启用数、是否崩溃）与结果 |

上限：默认 8 次、最多 20 次启动；默认 45 分钟；每次启动 10–600 s；`repeatPerCandidate` 1–3。启动次数或时间用完就停在当前最小集合并标 `budgetExhausted`。二分不下载、不改世界、不写原实例。

限制：二分只判断"哪些模组一起启用会崩"，不做修复；确认阶段出现不一致时最多加跑一轮，仍不一致回 `inconclusive`，不猜责任模组。

#### 9.4.7 Auto-safe 事务授权（Stage 5.7，D12）

用户显式打开后的一次性事务授权：提交并批准计划即视为同意"按计划执行到事务结束"。实现分两层——`AgentAutoSafe::Authorizer` 记范围/时效/启动次数/审计（纯记账 + 判定），`AgentCompositeToolEngine` 在 `ApprovalRequired` 时替用户点"允许"（范围内）或直接拒绝（硬禁止）。

| 项 | 行为 |
| --- | --- |
| 打开 | 只在崩溃急救/模组维护显示开关；需要先锁定实例；默认关闭，不随会话记忆 |
| 范围 | 已批准计划里的写工具、`fs.write` 路径、`settings.set` 键、`mods.setEnabled` 文件名、`mods.installFix` 项目号，加 `repair.begin/verify/finish/diff/status` 与启动测试/二分 |
| 时效 | 60 分钟；启动次数 20（`instance.testLaunch`/`repair.verify` 按 `repeat` 计，`mods.bisect` 按 `maxLaunches` 计）；到期或用满退回逐次确认 |
| 硬禁止 | `settings.set` 的 `java.path` / `args.extraJvm`；`fs.write` 的 `saves/` 与带 `..` 的路径；`backup.restore`；`instance.repairCore`；不在本事务工作区的写入 |
| 自动回滚 | `repair.verify` 失败时事务工具自己调 `repair.finish(rollback)`，把回滚结果并进验证结果 |
| 失效 | 换模式、换会话、关窗、事务结束（commit/rollback）、到期；关窗时停测试进程 |
| 审计 | 事务结束写 `agent/transactions/<id>/audit.json`：范围、限制、实际操作、启动次数、验证、最终状态、回滚结果；同时挂进 `repair.finish` 的结果数据 |

限制：Auto-safe 只跳过"写操作逐次确认"，不跳过外发确认（D11）与审批矩阵里的其它类别；`mods.bisect` 的一次授权按它的启动上限占预算，不做更细的逐轮记账。

### 9.5 联网与固定诊断命令

| 工具 | 审批 | 关键上限 |
| --- | --- | --- |
| `web.fetch` | 每次（或本会话同一 host 已放行） | 只 http/https；全部 A/AAAA 必须公网（拒绝 loopback/私网/链路本地/组播/保留，含 `::ffff:`）；连接已校验 IP，Host 与 TLS 主机名仍用原 host；重定向 ≤5 跳且每跳重检；强制直连（不继承系统代理）；文本类 Content-Type；响应 2 MB、正文 20 万字符；超时 20 s |
| `web.search` | 每次（或本会话 SearXNG host 已放行） | 只支持 SearXNG（设置里填地址）；查询 ≤400 字符；≤20 条结果；摘要遮凭据 |
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

每个模式都有一份**显式工具白名单**（S5.6），白名单在每个模式下都生效；技能只能缩小工具集合，改不了审批/外发策略。末尾带 `*` 的条目按前缀放行，只有 `mcp__*` 用到。

| 模式 | 白名单大致范围 | 不含 |
| --- | --- | --- |
| 自由对话 | 文件工具、只读诊断、设置/模组启停/备份、联网与诊断命令、`mcp__*` | 事务（`repair.*`）、`instance.testLaunch`、`mods.installFix`、`mods.bisect`、`instance.repairCore`、`workflow.*` |
| 实例体检 | 只读诊断 + 分析/校验 | 一切写工具、启动测试、事务 |
| 崩溃急救 / 模组维护 | 只读诊断 + 分析与校验 + 写工具 + 事务/启动测试/修复/二分 + `workflow.*` | — |
| 版本升级 / 移植 | 只读诊断 + `backup.create` + 升级入口 | 设置写、事务、启动测试 |

模式记在会话 `skill` 字段；换模式作废旧计划、证据保留。联网与诊断命令只在自由对话可用；修复工具只在崩溃急救/模组维护里放行。Auto-safe（D12）是修复类模式里的一个开关，不是第六个模式；换模式/换会话即关。

### 11.2 工作流

- **实例体检**：锁定实例 → 启动诊断 → 日志/模组/完整性 → 分级结论 + 证据 + 下一步选项；不写盘。
- **崩溃急救**：`diagnostics.crashSnapshot` 拿环境、阶段、异常链与命中规则（需要行号时再 `logs.search`，原文走 `crash.readFull`）→ 按 `matchedRules`、嫌疑的 role/置信度与证据决定下一步（类型清单在知识库，不写进提示词，也不猜具体模组）→ 计划 → `repair.begin` 建事务 → 逐步执行（事务内写工作区、每步单独审批、失败即停）→ `repair.verify` 在隔离工作区验证 → `repair.finish(commit/rollback)` → 报告写回退方式与未解决项。开了 Auto-safe 时计划批准后的写步骤不再逐个确认，验证失败自动回滚；世界问题只在副本世界验证，不打开源存档。
- **版本升级 / 移植**：`upgrade.inspect` 只读检查 + 解释风险；执行一律打开现有升级向导。
- **模组维护**：列清单 → `mods.analyzeCompatibility` 读声明查加载器/版本/缺前置/重复 → 需要逐文件哈希时 `mods.verifyFiles` → `repair.begin` → 在工作区启停/备份，或用 `mods.installFix` 按平台记录换掉损坏/不兼容的文件 → `repair.diff`/`repair.verify` → 提交或回滚。核心文件损坏走 `instance.repairCore`；说不清是哪几个模组时用 `mods.bisect` 在工作区二分（嫌疑来自 `diagnostics.crashSnapshot`）。

### 11.3 工作流状态机

```
Collect → AwaitPlanApproval → Execute*（每步审批）→ Verify → Report
                    ↘ 用户拒绝 → Collect        ↘ 某步失败 → Failed（写工具全锁）
```

- `workflow.submitPlan`：结构化 `{goal(≤500), steps[{tool, arguments, reason(≤300), verify}]}`；本地校验工具在白名单、参数过 schema、只读不许写、至少一个修复步与一个验证步（验证步必须只读）、步数 ≤20。
- 计划按数组顺序执行；不是当前这一步的写工具被拒绝并点名该做哪一步；批准计划 ≠ 批准写入。
- `workflow.finish`：`report(≤8000)` + `unresolved(≤2000)`；本地核对计划已批准、每步有结果、跑过验证步、必需工具碰过、至少一条证据；缺一项拒绝，失败过不能报完成。

### 11.4 核心提示词

提示词分三层（S5.6 Prompt V2）：技能提示词（角色、阶段、工具使用顺序、审批/验证/回滚要求）在 `assets/agent/prompts/`；诊断知识（崩溃签名、规则 id、置信度、建议验证）在 `assets/agent/knowledge/*.json`；工具在 `src/agent/`。提示词不背崩溃类型表，只引用当期白名单里存在的工具。

`AgentPromptLibrary` 从 Qt 资源 `:/agent/prompts/` 加载；`AgentSkill.cpp` 只留技能 id、显示名、工具白名单、只读/计划标记和提示词 id。组合顺序为 `common.md` + 技能文件 +（崩溃急救/模组维护）`repair_protocol.md`，用空行连接。每个技能带 `promptId`、`promptHash`、`promptVersion`；资源缺失、为空、不是 UTF-8 或超过预算时技能不可用，发送前报错，不回退成空提示。

| 预算 | 上限 |
| --- | --- |
| `common.md` | 4 KB |
| 单个技能文件 | 8 KB |
| `common + 技能 + repair_protocol` | 16 KB |

超预算的规则进知识库或工具。`tools/check-agent-release.ps1` 核对文件大小与“提示词里没有知识库签名”；`verify_agent` 核对组合大小、工具引用都在白名单内。

崩溃急救的目标定义是**执行器**：在锁定实例里尽可能自主完成诊断、修复、验证和回滚，工具和权限能做的直接做，不把人工操作步骤当结果；需要用户介入时说明缺的是工具、权限还是外部信息。六类条款（执行优先、验证、回滚、证据、禁止猜测、审批边界）与五个报告状态（`fixed_verified` / `fixed_unverified` / `mitigated` / `not_fixed` / `needs_user`）都写在 `crash_rescue.md` 与 `repair_protocol.md` 里，由 `verify_agent` 逐条断言。

共享规则（`common.md`，对所有技能生效）原文：

> 你在 WTFML 启动器里操作**本会话锁定的那个实例**，只能调用当前模式白名单里的工具。工具和权限允许做的直接做完，不要把“请用户自己去点”当成结果；做不到时说明缺的是工具、权限还是信息。
>
> 没有用工具读到的文件、日志或实例信息，不要假装读过；不知道就说不确定。任何凭据、令牌、账户信息都不要索要或复述。每条结论引用证据 id（E1、E2…）；拿不到证据就写“无法确认”并说明还需要什么。
>
> 不猜：模组责任、兼容版本、依赖、下载地址、退出原因都只能来自工具结果或用户明确给的信息。审批、外发、路径和下载来源策略不是你能绕开的：需要确认就停下来问，被拒绝就按拒绝处理，不换一条路绕过。

工具描述与参数 schema 在各自引擎的 `specs()` 里（文件工具 `AgentToolRegistry.cpp`、启动器 `AgentLauncherTools.cpp`、工作流 `AgentWorkflowEngine.cpp`、联网 `AgentWebTools.cpp`、命令 `AgentCommandTools.cpp`、模组与二分 `AgentModToolEngine.cpp`）。系统提示作为 `messages` 第一条发出，role 按 `systemRole` 适配；控制器每轮只追加一行「当前工作流状态（本地记账，不要复述给用户）：…」。提示词是模型输入，不是界面文案，不包 `tr()`。

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
| 单轮工具步数 / 单会话总数 | 8 / 40（总数跨回合、跨重启按会话累计；被上限挡住的那次不执行也不计数） |
| 重试 / 模型请求总时长 | 2 次 / 10 分钟（可取消） |
| 工具超时 | 文件 30 s、MCP 120 s、命令 60 s、联网 20 s |
| 测试启动 | 1–600 s，观察 0–60 s，重复 1–3 次；工作区记录/日志在本地 |
| 崩溃快照 / 崩溃报告全文 | 报告 8 MB、输出 16 KB（全文 24 KB）；规则命中 ≤8、嫌疑 ≤8；置信度 ≤0.95 |
| 日志检索 | 搜索 ≤200 条 / 结果 ≤24 KB 字符 / 扫描 6 s；按行读取 ≤500 行 / 256 KB |
| 模组兼容 / 校验 | 2000 个文件、本地 60 s；平台每批 100 个哈希、总 180 s；单文件哈希 512 MB |
| 模组修复 / 核心修复 | 单文件 100 MB、单事务 500 MB、≤10 个文件；核心修复 10 min |
| 依赖感知二分 | 默认 8 次、最多 20 次启动；默认 45 min；每次启动 10–600 s；连续 3 次启动失败就停 |
| Auto-safe | 60 min、20 次启动；一次只有一个授权；不随会话记忆 |
| 修复事务 | 范围 5 类；基线文件 ≤20000 个、总大小 ≤512 MB；单实例同时 1 个活动事务；提交备份在 `agent/transactions/<id>/backups/` |
| 目录列表 / `fs.read` / `fs.write` | 2000 项 / 256 KB（最大 1 MB）/ 1 MB |
| artifact 单会话配额 | 64 MB（按会话目录密文总字节；0 表示不限，只在测试里用） |
| 修复病例库 | ≤200 条、每条 ≤12 个动作标签与 ≤12 条路径明细；症状键 ≤160 字符；默认关闭 |
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
- 工具与安全：路径攻击矩阵、句柄级校验、审批矩阵、D11 边界表、artifact 分离、实例锁、备份生命周期、诊断投影、工作流闸门、MCP 协议/确认闸/假服务器、联网地址策略（假传输）、诊断命令、测试启动参数/审批/里程碑、工作区登记与清理、事务基线/差异/设置增量/审批、崩溃规则加载与坏规则跳过、崩溃报告解析、嫌疑角色与置信度、mixin 配置归属、日志行索引与按行读取、模组声明解析、兼容性判定（加载器/版本/缺前置/重复/损坏/平台结论）、修复工具的审批与失败不改工作区、二分的缩减/依赖保留/组合冲突/不可复现/不一致/预算与工作区恢复、提示词 V2（六类条款、五个报告状态、预算、无崩溃签名、工具引用都在白名单内、自由对话不放行修复工具）、Auto-safe（范围/时效/启动预算/硬禁止/审计落盘、复合层替用户点允许、验证失败自动回滚）。
- UI/集成：页面几何与状态、模式胶囊、工具卡/审批卡、真窗口里只读模式拒绝写、MCP 管理器接线、实例目录日志/崩溃报告读取。
- 性能基线：2000 条会话读写/加密、上下文裁剪、智能体页 2000 条消息（只摆末尾 200 条，数字见 perf-notes）。
- 会话与配额：单会话工具调用总数（跨回合累计、切会话带过来的计数、超限不执行）、artifact 单会话配额（写到上限就停、已写内容仍可读、0 表示不限）。
- 联网会话放行：审批卡给出「本会话允许」、同一 host 不再逐次确认、换 host 与换会话仍要审批。
- 日志字段投影：启动参数的账户字段、MAC、SID、硬件标识字段、点名账户名的整词替换与短名字豁免。
- 修复病例库：默认关闭时空转、落盘与读回、同键合并计数、本地排序、结果更新、键与条数上限。

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
| A9 | 真机 1.20.1：`instance.testLaunch` 到主菜单、崩溃复现、超时；源实例文件哈希不变 | 工作区隔离、无孤儿进程、日志可读 |
| A10 | 真机 1.20.1：`repair.begin` → 工作区改模组/配置 → `repair.verify` → commit/rollback；提交前手改源文件 | 回滚后源文件哈希恢复；源文件被改时拒绝提交；提交中断可恢复 |
| A11 | 真机 120 模组整合包：`mods.bisect` 在 12 次启动内定位已知问题模组；双模组冲突报 `pair`；取消后无孤儿进程、工作区恢复基线 | 必需前置始终启用；不误报 `not_found` |
| A12 | 真实模型 + Mixin 夹具：先调 `diagnostics.crashSnapshot` 再决定是否二分；自由对话模式调不到修复工具 | 不直接提交“禁用日志里第一个模组”；被拒时说明切模式 |
| A13 | 真机打开 Auto-safe：一次批准计划后完成「禁用模组 → 测试启动 → 提交」不再逐步弹审批；计划外改 `options.txt`/`args.extraJvm` 被拒；验证失败自动回滚；取消/超时/关窗停进程并留审计 | 审计文件完整；事务只动工作区 |

限制：A1–A10 需要真机与真实账号/服务，记录写进 [ACCEPTANCE_LOG](../archive/ACCEPTANCE_LOG.md)（一行：日期｜做了什么｜结论｜意外发现）。

## 14. 现状、未覆盖与剩余计划

已实现：聊天底座、文件工具、启动器工具、工作流、stdio MCP、联网与固定诊断命令、模式与证据、提示词资源化（Prompt V2）、隔离测试启动、修复事务、崩溃快照与日志检索、模组兼容与文件修复、依赖感知二分、Auto-safe 事务授权、发布检查脚本。核心计数与最近一次自动化结果见 [TEST_RUN_HISTORY](../archive/TEST_RUN_HISTORY.md)，Stage 5 的交付与代码位置见 [AGENT_AUTONOMY](../dev/AGENT_AUTONOMY.md)。

### 14.1 自主修复能力与缺口

闭环已覆盖"诊断 → 计划 → 事务 → 隔离验证 → 提交/回滚 → 报告"，模组侧还能读声明、按平台修文件、自动二分。仍按故障类型看：

| 故障 | 现有能力 | 缺口 |
| --- | --- | --- |
| Java 版本不匹配 | `java.list`、`settings.set` 改 Java 或自动选择；`instance.testLaunch` 可在工作区验证；`repair.*` 可提交/回滚 | 测试启动不自动下载 Java；提交后不在原件自动验证 |
| OOM | `diagnostics.crashSnapshot` 区分堆、Metaspace、Direct Buffer 与 native thread 并给不同建议；`memory.suggest`、`settings.set` 改 Xmx/Xms；可测试启动验证；可事务提交 | `memory.suggest` 仍只按堆内存给数；不判断 native 模块归属 |
| 缺模组前置 | `mods.analyzeCompatibility` 读声明与已装依赖图，指出缺的 mod id 和是否只有停用文件；`mods.installFix` 按平台记录补齐必需前置 | 只按声明与平台记录判断，不管 jar-in-jar 里的隐藏依赖 |
| Mod 版本冲突 | `mods.analyzeCompatibility` 对照实例的游戏/加载器/Java 范围与平台记录；`mods.installFix` 换掉不兼容的文件 | 没有社区兼容矩阵，不做多模组组合验证（S5.5 二分） |
| Mod 文件损坏 | `mods.verifyFiles` 查 zip 结构/大小/SHA-1；平台按哈希找回候选；`mods.installFix` 下进工作区 | `instance.integrity` 仍不覆盖 `mods/` |
| 配置文件错误 | `fs.read` / `fs.write` 白名单 | 不解析配置结构，只能靠备份整体回退 |
| Mixin 冲突 | `diagnostics.crashSnapshot` 给异常链、mixin 配置归属候选；`mods.bisect` 可在工作区自动二分定位到责任模组（含组合冲突） | 二分不修复；确认不一致时回 `inconclusive` |
| 游戏库文件损坏 | `instance.repairCore` 走 `VersionManager::repairVersionFiles()` 重下客户端 jar、库和资源索引 | 共享库不保留旧副本，不能回到旧版本 |
| 显卡 / Native 崩溃 | `diagnostics.crashSnapshot` 认 `hs_err_pid` 与 `EXCEPTION_ACCESS_VIOLATION`，与内存/模组问题分开报；可测试启动复现 | 不解析 native 模块归属，不改驱动或渲染后端 |
| 世界 / 存档崩溃 | 有限的崩溃诊断；可复制世界到工作区用 `quickPlay` 验证；可在副本里二分 | 事务不提交世界/存档，也不做原件提交后验证 |

Stage 5 的八个阶段已于 2026-11-24 全部完成，P2 修复病例库于 2026-11-25 补齐（默认关闭，见 §9.4.3 与 §12），交付与代码位置见 [AGENT_AUTONOMY](../dev/AGENT_AUTONOMY.md)。剩余工作只有人工验收项与 Stage 3 未开放的工具，见 [ROADMAP](../ROADMAP.md)。

未覆盖：

| 项 | 状态 |
| --- | --- |
| 模组平台查询（CurseForge） | 未实现；S5.4 只接 Modrinth，不按名称/slug 猜项目 |
| 二分的不一致判定 | 确认阶段出现不一致最多加跑一轮，仍不一致回 `inconclusive`；不做统计意义上的 flaky 概率 |
| 二分时的世界 | 只按工作区 `quickPlay` 可选进副本世界；不验证存档一致性，也不判断世界崩溃与模组的归属 |
| 自由对话的模型行为验收 | 白名单与提示词在本地断言，真实模型是否按提示词先诊断后动手见 A12 |
| Auto-safe 的真机行为 | 本地夹具覆盖范围/审计/自动回滚；真实游戏里"一次批准跑完并提交"、关窗清理见 A13 |
| `mods.installFix` 的补丁式回滚 | 失败时只撤销本次复制的文件；已完成的事务由 `repair.finish` 回滚 |
| 病例库的跨实例迁移 | 存在本机一份，不按实例/游戏版本分桶，也不合并不同版本的统计；换机器带走需要手动拷 `agent/repair-cases/` |
| 病例库对模型的影响 | 只作为 `priorCases` 提示，不改变工具判据、不自动重放历史动作 |
| 日志字段投影的边界 | 只认固定形态（启动参数名、MAC、SID、几个硬件标识字段、本机账户名）；显卡/驱动/音频型号保留作证据，自造字段名不处理 |
| 长会话窗口 | 只保留末尾 200 条视图，更早的点按钮补一页；搜索/跳转到窗口外消息未做 |
| 资源搜索/安装/更新工具、Brave/Tavily、HTTP/SSE MCP、`command.run` | 未实现（见 [ROADMAP](../ROADMAP.md)） |
| 高 DPI/读屏/长路径/换用户解密/视觉套件 | 人工项，见 §13.3 |

剩余计划只在 [ROADMAP](../ROADMAP.md) 维护：G1B 复测、网络错误验收、A1–A13 中未做的人工项、Stage 3 剩余工具、其它列表与复杂页面的性能收口。

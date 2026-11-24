# WTFML 智能体（Agent）设计与开发计划

智能体是启动器内置的实例维护助手。本文是设计规格和阶段计划：决策（D1–D11）、接口、数据格式、阶段划分（Stage 1A–4）、验收门（G1A–G4）都以本文为准；**已实现的事实**以 [FEATURES.md](../FEATURES.md) 和 [Stage 1A](AGENT_STAGE1A.md) / [Stage 1B](AGENT_STAGE1B.md) 为准，未实现的入口、界面和工具在本文里按"计划"阅读。要改 D1–D11，先改本文和测试计划，再动代码。

## 实现状态

| 阶段 | 状态 | 自动覆盖 | 人工验收 | 详见 |
| --- | --- | --- | --- | --- |
| Stage 1A：Provider + Loop + 会话 + 基础聊天 | 已实现 | `verify_agent` / `ui-agent` / `main-window-agent` | G1A 除网络错误外通过；网络错误延后 | [AGENT_STAGE1A](AGENT_STAGE1A.md) |
| Stage 1B：文件工具 + 审批 + 外发策略 | 已实现 | `verify_agent` / `ui-agent` / `main-window-agent` | G1B 复测待做：B1/B2/B3 已修，R1–R4、R6、R7、W4/W5、A1、P4、P5 未完成 | [AGENT_STAGE1B](AGENT_STAGE1B.md) |
| Stage 2B：维护工作流 + 启动器工具 | 已实现 | `verify_agent` / `ui-agent` / `main-window-agent` | G2B 待人工验收 | [AGENT_STAGE2B](AGENT_STAGE2B.md) |
| Stage 2A：stdio MCP | 已实现 | `verify_agent` / `ui-agent` / `main-window-agent` | G2A 待人工验收 | [AGENT_STAGE2A](AGENT_STAGE2A.md) |
| Stage 3：联网与固定诊断命令 | 已实现（资源工具与 HTTP MCP 未做） | `verify_agent` / `ui-agent` / `main-window-agent` | G3 待人工验收 | [AGENT_STAGE3](AGENT_STAGE3.md) |
| Stage 4：收口与发布 | 已实现（自动化回归/性能基线/发布检查/文档） | `verify_agent` / `ui_check_agent` / `mw_check_agent` + core/window/network/ci-ui | G4 待人工验收 | [AGENT_STAGE4](AGENT_STAGE4.md) |

关联文档：[产品边界与现有约定](../FEATURES.md)、[安全边界](SECURITY_BOUNDARIES.md)、[测试与验证](../dev/TESTING.md)、[性能基线与测量](../dev/PERFORMANCE.md)、[注释规范](../dev/COMMENT_STYLE.md)、[未完成工作总表](../ROADMAP.md)、[Stage 3/4 验收清单](AGENT_STAGE3_4_ACCEPTANCE.md)。

---

## 0. 先看结论

### 0.1 一句话定位

智能体是**实例维护助手**：

- 观察实例事实、解释异常、制定维护计划，用户批准后执行受控操作；
- 支持辅助生成诊断脚本或解释配置；
- 所有改盘动作经过权限闸、路径闸和可回退设计，不绕过启动器已有的安装、升级、备份、占用和校验规则。

### 0.2 推荐方案

**原生 Agent Loop + 声明式技能工作流 + 独立审批闸**：

1. 底层是 OpenAI-compatible 的流式 tool-calling loop（模型调用工具、结果回灌、继续推理，与 Codex / Claude Code 同形）；
2. 体检、崩溃急救、版本升级、模组维护这些关键流程由内置技能（Skill）/ 工作流状态机给出步骤、工具白名单、证据要求和审批点，不交给模型自由发挥；
3. 模型只在步骤内做判断和解释，不能修改技能、权限、API 配置或审批策略；
4. 所有工具调用有超时、大小上限、路径限制、并发限制和可见失败；
5. API key / access token 优先放 Windows 凭据管理器（DPAPI 后备），默认星号遮挡，不进日志、会话文件或 URL。

备选路线（纯 ReAct、外部 CLI 智能体、纯向导）的取舍见 §2 D3。

Stage 1 拆成两段：**1A** 是 Provider + Loop + 会话 + 基础聊天 UI（无工具）；**1B** 是受控文件工具 + 审批 + 外发策略 + 安全测试。MCP 放 Stage 2 单独验收；Skill 在 Stage 1 只保留接口与注册表骨架，工作流引擎从 Stage 2 开始。

### 0.3 分阶段结论

| 阶段 | 目标 | 用户能做什么 | 不做什么 |
| --- | --- | --- | --- |
| Stage 1A | Provider + Loop + 会话 + 基础聊天 | 建会话、配 API、选模型、流式对话、停止/重试、会话恢复 | 不调用工具，不读文件/日志，不接 MCP |
| Stage 1B | 受控文件工具 + 审批 + 外发策略 | 读目录元数据，按次确认文件名/配置/其他文件，受控写配置 | 不接 MCP，不做维护工作流，不做任意命令 |
| Stage 2 | 维护工作流 + MCP（单独验收） | 体检、启动/崩溃诊断、按计划修复、升级接入、stdio MCP | 不做原始 shell，不做无人值守 |
| Stage 3 | 扩大工具面 | 联网搜索/抓取、固定诊断命令、资源维护、更多 MCP 形态 | 不做静默后台自动维护，不做跨用户数据 |
| Stage 4 | 收口验收 | 完整回归、真机/实网验收、文档与发布 | — |

### 0.4 审批

D1–D11 的结论见 §2；实现阶段按 G1A / G1B / G2A / G2B / G3 / G4 做人工验收。D2 / D7 / D8 / D10 / D11 会影响后续代码结构。

---

## 1. 目标与非目标

### 1.1 目标

1. **修异常**：从启动器日志、游戏日志、崩溃报告、Java/内存/模组/完整性事实中定位问题，给出有证据的结论和可执行的修复方案。
2. **维护实例**：在用户批准下修改配置、启停模组、创建备份、升级/移植实例、修复缺失文件。
3. **降低门槛**：把分散在实例设置、日志窗、崩溃报告、体检、升级向导里的信息用对话串起来。
4. **保留确定性**：能由现有纯函数/事务判定的不由模型猜；模型负责解释、排序、追问和组织步骤，不发明成功状态。
5. **可审计**：每次工具调用、审批、写入、回退都有可见记录，出错能说清在哪一步、动了什么。

### 1.2 非目标

- 通用代码助手：不把"写一个 Mod / 写启动器功能"作为默认能力。
- 无人值守的自动维护、定时巡检、后台自动改盘。
- 执行模型生成的任意脚本、任意 shell、任意下载后立即运行。
- 替用户判断：删除、覆盖、升级、回退等不可逆或高风险动作必须人工确认。
- 把 API key、Minecraft 令牌、账户凭据、完整私有路径发到模型或第三方。
- 承诺模型给出的兼容性结论：未知就是未知，要求更多证据或转人工。
- 新增 WebEngine、Python、Node 等运行时依赖；MCP 服务器由用户自己提供。

### 1.3 产品原则

1. **证据先于结论**：每条诊断结论指向工具结果/文件/日志中的具体证据。
2. **先计划后执行**：写操作先展示目标、差异、备份和回退，批准后才落盘。
3. **最小权限**：只读默认放行；写、删、装、升级、命令、外部 MCP 逐级加闸。
4. **失败可见**：网络、解析、工具、审批、写盘失败都在对话里显示，不只写日志。
5. **可恢复**：写前备份、原子替换、保留原状态；不能恢复的操作提前说清。
6. **不伪造**：没拿到 usage 就不显示精确 token/费用；没联网成功就不说"已检查"。
7. **隐私默认**：只发送当前会话需要的最小上下文；密钥和令牌永不发送。
8. **可关闭**：不配置智能体也能正常使用启动器其它功能，智能体故障不影响启动。

---

## 2. 设计决策

D1–D11 是已确认的设计结论；要推翻某一条，先改本文和测试计划，再动代码。

| ID | 问题 | 结论 | 备注/未采纳备选 |
| --- | --- | --- | --- |
| **D1** | 导航位置与启动默认页 | 五个选项卡顺序 **主页 / 管理 / 下载 / 设置 / 智能体**；启动默认**主页** | "智能体放第一"会破坏 `homeTabIndex()==0` 并增加导航/测试返工，不采用 |
| **D2** | 会话历史、导出、备份、删除与隐私语义 | **启动器级全局目录** `%LOCALAPPDATA%\WTFML\agent`；消息/元数据默认明文 JSON；多实例会话通过 `instanceLock` 隔离；导出/备份/删除/整会话加密语义见 §6.5–§6.7 | 不按游戏文件夹各存一份（换文件夹历史消失、聊天可能随整合包被分享）；不把大文件塞进 HKCU |
| **D3** | Agent 技术路线 | **原生混合**：Stage 1A/1B 只做 loop/上下文/会话/受控文件工具；**MCP 放 Stage 2**；不实现 Skill 工作流引擎，只保留 `AgentSkill` / 工具宿主接口 | 纯 ReAct 难限权；外部 pi/codex/claude code 子进程带来打包/权限/UI 集成依赖；纯向导不够灵活 |
| **D4** | 工作流在界面上的呈现 | 用户可见的"模式"，内部是编译进程序的 Skill；Stage 1A/1B 只有"自由对话"，Stage 2 再放模式卡片/斜杠命令 | 只做斜杠命令不可发现；每个工作流单独一页太重；全塞进自由对话提示词无法限工具 |
| **D5** | 工具自动执行边界 | 只读自动；写/删/升级/命令/外部 MCP 逐次确认；**备份创建也必须审批**；不提供"全自动"模式 | 不做"本会话自动批准同类写操作" |
| **D6** | 模型/API 兼容面 | **OpenAI-compatible 基础兼容子集**；允许 provider 配置少量枚举化协议适配项（认证头、流式、`max_tokens` 字段名、system role、reasoning 回传等），不允许任意 header/URL 模板/脚本 | 不同时做 Anthropic Messages API（Stage 3 可选）；不"只做 DeepSeek" |
| **D7** | MCP 范围与风险 | **Stage 1 不实现 MCP**；Stage 2 只做 **stdio MCP 并单独验收**；服务器**首次启动、配置修改、重新连接**都必须用户确认并告知其具有当前 Windows 用户权限；确认卡展示可执行文件路径/SHA-256/参数/工作目录/环境白名单，文件身份变化后重新确认；所有 MCP 工具默认逐次审批 | 不做 HTTP/SSE（Stage 3 再开）；不自动重启/重连；不信任服务器声明的只读注解 |
| **D8** | 智能体操作哪个实例 | 会话**首次执行实例操作时自动锁定实例**；锁定后右上角芯片不再跟随主页切换；切换实例必须显式确认；工具不接受模型传入的实例 id | 不"始终跟随主页选中实例"（讨论 A 时切到 B 会误操作 B）；不"每次工具前弹选择"（打断过多） |
| **D9** | Stage 3 命令工具 | 第一版只开放**固定诊断命令 + 参数白名单**（`java -version`、`ping`、`nslookup`、`netstat` 等）；任意可执行文件调用是单独的高风险能力，需单独授权、默认关闭 | 不直接开放任意命令行；不把"专家模式"作为默认能力 |
| **D10** | Session 删除与文件维护备份的生命周期 | 删除 Session 默认**不删除**它产生的备份；备份有独立清理策略；删除整个 Agent 数据时清理会话/artifacts 和 `owner=agent` 的备份，`owner=launcher/user/shared` 的备份只删 Agent 引用记录、实际文件单独确认（API key/MCP 配置在同一流程里单独确认）；备份保留 `sourceSessionId` / `owner` / `source` 元数据，来源 Session 已删除也能恢复 | 不把备份绑在 Session 生命周期上 |
| **D11** | 模型上下文的数据外发范围 | 标准模式：**脱敏日志 / 脱敏崩溃报告 / 锁定实例元数据 / 必要目录元数据自动读取**；`fs.list` 不返回完整文件名；全局实例仅总数，完整列表逐次确认；诊断结果只走字段白名单；**完整文件名清单 / 游戏配置文件 / 任意其他文件逐次确认**，配置不提供会话级放行；**凭据文件/凭据存储禁止读取/发送**；普通文本中偶然出现的凭据字段仅本地扫描后发送脱敏片段，绝不发送原值；`artifact.read` 每次重新过 D11，不能读取原始存档。自动读取也要明确告知"可能发送给当前配置的第三方模型服务"；脱敏按内容字段过滤，不只看文件名；限制目录/文件类型 | 只读≠可外发；不把完整数据根/账户目录交给模型；文件名不是唯一脱敏判据 |

### 2.1 参考设计与取舍

只借结构，不照搬产品目标：

| 参考 | 核心设计 | 借鉴 | 不照搬 |
| --- | --- | --- | --- |
| Codex / OpenAI Agents | tool-calling loop、工具 schema、沙箱/审批、流式事件 | Agent Loop、工具注册/校验、错误回灌、单飞与上限 | 通用编码/仓库操作 |
| Claude Code | Plan Mode、权限提示、Skill/斜杠命令、上下文压缩、工具结果落盘 | 先计划再执行、逐次审批、技能白名单、artifact 摘要 | 编码工具链、IDE/终端工作流 |
| pi | 小型核心 loop + 扩展/技能注册、provider 适配 | 分层边界、provider 抽象、可测试的纯函数 | 远程加载扩展（技能直接编译进程序） |
| Deep Research 类 | 先规划、再检索、逐条引用证据、综合 | 证据模型、引用、无法确认时停止 | 长时间无人值守、海量网页抓取 |
| Cline / Roo Code 类 | 工具调用卡片、diff 预览、审批按钮 | 工具卡/审批卡/差异预览交互 | 以 IDE 插件为中心、自动执行大量编辑 |

组合是：Codex 式 loop + Claude Code 式审批/技能 + Deep Research 式证据 + pi 式分层。

### 2.2 由这些结论派生的默认设计

- 模型选择是**会话级**，默认继承上次选择；切换模型不静默换 provider，也不自动降级到另一个 provider/model；401/配额/网络失败让用户看见。
- 智能体页面**不显示 Minecraft 账号登录**，账号仍只在主页/账号与外观里。
- 会话标题默认取第一条用户消息前 24 个字符，可手动重命名。手动改过名的会话发第一条消息时不再被自动名覆盖；手动改成「新对话」等于没起名，仍会自动起名。
- 点「+」后光标直接进输入框（当前已是空会话、没有再新建时也一样）。
- "本次会话允许"只对 `web.search/fetch` 这类联网读取开放；文件读取、配置、凭据、写、删、升级、命令、MCP 都没有会话级放行。
- 会话首次涉及实例的工具调用后锁定实例，之后不再读取主页当前选择。
- 文件维护备份与会话生命周期解耦，备份元数据保留来源会话信息。
- 自动读取与外发范围由 D11 裁决：只读不代表可外发，由内容敏感度统一判定。

---

## 3. 总体架构

### 3.1 分层

`ui/Agent*` → `AgentController` → `src/agent/`（loop/context/session/provider/policy）→ 现有网络与业务层。核心层不直接持有 `VersionManager` / `MainWindow`。

### 3.2 依赖规则

- `ui/Agent*` 只 include `ui/` 与值类型，不 include `src/`。
- `src/agent/` 不 include `ui/` 或 `MainWindow`；业务能力通过 `AgentToolEngine` / 工具宿主接入。
- 网络只走 `AgentModelClient`；凭据只走 `AgentSecretStore`；会话落盘只走 `AgentSessionStore`。
- `AgentController` 是唯一同时认识 UI 与核心层的类。

### 3.3 关键模块

| 模块 | 职责 |
| --- | --- |
| `AgentTypes` / `AgentSession` | 消息、工具调用、结果、错误、用量、会话元数据 |
| `AgentSettings` / `AgentPaths` | 非密 provider 配置；数据目录唯一出处 |
| `AgentSecretStore` / `SecretProtector` | Windows 凭据管理器 + DPAPI 后备 |
| `AgentSessionStore` / `AgentSessionCrypto` | 原子读写、快照、损坏隔离、整会话加密 |
| `AgentRedaction` / `AgentEgressPolicy` | 内容脱敏；自动/确认/阻断 |
| `AgentSseParser` / `OpenAiCompatibleClient` / `AgentProviderAdapters` | SSE、请求构造、协议适配、错误分类 |
| `AgentContextBuilder` / `AgentLoop` | 预算裁剪；状态机、单飞、审批暂停、工具回灌 |
| `AgentToolEngine` / `AgentFileTools` / `AgentWinPath` | 工具 schema、文件工具、句柄级路径校验 |
| `AgentArtifactStore` / `AgentBackupIndex` | 模型可读/本地原始 artifact；维护备份 |
| `AgentInstanceLock` | 首次锁定、切换确认、游戏根校验 |

### 3.4 线程、异步与生命周期

- UI 更新在主线程；网络用异步 `QNetworkAccessManager`。
- 同一会话最多 1 个模型请求、1 个工具；工具串行。
- 异步回调带会话/请求代次；取消、切换会话、删除后旧回调只丢弃。
- 不使用嵌套事件循环；关窗有界取消，不卡退出。

## 4. 界面设计

- 顶部导航：主页 / 管理 / 下载 / 设置 / 智能体；启动默认主页。
- 智能体页双栏：左侧会话栏，右侧聊天区。
- 会话栏：新建、搜索、列表；每行可重命名、固定、加密存储、导出 Markdown、删除。
- 聊天顶行：标题、API 状态胶囊、实例胶囊、`⋯`、API 设置。实例胶囊未锁定时跟随主页，首次实例工具调用后锁定。
- 消息区：用户气泡、助手 Markdown、推理折叠、错误/中断、工具卡、审批卡。
- 输入区：Enter 发送、Shift+Enter 换行；发送/停止；空态说明隐私和外发范围。
- 加密存储在会话 `⋯` 菜单；标题和时间仍为明文。
- 严格模式开关在 API/模型设置页。
- 主题、尺寸和无障碍使用现有 Theme 令牌与 objectName 测试入口；Stage 2 的模式卡片/MCP 设置见 §14。

## 5. API 管理与网络安全

### 5.1 Provider 抽象与基础兼容子集

采用 OpenAI-compatible：`POST <base>/chat/completions`；`messages`、`tools`、`tool_choice:auto`；流式 SSE 或非流式 JSON。支持 `choices[].delta.content`、`reasoning_content`、`tool_calls`、`finish_reason`、`usage`、`data: [DONE]`。

协议适配只允许枚举：

| 适配项 | 取值 | 默认 |
| --- | --- | --- |
| `authMode` | `bearer` / `api-key-header` / `none` | `bearer` |
| `stream` | `on` / `off` | `on` |
| `maxTokensField` | `max_tokens` / `max_completion_tokens` | `max_tokens` |
| `systemRole` | `system` / `developer` | `system` |
| `reasoningEcho` | `display-only` / `send-back` | `display-only` |
| `temperature` | `send` / `omit` | `send` |
| `toolChoice` | `auto` / `omit` | `auto` |

不支持 Responses API、Anthropic content blocks、任意 header/URL 模板、请求体脚本、响应正则。工具线上名必须匹配 `^[A-Za-z0-9_-]+$`；内部 `fs.read` 映射为 `fs_read`。

### 5.2 API Key / Access Token 存储

API Key 只落 Windows 凭据管理器（`WTFML/Agent/<profileId>`），主路径失败时用 DPAPI 后备；两条路都失败拒绝保存并保留旧值，不降级为明文或 QSettings 明文。界面默认星号遮挡，不主动回显；Key 不进日志、会话、错误、导出或 URL。

### 5.3 网络边界

Base URL 只接受 `https://`；loopback `http://` 需显式勾选并持续提示；拒绝 userinfo、片段、查询串。不跟随重定向，不忽略 TLS 错误。响应总额 16 MB、SSE 单行 1 MB。模型 API 遵循 Qt/系统代理；Stage 3 的 `web.fetch` 不走系统代理。

### 5.4 提示注入与数据外发

工具结果、文件、日志、网页都按不可信数据包裹；系统提示明确其不是用户指令。模型不能修改系统提示、工具白名单、审批策略或 API 配置。写操作审批由本地策略生成，不采信模型自称的批准。

### 5.5 错误分类与用户文案

统一分类：`NoConfiguration`、`Unauthorized`、`RateLimited`、`Quota`、`Network`、`Protocol`、`ContextOverflow`、`ToolError`、`EgressApprovalRequired`、`EgressBlocked`、`ApprovalDenied`、`Cancelled`、`Internal`。401/403/配额不重试；首字节前网络/429/5xx 最多 2 次；流开始后失败保留部分文本、不自动重发。

### 5.6 模型上下文的数据外发范围（D11）

#### 5.6.2 默认边界

| 数据类型 | 默认行为 |
| --- | --- |
| 脱敏后的日志、崩溃报告、锁定实例元数据、必要目录元数据、全局实例总数 | 自动读取；首次使用提示「可能发送给当前模型服务」 |
| 全局实例完整列表、游戏配置文件、完整文件名清单、其他文本 | 逐次确认；确认后只发脱敏片段；没有会话级目录放行 |
| 凭据文件/凭据存储、二进制内容 | 硬阻断 |
| 严格模式 | 自动读取类别改为逐次确认；硬阻断不变 |

#### 5.6.2.1 当前脱敏方案

`AgentRedaction` 以内容为主、文件名为辅：

- 键名按 `_`、`-`、`.` 断开，小写/数字后紧跟大写再切一刀；`PASSWORD` 这类全大写串不拆词。
- 词根命中 `password/passwd/pwd/secret/token/credential/credentials/cookie/csrf/session/apikey`，或相邻词根组成 `api/key`、`key/api`、`private/key`、`access/key` 时，把值替换成 `<redacted>`，保留键名。
- 值少于 6 个字符不遮，避免误伤 `password = true` 这类说明文字。
- 固定形态覆盖 `Authorization: Bearer …`、`Bearer …`、`sk-…`、JWT（`eyJ…`）、AWS `AKIA…`、PEM 私钥块。
- 自动读取类别遮挡后发送，工具卡标命中处数；配置/其他文本先确认再发脱敏片段；凭据文件硬阻断。
- 凭据文件名按 dotenv 家族（`.env`、`*.env`、`*.env.*`）、证书后缀（`pem/key/p12/pfx/jks`）和 `credential`/`secret`/`token`/`apikey`/`api_key`/`api-key` 词根判定。
- 脱敏失败/不确定按更严格一档处理。

限制：词根表之外的自造字段名仍靠固定令牌形态兜底；日志里的用户名、启动参数回显、绝对路径和硬件指纹留给后续日志/诊断字段投影。

## 6. 会话、上下文与存储

### 6.1 存储位置

默认 `%LOCALAPPDATA%\WTFML\agent`。会话、artifacts、维护备份、MCP 配置分目录；测试用 `wtfml.testAgentDataDir` 隔离。

### 6.2 元数据与消息记录

`<uuid>.meta.json` 存标题、时间、固定、加密、skill、provider、instanceLock、messageCount、bodySha256、deletionState；`<uuid>.json` / `.enc` 存消息、summary、usage、pendingApproval。消息状态：`complete/streaming/interrupted/error/tool_pending/awaiting_approval`。

### 6.3 原子写、快照与损坏隔离

正文先写、元数据后写；`QSaveFile` 原子提交；稳定点保留最近 3 份快照；正文损坏改名隔离，不当空会话覆盖；启动时 `streaming` 恢复为 `interrupted`。

### 6.4 artifacts

模型可读 artifact 只存脱敏内容并 DPAPI 加密；本地原始存档 id 不在模型可见空间；`artifact.read` 每次重新脱敏。大文本 artifact 只存本次读取窗口，不是整个原文件。

### 6.5 多实例隔离与会话锁定

首次实例工具调用锁定当前实例并先落盘；锁定后不接受模型传入的实例 id；换游戏文件夹/实例删除返回 `InstanceUnavailable`；显式切换需确认。

### 6.6 导出、备份、删除

删除会话删正文/快照/artifacts/元数据，不删维护备份；审批请求不存「已批准」；活动会话先取消/等待；文件提交阶段拒绝强行中断。维护备份 `owner=agent` 保留 `sourceSessionId`，共享/外部备份只删引用。

### 6.7 本地隐私与整会话加密

默认明文 JSON，不含密钥；可选整会话 DPAPI 加密（`.enc`），标题和时间仍明文；加密失败保留明文并报错，解密失败不回退明文。加密只保护落盘，不改变发送给模型的内容。

## 7. 上下文管理

### 7.1 每轮环境快照

每轮生成只读环境快照：启动器版本、当前游戏根显示名、锁定实例摘要、最近启动/异常摘要、当前技能/工具/外发策略版本、会话预算。未锁定实例不假装已确定。

### 7.2 预算算法

保守估算 token：CJK 按 1，其他字符按 0.3；扣掉输出预留。优先级：系统提示/安全规则 → 当前技能工具 schema → 环境快照 → 最近完整轮次 → 工具结果摘要 → 更早摘要 → 省略标记。永远保留最后一条用户消息和进行中的 assistant/tool 配对；超预算先裁剪，仍超报 `ContextOverflow`。

### 7.3 压缩与摘要

当前使用确定性裁剪并插入「更早的 N 条已省略」；不做 LLM 摘要。原文仍保留在会话文件，只在请求上下文里省略。

### 7.4 消息转换

内部消息转 OpenAI：system 提示、user/assistant 文本、assistant tool_calls、tool 结果带 `tool_call_id`；推理字段按 provider 能力回传；工具结果用 JSON，含 `ok`、`summary`、`data`、`evidence`、`modelArtifact`、`truncated`。

## 8. Agent Loop

### 8.1 状态机

`Idle → Preparing → Requesting → AwaitingApproval / ExecutingTool → Completed / Failed / Cancelled`。同一会话不允许并行 send；有工具调用且需审批时暂停，用户决定后继续或把拒绝回灌。

### 8.2 一轮的步骤

校验配置/密钥/实例/技能 → 构建上下文 → 流式接收 → 合并文本/推理/tool_calls → 无工具则完成；有工具则校验名称/参数/风险 → 审批/执行 → 结果回灌继续。

### 8.3 重试与取消

首字节前网络/429/5xx 最多 2 次，退避 1s/4s，尊重 Retry-After；流开始后失败不自动重发。取消要 abort reply、清定时器、丢弃半截 tool_calls、停止工具、持久化部分消息、恢复可发送状态。

### 8.4 持久化时机

用户消息后、assistant 完成/中断后、每个工具结果后、审批创建/决定后、重命名/固定/删除后保存；流式每 2 秒最多一次。启动时把 `streaming` 恢复为 `interrupted`。

## 9. 工具系统

### 9.1 工具接口

工具声明名称、风险等级、JSON Schema、超时和幂等性；未声明风险按 `ExternalMcp` 逐次审批。参数校验失败不执行，返回结构化错误。实例由会话锁定提供，不写进工具参数。

### 9.2 风险与审批矩阵

只读工具自动；完整文件名清单/配置/其他文本逐次确认；`fs.write` 逐次审批并写前备份；命令/外部 MCP 逐次审批；没有会话级目录放行。审批请求持久化但不存「已批准」布尔，重启后作废。

### 9.3 Stage 1B 文件工具（已实现）

只对锁定实例生效，路径全部是相对路径：

- `fs.list` 只返回目录元数据，不返回文件名；
- `fs.listNames` 返回完整文件名清单，每次审批，最多 2000 项；
- `fs.read` 只读文本，默认 256 KB、最大 1 MB，二进制/凭据文件阻断，返回 sha256 与 `fileIdentityToken`；
- `fs.write` 只写 `config/**`、`defaultconfigs/**`、`options.txt`，必须带旧哈希与身份 token，写前备份、原子替换、写后回读；
- `artifact.read` 只读模型可读 artifact，每次重新脱敏；
- 路径校验使用字符串拒绝 + Windows 句柄级最终路径/文件 ID/reparse 核对；
- 维护备份 `owner=agent`，删除会话不删备份。

实现细节、当前边界和待人工测试项见 [AGENT_STAGE1B](AGENT_STAGE1B.md) 与 [SECURITY_BOUNDARIES](SECURITY_BOUNDARIES.md) §4。

### 9.4 Stage 2 启动器工具

状态：**Stage 2B 已实现**，19 个工具与 2 个工作流工具的现行行为、边界与待测项见 [AGENT_STAGE2B](AGENT_STAGE2B.md)；本节保留设计约定。

结论：包装现有能力，不重写业务判据；所有实例工具使用会话锁定的实例。

- `instance.list`：默认只返回实例总数（自动读取）；`detail="full"` 返回名称/版本/加载器，**逐次确认**，不锁定实例，不返回绝对路径/账户信息。
- `instance.context`：只返回当前锁定实例摘要；未锁定返回 `NeedsInstanceLock`；字段同 §5.6.2 的锁定实例元数据。
- `diagnostics.startup`：调用 `StartupDiagnosis::analyze()`，输入由现有事实组装，首次调用锁定实例；结果经 `AgentDiagnosticProjection` 投影后才能自动进入模型上下文。
- `diagnostics.crash`：调用 `GameExitDiagnosis` / 崩溃报告读取，只读，同样经投影。
- `logs.tail`：启动器日志/实例 latest.log 末尾，限行/字节，只读，按 D11。
- `crash.list/latest`：崩溃报告目录，只读，按 D11。
- `instance.integrity`：调用 `VersionIntegrity`，只读，结构化报告，路径/账户字段经投影，不自动外发。
- `mods.list`：调用 `InstanceFiles` 的扫描/分类，不解 jar 内容；按 D11 属于"其他文件"，逐次确认后才把文件名/启停状态发给模型，也可只给数量/分类汇总。
- `settings.get/set`：只允许白名单键（Java 选择、内存、启动后行为、JVM 参数等），经现有设置模型读写；每次写都审批、记录旧值和新值，失败不推进"已保存"基线；`settings.get` 默认只返回非路径字段。
- `java.list` / `memory.suggest`：复用现有 Java 扫描与内存建议纯函数，只读；`java.list` 只返回主版本/发行版等非路径字段。
- `config.read/validate`：文本配置读取与基本校验，读取按 D11 确认/预览，写走 `fs.write` 或配置专用工具。
- `backup.create`：调用 `BackupFiles`，**每次审批**，审批卡显示目标、范围、大小、保存位置、`owner/source`、清理策略、`sourceSessionId`；Agent 自建默认 `owner=agent` 写入 `backups/maintenance/`；在现有仓库新建的备份仍标 `owner=agent`，引用已有备份才标 `launcher/user/shared`。
- `backup.list`：只读，显示 owner/source，来源 Session 已删除也列得出来。
- `backup.restore`：调用现有恢复路径，要求审批 + 恢复前快照；不要求来源 Session 存在；共享/外部备份只引用，不改写原文件。
- `upgrade.inspect`：调用现有升级计划/兼容性报告，只读；`upgrade.openWizard`：由 `AgentController` 打开现有升级向导，不复制九类选择逻辑。
- `mods.dependencies`：调用 `ModDependencies` / 资源查询，网络只读，按 D11 外发说明。
- `resource.search`（Stage 3）：调用 `ResourceStore`，安装走现有安装控制器。
- `launcher.openPage`：只打开已有页面/入口（实例设置、日志、备份），不执行写操作。

长期任务（体检、依赖查询、升级）必须进入任务中心或对话内进度卡，可取消，失败保留报告，不与 `installingVersionIds()` / 文件占用冲突，不在 UI 线程做重 I/O。

#### 9.4.1 诊断结果字段白名单（`AgentDiagnosticProjection`）

`diagnostics.startup`、`diagnostics.crash` 与 `instance.integrity` 工具执行无需逐次手动审批，执行结果经纯函数投影并按字段白名单过滤后外发：

- **允许自动进入模型上下文**：实例 id/名称、真实版本、加载器名/版本、要求的 Java 主版本、实际使用的 Java 主版本（**不含路径**）、Xmx/Xms、模组数量、运行状态、发现项的 severity/code/title/hint（由启动器生成，不含路径/账户）、问题/提醒计数、OOM/缺前置等布尔事实、退出码/崩溃分类、证据 id。
- **本地脱敏或剔除**：Java 可执行文件绝对路径、数据根/实例目录/日志/崩溃报告绝对路径、OS 用户名、账户名/UUID、环境变量、主机名/服务器地址、原始 stack trace/原始日志片段。
- **超出白名单的字段**不进入模型上下文；诊断确实需要解释该字段时，走单独工具并逐次确认，或只给本地 UI/证据卡片。
- 投影发生在工具执行之后、写会话/发模型之前；不能把 `StartupDiagnosis::Report::toText()` 之类的整段文本直接塞进模型。
- 测试：绝对路径/账户字段被剔除；允许字段保留；投影后空结果不报成功；确认路径仍可单独读取。

### 9.5 Stage 3 网络与命令

`web.search`：provider 可选 Brave Search API、Tavily、用户自建 SearXNG（Stage 3 至少实现一种，配置与 key 走 §5.2）；查询串会发给第三方，首次使用需确认；结果标为不可信；不抓取搜索引擎结果页 HTML 充当搜索。

`web.fetch`：

- 只允许 http/https，拒绝 `file://`、`data:`、`ftp:` 等；
- 解析目标 host 的**全部 A/AAAA** 地址，任何一个落入私网/环回/链路本地/组播/保留地址就拒绝；
- **实际 HTTP 连接必须用已校验的 IP**：不能"校验时解析一次、连接时再解析一次"；为保留虚拟主机/TLS，连接 IP 时仍用原 host 的 `Host` 头和 TLS SNI/证书主机名校验（如 `QSslSocket::setPeerVerifyName`）；实现层若无法保证 pinned-IP，就不把 `web.fetch` 标成可防 DNS 重绑定；
- 每次重定向都重新执行完整解析 + 全地址校验 + pinned-IP 连接，不复用上一跳结果；
- **第一版不继承系统代理/Qt 代理，强制直连**：代理可能自行解析目标、绕过 pinned-IP 校验；以后支持代理必须单独设计并证明不绕过；
- 限制重定向次数、响应大小、Content-Type、超时；HTML 转文本、不执行 JS、不自动下载二进制、不跟随跨域到内网；
- 结果标为不可信并记录来源 URL/时间；内容同样按 D11 处理，不自动把内容里的凭据/本地路径带进上下文。
- 测试覆盖：多 A/AAAA 混私网、校验后 DNS 变化、连接目标 IP 不一致、重定向到私网、系统代理已配置、IPv6、证书主机名校验。

#### 9.5.1 第一版：固定诊断命令（`command.diagnose`）

只开放固定诊断命令 + 参数白名单：

- 命令定义是编译进程序的 `CommandSpec`，例如 `java -version`、`java -XshowSettings:properties -version`、`where java`、`ping <host>`（host 只允许域名或 IP，参数不进 shell）、`nslookup <host> [server]`、`netstat -ano`（固定参数）；
- 每个 spec 声明可执行文件、参数模板、允许的工作目录、超时、输出上限；参数过白名单/类型校验，不拼 shell 字符串；
- 每次执行都审批，展示完整命令行、工作目录、目标实例；默认关闭，设置里开启"诊断命令"后才可用；
- 用 Job Object 清理进程树，超时/取消/关窗走同一清理路径。

#### 9.5.2 任意可执行文件（`command.run`，高风险，单独授权）

- 第一版不开放；需要单独的"高风险命令"授权，不与 `command.diagnose` 共用开关；授权需要单独页面、明确风险说明、再次输入确认；默认关闭，更新后保持关闭；
- 只接受"解析后的绝对可执行文件 + 参数数组"，不经过 shell；逐次审批，展示命令行、工作目录、预计影响、环境变量白名单；
- 工作目录限定锁定实例；超时默认 60s、输出上限约 256 KB、非交互 stdin；进程树随超时/取消清理；退出码和 stderr 原样返回（脱敏后）；
- 不做"模型自己写脚本再执行"，写脚本需要用户先落盘/批准再作为命令执行；模型不能自己开启这个授权，也不能通过工具参数绕过。

### 9.6 证据模型

```json
{
  "id": "E1",
  "kind": "log|file|diagnostic|network|tool",
  "source": "logs/latest.log:120-140",
  "capturedAt": "2026-11-10T12:00:00Z",
  "sha256": "...",
  "excerpt": "..."
}
```

- 诊断技能要求最终报告至少引用一条证据；没有证据就输出"无法确认"并列出还需要什么。
- UI 工具卡显示证据徽标，最终消息里的 `[E1]` 可点击定位到卡片（Stage 2）。
- 证据只存本地会话/artifacts，不上传；发给模型时按上下文预算截断。

---

## 10. MCP（Stage 2 单独验收，只做 stdio）

状态：**Stage 2A 已实现**（stdio、确认闸、逐次审批、Job Object），现行行为、边界与待测项见 [AGENT_STAGE2A](AGENT_STAGE2A.md)；G2A 人工验收未做。HTTP/SSE 仍未实现。

### 10.1 配置

- 服务器条目：名称、绝对命令/解析后的可执行路径、参数数组、工作目录、环境变量白名单、启用开关、超时、信任级别（默认不信任）。
- **可执行文件身份**：确认卡和配置里记录解析后的绝对路径、文件 SHA-256、大小、mtime、卷序列 + 文件 ID（能取到时），并展示参数、工作目录、环境变量白名单；`configHash` 覆盖这些身份字段，不能只哈希命令/参数。
- 不自动下载/安装 MCP 服务器；不把 `npx` / `uvx` 当默认；Node/Python 服务器由用户自己提供解释器和脚本路径。
- 环境变量不默认继承全部（避免把账户密钥带进服务器）；允许显式添加，秘密值走 §5.2 的凭据存储（target `WTFML/Agent/Mcp/<serverId>/<envName>`），启动时传入，不写日志。
- 工作目录默认 agent 数据目录；可改成实例目录，但必须存在且不是可执行文件所在目录（避免相对路径劫持）。

**确认闸（D7）**：

- 服务器**首次启动**前确认，写明"该服务器以当前 Windows 用户权限运行，可能读写本机文件和网络；只添加你信任的服务器"，并展示可执行文件路径/SHA-256/参数/工作目录/环境白名单。
- 保存任何配置修改（命令、参数、工作目录、环境、启用开关）前确认，内容含差异摘要；保存后生成新的 `configHash`。
- **重新连接**（手动重连、崩溃后恢复、电脑休眠回来）前也要再次确认，不自动重连。
- 每次确认记录 `serverId + configHash + executableIdentity + confirmedAt`；首次启动和每次重连都在 UI 上显示"本机用户权限"警告。
- 启动/重连前重新计算可执行文件身份：路径存在但 SHA-256/文件 ID/大小/mtime 变化时旧确认失效、必须重新确认；文件缺失、不可读、身份计算失败则拒绝启动。
- 服务器 stdout/stderr 分开：stdout 只走协议，stderr 进脱敏日志。

### 10.2 协议

- JSON-RPC 2.0，stdio 传输，换行分隔 JSON。
- 握手：`initialize`（protocolVersion、capabilities、clientInfo；客户端采用自己支持的最高协议版本，至少 `2024-11-05`，以服务器返回的版本为准，不支持则报错）→ 收到 result 后发 `notifications/initialized` → `tools/list` 支持分页 cursor（最多 10 页）→ `tools/call` 处理 `isError` 和 content 数组；支持 `notifications/tools/list_changed`；服务器反向请求：`ping` 直接回，未知方法回 `-32601`，不实现的方法不挂起。
- 超时：握手 15s、list 15s、call 默认 120s，可配置但有上限；取消发 `notifications/cancelled`，超时/取消后按配置 kill 进程树。
- 进程：`QProcess` + Job Object；启动失败/崩溃/EOF 都把 pending 请求以错误结束，不永远等。
- 消息上限 8 MB/行；JSON 深度/对象数量限制；stdout 垃圾行计入错误但不断连（限次）。
- 服务器崩溃：**不自动重启**，标记断开、pending 请求以错误结束；用户手动"重新连接"并按 §10.1 再次确认；连续失败后停止尝试。
- 工具名 `mcp__<serverId>__<toolName>`，清理非法字符、限长、冲突拒绝，结果可映射回原工具。

### 10.3 审批与信任

- Stage 2 单独验收；Stage 1 不启动 MCP 进程、不暴露 MCP 设置页。
- 所有 MCP 工具默认逐次审批，即便服务器声明 `readOnlyHint`；不提供"信任只读注解自动放行"的开关（如后续要加，须单独审批并明确"注解由服务器自己声明，不可验证"）。
- MCP 工具参数也过 JSON Schema 校验；路径类参数由服务器自己解释时，仍提示用户它可能访问哪些目录。
- 不把 WTFML 的 API key/令牌通过环境或参数传给 MCP 服务器。
- 服务器生命周期控制与工具调用分别记录确认状态：启动服务器与执行特定工具各需独立授权。
- MCP 工具结果进入模型前同样过 D11 外发策略和脱敏。

### 10.4 测试

- 假 MCP 服务器（测试程序）覆盖：握手、分页、工具调用、isError、通知、取消、stdout 垃圾、stderr 日志、超时、崩溃、子进程残留；断言 `QProcess` 退出/析构后没有孤儿进程、Job Object 生效。
- 确认闸：首次启动必须确认；修改命令/参数/环境/工作目录后 `configHash` 变化、旧确认失效；可执行文件被替换（路径不变但 SHA-256/文件 ID 变化）必须重新确认；重连必须重新确认；崩溃后不自动重连；拒绝确认不启动进程；进程启动确认与工具调用审批保持独立；确认框含"当前 Windows 用户权限"提示和可执行文件身份。
- 工具结果进入模型前过 D11；MCP 返回的凭据形态内容被阻断/脱敏。

---

## 11. 技能与工作流

### 11.1 技能清单（编译进程序）

```cpp
struct AgentSkill {
    QString id;
    QString title;
    QString description;
    QString systemPrompt;
    QList<QString> allowedTools;
    QList<QString> requiredTools;
    bool readOnly;
    int maxSteps;
    QList<WorkflowStage> stages;
    QString completionCriteria;
    int version;
};
```

- 所有技能在代码里注册，版本随程序走；Stage 1A/1B/2 **不下载技能文件**（避免远程代码/提示注入）。
- Stage 1A/1B **不实现工作流引擎**：只注册 `AgentSkill` 结构和"自由对话"这一档（`src/agent/AgentSkill.*`）；`stages` / `completionCriteria` 先有类型、不执行状态机。Stage 2 才实现 Collect/Analyze/Plan/Approve/Execute/Verify/Report。
- 技能不能改审批策略，只能缩小工具集合；技能切换记录在会话里，旧工具结果保留、后续执行按新白名单；工作流开始前必须锁定实例，中途换实例按 D8 显式确认并作废旧计划。

### 11.2 Stage 2 工作流

#### A. 实例体检（只读）

1. 锁定/确认实例（D8；首次实例操作锁定，不在工作流中途换）；
2. 收集：实例元数据、Java、内存、mods 列表、完整性、最近日志、最近崩溃、磁盘空间（日志/文件按 D11 读取）；
3. 用现有诊断规则分类（Problem/Warning/Info/Ok）；
4. 每个结论附证据；
5. 输出"下一步可做什么"选项卡片（修复/升级/打开页面/导出报告）；
6. 不写盘。

#### B. 崩溃急救

1. 读取最近异常退出事实、日志尾部、最新崩溃报告；
2. 先匹配现有规则：OOM、Java 版本不匹配、缺前置、文件损坏、疑似模组冲突（只按现有规则，不猜具体模组）；
3. 输出"最可能 + 证据 + 不确定项"；
4. 修复计划（每步一个工具调用）：改内存/Java/配置 → `fs.write` 或设置工具（审批、备份、期望哈希）；修复缺失文件 → 现有完整性/下载工具（审批）；禁用模组 → 移动文件（审批、可恢复）；
5. 每步后重新体检关键项，失败则停止并显示证据；
6. 无法确认时明确说"无法确认"，建议导出报告/人工排查。

#### C. 版本升级 / 移植

**不重写**现有九类选择、兼容性报告、快照、事务、回滚。Agent 只做：`upgrade.inspect` 读取来源/目标/兼容性/空间/预计内容；向用户解释风险与选项；需要执行时打开现有升级页/向导，由用户在原界面里选择和确认；读取 `OperationReport`/历史解释结果。若 Stage 2 要做 headless 执行，必须复用同一个 `InstanceUpgrade` 事务，并由审批卡展示与现有向导同等的确认信息；未完成该复用前不做 headless 发布。

#### D. 模组维护

列表、依赖检查、更新检查走现有 `ModDependencies` / `ResourceStore`；删除/禁用走现有文件操作（移动而非永久删除，保留恢复路径）；安装/更新走现有安装控制器，展示来源、版本、前置、覆盖/新增文件；不按文件名猜兼容性，不确定时显示未知让用户决定。

### 11.3 工作流状态机（Stage 2）

状态：**Stage 2B 已实现**（`AgentWorkflow` + `AgentWorkflowEngine` + `AgentCompositeToolEngine`），现行行为见 [AGENT_STAGE2B](AGENT_STAGE2B.md) §1.3。

```
Collect → Analyze → Plan → AwaitPlanApproval → ExecuteStep* → Verify → Report
```

- 每个阶段有进入条件、允许工具、退出条件、最大步数；模型不能跳过 `Verify`，验证失败回到 Plan/停止。
- 计划必须由结构化工具提交（`workflow.submitPlan`）并本地校验字段，自然语言计划不算。
- 计划内每个执行步骤逐项审批，批准计划不等于批准所有写操作。
- 完成报告含：实例、目标、做了什么、证据、结果、回退方式、未解决项。

### 11.4 技能测试

- **Stage 1A/1B**：没有工作流状态机/`workflow.submitPlan`；只有"自由对话"，调用未注册技能必须拒绝。
- **Stage 2**：允许工具之外调用拒绝；只读技能不能写；计划缺字段/多余字段拒绝；验证失败不能报完成；用户拒绝某一步，模型收到事实、不能继续假装成功；技能版本固定，更新技能要有迁移说明。

---

## 12. 防御性编程清单

结论：本节是实现的硬约束，不满足就不算完成。路径与写盘的细节见 §9.3，不在此重复。

### 12.1 外部输入

- API JSON/SSE、MCP JSON、会话 JSON、工具参数、文件名、网页内容、日志内容：全部按"不可信"解析；有大小、深度、条目数、字符串长度上限；解析失败返回错误，不抛异常穿过边界；不把外部字符串拼进 shell/命令行/正则（必要时转义/用参数数组）。
- 拒绝未知/多余的工具参数（`additionalProperties=false`）。
- 文件名走 `InstanceFiles::validatePathSegment` / `validateFileName` 同一套判据，不另写一套"差不多"的。

### 12.2 路径与写盘

- 只用"实例 id + 相对路径"的接口；字符串检查只是第一道；已存在目标用句柄级校验，新建先校验父目录，写前记录对象身份、写后复核，对象变化报错并保留备份（§9.3）。
- 覆盖/删除必须有 `expectedSha256`/对象身份或明确的"已确认覆盖"；写前备份、临时文件 + 原子替换，失败保留原文件。
- 保护目录（versions/libraries/assets/java/servers）Stage 1B 原始写一律拒绝。
- 并发替换与文件竞争检测：对象身份复核失败作为可捕获错误处理。

### 12.3 并发与生命周期

- 每个异步请求带 requestId + sessionId + generation，旧回调丢弃；单会话单飞；工具串行；写操作串行。
- 所有 `QObject` 连接用上下文对象；跨对象回调用 `QPointer`/弱引用；删除后不裸解引用。
- 活动会话不能直接删除：先取消模型请求和工具、等待安全终止，再删数据；有 `deletionState=pending/deleting`，重启后先收尾再继续删除。
- 文件写入进入提交阶段后，删除会话/取消不能强行中断；只登记停止，等提交和写后校验结束；超时则报"无法安全停止，暂不删除"。
- 不用嵌套事件循环；MCP/命令进程有 Job Object，退出时清理；关闭窗口有界等待。

### 12.4 资源上限

| 项 | 默认上限 |
| --- | --- |
| 用户单条消息 | 64 KB |
| assistant 单条消息 | 512 KB |
| 单次 API 响应 | 16 MB |
| SSE 单行 | 1 MB |
| 单条工具参数 | 64 KB |
| 单条工具内联结果 | 16 KB |
| 单次工具 artifact | 16 MB |
| 单会话 artifacts | 64 MB |
| 单会话文件 | 8 MB / 2000 条消息 |
| 单次目录列表 | 2000 项 |
| `fs.read` | 默认 256 KB / 最大 1 MB |
| `fs.write` | 1 MB |
| MCP 单行 | 8 MB |
| 命令输出 | 256 KB |
| 最大步数 | 20 |
| 单轮工具调用 | 8 |
| 单会话总工具调用 | 40 |
| 重试 | 2 次 |
| 模型请求总时长 | 10 分钟（可取消） |
| 工具超时 | 120s（文件 30s、MCP 120s、命令 60s） |

所有数字有单一出处（常量/配置），测试引用常量，不散落字面量。

### 12.5 隐私与脱敏

- 密钥、令牌、密码、Cookie、Authorization 头不进日志/会话/错误。用户消息、工具结果、文件、网页在落盘和发模型前都过 `AgentRedaction`；当前规则见 §5.6.2.1。疑似密钥时 UI 提示、默认遮挡/不发送；用户消息里检测到 key/token 时不落明文。
- API key 存储规则见 §5.2；日志只记摘要，不记请求体/响应体全文，错误正文截断并脱敏。
- 会话元数据与消息默认明文存储（D2），加密只保护本地落盘，发送给模型的内容不变；敏感 artifacts 采用 DPAPI 加密，支持整会话 DPAPI 本地落盘加密（§6.7）。
- 外发按 D11（§5.6）。删除会话默认不删维护备份（D10）；删除整个 Agent 数据只清理 `owner=agent`。
- UI 不显示"已备份"除非真有；删除失败显示具体阶段。

### 12.6 G1A/G1B 安全审查清单（Stage 1A/1B 结束前分别打勾）

**G1A（1A：无工具聊天）** —— 人工通过，网络错误延后

- [x] 明文 key 不出现在任何日志、会话 JSON、错误卡片、导出文件、测试输出里；
- [x] Windows Credential Manager 主路径可用；DPAPI 后备只在主路径失败时写入；两者都失败拒绝保存；清除失败可见（主路径人工确认；后备/双失败自动测试覆盖）；
- [x] 密码输入默认遮挡；眼睛按钮只在用户操作期间显示明文；失焦/保存后恢复遮挡；
- [x] 非 https 的远程 Base URL 被拒绝；loopback HTTP 需显式确认并持续提示；
- [x] key 不进 URL；重定向/证书错误检查**延后**，见 [网络错误验收](../archive/ACCEPTANCE_LOG.md)；
- [x] 会话默认明文，整会话加密可选且失败不半加密；加密开关在聊天头部 `⋯` 菜单里，下注「标题和时间仍为明文。」；
- [x] 没有文件工具/MCP 进程；模型不能修改系统提示/API 配置；
- [x] 测试中用 grep/断言搜索 key 和敏感头，结果为空。

**G1B（1B：文件工具/审批/外发）** —— B1/B2/B3 已在代码里修复；R1–R4、R6、R7、W4/W5、A1、P4、P5 复测未完成，G1B 不写成已通过。记录见 [ACCEPTANCE_LOG](../archive/ACCEPTANCE_LOG.md)，当前边界见 [SECURITY_BOUNDARIES](SECURITY_BOUNDARIES.md) §4。

- [ ] 路径攻击矩阵（绝对路径、`..`、盘符、UNC、ADS、保留名、尾点、junction）全拒绝；
- [ ] 句柄级校验：最终路径/文件 ID/reparse 检查；写入前后对象身份比对与并发替换检测；
- [x] 写文件有期望哈希/fileIdentityToken、备份、原子提交、回读校验；拒绝审批时一个字节不变（人工：W1–W3 通过）；
- [x] 会话锁定后工具不接受模型传入的实例 id；切换实例/游戏文件夹有显式确认/阻断（人工复核通过）；
- [x] 敏感 artifacts 默认加密；模型可读 artifact 只存脱敏内容；本地原始存档模型不可枚举/不可读；`artifact.read` 每次重扫（A2/A3 无问题；脱敏变体缺口另记）；
- [x] D11 外发策略：标准模式边界表、自动读取告知、内容字段过滤、目录/类型限制、凭据文件/存储不可读、普通文本凭据字段只发脱敏片段、首次使用说明、本地/云提示（边界/告知/严格模式通过；**脱敏变体覆盖仍受 P0 影响**）；
- [ ] `fs.list` 不发完整文件名；完整清单/配置逐次确认且无会话目录放行；
- [x] 活动会话删除先取消/等待；文件提交阶段不强行中断；
- [ ] 备份创建也审批；删除 Session 不删除维护备份；`owner=agent` 由 Agent 清理，`owner=launcher/user/shared` 只删引用；来源 Session 删除后可恢复；
- [ ] 模型不能修改工具白名单/审批策略/API 配置；工具结果里的指令不改变权限；
- [x] 测试中用 grep/断言搜索 key 和敏感头，结果为空（人工 grep 结果为空）；
- [x] 真实模型 tool_calls 流程（DeepSeek/兼容服务）与文件读写审批在真机上走一遍；
- [x] 没有 MCP 进程/设置页。

### 12.7 用户可见的失败

- 任何影响结果的失败都有：一句人话、错误分类、下一步（重试/设置/导出/打开日志）；不伪造成功，不把未知写成正常。
- 部分成功要分开说：哪些文件写了、哪些没写、备份在哪、怎么回退。
- 工具被拒绝/取消后，模型不能继续执行同一动作，只能请求用户重新确认。

---

## 13. 测试与验证计划

结论：核心层有独立测试域 `verify_agent`，UI 与主窗口集成各有专项；测试不访问真实 API，不写真实用户目录。已实现部分的计数和本机结果见 [Stage 1A](AGENT_STAGE1A.md) / [Stage 1B](AGENT_STAGE1B.md) 与 [TESTING.md](../dev/TESTING.md)。

### 13.1 测试域与接线

- `verify_agent`：`tests/core_modules/agent_main.cpp` + `Agent*Tests.cpp`，独立构建；CTest `wtfml.core-agent`（标签 `core;core-agent;ci-core`，`RUN_SERIAL`，超时 300 秒，见 `tests/core_modules.cmake`）。
- UI：`tests/ui_check/modules/UiAgentCheck.cpp`，CTest `wtfml.ui-agent`；主窗口集成：`tests/mw_check/modules/MwAgentCheck.cpp`（内含 `FakeAgentModelClient`），CTest `wtfml.main-window-agent`。
- 归属与影响映射：`tests/core_modules/owners.json` 把每个 `Agent*Tests.cpp` 映射到 `agent`；`tools/test-impact.ps1` 把 `src/agent/` 与 `src/util/SecretProtector.*` 映到 `core-agent`，`ui/Agent*` 映到 `ui-agent`，`src/controllers/AgentController*` 映到 `agent-window` + `window`，不会落进"未分类 ⇒ full"；`tools/check-test-modules.ps1` 要求新测试文件有 owner。
- 套件入口和使用方法见 [TESTING.md](../dev/TESTING.md)「智能体（Agent）测试套件」。

### 13.2 纯函数/核心测试

按阶段分组：**1A 组**（SSE/Provider 适配、会话存储/加密、凭据存储、上下文、Loop 无工具、Markdown、聊天 UI）；**1B 组**（`AgentWinPath`/文件工具、审批、D11 外发、artifact 分离、实例锁、活动会话删除、备份元数据）；**Stage 2 组**（工作流、launcher 工具、MCP）。

| 测试文件 | 覆盖 | 状态 |
| --- | --- | --- |
| `AgentSseParserTests.cpp` | 分块边界、CRLF、`data:`、注释、`[DONE]`、JSON 拆分、UTF-8 多字节、tool_calls 分片、重复 index/id、reasoning、usage、错误事件、超长行、随机分块 fuzz | 已有 |
| `AgentOpenAiClientTests.cpp` | 请求构造、流式/非流式、错误体保留、重试与取消（本地假服务器） | 已有 |
| `AgentProviderAdapterTests.cpp` | 认证头/流式/`max_tokens` 字段/system role/reasoning/temperature/tool_choice 各枚举组合；未知值拒绝；不发送未启用字段 | 已有 |
| `AgentContextTests.cpp` | 预算裁剪、tool 配对、CJK/emoji 估算、系统提示保留、环境刷新、artifact 引用、省略标记、超长用户消息 | 已有 |
| `AgentSessionStoreTests.cpp` | 元数据/正文分开、往返、schema、损坏/截断、bodySha256、恢复快照、原子写、只读目录、路径 id、大小上限、删除标记/失败、活动删除待处理、改名/固定排序、artifacts 清理 | 已有 |
| `AgentSessionCryptoTests.cpp` | 整会话加密往返、明文正文删除、DPAPI 失败不半加密、解密失败不回退明文、加密快照/导出、元数据可列 | 已有 |
| `AgentArtifactTests.cpp` | 模型可读/本地不可读分离；模型 id 不能读原始存档；`artifact.read` 每次重扫；先前审批不免除；加密落盘；删除/配额 | 已有 |
| `AgentCredentialStoreTests.cpp` | Credential Manager 主路径（唯一 target，用后清理）、DPAPI 后备、迁移、两者失败拒绝保存、清除失败、后备密文不含明文、脱敏、掩码默认值 | 已有 |
| `AgentInstanceLockTests.cpp` | 首次锁定、不随主页切换、切换确认、待审批时禁止切换、实例删除/换游戏根/同名实例 | 已有 |
| `AgentBackupLifecycleTests.cpp` | `backup.create` 必审批、元数据含 `owner/source/sourceSessionId`、自有备份清理、launcher/user/shared 只删引用、删除会话不删备份、来源会话删除后恢复 | 已有 |
| `AgentEgressPolicyTests.cpp` | 标准模式边界表、文件名-only 误判、内容字段过滤、目录/类型限制、自动读取告知、本地/云、严格模式、预览脱敏、首次使用确认、确认后原文不进明文消息 | 已有 |
| `AgentPathPolicyTests.cpp` | 绝对路径、`..`、盘符、UNC、ADS、保留名、尾点/空格、长路径、junction/symlink/reparse、句柄级最终路径/文件 ID、写入前后对象替换、哈希不匹配、保护目录、大小/二进制拒绝 | 已有 |
| `AgentToolTests.cpp` | schema 校验、未知工具、模型传入 instanceId 被拒、未锁定/实例不存在、`fs.list` 不带文件名、`fs.listNames` 逐次确认、`fileIdentityToken` 不匹配、超时、取消、错误映射、输出截断、artifact、外发审批错误 | 已有 |
| `AgentApprovalTests.cpp` | 风险等级、默认拒绝、会话放行只对 web.search/fetch、文件/配置/凭据/写/删/MCP 不放行、备份创建也审批、审批状态恢复、Esc/拒绝 | 已有 |
| `AgentLoopTests.cpp` | 文本→完成、工具→结果→完成、多工具、未知工具、坏 JSON、最大步数、重复无进展、取消、401/429/5xx、部分流、持久化、审批拒绝 | 已有 |
| `AgentSkillsTests.cpp` | Stage 1A/1B 仅接口/自由对话；调用未注册技能拒绝 | 已有；Stage 2 补白名单/计划校验/验证失败 |
| `AgentMarkdownTests.cpp` | HTML 转义、script/iframe/img 去除、代码围栏、未闭合围栏、链接、表格、CJK、超长输入、无远程资源 | 已有 |
| `AgentDiagnosticProjectionTests.cpp` | 允许字段保留；绝对路径/账户/环境/原始日志剔除；空结果不报成功；`scrubPaths` | 已有 |
| `AgentLauncherToolsTests.cpp` | 19 个启动器工具的分派与审批；默认不带文件名；`settings.get` 不含路径；拒绝不改盘；模型传实例 id 被拒 | 已有 |
| `AgentWorkflowTests.cpp` | 计划字段/工具/只读/验证步/步数校验；计划卡拒绝作废；计划外写与跳步被拦；失败后锁写；完成闸 | 已有 |
| `AgentMcpProtocolTests.cpp` / `AgentMcpTrustTests.cpp` | JSON-RPC 帧与上限、工具线上名、`configHash` 变化；首次启动确认、身份变化失效、文件缺失阻断、重连确认、确认记录落盘、未连接拒绝 | 已有 |
| `AgentMcpClientTests.cpp` | 真子进程（`verify_agent --mcp-fake-server`）：握手、分页、stderr 脱敏、逐次审批、异步结果、`isError`、垃圾行、崩溃不自动重启、重连确认 | 已有 |
| `AgentWebPolicyTests.cpp` | scheme/userinfo/片段、A/AAAA 全量校验（v4/v6 各特殊段）、pinned-IP URL、content-type、HTML 转文本、截断 | 已有（真网/代理/证书项见 G3 清单） |
| `AgentWebToolsTests.cpp` | 审批闸、私网阻断、pinned-IP 与 Host/TLS 主机名、重定向逐跳重检、重定向上限、非文本类型、结果脱敏、搜索 JSON | 已有（用假传输，不访问公网） |
| `AgentCommandToolsTests.cpp` | 默认关闭、spec 白名单、host 参数校验、逐次审批、拒绝不执行、真跑一条命令 | 已有 |

### 13.3 本地假 HTTP 服务器

用 `QTcpServer`（与现有夹具一致）起 loopback SSE 服务器：正常流、分块边界（含拆在 UTF-8/JSON 中间）、慢流、断开、429/401/500、重定向、坏 JSON、超大响应、空响应；断言请求头（`Authorization` 存在且值正确、key 不在 URL、`Accept` 正确）；协议适配项逐个断言（`api-key` 头、非流式、`max_completion_tokens`、`developer`、reasoning 回传/不回传、temperature 省略、tool_choice 省略）；断言服务端日志/测试输出不出现 key。不访问公网；真网测试单列在验收，不用夹具冒充实网。

### 13.4 UI 测试（`ui_check_agent`）

覆盖项：

- 导航：5 个选项卡、顺序、5 颗导航按钮、居中、最小宽度不重叠、启动停在主页。
- 页面：双栏几何、侧栏宽度（248 / 窄窗 216）、空态、输入框高度、发送/停止状态；没有 API 配置时发送不可用，配置后启用，配置失效时可见。
- 会话：新建/选中/三个点菜单/重命名/固定/删除确认/删除后选中/排序/相对时间；实例徽标/分组/筛选；整会话加密开关/锁图标。
- 实例锁定：未锁定芯片跟随主页；首次工具后锁定；锁定后主页切换不影响；切换确认；待审批时切换被拒；实例不可用提示。
- 设置：默认星号遮挡、眼睛切换、保存后不回显、Credential Manager/后备状态、URL 校验、http 警告、部署类型、协议适配高级项、测试连接状态、删除 key 二次确认。
- 隐私/外发：标准/严格模式、自动读取告知、`fs.list` 只显示目录元数据、`fs.listNames`/配置逐次确认且无会话放行、确认预览卡、凭据文件不可读、工具卡"已发送 N KB 到 host"。
- 导出/备份/删除：导出范围/脱敏/加密选项；备份列表显示来源 Session；删除会话提示备份保留；活动会话删除先停止；删除全部 Agent 数据列出数量。
- 消息：流式增量、代码块、折叠思考、错误卡片、工具卡、审批卡、停止、自动滚动/上滚不跟随/回到最新、选中不丢。
- 主题（浅/深、背景图、禁用态、焦点环）、无障碍（objectName、accessible name、Tab 顺序、密码不泄露）、尺寸（900×620、更窄、高 DPI 逻辑像素，无横向滚动或重叠）。
- MCP 确认（Stage 2）：首次启动/配置修改/重连确认、本机权限警告、配置变化后旧确认失效。

### 13.5 主窗口集成（`main-window-agent`）

- 真 `MainWindow` 里存在智能体页、导航、控制器，启动默认主页。
- 注入假 `AgentModelClient`（不联网）：发消息 → 流式 → 完成 → 重开后会话还在；切换会话/切页/关窗取消，不崩、不挂、不残留定时器；失败/重试/取消状态正确。
- 假工具宿主：审批拒绝文件不变；审批通过文件原子写、备份存在、内容正确；路径越界/哈希不匹配/最终路径或文件 ID 不符拒绝且可见；写入前后目标被替换报错并保留备份。
- 实例锁定：`instance.list` 默认只有总数；`instance.context` 未锁定拒绝；讨论 A 后切主页到 B 工具仍只操作 A；显式切换需确认并作废旧审批/计划；待审批/执行中禁止切换；换游戏文件夹后锁定实例不可用。
- 外发策略：`fs.list` 只发目录元数据；完整清单/配置逐次确认；自动读取前有告知；凭据文件阻断；普通文本凭据字段只发脱敏片段；拒绝确认后模型不能继续发送；严格模式关闭自动读取；工具卡显示发送量与目标 host。
- artifact/加密：`artifact.read` 只读脱敏后的模型 artifact，先前审批不豁免；开启整会话加密后正文变 `.enc`、明文被删、重开可读，失败保留明文，坏值/换用户显示无法解密。
- 备份/删除：`backup.create` 必须审批；删除会话后备份还在；来源会话删除后备份仍可恢复；删除全部数据只清 `owner=agent`；活动会话不能直接删除，文件提交阶段等待。
- 与现有业务集成（Stage 2，临时实例）：体检调用 `StartupDiagnosis` 且与纯函数一致；诊断经字段白名单投影；升级只读检查复用现有事实；占用中的实例拒绝修改。
- 关闭窗口时流式/审批/工具执行中：有界退出，会话落盘，进程清理；文件提交阶段等待。

### 13.6 防御性/故障注入矩阵

必须覆盖的类别（每类的具体用例落在上面各测试文件里）：

- 网络：断流、慢流、429、401、5xx、TLS 错误、重定向、超大响应、坏 UTF-8。
- 模型输出：未知工具、坏 JSON、半截 tool_call、重复 id、超大步数、无进展循环、只输出工具不输出文本、文本里伪造审批。
- 工具：文件不存在、权限拒绝、磁盘满、哈希变化、junction 逃逸、超时、取消、重复执行、进程崩溃；最终路径/文件 ID 不符；写入前后对象被替换；`artifact.read` 试图读本地原始存档/绕过脱敏；诊断投影漏路径/账户字段。
- `web.fetch`、MCP、凭据、存储（会话损坏、元数据/正文哈希不一致、快照损坏、写盘失败、只读目录、artifacts 超额、删除标记残留、并发写、整会话加密/解密失败）、备份（审批拒绝、元数据缺字段、自有/共享混淆、来源会话删除、误删、清理策略）：用例清单见 §9.5、§10.4、§5.2、§6、§6.6。
- 外发：自动白名单误判、文件名正常但内容含凭据、确认被拒、严格模式、首次使用未勾选、自动读取未告知、本地/云切换、预览脱敏不彻底；`fs.list` 意外带出完整文件名；配置出现会话目录放行；`instance.list` summary 带出名称；`instance.context` 未锁定却返回全局摘要。
- 生命周期：关窗、切会话、切游戏文件夹、快速连点发送、审批中切换、锁定后切主页、活动会话删除、文件提交阶段删除、重开应用。
- 安全：key 出现在错误/日志/会话/导出中的搜索断言；密码字段默认遮挡；未知 URL/重定向/私网抓取拒绝；提示注入内容不能改变工具白名单。

### 13.7 真机/实网验收（单独记录）

- Windows 10/11 真机、Credential Manager 当前用户、DPAPI 后备、中文/空格/长路径数据目录。
- DeepSeek/兼容服务真实流式、429/401/断网、代理、TLS 证书错误；协议适配项各选一组。
- 高 DPI、读屏（至少基础焦点/名称检查）。
- 真实实例：锁定 A 后主页切 B、体检、崩溃日志、配置修改/回退、备份、升级向导打开与取消；删除会话后恢复其维护备份；删除全部 Agent 数据后确认备份/会话/密钥清理。
- 标准模式边界：日志/崩溃/元数据/目录元数据自动，`fs.list` 不带完整文件名，配置/完整清单/其他文件逐次确认且无会话放行，凭据文件不可读，自动读取前的"可能发送给当前模型服务"告知可见。
- 整会话加密在当前用户可读、换用户/机器不可读；加密导出提示。
- Stage 2：真实 MCP 服务器（Node/Python 各一）首次启动/配置修改/重连确认、可执行文件替换后的身份复核、退出/崩溃清理。Stage 3：`web.fetch` DNS 多地址/重绑定、系统代理已配置仍直连、证书主机名校验；真实命令诊断（固定白名单）；任意命令高风险授权不默认开启。
- 记录首次失败，不用复测覆盖。

### 13.8 测试纪律

- 不访问真实 API、不写真实用户目录、不用真实存档做破坏性验证。
- 每个测试用独立 `QTemporaryDir` 和独立 QSettings，恢复消息处理器/环境变量。
- 网络夹具按请求目标路由，不依赖请求顺序，未知响应不当成功。
- 遵守现有入口大小限制，新增测试先过 `check-test-modules`。
- 测试失败先区分产品回归与夹具限制，不为变绿放宽断言。

---

## 14. 阶段计划

结论：Stage 1A/1B 已实现，Stage 2–4 未开始；排期是参考（1 人，含测试/文档），每个阶段结束都要过验收门。

| 阶段 | 状态 | 验收门 | 备注 |
| --- | --- | --- | --- |
| Stage 1A | 已实现 | G1A：人工通过，网络错误延后 | 实现范围、盘上格式、待测项见 [AGENT_STAGE1A](AGENT_STAGE1A.md) |
| Stage 1B | 已实现 | G1B 复测待做：B1/B2/B3 已修，R1–R4、R6、R7、W4/W5、A1、P4、P5 未完成 | 范围、安全边界、待测项见 [AGENT_STAGE1B](AGENT_STAGE1B.md)；清单在 §12.6 |
| Stage 2B | 已实现 | G2B：人工验收待做 | 范围、边界、待测项见 [AGENT_STAGE2B](AGENT_STAGE2B.md) |
| Stage 2A | 已实现 | G2A：MCP 单独验收待做 | 范围、确认闸、待测项见 [AGENT_STAGE2A](AGENT_STAGE2A.md) |
| Stage 3 | 已实现（联网 + 固定诊断命令；资源工具/HTTP MCP 未做） | G3：人工验收待做 | 范围与边界见 [AGENT_STAGE3](AGENT_STAGE3.md) |
| Stage 4 | 已实现（收口项；真机项待做） | G4：人工验收待做 | 见 [AGENT_STAGE4](AGENT_STAGE4.md) |

G1B 通过前不得写成已通过；未完成的真机项：真实路径/junction/长路径、真实文件竞争/占用、高 DPI/读屏。

### Stage 1A / 1B：已完成

聊天底座（Provider、流式回复、会话、停止/重试、DPAPI 整会话加密）和锁定实例内的受控文件工具（目录元数据、文件名清单、文本读写、artifact、审批、备份）已经实现。当前行为和待测项见 [AGENT_STAGE1A](AGENT_STAGE1A.md)、[AGENT_STAGE1B](AGENT_STAGE1B.md)；G1B 的 B1/B2/B3 已在代码里修复，复测 R1–R4、R6、R7、W4/W5、A1、P4、P5 待做。

### Stage 2：维护工作流 + MCP（单独验收）（约 12–17 个工作日）

**当前状态：2B（工作流 + 启动器工具）与 2A（stdio MCP）均已实现；G2A/G2B 人工验收待做。** 范围与边界见 [AGENT_STAGE2B](AGENT_STAGE2B.md)、[AGENT_STAGE2A](AGENT_STAGE2A.md)。

目标：体检、崩溃急救、升级接入、证据与审批闭环；实现 Skill 工作流引擎；**stdio MCP 作为本阶段独立子项单独验收**。

- [ ] `AgentSkillRegistry` + `AgentWorkflowEngine`（Collect/Analyze/Plan/Approve/Execute/Verify/Report），把 Stage 1A/1B 的接口真正接上。
- [ ] 环境/上下文：锁定实例、切换文件夹失效、`NeedsInstanceLock` / `InstanceUnavailable`。
- [ ] `AgentLauncherTools`：
  - 只读：startup diagnosis、crash、logs tail、mods list、integrity、backup list、upgrade inspect、dependencies；
  - 写/高风险：config write（走已审的 fs 写）、backup.create（**每次审批**）、backup.restore（来源 Session 可已删除）、mods disable/remove、upgrade open wizard/execute（复用现有事务）；
  - `settings.get/set`（白名单键、审批、旧值/新值）；`diagnostics.*` / `instance.integrity` 结果经 `AgentDiagnosticProjection` 投影。
- [ ] 模式/技能：实例体检（只读）；崩溃急救（计划 + 逐步审批 + 验证）；版本升级/移植（优先打开现有向导）；模组维护（依赖/启停/更新，复用现有控制器）。
- [ ] 证据模型 + 最终报告引用；无法确认时停止。
- [ ] 审批 UI 增强：计划卡、diff、备份、危险操作二次确认、恢复待审批。
- [ ] 备份生命周期：`backups/maintenance` 独立清理策略/列表；元数据含 `owner/source/sourceSessionId`、实例锁、来源路径、sha256；Agent 小型文件备份默认 DPAPI，现有大快照沿用原策略；删除会话不删备份，删除全部数据只清 `owner=agent`；来源 Session 删除后仍可恢复。
- [ ] 任务中心/进度/取消接线；占用检查（安装/复制/备份/运行中）。
- [ ] **MCP 子项**：`McpClient/McpManager`（stdio、握手、tools/list、call、取消、Job Object）；首次启动/配置修改/重连确认，`configHash` + 可执行文件身份；可执行文件被替换后旧确认失效；不自动重连；MCP 设置页、工具列表、本机用户权限警告；假 MCP 服务器测试；MCP 工具结果不绕过 D11。
- [ ] 测试：工作流状态机/计划校验/证据/拒绝/验证失败；临时实例端到端（锁定、体检、配置修复回退、备份创建审批、来源会话删除后恢复、升级只读/向导）；与现有业务判据一致（不复制逻辑）。
- [ ] 验收门 **G2A（MCP）**：单独验收 MCP 首次启动/配置修改/重连确认、工具审批、进程清理、外发策略；不通过不把 MCP 算完成。
- [ ] 验收门 **G2B（工作流）**：用真实临时实例演示"发现 → 计划 → 批准 → 执行 → 验证 → 回退"，安全审查通过后再进 Stage 3。

Stage 2 完成判据：体检每条结论有证据，没有证据明确"无法确认"；崩溃急救识别现有规则覆盖的类别（OOM、Java 不匹配、缺前置、配置错误）；任一写操作可拒绝、可回退，验证失败不报成功；`backup.create` 每次审批，删除来源会话后备份仍可恢复；升级路径复用现有事务，取消/失败保留原实例；运行中/占用中的实例拒绝修改；实例锁定不被主页切换影响；MCP 子项单独验收通过（确认闸、身份复核、工具审批、崩溃/超时/退出无孤儿）；工作流/UI/集成测试全绿。

### Stage 3：扩大工具面（约 10–14 个工作日）

**当前状态：`web.search`（SearXNG）、`web.fetch`（pinned-IP）、`command.diagnose`（固定 spec）已实现；资源搜索/安装、Brave/Tavily、HTTP/SSE MCP、`command.run` 未做。** 现行行为见 [AGENT_STAGE3](AGENT_STAGE3.md)，G3/G4 人工清单见 [AGENT_STAGE3_4_ACCEPTANCE](AGENT_STAGE3_4_ACCEPTANCE.md)。

目标：联网搜索/抓取、固定诊断命令、资源维护、可选 provider。

- [ ] `web.search`（至少一个 provider）+ `web.fetch`（全 A/AAAA 校验、pinned-IP、重定向逐跳重检、不继承代理、SSRF/大小/content-type）。
- [ ] `command.diagnose`：固定诊断命令 + 参数白名单（§9.5.1），默认关闭，逐次审批，Job Object + 输出上限。
- [ ] `command.run`（任意可执行文件）作为单独高风险能力：独立授权页、默认关闭、不随更新自动开启；是否进入本阶段由 G3 单独决定，未批准不做。
- [ ] 资源搜索/安装/更新工具（复用 `ResourceStore` / 安装控制器），覆盖前置/冲突/覆盖确认。
- [ ] 更多 MCP（HTTP/SSE，若批准）、可选 Anthropic provider（若 D6 选择扩展，否则只保留接口）。
- [ ] 性能：长会话渲染、会话列表懒加载、工具卡虚拟化（按测量决定）。
- [ ] 测试：网络策略、真实本地服务器、固定命令进程树、资源安装回归、外发策略。
- [ ] 验收门 G3：网络/命令安全审查；真实网络/真机验收记录。

Stage 3 完成判据：私网/环回/重定向/超大响应/坏 content-type 全部拒绝并有测试；`web.fetch` 校验全部 A/AAAA、连接已校验 IP、重定向逐跳重检、不继承系统代理，DNS 重绑定/连接 IP 不一致有测试；`command.diagnose` 只能执行固定 spec/参数白名单，默认关闭，取消/超时无孤儿；`command.run` 未获单独授权时产品里不可用、不可被模型开启；资源安装沿用现有校验/回退；真实网络失败与成功都能说清。

### Stage 4：收口与发布（约 5–8 个工作日）

**当前状态：自动化回归、性能基线、发布检查脚本与文档收口已完成；真机/实网/高 DPI/读屏/换用户等人工项待做。** 结果见 [AGENT_STAGE4](AGENT_STAGE4.md)，性能数字见 [AGENT_STAGE4](../dev/perf-notes/AGENT_STAGE4.md)。

- [x] 完整回归（core 15/15、window 3/3、network 6/6、ci-ui 23/23；visual 未跑）。
- [ ] 真机/实网/高 DPI/读屏/长路径/中文路径/受限权限验收。
- [ ] Credential Manager 主路径、DPAPI 后备、整会话加密/换用户失败、备份生命周期验收。
- [ ] D11 外发范围验收：自动白名单、确认预览、凭据硬阻断、本地/云差异。
- [x] 性能测量：核心层 2000 条会话读写/加密/上下文裁剪/token 估算；智能体页 2000 条消息重摆（数字见 perf-notes）。
- [ ] 文档：`FEATURES.md`、`docs/dev/TESTING.md`、`ROADMAP.md`、`SECURITY_BOUNDARIES.md` 增补；本文状态改为已实现/验收记录。
- [x] 发布检查：`tools/check-agent-release.ps1` PASS（明文密钥/假端点/`command.run`/审批闸调用点/危险开关默认值）。
- [ ] 验收门 G4：发布负责人确认；未通过不上发布物。真机项清单见 [AGENT_STAGE3_4_ACCEPTANCE](AGENT_STAGE3_4_ACCEPTANCE.md)。

### 14.1 依赖关系

```
Stage1A 凭据/会话/provider/loop/UI（无工具）
       → G1A
       → Stage1B 文件工具/审批/外发/安全测试
       → G1B
       → Stage2 技能工作流/launcher 工具 → G2B
       → Stage2 MCP（stdio，独立）→ G2A
       → Stage3 web/固定诊断命令/resource → G3
       → Stage4 全量验收 → G4
```

- Stage 1A 不做工具，1B 才接文件工具；MCP 不进 Stage 1；Stage 1A/1B 不做技能工作流，但接口（`AgentSkill` / 工具宿主）先留好。
- Stage 2 的业务写工具依赖 Stage 1B 的审批/路径/备份/会话恢复，G1B 不过不进入；MCP 单独验收（G2A），不能拿 G2B 通过替代。
- Stage 3 的网络/命令依赖 Stage 2 的证据/审批，G2A/G2B 不过不进入。

---

## 15. 文件、构建与测试接线

### 15.1 文件布局

已实现的文件（行数见 `git ls-files`）；标"计划"的是后续阶段才会新增的。Stage 2B 新增的模块已标"已实现"。

```
src/agent/                                   （已实现）
  AgentTypes.hpp/.cpp          值类型：消息/错误/请求/回复
  AgentSettings, AgentPaths, AgentSecretStore
  AgentSession, AgentSessionStore, AgentSessionCrypto
  AgentArtifactStore, AgentInstanceLock, AgentBackupIndex
  AgentEgressPolicy, AgentRedaction, AgentApprovalPolicy
  AgentSseParser, AgentModelClient.hpp, OpenAiCompatibleClient, AgentProviderAdapters
  AgentContextBuilder, AgentLoop
  AgentTool.hpp, AgentToolRegistry.cpp, AgentFileToolEngine, AgentFileTools, AgentWinPath
  AgentSkill                   （Stage 2B：五个技能）
  AgentWorkflow, AgentWorkflowEngine, AgentCompositeToolEngine
  AgentLauncherHost.hpp, AgentLauncherTools, AgentDiagnosticProjection   （Stage 2B）
  McpTypes, McpProtocol, McpTrustStore, McpClient, McpManager, AgentMcpToolEngine（Stage 2A）
  AgentWebPolicy, AgentWebTransport, QtAgentWebTransport, AgentWebTools, AgentCommandTools（Stage 3）
src/controllers/AgentController.hpp/.cpp     （已实现）
src/controllers/AgentLauncherHostImpl.hpp/.cpp  （Stage 2B：启动器能力宿主）
src/util/SecretProtector.hpp/.cpp            （已实现；从 TokenStore 抽出的 DPAPI 封装，TokenStore 委托它）
ui/
  AgentPage, AgentMessageView, AgentComposer, AgentSettingsPage, AgentSecretEdit,
  AgentMarkdown, AgentToolCard, AgentApprovalCard, AgentMcpPage, AgentViewTypes.hpp
  （AgentSessionList 尚未单独拆出，会话列表在 AgentPage 内）
tests/core_modules/
  agent_main.cpp 与 Agent{SseParser,OpenAiClient,ProviderAdapter,Context,SessionStore,SessionCrypto,
  CredentialStore,InstanceLock,BackupLifecycle,EgressPolicy,PathPolicy,Tool,Approval,Loop,Skills,
  Markdown,Artifact}Tests.cpp
tests/ui_check/modules/UiAgentCheck.cpp
tests/mw_check/modules/MwAgentCheck.cpp      （含 FakeAgentModelClient）
```

### 15.2 构建

- 产品 `CMakeLists.txt` 把 `src/agent/`、`src/controllers/AgentController.cpp`、`ui/Agent*.cpp` 加进源文件清单；`ui/Agent*` 同时加进 UI 验证层的源文件清单，避免 `ui_check` / `mw_check` 漏链接。
- 已有链接：`Qt6::Network`、`Qt6::Widgets`、`crypt32`、`advapi32`（Credential Manager）；MCP 只需 QtCore（`QProcess`），不新增第三方库。
- 测试：`tests/core_modules.cmake`（`core_cases_agent`、`verify_agent`、`wtfml.core-agent`）、`tests/ui_check/CMakeLists.txt`（`ui_check_agent`）、`tests/mw_check/CMakeLists.txt`（`mw_check_agent`）。
- 新增 owner、入口行数、模块行数检查由 `tools/check-test-modules.ps1` 把关；新标签影响分区时补 `tools/check-ci-partitions.ps1` 映射；`tools/test.ps1` / `test-impact.ps1` 已有 `core-agent` / `ui-agent` / `agent-window` 映射，共享 `src/util` 改动仍走 full/核心。

### 15.3 文档

- 本文是设计与开发计划；实现完成后 [FEATURES.md](../FEATURES.md) 只写已实现行为、硬约束、验证边界；[TESTING.md](../dev/TESTING.md) 写测试入口；[SECURITY_BOUNDARIES.md](SECURITY_BOUNDARIES.md) 写安全边界；[ROADMAP.md](../ROADMAP.md) 挂未完成阶段/验收项，完成后移出。
- 文档链接用相对路径，`tools/check-doc-links.ps1` 检查；源码注释按 [COMMENT_STYLE.md](../dev/COMMENT_STYLE.md) 写"为什么/约束"。
- 源码注释里引用本文的小节编号（§5.1、§5.2、§5.3、§5.6.2、§5.6.2.1、§6.1、§6.2、§7.3、§8.1、§9.3、§13.2 及 D3 / D4 / D6 / G1A）保持不变；改编号要同步改 `src/agent/` 里的注释。

---

## 16. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 模型把日志/网页里的指令当命令（提示注入） | 越权改盘/泄露 | 不可信包裹、系统提示、权限闸独立于模型、写操作逐次审批 |
| API key 泄露 | 账号/费用损失 | Credential Manager 主路径、DPAPI 后备、默认掩码、不写日志/会话/URL、脱敏测试、失败拒绝降级 |
| 模型给出错误修复方案 | 实例损坏 | 证据要求、现有规则优先、计划 + 逐步审批、备份/回退、验证失败不报成功 |
| MCP 服务器任意代码/可执行文件被替换 | 本机被控 | 用户显式添加、绝对路径/SHA-256/文件 ID、最小环境、Job Object、启动/改配置/重连确认、逐次审批 |
| 命令工具误用 | 系统损坏 | 第一版只开放固定诊断命令 + 参数白名单；任意命令单独高风险授权、默认关闭 |
| 外发范围误判 | 隐私泄露 | D11 标准边界表、内容过滤（不只看文件名）、目录/类型限制、自动读取告知、凭据硬阻断、预览/脱敏、严格模式、本地/云提示；脱敏变体已补，复测待做 |
| `artifact.read` 绕过脱敏 | 隐私泄露 | 模型可读 artifact 只存脱敏内容；本地原始存档模型不可读；每次 `artifact.read` 重扫 D11 |
| `web.fetch` DNS 重绑定/代理 | SSRF/内网访问 | 全 A/AAAA 校验、pinned-IP 连接、重定向逐跳重检、第一版不继承代理；G3 测试 |
| 会话/备份隐私 | 聊天记录/文件泄露 | 默认明文但无密钥、可选整会话加密、敏感 artifact 加密、维护备份按 owner 清理 |
| 升级逻辑重复 | 数据不一致/回归 | Agent 只读检查 + 打开现有向导；headless 必须复用同一事务 |
| UI 线程卡顿 | 体验差/假死 | 异步网络/工具、流式合批、大小上限、无嵌套事件循环、性能测试 |
| 长会话/大输出吃内存 | 崩溃 | 预算、artifacts、截断、会话大小上限、懒加载 |
| 现有导航/布局回归 | 大量测试红 | 下标常量统一、先改测试再改页面、五颗导航几何断言 |
| 供应商 API 变化 | 功能失效 | provider 适配层、严格解析、错误可见、非流式回退、契约测试 |
| 费用不可控 | 用户投诉 | 步数/调用上限、429/配额错误、显示估算用量、不自动切换付费模型 |

---

## 17. 设计决策与验收门状态

D1–D11 已确认，内容见 §2。

| 编号 | 结论摘要 | 状态 |
| --- | --- | --- |
| D1 | 导航：主页 / 管理 / 下载 / 设置 / 智能体；启动默认主页 | 已确认，已实现 |
| D2 | 会话：全局 `%LOCALAPPDATA%\WTFML\agent`；默认明文 JSON；`instanceLock` 隔离；导出/备份/删除/整会话加密见 §6.5–§6.7 | 已确认，已实现 |
| D3 | Stage 1A/1B 只做 loop/上下文/会话/受控文件工具；MCP 放 Stage 2；不实现 Skill 工作流引擎 | 已确认 |
| D4 | 用户可见"模式"，内部 Skill；Stage 1 只有自由对话 | 已确认 |
| D5 | 只读自动；写/删/升级/命令/外部 MCP 逐次确认；`backup.create` 也审批；无全自动模式 | 已确认，已实现 |
| D6 | OpenAI-compatible 基础兼容子集 + 枚举化协议适配项 | 已确认，已实现 |
| D7 | stdio MCP 单独验收；首次启动/配置修改/重连均确认；文件身份变化重新确认；不自动重连；工具逐次审批 | 已确认，已实现（G2A 人工验收待做） |
| D8 | 首次实例操作锁定；切换显式确认；工具不接受模型传入实例 id | 已确认，已实现 |
| D9 | 第一版固定诊断命令 + 参数白名单；任意可执行文件单独高风险授权、默认关闭 | 已确认，已实现（`command.run` 未开放） |
| D10 | 删除 Session 不删维护备份；备份带 `owner/source`；只清理 Agent 自有；来源 Session 删除后可恢复 | 已确认，已实现（G1B 备份项待复核） |
| D11 | 外发范围见 §5.6.2 边界表；自动读取告知；内容字段过滤；严格模式 | 已确认，已实现；脱敏变体覆盖已修，复测待做 |

| 验收门 | 内容 | 状态 |
| --- | --- | --- |
| **G1A** | Provider / Credential Manager / DPAPI / 会话 / 聊天 UI，无工具 | 人工通过；网络错误项延后，见 [网络错误验收](../archive/ACCEPTANCE_LOG.md) |
| **G1B** | 路径句柄校验 / 审批 / 外发 / 写入 / 活动会话删除 / artifact 分离 | B1/B2/B3 已修，复测待做；清单见 §12.6 |
| **G2A** | Stage 2 MCP 单独验收：确认闸、身份变化复核、工具审批、进程清理 | 2A 已实现，人工验收待做；清单见 [AGENT_STAGE2A](AGENT_STAGE2A.md) §7 |
| **G2B** | Stage 2 工作流：真实临时实例"发现 → 计划 → 批准 → 执行 → 验证 → 回退"；诊断字段白名单复核 | 2B 已实现，人工验收待做；清单见 [AGENT_STAGE2B](AGENT_STAGE2B.md) §6 |
| **G3** | Stage 3 网络/命令安全审查：pinned-IP/无代理、固定诊断命令、高风险授权（若批准） | 3 已实现，人工验收待做；清单见 [AGENT_STAGE3_4_ACCEPTANCE](AGENT_STAGE3_4_ACCEPTANCE.md) |
| **G4** | 发布验收：全量回归、真机/实网、备份/加密/外发范围、发布检查 | 自动化与发布检查已完成，真机项待做；见 [AGENT_STAGE4](AGENT_STAGE4.md) |

每个阶段结束按 G1A–G4 做人工验收；未验收的计划项不写成"已完成"。


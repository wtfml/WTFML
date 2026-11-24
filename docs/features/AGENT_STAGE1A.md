# 智能体 Stage 1A：Provider、会话与聊天

本文给开发者和测试看 Stage 1A 的现行行为、盘上格式和仍需人工补测的项目。现行产品总账见 [FEATURES.md](../FEATURES.md)，智能体整体设计见 [AGENT.md](AGENT.md)，安全边界见 [SECURITY_BOUNDARIES.md](SECURITY_BOUNDARIES.md)。

## 1. 当前能力

- 导航第五项「智能体」，启动仍停在主页。
- 左侧会话栏：新建、选择、搜索过滤、重命名、固定、删除；标题取第一条用户消息，最长 24 字。
- 右侧聊天：流式正文、推理内容折叠、停止、错误/中断提示、重试；Markdown 只允许白名单标签，原始 HTML/script/iframe/远程图片不执行。
- API/模型设置：OpenAI-compatible 基础兼容子集；Base URL、模型、上下文窗口、最大输出、温度、超时、部署类型；枚举化协议适配（认证头、流式、`max_tokens` 字段、system role、reasoning 回传、temperature/tool_choice 省略）；测试连接只报告成功/HTTP 状态/耗时。
- 凭据：Windows 凭据管理器主路径，DPAPI 后备；两条路都失败拒绝保存并保留旧值；默认星号遮挡、不回显。
- 会话：默认明文 JSON；可在会话 `⋯` 菜单打开「加密存储」，正文改为 DPAPI `.enc`；标题和时间仍为明文。
- 取消：停止、切换会话、切到别的选项卡、关窗都取消在飞请求；流已开始后失败保留部分内容并标「已中断」。

## 2. 代码位置

| 层 | 文件 |
| --- | --- |
| 值类型/会话 | `src/agent/AgentTypes.*`、`AgentSession.*` |
| 设置/路径 | `src/agent/AgentSettings.*`、`AgentPaths.*` |
| 凭据 | `src/agent/AgentSecretStore.*`、`src/util/SecretProtector.*` |
| 会话存储/加密 | `src/agent/AgentSessionStore.*`、`AgentSessionCrypto.*` |
| 脱敏 | `src/agent/AgentRedaction.*` |
| Provider | `src/agent/AgentModelClient.hpp`、`OpenAiCompatibleClient.*`、`AgentProviderAdapters.*`、`AgentSseParser.*` |
| 上下文/Loop | `src/agent/AgentContextBuilder.*`、`AgentLoop.*` |
| 接线 | `src/controllers/AgentController.*` |
| UI | `ui/AgentPage.*`、`AgentMessageView.*`、`AgentComposer.*`、`AgentSettingsPage.*`、`AgentSecretEdit.*`、`AgentMarkdown.*`、`AgentViewTypes.hpp` |

`ui/Agent*` 只 include `ui/` 与值类型；`AgentController` 把核心层消息、会话元数据和 provider profile 翻译成界面值类型。

## 3. 盘上格式

默认数据目录 `%LOCALAPPDATA%\WTFML\agent`；测试用 `QCoreApplication::setProperty("wtfml.testAgentDataDir", <临时目录>)` 隔离。

```text
agent/
  sessions/
    <uuid>.meta.json        # 明文元数据：标题/时间/固定/加密标志/bodySha256
    <uuid>.json             # 默认明文正文
    <uuid>.enc              # 可选整会话 DPAPI 密文
    .recovery/<uuid>/...    # 最近 3 份稳定点快照
  artifacts/<uuid>/         # 模型可读/本地原始 artifact
  backups/maintenance/      # 文件维护备份
```

- 正文先写、元数据最后写，元数据带 `bodySha256`；两次提交之间崩溃时以正文为准重算元数据。
- 正文损坏改名为 `<id>.json.corrupt-<时间戳>`，列表显示「损坏，已隔离」，不当空会话覆盖。
- 单会话正文上限 8 MB / 2000 条消息；超限停止保存并提示导出或新建会话。
- 启动时 `streaming` 消息恢复成 `interrupted`，不自动重发。
- 删除先登记 `deletionState=pending`，再删正文/快照/artifacts/元数据；活动请求先取消；不删文件维护备份。

## 4. 网络与凭据

- Base URL 只接受 `https://`；loopback `http://` 需显式勾选并持续提示；拒绝 userinfo、片段、查询串。
- 不跟随重定向，不忽略 TLS 错误；API Key 只进认证头，不进 URL、日志、会话或错误。
- 响应总额 16 MB、SSE 单行 1 MB；429/5xx/首字节前网络错误最多重试 2 次，流已开始后失败保留部分文本、不自动重发。
- UI 消息先过 `AgentRedaction`，命中疑似凭据时本机遮挡后再落盘/发送并提示用户。
- `AgentSecretStore` 测试钩子只通过 QCoreApplication 属性启用（`wtfml.testAgentDisableCredentialManager`、`wtfml.testAgentSecretProtectFail`），产品入口不设置。

## 5. 待人工测试

- [ ] 网络错误矩阵：401/403、429、首字节前断网、流中断、重定向、TLS 证书错误、系统代理、400 错误体显示。清单与执行方式见 [ACCEPTANCE_LOG](../archive/ACCEPTANCE_LOG.md) 的「延后：网络错误」一节。

网络错误未完成前，不写成 G1A 全部通过；其余 G1A 项已人工通过。

## 6. 限制

- 设置页只编辑一个活动 provider；多 profile 列表/切换 UI 未做。
- 模型下拉只有当前一个模型。
- 会话重命名通过 `QInputDialog` 弹窗完成。
- 本阶段没有维护工作流、MCP、联网工具和命令执行。

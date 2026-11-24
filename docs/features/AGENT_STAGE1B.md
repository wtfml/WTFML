# 智能体 Stage 1B：受控文件工具与审批

本文给开发者和测试看 Stage 1B 的现行行为、盘上格式、安全边界和仍需人工补测的项目。聊天底座见 [AGENT_STAGE1A.md](AGENT_STAGE1A.md)，智能体整体设计见 [AGENT.md](AGENT.md)，安全边界见 [SECURITY_BOUNDARIES.md](SECURITY_BOUNDARIES.md)，未完成项见 [ROADMAP.md](../ROADMAP.md)。

## 1. 当前能力

- **实例锁定**：第一次实例工具调用锁定当前主页实例并先落盘；锁定后不跟随主页切换；换游戏文件夹或实例删除返回 `InstanceUnavailable`；显式切换需确认，可新建会话。
- **文件工具**：
  - `fs.list` 只返回目录元数据（文件/目录数、大小合计、是否截断），不返回文件名；
  - `fs.listNames` 返回完整文件名清单，每次审批，最多 2000 项；
  - `fs.read` 只读锁定实例内文本，默认 256 KB、最大 1 MB；二进制/凭据文件拒绝；返回 sha256 + `fileIdentityToken`；大文本转模型可读 artifact；
  - `fs.write` 只允许 `config/**`、`defaultconfigs/**`、`options.txt`；必须带旧哈希与身份 token；每次审批；写前 DPAPI 备份、原子替换、写后回读；对象/内容变化拒绝；
  - `artifact.read` 只读模型可读 artifact；每次重新脱敏；本地原始存档 id 模型不可读。
- **D11 外发**：日志/崩溃报告/锁定实例元数据/目录元数据自动读取；配置/完整文件名/其他文本/完整实例列表逐次确认；凭据文件/凭据存储/二进制硬阻断；严格模式关闭自动读取；自动读取也提示「可能发送给当前模型服务」。
- **审批**：只读自动；文件读确认/写/命令/外部 MCP 逐次；没有会话级目录放行；审批请求持久化但不存「已批准」布尔，重启后作废、只能重新请求。
- **工具循环**：模型返回 `tool_calls` 后逐次校验/执行/回灌；需审批时 Loop 进入 `AwaitingApproval`，通过后继续请求，拒绝以 `ApprovalDenied` 工具结果回灌。工具内部名（`fs.read`）发给 provider 前映射成 `fs_read`，符合 `^[A-Za-z0-9_-]+$`。
- **错误体保留**：流式请求遇到非 2xx 也保留供应商 JSON 错误体，界面显示安全摘要，不只显示「HTTP 400」。
- **维护备份**：`backups/maintenance/<id>/`；内容 DPAPI 加密，元数据带 `owner=agent` / `source=agent-session` / `sourceSessionId` / 实例 id / 相对路径 / sha256；删除会话不删备份，只清理 Agent 自有备份。
- **UI**：工具卡（待执行/执行中/完成/失败，可展开详情）、审批卡（目标/摘要/diff/预览/备份/外发说明/允许/拒绝）、实例胶囊、严格模式开关。

## 2. 代码位置

| 模块 | 文件 |
| --- | --- |
| 路径/对象身份 | `src/agent/AgentWinPath.*` |
| 外发策略 | `src/agent/AgentEgressPolicy.*` |
| 审批矩阵 | `src/agent/AgentApprovalPolicy.*` |
| 文件工具 | `src/agent/AgentFileTools.*`、`AgentFileToolEngine.*` |
| artifact | `src/agent/AgentArtifactStore.*` |
| 维护备份 | `src/agent/AgentBackupIndex.*` |
| 实例锁定 | `src/agent/AgentInstanceLock.*` |
| 工具 schema/注册 | `src/agent/AgentTool.hpp`、`AgentToolRegistry.cpp` |
| Loop | `src/agent/AgentLoop.*`（`AwaitingApproval` / `ExecutingTool`、工具结果回灌） |
| UI | `ui/AgentToolCard.*`、`AgentApprovalCard.*`、`AgentPage.*`、`AgentSettingsPage.*` |
| 接线 | `src/controllers/AgentController.*`、`src/MainWindow.*` |

`ui/Agent*` 只 include `ui/` 与值类型，不 include `src/`。

## 3. 盘上格式补充

```text
agent/
  artifacts/<sessionId>/<id>.enc          # DPAPI；模型可读/本地原始存档
  artifacts/<sessionId>/<id>.meta.json    # modelReadable/size/sha256，不含内容
  backups/maintenance/<backupId>/
    original.enc                          # DPAPI 备份内容
    meta.json                             # owner/source/sourceSessionId/哈希/路径
```

- 模型可读 artifact 写入前脱敏、读取时再次脱敏；本地原始存档 id 不在模型可见 id 空间里。
- 审批请求存 `pendingApproval`，不存 `approved`；恢复会话时作废并提示重新请求。
- 大文本 `truncated=true` 时，artifact 只保存本次读取窗口（默认 256 KB、最大 1 MB），不是整个原文件。

## 4. 实现边界

- `fs.write` 只开放文本配置白名单；`versions/libraries/assets/java/servers` 原始写拒绝；二进制/jar/zip 不读内容。
- 路径校验是字符串快速拒绝 + Win32 句柄级最终路径/文件 ID/reparse 核对；句柄校验降低 TOCTOU 竞争窗口，不宣称彻底解决。
- 本阶段没有 `logs.tail` / `diagnostics.*` / `instance.context` / `backup.restore`，也没有任意命令和 MCP。
- 日志自动读取仍可能带出 Windows 用户名、启动参数回显、绝对路径、硬件指纹；这些留给 Stage 2 的日志/诊断字段投影。
- 活动会话删除：有待审批/执行中先取消；文件提交阶段（`committing()`）拒绝删除并提示。

## 5. 待人工测试

- [ ] 真实 Windows 路径矩阵：绝对路径、`..`、盘符、UNC、ADS、保留名、尾点/空格、中文/空格/长路径、junction/symlink/挂载点。
- [ ] 句柄级最终路径/文件 ID/reparse 校验；写入前后对象替换时拒绝。
- [ ] B1/B2/B3 修复后的复测：R1–R4、R6、R7（脱敏变体、凭据文件识别、artifact 文案/取回边界）。
- [ ] W4/W5：写入后查看工具卡/备份 `meta.json` 的 sha256，并与 `Get-FileHash` 对比。
- [ ] A1：`artifact.read` 带 offset/length 分段读取，确认每次重新脱敏、读不到本地原始存档。
- [ ] P4：重启启动器后旧审批不自动执行，只提示重新请求。
- [ ] P5：文件提交阶段删除/关窗不强行中断；小文件难复现时以自动化覆盖为准。
- [ ] 真实大实例的配置写入与回退；真实文件系统竞争/杀毒软件占用。
- [ ] 网络错误矩阵：401/403、429、断网、流中断、重定向、TLS 证书错误、代理、400 错误体。见 [ACCEPTANCE_LOG](../archive/ACCEPTANCE_LOG.md) 的「延后：网络错误」一节。

网络错误和上面的 G1B 复测项未完成前，G1B 不算通过。

## 6. 未覆盖

| 项 | 状态 |
| --- | --- |
| 真实文件系统竞争/杀毒软件占用 | 未覆盖 |
| 真机高 DPI/读屏 | 未覆盖 |
| junction 无法创建的平台 | 夹具 `SKIP`，不计入通过 |
| 日志字段投影（用户名/参数/路径/硬件指纹） | 留给 Stage 2 |


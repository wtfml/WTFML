# 智能体 Stage 2B：维护工作流与启动器工具

本文给开发者和测试看 Stage 2B 的现行行为、代码位置、盘上格式、实现边界和仍需人工验收的项。聊天底座见 [AGENT_STAGE1A.md](AGENT_STAGE1A.md)，文件工具见 [AGENT_STAGE1B.md](AGENT_STAGE1B.md)，整体设计与阶段计划见 [AGENT.md](AGENT.md)，安全边界见 [SECURITY_BOUNDARIES.md](SECURITY_BOUNDARIES.md)。MCP 属于同阶段的另一个子项（G2A），不在本文范围。

## 1. 当前能力

### 1.1 模式（技能）

界面在输入岛上方多了一行模式胶囊，五个技能编译进程序：

| 技能 id | 名称 | 只读 | 要求先提交计划 | 必须碰过的工具 |
| --- | --- | --- | --- | --- |
| `general` | 自由对话 | 否 | 否 | — |
| `inspection` | 实例体检 | 是 | 否 | `diagnostics.startup` |
| `crash-rescue` | 崩溃急救 | 否 | 是 | `diagnostics.crash` |
| `upgrade` | 版本升级 / 移植 | 否 | 是 | `upgrade.inspect` |
| `mods` | 模组维护 | 否 | 是 | `mods.list` |

- 当前模式记在会话元数据 `skill` 字段里，换会话时跟着会话走；换模式会作废旧计划，证据保留。
- 技能只能缩小工具集合。只读技能里写工具在复合引擎就被拒绝，不会走到工具执行；要求计划的技能里写工具必须在**已批准计划的当前这一步**。
- 自由对话不限制工具集合，沿用 Stage 1B 的整套文件工具，另外多了启动器工具。

### 1.2 启动器工具（19 个）

全部经 `AgentLauncherHost`，实例来自会话锁定，工具参数里没有实例 id。

只读自动执行：

- `instance.list`：默认只返回实例总数；`detail=full` 返回名称/版本/加载器，**逐次确认**。
- `instance.context`：锁定实例的元数据白名单；未锁定回 `needsInstanceLock`。
- `diagnostics.startup`：调 `StartupDiagnosis::analyze()`，结果经 `AgentDiagnosticProjection::startup()` 投影。
- `diagnostics.crash`：`GameExitDiagnosis` 事实 + 规则提示，经 `crash()` 投影。
- `logs.tail`：`logs/latest.log`（缺失时 `logs/wtfml.log`）末尾，最多 500 行、读盘上限 256 KB；本机路径替换成 `<path>`，凭据形态内容遮挡。
- `crash.list`：默认只给条数与最新一份概况；`detail=full` 列文件名，逐次确认。
- `instance.integrity`：`VersionManager::integrityPlan()` + `VersionIntegrity::check()`，8 秒时间预算，只给分组计数，不给路径。
- `mods.list`：默认只给数量与启停统计；`detail=full` 列完整文件名，逐次确认。
- `settings.get`：只返回白名单键的非路径字段（`java.path` 只写不读）。
- `java.list`：本机 Java 主版本。
- `memory.suggest`：复用 `GlobalJavaSettings::suggestXmxMb()` / `suggestXmsMb()`。
- `backup.list`：维护备份 + 启动器自己的 zip 备份（后者只列文件名，`restorable=false`）。
- `upgrade.inspect`：按 `TransferPlanner::compatibility()` 给候选目标与兼容性结论，不执行升级。

写 / 高风险，逐次审批：

- `settings.set`：白名单键，审批卡写旧值 → 新值；失败不推进“已保存”基线。
- `mods.setEnabled`：只改扩展名（`.jar` ↔ `.jar.disabled`），可原路改回。
- `backup.create`：审批卡显示范围、文件数、字节数、保存位置；`owner=agent`，写 `backups/maintenance/`。
- `backup.restore`：恢复前先做一份快照；共享/外部备份只引用不改写（当前实现只恢复 Agent 自有备份）。

不写盘、直接开现有入口：

- `upgrade.openWizard`、`launcher.openPage`（实例设置 / 日志 / 备份 / 完整性）。

### 1.3 工作流

```
Collect → AwaitPlanApproval → Execute*（每步审批）→ Verify → Report
                     ↘ 用户拒绝 → Collect
                     ↘ 某步失败 → Failed（写工具全锁，只能 workflow.finish 说明原因）
```

- `workflow.submitPlan`：结构化提交 `{goal, steps:[{tool, arguments, reason, verify}]}`。本地逐条校验：工具存在且在当前模式白名单里、参数过该工具的 JSON Schema、只读模式不许写、至少要有一个修复步骤、至少一个验证步且验证步必须是只读工具、步数 ≤ 20、目标 ≤ 500 字。校验通过才出计划卡。
- 批准计划 ≠ 批准写入：每个写步骤执行时仍单独弹审批。
- 计划按顺序执行；不是当前这一步的写工具会被拒绝并点名该做哪一步。
- `workflow.finish`：本地核对“计划已批准 + 每步都有结果 + 跑过验证步 + 必需工具碰过 + 至少一条证据 id”；缺一项就拒绝并说缺哪一项。失败过的会话不能报完成。

### 1.4 证据

- 每条工具结果带 `evidence: [{id, kind, source, capturedAt, sha256?, excerpt?}]`，id 从 `E1` 起。
- 证据片段过两层：`AgentRedaction` 遮凭据，`sanitizeEvidence()` 丢掉含本机路径的整句（`X:\`、UNC、`/Users/`、`/home/`、`file://`）。
- 诊断投影后的 JSON 里没有 Java 路径、数据根、日志/崩溃报告绝对路径、原始 stack trace。

### 1.5 界面

- 模式胶囊：`objectName` 为 `agentSkill_<id>`，当前模式可勾选，`setSkills()` 重建后旧按钮立刻脱离父级（不会查到上一轮的控件）。
- 工作流状态行：`agentWorkflowStatus`，内容是「模式 / 阶段 / 计划步数 / 证据条数」，空串收起。
- 计划卡复用审批卡：标题为「修复计划」，预览区是逐步清单与参数，说明「批准计划不等于批准所有写入」。

## 2. 代码位置

| 模块 | 文件 |
| --- | --- |
| 技能定义 | `src/agent/AgentSkill.*` |
| 工作流状态机 | `src/agent/AgentWorkflow.*` |
| 计划卡与完成闸 | `src/agent/AgentWorkflowEngine.*` |
| 启动器工具 | `src/agent/AgentLauncherTools.*` |
| 启动器能力接口 | `src/agent/AgentLauncherHost.hpp` |
| 诊断字段投影 | `src/agent/AgentDiagnosticProjection.*` |
| 子引擎分派与技能闸 | `src/agent/AgentCompositeToolEngine.*` |
| 产品宿主实现 | `src/controllers/AgentLauncherHostImpl.*` |
| 启动诊断事实的唯一出处 | `MainWindow::startupDiagnosisInputs()`（`src/MainWindowInstanceEntry.cpp`） |
| 兼容性判据 | `TransferPlanner::compatibility()`（`src/operations/TransferPlanner.*`） |
| 多文件维护备份 | `AgentBackupIndex::createFromFiles()` / `readFiles()` |
| 界面 | `ui/AgentPage.*`、`ui/AgentViewTypes.hpp` |

依赖方向仍是 `ui/Agent*` → `AgentController` → `src/agent/`；`src/agent/` 只认 `AgentLauncherHost` 纯虚表，`AgentLauncherHostImpl` 在 `src/controllers/` 里才认识 `VersionManager` / `MainWindow`。

## 3. 盘上格式补充

```text
agent/backups/maintenance/<backupId>/
  original.enc     # DPAPI；多文件容器：{"format":"agent-maintenance-files","files":[{path,data(base64)}]}
  meta.json        # owner/source/sourceSessionId/instanceId/reason/originalPaths[]/sha256/size/createdAt
```

- 单文件备份（`createFromBytes`）与多文件备份（`createFromFiles`）在盘上是同一种形状，`readFiles()` 两种都读得回来；不认识的容器退回“单文件 + 元数据里第一条路径”。
- 备份内容里不放绝对路径：`originalPaths` 全是实例目录内的相对路径。
- 恢复顺序：先按当前内容做一份快照（`reason` 以 `backup.restore 恢复前快照` 开头），再逐文件用 `QSaveFile` 原子写回；路径越界或绝对路径直接拒绝。

## 4. 实现边界

- `instance.integrity` 在 UI 线程同步跑，8 秒预算后回部分结果（`checked` 是清单总条数，`problems` 是已比出来的那些）。大实例上可能只比了一部分。
- `backup.restore` 只恢复 `owner=agent` 的维护备份；启动器自己的 zip 备份只列出来。恢复现有 zip 需要起外部解压进程，Stage 2B 不做。
- `mods.setEnabled` 只改扩展名，不搬文件、不跨目录；目标文件名已存在时拒绝。
- `settings.get/set` 的白名单是 `memory.auto`、`memory.xmx`、`memory.xms`、`java.autoSelect`、`java.path`（只写）、`args.extraJvm`、`launch.afterLaunch`、`launch.showGameLog`、`launch.processPriority`。
- `upgrade.inspect` 只给已安装版本之间的兼容结论；执行一律走现有升级向导，Agent 不搬文件。
- `crashFacts` 的退出码来自启动账本的那一档：账本不存退出码，所以 `exitCode=0` 表示“不知道”，不编 NTSTATUS。
- 没有 MCP，没有任意命令，没有联网工具。
- 日志里的 Windows 用户名只在“路径形态”下被替换；裸用户名（例如 `C:\Users\张三` 之外的 `张三 加入游戏`）仍可能出现，留给 Stage 3 的日志字段投影。

## 5. 自动覆盖

| 套件 | 命令 | 本次结果 |
| --- | --- | --- |
| `verify_agent` | `build/verification-mingw13/tests/verify_agent.exe` | 535 项通过 |
| `ui_check_agent` | `build/verification-mingw13/tests/ui_check/ui_check_agent.exe` | 64 项通过 |
| `mw_check_agent` | `build/verification-mingw13/tests/mw_check/mw_check_agent.exe` | 70 项通过 |

新增用例：

- `tests/core_modules/AgentDiagnosticProjectionTests.cpp`：允许字段保留；绝对路径/Java 路径/用户名剔除；空结果不报成功；`containsLocalPath` / `scrubPaths`。
- `tests/core_modules/AgentLauncherToolsTests.cpp`：19 个工具的分派、审批与拒绝；`instance.list` 默认不带名称；`settings.get` 不含 `java.path`；`mods.setEnabled`/`settings.set`/`backup.create`/`backup.restore` 拒绝时宿主没被调用；模型传入 `instanceId` 被 schema 拒绝。
- `tests/core_modules/AgentWorkflowTests.cpp`：计划字段/工具/只读/验证步/步数校验；计划卡拒绝后作废；计划外写工具被拦；跳步被拦；某步失败后阶段 `Failed` 且写工具全锁；完成闸缺证据/缺必需工具时拒绝。
- `tests/core_modules/AgentBackupLifecycleTests.cpp`：多文件备份往返（文本 + 二进制）、`readFiles` 兼容单文件备份。
- `tests/ui_check/modules/UiAgentCheck.cpp`：模式胶囊按 id 摆出、选中态跟模式走、状态行空串收起。
- `tests/mw_check/modules/MwAgentCheck.cpp`：真 MainWindow 里切模式并写入会话、只读模式拒绝写工具且不动盘、`mods.list` 走真宿主只给数量。

## 6. 待人工测试（G2B）

用真实临时实例逐项过一遍，记录在 [ACCEPTANCE_LOG](../archive/ACCEPTANCE_LOG.md)。

- [ ] 切到「实例体检」跑一遍：结论分级是否有据、每条是否带证据 id、有没有把路径/用户名发给模型（看工具卡里的结果预览与会话 JSON）。
- [ ] 「崩溃急救」：先用真实 OOM 或 Java 版本不匹配跑崩一次，看它给的最可能原因与现有规则一致；提交计划 → 拒绝一次 → 再提交 → 批准；每个写步骤是否仍单独弹审批。
- [ ] 计划跳步：批准计划后手动让模型先做后面的写步骤，确认被拒绝并点名当前该做哪一步。
- [ ] 某一步失败（例如目标文件被占用）：确认阶段变 `Failed`、`workflow.finish` 被拒并说明原因、后续写工具全部被拒。
- [ ] 修改内存/Java 走 `settings.set`：审批卡显示旧值 → 新值；拒绝时设置不变；批准后设置页里能看到新值。
- [ ] `mods.setEnabled` 停用一个真实模组：文件变成 `.jar.disabled`；再启用能改回来；目标文件已存在时被拒。
- [ ] `backup.create` 备份 config+mods：审批卡显示文件数与字节数；批准后 `%LOCALAPPDATA%\WTFML\agent\backups\maintenance\` 里有那一份；删除会话后备份还在。
- [ ] `backup.restore`：先改坏一个配置文件，再恢复，确认内容回来、恢复前快照存在、`meta.json` 里两边的 `originalPaths` 都是相对路径。
- [ ] `instance.integrity` 在大实例上跑：确认界面没有长时间假死（8 秒预算）、结果只给分组计数。
- [ ] `logs.tail` / `crash.list`：确认真实日志里的 `C:\Users\…` 变成 `<path>`，口令/Tokens 被遮挡。
- [ ] `upgrade.inspect` + `upgrade.openWizard`：检查结论与实例设置的「迁移 / 升级」页一致；向导由用户在原界面操作。
- [ ] 占用中的实例（正在安装 / 搬文件 / 运行中）：确认写工具被拒或审批卡提示占用，不出现两边同时写同一个目录。
- [ ] 会话锁定不受主页切换影响：体检在做时把主页切到另一个实例，后续工具仍只操作锁定的那个。

## 7. 未覆盖

| 项 | 状态 |
| --- | --- |
| MCP（stdio、确认闸、工具审批） | Stage 2A，未开始 |
| 恢复启动器自己的 zip 备份 | 未覆盖，只列出 |
| 日志里的裸用户名、硬件指纹 | 未覆盖，留给 Stage 3 |
| `instance.integrity` 超出 8 秒预算的部分 | 未覆盖，回部分结果 |
| 真机高 DPI / 读屏 | 未覆盖 |
| 大实例上的长时工作流 | 未覆盖 |

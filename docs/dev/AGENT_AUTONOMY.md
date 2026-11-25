# 智能体自主修复（Stage 5）开发记录

给接手 Stage 5 的开发、评审和验收的人看：S5.0–S5.7 已在 2026-11-24 全部完成，**现行行为**写在 [AGENT](../features/AGENT.md) 的 §9、§11、§12，本文只留"每个阶段交付了什么、代码在哪、还剩什么、怎么复跑"。未完成项只在 [ROADMAP](../ROADMAP.md) 维护。

## 1. 完成情况

| 阶段 | 交付（一句话） | 现行行为 | 验收记录 |
| --- | --- | --- | --- |
| S5.0 | 技能提示词外置到 `assets/agent/prompts/`，`AgentSkill.cpp` 只留元数据 | AGENT §11.4 | 2026-11-24 提交 |
| S5.1 | 隔离工作区 + `instance.testLaunch`（异步、离线身份、里程碑判定、Job Object 清理） | AGENT §9.4.2 | 2026-11-24 提交 |
| S5.2 | 修复事务：基线、工作区写路由、差异、提交重放、回滚 | AGENT §9.4.3 | 2026-11-24 提交 |
| S5.3 | 崩溃规则与里程碑知识文件；`crashSnapshot`/`logs.search`/`logs.readRange`/`crash.readFull`；mixin 归属 | AGENT §9.4.4 | ACCEPTANCE_LOG 2026-11-24 |
| S5.4 | 共用 `ModDeclarations`；`mods.analyzeCompatibility`/`mods.verifyFiles`/`mods.installFix`/`instance.repairCore`；平台只发 SHA-1 | AGENT §9.4.5 | ACCEPTANCE_LOG 2026-11-24 |
| S5.5 | 依赖感知二分 `mods.bisect`（纯状态机 + 工作区重置/恢复） | AGENT §9.4.6 | ACCEPTANCE_LOG 2026-11-24 |
| S5.6 | Prompt V2 与知识分层；自由对话显式白名单；提示词预算 | AGENT §11.4 | ACCEPTANCE_LOG 2026-11-24 |
| S5.7 | Auto-safe 事务授权（D12）：一次批准有范围、有时效的事务，验证失败自动回滚，结束写审计 | AGENT §2 D12、§9.4.7 | ACCEPTANCE_LOG 2026-11-24 |

## 2. 代码在哪

| 能力 | 模块 |
| --- | --- |
| 本地修复病例库（P2） | `AgentRepairCases`、`AgentRepairToolEngine`、`AgentLauncherTools`（`priorCases`）、设置页「记住修复病例」 |
| 提示词与技能 | `AgentPromptLibrary`、`AgentSkill`、`assets/agent/prompts/` |
| 诊断知识（崩溃规则、里程碑） | `AgentKnowledgeRules`、`assets/agent/knowledge/` |
| 崩溃解析、日志索引、mixin 归属 | `AgentCrashSnapshot`、`AgentLogIndex`、`AgentMixinOwnership` |
| 模组声明（无 UI） | `src/operations/ModDeclarations`（升级评估与智能体共用） |
| 模组兼容、修复、二分 | `AgentModCompat`、`AgentModToolEngine`、`AgentModBisect`、`src/controllers/AgentModPlatform` |
| 事务与工作区 | `AgentRepairTransaction`、`AgentRepairToolEngine`、`AgentRepairRunner`、`AgentWorkspaceStore` |
| 测试启动 | `AgentTestLaunch`、`AgentTestLaunchToolEngine`、`AgentTestLaunchRunner` |
| Auto-safe 授权与审计 | `AgentAutoSafe` + `AgentCompositeToolEngine`（替用户点“允许”、收尾写审计） |
| 业务宿主 | `AgentLauncherHost` / `AgentLaunchHost` / `AgentRepairHost` / `AgentModHost`，实现都在 `src/controllers/AgentLauncherHostImpl.*` |

## 3. 数据与恢复

```text
%LOCALAPPDATA%/WTFML/agent/
  transactions/<id>/journal.json     事务状态、操作、基线、验证、备份引用
  transactions/<id>/diffs/           文本差异与文件清单
  transactions/<id>/backups/         该事务触达文件的备份
  transactions/<id>/audit.json       Auto-safe 审计（范围、操作、验证、回滚、结果）
  repair-cases/<id>.json             本地修复病例（症状键/动作标签/验证结果，默认关闭）
  workspaces/<id>.json               工作区登记：来源、用途、状态、日志位置
  workspaces/<id>/logs/<launchId>/   测试启动原始日志（本地，不进模型）
  staging/                           下载与二分快照的暂存区（结束后删）
```

工作区物理目录在 `<游戏根>/cache/agent-workspaces/<id>/`。事务日志只记相对/受控路径，不把绝对路径发给模型。工作区默认保留；**启动恢复扫描与自动清理仍未接到 UI**，`repair.status` 只列状态，不自动续跑也不删未知目录。

## 4. 未做与限制

| 项 | 状态 |
| --- | --- |
| 修复病例库（P2）的规模 | 已实现（2026-11-25）：≤200 条、每条 ≤12 个动作标签；不按实例/游戏版本分桶，跨机器要手拷 `agent/repair-cases/`。写/读上限只有单元测试，没有实盘大库测量 |
| 真机验收 A9–A13 | 未做：真实 1.20.1 实例的启动/提交/回滚、120 模组二分、Auto-safe 一次批准跑完、真实模型行为，清单见 AGENT §13.3 |
| CurseForge 修复 | 只接 Modrinth，不按名称/slug 猜项目 |
| `logs.search` 正则与异步扫描 | 只有字面量 + 6 s 预算；100 MB 实测见 [PERFORMANCE](PERFORMANCE.md) |
| 工作区自动清理、启动恢复扫描 | 未接到 UI |
| 性能数字 | 模型扫描、平台查询、二分每轮耗时、Auto-safe 审计未量，见 PERFORMANCE「未覆盖」 |
| 测试启动窗口 | 会打开真实游戏窗口，Windows 下不做无头渲染 |
| 世界/存档 | 只在副本世界验证；事务不提交世界，也不做原件提交后验证 |

## 5. 怎么复跑

```powershell
./build/verification-mingw13/tests/verify_agent.exe                      # 核心：工具/提示词/事务/二分/Auto-safe
./build/verification-mingw13/tests/ui_check/ui_check_agent.exe           # 智能体页（offscreen）
./build/verification-mingw13/tests/mw_check/mw_check_agent.exe           # 真主窗口 + 假模型
./tools/check-agent-release.ps1                                          # 提示词/知识文件/审批闸/预算
```

夹具在 `tests/meta/agent/`（崩溃报告与日志的结构样本）和 `tests/core_modules/Agent*Tests.cpp`（假宿主驱动）。最新计数见 [TEST_RUN_HISTORY](../archive/TEST_RUN_HISTORY.md)；验收流水见 [ACCEPTANCE_LOG](../archive/ACCEPTANCE_LOG.md)。100 MB 日志规模测量：`WTFML_AGENT_LOG_SCALE_MB=100 ./build/verification-mingw13/tests/verify_agent.exe`（只打印、不断言）。

# 内嵌诊断知识

给改规则的人看：这里放两件事，规则由 `src/agent/AgentKnowledgeRules.cpp` 加载，不写进提示词，也不编进 C++ 常量。

- `crash_rules.json`：崩溃签名规则。`diagnostics.crashSnapshot` 用它决定 `phase`、`matchedRules`、`recommendedTests`、`suggestedAction`，并给嫌疑模组的置信度打底。
- `launch_milestones.json`：测试启动的日志里程碑标记。`AgentTestLaunchMilestones` 按它判断主菜单、世界加载完成和日志级崩溃。

两个文件都在 `assets/agent/agent.qrc` 里登记，资源路径是 `:/agent/knowledge/`。改完跑 `pwsh tools/check-agent-release.ps1`（校验 JSON、BOM、大小、qrc 登记）和 `verify_agent`。

## crash_rules.json 字段

| 字段 | 约束 | 说明 |
| --- | --- | --- |
| `id` | `^[A-Z][A-Z0-9_]{2,31}$`，全文件唯一 | 报告里的 `matchedRules` 就是它 |
| `priority` | 1–100 | 越大越先；`phase`、`suggestedAction` 取最高优先级命中规则的值 |
| `phase` | `startup` / `mod_initialization` / `config_load` / `world_load` / `runtime` / `native` / `shutdown` | 崩溃阶段 |
| `match` | 1–6 条，每条 1–200 字符 | 字面量，大小写不敏感；**任一命中**即算这条规则命中 |
| `requires` | 0–4 条，每条 1–200 字符 | 额外条件：每条都要在文本里出现 |
| `suspects` | 0–8 个 mod id | 规则自带线索；只当“被提到”，置信度不超过 0.4 |
| `confidence` | 0–1 | 规则基础置信度；嫌疑模组在它上面按证据加减 |
| `confidenceBasis` | 1–120 字符 | 报告里 `confidenceBasis` 的前半句 |
| `recommendedTests` | 0–6 条 | 建议的下一步验证 |
| `suggestedActions` | 0–4 条 | 第一条进报告的 `suggestedAction` |

坏规则（字段缺失、id 重复、字符串超长）跳过并记进 `problems`，其余规则照常加载：一条规则写错不该让整个工具失去结论。

## 置信度怎么算

嫌疑模组的 `role` 与 `confidence` 由证据类型决定，不由规则随便给：

| 证据 | `role` | `confidence` |
| --- | --- | --- |
| `Mixin apply for mod X failed`，或 mod id 出现在失败区栈帧里 | `candidate` | 规则 `confidence` + 0.12 |
| 崩溃文本点名的 mixin 配置能由 JAR 声明归属到某个模组，或 mod id 出现在失败区文本里 | `candidate` | 规则 `confidence` + 0.05 |
| 只在报告头 `Suspected Mods`、模组清单或规则 `suspects` 里出现 | `related` | `min(规则 confidence, 0.4)` |
| 只有文件名，读不到声明 | `related` | 0.2，`basis` 写 `unknown` |

封顶 0.95，不返回 1.0。规则命中但证据不足时 `suspects` 可以是空数组：`recommendedTests` 照样给。

## launch_milestones.json

三个键必须齐（`main_menu`、`world_loaded`、`crash`），每个是 1–8 条、每条 1–200 字符的字面量，大小写敏感（游戏原文就这么打）。读不到文件时三个判定都返回 false，测试启动会一直等不到里程碑 —— 这是故意的：宁可超时，也不要凭空认为“进主菜单了”。

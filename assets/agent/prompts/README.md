# 技能提示词

`AgentPromptLibrary` 从 Qt 资源 `:/agent/prompts/<id>.md` 读取这些文件。组合顺序固定为 `common.md` + 技能文件 +（崩溃急救/模组维护）`repair_protocol.md`，用空行连接。

- 文件用 UTF-8、无 BOM、LF 换行；单文件不超过 64 KB，组合后不超过 64 KB。
- 技能 id 到文件名的映射在 `src/agent/AgentSkill.cpp`：`crash-rescue` → `crash_rescue.md`。
- 改提示词同时递增 `AgentSkill.promptVersion`，并跑 `verify_agent` 与 `tools/check-agent-release.ps1`。
- 这里放模型指令，不放界面文案，不包 `tr()`。
- 新增文件要登记到 `assets/agent/agent.qrc`，否则加载失败。

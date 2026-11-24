# 技能提示词

`AgentPromptLibrary` 从 Qt 资源 `:/agent/prompts/<id>.md` 读取这些文件。组合顺序固定为 `common.md` + 技能文件 +（崩溃急救/模组维护）`repair_protocol.md`，用空行连接。

- 文件用 UTF-8、无 BOM、LF 换行。
- 预算：`common.md` ≤4 KB，单个技能 ≤8 KB，`common + 技能 + repair_protocol` ≤16 KB。超预算的规则进 `assets/agent/knowledge/` 或工具，不进提示词；`AgentPromptLibrary::load()` 与 `tools/check-agent-release.ps1` 都会拦。
- 技能 id 到文件名的映射在 `src/agent/AgentSkill.cpp`：`crash-rescue` → `crash_rescue.md`。
- 提示词只引用当期工具白名单里存在的工具（`verify_agent` 逐条核对），不背崩溃签名清单（签名在知识库，`check-agent-release.ps1` 核对）。
- 改提示词同时递增 `AgentSkill.promptVersion`，并跑 `verify_agent` 与 `tools/check-agent-release.ps1`。
- 这里放模型指令，不放界面文案，不包 `tr()`。
- 新增文件要登记到 `assets/agent/agent.qrc`，否则加载失败。

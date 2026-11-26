# Skill Prompts

`AgentPromptLibrary` loads these files from the Qt resource path `:/agent/prompts/<id>.md`. Composition order is fixed as `common.md` + skill file + (`repair_protocol.md` for `crash-rescue` / `mods`), joined with blank lines.

- Use UTF-8 without BOM and LF (`\n`) line endings.
- Byte budgets: `common.md` <= 4 KB, single skill <= 8 KB, combined `common + skill + repair_protocol` <= 16 KB. Rules exceeding the budget belong in `assets/agent/knowledge/` or tools, not prompts; both `AgentPromptLibrary::load()` and `tools/check-agent-release.ps1` enforce these limits.
- Skill ID to filename mapping is in `src/agent/AgentSkill.cpp` (e.g. `crash-rescue` -> `crash_rescue.md`).
- Prompts may only reference tools present in the skill's allowlist (`AgentSkillsTests` checks every tool id) and must not hardcode crash signature lists (signatures live in the knowledge files and are checked by `check-agent-release.ps1`).
- Whenever a prompt changes, increment `AgentSkill.promptVersion` and run `AgentSkillsTests` and `tools/check-agent-release.ps1`.
- Store model instructions only—no UI copy and no `tr()`.
- Register any newly added files in `assets/agent/agent.qrc` so loading succeeds.

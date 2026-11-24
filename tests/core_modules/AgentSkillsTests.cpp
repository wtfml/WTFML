/**
 * @file AgentSkillsTests.cpp
 * @brief 内置技能（模式）：自由对话 + Stage 2 的四条工作流，白名单与只读约束逐条钉住。
 */

#include "Support.hpp"

#include "agent/AgentSkill.hpp"

void runAgentSkillsTests() {
    const QList<AgentSkill> skills = AgentSkillRegistry::builtins();
    check(skills.size() == 5, "技能：Stage 2 有五个内置技能", QString::number(skills.size()));
    const AgentSkill general = AgentSkillRegistry::find(QStringLiteral("general"));
    check(general.id == QStringLiteral("general"), "技能：general 可查");
    check(general.name == QStringLiteral("自由对话"), "技能：名字是自由对话");
    check(general.allowedTools.isEmpty(), "技能：自由对话不限制工具集合");
    check(!general.requiresPlan, "技能：自由对话不需要提交计划");
    check(general.systemPrompt.contains(QStringLiteral("工具")) &&
              general.systemPrompt.contains(QStringLiteral("用户确认")),
          "技能：系统提示与实际下发的工具一致（可读、写入须确认）");
    check(general.systemPrompt.contains(QStringLiteral("证据")),
          "技能：系统提示要求引用证据");

    const AgentSkill inspection = AgentSkillRegistry::find(QStringLiteral("inspection"));
    check(inspection.name == QStringLiteral("实例体检"), "技能：体检已注册");
    check(inspection.readOnly, "技能：体检是只读");
    check(!inspection.requiresPlan, "技能：体检不要求计划");
    check(inspection.allowedTools.contains(QStringLiteral("diagnostics.startup")),
          "技能：体检能跑启动诊断");
    check(!inspection.allowedTools.contains(QStringLiteral("fs.write")),
          "技能：体检的工具集里没有写工具");
    check(inspection.requiredTools.contains(QStringLiteral("diagnostics.startup")),
          "技能：体检必须跑启动诊断才能报完成");

    const AgentSkill crash = AgentSkillRegistry::find(QStringLiteral("crash-rescue"));
    check(crash.requiresPlan, "技能：崩溃急救必须先提交计划");
    check(!crash.readOnly, "技能：崩溃急救允许在计划内写");
    check(crash.allowedTools.contains(QStringLiteral("settings.set")),
          "技能：崩溃急救能用设置工具");
    check(crash.allowedTools.contains(QStringLiteral("backup.create")),
          "技能：崩溃急救能建备份");
    check(crash.allowedTools.contains(QStringLiteral("workflow.submitPlan")) &&
              crash.allowedTools.contains(QStringLiteral("workflow.finish")),
          "技能：崩溃急救能提交计划与报告");

    const AgentSkill upgrade = AgentSkillRegistry::find(QStringLiteral("upgrade"));
    check(upgrade.name.contains(QStringLiteral("升级")), "技能：升级技能已注册");
    check(!upgrade.allowedTools.contains(QStringLiteral("settings.set")),
          "技能：升级模式不改设置");

    const AgentSkill mods = AgentSkillRegistry::find(QStringLiteral("mods"));
    check(mods.allowedTools.contains(QStringLiteral("mods.setEnabled")),
          "技能：模组维护能启停模组");
    check(mods.requiredTools.contains(QStringLiteral("mods.list")),
          "技能：模组维护必须先列清单");

    check(AgentSkillRegistry::orderedIds().size() == 5, "技能：界面顺序表有五项");
    check(AgentSkillRegistry::orderedIds().constFirst() == QStringLiteral("general"),
          "技能：自由对话排在最前");
    check(AgentSkillRegistry::contains(QStringLiteral("general")), "技能：注册表 contains");
    check(!AgentSkillRegistry::contains(QStringLiteral("diagnose")),
          "技能：未注册的 id 仍然被拒");
    check(AgentSkillRegistry::find(QStringLiteral("missing")).id.isEmpty(),
          "技能：未知 id 返回空结构");
}

/**
 * @file AgentSkillsTests.cpp
 * @brief 内置技能（模式）：提示词资源、白名单与只读约束逐条钉住。
 */

#include "Support.hpp"

#include "agent/AgentPromptLibrary.hpp"
#include "agent/AgentSkill.hpp"

void runAgentSkillsTests() {
    const QList<AgentSkill> skills = AgentSkillRegistry::builtins();
    check(skills.size() == 5, "技能：Stage 2 有五个内置技能", QString::number(skills.size()));

    const AgentSkill general = AgentSkillRegistry::find(QStringLiteral("general"));
    check(general.id == QStringLiteral("general"), "技能：general 可查");
    check(general.name == QStringLiteral("自由对话"), "技能：名字是自由对话");
    check(general.allowedTools.isEmpty(), "技能：自由对话不限制工具集合");
    check(!general.requiresPlan, "技能：自由对话不需要提交计划");
    check(general.promptId == QStringLiteral("general"), "提示词：general 的资源 id");
    check(general.promptVersion >= 1, "提示词：general 有版本号");
    check(general.promptProblem.isEmpty(), "提示词：general 资源可加载", general.promptProblem);
    check(general.promptHash.size() == 64, "提示词：general 有 SHA-256", general.promptHash);
    check(general.systemPrompt.contains(QStringLiteral("工具")) &&
              general.systemPrompt.contains(QStringLiteral("用户确认")),
          "技能：系统提示与实际下发的工具一致（可读、写入须确认）");
    check(general.systemPrompt.contains(QStringLiteral("证据")) &&
              general.systemPrompt.contains(QStringLiteral("无法确认")),
          "技能：系统提示要求引用证据");
    check(general.systemPrompt.contains(QStringLiteral("凭据")),
          "提示词：共享规则覆盖凭据");

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
    check(inspection.promptId == QStringLiteral("inspection"), "提示词：体检的资源 id");
    check(inspection.systemPrompt.contains(QStringLiteral("全程只读")),
          "提示词：体检保留只读约束");
    check(!inspection.systemPrompt.contains(QStringLiteral("workflow.submitPlan")),
          "提示词：体检不接修复协议");

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
    check(crash.promptId == QStringLiteral("crash_rescue"), "提示词：崩溃急救的资源 id");
    check(crash.systemPrompt.contains(QStringLiteral("workflow.submitPlan")) &&
              crash.systemPrompt.contains(QStringLiteral("workflow.finish")),
          "提示词：崩溃急救带修复协议");
    check(crash.systemPrompt.contains(QStringLiteral("验证步")) &&
              crash.systemPrompt.contains(QStringLiteral("回退方式")),
          "提示词：修复协议要求验证与回退");

    const AgentSkill upgrade = AgentSkillRegistry::find(QStringLiteral("upgrade"));
    check(upgrade.name.contains(QStringLiteral("升级")), "技能：升级技能已注册");
    check(!upgrade.allowedTools.contains(QStringLiteral("settings.set")),
          "技能：升级模式不改设置");
    check(upgrade.promptId == QStringLiteral("upgrade"), "提示词：升级的资源 id");
    check(upgrade.systemPrompt.contains(QStringLiteral("upgrade.inspect")),
          "提示词：升级保留只读检查入口");

    const AgentSkill mods = AgentSkillRegistry::find(QStringLiteral("mods"));
    check(mods.allowedTools.contains(QStringLiteral("mods.setEnabled")),
          "技能：模组维护能启停模组");
    check(mods.requiredTools.contains(QStringLiteral("mods.list")),
          "技能：模组维护必须先列清单");
    check(mods.promptId == QStringLiteral("mods"), "提示词：模组维护的资源 id");
    check(mods.systemPrompt.contains(QStringLiteral("workflow.submitPlan")),
          "提示词：模组维护带修复协议");

    for (const AgentSkill &skill : skills) {
        check(!skill.promptHash.isEmpty(), "提示词：每个技能都有哈希", skill.id);
        check(skill.systemPrompt.size() <= AgentPromptLibrary::maxPromptBytes(),
              "提示词：每个技能不超过硬上限", skill.id);
        check(skill.systemPrompt.contains(QStringLiteral("证据 id")),
              "提示词：共享规则覆盖每个技能", skill.id);
    }

    QString missingProblem;
    const AgentPrompt missing = AgentPromptLibrary::load(QStringLiteral("missing"), &missingProblem);
    check(missing.text.isEmpty() && !missingProblem.isEmpty(),
          "提示词：缺失资源明确报错", missingProblem);
    check(AgentPromptLibrary::resourcePath(QStringLiteral("../common")).isEmpty(),
          "提示词：非法 id 不拼资源路径");
    check(AgentPromptLibrary::validId(QStringLiteral("crash_rescue")) &&
              !AgentPromptLibrary::validId(QStringLiteral("crash/rescue")),
          "提示词：id 只接受安全字符");
    check(AgentPromptLibrary::maxPromptBytes() > 0, "提示词：有大小上限");

    QString composeProblem;
    const AgentPrompt withoutProtocol =
        AgentPromptLibrary::compose(QStringLiteral("crash_rescue"), false, &composeProblem);
    check(withoutProtocol.ok() && composeProblem.isEmpty(),
          "提示词：组合技能不带协议");
    check(!withoutProtocol.text.contains(QStringLiteral("workflow.submitPlan")),
          "提示词：不带协议时不出现计划工具");
    const AgentPrompt withProtocol =
        AgentPromptLibrary::compose(QStringLiteral("crash_rescue"), true, &composeProblem);
    check(withProtocol.ok() && composeProblem.isEmpty(),
          "提示词：组合技能带协议");
    check(withProtocol.text.contains(QStringLiteral("workflow.submitPlan")),
          "提示词：带协议时包含计划工具");

    check(AgentSkillRegistry::orderedIds().size() == 5, "技能：界面顺序表有五项");
    check(AgentSkillRegistry::orderedIds().constFirst() == QStringLiteral("general"),
          "技能：自由对话排在最前");
    check(AgentSkillRegistry::contains(QStringLiteral("general")), "技能：注册表 contains");
    check(!AgentSkillRegistry::contains(QStringLiteral("diagnose")),
          "技能：未注册的 id 仍然被拒");
    check(AgentSkillRegistry::find(QStringLiteral("missing")).id.isEmpty(),
          "技能：未知 id 返回空结构");
}

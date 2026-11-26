/**
 * @file AgentSkillsTests.cpp
 * @brief 内置技能（模式）：提示词资源、白名单与只读约束逐条钉住。
 */

#include "Support.hpp"

#include "agent/AgentKnowledgeRules.hpp"
#include "agent/AgentPromptLibrary.hpp"
#include "agent/AgentSkill.hpp"
#include "agent/AgentTool.hpp"

void runAgentSkillsTests() {
    const QList<AgentSkill> skills = AgentSkillRegistry::builtins();
    check(skills.size() == 5, "技能：Stage 2 有五个内置技能", QString::number(skills.size()));

    const AgentSkill general = AgentSkillRegistry::find(QStringLiteral("general"));
    check(general.id == QStringLiteral("general"), "技能：general 可查");
    check(general.name == QStringLiteral("自由对话"), "技能：名字是自由对话");
    check(!general.allowedTools.isEmpty(), "技能：自由对话有显式工具白名单（S5.6）");
    check(general.allowedTools.contains(QStringLiteral("fs.read")) &&
              general.allowedTools.contains(QStringLiteral("diagnostics.crashSnapshot")) &&
              general.allowedTools.contains(QStringLiteral("web.fetch")) &&
              general.allowedTools.contains(QStringLiteral("mcp__*")),
          "技能：自由对话保留文件/诊断/联网/MCP 工具");
    check(!general.allowedTools.contains(QStringLiteral("instance.testLaunch")) &&
              !general.allowedTools.contains(QStringLiteral("repair.begin")) &&
              !general.allowedTools.contains(QStringLiteral("repair.finish")) &&
              !general.allowedTools.contains(QStringLiteral("mods.installFix")) &&
              !general.allowedTools.contains(QStringLiteral("mods.bisect")) &&
              !general.allowedTools.contains(QStringLiteral("instance.repairCore")) &&
              !general.allowedTools.contains(QStringLiteral("workflow.submitPlan")),
          "技能：自由对话不带 Stage 5 修复与计划工具");
    check(general.allows(QStringLiteral("mcp__server__tool")) &&
              !general.allows(QStringLiteral("mods.bisect")),
          "技能：通配白名单只放行对应前缀");
    check(!general.requiresPlan, "技能：自由对话不需要提交计划");
    check(general.promptId == QStringLiteral("general"), "提示词：general 的资源 id");
    check(general.promptVersion >= 1, "提示词：general 有版本号");
    check(general.promptProblem.isEmpty(), "提示词：general 资源可加载", general.promptProblem);
    check(general.promptHash.size() == 64, "提示词：general 有 SHA-256", general.promptHash);
    check(general.systemPrompt.contains(QStringLiteral("tools")) &&
              general.systemPrompt.contains(QStringLiteral("user confirmation")),
          "技能：系统提示与实际下发的工具一致（可读、写入须确认）");
    check(general.systemPrompt.contains(QStringLiteral("no repair transactions")) &&
              general.systemPrompt.contains(QStringLiteral("Crash Rescue")),
          "提示词：自由对话说明修复要切模式");
    check(general.systemPrompt.contains(QStringLiteral("evidence")) &&
              general.systemPrompt.contains(QStringLiteral("cannot confirm")),
          "技能：系统提示要求引用证据");
    check(general.systemPrompt.contains(QStringLiteral("credentials")),
          "提示词：共享规则覆盖凭据");
    check(general.systemPrompt.contains(
              QStringLiteral("Reply in the language of the user's most recent message")) &&
              general.systemPrompt.contains(
                  QStringLiteral("interface language stated in the context")),
          "提示词：共享规则要求按最新消息或界面语言回复");

    const AgentSkill inspection = AgentSkillRegistry::find(QStringLiteral("inspection"));
    check(inspection.name == QStringLiteral("实例体检"), "技能：体检已注册");
    check(inspection.readOnly, "技能：体检是只读");
    check(!inspection.requiresPlan, "技能：体检不要求计划");
    check(inspection.allowedTools.contains(QStringLiteral("diagnostics.startup")),
          "技能：体检能跑启动诊断");
    check(!inspection.allowedTools.contains(QStringLiteral("fs.write")),
          "技能：体检的工具集里没有写工具");
    check(!inspection.allowedTools.contains(QStringLiteral("instance.testLaunch")),
          "技能：体检不做测试启动");
    check(inspection.requiredTools.contains(QStringLiteral("diagnostics.startup")),
          "技能：体检必须跑启动诊断才能报完成");
    check(inspection.promptId == QStringLiteral("inspection"), "提示词：体检的资源 id");
    check(inspection.systemPrompt.contains(QStringLiteral("strictly read-only")),
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
    check(crash.allowedTools.contains(QStringLiteral("instance.testLaunch")),
          "技能：崩溃急救能做隔离测试启动");
    check(crash.allowedTools.contains(QStringLiteral("repair.begin")) &&
              crash.allowedTools.contains(QStringLiteral("repair.finish")),
          "技能：崩溃急救能用修复事务");
    check(crash.allowedTools.contains(QStringLiteral("workflow.submitPlan")) &&
              crash.allowedTools.contains(QStringLiteral("workflow.finish")),
          "技能：崩溃急救能提交计划与报告");
    check(crash.promptId == QStringLiteral("crash_rescue"), "提示词：崩溃急救的资源 id");
    check(crash.systemPrompt.contains(QStringLiteral("workflow.submitPlan")) &&
              crash.systemPrompt.contains(QStringLiteral("workflow.finish")),
          "提示词：崩溃急救带修复协议");
    check(crash.systemPrompt.contains(QStringLiteral("verification step")) &&
              crash.systemPrompt.contains(QStringLiteral("rollback method")),
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
        check(skill.systemPrompt.contains(QStringLiteral("evidence id")),
              "提示词：共享规则覆盖每个技能", skill.id);
    }

    // ------------------------------------------------------------ Prompt V2（S5.6）
    {
        const AgentSkill crashV2 = AgentSkillRegistry::find(QStringLiteral("crash-rescue"));
        // 六类条款：执行优先、验证、回滚、证据、禁止猜测、审批边界。
        check(crashV2.systemPrompt.contains(QStringLiteral("act directly")),
              "提示词 V2：崩溃急救写了执行优先", crashV2.id);
        check(crashV2.systemPrompt.contains(QStringLiteral("repair.verify")) ||
                  crashV2.systemPrompt.contains(QStringLiteral("verify")),
              "提示词 V2：崩溃急救要求验证");
        check(crashV2.systemPrompt.contains(QStringLiteral("rollback")),
              "提示词 V2：崩溃急救要求失败回滚");
        check(crashV2.systemPrompt.contains(QStringLiteral("evidence id")),
              "提示词 V2：崩溃急救要求引用证据");
        check(crashV2.systemPrompt.contains(QStringLiteral("Do not guess")),
              "提示词 V2：崩溃急救禁止猜测");
        check(crashV2.systemPrompt.contains(QStringLiteral("approval")),
              "提示词 V2：崩溃急救写明审批边界");
        check(crashV2.systemPrompt.contains(QStringLiteral("diagnostics.crashSnapshot")) &&
                  crashV2.systemPrompt.contains(QStringLiteral("matchedRules")),
              "提示词 V2：崩溃急救先调 crashSnapshot 并按规则走");
        check(crashV2.systemPrompt.contains(QStringLiteral("fixed_verified")) &&
                  crashV2.systemPrompt.contains(QStringLiteral("fixed_unverified")) &&
                  crashV2.systemPrompt.contains(QStringLiteral("mitigated")) &&
                  crashV2.systemPrompt.contains(QStringLiteral("not_fixed")) &&
                  crashV2.systemPrompt.contains(QStringLiteral("needs_user")),
              "提示词 V2：崩溃急救写清五个报告状态");
        check(crashV2.systemPrompt.contains(
                  QStringLiteral("whether a tool, permission, or external information is missing")),
              "提示词 V2：需要用户时说清缺什么");
        check(crashV2.systemPrompt.contains(QStringLiteral("mods.bisect")),
              "提示词 V2：崩溃急救指向二分工具");

        // 提示词里不许背崩溃类型表：知识库里的签名串一个都不能出现。
        const AgentKnowledge::CrashRuleSet rules = AgentKnowledge::loadCrashRules();
        check(rules.ok(), "提示词 V2：知识库规则可用", rules.resourceProblem);
        QStringList leaked;
        for (const AgentSkill &skill : skills) {
            for (const AgentKnowledge::CrashRule &rule : rules.rules) {
                for (const QString &needle : rule.match) {
                    if (needle.size() >= 8 && skill.systemPrompt.contains(needle)) {
                        leaked.append(QStringLiteral("%1→%2").arg(skill.id, needle));
                    }
                }
            }
        }
        check(leaked.isEmpty(), "提示词 V2：没有崩溃签名清单", leaked.join(QStringLiteral("; ")));

        // 预算：common ≤4 KB、单技能 ≤8 KB、组合 ≤16 KB。
        const AgentPrompt common = AgentPromptLibrary::load(QStringLiteral("common"));
        check(common.ok() &&
                  common.text.toUtf8().size() <= AgentPromptLibrary::maxCommonPromptBytes(),
              "提示词 V2：common 在 4 KB 预算内",
              QString::number(common.text.toUtf8().size()));
        for (const AgentSkill &skill : skills) {
            check(skill.systemPrompt.toUtf8().size() <=
                      AgentPromptLibrary::maxComposedPromptBytes(),
                  "提示词 V2：组合后不超过 16 KB", skill.id);
            QString problem;
            const AgentPrompt single = AgentPromptLibrary::load(skill.promptId, &problem);
            check(single.ok() &&
                      single.text.toUtf8().size() <= AgentPromptLibrary::maxSkillPromptBytes(),
                  "提示词 V2：单个技能不超过 8 KB", skill.id);
        }

        // 提示词只引用当期白名单里存在的工具。
        QStringList outside;
        for (const AgentSkill &skill : skills) {
            for (const AgentToolSpec &spec : AgentToolRegistry::builtins()) {
                if (!skill.systemPrompt.contains(spec.name)) continue;
                if (!skill.allows(spec.name)) {
                    outside.append(QStringLiteral("%1→%2").arg(skill.id, spec.name));
                }
            }
        }
        check(outside.isEmpty(), "提示词 V2：提示词里的工具都在该模式白名单里",
              outside.join(QStringLiteral("; ")));

        // 修复协议只挂在崩溃急救/模组维护上。
        check(crashV2.systemPrompt.contains(QStringLiteral("workflow.submitPlan")),
              "提示词 V2：崩溃急救带修复协议");
        for (const AgentSkill &skill : skills) {
            if (skill.id == QStringLiteral("crash-rescue") || skill.id == QStringLiteral("mods")) {
                continue;
            }
            check(!skill.systemPrompt.contains(QStringLiteral("repair.begin")),
                  "提示词 V2：非修复模式不带事务协议", skill.id);
        }
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

/**
 * @file AgentSkill.cpp
 * @brief 内置技能清单（编译进程序，不从远程下载）；提示词正文在 assets/agent/prompts。
 */

#include "AgentSkill.hpp"

#include "AgentPromptLibrary.hpp"

namespace {

/// 只读诊断类工具：体检和崩溃急救共用的那一批。
QStringList readOnlyTools() {
    return {QStringLiteral("instance.list"),
            QStringLiteral("instance.context"),
            QStringLiteral("diagnostics.startup"),
            QStringLiteral("diagnostics.crash"),
            QStringLiteral("diagnostics.crashSnapshot"),
            QStringLiteral("logs.tail"),
            QStringLiteral("logs.search"),
            QStringLiteral("logs.readRange"),
            QStringLiteral("crash.list"),
            QStringLiteral("crash.readFull"),
            QStringLiteral("instance.integrity"),
            QStringLiteral("mods.list"),
            QStringLiteral("settings.get"),
            QStringLiteral("java.list"),
            QStringLiteral("memory.suggest"),
            QStringLiteral("backup.list"),
            QStringLiteral("upgrade.inspect"),
            QStringLiteral("upgrade.openWizard"),
            QStringLiteral("launcher.openPage"),
            QStringLiteral("fs.list"),
            QStringLiteral("fs.read"),
            QStringLiteral("artifact.read"),
            QStringLiteral("workflow.submitPlan"),
            QStringLiteral("workflow.finish"),
            QStringLiteral("repair.diff"),
            QStringLiteral("repair.status")};
}

/// 写类工具：必须在已批准的计划里才放行。
QStringList repairTools() {
    return {QStringLiteral("settings.set"),
            QStringLiteral("fs.write"),
            QStringLiteral("mods.setEnabled"),
            QStringLiteral("backup.create"),
            QStringLiteral("backup.restore"),
            QStringLiteral("instance.testLaunch"),
            QStringLiteral("repair.begin"),
            QStringLiteral("repair.verify"),
            QStringLiteral("repair.finish")};
}

AgentSkill make(const QString &id, const QString &name, const QString &description,
                const QString &promptId, bool includeRepairProtocol, QStringList tools,
                bool readOnly, bool requiresPlan, const QStringList &requiredTools,
                const QString &completion, int promptVersion = 1) {
    AgentSkill skill;
    skill.id = id;
    skill.name = name;
    skill.description = description;
    skill.promptId = promptId;
    skill.promptVersion = promptVersion;
    QString problem;
    const AgentPrompt prompt =
        AgentPromptLibrary::compose(promptId, includeRepairProtocol, &problem);
    skill.systemPrompt = prompt.text;
    skill.promptHash = prompt.hash;
    skill.promptProblem = problem;
    skill.allowedTools = tools;
    skill.requiredTools = requiredTools;
    skill.readOnly = readOnly;
    skill.requiresPlan = requiresPlan;
    skill.completionCriteria = completion;
    skill.maxSteps = 20;
    skill.version = 1;
    return skill;
}

}  // namespace

QList<AgentSkill> AgentSkillRegistry::builtins() {
    QList<AgentSkill> list;

    list.append(make(
        QStringLiteral("general"), QStringLiteral("自由对话"),
        QStringLiteral("对话 + 锁定实例内的受控文件工具与诊断工具。"),
        QStringLiteral("general"), false, {}, false, false, {}, {}));

    list.append(make(
        QStringLiteral("inspection"), QStringLiteral("实例体检"),
        QStringLiteral("只读：跑启动诊断/完整性/模组统计，列出问题与下一步。"),
        QStringLiteral("inspection"), false, readOnlyTools(), true, false,
        {QStringLiteral("diagnostics.startup")},
        QStringLiteral("至少一条证据；给出问题分级与下一步选项；全程没有写操作。")));

    list.append(make(
        QStringLiteral("crash-rescue"), QStringLiteral("崩溃急救"),
        QStringLiteral("诊断 + 计划 + 逐步审批修复 + 验证；每步可拒绝、可回退。"),
        QStringLiteral("crash_rescue"), true, QStringList(readOnlyTools() + repairTools()), false,
        true, {QStringLiteral("diagnostics.crash")},
        QStringLiteral("计划已批准；每步都有结果；至少一条证据；有验证步骤；"
                       "报告写明回退方式与未解决项。"),
        2));

    list.append(make(
        QStringLiteral("upgrade"), QStringLiteral("版本升级 / 移植"),
        QStringLiteral("只读检查升级目标，执行时打开现有升级向导。"),
        QStringLiteral("upgrade"), false,
        QStringList(readOnlyTools() + QStringList{QStringLiteral("backup.create")}), false, true,
        {QStringLiteral("upgrade.inspect")},
        QStringLiteral("给出兼容性结论与风险；执行交给现有向导；有证据。")));

    list.append(make(
        QStringLiteral("mods"), QStringLiteral("模组维护"),
        QStringLiteral("列模组、按现有规则查依赖、启停或备份；不猜文件名兼容性。"),
        QStringLiteral("mods"), true, QStringList(readOnlyTools() + repairTools()), false, true,
        {QStringLiteral("mods.list")},
        QStringLiteral("模组清单有证据；每个改动都有结果；不确定项写明。")));

    return list;
}

bool AgentSkillRegistry::contains(const QString &id) {
    for (const AgentSkill &skill : builtins()) {
        if (skill.id == id) return true;
    }
    return false;
}

AgentSkill AgentSkillRegistry::find(const QString &id) {
    for (const AgentSkill &skill : builtins()) {
        if (skill.id == id) return skill;
    }
    return {};
}

QStringList AgentSkillRegistry::orderedIds() {
    return {QStringLiteral("general"), QStringLiteral("inspection"),
            QStringLiteral("crash-rescue"), QStringLiteral("upgrade"), QStringLiteral("mods")};
}

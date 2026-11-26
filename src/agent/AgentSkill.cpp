/**
 * @file AgentSkill.cpp
 * @brief 内置技能清单（编译进程序，不从远程下载）；提示词正文在 assets/agent/prompts。
 */

#include "AgentSkill.hpp"

#include "AgentPromptLibrary.hpp"

#include <QCoreApplication>

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
            QStringLiteral("mods.analyzeCompatibility"),
            QStringLiteral("mods.verifyFiles"),
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

/// 自由对话：Stage 1B/2/2A/3 的工具都能用，Stage 5 的修复工具（事务、启动测试、二分、
/// 安装修复、核心修复）与计划工具不进来（S5.6）。MCP 工具按前缀放行。
QStringList generalTools() {
    return {QStringLiteral("fs.list"),
            QStringLiteral("fs.listNames"),
            QStringLiteral("fs.read"),
            QStringLiteral("fs.write"),
            QStringLiteral("artifact.read"),
            QStringLiteral("instance.list"),
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
            QStringLiteral("mods.analyzeCompatibility"),
            QStringLiteral("mods.verifyFiles"),
            QStringLiteral("settings.get"),
            QStringLiteral("settings.set"),
            QStringLiteral("mods.setEnabled"),
            QStringLiteral("java.list"),
            QStringLiteral("memory.suggest"),
            QStringLiteral("backup.list"),
            QStringLiteral("backup.create"),
            QStringLiteral("backup.restore"),
            QStringLiteral("upgrade.inspect"),
            QStringLiteral("upgrade.openWizard"),
            QStringLiteral("launcher.openPage"),
            QStringLiteral("web.fetch"),
            QStringLiteral("web.search"),
            QStringLiteral("command.diagnose"),
            QStringLiteral("mcp__*")};
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
            QStringLiteral("repair.finish"),
            QStringLiteral("mods.installFix"),
            QStringLiteral("instance.repairCore"),
            QStringLiteral("mods.bisect")};
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
        QStringLiteral("general"),
        QCoreApplication::translate("AgentSkill", "自由对话"),
        QCoreApplication::translate("AgentSkill", "对话 + 锁定实例内的受控文件工具与诊断工具；不含事务/启动测试/二分。"),
        QStringLiteral("general"), false, generalTools(), false, false, {}, {}, 2));

    list.append(make(
        QStringLiteral("inspection"),
        QCoreApplication::translate("AgentSkill", "实例体检"),
        QCoreApplication::translate("AgentSkill", "只读：跑启动诊断/完整性/模组统计，列出问题与下一步。"),
        QStringLiteral("inspection"), false, readOnlyTools(), true, false,
        {QStringLiteral("diagnostics.startup")},
        QStringLiteral("至少一条证据；给出问题分级与下一步选项；全程没有写操作。"), 2));

    list.append(make(
        QStringLiteral("crash-rescue"),
        QCoreApplication::translate("AgentSkill", "崩溃急救"),
        QCoreApplication::translate("AgentSkill", "诊断 + 计划 + 逐步审批修复 + 验证；每步可拒绝、可回退。"),
        QStringLiteral("crash_rescue"), true, QStringList(readOnlyTools() + repairTools()), false,
        true, {QStringLiteral("diagnostics.crash")},
        QStringLiteral("计划已批准；每步都有结果；至少一条证据；有验证步骤；"
                       "报告写明回退方式与未解决项。"),
        4));

    list.append(make(
        QStringLiteral("upgrade"),
        QCoreApplication::translate("AgentSkill", "版本升级 / 移植"),
        QCoreApplication::translate("AgentSkill", "只读检查升级目标，执行时打开现有升级向导。"),
        QStringLiteral("upgrade"), false,
        QStringList(readOnlyTools() + QStringList{QStringLiteral("backup.create")}), false, true,
        {QStringLiteral("upgrade.inspect")},
        QStringLiteral("给出兼容性结论与风险；执行交给现有向导；有证据。"), 2));

    list.append(make(
        QStringLiteral("mods"),
        QCoreApplication::translate("AgentSkill", "模组维护"),
        QCoreApplication::translate("AgentSkill", "列模组、读声明查兼容/缺前置、启停或备份、按平台记录修复文件；不猜文件名兼容性。"),
        QStringLiteral("mods"), true, QStringList(readOnlyTools() + repairTools()), false, true,
        {QStringLiteral("mods.list")},
        QStringLiteral("模组清单有证据；每个改动都有结果；不确定项写明。"), 5));

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

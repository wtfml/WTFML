/**
 * @file AgentSkill.cpp
 * @brief 内置技能清单（编译进程序，不从远程下载）。
 */

#include "AgentSkill.hpp"

namespace {

/// 只读诊断类工具：体检和崩溃急救共用的那一批。
QStringList readOnlyTools() {
    return {QStringLiteral("instance.list"),
            QStringLiteral("instance.context"),
            QStringLiteral("diagnostics.startup"),
            QStringLiteral("diagnostics.crash"),
            QStringLiteral("logs.tail"),
            QStringLiteral("crash.list"),
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
            QStringLiteral("workflow.finish")};
}

/// 写类工具：必须在已批准的计划里才放行。
QStringList repairTools() {
    return {QStringLiteral("settings.set"),
            QStringLiteral("fs.write"),
            QStringLiteral("mods.setEnabled"),
            QStringLiteral("backup.create"),
            QStringLiteral("backup.restore")};
}

AgentSkill make(const QString &id, const QString &name, const QString &description,
                const QString &systemPrompt, QStringList tools, bool readOnly,
                bool requiresPlan, const QStringList &requiredTools, const QString &completion) {
    AgentSkill skill;
    skill.id = id;
    skill.name = name;
    skill.description = description;
    skill.systemPrompt = systemPrompt;
    skill.allowedTools = tools;
    skill.requiredTools = requiredTools;
    skill.readOnly = readOnly;
    skill.requiresPlan = requiresPlan;
    skill.completionCriteria = completion;
    skill.maxSteps = 20;
    skill.version = 1;
    return skill;
}

const char kCommonRules[] =
    "没有用工具读到的文件、日志或实例信息，不要假装读过；不知道就说不确定。"
    "任何凭据、令牌、账户信息都不要索要或复述。每条结论引用证据 id（E1、E2…）；"
    "拿不到证据就写“无法确认”并说明还需要什么。";

}  // namespace

QList<AgentSkill> AgentSkillRegistry::builtins() {
    QList<AgentSkill> list;

    AgentSkill general;
    general.id = QStringLiteral("general");
    general.name = QStringLiteral("自由对话");
    general.description = QStringLiteral("对话 + 锁定实例内的受控文件工具与诊断工具。");
    general.systemPrompt = QStringLiteral(
        "你是 WTFML 启动器内置的实例维护助手。你可以用提供的工具查看本会话关联实例的元数据、"
        "诊断报告、日志与配置文件；写入只限配置白名单文件，且每次写入都要用户确认。") +
        QString::fromUtf8(kCommonRules);
    general.allowedTools = {};  // 不限制：沿用 Stage 1B 的整套工具
    general.readOnly = false;
    general.requiresPlan = false;
    general.maxSteps = 20;
    list.append(general);

    list.append(make(
        QStringLiteral("inspection"), QStringLiteral("实例体检"),
        QStringLiteral("只读：跑启动诊断/完整性/模组统计，列出问题与下一步。"),
        QStringLiteral("你在做“实例体检”，全程只读，不要尝试写入或删除任何文件。"
                       "先看实例元数据，再跑启动诊断；需要时看日志结尾、模组统计和完整性体检。"
                       "按 问题 / 提醒 / 正常 分级给结论，每条引用证据 id，最后给“下一步可做什么”选项。"),
        readOnlyTools(), true, false, {QStringLiteral("diagnostics.startup")},
        QStringLiteral("至少一条证据；给出问题分级与下一步选项；全程没有写操作。")));

    list.append(make(
        QStringLiteral("crash-rescue"), QStringLiteral("崩溃急救"),
        QStringLiteral("诊断 + 计划 + 逐步审批修复 + 验证；每步可拒绝、可回退。"),
        QStringLiteral("你在做“崩溃急救”。先收集：最近异常退出事实、日志结尾、最新崩溃报告，"
                       "再按启动器现有规则（OOM、Java 版本不匹配、缺前置、文件损坏、配置错误）分类。"
                       "只按现有规则判断，不猜具体模组。给出“最可能 + 证据 + 不确定项”之后，"
                       "用 workflow.submitPlan 提交修复计划：每步只写一个工具调用并说明理由、"
                       "可回退方式；计划通过后逐步执行，每步之后重新体检关键项，失败就停下说清证据。"
                       "改内存/Java/配置走 settings.set 或 fs.write（带期望哈希），"
                       "禁用模组走 mods.setEnabled，动手前需要备份就用 backup.create。"
                       "最后用 workflow.finish 提交报告，写清做了什么、证据、回退方式、未解决项。"),
        QStringList(readOnlyTools() + repairTools()), false, true,
        {QStringLiteral("diagnostics.crash")},
        QStringLiteral("计划已批准；每步都有结果；至少一条证据；有验证步骤；"
                       "报告写明回退方式与未解决项。")));

    list.append(make(
        QStringLiteral("upgrade"), QStringLiteral("版本升级 / 移植"),
        QStringLiteral("只读检查升级目标，执行时打开现有升级向导。"),
        QStringLiteral("你在做“版本升级 / 移植”。用 upgrade.inspect 读现有升级计划与兼容性结论，"
                       "向用户解释风险与选项；真正执行时用 upgrade.openWizard 打开现有升级页，"
                       "由用户在原界面里选择和确认。不要自己搬文件、不要重写九类选择逻辑。"),
        QStringList(readOnlyTools() + QStringList{QStringLiteral("backup.create")}), false, true,
        {QStringLiteral("upgrade.inspect")},
        QStringLiteral("给出兼容性结论与风险；执行交给现有向导；有证据。")));

    list.append(make(
        QStringLiteral("mods"), QStringLiteral("模组维护"),
        QStringLiteral("列模组、按现有规则查依赖、启停或备份；不猜文件名兼容性。"),
        QStringLiteral("你在做“模组维护”。先用 mods.list 看清单（完整文件名需要用户确认），"
                       "依赖关系只能来自现有规则与工具结果，不按文件名猜兼容性；不确定就写未知。"
                       "启用/停用只用 mods.setEnabled（只改扩展名，可原路改回），"
                       "连续改多个文件之前先用 backup.create 备份 mods。"),
        QStringList(readOnlyTools() + repairTools()), false, true,
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

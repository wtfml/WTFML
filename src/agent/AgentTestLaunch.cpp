/**
 * @file AgentTestLaunch.cpp
 * @brief 测试启动的日志里程碑规则（见 .hpp）。
 *
 * 标记在 `assets/agent/knowledge/launch_milestones.json`（S5.3 从 C++ 字面量迁过去）：
 * 换一个游戏版本就可能要加一句标记，改数据比改代码安全。读不到规则时三个判定都
 * 返回 false —— 宁可超时，也不要凭空认为“进主菜单了”。
 */

#include "AgentTestLaunch.hpp"

#include "AgentKnowledgeRules.hpp"

#include <QStringList>

bool AgentTestLaunchMilestones::isMainMenuLine(const QString &line) {
    // 1.20.1 及常见加载器进主菜单前都会打出那两句；纹理图集那一类日志更早，
    // 不能单独当主菜单信号，否则会在资源加载阶段提前判成功。
    return AgentKnowledge::milestoneMatches(QStringLiteral("main_menu"), line);
}

bool AgentTestLaunchMilestones::isWorldLoadedLine(const QString &line) {
    return AgentKnowledge::milestoneMatches(QStringLiteral("world_loaded"), line);
}

bool AgentTestLaunchMilestones::isCrashLine(const QString &line) {
    return AgentKnowledge::milestoneMatches(QStringLiteral("crash"), line);
}

QStringList AgentTestLaunchMilestones::milestoneIds() {
    return {QStringLiteral("process_started"), QStringLiteral("main_menu"),
            QStringLiteral("world_loaded")};
}

QString AgentTestLaunchMilestones::normalizeMilestone(const QString &value) {
    const QString text = value.trimmed().toLower();
    if (text == QStringLiteral("process_started") || text == QStringLiteral("main_menu") ||
        text == QStringLiteral("world_loaded")) {
        return text;
    }
    return QStringLiteral("main_menu");
}

bool AgentTestLaunchMilestones::validMilestone(const QString &value) {
    return milestoneIds().contains(value.trimmed().toLower());
}

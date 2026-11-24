/**
 * @file AgentTestLaunch.cpp
 * @brief 测试启动的日志里程碑规则（见 .hpp）。
 */

#include "AgentTestLaunch.hpp"

#include <QStringList>

namespace {

bool containsAny(const QString &line, const QStringList &needles) {
    for (const QString &needle : needles) {
        if (line.contains(needle, Qt::CaseSensitive)) return true;
    }
    return false;
}

}  // namespace

bool AgentTestLaunchMilestones::isMainMenuLine(const QString &line) {
    // 1.20.1 及常见加载器进主菜单前都会打出这两句。纹理图集那一类日志更早，
    // 不能单独当主菜单信号，否则会在资源加载阶段提前判成功。
    return containsAny(line, {QStringLiteral("Sound engine started"),
                              QStringLiteral("OpenAL initialized")});
}

bool AgentTestLaunchMilestones::isWorldLoadedLine(const QString &line) {
    return containsAny(line, {QStringLiteral("Preparing spawn area: 100%"),
                              QStringLiteral("Loaded 0 advancements"),
                              QStringLiteral("Loaded 1 advancements")});
}

bool AgentTestLaunchMilestones::isCrashLine(const QString &line) {
    return containsAny(line, {QStringLiteral("---- Minecraft Crash Report ----"),
                              QStringLiteral("A fatal error has been detected by the Java Runtime Environment"),
                              QStringLiteral("MixinApplyError"),
                              QStringLiteral("Mixin apply failed"),
                              QStringLiteral("java.lang.OutOfMemoryError"),
                              QStringLiteral("EXCEPTION_ACCESS_VIOLATION"),
                              QStringLiteral("Could not find or load main class")});
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

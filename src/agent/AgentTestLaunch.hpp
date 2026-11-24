/**
 * @file AgentTestLaunch.hpp
 * @brief 测试启动的请求/结果值，以及日志里程碑判定（纯函数，供宿主和测试共用）。
 *
 * 真正的进程、工作区、Java 选择和超时在 `AgentTestLaunchRunner`（controllers 层）；
 * 这一层只决定"一行日志算不算主菜单/世界加载/崩溃"，保证规则可单元测试。
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

struct AgentTestLaunchOptions {
    int timeoutSeconds = 120;   ///< 1–600
    int settleSeconds = 10;     ///< 到达里程碑后再观察多久
    QString milestone = QStringLiteral("main_menu");  ///< process_started / main_menu / world_loaded
    int repeat = 1;             ///< 1–3
    QString quickPlayWorld;     ///< 工作区 saves/ 里的世界名；空 = 不进世界
    bool captureLogs = true;
    bool expectCrash = false;   ///< true = 成功复现崩溃才算 ok
};

struct AgentTestLaunchRunResult {
    bool processStarted = false;
    int exitCode = 0;
    bool crashed = false;
    bool timedOut = false;
    bool milestoneReached = false;
    bool newCrashReport = false;
    bool stoppedAfterSettle = false;
    QString startupPhase;       ///< not_started/starting/main_menu/world_loaded/exited/crashed/timed_out/cancelled
    qint64 durationMs = 0;
    QString detail;
    QStringList logRefs;
};

struct AgentTestLaunchSummary {
    bool ok = false;
    QString launchId;
    QString workspaceId;
    bool workspaceKept = false;
    QList<AgentTestLaunchRunResult> runs;
    QString problem;
};

class AgentTestLaunchMilestones {
public:
    /// 主菜单标记：原版/Forge/Fabric/NeoForge 1.20.1 的共同强信号。
    static bool isMainMenuLine(const QString &line);
    /// 世界加载完成标记（quickPlay 单机）。
    static bool isWorldLoadedLine(const QString &line);
    /// 日志里已经能确认的致命错误；进程退出/崩溃报告由宿主另判。
    static bool isCrashLine(const QString &line);

    static QStringList milestoneIds();
    static QString normalizeMilestone(const QString &value);
    static bool validMilestone(const QString &value);
};

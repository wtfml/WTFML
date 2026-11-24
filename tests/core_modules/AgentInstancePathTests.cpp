/**
 * @file AgentInstancePathTests.cpp
 * @brief 崩溃报告/日志的目录查找：游戏以 `--gameDir versions/<id>` 启动，实例目录优先，
 *        数据根兜底，同名文件只取实例目录那一份。
 */

#include "Support.hpp"

#include "agent/AgentInstancePaths.hpp"
#include "util/AppPaths.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

void writeFile(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(content);
    file.close();
}

}  // namespace

void runAgentInstancePathTests() {
    QTemporaryDir fixture;
    check(fixture.isValid(), "实例路径：临时目录可用");

    // verify_agent 的 main 已把 `wtfml.testDefaultMinecraftDir` 指到临时目录；
    // 这里直接按 AppPaths 算出来的位置造文件，避免测试自己拼第二套路径。
    const QString gameRoot = AppPaths::minecraftDir();
    const QString instanceId = QStringLiteral("1.20.1-Fabric");
    const QString instanceDir = AppPaths::versionDir(instanceId);
    check(instanceDir.startsWith(gameRoot), "实例路径：实例目录在数据根下", instanceDir);

    // 一开始什么都没有：两个列表都空。
    check(AgentInstancePaths::logSources(instanceId).isEmpty(), "实例路径：没有日志时不乱指");
    check(AgentInstancePaths::crashReportDirs(instanceId).isEmpty(),
          "实例路径：没有目录时不乱指");
    check(AgentInstancePaths::collectCrashReports(instanceId).isEmpty(),
          "实例路径：没有崩溃报告时返回空表");

    // 造：实例日志、实例崩溃报告、数据根崩溃报告、数据根启动器日志。
    writeFile(instanceDir + QStringLiteral("/logs/latest.log"),
              QByteArrayLiteral("[12:00:00] [main/INFO]: instance log\n"));
    writeFile(instanceDir + QStringLiteral("/crash-reports/crash-2026-01-01_12.00.00-client.txt"),
              QByteArrayLiteral("instance crash\n"));
    writeFile(gameRoot + QStringLiteral("/crash-reports/crash-2025-12-31_23.59.59-server.txt"),
              QByteArrayLiteral("shared crash\n"));
    writeFile(gameRoot + QStringLiteral("/logs/wtfml.log"), QByteArrayLiteral("launcher log\n"));

    const QList<AgentInstancePaths::Source> logs = AgentInstancePaths::logSources(instanceId);
    check(!logs.isEmpty(), "实例路径：能列出日志候选");
    check(logs.constFirst().path.contains(instanceId) &&
              logs.constFirst().label.startsWith(QStringLiteral("versions/")),
          "实例路径：实例目录的 latest.log 排第一", logs.constFirst().label);
    check(!logs.constFirst().label.startsWith(QLatin1Char('/')) &&
              !logs.constFirst().label.contains(QStringLiteral(":")),
          "实例路径：日志标签是相对路径（不含盘符）");
    check(logs.size() == 2, "实例路径：数据根 wtfml.log 也在候选里",
          QString::number(logs.size()));

    const QList<AgentInstancePaths::Source> dirs =
        AgentInstancePaths::crashReportDirs(instanceId);
    check(dirs.size() == 2, "实例路径：实例目录与数据根两处崩溃报告目录都列出来",
          QString::number(dirs.size()));
    check(dirs.constFirst().label ==
              QStringLiteral("versions/") + instanceId + QStringLiteral("/crash-reports"),
          "实例路径：实例目录优先", dirs.constFirst().label);

    const QList<AgentInstancePaths::CrashReportFile> reports =
        AgentInstancePaths::collectCrashReports(instanceId);
    check(reports.size() == 2, "实例路径：两处崩溃报告合并后是 2 份",
          QString::number(reports.size()));
    bool sawInstance = false;
    bool sawShared = false;
    for (const AgentInstancePaths::CrashReportFile &report : reports) {
        if (report.fileName.startsWith(QStringLiteral("crash-2026-01-01"))) sawInstance = true;
        if (report.fileName.startsWith(QStringLiteral("crash-2025-12-31"))) sawShared = true;
    }
    check(sawInstance && sawShared, "实例路径：实例目录和数据根的崩溃报告都收进来了");

    // 同名文件：实例目录那一份赢。
    writeFile(gameRoot + QStringLiteral("/crash-reports/crash-2026-01-01_12.00.00-client.txt"),
              QByteArrayLiteral("shared copy\n"));
    const QList<AgentInstancePaths::CrashReportFile> deduped =
        AgentInstancePaths::collectCrashReports(instanceId);
    check(deduped.size() == 2, "实例路径：同名崩溃报告只算一份",
          QString::number(deduped.size()));
    for (const AgentInstancePaths::CrashReportFile &report : deduped) {
        if (!report.fileName.startsWith(QStringLiteral("crash-2026-01-01"))) continue;
        check(report.path.contains(instanceId), "实例路径：同名时用实例目录那一份",
              report.label);
    }

    // 上限：一次最多取 limit 条。
    for (int i = 0; i < 5; ++i) {
        writeFile(instanceDir + QStringLiteral("/crash-reports/crash-2026-02-0%1_x.txt").arg(i),
                  QByteArrayLiteral("x"));
    }
    check(AgentInstancePaths::collectCrashReports(instanceId, 3).size() == 3,
          "实例路径：按上限截断");
}

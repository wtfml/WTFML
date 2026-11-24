/**
 * @file AgentInstancePaths.cpp
 * @brief 崩溃报告/日志的目录查找（见 .hpp）。
 */

#include "AgentInstancePaths.hpp"

#include "../util/AppPaths.hpp"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace {

QString relativeLabel(const QString &absolute) {
    const QString gameRoot = QDir::cleanPath(AppPaths::minecraftDir());
    const QString path = QDir::cleanPath(absolute);
    if (path.startsWith(gameRoot + QLatin1Char('/'))) return path.mid(gameRoot.size() + 1);
    return QFileInfo(absolute).fileName();
}

QString versionPrefix(const QString &instanceId) {
    return QStringLiteral("versions/") + instanceId;
}

}  // namespace

namespace AgentInstancePaths {

QList<Source> crashReportDirs(const QString &instanceId) {
    QList<Source> sources;
    if (!instanceId.isEmpty()) {
        const QString instance = AppPaths::versionDir(instanceId) + QStringLiteral("/crash-reports");
        if (QFileInfo(instance).isDir()) {
            sources.append({instance, versionPrefix(instanceId) + QStringLiteral("/crash-reports")});
        }
    }
    const QString shared = AppPaths::crashReportsDir();
    if (QFileInfo(shared).isDir()) {
        sources.append({shared, relativeLabel(shared)});
    }
    return sources;
}

QList<Source> logSources(const QString &instanceId) {
    QList<Source> sources;
    if (!instanceId.isEmpty()) {
        const QString latest = AppPaths::versionDir(instanceId) + QStringLiteral("/logs/latest.log");
        if (QFileInfo(latest).isFile()) {
            sources.append({latest, versionPrefix(instanceId) + QStringLiteral("/logs/latest.log")});
        }
    }
    const QString sharedLatest = AppPaths::minecraftDir() + QStringLiteral("/logs/latest.log");
    if (QFileInfo(sharedLatest).isFile()) {
        sources.append({sharedLatest, relativeLabel(sharedLatest)});
    }
    // 启动器自己的日志：没有游戏日志时也值得看一眼（它是我们写的，位置固定在数据根）。
    const QString launcherLog = AppPaths::minecraftDir() + QStringLiteral("/logs/wtfml.log");
    if (QFileInfo(launcherLog).isFile()) {
        sources.append({launcherLog, relativeLabel(launcherLog)});
    }
    return sources;
}

QList<CrashReportFile> collectCrashReports(const QString &instanceId, int limit) {
    QList<CrashReportFile> files;
    QHash<QString, int> indexByName;  // 已经收过的文件名 → 下标（实例目录优先，先到先得）
    for (const Source &source : crashReportDirs(instanceId)) {
        QDir dir(source.path);
        const QFileInfoList entries =
            dir.entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time);
        for (const QFileInfo &entry : entries) {
            if (indexByName.contains(entry.fileName())) continue;  // 同名保留先到的那一份
            CrashReportFile file;
            file.path = entry.absoluteFilePath();
            file.fileName = entry.fileName();
            file.label = source.label;
            file.modifiedAt = entry.lastModified();
            file.bytes = entry.size();
            indexByName.insert(file.fileName, int(files.size()));
            files.append(file);
        }
    }
    std::sort(files.begin(), files.end(), [](const CrashReportFile &a, const CrashReportFile &b) {
        return a.modifiedAt > b.modifiedAt;
    });
    if (limit > 0 && files.size() > limit) files = files.mid(0, limit);
    return files;
}

}  // namespace AgentInstancePaths

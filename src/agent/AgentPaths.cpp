/**
 * @file AgentPaths.cpp
 * @brief 智能体路径解析（测试 override 与目录创建都只在这里）。
 */

#include "AgentPaths.hpp"

#include "../util/AppPaths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace AgentPaths {

QString dataDir() {
    const auto app = QCoreApplication::instance();
    const QString isolated =
        app != nullptr ? app->property("wtfml.testAgentDataDir").toString() : QString();
    if (!isolated.isEmpty()) {
        return QDir::cleanPath(isolated);
    }
    // 只认 `%LOCALAPPDATA%/WTFML` 这一层（不再用 AppLocalDataLocation 拼出三层 WTFML）。
    return QDir::cleanPath(AppPaths::localDataRoot() + QStringLiteral("/agent"));
}

bool migrateLegacyDataDir(QString *problem) {
    const auto app = QCoreApplication::instance();
    if (app != nullptr && !app->property("wtfml.testAgentDataDir").toString().isEmpty()) {
        return false;  // 测试隔离下不碰真实用户目录
    }
    const QString root = AppPaths::localDataRoot();
    const QString legacyAgent = AppPaths::legacyAgentDataDir();
    const QString legacyCache = AppPaths::legacyLocalCacheRoot();
    if (!QFileInfo::exists(legacyAgent) && !QFileInfo::exists(legacyCache)) return false;

    bool moved = false;
    QDir().mkpath(root);
    const QString agentTarget = root + QStringLiteral("/agent");
    if (!QFileInfo::exists(agentTarget) && QFileInfo(legacyAgent).isDir()) {
        moved = QDir().rename(legacyAgent, agentTarget) || moved;
        if (!moved && problem != nullptr) {
            *problem = QStringLiteral("旧版智能体数据搬不动：%1").arg(legacyAgent);  // i18n-ignore
        }
    }
    const QString cacheTarget = root + QStringLiteral("/cache");
    if (!QFileInfo::exists(cacheTarget) && QFileInfo(legacyCache).isDir()) {
        moved = QDir().rename(legacyCache, cacheTarget) || moved;
    }
    // 旧目录只在空的时候删（`WTFML/WTFML` → `WTFML`），任何还有东西的情况都留着。
    const QString legacyInner = AppPaths::legacyLocalDataRoot() + QStringLiteral("/WTFML");
    QDir().rmdir(legacyInner);
    QDir().rmdir(AppPaths::legacyLocalDataRoot());
    return moved;
}

QString sessionsDir() { return dataDir() + QStringLiteral("/sessions"); }
QString artifactsRootDir() { return dataDir() + QStringLiteral("/artifacts"); }
QString artifactDir(const QString &sessionId) { return artifactsRootDir() + QLatin1Char('/') + sessionId; }
QString recoveryRootDir() { return sessionsDir() + QStringLiteral("/.recovery"); }
QString recoveryDir(const QString &sessionId) { return recoveryRootDir() + QLatin1Char('/') + sessionId; }
QString maintenanceBackupDir() { return dataDir() + QStringLiteral("/backups/maintenance"); }
QString mcpConfigDir() { return dataDir() + QStringLiteral("/mcp"); }
QString logsDir() { return dataDir() + QStringLiteral("/logs"); }

QString sessionMetaPath(const QString &sessionId) {
    return sessionsDir() + QLatin1Char('/') + sessionId + QStringLiteral(".meta.json");
}
QString sessionPlainBodyPath(const QString &sessionId) {
    return sessionsDir() + QLatin1Char('/') + sessionId + QStringLiteral(".json");
}
QString sessionEncryptedBodyPath(const QString &sessionId) {
    return sessionsDir() + QLatin1Char('/') + sessionId + QStringLiteral(".enc");
}
QString sessionBodyPath(const QString &sessionId, bool encrypted) {
    return encrypted ? sessionEncryptedBodyPath(sessionId) : sessionPlainBodyPath(sessionId);
}

bool ensureDataDir(QString *problem) {
    const QStringList dirs{sessionsDir(), artifactsRootDir(), recoveryRootDir(),
                           maintenanceBackupDir(), mcpConfigDir(), logsDir()};
    for (const QString &dir : dirs) {
        if (!QDir().mkpath(dir)) {
            if (problem != nullptr) {
                *problem = QStringLiteral("无法创建智能体数据目录：%1").arg(dir);
            }
            return false;
        }
    }
    return true;
}

}  // namespace AgentPaths

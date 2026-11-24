/**
 * @file AgentPaths.cpp
 * @brief 智能体路径解析（测试 override 与目录创建都只在这里）。
 */

#include "AgentPaths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace AgentPaths {

QString dataDir() {
    const auto app = QCoreApplication::instance();
    const QString isolated =
        app != nullptr ? app->property("wtfml.testAgentDataDir").toString() : QString();
    if (!isolated.isEmpty()) {
        return QDir::cleanPath(isolated);
    }
    const QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir::cleanPath(local + QStringLiteral("/WTFML/agent"));
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

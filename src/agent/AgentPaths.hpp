/**
 * @file AgentPaths.hpp
 * @brief 智能体数据目录的**唯一**出处。
 *
 * 正式运行：`%LOCALAPPDATA%/WTFML/agent`（与游戏目录解耦，见 docs/features/AGENT.md §6.1）。
 * 测试：在创建任何控制器之前用
 * `QCoreApplication::setProperty("wtfml.testAgentDataDir", <临时目录>)` 隔离；
 * 其它层不许自己拼 `%LOCALAPPDATA%` 或 `sessions/`。
 */

#pragma once

#include <QString>

namespace AgentPaths {

QString dataDir();
QString sessionsDir();
QString artifactsRootDir();
QString artifactDir(const QString &sessionId);
QString recoveryRootDir();
QString recoveryDir(const QString &sessionId);
QString maintenanceBackupDir();
QString mcpConfigDir();
QString logsDir();

QString sessionMetaPath(const QString &sessionId);
QString sessionBodyPath(const QString &sessionId, bool encrypted);
QString sessionPlainBodyPath(const QString &sessionId);
QString sessionEncryptedBodyPath(const QString &sessionId);

/// 建齐 `dataDir()` 及其下几个根目录；返回是否可用。
bool ensureDataDir(QString *problem = nullptr);

}  // namespace AgentPaths

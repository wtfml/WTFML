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
/// 一次性把旧版三层路径（`<LOCALAPPDATA>/WTFML/WTFML/WTFML/agent` 与
/// `<LOCALAPPDATA>/WTFML/WTFML/cache`）搬到新位置（`<LOCALAPPDATA>/WTFML/agent` / `cache`）。
///
/// 只在**没有**测试隔离（`wtfml.testAgentDataDir`）时动手；新位置已经存在就什么都不做；
/// 只搬该搬的那两个目录，旧目录只在**空**的时候 rmdir，绝不递归删除。
/// @return 搬过东西（或者试图搬过）返回 true。
bool migrateLegacyDataDir(QString *problem = nullptr);
QString sessionsDir();
QString artifactsRootDir();
QString artifactDir(const QString &sessionId);
QString recoveryRootDir();
QString recoveryDir(const QString &sessionId);
QString maintenanceBackupDir();
QString workspaceRecordsDir();
QString transactionsDir();
QString mcpConfigDir();
QString logsDir();

QString sessionMetaPath(const QString &sessionId);
QString sessionBodyPath(const QString &sessionId, bool encrypted);
QString sessionPlainBodyPath(const QString &sessionId);
QString sessionEncryptedBodyPath(const QString &sessionId);

/// 建齐 `dataDir()` 及其下几个根目录；返回是否可用。
bool ensureDataDir(QString *problem = nullptr);

}  // namespace AgentPaths

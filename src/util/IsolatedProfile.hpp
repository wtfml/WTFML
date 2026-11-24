/**
 * @file IsolatedProfile.hpp
 * @brief 为首次使用测试与启动验收隔离设置、游戏和智能体数据。
 */
#pragma once

#include "AppPaths.hpp"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

namespace IsolatedProfile {

inline bool safeDirectoryPath(const QString &path) {
    for (QString current = path;;) {
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction() || (info.exists() && !info.isDir())) return false;
        const QString parent = info.absolutePath();
        if (parent == current) return true;
        current = parent;
    }
}

inline bool configure(const QString &root) {
    const QString settings = root + QStringLiteral("/settings");
    const QString systemSettings = root + QStringLiteral("/settings-system");
    const QString game = root + QStringLiteral("/game");
    const QString agent = root + QStringLiteral("/agent");
    const QStringList paths{settings, systemSettings, game, agent};
    for (const QString &path : paths) {
        if (!safeDirectoryPath(path)) return false;
    }
    for (const QString &path : paths) {
        if (!QDir().mkpath(path)) return false;
    }

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, systemSettings);
    QCoreApplication::instance()->setProperty("wtfml.testDefaultMinecraftDir", game);
    QCoreApplication::instance()->setProperty("wtfml.testAgentDataDir", agent);
    AppPaths::setMinecraftDir(game);
    return true;
}

inline constexpr char firstRunOption[] = "--first-run-test=";
inline constexpr char firstRunDirectoryPrefix[] = "WTFML-first-run-";
inline constexpr char firstRunMarker[] = ".wtfml-first-run-test";

inline bool configureFirstRun(const QString &root) {
    const QFileInfo info(root);
    if (!QDir::isAbsolutePath(root) || root != QDir::cleanPath(root) ||
        info.absolutePath() != QDir::temp().canonicalPath() ||
        !info.fileName().startsWith(QLatin1String(firstRunDirectoryPrefix)) || !safeDirectoryPath(root)) {
        return false;
    }
    const QString marker = QDir(root).filePath(QLatin1String(firstRunMarker));
    const QFileInfo markerInfo(marker);
    if (!markerInfo.isFile() || markerInfo.isSymLink() || markerInfo.size() != 0 || !configure(root)) {
        return false;
    }
    // 测试档案的 DPAPI 凭据随独立设置保存。
    QCoreApplication::instance()->setProperty("wtfml.testAgentDisableCredentialManager", true);
    return true;
}

/// 成功时返回测试目录，关窗后也能查看日志。
inline QString launchFirstRun() {
    const QString tempRoot = QDir::temp().canonicalPath();
    if (tempRoot.isEmpty() || !safeDirectoryPath(tempRoot)) return {};
    QTemporaryDir profile(QDir(tempRoot).filePath(QLatin1String(firstRunDirectoryPrefix) + QStringLiteral("XXXXXX")));
    if (!profile.isValid()) return {};
    QFile marker(QDir(profile.path()).filePath(QLatin1String(firstRunMarker)));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return {};
    marker.close();
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {QLatin1String(firstRunOption) + profile.path()}, QDir::currentPath())) {
        return {};
    }
    profile.setAutoRemove(false);
    return profile.path();
}

}  // namespace IsolatedProfile

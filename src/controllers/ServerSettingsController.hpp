#pragma once

#include "server/ServerCheck.hpp"
#include "server/ServerModel.hpp"
#include "server/ServerPlayers.hpp"
#include "server/ServerPortCheck.hpp"

#include <QString>
#include <QHash>

class ServerWorkspaceController;

class ServerSettingsController final {
public:
    using Values = QList<QPair<QString, QString>>;

    struct SaveResult {
        bool ok = false;
        int changed = 0;
        QStringList rejected;
        QString problem;
        QString serverName;
    };

    explicit ServerSettingsController(const ServerWorkspaceController *workspace = nullptr)
        : m_workspace(workspace) {}

    QString readRaw(const QString &serverId) const;
    Values valuesFromRaw(const QString &raw) const;
    QString mergeIntoRaw(const QString &raw, const Values &edits) const;
    SaveResult save(const QString &serverId, const QString &raw, const Values &edits) const;
    Values fallbackEdits(const ServerModel::Properties &properties) const;

    QString worldName(const QString &serverId) const;
    QString worldDir(const QString &serverId) const;
    QString backupLabel(const QString &serverId) const;
    int worldBackupCount(const QString &serverId) const;
    ServerCheck::Facts checkFacts(const QString &serverId,
                                  ServerPortCheck::State portState) const;

    // ---------- 玩家名单（「玩家管理」） ----------
    //
    // 判据全在 `ServerPlayers`（纯函数）；这一层只负责"哪个文件、读写那几个字节"。
    // 服务端**正在跑**的时候不要调 `writePlayers()`：它退出时会把自己内存里那份名单
    // 写回盘上，刚写进去的那几条会被它盖掉（界面那边用 `setEditable()` 一起挡）。
    QString playersPath(const QString &serverId, ServerPlayers::List list) const;
    ServerPlayers::Content readPlayers(const QString &serverId, ServerPlayers::List list) const;
    /// 原子写（`QSaveFile`）。失败时 @p problemOut 里有话说。
    bool writePlayers(const QString &serverId, ServerPlayers::List list,
                      const QList<ServerPlayers::Entry> &entries,
                      QString *problemOut = nullptr) const;

private:
    mutable QHash<QString, QByteArray> m_propertySnapshots;
    static qint64 directoryBytes(const QString &dirPath);

    const ServerWorkspaceController *m_workspace = nullptr;
};

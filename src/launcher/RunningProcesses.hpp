/**
 * @file RunningProcesses.hpp
 * @brief 运行中进程列表（客户端会话与服务端进程）的纯数据模型、键编解码、排序与计数规则。
 */
#pragma once

#include <QList>
#include <QString>

#include <algorithm>

#include "download/ServerInstall.hpp"

namespace RunningProcesses {

/// 服务端面板行 key 的固定前缀。
/// 面板里客户端行用十进制会话号当 key，服务端目录名也可能是纯数字（比如 "1"），
/// 因此服务端行统一带此前缀，确保动作分派时绝不和客户端会话号相撞。
inline const QString &serverKeyPrefix() {
    static const QString prefix = QStringLiteral("server:");
    return prefix;
}

inline QString makeServerKey(const QString &serverId) {
    return serverKeyPrefix() + serverId;
}

inline bool parseServerKey(const QString &key, QString *serverIdOut = nullptr) {
    if (!key.startsWith(serverKeyPrefix())) {
        return false;
    }
    const QString serverId = key.mid(serverKeyPrefix().size());
    if (serverId.isEmpty()) {
        return false;
    }
    if (serverIdOut != nullptr) {
        *serverIdOut = serverId;
    }
    return true;
}

struct Entry {
    enum class Kind { Client, Server };

    Kind kind = Kind::Client;
    QString key;
    qint64 startedAtMs = 0;

    // 客户端会话字段
    int sessionId = 0;
    QString versionId;
    QString displayName;
    QString accountName;
    /// 同实例并发序号（1 起：#1, #2…；仅单份运行且未多开时为 0，不显示序号）。
    int instanceNumber = 0;

    // 服务端进程字段
    QString serverId;
    ServerInstall::Flavor serverFlavor = ServerInstall::Flavor::Vanilla;
    bool serverStopping = false;
    bool serverForceKillAllowed = false;
};

struct InstanceSlot {
    QString versionId;
    int instanceNumber = 0;
    bool instanceNumbered = false;
};

struct InstanceNumberAssignment {
    int instanceNumber = 1;
    bool instanceNumbered = false;
};

/// 仅当处于“需要展示序号”状态时返回正序号，否则返回 0（单开不加序号）。
inline int displayedInstanceNumber(int instanceNumber, bool instanceNumbered) {
    return instanceNumbered && instanceNumber > 0 ? instanceNumber : 0;
}

/// 多开时在实例名后补 `#N`（`instanceNumber <= 0` 或名字为空时原样返回）。
inline QString numberedName(const QString &baseName, int instanceNumber) {
    if (baseName.isEmpty() || instanceNumber <= 0) {
        return baseName;
    }
    return QStringLiteral("%1 #%2").arg(baseName).arg(instanceNumber);
}

/// 为同一实例新启动的会话分配序号，并在出现多份同时运行时同步标亮现有同实例会话。
inline InstanceNumberAssignment assignNextInstanceNumber(QList<InstanceSlot> &activeSessions,
                                                         const QString &versionId) {
    int maxExisting = 0;
    bool hasOtherSameInstance = false;
    for (const InstanceSlot &slot : activeSessions) {
        if (slot.versionId == versionId) {
            hasOtherSameInstance = true;
            maxExisting = qMax(maxExisting, slot.instanceNumber);
        }
    }
    if (!hasOtherSameInstance) {
        return {1, false};
    }
    for (InstanceSlot &slot : activeSessions) {
        if (slot.versionId == versionId) {
            if (slot.instanceNumber <= 0) {
                slot.instanceNumber = 1;
            }
            slot.instanceNumbered = true;
        }
    }
    return {maxExisting + 1, true};
}

/// 进程总计数（客户端与服务端合计）。
inline int totalCount(const QList<Entry> &entries) {
    return int(entries.size());
}

inline int totalCount(int clientCount, int serverCount) {
    return qMax(0, clientCount) + qMax(0, serverCount);
}

/// 按启动时间倒序排序：最新启动的排在最上面；客户端与服务端统一混排。
/// 同一毫秒内先按客户端更大的 sessionId 排前，再按输入相对顺序保持稳定。
inline void sortNewestFirst(QList<Entry> &entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.startedAtMs != b.startedAtMs) {
            return a.startedAtMs > b.startedAtMs;
        }
        if (a.kind == Entry::Kind::Client && b.kind == Entry::Kind::Client) {
            return a.sessionId > b.sessionId;
        }
        return false;
    });
}

}  // namespace RunningProcesses

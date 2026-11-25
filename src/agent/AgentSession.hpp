/**
 * @file AgentSession.hpp
 * @brief 一个会话的内存形状：元数据 + 消息正文 + 摘要/用量。
 *
 * 元数据与正文**分开落盘**（列表只读元数据；整会话加密时元数据仍可列）。字段名与
 * docs/features/AGENT.md §6.2 的 schema v1 对齐。
 */

#pragma once

#include "AgentTypes.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

struct AgentInstanceLock {
    QString id;
    QString name;
    QString gameRootId;
    QDateTime lockedAt;

    bool isValid() const { return !id.isEmpty(); }

    QJsonObject toJson() const {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("name"), name},
                           {QStringLiteral("gameRootId"), gameRootId},
                           {QStringLiteral("lockedAt"),
                            lockedAt.isValid() ? lockedAt.toUTC().toString(Qt::ISODateWithMs)
                                               : QString()}};
    }
    static AgentInstanceLock fromJson(const QJsonObject &object) {
        AgentInstanceLock lock;
        lock.id = object.value(QStringLiteral("id")).toString();
        lock.name = object.value(QStringLiteral("name")).toString();
        lock.gameRootId = object.value(QStringLiteral("gameRootId")).toString();
        lock.lockedAt = QDateTime::fromString(
            object.value(QStringLiteral("lockedAt")).toString(), Qt::ISODateWithMs);
        return lock;
    }
};

struct AgentSessionMeta {
    int schema = 1;
    QString id;
    QString title;
    QDateTime createdAt;
    QDateTime updatedAt;
    bool pinned = false;
    bool archived = false;
    bool encrypted = false;
    QString skill = QStringLiteral("general");
    QJsonObject provider;
    AgentInstanceLock instanceLock;
    int messageCount = 0;
    /// 这个会话已经用掉的工具调用次数（单会话上限按它累计，跨重启保留）。
    int toolCalls = 0;
    QString bodySha256;
    int policyVersion = 1;
    QString lastTurnState = QStringLiteral("complete");
    /// none / pending / deleting
    QString deletionState = QStringLiteral("none");
    /// 载入时发现元数据损坏（列表里显示“损坏，已隔离”，不当作空会话）。
    bool corrupt = false;

    bool isValid() const { return !id.isEmpty(); }
    QJsonObject toJson() const;
    static AgentSessionMeta fromJson(const QJsonObject &object);
};

struct AgentSession {
    AgentSessionMeta meta;
    QList<Agent::Message> messages;
    QJsonObject summary;
    Agent::Usage usage;
    QJsonObject pendingApproval;

    QString title() const { return meta.title; }
};

namespace AgentSessionTitle {
/// 会话还没起名时的标题。认这条标题的地方一律走这里：写成 `QLatin1String("新对话")` 会把
/// UTF-8 字节按 Latin-1 解，跟盘上读回的 `QString` 永远不相等（r109「空会话切走没丢」的根因）。
QString defaultTitle();
/// @p title 是不是「还没动过」的默认标题（空串也算：列表本来就把空标题显示成「新对话」）。
bool isDefault(const QString &title);
/// 第一条用户消息前 24 个字符；去掉换行/多余空白；空输入回退“新对话”。
QString fromMessage(const QString &message);
}  // namespace AgentSessionTitle

namespace AgentSessionExport {
/// 「导出…」写出的 Markdown：一级标题是会话标题，用户与助手的话按先后各一节；
/// 系统提示与工具结果不导出（工具结果可能含实例文件原文，导出只留对话本身）。
QString toMarkdown(const AgentSession &session);
}  // namespace AgentSessionExport

/**
 * @file AgentSession.cpp
 * @brief 会话元数据/标题的纯函数部分。
 */

#include "AgentSession.hpp"

#include <QCoreApplication>
#include <QStringList>

QJsonObject AgentSessionMeta::toJson() const {
    QJsonObject object{
        {QStringLiteral("schema"), schema},
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("createdAt"),
         createdAt.isValid() ? createdAt.toUTC().toString(Qt::ISODateWithMs) : QString()},
        {QStringLiteral("updatedAt"),
         updatedAt.isValid() ? updatedAt.toUTC().toString(Qt::ISODateWithMs) : QString()},
        {QStringLiteral("pinned"), pinned},
        {QStringLiteral("archived"), archived},
        {QStringLiteral("encrypted"), encrypted},
        {QStringLiteral("skill"), skill},
        {QStringLiteral("provider"), provider},
        {QStringLiteral("messageCount"), messageCount},
        {QStringLiteral("bodySha256"), bodySha256},
        {QStringLiteral("policyVersion"), policyVersion},
        {QStringLiteral("toolCalls"), toolCalls},
        {QStringLiteral("lastTurnState"), lastTurnState},
        {QStringLiteral("deletionState"), deletionState},
    };
    if (instanceLock.isValid()) {
        object.insert(QStringLiteral("instanceLock"), instanceLock.toJson());
    } else {
        object.insert(QStringLiteral("instanceLock"), QJsonValue::Null);
    }
    return object;
}

AgentSessionMeta AgentSessionMeta::fromJson(const QJsonObject &object) {
    AgentSessionMeta meta;
    meta.schema = object.value(QStringLiteral("schema")).toInt(1);
    meta.id = object.value(QStringLiteral("id")).toString();
    meta.title = object.value(QStringLiteral("title")).toString();
    meta.createdAt =
        QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(),
                              Qt::ISODateWithMs);
    meta.updatedAt =
        QDateTime::fromString(object.value(QStringLiteral("updatedAt")).toString(),
                              Qt::ISODateWithMs);
    meta.pinned = object.value(QStringLiteral("pinned")).toBool(false);
    meta.archived = object.value(QStringLiteral("archived")).toBool(false);
    meta.encrypted = object.value(QStringLiteral("encrypted")).toBool(false);
    meta.skill = object.value(QStringLiteral("skill")).toString(QStringLiteral("general"));
    meta.provider = object.value(QStringLiteral("provider")).toObject();
    meta.instanceLock =
        AgentInstanceLock::fromJson(object.value(QStringLiteral("instanceLock")).toObject());
    meta.messageCount = object.value(QStringLiteral("messageCount")).toInt(0);
    meta.bodySha256 = object.value(QStringLiteral("bodySha256")).toString();
    meta.policyVersion = object.value(QStringLiteral("policyVersion")).toInt(1);
    meta.toolCalls = object.value(QStringLiteral("toolCalls")).toInt(0);
    meta.lastTurnState =
        object.value(QStringLiteral("lastTurnState")).toString(QStringLiteral("complete"));
    meta.deletionState =
        object.value(QStringLiteral("deletionState")).toString(QStringLiteral("none"));
    return meta;
}

namespace AgentSessionTitle {
QString defaultTitle() { return QCoreApplication::translate("AgentSession", "新对话"); }

bool isDefault(const QString &title) { return title.isEmpty() || title == defaultTitle(); }

QString fromMessage(const QString &message) {
    const QString simplified = message.simplified();
    if (simplified.isEmpty()) {
        return defaultTitle();
    }
    if (simplified.size() <= 24) {
        return simplified;
    }
    // 超长：留 23 字 + 「…」（总长仍 ≤ 24），不劈开代理对；切点落在英文单词中间时退到前一个空格
    // （不远于 8 字），免得出现「日志里说 m…」。
    int cut = 23;
    if (simplified.at(cut - 1).isHighSurrogate()) --cut;
    const auto latin = [](QChar c) { return c.unicode() < 0x80 && c.isLetterOrNumber(); };
    if (latin(simplified.at(cut - 1)) && latin(simplified.at(cut))) {
        const int space = simplified.lastIndexOf(QLatin1Char(' '), cut - 1);
        if (space > 0 && cut - space <= 8) cut = space;
    }
    return simplified.left(cut).trimmed() + QStringLiteral("…");
}
}  // namespace AgentSessionTitle

namespace AgentSessionExport {
QString toMarkdown(const AgentSession &session) {
    QString out = QStringLiteral("# %1\n").arg(
        session.meta.title.isEmpty() ? AgentSessionTitle::defaultTitle() : session.meta.title);
    for (const Agent::Message &message : session.messages) {
        if (message.role != Agent::Role::User && message.role != Agent::Role::Assistant) continue;
        if (message.content.trimmed().isEmpty()) continue;
        const QString heading = message.role == Agent::Role::User
                                    ? QCoreApplication::translate("AgentSession", "我")
                                    : QCoreApplication::translate("AgentSession", "助手");
        out += QStringLiteral("\n## %1\n\n").arg(heading);
        out += message.content.trimmed() + QLatin1Char('\n');
    }
    return out;
}
}  // namespace AgentSessionExport

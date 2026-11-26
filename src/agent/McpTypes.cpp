/**
 * @file McpTypes.cpp
 * @brief MCP 值类型的序列化与配置哈希（见 .hpp）。
 */

#include "McpTypes.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace {

QString sha256Hex(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QStringList sortedUnique(const QStringList &values) {
    QStringList list = values;
    list.removeDuplicates();
    list.sort(Qt::CaseInsensitive);
    return list;
}

QJsonArray toArray(const QStringList &values) {
    QJsonArray array;
    for (const QString &value : values) array.append(value);
    return array;
}

QStringList fromArray(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray()) {
        const QString text = item.toString();
        if (!text.isEmpty()) list.append(text);
    }
    return list;
}

int boundedTimeout(int value, int fallback) {
    if (value < McpLimits::kMinTimeoutMs || value > McpLimits::kMaxTimeoutMs) return fallback;
    return value;
}

}  // namespace

QJsonObject McpExecutableIdentity::toJson() const {
    return QJsonObject{{QStringLiteral("absolutePath"), absolutePath},
                       {QStringLiteral("sha256"), sha256},
                       {QStringLiteral("size"), size},
                       {QStringLiteral("modifiedAt"), modifiedAt},
                       {QStringLiteral("volumeSerial"), volumeSerial},
                       {QStringLiteral("fileId"), fileId}};
}

McpExecutableIdentity McpExecutableIdentity::fromJson(const QJsonObject &object) {
    McpExecutableIdentity identity;
    identity.absolutePath = object.value(QStringLiteral("absolutePath")).toString();
    identity.sha256 = object.value(QStringLiteral("sha256")).toString();
    identity.size = object.value(QStringLiteral("size")).toInteger(-1);
    identity.modifiedAt = object.value(QStringLiteral("modifiedAt")).toString();
    identity.volumeSerial = object.value(QStringLiteral("volumeSerial")).toString();
    identity.fileId = object.value(QStringLiteral("fileId")).toString();
    return identity;
}

bool McpExecutableIdentity::sameAs(const McpExecutableIdentity &other) const {
    return absolutePath.compare(other.absolutePath, Qt::CaseInsensitive) == 0 &&
           sha256 == other.sha256 && size == other.size && fileId == other.fileId &&
           modifiedAt == other.modifiedAt;
}

QString McpExecutableIdentity::summary() const {
    if (!isValid()) return QCoreApplication::translate("McpTypes", "身份不可用");
    QString text = QCoreApplication::translate("McpTypes", "%1\nSHA-256：%2\n大小：%3 字节")
                       .arg(absolutePath, sha256)
                       .arg(size);
    if (!modifiedAt.isEmpty()) text += QCoreApplication::translate("McpTypes", "\n修改时间：%1").arg(modifiedAt);
    if (!fileId.isEmpty()) text += QCoreApplication::translate("McpTypes", "\n文件 ID：%1").arg(fileId);
    return text;
}

QString McpServerConfig::configHash() const {
    const QJsonObject canonical{
        {QStringLiteral("command"), command},
        {QStringLiteral("arguments"), toArray(arguments)},
        {QStringLiteral("workingDirectory"), workingDirectory},
        {QStringLiteral("environmentNames"), toArray(sortedUnique(environmentNames))},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("handshakeTimeoutMs"), handshakeTimeoutMs},
        {QStringLiteral("listTimeoutMs"), listTimeoutMs},
        {QStringLiteral("callTimeoutMs"), callTimeoutMs}};
    return sha256Hex(QJsonDocument(canonical).toJson(QJsonDocument::Compact));
}

QJsonObject McpServerConfig::toJson() const {
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("command"), command},
                       {QStringLiteral("arguments"), toArray(arguments)},
                       {QStringLiteral("workingDirectory"), workingDirectory},
                       {QStringLiteral("environmentNames"), toArray(sortedUnique(environmentNames))},
                       {QStringLiteral("enabled"), enabled},
                       {QStringLiteral("handshakeTimeoutMs"), handshakeTimeoutMs},
                       {QStringLiteral("listTimeoutMs"), listTimeoutMs},
                       {QStringLiteral("callTimeoutMs"), callTimeoutMs}};
}

McpServerConfig McpServerConfig::fromJson(const QJsonObject &object) {
    McpServerConfig server;
    server.id = object.value(QStringLiteral("id")).toString();
    server.name = object.value(QStringLiteral("name")).toString();
    server.command = object.value(QStringLiteral("command")).toString();
    server.arguments = fromArray(object.value(QStringLiteral("arguments")));
    server.workingDirectory = object.value(QStringLiteral("workingDirectory")).toString();
    server.environmentNames = fromArray(object.value(QStringLiteral("environmentNames")));
    server.enabled = object.value(QStringLiteral("enabled")).toBool(false);
    server.handshakeTimeoutMs = boundedTimeout(
        object.value(QStringLiteral("handshakeTimeoutMs")).toInt(), 15000);
    server.listTimeoutMs =
        boundedTimeout(object.value(QStringLiteral("listTimeoutMs")).toInt(), 15000);
    server.callTimeoutMs =
        boundedTimeout(object.value(QStringLiteral("callTimeoutMs")).toInt(), 120000);
    return server;
}

QJsonObject McpConfirmation::toJson() const {
    return QJsonObject{{QStringLiteral("serverId"), serverId},
                       {QStringLiteral("configHash"), configHash},
                       {QStringLiteral("identity"), identity.toJson()},
                       {QStringLiteral("confirmedAt"), confirmedAt},
                       {QStringLiteral("purpose"), purpose}};
}

McpConfirmation McpConfirmation::fromJson(const QJsonObject &object) {
    McpConfirmation confirmation;
    confirmation.serverId = object.value(QStringLiteral("serverId")).toString();
    confirmation.configHash = object.value(QStringLiteral("configHash")).toString();
    confirmation.identity = McpExecutableIdentity::fromJson(
        object.value(QStringLiteral("identity")).toObject());
    confirmation.confirmedAt = object.value(QStringLiteral("confirmedAt")).toString();
    confirmation.purpose = object.value(QStringLiteral("purpose")).toString();
    return confirmation;
}

QString McpToolDescriptor::wireName() const {
    // 线上名只允许 [A-Za-z0-9_-]（AGENT.md §5.1）；服务器 id 是 uuid，工具名可能带点或空格。
    static const QRegularExpression invalid(QStringLiteral("[^A-Za-z0-9_-]"));
    QString server = serverId;
    server.replace(invalid, QStringLiteral("_"));
    QString tool = name;
    tool.replace(invalid, QStringLiteral("_"));
    QString wire = QStringLiteral("mcp__%1__%2").arg(server, tool);
    if (wire.size() > McpLimits::kToolNameMaxLength) {
        wire = wire.left(McpLimits::kToolNameMaxLength);
    }
    return wire;
}

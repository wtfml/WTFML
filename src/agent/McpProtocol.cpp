/**
 * @file McpProtocol.cpp
 * @brief JSON-RPC 帧编解码（见 .hpp）。
 */

#include "McpProtocol.hpp"

#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

QJsonObject jsonRpc(const QString &key, const QJsonValue &value) {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {key, value}};
}

QByteArray oneLine(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + QByteArray("\n");
}

}  // namespace

namespace McpProtocol {

QString supportedProtocolVersion() { return QStringLiteral("2024-11-05"); }

Frame parseLine(const QByteArray &line, int maxBytes) {
    Frame frame;
    if (line.size() > maxBytes) {
        frame.problem = QStringLiteral("单行超过 %1 字节上限。").arg(maxBytes);  // i18n-ignore
        return frame;
    }
    QByteArray trimmed = line;
    while (trimmed.endsWith('\r') || trimmed.endsWith('\n')) trimmed.chop(1);
    if (trimmed.trimmed().isEmpty()) {
        frame.problem = QStringLiteral("空行。");  // i18n-ignore
        return frame;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        frame.problem = QStringLiteral("不是 JSON 对象：%1").arg(error.errorString());  // i18n-ignore
        return frame;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")) {
        frame.problem = QStringLiteral("缺少 jsonrpc=2.0。");  // i18n-ignore
        return frame;
    }
    frame.method = object.value(QStringLiteral("method")).toString();
    frame.params = object.value(QStringLiteral("params")).toObject();
    frame.result = object.value(QStringLiteral("result")).toObject();
    if (object.contains(QStringLiteral("error"))) {
        frame.hasError = true;
        frame.error = object.value(QStringLiteral("error")).toObject();
    }
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (id.isString()) {
        frame.hasId = true;
        frame.id = id.toString();
    } else if (id.isDouble()) {
        frame.hasId = true;
        frame.idWasNumber = true;
        frame.id = QString::number(qint64(id.toDouble()));
    }
    if (frame.method.isEmpty() && !frame.hasId) {
        frame.problem = QStringLiteral("既不是请求也不是响应。");  // i18n-ignore
        return frame;
    }
    frame.valid = true;
    return frame;
}

QByteArray encodeRequest(const QString &id, const QString &method, const QJsonObject &params) {
    QJsonObject object = jsonRpc(QStringLiteral("id"), id);
    object.insert(QStringLiteral("method"), method);
    object.insert(QStringLiteral("params"), params);
    return oneLine(object);
}

QByteArray encodeNotification(const QString &method, const QJsonObject &params) {
    QJsonObject object = jsonRpc(QStringLiteral("method"), method);
    object.insert(QStringLiteral("params"), params);
    return oneLine(object);
}

QByteArray encodeResult(const QString &id, const QJsonObject &result, bool numericId) {
    QJsonObject object = jsonRpc(QStringLiteral("result"), result);
    object.insert(QStringLiteral("id"),
                  numericId ? QJsonValue(qint64(id.toLongLong())) : QJsonValue(id));
    return oneLine(object);
}

QByteArray encodeError(const QString &id, int code, const QString &message, bool numericId) {
    QJsonObject object = jsonRpc(
        QStringLiteral("error"),
        QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}});
    object.insert(QStringLiteral("id"),
                  numericId ? QJsonValue(qint64(id.toLongLong())) : QJsonValue(id));
    return oneLine(object);
}

QByteArray replyToServerRequest(const Frame &frame) {
    if (!frame.valid || !frame.hasId || frame.method.isEmpty()) return QByteArray();
    if (frame.method == QStringLiteral("ping")) {
        return encodeResult(frame.id, QJsonObject{}, frame.idWasNumber);
    }
    return encodeError(frame.id, ErrorCode::kMethodNotFound,
                       QStringLiteral("不支持的方法：%1").arg(frame.method), frame.idWasNumber);  // i18n-ignore
}

}  // namespace McpProtocol

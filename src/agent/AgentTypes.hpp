/**
 * @file AgentTypes.hpp
 * @brief 智能体核心层共用的**值类型**：消息、用量、错误、工具调用。
 *
 * 只放可以在 `verify_agent` 里单独构造、序列化、比较的结构，不碰 QWidget、
 * 不碰网络、不碰业务管理器。UI 与 `AgentController` 只读这些字段。
 */

#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>

namespace Agent {

enum class Role { System, User, Assistant, Tool };
enum class MessageStatus { Complete, Streaming, Interrupted, Error, ToolPending, AwaitingApproval };

enum class ErrorKind {
    None,
    NoConfiguration,
    Unauthorized,
    RateLimited,
    Quota,
    Network,
    Protocol,
    ContextOverflow,
    ToolError,
    EgressApprovalRequired,
    EgressBlocked,
    ApprovalDenied,
    Cancelled,
    Internal,
};

QString roleKey(Role role);
Role roleFromKey(const QString &key);
QString statusKey(MessageStatus status);
MessageStatus statusFromKey(const QString &key);
QString errorKey(ErrorKind kind);
ErrorKind errorFromKey(const QString &key);

struct ToolCall {
    QString id;
    QString name;
    QJsonObject arguments;

    QJsonObject toJson() const {
        return QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("name"), name},
                           {QStringLiteral("arguments"), arguments}};
    }
    static ToolCall fromJson(const QJsonObject &object) {
        ToolCall call;
        call.id = object.value(QStringLiteral("id")).toString();
        call.name = object.value(QStringLiteral("name")).toString();
        const QJsonValue args = object.value(QStringLiteral("arguments"));
        if (args.isObject()) {
            call.arguments = args.toObject();
        } else if (args.isString()) {
            // 兼容 OpenAI 的“arguments 是一段 JSON 字符串”。
            const QJsonDocument document = QJsonDocument::fromJson(args.toString().toUtf8());
            if (document.isObject()) {
                call.arguments = document.object();
            }
        }
        return call;
    }
};

struct Message {
    QString id;
    Role role = Role::User;
    QString content;
    QString reasoning;
    QList<ToolCall> toolCalls;
    QString toolCallId;
    QString toolName;
    MessageStatus status = MessageStatus::Complete;
    QDateTime createdAt;
    /// 错误卡片上的用户可读说明；成功消息为空。
    QString errorText;
    ErrorKind errorKind = ErrorKind::None;
    /// 发给模型的本轮快照。随用户消息保存，后续发送沿用同一份前缀。
    QString modelContext;

    bool empty() const { return id.isEmpty(); }

    QJsonObject toJson() const {
        QJsonObject object{{QStringLiteral("id"), id},
                           {QStringLiteral("role"), roleKey(role)},
                           {QStringLiteral("content"), content},
                           {QStringLiteral("status"), statusKey(status)},
                           {QStringLiteral("createdAt"),
                            createdAt.isValid() ? createdAt.toUTC().toString(Qt::ISODateWithMs)
                                                : QString()}};
        if (!reasoning.isEmpty()) {
            object.insert(QStringLiteral("reasoning"), reasoning);
        }
        if (!modelContext.isEmpty()) {
            object.insert(QStringLiteral("modelContext"), modelContext);
        }
        if (!toolCalls.isEmpty()) {
            QJsonArray calls;
            for (const ToolCall &call : toolCalls) {
                calls.append(call.toJson());
            }
            object.insert(QStringLiteral("toolCalls"), calls);
        }
        if (!toolCallId.isEmpty()) {
            object.insert(QStringLiteral("toolCallId"), toolCallId);
        }
        if (!toolName.isEmpty()) {
            object.insert(QStringLiteral("name"), toolName);
        }
        if (!errorText.isEmpty()) {
            object.insert(QStringLiteral("errorText"), errorText);
            object.insert(QStringLiteral("errorKind"), errorKey(errorKind));
        }
        return object;
    }

    static Message fromJson(const QJsonObject &object) {
        Message message;
        message.id = object.value(QStringLiteral("id")).toString();
        message.role = roleFromKey(object.value(QStringLiteral("role")).toString());
        message.content = object.value(QStringLiteral("content")).toString();
        message.reasoning = object.value(QStringLiteral("reasoning")).toString();
        message.modelContext = object.value(QStringLiteral("modelContext")).toString();
        message.status = statusFromKey(object.value(QStringLiteral("status")).toString());
        message.createdAt =
            QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(),
                                  Qt::ISODateWithMs);
        for (const QJsonValue &value : object.value(QStringLiteral("toolCalls")).toArray()) {
            message.toolCalls.append(ToolCall::fromJson(value.toObject()));
        }
        message.toolCallId = object.value(QStringLiteral("toolCallId")).toString();
        message.toolName = object.value(QStringLiteral("name")).toString();
        message.errorText = object.value(QStringLiteral("errorText")).toString();
        message.errorKind = errorFromKey(object.value(QStringLiteral("errorKind")).toString());
        return message;
    }
};

struct Usage {
    int promptTokens = 0;
    int completionTokens = 0;
    bool estimated = false;
    int cachedPromptTokens = -1;  ///< -1 表示服务商未返回缓存统计。

    bool isValid() const { return promptTokens > 0 || completionTokens > 0; }

    QJsonObject toJson() const {
        QJsonObject object{{QStringLiteral("promptTokens"), promptTokens},
                           {QStringLiteral("completionTokens"), completionTokens},
                           {QStringLiteral("estimated"), estimated}};
        if (cachedPromptTokens >= 0) {
            object.insert(QStringLiteral("cachedPromptTokens"), cachedPromptTokens);
        }
        return object;
    }
    static Usage fromJson(const QJsonObject &object) {
        Usage usage;
        usage.promptTokens = object.value(QStringLiteral("promptTokens")).toInt();
        usage.completionTokens = object.value(QStringLiteral("completionTokens")).toInt();
        usage.estimated = object.value(QStringLiteral("estimated")).toBool();
        usage.cachedPromptTokens = object.value(QStringLiteral("cachedPromptTokens")).toInt(-1);
        return usage;
    }
};

struct Error {
    ErrorKind kind = ErrorKind::None;
    QString message;
    int httpStatus = 0;
    int retryAfterSec = -1;
    bool retryable = false;

    bool isEmpty() const { return kind == ErrorKind::None; }

    QJsonObject toJson() const {
        return QJsonObject{{QStringLiteral("kind"), errorKey(kind)},
                           {QStringLiteral("message"), message},
                           {QStringLiteral("httpStatus"), httpStatus},
                           {QStringLiteral("retryAfterSec"), retryAfterSec},
                           {QStringLiteral("retryable"), retryable}};
    }
};

/// 模型客户端的最小输入：谁、哪套协议、要发什么。
struct ModelRequest {
    QString requestId;
    QString baseUrl;
    QString model;
    QString apiKey;
    QList<Message> messages;
    QJsonArray tools;
    QString systemPrompt;
    int contextWindow = 32768;
    int maxOutput = 4096;
    double temperature = 0.7;
    int timeoutSec = 120;
    QString authMode = QStringLiteral("bearer");
    bool stream = true;
    QString maxTokensField = QStringLiteral("max_tokens");
    QString systemRole = QStringLiteral("system");
    QString reasoningEcho = QStringLiteral("display-only");
    bool sendTemperature = true;
    bool sendToolChoice = true;
    bool localHttpAllowed = false;
};

struct ModelReply {
    Message message;
    Usage usage;
    QString finishReason;
    bool streamed = false;
};

}  // namespace Agent

Q_DECLARE_METATYPE(Agent::Message)
Q_DECLARE_METATYPE(Agent::Usage)
Q_DECLARE_METATYPE(Agent::Error)
Q_DECLARE_METATYPE(Agent::ModelReply)

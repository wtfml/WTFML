/**
 * @file AgentProviderAdapters.cpp
 * @brief 请求构造与 URL 校验（见 .hpp）。
 */

#include "AgentProviderAdapters.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QUrl>

namespace AgentProviderAdapters {
namespace {

bool isLoopbackHost(const QString &host) {
    const QString lower = host.toLower();
    return lower == QLatin1String("127.0.0.1") || lower == QLatin1String("localhost") ||
           lower == QLatin1String("::1") || lower == QLatin1String("[::1]");
}

QString withoutTrailingSlash(QString url) {
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    return url;
}

}  // namespace

bool validateBaseUrl(const QString &baseUrl, bool localHttpAllowed, bool *isLoopback,
                     QString *problem) {
    if (isLoopback != nullptr) *isLoopback = false;
    const auto reject = [problem](const QString &text) {
        if (problem != nullptr) *problem = text;
        return false;
    };
    if (baseUrl.trimmed().isEmpty()) {
        return reject(QCoreApplication::translate("AgentProviderAdapters", "Base URL 不能为空。"));
    }
    QUrl url(baseUrl.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) {
        return reject(QCoreApplication::translate("AgentProviderAdapters", "Base URL 格式无效。"));
    }
    if (!url.userInfo().isEmpty()) {
        return reject(QCoreApplication::translate("AgentProviderAdapters", "Base URL 不能包含用户名或密码。"));
    }
    if (url.hasFragment()) {
        return reject(QCoreApplication::translate("AgentProviderAdapters", "Base URL 不能包含 # 片段。"));
    }
    if (!url.query().isEmpty()) {
        return reject(QCoreApplication::translate("AgentProviderAdapters", "Base URL 不能包含查询串。"));
    }
    const bool loopback = isLoopbackHost(url.host());
    if (isLoopback != nullptr) *isLoopback = loopback;
    if (url.scheme() == QLatin1String("https")) {
        return true;
    }
    if (url.scheme() == QLatin1String("http")) {
        if (!loopback) {
            return reject(QCoreApplication::translate("AgentProviderAdapters", "远程服务必须使用 https://，http:// 仅允许本机回环地址。"));
        }
        if (!localHttpAllowed) {
            return reject(QCoreApplication::translate("AgentProviderAdapters", "本机 http:// 地址需先在「连接」中启用「允许本机 HTTP」。"));
        }
        return true;
    }
    return reject(QCoreApplication::translate("AgentProviderAdapters", "Base URL 仅支持 https://（本机回环地址可显式启用 http://）。"));
}

bool validateProfile(const AgentProviderProfile &profile, QString *problem) {
    if (!profile.validateEnumFields(problem)) {
        return false;
    }
    bool loopback = false;
    if (!validateBaseUrl(profile.baseUrl, profile.localHttpAllowed, &loopback, problem)) {
        return false;
    }
    if (profile.deployment == QLatin1String("local") && !loopback) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentProviderAdapters", "部署类型为本地，但 Base URL 不是回环地址。");
        }
        return false;
    }
    if (profile.authMode == QLatin1String("none") && !loopback) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentProviderAdapters", "无认证模式仅限本机回环服务。");
        }
        return false;
    }
    if (profile.model.trimmed().isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentProviderAdapters", "模型 ID 不能为空。");
        return false;
    }
    return true;
}

QString chatCompletionsUrl(const AgentProviderProfile &profile) {
    return withoutTrailingSlash(profile.baseUrl.trimmed()) + QStringLiteral("/chat/completions");
}

QString modelsUrl(const AgentProviderProfile &profile) {
    return withoutTrailingSlash(profile.baseUrl.trimmed()) + QStringLiteral("/models");
}

QByteArray authHeaderName(const AgentProviderProfile &profile) {
    if (profile.authMode == QLatin1String("api-key-header")) return QByteArrayLiteral("api-key");
    if (profile.authMode == QLatin1String("none")) return {};
    return QByteArrayLiteral("Authorization");
}

QByteArray authHeaderValue(const AgentProviderProfile &profile, const QString &apiKey) {
    if (profile.authMode == QLatin1String("none")) return {};
    if (profile.authMode == QLatin1String("api-key-header")) return apiKey.toUtf8();
    return QByteArrayLiteral("Bearer ") + apiKey.toUtf8();
}

QJsonArray buildMessages(const AgentProviderProfile &profile, const QList<Agent::Message> &messages,
                         const QString &systemPrompt) {
    QJsonArray result;
    if (!systemPrompt.trimmed().isEmpty()) {
        result.append(QJsonObject{{QStringLiteral("role"), profile.systemRole},
                                  {QStringLiteral("content"), systemPrompt}});
    }
    for (const Agent::Message &message : messages) {
        switch (message.role) {
        case Agent::Role::System:
            break;  // 系统提示由上面的 systemPrompt 统一给，避免重复。
        case Agent::Role::User:
            result.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                      {QStringLiteral("content"), message.content}});
            break;
        case Agent::Role::Assistant: {
            QJsonObject object{{QStringLiteral("role"), QStringLiteral("assistant")},
                               {QStringLiteral("content"), message.content}};
            if (profile.reasoningEcho == QLatin1String("send-back") &&
                !message.reasoning.isEmpty()) {
                object.insert(QStringLiteral("reasoning_content"), message.reasoning);
            }
            if (!message.toolCalls.isEmpty()) {
                QJsonArray calls;
                for (const Agent::ToolCall &call : message.toolCalls) {
                    calls.append(QJsonObject{
                        {QStringLiteral("id"), call.id},
                        {QStringLiteral("type"), QStringLiteral("function")},
                        {QStringLiteral("function"),
                         QJsonObject{{QStringLiteral("name"), call.name},
                                     {QStringLiteral("arguments"),
                                      QString::fromUtf8(
                                          QJsonDocument(call.arguments).toJson(QJsonDocument::Compact))}}}});
                }
                object.insert(QStringLiteral("tool_calls"), calls);
                if (message.content.isEmpty()) object.remove(QStringLiteral("content"));
            }
            result.append(object);
            break;
        }
        case Agent::Role::Tool:
            result.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                      {QStringLiteral("tool_call_id"), message.toolCallId},
                                      {QStringLiteral("name"), message.toolName},
                                      {QStringLiteral("content"), message.content}});
            break;
        }
    }
    return result;
}

QJsonObject buildRequest(const AgentProviderProfile &profile, const QList<Agent::Message> &messages,
                         const QString &systemPrompt, const QJsonArray &tools) {
    QJsonObject payload{
        {QStringLiteral("model"), profile.model},
        {QStringLiteral("messages"), buildMessages(profile, messages, systemPrompt)},
        {QStringLiteral("stream"), profile.stream},
        {profile.maxTokensField, profile.maxOutput},
    };
    if (profile.sendTemperature) {
        payload.insert(QStringLiteral("temperature"), profile.temperature);
    }
    // 思考强度：空串 = 不发（模型不支持时界面那边就不给这个选项）。
    if (!profile.reasoningEffort.isEmpty() && profile.reasoningEffort != QLatin1String("off")) {
        payload.insert(QStringLiteral("reasoning_effort"), profile.reasoningEffort);
    }
    if (!tools.isEmpty()) {
        payload.insert(QStringLiteral("tools"), tools);
        if (profile.sendToolChoice) {
            payload.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
        }
    }
    return payload;
}

}  // namespace AgentProviderAdapters

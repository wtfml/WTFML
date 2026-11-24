/**
 * @file OpenAiCompatibleClient.cpp
 * @brief `/chat/completions` 的流式/非流式解析与错误分类（见 .hpp 的边界）。
 */

#include "OpenAiCompatibleClient.hpp"

#include "AgentProviderAdapters.hpp"
#include "AgentRedaction.hpp"
#include "AgentSseParser.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>
#include <QUrl>

#include <algorithm>

namespace {

int clampTimeout(int seconds) { return qBound(5, seconds, 600); }

QString bodySummary(const QByteArray &body) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body.left(8192), &error);
    if (error.error == QJsonParseError::NoError && document.isObject()) {
        const QJsonObject object = document.object();
        const QJsonValue errorValue = object.value(QStringLiteral("error"));
        if (errorValue.isObject()) {
            const QJsonObject errorObject = errorValue.toObject();
            QString text = errorObject.value(QStringLiteral("message")).toString();
            if (text.isEmpty()) text = errorObject.value(QStringLiteral("code")).toString();
            if (!text.isEmpty()) return AgentRedaction::redact(text.left(300)).text;
        }
        const QString message = object.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) return AgentRedaction::redact(message.left(300)).text;
    }
    return AgentRedaction::redact(QString::fromUtf8(body.left(300))).text;
}

}  // namespace

OpenAiCompatibleClient::OpenAiCompatibleClient(QObject *parent)
    : AgentModelClient(parent), m_manager(new QNetworkAccessManager(this)), m_ownsManager(true) {}

OpenAiCompatibleClient::~OpenAiCompatibleClient() {
    const auto keys = m_active.keys();
    for (const QString &key : keys) {
        ActiveRequest *request = m_active.take(key);
        if (request != nullptr) {
            if (request->reply != nullptr) {
                request->reply->abort();
                request->reply->deleteLater();
            }
            delete request->parser;
            delete request;
        }
    }
}

int OpenAiCompatibleClient::maxResponseBytes() { return 16 * 1024 * 1024; }
int OpenAiCompatibleClient::maxSseLineBytes() { return AgentSseParser::maxLineBytes(); }

void OpenAiCompatibleClient::setNetworkAccessManagerForTest(QNetworkAccessManager *manager) {
    if (manager == nullptr || manager == m_manager) return;
    if (m_ownsManager && m_manager != nullptr) m_manager->deleteLater();
    m_manager = manager;
    m_ownsManager = false;
}

void OpenAiCompatibleClient::start(const Agent::ModelRequest &request) {
    begin(request, false);
}

void OpenAiCompatibleClient::testConnection(const Agent::ModelRequest &request) {
    begin(request, true);
}

void OpenAiCompatibleClient::begin(const Agent::ModelRequest &request, bool connectionTest) {
    if (m_manager == nullptr) {
        m_manager = new QNetworkAccessManager(this);
        m_ownsManager = true;
    }
    const AgentProviderProfile validationProfile = [&request] {
        AgentProviderProfile profile;
        profile.baseUrl = request.baseUrl;
        profile.model = request.model;
        profile.localHttpAllowed = request.localHttpAllowed;
        profile.authMode = request.authMode;
        return profile;
    }();
    QString problem;
    bool loopback = false;
    if (!AgentProviderAdapters::validateBaseUrl(request.baseUrl, request.localHttpAllowed,
                                                &loopback, &problem) ||
        (!connectionTest &&
         !AgentProviderAdapters::validateProfile(validationProfile, &problem))) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::NoConfiguration;
        error.message = problem;
        emit failed(request.requestId, error);
        return;
    }
    if (request.apiKey.isEmpty() && request.authMode != QLatin1String("none")) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::NoConfiguration;
        error.message = QStringLiteral("没有可用的 API Key，请先在设置里配置。");
        emit failed(request.requestId, error);
        return;
    }

    const QString urlText = connectionTest ? AgentProviderAdapters::modelsUrl(validationProfile)
                                           : AgentProviderAdapters::chatCompletionsUrl(validationProfile);
    QNetworkRequest networkRequest{QUrl(urlText)};
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                               QNetworkRequest::ManualRedirectPolicy);
    networkRequest.setTransferTimeout(clampTimeout(request.timeoutSec) * 1000);
    networkRequest.setRawHeader("Accept", connectionTest ? "application/json"
                                                          : (request.stream
                                                                 ? "text/event-stream"
                                                                 : "application/json"));
    const QByteArray authName = AgentProviderAdapters::authHeaderName(validationProfile);
    const QByteArray authValue = AgentProviderAdapters::authHeaderValue(validationProfile, request.apiKey);
    if (!authName.isEmpty() && !authValue.isEmpty()) {
        networkRequest.setRawHeader(authName, authValue);
    }

    auto *active = new ActiveRequest;
    active->request = request;
    active->streaming = !connectionTest && request.stream;
    active->isConnectionTest = connectionTest;
    active->parser = active->streaming ? new AgentSseParser : nullptr;
    active->startedAt = QDateTime::currentMSecsSinceEpoch();
    m_active.insert(request.requestId, active);

    QNetworkReply *reply = nullptr;
    if (connectionTest) {
        reply = m_manager->get(networkRequest);
    } else {
        networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                 QStringLiteral("application/json"));
        const QJsonObject payload = AgentProviderAdapters::buildRequest(
            validationProfile, request.messages, request.systemPrompt, request.tools);
        reply = m_manager->post(networkRequest,
                                QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }
    active->reply = reply;
    connect(reply, &QNetworkReply::readyRead, this,
            [this, id = request.requestId, reply] { feedReply(id, reply); });
    connect(reply, &QNetworkReply::finished, this, [this, id = request.requestId] {
        ActiveRequest *requestState = this->active(id);
        if (requestState == nullptr) return;
        finishReply(id, requestState);
    });
}

void OpenAiCompatibleClient::feedReply(const QString &requestId, QNetworkReply *reply) {
    ActiveRequest *request = active(requestId);
    if (request == nullptr || request->reply != reply) return;
    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty()) return;
    request->totalBytes += chunk.size();
    if (request->totalBytes > maxResponseBytes()) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::Protocol;
        error.message = QStringLiteral("模型响应超过 16 MB 上限，已中止。");
        reply->abort();
        failRequest(requestId, error);
        return;
    }
    // 即使请求是流式，也保留原始字节：非 2xx 时供应商返回的是普通 JSON 错误体，
    // SSE 解析器会把它当“没有 data:”丢掉，导致界面只剩“HTTP 400”而看不到原因。
    request->buffer.append(chunk);
    if (request->streaming) {
        const QList<AgentSseParser::Event> events = request->parser->feed(chunk);
        for (const AgentSseParser::Event &event : events) {
            if (event.kind == AgentSseParser::Event::Kind::Data) {
                processStreamJson(requestId, request, event.data);
                if (active(requestId) == nullptr) return;
            } else if (event.kind == AgentSseParser::Event::Kind::Done) {
                request->finishedSeen = true;
            } else if (event.kind == AgentSseParser::Event::Kind::Error) {
                Agent::Error error;
                error.kind = Agent::ErrorKind::Protocol;
                error.message = event.error;
                request->reply->abort();
                failRequest(requestId, error);
                return;
            }
        }
    }
    if (request->totalBytes > 0) request->receivedAny = true;
}

void OpenAiCompatibleClient::processStreamJson(const QString &requestId, ActiveRequest *request,
                                               const QByteArray &data) {
    if (data.trimmed().isEmpty()) return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::Protocol;
        error.message = QStringLiteral("模型返回了无法解析的流式数据。");
        request->reply->abort();
        failRequest(requestId, error);
        return;
    }
    const QJsonObject object = document.object();
    if (object.contains(QStringLiteral("error"))) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::Protocol;
        error.message = bodySummary(data);
        request->reply->abort();
        failRequest(requestId, error);
        return;
    }
    if (object.contains(QStringLiteral("usage"))) {
        const QJsonObject usage = object.value(QStringLiteral("usage")).toObject();
        request->usage.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
        request->usage.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInt();
        request->usage.estimated = false;
    }
    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) return;
    const QJsonObject choice = choices.at(0).toObject();
    const QString finish = choice.value(QStringLiteral("finish_reason")).toString();
    if (!finish.isEmpty()) {
        request->finishReason = finish;
        request->finishedSeen = true;
    }
    const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
    if (delta.contains(QStringLiteral("content"))) {
        const QString text = delta.value(QStringLiteral("content")).toString();
        if (!text.isEmpty()) {
            request->text += text;
            emit textDelta(requestId, text);
        }
    }
    QString reasoning = delta.value(QStringLiteral("reasoning_content")).toString();
    if (reasoning.isEmpty()) reasoning = delta.value(QStringLiteral("reasoning")).toString();
    if (!reasoning.isEmpty()) {
        request->reasoning += reasoning;
        emit reasoningDelta(requestId, reasoning);
    }
    for (const QJsonValue &value : delta.value(QStringLiteral("tool_calls")).toArray()) {
        const QJsonObject call = value.toObject();
        const int index = call.value(QStringLiteral("index")).toInt(request->toolCalls.size());
        Agent::ToolCall accumulated = request->toolCalls.value(index);
        const QString id = call.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) accumulated.id = id;
        const QJsonObject function = call.value(QStringLiteral("function")).toObject();
        const QString name = function.value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) accumulated.name += name;  // 分片可能把函数名拆开
        const QString arguments = function.value(QStringLiteral("arguments")).toString();
        if (!arguments.isEmpty()) {
            static const QString key = QStringLiteral("__raw");
            const QString raw = accumulated.arguments.value(key).toString() + arguments;
            accumulated.arguments.insert(key, raw);
        }
        request->toolCalls.insert(index, accumulated);
    }
}

void OpenAiCompatibleClient::processNonStreamBody(const QString &requestId, ActiveRequest *request,
                                                  const QByteArray &body) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::Protocol;
        error.message = QStringLiteral("模型返回了无法解析的 JSON。");
        failRequest(requestId, error);
        return;
    }
    const QJsonObject object = document.object();
    if (object.contains(QStringLiteral("error"))) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::Protocol;
        error.message = bodySummary(body);
        failRequest(requestId, error);
        return;
    }
    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::Protocol;
        error.message = QStringLiteral("模型响应里没有 choices。");
        failRequest(requestId, error);
        return;
    }
    const QJsonObject choice = choices.at(0).toObject();
    const QJsonObject message = choice.value(QStringLiteral("message")).toObject();
    request->text = message.value(QStringLiteral("content")).toString();
    request->reasoning = message.value(QStringLiteral("reasoning_content")).toString();
    if (request->reasoning.isEmpty()) {
        request->reasoning = message.value(QStringLiteral("reasoning")).toString();
    }
    int index = 0;
    for (const QJsonValue &value : message.value(QStringLiteral("tool_calls")).toArray()) {
        const QJsonObject call = value.toObject();
        Agent::ToolCall toolCall;
        toolCall.id = call.value(QStringLiteral("id")).toString();
        const QJsonObject function = call.value(QStringLiteral("function")).toObject();
        toolCall.name = function.value(QStringLiteral("name")).toString();
        toolCall.arguments.insert(QStringLiteral("__raw"),
                                  function.value(QStringLiteral("arguments")).toString());
        request->toolCalls.insert(index++, toolCall);
    }
    const QJsonObject usage = object.value(QStringLiteral("usage")).toObject();
    request->usage.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
    request->usage.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInt();
    request->finishReason = choice.value(QStringLiteral("finish_reason")).toString();
    request->finishedSeen = true;
}

Agent::ModelReply OpenAiCompatibleClient::replyFromActive(const ActiveRequest &request) const {
    Agent::ModelReply reply;
    reply.message.role = Agent::Role::Assistant;
    reply.message.content = request.text;
    reply.message.reasoning = request.reasoning;
    reply.message.status = Agent::MessageStatus::Complete;
    reply.usage = request.usage;
    reply.finishReason = request.finishReason;
    reply.streamed = request.streaming;
    QList<int> indexes = request.toolCalls.keys();
    std::sort(indexes.begin(), indexes.end());
    for (const int index : indexes) {
        Agent::ToolCall call = request.toolCalls.value(index);
        const QString raw = call.arguments.value(QStringLiteral("__raw")).toString();
        call.arguments.remove(QStringLiteral("__raw"));
        if (!raw.isEmpty()) {
            const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
            if (document.isObject()) call.arguments = document.object();
        }
        if (!call.name.isEmpty()) reply.message.toolCalls.append(call);
    }
    return reply;
}

void OpenAiCompatibleClient::finishReply(const QString &requestId, ActiveRequest *request) {
    QNetworkReply *reply = request->reply;
    if (reply == nullptr) return;
    feedReply(requestId, reply);
    if (active(requestId) == nullptr) return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = request->buffer;

    if (request->aborted) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::Cancelled;
        error.message = QStringLiteral("已停止生成。");
        failRequest(requestId, error);
        return;
    }
    if (request->isConnectionTest) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - request->startedAt;
        const bool ok = status >= 200 && status < 300;
        QString summary;
        if (ok) {
            summary = QStringLiteral("连接成功（HTTP %1，耗时 %2 ms）。").arg(status).arg(elapsed);
        } else if (status == 404) {
            summary = QStringLiteral("服务端不支持模型列表（HTTP 404）；无法通过列表验证，但配置格式有效。");
        } else if (status == 401 || status == 403) {
            summary = QStringLiteral("认证失败（HTTP %1），请检查 API Key。").arg(status);
        } else if (status == 0) {
            summary = classifyNetworkError(reply).message;
        } else {
            summary = QStringLiteral("连接失败（HTTP %1）。").arg(status);
        }
        emit connectionTested(requestId, ok, status, summary);
        cleanup(request);
        return;
    }
    if (status >= 300) {
        failRequest(requestId, classifyHttpStatus(status, body));
        return;
    }
    if (status == 0 && reply->error() != QNetworkReply::NoError) {
        Agent::Error error = classifyNetworkError(reply);
        if (!request->text.isEmpty() || !request->toolCalls.isEmpty()) {
            error.kind = Agent::ErrorKind::Network;
            error.message = QStringLiteral("流已开始后中断，已保留收到的部分内容。");
            error.retryable = false;
        }
        failRequest(requestId, error);
        return;
    }
    if (request->streaming) {
        if (request->parser != nullptr) {
            const QList<AgentSseParser::Event> tail = request->parser->finish();
            for (const AgentSseParser::Event &event : tail) {
                if (event.kind == AgentSseParser::Event::Kind::Data) {
                    processStreamJson(requestId, request, event.data);
                    if (active(requestId) == nullptr) return;
                } else if (event.kind == AgentSseParser::Event::Kind::Error) {
                    Agent::Error error;
                    error.kind = Agent::ErrorKind::Protocol;
                    error.message = event.error;
                    failRequest(requestId, error);
                    return;
                }
            }
        }
    } else {
        processNonStreamBody(requestId, request, body);
        if (active(requestId) == nullptr) return;
    }
    if (request->text.isEmpty() && request->toolCalls.isEmpty() &&
        request->finishReason.isEmpty()) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::Protocol;
        error.message = QStringLiteral("模型返回了空响应。");
        failRequest(requestId, error);
        return;
    }
    if (request->usage.promptTokens == 0 && request->usage.completionTokens == 0) {
        request->usage.estimated = true;
    }
    emit finished(requestId, replyFromActive(*request));
    cleanup(request);
}

void OpenAiCompatibleClient::cancel(const QString &requestId) {
    ActiveRequest *request = active(requestId);
    if (request == nullptr) return;
    request->aborted = true;
    if (request->reply != nullptr) request->reply->abort();
}

void OpenAiCompatibleClient::failRequest(const QString &requestId, const Agent::Error &error) {
    cleanup(active(requestId));
    emit failed(requestId, error);
}

void OpenAiCompatibleClient::cleanup(ActiveRequest *request) {
    if (request == nullptr) return;
    for (auto it = m_active.begin(); it != m_active.end(); ++it) {
        if (it.value() == request) {
            m_active.erase(it);
            break;
        }
    }
    if (request->reply != nullptr) request->reply->deleteLater();
    delete request->parser;
    delete request;
}

Agent::Error OpenAiCompatibleClient::classifyNetworkError(QNetworkReply *reply) const {
    Agent::Error error;
    error.kind = Agent::ErrorKind::Network;
    switch (reply->error()) {
    case QNetworkReply::OperationCanceledError:
        error.kind = Agent::ErrorKind::Cancelled;
        error.message = QStringLiteral("已停止生成。");
        break;
    case QNetworkReply::TimeoutError:
        error.message = QStringLiteral("请求超时。");
        error.retryable = true;
        break;
    case QNetworkReply::SslHandshakeFailedError:
        error.message = QStringLiteral("TLS 证书握手失败；不会忽略证书错误。");
        break;
    case QNetworkReply::HostNotFoundError:
        error.message = QStringLiteral("找不到模型服务主机（DNS 失败）。");
        error.retryable = true;
        break;
    case QNetworkReply::ConnectionRefusedError:
        error.message = QStringLiteral("模型服务拒绝连接。");
        error.retryable = true;
        break;
    case QNetworkReply::RemoteHostClosedError:
        error.message = QStringLiteral("模型服务提前关闭了连接。");
        error.retryable = true;
        break;
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        error.message = QStringLiteral("网络暂时不可用。");
        error.retryable = true;
        break;
    default:
        error.message = QStringLiteral("网络请求失败（%1）。").arg(reply->errorString());
        break;
    }
    return error;
}

Agent::Error OpenAiCompatibleClient::classifyHttpStatus(int status, const QByteArray &body) const {
    Agent::Error error;
    error.httpStatus = status;
    if (status == 401 || status == 403) {
        error.kind = Agent::ErrorKind::Unauthorized;
        error.message = QStringLiteral("认证失败（HTTP %1），请检查 API Key/权限。").arg(status);
        return error;
    }
    if (status == 429) {
        error.kind = Agent::ErrorKind::RateLimited;
        error.message = QStringLiteral("请求被限流（HTTP 429）。");
        error.retryable = true;
        return error;
    }
    const QString summary = bodySummary(body);
    if (status == 402 || summary.contains(QLatin1String("quota"), Qt::CaseInsensitive) ||
        summary.contains(QStringLiteral("余额"), Qt::CaseInsensitive) ||
        summary.contains(QStringLiteral("配额"), Qt::CaseInsensitive)) {
        error.kind = Agent::ErrorKind::Quota;
        error.message = summary.isEmpty() ? QStringLiteral("额度/配额不足。") : summary;
        return error;
    }
    if (status >= 500) {
        error.kind = Agent::ErrorKind::Network;
        error.message = QStringLiteral("模型服务暂时不可用（HTTP %1）。").arg(status);
        error.retryable = true;
        return error;
    }
    if (status >= 300 && status < 400) {
        error.kind = Agent::ErrorKind::Protocol;
        error.message = QStringLiteral("模型服务返回了重定向（HTTP %1）；本客户端不跟随重定向。").arg(status);
        return error;
    }
    error.kind = Agent::ErrorKind::Protocol;
    error.message = summary.isEmpty() ? QStringLiteral("模型服务返回 HTTP %1。").arg(status) : summary;
    return error;
}

OpenAiCompatibleClient::ActiveRequest *OpenAiCompatibleClient::active(const QString &requestId) {
    const auto it = m_active.constFind(requestId);
    return it == m_active.constEnd() ? nullptr : it.value();
}

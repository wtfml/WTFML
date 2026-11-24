/**
 * @file AgentLoop.cpp
 * @brief 无工具对话循环（见 .hpp 的重试/取消边界）。
 */

#include "AgentLoop.hpp"

#include "AgentContextBuilder.hpp"
#include "AgentProviderAdapters.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>

AgentLoop::AgentLoop(AgentModelClient *client, QObject *parent)
    : QObject(parent), m_client(client) {
    if (m_client != nullptr) {
        connect(m_client, &AgentModelClient::textDelta, this,
                [this](const QString &requestId, const QString &text) {
                    if (requestId != m_requestId || m_state != State::Requesting) return;
                    m_assistant.content += text;
                    emit assistantUpdated(m_assistant);
                });
        connect(m_client, &AgentModelClient::reasoningDelta, this,
                [this](const QString &requestId, const QString &text) {
                    if (requestId != m_requestId || m_state != State::Requesting) return;
                    m_assistant.reasoning += text;
                    emit assistantUpdated(m_assistant);
                });
        connect(m_client, &AgentModelClient::finished, this,
                [this](const QString &requestId, const Agent::ModelReply &reply) {
                    handleFinished(requestId, reply);
                });
        connect(m_client, &AgentModelClient::failed, this,
                [this](const QString &requestId, const Agent::Error &error) {
                    handleFailed(requestId, error);
                });
    }
}

void AgentLoop::setClient(AgentModelClient *client) {
    if (client == m_client) return;
    if (m_client != nullptr) disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    if (m_client != nullptr) {
        connect(m_client, &AgentModelClient::textDelta, this,
                [this](const QString &requestId, const QString &text) {
                    if (requestId != m_requestId || m_state != State::Requesting) return;
                    m_assistant.content += text;
                    emit assistantUpdated(m_assistant);
                });
        connect(m_client, &AgentModelClient::reasoningDelta, this,
                [this](const QString &requestId, const QString &text) {
                    if (requestId != m_requestId || m_state != State::Requesting) return;
                    m_assistant.reasoning += text;
                    emit assistantUpdated(m_assistant);
                });
        connect(m_client, &AgentModelClient::finished, this,
                [this](const QString &requestId, const Agent::ModelReply &reply) {
                    handleFinished(requestId, reply);
                });
        connect(m_client, &AgentModelClient::failed, this,
                [this](const QString &requestId, const Agent::Error &error) {
                    handleFailed(requestId, error);
                });
    }
}

void AgentLoop::setConfig(const Config &config) { m_config = config; }

void AgentLoop::setToolEngine(AgentToolEngine *engine) {
    m_toolEngine = engine;
    if (m_toolEngine != nullptr) {
        m_toolEngine->setCompletionSink(
            [this](const Agent::ToolCall &call, const AgentToolResult &result) {
                onAsyncToolDone(call, result);
            });
    }
}

void AgentLoop::setHistory(const QList<Agent::Message> &history) { m_history = history; }

void AgentLoop::setRetryDelaysForTest(const QList<int> &delays) {
    m_config.retryDelaysMs = delays;
}

bool AgentLoop::busy() const {
    return m_state == State::Preparing || m_state == State::Requesting ||
           m_state == State::AwaitingApproval || m_state == State::ExecutingTool;
}

QString AgentLoop::stateName(State state) {
    switch (state) {
    case State::Idle: return QStringLiteral("idle");
    case State::Preparing: return QStringLiteral("preparing");
    case State::Requesting: return QStringLiteral("requesting");
    case State::AwaitingApproval: return QStringLiteral("awaiting-approval");
    case State::ExecutingTool: return QStringLiteral("executing-tool");
    case State::Completed: return QStringLiteral("completed");
    case State::Failed: return QStringLiteral("failed");
    case State::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("idle");
}

void AgentLoop::setState(State state) {
    if (m_state == state) return;
    m_state = state;
    emit stateChanged(state);
}

Agent::Error AgentLoop::configurationError() const {
    Agent::Error error;
    error.kind = Agent::ErrorKind::NoConfiguration;
    if (m_client == nullptr) {
        error.message = QStringLiteral("模型客户端未就绪。");
        return error;
    }
    QString problem;
    if (!AgentProviderAdapters::validateProfile(m_config.profile, &problem)) {
        error.message = problem;
        return error;
    }
    if (m_config.apiKey.isEmpty() && m_config.profile.authMode != QLatin1String("none")) {
        error.message = QStringLiteral("没有可用的 API Key，请先在设置里配置。");
        return error;
    }
    return {};
}

void AgentLoop::start() {
    if (busy()) return;
    if (m_history.isEmpty() || m_history.constLast().role != Agent::Role::User) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::NoConfiguration;
        error.message = QStringLiteral("没有可发送的用户消息。");
        emit turnFailed(error);
        return;
    }
    m_generation += 1;
    m_retryAttempt = 0;
    m_toolSteps = 0;
    m_nextToolCall = 0;
    m_toolQueue.clear();
    m_approvalId.clear();
    m_cancelled = false;
    m_lastError = {};
    m_usage = {};
    beginRequest(false);
}

void AgentLoop::retry() {
    if (busy()) return;
    if (!m_history.isEmpty() && m_history.constLast().role == Agent::Role::Assistant) {
        m_history.removeLast();
    }
    if (m_history.isEmpty() || m_history.constLast().role != Agent::Role::User) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::NoConfiguration;
        error.message = QStringLiteral("没有可重试的用户消息。");
        emit turnFailed(error);
        return;
    }
    m_generation += 1;
    m_retryAttempt = 0;
    m_toolSteps = 0;
    m_nextToolCall = 0;
    m_toolQueue.clear();
    m_approvalId.clear();
    m_cancelled = false;
    m_lastError = {};
    beginRequest(false);
}

void AgentLoop::beginRequest(bool isRetry) {
    Q_UNUSED(isRetry);
    const Agent::Error configuration = configurationError();
    if (!configuration.isEmpty()) {
        deliverFailure(configuration);
        return;
    }
    setState(State::Preparing);
    AgentContextReport report;
    const QList<Agent::Message> context = AgentContextBuilder::build(
        m_history, m_config.systemPrompt, m_config.profile.contextWindow,
        m_config.profile.maxOutput, &report);
    if (context.isEmpty() && m_history.constLast().role == Agent::Role::User) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::ContextOverflow;
        error.message = QStringLiteral("上下文预算不足以容纳当前消息。");
        deliverFailure(error);
        return;
    }
    if (m_assistant.id.isEmpty() || m_assistant.status != Agent::MessageStatus::Streaming) {
        m_assistant = {};
        m_assistant.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_assistant.role = Agent::Role::Assistant;
        m_assistant.status = Agent::MessageStatus::Streaming;
        m_assistant.createdAt = QDateTime::currentDateTimeUtc();
        m_assistantAppended = false;
    }
    Agent::ModelRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.baseUrl = m_config.profile.baseUrl;
    request.model = m_config.profile.model;
    request.apiKey = m_config.apiKey;
    request.messages = context;
    request.systemPrompt = m_config.systemPrompt;
    request.contextWindow = m_config.profile.contextWindow;
    request.maxOutput = m_config.profile.maxOutput;
    request.temperature = m_config.profile.temperature;
    request.timeoutSec = m_config.profile.timeoutSec;
    request.authMode = m_config.profile.authMode;
    request.stream = m_config.profile.stream;
    request.maxTokensField = m_config.profile.maxTokensField;
    request.systemRole = m_config.profile.systemRole;
    request.reasoningEcho = m_config.profile.reasoningEcho;
    request.sendTemperature = m_config.profile.sendTemperature;
    request.sendToolChoice = m_config.profile.sendToolChoice;
    request.localHttpAllowed = m_config.profile.localHttpAllowed;
    if (m_toolEngine != nullptr) request.tools = m_toolEngine->schemas();
    m_requestId = request.requestId;
    setState(State::Requesting);
    emit assistantUpdated(m_assistant);
    m_client->start(request);
}

void AgentLoop::handleFinished(const QString &requestId, const Agent::ModelReply &reply) {
    if (requestId != m_requestId || m_state != State::Requesting) return;
    m_requestId.clear();
    if (!reply.message.content.isEmpty()) m_assistant.content = reply.message.content;
    if (!reply.message.reasoning.isEmpty()) m_assistant.reasoning = reply.message.reasoning;
    if (!reply.message.toolCalls.isEmpty()) m_assistant.toolCalls = reply.message.toolCalls;
    m_assistant.status = Agent::MessageStatus::Complete;
    m_assistant.errorText.clear();
    m_assistant.errorKind = Agent::ErrorKind::None;
    m_usage = reply.usage;
    emit usageUpdated(m_usage);
    appendAssistantToHistory();
    emit assistantUpdated(m_assistant);
    emit messageAdded(m_assistant);
    if (!m_assistant.toolCalls.isEmpty()) {
        m_toolQueue = m_assistant.toolCalls;
        m_nextToolCall = 0;
        processToolCalls();
        return;
    }
    setState(State::Completed);
    emit turnFinished(m_assistant);
}

void AgentLoop::handleFailed(const QString &requestId, const Agent::Error &error) {
    if (requestId != m_requestId || m_state != State::Requesting) return;
    m_requestId.clear();
    m_lastError = error;
    if (error.kind == Agent::ErrorKind::Cancelled) {
        m_assistant.status = Agent::MessageStatus::Interrupted;
        appendAssistantToHistory();
        setState(State::Cancelled);
        emit assistantUpdated(m_assistant);
        emit turnFinished(m_assistant);
        return;
    }
    // 首字节前可重试；已经有文本就不自动重发（避免重复计费/拼接错乱）。
    const bool noContent = m_assistant.content.isEmpty() && m_assistant.reasoning.isEmpty();
    if (error.retryable && noContent && m_retryAttempt < m_config.maxRetry) {
        const int index = qMin(m_retryAttempt, m_config.retryDelaysMs.size() - 1);
        int delay = index >= 0 ? m_config.retryDelaysMs.at(index) : 0;
        if (error.retryAfterSec >= 0) {
            delay = qMax(delay, qBound(0, error.retryAfterSec, 60) * 1000);
        }
        m_retryAttempt += 1;
        setState(State::Preparing);
        const int generation = m_generation;
        QTimer::singleShot(qMax(0, delay), this, [this, generation] {
            if (generation != m_generation || m_state != State::Preparing) return;
            beginRequest(true);
        });
        return;
    }
    if (noContent) {
        m_assistant.status = Agent::MessageStatus::Error;
    } else {
        m_assistant.status = Agent::MessageStatus::Interrupted;
    }
    m_assistant.errorText = error.message;
    m_assistant.errorKind = error.kind;
    appendAssistantToHistory();
    setState(State::Failed);
    emit assistantUpdated(m_assistant);
    emit turnFinished(m_assistant);
    emit turnFailed(error);
}

void AgentLoop::deliverFailure(const Agent::Error &error) {
    m_lastError = error;
    m_assistantAppended = false;
    m_assistant.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_assistant.role = Agent::Role::Assistant;
    m_assistant.status = Agent::MessageStatus::Error;
    m_assistant.errorText = error.message;
    m_assistant.errorKind = error.kind;
    m_assistant.createdAt = QDateTime::currentDateTimeUtc();
    appendAssistantToHistory();
    setState(State::Failed);
    emit assistantUpdated(m_assistant);
    emit turnFinished(m_assistant);
    emit turnFailed(error);
}

void AgentLoop::appendAssistantToHistory() {
    if (m_assistant.id.isEmpty() || m_assistantAppended) return;
    m_history.append(m_assistant);
    m_assistantAppended = true;
}

void AgentLoop::processToolCalls() {
    while (m_nextToolCall < m_toolQueue.size()) {
        const Agent::ToolCall call = m_toolQueue.at(m_nextToolCall);
        emit toolCallStarted(call);
        if (m_toolEngine == nullptr) {
            AgentToolResult result;
            result.status = AgentToolResult::Status::Error;
            result.summary = QStringLiteral("工具未注册。");
            result.error.kind = Agent::ErrorKind::ToolError;
            result.error.message = result.summary;
            handleToolOutcome(call, result);
            ++m_nextToolCall;
            continue;
        }
        setState(State::ExecutingTool);
        const AgentToolResult result = m_toolEngine->invoke(call);
        if (result.status == AgentToolResult::Status::Pending) {
            // 异步工具（MCP）：等它自己回来，不推进 m_nextToolCall。
            m_asyncPending = true;
            m_asyncGeneration = m_generation;
            return;
        }
        if (result.status == AgentToolResult::Status::ApprovalRequired) {
            m_pendingCall = call;
            m_approvalId = result.approval.value(QStringLiteral("approvalId")).toString();
            setState(State::AwaitingApproval);
            emit approvalRequested(m_approvalId, call, result.approval);
            return;
        }
        handleToolOutcome(call, result);
        ++m_nextToolCall;
    }
    m_toolQueue.clear();
    m_nextToolCall = 0;
    m_toolSteps += 1;
    if (m_toolSteps > m_config.maxToolSteps) {
        Agent::Error error;
        error.kind = Agent::ErrorKind::ToolError;
        error.message = QStringLiteral("超过最大工具步数，已停止。");
        deliverFailure(error);
        return;
    }
    setState(State::Preparing);
    beginRequest(false);
}

void AgentLoop::handleToolOutcome(const Agent::ToolCall &call, const AgentToolResult &result) {
    Agent::Message toolMessage;
    toolMessage.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    toolMessage.role = Agent::Role::Tool;
    toolMessage.toolCallId = call.id;
    toolMessage.toolName = call.name;
    toolMessage.status = result.status == AgentToolResult::Status::Ok
                             ? Agent::MessageStatus::Complete
                             : Agent::MessageStatus::Error;
    toolMessage.errorText = result.error.message;
    toolMessage.errorKind = result.error.kind;
    toolMessage.createdAt = QDateTime::currentDateTimeUtc();
    QJsonObject body{{QStringLiteral("ok"), result.status == AgentToolResult::Status::Ok},
                     {QStringLiteral("summary"), result.summary},
                     {QStringLiteral("data"), result.data}};
    if (!result.error.isEmpty()) body.insert(QStringLiteral("error"), result.error.toJson());
    toolMessage.content = QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_history.append(toolMessage);
    emit messageAdded(toolMessage);
    emit toolCallFinished(call, result);
}

void AgentLoop::resolveApproval(bool approved) {
    if (m_state != State::AwaitingApproval || m_toolEngine == nullptr) return;
    const QString approvalId = m_approvalId;
    m_approvalId.clear();
    setState(State::ExecutingTool);
    const AgentToolResult result = m_toolEngine->resolveApproval(approvalId, approved);
    if (result.status == AgentToolResult::Status::Pending) {
        // 批准之后才开始真正的异步调用：等 sink 回来再推进。
        m_asyncPending = true;
        m_asyncGeneration = m_generation;
        m_pendingCall = {};
        return;
    }
    handleToolOutcome(m_pendingCall, result);
    m_pendingCall = {};
    ++m_nextToolCall;
    processToolCalls();
}

void AgentLoop::invalidateRequest() {
    m_generation += 1;
    if (!m_requestId.isEmpty() && m_client != nullptr) {
        m_client->cancel(m_requestId);
    }
    m_requestId.clear();
}

void AgentLoop::onAsyncToolDone(const Agent::ToolCall &call, const AgentToolResult &result) {
    if (!m_asyncPending) return;
    if (m_generation != m_asyncGeneration) return;  // 换会话/取消后的旧回调
    if (m_state != State::ExecutingTool) return;
    m_asyncPending = false;
    handleToolOutcome(call, result);
    ++m_nextToolCall;
    processToolCalls();
}

void AgentLoop::cancel() {
    if (!busy() || m_cancelled) return;
    m_cancelled = true;
    m_asyncPending = false;
    if (m_toolEngine != nullptr) m_toolEngine->cancelPending();
    m_approvalId.clear();
    m_pendingCall = {};
    m_toolQueue.clear();
    m_nextToolCall = 0;
    invalidateRequest();
    m_assistant.status = Agent::MessageStatus::Interrupted;
    appendAssistantToHistory();
    setState(State::Cancelled);
    emit assistantUpdated(m_assistant);
    emit turnFinished(m_assistant);
}

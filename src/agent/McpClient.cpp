/**
 * @file McpClient.cpp
 * @brief stdio MCP 会话（见 .hpp）。
 */

#include "McpClient.hpp"

#include "AgentRedaction.hpp"
#include "McpProtocol.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QTimer>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QString truncated(const QString &text, int limit) {
    return text.size() > limit ? text.left(limit) + QStringLiteral("…") : text;
}

Agent::Error errorFor(Agent::ErrorKind kind, const QString &message) {
    Agent::Error error;
    error.kind = kind;
    error.message = message;
    return error;
}

}  // namespace

McpClient::McpClient(QObject *parent) : QObject(parent) {}

McpClient::~McpClient() {
    // 析构里也要清进程树：Job Object 关掉时 KILL_ON_JOB_CLOSE 会带走子进程。
    stop();
}

QString McpClient::stateName(State state) {
    switch (state) {
    case State::Idle: return QStringLiteral("idle");
    case State::Starting: return QStringLiteral("starting");
    case State::Handshaking: return QStringLiteral("handshaking");
    case State::Listing: return QStringLiteral("listing");
    case State::Ready: return QStringLiteral("ready");
    case State::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("idle");
}

void McpClient::setState(State state, const QString &detail) {
    m_state = state;
    m_detail = detail;
    emit stateChanged(state, detail);
}

void McpClient::setupJobObject() {
#if defined(Q_OS_WIN)
    if (m_jobHandle != 0) return;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        emitLog(QStringLiteral("MCP：Job Object 创建失败，进程树可能残留。"));  // i18n-ignore
        return;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        emitLog(QStringLiteral("MCP：Job Object 限制设置失败。"));  // i18n-ignore
        return;
    }
    HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                 DWORD(m_process->processId()));
    if (process != nullptr) {
        AssignProcessToJobObject(job, process);
        CloseHandle(process);
    }
    m_jobHandle = qint64(job);
#endif
}

void McpClient::killProcessTree() {
#if defined(Q_OS_WIN)
    if (m_jobHandle != 0) {
        TerminateJobObject(HANDLE(m_jobHandle), 1);
        CloseHandle(HANDLE(m_jobHandle));
        m_jobHandle = 0;
    }
#endif
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void McpClient::emitLog(const QString &text) {
    const AgentRedaction::Result redacted = AgentRedaction::redact(text);
    emit logLine(truncated(redacted.text, 500));
}

void McpClient::start(const McpServerConfig &server, const McpExecutableIdentity &identity) {
    if (m_process != nullptr) stop();
    m_server = server;
    m_identity = identity;
    m_protocolVersion.clear();
    m_tools.clear();
    m_listCursor.clear();
    m_listPages = 0;
    m_junkLines = 0;
    m_buffer.clear();
    m_stopping = false;

    if (!m_identity.isValid()) {
        setState(State::Failed, tr("可执行文件身份不可用，拒绝启动。"));
        return;
    }
    if (!QFileInfo(server.command).isAbsolute()) {
        setState(State::Failed, tr("可执行文件须为绝对路径。"));
        return;
    }
    if (!QFileInfo(server.command).exists()) {
        setState(State::Failed, tr("可执行文件不存在。"));
        return;
    }

    m_process = new QProcess(this);
    m_process->setProgram(server.command);
    m_process->setArguments(server.arguments);
    QString workDir = server.workingDirectory;
    if (workDir.isEmpty()) workDir = QDir::currentPath();
    m_process->setWorkingDirectory(workDir);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // 只额外透传白名单里的变量名；不把整份环境再"加"一遍，也不传 WTFML 的凭据。
    if (!server.environmentNames.isEmpty()) {
        QProcessEnvironment filtered = QProcessEnvironment();
        for (const QString &name : server.environmentNames) {
            if (environment.contains(name)) filtered.insert(name, environment.value(name));
        }
        environment = filtered;
    }
    m_process->setProcessEnvironment(environment);

    connect(m_process, &QProcess::started, this, &McpClient::onStarted);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &McpClient::onReadyRead);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        if (m_process == nullptr) return;
        const QString text = QString::fromUtf8(m_process->readAllStandardError());
        for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            emitLog(QStringLiteral("stderr: %1").arg(line));
        }
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_stopping || m_process == nullptr) return;
        if (error == QProcess::FailedToStart) {
            failSession(tr("服务器进程无法启动。"));
        }
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                if (m_stopping) return;
                Q_UNUSED(status);
                failSession(tr("服务器进程已退出（退出码 %1），不会自动重启。").arg(code));
            });

    setState(State::Starting, tr("正在启动服务器…"));
    m_process->start();
}

void McpClient::onStarted() {
    if (m_process == nullptr) return;
    setupJobObject();
    setState(State::Handshaking, tr("正在握手…"));
    const QJsonObject params{
        {QStringLiteral("protocolVersion"), McpProtocol::supportedProtocolVersion()},
        {QStringLiteral("capabilities"), QJsonObject{}},
        {QStringLiteral("clientInfo"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("WTFML Agent")},
                     {QStringLiteral("version"), QStringLiteral("1")}}}};
    const QString id = QStringLiteral("init-%1").arg(m_nextRequestId++);
    startRequest(id, Pending::Kind::Initialize,
                 McpProtocol::encodeRequest(id, QStringLiteral("initialize"), params),
                 m_server.handshakeTimeoutMs);
}

void McpClient::startRequest(const QString &id, Pending::Kind kind, const QByteArray &frame,
                             int timeoutMs, const QString &toolName) {
    if (m_process == nullptr) return;
    if (m_pending.size() >= McpLimits::kMaxPendingRequests) {
        failPending(id, Agent::ErrorKind::ToolError, tr("并发 MCP 请求过多。"));
        return;
    }
    Pending pending;
    pending.kind = kind;
    pending.toolName = toolName;
    pending.timer = new QTimer(this);
    pending.timer->setSingleShot(true);
    pending.timer->setInterval(timeoutMs);
    connect(pending.timer, &QTimer::timeout, this, [this, id, kind] {
        const QString what = kind == Pending::Kind::Initialize ? tr("握手")
                             : kind == Pending::Kind::List     ? tr("列工具")
                                                               : tr("工具调用");
        failPending(id, Agent::ErrorKind::Protocol,
                    tr("MCP %1超时，已按配置终止服务器进程。").arg(what));
        failSession(tr("MCP %1超时，不自动重启。").arg(what));
    });
    pending.timer->start();
    m_pending.insert(id, pending);
    m_process->write(frame);
    m_process->waitForBytesWritten(200);
}

void McpClient::finishPending(const QString &id) {
    if (!m_pending.contains(id)) return;
    Pending pending = m_pending.take(id);
    if (pending.timer != nullptr) {
        pending.timer->stop();
        pending.timer->deleteLater();
    }
}

void McpClient::failPending(const QString &id, Agent::ErrorKind kind, const QString &message) {
    if (!m_pending.contains(id)) return;
    Pending pending = m_pending.value(id);
    finishPending(id);
    if (pending.kind == Pending::Kind::Call) {
        emit toolFailed(id, errorFor(kind, message));
    }
}

void McpClient::failAllPending(Agent::ErrorKind kind, const QString &message) {
    const QStringList ids = m_pending.keys();
    for (const QString &id : ids) failPending(id, kind, message);
}

void McpClient::failSession(const QString &reason) {
    const bool wasFailed = m_state == State::Failed;
    failAllPending(Agent::ErrorKind::Protocol, reason);
    killProcessTree();
    if (m_process != nullptr) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_tools.clear();
    if (!wasFailed) {
        setState(State::Failed, reason);
        emit toolsChanged();
    }
}

void McpClient::stop() {
    m_stopping = true;
    failAllPending(Agent::ErrorKind::Cancelled, tr("MCP 会话已停止。"));
    killProcessTree();
    if (m_process != nullptr) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_tools.clear();
    if (m_state != State::Idle) {
        setState(State::Idle, tr("已停止。"));
        emit toolsChanged();
    }
    m_stopping = false;
}

void McpClient::onReadyRead() {
    if (m_process == nullptr) return;
    m_buffer += m_process->readAllStandardOutput();
    if (m_buffer.size() > McpLimits::kMaxLineBytes * 2) {
        // 单行超过上限还等不到换行：按垃圾行处理，绝不让缓冲无界增长。
        emitLog(QStringLiteral("stdout 出现超长行，已丢弃。"));  // i18n-ignore
        m_buffer.clear();
        if (++m_junkLines > McpLimits::kMaxJunkLines) {
            failSession(tr("stdout 无效输出过多，已终止会话。"));
        }
        return;
    }
    int newline = m_buffer.indexOf('\n');
    while (newline >= 0) {
        const QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        handleLine(line);
        if (m_process == nullptr) return;
        newline = m_buffer.indexOf('\n');
    }
}

void McpClient::handleLine(const QByteArray &line) {
    const McpProtocol::Frame frame = McpProtocol::parseLine(line);
    if (!frame.valid) {
        ++m_junkLines;
        emitLog(QStringLiteral("stdout 非协议内容：%1").arg(frame.problem));  // i18n-ignore
        if (m_junkLines > McpLimits::kMaxJunkLines) {
            failSession(tr("stdout 无效输出过多，已终止会话。"));
        }
        return;
    }
    // 服务器反向请求：ping 直接回，其它方法回 -32601，不挂起。
    if (!frame.method.isEmpty()) {
        if (frame.hasId) {
            if (m_process != nullptr) {
                m_process->write(McpProtocol::replyToServerRequest(frame));
                m_process->waitForBytesWritten(100);
            }
        } else if (frame.method == QStringLiteral("notifications/tools/list_changed")) {
            requestToolList();
        }
        return;
    }
    if (!frame.hasId) return;
    if (!m_pending.contains(frame.id)) {
        emitLog(QStringLiteral("收到未知请求 id 的回包：%1").arg(frame.id));  // i18n-ignore
        return;
    }
    handleResponse(frame.id, frame.hasError, frame.error, frame.result);
}

void McpClient::handleResponse(const QString &id, const bool hasError, const QJsonObject &error,
                               const QJsonObject &result) {
    const Pending pending = m_pending.value(id);
    finishPending(id);
    if (hasError) {
        const QString message = tr("MCP 错误 %1：%2")
                                    .arg(error.value(QStringLiteral("code")).toInt())
                                    .arg(error.value(QStringLiteral("message")).toString());
        if (pending.kind == Pending::Kind::Call) {
            emit toolFailed(id, errorFor(Agent::ErrorKind::ToolError, message));
        } else {
            failSession(message);
        }
        return;
    }
    switch (pending.kind) {
    case Pending::Kind::Initialize: {
        m_protocolVersion = result.value(QStringLiteral("protocolVersion")).toString();
        if (m_protocolVersion.isEmpty()) {
            failSession(tr("服务器未返回协议版本。"));
            return;
        }
        if (m_protocolVersion < QStringLiteral("2024-11-05")) {
            failSession(tr("服务器协议版本 %1 过旧（最低要求 2024-11-05）。")
                            .arg(m_protocolVersion));
            return;
        }
        if (m_process != nullptr) {
            m_process->write(McpProtocol::encodeNotification(
                QStringLiteral("notifications/initialized"), QJsonObject{}));
        }
        m_listCursor.clear();
        m_listPages = 0;
        m_tools.clear();
        setState(State::Listing, tr("正在获取工具列表…"));
        sendToolsList();
        return;
    }
    case Pending::Kind::List: {
        parseToolsResult(result);
        const QString next = result.value(QStringLiteral("nextCursor")).toString();
        ++m_listPages;
        if (!next.isEmpty() && m_listPages < McpLimits::kMaxListPages) {
            m_listCursor = next;
            sendToolsList();
            return;
        }
        if (!next.isEmpty()) {
            emitLog(QStringLiteral("tools/list 超过 %1 页，剩下的没有列出来。")  // i18n-ignore
                        .arg(McpLimits::kMaxListPages));
        }
        m_listCursor.clear();
        setState(State::Ready, tr("就绪（%1 个工具）").arg(m_tools.size()));
        emit toolsChanged();
        return;
    }
    case Pending::Kind::Call:
        emit toolFinished(id, result);
        return;
    }
}

void McpClient::parseToolsResult(const QJsonObject &result) {
    for (const QJsonValue &value : result.value(QStringLiteral("tools")).toArray()) {
        if (m_tools.size() >= McpLimits::kMaxTools) {
            emitLog(QStringLiteral("服务器工具数超过 %1，多余的没有列出来。")  // i18n-ignore
                        .arg(McpLimits::kMaxTools));
            return;
        }
        const QJsonObject tool = value.toObject();
        McpToolDescriptor descriptor;
        descriptor.serverId = m_server.id;
        descriptor.serverName = m_server.name;
        descriptor.name = tool.value(QStringLiteral("name")).toString();
        descriptor.description = tool.value(QStringLiteral("description")).toString();
        descriptor.inputSchema = tool.value(QStringLiteral("inputSchema")).toObject();
        if (descriptor.name.isEmpty()) continue;
        bool duplicate = false;
        for (const McpToolDescriptor &existing : m_tools) {
            if (existing.name == descriptor.name) duplicate = true;
        }
        if (duplicate) continue;
        m_tools.append(descriptor);
    }
}

void McpClient::sendToolsList() {
    if (m_process == nullptr) return;
    QJsonObject params;
    if (!m_listCursor.isEmpty()) params.insert(QStringLiteral("cursor"), m_listCursor);
    const QString id = QStringLiteral("list-%1").arg(m_nextRequestId++);
    startRequest(id, Pending::Kind::List,
                 McpProtocol::encodeRequest(id, QStringLiteral("tools/list"), params),
                 m_server.listTimeoutMs);
}

void McpClient::requestToolList() {
    if (m_state != State::Ready || m_process == nullptr) return;
    m_tools.clear();
    m_listCursor.clear();
    m_listPages = 0;
    setState(State::Listing, tr("工具清单已变更，正在重新获取…"));
    sendToolsList();
}

QString McpClient::callTool(const QString &toolName, const QJsonObject &arguments) {
    if (m_state != State::Ready || m_process == nullptr) return QString();
    if (m_pending.size() >= McpLimits::kMaxPendingRequests) {
        emitLog(QStringLiteral("MCP 同时进行的请求太多，已拒绝这一次调用。"));  // i18n-ignore
        return QString();
    }
    const QString id = QStringLiteral("call-%1").arg(m_nextRequestId++);
    const QJsonObject params{{QStringLiteral("name"), toolName},
                             {QStringLiteral("arguments"), arguments}};
    startRequest(id, Pending::Kind::Call,
                 McpProtocol::encodeRequest(id, QStringLiteral("tools/call"), params),
                 m_server.callTimeoutMs, toolName);
    return id;
}

void McpClient::cancelCall(const QString &requestId) {
    if (!m_pending.contains(requestId)) return;
    const Pending pending = m_pending.value(requestId);
    if (pending.kind != Pending::Kind::Call) return;
    if (m_process != nullptr) {
        m_process->write(McpProtocol::encodeNotification(
            QStringLiteral("notifications/cancelled"),
            QJsonObject{{QStringLiteral("requestId"), requestId},
                        {QStringLiteral("reason"), QStringLiteral("用户取消")}}));  // i18n-ignore
    }
    failPending(requestId, Agent::ErrorKind::Cancelled, tr("用户取消了本次 MCP 调用。"));
}

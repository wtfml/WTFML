/**
 * @file AgentCommandTools.cpp
 * @brief 固定诊断命令的执行与审批（见 .hpp）。
 */

#include "AgentCommandTools.hpp"

#include "AgentApprovalPolicy.hpp"
#include "AgentRedaction.hpp"
#include "AgentSettings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr int kMaxOutputBytes = 256 * 1024;

QJsonObject objectSchema(const QStringList &required, const QJsonObject &properties) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("required"), QJsonArray::fromStringList(required)},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject autoEgress() {
    return QJsonObject{{QStringLiteral("kind"), QStringLiteral("command")},
                       {QStringLiteral("decision"), QStringLiteral("confirm")},
                       {QStringLiteral("reason"), QStringLiteral("本机命令输出，按不可信内容处理")}};
}

}  // namespace

namespace AgentCommandSpecs {

QList<AgentCommandSpec> all() {
    QList<AgentCommandSpec> list;
    list.append(AgentCommandSpec{QStringLiteral("java-version"), QStringLiteral("java -version"),
                                 QStringLiteral("java"), {QStringLiteral("-version")}, false,
                                 20000,
                                 QStringLiteral("看本机默认 java 的版本与发行版。")});
    list.append(AgentCommandSpec{QStringLiteral("where-java"), QStringLiteral("where java"),
                                 QStringLiteral("where"), {QStringLiteral("java")}, false, 15000,
                                 QStringLiteral("列出 PATH 上能找到的 java.exe。")});
    list.append(AgentCommandSpec{QStringLiteral("ping"), QStringLiteral("ping 主机"),
                                 QStringLiteral("ping"),
                                 {QStringLiteral("-n"), QStringLiteral("4"), QStringLiteral("%1")},
                                 true, 30000, QStringLiteral("连发 4 个 ICMP 包。")});
    list.append(AgentCommandSpec{QStringLiteral("nslookup"), QStringLiteral("nslookup 主机"),
                                 QStringLiteral("nslookup"), {QStringLiteral("%1")}, true, 30000,
                                 QStringLiteral("查一次 DNS 解析。")});
    list.append(AgentCommandSpec{QStringLiteral("netstat"), QStringLiteral("netstat -ano"),
                                 QStringLiteral("netstat"), {QStringLiteral("-ano")}, false,
                                 30000, QStringLiteral("列出本机连接与 PID。")});
    list.append(AgentCommandSpec{QStringLiteral("ipconfig"), QStringLiteral("ipconfig /all"),
                                 QStringLiteral("ipconfig"), {QStringLiteral("/all")}, false,
                                 30000, QStringLiteral("本机网络配置（含适配器与 DNS）。")});
    return list;
}

AgentCommandSpec find(const QString &id) {
    for (const AgentCommandSpec &spec : all()) {
        if (spec.id == id) return spec;
    }
    return {};
}

bool validHostArgument(const QString &host) {
    if (host.isEmpty() || host.size() > 253) return false;
    // 只允许域名/IP 字面量字符；杜绝空白、引号、管道、重定向、分号等 shell 元字符。
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9.:_-]+$"));
    if (!pattern.match(host).hasMatch()) return false;
    if (host.startsWith(QLatin1Char('-')) || host.startsWith(QLatin1Char('.'))) return false;
    return true;
}

}  // namespace AgentCommandSpecs

AgentCommandTools::AgentCommandTools(ContextProvider provider, QObject *parent)
    : QObject(parent), m_provider(std::move(provider)) {}

QList<AgentToolSpec> AgentCommandTools::specs() {
    QStringList ids;
    for (const AgentCommandSpec &spec : AgentCommandSpecs::all()) ids.append(spec.id);
    return {AgentToolSpec{
        QStringLiteral("command.diagnose"), QCoreApplication::translate("AgentCommandTools", "诊断命令"),
        QStringLiteral("只执行固定诊断命令（java -version、ping、nslookup、netstat -ano 等）；"
                       "默认关闭，每次都要审批。"),
        AgentRisk::Command,
        objectSchema({QStringLiteral("command")},
                     {{QStringLiteral("command"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("enum"), QJsonArray::fromStringList(ids)}}},
                      {QStringLiteral("host"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 253}}}})}};
}

bool AgentCommandTools::isCommandTool(const QString &internalName) {
    return internalName == QStringLiteral("command.diagnose");
}

QJsonArray AgentCommandTools::schemas() const {
    QJsonArray array;
    for (const AgentToolSpec &spec : specs()) {
        array.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), AgentToolRegistry::wireName(spec.name)},
                         {QStringLiteral("description"), spec.description},
                         {QStringLiteral("parameters"), spec.schema}}}});
    }
    return array;
}

bool AgentCommandTools::handles(const QString &internalName) const {
    return isCommandTool(internalName);
}

AgentToolResult AgentCommandTools::errorResult(Agent::ErrorKind kind,
                                               const QString &message) const {
    AgentToolResult result;
    result.status = Agent::ErrorKind::EgressBlocked == kind ? AgentToolResult::Status::Blocked
                                                             : AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    return result;
}

AgentToolResult AgentCommandTools::invoke(const Agent::ToolCall &call) {
    const AgentToolSpec toolSpec = AgentToolRegistry::find(QStringLiteral("command.diagnose"));
    QString problem;
    if (!AgentToolRegistry::validateArguments(toolSpec.schema, call.arguments, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (!AgentSettings::diagnoseCommandsEnabled()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("诊断命令默认关闭：先到 API 设置里打开「允许诊断命令」。"));
    }
    const QString commandId = call.arguments.value(QStringLiteral("command")).toString();
    const AgentCommandSpec spec = AgentCommandSpecs::find(commandId);
    if (spec.id.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("没有这条诊断命令。"));
    }
    const QString host = call.arguments.value(QStringLiteral("host")).toString();
    if (spec.needsHost && !AgentCommandSpecs::validHostArgument(host)) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("host 只接受域名或 IP 字面量。"));
    }
    if (m_process != nullptr) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("已有一条诊断命令在跑，等它结束。"));
    }
    const QString programPath = QStandardPaths::findExecutable(spec.program);
    if (programPath.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError,
                           QStringLiteral("找不到 %1（不在 PATH 里）。").arg(spec.program));
    }
    QStringList arguments;
    for (const QString &argument : spec.fixedArgs) {
        arguments.append(argument == QStringLiteral("%1") ? host : argument);
    }
    AgentToolContext context;
    if (m_provider != nullptr) {
        QString contextProblem;
        context = m_provider(&contextProblem);
    }
    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Approval approval;
    approval.spec = spec;
    approval.host = host;
    approval.programPath = programPath;
    approval.arguments = arguments;
    approval.call = call;
    approval.context = context;
    m_approvals.insert(approvalId, approval);

    const QString commandLine =
        QStringLiteral("%1 %2").arg(QDir::toNativeSeparators(programPath),
                                    arguments.join(QLatin1Char(' ')));
    const QString summary = QStringLiteral("执行诊断命令：%1").arg(spec.displayName);
    QJsonObject request = AgentApprovalPolicy::request(
        call.name, QStringLiteral("诊断命令"), spec.id, summary, commandLine, QString(),
        QStringLiteral("进程树随超时/取消清理；输出上限 256 KB，结果按不可信内容处理。"),
        autoEgress());
    request.insert(QStringLiteral("approvalId"), approvalId);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = summary;
    result.approval = request;
    return result;
}

AgentToolResult AgentCommandTools::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_approvals.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("审批请求已失效。"));
    }
    const Approval approval = m_approvals.take(approvalId);
    if (!approved) {
        AgentToolResult result =
            errorResult(Agent::ErrorKind::ApprovalDenied, QStringLiteral("用户拒绝了这次命令。"));
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    const QString requestId = QStringLiteral("cmd-%1").arg(m_counter++);
    m_running.insert(requestId, approval.call);
    m_currentId = requestId;
    m_output.clear();
    m_truncated = false;

    m_process = new QProcess(this);
    m_process->setProgram(approval.programPath);
    m_process->setArguments(approval.arguments);
    // Windows 上 `where` / `ping` 都在 PATH 上；工作目录用 agent 数据目录，不落进实例目录。
    m_process->setWorkingDirectory(QDir::currentPath());
    m_process->setProcessChannelMode(QProcess::MergedChannels);

#if defined(Q_OS_WIN)
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        m_jobHandle = qint64(job);
    }
#endif

    connect(m_process, &QProcess::started, this, [this] {
#if defined(Q_OS_WIN)
        if (m_jobHandle != 0) {
            HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                         DWORD(m_process->processId()));
            if (process != nullptr) {
                AssignProcessToJobObject(HANDLE(m_jobHandle), process);
                CloseHandle(process);
            }
        }
#endif
    });
    connect(m_process, &QProcess::readyRead, this, [this] {
        if (m_process == nullptr) return;
        const QByteArray chunk = m_process->readAll();
        if (m_output.size() + chunk.size() > kMaxOutputBytes) {
            const int room = kMaxOutputBytes - m_output.size();
            if (room > 0) m_output += chunk.left(room);
            m_truncated = true;
            return;
        }
        m_output += chunk;
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
                Q_UNUSED(status);
                if (m_process == nullptr) return;
                m_output += m_process->readAll();
                killTree();
                m_process->disconnect(this);
                m_process->deleteLater();
                m_process = nullptr;
                const QString text = QString::fromUtf8(m_output);
                const AgentRedaction::Result redacted = AgentRedaction::redact(text);
                AgentToolResult result;
                result.status = exitCode == 0 ? AgentToolResult::Status::Ok
                                              : AgentToolResult::Status::Error;
                result.summary = QStringLiteral("诊断命令结束（退出码 %1）").arg(exitCode);
                if (exitCode != 0) {
                    result.error.kind = Agent::ErrorKind::ToolError;
                    result.error.message = result.summary;
                }
                result.data = QJsonObject{{QStringLiteral("exitCode"), exitCode},
                                          {QStringLiteral("output"), redacted.text},
                                          {QStringLiteral("truncated"), m_truncated},
                                          {QStringLiteral("untrusted"), true},
                                          {QStringLiteral("egress"), autoEgress()}};
                finish(m_currentId, result);
            });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_process == nullptr) return;
        killTree();
        m_process->deleteLater();
        m_process = nullptr;
        finish(m_currentId,
               errorResult(Agent::ErrorKind::ToolError, QStringLiteral("命令起不来。")));
    });

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(approval.spec.timeoutMs);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (m_process != nullptr) {
            killTree();
            m_process->disconnect(this);
            m_process->kill();
            m_process->deleteLater();
            m_process = nullptr;
        }
        finish(m_currentId,
               errorResult(Agent::ErrorKind::Cancelled, QStringLiteral("命令超时，已清理进程树。")));
    });
    m_timer->start();
    m_process->start();

    AgentToolResult pending;
    pending.status = AgentToolResult::Status::Pending;
    pending.summary = QStringLiteral("诊断命令执行中…");
    return pending;
}

void AgentCommandTools::killTree() {
    if (m_timer != nullptr) {
        m_timer->stop();
        m_timer->deleteLater();
        m_timer = nullptr;
    }
#if defined(Q_OS_WIN)
    if (m_jobHandle != 0) {
        TerminateJobObject(HANDLE(m_jobHandle), 1);
        CloseHandle(HANDLE(m_jobHandle));
        m_jobHandle = 0;
    }
#endif
}

void AgentCommandTools::finish(const QString &requestId, const AgentToolResult &result) {
    if (!m_running.contains(requestId)) return;
    const Agent::ToolCall call = m_running.take(requestId);
    if (m_currentId == requestId) m_currentId.clear();
    if (m_sink) m_sink(call, result);
}

void AgentCommandTools::cancelPending() {
    m_approvals.clear();
    if (m_process != nullptr) {
        killTree();
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
    const QStringList ids = m_running.keys();
    for (const QString &id : ids) {
        const Agent::ToolCall call = m_running.take(id);
        AgentToolResult result =
            errorResult(Agent::ErrorKind::Cancelled, QStringLiteral("诊断命令已取消。"));
        if (m_sink) m_sink(call, result);
    }
}

/**
 * @file AgentTestLaunchToolEngine.cpp
 * @brief `instance.testLaunch` 的参数、审批与异步交付（见 .hpp）。
 */

#include "AgentTestLaunchToolEngine.hpp"

#include "AgentApprovalPolicy.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QUuid>

namespace {

constexpr int kDefaultTimeoutSeconds = 120;
constexpr int kDefaultSettleSeconds = 10;
constexpr int kMaxTimeoutSeconds = 600;
constexpr int kMaxSettleSeconds = 60;
constexpr int kMaxRepeat = 3;

QJsonObject stringField(const QString &type, int maxLength) {
    return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("maxLength"), maxLength}};
}

QJsonObject integerField(int maximum) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                       {QStringLiteral("maximum"), maximum}};
}

QJsonObject enumField(const QStringList &values) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("enum"), QJsonArray::fromStringList(values)}};
}

QJsonObject objectSchema(const QJsonObject &properties) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

AgentToolSpec spec() {
    const QJsonObject quickPlay = objectSchema(
        {{QStringLiteral("world"), stringField(QStringLiteral("string"), 128)}});
    const QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("workspaceId"), stringField(QStringLiteral("string"), 80)},
             {QStringLiteral("timeoutSeconds"), integerField(kMaxTimeoutSeconds)},
             {QStringLiteral("settleSeconds"), integerField(kMaxSettleSeconds)},
             {QStringLiteral("milestone"),
              enumField({QStringLiteral("process_started"), QStringLiteral("main_menu"),
                         QStringLiteral("world_loaded")})},
             {QStringLiteral("repeat"), integerField(kMaxRepeat)},
             {QStringLiteral("quickPlay"), quickPlay},
             {QStringLiteral("captureLogs"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
             {QStringLiteral("expect"),
              enumField({QStringLiteral("stable"), QStringLiteral("crash")})},
         }},
        {QStringLiteral("additionalProperties"), false}};
    return AgentToolSpec{QStringLiteral("instance.testLaunch"),
                         QCoreApplication::translate("AgentTestLaunchToolEngine", "测试启动"),
                         QStringLiteral("在隔离工作区用启动器自己的启动流程跑一次游戏，"  // i18n-ignore
                                        "等待主菜单/世界/崩溃或超时；不改原实例。"),  // i18n-ignore
                         AgentRisk::Command, schema};
}

QJsonObject localEgress() {
    return QJsonObject{{QStringLiteral("kind"), QStringLiteral("local")},
                       {QStringLiteral("decision"), QStringLiteral("auto")},
                       {QStringLiteral("reason"), QStringLiteral("测试启动只在本机工作区，不发送文件内容")},  // i18n-ignore
                       {QStringLiteral("redactionHits"), 0}};
}

bool safeRelativeName(const QString &value) {
    return !value.isEmpty() && !value.contains(QLatin1Char('/')) &&
           !value.contains(QLatin1Char('\\')) && value != QStringLiteral(".") &&
           value != QStringLiteral("..");
}

}  // namespace

AgentTestLaunchToolEngine::AgentTestLaunchToolEngine(AgentLaunchHost *host,
                                                     ContextProvider provider)
    : m_host(host), m_provider(std::move(provider)) {}

QList<AgentToolSpec> AgentTestLaunchToolEngine::specs() {
    return {spec()};
}

bool AgentTestLaunchToolEngine::parseOptions(const QJsonObject &arguments,
                                             AgentTestLaunchOptions *options,
                                             QString *problem) {
    if (options == nullptr) return false;
    *options = AgentTestLaunchOptions{};

    const QJsonValue timeoutValue = arguments.value(QStringLiteral("timeoutSeconds"));
    if (!timeoutValue.isUndefined()) {
        if (!timeoutValue.isDouble()) {
            if (problem != nullptr) *problem = QStringLiteral("timeoutSeconds 必须是整数。");  // i18n-ignore
            return false;
        }
        options->timeoutSeconds = timeoutValue.toInt();
        if (options->timeoutSeconds < 1 || options->timeoutSeconds > kMaxTimeoutSeconds) {
            if (problem != nullptr) *problem = QStringLiteral("timeoutSeconds 必须在 1–600 之间。");  // i18n-ignore
            return false;
        }
    } else {
        options->timeoutSeconds = kDefaultTimeoutSeconds;
    }

    const QJsonValue settleValue = arguments.value(QStringLiteral("settleSeconds"));
    if (!settleValue.isUndefined()) {
        if (!settleValue.isDouble()) {
            if (problem != nullptr) *problem = QStringLiteral("settleSeconds 必须是整数。");  // i18n-ignore
            return false;
        }
        options->settleSeconds = settleValue.toInt();
        if (options->settleSeconds < 0 || options->settleSeconds > kMaxSettleSeconds) {
            if (problem != nullptr) *problem = QStringLiteral("settleSeconds 必须在 0–60 之间。");  // i18n-ignore
            return false;
        }
    } else {
        options->settleSeconds = kDefaultSettleSeconds;
    }

    const QJsonValue repeatValue = arguments.value(QStringLiteral("repeat"));
    if (!repeatValue.isUndefined()) {
        if (!repeatValue.isDouble()) {
            if (problem != nullptr) *problem = QStringLiteral("repeat 必须是整数。");  // i18n-ignore
            return false;
        }
        options->repeat = repeatValue.toInt();
        if (options->repeat < 1 || options->repeat > kMaxRepeat) {
            if (problem != nullptr) *problem = QStringLiteral("repeat 必须在 1–3 之间。");  // i18n-ignore
            return false;
        }
    } else {
        options->repeat = 1;
    }

    const QString milestone = arguments.value(QStringLiteral("milestone")).toString();
    if (!milestone.isEmpty() && !AgentTestLaunchMilestones::validMilestone(milestone)) {
        if (problem != nullptr) *problem = QStringLiteral("milestone 只支持 process_started/main_menu/world_loaded。");  // i18n-ignore
        return false;
    }
    options->milestone = AgentTestLaunchMilestones::normalizeMilestone(milestone);

    const QString expect = arguments.value(QStringLiteral("expect")).toString();
    if (!expect.isEmpty() && expect != QStringLiteral("stable") && expect != QStringLiteral("crash")) {
        if (problem != nullptr) *problem = QStringLiteral("expect 只支持 stable/crash。");  // i18n-ignore
        return false;
    }
    options->expectCrash = expect == QStringLiteral("crash");

    const QJsonValue capture = arguments.value(QStringLiteral("captureLogs"));
    if (!capture.isUndefined()) {
        if (!capture.isBool()) {
            if (problem != nullptr) *problem = QStringLiteral("captureLogs 必须是布尔值。");  // i18n-ignore
            return false;
        }
        options->captureLogs = capture.toBool();
    }

    const QJsonValue quickPlay = arguments.value(QStringLiteral("quickPlay"));
    if (!quickPlay.isUndefined() && !quickPlay.isNull()) {
        if (!quickPlay.isObject()) {
            if (problem != nullptr) *problem = QStringLiteral("quickPlay 必须是对象。");  // i18n-ignore
            return false;
        }
        const QJsonObject object = quickPlay.toObject();
        const QString world = object.value(QStringLiteral("world")).toString();
        if (!world.isEmpty()) {
            if (!safeRelativeName(world)) {
                if (problem != nullptr) *problem = QStringLiteral("quickPlay.world 只能是世界名，不能带路径。");  // i18n-ignore
                return false;
            }
            options->quickPlayWorld = world;
        }
    }

    if (options->milestone == QStringLiteral("world_loaded") && options->quickPlayWorld.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("milestone=world_loaded 需要 quickPlay.world。");  // i18n-ignore
        return false;
    }
    return true;
}

QJsonObject AgentTestLaunchToolEngine::summaryJson(const AgentTestLaunchSummary &summary) {
    QJsonArray runs;
    QJsonArray evidence;
    int evidenceIndex = 1;
    for (const AgentTestLaunchRunResult &run : summary.runs) {
        QJsonArray refs;
        for (const QString &ref : run.logRefs) {
            refs.append(ref);
            evidence.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("E%1").arg(evidenceIndex++)},
                {QStringLiteral("kind"), QStringLiteral("test-launch")},
                {QStringLiteral("source"), ref},
                {QStringLiteral("capturedAt"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                {QStringLiteral("excerpt"), run.detail.left(400)},
            });
        }
        runs.append(QJsonObject{
            {QStringLiteral("processStarted"), run.processStarted},
            {QStringLiteral("exitCode"), run.exitCode},
            {QStringLiteral("startupPhase"), run.startupPhase},
            {QStringLiteral("milestoneReached"), run.milestoneReached},
            {QStringLiteral("crashed"), run.crashed},
            {QStringLiteral("timedOut"), run.timedOut},
            {QStringLiteral("newCrashReport"), run.newCrashReport},
            {QStringLiteral("stoppedAfterSettle"), run.stoppedAfterSettle},
            {QStringLiteral("durationMs"), run.durationMs},
            {QStringLiteral("detail"), run.detail},
            {QStringLiteral("logRefs"), refs},
        });
    }
    return QJsonObject{
        {QStringLiteral("ok"), summary.ok},
        {QStringLiteral("launchId"), summary.launchId},
        {QStringLiteral("workspaceId"), summary.workspaceId},
        {QStringLiteral("workspaceKept"), summary.workspaceKept},
        {QStringLiteral("problem"), summary.problem},
        {QStringLiteral("runs"), runs},
        {QStringLiteral("evidence"), evidence},
    };
}

QJsonArray AgentTestLaunchToolEngine::schemas() const {
    QJsonArray result;
    for (const AgentToolSpec &item : specs()) {
        result.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), AgentToolRegistry::wireName(item.name)},
                         {QStringLiteral("description"), item.description},
                         {QStringLiteral("parameters"), item.schema}}}});
    }
    return result;
}

bool AgentTestLaunchToolEngine::handles(const QString &internalName) const {
    return internalName == QStringLiteral("instance.testLaunch");
}

AgentToolResult AgentTestLaunchToolEngine::errorResult(Agent::ErrorKind kind,
                                                       const QString &message) const {
    AgentToolResult result;
    result.status = AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    return result;
}

AgentToolResult AgentTestLaunchToolEngine::invoke(const Agent::ToolCall &call) {
    const QString internal = AgentToolRegistry::internalName(call.name);
    if (!handles(internal)) return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("未知工具。"));  // i18n-ignore
    if (m_active) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("已有一个测试启动在运行。"));  // i18n-ignore
    }
    const AgentToolSpec toolSpec = specs().constFirst();
    QString problem;
    if (!AgentToolRegistry::validateArguments(toolSpec.schema, call.arguments, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    AgentTestLaunchOptions options;
    if (!parseOptions(call.arguments, &options, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (m_host == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("测试启动宿主未接入。"));  // i18n-ignore
    }
    QString contextProblem;
    const AgentToolContext context =
        m_provider != nullptr ? m_provider(&contextProblem) : AgentToolContext{};
    if (!contextProblem.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, contextProblem);
    }
    if (context.instanceId.isEmpty()) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("会话还没有锁定实例。"));  // i18n-ignore
    }
    const QString workspaceId = call.arguments.value(QStringLiteral("workspaceId")).toString();
    if (!workspaceId.isEmpty() &&
        (workspaceId.size() > 80 || workspaceId.contains(QLatin1Char('/')) ||
         workspaceId.contains(QLatin1Char('\\')))) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("workspaceId 不合法。"));  // i18n-ignore
    }

    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Pending pending;
    pending.call = call;
    pending.context = context;
    pending.options = options;
    pending.workspaceId = workspaceId;
    m_pending.insert(approvalId, pending);

    const QString summary =
        QStringLiteral("在隔离工作区启动「%1」：最多 %2 秒，里程碑 %3，重复 %4 次。")  // i18n-ignore
            .arg(context.instanceName.isEmpty() ? context.instanceId : context.instanceName)
            .arg(options.timeoutSeconds)
            .arg(options.milestone)
            .arg(options.repeat);
    const QString preview =
        QStringLiteral("工作区：%1；观察窗口 %2 秒；期望：%3；%4")  // i18n-ignore
            .arg(workspaceId.isEmpty() ? QStringLiteral("新建临时工作区") : workspaceId)  // i18n-ignore
            .arg(options.settleSeconds)
            .arg(options.expectCrash ? QStringLiteral("复现崩溃") : QStringLiteral("稳定到里程碑"))  // i18n-ignore
            .arg(options.quickPlayWorld.isEmpty() ? QStringLiteral("不进世界")  // i18n-ignore
                                                  : QStringLiteral("进入副本世界 %1").arg(options.quickPlayWorld));  // i18n-ignore
    QJsonObject request = AgentApprovalPolicy::request(
        internal, QStringLiteral("测试启动"), pending.context.instanceName, summary, preview,  // i18n-ignore
        QString(), QStringLiteral("只写隔离工作区；原实例一个字节不改。"), localEgress());  // i18n-ignore
    request.insert(QStringLiteral("approvalId"), approvalId);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = summary;
    result.approval = request;
    return result;
}

AgentToolResult AgentTestLaunchToolEngine::resolveApproval(const QString &approvalId,
                                                           bool approved) {
    if (!m_pending.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("审批请求已失效。"));  // i18n-ignore
    }
    const Pending pending = m_pending.take(approvalId);
    if (!approved) {
        AgentToolResult result = errorResult(
            Agent::ErrorKind::ApprovalDenied, QStringLiteral("用户拒绝了本次测试启动；未启动进程。"));  // i18n-ignore
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    if (m_active) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("已有一个测试启动在运行。"));  // i18n-ignore
    }
    if (m_host == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("测试启动宿主未接入。"));  // i18n-ignore
    }

    m_active = true;
    m_activeCall = pending.call;
    const Agent::ToolCall activeCall = pending.call;
    QString problem;
    const bool started = m_host->startTestLaunch(
        pending.context.instanceId, pending.workspaceId, pending.options,
        [this, activeCall](const AgentTestLaunchSummary &summary) {
            if (!m_active) return;  // 已取消/换代  // i18n-ignore
            m_active = false;
            m_activeCall = {};
            AgentToolResult out;
            out.data = summaryJson(summary);
            out.status = summary.ok ? AgentToolResult::Status::Ok : AgentToolResult::Status::Error;
            out.summary = summary.ok
                              ? QStringLiteral("测试启动达到目标（%1）。").arg(summary.launchId)  // i18n-ignore
                              : (summary.problem.isEmpty()
                                     ? QStringLiteral("测试启动没有达到目标（%1）。").arg(summary.launchId)  // i18n-ignore
                                     : summary.problem);
            if (!summary.ok) {
                out.error.kind = Agent::ErrorKind::ToolError;
                out.error.message = out.summary;
            }
            if (m_sink) m_sink(activeCall, out);
        },
        &problem);
    if (!started) {
        m_active = false;
        m_activeCall = {};
        return errorResult(Agent::ErrorKind::ToolError,
                           problem.isEmpty() ? QStringLiteral("测试启动没有开始。") : problem);  // i18n-ignore
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Pending;
    result.summary = QStringLiteral("测试启动已开始，等待里程碑或超时…");  // i18n-ignore
    return result;
}

void AgentTestLaunchToolEngine::cancelPending() {
    m_pending.clear();
    if (m_host != nullptr) m_host->cancelTestLaunch();
    const Agent::ToolCall call = m_activeCall;
    m_active = false;
    m_activeCall = {};
    if (m_sink && !call.name.isEmpty()) {
        AgentToolResult out = errorResult(Agent::ErrorKind::Cancelled,
                                          QStringLiteral("这次测试启动已取消。"));  // i18n-ignore
        m_sink(call, out);
    }
}

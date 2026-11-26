#include "controllers/AgentTaskLedger.hpp"

#include "agent/AgentRepairTransaction.hpp"
#include "agent/AgentWorkflow.hpp"
#include "tasks/TaskManager.hpp"

namespace {
QString tool(const Agent::ToolCall &call) { return AgentToolRegistry::internalName(call.name); }
QString arg(const Agent::ToolCall &call, const char *key) {
    return call.arguments.value(QLatin1String(key)).toString();
}
}  // namespace

AgentTaskLedger::AgentTaskLedger(AgentWorkflow *workflow, QObject *parent)
    : QObject(parent), m_workflow(workflow) {}

void AgentTaskLedger::attach(AgentLoop *loop) {
    connect(loop, &AgentLoop::toolCallStarted, this, &AgentTaskLedger::onToolStarted);
    connect(loop, &AgentLoop::approvalRequested, this,
            [this](const QString &, const Agent::ToolCall &call, const QJsonObject &) {
                onApprovalRequested(call);
            });
    connect(loop, &AgentLoop::toolCallFinished, this, &AgentTaskLedger::onToolFinished);
    connect(loop, &AgentLoop::stateChanged, this, &AgentTaskLedger::onLoopState);
    connect(loop, &AgentLoop::turnFailed, this, &AgentTaskLedger::onTurnFailed);
}

AgentTaskLedger::Policy AgentTaskLedger::policyFor(const QString &name) {
    static const QStringList longTools{
        QStringLiteral("instance.integrity"),   QStringLiteral("mods.verifyFiles"),
        QStringLiteral("mods.analyzeCompatibility"), QStringLiteral("backup.create"),
        QStringLiteral("backup.restore"),       QStringLiteral("mods.installFix"),
        QStringLiteral("instance.testLaunch"),  QStringLiteral("mods.bisect")};
    static const QStringList quickWrites{QStringLiteral("fs.write"), QStringLiteral("mods.setEnabled"),
                                         QStringLiteral("settings.set")};
    if (longTools.contains(name)) return Policy::Long;
    if (quickWrites.contains(name)) return Policy::QuickWrite;
    return Policy::None;
}

bool AgentTaskLedger::isPlanStep(const QString &name) const {
    return m_workflow != nullptr && m_workflow->requiresPlan() && m_workflow->planApproved() &&
           (m_workflow->stage() == AgentWorkflowStage::Execute ||
            m_workflow->stage() == AgentWorkflowStage::Verify) &&
           m_workflow->stepAllows(name);
}

Tasks::TaskKind AgentTaskLedger::kindFor(const QString &name) const {
    if (name == QStringLiteral("instance.integrity") || name == QStringLiteral("mods.verifyFiles"))
        return Tasks::TaskKind::Integrity;
    if (name.startsWith(QStringLiteral("backup."))) return Tasks::TaskKind::Backup;
    if (name == QStringLiteral("mods.installFix")) return Tasks::TaskKind::Download;
    // 测试启动不记成 Launch：那一类的取消/会话号/崩溃卡片都按真启动走。
    return Tasks::TaskKind::Other;
}

QString AgentTaskLedger::titleFor(const Agent::ToolCall &call, const QString &name) const {
    const Context ctx = context();
    QString instance = ctx.instanceName.isEmpty() ? ctx.instanceId : ctx.instanceName;
    if (instance.isEmpty()) instance = arg(call, "instanceId");
    if (name == QStringLiteral("instance.integrity")) return tr("智能体：检查「%1」的文件").arg(instance);
    if (name == QStringLiteral("mods.verifyFiles")) return tr("智能体：校验「%1」的模组文件").arg(instance);
    if (name == QStringLiteral("mods.analyzeCompatibility")) return tr("智能体：分析「%1」的模组兼容性").arg(instance);
    if (name == QStringLiteral("backup.create")) return tr("智能体：备份「%1」").arg(instance);
    if (name == QStringLiteral("backup.restore")) return tr("智能体：还原「%1」的备份").arg(instance);
    if (name == QStringLiteral("fs.write")) return tr("智能体：修改「%1」中的 %2").arg(instance, arg(call, "path"));
    if (name == QStringLiteral("mods.setEnabled")) {
        return call.arguments.value(QStringLiteral("enabled")).toBool()
                   ? tr("智能体：启用模组 %1").arg(arg(call, "fileName"))
                   : tr("智能体：禁用模组 %1").arg(arg(call, "fileName"));
    }
    if (name == QStringLiteral("settings.set")) return tr("智能体：修改启动器设置");
    if (name == QStringLiteral("mods.installFix")) return tr("智能体：下载修复模组");
    if (name == QStringLiteral("instance.testLaunch")) return tr("智能体：测试启动「%1」").arg(instance);
    if (name == QStringLiteral("mods.bisect")) return tr("智能体：二分定位「%1」的问题模组").arg(instance);
    return name;
}

Tasks::TaskId AgentTaskLedger::beginTask(Tasks::TaskKind kind, const QString &title,
                                         const QList<Tasks::TaskStep> &steps) {
    if (m_tasks == nullptr) return Tasks::invalidTaskId();
    const Context ctx = context();
    Tasks::TaskCapabilities caps;
    caps.canCancel = true;
    caps.canOpenAgentSession = !ctx.sessionId.isEmpty();
    const Tasks::TaskId id = m_tasks->begin(kind, title, ctx.instanceId, steps, caps);
    m_sessions.insert(id, ctx.sessionId);
    return id;
}

bool AgentTaskLedger::isActive(Tasks::TaskId id) const {
    return m_tasks != nullptr && owns(id) && m_tasks->has(id) &&
           Tasks::isActiveState(m_tasks->snapshot(id).state);
}

void AgentTaskLedger::setWaiting(Tasks::TaskId id, bool waiting, const QString &detail) {
    if (!isActive(id)) return;
    m_tasks->setWaitingUser(id, waiting);
    m_tasks->setDetail(id, detail);
}

void AgentTaskLedger::setCancelBlocked(Tasks::TaskId id, const QString &reason) {
    if (!isActive(id)) return;
    Tasks::TaskCapabilities caps = m_tasks->snapshot(id).capabilities;
    caps.canCancel = reason.isEmpty();
    caps.cancelBlockedReason = reason;
    m_tasks->setCapabilities(id, caps);
}

void AgentTaskLedger::cancelWith(Tasks::TaskId *id, const QString &detail) {
    if (m_tasks != nullptr && *id != Tasks::invalidTaskId()) {
        if (isActive(*id)) m_tasks->setDetail(*id, detail);
        m_tasks->cancel(*id);
    }
    *id = Tasks::invalidTaskId();
}

void AgentTaskLedger::ensurePlanTask() {
    if (m_tasks == nullptr || isActive(m_planTask) || m_workflow == nullptr ||
        !m_workflow->planApproved() || m_workflow->stage() == AgentWorkflowStage::Failed ||
        m_workflow->stage() == AgentWorkflowStage::Report) {
        return;
    }
    QList<Tasks::TaskStep> steps;
    for (const AgentPlanStep &step : m_workflow->planSteps()) {
        Tasks::TaskStep row;
        row.name = step.reason.trimmed().isEmpty() ? step.tool : step.reason.trimmed().left(60);
        steps.append(row);
    }
    const Context ctx = context();
    const QString instance = ctx.instanceName.isEmpty() ? ctx.instanceId : ctx.instanceName;
    m_planTask = beginTask(Tasks::TaskKind::Other, tr("智能体修复计划：%1").arg(instance), steps);
    m_tasks->setDetail(m_planTask, m_workflow->planGoal());
    syncPlan();
}

void AgentTaskLedger::syncPlan() {
    if (!isActive(m_planTask) || m_workflow == nullptr) return;
    if (!m_workflow->planApproved()) {
        cancelWith(&m_planTask, tr("计划已作废。"));
        return;
    }
    const int total = m_tasks->snapshot(m_planTask).steps.size();
    const int next = m_workflow->nextStepIndex();
    for (int i = 0; i < qMin(next, total); ++i)
        m_tasks->setStepState(m_planTask, i, Tasks::TaskStepState::Done);
    if (m_workflow->stage() == AgentWorkflowStage::Failed) {
        if (next < total)
            m_tasks->setStepState(m_planTask, next, Tasks::TaskStepState::Failed, m_workflow->failureReason());
        m_tasks->fail(m_planTask, m_workflow->failureReason());
        m_planTask = Tasks::invalidTaskId();
    } else if (m_workflow->stage() == AgentWorkflowStage::Report) {
        m_tasks->succeed(m_planTask);
        m_planTask = Tasks::invalidTaskId();
    }
}

void AgentTaskLedger::onToolStarted(const Agent::ToolCall &call) {
    if (m_tasks == nullptr) return;
    const QString name = tool(call);
    m_awaitingApproval = false;
    m_rejected = false;
    const bool planStep = isPlanStep(name);
    if (planStep) {
        ensurePlanTask();
        setWaiting(m_planTask, false, m_workflow->planGoal());
        if (isActive(m_planTask))
            m_tasks->setStepState(m_planTask, m_workflow->nextStepIndex(), Tasks::TaskStepState::Running);
    }
    if (name == QStringLiteral("repair.finish")) {
        m_committing = true;
        setCancelBlocked(m_planTask, tr("正在写回实例文件，完成后才能取消。"));
    }
    if (!planStep && policyFor(name) == Policy::Long) {
        m_standalone = beginTask(kindFor(name), titleFor(call, name), {});
        m_standaloneCall = call.id;
    }
}

void AgentTaskLedger::onApprovalRequested(const Agent::ToolCall &call) {
    if (m_tasks == nullptr) return;
    m_awaitingApproval = true;
    const QString name = tool(call);
    if (name == QStringLiteral("workflow.submitPlan")) return;  // 计划卡本身：批准后才有计划任务
    if (m_standalone == Tasks::invalidTaskId() && !isPlanStep(name) && policyFor(name) == Policy::QuickWrite) {
        m_standalone = beginTask(Tasks::TaskKind::Other, titleFor(call, name), {});
        m_standaloneCall = call.id;
    }
    if (isActive(m_standalone) && m_standaloneCall == call.id) {
        setWaiting(m_standalone, true, tr("等待确认"));
    } else if (isPlanStep(name) && isActive(m_planTask)) {
        setWaiting(m_planTask, true, tr("等待确认"));
    }
}

void AgentTaskLedger::onApprovalResolved(bool approved) {
    if (!m_awaitingApproval || m_tasks == nullptr) return;
    m_awaitingApproval = false;
    const bool planWaiting = isActive(m_planTask) &&
                             m_tasks->snapshot(m_planTask).state == Tasks::TaskState::WaitingUser;
    if (approved) {
        if (isActive(m_standalone)) setWaiting(m_standalone, false, QString());
        if (planWaiting) setWaiting(m_planTask, false, m_workflow->planGoal());
        return;
    }
    m_rejected = true;
    if (isActive(m_standalone)) {
        cancelWith(&m_standalone, tr("已拒绝，未修改任何内容。"));
    } else if (planWaiting) {
        const int step = m_workflow->nextStepIndex();
        m_tasks->setStepState(m_planTask, step, Tasks::TaskStepState::Failed);
        cancelWith(&m_planTask, tr("已拒绝第 %1 步，计划未继续。").arg(step + 1));
    }
}

void AgentTaskLedger::onToolFinished(const Agent::ToolCall &call, const AgentToolResult &result) {
    if (m_tasks == nullptr) return;
    const QString name = tool(call);
    if (name == QStringLiteral("repair.finish")) {
        m_committing = false;
        setCancelBlocked(m_planTask, QString());
    }
    if (isActive(m_standalone) && m_standaloneCall == call.id && !m_rejected) {
        if (result.status == AgentToolResult::Status::Ok) {
            m_tasks->setDetail(m_standalone, result.summary);
            m_tasks->succeed(m_standalone);
        } else {
            m_tasks->fail(m_standalone, result.summary);
        }
    }
    if (m_standaloneCall == call.id) {
        m_standalone = Tasks::invalidTaskId();
        m_standaloneCall.clear();
    }
    m_rejected = false;
    if (name == QStringLiteral("workflow.submitPlan") && result.status == AgentToolResult::Status::Ok) {
        ensurePlanTask();
    }
    syncPlan();
}

void AgentTaskLedger::onLoopState(AgentLoop::State state) {
    switch (state) {
    case AgentLoop::State::Preparing:
    case AgentLoop::State::Requesting:
    case AgentLoop::State::ExecutingTool:
        if (!m_awaitingApproval && isActive(m_planTask) &&
            m_tasks->snapshot(m_planTask).state == Tasks::TaskState::WaitingUser) {
            setWaiting(m_planTask, false, m_workflow->planGoal());
        }
        break;
    case AgentLoop::State::Completed:
        syncPlan();
        if (isActive(m_planTask)) setWaiting(m_planTask, true, tr("等待在会话中继续"));
        break;
    case AgentLoop::State::Cancelled:
        m_awaitingApproval = false;
        m_committing = false;
        cancelWith(&m_standalone, tr("已离开智能体页或已停止，操作未继续。"));
        cancelWith(&m_planTask, tr("已离开智能体页或已停止，操作未继续。"));
        break;
    case AgentLoop::State::Idle:
    case AgentLoop::State::AwaitingApproval:
    case AgentLoop::State::Failed:
        break;  // 失败那一刻的原因由 turnFailed 给
    }
}

void AgentTaskLedger::onTurnFailed(const Agent::Error &error) {
    if (m_tasks == nullptr) return;
    m_awaitingApproval = false;
    if (isActive(m_standalone)) m_tasks->fail(m_standalone, error.message);
    m_standalone = Tasks::invalidTaskId();
    syncPlan();
    if (isActive(m_planTask)) setWaiting(m_planTask, true, tr("等待在会话中继续"));
}

void AgentTaskLedger::onWorkflowReset() {
    cancelWith(&m_standalone, tr("计划已作废。"));
    cancelWith(&m_planTask, tr("计划已作废。"));
    m_awaitingApproval = false;
}

bool AgentTaskLedger::canCancel(Tasks::TaskId id) const {
    if (!isActive(id) || (id != m_planTask && id != m_standalone)) return false;
    return !(m_committing && id == m_planTask);
}

bool AgentTaskLedger::cancelFromTaskCenter(Tasks::TaskId id) {
    if (!canCancel(id)) return false;
    if (id == m_planTask) {
        const Context ctx = context();
        AgentRepairTransactionStore transactions;
        // 定案 9：任务中心取消不回滚未提交的修复事务，只把这件事说清楚。
        const bool pendingTransaction =
            !ctx.instanceId.isEmpty() && transactions.activeForInstance(ctx.instanceId, nullptr, nullptr);
        m_workflow->markFailed(tr("已在任务中心取消。"));
        cancelWith(&m_planTask, pendingTransaction ? tr("修复事务未提交，原实例未改动。") : tr("已在任务中心取消。"));
    } else {
        cancelWith(&m_standalone, tr("已在任务中心取消。"));
    }
    m_awaitingApproval = false;
    return true;
}

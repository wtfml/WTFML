/**
 * @file AgentTaskLedgerTests.cpp
 * @brief #130：智能体操作进入任务中心。只读不记、弹确认才记快写、计划任务按步推进、
 *        等待用户进出、拒绝/离页/作废/失败的终态、任务中心取消。
 */

#include "Support.hpp"

#include "agent/AgentSkill.hpp"
#include "agent/AgentWorkflow.hpp"
#include "controllers/AgentTaskLedger.hpp"
#include "tasks/TaskManager.hpp"

#include <QJsonObject>

namespace {

Agent::ToolCall callOf(const QString &id, const QString &name, const QJsonObject &arguments = {}) {
    Agent::ToolCall call;
    call.id = id;
    call.name = name;
    call.arguments = arguments;
    return call;
}

AgentToolResult resultOf(bool ok, const QString &summary = QStringLiteral("完成")) {
    AgentToolResult result;
    result.status = ok ? AgentToolResult::Status::Ok : AgentToolResult::Status::Error;
    result.summary = summary;
    return result;
}

struct Rig {
    Tasks::TaskManager tasks;
    AgentWorkflow workflow;
    AgentTaskLedger ledger{&workflow};
    Rig() {
        ledger.setTaskManager(&tasks);
        ledger.setContextProvider([] {
            return AgentTaskLedger::Context{QStringLiteral("session-1"), QStringLiteral("demo"),
                                            QStringLiteral("Demo")};
        });
    }
    /// 真流程里复合引擎先记账、循环再发 toolCallFinished：这里照同一顺序喂。
    void run(const Agent::ToolCall &call, bool ok, bool approval = false, bool approved = true) {
        ledger.onToolStarted(call);
        if (approval) {
            ledger.onApprovalRequested(call);
            ledger.onApprovalResolved(approved);
        }
        const AgentToolResult result = resultOf(ok && approved);
        workflow.recordResult(call, result);
        ledger.onToolFinished(call, result);
    }
    bool approvePlan(const QList<AgentPlanStep> &steps) {
        workflow.setSkill(AgentSkillRegistry::find(QStringLiteral("crash-rescue")));
        QString problem;
        if (!workflow.submitPlan(QStringLiteral("恢复游戏启动"), steps, &problem)) return false;
        const Agent::ToolCall submit = callOf(QStringLiteral("plan"), QStringLiteral("workflow.submitPlan"));
        ledger.onToolStarted(submit);
        ledger.onApprovalRequested(submit);
        ledger.onApprovalResolved(true);
        workflow.approvePlan(true);
        ledger.onToolFinished(submit, resultOf(true));
        return workflow.planApproved();
    }
};

AgentPlanStep planStep(const QString &tool, const QJsonObject &arguments, const QString &reason,
                       bool verify = false) {
    AgentPlanStep step;
    step.tool = tool;
    step.arguments = arguments;
    step.reason = reason;
    step.verify = verify;
    return step;
}

QList<AgentPlanStep> twoSteps() {
    return {planStep(QStringLiteral("mods.setEnabled"),
                     {{QStringLiteral("fileName"), QStringLiteral("a.jar")}, {QStringLiteral("enabled"), false}},
                     QStringLiteral("禁用与 Sodium 冲突的 a.jar")),
            planStep(QStringLiteral("diagnostics.startup"), {}, QString(), true)};
}

}  // namespace

void runAgentTaskLedgerTests() {
    section("AT1. 只读不记；快写只在弹确认时记");
    {
        Rig rig;
        rig.run(callOf(QStringLiteral("r1"), QStringLiteral("logs.search")), true);
        rig.run(callOf(QStringLiteral("r2"), QStringLiteral("fs.read")), true);
        check(rig.tasks.all().isEmpty(), "AT1 只读工具一项都不记");
        rig.run(callOf(QStringLiteral("w0"), QStringLiteral("settings.set")), true);
        check(rig.tasks.all().isEmpty(), "AT1 自动放行（没弹确认）的快写不记");

        const Agent::ToolCall set = callOf(QStringLiteral("w1"), QStringLiteral("settings.set"),
                                           {{QStringLiteral("key"), QStringLiteral("memory.xmx")}});
        rig.ledger.onToolStarted(set);
        check(rig.tasks.all().isEmpty(), "AT1 快写开始时还不记");
        rig.ledger.onApprovalRequested(set);
        const Tasks::TaskId id = rig.ledger.standaloneTask();
        const Tasks::TaskSnapshot waiting = rig.tasks.snapshot(id);
        check(id != Tasks::invalidTaskId() && waiting.state == Tasks::TaskState::WaitingUser &&
                  waiting.title == QStringLiteral("智能体：修改启动器设置") &&
                  waiting.detail == QStringLiteral("等待确认") && waiting.kind == Tasks::TaskKind::Other,
              "AT1 弹确认 → 记一项「等待确认」", waiting.title + QLatin1Char('|') + waiting.detail);
        check(waiting.capabilities.canOpenAgentSession && waiting.capabilities.canCancel &&
                  rig.ledger.sessionFor(id) == QStringLiteral("session-1"),
              "AT1 带「查看会话」与会话号");
        rig.ledger.onApprovalResolved(true);
        check(rig.tasks.snapshot(id).state == Tasks::TaskState::Running, "AT1 批准后回到进行中");
        rig.ledger.onToolFinished(set, resultOf(true, QStringLiteral("已修改")));
        check(rig.tasks.snapshot(id).state == Tasks::TaskState::Succeeded, "AT1 执行成功 → 已完成");
        check(rig.tasks.all().size() == 1, "AT1 全程只有这一项");
    }

    section("AT2. 拒绝 / 失败 / 离页 / 回合出错 的终态");
    {
        Rig rig;
        const Agent::ToolCall write = callOf(QStringLiteral("w2"), QStringLiteral("fs.write"),
                                             {{QStringLiteral("path"), QStringLiteral("options.txt")}});
        rig.ledger.onToolStarted(write);
        rig.ledger.onApprovalRequested(write);
        const Tasks::TaskId rejected = rig.ledger.standaloneTask();
        check(rig.tasks.snapshot(rejected).title == QStringLiteral("智能体：修改「Demo」中的 options.txt"),
              "AT2 fs.write 标题带实例名与路径", rig.tasks.snapshot(rejected).title);
        rig.ledger.onApprovalResolved(false);
        rig.ledger.onToolFinished(write, resultOf(false, QStringLiteral("用户拒绝")));
        check(rig.tasks.snapshot(rejected).state == Tasks::TaskState::Cancelled &&
                  rig.tasks.snapshot(rejected).detail == QStringLiteral("已拒绝，未修改任何内容。"),
              "AT2 拒绝 → 已取消，说明未修改", rig.tasks.snapshot(rejected).detail);

        const Agent::ToolCall integrity = callOf(QStringLiteral("l1"), QStringLiteral("instance.integrity"));
        rig.ledger.onToolStarted(integrity);
        const Tasks::TaskId failed = rig.ledger.standaloneTask();
        check(rig.tasks.snapshot(failed).kind == Tasks::TaskKind::Integrity &&
                  rig.tasks.snapshot(failed).state == Tasks::TaskState::Running &&
                  rig.tasks.snapshot(failed).title == QStringLiteral("智能体：检查「Demo」的文件"),
              "AT2 耗时工具开始就记（检查文件 = Integrity）");
        rig.ledger.onToolFinished(integrity, resultOf(false, QStringLiteral("读不到版本文件")));
        check(rig.tasks.snapshot(failed).state == Tasks::TaskState::Failed &&
                  rig.tasks.snapshot(failed).errorSummary == QStringLiteral("读不到版本文件"),
              "AT2 工具失败 → 失败并带原因");

        rig.ledger.onToolStarted(callOf(QStringLiteral("l2"), QStringLiteral("backup.create")));
        const Tasks::TaskId left = rig.ledger.standaloneTask();
        check(rig.tasks.snapshot(left).kind == Tasks::TaskKind::Backup, "AT2 备份记成 Backup");
        rig.ledger.onLoopState(AgentLoop::State::Cancelled);
        check(rig.tasks.snapshot(left).state == Tasks::TaskState::Cancelled &&
                  rig.tasks.snapshot(left).detail == QStringLiteral("已离开智能体页或已停止，操作未继续。"),
              "AT2 停止 / 离页 → 已取消");

        rig.ledger.onToolStarted(callOf(QStringLiteral("l3"), QStringLiteral("instance.testLaunch")));
        const Tasks::TaskId launch = rig.ledger.standaloneTask();
        check(rig.tasks.snapshot(launch).kind == Tasks::TaskKind::Other, "AT2 测试启动不记成 Launch");
        Agent::Error error;
        error.message = QStringLiteral("网络中断");
        rig.ledger.onTurnFailed(error);
        check(rig.tasks.snapshot(launch).state == Tasks::TaskState::Failed &&
                  rig.tasks.snapshot(launch).errorSummary == QStringLiteral("网络中断"),
              "AT2 回合出错 → 失败");
    }

    section("AT3. 计划任务：按步推进，计划内的步骤不另记，等待在会话中继续");
    {
        Rig rig;
        check(rig.approvePlan(twoSteps()), "AT3 计划提交并批准");
        const Tasks::TaskId plan = rig.ledger.planTask();
        Tasks::TaskSnapshot snap = rig.tasks.snapshot(plan);
        check(plan != Tasks::invalidTaskId() && snap.title == QStringLiteral("智能体修复计划：Demo") &&
                  snap.detail == QStringLiteral("恢复游戏启动") && snap.steps.size() == 2 &&
                  snap.steps.at(0).name == QStringLiteral("禁用与 Sodium 冲突的 a.jar") &&
                  snap.steps.at(1).name == QStringLiteral("diagnostics.startup"),
              "AT3 批准计划时建计划任务：步骤 = 计划步骤（理由为空用工具名）", snap.title);

        const Agent::ToolCall disable = callOf(QStringLiteral("s1"), QStringLiteral("mods.setEnabled"),
                                               {{QStringLiteral("fileName"), QStringLiteral("a.jar")},
                                                {QStringLiteral("enabled"), false}});
        rig.ledger.onToolStarted(disable);
        check(rig.tasks.snapshot(plan).steps.at(0).state == Tasks::TaskStepState::Running,
              "AT3 第 1 步开始 → 那一步进行中");
        rig.ledger.onApprovalRequested(disable);
        check(rig.tasks.snapshot(plan).state == Tasks::TaskState::WaitingUser &&
                  rig.ledger.standaloneTask() == Tasks::invalidTaskId(),
              "AT3 计划步骤弹确认 → 计划任务等待确认，不另记一项");
        rig.ledger.onApprovalResolved(true);
        rig.workflow.recordResult(disable, resultOf(true));
        rig.ledger.onToolFinished(disable, resultOf(true));
        snap = rig.tasks.snapshot(plan);
        check(snap.state == Tasks::TaskState::Running && snap.steps.at(0).state == Tasks::TaskStepState::Done,
              "AT3 第 1 步完成");

        rig.ledger.onLoopState(AgentLoop::State::Completed);
        snap = rig.tasks.snapshot(plan);
        check(snap.state == Tasks::TaskState::WaitingUser && snap.detail == QStringLiteral("等待在会话中继续"),
              "AT3 回合结束而计划没做完 → 等待在会话中继续");
        rig.ledger.onLoopState(AgentLoop::State::Preparing);
        check(rig.tasks.snapshot(plan).state == Tasks::TaskState::Running, "AT3 用户接着发消息 → 回到进行中");

        rig.run(callOf(QStringLiteral("s2"), QStringLiteral("diagnostics.startup")), true);
        check(rig.tasks.snapshot(plan).steps.at(1).state == Tasks::TaskStepState::Done, "AT3 验证步完成");
        rig.workflow.markFinished();
        rig.ledger.onToolFinished(callOf(QStringLiteral("f"), QStringLiteral("workflow.finish")), resultOf(true));
        check(rig.tasks.snapshot(plan).state == Tasks::TaskState::Succeeded, "AT3 workflow.finish → 已完成");
        check(rig.tasks.all().size() == 1, "AT3 整个计划只有计划任务这一项",
              QString::number(rig.tasks.all().size()));
    }

    section("AT4. 计划失败 / 拒绝某一步 / 作废");
    {
        Rig rig;
        rig.approvePlan(twoSteps());
        const Tasks::TaskId plan = rig.ledger.planTask();
        rig.run(callOf(QStringLiteral("s1"), QStringLiteral("mods.setEnabled")), false);
        const Tasks::TaskSnapshot snap = rig.tasks.snapshot(plan);
        check(snap.state == Tasks::TaskState::Failed && snap.steps.at(0).state == Tasks::TaskStepState::Failed &&
                  snap.errorSummary.contains(QStringLiteral("第 1 步")),
              "AT4 计划步骤失败 → 那一步失败、计划任务失败并带原因", snap.errorSummary);
    }
    {
        Rig rig;
        rig.approvePlan(twoSteps());
        const Tasks::TaskId plan = rig.ledger.planTask();
        rig.run(callOf(QStringLiteral("s1"), QStringLiteral("mods.setEnabled")), true, true, false);
        check(rig.tasks.snapshot(plan).state == Tasks::TaskState::Cancelled &&
                  rig.tasks.snapshot(plan).detail == QStringLiteral("已拒绝第 1 步，计划未继续。"),
              "AT4 拒绝计划中的一步 → 计划任务已取消", rig.tasks.snapshot(plan).detail);
    }
    {
        Rig rig;
        rig.approvePlan(twoSteps());
        const Tasks::TaskId plan = rig.ledger.planTask();
        rig.ledger.onWorkflowReset();
        check(rig.tasks.snapshot(plan).state == Tasks::TaskState::Cancelled &&
                  rig.tasks.snapshot(plan).detail == QStringLiteral("计划已作废。"),
              "AT4 换会话 / 换模式 → 计划已作废");
    }

    section("AT5. 任务中心取消");
    {
        Rig rig;
        rig.approvePlan(twoSteps());
        const Tasks::TaskId plan = rig.ledger.planTask();
        check(rig.ledger.owns(plan) && rig.ledger.canCancel(plan), "AT5 计划任务可从任务中心取消");
        rig.ledger.onToolStarted(callOf(QStringLiteral("c"), QStringLiteral("repair.finish")));
        check(!rig.ledger.canCancel(plan) && !rig.tasks.snapshot(plan).capabilities.canCancel &&
                  rig.tasks.snapshot(plan).capabilities.cancelBlockedReason ==
                      QStringLiteral("正在写回实例文件，完成后才能取消。"),
              "AT5 repair.finish 写回中不能取消，并说明原因");
        rig.ledger.onToolFinished(callOf(QStringLiteral("c"), QStringLiteral("repair.finish")), resultOf(true));
        check(rig.ledger.canCancel(plan), "AT5 写回结束后又能取消");
        check(rig.ledger.cancelFromTaskCenter(plan), "AT5 取消成功");
        check(rig.tasks.snapshot(plan).state == Tasks::TaskState::Cancelled &&
                  rig.workflow.stage() == AgentWorkflowStage::Failed &&
                  rig.workflow.failureReason() == QStringLiteral("已在任务中心取消。"),
              "AT5 计划任务已取消，工作流标为失败");
        check(!rig.ledger.cancelFromTaskCenter(plan), "AT5 已收尾的不能再取消");

        const Agent::ToolCall verify = callOf(QStringLiteral("v"), QStringLiteral("mods.verifyFiles"));
        rig.ledger.onToolStarted(verify);
        const Tasks::TaskId standalone = rig.ledger.standaloneTask();
        check(rig.ledger.cancelFromTaskCenter(standalone) &&
                  rig.tasks.snapshot(standalone).state == Tasks::TaskState::Cancelled &&
                  rig.tasks.snapshot(standalone).detail == QStringLiteral("已在任务中心取消。"),
              "AT5 独立任务取消");
        rig.ledger.onToolFinished(verify, resultOf(true));
        check(rig.tasks.snapshot(standalone).state == Tasks::TaskState::Cancelled, "AT5 取消后迟到的结果不改判");
        check(!rig.ledger.owns(12345), "AT5 别人的任务号不认");
    }
}

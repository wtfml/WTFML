/**
 * @file AgentWorkflowTests.cpp
 * @brief 工作流状态机：计划校验、批准闸、写步骤必须在计划里、失败即停、完成闸要证据。
 */

#include "Support.hpp"

#include "agent/AgentCompositeToolEngine.hpp"
#include "agent/AgentSkill.hpp"
#include "agent/AgentTool.hpp"
#include "agent/AgentWorkflow.hpp"
#include "agent/AgentWorkflowEngine.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace {

Agent::ToolCall callFor(const QString &name, const QJsonObject &arguments = {}) {
    Agent::ToolCall call;
    call.id = QStringLiteral("call-1");
    call.name = name;
    call.arguments = arguments;
    return call;
}

AgentPlanStep stepFor(const QString &tool, const QJsonObject &arguments = {},
                      const QString &reason = QStringLiteral("原因"),
                      bool verify = false) {
    AgentPlanStep step;
    step.tool = tool;
    step.arguments = arguments;
    step.reason = reason;
    step.verify = verify;
    return step;
}

QList<AgentPlanStep> validPlanSteps() {
    return {stepFor(QStringLiteral("mods.setEnabled"),
                    QJsonObject{{QStringLiteral("fileName"), QStringLiteral("a.jar")},
                                {QStringLiteral("enabled"), false}}),
            stepFor(QStringLiteral("diagnostics.startup"), {}, QStringLiteral("验证启动诊断"), true)};
}

/// 一个只会回固定结果的假引擎，用来验复合层的闸和记账。
class StubEngine final : public AgentToolEngine {
public:
    QString name;
    AgentToolResult last;
    int calls = 0;
    bool approval = false;

    QJsonArray schemas() const override {
        QJsonArray array;
        array.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), name},
                         {QStringLiteral("description"), QStringLiteral("stub")},
                         {QStringLiteral("parameters"), QJsonObject{}}}}});
        return array;
    }
    bool handles(const QString &internalName) const override { return internalName == name; }
    AgentToolResult invoke(const Agent::ToolCall &) override {
        ++calls;
        if (approval) {
            AgentToolResult result;
            result.status = AgentToolResult::Status::ApprovalRequired;
            result.approval = QJsonObject{{QStringLiteral("approvalId"), QStringLiteral("appr-1")}};
            return result;
        }
        return last;
    }
    AgentToolResult resolveApproval(const QString &, bool approved) override {
        AgentToolResult result;
        result.status = approved ? AgentToolResult::Status::Ok : AgentToolResult::Status::Error;
        result.error.kind = approved ? Agent::ErrorKind::Internal : Agent::ErrorKind::ApprovalDenied;
        result.summary = approved ? QStringLiteral("ok") : QStringLiteral("denied");
        return result;
    }
    void cancelPending() override {}
};

AgentToolResult okResultWithEvidence(const QString &evidenceId) {
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = QStringLiteral("读到了");
    result.data = QJsonObject{
        {QStringLiteral("evidence"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), evidenceId},
                                {QStringLiteral("kind"), QStringLiteral("diagnostic")}}}}};
    return result;
}

AgentToolResult failedResult() {
    AgentToolResult result;
    result.status = AgentToolResult::Status::Error;
    result.summary = QStringLiteral("写盘失败");
    result.error.kind = Agent::ErrorKind::ToolError;
    result.error.message = result.summary;
    return result;
}

}  // namespace

void runAgentWorkflowTests() {
    AgentWorkflow workflow;
    workflow.setSkill(AgentSkillRegistry::find(QStringLiteral("general")));
    check(!workflow.active(), "工作流：自由对话不算工作流");

    // ---------- 提交计划：字段与工具校验 ----------
    workflow.setSkill(AgentSkillRegistry::find(QStringLiteral("crash-rescue")));
    check(workflow.active(), "工作流：崩溃急救激活状态机");
    check(workflow.requiresPlan(), "工作流：崩溃急救要求先提交计划");

    QString problem;
    check(!workflow.submitPlan(QString(), validPlanSteps(), &problem),
          "工作流：缺目标的计划被拒");
    check(problem.contains(QStringLiteral("目标")), "工作流：缺目标时说清是目标");
    check(!workflow.submitPlan(QStringLiteral("修一下"), {}, &problem),
          "工作流：空步骤计划被拒");
    check(!workflow.submitPlan(QStringLiteral("修一下"),
                               {stepFor(QStringLiteral("no.such.tool")),
                                stepFor(QStringLiteral("diagnostics.startup"), {}, QString(), true)},
                               &problem),
          "工作流：未知工具的计划被拒");
    check(!workflow.submitPlan(
              QStringLiteral("修一下"),
              {stepFor(QStringLiteral("settings.set"),
                       QJsonObject{{QStringLiteral("key"), QStringLiteral("memory.xmx")}}),
               stepFor(QStringLiteral("diagnostics.startup"), {}, QString(), true)},
              &problem),
          "工作流：写步骤缺必填参数时被拒（parameters 校验）");
    check(!workflow.submitPlan(
              QStringLiteral("列一下文件名"),
              {stepFor(QStringLiteral("fs.listNames"),
                       QJsonObject{{QStringLiteral("path"), QStringLiteral("mods")}}),
               stepFor(QStringLiteral("diagnostics.startup"), {}, QString(), true)},
              &problem),
          "工作流：不在白名单的工具被拒");
    check(workflow.stage() == AgentWorkflowStage::Collect, "工作流：计划被拒时状态不变");

    check(!workflow.submitPlan(QStringLiteral("修一下"),
                               {stepFor(QStringLiteral("mods.setEnabled"),
                                        QJsonObject{{QStringLiteral("fileName"),
                                                     QStringLiteral("a.jar")},
                                                    {QStringLiteral("enabled"), false}})},
                               &problem),
          "工作流：没有验证步的计划被拒");
    check(problem.contains(QStringLiteral("验证")), "工作流：缺验证步时说清");
    check(!workflow.submitPlan(QStringLiteral("修一下"),
                               {stepFor(QStringLiteral("logs.tail")),
                                stepFor(QStringLiteral("logs.tail"), {}, QString(), true)},
                               &problem),
          "工作流：只有只读步骤的计划被拒（没有修复步骤）");
    check(!workflow.submitPlan(
              QStringLiteral("修一下"),
              {stepFor(QStringLiteral("mods.setEnabled"),
                       QJsonObject{{QStringLiteral("fileName"), QStringLiteral("a.jar")},
                                   {QStringLiteral("enabled"), false}}),
               stepFor(QStringLiteral("fs.write"),
                       QJsonObject{{QStringLiteral("path"), QStringLiteral("options.txt")},
                                   {QStringLiteral("content"), QStringLiteral("x")}},
                       QStringLiteral("写"), true)},
              &problem),
          "工作流：验证步本身是写操作时被拒");

    check(workflow.submitPlan(QStringLiteral("关掉冲突模组"), validPlanSteps(), &problem),
          "工作流：合法计划通过校验", problem);
    check(workflow.stage() == AgentWorkflowStage::AwaitPlanApproval,
          "工作流：通过校验后等审批");
    check(!workflow.planApproved(), "工作流：提交 ≠ 批准");

    // ---------- 计划卡 ----------
    AgentWorkflowEngine engine(&workflow);
    Agent::ToolCall submit;
    submit.id = QStringLiteral("c1");
    submit.name = QStringLiteral("workflow.submitPlan");
    submit.arguments = QJsonObject{
        {QStringLiteral("goal"), QStringLiteral("关掉冲突模组")},
        {QStringLiteral("steps"),
         QJsonArray{QJsonObject{{QStringLiteral("tool"), QStringLiteral("mods.setEnabled")},
                                {QStringLiteral("arguments"),
                                 QJsonObject{{QStringLiteral("fileName"), QStringLiteral("a.jar")},
                                             {QStringLiteral("enabled"), false}}},
                                {QStringLiteral("reason"), QStringLiteral("先停掉它")}},
                    QJsonObject{{QStringLiteral("tool"), QStringLiteral("diagnostics.startup")},
                                {QStringLiteral("reason"), QStringLiteral("看诊断")},
                                {QStringLiteral("verify"), true}}}}};
    AgentToolResult planResult = engine.invoke(submit);
    check(planResult.status == AgentToolResult::Status::ApprovalRequired,
          "工作流：提交计划要计划卡审批");
    check(planResult.approval.contains(QStringLiteral("plan")), "工作流：计划卡带结构化计划");
    const QString approvalId = planResult.approval.value(QStringLiteral("approvalId")).toString();
    const AgentToolResult denied = engine.resolveApproval(approvalId, false);
    check(denied.error.kind == Agent::ErrorKind::ApprovalDenied, "工作流：拒绝计划");
    check(workflow.stage() == AgentWorkflowStage::Collect, "工作流：拒绝后计划作废");

    // 重新提交并批准
    planResult = engine.invoke(submit);
    const QString approvedId = planResult.approval.value(QStringLiteral("approvalId")).toString();
    const AgentToolResult approved = engine.resolveApproval(approvedId, true);
    check(approved.status == AgentToolResult::Status::Ok, "工作流：批准计划");
    check(workflow.planApproved(), "工作流：批准后 planApproved");
    check(workflow.stage() == AgentWorkflowStage::Execute, "工作流：批准后进入执行阶段");

    // ---------- 复合闸：写步骤必须在计划里 ----------
    StubEngine fileEngine;
    fileEngine.name = QStringLiteral("mods.setEnabled");
    fileEngine.last = okResultWithEvidence(QStringLiteral("E9"));
    StubEngine readEngine;
    readEngine.name = QStringLiteral("diagnostics.startup");
    readEngine.last = okResultWithEvidence(QStringLiteral("E1"));
    StubEngine settingsEngine;
    settingsEngine.name = QStringLiteral("settings.set");
    settingsEngine.last = okResultWithEvidence(QStringLiteral("E2"));

    AgentCompositeToolEngine composite;
    composite.addEngine(&fileEngine);
    composite.addEngine(&readEngine);
    composite.addEngine(&settingsEngine);
    composite.addEngine(&engine);
    composite.setWorkflow(&workflow);

    AgentToolResult gated = composite.invoke(callFor(QStringLiteral("settings.set"),
                                                     QJsonObject{{QStringLiteral("key"),
                                                                  QStringLiteral("memory.xmx")},
                                                                 {QStringLiteral("value"),
                                                                  QStringLiteral("3072")}}));
    check(gated.status == AgentToolResult::Status::Error,
          "工作流：计划外的写工具被复合层拒绝");
    check(gated.data.value(QStringLiteral("blockedByWorkflow")).toBool(),
          "工作流：拒绝时带 blockedByWorkflow 标记");
    check(settingsEngine.calls == 0, "工作流：被拒的写工具没有真的执行");

    gated = composite.invoke(callFor(QStringLiteral("mods.setEnabled"),
                                     QJsonObject{{QStringLiteral("fileName"),
                                                  QStringLiteral("a.jar")},
                                                 {QStringLiteral("enabled"), false}}));
    // 计划里那一步的工具参数与这次不完全一样，但闸只看工具名 —— 与文档一致。
    check(fileEngine.calls == 1, "工作流：计划内的写工具放行");
    check(workflow.evidenceIds().contains(QStringLiteral("E9")), "工作流：执行结果进证据账");
    check(workflow.statusJson().value(QStringLiteral("executedSteps")).toInt() == 1,
          "工作流：执行步数推进");

    // ---------- 顺序闸：不能跳步 ----------
    gated = composite.invoke(callFor(QStringLiteral("mods.setEnabled"),
                                     QJsonObject{{QStringLiteral("fileName"),
                                                  QStringLiteral("b.jar")},
                                                 {QStringLiteral("enabled"), false}}));
    check(gated.status == AgentToolResult::Status::Error,
          "工作流：不是当前这一步的写工具被拒（不能跳步）");
    check(gated.summary.contains(QStringLiteral("diagnostics.startup")),
          "工作流：拒绝时点名当前该做哪一步");

    // ---------- 失败即停 ----------
    readEngine.last = failedResult();
    gated = composite.invoke(callFor(QStringLiteral("diagnostics.startup")));
    check(gated.status == AgentToolResult::Status::Error, "工作流：失败步骤原样回错误");
    check(workflow.stage() == AgentWorkflowStage::Failed, "工作流：失败后阶段转 Failed");
    check(!workflow.canFinish(&problem), "工作流：失败后不能报完成");
    check(problem.contains(QStringLiteral("失败")), "工作流：失败原因说得出来");
    gated = composite.invoke(callFor(QStringLiteral("settings.set"),
                                     QJsonObject{{QStringLiteral("key"),
                                                  QStringLiteral("memory.xmx")},
                                                 {QStringLiteral("value"),
                                                  QStringLiteral("4096")}}));
    check(gated.status == AgentToolResult::Status::Error, "工作流：失败后写工具继续被拒");

    // ---------- 完成闸：证据 + 计划 + 验证 ----------
    AgentWorkflow second;
    second.setSkill(AgentSkillRegistry::find(QStringLiteral("inspection")));
    check(second.readOnly(), "工作流：体检是只读模式");
    check(!second.requiresPlan(), "工作流：体检不需要计划");
    AgentWorkflowEngine secondEngine(&second);
    Agent::ToolCall finish;
    finish.id = QStringLiteral("c2");
    finish.name = QStringLiteral("workflow.finish");
    finish.arguments = QJsonObject{{QStringLiteral("report"), QStringLiteral("都正常")}};
    AgentToolResult finishResult = secondEngine.invoke(finish);
    check(finishResult.status == AgentToolResult::Status::Error,
          "工作流：没有任何证据时报完成被拒");
    check(finishResult.summary.contains(QStringLiteral("diagnostics.startup")),
          "工作流：缺必需工具时点名缺哪一个");

    StubEngine startupStub;
    startupStub.name = QStringLiteral("diagnostics.startup");
    startupStub.last = okResultWithEvidence(QStringLiteral("E1"));
    AgentCompositeToolEngine secondComposite;
    secondComposite.addEngine(&startupStub);
    secondComposite.addEngine(&secondEngine);
    secondComposite.setWorkflow(&second);
    secondComposite.invoke(callFor(QStringLiteral("diagnostics.startup")));
    finishResult = secondEngine.invoke(finish);
    check(finishResult.status == AgentToolResult::Status::Ok,
          "工作流：有证据且跑过诊断后可以报完成", finishResult.summary);
    check(second.stage() == AgentWorkflowStage::Report, "工作流：完成后阶段是 report");

    // ---------- 只读模式禁止写 ----------
    AgentWorkflow third;
    third.setSkill(AgentSkillRegistry::find(QStringLiteral("inspection")));
    StubEngine writeStub;
    writeStub.name = QStringLiteral("settings.set");
    writeStub.last = okResultWithEvidence(QStringLiteral("E4"));
    AgentCompositeToolEngine thirdComposite;
    thirdComposite.addEngine(&writeStub);
    thirdComposite.setWorkflow(&third);
    const AgentToolResult readOnlyBlocked = thirdComposite.invoke(
        callFor(QStringLiteral("settings.set"),
                QJsonObject{{QStringLiteral("key"), QStringLiteral("memory.xmx")},
                            {QStringLiteral("value"), QStringLiteral("2048")}}));
    check(readOnlyBlocked.status == AgentToolResult::Status::Error,
          "工作流：只读模式拒绝写工具");
    check(writeStub.calls == 0, "工作流：只读模式下写工具没执行");
}

/**
 * @file AgentAutoSafeTests.cpp
 * @brief S5.7：Auto-safe 授权的范围、时效、启动预算、硬禁止、审计落盘，以及复合引擎在
 *        授权范围内替用户点"允许"、范围外仍旧逐次确认。
 */

#include "Support.hpp"

#include "agent/AgentApprovalPolicy.hpp"
#include "agent/AgentAutoSafe.hpp"
#include "agent/AgentCompositeToolEngine.hpp"
#include "agent/AgentPaths.hpp"
#include "agent/AgentTool.hpp"
#include "agent/AgentWorkflow.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

namespace {

Agent::ToolCall callFor(const QString &name, const QJsonObject &arguments = {}) {
    Agent::ToolCall call;
    call.id = QStringLiteral("call-auto-safe");
    call.name = name;
    call.arguments = arguments;
    return call;
}

AgentPlanStep stepFor(const QString &tool, const QJsonObject &arguments = {}) {
    AgentPlanStep step;
    step.tool = tool;
    step.arguments = arguments;
    step.reason = QStringLiteral("测试");
    return step;
}

/// 每个工具都先要审批；批准后回 Ok。用来观察复合层有没有替用户点"允许"。
class FakeWriteEngine final : public AgentToolEngine {
public:
    QStringList tools{QStringLiteral("mods.setEnabled"), QStringLiteral("fs.write"),
                      QStringLiteral("settings.set"), QStringLiteral("repair.begin"),
                      QStringLiteral("repair.finish"), QStringLiteral("instance.testLaunch"),
                      QStringLiteral("instance.repairCore"), QStringLiteral("backup.restore"),
                      QStringLiteral("mods.analyzeCompatibility")};

    QJsonArray schemas() const override { return {}; }
    bool handles(const QString &internalName) const override {
        return tools.contains(internalName);
    }
    AgentToolResult invoke(const Agent::ToolCall &call) override {
        AgentToolResult result;
        result.status = AgentToolResult::Status::ApprovalRequired;
        result.summary = QStringLiteral("需要审批");
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject request = AgentApprovalPolicy::request(
            call.name, QStringLiteral("测试"), QString(), QStringLiteral("需要审批"), QString(),
            QString(), QString(), QJsonObject{});
        request.insert(QStringLiteral("approvalId"), id);
        result.approval = request;
        m_pending.insert(id, call);
        return result;
    }
    AgentToolResult resolveApproval(const QString &approvalId, bool approved) override {
        if (!m_pending.contains(approvalId)) {
            AgentToolResult result;
            result.status = AgentToolResult::Status::Error;
            return result;
        }
        m_pending.remove(approvalId);
        ++resolveCalls;
        AgentToolResult result;
        result.status = approved ? AgentToolResult::Status::Ok : AgentToolResult::Status::Error;
        result.summary = approved ? QStringLiteral("已执行") : QStringLiteral("被拒");
        result.data = QJsonObject{{QStringLiteral("executed"), approved}};
        return result;
    }
    void cancelPending() override { m_pending.clear(); }

    int resolveCalls = 0;
    QHash<QString, Agent::ToolCall> m_pending;
};

QDateTime baseTime() {
    return QDateTime(QDate(2026, 11, 24), QTime(12, 0, 0), QTimeZone::UTC);
}

}  // namespace

void runAgentAutoSafeTests() {
    // ---------------------------------------------------------------- 范围与判定
    {
        AgentAutoSafe::Authorizer authorizer;
        check(!authorizer.enabled(), "Auto-safe：默认关闭");
        authorizer.enable(QStringLiteral("inst-1"), QStringLiteral("session-1"), baseTime());
        check(authorizer.enabled() && authorizer.matchesInstance(QStringLiteral("inst-1")) &&
                  !authorizer.matchesInstance(QStringLiteral("inst-2")),
              "Auto-safe：只对开启时的实例生效");
        check(authorizer.deadline() == baseTime().addSecs(60 * 60),
              "Auto-safe：默认 60 分钟到期", authorizer.deadline().toString(Qt::ISODate));
        check(!authorizer.expired(baseTime().addSecs(59 * 60)) &&
                  authorizer.expired(baseTime().addSecs(61 * 60)),
              "Auto-safe：到期时间按 60 分钟判");

        const QList<AgentPlanStep> steps{
            stepFor(QStringLiteral("mods.setEnabled"),
                    QJsonObject{{QStringLiteral("fileName"), QStringLiteral("x.jar")}}),
            stepFor(QStringLiteral("fs.write"),
                    QJsonObject{{QStringLiteral("path"), QStringLiteral("config/a.toml")}}),
            stepFor(QStringLiteral("settings.set"),
                    QJsonObject{{QStringLiteral("key"), QStringLiteral("memory.xmx")}}),
            stepFor(QStringLiteral("mods.installFix"),
                    QJsonObject{{QStringLiteral("projectId"), QStringLiteral("proj-1")}})};
        authorizer.bindPlan(steps);
        authorizer.bindTransaction(QStringLiteral("tx-1"), QStringLiteral("ws-1"));
        check(authorizer.coveredPaths() == QStringList{QStringLiteral("config/a.toml")} &&
                  authorizer.coveredKeys() == QStringList{QStringLiteral("memory.xmx")} &&
                  authorizer.coveredModFiles() == QStringList{QStringLiteral("x.jar")},
              "Auto-safe：范围来自已批准计划");

        const QDateTime now = baseTime().addSecs(60);
        QString reason;
        int launches = 0;
        const auto decide = [&](const Agent::ToolCall &call) {
            reason.clear();
            launches = 0;
            return authorizer.authorize(call, now, &reason, &launches);
        };

        check(decide(callFor(QStringLiteral("mods.setEnabled"),
                             {{QStringLiteral("fileName"), QStringLiteral("x.jar")},
                              {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}})) ==
                  AgentAutoSafe::Decision::Allow,
              "Auto-safe：计划内的模组启停放行");
        check(decide(callFor(QStringLiteral("mods.setEnabled"),
                             {{QStringLiteral("fileName"), QStringLiteral("y.jar")},
                              {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}})) ==
                  AgentAutoSafe::Decision::RequireApproval,
              "Auto-safe：计划外的模组仍要确认");
        check(decide(callFor(QStringLiteral("mods.setEnabled"),
                             {{QStringLiteral("fileName"), QStringLiteral("x.jar")},
                              {QStringLiteral("workspaceId"), QStringLiteral("ws-2")}})) ==
                  AgentAutoSafe::Decision::Deny,
              "Auto-safe：别的工区写入直接拒绝");
        check(decide(callFor(QStringLiteral("fs.write"),
                             {{QStringLiteral("path"), QStringLiteral("config/a.toml")},
                              {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}})) ==
                  AgentAutoSafe::Decision::Allow,
              "Auto-safe：计划内的配置写入放行");
        check(decide(callFor(QStringLiteral("fs.write"),
                             {{QStringLiteral("path"), QStringLiteral("options.txt")},
                              {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}})) ==
                  AgentAutoSafe::Decision::RequireApproval,
              "Auto-safe：计划外的 options.txt 仍要确认", reason);
        check(decide(callFor(QStringLiteral("fs.write"),
                             {{QStringLiteral("path"), QStringLiteral("saves/level.dat")},
                              {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}})) ==
                  AgentAutoSafe::Decision::Deny,
              "Auto-safe：世界/存档直接拒绝", reason);
        check(decide(callFor(QStringLiteral("settings.set"),
                             {{QStringLiteral("key"), QStringLiteral("memory.xmx")}})) ==
                  AgentAutoSafe::Decision::Allow,
              "Auto-safe：计划内的设置键放行");
        check(decide(callFor(QStringLiteral("settings.set"),
                             {{QStringLiteral("key"), QStringLiteral("args.extraJvm")}})) ==
                  AgentAutoSafe::Decision::Deny,
              "Auto-safe：args.extraJvm 直接拒绝", reason);
        check(decide(callFor(QStringLiteral("settings.set"),
                             {{QStringLiteral("key"), QStringLiteral("java.path")}})) ==
                  AgentAutoSafe::Decision::Deny,
              "Auto-safe：java.path 直接拒绝", reason);
        check(decide(callFor(QStringLiteral("settings.set"),
                             {{QStringLiteral("key"), QStringLiteral("memory.xms")}})) ==
                  AgentAutoSafe::Decision::RequireApproval,
              "Auto-safe：计划外的设置键仍要确认");
        check(decide(callFor(QStringLiteral("mods.installFix"),
                             {{QStringLiteral("projectId"), QStringLiteral("proj-1")},
                              {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}})) ==
                  AgentAutoSafe::Decision::Allow,
              "Auto-safe：计划内的模组修复放行");
        check(decide(callFor(QStringLiteral("mods.installFix"),
                             {{QStringLiteral("projectId"), QStringLiteral("proj-2")},
                              {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}})) ==
                  AgentAutoSafe::Decision::RequireApproval,
              "Auto-safe：计划外的项目仍要确认");
        check(decide(callFor(QStringLiteral("backup.restore"))) ==
                  AgentAutoSafe::Decision::Deny,
              "Auto-safe：恢复备份直接拒绝");
        check(decide(callFor(QStringLiteral("instance.repairCore"))) ==
                  AgentAutoSafe::Decision::Deny,
              "Auto-safe：核心文件修复不在授权里");
        check(decide(callFor(QStringLiteral("repair.finish"),
                             {{QStringLiteral("transactionId"), QStringLiteral("tx-1")}})) ==
                  AgentAutoSafe::Decision::Allow,
              "Auto-safe：事务提交/回滚包含在授权里");

        // 启动次数：上限 4（把 limits 调小方便测）。
        authorizer.enable(QStringLiteral("inst-1"), QStringLiteral("session-1"), baseTime(),
                          AgentAutoSafe::Limits{60, 4});
        authorizer.bindPlan(steps);
        authorizer.bindTransaction(QStringLiteral("tx-1"), QStringLiteral("ws-1"));
        check(decide(callFor(QStringLiteral("instance.testLaunch"),
                             {{QStringLiteral("repeat"), 2}})) ==
                  AgentAutoSafe::Decision::Allow && launches == 2,
              "Auto-safe：启动放行并记次数", QString::number(launches));
        AgentToolResult ok;
        ok.status = AgentToolResult::Status::Ok;
        authorizer.record(callFor(QStringLiteral("instance.testLaunch")), ok, 2, now);
        check(authorizer.launchesUsed() == 2 && authorizer.remainingLaunches() == 2,
              "Auto-safe：启动次数记账", QString::number(authorizer.launchesUsed()));
        check(decide(callFor(QStringLiteral("instance.testLaunch"),
                             {{QStringLiteral("repeat"), 1}})) ==
                  AgentAutoSafe::Decision::Allow,
              "Auto-safe：预算内还能再启动");
        check(decide(callFor(QStringLiteral("instance.testLaunch"),
                             {{QStringLiteral("repeat"), 3}})) ==
                  AgentAutoSafe::Decision::RequireApproval,
              "Auto-safe：超出启动预算退回逐次确认", reason);
    }

    // ---------------------------------------------------------------- 审计落盘
    {
        AgentAutoSafe::Authorizer authorizer;
        authorizer.enable(QStringLiteral("inst-1"), QStringLiteral("session-1"), baseTime());
        authorizer.bindPlan({stepFor(QStringLiteral("fs.write"),
                                     {{QStringLiteral("path"), QStringLiteral("config/a.toml")}})});
        authorizer.bindTransaction(QStringLiteral("tx-audit"), QStringLiteral("ws-1"));
        AgentToolResult ok;
        ok.status = AgentToolResult::Status::Ok;
        ok.summary = QStringLiteral("已写入");
        authorizer.record(callFor(QStringLiteral("fs.write"),
                                  {{QStringLiteral("path"), QStringLiteral("config/a.toml")}}),
                          ok, 0, baseTime().addSecs(30));
        authorizer.noteVerification(QJsonObject{{QStringLiteral("ok"), true}});
        const QJsonObject audit =
            authorizer.finish(QStringLiteral("committed"), QString(), baseTime().addSecs(300));
        check(!authorizer.enabled(), "Auto-safe：事务结束即失效");
        check(audit.value(QStringLiteral("state")).toString() == QStringLiteral("committed"),
              "Auto-safe：审计写最终状态");
        check(audit.value(QStringLiteral("scope")).toObject()
                      .value(QStringLiteral("transactionId"))
                      .toString() == QStringLiteral("tx-audit"),
              "Auto-safe：审计带事务 id");
        check(audit.value(QStringLiteral("operations")).toArray().size() == 1,
              "Auto-safe：审计记了实际操作");
        check(audit.value(QStringLiteral("verification")).toObject()
                  .value(QStringLiteral("ok"))
                  .toBool(),
              "Auto-safe：审计带验证结果");
        const QString path = audit.value(QStringLiteral("path")).toString();
        check(path == QStringLiteral("transactions/tx-audit/audit.json"), "Auto-safe：审计路径",
              path);
        const QString absolute = AgentPaths::transactionsDir() + QStringLiteral("/tx-audit/audit.json");
        check(QFileInfo::exists(absolute), "Auto-safe：审计写进了事务目录", absolute);
        QFile file(absolute);
        check(file.open(QIODevice::ReadOnly) &&
                  QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("state"))
                          .toString() == QStringLiteral("committed"),
              "Auto-safe：审计文件内容可读");
        check(authorizer.finish(QStringLiteral("committed"), QString(), baseTime().addSecs(400))
                      .value(QStringLiteral("state"))
                      .toString() == QStringLiteral("committed"),
              "Auto-safe：重复收尾返回同一份审计");
    }

    // ---------------------------------------------------------------- 复合引擎替用户点"允许"
    {
        AgentAutoSafe::Authorizer authorizer;
        // 复合层用的是真实时钟（QDateTime::currentDateTimeUtc），这里按"现在"开启。
        authorizer.enable(QStringLiteral("inst-1"), QStringLiteral("session-1"),
                          QDateTime::currentDateTimeUtc());
        authorizer.bindPlan(
            {stepFor(QStringLiteral("mods.setEnabled"),
                     {{QStringLiteral("fileName"), QStringLiteral("x.jar")}}),
             stepFor(QStringLiteral("fs.write"),
                     {{QStringLiteral("path"), QStringLiteral("config/a.toml")}}),
             stepFor(QStringLiteral("settings.set"),
                     {{QStringLiteral("key"), QStringLiteral("memory.xmx")}})});
        authorizer.bindTransaction(QStringLiteral("tx-1"), QStringLiteral("ws-1"));

        AgentWorkflow workflow;
        workflow.setSkill(AgentSkillRegistry::find(QStringLiteral("crash-rescue")));
        QString planProblem;
        // 顺序就是执行顺序；step 带完整参数（计划校验按 schema 查）。
        QList<AgentPlanStep> verifiedPlan{
            stepFor(QStringLiteral("settings.set"),
                    {{QStringLiteral("key"), QStringLiteral("memory.xmx")},
                     {QStringLiteral("value"), QStringLiteral("4096")}}),
            stepFor(QStringLiteral("fs.write"),
                    {{QStringLiteral("path"), QStringLiteral("config/a.toml")},
                     {QStringLiteral("content"), QStringLiteral("{}")},
                     {QStringLiteral("expectedSha256"), QString(64, QLatin1Char('a'))},
                     {QStringLiteral("fileIdentityToken"), QStringLiteral("token")}}),
            stepFor(QStringLiteral("mods.setEnabled"),
                    {{QStringLiteral("fileName"), QStringLiteral("x.jar")},
                     {QStringLiteral("enabled"), false}}),
            stepFor(QStringLiteral("logs.search"),
                    {{QStringLiteral("query"), QStringLiteral("ERROR")}})};
        verifiedPlan.last().verify = true;
        check(workflow.submitPlan(QStringLiteral("修一下"), verifiedPlan, &planProblem),
              "Auto-safe：计划提交成功", planProblem);
        workflow.approvePlan(true);
        check(workflow.planApproved(), "Auto-safe：计划已批准");

        FakeWriteEngine engine;
        AgentCompositeToolEngine composite;
        composite.addEngine(&engine);
        composite.setWorkflow(&workflow);
        composite.setAutoSafe(&authorizer);

        // 计划里没有 args.extraJvm：硬禁止（不消耗计划步骤）。
        AgentToolResult result = composite.invoke(callFor(
            QStringLiteral("settings.set"),
            {{QStringLiteral("key"), QStringLiteral("args.extraJvm")}}));
        check(result.status == AgentToolResult::Status::Error &&
                  result.data.value(QStringLiteral("autoSafeDenied")).toBool(),
              "Auto-safe：args.extraJvm 被直接拒绝", result.summary);
        check(engine.resolveCalls == 0, "Auto-safe：被拒的调用没有触发批准");
        check(workflow.nextStepIndex() == 0, "Auto-safe：被拒的调用不消耗计划步骤");

        // 计划内的设置键：替用户点"允许"。
        result = composite.invoke(callFor(
            QStringLiteral("settings.set"),
            {{QStringLiteral("key"), QStringLiteral("memory.xmx")},
             {QStringLiteral("value"), QStringLiteral("6144")}}));
        check(result.status == AgentToolResult::Status::Ok && engine.resolveCalls == 1,
              "Auto-safe：计划内的设置写入不再弹审批", result.summary);
        check(workflow.nextStepIndex() == 1, "Auto-safe：执行过的步骤推进计划");

        // 计划外的 options.txt：仍弹审批，并写明原因。
        result = composite.invoke(callFor(
            QStringLiteral("fs.write"),
            {{QStringLiteral("path"), QStringLiteral("options.txt")},
             {QStringLiteral("content"), QStringLiteral("x")},
             {QStringLiteral("expectedSha256"), QString(64, QLatin1Char('b'))},
             {QStringLiteral("fileIdentityToken"), QStringLiteral("token")},
             {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}}));
        check(result.status == AgentToolResult::Status::ApprovalRequired,
              "Auto-safe：计划外的 options.txt 仍弹审批", result.summary);
        check(!result.approval.value(QStringLiteral("autoSafeReason")).toString().isEmpty(),
              "Auto-safe：审批卡说明为什么没自动放行");
        engine.cancelPending();

        result = composite.invoke(callFor(
            QStringLiteral("fs.write"),
            {{QStringLiteral("path"), QStringLiteral("config/a.toml")},
             {QStringLiteral("content"), QStringLiteral("{}")},
             {QStringLiteral("expectedSha256"), QString(64, QLatin1Char('a'))},
             {QStringLiteral("fileIdentityToken"), QStringLiteral("token")},
             {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}}));
        check(result.status == AgentToolResult::Status::Ok && engine.resolveCalls == 2,
              "Auto-safe：计划内的配置写入不再弹审批", result.summary);

        result = composite.invoke(callFor(
            QStringLiteral("mods.setEnabled"),
            {{QStringLiteral("fileName"), QStringLiteral("x.jar")},
             {QStringLiteral("enabled"), false},
             {QStringLiteral("workspaceId"), QStringLiteral("ws-1")}}));
        check(result.status == AgentToolResult::Status::Ok && engine.resolveCalls == 3,
              "Auto-safe：计划内的模组启停不再弹审批", result.summary);

        // 外发确认不归 Auto-safe 管。
        result = composite.invoke(callFor(QStringLiteral("mods.analyzeCompatibility")));
        check(result.status == AgentToolResult::Status::ApprovalRequired,
              "Auto-safe：外发确认不归它管（NetworkRead 仍要审批）");
        engine.cancelPending();

        result = composite.invoke(callFor(
            QStringLiteral("repair.finish"),
            {{QStringLiteral("transactionId"), QStringLiteral("tx-1")}}));
        check(result.status == AgentToolResult::Status::Ok,
              "Auto-safe：事务提交不再弹审批", result.summary);
        check(result.data.contains(QStringLiteral("autoSafeAudit")),
              "Auto-safe：事务结束把审计挂进结果");
        check(!authorizer.enabled(), "Auto-safe：事务结束后授权失效");
    }
}

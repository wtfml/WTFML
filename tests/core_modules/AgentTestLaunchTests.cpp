/**
 * @file AgentTestLaunchTests.cpp
 * @brief 测试启动：里程碑规则、参数解析、审批与异步结果交付。
 */

#include "Support.hpp"

#include "agent/AgentLaunchHost.hpp"
#include "agent/AgentTestLaunch.hpp"
#include "agent/AgentTestLaunchToolEngine.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace {

Agent::ToolCall callFor(const QJsonObject &arguments) {
    Agent::ToolCall call;
    call.id = QStringLiteral("call-test-launch");
    call.name = QStringLiteral("instance.testLaunch");
    call.arguments = arguments;
    return call;
}

class FakeLaunchHost final : public AgentLaunchHost {
public:
    bool startTestLaunch(const QString &instanceId, const QString &workspaceId,
                         const AgentTestLaunchOptions &options, const Completion &done,
                         QString *problem) override {
        Q_UNUSED(problem);
        started = true;
        lastInstanceId = instanceId;
        lastWorkspaceId = workspaceId;
        lastOptions = options;
        completion = done;
        return true;
    }
    void cancelTestLaunch() override { cancelled = true; }

    bool started = false;
    bool cancelled = false;
    QString lastInstanceId;
    QString lastWorkspaceId;
    AgentTestLaunchOptions lastOptions;
    Completion completion;
};

}  // namespace

void runAgentTestLaunchTests() {
    using M = AgentTestLaunchMilestones;
    check(M::isMainMenuLine(QStringLiteral("[Render thread/INFO]: Sound engine started")),
          "测试启动：主菜单标记");
    check(M::isMainMenuLine(QStringLiteral("OpenAL initialized")),
          "测试启动：OpenAL 也是主菜单标记");
    check(!M::isMainMenuLine(QStringLiteral("Created: 1024x512 textures-atlas")),
          "测试启动：纹理图集日志不算主菜单");
    check(M::isWorldLoadedLine(QStringLiteral("Preparing spawn area: 100%")),
          "测试启动：世界加载标记");
    check(M::isCrashLine(QStringLiteral("---- Minecraft Crash Report ----")),
          "测试启动：崩溃报告标记");
    check(M::isCrashLine(QStringLiteral("java.lang.OutOfMemoryError: Java heap space")),
          "测试启动：OOM 标记");
    check(!M::isCrashLine(QStringLiteral("[Mod] caught an exception and continued")),
          "测试启动：普通异常日志不算崩溃");
    check(M::validMilestone(QStringLiteral("main_menu")) &&
              !M::validMilestone(QStringLiteral("menu")),
          "测试启动：里程碑枚举");
    check(M::normalizeMilestone(QStringLiteral("WORLD_LOADED")) ==
              QStringLiteral("world_loaded"),
          "测试启动：里程碑归一化");

    AgentTestLaunchOptions options;
    QString problem;
    check(AgentTestLaunchToolEngine::parseOptions({}, &options, &problem),
          "测试启动：空参数用默认值", problem);
    check(options.timeoutSeconds == 120 && options.settleSeconds == 10 && options.repeat == 1,
          "测试启动：默认超时/观察/重复");
    check(options.milestone == QStringLiteral("main_menu") && !options.expectCrash,
          "测试启动：默认里程碑与期望");

    const QJsonObject args{
        {QStringLiteral("timeoutSeconds"), 30},
        {QStringLiteral("settleSeconds"), 0},
        {QStringLiteral("milestone"), QStringLiteral("world_loaded")},
        {QStringLiteral("repeat"), 2},
        {QStringLiteral("expect"), QStringLiteral("crash")},
        {QStringLiteral("quickPlay"), QJsonObject{{QStringLiteral("world"), QStringLiteral("TestWorld")}}},
    };
    check(AgentTestLaunchToolEngine::parseOptions(args, &options, &problem),
          "测试启动：完整参数解析", problem);
    check(options.timeoutSeconds == 30 && options.settleSeconds == 0 && options.repeat == 2,
          "测试启动：数值参数生效");
    check(options.milestone == QStringLiteral("world_loaded") && options.expectCrash,
          "测试启动：枚举参数生效");
    check(options.quickPlayWorld == QStringLiteral("TestWorld"), "测试启动：世界名生效");

    check(!AgentTestLaunchToolEngine::parseOptions(
              QJsonObject{{QStringLiteral("timeoutSeconds"), 0}}, &options, &problem),
          "测试启动：超时下限");
    check(!AgentTestLaunchToolEngine::parseOptions(
              QJsonObject{{QStringLiteral("milestone"), QStringLiteral("world_loaded")}},
              &options, &problem),
          "测试启动：world_loaded 必须带世界");
    check(!AgentTestLaunchToolEngine::parseOptions(
              QJsonObject{{QStringLiteral("quickPlay"),
                           QJsonObject{{QStringLiteral("world"), QStringLiteral("../x")}}}},
              &options, &problem),
          "测试启动：世界名拒绝路径");

    check(!AgentToolRegistry::find(QStringLiteral("instance.testLaunch")).name.isEmpty(),
          "测试启动：工具已注册");
    const AgentToolSpec spec = AgentToolRegistry::find(QStringLiteral("instance.testLaunch"));
    check(spec.risk == AgentRisk::Command, "测试启动：风险等级是逐次执行");

    FakeLaunchHost host;
    AgentTestLaunchToolEngine engine(&host, [](QString *problem) {
        if (problem != nullptr) problem->clear();
        AgentToolContext context;
        context.instanceId = QStringLiteral("1.20.1-forge");
        context.instanceName = QStringLiteral("测试实例");
        context.sessionId = QStringLiteral("session-1");
        return context;
    });

    const Agent::ToolCall call = callFor(
        QJsonObject{{QStringLiteral("timeoutSeconds"), 20},
                    {QStringLiteral("settleSeconds"), 0}});
    AgentToolResult result = engine.invoke(call);
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "测试启动：先要审批");
    const QString approvalId = result.approval.value(QStringLiteral("approvalId")).toString();
    check(!approvalId.isEmpty(), "测试启动：审批 id 非空");
    check(result.approval.value(QStringLiteral("tool")).toString() ==
              QStringLiteral("instance.testLaunch"),
          "测试启动：审批卡工具名");

    result = engine.resolveApproval(approvalId, true);
    check(result.status == AgentToolResult::Status::Pending, "测试启动：批准后进入等待");
    check(host.started && host.lastInstanceId == QStringLiteral("1.20.1-forge"),
          "测试启动：宿主收到实例");
    check(host.lastOptions.timeoutSeconds == 20, "测试启动：宿主收到超时");

    bool delivered = false;
    AgentToolResult deliveredResult;
    engine.setCompletionSink([&](const Agent::ToolCall &, const AgentToolResult &value) {
        delivered = true;
        deliveredResult = value;
    });
    AgentTestLaunchSummary summary;
    summary.ok = true;
    summary.launchId = QStringLiteral("launch-1");
    summary.workspaceId = QStringLiteral("ws-1");
    summary.workspaceKept = true;
    AgentTestLaunchRunResult run;
    run.processStarted = true;
    run.milestoneReached = true;
    run.stoppedAfterSettle = true;
    run.startupPhase = QStringLiteral("main_menu");
    run.logRefs = {QStringLiteral("launch-1/latest.log")};
    summary.runs.append(run);
    host.completion(summary);
    check(delivered, "测试启动：结果异步交付");
    check(deliveredResult.status == AgentToolResult::Status::Ok,
          "测试启动：成功结果状态");
    check(deliveredResult.data.value(QStringLiteral("ok")).toBool(),
          "测试启动：结果里有 ok");
    check(deliveredResult.data.value(QStringLiteral("workspaceId")).toString() ==
              QStringLiteral("ws-1"),
          "测试启动：结果里有工作区 id");
    check(deliveredResult.data.value(QStringLiteral("evidence")).toArray().size() == 1,
          "测试启动：日志引用生成证据");

    result = engine.invoke(call);
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "测试启动：第二次仍要审批");
    const QString secondId = result.approval.value(QStringLiteral("approvalId")).toString();
    result = engine.resolveApproval(secondId, false);
    check(result.error.kind == Agent::ErrorKind::ApprovalDenied,
          "测试启动：拒绝不启动进程");
    check(!host.cancelled, "测试启动：拒绝不调用取消");
}

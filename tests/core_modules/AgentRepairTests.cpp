/**
 * @file AgentRepairTests.cpp
 * @brief 修复事务：基线/差异、事务日志、工具审批与异步交付。
 */

#include "Support.hpp"

#include "agent/AgentLaunchHost.hpp"
#include "agent/AgentRepairHost.hpp"
#include "agent/AgentRepairToolEngine.hpp"
#include "agent/AgentTestLaunch.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

Agent::ToolCall callFor(const QString &name, const QJsonObject &arguments = {}) {
    Agent::ToolCall call;
    call.id = QStringLiteral("call-repair");
    call.name = name;
    call.arguments = arguments;
    return call;
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

class FakeRepairHost final : public AgentRepairHost {
public:
    bool beginTransaction(const QString &instanceId, const QStringList &scopes,
                          const BeginCompletion &done, QString *problem) override {
        Q_UNUSED(problem);
        beginInstance = instanceId;
        beginScopes = scopes;
        beginDone = done;
        return true;
    }
    bool finishTransaction(const QString &transactionId, bool commit,
                           const FinishCompletion &done, QString *problem) override {
        Q_UNUSED(problem);
        finishTransactionId = transactionId;
        finishCommit = commit;
        finishDone = done;
        return true;
    }

    QString beginInstance;
    QStringList beginScopes;
    BeginCompletion beginDone;
    QString finishTransactionId;
    bool finishCommit = false;
    FinishCompletion finishDone;
};

class FakeLaunchHost final : public AgentLaunchHost {
public:
    bool startTestLaunch(const QString &instanceId, const QString &workspaceId,
                         const AgentTestLaunchOptions &options, const Completion &done,
                         QString *problem) override {
        Q_UNUSED(problem);
        launchInstance = instanceId;
        launchWorkspace = workspaceId;
        launchOptions = options;
        launchDone = done;
        return true;
    }
    void cancelTestLaunch() override {}

    QString launchInstance;
    QString launchWorkspace;
    AgentTestLaunchOptions launchOptions;
    Completion launchDone;
};

AgentToolContext testContext() {
    AgentToolContext context;
    context.instanceId = QStringLiteral("1.20.1-forge");
    context.instanceName = QStringLiteral("测试实例");
    context.sessionId = QStringLiteral("session-repair");
    return context;
}

}  // namespace

void runAgentRepairTests() {
    QTemporaryDir temp;
    check(temp.isValid(), "事务：临时目录可建");
    if (!temp.isValid()) return;

    // ---------- 基线与差异 ----------
    const QString source = temp.filePath(QStringLiteral("source"));
    const QString workspace = temp.filePath(QStringLiteral("workspace"));
    writeFile(source + QStringLiteral("/mods/a.jar"), "a");
    writeFile(source + QStringLiteral("/config/x.json"), "{\"x\":1}");
    writeFile(source + QStringLiteral("/options.txt"), "renderDistance:8\n");
    const QStringList scopes{QStringLiteral("mods"), QStringLiteral("config"),
                             QStringLiteral("options")};
    QString problem;
    const QJsonObject baseline =
        AgentRepairTransactionStore::captureBaseline(source, scopes, &problem);
    check(problem.isEmpty() && baseline.size() == 3, "事务：基线有 3 个文件", problem);

    writeFile(workspace + QStringLiteral("/mods/a.jar"), "changed");
    writeFile(workspace + QStringLiteral("/mods/b.jar"), "new");
    writeFile(workspace + QStringLiteral("/options.txt"), "renderDistance:12\n");
    QDir().remove(workspace + QStringLiteral("/config/x.json"));
    const QList<AgentRepairFileChange> changes =
        AgentRepairTransactionStore::diff(workspace, baseline, scopes, &problem);
    check(problem.isEmpty() && changes.size() == 4, "事务：差异有 4 项", problem);
    int added = 0, modified = 0, deleted = 0;
    for (const auto &change : changes) {
        if (change.kind == AgentRepairFileChange::Kind::Added) ++added;
        if (change.kind == AgentRepairFileChange::Kind::Modified) ++modified;
        if (change.kind == AgentRepairFileChange::Kind::Deleted) ++deleted;
    }
    check(added == 1 && modified == 2 && deleted == 1,
          "事务：新增/修改/删除分类正确");

    // ---------- 事务日志 ----------
    AgentRepairTransactionStore store(temp.filePath(QStringLiteral("transactions")));
    AgentRepairTransactionRecord record;
    record.id = AgentRepairTransactionStore::newId();
    record.instanceId = QStringLiteral("1.20.1-forge");
    record.workspaceId = QStringLiteral("ws-test");
    record.state = QStringLiteral("ready");
    record.scopes = scopes;
    record.baseline = baseline;
    check(store.create(record, &problem), "事务：日志创建", problem);
    AgentRepairTransactionRecord loaded;
    check(store.load(record.id, &loaded, &problem), "事务：日志读取", problem);
    check(loaded.baseline.size() == 3, "事务：基线往返");
    check(store.activeForInstance(record.instanceId, &loaded, &problem),
          "事务：活动事务可查");
    check(store.recordSetting(record.id, QStringLiteral("memory.xmx"), 2048, 4096, &problem),
          "事务：设置增量可记", problem);
    check(store.load(record.id, &loaded, &problem) &&
              loaded.settings.value(QStringLiteral("memory.xmx")).toObject()
                      .value(QStringLiteral("new")).toInt() == 4096,
          "事务：设置增量落盘");

    for (const QString &name : {QStringLiteral("repair.begin"), QStringLiteral("repair.diff"),
                                QStringLiteral("repair.verify"), QStringLiteral("repair.finish"),
                                QStringLiteral("repair.status")}) {
        check(!AgentToolRegistry::find(name).name.isEmpty(), "事务：工具已注册 " + name);
    }

    // ---------- 工具：begin 审批/异步 ----------
    FakeRepairHost repairHost;
    FakeLaunchHost launchHost;
    AgentRepairToolEngine engine(&repairHost, &launchHost, [](QString *contextProblem) {
        if (contextProblem != nullptr) contextProblem->clear();
        return testContext();
    });
    AgentToolResult result = engine.invoke(callFor(
        QStringLiteral("repair.begin"),
        QJsonObject{{QStringLiteral("scopes"), QJsonArray{QStringLiteral("mods"), QStringLiteral("options")}}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired, "事务：begin 先审批");
    const QString beginApproval = result.approval.value(QStringLiteral("approvalId")).toString();
    check(!beginApproval.isEmpty(), "事务：begin 审批 id");
    result = engine.resolveApproval(beginApproval, true);
    check(result.status == AgentToolResult::Status::Pending, "事务：begin 进入等待");
    check(repairHost.beginScopes == QStringList({QStringLiteral("mods"), QStringLiteral("options")}),
          "事务：范围传到宿主");

    bool delivered = false;
    AgentToolResult deliveredResult;
    engine.setCompletionSink([&](const Agent::ToolCall &, const AgentToolResult &value) {
        delivered = true;
        deliveredResult = value;
    });
    AgentRepairBeginResult beginResult;
    beginResult.ok = true;
    beginResult.transactionId = QStringLiteral("tx-test");
    beginResult.workspaceId = QStringLiteral("ws-test");
    beginResult.baselineBytes = 42;
    repairHost.beginDone(beginResult);
    check(delivered && deliveredResult.status == AgentToolResult::Status::Ok,
          "事务：begin 结果异步交付");
    check(deliveredResult.data.value(QStringLiteral("workspaceId")).toString() ==
              QStringLiteral("ws-test"),
          "事务：begin 返回工作区 id");

    // ---------- 工具：verify 记录到事务 ----------
    AgentRepairTransactionRecord verifyRecord = record;
    verifyRecord.id = AgentRepairTransactionStore::newId();
    verifyRecord.workspaceId = QStringLiteral("ws-verify");
    verifyRecord.state = QStringLiteral("ready");
    verifyRecord.verification = {};
    check(engine.handles(QStringLiteral("repair.verify")), "事务：verify 工具已注册");
    // 使用默认 store 的根，写一份工具能读到的事务。
    AgentRepairTransactionStore defaultStore;
    check(defaultStore.create(verifyRecord, &problem), "事务：verify 夹具创建", problem);

    delivered = false;
    engine.setCompletionSink([&](const Agent::ToolCall &, const AgentToolResult &value) {
        delivered = true;
        deliveredResult = value;
    });
    result = engine.invoke(callFor(QStringLiteral("repair.verify"),
                                   QJsonObject{{QStringLiteral("transactionId"),
                                                verifyRecord.id}}));
    check(result.status == AgentToolResult::Status::Pending, "事务：verify 进入等待");
    check(launchHost.launchWorkspace == QStringLiteral("ws-verify"), "事务：verify 用工作区");
    AgentTestLaunchSummary summary;
    summary.ok = true;
    summary.launchId = QStringLiteral("launch-verify");
    summary.workspaceId = QStringLiteral("ws-verify");
    AgentTestLaunchRunResult run;
    run.milestoneReached = true;
    run.stoppedAfterSettle = true;
    run.startupPhase = QStringLiteral("main_menu");
    summary.runs.append(run);
    launchHost.launchDone(summary);
    check(delivered && deliveredResult.status == AgentToolResult::Status::Ok,
          "事务：verify 结果交付");
    check(defaultStore.load(verifyRecord.id, &loaded, &problem) &&
              loaded.state == QStringLiteral("ready_to_commit"),
          "事务：验证通过后状态 ready_to_commit");

    // ---------- 工具：finish commit 审批 ----------
    delivered = false;
    result = engine.invoke(callFor(QStringLiteral("repair.finish"),
                                   QJsonObject{{QStringLiteral("transactionId"), verifyRecord.id},
                                               {QStringLiteral("action"), QStringLiteral("commit")}}));
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "事务：commit 先审批");
    const QString finishApproval = result.approval.value(QStringLiteral("approvalId")).toString();
    result = engine.resolveApproval(finishApproval, true);
    check(result.status == AgentToolResult::Status::Pending, "事务：commit 进入等待");
    check(repairHost.finishCommit && repairHost.finishTransactionId == verifyRecord.id,
          "事务：commit 传到宿主");
    AgentRepairFinishResult finishResult;
    finishResult.ok = true;
    finishResult.committed = true;
    finishResult.transactionId = verifyRecord.id;
    finishResult.changedFiles = {QStringLiteral("mods/a.jar")};
    repairHost.finishDone(finishResult);
    check(delivered && deliveredResult.status == AgentToolResult::Status::Ok,
          "事务：commit 结果交付");
    check(deliveredResult.data.value(QStringLiteral("committed")).toBool(),
          "事务：commit 结果标记已提交");
}

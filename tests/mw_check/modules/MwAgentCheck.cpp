/**
 * @file MwAgentChecks.cpp
 * @brief 真 MainWindow + 假模型客户端：智能体页存在、发消息→流式→完成→落盘、停止、无明文 key。
 */

#include "Support.hpp"

#include "AgentApprovalCard.hpp"
#include "AgentPage.hpp"
#include "AgentToolCard.hpp"
#include "AgentSettingsPage.hpp"
#include "OverlayHost.hpp"
#include "agent/AgentPaths.hpp"
#include "util/AppPaths.hpp"
#include "agent/AgentSessionStore.hpp"
#include "controllers/AgentController.hpp"
#include "agent/AgentModelClient.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDialog>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QUuid>

namespace {

class FakeAgentModelClient : public AgentModelClient {
public:
    explicit FakeAgentModelClient(QObject *parent = nullptr) : AgentModelClient(parent) {}

    void start(const Agent::ModelRequest &request) override {
        starts += 1;
        lastRequest = request;
        const bool hold = holdNext;
        holdNext = false;
        if (toolCallNext && toolRounds == 0) {
            ++toolRounds;
            QTimer::singleShot(0, this, [this, request] {
                Agent::ModelReply reply;
                reply.message.role = Agent::Role::Assistant;
                reply.message.status = Agent::MessageStatus::Complete;
                Agent::ToolCall call;
                call.id = QStringLiteral("mw-call-1");
                call.name = QStringLiteral("fs.read");
                call.arguments = QJsonObject{{QStringLiteral("path"),
                                              QStringLiteral("config/a.toml")}};
                reply.message.toolCalls.append(call);
                emit finished(request.requestId, reply);
            });
            return;
        }
        QTimer::singleShot(0, this, [this, request, hold] {
            emit textDelta(request.requestId, QStringLiteral("收到："));
            emit textDelta(request.requestId, QStringLiteral("启动日志结论"));
            if (hold) return;
            Agent::ModelReply reply;
            reply.message.role = Agent::Role::Assistant;
            reply.message.content = QStringLiteral("收到：启动日志结论");
            reply.message.status = Agent::MessageStatus::Complete;
            reply.usage = Agent::Usage{12, 5, false};
            emit finished(request.requestId, reply);
        });
    }
    void cancel(const QString &requestId) override {
        cancelled.append(requestId);
    }
    void testConnection(const Agent::ModelRequest &request) override {
        emit connectionTested(request.requestId, true, 200, QStringLiteral("连接成功（假）。"));
    }

    int starts = 0;
    int toolRounds = 0;
    bool holdNext = false;
    bool toolCallNext = false;
    QStringList cancelled;
    Agent::ModelRequest lastRequest;
};

bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 3000) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

QStringList filesUnder(const QString &root) {
    QStringList files;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) files.append(it.next());
    return files;
}

}  // namespace

int MwAgentChecks(QApplication &app) {
    Q_UNUSED(app);
    MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    QTabWidget *tabs = window.findChild<QTabWidget *>(MainWindowUI::tabWidgetName());
    check(tabs != nullptr && tabs->count() == 6, "真窗口：六个选项卡（含工具箱）");
    AgentPage *page = window.findChild<AgentPage *>(QStringLiteral("agentPage"));
    check(page != nullptr && window.ui()->getAgentPage() == page,
          "真窗口：智能体页由 MainWindowUI 持有");
    AgentController *controller = window.agentController();
    check(controller != nullptr, "真窗口：AgentController 已接线");

    FakeAgentModelClient fake;
    controller->setModelClientForTest(&fake);
    AgentProviderProfile profile;
    profile.id = QStringLiteral("mw-test");
    profile.name = QStringLiteral("假服务");
    profile.baseUrl = QStringLiteral("https://api.example.com/v1");
    profile.model = QStringLiteral("fake-model");
    const QString secret = QStringLiteral("sk-mw-secret-1234567890");
    controller->applyProfileForTest(profile, secret);

    check(page != nullptr && page->composer() != nullptr, "真窗口：输入框在");
    controller->sendForTest(QStringLiteral("启动就崩溃，帮我看看"));
    {
        AgentSession immediate;
        QString immediateProblem;
        controller->sessionStore()->load(controller->currentSessionId(), &immediate,
                                         &immediateProblem);
        check(immediate.meta.title.contains(QStringLiteral("启动就崩溃")),
              "真窗口：发送后标题立即写入盘中", immediate.meta.title);
    }
    check(waitFor([&] {
              return controller->loop() != nullptr &&
                     controller->loop()->state() == AgentLoop::State::Completed;
          }),
          "真窗口：假模型流式完成后 Loop 进入 Completed");
    check(fake.starts == 1, "真窗口：只发一次请求");
    check(!fake.lastRequest.apiKey.isEmpty() && fake.lastRequest.apiKey == secret,
          "真窗口：请求构造时带上 key（只在内存里）");
    check(fake.lastRequest.systemPrompt.contains(QStringLiteral("no repair transactions")),
          "真窗口：系统提示说明无工具权限");

    AgentSession loaded;
    QString problem;
    check(controller->sessionStore()->load(controller->currentSessionId(), &loaded, &problem),
          "真窗口：会话可从盘上读回", problem);
    check(loaded.messages.size() == 2 && loaded.messages.constFirst().role == Agent::Role::User &&
              loaded.messages.constLast().role == Agent::Role::Assistant &&
              loaded.messages.constLast().content.contains(QStringLiteral("启动日志结论")),
          "真窗口：用户消息 + 完整助手回复都落了盘",
          QString::number(loaded.messages.size()));
    check(loaded.meta.title.contains(QStringLiteral("启动就崩溃")),
          "真窗口：标题取自第一条用户消息", loaded.meta.title);

    // key 不得出现在会话目录/元数据/日志里。
    bool sawSecret = false;
    for (const QString &path : filesUnder(controller->sessionStore()->root())) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.readAll().contains(secret.toUtf8())) {
            sawSecret = true;
        }
    }
    check(!sawSecret, "真窗口：会话目录 grep 不到明文 API Key");

    // 停止：半截流 + 取消，保留部分内容并标中断。
    fake.holdNext = true;
    controller->sendForTest(QStringLiteral("再来一次"));
    check(waitFor([&] {
              return controller->loop()->state() == AgentLoop::State::Requesting;
          }),
          "真窗口：第二次请求进入 Requesting");
    check(waitFor([&] {
              return !controller->loop()->assistantMessage().content.isEmpty();
          }),
          "真窗口：收到部分流式文本");
    controller->cancelForTest();
    check(controller->loop()->state() == AgentLoop::State::Cancelled,
          "真窗口：停止后进入 Cancelled");
    check(!fake.cancelled.isEmpty(), "真窗口：停止会取消在飞请求");
    check(controller->sessionStore()->load(controller->currentSessionId(), &loaded, &problem),
          "真窗口：停止后的会话仍可读回");
    check(loaded.messages.constLast().status == Agent::MessageStatus::Interrupted,
          "真窗口：停止后最后一条标为中断");

    // Stage 1B：实例锁定 + 文件读审批 + 工具卡 + 活动会话删除。
    {
        const QString root = QDir::tempPath() + QStringLiteral("/wtfml-agent-mw-instance-") +
                             QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir().mkpath(root + QStringLiteral("/config"));
        QFile config(root + QStringLiteral("/config/a.toml"));
        check(config.open(QIODevice::WriteOnly), "真窗口：造一个实例配置文件", config.fileName());
        config.write("maxMemory = 4096\n");
        config.close();
        AgentInstanceInfo info;
        info.id = QStringLiteral("mw-inst-1");
        info.name = QStringLiteral("MW 实例");
        info.gameRootId = AgentInstanceLocks::gameRootId(root);
        controller->setInstanceContextForTest(root, info);
        fake.toolCallNext = true;
        fake.toolRounds = 0;
        controller->sendForTest(QStringLiteral("看看配置"));
        check(waitFor([&] {
                  return controller->loop()->state() == AgentLoop::State::AwaitingApproval;
              }),
              "真窗口：文件读需要审批，Loop 暂停");
        check(page->findChild<AgentApprovalCard *>() != nullptr,
              "真窗口：审批卡显示在智能体页");
        AgentSession lockedSession;
        QString lockProblem;
        check(controller->sessionStore()->load(controller->currentSessionId(), &lockedSession,
                                               &lockProblem) &&
                  lockedSession.meta.instanceLock.id == info.id,
              "真窗口：首次文件工具前锁定实例并落盘", lockProblem);
        check(page->findChild<QPushButton *>(QStringLiteral("agentApprovalSessionAllow")) == nullptr ||
                  !page->findChild<QPushButton *>(QStringLiteral("agentApprovalSessionAllow"))->isVisible(),
              "真窗口：没有会话级目录放行");
        controller->approveToolForTest(true);
        check(waitFor([&] {
                  return controller->loop()->state() == AgentLoop::State::Completed;
              }),
              "真窗口：审批通过后模型继续并完成");
        check(page->findChild<AgentToolCard *>() != nullptr,
              "真窗口：工具结果卡片存在");
        AgentSession readSession;
        controller->sessionStore()->load(controller->currentSessionId(), &readSession, nullptr);
        bool hasToolMessage = false;
        for (const Agent::Message &message : readSession.messages) {
            if (message.role == Agent::Role::Tool &&
                message.content.contains(QStringLiteral("4096"))) {
                hasToolMessage = true;
            }
        }
        check(hasToolMessage, "真窗口：工具结果进了会话正文");

        // 待审批时取消并删除：不执行工具，删除先取消。
        fake.toolCallNext = true;
        fake.toolRounds = 0;
        controller->sendForTest(QStringLiteral("再读一次"));
        waitFor([&] { return controller->loop()->state() == AgentLoop::State::AwaitingApproval; });
        const QString toDelete = controller->currentSessionId();
        controller->cancelForTest();
        check(controller->deleteSessionForTest(toDelete), "真窗口：取消后可删除活动会话");
        AgentSession gone;
        check(!controller->sessionStore()->load(toDelete, &gone, nullptr),
              "真窗口：删除后会话不可读回");
        QDir(root).removeRecursively();
    }

    // Stage 2：模式切换 + 启动器工具（真 MainWindow 里的宿主）+ 计划闸。
    {
        // 宿主按 AppPaths::versionDir() 找实例目录，所以这里造在同一个位置 —— 真窗口里
        // 两条路（会话上下文 vs 启动器工具宿主）必须指向同一个实例。
        const QString root = AppPaths::versionDir(QStringLiteral("mw-stage2"));
        QDir().mkpath(root + QStringLiteral("/mods"));
        QFile mod(root + QStringLiteral("/mods/example.jar"));
        check(mod.open(QIODevice::WriteOnly), "真窗口：造一个模组文件", mod.fileName());
        mod.write("jar");
        mod.close();
        AgentInstanceInfo info;
        info.id = QStringLiteral("mw-stage2");
        info.name = QStringLiteral("Stage 2 实例");
        info.gameRootId = AgentInstanceLocks::gameRootId(root);
        controller->setInstanceContextForTest(root, info);

        // 先有一个会话：模式记在会话元数据里，工具也要有会话才能取上下文。
        fake.toolCallNext = false;
        fake.toolRounds = 0;
        controller->sendForTest(QStringLiteral("Stage 2 模式测试"));
        check(waitFor([&] { return controller->loop()->state() == AgentLoop::State::Completed; }),
              "真窗口：Stage 2 用例先建立一个会话");
        controller->chooseSkillForTest(QStringLiteral("inspection"));
        check(controller->workflow()->skill().id == QStringLiteral("inspection"),
              "真窗口：切到实例体检模式");
        check(page->findChild<QPushButton *>(QStringLiteral("agentSkill_inspection")) != nullptr,
              "真窗口：模式卡片跟着换");
        AgentSession skillSession;
        controller->sessionStore()->load(controller->currentSessionId(), &skillSession, nullptr);
        check(skillSession.meta.skill == QStringLiteral("inspection"),
              "真窗口：会话记住当前模式");

        // 只读模式：写工具在复合层就被拦下，不会走到工具执行。
        Agent::ToolCall write;
        write.id = QStringLiteral("mw-stage2-write");
        write.name = QStringLiteral("mods.setEnabled");
        write.arguments = QJsonObject{{QStringLiteral("fileName"), QStringLiteral("example.jar")},
                                      {QStringLiteral("enabled"), false}};
        const AgentToolResult blocked = controller->compositeEngine()->invoke(write);
        check(blocked.status == AgentToolResult::Status::Error,
              "真窗口：只读模式拒绝写工具");
        check(QFile::exists(root + QStringLiteral("/mods/example.jar")),
              "真窗口：被拒的写工具没有动盘");

        // 启动器工具走真宿主：模组概况只给数量，不给文件名。
        Agent::ToolCall modsList;
        modsList.id = QStringLiteral("mw-stage2-mods");
        modsList.name = QStringLiteral("mods.list");
        const AgentToolResult modsResult = controller->compositeEngine()->invoke(modsList);
        check(modsResult.status == AgentToolResult::Status::Ok, "真窗口：mods.list 走真宿主");
        check(modsResult.data.value(QStringLiteral("mods")).toInt() == 1,
              "真窗口：真宿主数出 1 个模组",
              QString::number(modsResult.data.value(QStringLiteral("mods")).toInt()));
        check(!modsResult.data.contains(QStringLiteral("names")),
              "真窗口：模组概况不带文件名");

        // 结束：回到自由对话，别影响后面的用例。
        controller->chooseSkillForTest(QStringLiteral("general"));
        check(controller->workflow()->skill().id == QStringLiteral("general"),
              "真窗口：切回自由对话");
        QDir(root).removeRecursively();
    }

    // Stage 2A：MCP 管理器和工具引擎都接上了；没配置服务器时调用被拒、不会起进程。
    {
        check(controller->mcpManager() != nullptr, "真窗口：MCP 管理器已接线");
        check(controller->mcpManager()->servers().isEmpty() ||
                  !controller->mcpManager()->anyReady(),
              "真窗口：没有服务器自动连上");
        Agent::ToolCall mcpCall;
        mcpCall.id = QStringLiteral("mw-mcp-1");
        mcpCall.name = QStringLiteral("mcp__none__ping");
        mcpCall.arguments = QJsonObject{};
        const AgentToolResult mcpResult = controller->compositeEngine()->invoke(mcpCall);
        check(mcpResult.status == AgentToolResult::Status::Error,
              "真窗口：未知 MCP 工具被拒");
        check(controller->mcpManager()->tools().isEmpty(), "真窗口：没有 MCP 工具时清单为空");
    }

    // 崩溃报告与游戏日志在实例目录里（游戏以 --gameDir versions/<id> 启动）。
    {
        const QString instanceRoot = AppPaths::versionDir(QStringLiteral("mw-stage2"));
        QDir().mkpath(instanceRoot + QStringLiteral("/crash-reports"));
        QDir().mkpath(instanceRoot + QStringLiteral("/logs"));
        QFile crash(instanceRoot + QStringLiteral("/crash-reports/crash-2026-01-01_00.00.00-client.txt"));
        check(crash.open(QIODevice::WriteOnly), "真窗口：造一份实例崩溃报告", crash.fileName());
        crash.write("java.lang.OutOfMemoryError: Java heap space\n");
        crash.close();
        QFile gameLog(instanceRoot + QStringLiteral("/logs/latest.log"));
        check(gameLog.open(QIODevice::WriteOnly), "真窗口：造一份实例 latest.log",
              gameLog.fileName());
        gameLog.write("[12:00:00] [main/INFO]: instance log\n");
        gameLog.close();

        Agent::ToolCall crashCall;
        crashCall.id = QStringLiteral("mw-crash-1");
        crashCall.name = QStringLiteral("crash.list");
        crashCall.arguments = QJsonObject{{QStringLiteral("detail"), QStringLiteral("full")}};
        const AgentToolResult crashList = controller->compositeEngine()->invoke(crashCall);
        check(crashList.status == AgentToolResult::Status::ApprovalRequired,
              "真窗口：崩溃报告文件名清单要审批");
        const AgentToolResult crashApproved = controller->compositeEngine()->resolveApproval(
            crashList.approval.value(QStringLiteral("approvalId")).toString(), true);
        check(crashApproved.status == AgentToolResult::Status::Ok,
              "真窗口：crash.list 读得到实例目录里的崩溃报告",
              crashApproved.summary);
        check(crashApproved.summary.contains(QStringLiteral("1 份")),
              "真窗口：只统计到 1 份崩溃报告", crashApproved.summary);

        Agent::ToolCall logCall;
        logCall.id = QStringLiteral("mw-log-1");
        logCall.name = QStringLiteral("logs.tail");
        logCall.arguments = QJsonObject{{QStringLiteral("lines"), 20}};
        const AgentToolResult logResult = controller->compositeEngine()->invoke(logCall);
        check(logResult.status == AgentToolResult::Status::Ok,
              "真窗口：logs.tail 读得到实例目录的 latest.log", logResult.summary);
        check(logResult.data.value(QStringLiteral("content")).toString().contains(
                  QStringLiteral("instance log")),
              "真窗口：读到的是实例目录那份日志");

        // Stage 5.3：崩溃快照与按行检索走真宿主、读的是实例目录里的文件。
        Agent::ToolCall snapshotCall;
        snapshotCall.id = QStringLiteral("mw-crash-2");
        snapshotCall.name = QStringLiteral("diagnostics.crashSnapshot");
        const AgentToolResult snapshot = controller->compositeEngine()->invoke(snapshotCall);
        check(snapshot.status == AgentToolResult::Status::Ok,
              "真窗口：crashSnapshot 读得到实例目录的崩溃报告", snapshot.summary);
        check(snapshot.data.value(QStringLiteral("crash"))
                  .toObject()
                  .value(QStringLiteral("exception"))
                  .toString() == QStringLiteral("java.lang.OutOfMemoryError"),
              "真窗口：crashSnapshot 认出顶层异常");

        Agent::ToolCall searchCall;
        searchCall.id = QStringLiteral("mw-log-2");
        searchCall.name = QStringLiteral("logs.search");
        searchCall.arguments = QJsonObject{{QStringLiteral("query"), QStringLiteral("instance log")}};
        const AgentToolResult searchResult = controller->compositeEngine()->invoke(searchCall);
        check(searchResult.status == AgentToolResult::Status::Ok,
              "真窗口：logs.search 走真宿主", searchResult.summary);
        check(searchResult.data.value(QStringLiteral("totalMatches")).toInt() == 1 &&
                  searchResult.data.value(QStringLiteral("matches"))
                          .toArray()
                          .first()
                          .toObject()
                          .value(QStringLiteral("line"))
                          .toInt() == 1,
              "真窗口：logs.search 带正确的行号");

        Agent::ToolCall rangeCall;
        rangeCall.id = QStringLiteral("mw-log-3");
        rangeCall.name = QStringLiteral("logs.readRange");
        rangeCall.arguments = QJsonObject{{QStringLiteral("startLine"), 1},
                                          {QStringLiteral("lineCount"), 5}};
        const AgentToolResult rangeResult = controller->compositeEngine()->invoke(rangeCall);
        check(rangeResult.status == AgentToolResult::Status::Ok,
              "真窗口：logs.readRange 走真宿主", rangeResult.summary);
        check(rangeResult.data.value(QStringLiteral("totalLines")).toInt() == 1,
              "真窗口：logs.readRange 回总行数");

        // Stage 5.4/5.5/5.6：自由对话的显式白名单里没有修复工具，切到修复模式才放行。
        Agent::ToolCall installCall;
        installCall.id = QStringLiteral("mw-mod-1");
        installCall.name = QStringLiteral("mods.installFix");
        installCall.arguments = QJsonObject{
            {QStringLiteral("projectId"), QStringLiteral("proj-x")},
            {QStringLiteral("workspaceId"), QStringLiteral("ws-unknown")}};
        const AgentToolResult installBlocked =
            controller->compositeEngine()->invoke(installCall);
        check(installBlocked.status == AgentToolResult::Status::Error,
              "真窗口：自由对话不放行 mods.installFix", installBlocked.summary);
        check(installBlocked.summary.contains(QStringLiteral("不允许")),
              "真窗口：被拒原因写的是模式白名单", installBlocked.summary);

        Agent::ToolCall bisectCall;
        bisectCall.id = QStringLiteral("mw-mod-2");
        bisectCall.name = QStringLiteral("mods.bisect");
        bisectCall.arguments = QJsonObject{
            {QStringLiteral("workspaceId"), QStringLiteral("ws-unknown")}};
        const AgentToolResult bisectBlocked =
            controller->compositeEngine()->invoke(bisectCall);
        check(bisectBlocked.status == AgentToolResult::Status::Error,
              "真窗口：自由对话不放行 mods.bisect", bisectBlocked.summary);

        Agent::ToolCall repairBlockedCall;
        repairBlockedCall.id = QStringLiteral("mw-core-0");
        repairBlockedCall.name = QStringLiteral("instance.repairCore");
        const AgentToolResult repairBlocked =
            controller->compositeEngine()->invoke(repairBlockedCall);
        check(repairBlocked.status == AgentToolResult::Status::Error,
              "真窗口：自由对话不放行 instance.repairCore", repairBlocked.summary);

        // Auto-safe（S5.7）：只在修复模式可开；换模式即关。
        controller->chooseSkillForTest(QStringLiteral("crash-rescue"));
        auto *autoSafeButton =
            page->findChild<QPushButton *>(QStringLiteral("agentAutoSafeButton"));
        check(autoSafeButton != nullptr && !autoSafeButton->isHidden(),
              "真窗口：崩溃急救模式下 Auto-safe 可见");
        if (autoSafeButton != nullptr) autoSafeButton->click();
        check(page->autoSafeEnabled(), "真窗口：点开 Auto-safe 后是勾选态");
        controller->chooseSkillForTest(QStringLiteral("general"));
        check(autoSafeButton != nullptr && autoSafeButton->isHidden() &&
                  !page->autoSafeEnabled(),
              "真窗口：回到自由对话后 Auto-safe 收起并关闭");

        // 切到模组维护：修复工具才进白名单；仍要求事务工作区/审批。
        controller->chooseSkillForTest(QStringLiteral("mods"));
        const AgentToolResult installInMode =
            controller->compositeEngine()->invoke(installCall);
        check(installInMode.status == AgentToolResult::Status::Error &&
                  !installInMode.summary.contains(QStringLiteral("不允许")),
              "真窗口：模组维护模式放行 mods.installFix（此时缺的是已批准计划）",
              installInMode.summary);
        const AgentToolResult bisectInMode =
            controller->compositeEngine()->invoke(bisectCall);
        check(bisectInMode.status == AgentToolResult::Status::Error &&
                  !bisectInMode.summary.contains(QStringLiteral("不允许")),
              "真窗口：模组维护模式放行 mods.bisect（此时缺的是已批准计划）",
              bisectInMode.summary);
        Agent::ToolCall repairCall;
        repairCall.id = QStringLiteral("mw-core-1");
        repairCall.name = QStringLiteral("instance.repairCore");
        const AgentToolResult repairResult =
            controller->compositeEngine()->invoke(repairCall);
        check(repairResult.status == AgentToolResult::Status::Error &&
                  repairResult.summary.contains(QStringLiteral("计划")),
              "真窗口：修复模式里 instance.repairCore 要先有已批准计划",
              repairResult.summary);
        controller->compositeEngine()->cancelPending();
        controller->chooseSkillForTest(QStringLiteral("general"));

        Agent::ToolCall fullCall;
        fullCall.id = QStringLiteral("mw-crash-3");
        fullCall.name = QStringLiteral("crash.readFull");
        fullCall.arguments = QJsonObject{
            {QStringLiteral("reportId"), QStringLiteral("crash-2026-01-01_00.00.00-client.txt")}};
        const AgentToolResult fullResult = controller->compositeEngine()->invoke(fullCall);
        check(fullResult.status == AgentToolResult::Status::Ok,
              "真窗口：crash.readFull 读得到实例目录里的报告", fullResult.summary);
        check(!fullResult.data.value(QStringLiteral("artifactId")).toString().isEmpty(),
              "真窗口：crash.readFull 存了 artifact");
    }

    // Stage 3：联网与诊断命令都接上了；诊断命令默认关闭，联网工具每次都要审批。
    {
        Agent::ToolCall commandCall;
        commandCall.id = QStringLiteral("mw-cmd-1");
        commandCall.name = QStringLiteral("command.diagnose");
        commandCall.arguments = QJsonObject{{QStringLiteral("command"),
                                             QStringLiteral("java-version")}};
        const AgentToolResult commandResult = controller->compositeEngine()->invoke(commandCall);
        check(commandResult.status == AgentToolResult::Status::Error,
              "真窗口：诊断命令默认关闭");
        Agent::ToolCall fetchCall;
        fetchCall.id = QStringLiteral("mw-web-1");
        fetchCall.name = QStringLiteral("web.fetch");
        fetchCall.arguments = QJsonObject{{QStringLiteral("url"),
                                           QStringLiteral("https://example.com/")}};
        const AgentToolResult fetchResult = controller->compositeEngine()->invoke(fetchCall);
        check(fetchResult.status == AgentToolResult::Status::ApprovalRequired,
              "真窗口：联网抓取要审批");
        controller->compositeEngine()->cancelPending();
    }

    // 「API 设置」被别的页占着覆盖层时也要打得开（用户报的「点了没反应」：别的选项卡上开的页收着、仍占着这一位，
    // showPage() 返回 false，按钮静默无事发生）。
    {
        OverlayHost *overlay = window.ui()->overlay();
        check(overlay != nullptr, "真窗口：有覆盖层");
        if (overlay != nullptr) {
            auto *other = new QDialog;
            QPointer<QDialog> otherGuard(other);
            check(overlay->showPage(other, QStringLiteral("别的页")), "真窗口：先开一页占住覆盖层");
            tabs->setCurrentIndex(MainWindowUI::homeTabIndex());  // 页所属的选项卡以外 = 收着
            QCoreApplication::processEvents();
            tabs->setCurrentWidget(page);
            QCoreApplication::processEvents();
            QPushButton *apiButton = page->findChild<QPushButton *>(QStringLiteral("agentApiSettingsButton"));
            check(apiButton != nullptr, "真窗口：找得到「API 设置」钮");
            if (apiButton != nullptr) apiButton->click();
            check(waitFor([&] { return window.findChild<AgentSettingsPage *>() != nullptr && overlay->pageOpen(); }),
                  "真窗口：覆盖层被别的页占着时点「API 设置」照样打开");
            check(otherGuard.isNull() || !otherGuard->isVisible(), "真窗口：占着的那页被静默收掉");
            overlay->evict();
            QCoreApplication::processEvents();
        }
    }

    // r109：空「新对话」被选中后切走即丢（108 加的功能在真 exe 上没生效）。
    // 真机复现：列表里一条空「新对话」+ 一条有消息的会话；点空会话（主区空态）再点有消息那条，
    // 空会话还在列表里，盘上 `<id>.meta.json` 的修改时间也没动。这一节走**真鼠标**：按下 + 松开
    // 两发打到列表视口上（切换会话只认 `QListWidget::itemClicked`），断言盘上和列表里都干净。
    {
        AgentSessionStore *store = controller->sessionStore();
        QListWidget *list = page->findChild<QListWidget *>(QStringLiteral("agentSessionList"));
        check(list != nullptr, "空会话：会话列表在");
        QPushButton *plus = page->findChild<QPushButton *>(QStringLiteral("agentNewSessionButton"));
        check(plus != nullptr, "空会话：「+」钮在");
        if (list != nullptr && plus != nullptr) {
            const auto clickSession = [&](const QString &id) {
                for (int i = 0; i < list->count(); ++i) {
                    QListWidgetItem *row = list->item(i);
                    if (row->data(Qt::UserRole).toString() != id) continue;
                    list->scrollToItem(row);
                    QCoreApplication::processEvents();
                    const QPoint local = list->visualItemRect(row).center();
                    const QPointF global(list->viewport()->mapToGlobal(local));
                    ::QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), global,
                                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(list->viewport(), &press);
                    ::QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), global,
                                          Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(list->viewport(), &release);
                    QCoreApplication::processEvents();
                    return true;
                }
                return false;
            };
            const auto hasMeta = [&](const QString &id) {
                QList<AgentSessionMeta> metas;
                store->list(&metas, nullptr);
                for (const AgentSessionMeta &meta : metas) {
                    if (meta.id == id) return true;
                }
                return false;
            };
            const auto hasRow = [&](const QString &id) {
                for (int i = 0; i < list->count(); ++i) {
                    if (list->item(i)->data(Qt::UserRole).toString() == id) return true;
                }
                return false;
            };
            const auto makeSession = [&](const QString &title, const QString &line) {
                AgentSession made;
                QString makeProblem;
                if (!store->create(&made, &makeProblem)) return QString();
                Agent::Message mine;
                mine.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                mine.role = Agent::Role::User;
                mine.content = line;
                mine.status = Agent::MessageStatus::Complete;
                mine.createdAt = QDateTime::currentDateTimeUtc();
                made.messages.append(mine);
                made.meta.title = title;
                if (!store->save(made, false, &makeProblem)) return QString();
                return made.meta.id;
            };

            const QString withMessages =
                makeSession(QStringLiteral("有消息的会话"), QStringLiteral("这条有消息，切走不该被丢"));
            check(!withMessages.isEmpty(), "空会话：造一条有消息的会话");

            // ① 「+」建一条空「新对话」并选中（产品的正常入口）。
            plus->click();
            QCoreApplication::processEvents();
            const QString blankId = controller->currentSessionId();
            check(!blankId.isEmpty() && blankId != withMessages, "空会话：「+」后停在新会话上", blankId);
            check(!blankId.isEmpty() && hasMeta(blankId) && hasRow(blankId),
                  "空会话：新会话在盘上和列表里都在");

            // ② 点有消息的那条：空「新对话」应当当场丢掉。
            check(clickSession(withMessages), "空会话：点得到有消息那行");
            check(controller->currentSessionId() == withMessages, "空会话：切到了有消息的会话");
            check(!blankId.isEmpty() && !hasMeta(blankId), "空会话：空「新对话」从盘上没了", blankId);
            check(!blankId.isEmpty() && !hasRow(blankId), "空会话：空「新对话」从列表里没了");
            check(!QFileInfo::exists(AgentPaths::sessionMetaPath(blankId)),
                  "空会话：`<id>.meta.json` 不在了");

            // ③ 再建一条空「新对话」，这次先改个名：改过名的不算「没动过」。
            plus->click();
            QCoreApplication::processEvents();
            const QString renamedId = controller->currentSessionId();
            check(!renamedId.isEmpty() && renamedId != withMessages, "空会话：又停在新会话上", renamedId);
            QMetaObject::invokeMethod(page, "sessionRenameRequested", Qt::DirectConnection,
                                      Q_ARG(QString, renamedId),
                                      Q_ARG(QString, QStringLiteral("我改过名")));
            QCoreApplication::processEvents();
            AgentSession named;
            QString namedProblem;
            check(store->load(renamedId, &named, &namedProblem) &&
                      named.meta.title == QStringLiteral("我改过名"),
                  "空会话：改名落到了盘上", namedProblem);

            // ④ 从改过名的空会话切走：留着。
            check(clickSession(withMessages), "空会话：点回有消息那行");
            check(hasMeta(renamedId), "空会话：改过名的空会话不丢");
            check(hasRow(renamedId), "空会话：改过名的空会话还在列表里");

            // ⑤ 从有消息的会话切走：也留着。
            check(clickSession(renamedId), "空会话：点回改过名那行");
            check(controller->currentSessionId() == renamedId, "空会话：切到了改过名的会话");
            check(hasMeta(withMessages), "空会话：有消息的会话不丢");
            check(hasRow(withMessages), "空会话：有消息的会话还在列表里");

            // ⑥ 「+」不再攒第二条空「新对话」：当前会话有内容、盘上已有现成的空会话时切过去，不再新建
            //    （r104 的「列表里两条空会话」就是这个判断一次都没成立过）。
            AgentSession extra;
            QString extraProblem;
            check(store->create(&extra, &extraProblem), "空会话：盘上再摆一条空「新对话」", extraProblem);
            check(clickSession(withMessages), "空会话：点回有消息那行（好让「+」走找现成空会话那支）");
            plus->click();
            QCoreApplication::processEvents();
            check(controller->currentSessionId() == extra.meta.id,
                  "空会话：「+」复用盘上现成的空会话", controller->currentSessionId());
        }
    }

    controller->shutdown();
    check(controller->loop()->state() == AgentLoop::State::Cancelled ||
              !controller->loop()->busy(),
          "真窗口：shutdown 后不再 busy");
    return 0;
}

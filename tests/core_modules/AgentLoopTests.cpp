/**
 * @file AgentLoopTests.cpp
 * @brief 无工具 loop：文本→完成、重试、取消、部分流不自动重发、无 key 报错。
 */

#include "Support.hpp"

#include "agent/AgentContextBuilder.hpp"
#include "agent/AgentLoop.hpp"
#include "agent/AgentModelClient.hpp"
#include "agent/AgentTool.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <QTranslator>

namespace {

class EnglishUiTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (qstrcmp(context, "UiLanguage") == 0 && qstrcmp(sourceText, "zh") == 0) {
            return QStringLiteral("en");
        }
        return {};
    }
};

class FakeModelClient : public AgentModelClient {
public:
    explicit FakeModelClient(QObject *parent = nullptr) : AgentModelClient(parent) {}

    void start(const Agent::ModelRequest &request) override {
        starts += 1;
        requests.append(request);
        const int mode = behaviours.isEmpty() ? 0 : behaviours.takeFirst();
        if (useTools && toolRounds < toolRoundLimit) {
            ++toolRounds;
            QTimer::singleShot(0, this, [this, request] {
                Agent::ModelReply reply;
                reply.message.role = Agent::Role::Assistant;
                reply.message.content.clear();
                reply.message.status = Agent::MessageStatus::Complete;
                Agent::ToolCall call;
                call.id = QStringLiteral("call-1");
                call.name = QStringLiteral("fs.list");
                // 每轮换一个路径：默认同一个参数会被 `AgentLoop` 的去重（kRepeatReuseWindow）
                // 直接复用，测不到"不设限"那一条。
                call.arguments = QJsonObject{
                    {QStringLiteral("path"), QStringLiteral("dir-%1").arg(toolRounds)}};
                reply.message.toolCalls.append(call);
                emit finished(request.requestId, reply);
            });
            return;
        }
        QTimer::singleShot(0, this, [this, request, mode] {
            if (mode == 1) {
                emit failed(request.requestId,
                            Agent::Error{Agent::ErrorKind::Network, QStringLiteral("断网"),
                                         0, -1, true});
                return;
            }
            if (mode == 2) {
                emit textDelta(request.requestId, QStringLiteral("半截"));
                emit failed(request.requestId,
                            Agent::Error{Agent::ErrorKind::Network, QStringLiteral("流中断"),
                                         0, -1, true});
                return;
            }
            emit textDelta(request.requestId, QStringLiteral("你"));
            emit textDelta(request.requestId, QStringLiteral("好"));
            Agent::ModelReply reply;
            reply.message.role = Agent::Role::Assistant;
            reply.message.content = QStringLiteral("你好");
            reply.message.status = Agent::MessageStatus::Complete;
            reply.usage = Agent::Usage{10, 2, false};
            emit finished(request.requestId, reply);
        });
    }
    void cancel(const QString &requestId) override { cancelled.append(requestId); }
    void testConnection(const Agent::ModelRequest &) override {}

    int starts = 0;
    int toolRounds = 0;
    /// 连续发几轮工具调用（默认 1 = 老行为：只发一次，之后收尾）。
    int toolRoundLimit = 1;
    bool useTools = false;
    QList<int> behaviours;
    QList<Agent::ModelRequest> requests;
    QStringList cancelled;
};

class FakeToolEngine : public AgentToolEngine {
public:
    QJsonArray schemas() const override {
        return QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("fs.list")}}}}};
    }
    AgentToolResult invoke(const Agent::ToolCall &) override {
        invokeCount += 1;
        AgentToolResult result;
        result.status = AgentToolResult::Status::ApprovalRequired;
        result.summary = QStringLiteral("需要审批");
        result.approval = QJsonObject{{QStringLiteral("approvalId"), QStringLiteral("a1")},
                                      {QStringLiteral("tool"), QStringLiteral("fs.list")}};
        return result;
    }
    AgentToolResult resolveApproval(const QString &, bool approved) override {
        resolvedCount += 1;
        AgentToolResult result;
        if (approved) {
            result.status = AgentToolResult::Status::Ok;
            result.summary = QStringLiteral("目录元数据");
            result.data = QJsonObject{{QStringLiteral("entries"), 2}};
        } else {
            result.status = AgentToolResult::Status::Error;
            result.summary = QStringLiteral("用户拒绝");
            result.error.kind = Agent::ErrorKind::ApprovalDenied;
            result.error.message = result.summary;
        }
        return result;
    }
    void cancelPending() override { cancelled = true; }
    bool handles(const QString &internalName) const override {
        return internalName == QStringLiteral("fs.list");
    }
    int invokeCount = 0;
    int resolvedCount = 0;
    bool cancelled = false;
};

/// 异步引擎：invoke 立刻回 Pending，结果从 completion sink 回来（MCP 就是这个形状）。
class AsyncFakeEngine : public AgentToolEngine {
public:
    QJsonArray schemas() const override {
        return QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("fs.list")}}}}};
    }
    AgentToolResult invoke(const Agent::ToolCall &) override {
        ++invokeCount;
        AgentToolResult result;
        result.status = AgentToolResult::Status::Pending;
        result.summary = QStringLiteral("等异步结果");
        return result;
    }
    AgentToolResult resolveApproval(const QString &, bool) override { return {}; }
    void cancelPending() override { cancelled = true; }
    bool handles(const QString &internalName) const override {
        return internalName == QStringLiteral("fs.list");
    }
    void setCompletionSink(AgentToolSink sink) override { m_sink = std::move(sink); }
    void deliver(const QString &toolName) {
        Agent::ToolCall call;
        call.id = QStringLiteral("call-1");
        call.name = toolName;
        AgentToolResult result;
        result.status = AgentToolResult::Status::Ok;
        result.summary = QStringLiteral("异步完成");
        result.data = QJsonObject{{QStringLiteral("entries"), 3}};
        if (m_sink) m_sink(call, result);
    }

    int invokeCount = 0;
    bool cancelled = false;
    AgentToolSink m_sink;
};

bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 2000) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

Agent::Message user(const QString &text) {
    Agent::Message message;
    message.id = QStringLiteral("u1");
    message.role = Agent::Role::User;
    message.content = text;
    message.status = Agent::MessageStatus::Complete;
    message.createdAt = QDateTime::currentDateTimeUtc();
    return message;
}

}  // namespace

void runAgentLoopTests() {
    {
        FakeModelClient client;
        AgentLoop loop(&client);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        config.systemPrompt = QStringLiteral("system");
        loop.setConfig(config);
        Agent::Message finished;
        QObject::connect(&loop, &AgentLoop::turnFinished, &loop,
                         [&finished](const Agent::Message &message) { finished = message; });
        loop.setHistory({user(QStringLiteral("hi"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::Completed; }),
              "Loop：文本流完成后进入 Completed");
        check(finished.content == QStringLiteral("你好") &&
                  finished.status == Agent::MessageStatus::Complete,
              "Loop：合并 delta 成完整回复", finished.content);
        check(loop.history().size() == 2 && loop.history().constLast().role == Agent::Role::Assistant,
              "Loop：完成后历史追加 assistant");
        check(client.starts == 1, "Loop：只发一次请求");
        check(client.requests.constFirst().systemPrompt ==
                  QStringLiteral("system\n\nInterface language: Simplified Chinese"),
              "Loop：系统提示动态注入当前界面语言", client.requests.constFirst().systemPrompt);

        // 切换界面翻译后，下一轮不改 config 也应即时带入新界面语言。
        EnglishUiTranslator translator;
        QCoreApplication::installTranslator(&translator);
        QList<Agent::Message> secondHistory = loop.history();
        secondHistory.append(user(QStringLiteral("hi again")));
        loop.setHistory(secondHistory);
        loop.start();
        check(waitFor([&] { return client.starts == 2 && loop.state() == AgentLoop::State::Completed; }),
              "Loop：切语言后第二轮完成");
        check(client.starts == 2 &&
                  client.requests.constLast().systemPrompt ==
                      QStringLiteral("system\n\nInterface language: English"),
              "Loop：切换语言后下一轮系统提示即时生效",
              client.requests.constLast().systemPrompt);
        QCoreApplication::removeTranslator(&translator);
    }
    {
        FakeModelClient client;
        client.behaviours = {1, 0};
        AgentLoop loop(&client);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setRetryDelaysForTest({0, 0});
        loop.setHistory({user(QStringLiteral("hi"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::Completed; }),
              "Loop：首字节前网络失败自动重试后完成");
        check(client.starts == 2, "Loop：重试一次", QString::number(client.starts));
    }
    {
        FakeModelClient client;
        client.behaviours = {2};
        AgentLoop loop(&client);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setRetryDelaysForTest({0, 0});
        loop.setHistory({user(QStringLiteral("hi"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::Failed; }),
              "Loop：流已开始后失败进入 Failed");
        check(client.starts == 1, "Loop：已有文本时不自动重发");
        check(loop.history().constLast().status == Agent::MessageStatus::Interrupted,
              "Loop：保留部分文本并标为中断");
        check(loop.history().constLast().content == QStringLiteral("半截"),
              "Loop：中断时保留已收到文本");
    }
    {
        FakeModelClient client;
        AgentLoop loop(&client);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("hi"))});
        loop.start();
        check(loop.busy(), "Loop：发出后处于 busy");
        loop.cancel();
        check(!loop.busy() && loop.state() == AgentLoop::State::Cancelled,
              "Loop：取消后立即回到非 busy / Cancelled");
        check(!client.cancelled.isEmpty(), "Loop：取消会通知客户端");
        check(loop.history().constLast().status == Agent::MessageStatus::Interrupted,
              "Loop：取消保留中断消息");
    }
    {
        FakeModelClient client;
        AgentLoop loop(&client);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey.clear();
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("hi"))});
        Agent::Error error;
        QObject::connect(&loop, &AgentLoop::turnFailed, &loop,
                         [&error](const Agent::Error &value) { error = value; });
        loop.start();
        check(error.kind == Agent::ErrorKind::NoConfiguration,
              "Loop：没有 API Key 时拒绝发送");
        check(client.starts == 0, "Loop：配置不完整时不发网络请求");
    }
    {
        FakeModelClient client;
        client.behaviours = {1, 0};
        AgentLoop loop(&client);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setRetryDelaysForTest({0, 0});
        loop.setHistory({user(QStringLiteral("hi"))});
        loop.start();
        waitFor([&] { return loop.state() == AgentLoop::State::Completed; });
        check(client.requests.size() == 2 &&
                  client.requests.at(1).messages.constLast().role == Agent::Role::User,
              "Loop：重试请求仍以同一条 user 结尾（不重复追加）");
    }
    {
        FakeModelClient client;
        client.useTools = true;
        FakeToolEngine engine;
        AgentLoop loop(&client);
        loop.setToolEngine(&engine);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("查一下目录"))});
        QString approvalId;
        QObject::connect(&loop, &AgentLoop::approvalRequested, &loop,
                         [&approvalId](const QString &id, const Agent::ToolCall &,
                                       const QJsonObject &) { approvalId = id; });
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::AwaitingApproval; }),
              "Loop 工具：有工具调用时暂停等待审批");
        check(engine.invokeCount == 1 && !approvalId.isEmpty(),
              "Loop 工具：审批请求带 id");
        check(!client.requests.constFirst().tools.isEmpty(),
              "Loop 工具：请求里带工具 schema");
        loop.resolveApproval(true);
        check(waitFor([&] { return loop.state() == AgentLoop::State::Completed; }),
              "Loop 工具：审批通过后继续请求并完成");
        check(engine.resolvedCount == 1, "Loop 工具：引擎收到审批结果");
        const QList<Agent::Message> history = loop.history();
        check(history.size() == 4, "Loop 工具：user + assistant(tool_calls) + tool + assistant",
              QString::number(history.size()));
        check(history.at(1).toolCalls.size() == 1 && history.at(2).role == Agent::Role::Tool &&
                  history.at(3).content == QStringLiteral("你好"),
              "Loop 工具：历史顺序正确");
        check(history.at(2).content.contains(QStringLiteral("目录元数据")),
              "Loop 工具：tool 结果回灌给模型");
    }
    {
        FakeModelClient client;
        client.useTools = true;
        FakeToolEngine engine;
        AgentLoop loop(&client);
        loop.setToolEngine(&engine);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("查一下目录"))});
        loop.start();
        waitFor([&] { return loop.state() == AgentLoop::State::AwaitingApproval; });
        loop.resolveApproval(false);
        check(waitFor([&] { return loop.state() == AgentLoop::State::Completed; }),
              "Loop 工具：拒绝后仍把拒绝事实回灌并完成");
        const QList<Agent::Message> history = loop.history();
        check(history.size() == 4 && history.at(2).status == Agent::MessageStatus::Error &&
                  history.at(2).content.contains(QStringLiteral("用户拒绝")),
              "Loop 工具：拒绝以 tool 错误结果回灌，不假装成功");
    }
    {
        FakeModelClient client;
        client.useTools = true;
        FakeToolEngine engine;
        AgentLoop loop(&client);
        loop.setToolEngine(&engine);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("查一下目录"))});
        loop.start();
        waitFor([&] { return loop.state() == AgentLoop::State::AwaitingApproval; });
        loop.cancel();
        check(loop.state() == AgentLoop::State::Cancelled && engine.cancelled,
              "Loop 工具：待审批时取消会清空 pending，不执行");
    }
    {
        // 单会话工具调用总数（设计 40）：跨回合累计，超过就不再执行工具。
        FakeModelClient client;
        client.useTools = true;
        FakeToolEngine engine;
        AgentLoop loop(&client);
        loop.setToolEngine(&engine);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        config.maxSessionToolCalls = 1;
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("查一下目录"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::AwaitingApproval; }),
              "Loop 会话上限：第一次工具调用照常审批");
        loop.resolveApproval(true);
        check(waitFor([&] { return loop.state() == AgentLoop::State::Completed; }),
              "Loop 会话上限：第一次工具调用完成");
        check(loop.sessionToolCalls() == 1, "Loop 会话上限：会话计数 +1",
              QString::number(loop.sessionToolCalls()));

        client.toolRounds = 0;  // 再让模型发一次工具调用
        loop.setHistory({user(QStringLiteral("再查一次"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::Failed; }),
              "Loop 会话上限：第二次工具调用被挡");
        check(loop.sessionToolCalls() == 1, "Loop 会话上限：被挡的那次不计入次数");
        bool sawLimit = false;
        for (const Agent::Message &message : loop.history()) {
            if (message.role == Agent::Role::Assistant &&
                message.errorText.contains(QStringLiteral("上限"))) {
                sawLimit = true;
            }
        }
        check(sawLimit, "Loop 会话上限：失败原因写清是会话上限");

        // 切会话时控制器写入已用次数：从 40 开始，第一条工具调用就挡住。
        FakeModelClient seeded;
        seeded.useTools = true;
        FakeToolEngine seededEngine;
        AgentLoop seededLoop(&seeded);
        seededLoop.setToolEngine(&seededEngine);
        seededLoop.setConfig(config);
        seededLoop.setSessionToolCalls(config.maxSessionToolCalls);
        seededLoop.setHistory({user(QStringLiteral("查一下目录"))});
        seededLoop.start();
        check(waitFor([&] { return seededLoop.state() == AgentLoop::State::Failed; }),
              "Loop 会话上限：切会话带过来的计数生效");
        check(seededEngine.invokeCount == 0, "Loop 会话上限：超限的工具一次都不执行");
    }
    {
        // 2026-11-27：工具调用不再有次数上限。默认配置连跑多轮也不该被挡住。
        FakeModelClient client;
        client.useTools = true;
        FakeToolEngine engine;
        AgentLoop loop(&client);
        loop.setToolEngine(&engine);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        check(config.maxToolSteps == 0 && config.maxSessionToolCalls == 0,
              "Loop 不设限：默认配置就是 0（0 = 不限）");
        loop.setConfig(config);
        client.toolRoundLimit = 45;  // 连发 45 轮，跨过原来的 40 次会话上限
        loop.setHistory({user(QStringLiteral("一直查"))});
        loop.start();
        int rounds = 0;
        while (rounds < 60) {
            if (loop.state() == AgentLoop::State::AwaitingApproval) {
                loop.resolveApproval(true);
                ++rounds;
                continue;
            }
            if (loop.state() == AgentLoop::State::Completed ||
                loop.state() == AgentLoop::State::Failed) {
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        check(loop.state() != AgentLoop::State::Failed,
              "Loop 不设限：撞不到「最大工具步数」/「会话上限」");
        check(loop.sessionToolCalls() > 40,
              "Loop 不设限：会话累计超过原来的 40 次也照样跑",
              QString::number(loop.sessionToolCalls()));
    }
    {
        // 同一工具 + 同一参数重复调用：第二次直接复用上次结果，不再走一次往返。
        FakeModelClient client;
        client.useTools = true;
        FakeToolEngine firstEngine;
        AgentLoop loop(&client);
        loop.setToolEngine(&firstEngine);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("查一次目录"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::AwaitingApproval; }),
              "Loop 去重：第一次调用照常审批");
        loop.resolveApproval(true);
        check(waitFor([&] { return loop.state() == AgentLoop::State::Completed; }),
              "Loop 去重：第一次调用完成");

        // 模型再发一次**完全一样**的调用：不去问执行器，直接抄上次的结果。
        client.toolRounds = 0;
        FakeToolEngine secondEngine;
        loop.setToolEngine(&secondEngine);
        loop.setHistory({user(QStringLiteral("再查一次同一个目录"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::Completed ||
                                    loop.state() == AgentLoop::State::AwaitingApproval; }),
              "Loop 去重：第二次请求回来了");
        check(secondEngine.invokeCount == 0,
              "Loop 去重：同参数同工具第二次没有真正执行",
              QString::number(secondEngine.invokeCount));
        bool sawReuse = false;
        for (const Agent::Message &message : loop.history()) {
            if (message.role == Agent::Role::Tool &&
                message.content.contains(QStringLiteral("reusedRepeat"))) {
                sawReuse = true;
            }
        }
        check(sawReuse, "Loop 去重：结果里标了 reusedRepeat");
    }
    {
        FakeModelClient client;
        client.useTools = true;
        AsyncFakeEngine engine;
        AgentLoop loop(&client);
        loop.setToolEngine(&engine);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("异步工具"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::ExecutingTool; }),
              "Loop 异步：异步工具进入执行中");
        check(loop.busy(), "Loop 异步：等待异步结果时仍算忙");
        engine.deliver(QStringLiteral("fs.list"));
        check(waitFor([&] { return loop.state() == AgentLoop::State::Completed; }),
              "Loop 异步：结果回来后这一轮完成");
        bool sawToolMessage = false;
        for (const Agent::Message &message : loop.history()) {
            if (message.role == Agent::Role::Tool &&
                message.content.contains(QStringLiteral("异步完成"))) {
                sawToolMessage = true;
            }
        }
        check(sawToolMessage, "Loop 异步：异步工具结果进了历史");
    }
    {
        FakeModelClient client;
        client.useTools = true;
        AsyncFakeEngine engine;
        AgentLoop loop(&client);
        loop.setToolEngine(&engine);
        AgentLoop::Config config;
        config.profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        config.profile.model = QStringLiteral("m");
        config.apiKey = QStringLiteral("k");
        loop.setConfig(config);
        loop.setHistory({user(QStringLiteral("异步工具然后取消"))});
        loop.start();
        check(waitFor([&] { return loop.state() == AgentLoop::State::ExecutingTool; }),
              "Loop 异步：取消前在等结果");
        loop.cancel();
        check(engine.cancelled, "Loop 异步：取消会通知引擎");
        engine.deliver(QStringLiteral("fs.list"));
        QCoreApplication::processEvents();
        check(loop.state() == AgentLoop::State::Cancelled,
              "Loop 异步：取消后的迟到结果被丢弃");
    }

}

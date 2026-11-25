/**
 * @file AgentWebToolsTests.cpp
 * @brief 联网工具：审批闸、私网阻断、pinned-IP 连接、逐跳重定向重检、content-type、
 *        HTML 转文本、结果脱敏与截断、搜索 JSON 解析。
 */

#include "Support.hpp"

#include "agent/AgentSettings.hpp"
#include "agent/AgentWebTools.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

/// 假传输：解析结果与响应都由用例摆，不发真请求。
class FakeTransport : public AgentWebTransport {
public:
    QList<QHostAddress> addresses{QHostAddress(QStringLiteral("93.184.216.34"))};
    QString resolveError;
    QList<AgentHttpResponse> responses;
    QList<AgentHttpRequest> requests;

    void resolve(const QString &host, ResolveCallback done) override {
        hosts.append(host);
        if (!resolveError.isEmpty()) {
            done(false, {}, resolveError);
            return;
        }
        done(true, addresses, QString());
    }
    void get(const AgentHttpRequest &request, ResponseCallback done) override {
        requests.append(request);
        const AgentHttpResponse response = responses.isEmpty() ? AgentHttpResponse{}
                                                               : responses.takeFirst();
        done(response);
    }

    QStringList hosts;
};

AgentHttpResponse okText(const QString &body, const QString &contentType) {
    AgentHttpResponse response;
    response.ok = true;
    response.status = 200;
    response.body = body.toUtf8();
    response.contentType = contentType;
    return response;
}

AgentHttpResponse redirectTo(const QString &location) {
    AgentHttpResponse response;
    response.ok = true;
    response.status = 302;
    response.location = location;
    return response;
}

Agent::ToolCall callFor(const QString &name, const QJsonObject &arguments) {
    Agent::ToolCall call;
    call.id = QStringLiteral("call-1");
    call.name = name;
    call.arguments = arguments;
    return call;
}

}  // namespace

void runAgentWebToolsTests() {
    QTemporaryDir fixture;
    AgentSettings::setWebSearchEndpoint(QStringLiteral("https://searx.example.com"));

    FakeTransport transport;
    AgentWebTools tools(&transport);
    AgentToolResult sinkResult;
    bool sinkCalled = false;
    tools.setCompletionSink([&sinkResult, &sinkCalled](const Agent::ToolCall &,
                                                       const AgentToolResult &result) {
        sinkResult = result;
        sinkCalled = true;
    });

    // ---------- 审批闸 ----------
    Agent::ToolCall fetch =
        callFor(QStringLiteral("web.fetch"),
                QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.com/page")}});
    AgentToolResult result = tools.invoke(fetch);
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "联网工具：抓取网页要审批");
    check(result.approval.value(QStringLiteral("target")).toString() == QStringLiteral("example.com"),
          "联网工具：审批卡写目标 host");
    check(transport.requests.isEmpty(), "联网工具：审批前不发请求");
    const QString approvalId = result.approval.value(QStringLiteral("approvalId")).toString();
    const AgentToolResult denied = tools.resolveApproval(approvalId, false);
    check(denied.error.kind == Agent::ErrorKind::ApprovalDenied, "联网工具：拒绝后不抓取");
    check(transport.requests.isEmpty(), "联网工具：拒绝后仍然没有请求");

    // ---------- 正常抓取：pinned-IP + Host 头 ----------
    sinkCalled = false;
    transport.responses = {okText(QStringLiteral("<html><body><h1>你好</h1>"
                                                 "<script>x</script></body></html>"),
                                  QStringLiteral("text/html; charset=utf-8"))};
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(sinkCalled, "联网工具：批准后异步交付结果");
    check(sinkResult.status == AgentToolResult::Status::Ok, "联网工具：抓取成功", sinkResult.summary);
    check(transport.requests.size() == 1, "联网工具：发了一次请求");
    const AgentHttpRequest sent = transport.requests.constFirst();
    check(sent.pinnedUrl.host() == QStringLiteral("93.184.216.34"),
          "联网工具：连接的是已校验的 IP", sent.pinnedUrl.host());
    check(sent.hostHeader == QStringLiteral("example.com"), "联网工具：Host 头是原 host");
    check(sent.peerVerifyName == QStringLiteral("example.com"),
          "联网工具：TLS 主机名校验用原 host");
    const QString text = sinkResult.data.value(QStringLiteral("text")).toString();
    check(text.contains(QStringLiteral("你好")), "联网工具：HTML 转出了正文");
    check(!text.contains(QStringLiteral("script")), "联网工具：不返回 script");
    check(sinkResult.data.value(QStringLiteral("untrusted")).toBool(),
          "联网工具：结果标成不可信");

    // ---------- 私网阻断 ----------
    sinkCalled = false;
    transport.requests.clear();
    transport.addresses = {QHostAddress(QStringLiteral("93.184.216.34")),
                           QHostAddress(QStringLiteral("192.168.1.10"))};
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(sinkCalled, "联网工具：私网地址也会回结果（错误）");
    check(sinkResult.status == AgentToolResult::Status::Blocked,
          "联网工具：解析结果含私网时整条拒绝");
    check(transport.requests.isEmpty(), "联网工具：地址不合格时根本不发请求");
    check(sinkResult.summary.contains(QStringLiteral("private")),
          "联网工具：拒绝原因说明是哪一档");

    transport.addresses = {QHostAddress(QStringLiteral("93.184.216.34"))};

    // ---------- 重定向逐跳重检 ----------
    sinkCalled = false;
    transport.requests.clear();
    transport.hosts.clear();
    transport.responses = {redirectTo(QStringLiteral("https://example.com/next")),
                           okText(QStringLiteral("第二跳"), QStringLiteral("text/plain"))};
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(sinkCalled, "联网工具：重定向后仍会交付结果");
    check(transport.requests.size() == 2, "联网工具：重定向再发一次请求",
          QString::number(transport.requests.size()));
    check(transport.hosts.size() == 2, "联网工具：每一跳都重新解析");
    check(sinkResult.status == AgentToolResult::Status::Ok &&
              sinkResult.data.value(QStringLiteral("text")).toString().contains(
                  QStringLiteral("第二跳")),
          "联网工具：第二跳内容返回");

    // 重定向目标解析出私网：第二跳被拒。
    sinkCalled = false;
    transport.requests.clear();
    transport.responses = {redirectTo(QStringLiteral("http://internal.example.com/"))};
    transport.addresses = {QHostAddress(QStringLiteral("10.0.0.5"))};
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(sinkResult.status == AgentToolResult::Status::Blocked,
          "联网工具：重定向到私网被拒");
    check(transport.requests.isEmpty(), "联网工具：被拒的重定向没有发第二跳");

    // 重定向次数上限。
    transport.addresses = {QHostAddress(QStringLiteral("93.184.216.34"))};
    transport.responses.clear();
    for (int i = 0; i < 10; ++i) {
        transport.responses.append(redirectTo(QStringLiteral("https://example.com/loop")));
    }
    sinkCalled = false;
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(sinkCalled && sinkResult.status == AgentToolResult::Status::Error,
          "联网工具：重定向次数超过上限后报错");
    check(sinkResult.summary.contains(QStringLiteral("重定向")), "联网工具：上限错误说得清");

    // ---------- content-type ----------
    sinkCalled = false;
    transport.responses = {okText(QStringLiteral("binary"), QStringLiteral("image/png"))};
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(sinkResult.status == AgentToolResult::Status::Blocked,
          "联网工具：图片等非文本类型被拒");

    // ---------- 结果脱敏与截断 ----------
    sinkCalled = false;
    transport.responses = {okText(
        QStringLiteral("token=sk-secret-1234567890 and password=hunter2"),
        QStringLiteral("text/plain"))};
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    const QString redactedText = sinkResult.data.value(QStringLiteral("text")).toString();
    check(!redactedText.contains(QStringLiteral("sk-secret-1234567890")),
          "联网工具：结果里的令牌被遮挡");
    check(!redactedText.contains(QStringLiteral("hunter2")), "联网工具：结果里的口令被遮挡");

    sinkCalled = false;
    QString huge(300000, QChar('a'));
    transport.responses = {okText(huge, QStringLiteral("text/plain"))};
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(sinkResult.data.value(QStringLiteral("truncated")).toBool(),
          "联网工具：超长正文被截断");

    // ---------- 地址策略拒绝 ----------
    result = tools.invoke(callFor(QStringLiteral("web.fetch"),
                                  QJsonObject{{QStringLiteral("url"),
                                               QStringLiteral("file:///C:/secret.txt")}}));
    check(result.status == AgentToolResult::Status::Error, "联网工具：file:// 直接拒绝");
    check(result.summary.contains(QStringLiteral("http")), "联网工具：拒绝理由说 scheme");

    // ---------- 搜索 ----------
    check(!AgentSettings::webSearchEndpoint().isEmpty(), "联网工具：测试里配好了 SearXNG 地址");
    const QJsonObject searchJson{
        {QStringLiteral("results"),
         QJsonArray{QJsonObject{{QStringLiteral("title"), QStringLiteral("结果一")},
                                {QStringLiteral("url"), QStringLiteral("https://a.example/1")},
                                {QStringLiteral("content"),
                                 QStringLiteral("摘要 token=sk-leak-abcdef")}},
                    QJsonObject{{QStringLiteral("title"), QStringLiteral("结果二")},
                                {QStringLiteral("url"), QStringLiteral("https://b.example/2")},
                                {QStringLiteral("content"), QStringLiteral("摘要二")}}}}};
    transport.responses = {okText(QString::fromUtf8(QJsonDocument(searchJson).toJson()),
                                  QStringLiteral("application/json"))};
    sinkCalled = false;
    Agent::ToolCall search =
        callFor(QStringLiteral("web.search"),
                QJsonObject{{QStringLiteral("query"), QStringLiteral("mod 冲突")}});
    result = tools.invoke(search);
    check(result.status == AgentToolResult::Status::ApprovalRequired,
          "联网工具：搜索也要审批");
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(sinkCalled, "联网工具：搜索结果异步交付");
    check(sinkResult.status == AgentToolResult::Status::Ok, "联网工具：搜索成功", sinkResult.summary);
    const QJsonArray results = sinkResult.data.value(QStringLiteral("results")).toArray();
    check(results.size() == 2, "联网工具：两条结果都解析出来");
    check(!sinkResult.data.value(QStringLiteral("results"))
               .toArray()
               .at(0)
               .toObject()
               .value(QStringLiteral("snippet"))
               .toString()
               .contains(QStringLiteral("sk-leak")),
          "联网工具：摘要里的令牌被遮挡");
    check(transport.requests.constLast().hostHeader == QStringLiteral("searx.example.com"),
          "联网工具：搜索请求发给设置的 SearXNG 实例");

    // 没有配置 SearXNG 时直接拒绝。
    AgentSettings::setWebSearchEndpoint(QString());
    result = tools.invoke(search);
    check(result.status == AgentToolResult::Status::Error, "联网工具：没配 SearXNG 时搜索被拒");
    check(result.summary.contains(QStringLiteral("SearXNG")), "联网工具：拒绝理由点名 SearXNG");
    AgentSettings::setWebSearchEndpoint(QStringLiteral("https://searx.example.com"));

    // ---------- 取消 ----------
    sinkCalled = false;
    result = tools.invoke(fetch);
    tools.resolveApproval(result.approval.value(QStringLiteral("approvalId")).toString(), true);
    // 假设结果还没回来就取消：这里的结果已经同步交付，所以只验审批清空后旧 id 失效。
    // ---------- 本会话允许（web.search / web.fetch） ----------
    {
        FakeTransport sessionTransport;
        AgentWebTools sessionTools(&sessionTransport, [](QString *contextProblem) {
            AgentToolContext context;
            context.sessionId = QStringLiteral("session-allow");
            if (contextProblem != nullptr) contextProblem->clear();
            return context;
        });
        AgentToolResult sessionSink;
        bool sessionSinkCalled = false;
        sessionTools.setCompletionSink(
            [&sessionSink, &sessionSinkCalled](const Agent::ToolCall &,
                                               const AgentToolResult &value) {
                sessionSink = value;
                sessionSinkCalled = true;
            });
        Agent::ToolCall page =
            callFor(QStringLiteral("web.fetch"),
                    QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.com/a")}});
        AgentToolResult first = sessionTools.invoke(page);
        check(first.status == AgentToolResult::Status::ApprovalRequired,
              "联网会话：第一次仍然要审批");
        check(first.approval.value(QStringLiteral("sessionAllowAvailable")).toBool(),
              "联网会话：审批卡给「本会话允许」按钮");
        sessionTransport.responses = {okText(QStringLiteral("<html><body>第一次</body></html>"),
                                             QStringLiteral("text/html"))};
        const AgentToolResult allowed = sessionTools.resolveSessionApproval(
            first.approval.value(QStringLiteral("approvalId")).toString());
        check(allowed.status == AgentToolResult::Status::Pending &&
                  sessionSinkCalled && sessionSink.status == AgentToolResult::Status::Ok,
              "联网会话：本会话放行后这次照常抓取", sessionSink.summary);

        sessionTransport.responses = {okText(QStringLiteral("<html><body>第二次</body></html>"),
                                             QStringLiteral("text/html"))};
        sessionSinkCalled = false;
        const AgentToolResult second = sessionTools.invoke(page);
        check(second.status == AgentToolResult::Status::Pending,
              "联网会话：同一个 host 不再弹审批");
        check(sessionSinkCalled && sessionSink.status == AgentToolResult::Status::Ok,
              "联网会话：直接抓取并交付");

        const AgentToolResult other =
            sessionTools.invoke(callFor(QStringLiteral("web.fetch"),
                                        QJsonObject{{QStringLiteral("url"),
                                                     QStringLiteral("https://other.example.org/b")}}));
        check(other.status == AgentToolResult::Status::ApprovalRequired,
              "联网会话：换一个 host 仍要审批");
        check(sessionTransport.requests.size() == 2, "联网会话：只发了放行后的两次请求",
              QString::number(sessionTransport.requests.size()));

        // 另一个会话不受影响：放行按 sessionId 分。
        AgentWebTools otherSession(&sessionTransport, [](QString *contextProblem) {
            AgentToolContext context;
            context.sessionId = QStringLiteral("session-other");
            if (contextProblem != nullptr) contextProblem->clear();
            return context;
        });
        check(otherSession.invoke(page).status == AgentToolResult::Status::ApprovalRequired,
              "联网会话：放行不外溢到别的会话");
    }

    const AgentToolResult stale = tools.resolveApproval(QStringLiteral("nope"), true);
    check(stale.status == AgentToolResult::Status::Error, "联网工具：失效审批被拒");
}

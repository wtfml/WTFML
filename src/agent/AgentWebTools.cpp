/**
 * @file AgentWebTools.cpp
 * @brief 联网工具的审批、pinned-IP 抓取与结果换算（见 .hpp）。
 */

#include "AgentWebTools.hpp"

#include "AgentApprovalPolicy.hpp"
#include "AgentRedaction.hpp"
#include "AgentSettings.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace {

QJsonObject stringField(int maxLength) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("maxLength"), maxLength}};
}

QJsonObject objectSchema(const QStringList &required, const QJsonObject &properties) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("required"), QJsonArray::fromStringList(required)},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

/// 「审批请求已失效」只有一句：两个审批入口（逐次 / 本会话）共用，别写第二份。
const QString &approvalExpiredText() {
    static const QString text = QStringLiteral("审批请求已失效。");
    return text;
}

QJsonObject webEgress(const QString &host) {
    return QJsonObject{{QStringLiteral("kind"), QStringLiteral("web")},
                       {QStringLiteral("decision"), QStringLiteral("confirm")},
                       {QStringLiteral("reason"),
                        QStringLiteral("请求会发给第三方站点 %1").arg(host)},
                       {QStringLiteral("redactionHits"), 0}};
}

}  // namespace

AgentWebTools::AgentWebTools(AgentWebTransport *transport, ContextProvider provider,
                             QObject *parent)
    : QObject(parent), m_transport(transport), m_provider(std::move(provider)) {}

void AgentWebTools::setTransport(AgentWebTransport *transport) {
    m_transport = transport;
}

QList<AgentToolSpec> AgentWebTools::specs() {
    QList<AgentToolSpec> list;
    list.append(AgentToolSpec{
        QStringLiteral("web.search"), QStringLiteral("联网搜索"),
        QStringLiteral("用设置里的 SearXNG 实例搜一次；搜索词会发给第三方，每次都要确认。"),
        AgentRisk::NetworkRead,
        objectSchema({QStringLiteral("query")},
                     {{QStringLiteral("query"), stringField(400)}})});
    list.append(AgentToolSpec{
        QStringLiteral("web.fetch"), QStringLiteral("抓取网页"),
        QStringLiteral("只抓 http/https 公网地址；全部 A/AAAA 校验、逐跳重定向重检、强制直连；"
                       "HTML 转文本后返回。"),
        AgentRisk::NetworkRead,
        objectSchema({QStringLiteral("url")},
                     {{QStringLiteral("url"), stringField(2048)}})});
    return list;
}

bool AgentWebTools::isWebTool(const QString &internalName) {
    return internalName == QStringLiteral("web.search") ||
           internalName == QStringLiteral("web.fetch");
}

QJsonArray AgentWebTools::schemas() const {
    QJsonArray array;
    for (const AgentToolSpec &spec : specs()) {
        array.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), AgentToolRegistry::wireName(spec.name)},
                         {QStringLiteral("description"), spec.description},
                         {QStringLiteral("parameters"), spec.schema}}}});
    }
    return array;
}

bool AgentWebTools::handles(const QString &internalName) const { return isWebTool(internalName); }

QString AgentWebTools::newRequestId() const {
    return QStringLiteral("web-%1").arg(m_counter++);
}

AgentToolResult AgentWebTools::errorResult(Agent::ErrorKind kind, const QString &message,
                                           const QJsonObject &data) const {
    AgentToolResult result;
    result.status = kind == Agent::ErrorKind::EgressBlocked ? AgentToolResult::Status::Blocked
                                                            : AgentToolResult::Status::Error;
    result.summary = message;
    result.error.kind = kind;
    result.error.message = message;
    result.data = data;
    return result;
}

AgentToolResult AgentWebTools::invoke(const Agent::ToolCall &call) {
    const QString internal = AgentToolRegistry::internalName(call.name);
    const AgentToolSpec toolSpec = AgentToolRegistry::find(internal);
    QString problem;
    if (!AgentToolRegistry::validateArguments(toolSpec.schema, call.arguments, &problem)) {
        return errorResult(Agent::ErrorKind::ToolError, problem);
    }
    if (m_transport == nullptr) {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("联网组件未接入。"));
    }
    AgentToolContext context;
    if (m_provider != nullptr) {
        QString contextProblem;
        context = m_provider(&contextProblem);
    }
    Approval approval;
    approval.call = call;
    approval.context = context;
    if (internal == QStringLiteral("web.search")) {
        approval.kind = Approval::Kind::Search;
        approval.query = call.arguments.value(QStringLiteral("query")).toString().trimmed();
        if (approval.query.isEmpty()) {
            return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("搜索词不能为空。"));
        }
        const QString endpoint = AgentSettings::webSearchEndpoint();
        if (endpoint.isEmpty()) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QStringLiteral("先在 API 设置里填 SearXNG 实例地址，才能联网搜索。"));
        }
        approval.url = QUrl(endpoint);
        const AgentWebPolicy::UrlCheck check = AgentWebPolicy::validateUrl(approval.url);
        if (!check.ok) {
            return errorResult(Agent::ErrorKind::ToolError,
                               QStringLiteral("SearXNG 地址不可用：%1").arg(check.problem));
        }
    } else if (internal == QStringLiteral("web.fetch")) {
        approval.kind = Approval::Kind::Fetch;
        approval.url = QUrl(call.arguments.value(QStringLiteral("url")).toString());
        const AgentWebPolicy::UrlCheck check = AgentWebPolicy::validateUrl(approval.url);
        if (!check.ok) return errorResult(Agent::ErrorKind::ToolError, check.problem);
    } else {
        return errorResult(Agent::ErrorKind::ToolError, QStringLiteral("未知工具。"));
    }
    // 本会话已经放行过同一个 host：不再弹审批，直接走同一段请求代码。
    const QString sessionHost = approval.url.host().toLower();
    if (!sessionHost.isEmpty() && sessionAllowed(context, sessionHost)) {
        return startApproved(approval);
    }
    const QString approvalId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_approvals.insert(approvalId, approval);
    const QString target = approval.url.host();
    const QString summary = approval.kind == Approval::Kind::Search
                                ? QStringLiteral("联网搜索：%1").arg(approval.query)
                                : QStringLiteral("抓取网页：%1").arg(approval.url.toString());
    QJsonObject request = AgentApprovalPolicy::request(
        call.name,
        approval.kind == Approval::Kind::Search ? QStringLiteral("联网搜索")
                                                : QStringLiteral("抓取网页"),
        target, summary, approval.url.toString(), QString(),
        QStringLiteral("只连公网地址；不跟随到内网；结果按不可信内容处理。"), webEgress(target));
    request.insert(QStringLiteral("approvalId"), approvalId);
    AgentToolResult result;
    result.status = AgentToolResult::Status::ApprovalRequired;
    result.summary = summary;
    result.approval = request;
    return result;
}

AgentToolResult AgentWebTools::resolveApproval(const QString &approvalId, bool approved) {
    if (!m_approvals.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, approvalExpiredText());
    }
    const Approval approval = m_approvals.take(approvalId);
    if (!approved) {
        AgentToolResult result =
            errorResult(Agent::ErrorKind::ApprovalDenied, QStringLiteral("用户拒绝了这次联网请求。"));
        result.data = QJsonObject{{QStringLiteral("denied"), true}};
        return result;
    }
    return startApproved(approval);
}

AgentToolResult AgentWebTools::resolveSessionApproval(const QString &approvalId) {
    if (!m_approvals.contains(approvalId)) {
        return errorResult(Agent::ErrorKind::ToolError, approvalExpiredText());
    }
    const Approval approval = m_approvals.take(approvalId);
    rememberSession(approval.context, approval.url.host().toLower());
    AgentToolResult result = startApproved(approval);
    if (result.status == AgentToolResult::Status::Pending) {
        result.summary = tr("本会话已放行 %1，正在联网获取…").arg(approval.url.host());
    }
    return result;
}

bool AgentWebTools::sessionAllowed(const AgentToolContext &context, const QString &host) const {
    if (context.sessionId.isEmpty() || host.isEmpty()) return false;
    return m_sessionHosts.value(context.sessionId).contains(host);
}

void AgentWebTools::rememberSession(const AgentToolContext &context, const QString &host) {
    if (context.sessionId.isEmpty() || host.isEmpty()) return;
    m_sessionHosts[context.sessionId].insert(host);
}

AgentToolResult AgentWebTools::startApproved(const Approval &approval) {
    const QString requestId = newRequestId();
    RunningFlight flight;
    flight.call = approval.call;
    flight.currentUrl = approval.url;
    flight.search = approval.kind == Approval::Kind::Search;
    flight.context = approval.context;
    m_running.insert(requestId, flight);
    startFetch(requestId, approval.url, 0, flight.search, approval.call, approval.context);
    AgentToolResult result;
    result.status = AgentToolResult::Status::Pending;
    result.summary = QStringLiteral("正在联网获取…");
    return result;
}

void AgentWebTools::startFetch(const QString &requestId, const QUrl &url, int redirects,
                               bool search, const Agent::ToolCall &call,
                               const AgentToolContext &context) {
    Q_UNUSED(call);
    Q_UNUSED(context);
    const AgentWebPolicy::UrlCheck check = AgentWebPolicy::validateUrl(url);
    if (!check.ok) {
        finish(requestId, errorResult(Agent::ErrorKind::ToolError, check.problem));
        return;
    }
    m_transport->resolve(check.host, [this, requestId, url, redirects, search](
                                         bool ok, const QList<QHostAddress> &addresses,
                                         const QString &error) {
        if (!m_running.contains(requestId)) return;
        if (!ok) {
            finish(requestId, errorResult(Agent::ErrorKind::Network,
                                          QStringLiteral("解析失败：%1").arg(error)));
            return;
        }
        QString addressProblem;
        if (!AgentWebPolicy::allAddressesAllowed(addresses, &addressProblem)) {
            finish(requestId, errorResult(Agent::ErrorKind::EgressBlocked, addressProblem));
            return;
        }
        RunningFlight &flight = m_running[requestId];
        flight.currentUrl = url;
        flight.redirects = redirects;
        AgentHttpRequest request;
        request.pinnedUrl = AgentWebPolicy::pinnedUrl(url, addresses.constFirst());
        request.hostHeader = hostHeaderFor(url);
        request.peerVerifyName = url.host();
        request.accept = search ? QStringLiteral("application/json")
                                : QStringLiteral("text/html,application/xhtml+xml,text/plain,"
                                                 "application/json;q=0.8,*/*;q=0.1");
        m_transport->get(request, [this, requestId](const AgentHttpResponse &response) {
            onResponse(requestId, response);
        });
    });
}

void AgentWebTools::onResponse(const QString &requestId, const AgentHttpResponse &response) {
    if (!m_running.contains(requestId)) return;
    const RunningFlight flight = m_running.value(requestId);
    if (flight.redirects >= AgentWebPolicy::kMaxRedirects) {
        finish(requestId, errorResult(Agent::ErrorKind::Network,
                                      QStringLiteral("重定向次数超过上限（%1）。")
                                          .arg(AgentWebPolicy::kMaxRedirects)));
        return;
    }
    if (response.status >= 300 && response.status < 400 && !response.location.isEmpty()) {
        const QUrl next = flight.currentUrl.resolved(QUrl(response.location));
        const AgentWebPolicy::UrlCheck check = AgentWebPolicy::validateUrl(next);
        if (!check.ok) {
            finish(requestId, errorResult(Agent::ErrorKind::EgressBlocked,
                                          QStringLiteral("重定向目标不可用：%1").arg(check.problem)));
            return;
        }
        // 每一跳都重新解析 + 全地址校验 + pinned-IP 连接，不复用上一跳结果。
        startFetch(requestId, next, flight.redirects + 1, flight.search, flight.call,
                   flight.context);
        return;
    }
    if (!response.ok && response.status == 0) {
        finish(requestId, errorResult(Agent::ErrorKind::Network, response.error));
        return;
    }
    if (response.status >= 400) {
        finish(requestId, errorResult(Agent::ErrorKind::Network,
                                      QStringLiteral("目标返回 HTTP %1。").arg(response.status)));
        return;
    }
    if (!AgentWebPolicy::contentTypeAllowed(response.contentType)) {
        finish(requestId, errorResult(Agent::ErrorKind::EgressBlocked,
                                      QStringLiteral("拒绝解析这个 Content-Type：%1")
                                          .arg(response.contentType)));
        return;
    }
    if (flight.search) {
        finish(requestId, searchResult(flight, response));
    } else {
        finish(requestId, fetchResult(flight, response));
    }
}

namespace {

QJsonObject egressFor(const QUrl &url) {
    return QJsonObject{{QStringLiteral("kind"), QStringLiteral("web")},
                       {QStringLiteral("decision"), QStringLiteral("confirm")},
                       {QStringLiteral("reason"),
                        QStringLiteral("内容来自第三方站点 %1，按不可信数据处理").arg(url.host())}};
}

}  // namespace

AgentToolResult AgentWebTools::fetchResult(const RunningFlight &flight,
                                           const AgentHttpResponse &response) const {
    QString text = QString::fromUtf8(response.body);
    const bool isHtml = response.contentType.toLower().contains(QStringLiteral("html"));
    if (isHtml) text = AgentWebPolicy::htmlToText(text);
    const AgentRedaction::Result redacted = AgentRedaction::redact(text);
    QString output = redacted.text;
    bool truncated = response.truncated;
    if (AgentWebPolicy::truncateText(&output)) truncated = true;
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = QStringLiteral("抓取完成：%1（HTTP %2，%3 字节）")
                         .arg(flight.currentUrl.host())
                         .arg(response.status)
                         .arg(response.body.size());
    result.data = QJsonObject{{QStringLiteral("url"), flight.currentUrl.toString()},
                              {QStringLiteral("status"), response.status},
                              {QStringLiteral("contentType"), response.contentType},
                              {QStringLiteral("text"), output},
                              {QStringLiteral("truncated"), truncated},
                              {QStringLiteral("untrusted"), true},
                              {QStringLiteral("egress"), egressFor(flight.currentUrl)}};
    return result;
}

AgentToolResult AgentWebTools::searchResult(const RunningFlight &flight,
                                            const AgentHttpResponse &response) const {
    const QJsonDocument document = QJsonDocument::fromJson(response.body);
    if (!document.isObject()) {
        return errorResult(Agent::ErrorKind::Protocol,
                           QStringLiteral("SearXNG 返回的不是 JSON。"));
    }
    QJsonArray results;
    for (const QJsonValue &value : document.object().value(QStringLiteral("results")).toArray()) {
        const QJsonObject item = value.toObject();
        const QJsonObject entry{
            {QStringLiteral("title"),
             AgentRedaction::redact(item.value(QStringLiteral("title")).toString()).text},
            {QStringLiteral("url"), item.value(QStringLiteral("url")).toString()},
            {QStringLiteral("snippet"),
             AgentRedaction::redact(item.value(QStringLiteral("content")).toString()).text}};
        results.append(entry);
        if (results.size() >= 20) break;
    }
    AgentToolResult result;
    result.status = AgentToolResult::Status::Ok;
    result.summary = QStringLiteral("搜索完成：%1 条结果").arg(results.size());
    result.data = QJsonObject{{QStringLiteral("query"), flight.currentUrl.query()},
                              {QStringLiteral("results"), results},
                              {QStringLiteral("untrusted"), true},
                              {QStringLiteral("egress"), egressFor(flight.currentUrl)}};
    return result;
}

void AgentWebTools::finish(const QString &requestId, const AgentToolResult &result) {
    if (!m_running.contains(requestId)) return;
    const RunningFlight flight = m_running.take(requestId);
    if (m_sink) m_sink(flight.call, result);
}

void AgentWebTools::cancelPending() {
    m_approvals.clear();
    const QStringList ids = m_running.keys();
    for (const QString &id : ids) {
        const RunningFlight flight = m_running.take(id);
        AgentToolResult result =
            errorResult(Agent::ErrorKind::Cancelled, QStringLiteral("这次联网请求已取消。"));
        if (m_sink) m_sink(flight.call, result);
    }
}

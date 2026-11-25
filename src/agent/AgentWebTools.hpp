/**
 * @file AgentWebTools.hpp
 * @brief Stage 3 联网工具：`web.search`（SearXNG）与 `web.fetch`（pinned-IP 抓取）。
 *
 * 两道闸：每次调用都要审批（搜索词/URL 会发给第三方），用户选“本会话允许”后同一个 host
 * 在本会话内不再逐次确认；网络层再按 `AgentWebPolicy` 校验全部 A/AAAA、逐跳重定向重检、
 * 强制直连。结果是不可信内容：HTML 转文本、遮凭据、截断后进上下文。
 */

#pragma once

#include "AgentTool.hpp"
#include "AgentWebPolicy.hpp"
#include "AgentWebTransport.hpp"

#include <QHash>
#include <QSet>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

class AgentWebTools : public QObject, public AgentToolEngine {
    Q_OBJECT

public:
    using ContextProvider = std::function<AgentToolContext(QString *problem)>;

    explicit AgentWebTools(AgentWebTransport *transport, ContextProvider provider = {},
                           QObject *parent = nullptr);

    void setTransport(AgentWebTransport *transport);
    static QList<AgentToolSpec> specs();
    static bool isWebTool(const QString &internalName);

    QJsonArray schemas() const override;
    AgentToolResult invoke(const Agent::ToolCall &call) override;
    AgentToolResult resolveApproval(const QString &approvalId, bool approved) override;
    /// 本会话放行：这一次照常执行，同一个 host 在同一个 sessionId 下后续不再弹审批。
    AgentToolResult resolveSessionApproval(const QString &approvalId) override;
    void cancelPending() override;
    bool handles(const QString &internalName) const override;
    void setCompletionSink(AgentToolSink sink) override { m_sink = std::move(sink); }

    int running() const { return m_running.size(); }

private:
    struct Approval {
        enum class Kind { Fetch, Search };
        Kind kind = Kind::Fetch;
        Agent::ToolCall call;
        QUrl url;
        QString query;
        AgentToolContext context;
    };

    struct RunningFlight {
        Agent::ToolCall call;
        QUrl currentUrl;   ///< 当前这一跳的 URL（原 host，不是 IP）
        int redirects = 0;
        bool search = false;
        AgentToolContext context;
    };

    void startFetch(const QString &requestId, const QUrl &url, int redirects, bool search,
                    const Agent::ToolCall &call, const AgentToolContext &context);
    void onResolved(const QString &requestId, bool ok, const QList<QHostAddress> &addresses,
                    const QString &error);
    void onResponse(const QString &requestId, const AgentHttpResponse &response);
    void finish(const QString &requestId, const AgentToolResult &result);
    AgentToolResult fetchResult(const RunningFlight &flight, const AgentHttpResponse &response) const;
    AgentToolResult searchResult(const RunningFlight &flight, const AgentHttpResponse &response) const;
    AgentToolResult errorResult(Agent::ErrorKind kind, const QString &message,
                                const QJsonObject &data = {}) const;
    /// 审批通过后的统一入口（逐次允许和本会话放行走同一段）。
    AgentToolResult startApproved(const Approval &approval);
    bool sessionAllowed(const AgentToolContext &context, const QString &host) const;
    void rememberSession(const AgentToolContext &context, const QString &host);
    QString newRequestId() const;

    AgentWebTransport *m_transport = nullptr;
    ContextProvider m_provider;
    AgentToolSink m_sink;
    QHash<QString, Approval> m_approvals;
    /// 会话 id → 已放行的 host（只在内存里，重启/切会话即失效）。
    QHash<QString, QSet<QString>> m_sessionHosts;
    QHash<QString, RunningFlight> m_running;
    mutable int m_counter = 1;
};

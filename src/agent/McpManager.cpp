/**
 * @file McpManager.cpp
 * @brief MCP 生命周期与工具汇总（见 .hpp）。
 */

#include "McpManager.hpp"

#include <QSet>

McpManager::McpManager(McpTrustStore *store, QObject *parent) : QObject(parent), m_store(store) {}

McpManager::~McpManager() { stopAll(); }

QString McpManager::startResultName(StartResult result) {
    switch (result) {
    case StartResult::Started: return QStringLiteral("started");
    case StartResult::NeedsConfirmation: return QStringLiteral("needs-confirmation");
    case StartResult::Blocked: return QStringLiteral("blocked");
    }
    return QStringLiteral("blocked");
}

bool McpManager::reload(QString *problem) {
    if (m_store == nullptr) {
        if (problem != nullptr) *problem = tr("MCP 配置存储未接入。");
        return false;
    }
    QList<McpServerConfig> servers;
    if (!m_store->load(&servers, problem)) return false;
    // 只保留仍在配置里的客户端；被删掉的服务器直接停掉（不自动重启）。
    const QSet<QString> ids = [&servers] {
        QSet<QString> set;
        for (const McpServerConfig &server : servers) set.insert(server.id);
        return set;
    }();
    const QStringList running = m_clients.keys();
    for (const QString &id : running) {
        if (ids.contains(id)) continue;
        stop(id);
    }
    m_servers = servers;
    rebuildTools();
    emit serversChanged();
    return true;
}

McpServerConfig McpManager::server(const QString &serverId) const {
    for (const McpServerConfig &server : m_servers) {
        if (server.id == serverId) return server;
    }
    return {};
}

McpClient::State McpManager::state(const QString &serverId) const {
    const McpClient *client = m_clients.value(serverId, nullptr);
    return client == nullptr ? McpClient::State::Idle : client->state();
}

QString McpManager::stateDetail(const QString &serverId) const {
    const McpClient *client = m_clients.value(serverId, nullptr);
    return client == nullptr ? tr("未启动。") : client->stateDetail();
}

bool McpManager::anyReady() const {
    for (const McpClient *client : m_clients) {
        if (client->ready()) return true;
    }
    return false;
}

McpClient *McpManager::ensureClient(const QString &serverId) {
    if (m_clients.contains(serverId)) return m_clients.value(serverId);
    auto *client = new McpClient(this);
    connect(client, &McpClient::stateChanged, this,
            [this, serverId](McpClient::State state, const QString &detail) {
                emit serverStateChanged(serverId, McpClient::stateName(state), detail);
            });
    connect(client, &McpClient::toolsChanged, this, [this] {
        rebuildTools();
        emit toolsChanged();
    });
    connect(client, &McpClient::logLine, this,
            [this, serverId](const QString &text) { emit logLine(serverId, text); });
    connect(client, &McpClient::toolFinished, this,
            [this](const QString &requestId, const QJsonObject &result) {
                m_requestServer.remove(requestId);
                emit toolFinished(requestId, result);
            });
    connect(client, &McpClient::toolFailed, this,
            [this](const QString &requestId, const Agent::Error &error) {
                m_requestServer.remove(requestId);
                emit toolFailed(requestId, error);
            });
    m_clients.insert(serverId, client);
    return client;
}

McpManager::StartResult McpManager::start(const QString &serverId,
                                          McpTrustStore::GateResult *gate, QString *problem) {
    if (m_store == nullptr) {
        if (problem != nullptr) *problem = tr("MCP 配置存储未接入。");
        return StartResult::Blocked;
    }
    const McpTrustStore::GateResult result = m_store->checkStart(serverId);
    if (gate != nullptr) *gate = result;
    if (result.gate == McpTrustStore::GateResult::Gate::Blocked) {
        if (problem != nullptr) *problem = result.reason;
        return StartResult::Blocked;
    }
    if (result.gate == McpTrustStore::GateResult::Gate::NeedsConfirmation) {
        return StartResult::NeedsConfirmation;
    }
    return startConfirmed(serverId, problem);
}

McpManager::StartResult McpManager::reconnect(const QString &serverId,
                                              McpTrustStore::GateResult *gate,
                                              QString *problem) {
    if (m_store == nullptr) {
        if (problem != nullptr) *problem = tr("MCP 配置存储未接入。");
        return StartResult::Blocked;
    }
    stop(serverId);
    const McpTrustStore::GateResult result = m_store->checkStart(serverId);
    if (gate != nullptr) *gate = result;
    if (result.gate == McpTrustStore::GateResult::Gate::Blocked) {
        if (problem != nullptr) *problem = result.reason;
        return StartResult::Blocked;
    }
    // 就算配置和身份都没变，手动重连也要再确认一次。
    McpTrustStore::GateResult pending = result;
    pending.gate = McpTrustStore::GateResult::Gate::NeedsConfirmation;
    pending.reason = tr("重新连接前需确认。");
    if (gate != nullptr) *gate = pending;
    return StartResult::NeedsConfirmation;
}

McpManager::StartResult McpManager::startConfirmed(const QString &serverId, QString *problem) {
    const McpServerConfig config = server(serverId);
    if (!config.isValid()) {
        if (problem != nullptr) *problem = tr("服务器配置不存在。");
        return StartResult::Blocked;
    }
    const QString configProblem = McpTrustStore::validateConfig(config);
    if (!configProblem.isEmpty()) {
        if (problem != nullptr) *problem = configProblem;
        return StartResult::Blocked;
    }
    if (m_store != nullptr) {
        // 再核一次：调用方可能没写确认就直接来起进程。
        const McpTrustStore::GateResult gate = m_store->checkStart(serverId);
        if (!gate.allowed()) {
            if (problem != nullptr) *problem = gate.reason;
            return gate.gate == McpTrustStore::GateResult::Gate::Blocked
                       ? StartResult::Blocked
                       : StartResult::NeedsConfirmation;
        }
        McpClient *client = ensureClient(serverId);
        client->start(config, gate.identity);
        return StartResult::Started;
    }
    if (problem != nullptr) *problem = tr("MCP 配置存储未接入。");
    return StartResult::Blocked;
}

void McpManager::stop(const QString &serverId) {
    McpClient *client = m_clients.value(serverId, nullptr);
    if (client == nullptr) return;
    client->stop();
    m_clients.remove(serverId);
    client->deleteLater();
    rebuildTools();
    emit toolsChanged();
}

void McpManager::stopAll() {
    const QStringList ids = m_clients.keys();
    for (const QString &id : ids) stop(id);
}

void McpManager::rebuildTools() {
    m_tools.clear();
    m_toolConflict.clear();
    QHash<QString, QString> owner;
    for (const McpServerConfig &server : m_servers) {
        McpClient *client = m_clients.value(server.id, nullptr);
        if (client == nullptr) continue;
        for (const McpToolDescriptor &tool : client->tools()) {
            const QString wire = tool.wireName();
            if (wire.isEmpty()) continue;
            if (owner.contains(wire)) {
                m_toolConflict.insert(
                    wire, tr("「%1」与「%2」映射出了同一工具名，已拒绝。")
                              .arg(owner.value(wire), server.name));
                continue;
            }
            owner.insert(wire, server.name);
            m_tools.append(tool);
        }
    }
}

QString McpManager::conflictReason(const QString &wireName) const {
    return m_toolConflict.value(wireName);
}

McpToolDescriptor McpManager::toolForWireName(const QString &wireName) const {
    for (const McpToolDescriptor &tool : m_tools) {
        if (tool.wireName() == wireName) return tool;
    }
    return {};
}

QString McpManager::requestOwner(const QString &requestId) const {
    return m_requestServer.value(requestId);
}

QString McpManager::callTool(const QString &serverId, const QString &toolName,
                             const QJsonObject &arguments) {
    McpClient *client = m_clients.value(serverId, nullptr);
    if (client == nullptr) return QString();
    const QString requestId = client->callTool(toolName, arguments);
    if (!requestId.isEmpty()) m_requestServer.insert(requestId, serverId);
    return requestId;
}

void McpManager::cancelCall(const QString &requestId) {
    const QString serverId = requestOwner(requestId);
    if (serverId.isEmpty()) return;
    McpClient *client = m_clients.value(serverId, nullptr);
    if (client != nullptr) client->cancelCall(requestId);
}

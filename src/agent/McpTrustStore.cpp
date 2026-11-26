/**
 * @file McpTrustStore.cpp
 * @brief MCP 配置落盘、身份计算与确认闸（见 .hpp）。
 */

#include "McpTrustStore.hpp"

#include "AgentPaths.hpp"
#include "AgentRedaction.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QString sha256OfFile(const QString &path, QString *problem) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "无法读取可执行文件：%1").arg(path);
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "可执行文件哈希计算失败。");
        return QString();
    }
    return QString::fromLatin1(hash.result().toHex());
}

/// 卷序列 + 文件 ID：能取到就取，取不到留空（§10.1 允许"能取到时"）。
void fillFileIdentity(const QString &path, McpExecutableIdentity *identity) {
#if defined(Q_OS_WIN)
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info)) {
        identity->volumeSerial =
            QString::number(info.dwVolumeSerialNumber, 16).rightJustified(8, QLatin1Char('0'));
        identity->fileId = QStringLiteral("%1%2")
                               .arg(info.nFileIndexHigh, 8, 16, QLatin1Char('0'))
                               .arg(info.nFileIndexLow, 8, 16, QLatin1Char('0'));
    }
    CloseHandle(handle);
#else
    Q_UNUSED(path);
    Q_UNUSED(identity);
#endif
}

}  // namespace

QString McpTrustStore::GateResult::gateName() const {
    switch (gate) {
    case Gate::Allowed: return QStringLiteral("allowed");
    case Gate::NeedsConfirmation: return QStringLiteral("needs-confirmation");
    case Gate::Blocked: return QStringLiteral("blocked");
    }
    return QStringLiteral("blocked");
}

McpTrustStore::McpTrustStore(const QString &root)
    : m_root(root.isEmpty() ? AgentPaths::mcpConfigDir() : QDir::cleanPath(root)) {}

void McpTrustStore::setRoot(const QString &root) {
    m_root = root.isEmpty() ? AgentPaths::mcpConfigDir() : QDir::cleanPath(root);
    m_confirmations.clear();
    m_confirmationsLoaded = false;
}

QString McpTrustStore::serversPath() const {
    return m_root + QStringLiteral("/servers.json");
}

QString McpTrustStore::confirmationsPath() const {
    return m_root + QStringLiteral("/confirmations.json");
}

bool McpTrustStore::load(QList<McpServerConfig> *servers, QString *problem) const {
    if (servers == nullptr) return false;
    servers->clear();
    QFile file(serversPath());
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "无法打开 MCP 配置文件。");
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "MCP 配置文件格式无效。");
        return false;
    }
    for (const QJsonValue &value : document.object().value(QStringLiteral("servers")).toArray()) {
        const McpServerConfig server = McpServerConfig::fromJson(value.toObject());
        if (server.isValid()) servers->append(server);
    }
    return true;
}

bool McpTrustStore::save(const QList<McpServerConfig> &servers, QString *problem) {
    if (!QDir().mkpath(m_root)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "无法创建 MCP 配置目录。");
        return false;
    }
    QJsonArray array;
    for (const McpServerConfig &server : servers) array.append(server.toJson());
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1},
                                                         {QStringLiteral("servers"), array}})
                                   .toJson(QJsonDocument::Indented);
    QSaveFile file(serversPath());
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() ||
        !file.commit()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "MCP 配置写盘失败。");
        return false;
    }
    return true;
}

bool McpTrustStore::upsert(const McpServerConfig &server, QString *problem) {
    QList<McpServerConfig> servers;
    if (!load(&servers, problem)) return false;
    bool replaced = false;
    for (McpServerConfig &existing : servers) {
        if (existing.id != server.id) continue;
        existing = server;
        replaced = true;
    }
    if (!replaced) servers.append(server);
    if (!save(servers, problem)) return false;
    // 配置写入即代表"内容变了"：旧确认不再匹配新的 configHash，不需要主动删。
    return true;
}

bool McpTrustStore::remove(const QString &serverId, QString *problem) {
    QList<McpServerConfig> servers;
    if (!load(&servers, problem)) return false;
    QList<McpServerConfig> kept;
    for (const McpServerConfig &server : servers) {
        if (server.id != serverId) kept.append(server);
    }
    if (!save(kept, problem)) return false;
    revoke(serverId);
    return true;
}

McpServerConfig McpTrustStore::find(const QString &serverId) const {
    QList<McpServerConfig> servers;
    load(&servers, nullptr);
    for (const McpServerConfig &server : servers) {
        if (server.id == serverId) return server;
    }
    return {};
}

QString McpTrustStore::validateConfig(const McpServerConfig &server) {
    if (server.id.isEmpty()) return QCoreApplication::translate("McpTrustStore", "服务器缺少 id。");
    if (server.name.trimmed().isEmpty()) return QCoreApplication::translate("McpTrustStore", "服务器缺少名称。");
    if (server.command.trimmed().isEmpty()) return QCoreApplication::translate("McpTrustStore", "未填写可执行文件。");
    if (!QDir::isAbsolutePath(server.command)) {
        return QCoreApplication::translate("McpTrustStore", "可执行文件须为绝对路径（不解析 PATH）。");
    }
    if (server.arguments.size() > McpLimits::kMaxArguments) {
        return QCoreApplication::translate("McpTrustStore", "参数超过 %1 个。").arg(McpLimits::kMaxArguments);
    }
    for (const QString &argument : server.arguments) {
        if (argument.size() > McpLimits::kMaxArgumentLength) {
            return QCoreApplication::translate("McpTrustStore", "单个参数超过 %1 字符。").arg(McpLimits::kMaxArgumentLength);
        }
    }
    if (!server.workingDirectory.isEmpty()) {
        if (!QDir::isAbsolutePath(server.workingDirectory)) {
            return QCoreApplication::translate("McpTrustStore", "工作目录须为绝对路径。");
        }
        if (server.workingDirectory.size() > McpLimits::kMaxCwdLength) {
            return QCoreApplication::translate("McpTrustStore", "工作目录路径过长。");
        }
        if (QFileInfo(server.workingDirectory) == QFileInfo(server.command)) {
            return QCoreApplication::translate("McpTrustStore", "工作目录不能是可执行文件自身。");
        }
    }
    return QString();
}

McpExecutableIdentity McpTrustStore::computeIdentity(const QString &path, QString *problem) {
    McpExecutableIdentity identity;
    if (path.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "可执行文件路径为空。");
        return identity;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "可执行文件不存在：%1").arg(path);
        return identity;
    }
    identity.absolutePath = info.canonicalFilePath();
    if (identity.absolutePath.isEmpty()) identity.absolutePath = info.absoluteFilePath();
    identity.size = info.size();
    identity.modifiedAt = info.lastModified().toUTC().toString(Qt::ISODate);
    fillFileIdentity(identity.absolutePath, &identity);
    QString hashProblem;
    identity.sha256 = sha256OfFile(identity.absolutePath, &hashProblem);
    if (identity.sha256.isEmpty()) {
        if (problem != nullptr) *problem = hashProblem;
        return McpExecutableIdentity{};
    }
    return identity;
}

bool McpTrustStore::loadConfirmations(QString *problem) const {
    m_confirmations.clear();
    m_confirmationsLoaded = true;
    QFile file(confirmationsPath());
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "无法打开 MCP 确认记录。");
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    for (const QJsonValue &value :
         document.object().value(QStringLiteral("confirmations")).toArray()) {
        const McpConfirmation confirmation = McpConfirmation::fromJson(value.toObject());
        if (!confirmation.serverId.isEmpty()) {
            m_confirmations.insert(confirmation.serverId, confirmation);
        }
    }
    return true;
}

bool McpTrustStore::saveConfirmations(QString *problem) const {
    if (!QDir().mkpath(m_root)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "无法创建 MCP 配置目录。");
        return false;
    }
    QJsonArray array;
    for (const McpConfirmation &confirmation : m_confirmations) {
        array.append(confirmation.toJson());
    }
    const QByteArray payload =
        QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1},
                                  {QStringLiteral("confirmations"), array}})
            .toJson(QJsonDocument::Indented);
    QSaveFile file(confirmationsPath());
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() ||
        !file.commit()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("McpTrustStore", "MCP 确认记录写盘失败。");
        return false;
    }
    return true;
}

bool McpTrustStore::confirmation(const QString &serverId, McpConfirmation *out) const {
    if (!m_confirmationsLoaded) loadConfirmations(nullptr);
    if (!m_confirmations.contains(serverId)) return false;
    if (out != nullptr) *out = m_confirmations.value(serverId);
    return true;
}

void McpTrustStore::revoke(const QString &serverId) {
    if (!m_confirmationsLoaded) loadConfirmations(nullptr);
    if (m_confirmations.remove(serverId) > 0) saveConfirmations(nullptr);
}

McpTrustStore::GateResult McpTrustStore::checkStart(const QString &serverId) const {
    GateResult result;
    result.server = find(serverId);
    if (!result.server.isValid()) {
        result.gate = GateResult::Gate::Blocked;
        result.reason = QCoreApplication::translate("McpTrustStore", "服务器配置不存在。");
        return result;
    }
    if (!result.server.enabled) {
        result.gate = GateResult::Gate::Blocked;
        result.reason = QCoreApplication::translate("McpTrustStore", "服务器未启用。");
        return result;
    }
    result.configHash = result.server.configHash();
    QString identityProblem;
    result.identity = computeIdentity(result.server.command, &identityProblem);
    if (!result.identity.isValid()) {
        result.gate = GateResult::Gate::Blocked;
        result.reason = identityProblem.isEmpty() ? QCoreApplication::translate("McpTrustStore", "无法获取可执行文件身份。")
                                                   : identityProblem;
        return result;
    }
    McpConfirmation stored;
    if (!confirmation(serverId, &stored)) {
        result.gate = GateResult::Gate::NeedsConfirmation;
        result.reason = QCoreApplication::translate("McpTrustStore", "首次启动该服务器前需确认。");
        return result;
    }
    if (stored.configHash != result.configHash) {
        result.gate = GateResult::Gate::NeedsConfirmation;
        result.reason = QCoreApplication::translate("McpTrustStore", "配置已变更，需重新确认。");
        return result;
    }
    if (!stored.identity.sameAs(result.identity)) {
        result.gate = GateResult::Gate::NeedsConfirmation;
        result.reason = QCoreApplication::translate("McpTrustStore", "可执行文件已变更（SHA-256、大小或文件 ID），需重新确认。");
        return result;
    }
    result.gate = GateResult::Gate::Allowed;
    result.reason = QCoreApplication::translate("McpTrustStore", "已确认（%1）。").arg(stored.confirmedAt);
    return result;
}

bool McpTrustStore::confirm(const QString &serverId, const QString &purpose, GateResult *result,
                            QString *problem) {
    GateResult gate = checkStart(serverId);
    if (gate.gate == GateResult::Gate::Blocked) {
        if (problem != nullptr) *problem = gate.reason;
        if (result != nullptr) *result = gate;
        return false;
    }
    McpConfirmation confirmation;
    confirmation.serverId = serverId;
    confirmation.configHash = gate.configHash;
    confirmation.identity = gate.identity;
    confirmation.confirmedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    confirmation.purpose = purpose;
    if (!m_confirmationsLoaded) loadConfirmations(nullptr);
    m_confirmations.insert(serverId, confirmation);
    if (!saveConfirmations(problem)) return false;
    gate.gate = GateResult::Gate::Allowed;
    gate.reason = QCoreApplication::translate("McpTrustStore", "已确认，可启动。");
    if (result != nullptr) *result = gate;
    return true;
}

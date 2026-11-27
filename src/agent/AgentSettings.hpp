/**
 * @file AgentSettings.hpp
 * @brief 智能体模型服务的**非密**配置：provider profile、模型、枚举化协议适配项。
 *
 * 盘上落在 QSettings（可被测试重定向到临时 ini）。这里**绝不**存 API key ——
 * 密钥只走 `AgentSecretStore`。profileId 是内部生成的受限 slug，不直接用显示名拼
 * 凭据管理器 target，避免 target 注入。
 */

#pragma once

#include <QCoreApplication>
#include <QJsonObject>
#include <QList>
#include <QString>

struct AgentProviderProfile {
    QString id = QStringLiteral("default");
    QString name = QCoreApplication::translate("AgentSettings", "默认模型服务");
    QString baseUrl;
    QString model;
    int contextWindow = 32768;
    int maxOutput = 4096;
    double temperature = 0.7;
    int timeoutSec = 120;
    /// cloud / local。本地只影响外发说明，不改变 URL 校验。
    QString deployment = QStringLiteral("cloud");
    /// 内置厂商目录里的 id（`AgentProviderCatalog`）；空 = 用户选了「自定义」或从旧配置读上来。
    QString providerId;
    /// 思考强度：off / low / medium / high。空 = 不发送（模型不支持时也留空）。
    QString reasoningEffort;
    /// bearer / api-key-header / none。none 只用于本地服务。
    QString authMode = QStringLiteral("bearer");
    bool stream = true;
    QString maxTokensField = QStringLiteral("max_tokens");
    QString systemRole = QStringLiteral("system");
    QString reasoningEcho = QStringLiteral("display-only");
    bool sendTemperature = true;
    bool sendToolChoice = true;
    bool localHttpAllowed = false;
    /// 首次使用外发说明的确认记录（providerId + host + policyVersion）。
    bool egressAcknowledged = false;
    int egressPolicyVersion = 0;
    /// credential-manager / dpapi-fallback；只用于界面显示，不代表值本身。
    QString storageBackend = QStringLiteral("credential-manager");

    bool isValid() const { return !id.isEmpty() && !baseUrl.isEmpty() && !model.isEmpty(); }

    QJsonObject toJson() const;
    static AgentProviderProfile fromJson(const QJsonObject &object);
    /// 只校验枚举与数值范围；URL 规则在 AgentProviderAdapters::validateBaseUrl()。
    bool validateEnumFields(QString *problem) const;
};

class AgentSettings {
public:
    /// 全部 profile（盘上没有就返回空表，由调用方决定是否建默认档）。
    static QList<AgentProviderProfile> profiles();
    static bool saveProfile(const AgentProviderProfile &profile, QString *problem = nullptr);
    static bool removeProfile(const QString &profileId, QString *problem = nullptr);
    static AgentProviderProfile profile(const QString &profileId);
    static bool strictEgress();
    static void setStrictEgress(bool strict);
    static QString activeProfileId();
    static void setActiveProfileId(const QString &profileId);

    /// Stage 3：SearXNG 实例地址（`web.search` 用）。空 = 没配，联网搜索不可用。
    static QString webSearchEndpoint();
    static void setWebSearchEndpoint(const QString &endpoint);
    /// Stage 3：诊断命令总开关，**默认关闭**（§9.5.1）。
    static bool diagnoseCommandsEnabled();
    static void setDiagnoseCommandsEnabled(bool enabled);
    /// P2 修复病例库，**默认关闭**：关着的时候既不落盘也不参与排序。
    static bool repairCasesEnabled();
    static void setRepairCasesEnabled(bool enabled);

    static QString webSearchKey() { return QStringLiteral("Agent/WebSearchEndpoint"); }
    static QString diagnoseCommandsKey() { return QStringLiteral("Agent/DiagnoseCommands"); }
    static QString repairCasesKey() { return QStringLiteral("Agent/RepairCases"); }

    static QString profilesKey() { return QStringLiteral("Agent/Profiles"); }
    static QString activeKey() { return QStringLiteral("Agent/ActiveProfile"); }
    static QString strictEgressKey() { return QStringLiteral("Agent/StrictEgress"); }
};

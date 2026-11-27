/**
 * @file AgentSettings.cpp
 * @brief 非密配置的读写（QSettings 可隔离；密钥不在这里）。
 */

#include "AgentSettings.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

QJsonObject AgentProviderProfile::toJson() const {
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("baseUrl"), baseUrl},
        {QStringLiteral("model"), model},
        {QStringLiteral("contextWindow"), contextWindow},
        {QStringLiteral("maxOutput"), maxOutput},
        {QStringLiteral("temperature"), temperature},
        {QStringLiteral("timeoutSec"), timeoutSec},
        {QStringLiteral("deployment"), deployment},
        {QStringLiteral("providerId"), providerId},
        {QStringLiteral("reasoningEffort"), reasoningEffort},
        {QStringLiteral("authMode"), authMode},
        {QStringLiteral("stream"), stream},
        {QStringLiteral("maxTokensField"), maxTokensField},
        {QStringLiteral("systemRole"), systemRole},
        {QStringLiteral("reasoningEcho"), reasoningEcho},
        {QStringLiteral("sendTemperature"), sendTemperature},
        {QStringLiteral("sendToolChoice"), sendToolChoice},
        {QStringLiteral("localHttpAllowed"), localHttpAllowed},
        {QStringLiteral("egressAcknowledged"), egressAcknowledged},
        {QStringLiteral("egressPolicyVersion"), egressPolicyVersion},
        {QStringLiteral("storageBackend"), storageBackend},
    };
}

AgentProviderProfile AgentProviderProfile::fromJson(const QJsonObject &object) {
    AgentProviderProfile profile;
    profile.id = object.value(QStringLiteral("id")).toString(profile.id);
    profile.name = object.value(QStringLiteral("name")).toString(profile.name);
    profile.baseUrl = object.value(QStringLiteral("baseUrl")).toString();
    profile.model = object.value(QStringLiteral("model")).toString();
    profile.contextWindow = object.value(QStringLiteral("contextWindow")).toInt(32768);
    profile.maxOutput = object.value(QStringLiteral("maxOutput")).toInt(4096);
    profile.temperature = object.value(QStringLiteral("temperature")).toDouble(0.7);
    profile.timeoutSec = object.value(QStringLiteral("timeoutSec")).toInt(120);
    profile.deployment = object.value(QStringLiteral("deployment")).toString(QStringLiteral("cloud"));
    profile.providerId = object.value(QStringLiteral("providerId")).toString();
    profile.reasoningEffort = object.value(QStringLiteral("reasoningEffort")).toString();
    profile.authMode = object.value(QStringLiteral("authMode")).toString(QStringLiteral("bearer"));
    profile.stream = object.value(QStringLiteral("stream")).toBool(true);
    profile.maxTokensField =
        object.value(QStringLiteral("maxTokensField")).toString(QStringLiteral("max_tokens"));
    profile.systemRole =
        object.value(QStringLiteral("systemRole")).toString(QStringLiteral("system"));
    profile.reasoningEcho =
        object.value(QStringLiteral("reasoningEcho")).toString(QStringLiteral("display-only"));
    profile.sendTemperature = object.value(QStringLiteral("sendTemperature")).toBool(true);
    profile.sendToolChoice = object.value(QStringLiteral("sendToolChoice")).toBool(true);
    profile.localHttpAllowed = object.value(QStringLiteral("localHttpAllowed")).toBool(false);
    profile.egressAcknowledged = object.value(QStringLiteral("egressAcknowledged")).toBool(false);
    profile.egressPolicyVersion = object.value(QStringLiteral("egressPolicyVersion")).toInt(0);
    profile.storageBackend =
        object.value(QStringLiteral("storageBackend")).toString(QStringLiteral("credential-manager"));
    return profile;
}

bool AgentProviderProfile::validateEnumFields(QString *problem) const {
    const auto reject = [problem](const QString &text) {
        if (problem != nullptr) {
            *problem = text;
        }
        return false;
    };
    if (id.isEmpty()) return reject(QCoreApplication::translate("AgentSettings", "配置 id 不能为空。"));
    if (id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\')) ||
        id.contains(QLatin1Char('?')) || id.contains(QLatin1Char('#'))) {
        return reject(QCoreApplication::translate("AgentSettings", "配置 id 含非法字符。"));
    }
    if (authMode != QLatin1String("bearer") && authMode != QLatin1String("api-key-header") &&
        authMode != QLatin1String("none")) {
        return reject(QCoreApplication::translate("AgentSettings", "认证头模式不是允许的枚举值。"));
    }
    if (!reasoningEffort.isEmpty() && reasoningEffort != QLatin1String("off") &&
        reasoningEffort != QLatin1String("low") && reasoningEffort != QLatin1String("medium") &&
        reasoningEffort != QLatin1String("high")) {
        return reject(QCoreApplication::translate("AgentSettings", "思考强度不是允许的枚举值。"));
    }
    if (maxTokensField != QLatin1String("max_tokens") &&
        maxTokensField != QLatin1String("max_completion_tokens")) {
        return reject(QCoreApplication::translate("AgentSettings", "max_tokens 字段名不是允许的枚举值。"));
    }
    if (systemRole != QLatin1String("system") && systemRole != QLatin1String("developer")) {
        return reject(QCoreApplication::translate("AgentSettings", "system role 不是允许的枚举值。"));
    }
    if (reasoningEcho != QLatin1String("display-only") &&
        reasoningEcho != QLatin1String("send-back")) {
        return reject(QCoreApplication::translate("AgentSettings", "reasoning 回传方式不是允许的枚举值。"));
    }
    if (deployment != QLatin1String("cloud") && deployment != QLatin1String("local")) {
        return reject(QCoreApplication::translate("AgentSettings", "部署类型不是允许的枚举值。"));
    }
    if (contextWindow < 1024 || contextWindow > 10000000) {
        return reject(QCoreApplication::translate("AgentSettings", "上下文窗口超出合理范围。"));
    }
    if (maxOutput < 1 || maxOutput >= contextWindow) {
        return reject(QCoreApplication::translate("AgentSettings", "最大输出必须小于上下文窗口。"));
    }
    if (timeoutSec < 5 || timeoutSec > 600) {
        return reject(QCoreApplication::translate("AgentSettings", "超时时间应在 5–600 秒之间。"));
    }
    if (temperature < 0.0 || temperature > 2.0) {
        return reject(QCoreApplication::translate("AgentSettings", "温度应在 0–2 之间。"));
    }
    return true;
}

QList<AgentProviderProfile> AgentSettings::profiles() {
    QSettings settings;
    const QByteArray raw = settings.value(profilesKey()).toByteArray();
    QList<AgentProviderProfile> result;
    if (raw.isEmpty()) {
        return result;
    }
    const QJsonDocument document = QJsonDocument::fromJson(raw);
    for (const QJsonValue &value : document.array()) {
        if (value.isObject()) {
            result.append(AgentProviderProfile::fromJson(value.toObject()));
        }
    }
    return result;
}

bool AgentSettings::saveProfile(const AgentProviderProfile &profile, QString *problem) {
    if (!profile.validateEnumFields(problem)) {
        return false;
    }
    QList<AgentProviderProfile> list = profiles();
    bool replaced = false;
    for (AgentProviderProfile &existing : list) {
        if (existing.id == profile.id) {
            existing = profile;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        list.append(profile);
    }
    QJsonArray array;
    for (const AgentProviderProfile &entry : list) {
        array.append(entry.toJson());
    }
    QSettings settings;
    settings.setValue(profilesKey(), QJsonDocument(array).toJson(QJsonDocument::Compact));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentSettings", "配置写盘失败（%1）。").arg(settings.fileName());
        }
        return false;
    }
    return true;
}

bool AgentSettings::removeProfile(const QString &profileId, QString *problem) {
    QList<AgentProviderProfile> list = profiles();
    const auto before = list.size();
    list.removeIf([&profileId](const AgentProviderProfile &entry) { return entry.id == profileId; });
    if (list.size() == before) {
        return true;
    }
    QJsonArray array;
    for (const AgentProviderProfile &entry : list) {
        array.append(entry.toJson());
    }
    QSettings settings;
    settings.setValue(profilesKey(), QJsonDocument(array).toJson(QJsonDocument::Compact));
    if (activeProfileId() == profileId) {
        settings.remove(activeKey());
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentSettings", "配置写盘失败（%1）。").arg(settings.fileName());
        }
        return false;
    }
    return true;
}

AgentProviderProfile AgentSettings::profile(const QString &profileId) {
    for (const AgentProviderProfile &entry : profiles()) {
        if (entry.id == profileId) {
            return entry;
        }
    }
    return {};
}

bool AgentSettings::strictEgress() {
    QSettings settings;
    return settings.value(strictEgressKey(), false).toBool();
}

void AgentSettings::setStrictEgress(bool strict) {
    QSettings settings;
    settings.setValue(strictEgressKey(), strict);
    settings.sync();
}

QString AgentSettings::webSearchEndpoint() {
    return QSettings().value(webSearchKey()).toString().trimmed();
}

void AgentSettings::setWebSearchEndpoint(const QString &endpoint) {
    QSettings().setValue(webSearchKey(), endpoint.trimmed());
}

bool AgentSettings::diagnoseCommandsEnabled() {
    return QSettings().value(diagnoseCommandsKey(), false).toBool();
}

void AgentSettings::setDiagnoseCommandsEnabled(bool enabled) {
    QSettings().setValue(diagnoseCommandsKey(), enabled);
}

bool AgentSettings::repairCasesEnabled() {
    return QSettings().value(repairCasesKey(), false).toBool();
}

void AgentSettings::setRepairCasesEnabled(bool enabled) {
    QSettings().setValue(repairCasesKey(), enabled);
}

QString AgentSettings::activeProfileId() {
    QSettings settings;
    return settings.value(activeKey()).toString();
}

void AgentSettings::setActiveProfileId(const QString &profileId) {
    QSettings settings;
    settings.setValue(activeKey(), profileId);
    settings.sync();
}

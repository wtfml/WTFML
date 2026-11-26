/**
 * @file AgentEgressPolicy.cpp
 * @brief D11 边界表与内容脱敏（见 .hpp）。
 */

#include "AgentEgressPolicy.hpp"

#include "AgentRedaction.hpp"

#include <QCoreApplication>
#include <QFileInfo>

namespace AgentEgressPolicy {

int policyVersion() { return 1; }

QString kindKey(Kind kind) {
    switch (kind) {
    case Kind::Log: return QStringLiteral("log");
    case Kind::CrashReport: return QStringLiteral("crash-report");
    case Kind::InstanceMetadata: return QStringLiteral("instance-metadata");
    case Kind::DirectoryMetadata: return QStringLiteral("directory-metadata");
    case Kind::InstanceListSummary: return QStringLiteral("instance-list-summary");
    case Kind::InstanceListFull: return QStringLiteral("instance-list-full");
    case Kind::ConfigFile: return QStringLiteral("config-file");
    case Kind::FileNameList: return QStringLiteral("file-name-list");
    case Kind::OtherText: return QStringLiteral("other-text");
    case Kind::CredentialFile: return QStringLiteral("credential-file");
    case Kind::CredentialStore: return QStringLiteral("credential-store");
    case Kind::Binary: return QStringLiteral("binary");
    case Kind::ArtifactRead: return QStringLiteral("artifact-read");
    }
    return QStringLiteral("other-text");
}

QString decisionKey(Decision decision) {
    switch (decision) {
    case Decision::Auto: return QStringLiteral("auto");
    case Decision::Confirm: return QStringLiteral("confirm");
    case Decision::Block: return QStringLiteral("block");
    }
    return QStringLiteral("block");
}

bool isCredentialFileName(const QString &relativePath) {
    const QString name = QFileInfo(relativePath).fileName().toLower();
    // dotenv 家族：裸 `.env`，以及 `prod.env`、`.env.local`、`app.env.production`。
    // 只判等于 `.env` 放过的是后面这两种 —— 它们才是实际项目里常见的写法。
    if (name.endsWith(QLatin1String(".env")) || name.contains(QLatin1String(".env."))) {
        return true;
    }
    if (name == QLatin1String("launcher_accounts.json")) {
        return true;
    }
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (suffix == QLatin1String("pem") || suffix == QLatin1String("key") ||
        suffix == QLatin1String("p12") || suffix == QLatin1String("pfx") ||
        suffix == QLatin1String("jks")) {
        return true;
    }
    // 词根按「名字里出现」判，不按开头判：`my_credentials_backup.json`、`github_token.json`
    // 都不以这些词起头，按 startsWith 就漏。这里宁可多拦 —— 文件名是别人起的，
    // 少拦一次是把真凭据发给模型服务。
    static const QStringList roots = {
        QStringLiteral("credential"), QStringLiteral("secret"),      QStringLiteral("token"),
        QStringLiteral("apikey"),     QStringLiteral("api_key"),     QStringLiteral("api-key")};
    for (const QString &root : roots) {
        if (name.contains(root)) {
            return true;
        }
    }
    return name.startsWith(QLatin1String("auth."));
}

Kind kindForPath(const QString &relativePath) {
    const QString path = relativePath.toLower();
    if (isCredentialFileName(path)) return Kind::CredentialFile;
    if (path.startsWith(QLatin1String("config/")) ||
        path.startsWith(QLatin1String("defaultconfigs/")) ||
        path == QLatin1String("options.txt")) {
        return Kind::ConfigFile;
    }
    if (path.startsWith(QLatin1String("logs/")) || path.endsWith(QLatin1String(".log"))) {
        return Kind::Log;
    }
    if (path.startsWith(QLatin1String("crash-reports/")) ||
        path.contains(QLatin1String("crash-"))) {
        return Kind::CrashReport;
    }
    return Kind::OtherText;
}

Result evaluate(const Request &request) {
    Result result;
    result.containsCredentials = AgentRedaction::containsCredentials(request.content);
    if (request.kind == Kind::CredentialFile || request.kind == Kind::CredentialStore) {
        result.decision = Decision::Block;
        result.reason = QCoreApplication::translate("AgentEgressPolicy", "禁止读取和发送凭据文件或凭据存储。");
        return result;
    }
    if (request.kind == Kind::Binary) {
        result.decision = Decision::Block;
        result.reason = QCoreApplication::translate("AgentEgressPolicy", "二进制文件仅提供元数据，不读取或发送内容。");
        return result;
    }
    if (request.kind == Kind::DirectoryMetadata ||
        request.kind == Kind::InstanceMetadata ||
        request.kind == Kind::InstanceListSummary) {
        result.decision = request.strictMode ? Decision::Confirm : Decision::Auto;
        result.needsNotice = !request.strictMode;
        result.reason = request.strictMode ? QCoreApplication::translate("AgentEgressPolicy", "严格模式：元数据同样需逐次确认。")
                                           : QCoreApplication::translate("AgentEgressPolicy", "自动读取，可能发送至当前模型服务。");
        return result;
    }
    if (request.kind == Kind::Log || request.kind == Kind::CrashReport ||
        request.kind == Kind::ArtifactRead) {
        result.decision = request.strictMode ? Decision::Confirm : Decision::Auto;
        result.needsNotice = !request.strictMode;
        result.redactedContent = AgentRedaction::redact(request.content).text;
        result.redactionHits = AgentRedaction::redact(request.content).hits;
        result.reason = request.strictMode ? QCoreApplication::translate("AgentEgressPolicy", "严格模式：日志、崩溃报告与 artifact 需逐次确认。")
                                           : QCoreApplication::translate("AgentEgressPolicy", "自动读取且已脱敏，可能发送至当前模型服务。");
        return result;
    }
    // 配置文件、完整文件名清单、其他文本、完整实例列表：每次都确认，没有会话级目录放行。
    result.decision = Decision::Confirm;
    result.redactedContent = AgentRedaction::redact(request.content).text;
    result.redactionHits = AgentRedaction::redact(request.content).hits;
    result.reason = QCoreApplication::translate("AgentEgressPolicy", "需逐次确认；确认后仅发送脱敏片段。");
    return result;
}

}  // namespace AgentEgressPolicy

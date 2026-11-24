/**
 * @file AgentModPlatform.cpp
 * @brief 平台查询与地址解析（见 .hpp）。
 */

#include "AgentModPlatform.hpp"

#include "manage/ModpackExport.hpp"

#include "download/ResourceCatalog.hpp"
#include "net/HttpClient.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QUrl>

namespace {

constexpr int kLookupChunk = 100;

const QByteArray &userAgent() {
    static const QByteArray agent("WTFML-Agent-ModCompat/1.0");
    return agent;
}

QString tr(const char *text) {
    return QCoreApplication::translate("AgentModPlatform", text);
}

QString apiRoot(const QString &override) {
    return override.isEmpty() ? ResourceCatalog::modrinthApiRoot() : override;
}

}  // namespace

namespace AgentModPlatform {

LookupResult lookupModrinth(const LookupQuery &query) {
    LookupResult result;
    if (query.sha1s.isEmpty()) return result;
    QNetworkAccessManager network;
    const QString root = apiRoot(query.apiBase);
    const QList<QPair<QByteArray, QByteArray>> headers{
        {QByteArrayLiteral("User-Agent"), userAgent()}};
    QElapsedTimer clock;
    clock.start();
    const QStringList sha1s = query.sha1s.mid(0, 2000);
    for (int offset = 0; offset < sha1s.size(); offset += kLookupChunk) {
        if (clock.elapsed() > query.deadlineMs) {
            result.problem = tr("平台查询超过 %1 秒，已返回部分结果。").arg(query.deadlineMs / 1000);
            break;
        }
        const QByteArray body = ModpackExport::modrinthVersionFilesRequestBody(
            sha1s.mid(offset, kLookupChunk));
        if (body.isEmpty()) continue;
        const HttpClient::Reply reply = HttpClient::send(
            &network, QUrl(root + QStringLiteral("/version_files")), HttpClient::Verb::Post,
            body, QByteArrayLiteral("application/json"), QByteArray(),
            HttpClient::kDefaultTimeoutMs, headers);
        if (!reply.ok()) {
            result.problem = reply.networkError.isEmpty()
                                 ? tr("Modrinth 反查回了 HTTP %1。").arg(reply.status)
                                 : tr("Modrinth 反查没连上：%1").arg(reply.networkError);
            return result;
        }
        QString parseProblem;
        const QHash<QString, ModpackExport::ModrinthHit> hits =
            ModpackExport::parseModrinthVersionFiles(reply.raw, &parseProblem);
        if (!parseProblem.isEmpty()) {
            result.problem = parseProblem;
            return result;
        }
        const QHash<QString, ResourceCatalog::VersionEntry> versions =
            ResourceCatalog::parseVersionFilesUpdate(reply.raw);
        for (auto it = hits.constBegin(); it != hits.constEnd(); ++it) {
            AgentModCompat::PlatformEntry entry;
            entry.sha1 = it.key().toLower();
            entry.projectId = it.value().projectId;
            const ResourceCatalog::VersionEntry version = versions.value(it.key());
            entry.versionId = version.id;
            entry.versionNumber = version.versionNumber.isEmpty() ? it.value().versionNumber
                                                                  : version.versionNumber;
            entry.gameVersions = version.gameVersions;
            entry.loaders = version.loaders;
            for (const ResourceCatalog::Dependency &dependency : version.dependencies) {
                if (dependency.required() && !dependency.projectId.isEmpty()) {
                    entry.requiredProjectIds.append(dependency.projectId);
                }
            }
            if (entry.projectId.isEmpty()) entry.problem = tr("Modrinth 没有这个哈希的项目号");
            result.entries.append(entry);
        }
    }
    return result;
}

TargetResult resolveModrinth(const TargetQuery &query) {
    TargetResult result;
    QNetworkAccessManager network;
    const QString root = apiRoot(query.apiBase);
    const QList<QPair<QByteArray, QByteArray>> headers{
        {QByteArrayLiteral("User-Agent"), userAgent()}};
    const QUrl url(ResourceCatalog::versionsUrl(query.projectId, query.gameVersion, query.loader,
                                                ResourceCatalog::Source::Modrinth, root));
    const HttpClient::Reply reply = HttpClient::send(
        &network, url, HttpClient::Verb::Get, QByteArray(), QByteArray(), QByteArray(),
        HttpClient::kDefaultTimeoutMs, headers);
    if (!reply.ok()) {
        result.problem = reply.networkError.isEmpty()
                             ? tr("Modrinth 版本列表回了 HTTP %1。").arg(reply.status)
                             : tr("Modrinth 版本列表没连上：%1").arg(reply.networkError);
        return result;
    }
    const QList<ResourceCatalog::VersionEntry> versions = ResourceCatalog::parseVersions(reply.raw);
    if (versions.isEmpty()) {
        result.problem = tr("这个项目没有可用的版本。");
        return result;
    }
    const ResourceCatalog::VersionEntry *chosen = nullptr;
    if (!query.versionId.isEmpty()) {
        for (const ResourceCatalog::VersionEntry &version : versions) {
            if (version.id == query.versionId) {
                chosen = &version;
                break;
            }
        }
    } else {
        chosen = &versions.constFirst();
    }
    if (chosen == nullptr) {
        result.problem = tr("版本列表里没有 %1。").arg(query.versionId);
        return result;
    }
    const ResourceCatalog::FileEntry *file = nullptr;
    if (!query.expectedSha1.isEmpty()) {
        for (const ResourceCatalog::FileEntry &candidate : chosen->files) {
            if (candidate.sha1.compare(query.expectedSha1, Qt::CaseInsensitive) == 0) {
                file = &candidate;
                break;
            }
        }
        if (file == nullptr) {
            result.problem = tr("这一版里没有哈希为 %1 的文件。").arg(query.expectedSha1);
            return result;
        }
    } else {
        file = chosen->primaryFile();
        if (file == nullptr && !chosen->files.isEmpty()) file = &chosen->files.constFirst();
    }
    if (file == nullptr || file->url.isEmpty()) {
        result.problem = tr("这一版没有可下载的文件。");
        return result;
    }
    result.target.projectId = query.projectId;
    result.target.versionId = chosen->id;
    result.target.versionNumber = chosen->versionNumber;
    result.target.fileName = file->filename;
    result.target.url = file->url;
    result.target.sha1 = file->sha1;
    result.target.size = file->size;
    for (const ResourceCatalog::Dependency &dependency : chosen->dependencies) {
        if (dependency.required() && !dependency.projectId.isEmpty()) {
            result.target.requiredProjectIds.append(dependency.projectId);
        }
    }
    return result;
}

}  // namespace AgentModPlatform

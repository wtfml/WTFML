/**
 * @file InstanceIconLookup.cpp
 * @brief 实现见 .hpp。
 */

#include "InstanceIconLookup.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QPair>
#include <QSaveFile>
#include <QSet>

#include "../net/HttpClient.hpp"
#include "ModpackExport.hpp"
#include "ModpackImport.hpp"

namespace InstanceIconLookup {

namespace {

/// 一发最多带多少个哈希 / 指纹。**和 `VersionManager::kLookupChunk` 是同一个数**（100）——
/// 两处都是"批量反查"的同一台接口，那边那个数的来龙去脉写在 VersionManager.cpp 里。
constexpr int kLookupChunk = 100;

/// Modrinth 的接口规矩里写着要能认出是谁在问（它按这个封滥用）。和别处那几个串逐字相同，
/// 但**不能共用一份**（一个在 manage/、一个在 download/、一个在别处，为一行字建一个头
/// 文件不划算）—— 改的时候记得一起看。
const QByteArray kUserAgent = QByteArrayLiteral("WTFML");

/// 「禁用」那四个字。**唯一出处**是 `InstanceFilesPage::disabledSuffix()`（ui/ 里），而这一
/// 层不许 include ui/ —— 所以这儿只认后缀的样子，和 `isLookupCandidate()` 同一句老实话。
const char *kDisabledSuffix = ".disabled";

/// 盘上那份缓存里的一行（见 .hpp 里【结果留一份在盘上】那段）。
///
/// 键是**身份路径**（摘掉「禁用」那四个字之后的绝对路径）：用户点一下开关只是把文件改了
/// 个名，盘的字节一个都没动 —— 键用盘上真那个名字的话，那一列每次开关都要重查一遍。
/// 大小和修改时间**单独存两栏**，不拼进键里：读的时候要的是"这个文件还是不是原来那一份"，
/// 拼进键里就变成"再来一份新的吧"（同一个意思，但每一栏都要自己解析回来）。
struct CacheEntry {
    qint64 size = 0;
    qint64 mtimeMs = 0;
    /// 这个文件的 sha1（`Found::sha1`，实例设置那个「检查更新」要的就是它）。
    QString sha1;
    /// 认出来了（假 = **问过了、这两家都没有**）—— 见 .hpp 里那条 NOTE。
    bool found = false;
    ResourceCatalog::Hit hit;
    ResourceCatalog::VersionEntry currentVersion;
};

/// 摘掉「禁用」那四个字（`X.jar.disabled` → `X.jar`）。摘不动就原样回。
QString identityPath(const QString &path) {
    if (path.endsWith(QLatin1String(kDisabledSuffix), Qt::CaseInsensitive)) {
        return path.left(path.size() - QLatin1String(kDisabledSuffix).size());
    }
    return path;
}

/// 这个文件**现在**是当初记下的那一份吗（大小 + 修改时间两样都对）。
bool sameFile(const CacheEntry &entry, const QFileInfo &info) {
    return entry.size == info.size() &&
           entry.mtimeMs == info.lastModified().toMSecsSinceEpoch();
}

/// 把这个文件填成缓存里的一行。调用方保证 `info` 是刚问过的。
CacheEntry entryFor(const QFileInfo &info, const QString &sha1, bool found,
                    const ResourceCatalog::Hit &hit,
                    const ResourceCatalog::VersionEntry &version = {}) {
    CacheEntry entry;
    entry.size = info.size();
    entry.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    entry.sha1 = sha1;
    entry.found = found;
    entry.hit = hit;
    entry.currentVersion = version;
    return entry;
}

/// 盘上那份的根：`cacheDir` 或 `cacheKey` 空 = 这一趟不用缓存（见 .hpp）。
bool cacheEnabled(const Options &options) {
    return !options.cacheDir.isEmpty() && !options.cacheKey.isEmpty();
}

/// 读盘上那份。读不动（没有 / 坏了 / 没开缓存）一律回**空表** —— 缓存读不出来只说明这一趟
/// 要多问几发，不是错。**读这一路上一个字节都不写**（写坏的那份留给下一次保存覆盖掉）。
QHash<QString, CacheEntry> loadCache(const Options &options) {
    QHash<QString, CacheEntry> out;
    const QString path = cachePath(options);
    if (path.isEmpty()) {
        return out;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return out;
    }
    const QByteArray bytes = file.readAll();
    file.close(); // 句柄不放手，后面换不掉这个文件（同 ResourceStore::loadIconFromCache 那条）

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return out;
    }
    const QJsonObject files =
        document.object().value(QStringLiteral("files")).toObject();
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject object = it.value().toObject();
        CacheEntry entry;
        entry.size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
        entry.mtimeMs =
            static_cast<qint64>(object.value(QStringLiteral("mtime")).toDouble());
        entry.sha1 = object.value(QStringLiteral("sha1")).toString();
        // **没有 `hit` 那一栏 = 问过了、没有**（见 .hpp 里那条 NOTE）。
        const QJsonValue hitValue = object.value(QStringLiteral("hit"));
        if (hitValue.isObject()) {
            entry.found = true;
            entry.hit = ResourceCatalog::hitFromJson(hitValue.toObject());
            const auto versions = ResourceCatalog::parseVersions(QJsonDocument(
                QJsonArray{object.value(QStringLiteral("currentVersion"))}).toJson());
            if (!versions.isEmpty()) entry.currentVersion = versions.first();
        }
        out.insert(it.key(), entry);
    }
    return out;
}

}  // namespace

QString cachePath(const Options &options) {
    if (!cacheEnabled(options)) {
        return QString();
    }
    const QByteArray digest =
        QCryptographicHash::hash(options.cacheKey.toUtf8(), QCryptographicHash::Sha1).toHex();
    return options.cacheDir + QLatin1Char('/') + QString::fromLatin1(digest) +
           QStringLiteral(".json");
}

bool isLookupCandidate(const QString &path) {
    QString name = QFileInfo(path).fileName();
    // 禁用着的模组叫 `X.jar.disabled` —— 尾巴上是那四个字，内容还是那个 jar。
    // 这个后缀的**唯一出处**是 InstanceFilesPage::disabledSuffix()，但那个在 ui/ 里
    // （这一层不许 include ui/，反过来的方向才是对的），所以这儿只认后缀的样子。
    if (name.endsWith(QStringLiteral(".disabled"))) {
        name.chop(QStringLiteral(".disabled").size());
    }
    return name.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive) ||
           name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive);
}

namespace {

/// 真正要发请求的那一趟。盘上那份命中与否由外面那个 `lookup()` 管，这一层只管
/// `paths` 里递进来的这些。
Result lookupNetwork(const QStringList &paths, const Options &options) {
    Result result;

    // ---------- ① 算哈希 ----------
    //
    // 一个 sha1 可能对应好几个路径（同一个 jar 摆了两份、或者 `X.jar` 和 `X.jar.disabled`
    // 同时存在），所以两张表都是"哈希 → 路径表"。
    QHash<QString, QStringList> pathsBySha1;
    QHash<quint32, QStringList> pathsByFingerprint;
    // 路径 → sha1。★ 这一位**不是为了认身份**（认身份用上面那两张
    // 表），是为了 `Found::sha1` —— 实例设置那个「检查更新」拿它判"盘上这一份是第几版"
    // （见 Found::sha1 那段）。键是**路径**：同一个文件摆了两份时两条路径都要拿到它。
    QHash<QString, QString> sha1ByPath;
    for (const QString &path : paths) {
        if (!isLookupCandidate(path)) {
            continue;
        }
        ModpackExport::FileHashes hashes;
        QString error;
        // needFingerprint = true：两个来源都要问，而算哈希要**读两趟文件**
        // （见 hashFile 的 NOTE）。一趟把 sha1 和指纹都算出来，比两条路各读一遍便宜。
        if (!ModpackExport::hashFile(path, true, &hashes, &error)) {
            ++result.unreadable;
            continue;
        }
        ++result.hashed;
        sha1ByPath.insert(path, hashes.sha1);
        pathsBySha1[hashes.sha1].append(path);
        if (hashes.fingerprintOk) {
            pathsByFingerprint[hashes.fingerprint].append(path);
        }
    }
    if (pathsBySha1.isEmpty()) {
        return result;
    }

    // 这一层自己 new 一个 QNAM：**它归调用它的那个线程**（见 .hpp 里那段"为什么不在
    // 工作线程里用 VersionManager 那两个 lookup"）。三发都走它。
    QNetworkAccessManager network;

    // ---------- ② Modrinth：sha1 → 项目号 ----------
    //
    // 这一趟只定"哪个文件是哪个项目"，**不再当场造 Found** —— 项目号到手时
    // 手上还没有名字 / slug / 简介 / 支持的游戏版本（那几样只在**项目对象**里，见 ②'）。
    // 记成"路径 → 项目号"，等详情回来再摊成一条一条。
    if (!pathsBySha1.isEmpty()) {
        QHash<QString, QString> projectByPath;
        QHash<QString, ResourceCatalog::VersionEntry> versionsByPath;
        QStringList wantedProjects;
        // 根地址：验证工程的口子优先，否则官方那一份（唯一出处仍是 ResourceCatalog 里
        // 那个 apiRoot()）—— 和 VersionManager 那边逐字同一条规矩。
        const QString root = options.modrinthApiBaseOverride.isEmpty()
                                 ? ResourceCatalog::modrinthApiRoot()
                                 : options.modrinthApiBaseOverride;
        const QList<QPair<QByteArray, QByteArray>> headers{{QByteArrayLiteral("User-Agent"), kUserAgent}};
        const QStringList sha1s = pathsBySha1.keys();
        for (int offset = 0; offset < sha1s.size(); offset += kLookupChunk) {
            const QByteArray body =
                ModpackExport::modrinthVersionFilesRequestBody(sha1s.mid(offset, kLookupChunk));
            if (body.isEmpty()) {
                continue;
            }
            const HttpClient::Reply reply = HttpClient::send(
                &network, QUrl(root + QStringLiteral("/version_files")), HttpClient::Verb::Post, body,
                QByteArrayLiteral("application/json"), QByteArray(), HttpClient::kDefaultTimeoutMs,
                headers);
            if (!reply.ok()) {
                // 这一发没成：**到此为止**（不再往下问 CurseForge），但**已经认出来的那些
                // 留着** —— 它们是真的，没理由因为后面这一发失败就把它们扔掉。
                //
                // 真正要紧的是 `problem` 立起来了：接的人按它决定"没认出来的那些**别**
                // 记成'问过了、没有'"。半真半假的一趟要是把负缓存写了，用户这一整个会话
                // 都再也看不到图标（见 Result::problem 那段）。
                result.problem = reply.networkError.isEmpty()
                                     ? QObject::tr("Modrinth 反查回了 HTTP %1。").arg(reply.status)
                                     : QObject::tr("Modrinth 反查没连上：%1").arg(reply.networkError);
                return result;
            }
            QString problem;
            const QHash<QString, ModpackExport::ModrinthHit> part =
                ModpackExport::parseModrinthVersionFiles(reply.raw, &problem);
            if (!problem.isEmpty()) {
                result.problem = problem;
                return result;
            }
            const auto versionMetadata = ResourceCatalog::parseVersionFilesUpdate(reply.raw);
            for (auto it = part.constBegin(); it != part.constEnd(); ++it) {
                // 这个哈希对应哪几个文件（见上面那段）。同名的两个都要有图标。
                const QString projectId = it.value().projectId;
                if (projectId.isEmpty()) {
                    continue;
                }
                // 只有**这一趟真的有文件对得上**的那个号才进 wantedProjects：这一发要的是
                // "这些文件分别属于哪个项目"，回一句里多出来的条目（对不上任何路径的）跟
                // 这一趟无关。按整份回复收号的话，那一发会**替不相干的号白问一次详情**
                // —— 请求地址更长、偶尔多一批 404，而屏幕上一点异常都没有（mw_check 的
                // T22.4b「同一个资源摆两份」那一条就盯在这里）。
                const QStringList paths = pathsBySha1.value(it.key());
                if (paths.isEmpty()) {
                    continue;
                }
                if (!wantedProjects.contains(projectId)) {
                    wantedProjects.append(projectId);
                }
                for (const QString &path : paths) {
                    projectByPath.insert(path, projectId);
                    versionsByPath.insert(path, versionMetadata.value(it.key()));
                }
            }
        }

        // ---------- ②' 这些项目号各自的**项目对象**（图标地址 + 详情）----------
        //
        // 这一发**必须收图标地址**，不能指望 `Hit::iconPngUrl()` 自己拼的
        // `cdn.modrinth.com/data/<id>/icon.png`：那条地址**只是老项目的别名**，
        // 绝大多数模组回 404，于是这一页上只有占位拼图，而屏幕上一点异常都没有
        // （404 被当成"这个项目没传过图标"—— 那本来是完全正常的一件事）。
        QHash<QString, ResourceCatalog::Hit> hitsByProject;
        for (int offset = 0; offset < wantedProjects.size(); offset += kLookupChunk) {
            const HttpClient::Reply reply = HttpClient::send(
                &network,
                QUrl(ResourceCatalog::projectsRequestUrl(wantedProjects.mid(offset, kLookupChunk),
                                                         root)),
                HttpClient::Verb::Get, QByteArray(), QByteArray(), QByteArray(),
                HttpClient::kDefaultTimeoutMs, headers);
            if (!reply.ok()) {
                // **不 return**（和上面那两处不一样）：身份已经定下来了 —— 上面那一发认
                // 出来的那些是真的，缺的只是详情与图，那几格退回占位拼图就是正确的界面。
                // 而且也不能往 `problem` 里写：它是"这一趟完整不完整"的判据，这一发失败
                // 不影响那个判断，写进去的代价是下一趟把几十兆 jar 从头重读一遍
                // （见 Result::iconProblem）。
                result.iconProblem = reply.networkError.isEmpty()
                                         ? QObject::tr("Modrinth 取图标地址时回了 HTTP %1。")
                                               .arg(reply.status)
                                         : QObject::tr("Modrinth 取图标地址时没连上：%1")
                                               .arg(reply.networkError);
                break;
            }
            for (const ResourceCatalog::Hit &hit : ResourceCatalog::parseProjects(reply.raw)) {
                hitsByProject.insert(hit.projectId, hit);
            }
        }
        // ②'' 摊成"一个文件一条"。
        for (auto it = projectByPath.constBegin(); it != projectByPath.constEnd(); ++it) {
            Found found;
            found.path = it.key();
            found.sha1 = sha1ByPath.value(it.key());
            found.currentVersion = versionsByPath.value(it.key());
            found.hit = hitsByProject.value(it.value());
            // 详情那一发没成、或者回复里没有这个号：**照样认这一条** —— 身份（项目号 +
            // 来源）是从哈希定下来的、那一步已经做完，缺的只是详情（名字空着 ⇒ 那一行
            // 的名字点不动，见 MainWindow 那段）与图标（退回 iconPngUrl() 那条兜底）。
            found.hit.projectId = it.value();
            found.hit.source = ResourceCatalog::Source::Modrinth;
            result.found.append(found);
        }
    }

    // ---------- ③ CurseForge：指纹 → 文件 → 项目 → logo ----------
    if (!pathsByFingerprint.isEmpty()) {
        const QString base = ModpackImport::curseForgeApiBase(options.curseForgeApiKey,
                                                              options.curseForgeApiBaseOverride);
        const bool hasKey = !options.curseForgeApiKey.isEmpty();
        QList<QPair<QByteArray, QByteArray>> headers;
        if (hasKey) {
            headers.append({QByteArrayLiteral("x-api-key"), options.curseForgeApiKey.toUtf8()});
        }

        // 指纹 → 项目号。和 Modrinth 那一支一样分批发（同一台接口的同一个上限）。
        QHash<quint32, int> projectByFingerprint;
        QHash<quint32, ResourceCatalog::VersionEntry> versionsByFingerprint;
        const QList<quint32> fingerprints = pathsByFingerprint.keys();
        for (int offset = 0; offset < fingerprints.size(); offset += kLookupChunk) {
            const QByteArray body = ModpackExport::curseForgeFingerprintsRequestBody(
                fingerprints.mid(offset, kLookupChunk));
            if (body.isEmpty()) {
                continue;
            }
            const HttpClient::Reply reply = HttpClient::send(
                &network, QUrl(base + QStringLiteral("/fingerprints")), HttpClient::Verb::Post, body,
                QByteArrayLiteral("application/json"), QByteArray(), HttpClient::kDefaultTimeoutMs,
                headers);
            if (!reply.ok()) {
                // 同上：已经认出来的留着，problem 立起来（那一句是给日志的）。
                result.problem = reply.networkError.isEmpty()
                                     ? QObject::tr("CurseForge 反查回了 HTTP %1。").arg(reply.status)
                                     : QObject::tr("CurseForge 反查没连上：%1").arg(reply.networkError);
                return result;
            }
            QString problem;
            const QHash<quint32, ModpackExport::CurseForgeHit> part =
                ModpackExport::parseCurseForgeFingerprints(reply.raw, &problem);
            const auto matches = QJsonDocument::fromJson(reply.raw).object()
                .value(QStringLiteral("data")).toObject().value(QStringLiteral("exactMatches")).toArray();
            for (const auto &match : matches) {
                const auto file = match.toObject().value(QStringLiteral("file")).toObject();
                const auto number = file.value(QStringLiteral("fileFingerprint")).toInteger(-1);
                if (number < 0 || number > 4294967295LL ||
                    file.value(QStringLiteral("fileFingerprint")).toDouble() != double(number)) continue;
                const auto fp = static_cast<quint32>(number);
                if (!part.contains(fp)) continue;
                const auto versions = ResourceCatalog::parseCurseForgeVersions(QJsonDocument(
                    QJsonObject{{QStringLiteral("data"), QJsonArray{file}}}).toJson());
                if (!versions.isEmpty()) versionsByFingerprint.insert(fp, versions.first());
            }
            if (!problem.isEmpty()) {
                result.problem = problem;
                return result;
            }
            for (auto it = part.constBegin(); it != part.constEnd(); ++it) {
                if (it.value().projectId > 0) {
                    projectByFingerprint.insert(it.key(), it.value().projectId);
                }
            }
        }

        // 项目号 → **项目对象**（logo 那两条地址，以及名字 / slug / 简介 / 游戏版本）。
        // **只有认出来的项目才问**（十几个号一发就够）。
        QList<int> projectIds;
        QSet<int> seenProjectIds;
        for (auto it = projectByFingerprint.constBegin(); it != projectByFingerprint.constEnd();
             ++it) {
            const int projectId = it.value();
            if (seenProjectIds.contains(projectId)) {
                continue;
            }
            seenProjectIds.insert(projectId);
            projectIds.append(projectId);
        }
        QHash<int, ResourceCatalog::Hit> hitsByProject;
        for (int offset = 0; offset < projectIds.size(); offset += kLookupChunk) {
            const QByteArray body =
                ModpackImport::curseForgeModsRequestBody(projectIds.mid(offset, kLookupChunk));
            if (body.isEmpty()) {
                continue;
            }
            const HttpClient::Reply reply = HttpClient::send(
                &network, QUrl(base + QStringLiteral("/mods")), HttpClient::Verb::Post, body,
                QByteArrayLiteral("application/json"), QByteArray(), HttpClient::kDefaultTimeoutMs,
                headers);
            if (!reply.ok()) {
                // 这一发只影响"详情与图标拿不拿得到"，不改变"这是哪个资源"（那一步已经定了）。
                // 所以**认出来的那些照样往下走**（名字空着 ⇒ 那一行的名字点不动，图标退回
                // 占位拼图），只把 iconProblem 立起来。
                //
                // 这里写的是 `iconProblem` 而不是 `problem`（那个字段是**整趟完整不完整**
                // 的判据，见 Result::iconProblem 那段）：这一发失败不该让整趟作废，否则
                // 每开一次设置页都要把几十兆 jar 从头重读重算一遍，换来的只是"图标可能来了"。
                result.iconProblem = reply.networkError.isEmpty()
                                         ? QObject::tr("CurseForge 取图标时回了 HTTP %1。")
                                               .arg(reply.status)
                                         : QObject::tr("CurseForge 取图标时没连上：%1")
                                               .arg(reply.networkError);
                break;
            }
            for (const ResourceCatalog::Hit &hit : ResourceCatalog::parseCurseForgeMods(reply.raw)) {
                hitsByProject.insert(hit.projectId.toInt(), hit);
            }
        }

        for (auto it = projectByFingerprint.constBegin(); it != projectByFingerprint.constEnd();
             ++it) {
            const int projectId = it.value();
            for (const QString &path : pathsByFingerprint.value(it.key())) {
                Found found;
                found.path = path;
                found.sha1 = sha1ByPath.value(path);
                found.currentVersion = versionsByFingerprint.value(it.key());
                found.hit = hitsByProject.value(projectId);
                // 详情那一发没成 / 那个号在回复里缺席也**照样认这一条**：图标是空的 ⇒
                // ResourceStore 发一张空 pixmap ⇒ 那一格退回占位拼图。比"整个来源都不认"
                // 好 —— 项目号和来源还是对的（那是指纹那一步定下来的）。
                found.hit.projectId = QString::number(projectId);
                found.hit.source = ResourceCatalog::Source::CurseForge;
                result.found.append(found);
            }
        }
    }

    return result;
}

/// 把这一趟的结果并回盘上那份、写回去（见 .hpp 里那段）。
///
/// 三条规矩：
/// * **盘上那些照旧留着**（这一趟没问它们）；「这一趟问过、这一趟有结论」的那些才覆盖掉。
/// * **阴性只在整趟跑完时才记**（`problem` 空）—— 断网那一趟记下去，用户网好了也看不到
/// 图标（见 `Result::problem` 那段）。
/// * 盘上那个文件已经不在的就**顺手丢掉**（不然一份缓存会越攒越大，而它记的都是些早被
/// 用户删掉的文件）。
///
/// NOTE: 判"文件还在不在"要**两个名字都试**（身份路径，以及它 + `.disabled`）—— 身份路径
/// 是摘过后缀的那一个，禁用着的模组在盘上叫另一个名字。
void saveCache(const Options &options, const QStringList &paths, const Result &result,
               const QHash<QString, CacheEntry> &cached) {
    if (!cacheEnabled(options)) {
        return;
    }
    // 这一趟真问出来的那些：路径 → 结论（只在"问到底了"的前提下才记阴性）。
    QHash<QString, Found> foundByPath;
    for (const Found &found : result.found) {
        foundByPath.insert(identityPath(found.path), found);
    }
    const bool complete = result.problem.isEmpty();

    // 盘上那份：还在的留着，不在的丢掉。
    QHash<QString, CacheEntry> merged;
    for (auto it = cached.constBegin(); it != cached.constEnd(); ++it) {
        if (QFileInfo::exists(it.key()) ||
            QFileInfo::exists(it.key() + QLatin1String(kDisabledSuffix))) {
            merged.insert(it.key(), it.value());
        }
    }

    for (const QString &path : paths) {
        if (!isLookupCandidate(path)) {
            continue;
        }
        const QFileInfo info(path);
        if (!info.exists()) {
            continue;
        }
        const QString identity = identityPath(path);
        const auto found = foundByPath.constFind(identity);
        if (found != foundByPath.constEnd()) {
            merged.insert(identity, entryFor(info, found.value().sha1, true, found.value().hit,
                                            found.value().currentVersion));
            continue;
        }
        // 这一趟里没有它 ⇒ 两种可能：① 刚从盘上那份命中的（原样留着，别去动它）；
        // ② 真问了、没认出来 —— 那就记一条阴性，**但只在整趟完整跑完时**。
        const auto existing = merged.constFind(identity);
        if (existing != merged.constEnd() && sameFile(existing.value(), info)) {
            continue;
        }
        if (complete) {
            merged.insert(identity, entryFor(info, QString(), false, ResourceCatalog::Hit()));
        }
    }

    QJsonObject files;
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
        QJsonObject object;
        object.insert(QStringLiteral("size"), static_cast<double>(it.value().size));
        object.insert(QStringLiteral("mtime"), static_cast<double>(it.value().mtimeMs));
        if (!it.value().sha1.isEmpty()) {
            object.insert(QStringLiteral("sha1"), it.value().sha1);
        }
        if (it.value().found) {
            object.insert(QStringLiteral("hit"), ResourceCatalog::hitToJson(it.value().hit));
            const auto &version = it.value().currentVersion;
            QJsonObject metadata{{QStringLiteral("id"), version.id},
                                 {QStringLiteral("version_number"), version.versionNumber},
                                 {QStringLiteral("name"), version.name}};
            if (version.dependencyMetadataKnown) {
                QJsonArray dependencies;
                for (const auto &dep : version.dependencies) dependencies.append(QJsonObject{
                    {QStringLiteral("project_id"), dep.projectId}, {QStringLiteral("version_id"), dep.versionId},
                    {QStringLiteral("dependency_type"), dep.type}});
                metadata.insert(QStringLiteral("dependencies"), dependencies);
            }
            object.insert(QStringLiteral("currentVersion"), metadata);
        }
        files.insert(it.key(), object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("files"), files);

    const QString path = cachePath(options);
    // QSaveFile：先写旁边的临时文件、成了再换上去 —— 同一个实例的两趟（图标那一趟和
    // 「检查更新」那一趟）可能同时在跑，直接 `Truncate` 的话读的人有机会读到半份。
    // 那份坏字节读出来只会让这一趟多问几发（loadCache 一律当"没有"），但没必要留着它。
    QDir().mkpath(options.cacheDir);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return; // best-effort：写不进去只是下次多问几发，不是错
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

}  // namespace

Result lookup(const QStringList &paths, const Options &options) {
    // ---------- ① 盘上那份：命中就**一个字节的网络都不走** ----------
    //
    // 见 .hpp 里【结果留一份在盘上】那段。判"命中"要两样都对：**还是那个文件**
    // （大小 + 修改时间，`sameFile()`）**而且**这一条问到底过。
    const QHash<QString, CacheEntry> cached = loadCache(options);
    Result result;
    QStringList todo;  // 盘上那份没有的：交给下面真正要发请求的那一趟
    todo.reserve(paths.size());
    for (const QString &path : paths) {
        if (!isLookupCandidate(path)) {
            continue;
        }
        const QString identity = identityPath(path);
        const auto it = cached.constFind(identity);
        if (it == cached.constEnd() || !sameFile(it.value(), QFileInfo(path)) ||
            (options.requireVersionMetadata && !options.cacheOnly && it.value().found && it.value().currentVersion.id.isEmpty())) {
            todo.append(path);
            continue;
        }
        if (it.value().found) {
            Found found;
            // 递进去的那个**盘上真那个名字**原样回来（调用方按它认行）。
            found.path = path;
            found.sha1 = it.value().sha1;
            found.hit = it.value().hit;
            found.currentVersion = it.value().currentVersion;
            result.found.append(found);
        }
        // found == false = **问过了、这两家都没有**：这一条到此为止（既不算 found，
        // 也不算 unreadable）—— 接的人按"不在 found 里"记它，和当场问出来的一样。
    }

    // ---------- ② 剩下的：算哈希 + 问接口 ----------
    if (!todo.isEmpty() && !options.cacheOnly) {
        const Result fresh = lookupNetwork(todo, options);
        // 两份合起来（顺序不定，见 Result::found 那段）。
        result.found.append(fresh.found);
        result.hashed = fresh.hashed;
        result.unreadable = fresh.unreadable;
        // 后两样是**整趟**的性质（"这一趟完整不完整"），取那一趟的：
        // 盘上命中的那些不需要网络，也就谈不上"这一趟通不通"。
        result.problem = fresh.problem;
        result.iconProblem = fresh.iconProblem;
    }

    // ---------- ③ 并回去、写在盘上 ----------
    //
    // 摆在这一整个函数的**最后**（而不是每个 return 之前）：上面那两个来源有好几条早退路
    // （接口挂了、正文读不懂），而**没认出来的那些不该因为一次断网就被判成"没有"**
    // —— 那件事由 saveCache 里 `complete` 那一条管（见它那段）。
    if (!options.cacheOnly) saveCache(options, paths, result, cached);
    return result;
}

}  // namespace InstanceIconLookup

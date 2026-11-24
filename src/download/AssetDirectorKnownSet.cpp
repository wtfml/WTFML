#include "AssetDirectorKnownSet.hpp"
#include <QCoreApplication>

#include "../download/DownloadManager.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace AssetDirectorKnownSet {
namespace {

/// 清单本身的上限：一份 756 个对象的清单约 110 KB，4 MiB 足够挡住"拿别的东西冒充"。
constexpr int kMaxManifestBytes = 4 * 1024 * 1024;
constexpr int kMaxSets = 16;
constexpr int kMaxObjectsPerSet = 8192;
constexpr int kMaxPatternsPerSet = 8;
constexpr int kMaxPatternLength = 200;
/// 单件资源对象的上限。实测最大的那几个是音乐/唱片 ogg（几 MB），64 MiB 已经很宽松。
constexpr qint64 kMaxObjectBytes = 64LL * 1024 * 1024;
/// 版本 JSON / 资源索引这几份的上限（1.21 的索引 449 KB，1.21.10 的 508 KB）。
constexpr qint64 kMaxJsonBytes = 16LL * 1024 * 1024;

bool sha1Valid(const QString &value, QString *normalized) {
    static const QRegularExpression hash(QStringLiteral("^[a-fA-F0-9]{40}$"));
    if (!hash.match(value).hasMatch()) {
        return false;
    }
    if (normalized != nullptr) {
        *normalized = value.toLower();
    }
    return true;
}

/// 只接受 Mojang 自己的 HTTPS 主机：清单是随产品发布的，地址被人改过就等于把用户
/// 送到别处去下东西。
bool officialUrl(const QString &text) {
    const QUrl url(text);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty()
        || !url.userInfo().isEmpty() || !url.fragment().isEmpty()) {
        return false;
    }
    static const QSet<QString> hosts{QStringLiteral("piston-meta.mojang.com"),
                                     QStringLiteral("launchermeta.mojang.com"),
                                     QStringLiteral("resources.download.minecraft.net")};
    return hosts.contains(url.host());
}

bool fileRef(const QJsonValue &value, const QString &field, FileRef *out, QString *error) {
    if (!value.isObject()) {
        *error = QCoreApplication::translate("AssetDirectorKnownSet", "%1 不是对象。").arg(field);
        return false;
    }
    const QJsonObject object = value.toObject();
    out->url = object.value(QStringLiteral("url")).toString();
    out->size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble(-1));
    if (!officialUrl(out->url)) {
        *error = QCoreApplication::translate("AssetDirectorKnownSet", "%1 的地址不是 Mojang 官方 HTTPS 地址。").arg(field);
        return false;
    }
    if (!sha1Valid(object.value(QStringLiteral("sha1")).toString(), &out->sha1)) {
        *error = QCoreApplication::translate("AssetDirectorKnownSet", "%1 的 SHA-1 不是 40 位十六进制。").arg(field);
        return false;
    }
    if (out->size <= 0 || out->size > kMaxJsonBytes) {
        *error = QCoreApplication::translate("AssetDirectorKnownSet", "%1 的字节数不合法。").arg(field);
        return false;
    }
    return true;
}

bool objectNameValid(const QString &name) {
    if (name.isEmpty() || name.size() > 256 || name.startsWith(QLatin1Char('/'))
        || name.contains(QLatin1String("..")) || name.contains(QLatin1String("//"))) {
        return false;
    }
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_./-]+$"));
    return safe.match(name).hasMatch();
}

/// 路径段：只给合法哈希拼路径，所以这里只要 40 位十六进制。
QString hashPrefix(const QString &hash) { return hash.left(2); }

}  // namespace

bool parse(const QByteArray &json, Manifest *out, QString *error) {
    const auto fail = [&](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (out == nullptr) {
        return fail(QCoreApplication::translate("AssetDirectorKnownSet", "未提供清单结果。"));
    }
    out->sets.clear();
    if (json.isEmpty() || json.size() > kMaxManifestBytes) {
        return fail(QCoreApplication::translate("AssetDirectorKnownSet", "已知集合清单为空或过大。"));
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (!document.isObject()) {
        return fail(QCoreApplication::translate("AssetDirectorKnownSet", "已知集合清单不是 JSON 对象（%1）。").arg(parseError.errorString()));
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("formatVersion")).toInt(-1) != kFormatVersion) {
        return fail(QCoreApplication::translate("AssetDirectorKnownSet", "不支持此已知资源清单的格式版本。"));
    }
    const QJsonValue setsValue = root.value(QStringLiteral("knownSets"));
    if (!setsValue.isArray() || setsValue.toArray().isEmpty() || setsValue.toArray().size() > kMaxSets) {
        return fail(QCoreApplication::translate("AssetDirectorKnownSet", "已知集合清单中无可用条目。"));
    }
    QSet<QString> ids;
    QSet<QString> hashes;
    for (const QJsonValue &entry : setsValue.toArray()) {
        if (!entry.isObject()) {
            return fail(QCoreApplication::translate("AssetDirectorKnownSet", "已知集合条目不是对象。"));
        }
        const QJsonObject object = entry.toObject();
        Set set;
        set.id = object.value(QStringLiteral("id")).toString();
        static const QRegularExpression idPattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
        if (!idPattern.match(set.id).hasMatch() || ids.contains(set.id)) {
            return fail(QCoreApplication::translate("AssetDirectorKnownSet", "已知集合的 id 不合法或重复：%1").arg(set.id));
        }
        ids.insert(set.id);
        set.note = object.value(QStringLiteral("note")).toString().left(200);
        set.minecraftVersion = object.value(QStringLiteral("minecraftVersion")).toString();
        static const QRegularExpression versionPattern(QStringLiteral("^[0-9A-Za-z._-]{1,32}$"));
        if (!versionPattern.match(set.minecraftVersion).hasMatch()) {
            return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 的 MC 版本不合法。").arg(set.id));
        }
        const QJsonValue patternsValue = object.value(QStringLiteral("consumerPatterns"));
        if (!patternsValue.isArray() || patternsValue.toArray().isEmpty()
            || patternsValue.toArray().size() > kMaxPatternsPerSet) {
            return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 无可用消费者匹配规则。").arg(set.id));
        }
        for (const QJsonValue &patternValue : patternsValue.toArray()) {
            const QString pattern = patternValue.toString();
            if (pattern.isEmpty() || pattern.size() > kMaxPatternLength) {
                return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 的消费者匹配规则不合法。").arg(set.id));
            }
            const QRegularExpression expression(pattern);
            if (!expression.isValid()) {
                return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 的消费者匹配规则无效。").arg(set.id));
            }
            set.consumerPatterns.append(pattern);
        }
        if (!fileRef(object.value(QStringLiteral("versionJson")), QCoreApplication::translate("AssetDirectorKnownSet", "%1 的版本 JSON").arg(set.id),
                     &set.versionJson, error)) {
            return false;
        }
        const QJsonValue indexValue = object.value(QStringLiteral("assetIndex"));
        if (!indexValue.isObject()) {
            return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 无资源索引。").arg(set.id));
        }
        set.assetIndexId = indexValue.toObject().value(QStringLiteral("id")).toString();
        static const QRegularExpression indexIdPattern(QStringLiteral("^[0-9A-Za-z._-]{1,64}$"));
        if (!indexIdPattern.match(set.assetIndexId).hasMatch()) {
            return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 的资源索引 id 不合法。").arg(set.id));
        }
        if (!fileRef(indexValue, QCoreApplication::translate("AssetDirectorKnownSet", "%1 的资源索引").arg(set.id), &set.assetIndex, error)) {
            return false;
        }
        const QJsonValue objectsValue = object.value(QStringLiteral("objects"));
        if (!objectsValue.isArray() || objectsValue.toArray().isEmpty()
            || objectsValue.toArray().size() > kMaxObjectsPerSet) {
            return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 的对象列表为空或过大。").arg(set.id));
        }
        QSet<QString> setHashes;
        qint64 sum = 0;
        for (const QJsonValue &objectValue : objectsValue.toArray()) {
            if (!objectValue.isObject()) {
                return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 的对象条目不是对象。").arg(set.id));
            }
            const QJsonObject item = objectValue.toObject();
            Object one;
            one.name = item.value(QStringLiteral("name")).toString();
            one.size = static_cast<qint64>(item.value(QStringLiteral("size")).toDouble(-1));
            if (!objectNameValid(one.name)) {
                return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 里有不合法的对象名。").arg(set.id));
            }
            if (!sha1Valid(item.value(QStringLiteral("hash")).toString(), &one.hash)) {
                return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 里 %2 的哈希不是 40 位十六进制。").arg(set.id, one.name));
            }
            if (one.size <= 0 || one.size > kMaxObjectBytes) {
                return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 里 %2 的字节数不合法。").arg(set.id, one.name));
            }
            if (setHashes.contains(one.hash)) {
                return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 里同一个哈希出现了两次。").arg(set.id));
            }
            setHashes.insert(one.hash);
            hashes.insert(one.hash);
            sum += one.size;
            set.objects.append(one);
        }
        // 声明的总量必须和逐项之和对得上：对不上说明这份清单是半截生成的，
        // 而"少算的字节数"正是上限判据要用的那一格。
        const double declared = object.value(QStringLiteral("totalBytes")).toDouble(-1);
        if (declared != static_cast<double>(sum)) {
            return fail(QCoreApplication::translate("AssetDirectorKnownSet", "%1 的总大小与对象列表不一致。").arg(set.id));
        }
        set.totalBytes = sum;
        out->sets.append(set);
    }
    return true;
}

bool loadEmbedded(Manifest *out, QString *error) {
    return parse(embeddedManifest(), out, error);
}

const Set *matchConsumer(const Manifest &manifest, const QStringList &modFileNames) {
    for (const Set &set : manifest.sets) {
        for (const QString &pattern : set.consumerPatterns) {
            const QRegularExpression expression(pattern);
            if (!expression.isValid()) {
                continue;
            }
            for (const QString &name : modFileNames) {
                if (expression.match(name).hasMatch()) {
                    return &set;
                }
            }
        }
    }
    return nullptr;
}

QString overlayDir(const QString &assetsRoot) {
    return assetsRoot + QStringLiteral("/asset_director");
}

QString overlayObjectPath(const QString &assetsRoot, const QString &hash) {
    return overlayDir(assetsRoot) + QStringLiteral("/assets/objects/") + hashPrefix(hash)
           + QLatin1Char('/') + hash;
}

QString overlayVersionJsonPath(const QString &assetsRoot, const QString &minecraftVersion) {
    return overlayDir(assetsRoot) + QStringLiteral("/versions/") + minecraftVersion + QLatin1Char('/')
           + minecraftVersion + QStringLiteral(".json");
}

QString overlayAssetIndexPath(const QString &assetsRoot, const QString &assetIndexId) {
    return overlayDir(assetsRoot) + QStringLiteral("/assets/indexes/") + assetIndexId
           + QStringLiteral(".json");
}

QString objectUrl(const QString &hash) {
    return QStringLiteral("https://resources.download.minecraft.net/") + hashPrefix(hash)
           + QLatin1Char('/') + hash;
}

Plan plan(const Set &set, const QString &assetsRoot, const Validator &valid) {
    Plan result;
    result.objects = static_cast<int>(set.objects.size());
    result.bytesTotal = set.totalBytes;
    if (set.totalBytes > kMaxKnownSetBytes) {
        result.refusal = QCoreApplication::translate("AssetDirectorKnownSet", "这套已知集合有 %1 MiB，超过了允许自动下载的上限 %2 MiB。")
                             .arg(set.totalBytes / (1024.0 * 1024.0), 0, 'f', 1)
                             .arg(kMaxKnownSetBytes / (1024 * 1024));
        return result;
    }
    const Validator check = valid
        ? valid
        : [](const QString &path, qint64 size, const QString &sha1) {
              const QFileInfo info(path);
              if (!info.exists() || !info.isFile() || info.isSymLink()) {
                  return false;
              }
              if (size > 0 && info.size() != size) {
                  return false;
              }
              return sha1.isEmpty() || DownloadManager::calculateFileSha1(path) == sha1;
          };
    const QString sharedObjects = assetsRoot + QStringLiteral("/objects/");
    qint64 bytes = 0;
    for (const Object &object : set.objects) {
        if (check(overlayObjectPath(assetsRoot, object.hash), object.size, object.hash)) {
            ++result.reuseOverlay;
            continue;
        }
        // AD 自己也读共享那份（只读、从不写），所以这里只判"在不在、对不对"。
        if (check(sharedObjects + hashPrefix(object.hash) + QLatin1Char('/') + object.hash,
                  object.size, object.hash)) {
            ++result.reuseAssets;
            continue;
        }
        result.missing.append(object);
        bytes += object.size;
    }
    result.toDownload = static_cast<int>(result.missing.size());
    result.needVersionJson = !check(overlayVersionJsonPath(assetsRoot, set.minecraftVersion),
                                    set.versionJson.size, set.versionJson.sha1);
    result.needAssetIndex = !check(overlayAssetIndexPath(assetsRoot, set.assetIndexId),
                                   set.assetIndex.size, set.assetIndex.sha1);
    if (result.needVersionJson) {
        bytes += set.versionJson.size;
    }
    if (result.needAssetIndex) {
        bytes += set.assetIndex.size;
    }
    result.bytesToDownload = bytes;
    return result;
}

}  // namespace AssetDirectorKnownSet

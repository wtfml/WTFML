/**
 * @file UpdatePolicy.cpp
 * @brief 实现「忽略 / 固定」那本账（格式与取舍见 .hpp）。
 */

#include "manage/UpdatePolicy.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QObject>
#include <QSaveFile>
#include "util/WriteDirectoryGuard.hpp"

#include "util/AppPaths.hpp"

namespace UpdatePolicy {

namespace {

/// 文件里认的格式版本。将来格式真变了（比如要记"忽略到什么时候为止"）就 +1，而不认识的
/// 版本一律当读不懂（和 `InstanceGroupStore` 那条一个规矩）。
constexpr int kVersion = 1;

const QString kVersionField = QStringLiteral("version");
const QString kInstancesField = QStringLiteral("instances");
const QString kIgnoreField = QStringLiteral("ignore");
const QString kPinField = QStringLiteral("pin");

/// 一遍收尾：空的记录、空的实例都不留在表里（盘上那份文件要干净；留空壳的话，一次"忽略
/// 了再取消"会在文件里留下一个空对象，用户拿记事本打开会以为没删掉）。
void compact(Store &store) {
    for (auto it = store.byInstance.begin(); it != store.byInstance.end();) {
        QHash<QString, Entry> &entries = it.value();
        for (auto e = entries.begin(); e != entries.end();) {
            if (e.value().isEmpty() || e.key().isEmpty()) {
                e = entries.erase(e);
            } else {
                ++e;
            }
        }
        if (entries.isEmpty()) {
            it = store.byInstance.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

Suppress suppressed(const Entry &entry, const QString &currentSha1,
                    const QString &targetVersionId) {
    // 固定先判：用户按的是"我就停在这儿"，它比"忽略这一版"更强（这一版、下一版、再下一版
    // 都别提）。两个都记着的时候，屏幕上那句也按固定说 —— 见 .hpp 里两档各是什么那段。
    if (!entry.pinnedSha1.isEmpty() && !currentSha1.isEmpty()
        && entry.pinnedSha1 == currentSha1) {
        return Suppress::Pinned;
    }
    if (!entry.ignoreVersionId.isEmpty() && !targetVersionId.isEmpty()
        && entry.ignoreVersionId == targetVersionId) {
        return Suppress::Ignored;
    }
    return Suppress::None;
}

bool pinIsStale(const Entry &entry, const QString &currentSha1) {
    if (entry.pinnedSha1.isEmpty()) {
        return false;
    }
    // 不知道现在盘上是哪一份（读不动、sha1 没算出来）时**不算过期** —— "不知道"不能当成
    // "换了"：那会把用户的固定悄悄删掉（清一条记录是不可逆的）。
    if (currentSha1.isEmpty()) {
        return false;
    }
    return entry.pinnedSha1 != currentSha1;
}

int dropStalePins(Store &store, const QString &instanceId,
                  const QHash<QString, QString> &currentSha1ByKey) {
    auto it = store.byInstance.find(instanceId);
    if (it == store.byInstance.end()) {
        return 0;
    }
    int dropped = 0;
    QHash<QString, Entry> &entries = it.value();
    for (auto e = entries.begin(); e != entries.end();) {
        // 这一趟**没有**这个键（文件被删了 / 挪到别的目录去了）时清掉：那条记录已经
        // 指不到任何东西了，留着只是让文件越来越大。
        const QString sha1 = currentSha1ByKey.value(e.key());
        if (pinIsStale(e.value(), sha1) || (sha1.isEmpty() && !currentSha1ByKey.contains(e.key()))) {
            e.value().pinnedSha1.clear();
        }
        if (e.value().isEmpty()) {
            e = entries.erase(e);
            ++dropped;
        } else {
            ++e;
        }
    }
    if (entries.isEmpty()) {
        store.byInstance.erase(it);
    }
    return dropped;
}

void setIgnored(Store &store, const QString &instanceId, const QString &key,
                const QString &targetVersionId) {
    if (instanceId.isEmpty() || key.isEmpty()) {
        return;
    }
    Entry &entry = store.byInstance[instanceId][key];
    entry.ignoreVersionId = targetVersionId;
    if (entry.isEmpty()) {
        store.byInstance[instanceId].remove(key);
        compact(store);
    }
}

void setPinned(Store &store, const QString &instanceId, const QString &key,
               const QString &pinnedSha1) {
    if (instanceId.isEmpty() || key.isEmpty()) {
        return;
    }
    Entry &entry = store.byInstance[instanceId][key];
    entry.pinnedSha1 = pinnedSha1;
    if (entry.isEmpty()) {
        store.byInstance[instanceId].remove(key);
        compact(store);
    }
}

Entry entryOf(const Store &store, const QString &instanceId, const QString &key) {
    return store.byInstance.value(instanceId).value(key);
}

QString storeKey(const QString &instanceDir, const QString &absolutePath) {
    if (instanceDir.isEmpty() || absolutePath.isEmpty()) {
        return QString();
    }
    // 反斜杠**全都正过来**（QDir 在 Windows 上回的相对路径就是反斜杠的）：键是跨机器读的
    // （实例目录整个拷过去、换一份 .minecraft），两套分隔符混在一本账里迟早对不上。
    const QString relative = QDir(instanceDir).relativeFilePath(absolutePath);
    if (relative.isEmpty()) {
        return QString();
    }
    const QString unified = QString(relative).replace(QLatin1Char('\\'), QLatin1Char('/'));
    // 不在这个实例底下（`..` 打头 / 绝对路径 / 盘符）：**回空串让调用方跳过**，别记一个
    // 指向实例外面的键 —— 那种键还原/清理时都会走到实例目录外面去。
    if (unified.startsWith(QStringLiteral("..")) || QDir::isAbsolutePath(unified)
        || unified.contains(QLatin1Char(':'))) {
        return QString();
    }
    return unified;
}

QString storePath() {
    return AppPaths::minecraftDir() + QStringLiteral("/wtfml.updates");
}

QString formatTag() { return QStringLiteral("wtfml-updates"); }

QByteArray toJson(const Store &store) {
    Store copy = store;
    compact(copy);
    QJsonObject root;
    root.insert(QStringLiteral("format"), formatTag());
    root.insert(kVersionField, kVersion);
    QJsonObject instances;
    for (auto it = copy.byInstance.constBegin(); it != copy.byInstance.constEnd(); ++it) {
        QJsonObject one;
        for (auto e = it.value().constBegin(); e != it.value().constEnd(); ++e) {
            QJsonObject record;
            if (!e.value().ignoreVersionId.isEmpty()) {
                record.insert(kIgnoreField, e.value().ignoreVersionId);
            }
            if (!e.value().pinnedSha1.isEmpty()) {
                record.insert(kPinField, e.value().pinnedSha1);
            }
            one.insert(e.key(), record);
        }
        instances.insert(it.key(), one);
    }
    root.insert(kInstancesField, instances);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

Store fromJson(const QByteArray &bytes, bool *ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    Store store;
    if (bytes.trimmed().isEmpty()) {
        if (ok != nullptr) {
            *ok = true; // 空文件 = 空账（不是"读不懂"）
        }
        return store;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return store;
    }
    const QJsonObject root = document.object();
    // 格式标记不认识（别人的文件、以后改过格式的文件）⇒ 当"读不懂"：一个记录都不往里
    // 加。半信半疑地读进来，最坏的结果是**把用户手上那几十个文件按一份莫名其妙的名单
    // 判成"已忽略"**，而他什么都看不出来。
    if (root.value(QStringLiteral("format")).toString() != formatTag()) {
        return store;
    }
    if (root.value(kVersionField).toInt(-1) != kVersion) {
        return store;
    }
    const QJsonObject instances = root.value(kInstancesField).toObject();
    for (auto it = instances.constBegin(); it != instances.constEnd(); ++it) {
        const QJsonObject one = it.value().toObject();
        for (auto e = one.constBegin(); e != one.constEnd(); ++e) {
            const QJsonObject record = e.value().toObject();
            Entry entry;
            entry.ignoreVersionId = record.value(kIgnoreField).toString();
            entry.pinnedSha1 = record.value(kPinField).toString();
            if (!entry.isEmpty()) {
                store.byInstance[it.key()].insert(e.key(), entry);
            }
        }
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return store;
}

Store load(const QString &path, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    QFile file(path);
    if (!file.exists()) {
        return Store{};  // 从来没忽略过谁就是这样，不是错误
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("读取「%1」失败：%2").arg(path, file.errorString());
        }
        return Store{};
    }
    const QByteArray bytes = file.readAll();
    file.close();
    bool ok = false;
    const Store store = fromJson(bytes, &ok);
    if (!ok && problemOut != nullptr) {
        *problemOut = QObject::tr("无法解析「%1」（不是本程序写入的格式），本次按没有任何记录处理。")
                          .arg(path);
    }
    return store;
}

bool save(const Store &store, const QString &path, QString *problemOut) {
    if (!WriteDirectoryGuard::acquire(path, problemOut)) return false;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("创建目录「%1」失败。").arg(info.absolutePath());
        }
        return false;
    }
    // 原子写：写到一半被打断（关启动器、断电）留下的半截 json 会被下一次读成"读不懂" ——
    // 而"读不懂"按空账算，于是用户那几十条忽略/固定**全没了**。QSaveFile 不会留下半截。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("写不动「%1」：%2").arg(path, file.errorString());
        }
        return false;
    }
    const QByteArray bytes = toJson(store);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("「%1」写入不完整：%2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

}  // namespace UpdatePolicy

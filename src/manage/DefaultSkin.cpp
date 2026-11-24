/**
 * @file DefaultSkin.cpp
 * @brief DefaultSkin 的实现。
 * @note 两套规矩、18 条路径、Java 那个哈希的来历，全在 .hpp 文件头。这里只写"怎么落盘"。
 */
#include "DefaultSkin.hpp"
#include <QCoreApplication>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

#include <cstring>

#include "../auth/AccountStore.hpp"  // offlineUuidFor()：离线身份只有这一个出处
#include "../util/AppPaths.hpp"
#include "../util/ZipUtil.hpp"
#include "InstanceFiles.hpp"  // jarPath()：客户端 JAR 在哪个版本目录里由它说了算
#include "PlayerSkinStore.hpp"  // uuidKey()：账号键的写法只有这一份

namespace {

/// 新布局那 18 条所在的目录（数组前 9 条在 slim/ 底下、后 9 条在 wide/ 底下）。
constexpr char kPlayerDir[] = "assets/minecraft/textures/entity/player/";
/// 旧版布局那两张所在的目录。
constexpr char kEntityDir[] = "assets/minecraft/textures/entity/";

/// PNG 头那 8 个字节。**落盘之前必须过这一道**：条目坏了/不是图的时候，写成文件之后
/// "文件存在"就等于"已经算好了"，于是那张坏图会永远躺在缓存里 —— 每次开启动器都拿它去
/// 解码、每次都会失败，而且再也不会有哪一趟来纠正它（和 icon 缓存那边"两种字节都按内容认"
/// 是同一种防法）。
const char kPngSignature[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};

/// 九张官方默认皮肤的名字，**顺序就是游戏里那个数组的顺序**（1.20.1 的常量池里逐个抄的，
/// 见 .hpp 文件头）。新规矩那 18 条 = 这张表先配 slim、再配 wide，所以 18 = 9 × 2。
const QStringList &nameTable() {
    static const QStringList names = {
        QStringLiteral("alex"),  QStringLiteral("ari"),   QStringLiteral("efe"),
        QStringLiteral("kai"),   QStringLiteral("makena"), QStringLiteral("noor"),
        QStringLiteral("steve"), QStringLiteral("sunny"), QStringLiteral("zuri"),
    };
    return names;
}

/// 条目路径 → 缓存文件名里那一段（`assets/.../player/wide/kai.png` → `player-wide-kai`）。
///
/// 拿不到合法的一段（空、或者有超出 `[a-z0-9-]` 的字符）时返回**空串** —— 调用方据此不拼
/// 路径。和 PlayerSkinStore::uuidKey() 不肯拼野路径是同一条规矩：拼出来的那个名字会被当成
/// "某张已经算好的图"，而它其实谁都不是。
QString entrySlug(const QString &entryPath) {
    QString rest = entryPath;
    // 剥前缀纯粹是为了短一点：这一段的唯一职责是"同一张图每次都得到同一段"，剥不剥都成立。
    const QString prefix = QString::fromLatin1(kEntityDir);
    if (rest.startsWith(prefix)) {
        rest.remove(0, prefix.size());
    }
    if (rest.endsWith(QLatin1String(".png"))) {
        rest.chop(4);
    }
    rest.replace(QLatin1Char('/'), QLatin1Char('-'));
    if (rest.isEmpty()) {
        return QString();
    }
    for (const QChar ch : rest) {
        const bool ok = ch == QLatin1Char('-') ||
                        (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) ||
                        (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'));
        if (!ok) {
            return QString();
        }
    }
    return rest;
}

/// 版本 id → 缓存文件名里那一段（md5 前 12 位十六进制）。
QString versionKey(const QString &versionId) {
    if (versionId.isEmpty()) {
        return QString();
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(versionId.toUtf8(), QCryptographicHash::Md5)
            .toHex()
            .left(12));
}

/// 把字节原子地落进缓存（QSaveFile：先写同目录的临时文件、commit() 时才改名覆盖）。
/// 半张图留在盘上的后果和上面那条一样：文件在就等于"算好了"，那张图再也修不回来。
bool writeCacheFile(const QString &path, const QByteArray &bytes, QString *errorOut) {
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("DefaultSkin", "无法创建皮肤缓存目录 %1。").arg(dir);
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("DefaultSkin", "写不了 %1：%2").arg(path, file.errorString());
        }
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("DefaultSkin", "写 %1 失败：%2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

}  // namespace

QStringList DefaultSkin::skinNames() { return nameTable(); }

int DefaultSkin::eighteenCount() { return nameTable().size() * 2; }

bool DefaultSkin::uuidHashCode(const QString &uuid, int *hashOut) {
    // 形状和大小写都先归一：uuidKey() 是"这个字符串算不算一个 uuid"的唯一判据
    // （去横线、转小写、要求正好 32 位十六进制）。
    const QString key = PlayerSkinStore::uuidKey(uuid);
    if (key.isEmpty()) {
        return false;
    }
    const quint64 high = key.left(16).toULongLong(nullptr, 16);
    const quint64 low = key.mid(16).toULongLong(nullptr, 16);

    // Java: `int hilo = (int)(mostSigBits ^ leastSigBits)` 那个 long 被转成 int 之后，
    // 取的是低 32 位；哈希再 = `(int)(hilo >> 32) ^ (int)hilo`（>> 是算术右移，
    // 但两边都是先截成 int 再异或，所以位形态上和"两个 32 位半段直接异或"完全一样）。
    const quint64 hilo = high ^ low;
    const quint32 h = static_cast<quint32>(hilo >> 32) ^ static_cast<quint32>(hilo & 0xFFFFFFFFull);
    if (hashOut != nullptr) {
        *hashOut = static_cast<qint32>(h);
    }
    return true;
}

DefaultSkin::Prediction DefaultSkin::predict(const QString &uuid, Rule rule) {
    Prediction result;
    int hash = 0;
    if (rule == Rule::Unknown || !uuidHashCode(uuid, &hash)) {
        return result;
    }
    result.rule = rule;

    if (rule == Rule::Eighteen) {
        // Java 的 Math.floorMod：负数也要落回 [0, n)。C++ 的 % 会给负数，得自己掰回来。
        const int count = eighteenCount();
        int index = hash % count;
        if (index < 0) {
            index += count;
        }
        result.index = index;
        // 前 9 条是窄版、后 9 条是宽版，名字表两段一模一样。宽版那个模型串在游戏里叫
        // "default"（不是 "wide"），对应启动器这边的 slim = false。
        result.slim = index < nameTable().size();
        result.name = nameTable().at(index % nameTable().size());
        result.entryPath = QString::fromLatin1(kPlayerDir) +
                           (result.slim ? QStringLiteral("slim/") : QStringLiteral("wide/")) +
                           result.name + QStringLiteral(".png");
        result.valid = true;
        return result;
    }

    // 旧版布局：`c(UUID)` 那个私有谓词返回真走 alex（= slim），假走 steve（= 宽）。
    // 真值取自 hash 的最低位 —— 字节码里就是 `(hashCode & 1) == 1`。
    result.slim = (hash & 1) == 1;
    result.name = result.slim ? QStringLiteral("alex") : QStringLiteral("steve");
    result.index = -1;
    result.entryPath = QString::fromLatin1(kEntityDir) + result.name + QStringLiteral(".png");
    result.valid = true;
    return result;
}

QString DefaultSkin::cachePathFor(const QString &uuid, const QString &versionId,
                                  const QString &entryPath) {
    const QString account = PlayerSkinStore::uuidKey(uuid);
    const QString version = versionKey(versionId);
    const QString slug = entrySlug(entryPath);
    if (account.isEmpty() || version.isEmpty() || slug.isEmpty()) {
        return QString();
    }
    return AppPaths::skinsDir() + QLatin1Char('/') + QStringLiteral("default-") + account +
           QLatin1Char('-') + version + QLatin1Char('-') + slug + QStringLiteral(".png");
}

DefaultSkin::Result DefaultSkin::resolve(const QString &playerName, const QString &versionId,
                                        QString *pathOut, bool *slimOut, QString *reasonOut) {
    if (pathOut != nullptr) {
        pathOut->clear();
    }
    if (slimOut != nullptr) {
        *slimOut = false;
    }
    if (reasonOut != nullptr) {
        reasonOut->clear();
    }

    // uuid 只有一个出处：**游戏自己也是拿进 --username 的那个名字去算的**，所以这里收的是
    // 名字（不是 uuid），算的是同一个函数 —— 差一个字符就是另一张脸。
    const QString uuid = AccountStore::offlineUuidFor(playerName);
    int hash = 0;
    if (!uuidHashCode(uuid, &hash)) {
        // 到不了这儿（offlineUuidFor 给的一定是 32 位十六进制）。留着是为了这一条路自己
        // 读起来是完整的，不靠"上面那个函数不会出错"。
        if (reasonOut != nullptr) {
            *reasonOut = QCoreApplication::translate("DefaultSkin", "找不到此玩家名称对应的 UUID。");
        }
        return Result::Unavailable;
    }
    if (versionId.isEmpty()) {
        if (reasonOut != nullptr) {
            *reasonOut = QCoreApplication::translate("DefaultSkin", "没有选中任何已安装版本。");
        }
        return Result::Unavailable;
    }

    // ① 缓存命中：这个 (账号, 版本) 算过了吗？两条规矩各一个文件名（版本键在里面，
    // 所以换到低版本不会拿现代那张顶上去），**先新后老**。
    // 命中时一个进程都不起 —— "平时开启动器零代价"就是这一句。
    for (const Rule rule : {Rule::Eighteen, Rule::Two}) {
        const Prediction p = predict(uuid, rule);
        if (!p.valid) {
            continue;
        }
        const QString cached = cachePathFor(uuid, versionId, p.entryPath);
        if (!cached.isEmpty() && QFile::exists(cached)) {
            if (pathOut != nullptr) {
                *pathOut = cached;
            }
            if (slimOut != nullptr) {
                *slimOut = p.slim;
            }
            return Result::Resolved;
        }
    }

    // ② 现算。判据是**那个 JAR 里有没有这一条**，不是版本号（理由见 .hpp 文件头）。
    const QString jar = InstanceFiles::jarPath(versionId);
    if (!QFile::exists(jar)) {
        if (reasonOut != nullptr) {
            *reasonOut = QCoreApplication::translate("DefaultSkin", "这个版本的客户端 JAR 不在（%1）。").arg(jar);
        }
        return Result::Unavailable;
    }

    QString lastReason;
    for (const Rule rule : {Rule::Eighteen, Rule::Two}) {
        const Prediction p = predict(uuid, rule);
        if (!p.valid) {
            continue;
        }
        QByteArray bytes;
        QString error;
        const ZipUtil::EntryStatus status =
            ZipUtil::readEntry(jar, p.entryPath, &bytes, &error,
                               QCoreApplication::translate("DefaultSkin", "读客户端 JAR 里的贴图"));
        if (status == ZipUtil::EntryStatus::Ok) {
            if (bytes.size() < 8 || std::memcmp(bytes.constData(), kPngSignature, 8) != 0) {
                if (reasonOut != nullptr) {
                    *reasonOut = QCoreApplication::translate("DefaultSkin", "客户端 JAR 里那条贴图不是一张 PNG（%1）。")
                                     .arg(p.entryPath);
                }
                return Result::Failed;
            }
            const QString target = cachePathFor(uuid, versionId, p.entryPath);
            QString writeError;
            if (target.isEmpty() || !writeCacheFile(target, bytes, &writeError)) {
                if (reasonOut != nullptr) {
                    *reasonOut = target.isEmpty()
                                     ? QCoreApplication::translate("DefaultSkin", "拼不出这个账号的缓存路径。")
                                     : writeError;
                }
                return Result::Failed;
            }
            if (pathOut != nullptr) {
                *pathOut = target;
            }
            if (slimOut != nullptr) {
                *slimOut = p.slim;
            }
            return Result::Resolved;
        }
        if (status == ZipUtil::EntryStatus::Failed ||
            status == ZipUtil::EntryStatus::NotAnArchive) {
            // 这次是**真的出错**（PowerShell 起不来 / 超时 / 那个 JAR 坏了），不是"没有这一条"。
            // 两件事分开：前者值得记一行日志，后者是常态（换个规矩再问一次就行）。
            //
            // NOTE: NotAnArchive 时 ZipUtil **不给** error（它自己那一层没有话可说：退出码 4
            // 就是终点，连异常文字都没有），照抄过来就是一句「算离线默认皮肤失败：」
            // 后面什么都没有 —— 那句日志的全部意义就是"怎么老是随机那张"有个交代，
            // 空着等于没写。这里补上，并且两种都带上路径（到底哪个版本的 JAR 坏了）。
            if (reasonOut != nullptr) {
                if (status == ZipUtil::EntryStatus::NotAnArchive) {
                    *reasonOut = QCoreApplication::translate("DefaultSkin", "无法打开这个版本的客户端 JAR（不是 zip 压缩包，或文件已损坏）：%1。")
                                     .arg(jar);
                } else if (error.isEmpty()) {
                    *reasonOut = QCoreApplication::translate("DefaultSkin", "读 %1 里的贴图失败。").arg(jar);
                } else {
                    *reasonOut = error;
                }
            }
            return Result::Failed;
        }
        // NotFound：这个 JAR 不是这一套规矩 → 换下一套再问一次。
        lastReason = QCoreApplication::translate("DefaultSkin", "客户端 JAR 里没有 %1。").arg(p.entryPath);
    }
    if (reasonOut != nullptr) {
        *reasonOut = lastReason;
    }
    return Result::Unavailable;
}

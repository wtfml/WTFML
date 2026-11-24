/**
 * @file ModpackExport.cpp
 * @brief 见 .hpp。从上往下四层：murmur2 那台小状态机 → 两份清单的序列化 →
 * 枚举与"哪些文件要问接口"的判据表 → 把前几层串成一次导出的 buildExportPlan()。
 * 全篇**不联网、不写文件**（反查走注入进来的 query），所以这一层整体可离线验。
 */

#include "ModpackExport.hpp"

#include "InstanceFiles.hpp"
#include "ModpackImport.hpp"
#include "util/AppPaths.hpp"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace ModpackExport {
namespace {

// ---------------------------------------------------------------------------
// murmur2（CurseForge 指纹那一半）
// ---------------------------------------------------------------------------

constexpr quint32 kMurmur2M = 0x5BD1E995u;

/// 那四种被剔掉的字节。**就这四种**：别的空白（0x0B 垂直制表、0x0C 换页）不剔，
/// 非 ASCII 的空白（比如全角空格）也不剔 —— 它们都是普通字节。
inline bool isStrippedByte(quint8 byte) {
    return byte == 0x09 || byte == 0x0A || byte == 0x0D || byte == 0x20;
}

/// 一个 32 位字进来之后该怎么混。顺序是 `乘 → 异或右移24 → 乘`，注意第一步在**外**面
/// 跟 h 的乘法交错（见下面 flushWord：h 先乘再异或这个结果）。
inline quint32 murmur2MixWord(quint32 word) {
    word *= kMurmur2M;
    word ^= word >> 24;
    word *= kMurmur2M;
    return word;
}

/**
 * @brief 流式喂字节的 murmur2。**长度必须事先知道**（初值 = `1 ^ 长度`）。
 *
 * NOTE: 正因为长度要先知道，算一个文件得读**两趟**：第一趟只数非空白字节有几个，
 * 第二趟才喂。代价是多读一遍文件，换来的是 O(1) 内存 —— 一个 200 MB 的 mod
 * 不会让启动器当场涨 200 MB（导出是并行算好几个文件的，那种涨法是乘以线程数的）。
 */
class Murmur2Stream {
public:
    explicit Murmur2Stream(quint32 length) : m_hash(1u ^ length) {}

    void add(const char *data, qsizetype size) {
        for (qsizetype i = 0; i < size; ++i) {
            const quint8 byte = static_cast<quint8>(data[i]);
            if (isStrippedByte(byte)) {
                continue;
            }
            m_tail[m_tailSize++] = byte;
            if (m_tailSize == 4) {
                flushWord();
            }
        }
    }

    quint32 finish() {
        if (m_tailSize > 0) {
            // 末梢那 1~3 个字节按小端拼成一个字，**先异或再乘**（顺序和主循环相反，
            // 这是 murmur2 本来的写法，别"顺手"统一成主循环那个顺序）。
            quint32 word = 0;
            for (qsizetype i = m_tailSize; i-- > 0;) {
                word = (word << 8) | m_tail[i];
            }
            m_hash ^= word;
            m_hash *= kMurmur2M;
            m_tailSize = 0;
        }
        m_hash ^= m_hash >> 13;
        m_hash *= kMurmur2M;
        m_hash ^= m_hash >> 15;
        return m_hash;
    }

private:
    void flushWord() {
        const quint32 word = static_cast<quint32>(m_tail[0]) |
                             (static_cast<quint32>(m_tail[1]) << 8) |
                             (static_cast<quint32>(m_tail[2]) << 16) |
                             (static_cast<quint32>(m_tail[3]) << 24);
        m_hash *= kMurmur2M;
        m_hash ^= murmur2MixWord(word);
        m_tailSize = 0;
    }

    quint32 m_hash;
    quint8 m_tail[4] = {};
    qsizetype m_tailSize = 0;
};

/// 非空白字节有几个。
qint64 countStrippedBytes(const char *data, qsizetype size) {
    qint64 count = 0;
    for (qsizetype i = 0; i < size; ++i) {
        if (!isStrippedByte(static_cast<quint8>(data[i]))) {
            ++count;
        }
    }
    return count;
}

/// **唯一**的读文件循环（64 KiB 一块）。一趟数、一趟喂、还有"同时算三个哈希"那趟
/// 走的都是它 —— 省得"缓冲开多大、读一半算不算成功"这种细节在三处各写一遍。
template <typename Sink>
bool readFileChunks(const QString &path, Sink &&sink, QString *errorOut) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut != nullptr) {
            *errorOut = QObject::tr("打开文件失败：%1").arg(file.errorString());
        }
        return false;
    }
    QByteArray buffer;
    buffer.resize(64 * 1024);
    for (;;) {
        const qint64 read = file.read(buffer.data(), buffer.size());
        if (read < 0) {
            if (errorOut != nullptr) {
                *errorOut = QObject::tr("读这个文件的时候出错了：%1").arg(file.errorString());
            }
            return false;
        }
        if (read == 0) {
            break;  // 读到头
        }
        sink(buffer.constData(), read);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 清单序列化
// ---------------------------------------------------------------------------

/// 接口回的字节数那一格（Modrinth 的 `size` / CurseForge 的 `fileLength`）→ 字节数。
/// 不是数、或者是负数、或者大到塞不进 qint64 ⇒ **-1 = "不知道字节数"**（不强转）。
///
/// NOTE: 判据和 ModpackImport::parseModrinthIndex() 里那一份**逐字相同**，两处都必须有：
/// 那边读的是**清单文件**、这边读的是**接口的回复**，都是外部输入。裸的
/// `static_cast<qint64>(double)` 对越界的值是**未定义行为**（x86 上给出 0x8000…，
/// 一个负数），而那个负数会一路走进"这份文件下完了没有"的判断里 —— 变成一个永远
/// 下不完的作业。2^63 这个字面量在 double 里表示得**正好**，`<` 它就一定塞得进。
/// NOTE: 0 留着（0 字节的文件确实存在，那份字节数由 sha1 那一侧兜）。
qint64 fileSizeFromJson(const QJsonValue &value) {
    constexpr double kQint64UpperBound = 9223372036854775808.0;
    if (value.isDouble() && value.toDouble() >= 0.0 && value.toDouble() < kQint64UpperBound) {
        return static_cast<qint64>(value.toDouble());
    }
    return -1;
}

/// `hashes` 那一段。sha1 与 sha512 **两个都必须有**（规范原文是 MUST）——
/// 缺哪个都不该写成一条，理由见 writableAsIndexFile()。
QJsonObject hashesOf(const MatchedFile &file) {
    QJsonObject hashes;
    if (!file.sha1.isEmpty()) {
        hashes.insert(QStringLiteral("sha1"), file.sha1);
    }
    if (!file.sha512.isEmpty()) {
        hashes.insert(QStringLiteral("sha512"), file.sha512);
    }
    return hashes;
}

}  // namespace

QString modrinthDependencyKey(const QString &loaderKey) {
    const QString key = loaderKey.trimmed();
    if (key.isEmpty()) {
        return QString();  // 纯原版：依赖表里只有 minecraft 那一行
    }
    if (key == ModpackImport::fabricLoaderKey() || key == ModpackImport::forgeLoaderKey() ||
        key == ModpackImport::neoforgeLoaderKey() || key == ModpackImport::quiltLoaderKey()) {
        return key;
    }
    // 认不出来的加载器（OptiFine、rift、以后新出的……）：**不编一个键出来**。
    // 编了的话导入方会拿它去装一个不存在的东西；返回空串让调用方照实说
    // "这个实例的加载器两种规范都表达不了"。
    return QString();
}

QString curseForgeLoaderId(const QString &loaderKey, const QString &loaderVersion,
                           const QString &gameVersion) {
    const QString key = loaderKey.trimmed();
    const QString version = loaderVersion.trimmed();
    if (key.isEmpty() || version.isEmpty()) {
        return QString();
    }
    QString prefix;
    if (key == ModpackImport::forgeLoaderKey()) {
        prefix = QStringLiteral("forge");
    } else if (key == ModpackImport::neoforgeLoaderKey()) {
        prefix = QStringLiteral("neoforge");
    } else if (key == ModpackImport::quiltLoaderKey()) {
        prefix = QStringLiteral("quilt");
    } else if (key == ModpackImport::fabricLoaderKey()) {
        // 只有这一种写法是官方明文确认过的：`fabric-x.y.z`（CurseForge 的拒稿文案里
        // 逐字写着 "Invalid Fabric mod loader id"，要求就是 `fabric-<版本>`）。
        prefix = QStringLiteral("fabric");
    } else {
        return QString();
    }
    // NeoForge 的 1.20.1 那一代版本号本身就是 `1.20.1-47.1.106` 这个形状，所以那一个
    // 游戏版本上要把它拼回来。别的版本加了反而是错的（CurseForge 那边认得的是
    // `neoforge-21.1.210`）—— 这套是照 Prism 的做法，不是官方文档里的（官方只
    // 明文写了 fabric 那一种）。
    if (key == ModpackImport::neoforgeLoaderKey() && gameVersion.trimmed() == QLatin1String("1.20.1")) {
        return prefix + QLatin1Char('-') + gameVersion.trimmed() + QLatin1Char('-') + version;
    }
    return prefix + QLatin1Char('-') + version;
}

QStringList uniqueSha1s(const QStringList &sha1s) {
    QStringList unique;
    unique.reserve(sha1s.size());
    QSet<QString> seen;
    for (const QString &sha1 : sha1s) {
        const QString trimmed = sha1.trimmed();
        if (trimmed.isEmpty() || seen.contains(trimmed)) {
            continue;
        }
        seen.insert(trimmed);
        unique.append(trimmed);
    }
    return unique;
}

QList<quint32> uniqueFingerprints(const QList<quint32> &fingerprints) {
    QList<quint32> unique;
    unique.reserve(fingerprints.size());
    QSet<quint32> seen;
    for (quint32 value : fingerprints) {
        if (seen.contains(value)) {
            continue;
        }
        seen.insert(value);
        unique.append(value);
    }
    return unique;
}

QByteArray modrinthVersionFilesRequestBody(const QStringList &sha1s) {
    QJsonArray hashes;
    // 同一个哈希只发一次：接口按哈希回一条，重复的那几格只会白占批量名额
    // （一批 100 条，内容相同的两个 jar 各占一格，别的那几个就少问一次）。
    for (const QString &sha1 : uniqueSha1s(sha1s)) {
        hashes.append(sha1);
    }
    if (hashes.isEmpty()) {
        return QByteArray();
    }
    QJsonObject body;
    body.insert(QStringLiteral("hashes"), hashes);
    body.insert(QStringLiteral("algorithm"), QStringLiteral("sha1"));
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QByteArray curseForgeFingerprintsRequestBody(const QList<quint32> &fingerprints) {
    QJsonArray list;
    // 同上：内容相同的两个文件指纹一样，一条就够（重复的只占批量名额）。
    for (quint32 value : uniqueFingerprints(fingerprints)) {
        // 这一格必须发**数字**，不能发 `QString::number(value)` 那样的字符串：
        // `{"fingerprints":["3816865428"]}` 会回 HTTP 400（`invalid type: string …,
        // expected i64`），`{"fingerprints":[3816865428]}` 才是 HTTP 200。
        // 这台镜像（`mod.mcimirror.top/curseforge/v1`，见 curseForgeApiBase 那段）
        // 是照官方那份做的，规矩就是官方的规矩：这一格是 **int64**。
        //
        // 发错了的症状只有「有一格没图」—— 和"这个资源本来就没传图标"长得一模一样；
        // 唯一会说话的是日志里那句「资源图标没查完：CurseForge 反查回了 HTTP 400。」
        // 而 400 的正文不会被读出来。于是所有"只在 CurseForge 上有"的资源都停在占位图上。
        //
        // NOTE: `idListRequestBody()`（`{"modIds":[…]}` / `{"fileIds":[…]}`）发的也是
        // **数字**，见 `src/manage/ModpackImport.cpp`。
        list.append(static_cast<qint64>(value));
    }
    if (list.isEmpty()) {
        return QByteArray();
    }
    QJsonObject body;
    body.insert(QStringLiteral("fingerprints"), list);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

bool isPoisonFingerprint(quint32 fingerprint) {
    return fingerprint == 811513880u;
}

QHash<QString, ModrinthHit> parseModrinthVersionFiles(const QByteArray &json, QString *problemOut) {
    QHash<QString, ModrinthHit> hits;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (problemOut != nullptr) {
            *problemOut =
                QObject::tr("Modrinth 的接口返回了无法解析的内容（%1）。").arg(parseError.errorString());
        }
        return hits;
    }

    const QJsonObject root = doc.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        // 键就是**我们发过去的那个 sha1**（规范原话：a map from hashes to versions）。
        // 不是 40 位十六进制的东西不猜（那说明这不是这一发的回复）。
        const QString key = it.key().trimmed().toLower();
        if (key.size() != 40) {
            continue;
        }
        const QJsonObject version = it.value().toObject();

        // 在这个版本的文件里挑**就是那一份**的：文件自己的 sha1 与那个键相同。
        // 挑不到就不认这一条（理由见 .hpp 里那条 NOTE）。
        const QJsonArray files = version.value(QStringLiteral("files")).toArray();
        for (const QJsonValue &value : files) {
            const QJsonObject file = value.toObject();
            const QJsonObject hashes = file.value(QStringLiteral("hashes")).toObject();
            const QString fileSha1 = hashes.value(QStringLiteral("sha1")).toString().toLower();
            if (fileSha1 != key) {
                continue;
            }
            ModrinthHit hit;
            hit.projectId = version.value(QStringLiteral("project_id")).toString();
            hit.versionNumber = version.value(QStringLiteral("version_number")).toString();
            hit.filename = file.value(QStringLiteral("filename")).toString();
            hit.sha1 = fileSha1;
            hit.sha512 = hashes.value(QStringLiteral("sha512")).toString().toLower();
            hit.url = file.value(QStringLiteral("url")).toString();
            hit.size = fileSizeFromJson(file.value(QStringLiteral("size")));
            hit.environment = version.value(QStringLiteral("environment")).toString();
            if (!hit.url.isEmpty() && !hit.sha512.isEmpty()) {
                hits.insert(key, hit);
            }
            break;
        }
    }
    return hits;
}

QHash<quint32, CurseForgeHit> parseCurseForgeFingerprints(const QByteArray &json,
                                                          QString *problemOut) {
    QHash<quint32, CurseForgeHit> hits;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("CurseForge 的接口返回了无法解析的内容（%1）。")
                              .arg(parseError.errorString());
        }
        return hits;
    }

    const QJsonObject root = doc.object();
    const QJsonValue data = root.value(QStringLiteral("data"));
    if (!data.isObject()) {
        // 出错时那边回 `{"error":"…"}`（没有 data）：让用户看见接口说了什么。
        const QString error = root.value(QStringLiteral("error")).toString().trimmed();
        if (problemOut != nullptr) {
            *problemOut = error.isEmpty()
                              ? QObject::tr("CurseForge 的接口回的话里没有指纹表。")
                              : QObject::tr("CurseForge 的接口拒绝了这一问：%1").arg(error);
        }
        return hits;
    }

    const QJsonArray matches = data.toObject().value(QStringLiteral("exactMatches")).toArray();
    for (const QJsonValue &value : matches) {
        // 取的是**里面那条 file 记录**（那一层 `id` 是项目号，不是指纹；按它归档会和
        // 文件号混起来）。文件记录自己带 `fileFingerprint`，那才是我们要的那个键。
        const QJsonObject file = value.toObject().value(QStringLiteral("file")).toObject();
        const QJsonValue fingerprint = file.value(QStringLiteral("fileFingerprint"));
        if (!fingerprint.isDouble()) {
            continue;
        }
        const quint32 key = static_cast<quint32>(fingerprint.toDouble());
        // 毒指纹（见 isPoisonFingerprint）：**这一条记录不代表任何一个具体文件**，所以
        // 不能拿它当键插进表里 —— 插了就等于给我们某个"恰好算出这个指纹"的文件安排了一个
        // 假匹配（那个号配着的项目号和本地那份文件没有关系）。切在解析这一层是因为
        // 信息就在**这条记录自己的** `fileFingerprint` 上：再往下走，这个值已经被当成
        // "我们发过去的某个指纹"了，那时候再拦就晚了。
        if (isPoisonFingerprint(key)) {
            continue;
        }
        CurseForgeHit hit;
        hit.projectId = file.value(QStringLiteral("modId")).toInt(0);
        hit.fileId = file.value(QStringLiteral("id")).toInt(0);
        const QJsonValue availableValue = file.value(QStringLiteral("isAvailable"));
        hit.availableKnown = !availableValue.isUndefined() && !availableValue.isNull();
        hit.available = availableValue.toBool(false);
        hit.displayName = file.value(QStringLiteral("displayName")).toString();
        hit.fileName = file.value(QStringLiteral("fileName")).toString();
        // 作者关了第三方分发时这一条就是 null —— 那**不是**"读不懂"，是接口在明说
        // "不许代下"。照样收进表里（`available` 是假），让调用方单独报数。
        hit.downloadUrl = file.value(QStringLiteral("downloadUrl")).toString();
        hit.size = fileSizeFromJson(file.value(QStringLiteral("fileLength")));
        // `hashes` 里 `algo == 1` 的那一条是 sha1（2 是 md5）。
        const QJsonArray hashes = file.value(QStringLiteral("hashes")).toArray();
        for (const QJsonValue &hashValue : hashes) {
            const QJsonObject hash = hashValue.toObject();
            if (hash.value(QStringLiteral("algo")).toInt(0) == 1) {
                hit.sha1 = hash.value(QStringLiteral("value")).toString().toLower();
            }
        }
        if (hit.projectId > 0 && hit.fileId > 0) {
            hits.insert(key, hit);
        }
    }
    return hits;
}

QString instanceExclusionReason(const QString &relativePath, const QString &instanceId,
                                const IncludeOptions &options) {
    // 规则**都只按顶层那一段判**：说的都是实例根底下的东西。所以 `mods/logs/` 这种
    // 是"mod 自己的日志"，不跟着 `logs/` 一起排掉。
    //
    // 目录那几条命中之后，**底下的内容一起排**（判据是首段，天然如此）。
    struct Rule {
        const char *name;
        const char *reason;
    };
    static const Rule kDirRules[] = {
        {"logs", QT_TRANSLATE_NOOP("ModpackExport", "日志")},
        {"crash-reports", QT_TRANSLATE_NOOP("ModpackExport", "崩溃报告")},
        {"natives", QT_TRANSLATE_NOOP("ModpackExport", "启动器解出来的运行时文件")},
        {"libraries", QT_TRANSLATE_NOOP("ModpackExport", "游戏库文件")},
        {".mixin.out", QT_TRANSLATE_NOOP("ModpackExport", "Mixin 的调试输出")},
        {".cache", QT_TRANSLATE_NOOP("ModpackExport", "加载器的缓存")},
        // `.fabric` / `.quilt` 底下是**重映射过的 mod 副本**：原样进包等于把一个改过的
        // jar 发出去，而且体积翻倍（原 jar 已经在 mods/ 里、已经进包了）。
        {".fabric", QT_TRANSLATE_NOOP("ModpackExport", "Fabric 的重映射缓存")},
        {".quilt", QT_TRANSLATE_NOOP("ModpackExport", "Quilt 的重映射缓存")},
        // 别的启动器的状态。**尤其是凭据** —— 打包是把登录信息泄出去最容易的一条路：
        // 这些名字底下放的是账号、令牌、上一次登录取到的档案。
        {".hmcl", QT_TRANSLATE_NOOP("ModpackExport", "其他启动器的账号状态")},
        {"PCL", QT_TRANSLATE_NOOP("ModpackExport", "其他启动器的账号状态")},
        {".curseclient", QT_TRANSLATE_NOOP("ModpackExport", "其他启动器的账号状态")},
    };
    static const Rule kFileRules[] = {
        {".ds_store", QT_TRANSLATE_NOOP("ModpackExport", "系统的垃圾文件")},
        {"thumbs.db", QT_TRANSLATE_NOOP("ModpackExport", "系统的垃圾文件")},
        // 启动器自己的状态：instanceId / jvmArgs / 分组 / 图标。导出去等于把上一个人的
        // 实例设置抄给下一个人（而且 `icon.png` 是我们自己那份缓存，不是包的内容）。
        //
        // NOTE: 实例设置那份文件名**不写死在这张表里**：`AppPaths::versionMetaFileName()`
        // 是唯一出处（见下面那一条现算的判据）。表是静态初始化的，改文件名时它不跟着动，
        // 而漏掉的症状是"导出整合包时那份实例设置没被排除"——不报错、也看不出来。
        {"icon.png", QT_TRANSLATE_NOOP("ModpackExport", "启动器保存的实例图标")},
        {"launcher_accounts.json", QT_TRANSLATE_NOOP("ModpackExport", "登录凭据")},
        {"launcher_msa_credentials.bin", QT_TRANSLATE_NOOP("ModpackExport", "登录凭据")},
        {"launcher_profiles.json", QT_TRANSLATE_NOOP("ModpackExport", "其他启动器的账号状态")},
        {"usernamecache.json", QT_TRANSLATE_NOOP("ModpackExport", "账号名缓存")},
        {"minecraftinstance.json", QT_TRANSLATE_NOOP("ModpackExport", "其他启动器的账号状态")},
        {"pcl.ini", QT_TRANSLATE_NOOP("ModpackExport", "其他启动器的账号状态")},
        // Prism 那份「打这个实例时别带哪些东西」的名单。它自己**不**进包（这是上一个
        // 导出者的状态，不是包的内容），而且**它的内容我们也不看** —— 这一点是条已知的
        // 缺口：真照它办的话，是我们替用户决定少带哪些文件，而界面上没有那一套勾选项。
        {".packignore", QT_TRANSLATE_NOOP("ModpackExport", "其他启动器的打包设置")},
    };

    QString path = relativePath;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QLatin1String("./"))) {
        path.remove(0, 2);
    }
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return QString();
    }
    const QString first = parts.first();
    const QString lower = first.toLower();

    for (const Rule &rule : kDirRules) {
        if (lower == QLatin1String(rule.name)) {
            return QCoreApplication::translate("ModpackExport", rule.reason);
        }
    }
    if (!options.saves && lower == QLatin1String("saves")) {
        return QCoreApplication::translate("ModpackExport", "存档（没勾上才排）");
    }
    if (!options.screenshots && lower == QLatin1String("screenshots")) {
        return QCoreApplication::translate("ModpackExport", "截图（没勾上才排）");
    }
    // 文件那几条要求**正好是顶层那一个文件**：`mods/icon.png` 是 mod 自己的资源，
    // 不能因为名字撞上就把它排掉。
    if (parts.size() == 1) {
        // 这一份实例设置的文件名只有 `AppPaths::versionMetaFileName()` 一处出处：
        // 表是静态初始化的，抄一份字面量进来就等于改文件名时必然漏掉这一处（漏掉的
        // 症状是"导出整合包时那份实例设置没被排除"，不报错、也看不出来）。
        if (lower == AppPaths::versionMetaFileName().toLower()) {
            return QCoreApplication::translate("ModpackExport", "启动器保存的实例设置");
        }
        for (const Rule &rule : kFileRules) {
            if (lower == QLatin1String(rule.name)) {
                return QCoreApplication::translate("ModpackExport", rule.reason);
            }
        }
        // 这个实例自己的版本文件：`<实例名>.jar` 与 `<实例名>.json`。**装什么由清单里的
        // 依赖管**（`dependencies` / `modLoaders`），把 jar 塞进包一份既没地方放、
        // 又会和导入方自己下下来的那一份打架。
        const QString id = instanceId.trimmed();
        if (!id.isEmpty() &&
            (first == id + QLatin1String(".jar") || first == id + QLatin1String(".json"))) {
            return QCoreApplication::translate("ModpackExport", "当前实例的版本文件");
        }
    }
    return QString();
}

namespace {

/// 这个目录项是不是一个"指到别处去"的链接（Windows 的 junction / 符号链接、Unix 的符号
/// 链接）。
///
/// `QDir::NoSymLinks` **挡不住 junction**：见 `src/manage/FolderManager.cpp`
/// 里 `isLinkToElsewhere()` 上面那段（`mklink /J` 造出来的 junction，`isSymLink()` 是
/// **false**、`canonicalFilePath()` 也原样返回字面路径，只有 `isJunction()` 认得出）。
/// 那个函数在 FolderManager.cpp 的匿名 namespace 里，取不到，所以这里照抄一份 ——
/// 两处判据是同一件事，改一边记得看另一边。
///
/// 所以链接目录**本身就不进去**（不进包、不数里面）：包里存不下一份联接，而"把实例外面的
/// 文件打进包"恰恰是这一层要避免的事。放在 skippedDirs 里如实报出去，别静默少东西。
/// 文件那一边不受影响（一个指向文件的链接就是一份文件，照收，Prism 也这么做）。
bool isLinkToElsewhere(const QFileInfo &info) {
    return info.isJunction() || info.isSymLink();
}

/// 一个被排除掉的目录**连里面的东西**一共有多少个文件（只数，不读内容）。
///
/// NOTE: 目录**不跟符号链接**（和主枚举同一份 flags，那里有完整来路）：跟了可能绕回自己，
/// 而"数出无限多个文件"这种事既没法报错也没法收场。文件那一边跟着（一个指向文件的
/// 链接就是一份文件）。用显式栈而不是递归，理由和主枚举一样。
int countFilesUnder(const QString &dirPath) {
    int count = 0;
    QStringList stack;
    stack.append(dirPath);
    while (!stack.isEmpty()) {
        const QDir dir(stack.takeLast());
        count += dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                                   QDir::NoSort)
                     .size();
        const QFileInfoList subs =
            dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden,
                              QDir::NoSort);
        for (const QFileInfo &sub : subs) {
            if (isLinkToElsewhere(sub)) {
                continue;
            }
            stack.append(sub.absoluteFilePath());
        }
    }
    return count;
}

}  // namespace

CollectResult collectInstanceFiles(const QString &instanceDir, const IncludeOptions &options) {
    CollectResult result;
    const QDir root(instanceDir);
    if (!root.exists()) {
        result.problem =
            QObject::tr("找不到这个实例目录：%1").arg(QDir::toNativeSeparators(instanceDir));
        return result;
    }
    const QString instanceId = QFileInfo(QDir::cleanPath(instanceDir)).fileName();

    // 顶层那两份"这个实例自己的版本文件"叫什么 —— 判据**不在这儿自己写一份**：走
    // InstanceFiles::resolve()，也就是启动器认客户端 jar 的那一条路（① 同名 ② 顶层恰好
    // 一个 jar）。这个结构是纯数据、那个函数是纯函数，所以这里只是把顶层那份列出来的
    // 名字交给它，不额外读一次盘。
    InstanceFiles::DirListing listing;
    listing.dirName = instanceId;
    listing.jarNames = root.entryList(QStringList() << QStringLiteral("*.jar"), QDir::Files);
    listing.jsonNames = root.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files);
    const InstanceFiles::Resolved resolved = InstanceFiles::resolve(listing);

    // 显式栈的深度优先（不递归：实例目录可以很深，而栈溢出没法报成一句人话）。
    struct Frame {
        QString absolute;
        QString prefix;  ///< 相对实例目录的前缀，带尾斜杠；顶层是空
    };
    QList<Frame> stack;
    stack.append({QDir::cleanPath(instanceDir), QString()});
    QSet<QString> seenDirs;  // 去重（. 和大小写这类）：同一个目录只走一遍
    while (!stack.isEmpty()) {
        const Frame frame = stack.takeLast();
        const QString dirKey = QDir::cleanPath(frame.absolute).toLower();
        if (seenDirs.contains(dirKey)) {
            continue;
        }
        seenDirs.insert(dirKey);

        const QDir dir(frame.absolute);
        // 目录**不跟符号链接**（跟了可能绕回自己，而"枚举出无限深"是没法报错的东西）；
        // 文件那一边跟着（一个指向文件的符号链接就是一份文件，Prism 也是这么做的）。
        // `QDir::Hidden`：**隐藏文件也要收**。实例里带点的东西不少（`.packignore`、
        // 加载器自己写的状态文件），而"这一个该不该进包"由排除表说了算 —— 让"名字带不带点"
        // 这个跟我们无关的差别去决定要不要，是最容易漏文件的那种漏法。
        const QFileInfoList dirs = dir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden, QDir::Name);
        const QFileInfoList files =
            dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);

        for (const QFileInfo &entry : dirs) {
            const QString prefix = frame.prefix + entry.fileName() + QLatin1Char('/');
            // 链接目录（junction / 符号链接）**一个都不进去**。
            // 判据不能只靠上面那个 `QDir::NoSymLinks`（它挡不住 junction —— 见
            // isLinkToElsewhere() 那段）：一个指回祖先的 junction 能让下面
            // 这圈栈式枚举无限往下走，而一个指向实例外面的 junction 会把别人的文件打进包。
            // 如实记进 skippedDirs（报告里那句「没进包：…」会带上它的名字与文件数），
            // 不静默少东西。
            if (isLinkToElsewhere(entry)) {
                const QString shown = prefix.left(prefix.size() - 1);
                if (!result.skippedDirs.contains(shown)) {
                    result.skippedDirs.append(shown);
                    result.skippedFileCount += countFilesUnder(entry.absoluteFilePath());
                }
                continue;
            }
            const QString reason = instanceExclusionReason(prefix, instanceId, options);
            if (!reason.isEmpty()) {
                const QString shown = prefix.left(prefix.size() - 1);
                // **这个目录里那些文件也要数进去**：目录不进去时，里面有几个文件也得问，
                // 否则报告里「没进包：存档、截图（共 N 个文件）」的 N 只数得到"被排除的
                // **文件**"，而 logs / saves / screenshots 这一档整个是目录 —— 那句话
                // 几乎永远是「共 0 个文件」。数的口径见上面 countFilesUnder() 那段。
                // 去重和 skippedDirs 同一处：同一个目录只数一次（重复 append 的那一路
                // 连数都不该数）。
                if (!result.skippedDirs.contains(shown)) {
                    result.skippedDirs.append(shown);
                    result.skippedFileCount += countFilesUnder(entry.absoluteFilePath());
                }
                continue;
            }
            stack.append({entry.absoluteFilePath(), prefix});
        }
        for (const QFileInfo &entry : files) {
            const QString relative = frame.prefix + entry.fileName();
            // 顶层那两个走 resolve() 判出来的名字（见上面那段 NOTE），其余照样问排除表。
            // 两处都问过一遍不算"两份判据"：resolve() 的 ① 与排除表那一条**是同一件事**
            // （名字等于目录名），② 是排除表那条纯函数**看不见**的（它只看一条路径、看不到
            // "顶层一共有几个 jar"），所以这一道只有枚举这一层补得上。
            const bool isOwnVersionFile =
                frame.prefix.isEmpty() &&
                ((!resolved.jarName.isEmpty() && entry.fileName() == resolved.jarName) ||
                 (!resolved.jsonName.isEmpty() && entry.fileName() == resolved.jsonName));
            if (isOwnVersionFile || !instanceExclusionReason(relative, instanceId, options).isEmpty()) {
                ++result.skippedFileCount;
                continue;
            }
            result.files.append({relative, entry.size()});
        }
    }

    // 顺序**钉死**（按路径排序）：同一个实例两次导出，条目顺序必须一样 —— 否则
    // "同一个包导出两次应该是同一份东西"这句话就没法验，出问题也没法对。
    std::sort(result.files.begin(), result.files.end(),
              [](const InstanceFile &left, const InstanceFile &right) {
                  return left.path < right.path;
              });
    result.skippedDirs.sort();
    return result;
}

bool looksLikeResolvableResource(const QString &relativePath) {
    const QString path = relativePath.trimmed().toLower();
    if (path.isEmpty()) {
        return false;
    }
    // 扩展名那一半。`.disabled` / `.old` 也认：那两种是"改了后缀的同一个文件"，
    // 字节没变，照样能反查出来（见 .hpp 里 MatchedFile::path 那段）。
    static const QStringList kExtensions = {QStringLiteral("jar"),   QStringLiteral("litemod"),
                                            QStringLiteral("zip"),   QStringLiteral("rar"),
                                            QStringLiteral("disabled"), QStringLiteral("old")};
    const QString suffix = QFileInfo(path).suffix();
    if (!kExtensions.contains(suffix)) {
        return false;
    }
    // 路径那另一半：**必须待在某个资源目录底下**。为什么要有这一条：导出时每个候选
    // 文件都要读一遍（CurseForge 那边还得读两遍）再往接口发一遍，而 config/ 里成千上万个
    // 小文件没有一个是能反查出来的 —— 不问，直接进 overrides 就行。代价是慢，收益是零。
    static const QStringList kContainers = {
        QStringLiteral("mods"),        QStringLiteral("coremods"), QStringLiteral("resourcepacks"),
        QStringLiteral("texturepacks"), QStringLiteral("shaderpacks"), QStringLiteral("datapacks"),
        QStringLiteral("openloader")};
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {  // 最后一段是文件名，不算
        if (kContainers.contains(parts.at(i))) {
            return true;
        }
    }
    return false;
}

QString indexFileName(Format format) {
    return format == Format::CurseForge ? QStringLiteral("manifest.json")
                                        : QStringLiteral("modrinth.index.json");
}

QString overridesPrefix(const Spec &spec) {
    // 两边现在都是 `overrides`，但来路不一样：Modrinth 那份规范把它**钉死**成
    // `overrides/`（清单里根本没有这一格），CF 那份清单自己写着 `overrides` 一格
    // （= `Spec::overridesDir`）。所以这里不是"反正都是 overrides"，是**分别**给的 ——
    // 哪天 overridesDir 真变成可配置的，Modrinth 那一侧不许跟着动。
    if (spec.format == Format::CurseForge && !spec.overridesDir.trimmed().isEmpty()) {
        return spec.overridesDir.trimmed();
    }
    return QStringLiteral("overrides");
}

QList<ZipUtil::ZipSource> archiveEntries(const ExportPlan &plan, const QString &manifestPath) {
    QList<ZipUtil::ZipSource> entries;
    ZipUtil::ZipSource manifest;
    manifest.entryName = indexFileName(plan.spec.format);
    manifest.sourcePath = manifestPath;
    entries.append(manifest);

    const QString prefix = overridesPrefix(plan.spec);
    const QDir root(plan.instanceDir);
    // `plan.overrides` 在 buildExportPlan 里已经排过序了，这里不再排（**排序的唯一出处
    // 是那一处**，两处各排一次的话，哪天口径变了就会两边不一致）。
    for (const InstanceFile &file : plan.overrides) {
        ZipUtil::ZipSource entry;
        entry.entryName = prefix + QLatin1Char('/') + file.path;
        entry.sourcePath = root.filePath(file.path);
        entries.append(entry);
    }
    return entries;
}

bool reimportable(const Spec &spec) {
    // 两种格式的判据是同一条：清单里得有东西。CF 那边进清单的是 `refs`（「拿号换文件」
    // 那条路），Modrinth 那边是 `files`。
    return !spec.files.isEmpty() || !spec.refs.isEmpty();
}

bool writableAsIndexFile(const MatchedFile &file) {
    if (file.path.trimmed().isEmpty()) {
        return false;  // 没路径的条目导入方会直接跳过（等于没这条）
    }
    // **路径必须过得了读侧那条判据**，判据本身从读侧那一处来（不在这儿
    // 抄第二份数字）。`files[].path` 是写进清单的，而读侧对清单里的每一条路径都跑
    // isSafeRelativePath()，**不过就是整包拒绝**（ModpackImport.cpp 里那句"所以整个包
    // 都没有导入"）。不在这里先拦，"实例里有个名字特别长的文件"（整条相对路径
    // >240 —— 段名没超，是嵌套拼起来超的）就能让我们导出一个人家装不进去的包，
    // 而且**当场看不出来**：包打得出来、我们自己（写的那一半）也不读它。
    // 退回 overrides 就没事：overrides 里那条路不经过这条判据（那些文件是照名字
    // 原样解出来的），文件一个不少，包也装得进去。
    if (!ModpackImport::isSafeRelativePath(file.path)) {
        return false;
    }
    if (file.sha1.trimmed().isEmpty() || file.sha512.trimmed().isEmpty()) {
        // 规范原文是 MUST 两条都有。缺了**不写**：清单里的 sha1 是安装时唯一的完整性
        // 判据（DownloadManager::fileAlreadyValid 在 sha1 空着时直接点头），少了它
        // 半截文件也算"下好了" —— 那种包比"文件躺在 overrides 里"坏得多。
        return false;
    }
    for (const QString &url : file.urls) {
        if (!url.trimmed().isEmpty()) {
            return true;
        }
    }
    return false;  // 没有地址 = 导入方不知道该从哪下
}

QByteArray buildModrinthIndex(const Spec &spec) {
    QJsonArray files;
    for (const MatchedFile &file : spec.files) {
        if (!writableAsIndexFile(file)) {
            // 这一条本该被调用方拦下（走 writableAsIndexFile 那个分岔口去 overrides）。
            // 真漏到这里就跳过不写：写一条坏的会让导入方少一个文件，
            // 而"少一个文件"是最难被发现的那种坏 —— 包能装完，游戏能启动，
            // 只是某个 mod 不见了。
            continue;
        }
        QJsonObject entry;
        entry.insert(QStringLiteral("path"), file.path);
        entry.insert(QStringLiteral("hashes"), hashesOf(file));
        QJsonArray downloads;
        for (const QString &url : file.urls) {
            if (!url.trimmed().isEmpty()) {
                downloads.append(url);
            }
        }
        entry.insert(QStringLiteral("downloads"), downloads);
        if (file.size >= 0) {
            entry.insert(QStringLiteral("fileSize"), static_cast<double>(file.size));
        }
        if (!file.envClient.isEmpty() && !file.envServer.isEmpty()) {
            QJsonObject env;
            env.insert(QStringLiteral("client"), file.envClient);
            env.insert(QStringLiteral("server"), file.envServer);
            entry.insert(QStringLiteral("env"), env);
        }
        files.append(entry);
    }

    QJsonObject dependencies;
    dependencies.insert(QStringLiteral("minecraft"), spec.gameVersion);
    const QString loaderKey = modrinthDependencyKey(spec.loaderKey);
    if (!loaderKey.isEmpty() && !spec.loaderVersion.trimmed().isEmpty()) {
        dependencies.insert(loaderKey, spec.loaderVersion.trimmed());
    }

    QJsonObject root;
    // 头两条是"这份 json 就是 modrinth.index.json"的标记，我们的解析器也按它们判
    // （见 parseModrinthIndex 开头那两条）。`game` 只可能是 minecraft。
    root.insert(QStringLiteral("formatVersion"), 1);
    root.insert(QStringLiteral("game"), QStringLiteral("minecraft"));
    root.insert(QStringLiteral("versionId"), spec.version);
    root.insert(QStringLiteral("name"), spec.name);
    if (!spec.summary.trimmed().isEmpty()) {
        root.insert(QStringLiteral("summary"), spec.summary.trimmed());
    }
    root.insert(QStringLiteral("files"), files);
    root.insert(QStringLiteral("dependencies"), dependencies);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray buildCurseForgeManifest(const Spec &spec) {
    QJsonArray files;
    for (const MatchedRef &ref : spec.refs) {
        if (ref.projectId <= 0 || ref.fileId <= 0) {
            continue;  // 两个号缺一个就换不出文件来，那一条对导入方是纯噪音
        }
        QJsonObject entry;
        entry.insert(QStringLiteral("projectID"), ref.projectId);
        entry.insert(QStringLiteral("fileID"), ref.fileId);
        // `required` 恒为 true：我们列出来的都是这个实例**正在用**的文件。
        // "可选文件"那一套（导入时让用户勾）我们没做，写了反而给出一个假的选择。
        entry.insert(QStringLiteral("required"), true);
        files.append(entry);
    }

    QJsonObject minecraft;
    minecraft.insert(QStringLiteral("version"), spec.gameVersion);
    const QString loaderId =
        curseForgeLoaderId(spec.loaderKey, spec.loaderVersion, spec.gameVersion);
    if (!loaderId.isEmpty()) {
        QJsonObject loader;
        loader.insert(QStringLiteral("id"), loaderId);
        loader.insert(QStringLiteral("primary"), true);
        minecraft.insert(QStringLiteral("modLoaders"), QJsonArray{loader});
    }

    QJsonObject root;
    root.insert(QStringLiteral("manifestType"), QStringLiteral("minecraftModpack"));
    root.insert(QStringLiteral("manifestVersion"), 1);
    root.insert(QStringLiteral("name"), spec.name);
    root.insert(QStringLiteral("version"), spec.version);
    if (!spec.author.trimmed().isEmpty()) {
        root.insert(QStringLiteral("author"), spec.author.trimmed());
    }
    root.insert(QStringLiteral("minecraft"), minecraft);
    root.insert(QStringLiteral("files"), files);
    // 这一格**必须明写**：规范只说过"有个叫 overrides 的目录"，没定过缺省值是什么
    // （各家实现都自己写死）。所以我们也不赌，写出来。
    root.insert(QStringLiteral("overrides"), spec.overridesDir);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray buildIndexFile(const Spec &spec) {
    // 唯一的分岔（见 .hpp）：写在名字旁边，别再让调用点自己判一次。
    if (spec.format == Format::CurseForge) {
        return buildCurseForgeManifest(spec);
    }
    return buildModrinthIndex(spec);
}

quint32 curseForgeFingerprintOfBytes(const QByteArray &bytes) {
    quint32 count = 0;
    for (const char byte : bytes) {
        if (!isStrippedByte(static_cast<quint8>(byte))) {
            ++count;
        }
    }
    Murmur2Stream stream(count);
    stream.add(bytes.constData(), bytes.size());
    return stream.finish();
}

bool curseForgeFingerprint(const QString &path, quint32 *out, QString *errorOut) {
    if (out == nullptr) {
        // 出参没给就没什么可做的了 —— 这是调用方的编程错误，不当成"文件读不了"。
        if (errorOut != nullptr) {
            *errorOut = QObject::tr("内部错误：没有给出放指纹的地方。");
        }
        return false;
    }
    qint64 count = 0;
    if (!readFileChunks(
            path,
            [&count](const char *data, qsizetype size) { count += countStrippedBytes(data, size); },
            errorOut)) {
        return false;
    }
    // 长度要先知道，所以这里才开第二趟。上限按 32 位截断：murmur2 的初值就是
    // `1 ^ (uint32)长度`，超 4 GB 的文件在算法里本来也是这么截的。
    Murmur2Stream stream(static_cast<quint32>(count));
    if (!readFileChunks(
            path, [&stream](const char *data, qsizetype size) { stream.add(data, size); },
            errorOut)) {
        return false;
    }
    *out = stream.finish();
    return true;
}

bool hashFile(const QString &path, bool needFingerprint, FileHashes *out, QString *errorOut) {
    if (out == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QObject::tr("内部错误：没有给出放哈希的地方。");
        }
        return false;
    }
    *out = FileHashes();
    QCryptographicHash sha1(QCryptographicHash::Sha1);
    QCryptographicHash sha512(QCryptographicHash::Sha512);
    // 两个流量哈希和指纹状态机在**同一趟**里喂：文件只读那么多次，而 IO 是这里最贵的东西。
    const auto feed = [&sha1, &sha512](const char *data, qsizetype size, Murmur2Stream *stream) {
        // QByteArrayView 那一版是这个 API 的现在时（Qt 6.10 起旧的那版标了 deprecated）。
        sha1.addData(QByteArrayView(data, size));
        sha512.addData(QByteArrayView(data, size));
        if (stream != nullptr) {
            stream->add(data, size);
        }
    };
    if (!needFingerprint) {
        if (!readFileChunks(
                path, [&feed](const char *data, qsizetype size) { feed(data, size, nullptr); },
                errorOut)) {
            return false;
        }
    } else {
        // 指纹要的"非空白字节数"得先知道 ⇒ 第一趟**只数**。
        // NOTE: 这一趟不能顺手喂那两个哈希 —— 喂了它们就把文件吃两遍了（哈希当然全错，
        // 而且错得很有迷惑性：长度对得上，就是值不对）。
        qint64 count = 0;
        if (!readFileChunks(
                path,
                [&count](const char *data, qsizetype size) {
                    count += countStrippedBytes(data, size);
                },
                errorOut)) {
            return false;
        }
        Murmur2Stream stream(static_cast<quint32>(count));
        if (!readFileChunks(
                path, [&feed, &stream](const char *data, qsizetype size) { feed(data, size, &stream); },
                errorOut)) {
            return false;
        }
        out->fingerprint = stream.finish();
        out->fingerprintOk = true;
    }
    out->sha1 = QString::fromLatin1(sha1.result().toHex());
    out->sha512 = QString::fromLatin1(sha512.result().toHex());
    return true;
}

// ---------------------------------------------------------------------------
// 组合层：枚举 → 算哈希 → 批量反查 → 拼清单
// ---------------------------------------------------------------------------

namespace {

/// Modrinth 那条 `environment` → 清单里 `env` 那一对。
///
/// 接口那一边的值是这五种：`client_and_server` / `client_only` / `server_only` /
/// `client_or_server` / `client_or_server_prefers_both`（真回复里见到的是最后一种）。
/// **只有 `client_only` 这一种写 `env`**，而且只写 `server: unsupported` 那一半 ——
/// 理由是不对称的：
/// * `client_only` 写下去与事实一致（本地就有这个文件，两边都不吃亏）；
/// * `server_only` **故意不写**。照上游那句话写 `client: unsupported` 看着更"忠实"，
/// 可导入方见到 unsupported 会**直接跳过这个文件** —— 于是一个本地确实躺着的模组
/// 在"导出再导入"之后凭空消失。`environment` 只是提示、不是规范强制要求的东西，
/// 而"导出不许丢文件"是我们自己钉死的不变量，两者冲突时丢文件那一侧输。
/// * 认不出的值一律不写（不写 = 两边都要 = 导入方都装，不会少装）。
void applyEnvironment(const QString &environment, MatchedFile *file) {
    if (file == nullptr) {
        return;
    }
    if (environment.trimmed().toLower() == QLatin1String("client_only")) {
        file->envClient = QStringLiteral("required");
        file->envServer = QStringLiteral("unsupported");
    }
}

/// 反查没成（或者用户叫停）时，候选文件该往哪去。
void pushOverride(const InstanceFile &file, QList<InstanceFile> *out) {
    if (out != nullptr) {
        out->append(file);
    }
}

}  // namespace

ExportPlan buildExportPlan(const ExportRequest &request, const ModrinthQuery &queryModrinth,
                           const CurseForgeQuery &queryCurseForge, const ExportProgress &progress,
                           const ExportCancel &cancel) {
    ExportPlan plan;
    plan.instanceDir = request.instanceDir;
    plan.collect = collectInstanceFiles(request.instanceDir, request.include);
    if (!plan.collect.problem.isEmpty()) {
        plan.problem = plan.collect.problem;
        return plan;
    }

    // 第一趟：把候选挑出来、顺手把请求表拼好。**顺序就是 collect 的顺序**（按路径排过），
    // 所以最后进清单的条目顺序是可复现的。
    QList<InstanceFile> candidates;
    for (const InstanceFile &file : plan.collect.files) {
        if (looksLikeResolvableResource(file.path)) {
            candidates.append(file);
        } else {
            // 打不开也不问接口的那些（config/、options.txt…）在这儿就定了：它们本来
            // 就只可能进 overrides。放在这一趟是为了让"要算哈希的"与"不用的"分开，
            // 后面报的 `total` 才等于真正要读的文件的个数。
            pushOverride(file, &plan.overrides);
        }
    }

    // 第二趟：算哈希。失败的**不算错**（照样进 overrides），但要照实报出来。
    const bool needFingerprint = request.format == Format::CurseForge;
    QHash<QString, QString> sha1OfPath;              ///< 相对路径 → sha1
    QHash<QString, quint32> fingerprintOfPath;       ///< 相对路径 → 指纹
    int done = 0;
    const int total = candidates.size();
    for (const InstanceFile &file : candidates) {
        // 取消**在文件之间**看：一个文件读到一半掐掉会把半份数据算成一个哈希。
        if (cancel && cancel()) {
            plan.cancelled = true;
            return plan;
        }
        if (progress) {
            progress(done, total, file.path);
        }
        FileHashes hashes;
        QString error;
        const QString absolute = QDir(request.instanceDir).filePath(file.path);
        if (!hashFile(absolute, needFingerprint, &hashes, &error) || hashes.sha1.isEmpty()) {
            plan.hashFailures.append(file.path);
            pushOverride(file, &plan.overrides);
            ++done;
            continue;
        }
        if (needFingerprint) {
            if (!hashes.fingerprintOk) {
                plan.hashFailures.append(file.path);
                pushOverride(file, &plan.overrides);
                ++done;
                continue;
            }
            fingerprintOfPath.insert(file.path, hashes.fingerprint);
        } else {
            sha1OfPath.insert(file.path, hashes.sha1);
        }
        ++done;
    }

    // 第三趟：反查。**一次发完**（两条路都是）：Modrinth 那一发就是按 sha1 列表来的，
    // CurseForge 那一发也是收一串指纹 —— 一个文件一发会让 400 个模组变成 400 个 RTT。
    bool lookupOk = true;
    QString lookupProblem;
    QHash<QString, ModrinthHit> modrinthHits;
    QHash<quint32, CurseForgeHit> curseForgeHits;
    // `plan.queried` **只在真的要发出去那一处**赋值（见下面两个分支）。接口**没接上**
    // 的时候（`queryModrinth` / `queryCurseForge` 是空函数，`lookupProblem` 那句
    // "没有接上查询接口"就是说这个），一个字节都没发出去，报告里不能写
    // 「问过 12 个文件，认出来 0 个」—— 用户会拿着这句话去查网络、查限流、等着重试，
    // 而真相是"这一趟根本没联网"。这个数的含义钉死成：**真的发出去问过的文件个数**
    // （发出去了但没答话的也算，那是"问了没回"，和"没问"不是一回事）。
    //
    // NOTE: 内容相同的两个文件（换了名字的同一样东西）在这里算**两个文件**、只发**一个
    // 哈希** —— 去重在 `lookup*` 那一层（`uniqueSha1s` / `uniqueFingerprints`）。所以
    // 报告里"问过 N 个文件"比请求体里那一串长，是照实的：两份都查过，只是查的是同一份内容。
    const int toAsk = needFingerprint ? fingerprintOfPath.size() : sha1OfPath.size();
    if (toAsk > 0) {
        if (progress) {
            progress(total, total, QString());  // 没有具体文件了：界面该显示"正在问接口"
        }
        if (needFingerprint) {
            QList<quint32> fingerprints;
            for (auto it = fingerprintOfPath.constBegin(); it != fingerprintOfPath.constEnd(); ++it) {
                fingerprints.append(it.value());
            }
            if (queryCurseForge) {
                plan.queried = toAsk;
                lookupOk = queryCurseForge(fingerprints, &curseForgeHits, &lookupProblem);
            } else {
                lookupOk = false;
                lookupProblem = QObject::tr("没有接上 CurseForge 的查询接口。");
            }
        } else {
            QStringList sha1s;
            for (auto it = sha1OfPath.constBegin(); it != sha1OfPath.constEnd(); ++it) {
                sha1s.append(it.value());
            }
            if (queryModrinth) {
                plan.queried = toAsk;
                lookupOk = queryModrinth(sha1s, &modrinthHits, &lookupProblem);
            } else {
                lookupOk = false;
                lookupProblem = QObject::tr("没有接上 Modrinth 的查询接口。");
            }
        }
    }
    if (!lookupOk) {
        // **不是错误**：认不出信息的那一份照样能打包（躺在 overrides 里），断网也能出包。
        // 要不要接着做，由调用点去问用户（PCL 那个「是否继续导出？」的形状）。
        plan.lookupFailed = true;
        plan.lookupProblem = lookupProblem.trimmed().isEmpty()
                                 ? QObject::tr("联网获取部分文件信息失败。")
                                 : lookupProblem;
    }

    // 第四趟：认领。认出来的进清单，**其余一个不落**全进 overrides。
    plan.spec.format = request.format;
    plan.spec.name = request.name;
    plan.spec.version = request.version;
    plan.spec.summary = request.summary;
    plan.spec.author = request.author;
    plan.spec.gameVersion = request.gameVersion;
    plan.spec.loaderKey = request.loaderKey;
    plan.spec.loaderVersion = request.loaderVersion;
    for (const InstanceFile &file : candidates) {
        if (plan.hashFailures.contains(file.path)) {
            continue;  // 上面已经放进 overrides 了
        }
        if (needFingerprint) {
            const quint32 fingerprint = fingerprintOfPath.value(file.path, 0u);
            // 毒指纹那一刀切在解析那一层（见 parseCurseForgeFingerprints），这里不用再拦。
            // 一次桶查找就够：`contains()` + `value()` 是两趟，而没命中的时候 `value()` 还会
            // 白构造一个 CurseForgeHit（它身上带着 QStringList）—— 这一趟是**每个文件一次**。
            const auto foundIt = lookupOk ? curseForgeHits.constFind(fingerprint)
                                          : curseForgeHits.constEnd();
            const bool hit = foundIt != curseForgeHits.constEnd();
            const CurseForgeHit found = hit ? *foundIt : CurseForgeHit();
            // 号得是两个正整数才写得进清单（`projectID` / `fileID` 都是必需项），
            // 并且 `isAvailable` 要为真 —— 假的时候写进去等于给出一个装不上的包，
            // 退回 overrides 并单独报数（调用点要在报告里说出来）。
            // 假的**那两种来路分开报**：接口明说不能分发（`availableKnown`）
            // 才是"作者关掉了第三方分发"；接口根本没给这一项是**另一回事**，报成前者
            // 是替接口认下一个它没做的决定（见 CurseForgeHit 里那段）。两种都进
            // overrides，只是报告里那两句话不能混。
            if (!hit || found.projectId <= 0 || found.fileId <= 0) {
                pushOverride(file, &plan.overrides);
                continue;
            }
            if (!found.available) {
                if (found.availableKnown) {
                    ++plan.notDistributable;
                } else {
                    ++plan.notDistributableUnknown;
                }
                pushOverride(file, &plan.overrides);
                continue;
            }
            // **改过名字 / 禁用着的** mod 不能写进 CF 清单。清单里那一条
            // 只有 projectID + fileID，收包的人那边是拿这两个号去 CurseForge 换一份文件
            // 回来，落到盘上的是**作者给的那个文件名**（CF 那份规范里根本没有"文件叫
            // 什么"这一格，只有号）。于是：
            // - `foo.jar.disabled`（用户自己禁用的那个）到了那边叫 `foo.jar`，
            // 装好就是**启用**的 —— 用户特意关掉的 mod 自己开了；
            // - 用户改过名的（`bar.jar`）到了那边叫回 `foo.jar`。
            // 两种都是"我们手里的状态被无声地改掉"，而改过的名字/后缀只有**原样带过去**
            // 才留得住 ⇒ 这一个退回 overrides（跟着包走，字节一模一样，名字也是用户那个）。
            // 代价是它不再被启动器单独更新/去重 —— 那个代价是对的：用户自己动过的名字，
            // 比"下次更新时自动改回来"要紧。
            // NOTE: 名字对不上才算"改过名"；接口没给 `fileName` 时**不猜**（那一种只能靠
            // 后缀认，见下面 stateInName）。大小写不算改名（Windows 上那是同一个
            // 名字，为它多塞一份文件进包不值）。
            const QString localName = QFileInfo(file.path).fileName();
            const QString localSuffix = QFileInfo(file.path).suffix();
            const bool stateInName =
                localSuffix.compare(QLatin1String("disabled"), Qt::CaseInsensitive) == 0 ||
                localSuffix.compare(QLatin1String("old"), Qt::CaseInsensitive) == 0;
            const QString remoteName = found.fileName.trimmed();
            const bool renamed =
                !remoteName.isEmpty() &&
                localName.compare(remoteName, Qt::CaseInsensitive) != 0;
            if (stateInName || renamed) {
                ++plan.keptLocalState;
                pushOverride(file, &plan.overrides);
                continue;
            }
            MatchedRef ref;
            ref.projectId = found.projectId;
            ref.fileId = found.fileId;
            plan.spec.refs.append(ref);
            ++plan.matched;
            continue;
        }
        const QString sha1 = sha1OfPath.value(file.path);
        // 同上（CurseForge 那一支 ★）：一次桶查找，不在没命中时白构造一份 ModrinthHit。
        const auto foundIt =
            lookupOk ? modrinthHits.constFind(sha1) : modrinthHits.constEnd();
        if (foundIt == modrinthHits.constEnd()) {
            pushOverride(file, &plan.overrides);
            continue;
        }
        const ModrinthHit &found = *foundIt;
        MatchedFile matched;
        matched.path = file.path;
        matched.sha1 = found.sha1.trimmed().isEmpty() ? sha1 : found.sha1.trimmed();
        matched.sha512 = found.sha512;
        if (!found.url.trimmed().isEmpty()) {
            matched.urls.append(found.url);
        }
        matched.size = found.size >= 0 ? found.size : file.size;
        applyEnvironment(found.environment, &matched);
        if (!writableAsIndexFile(matched)) {
            // 缺地址或缺哈希的回复**不写进清单**：导入方要么下不了它，要么没有完整性判据。
            // 退回 overrides，一份文件都不丢。
            pushOverride(file, &plan.overrides);
            continue;
        }
        plan.spec.files.append(matched);
        ++plan.matched;
    }

    // 清单里的条目顺序照旧钉死（与 overrides 同一个口径：按路径排）。
    std::sort(plan.spec.files.begin(), plan.spec.files.end(),
              [](const MatchedFile &left, const MatchedFile &right) { return left.path < right.path; });
    std::sort(plan.overrides.begin(), plan.overrides.end(),
              [](const InstanceFile &left, const InstanceFile &right) { return left.path < right.path; });
    return plan;
}

}  // namespace ModpackExport

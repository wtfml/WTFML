/**
 * @file PresetStore.cpp
 * @brief 实例预设落盘那一层的实现（形状与取舍见头文件与 `PresetTypes.hpp`）。
 *
 * 这一份里三处最容易写歪的地方，各自都有长注释：
 * · `isValidPresetId()` —— 唯一的目录逃逸闸（验收之一）；
 * · `list()` —— "一份坏了不许连坐别的"（另一条验收）落成的那个形状；
 * · `remove()` / `create()` 里那两句"下标读不回来就**不写回去**" —— 写回去等于把用户
 * 排好的顺序抹平，而那一下屏幕上什么都不会说。
 */

#include "PresetStore.hpp"
#include <QCoreApplication>
#include "PresetWorkspace.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QThreadPool>
#include <QUuid>

#include <QtConcurrent>

#include "GenericFiles.hpp"
#include "manage/FolderManager.hpp"
#include "util/AppPaths.hpp"
#include "util/WriteDirectoryGuard.hpp"

namespace Presets {
namespace {

const char *kIndexFileName = "index.json";
const char *kManifestFileName = "manifest.json";
const char *kFilesDirName = "files";
/// `list()` 的摘要缓存（见 `summaryCachePath()` 与下面那张表的注释）。
const char *kSummaryCacheFileName = ".preset-summaries.json";
const char *kSummaryCacheFormat = "wtfml.preset-summaries";
constexpr int kSummaryCacheVersion = 2;
constexpr qint64 kSummaryCacheBytesLimit = 8 * 1024 * 1024;

constexpr int kDisplayNameLimit = 80;

/// 说明那一栏的上限。比名字宽松得多 —— 说明本来就是一段话。
constexpr int kDescriptionLimit = 400;
constexpr qint64 kIndexLimit = 4 * 1024 * 1024;
constexpr qint64 kManifestLimit = 16 * 1024 * 1024;

/// `list()` 每一趟都要把每一份 manifest 读回来、解析一遍。1000 份实测 2.3 秒
/// （见 `docs/dev/perf-notes/MAIN_THREAD_STALLS.md`），而同一个进程里它会被问很多次（预设页、
/// 安装选择弹窗、`availablePresets()`…）。这里按"这份文件的字节数 + 修改时刻"记住
/// 上一次读出来的、**列表要的那几格**；任何一个变了就重读。
///
/// 只缓存**摘要**，不缓存 manifest 正文：正文是"应用到实例"那条路上真正要用的东西，
/// 那条路多读一次没关系，读到旧的那一份不行。
struct SummaryCacheEntry {
    qint64 size = -1;
    qint64 mtimeMs = -1;
    /// false = 内部部件（`internalComponent`）：`list()` 本来就不列它，但也要记住，
    /// 否则每一趟都还得把它的 manifest 读一遍才知道该跳过。
    bool listed = true;
    /// **不含 `isDefault`**：那一格由每一次调用的下标决定，不进缓存。
    PresetSummary summary;
};

QHash<QString, SummaryCacheEntry> g_summaryCache;
QMutex g_summaryCacheMutex;

/// 上限只是防长跑进程里无限长；到顶整张丢掉重来（这是缓存，不是数据）。
constexpr int kSummaryCacheEntriesLimit = 4096;

/// 这一趟进程里**已经从盘上那份摘要缓存灌过**的 root（`QDir::absolutePath()` 归一化）。
/// 灌一次就够：之后都靠内存那张 + 每次的 stat 校验。
QSet<QString> g_summaryCacheLoadedRoots;

/// 本进程写过的 manifest（完整路径）。★ 盘上那份摘要是上一次进程留下的；如果本进程
/// 写完之后"字节数 + 毫秒"都没变（同一个名字长度、同一毫秒），stat 校验认不出来。
/// 所以这一张里的路径在灌盘上缓存时**一律跳过**，下一趟从 manifest 重读。
QSet<QString> g_summaryCacheWrittenPaths;

void forgetSummaryCacheEntry(const QString &path) {
    const QMutexLocker locker(&g_summaryCacheMutex);
    g_summaryCache.remove(path);
    if (QFileInfo(path).fileName() == QLatin1String(kManifestFileName)) {
        g_summaryCacheWrittenPaths.insert(path);
    }
}

// ---------------------------------------------------------------------------
// 摘要缓存落盘的那一份
// ---------------------------------------------------------------------------
//
// 【为什么盘上还要有一份】
//
// 进程里那张缓存只帮第二趟：冷启动后第一次 `list()` 仍要把每份 manifest 读一遍
// （并行之后 1000 份 0.40 s，见 `docs/dev/perf-notes/PRESET_LIST_IO.md`）。而预设页
// 和安装选择弹窗的第一趟就压在这一下上。把"上一次读出来的摘要 + 那一份文件的
// 字节数/修改时刻"写在盘上，下一次启动只要 stat 一遍就能跳过 manifest 正文。
//
// 【形状】
//
// `{"format":"wtfml.preset-summaries","version":2,"entries":[{"id":…,"size":…,
// "mtime":…,"listed":…,"name":…,…}]}`，一份文件属于一个 root（放在 `root` 底下）。
// 判据与进程里那张一模一样：`size + mtimeMs` 对不上就读真的 manifest。
//
// 【它坏了的后果只有一种】
//
// 变慢。文件读不回来、格式标记不认识、版本比我们新、某一项缺字段 —— 都当"没有缓存"，
// 下一趟照旧从 manifest 读。所以往这里写之前不必先校验盘上那份。
QString summaryCacheRootKey(const QString &root) {
    return QDir(root).absolutePath();
}

QJsonObject summaryEntryToJson(const QString &id, const SummaryCacheEntry &entry) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), id);
    object.insert(QStringLiteral("size"), static_cast<double>(entry.size));
    object.insert(QStringLiteral("mtime"), static_cast<double>(entry.mtimeMs));
    object.insert(QStringLiteral("listed"), entry.listed);
    if (!entry.listed) {
        return object;
    }
    const PresetSummary &summary = entry.summary;
    object.insert(QStringLiteral("name"), summary.name);
    object.insert(QStringLiteral("description"), summary.description);
    object.insert(QStringLiteral("updatedAt"), summary.updatedAt.toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("sections"), summary.sectionCount);
    object.insert(QStringLiteral("items"), summary.itemCount);
    QJsonObject itemCounts;
    for (auto it = summary.componentItemCounts.cbegin(); it != summary.componentItemCounts.cend(); ++it)
        itemCounts.insert(it.key(), it.value());
    object.insert(QStringLiteral("componentItems"), itemCounts);
    object.insert(QStringLiteral("category"), presetCategoryKey(summary.category));
    object.insert(QStringLiteral("components"), QJsonArray::fromStringList(summary.componentCategories));
    object.insert(QStringLiteral("icon"), summary.iconPath);
    return object;
}

/// 一项读回来。**任何一格不对就整项不认**（那一份照旧去读 manifest）——
/// 半份摘要比没有摘要危险：名字对了、类别错了，界面就会把一份预设画到错的地方。
bool summaryEntryFromJson(const QJsonObject &object, QString *id, SummaryCacheEntry *entry) {
    const QString entryId = object.value(QStringLiteral("id")).toString();
    const QJsonValue sizeValue = object.value(QStringLiteral("size"));
    const QJsonValue mtimeValue = object.value(QStringLiteral("mtime"));
    const QJsonValue listedValue = object.value(QStringLiteral("listed"));
    if (!isValidPresetId(entryId) || !sizeValue.isDouble() || !mtimeValue.isDouble()
        || !listedValue.isBool()) {
        return false;
    }
    SummaryCacheEntry parsed;
    parsed.size = static_cast<qint64>(sizeValue.toDouble());
    parsed.mtimeMs = static_cast<qint64>(mtimeValue.toDouble());
    parsed.listed = listedValue.toBool();
    if (parsed.size < 0 || parsed.mtimeMs < 0) {
        return false;
    }
    if (parsed.listed) {
        const QString name = object.value(QStringLiteral("name")).toString();
        // 盘上的 manifest 不可能没有名字（`manifestFromJson` 那一关），所以空的这一项
        // 一定是被改坏/写坏了。
        if (name.isEmpty()) {
            return false;
        }
        bool categoryOk = false;
        const PresetCategory category =
            presetCategoryFromKey(object.value(QStringLiteral("category")).toString(), &categoryOk);
        const QJsonValue sections = object.value(QStringLiteral("sections"));
        const QJsonValue components = object.value(QStringLiteral("components"));
        const int sectionCount = sections.isDouble() ? sections.toInt(-1) : -1;
        const QJsonValue items = object.value(QStringLiteral("items"));
        const int itemCount = items.isDouble() ? items.toInt(-1) : -1;
        const QJsonValue componentItems = object.value(QStringLiteral("componentItems"));
        if (!categoryOk || sectionCount < 0 || !components.isArray() || itemCount < 0 ||
            items.toDouble() != itemCount || !componentItems.isObject()) {
            return false;
        }
        QMap<QString, int> counts;
        qint64 counted = 0;
        const auto componentObject = componentItems.toObject();
        for (auto it = componentObject.begin(); it != componentObject.end(); ++it) {
            bool keyOk = false;
            const auto part = presetCategoryFromKey(it.key(), &keyOk);
            const int count = it.value().isDouble() ? it.value().toInt(-1) : -1;
            if (!keyOk || part == PresetCategory::Unset || count < 0 || it.value().toDouble() != count)
                return false;
            counts.insert(it.key(), count);
            counted += count;
        }
        if (counted > itemCount) return false;
        PresetSummary summary;
        summary.itemCount = itemCount;
        summary.componentItemCounts = counts;
        summary.id = entryId;
        summary.name = name;
        summary.description = object.value(QStringLiteral("description")).toString();
        const QString updatedRaw = object.value(QStringLiteral("updatedAt")).toString();
        if (!updatedRaw.isEmpty()) {
            summary.updatedAt = QDateTime::fromString(updatedRaw, Qt::ISODateWithMs);
        }
        summary.sectionCount = sectionCount;
        summary.category = category;
        for (const QJsonValue &value : components.toArray()) {
            if (value.isString()) {
                summary.componentCategories.append(value.toString());
            }
        }
        summary.iconPath = object.value(QStringLiteral("icon")).toString();
        parsed.summary = summary;
    }
    *id = entryId;
    *entry = parsed;
    return true;
}

/// 这个 root 的盘上缓存灌进进程里那张（**只补空位**：进程里已经有的比盘上的新）。
void ensureSummaryCacheLoaded(const QString &root) {
    const QString rootKey = summaryCacheRootKey(root);
    {
        const QMutexLocker locker(&g_summaryCacheMutex);
        if (g_summaryCacheLoadedRoots.contains(rootKey)) {
            return;
        }
        g_summaryCacheLoadedRoots.insert(rootKey);
    }
    const QFileInfo info(summaryCachePath(root));
    if (!info.exists() || info.size() > kSummaryCacheBytesLimit) {
        return;
    }
    QFile file(info.filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray bytes = file.read(kSummaryCacheBytesLimit + 1);
    file.close();
    if (bytes.size() > kSummaryCacheBytesLimit) {
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) {
        return;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("format")).toString() != QLatin1String(kSummaryCacheFormat)
        || object.value(QStringLiteral("version")).toInt(-1) != kSummaryCacheVersion) {
        return;
    }
    const QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    QList<QPair<QString, SummaryCacheEntry>> loaded;
    loaded.reserve(qMin(entries.size(), qsizetype(kSummaryCacheEntriesLimit)));
    for (const QJsonValue &value : entries) {
        if (!value.isObject()) {
            continue;
        }
        QString id;
        SummaryCacheEntry entry;
        if (!summaryEntryFromJson(value.toObject(), &id, &entry)) {
            continue;
        }
        loaded.append(qMakePair(id, entry));
        if (loaded.size() >= kSummaryCacheEntriesLimit) {
            break;
        }
    }
    if (loaded.isEmpty()) {
        return;
    }
    const QMutexLocker locker(&g_summaryCacheMutex);
    for (const auto &pair : loaded) {
        const QString path = manifestPath(root, pair.first);
        // 本进程写过的：盘上那份可能是写之前的，stat 撞上同一毫秒也认不出。
        if (g_summaryCacheWrittenPaths.contains(path)) {
            continue;
        }
        if (!g_summaryCache.contains(path)) {
            g_summaryCache.insert(path, pair.second);
        }
    }
}

/// 把这一趟 `list()` 认得的那些（`ordered` 里、进程缓存中有的）整份重写下去。
///
/// 只在**真有新读回来的**时候调用（见 `list()`）。写不进去就算了：缓存坏了只许变慢。
void writeSummaryCacheFile(const QString &root, const QStringList &ordered) {
    QJsonArray entries;
    {
        const QMutexLocker locker(&g_summaryCacheMutex);
        for (const QString &id : ordered) {
            const auto it = g_summaryCache.constFind(manifestPath(root, id));
            if (it == g_summaryCache.constEnd()) {
                continue;
            }
            entries.append(summaryEntryToJson(id, *it));
            if (entries.size() >= kSummaryCacheEntriesLimit) {
                break;
            }
        }
    }
    QJsonObject object;
    object.insert(QStringLiteral("format"), QLatin1String(kSummaryCacheFormat));
    object.insert(QStringLiteral("version"), kSummaryCacheVersion);
    object.insert(QStringLiteral("entries"), entries);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > kSummaryCacheBytesLimit) {
        return;
    }
    const QString path = summaryCachePath(root);
    if (!WriteDirectoryGuard::acquire(path)) {
        return;
    }
    if (!QFileInfo(path).absoluteDir().exists()) {
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

/// 写盘：临时文件 + 原子改名（和任务账本 / 启动历史同一套）。
/// 新建文件的快速原子写：目标**必须是新路径**（不存在），写临时文件再改名。
/// 用在"每次发布新建一份不可变子清单"上：省掉 QSaveFile 每份的落盘刷写。
bool writeBytesAtomicNew(const QString &path, const QByteArray &bytes) {
    const qint64 limit = QFileInfo(path).fileName() == QLatin1String(kIndexFileName)
        ? kIndexLimit : kManifestLimit;
    if (bytes.size() > limit) return false;
    if (QFileInfo::exists(path)) return false;
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) return false;
    if (!WriteDirectoryGuard::acquire(path)) return false;
    const QString temporary = path + QStringLiteral(".new-") +
                              QString::number(QCoreApplication::applicationPid()) + QLatin1Char('-') +
                              QString::number(QDateTime::currentMSecsSinceEpoch());
    QFile file(temporary);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(bytes) != bytes.size()) { file.close(); QFile::remove(temporary); return false; }
    file.flush();
    file.close();
    if (!QFile::rename(temporary, path)) { QFile::remove(temporary); return false; }
    return true;
}

bool writeBytesAtomic(const QString &path, const QByteArray &bytes) {
    const qint64 limit = QFileInfo(path).fileName() == QLatin1String(kIndexFileName)
        ? kIndexLimit : kManifestLimit;
    if (bytes.size() > limit) return false;
    // 锁要落在**真正被写的那个数据根**上。预设住在启动器自己的数据根
    // （`presetsRoot()` = `defaultMinecraftDir()/presets`），不是当前游戏文件夹
    // （`minecraftDir()`）—— 换一个游戏文件夹之后预设不跟着走，锁要是还认旧的那一格，
    // 两个启动器进程（各自开着不同的游戏文件夹、却共用同一份预设）就谁也没挡住谁。
    // `WriteDirectoryGuard::rootFor()` 正好就是"按这个路径往上找它属于哪个数据根"。
    if (!WriteDirectoryGuard::acquire(path)) return false;
    const QFileInfo info(path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        return false;
    }
    // 本进程自己写的这一份，摘要缓存立刻作废。不靠"修改时刻"这一格兜底：
    // 同一毫秒里先写后读、时间戳粒度粗一点，都可能让它看起来没变。
    forgetSummaryCacheEntry(path);
    return true;
}

/// 说明那一栏怎么收：只去首尾空白，**不折行**（它是给人看的一段话，换行是有意义的）。
QString cleanDescription(const QString &raw) {
    QString cleaned = raw.trimmed();
    if (cleaned.size() > kDescriptionLimit) {
        cleaned.truncate(kDescriptionLimit);
    }
    return cleaned;
}

/// Windows 上那几个保留的设备名（`CON` / `NUL` / `COM1`……）。★ 它不属于"穿越"，属于
/// "写下去就不见了"：拿它当文件名，`QFile::open()` 那句**会成功**，写进黑洞里。
///
/// 判的是**基名**（第一个点之前那一截）：Windows 上 `nul.txt` 一样是那个空设备
/// （跟后缀无关），而 `console.txt` 是正常文件 —— 差的就是这个点。
bool isReservedDeviceName(const QString &part) {
    QString base = part;
    const int dot = base.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        base.truncate(dot);
    }
    // Windows 还会把结尾的空格和点吃掉（`nul ` 也是 nul）。
    while (!base.isEmpty() && (base.endsWith(QLatin1Char(' ')) || base.endsWith(QLatin1Char('.')))) {
        base.chop(1);
    }
    const QString upper = base.toUpper();
    if (upper == QLatin1String("CON") || upper == QLatin1String("PRN")
        || upper == QLatin1String("AUX") || upper == QLatin1String("NUL")) {
        return true;
    }
    if (upper.size() == 4 && (upper.startsWith(QLatin1String("COM"))
                              || upper.startsWith(QLatin1String("LPT")))) {
        const QChar digit = upper.at(3);
        return digit >= QLatin1Char('1') && digit <= QLatin1Char('9');
    }
    return false;
}

/// 以 `id` 为目录名的那一份，路径拼出来在 root 里面吗（最后一道自检，见 `isValidPresetId`）。
bool pathIsInsideRoot(const QString &root, const QString &path) {
    const QString canonicalRoot = QDir(root).absolutePath();
    const QString canonicalPath = QDir(path).absolutePath();
    return canonicalPath == canonicalRoot
           || canonicalPath.startsWith(canonicalRoot + QLatin1Char('/'));
}

/// 一句"哪个文件、怎么了"。用户的每一句抱怨都该说得出文件，所以路径摆在最前面。
QString faultAt(const QString &path, const QString &why) {
    return path + QCoreApplication::translate("PresetStore", "：") + why;
}

}  // namespace

// ---------------------------------------------------------------------------
// 落点
// ---------------------------------------------------------------------------

QString presetsRoot() {
    // **跟着当前游戏文件夹走**（和实例同一个归属）：换一个 .minecraft 就是换一套预设，
    // 和那一份文件夹里的实例对得上。早先放在启动器数据根下（"用户习惯不跟文件夹走"），
    // 用户看两张列表一对照就说不对 —— 预设服务的正是这个文件夹里的实例。
    // 全局预设是以后的事，不是现在这一份的形状。
    return AppPaths::minecraftDir() + QStringLiteral("/presets");
}

QString indexPath(const QString &root) {
    return root + QLatin1Char('/') + QLatin1String(kIndexFileName);
}

QString presetDir(const QString &root, const QString &id) {
    return root + QLatin1Char('/') + id;
}

QString manifestPath(const QString &root, const QString &id) {
    return presetDir(root, id) + QLatin1Char('/') + QLatin1String(kManifestFileName);
}

QString filesDir(const QString &root, const QString &id) {
    return presetDir(root, id) + QLatin1Char('/') + QLatin1String(kFilesDirName);
}

QString summaryCachePath(const QString &root) {
    // 和 `indexPath()` 一样是"root + 文件名"：root 的写法由调用方决定（验证工程也会给
    // 相对路径），缓存键跟着它走没有关系 —— 它只是加速，不是数据。
    return root + QLatin1Char('/') + QLatin1String(kSummaryCacheFileName);
}

// ---------------------------------------------------------------------------
// 名字与 id
// ---------------------------------------------------------------------------

QString newPresetId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool isValidPresetId(const QString &id) {
    // 这一条是**唯一**的目录逃逸闸，所以判据是"必须是一个 UUID"而不是"里面没有斜杠"。
    // "没有斜杠"那种黑名单挡不住 `..`、挡不住 `C:`、挡不住以点开头、也挡不住 Windows
    // 那几个设备名 —— 而这几样正好都是"拼出来的路径不在 root 里面"的路子。
    //
    // 连带的好处：我们自己在 `newPresetId()` 里造出来的 id 永远是 UUID，所以"用户手打
    // 一个 id 进来"这种事在这一层根本不存在（它只会走显示名那一路，而显示名不进路径）。
    if (id.isEmpty() || id.size() > 64) {
        return false;
    }
    const QUuid parsed = QUuid::fromString(id);
    if (parsed.isNull()) {
        return false;
    }
    // 要的是**逐字**等于规范写法（小写、不带花括号）：`QUuid::fromString` 连
    // `{...}` 和大小写混着写都认，认下来就等于"同一个 id 有两种写法" —— 而目录名是
    // 按字符串比的，两种写法在盘上就是两个目录。
    return parsed.toString(QUuid::WithoutBraces) == id;
}

QString cleanDisplayName(const QString &raw) {
    // `simplified()` 一举三得：去首尾空白、把中间连续的空格折成一个、把换行/制表符也折成
    // 空格 —— 后面那一条是必需的，名字会被写进任务标题（那边的行高是按一行算的）。
    QString cleaned = raw.simplified();
    if (cleaned.size() > kDisplayNameLimit) {
        cleaned.truncate(kDisplayNameLimit);
    }
    return cleaned;
}

bool isSafeRelativePath(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    // 结尾那一个斜杠是"这是个目录"的写法，无害，先摘掉；摘完是空的说明它整个就是斜杠。
    while (normalized.endsWith(QLatin1Char('/'))) {
        normalized.chop(1);
    }
    if (normalized.isEmpty() || normalized.startsWith(QLatin1Char('/'))) {
        return false;
    }
    const QStringList parts = normalized.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        // 空的那一段 = 连着两个斜杠：同一个路径有两种写法，别放进来。
        if (part.isEmpty() || part == QLatin1String("..")) {
            return false;
        }
        if (part == QLatin1String(".")) {
            continue;
        }
        // 盘符（`C:`）/ NTFS 备用数据流（`name:stream`）/ 任何带冒号的怪名字。
        if (part.contains(QLatin1Char(':'))) {
            return false;
        }
        if (isReservedDeviceName(part)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 下标
// ---------------------------------------------------------------------------

PresetIndex appendToIndex(PresetIndex index, const QString &id) {
    if (id.isEmpty() || index.order.contains(id)) {
        return index;
    }
    index.order.append(id);
    return index;
}

PresetIndex dropFromIndex(PresetIndex index, const QString &id) {
    index.order.removeAll(id);
    if (index.defaultPresetId == id) {
        index.defaultPresetId.clear();
    }
    // 哪一类的默认项是它：**留键、清成空串**（= 这一类现在明确没有默认）。
    // 直接把键删掉的话，旧版那个单默认项（如果还指着同类的另一个人）又会顶上来 ——
    // 用户刚删掉一份预设，屏幕上却冒出另一个"默认"，那一手不该发生。
    for (auto it = index.categoryDefaults.begin(); it != index.categoryDefaults.end(); ++it) {
        if (it.value() == id) {
            it.value().clear();
        }
    }
    return index;
}

/// 下标里"哪一类默认是哪一份"这一格能不能留：认得出的四类才留；值要么是空串
/// （明确不要默认），要么是合法 id。读、写两侧都过这一道。
void normalizeCategoryDefaults(QMap<QString, QString> *byCategory) {
    for (auto it = byCategory->begin(); it != byCategory->end();) {
        bool keyOk = false;
        const PresetCategory category = presetCategoryFromKey(it.key(), &keyOk);
        if (!keyOk || category == PresetCategory::Unset) {
            it = byCategory->erase(it);
            continue;
        }
        if (!it.value().isEmpty() && !isValidPresetId(it.value())) {
            // 坏掉的一格当"没表过态"：比"这一类明确不要默认"松一档，别让一格坏数据
            // 把用户将来的默认一起锁死。
            it = byCategory->erase(it);
            continue;
        }
        ++it;
    }
}

PresetIndex moveInIndex(PresetIndex index, const QString &id, int delta) {
    const int from = index.order.indexOf(id);
    const int to = from + delta;
    if (from < 0 || delta == 0 || to < 0 || to >= index.order.size()) {
        return index;
    }
    index.order.move(from, to);
    return index;
}

PresetIndex loadIndex(const QString &root, bool *ok) {
    if (ok != nullptr) {
        // 文件不在**不算错**：一次都没做过预设的新用户就是这样。
        *ok = true;
    }
    QFile file(indexPath(root));
    if (!file.open(QIODevice::ReadOnly)) {
        if (ok && QFileInfo::exists(file.fileName())) *ok = false;
        return PresetIndex();
    }
    const QByteArray bytes = file.size() <= kIndexLimit ? file.read(kIndexLimit + 1) : QByteArray();
    const bool readOk = file.error() == QFileDevice::NoError && !bytes.isEmpty() && bytes.size() <= kIndexLimit;
    file.close();
    if (!readOk) {
        if (ok) *ok = false;
        return PresetIndex();
    }

    bool parsed = false;
    PresetIndex index = indexFromJson(bytes, &parsed);
    if (!parsed) {
        // 读不回来时回的是**空下标 + ok=false**：调用方那一下不许把它写回去。写回去等于
        // 把用户排好的顺序抹平，而屏幕上什么都不会说（列表还是那么几份，只是次序变了）。
        if (ok != nullptr) {
            *ok = false;
        }
        return PresetIndex();
    }
    QStringList kept;
    for (const QString &id : index.order) {
        if (isValidPresetId(id)) {
            kept.append(id);
        }
    }
    index.order = kept;
    if (!index.defaultPresetId.isEmpty() && !isValidPresetId(index.defaultPresetId)) {
        index.defaultPresetId.clear();
    }
    normalizeCategoryDefaults(&index.categoryDefaults);
    return index;
}

bool saveIndex(const QString &root, const PresetIndex &index) {
    PresetIndex normalized = index;
    normalized.schemaVersion = kSchemaVersion;
    // 顺序表里只留合法 id（写侧同样要过一遍：`appendToIndex()` 进得来，`loadIndex()` 也可能
    // 是被别处拼出来直接喂进来的）。
    QStringList kept;
    for (const QString &id : normalized.order) {
        if (isValidPresetId(id) && !kept.contains(id)) {
            kept.append(id);
        }
    }
    normalized.order = kept;
    if (!isValidPresetId(normalized.defaultPresetId)) {
        normalized.defaultPresetId.clear();
    }
    normalizeCategoryDefaults(&normalized.categoryDefaults);
    if (!QDir().mkpath(root)) {
        return false;
    }
    return writeBytesAtomic(indexPath(root), indexToBytes(normalized));
}

// ---------------------------------------------------------------------------
// 类别约束
// ---------------------------------------------------------------------------

bool sectionFitsCategory(PresetCategory category, const PresetSection &section) {
    if (category == PresetCategory::Unset) {
        // 旧版混合预设：老数据一个字节都不动（也照样能提取/应用）。
        return true;
    }
    // 通用文件组那四段同一个 `SectionKind`，靠段 id 里那半截名字分家（`fileGroupFromSectionId`
    // 是唯一出处，和 PresetController 显示那些细分名字用的是同一条判断）。
    FileGroup group = FileGroup::Config;
    const bool isGeneric =
        section.kind == SectionKind::GenericFiles && fileGroupFromSectionId(section.id, &group);
    switch (category) {
    case PresetCategory::GameSettings:
        switch (section.kind) {
        case SectionKind::VanillaOptions:
        case SectionKind::VanillaKeys:
        case SectionKind::MalilibKeys:
        case SectionKind::ServerList:
            return true;
        case SectionKind::GenericFiles:
            // `config/` 与 `defaultconfigs/` 是"高级内容"，首版先收在游戏设置里；
            // 资源包/光影归 `ResourcePacks`，不许混进来。
            return isGeneric && (group == FileGroup::Config || group == FileGroup::DefaultConfigs);
        case SectionKind::Custom:
            return false;
        }
        return false;
    case PresetCategory::ResourcePacks:
        // 旧预设曾把光影并入资源包；读取时保持其原范围，不自动重写用户文件。
        return isGeneric && (group == FileGroup::ResourcePacks || group == FileGroup::ShaderPacks);
    case PresetCategory::ShaderPacks:
        return isGeneric && group == FileGroup::ShaderPacks;
    case PresetCategory::Mods:
    case PresetCategory::DataPacks:
        // 模组引用不走段表（`PresetManifest::modReferences`），模组类允许空预设；
        // 数据包载荷还没有对应的段种类。首版这两类先只放得下 `Custom`。
        return section.kind == SectionKind::Custom;
    case PresetCategory::Unset:
        break;
    }
    return false;
}

QString categoryMismatch(const PresetManifest &manifest) {
    if (manifest.category == PresetCategory::Unset) {
        return QString();
    }
    // 模组引用只属于「模组」这一类。放进别的类别 = 串类别（一条验收），
    // 拼出来的安装计划会去改一类用户没选的东西。
    if (!manifest.modReferences.isEmpty() && manifest.category != PresetCategory::Mods) {
        return QCoreApplication::translate("PresetStore", "类别与内容不匹配。模组引用应归入「模组」类别");
    }
    for (const PresetSection &section : manifest.sections) {
        if (!sectionFitsCategory(manifest.category, section)) {
            return QCoreApplication::translate("PresetStore", "预设包含不属于所选类别的内容");
        }
    }
    return QString();
}

// ---------------------------------------------------------------------------
// 单份
// ---------------------------------------------------------------------------

PresetManifest loadManifest(const QString &root, const QString &id, bool *ok, QString *reason) {
    const auto fail = [&](const QString &why) {
        if (ok != nullptr) {
            *ok = false;
        }
        if (reason != nullptr) {
            *reason = why;
        }
        return PresetManifest();
    };
    if (ok != nullptr) {
        *ok = true;
    }
    if (reason != nullptr) {
        reason->clear();
    }
    const QString dir = presetDir(root, id);
    if (!isValidPresetId(id)) {
        return fail(faultAt(dir, QCoreApplication::translate("PresetStore", "此 id 不是预设（预设目录名只能为 UUID）")));
    }
    if (!pathIsInsideRoot(root, dir)) {
        return fail(faultAt(dir, QCoreApplication::translate("PresetStore", "生成的路径超出预设目录")));
    }
    const QString path = manifestPath(root, id);
    if (!QFileInfo::exists(path)) {
        if (!QFileInfo::exists(dir)) {
            return fail(faultAt(dir, QCoreApplication::translate("PresetStore", "已不存在")));
        }
        return fail(faultAt(path, QCoreApplication::translate("PresetStore", "不存在（该目录中无此预设）")));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(faultAt(path, QCoreApplication::translate("PresetStore", "无法打开预设文件")));
    }
    if (file.size() > kManifestLimit)
        return fail(faultAt(path, QCoreApplication::translate("PresetStore", "清单超过 16 MiB，未读取")));
    const QByteArray bytes = file.read(kManifestLimit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > kManifestLimit)
        return fail(faultAt(path, QCoreApplication::translate("PresetStore", "读取清单失败或清单过大")));
    file.close();

    bool parsed = false;
    const PresetManifest manifest = manifestFromJson(bytes, &parsed);
    if (!parsed) {
        QJsonParseError syntax;
        QJsonDocument::fromJson(bytes, &syntax);
        if (syntax.error != QJsonParseError::NoError)
            return fail(faultAt(path, QCoreApplication::translate("PresetStore", "JSON 字节位置 %1：%2").arg(syntax.offset).arg(syntax.errorString())));
        return fail(faultAt(path, QCoreApplication::translate("PresetStore", "预设有误。请检查 JSON 格式、版本、ID 和名称")));
    }
    // 记的 id 和目录名对不上 ⇒ 这一份被人动过（拷串了、或者手改过）。这种时候**宁可
    // 把它报成坏的**，也不要拿着"目录 A 里的内容属于 B"去覆盖用户的实例。
    if (manifest.id != id) {
        return fail(faultAt(path, QCoreApplication::translate("PresetStore", "预设 ID「%1」与目录名不一致").arg(manifest.id)));
    }
    // `category` 与段表对不上（比如有人手改过、或者以后的数据被降级写回来）⇒ 同样报坏：
    // 拿一份"类别说 A、内容其实是 B"的预设去改用户实例，比少一份预设危险得多。
    const QString mismatch = categoryMismatch(manifest);
    if (!mismatch.isEmpty()) {
        return fail(faultAt(path, mismatch));
    }
    return manifest;
}

bool saveManifest(const QString &root, const PresetManifest &manifest) {
    if (!isValidPresetId(manifest.id)) {
        return false;
    }
    if (cleanDisplayName(manifest.name).isEmpty()) {
        return false;
    }
    // 写侧也过同一道"类别 ↔ 内容"闸：调用方那一侧漏过滤时，宁可这一趟存不下去，
    // 也不要把一份串了类的预设写进盘里（读侧那道闸会把它当坏预设，用户就"丢"了一份）。
    if (!categoryMismatch(manifest).isEmpty()) {
        return false;
    }
    const QString dir = presetDir(root, manifest.id);
    if (!pathIsInsideRoot(root, dir)) {
        return false;
    }
    if (!QDir().mkpath(dir)) {
        return false;
    }
    // schema 版本由这一层钉死（调用方手里那一份可能是个 0）。理由：读的时候 `<= 0` 是要被
    // 挡住的，写一个 0 下去等于当场把这份预设写成"坏的那一档"。
    PresetManifest normalized = manifest;
    normalized.schemaVersion = kSchemaVersion;
    return writeBytesAtomic(manifestPath(root, manifest.id), manifestToBytes(normalized));
}

/// 新建一份**不可变子清单**（多份综合预设发布时用）：目标 id 是新生成的，文件不存在，
/// 用快速原子写代替 QSaveFile 的逐份刷盘；父清单仍然等所有子项就位后才写。
bool saveNewManifest(const QString &root, const PresetManifest &manifest) {
    if (!isValidPresetId(manifest.id) || cleanDisplayName(manifest.name).isEmpty()) return false;
    if (!categoryMismatch(manifest).isEmpty()) return false;
    const QString dir = presetDir(root, manifest.id);
    if (!pathIsInsideRoot(root, dir) || !QDir().mkpath(dir)) return false;
    PresetManifest normalized = manifest;
    normalized.schemaVersion = kSchemaVersion;
    return writeBytesAtomicNew(manifestPath(root, manifest.id), manifestToBytes(normalized));
}

// ---------------------------------------------------------------------------
// 列表
// ---------------------------------------------------------------------------

namespace {

struct ManifestLoadResult {
    PresetManifest manifest;
    bool ok = false;
    QString reason;
};

ManifestLoadResult loadForList(const QString &root, const QString &id) {
    ManifestLoadResult result;
    result.manifest = loadManifest(root, id, &result.ok, &result.reason);
    return result;
}

/// 读一批 manifest，**次序与 `ids` 一致**。
///
/// 1000 份串行 2.4 s、并行 0.4 s（探针实测，见
/// `docs/dev/perf-notes/PRESET_LIST_IO.md`），所以够多就并行。少的时候不折腾：
/// 起线程的价钱比几份文件还贵。调用方自己就是线程池线程时也不用怕互等 ——
/// 全局池只剩一个线程时退回串行。
QList<ManifestLoadResult> loadManifests(const QString &root, const QStringList &ids) {
    constexpr qsizetype kParallelThreshold = 8;
    const bool roomToParallel = ids.size() >= kParallelThreshold
        && QThreadPool::globalInstance()->maxThreadCount() > 1;
    if (!roomToParallel) {
        QList<ManifestLoadResult> loaded;
        loaded.reserve(ids.size());
        for (const QString &id : ids) {
            loaded.append(loadForList(root, id));
        }
        return loaded;
    }
    return QtConcurrent::blockingMapped<QList<ManifestLoadResult>>(
        ids, [&root](const QString &id) { return loadForList(root, id); });
}

/// 这一行现在算不算"它那一类的默认项"。
///
/// 显式表过态（下标里有这一类那一格）就只听显式的 —— 哪怕值是空串（"这一类不要默认"），
/// 也不许旧版那个单默认项偷偷顶上来。没表过态时才轮到旧版那个（它只点亮**它自己那一行**，
/// 所以不会一夜之间变成四类都有默认）。
bool isDefaultPreset(const PresetIndex &index, const PresetSummary &summary) {
    if (summary.category != PresetCategory::Unset) {
        const auto it = index.categoryDefaults.constFind(presetCategoryKey(summary.category));
        if (it != index.categoryDefaults.constEnd()) {
            return !it.value().isEmpty() && it.value() == summary.id;
        }
    }
    return summary.id == index.defaultPresetId;
}

}  // namespace

QList<PresetSummary> list(const QString &root, QList<BrokenPreset> *broken) {
    const PresetIndex index = loadIndex(root, nullptr);
    // 盘上那份摘要缓存只在**这个进程第一次问这个 root** 时灌进来；之后靠内存那张，
    // 每次 list 的 stat 校验负责发现"盘上变了"。
    ensureSummaryCacheLoaded(root);

    // 先把这一趟的次序定下来：下标里的（用户排好的），再接盘上多出来的。
    QStringList ordered;
    QSet<QString> seen;
    for (const QString &id : index.order) {
        seen.insert(id);
        ordered.append(id);
    }
    {
        // 盘上有、下标里没有的（用户自己拷进来一份、或者下标被写坏过）→ **追加在末尾**。
        // 不丢东西是这一条的全部理由：那些目录里躺着用户真正的预设。
        QStringList extras;
        const QDir dir(root);
        const QStringList entries =
            dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QString &entry : entries) {
            if (!isValidPresetId(entry) || seen.contains(entry)) {
                continue;
            }
            extras.append(entry);
        }
        // 读盘那一趟的次序要确定（`entryList` 的次序是文件系统给的，两台机器上不一样）。
        extras.sort();
        ordered.append(extras);
    }

    // 第一趟只 stat（一份十来微秒），把缓存命中的挑出来；没命中的攒起来一起读。
    struct Slot {
        bool exists = false;
        qint64 size = -1;
        qint64 mtimeMs = -1;
        /// 有东西可报：缓存命中，或者刚读回来并且读成功了。
        bool resolved = false;
        SummaryCacheEntry entry;
    };
    // 变量名别叫 `slots`：那是 Qt 的空宏（`private slots:` 那个）。
    QList<Slot> cells;
    cells.resize(ordered.size());
    QStringList missIds;
    QList<int> missAt;
    for (int i = 0; i < ordered.size(); ++i) {
        Slot &cell = cells[i];
        const QString path = manifestPath(root, ordered.at(i));
        const QFileInfo info(path);
        cell.exists = info.exists();
        if (!cell.exists) {
            // 盘上没有：照样"读"一次 —— 那条路会说清"已经不在了"并进 broken。
            missIds.append(ordered.at(i));
            missAt.append(i);
            continue;
        }
        cell.size = info.size();
        cell.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        const QMutexLocker locker(&g_summaryCacheMutex);
        const auto it = g_summaryCache.constFind(path);
        if (it != g_summaryCache.constEnd() && it->size == cell.size
            && it->mtimeMs == cell.mtimeMs) {
            cell.entry = *it;
            cell.resolved = true;
            continue;
        }
        missIds.append(ordered.at(i));
        missAt.append(i);
    }

    // 第二趟：没命中的读回来（够多就并行）。结果按次序落回各自的格子上。
    bool freshReads = false;
    if (!missIds.isEmpty()) {
        const QList<ManifestLoadResult> loaded = loadManifests(root, missIds);
        for (int k = 0; k < loaded.size(); ++k) {
            const QString &id = missIds.at(k);
            Slot &cell = cells[missAt.at(k)];
            const ManifestLoadResult &result = loaded.at(k);
            if (!result.ok) {
                // 坏了的那一份**不进缓存**：它多半是"还没写完"或者"正被改"，
                // 记住一个错误只会让下一趟也看不见它。修好之后自然会读回来。
                if (broken != nullptr) {
                    broken->append(BrokenPreset{id, result.reason});
                }
                continue;
            }
            cell.entry.size = cell.size;
            cell.entry.mtimeMs = cell.mtimeMs;
            cell.entry.listed = !result.manifest.internalComponent;
            if (cell.entry.listed) {
                PresetSummary &summary = cell.entry.summary;
                summary.id = id;
                summary.name = result.manifest.name;
                summary.description = result.manifest.description;
                summary.updatedAt = result.manifest.updatedAt;
                summary.sectionCount = result.manifest.sections.size();
                summary.itemCount = result.manifest.modReferences.size();
                if (!result.manifest.modReferences.isEmpty())
                    summary.componentItemCounts.insert(QStringLiteral("mods"), result.manifest.modReferences.size());
                for (const auto &section : result.manifest.sections) {
                    summary.itemCount += section.items.size();
                    const QString part = editorSectionPart(section);
                    if (!part.isEmpty()) summary.componentItemCounts[part] += section.items.size();
                }
                summary.category = result.manifest.category;
                summary.componentCategories =
                    result.manifest.editorState.value("components").toObject().keys();
                // 旧综合投影的资源包部分曾同时包含光影；摘要应反映该投影真正会应用的数量。
                if (summary.componentCategories.contains(QStringLiteral("resourcePacks")) &&
                    !summary.componentCategories.contains(QStringLiteral("shaderPacks"))) {
                    summary.componentItemCounts[QStringLiteral("resourcePacks")] +=
                        summary.componentItemCounts.take(QStringLiteral("shaderPacks"));
                }
                summary.iconPath = result.manifest.editorState.value("icon").toString();
            }
            cell.resolved = true;
            freshReads = true;
            if (cell.exists) {
                const QMutexLocker locker(&g_summaryCacheMutex);
                if (g_summaryCache.size() >= kSummaryCacheEntriesLimit) {
                    g_summaryCache.clear();
                }
                g_summaryCache.insert(manifestPath(root, id), cell.entry);
            }
        }
    }

    // 这一趟真读回来新的（缓存没命中、而且读成功了）⇒ 把整张摘要重写到盘上。
    // 全是缓存命中的时候一个字节都不写：`list()` 热的那一趟还是十毫秒级。
    if (freshReads) {
        writeSummaryCacheFile(root, ordered);
    }

    QList<PresetSummary> summaries;
    summaries.reserve(ordered.size());
    for (int i = 0; i < ordered.size(); ++i) {
        const Slot &cell = cells.at(i);
        // 内部部件（`internalComponent`）本来就不列，但也要记住 —— 否则每一趟
        // 都还得把它整份 manifest 读一遍才知道该跳过。
        if (!cell.resolved || !cell.entry.listed) {
            continue;
        }
        PresetSummary summary = cell.entry.summary;
        summary.id = ordered.at(i);
        // `isDefault` 不进缓存：它由每一次调用的下标决定，而缓存是跨调用活着的。
        summary.isDefault = isDefaultPreset(index, summary);
        summaries.append(summary);
    }
    return summaries;
}

namespace {

/// 回收一份内部投影目录：路径必须在根里、先取目录锁、再整棵移进回收站。
/// 不追链接的规矩由 `moveToRecycleBin()` 接着守（和 `remove()` 同一条）。
bool recycleInternalComponent(const QString &root, const QString &id) {
    if (!isValidPresetId(id)) {
        return false;
    }
    const QString dir = presetDir(root, id);
    if (!pathIsInsideRoot(root, dir) || !WriteDirectoryGuard::acquire(dir)) {
        return false;
    }
    if (!QFileInfo::exists(dir)) {
        return true;
    }
    return moveToRecycleBin(dir);
}

}  // namespace

OrphanComponents orphanComponentIds(const QString &root) {
    OrphanComponents result;
    bool indexOk = true;
    const PresetIndex index = loadIndex(root, &indexOk);
    if (!indexOk) {
        // 下标读不回来 ⇒ 不知道默认项指着谁；这一趟不猜。
        result.unreadable = 1;
        return result;
    }

    // 和 `list()` 看同一批：下标里的 + 盘上多出来的合法 id。
    QStringList ids;
    QSet<QString> seen;
    for (const QString &id : index.order) {
        if (seen.contains(id)) {
            continue;
        }
        seen.insert(id);
        ids.append(id);
    }
    QStringList extras;
    const QStringList entries =
        QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QString &entry : entries) {
        if (!isValidPresetId(entry) || seen.contains(entry)) {
            continue;
        }
        extras.append(entry);
    }
    extras.sort();
    for (const QString &id : extras) {
        ids.append(id);
    }

    const QList<ManifestLoadResult> loaded = loadManifests(root, ids);

    QSet<QString> referenced;
    if (!index.defaultPresetId.isEmpty()) {
        referenced.insert(index.defaultPresetId);
    }
    for (auto it = index.categoryDefaults.cbegin(); it != index.categoryDefaults.cend(); ++it) {
        if (!it.value().isEmpty()) {
            referenced.insert(it.value());
        }
    }

    QList<PresetManifest> manifests;
    manifests.reserve(loaded.size());
    for (const ManifestLoadResult &one : loaded) {
        if (!one.ok) {
            ++result.unreadable;
            continue;
        }
        manifests.append(one.manifest);
        const QJsonObject parts =
            one.manifest.editorState.value(QStringLiteral("components")).toObject();
        for (auto it = parts.begin(); it != parts.end(); ++it) {
            if (it.value().isString()) {
                referenced.insert(it.value().toString());
            }
        }
    }
    // 有一份读不回来就不知道它引用了谁 —— 整趟停手，`ids` 保持空。
    if (result.unreadable > 0) {
        return result;
    }

    for (const PresetManifest &manifest : manifests) {
        if (!manifest.internalComponent || referenced.contains(manifest.id) ||
            index.order.contains(manifest.id)) {
            continue;
        }
        result.ids.append(manifest.id);
    }
    return result;
}

OrphanSweepResult sweepOrphanComponents(const QString &root) {
    OrphanSweepResult result;
    const OrphanComponents audit = orphanComponentIds(root);
    result.unreadable = audit.unreadable;
    for (const QString &id : audit.ids) {
        if (recycleInternalComponent(root, id)) {
            result.removedIds.append(id);
        } else {
            result.failedIds.append(id);
        }
    }
    return result;
}

bool discardInternalComponent(const QString &root, const QString &id) {
    if (!isValidPresetId(id)) {
        return false;
    }
    if (QFileInfo::exists(manifestPath(root, id))) {
        bool ok = false;
        const PresetManifest manifest = loadManifest(root, id, &ok);
        // 清单在却读不回来：不是「刚创建」的那一份，不碰。
        if (!ok || !manifest.internalComponent) {
            return false;
        }
    }
    return recycleInternalComponent(root, id);
}

void forgetSummaryCache(const QString &root, bool keepDiskCache) {
    // 键是 manifest 的完整路径，所以按"这个根下面"整个扫一遍。
    const QString rootKey = summaryCacheRootKey(root);
    const QString prefix = rootKey + QLatin1Char('/');
    QStringList cacheFiles;
    if (!keepDiskCache) {
        cacheFiles.append(summaryCachePath(rootKey));
    }
    {
        const QMutexLocker locker(&g_summaryCacheMutex);
        for (auto it = g_summaryCache.begin(); it != g_summaryCache.end();) {
            if (QFileInfo(it.key()).absolutePath().startsWith(prefix)) {
                it = g_summaryCache.erase(it);
            } else {
                ++it;
            }
        }
        // 盘上那份也要删（`root` 自己和它底下已经灌过的那些，比如 `root/drafts`）——
        // 不删的话"冷读"测量下一次还是会从摘要文件里拿到答案。`keepDiskCache` 那一档
        // 只清"已灌过"的记号：下一趟 `list()` 会重新把盘上那份灌进来。
        for (auto it = g_summaryCacheLoadedRoots.begin(); it != g_summaryCacheLoadedRoots.end();) {
            if (*it == rootKey || it->startsWith(prefix)) {
                if (!keepDiskCache) {
                    cacheFiles.append(summaryCachePath(*it));
                }
                it = g_summaryCacheLoadedRoots.erase(it);
            } else {
                ++it;
            }
        }
        // 不论哪一档都清：新进程里这一串本来就是空的，留着就等于假装本进程的写还在。
        for (auto it = g_summaryCacheWrittenPaths.begin(); it != g_summaryCacheWrittenPaths.end();) {
            if (QFileInfo(*it).absolutePath().startsWith(prefix)) {
                it = g_summaryCacheWrittenPaths.erase(it);
            } else {
                ++it;
            }
        }
    }
    cacheFiles.removeDuplicates();
    for (const QString &path : cacheFiles) {
        QFile::remove(path);
    }
}

QString defaultPresetId(const QString &root) {
    return loadIndex(root, nullptr).defaultPresetId;
}

PresetDefaults defaults(const QString &root) {
    const PresetIndex index = loadIndex(root, nullptr);
    PresetDefaults out;
    out.legacyId = index.defaultPresetId;
    out.byCategory = index.categoryDefaults;
    return out;
}

QString defaultFor(const PresetDefaults &defaults, PresetCategory category,
                   PresetCategory legacyCategory) {
    if (category == PresetCategory::Unset) {
        // 混合/认不出的类别没有"分类默认"：老那个单默认项就是它的全部答案。
        return defaults.legacyId;
    }
    const auto it = defaults.byCategory.constFind(presetCategoryKey(category));
    if (it != defaults.byCategory.constEnd()) {
        return it.value();  // 空串 = 这一类明确不要默认（不是"没表过态"）
    }
    // 没表过态：旧版那个单默认项**自己就属于这一类**时才顶上来。它要是混合预设
    // （调用方算出来是 `Unset`）就不推导 —— 把一份混合预设拆成几类默认正是
    // 「不静默重组混合默认」不许做的事。
    return legacyCategory == category ? defaults.legacyId : QString();
}

// ---------------------------------------------------------------------------
// 增删改
// ---------------------------------------------------------------------------

PresetManifest create(const QString &root, const QString &name, const QString &description,
                      PresetCategory category) {
    const QString cleaned = cleanDisplayName(name);
    if (cleaned.isEmpty()) {
        // 名字是这一份身份里给用户看的那一半，空的建不了（界面上"新建"那一趟总会先问到名字）。
        return PresetManifest();
    }
    PresetManifest manifest;
    manifest.id = newPresetId();
    manifest.name = cleaned;
    manifest.description = cleanDescription(description);
    manifest.createdAt = QDateTime::currentDateTime();
    manifest.updatedAt = manifest.createdAt;
    manifest.category = category;

    // mkpath 就是**这一份新预设的第一笔写入**：和 `duplicate()` 一样，先把所属数据根
    // 的生命周期锁取上（另一份启动器持着时连目录都不建）。
    if (!WriteDirectoryGuard::acquire(presetDir(root, manifest.id))) {
        return PresetManifest();
    }
    if (!QDir().mkpath(filesDir(root, manifest.id))) {
        return PresetManifest();
    }
    if (!saveManifest(root, manifest)) {
        // 建目录成、清单没写成 ⇒ 把这个 id 的目录整棵收掉。不收的话盘上
        // 留下一个**有目录没 manifest** 的 UUID：`list()` 会把它当成"坏预设"列出来，
        // 用户看到一条自己没建过的坏条目，重试还会再留一个。`duplicate()` 那条路也是这么收的。
        const QString dir = presetDir(root, manifest.id);
        if (QFileInfo::exists(dir)) {
            removeTreeWithoutFollowingLinks(dir);
        }
        return PresetManifest();
    }

    bool indexReadable = true;
    PresetIndex index = loadIndex(root, &indexReadable);
    if (indexReadable) {
        // 下标读不回来时**不去覆盖它**（那会把用户的顺序抹平）；代价是刚建的这一份这一趟
        // 进不了顺序表 —— 而 `list()` 会把"盘上有、下标里没有"的那一份追加在末尾列出来，
        // 所以它从用户的角度依然是"建出来了"。
        if (saveIndex(root, appendToIndex(index, manifest.id))) {
            return manifest;
        }
    }
    return manifest;
}

PresetManifest duplicate(const QString &root, const QString &id, const QString &newName) {
    bool ok = false;
    QString reason;
    const PresetManifest source = loadManifest(root, id, &ok, &reason);
    if (!ok) {
        return PresetManifest();
    }
    PresetManifest copy = source;
    copy.id = newPresetId();
    const QString cleaned = cleanDisplayName(newName);
    copy.name = cleaned.isEmpty() ? cleanDisplayName(source.name + QCoreApplication::translate("PresetStore", " 副本")) : cleaned;
    if (copy.name.isEmpty()) {
        return PresetManifest();
    }
    copy.sections = source.sections;
    copy.createdAt = QDateTime::currentDateTime();
    copy.updatedAt = copy.createdAt;

    // 同上：锁落在这一份副本真正住的那个数据根上（不是当前游戏文件夹）。
    if (!WriteDirectoryGuard::acquire(presetDir(root, copy.id))) return PresetManifest();
    const QString newDir = presetDir(root, copy.id);
    if (!QDir().mkpath(newDir)) {
        return PresetManifest();
    }
    const QString fromFiles = filesDir(root, id);
    if (QFileInfo::exists(fromFiles)) {
        QString copyProblem;
        if (!copyTreeWithoutFollowingLinks(fromFiles, filesDir(root, copy.id), {}, &copyProblem)) {
            // 拷了一半：把这一份的新目录整棵收掉，别在预设列表里留一个半成品。
            // 这个入口的回调只有"成功/空清单"两种答案，首因至少留在日志里（P0 取证用）。
            qWarning() << "PresetStore: cannot duplicate preset files:" << copyProblem;
            removeTreeWithoutFollowingLinks(newDir);
            return PresetManifest();
        }
    } else if (!QDir().mkpath(filesDir(root, copy.id))) {
        return PresetManifest();
    }
    if (!saveManifest(root, copy)) {
        removeTreeWithoutFollowingLinks(newDir);
        return PresetManifest();
    }

    bool indexReadable = true;
    const PresetIndex index = loadIndex(root, &indexReadable);
    if (indexReadable) {
        saveIndex(root, appendToIndex(index, copy.id));
    }
    return copy;
}

bool rename(const QString &root, const QString &id, const QString &newName) {
    const QString cleaned = cleanDisplayName(newName);
    if (cleaned.isEmpty()) {
        return false;
    }
    bool ok = false;
    QString reason;
    PresetManifest manifest = loadManifest(root, id, &ok, &reason);
    if (!ok) {
        return false;
    }
    if (manifest.name == cleaned) {
        // 名字没变：算成功，但**不动** `updatedAt`（"改了一次"这个动作没发生过）。
        return true;
    }
    manifest.name = cleaned;
    manifest.updatedAt = QDateTime::currentDateTime();
    return saveManifest(root, manifest);
}

bool remove(const QString &root, const QString &id) {
    if (!isValidPresetId(id)) {
        return false;
    }
    const QString dir = presetDir(root, id);
    if (!pathIsInsideRoot(root, dir)) {
        return false;
    }
    // 删整棵目录是这一份的第一笔写入：先取所属数据根的生命周期锁（和 `create()` /
    // `duplicate()` 同一条），另一份启动器持着时不删。
    if (!WriteDirectoryGuard::acquire(dir)) {
        return false;
    }
    // 整棵目录**移进回收站**（用户亲手配出来的预设，删错了得能捞回来）；不追链接的规矩
    // 由 moveToRecycleBin() 接着守。第三段的事务暂存仍然走永久删除（那里面没有用户内容）。
    if (QFileInfo::exists(dir) && !moveToRecycleBin(dir)) {
        return false;
    }
    bool indexReadable = true;
    const PresetIndex index = loadIndex(root, &indexReadable);
    if (!indexReadable) {
        // 下标本来就坏着：不拿这一趟去覆盖它。目录已经摘掉了，从用户的角度这一份没了。
        // 引用核对同样依赖一份读得懂的下标，所以这一趟不回收子项（原因同上面那条）。
        return true;
    }
    const bool saved = saveIndex(root, dropFromIndex(index, id));
    // 这一份带过的综合子项可能就此没人引用了；别的父预设还引用着的那几份不动。
    // 失败不影响「删除」本身的答案（那一份已经进了回收站）。
    sweepOrphanComponents(root);
    return saved;
}

bool setDefault(const QString &root, const QString &id) {
    bool indexReadable = true;
    PresetIndex index = loadIndex(root, &indexReadable);
    if (!indexReadable) {
        return false;
    }
    if (id.isEmpty()) {
        index.defaultPresetId.clear();
        return saveIndex(root, index);
    }
    if (!index.contains(id)) {
        return false;
    }
    index.defaultPresetId = id;
    return saveIndex(root, index);
}

bool setDefault(const QString &root, PresetCategory category, const QString &id) {
    if (category == PresetCategory::Unset) {
        // 混合预设没有"这一类"可言。要动它走上面那个单默认项的老入口。
        return false;
    }
    bool indexReadable = true;
    PresetIndex index = loadIndex(root, &indexReadable);
    if (!indexReadable) {
        return false;
    }
    if (!id.isEmpty() && !isValidPresetId(id)) {
        return false;
    }
    // **不要求 `index.contains(id)`**：综合预设里的子项也算"一份预设"（它在安装页
    // 是单独一格），而子项不出现在 `order` 里。
    index.categoryDefaults.insert(presetCategoryKey(category), id);
    return saveIndex(root, index);
}

bool move(const QString &root, const QString &id, int delta) {
    bool indexReadable = true;
    const PresetIndex index = loadIndex(root, &indexReadable);
    if (!indexReadable) {
        return false;
    }
    const PresetIndex moved = moveInIndex(index, id, delta);
    if (moved.order == index.order) {
        return false;
    }
    return saveIndex(root, moved);
}

}  // namespace Presets

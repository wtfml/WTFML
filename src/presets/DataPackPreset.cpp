/**
 * @file DataPackPreset.cpp
 * @brief 实现 DataPackPreset.hpp（数据包属于世界：有世界就部署，没有就记待应用）。
 */

#include "DataPackPreset.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include "GenericFiles.hpp"
#include "manage/FolderManager.hpp"
#include "manage/InstanceFiles.hpp"

namespace Presets {

namespace {

[[maybe_unused]] QString dataPackText(const char *source) { return QCoreApplication::translate("Presets", source); }

constexpr auto kSavesDir = "saves";
constexpr auto kDataPacksDir = "datapacks";
constexpr auto kPendingFormatTag = "wtfml-datapacks/1";
constexpr int kPendingSchemaVersion = 1;

/// 一根链接（symbolic link / junction）。★ 两个都要判（见 `InstanceFiles.hpp`）。
bool isLink(const QFileInfo &info) { return info.isJunction() || info.isSymLink(); }

/**
 * @brief 待应用清单 `<实例>/wtfml.datapacks` 的内容（世界 → 包名）。
 *
 * 读不出来 ⇒ `ok = false` 并给一句人话。★ 这份文件**只是我们自己的一份账**：读不动时
 * 宁可整趟不写它，也不拿一份空的盖掉它（那样用户之前记下的待应用包就没了）。
 */
struct PendingList {
    bool ok = true;
    bool exists = false;
    QString problem;
    QMap<QString, QStringList> worlds;  // 世界名 → 包名（有序、已去重）
};

PendingList readPendingList(const QString &instanceRoot) {
    PendingList out;
    const QString path = instanceRoot + QLatin1Char('/') + dataPackPendingFileName();
    const QFileInfo info(path);
    if (!info.exists()) {
        return out;
    }
    out.exists = true;
    if (!info.isFile()) {
        out.ok = false;
        out.problem = QCoreApplication::translate("Presets", "「%1」不是普通文件，无法读取待应用清单。").arg(path);
        return out;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        out.ok = false;
        out.problem = QCoreApplication::translate("Presets", "无法读取待应用清单「%1」：%2").arg(path, file.errorString());
        return out;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        out.ok = false;
        out.problem = QCoreApplication::translate("Presets", "读待应用清单「%1」读到一半出错了：%2")
                          .arg(path, file.errorString());
        return out;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        out.ok = false;
        out.problem = QCoreApplication::translate("Presets", "待应用清单「%1」不是有效的 JSON，文件已保留。")
                          .arg(path);
        return out;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String(kPendingFormatTag) ||
        root.value(QStringLiteral("schemaVersion")).toInt() != kPendingSchemaVersion) {
        out.ok = false;
        out.problem = QCoreApplication::translate("Presets", "待应用清单「%1」的格式不受此版本支持，文件已保留。")
                          .arg(path);
        return out;
    }
    const QJsonValue worlds = root.value(QStringLiteral("worlds"));
    if (!worlds.isUndefined() && !worlds.isArray()) {
        out.ok = false;
        out.problem = QCoreApplication::translate("Presets", "待应用清单「%1」的 `worlds` 字段应为数组，文件已保留。")
                          .arg(path);
        return out;
    }
    for (const QJsonValue &value : worlds.toArray()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject one = value.toObject();
        const QString world = one.value(QStringLiteral("name")).toString();
        if (world.isEmpty() || !InstanceFiles::validatePathSegment(world).isEmpty()) {
            continue;  // 坏名字不认，也不连坐别的世界
        }
        QStringList packs;
        for (const QJsonValue &packValue : one.value(QStringLiteral("packs")).toArray()) {
            const QString pack = packValue.toString();
            if (!pack.isEmpty() && InstanceFiles::validatePathSegment(pack).isEmpty()) {
                packs.append(pack);
            }
        }
        packs.removeDuplicates();
        out.worlds.insert(world, packs);
    }
    return out;
}

/// 原子写待应用清单。`ok = false` 时原来那一份还在。
bool writePendingList(const QString &instanceRoot, const QMap<QString, QStringList> &worlds,
                      QString *problem) {
    const QString path = instanceRoot + QLatin1Char('/') + dataPackPendingFileName();
    QJsonObject root;
    root.insert(QStringLiteral("format"), QLatin1String(kPendingFormatTag));
    root.insert(QStringLiteral("schemaVersion"), kPendingSchemaVersion);
    QJsonArray array;
    for (auto it = worlds.constBegin(); it != worlds.constEnd(); ++it) {
        if (it.value().isEmpty()) {
            continue;
        }
        QJsonObject one;
        one.insert(QStringLiteral("name"), it.key());
        one.insert(QStringLiteral("packs"), QJsonArray::fromStringList(it.value()));
        array.append(one);
    }
    root.insert(QStringLiteral("worlds"), array);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *problem = QCoreApplication::translate("Presets", "无法写入待应用清单「%1」：%2").arg(path, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        *problem = QCoreApplication::translate("Presets", "写待应用清单「%1」写到一半出错了：%2")
                       .arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *problem = QCoreApplication::translate("Presets", "待应用清单「%1」替换失败：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

/**
 * @brief 删掉一棵树（**一根链接都不跟着删**）。
 *
 * `QDir::removeRecursively()` 会跟着 junction 删到那头去（见 `InstanceFiles.hpp`
 * 那段）。「覆盖」一个同名的数据包目录时，那一棵树里夹着链接的场合虽然怪，但删到
 * 用户别的盘上是不可接受的 —— 所以自己走一遍，碰到链接整项失败、什么都不再删。
 */
bool removeTreeNoLinks(const QString &path, QString *problem) {
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return true;
    }
    if (isLink(info)) {
        *problem = QCoreApplication::translate("Presets", "已跳过链接「%1」。").arg(path);
        return false;
    }
    if (!info.isDir()) {
        if (!QFile::remove(path)) {
            *problem = QCoreApplication::translate("Presets", "无法删除「%1」。").arg(path);
            return false;
        }
        return true;
    }
    QDir dir(path);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                              QDir::System,
                          QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (!removeTreeNoLinks(entry.absoluteFilePath(), problem)) {
            return false;
        }
    }
    if (!QDir().rmdir(path)) {
        *problem = QCoreApplication::translate("Presets", "无法删除目录「%1」。").arg(path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 换包事务登记
//
// 两段改名（目标 → `.wtfml-old-*`，暂存 → 目标）之间有一个**必然**的崩溃窗口：
// 进程死在那儿时，盘上只有两坨名字带前缀的东西，凭名字判断归属是不够的 ——
// 用户完全可能自己建了一个 `.wtfml-copy-<标记>-x` 的目录。
//
// 所以换包之前先在目标旁边落一份登记（`.wtfml-txn-<标记>.json`），每一步之前把
// 接下来要做的事写进去。下一次碰同一个目标时：
//   · 有登记、里面的名字与目标标记逐字对得上 ⇒ 按状态机把这一趟收尾（放回原件或收掉暂存）；
//   · 没有登记、或者登记读不懂/对不上 ⇒ **一个字节都不动**，保留现场并让人工检查。
//
// 登记只证明"是我们写的"，不证明目录里的内容是谁的：恢复时只动登记里逐字记录的那几个
// 名字，且链接一律不碰。
// ---------------------------------------------------------------------------

constexpr auto kTxnFormatTag = "wtfml-datapack-txn/1";
constexpr auto kCopyPrefix = ".wtfml-copy-";
constexpr auto kOldPrefix = ".wtfml-old-";
constexpr auto kTxnPrefix = ".wtfml-txn-";

QString manualCheckText(const QString &base, const QString &parent, const QString &why) {
    return dataPackText("「%1」旁有一次没做完的换包，现场认不准（%2）；"
                       "一个字节都没有动，请先人工检查目录：%3")
        .arg(base, why, QDir::toNativeSeparators(parent));
}

bool pathExists(const QString &path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

/// 单段名字（不能带分隔符、不能是 `.` / `..`）。登记里的名字先过这一关再拼路径。
bool isPlainComponent(const QString &name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..") &&
           !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) &&
           !name.contains(QLatin1Char(':'));
}

QString txnJournalPath(const QString &parent, const QString &base) {
    return parent + QLatin1Char('/') + QLatin1String(kTxnPrefix) + dataPackReplaceToken(base) +
           QStringLiteral(".json");
}

/// 删登记。删不掉时留着也无害：下一次按同一份登记再走一遍恢复。
bool clearJournal(const QString &parent, const QString &base) {
    const QString path = txnJournalPath(parent, base);
    const QFileInfo info(path);
    if (!pathExists(path)) return true;
    if (isLink(info)) return false;
    return QFile::remove(path);
}

/// 原子写登记。`staging` / `displaced` 是**裸文件名**（不是路径），空串表示这一步还没有。
bool writeJournal(const QString &parent, const QString &base, const QString &staging,
                  const QString &displaced, const QString &state, QString *problem) {
    const QString path = txnJournalPath(parent, base);
    if (isLink(QFileInfo(path))) {
        *problem = manualCheckText(base, parent, QCoreApplication::translate("Presets", "登记文件本身是一根链接"));
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QLatin1String(kTxnFormatTag));
    root.insert(QStringLiteral("target"), base);
    root.insert(QStringLiteral("staging"), staging);
    root.insert(QStringLiteral("displaced"), displaced);
    root.insert(QStringLiteral("state"), state);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *problem = QCoreApplication::translate("Presets", "写不下换包登记「%1」：%2").arg(path, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        *problem = QCoreApplication::translate("Presets", "写换包登记「%1」写到一半出错了：%2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *problem = QCoreApplication::translate("Presets", "换包登记「%1」替换失败：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

struct ReplaceJournal {
    QString target;
    QString staging;
    QString displaced;
    QString state;
};

bool readJournal(const QString &path, const QString &base, const QString &parent,
                 ReplaceJournal *out, QString *problem) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *problem = manualCheckText(base, parent, QCoreApplication::translate("Presets", "无法读取登记文件"));
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *problem = manualCheckText(base, parent, QCoreApplication::translate("Presets", "登记文件读到一半出错"));
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        *problem = manualCheckText(base, parent, QCoreApplication::translate("Presets", "登记文件不是能读的 JSON"));
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String(kTxnFormatTag)) {
        *problem = manualCheckText(base, parent, QCoreApplication::translate("Presets", "此版本不支持记录中的文件格式。"));
        return false;
    }
    out->target = root.value(QStringLiteral("target")).toString();
    out->staging = root.value(QStringLiteral("staging")).toString();
    out->displaced = root.value(QStringLiteral("displaced")).toString();
    out->state = root.value(QStringLiteral("state")).toString();
    return true;
}

/// 收掉登记里点名的一棵暂存/旧内容。链接不碰（不受登记支配）。
bool removeRegisteredTree(const QString &path, const QString &base, const QString &parent,
                          QString *problem) {
    if (!pathExists(path)) return true;
    const QFileInfo info(path);
    if (isLink(info)) {
        *problem = manualCheckText(base, parent, QCoreApplication::translate("Presets", "登记记录引用的项目是链接"));
        return false;
    }
    return removeTreeNoLinks(path, problem);
}

/// 按登记把上一趟没做完的换包收尾。没有登记 ⇒ 什么都不做；登记不可信 ⇒ 保留现场并拒绝。
bool recoverJournal(const QString &parent, const QString &base, QString *problem) {
    const QString path = txnJournalPath(parent, base);
    if (!pathExists(path)) return true;
    if (isLink(QFileInfo(path))) {
        *problem = manualCheckText(base, parent, QCoreApplication::translate("Presets", "登记文件本身是一根链接"));
        return false;
    }

    ReplaceJournal journal;
    if (!readJournal(path, base, parent, &journal, problem)) return false;

    const QString token = dataPackReplaceToken(base);
    const QString copyPrefix = QLatin1String(kCopyPrefix) + token + QLatin1Char('-');
    const QString oldPrefix = QLatin1String(kOldPrefix) + token + QLatin1Char('-');
    const auto namedOk = [](const QString &name, const QString &prefix) {
        return name.isEmpty() ||
               (isPlainComponent(name) && name.startsWith(prefix) && name.size() > prefix.size());
    };
    const bool knownState = journal.state == QLatin1String("staging") ||
                            journal.state == QLatin1String("staged") ||
                            journal.state == QLatin1String("displacing") ||
                            journal.state == QLatin1String("displaced") ||
                            journal.state == QLatin1String("publishing") ||
                            journal.state == QLatin1String("published");
    if (journal.target != base || !knownState || !namedOk(journal.staging, copyPrefix) ||
        !namedOk(journal.displaced, oldPrefix)) {
        *problem = manualCheckText(base, parent, QCoreApplication::translate("Presets", "登记文件中的名称或状态不一致"));
        return false;
    }

    const QString target = parent + QLatin1Char('/') + base;
    const QString staging = journal.staging.isEmpty()
                                ? QString()
                                : parent + QLatin1Char('/') + journal.staging;
    const QString displaced = journal.displaced.isEmpty()
                                  ? QString()
                                  : parent + QLatin1Char('/') + journal.displaced;
    const bool targetThere = pathExists(target);
    const bool stagingThere = !staging.isEmpty() && pathExists(staging);
    const bool displacedThere = !displaced.isEmpty() && pathExists(displaced);

    const auto keepAll = [&](const QString &why) {
        *problem = manualCheckText(base, parent, why);
        return false;
    };
    const auto restoreDisplaced = [&]() -> bool {
        if (!displacedThere) return false;
        if (isLink(QFileInfo(displaced))) return false;
        return QDir().rename(displaced, target);
    };

    if (journal.state == QLatin1String("staging") ||
        journal.state == QLatin1String("staged")) {
        // 目标还没动过：只收暂存。
        if (stagingThere && !removeRegisteredTree(staging, base, parent, problem)) return false;
        clearJournal(parent, base);
        return true;
    }
    if (journal.state == QLatin1String("displacing")) {
        // 登记说"正在把目标挪去旧位置"，实际两种都可能是：
        if (targetThere && !displacedThere) {
            // 改名没发生；目标原样，收掉暂存。
            if (stagingThere && !removeRegisteredTree(staging, base, parent, problem)) return false;
        } else if (!targetThere && displacedThere) {
            // 改名发生了；把原件放回去。
            if (!restoreDisplaced()) {
                return keepAll(QCoreApplication::translate("Presets", "原件放不回原位"));
            }
            if (stagingThere && !removeRegisteredTree(staging, base, parent, problem)) return false;
        } else {
            return keepAll(QCoreApplication::translate("Presets", "目标和旧内容同时都在或同时都不在"));
        }
        clearJournal(parent, base);
        return true;
    }
    if (journal.state == QLatin1String("displaced")) {
        // 目标已经挪走、新内容还没发布：一律回滚，用户看到的是换包之前的样子。
        if (targetThere || !displacedThere) {
            return keepAll(QCoreApplication::translate("Presets", "目标和旧内容的实际状态与登记不符"));
        }
        if (!restoreDisplaced()) return keepAll(QCoreApplication::translate("Presets", "原件放不回原位"));
        if (stagingThere && !removeRegisteredTree(staging, base, parent, problem)) return false;
        clearJournal(parent, base);
        return true;
    }
    if (journal.state == QLatin1String("publishing")) {
        if (targetThere && !stagingThere) {
            // 已经发布成功；旧内容可以收。
            if (displacedThere && !removeRegisteredTree(displaced, base, parent, problem)) {
                return false;
            }
        } else if (!targetThere && stagingThere) {
            // 还没换过去：回滚。
            if (displacedThere && !restoreDisplaced()) {
                return keepAll(QCoreApplication::translate("Presets", "原件放不回原位"));
            }
            if (!removeRegisteredTree(staging, base, parent, problem)) return false;
        } else {
            return keepAll(QCoreApplication::translate("Presets", "目标和暂存的实际状态与登记不符"));
        }
        clearJournal(parent, base);
        return true;
    }
    // published：新内容已经在目标上，只收残骸。
    if (displacedThere && !removeRegisteredTree(displaced, base, parent, problem)) return false;
    if (stagingThere && !removeRegisteredTree(staging, base, parent, problem)) return false;
    clearJournal(parent, base);
    return true;
}

/// 从一份登记文件本身读出它认领的目标，再按那一份登记恢复。
/// 文件名必须与它自称的目标算出来的标记一致 —— 改个名字就想认领别人的目录，这一层不认。
bool recoverJournalFile(const QString &parent, const QString &fileName, QString *problem) {
    const QString path = parent + QLatin1Char('/') + fileName;
    if (isLink(QFileInfo(path))) {
        *problem = manualCheckText(fileName, parent, QCoreApplication::translate("Presets", "登记文件本身是一根链接"));
        return false;
    }
    ReplaceJournal journal;
    if (!readJournal(path, fileName, parent, &journal, problem)) return false;
    if (!isPlainComponent(journal.target)) {
        *problem = manualCheckText(fileName, parent, QCoreApplication::translate("Presets", "登记里的目标名不能当目录名"));
        return false;
    }
    if (txnJournalPath(parent, journal.target) != path) {
        *problem = manualCheckText(fileName, parent,
                                   QCoreApplication::translate("Presets", "登记文件名与目标名称不一致"));
        return false;
    }
    return recoverJournal(parent, journal.target, problem);
}

/// 把一个目录里**所有**登记都收尾。
///
/// 为什么不能只在 `copyOverwrite()` 里按目标恢复：换包崩在"目标已经挪走"那一步时，
/// 目标本身是不在的。采集只枚举盘上还存在的项，根本轮不到那个目标调用 `copyOverwrite()`，
/// 残骸就会一直留到用户手动处理。所以每碰一个 datapacks/ 目录，先把这一目录的登记全走一遍。
bool recoverInterruptedReplacesIn(const QString &parent, QString *problem) {
    const QStringList journals = QDir(parent).entryList(
        {QStringLiteral(".wtfml-txn-*.json")},
        QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : journals) {
        if (!recoverJournalFile(parent, name, problem)) return false;
    }
    return true;
}

/// 先按登记收尾；没有登记的残留只能人工检查，文件名前缀不是所有权证明。
/// 不自动删除，也不把一个恰好同名的用户目录当作原件改名。
bool recoverInterruptedReplace(const QString &parent, const QString &base, QString *problem) {
    if (!recoverJournal(parent, base, problem)) return false;
    const QString token = dataPackReplaceToken(base);
    const auto leftovers = QDir(parent).entryList(
        {QStringLiteral(".wtfml-old-") + token + QStringLiteral("-*"),
         QStringLiteral(".wtfml-copy-") + token + QStringLiteral("-*")},
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    if (leftovers.isEmpty()) return true;
    if (problem != nullptr) {
        *problem = dataPackText("「%1」旁有未登记的旧内容或暂存，无法确认归属；已全部保留，"
                               "请先人工检查目录：%2")
                       .arg(base, QDir::toNativeSeparators(parent));
    }
    return false;
}

/// 把一份文件/一棵目录拷到目标上（**换掉目标上已有的那一份**）。目标上一根链接都不跟。
///
/// 次序**先拷到旁边、再换过去**，不能"先删目标、再拷"：拷到一半失败（盘满、源在
/// 枚举之后消失、目标被占用）时，目标上原来那一份**已经没了**，而且采集那一档
/// （往预设 `files/` 里收）没有任何备份可回滚 —— 用户重收一次数据包失败，预设里
/// 之前收好的那一份就永久丢了。先拷的次序里，拷失败时目标原样还在；换过去那一步用的是
/// 目录改名（同盘、原子），失败的话把原来那一份改名回来。
bool copyOverwrite(const QString &source, const QString &target, QString *problem) {
    const QString parent = QFileInfo(target).absolutePath();
    if (!parent.isEmpty() && !QDir().mkpath(parent)) {
        *problem = QCoreApplication::translate("Presets", "无法创建目录「%1」。").arg(parent);
        return false;
    }

    // 上一趟崩在半路留下的残骸先收拾掉（见 recoverInterruptedReplace 文件头）：
    // "目标不见了"那种情况在这里有机会恢复，不能等下一次拷同一份时才发现。
    // 先扫整个目录的登记（可能牵连别的目标），再查这个目标有没有**未登记**的残骸。
    QString sweepProblem;
    if (!recoverInterruptedReplacesIn(parent, &sweepProblem)) {
        *problem = sweepProblem;
        return false;
    }
    const QString base = QFileInfo(target).fileName();
    QString problemText;
    if (!recoverInterruptedReplace(parent, base, &problemText)) {
        *problem = problemText;
        return false;
    }

    const QString token = dataPackReplaceToken(base) + QLatin1Char('-') +
                          QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stagingName = QLatin1String(kCopyPrefix) + token;
    const QString staging = parent + QLatin1Char('/') + stagingName;
    const QString displacedName = QLatin1String(kOldPrefix) + token;
    const QString displaced = parent + QLatin1Char('/') + displacedName;

    // **先登记再动手**。登记落不下去就整趟不换：没有登记的话，崩在半路的残骸
    // 谁也不敢判断归属，只能一直留给用户人工检查。
    if (!writeJournal(parent, base, stagingName, QString(), QStringLiteral("staging"), problem)) {
        return false;
    }
    QString scratch;
    QString copyProblem;
    if (!copyTreeWithoutFollowingLinks(source, staging, {}, &copyProblem)) {
        removeTreeNoLinks(staging, &scratch);
        clearJournal(parent, base);
        *problem = QCoreApplication::translate("Presets", "无法复制「%1」：%2").arg(target, copyProblem);
        return false;
    }
    if (!writeJournal(parent, base, stagingName, QString(), QStringLiteral("staged"), problem)) {
        removeTreeNoLinks(staging, &scratch);
        clearJournal(parent, base);
        return false;
    }

    const QFileInfo targetInfo(target);
    const bool existed = targetInfo.exists() || targetInfo.isSymLink();
    if (existed) {
        if (!writeJournal(parent, base, stagingName, displacedName,
                          QStringLiteral("displacing"), problem)) {
            removeTreeNoLinks(staging, &scratch);
            clearJournal(parent, base);
            return false;
        }
        if (!QDir().rename(target, displaced)) {
            removeTreeNoLinks(staging, &scratch);
            clearJournal(parent, base);
            *problem = QCoreApplication::translate("Presets", "无法替换目标中的「%1」。请检查文件是否被占用。").arg(target);
            return false;
        }
        QString journalWhy;
        if (!writeJournal(parent, base, stagingName, displacedName,
                          QStringLiteral("displaced"), &journalWhy)) {
            // 登记跟不上就不发布：把原件放回去，整趟作废。
            if (QDir().rename(displaced, target)) {
                removeTreeNoLinks(staging, &scratch);
                clearJournal(parent, base);
            }
            *problem = QCoreApplication::translate("Presets", "无法写入替换记录，已取消替换「%1」：%2")
                           .arg(target, journalWhy);
            return false;
        }
    }

    QString publishWhy;
    if (!writeJournal(parent, base, stagingName, existed ? displacedName : QString(),
                      QStringLiteral("publishing"), &publishWhy)) {
        if (existed && QDir().rename(displaced, target)) {
            removeTreeNoLinks(staging, &scratch);
            clearJournal(parent, base);
        }
        *problem = QCoreApplication::translate("Presets", "无法写入替换记录，已取消替换「%1」：%2")
                       .arg(target, publishWhy);
        return false;
    }

    if (!QDir().rename(staging, target)) {
        // 发布失败：先把原件放回去；放不回去时把登记和暂存都留着，下一次按登记恢复。
        if (existed && QDir().rename(displaced, target)) {
            removeTreeNoLinks(staging, &scratch);
            clearJournal(parent, base);
            *problem = QCoreApplication::translate("Presets", "无法替换目标中的「%1」。请检查文件是否被占用。").arg(target);
            return false;
        }
        *problem = existed
                       ? QCoreApplication::translate("Presets", "发布失败，原内容未能归位，仍完整保留在：%1")
                             .arg(QDir::toNativeSeparators(displaced))
                       : QCoreApplication::translate("Presets", "发布失败，新内容仍完整保留在暂存里：%1")
                             .arg(QDir::toNativeSeparators(staging));
        return false;
    }

    if (existed) {
        // 新的已经在目标上；旧内容收不掉就留着登记（下一次按 `published` 那一档再收）。
        QString cleanupWhy;
        if (!removeRegisteredTree(displaced, base, parent, &cleanupWhy)) {
            return true;
        }
    }
    clearJournal(parent, base);
    return true;
}

/// 段里那一项的形状（`saves/<world>/datapacks/<name>`）。
struct ParsedPath {
    QString world;
    QString name;
    bool ok = false;
};

ParsedPath parseDataPackPath(const QString &relativePath) {
    ParsedPath out;
    const QStringList segments = relativePath.split(QLatin1Char('/'));
    if (segments.size() != 4 || segments.at(0) != QLatin1String(kSavesDir) ||
        segments.at(2) != QLatin1String(kDataPacksDir)) {
        return out;
    }
    out.world = segments.at(1);
    out.name = segments.at(3);
    out.ok = true;
    return out;
}

/// 世界目录（`<实例>/saves/<world>`）能不能往下写。链接 ⇒ 不写。
///
/// 判的是**一路上每一层**：只看 `saves/<world>` 这一格本身不够 ——
/// `<实例>/saves` 自己是一根 junction 时，只看最后一格会一字不差地判"能用"。
/// 采集那一侧对 `saves/` 明确判过链接（见下面 `captureDataPacks()`），应用这一侧必须
/// 同一个标准。
bool worldUsable(const QString &instanceRoot, const QString &world, bool *exists) {
    const QString path =
        instanceRoot + QLatin1Char('/') + QLatin1String(kSavesDir) + QLatin1Char('/') + world;
    const QFileInfo info(path);
    const bool linked =
        isLink(info) ||
        InstanceFiles::pathHasLinkComponent(instanceRoot,
                                            QStringLiteral("saves/%1").arg(world));
    *exists = info.exists() && info.isDir() && !linked;
    return !linked;
}

/// 一个包在目标上的落点（有世界 ⇒ 世界里；没有 ⇒ `datapacks-pending/`）。
QString targetPathFor(const QString &instanceRoot, const QString &world, const QString &name,
                      bool worldExists) {
    if (worldExists) {
        return instanceRoot + QLatin1Char('/') + QLatin1String(kSavesDir) + QLatin1Char('/') +
               world + QLatin1Char('/') + QLatin1String(kDataPacksDir) + QLatin1Char('/') + name;
    }
    return instanceRoot + QLatin1Char('/') + dataPackPendingDirName() + QLatin1Char('/') + world +
           QLatin1Char('/') + name;
}

}  // namespace

QString dataPackReplaceToken(const QString &base) {
    // 名字里**不直接嵌文件名**：暂存和备份就在同一个目录里，长文件名再拼一截会顶到
    // Windows 的路径上限（而那时源文件是好端端的，失败纯属自找）。
    return QString::fromLatin1(
        QCryptographicHash::hash(base.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
}

QString dataPackSectionId() {
    return sectionIdFor(SectionKind::Custom, QStringLiteral("dataPacks"));
}

bool dataPackHandlesSection(const PresetSection &section) {
    return section.kind == SectionKind::Custom && section.id == dataPackSectionId();
}

QString dataPackPendingFileName() {
    return QStringLiteral("wtfml.datapacks");
}

QString dataPackPendingDirName() {
    return QStringLiteral("datapacks-pending");
}

bool dataPackItemsFromSection(const PresetSection &section, QList<DataPackItem> *out,
                              QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (!dataPackHandlesSection(section)) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("Presets", "此部分不是数据包配置。");
        }
        return false;
    }
    QList<DataPackItem> items;
    for (const QJsonValue &value : section.items) {
        if (!value.isObject()) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate("Presets", "数据包这一段里有不是对象的一项。");
            }
            return false;
        }
        const QJsonObject object = value.toObject();
        const QString relative = object.value(QStringLiteral("path")).toString();
        QString why;
        if (!genericPathIsSafe(relative, &why)) {
            if (problemOut != nullptr) {
                *problemOut = why;
            }
            return false;
        }
        const ParsedPath parsed = parseDataPackPath(relative);
        if (!parsed.ok) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate("Presets", "「%1」不是 saves/<世界>/datapacks/<包> 这种路径。")
                                  .arg(relative);
            }
            return false;
        }
        const QString world = object.value(QStringLiteral("world")).toString();
        if (!world.isEmpty() && world != parsed.world) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate("Presets", "世界「%1」的名称与路径不一致。").arg(relative);
            }
            return false;
        }
        DataPackItem item;
        item.world = parsed.world;
        item.name = parsed.name;
        item.relativePath = relative;
        items.append(item);
    }
    if (out != nullptr) {
        *out = items;
    }
    return true;
}

bool sectionsFromDataPacks(const QList<DataPackItem> &items, QList<PresetSection> *out,
                           QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    PresetSection section;
    section.id = dataPackSectionId();
    section.kind = SectionKind::Custom;
    section.title = QCoreApplication::translate("Presets", "数据包");
    section.mode = ApplyMode::CopyIfMissing;
    for (const DataPackItem &item : items) {
        const QString relative = QString::fromLatin1(kSavesDir) + QLatin1Char('/') + item.world +
                                 QLatin1Char('/') + QLatin1String(kDataPacksDir) +
                                 QLatin1Char('/') + item.name;
        QString why;
        if (!genericPathIsSafe(relative, &why)) {
            if (problemOut != nullptr) {
                *problemOut = why;
            }
            return false;
        }
        QJsonObject row;
        row.insert(QStringLiteral("world"), item.world);
        row.insert(QStringLiteral("path"), relative);
        section.items.append(row);
    }
    if (section.items.isEmpty()) {
        return true;  // 空段：不写进清单（调用方自己判）
    }
    if (out != nullptr) {
        out->append(section);
    }
    return true;
}

DataPackCaptureReport captureDataPacks(const QString &instanceRoot,
                                       const QString &presetFilesRoot) {
    DataPackCaptureReport report;

    if (instanceRoot.trimmed().isEmpty() || !QFileInfo(instanceRoot).isDir()) {
        report.problem = QCoreApplication::translate("Presets", "未找到实例目录，无法提取数据包。");
        return report;
    }
    if (presetFilesRoot.trimmed().isEmpty()) {
        report.problem = QCoreApplication::translate("Presets", "未指定预设的 files 目录。");
        return report;
    }

    const QString savesPath = instanceRoot + QLatin1Char('/') + QLatin1String(kSavesDir);
    const QFileInfo savesInfo(savesPath);
    if (!savesInfo.exists()) {
        report.ok = true;
        report.notes.append(QCoreApplication::translate("Presets", "实例缺少 saves/ 目录，无法提取数据包。"));
        return report;
    }
    if (isLink(savesInfo)) {
        report.problem = QCoreApplication::translate("Presets", "「%1」为链接，未跟随。").arg(savesPath);
        return report;
    }
    if (!savesInfo.isDir()) {
        report.problem = QCoreApplication::translate("Presets", "「%1」不是目录，未扫描。").arg(savesPath);
        return report;
    }

    const QFileInfoList worldDirs =
        QDir(savesPath).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                                          QDir::System,
                                      QDir::Name);
    QList<DataPackItem> items;
    QSet<QString> worldsWithPacks;
    bool sawAnyDatapacksDir = false;

    for (const QFileInfo &worldDir : worldDirs) {
        if (isLink(worldDir)) {
            report.problems.append(QCoreApplication::translate("Presets", "存档「%1」为链接，未跟随。")
                                       .arg(worldDir.fileName()));
            continue;
        }
        const QString world = worldDir.fileName();
        if (!InstanceFiles::validatePathSegment(world).isEmpty()) {
            report.problems.append(
                QCoreApplication::translate("Presets", "存档名称「%1」无法用作目录名，已跳过。").arg(world));
            continue;
        }
        const QString packsPath = worldDir.absoluteFilePath() + QLatin1Char('/') +
                                  QLatin1String(kDataPacksDir);
        const QFileInfo packsInfo(packsPath);
        if (!packsInfo.exists()) {
            continue;
        }
        sawAnyDatapacksDir = true;
        if (isLink(packsInfo)) {
            report.problems.append(QCoreApplication::translate("Presets", "存档「%1」的 datapacks/ 是链接，已跳过。")
                                       .arg(world));
            continue;
        }
        if (!packsInfo.isDir()) {
            report.problems.append(QCoreApplication::translate("Presets", "存档「%1」的 datapacks 不是目录，已跳过。")
                                       .arg(world));
            continue;
        }

        // 目标已经挪走那种中断里，现场一整项都"不在"，枚举根本看不到它 ——
        // 先把这一目录的登记全收尾，再枚举。
        QString sweepProblem;
        if (!recoverInterruptedReplacesIn(packsPath, &sweepProblem)) {
            report.problems.append(sweepProblem);
            continue;  // 这个世界的换包现场没弄明白，这一趟不收它
        }
        const QFileInfoList entries =
            QDir(packsPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                                              QDir::Hidden | QDir::System,
                                          QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QString name = entry.fileName();
            if (name.startsWith(QStringLiteral(".wtfml-"))) {
                // 换包事务自己的东西（登记 / 暂存 / 上一趟的旧内容）。它们不是用户的
                // 数据包，更不该被收进预设 —— 上一次换包崩在半路时它们才留在盘上。
                report.problems.append(
                    dataPackText("「%1」里有启动器换包留下的「%2」，没有收它；"
                                 "下一次应用同一项时会按登记处理，没有登记的话请人工检查。")
                        .arg(world, name));
                continue;
            }
            if (isLink(entry)) {
                report.problems.append(QCoreApplication::translate("Presets", "「%1」中的「%2」为链接，未收集。")
                                           .arg(world, name));
                continue;
            }
            if (!InstanceFiles::validatePathSegment(name).isEmpty()) {
                report.problems.append(QCoreApplication::translate("Presets", "「%1」中有一项名称不能作为文件名（%2），未收集。")
                                           .arg(world, name));
                continue;
            }
            const QString relative = QString::fromLatin1(kSavesDir) + QLatin1Char('/') + world +
                                     QLatin1Char('/') + QLatin1String(kDataPacksDir) +
                                     QLatin1Char('/') + name;
            const QString target = presetFilesRoot + QLatin1Char('/') + relative;
            QString why;
            if (!copyOverwrite(entry.absoluteFilePath(), target, &why)) {
                report.problems.append(
                    QCoreApplication::translate("Presets", "无法提取「%1」：%2").arg(relative, why));
                continue;
            }
            DataPackItem item;
            item.world = world;
            item.name = name;
            item.relativePath = relative;
            items.append(item);
            worldsWithPacks.insert(world);
        }
    }

    if (items.isEmpty()) {
        report.ok = true;
        report.notes.append(sawAnyDatapacksDir
                                ? QCoreApplication::translate("Presets", "存档里的 datapacks/ 都是空的。")
                                : QCoreApplication::translate("Presets", "未找到包含 datapacks/ 的世界存档。"));
        return report;
    }

    QList<PresetSection> made;
    QString why;
    if (!sectionsFromDataPacks(items, &made, &why)) {
        report.problem = why;
        return report;
    }
    report.sections = made;
    report.worlds = static_cast<int>(worldsWithPacks.size());
    report.packs = static_cast<int>(items.size());
    report.notes.append(QCoreApplication::translate("Presets", "已收集 %1 个存档中的 %2 个数据包。")
                            .arg(report.worlds)
                            .arg(report.packs));
    report.ok = true;
    return report;
}

DataPackPlan planDataPacks(const QString &instanceRoot, const QList<DataPackItem> &items,
                           ApplyMode mode) {
    DataPackPlan plan;
    if (instanceRoot.trimmed().isEmpty()) {
        plan.problem = QCoreApplication::translate("Presets", "未指定目标实例，无法制定数据包操作计划。");
        return plan;
    }

    for (const DataPackItem &raw : items) {
        DataPackPlanItem item;
        item.world = raw.world;
        item.name = raw.name;
        item.relativePath = raw.relativePath;

        QString why;
        if (!genericPathIsSafe(item.relativePath, &why)) {
            plan.problem = why;
            return plan;
        }
        const ParsedPath parsed = parseDataPackPath(item.relativePath);
        if (!parsed.ok || parsed.world != item.world || parsed.name != item.name) {
            plan.problem = QCoreApplication::translate("Presets", "预设里的数据包项「%1」形状不对。").arg(item.relativePath);
            return plan;
        }

        bool exists = false;
        if (!worldUsable(instanceRoot, item.world, &exists)) {
            // 世界那一格是一根链接：不往里写。当成"没有这个世界"会悄悄记成待应用，
            // 但用户明明有那个存档 —— 所以整段做不了，理由说清楚。
            plan.problem =
                QCoreApplication::translate("Presets", "目标上的存档「%1」是一根链接（或者 `saves/` 这一层是），没有往里写。")
                    .arg(item.world);
            return plan;
        }
        const QFileInfo worldInfo(instanceRoot + QLatin1Char('/') + QLatin1String(kSavesDir) +
                                  QLatin1Char('/') + item.world);
        if (!exists && worldInfo.exists()) {
            plan.problem = QCoreApplication::translate("Presets", "目标「saves/%1」不是目录，未写入。")
                               .arg(item.world);
            return plan;
        }
        item.worldExists = exists;
        const QString target = targetPathFor(instanceRoot, item.world, item.name, exists);
        const QFileInfo targetInfo(target);
        item.targetExists = targetInfo.exists() || targetInfo.isSymLink();

        switch (mode) {
        case ApplyMode::Skip:
            ++plan.skipped;
            break;
        case ApplyMode::Merge:
        case ApplyMode::Append:
            ++plan.needsChoice;
            break;
        case ApplyMode::CopyIfMissing:
            if (item.targetExists) {
                ++plan.skipped;
            } else if (item.worldExists) {
                ++plan.deploy;
            } else {
                ++plan.deferred;
            }
            break;
        case ApplyMode::Replace:
            if (item.worldExists) {
                ++plan.deploy;
            } else {
                ++plan.deferred;
            }
            break;
        }
        plan.items.append(item);
    }
    plan.ok = true;
    return plan;
}

DataPackApplyReport applyDataPacks(const QString &presetFilesRoot, const QString &instanceRoot,
                                   const PresetSection &section, ApplyMode mode,
                                   bool replaceConfirmed) {
    DataPackApplyReport report;

    QList<DataPackItem> items;
    QString why;
    if (!dataPackItemsFromSection(section, &items, &why)) {
        report.problem = why;
        return report;
    }

    if (instanceRoot.trimmed().isEmpty() || !QFileInfo(instanceRoot).isDir()) {
        report.problem = QCoreApplication::translate("Presets", "未找到目标实例目录，无法应用数据包。");
        return report;
    }
    if (presetFilesRoot.trimmed().isEmpty()) {
        report.problem = QCoreApplication::translate("Presets", "预设缺少 files/ 根目录，无法复制数据包。");
        return report;
    }

    if (mode == ApplyMode::Skip) {
        report.ok = true;
        report.skipped = static_cast<int>(items.size());
        return report;
    }
    if (mode == ApplyMode::Merge || mode == ApplyMode::Append) {
        // 数据包是游戏自己读的整份东西：我们不解释它里面的内容，"合并"无从谈起。
        // 这一档一个字节都不写，只把"要你选"的数报回去（和通用文件组那一档同一条）。
        report.ok = true;
        report.needsChoice = static_cast<int>(items.size());
        report.details.append(dataPackText("这一档下数据包做不了主（要做的是整份拷进去），"
                                           "没有动任何文件；请把这一段改成「覆盖」或「缺了才拷」。"));
        return report;
    }
    if (mode == ApplyMode::Replace && !replaceConfirmed) {
        report.problem = QCoreApplication::translate("Presets", "覆盖操作尚未确认，数据包未修改。");
        return report;
    }

    const DataPackPlan plan = planDataPacks(instanceRoot, items, mode);
    if (!plan.ok) {
        report.problem = plan.problem;
        return report;
    }

    // 待应用清单：动手之前先读一遍。读不动 ⇒ 整段不动（不拿一份空的盖掉用户的账）。
    PendingList pending;
    bool hasDeferred = false;
    for (const DataPackPlanItem &item : plan.items) {
        if (!item.worldExists && !(mode == ApplyMode::CopyIfMissing && item.targetExists)) {
            hasDeferred = true;
            break;
        }
    }
    if (hasDeferred) {
        pending = readPendingList(instanceRoot);
        if (!pending.ok) {
            report.problem = pending.problem;
            return report;
        }
    }

    int deployed = 0;
    int deferred = 0;
    int skipped = 0;

    for (const DataPackPlanItem &item : plan.items) {
        const QString source = presetFilesRoot + QLatin1Char('/') + item.relativePath;
        const QFileInfo sourceInfo(source);
        if (!sourceInfo.exists()) {
            report.problem = QCoreApplication::translate("Presets", "预设的 files/ 目录中未找到「%1」。")
                                 .arg(item.relativePath);
            // 已经落下去的那些留在盘上（和通用文件组那一条一致）。
            return report;
        }
        if (isLink(sourceInfo)) {
            report.problem = QCoreApplication::translate("Presets", "预设中的「%1」是链接，已跳过。")
                                 .arg(item.relativePath);
            return report;
        }
        // 预设 `files/` 里这一条**路过的**某一层是链接（手改过的预设、或者别处塞进来的
        // 一棵树）：上面那一格只判最后那一个包名，判不到 `saves/` / `datapacks/` 中间层。
        if (InstanceFiles::pathHasLinkComponent(presetFilesRoot, item.relativePath)) {
            report.problem = QCoreApplication::translate("Presets", "预设中「%1」路径包含链接，未跟随。")
                                 .arg(item.relativePath);
            return report;
        }

        const QString target = targetPathFor(instanceRoot, item.world, item.name, item.worldExists);
        const QFileInfo targetInfo(target);
        const bool targetExists = targetInfo.exists() || targetInfo.isSymLink();

        if (mode == ApplyMode::CopyIfMissing && targetExists) {
            ++skipped;
            continue;
        }
        if (targetExists && isLink(targetInfo)) {
            report.problem =
                QCoreApplication::translate("Presets", "目标「%1」是链接，已跳过。").arg(target);
            return report;
        }
        // 落点**上面那几层**（`saves/`、`saves/<世界>/datapacks/`、`datapacks-pending/`）
        // 也得是普通目录：存档放在网盘/另一块盘是常见做法，那些目录被做成
        // junction 时，`removeTreeNoLinks()` / 拷贝会一路穿到链接那头 —— 删改的就不是
        // "这个实例里的包"了。`worldUsable()` 只判到 `<世界>` 这一格，`datapacks/` 那一层
        // 在这儿。
        const QString parentRelative =
            item.worldExists ? QStringLiteral("saves/%1/datapacks").arg(item.world)
                             : QStringLiteral("%1/%2").arg(dataPackPendingDirName(), item.world);
        if (InstanceFiles::pathHasLinkComponent(instanceRoot, parentRelative)) {
            report.problem =
                QCoreApplication::translate("Presets", "目标上的「%1」是一根链接（或者它上面一层是），没有往里写。")
                    .arg(parentRelative);
            return report;
        }
        // 「覆盖」**不再先删目标**。`copyOverwrite()` 自己就是"先拷到旁边、
        // 再改名换过去，换不上去把原来那一份放回去"的原子换法 —— 这里再来一发预删除，
        // 等于把它的保护绕开：拷贝失败（盘满 / IO 错）时目标上原来那个包已经没了，
        // 而备份那一份不会自动还原，用户只能自己去 `presets/backups/` 里摸。
        QString copyWhy;
        if (!copyOverwrite(source, target, &copyWhy)) {
            report.problem = copyWhy;
            return report;
        }

        if (item.worldExists) {
            ++deployed;
        } else {
            ++deferred;
            QStringList &packs = pending.worlds[item.world];
            if (!packs.contains(item.name)) {
                packs.append(item.name);
                packs.sort();
            }
        }
    }

    if (deferred > 0) {
        QString writeWhy;
        if (!writePendingList(instanceRoot, pending.worlds, &writeWhy)) {
            report.problem = writeWhy;
            return report;
        }
        report.pendingFile = instanceRoot + QLatin1Char('/') + dataPackPendingFileName();
    }

    report.ok = true;
    report.deployed = deployed;
    report.deferred = deferred;
    report.skipped = skipped;

    if (deployed > 0) {
        report.details.append(dataPackText("已把 %1 个数据包部署进对应的存档（datapacks/）。"
                                           "游戏里开没开由存档自己决定，可以在游戏内用 "
                                           "/datapack list 查看。")
                               .arg(deployed));
    }
    if (deferred > 0) {
        report.details.append(dataPackText("目标上没有这些存档，%1 个数据包记为「待应用」"
                                           "（%2/<存档名>/，清单是 %3）；这一趟没有新建世界。")
                               .arg(deferred)
                               .arg(dataPackPendingDirName(), dataPackPendingFileName()));
    }
    if (skipped > 0) {
        report.details.append(QCoreApplication::translate("Presets", "已保留目标中的 %1 个同名数据包。")
                                  .arg(skipped));
    }
    return report;
}

QStringList pendingDataPacks(const QString &instanceRoot, const QString &world) {
    if (instanceRoot.trimmed().isEmpty() || world.trimmed().isEmpty()) {
        return QStringList();
    }
    const PendingList pending = readPendingList(instanceRoot);
    if (!pending.ok) {
        return QStringList();
    }
    return pending.worlds.value(world);
}

}  // namespace Presets

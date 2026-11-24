/**
 * @file GenericFiles.cpp
 * @brief 实现 GenericFiles.hpp（扫这一组 / 算这一趟 / 落盘）。理由和取舍都写在那边文件头。
 *
 * 【这一层里唯一一处"写盘"长什么样】
 *
 * 一份文件一份文件地写，每份都走 `QSaveFile`（先写临时文件再原子换）—— 一份写到一半的配置
 * 比没有还糟：模组读它就是"这个配置文件坏了"，而不是"少了一条设置"。**目录先建再写**
 * （`QDir::mkpath`，同一份目录建两次不是错）。
 *
 * 【链接那一档为什么"整项失败"而不是"跳过那一份"】
 *
 * 源那一侧的一棵树里夹着一根链接时，如果只是把那一份略过，用户拿到的是一棵**缺了一块**的
 * 配置树 —— 而界面上报的是"复制成功"。整项失败至少让用户知道这一项没成，而且他知道该去
 * 哪儿看（那一项的名字在报告里）。
 */

#include "GenericFiles.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>

#include "download/ResourceCatalog.hpp"
#include "manage/InstanceFiles.hpp"
#include "presets/MalilibKeys.hpp"

namespace Presets {

namespace {

/// 这一层的提示语都在同一个上下文里（和 `VanillaOptions` / `ServerListPreset` 同一个名字）。
[[maybe_unused]] QString presetText(const char *source) { return QCoreApplication::translate("Presets", source); }

/// 一根链接（symbolic link / junction）。★ **两个都要判**：junction 的 `isSymLink()` 是
/// false（见 `InstanceFiles.hpp` 那段）。全仓四处都是这么写的，判法必须一致。
bool isLink(const QFileInfo &info) { return info.isJunction() || info.isSymLink(); }

/// 一份文件没拷过去时给用户看的那句话。
void failWith(QString *problemOut, const QString &why) {
    if (problemOut != nullptr) {
        *problemOut = why;
    }
}

/// 一棵子树多大（**不追链接**：链接那一头的东西不算，链接自己按 0 算）。只用于显示。
qint64 treeSize(const QString &absolutePath) {
    const QFileInfo info(absolutePath);
    if (isLink(info)) {
        return 0;
    }
    if (!info.isDir()) {
        return info.size();
    }
    qint64 total = 0;
    QDir dir(absolutePath);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                              QDir::System,
                          QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (isLink(entry)) {
            continue;
        }
        total += entry.isDir() ? treeSize(entry.absoluteFilePath()) : entry.size();
    }
    return total;
}

/// 一份文件 → 另一份（原子换）。**覆盖目标上已有的那一份**。
bool copyOneFile(const QString &source, const QString &target, QString *problemOut) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        failWith(problemOut, QCoreApplication::translate("Presets", "无法读取「%1」：%2").arg(source, in.errorString()));
        return false;
    }
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        failWith(problemOut, QCoreApplication::translate("Presets", "无法写入「%1」：%2").arg(target, out.errorString()));
        return false;
    }

    char buffer[64 * 1024];
    while (true) {
        const qint64 got = in.read(buffer, static_cast<qint64>(sizeof(buffer)));
        // 读到尾（0）和读出错（-1）是两件事 —— 并成一个出口的话，"读到一半失败"会被当成
        // "抄完了"，一份截断的配置照样原子地盖上去（`ServerListFile::commitBytes()` 同一条）。
        if (got < 0) {
            out.cancelWriting();
            failWith(problemOut, QCoreApplication::translate("Presets", "读取「%1」中断：%2")
                                     .arg(source, in.errorString()));
            return false;
        }
        if (got == 0) {
            break;
        }
        if (out.write(buffer, got) != got) {
            out.cancelWriting();
            failWith(problemOut, QCoreApplication::translate("Presets", "写入「%1」中断：%2")
                                     .arg(target, out.errorString()));
            return false;
        }
    }

    if (!out.commit()) {
        failWith(problemOut, QCoreApplication::translate("Presets", "替换「%1」失败：%2").arg(target, out.errorString()));
        return false;
    }
    return true;
}

/// 一项这一趟在盘上做了什么（按**文件**算）。
struct WalkCounts {
    int written = 0;
    int skipped = 0;
    QStringList skippedPaths;
};

/// 走这一项底下的每一份文件。见 .hpp 文件头（三道闸、链接拒掉、`dryRun` 那一档）。
bool walkItem(const QString &sourceRoot, const QString &targetRoot, FileGroup group,
              const GenericItem &item, bool overwrite, bool dryRun, WalkCounts *counts,
              QString *problemOut);

/// 走一个目录里的每一项（`walkItem()` 的递归那一半）。
bool walkDir(const QString &sourceDir, const QString &targetDir, const QString &sourceGroupDir,
             const QString &targetGroupDir, const QString &groupDirName, bool overwrite,
             bool dryRun, WalkCounts *counts, QString *problemOut) {
    // 目标这一层**自己**就是一根链接 ⇒ 拒。这一道非有不可：调用方那一手 `mkpath()`
    // 对着一根已经存在的 junction 回 true（它"已经有了"），接着往下递归就是**写到链接
    // 那头去** —— 而名字那两道闸看这条路径完全正常（`config/子目录/深.txt`）。挡住它的
    // 只可能是"目标这一层是不是链接"这一条。
    if (isLink(QFileInfo(targetDir))) {
        failWith(problemOut, QCoreApplication::translate("Presets", "目标「%1」是链接，已跳过。").arg(targetDir));
        return false;
    }
    QDir dir(sourceDir);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                              QDir::System,
                          QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString sourceFile = entry.absoluteFilePath();
        const QString targetFile = targetDir + QLatin1Char('/') + entry.fileName();

        if (isLink(entry)) {
            // 源那一侧夹着一根链接 ⇒ **整项失败**（见文件头那段）。
            failWith(problemOut, QCoreApplication::translate("Presets", "「%1」中包含链接（%2），未跟随。")
                                     .arg(groupDirName, entry.fileName()));
            return false;
        }
        // 第 3 道闸（第 1、2 道在扫和读段那一趟判过了，但那两道判的都是**字面**路径）：
        // 真正挡住"链接逃逸"的是这两个 —— `pathIsSameOrInside()` 自己会解 junction。
        if (!InstanceFiles::pathIsSameOrInside(sourceGroupDir, sourceFile)) {
            failWith(problemOut, QCoreApplication::translate("Presets", "「%1」指向允许范围以外的位置，已跳过。")
                                     .arg(sourceFile));
            return false;
        }
        // 目标那一侧**先看它是不是一根链接**，再看它在不在这一组目录底下。两件事都得判，
        // 但链接那条先说：一句话「往一根链接里写」是把问题说清楚了，而「在这一组目录
        // 外面」只说了现象（链接的 `canonicalFilePath()` 解不开，所以挡住它的其实是下面
        // 第二道——两句话说的是同一件事，第一句能让人直接去 `mklink` 那儿看）。
        // 同一道闸挡的还有**目录**：目标上某一层是链接时，接着递归就会写到链接那头去。
        const QFileInfo targetInfo(targetFile);
        if (isLink(targetInfo)) {
            failWith(problemOut, QCoreApplication::translate("Presets", "目标「%1」是链接，已跳过。")
                                     .arg(targetFile));
            return false;
        }
        if (!InstanceFiles::pathIsSameOrInside(targetGroupDir, targetFile)) {
            failWith(problemOut, QCoreApplication::translate("Presets", "目标路径「%1」超出允许范围，已跳过。")
                                     .arg(targetFile));
            return false;
        }

        if (entry.isDir()) {
            if (!dryRun && !QDir().mkpath(targetFile)) {
                failWith(problemOut,
                         QCoreApplication::translate("Presets", "无法创建目录「%1」，复制已停止。").arg(targetFile));
                return false;
            }
            if (!walkDir(sourceFile, targetFile, sourceGroupDir, targetGroupDir, groupDirName,
                         overwrite, dryRun, counts, problemOut)) {
                return false;
            }
            continue;
        }

        if (!overwrite && targetInfo.exists()) {
            ++counts->skipped;
            // 记的是**相对实例根**的那条路径（`config/foo/bar.json`）—— 报告的读者拿它去
            // 界面上找那一行，所以不能是相对这一组目录的（`foo/bar.json` 找不到）。
            counts->skippedPaths.append(targetFile.mid(targetGroupDir.size() + 1));
            continue;
        }

        ++counts->written;
        if (dryRun) {
            continue;
        }
        if (!QDir().mkpath(targetDir)) {
            failWith(problemOut, QCoreApplication::translate("Presets", "无法创建目录「%1」，复制已停止。").arg(targetDir));
            return false;
        }
        if (!copyOneFile(sourceFile, targetFile, problemOut)) {
            return false;
        }
    }
    return true;
}

bool walkItem(const QString &sourceRoot, const QString &targetRoot, FileGroup group,
              const GenericItem &item, bool overwrite, bool dryRun, WalkCounts *counts,
              QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const QString groupDirName = fileGroupDirName(group);
    const QString sourceGroupDir = genericJoined(sourceRoot, groupDirName);
    const QString targetGroupDir = genericJoined(targetRoot, groupDirName);

    // 组根自己是一根链接 ⇒ 不往下走。`walkDir()` 那道 `isLink(targetDir)`
    // 判的是**这一次要落的那个目录**，而组根在它上面一层：`<实例>/config` 是 junction 时，
    // `walkDir()` 拿到的是 `<实例>/config/某某`（孩子本身不是链接），于是整条路穿过去。
    // 源那一侧同理：`scanFileGroup()` 现在也会挡，但这里再判一次是因为
    // `applyFileGroup(…, items)` 那一档的清单可以来自预设段（那一趟没有扫盘）。
    if (isLink(QFileInfo(sourceGroupDir))) {
        failWith(problemOut,
                 QCoreApplication::translate("Presets", "「%1」为链接（符号链接 / junction），未跟随。")
                     .arg(sourceGroupDir));
        return false;
    }
    if (isLink(QFileInfo(targetGroupDir))) {
        failWith(problemOut,
                 QCoreApplication::translate("Presets", "目标「%1」为链接，未写入。").arg(targetGroupDir));
        return false;
    }

    const QString sourceAbs = genericJoined(sourceRoot, item.relativePath);
    const QFileInfo sourceInfo(sourceAbs);
    if (!sourceInfo.exists()) {
        failWith(problemOut,
                 QCoreApplication::translate("Presets", "「%1」不存在（预设中记录有此文件）。").arg(item.relativePath));
        return false;
    }
    if (isLink(sourceInfo)) {
        failWith(problemOut,
                 QCoreApplication::translate("Presets", "「%1」为链接，未跟随。").arg(item.relativePath));
        return false;
    }

    if (sourceInfo.isDir()) {
        return walkDir(sourceAbs, genericJoined(targetRoot, item.relativePath), sourceGroupDir,
                       targetGroupDir, groupDirName, overwrite, dryRun, counts, problemOut);
    }

    const QString targetAbs = genericJoined(targetRoot, item.relativePath);
    // 次序同 `walkDir()` 那一段（链接先、逃逸后）—— 理由也在那儿。
    const QFileInfo targetInfo(targetAbs);
    if (isLink(targetInfo)) {
        failWith(problemOut, QCoreApplication::translate("Presets", "目标「%1」为链接，未写入。")
                                 .arg(targetAbs));
        return false;
    }
    if (!InstanceFiles::pathIsSameOrInside(targetGroupDir, targetAbs)) {
        failWith(problemOut, QCoreApplication::translate("Presets", "目标路径「%1」超出允许范围，已跳过。")
                                 .arg(targetAbs));
        return false;
    }
    if (!overwrite && targetInfo.exists()) {
        ++counts->skipped;
        counts->skippedPaths.append(item.relativePath);
        return true;
    }

    ++counts->written;
    if (dryRun) {
        return true;
    }
    if (!QDir().mkpath(QFileInfo(targetAbs).absolutePath())) {
        failWith(problemOut, QCoreApplication::translate("Presets", "无法创建上级目录「%1」，复制已停止。").arg(targetAbs));
        return false;
    }
    return copyOneFile(sourceAbs, targetAbs, problemOut);
}

void countAction(const GenericPlanItem &row, int *replaced, int *copied, int *skipped,
                 int *needsChoice) {
    switch (row.action) {
    case GenericAction::Replace:
        ++(*replaced);
        break;
    case GenericAction::CopyIfMissing:
        ++(*copied);
        break;
    case GenericAction::Skip:
        ++(*skipped);
        break;
    case GenericAction::NeedsChoice:
        ++(*needsChoice);
        break;
    }
}

/// 「整组不认」的那一份计划：**把已经攒下的那些数一起清干净**。
///
/// 这一手不是洁癖。路径那两道路径判据是在**逐项**的那一趟里判的，所以坏项恰好在清单
/// 尾巴上时，前面那几项已经攒进 `replaced` 了 —— 而读者（报告、界面）看的是
/// `writesAnything()`：不清的话它会说"这一趟要写 4 项"，而其实一个字节都不会写。
/// `ok = false` 的计划里**清单和四个计数都必须是空的**，那是一句能兑现的承诺。
GenericPlan refusePlan(GenericPlan plan, const QString &why) {
    plan.ok = false;
    plan.problem = why;
    plan.items.clear();
    plan.replaced = 0;
    plan.copied = 0;
    plan.skipped = 0;
    plan.needsChoice = 0;
    return plan;
}

}  // namespace

// ---------------------------------------------------------------------------
// 四个落点
// ---------------------------------------------------------------------------

QString fileGroupDirName(FileGroup group) {
    switch (group) {
    case FileGroup::Config:
        return QStringLiteral("config");
    case FileGroup::DefaultConfigs:
        return QStringLiteral("defaultconfigs");
    case FileGroup::ResourcePacks:
        // 不写死：这一串还得和"下载页把资源包下到哪儿"一致，而那件事的唯一出处是
        // `ResourceCatalog` 那张表（见 .hpp 里那段）。
        return ResourceCatalog::folderForKind(ResourceCatalog::Kind::ResourcePack);
    case FileGroup::ShaderPacks:
        return ResourceCatalog::folderForKind(ResourceCatalog::Kind::Shader);
    }
    return QString();
}

bool fileGroupFromDirName(const QString &dirName, FileGroup *out) {
    const QList<FileGroup> groups = allFileGroups();
    for (FileGroup group : groups) {
        if (fileGroupDirName(group) == dirName) {
            if (out != nullptr) {
                *out = group;
            }
            return true;
        }
    }
    return false;
}

QList<FileGroup> allFileGroups() {
    return {FileGroup::Config, FileGroup::DefaultConfigs, FileGroup::ResourcePacks,
            FileGroup::ShaderPacks};
}

bool fileGroupOfPath(const QString &relativePath, FileGroup *out) {
    const int cut = relativePath.indexOf(QLatin1Char('/'));
    if (cut <= 0) {
        // 没有第一层目录（`a.json`）或者第一段是空的（`/a.json`）⇒ 不在这四组里。
        return false;
    }
    return fileGroupFromDirName(relativePath.left(cut), out);
}

// ---------------------------------------------------------------------------
// 名字与路径
// ---------------------------------------------------------------------------

bool genericPathIsSafe(const QString &relativePath, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (relativePath.isEmpty()) {
        failWith(problemOut, QCoreApplication::translate("Presets", "此项路径为空。"));
        return false;
    }
    if (QDir::isAbsolutePath(relativePath) || relativePath.startsWith(QLatin1Char('\\')) ||
        relativePath.startsWith(QLatin1Char('/')) || relativePath.contains(QLatin1Char(':'))) {
        // `C:/x`、`/x`、`\\server\share` 都是"绝对的"，`:` 还得单判一道（`C:x` 在
        // `QDir::isAbsolutePath()` 那儿不算绝对的，但它照样能跑到别处去）。
        failWith(problemOut, QCoreApplication::translate("Presets", "「%1」不是相对路径。").arg(relativePath));
        return false;
    }

    const QStringList segments = relativePath.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        // 每一段都过一遍实例名那套规矩（Windows 的禁区、`.` / `..`、保留设备名、
        // 结尾的点或空格）—— 那一条**只有一处实现**，正是 `validatePathSegment()`。
        const QString why = InstanceFiles::validatePathSegment(segment);
        if (!why.isEmpty()) {
            failWith(problemOut,
                     QCoreApplication::translate("Presets", "「%1」中的「%2」名称无效：%3")
                         .arg(relativePath, segment, why));
            return false;
        }
    }
    return true;
}

QString genericJoined(const QString &root, const QString &relativePath) {
    QString base = root;
    while (base.endsWith(QLatin1Char('/')) || base.endsWith(QLatin1Char('\\'))) {
        base.chop(1);
    }
    // **不做 `QDir::cleanPath()`**：这一层的每一道闸判的都是**字面**路径，清理一遍之后
    // `config/../../x` 就变成 `x` 了 ——"名字里有没有 `..`"这件事再也看不出来。
    return base + QLatin1Char('/') + relativePath;
}

bool genericPathIsLink(const QString &absolutePath) {
    const QFileInfo info(absolutePath);
    if (!info.exists()) {
        return false;  // "没有"不是"链接"
    }
    return isLink(info);
}

// ---------------------------------------------------------------------------
// 扫这一组
// ---------------------------------------------------------------------------

bool genericConfigLooksStructured(const QString &absolutePath) {
    if (absolutePath.trimmed().isEmpty()) {
        return false;
    }
    const QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    // 大文件**不看**：`config/` 底下该有的都是几 KB 的东西，几十 MB 的 JSON 只可能是
    // 别的什么（而"读它一遍"这件事本身就不该在一份**计划**里发生）。
    constexpr qint64 kMaxSniffBytes = 8 * 1024 * 1024;
    if (info.size() > kMaxSniffBytes) {
        return false;
    }
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = file.read(kMaxSniffBytes);
    file.close();
    return malilibLooksLikeConfig(bytes);
}

bool genericItemIsCopyOnly(FileGroup group, const QString &relativePath,
                           const QString &absolutePath) {
    // 这个记号的**唯一出处**（"未知配置明确标记"与"按结构认"两处规矩）。两截判法写在
    // 头文件那一段里，这儿只说为什么是这个次序：**名字那两关先拦掉绝大多数**（不是
    // `config/` 底下的、不是 `.json` 的连盘都不用开），剩下那几份才真去读一遍内容。
    if (group != FileGroup::Config) {
        return true;
    }
    if (!relativePath.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        return true;
    }
    if (absolutePath.isEmpty()) {
        return true;  // 没有盘上的位置 ⇒ 只能按名字算（见头文件那一条）
    }
    // 到这儿 = `config/` 底下的一份 `.json`。它是不是 MaLiLib 那一族，问**那一层**
    // （判据只有一处，这边不自己认结构）。依赖是**单向**的：通用文件组知道 MaLiLib
    // 那一族的存在，反过来那边一个字都不知道通用文件组。
    return !genericConfigLooksStructured(absolutePath);
}

bool scanFileGroup(const QString &root, FileGroup group, QList<GenericItem> *out,
                   QStringList *refusedOut, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (root.trimmed().isEmpty()) {
        failWith(problemOut, QCoreApplication::translate("Presets", "未指定要扫描的目录。"));
        return false;
    }
    const QString groupDirName = fileGroupDirName(group);
    const QString groupDir = genericJoined(root, groupDirName);
    const QFileInfo groupInfo(groupDir);
    // 这一组**自己**是一根链接 ⇒ 整组不扫。这一道挡的是"组根被挂到别处"：
    // 逐项那道 `isLink(entry)` 只看组里的**条目**，组根本身是 junction 时它一条都不挡 ——
    // 扫（和后面的写）全都会穿到链接那一头去。用户把 `config` / `resourcepacks` 挂到
    // 共享盘是常见做法，而这一层的承诺是"只动这个实例"（见 .hpp 文件头）。
    // 先判链接再判存在：一根**悬空**的 junction `exists()` 是 false，按"这一组不在"
    // 放过去的话，后面 `mkpath()` 会顺着它建到链接那一头。
    if (isLink(groupInfo)) {
        failWith(problemOut,
                 QCoreApplication::translate("Presets", "「%1」为链接（符号链接 / junction），未扫描。").arg(groupDir));
        return false;
    }
    if (!groupInfo.exists()) {
        // 这一组不在 ⇒ 空清单（"这一组是空的"不是失败：一个刚建的实例没有 `shaderpacks/`
        // 是最正常的事）。
        return true;
    }
    if (!groupInfo.isDir()) {
        failWith(problemOut,
                 QCoreApplication::translate("Presets", "「%1」所在位置不是目录，未扫描。").arg(groupDir));
        return false;
    }

    QDir dir(groupDir);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                              QDir::System,
                          QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString relative = groupDirName + QLatin1Char('/') + entry.fileName();
        QString why;
        if (!genericPathIsSafe(relative, &why)) {
            if (refusedOut != nullptr) {
                refusedOut->append(relative + QStringLiteral("：") + why);
            }
            continue;
        }
        if (isLink(entry)) {
            if (refusedOut != nullptr) {
                refusedOut->append(relative +
                                   QCoreApplication::translate("Presets", "：为链接（符号链接 / junction），未跟随。"));
            }
            continue;
        }
        GenericItem item;
        item.relativePath = relative;
        item.directory = entry.isDir();
        item.copyOnly = genericItemIsCopyOnly(group, relative, entry.absoluteFilePath());
        item.sizeBytes = item.directory ? treeSize(entry.absoluteFilePath()) : entry.size();
        if (out != nullptr) {
            out->append(item);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 计划
// ---------------------------------------------------------------------------

QString genericActionName(GenericAction action) {
    switch (action) {
    case GenericAction::Replace:
        return QCoreApplication::translate("Presets", "覆盖");
    case GenericAction::CopyIfMissing:
        return QCoreApplication::translate("Presets", "仅在缺失时复制");
    case GenericAction::Skip:
        return QCoreApplication::translate("Presets", "跳过");
    case GenericAction::NeedsChoice:
        return QCoreApplication::translate("Presets", "需要选择处理方式");
    }
    return QString();
}

GenericPlan planFileGroup(const QString &sourceRoot, const QList<GenericItem> &items,
                          const QString &targetRoot, ApplyMode mode) {
    GenericPlan plan;
    for (const GenericItem &item : items) {
        QString why;
        if (!genericPathIsSafe(item.relativePath, &why)) {
            // 一份自己带着非法路径的预设：**整组不认**（不是"跳过那一项"）。
            return refusePlan(plan,
                              QCoreApplication::translate("Presets", "此部分包含无效路径（%1），未应用任何项目。")
                                  .arg(item.relativePath + QStringLiteral("：") + why));
        }

        GenericPlanItem row;
        row.relativePath = item.relativePath;
        switch (mode) {
        case ApplyMode::Replace:
            row.action = GenericAction::Replace;
            row.reason = QCoreApplication::translate("Presets", "以预设文件替换同名文件，保留目标中的其他文件。");
            break;
        case ApplyMode::CopyIfMissing: {
            FileGroup group = FileGroup::Config;
            if (!fileGroupOfPath(item.relativePath, &group)) {
                return refusePlan(
                    plan, QCoreApplication::translate("Presets", "「%1」不在允许的目录内，此部分未应用。")
                              .arg(item.relativePath));
            }
            WalkCounts counts;
            QString walkWhy;
            if (!walkItem(sourceRoot, targetRoot, group, item, false, true, &counts, &walkWhy)) {
                return refusePlan(plan, walkWhy);
            }
            if (counts.written == 0) {
                row.action = GenericAction::Skip;
                row.reason = QCoreApplication::translate("Presets", "目标中已有此预设。");
            } else {
                row.action = GenericAction::CopyIfMissing;
                row.reason = QCoreApplication::translate("Presets", "将目标缺失的项复制到目标中（已有项不变）。");
            }
            break;
        }
        case ApplyMode::Skip:
            row.action = GenericAction::Skip;
            row.reason = QCoreApplication::translate("Presets", "未修改此部分。");
            break;
        case ApplyMode::Merge:
        case ApplyMode::Append: {
            row.action = GenericAction::NeedsChoice;
            // 「我们认不认得出它的结构」**这儿再算一遍**，而且这次带上**源那一侧的盘上
            // 位置**：`GenericItem::copyOnly` 那一格可能是从段载荷读回来的（那儿只有相对
            // 路径、名字那一截说了算），而 MaLiLib 那一族是**按结构**认的。
            // 两截判法合起来只有一处（`genericItemIsCopyOnly()`），这儿只是**喂它一个位置**。
            FileGroup groupOfItem = FileGroup::Config;
            const bool copyOnly =
                item.copyOnly &&
                (!fileGroupOfPath(item.relativePath, &groupOfItem) ||
                 genericItemIsCopyOnly(groupOfItem, item.relativePath,
                                       genericJoined(sourceRoot, item.relativePath)));
            row.reason = copyOnly
                             ? QCoreApplication::translate("Presets", "无法解析此项结构，仅支持复制或覆盖。")
                             : presetText("这一份是 MaLiLib 那一族的配置（结构我们认得）—— "
                                          "要搬的是它里面的热键，那是「模组热键」那一段的活；"
                                          "通用文件组这一层只做整份的复制和覆盖。");
            break;
        }
        }
        countAction(row, &plan.replaced, &plan.copied, &plan.skipped, &plan.needsChoice);
        plan.items.append(row);
    }
    plan.ok = true;
    return plan;
}

// ---------------------------------------------------------------------------
// 落盘
// ---------------------------------------------------------------------------

/// 落盘的公共那一半：清单**已经定好**（扫出来的，或者预设段里读回来的）。
GenericReport applyFileGroupItems(const QString &sourceRoot, const QString &targetRoot,
                                  FileGroup group, const QList<GenericItem> &items, ApplyMode mode,
                                  const QStringList &refused) {
    GenericReport report;
    report.refused = refused;
    if (sourceRoot.trimmed().isEmpty() || targetRoot.trimmed().isEmpty()) {
        report.problem = QCoreApplication::translate("Presets", "未指定目标实例。");
        return report;
    }

    QString why;
    const GenericPlan plan = planFileGroup(sourceRoot, items, targetRoot, mode);
    if (!plan.ok) {
        report.problem = plan.problem;
        return report;
    }

    // `plan.items` 和 `items` **一一对应、同一个次序**（那是上面那趟扫出来的顺序，
    // `planFileGroup()` 一项一项地收）—— 所以按下标取，不按名字再找一遍。
    for (int i = 0; i < items.size(); ++i) {
        const GenericPlanItem &row = plan.items.at(i);
        if (row.action == GenericAction::Skip || row.action == GenericAction::NeedsChoice) {
            continue;
        }
        WalkCounts counts;
        if (!walkItem(sourceRoot, targetRoot, group, items.at(i),
                      row.action == GenericAction::Replace, false, &counts, &why)) {
            report.problem = why;
            // 已经写下去的那些**留在盘上**（见 .hpp 文件头末尾）—— `ok = false` 就是要
            // 让调用方知道"这一趟没走完"，而不是"什么都没做"。
            return report;
        }
        report.filesWritten += counts.written;
        report.skippedPaths += counts.skippedPaths;
    }

    report.replaced = plan.replaced;
    report.copied = plan.copied;
    report.skipped = plan.skipped;
    report.needsChoice = plan.needsChoice;
    report.ok = true;
    return report;
}

GenericReport applyFileGroup(const QString &sourceRoot, const QString &targetRoot, FileGroup group,
                             ApplyMode mode) {
    QStringList refused;
    QList<GenericItem> items;
    QString why;
    if (!sourceRoot.trimmed().isEmpty() && !targetRoot.trimmed().isEmpty() &&
        !scanFileGroup(sourceRoot, group, &items, &refused, &why)) {
        GenericReport report;
        report.refused = refused;
        report.problem = why;
        return report;
    }
    return applyFileGroupItems(sourceRoot, targetRoot, group, items, mode, refused);
}

GenericReport applyFileGroup(const QString &sourceRoot, const QString &targetRoot, FileGroup group,
                             ApplyMode mode, const QList<GenericItem> &items) {
    // 清单就是清单：预设段里列了哪几项，这一趟就只动哪几项。不能不管调用方手里的清单、
    // 直接重扫预设的 `files/<组>` —— 那份目录里可能躺着**清单之外**的文件（提取那一档
    // "覆盖同名、多出来的不删"，重收之后旧的仍在），于是计划/备份按清单算、真正落盘的
    // 按盘上算，两边对不上（多落的那几份还不在备份里）。
    return applyFileGroupItems(sourceRoot, targetRoot, group, items, mode, QStringList());
}

GenericReport extractFileGroup(const QString &sourceRoot, const QString &presetFilesRoot,
                               FileGroup group) {
    GenericReport report;
    if (sourceRoot.trimmed().isEmpty() || presetFilesRoot.trimmed().isEmpty()) {
        report.problem = QCoreApplication::translate("Presets", "未指定此组的收集目标。");
        return report;
    }

    QList<GenericItem> items;
    QString why;
    if (!scanFileGroup(sourceRoot, group, &items, &report.refused, &why)) {
        report.problem = why;
        return report;
    }

    for (const GenericItem &item : items) {
        WalkCounts counts;
        // 提取那一档**一律覆盖**（收的就是"现在这一份"，同名的那份换成它）。
        if (!walkItem(sourceRoot, presetFilesRoot, group, item, true, false, &counts, &why)) {
            report.problem = why;
            return report;
        }
        report.filesWritten += counts.written;
        ++report.replaced;
    }
    report.ok = true;
    return report;
}

// ---------------------------------------------------------------------------
// 预设段 ⇄ 这一组
// ---------------------------------------------------------------------------

bool sectionsFromFileGroup(FileGroup group, const QList<GenericItem> &items, ApplyMode mode,
                           QList<PresetSection> *out, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }

    PresetSection section;
    section.id = sectionIdFor(SectionKind::GenericFiles, fileGroupDirName(group));
    section.kind = SectionKind::GenericFiles;
    section.mode = mode;

    for (const GenericItem &item : items) {
        FileGroup belongsTo = FileGroup::Config;
        if (!genericPathIsSafe(item.relativePath, problemOut) ||
            !fileGroupOfPath(item.relativePath, &belongsTo) || belongsTo != group) {
            if (problemOut != nullptr && problemOut->isEmpty()) {
                *problemOut = QCoreApplication::translate("Presets", "「%1」不属于此组（%2）。")
                                  .arg(item.relativePath, fileGroupDirName(group));
            }
            return false;
        }
        QJsonObject row;
        row.insert(QStringLiteral("path"), item.relativePath);
        // `copyOnly` **照样写下去**：它是"未知配置明确标记"在盘上的样子 ——
        // 换一个启动器读这份预设时，它自己能不能合并这一项，看的就是这一格。
        row.insert(QStringLiteral("copyOnly"), item.copyOnly);
        section.items.append(row);
    }

    if (out != nullptr) {
        out->append(section);
    }
    return true;
}

bool fileGroupFromSectionId(const QString &sectionId, FileGroup *out) {
    const QList<FileGroup> groups = allFileGroups();
    for (FileGroup group : groups) {
        if (sectionIdFor(SectionKind::GenericFiles, fileGroupDirName(group)) == sectionId) {
            if (out != nullptr) {
                *out = group;
            }
            return true;
        }
    }
    return false;
}

bool fileGroupFromSection(const PresetSection &section, QList<GenericItem> *out,
                          QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (!genericFilesHandlesSection(section.kind)) {
        failWith(problemOut, QCoreApplication::translate("Presets", "此段不属于通用文件层管理范围。"));
        return false;
    }

    QList<GenericItem> items;
    /// 这一段的各项是不是都落在同一组上（第一项说了算）。
    FileGroup seen = FileGroup::Config;
    bool haveSeen = false;

    for (int i = 0; i < section.items.size(); ++i) {
        const QJsonValue value = section.items.at(i);
        if (!value.isObject()) {
            failWith(problemOut, QCoreApplication::translate("Presets", "通用文件组的第 %1 项应为对象。")
                                     .arg(i + 1));
            return false;
        }
        const QJsonValue path = value.toObject().value(QStringLiteral("path"));
        if (!path.isString() || path.toString().isEmpty()) {
            failWith(problemOut, presetText("通用文件组那一段里第 %1 项缺了 path（或者它不是"
                                            "一个字符串）。")
                                     .arg(i + 1));
            return false;
        }
        const QString relative = path.toString();

        QString why;
        if (!genericPathIsSafe(relative, &why)) {
            failWith(problemOut, QCoreApplication::translate("Presets", "通用文件组的第 %1 项路径无效：%2")
                                     .arg(i + 1)
                                     .arg(why));
            return false;
        }
        FileGroup belongsTo = FileGroup::Config;
        if (!fileGroupOfPath(relative, &belongsTo)) {
            failWith(problemOut, presetText("通用文件组那一段里第 %1 项（%2）不属于这一组"
                                            "管的任何一个目录。")
                                     .arg(i + 1)
                                     .arg(relative));
            return false;
        }
        if (haveSeen && belongsTo != seen) {
            failWith(problemOut,
                     presetText("通用文件组那一段里第 %1 项落在%2底下，而前面那些落在%3底下 —— "
                                "一段只能是一组。")
                         .arg(i + 1)
                         .arg(fileGroupDirName(belongsTo))
                         .arg(fileGroupDirName(seen)));
            return false;
        }
        seen = belongsTo;
        haveSeen = true;

        GenericItem item;
        item.relativePath = relative;
        // `copyOnly` 认不出来时按**这一层的判据**算一遍（别信盘上那一格：它可能是手改的，
        // 也可能是一份旧预设留下的）。
        item.copyOnly = genericItemIsCopyOnly(belongsTo, relative);
        items.append(item);
    }

    if (out != nullptr) {
        *out = items;
    }
    return true;
}

bool genericFilesHandlesSection(SectionKind kind) {
    return kind == SectionKind::GenericFiles;
}

}  // namespace Presets

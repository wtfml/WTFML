/**
 * @file TransferApply.cpp
 * @brief TransferApply.hpp 的实现：按计划搬文件，只增不删、不跟链接。
 */

#include "operations/TransferApply.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryFile>
#include <QUuid>
#include <QCryptographicHash>
#include <QElapsedTimer>

#include "manage/InstanceFiles.hpp"
#include "tools/FileHash.hpp"
#include "util/WriteDirectoryGuard.hpp"

namespace TransferApply {

ModTransferResult transferMods(const QString &sourceRoot, const QString &targetRoot,
                              const QStringList &paths, bool move, const ModProgress &progress,
                              const std::function<bool()> &cancelled) {
    ModTransferResult result;
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(sourceRoot, &lockProblem)
        || !WriteDirectoryGuard::acquire(targetRoot, &lockProblem)) {
        result.failed = qMax(1, int(paths.size()));
        result.problems.append(lockProblem);
        return result;
    }
    QElapsedTimer elapsed; elapsed.start();
    const auto stopped = [&] { return (cancelled && cancelled()) || elapsed.elapsed() >= 600000; };
    const auto ordinaryDir = [](const QString &path) {
        const QFileInfo info(path);
        return info.isDir() && !info.isSymLink() && !info.isJunction();
    };
    const QString sourceMods = QDir(sourceRoot).filePath(QStringLiteral("mods"));
    const QString targetMods = QDir(targetRoot).filePath(QStringLiteral("mods"));
    if (!ordinaryDir(sourceRoot) || !ordinaryDir(targetRoot) || !ordinaryDir(sourceMods) ||
        InstanceFiles::pathIsSameOrInside(sourceRoot, targetRoot) ||
        InstanceFiles::pathIsSameOrInside(targetRoot, sourceRoot) ||
        (QFileInfo::exists(targetMods) && !ordinaryDir(targetMods)) ||
        !QDir().mkpath(targetMods) || !ordinaryDir(targetMods)) {
        result.failed = paths.size();
        result.problems.append(QCoreApplication::translate("TransferApply", "源或目标目录不可用、包含链接，或互相嵌套。"));
        return result;
    }
    int completed = 0;
    for (const auto &path : paths) {
        if (stopped()) { result.cancelled = true; break; }
        const QFileInfo source(path);
        const auto report = [&](const QString &phase) {
            if (progress) progress(completed, paths.size(), phase + QCoreApplication::translate("TransferApply", "：") + source.fileName());
        };
        const auto fail = [&](const QString &reason) {
            ++result.failed;
            if (result.problems.size() < 8) result.problems.append(source.fileName() + QCoreApplication::translate("TransferApply", "：") + reason);
        };
        if (!source.isFile() || source.isSymLink() || source.isJunction() ||
            source.absolutePath() != QDir(sourceMods).absolutePath()) {
            fail(QCoreApplication::translate("TransferApply", "不是源实例 mods 中的普通文件")); ++completed; continue;
        }
        const QString target = QDir(targetMods).filePath(source.fileName());
        const QString suffix = QStringLiteral(".disabled");
        const QString counterpart = target.endsWith(suffix, Qt::CaseInsensitive)
            ? target.left(target.size() - suffix.size()) : target + suffix;
        if (QFileInfo::exists(target) || QFileInfo(target).isSymLink() || QFileInfo::exists(counterpart)) {
            ++result.skipped; ++completed; report(QCoreApplication::translate("TransferApply", "跳过同名文件")); continue;
        }
        report(QCoreApplication::translate("TransferApply", "复制"));
        const auto stamp = FileHash::stampOf(path);
        QFile input(path);
        QTemporaryFile staging(QDir(targetMods).filePath(QStringLiteral(".wtfml-transfer-XXXXXX")));
        if (!input.open(QIODevice::ReadOnly) || !staging.open()) {
            fail(QCoreApplication::translate("TransferApply", "无法打开文件或创建临时文件")); ++completed; continue;
        }
        QCryptographicHash copiedHash(QCryptographicHash::Sha256);
        bool writeOk = true;
        qint64 reportedAt = elapsed.elapsed();
        while (!input.atEnd() && !stopped()) {
            const QByteArray chunk = input.read(256 * 1024);
            if (chunk.isEmpty() || staging.write(chunk) != chunk.size()) { writeOk = false; break; }
            copiedHash.addData(chunk);
            if (elapsed.elapsed() - reportedAt >= 100) { report(QCoreApplication::translate("TransferApply", "复制")); reportedAt = elapsed.elapsed(); }
        }
        input.close();
        if (stopped()) { result.cancelled = true; break; }
        if (!writeOk || !staging.flush()) { fail(QCoreApplication::translate("TransferApply", "读取或写入失败")); ++completed; continue; }
        staging.close();
        report(QCoreApplication::translate("TransferApply", "校验"));
        const auto sourceHash = FileHash::digestOfFile(path, FileHash::Algorithm::Sha256, {}, stopped);
        const auto targetHash = FileHash::digestOfFile(staging.fileName(), FileHash::Algorithm::Sha256, {}, stopped);
        if (stopped()) { result.cancelled = true; break; }
        const QString copiedHex = QString::fromLatin1(copiedHash.result().toHex());
        if (!sourceHash.ok || !targetHash.ok || sourceHash.hex != copiedHex || targetHash.hex != copiedHex ||
            !FileHash::sameStamp(stamp, FileHash::stampOf(path))) {
            fail(QCoreApplication::translate("TransferApply", "校验失败或来源在处理期间发生变化")); ++completed; continue;
        }
        // QFile::rename never replaces an existing destination. Recheck directory
        // links before publishing; staging remains auto-removable on all failures.
        if (!ordinaryDir(sourceRoot) || !ordinaryDir(targetRoot) || !ordinaryDir(sourceMods) ||
            !ordinaryDir(targetMods) || QFileInfo::exists(counterpart) ||
            !staging.rename(target)) {
            fail(QCoreApplication::translate("TransferApply", "目标已变化或无法提交文件（未覆盖）")); ++completed; continue;
        }
        staging.setAutoRemove(false);
        ++result.copied;
        // 发布成功就算这一条"复制完成"，进度要跟着走：
        // 之前这里先判停再 `++completed`，于是"刚发布完就收到取消"那一
        // 瞬间账上 `copied` 已经是 1、进度却还停在上一条（界面少记一个）。
        // 移动删源是另一件事（没删掉时来源原样保留），它的成败不影响这条文件已复制。
        ++completed;
        report(QCoreApplication::translate("TransferApply", "已复制"));
        if (move) {
            if (stopped()) { result.cancelled = true; break; }
            if (FileHash::sameStamp(stamp, FileHash::stampOf(path)) && QFile::remove(path)) ++result.moved;
            else fail(QCoreApplication::translate("TransferApply", "副本已验证，但未能删除来源；已保留两份"));
        }
        report(QCoreApplication::translate("TransferApply", "完成"));
    }
    if (elapsed.elapsed() >= 600000) result.problems.append(QCoreApplication::translate("TransferApply", "已达到 600 秒截止时间。"));
    return result;
}

namespace {

constexpr int kFailureListLimit = 5;

void noteFailure(Result *result, const QString &path) {
    ++result->failedFiles;
    if (result->failures.size() < kFailureListLimit) result->failures << path;
}

/// 一个相对路径得是"普通的相对路径"：不带分隔符换向、不带 `..`、不是绝对路径。
/// 表是这一层写死的常量，但这条路将来会接用户选的东西（例如自定义目录），所以闸在这儿。
bool safeRelativePath(const QString &relativePath) {
    if (relativePath.isEmpty()) return false;
    if (QDir::isAbsolutePath(relativePath)) return false;
    const QString unified = QString(relativePath).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList segments = unified.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        if (segment == QStringLiteral("..") || segment == QStringLiteral(".")) return false;
    }
    return !segments.isEmpty();
}

/// 拷一个文件；目标已存在时**跳过**（不覆盖）。
void copyOneFile(const QString &sourceFile, const QString &targetFile, Result *result) {
    const QFileInfo info(sourceFile);
    if (info.isJunction() || info.isSymLink()) {
        noteFailure(result, sourceFile);  // 链接不跟
        return;
    }
    // 目标那一格自己是一根链接 ⇒ 也不写。上面的"目标已存在就跳过"只挡
    // 得住**指得通**的那种；一根悬空的链接 `exists()` 是 false，接着 `QFile::copy()`
    // 会顺着它写到链接那一头去。承诺是"只动目标实例里的文件"（见 .hpp 文件头 ②）。
    const QFileInfo targetInfo(targetFile);
    if (targetInfo.isJunction() || targetInfo.isSymLink()) {
        noteFailure(result, targetFile);
        return;
    }
    if (targetInfo.exists()) {
        ++result->skippedFiles;
        return;
    }
    if (!QDir().mkpath(QFileInfo(targetFile).absolutePath())) {
        noteFailure(result, targetFile);
        return;
    }
    // 先写**同目录**的临时名，再改名发布：`QFile::copy()` 写到一半失败（磁盘满、
    // 杀软扫描、移动盘被拔）会留下半截目标文件，而"目标已存在就跳过"下一次会把这
    // 半截当成已经搬好的一份，从此再也不重试。同卷改名是原子的；失败时临时文件删掉。
    // 名字带 UUID：`QFile::copy()` 遇到已存在的目标会直接失败，不会覆盖别人的东西。
    const QString stagingPath = QFileInfo(targetFile).absoluteDir().filePath(
        QStringLiteral(".wtfml-part-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QFile::copy(sourceFile, stagingPath)) {
        QFile::remove(stagingPath);
        noteFailure(result, sourceFile);
        return;
    }
    // 复制期间目标可能被别人建出来了（另一个启动器、同步盘）：仍然按"不覆盖"处理。
    if (QFileInfo::exists(targetFile) || !QFile::rename(stagingPath, targetFile)) {
        QFile::remove(stagingPath);
        if (QFileInfo::exists(targetFile)) {
            ++result->skippedFiles;
            return;
        }
        noteFailure(result, targetFile);
        return;
    }
    ++result->copiedFiles;
    result->copiedBytes += info.size();
}

/// 把一棵目录**合并**进目标：只拷目标里没有的文件，目录先建出来。
void mergeTree(const QString &sourceDir, const QString &targetDir, Result *result) {
    // 这一层（目标）自己是一根链接 ⇒ 不进去。递归到下一层时子目录那道
    // `sub.isJunction()` 挡的是**源**那一侧；目标那一侧没有对应的闸 ——
    // `<目标>/config/共享` 是 junction 时，`mkpath()` 对着一根已经存在的链接回 true，
    // 接着就写到链接那头去了。
    const QFileInfo sourceInfo(sourceDir);
    if (sourceInfo.isJunction() || sourceInfo.isSymLink()) {
        noteFailure(result, sourceDir);
        return;
    }
    const QFileInfo targetInfo(targetDir);
    if (targetInfo.isJunction() || targetInfo.isSymLink()) {
        noteFailure(result, targetDir);
        return;
    }
    if (!QDir().mkpath(targetDir)) {
        noteFailure(result, targetDir);
        return;
    }
    const QDir source(sourceDir);
    const QFileInfoList files =
        source.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDir::NoSort);
    for (const QFileInfo &file : files) {
        copyOneFile(file.absoluteFilePath(),
                    targetDir + QLatin1Char('/') + file.fileName(), result);
    }
    const QFileInfoList subs =
        source.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::NoSort);
    for (const QFileInfo &sub : subs) {
        if (sub.isJunction() || sub.isSymLink()) {
            noteFailure(result, sub.absoluteFilePath());
            continue;
        }
        mergeTree(sub.absoluteFilePath(), targetDir + QLatin1Char('/') + sub.fileName(), result);
    }
}

// ---------- 校验（最后一步） ----------

constexpr int kVerifyListLimit = 5;

/// 对**一个文件**：源里有的，目标里在不在、大小一不一样。
/// 源里的链接不跟 —— `apply()` 本来也不拷它们，所以它们不算"该在目标里"。
void checkOneFile(const QString &sourceFile, const QString &targetFile, VerifySummary *summary) {
    const QFileInfo info(sourceFile);
    if (info.isJunction() || info.isSymLink()) return;
    ++summary->checkedFiles;
    const QFileInfo targetInfo(targetFile);
    if (!targetInfo.exists() || !targetInfo.isFile()) {
        ++summary->missingFiles;
        if (summary->missing.size() < kVerifyListLimit) summary->missing << targetFile;
        return;
    }
    if (targetInfo.size() != info.size()) {
        ++summary->sizeMismatchFiles;
        if (summary->mismatched.size() < kVerifyListLimit) summary->mismatched << targetFile;
    }
}

void checkTree(const QString &sourceDir, const QString &targetDir, VerifySummary *summary) {
    const QDir source(sourceDir);
    const QFileInfoList files =
        source.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDir::NoSort);
    for (const QFileInfo &file : files) {
        checkOneFile(file.absoluteFilePath(),
                     targetDir + QLatin1Char('/') + file.fileName(), summary);
    }
    const QFileInfoList subs =
        source.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::NoSort);
    for (const QFileInfo &sub : subs) {
        if (sub.isJunction() || sub.isSymLink()) continue;
        checkTree(sub.absoluteFilePath(), targetDir + QLatin1Char('/') + sub.fileName(), summary);
    }
}

}  // namespace

QString Result::text() const {
    if (!ok) return problem;
    QStringList parts;
    parts << QCoreApplication::translate("TransferApply", "复制了 %1 个文件（%2）")
                 .arg(copiedFiles)
                 .arg(FileHash::fileSizeText(copiedBytes));
    if (skippedFiles > 0) {
        parts << QCoreApplication::translate("TransferApply", "跳过 %1 个（目标中已有同名文件，未覆盖）").arg(skippedFiles);
    }
    if (failedFiles > 0) {
        parts << QCoreApplication::translate("TransferApply", "失败 %1 个").arg(failedFiles);
    }
    QString text = parts.join(QCoreApplication::translate("TransferApply", "，")) + QCoreApplication::translate("TransferApply", "。");
    if (!failures.isEmpty()) {
        text += QCoreApplication::translate("TransferApply", "\n\n未迁移的文件：");
        for (const QString &failure : failures) {
            text += QStringLiteral("\n• %1").arg(QDir::toNativeSeparators(failure));
        }
        if (failedFiles > failures.size()) {
            text += QCoreApplication::translate("TransferApply", "\n• …另有 %1 个").arg(failedFiles - failures.size());
        }
    }
    return text;
}

Result apply(const QString &sourceDir, const QString &targetDir,
             const QList<TransferPlanner::Item> &items, const QStringList &keys,
             const ProgressCallback &progress) {
    Result result;

    const QString source = sourceDir.trimmed();
    const QString target = targetDir.trimmed();
    if (source.isEmpty() || target.isEmpty()) {
        result.problem = QCoreApplication::translate("TransferApply", "未提供完整的源实例和目标实例目录。");
        return result;
    }
    if (!QDir(source).exists()) {
        result.problem = QCoreApplication::translate("TransferApply", "未找到源实例目录：%1")
                             .arg(QDir::toNativeSeparators(source));
        return result;
    }
    if (!QDir(target).exists()) {
        result.problem = QCoreApplication::translate("TransferApply", "未找到目标实例目录：%1")
                             .arg(QDir::toNativeSeparators(target));
        return result;
    }
    if (!WriteDirectoryGuard::acquire(source, &result.problem)
        || !WriteDirectoryGuard::acquire(target, &result.problem)) return result;
    // ② 源和目标不能是同一份、也不能互相嵌套。判据和「复制实例」那道闸是同一个函数
    // （它解 junction / 8.3 短名 —— 光比字面路径挡不住"拿一个指着源实例的 junction
    // 当目标"）。双向各问一次：往源里面搬 = 自己抄自己，往源外面搬进它的祖先也一样危险。
    if (InstanceFiles::pathIsSameOrInside(source, target)
        || InstanceFiles::pathIsSameOrInside(target, source)) {
        result.problem = QCoreApplication::translate("TransferApply", "源实例与目标实例相同或互相嵌套，本次未迁移任何文件。");
        return result;
    }

    for (const TransferPlanner::Item &item : items) {
        if (!keys.contains(item.key)) continue;
        if (!item.exists) continue;
        if (!safeRelativePath(item.relativePath)) {
            noteFailure(&result, item.relativePath);
            continue;
        }
        const QString sourcePath = source + QLatin1Char('/') + item.relativePath;
        const QString targetPath = target + QLatin1Char('/') + item.relativePath;
        // 这一项**路过的每一层**都得是普通目录。选中的那一项自己是
        // junction / symlink 时，`item.isDirectory && isDir()` 对它回 true（`isDir()`
        // 跟着链接走），下面整棵就搬到链接那一头去了 —— `mergeTree()` 里那道
        // `sub.isJunction()` 只判**子树里的子目录**，判不到项根。
        // 两侧都判：源是链接 ⇒ 从别处读；目标是链接 ⇒ 写到别处去。
        const bool sourceLinked =
            InstanceFiles::pathHasLinkComponent(source, item.relativePath);
        const bool targetLinked =
            InstanceFiles::pathHasLinkComponent(target, item.relativePath);
        if (sourceLinked || targetLinked) {
            noteFailure(&result, sourceLinked ? sourcePath : targetPath);
            if (progress) progress(item.key, false);
            continue;
        }
        const int failedBefore = result.failedFiles;
        if (item.isDirectory && QFileInfo(sourcePath).isDir()) {
            mergeTree(sourcePath, targetPath, &result);
        } else {
            copyOneFile(sourcePath, targetPath, &result);
        }
        // 逐项报告：一块搬完就报一次，**跳过不算失败**。
        if (progress) progress(item.key, result.failedFiles == failedBefore);
    }

    result.ok = true;
    return result;
}

QString VerifySummary::text() const {
    if (!ok) return problem;
    QString head = QCoreApplication::translate("TransferApply", "校验：已检查 %1 个文件，缺少 %2 个").arg(checkedFiles).arg(missingFiles);
    if (sizeMismatchFiles > 0) {
        head += QCoreApplication::translate("TransferApply", "；另有 %1 个目标中原有的同名文件大小不同（按规则未覆盖）")
                    .arg(sizeMismatchFiles);
    }
    head += QCoreApplication::translate("TransferApply", "。");
    if (!missing.isEmpty()) {
        head += QCoreApplication::translate("TransferApply", "\n\n目标中未找到的文件：");
        for (const QString &path : missing) {
            head += QStringLiteral("\n• %1").arg(QDir::toNativeSeparators(path));
        }
        if (missingFiles > missing.size()) {
            head += QCoreApplication::translate("TransferApply", "\n• …另有 %1 个").arg(missingFiles - missing.size());
        }
    }
    if (!mismatched.isEmpty()) {
        head += QCoreApplication::translate("TransferApply", "\n\n大小不同（目标原有）：");
        for (const QString &path : mismatched) {
            head += QStringLiteral("\n• %1").arg(QDir::toNativeSeparators(path));
        }
        if (sizeMismatchFiles > mismatched.size()) {
            head += QCoreApplication::translate("TransferApply", "\n• …另有 %1 个").arg(sizeMismatchFiles - mismatched.size());
        }
    }
    return head;
}

VerifySummary verify(const QString &sourceDir, const QString &targetDir,
                     const QList<TransferPlanner::Item> &items, const QStringList &keys) {
    VerifySummary summary;

    const QString source = sourceDir.trimmed();
    const QString target = targetDir.trimmed();
    if (source.isEmpty() || target.isEmpty()) {
        summary.problem = QCoreApplication::translate("TransferApply", "未提供完整的源实例和目标实例目录。");
        return summary;
    }
    if (!QDir(source).exists()) {
        summary.problem = QCoreApplication::translate("TransferApply", "未找到源实例目录：%1")
                              .arg(QDir::toNativeSeparators(source));
        return summary;
    }
    if (!QDir(target).exists()) {
        summary.problem = QCoreApplication::translate("TransferApply", "未找到目标实例目录：%1")
                              .arg(QDir::toNativeSeparators(target));
        return summary;
    }

    for (const TransferPlanner::Item &item : items) {
        if (!keys.contains(item.key)) continue;
        if (!item.exists) continue;
        if (!safeRelativePath(item.relativePath)) continue;   // apply() 里已经记过这一条失败
        // apply() 对"这一项路过的某一层是链接"的那几项是**当场失败、一个字节都不搬**的
        // （见上面那一段）—— 它们不算"该在目标里"，这里也就不该报"缺了"。
        if (InstanceFiles::pathHasLinkComponent(source, item.relativePath) ||
            InstanceFiles::pathHasLinkComponent(target, item.relativePath)) {
            continue;
        }
        const QString sourcePath = source + QLatin1Char('/') + item.relativePath;
        const QString targetPath = target + QLatin1Char('/') + item.relativePath;
        if (item.isDirectory && QFileInfo(sourcePath).isDir()) {
            checkTree(sourcePath, targetPath, &summary);
        } else if (QFileInfo(sourcePath).isFile()) {
            checkOneFile(sourcePath, targetPath, &summary);
        }
    }

    summary.ok = true;
    return summary;
}

}  // namespace TransferApply

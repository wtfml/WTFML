#include "InstanceUpgrade.hpp"
#include <QCoreApplication>
#include "UpgradeHistory.hpp"
#include "UpgradeOptions.hpp"

#include "manage/BackupFiles.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/InstanceMetaFile.hpp"
#include "manage/FolderManager.hpp"
#include "util/ZipUtil.hpp"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLockFile>
#include "util/DirectorySessionLock.hpp"
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QLibrary>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <restartmanager.h>
#include <QThread>
#include <QElapsedTimer>
#endif
#include <limits>
#include <vector>

namespace InstanceUpgrade {
namespace {
QString originalDir(const Request &r) { return QDir(r.gameDir).filePath("versions/" + r.instanceId); }
QString journalPath(const Request &r) { return transactionDir(r) + "/journal.json"; }
#ifdef Q_OS_WIN
// Failure-only diagnostics. Never stop another application or treat an owner
// lookup failure as permission to publish. Bound both traversal and result size.
QString directoryLockOwners(const QString &source) {
    QLibrary library(QStringLiteral("rstrtmgr"));
    if (!library.load()) return {};
    const auto start = reinterpret_cast<decltype(&RmStartSession)>(library.resolve("RmStartSession"));
    const auto add = reinterpret_cast<decltype(&RmRegisterResources)>(library.resolve("RmRegisterResources"));
    const auto list = reinterpret_cast<decltype(&RmGetList)>(library.resolve("RmGetList"));
    const auto end = reinterpret_cast<decltype(&RmEndSession)>(library.resolve("RmEndSession"));
    if (!start || !add || !list || !end) return {};
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1]{};
    if (start(&session, 0, key) != ERROR_SUCCESS) return {};
    struct SessionGuard {
        DWORD session;
        decltype(end) close;
        ~SessionGuard() { close(session); }
    } guard{session, end};
    std::vector<std::wstring> paths;
    // Do not recursively scan worlds or caches merely to explain a refusal.
    // Unlisted/deep owners may remain unknown; the original error still stands.
    for (const auto &root : {source, source + QStringLiteral("/mods")}) {
        QDirIterator entries(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks);
        while (paths.size() < 128 && entries.hasNext())
            paths.push_back(QDir::toNativeSeparators(entries.next()).toStdWString());
        if (paths.size() >= 128) break;
    }
    if (paths.empty()) return {};
    std::vector<LPCWSTR> names;
    for (const auto &path : paths) names.push_back(path.c_str());
    if (add(session, DWORD(names.size()), names.data(), 0, nullptr, 0, nullptr) != ERROR_SUCCESS) return {};
    UINT needed = 0, count = 32;
    DWORD reason = 0;
    RM_PROCESS_INFO owners[32]{};
    if (list(session, &needed, &count, owners, &reason) != ERROR_SUCCESS) return {};
    QStringList labels;
    for (UINT i = 0; i < count; ++i) {
        labels.append(QCoreApplication::translate("InstanceUpgrade", "%1（PID %2）")
            .arg(QString::fromWCharArray(owners[i].strAppName))
            .arg(owners[i].Process.dwProcessId));
    }
    labels.removeDuplicates();
    return labels.join(QCoreApplication::translate("InstanceUpgrade", "、"));
}
#endif
bool retainDirectory(const QString &source, const QString &target, QString *problem) {
#ifdef Q_OS_WIN
    const auto nativePath = [](const QString &path) {
        const QString full = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        if (full.startsWith(QStringLiteral("\\\\?\\"))) return full.toStdWString();
        return (full.startsWith(QStringLiteral("\\\\"))
            ? QStringLiteral("\\\\?\\UNC\\") + full.mid(2)
            : QStringLiteral("\\\\?\\") + full).toStdWString();
    };
    const auto from = nativePath(source), to = nativePath(target);
    QElapsedTimer clock; clock.start();
    DWORD error = ERROR_SUCCESS;
    do {
        // No replacement flag: a pre-existing recovery directory is never overwritten.
        if (MoveFileExW(from.c_str(), to.c_str(), 0)) return true;
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION &&
            error != ERROR_ACCESS_DENIED) break;
        if (clock.elapsed() >= 1000) break;
        // Retry a brief Windows refusal without pumping GUI events. The upgrade
        // worker must still report persistent denial and leave the source intact.
        QThread::msleep(50);
    } while (true);
    *problem = QCoreApplication::translate("InstanceUpgrade", "无法保留原目录，未替换实例（Windows 错误 %1）。").arg(error);
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        const QString owners = directoryLockOwners(source);
        if (!owners.isEmpty())
            *problem += QCoreApplication::translate("InstanceUpgrade", " 请先关闭占用实例文件的程序：%1，然后重试。原实例和完整快照均保留。").arg(owners);
    }
    return false;
#else
    if (QDir().rename(source, target)) return true;
    *problem = QCoreApplication::translate("InstanceUpgrade", "无法保留原目录，未替换实例。");
    return false;
#endif
}
bool redirected(const QString &path) {
    const QFileInfo f(path);
    return f.isSymLink() || f.isJunction();
}
QString validate(const Request &r) {
    if (!QDir::isAbsolutePath(r.gameDir) || !QFileInfo(r.gameDir).isDir()
        || !InstanceFiles::validateDirName(r.instanceId).isEmpty()
        || !QRegularExpression("^[A-Za-z0-9-]{1,80}$").match(r.transactionId).hasMatch())
        return QCoreApplication::translate("InstanceUpgrade", "升级事务的游戏目录、实例名或事务编号无效。");
    const QStringList paths{r.gameDir, QDir(r.gameDir).filePath("versions"),
        QDir(r.gameDir).filePath("cache"), QDir(r.gameDir).filePath("cache/instance-upgrades"),
        QDir(r.gameDir).filePath("cache/backups"), QDir(r.gameDir).filePath("cache/backups/instances"),
        transactionDir(r), transactionDir(r) + "/prepared", transactionDir(r) + "/prepared/versions",
        originalDir(r), candidateDir(r), transactionDir(r) + "/original",
        transactionDir(r) + "/replaced", journalPath(r)};
    for (const auto &path : paths)
        if (redirected(path)) return QCoreApplication::translate("InstanceUpgrade", "升级路径包含链接，无法保证完整快照和安全回滚：%1").arg(path);
    // 副本模式的"只复制这几条"必须是单层相对名：不能让 `../` 或绝对路径从调用方漏进来。
    for (const QString &root : r.copyRoots) {
        if (root.isEmpty() || root.contains(QLatin1Char('/')) || root.contains(QLatin1Char('\\'))
            || root == QStringLiteral(".") || root == QStringLiteral("..")
            || QDir::isAbsolutePath(root)) {
            return QCoreApplication::translate("InstanceUpgrade", "要复制的类别路径不合法：%1").arg(root);
        }
    }
    return {};
}
// Full snapshots must not silently omit junctions, symlinks or unreadable trees.
QString checkTree(const QString &root) {
    QList<QString> stack{root};
    while (!stack.isEmpty()) {
        const QString path = stack.takeLast();
        const QFileInfo info(path);
        if (redirected(path) || !info.isReadable())
            return QCoreApplication::translate("InstanceUpgrade", "目录含链接或不可读内容，未执行原地升级：%1").arg(path);
        if (info.isDir()) {
            const auto entries = QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden
                | QDir::System | QDir::NoDotAndDotDot, QDir::NoSort);
            for (const auto &entry : entries) stack.append(entry.absoluteFilePath());
        } else if (!info.isFile()) {
            return QCoreApplication::translate("InstanceUpgrade", "不是普通文件，无法制作完整快照：%1").arg(path);
        }
    }
    return {};
}
/// 游戏根相对它所在卷的路径（`D:/Games/MC` → `Games/MC`）。
///
/// 这不是判据，只是给日志/人看的提示：整个游戏目录被改名或搬走之后它会变。真正用来
/// 认出“这份事务还是不是同一个游戏根”的是 `rootFingerprint`。
QString rootRelativePath(const QString &gameDir) {
    const QFileInfo info(QDir::cleanPath(gameDir));
    const QString path = info.absoluteFilePath();
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':')) {
        return QDir(path.left(2) + QStringLiteral("/")).relativeFilePath(path);
    }
    return info.fileName();
}

/// 根指纹：只由**事务自己携带的材料**算出来，所以整个游戏目录被改名/搬到别的盘之后
/// 仍然能验证。
///
/// 为什么不用绝对路径：那正是改名/迁移会变的東西。为什么不用卷序列号：换盘就失效，
/// 而用户的“迁移”经常就是换盘。事务编号 + 实例名 + 快照文件名 + 原始文件指纹都在
/// 事务目录里，跟着目录走；同目录里如果没有这几样，rollback 也做不成。
QString rootFingerprintFor(const QString &instanceId, const QString &transactionId,
                           const QString &snapshotName, const QString &originalFingerprint) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("wtfml-upgrade-root/1\n"));
    hash.addData(instanceId.toUtf8());
    hash.addData(QByteArrayLiteral("\n"));
    hash.addData(transactionId.toUtf8());
    hash.addData(QByteArrayLiteral("\n"));
    hash.addData(QFileInfo(snapshotName).fileName().toUtf8());
    hash.addData(QByteArrayLiteral("\n"));
    hash.addData(originalFingerprint.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

QJsonObject readJournal(const Request &r) {
    QFile f(journalPath(r));
    if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024) return {};
    const auto json = QJsonDocument::fromJson(f.readAll()).object();
    if (json.value("schema").toInt() != 1
        || json.value("instanceId").toString() != r.instanceId
        || json.value("transactionId").toString() != r.transactionId) {
        return {};
    }
    if (QDir::cleanPath(json.value("gameDir").toString()) == QDir::cleanPath(r.gameDir)) {
        return json;
    }
    // 游戏目录被改名/搬走：按事务自带的根指纹验证。没有指纹的旧事务（或指纹不符）
    // 一律不认 —— 宁可让用户用完整 ZIP 快照恢复，也不在错误目录上做回滚。
    const QString stored = json.value("rootFingerprint").toString();
    if (stored.isEmpty()) return {};
    const QString expected = rootFingerprintFor(
        json.value("instanceId").toString(), json.value("transactionId").toString(),
        json.value("snapshotZip").toString(), json.value("originalFingerprint").toString());
    if (stored != expected) return {};
    return json;
}

bool writeJournal(const Request &r, const QString &state, const QString &snapshot) {
    // 先读旧记录：回滚那几笔写进 journal 时 Request 里没有原始指纹，不能因此把
    // 根指纹改掉 —— 改掉之后下一次“改名后恢复”就认不出自己了。
    QJsonObject previous;
    {
        QFile f(journalPath(r));
        if (f.open(QIODevice::ReadOnly) && f.size() <= 64 * 1024) {
            previous = QJsonDocument::fromJson(f.readAll()).object();
        }
    }
    const QString originalFingerprint = r.originalFingerprint.isEmpty()
        ? previous.value("originalFingerprint").toString()
        : QString::fromLatin1(r.originalFingerprint.toHex());
    // 根被搬走之后，旧 journal 里的快照是旧绝对路径。同名快照跟着根一起搬过来了的话，
    // 把它改写成新根下的路径；没搬过来就原样保留（只影响显示，不用于回滚）。
    QString snapshotPath = snapshot;
    if (!snapshotPath.isEmpty() && !QFileInfo::exists(snapshotPath)) {
        const QString rebased = QDir(r.gameDir).filePath(
            QStringLiteral("cache/backups/instances/") + QFileInfo(snapshotPath).fileName());
        if (QFileInfo::exists(rebased)) snapshotPath = rebased;
    }
    const QString snapshotName = QFileInfo(snapshotPath).fileName();
    QString rootFingerprint = previous.value("rootFingerprint").toString();
    if (rootFingerprint.isEmpty()) {
        rootFingerprint = rootFingerprintFor(r.instanceId, r.transactionId, snapshotName,
                                             originalFingerprint);
    }
    const QJsonObject json{{"schema", 1}, {"instanceId", r.instanceId},
        {"transactionId", r.transactionId}, {"gameDir", QDir::cleanPath(r.gameDir)},
        {"rootName", QFileInfo(QDir::cleanPath(r.gameDir)).fileName()},
        {"rootRelative", rootRelativePath(r.gameDir)},
        {"rootFingerprint", rootFingerprint},
        {"originalFingerprint", originalFingerprint},
        {"snapshotName", snapshotName},
        {"state", state}, {"snapshotZip", snapshotPath},
        {"updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    QSaveFile f(journalPath(r));
    const auto bytes = QJsonDocument(json).toJson();
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}
bool sameFiles(const QString &root, const QList<BackupFiles::FileRecord> &expected,
               const std::function<bool()> &cancelled = {}, QString *reason = nullptr) {
    QString problem;
    const auto actual = BackupFiles::collectFiles(root, {}, &problem, cancelled);
    if (!problem.isEmpty() || actual.size() != expected.size()) {
        if (reason) *reason = problem.isEmpty()
            ? QCoreApplication::translate("InstanceUpgrade", "副本文件数 %1，预期 %2").arg(actual.size()).arg(expected.size()) : problem;
        return false;
    }
    for (qsizetype i = 0; i < actual.size(); ++i)
        if (actual[i].path != expected[i].path || actual[i].size != expected[i].size
            || actual[i].sha1 != expected[i].sha1) {
            if (reason) *reason = QCoreApplication::translate("InstanceUpgrade", "副本不一致：%1（预期 %2；大小 %3/%4）")
                .arg(actual[i].path, expected[i].path).arg(actual[i].size).arg(expected[i].size);
            return false;
        }
    return true;
}
QByteArray fingerprint(const QList<BackupFiles::FileRecord> &files) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &file : files) {
        const auto bytes = QJsonDocument(QJsonObject{{"path", file.path},
            {"size", QString::number(file.size)}, {"sha1", file.sha1}}).toJson(QJsonDocument::Compact);
        hash.addData(bytes);
        hash.addData(QByteArray("\n"));
    }
    return hash.result();
}
bool saveObject(const QString &path, const QJsonObject &json) {
    QSaveFile f(path);
    const auto bytes = QJsonDocument(json).toJson();
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}
// A failed attempt can leave `prepared/` holding a full copy of the instance
// (saving everything, mods, screenshots). The copy has no recovery value until
// the original is retained: before that point nothing under `versions/` has
// changed, and a journal without `original/` is ignored by recovery. Keep the
// transaction directory only when it holds rollback material.
void discardUnpublishedStaging(const Request &r) {
    // Never resolve a relative/empty game dir against the process working
    // directory; only a request that passed validation names a real location.
    if (!validate(r).isEmpty()) return;
    const QString root = transactionDir(r);
    if (!QFileInfo(root).isDir() || redirected(root)) return;
    if (QFileInfo(root + "/original").isDir() || QFileInfo(root + "/replaced").isDir()) return;
    removeTreeWithoutFollowingLinks(root);
}
// Recovery first acquires the transaction lock. Copied files preserve source mtimes,
// so include the staging directory's own timestamp in the abandonment grace period.
bool stagingIsStale(const QString &dir) {
    QDateTime newest = QFileInfo(dir).lastModified();
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QDateTime at = it.fileInfo().lastModified();
        if (at.isValid() && (newest.isNull() || at > newest)) newest = at;
    }
    if (newest.isNull()) newest = QFileInfo(dir).lastModified();  // 一个文件都还没拷
    return newest.isValid()
           && newest.secsTo(QDateTime::currentDateTime()) > 30 * 60;
}
} // namespace

QString transactionDir(const Request &r) {
    return QDir(r.gameDir).filePath("cache/instance-upgrades/" + r.transactionId);
}
QString candidateDir(const Request &r) {
    return transactionDir(r) + "/prepared/versions/" + r.instanceId;
}

QString dependentInstanceProblem(const QString &gameDir, const QString &instanceId) {
    if (!QDir::isAbsolutePath(gameDir) || !InstanceFiles::validateDirName(instanceId).isEmpty())
        return QCoreApplication::translate("InstanceUpgrade", "无法安全检查实例引用关系。");
    const QDir versions(QDir(gameDir).filePath("versions"));
    const QString original = versions.filePath(instanceId);
    for (const auto &entry : versions.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (entry.fileName().compare(instanceId, Qt::CaseInsensitive) == 0) continue;
        if (redirected(entry.absoluteFilePath())
            && InstanceFiles::pathIsSameOrInside(original, entry.absoluteFilePath()))
            return QCoreApplication::translate("InstanceUpgrade", "实例 %1 通过目录链接使用此核心，请改用升级副本。").arg(entry.fileName());
        const QDir profile(entry.absoluteFilePath());
        for (const auto &name : profile.entryList({"*.json"}, QDir::Files | QDir::Hidden)) {
            QFile f(profile.filePath(name));
            if (!f.open(QIODevice::ReadOnly) || f.size() > 16 * 1024 * 1024)
                return QCoreApplication::translate("InstanceUpgrade", "无法核实 %1 的核心引用，未执行原地替换。").arg(entry.fileName());
            const auto document = QJsonDocument::fromJson(f.readAll());
            if (!document.isObject()) continue; // Non-profile JSON is not an inheritance edge.
            const auto json = document.object();
            for (const QString &field : {QStringLiteral("inheritsFrom"), QStringLiteral("jar")}) {
                if (json.value(field).toString().compare(instanceId, Qt::CaseInsensitive) == 0)
                    return QCoreApplication::translate("InstanceUpgrade", "实例 %1 的 %2 引用了此核心。请改用升级副本，避免影响其它实例。")
                        .arg(entry.fileName(), field);
            }
        }
    }
    return {};
}

/// 把新核心、元数据与逐文件模组决定落到**候选目录**上（原地升级与副本模式共用）。
///
/// 调用前候选目录必须已经是一份通过完整校验的用户文件副本：这里只改核心那几样
/// （旧 jar/json / natives / 模组后缀 / 新 jar+json+meta），失败时一律原实例未改变。
namespace {

/// 流式核对一个文件的 SHA-1 与大小（不把 jar 读进内存）。
bool verifyFileDigest(const QString &path, qint64 expectedSize, const QString &expectedSha1,
                      QString *problem) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.isJunction()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("InstanceUpgrade", "文件不存在或是链接：%1").arg(path);
        return false;
    }
    if (expectedSize >= 0 && info.size() != expectedSize) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("InstanceUpgrade", "文件大小不符：%1（期望 %2，实际 %3）")
                           .arg(QFileInfo(path).fileName()).arg(expectedSize).arg(info.size());
        }
        return false;
    }
    if (expectedSha1.trimmed().isEmpty()) {
        return true;  // 平台没给哈希：不假装校验过
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("InstanceUpgrade", "读不了：%1").arg(path);
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha1);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) break;
        hash.addData(chunk);
    }
    const QString actual = QString::fromLatin1(hash.result().toHex());
    if (actual.compare(expectedSha1.trimmed(), Qt::CaseInsensitive) != 0) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("InstanceUpgrade", "SHA-1 不符：%1（期望 %2，实际 %3）")
                           .arg(QFileInfo(path).fileName(), expectedSha1, actual);
        }
        return false;
    }
    return true;
}

/// 把一份平台适配版从暂存写进候选目录。目标已存在且内容一致时跳过（可重入）。
bool copyReplacementIntoCandidate(const ModReplacement &one, const QString &modsPath,
                                  QString *problem) {
    const QString target = QDir(modsPath).filePath(one.fileName);
    if (QFileInfo(target).exists()) {
        if (verifyFileDigest(target, one.size, one.sha1, nullptr)) {
            return true;
        }
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("InstanceUpgrade", "同名模组已存在且内容不同，未覆盖：%1").arg(one.fileName);
        }
        return false;
    }
    QFile input(one.stagedPath);
    QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("InstanceUpgrade", "无法写入适配模组：%1").arg(one.fileName);
        }
        return false;
    }
    while (!input.atEnd()) {
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
            output.cancelWriting();
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("InstanceUpgrade", "适配模组写入失败：%1").arg(one.fileName);
            }
            return false;
        }
    }
    if (!output.commit() || !verifyFileDigest(target, one.size, one.sha1, problem)) {
        if (problem != nullptr && problem->isEmpty()) {
            *problem = QCoreApplication::translate("InstanceUpgrade", "适配模组提交后校验失败：%1").arg(one.fileName);
        }
        return false;
    }
    return true;
}

/// 文件变更日志：跟着事务候选目录一起发布，所以失败时它也不会留下。
///
/// 格式是 `wtfml-update-changes/1`，只追加最近 20 趟。嵌套 JAR 不单独成项 —— 它们随
/// 父 JAR 一起替换，这里用一句 note 说清，避免让人误以为漏了。
bool writeUpdateChangeLog(const Request &r, const Core &core, const QString &candidate,
                          Result *result) {
    const QString path = candidate + QStringLiteral("/wtfml.update-changes.json");
    QJsonArray runs;
    QFile previous(path);
    if (!QFileInfo(path).isSymLink() && previous.open(QIODevice::ReadOnly)
        && previous.size() <= 4 * 1024 * 1024) {
        const auto object = QJsonDocument::fromJson(previous.readAll()).object();
        if (object.value(QStringLiteral("format")).toString()
            == QStringLiteral("wtfml-update-changes/1")) {
            runs = object.value(QStringLiteral("runs")).toArray();
        }
    }
    previous.close();
    QJsonArray changes;
    for (const ModReplacement &one : core.replacements) {
        changes.append(QJsonObject{
            {QStringLiteral("action"),
             one.replaces.isEmpty() ? QStringLiteral("add") : QStringLiteral("replace")},
            {QStringLiteral("file"), one.fileName},
            {QStringLiteral("replacedFile"), one.replaces},
            {QStringLiteral("project"), one.projectKey},
            {QStringLiteral("versionId"), one.versionId},
            {QStringLiteral("title"), one.title},
            {QStringLiteral("fromVersion"), one.fromVersion},
            {QStringLiteral("toVersion"), one.toVersion},
            {QStringLiteral("sha1"), one.sha1},
            {QStringLiteral("size"), QString::number(one.size)},
            {QStringLiteral("fromDependency"), one.fromDependency},
        });
    }
    runs.append(QJsonObject{
        {QStringLiteral("transactionId"), r.transactionId},
        {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("targetGameVersion"), core.gameVersion},
        {QStringLiteral("loaderName"), core.loaderName},
        {QStringLiteral("loaderVersion"), core.loaderVersion},
        {QStringLiteral("coreJar"), r.instanceId + QStringLiteral(".jar")},
        {QStringLiteral("coreJson"), r.instanceId + QStringLiteral(".json")},
        {QStringLiteral("disabledMods"), result->disabledMods},
        {QStringLiteral("changes"), changes},
        {QStringLiteral("nestedNote"),
         QCoreApplication::translate("InstanceUpgrade", "嵌套 JAR 随父 JAR 一起替换，不单独列出或替换。")},
    });
    while (runs.size() > 20) runs.removeFirst();
    const QByteArray bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("format"), QStringLiteral("wtfml-update-changes/1")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("runs"), runs},
    }).toJson(QJsonDocument::Indented);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "无法写入文件变更日志，本次更新未发布。");
        return false;
    }
    return true;
}

}  // namespace

bool applyCoreToCandidate(const Request &r, const Core &core, const QString &candidate,
                          Result *result) {
    // Read identity/settings from the copied original, never from the target.
    // 这里只**验证**它读得懂：升级要干一大摊事，读不懂的文件得现在就说，别拖到最后
    // 写不进去才发现。真正的合并写在这条路末尾（原来那份内容一个键都不会少）。
    const QString metaPath = candidate + "/wtfml.meta";
    InstanceMetaFile::Status metaStatus = InstanceMetaFile::Status::Missing;
    InstanceMetaFile::read(metaPath, &metaStatus);
    if (metaStatus == InstanceMetaFile::Status::Unreadable) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "无法读取原实例设置，未继续升级。"); return false;
    }
    if (metaStatus == InstanceMetaFile::Status::BadVersion) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "原实例设置的格式版本比这个启动器新，未继续升级。"); return false;
    }
    auto json = core.mergedJson;
    json.insert("id", r.instanceId);
    json.remove("inheritsFrom"); // Flattening prevents inheritance through the id being replaced.
    json.remove("jar");
    const QJsonObject metaFields{
        {"realVersion", core.gameVersion},
        {"loaderName", core.loaderName},
        {"loaderVersion", core.loaderVersion},
        {"upgradeTransactionId", r.transactionId},
    };
    QFile jar(core.clientJar);
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const QString expected = json.value("downloads").toObject().value("client").toObject().value("sha1").toString();
    if (!jar.open(QIODevice::ReadOnly) || !hash.addData(&jar)
        || (!expected.isEmpty() && QString::fromLatin1(hash.result().toHex()).compare(expected, Qt::CaseInsensitive) != 0)) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "目标客户端文件未通过校验，原实例未改变。"); return false;
    }
    jar.close();
    // Remove the previous core's jar/json pair before writing the new one.
    //
    // A directory that was renamed outside the launcher (or whose rename failed halfway)
    // still holds a jar/json named after the *old* name. Leaving them beside the new pair
    // is not cosmetic: the launcher adds any top-level jar that contains the main class to
    // the classpath, and the stale client jar of course does — so the old and new cores
    // end up on one classpath.
    //
    // Only these two names count as the previous core (other jars in a version directory,
    // e.g. `lwjgl3ify-forgePatches.jar`, are required at runtime and must not be touched):
    // · `<json id>.jar` (both vanilla and instances the launcher created), and
    // · `<json stem>.jar` (the leftover half of a rename).
    // NOTE: the json's own `jar` field is *not* used — in a version json it names another
    // *version id*, not a file in this directory, so deleting by it could hit an
    // unrelated jar.
    const QString oldJsonName = InstanceFiles::jsonName(r.instanceId);
    if (oldJsonName.contains('/') || oldJsonName.contains('\\') || oldJsonName.contains(':')) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "旧版本信息文件名不安全，原实例未改变。"); return false;
    }
    const QString newJarName = r.instanceId + QStringLiteral(".jar");
    const QString newJsonName = r.instanceId + QStringLiteral(".json");
    QJsonObject oldJson;
    {
        QFile oldJsonFile(candidate + '/' + oldJsonName);
        if (oldJsonFile.open(QIODevice::ReadOnly) && oldJsonFile.size() <= 16 * 1024 * 1024) {
            const auto document = QJsonDocument::fromJson(oldJsonFile.readAll());
            if (document.isObject()) oldJson = document.object();
        }
    }
    const QStringList staleJars{oldJson.value(QStringLiteral("id")).toString() + QStringLiteral(".jar"),
                                QFileInfo(oldJsonName).completeBaseName() + QStringLiteral(".jar")};
    for (const QString &name : staleJars) {
        if (name.contains('/') || name.contains('\\') || name.contains(':')
            || !InstanceFiles::validateDirName(name).isEmpty()) {
            result->problem = QCoreApplication::translate("InstanceUpgrade", "旧版本信息包含不安全的客户端文件名，原实例未改变。");
            return false;
        }
        if (name.isEmpty() || name.compare(newJarName, Qt::CaseInsensitive) == 0) continue;
        const QString stale = candidate + '/' + name;
        if (QFileInfo::exists(stale) && !QFile::remove(stale)) {
            result->problem = QCoreApplication::translate("InstanceUpgrade", "无法清理暂存副本中的旧客户端文件，原实例未改变。");
            return false;
        }
    }
    if (oldJsonName.compare(newJsonName, Qt::CaseInsensitive) != 0) {
        const QString stale = candidate + '/' + oldJsonName;
        if (QFileInfo::exists(stale) && !QFile::remove(stale)) {
            result->problem = QCoreApplication::translate("InstanceUpgrade", "无法清理暂存副本中的旧版本信息，原实例未改变。");
            return false;
        }
    }
    const QString client = candidate + '/' + r.instanceId + ".jar";
    if ((QFileInfo::exists(client) && !QFile::remove(client)) || !QFile::copy(core.clientJar, client)
        || !saveObject(candidate + '/' + r.instanceId + ".json", json)) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "无法准备新核心或设置，原实例未改变。"); return false;
    }
    QString metaProblem;
    if (!InstanceMetaFile::mergeWrite(metaPath, metaFields, &metaProblem)) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "无法写入原实例设置，原实例未改变：%1").arg(metaProblem);
        return false;
    }
    QFile copiedJar(client);
    QCryptographicHash copiedHash(QCryptographicHash::Sha1);
    if (!copiedJar.open(QIODevice::ReadOnly) || !copiedHash.addData(&copiedJar)
        || copiedHash.result() != hash.result()) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "目标客户端在复制期间发生变化，未发布新版本。"); return false;
    }
    copiedJar.close();
    const QString natives = candidate + "/natives";
    if (QFileInfo::exists(natives) && !removeTreeWithoutFollowingLinks(natives)) {
        result->problem = QCoreApplication::translate("InstanceUpgrade", "无法清理暂存副本中的旧原生库，原实例未改变。"); return false;
    }
    // ---------- 2.1.3 平台适配版替换 ----------
    // 全部落在**候选目录**里：任何一步失败都让整趟回滚，源实例/原目录不受影响。
    QSet<QString> replacementNames;
    if (!core.replacements.isEmpty()) {
        const QString modsPath = candidate + QStringLiteral("/mods");
        if (redirected(modsPath) || (QFileInfo::exists(modsPath) && !QFileInfo(modsPath).isDir())) {
            result->problem = QCoreApplication::translate("InstanceUpgrade", "候选模组目录不可用，本次更新未发布。");
            return false;
        }
        if (!QFileInfo::exists(modsPath) && !QDir().mkpath(modsPath)) {
            result->problem = QCoreApplication::translate("InstanceUpgrade", "无法创建候选模组目录，本次更新未发布。");
            return false;
        }
        for (const ModReplacement &one : core.replacements) {
            if (one.fileName.isEmpty() || one.fileName != QFileInfo(one.fileName).fileName()
                || one.fileName.contains(QLatin1Char('\\'))
                || one.fileName.contains(QLatin1Char(':'))
                || !one.fileName.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)
                || (!one.replaces.isEmpty()
                    && (one.replaces != QFileInfo(one.replaces).fileName()
                        || one.replaces.contains(QLatin1Char('\\'))
                        || one.replaces.contains(QLatin1Char(':'))))) {
                result->problem = QCoreApplication::translate("InstanceUpgrade", "适配模组文件名不安全，本次更新未发布：%1")
                                      .arg(one.fileName);
                return false;
            }
            QString problem;
            if (!verifyFileDigest(one.stagedPath, one.size, one.sha1, &problem)) {
                result->problem = QCoreApplication::translate("InstanceUpgrade", "适配模组未通过校验，整趟更新已回滚：%1")
                                      .arg(problem);
                return false;
            }
            if (!one.replaces.isEmpty()) {
                const QString oldPath = QDir(modsPath).filePath(one.replaces);
                if (redirected(oldPath)) {
                    result->problem = QCoreApplication::translate("InstanceUpgrade", "要替换的旧模组是链接，未动它：%1").arg(one.replaces);
                    return false;
                }
                if (QFileInfo::exists(oldPath) && !QFile::remove(oldPath)) {
                    result->problem = QCoreApplication::translate("InstanceUpgrade", "无法移除旧模组，本次更新未发布：%1")
                                          .arg(one.replaces);
                    return false;
                }
            }
            if (!copyReplacementIntoCandidate(one, modsPath, &problem)) {
                result->problem = QCoreApplication::translate("InstanceUpgrade", "适配模组写入失败，整趟更新已回滚：%1").arg(problem);
                return false;
            }
            replacementNames.insert(one.fileName);
            if (one.replaces.isEmpty()) {
                result->changes.append(QObject::tr("新增前置：%1（%2 %3）")
                                           .arg(one.fileName, one.title, one.toVersion));
            } else {
                result->changes.append(QObject::tr("替换：%1 → %2（%3 %4 → %5）")
                                           .arg(one.replaces, one.fileName, one.title,
                                                one.fromVersion, one.toVersion));
            }
        }
    }
    if (core.disableOldMods) {
        QDirIterator it(candidate + "/mods", QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (!path.endsWith(".jar", Qt::CaseInsensitive)) continue;
              const QString relative = QDir(candidate + "/mods").relativeFilePath(path);
              if (core.keepOldMods.contains(relative) || replacementNames.contains(relative)) continue;
            if (QFileInfo::exists(path + ".disabled") || !QFile::rename(path, path + ".disabled")) {
                result->problem = QCoreApplication::translate("InstanceUpgrade", "无法禁用尚未验证兼容性的旧模组，原实例未改变。"); return false;
            }
            ++result->disabledMods;
        }
    }
    if (result->disabledMods > 0) {
        result->changes.append(QObject::tr("禁用未验证兼容性的旧模组 %1 个（文件保留为 .disabled）")
                                   .arg(result->disabledMods));
    }
    if (!writeUpdateChangeLog(r, core, candidate, result)) return false;
    return true;
}

/// 提交/回滚收尾时写一条升级历史（目标实例一份；副本模式源实例也留一条，同一个事务号）。
///
/// 失败不改变升级结果：历史写不进去只在 `problem` 后面补一句，**不把成功报成失败**。
static void recordUpgradeHistory(const Request &r, const Core &core, const QString &sourceDir,
                          const QString &targetDir, const QString &startedAt,
                          [[maybe_unused]] const QString &snapshotZip, bool ok, bool cancelled,
                          const QString &problem, int copied, int skipped) {
    UpgradeHistory::Entry entry;
    entry.transactionId = r.transactionId;
    entry.sourceId = r.sourceInstanceId;
    entry.targetId = r.instanceId;
    entry.targetName = r.label;
    entry.startedAt = startedAt;
    entry.finishedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    entry.toGameVersion = core.gameVersion;
    entry.toLoader = core.loaderName.isEmpty()
                         ? QString()
                         : core.loaderName + QLatin1Char(' ') + core.loaderVersion;
    entry.categories = r.copyRoots;
    entry.ok = ok;
    entry.cancelled = cancelled;
    entry.problem = problem;
    entry.copiedFiles = copied;
    entry.skippedFiles = skipped;
    // 来源那几格从源实例自己的 wtfml.meta 里读（读不出就留空，不猜）。
    for (const QString &dir : {sourceDir, targetDir}) {
        QFile meta(QDir(dir).filePath(QStringLiteral("wtfml.meta")));
        if (!meta.open(QIODevice::ReadOnly) || meta.size() > 1024 * 1024) {
            continue;
        }
        const QJsonObject object = QJsonDocument::fromJson(meta.readAll()).object();
        if (object.isEmpty()) {
            continue;
        }
        if (entry.fromGameVersion.isEmpty()) {
            entry.fromGameVersion = object.value(QStringLiteral("realVersion")).toString();
        }
        if (entry.fromLoader.isEmpty()) {
            entry.fromLoader = object.value(QStringLiteral("loaderName")).toString();
        }
        if (!entry.sourceName.isEmpty()) {
            break;
        }
        entry.sourceName = object.value(QStringLiteral("name")).toString();
    }
    if (!r.copyRoots.isEmpty()) {
        entry.categories = r.copyRoots;
    }
    QString historyProblem;
    UpgradeHistory::append(targetDir, entry, &historyProblem);
    if (!sourceDir.isEmpty() && QDir::cleanPath(sourceDir) != QDir::cleanPath(targetDir)) {
        UpgradeHistory::append(sourceDir, entry, &historyProblem);
    }
}

static Result prepareAndCommitImpl(Request r, const Core &core, const std::function<void(Stage)> &progress,
                                   const std::function<bool()> &cancelled) {
    Result result;
    result.problem = validate(r);
    if (!result.problem.isEmpty()) return result;
    result.problem = dependentInstanceProblem(r.gameDir, r.instanceId);
    if (!result.problem.isEmpty()) return result;
    if (core.mergedJson.value("mainClass").toString().isEmpty() || core.gameVersion.isEmpty()
        || !QDir::isAbsolutePath(core.clientJar) || !QFileInfo(core.clientJar).isFile()
        || redirected(core.clientJar) || QFileInfo::exists(candidateDir(r))) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "目标核心、客户端文件或暂存路径无效，未执行原地升级。"); return result;
    }
    if (progress) progress(Stage::Checking);
    const QString original = originalDir(r), candidate = candidateDir(r);
    result.problem = checkTree(original);
    if (!result.problem.isEmpty()) return result;
    QString problem;
    const auto files = BackupFiles::collectFiles(original, {}, &problem, cancelled);
    if (cancelled && cancelled()) {
        result.cancelled = true;
        result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消准备。");
        return result;
    }
    if (!problem.isEmpty() || files.isEmpty()) {
        result.problem = problem.isEmpty() ? QCoreApplication::translate("InstanceUpgrade", "原实例是空的。") : problem; return result;
    }
    r.originalFingerprint = fingerprint(files);
    result.copiedFiles = files.size();
    qint64 size = 0;
    for (const auto &file : files) {
        if (file.size < 0 || size > std::numeric_limits<qint64>::max() - file.size) {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "实例大小溢出，未开始准备。"); return result;
        }
        size += file.size;
    }
    const qint64 available = QStorageInfo(r.gameDir).bytesAvailable();
    if (available < 0 || size > (std::numeric_limits<qint64>::max() - 32 * 1024 * 1024) / 3
        || available < size * 3 + 32 * 1024 * 1024
        || (r.spaceBudgetBytes >= 0 && r.spaceBudgetBytes < size * 3 + 32 * 1024 * 1024)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "空间不足以准备副本和完整快照，原实例未改变。"); return result;
    }
    if (cancelled && cancelled()) { result.cancelled = true; result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消准备。"); return result; }
    QString copyProblem;
    if (!copyTreeWithoutFollowingLinks(original, candidate, cancelled, &copyProblem)) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消准备。");
        } else {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "无法完整复制用户文件，原实例未改变。\n%1").arg(copyProblem);
        }
        return result;
    }
    if (cancelled && cancelled()) { result.cancelled = true; result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消准备。"); return result; }
    if (!sameFiles(candidate, files, cancelled, &copyProblem)) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消准备。");
        } else {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "用户文件副本未通过完整校验，原实例未改变。\n%1").arg(copyProblem);
        }
        return result;
    }
    if (!applyCoreToCandidate(r, core, candidate, &result)) return result;
    const int disabled = result.disabledMods;
    const QStringList appliedChanges = result.changes;
    result = commitPrepared(r, progress, cancelled);
    result.disabledMods = disabled;
    result.changes = appliedChanges;
    return result;
}

Result prepareAndCommit(Request r, const Core &core, const std::function<void(Stage)> &progress,
                        const std::function<bool()> &cancelled) {
    const QString invalid = validate(r);
    if (!invalid.isEmpty()) { Result result; result.problem = invalid; return result; }
    QString lockProblem;
    if (!DirectorySessionLock::acquire(r.gameDir, &lockProblem)) {
        Result result; result.problem = lockProblem; return result;
    }
    const QString parent = QFileInfo(transactionDir(r)).absolutePath();
    QDir().mkpath(parent);
    QLockFile lock(transactionDir(r) + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        Result result; result.problem = QCoreApplication::translate("InstanceUpgrade", "升级事务正在使用，未修改实例。"); return result;
    }
    const QString startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const auto result = prepareAndCommitImpl(r, core, progress, cancelled);
    if (!result.ok) discardUnpublishedStaging(r);
    recordUpgradeHistory(r, core, originalDir(r), originalDir(r), startedAt,
                         result.snapshotZip, result.ok,
                         result.cancelled, result.problem, result.copiedFiles,
                         result.skippedFiles);
    return result;
}

namespace {
/// 副本模式：源实例只读，候选目录是唯一会被写的东西。
///
/// 和原地升级的取舍（见头文件）：**不做 ZIP 快照**。理由是源实例一个字节都不动 ——
/// 它就是回滚材料；再为副本打一份完整 ZIP 只是把同一份内容第五次写进磁盘。
/// 反过来，校验必须一样严：复制后、发布前、发布后各对一遍路径/大小/SHA-1。
static Result prepareCopyAndCommitImpl(Request r, const Core &core, const QString &sourceDir,
                                       const std::function<void(Stage)> &progress,
                                       const std::function<bool()> &cancelled) {
    Result result;
    const auto stopped = [&] { return cancelled && cancelled(); };
    const auto cancelResult = [&](const QString &note) {
        result.cancelled = true;
        result.problem = note;
        return result;
    };
    result.problem = validate(r);
    if (!result.problem.isEmpty()) return result;
    // 源在同一个游戏目录里、必须是目录、不能是链接；目标名字不能已经存在。
    const QString original = originalDir(r), candidate = candidateDir(r);
    if (QDir::cleanPath(sourceDir) == QDir::cleanPath(original)
        || !QFileInfo(sourceDir).isDir() || redirected(sourceDir)
        || InstanceFiles::pathIsSameOrInside(original, sourceDir)
        || InstanceFiles::pathIsSameOrInside(candidate, sourceDir)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "副本的源实例或目标目录不安全，未复制任何文件。");
        return result;
    }
    // 第一版不允许"跨游戏根"复制：源实例必须在这次事务的游戏目录里。
    // 跨根要同时处理两个根的锁、空间、路径与历史归属，不能只改一个路径字符串。
    if (!InstanceFiles::pathIsSameOrInside(r.gameDir, sourceDir)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "源实例不在当前游戏目录中，未复制文件。");
        return result;
    }
    if (QFileInfo::exists(original) || QFileInfo::exists(candidate)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "目标实例名已经存在，未覆盖任何文件。");
        return result;
    }
    if (core.mergedJson.value("mainClass").toString().isEmpty() || core.gameVersion.isEmpty()
        || !QDir::isAbsolutePath(core.clientJar) || !QFileInfo(core.clientJar).isFile()
        || redirected(core.clientJar)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "目标核心或客户端文件无效，未执行副本更新。");
        return result;
    }
    if (progress) progress(Stage::Checking);
    result.problem = checkTree(sourceDir);
    if (!result.problem.isEmpty()) return result;
    QString problem;
    const auto files = BackupFiles::collectFiles(sourceDir, {}, &problem, cancelled);
    if (stopped()) return cancelResult(QCoreApplication::translate("InstanceUpgrade", "已取消复制。"));
    if (!problem.isEmpty() || files.isEmpty()) {
        result.problem = problem.isEmpty() ? QCoreApplication::translate("InstanceUpgrade", "源实例是空的。") : problem;
        return result;
    }
    r.originalFingerprint = fingerprint(files);
    // 只复制选中的类别（`copyRoots` 为空 = 整棵树，旧行为）。
    QList<BackupFiles::FileRecord> selected;
    if (r.copyRoots.isEmpty()) {
        selected = files;
    } else {
        for (const auto &file : files) {
            const QString first = file.path.section(QLatin1Char('/'), 0, 0);
            if (r.copyRoots.contains(first) || r.copyRoots.contains(file.path)) {
                selected.append(file);
            }
        }
    }
    qint64 size = 0;
    result.copiedFiles = selected.size();
    result.skippedFiles = files.size() - selected.size();
    for (const auto &file : selected) {
        if (file.size < 0 || size > std::numeric_limits<qint64>::max() - file.size) {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "实例大小溢出，未开始复制。");
            return result;
        }
        size += file.size;
    }
    // 只有一次复制：源 → 暂存候选。校验是读一遍比对，不再另占一份空间。
    const qint64 margin = 64 * 1024 * 1024;
    const qint64 available = QStorageInfo(r.gameDir).bytesAvailable();
    const qint64 budget = r.spaceBudgetBytes >= 0
        ? qMin(available, r.spaceBudgetBytes) : available;
    if (available < 0 || size > std::numeric_limits<qint64>::max() - margin
        || budget < size + margin) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "空间不足以复制更新副本（只需一份用户文件的体积），未复制任何文件。");
        return result;
    }
    if (stopped()) return cancelResult(QCoreApplication::translate("InstanceUpgrade", "已取消复制。"));
    if (progress) progress(Stage::Copy);
    const auto copySelected = [&]() -> bool {
        if (r.copyRoots.isEmpty()) {
            return copyTreeWithoutFollowingLinks(sourceDir, candidate, cancelled);
        }
        // 只复制"文件"类（options.txt / servers.dat）时没有目录树来建候选目录，这里先建出来。
        QDir().mkpath(candidate);
        for (const QString &root : r.copyRoots) {
            const QString from = sourceDir + QLatin1Char('/') + root;
            if (!QFileInfo::exists(from) || QFileInfo(from).isSymLink()) {
                continue;  // 这一类盘上没有（或是指向别处的链接）→ 不复制
            }
            QString copyProblem;
            if (!copyTreeWithoutFollowingLinks(from, candidate + QLatin1Char('/') + root,
                                              cancelled, &copyProblem)) {
                return false;
            }
        }
        return true;
    };
    if (!copySelected()) {
        if (stopped()) return cancelResult(QCoreApplication::translate("InstanceUpgrade", "已取消复制。"));
        result.problem = QCoreApplication::translate("InstanceUpgrade", "更新副本复制失败，原实例未改变。");
        return result;
    }
    // 只校验真的复制了的那一份；一条都没选时跳过（候选目录里此刻只有核心文件，稍后写入）。
    if (!selected.isEmpty() && !sameFiles(candidate, selected, cancelled)) {
        if (stopped()) return cancelResult(QCoreApplication::translate("InstanceUpgrade", "已取消复制。"));
        result.problem = QCoreApplication::translate("InstanceUpgrade", "更新副本未通过完整校验，原实例未改变。");
        return result;
    }
    // U4：资源包启用列表按"真的迁入了哪些包"重建（只在 options / resourcepacks 参与这次复制时做）。
    if (r.copyRoots.isEmpty() || r.copyRoots.contains(QStringLiteral("options.txt"))
        || r.copyRoots.contains(QStringLiteral("resourcepacks"))) {
        const QString optionsPath = candidate + QStringLiteral("/options.txt");
        if (QFileInfo::exists(optionsPath)) {
            QFile optionsFile(optionsPath);
            if (optionsFile.open(QIODevice::ReadOnly)) {
                const QString optionsText = QString::fromUtf8(optionsFile.readAll());
                optionsFile.close();
                QStringList packs;
                QDirIterator packIt(candidate + QStringLiteral("/resourcepacks"),
                                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
                while (packIt.hasNext()) {
                    const QFileInfo packInfo(packIt.next());
                    packs.append(QStringLiteral("file/") + packInfo.fileName());
                }
                const QString rewritten =
                    UpgradeOptions::rewriteKeepingAll(optionsText, packs);
                QSaveFile optionsOut(optionsPath);
                if (optionsOut.open(QIODevice::WriteOnly)) {
                    optionsOut.write(rewritten.toUtf8());
                    optionsOut.commit();
                }
            }
        }
    }
    if (!applyCoreToCandidate(r, core, candidate, &result)) return result;
    if (stopped()) return cancelResult(QCoreApplication::translate("InstanceUpgrade", "已取消，更新副本未发布。"));
    const int disabled = result.disabledMods;
    const auto newFiles = BackupFiles::collectFiles(candidate, {}, &problem, cancelled);
    if (stopped()) return cancelResult(QCoreApplication::translate("InstanceUpgrade", "已取消，更新副本未发布。"));
    if (!problem.isEmpty() || newFiles.isEmpty()) {
        result.problem = problem.isEmpty() ? QCoreApplication::translate("InstanceUpgrade", "更新副本没有文件。") : problem;
        return result;
    }
    result.problem = checkTree(candidate);
    if (result.problem.isEmpty() && !sameFiles(candidate, newFiles, cancelled)) {
        result.problem = stopped() ? QString() : QCoreApplication::translate("InstanceUpgrade", "更新副本在准备期间发生变化，未发布。");
        if (result.problem.isEmpty()) return cancelResult(QCoreApplication::translate("InstanceUpgrade", "已取消，更新副本未发布。"));
        return result;
    }
    if (!result.problem.isEmpty()) return result;
    const QString journalState = QStringLiteral("copy_prepared");
    if (!writeJournal(r, journalState, QString())) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "无法写入副本事务记录，未发布。");
        return result;
    }
    if (progress) progress(Stage::Publish);
    if (stopped()) return cancelResult(QCoreApplication::translate("InstanceUpgrade", "已取消发布。"));
    if (QFileInfo::exists(original)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "目标实例名在复制期间被创建，未覆盖。");
        return result;
    }
    if (!QDir().rename(candidate, original)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "无法发布更新副本，原实例未改变。");
        return result;
    }
    if (progress) progress(Stage::Verify);
    if (checkTree(original).isEmpty() && sameFiles(original, newFiles, cancelled)) {
        result.disabledMods = disabled;
        result.requiresReload = true;
        result.ok = writeJournal(r, QStringLiteral("committed"), QString());
        if (!result.ok) {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "副本已发布，但事务记录未能更新；没有删除任何文件。");
        }
        if (progress && result.ok) progress(Stage::Done);
        return result;
    }
    // 发布后的校验没过：不删这份副本，但也不把它留在实例名上（那一份会被当成能启动）。
    // 换成 rejected 名字，并在记录里说清它是哪一次、为什么被拒。
    const QString rejected = transactionDir(r) + QStringLiteral("/rejected");
    result.requiresReload = true;
    if (!QFileInfo::exists(rejected) && QDir().rename(original, rejected)
        && writeJournal(r, QStringLiteral("copy_rejected"), QString())) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "更新副本发布后校验失败，已保留在被拒绝目录：")
            + rejected;
    } else {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "升级副本校验失败，自动撤回也未完成。请检查以下内容：")
            + original;
    }
    return result;
}
} // namespace

Result prepareCopyAndCommit(Request r, const Core &core, const QString &sourceDir,
                            const std::function<void(Stage)> &progress,
                            const std::function<bool()> &cancelled) {
    const QString invalid = validate(r);
    if (!invalid.isEmpty()) { Result result; result.problem = invalid; return result; }
    QString lockProblem;
    if (!DirectorySessionLock::acquire(r.gameDir, &lockProblem)) {
        Result result; result.problem = lockProblem; return result;
    }
    const QString parent = QFileInfo(transactionDir(r)).absolutePath();
    QDir().mkpath(parent);
    QLockFile lock(transactionDir(r) + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        Result result; result.problem = QCoreApplication::translate("InstanceUpgrade", "副本事务正在使用，未修改实例。"); return result;
    }
    const QString startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const auto result = prepareCopyAndCommitImpl(r, core, sourceDir, progress, cancelled);
    if (!result.ok) discardUnpublishedStaging(r);
    if (result.ok) {
        recordUpgradeHistory(r, core, sourceDir, originalDir(r), startedAt, QString(),
                             true, false, QString(), result.copiedFiles, result.skippedFiles);
    } else if (!result.cancelled) {
        recordUpgradeHistory(r, core, sourceDir, originalDir(r), startedAt, QString(),
                             false, false, result.problem, result.copiedFiles,
                             result.skippedFiles);
    } else {
        recordUpgradeHistory(r, core, sourceDir, originalDir(r), startedAt, QString(),
                             false, true, result.problem, result.copiedFiles,
                             result.skippedFiles);
    }
    return result;
}

static Result rollbackImpl(const Request &r, bool verifyRetained,
                           const std::function<bool()> &cancelled = {}) {
    Result result;
    const auto stopped = [&] { return cancelled && cancelled(); };
    const auto stopResult = [&] {
        result.cancelled = true;
        result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，当前实例未移动。");
        return result;
    };
    result.problem = validate(r);
    if (!result.problem.isEmpty()) return result;
    const auto journal = readJournal(r);
    const QString retained = transactionDir(r) + "/original";
    const QString displaced = transactionDir(r) + "/replaced";
    const QString original = originalDir(r);
    if (journal.isEmpty() || !QFileInfo(retained).isDir()) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "未找到升级前的实例目录，未执行回滚。");
        return result;
    }
    result.snapshotZip = journal.value("snapshotZip").toString();
    if (!result.snapshotZip.isEmpty() && !QFileInfo::exists(result.snapshotZip)) {
        const QString rebased = QDir(r.gameDir).filePath(
            QStringLiteral("cache/backups/instances/") + QFileInfo(result.snapshotZip).fileName());
        if (QFileInfo::exists(rebased)) result.snapshotZip = rebased;
    }
    result.rollbackAvailable = true;
    if (stopped()) return stopResult();
    if (verifyRetained) {
        result.problem = dependentInstanceProblem(r.gameDir, r.instanceId);
        if (!result.problem.isEmpty()) return result;
        const QString expectedFolder = QDir(r.gameDir).filePath("cache/backups/instances");
        if (redirected(result.snapshotZip) || QDir(QFileInfo(result.snapshotZip).absolutePath()) != QDir(expectedFolder)) {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "快照位置不符合本次事务，未移动当前实例。"); return result;
        }
        bool ok = false;
        QString problem;
        const auto manifest = BackupFiles::readManifest(result.snapshotZip, &ok, &problem);
        const QString treeProblem = checkTree(retained);
        if (!ok || !manifest.complete || !manifest.excluded.isEmpty() || manifest.root != r.instanceId
            || manifest.kind != BackupFiles::Kind::Instance || !treeProblem.isEmpty()) {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "保留的原目录未通过快照清单校验；未移动当前实例，请使用完整 ZIP 快照恢复。%1")
                                 .arg(problem.isEmpty() ? treeProblem : problem);
            return result;
        }
        if (stopped()) return stopResult();
        QString verifyProblem;
        if (!sameFiles(retained, manifest.files, cancelled, &verifyProblem)) {
            if (stopped()) return stopResult();
            result.problem = QCoreApplication::translate("InstanceUpgrade", "保留的原目录未通过快照清单校验；未移动当前实例，请使用完整 ZIP 快照恢复。%1")
                                 .arg(verifyProblem);
            return result;
        }
    }
    const bool hadCurrent = QFileInfo::exists(original);
    if (hadCurrent && QFileInfo::exists(displaced)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "回滚保存位置已有文件，未覆盖：%1").arg(displaced);
        return result;
    }
    // 最后这一小段（写阶段记录 + 两三次同卷改名）是**故意不可取消**的临界区：
    // 停在中间会把"当前版本"和"保留的原目录"两边都留在半路。
    if (stopped()) return stopResult();
    if (!writeJournal(r, "rollback_pending", result.snapshotZip)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "无法记录回滚阶段，未移动实例文件。");
        return result;
    }
    if (hadCurrent && !QDir().rename(original, displaced)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "无法暂存当前版本，未覆盖或删除任何文件。");
        return result;
    }
    if (!QDir().rename(retained, original)) {
        const bool putBack = !hadCurrent || QDir().rename(displaced, original);
        result.problem = QCoreApplication::translate("InstanceUpgrade", "原目录未能恢复，恢复材料保留在 %1。%2")
            .arg(retained, putBack ? QString() : QCoreApplication::translate("InstanceUpgrade", "当前版本也保留在 replaced 目录中。"));
        return result;
    }
    result.rolledBack = true;
    result.rollbackAvailable = false;
    result.ok = writeJournal(r, "rolled_back", result.snapshotZip);
    if (!result.ok) result.problem = QCoreApplication::translate("InstanceUpgrade", "原实例已恢复，但回滚记录未能更新；没有删除恢复材料。");
    return result;
}

Result rollback(const Request &r, const std::function<bool()> &cancelled) {
    QString problem;
    if (!DirectorySessionLock::acquire(r.gameDir, &problem)) {
        Result result; result.problem = problem; return result;
    }
    return rollbackImpl(r, true, cancelled);
}

static Result commitPreparedImpl(const Request &r, const std::function<void(Stage)> &progress,
                                 const std::function<bool()> &cancelled) {
    Result result;
    const auto report = [&](Stage stage) { if (progress) progress(stage); };
    const auto stopped = [&] { return cancelled && cancelled(); };
    report(Stage::Checking);
    result.problem = validate(r);
    if (!result.problem.isEmpty()) return result;
    const QString original = originalDir(r), candidate = candidateDir(r);
    if (QFileInfo::exists(journalPath(r)) || !QFileInfo(original).isDir()
        || !QFileInfo(candidate).isDir() || QFileInfo::exists(transactionDir(r) + "/original")) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "原实例、暂存版本或事务目录状态不正确，未替换实例。");
        return result;
    }
    for (const auto &root : {original, candidate}) {
        result.problem = checkTree(root);
        if (!result.problem.isEmpty()) return result;
        // 这两趟整树扫描各自可能几十秒（大整合包）；不在这儿看取消位，
        // 用户按了取消要等整棵树扫完才有反应 —— 而这时候还什么都没动。
        if (stopped()) {
            result.cancelled = true;
            result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，原实例未改变。");
            return result;
        }
    }
    QFile version(candidate + '/' + r.instanceId + ".json");
    if (!version.open(QIODevice::ReadOnly) || version.size() > 16 * 1024 * 1024
        || QJsonDocument::fromJson(version.readAll()).object().value("id").toString() != r.instanceId) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "暂存版本的核心 JSON 缺失或实例名不匹配，未替换。");
        return result;
    }
    version.close();
    QString problem;
    const auto newFiles = BackupFiles::collectFiles(candidate, {}, &problem, cancelled);
    if (stopped()) {
        result.cancelled = true;
        result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，原实例未改变。");
        return result;
    }
    if (!problem.isEmpty() || newFiles.isEmpty()) {
        result.problem = problem.isEmpty() ? QCoreApplication::translate("InstanceUpgrade", "暂存版本没有文件。") : problem;
        return result;
    }
    const auto oldFiles = BackupFiles::collectFiles(original, {}, &problem, cancelled);
    if (stopped()) {
        result.cancelled = true;
        result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，原实例未改变。");
        return result;
    }
    if (!r.originalFingerprint.isEmpty() && fingerprint(oldFiles) != r.originalFingerprint) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "原实例在准备期间发生了变化，未替换实例。"); return result;
    }
    qint64 oldBytes = 0;
    for (const auto &file : oldFiles) {
        if (file.size < 0 || file.size > (std::numeric_limits<qint64>::max() / 2 - oldBytes)) {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "实例大小超出可安全计算的范围。"); return result;
        }
        oldBytes += file.size;
    }
    const QStorageInfo storage(r.gameDir);
    const qint64 available = r.spaceBudgetBytes >= 0
        ? qMin(storage.bytesAvailable(), r.spaceBudgetBytes) : storage.bytesAvailable();
    const qint64 margin = 32 * 1024 * 1024;
    if (!problem.isEmpty() || oldFiles.isEmpty() || !storage.isReady()
        || available < 0 || oldBytes > (std::numeric_limits<qint64>::max() - margin) / 2
        || available < oldBytes * 2 + margin) {
        result.problem = problem.isEmpty() ? QCoreApplication::translate("InstanceUpgrade", "无法确认有足够空间制作并校验完整快照，未替换实例。") : problem;
        return result;
    }
    if (stopped()) { result.cancelled = true; result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，原实例未改变。"); return result; }
    report(Stage::Snapshot);
    BackupFiles::Manifest manifest;
    manifest.kind = BackupFiles::Kind::Instance;
    manifest.root = r.instanceId;
    // BackupController discovers instance snapshots by directory-name prefix,
    // not the formatted/custom display name (which changes after an upgrade).
    manifest.label = r.instanceId;
    manifest.sourceInstanceId = r.instanceId;
    manifest.sourceInstanceName = r.label.isEmpty() ? r.instanceId : r.label;
    manifest.complete = true;
    manifest.safety = true;
    manifest.scope = {QCoreApplication::translate("InstanceUpgrade", "原地升级前完整实例快照（不排除 assets、libraries 或用户文件）")};
    QString zip = QDir(r.gameDir).filePath("cache/backups/instances/")
        + BackupFiles::backupFileName(manifest.label, QDateTime::currentDateTime(), true)
              .chopped(4) + '-' + r.transactionId + ".zip";
    if (QFileInfo::exists(zip) || redirected(zip)) { result.problem = QCoreApplication::translate("InstanceUpgrade", "快照名称已存在或是链接，未覆盖。"); return result; }
    const auto snapshot = BackupFiles::writeBackup(manifest, original, zip, cancelled);
    if (!snapshot.ok) {
        result.cancelled = stopped(); // Snapshot phase has not moved the original directory.
        result.problem = snapshot.problem;
        return result;
    }
    // `snapshot.zipPath` 才是真正落盘的那一个（撞名时 `writeBackup()` 会换一个名字）。
    // 回滚读的是**记进事务文件的那条路径**，记错名字等于那一份快照永远找不回来。
    zip = snapshot.zipPath;
    result.snapshotZip = zip;
    report(Stage::VerifySnapshot);
    {
        QTemporaryDir verify(transactionDir(r) + "/verify-XXXXXX");
        if (!verify.isValid() || !ZipUtil::extractArchives({zip}, verify.path(), {}, &problem)) {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "完整快照校验失败，或原实例在备份期间发生变化，未替换。%1").arg(problem);
            return result;
        }
        if (stopped()) {
            result.cancelled = true;
            result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，快照保留，原实例未改变。");
            return result;
        }
        QString snapshotProblem;
        const auto sameAsSnapshot = [&](const QString &root,
                                        const QList<BackupFiles::FileRecord> &expected,
                                        const QString &label) {
            QString reason;
            if (sameFiles(root, expected, cancelled, &reason)) return true;
            snapshotProblem = QCoreApplication::translate("InstanceUpgrade", "（%1：%2）").arg(label, reason);
            return false;
        };
        if (!sameAsSnapshot(verify.path() + '/' + r.instanceId, snapshot.manifest.files,
                            QCoreApplication::translate("InstanceUpgrade", "快照解压"))
            || !sameAsSnapshot(original, snapshot.manifest.files, QCoreApplication::translate("InstanceUpgrade", "原实例与快照"))
            || !sameAsSnapshot(original, oldFiles, QCoreApplication::translate("InstanceUpgrade", "原实例与准备清单"))) {
            if (stopped()) {
                result.cancelled = true;
                result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，快照保留，原实例未改变。");
                return result;
            }
            result.problem = QCoreApplication::translate("InstanceUpgrade", "完整快照校验失败，或原实例在备份期间发生变化，未替换。%1")
                                 .arg(snapshotProblem);
            return result;
        }
    }
    if (stopped()) { result.cancelled = true; result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，快照保留，原实例未改变。"); return result; }
    result.problem = checkTree(original);
    if (result.problem.isEmpty()) result.problem = checkTree(candidate);
    QString stagedProblem;
    const bool stagedSame = result.problem.isEmpty()
                            && sameFiles(candidate, newFiles, cancelled, &stagedProblem);
    if (!result.problem.isEmpty() || !stagedSame) {
        if (stopped()) {
            result.cancelled = true;
            result.problem = QCoreApplication::translate("InstanceUpgrade", "已取消，原实例未改变。");
        } else if (!stagedProblem.isEmpty()) {
            result.problem = QCoreApplication::translate("InstanceUpgrade", "暂存版本在准备期间发生变化，未替换。%1").arg(stagedProblem);
        }
        return result;
    }
    if (!writeJournal(r, "prepared", zip)) {
        result.problem = QCoreApplication::translate("InstanceUpgrade", "无法写入升级事务记录，未替换实例。"); return result;
    }
    const auto failAndRestore = [&](const QString &reason) {
        result.requiresReload = true;
        const auto recovery = rollbackImpl(r, false);
        result.rolledBack = recovery.rolledBack;
        result.rollbackAvailable = recovery.rollbackAvailable;
        // 只有确认恢复了原实例才可显示“取消”。恢复失败必须保持失败终态。
        if (!recovery.ok || !recovery.rolledBack) result.cancelled = false;
        result.problem = reason + (recovery.rolledBack ? QCoreApplication::translate("InstanceUpgrade", " 原实例已自动恢复。")
            : QCoreApplication::translate("InstanceUpgrade", " 回滚未完成：%1").arg(recovery.problem));
        return result;
    };
    report(Stage::RetainOriginal);
    result.problem = dependentInstanceProblem(r.gameDir, r.instanceId);
    if (!result.problem.isEmpty()) return result;
    if (!retainDirectory(original, transactionDir(r) + "/original", &result.problem)) return result;
    if (!writeJournal(r, "original_retained", zip))
        return failAndRestore(QCoreApplication::translate("InstanceUpgrade", "无法记录原目录位置。"));
    report(Stage::Publish);
    if (stopped()) {
        // 取消落在"已经把原目录收起来、还没换上新版本"这一小段：自动恢复再报取消。
        result.cancelled = true;
        return failAndRestore(QCoreApplication::translate("InstanceUpgrade", "已取消发布。"));
    }
    if (!QDir().rename(candidate, original))
        return failAndRestore(QCoreApplication::translate("InstanceUpgrade", "发布新版本中断或失败。"));
    report(Stage::Verify);
    if (stopped()) {
        result.cancelled = true;
        return failAndRestore(QCoreApplication::translate("InstanceUpgrade", "已取消发布后的校验。"));
    }
    const QString publishTreeProblem = checkTree(original);
    QString publishProblem;
    const bool publishSame = publishTreeProblem.isEmpty()
                             && sameFiles(original, newFiles, cancelled, &publishProblem);
    if (!publishTreeProblem.isEmpty() || !publishSame) {
        if (stopped()) result.cancelled = true;
        return failAndRestore(QCoreApplication::translate("InstanceUpgrade", "发布后的实例校验失败。%1")
                                  .arg(publishTreeProblem.isEmpty() ? publishProblem : publishTreeProblem));
    }
    if (!writeJournal(r, "committed", zip))
        return failAndRestore(QCoreApplication::translate("InstanceUpgrade", "无法记录升级完成状态。"));
    result.ok = true;
    result.rollbackAvailable = true;
    report(Stage::Done);
    return result;
}

Result commitPrepared(const Request &r, const std::function<void(Stage)> &progress,
                      const std::function<bool()> &cancelled) {
    QString problem;
    if (!DirectorySessionLock::acquire(r.gameDir, &problem)) {
        Result result; result.problem = problem; return result;
    }
    const auto result = commitPreparedImpl(r, progress, cancelled);
    if (!result.ok) discardUnpublishedStaging(r);
    return result;
}

QStringList recoverableTransactions(const QString &gameDir, const QString &instanceId) {
    QStringList ids;
    const QDir root(QDir(gameDir).filePath("cache/instance-upgrades"));
    for (const auto &id : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
        const Request r{gameDir, instanceId, id, {}, -1, {}, {}};
        if (validate(r).isEmpty() && !readJournal(r).isEmpty()
            && QFileInfo(transactionDir(r) + "/original").isDir()) ids.append(id);
    }
    return ids;
}

QStringList recoverInterrupted(const QString &gameDir) {
    QStringList messages;
    QString problem;
    if (!DirectorySessionLock::acquire(gameDir, &problem)) return {problem};
    const QDir root(QDir(gameDir).filePath("cache/instance-upgrades"));
    if (redirected(root.absolutePath())) return {QCoreApplication::translate("InstanceUpgrade", "升级事务目录是链接，未自动恢复。")};
    for (const auto &id : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QLockFile lock(root.filePath(id + QStringLiteral(".lock")));
        lock.setStaleLockTime(0);
        if (!lock.tryLock()) continue;
        QFile f(root.filePath(id + "/journal.json"));
        if (redirected(root.filePath(id)) || redirected(f.fileName())) continue;
        if (!f.exists()) {
            // 没有清单的事务目录只有两种来路 —— 「准备」拷到一半就崩了
            // （暂存里有整整一份实例，几个 GB，永远没人清），或者**另一个启动器正在拷**
            // （这份程序没有"只许开一个"的锁）。判据用"最近改过没有"：正在拷的那一份
            // 时时刻刻在写文件；半个小时没人碰它，就只能是一具尸体。
            const QString txn = QDir(gameDir).filePath("cache/instance-upgrades/" + id);
            const bool shaped = QRegularExpression("^[A-Za-z0-9-]{1,80}$").match(id).hasMatch();
            if (shaped && !QFileInfo(txn + "/original").isDir()
                && !QFileInfo(txn + "/replaced").isDir() && stagingIsStale(txn)
                && removeTreeWithoutFollowingLinks(txn))
                messages.append(QCoreApplication::translate("InstanceUpgrade", "已清理未登记的升级暂存：%1").arg(id));
            continue;
        }
        if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024) continue;
        const auto json = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        const Request r{gameDir, json.value("instanceId").toString(), id, {}, -1, {}, {}};
        if (!validate(r).isEmpty()) continue;
        if (readJournal(r).isEmpty()) {
            // 记录在、但这一个游戏根不是它当初写下的那一个（或被改坏）：不自动动盘，
            // 把事务与快照位置说清楚，让用户用完整 ZIP 快照恢复。
            if (!json.value("instanceId").toString().isEmpty()
                && json.value("transactionId").toString() == id) {
                const QString snapshot = json.value("snapshotZip").toString();
                messages.append(
                    QCoreApplication::translate("InstanceUpgrade", "升级事务 %1（实例 %2）找不到匹配的游戏根指纹，未自动恢复；" "请使用完整 ZIP 快照%3恢复。")
                        .arg(id, json.value("instanceId").toString(),
                             snapshot.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" %1 ").arg(QFileInfo(snapshot).fileName())));
            }
            continue;
        }
        const QString state = json.value("state").toString();
        // 副本模式（2.1.1）：源实例没被动过，所以**没有**回滚材料；这里只负责
        // 识别并收掉自己留下的暂存。不认识的目录一律不碰。
        if (state == QLatin1String("copy_prepared")) {
            const bool published = QFileInfo(originalDir(r)).isDir();
            const bool cleaned = removeTreeWithoutFollowingLinks(transactionDir(r));
            messages.append(published
                ? QCoreApplication::translate("InstanceUpgrade", "上次的更新副本已发布（事务记录未更新），已清理暂存：%1").arg(r.instanceId)
                : QCoreApplication::translate("InstanceUpgrade", "已清理未发布的更新副本暂存：%1%2")
                      .arg(r.instanceId, cleaned ? QString() : QCoreApplication::translate("InstanceUpgrade", "（未能删除暂存文件，需检查）")));
            continue;
        }
        if (state == QLatin1String("copy_rejected")) {
            messages.append(QCoreApplication::translate("InstanceUpgrade", "更新副本发布后校验失败，材料保留在：%1")
                                .arg(transactionDir(r) + QStringLiteral("/rejected")));
            continue;
        }
        if (state == QLatin1String("committed")
            && !QFileInfo(transactionDir(r) + "/original").isDir()
            && !QFileInfo(transactionDir(r) + "/replaced").isDir()) {
            // 副本模式那次已成功提交：没有回滚材料，事务目录只剩一份记录，不必再留。
            if (removeTreeWithoutFollowingLinks(transactionDir(r))) {
                messages.append(QCoreApplication::translate("InstanceUpgrade", "已清理副本升级记录：%1").arg(r.instanceId));
            }
            continue;
        }
        if (state != "prepared" && state != "original_retained" && state != "rollback_pending") continue;
        if (state == "prepared" && !QFileInfo(transactionDir(r) + "/original").isDir()) {
            // Crash before the original was retained: nothing under versions/ was
            // touched, so the staged copy has no recovery value. Do not leak it.
            // (`prepared` with an original/ directory can only mean the crash hit
            // between the rename and the journal update — that one must roll back.)
            if (removeTreeWithoutFollowingLinks(transactionDir(r)))
                messages.append(QCoreApplication::translate("InstanceUpgrade", "已清理未发布的升级暂存：%1").arg(r.instanceId));
            continue;
        }
        if (!QFileInfo(transactionDir(r) + "/original").isDir()) continue;
        const auto result = rollbackImpl(r, false);
        messages.append(result.rolledBack ? QCoreApplication::translate("InstanceUpgrade", "中断的升级已恢复原实例：%1").arg(r.instanceId)
            : QCoreApplication::translate("InstanceUpgrade", "升级恢复需要处理：%1；%2").arg(r.instanceId, result.problem));
    }
    return messages;
}

QStringList recoverInterruptedInRoots(const QStringList &gameDirs) {
    QStringList messages;
    QSet<QString> seen;
    for (const QString &raw : gameDirs) {
        const QString dir = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
        const QString key = InstanceFiles::canonicalPath(dir).toLower();
        if (dir.isEmpty() || key.isEmpty() || seen.contains(key) || !QFileInfo(dir).isDir()) {
            continue;
        }
        seen.insert(key);
        const QStringList one = recoverInterrupted(dir);
        for (const QString &message : one) {
            // 多个根一起扫时带上根名，否则“已恢复原实例”看不出是哪一份。
            messages.append(gameDirs.size() > 1
                                ? QStringLiteral("[%1] %2").arg(QFileInfo(dir).fileName(), message)
                                : message);
        }
    }
    return messages;
}
} // namespace InstanceUpgrade









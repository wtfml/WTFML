/**
 * @file InstanceMaintenanceController.cpp
 * @brief 实现实例配置与世界 level.dat 的轻量备份。
 */
#include "controllers/InstanceMaintenanceController.hpp"
#include <QCoreApplication>

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <functional>

#include "manage/FolderManager.hpp"
#include "manage/WorldFiles.hpp"
#include "ConfigFileText.hpp"
#include "util/AppPaths.hpp"
#include "util/WriteDirectoryGuard.hpp"

namespace {

/// 一个存档在 `<数据目录>/cache/world-backups` 底下占的**格子名**。
///
/// 只用**存档目录名**（`QFileInfo(worldDir).fileName()`）不行：
/// 两个实例各有一个叫 `world`（中文环境里更常见的是都叫「新的世界」）的存档就共用一个
/// 格子，两件坏事同时发生：
/// · 清理按"每个格子只留最近 N 代"算 —— A 备份一次就把 B 的代数挤掉一份，B 那份
/// 可能是唯一一份；
/// · `latestWorldBackup()` 会拿到**另一个世界**的 level.dat，而界面上那句提示正是
/// "要还原的话把那一份拷回存档目录" —— 用户照着做，拿到的是别人的种子、地形生成
/// 设置和出生点，而原来那份再也回不来了（自动备份只留 N 代，早被挤掉）。
/// 所以格子名必须能区分"哪个实例的哪个世界"。
///
/// 标准布局 `<数据目录>/versions/<实例 id>/saves/<世界>` 下取
/// `"<实例 id>__<世界>-<绝对路径哈希>"`：
/// 一眼看得懂是哪个实例的哪个存档；实例目录改名/删掉之后旧备份不再和这一份认亲 ——
/// 那是"另一个世界"了，正是这里要的。别的布局拿绝对路径的短哈希兜底（**不能**只退回
/// 世界名，那正是要避免的那个撞车）。
///
/// 补：**最后那段哈希不能省**。`__` 在实例名和世界名里都是合法字符
/// （`validatePathSegment` 只挡 `<>:"/\|?*`），所以纯 `实例__世界` 仍然不是单射：
/// 实例「SMP__2024」+ 世界「old」和实例「SMP」+ 世界「2024__old」拼出来是同一个
/// 格子名 —— 两个不同世界的 level.dat 又会互相挤掉代数、`latestWorldBackup()`
/// 拿到别人的文件。加了绝对路径的哈希之后两边才真正分开。
QString worldBackupSlot(const QString &worldDir) {
    const QFileInfo info(worldDir);
    const QString worldName = info.fileName();
    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    const QByteArray digest =
        QCryptographicHash::hash(absolute.toUtf8(), QCryptographicHash::Sha256);
    const QString tag = QString::fromLatin1(digest.toHex().left(8));
    const QString versionsRoot =
        QDir::cleanPath(AppPaths::minecraftDir() + QStringLiteral("/versions")) + QLatin1Char('/');
    if (absolute.startsWith(versionsRoot, Qt::CaseInsensitive)) {
        const QStringList parts = absolute.mid(versionsRoot.size()).split(QLatin1Char('/'));
        if (parts.size() >= 3 && parts.at(1) == QStringLiteral("saves")) {
            return parts.at(0) + QStringLiteral("__") + worldName + QLatin1Char('-') + tag;
        }
    }
    return worldName + QLatin1Char('-') + tag;
}

}  // namespace

QStringList InstanceMaintenanceController::configBackups(const QString &instanceId,
                                                         const QString &configDir,
                                                         const QString &path) {
    QStringList backups;
    if (instanceId.isEmpty() || instanceId == QStringLiteral(".") || instanceId == QStringLiteral("..")
        || instanceId.contains(QLatin1Char('/')) || instanceId.contains(QLatin1Char('\\'))) {
        return backups;
    }
    const QString root = AppPaths::configBackupDir();
    const QStringList names =
        QDir(root + QLatin1Char('/') + instanceId).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList candidates;
    for (const QString &name : names) {
        if (ConfigFileText::isBackupFolderName(name)) {
            candidates.append(name);
        }
    }
    std::sort(candidates.begin(), candidates.end(), std::greater<QString>());
    for (const QString &name : candidates) {
        const QString candidate =
            ConfigFileText::backupPathFor(root, instanceId, configDir, path, name);
        const QFileInfo file(candidate);
        if (!file.isFile() || file.isSymLink()) continue;
        const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
        const QString canonicalFile = file.canonicalFilePath();
        if (canonicalRoot.isEmpty() || canonicalFile.isEmpty()) continue;
        const QString generationRoot = QDir(canonicalRoot).filePath(instanceId + QLatin1Char('/') + name);
        const QString relative = QDir(generationRoot).relativeFilePath(canonicalFile);
        if (QDir::isAbsolutePath(relative) || relative == QStringLiteral("..")
            || relative.startsWith(QStringLiteral("../"))) continue;
        backups.append(candidate);
    }
    return backups;
}

QString InstanceMaintenanceController::latestConfigBackup(const QString &instanceId,
                                                           const QString &configDir,
                                                           const QString &path) {
    return configBackups(instanceId, configDir, path).value(0);
}

void InstanceMaintenanceController::pruneConfigBackups(const QString &instanceId, int keep) {
    if (instanceId.isEmpty()) {
        return;
    }
    if (!WriteDirectoryGuard::acquire(AppPaths::minecraftDir())) {
        return;
    }
    const QString dir = AppPaths::configBackupDir() + QLatin1Char('/') + instanceId;
    const QStringList names = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : ConfigFileText::backupFoldersToRemove(names, keep)) {
        removeTreeWithoutFollowingLinks(dir + QLatin1Char('/') + name);
    }
}

bool InstanceMaintenanceController::backupWorldLevelData(const QString &worldDir, int keep,
                                                         QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const auto fail = [problemOut](const QString &why) {
        if (problemOut != nullptr) {
            *problemOut = why;
        }
        return false;
    };
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(worldDir, &lockProblem)) {
        return fail(lockProblem);
    }
    const QString path = WorldFiles::levelDataPath(worldDir);
    if (!QFileInfo::exists(path)) {
        return fail(QCoreApplication::translate("InstanceMaintenanceController", "这个存档里没有 level.dat。"));
    }

    const QString root = AppPaths::worldBackupDir();
    // 格子按"哪个实例的哪个世界"分（见 worldBackupSlot() 顶上那段）—— 不能只用世界名。
    const QString slot = worldBackupSlot(worldDir);
    const QString base = root + QLatin1Char('/') + slot;
    const QStringList existing = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const QString folder = ConfigFileText::backupFolderName(QDateTime::currentDateTime(), existing);
    const QString target = ConfigFileText::backupPathFor(root, slot, worldDir, path, folder);
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        return fail(QCoreApplication::translate("InstanceMaintenanceController", "无法创建备份目录：%1").arg(QDir::toNativeSeparators(target)));
    }
    // 用 QSaveFile（目标旁写临时文件、commit 时原子改名），**不用 `QFile::copy`**：
    // 这一步抄出来的东西是"改世界设置之前的那一份"，而 `latestWorldBackup()` 认的是
    // 目录里那个 `level.dat` 在不在。直接 copy 到一半被杀掉（关窗、断电、磁盘满）会留下
    // 一个截断的 `level.dat`，下一次"恢复上一次的存档设置"就会拿它去盖现在那份好的 ——
    // 半截的备份比没有备份更坏，因为它看着像一份备份。
    QFile source(path);
    QSaveFile backup(target);
    if (!source.open(QIODevice::ReadOnly) || !backup.open(QIODevice::WriteOnly)) {
        return fail(QCoreApplication::translate("InstanceMaintenanceController", "无法将 level.dat 复制到 %1。请检查磁盘空间和文件访问权限。")
                        .arg(QDir::toNativeSeparators(target)));
    }
    bool copied = true;
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    while (!source.atEnd()) {
        const qint64 got = source.read(buffer.data(), buffer.size());
        if (got <= 0 || backup.write(buffer.constData(), got) != got) {
            copied = false;
            break;
        }
    }
    if (!copied || !backup.commit()) {
        return fail(QCoreApplication::translate("InstanceMaintenanceController", "无法将 level.dat 复制到 %1。请检查磁盘空间和文件访问权限。")
                        .arg(QDir::toNativeSeparators(target)));
    }

    const QStringList names = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : ConfigFileText::backupFoldersToRemove(names, keep)) {
        removeTreeWithoutFollowingLinks(base + QLatin1Char('/') + name);
    }
    return true;
}

QString InstanceMaintenanceController::latestWorldBackup(const QString &worldDir) {
    const QString dir = AppPaths::worldBackupDir() + QLatin1Char('/') + worldBackupSlot(worldDir);
    QStringList candidates;
    for (const QString &name : QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (ConfigFileText::isBackupFolderName(name)) {
            candidates.append(name);
        }
    }
    std::sort(candidates.begin(), candidates.end(), std::greater<QString>());
    for (const QString &name : candidates) {
        const QString candidate = dir + QLatin1Char('/') + name + QStringLiteral("/level.dat");
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

QStringList InstanceMaintenanceController::migrateWorldBackupSlots(
    const QString &oldInstanceDir, const QString &newInstanceDir, int *movedOut) {
    if (movedOut != nullptr) {
        *movedOut = 0;
    }
    QStringList problems;
    if (oldInstanceDir.trimmed().isEmpty() || newInstanceDir.trimmed().isEmpty()) {
        return problems;
    }
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(newInstanceDir, &lockProblem)) {
        problems.append(lockProblem);
        return problems;
    }
    // 列的是**新**目录：这一条在改名成功之后才调，旧目录已经不在了。世界目录相对
    // `saves/` 的那一段没变，所以拿它拼回旧路径就能算出旧格子名。
    const QDir newSaves(QDir(newInstanceDir).filePath(QStringLiteral("saves")));
    const QString root = AppPaths::worldBackupDir();
    int moved = 0;
    for (const QString &world : newSaves.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString newWorldDir = newSaves.filePath(world);
        const QString oldWorldDir =
            QDir(oldInstanceDir).filePath(QStringLiteral("saves/") + world);
        const QString fromSlot = worldBackupSlot(oldWorldDir);
        const QString toSlot = worldBackupSlot(newWorldDir);
        if (fromSlot == toSlot) {
            continue;  // 不是标准布局之类，格子名没跟着实例名走
        }
        const QString from = root + QLatin1Char('/') + fromSlot;
        const QString to = root + QLatin1Char('/') + toSlot;
        if (!QFileInfo::exists(from)) {
            continue;  // 这个世界没备份过（绝大多数都是这样）
        }
        const QFileInfo fromInfo(from);
        if (!fromInfo.isDir() || fromInfo.isSymLink() || fromInfo.isJunction()) {
            problems.append(QCoreApplication::translate("InstanceMaintenanceController", "存档备份没有搬：%1 不是一个普通目录。")
                                .arg(QDir::toNativeSeparators(from)));
            continue;
        }
        if (QFileInfo::exists(to)) {
            problems.append(QCoreApplication::translate("InstanceMaintenanceController", "%1 已存在，旧存档备份保留在原位置。")
                                .arg(QDir::toNativeSeparators(to)));
            continue;
        }
        if (!QDir().rename(from, to)) {
            problems.append(QCoreApplication::translate("InstanceMaintenanceController", "无法将存档备份从 %1 移至 %2。请检查文件是否被占用。")
                                .arg(QDir::toNativeSeparators(from),
                                     QDir::toNativeSeparators(to)));
            continue;
        }
        ++moved;
    }
    if (movedOut != nullptr) {
        *movedOut = moved;
    }
    return problems;
}

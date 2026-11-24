/**
 * @file PresetApply.cpp
 * @brief 实现 PresetApply.hpp。取舍都写在头文件里，这里只留实现上的几个坑。
 */

#include "PresetApply.hpp"
#include "PresetWriteLock.hpp"
#include "PresetExecutionReport.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QSaveFile>
#include <QSet>

#include "DataPackPreset.hpp"
#include "GenericFiles.hpp"
#include "PresetPlanner.hpp"
#include "PresetStore.hpp"
#include "ResourcePackPreset.hpp"
#include "ServerListPreset.hpp"
#include "VanillaOptions.hpp"
#include "manage/FolderManager.hpp"
#include "manage/ServerListFile.hpp"
#include "util/WriteDirectoryGuard.hpp"

namespace Presets {

namespace {

[[maybe_unused]] QString applyText(const char *source) { return QCoreApplication::translate("Presets", source); }

/// `options.txt` 的读取上限（同 `PresetCapture.cpp`）：真机上几十 KB，超过这个数不猜。
constexpr qint64 kOptionsMaxBytes = 32LL * 1024 * 1024;

/// 读一整份文件。`false` 时 `problem` 是一句人话。
bool readWholeFile(const QString &path, QByteArray *out, QString *problem) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *problem = QCoreApplication::translate("Presets", "无法读取「%1」：%2").arg(path, file.errorString());
        return false;
    }
    *out = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *problem = QCoreApplication::translate("Presets", "读取「%1」中断：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

/// 原子写一整份文件（`QSaveFile`）。`false` 时原来那一份**还在**。
bool writeWholeFile(const QString &path, const QByteArray &bytes, QString *problem) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *problem = QCoreApplication::translate("Presets", "无法写入「%1」：%2").arg(path, file.errorString());
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        *problem = QCoreApplication::translate("Presets", "写入「%1」中断：%2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *problem = QCoreApplication::translate("Presets", "「%1」替换失败：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

/**
 * @brief 造一个这一趟专用的备份目录（`<presets 根>/backups/<实例>/<时间>`）。
 *
 * 同一秒里撞上同名目录时往后加 `-2` / `-3`：覆盖掉上一份备份等于这一趟没备份。
 */
QString makeBackupDir(const QString &presetsRoot, const QString &instanceId) {
    // 备份落在**预设库**那一头（通常是默认数据根），可能不在调用方持有的实例根里。
    // 单独再取一次根锁：另一份启动器持着预设库时也不写备份，返回空串让调用方停下
    // （它的话术是"不敢在没有备份的情况下动这个实例"）。
    {
        QString lockProblem;
        if (!WriteDirectoryGuard::acquire(presetsRoot, &lockProblem)) {
            return QString();
        }
    }
    const QString base = presetsRoot + QStringLiteral("/backups/") +
                         (instanceId.isEmpty() ? QStringLiteral("instance") : instanceId) +
                         QLatin1Char('/') +
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString candidate = base;
    for (int n = 2; QFileInfo::exists(candidate) && n < 100; ++n) {
        candidate = base + QStringLiteral("-%1").arg(n);
    }
    if (!QDir().mkpath(candidate)) {
        return QString();
    }
    return candidate;
}

/**
 * @brief 把 @p relative 那一份（文件或整棵目录）从实例里拷进备份目录。
 *
 * @return 成功、或者目标上本来就没有这一份 ⇒ true。**备份失败必须让整趟停下** —— 这是
 * 调用方在写任何东西之前最后一道"还能回到原样"的保证。
 */
bool backupOne(const QString &instanceRoot, const QString &backupDir, const QString &relative,
               QString *problem) {
    if (relative.isEmpty()) {
        return true;
    }
    const QString source = instanceRoot + QLatin1Char('/') + relative;
    if (!QFileInfo::exists(source)) {
        return true;
    }
    const QString target = backupDir + QLatin1Char('/') + relative;
    const QString parent = QFileInfo(target).absolutePath();
    if (!QDir().mkpath(parent)) {
        *problem = QCoreApplication::translate("Presets", "无法创建备份目录「%1」。").arg(parent);
        return false;
    }
    QString copyProblem;
    if (!copyTreeWithoutFollowingLinks(source, target, {}, &copyProblem)) {
        *problem = QCoreApplication::translate("Presets", "备份「%1」失败，本次未写入任何内容：%2").arg(relative, copyProblem);
        return false;
    }
    return true;
}

QString fallbackSectionTitle(SectionKind kind) {
    switch (kind) {
    case SectionKind::VanillaOptions:
        return QCoreApplication::translate("Presets", "游戏设置");
    case SectionKind::VanillaKeys:
        return QCoreApplication::translate("Presets", "原版按键");
    case SectionKind::ServerList:
        return QCoreApplication::translate("Presets", "服务器列表");
    case SectionKind::MalilibKeys:
        return QCoreApplication::translate("Presets", "MaLiLib 热键");
    case SectionKind::GenericFiles:
        return QCoreApplication::translate("Presets", "配置文件");
    case SectionKind::Custom:
        break;
    }
    return QCoreApplication::translate("Presets", "未知内容");
}

PresetApplyRow rowFromPlan(const ApplyPlanRow &planRow) {
    PresetApplyRow row;
    row.sectionId = planRow.sectionId;
    row.kind = planRow.kind;
    row.mode = planRow.mode;
    row.title = planRow.title.isEmpty() ? fallbackSectionTitle(planRow.kind) : planRow.title;
    row.blocked = planRow.blocked;
    row.ok = !planRow.blocked && planRow.mode == ApplyMode::Skip;
    row.skipped = planRow.mode == ApplyMode::Skip;
    row.written = planRow.count;
    row.removed = planRow.removed;
    row.skippedItems = planRow.skipped;
    row.deferred = planRow.deferred;
    row.problem = planRow.problem;
    return row;
}

}  // namespace

TargetSnapshot readTargetSnapshot(const QString &instanceRoot, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    TargetSnapshot snapshot;
    snapshot.instanceRoot = instanceRoot;
    if (instanceRoot.trimmed().isEmpty()) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("Presets", "未指定目标实例。");
        }
        return snapshot;
    }

    // ---------- options.txt ----------
    const QString optionsPath = instanceRoot + QStringLiteral("/options.txt");
    const QFileInfo optionsInfo(optionsPath);
    if (optionsInfo.exists()) {
        if (!optionsInfo.isFile()) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate("Presets", "「%1」不是普通文件，已跳过应用预设。")
                                  .arg(optionsPath);
            }
            return snapshot;
        }
        if (optionsInfo.size() > kOptionsMaxBytes) {
            if (problemOut != nullptr) {
                *problemOut = applyText("「%1」大到不像是 options.txt（%2 字节），"
                                        "不敢拿预设去改它。")
                                  .arg(optionsPath)
                                  .arg(optionsInfo.size());
            }
            return snapshot;
        }
        QByteArray bytes;
        QString why;
        if (!readWholeFile(optionsPath, &bytes, &why)) {
            if (problemOut != nullptr) {
                *problemOut = why;
            }
            return snapshot;
        }
        snapshot.options = parseOptions(bytes);
    }

    // ---------- servers.dat ----------
    const QString serversPath = instanceRoot + QStringLiteral("/servers.dat");
    NbtTree::Document document;
    bool gzipped = false;
    QString why;
    const ServerListFile::FileState state =
        ServerListFile::loadDocument(serversPath, &document, &gzipped, &why);
    if (state == ServerListFile::FileState::Unreadable) {
        // 读不动和"没有这一份"是两件事（见 `PresetPlanner::TargetSnapshot` 那段）：
        // 前者会让服务器列表这一段这一趟做不了，后者是最正常的新实例。
        snapshot.serversProblem = why;
    } else if (state == ServerListFile::FileState::Ok) {
        if (!serverEntriesOf(&document, &snapshot.servers, &snapshot.serversUntouchable, &why)) {
            snapshot.serversProblem = why;
        }
    }
    return snapshot;
}

PresetApplyResult applyPresetToInstance(const QString &presetsRoot, const PresetManifest &sourceManifest,
                                        const PresetApplyRequest &request) {
    PresetApplyResult result;
    PresetManifest manifest = sourceManifest;
    QJsonObject previous;
    bool reportStarted = false;
    bool reportHealthy = true;
    const auto checkpoint = [&](bool finished) {
        if (!reportStarted) return;
        QString problem;
        if (!writePresetExecutionReport(request.instanceRoot, sourceManifest, result, finished, previous, &problem)) {
            result.ok = false; result.problem = problem; reportHealthy = false;
        }
    };
    const auto finish = [&]() { checkpoint(true); return result; };

    if (!isValidPresetId(manifest.id)) {
        result.problem = QCoreApplication::translate("Presets", "预设 ID 无效，无法应用。");
        return finish();
    }
    if (request.instanceRoot.trimmed().isEmpty() ||
        !QFileInfo(request.instanceRoot).isDir()) {
        result.problem = QCoreApplication::translate("Presets", "未找到目标实例目录，无法应用预设。");
        return finish();
    }

    if (!WriteDirectoryGuard::acquire(request.instanceRoot, &result.problem)) return finish();
    const auto writeLock = acquirePresetWrite(request.instanceRoot, &result.problem);
    if (!writeLock) return finish();
    if (request.resumeUnfinished) {
        previous = readPresetExecutionReport(request.instanceRoot, sourceManifest, &result.problem);
        if (previous.isEmpty()) return finish();
        QSet<QString> done;
        for (const auto &value : previous.value("rows").toArray()) {
            const auto row = value.toObject();
            const QString state = row.value("state").toString();
            if (state == QStringLiteral("done"))
                done.insert(row.value("sectionId").toString());
        }
        for (int i = manifest.sections.size() - 1; i >= 0; --i)
            if (done.contains(manifest.sections[i].id)) manifest.sections.removeAt(i);
        if (manifest.sections.isEmpty()) {
            result.ok = true; result.details.append(QCoreApplication::translate("Presets", "无未完成的预设段；待应用数据包仍等待目标世界。"));
            return finish();
        }
    }

    // ------------------------------------------------------------------
    // 1. 目标快照
    // ------------------------------------------------------------------
    QString snapshotProblem;
    TargetSnapshot snapshot = readTargetSnapshot(request.instanceRoot, &snapshotProblem);
    if (!snapshotProblem.isEmpty()) {
        result.problem = snapshotProblem;
        return finish();
    }
    // 版本那一格由调用方（它认识 `VersionManager`）递进来；默认 true。
    snapshot.supportsDataPacks = request.supportsDataPacks;

    // ------------------------------------------------------------------
    // 2. 计划（只读）
    // ------------------------------------------------------------------
    PlanRequest planRequest;
    planRequest.presetFilesRoot = filesDir(presetsRoot, manifest.id);
    planRequest.sections = manifest.sections;
    planRequest.target = snapshot;
    planRequest.replaceConfirmed = request.replaceConfirmed;
    planRequest.gameRunning = request.gameRunning;

    const ApplyPlan plan = planPresetApplication(planRequest);
    if (!plan.ok) {
        result.problem = plan.problem;
        return finish();
    }
    result.keyConflicts = plan.keyConflicts;

    if (!plan.blockedReason.isEmpty()) {
        // 游戏在跑 —— 整趟不许开始（连备份都不做）。
        result.blockedReason = plan.blockedReason;
        result.rows.reserve(plan.rows.size());
        for (const ApplyPlanRow &one : plan.rows) {
            result.rows.append(rowFromPlan(one));
        }
        return finish();
    }

    if (plan.needsConfirm() && !request.replaceConfirmed) {
        // 第一趟：「覆盖」还没点头。**一个字节都不写**，把"哪几段在等你确认"报回去。
        result.needsConfirm = true;
        result.rows.reserve(plan.rows.size());
        for (const ApplyPlanRow &one : plan.rows) {
            result.rows.append(rowFromPlan(one));
        }
        return finish();
    }

    // ------------------------------------------------------------------
    // 3. 备份：把"这一趟会改写或删掉的、已经存在的"那几样先收走
    // ------------------------------------------------------------------
    //
    // 这一趟算的是"哪些会被动"，和下面第 4 步用的是**同一套判据**（适配器自己的
    // 计划函数）。不重算写出来的那一份，只重算"目标上现在有没有"。
    if (!writePresetExecutionReport(request.instanceRoot, sourceManifest, result, false, previous, &result.problem))
        return finish();
    reportStarted = true;
    QSet<QString> backupPaths;
    const QString optionsPath = request.instanceRoot + QStringLiteral("/options.txt");
    const QString serversPath = request.instanceRoot + QStringLiteral("/servers.dat");

    // ---------- options.txt：在内存里叠加一遍，看最后的字节变没变 ----------
    {
        OptionsFile running = snapshot.options;
        const QByteArray original = running.toBytes();
        for (const PresetSection &section : manifest.sections) {
            if (!optionsHandlesSection(section.kind)) {
                continue;
            }
            bool readable = false;
            const OptionsFile source = optionsFromSections({section}, &readable);
            if (!readable) {
                continue;  // 这一段下面会被报成 blocked，不备份
            }
            QSet<QString> selected;
            const QStringList keys = source.keys();
            for (const QString &key : keys) {
                selected.insert(key);
            }
            const OptionsApplyResult applied =
                applyOptions(section.kind, running, source, selected, section.mode, true);
            if (!applied.report.ok) {
                continue;
            }
            running = parseOptions(applied.bytes);
        }
        if (running.toBytes() != original && QFileInfo::exists(optionsPath)) {
            backupPaths.insert(QStringLiteral("options.txt"));
        }
    }

    // ---------- servers.dat ----------
    for (const PresetSection &section : manifest.sections) {
        if (!serverListHandlesSection(section.kind) || section.mode == ApplyMode::Skip) {
            continue;
        }
        QList<NbtUtil::ServerEntry> source;
        QString why;
        if (!serverListFromSection(section, &source, &why)) {
            continue;
        }
        const ServerListReport planned = planServerListApply(
            section.mode, snapshot.servers, snapshot.serversUntouchable, source, true);
        if (planned.ok && (planned.written > 0 || planned.removed > 0) &&
            QFileInfo::exists(serversPath)) {
            backupPaths.insert(QStringLiteral("servers.dat"));
        }
    }

    // ---------- 通用文件组：这一趟会被换成预设那一份的同名项 ----------
    const QString presetFilesRoot = filesDir(presetsRoot, manifest.id);
    for (const PresetSection &section : manifest.sections) {
        if (!genericFilesHandlesSection(section.kind) || section.mode == ApplyMode::Skip) {
            continue;
        }
        QList<GenericItem> items;
        QString why;
        if (!fileGroupFromSection(section, &items, &why)) {
            continue;
        }
        FileGroup group = FileGroup::Config;
        const bool idSays = fileGroupFromSectionId(section.id, &group);
        if (!idSays || items.isEmpty()) {
            continue;
        }
        const GenericPlan planned =
            planFileGroup(presetFilesRoot, items, request.instanceRoot, section.mode);
        if (!planned.ok) {
            continue;
        }
        for (int i = 0; i < items.size() && i < planned.items.size(); ++i) {
            if (planned.items.at(i).action != GenericAction::Replace) {
                continue;
            }
            const QString relative = items.at(i).relativePath;
            if (QFileInfo::exists(request.instanceRoot + QLatin1Char('/') + relative)) {
                backupPaths.insert(relative);
            }
        }
    }

    // ---------- MaLiLib：这一趟内容真的会变的那些文件 ----------
    for (const PresetSection &section : manifest.sections) {
        if (!malilibHandlesSection(section.kind) || section.mode == ApplyMode::Skip) {
            continue;
        }
        QList<MalilibEntry> entries;
        QString why;
        if (!malilibEntriesFromSections({section}, &entries, &why)) {
            continue;
        }
        QStringList modIds;
        for (const MalilibEntry &entry : entries) {
            if (!modIds.contains(entry.modId)) {
                modIds.append(entry.modId);
            }
        }
        const MalilibTarget target = malilibReadTarget(request.instanceRoot, modIds);
        const MalilibApplyResult applied = applyMalilibEntries(entries, target, section.mode, true);
        if (!applied.report.ok) {
            continue;
        }
        for (const MalilibFileResult &file : applied.files) {
            if (!file.changed) {
                continue;
            }
            const QString relative = QStringLiteral("config/%1.json").arg(file.modId);
            if (QFileInfo::exists(request.instanceRoot + QLatin1Char('/') + relative)) {
                backupPaths.insert(relative);
            }
        }
    }

    // ---------- 资源包启用顺序：这一段会改写 options.txt 的 resourcePacks 那一行 ----------
    for (const PresetSection &section : manifest.sections) {
        if (!genericFilesHandlesSection(section.kind) || section.mode == ApplyMode::Skip) {
            continue;
        }
        FileGroup group = FileGroup::Config;
        if (!fileGroupFromSectionId(section.id, &group) || group != FileGroup::ResourcePacks) {
            continue;
        }
        if (resourcePackSectionHasOrdering(section) && QFileInfo::exists(optionsPath)) {
            // 顺序真要变才改写；这儿不重算（`ResourcePackOptions::save()` 自己会判"没变
            // 就不写"），多备一份和"该备没备"比，前者只是多一个文件。
            backupPaths.insert(QStringLiteral("options.txt"));
        }
    }

    // ---------- 数据包：覆盖已有世界里的同名包 / 改写待应用清单 ----------
    for (const PresetSection &section : manifest.sections) {
        if (!dataPackHandlesSection(section) || section.mode == ApplyMode::Skip) {
            continue;
        }
        QList<DataPackItem> items;
        QString why;
        if (!dataPackItemsFromSection(section, &items, &why)) {
            continue;
        }
        const DataPackPlan planned =
            planDataPacks(request.instanceRoot, items, section.mode);
        if (!planned.ok) {
            continue;
        }
        if (section.mode == ApplyMode::Replace) {
            for (const DataPackPlanItem &item : planned.items) {
                if (!item.targetExists) {
                    continue;
                }
                const QString relative =
                    item.worldExists
                        ? QStringLiteral("saves/%1/datapacks/%2").arg(item.world, item.name)
                        : dataPackPendingDirName() + QLatin1Char('/') + item.world +
                              QLatin1Char('/') + item.name;
                backupPaths.insert(relative);
            }
        }
        if (planned.deferred > 0 &&
            QFileInfo::exists(request.instanceRoot + QLatin1Char('/') +
                              dataPackPendingFileName())) {
            backupPaths.insert(dataPackPendingFileName());
        }
    }

    if (!backupPaths.isEmpty()) {
        const QString instanceId = request.instanceId.isEmpty()
                                       ? QFileInfo(request.instanceRoot).fileName()
                                       : request.instanceId;
        const QString backupDir = makeBackupDir(presetsRoot, instanceId);
        if (backupDir.isEmpty()) {
            result.problem = applyText("无法建立备份目录，本次未修改这个实例"
                                       "（没有备份时不改动实例）。");
            return finish();
        }
        QStringList ordered = backupPaths.values();
        ordered.sort();
        for (const QString &relative : ordered) {
            QString why;
            if (!backupOne(request.instanceRoot, backupDir, relative, &why)) {
                result.problem = why;
                return finish();
            }
        }
        result.backupDir = backupDir;
    }

    // ------------------------------------------------------------------
    // 4. 执行
    // ------------------------------------------------------------------
    //
    // 一行一段，次序照 manifest。`options.txt` 那两段只在内存里叠，最后写一次。
    OptionsFile optionsTarget = snapshot.options;
    QList<int> optionsRowIndexes;
    bool optionsTouched = false;
    /// 资源包那一段应用完之后还要再动一次 options.txt（启用顺序），所以记住是哪一行。
    QList<QPair<int, PresetSection>> resourcePackRows;

    for (const PresetSection &section : manifest.sections) {
        PresetApplyRow row;
        if (!reportHealthy) return finish();
        row.sectionId = section.id;
        row.kind = section.kind;
        row.mode = section.mode;
        row.title = section.title.isEmpty() ? fallbackSectionTitle(section.kind) : section.title;

        if (section.mode == ApplyMode::Skip) {
            row.ok = true;
            row.skipped = true;
            result.rows.append(row); checkpoint(false);
            continue;
        }

        if (optionsHandlesSection(section.kind)) {
            bool readable = false;
            const OptionsFile source = optionsFromSections({section}, &readable);
            if (!readable) {
                row.blocked = true;
                row.problem = QCoreApplication::translate("Presets", "预设内容格式无效，已跳过此部分。");
                result.rows.append(row); checkpoint(false);
                continue;
            }
            QSet<QString> selected;
            const QStringList keys = source.keys();
            for (const QString &key : keys) {
                selected.insert(key);
            }
            const OptionsApplyResult applied = applyOptions(
                section.kind, optionsTarget, source, selected, section.mode,
                request.replaceConfirmed);
            if (!applied.report.ok) {
                row.blocked = true;
                row.problem = applied.report.problem;
                result.rows.append(row); checkpoint(false);
                continue;
            }
            optionsTarget = parseOptions(applied.bytes);
            optionsTouched = true;
            optionsRowIndexes.append(static_cast<int>(result.rows.size()));
            row.ok = true;
            row.written = applied.report.written;
            row.added = applied.report.added;
            row.removed = applied.report.removed;
            row.skippedItems = applied.report.skipped;
            for (const OptionsConflict &conflict : applied.report.conflicts) {
                row.details.append(QCoreApplication::translate("Presets", "按键「%1」被 %2 个动作占用。")
                                       .arg(conflict.binding)
                                       .arg(conflict.claimedBy()));
            }
            result.rows.append(row); checkpoint(false);
            continue;
        }

        if (serverListHandlesSection(section.kind)) {
            QList<NbtUtil::ServerEntry> source;
            QString why;
            if (!serverListFromSection(section, &source, &why)) {
                row.blocked = true;
                row.problem = why;
                result.rows.append(row); checkpoint(false);
                continue;
            }
            const ServerListReport applied = applyServerListToInstance(
                serversPath, section.mode, source, request.replaceConfirmed);
            row.ok = applied.ok;
            row.blocked = !applied.ok;
            row.problem = applied.problem;
            row.written = applied.written;
            row.added = applied.added;
            row.removed = applied.removed;
            row.skippedItems = applied.skipped;
            if (applied.skipped > 0) {
                row.details.append(QCoreApplication::translate("Presets", "以下服务器已在列表中：%1")
                                       .arg(applied.skippedAddresses.join(QStringLiteral("、"))));
            }
            result.rows.append(row); checkpoint(false);
            continue;
        }

        if (genericFilesHandlesSection(section.kind)) {
            QList<GenericItem> items;
            QString why;
            if (!fileGroupFromSection(section, &items, &why)) {
                row.blocked = true;
                row.problem = why;
                result.rows.append(row); checkpoint(false);
                continue;
            }
            FileGroup group = FileGroup::Config;
            if (!fileGroupFromSectionId(section.id, &group)) {
                if (items.isEmpty()) {
                    row.blocked = true;
                    row.problem = QCoreApplication::translate("Presets", "无法识别此部分所属的文件组，已跳过。");
                    result.rows.append(row); checkpoint(false);
                    continue;
                }
                // 路径自己说了算（`fileGroupOfPath()` 已经有那一口；这里按第一项推）。
                if (!fileGroupOfPath(items.at(0).relativePath, &group)) {
                    row.blocked = true;
                    row.problem = QCoreApplication::translate("Presets", "此部分包含文件组范围以外的路径，已跳过。");
                    result.rows.append(row); checkpoint(false);
                    continue;
                }
            }
            // 按**段里这份清单**落，不能重扫预设的 `files/<组>`：盘上多出来的旧文件
            // 也会被应用，而计划表和备份范围都是按清单算的 —— 于是"落下去的那一份"
            // 不在备份里（见 `applyFileGroup(…, items)` 那段）。
            const GenericReport applied = applyFileGroup(presetFilesRoot, request.instanceRoot,
                                                         group, section.mode, items);
            row.ok = applied.ok;
            row.blocked = !applied.ok;
            row.problem = applied.problem;
            row.written = applied.replaced + applied.copied;
            row.skippedItems = applied.skipped;
            for (const QString &one : applied.refused) {
                row.details.append(one);
            }
            if (applied.needsChoice > 0) {
                row.details.append(QCoreApplication::translate("Presets", "当前模式无法处理此组中的 %1 项，已跳过。")
                                       .arg(applied.needsChoice));
            }
            if (applied.ok && group == FileGroup::ResourcePacks &&
                resourcePackSectionHasOrdering(section)) {
                // 留到**最后**再动：`options.txt` 那几段也在内存里叠着，谁先写谁被盖。
                resourcePackRows.append(qMakePair(static_cast<int>(result.rows.size()), section));
            }
            result.rows.append(row); checkpoint(false);
            continue;
        }

        if (dataPackHandlesSection(section)) {
            if (row.title.isEmpty()) {
                row.title = QCoreApplication::translate("Presets", "数据包");
            }
            if (!request.supportsDataPacks) {
                row.blocked = true;
                row.problem = applyText("这个实例的游戏版本还没有数据包（17w43a 之前），"
                                        "这一段这一趟不动。");
                result.rows.append(row); checkpoint(false);
                continue;
            }
            const DataPackApplyReport applied = applyDataPacks(
                presetFilesRoot, request.instanceRoot, section, section.mode,
                request.replaceConfirmed);
            row.ok = applied.ok;
            row.blocked = !applied.ok;
            row.problem = applied.problem;
            row.written = applied.deployed;
            row.skippedItems = applied.skipped;
            row.deferred = applied.deferred;
            row.details.append(applied.details);
            if (applied.needsChoice > 0) {
                row.skippedItems += applied.needsChoice;
            }
            result.rows.append(row); checkpoint(false);
            continue;
        }

        if (malilibHandlesSection(section.kind)) {
            QList<MalilibEntry> entries;
            QString why;
            if (!malilibEntriesFromSections({section}, &entries, &why)) {
                row.blocked = true;
                row.problem = why;
                result.rows.append(row); checkpoint(false);
                continue;
            }
            const MalilibApplyReport applied = applyMalilibToInstance(
                request.instanceRoot, entries, section.mode, request.replaceConfirmed);
            row.ok = applied.ok;
            row.blocked = !applied.ok;
            row.problem = applied.problem;
            // 这一格报的是"**真改了几个**"：`written` 里含"目标上本来就是那个值"的那些
            // （`MalilibKeys.hpp` 里那条口径，和原版设置那边同源）。那一档一个字节都没写
            // —— 同一份预设连落两趟时，照抄 `applied.written` 会让界面/日志说"写入 N 项"，
            // 而盘上一个字都没动（options 那一段在下面专门归过一次零，理由一样）。
            row.written = applied.written - applied.unchanged;
            row.added = applied.added;
            row.removed = applied.removed;
            row.skippedItems = applied.skipped;
            row.details.append(applied.details);
            if (applied.typeMismatch > 0) {
                row.details.append(QCoreApplication::translate("Presets", "有 %1 个值的类型不匹配，已跳过转换。")
                                       .arg(applied.typeMismatch));
            }
            result.rows.append(row); checkpoint(false);
            continue;
        }

        row.blocked = true;
        row.problem = QCoreApplication::translate("Presets", "此部分的格式不受支持，已跳过。");
        result.rows.append(row); checkpoint(false);
    }

    // ---------- options.txt：最后写一次 ----------
    if (!reportHealthy) return finish();
    if (optionsTouched) {
        const QByteArray bytes = optionsTarget.toBytes();
        if (bytes != snapshot.options.toBytes()) {
            QString why;
            if (!writeWholeFile(optionsPath, bytes, &why)) {
                for (int index : optionsRowIndexes) {
                    PresetApplyRow &row = result.rows[index];
                    row.ok = false;
                    row.blocked = true;
                    row.problem = why;
                }
            }
        } else {
            // 字节一个都没变（同一份预设连着落两趟就是这个样子）：适配器报的"N 项"是
            // "这一段管着几个键"，不是"盘上改了几个键"。在这儿归零，让
            // `wroteAnything()` 在"什么都没变"的那一趟说实话 —— 界面和日志都拿它判
            // "这一趟办完了没有"，而"写了 2 项"在一份没动的文件上是假话。
            for (int index : optionsRowIndexes) {
                PresetApplyRow &row = result.rows[index];
                row.written = 0;
                row.added = 0;
                row.removed = 0;
            }
        }
    }

    // ---------- 资源包启用顺序（必须在 options.txt 最后那一次写之后） ----------
    //
    // 顺序是**低优先级在前**，写的是最后一条 `resourcePacks:`。这一段单独走是因为它
    // 和 `options.txt` 那两段落在同一个文件上：先写它再写选项，那几行就被盖掉了。
    if (!resourcePackRows.isEmpty()) {
        const QString instanceIdForBackup =
            request.instanceId.isEmpty() ? QFileInfo(request.instanceRoot).fileName()
                                         : request.instanceId;
        const QString packBackupRoot = presetsRoot + QStringLiteral("/backups/") +
                                       instanceIdForBackup +
                                       QStringLiteral("/resource-packs");
        for (const auto &one : resourcePackRows) {
            PresetApplyRow &row = result.rows[one.first];
            if (row.blocked) {
                continue;
            }
            const ResourcePackOrderReport order =
                applyResourcePackOrdering(request.instanceRoot, one.second, packBackupRoot);
            row.details.append(order.details);
            if (!order.ok) {
                row.ok = false;
                row.blocked = true;
                row.problem = order.problem.isEmpty()
                                  ? QCoreApplication::translate("Presets", "资源包启用顺序未写入。")
                                  : order.problem;
            }
        }
    }

    for (const PresetApplyRow &row : result.rows) {
        result.written += row.written;
        result.removed += row.removed;
        result.deferred += row.deferred;
    }
    // `ok` 是"**这一趟办完了没有**"，不是"流程走到了末尾"：
    // 无条件置真的话，某一段写盘失败（servers.dat 被占用、options.txt 提交失败、
    // 通用文件组中途拷不动……）时，管理页顶上照样写「应用完成。」、日志照样记
    // "写入 N 项"，而面板里某一行明写着"做不了"。`Skip` 那几行是**办完了**（不算失败）。
    result.ok = true;
    for (const PresetApplyRow &row : result.rows) {
        if (row.blocked) {
            result.ok = false;
            break;
        }
    }
    return finish();
}

}  // namespace Presets

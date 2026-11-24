#include "InstanceFileController.hpp"
#include <QCoreApplication>

#include "InlineMessage.hpp"
#include "InstanceFilesPage.hpp"
#include "OverlayHost.hpp"
#include "manage/FolderManager.hpp"
#include "util/WriteDirectoryGuard.hpp"
#include "util/AppPaths.hpp"
#include "operations/OperationReport.hpp"
#include <QUuid>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSet>
#include <QSaveFile>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <utility>

namespace {

/// 一条 junction / 符号链接。**两个都要问**：`isSymLink()` 对 junction 是 `false`
/// （见 [[wtfml-junction-facts]]），只判那头会把 junction 漏成一个普通文件夹。
bool isLinkEntry(const QFileInfo &info) {
    return info.isSymLink() || info.isJunction();
}

bool plainLifecyclePath(const QString &path) {
    if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path) return false;
    for (QString cursor = path;;) {
        const QFileInfo info(cursor);
        if (!info.exists() || isLinkEntry(info)) return false;
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(cursor);
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        if (QDir(cursor).isRoot()) return true;
        if (!InstanceFiles::validatePathSegment(info.fileName()).isEmpty()) return false;
        const QString parent = info.absolutePath();
        if (parent == cursor) return false;
        cursor = parent;
    }
}

/// 单条删除那一问。★ ：链接**不能**按文件夹写 —— `removeUnasked()` 对目录走
/// `moveToRecycleBin()`，它碰到链接只摘掉链接本身、指向的东西一个字节都不动；
/// 而 `QFileInfo::isDir()` 对 junction 是 `true`。照着 `isDir()` 写这一问，用户看到的是
/// 「连同里面的东西一起删掉」，实际删掉的是个两字节的壳子 —— 反过来更糟：用户以为链接背后
/// 那棵大树还在、其实（如果他自己以为是删文件）也没有。说什么就得做什么。
QString removalQuestionFor(const QFileInfo &info) {
    const QString name = info.fileName();
    if (isLinkEntry(info)) {
        return QObject::tr("确定删除符号链接「%1」？仅移除链接，目标文件不受影响。")
            .arg(name);
    }
    if (info.isDir()) {
        return QObject::tr("确定删除文件夹「%1」及其所有内容？文件将移至回收站。")
            .arg(name);
    }
    return QObject::tr("确定删除「%1」？文件将移至回收站。").arg(name);
}

}  // namespace

InstanceFileController::InstanceFileController(OverlayHost *overlay, Host host, QObject *parent)
    : QObject(parent), m_overlay(overlay), m_host(std::move(host)) {}

void InstanceFileController::log(const QString &text) const {
    if (m_host.log) m_host.log(text);
}

QString InstanceFileController::firstRefusal(const QStringList &paths) const {
    if (!m_host.refuseReason) return QString();
    for (const QString &path : paths) {
        const QString why = m_host.refuseReason(path);
        if (!why.isEmpty()) return why;
    }
    return QString();
}

QStringList InstanceFileController::beginFileOperation(const QStringList &paths) {
    QStringList ids;
    if (!m_host.instanceIdOfPath || !m_host.setFileOperationBusy) return ids;
    for (const QString &path : paths) {
        const QString id = m_host.instanceIdOfPath(path);
        if (id.isEmpty() || ids.contains(id)) continue;
        ids.append(id);
        m_host.setFileOperationBusy(id, true);
    }
    return ids;
}

void InstanceFileController::endFileOperation(const QStringList &ids) {
    if (!m_host.setFileOperationBusy) return;
    for (const QString &id : ids) m_host.setFileOperationBusy(id, false);
}

bool InstanceFileController::toggle(const QString &path, QString *problem) {
    if (const QString why = firstRefusal({path}); !why.isEmpty()) {
        if (problem) *problem = why;
        else InlineMessage::warning(m_overlay, tr("改动失败"), why);
        return false;
    }
    return toggleUnchecked(path, problem);
}

bool InstanceFileController::toggleUnchecked(const QString &path, QString *problem) {
    const auto fail = [this, problem](const QString &text) {
        if (problem) *problem = text;
        else InlineMessage::warning(m_overlay, tr("改动失败"), text);
        return false;
    };
    const QFileInfo info(path);
    if (!info.exists())
        return fail(tr("找不到「%1」。").arg(QDir::toNativeSeparators(path)));
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(path, &lockProblem)) return fail(lockProblem);
    if (info.isDir())
        return fail(tr("「%1」是一个文件夹，不能启用 / 禁用。\n\n（把一个文件夹改了名，它里面的东西会一起从游戏眼里消失。）")
                        .arg(QDir::toNativeSeparators(path)));
    const QString suffix = InstanceFilesPage::disabledSuffix();
    const QString target = info.fileName().endsWith(suffix, Qt::CaseInsensitive)
                               ? path.left(path.size() - suffix.size()) : path + suffix;
    if (QFileInfo::exists(target))
        return fail(tr("目标「%1」已存在。请先重命名或删除现有项目。")
                        .arg(QDir::toNativeSeparators(target)));
    if (!QFile::rename(path, target))
        return fail(tr("无法重命名「%1」。请关闭占用文件的程序后重试。")
                        .arg(QDir::toNativeSeparators(path)));
    log(target.endsWith(suffix, Qt::CaseInsensitive)
            ? tr("禁用文件：%1").arg(QDir::toNativeSeparators(path))
            : tr("启用文件：%1").arg(QDir::toNativeSeparators(target)));
    return true;
}

bool InstanceFileController::remove(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        InlineMessage::warning(m_overlay, tr("删除失败"),
                               tr("找不到「%1」。").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    if (const QString why = firstRefusal({path}); !why.isEmpty()) {
        InlineMessage::warning(m_overlay, tr("删除失败"), why);
        return false;
    }
    const QString question = removalQuestionFor(info);
    if (!InlineMessage::question(m_overlay, tr("删除文件"), question, tr("删除"), tr("取消"),
                                 false, true)) return false;
    // 确认框可能开着很久。开头那道闸只管"按下去那一刻"，期间另一项文件操作或游戏
    // 完全可能已经开始；动盘之前按同一套判据再问一次。
    if (const QString why = firstRefusal({path}); !why.isEmpty()) {
        InlineMessage::warning(m_overlay, tr("删除失败"), why);
        return false;
    }
    // 目录删除可能递归很久，期间不能再让备份 / 快照开始读同一棵树。
    const QStringList ids = beginFileOperation({path});
    const bool ok = removeUnchecked(path, nullptr);
    endFileOperation(ids);
    return ok;
}

bool InstanceFileController::removeUnasked(const QString &path, QString *problem) {
    if (const QString why = firstRefusal({path}); !why.isEmpty()) {
        if (problem) *problem = why;
        else InlineMessage::warning(m_overlay, tr("删除失败"), why);
        return false;
    }
    return removeUnchecked(path, problem);
}

bool InstanceFileController::removeUnchecked(const QString &path, QString *problem) {
    const auto fail = [this, problem](const QString &text) {
        if (problem) *problem = text;
        else InlineMessage::warning(m_overlay, tr("删除失败"), text);
        return false;
    };
    const QFileInfo info(path);
    if (!info.exists())
        return fail(tr("找不到「%1」。").arg(QDir::toNativeSeparators(path)));
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(path, &lockProblem)) return fail(lockProblem);
    // 进**回收站**：文件页上的东西（模组、资源包、存档里的一份文件）都是用户自己攒的，
    // 点错了得有地方捞回来。目录那条路连同里面一起移，链接只摘壳子 —— 两条约定都在
    // `moveToRecycleBin()` 里（见 FolderManager.hpp）。
    QString recycleProblem;
    // 原因由 `moveToRecycleBin()` 给全（权限 / 被占着 / 回收站本身），这里只包一层
    // 「是哪一件」—— 不再替它补一句「先把游戏关掉再试」：权限不够的时候那句话是错的。
    if (!moveToRecycleBin(path, &recycleProblem))
        return fail(tr("无法将「%1」移入回收站。\n\n%2")
                        .arg(QDir::toNativeSeparators(path), recycleProblem));
    log(tr("删除文件：%1").arg(QDir::toNativeSeparators(path)));
    return true;
}

namespace {
/// 批量文件操作也走统一的 `OperationReport`（2.1.2）：逐项结果与“为什么没做完”只有一套说法。
void recordBatchReport(const QString &instanceId, const QString &title,
                       const QList<OperationReport::Entry> &entries) {
    if (title.isEmpty() || entries.isEmpty()) return;
    OperationReport::Report report;
    report.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    report.kind = OperationReport::Kind::Batch;
    report.title = title;
    report.instanceId = instanceId;
    report.targetRoot = instanceId.isEmpty() ? QString() : AppPaths::versionDir(instanceId);
    report.totalFiles = entries.size();
    report.startedAt = QDateTime::currentDateTime();
    report.updatedAt = report.startedAt;
    for (const auto &entry : entries) {
        report.entries.append(entry);
        if (entry.state == OperationReport::EntryState::Done) ++report.doneFiles;
        else if (entry.state == OperationReport::EntryState::Skipped) ++report.skippedFiles;
        else if (entry.state == OperationReport::EntryState::Failed) ++report.failedFiles;
    }
    report.complete = report.failedFiles == 0;
    if (!report.complete) {
        report.failureReason = QObject::tr("有 %1 项没有完成。").arg(report.failedFiles);
    }
    const QString path = OperationReport::reportPath(AppPaths::minecraftDir(), report.id);
    QString problem;
    if (OperationReport::saveReport(path, report, &problem) && !report.complete) {
        OperationReport::writeFailureText(path, report);
    }
}
}  // namespace

void InstanceFileController::toggleMany(const QStringList &paths, bool disable) {
    if (paths.isEmpty()) return;
    if (const QString why = firstRefusal(paths); !why.isEmpty()) {
        InlineMessage::warning(m_overlay, tr("改动失败"), why);
        return;
    }
    const QString suffix = InstanceFilesPage::disabledSuffix();
    const QStringList ids = beginFileOperation(paths);
    int changed = 0, failed = 0;
    QStringList problems;
    QList<OperationReport::Entry> entries;
    for (const QString &path : paths) {
        if (path.endsWith(suffix, Qt::CaseInsensitive) == disable) {
            entries.append(OperationReport::Entry{
                QFileInfo(path).fileName(), QFileInfo(path).size(),
                OperationReport::EntryState::Skipped, QObject::tr("已是目标状态")});
            continue;
        }
        QString problem;
        if (toggleUnchecked(path, &problem)) {
            ++changed;
            entries.append(OperationReport::Entry{
                QFileInfo(path).fileName(), QFileInfo(path).size(),
                OperationReport::EntryState::Done, QString()});
        } else {
            ++failed;
            if (!problem.isEmpty() && !problems.contains(problem)) problems.append(problem);
            entries.append(OperationReport::Entry{
                QFileInfo(path).fileName(), QFileInfo(path).size(),
                OperationReport::EntryState::Failed, problem});
        }
    }
    endFileOperation(ids);
    recordBatchReport(ids.isEmpty() ? QString() : ids.constFirst(),
                      disable ? tr("批量禁用 %1 项").arg(paths.size())
                              : tr("批量启用 %1 项").arg(paths.size()),
                      entries);
    log(tr("%1：动了 %2 个文件。").arg(disable ? tr("批量禁用") : tr("批量启用")).arg(changed));
    if (failed > 0)
        InlineMessage::warning(m_overlay, tr("改动失败"),
                               tr("有 %1 个文件修改失败。\n\n%2")
                                   .arg(failed).arg(problems.join(QStringLiteral("\n"))));
}

void InstanceFileController::removeMany(const QStringList &paths) {
    if (paths.isEmpty()) return;
    if (const QString why = firstRefusal(paths); !why.isEmpty()) {
        InlineMessage::warning(m_overlay, tr("删除失败"), why);
        return;
    }
    // 这一批里**可能有文件夹**。资源包 / 数据包 / 光影包那三页连目录一起列
    // （未压缩的目录包正是游戏认可的一种形态），而 `removeUnasked()` 对目录是**连里面的
    // 东西一起递归移**（`moveToRecycleBin`）。这一问若无论收到什么都
    // 写「这 N 个文件」，唯一的确认框就把实际破坏范围说小了，用户以为只删一个壳子。
    // 单条那条路（`remove()`）是分开写的，批量这条路也要照办。
    // 链接单独数：它们**不**是"里面的东西一起删"，只摘壳子（见 `removalQuestionFor()`）。
    int dirCount = 0;
    int linkCount = 0;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (isLinkEntry(info)) ++linkCount;
        else if (info.isDir()) ++dirCount;
    }
    const QString question = paths.size() == 1
        ? removalQuestionFor(QFileInfo(paths.first()))
        : (linkCount == paths.size()
               ? tr("确定删除选中的 %1 个链接？目标文件不受影响。")
                     .arg(paths.size())
               : (dirCount == 0 && linkCount == 0
                      ? tr("确定删除选中的 %1 个文件？文件将移至回收站。")
                            .arg(paths.size())
                      : (linkCount == 0 && dirCount == paths.size()
                             ? tr("确定删除选中的 %1 个文件夹及其内容？文件将移至回收站。")
                                   .arg(paths.size())
                             : [&] {
                                   QString text =
                                       tr("确定删除选中的 %1 项？").arg(paths.size());
                                   if (dirCount > 0)
                                       text += tr("其中 %1 个是文件夹（连同里面的东西一起删）")
                                                   .arg(dirCount);
                                   if (linkCount > 0) {
                                       if (dirCount > 0) text += tr("，");
                                       text += tr("%1 个是链接（只删链接本身，指向的东西不动）")
                                                   .arg(linkCount);
                                   }
                                   return text + tr("。它们会进回收站，要恢复可以从那里找回来。");
                               }())));
    if (InlineMessage::ask(m_overlay, MessageKind::Question, tr("删除文件"), question,
                           {tr("删除"), tr("取消")}, 1, 0) != 0) return;
    // 和单条删除同一条理由：确认框期间可能已经有别的操作开始，动盘前再问一次。
    if (const QString why = firstRefusal(paths); !why.isEmpty()) {
        InlineMessage::warning(m_overlay, tr("删除失败"), why);
        return;
    }
    int removed = 0, failed = 0;
    QStringList problems;
    QList<OperationReport::Entry> entries;
    const QStringList ids = beginFileOperation(paths);
    for (const QString &path : paths) {
        QString problem;
        if (removeUnchecked(path, &problem)) {
            ++removed;
            entries.append(OperationReport::Entry{
                QFileInfo(path).fileName(), QFileInfo(path).size(),
                OperationReport::EntryState::Done, QString()});
        } else {
            ++failed;
            if (!problem.isEmpty() && !problems.contains(problem)) problems.append(problem);
            entries.append(OperationReport::Entry{
                QFileInfo(path).fileName(), QFileInfo(path).size(),
                OperationReport::EntryState::Failed, problem});
        }
    }
    endFileOperation(ids);
    recordBatchReport(ids.isEmpty() ? QString() : ids.constFirst(),
                      tr("批量删除 %1 项").arg(paths.size()), entries);
    // 「项」而不是「个文件」：这一批里可能混着文件夹（见上面那一问）。
    log(tr("批量删除：删掉了 %1 项。").arg(removed));
    if (failed > 0)
        InlineMessage::warning(m_overlay, tr("删除失败"),
                               tr("%1 项删除失败：\n\n%2")
                                   .arg(failed).arg(problems.join(QStringLiteral("\n"))));
}

InstanceFileController::LifecycleResult InstanceFileController::applyLifecycleBatch(
    LifecycleAction action, const QString &sourceRoot, const QStringList &paths, const QString &destination,
    bool nested) {
    LifecycleResult result;
    const bool transferring = action != LifecycleAction::Remove;
    const auto invalidSource = [](const QString &path) {
        return tr("源文件无效、不在列表目录内或经过链接：%1").arg(QDir::toNativeSeparators(path));
    };
    if (paths.isEmpty() || paths.size() > 128) {
        result.problem = tr("每批可操作 1 至 128 个文件。");
        return result;
    }
    if (!QFileInfo(sourceRoot).isDir() || !plainLifecyclePath(sourceRoot)) {
        result.problem = tr("列表目录不存在或经过链接，未改动任何文件。");
        return result;
    }
    if (transferring && (!QFileInfo(destination).isDir() || !plainLifecyclePath(destination)
                         || InstanceFiles::canonicalPath(sourceRoot).compare(
                                InstanceFiles::canonicalPath(destination), Qt::CaseInsensitive) == 0)) {
        result.problem = tr("请选择另一个已存在且不经过链接的目标文件夹。");
        return result;
    }

    QStringList affected;
    QStringList targets;
    QSet<QString> names;
    qint64 bytes = 0;
    constexpr qint64 copyLimit = 512LL * 1024 * 1024;
    const auto sourceAllowed = [&sourceRoot, nested](const QString &path) {
        const QFileInfo info(path);
        const bool inside = nested
            ? info.absolutePath().startsWith(sourceRoot + QLatin1Char('/')) && path.contains(QStringLiteral("/generated/"))
            : info.absolutePath() == sourceRoot;
        return info.isFile() && inside && plainLifecyclePath(path);
    };
    const auto targetOccupied = [](const QString &path) {
        const QFileInfo info(path);
        return info.exists() || isLinkEntry(info);
    };
    for (const QString &rawPath : paths) {
        const QString path = QDir::fromNativeSeparators(rawPath);
        const QFileInfo info(path);
        const QString name = info.fileName();
        // 子目录来源里不同存档可以有同名文件：删除按完整路径判重，复制 / 移动按文件名判重（落在同一层）。
        const QString identity = nested && !transferring ? path.toCaseFolded() : name.toCaseFolded();
        if (!sourceAllowed(path)) {
            result.problem = invalidSource(path);
            return result;
        }
        if (names.contains(identity)) {
            result.problem = nested && transferring
                ? tr("所选文件中有重名文件，请分批操作：%1").arg(name)
                : invalidSource(path);
            return result;
        }
        names.insert(identity);
        affected.append(path);
        if (transferring) {
            const QString target = QDir(destination).filePath(name);
            if (targetOccupied(target)) {
                result.problem = tr("目标已存在，不覆盖同名文件：%1").arg(QDir::toNativeSeparators(target));
                return result;
            }
            targets.append(target);
        }
        if (action == LifecycleAction::Copy) {
            const qint64 size = info.size();
            if (size > copyLimit - bytes) {
                result.problem = tr("每批最多复制 512 MiB。请减少所选文件，或在资源管理器中复制大文件。");
                return result;
            }
            bytes += size;
        }
    }
    QStringList guardedPaths = affected;
    if (transferring) {
        guardedPaths.append(targets);
        const QDir resolved(InstanceFiles::canonicalPath(destination));
        if (resolved.path() != destination) {
            for (const QString &target : targets)
                guardedPaths.append(resolved.filePath(QFileInfo(target).fileName()));
        }
    }
    result.problem = firstRefusal(guardedPaths);
    if (!result.problem.isEmpty()) return result;
    if (!WriteDirectoryGuard::acquire(sourceRoot, &result.problem)
        || (transferring && !WriteDirectoryGuard::acquire(destination, &result.problem))) return result;

    const auto ids = beginFileOperation(guardedPaths);
    const auto release = qScopeGuard([this, &ids] { endFileOperation(ids); });
    for (qsizetype index = 0; index < affected.size(); ++index) {
        const QString &path = affected.at(index);
        if (!sourceAllowed(path)) {
            result.problem = invalidSource(path);
            break;
        }
        if (transferring) {
            const QString &target = targets.at(index);
            if (!plainLifecyclePath(destination) || targetOccupied(target)) {
                result.problem = tr("目标目录或同名项发生变化，未继续操作：%1").arg(QDir::toNativeSeparators(target));
                break;
            }
            if (action == LifecycleAction::Copy) {
                QFile source(path);
                if (!source.copy(target)) {
                    result.problem = tr("无法复制「%1」：%2").arg(QFileInfo(path).fileName(), source.errorString());
                    break;
                }
            } else {
#ifdef Q_OS_WIN
                const QString from = QDir::toNativeSeparators(path);
                const QString to = QDir::toNativeSeparators(target);
                if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                                 reinterpret_cast<LPCWSTR>(to.utf16()), 0)) {
                    const DWORD error = GetLastError();
                    result.problem = error == ERROR_NOT_SAME_DEVICE
                        ? tr("仅支持同卷移动。跨盘请先复制，核对目标文件后再处理原件。")
                        : tr("无法移动「%1」（Windows 错误 %2）。原文件未移除。")
                              .arg(QFileInfo(path).fileName()).arg(error);
                    break;
                }
#else
                result.problem = tr("此平台不支持安全移动。");
                break;
#endif
            }
        } else if (!moveToRecycleBin(path, &result.problem)) {
            result.problem = tr("无法将「%1」移入回收站：%2").arg(QFileInfo(path).fileName(), result.problem);
            break;
        }
        ++result.done;
    }
    return result;
}

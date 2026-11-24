#pragma once

#include "manage/RestoreJournal.hpp"
#include "util/DirectorySessionLock.hpp"
#include <QLockFile>

// Inspection is read-only. Acknowledging archives only our journal, never user data.
namespace RecoveryReview {
struct Item {
    QString journal;
    QString target;
    QString retained;
    QString problem;
};
inline Item read(const QString &root, const QString &path) {
    Item item{path, {}, {}, {}};
    QFile file(path);
    if (!RestoreJournal::ordinaryPath(root, path)
        || !file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        item.problem = QStringLiteral("日志不可读取或路径不安全；请人工检查，不自动清理。");
        return item;
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()
        || !RestoreJournal::valid(root, doc.object())) {
        item.problem = QStringLiteral("日志格式或目录边界不合法；不提供自动处理。");
        return item;
    }
    item.target = doc.object().value(QStringLiteral("target")).toString();
    item.retained = doc.object().value(QStringLiteral("retained")).toString();
    return item;
}
inline QList<Item> inspect(const QString &gameDir) {
    QList<Item> items;
    const QString root = InstanceFiles::canonicalPath(gameDir);
    const QString directory = RestoreJournal::directory(root);
    if (!RestoreJournal::ordinaryPath(root, directory)) return items;
    const QDir dir(directory);
    const auto names = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto &name : names.mid(0, 128)) {
        items.append(read(root, dir.filePath(name)));
    }
    return items;
}
inline bool acknowledge(const QString &gameDir, const Item &expected, QString *problem) {
    const auto fail = [&](const QString &text) { if (problem) *problem = text; return false; };
    if (problem) problem->clear();
    const QString root = InstanceFiles::canonicalPath(gameDir);
    if (!DirectorySessionLock::acquire(root, problem)) return false;
    const QFileInfo journal(expected.journal);
    if (journal.absolutePath() != RestoreJournal::directory(root)
        || QUuid(journal.completeBaseName()).isNull()
        || !RestoreJournal::ordinaryPath(root, expected.journal + QStringLiteral(".lock")))
        return fail(QStringLiteral("日志位置不安全，未处理。"));
    QLockFile lock(expected.journal + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return fail(QStringLiteral("还原事务仍在使用中，未处理。"));
    const Item current = read(root, expected.journal);
    if (!current.problem.isEmpty() || current.target != expected.target
        || current.retained != expected.retained || current.target.isEmpty()
        || !QFileInfo(current.target).isDir() || !QFileInfo(current.retained).isDir())
        return fail(QStringLiteral("目录或日志已变化，或两份目录未同时保留，请重新检查。"));
    const QString archiveDir = QDir(root).filePath(QStringLiteral("cache/restore-reviewed"));
    const QString archive = QDir(archiveDir).filePath(journal.fileName());
    if (!RestoreJournal::ordinaryPath(root, archive) || QFileInfo::exists(archive)
        || !QDir().mkpath(archiveDir) || !QFile::rename(expected.journal, archive))
        return fail(QStringLiteral("检查记录未能归档，原日志和两份目录均保留。"));
    return true;
}
} // namespace RecoveryReview

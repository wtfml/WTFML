#pragma once
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

namespace DownloadCache {
inline QString root() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/range-downloads"); }
enum class Action { Inspect, Trim, ClearIdle, ClearExpired };
struct Result { qint64 bytes = 0, removed = 0; int entries = 0, busy = 0, failed = 0; bool incomplete = false; };
// Only this downloader's fixed names are eligible. Never recurse or follow reparse points.
inline bool managedFile(const QString &name) {
    static const QRegularExpression part(QStringLiteral("^part-[0-9]+\\.bin$"));
    return name == QStringLiteral("manifest.json") || part.match(name).hasMatch();
}
inline Result run(Action action, qint64 limit, const QString &directory = root()) {
    Result result;
    const QFileInfo base(directory);
    if (!base.exists()) return result;
    if (!base.isDir() || base.isSymLink() || base.isJunction()) { result.failed = 1; return result; }
    QLockFile management(QDir(directory).filePath(QStringLiteral("management.lock")));
    if (!management.tryLock(0)) { result.busy = 1; return result; }
    struct Entry { QString path; qint64 size; QDateTime changed; };
    QList<Entry> entries;
    QElapsedTimer timer; timer.start();
    static const QRegularExpression key(QStringLiteral("^[a-f0-9]{64}$"));
    QDirIterator dirs(directory, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    int inspected = 0;
    while (dirs.hasNext()) {
        dirs.next();
        if (++inspected > 10000 || timer.elapsed() > 10000) { result.incomplete = true; break; }
        const QFileInfo folder = dirs.fileInfo();
        if (!key.match(folder.fileName()).hasMatch() || folder.isSymLink() || folder.isJunction()) continue;
        qint64 size = 0; QDateTime changed = folder.lastModified();
        QDirIterator files(folder.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks);
        while (files.hasNext()) {
            files.next();
            if (++inspected > 10000 || timer.elapsed() > 10000) { result.incomplete = true; break; }
            const auto file = files.fileInfo();
            if (!managedFile(file.fileName()) || file.isSymLink() || file.isJunction()) continue;
            size += file.size(); changed = qMax(changed, file.lastModified());
        }
        result.bytes += size; ++result.entries;
        entries.append({folder.absoluteFilePath(), size, changed});
        if (result.incomplete) break;
    }
    if (action == Action::Inspect || result.incomplete) return result;
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.changed < b.changed; });
    for (const auto &entry : entries) {
        if (timer.elapsed() > 10000) { result.incomplete = true; break; }
        if (action == Action::Trim && result.bytes <= qMax<qint64>(0, limit)) break;
        if (action == Action::ClearExpired && entry.changed > QDateTime::currentDateTimeUtc().addDays(-30)) continue;
        const QFileInfo folder(entry.path);
        if (folder.isSymLink() || folder.isJunction()) { ++result.failed; continue; }
        QLockFile lock(QDir(entry.path).filePath(QStringLiteral("lock")));
        if (!lock.tryLock(0)) { ++result.busy; continue; }
        QDirIterator files(entry.path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks);
        while (files.hasNext()) {
            files.next();
            const auto file = files.fileInfo();
            if (!managedFile(file.fileName()) || file.isSymLink() || file.isJunction()) continue;
            const qint64 bytes = file.size();
            if (QFile::remove(file.absoluteFilePath())) { result.bytes -= bytes; result.removed += bytes; }
            else ++result.failed;
        }
        lock.unlock();
        QDir().rmdir(entry.path); // Empty only; unknown files stay untouched.
    }
    return result;
}
}

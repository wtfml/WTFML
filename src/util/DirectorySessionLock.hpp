#pragma once

#include "manage/InstanceFiles.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>
#include <memory>

/// 只在产品 main 中启用。持有到进程退出，避免切目录后仍在收尾的任务失去保护。
namespace DirectorySessionLock {
inline bool enabled = false;
inline QHash<QString, std::shared_ptr<QLockFile>> held;
inline QMutex mutex;

/// 「不够久就当作还在跑」的窗口。
///
/// 为什么**不能**是 0：`QLockFilePrivate::isApparentlyStale()` 只在 `staleLockTime > 0`
/// **且**文件比它旧时才判陈旧。给 0 的效果是"永不过期" —— 启动器被任务管理器结束 /
/// 崩溃 / 断电一次之后，这个数据目录会一直报「已被另一份启动器占用」，而真正的文件是
/// `%TEMP%` 里一串 SHA-256 名字，用户根本找不到（实测：一台开发机上积了 6 个死 PID 的锁）。
///
/// 为什么给 60 秒而不是更久：Qt 判陈旧的**第一道**是"那个 PID 还活着、且进程名对得上"
/// —— 活着的启动器永远不会被抢走，时间这一道只在"读不到对方的进程名"（跨用户/权限不足）
/// 或"PID 已经没了"时才生效。所以 60 秒只在崩溃之后让用户等一分钟，而不会误伤正在跑的
/// 那一份；反过来给十分钟，崩一次就要人干等十分钟，比锁死好不了多少。
inline constexpr int kStaleLockMs = 60 * 1000;

/// 锁文件落在系统临时目录（跨数据目录唯一）。名字是数据根路径的 SHA-256：
/// 路径里的中文/大小写差异都在 keyFor() 里归过了。
///
/// **临时目录不可写时要退到自己的本地数据目录**：有些机器上 `TEMP` 没设、指到
/// 一个不存在或只读的目录（服务账户、精简系统、被策略改过的用户环境），QLockFile
/// 会在"建锁文件"这一步就报权限错，界面只看到"无法取得数据目录的安全锁"，
/// 而数据目录本身完全正常。这里先挑一个**真的能写**的目录；回落之后仍失败，
/// 就把锁路径写进错误里，别再让人猜。
inline QString lockDirectory() {
    const QString temp = QDir::tempPath();
    const QFileInfo tempInfo(temp);
    if (tempInfo.isDir() && tempInfo.isWritable()) {
        return temp;
    }
    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!fallback.isEmpty() && QDir().mkpath(fallback)) {
        const QFileInfo fallbackInfo(fallback);
        if (fallbackInfo.isDir() && fallbackInfo.isWritable()) {
            return fallback;
        }
    }
    return temp;
}

inline QString lockFilePath(const QString &key) {
    return QDir(lockDirectory()).filePath(QStringLiteral("wtfml-directory-%1.lock").arg(key));
}

inline QString keyFor(const QString &root) {
    QString path = QDir::fromNativeSeparators(InstanceFiles::canonicalPath(root));
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex());
}

inline bool acquire(const QString &root, QString *problem = nullptr) {
    if (problem) problem->clear();
    if (!enabled) return true;
    if (root.trimmed().isEmpty()) {
        if (problem) *problem = QStringLiteral("未指定数据目录，未继续操作。");
        return false;
    }
    const auto key = keyFor(root);
    const QString lockPath = lockFilePath(key);
    // Background import/backup/upgrade may enter together. Checking then inserting
    // without a mutex both races the QHash and can reject our own second request.
    QMutexLocker guard(&mutex);
    if (held.contains(key)) return true;
    auto lock = std::make_shared<QLockFile>(lockPath);
    lock->setStaleLockTime(kStaleLockMs);
    // 临时目录里的锁文件偶尔会被杀毒/索引短暂占住（共享冲突）；给它两三次机会，
    // 别把一次瞬时占用报成"目录不可用"。
    bool locked = lock->tryLock(0);
    for (int attempt = 0; !locked && attempt < 2; ++attempt) {
        QThread::msleep(50);
        locked = lock->tryLock(0);
    }
    if (!locked) {
        if (problem) {
            *problem = lock->error() == QLockFile::LockFailedError
                ? QStringLiteral("这个数据目录已被另一份启动器占用，请先关闭那一份窗口。\n%1\n\n"
                                 "如果确认没有别的启动器在运行（上一次是崩溃或强制结束的），"
                                 "删掉这个文件再试：\n%2")
                      .arg(QDir::toNativeSeparators(root), QDir::toNativeSeparators(lockPath))
                : QStringLiteral("无法取得数据目录的安全锁，未继续操作。\n%1\n\n"
                                 "锁文件：%2")
                      .arg(QDir::toNativeSeparators(root),
                           QDir::toNativeSeparators(lockPath));
        }
        return false;
    }
    held.insert(key, std::move(lock));
    return true;
}

/// 主动放掉本进程拿到的**全部**锁。
///
/// 只有「立即重启」那一条路会调它（见 `MainWindow::onLanguageChanged()`）：新进程起来
/// 第一件事就是取同一把锁，旧进程不先放，它会停在「启动器已在运行」那一屏。`QLockFile`
/// 的析构会解锁文件，所以清掉这一表就够了。
inline void releaseAll() {
    QMutexLocker guard(&mutex);
    held.clear();
}
} // namespace DirectorySessionLock

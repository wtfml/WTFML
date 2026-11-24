#pragma once
#include "manage/InstanceFiles.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QLockFile>
#include <memory>

namespace Presets {
inline std::unique_ptr<QLockFile> acquirePresetWrite(const QString &root, QString *problem) {
    QString path = InstanceFiles::canonicalPath(root);
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    const auto key = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    auto lock = std::make_unique<QLockFile>(QDir::temp().filePath(
        QStringLiteral("wtfml-preset-write-%1.lock").arg(QString::fromLatin1(key))));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        if (problem) *problem = QStringLiteral("这个实例的另一项预设应用尚未结束，请等待或取消后重试。");
        return {};
    }
    return lock;
}
}

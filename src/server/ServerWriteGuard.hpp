#pragma once
#include "manage/RestoreJournal.hpp"
#include "util/AppPaths.hpp"
#include "util/DirectorySessionLock.hpp"

namespace ServerWriteGuard {
inline bool acquire(const QString &id, QString *problem) {
    const QString invalid = InstanceFiles::validatePathSegment(id);
    if (!invalid.isEmpty()) {
        if (problem) *problem = invalid;
        return false;
    }
    const QString root = InstanceFiles::canonicalPath(AppPaths::minecraftDir());
    if (!DirectorySessionLock::acquire(root, problem)) return false;
    if (!RestoreJournal::ordinaryPath(root, QDir(AppPaths::serverDir(id)).absolutePath())) {
        if (problem) *problem = QStringLiteral("服务端路径含链接或越界，未写入或删除。");
        return false;
    }
    return true;
}
inline bool acquireFile(const QString &id, const QString &name, QString *problem) {
    if (!acquire(id, problem)) return false;
    const QString root = InstanceFiles::canonicalPath(AppPaths::minecraftDir());
    if (!InstanceFiles::validatePathSegment(name).isEmpty()
        || !RestoreJournal::ordinaryPath(root, QDir(AppPaths::serverDir(id)).filePath(name))) {
        if (problem) *problem = QStringLiteral("服务端文件路径含链接或越界，原文件未替换。");
        return false;
    }
    return true;
}
}

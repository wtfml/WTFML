#include "ProcessPathLimit.hpp"

#include <QDir>

namespace ProcessPathLimit {

bool tooLong(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }
    // 按 UTF-16 字符数算（QString::size）。Windows 的 260 含结尾 NUL，所以恰好 260 个
    // 字符一定放不下；259 个是能放下的上界。
    return path.size() >= kMaxPathChars;
}

QString tooLongHint(const QStringList &paths) {
    const QString *longest = nullptr;
    for (const QString &path : paths) {
        if (!tooLong(path)) {
            continue;
        }
        if (longest == nullptr || path.size() > longest->size()) {
            longest = &path;
        }
    }
    if (longest == nullptr) {
        return QString();
    }
    // 提示里放整条路径：那正是用户要去改的那一条，截断反而让人无从下手。
    return QStringLiteral("\n%1 有 %2 个字符，超过了 Windows 起进程时 260 字符的路径上限。"
                          "\n把 Java 或实例挪到浅一点的目录（比如 D:/MC）再试。")
        .arg(QDir::toNativeSeparators(*longest), QString::number(longest->size()));
}

}  // namespace ProcessPathLimit

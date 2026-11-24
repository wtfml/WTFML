#include "server/ServerContent.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFileInfo>

namespace ServerContent {

int kindCount() {
    return 2;
}

QString kindTitle(Kind kind) {
    return kind == Kind::Datapacks ? QCoreApplication::translate("ServerContent", "数据包") : QCoreApplication::translate("ServerContent", "模组");
}

QString folderName(Kind kind) {
    return kind == Kind::Datapacks ? QStringLiteral("datapacks") : QStringLiteral("mods");
}

QString fileSuffix(Kind kind) {
    return kind == Kind::Datapacks ? QStringLiteral(".zip") : QStringLiteral(".jar");
}

QString disabledSuffix() {
    return QStringLiteral(".disabled");
}

bool isDisabledName(const QString &name) {
    return name.endsWith(disabledSuffix(), Qt::CaseInsensitive);
}

QString enabledName(const QString &name) {
    if (!isDisabledName(name)) {
        return name;
    }
    return name.left(name.size() - disabledSuffix().size());
}

bool isContentFileName(Kind kind, const QString &name) {
    const QString base = enabledName(name);
    // 只看"摘掉尾巴之后"的扩展名：`X.jar.disabled` 是禁用的模组，`X.jar.bak` 不是模组
    // （`.bak` 那个尾巴没摘，扩展名对不上就落选）。
    return base.endsWith(fileSuffix(kind), Qt::CaseInsensitive)
           && base.size() > fileSuffix(kind).size();
}

QString toggledName(Kind kind, const QString &name, bool enable) {
    if (!isContentFileName(kind, name)) {
        return QString();
    }
    const QString base = enabledName(name);
    return enable ? base : base + disabledSuffix();
}

QString datapacksDir(const QString &worldDir) {
    return worldDir.isEmpty() ? QString()
                              : QDir(worldDir).filePath(folderName(Kind::Datapacks));
}

QString modsDir(const QString &serverDir) {
    return serverDir.isEmpty() ? QString() : QDir(serverDir).filePath(folderName(Kind::Mods));
}

QList<Entry> entriesFromNames(const QStringList &names, Kind kind) {
    QList<Entry> entries;
    for (const QString &name : names) {
        if (!isContentFileName(kind, name)) {
            continue;
        }
        Entry entry;
        entry.name = name;
        entry.disabled = isDisabledName(name);
        entries.append(entry);
    }
    return entries;
}

}  // namespace ServerContent

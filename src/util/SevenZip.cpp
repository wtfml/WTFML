#include "SevenZip.hpp"

#include "util/ZipUtil.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
#include <QUuid>

namespace SevenZip {
namespace {
QString fromRegistry() {
    for (const auto &root : {QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\7-Zip"),
                             QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\7-Zip")}) {
        QSettings key(root, QSettings::NativeFormat);
        QString dir = key.value(QStringLiteral("Path")).toString();
        if (dir.isEmpty()) dir = key.value(QStringLiteral("InstallLocation")).toString();
        if (!dir.isEmpty()) {
            const QString exe = QDir(dir).filePath(QStringLiteral("7z.exe"));
            if (QFileInfo::exists(exe)) return exe;
        }
    }
    return {};
}
}  // namespace

QString executable() {
    static const QString cached = [] {
        for (const QString &candidate : {
                 QStringLiteral("C:/Program Files/7-Zip/7z.exe"),
                 QStringLiteral("C:/Program Files (x86)/7-Zip/7z.exe"),
                 QStringLiteral("D:/Program Files/7-Zip/7z.exe")}) {
            if (QFileInfo::exists(candidate)) return candidate;
        }
        const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("7z"));
        if (!fromPath.isEmpty()) return fromPath;
        return fromRegistry();
    }();
    return cached;
}

bool available() { return !executable().isEmpty(); }

bool isArchive(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("rar") || suffix == QLatin1String("7z");
}

QString toZip(const QString &archive, QString *error) {
    if (!isArchive(archive)) {
        if (error) *error = QStringLiteral("不是 rar/7z。");
        return {};
    }
    const QString sevenZip = executable();
    if (sevenZip.isEmpty()) {
        if (error) *error = QStringLiteral("没找到 7z.exe：请先装 7-Zip，或手动解压后再导入。");
        return {};
    }
    QTemporaryDir dir;
    if (!dir.isValid()) {
        if (error) *error = QStringLiteral("建不了临时目录。");
        return {};
    }
    QProcess process;
    process.setProgram(sevenZip);
    process.setArguments({QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-bso0"),
                          QStringLiteral("-bsp0"),
                          QStringLiteral("-o") + QDir::toNativeSeparators(dir.path()),
                          QDir::toNativeSeparators(archive)});
    process.start();
    if (!process.waitForStarted(30000)) {
        if (error) *error = QStringLiteral("7z.exe 起不来。");
        return {};
    }
    while (!process.waitForFinished(1000)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) *error = QStringLiteral("7-Zip 解压失败：%1")
                                .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        return {};
    }
    const QString target = QDir::temp().filePath(
        QStringLiteral("wtfml-sevenzip-%1.zip")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QList<ZipUtil::ZipSource> sources;
    QDirIterator it(dir.path(), QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (!info.isFile() || info.isSymLink()) continue;
        sources.append({QDir(dir.path()).relativeFilePath(info.absoluteFilePath()),
                        info.absoluteFilePath()});
    }
    QString zipError;
    if (!ZipUtil::createZip(target, sources, &zipError)) {
        if (error) *error = zipError.isEmpty() ? QStringLiteral("压成 zip 失败。") : zipError;
        return {};
    }
    return target;
}

}  // namespace SevenZip


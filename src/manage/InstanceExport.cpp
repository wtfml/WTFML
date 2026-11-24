#include "InstanceExport.hpp"
#include <QCoreApplication>

#include "util/AppPaths.hpp"
#include "util/ZipUtil.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

namespace InstanceExport {
namespace {
QString loaderUid(const QString &key) {
    if (key == QLatin1String("fabric")) return QStringLiteral("net.fabricmc.fabric-loader");
    if (key == QLatin1String("forge")) return QStringLiteral("net.minecraftforge");
    if (key == QLatin1String("neoforge")) return QStringLiteral("net.neoforged");
    if (key == QLatin1String("quilt")) return QStringLiteral("org.quiltmc.quilt-loader");
    return {};
}
}  // namespace

Result exportInstance(const QString &instanceId, const QString &gameVersion,
                      const QString &loaderKey, const QString &loaderVersion,
                      const QString &displayName, bool customLaunch, const QString &targetZip) {
    Result result;
    const QDir instance(AppPaths::versionDir(instanceId));
    if (!instance.exists()) {
        result.problem = QCoreApplication::translate("InstanceExport", "找不到实例目录：%1").arg(instance.absolutePath());
        return result;
    }
    if (gameVersion.trimmed().isEmpty()) {
        result.problem = QCoreApplication::translate("InstanceExport", "无法识别实例的原版游戏版本，未生成 mmc-pack.json。");
        return result;
    }
    QTemporaryDir staging;
    if (!staging.isValid()) {
        result.problem = QCoreApplication::translate("InstanceExport", "无法创建导出临时目录。");
        return result;
    }
    // 导入时留过原包身份（MultiMC/Prism 的 patches）就原样带回去：特殊启动配置
    // （lwjgl3ify 那类）只有这样才保得住；没有才退回合成的 mmc-pack.json。
    const QDir source(instance.absoluteFilePath(QStringLiteral(".wtfml/pack-source")));
    const bool preservesSource = source.exists(QStringLiteral("mmc-pack.json"));
    if (preservesSource) {
        QDirIterator stored(source.absolutePath(), QDir::Files | QDir::Hidden,
                            QDirIterator::Subdirectories);
        while (stored.hasNext()) {
            stored.next();
            const QString relative = source.relativeFilePath(stored.filePath());
            QDir().mkpath(QFileInfo(staging.filePath(relative)).absolutePath());
            QFile::copy(stored.filePath(), staging.filePath(relative));
        }
    } else {
        QJsonArray components;
        components.append(QJsonObject{{QStringLiteral("uid"), QStringLiteral("net.minecraft")},
                                      {QStringLiteral("version"), gameVersion}});
        const QString uid = loaderUid(loaderKey);
        if (!uid.isEmpty() && !loaderVersion.trimmed().isEmpty()) {
            components.append(QJsonObject{{QStringLiteral("uid"), uid},
                                          {QStringLiteral("version"), loaderVersion}});
        }
        const QJsonObject pack{{QStringLiteral("formatVersion"), 1},
                               {QStringLiteral("components"), components}};
        const QString packPath = staging.filePath(QStringLiteral("mmc-pack.json"));
        QFile packFile(packPath);
        if (!packFile.open(QIODevice::WriteOnly)
            || packFile.write(QJsonDocument(pack).toJson()) < 0) {
            result.problem = QCoreApplication::translate("InstanceExport", "写不了 mmc-pack.json。");
            return result;
        }
        packFile.close();
        const QString cfgPath = staging.filePath(QStringLiteral("instance.cfg"));
        QFile cfgFile(cfgPath);
        if (!cfgFile.open(QIODevice::WriteOnly)) {
            result.problem = QCoreApplication::translate("InstanceExport", "写不了 instance.cfg。");
            return result;
        }
        cfgFile.write(QStringLiteral("InstanceType=OneSix\nname=%1\n").arg(displayName).toUtf8());
        cfgFile.close();
    }
    QList<ZipUtil::ZipSource> sources;
    QDirIterator it(staging.path(), QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (!info.isFile() || info.isSymLink()) continue;
        sources.append({QDir(staging.path()).relativeFilePath(info.absoluteFilePath()),
                        info.absoluteFilePath()});
    }
    qint64 bytes = 0;
    QDirIterator game(instance.absolutePath(), QDir::Files | QDir::Hidden | QDir::System,
                      QDirIterator::Subdirectories);
    while (game.hasNext()) {
        game.next();
        const QFileInfo info = game.fileInfo();
        if (!info.isFile() || info.isSymLink()) continue;
        const QString relative = instance.relativeFilePath(info.absoluteFilePath());
        if (relative.startsWith(QStringLiteral(".wtfml/"))) continue;
        if (relative.startsWith(QStringLiteral(".."))) continue;
        sources.append({QStringLiteral(".minecraft/") + relative, info.absoluteFilePath()});
        bytes += info.size();
    }
    QString error;
    if (!ZipUtil::createZip(targetZip, sources, &error)) {
        result.problem = error.isEmpty() ? QCoreApplication::translate("InstanceExport", "打包失败。") : error;
        return result;
    }
    result.ok = true;
    result.files = sources.size();
    result.bytes = bytes;
    if (customLaunch && !preservesSource) {
        result.warning = QCoreApplication::translate("InstanceExport", "实例修改了主类或 JVM 参数。Prism / MultiMC 能识别导出的包，但可能缺少这些启动配置并导致启动失败。建议分发原整合包。");
    } else if (bytes > 1024LL * 1024 * 1024) {
        result.warning = QCoreApplication::translate("InstanceExport", "实例超过 1 GB，导出和导入需要较长时间。接收方须使用兼容的 Java 版本，并预留足够内存和磁盘空间。");
    }
    return result;
}

}  // namespace InstanceExport


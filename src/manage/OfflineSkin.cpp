/**
 * @file OfflineSkin.cpp
 * @brief `OfflineSkin.hpp` 的实现：两小段文件读写，没有网络、没有 JAR。
 */

#include "OfflineSkin.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "PlayerSkinStore.hpp"
#include "../util/AppPaths.hpp"

namespace {

/// 那一行模型文字的归一化：**只认 slim**，别的（包括空、大写、乱写）一律经典。
/// 和界面下拉框那两个值（classic / slim）是同一套词。
QString normalizeVariant(const QString &variant) {
    return variant.trimmed().compare(QLatin1String("slim"), Qt::CaseInsensitive) == 0
               ? QStringLiteral("slim")
               : QStringLiteral("classic");
}

/// 账号键 + 目录。key 认不出来时 *key 为空，调用方返回空串/失败。
QString skinDirFor(const QString &uuid, QString *key) {
    const QString k = PlayerSkinStore::uuidKey(uuid);
    if (key != nullptr) {
        *key = k;
    }
    return k.isEmpty() ? QString() : AppPaths::skinsDir();
}

}  // namespace

namespace OfflineSkin {

QString skinPathFor(const QString &uuid) {
    QString key;
    const QString dir = skinDirFor(uuid, &key);
    if (dir.isEmpty()) {
        return QString();
    }
    return dir + QLatin1Char('/') + QStringLiteral("manual-") + key + QStringLiteral(".png");
}

QString modelPathFor(const QString &uuid) {
    QString key;
    const QString dir = skinDirFor(uuid, &key);
    if (dir.isEmpty()) {
        return QString();
    }
    return dir + QLatin1Char('/') + QStringLiteral("manual-") + key + QStringLiteral(".txt");
}

bool hasSkin(const QString &uuid) {
    const QString path = skinPathFor(uuid);
    return !path.isEmpty() && QFile::exists(path);
}

QString loadModel(const QString &uuid) {
    const QString path = modelPathFor(uuid);
    if (path.isEmpty()) {
        return QStringLiteral("classic");
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("classic");
    }
    return normalizeVariant(QString::fromUtf8(file.readAll()).trimmed());
}

bool save(const QString &uuid, const QImage &image, const QString &variant, QString *errorOut) {
    const QString path = skinPathFor(uuid);
    if (path.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("OfflineSkin", "无法识别账号 UUID，皮肤未保存。");
        }
        return false;
    }
    if (image.isNull()) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("OfflineSkin", "无法读取图片。");
        }
        return false;
    }
    // 目录可能还不存在（从没下过皮肤的新账号）。建不出来时 QSaveFile 那一句会报出来。
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("OfflineSkin", "无法保存「%1」：%2").arg(path, file.errorString());
        }
        return false;
    }
    // 一律按 PNG 写：来源可能是 jpg/bmp（QImage 都读得进来），而那条记录的后缀写的是 .png ——
    // 内容和后缀对不上，以后任何"按后缀认格式"的读者都会认错。PNG 是无损的，重新编码不丢像素。
    if (!image.save(&file, "PNG")) {
        file.cancelWriting();
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("OfflineSkin", "这张图存不成 PNG：%1").arg(file.errorString());
        }
        return false;
    }
    if (!file.commit()) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("OfflineSkin", "皮肤图没写完整：%1").arg(file.errorString());
        }
        return false;
    }

    // 模型那一行：写不进去**不算失败** —— 下次按经典显示，比"图存下了却报一个错"好。
    const QString modelPath = modelPathFor(uuid);
    if (!modelPath.isEmpty()) {
        QSaveFile modelFile(modelPath);
        if (modelFile.open(QIODevice::WriteOnly)) {
            modelFile.write(normalizeVariant(variant).toUtf8());
            modelFile.write("\n");
            modelFile.commit();
        }
    }
    return true;
}

bool clear(const QString &uuid) {
    const QString path = skinPathFor(uuid);
    const QString modelPath = modelPathFor(uuid);
    if (path.isEmpty()) {
        return false;
    }
    if (QFile::exists(path) && !QFile::remove(path)) {
        return false;
    }
    if (!modelPath.isEmpty() && QFile::exists(modelPath) && !QFile::remove(modelPath)) {
        return false;
    }
    return !QFile::exists(path) && (modelPath.isEmpty() || !QFile::exists(modelPath));
}

}  // namespace OfflineSkin

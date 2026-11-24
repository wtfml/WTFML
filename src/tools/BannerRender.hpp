#pragma once
#include <QHash>
#include <QImage>
#include <QPainter>
#include "Banner.hpp"
#include "BeaconBeam.hpp"

namespace BannerRender {
inline QString textureName(QString id) {
    static const QHash<QString, QString> aliases{{"bordure", "border"}, {"bordure_indented", "curly_border"},
        {"field_masoned", "bricks"}, {"creeper_charge", "creeper"}, {"flower_charge", "flower"},
        {"skull_charge", "skull"}, {"thing", "mojang"}, {"snout", "piglin"}};
    return aliases.value(id, id);
}
inline QImage compose(int baseColor, const QList<Banner::Layer> &layers,
                      const QHash<QString, QImage> &textures, QString *error = nullptr) {
    if (error) error->clear();
    if (!BeaconBeam::dyeColor(baseColor).isValid() || layers.size() > Banner::kMaxLayers) return {};
    QImage result(20, 40, QImage::Format_ARGB32_Premultiplied);
    result.fill(BeaconBeam::dyeColor(baseColor));
    QPainter painter(&result);
    for (const auto &layer : layers) {
        const auto pattern = Banner::patternAt(layer.patternIndex);
        const auto dye = BeaconBeam::dyeColor(layer.colorIndex);
        const auto mask = textures.value(textureName(pattern.id));
        if (pattern.id.isEmpty() || !dye.isValid() || mask.isNull()) {
            if (error) *error = QStringLiteral("缺少图案贴图：%1。请加载包含该图案的游戏 JAR。").arg(pattern.name);
            return {};
        }
        // 原版旗面正面的 UV 为 64×64 纹理中的 (1,1,20,40)。加载器先归一化高清纹理。
        QImage face = mask.copy(1, 1, 20, 40).convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < face.height(); ++y) for (int x = 0; x < face.width(); ++x) {
            const QColor pixel = face.pixelColor(x, y);
            face.setPixelColor(x, y, QColor(pixel.red() * dye.red() / 255,
                pixel.green() * dye.green() / 255, pixel.blue() * dye.blue() / 255, pixel.alpha()));
        }
        painter.drawImage(0, 0, face);
    }
    return result;
}
}

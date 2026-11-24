#pragma once
#include <QColor>
#include <QList>
#include <QStringList>
#include <array>

namespace BeaconBeam {
// Java 现代版染料纹理漫反射色；不是主题配色，也不是把所有玻璃一次性平均。
inline QColor dyeColor(int index) {
    static constexpr quint32 colors[]{0xf9fffe,0xf9801d,0xc74ebd,0x3ab3da,0xfed83d,0x80c71f,
        0xf38baa,0x474f52,0x9d9d97,0x169c9c,0x8932b8,0x3c44aa,0x835432,0x5e7c16,0xb02e26,0x1d1d21};
    return index >= 0 && index < 16 ? QColor::fromRgb(colors[index]) : QColor();
}
struct Result { bool ok = true; QString error; QList<QColor> segments; QColor finalColor = Qt::white; };
inline Result calculate(const QList<int> &glass) {
    Result result;
    if (glass.size() > 64) { result.ok = false; result.error = QStringLiteral("最多计算 64 层染色玻璃。"); return result; }
    std::array<double, 3> color{1, 1, 1};
    for (qsizetype i = 0; i < glass.size(); ++i) {
        const auto dye = dyeColor(glass[i]);
        if (!dye.isValid()) { result.ok = false; result.error = QStringLiteral("玻璃颜色无效。"); result.segments.clear(); return result; }
        const std::array<double, 3> next{dye.redF(), dye.greenF(), dye.blueF()};
        for (int channel = 0; channel < 3; ++channel)
            color[channel] = i == 0 ? next[channel] : (color[channel] + next[channel]) / 2;
        result.finalColor = QColor::fromRgbF(color[0], color[1], color[2]);
        result.segments.append(result.finalColor);
    }
    return result;
}
}

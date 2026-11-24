#pragma once
#include "Banner.hpp"
#include "GameVersion.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace BannerDesign {
struct Design { int base = 0; QString version; QStringList layers; };
inline QStringList versions() { return GameVersion::versions(); }
inline bool available(int pattern, const QString &version) {
    return GameVersion::availableIn(Banner::patternSince(pattern), version);
}
inline QJsonObject encode(const Design &design) {
    QJsonArray layers;
    for (const auto &entry : design.layers) {
        const auto parts = entry.split(':');
        layers.append(QJsonObject{{QStringLiteral("pattern"), Banner::patternAt(parts.value(0).toInt()).id},
            {QStringLiteral("color"), Banner::colorId(parts.value(1).toInt())}});
    }
    return {{QStringLiteral("format"), QStringLiteral("wtfml-banner")}, {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("base"), Banner::colorId(design.base)}, {QStringLiteral("version"), design.version},
        {QStringLiteral("layers"), layers}};
}
inline bool decode(const QByteArray &bytes, Design *out, QString *error) {
    const auto fail = [&](const QString &text) { if (error) *error = text; return false; };
    if (!out || bytes.size() > 64 * 1024) return fail(QStringLiteral("设计文件过大或参数无效。"));
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError)
        return fail(QStringLiteral("JSON 格式错误（字节 %1）：%2").arg(parse.offset).arg(parse.errorString()));
    const auto object = doc.object();
    if (!doc.isObject() || object.value("format") != QJsonValue(QStringLiteral("wtfml-banner"))
        || object.value("schemaVersion") != QJsonValue(1) || !object.value("base").isString()
        || !object.value("version").isString() || !object.value("layers").isArray())
        return fail(QStringLiteral("不是受支持的 WTFML 旗帜设计文件。"));
    const auto colorIndex = [](const QString &id) {
        for (int i = 0; i < Banner::colorCount(); ++i) if (Banner::colorId(i) == id) return i;
        return -1;
    };
    Design design;
    design.base = colorIndex(object.value("base").toString()); design.version = object.value("version").toString();
    if (design.base < 0 || !versions().contains(design.version)) return fail(QStringLiteral("底色或目标版本无效。"));
    const auto layers = object.value("layers").toArray();
    if (layers.size() > Banner::kMaxLayers) return fail(QStringLiteral("旗帜最多支持 6 层纹样。"));
    for (const auto &value : layers) {
        const auto layer = value.toObject();
        if (!value.isObject() || !layer.value("pattern").isString() || !layer.value("color").isString())
            return fail(QStringLiteral("图层字段类型无效。"));
        const int pattern = Banner::patternIndexById(layer.value("pattern").toString());
        const int color = colorIndex(layer.value("color").toString());
        if (pattern < 0 || color < 0) return fail(QStringLiteral("设计包含无法识别的图案或颜色。"));
        design.layers.append(QStringLiteral("%1:%2").arg(pattern).arg(color));
    }
    *out = design;
    if (error) error->clear();
    return true;
}
inline bool save(const QString &path, const Design &design, QString *error) {
    const auto bytes = QJsonDocument(encode(design)).toJson();
    Design checked;
    if (!decode(bytes, &checked, error)) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    if (error) error->clear();
    return true;
}
inline bool load(const QString &path, Design *out, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
    return decode(file.read(64 * 1024 + 1), out, error);
}
}

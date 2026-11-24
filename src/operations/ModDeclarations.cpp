/**
 * @file ModDeclarations.cpp
 * @brief 模组声明解析（见 .hpp）。
 */

#include "ModDeclarations.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("ModDeclarations", text);
}

const QStringList kLoaderIds{QStringLiteral("minecraft"), QStringLiteral("fabricloader"),
                             QStringLiteral("fabric-loader"), QStringLiteral("quilt_loader"),
                             QStringLiteral("quilt-loader"), QStringLiteral("forge"),
                             QStringLiteral("neoforge"), QStringLiteral("java")};

bool isLoaderId(const QString &id) {
    return kLoaderIds.contains(id.toLower());
}

/// `depends` 对象里的键：加载器/游戏/java 归到各自的槽，其余是 required 模组前置。
void collectObjectDependencies(const QJsonObject &depends, QStringList *modIds) {
    for (auto it = depends.constBegin(); it != depends.constEnd(); ++it) {
        if (isLoaderId(it.key())) continue;
        if (!modIds->contains(it.key())) modIds->append(it.key());
    }
}

/// Quilt 的依赖/冲突表是数组：`{"id":"minecraft","versions":"..."}`。
/// 取同一个 id 的所有范围；多个不同范围时返回空（调用方记 problem）。
QJsonValue quiltRange(const QJsonArray &list, const QString &id, QString *problem) {
    QStringList ranges;
    for (const QJsonValue &value : list) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("id")).toString() != id) continue;
        const QJsonValue versions = entry.value(QStringLiteral("versions"));
        if (versions.isString()) {
            ranges.append(versions.toString());
        } else if (versions.isArray()) {
            for (const QJsonValue &one : versions.toArray()) {
                if (one.isString()) ranges.append(one.toString());
            }
        } else if (problem != nullptr) {
            *problem = tr("Quilt 的 %1 依赖版本不是字符串或数组").arg(id);
        }
    }
    ranges.removeDuplicates();
    if (ranges.size() > 1) {
        if (problem != nullptr) *problem = tr("Quilt 的 %1 有多个不同的版本范围").arg(id);
        return QJsonValue();
    }
    return ranges.isEmpty() ? QJsonValue() : QJsonValue(ranges.constFirst());
}

void collectQuiltDependencies(const QJsonArray &list, QStringList *modIds) {
    for (const QJsonValue &value : list) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (id.isEmpty() || isLoaderId(id) || modIds->contains(id)) continue;
        modIds->append(id);
    }
}

/// 从 `mods.toml` 里读三类依赖：minecraft / 加载器自己 / java，外加 required 的前置 mod id。
/// 同一个类别出现多个**不同**范围时记 problem（不替用户挑一个）。
struct TomlRequirements {
    QJsonValue game;
    QJsonValue loader;
    QJsonValue java;
    QStringList requiredModIds;
    QString problem;
};

TomlRequirements tomlRequirements(const QByteArray &bytes, const QString &loaderId,
                                  QStringList *loaderIds) {
    TomlRequirements result;
    const QString text = QString::fromUtf8(bytes);
    auto blocks = QRegularExpression(QStringLiteral(
        "(?ms)^\\s*\\[\\[dependencies\\.[^\\]]+\\]\\]\\s*(.*?)(?=^\\s*\\[|\\z)"))
                      .globalMatch(text);
    QHash<QString, QString> ranges;
    while (blocks.hasNext()) {
        const QString block = blocks.next().captured(1);
        const auto id = QRegularExpression(QStringLiteral("(?m)^\\s*modId\\s*=\\s*[\"']([^\"']+)[\"']"))
                            .match(block);
        if (!id.hasMatch()) continue;
        const auto type = QRegularExpression(QStringLiteral("(?m)^\\s*type\\s*=\\s*[\"']([^\"']+)[\"']"))
                              .match(block);
        if (type.hasMatch() && type.captured(1) != QStringLiteral("required")) continue;
        const QString key = id.captured(1).trimmed();
        const auto range = QRegularExpression(
            QStringLiteral("(?m)^\\s*versionRange\\s*=\\s*[\"']([^\"']+)[\"']")).match(block);
        if (!range.hasMatch()) {
            result.problem = tr("依赖块缺少 versionRange");
            continue;
        }
        const QString value = range.captured(1).trimmed();
        if (key.toLower() == QStringLiteral("minecraft") || key == loaderId ||
            key.toLower() == QStringLiteral("java")) {
            if (!value.startsWith('[') && !value.startsWith('(')) {
                result.problem = tr("%1 的版本范围语法无效：%2").arg(key, value);
                continue;
            }
            if (ranges.contains(key) && ranges.value(key) != value) {
                result.problem = tr("%1 有多个不同的版本范围").arg(key);
                continue;
            }
            ranges.insert(key, value);
            continue;
        }
        if (isLoaderId(key)) {
            if (loaderIds != nullptr && !loaderIds->contains(key)) loaderIds->append(key);
            continue;
        }
        if (!result.requiredModIds.contains(key)) result.requiredModIds.append(key);
    }
    if (ranges.contains(QStringLiteral("minecraft"))) {
        result.game = QJsonValue(ranges.value(QStringLiteral("minecraft")));
    }
    if (ranges.contains(loaderId)) result.loader = QJsonValue(ranges.value(loaderId));
    if (ranges.contains(QStringLiteral("java"))) {
        result.java = QJsonValue(ranges.value(QStringLiteral("java")));
    }
    return result;
}

ModDeclarations::Loader loaderForEntry(const QString &entryName) {
    if (entryName == QStringLiteral("fabric.mod.json")) return ModDeclarations::Loader::Fabric;
    if (entryName == QStringLiteral("quilt.mod.json")) return ModDeclarations::Loader::Quilt;
    if (entryName == QStringLiteral("META-INF/neoforge.mods.toml")) {
        return ModDeclarations::Loader::NeoForge;
    }
    if (entryName == QStringLiteral("META-INF/mods.toml")) return ModDeclarations::Loader::Forge;
    if (entryName == QStringLiteral("mcmod.info")) return ModDeclarations::Loader::Forge;
    return ModDeclarations::Loader::Unknown;
}

}  // namespace

namespace ModDeclarations {

QString loaderKey(Loader loader) {
    switch (loader) {
    case Loader::Fabric: return QStringLiteral("fabric");
    case Loader::Quilt: return QStringLiteral("quilt");
    case Loader::Forge: return QStringLiteral("forge");
    case Loader::NeoForge: return QStringLiteral("neoforge");
    case Loader::Unknown: break;
    }
    return QStringLiteral("unknown");
}

Loader loaderFromKey(const QString &key) {
    const QString lowered = key.trimmed().toLower();
    if (lowered == QStringLiteral("fabric")) return Loader::Fabric;
    if (lowered == QStringLiteral("quilt")) return Loader::Quilt;
    if (lowered == QStringLiteral("forge")) return Loader::Forge;
    if (lowered == QStringLiteral("neoforge")) return Loader::NeoForge;
    return Loader::Unknown;
}

QStringList metadataEntries() {
    return {QStringLiteral("fabric.mod.json"), QStringLiteral("quilt.mod.json"),
            QStringLiteral("META-INF/neoforge.mods.toml"), QStringLiteral("META-INF/mods.toml"),
            QStringLiteral("mcmod.info")};
}

bool Mod::hasUsableDeclaration() const {
    for (const Declaration &declaration : declarations) {
        if (declaration.problem.isEmpty() && !declaration.game.isNull() &&
            !declaration.game.isUndefined()) {
            return true;
        }
    }
    return false;
}

bool parseEntry(const QString &entryName, const QByteArray &bytes, Declaration *declaration,
                QStringList *requiredModIds, QStringList *requiredLoaderIds) {
    if (declaration == nullptr) return false;
    *declaration = Declaration{};
    declaration->loader = loaderForEntry(entryName);
    declaration->basis = entryName;
    const Loader loader = declaration->loader;

    if (loader == Loader::Fabric) {
        QJsonParseError parse{};
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            declaration->problem = tr("Fabric 声明不是有效 JSON 对象");
            return false;
        }
        const QJsonObject object = document.object();
        const QJsonObject depends = object.value(QStringLiteral("depends")).toObject();
        declaration->game = depends.value(QStringLiteral("minecraft"));
        declaration->basis += QStringLiteral(" / depends.minecraft");
        declaration->loaderRange = depends.value(QStringLiteral("fabricloader"));
        declaration->loaderBasis = declaration->basis + QStringLiteral(" / fabricloader");
        declaration->javaRange = depends.value(QStringLiteral("java"));
        declaration->javaBasis = declaration->basis + QStringLiteral(" / java");
        declaration->breaks =
            object.value(QStringLiteral("breaks")).toObject().value(QStringLiteral("minecraft"));
        if (requiredModIds != nullptr) collectObjectDependencies(depends, requiredModIds);
        if (requiredLoaderIds != nullptr) requiredLoaderIds->append(QStringLiteral("fabricloader"));
        return true;
    }

    if (loader == Loader::Quilt) {
        QJsonParseError parse{};
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            declaration->problem = tr("Quilt 声明不是有效 JSON 对象");
            return false;
        }
        const QJsonObject object = document.object();
        const QJsonArray depends = object.value(QStringLiteral("depends")).toArray();
        QString problem;
        declaration->game = quiltRange(depends, QStringLiteral("minecraft"), &problem);
        declaration->basis += QStringLiteral(" / depends[minecraft]");
        declaration->loaderRange = quiltRange(depends, QStringLiteral("quilt_loader"), &problem);
        declaration->loaderBasis =
            declaration->basis + QStringLiteral(" / quilt_loader");
        declaration->javaRange = quiltRange(depends, QStringLiteral("java"), &problem);
        declaration->javaBasis = declaration->basis + QStringLiteral(" / java");
        declaration->breaks =
            quiltRange(object.value(QStringLiteral("breaks")).toArray(),
                       QStringLiteral("minecraft"), &problem);
        declaration->problem = problem;
        if (requiredModIds != nullptr) collectQuiltDependencies(depends, requiredModIds);
        if (requiredLoaderIds != nullptr) requiredLoaderIds->append(QStringLiteral("quilt_loader"));
        return true;
    }

    if (loader == Loader::Forge || loader == Loader::NeoForge) {
        if (entryName.endsWith(QStringLiteral(".toml"))) {
            const QString loaderId = loader == Loader::NeoForge ? QStringLiteral("neoforge")
                                                                : QStringLiteral("forge");
            const TomlRequirements requirements =
                tomlRequirements(bytes, loaderId, requiredLoaderIds);
            declaration->game = requirements.game;
            declaration->basis += QStringLiteral(" / minecraft.versionRange");
            declaration->loaderRange = requirements.loader;
            declaration->loaderBasis = declaration->basis + QStringLiteral(" / ") + loaderId;
            declaration->javaRange = requirements.java;
            declaration->javaBasis = declaration->basis + QStringLiteral(" / java");
            declaration->problem = requirements.problem;
            if (requiredModIds != nullptr) requiredModIds->append(requirements.requiredModIds);
            return true;
        }
        // mcmod.info：数组 / {modList:[...]} / 单个对象都认。
        QJsonParseError parse{};
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError) {
            declaration->problem = tr("mcmod.info 无法解析（%1）").arg(parse.errorString());
            return false;
        }
        QList<QJsonObject> entries;
        if (document.isArray()) {
            for (const QJsonValue &value : document.array()) entries.append(value.toObject());
        } else if (document.isObject()) {
            const QJsonObject object = document.object();
            if (object.value(QStringLiteral("modList")).isArray()) {
                for (const QJsonValue &value : object.value(QStringLiteral("modList")).toArray()) {
                    entries.append(value.toObject());
                }
            } else {
                entries.append(object);
            }
        }
        QStringList ranges;
        for (const QJsonObject &entry : entries) {
            const QJsonValue accepted = entry.value(QStringLiteral("acceptedMinecraftVersions"));
            if (accepted.isString()) {
                ranges.append(accepted.toString());
            } else if (accepted.isArray()) {
                for (const QJsonValue &one : accepted.toArray()) {
                    if (one.isString()) ranges.append(one.toString());
                }
            }
            if (requiredModIds != nullptr) {
                for (const QJsonValue &required :
                     entry.value(QStringLiteral("requiredMods")).toArray()) {
                    QString id = required.toString().trimmed();
                    const int at = id.indexOf(QLatin1Char('@'));
                    if (at > 0) id = id.left(at);
                    if (!id.isEmpty() && !isLoaderId(id) && !requiredModIds->contains(id)) {
                        requiredModIds->append(id);
                    }
                }
            }
        }
        ranges.removeDuplicates();
        declaration->basis += QStringLiteral(" / acceptedMinecraftVersions");
        if (ranges.size() == 1) {
            declaration->game = ranges.constFirst();
        } else if (ranges.size() > 1) {
            declaration->problem = tr("mcmod.info 中多个 mod 的 acceptedMinecraftVersions 不一致");
        } else {
            declaration->problem = tr("mcmod.info 缺少 acceptedMinecraftVersions");
        }
        if (requiredLoaderIds != nullptr) requiredLoaderIds->append(QStringLiteral("forge"));
        return true;
    }

    declaration->problem = tr("不认识的声明文件：%1").arg(entryName);
    return false;
}

void parse(const QString &fileName, const QHash<QString, QByteArray> &entries,
           const QHash<QString, QString> &entryProblems, Mod *mod) {
    if (mod == nullptr) return;
    *mod = Mod{};
    mod->fileName = fileName;
    const auto readProblem = [&](const QString &entry, Loader loader) {
        Declaration declaration;
        declaration.loader = loader;
        declaration.basis = entry;
        declaration.problem = entryProblems.value(entry);
        mod->declarations.append(declaration);
    };
    const auto parseJson = [&](const QString &entry) {
        Declaration declaration;
        if (!parseEntry(entry, entries.value(entry), &declaration, &mod->requiredModIds,
                        &mod->requiredLoaderIds)) {
            mod->declarations.append(declaration);
            return;
        }
        mod->declarations.append(declaration);
    };
    if (entryProblems.contains(QStringLiteral("fabric.mod.json"))) {
        readProblem(QStringLiteral("fabric.mod.json"), Loader::Fabric);
    } else if (entries.contains(QStringLiteral("fabric.mod.json"))) {
        parseJson(QStringLiteral("fabric.mod.json"));
        const QJsonObject object =
            QJsonDocument::fromJson(entries.value(QStringLiteral("fabric.mod.json"))).object();
        mod->modId = object.value(QStringLiteral("id")).toString();
        mod->name = object.value(QStringLiteral("name")).toString();
        mod->version = object.value(QStringLiteral("version")).toString();
    }
    if (entryProblems.contains(QStringLiteral("quilt.mod.json"))) {
        readProblem(QStringLiteral("quilt.mod.json"), Loader::Quilt);
    } else if (entries.contains(QStringLiteral("quilt.mod.json"))) {
        parseJson(QStringLiteral("quilt.mod.json"));
        const QJsonObject object =
            QJsonDocument::fromJson(entries.value(QStringLiteral("quilt.mod.json"))).object();
        const QJsonObject loader = object.value(QStringLiteral("quilt_loader")).toObject();
        mod->modId = loader.value(QStringLiteral("id")).toString();
        mod->version = loader.value(QStringLiteral("version")).toString();
        mod->name = loader.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("name")).toString();
    }
    for (const QString &entry : {QStringLiteral("META-INF/neoforge.mods.toml"),
                                 QStringLiteral("META-INF/mods.toml")}) {
        const Loader loader = loaderForEntry(entry);
        if (entryProblems.contains(entry)) {
            readProblem(entry, loader);
            continue;
        }
        if (!entries.contains(entry)) continue;
        parseJson(entry);
        // `[[mods]]` 段里的 modId / version / displayName（只取第一段）。
        const QString text = QString::fromUtf8(entries.value(entry));
        static const QRegularExpression modsBlock(
            QStringLiteral("(?ms)^\\s*\\[\\[mods\\]\\]\\s*(.*?)(?=^\\s*\\[\\[|\\z)"));
        const auto mods = modsBlock.match(text);
        if (mods.hasMatch()) {
            const QString block = mods.captured(1);
            const auto value = [&block](const QString &key) {
                const auto match = QRegularExpression(
                    QStringLiteral("(?m)^\\s*%1\\s*=\\s*[\"']([^\"']*)[\"']").arg(key))
                                       .match(block);
                return match.hasMatch() ? match.captured(1) : QString();
            };
            if (mod->modId.isEmpty()) mod->modId = value(QStringLiteral("modId"));
            if (mod->version.isEmpty()) mod->version = value(QStringLiteral("version"));
            if (mod->name.isEmpty()) mod->name = value(QStringLiteral("displayName"));
        }
    }
    if (entryProblems.contains(QStringLiteral("mcmod.info"))) {
        readProblem(QStringLiteral("mcmod.info"), Loader::Forge);
    } else if (entries.contains(QStringLiteral("mcmod.info"))) {
        parseJson(QStringLiteral("mcmod.info"));
        const QJsonDocument document =
            QJsonDocument::fromJson(entries.value(QStringLiteral("mcmod.info")));
        QList<QJsonObject> list;
        if (document.isArray()) {
            for (const QJsonValue &value : document.array()) list.append(value.toObject());
        } else if (document.object().value(QStringLiteral("modList")).isArray()) {
            for (const QJsonValue &value : document.object().value(QStringLiteral("modList")).toArray()) {
                list.append(value.toObject());
            }
        } else if (document.isObject()) {
            list.append(document.object());
        }
        for (const QJsonObject &entry : list) {
            if (mod->modId.isEmpty()) mod->modId = entry.value(QStringLiteral("modid")).toString();
            if (mod->name.isEmpty()) mod->name = entry.value(QStringLiteral("name")).toString();
            if (mod->version.isEmpty()) mod->version = entry.value(QStringLiteral("version")).toString();
        }
    }
    if (mod->declarations.isEmpty()) mod->problem = tr("JAR 里没有可识别的模组声明");
    mod->requiredModIds.removeDuplicates();
    mod->requiredLoaderIds.removeDuplicates();
}

}  // namespace ModDeclarations

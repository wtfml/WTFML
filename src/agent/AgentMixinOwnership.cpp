/**
 * @file AgentMixinOwnership.cpp
 * @brief mixin 配置归属（见 .hpp）。
 */

#include "AgentMixinOwnership.hpp"

#include "../util/ZipUtil.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("AgentMixinOwnership", text);
}

QString findDeclaration(const QHash<QString, QByteArray> &declarations, const QString &name) {
    const auto direct = declarations.constFind(name);
    if (direct != declarations.constEnd()) return QString::fromUtf8(direct.value());
    for (auto it = declarations.constBegin(); it != declarations.constEnd(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) return QString::fromUtf8(it.value());
    }
    return QString();
}

/// `mixins` / `mixin` 数组：元素是字符串，或者是带 `config` 的对象。
void collectJsonMixins(const QJsonValue &value, QStringList *configs) {
    if (value.isString()) {
        configs->append(value.toString());
        return;
    }
    if (!value.isArray()) return;
    for (const QJsonValue &item : value.toArray()) {
        if (item.isString()) {
            configs->append(item.toString());
        } else if (item.isObject()) {
            const QString config = item.toObject().value(QStringLiteral("config")).toString();
            if (!config.isEmpty()) configs->append(config);
        }
    }
}

/// `mods.toml` 里 `[[mixins]]` 段下的 `config = "..."`；`modId = "..."` 取第一处。
void collectTomlMixins(const QString &text, QString *modId, QStringList *configs) {
    static const QRegularExpression modIdPattern(
        QStringLiteral(R"((?m)^\s*modId\s*=\s*["']([^"']+)["'])"));
    const auto modIdMatch = modIdPattern.match(text);
    if (modIdMatch.hasMatch()) *modId = modIdMatch.captured(1);

    static const QRegularExpression sectionPattern(QStringLiteral(R"(^\s*\[\[?\s*([^\]]+?)\s*\]?\])"));
    static const QRegularExpression configPattern(
        QStringLiteral(R"((?m)^\s*config\s*=\s*["']([^"']+)["'])"));
    bool inMixins = false;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const auto section = sectionPattern.match(line);
        if (section.hasMatch()) {
            inMixins = section.captured(1).trimmed().startsWith(QStringLiteral("mixins"));
            continue;
        }
        if (!inMixins) continue;
        const auto config = configPattern.match(line);
        if (config.hasMatch()) configs->append(config.captured(1));
    }
}

/// `META-INF/MANIFEST.MF` 的 `MixinConfigs:`（折行以单个空格继续）。
QStringList manifestMixins(const QString &text) {
    QStringList configs;
    QStringList buffer;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        QString line = raw;
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.startsWith(QLatin1Char(' ')) && !buffer.isEmpty()) {
            buffer.last() += line.mid(1);
            continue;
        }
        buffer.append(line);
    }
    for (const QString &line : buffer) {
        if (!line.startsWith(QStringLiteral("MixinConfigs:"), Qt::CaseInsensitive)) continue;
        const QString value = line.mid(line.indexOf(QLatin1Char(':')) + 1);
        for (const QString &part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString trimmed = part.trimmed();
            if (!trimmed.isEmpty()) configs.append(trimmed);
        }
    }
    return configs;
}

QList<AgentMixinOwnership::Entry> entriesFor(const QStringList &configs, const QString &modId,
                                             const QString &fileName) {
    QList<AgentMixinOwnership::Entry> result;
    for (const QString &config : configs) {
        const QString normalized = AgentMixinOwnership::normalizeConfig(config);
        if (normalized.isEmpty()) continue;
        AgentMixinOwnership::Entry entry;
        entry.config = normalized;
        entry.modId = modId;
        entry.fileName = fileName;
        result.append(entry);
    }
    return result;
}

}  // namespace

namespace AgentMixinOwnership {

QString normalizeConfig(const QString &name) {
    QString value = name.trimmed();
    const int slash = qMax(value.lastIndexOf(QLatin1Char('/')), value.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0) value = value.mid(slash + 1);
    if (value.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) value.chop(5);
    return value.toLower();
}

QList<Entry> fromDeclarations(const QHash<QString, QByteArray> &declarations,
                              const QString &fileName) {
    QList<Entry> result;
    QString modId;
    QStringList configs;

    for (const QString &name : {QStringLiteral("fabric.mod.json"), QStringLiteral("quilt.mod.json")}) {
        const QString text = findDeclaration(declarations, name);
        if (text.isEmpty()) continue;
        const QJsonObject object = QJsonDocument::fromJson(text.toUtf8()).object();
        if (name == QStringLiteral("fabric.mod.json")) {
            modId = object.value(QStringLiteral("id")).toString();
            collectJsonMixins(object.value(QStringLiteral("mixins")), &configs);
        } else {
            const QJsonObject loader = object.value(QStringLiteral("quilt_loader")).toObject();
            modId = loader.value(QStringLiteral("id")).toString();
            collectJsonMixins(object.value(QStringLiteral("mixin")), &configs);
            collectJsonMixins(loader.value(QStringLiteral("mixin")), &configs);
        }
    }

    for (const QString &name :
         {QStringLiteral("META-INF/mods.toml"), QStringLiteral("META-INF/neoforge.mods.toml")}) {
        const QString text = findDeclaration(declarations, name);
        if (text.isEmpty()) continue;
        QString tomlModId;
        QStringList tomlConfigs;
        collectTomlMixins(text, &tomlModId, &tomlConfigs);
        if (modId.isEmpty()) modId = tomlModId;
        configs.append(tomlConfigs);
    }

    const QString manifest = findDeclaration(declarations, QStringLiteral("META-INF/MANIFEST.MF"));
    if (!manifest.isEmpty()) configs.append(manifestMixins(manifest));

    result.append(entriesFor(configs, modId, fileName));
    return result;
}

QList<Entry> scan(const QString &modsDir, int maxJars, int *skipped, QString *problem) {
    QList<Entry> result;
    if (skipped != nullptr) *skipped = 0;
    QDir dir(modsDir);
    if (!dir.exists()) {
        if (problem != nullptr) *problem = tr("mods 目录不存在。");
        return result;
    }
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("*.jar")}, QDir::Files, QDir::Name);
    QStringList archives;
    for (const QFileInfo &file : files) {
        if (archives.size() >= qMax(0, maxJars)) break;
        archives.append(file.absoluteFilePath());
    }
    if (archives.isEmpty()) return result;
    const QStringList entries{QStringLiteral("fabric.mod.json"), QStringLiteral("quilt.mod.json"),
                              QStringLiteral("META-INF/mods.toml"),
                              QStringLiteral("META-INF/neoforge.mods.toml"),
                              QStringLiteral("META-INF/MANIFEST.MF")};
    QString batchProblem;
    const QHash<QString, ZipUtil::ArchiveMetadata> metadata =
        ZipUtil::readMetadataBatch(archives, entries, &batchProblem);
    for (const QString &archive : archives) {
        const auto it = metadata.constFind(archive);
        if (it == metadata.constEnd()) {
            if (skipped != nullptr) *skipped += 1;
            continue;
        }
        const QList<Entry> found =
            fromDeclarations(it->entries, QFileInfo(archive).fileName());
        if (found.isEmpty() && skipped != nullptr) *skipped += 1;
        result.append(found);
    }
    if (result.isEmpty() && problem != nullptr) {
        *problem = batchProblem.isEmpty() ? tr("没有模组声明 mixin 配置。") : batchProblem;
    }
    return result;
}

}  // namespace AgentMixinOwnership

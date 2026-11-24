#include "UpgradeAssessment.hpp"
#include <QCoreApplication>
#include "InstanceSettingsDialog.hpp"
#include "controllers/InstancePagesController.hpp"
#include "manage/VersionManager.hpp"
#include "manage/VersionDisplay.hpp"
#include "manage/InstanceIconLookup.hpp"
#include "download/ResourceStore.hpp"
#include "operations/ModDeclarations.hpp"
#include "operations/ModVersionDeclaration.hpp"
#include "operations/FabricVersionDeclaration.hpp"
#include "util/ZipUtil.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <memory>
#include <functional>

namespace {
std::atomic_bool g_wallClockBudgets{true};
using Declaration = ModDeclarations::Declaration;

struct NestedCheck {
    QString name;
    QJsonValue game;
    QString problem;   // 读不动 / 没有声明 / 超过检查上限
    bool parsed = false;
};
struct Mod {
    QString path, name, sha1;
    QList<Declaration> declarations;
    QString problem, identityProblem;
    ResourceCatalog::Hit identity;
    bool identified = false;
    QString localVersion;
    QStringList nested;
    bool nestedIncomplete = false;
    QList<NestedCheck> nestedChecks;
    bool replacementAvailable = false;
    ResourceCatalog::VersionEntry replacement;
};
struct Scan { QList<Mod> mods; QString note; };

/// 把一份 JAR 的声明搬进升级评估用的 `Mod`（解析在 `ModDeclarations`）。
void readModDeclarations(Mod &mod, const ZipUtil::ArchiveMetadata &record) {
    ModDeclarations::Mod parsed;
    ModDeclarations::parse(QFileInfo(mod.path).fileName(), record.entries, record.entryProblems,
                           &parsed);
    mod.declarations = parsed.declarations;
    if (!parsed.name.isEmpty()) mod.name = parsed.name;
    if (!parsed.version.isEmpty()) mod.localVersion = parsed.version;
}

ModVersionDeclaration::Match matchModVersion(const Declaration &declaration, const QJsonValue &rule,
                                             const QString &target) {
    if (declaration.loader != ModDeclarations::Loader::Fabric) {
        return ModVersionDeclaration::matches(rule, target);
    }
    const auto result = FabricVersionDeclaration::matches(rule, target);
    if (result == FabricVersionDeclaration::Match::Yes) return ModVersionDeclaration::Match::Yes;
    if (result == FabricVersionDeclaration::Match::No) return ModVersionDeclaration::Match::No;
    return ModVersionDeclaration::Match::Unknown;
}

/// 只读 class 文件的常量池，用来找旧 Forge 的 `@Mod` 注解。
/// 不解析整份 class（字段/方法/属性都跳过），所以坏字节只影响"能不能读出来"。
QStringList constantPoolStrings(const QByteArray &bytes, QString *problem) {
    QStringList strings;
    if (bytes.size() < 10 || static_cast<quint8>(bytes.at(0)) != 0xCA
        || static_cast<quint8>(bytes.at(1)) != 0xFE) {
        if (problem) *problem = QObject::tr("不是有效的 class 文件");
        return strings;
    }
    qsizetype pos = 8;
    const auto u1 = [&](quint8 *out) {
        if (pos + 1 > bytes.size()) return false;
        *out = static_cast<quint8>(bytes.at(pos++));
        return true;
    };
    const auto u2 = [&](quint16 *out) {
        if (pos + 2 > bytes.size()) return false;
        *out = (static_cast<quint16>(static_cast<quint8>(bytes.at(pos))) << 8)
               | static_cast<quint8>(bytes.at(pos + 1));
        pos += 2;
        return true;
    };
    quint16 count = 0;
    if (!u2(&count) || count == 0) {
        if (problem) *problem = QObject::tr("class 常量池长度不可用");
        return strings;
    }
    for (quint16 i = 1; i < count; ++i) {
        quint8 tag = 0;
        if (!u1(&tag)) {
            if (problem) *problem = QObject::tr("class 常量池被截断");
            return strings;
        }
        switch (tag) {
        case 1: {
            quint16 length = 0;
            if (!u2(&length) || pos + length > bytes.size()) {
                if (problem) *problem = QObject::tr("class 字符串常量被截断");
                return strings;
            }
            strings.append(QString::fromUtf8(bytes.mid(pos, length)));
            pos += length;
            break;
        }
        case 3: case 4: pos += 4; break;
        case 5: case 6: pos += 8; ++i; break;
        case 7: case 8: case 16: case 19: case 20: pos += 2; break;
        case 9: case 10: case 11: case 12: case 17: case 18: pos += 4; break;
        case 15: pos += 3; break;
        default:
            if (problem) *problem = QObject::tr("无法识别 Java 类文件常量池的标签 %1").arg(tag);
            return strings;
        }
        if (pos > bytes.size()) {
            if (problem) *problem = QObject::tr("class 常量池越界");
            return strings;
        }
    }
    return strings;
}

/// 旧 Forge 注解里的 `acceptedMinecraftVersions`。常量池里那个名字和值是两条相邻的
/// 字符串常量；名字后面第一条形如 `[1.12,1.13)` / `1.12.2` 的字符串就是它。
QString annotationAcceptedVersions(const QStringList &strings, bool *found, QString *problem) {
    *found = false;
    const int annotation = strings.indexOf(QStringLiteral("Lnet/minecraftforge/fml/common/Mod;"));
    if (annotation < 0) return QString();
    *found = true;
    const int name = strings.indexOf(QStringLiteral("acceptedMinecraftVersions"));
    if (name < 0) {
        if (problem) *problem = QObject::tr("旧 Forge @Mod 没有 acceptedMinecraftVersions");
        return QString();
    }
    static const QRegularExpression range(
        QStringLiteral("^([\\[(].*[\\])]|\\d+(\\.\\d+)*)$"));
    for (int offset = 1; offset <= 8; ++offset) {
        if (name + offset >= strings.size()) break;
        const QString value = strings.at(name + offset);
        if (range.match(value).hasMatch()) return value;
    }
    for (int index = name - 1; index >= 0 && index >= name - 8; --index) {
        const QString value = strings.at(index);
        if (range.match(value).hasMatch()) return value;
    }
    if (problem) *problem = QObject::tr("无法读取旧版 Forge @Mod 的 acceptedMinecraftVersions");
    return QString();
}

Scan scan(const QString &root, const QString &sourceId, const QString &cache, const std::shared_ptr<std::atomic_bool> &stop) {
    Scan result;
    const QDir mods(QDir(root).filePath(QStringLiteral("mods")));
    if (QFileInfo(mods.absolutePath()).isSymLink() || QFileInfo(mods.absolutePath()).isJunction()) { result.note = QObject::tr("模组目录是链接，未扫描。"); return result; }
    QStringList paths;
    for (const auto &file : mods.entryInfoList({QStringLiteral("*.jar")}, QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        if (paths.size() >= 10000) { result.note = QObject::tr("模组超过 10000 个，列表已截断。"); break; }
        paths.append(file.absoluteFilePath());
    }
    InstanceIconLookup::Options options; options.cacheDir = cache; options.cacheKey = sourceId; options.cacheOnly = true;
    const auto cached = InstanceIconLookup::lookup(paths, options);
    QHash<QString, InstanceIconLookup::Found> identities;
    for (const auto &entry : cached.found) identities.insert(entry.path, entry);
    QHash<QString, QList<QJsonObject>> installedRecords;
    const QString auditPath = QDir(root).filePath(QStringLiteral("wtfml.preset-mod-install.json"));
    QFile audit(auditPath);
    if (!QFileInfo(auditPath).isSymLink() && audit.open(QIODevice::ReadOnly) && audit.size() <= 8 * 1024 * 1024) {
        const auto object = QJsonDocument::fromJson(audit.readAll()).object();
        if (object.value(QStringLiteral("format")).toString() == QStringLiteral("wtfml-preset-mod-result/1"))
            for (const auto &value : object.value(QStringLiteral("plannedFiles")).toArray()) {
                const auto record = value.toObject(); installedRecords[record.value(QStringLiteral("filename")).toString()].append(record);
            }
    }
    audit.close();
    QString batchProblem;
    const auto metadata = ZipUtil::readMetadataBatch(paths, ModDeclarations::metadataEntries(),
        &batchProblem, [stop] { return stop->load(); });
    QElapsedTimer identityClock; identityClock.start();
    for (const auto &path : paths) {
        if (stop->load()) break;
        Mod mod; mod.path = path; mod.name = QFileInfo(path).fileName();
        const auto record = metadata.value(path);
        if (!metadata.contains(path)) {
            mod.problem = batchProblem.isEmpty() ? QObject::tr("未检查：本轮未返回声明结果")
                                                 : batchProblem;
        } else {
            mod.problem = record.problem;
        }
        readModDeclarations(mod, record);
        const auto identity = identities.constFind(path);
        const auto records = installedRecords.value(QFileInfo(path).fileName());
        if ((identity != identities.cend() && !identity->sha1.isEmpty()) || !records.isEmpty()) {
            QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha1); bool good = file.open(QIODevice::ReadOnly);
            while (good && !file.atEnd()) {
                if (stop->load() || identityClock.elapsed() > 120000) { good = false; mod.identityProblem = QObject::tr("平台身份待校验：文件哈希检查超时"); break; }
                const auto data = file.read(1024 * 1024); if (data.isEmpty()) { good = false; break; } hash.addData(data);
            }
            const QString actualHash = QString::fromLatin1(hash.result().toHex());
            if (good) mod.sha1 = actualHash;
            if (good && identity != identities.cend() && actualHash == identity->sha1.toLower()) {
                mod.identity = identity->hit; mod.identified = !mod.identity.projectId.isEmpty();
            }
            if (good && !mod.identified) {
                QSet<QString> projects;
                for (const auto &record : records) if (record.value(QStringLiteral("sha1")).toString().toLower() == actualHash) {
                    const QString key = record.value(QStringLiteral("project")).toString();
                    if (key.startsWith(QStringLiteral("modrinth:")) || key.startsWith(QStringLiteral("curseforge:"))) projects.insert(key);
                }
                if (projects.size() == 1) {
                    const QString key = *projects.cbegin();
                    mod.identity.source = key.startsWith(QStringLiteral("curseforge:")) ? ResourceCatalog::Source::CurseForge : ResourceCatalog::Source::Modrinth;
                    mod.identity.projectId = key.mid(key.indexOf(':') + 1); mod.identified = !mod.identity.projectId.isEmpty();
                }
            }
        }
        result.mods.append(mod);
    }
    // 嵌套 JAR / 旧 Forge 注解：都只做**有界**检查（每次 check 都要起一个 PowerShell
    // 进程）。超过预算时明确写“未完整检查”，并按未知处理 —— 不假装看过了。
    QElapsedTimer nestedClock; nestedClock.start();
    int nestedBudget = 200;
    int nestedReadBudget = 32;
    int legacyBudget = 24;
    QTemporaryDir nestedTemp;
    QList<QPair<QString, QPair<int, QString>>> nestedFiles;  // temp 文件 → (mod 下标, 条目名)
    for (int modIndex = 0; modIndex < result.mods.size(); ++modIndex) {
        Mod &mod = result.mods[modIndex];
        if (stop->load()) break;
        if (nestedBudget <= 0 || (g_wallClockBudgets.load() && nestedClock.elapsed() > 10000)) {
            mod.nestedIncomplete = true;
            continue;
        }
        --nestedBudget;
        QStringList entries;
        QString nestedProblem;
        const auto status = ZipUtil::listEntries(mod.path, &entries, &nestedProblem,
                                                 QObject::tr("读取模组嵌套 JAR 列表"));
        if (status != ZipUtil::EntryStatus::Ok) {
            mod.nestedIncomplete = true;
            if (mod.declarations.isEmpty()) {
                Declaration declaration;
                declaration.loader = ModDeclarations::Loader::Forge;
                declaration.basis = QCoreApplication::translate("UpgradeAssessment", "旧 Forge 注解");
                declaration.problem = QObject::tr("读不到模组条目表，旧 Forge 注解无法检查：%1")
                                          .arg(nestedProblem);
                mod.declarations.append(declaration);
            }
            continue;
        }
        for (const QString &entry : entries) {
            if (entry.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)
                && mod.nested.size() < 64) {
                mod.nested.append(entry);
            }
        }
        // 旧 Forge 注解：只在没有任何现代声明时尝试；最多 3 个 class / 一个 JAR。
        if (mod.declarations.isEmpty() && legacyBudget > 0 && (!g_wallClockBudgets.load() || nestedClock.elapsed() < 8000)) {
            QStringList classes;
            for (const QString &entry : entries) {
                if (entry.endsWith(QStringLiteral(".class"), Qt::CaseInsensitive)
                    && !entry.contains(QLatin1Char('$'))) {
                    classes.append(entry);
                }
            }
            std::sort(classes.begin(), classes.end(), [](const QString &left, const QString &right) {
                const bool leftMod = left.contains(QStringLiteral("mod"), Qt::CaseInsensitive);
                const bool rightMod = right.contains(QStringLiteral("mod"), Qt::CaseInsensitive);
                if (leftMod != rightMod) return leftMod;
                return left.size() < right.size();
            });
            const int take = qMin(3, qMin(classes.size(), legacyBudget));
            legacyBudget -= take;
            QString annotationProblem;
            bool annotationFound = false;
            for (int i = 0; i < take; ++i) {
                QByteArray classBytes;
                QString readProblem;
                const auto read = ZipUtil::readEntry(
                    mod.path, classes.at(i), &classBytes, &readProblem,
                    QObject::tr("读取旧 Forge 注解"), 4 * 1024 * 1024);
                if (read != ZipUtil::EntryStatus::Ok) {
                    annotationProblem = readProblem.isEmpty()
                        ? QObject::tr("无法读取旧版 Forge 的注解类文件") : readProblem;
                    continue;
                }
                QString parseProblem;
                const QStringList strings = constantPoolStrings(classBytes, &parseProblem);
                if (!parseProblem.isEmpty()) {
                    annotationProblem = parseProblem;
                    continue;
                }
                bool found = false;
                QString rangeProblem;
                const QString accepted = annotationAcceptedVersions(strings, &found, &rangeProblem);
                if (!found) continue;
                annotationFound = true;
                Declaration declaration;
                declaration.loader = ModDeclarations::Loader::Forge;
                declaration.basis = QCoreApplication::translate("UpgradeAssessment", "旧 Forge @Mod 注解 / acceptedMinecraftVersions");
                declaration.game = accepted;
                declaration.problem = rangeProblem;
                mod.declarations.append(declaration);
                break;
            }
            if (!annotationFound) {
                Declaration declaration;
                declaration.loader = ModDeclarations::Loader::Forge;
                declaration.basis = QCoreApplication::translate("UpgradeAssessment", "旧 Forge 注解");
                declaration.problem = classes.size() > take
                    ? QObject::tr("旧 Forge 注解未完整扫描（候选 %1 个，只看了 %2 个）")
                          .arg(classes.size()).arg(take)
                    : annotationProblem.isEmpty()
                          ? QObject::tr("没有读到旧 Forge @Mod 注解")
                          : QObject::tr("旧 Forge 注解无法解析：%1").arg(annotationProblem);
                // 只有真的像旧模组（有 class 且没有任何现代声明）时才挂这条“未知”。
                if (!classes.isEmpty()) mod.declarations.append(declaration);
            }
        }
        // 嵌套 JAR 的声明：有界地读几份出来，再用一次批量元数据读取。
        if (nestedTemp.isValid()) {
            for (const QString &entry : mod.nested) {
                if (nestedReadBudget <= 0) {
                    NestedCheck check;
                    check.name = entry;
                    check.problem = QObject::tr("嵌套 JAR 超过本轮检查上限，未检查");
                    mod.nestedChecks.append(check);
                    continue;
                }
                --nestedReadBudget;
                QByteArray bytes;
                QString readProblem;
                const auto read = ZipUtil::readEntry(
                    mod.path, entry, &bytes, &readProblem,
                    QObject::tr("读取嵌套 JAR"), 8 * 1024 * 1024);
                if (read != ZipUtil::EntryStatus::Ok || bytes.isEmpty()) {
                    NestedCheck check;
                    check.name = entry;
                    check.problem = readProblem.isEmpty()
                        ? QObject::tr("无法读取嵌套 JAR") : readProblem;
                    mod.nestedChecks.append(check);
                    continue;
                }
                const QString tempPath = nestedTemp.filePath(
                    QStringLiteral("%1-%2.jar").arg(modIndex).arg(nestedFiles.size()));
                QFile file(tempPath);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
                    NestedCheck check;
                    check.name = entry;
                    check.problem = QObject::tr("嵌套 JAR 暂存写入失败");
                    mod.nestedChecks.append(check);
                    continue;
                }
                file.close();
                nestedFiles.append({tempPath, {modIndex, entry}});
            }
        }
    }
    if (!nestedFiles.isEmpty()) {
        QStringList archives;
        for (const auto &nested : nestedFiles) archives.append(nested.first);
        QString nestedBatchProblem;
        const auto nestedMetadata = ZipUtil::readMetadataBatch(
            archives,
            {QStringLiteral("fabric.mod.json"), QStringLiteral("quilt.mod.json"),
             QStringLiteral("META-INF/mods.toml"), QStringLiteral("META-INF/neoforge.mods.toml")},
            &nestedBatchProblem, [stop] { return stop->load(); });
        for (const auto &nested : nestedFiles) {
            const auto record = nestedMetadata.value(nested.first);
            NestedCheck check;
            check.name = nested.second.second;
            if (!nestedMetadata.contains(nested.first) || !record.problem.isEmpty()) {
                check.problem = record.problem.isEmpty() ? nestedBatchProblem : record.problem;
            } else {
                // 复用同一套声明解析（嵌套 JAR 的条目名和顶层不一样，只走 parseEntry）。
                Mod parsed;
                parsed.path = nested.first;
                parsed.name = QFileInfo(nested.first).fileName();
                for (const QString &entry : {QStringLiteral("fabric.mod.json"),
                                             QStringLiteral("quilt.mod.json"),
                                             QStringLiteral("META-INF/mods.toml"),
                                             QStringLiteral("META-INF/neoforge.mods.toml")}) {
                    if (!record.entries.contains(entry)) continue;
                    Declaration declaration;
                    ModDeclarations::parseEntry(entry, record.entries.value(entry), &declaration);
                    declaration.basis =
                        QCoreApplication::translate("UpgradeAssessment", "嵌套 ") + declaration.basis;
                    parsed.declarations.append(declaration);
                }
                // 优先用 quilt / fabric 这种显式声明；没有就取第一条有 game 的。
                const Declaration *chosen = nullptr;
                for (const auto &declaration : parsed.declarations) {
                    if (declaration.loader == ModDeclarations::Loader::Quilt
                        || declaration.loader == ModDeclarations::Loader::Fabric) {
                        chosen = &declaration;
                        break;
                    }
                }
                if (chosen == nullptr) {
                    for (const auto &declaration : parsed.declarations) {
                        if ((!declaration.game.isNull() && !declaration.game.isUndefined())
                            || !declaration.problem.isEmpty()) {
                            chosen = &declaration;
                            break;
                        }
                    }
                }
                if (chosen == nullptr) {
                    check.problem = QObject::tr("嵌套 JAR 没有可读的兼容声明");
                } else if (!chosen->problem.isEmpty()) {
                    check.problem = chosen->problem;
                } else {
                    check.game = chosen->game;
                    check.parsed = true;
                }
            }
            const int index = nested.second.first;
            if (index >= 0 && index < result.mods.size()) {
                result.mods[index].nestedChecks.append(check);
            }
        }
    }
    result.note += QObject::tr("仅检查顶层启用 JAR；按包内声明判断 Minecraft 范围，加载器版本、Java 和前置在本轮由适配替换再核对。嵌套 JAR 只随父 JAR 替换，不单独替换。");
    return result;
}
struct State {
    Scan scan; bool ready = false, started = false; QString target, game, loader, loaderVersion;
    int javaMajor = -1;
    QHash<QString, QString> platform; QList<int> pending;
    QHash<QString, int> active; QHash<int, int> attempts;
    QHash<int, QString> fallbackProjects; QSet<int> fallbackTried;
    QString batchKey;
    int index = 0; quint64 generation = 0;
};
QString localState(const Mod &mod, const QString &game, const QString &loader,
                   const QString &loaderVersion, int javaMajor, QString *basis) {
    if (game.isEmpty()) return QObject::tr("未选择目标");
    if (!mod.problem.isEmpty()) {
        *basis = mod.problem;
        return mod.problem.startsWith(QObject::tr("未检查")) ? QObject::tr("声明尚未检查")
                                                             : QObject::tr("声明读取失败");
    }
    if (loader.isEmpty()) {
        *basis = QObject::tr("纯原版不会加载模组");
        return QObject::tr("目标不使用模组加载器");
    }
    const Declaration *chosen = nullptr;
    for (const auto &declaration : mod.declarations) {
        if (ModDeclarations::loaderKey(declaration.loader) == loader) {
            chosen = &declaration;
            break;
        }
    }
    // Quilt 能加载 Fabric 模组：没有 quilt.mod.json 时按 Fabric 声明检查，但**不拿
    // fabricloader 的范围去卡 Quilt 的加载器版本**（两者不是同一个版本序列）。
    bool fabricFallback = false;
    if (chosen == nullptr && loader == QStringLiteral("quilt")) {
        for (const auto &declaration : mod.declarations) {
            if (declaration.loader == ModDeclarations::Loader::Fabric) {
                chosen = &declaration;
                fabricFallback = true;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        *basis = QObject::tr("未找到 %1 的兼容声明文件").arg(loader);
        return QObject::tr("缺少目标加载器声明");
    }
    if (!chosen->problem.isEmpty()) {
        *basis = chosen->problem;
        return QObject::tr("声明读取失败");
    }
    *basis = chosen->basis + QStringLiteral(": ")
             + QString::fromUtf8(QJsonDocument(QJsonArray{chosen->game})
                                     .toJson(QJsonDocument::Compact));
    if (fabricFallback) {
        *basis += QObject::tr("（按 Fabric 声明；Quilt 可加载 Fabric 模组）");
    }
    const auto matchRule = [chosen](const QJsonValue &rule, const QString &target) {
        return matchModVersion(*chosen, rule, target);
    };
    if (!chosen->breaks.isNull() && !chosen->breaks.isUndefined()) {
        const auto excluded = matchRule(chosen->breaks, game);
        *basis += QStringLiteral("; breaks.minecraft=")
                  + QString::fromUtf8(QJsonDocument(QJsonArray{chosen->breaks})
                                          .toJson(QJsonDocument::Compact));
        if (excluded == ModVersionDeclaration::Match::Yes) {
            return QObject::tr("本地明确排除此版本");
        }
        if (excluded == ModVersionDeclaration::Match::Unknown) {
            return QObject::tr("排除范围无法确定");
        }
    }
    if (chosen->game.isNull() || chosen->game.isUndefined()) {
        return QObject::tr("未声明 Minecraft 版本范围");
    }
    const auto gameMatch = matchRule(chosen->game, game);
    if (gameMatch == ModVersionDeclaration::Match::No) return QObject::tr("本地声明不支持");
    if (gameMatch == ModVersionDeclaration::Match::Unknown) {
        return QObject::tr("声明范围语法尚不支持");
    }
    // ---- 加载器具体版本 ----
    if (!fabricFallback && !chosen->loaderRange.isNull()
        && !chosen->loaderRange.isUndefined()) {
        *basis += QStringLiteral("; ") + chosen->loaderBasis + QStringLiteral("=")
                  + QString::fromUtf8(QJsonDocument(QJsonArray{chosen->loaderRange})
                                          .toJson(QJsonDocument::Compact));
        if (loaderVersion.isEmpty()) {
            return QObject::tr("未选择加载器版本，无法确认");
        }
        const auto loaderMatch = ModVersionDeclaration::matches(chosen->loaderRange, loaderVersion);
        if (loaderMatch == ModVersionDeclaration::Match::No) {
            return QObject::tr("加载器版本不满足（要求 %1，当前 %2）")
                .arg(QString::fromUtf8(QJsonDocument(QJsonArray{chosen->loaderRange})
                                           .toJson(QJsonDocument::Compact)),
                     loaderVersion);
        }
        if (loaderMatch == ModVersionDeclaration::Match::Unknown) {
            return QObject::tr("加载器版本范围无法解析");
        }
    }
    // ---- Java ----
    if (!chosen->javaRange.isNull() && !chosen->javaRange.isUndefined()) {
        *basis += QStringLiteral("; ") + chosen->javaBasis + QStringLiteral("=")
                  + QString::fromUtf8(QJsonDocument(QJsonArray{chosen->javaRange})
                                          .toJson(QJsonDocument::Compact));
        if (javaMajor <= 0) {
            return QObject::tr("目标 Java 版本未确认");
        }
        const auto javaMatch = ModVersionDeclaration::matches(chosen->javaRange,
                                                              QString::number(javaMajor));
        if (javaMatch == ModVersionDeclaration::Match::No) {
            return QObject::tr("需要 Java %1（当前 %2）")
                .arg(QString::fromUtf8(QJsonDocument(QJsonArray{chosen->javaRange})
                                           .toJson(QJsonDocument::Compact)),
                     QString::number(javaMajor));
        }
        if (javaMatch == ModVersionDeclaration::Match::Unknown) {
            return QObject::tr("Java 范围无法解析");
        }
    }
    // ---- 嵌套 JAR 声明 ----
    if (mod.nestedIncomplete) {
        *basis += QObject::tr("；嵌套 JAR 未完整检查");
        return QObject::tr("嵌套 JAR 声明无法确认");
    }
    int nestedMatched = 0;
    for (const auto &check : mod.nestedChecks) {
        if (!check.problem.isEmpty()) {
            *basis += QObject::tr("；嵌套 %1：%2").arg(check.name, check.problem);
            return QObject::tr("嵌套 JAR 声明无法解析");
        }
        if (!check.parsed || check.game.isNull() || check.game.isUndefined()) {
            *basis += QObject::tr("；嵌套 %1：没有可读的 Minecraft 版本范围").arg(check.name);
            return QObject::tr("嵌套 JAR 声明无法确认");
        }
        const auto nestedMatch = ModVersionDeclaration::matches(check.game, game);
        if (nestedMatch == ModVersionDeclaration::Match::No) {
            *basis += QObject::tr("；嵌套 %1：声明不支持目标版本").arg(check.name);
            return QObject::tr("嵌套 JAR 声明不支持");
        }
        if (nestedMatch == ModVersionDeclaration::Match::Unknown) {
            *basis += QObject::tr("；嵌套 %1：声明范围无法解析").arg(check.name);
            return QObject::tr("嵌套 JAR 声明无法确认");
        }
        ++nestedMatched;
    }
    if (nestedMatched > 0) {
        *basis += QObject::tr("；含 %1 个嵌套 JAR，声明均符合").arg(nestedMatched);
    }
    return QObject::tr("本地 Minecraft 声明符合");
}
}

void UpgradeAssessment::setWallClockBudgetsForTesting(bool enabled) { g_wallClockBudgets.store(enabled); }

QHash<QString, UpgradeAssessment::PickerMismatch> UpgradeAssessment::checkDroppedMods(
    const QStringList &paths, const QList<PickerTarget> &targets) {
    if (paths.isEmpty() || targets.isEmpty()) return {};
    const auto archives =
        ZipUtil::readMetadataBatch(paths, ModDeclarations::metadataEntries());
    QHash<QString, PickerMismatch> results;
    for (auto it = archives.cbegin(); it != archives.cend(); ++it) {
        Mod mod;
        mod.path = it.key();
        readModDeclarations(mod, it.value());
        if (mod.declarations.isEmpty()) continue;
        for (const auto &target : targets) {
            const auto supportsLoader = [&target](const Declaration &declaration) {
                const QString key = ModDeclarations::loaderKey(declaration.loader);
                return key == target.loader ||
                       (target.loader == QStringLiteral("quilt") &&
                        key == QStringLiteral("fabric"));
            };
            const bool matchingLoader = std::any_of(mod.declarations.cbegin(), mod.declarations.cend(),
                                                    supportsLoader);
            auto &mismatch = results[target.id];
            mismatch.loader |= !matchingLoader;
            if (target.gameVersion.isEmpty()) continue;
            bool checked = false;
            bool rejected = true;
            for (const auto &declaration : mod.declarations) {
                if (matchingLoader && !supportsLoader(declaration)) continue;
                checked = true;
                const bool excluded = matchModVersion(declaration, declaration.breaks, target.gameVersion)
                    == ModVersionDeclaration::Match::Yes;
                const bool unsupported = matchModVersion(declaration, declaration.game, target.gameVersion)
                    == ModVersionDeclaration::Match::No;
                rejected &= declaration.problem.isEmpty() && (excluded || unsupported);
            }
            mismatch.gameVersion |= checked && rejected;
        }
    }
    return results;
}

void UpgradeAssessment::attach(InstanceSettingsDialog *dialog, VersionManager *versions, ResourceStore *store,
                               const QString &sourceId, const QString &root, const QString &identityCache) {
    const auto state = std::make_shared<State>();
    const auto stop = std::make_shared<std::atomic_bool>(false);
    QObject::connect(dialog, &QObject::destroyed, [stop] { stop->store(true); });
    const auto render = [dialog, state] {
        QList<QStringList> rows;
        QVariantMap replacements;
        QStringList changes;
        QStringList unknownMods;
        if (state->ready) for (const auto &mod : state->scan.mods) {
            QString basis;
            const QString local = localState(mod, state->game, state->loader,
                                             state->loaderVersion, state->javaMajor, &basis);
            const QString platform = state->platform.value(mod.path, mod.identified ? QObject::tr("平台项目已识别，等待查询")
                : mod.identityProblem.isEmpty() ? QObject::tr("未识别平台项目（无有效识别记录）") : mod.identityProblem);
            const QString filename = QFileInfo(mod.path).fileName();
            const QString title = mod.name == filename ? mod.name : mod.name + QCoreApplication::translate("UpgradeAssessment", "（%1）").arg(filename);
            QString replacementLabel;
            QString replacementDefault;
            if (mod.replacementAvailable) {
                const auto file = mod.replacement.primaryFile();
                const QString version = mod.replacement.versionNumber.isEmpty()
                    ? mod.replacement.id : mod.replacement.versionNumber;
                replacementLabel = version;
                replacementDefault = QStringLiteral("replace");
                if (file != nullptr) {
                    const QString platformName = mod.identity.source == ResourceCatalog::Source::CurseForge
                        ? QStringLiteral("curseforge") : QStringLiteral("modrinth");
                    replacements.insert(filename, QVariantMap{
                        {QStringLiteral("title"), mod.name},
                        {QStringLiteral("oldFile"), filename},
                        {QStringLiteral("platform"), platformName},
                        {QStringLiteral("projectId"), mod.identity.projectId},
                        {QStringLiteral("projectKey"), platformName + QLatin1Char(':') + mod.identity.projectId},
                        {QStringLiteral("versionId"), mod.replacement.id},
                        {QStringLiteral("versionNumber"), version},
                        {QStringLiteral("fileName"), file->filename},
                        {QStringLiteral("sha1"), file->sha1},
                        {QStringLiteral("size"), file->size},
                        {QStringLiteral("url"), file->url},
                        {QStringLiteral("fromVersion"), mod.localVersion},
                        {QStringLiteral("nestedCount"), mod.nested.size()},
                        {QStringLiteral("nestedNames"), mod.nested.mid(0, 5)},
                    });
                    changes.append(QObject::tr("替换：mods/%1 → %2（%3 %4，SHA-1 %5…）")
                                       .arg(filename, file->filename, mod.name, version,
                                            file->sha1.left(12)));
                }
            }
            QString nestedNote;
            if (!mod.nested.isEmpty()) {
                nestedNote = QObject::tr("\n含 %1 个嵌套 JAR（随父 JAR 替换，不单独替换）")
                                 .arg(mod.nested.size());
            } else if (mod.nestedIncomplete) {
                nestedNote = QObject::tr("\n嵌套 JAR 未完整枚举");
            }
            if (local != QObject::tr("本地 Minecraft 声明符合")
                && local != QObject::tr("本地声明不支持")
                && local != QObject::tr("本地明确排除此版本")
                && local != QObject::tr("目标不使用模组加载器")) {
                unknownMods.append(QCoreApplication::translate("UpgradeAssessment", "%1：%2（%3）").arg(filename, local, basis));
            }
            const bool knownUnsupported =
                local == QObject::tr("本地声明不支持")
                || local == QObject::tr("本地明确排除此版本")
                || local == QObject::tr("目标不使用模组加载器")
                || local.startsWith(QObject::tr("加载器版本不满足"))
                || local.startsWith(QObject::tr("需要 Java"))
                || local == QObject::tr("嵌套 JAR 声明不支持");
            const bool knownCompatible = local == QObject::tr("本地 Minecraft 声明符合");
            // 未知/解析失败（含嵌套、旧 Forge 注解、加载器/Java 范围读不动）一律**强制选择**：
            // 即便平台有适配版，也不能替用户默认成 replace。
            const QString defaultChoice = (!knownCompatible && !knownUnsupported)
                ? QStringLiteral("review")
                : (!replacementDefault.isEmpty() ? replacementDefault
                                                 : (knownCompatible ? QStringLiteral("keep")
                                                                    : QStringLiteral("disable")));
            // 「版本升级」的语义：本地声明只覆盖旧版本、但平台上有目标发行版的，
            // 属于"可替换"，不是不兼容；只有明确排除且没有替换时才算不兼容。
            const QString bucketKey = !replacementLabel.isEmpty()
                ? QStringLiteral("compatible")
                : (knownUnsupported ? QStringLiteral("incompatible")
                                   : (knownCompatible ? QStringLiteral("compatible")
                                                      : QStringLiteral("unknown")));
            rows.append({title, platform + QLatin1Char('\n') + local, basis + nestedNote, filename,
                defaultChoice,
                knownCompatible && platform.startsWith(QObject::tr("平台有目标发行版"))
                    ? QStringLiteral("compatible") : QString(),
                replacementLabel, bucketKey});
        }
        dialog->setProperty("upgradeModReplacements", replacements);
        dialog->setProperty("upgradeUnknownMods", unknownMods);
        if (changes.isEmpty()) {
            changes.append(QObject::tr("本次不替换模组文件（按逐项选择保留/禁用）。"));
        } else {
            changes.append(QObject::tr("必要前置会在下载阶段自动补齐，并在替换前再次列出实际文件。"));
            changes.append(QObject::tr("替换失败或必要前置失败时整趟回滚，并写出原因报告。"));
        }
        if (!unknownMods.isEmpty()) {
            changes.append(QObject::tr("声明未知/需确认 %1 项（必须逐项选择）：").arg(unknownMods.size()));
            for (int i = 0; i < unknownMods.size() && i < 10; ++i) changes.append(unknownMods.at(i));
            if (unknownMods.size() > 10) changes.append(QObject::tr("…（其余见启动器日志）"));
        }
        dialog->setProperty("upgradeFileChanges", changes);
        const QString assumption = state->target.startsWith(QStringLiteral("@download:"))
            ? (dialog->property("modernUpgradeFlow").toBool()
                ? QObject::tr("按所选加载器 %1 检查。 ") : QObject::tr("目标核心未准备，暂按当前加载器 %1 评估；安装时仍须确认。 "))
                .arg(state->loader.isEmpty() ? QObject::tr("原版 / 未识别") : state->loader) : QString();
        dialog->setUpgradeModChecks(rows, state->ready ? assumption + QObject::tr("%1 个启用模组。%2").arg(rows.size()).arg(state->scan.note) : QObject::tr("正在后台读取本地声明…"));
    };
    const auto advance = std::make_shared<std::function<void()>>();
    *advance = [dialog, store, state, render] {
        // Qt network I/O is asynchronous: four simultaneous requests, not four GUI-blocking threads.
        // Qt 网络是异步的：这里发的是**并发请求**，不是阻塞线程。原来 4 条，遇到国内
        // 直连 Modrinth/CurseForge 抖动时，几十个模组要排很久；提到 10 条并保持渲染合并，
        // 单个平台慢不再拖住整批（失败仍按原逻辑记录/重试）。
        while (state->active.size() < 10 && state->index < state->pending.size()) {
            const int index = state->pending.at(state->index++);
            const auto &mod = state->scan.mods.at(index);
            const int attempt = ++state->attempts[index];
            const QString key = QStringLiteral("upgrade-assess:%1:%2:%3:%4").arg(quintptr(dialog)).arg(state->generation).arg(index).arg(attempt);
            state->active.insert(key, index);
            state->platform[mod.path] = attempt > 1 ? QObject::tr("正在重试平台查询") : QObject::tr("正在查询平台");
            const QString fallback = state->fallbackProjects.value(index);
            store->fetchVersions(key, fallback.isEmpty() ? mod.identity.projectId : fallback,
                fallback.isEmpty() ? mod.identity.source : ResourceCatalog::Source::Modrinth, state->game, state->loader);
        }
        dialog->setProperty("upgradeAssessmentQuerying", !state->active.isEmpty() || state->index < state->pending.size());
        render();
    };
    QObject::connect(store, &ResourceStore::versionsFinished, dialog,
        [store, state, render, advance](const QString &key, const QList<ResourceCatalog::VersionEntry> &entries, const QString &problem) {
            if (!state->active.contains(key)) return;
            const int index = state->active.take(key);
            const auto &mod = state->scan.mods.at(index);
            bool incomplete = false;
            ResourceCatalog::VersionEntry selected;
            for (const auto &entry : entries) {
                if (entry.gameVersions.isEmpty() || entry.loaders.isEmpty()) { incomplete = true; continue; }
                if (!entry.gameVersions.contains(state->game)
                    || !entry.loaders.contains(state->loader, Qt::CaseInsensitive)) continue;
                // 只接受**能安全下载并校验**的那一条：地址、文件名、大小、SHA-1 齐了才算
                // “有目标发行版”。否则界面只能说“元数据不完整”，不能假装可以替换。
                const ResourceCatalog::FileEntry *file = entry.primaryFile();
                if (file == nullptr) continue;
                const QUrl url(file->url);
                if (!url.isValid() || (url.scheme() != QStringLiteral("https")
                                       && url.scheme() != QStringLiteral("http"))
                    || !url.userInfo().isEmpty()
                    || file->filename != QFileInfo(file->filename).fileName()
                    || file->filename.contains(QLatin1Char('\\'))
                    || file->filename.contains(QLatin1Char(':'))
                    || !file->filename.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)
                    || file->size < 0
                    || !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{40}$"))
                            .match(file->sha1).hasMatch()) {
                    incomplete = true;
                    continue;
                }
                selected = entry;
                break;
            }
            auto &mutableMod = state->scan.mods[index];
            mutableMod.replacementAvailable = !selected.id.isEmpty();
            if (mutableMod.replacementAvailable) {
                mutableMod.replacement = selected;
            }
            state->platform[mod.path] = !problem.isEmpty() ? QObject::tr("平台查询失败：%1").arg(problem)
                : selected.id.isEmpty() ? (incomplete ? QObject::tr("平台发行元数据不完整，无法确定")
                                                      : QObject::tr("平台未找到目标发行版"))
                                        : QObject::tr("平台有目标发行版 %1")
                                              .arg(selected.versionNumber.isEmpty() ? selected.id
                                                                                    : selected.versionNumber);
            if (problem.isEmpty() && state->fallbackProjects.contains(index)) state->platform[mod.path] += QObject::tr("（Modrinth 后备）");
            // Retry one transient failure; permanent authentication/permission errors are not retried.
            if (!problem.isEmpty()) {
                qWarning().noquote() << "[InstanceUpdate]" << mod.identity.projectId << problem;
                if (mod.identity.source == ResourceCatalog::Source::CurseForge && !mod.sha1.isEmpty() && !state->fallbackTried.contains(index)) {
                    state->fallbackTried.insert(index);
                    const QString lookupKey = key + QStringLiteral(":hash-fallback");
                    state->active.insert(lookupKey, index);
                    state->platform[mod.path] = QObject::tr("原平台查询失败，正在通过文件哈希核验 Modrinth 后备项目");
                    render(); store->fetchModrinthFiles(lookupKey, {mod.sha1}); (*advance)(); return;
                }
                const bool permanent = problem.contains(QStringLiteral("403")) || problem.contains(QStringLiteral("401"))
                    || problem.contains(QStringLiteral("404")) || problem.contains(QStringLiteral("429"));
                if (!permanent && state->attempts.value(index) < 2) state->pending.append(index);
            }
            render(); (*advance)();
        });
    QObject::connect(store, &ResourceStore::modrinthFilesFinished, dialog,
        [state, render, advance](const QString &key, const QHash<QString, QString> &projects,
            const QHash<QString, ResourceCatalog::VersionEntry> &, const QString &problem) {
            if (!key.endsWith(QStringLiteral(":hash-fallback")) || !state->active.contains(key)) return;
            const int index = state->active.take(key);
            const auto &mod = state->scan.mods.at(index);
            const QString project = projects.value(mod.sha1);
            if (problem.isEmpty() && !project.isEmpty()) {
                state->fallbackProjects.insert(index, project); state->attempts[index] = 0; state->pending.append(index);
            } else {
                state->platform[mod.path] = QObject::tr("原平台查询失败；Modrinth 后备身份%1，仍需确认")
                    .arg(problem.isEmpty() ? QObject::tr("未匹配") : QObject::tr("查询失败：%1").arg(problem));
                if (!problem.isEmpty()) qWarning().noquote() << "[InstanceUpdate] hash fallback:" << problem;
            }
            render(); (*advance)();
        });
    auto byShaBatch = std::make_shared<QHash<QString, int>>();
    QObject::connect(store, &ResourceStore::latestVersionsFinished, dialog,
        [state, render, advance, byShaBatch, dialog](const QString &key,
            const QHash<QString, ResourceCatalog::VersionEntry> &latest, const QString &problem) {
            if (key != state->batchKey) return;
            state->batchKey.clear();
            for (auto it = byShaBatch->cbegin(); it != byShaBatch->cend(); ++it) {
                const int index = it.value();
                const ResourceCatalog::VersionEntry entry = latest.value(it.key());
                auto &mod = state->scan.mods[index];
                if (!problem.isEmpty() || entry.id.isEmpty()) {
                    state->pending.append(index);
                    state->platform[mod.path] = problem.isEmpty()
                        ? QObject::tr("正在查询平台")
                        : QObject::tr("平台查询失败：%1").arg(problem);
                    continue;
                }
                const ResourceCatalog::FileEntry *file = entry.primaryFile();
                const QUrl url(file != nullptr ? file->url : QString());
                const bool usable = file != nullptr && url.isValid()
                    && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"))
                    && url.userInfo().isEmpty() && file->filename == QFileInfo(file->filename).fileName()
                    && !file->filename.contains(QLatin1Char('\\'))
                    && !file->filename.contains(QLatin1Char(':'))
                    && file->filename.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)
                    && file->size >= 0
                    && QRegularExpression(QStringLiteral("^[0-9a-fA-F]{40}$")).match(file->sha1).hasMatch();
                if (!usable) {
                    state->platform[mod.path] = QObject::tr("平台发行元数据不完整，无法确定");
                    continue;
                }
                mod.replacementAvailable = true;
                mod.replacement = entry;
                state->platform[mod.path] = QObject::tr("平台有目标发行版 %1")
                    .arg(entry.versionNumber.isEmpty() ? entry.id : entry.versionNumber);
            }
            dialog->setProperty("upgradeAssessmentQuerying", !state->pending.isEmpty());
            render(); (*advance)();
        });

    const auto select = [dialog, store, state, versions, sourceId, render, advance, byShaBatch](const QString &key, const QString &target) {
        const QString selectedLoader = dialog->property("upgradeTargetLoader").toString();
        if (key != QStringLiteral("upgrade") || (target == state->target && state->ready
            && (!dialog->property("modernUpgradeFlow").toBool() || selectedLoader == state->loader))) return;
        state->target = target; state->platform.clear(); state->pending.clear(); state->attempts.clear(); state->index = 0; state->active.clear(); state->batchKey.clear(); ++state->generation;
        state->fallbackProjects.clear(); state->fallbackTried.clear();
        dialog->setProperty("upgradeAssessmentQuerying", false);
        store->abortAll();
        state->game.clear(); state->loader.clear();
        state->loaderVersion = dialog->property("upgradeTargetLoaderVersion").toString();
        state->javaMajor = dialog->property("upgradeRuntimeJavaMajor").toInt();
        if (target.startsWith(QStringLiteral("@download:"))) {
            state->game = target.mid(10);
            // 加载器名走 `InstancePagesController::loaderName()`，**不读
            // `displayParts().loaderName`**：那一格只有我们自己的安装器会写，PCL / HMCL 合并出来
            // 的实例（加载器只写在版本 json 的 `libraries` 里）在那儿是空的 ⇒ 这一趟会按「原版」
            // 评估模组兼容。写法与 `InstanceUpgradeController.cpp:53/157` 同一套。
            const auto sourceParts = versions->displayParts(sourceId);
            const QString sourceLoader =
                InstancePagesController::loaderName(sourceParts, versions);
            state->loader = dialog->property("modernUpgradeFlow").toBool() ? selectedLoader
                : sourceLoader.toLower();
        }
        else if (!target.isEmpty()) {
            const auto parts = versions->displayParts(target);
            state->game = parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
            QString targetLoaderVersion;
            state->loader =
                InstancePagesController::loaderName(parts, versions, &targetLoaderVersion).toLower();
            if (state->loaderVersion.isEmpty()) state->loaderVersion = targetLoaderVersion;
        }
        render();
        // 先用**一发批量接口**把 Modrinth 身份、哈希可用的模组问一遍（100 个哈希一发），
        // 只有它没返回的那些才退回逐个查询。50 个模组从 50 发请求降到 1 发。
        // 少于 8 个时不值得多这一发往返，直接走原来的逐项查询。
        QStringList sha1s;
        byShaBatch->clear();
        for (int i = 0; i < state->scan.mods.size(); ++i) {
            const Mod &mod = state->scan.mods.at(i);
            if (!mod.identified) continue;
            if (!mod.sha1.isEmpty() && mod.identity.source == ResourceCatalog::Source::Modrinth) {
                sha1s.append(mod.sha1.toLower());
                byShaBatch->insert(mod.sha1.toLower(), i);
            } else state->pending.append(i);
        }
        if (sha1s.size() >= 8) {
            state->batchKey = QStringLiteral("upgrade-assess-batch:%1:%2")
                                  .arg(quintptr(dialog)).arg(state->generation);
            store->fetchLatestVersions(state->batchKey, sha1s, state->game, state->loader);
            dialog->setProperty("upgradeAssessmentQuerying", true);
        } else {
            for (int i = 0; i < state->scan.mods.size(); ++i) {
                if (byShaBatch->contains(state->scan.mods.at(i).sha1.toLower()))
                    state->pending.append(i);
            }
        }
        dialog->setProperty("upgradeAssessmentQuerying", !state->pending.isEmpty());
        const auto generation = state->generation;
        QTimer::singleShot(300, dialog, [state, advance, generation] { if (generation == state->generation) (*advance)(); });
        QTimer::singleShot(180000, dialog, [dialog, store, state, render, generation] {
            if (generation != state->generation || !dialog->property("upgradeAssessmentQuerying").toBool()) return;
            for (const int i : state->active) state->platform[state->scan.mods.at(i).path] = QObject::tr("平台查询超时，不能视为无适配版");
            state->active.clear(); state->batchKey.clear();
            for (int i = state->index; i < state->pending.size(); ++i) state->platform[state->scan.mods.at(state->pending.at(i)).path] = QObject::tr("平台查询超时，不能视为无适配版");
            state->pending.clear(); store->abortAll(); render();
            dialog->setProperty("upgradeAssessmentQuerying", false);
        });
    };
    QObject::connect(dialog, &InstanceSettingsDialog::stopUpgradeQueryRequested, dialog, [dialog, store, state, render] {
        for (const int i : state->active) state->platform[state->scan.mods.at(i).path] = QObject::tr("平台查询已停止，结果未确认");
        state->active.clear(); state->batchKey.clear(); ++state->generation;
        for (int i = state->index; i < state->pending.size(); ++i)
            state->platform[state->scan.mods.at(state->pending.at(i)).path] = QObject::tr("平台查询已停止，结果未确认");
        state->pending.clear(); store->abortAll(); dialog->setProperty("upgradeAssessmentQuerying", false); render();
    });
    QObject::connect(dialog, &InstanceSettingsDialog::transferTargetChosen, dialog, select);
    // 「重新检查」那一颗**不在这儿 findChild**：产品的顺序是开页那一刻就 attach，而「升级」
    // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）—— 这里拿到的是
    // nullptr，`clicked → startScan` 就静默接不上（点了什么都不发生）。改成接对话框自己那一发
    // 信号（按钮在页里发，见 buildLifecyclePage()）；那一颗的可用性也**用时再找**，同一个理由。
    const auto setRefreshEnabled = [dialog](bool on) {
        if (auto *button = dialog->findChild<QPushButton *>(QStringLiteral("instanceUpgradeRecheck"))) {
            button->setEnabled(on);
        }
    };
    const auto startScan = [dialog, store, state, root, sourceId, identityCache, stop, select, render,
                            setRefreshEnabled] {
        if (state->started && !state->ready) return;
        state->started = true; state->ready = false; state->active.clear(); ++state->generation; store->abortAll();
        dialog->setProperty("upgradeAssessmentReading", true); render();
        dialog->setProperty("upgradeAssessmentQuerying", false);
        setRefreshEnabled(false);
        auto *watcher = new QFutureWatcher<QPair<Scan, int>>(dialog);
        QObject::connect(watcher, &QFutureWatcher<QPair<Scan, int>>::finished, dialog,
                         [watcher, dialog, state, select, setRefreshEnabled] {
            const auto result = watcher->result();
            state->scan = result.first;
            dialog->setProperty("upgradeRuntimeJavaMajor", result.second);
            state->ready = true; state->target = QStringLiteral("@invalid");
            dialog->setProperty("upgradeAssessmentReading", false);
            setRefreshEnabled(true);
            select(QStringLiteral("upgrade"), dialog->transferTarget(QStringLiteral("upgrade"))); watcher->deleteLater();
        });
        const QString javaPath = dialog->property("upgradeRuntimeJavaPath").toString();
        const int knownJavaMajor = dialog->property("upgradeRuntimeJavaMajor").toInt();
        watcher->setFuture(QtConcurrent::run([root, sourceId, identityCache, stop, javaPath, knownJavaMajor] {
            const int major = knownJavaMajor > 0 || javaPath.isEmpty()
                ? knownJavaMajor : GlobalJavaSettings::probeJavaMajorVersion(javaPath);
            return qMakePair(scan(root, sourceId, identityCache, stop), major);
        }));
    };
    QObject::connect(dialog, &InstanceSettingsDialog::upgradeRecheckRequested, dialog, startScan);
    QObject::connect(dialog, &InstanceSettingsDialog::transferTargetChosen, dialog,
        [state, startScan](const QString &key, const QString &) {
            if (key != QStringLiteral("upgrade") || state->started) return;
            startScan();
        });
}




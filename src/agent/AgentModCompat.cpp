/**
 * @file AgentModCompat.cpp
 * @brief 兼容性判定（见 .hpp）。
 */

#include "AgentModCompat.hpp"

#include "../operations/FabricVersionDeclaration.hpp"
#include "../operations/ModVersionDeclaration.hpp"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("AgentModCompat", text);
}

bool platformLoaderCompatible(const QStringList &loaders, const QString &target) {
    if (loaders.isEmpty() || target.isEmpty()) return true;
    for (const QString &loader : loaders) {
        if (loader.compare(target, Qt::CaseInsensitive) == 0) return true;
        if (target == QStringLiteral("quilt") &&
            loader.compare(QStringLiteral("fabric"), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString rangeText(const QJsonValue &value) {
    if (value.isString()) return value.toString();
    if (value.isArray()) {
        QStringList parts;
        for (const QJsonValue &item : value.toArray()) {
            if (item.isString()) parts.append(item.toString());
        }
        return parts.join(QStringLiteral(", "));
    }
    return QString();
}

/// 选一条与目标加载器匹配的声明：优先同名，其次 Fabric→Quilt 的回退。
const ModDeclarations::Declaration *chooseDeclaration(const ModDeclarations::Mod &mod,
                                                      const QString &target,
                                                      bool *fabricFallback) {
    if (fabricFallback != nullptr) *fabricFallback = false;
    for (const ModDeclarations::Declaration &declaration : mod.declarations) {
        if (ModDeclarations::loaderKey(declaration.loader) == target) return &declaration;
    }
    if (target == QStringLiteral("quilt")) {
        for (const ModDeclarations::Declaration &declaration : mod.declarations) {
            if (declaration.loader == ModDeclarations::Loader::Fabric) {
                if (fabricFallback != nullptr) *fabricFallback = true;
                return &declaration;
            }
        }
    }
    return nullptr;
}

QString declaredLoaders(const ModDeclarations::Mod &mod) {
    QStringList loaders;
    for (const ModDeclarations::Declaration &declaration : mod.declarations) {
        const QString key = ModDeclarations::loaderKey(declaration.loader);
        if (!loaders.contains(key)) loaders.append(key);
    }
    return loaders.join(QStringLiteral(", "));
}

void addIssue(AgentModCompat::Analysis *analysis, const QString &type, const QString &severity,
              const QString &modId, const QString &fileName, const QString &detail,
              const QStringList &evidence = {}) {
    AgentModCompat::Issue issue;
    issue.type = type;
    issue.severity = severity;
    issue.modId = modId;
    issue.fileName = fileName;
    issue.detail = detail;
    issue.evidence = evidence;
    analysis->issues.append(issue);
    if (type == QStringLiteral("unknown")) ++analysis->unknownCount;
}

}  // namespace

namespace AgentModCompat {

QStringList issueTypes() {
    return {QStringLiteral("missing_dependency"), QStringLiteral("incompatible_version"),
            QStringLiteral("loader_mismatch"), QStringLiteral("duplicate_mod"),
            QStringLiteral("corrupt_file"), QStringLiteral("unknown")};
}

QString modIdOf(const InstalledFile &file) {
    return file.declaration.modId.trimmed();
}

QString matchText(const ModDeclarations::Declaration &declaration, const QJsonValue &rule,
                  const QString &target) {
    if (target.isEmpty()) return QStringLiteral("unknown");
    ModVersionDeclaration::Match match;
    if (declaration.loader == ModDeclarations::Loader::Fabric) {
        const FabricVersionDeclaration::Match fabric = FabricVersionDeclaration::matches(rule, target);
        match = fabric == FabricVersionDeclaration::Match::Yes
                    ? ModVersionDeclaration::Match::Yes
                    : fabric == FabricVersionDeclaration::Match::No
                          ? ModVersionDeclaration::Match::No
                          : ModVersionDeclaration::Match::Unknown;
    } else {
        match = ModVersionDeclaration::matches(rule, target);
    }
    if (match == ModVersionDeclaration::Match::Yes) return QStringLiteral("yes");
    if (match == ModVersionDeclaration::Match::No) return QStringLiteral("no");
    return QStringLiteral("unknown");
}

Analysis analyzeLocal(const QList<InstalledFile> &files, const Target &target) {
    Analysis analysis;
    analysis.scanned = int(files.size());

    // mod id → 提供它的文件（启用与停用都算，用来区分"没装"和"只有停用文件"）。
    QHash<QString, QList<QPair<QString, bool>>> providers;
    for (const InstalledFile &file : files) {
        const QString id = modIdOf(file).toLower();
        if (id.isEmpty()) continue;
        providers[id].append({file.fileName, file.enabled});
    }

    QSet<QString> duplicateReported;
    for (const InstalledFile &file : files) {
        if (!file.readProblem.isEmpty()) {
            addIssue(&analysis, QStringLiteral("corrupt_file"), QStringLiteral("problem"),
                     modIdOf(file), file.fileName, file.readProblem);
            continue;
        }
        if (file.bytes == 0) {
            addIssue(&analysis, QStringLiteral("corrupt_file"), QStringLiteral("problem"),
                     modIdOf(file), file.fileName, tr("文件大小为 0"));
            continue;
        }
        if (file.declaration.declarations.isEmpty()) {
            addIssue(&analysis, QStringLiteral("unknown"), QStringLiteral("info"), QString(), file.fileName,
                     tr("JAR 里没有可识别的模组声明"));
            continue;
        }
        if (!target.loader.isEmpty()) {
            bool fallback = false;
            const ModDeclarations::Declaration *chosen =
                chooseDeclaration(file.declaration, target.loader, &fallback);
            if (chosen == nullptr) {
                addIssue(&analysis, QStringLiteral("loader_mismatch"), QStringLiteral("problem"),
                         modIdOf(file), file.fileName,
                         tr("声明的加载器（%1）与实例（%2）不一致")
                             .arg(declaredLoaders(file.declaration), target.loader));
                continue;
            }
            if (!chosen->problem.isEmpty()) {
                addIssue(&analysis, QStringLiteral("unknown"), QStringLiteral("info"),
                         modIdOf(file), file.fileName, chosen->problem);
            } else {
                if (chosen->game.isNull() || chosen->game.isUndefined()) {
                    addIssue(&analysis, QStringLiteral("unknown"), QStringLiteral("warning"),
                             modIdOf(file), file.fileName,
                             tr("声明里没有 Minecraft 版本范围"));
                } else {
                    const QString game = matchText(*chosen, chosen->game, target.gameVersion);
                    if (game == QStringLiteral("no")) {
                        addIssue(&analysis, QStringLiteral("incompatible_version"),
                                 QStringLiteral("problem"), modIdOf(file), file.fileName,
                                 tr("声明不支持 Minecraft %1（要求 %2）")
                                     .arg(target.gameVersion, rangeText(chosen->game)),
                                 {chosen->basis});
                    } else if (game == QStringLiteral("unknown")) {
                        addIssue(&analysis, QStringLiteral("unknown"), QStringLiteral("info"),
                                 modIdOf(file), file.fileName,
                                 tr("Minecraft 版本范围无法解析：%1").arg(rangeText(chosen->game)));
                    }
                }
                if (!fallback && !chosen->loaderRange.isNull() &&
                    !chosen->loaderRange.isUndefined() && !target.loaderVersion.isEmpty()) {
                    const QString loaderMatch =
                        matchText(*chosen, chosen->loaderRange, target.loaderVersion);
                    if (loaderMatch == QStringLiteral("no")) {
                        addIssue(&analysis, QStringLiteral("incompatible_version"),
                                 QStringLiteral("problem"), modIdOf(file), file.fileName,
                                 tr("需要加载器版本 %1（当前 %2）")
                                     .arg(rangeText(chosen->loaderRange), target.loaderVersion),
                                 {chosen->basis});
                    } else if (loaderMatch == QStringLiteral("unknown")) {
                        addIssue(&analysis, QStringLiteral("unknown"), QStringLiteral("info"),
                                 modIdOf(file), file.fileName,
                                 tr("加载器版本范围无法解析：%1")
                                     .arg(rangeText(chosen->loaderRange)));
                    }
                }
                if (!chosen->javaRange.isNull() && !chosen->javaRange.isUndefined() &&
                    target.javaMajor > 0) {
                    const QString javaMatch = matchText(*chosen, chosen->javaRange,
                                                        QString::number(target.javaMajor));
                    if (javaMatch == QStringLiteral("no")) {
                        addIssue(&analysis, QStringLiteral("incompatible_version"),
                                 QStringLiteral("problem"), modIdOf(file), file.fileName,
                                 tr("需要 Java %1（当前 %2）")
                                     .arg(rangeText(chosen->javaRange),
                                          QString::number(target.javaMajor)),
                                 {chosen->basis});
                    } else if (javaMatch == QStringLiteral("unknown")) {
                        addIssue(&analysis, QStringLiteral("unknown"), QStringLiteral("info"),
                                 modIdOf(file), file.fileName,
                                 tr("Java 范围无法解析：%1").arg(rangeText(chosen->javaRange)));
                    }
                }
            }
        }
        if (modIdOf(file).isEmpty()) {
            addIssue(&analysis, QStringLiteral("unknown"), QStringLiteral("warning"), QString(),
                     file.fileName, tr("读不到 mod id，依赖图里对不上"));
        } else {
            const QString id = modIdOf(file).toLower();
            const auto files = providers.value(id);
            if (files.size() > 1 && !duplicateReported.contains(id)) {
                duplicateReported.insert(id);
                int enabledCount = 0;
                QStringList names;
                for (const auto &item : files) {
                    if (item.second) ++enabledCount;
                    names.append(item.first);
                }
                addIssue(&analysis, QStringLiteral("duplicate_mod"),
                         enabledCount > 1 ? QStringLiteral("problem") : QStringLiteral("warning"),
                         modIdOf(file), files.constFirst().first,
                         tr("同一个 mod id 出现在 %1 个文件里：%2")
                             .arg(files.size())
                             .arg(names.join(QStringLiteral(", "))));
            }
        }
        // 依赖：只对启用的文件算（停用的模组不加载，缺前置是正常的）。
        if (!file.enabled) continue;
        for (const QString &required : file.declaration.requiredModIds) {
            const QString key = required.toLower();
            const auto provided = providers.value(key);
            bool satisfied = false;
            QStringList disabledFiles;
            for (const auto &item : provided) {
                if (item.second) {
                    satisfied = true;
                    break;
                }
                disabledFiles.append(item.first);
            }
            if (satisfied) continue;
            const QString detail = disabledFiles.isEmpty()
                                       ? tr("缺少前置 %1（未安装）").arg(required)
                                       : tr("缺少前置 %1（只有停用文件：%2）")
                                             .arg(required, disabledFiles.join(QStringLiteral(", ")));
            addIssue(&analysis, QStringLiteral("missing_dependency"), QStringLiteral("problem"),
                     modIdOf(file), file.fileName, detail, {required});
        }
    }
    return analysis;
}

void applyPlatform(Analysis *analysis, const QList<InstalledFile> &files,
                   const QList<PlatformEntry> &platform, const Target &target,
                   bool platformComplete) {
    if (analysis == nullptr) return;
    QHash<QString, PlatformEntry> bySha1;
    QSet<QString> installedProjects;
    for (const PlatformEntry &entry : platform) {
        const QString sha1 = entry.sha1.trimmed().toLower();
        if (!sha1.isEmpty()) bySha1.insert(sha1, entry);
        if (!entry.projectId.isEmpty()) installedProjects.insert(entry.projectId);
    }
    for (const InstalledFile &file : files) {
        const QString sha1 = file.sha1.trimmed().toLower();
        if (sha1.isEmpty()) continue;
        const auto it = bySha1.constFind(sha1);
        if (it == bySha1.constEnd()) {
            if (platformComplete) {
                addIssue(analysis, QStringLiteral("unknown"), QStringLiteral("info"), modIdOf(file),
                         file.fileName, tr("平台没有这个哈希的记录（可能是自行构建的版本）"));
            }
            continue;
        }
        const PlatformEntry &entry = it.value();
        if (!entry.problem.isEmpty()) {
            addIssue(analysis, QStringLiteral("unknown"), QStringLiteral("info"), modIdOf(file),
                     file.fileName, entry.problem);
            continue;
        }
        const QString versionNote = entry.versionNumber.isEmpty()
                                        ? entry.versionId
                                        : entry.versionNumber;
        if (!entry.gameVersions.isEmpty() && !target.gameVersion.isEmpty() &&
            !entry.gameVersions.contains(target.gameVersion)) {
            addIssue(analysis, QStringLiteral("incompatible_version"), QStringLiteral("warning"),
                     modIdOf(file), file.fileName,
                     tr("平台记录的这一版（%1）只支持 Minecraft %2，实例是 %3")
                         .arg(versionNote, entry.gameVersions.join(QStringLiteral(", ")),
                              target.gameVersion));
        }
        if (!platformLoaderCompatible(entry.loaders, target.loader)) {
            addIssue(analysis, QStringLiteral("loader_mismatch"), QStringLiteral("warning"),
                     modIdOf(file), file.fileName,
                     tr("平台记录的这一版（%1）的加载器是 %2，实例是 %3")
                         .arg(versionNote, entry.loaders.join(QStringLiteral(", ")),
                              target.loader));
        }
        // 平台版本元数据里的必需前置：按项目号对已装清单，补上"声明没写但平台要求"的那一类。
        for (const QString &project : entry.requiredProjectIds) {
            if (installedProjects.contains(project)) continue;
            addIssue(analysis, QStringLiteral("missing_dependency"), QStringLiteral("warning"),
                     modIdOf(file), file.fileName,
                     tr("平台要求的前置项目 %1 不在已装清单里").arg(project), {project});
        }
    }
}

}  // namespace AgentModCompat

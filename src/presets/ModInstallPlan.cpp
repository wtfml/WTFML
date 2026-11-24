/**
 * @file ModInstallPlan.cpp
 * @brief 实现 ModInstallPlan.hpp（必要/可选、依赖展开、冲突、替代确认与落盘校验）。
 */

#include "ModInstallPlan.hpp"
#include "PresetCompatibility.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSet>

#include <functional>

namespace Presets {

namespace {

[[maybe_unused]] QString planText(const char *source) { return QCoreApplication::translate("Presets", source); }

QString resolvedTitle(const ModResolveInput &input, const ModReference &reference) {
    if (!input.title.trimmed().isEmpty()) {
        return input.title.trimmed();
    }
    if (!reference.name.trimmed().isEmpty()) {
        return reference.name.trimmed();
    }
    return reference.projectId;
}

QString effectiveGameVersion(const ModInstallRequest &request, const ModReference &reference) {
    return reference.gameVersion.trimmed().isEmpty() ? request.gameVersion.trimmed()
                                                     : reference.gameVersion.trimmed();
}

QString effectiveLoader(const ModInstallRequest &request, const ModReference &reference) {
    return normalizeCompatibilityLoader(reference.loader.trimmed().isEmpty() ? request.loader
                                                                            : reference.loader);
}

QString contextText(const QString &gameVersion, const QString &loader) {
    QStringList parts;
    if (!gameVersion.isEmpty()) {
        parts.append(gameVersion);
    }
    if (!loader.isEmpty()) {
        parts.append(loader);
    }
    return parts.join(QLatin1Char(' '));
}

/// 这一版自己的元数据能不能装进这个上下文。
///
/// 两格**都空**时不判：接口有时候不给（尤其 CurseForge 的旧数据），"不知道"不是
/// "不兼容"。只要给了、而且目标值也给了，就必须对上。
bool versionMatches(const ResourceCatalog::VersionEntry &version, const QString &gameVersion,
                    const QString &loader) {
    if (!gameVersion.isEmpty() && !version.gameVersions.isEmpty()) {
        bool hit = false;
        for (const QString &candidate : version.gameVersions) {
            if (candidate.compare(gameVersion, Qt::CaseInsensitive) == 0) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    if (!loader.isEmpty() && !version.loaders.isEmpty()) {
        bool hit = false;
        for (const QString &candidate : version.loaders) {
            if (normalizeCompatibilityLoader(candidate) == normalizeCompatibilityLoader(loader)) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    return true;
}

/// 从列表里挑第一版「上下文对得上、而且真有文件可下」的。
bool chooseVersion(const QList<ResourceCatalog::VersionEntry> &versions,
                   const QString &gameVersion, const QString &loader,
                   ResourceCatalog::VersionEntry *out, QString *why) {
    if (versions.isEmpty()) {
        *why = QCoreApplication::translate("Presets", "平台未提供可用版本。");
        return false;
    }
    bool sawMatch = false;
    QString fileWhy;
    for (const ResourceCatalog::VersionEntry &version : versions) {
        if (!versionMatches(version, gameVersion, loader)) {
            continue;
        }
        sawMatch = true;
        const ResourceCatalog::FileEntry *file = version.primaryFile();
        if (file == nullptr) {
            fileWhy = QCoreApplication::translate("Presets", "此版本无可下载的文件。");
            continue;
        }
        if (!file->unavailableNote.trimmed().isEmpty()) {
            fileWhy = file->unavailableNote;
            continue;
        }
        if (file->url.trimmed().isEmpty()) {
            fileWhy = QCoreApplication::translate("Presets", "此版本的文件无下载地址。");
            continue;
        }
        *out = version;
        return true;
    }
    if (sawMatch && !fileWhy.isEmpty()) {
        *why = fileWhy;
    } else {
        const QString context = contextText(gameVersion, loader);
        *why = context.isEmpty()
                   ? QCoreApplication::translate("Presets", "版本列表中无兼容此实例的发行版。")
                   : QCoreApplication::translate("Presets", "版本列表中无匹配「%1」的发行版。").arg(context);
    }
    return false;
}

QString platformOfKey(const QString &key) {
    const int colon = key.indexOf(QLatin1Char(':'));
    return colon > 0 ? key.left(colon) : QString();
}

}  // namespace

int ModInstallPlan::requiredFailures() const {
    int count = 0;
    for (const ModInstallRow &row : rows) {
        if (row.required && row.isFailure()) {
            ++count;
        }
    }
    return count;
}

int ModInstallPlan::optionalFailures() const {
    int count = 0;
    for (const ModInstallRow &row : rows) {
        if (!row.required && row.isFailure()) {
            ++count;
        }
    }
    return count;
}

QList<ModInstallRow> ModInstallPlan::downloads() const {
    QList<ModInstallRow> out;
    for (const ModInstallRow &row : rows) {
        if (row.status == ModInstallStatus::Download) {
            out.append(row);
        }
    }
    return out;
}

static ModInstallPlan planModInstallPass(const ModInstallRequest &request) {
    ModInstallPlan plan;

    QSet<QString> seen;
    QSet<QString> optionalSeen;
    // 父行 key → 它选中的那一版声明为 incompatible 的项目 key
    QHash<QString, QSet<QString>> incompatible;

    // 已经递进来的引用，按 key 找（替代项目要用）。
    QHash<QString, const ModResolveInput *> inputsByKey;
    for (const ModResolveInput &input : request.references) {
        const QString key = input.reference.key();
        if (!key.isEmpty() && !inputsByKey.contains(key)) {
            inputsByKey.insert(key, &input);
        }
    }

    std::function<void(const ModReference &, bool, const ModResolveInput *, int)> visit;
    const auto expandDependencies = [&](const ModReference &reference,
                                        const ResourceCatalog::VersionEntry &chosen,
                                        const QString &key, int depth) {
        for (const auto &dependency : chosen.dependencies) {
            if (dependency.projectId.trimmed().isEmpty()) continue;
            ModReference depRef;
            depRef.platform = platformOfKey(key);
            depRef.projectId = dependency.projectId.trimmed();
            depRef.name = request.dependencyTitles.value(depRef.key());
            depRef.required = reference.required;
            const QString depKey = depRef.key();
            if (dependency.type == QLatin1String("embedded")) continue;
            if (dependency.type == QLatin1String("optional")) {
                if (!optionalSeen.contains(depKey)) {
                    optionalSeen.insert(depKey);
                    plan.optionalDependencyKeys.append(depKey);
                }
                continue;
            }
            if (dependency.type == QLatin1String("incompatible")) {
                incompatible[key].insert(depKey);
                continue;
            }
            if (!dependency.required() || seen.contains(depKey)) continue;
            if (depth >= request.maxDependencyDepth) {
                ModInstallRow cut;
                cut.reference = depRef;
                cut.key = depKey;
                cut.title = depRef.name.isEmpty() ? depRef.projectId : depRef.name;
                cut.required = depRef.required;
                cut.fromDependency = true;
                cut.status = ModInstallStatus::Unknown;
                cut.problem = QCoreApplication::translate("Presets", "前置链超过 %1 层，未继续展开。").arg(request.maxDependencyDepth);
                plan.rows.append(cut);
                continue;
            }
            visit(depRef, true, nullptr, depth + 1);
        }
    };
    visit = [&](const ModReference &reference, bool fromDependency,
                const ModResolveInput *given, int depth) {
        const QString key = reference.key();
        ModInstallRow row;
        row.reference = reference;
        row.key = key;
        row.required = reference.required;
        row.fromDependency = fromDependency;

        if (key.isEmpty()) {
            row.status = ModInstallStatus::Blocked;
            row.problem = QCoreApplication::translate("Presets", "模组引用缺少平台或项目 ID。");
            plan.rows.append(row);
            return;
        }
        if (seen.contains(key)) {
            return;  // 同一项目只算一次（引用重复 / 依赖环）
        }
        seen.insert(key);

        // ---------- 实例里已经有了吗 ----------
        if (request.installedKeys.contains(key)) {
            row.status = ModInstallStatus::AlreadyInstalled;
            row.title = resolvedTitle(given ? *given : ModResolveInput(), reference);
            row.version = request.installedVersions.value(key);
            if (!row.version.id.isEmpty() && !versionMatches(row.version,
                effectiveGameVersion(request, reference), effectiveLoader(request, reference))) {
                row.status = ModInstallStatus::Incompatible;
                row.problem = QCoreApplication::translate("Presets", "已安装发行版不兼容目标游戏版本或加载器，未自动替换。");
            }
            plan.rows.append(row);
            if (row.status == ModInstallStatus::AlreadyInstalled)
                expandDependencies(reference, row.version, key, depth);
            return;
        }

        // ---------- 拿这一条的答复 ----------
        ModResolveInput local;
        const ModResolveInput *input = given;
        if (input == nullptr) {
            if (request.dependencyAnswers.contains(key)) {
                local.reference = reference;
                local.queried = true;
                local.versions = request.dependencyAnswers.value(key);
                local.title = request.dependencyTitles.value(key);
                input = &local;
            }
        }
        if (input != nullptr) {
            row.title = resolvedTitle(*input, reference);
        } else {
            row.title = resolvedTitle(ModResolveInput(), reference);
        }
        if (row.title.isEmpty()) {
            row.title = reference.projectId;
        }

        if (input == nullptr) {
            row.status = ModInstallStatus::Unknown;
            row.problem = fromDependency
                              ? QCoreApplication::translate("Presets", "尚未取得此必需依赖的版本列表。")
                              : QCoreApplication::translate("Presets", "尚未从平台取得下载信息，暂时无法下载。");
            plan.rows.append(row);
            return;
        }
        if (!input->queried) {
            row.status = ModInstallStatus::Unknown;
            row.problem = QCoreApplication::translate("Presets", "等待查询平台。");
            plan.rows.append(row);
            return;
        }
        if (input->queryFailed) {
            row.status = ModInstallStatus::Unknown;
            row.problem = input->queryProblem.trimmed().isEmpty()
                              ? QCoreApplication::translate("Presets", "平台查询失败（网络或接口无响应）。")
                              : input->queryProblem.trimmed();
            plan.rows.append(row);
            return;
        }

        const QString gameVersion = effectiveGameVersion(request, reference);
        const QString loader = effectiveLoader(request, reference);
        ResourceCatalog::VersionEntry chosen;
        QString why;
        if (!chooseVersion(input->versions, gameVersion, loader, &chosen, &why)) {
            // ---------- 缺失：先看有没有用户确认过的替代 ----------
            const QList<ModReference> alternatives = request.alternativesByKey.value(key);
            ModReference confirmed;
            bool hasConfirmed = false;
            for (const ModReference &candidate : alternatives) {
                if (!candidate.key().isEmpty() &&
                    request.confirmedAlternatives.contains(candidate.key())) {
                    confirmed = candidate;
                    hasConfirmed = true;
                    break;
                }
            }
            if (hasConfirmed) {
                const ModResolveInput *altInput = inputsByKey.value(confirmed.key());
                const QString altTitle =
                    altInput != nullptr ? resolvedTitle(*altInput, confirmed)
                                        : (confirmed.name.isEmpty() ? confirmed.projectId
                                                                    : confirmed.name);
                // 替代项也走同一条解析路：它自己照样要过"上下文 + 文件"那两关，
                // 过不了就是它自己的失败（原那一条**不再单出一行**，免得同一条在表上
                // 出现两次、用户不知道该看哪一行）。
                visit(confirmed, fromDependency, altInput, depth);
                bool marked = false;
                for (int index = plan.rows.size() - 1; index >= 0; --index) {
                    ModInstallRow &candidate = plan.rows[index];
                    if (candidate.key != confirmed.key()) {
                        continue;
                    }
                    candidate.substituted = true;
                    candidate.substitutedKey = confirmed.key();
                    candidate.replacesKeys.append(key);
                    candidate.replacesKeys.removeDuplicates();
                    candidate.details.append(QCoreApplication::translate("Presets", "本次替代：%1 → %2。").arg(key, candidate.key));
                    candidate.details.append(modInstallSubstitutedText(altTitle));
                    marked = true;
                    break;
                }
                if (!marked) {
                    // 替身那一行本来就在表上（例如它自己也是预设里的一条）时，
                    // 原条目不能就这么消失 —— 留一行点明"按你确认的那一条走"。
                    row.status = ModInstallStatus::Missing;
                    row.problem = why;
                    row.details.append(
                        QCoreApplication::translate("Presets", "已按确认结果替换为「%1」。").arg(altTitle));
                    plan.rows.append(row);
                }
                return;
            }
            row.status = ModInstallStatus::Missing;
            row.problem = why;
            if (!alternatives.isEmpty()) {
                row.status = ModInstallStatus::AlternativeNeedsConfirm;
                QStringList names;
                for (const ModReference &candidate : alternatives) {
                    names.append(candidate.name.trimmed().isEmpty() ? candidate.projectId
                                                                    : candidate.name.trimmed());
                }
                row.details.append(modInstallAlternativeText(names));
            }
            plan.rows.append(row);
            return;
        }

        row.version = chosen;
        const ResourceCatalog::FileEntry *file = chosen.primaryFile();
        row.file = *file;
        row.status = ModInstallStatus::Download;
        plan.rows.append(row);

        expandDependencies(reference, chosen, key, depth);
    };

    for (const ModResolveInput &input : request.references) {
        visit(input.reference, false, &input, 0);
    }

    // ---------- 冲突复核：声明了 incompatible 的那一项在这一趟里 / 已经装着 ----------
    QSet<QString> planned;
    QHash<QString, QString> titleByKey;
    for (const ModInstallRow &row : plan.rows) {
        titleByKey.insert(row.key, row.title);
        if (row.status == ModInstallStatus::Download ||
            row.status == ModInstallStatus::AlreadyInstalled) {
            planned.insert(row.key);
        }
    }
    for (ModInstallRow &row : plan.rows) {
        if (row.key.isEmpty()) {
            continue;
        }
        const QSet<QString> bad = incompatible.value(row.key);
        for (const QString &other : bad) {
            if (other == row.key) {
                continue;
            }
            const bool inPlan = planned.contains(other);
            const bool onDisk = request.installedKeys.contains(other);
            if (!inPlan && !onDisk) {
                continue;
            }
            const QString otherTitle = titleByKey.value(other, other);
            row.status = ModInstallStatus::Incompatible;
            row.problem = QCoreApplication::translate("Presets", "此版本与「%1」不兼容，未加入安装计划。")
                              .arg(otherTitle);
            plan.conflicts.append(
                QCoreApplication::translate("Presets", "「%1」和「%2」不能一起装。").arg(row.title, otherTitle));
            break;
        }
    }

    // Requiredness belongs to the complete dependency graph, not DFS encounter
    // order. A shared dependency first visited from an optional parent may also
    // be needed by a required root later in the same request.
    QSet<QString> requiredKeys;
    for (const auto &input : request.references)
        if (input.reference.required) requiredKeys.insert(input.reference.key());
    for (const auto &row : plan.rows) if (row.required) requiredKeys.insert(row.key);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &row : plan.rows) {
            if (!requiredKeys.contains(row.key)) continue;
            for (const auto &dependency : row.version.dependencies) {
                if (!dependency.required() || dependency.projectId.isEmpty()) continue;
                const QString key = platformOfKey(row.key) + QLatin1Char(':') + dependency.projectId;
                if (!requiredKeys.contains(key)) { requiredKeys.insert(key); changed = true; }
            }
        }
    }
    for (auto &row : plan.rows) {
        if (requiredKeys.contains(row.key)) { row.required = true; row.reference.required = true; }
    }

    // Merge exact constraints before publishing the plan. Project-only identity is
    // insufficient for pinned dependencies; never report an unknown on-disk version as satisfied.
    QHash<QString, QSet<QString>> exactVersions;
    for (const auto &row : plan.rows) {
        if (row.status != ModInstallStatus::Download && row.status != ModInstallStatus::AlreadyInstalled) continue;
        for (const auto &dependency : row.version.dependencies) {
            if (!dependency.required() || dependency.versionId.trimmed().isEmpty() ||
                dependency.projectId.trimmed().isEmpty()) continue;
            const QString key = platformOfKey(row.key) + QLatin1Char(':') + dependency.projectId.trimmed();
            exactVersions[key].insert(dependency.versionId.trimmed());
        }
    }
    for (auto &row : plan.rows) {
        if (row.status != ModInstallStatus::Download && row.status != ModInstallStatus::AlreadyInstalled) continue;
        for (const auto &dependency : row.version.dependencies) {
            if (!dependency.required()) continue;
            if (dependency.projectId.trimmed().isEmpty() && dependency.versionId.trimmed().isEmpty()) {
                row.status = ModInstallStatus::Unknown;
                row.problem = QCoreApplication::translate("Presets", "必要前置缺少有效的项目 / 发行版身份，未允许安装。"); break;
            }
            if (dependency.versionId.trimmed().isEmpty()) continue;
            if (dependency.projectId.trimmed().isEmpty()) {
                row.status = ModInstallStatus::Unknown;
                row.problem = QCoreApplication::translate("Presets", "必要前置只提供发行版 ID（%1），尚无法确认所属项目，未允许安装。")
                                  .arg(dependency.versionId);
                break;
            }
            const QString key = platformOfKey(row.key) + QLatin1Char(':') + dependency.projectId.trimmed();
            if (exactVersions.value(key).size() > 1) {
                QStringList versions = exactVersions.value(key).values();
                versions.sort();
                row.status = ModInstallStatus::Incompatible;
                row.problem = QCoreApplication::translate("Presets", "「%1」存在互不相容的精确前置版本要求：%2。")
                                  .arg(key, versions.join(QStringLiteral(" / ")));
                if (!plan.conflicts.contains(row.problem)) plan.conflicts.append(row.problem);
                break;
            }
            if (!request.installedKeys.contains(key)) continue;
            const QString installed = request.installedVersionIds.value(key).trimmed();
            if (installed == dependency.versionId.trimmed()) continue;
            row.status = installed.isEmpty() ? ModInstallStatus::Unknown : ModInstallStatus::Incompatible;
            row.problem = installed.isEmpty()
                ? QCoreApplication::translate("Presets", "已安装「%1」，但发行版身份未知，无法确认所需版本 %2。")
                      .arg(key, dependency.versionId)
                : QCoreApplication::translate("Presets", "已安装「%1」的版本 %2，不满足所需版本 %3；需确认替换。")
                      .arg(key, installed, dependency.versionId);
            if (!installed.isEmpty()) plan.conflicts.append(row.problem);
            break;
        }
    }

    // A dependency failure belongs to every parent that needs it, including
    // optional roots. Resolve the graph to a fixed point; valid cycles remain valid.
    QHash<QString, int> rowByKey;
    for (int i = 0; i < plan.rows.size(); ++i) rowByKey.insert(plan.rows[i].key, i);
    for (int pass = 0; pass < plan.rows.size(); ++pass) {
        bool changed = false;
        for (auto &row : plan.rows) {
            if (row.status != ModInstallStatus::Download && row.status != ModInstallStatus::AlreadyInstalled) continue;
            for (const auto &dependency : row.version.dependencies) {
                if (!dependency.required() || dependency.projectId.isEmpty()) continue;
                const QString key = platformOfKey(row.key) + QLatin1Char(':') + dependency.projectId;
                if (!rowByKey.contains(key)) continue;
                const auto child = plan.rows.at(rowByKey.value(key));
                if (!child.isFailure()) continue;
                row.status = child.status == ModInstallStatus::Incompatible
                    ? ModInstallStatus::Incompatible : ModInstallStatus::Unknown;
                row.problem = QCoreApplication::translate("Presets", "必要前置「%1」未解决：%2").arg(child.title, child.problem);
                changed = true; break;
            }
        }
        if (!changed) break;
    }

    // ---------- 总账 ----------
    for (const ModInstallRow &row : plan.rows) {
        switch (row.status) {
        case ModInstallStatus::Download:
            ++plan.download;
            break;
        case ModInstallStatus::AlreadyInstalled:
            ++plan.alreadyInstalled;
            break;
        case ModInstallStatus::Missing:
            ++plan.missing;
            break;
        case ModInstallStatus::Unknown:
            ++plan.unknown;
            break;
        case ModInstallStatus::Incompatible:
            ++plan.incompatible;
            break;
        case ModInstallStatus::AlternativeNeedsConfirm:
            ++plan.needsConfirm;
            break;
        case ModInstallStatus::Blocked:
            ++plan.blocked;
            break;
        }
        if (row.substituted) {
            ++plan.substituted;
        }
    }
    plan.ok = !plan.hasRequiredFailure();
    return plan;
}

ModInstallPlan planModInstall(const ModInstallRequest &request) {
    ModInstallRequest constrained = request;
    QSet<QString> seen;
    ModInstallPlan plan;
    for (int pass = 0; pass < 256; ++pass) {
        plan = planModInstallPass(constrained);
        if (!plan.conflicts.isEmpty()) return plan;
        QHash<QString, QString> pins;
        for (const auto &row : plan.rows) for (const auto &dependency : row.version.dependencies) {
            if (!dependency.required() || dependency.versionId.isEmpty() || dependency.projectId.isEmpty()) continue;
            const QString key = platformOfKey(row.key) + QLatin1Char(':') + dependency.projectId;
            if (pins.contains(key) && pins.value(key) != dependency.versionId) return plan;
            pins.insert(key, dependency.versionId);
        }
        bool changed = false;
        for (const auto &row : plan.rows)
            if (row.isDownload() && pins.contains(row.key) && row.version.id != pins.value(row.key)) changed = true;
        if (!changed) return plan;
        QStringList signature;
        for (const auto &row : plan.rows) signature.append(row.key + QLatin1Char('=') + row.version.id);
        signature.sort();
        const QString state = signature.join(QLatin1Char('\n'));
        if (seen.contains(state)) break;
        seen.insert(state);
        // Always filter the original candidate sets. A later pin may legitimately
        // select a different version; do not permanently discard that candidate.
        constrained = request;
        const auto filter = [&](const QString &key, QList<ResourceCatalog::VersionEntry> &versions) {
            if (!pins.contains(key)) return;
            for (int i = versions.size() - 1; i >= 0; --i)
                if (versions[i].id != pins.value(key)) versions.removeAt(i);
        };
        for (auto &input : constrained.references) filter(input.reference.key(), input.versions);
        for (auto it = constrained.dependencyAnswers.begin(); it != constrained.dependencyAnswers.end(); ++it)
            filter(it.key(), it.value());
    }
    plan.ok = false;
    plan.conflicts.append(QCoreApplication::translate("Presets", "精确前置约束循环变化，无法生成稳定计划；未允许安装。"));
    for (auto &row : plan.rows) if (row.isDownload()) {
        row.status = ModInstallStatus::Unknown;
        row.problem = plan.conflicts.last();
        --plan.download; ++plan.unknown;
    }
    return plan;
}

DownloadVerification verifyDownloadedFile(const QString &path, qint64 expectedSize,
                                          const QString &expectedSha1) {
    DownloadVerification out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        out.problem = QCoreApplication::translate("Presets", "无法读取刚下载的文件「%1」：%2").arg(path, file.errorString());
        return out;
    }
    out.size = file.size();

    QCryptographicHash hash(QCryptographicHash::Sha1);
    qint64 total = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1 << 16);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            out.problem = QCoreApplication::translate("Presets", "读「%1」读到一半出错了：%2").arg(path, file.errorString());
            return out;
        }
        total += chunk.size();
        hash.addData(chunk);
    }
    out.size = total;
    out.sha1 = QString::fromLatin1(hash.result().toHex());

    if (expectedSize >= 0 && total != expectedSize) {
        out.problem = QCoreApplication::translate("Presets", "文件大小不符。预期 %1 字节，实际 %2 字节。")
                          .arg(expectedSize)
                          .arg(total);
        return out;
    }
    if (!expectedSha1.trimmed().isEmpty()) {
        if (out.sha1.compare(expectedSha1.trimmed(), Qt::CaseInsensitive) != 0) {
            out.problem = QCoreApplication::translate("Presets", "SHA-1 校验失败。预期 %1，实际 %2。")
                              .arg(expectedSha1.trimmed(), out.sha1);
            return out;
        }
        out.ok = true;
        return out;
    }
    // 接口没给哈希：大小对上了也不能说"校验通过"。
    out.ok = true;
    out.unverified = true;
    out.problem = QCoreApplication::translate("Presets", "平台未提供此文件的 SHA-1，仅核对了文件大小。");
    return out;
}

// ---------------------------------------------------------------------------
// 字
// ---------------------------------------------------------------------------

QString modInstallStatusText(ModInstallStatus status) {
    switch (status) {
    case ModInstallStatus::Download:
        return QCoreApplication::translate("Presets", "下载");
    case ModInstallStatus::AlreadyInstalled:
        return QCoreApplication::translate("Presets", "已经在实例里");
    case ModInstallStatus::Missing:
        return QCoreApplication::translate("Presets", "缺失");
    case ModInstallStatus::Unknown:
        return QCoreApplication::translate("Presets", "暂无法确认");
    case ModInstallStatus::Incompatible:
        return QCoreApplication::translate("Presets", "不兼容");
    case ModInstallStatus::AlternativeNeedsConfirm:
        return QCoreApplication::translate("Presets", "待确认的替代项目");
    case ModInstallStatus::Blocked:
        return QCoreApplication::translate("Presets", "无法完成");
    }
    return QString();
}

QString modInstallSummaryText(const ModInstallPlan &plan) {
    QStringList parts;
    if (plan.download > 0) {
        parts.append(QCoreApplication::translate("Presets", "下载 %1 个").arg(plan.download));
    }
    if (plan.alreadyInstalled > 0) {
        parts.append(QCoreApplication::translate("Presets", "已经在实例里 %1 个").arg(plan.alreadyInstalled));
    }
    if (plan.missing > 0) {
        parts.append(QCoreApplication::translate("Presets", "缺失 %1 个").arg(plan.missing));
    }
    if (plan.unknown > 0) {
        parts.append(QCoreApplication::translate("Presets", "暂无法确认 %1 个").arg(plan.unknown));
    }
    if (plan.incompatible > 0) {
        parts.append(QCoreApplication::translate("Presets", "不兼容 %1 个").arg(plan.incompatible));
    }
    if (plan.needsConfirm > 0) {
        parts.append(QCoreApplication::translate("Presets", "有 %1 个替代项目待确认").arg(plan.needsConfirm));
    }
    if (plan.blocked > 0) {
        parts.append(QCoreApplication::translate("Presets", "无法完成 %1 个").arg(plan.blocked));
    }
    const int required = plan.requiredFailures();
    if (required > 0) {
        parts.append(QCoreApplication::translate("Presets", "其中必要项失败 %1 个").arg(required));
    }
    if (parts.isEmpty()) {
        return QCoreApplication::translate("Presets", "无待处理内容。");
    }
    return parts.join(QCoreApplication::translate("Presets", "，")) + QCoreApplication::translate("Presets", "。");
}

QString modInstallSubstitutedText(const QString &title) {
    return QCoreApplication::translate("Presets", "已确认并替换为「%1」。").arg(title);
}

QString modInstallAlternativeText(const QStringList &names) {
    return QCoreApplication::translate("Presets", "以下替代项目需要确认：%1。")
        .arg(names.join(QCoreApplication::translate("Presets", "、")));
}

QString modInstallPartialText() {
    return QCoreApplication::translate("Presets", "实例可用，预设部分应用。");
}

}  // namespace Presets

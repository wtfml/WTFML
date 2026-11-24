/**
 * @file ModDependencies.cpp
 * @brief ModDependencies.hpp 的实现。**一行网络、一个文件都不碰**（见那边文件头）。
 */

#include "ModDependencies.hpp"
#include <QCoreApplication>
#include "DependencyInspection.hpp"

namespace ModDependencies {

namespace {
bool knownDependencyType(const QString &type) {
    return type == QStringLiteral("required") || type == QStringLiteral("optional") ||
           type == QStringLiteral("incompatible") || type == QStringLiteral("embedded");
}

QString dependencyLabel(const ResourceCatalog::Dependency &d, const QHash<QString, QString> &names) {
    QString label = names.value(d.projectId, d.projectId);
    if (label.isEmpty()) label = QCoreApplication::translate("ModDependencies", "未提供项目号");
    const QString type = d.type == QStringLiteral("required") ? QCoreApplication::translate("ModDependencies", "必需")
        : d.type == QStringLiteral("optional") ? QCoreApplication::translate("ModDependencies", "可选")
        : d.type == QStringLiteral("incompatible") ? QCoreApplication::translate("ModDependencies", "不兼容")
        : d.type == QStringLiteral("embedded") ? QCoreApplication::translate("ModDependencies", "已内嵌") : QCoreApplication::translate("ModDependencies", "类型未知");
    return label + QStringLiteral(" [%1%2]").arg(type,
        d.versionId.isEmpty() ? QString() : QCoreApplication::translate("ModDependencies", "，固定版本 %1").arg(d.versionId));
}

QStringList dependencyWarnings(const ResourceCatalog::Dependency &d,
                               const QHash<QString, QString> &versions,
                               const QHash<QString, QString> &names, const QString &scope) {
    QStringList lines;
    const QString label = dependencyLabel(d, names);
    if (d.required()) {
        if (d.projectId.isEmpty() || !versions.contains(d.projectId))
            lines.append(QCoreApplication::translate("ModDependencies", "需确认：%1 未在%2中找到，不等于确定未安装。此步骤不会自动补装依赖。").arg(label, scope));
        else if (!d.versionId.isEmpty() && versions.value(d.projectId).isEmpty())
            lines.append(QCoreApplication::translate("ModDependencies", "需确认：%1 已识别项目，但版本号未知。").arg(label));
        else if (!d.versionId.isEmpty() && versions.value(d.projectId) != d.versionId)
            lines.append(QCoreApplication::translate("ModDependencies", "版本不符：%1 与%2版本不一致。").arg(label, scope));
    } else if (d.type == QStringLiteral("incompatible") && versions.contains(d.projectId)) {
        const auto installed = versions.value(d.projectId);
        if (d.versionId.isEmpty() || installed == d.versionId)
            lines.append(QCoreApplication::translate("ModDependencies", "冲突提示：已识别清单中存在 %1。").arg(label));
        else if (installed.isEmpty())
            lines.append(QCoreApplication::translate("ModDependencies", "需确认：%1 的已安装版本未知，无法判断是否命中冲突。").arg(label));
    } else if (!knownDependencyType(d.type) || d.projectId.isEmpty()) {
        lines.append(QCoreApplication::translate("ModDependencies", "需确认：依赖字段不完整，不能自动判定兼容。"));
    }
    return lines;
}
}

QString updateDifferenceText(const ResourceCatalog::VersionEntry &current,
                             const ResourceCatalog::VersionEntry &target,
                             const QHash<QString, QString> &plannedVersions,
                             const QHash<QString, QString> &projectNames) {
    const auto versionName = [](const ResourceCatalog::VersionEntry &v) {
        return v.id.isEmpty() ? QCoreApplication::translate("ModDependencies", "未知（未取得当前版本资料）")
            : !v.versionNumber.isEmpty() ? v.versionNumber : !v.name.isEmpty() ? v.name : v.id;
    };
    const auto depName = [&](const ResourceCatalog::Dependency &d) { return dependencyLabel(d, projectNames); };
    QStringList lines{QCoreApplication::translate("ModDependencies", "版本：%1 → %2").arg(versionName(current), versionName(target))};
    if (!target.dependencyMetadataKnown) {
        lines.append(QCoreApplication::translate("ModDependencies", "依赖：目标版本未提供依赖资料，不能判断变化。"));
        return lines.join(QLatin1Char('\n'));
    }
    const auto key = [](const ResourceCatalog::Dependency &d) {
        return d.projectId.isEmpty() ? QStringLiteral("version:") + d.versionId : QStringLiteral("project:") + d.projectId;
    };
    QHash<QString, ResourceCatalog::Dependency> before;
    bool complete = true;
    QSet<QString> oldKeys, newKeys;
    for (const auto &d : current.dependencies) {
        if (!knownDependencyType(d.type) || (d.projectId.isEmpty() && d.versionId.isEmpty()) || oldKeys.contains(key(d))) complete = false;
        oldKeys.insert(key(d));
    }
    for (const auto &d : target.dependencies) {
        if (!knownDependencyType(d.type) || (d.projectId.isEmpty() && d.versionId.isEmpty()) || newKeys.contains(key(d))) complete = false;
        newKeys.insert(key(d));
    }
    if (!complete) lines.append(QCoreApplication::translate("ModDependencies", "需确认：依赖字段缺失、类型未知或项目重复，部分差异无法确定。"));
    for (const auto &d : current.dependencies) before.insert(key(d), d);
    QSet<QString> seen;
    bool changes = false;
    if (!current.dependencyMetadataKnown)
        lines.append(QCoreApplication::translate("ModDependencies", "当前依赖资料未知，以下仅列目标要求，不标为新增或移除。"));
    for (const auto &d : target.dependencies) {
        seen.insert(key(d));
        const auto old = before.constFind(key(d));
        if (!current.dependencyMetadataKnown) lines.append(QCoreApplication::translate("ModDependencies", "目标要求：") + depName(d));
        else if (old == before.cend()) { lines.append(QCoreApplication::translate("ModDependencies", "新增依赖：") + depName(d)); changes = true; }
        else if (old->type != d.type || old->versionId != d.versionId) {
            lines.append(QCoreApplication::translate("ModDependencies", "依赖变化：%1 → %2").arg(depName(*old), depName(d))); changes = true;
        }
        lines.append(dependencyWarnings(d, plannedVersions, projectNames, QCoreApplication::translate("ModDependencies", "本次更新后的已识别清单")));
    }
    if (current.dependencyMetadataKnown) {
        QStringList removed;
        for (auto it = before.cbegin(); it != before.cend(); ++it)
            if (!seen.contains(it.key())) removed.append(QCoreApplication::translate("ModDependencies", "移除声明：") + depName(it.value()));
        removed.sort(); lines.append(removed);
        if (complete && !changes && removed.isEmpty()) lines.append(QCoreApplication::translate("ModDependencies", "依赖声明未变化。"));
    }
    lines.append(QCoreApplication::translate("ModDependencies", "移除声明不删除现有模组；此预览不是完整依赖求解或启动兼容保证。"));
    return lines.join(QLatin1Char('\n'));
}

QString inspectInstalledDependencies(const QList<InstalledItem> &selected,
                                     const QList<InstalledItem> &inventory) {
    const auto disabled = [](const InstalledItem &item) { return item.path.endsWith(QStringLiteral(".disabled"), Qt::CaseInsensitive); };
    const auto fileName = [](const InstalledItem &item) {
        return item.title.isEmpty() ? QString(item.path).replace(QLatin1Char('\\'), QLatin1Char('/')).section(QLatin1Char('/'), -1) : item.title;
    };
    const auto projectKey = [](const InstalledItem &item) {
        ResourceCatalog::Hit hit; hit.projectId = item.projectId; hit.source = item.source; return hit.iconCacheId();
    };
    QHash<QString, QStringList> projectFiles;
    QSet<QString> seenPaths;
    int recognized = 0, unknown = 0;
    for (const auto &item : inventory) {
        if (disabled(item) || seenPaths.contains(item.path)) continue;
        seenPaths.insert(item.path);
        if (item.projectId.isEmpty()) { ++unknown; continue; }
        ++recognized;
        projectFiles[projectKey(item)].append(QString(item.path).replace(QLatin1Char('\\'), QLatin1Char('/')).section(QLatin1Char('/'), -1));
    }
    QStringList lines{QCoreApplication::translate("ModDependencies", "选中 %1 项 / 已识别启用文件 %2 项 / 未识别启用文件 %3 项")
        .arg(selected.size()).arg(recognized).arg(unknown)};
    QStringList duplicates;
    for (auto it = projectFiles.cbegin(); it != projectFiles.cend(); ++it)
        if (it.value().size() > 1) duplicates.append(QCoreApplication::translate("ModDependencies", "重复启用：%1（%2）。不自动选定版本或删除文件。")
            .arg(it.key(), it.value().join(QStringLiteral(" / "))));
    duplicates.sort(); lines.append(duplicates);
    for (const auto &item : selected) {
        lines.append(QStringLiteral("\n%1%2").arg(fileName(item), disabled(item) ? QCoreApplication::translate("ModDependencies", "（已禁用）") : QString()));
        const auto &version = item.currentVersion;
        if (version.id.isEmpty() || !version.dependencyMetadataKnown) {
            lines.append(QCoreApplication::translate("ModDependencies", "当前版本或依赖资料未知。可先检查更新，再回来查看；未知不等于没有依赖。"));
            continue;
        }
        lines.append(QCoreApplication::translate("ModDependencies", "当前版本：%1").arg(!version.versionNumber.isEmpty() ? version.versionNumber : version.id));
        if (version.dependencies.isEmpty()) lines.append(QCoreApplication::translate("ModDependencies", "该版本明确声明无依赖。"));
        QHash<QString, QString> versions, names;
        for (const auto &known : inventory) {
            if (disabled(known) || known.source != item.source || known.projectId.isEmpty()) continue;
            versions.insert(known.projectId, projectFiles.value(projectKey(known)).size() > 1 ? QString() : known.currentVersion.id);
            names.insert(known.projectId, fileName(known));
        }
        if (disabled(item)) lines.append(QCoreApplication::translate("ModDependencies", "当前未启用，仅展示缓存声明，不纳入运行依赖/冲突判断。"));
        QSet<QString> declared;
        for (const auto &dep : version.dependencies) {
            lines.append(QCoreApplication::translate("ModDependencies", "声明：") + dependencyLabel(dep, names));
            const auto key = dep.projectId + QLatin1Char('|') + dep.versionId;
            if (declared.contains(key)) lines.append(QCoreApplication::translate("ModDependencies", "需确认：依赖声明重复，不作完整兼容判断。"));
            declared.insert(key);
            if (!disabled(item)) lines.append(dependencyWarnings(dep, versions, names, QCoreApplication::translate("ModDependencies", "当前已识别启用清单")));
        }
    }
    lines.append(QCoreApplication::translate("ModDependencies", "\n仅根据有效缓存和已识别资料判断；未识别项目仍需自行确认。不是完整求解器，不保证可启动，也不会修改文件。"));
    return lines.join(QLatin1Char('\n'));
}

QList<UpdateVerdict> mergeScopedUpdates(const QList<UpdateVerdict> &previous,
                                      const QList<UpdateVerdict> &checked,
                                      const QSet<QString> &scope) {
    QList<UpdateVerdict> merged = checked;
    if (!scope.isEmpty()) {
        for (const auto &entry : previous)
            if (!scope.contains(entry.item.path)) merged.append(entry);
    }
    return merged;
}

QString versionsKeyFor(const QString &consumer, const QString &projectId,
                       const QString &gameVersion, const QString &loader,
                       ResourceCatalog::Source source) {
    // 分隔符 `|`（`versionsRequestKey` 里面用的也是它，那几个字段里都不会有它）。
    return ResourceCatalog::versionsRequestKey(projectId, gameVersion, loader, source) +
           QLatin1Char('|') + consumer;
}

QString projectsKeyFor(const QString &consumer, const QStringList &projectIds,
                       ResourceCatalog::Source source) {
    return ResourceCatalog::projectsRequestKey(projectIds, source) + QLatin1Char('|') + consumer;
}

bool kindUsesLoader(ResourceCatalog::Kind kind) {
    // **只有模组那一类要**（理由全在 .hpp 的 `InstalledItem::loaderApplies` 那段里）。
    // 写成一串 `==` 而不是"排除那几个"：后加一类资源时，默认**不带**比默认带上去安全 ——
    // 带错了的症状是"这一类一条都判不出来"，而不带的症状最坏是"问回来的是另一条版本线"
    // （那一条至少还在报告里，用户看得见）。
    switch (kind) {
    case ResourceCatalog::Kind::Mod:
    case ResourceCatalog::Kind::Modpack:
        return true;
    case ResourceCatalog::Kind::DataPack:
    case ResourceCatalog::Kind::Shader:
    case ResourceCatalog::Kind::ResourcePack:
        return false;
    }
    return false;
}

/// 前置这一趟的消费方名字（**唯一一处** —— 拼钥匙和"那个槽认不认这一发"读的是同一个字）。
static QString dependencyConsumerName() { return QStringLiteral("dependency"); }

/// 「这种前置不用在详情页反复列它」—— 现在只排掉 **Fabric API**。
///
/// 它**只影响展示清单**。安装判定仍走 `requiredDependenciesOf()`：
/// 实例缺 Fabric API 时仍会进入「一起下载」。把它从真实依赖集合里删掉会让 UI
/// 看起来干净，但最后装出一份缺前置的实例。
///
/// NOTE: 同一条理由以后要隐藏别人时加进这张表就行，别污染真实依赖判定。
static bool isObviousPrerequisite(const QString &projectId) {
    static const QSet<QString> kIds = {
        QStringLiteral("P7dR8mSH"),  // Modrinth: fabric-api
        QStringLiteral("306612"),    // CurseForge: Fabric API
    };
    return kIds.contains(projectId);
}

QList<ResourceCatalog::Dependency> requiredDependenciesOf(
    const ResourceCatalog::VersionEntry &version) {
    QList<ResourceCatalog::Dependency> out;
    // 同一个项目**只留一条**（R7）。接口给的清单里作者把同一条 required 写重了是允许的；
    // 两份往下走会变成两发**同钥匙**的版本请求 —— `ResourceStore::fetchVersions()` 按钥匙
    // 去重（第二发把第一发 abort 掉、**而且不回调**），而调用方按条数等回信的话
    // `pending` 永远归不了零，这一趟安装静默不发生。去重放在这里：展示清单和缺失判定
    // 读的是同一份，两处都不会再列出同一个名字。
    QSet<QString> seen;
    for (const ResourceCatalog::Dependency &dependency : version.dependencies) {
        // 这两条都是"不算必装依赖"，但理由不一样：
        // · optional / incompatible / embedded → 用户没要，或压根不该单独下一份；
        // · projectId 空 → 接口给的烂数据（一版里出现过 `{"project_id":null}` 这种），
        // 拿它拼出来的地址会是一个"问了一堆不相干的东西"的请求。
        // "已经装着了"**不在这一层**：那是 `missingRequiredDependencies()` 的事，
        // 因为"装没装"是这个函数不该知道的东西（见 .hpp 文件头那段）。
        if (!dependency.required()) {
            continue;
        }
        if (dependency.projectId.isEmpty()) {
            continue;
        }
        if (seen.contains(dependency.projectId)) {
            continue;
        }
        seen.insert(dependency.projectId);
        out.append(dependency);
    }
    return out;
}

QList<ResourceCatalog::Dependency> displayedRequiredDependencies(
    const ResourceCatalog::VersionEntry &version) {
    QList<ResourceCatalog::Dependency> out;
    for (const ResourceCatalog::Dependency &dependency : requiredDependenciesOf(version)) {
        if (!isObviousPrerequisite(dependency.projectId)) {
            out.append(dependency);
        }
    }
    return out;
}

QList<Lookup> missingRequiredDependencies(const ResourceCatalog::VersionEntry &version,
                                          const QSet<QString> &installedKeys,
                                          const Target &target) {
    QList<Lookup> out;
    // 「什么算必装前置」「同一项目只留一条」那几条判据在 requiredDependenciesOf() 里
    // （**唯一一处**），这里只往下接第三条 —— 已经装着了就不再下一份。
    for (const ResourceCatalog::Dependency &dependency : requiredDependenciesOf(version)) {
        if (installedKeys.contains(target.installedKey(dependency.projectId))) {
            continue;
        }

        Lookup lookup;
        lookup.dependency = dependency;
        // 地址和钥匙都走 ResourceCatalog 那两个唯一出处（两个来源的参数名不一样，
        // 而且 CurseForge 那边加载器要的是**数字**，见 versionsUrl 里那段）。
        lookup.url = ResourceCatalog::versionsUrl(dependency.projectId, target.gameVersion,
                                                 target.loader, target.source, target.apiBase);
        lookup.requestKey = versionsKeyFor(dependencyConsumerName(), dependency.projectId,
                                           target.gameVersion, target.loader, target.source);
        out.append(lookup);
    }
    return out;
}

bool resolveDependency(const ResourceCatalog::Dependency &dependency,
                       const QList<ResourceCatalog::VersionEntry> &versions, Resolved *out,
                       QString *why) {
    const auto fail = [why](const QString &reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };

    if (versions.isEmpty()) {
        // 空的两种（那一发失败 / 这个项目在这个游戏版本+加载器下没有版本）在这一层分不开
        // —— 分得开的那一位在调用方手上（那一发有没有报错）。**这句话两边都说得通**，
        // 所以就把两边都说清楚，别替另一件下结论。
        return fail(QCoreApplication::translate("ModDependencies", "无法获取兼容版本，请检查网络连接及目标版本支持情况。"));
    }

    const ResourceCatalog::VersionEntry *picked = nullptr;
    if (!dependency.versionId.isEmpty()) {
        for (const ResourceCatalog::VersionEntry &version : versions) {
            if (version.id == dependency.versionId) {
                picked = &version;
                break;
            }
        }
        if (picked == nullptr) {
            // **不退而求其次挑最新的一条**。作者钉版本往往是因为新旧不兼容，悄悄换一版
            // 装进去，症状是"这几个模组一起不生效"，而那一刻离这里已经很远了。
            return fail(QCoreApplication::translate("ModDependencies", "指定版本（%1）不在目标实例的兼容版本列表中。")
                            .arg(dependency.versionId));
        }
    } else {
        picked = &versions.first();
    }

    const ResourceCatalog::FileEntry *file = picked->primaryFile();
    if (file == nullptr) {
        return fail(QCoreApplication::translate("ModDependencies", "所选版本（%1）未提供文件。").arg(picked->versionNumber));
    }
    if (file->url.isEmpty()) {
        // 作者关掉了第三方分发时，那边写好的那句话比"没有可下载的文件"准确得多
        // （见 FileEntry::unavailableNote 那段），有就用它。
        return fail(file->unavailableNote.isEmpty()
                        ? QCoreApplication::translate("ModDependencies", "所选版本（%1）未提供可下载文件。")
                              .arg(picked->versionNumber)
                        : file->unavailableNote);
    }

    if (out != nullptr) {
        out->dependency = dependency;
        out->version = *picked;
        out->file = *file;
    }
    return true;
}

Plan buildPlan(const QList<Lookup> &lookups,
               const QHash<QString, QList<ResourceCatalog::VersionEntry>> &answers,
               const QHash<QString, QString> &titlesByProjectId) {
    Plan plan;
    for (const Lookup &lookup : lookups) {
        const QString projectId = lookup.dependency.projectId;
        // 名字取不到就退成项目号（`nvQzSEkH` 这种用户认不出，但比"有一个前置没找到"强）。
        QString label = titlesByProjectId.value(projectId);
        if (label.isEmpty()) {
            label = projectId;
        }

        const auto answersIt = answers.constFind(lookup.requestKey);
        if (answersIt == answers.constEnd()) {
            // 钥匙不在表里 = 那一发没回来（或者回来时没被记下来）。和"它没有版本"分开说。
            plan.unresolved.append(
                QCoreApplication::translate("ModDependencies", "%1：无法获取版本列表。").arg(label));
            continue;
        }

        Resolved resolved;
        QString why;
        if (!resolveDependency(lookup.dependency, answersIt.value(), &resolved, &why)) {
            plan.unresolved.append(QCoreApplication::translate("ModDependencies", "%1：%2").arg(label, why));
            continue;
        }
        resolved.title = label;
        plan.downloads.append(resolved);
    }
    return plan;
}

UpdateVerdict judgeUpdate(const InstalledItem &item,
                          const QList<ResourceCatalog::VersionEntry> &versions,
                          const QString &problem) {
    UpdateVerdict verdict;
    verdict.item = item;
    verdict.state = UpdateState::Unknown;

    if (item.sha1.isEmpty()) {
        // 认不出来是哪个文件（读不动、或者哈希那一步没成）—— 这一条**不是**"已是最新"。
        verdict.reason = QCoreApplication::translate("ModDependencies", "无法识别本地文件版本。");
        return verdict;
    }
    if (versions.isEmpty()) {
        verdict.reason = problem.isEmpty() ? QCoreApplication::translate("ModDependencies", "无法获取版本列表。") : problem;
        return verdict;
    }

    int found = -1;
    for (int i = 0; i < versions.size(); ++i) {
        for (const ResourceCatalog::FileEntry &file : versions.at(i).files) {
            if (!file.sha1.isEmpty() && file.sha1.compare(item.sha1, Qt::CaseInsensitive) == 0) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            break;
        }
    }
    if (found < 0) {
        // 三种情况合成一句（列表筛过、作者删了老版本、用户改过那个 jar），
        // 见头文件里那段 —— 下一步都是"去项目页自己看"。
        verdict.reason = QCoreApplication::translate("ModDependencies", "当前文件版本不在兼容版本列表中。");
        return verdict;
    }

    verdict.latestVersionId = versions.first().id;
    verdict.currentVersion = versions.at(found);
    verdict.latestVersionNumber = versions.first().versionNumber;
    const ResourceCatalog::FileEntry *latestFile = versions.first().primaryFile();
    verdict.latestFileName = latestFile == nullptr ? QString() : latestFile->filename;
    verdict.newerCount = found;
    verdict.state = found == 0 ? UpdateState::UpToDate : UpdateState::Outdated;
    return verdict;
}

UpdateVerdict judgeUpdateAgainstLatest(const InstalledItem &item,
                                       const ResourceCatalog::VersionEntry &latest,
                                       const QString &problem) {
    UpdateVerdict verdict;
    verdict.item = item;
    verdict.state = UpdateState::Unknown;

    if (item.sha1.isEmpty()) {
        // 和上面那条**逐字同一句话**：认不出来是哪一版，就谈不上"是不是最新的"。
        verdict.reason = QCoreApplication::translate("ModDependencies", "无法识别本地文件版本。");
        return verdict;
    }
    if (latest.id.isEmpty()) {
        // 接口对这个哈希**什么都没给**。三种可能分不开（这一层手上没有分得开的东西）：
        // 那一批自己没成（problem）、这个项目在这个游戏版本/加载器下没有版本、
        // 或者它压根不在 Modrinth 上。所以 reason 那句按 problem 说（429 那条尤其 ——
        // 见 .hpp 里那段：真因不许被吞成"网络没成"）。
        verdict.reason =
            problem.isEmpty()
                ? QCoreApplication::translate("ModDependencies", "未找到支持目标游戏版本及加载器的版本。")
                : problem;
        return verdict;
    }

    // 判据和 `judgeUpdate()` 那一条**是同一句话**：盘上这一份的 sha1 就是"最新那一版"的
    // 某个文件的 sha1 ⇒ 已是最新。这里不必（也没法）说"比它新的有几条" —— 接口给的就是
    // 最新的那一条，`newerCount` 于是只在"可更新"这一档给一个 ≥1 的值。
    bool sameFile = false;
    for (const ResourceCatalog::FileEntry &file : latest.files) {
        if (!file.sha1.isEmpty() && file.sha1.compare(item.sha1, Qt::CaseInsensitive) == 0) {
            sameFile = true;
            break;
        }
    }

    verdict.latestVersionId = latest.id;
    verdict.currentVersion = sameFile ? latest : item.currentVersion;
    verdict.latestVersionNumber = latest.versionNumber;
    const ResourceCatalog::FileEntry *latestFile = latest.primaryFile();
    verdict.latestFileName = latestFile == nullptr ? QString() : latestFile->filename;
    verdict.newerCount = sameFile ? 0 : 1;
    verdict.state = sameFile ? UpdateState::UpToDate : UpdateState::Outdated;
    return verdict;
}

QString latestVersionsKeyFor(const QString &consumer, const QString &gameVersion,
                             const QString &loader) {
    // 分隔符 `|`：项目号 / 游戏版本 / 加载器 / 消费方这几个字段里都不会出现它，所以不用转义
    // （和 `versionsRequestKey()` 那边逐字同一个规矩）。**来源不需要**：这一发只有 Modrinth
    // 有（CurseForge 没有对应的批量接口），所以拼不拼上它都是同一把钥匙。
    return QStringLiteral("latest|") + gameVersion + QLatin1Char('|') + loader +
           QLatin1Char('|') + consumer;
}

// ---------- 界面上的字 ----------
namespace {

/// 名单怎么摆 —— **唯一出处**（界面和验证工程都走这儿）。
///
/// 用户 的原话：「（模组名称，一行一个，如果多于 3 个的话写 还有 x 个未列出...
/// （当然如果刚好还剩一个就列出来了不要搞一行显示 1 个未列出这种高血压操作）」
///
/// 翻成两条判据：
/// · **总数 ≤ 4 ⇒ 一条不省，全列**（"刚好还剩一个"的就是 4 条这一档，它走的是全列）；
/// · **总数 > 4 ⇒ 列前 3 条 + 「还有 N 个未列出…」**（N = 总数 − 3）。
/// 所以「还有 1 个未列出」这一行**永远画不出来** —— 那正是用户点名不要的那一句。
QString dependencyNameLines(const QStringList &names) {
    /// 到这一档为止一律全列（含 4 —— 多出来的第 4 条宁可多占一行，也不给用户看
    /// 「还有 1 个未列出」）。
    static constexpr int kListAllUpTo = 4;
    /// 真要截断时留下几条。
    static constexpr int kKeepWhenTruncated = 3;

    QStringList lines;
    if (names.size() <= kListAllUpTo) {
        lines.reserve(names.size());
        for (const QString &name : names) {
            lines.append(QStringLiteral("- ") + name);
        }
        return lines.join(QLatin1Char('\n'));
    }
    lines.reserve(kKeepWhenTruncated + 1);
    for (int i = 0; i < kKeepWhenTruncated; ++i) {
        lines.append(QStringLiteral("- ") + names.at(i));
    }
    // 这一行**不带短横**：它不是名单里的一条，是一句"后面还有"。
    lines.append(QCoreApplication::translate("ModDependencies", "还有 %1 个未列出…").arg(names.size() - kKeepWhenTruncated));
    return lines.join(QLatin1Char('\n'));
}

}  // namespace

QString dependencyPromptHead(ResourceCatalog::Kind kind) {
    return kind == ResourceCatalog::Kind::Shader ? QCoreApplication::translate("ModDependencies", "此光影包缺失必要的前置：")
                                                 : QCoreApplication::translate("ModDependencies", "此模组缺失必要的前置：");
}

QString dependencyPromptTitle() { return QCoreApplication::translate("ModDependencies", "还有前置没装"); }

QString dependencyPromptText(ResourceCatalog::Kind kind, const QStringList &names) {
    if (names.isEmpty()) {
        return QString();
    }
    return QCoreApplication::translate("ModDependencies", "%1\n\n%2\n\n要一起下载吗？")
        .arg(dependencyPromptHead(kind), dependencyNameLines(names));
}

QString dependencyPromptUnknownText(ResourceCatalog::Kind kind, const QStringList &names) {
    if (names.isEmpty()) {
        return QString();
    }
    return QCoreApplication::translate("ModDependencies", "%1\n\n%2\n\n尚未核对实例中已安装的依赖。确认一并下载？")
        .arg(dependencyPromptHead(kind), dependencyNameLines(names));
}

QString dependencyPromptNoteText() {
    // 那一行灰字。**唯一出处**：界面（InlineMessage::ask 的 note）和验证工程都读它。
    // 写法照「提示」栏里那两条（重复启动 / 进度提醒）—— 都是"提醒本身 + 去哪儿关"。
    return QCoreApplication::translate("ModDependencies", "可以在设置中关闭此提示（设置 → 提示 →「检查前置模组」）。");
}

QString dependencyAllButtonText() { return QCoreApplication::translate("ModDependencies", "一起下载"); }

QString dependencyOnlyThisButtonText() { return QCoreApplication::translate("ModDependencies", "只下这个"); }

QString unresolvedNoteText(const QStringList &lines) {
    if (lines.isEmpty()) {
        return QString();
    }
    return QCoreApplication::translate("ModDependencies", "以下依赖缺少兼容版本：\n\n%1\n\n其余文件已下载。可在对应项目页面查找并手动安装依赖。")
        .arg(lines.join(QLatin1Char('\n')));
}

QString dependencyLookupLogText(int count) {
    return QCoreApplication::translate("ModDependencies", "正在查询 %1 个前置模组的可用版本…").arg(count);
}

QString detailPrerequisiteText(const QStringList &names) {
    if (names.isEmpty()) {
        return QString();   // 一条前置都没有 = 这一行整个不画（调用点判空）
    }
    // 顿号连起来。**不截断**：前置一般就一两条，真遇上十条的（那种"全家桶"整合包里的
    // 核心模组）也该照实列全 —— 截断省下的那点宽度换来的是"还有一个是谁"。
    return QCoreApplication::translate("ModDependencies", "这一版要装的前置：%1").arg(names.join(QCoreApplication::translate("ModDependencies", "、")));
}

QString detailPrerequisiteCountText(int count) {
    if (count <= 0) {
        return QString();
    }
    return QCoreApplication::translate("ModDependencies", "这一版要 %1 个前置模组（名字没取到）。").arg(count);
}

QString updateUpToDateText() { return QCoreApplication::translate("ModDependencies", "已是最新"); }

QString updateOutdatedText() { return QCoreApplication::translate("ModDependencies", "有新版本"); }

QString updateUnknownText() { return QCoreApplication::translate("ModDependencies", "无法确认"); }

QString updateSummaryText(int checked, int outdated) {
    return QCoreApplication::translate("ModDependencies", "检查了 %1 个资源，%2 个有新版本。").arg(checked).arg(outdated);
}

}  // namespace ModDependencies

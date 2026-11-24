#include "Support.hpp"

void runModInstallPlanTests() {
    {
        QTemporaryDir inventoryRoot;
        const auto jar = inventoryRoot.filePath(QStringLiteral("mods/renamed.jar"));
        const QByteArray bytes("inventory-content");
        writeFileAt(jar, bytes);
        writeFileAt(inventoryRoot.filePath(QStringLiteral("mods/disabled.jar.disabled")), bytes);
        const QString sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
        writeFileAt(inventoryRoot.filePath(QStringLiteral("wtfml.preset-mod-install.json")),
            QJsonDocument(QJsonObject{{"format", "wtfml-preset-mod-result/1"}, {"plannedFiles", QJsonArray{QJsonObject{
                {"filename", "renamed.jar"}, {"project", "modrinth:dep"}, {"versionId", ""}, {"sha1", sha}}, QJsonObject{
                {"filename", "renamed.jar"}, {"project", "modrinth:dep"}, {"versionId", "exact"}, {"sha1", sha}}}}}).toJson());
        const auto inventory = Presets::scanInstalledMods(inventoryRoot.path(), nullptr);
        check(inventory.problem.isEmpty() && inventory.files.size() == 1
                  && inventory.files[0].projectKey == QStringLiteral("modrinth:dep")
                  && inventory.files[0].versionId == QStringLiteral("exact"),
              "PM 安装记录经实际 SHA-1 复核可确认项目/发行版；禁用文件不满足依赖");
        check(Presets::sameInstalledMods(inventory, inventory), "PM 相同已安装清单可确认稳定");
        writeFileAt(jar, QByteArray("changed-content!!"));
        const auto changed = Presets::scanInstalledMods(inventoryRoot.path(), nullptr);
        check(changed.problem.isEmpty() && changed.files[0].projectKey.isEmpty()
                  && !Presets::sameInstalledMods(inventory, changed),
              "PM 内容变化使历史身份失效，提交前的清单变化可检测");
        std::atomic_bool stop{true};
        check(!Presets::scanInstalledMods(inventoryRoot.path(), &stop).problem.isEmpty(),
              "PM 已安装清单扫描响应取消");
    }
    QTemporaryDir scratch;
    check(scratch.isValid(), "PM 建得出一个临时目录来做落盘校验");
    if (!scratch.isValid()) {
        return;
    }

    const auto makeFile = [](const QString &name, qint64 size = 1024,
                             const QString &sha1 = QString()) {
        ResourceCatalog::FileEntry file;
        file.url = QStringLiteral("https://cdn.example.invalid/") + name;
        file.filename = name;
        file.size = size;
        file.sha1 = sha1;
        file.primary = true;
        return file;
    };
    const auto makeDependency = [](const QString &projectId, const QString &type) {
        ResourceCatalog::Dependency dependency;
        dependency.projectId = projectId;
        dependency.type = type;
        return dependency;
    };
    const auto makeVersion = [&](const QString &id, const QStringList &games,
                                 const QStringList &loaders,
                                 const QList<ResourceCatalog::Dependency> &dependencies = {}) {
        ResourceCatalog::VersionEntry version;
        version.id = id;
        version.versionNumber = id;
        version.versionType = QStringLiteral("release");
        version.gameVersions = games;
        version.loaders = loaders;
        version.files.append(makeFile(id + QStringLiteral(".jar")));
        version.dependencies = dependencies;
        return version;
    };
    const auto makeReference = [](const QString &projectId, const QString &name,
                                  bool required = true) {
        Presets::ModReference reference;
        reference.platform = QStringLiteral("modrinth");
        reference.projectId = projectId;
        reference.name = name;
        reference.required = required;
        return reference;
    };
    const auto makeInput = [&](const Presets::ModReference &reference,
                               const QList<ResourceCatalog::VersionEntry> &versions,
                               bool queried = true, bool failed = false) {
        Presets::ModResolveInput input;
        input.reference = reference;
        input.queried = queried;
        input.queryFailed = failed;
        input.title = reference.name;
        input.versions = versions;
        return input;
    };
    const auto findRow = [](const Presets::ModInstallPlan &plan, const QString &key)
        -> const Presets::ModInstallRow * {
        for (const Presets::ModInstallRow &row : plan.rows) {
            if (row.key == key) {
                return &row;
            }
        }
        return nullptr;
    };

    Presets::ModInstallRequest base;
    base.gameVersion = QStringLiteral("1.20.1");
    base.loader = QStringLiteral("fabric");

    {
        auto root = makeVersion(QStringLiteral("root-v"), {base.gameVersion}, {base.loader},
            {makeDependency(QStringLiteral("EXISTING-DEP"), QStringLiteral("required"))});
        root.dependencyMetadataKnown = true;
        Presets::ModInstallRequest request = base;
        request.references.append(makeInput(makeReference(QStringLiteral("EXISTING"), QStringLiteral("已安装")), {}));
        request.installedKeys.insert(QStringLiteral("modrinth:EXISTING"));
        request.installedVersionIds.insert(QStringLiteral("modrinth:EXISTING"), root.id);
        request.installedVersions.insert(QStringLiteral("modrinth:EXISTING"), root);
        auto plan = Presets::planModInstall(request);
        check(!plan.ok && plan.unknown > 0, "PM 已安装父模组仍需检查自己的必要前置，不直接视为完整");
        auto dep = makeVersion(QStringLiteral("dep-v"), {base.gameVersion}, {base.loader});
        dep.dependencyMetadataKnown = true;
        request.dependencyAnswers[QStringLiteral("modrinth:EXISTING-DEP")] = {dep};
        plan = Presets::planModInstall(request);
        check(plan.ok && plan.alreadyInstalled == 1 && plan.download == 1,
              "PM 已安装父模组保留，只补缺少的前置");
        root.dependencies[0].versionId = QStringLiteral("dep-required");
        request.installedVersions[QStringLiteral("modrinth:EXISTING")] = root;
        request.installedKeys.insert(QStringLiteral("modrinth:EXISTING-DEP"));
        request.installedVersionIds[QStringLiteral("modrinth:EXISTING-DEP")] = QStringLiteral("dep-other");
        request.installedVersions[QStringLiteral("modrinth:EXISTING-DEP")] = dep;
        plan = Presets::planModInstall(request);
        check(!plan.ok && !plan.conflicts.isEmpty(), "PM 已安装父模组的精确前置版本不符仍阻止，不静默替换");
    }

    // ---------- PM1 必要 / 可选：一条失败不拖垮另一条的账 ----------
    {
        Presets::ModInstallRequest request = base;
        request.references.append(
            makeInput(makeReference(QStringLiteral("P-OLD"), QStringLiteral("旧模组")),
                      {makeVersion(QStringLiteral("old-1"), {QStringLiteral("1.16.5")},
                                   {QStringLiteral("fabric")})}));
        Presets::ModReference optional = makeReference(QStringLiteral("P-OPT"),
                                                       QStringLiteral("可选模组"), false);
        request.references.append(makeInput(optional, {}, false, true));

        const Presets::ModInstallPlan plan = Presets::planModInstall(request);
        check(plan.rows.size() == 2, "PM1 两条引用各出一行",
              QStringLiteral("rows=%1").arg(plan.rows.size()));
        check(!plan.rows.isEmpty() &&
                  plan.rows.first().status == Presets::ModInstallStatus::Missing,
              "★ PM1 目标版本对不上 ⇒ 缺失（不是「暂无法确认」）");
        check(plan.rows.size() > 1 &&
                  plan.rows.at(1).status == Presets::ModInstallStatus::Unknown,
              "★ PM1 问平台失败 ⇒ 暂无法确认（不是「缺失」）");
        check(!plan.ok && plan.requiredFailures() == 1 && plan.optionalFailures() == 1,
              "★★ PM1 必要 1 / 可选 1 分开数，整份不可继续",
              QStringLiteral("ok=%1 req=%2 opt=%3")
                  .arg(plan.ok ? 1 : 0)
                  .arg(plan.requiredFailures())
                  .arg(plan.optionalFailures()));
        check(plan.requiredFailures() + plan.optionalFailures() ==
                  plan.missing + plan.unknown + plan.incompatible + plan.needsConfirm +
                      plan.blocked,
              "PM1 失败总数对得上各类计数");
    }

    // ---------- PM2 去重：同一条引用出现两次只算一次 ----------
    {
        Presets::ModInstallRequest request = base;
        const Presets::ModReference reference =
            makeReference(QStringLiteral("P-ONE"), QStringLiteral("一次"));
        const Presets::ModResolveInput input =
            makeInput(reference, {makeVersion(QStringLiteral("one-1"),
                                              {QStringLiteral("1.20.1")},
                                              {QStringLiteral("fabric")})});
        request.references.append(input);
        request.references.append(input);
        const Presets::ModInstallPlan plan = Presets::planModInstall(request);
        check(plan.rows.size() == 1 && plan.download == 1,
              "★★ PM2 同一项目出现两次 ⇒ 只下一份",
              QStringLiteral("rows=%1 download=%2").arg(plan.rows.size()).arg(plan.download));
    }

    for (const QString platformLoader : {QStringLiteral("fabric"), QStringLiteral("quilt")}) {
        auto request = base;
        request.loader = platformLoader + QStringLiteral("-loader");
        auto reference = makeReference(QStringLiteral("P-ALIAS"), QStringLiteral("加载器别名"));
        reference.loader = request.loader;
        request.references.append(makeInput(reference, {makeVersion(QStringLiteral("alias-1"),
            {QStringLiteral("1.20.1")}, {platformLoader})}));
        check(Presets::planModInstall(request).ok, "PM 实例加载器别名匹配平台发行版", platformLoader);
        request.loader = QStringLiteral("forge");
        reference.loader.clear();
        request.references.clear();
        request.references.append(makeInput(reference, {makeVersion(QStringLiteral("wrong-loader"),
            {QStringLiteral("1.20.1")}, {platformLoader})}));
        check(!Presets::planModInstall(request).ok, "PM 不把真正不同的加载器当别名", platformLoader);
    }

    // ---------- PM3 依赖展开：required 折进来、optional 只列、embedded 无视 ----------
    {
        Presets::ModInstallRequest request = base;
        request.dependencyTitles.insert(QStringLiteral("modrinth:P-DEP"),
                                        QStringLiteral("前置甲"));
        request.dependencyAnswers.insert(
            QStringLiteral("modrinth:P-DEP"),
            {makeVersion(QStringLiteral("dep-1"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")})});
        request.references.append(makeInput(
            makeReference(QStringLiteral("P-PARENT"), QStringLiteral("父模组")),
            {makeVersion(QStringLiteral("parent-1"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")},
                         {makeDependency(QStringLiteral("P-DEP"), QStringLiteral("required")),
                          makeDependency(QStringLiteral("P-OPTIONAL"), QStringLiteral("optional")),
                          makeDependency(QStringLiteral("P-EMBED"), QStringLiteral("embedded")),
                          makeDependency(QStringLiteral("P-BAD"), QStringLiteral("incompatible"))})}));

        const Presets::ModInstallPlan plan = Presets::planModInstall(request);
        check(plan.rows.size() == 2 && plan.download == 2,
              "★★ PM3 必要前置折进同一张下载清单",
              QStringLiteral("rows=%1 download=%2").arg(plan.rows.size()).arg(plan.download));
        const Presets::ModInstallRow *dependency = findRow(plan, QStringLiteral("modrinth:P-DEP"));
        check(dependency != nullptr &&
                  dependency->status == Presets::ModInstallStatus::Download &&
                  dependency->fromDependency && dependency->required,
              "★★ PM3 前置行标着「从依赖来」、且跟着父项的「必要」走");
        check(dependency != nullptr && dependency->title == QStringLiteral("前置甲"),
              "PM3 前置的名字用答复里给的那一份",
              dependency != nullptr ? dependency->title : QStringLiteral("<缺>"));
        check(plan.optionalDependencyKeys ==
                  QStringList{QStringLiteral("modrinth:P-OPTIONAL")},
              "★★ PM3 optional 前置只列出来，**不自动下**",
              plan.optionalDependencyKeys.join(QStringLiteral(",")));
        check(findRow(plan, QStringLiteral("modrinth:P-EMBED")) == nullptr,
              "★★ PM3 embedded 前置不单独下一份（已经打进 jar）");
        check(findRow(plan, QStringLiteral("modrinth:P-BAD")) == nullptr,
              "PM3 incompatible 不当下拉清单里的一条（它只管冲突）");
        check(plan.ok, "PM3 全都能下 ⇒ 计划可以继续");
    }

    // ---------- PM4 必要前置没答复 ⇒ 暂无法确认（不许默认"没有前置"） ----------
    {
        Presets::ModInstallRequest request = base;
        request.references.append(makeInput(
            makeReference(QStringLiteral("P-PARENT4"), QStringLiteral("父模组")),
            {makeVersion(QStringLiteral("parent-4"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")},
                         {makeDependency(QStringLiteral("P-NOANSWER"), QStringLiteral("required"))})}));
        const Presets::ModInstallPlan plan = Presets::planModInstall(request);
        const Presets::ModInstallRow *dependency =
            findRow(plan, QStringLiteral("modrinth:P-NOANSWER"));
        check(dependency != nullptr &&
                  dependency->status == Presets::ModInstallStatus::Unknown &&
                  dependency->fromDependency,
              "★★★ PM4 必要前置还没问到 ⇒ 那一行「暂无法确认」（不许当成「没有」）",
              dependency != nullptr ? dependency->problem : QStringLiteral("<没有这一行>"));
        check(!plan.ok, "★★ PM4 必要前置说不清 ⇒ 整份计划不可继续");
    }

    // ---------- PM5 实例里已经有了 ----------
    {
        Presets::ModInstallRequest request = base;
        request.installedKeys.insert(QStringLiteral("modrinth:P-HAVE"));
        request.references.append(makeInput(
            makeReference(QStringLiteral("P-HAVE"), QStringLiteral("已装")),
            {makeVersion(QStringLiteral("have-1"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")})}));
        const Presets::ModInstallPlan plan = Presets::planModInstall(request);
        check(plan.rows.size() == 1 &&
                  plan.rows.first().status == Presets::ModInstallStatus::AlreadyInstalled &&
                  plan.download == 0 && plan.ok,
              "★★ PM5 已经装着 ⇒ 不重下，也不算失败",
              QStringLiteral("download=%1").arg(plan.download));
    }

    // ---------- PM6 冲突：这一趟里的两项互相不兼容 ----------
    {
        Presets::ModInstallRequest request = base;
        request.references.append(makeInput(
            makeReference(QStringLiteral("P-A"), QStringLiteral("甲")),
            {makeVersion(QStringLiteral("a-1"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")},
                         {makeDependency(QStringLiteral("P-B"), QStringLiteral("incompatible"))})}));
        request.references.append(makeInput(
            makeReference(QStringLiteral("P-B"), QStringLiteral("乙")),
            {makeVersion(QStringLiteral("b-1"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")})}));
        const Presets::ModInstallPlan plan = Presets::planModInstall(request);
        const Presets::ModInstallRow *a = findRow(plan, QStringLiteral("modrinth:P-A"));
        const Presets::ModInstallRow *b = findRow(plan, QStringLiteral("modrinth:P-B"));
        check(a != nullptr && a->status == Presets::ModInstallStatus::Incompatible,
              "★★★ PM6 声明不兼容 ⇒ 这一项判「不兼容」，不下",
              a != nullptr ? a->problem : QStringLiteral("<没有这一行>"));
        check(b != nullptr && b->status == Presets::ModInstallStatus::Download,
              "PM6 被声明的那一项自己没毛病 ⇒ 照下（谁声明谁让路）");
        check(plan.conflicts.size() == 1 && !plan.ok && plan.incompatible == 1,
              "★★ PM6 冲突点名一句话，整份不可继续",
              plan.conflicts.join(QStringLiteral(" / ")));
        check(!plan.conflicts.isEmpty() &&
                  plan.conflicts.first().contains(QStringLiteral("甲")) &&
                  plan.conflicts.first().contains(QStringLiteral("乙")),
              "PM6 冲突那句话两头都点名");
    }

    // ---------- PM7 冲突：和实例里已经装着的撞上 ----------
    {
        Presets::ModInstallRequest request = base;
        request.installedKeys.insert(QStringLiteral("modrinth:P-OLDENEMY"));
        request.references.append(makeInput(
            makeReference(QStringLiteral("P-NEW"), QStringLiteral("新来的")),
            {makeVersion(QStringLiteral("new-1"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")},
                         {makeDependency(QStringLiteral("P-OLDENEMY"),
                                         QStringLiteral("incompatible"))})}));
        const Presets::ModInstallPlan plan = Presets::planModInstall(request);
        check(plan.rows.size() == 1 &&
                  plan.rows.first().status == Presets::ModInstallStatus::Incompatible &&
                  !plan.ok,
              "★★★ PM7 和实例里已有的冲突 ⇒ 同样不下（「不错装不兼容文件」）");
    }

    // ---------- PM8 本地兼容筛：接口筛过不算数 ----------
    {
        ResourceCatalog::VersionEntry wrongGame =
            makeVersion(QStringLiteral("v-wrong-game"), {QStringLiteral("1.20.4")},
                        {QStringLiteral("fabric")});
        ResourceCatalog::VersionEntry noUrl =
            makeVersion(QStringLiteral("v-no-url"), {QStringLiteral("1.20.1")},
                        {QStringLiteral("fabric")});
        noUrl.files[0].url.clear();
        ResourceCatalog::VersionEntry noMetadata =
            makeVersion(QStringLiteral("v-unknown"), {}, {});

        Presets::ModInstallRequest request = base;
        request.references.append(makeInput(
            makeReference(QStringLiteral("P-FILTER"), QStringLiteral("筛选")),
            {wrongGame, noUrl, noMetadata}));
        const Presets::ModInstallPlan plan = Presets::planModInstall(request);
        check(plan.download == 1 && !plan.rows.isEmpty() &&
                  plan.rows.first().version.id == QStringLiteral("v-unknown"),
              "★★★ PM8 跳过版本对不上的、跳过没有下载地址的，挑到「元数据空着」那一版",
              plan.rows.isEmpty() ? QString() : plan.rows.first().version.id);

        Presets::ModInstallRequest onlyBad = base;
        ResourceCatalog::VersionEntry onlyNoUrl =
            makeVersion(QStringLiteral("v-only-no-url"), {QStringLiteral("1.20.1")},
                        {QStringLiteral("fabric")});
        onlyNoUrl.files[0].url.clear();
        onlyBad.references.append(makeInput(
            makeReference(QStringLiteral("P-NOURL"), QStringLiteral("没地址")), {onlyNoUrl}));
        const Presets::ModInstallPlan noUrlPlan = Presets::planModInstall(onlyBad);
        check(noUrlPlan.missing == 1 && !noUrlPlan.rows.isEmpty() &&
                  noUrlPlan.rows.first().problem.contains(QStringLiteral("下载地址")),
              "★★ PM8 只有「没有下载地址」的版本 ⇒ 缺失并说清为什么",
              noUrlPlan.rows.isEmpty() ? QString() : noUrlPlan.rows.first().problem);
    }

    // ---------- PM9 缺失与说不清的理由分开写 ----------
    {
        Presets::ModInstallRequest failed = base;
        Presets::ModResolveInput failedInput =
            makeInput(makeReference(QStringLiteral("P-FAIL"), QStringLiteral("问挂了")), {},
                      true, true);
        failedInput.queryProblem = QStringLiteral("连接超时");
        failed.references.append(failedInput);
        const Presets::ModInstallPlan failedPlan = Presets::planModInstall(failed);
        check(failedPlan.unknown == 1 && !failedPlan.rows.isEmpty() &&
                  failedPlan.rows.first().status == Presets::ModInstallStatus::Unknown &&
                  failedPlan.rows.first().problem == QStringLiteral("连接超时"),
              "★ PM9 问平台失败的原话照抄到行上（不是「缺失」）",
              failedPlan.rows.isEmpty() ? QString() : failedPlan.rows.first().problem);

        Presets::ModInstallRequest notQueried = base;
        notQueried.references.append(makeInput(
            makeReference(QStringLiteral("P-WAIT"), QStringLiteral("还没问")), {}, false));
        const Presets::ModInstallPlan waitPlan = Presets::planModInstall(notQueried);
        check(waitPlan.unknown == 1 && !waitPlan.rows.isEmpty() &&
                  waitPlan.rows.first().problem.contains(QStringLiteral("等待查询平台")),
              "★ PM9 没问过 ⇒ 暂无法确认（「还没轮到」，不是「没有」）",
              waitPlan.rows.isEmpty() ? QString() : waitPlan.rows.first().problem);
    }

    // ---------- PM10 替代项目：不确认就不换 ----------
    {
        const Presets::ModReference original =
            makeReference(QStringLiteral("P-GONE"), QStringLiteral("停更的"));
        const Presets::ModReference alternative =
            makeReference(QStringLiteral("P-ALT"), QStringLiteral("替代的"));

        Presets::ModInstallRequest pending = base;
        pending.references.append(makeInput(original, {}));
        pending.alternativesByKey.insert(original.key(), {alternative});
        const Presets::ModInstallPlan pendingPlan = Presets::planModInstall(pending);
        check(pendingPlan.rows.size() == 1 &&
                  pendingPlan.rows.first().status ==
                      Presets::ModInstallStatus::AlternativeNeedsConfirm &&
                  pendingPlan.needsConfirm == 1 && pendingPlan.download == 0,
              "★★★ PM10 有候选替代但没确认 ⇒ 只报「等你确认」，绝不自作主张",
              pendingPlan.rows.isEmpty() ? QString() : pendingPlan.rows.first().problem);
        check(pendingPlan.rows.size() == 1 &&
                  !pendingPlan.rows.first().details.isEmpty() &&
                  pendingPlan.rows.first().details.first().contains(QStringLiteral("替代的")),
              "PM10 候选替代的名字列给用户看");
        check(!pendingPlan.ok, "PM10 等确认也算必要项失败（不能悄悄跳过）");

        // 确认了，但没有给替代项的答复 ⇒ 也不能凭空造一份下载。
        Presets::ModInstallRequest confirmedNoAnswer = base;
        confirmedNoAnswer.references.append(makeInput(original, {}));
        confirmedNoAnswer.alternativesByKey.insert(original.key(), {alternative});
        confirmedNoAnswer.confirmedAlternatives.insert(alternative.key());
        const Presets::ModInstallPlan noAnswerPlan =
            Presets::planModInstall(confirmedNoAnswer);
        check(noAnswerPlan.rows.size() == 1 &&
                  noAnswerPlan.rows.first().key == alternative.key() &&
                  noAnswerPlan.rows.first().status == Presets::ModInstallStatus::Unknown &&
                  noAnswerPlan.download == 0,
              "★★ PM10 确认过替代、但替代自己没答复 ⇒ 还是不造下载",
              noAnswerPlan.rows.isEmpty() ? QString() : noAnswerPlan.rows.first().problem);

        // 确认 + 替代项自己有兼容答复 ⇒ 换成它，并且它自己也过兼容关。
        Presets::ModInstallRequest substituted = base;
        substituted.references.append(makeInput(original, {}));
        substituted.references.append(makeInput(
            alternative, {makeVersion(QStringLiteral("alt-1"), {QStringLiteral("1.20.1")},
                                      {QStringLiteral("fabric")})}));
        substituted.alternativesByKey.insert(original.key(), {alternative});
        substituted.confirmedAlternatives.insert(alternative.key());
        const Presets::ModInstallPlan substitutedPlan = Presets::planModInstall(substituted);
        check(substitutedPlan.rows.size() == 1 && substitutedPlan.download == 1,
              "★★ PM10 确认过 ⇒ 换成替代项，原条目不再单占一行",
              QStringLiteral("rows=%1 download=%2")
                  .arg(substitutedPlan.rows.size())
                  .arg(substitutedPlan.download));
        check(substitutedPlan.rows.size() == 1 &&
                  substitutedPlan.rows.first().key == alternative.key() &&
                  substitutedPlan.rows.first().substituted &&
                  substitutedPlan.rows.first().substitutedKey == alternative.key() &&
                  substitutedPlan.substituted == 1,
              "★★ PM10 那一行标着「用的是替代项目」，界面才说得清换了什么");
        check(substitutedPlan.ok, "PM10 替代项自己兼容 ⇒ 计划可以继续");
    }

    // ---------- PM11 前置链太深 / 引用坏掉 ----------
    {
        Presets::ModInstallRequest deep = base;
        deep.maxDependencyDepth = 1;
        deep.dependencyAnswers.insert(
            QStringLiteral("modrinth:P-D1"),
            {makeVersion(QStringLiteral("d1"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")},
                         {makeDependency(QStringLiteral("P-D2"), QStringLiteral("required"))})});
        deep.references.append(makeInput(
            makeReference(QStringLiteral("P-ROOT"), QStringLiteral("根")),
            {makeVersion(QStringLiteral("root"), {QStringLiteral("1.20.1")},
                         {QStringLiteral("fabric")},
                         {makeDependency(QStringLiteral("P-D1"), QStringLiteral("required"))})}));
        const Presets::ModInstallPlan deepPlan = Presets::planModInstall(deep);
        const Presets::ModInstallRow *cut = findRow(deepPlan, QStringLiteral("modrinth:P-D2"));
        check(cut != nullptr && cut->status == Presets::ModInstallStatus::Unknown &&
                  cut->problem.contains(QStringLiteral("超过")) && !deepPlan.ok,
              "★★ PM11 前置链超过上限 ⇒ 由一行「暂无法确认」点名，不静默截断",
              cut != nullptr ? cut->problem : QStringLiteral("<没有那一行>"));

        Presets::ModInstallRequest broken = base;
        Presets::ModReference bad;
        bad.projectId = QStringLiteral("P-NOPLATFORM");
        broken.references.append(makeInput(bad, {}));
        const Presets::ModInstallPlan brokenPlan = Presets::planModInstall(broken);
        check(brokenPlan.blocked == 1 && !brokenPlan.rows.isEmpty() &&
                  brokenPlan.rows.first().status == Presets::ModInstallStatus::Blocked &&
                  !brokenPlan.ok,
              "★ PM11 平台认不出来的引用 ⇒「做不了」并算必要失败");
    }

    // Exact platform version IDs must not be confused with project identity.
    {
        auto dependency = makeDependency(QStringLiteral("PIN-DEP"), QStringLiteral("required"));
        dependency.versionId = QStringLiteral("release-a");
        const auto parent = [&](const QString &id, const ResourceCatalog::Dependency &dep) {
            return makeInput(makeReference(id, id),
                {makeVersion(id, {QStringLiteral("1.20.1")}, {QStringLiteral("fabric")}, {dep})});
        };
        Presets::ModInstallRequest request = base;
        request.references.append(parent(QStringLiteral("PIN-A"), dependency));
        request.installedKeys.insert(QStringLiteral("modrinth:PIN-DEP"));
        auto plan = Presets::planModInstall(request);
        check(!plan.ok && plan.unknown == 1, "PM-PIN 已安装项目身份不能证明精确发行版满足");
        request.installedVersionIds.insert(QStringLiteral("modrinth:PIN-DEP"), QStringLiteral("release-a"));
        plan = Presets::planModInstall(request);
        check(plan.ok && plan.download == 1, "PM-PIN 精确发行版已安装时允许父项安装");
        request.installedVersionIds[QStringLiteral("modrinth:PIN-DEP")] = QStringLiteral("release-b");
        plan = Presets::planModInstall(request);
        check(!plan.ok && plan.incompatible == 1 && !plan.conflicts.isEmpty(),
              "PM-PIN 已装不同发行版需确认替换而非静默满足");
        request.installedVersionIds[QStringLiteral("modrinth:PIN-DEP")] = QStringLiteral("release-a");
        auto other = dependency;
        other.versionId = QStringLiteral("release-b");
        request.references.append(parent(QStringLiteral("PIN-B"), other));
        plan = Presets::planModInstall(request);
        check(!plan.ok && plan.incompatible == 2 && plan.conflicts.size() == 1,
              "PM-PIN 两个父项要求不同精确前置时汇总冲突");
        std::reverse(request.references.begin(), request.references.end());
        const auto reversed = Presets::planModInstall(request);
        check(reversed.conflicts == plan.conflicts && reversed.incompatible == plan.incompatible,
              "PM-PIN 精确约束冲突不依赖引用顺序");
        request.references.clear();
        dependency.projectId.clear();
        request.references.append(parent(QStringLiteral("PIN-ONLY-VERSION"), dependency));
        plan = Presets::planModInstall(request);
        check(!plan.ok && plan.unknown == 1 && plan.download == 0,
              "PM-PIN 只有发行版 ID 的必要前置不得静默忽略");
    }

    {
        Presets::ModInstallRequest request = base;
        auto pin = makeDependency("SHARED", "required"); pin.versionId = "old-compatible";
        request.references.append(makeInput(makeReference("PARENT-A", "A"),
            {makeVersion("parent-a", {"1.20.1"}, {"fabric"}, {pin})}));
        request.references.append(makeInput(makeReference("PARENT-B", "B"),
            {makeVersion("parent-b", {"1.20.1"}, {"fabric"}, {pin})}));
        request.dependencyAnswers["modrinth:SHARED"] = {
            makeVersion("newest", {"1.20.1"}, {"fabric"}),
            makeVersion("old-compatible", {"1.20.1"}, {"fabric"})};
        const auto plan = Presets::planModInstall(request);
        QString selected;
        for (const auto &row : plan.rows) if (row.key == "modrinth:SHARED") selected = row.version.id;
        check(plan.ok && plan.download == 3 && selected == "old-compatible",
              "shared planner merges two parent pins and chooses the exact compatible release rather than latest");
        request.references.clear(); request.dependencyAnswers.clear();
        request.references.append(makeInput(makeReference("CYCLE-A", "A"),
            {makeVersion("a", {"1.20.1"}, {"fabric"}, {makeDependency("CYCLE-B", "required")})}));
        request.dependencyAnswers["modrinth:CYCLE-B"] = {
            makeVersion("b", {"1.20.1"}, {"fabric"}, {makeDependency("CYCLE-A", "required")})};
        const auto cycle = Presets::planModInstall(request);
        check(cycle.ok && cycle.download == 2 && cycle.rows.size() == 2,
              "valid dependency cycle is finite and installs each project once");
        request.references.clear(); request.dependencyAnswers.clear();
        request.references.append(makeInput(makeReference("OPTIONAL-PARENT", "Optional", false),
            {makeVersion("optional", {"1.20.1"}, {"fabric"}, {makeDependency("ABSENT", "required")})}));
        request.references.append(makeInput(makeReference("INDEPENDENT", "Independent", false),
            {makeVersion("independent", {"1.20.1"}, {"fabric"})}));
        const auto optional = Presets::planModInstall(request);
        check(optional.ok && optional.download == 1 && optional.optionalFailures() == 2,
              "failed necessary child blocks its optional parent while preserving an unrelated optional component");
    }

    // ---------- PM12 落盘校验 ----------
    {
        const QByteArray payload = QByteArrayLiteral("wtfml-verify-download");
        const QString path = scratch.filePath(QStringLiteral("mod.jar"));
        {
            QFile file(path);
            check(file.open(QIODevice::WriteOnly), "PM12 写得出临时文件");
            file.write(payload);
        }
        const QString sha1 = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex());

        const Presets::DownloadVerification good =
            Presets::verifyDownloadedFile(path, payload.size(), sha1);
        check(good.ok && !good.unverified && good.size == payload.size() &&
                  good.sha1.compare(sha1, Qt::CaseInsensitive) == 0,
              "★★ PM12 大小 + sha1 都对 ⇒ 通过",
              good.problem);

        const Presets::DownloadVerification wrongSize =
            Presets::verifyDownloadedFile(path, payload.size() + 1, sha1);
        check(!wrongSize.ok && wrongSize.problem.contains(QStringLiteral("大小")),
              "★★ PM12 大小对不上 ⇒ 不过，并说清差多少",
              wrongSize.problem);

        const Presets::DownloadVerification wrongHash =
            Presets::verifyDownloadedFile(
                path, payload.size(),
                QStringLiteral("0000000000000000000000000000000000000000"));
        check(!wrongHash.ok && wrongHash.problem.contains(QStringLiteral("SHA-1 校验失败")),
              "★★★ PM12 sha1 对不上 ⇒ 不过（这一条就是「不错装坏文件」）",
              wrongHash.problem);

        const Presets::DownloadVerification noHash =
            Presets::verifyDownloadedFile(path, -1, QString());
        check(noHash.ok && noHash.unverified &&
                  noHash.problem.contains(QStringLiteral("未提供此文件的 SHA-1")),
              "★★ PM12 接口没给 sha1 ⇒ 只能说「没校验」，不许说「校验通过」",
              noHash.problem);

        const Presets::DownloadVerification missing = Presets::verifyDownloadedFile(
            scratch.filePath(QStringLiteral("不存在.jar")), 10, sha1);
        check(!missing.ok && !missing.unverified,
              "PM12 文件都不在 ⇒ 不过（不是「没校验」）", missing.problem);
    }

    // ---------- PM13 界面上的字 ----------
    {
        check(Presets::modInstallStatusText(Presets::ModInstallStatus::Download) !=
                  Presets::modInstallStatusText(Presets::ModInstallStatus::Missing),
              "PM13 每一档状态有自己的字");

        Presets::ModInstallPlan empty;
        check(Presets::modInstallSummaryText(empty) == QStringLiteral("无待处理内容。"),
              "PM13 空计划一句人话", Presets::modInstallSummaryText(empty));

        Presets::ModInstallPlan summary;
        summary.download = 3;
        summary.missing = 1;
        summary.needsConfirm = 1;
        const QString text = Presets::modInstallSummaryText(summary);
        check(text.contains(QStringLiteral("下载 3 个")) &&
                  text.contains(QStringLiteral("缺失 1 个")) &&
                  text.contains(QStringLiteral("有 1 个替代项目待确认")),
              "★ PM13 总账把三类都数出来", text);

        check(Presets::modInstallPartialText().contains(QStringLiteral("部分应用")),
              "PM13 部分应用那一句不撒谎");
        check(Presets::modInstallAlternativeText({QStringLiteral("甲"), QStringLiteral("乙")})
                  .contains(QStringLiteral("甲、乙")),
              "PM13 替代候选列表点名到个");
        check(Presets::modInstallSubstitutedText(QStringLiteral("甲"))
                  .contains(QStringLiteral("甲")),
              "PM13 换成了谁说得出来");
    }
}


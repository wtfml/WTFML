#include "Support.hpp"

namespace {
/// 数 `rawVersionJson()` 那句"文件不在"的告警（QQ7 那一条用）。
/// NOTE: 处理器必须是**自由函数**（QtMessageHandler 就是个函数指针，带捕获的 lambda
/// 换不过去），所以计数放在文件作用域。
int g_missingJsonWarnings = 0;
void countMissingJsonWarnings(QtMsgType, const QMessageLogContext &, const QString &message) {
    if (message.contains(QLatin1String("Version JSON not found on disk"))) {
        ++g_missingJsonWarnings;
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// P. 实例特征（加载器 + 版本类型）：一次问清楚，缓存跟着 json 缓存走
//
// 每轮刷新实例列表，界面上有三处都在问同一个实例的两件事：「是哪个加载器」（实例行
// 前面那个标志、实例设置里「模组」那一页开不开、导出时的依赖表）和「是什么版本类型」
// （频道、默认分组）。这两件事的出处是同一份 wtfml.meta + 同一份合并过的版本 json，
// 所以问一次能答两问（VersionManager::instanceTraits）。这一节钉的是它的三条契约：
//
// ① meta 里写了加载器就以 meta 为准，没写才去 json 的 `libraries` 里认；
// ② 认出来的结果记在 mergedVersionJsons 那一格里 —— clearVersionJsonCache() 必须让
//    它重新算（换 .minecraft、装 / 删 / 复制实例那六处都靠这一句）；
// ③ meta 那半每次都要看一眼盘：外部直接改了 wtfml.meta，下一轮刷新就得看得见。
//
// 为什么要单列一节：缓存接错时的症状是"装完加载器界面还当它是原版"，而那种错
// 没有任何一条现有断言会红。
// ---------------------------------------------------------------------------
int runInstanceTraitsTests(const QString &sandbox) {
    section("QQ. 实例特征（加载器 + 版本类型）一次问清楚");

    const QString versionsRoot = sandbox + "/versions";
    const auto writeJson = [&versionsRoot](const QString &id, const QByteArray &json) {
        writeFileAt(versionsRoot + "/" + id + "/" + id + ".json", json);
    };

    // 自己就是完整 json —— PCL / HMCL 合并出来的实例长这样（没有 inheritsFrom，
    // fabric-loader 那一行就在自己的 `libraries` 里）。
    const QString fabricId = QStringLiteral("QQ-Fabric");
    writeJson(fabricId, R"({"id":"QQ-Fabric","type":"release",
        "mainClass":"net.fabricmc.loader.impl.launch.knot.KnotClient",
        "libraries":[{"name":"net.fabricmc:fabric-loader:0.16.14"}]})");

    // 薄 json：加载器那一行在父版本里（本启动器自己装出来的实例长这样）。
    const QString parentId = QStringLiteral("QQ-父版本");
    writeJson(parentId, R"({"id":"QQ-父版本","type":"release",
        "libraries":[{"name":"net.fabricmc:fabric-loader:0.16.14"}]})");
    const QString thinId = QStringLiteral("QQ-薄json");
    writeJson(thinId, QByteArray("{\"id\":\"QQ-薄json\",\"inheritsFrom\":\"")
                          + parentId.toUtf8() + "\"}");

    const QString vanillaId = QStringLiteral("QQ-原版");
    writeJson(vanillaId, R"({"id":"QQ-原版","type":"snapshot","libraries":[]})");

    VersionManager vm;

    // ---- ① 从 json 的 libraries 认得出 ----
    {
        const VersionManager::InstanceTraits traits = vm.instanceTraits(fabricId);
        check(traits.loaderName == QStringLiteral("Fabric"),
              "QQ1 从 json 的 libraries 认出 Fabric", traits.loaderName);
        check(traits.loaderVersion == QStringLiteral("0.16.14"),
              "QQ1 加载器版本从 maven 坐标里切出来", traits.loaderVersion);
        check(traits.type == QStringLiteral("release"),
              "QQ1 版本类型从同一份 json 一起答出来", traits.type);

        QString name;
        QString version;
        check(vm.instanceModLoader(fabricId, &name, &version) && name == traits.loaderName
                  && version == traits.loaderVersion,
              "QQ1 instanceModLoader() 和 instanceTraits() 给同一份答案",
              name + "/" + version);
    }

    // ---- ① 反向：没写加载器的不许被"猜"出一个来 ----
    {
        const VersionManager::InstanceTraits traits = vm.instanceTraits(vanillaId);
        check(traits.loaderName.isEmpty(), "QQ2 纯原版 json 的加载器名是空的（不是猜的）",
              traits.loaderName);
        check(traits.loaderVersion.isEmpty(), "QQ2 加载器名为空时版本号也必须是空的",
              traits.loaderVersion);
        check(traits.type == QStringLiteral("snapshot"), "QQ2 快照照原样说快照", traits.type);

        QString name = QStringLiteral("上一次留下的脏值");
        check(!vm.instanceModLoader(vanillaId, &name), "QQ2 instanceModLoader() 回 false");
        check(name.isEmpty(), "QQ2 回 false 时出参是空的（不留旧值）", name);
    }

    // ---- ① 薄 json：加载器在父那层，合并之后照样问得出来 ----
    {
        const VersionManager::InstanceTraits traits = vm.instanceTraits(thinId);
        check(traits.loaderName == QStringLiteral("Fabric")
                  && traits.loaderVersion == QStringLiteral("0.16.14"),
              "QQ2 薄 json 的加载器从父版本继承上来",
              traits.loaderName + "/" + traits.loaderVersion);
        check(traits.type == QStringLiteral("release"),
              "QQ2 薄 json 的类型也从父版本继承上来", traits.type);
    }

    // ---- 问不出来时给三个空答案，不崩、不报错 ----
    {
        const VersionManager::InstanceTraits blank = vm.instanceTraits(QString());
        check(blank.loaderName.isEmpty() && blank.loaderVersion.isEmpty() && blank.type.isEmpty(),
              "QQ2 空 id 什么都不问，给三个空答案");
        const VersionManager::InstanceTraits missing = vm.instanceTraits(QStringLiteral("QQ-根本没这个"));
        check(missing.loaderName.isEmpty() && missing.loaderVersion.isEmpty()
                  && missing.type.isEmpty(),
              "QQ2 不存在的实例给三个空答案（走的是\"文件不在就别去读\"那条闸，不落告警）");
    }

    // ---- ③ meta 那半：每次都要看见盘上的变化 ----
    const QString metaId = QStringLiteral("QQ-带meta");
    writeJson(metaId, QByteArray("{\"id\":\"") + metaId.toUtf8()
                          + "\",\"type\":\"snapshot\"}");
    const QString metaPath = AppPaths::versionMetaPath(metaId);
    check(vm.instanceTraits(metaId).loaderName.isEmpty(), "QQ3 还没写 meta 时按 json 说话");

    // 直接落盘、**不走** updateVersionMeta：装加载器那条路就是另一处写盘的地方，
    // 它写完之后这个会话里下一轮刷新必须看得见。
    writeFileAt(metaPath, R"({"loaderName":"Forge","loaderVersion":"47.2.0"})");
    {
        const VersionManager::InstanceTraits traits = vm.instanceTraits(metaId);
        check(traits.loaderName == QStringLiteral("Forge")
                  && traits.loaderVersion == QStringLiteral("47.2.0"),
              "QQ3 外部改了 wtfml.meta，下一次问就看得见",
              traits.loaderName + "/" + traits.loaderVersion);
        check(traits.type == QStringLiteral("snapshot"),
              "QQ3 meta 没写 type，类型仍然去 json 里要（两个答案各自缺就各自补）", traits.type);
    }

    // 同一个会话里再改一次（长度不同：缓存靠"修改时刻 + 大小"认出变化）。
    writeFileAt(metaPath, R"({"loaderName":"NeoForge","loaderVersion":"21.1.80","type":"release"})");
    {
        const VersionManager::InstanceTraits traits = vm.instanceTraits(metaId);
        check(traits.loaderName == QStringLiteral("NeoForge")
                  && traits.loaderVersion == QStringLiteral("21.1.80")
                  && traits.type == QStringLiteral("release"),
              "QQ3 同一个会话里再改一次 meta 也立刻生效（不吃上一次的缓存）",
              traits.loaderName + "/" + traits.loaderVersion + "/" + traits.type);
    }

    // ---- ① meta 写了加载器就以 meta 为准（json 里那个让位） ----
    {
        QJsonObject fields;
        fields.insert(QStringLiteral("loaderName"), QStringLiteral("OptiFine"));
        fields.insert(QStringLiteral("loaderVersion"), QStringLiteral("HD_U_I6"));
        QString error;
        check(vm.updateVersionMeta(fabricId, fields, &error),
              "QQ4 往 Fabric 实例上写 meta 加载器成功", error);
        const VersionManager::InstanceTraits traits = vm.instanceTraits(fabricId);
        check(traits.loaderName == QStringLiteral("OptiFine")
                  && traits.loaderVersion == QStringLiteral("HD_U_I6"),
              "QQ4 meta 里写了加载器就以 meta 为准（json 里那个 Fabric 让位）",
              traits.loaderName + "/" + traits.loaderVersion);
        check(traits.type == QStringLiteral("release"), "QQ4 类型不受影响", traits.type);
    }

    // ---- ② 认出来的结果跟着 json 缓存走 ----
    const QString swapId = QStringLiteral("QQ-换加载器");
    writeJson(swapId, R"({"id":"QQ-换加载器","type":"release",
        "libraries":[{"name":"net.fabricmc:fabric-loader:0.15.0"}]})");
    check(vm.instanceTraits(swapId).loaderName == QStringLiteral("Fabric"),
          "QQ5 先认出 Fabric");
    writeJson(swapId, R"({"id":"QQ-换加载器","type":"release",
        "libraries":[{"name":"net.minecraftforge:forge:1.20.1-47.2.0"}]})");
    check(vm.instanceTraits(swapId).loaderName == QStringLiteral("Fabric"),
          "QQ5 没清缓存就还是缓存里那一份（省下来的正是这次重读 + 重扫）");
    vm.clearVersionJsonCache();
    {
        const VersionManager::InstanceTraits traits = vm.instanceTraits(swapId);
        check(traits.loaderName == QStringLiteral("Forge"),
              "QQ5 clearVersionJsonCache() 之后重新认，认成 Forge", traits.loaderName);
        check(traits.loaderVersion == QStringLiteral("47.2.0"),
              "QQ5 换过来的版本号也对（只切第一个横杠）", traits.loaderVersion);
    }

    // ---- QQ7：父版本没装时，那份不存在的 json 一次会话里只读一遍、只告警一遍 ----
    //
    // 真机日志里 "Version JSON not found on disk" 刷了十几遍（实例树每重灌一次就重问一遍
    // 同一个父版本）。判据：三连问只落一条告警，不重读；父版本装上去之后下一问就看得见。
    const QString orphanId = QStringLiteral("QQ-没父版本");
    const QString orphanParentId = QStringLiteral("QQ-压根没装的父版本");
    writeJson(orphanId, QByteArray("{\"id\":\"QQ-没父版本\",\"inheritsFrom\":\"")
                          + orphanParentId.toUtf8() + "\"}");
    {
        g_missingJsonWarnings = 0;
        const QtMessageHandler previous = qInstallMessageHandler(countMissingJsonWarnings);
        for (int round = 0; round < 3; ++round) {
            const VersionManager::InstanceTraits traits = vm.instanceTraits(orphanId);
            check(traits.loaderName.isEmpty(),
                  QStringLiteral("QQ7 父版本没装时问不出加载器（第 %1 轮）").arg(round + 1));
        }
        qInstallMessageHandler(previous);
        check(g_missingJsonWarnings == 1,
              "QQ7 不存在的那份版本 json 只告警一次（不是每轮刷新一次）",
              QString::number(g_missingJsonWarnings));
    }
    // 盘上那份一变（这里是父版本装上了）就一定重读 —— "读不到就不缓存"那条规矩不能被
    // 这一层记的"上次没有"吃掉。
    writeJson(orphanParentId, R"({"id":"QQ-压根没装的父版本","type":"release",
        "libraries":[{"name":"net.fabricmc:fabric-loader:0.16.14"}]})");
    {
        const VersionManager::InstanceTraits traits = vm.instanceTraits(orphanId);
        check(traits.loaderName == QStringLiteral("Fabric")
                  && traits.loaderVersion == QStringLiteral("0.16.14"),
              "QQ7 父版本装好之后下一问就看得见（不留在「上次没有」上）",
              traits.loaderName + "/" + traits.loaderVersion);
        check(traits.type == QStringLiteral("release"), "QQ7 类型也跟着继承上来", traits.type);
    }

    // ---- 走之前把自己造的目录删干净 ----
    // NOTE: sandbox 是所有实例测试共用的。薄 json 那个目录（有 inheritsFrom、没有顶层
    // jar）在 installedVersions() 里会被当成"上一次没导完的整合包"报一条告警 —— 留着它，
    // 后面 InstalledScanTests 的"恰好上报一条"就会变成两条。这一类跨段污染没有别的
    // 挡法，只能由造它的人自己收拾（并且回头看一眼删掉没有）。
    QStringList leftovers;
    for (const QString &id :
         {fabricId, parentId, thinId, vanillaId, metaId, swapId, orphanId, orphanParentId}) {
        QDir(AppPaths::versionDir(id)).removeRecursively();
        if (QDir(AppPaths::versionDir(id)).exists()) {
            leftovers.append(id);
        }
    }
    check(leftovers.isEmpty(), "QQ6 造出来的目录一个都没剩下（不许污染后面几段）",
          leftovers.join(QLatin1Char('|')));

    return 0;
}

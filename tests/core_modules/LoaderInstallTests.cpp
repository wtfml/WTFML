#include "Support.hpp"
#include "manage/ModpackImport.hpp"

void runLoaderInstallTests() {
    section("V. 加载器安装（纯函数：地址 / 挑版本 / profile 检查 / 下标翻译）");

    const QString metaDir = QStringLiteral(REPO_ROOT_PATH) + "/tests/meta";
    const QJsonObject loaderProfile = loadJson(metaDir + "/fabric-loader-0.19.5-1.19.json");
    check(!loaderProfile.isEmpty(), "V 真 profile fixture 读到了");
    if (loaderProfile.isEmpty()) {
        return;
    }

    // --- V1. 地址怎么拼 ---------------------------------------------------------
    check(LoaderMeta::fabricLoaderListUrl(QStringLiteral("1.19")) ==
              QStringLiteral("https://meta.fabricmc.net/v2/versions/loader/1.19"),
          "V1 加载器列表的地址",
          LoaderMeta::fabricLoaderListUrl(QStringLiteral("1.19")));
    check(LoaderMeta::fabricProfileUrl(QStringLiteral("1.19"), QStringLiteral("0.19.5")) ==
              QStringLiteral(
                  "https://meta.fabricmc.net/v2/versions/loader/1.19/0.19.5/profile/json"),
          "V1 profile 的地址",
          LoaderMeta::fabricProfileUrl(QStringLiteral("1.19"), QStringLiteral("0.19.5")));
    check(LoaderMeta::fabricLoaderId(QStringLiteral("1.19"), QStringLiteral("0.19.5")) ==
              QStringLiteral("fabric-loader-0.19.5-1.19"),
          "V1 版本 id 是 fabric-loader-<加载器>-<游戏>",
          LoaderMeta::fabricLoaderId(QStringLiteral("1.19"), QStringLiteral("0.19.5")));
    check(!LoaderMeta::fabricMetaBase().endsWith(QLatin1Char('/')),
          "V1 根地址不带结尾的斜杠（拼出来不会出现双斜杠）");

    // --- V2. 列表里挑哪一个 -----------------------------------------------------
    // 列表的形状照实测来：**按新到旧排**，而且只有最新那条的 stable 是 true。
    const auto entry = [](const QString &version, bool stable) {
        const QJsonObject loader{{QStringLiteral("version"), version},
                                 {QStringLiteral("stable"), stable}};
        return QJsonObject{{QStringLiteral("loader"), loader}};
    };
    {
        const QJsonArray realShaped{entry(QStringLiteral("0.19.5"), true),
                                    entry(QStringLiteral("0.19.4"), false),
                                    entry(QStringLiteral("0.19.3"), false)};
        const LoaderMeta::LoaderPick pick = LoaderMeta::pickFabricLoader(realShaped);
        check(pick.ok() && pick.version == QStringLiteral("0.19.5") && pick.stable,
              "V2 实际形状（最新那条是 stable）-> 挑最新那条", pick.version);

        // 有更新的非 stable 在前面时，仍然挑 stable 那条（版本号比较不是我们判断的事，
        // 但"优先正式版"是）。挑错这条的表现是"装上一个 corner 版本"。
        const QJsonArray newerUnstable{entry(QStringLiteral("0.20.0"), false),
                                       entry(QStringLiteral("0.19.5"), true),
                                       entry(QStringLiteral("0.19.4"), false)};
        const LoaderMeta::LoaderPick stablePick = LoaderMeta::pickFabricLoader(newerUnstable);
        check(stablePick.ok() && stablePick.version == QStringLiteral("0.19.5") && stablePick.stable,
              "V2 前面有更新的非正式版时，挑的是那条稳定的", stablePick.version);

        // 一个 stable 都没有（老版本的游戏常常是这种）：退回第一个，并且**如实**说它不是。
        const QJsonArray allUnstable{entry(QStringLiteral("0.15.0"), false),
                                     entry(QStringLiteral("0.14.9"), false)};
        const LoaderMeta::LoaderPick firstPick = LoaderMeta::pickFabricLoader(allUnstable);
        check(firstPick.ok() && firstPick.version == QStringLiteral("0.15.0") && !firstPick.stable,
              "V2 没有 stable 时退回第一个，并如实标成「不是正式版」", firstPick.version);

        check(!LoaderMeta::pickFabricLoader(QJsonArray()).ok(),
              "V2 空列表（快照那种「还没支持」的版本）-> 挑不出来，由调用方报错");

        // 坏条目跳过，不要让一个空 version 把整次安装带偏。
        const QJsonArray withJunk{entry(QString(), false), QJsonObject(), entry(QStringLiteral("0.19.0"), false)};
        const LoaderMeta::LoaderPick junkPick = LoaderMeta::pickFabricLoader(withJunk);
        check(junkPick.ok() && junkPick.version == QStringLiteral("0.19.0"),
              "V2 没有版本号的条目被跳过，挑到后面那个真的", junkPick.version);
        check(!LoaderMeta::pickFabricLoader(QJsonArray{QJsonObject(), entry(QString(), false)}).ok(),
              "V2 全是坏条目时也挑不出来（不是「挑一个空串」）");
    }

    // --- V3. profile 能不能照这样装下去 -----------------------------------------
    {
        const LoaderMeta::ProfileCheck good =
            LoaderMeta::checkFabricProfile(loaderProfile, QStringLiteral("1.19"));
        check(good.ok(), "V3 真 profile + 它真正基于的那个版本 -> 通过",
              QString::number(static_cast<int>(good.problem)));
        // 这条是"别自己发明 id 格式"的钉子：目录名同时是别的启动器/加载器找版本时用的名字。
        check(good.ok() &&
                  good.id == LoaderMeta::fabricLoaderId(QStringLiteral("1.19"),
                                                        QStringLiteral("0.19.5")),
              "V3 真 profile 里的 id 正好是 fabric-loader-0.19.5-1.19（我们没自己造）",
              good.id);
        check(good.ok() && good.parent == QStringLiteral("1.19"),
              "V3 真 profile 的 inheritsFrom 就是原版 1.19", good.parent);

        // 拿真数据只换一个游戏版本 = 最像真事故的那种情况（服务端给了别的版本的 profile）。
        const LoaderMeta::ProfileCheck wrongParent =
            LoaderMeta::checkFabricProfile(loaderProfile, QStringLiteral("1.18.2"));
        check(wrongParent.problem == LoaderMeta::ProfileProblem::WrongParent &&
                  wrongParent.parent == QStringLiteral("1.19"),
              "V3 装到 1.18.2 上、profile 却基于 1.19 -> WrongParent（宁可停下也不装出个错版本）");

        const auto withId = [&loaderProfile](const QString &id) {
            QJsonObject copy = loaderProfile;
            copy[QStringLiteral("id")] = id;
            return copy;
        };
        const auto idProblem = [&](const QString &id) {
            return LoaderMeta::checkFabricProfile(withId(id), QStringLiteral("1.19")).problem;
        };
        check(idProblem(QStringLiteral("../evil")) == LoaderMeta::ProfileProblem::BadId,
              "V3 id 里带 ../ -> BadId（这个 json 会原样落盘成 versions/<id>/，不能写到外面去）");
        check(idProblem(QStringLiteral("a/b")) == LoaderMeta::ProfileProblem::BadId,
              "V3 id 里带路径分隔符 -> BadId");
        check(idProblem(QStringLiteral("a b")) == LoaderMeta::ProfileProblem::BadId,
              "V3 id 里带空格 -> BadId");
        check(idProblem(QString()) == LoaderMeta::ProfileProblem::BadId, "V3 id 空着 -> BadId");
        check(idProblem(QStringLiteral(".")) == LoaderMeta::ProfileProblem::BadId &&
                  idProblem(QStringLiteral("..")) == LoaderMeta::ProfileProblem::BadId,
              "V3 id 是 . 或 .. -> BadId（它们能过字符白名单，但不是版本名）");

        // 白名单本身的真值表：**和 maven 那条是同一个**（都是"网上来的字符串当路径段用"）。
        check(LoaderMeta::isSafeVersionId(QStringLiteral("fabric-loader-0.19.5-1.19")) &&
                  LoaderMeta::isSafeVersionId(QStringLiteral("1.19.2")) &&
                  LoaderMeta::isSafeVersionId(QStringLiteral("26.2-snapshot-1")) &&
                  LoaderMeta::isSafeVersionId(QStringLiteral("a_b+c.d")),
              "V3 合法版本名（含真 Fabric id、快照名、下划线加号）都放行");
        check(!LoaderMeta::isSafeVersionId(QStringLiteral("a\\b")) &&
                  !LoaderMeta::isSafeVersionId(QStringLiteral("a:b")) &&
                  !LoaderMeta::isSafeVersionId(QStringLiteral("上")) &&
                  !LoaderMeta::isSafeVersionId(QStringLiteral("1.19 ")) &&
                  !LoaderMeta::isSafeVersionId(QStringLiteral("../1.19")),
              "V3 反斜杠 / 冒号 / 中文 / 尾空格 / ../ 一律拒绝");

        // 缺 inheritsFrom：不是"通过"，而是"父版本对不上"（空 != 1.19）。
        QJsonObject noParent = loaderProfile;
        noParent.remove(QStringLiteral("inheritsFrom"));
        check(LoaderMeta::checkFabricProfile(noParent, QStringLiteral("1.19")).problem ==
                  LoaderMeta::ProfileProblem::WrongParent,
              "V3 profile 里没有 inheritsFrom -> 当成父版本对不上，不装");
    }

    // --- V4. 界面下标 -> 加载器 -------------------------------------------------
    {
        check(VersionManager::loaderKindCount() == 5,
              "V4 五个加载器（界面那组钮里 0..4 是它们，第 5 个是「不装」）",
              QString::number(VersionManager::loaderKindCount()));

        struct Expect {
            int index;
            VersionManager::LoaderKind kind;
            const char *name;
        };
        const Expect expects[] = {
            {0, VersionManager::LoaderKind::Fabric, "Fabric"},
            {1, VersionManager::LoaderKind::Forge, "Forge"},
            {2, VersionManager::LoaderKind::NeoForge, "NeoForge"},
            {3, VersionManager::LoaderKind::OptiFine, "OptiFine"},
            {4, VersionManager::LoaderKind::Quilt, "Quilt"},
        };
        for (const Expect &e : expects) {
            // 故意先填一个别的：翻译失败时**不许**动这个变量（调用方可能还指望旧的）。
            VersionManager::LoaderKind kind = VersionManager::LoaderKind::OptiFine;
            const bool ok = VersionManager::loaderKindFromIndex(e.index, &kind);
            check(ok && kind == e.kind &&
                      VersionManager::loaderDisplayName(kind) == QLatin1String(e.name) &&
                      VersionManager::loaderIndexForKind(e.kind) == e.index,
                  QString("V4 下标 %1 <-> %2（互为反函数）").arg(e.index).arg(QLatin1String(e.name)),
                  VersionManager::loaderDisplayName(kind));
        }

        // 现有四种加载器下标稳定不变，不因新增 Quilt 而错位。
        check(VersionManager::loaderIndexForKind(VersionManager::LoaderKind::Fabric) == 0 &&
                  VersionManager::loaderIndexForKind(VersionManager::LoaderKind::Forge) == 1 &&
                  VersionManager::loaderIndexForKind(VersionManager::LoaderKind::NeoForge) == 2 &&
                  VersionManager::loaderIndexForKind(VersionManager::LoaderKind::OptiFine) == 3 &&
                  VersionManager::loaderIndexForKind(VersionManager::LoaderKind::Quilt) == 4,
              "V4 现有四种加载器的选择下标保持 0..3 不错位，Quilt 落在下标 4");

        VersionManager::LoaderKind keep = VersionManager::LoaderKind::NeoForge;
        check(!VersionManager::loaderKindFromIndex(-1, &keep) &&
                  keep == VersionManager::LoaderKind::NeoForge,
              "V4 -1（一个都没勾）翻不出来，而且没动传进去的那个变量");
        check(!VersionManager::loaderKindFromIndex(VersionManager::loaderKindCount(), &keep) &&
                  keep == VersionManager::LoaderKind::NeoForge,
              "V4 下标 5（界面上的「不装」）翻不出来 —— 它是「装纯原版」，不是加载器");
        check(!VersionManager::loaderKindFromIndex(99, &keep),
              "V4 越界下标翻不出来（界面表和枚举表对不上时宁可什么都不装）");
        check(VersionManager::loaderKindFromIndex(0, nullptr),
              "V4 out 允许传 nullptr（只想问「这个下标认不认」）");

        // 五个都接通了（OptiFine 走外部安装器，Quilt 走与 Fabric 共用的 profile + 库路径）。
        check(VersionManager::loaderInstallSupported(VersionManager::LoaderKind::Fabric) &&
                  VersionManager::loaderInstallSupported(VersionManager::LoaderKind::Forge) &&
                  VersionManager::loaderInstallSupported(VersionManager::LoaderKind::NeoForge) &&
                  VersionManager::loaderInstallSupported(VersionManager::LoaderKind::OptiFine) &&
                  VersionManager::loaderInstallSupported(VersionManager::LoaderKind::Quilt),
              "V4 五个加载器都接通了");

        // 显示名不能空：它既写进 wtfml.meta 的 loaderName，也是弹窗里的主语。
        for (int i = 0; i < VersionManager::loaderKindCount(); ++i) {
            VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
            VersionManager::loaderKindFromIndex(i, &kind);
            check(!VersionManager::loaderDisplayName(kind).isEmpty(),
                  QString("V4 下标 %1 的加载器有显示名").arg(i));
        }
    }

    // --- V5. Forge：版本从推广表里挑、地址怎么拼 --------------------------------
    //
    // 这份表是 4 KB 的推广版本表，一次把**所有**游戏版本的 Forge 推荐版本都带回来。
    const QJsonObject forgePromos = loadJson(metaDir + "/forge-promotions_slim.json");
    check(!forgePromos[QStringLiteral("promos")].toObject().isEmpty(),
          "V5 真推广表读到了（顶层得有个非空的 promos）");
    {
        const LoaderMeta::LoaderPick rec =
            LoaderMeta::pickForgeVersion(forgePromos, QStringLiteral("1.20.1"));
        check(rec.ok() && rec.version == QStringLiteral("47.4.10") && rec.stable,
              "V5 有 recommended 时挑 recommended（不是数字更大的 latest）",
              rec.ok() ? QStringLiteral("%1 stable=%2").arg(rec.version).arg(rec.stable)
                       : QStringLiteral("挑不出来"));

        // 真表里有一大批游戏版本**只有 latest 一个键**（实测 1.20 只有 46.0.14，
        // 1.19.1 只有 42.0.9，1.21.6 只有 56.0.9）：必须兜底到它，否则那些版本一个都
        // 装不了。这一条只能拿真数据验 —— 自造的样例很容易写成"每个版本两个键都在"。
        const LoaderMeta::LoaderPick onlyLatest =
            LoaderMeta::pickForgeVersion(forgePromos, QStringLiteral("1.20"));
        check(onlyLatest.ok() && onlyLatest.version == QStringLiteral("46.0.14") &&
                  !onlyLatest.stable,
              "V5 只有 latest 的游戏版本 -> 用 latest，并如实标成「不是推荐的」",
              onlyLatest.ok() ? QStringLiteral("%1 stable=%2").arg(onlyLatest.version)
                                    .arg(onlyLatest.stable)
                              : QStringLiteral("挑不出来"));

        // 反过来钉一下上面那条的前提：1.7.10 在真表里**两个键都有**（值还一样），
        // 不能假定它只有 latest。
        check(LoaderMeta::pickForgeVersion(forgePromos, QStringLiteral("1.7.10")).stable,
              "V5 1.7.10 在真表里两个键都有（所以它走的是 recommended 那一支）",
              LoaderMeta::pickForgeVersion(forgePromos, QStringLiteral("1.7.10")).version);

        // 表里根本没有这个游戏版本（快照、或者比 Forge 最新的正式版还新）：挑不出来，
        // 由调用方报"还没有支持这个版本"，**不能**退回装纯原版。
        check(!LoaderMeta::pickForgeVersion(forgePromos, QStringLiteral("26.2-snapshot-1")).ok(),
              "V5 表里没有的游戏版本 -> 挑不出来");
        check(!LoaderMeta::pickForgeVersion(QJsonObject(), QStringLiteral("1.20.1")).ok(),
              "V5 表整个是空的（网络拿到一份烂东西）-> 挑不出来，不崩");

        // 地址和目录名。这两个名字**不是我们发明的**：`1.20.1-forge-47.4.10` 是安装器
        // 自己写进版本 json 的 id，也是别的启动器认的目录名。
        check(LoaderMeta::forgeLoaderId(QStringLiteral("1.20.1"), QStringLiteral("47.4.10")) ==
                  QStringLiteral("1.20.1-forge-47.4.10"),
              "V5 Forge 的目录名是 <游戏>-forge-<加载器>",
              LoaderMeta::forgeLoaderId(QStringLiteral("1.20.1"), QStringLiteral("47.4.10")));
        check(LoaderMeta::forgeInstallerUrl(QStringLiteral("1.20.1"),
                                            QStringLiteral("47.4.10")) ==
                  QStringLiteral("https://maven.minecraftforge.net/net/minecraftforge/forge/"
                                 "1.20.1-47.4.10/forge-1.20.1-47.4.10-installer.jar"),
              "V5 Forge 安装器地址（maven 直链，坐标是 <游戏>-<加载器>）",
              LoaderMeta::forgeInstallerUrl(QStringLiteral("1.20.1"),
                                            QStringLiteral("47.4.10")));
        check(!LoaderMeta::forgePromotionsUrl().contains(QStringLiteral(" ")),
              "V5 推广表地址里没有空格（拼坏了的症状是 404，不是崩）");
    }

    // --- V5b. 镜像那份推广表是**旧的**--------------------------
    //
    // 所以版本表那条路是"镜像发一发，挑不出来就换官方再发一发"（见 VersionManager::
    // retryInstallerListFromOfficial）。这一节钉的就是那个前提：镜像那份确实挑不出来、
    // 官方那份确实挑得出来。
    {
        QFile mirrorFile(metaDir + "/forge-promotions_slim_mirror.json");
        QByteArray mirrorBytes;
        if (mirrorFile.open(QIODevice::ReadOnly)) {
            mirrorBytes = mirrorFile.readAll();
        }
        const QJsonObject promosMirror =
            loadJson(metaDir + "/forge-promotions_slim_mirror.json");
        check(!promosMirror[QStringLiteral("promos")].toObject().isEmpty() &&
                  mirrorBytes.size() > 100,
              "V5b 镜像那份推广表 fixture 读到了", QString::number(mirrorBytes.size()));
        if (!promosMirror.isEmpty()) {
            check(!LoaderMeta::pickForgeVersion(promosMirror, QStringLiteral("26.2")).ok(),
                  "V5b 拿镜像那份挑 26.2 -> 挑不出来（用户看到的「没有支持」就是这么来的）");
            check(!LoaderMeta::pickForgeVersion(promosMirror, QStringLiteral("1.20.1")).ok(),
                  "V5b 拿镜像那份挑 1.20.1 -> 也挑不出来（不是只有最新那几个版本受影响）");
            check(LoaderMeta::pickForgeVersion(promosMirror, QStringLiteral("1.8.9")).ok(),
                  "V5b 镜像那份里**有** 1.8.9（所以那次 1.8.9 的安装器真的被下下来、跑了 —— "
                  "日志里那段报错是另一件事）",
                  LoaderMeta::pickForgeVersion(promosMirror, QStringLiteral("1.8.9")).version);
            // 同一份代码、换官方那份就挑得出来 —— 兜底那条路的意义就在这里。
            check(LoaderMeta::pickForgeVersion(forgePromos, QStringLiteral("26.2")).ok(),
                  "V5b 换官方那份之后挑得出来（镜像挑不出来就退官方，有用的）",
                  LoaderMeta::pickForgeVersion(forgePromos, QStringLiteral("26.2")).version);
            // 两份的体量：镜像那份小得多。不靠"哪个文件新"判断，靠"挑不出来就退"。
            QFileInfo officialInfo(metaDir + "/forge-promotions_slim.json");
            check(mirrorBytes.size() < officialInfo.size(),
                  "V5b 镜像那份比官方那份小得多（1782 B / 4111 B）",
                  QStringLiteral("%1 / %2").arg(mirrorBytes.size()).arg(officialInfo.size()));
        }
    }

    // --- V6. NeoForge：版本号比大小 / 前缀匹配 / 从 maven 元数据里挑 ------------
    {
        // 一个小糖：让下面那几条断言能写成 nums({21, 1, 251}) 的样子。
        const auto nums = [](QList<int> values) { return values; };
        // 反过来：把解析结果写成 "21.1" 的样子，失败时看得懂（QList<int> 没有 join()）。
        const auto dotted = [](const QList<int> &values) {
            QStringList parts;
            for (int value : values) {
                parts.append(QString::number(value));
            }
            return parts.join(QLatin1Char('.'));
        };

        check(LoaderMeta::parseDottedVersion(QStringLiteral("21.1.251")) ==
                  nums({21, 1, 251}),
              "V6 点分版本号解析", QStringLiteral("21.1.251"));
        check(LoaderMeta::parseDottedVersion(QString()).isEmpty() &&
                  LoaderMeta::parseDottedVersion(QStringLiteral("21.1-beta")).isEmpty() &&
                  LoaderMeta::parseDottedVersion(QStringLiteral("+5")).isEmpty() &&
                  LoaderMeta::parseDottedVersion(QStringLiteral(" 5")).isEmpty() &&
                  LoaderMeta::parseDottedVersion(QStringLiteral("21..1")).isEmpty(),
              "V6 不是纯数字的段一律解析失败（顺带挡住带符号/空白的写法）");

        check(LoaderMeta::compareDottedVersions(nums({21, 1, 251}), nums({21, 10, 64})) < 0 &&
                  LoaderMeta::compareDottedVersions(nums({26, 2, 0, 88}), nums({26, 2})) > 0 &&
                  LoaderMeta::compareDottedVersions(nums({21, 1, 251}), nums({21, 1, 251})) == 0,
              "V6 比大小：短的按补零算（26.2 < 26.2.0.88），且 21.1.x < 21.10.x");

        check(LoaderMeta::isDottedPrefix(nums({21, 1}), nums({21, 1, 251})) &&
                  !LoaderMeta::isDottedPrefix(nums({21, 1}), nums({21, 10, 64})) &&
                  !LoaderMeta::isDottedPrefix(nums({21, 1, 251}), nums({21, 1})) &&
                  !LoaderMeta::isDottedPrefix(QList<int>(), nums({21, 1})),
              "V6 前缀是**逐段**比的：21.1 是 21.1.251 的前缀，不是 21.10.64 的");

        // 游戏版本 -> NeoForge 前缀。去掉开头的 `1.` 再取两段；只有一段时补一个 `.0`
        // （不补的话 `1.21` 会退化成前缀 `[21]`，把 21.1.x / 21.11.x 全算进来）。
        check(LoaderMeta::neoForgeVersionPrefix(QStringLiteral("1.21.1")) == nums({21, 1}) &&
                  LoaderMeta::neoForgeVersionPrefix(QStringLiteral("1.20.2")) == nums({20, 2}) &&
                  LoaderMeta::neoForgeVersionPrefix(QStringLiteral("1.21")) == nums({21, 0}) &&
                  LoaderMeta::neoForgeVersionPrefix(QStringLiteral("26.2")) == nums({26, 2}) &&
                  LoaderMeta::neoForgeVersionPrefix(QStringLiteral("26.1.2")) == nums({26, 1}),
              "V6 游戏版本 -> NeoForge 前缀",
              QStringLiteral("%1 / %2")
                  .arg(dotted(LoaderMeta::neoForgeVersionPrefix(QStringLiteral("1.21.1"))),
                       dotted(LoaderMeta::neoForgeVersionPrefix(QStringLiteral("1.21")))));
        check(LoaderMeta::neoForgeVersionPrefix(QStringLiteral("snapshot")).isEmpty(),
              "V6 不是数字的游戏版本 -> 空前缀（调用方据此报错，不会乱挑）");

        // --- V6b 自然序比较（compareVersionStrings）----------------------------
        //
        // 这一级是 修的：排序的最后一级 tie-break 原来是 `a < b`（字符串），
        // 而它接手的正是"切掉后缀之后数字一模一样"的那一族 —— NeoForge 26.1 那批
        // `26.1.0.0-alpha.N+snapshot-N`。按字符串比 `alpha.10 < alpha.2`，于是「最新档」
        // 会挑中一个更旧的快照（列表是"新到旧"的）。
        {
            const auto cmp = [](const QString &l, const QString &r) {
                return LoaderMeta::compareVersionStrings(l, r);
            };
            check(cmp(QStringLiteral("26.1.0.0-alpha.2+snapshot-1"),
                      QStringLiteral("26.1.0.0-alpha.1+snapshot-1")) > 0,
                  "V6b alpha.2 比 alpha.1 新（按字符串比 `\"alpha.1\" < \"alpha.2\"` 会反过来）");
            check(cmp(QStringLiteral("26.1.0.0-alpha.10+snapshot-1"),
                      QStringLiteral("26.1.0.0-alpha.2+snapshot-1")) > 0,
                  "V6b alpha.10 比 alpha.2 新（数字段按**数值**比，不是按首位字符）");
            check(cmp(QStringLiteral("26.3.0.10-beta"), QStringLiteral("26.3.0.9-beta")) > 0,
                  "V6b 26.3.0.10-beta 比 26.3.0.9-beta 新");
            check(cmp(QStringLiteral("1.21.10"), QStringLiteral("1.21.2")) > 0,
                  "V6b 1.21.10 比 1.21.2 新（按字符串比是反的：1.21.10 排在 1.21.2 前面）");
            check(cmp(QStringLiteral("1.21"), QStringLiteral("1.21.0")) < 0,
                  "V6b 前缀短的算旧（1.21 < 1.21.0）—— 靠这一条把次序钉死成全序");
            check(cmp(QStringLiteral("1.2.3"), QStringLiteral("1.2.3")) == 0,
                  "V6b 同一个版本号比出来是 0");
            // 全序的三条性质：反对称、传递、（对同一对）结果稳定。std::sort 要的正是这些。
            check(cmp(QStringLiteral("a.2"), QStringLiteral("a.10")) ==
                      -cmp(QStringLiteral("a.10"), QStringLiteral("a.2")),
                  "V6b 反对称：cmp(a,b) == -cmp(b,a)");
            check(cmp(QStringLiteral("1.9"), QStringLiteral("1.10")) < 0 &&
                      cmp(QStringLiteral("1.10"), QStringLiteral("1.11")) < 0 &&
                      cmp(QStringLiteral("1.9"), QStringLiteral("1.11")) < 0,
                  "V6b 传递：1.9 < 1.10 < 1.11 且 1.9 < 1.11");
        }

        // --- V6c 排序里"后缀不同、数字相同"那一族：新的在前 ---------------------
        // 拿真那一族的形状造一份最小的列表（26.1.0.0-alpha.1 / .2 / .10 + 一个正式版）。
        // 这几条**故意**用 sortLoaderVersionsNewestFirst() 自己排，而不是直接调比较函数：
        // 判据就落在这个函数的最后一级。
        {
            LoaderMeta::LoaderVersionList list;
            list.append({QStringLiteral("26.1.0.0-alpha.1+snapshot-1"), false});
            list.append({QStringLiteral("26.1.0.0-alpha.10+snapshot-1"), false});
            list.append({QStringLiteral("26.1.0.0-alpha.2+snapshot-1"), false});
            list.append({QStringLiteral("26.1.0.0"), true});
            LoaderMeta::sortLoaderVersionsNewestFirst(list);
            check(list.size() == 4, "V6c 排完还是四条（没丢没重）");
            check(list.value(0).version == QStringLiteral("26.1.0.0"),
                  "V6c 正式版排在最前（数字一样时正式版在前，这一级没被 tie-break 顶掉）",
                  list.value(0).version);
            check(list.value(1).version == QStringLiteral("26.1.0.0-alpha.10+snapshot-1") &&
                      list.value(2).version == QStringLiteral("26.1.0.0-alpha.2+snapshot-1") &&
                      list.value(3).version == QStringLiteral("26.1.0.0-alpha.1+snapshot-1"),
                  "★ V6c 三个快照按 10 > 2 > 1 排",
                  QStringLiteral("%1, %2, %3")
                      .arg(list.value(1).version, list.value(2).version, list.value(3).version));
        }

        // 真 maven 元数据。
        QFile mavenFile(metaDir + "/neoforge-maven-metadata.xml");
        QByteArray maven;
        if (mavenFile.open(QIODevice::ReadOnly)) {
            maven = mavenFile.readAll();
        }
        check(maven.size() > 10000, "V6 真 maven 元数据读到了",
              QString::number(maven.size()));
        if (!maven.isEmpty()) {
            // 实测：21.1.x 那一族数字最大的是 21.1.251，而这**正好是文件末尾附近**那个
            // 夹在一堆 26.3 快照中间的行 —— 所以"挑出来是它"不能证明什么，"不是 26.3.x
            // 也不是 21.10.64"才是这两条断言的意思。
            const LoaderMeta::LoaderPick pick1211 =
                LoaderMeta::pickNeoForgeVersion(maven, QStringLiteral("1.21.1"));
            check(pick1211.ok() && pick1211.version == QStringLiteral("21.1.251"),
                  "V6 1.21.1 -> 21.1.251（真数据里 21.1.x 那一族最大那个）",
                  pick1211.version);
            check(!pick1211.version.startsWith(QStringLiteral("21.10.")),
                  "V6 没有把 21.10.x 当成 1.21.1 的加载器（字符串前缀比就会犯这个错）",
                  pick1211.version);

            // 反过来：1.21.10 那一族也得挑得出来（证明前缀比较不是"只认短的"）。
            const LoaderMeta::LoaderPick pick12110 =
                LoaderMeta::pickNeoForgeVersion(maven, QStringLiteral("1.21.10"));
            check(pick12110.ok() && pick12110.version == QStringLiteral("21.10.64"),
                  "V6 1.21.10 -> 21.10.64（反过来那一族）", pick12110.version);

            // 两段式游戏版本：1.21 -> 21.0.x。
            const LoaderMeta::LoaderPick pick121 =
                LoaderMeta::pickNeoForgeVersion(maven, QStringLiteral("1.21"));
            check(pick121.ok() && pick121.version == QStringLiteral("21.0.167"),
                  "V6 1.21 -> 21.0.167（前缀补的那个 .0 起作用了）", pick121.version);

            // 四段式：26.2 -> 26.2.0.88。这条钉的是"比大小要能比长度不一样的两串"。
            const LoaderMeta::LoaderPick pick262 =
                LoaderMeta::pickNeoForgeVersion(maven, QStringLiteral("26.2"));
            check(pick262.ok() && pick262.version == QStringLiteral("26.2.0.88"),
                  "V6 26.2 -> 26.2.0.88（四段）", pick262.version);

            // NeoForge 从 1.20.2 才开始有：1.20.1 得挑不出来（真数据里确实一个都没有）。
            check(!LoaderMeta::pickNeoForgeVersion(maven, QStringLiteral("1.20.1")).ok(),
                  "V6 1.20.1 -> 挑不出来（NeoForge 那时还没出生）");

            // 带后缀的一个都不能挑中：`-beta` 那一大堆占了整个文件的大多数。
            check(!pick1211.version.contains(QLatin1Char('-')),
                  "V6 挑中的版本号不带后缀（beta 全都跳过了）", pick1211.version);
        }

        check(LoaderMeta::neoforgeLoaderId(QStringLiteral("21.1.251")) ==
                  QStringLiteral("neoforge-21.1.251"),
              "V6 NeoForge 的目录名**不带**游戏版本前缀（和 Forge 不一样，不能统一）",
              LoaderMeta::neoforgeLoaderId(QStringLiteral("21.1.251")));
        check(LoaderMeta::neoforgeInstallerUrl(QStringLiteral("21.1.251")) ==
                  QStringLiteral("https://maven.neoforged.net/releases/net/neoforged/neoforge/"
                                 "21.1.251/neoforge-21.1.251-installer.jar"),
              "V6 NeoForge 安装器地址",
              LoaderMeta::neoforgeInstallerUrl(QStringLiteral("21.1.251")));
    }

    // --- V6b. 官方地址 -> 镜像地址（含 新认的 Forge / NeoForge 那几条）------
    //
    // 这一批是**纯函数**、不碰网络，钉的是"哪条地址会被换成什么"。换错的症状不是崩，而是
    // "下载全失败"或者"白等了很久才发现根本没走镜像"，所以每条都得有写死的期望值。
    // NOTE: 这几条 才补上（在此之前这四个函数一条断言都没有）。
    {
        const QString base = QStringLiteral("https://bmclapi2.bangbang93.com");

        // 原来就认得的那三个：路径形状和官方一样（只有资源对象要多一层 /assets）。
        check(DownloadManager::mirrorUrl(
                  QStringLiteral("https://launchermeta.mojang.com/mc/game/version_manifest.json")) ==
                  base + QStringLiteral("/mc/game/version_manifest.json"),
              "V6b 版本清单：换 host、路径照抄",
              DownloadManager::mirrorUrl(QStringLiteral(
                  "https://launchermeta.mojang.com/mc/game/version_manifest.json")));
        check(DownloadManager::mirrorUrl(
                  QStringLiteral("https://resources.download.minecraft.net/ab/abcdef")) ==
                  base + QStringLiteral("/assets/ab/abcdef"),
              "V6b 资源对象：多一层 /assets");
        check(DownloadManager::mirrorUrl(
                  QStringLiteral("https://libraries.minecraft.net/com/foo/bar/1.0/bar-1.0.jar")) ==
                  base + QStringLiteral("/maven/com/foo/bar/1.0/bar-1.0.jar"),
              "V6b 原版库：加 /maven 前缀");

        check(DownloadManager::mirrorUrl(LoaderMeta::forgeInstallerUrl(
                  QStringLiteral("1.20.1"), QStringLiteral("47.4.10"))) ==
                  base + QStringLiteral("/maven/net/minecraftforge/forge/1.20.1-47.4.10/"
                                        "forge-1.20.1-47.4.10-installer.jar"),
              "V6b Forge 安装器 jar 走镜像（以前是原样过去）",
              DownloadManager::mirrorUrl(LoaderMeta::forgeInstallerUrl(QStringLiteral("1.20.1"),
                                                                      QStringLiteral("47.4.10"))));
        check(DownloadManager::mirrorUrl(
                  QStringLiteral("https://maven.minecraftforge.net/net/minecraftforge/"
                                 "JarJarFileSystems/0.4.2/JarJarFileSystems-0.4.2.jar")) ==
                  base + QStringLiteral("/maven/net/minecraftforge/JarJarFileSystems/0.4.2/"
                                        "JarJarFileSystems-0.4.2.jar"),
              "V6b Forge 的库也走镜像（装出来的版本 json 里就是这些地址）");
        check(DownloadManager::mirrorUrl(LoaderMeta::forgePromotionsUrl()) ==
                  base + QStringLiteral("/maven/net/minecraftforge/forge/promotions_slim.json"),
              "V6b Forge 推广表：files 那个 host 也是标准 maven 布局",
              DownloadManager::mirrorUrl(LoaderMeta::forgePromotionsUrl()));
        check(DownloadManager::mirrorUrl(LoaderMeta::neoforgeInstallerUrl(
                  QStringLiteral("21.1.251"))) ==
                  base + QStringLiteral("/maven/net/neoforged/neoforge/21.1.251/"
                                        "neoforge-21.1.251-installer.jar"),
              "V6b NeoForge 安装器 jar 走镜像（官方那个 /releases 前缀要**去掉**）",
              DownloadManager::mirrorUrl(
                  LoaderMeta::neoforgeInstallerUrl(QStringLiteral("21.1.251"))));
        check(DownloadManager::mirrorUrl(LoaderMeta::neoforgeMavenMetadataUrl()) ==
                  base + QStringLiteral("/maven/net/neoforged/neoforge/maven-metadata.xml"),
              "V6b NeoForge 的版本表走镜像");

        // 镜像上**没有**的那些：宁可原样走官方，也不许拼一个不存在的地址出来。
        check(DownloadManager::mirrorUrl(
                  QStringLiteral("https://maven.neoforged.net/snapshots/net/neoforged/neoforge/"
                                 "26.2.0.1/neoforge-26.2.0.1-installer.jar")) ==
                  QStringLiteral("https://maven.neoforged.net/snapshots/net/neoforged/neoforge/"
                                 "26.2.0.1/neoforge-26.2.0.1-installer.jar"),
              "V6b NeoForge 的 snapshots 支**不**换（镜像只收 releases，换了就是 404）");
        check(DownloadManager::mirrorUrl(
                  QStringLiteral("https://meta.fabricmc.net/v2/versions/loader/1.19")) ==
                  QStringLiteral("https://meta.fabricmc.net/v2/versions/loader/1.19"),
              "V6b 认不出来的 host（Fabric 那套）原样返回");

        // 按版本号 / 索引号重拼的那三个（官方地址里的 sha1 镜像不认，只能用 id 拼）。
        check(DownloadManager::mirrorVersionJsonUrl(QStringLiteral("1.18.2")) ==
                  base + QStringLiteral("/version/1.18.2/json"),
              "V6b 版本 JSON 按版本号拼",
              DownloadManager::mirrorVersionJsonUrl(QStringLiteral("1.18.2")));
        check(DownloadManager::mirrorClientJarUrl(QStringLiteral("1.18.2")) ==
                  base + QStringLiteral("/version/1.18.2/client"),
              "V6b 客户端 jar 按版本号拼");
        check(DownloadManager::mirrorAssetIndexUrl(QStringLiteral("1.18")) ==
                  base + QStringLiteral("/indexes/1.18.json"),
              "V6b 资源索引按索引号拼（这条地址在镜像上回 404，所以下载那边有一条"
              "退回官方的兜底 —— 那一段只有联网真跑才看得见，见 loader-install 的 mirror 模式）");

        const QString mavenRoot = DownloadManager::mirrorMavenRootUrl();
        check(mavenRoot == base + QStringLiteral("/maven/"),
              "V6b --mirror 用的根地址就是这个", mavenRoot);
        check(mavenRoot.endsWith(QLatin1Char('/')),
              "V6b 根地址结尾必须有斜杠");
    }

    // --- V6c. 安装器命令行（--mirror 把安装器自己那条下载路钉到一个明确的根上）-----
    {
        const QString jar = QStringLiteral("C:/cache/installers/forge-1.20.1-47.4.10-installer.jar");
        const QStringList official = VersionManager::forgeInstallerArguments(
            jar, QStringLiteral("D:/mc"), LoaderMeta::forgeMavenRoot());
        check(official.size() == 6 && official.at(0) == QStringLiteral("-jar") &&
                  official.at(1) == jar && official.at(2) == QStringLiteral("--installClient") &&
                  official.at(3) == QStringLiteral("D:/mc") &&
                  official.at(4) == QStringLiteral("--mirror") &&
                  official.at(5) == LoaderMeta::forgeMavenRoot(),
              "V6c 官方源：--installClient 带着目标目录，--mirror 钉到官方 maven 根（不给它，"
              "安装器会去挑第三方镜像）",
              official.join(QLatin1Char(' ')));
        check(LoaderMeta::forgeMavenRoot().endsWith(QLatin1Char('/')),
              "V6c 官方 maven 根结尾必须有斜杠（安装器直接拼相对路径）",
              LoaderMeta::forgeMavenRoot());

        const QStringList noRoot =
            VersionManager::forgeInstallerArguments(jar, QStringLiteral("D:/mc"), QString());
        check(noRoot.size() == 4 && !noRoot.contains(QStringLiteral("--mirror")),
              "V6c 没有可用根（空串）时不给 --mirror，其余四个参数一字不动",
              noRoot.join(QLatin1Char(' ')));

        const QStringList mirror = VersionManager::forgeInstallerArguments(
            jar, QStringLiteral("D:/mc"), DownloadManager::mirrorMavenRootUrl());
        check(mirror.size() == 6 && mirror.at(4) == QStringLiteral("--mirror") &&
                  mirror.at(5) == DownloadManager::mirrorMavenRootUrl(),
              "V6c 镜像源：--mirror 指向 BMCLAPI 的 /maven/（安装器自己下那十几个库也走它）",
              mirror.join(QLatin1Char(' ')));
        check(mirror.mid(0, 4) == noRoot,
              "V6c 加不加 --mirror，前面那四个参数一字不差");
        check(LoaderMeta::neoForgeMavenRoot() ==
                  QStringLiteral("https://maven.neoforged.net/releases/"),
              "V6c NeoForge 用自己的官方 maven 根（它的库不在 Forge 那个根上）",
              LoaderMeta::neoForgeMavenRoot());
    }

    // --- V6g. 安装器的命令行开关要**先探一次**-----------------
    //
    // 起因是一条真机日志（用户报的那个 1.8.9 安装失败）：安装器被喂了 `--installClient`，
    // 它当场 joptsimple.UnrecognizedOptionException 退出码 1 —— 那个年代的安装器把"装
    // 客户端"做成了图形界面，命令行里根本没有这条路。所以跑安装器之前先跑一次 `--help`
    // 问它。下面两份帮助都是真的：在真安装器 jar 上跑出来的，原样存进 meta/。
    //
    // 实测的边界（每个都下了 4~5 MB 的安装器跑 `--help`）：1.7.10 / 1.8.9（11.15.1.1722
    // 和 11.15.1.2318）/ 1.9.4 / 1.10.2 / 1.11.2 都不认；1.12.2（14.23.5.2859）和 26.2
    // （65.1.0）都认。
    {
        const auto readMetaText = [&metaDir](const QString &name) {
            QFile file(metaDir + QLatin1Char('/') + name);
            if (!file.open(QIODevice::ReadOnly)) {
                return QString();
            }
            return QString::fromUtf8(file.readAll());
        };

        const QString legacyHelp =
            readMetaText(QStringLiteral("installer-help-legacy-1.8.9.txt"));
        check(legacyHelp.contains(QStringLiteral("--extract")) &&
                  legacyHelp.contains(QStringLiteral("--installServer")),
              "V6g 老安装器那份帮助读到了（--extract / --installServer 都在，说明是真东西）",
              QString::number(legacyHelp.size()));
        if (!legacyHelp.isEmpty()) {
            const VersionManager::InstallerOptions legacy =
                VersionManager::parseInstallerHelp(legacyHelp);
            check(legacy.probed, "V6g 探到了就置 probed");
            check(!legacy.supportsInstallClient,
                  "V6g 老安装器不认 --installClient -> 报「这个安装器只有图形界面」"
                  "（以前是把它喂过去，用户看到一段 Java 堆栈）");
            check(!legacy.supportsMirror, "V6g 老安装器也不认 --mirror");
        }

        const QString modernHelp =
            readMetaText(QStringLiteral("installer-help-neoforge-26.2.txt"));
        check(modernHelp.contains(QStringLiteral("--installClient")),
              "V6g 现代安装器那份帮助读到了（--installClient 在）",
              QString::number(modernHelp.size()));
        if (!modernHelp.isEmpty()) {
            const VersionManager::InstallerOptions modern =
                VersionManager::parseInstallerHelp(modernHelp);
            check(modern.supportsInstallClient && modern.supportsMirror,
                  "V6g 现代安装器两个开关都认 -> 这条路一行都没变（26.2 那次真装就是这么跑的）");
        }

        // 探不出来时的默认值必须是"宁可信其有"：探针是为了少一次莫名其妙的报错，
        // 不能反过来变成新的"把本来能装的版本拦下来"的关。
        const VersionManager::InstallerOptions none;
        check(!none.probed && none.supportsInstallClient && none.supportsMirror,
              "V6g 没探到时默认两个都认（起不来 / 超时 / 输出是空的那几支都走这里）");

        // 探到不认 --mirror 就不给：宁可它自己那条下载路慢一点，也不能死在多给一个参数上。
        const QString probeJar =
            QStringLiteral("C:/cache/installers/forge-1.8.9-11.15.1.1722-installer.jar");
        const QStringList noMirror = VersionManager::forgeInstallerArguments(
            probeJar, QStringLiteral("D:/mc"), LoaderMeta::forgeMavenRoot(), false);
        check(noMirror.size() == 4 && !noMirror.contains(QStringLiteral("--mirror")),
              "V6g 探到不认 --mirror -> 那一条就不给（其余四个参数一字不动）",
              noMirror.join(QLatin1Char(' ')));
        const QStringList yesMirror = VersionManager::forgeInstallerArguments(
            probeJar, QStringLiteral("D:/mc"), LoaderMeta::forgeMavenRoot(), true);
        check(yesMirror.size() == 6,
              "V6g 认 --mirror 时照旧给", yesMirror.join(QLatin1Char(' ')));

        check(LoaderMeta::forgeInstallerUrlAlternate(QStringLiteral("1.8.9"),
                                                     QStringLiteral("11.15.1.2318")) ==
                  QStringLiteral("https://maven.minecraftforge.net/net/minecraftforge/forge/"
                                 "1.8.9-11.15.1.2318-1.8.9/"
                                 "forge-1.8.9-11.15.1.2318-1.8.9-installer.jar"),
              "V6g 第二种坐标是 <游戏>-<加载器>-<游戏>（文件名里也是三段）",
              LoaderMeta::forgeInstallerUrlAlternate(QStringLiteral("1.8.9"),
                                                     QStringLiteral("11.15.1.2318")));
        check(LoaderMeta::forgeInstallerUrlAlternate(QStringLiteral("1.8.9"),
                                                     QStringLiteral("11.15.1.2318")) !=
                  LoaderMeta::forgeInstallerUrl(QStringLiteral("1.8.9"),
                                                QStringLiteral("11.15.1.2318")),
              "V6g 两种坐标确实是两个地址（不一样才值得再下一遍）"
              " —— 也正是 V6d 那条 shouldRecordOfficialFallback 的判据");
    }

    // --- V6i. 老 Forge：照它自己那份安装说明装-----------------
    //
    // 起因是用户那句话：「forge 在新电脑上完全装不了难道不算 bug」—— 1.7.10 / 1.8.9 /
    // 1.9.4 / 1.10.2 / 1.11.2 和 1.12.2 的**推荐版（2847）**这些安装器都没有
    // `--installClient`（见 V6g 那段实测边界），而安装页默认挑的正是"稳定版"那一颗。
    // 照它 jar 里那份 install_profile.json 装，不用它的界面、也不用起 JVM。
    //
    // 三份 profile 都是真数据：从真的安装器 jar 里抽出来的原样（见文件头那份清单）。
    // 原版那一份用 meta/1.12.2.json（V6h 那一节已经在用它了）。
    {
        // 按名字把库表摊成一张表，下面按名字问。
        const auto librariesByName = [](const QJsonObject &json) {
            QHash<QString, QJsonObject> out;
            for (const QJsonValue &value : json.value(QStringLiteral("libraries")).toArray()) {
                const QJsonObject libObj = value.toObject();
                out.insert(libObj.value(QStringLiteral("name")).toString(), libObj);
            }
            return out;
        };

        const QJsonObject vanilla122 = loadJson(metaDir + QStringLiteral("/1.12.2.json"));
        check(!vanilla122.isEmpty(), "V6i 原版 1.12.2 那份 fixture 读到了");

        const QString profile2847Path =
            metaDir + QStringLiteral("/forge-install-profile-1.12.2-14.23.5.2847.json");
        QFile profile2847File(profile2847Path);
        const bool profile2847Open = profile2847File.open(QIODevice::ReadOnly);
        const QByteArray profile2847 = profile2847Open ? profile2847File.readAll() : QByteArray();
        check(profile2847Open && profile2847.size() > 4000,
              "V6i 2847 那份安装说明读到了（4.6 MB 的安装器里那份 5878 B 的原样）",
              QString::number(profile2847.size()));

        const QString ourId = QStringLiteral("1.12.2-forge-14.23.5.2847");
        const LegacyForgeInstall::Plan plan =
            LegacyForgeInstall::planFromInstallProfile(profile2847, vanilla122, ourId);
        check(plan.ok, "V6i 老格式（install + versionInfo）这份认得出来");
        if (plan.ok) {
            // 落盘那份 json 的四条"少了任何一条都启动不了"的字段。id 必须是**我们的**：
            // 上游写的是 `1.12.2-forge1.12.2-14.23.5.2847`（多一截、没连字符），它要是飘进
            // 目录名 / wtfml.meta / 版本列表，这个版本的显示名就和别的加载器长得不一样。
            check(plan.inheritsFrom == QStringLiteral("1.12.2"),
                  "V6i 它的父版本是 1.12.2（走的是 versionInfo.inheritsFrom）", plan.inheritsFrom);
            check(plan.versionJson.value(QStringLiteral("id")).toString() == ourId,
                  "V6i 落盘那份的 id 是**我们的**（上游那个 1.12.2-forge1.12.2-… 不许进去）",
                  plan.versionJson.value(QStringLiteral("id")).toString());
            check(plan.versionJson.value(QStringLiteral("jar")).toString() ==
                      QStringLiteral("1.12.2"),
                  "V6i jar 字段指原版那一份 ⇒ 不用往版本目录里再抄一份客户端 jar"
                  "（安装器那条路也不抄：1.12.2 的 jar 和原版逐字节一样）");
            check(plan.versionJson.value(QStringLiteral("mainClass")).toString() ==
                      QStringLiteral("net.minecraft.launchwrapper.Launch"),
                  "V6i 主类照它给的来（Forge 自己那份 launchwrapper，不是原版那个）");
            check(!plan.versionJson.value(QStringLiteral("minecraftArguments")).toString().isEmpty(),
                  "V6i 游戏参数那一串也是它给的（比如 --tweakClass，自己拼不出来）");

            // 两颗路径必须一致：一个是"jar 摆到哪"，一个是"classpath 从哪挂"。
            check(plan.universalEntry ==
                      QStringLiteral("forge-1.12.2-14.23.5.2847-universal.jar"),
                  "V6i universal jar 的条目名 = 它 install.filePath 那一句", plan.universalEntry);
            check(plan.universalLibraryPath ==
                      QStringLiteral("net/minecraftforge/forge/1.12.2-14.23.5.2847/"
                                     "forge-1.12.2-14.23.5.2847.jar"),
                  "V6i 它该落在 libraries/ 下**普通名字**那个位置（本机装好的 2860 / 1.7.10 "
                  "都是这个形状：仓库上普通名字 404、-universal.jar 才是 200）",
                  plan.universalLibraryPath);

            const QHash<QString, QJsonObject> libs = librariesByName(plan.versionJson);
            check(libs.size() == 19,
                  "V6i 21 条库丢掉原版已经带着的那 2 条，其余 19 条都在",
                  QString::number(libs.size()));
            check(plan.droppedFromParent == 2 && !libs.contains(QStringLiteral("net.java.dev.jna"
                                                                               ":jna:4.4.0")) &&
                      !libs.contains(QStringLiteral("net.sf.jopt-simple:jopt-simple:5.0.3")),
                  "V6i 丢的正好是原版 1.12.2 自己带着的那两颗（jna 4.4.0 / jopt-simple "
                  "5.0.3）—— 留着就是拿我们猜的地址下重一遍",
                  QString::number(plan.droppedFromParent));

            // 这一条是整条路的地基：它自己那颗库**不能带 url**。
            const LibraryRules::Artifact universal = LibraryRules::artifactFor(
                libs.value(QStringLiteral("net.minecraftforge:forge:1.12.2-14.23.5.2847")));
            check(universal.localOnly && universal.hasPath() && !universal.valid(),
                  "V6i 它自己那颗是「只有 name、没有 url」的写法（LibraryRules 的第 3 种）"
                  "⇒ 不去下（普通名字 404），但**必须**进 classpath —— 少了它，装完启动时 "
                  "NoClassDefFoundError: net.minecraft.launchwrapper.Launch",
                  universal.path);
            check(universal.path == plan.universalLibraryPath,
                  "V6i 「摆到哪」和「classpath 挂哪」是同一处算出来的", universal.path);

            check(plan.assumedMojang == 4,
                  "V6i 没有 url 而原版也不带的正好四颗（launchwrapper / lzma / vecmath / "
                  "trove4j）",
                  QString::number(plan.assumedMojang));
            const LibraryRules::Artifact launchwrapper = LibraryRules::artifactFor(
                libs.value(QStringLiteral("net.minecraft:launchwrapper:1.12")));
            check(launchwrapper.valid() &&
                      launchwrapper.url ==
                          LegacyForgeInstall::mojangLibraryBase() +
                              QStringLiteral("net/minecraft/launchwrapper/1.12/"
                                             "launchwrapper-1.12.jar"),
                  "V6i 那四颗按 maven 规矩拼到 Mojang 那份仓库上（forge maven 上没有它）",
                  launchwrapper.url);

            // 有 url 的照它给的（别自作聪明换仓库）。这两颗 scala-continuations 是**唯一**
            // 容易被"补上 _mc 后缀"的那一对 —— 实测普通 1.0.2 在 forge maven 上就是 200，
            // 加了 _mc 反而多一个不再需要的假设。
            const LibraryRules::Artifact scalaContinuations = LibraryRules::artifactFor(
                libs.value(QStringLiteral(
                    "org.scala-lang.plugins:scala-continuations-library_2.11:1.0.2")));
            check(scalaContinuations.valid() &&
                      scalaContinuations.url ==
                          QStringLiteral("https://maven.minecraftforge.net/org/scala-lang/plugins/"
                                         "scala-continuations-library_2.11/1.0.2/"
                                         "scala-continuations-library_2.11-1.0.2.jar"),
                  "V6i 有 url 的库原样照它给的 url（不加 _mc 后缀也一样能下：回 200）",
                  scalaContinuations.url);

            // 一条都不许静默消失：每条都得拼得出 libraries/ 下的路径（拼不出来的那种，
            // 症状是游戏起来时一个 NoClassDefFoundError，而安装界面一路绿灯）。
            int unresolved = 0;
            for (auto it = libs.constBegin(); it != libs.constEnd(); ++it) {
                if (!LibraryRules::artifactFor(it.value()).hasPath()) {
                    ++unresolved;
                }
            }
            check(unresolved == 0, "V6i 19 条库一条都没丢（每条都解析出了路径）",
                  QString::number(unresolved));
        }

        QFile profile1710File(metaDir +
                              QStringLiteral("/forge-install-profile-1.7.10-10.13.4.1614-1.7.10.json"));
        const bool profile1710Open = profile1710File.open(QIODevice::ReadOnly);
        const QByteArray profile1710 = profile1710Open ? profile1710File.readAll() : QByteArray();
        check(profile1710.size() > 4000, "V6i 1.7.10 那份安装说明读到了",
              QString::number(profile1710.size()));
        const QString id1710 = QStringLiteral("1.7.10-forge-10.13.4.1614-1.7.10");
        const LegacyForgeInstall::Plan plan1710 =
            LegacyForgeInstall::planFromInstallProfile(profile1710, QJsonObject(), id1710);
        check(plan1710.ok, "V6i 1.7.10 那份（同一个老格式）也认得出来");
        if (plan1710.ok) {
            check(plan1710.inheritsFrom == QStringLiteral("1.7.10") && plan1710.droppedFromParent == 0,
                  "V6i 原版那份拿不到时一条都不丢（继承链那边管不着，靠 Mojang 兜底）",
                  QString::number(plan1710.droppedFromParent));
            check(plan1710.universalLibraryPath ==
                      QStringLiteral("net/minecraftforge/forge/1.7.10-10.13.4.1614-1.7.10/"
                                     "forge-1.7.10-10.13.4.1614-1.7.10.jar"),
                  "V6i 1.7.10 的 universal jar 落在同一个形状上（本机 PCL 装的那份就在这个"
                  "路径上）",
                  plan1710.universalLibraryPath);
            check(plan1710.versionJson.value(QStringLiteral("id")).toString() == id1710 &&
                      plan1710.versionJson.value(QStringLiteral("mainClass")).toString() ==
                          QStringLiteral("net.minecraft.launchwrapper.Launch"),
                  "V6i 1.7.10 那份的 id 也是我们的、主类也是它给的");
        }

        // 反面判据：认不出来的那几种，必须**明确不 ok**（调用方据此回退到"请你用别的启动器
        // 装一次"那句话），而不是写出一份半截 json 落盘。
        {
            QFile modernFile(metaDir + QStringLiteral("/forge-install-profile-1.12.2-14.23.5.2860"
                                                      ".json"));
            const bool modernOpen = modernFile.open(QIODevice::ReadOnly);
            const QByteArray modern = modernOpen ? modernFile.readAll() : QByteArray();
            check(modern.size() > 4000, "V6i 2860 那份（新格式）安装说明读到了",
                  QString::number(modern.size()));
            const LegacyForgeInstall::Plan modernPlan =
                LegacyForgeInstall::planFromInstallProfile(modern, vanilla122, ourId);
            check(!modernPlan.ok &&
                      modernPlan.problem == LegacyForgeInstall::Problem::ModernProfile,
                  "V6i 新格式那份（spec/profile/processors，认 --installClient）明确不 ok —— "
                  "那条路本来就走安装器，不该照这份自己装");
        }
        {
            const LegacyForgeInstall::Plan notJson =
                LegacyForgeInstall::planFromInstallProfile(QByteArray("not json at all"),
                                                           vanilla122, ourId);
            check(!notJson.ok && notJson.problem == LegacyForgeInstall::Problem::NotJson,
                  "V6i 那个条目不是 json -> 明确报出来（半截 json 不许落盘）");
        }
        {
            // 真数据上抠掉一个字段，看它认不认得出缺的是哪一节（两份 profile 都是真数据 +
            // 一处定向改动，不是自造的样例）。
            QJsonObject broken = QJsonDocument::fromJson(profile2847).object();
            QJsonObject install = broken.value(QStringLiteral("install")).toObject();
            install.remove(QStringLiteral("filePath"));
            broken[QStringLiteral("install")] = install;
            const LegacyForgeInstall::Plan missing = LegacyForgeInstall::planFromInstallProfile(
                QJsonDocument(broken).toJson(QJsonDocument::Compact), vanilla122, ourId);
            check(!missing.ok && missing.problem == LegacyForgeInstall::Problem::MissingInstall &&
                      missing.detail == QStringLiteral("filePath"),
                  "V6i install 里缺 filePath -> 说得出缺的是哪一项", missing.detail);
        }
        {
            QJsonObject broken = QJsonDocument::fromJson(profile2847).object();
            QJsonObject install = broken.value(QStringLiteral("install")).toObject();
            install[QStringLiteral("path")] = QStringLiteral("net.minecraftforge:forge:..");
            broken[QStringLiteral("install")] = install;
            const LegacyForgeInstall::Plan unsafe = LegacyForgeInstall::planFromInstallProfile(
                QJsonDocument(broken).toJson(QJsonDocument::Compact), vanilla122, ourId);
            check(!unsafe.ok && unsafe.problem == LegacyForgeInstall::Problem::UnknownPath,
                  "V6i 坐标里有 `..`（拼出来会写到 libraries/ 外面）-> 拒绝，不猜路径",
                  unsafe.detail);
        }
        {
            QJsonObject broken = QJsonDocument::fromJson(profile2847).object();
            QJsonObject vi = broken.value(QStringLiteral("versionInfo")).toObject();
            vi.remove(QStringLiteral("inheritsFrom"));
            QJsonObject install = broken.value(QStringLiteral("install")).toObject();
            install.remove(QStringLiteral("minecraft"));
            broken[QStringLiteral("versionInfo")] = vi;
            broken[QStringLiteral("install")] = install;
            const LegacyForgeInstall::Plan orphan = LegacyForgeInstall::planFromInstallProfile(
                QJsonDocument(broken).toJson(QJsonDocument::Compact), vanilla122, ourId);
            check(!orphan.ok && orphan.problem == LegacyForgeInstall::Problem::NoParent,
                  "V6i 两个字段都认不出父版本 -> 拒绝（否则写出来的 json 指向一个没下过的原版）");
        }
        {
            QJsonObject keep = QJsonDocument::fromJson(profile2847).object();
            const LegacyForgeInstall::Plan keepPlan = LegacyForgeInstall::planFromInstallProfile(
                QJsonDocument(keep).toJson(QJsonDocument::Compact), vanilla122, ourId);
            const QHash<QString, QJsonObject> keepLibs = librariesByName(keepPlan.versionJson);
            const LibraryRules::Artifact jline =
                LibraryRules::artifactFor(keepLibs.value(QStringLiteral("org.jline:jline:3.5.1")));
            check(keepPlan.ok && jline.valid() &&
                      jline.url == QStringLiteral("https://maven.minecraftforge.net/org/jline/jline/"
                                                  "3.5.1/jline-3.5.1.jar"),
                  "V6i clientreq: false 的库**照留**（真装出来的客户端 classpath 上有 jline）",
                  jline.url);
        }
    }

    // --- V6h. version.json 自己声明的游戏参数------------------
    //
    // NeoForge 26.2 的版本 json 在 arguments.game 里写着
    // `--fml.neoForgeVersion 26.2.0.88 --fml.mcVersion 26.2 --fml.neoFormVersion 2`，启动器
    // 必须把它展开：只展开 arguments.jvm 时 FML 报 "--fml.mcVersion must be present as a
    // command-line argument"、接着 VersionSupportMatrix 拿着空版本号 NPE，游戏在加载窗口
    // 那一步就没了。
    //
    // 两份都是真数据：meta/neoforge-26.2.0.88.json 是那个安装器写出来的 profile 原样，
    // meta/26.2.json 是原版那份。
    //
    // NOTE: 判据必须是**继承链合并过**的那份 json —— 真实启动时 generateJvmArguments 拿到的
    // 就是它。判据设在 `inheritsFrom` 上不行：merge 刻意丢掉了那个字段，单测喂原样
    // profile 会全绿、真机照旧崩。所以下面先合并再喂。
    {
        const QJsonObject neoforgeJson = loadJson(metaDir + "/neoforge-26.2.0.88.json");
        const QJsonObject vanillaJson = loadJson(metaDir + "/26.2.json");
        check(!neoforgeJson.isEmpty() && !vanillaJson.isEmpty(),
              "V6h 两份真数据都读到了");
        if (!neoforgeJson.isEmpty() && !vanillaJson.isEmpty()) {
            QHash<QString, QString> placeholders;
            placeholders[QStringLiteral("auth_player_name")] = QStringLiteral("Steve");
            placeholders[QStringLiteral("version_name")] = QStringLiteral("neoforge-26.2.0.88");
            placeholders[QStringLiteral("game_directory")] =
                QStringLiteral("D:/mc/versions/neoforge-26.2.0.88");
            placeholders[QStringLiteral("assets_root")] = QStringLiteral("D:/mc/assets");
            placeholders[QStringLiteral("assets_index_name")] = QStringLiteral("29");
            placeholders[QStringLiteral("auth_uuid")] = QStringLiteral("uuid-1");
            placeholders[QStringLiteral("auth_access_token")] = QStringLiteral("tok");
            placeholders[QStringLiteral("user_type")] = QStringLiteral("msa");
            placeholders[QStringLiteral("version_type")] = QStringLiteral("release");

            const QJsonObject neoforgeMerged =
                VersionInheritance::merge(neoforgeJson, vanillaJson);
            check(neoforgeMerged.value(QStringLiteral("inheritsFrom")).toString().isEmpty() &&
                      !neoforgeMerged.value(QStringLiteral("mainClass")).toString().isEmpty() &&
                      !neoforgeMerged.value(QStringLiteral("libraries")).toArray().isEmpty(),
                  "V6h 合并后的 json 是个完整的版本（没有 inheritsFrom、有 mainClass 和库）"
                  "—— 启动时喂进 generateJvmArguments 的就是这一份",
                  neoforgeMerged.value(QStringLiteral("mainClass")).toString());
            const QStringList extra = VersionArguments::extraGameArguments(
                neoforgeMerged, QStringList(), placeholders);
            check(extra == QStringList{QStringLiteral("--fml.neoForgeVersion"),
                                       QStringLiteral("26.2.0.88"),
                                       QStringLiteral("--fml.mcVersion"),
                                       QStringLiteral("26.2"),
                                       QStringLiteral("--fml.neoFormVersion"),
                                       QStringLiteral("2")},
                  "V6h NeoForge 那三条 --fml.* 一条不少地带上（少了 --fml.mcVersion 就是那次"
                  "崩溃的第一句话）",
                  extra.join(QLatin1Char(' ')));

            // 已经在命令行里的选项要跳过（连同它的值），否则同一个选项发两遍。
            const QStringList deduped = VersionArguments::extraGameArguments(
                neoforgeMerged, QStringList{QStringLiteral("--fml.mcVersion")}, placeholders);
            check(deduped == QStringList{QStringLiteral("--fml.neoForgeVersion"),
                                         QStringLiteral("26.2.0.88"),
                                         QStringLiteral("--fml.neoFormVersion"),
                                         QStringLiteral("2")},
                  "V6h 已经发过的选项连它的值一起跳过", deduped.join(QLatin1Char(' ')));

            // 纯原版那条路必须**逐字节不变**（原版没有父版本，喂进来的就是 json 自己那份）：
            // 判据是它 arguments.game 里全是原版那套名字，而那一套启动器上面已经发过了。
            check(VersionArguments::extraGameArguments(vanillaJson, QStringList(), placeholders)
                      .isEmpty(),
                  "V6h 纯原版（arguments.game 那代）一条都不加",
                  QString::number(
                      vanillaJson[QStringLiteral("arguments")].toObject()[QStringLiteral("game")]
                          .toArray()
                          .size()));
            // 老格式那份也一样：1.12.2 的原版 json 用 minecraftArguments 写同一套名字。
            const QJsonObject vanillaLegacy = loadJson(metaDir + "/1.12.2.json");
            check(!vanillaLegacy.value(QStringLiteral("minecraftArguments")).toString().isEmpty(),
                  "V6h 1.12.2 那份原版确实是老格式（minecraftArguments）—— 老格式这条路也验到了");
            check(VersionArguments::extraGameArguments(vanillaLegacy, QStringList(), placeholders)
                      .isEmpty(),
                  "V6h 纯原版（minecraftArguments 那代）也一条都不加");
            // 占位符警告（ 修的那条）：extraGameArguments 拿到的这一份里，
            // 绝大多数条目**下一句就会被整条跳掉**（原版那套名字启动器上面已经发过了），
            // 而它们身上的 ${clientid} / ${auth_xuid} / ${user_properties} … 我们本来就
            // 不提供 —— 所以"替换"必须排到过滤**之后**去做，否则每一条都会甩一句
            // "Unresolved placeholder"（实测 1.18.2 / 1.20.1 各 11 条、1.21.11+ 有 10 条），
            // 而那几行**一个字节都不会进命令行**。
            //
            // 原来那一版传的是 `expand(game, 空映射, ...)` —— **空映射不等于"不替换"**，
            // substitutePlaceholders() 是无条件调的，警告照发，注释逐字承诺的事一件没做到。
            //
            // 这里量两件事：真夹具过一遍**一条警告都没有**；**并且先自检拦法通不通** ——
            // 故意拿一个没人填的占位符调一次，计数必须动一下，否则"0 条"可能只是这个处理器
            // 压根收不到东西（那种假绿正是这个坑第一次溜过去的原因）。
            {
                const QtMessageHandler previous = qInstallMessageHandler(countPlaceholderWarnings);
                g_placeholderWarnings = 0;
                const QStringList vanillaArgs =
                    VersionArguments::extraGameArguments(vanillaJson, QStringList(), placeholders);
                const QStringList legacyArgs = VersionArguments::extraGameArguments(
                    vanillaLegacy, QStringList(), placeholders);
                const int fromRealJsons = g_placeholderWarnings;
                VersionArguments::substitutePlaceholders(QStringLiteral("--clientId ${clientid}"),
                                                         QHash<QString, QString>());
                const int afterProbe = g_placeholderWarnings;
                qInstallMessageHandler(previous);
                check(fromRealJsons == 0,
                      "V6h 两份真原版 json 过一遍 extraGameArguments()：一句占位符警告都没有"
                      "（替换排在过滤之后 —— 原来这里每次启动刷 10~11 条）",
                      QStringLiteral("%1 条；两次都回空表（%2 / %3 项）")
                          .arg(fromRealJsons)
                          .arg(vanillaArgs.size())
                          .arg(legacyArgs.size()));
                check(afterProbe == fromRealJsons + 1,
                      "V6h 自检：这个计数器确实数得到那条警告（不然上面那条是白绿）",
                      QStringLiteral("%1 → %2").arg(fromRealJsons).arg(afterProbe));
            }
            // Fabric 的 profile 自己不带游戏参数，合并之后 arguments.game 里只剩原版那一套 ——
            // 生产那条路喂给它的正是合并后的这份。
            const QJsonObject fabricProfile =
                loadJson(metaDir + "/fabric-loader-0.19.5-1.19.json");
            const QJsonObject fabricVanilla = loadJson(metaDir + "/1.19.json");
            check(!fabricProfile.isEmpty() && !fabricVanilla.isEmpty(),
                  "V6h Fabric + 它依赖的那份原版都读到了");
            check(VersionArguments::extraGameArguments(
                      VersionInheritance::merge(fabricProfile, fabricVanilla), QStringList(),
                      placeholders)
                          .isEmpty(),
                  "V6h Fabric 合并之后也不加（它的东西都在 arguments.jvm 里）");

            // 规则对象照旧复用 LibraryRules：命中的留下、不命中的整条丢掉。
            QJsonObject ruled;
            ruled[QStringLiteral("inheritsFrom")] = QStringLiteral("1.26");
            QJsonObject ruledArguments;
            QJsonArray ruledGame;
            QJsonObject linuxEntry;
            linuxEntry[QStringLiteral("rules")] =
                QJsonArray{QJsonObject{{QStringLiteral("action"), QStringLiteral("allow")},
                                       {QStringLiteral("os"),
                                        QJsonObject{{QStringLiteral("name"),
                                                     QStringLiteral("linux")}}}}};
            linuxEntry[QStringLiteral("value")] = QStringLiteral("--fml.linuxOnly");
            QJsonObject windowsEntry;
            windowsEntry[QStringLiteral("rules")] =
                QJsonArray{QJsonObject{{QStringLiteral("action"), QStringLiteral("allow")},
                                       {QStringLiteral("os"),
                                        QJsonObject{{QStringLiteral("name"),
                                                     QStringLiteral("windows")}}}}};
            windowsEntry[QStringLiteral("value")] = QStringLiteral("--launchTarget");
            ruledGame.append(linuxEntry);
            ruledGame.append(windowsEntry);
            ruledGame.append(QStringLiteral("forgeclient"));
            ruledArguments[QStringLiteral("game")] = ruledGame;
            ruled[QStringLiteral("arguments")] = ruledArguments;
            const QStringList ruledOut =
                VersionArguments::extraGameArguments(ruled, QStringList(), placeholders);
            check(ruledOut == QStringList{QStringLiteral("--launchTarget"),
                                          QStringLiteral("forgeclient")},
                  "V6h 规则命中的留下、不命中的丢掉（复用 LibraryRules，没有第二份规则逻辑）",
                  ruledOut.join(QLatin1Char(' ')));

            // 老格式（1.12.2 那一代 Forge 的 minecraftArguments）：原版那套名字一个都不重复，
            // 加载器自己的 --tweakClass 得带上。
            QJsonObject legacyForge;
            legacyForge[QStringLiteral("inheritsFrom")] = QStringLiteral("1.12.2");
            legacyForge[QStringLiteral("minecraftArguments")] =
                QStringLiteral("--username ${auth_player_name} --version ${version_name} "
                               "--gameDir ${game_directory} --assetsDir ${assets_root} "
                               "--assetIndex ${assets_index_name} --uuid ${auth_uuid} "
                               "--accessToken ${auth_access_token} --userType ${user_type} "
                               "--tweakClass net.minecraftforge.fml.common.launcher.FMLTweaker");
            const QStringList legacyOut = VersionArguments::extraGameArguments(
                VersionInheritance::merge(legacyForge, vanillaLegacy), QStringList(),
                placeholders);
            check(legacyOut ==
                      QStringList{QStringLiteral("--tweakClass"),
                                  QStringLiteral("net.minecraftforge.fml.common.launcher.FMLTweaker")},
                  "V6h 老格式的 minecraftArguments 也吃：原版那套名字全跳过，--tweakClass 带上",
                  legacyOut.join(QLatin1Char(' ')));
        }
    }

    // --- V6d. 镜像失败时"退一步走官方"的两条边界----------------
    //
    // 兜底要覆盖**所有**走镜像的下载，不只是资源索引（镜像那条 /indexes/<id>.json 全
    // 404）。实测：索引下成之后，失败变成 38 个资源对象 —— 镜像不是 404，是 302 跳到
    // openbmclapi 的兄弟节点、慢到 30 秒都拿不全（官方同一时刻 1 秒就给）。
    {
        check(!DownloadManager::shouldRecordOfficialFallback(QStringLiteral("https://a/x"), QString()),
              "V6d 调用方没给官方地址 -> 不登记（没有第二条路）");
        check(!DownloadManager::shouldRecordOfficialFallback(QStringLiteral("https://a/x"),
                                            QStringLiteral("https://a/x")),
              "V6d 两个地址一样 -> 不登记（否则等于原样再发一次，白等一个超时）");
        check(DownloadManager::shouldRecordOfficialFallback(
                  DownloadManager::mirrorUrl(
                      QStringLiteral("https://resources.download.minecraft.net/0a/0a1234")),
                  QStringLiteral("https://resources.download.minecraft.net/0a/0a1234")),
              "V6d 镜像地址 + 官方地址（资源对象就是这么登记的）-> 登记",
              DownloadManager::mirrorUrl(
                  QStringLiteral("https://resources.download.minecraft.net/0a/0a1234")));
        check(DownloadManager::shouldRecordOfficialFallback(
                  DownloadManager::mirrorClientJarUrl(QStringLiteral("1.18.2")),
                  QStringLiteral("https://piston-data.mojang.com/v1/objects/xx/client.jar")),
              "V6d 客户端 jar：镜像那条是按版本号拼的，跟官方那条完全不像 -> 登记");
    }

    // --- V6e. 镜像"太慢就先不用"的判据---------------------------
    //
    // 兜底只认"失败"，可镜像这条链子**不是失败**：它是 302 跳到一条只有几百字节每秒的
    // 链子上、细水长流地掉字节 —— Qt 的 setTransferTimeout() 是"不活动超时"，于是既不会
    // 失败（等不到兜底）也不会完成，整条安装就那么挂着（gdb 里主线程空转在事件循环里，
    // 三个 socket 挂着、一个文件不落）。处置是给"走了镜像、而且知道官方那条地址"的回复
    // 加一条失速判据 + 一段降级期。
    //
    // 这几条钉的是判据本身（纯函数 + 公开的两个开关），"真慢到触发"那一半只能靠 live
    // 那一跑 —— 本地造不出一条慢慢掉字节的服务器。
    {
        const qint64 kMinBytes = 64 * 1024;

        check(!DownloadManager::mirrorDemotedAt(1000, 0),
              "V6e 从来没降级过（期限 0）-> 不在降级期里");
        check(DownloadManager::mirrorDemotedAt(1000, 2000),
              "V6e 现在还没到期限 -> 在降级期里");
        check(!DownloadManager::mirrorDemotedAt(2000, 2000),
              "V6e 正好到期限那一刻 -> 已经不再是降级期（边界取开）");
        check(!DownloadManager::mirrorDemotedAt(2001, 2000),
              "V6e 期限过了 -> 镜像又能用了");

        check(DownloadManager::mirrorStalled(0, kMinBytes),
              "V6e 一跳一个字节都没进 -> 算没动（连接挂死就是这种）");
        check(DownloadManager::mirrorStalled(kMinBytes - 1, kMinBytes),
              "V6e 差一个字节到下限 -> 算没动");
        check(!DownloadManager::mirrorStalled(kMinBytes, kMinBytes),
              "V6e 正好到下限 -> 不算没动（判据是「不到下限」）");
        check(!DownloadManager::mirrorStalled(400 * 1024, kMinBytes),
              "V6e 一跳 400 KB（官方那条版本清单的水平）-> 不算没动");
        check(DownloadManager::mirrorStalled(-5, kMinBytes),
              "V6e 计数变小（跳转会让 downloadProgress 从头报）-> 按「没动」算，"
              "最多误记一跳，下一跳真在动就会把连击清零");

        // 加的那一道闸（理由和实测见头文件 countsTowardStallStrike()）：
        // 一跳的进账不到下限**还不够**记 strike，这条回复必须至少收到过一个字节。
        // 上面那条 mirrorStalled(0, ...) 仍然成立 —— 变的不是"0 算不算没动"，
        // 变的是"一条还没开始传的回复该不该被这条判据管"。
        check(!DownloadManager::countsTowardStallStrike(0, 0, kMinBytes),
              "V6e 一个字节都没收到、进账 0 -> **不算**失速（它不是慢，是还没轮到它）");
        check(!DownloadManager::countsTowardStallStrike(0, 10 * 1024 * 1024, kMinBytes),
              "V6e 闸门只看「收到过字节没有」：没收到过的话，进账那一路怎么算都不记 strike");
        check(DownloadManager::countsTowardStallStrike(1, 0, kMinBytes),
              "V6e 收到过 1 个字节之后停住 -> 算失速（细水长流那条链子的真形状）");
        check(DownloadManager::countsTowardStallStrike(1, kMinBytes - 1, kMinBytes),
              "V6e 收到过字节、这一跳差一个字节到下限 -> 算失速");
        check(!DownloadManager::countsTowardStallStrike(1, kMinBytes, kMinBytes),
              "V6e 收到过字节、这一跳到下限 -> 不算失速（闸门不是在 mirrorStalled 上再收紧一次）");
        check(DownloadManager::countsTowardStallStrike(1024, -5, kMinBytes),
              "V6e 收到过字节、计数变小 -> 照样算失速");

        check(DownloadManager::demoteDeadline(400000, 100000) == 400000 +
                  DownloadManager::kMirrorDemoteMs,
              "V6e 上一次降级早就过期了 -> 从现在起重开一段");
        check(DownloadManager::demoteDeadline(1000, 500000) == 500000,
              "V6e 上一次降级还没过期 -> **顺延**（取晚的那个），不会把期限提前");

        // 真对象上的两种状态：降级只影响"往哪走"，不改用户的设置。
        DownloadManager dm;
        const QString assetUrl = QStringLiteral(
            "https://resources.download.minecraft.net/0a/0a1234");
        dm.setUseMirror(true);
        check(dm.mirrorPreferred(), "V6e 选了镜像源、又没降级 -> 走镜像");
        check(dm.endpoint(assetUrl) != assetUrl && dm.endpoint(assetUrl).contains(
                  QStringLiteral("bmclapi")),
              "V6e 这时候 endpoint() 给的是镜像那条", dm.endpoint(assetUrl));
        dm.demoteMirrorForSlowDownload(QStringLiteral("bmclapi2.bangbang93.com"));
        check(dm.isMirrorDemoted(), "V6e 降级镜像源那个主机 -> 就在降级期里");
        check(!dm.mirrorPreferred(), "V6e 降级期里**不再**走镜像");
        check(dm.endpoint(assetUrl) == assetUrl,
              "V6e 降级期里 endpoint() 给回官方那条（只换地址，后面那套记账不用改）",
              dm.endpoint(assetUrl));
        check(dm.useMirror(),
              "V6e 降级**没有**把用户的「下载源」设置改掉（设置里还是镜像源）");
        dm.setUseMirror(false);
        check(dm.endpoint(assetUrl) == assetUrl, "V6e 用户改回官方源 -> 当然还是官方那条");

        // 降级**按主机**：整合包那条路上"镜像"是每条文件各自的首选地址
        // （Modrinth 清单里给 2~3 条，preferredUrlOrder() 把 cdn.modrinth.com 排前面），
        // 一个全局的降级位会让一个慢 CDN 把别的 CDN 一起关掉：mediafilez 上 8 条不动
        // -> 全体降级 -> 89% 的字节（1405.9 MB）被赶到慢 CDN 上。
        {
            DownloadManager perHost;
            perHost.setUseMirror(true);
            const QString slowCdn = QStringLiteral("mediafilez.forgecdn.net");
            const QString fastCdn = QStringLiteral("cdn.modrinth.com");
            perHost.demoteMirrorForSlowDownload(slowCdn);
            check(perHost.mirrorDemotedFor(slowCdn), "V6e 按主机降级：那条慢 CDN 确实在降级期里");
            check(!perHost.mirrorDemotedFor(fastCdn),
                  "V6e 按主机降级：**别的 CDN 一点没受影响**（这次事故的判据就是这一条）");
            check(!perHost.mirrorDemotedFor(QString()), "V6e 空主机名 -> 按没降级算");
            check(perHost.mirrorDemotedFor(QStringLiteral("MEDIAFILEZ.forgecdn.net")) == false,
                  "V6e 主机名是区分大小写的串比较（QUrl::host() 给的一律小写，这一条只是钉住"
                  "「不做事先转换」）");
            check(perHost.isMirrorDemoted() == false,
                  "V6e 被降级的那条不是镜像源那个主机 -> 镜像源这一侧照走");
            check(perHost.mirrorPreferred(), "V6e 镜像源没被降级 -> mirrorPreferred() 仍然为真");
            // 顺延是**按主机**各自算的：再降一次慢 CDN 不影响快 CDN 的（本来就没有的）期限。
            perHost.demoteMirrorForSlowDownload(fastCdn);
            check(perHost.mirrorDemotedFor(fastCdn), "V6e 再降另一个主机 -> 各记各的");
            check(perHost.mirrorDemotedFor(slowCdn), "V6e 后降的那个不覆盖先降的那个");
        }
    }

    // --- V6f. 看门狗端到端：挂着不动的那条真的会被掐掉、换官方那条（本地假镜像）------
    //
    // V6e 钉的是判据，这一节把整条链子跑一遍。两个假镜像都给 HTTP 头、body 只给一点点
    // 就不给了，它们对应真慢链子的两种形状：
    // (a) 细水长流：吐了一小口（够不着"一跳 64 KB"那条下限）就停住 —— 镜像那条 302 到
    // MinIO 的链子就是这个形状（实测 0.85~10 KB/s），**这一种该被看门狗在 10 秒判掉**；
    // (b) 一个字节都不吐。这一种**不记失速 strike**（它可能只是还在建连 / 排在一条 h2
    // 连接的队列里； 的判据，理由见 countsTowardStallStrike()）。
    // 起它由 (0b) 那条「12 秒零字节就换路」收尾（理由见下面 (b) 那段注释），照样走
    // "退回官方、不算失败"那条路。
    // "官方那条"给成 file://（一个真文件），所以这两条用例**都不碰外网**，在哪台机器上跑
    // 都是同一个结果；顺带证明了 abort / 超时之后那次重发真的走通了。
    // 代价：十来秒（看门狗两跳）+ 十五秒（(0b) 那条），是这套里仅有的按秒计的用例。
    {
        QTemporaryDir scratch(QDir::tempPath() + "/wtfml_stallwatch-XXXXXX");
        check(scratch.isValid(), "V6f 临时目录建出来了");
        QTcpServer fakeMirror;
        const bool listening = fakeMirror.listen(QHostAddress::LocalHost, 0);
        check(listening, "V6f 本地假镜像起来了（起不来这条用例就没意义，直接算失败）");
        if (scratch.isValid() && listening) {
            // 细水长流：请求读掉，回一个 Content-Length 很大、body 只给 1 KB 的响应，
            // 然后**再也不给**（连接也不关 —— 关了 Qt 会当成"传完了"）。
            // 那 1 KB 是关键：一个字节都没收到过的回复按新判据不记 strike，这一条就变成
            // (b) 那个形状了（下面单独有用例钉住）。
            QObject::connect(&fakeMirror, &QTcpServer::newConnection, &fakeMirror, [&fakeMirror] {
                while (QTcpSocket *s = fakeMirror.nextPendingConnection()) {
                    s->setParent(&fakeMirror);
                    QObject::connect(s, &QTcpSocket::readyRead, s, [s]() {
                        if (s->property("answered").toBool()) {
                            return;
                        }
                        s->setProperty("answered", true);
                        s->readAll();
                        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                                 "Content-Length: 1000000\r\n\r\n");
                        s->write(QByteArray(1024, 'x'));
                        s->flush();
                    });
                }
            });

            const QString officialPath = scratch.path() + "/official.txt";
            const QByteArray body = QByteArrayLiteral("official-source-payload");
            {
                QFile f(officialPath);
                check(f.open(QIODevice::WriteOnly) && f.write(body) == body.size(),
                      "V6f 官方那条（一个真文件）写好了");
            }

            const QString mirrorUrl = QStringLiteral("http://127.0.0.1:%1/pretend-mirror")
                                          .arg(fakeMirror.serverPort());
            const QString destPath = scratch.path() + "/out.txt";
            const QString sha1 = QString::fromLatin1(
                QCryptographicHash::hash(body, QCryptographicHash::Sha1).toHex());

            DownloadManager dm;
            dm.setUseMirror(true);
            QEventLoop loop;
            bool finished = false;
            bool ok = false;
            QString failure;
            QObject::connect(&dm, &DownloadManager::allDownloadsFinished, &loop,
                             [&](bool success) {
                                 finished = true;
                                 ok = success;
                                 loop.quit();
                             });
            QObject::connect(&dm, &DownloadManager::downloadFinished, &loop,
                             [&](const QString &, bool success, const QString &m) {
                                 if (!success) {
                                     failure = m;
                                 }
                             });
            QElapsedTimer elapsed;
            elapsed.start();
            dm.beginBatch();
            dm.downloadFile(mirrorUrl, destPath, sha1, body.size(), VerifyMode::SizeAndSha1,
                            QUrl::fromLocalFile(officialPath).toString());
            dm.endBatch();
            // 兜底：真卡住也不能让验证工程自己挂在这儿（看门狗那两跳一共 10 秒）。
            QTimer::singleShot(60000, &loop, &QEventLoop::quit);
            loop.exec();
            const qint64 tookMs = elapsed.elapsed();

            check(finished, "V6f 这条下载有终态（没终态 = 挂住了，整批都不动了）");
            check(dm.mirrorDemotedFor(QStringLiteral("127.0.0.1")),
                  "V6f 假镜像细水长流了 10 秒 -> **那个主机**被降级（降级是按主机记的，"
                  "所以问的是 127.0.0.1 而不是镜像源那个主机）");
            check(ok, "V6f 退回官方那条之后下成了", failure);
            QFile out(destPath);
            check(out.exists(), "V6f 文件真的落盘了", destPath);
            check(out.open(QIODevice::ReadOnly) && out.readAll() == body,
                  "V6f 落盘的内容就是官方那条给的（不是假镜像那份半截 body）");
            check(tookMs >= 8000 && tookMs < 45000,
                  "V6f 是「等两跳才退」那个时间（10 秒上下：不是立刻退，也没等到 30 秒超时）",
                  QStringLiteral("%1 ms").arg(tookMs));
        }

        // (b) 一个字节都不吐的假镜像。
        //
        // 422 个文件的导入会因"还没收到第一个字节也算失速"垮掉：
        // 头一批里 8 条两跳就被判"镜像太慢"，然后整个主机被降级 300 秒。所以
        // countsTowardStallStrike() 那条判据（零字节不算"太慢"）**仍然保留**。
        QTcpServer silentMirror;
        const bool silentListening = silentMirror.listen(QHostAddress::LocalHost, 0);
        check(silentListening, "V6f 第二个假镜像（只 accept、一个字节都不发）起来了");
        if (scratch.isValid() && silentListening) {
            QObject::connect(&silentMirror, &QTcpServer::newConnection, &silentMirror,
                             [&silentMirror] {
                                 silentMirror.nextPendingConnection()->setParent(&silentMirror);
                             });

            const QString officialPath = scratch.path() + "/official2.txt";
            const QByteArray body2 = QByteArrayLiteral("official-source-payload-2");
            {
                QFile f(officialPath);
                check(f.open(QIODevice::WriteOnly) && f.write(body2) == body2.size(),
                      "V6f 官方那条（第二个真文件）写好了");
            }

            DownloadManager dm2;
            dm2.setUseMirror(true);
            QEventLoop loop;
            bool finished = false;
            bool ok = false;
            QString failure;
            QObject::connect(&dm2, &DownloadManager::allDownloadsFinished, &loop,
                             [&](bool success) {
                                 finished = true;
                                 ok = success;
                                 loop.quit();
                             });
            QObject::connect(&dm2, &DownloadManager::downloadFinished, &loop,
                             [&](const QString &, bool success, const QString &m) {
                                 if (!success) {
                                     failure = m;
                                 }
                             });
            QElapsedTimer elapsed;
            elapsed.start();
            dm2.beginBatch();
            dm2.downloadFile(QStringLiteral("http://127.0.0.1:%1/silent-mirror")
                                 .arg(silentMirror.serverPort()),
                             scratch.path() + "/out2.txt",
                             QString::fromLatin1(
                                 QCryptographicHash::hash(body2, QCryptographicHash::Sha1).toHex()),
                             body2.size(), VerifyMode::SizeAndSha1,
                             QUrl::fromLocalFile(officialPath).toString());
            dm2.endBatch();
            // 兜底：真卡住也不能让验证工程自己挂在这儿（(0b) 那条判据 12 秒）。
            QTimer::singleShot(60000, &loop, &QEventLoop::quit);
            loop.exec();
            const qint64 tookMs = elapsed.elapsed();

            check(finished, "V6f 一个字节都不吐的那条也有终态（没被误判成宿醉，也没挂住）");
            check(dm2.mirrorDemotedFor(QStringLiteral("127.0.0.1")),
                  "V6f 入口自己 12 秒一个字节都不给 -> 那个主机被降级（排队里的作业改走官方）。"
                  "零字节那一条**不是**靠 countsTowardStallStrike() 两跳判的 —— 判据没动");
            check(ok, "V6f 它照样退回官方那条下成了", failure);
            QFile out2(scratch.path() + "/out2.txt");
            check(out2.exists() && out2.open(QIODevice::ReadOnly) && out2.readAll() == body2,
                  "V6f 落盘的内容就是官方那条给的");
            check(tookMs >= 11000 && tookMs < 25000,
                  "V6f 走的是 (0b) 那条「12 秒零字节就换路」（不是 Qt 那条 30 秒不活动超时）",
                  QStringLiteral("%1 ms").arg(tookMs));
        }
    }

    // --- V6i. 发出去的请求有上限 / 卡死的回复会被判失败----------
    //
    // 这一节钉的是"一次 422 个文件的整合包导入把整批挂死"那两个真因。现场：
    // 422 个请求一次性全交给 QNetworkAccessManager → Qt 每个主机只开 6 条连接，其余在它
    // 内部排队（30 秒的传输超时从请求建好就开始算）→ 排在后面的回复一个字节都收不到 →
    // 失速看门狗两跳（10 秒）把它们全判成"镜像太慢"并重发 → 重发又是几百条一起发 →
    // 最后整批再也不动：进度条停在 1%、没有错误、没有弹窗，用户只能重启。
    //
    // 两条都在本地跑：假服务器只 accept、一个字节都不发，一次外网都不碰。
    {
        // (1) 判据本身（纯函数）：到顶之后**不管还排着多少条**都不发。
        constexpr int kCap = DownloadManager::kMaxConcurrentDownloads;
        check(!DownloadManager::mayIssueAnotherDownload(0, 0),
              "V6i 队列是空的 -> 没有什么可发");
        check(DownloadManager::mayIssueAnotherDownload(0, 1),
              "V6i 队列里有一条、连接空着 -> 发");
        check(DownloadManager::mayIssueAnotherDownload(kCap - 1, 1),
              "V6i 第 kCap 条还能发（0 起算 = kCap-1）",
              QStringLiteral("kCap=%1").arg(kCap));
        check(!DownloadManager::mayIssueAnotherDownload(kCap, 1),
              "V6i 到顶了 -> 不发（哪怕队列里还排着一条）",
              QStringLiteral("kCap=%1").arg(kCap));
        check(!DownloadManager::mayIssueAnotherDownload(kCap, 10000),
              "V6i 到顶了、还排着一万条 -> 一条都不发（挂死那次的形状）");
        check(!DownloadManager::mayIssueAnotherDownload(kCap + 1, 3),
              "V6i 超过了也不发");
    }

    // (1b) 判据本身（纯函数）：**按主机**那一条（ 加，把上限从 6 调到 24 时补的）。
    //
    // 全局放 24 条之后，"排队必须发生在我们自己手里"这条不变量在**单个主机**上会被打破：
    // 一个走 h1.1 的主机，Qt 只认 6 条连接，第 7 条开始进 Qt 内部那个会超时的队列
    // （本地实测：24 路放给一个每条 12 秒才回的 h1.1 假服务器，40 条作业里 28 条
    // `Operation timed out`）。所以没证明自己走 h2 的主机只能放 6 条 —— 这个 6 就是
    // Qt 每主机的连接上限，正好是"我们发出去的都拿得到连接"的最大条数。
    {
        constexpr int kHostCap = DownloadManager::kMaxConcurrentPerHost;
        check(DownloadManager::mayIssueAnotherDownloadToHost(0, false),
              "V6i 这个主机还没证明过 h2、当下一条都没有 -> 发");
        check(DownloadManager::mayIssueAnotherDownloadToHost(kHostCap - 1, false),
              "V6i 还没证明过 h2、到第 kHostCap-1 条 -> 还能发",
              QStringLiteral("kHostCap=%1").arg(kHostCap));
        check(!DownloadManager::mayIssueAnotherDownloadToHost(kHostCap, false),
              "V6i 还没证明过 h2 的主机放满 kHostCap 条 -> 不发（多发的会进 Qt 那个会超时的队列）",
              QStringLiteral("kHostCap=%1").arg(kHostCap));
        check(!DownloadManager::mayIssueAnotherDownloadToHost(kHostCap + 5, false),
              "V6i 一个没证明过 h2 的主机超了 -> 更不发");
        check(DownloadManager::mayIssueAnotherDownloadToHost(kHostCap, true),
              "V6i 证明过 h2 了 -> 这条判据不再管它（放几条归全局上限）",
              QStringLiteral("kHostCap=%1").arg(kHostCap));
        check(DownloadManager::mayIssueAnotherDownloadToHost(1000, true),
              "V6i 证明过 h2 的主机上一千条在飞也不管（流多路复用，不是连接）");
    }

    // (2) 端到端：一次排进去**比上限多得多**的作业，真正被交给 Qt 的只有按主机允许的那几条。
    //
    // NOTE: 作业数必须**超过**上限，否则这条用例会退化成"全都发出去了也算过"—— 写死
    // 20 条时，上限一调高就一条都不会排队，用例还照样绿。
    //
    // 【为什么非要**两台**假服务器】单看一台是量不出按主机那条判据的：Qt 自己对每个
    // host:port 也只开一个固定的通道数，所以"我们拦住了"和"Qt 拦住了"这两种情况在那台
    // 服务器上看到的都是一样的条数 —— 那么写的话，把判据整个删掉也照样绿
    // （**断言要量"由我们自己决定"的那一格**）。
    // 两台服务器就不一样了：Qt 的通道数按 host:port 各算各的（敢给的话它开 12 + 12），
    // 而我们的判据按**主机名**算（同一个 127.0.0.1，两个端口合起来只有
    // kMaxConcurrentPerHost 条）。于是"两台服务器上一共挂了几条连接"这一格由我们说了算：
    // kMaxConcurrentPerHost = 判据在，翻倍 = 判据被删了。
    //
    // 这条同时是那类挂死的**直接**回归测试：没有队列时几百条会一个不落地全建连（422 条
    // 就是 422 条），多出来的留在我们自己的队列里 —— 只有"在我们手里排"才谈得上后面那些
    // 看门狗判据。
    {
        QTcpServer floodA;
        QTcpServer floodB;
        const bool floodListening = floodA.listen(QHostAddress::LocalHost, 0)
                                    && floodB.listen(QHostAddress::LocalHost, 0);
        check(floodListening, "V6i 两个假服务器都起来了（起不来这条用例就没意义，直接算失败）");
        if (floodListening) {
            // 只接不回：连上的那些就一直挂着（"收不到一个字节"那种回复）。
            const auto hold = [](QTcpServer *server) {
                QObject::connect(server, &QTcpServer::newConnection, server,
                                 [server] { server->nextPendingConnection()->setParent(server); });
            };
            hold(&floodA);
            hold(&floodB);
            QTemporaryDir floodDir(QDir::tempPath() + "/wtfml_flood-XXXXXX");
            check(floodDir.isValid(), "V6i 落盘用的临时目录建出来了");
            if (floodDir.isValid()) {
                {
                    DownloadManager dm;
                    QEventLoop spin;
                    const int jobs = DownloadManager::kMaxConcurrentDownloads * 2;
                    dm.beginBatch();
                    for (int i = 0; i < jobs; ++i) {
                        // 两个端口交替：两个主机名相同、端口不同 —— 正是上面说的那个形状。
                        const quint16 port = (i % 2 == 0) ? floodA.serverPort() : floodB.serverPort();
                        dm.downloadFile(QStringLiteral("http://127.0.0.1:%1/mod%2.jar")
                                            .arg(port)
                                            .arg(i),
                                        floodDir.path() + QStringLiteral("/mod%1.jar").arg(i));
                    }
                    dm.endBatch();
                    // 给 Qt 一点时间把连接建起来（它自己的超时是 30 秒，这里只等 0.7 秒）。
                    QTimer::singleShot(700, &spin, &QEventLoop::quit);
                    spin.exec();
                    const int sockets = floodA.findChildren<QTcpSocket *>().size()
                                        + floodB.findChildren<QTcpSocket *>().size();
                    out() << "        " << jobs << " 条作业 -> 两个端口的假服务器上一共挂着 "
                          << sockets << " 条连接（按主机上限 "
                          << DownloadManager::kMaxConcurrentPerHost << "；判据没了的话是 "
                          << DownloadManager::kMaxConcurrentPerHost * 2 << "）\n";
                    check(sockets >= 1, "V6i 至少真发出去了一条（不是整批都没动）",
                          QStringLiteral("connections=%1").arg(sockets));
                    check(sockets <= DownloadManager::kMaxConcurrentPerHost,
                          "V6i 一次发出去的作业比上限多 -> 多出来的排在我们自己的队列里"
                          "（同一个主机名的两个端口合起来也只放 kMaxConcurrentPerHost 条）",
                          QStringLiteral("connections=%1").arg(sockets));
                    // 这时候整批当然还没完（6 条挂着、42 条排着）：界面不该解禁。
                    check(dm.isBusy(), "V6i 队列里还有活儿的时候 isBusy() 为真");
                }
            }
        }
    }

    // (2b) Qt 的每主机通道数：同样一次性排满一档作业，但**全发给同一个 host:port**。
    //
    // Qt 默认每个 HTTP/1.1 主机只开 6 条通道，第 7 条起进它内部那个会超时的队列 ——
    // 那正是 (2) 里那条端到端事故的形状。现在每条请求都带
    // `QHttp1Configuration::setNumberOfConnectionsPerHost(kMaxConcurrentPerHost)`，
    // 所以同一个 host:port 上应该真能看到超过 6 条连接。这条钉子量的就是
    // "我们的按主机闸"和"Qt 的通道数"是不是同一个数：把 configureHttp1() 删掉，
    // 这里会退回 6。
    {
        QTcpServer single;
        const bool singleListening = single.listen(QHostAddress::LocalHost, 0);
        check(singleListening, "V6i 量通道数用的假服务器起来了");
        if (singleListening) {
            QObject::connect(&single, &QTcpServer::newConnection, &single,
                             [&single] { single.nextPendingConnection()->setParent(&single); });
            QTemporaryDir channelDir(QDir::tempPath() + "/wtfml_channels-XXXXXX");
            check(channelDir.isValid(), "V6i 通道数用例的临时目录建出来了");
            if (channelDir.isValid()) {
                DownloadManager dm;
                QEventLoop spin;
                const int jobs = DownloadManager::kMaxConcurrentPerHost;
                dm.beginBatch();
                for (int i = 0; i < jobs; ++i) {
                    dm.downloadFile(QStringLiteral("http://127.0.0.1:%1/same%2.jar")
                                        .arg(single.serverPort())
                                        .arg(i),
                                    channelDir.path() + QStringLiteral("/same%1.jar").arg(i));
                }
                dm.endBatch();
                QTimer::singleShot(700, &spin, &QEventLoop::quit);
                spin.exec();
                const int sockets = single.findChildren<QTcpSocket *>().size();
                out() << "        " << jobs << " 条同 host:port 作业 -> 假服务器上挂着 "
                      << sockets << " 条连接（Qt 出厂每主机 6 条）\n";
                check(sockets > 6,
                      "V6i Qt 的每主机通道数已经抬到 kMaxConcurrentPerHost"
                      "（没抬的话这里只有 6 条，多出来的两条会进 Qt 内部那个会超时的队列）",
                      QStringLiteral("connections=%1").arg(sockets));
                check(sockets <= DownloadManager::kMaxConcurrentPerHost,
                      "V6i 抬通道数没有越过我们自己那道按主机闸",
                      QStringLiteral("connections=%1").arg(sockets));
            }
        }
    }

    // (3) 端到端：一条**没有备用地址**、发出去之后一个字节都收不到的下载，到点会被判失败。
    //
    // 这一条是 V6f 的另一半。V6f 那种回复有退路（走镜像 + 知道官方那条地址），看门狗 10 秒
    // 就 abort 它换一条路；**没有**退路的那些也必须有人管 —— 它一旦永远不结束，
    // pendingDownloads 就永远归不了零，整批永远收不了尾。
    {
        QTcpServer deadServer;
        const bool deadListening = deadServer.listen(QHostAddress::LocalHost, 0);
        check(deadListening, "V6i 第二个假服务器起来了（这条用来验卡死兜底）");
        if (deadListening) {
            QObject::connect(&deadServer, &QTcpServer::newConnection, &deadServer,
                             [&deadServer] {
                                 deadServer.nextPendingConnection()->setParent(&deadServer);
                             });
            QTemporaryDir deadDir(QDir::tempPath() + "/wtfml_wedged-XXXXXX");
            check(deadDir.isValid(), "V6i 卡死那条的临时目录建出来了");
            if (deadDir.isValid()) {
                const QString destPath = deadDir.path() + "/never.jar";
                DownloadManager dm;
                // 真机上这个期限是 90 秒（看门狗两跳的九倍），逐条用例等不起，压到 3 秒。
                dm.setNoProgressGiveUpMs(3000);
                bool finished = false;
                bool ok = true;
                QString failure;
                QEventLoop loop;
                QObject::connect(&dm, &DownloadManager::allDownloadsFinished, &loop,
                                 [&](bool success) {
                                     finished = true;
                                     ok = success;
                                     loop.quit();
                                 });
                QObject::connect(&dm, &DownloadManager::downloadFinished, &loop,
                                 [&](const QString &, bool success, const QString &m) {
                                     if (!success) {
                                         failure = m;
                                     }
                                 });
                QElapsedTimer elapsed;
                elapsed.start();
                dm.beginBatch();
                dm.downloadFile(QStringLiteral("http://127.0.0.1:%1/never.jar")
                                    .arg(deadServer.serverPort()),
                                destPath);
                dm.endBatch();
                // 兜底：真卡住也不能让验证工程自己挂在这儿。
                QTimer::singleShot(30000, &loop, &QEventLoop::quit);
                loop.exec();
                const qint64 tookMs = elapsed.elapsed();

                check(finished, "V6i 没有备用地址、又收不到字节的那条也有终态（以前它能永远挂着）");
                check(!ok, "V6i 它被判成失败（不是「什么都没发生」）");
                check(dm.stats().failed == 1, "V6i 失败数记上了",
                      QString::number(dm.stats().failed));
                check(failure == QStringLiteral("下载源 127.0.0.1 在 3 秒内未返回数据。"),
                      "V6i 无数据超时失败原因包含节点和时长", failure);
                check(!QFile::exists(destPath), "V6i 没有落下半个文件", destPath);
                check(tookMs >= 2500 && tookMs < 25000,
                      "V6i 是「到点才判」那个时间（不是立刻失败，也不是等到 30 秒传输超时）",
                      QStringLiteral("%1 ms").arg(tookMs));
                check(!dm.isBusy(), "V6i 收尾之后不忙了 —— 界面不会永远停在「下载中」");
            }
        }
    }

    // --- V6j. 镜像 302 到一台"接得上、一个字节都不给"的落点--------
    //
    // 实测的形状（校园网，无代理）：BMCLAPI 的资源地址 302 到
    // bmclapiN.sakura.sld.tw，TLS 接得上、一个字节都不来（curl 25 秒 0 字节）；
    // 同一时刻官方 resources.download.minecraft.net 0.4~1.6 秒一个对象。
    // 只靠 90 秒兜底动不了这种回复 —— 一条死落点占满一个并发位一分半钟，
    // 24 位里常年有几条在等死节点，整批只剩一两文件/秒（实测 751/5222、十一分钟）。
    //
    // 现在要钉住的三件事（这条用例把它压成一秒半）：
    // 1) 走镜像、有退路、连首字节都没等到的回复，到点就换路（不等 90 秒）；
    // 2) 那台落点被拉黑，第二次再转到它时当场 abort（约一个往返）；
    // 3) 两条都没成之后退回官方，作业照常下成、不算失败。
    {
        QTemporaryDir scratch(QDir::tempPath() + "/wtfml_deadredir-XXXXXX");
        check(scratch.isValid(), "V6j 临时目录建出来了");
        QTcpServer fakeMirror;
        QTcpServer deadBackend;
        const bool listening = fakeMirror.listen(QHostAddress::LocalHost, 0)
                               && deadBackend.listen(QHostAddress::LocalHost, 0);
        check(listening, "V6j 两个假服务器起来了（入口 + 死落点）");
        if (scratch.isValid() && listening) {
            // 死落点：连接照接、一个字都不写（sakura 那几台就是这个形状）。
            QObject::connect(&deadBackend, &QTcpServer::newConnection, &deadBackend,
                             [&deadBackend] {
                                 while (QTcpSocket *s = deadBackend.nextPendingConnection()) {
                                     s->setParent(&deadBackend);
                                 }
                             });
            // 入口：每条请求回一个 302，转到上面那台死落点（同主机不同端口 ——
            // 判"换了一台落点"用的是主机 + 端口，见 authorityKey()）。
            const QByteArray location =
                QStringLiteral("http://127.0.0.1:%1/dead-backend")
                    .arg(deadBackend.serverPort())
                    .toUtf8();
            QObject::connect(&fakeMirror, &QTcpServer::newConnection, &fakeMirror,
                             [&fakeMirror, location] {
                                 while (QTcpSocket *s = fakeMirror.nextPendingConnection()) {
                                     s->setParent(&fakeMirror);
                                     QObject::connect(s, &QTcpSocket::readyRead, s, [s, location]() {
                                         if (s->property("answered").toBool()) return;
                                         s->setProperty("answered", true);
                                         s->readAll();
                                         s->write("HTTP/1.1 302 Found\r\nLocation: " + location
                                                  + "\r\nContent-Length: 0\r\n"
                                                    "Connection: close\r\n\r\n");
                                         s->flush();
                                     });
                                 }
                             });

            const QString officialPath = scratch.path() + "/official.txt";
            const QByteArray body = QByteArrayLiteral("official-payload-after-dead-redirect");
            {
                QFile f(officialPath);
                check(f.open(QIODevice::WriteOnly) && f.write(body) == body.size(),
                      "V6j 官方那条（一个真文件）写好了");
            }

            const QString mirrorUrl = QStringLiteral("http://127.0.0.1:%1/pretend-mirror")
                                          .arg(fakeMirror.serverPort());
            const QString destPath = scratch.path() + "/out.txt";
            const QString sha1 = QString::fromLatin1(
                QCryptographicHash::hash(body, QCryptographicHash::Sha1).toHex());

            DownloadManager dm;
            dm.setUseMirror(true);
            // 真机上首字节期限是 12 秒；这一条只能靠时间验，压到 1.5 秒。
            dm.setNoFirstByteGiveUpMs(1500);
            QEventLoop loop;
            bool finished = false;
            bool ok = false;
            QString failure;
            QObject::connect(&dm, &DownloadManager::allDownloadsFinished, &loop,
                             [&](bool success) {
                                 finished = true;
                                 ok = success;
                                 loop.quit();
                             });
            QObject::connect(&dm, &DownloadManager::downloadFinished, &loop,
                             [&](const QString &, bool success, const QString &m) {
                                 if (!success) failure = m;
                             });
            QElapsedTimer elapsed;
            elapsed.start();
            dm.beginBatch();
            dm.downloadFile(mirrorUrl, destPath, sha1, body.size(), VerifyMode::SizeAndSha1,
                            QUrl::fromLocalFile(officialPath).toString());
            dm.endBatch();
            // 兜底：真卡住也不能让验证工程自己挂在这儿。
            QTimer::singleShot(30000, &loop, &QEventLoop::quit);
            loop.exec();
            const qint64 tookMs = elapsed.elapsed();

            check(finished, "V6j 这条下载有终态（没终态 = 死落点还在占着并发位）");
            check(ok, "V6j 退回官方那条之后下成了", failure);
            check(tookMs >= 1200 && tookMs < 10000,
                  "V6j 是「到点就换路」那个时间（1.5 秒左右；不是等 90 秒兜底、"
                  "也不是立刻失败）",
                  QStringLiteral("%1 ms").arg(tookMs));
            QFile out(destPath);
            check(out.exists() && out.open(QIODevice::ReadOnly) && out.readAll() == body,
                  "V6j 落盘的内容就是官方那条给的（死落点一个字节都没混进来）");
            check(dm.stats().failed == 0, "V6j 换路不算失败",
                  QString::number(dm.stats().failed));
        }
    }

    // --- V7. 安装完之后的产物核对 + launcher_profiles ---------------------------
    //
    // 这两条是"退出码 0 不等于装好了"那一道闸。退出码是安装器自己说了算的，而**磁盘上
    // 那份 json** 才是别的启动器和我们的启动路径要读的东西。
    {
        const auto produced = [](const QString &id, const QString &parent) {
            QJsonObject json;
            if (!id.isNull()) {
                json.insert(QStringLiteral("id"), id);
            }
            if (!parent.isNull()) {
                json.insert(QStringLiteral("inheritsFrom"), parent);
            }
            return json;
        };
        const QString forgeId = LoaderMeta::forgeLoaderId(QStringLiteral("1.20.1"),
                                                          QStringLiteral("47.4.10"));

        const LoaderMeta::ProfileCheck good =
            LoaderMeta::checkInstallerResult(produced(forgeId, QStringLiteral("1.20.1")),
                                             QStringLiteral("1.20.1"), forgeId);
        check(good.ok(), "V7 id 和 inheritsFrom 都对 -> 认它装好了",
              QStringLiteral("%1 / %2").arg(good.id, good.parent));

        const LoaderMeta::ProfileCheck renamed =
            LoaderMeta::checkInstallerResult(produced(QStringLiteral("1.20.1-forge-47.4.23"),
                                                      QStringLiteral("1.20.1")),
                                             QStringLiteral("1.20.1"), forgeId);
        check(renamed.problem == LoaderMeta::ProfileProblem::BadId,
              "V7 安装器哪天换了命名约定（id 和我们算的目录名对不上）-> 不认",
              renamed.id);

        // 目录名合法但安装器认的是另一个父版本：装是装上了，但它基于的不是用户选的那个。
        const LoaderMeta::ProfileCheck otherParent =
            LoaderMeta::checkInstallerResult(produced(forgeId, QStringLiteral("1.20.4")),
                                             QStringLiteral("1.20.1"), forgeId);
        check(otherParent.problem == LoaderMeta::ProfileProblem::WrongParent,
              "V7 装出来的版本基于另一个原版 -> 不认（启动时它去找那个原版，用户不知道）",
              otherParent.parent);

        // 空 json / 只有一半：走同一条路，别崩。
        check(LoaderMeta::checkInstallerResult(QJsonObject(), QStringLiteral("1.20.1"), forgeId)
                      .problem == LoaderMeta::ProfileProblem::BadId &&
                  LoaderMeta::checkInstallerResult(produced(forgeId, QStringLiteral("1.20.1")),
                                                   QStringLiteral("1.20.1"), QString())
                          .problem == LoaderMeta::ProfileProblem::BadId,
              "V7 空 json / 期望 id 是空的 -> BadId，不是崩");

        // launcher_profiles.json：安装器的硬前置（缺了它安装器直接拒绝干活）。我们只在
        // 它不存在时才补一份 —— 已有的那份里可能是用户真正的启动器档案。
        check(LoaderMeta::launcherProfilesFileName() == QStringLiteral("launcher_profiles.json"),
              "V7 前置文件名");
        const QJsonObject minimal =
            QJsonDocument::fromJson(LoaderMeta::minimalLauncherProfiles()).object();
        check(minimal.value(QStringLiteral("version")).toInt() == 3 &&
                  minimal.value(QStringLiteral("profiles")).isObject() &&
                  minimal.value(QStringLiteral("settings")).isObject(),
              "V7 补的那份是能解析的 JSON，且带着安装器认的那三个键",
              QString::fromUtf8(LoaderMeta::minimalLauncherProfiles()).left(60));
    }

    // --- V8. OptiFine：版本表得从"网页"里读出来 --------------------------------
    //
    // 这一节整节拿**真数据**验（三份 OptiFine fixture 见本函数开头那份清单）。OptiFine
    // 和另外三个加载器最不一样的地方就在这儿：它**没有一份规整的版本表接口** —— 镜像那边
    // 是一条 JSON 数组（正式版和预览版混在一起），官方那边只有一张给人看的下载页（203 KB
    // 的 HTML，53 个游戏版本各一节，每节里"正式版/预览版/旧版本"三类行混排）。所以"挑哪
    // 一个"这件事全是从网页里抠出来的，也正是最该拿真页面钉住的地方。
    {
        // --- V8.1 地址怎么拼（四条都真发过请求）---
        check(LoaderMeta::optifineDownloadsPageUrl() ==
                  QStringLiteral("https://optifine.net/downloads"),
              "V8.1 官方下载页地址", LoaderMeta::optifineDownloadsPageUrl());
        check(LoaderMeta::optifineMirrorListUrl(QStringLiteral("1.18.2")) ==
                  QStringLiteral("https://bmclapi2.bangbang93.com/optifine/1.18.2"),
              "V8.1 镜像的版本表地址（一个游戏版本一页）",
              LoaderMeta::optifineMirrorListUrl(QStringLiteral("1.18.2")));
        check(LoaderMeta::optifineMirrorFileUrl(QStringLiteral("1.18.2"), QStringLiteral("HD_U"),
                                                QStringLiteral("H9")) ==
                  QStringLiteral("https://bmclapi2.bangbang93.com/optifine/1.18.2/HD_U/H9"),
              "V8.1 镜像的文件地址最后两段是 type/patch（**不是**文件名）",
              LoaderMeta::optifineMirrorFileUrl(QStringLiteral("1.18.2"), QStringLiteral("HD_U"),
                                                QStringLiteral("H9")));
        check(LoaderMeta::optifineAdPageUrl(QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar")) ==
                  QStringLiteral("https://optifine.net/adloadx?f=OptiFine_1.18.2_HD_U_H9.jar"),
              "V8.1 官方那条路上的广告页地址",
              LoaderMeta::optifineAdPageUrl(QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar")));

        // --- V8.2 文件名白名单：它要落成盘上一个文件、也要拼进地址 ---
        check(LoaderMeta::isSafeJarFileName(QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar")) &&
                  LoaderMeta::isSafeJarFileName(
                      QStringLiteral("preview_OptiFine_1.18.2_HD_U_H9_pre2.jar")) &&
                  LoaderMeta::isSafeJarFileName(
                      QStringLiteral("forge-1.20.1-47.4.10-installer.jar")),
              "V8.2 真文件名都放行（正式版 / 预览版 / Forge 那个）");
        check(!LoaderMeta::isSafeJarFileName(QStringLiteral("../x.jar")) &&
                  !LoaderMeta::isSafeJarFileName(QStringLiteral("a/b.jar")) &&
                  !LoaderMeta::isSafeJarFileName(QStringLiteral("a\\b.jar")) &&
                  !LoaderMeta::isSafeJarFileName(QStringLiteral("x.jar?x=1")) &&
                  !LoaderMeta::isSafeJarFileName(QStringLiteral("x.jar.exe")) &&
                  !LoaderMeta::isSafeJarFileName(QStringLiteral("上.jar")) &&
                  !LoaderMeta::isSafeJarFileName(QStringLiteral("x.jar ")) &&
                  !LoaderMeta::isSafeJarFileName(QStringLiteral("x.zip")) &&
                  !LoaderMeta::isSafeJarFileName(QString()),
              "V8.2 带路径 / 带参数 / 中文 / 尾空格 / 空 / 不是 .jar 的一律拒绝");
        check(LoaderMeta::isOptifinePreviewFile(
                  QStringLiteral("preview_OptiFine_1.18.2_HD_U_H9_pre2.jar")) &&
                  !LoaderMeta::isOptifinePreviewFile(
                      QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar")),
              "V8.2 预览版靠文件名前缀认（真数据里就是这一条把 11 条筛掉了 8 条）");

        // --- V8.3 安装器文件名 -> 版本号（要写进 wtfml.meta、也要和目录名对得上）---
        const auto editionOf = [](const QString &mc, const QString &file) {
            return LoaderMeta::optifineEditionFromFile(mc, file);
        };
        check(editionOf(QStringLiteral("1.18.2"),
                        QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar")) ==
                  QStringLiteral("HD_U_H9"),
              "V8.3 OptiFine_1.18.2_HD_U_H9.jar -> HD_U_H9",
              editionOf(QStringLiteral("1.18.2"), QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar")));
        check(editionOf(QStringLiteral("1.18.2"),
                        QStringLiteral("preview_OptiFine_1.18.2_HD_U_H9_pre2.jar")) ==
                  QStringLiteral("HD_U_H9_pre2"),
              "V8.3 预览版切出来是 HD_U_H9_pre2（和镜像那份 type=HD_U_H9 + patch=pre2 对得上）",
              editionOf(QStringLiteral("1.18.2"),
                        QStringLiteral("preview_OptiFine_1.18.2_HD_U_H9_pre2.jar")));
        // 换个游戏版本就得换前缀切 —— 切法只有这一处，官方/镜像两条路才切得同一个值。
        check(editionOf(QStringLiteral("1.8.9"), QStringLiteral("OptiFine_1.8.9_HD_U_M5.jar")) ==
                  QStringLiteral("HD_U_M5"),
              "V8.3 换一个游戏版本（1.8.9 -> HD_U_M5）",
              editionOf(QStringLiteral("1.8.9"), QStringLiteral("OptiFine_1.8.9_HD_U_M5.jar")));

        // --- V8.4 官方下载页：稳定档认 downloadLineMain，最新档认最上面那条 ------
        QFile downloadsFile(metaDir + "/optifine-downloads.html");
        QByteArray downloads;
        if (downloadsFile.open(QIODevice::ReadOnly)) {
            downloads = downloadsFile.readAll();
        }
        check(downloads.size() > 100000, "V8.4 真下载页读到了",
              QString("%1 字节").arg(downloads.size()));
        if (!downloads.isEmpty()) {
            const QString html = QString::fromUtf8(downloads);
            const auto pickFromPage = [&html](const QString &mc) {
                return LoaderMeta::pickOptifineFromDownloadsPage(html, mc);
            };
            const auto pickNewest = [&html](const QString &mc) {
                return LoaderMeta::pickOptifineFromDownloadsPage(html, mc,
                                                                 LoaderMeta::LoaderChannel::Newest);
            };

            const LoaderMeta::OptiFinePick pick1182 = pickFromPage(QStringLiteral("1.18.2"));
            check(pick1182.ok() &&
                      pick1182.fileName == QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar"),
                  "V8.4 1.18.2 稳定档 -> H9（More 里那两条 H7/H6 不能要）", pick1182.fileName);
            // 这一节里没有预览版，所以**两档挑出来的是同一个** —— 这正是"换了档才会不一样"
            // 那条分界的反证：不看数据就没法知道哪一版有这个区别。
            check(pickNewest(QStringLiteral("1.18.2")).fileName ==
                      QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar"),
                  "V8.4 1.18.2 最新档也是 H9（这一节里没有预览版）",
                  pickNewest(QStringLiteral("1.18.2")).fileName);

            // 1.8.9 那一节更狠：预览版（M6-pre2/pre1）排在最前面，正文是 M5，More 里还有一堆
            // 旧版本。稳定档必须挑 M5；最新档挑的**就是**最上面那条预览版（用户
            // 拍板："beta 版虽然不稳定，但是也能使用，要列出来"——预览版同理）。
            const LoaderMeta::OptiFinePick pick189 = pickFromPage(QStringLiteral("1.8.9"));
            check(pick189.ok() &&
                      pick189.fileName == QStringLiteral("OptiFine_1.8.9_HD_U_M5.jar"),
                  "V8.4 1.8.9 稳定档 -> M5（那一节最前面是预览版 M6-pre2，不能要）",
                  pick189.fileName);
            const LoaderMeta::OptiFinePick newest189 = pickNewest(QStringLiteral("1.8.9"));
            check(newest189.fileName ==
                      QStringLiteral("preview_OptiFine_1.8.9_HD_U_M6_pre2.jar"),
                  "V8.4 1.8.9 最新档 -> 最上面那条预览版 M6-pre2", newest189.fileName);
            // 两档挑出来的**必须是不同的文件**：这一节就是"多一档"这件事在真数据上的证据。
            check(newest189.fileName != pick189.fileName,
                  "V8.4 1.8.9 上两档挑出不同版本（稳定 M5 / 最新 M6-pre2）");

            // 只有预览版、还没发正式版的游戏版本：那一节里**根本没有 main 行** ——
            // 稳定档挑不出来（由调用方报"还没有支持这个版本的正式版"），**最新档挑得出来**
            // （ 之前这里两档都挑不出来，预览版也装不了；现在装得下去了）。
            check(!pickFromPage(QStringLiteral("1.21.8")).ok() &&
                      !pickFromPage(QStringLiteral("26.1.2")).ok(),
                  "V8.4 只有预览版的版本（1.21.8 / 26.1.2）稳定档挑不出来");
            check(pickNewest(QStringLiteral("1.21.8")).fileName ==
                      QStringLiteral("preview_OptiFine_1.21.8_HD_U_J6_pre16.jar") &&
                      pickNewest(QStringLiteral("26.1.2")).fileName ==
                          QStringLiteral("preview_OptiFine_26.1.2_HD_U_K1_pre2.jar"),
                  "V8.4 同一批版本最新档挑得出预览版（1.21.8 -> J6_pre16 / 26.1.2 -> K1_pre2）",
                  QStringLiteral("%1 / %2")
                      .arg(pickNewest(QStringLiteral("1.21.8")).fileName,
                           pickNewest(QStringLiteral("26.1.2")).fileName));

            // 1.21.4 那一节：预览版 J4_pre2/J4_pre1 在前、正式版 J3 在后 —— 两档在这一节上
            // 分开（和 1.8.9 同理，只是这里的是"预览版比正式版更新"的现代版本）。
            check(pickFromPage(QStringLiteral("1.21.4")).fileName ==
                      QStringLiteral("OptiFine_1.21.4_HD_U_J3.jar") &&
                      pickNewest(QStringLiteral("1.21.4")).fileName ==
                          QStringLiteral("preview_OptiFine_1.21.4_HD_U_J4_pre2.jar"),
                  "V8.4 1.21.4 稳定档 J3 / 最新档 J4_pre2",
                  QStringLiteral("%1 / %2")
                      .arg(pickFromPage(QStringLiteral("1.21.4")).fileName,
                           pickNewest(QStringLiteral("1.21.4")).fileName));

            // 页面上根本没有的游戏版本：**不许**从整页里随便挑一个 —— 那会装出一个别的游戏
            // 版本的 OptiFine，而用户以为装的是他要的那个。
            check(!pickFromPage(QStringLiteral("1.99")).ok() &&
                      !pickFromPage(QString()).ok() &&
                      !pickNewest(QStringLiteral("1.99")).ok(),
                  "V8.4 页面上没有的版本 -> 两档都挑不出来，不是「随便挑一个」");
        }

        // --- V8.5 镜像的版本表：按发布时间（`_id` 那 4 个字节）从新到旧 -------------
        QFile mirrorFile(metaDir + "/optifine-1.18.2-list.json");
        QByteArray mirrorBytes;
        if (mirrorFile.open(QIODevice::ReadOnly)) {
            mirrorBytes = mirrorFile.readAll();
        }
        const QJsonArray mirrorList = QJsonDocument::fromJson(mirrorBytes).array();
        check(mirrorList.size() == 11, "V8.5 镜像那份真数据读到了（11 条）",
              QString("%1 条").arg(mirrorList.size()));
        if (!mirrorList.isEmpty()) {
            const LoaderMeta::OptiFinePick mirrorPick =
                LoaderMeta::pickOptifineFromMirror(mirrorList, QStringLiteral("1.18.2"));
            check(mirrorPick.ok() &&
                      mirrorPick.fileName == QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar") &&
                      mirrorPick.type == QStringLiteral("HD_U") &&
                      mirrorPick.patch == QStringLiteral("H9"),
                  "V8.5 1.18.2 的镜像表稳定档 -> H9", QStringLiteral("%1 type=%2 patch=%3")
                      .arg(mirrorPick.fileName, mirrorPick.type, mirrorPick.patch));

            // 稳定档挑出来的那条，type/patch 用的是**镜像自己那两个字段**（不是从版本号切
            // 出来的）—— 那既是从下载地址的唯一来源，也钉住了"挑法和列表走同一份取数"。
            const QString lastFile =
                mirrorList.last().toObject().value(QStringLiteral("filename")).toString();
            check(LoaderMeta::isOptifinePreviewFile(lastFile),
                  "V8.5 那份数组的最后一条是预览版（所以「取最后一条」那个错做法会挑中它）",
                  lastFile);

            // 1.18.2 这份数据上 H9 正式版就是最新发布的那一条，所以两档挑出来是同一个；
            // 真正的"两档分开"在 1.21.4 那一份上（见下面 V8.5c）。
            check(LoaderMeta::pickOptifineFromMirror(mirrorList, QStringLiteral("1.18.2"),
                                                     LoaderMeta::LoaderChannel::Newest)
                      .fileName == QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar"),
                  "V8.5 1.18.2 最新档也是 H9（H9 就是这份数据里最后发出来的那条）");

            // 全是预览版（正式的还没发）-> **稳定档挑不出来、最新档挑得出来**。
            QJsonArray previewsOnly;
            for (const QJsonValue &value : mirrorList) {
                if (LoaderMeta::isOptifinePreviewFile(
                        value.toObject().value(QStringLiteral("filename")).toString())) {
                    previewsOnly.append(value);
                }
            }
            check(!previewsOnly.isEmpty() &&
                      !LoaderMeta::pickOptifineFromMirror(previewsOnly, QStringLiteral("1.18.2"))
                           .ok(),
                  "V8.5 只剩预览版时稳定档挑不出来",
                  QString("%1 条预览版").arg(previewsOnly.size()));
            check(LoaderMeta::pickOptifineFromMirror(previewsOnly, QStringLiteral("1.18.2"),
                                                     LoaderMeta::LoaderChannel::Newest)
                      .fileName == QStringLiteral("preview_OptiFine_1.18.2_HD_U_H9_pre3.jar"),
                  "V8.5 同一批数据最新档挑得出预览版（H9_pre3，2023-03-26 那条）");

            // 缺字段 / 文件名不能用的条目一律跳过：别让一条烂数据把整次安装带偏（挑不出来，
            // 而不是"挑一条烂的"）。
            const QJsonArray junk{
                QJsonObject{{QStringLiteral("filename"),
                             QStringLiteral("OptiFine_1.18.2_HD_U_X1.jar")}},
                QJsonObject{{QStringLiteral("filename"), QStringLiteral("../evil.jar")},
                            {QStringLiteral("type"), QStringLiteral("HD_U")},
                            {QStringLiteral("patch"), QStringLiteral("X1")}},
                QJsonObject{{QStringLiteral("filename"),
                             QStringLiteral("OptiFine_1.18.2_HD_U_X2.jar")},
                            {QStringLiteral("type"), QString()},
                            {QStringLiteral("patch"), QStringLiteral("X2")}}};
            check(!LoaderMeta::pickOptifineFromMirror(junk, QStringLiteral("1.18.2")).ok(),
                  "V8.5 缺 type/patch 的、文件名带路径的 -> 全跳过");
            check(!LoaderMeta::pickOptifineFromMirror(QJsonArray(), QStringLiteral("1.18.2")).ok(),
                  "V8.5 空数组 -> 挑不出来（快照那种「还没支持」的版本）");
        }

        // --- V8.5b 镜像那份数组**不是按时间排的**，是按 (type, patch) 字典序 ---------
        //
        // 1.21.1 那 8 条是最好的证据：数组里 `pre10, pre11, … pre15` 排在 `pre9` **前面**
        // （字典序），而时间上 pre9 是最老的（09-04）、pre15 是最新的（09-23）——
        // 所以"把数组倒过来"这个老做法在这份真数据上挑出来的是**最老**的那条预览版。
        {
            QFile f(metaDir + "/optifine-1.21.1-list.json");
            QByteArray bytes;
            if (f.open(QIODevice::ReadOnly)) {
                bytes = f.readAll();
            }
            const QJsonArray arr = QJsonDocument::fromJson(bytes).array();
            check(arr.size() == 8, "V8.5b 1.21.1 的镜像数据读到了（8 条）",
                  QString("%1 条").arg(arr.size()));
            const QString firstPatch =
                arr.first().toObject().value(QStringLiteral("patch")).toString();
            const QString lastPatch =
                arr.last().toObject().value(QStringLiteral("patch")).toString();
            check(firstPatch == QStringLiteral("J1") && lastPatch == QStringLiteral("pre9"),
                  "V8.5b 数组首尾是 J1 / pre9（字典序，不是时间序）",
                  QStringLiteral("%1 … %2").arg(firstPatch, lastPatch));

            const LoaderMeta::LoaderVersionList list =
                LoaderMeta::listOptifineVersionsFromMirror(arr, QStringLiteral("1.21.1"));
            check(LoaderMeta::loaderVersionStrings(list) ==
                      (QStringList{QStringLiteral("HD_U_J1"), QStringLiteral("HD_U_J1_pre15"),
                                   QStringLiteral("HD_U_J1_pre14"), QStringLiteral("HD_U_J1_pre13"),
                                   QStringLiteral("HD_U_J1_pre12"), QStringLiteral("HD_U_J1_pre11"),
                                   QStringLiteral("HD_U_J1_pre10"),
                                   QStringLiteral("HD_U_J1_pre9")}),
                  "V8.5b 列表按发布时间从新到旧（J1 09-24 → pre15 09-23 → … → pre9 09-04）",
                  LoaderMeta::loaderVersionStrings(list).join(QStringLiteral(", ")));
            // 反证：数组倒过来（老做法）得到的是 pre9 打头 —— 全份数据里最老的那条。
            QStringList reversed = LoaderMeta::loaderVersionStrings(list);
            std::reverse(reversed.begin(), reversed.end());
            check(reversed.first() == QStringLiteral("HD_U_J1_pre9") &&
                      list.first().version == QStringLiteral("HD_U_J1"),
                  "★ V8.5b 倒过来是 pre9 打头（最老），按 `_id` 排是 J1 打头（最新）");
            check(list.value(0).stable && !list.value(1).stable,
                  "V8.5b 正式版标记只在那条正式版上（J1），预览版那 7 条都是 false");
            check(LoaderMeta::pickOptifineFromMirror(arr, QStringLiteral("1.21.1"),
                                                     LoaderMeta::LoaderChannel::Newest)
                          .fileName == QStringLiteral("OptiFine_1.21.1_HD_U_J1.jar") &&
                      LoaderMeta::pickOptifineFromMirror(arr, QStringLiteral("1.21.1")).fileName ==
                          QStringLiteral("OptiFine_1.21.1_HD_U_J1.jar"),
                  "V8.5b 1.21.1 上两档挑出来是同一个（正式版 J1 反而是最后发的那条）");
        }

        // --- V8.5c 镜像源上"预览版比正式版新"的版本：两档分开 --------------------
        //
        // 1.21.4 那一份：J4_pre2、J4_pre1（02-19）都**比正式版 J3（02-09）新**，
        // 而且这份数据里 18 条只有 1 条正式版 —— 正是"最新档装预览版、稳定档装正式版"的
        // 真实场景（1.21.4 的正式版 J4 到 都还没发）。
        {
            QFile f(metaDir + "/optifine-1.21.4-list.json");
            QByteArray bytes;
            if (f.open(QIODevice::ReadOnly)) {
                bytes = f.readAll();
            }
            const QJsonArray arr = QJsonDocument::fromJson(bytes).array();
            const LoaderMeta::LoaderVersionList list =
                LoaderMeta::listOptifineVersionsFromMirror(arr, QStringLiteral("1.21.4"));
            check(arr.size() == 18 && list.size() == 18,
                  "V8.5c 1.21.4 的镜像数据读到了（18 条，一条都没被跳过）",
                  QStringLiteral("%1 / %2").arg(arr.size()).arg(list.size()));
            int stableCount = 0;
            for (const LoaderMeta::LoaderVersionEntry &entry : list) {
                stableCount += entry.stable ? 1 : 0;
            }
            check(stableCount == 1 && list.value(2).version == QStringLiteral("HD_U_J3") &&
                      list.value(2).stable,
                  "V8.5c 18 条里只有 1 条正式版（J3），它排在第 3 位（前两条是更新的预览版）",
                  QStringLiteral("正式版 %1 条，第 3 条是 %2")
                      .arg(stableCount)
                      .arg(list.value(2).version));
            check(list.value(0).version == QStringLiteral("HD_U_J4_pre2") &&
                      list.value(1).version == QStringLiteral("HD_U_J4_pre1"),
                  "V8.5c 头两条是 J4 的两个预览版（它们比正式版 J3 新）",
                  QStringLiteral("%1, %2").arg(list.value(0).version, list.value(1).version));
            const LoaderMeta::OptiFinePick newest =
                LoaderMeta::pickOptifineFromMirror(arr, QStringLiteral("1.21.4"),
                                                   LoaderMeta::LoaderChannel::Newest);
            const LoaderMeta::OptiFinePick stablePick =
                LoaderMeta::pickOptifineFromMirror(arr, QStringLiteral("1.21.4"));
            check(newest.fileName == QStringLiteral("preview_OptiFine_1.21.4_HD_U_J4_pre2.jar") &&
                      stablePick.fileName == QStringLiteral("OptiFine_1.21.4_HD_U_J3.jar"),
                  "★ V8.5c 1.21.4 上两档挑出不同版本（最新 J4_pre2 预览版 / 稳定 J3 正式版）",
                  QStringLiteral("%1 / %2").arg(newest.fileName, stablePick.fileName));
        }

        // --- V8.6 广告页 -> 真下载地址（官方那条路多出来的那一步）-----------------
        QFile adFile(metaDir + "/optifine-adloadx.html");
        QByteArray ad;
        if (adFile.open(QIODevice::ReadOnly)) {
            ad = adFile.readAll();
        }
        check(!ad.isEmpty(), "V8.6 真广告页读到了", QString("%1 字节").arg(ad.size()));
        if (!ad.isEmpty()) {
            const QString adHtml = QString::fromUtf8(ad);
            const QString h7 = LoaderMeta::optifineJarUrlFromAdPage(
                adHtml, QStringLiteral("OptiFine_1.18.2_HD_U_H7.jar"));
            check(h7 == QStringLiteral("https://optifine.net/downloadx?f=OptiFine_1.18.2_HD_U_H7.jar"
                                       "&x=cf26b5f8b42616a53a7c935f403ec643"),
                  "V8.6 页面里那条**相对**链接被补成了绝对地址（含 token）", h7);

            // 我们要的是 H9，而页面上两条都是 H7：**不猜**。猜的代价是下一个不知道是什么版本
            // 的 jar —— 而"看起来能下"恰恰是这种错最难被发现的地方。
            check(LoaderMeta::optifineJarUrlFromAdPage(
                      adHtml, QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar"))
                      .isEmpty(),
                  "V8.6 页面上没有我们要的那个文件名（又是两条）-> 空，不猜");
        }
        // 页面里**只有一条**链接时就直接用它 —— 实测那个 token 和文件名**不绑定**（拿 H7 的
        // token 去请求 H9，下下来的照样是 6.7 MB 的 H9 jar），所以"只有一条"是可以信的。
        // NOTE: 这一条是**自造**的一条链接（上面那两条才是真页面）—— 真页面里凑不出"只有一条
        // 而且文件名对不上"的形状，但那条分支在真实请求里会走到（页面结构变简单时）。
        check(LoaderMeta::optifineJarUrlFromAdPage(
                  QStringLiteral("<a href=\"downloadx?f=OptiFine_1.18.2_HD_U_H9.jar"
                                 "&x=0123456789abcdef\">"),
                  QStringLiteral("OptiFine_1.18.2_HD_U_H9.jar")) ==
                  QStringLiteral("https://optifine.net/downloadx?f=OptiFine_1.18.2_HD_U_H9.jar"
                                 "&x=0123456789abcdef"),
              "V8.6 只有一条链接时直接用（token 不跟文件名绑定）");
        check(LoaderMeta::optifineJarUrlFromAdPage(QString(), QStringLiteral("x.jar")).isEmpty() &&
                  LoaderMeta::optifineJarUrlFromAdPage(
                      QStringLiteral("<a href=\"downloadx?f=a.jar&x=zz\">"),
                      QStringLiteral("a.jar"))
                      .isEmpty(),
              "V8.6 没有链接 / token 不像十六进制 -> 空（不往下猜地址）");

        // --- V8.7 装完之后"认目录名"用的那个形状 ---------------------------------
        //
        // 真装出来的是 `versions/1.18.2-OptiFine_HD_U_H9/`，而这个名字**不是我们拼的**
        // （安装器按自己那份 jar 里的版本号拼的）—— 所以那一步是"装完去盘上认"，认出来之后
        // 用它切出版本号写进 wtfml.meta。这一条钉住那个切法：
        {
            const QString id = QStringLiteral("1.18.2-OptiFine_HD_U_H9");
            const QString prefix = QStringLiteral("1.18.2") + QStringLiteral("-OptiFine_");
            check(LoaderMeta::isSafeVersionId(id) && id.startsWith(prefix) &&
                      id.mid(prefix.size()) == QStringLiteral("HD_U_H9"),
                  "V8.7 安装器装出来的目录名能当版本 id，切出来就是 HD_U_H9");
        }
    }

    // --- V9. Quilt：meta 地址 / 版本排序 / 稳定档 / profile 检查 / 安装支持 ---------
    {
        check(LoaderMeta::quiltLoaderListUrl(QStringLiteral("1.21.1")) ==
                  QStringLiteral("https://meta.quiltmc.org/v3/versions/loader/1.21.1"),
              "V9 Quilt 版本列表地址", LoaderMeta::quiltLoaderListUrl(QStringLiteral("1.21.1")));
        check(LoaderMeta::quiltProfileUrl(QStringLiteral("1.21.1"), QStringLiteral("0.26.4")) ==
                  QStringLiteral("https://meta.quiltmc.org/v3/versions/loader/1.21.1/0.26.4/profile/json"),
              "V9 Quilt profile 地址");
        check(LoaderMeta::quiltLoaderId(QStringLiteral("1.21.1"), QStringLiteral("0.26.4")) ==
                  QStringLiteral("quilt-loader-0.26.4-1.21.1"),
              "V9 Quilt 版本 id 形状");

        const auto quiltEntry = [](const QString &version) {
            return QJsonObject{{QStringLiteral("loader"),
                                QJsonObject{{QStringLiteral("version"), version}}}};
        };
        // 实测列表不是按版本号排的（0.20.0-beta.9 会排在 0.26.4 前面），所以这里必须
        // 自己按点分数字排序；stable 按“没有 - / + 后缀”判。
        const QJsonArray quiltList{quiltEntry(QStringLiteral("0.20.0-beta.9")),
                                   quiltEntry(QStringLiteral("0.26.4")),
                                   quiltEntry(QStringLiteral("0.24.0")),
                                   quiltEntry(QStringLiteral("0.23.0"))};
        const auto sorted = LoaderMeta::listQuiltLoaderVersions(quiltList);
        const auto stable = LoaderMeta::pickQuiltLoader(quiltList, LoaderMeta::LoaderChannel::Stable);
        const auto newest = LoaderMeta::pickQuiltLoader(quiltList, LoaderMeta::LoaderChannel::Newest);
        check(sorted.size() == 4 && sorted.first().version == QStringLiteral("0.26.4")
                  && stable.ok() && stable.version == QStringLiteral("0.26.4") && stable.stable
                  && newest.ok() && newest.version == QStringLiteral("0.26.4"),
              "V9 Quilt 列表按版本号新到旧排；稳定档挑最高的正式版",
              sorted.isEmpty() ? QString() : sorted.first().version);
        const QJsonArray betaOnly{quiltEntry(QStringLiteral("0.20.0-beta.9"))};
        check(!LoaderMeta::pickQuiltLoader(betaOnly, LoaderMeta::LoaderChannel::Stable).ok()
                  && LoaderMeta::pickQuiltLoader(betaOnly, LoaderMeta::LoaderChannel::Newest).ok(),
              "V9 只有测试版时稳定档挑不出来（不把 beta 伪装成正式版），最新档仍可选");

        VersionManager::LoaderKind kind = VersionManager::LoaderKind::Fabric;
        check(VersionManager::loaderKindForDependencyKey(ModpackImport::quiltLoaderKey(), &kind)
                  && kind == VersionManager::LoaderKind::Quilt
                  && VersionManager::loaderDisplayName(kind) == QStringLiteral("Quilt")
                  && VersionManager::loaderInstallSupported(kind),
              "V9 quilt-loader 键映射到 Quilt，且安装路径已接通");
        check(ModpackImport::unsupportedLoaderText(ModpackImport::quiltLoaderKey(),
                                                   QStringLiteral("0.26.4")).isEmpty(),
              "V9 Quilt 不再是整合包导入的“不支持”加载器");

        const QJsonObject profile{{QStringLiteral("id"), QStringLiteral("quilt-loader-0.26.4-1.21.1")},
                                  {QStringLiteral("inheritsFrom"), QStringLiteral("1.21.1")},
                                  {QStringLiteral("mainClass"), QStringLiteral("org.quiltmc.loader.impl.launch.knot.KnotClient")}};
        check(LoaderMeta::checkFabricProfile(profile, QStringLiteral("1.21.1")).ok()
                  && !LoaderMeta::checkFabricProfile(profile, QStringLiteral("1.20.1")).ok(),
              "V9 Quilt profile 复用同一套 id/父版本检查");

        // 重复版本号与混合测试版/正式版的解析排序（兼容版本 1.20.1 形态）
        const QJsonArray dupAndMixed{quiltEntry(QStringLiteral("0.27.0-beta.1")),
                                     quiltEntry(QStringLiteral("0.26.4")),
                                     quiltEntry(QStringLiteral("0.26.4")),
                                     quiltEntry(QStringLiteral("0.26.3")),
                                     quiltEntry(QString())};
        const auto mixedList = LoaderMeta::listQuiltLoaderVersions(dupAndMixed);
        check(mixedList.size() == 3 &&
                  mixedList.at(0).version == QStringLiteral("0.27.0-beta.1") && !mixedList.at(0).stable &&
                  mixedList.at(1).version == QStringLiteral("0.26.4") && mixedList.at(1).stable &&
                  mixedList.at(2).version == QStringLiteral("0.26.3") && mixedList.at(2).stable,
              "V9 Quilt 列表去重、过滤空串、区分稳定版与测试版并按新到旧排序");
        const auto mixedStable = LoaderMeta::pickQuiltLoader(dupAndMixed, LoaderMeta::LoaderChannel::Stable);
        const auto mixedNewest = LoaderMeta::pickQuiltLoader(dupAndMixed, LoaderMeta::LoaderChannel::Newest);
        check(mixedStable.ok() && mixedStable.version == QStringLiteral("0.26.4") && mixedStable.stable &&
                  mixedNewest.ok() && mixedNewest.version == QStringLiteral("0.27.0-beta.1") && !mixedNewest.stable,
              "V9 Quilt 稳定档跳过顶部 beta 选 0.26.4，最新档选 0.27.0-beta.1");

        // 不兼容的游戏版本：Quilt meta 回 HTTP 404 + {"code":"not_found"}，Fabric 回 HTTP 400 + []
        check(LoaderMeta::listQuiltLoaderVersions(QJsonArray()).isEmpty() &&
                  !LoaderMeta::pickQuiltLoader(QJsonArray(), LoaderMeta::LoaderChannel::Newest).ok(),
              "V9 Quilt 空列表时不兼容版本解析为空且挑不出版本");
        const QByteArray quiltNotFoundBody =
            QByteArrayLiteral("{\"code\":\"not_found\",\"message\":\"File with such name does not exist.\",\"status\":404}");
        check(LoaderMeta::isUnsupportedGameVersionMetaReply(true, 404, quiltNotFoundBody) &&
                  LoaderMeta::isUnsupportedGameVersionMetaReply(true, 200, QByteArrayLiteral("[]")) &&
                  LoaderMeta::isUnsupportedGameVersionMetaReply(false, 400, QByteArrayLiteral("[]")),
              "V9 Quilt 404 not_found 与 Fabric 400 [] 判为「游戏版本不支持」而非网络失败");
        check(!LoaderMeta::isUnsupportedGameVersionMetaReply(true, 500, quiltNotFoundBody) &&
                  !LoaderMeta::isUnsupportedGameVersionMetaReply(true, 404, QByteArrayLiteral("<html>404</html>")) &&
                  !LoaderMeta::isUnsupportedGameVersionMetaReply(true, 404, QByteArray()),
              "V9 500 / HTML 错误页 / 空正文不被误判为版本不支持");
    }
}


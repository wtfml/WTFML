#include "Support.hpp"

void runServerInstallRunTests() {
    section("ZY. 服务端安装那四段：还差哪一份元数据 / 取回来怎么读");

    // ---------- ZY0. 哪一档这一轮接没接 ----------
    {
        check(!ServerInstallRun::flavorSupported(ServerInstall::Flavor::Spigot),
              "★★ ZY0 Spigot 这一轮**没接**（官方不发成品，要用 BuildTools 现编）");
        int supported = 0;
        bool invariant = true;
        for (const ServerInstall::Flavor flavor : ServerInstall::allFlavors()) {
            if (ServerInstallRun::flavorSupported(flavor)) {
                ++supported;
            }
            // 两句话必须是**同一件事**：没接的档一定说得出为什么，接了的档一个字都不说。
            // 漂开的症状是"界面上写着暂不支持，可「创建」是能点的"。
            if (ServerInstallRun::unsupportedReason(flavor).isEmpty()
                == !ServerInstallRun::flavorSupported(flavor)) {
                invariant = false;
            }
        }
        check(supported == 7, "★ ZY0 八档里接了七档", QString::number(supported));
        check(invariant,
              "★★ ZY0 「接没接」和「有没有那句原因」永远是同一件事（两处判据不许漂开）");
        const QString reason =
            ServerInstallRun::unsupportedReason(ServerInstall::Flavor::Spigot);
        check(reason.contains(QStringLiteral("Git")),
              "★ ZY0 那句话里点明了**为什么要本机装 git**（用户得知道卡在哪儿）", reason);
        check(reason.contains(QStringLiteral("暂不支持此安装方式")) &&
                  reason.contains(QStringLiteral("选择其他服务端类型")),
              "ZY0 说明当前版本不支持此安装方式，并引导选择其他类型", reason);
    }

    // ---------- ZY1. 这一档还差哪一份元数据 ----------
    {
        const ServerInstall::Request vanilla = [] {
            ServerInstall::Request request;
            request.flavor = ServerInstall::Flavor::Vanilla;
            request.gameVersion = QStringLiteral("1.20.1");
            return request;
        }();
        const ServerInstall::Plan vanillaPlan = ServerInstall::planFor(vanilla);
        check(vanillaPlan.ok, "ZY1 原版这一档的计划本身是成立的", vanillaPlan.problem);
        const ServerInstallRun::Need vanillaNeed =
            ServerInstallRun::needFor(vanilla, vanillaPlan);
        check(vanillaNeed.kind == ServerInstallRun::Metadata::VersionJson,
              "★ ZY1 原版要的是那一份**版本 json**");
        check(vanillaNeed.url.isEmpty(),
              "★★ ZY1 版本 json 特意**不给地址**——先在盘上找（装过客户端就有），没有才联网");
        check(vanillaNeed.label.contains(QStringLiteral("原版")),
              "ZY1 那一步的名字说得出是干什么的", vanillaNeed.label);

        ServerInstall::Request paper = vanilla;
        paper.flavor = ServerInstall::Flavor::Paper;
        const ServerInstall::Plan paperPlan = ServerInstall::planFor(paper);
        check(paperPlan.ok, "ZY1 Paper 这一档的计划也是成立的", paperPlan.problem);
        const ServerInstallRun::Need paperNeed = ServerInstallRun::needFor(paper, paperPlan);
        check(paperNeed.kind == ServerInstallRun::Metadata::PaperBuilds,
              "★ ZY1 Paper 要的是**构建列表**");
        check(paperNeed.url == ServerInstall::paperBuildsUrl(QStringLiteral("1.20.1")),
              "★★ ZY1 那一条地址就是 `paperBuildsUrl()`（不在这儿另拼一遍）", paperNeed.url);

        // 剩下那五档的地址在 `planFor()` 里就拼好了 —— 一个请求都不用发。
        bool restQuiet = true;
        for (const ServerInstall::Flavor flavor :
             {ServerInstall::Flavor::Purpur, ServerInstall::Flavor::Fabric,
              ServerInstall::Flavor::Forge, ServerInstall::Flavor::NeoForge,
              ServerInstall::Flavor::Quilt}) {
            ServerInstall::Request request;
            request.flavor = flavor;
            request.gameVersion = QStringLiteral("1.20.1");
            request.loaderVersion = QStringLiteral("47.2.0");
            request.installerVersion = QStringLiteral("1.0.1");
            request.buildId = QStringLiteral("1234");
            const ServerInstall::Plan plan = ServerInstall::planFor(request);
            if (!plan.ok || ServerInstallRun::needFor(request, plan).ok()) {
                restQuiet = false;
                out() << "        （没安静下来的是 " << ServerInstall::flavorName(flavor) << "）\n";
            }
        }
        check(restQuiet,
              "★★ ZY1 另外五档一份元数据都不差（地址在计划里就拼好了 ⇒ 少一次往返）");

        const ServerInstall::Plan bad = ServerInstall::planFor(ServerInstall::Request{});
        check(!bad.ok && !ServerInstallRun::needFor(ServerInstall::Request{}, bad).ok(),
              "ZY1 计划本身不成立时，不问元数据（先把那句问题解决掉）");
    }

    // ---------- ZY2. 原版：版本 json 怎么读 ----------
    {
        ServerInstall::Request request;
        request.flavor = ServerInstall::Flavor::Vanilla;
        request.gameVersion = QStringLiteral("1.20.1");
        const ServerInstall::Plan plan = ServerInstall::planFor(request);

        const QByteArray versionJson = R"({
            "id": "1.20.1",
            "downloads": {
                "server": {
                    "sha1": "0d2b1e6b2b0e1f3b6dcb0a1f6b4a3d94d1f9a3b2",
                    "size": 51272711,
                    "url": "https://piston-data.mojang.com/v1/objects/0d2b…/server.jar"
                }
            }
        })";
        const ServerInstallRun::Fill fill =
            ServerInstallRun::fillDownloads(request, plan, versionJson);
        check(fill.ok, "ZY2 一份正常的版本 json 读得下来", fill.problem);
        check(fill.plan.downloads.size() == 1,
              "★ ZY2 填完正好一份要下的东西", QString::number(fill.plan.downloads.size()));
        if (fill.plan.downloads.size() == 1) {
            const ServerInstall::Download jar = fill.plan.downloads.first();
            check(jar.fileName == QStringLiteral("server.jar"),
                  "★★ ZY2 落地的名字是 `server.jar`（上游那个名字我们不用）", jar.fileName);
            check(jar.url.contains(QStringLiteral("piston-data.mojang.com")),
                  "ZY2 地址就是 json 里那一条", jar.url);
            check(jar.size == 51272711,
                  "★ ZY2 大小照抄（原版这条**有** size ⇒ 进度算得出来百分比）",
                  QString::number(jar.size));
        }
        check(fill.detail.isEmpty(),
              "ZY2 原版这一步没有小字要说（它没有「构建号」这种东西）");

        // 远古版本：官方压根没发过服务端。这不是"下载失败"，得说清楚。
        const ServerInstallRun::Fill ancient = ServerInstallRun::fillDownloads(
            request, plan, QByteArray("{\"id\":\"a1.1.2a\",\"downloads\":{}}"));
        check(!ancient.ok, "★★ ZY2 版本 json 里没有 `downloads.server` ⇒ 不成立");
        check(ancient.problem.contains(QStringLiteral("1.2.5")),
              "★★ ZY2 而且那句说的是「官方没发过」（不是「网络失败了」）—— 用户该做的是换个版本",
              ancient.problem);

        const ServerInstallRun::Fill garbage =
            ServerInstallRun::fillDownloads(request, plan, QByteArray("<html>404</html>"));
        check(!garbage.ok && garbage.problem.contains(QStringLiteral("JSON"), Qt::CaseInsensitive),
              "★ ZY2 回来的不是 json ⇒ 说「无法解析」（别硬解）", garbage.problem);
    }

    // ---------- ZY3. Paper：构建列表怎么读 ----------
    {
        ServerInstall::Request request;
        request.flavor = ServerInstall::Flavor::Paper;
        request.gameVersion = QStringLiteral("1.21.4");
        // 起手那一份计划里 `downloads` 是**空的**（地址里带着对象 sha256，拼不出来）。
        const ServerInstall::Plan plan = ServerInstall::planFor(request);
        check(plan.ok && plan.downloads.isEmpty(),
              "★ ZY3 Paper 起手那份计划就是要联网现查地址的那种");

        // 上游回的是**升序**（老的在前），而这里给的是"232 是 ALPHA、231 是 STABLE"。
        const QByteArray builds = R"([
            {"id": 231, "channel": "STABLE", "downloads": {"server:default": {
                "name": "paper-1.21.4-231.jar",
                "url": "https://api.papermc.io/v2/projects/paper/versions/1.21.4/builds/231/downloads/paper-1.21.4-231.jar",
                "checksums": {"sha256": "aaaa"}, "size": 49000000}}},
            {"id": 232, "channel": "ALPHA", "downloads": {"server:default": {
                "name": "paper-1.21.4-232.jar",
                "url": "https://api.papermc.io/v2/projects/paper/versions/1.21.4/builds/232/downloads/paper-1.21.4-232.jar",
                "checksums": {"sha256": "bbbb"}, "size": 49100000}}}
        ])";
        const ServerInstallRun::Fill fill =
            ServerInstallRun::fillDownloads(request, plan, builds);
        check(fill.ok, "ZY3 构建列表读得下来", fill.problem);
        check(fill.plan.downloads.size() == 1,
              "★★ ZY3 纸面上一共有两条构建，真正要下的是**一条**（挑出来的那一条）",
              QString::number(fill.plan.downloads.size()));
        if (fill.plan.downloads.size() == 1) {
            const ServerInstall::Download jar = fill.plan.downloads.first();
            check(jar.fileName == QStringLiteral("server.jar"),
                  "ZY3 落地名字照旧是 `server.jar`", jar.fileName);
            check(jar.url.contains(QStringLiteral("/builds/231/")),
                  "★★ ZY3 挑的是**稳定档**那一条（231），不是最新的那条 ALPHA", jar.url);
            check(jar.sha256 == QStringLiteral("aaaa"),
                  "★ ZY3 sha256 照抄（Paper 只给 sha256，没有 sha1）", jar.sha256);
        }
        check(fill.detail.contains(QStringLiteral("231")) &&
                  fill.detail.contains(QStringLiteral("稳定档")),
              "★★ ZY3 那一步收尾时的小字把**构建号**说出来（用户得知道装的是哪一条）",
              fill.detail);

        // 上游没标稳定档时：退回最新那条，但**必须说出来** —— 不然用户以为手上这个是稳定版。
        const QByteArray onlyAlpha = R"([
            {"id": 231, "channel": "ALPHA", "downloads": {"server:default": {
                "name": "paper-1.21.4-231.jar", "url": "https://example.invalid/231.jar",
                "checksums": {"sha256": "cccc"}, "size": 1}}},
            {"id": 232, "channel": "ALPHA", "downloads": {"server:default": {
                "name": "paper-1.21.4-232.jar", "url": "https://example.invalid/232.jar",
                "checksums": {"sha256": "dddd"}, "size": 1}}}
        ])";
        const ServerInstallRun::Fill alpha =
            ServerInstallRun::fillDownloads(request, plan, onlyAlpha);
        check(alpha.ok && alpha.plan.downloads.size() == 1 &&
                  alpha.plan.downloads.first().url.endsWith(QStringLiteral("232.jar")),
              "★★ ZY3 一条稳定档都没有 ⇒ 退回最新那条（不是「一条都下不了」）",
              alpha.problem);
        check(alpha.detail.contains(QStringLiteral("没标稳定档")),
              "★★ ZY3 而且那句小字**明说**这不是稳定档（用户不能以为手上这个是稳的）",
              alpha.detail);

        const ServerInstallRun::Fill none =
            ServerInstallRun::fillDownloads(request, plan, QByteArray("[]"));
        check(!none.ok && none.problem.contains(QStringLiteral("没给这个游戏版本发过构建")),
              "★ ZY3 空列表 ⇒ 明说「这个版本没有构建」（换个版本，不是重试）", none.problem);

        const ServerInstallRun::Fill wrongShape =
            ServerInstallRun::fillDownloads(request, plan, QByteArray("{\"a\":1}"));
        check(!wrongShape.ok, "ZY3 回来的不是数组 ⇒ 不成立", wrongShape.problem);
    }

    // ---------- ZY4. 其余那几档：地址已经拼好了，一个字节都不用读 ----------
    {
        for (const ServerInstall::Flavor flavor :
             {ServerInstall::Flavor::Purpur, ServerInstall::Flavor::Fabric,
              ServerInstall::Flavor::Forge, ServerInstall::Flavor::NeoForge,
              ServerInstall::Flavor::Quilt}) {
            ServerInstall::Request request;
            request.flavor = flavor;
            request.gameVersion = QStringLiteral("1.20.1");
            request.loaderVersion = QStringLiteral("47.2.0");
            request.installerVersion = QStringLiteral("1.0.1");
            const ServerInstall::Plan plan = ServerInstall::planFor(request);
            if (!plan.ok) {
                check(false, "ZY4 这一档的计划该是成立的", ServerInstall::flavorName(flavor));
                continue;
            }
            // 喂一份**垃圾**进去：这一档根本不该读它。
            const ServerInstallRun::Fill fill = ServerInstallRun::fillDownloads(
                request, plan, QByteArray("<html>这不是 json</html>"));
            // 逐项比（`Download` 没有 `operator==`）：条数一样、每一条的地址和落地名字都一样。
            bool same = fill.ok && fill.plan.downloads.size() == plan.downloads.size();
            for (int i = 0; same && i < plan.downloads.size(); ++i) {
                same = fill.plan.downloads.at(i).url == plan.downloads.at(i).url
                       && fill.plan.downloads.at(i).fileName
                              == plan.downloads.at(i).fileName;
            }
            check(same,
                  "★★ ZY4 计划里地址齐了的档：喂什么进来都原样出去（一个字节都不读）",
                  ServerInstall::flavorName(flavor));
        }
    }

    // ---------- ZY5 / ZY6. 跑安装器那条命令行 ----------
    {
        ServerInstall::Request forge;
        forge.flavor = ServerInstall::Flavor::Forge;
        forge.gameVersion = QStringLiteral("1.20.1");
        forge.loaderVersion = QStringLiteral("47.2.0");
        const ServerInstall::Plan forgePlan = ServerInstall::planFor(forge);
        check(forgePlan.ok, "ZY5 Forge 这份计划成立", forgePlan.problem);

        const QString jarName = ServerInstallRun::installerJarName(forgePlan);
        check(!jarName.isEmpty(), "★★ ZY5 Forge 要跑安装器（判据 = 计划里给没给工具参数）");
        check(jarName == forgePlan.downloads.first().fileName,
              "★ ZY5 要跑的就是计划里下下来的**第一份**", jarName);

        const QStringList command =
            ServerInstallRun::toolCommand(forgePlan, QStringLiteral("C:/srv/") + jarName);
        check(command.size() == 2 + forgePlan.toolArguments.size(),
              "★ ZY5 命令行 = `-jar` + jar 路径 + 计划里那些参数", command.join(QLatin1Char(' ')));
        check(command.size() >= 2 && command.at(0) == QStringLiteral("-jar") &&
                  command.at(1).endsWith(jarName),
              "★★ ZY5 头两个是 `-jar <那个 jar 的**完整路径**>`（相对路径起不来）",
              command.join(QLatin1Char(' ')));
        check(command.contains(QStringLiteral("--installServer")),
              "★ ZY5 Forge 那几条参数原样接在后面", command.join(QLatin1Char(' ')));
        check(ServerInstallRun::toolCommand(forgePlan, QString()).isEmpty(),
              "★ ZY5 没给 jar 路径 ⇒ 空命令行（调用方不用再判一次）");

        // 不跑安装器的那几档：`installerJarName()` 得回空。
        bool quiet = true;
        for (const ServerInstall::Flavor flavor :
             {ServerInstall::Flavor::Vanilla, ServerInstall::Flavor::Paper,
              ServerInstall::Flavor::Purpur, ServerInstall::Flavor::Fabric}) {
            ServerInstall::Request request;
            request.flavor = flavor;
            request.gameVersion = QStringLiteral("1.20.1");
            request.loaderVersion = QStringLiteral("0.15.0");
            request.installerVersion = QStringLiteral("1.0.1");
            const ServerInstall::Plan plan = ServerInstall::planFor(request);
            if (!ServerInstallRun::installerJarName(plan).isEmpty()) {
                quiet = false;
                out() << "        （多说话的是 " << ServerInstall::flavorName(flavor) << "）\n";
            }
        }
        check(quiet,
              "★ ZY6 下下来就是服务端的那几档：不跑安装器（这几档起手就该直接核对）");

        // 这一条是那一层自己的不变式：判据是"有没有工具参数"，不是"是哪一档"。
        bool sameYesNo = true;
        for (const ServerInstall::Flavor flavor : ServerInstall::allFlavors()) {
            ServerInstall::Request request;
            request.flavor = flavor;
            request.gameVersion = QStringLiteral("1.20.1");
            request.loaderVersion = QStringLiteral("47.2.0");
            request.installerVersion = QStringLiteral("1.0.1");
            request.buildId = QStringLiteral("231");
            const ServerInstall::Plan plan = ServerInstall::planFor(request);
            if (ServerInstallRun::installerJarName(plan).isEmpty()
                != plan.toolArguments.isEmpty()) {
                sameYesNo = false;
            }
        }
        check(sameYesNo,
              "★★ ZY6 「要不要跑安装器」和「有没有工具参数」永远是同一件事（八档全过一遍）");
    }

    // ---------- ZY7. 这件事分成几步 ----------
    {
        ServerInstall::Request vanilla;
        vanilla.flavor = ServerInstall::Flavor::Vanilla;
        vanilla.gameVersion = QStringLiteral("1.20.1");
        const ServerInstall::Plan vanillaPlan = ServerInstall::planFor(vanilla);
        // 起手那一份（元数据还没取回来）：两步 —— 去要那一份 + 最后核对。
        const QStringList startSteps = ServerInstallRun::stepNames(vanilla, vanillaPlan);
        check(startSteps.size() == 2 && startSteps.first().contains(QStringLiteral("原版")),
              "★★ ZY7 原版起手先摆两步（去要版本信息 + 核对）", startSteps.join(QLatin1Char('|')));
        check(startSteps.last() == QStringLiteral("核对安装结果"),
              "★★ ZY7 最后一步永远是「核对安装结果」—— 它是「不留半成品」那句话的落点",
              startSteps.last());

        // 元数据取回来之后：三步（多出那条下载）。
        const QByteArray versionJson =
            R"({"downloads":{"server":{"url":"https://example.invalid/server.jar","sha1":"","size":1}}})";
        const ServerInstallRun::Fill filled =
            ServerInstallRun::fillDownloads(vanilla, vanillaPlan, versionJson);
        check(filled.ok, "ZY7 前置：那一份填好了", filled.problem);
        const QStringList steps = ServerInstallRun::stepNames(vanilla, filled.plan);
        check(steps.size() == 3, "★ ZY7 填完之后多出那条下载（一共三步）",
              steps.join(QLatin1Char('|')));
        bool named = !steps.isEmpty();
        for (const QString &step : steps) {
            if (step.trimmed().isEmpty()) {
                named = false;
            }
        }
        check(named, "★ ZY7 每一格都有名字（任务中心那一行不能是空的）");

        ServerInstall::Request forge;
        forge.flavor = ServerInstall::Flavor::Forge;
        forge.gameVersion = QStringLiteral("1.20.1");
        forge.loaderVersion = QStringLiteral("47.2.0");
        const ServerInstall::Plan forgePlan = ServerInstall::planFor(forge);
        const QStringList forgeSteps = ServerInstallRun::stepNames(forge, forgePlan);
        check(forgeSteps.contains(QStringLiteral("跑安装器")),
              "★★ ZY7 要跑安装器的档多一格「跑安装器」（用户得看得见机器在干活）",
              forgeSteps.join(QLatin1Char('|')));
        check(forgeSteps.indexOf(QStringLiteral("跑安装器")) == forgeSteps.size() - 2,
              "★ ZY7 它在核对那一步**前面**（顺序：下载 → 跑安装器 → 核对）",
              forgeSteps.join(QLatin1Char('|')));

        // Paper 起手那一份：元数据 + 核对，两步（地址还不知道，下载那几格也还不知道）。
        ServerInstall::Request paper;
        paper.flavor = ServerInstall::Flavor::Paper;
        paper.gameVersion = QStringLiteral("1.21.4");
        const QStringList paperSteps =
            ServerInstallRun::stepNames(paper, ServerInstall::planFor(paper));
        check(paperSteps.size() == 2 && paperSteps.first().contains(QStringLiteral("Paper")),
              "★ ZY7 Paper 起手也是两步（先问构建列表）", paperSteps.join(QLatin1Char('|')));
    }
}


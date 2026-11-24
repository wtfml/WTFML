#include "Support.hpp"
#include "launcher/RunningProcesses.hpp"

void runServerRunTests() {
    section("ZZ. 服务端跑起来 / 停下来：命令怎么拼、起来没有、宽限多久");

    // ---------- ZZ0. 那条启动命令怎么拼 ----------
    {
        ServerInstall::Request vanillaReq;
        vanillaReq.flavor = ServerInstall::Flavor::Vanilla;
        vanillaReq.gameVersion = QStringLiteral("1.20.1");
        const ServerInstall::Plan vanilla = ServerInstall::planFor(vanillaReq);
        check(vanilla.ok && !vanilla.launchTokens.isEmpty(), "★★ ZZ0 前置：原版那份计划是好的",
              vanilla.problem);

        // 带内存：接在最前面（原版这一档的 index 是 0）。
        const QStringList withMem = ServerRun::launchArguments(vanilla, 4096);
        check(withMem.size() == vanilla.launchTokens.size() + 1,
              "★ ZZ0 内存那一项只**多出一个** token（没把原来的抄一遍也没丢）",
              withMem.join(QLatin1Char(' ')));
        check(withMem.value(0) == QStringLiteral("-Xmx4096m"),
              "★★ ZZ0 `-Xmx` 接在最前面（index 0 那一档）", withMem.join(QLatin1Char(' ')));
        check(withMem.mid(1) == vanilla.launchTokens,
              "★ ZZ0 原来那串 token 原样跟在后面、一个不差", withMem.join(QLatin1Char(' ')));

        // `<= 0` 就一项都不发（"不限制"是 java 的默认档；编一个数出来比不发更危险 ——
        // 同实例设置那条规则）。
        for (const int memory : {0, -1, -4096}) {
            const QStringList none = ServerRun::launchArguments(vanilla, memory);
            bool hasXmx = false;
            for (const QString &token : none) {
                if (token.contains(QStringLiteral("-Xmx"))) {
                    hasXmx = true;
                }
            }
            check(!hasXmx && none == vanilla.launchTokens,
                  QStringLiteral("★★ ZZ0 内存 %1 ⇒ 一个 `-Xmx` 都不发（原封不动）").arg(memory),
                  none.join(QLatin1Char(' ')));
        }

        // 别的 JVM 参数排在内存那几项**后面**，但仍然在同一个插入位置上。
        const QStringList extra = ServerRun::launchArguments(
            vanilla, 2048, QStringList{QStringLiteral("-XX:+UseG1GC")});
        check(extra.size() == vanilla.launchTokens.size() + 2
                  && extra.value(0) == QStringLiteral("-Xmx2048m")
                  && extra.value(1) == QStringLiteral("-XX:+UseG1GC"),
              "★★ ZZ0 额外的 JVM 参数排在内存那几项后面", extra.join(QLatin1Char(' ')));

        // 只给了别的参数、没给内存：那就只插它一个。
        const QStringList onlyExtra = ServerRun::launchArguments(
            vanilla, 0, QStringList{QStringLiteral("-XX:+UseG1GC")});
        check(onlyExtra.value(0) == QStringLiteral("-XX:+UseG1GC")
                  && onlyExtra.mid(1) == vanilla.launchTokens,
              "★ ZZ0 没给内存 ⇒ 只有额外那一项被插进来", onlyExtra.join(QLatin1Char(' ')));

        // 插在越界的那一位上：钳到两头，token 一个都不许丢。
        ServerInstall::Plan past = vanilla;
        past.launchJvmArgsIndex = 99;
        const QStringList tail = ServerRun::launchArguments(past, 1024);
        check(tail.size() == past.launchTokens.size() + 1
                  && tail.last() == QStringLiteral("-Xmx1024m"),
              "★ ZZ0 给的位子过了头 ⇒ 钳到最后一位（不丢 token）", tail.join(QLatin1Char(' ')));
        past.launchJvmArgsIndex = -7;
        const QStringList head = ServerRun::launchArguments(past, 1024);
        check(head.value(0) == QStringLiteral("-Xmx1024m"),
              "★ ZZ0 给的位子是负数 ⇒ 钳到最前面", head.join(QLatin1Char(' ')));
    }

    // ---------- ZZ0b. ★ Forge / NeoForge：内存必须夹在两张参数文件中间 ----------
    {
        ServerInstall::Request forgeReq;
        forgeReq.flavor = ServerInstall::Flavor::Forge;
        forgeReq.gameVersion = QStringLiteral("1.20.1");
        forgeReq.loaderVersion = QStringLiteral("47.2.0");
        const ServerInstall::Plan forge = ServerInstall::planFor(forgeReq);
        check(forge.ok, "ZZ0b 前置：Forge 那份计划是好的", forge.problem);

        ServerInstall::Request neoReq;
        neoReq.flavor = ServerInstall::Flavor::NeoForge;
        neoReq.gameVersion = QStringLiteral("1.20.1");
        neoReq.loaderVersion = QStringLiteral("20.4.237");
        const ServerInstall::Plan neo = ServerInstall::planFor(neoReq);
        check(neo.ok, "ZZ0b 前置：NeoForge 那份计划是好的", neo.problem);

        for (const ServerInstall::Plan &plan : {forge, neo}) {
            const QString name = ServerInstall::flavorName(plan.flavor);
            const QStringList args = ServerRun::launchArguments(plan, 4096);
            const QString joined = args.join(QLatin1Char(' '));

            check(args.size() == 4, QStringLiteral("★ ZZ0b %1 四个 token").arg(name), joined);
            check(args.value(0) == QStringLiteral("@user_jvm_args.txt"),
                  QStringLiteral("★★ ZZ0b %1 第一位是那张写着默认内存的参数文件").arg(name),
                  joined);

            const int memAt = args.indexOf(QStringLiteral("-Xmx4096m"));
            int mainAt = -1;  // 自带主类的那一张（`@…/win_args.txt`）
            for (int i = 0; i < args.size(); ++i) {
                if (args.at(i).startsWith(QLatin1Char('@'))
                    && args.at(i).endsWith(QStringLiteral("win_args.txt"))) {
                    mainAt = i;
                }
            }
            const int noguiAt = args.indexOf(QStringLiteral("nogui"));

            check(memAt == 1, QStringLiteral("★★ ZZ0b %1 `-Xmx` 在第二位（两张文件中间）").arg(name),
                  joined);
            check(mainAt == 2 && noguiAt == 3,
                  QStringLiteral("★★ ZZ0b %1 自带主类那张在 `-Xmx` **之后**").arg(name), joined);
            // 这一条就是那个坑本身：`-Xmx` 一旦跑到主类后面，它就成了"给服务端的参数"。
            check(memAt >= 0 && mainAt >= 0 && memAt < mainAt,
                  QStringLiteral("★★ ZZ0b %1 `-Xmx` 不许落在主类后面（落了就等于没发）")
                      .arg(name),
                  joined);
        }
    }

    // ---------- ZZ0c. 七档都过一遍：那个"插第几位"的表不许漂 ----------
    {
        struct Sample {
            ServerInstall::Request request;
            int expectIndex;
        };
        QList<Sample> samples;
        auto add = [&samples](ServerInstall::Flavor flavor, const QString &game,
                              const QString &loader, const QString &installer, int expectIndex) {
            ServerInstall::Request req;
            req.flavor = flavor;
            req.gameVersion = game;
            req.loaderVersion = loader;
            req.installerVersion = installer;
            samples.append(Sample{req, expectIndex});
        };
        add(ServerInstall::Flavor::Vanilla, QStringLiteral("1.20.1"), QString(), QString(), 0);
        add(ServerInstall::Flavor::Paper, QStringLiteral("1.21.4"), QString(), QString(), 0);
        add(ServerInstall::Flavor::Purpur, QStringLiteral("1.21.4"), QString(), QString(), 0);
        add(ServerInstall::Flavor::Fabric, QStringLiteral("1.20.1"), QStringLiteral("0.15.0"),
            QStringLiteral("1.0.1"), 0);
        add(ServerInstall::Flavor::Forge, QStringLiteral("1.20.1"), QStringLiteral("47.2.0"),
            QString(), 1);
        add(ServerInstall::Flavor::NeoForge, QStringLiteral("1.20.1"), QStringLiteral("20.4.237"),
            QString(), 1);
        add(ServerInstall::Flavor::Quilt, QStringLiteral("1.20.1"), QString(), QStringLiteral("0.9.0"),
            0);

        int ok = 0;
        bool indicesOk = true;
        bool atOk = true;
        bool sizeOk = true;
        for (const Sample &sample : samples) {
            const ServerInstall::Plan plan = ServerInstall::planFor(sample.request);
            if (!plan.ok) {
                continue;
            }
            ++ok;
            if (plan.launchJvmArgsIndex != sample.expectIndex) {
                indicesOk = false;
            }
            // 两张 `@` 参数文件是 Forge / NeoForge 那两档的记号：有它俩的档 index 必须是 1，
            // 没有的必须是 0。这条把"表"和"token 的形状"绑在一起 —— 将来加一档忘了填
            // 这一格，这儿就红。
            const bool hasArgFiles =
                !plan.launchTokens.isEmpty() && plan.launchTokens.first().startsWith(QLatin1Char('@'));
            if (hasArgFiles != (plan.launchJvmArgsIndex == 1)) {
                atOk = false;
            }
            if (ServerRun::launchArguments(plan, 2048).size() != plan.launchTokens.size() + 1) {
                sizeOk = false;
            }
        }
        check(ok == samples.size(), "★ ZZ0c 前置：七档的计划全都拼出来了（Spigot 不在里面）",
              QStringLiteral("%1/%2").arg(ok).arg(samples.size()));
        check(indicesOk, "★★ ZZ0c 每一档的 `launchJvmArgsIndex` 都对（0 一堆，Forge/NeoForge 是 1）");
        check(atOk, "★★ ZZ0c 「两张 `@` 参数文件」和「index 是 1」永远是同一件事");
        check(sizeOk, "★ ZZ0c 七档都只多出一个 `-Xmx` token");
    }

    // ---------- ZZ1. 让它停下来那一句 / 「起来了吗」 ----------
    {
        const QByteArray stop = ServerRun::stopCommandBytes();
        check(stop == QByteArrayLiteral("stop\n"), "★★ ZZ1 正常停 = 写一行 `stop`", QString::fromLatin1(stop));
        check(stop.endsWith('\n'), "★★ ZZ1 末尾那个换行**不能少**（不带回车它读到 EOF 才动）");
        check(!stop.contains('\r'), "★ ZZ1 没有多余的回车（服务端那边是按行读的）");

        bool readyAll = true;
        for (const char *line :
             {"Done (12.345s)! For help, type \"help\"",
              "[12:00:00] [Server thread/INFO]: Done (1.234s)! For help, type \"help\"",
              "[Server thread/INFO]: Done (0.001s)!"}) {
            if (!ServerRun::isReadyLine(QString::fromLatin1(line))) {
                readyAll = false;
            }
        }
        check(readyAll, "★★ ZZ1 五档都那一种 `Done (…)` 认得出来（原版/Paper/Fabric/Forge/Neo）");

        bool notReady = true;
        for (const char *line :
             {"Done loading 512 recipes",
              "[Server thread/INFO]: Preparing level \"world\"",
              "Done",
              "",
              "Starting minecraft server version 1.20.1"}) {
            if (ServerRun::isReadyLine(QString::fromLatin1(line))) {
                notReady = false;
            }
        }
        check(notReady, "★★ ZZ1 别的句子一个都不许混进来（尤其那句光秃秃的 Done）");
    }

    // ---------- ZZ2. 退了之后算哪一档 ----------
    {
        check(ServerRun::statusAfterExit(0, true) == ServerModel::Status::Stopped,
              "★★ ZZ2 退出码 0 ⇒ 已停止（我们发的 stop 生效了）");
        check(ServerRun::statusAfterExit(0, false) == ServerModel::Status::Stopped,
              "★★ ZZ2 还没说停它就 0 退出了，也算已停止（eula 没同意 / 端口被占就走这条，"
              "报成「异常退出」等于让用户去查一个不存在的崩溃）");
        check(ServerRun::statusAfterExit(1, false) == ServerModel::Status::Crashed,
              "★★ ZZ2 非 0 ⇒ 异常退出");
        check(ServerRun::statusAfterExit(1, true) == ServerModel::Status::Crashed,
              "★★ ZZ2 我们叫它停、它却非 0 退出 ⇒ 仍然是异常退出");
        check(ServerRun::statusAfterExit(-1, false) == ServerModel::Status::Crashed,
              "★ ZZ2 `FailedToStart` 那一路（−1）也算异常退出");
    }

    // ---------- ZZ3. 宽限多久才允许强杀（那张表的唯一出处） ----------
    {
        constexpr qint64 kGiB = 1024LL * 1024LL * 1024LL;
        check(ServerRun::gracefulStopMs(0, 0) == 15'000,
              "★★ ZZ3 空目录空存档 = 起手 15 秒", QString::number(ServerRun::gracefulStopMs(0, 0)));

        check(ServerRun::gracefulStopSeconds(0, 0) == 15, "★ ZZ3 秒数就是毫秒除以 1000");

        // 模组那一头：每 32 个 +5 秒。
        check(ServerRun::gracefulStopSeconds(32, 0) == 20, "★★ ZZ3 32 个模组 ⇒ 20 秒",
              QString::number(ServerRun::gracefulStopSeconds(32, 0)));
        check(ServerRun::gracefulStopSeconds(64, 0) == 25, "★ ZZ3 64 个模组 ⇒ 25 秒",
              QString::number(ServerRun::gracefulStopSeconds(64, 0)));
        check(ServerRun::gracefulStopSeconds(31, 0) == 15,
              "★ ZZ3 不到一步的不算（31 个还是 15 秒 —— 别四舍五入到 20）");
        // 封顶 +45 秒。
        check(ServerRun::gracefulStopSeconds(100'000, 0) == 60,
              "★★ ZZ3 几百个模组也只加到封顶那一档（15 + 45 = 60 秒）",
              QString::number(ServerRun::gracefulStopSeconds(100'000, 0)));

        // 存档那一头：每 1 GiB +5 秒，不足 1 GiB 也算一步。
        check(ServerRun::gracefulStopSeconds(0, kGiB) == 20, "★★ ZZ3 1 GiB 存档 ⇒ 20 秒",
              QString::number(ServerRun::gracefulStopSeconds(0, kGiB)));
        check(ServerRun::gracefulStopSeconds(0, 1) == 20,
              "★★ ZZ3 空世界但只要**有**这个世界也按一步算（1 字节和 1 GiB 同级？—— 这里"
              "刻意如此：目录在就说明有东西要存）",
              QString::number(ServerRun::gracefulStopSeconds(0, 1)));
        check(ServerRun::gracefulStopSeconds(0, kGiB / 2) == 20,
              "★ ZZ3 半个 GiB 也算一步（不然 0.9 GiB 和一个空目录一个待遇）");
        check(ServerRun::gracefulStopSeconds(0, 2 * kGiB) == 25, "★ ZZ3 2 GiB ⇒ 25 秒");
        // 封顶 +60 秒。
        check(ServerRun::gracefulStopSeconds(0, 20 * kGiB) == 75,
              "★★ ZZ3 20 GiB 也只加到封顶那一档（15 + 60 = 75 秒）",
              QString::number(ServerRun::gracefulStopSeconds(0, 20 * kGiB)));

        // 两头都拉满 ⇒ 总上限 120 秒。
        check(ServerRun::gracefulStopSeconds(100'000, 50 * kGiB) == 120,
              "★★ ZZ3 两头都拉满 ⇒ 总上限 120 秒",
              QString::number(ServerRun::gracefulStopSeconds(100'000, 50 * kGiB)));

        // 两个方向的单调：东西越多只会等得越久，不会反而更短。
        bool monotone = true;
        int prevMods = 0;
        int prevWorld = 0;
        for (const int mods : {0, 1, 31, 32, 100, 500, 5000}) {
            const int ms = ServerRun::gracefulStopMs(mods, 0);
            if (ms < prevMods) {
                monotone = false;
            }
            prevMods = ms;
        }
        for (const qint64 bytes : {0LL, 1LL, kGiB, 5 * kGiB, 100 * kGiB}) {
            const int ms = ServerRun::gracefulStopMs(0, bytes);
            if (ms < prevWorld) {
                monotone = false;
            }
            prevWorld = ms;
        }
        check(monotone, "★★ ZZ3 模组更多 / 存档更大 ⇒ 等的时间只增不减");
        check(ServerRun::gracefulStopMs(-3, -7) == 15'000,
              "★ ZZ3 负数（读不出来 / 没扫到）按 0 走，不是「扣掉 5 秒」");
    }

    // ---------- ZZ4. 给用户看的那几句 ----------
    {
        const QString stopping = ServerRun::stoppingNote(20);
        check(stopping.contains(QStringLiteral("20")), "★★ ZZ4 「停止中」那句把秒数写出来", stopping);
        const QString force = ServerRun::forceKillNote(20);
        check(force.contains(QStringLiteral("20")) && force.contains(QStringLiteral("存档")),
              "★★ ZZ4 「可以强制结束了」那句把秒数写出来，并且**说清楚代价**（可能丢存档）", force);

        const QString missing = ServerRun::notInstalledMessage();
        check(!missing.isEmpty() && missing.contains(QStringLiteral("修复")),
              "★★ ZZ4 没装好那句指向「修复」那一颗（不能只说「装不了」）", missing);

        check(ServerRun::eulaUrl() == QStringLiteral("https://aka.ms/MinecraftEULA"),
              "★★ ZZ4 EULA 原文地址（可点那一行的落点）", ServerRun::eulaUrl());
        check(ServerRun::eulaUrl().startsWith(QStringLiteral("https://")),
              "★ ZZ4 是 https —— 用户点开之前那一行得能看出这是外链");
        check(!ServerRun::eulaLinkText().isEmpty()
                  && ServerRun::eulaLinkText().contains(QStringLiteral("许可协议"))
                  && ServerRun::eulaLinkText().contains(QStringLiteral("打开")),
              "★★ ZZ4 那一行可点链接的文字（用户点名要的）—— 是一句「打开…」，"
              "不是一个光秃秃的网址",
              ServerRun::eulaLinkText());

        check(ServerRun::eulaCheckBoxText().contains(QStringLiteral("我已阅读并同意")),
              "★★ ZZ4 勾选框那句是**第一人称的声明**（「我读过并同意」），不是「本协议没问题」",
              ServerRun::eulaCheckBoxText());

        const QString text = ServerRun::eulaPromptText();
        check(text.contains(QStringLiteral("eula=true")),
              "★★ ZZ4 EULA 那道闸说清楚它到底会写什么", text);
        check(text.contains(QStringLiteral("仅在勾选协议并选择「同意并启动」后")),
              "★★ ZZ4 而且说清楚**不自动接受**", text);
        check(!ServerRun::eulaPromptTitle().isEmpty(), "★ ZZ4 那道闸有标题");

        for (const QString &s : {ServerRun::eulaPromptTitle(), ServerRun::eulaPromptText(),
                                 ServerRun::eulaCheckBoxText(), ServerRun::eulaLinkText(),
                                 ServerRun::notInstalledMessage(), stopping, force}) {
            if (s.trimmed().isEmpty()) {
                check(false, "★★ ZZ4 这几句话一句都不许是空的");
            }
        }
    }

    // ---------- ZZ5. 管道里的字节怎么切成行（★ 一个多字节字符被劈成两半的那一路） ----------
    {
        // --- 一趟能凑几行：切出完整的行、剩下半行留到下趟 ---
        {
            QByteArray pending;
            const QStringList first = ServerRun::takeLines(pending, QByteArrayLiteral("a\nb\ncc"));
            check(first == QStringList{QStringLiteral("a"), QStringLiteral("b")},
                  "★★ ZZ5 一趟到手三行里有两整行 + 半行 ⇒ 只报那两条", first.join(QLatin1Char('|')));
            check(pending == QByteArrayLiteral("cc"),
                  "★★ ZZ5 那半行**原样**留在攒着的那截里（不是丢掉也不是先解码）",
                  QString::fromLatin1(pending));

            const QStringList second = ServerRun::takeLines(pending, QByteArrayLiteral("c\nd\n"));
            check(second == QStringList{QStringLiteral("ccc"), QStringLiteral("d")},
                  "★★ ZZ5 攒着的那截和这一趟的头一个字**拼**成一行（跨趟拼行是常态，不是边角）",
                  second.join(QLatin1Char('|')));
            check(pending.isEmpty(), "★ ZZ5 吃干净之后攒着的那截要空（不然下次会重复报）");
        }

        // --- ★★ 多字节字符被切断：这是原来那版会红的那一路 ---
        //
        // 造字节要用**本地代码页**，不是 UTF-8 —— `takeLines()` 解码走的是
        // `fromLocal8Bit`（服务端 stdout 的编码），原来的 `.toUtf8()` 只在"本地代码页
        // 恰好是 UTF-8"的机器上才自洽。英文版 Windows 的代码页是单字节的（cp1252），
        // 那儿根本没有多字节字符可切，这一路无从量起 —— 如实跳过，不假装量过。
        {
            const QString wide =
                QStringLiteral("[12:00:02] [Server thread/INFO]: [回声] say 你好");
            const QByteArray wideBytes = wide.toLocal8Bit();
            const bool wideLocal = QString::fromLocal8Bit(wideBytes) == wide;
            const bool localMultibyte = wideLocal && wideBytes.size() > wide.size();
            if (!localMultibyte) {
                check(true,
                      "★★ ZZ5 本机代码页是单字节的（英文版 Windows）⇒ 没有多字节字符可切，"
                      "这一段如实跳过");
            } else {
                const QString text = wide;
                const QByteArray whole = text.toLocal8Bit() + QByteArrayLiteral("\n");

                // 切在最后那个「好」的字节中间（末尾 `\n` 之前第 2 个字节上）。
                // 本地代码页是 UTF-8 时那是后续字节；cp936 时是双字节字符的后半。
                const int lastCharBytes = QStringLiteral("好").toLocal8Bit().size();
                const int cutAt = whole.size() - 1 - (lastCharBytes / 2);
                // 前置：切点落在最后那个字符的中间（不是它的第一个字节）。
                check(cutAt > whole.size() - 1 - lastCharBytes,
                      "★★ ZZ5 前置：这一刀真的切在一个多字节字符**中间**");

                QByteArray pending;
                const QStringList half = ServerRun::takeLines(pending, whole.left(cutAt));
                check(half.isEmpty(),
                      "★★ ZZ5 切在字符中间时**一行都不许报**（原来那版会在这儿解出两个乱码、"
                      "当成一整行报上去 —— T41.3 时红时绿就是它）",
                      half.join(QLatin1Char('|')));

                const QStringList rest = ServerRun::takeLines(pending, whole.mid(cutAt));
                check(rest.size() == 1 && rest.first() == text,
                      "★★ ZZ5 后半截一到手，拼出来的是**原来那句话**（一个字符都不差）",
                      rest.join(QLatin1Char('|')));
                check(pending.isEmpty(), "★ ZZ5 拼完又吃干净了");
            }
        }

        // --- 一字节一字节地喂（最坏情况：每次 readyRead 只来一个字节） ---
        {
            const QString wideText = QStringLiteral("中文 漂 亮 🎉");
            const QString text =
                QString::fromLocal8Bit(wideText.toLocal8Bit()) == wideText
                    ? wideText
                    : QStringLiteral("ascii bytes");
            const QByteArray whole = text.toLocal8Bit() + QByteArrayLiteral("\n");
            QByteArray pending;
            QStringList got;
            for (int i = 0; i < whole.size(); ++i) {
                got += ServerRun::takeLines(pending, whole.mid(i, 1));
            }
            check(got.size() == 1 && got.first() == text,
                  "★★ ZZ5 一个字节一个字节喂（最碎的那种分包）也只出**一行**、内容不变",
                  got.join(QLatin1Char('|')));
            check(pending.isEmpty(), "★ ZZ5 碎喂之后也不该剩下东西");
        }

        // --- 两种行尾：`\n` 是真行尾，`\r` 是它重画进度那一行自己回的 ---
        {
            QByteArray pending;
            const QStringList crlf = ServerRun::takeLines(
                pending, QByteArrayLiteral("x\r\ny\r\n"));
            check(crlf == QStringList{QStringLiteral("x"), QStringLiteral("y")},
                  "★★ ZZ5 `\\r\\n`（Windows 那套）切成两行，**不许**多出一个空行"
                  "（行数一对不上，下游数行数的判据全跟着漂）",
                  crlf.join(QLatin1Char('|')));
            check(pending.isEmpty(), "★ ZZ5 CRLF 也吃干净");

            const QStringList cr = ServerRun::takeLines(
                pending, QByteArrayLiteral("...10%\r...30%\r"));
            check(cr == QStringList{QStringLiteral("...10%"), QStringLiteral("...30%")},
                  "★★ ZZ5 单独一个 `\\r`（进度条重画）也当行尾切 —— 不切的话会挂成一长条",
                  cr.join(QLatin1Char('|')));

            const QStringList blanks = ServerRun::takeLines(pending, QByteArrayLiteral("\n\n"));
            check(blanks.isEmpty(), "★★ ZZ5 空行不回来（服务端自己会打空行）",
                  blanks.join(QLatin1Char('|')));
            check(pending.isEmpty(), "★ ZZ5 空行也吃干净");
        }

        // --- 攒着的那截在"没等到行尾"时不许自己长大之后又被当成一行 ---
        {
            QByteArray pending;
            ServerRun::takeLines(pending, QByteArrayLiteral("no newline yet"));
            check(pending == QByteArrayLiteral("no newline yet"),
                  "★ ZZ5 迟迟没有行尾 ⇒ 全攒着（一个字都不丢）", QString::fromLatin1(pending));
            const QStringList done = ServerRun::takeLines(pending, QByteArrayLiteral("\n"));
            check(done.size() == 1 && done.first() == QStringLiteral("no newline yet"),
                  "★ ZZ5 行尾终于到了 ⇒ 攒着的那些一次性成行", done.join(QLatin1Char('|')));
        }
    }

    // ---------- ZZ6. 运行中进程混排（客户端 + 服务端）、计数与 key 区分 ----------
    {
        // 数字目录名的服务端不能和客户端会话号相撞
        const QString serverKey = RunningProcesses::makeServerKey(QStringLiteral("1"));
        check(serverKey != QStringLiteral("1"),
              "ZZ6 服务端面板行 key 带前缀，数字目录名不与客户端会话号相撞", serverKey);
        QString parsed;
        check(RunningProcesses::parseServerKey(serverKey, &parsed) && parsed == QStringLiteral("1"),
              "ZZ6 服务端 key 可反解回原 serverId", parsed);
        check(!RunningProcesses::parseServerKey(QStringLiteral("1"), &parsed)
                  && !RunningProcesses::parseServerKey(RunningProcesses::serverKeyPrefix(), &parsed),
              "ZZ6 客户端会话号与空 serverId 不被误判为服务端 key");

        RunningProcesses::Entry c1;
        c1.kind = RunningProcesses::Entry::Kind::Client;
        c1.sessionId = 1;
        c1.key = QStringLiteral("1");
        c1.displayName = QStringLiteral("1.20.1");
        c1.startedAtMs = 1000;

        RunningProcesses::Entry s1;
        s1.kind = RunningProcesses::Entry::Kind::Server;
        s1.serverId = QStringLiteral("survival");
        s1.key = RunningProcesses::makeServerKey(s1.serverId);
        s1.displayName = QStringLiteral("生存服");
        s1.startedAtMs = 2000;

        RunningProcesses::Entry c2;
        c2.kind = RunningProcesses::Entry::Kind::Client;
        c2.sessionId = 2;
        c2.key = QStringLiteral("2");
        c2.displayName = QStringLiteral("1.21.4");
        c2.startedAtMs = 3000;

        RunningProcesses::Entry s2;
        s2.kind = RunningProcesses::Entry::Kind::Server;
        s2.serverId = QStringLiteral("creative");
        s2.key = RunningProcesses::makeServerKey(s2.serverId);
        s2.displayName = QStringLiteral("创造服");
        s2.startedAtMs = 2500;

        QList<RunningProcesses::Entry> mixed{c1, s1, c2, s2};
        check(RunningProcesses::totalCount(mixed) == 4
                  && RunningProcesses::totalCount(2, 2) == 4
                  && RunningProcesses::totalCount(0, 1) == 1
                  && RunningProcesses::totalCount(0, 0) == 0,
              "ZZ6 进程合计数按客户端 + 服务端总和计算");

        RunningProcesses::sortNewestFirst(mixed);
        check(mixed.size() == 4
                  && mixed.at(0).key == QStringLiteral("2")
                  && mixed.at(1).key == RunningProcesses::makeServerKey(QStringLiteral("creative"))
                  && mixed.at(2).key == RunningProcesses::makeServerKey(QStringLiteral("survival"))
                  && mixed.at(3).key == QStringLiteral("1"),
              "★★ ZZ6 客户端与服务端按启动时间倒序混排（最新启动的排在最上面）",
              QStringLiteral("%1 > %2 > %3 > %4")
                  .arg(mixed.value(0).key, mixed.value(1).key,
                       mixed.value(2).key, mixed.value(3).key));

        // 同一毫秒内的两个客户端按更大 sessionId 排前（后启动的排上面）
        RunningProcesses::Entry cSameA = c1;
        cSameA.sessionId = 5;
        cSameA.key = QStringLiteral("5");
        cSameA.startedAtMs = 5000;
        RunningProcesses::Entry cSameB = c1;
        cSameB.sessionId = 8;
        cSameB.key = QStringLiteral("8");
        cSameB.startedAtMs = 5000;
        QList<RunningProcesses::Entry> sameMs{cSameA, cSameB};
        RunningProcesses::sortNewestFirst(sameMs);
        check(sameMs.at(0).sessionId == 8 && sameMs.at(1).sessionId == 5,
              "ZZ6 同毫秒启动的两个客户端按后发起的 sessionId 排在上面");
    }
}


#include "Support.hpp"
void runGameCrashTests() {
    const QString ledgerPath =
        AppPaths::defaultMinecraftDir() + QStringLiteral("/wtfml.tasks");
    const auto crashTasks = [&ledgerPath]() {
        QList<Tasks::TaskSnapshot> found;
        for (const Tasks::TaskSnapshot &task : Tasks::load(ledgerPath)) {
            if (task.kind == Tasks::TaskKind::Crash) {
                found.append(task);
            }
        }
        return found;
    };
    // 账本上**已经有的**那些号。挑"这一发新补出来的那一条"靠它，而不是靠位置 ——
    // 这一本账是同一个可执行文件里所有节一起写的、而且**跨运行留着**（见上面那一段），
    // 位置会随别的节写了什么而变。顺带把 `TaskHistory.hpp` 那句「第 0 条是最近的」也
    // 绕开了：按位置取的话，`last()` 拿到的是**最老**的那一条（地雷就在那儿）。
    const auto crashIds = [&ledgerPath]() {
        QSet<quint64> ids;
        for (const Tasks::TaskSnapshot &task : Tasks::load(ledgerPath)) {
            if (task.kind == Tasks::TaskKind::Crash) {
                ids.insert(task.id);
            }
        }
        return ids;
    };
    // 跑之前没有、跑之后有的那一条。正常只该有一条；真出了两条就按文件顺序取**先遇到的**
    // 那一条（第 0 条是最近的 ⇒ 先遇到的更近）。
    const auto newCrash = [&ledgerPath](const QSet<quint64> &known) {
        Tasks::TaskSnapshot found;
        for (const Tasks::TaskSnapshot &task : Tasks::load(ledgerPath)) {
            if (task.kind == Tasks::TaskKind::Crash && !known.contains(task.id)) {
                found = task;
                break;
            }
        }
        return found;
    };
    const int baseline = crashTasks().size();

    // 一个"假 Java"：只有 `release`（`probeJavaMajorVersion()` 认的就是它里面的
    // JAVA_VERSION），**不需要**一个能跑的 java.exe —— 那一步根本走不到。
    QTemporaryDir jdkRoot;
    check(jdkRoot.isValid(), "T34.0 临时目录建出来了（摆那个假 Java）");
    if (!jdkRoot.isValid()) {
        return;
    }
    const QDir jdk(jdkRoot.path());
    jdk.mkpath(QStringLiteral("jdk17/bin"));
    writeFileAt(jdk.filePath(QStringLiteral("jdk17/bin/java.exe")), QByteArrayLiteral("stub"));
    writeFileAt(jdk.filePath(QStringLiteral("jdk17/release")),
                QByteArrayLiteral("JAVA_VERSION=\"17.0.9\"\n"));
    const QString javaPath = jdk.filePath(QStringLiteral("jdk17/bin/java.exe"));
    check(QFileInfo::exists(javaPath), "T34.0 那个假 Java 摆在盘上了");

    // 离线身份：MainWindow 构造末尾那次皮肤加载要走本地，这一节一个请求都不发。
    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    const QString accountName = QStringLiteral("T34User");
    QString offlineError;
    check(AccountStore::addOffline(accountName, &offlineError),
          "T34.0 建一个离线账号并设成当前身份", offlineError);

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();

        // ---------- ① 崩溃那一趟：账本上补出一项 ----------
        //
        // 摆的现场是**三条提示凑齐**的那一种：要 Java 21 而实际用的是 17（两个数都有 ⇒
        // 说得出来）、崩在"坏内存"那一类上（装了加载器 ⇒ 口风指向模组）、以及这一趟
        // 压根没拿到游戏的输出（没有日志窗 ⇒ 明说少一份依据）。内存那一条**故意不摆**：
        // 它要跟这台机器的物理内存比，那个数在这里不可控（规则本身在 verify 的 ZP5）。
        const int crashSession = 4242;
        const QString crashInstance = QStringLiteral("1.199.1-T34崩溃");
        w.seedSessionFactsForTest(crashSession, crashInstance, QStringLiteral("T34崩溃目标"), 21,
                                  javaPath, 0, 0, QStringLiteral("Fabric"),
                                  QStringLiteral("0.15.11"));
        const QSet<quint64> beforeCrash = crashIds();
        const bool invoked =
            QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                      Q_ARG(int, crashSession), Q_ARG(int, -1073741819),
                                      Q_ARG(QProcess::ExitStatus, QProcess::CrashExit));
        check(invoked, "★ T34.1 onGameFinished 那一槽真的打进去了");

        QList<Tasks::TaskSnapshot> after = crashTasks();
        check(after.size() == baseline + 1,
              "★★ T34.1 崩溃退出 ⇒ 账本上**多出一项**「游戏异常退出」",
              QStringLiteral("%1 → %2").arg(baseline).arg(after.size()));
        if (after.size() == baseline + 1) {
            const Tasks::TaskSnapshot crash = newCrash(beforeCrash);
            check(crash.state == Tasks::TaskState::Failed,
                  "★ T34.1 它是**失败**收尾的（底栏那颗红点靠这个亮起来）");
            check(crash.title == QStringLiteral("游戏异常退出 / T34崩溃目标"),
                  "★ T34.1 标题里带着**用户认得的那个名字**", crash.title);
            check(crash.targetId == crashInstance,
                  "★ T34.1 targetId = 实例 id（「打开目标位置」那一颗要照它开目录）",
                  crash.targetId);
            // 那一句原因 = 概括 + 空行 + 那几条提示（并成**一整块**：「复制摘要」复制的就是它）。
            check(crash.errorSummary.startsWith(
                      QStringLiteral("「T34崩溃目标」崩溃退出（0xC0000005（内存访问冲突））。")),
                  "★★ T34.1 第一句说的是谁、怎么退的（退出码写成人话）",
                  crash.errorSummary.left(60));
            check(crash.errorSummary.contains(QStringLiteral("要求 Java 21"))
                      && crash.errorSummary.contains(QStringLiteral("实际使用的是 Java 17")),
                  "★★ T34.1 「Java 版本对不上」那一条在（两个数都是现算的：要的来自会话事实、"
                  "用的来自那个假 release 文件）",
                  crash.errorSummary);
            check(crash.errorSummary.contains(QStringLiteral("模组")),
                  "★ T34.1 装了加载器 ⇒ 崩在坏内存上那句话指向模组（不是「驱动」）",
                  crash.errorSummary);
            check(crash.errorSummary.contains(QStringLiteral("未生成日志")),
                  "★ T34.1 这一趟没有日志窗 ⇒ 明说少一份依据（而不是假装日志干净）");
        }

        // ---------- ② 以 0 正常退出：一项都不补 ----------
        //
        // 要求的是「游戏**异常**退出后…」—— 跑得好好的退出一项都不该冒出来。
        QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                  Q_ARG(int, 4243), Q_ARG(int, 0),
                                  Q_ARG(QProcess::ExitStatus, QProcess::NormalExit));
        check(crashTasks().size() == baseline + 1,
              "★★ T34.2 以 0 正常退出 ⇒ 一条都不补（判据是 isAbnormal()）",
              QString::number(crashTasks().size()));

        // ---------- ③ 非崩溃、但退出码非 0：也算异常 ----------
        const int quietSession = 4244;
        w.seedSessionFactsForTest(quietSession, QStringLiteral("1.199.2-T34安静"),
                                  QStringLiteral("T34安静退出"), 17, javaPath, 0, 0, QString(),
                                  QString());
        const QSet<quint64> beforeQuiet = crashIds();
        QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                  Q_ARG(int, quietSession), Q_ARG(int, 1),
                                  Q_ARG(QProcess::ExitStatus, QProcess::NormalExit));
        QList<Tasks::TaskSnapshot> quiet = crashTasks();
        check(quiet.size() == baseline + 2,
              "★ T34.3 非崩溃、但退出码是 1 ⇒ 也算异常退出，补一项",
              QString::number(quiet.size()));
        if (quiet.size() == baseline + 2) {
            const Tasks::TaskSnapshot added = newCrash(beforeQuiet);
            check(added.errorSummary.startsWith(
                      QStringLiteral("「T34安静退出」以非 0 退出码结束（1）。")),
                  "★ T34.3 这一档说的是「以非 0 退出码结束」，**不是**「崩溃退出」",
                  added.errorSummary.left(60));
        }

        // ---------- ④ 那一份启动事实是**用掉就删**的 ----------
        //
        // 盯着这一条是因为 `m_sessionLaunchFacts` 不清会随开过的实例数一直长（和
        // `m_sessionDisplayNames` 同一个理由）。同一个会话号再来一发，那一份已经没了 ⇒
        // 出来的就是"只知道退出码"的一份：名字退回没有、标题里那个实例也不在了。
        const QSet<quint64> beforeAgain = crashIds();
        QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                  Q_ARG(int, quietSession), Q_ARG(int, 1),
                                  Q_ARG(QProcess::ExitStatus, QProcess::NormalExit));
        QList<Tasks::TaskSnapshot> again = crashTasks();
        check(again.size() == baseline + 3,
              "★ T34.4 同一个会话号再来一发 ⇒ 又补一项（这一槽本身不认「这号跑过没有」）",
              QString::number(again.size()));
        if (again.size() == baseline + 3) {
            const Tasks::TaskSnapshot added = newCrash(beforeAgain);
            check(added.title == QStringLiteral("游戏异常退出")
                      && added.errorSummary.startsWith(
                          QStringLiteral("游戏以非 0 退出码结束（1）。")),
                  "★★ T34.4 这一发手上没有任何会话事实了（名字那一份**用掉就删**）—— "
                  "标题退回没有实例、那句概括也不带名字",
                  added.title);
        }

        // ---------- ⑤ 用户从任务中心主动停止：强杀形状也不算异常 ----------
        // Windows 上 QProcess::kill() 通常给 CrashExit / 非零码；这里钉的是“谁要求停的”，
        // 不能只看进程退出形状，否则用户自己的停止动作会制造一条失败任务和红点。
        const int stoppedSession = 4245;
        w.seedSessionFactsForTest(stoppedSession, QStringLiteral("1.199.3-T34主动停止"),
                                  QStringLiteral("T34主动停止"), 17, javaPath, 0, 0,
                                  QString(), QString());
        w.markGameStopRequestedForTest(stoppedSession);
        QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                  Q_ARG(int, stoppedSession), Q_ARG(int, -1),
                                  Q_ARG(QProcess::ExitStatus, QProcess::CrashExit));
        check(crashTasks().size() == baseline + 3,
              "★★ T34.5 用户主动停止 ⇒ 即使是 CrashExit / 非零码也不补异常退出任务",
              QString::number(crashTasks().size()));

        // ---------- ⑥ 出过话的崩溃：没有日志窗也要带着那段日志 ----------
        const int talkativeSession = 4246;
        w.seedSessionFactsForTest(talkativeSession, QStringLiteral("1.199.4-T34出过话"),
                                  QStringLiteral("T34出过话"), 17, javaPath, 0, 0,
                                  QStringLiteral("Fabric"), QStringLiteral("0.15.11"));
        QMetaObject::invokeMethod(&w, "onProcessOutput", Qt::DirectConnection,
                                  Q_ARG(int, talkativeSession),
                                  Q_ARG(QString, QStringLiteral("[12:00:00] [main/ERROR] boom\n")));
        const QSet<quint64> beforeTalkative = crashIds();
        QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                  Q_ARG(int, talkativeSession), Q_ARG(int, -1073741819),
                                  Q_ARG(QProcess::ExitStatus, QProcess::CrashExit));
        check(crashTasks().size() == baseline + 4,
              "★ T34.6 出过话的崩溃照样补一项",
              QString::number(crashTasks().size()));
        if (crashTasks().size() == baseline + 4) {
            const Tasks::TaskSnapshot added = newCrash(beforeTalkative);
            check(!added.errorSummary.contains(QStringLiteral("未生成日志")),
                  "★★ T34.6 进程输出有底账（和日志窗无关）⇒ 不再说「一行都没收到」",
                  added.errorSummary);
        }

        // ---------- ⑦ #151：退出码 0，但本次日志有 Forge 加载失败特征 → 判为异常 ----------
        const int forgeFailSession = 4247;
        w.seedSessionFactsForTest(forgeFailSession, QStringLiteral("1.20.1-T34Forge失败"),
                                  QStringLiteral("T34Forge加载失败"), 17, javaPath, 0, 0,
                                  QStringLiteral("Forge"), QStringLiteral("47.3.0"));
        QMetaObject::invokeMethod(
            &w, "onProcessOutput", Qt::DirectConnection, Q_ARG(int, forgeFailSession),
            Q_ARG(QString,
                  QStringLiteral("[15:50:52] [main/ERROR]: Missing or unsupported mandatory dependencies:\n"
                                 "net.minecraftforge.fml.ModLoadingException: Mod loading has failed\n")));
        const QSet<quint64> beforeForgeFail = crashIds();
        QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                  Q_ARG(int, forgeFailSession), Q_ARG(int, 0),
                                  Q_ARG(QProcess::ExitStatus, QProcess::NormalExit));
        check(crashTasks().size() == baseline + 5,
              "★★ T34.7 退出码 0 但日志含 Forge 加载失败特征 ⇒ 判为异常退出，补一项",
              QString::number(crashTasks().size()));
        if (crashTasks().size() == baseline + 5) {
            const Tasks::TaskSnapshot added = newCrash(beforeForgeFail);
            check(added.errorSummary.contains(QStringLiteral("模组加载失败并退出（退出码 0）"))
                      && added.errorSummary.contains(QStringLiteral("缺少模组依赖")),
                  "★ T34.7 摘要写明模组加载失败且退出码 0，并保留缺前置提示",
                  added.errorSummary);
        }

        // ---------- ⑧ #151：退出码 0，只有早于本次启动的旧崩溃报告 → 判为正常退出 ----------
        const QString reportInstance = QStringLiteral("1.20.1-T34崩溃目录");
        const QString reportCrashDir =
            AppPaths::versionDir(reportInstance) + QStringLiteral("/crash-reports");
        QDir().mkpath(reportCrashDir);
        const qint64 sessionStartMs = QDateTime::currentMSecsSinceEpoch();
        const QString oldCrashPath =
            reportCrashDir + QStringLiteral("/crash-2020-01-01_00.00.00-client.txt");
        {
            QFile f(oldCrashPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write("---- Minecraft Crash Report ----\n");
                f.setFileTime(QDateTime::fromMSecsSinceEpoch(sessionStartMs - 60000),
                                    QFileDevice::FileModificationTime);
                f.close();
            }
        }
        const int oldReportSession = 4248;
        w.seedSessionFactsForTest(oldReportSession, reportInstance,
                                  QStringLiteral("T34旧崩溃报告"), 17, javaPath, 0, 0,
                                  QStringLiteral("Forge"), QStringLiteral("47.3.0"),
                                  sessionStartMs);
        QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                  Q_ARG(int, oldReportSession), Q_ARG(int, 0),
                                  Q_ARG(QProcess::ExitStatus, QProcess::NormalExit));
        check(crashTasks().size() == baseline + 5,
              "★★ T34.8 退出码 0 且只有早于本次启动的旧崩溃报告 ⇒ 判为正常退出，不补异常任务",
              QString::number(crashTasks().size()));

        // ---------- ⑨ #151：退出码 0，本次启动后新生成崩溃报告 → 判为异常 ----------
        const QString newCrashPath =
            reportCrashDir + QStringLiteral("/crash-2026-11-27_00.00.00-client.txt");
        {
            QFile f(newCrashPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write("---- Minecraft Crash Report ----\n");
                f.setFileTime(QDateTime::fromMSecsSinceEpoch(sessionStartMs + 1000),
                                    QFileDevice::FileModificationTime);
                f.close();
            }
        }
        const int newReportSession = 4249;
        w.seedSessionFactsForTest(newReportSession, reportInstance,
                                  QStringLiteral("T34新崩溃报告"), 17, javaPath, 0, 0,
                                  QStringLiteral("Forge"), QStringLiteral("47.3.0"),
                                  sessionStartMs);
        QMetaObject::invokeMethod(&w, "onGameFinished", Qt::DirectConnection,
                                  Q_ARG(int, newReportSession), Q_ARG(int, 0),
                                  Q_ARG(QProcess::ExitStatus, QProcess::NormalExit));
        check(crashTasks().size() == baseline + 6,
              "★ T34.8b 退出码 0 但本次启动后新生成了崩溃报告 ⇒ 判为异常退出",
              QString::number(crashTasks().size()));
        QDir(AppPaths::versionDir(reportInstance)).removeRecursively();
    }

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
          "T34.9 这一节加的账号清干净了");
}


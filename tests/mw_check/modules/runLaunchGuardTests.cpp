#include "Support.hpp"
void runLaunchGuardTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "AJ0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }
    // 数据根目录**两头都要改**（理由见 AH0 那段：MainWindow 构造里会拿盘上那个「当前游戏
    // 文件夹」再把 AppPaths 改一次）。
    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());
    QDir().mkpath(root.path() + QStringLiteral("/libraries/占位"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));
    const QDir versionsDir(root.path() + QStringLiteral("/versions"));

    // 一个"真走得完启动路径"的实例：客户端 jar 要有（checkPreLaunch 要）、json 要有主类。
    const QString id = QStringLiteral("1.20.1");
    const QString customName = QStringLiteral("护栏测试包");
    const QDir instanceDir(versionsDir.filePath(id));
    instanceDir.mkpath(QStringLiteral("."));
    writeFileAt(instanceDir.filePath(id + QStringLiteral(".jar")), QByteArrayLiteral("stub"));
    writeFileAt(instanceDir.filePath(id + QStringLiteral(".json")),
                QByteArrayLiteral("{\"id\":\"1.20.1\",\"libraries\":[],"
                                  "\"mainClass\":\"net.minecraft.client.main.Main\","
                                  "\"javaVersion\":{\"majorVersion\":17}}"));
    // 假 java：**这个 exe 的一份副本** + 旁边一个 release 文件（和 AG0 同一套；没有
    // release 就会真的去跑一次 `java.exe -version` 探主版本）。
    const QDir fakeJavaDir(root.filePath(QStringLiteral("java/guard17")));
    fakeJavaDir.mkpath(QStringLiteral("bin"));
    const QString fakeJava = fakeJavaDir.filePath(QStringLiteral("bin/java.exe"));
    check(QFile::copy(QCoreApplication::applicationFilePath(), fakeJava),
          "AJ0 假 java 摆好了（这个 exe 的一份副本）", fakeJava);
    writeFileAt(fakeJavaDir.filePath(QStringLiteral("release")),
                QByteArrayLiteral("JAVA_VERSION=\"17.0.12\"\n"));
    {
        QJsonObject meta;
        meta.insert(QStringLiteral("javaPath"), fakeJava);
        meta.insert(QStringLiteral("name"), customName);
        writeFileAt(instanceDir.filePath(QStringLiteral("wtfml.meta")),
                    QJsonDocument(meta).toJson(QJsonDocument::Compact));
    }

    // 身份：一个**新加的离线账号**。列表先清空 —— 上一节（AH）留下了什么不该混进来，
    // 而"当前是谁"正是 ① 那条判据的一半。
    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    const QString accountName = QStringLiteral("Guard");
    QString offlineError;
    check(AccountStore::addOffline(accountName, &offlineError) &&
              AccountStore::activeUuid() == AccountStore::offlineUuidFor(accountName),
          "AJ0 当前身份 = 刚加的那个离线账号（离线 ⇒ 这一节一个请求都不发）", offlineError);

    // 让开窗时就把那个实例选上：盘上记着的「上次启动的版本」是产品**唯一**的恢复来源
    // （和 AG 那节同一个做法）。不写这一句的话，下拉框里明明有那个实例、却一个都没选 ——
    // 而 `onStartButtonClicked()` 里"没选版本"那一档会拦在前面，两条提醒根本轮不到。
    rememberLastVersion(id);

    bool ready = false;
    {
        MainWindow w;
        w.show();
        QApplication::processEvents();
        QComboBox *combo = comboByObjectName(&w, MainWindowUI::installedVersionComboName());
        // 产品那条恢复路走完之后是**没选中**（沙盒里那个键第一次写，理论上现在该选中了）——
        // 真没选上就直接摆一下，同时在断言明细里报出来（`selectedBySettings`），
        // 免得"产品那条路其实断了、靠这一句兜住了"被这一句盖过去。
        const bool selectedBySettings = combo != nullptr && selectedId(combo) == id;
        if (!selectedBySettings && combo != nullptr && combo->findData(id) >= 0) {
            combo->setCurrentIndex(combo->findData(id));
            QApplication::processEvents();
        }
        LauncherManager *manager = w.findChild<LauncherManager *>();
        AuthManager *auth = w.findChild<AuthManager *>();
        QCheckBox *duplicateCheck =
            w.ui() != nullptr ? w.ui()->getWarnDuplicateLaunchCheck() : nullptr;
        QCheckBox *worldLossCheck =
            w.ui() != nullptr ? w.ui()->getWarnWorldLossCheck() : nullptr;
        ready = combo != nullptr && manager != nullptr && auth != nullptr &&
                duplicateCheck != nullptr && worldLossCheck != nullptr &&
                selectedId(combo) == id;
        check(ready,
              "AJ0 下拉框（选中的就是那一个实例）/ LauncherManager / AuthManager / "
              "设置页那两个勾选框都找得到",
              // NOTE: 这里的分档表是精心数过的：**最多用到 %9**。写成 %10 会让
              // `QString::arg` 报一句"Argument missing"（两位数那一档它按自己的规矩
              // 找不着），而这句警告出现在输出里很像"某条断言挂了"。
              QStringLiteral("combo=%1(%2 项, 选中 %3) manager=%4 auth=%5 dup=%6 "
                             "loss=%7 期望选中 %8（产品那条恢复路给的是 %9）")
                  .arg(combo != nullptr)
                  .arg(combo != nullptr ? combo->count() : -1)
                  .arg(combo != nullptr ? combo->currentText() : QString())
                  .arg(manager != nullptr)
                  .arg(auth != nullptr)
                  .arg(duplicateCheck != nullptr)
                  .arg(worldLossCheck != nullptr)
                  .arg(id)
                  .arg(selectedBySettings));
        check(duplicateCheck != nullptr && duplicateCheck->isChecked() &&
                  worldLossCheck != nullptr && worldLossCheck->isChecked(),
              "AJ0 两个开关默认都是**开着**的（默认就该拦一下）");
        if (ready) {
            // 两个开关怎么写盘：改一下立刻存（这一格是边改边存，和「外观」那格同一个规矩）。
            duplicateCheck->setChecked(false);
            QApplication::processEvents();
            {
                LauncherSettings probe;
                check(!probe.warnDuplicateLaunch() && probe.warnWorldLoss(),
                      "AJ0 取消「重复启动」那一格 ⇒ 盘上那个键立刻跟着变（另一个不动）");
            }
            duplicateCheck->setChecked(true);
            QApplication::processEvents();
            {
                LauncherSettings probe;
                check(probe.warnDuplicateLaunch(),
                      "AJ0 勾回来 ⇒ 盘上也跟着回来（这一节后面靠的就是「它是对的」）");
            }
        }

        // ---- AJ1 一场"同一个身份、同一个实例"的会话：两条提醒的输入齐了 ----
        if (ready) {
            EffectiveJavaSettings holderSettings;
            // javaPath = **这个 exe 自己**，argv 里那一个词让它睡够四分钟（见 main() 开头）。
            holderSettings.javaPath = QCoreApplication::applicationFilePath();
            const QStringList holderArgs{QStringLiteral("session-holder")};
            check(auth->playerName() == accountName,
                  "AJ1 前提：当前身份的名字就是那个离线账号（① 那条判据的一半就在这儿）",
                  auth->playerName());
            // 身份按**产品那一套规矩**给（离线账号的 uuid 是空的，所以键落在名字上）——
            // 这里直接抄 auth->playerName()，正是 launchIdentityKey() 对离线账号取的那个值。
            const int holder =
                manager->startGame(holderArgs, holderSettings, id, customName, auth->playerName());
            check(holder > 0 && manager->runningGames().size() == 1,
                  "AJ1 前提：那个实例上有一份会话在跑（身份 = 当前这个离线账号）",
                  QStringLiteral("会话 %1 · 在跑 %2 份")
                      .arg(holder)
                      .arg(manager->runningGames().size()));
            // NOTE: 这里**不**调 repaintBottomBar()：这一节量的是启动前那两问，底栏画成
            // 什么样跟它们无关（真会话在跑那件事走的是 LauncherManager，不看界面）。
        }

        // 点一下「启动游戏」，把这一发弹的每一条按脚本答掉；返回**依次**弹了哪几条。
        const auto startWith = [&w](const QList<int> &script) {
            PromptAnswerer answerer(script);
            answerer.start();
            QMetaObject::invokeMethod(&w, "onStartButtonClicked", Qt::DirectConnection);
            QApplication::processEvents();
            answerer.stop();
            return answerer.answered;
        };
        const auto titlesOf = [](const QList<PromptAnswerer::Answered> &answered) {
            QStringList titles;
            for (const PromptAnswerer::Answered &one : answered) {
                titles.append(one.title);
            }
            return titles;
        };
        const auto runningCount = [&w]() -> int {
            LauncherManager *manager3 = w.findChild<LauncherManager *>();
            return manager3 != nullptr ? manager3->runningGames().size() : -1;
        };
        // 用户点名的那两句（① 是他逐字写的），写死在这里就是**钉住措辞本身**。
        const QString duplicateTitle = QStringLiteral("已经有一个相同的运行中游戏进程了！");
        const QString worldLossTitle = QStringLiteral("进入单人世界可能会丢失进度");
        const QString canDisable = QStringLiteral("可以在设置中关闭此提示");
        const QStringList bothButtons{QStringLiteral("取消"), QStringLiteral("仍要启动")};

        if (ready) {
            // ---- AJ2 第一下：只问 ②「进度提醒」；点「取消」⇒ 一份新进程都没有 ----
            {
                const QList<PromptAnswerer::Answered> answered = startWith({0});
                check(titlesOf(answered) == QStringList({worldLossTitle}),
                      "AJ2 第一下点启动只问 ②（① 要「我十秒内点过」才算，这一下不算）",
                      titlesOf(answered).join(QStringLiteral(" | ")));
                const PromptAnswerer::Answered first =
                    answered.isEmpty() ? PromptAnswerer::Answered() : answered.first();
                check(first.buttons == bothButtons,
                      "AJ2 下方就是「取消 / 仍要启动」两颗（用户原话）",
                      first.buttons.join(QStringLiteral(" | ")));
                check(first.text.contains(customName),
                      "AJ2 正文里点着实例的**名字**（不是版本号）", first.text);
                check(first.note.contains(canDisable) &&
                          first.note.contains(QStringLiteral("同一实例已运行时提醒")),
                      "AJ2 那行小字写着「可以在设置中关闭此提示」，并指明是哪一个开关",
                      first.note);
                check(runningCount() == 1,
                      "AJ2 点了「取消」⇒ 一份新的游戏进程都没起来",
                      QString::number(runningCount()));
            }

            // ---- AJ3 第二下：还是只问 ②（上一发点了取消，不算"他刚刚想启动过"）----
            {
                const QList<PromptAnswerer::Answered> answered = startWith({1});
                check(titlesOf(answered) == QStringList({worldLossTitle}),
                      "AJ3 第二下还是只问 ②（AJ2 那一发点了「取消」，不该被当成「想启动过」）",
                      titlesOf(answered).join(QStringLiteral(" | ")));
                check(runningCount() == 2,
                      "AJ3 点「仍要启动」⇒ 那一份真的起来了（假 java 真进程，不是只把框收掉）",
                      QString::number(runningCount()));
            }

            // ---- AJ4 十秒内再点：① 响了，标题就是用户写的那一句；点取消 ⇒ 又拦住了 ----
            {
                const QList<PromptAnswerer::Answered> answered = startWith({0});
                check(titlesOf(answered) == QStringList({duplicateTitle}),
                      "AJ4 紧接着再点一下 ⇒ 问 ①「重复启动」，标题是用户原话那一句",
                      titlesOf(answered).join(QStringLiteral(" | ")));
                const PromptAnswerer::Answered first =
                    answered.isEmpty() ? PromptAnswerer::Answered() : answered.first();
                check(first.buttons == bothButtons &&
                          first.note.contains(canDisable) &&
                          first.note.contains(QStringLiteral("重复启动")),
                      "AJ4 同一套按钮 + 同一句小字（指的是 ① 那个开关）",
                      first.buttons.join(QStringLiteral(" | ")) + QStringLiteral(" / ") +
                          first.note);
                check(runningCount() == 2,
                      "AJ4 这一问点「取消」⇒ 还是没有新进程（那两份是 AJ1/AJ3 的）",
                      QString::number(runningCount()));
            }

            // ---- AJ5 ① 上选「仍要启动」之后**接着**问 ②（一次点击两个框）----
            // 要求：「在启动同一个实例时（在前面选择仍要启动的情况下/不同账户）弹出
            // 进入单人世界可能会丢失进度的提示」——所以两条不是二选一，是前后两个框。
            {
                const QList<PromptAnswerer::Answered> answered = startWith({1, 1});
                check(titlesOf(answered) == QStringList({duplicateTitle, worldLossTitle}),
                      "AJ5 ① 放行之后紧接着问 ②（前后两个框，一次点击）",
                      titlesOf(answered).join(QStringLiteral(" | ")));
                check(runningCount() == 3,
                      "AJ5 两问都放行 ⇒ 又起来一份",
                      QString::number(runningCount()));
            }

            // ---- AJ6 十秒那个窗口的两侧 + "时钟被往回拨"那一档 ----
            // 这三下都答「取消」⇒ 一份进程都不多，量的就是**弹出来的那一条是什么**。
            // NOTE: 真等十秒太贵，所以往 m_lastLaunchAttempts 里直接摆时刻（那格是判据的
            // 全部输入，见 seedLastLaunchAttemptForTest()）。
            {
                const qint64 window = 10000;  // 用户原话「在10s内」
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                // NOTE: 一回一算好了再断言（**别**把它写进 check 的实参里：`check()` 那个
                // detail 是**恒求值**的，写进去就是"边界那一下点两次启动"，
                // 第二次读的是刚被改过的那一格，量的东西全乱）。
                const auto atOffsetMs = [&](qint64 minusMs) {
                    w.seedLastLaunchAttemptForTest(auth->playerName(), id, now - minusMs);
                    return titlesOf(startWith({0}));
                };
                const QStringList justNow = atOffsetMs(100);
                check(justNow == QStringList({duplicateTitle}),
                      "AJ6 那一笔在 0.1 秒之前 ⇒ ① 响",
                      justNow.join(QStringLiteral(" | ")));
                const QStringList insideEdge = atOffsetMs(window - 500);
                check(insideEdge == QStringList({duplicateTitle}),
                      "AJ6 在 9.5 秒之前 ⇒ ① 还是响（窗口内侧）",
                      insideEdge.join(QStringLiteral(" | ")));
                const QStringList outsideEdge = atOffsetMs(window + 500);
                check(outsideEdge == QStringList({worldLossTitle}),
                      "AJ6 在 10.5 秒之前 ⇒ ① 不响了（只剩 ②）——「10s内」那个边界就在这两档中间",
                      outsideEdge.join(QStringLiteral(" | ")));
                const QStringList backwards = atOffsetMs(-60000);
                check(backwards == QStringList({worldLossTitle}),
                      "AJ6 那一笔在**将来**（时钟被往回拨过）⇒ ① 不响（负的间隔不算「刚刚点过」）",
                      backwards.join(QStringLiteral(" | ")));
                check(runningCount() == 3,
                      "AJ6 这四下都点了「取消」⇒ 一份新的都没起来",
                      QString::number(runningCount()));
            }

            // ---- AJ7 两个开关都关掉 ⇒ 一下就放行、什么都不问 ----
            {
                duplicateCheck->setChecked(false);
                worldLossCheck->setChecked(false);
                QApplication::processEvents();
                const QList<PromptAnswerer::Answered> answered = startWith({0});
                check(answered.isEmpty(),
                      "AJ7 两个开关都关掉 ⇒ 一个框都不弹（「可以在设置中关闭此提示」真的管用）",
                      titlesOf(answered).join(QStringLiteral(" | ")));
                check(runningCount() == 4,
                      "AJ7 而且直接放行、真起来了一份",
                      QString::number(runningCount()));
                duplicateCheck->setChecked(true);
                worldLossCheck->setChecked(true);
                QApplication::processEvents();
                LauncherSettings probe;
                check(probe.warnDuplicateLaunch() && probe.warnWorldLoss(),
                      "AJ7 收尾：两个开关勾回默认（别把这一节的状态留给下一次跑）");
            }

            // ---- AJ8 收尾：会话全停（holder + 三份假 java）----
            for (const LauncherManager::GameSession &game : manager->runningGames()) {
                manager->stopGame(game.sessionId);
            }
            const bool empty =
                pumpUntil([manager] { return manager->runningGames().isEmpty(); }, 15000);
            check(empty, "AJ8 会话全停（活着的 holder / 假 java 会锁住 exe，下次构建写不进去）",
                  QString::number(manager->runningGames().size()));
        }
    }

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
          "AJ8 这一节加的账号清干净了（后面不该知道它存在过）");

    sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
    sandbox.sync();
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "AJ8 数据根目录换回来了");
}


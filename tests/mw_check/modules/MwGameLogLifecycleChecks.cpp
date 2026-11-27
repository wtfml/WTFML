#include "Support.hpp"
int MwGameLogLifecycleChecks(const bool settingsWired, MainWindow &window, QCheckBox * &showLogCheck, QComboBox * &priorityCombo, QSpinBox * &windowWidthSpin, QSpinBox * &windowHeightSpin, QComboBox * &windowModeCombo, QLabel * &windowSizeTimes) {
    // NOTE: 「实例名进窗口标题」那一半在这个工程里量不到（那要一个真会话，而离线跑不出
    // 真会话）。这里量得到的是"开关关着一个窗都不建 / 开着就建、同一份只建一个、
    // 用户关掉之后不再自动开"—— 那几条才是这条路上会坏的。
    if (settingsWired) {
        // ---- ① 开窗时灌的就是盘上那份（这一节开头沙盒刚被清过，所以是默认值）----
        {
            LauncherSettings probe;   // 同一份沙盒 ini 的另一个读者
            check(!showLogCheck->isChecked() && !probe.showGameLog(),
                  "AE1 前提：开窗时盘上是关着的（这一节后面要靠它当反面）",
                  QString("界面=%1 盘上=%2")
                      .arg(showLogCheck->isChecked() ? QStringLiteral("勾了") : QStringLiteral("没勾"))
                      .arg(probe.showGameLog() ? QStringLiteral("开") : QStringLiteral("关")));
            check(showLogCheck->isChecked() == probe.showGameLog(),
                  "AE1 「显示游戏日志」那个勾显示的就是盘上存的那个值");
            check(windowWidthSpin->value() == probe.gameWindowWidth() &&
                      windowHeightSpin->value() == probe.gameWindowHeight(),
                  "AE1 两个数字框显示的也是盘上存的那一对",
                  QString("界面 %1×%2，盘上 %3×%4")
                      .arg(windowWidthSpin->value())
                      .arg(windowHeightSpin->value())
                      .arg(probe.gameWindowWidth())
                      .arg(probe.gameWindowHeight()));
            // 默认值就是原版的 854×480（verify 的 ZD1 从设置那一侧钉过一次，这里从
            // 界面这一侧再钉一次：两边任意一侧把初值改歪了都会红）。
            check(probe.gameWindowWidth() == 854 && probe.gameWindowHeight() == 480,
                  "AE1 这一对默认值就是原版那个 854×480",
                  QString("%1×%2").arg(probe.gameWindowWidth()).arg(probe.gameWindowHeight()));

            // ---- 优先级下拉框：五档，itemData 就是枚举值，默认选中「正常」 ----
            // 条目是 MainWindow 灌的（界面层只搭壳），所以"灌没灌、灌了几条、条目的
            // itemData 对不对"只有这里看得见。**五条这个数**尤其要钉：applySettingsToUi()
            // 里那句 addItem 不去重 —— 哪天多调一次（或者界面层自己也灌一遍），用户看到的
            // 就是十档、而且选中的那条会跑到另一半去。
            check(priorityCombo->count() == 5, "AE1 优先级下拉框正好五档",
                  QString::number(priorityCombo->count()));
            const QList<ProcessPriority> order = {
                ProcessPriority::Low, ProcessPriority::BelowNormal, ProcessPriority::Normal,
                ProcessPriority::AboveNormal, ProcessPriority::High};
            QStringList got;
            for (int i = 0; i < priorityCombo->count() && i < order.size(); ++i) {
                got << QString::number(priorityCombo->itemData(i).toInt());
            }
            check(priorityCombo->count() == order.size() &&
                      priorityCombo->itemData(0).toInt() == int(ProcessPriority::Low) &&
                      priorityCombo->itemData(1).toInt() == int(ProcessPriority::BelowNormal) &&
                      priorityCombo->itemData(2).toInt() == int(ProcessPriority::Normal) &&
                      priorityCombo->itemData(3).toInt() == int(ProcessPriority::AboveNormal) &&
                      priorityCombo->itemData(4).toInt() == int(ProcessPriority::High),
                  "AE1 五档的 itemData 从上往下就是 Low→High（顺序反了就是「往低调」变成「往高调」）",
                  got.join(QStringLiteral(" | ")));
            check(priorityCombo->currentData().toInt() == int(ProcessPriority::Normal),
                  "AE1 默认选中的是「正常」那一档",
                  QString::number(priorityCombo->currentData().toInt()));
            check(priorityCombo->currentData().toInt() == int(probe.processPriority()),
                  "AE1 选中的那一档和盘上存的也是同一个");
            // 每一条都得有话说：空串的条目在界面上就是一格空白。
            QStringList blank;
            for (int i = 0; i < priorityCombo->count(); ++i) {
                if (priorityCombo->itemText(i).isEmpty()) {
                    blank << QString::number(i);
                }
            }
            check(blank.isEmpty(), "AE1 五档都有文案", blank.join(QStringLiteral(", ")));

            // ---- 窗口模式下拉框：三档，itemData 就是枚举值 ----
            // NOTE: 这里存的是 `int(GameWindowMode::…)`，和上面「进程优先级」那个下拉框
            // 同一个形状（**不是**存盘那个代号串 —— 代号只在落盘那一刻用一次，见
            // LauncherSettings::save() / gameWindowModeToken()）。所以这一条钉的是
            // "两条平行表的顺序"，**别**照着代号串去 findData（那会一条都找不到、
            // 静默停在 -1，而界面上看起来一切正常）。
            check(windowModeCombo->count() == 3, "AE1 窗口模式下拉框正好三档",
                  QString::number(windowModeCombo->count()));
            QStringList modeData;
            for (int i = 0; i < windowModeCombo->count(); ++i) {
                modeData << QString::number(windowModeCombo->itemData(i).toInt());
            }
            check(windowModeCombo->count() == 3 &&
                      windowModeCombo->itemData(0).toInt() == int(GameWindowMode::Windowed) &&
                      windowModeCombo->itemData(1).toInt() == int(GameWindowMode::Maximized) &&
                      windowModeCombo->itemData(2).toInt() == int(GameWindowMode::Fullscreen),
                  "AE1 三档的 itemData 从上往下就是小窗→最大化→全屏（顺序反了就是"
                  "「进全屏」变成「开小窗」）",
                  modeData.join(QStringLiteral(" | ")));
            check(windowModeCombo->currentData().toInt() == int(probe.gameWindowMode()),
                  "AE1 选中的那一档和盘上存的是同一个",
                  QString::number(windowModeCombo->currentData().toInt()));
            check(probe.gameWindowMode() == GameWindowMode::Windowed,
                  "AE1 这一档的默认值是「小窗」（加这个功能之前的行为）",
                  gameWindowModeToken(probe.gameWindowMode()));
            QStringList blankModes;
            for (int i = 0; i < windowModeCombo->count(); ++i) {
                if (windowModeCombo->itemText(i).isEmpty()) {
                    blankModes << QString::number(i);
                }
            }
            check(blankModes.isEmpty(), "AE1 三档都有文案",
                  blankModes.join(QStringLiteral(", ")));
            // 小窗那一档下面那两格是**能改**的（那是它唯一在用的一对）。
            check(windowWidthSpin->isEnabled() && windowHeightSpin->isEnabled(),
                  "AE1 小窗那一档：下面那两格是可用的");
        }

        // ---- ①b 「窗口模式」动一下：落盘 + 那两格跟着灰/亮----
        {
            windowModeCombo->setCurrentIndex(
                windowModeCombo->findData(int(GameWindowMode::Maximized)));
            QApplication::processEvents();
            {
                LauncherSettings probe;
                check(probe.gameWindowMode() == GameWindowMode::Maximized,
                      "AE2 选「最大化」立刻落盘",
                      gameWindowModeToken(probe.gameWindowMode()));
            }
            check(!windowWidthSpin->isEnabled() && !windowHeightSpin->isEnabled() &&
                      (windowSizeTimes == nullptr || !windowSizeTimes->isEnabled()),
                  "AE2 「最大化」那一档下面那两格连着中间的 × 一起灰掉"
                  "（最大化的大小是照屏幕算的，用户填的数这时不生效）");
            // 灰掉 ≠ 清空：盘上那一对留在原地，切回来原样还在。要是灰的时候顺手把它
            // 清成 0，用户切回小窗就会拿到一个 0×0 —— 而 0 在命令行上是一条坏参数。
            windowModeCombo->setCurrentIndex(
                windowModeCombo->findData(int(GameWindowMode::Fullscreen)));
            QApplication::processEvents();
            {
                LauncherSettings probe;
                check(probe.gameWindowMode() == GameWindowMode::Fullscreen,
                      "AE2 选「全屏」也立刻落盘",
                      gameWindowModeToken(probe.gameWindowMode()));
                check(probe.gameWindowWidth() == 854 && probe.gameWindowHeight() == 480,
                      "AE2 路过「最大化」那一下没把盘上那对数改掉/清掉",
                      QString("%1×%2").arg(probe.gameWindowWidth()).arg(probe.gameWindowHeight()));
            }
            // 「全屏」这一档那两格**不灰**：那两个数是「按 F11 退出全屏之后回到多大」，
            // 全屏那一趟真的在用。三档都灰的话用户就再也改不了它了。
            check(windowWidthSpin->isEnabled() && windowHeightSpin->isEnabled() &&
                      (windowSizeTimes == nullptr || windowSizeTimes->isEnabled()),
                  "AE2 「全屏」那一档下面那两格是**可用**的（F11 退出全屏后就用这一对）");
            check(windowWidthSpin->value() == 854 && windowHeightSpin->value() == 480,
                  "AE2 界面那两格也从「最大化」那一下原样回来了",
                  QString("%1×%2").arg(windowWidthSpin->value()).arg(windowHeightSpin->value()));
            // 收尾：切回小窗 —— 下面 ② ③ 两节要改那两格，而最大化时它们是灰的。
            windowModeCombo->setCurrentIndex(
                windowModeCombo->findData(int(GameWindowMode::Windowed)));
            QApplication::processEvents();
        }

        // ---- ② 改一下：立刻落盘（另起一个读盘者读回来）----
        showLogCheck->setChecked(true);
        priorityCombo->setCurrentIndex(priorityCombo->findData(int(ProcessPriority::High)));
        windowWidthSpin->setValue(1600);
        windowHeightSpin->setValue(900);
        QApplication::processEvents();
        {
            LauncherSettings probe;
            check(probe.showGameLog(), "AE2 勾上「显示游戏日志」立刻落盘");
            check(probe.processPriority() == ProcessPriority::High,
                  "AE2 在下拉框里选一档立刻落盘",
                  QString::number(int(probe.processPriority())));
            check(probe.gameWindowWidth() == 1600 && probe.gameWindowHeight() == 900,
                  "AE2 两个数字框改完立刻落盘",
                  QString("%1×%2").arg(probe.gameWindowWidth()).arg(probe.gameWindowHeight()));
        }

        // ---- ③ 两个端点：界面允许的值读回来必须一模一样 ----
        // 这一条盯的是"界面允许的范围 ⊆ 盘上钳位的范围"。界面放行一个后端会改掉的值，
        // 症状就是用户填了 320、下次打开变成 256 —— 这一组里最容易被忽略的那种错。
        windowWidthSpin->setValue(windowWidthSpin->minimum());
        windowHeightSpin->setValue(windowHeightSpin->maximum());
        QApplication::processEvents();
        {
            LauncherSettings probe;
            check(probe.gameWindowWidth() == windowWidthSpin->minimum(),
                  "AE2 填下限（320）读回来还是 320，没被后端抬高",
                  QString("界面=%1 盘上=%2")
                      .arg(windowWidthSpin->minimum())
                      .arg(probe.gameWindowWidth()));
            check(probe.gameWindowHeight() == windowHeightSpin->maximum(),
                  "AE2 填上限（4320）读回来还是 4320，没被后端压低",
                  QString("界面=%1 盘上=%2")
                      .arg(windowHeightSpin->maximum())
                      .arg(probe.gameWindowHeight()));
        }

        // ---- ④ 日志窗：开关关着时一个窗都不建 ----
        //
        // 【为什么这一节从顶层窗里找日志窗】
        // 用户报「日志一定会显示在启动器前面，会遮挡」—— 病根是**有主的窗在 Windows 上永远
        // 压在主窗上面**，所以产品那边用的是 `new GameLogWindow(name, nullptr)`（没有父窗口，
        // 见 GameLogWindow 头文件那段）。父窗口没了，这些窗就不是 window 的孩子了，
        // `window.findChild<GameLogWindow*>()` 从此**一个都找不到** —— 得从顶层窗里挑。
        const auto openLogWindows = []() {
            QList<GameLogWindow *> found;
            for (QWidget *top : QApplication::topLevelWidgets()) {
                if (auto *log = qobject_cast<GameLogWindow *>(top)) {
                    found.append(log);
                }
            }
            return found;
        };
        check(openLogWindows().isEmpty(),
              "AE3 前提：走到这里一个日志窗都还没开（下面几条量的是个数）",
              QString::number(openLogWindows().size()));

        // 走的是真槽（onProcessOutput 是私有的，靠 invokeMethod 打进去，和 U 节一个路子）。
        // sessionId 用一个不存在的号：showGameLogLine() 里"这一份是哪个实例"只用来填标题，
        // 号不存在时拿到的是一个默认会话（空名字 → 标题就是「游戏日志」）—— 对这条断言
        // 来说正合适：它量的是"建不建窗"，不是"标题写什么"。
        //
        // NOTE: 上面 ② 把这个开关**打开过**（那是为了验落盘），所以这里必须先关回去 ——
        // 不然这一条量的就成了"开着的时候建不建窗"，而它恰恰是开着才建的。
        showLogCheck->setChecked(false);
        QApplication::processEvents();
        const int fakeSession = 424242;
        QMetaObject::invokeMethod(&window, "onProcessOutput", Qt::DirectConnection,
                                  Q_ARG(int, fakeSession),
                                  Q_ARG(QString, QStringLiteral("这一行不该建窗\n")));
        QApplication::processEvents();
        check(openLogWindows().isEmpty(),
              "AE3 开关关着时，游戏输出一个日志窗都不建（绝大多数用户的状态）",
              QString::number(openLogWindows().size()));

        // ---- ⑤ 开关开着：第一条输出就建窗，而且字写进去了 ----
        showLogCheck->setChecked(true);
        QApplication::processEvents();
        QMetaObject::invokeMethod(&window, "onProcessOutput", Qt::DirectConnection,
                                  Q_ARG(int, fakeSession),
                                  Q_ARG(QString, QStringLiteral("第一行输出\n")));
        QApplication::processEvents();
        const QList<GameLogWindow *> firstWindows = openLogWindows();
        GameLogWindow *logWin = firstWindows.isEmpty() ? nullptr : firstWindows.first();
        check(logWin != nullptr, "AE4 开着时第一条输出就建出日志窗");
        if (logWin != nullptr) {
            check(logWin->logText().contains(QStringLiteral("第一行输出")),
                  "AE4 那一行也真的写进去了（不是建了个空窗）", logWin->logText().left(120));
            check(logWin->isModal() == false,
                  "AE4 这个窗是非模态的（游戏跑着的时候用户还要在启动器上干别的）");

            // ---- 它**没有父窗口**（用户报的「日志一定会显示在启动器前面，会遮挡」那个病根）----
            // 在 Windows 上"有主的窗"永远压在主窗上面，那条规则不是风格问题、也没法用别的手法
            // 绕开 —— 所以产品那边把 parent 传成了 nullptr（见 GameLogWindow 头文件那段），
            // 这一条量的是"那条路真的走了 nullptr 这条路"。
            check(logWin->parentWidget() == nullptr,
                  "AE4 日志窗没有父窗口（有主的窗在 Windows 上永远压在启动器上面，点都点不动它）",
                  logWin->parentWidget() == nullptr ? QStringLiteral("<null>")
                                                    : QStringLiteral("有父窗口！"));
            check(logWin->isWindow(), "AE4 是个独立的顶层窗（任务栏上有自己一格、能自己最小化）");

            // ---- 摆的位置：按「启动器主窗的矩形」算出来的那一个 ----
            // 这里拿**同一个纯函数**再算一遍：它挡的是"move(placementFor(...)) 那一句被删了"
            // —— 删掉的话窗停在默认位置（多半正好压着启动器），而"没有父窗口"那几条照样绿。
            //
            // NOTE: 这一条在沙盒里**信号很弱** —— offscreen 那块屏只有 800x800，而启动器
            // （1024x750）加上一个 820x520 的日志窗两边都塞不下，于是 placementFor 按
            // "躲不开就居中"回的是 (0,0)，跟"没调用过 move()"的默认位置**恰好一样**
            // （每次跑都会把那个值打出来）。所以在沙盒里它能钉住的只是"没走岔到别的
            // 公式上"；真的几何（贴右 / 右边挤不下翻左 / 两边都挤不下居中 / 错开）在
            // ui_check 的 Z3b 里按假矩形一条条量过 —— 那边不受这台机器屏幕多大影响。
            {
                const QRect launcherFrame = window.frameGeometry();
                const QScreen *scr = QGuiApplication::screenAt(launcherFrame.center());
                if (scr == nullptr) {
                    scr = QGuiApplication::primaryScreen();
                }
                if (scr != nullptr) {
                    const QPoint want = GameLogWindow::placementFor(
                        launcherFrame, logWin->size(), scr->availableGeometry(), 0);
                    const QString detail =
                        QStringLiteral("实际=%1,%2 该=%3,%4；启动器=%5,%6 %7x%8；日志窗=%9x%10；屏幕=%11x%12")
                            .arg(logWin->x())
                            .arg(logWin->y())
                            .arg(want.x())
                            .arg(want.y())
                            .arg(launcherFrame.x())
                            .arg(launcherFrame.y())
                            .arg(launcherFrame.width())
                            .arg(launcherFrame.height())
                            .arg(logWin->width())
                            .arg(logWin->height())
                            .arg(scr->availableGeometry().width())
                            .arg(scr->availableGeometry().height());
                    check(logWin->pos() == want,
                          "AE4 摆的位置就是按启动器主窗算出来的那一个（这一句挡的是 move() 被删掉）",
                          detail);
                    std::printf("  [AE4 摆位] %s\n", qPrintable(detail));
                }
            }


            // stderr 那条路也进同一个窗，而且**不另开一个**。
            QMetaObject::invokeMethod(&window, "onProcessError", Qt::DirectConnection,
                                      Q_ARG(int, fakeSession),
                                      Q_ARG(QString, QStringLiteral("往 stderr 写的一行\n")));
            QApplication::processEvents();
            check(openLogWindows().size() == 1,
                  "AE4 stderr 那一行进的是同一个窗，没有再开一个",
                  QString::number(openLogWindows().size()));
            check(logWin->logText().contains(QStringLiteral("往 stderr 写的一行")),
                  "AE4 而且 stderr 那行也写进去了");
            // stderr 那一行**不**加"错误"前缀：很多模组把 INFO 也写 stderr，加了反而误导。
            check(!logWin->logText().contains(QStringLiteral("[游戏错误]")),
                  "AE4 窗里的是游戏原话，没有启动器加的前缀");

            // 另一份会话（另一个 sessionId）要**另开一个**窗 —— 两三个实例输出混在一个窗里
            // 等于没法看。这条是"一份游戏一个窗"那句承诺的断言。
            QMetaObject::invokeMethod(&window, "onProcessOutput", Qt::DirectConnection,
                                      Q_ARG(int, fakeSession + 1),
                                      Q_ARG(QString, QStringLiteral("另一份游戏的一行\n")));
            QApplication::processEvents();
            check(openLogWindows().size() == 2,
                  "AE4 另一份会话另开一个窗（一份游戏一个窗）",
                  QString::number(openLogWindows().size()));

            // ---- ⑥ 用户关掉这一份的窗：不再自动开 ----
            logWin->close();
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            check(openLogWindows().size() == 1,
                  "AE4 关掉之后那个窗真的没了（WA_DeleteOnClose）",
                  QString::number(openLogWindows().size()));
            QMetaObject::invokeMethod(&window, "onProcessOutput", Qt::DirectConnection,
                                      Q_ARG(int, fakeSession),
                                      Q_ARG(QString, QStringLiteral("关窗之后又来的一行\n")));
            QApplication::processEvents();
            check(openLogWindows().size() == 1,
                  "AE4 用户关过的那一份不再自动开（不然就是关不掉的窗）",
                  QString::number(openLogWindows().size()));

            // 收拾：剩下的那个窗关掉（留着会让后面的节数出多余的窗）
            for (GameLogWindow *left : openLogWindows()) {
                left->close();
            }
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            check(openLogWindows().isEmpty(),
                  "AE4 天亮之前把窗都收干净了",
                  QString::number(openLogWindows().size()));
        }

        // ---- ⑦ 日志窗按级别染底 + 顶上那排带计数的筛选（喂真槽）----
        // 上面几条量的是"建窗 / 写字"这条线；这一节量用户新要的那两件事：**按级别自动染底**
        // 和**顶上一排只显示某几档、并且统计各档有几条**。
        //
        // 走的是同一条真槽（onProcessOutput），**不**另外喂一遍 GameLogWindow::appendText ——
        // 那等于自己给自己出题：产品把哪一行认成哪一档、账记成几条，这里就量什么。
        //
        // 用**另一个 sessionId**：上面那个 fakeSession 已经进了"用户关过的不再自动开"名单
        // （AE4 那条刚验过），再喂它连窗都不会建。此刻窗已经被 ⑥ 收干净了，所以下面 findChild
        // 拿到的必定是这一节自己新建的这一个。
        const int levelSession = fakeSession + 7;
        QMetaObject::invokeMethod(&window, "onProcessOutput", Qt::DirectConnection,
                                  Q_ARG(int, levelSession),
                                  Q_ARG(QString, QStringLiteral("[12:00:00] [main/INFO]: 起来了一条\n"
                                                                "[12:00:01] [main/DEBUG]: 调试一条\n"
                                                                "[12:00:02] [main/WARN]: 警告一条\n"
                                                                "[12:00:03] [main/ERROR]: 出错一条\n"
                                                                "[12:00:04] [main/FATAL]: 全完了\n"
                                                                "认不出级别的一条续行\n")));
        QApplication::processEvents();
        const QList<GameLogWindow *> levelWindows = openLogWindows();
        GameLogWindow *levelWin = levelWindows.isEmpty() ? nullptr : levelWindows.first();
        check(levelWin != nullptr, "AE6 前提：换一份会话照样建窗（这一节全靠它）");
        if (levelWin != nullptr) {
            // ① 认级别：五档各一条；最后那行认不出级别，会沿用 FATAL 的颜色与筛选归属，
            // 但它只是上一条日志的续行，不应把顶部的“日志记录数”再加一次。
            const int expect[GameLogWindow::levelCount()] = {1, 1, 1, 1, 1};   // Debug/Info/Warn/Error/Fatal
            check(levelWin->lineCount() == 6, "AE6 六行一条不多一条不少",
                  QString::number(levelWin->lineCount()));
            QString wrong;
            int sum = 0;
            for (int i = 0; i < GameLogWindow::levelCount(); ++i) {
                const GameLogWindow::LogLevel lv = GameLogWindow::levelAt(i);
                const int got = levelWin->lineCountFor(lv);
                sum += got;
                if (got != expect[i]) {
                    wrong += QStringLiteral("%1=%2 ").arg(GameLogWindow::levelName(lv)).arg(got);
                }
            }
            check(wrong.isEmpty(),
                  "AE6 五档各有一条独立日志（续行归入 FATAL，但不重复计数）", wrong);
            check(sum == 5 && levelWin->lineCount() == 6,
                  "AE6 顶部统计五条日志记录，正文仍保留六个物理行",
                  QStringLiteral("%1 vs %2").arg(sum).arg(levelWin->lineCount()));

            // ② 顶上那排开关：五颗都在，而且**每颗上写的数字就是这一档的账**（用户要的统计）。
            QString missing, mismatched;
            for (int i = 0; i < GameLogWindow::levelCount(); ++i) {
                const GameLogWindow::LogLevel lv = GameLogWindow::levelAt(i);
                QToolButton *button = levelWin->getFilterButton(lv);
                if (button == nullptr) {
                    missing += GameLogWindow::levelName(lv) + QStringLiteral(" ");
                    continue;
                }
                const QString want = GameLogWindow::filterButtonText(lv, levelWin->lineCountFor(lv));
                if (button->text() != want) {
                    mismatched += QStringLiteral("%1写[%2]该写[%3] ")
                                      .arg(GameLogWindow::levelName(lv), button->text(), want);
                }
            }
            check(missing.isEmpty(), "AE6 五档各有一颗筛选按钮（顶上那排五颗都在）", missing);
            check(mismatched.isEmpty(), "AE6 每颗按钮上的计数就是这一档的账（那份统计的出处）",
                  mismatched);
            // 顶上那排右边那颗**行数上限**下拉框（用户让把「显示全部」换成它：
            // 「显示500/1000/2000/25/10k/all（直接写数字，不要学我）条」—— 原话里那个
            // 「25」是他打错了，订正成 5000；这一节不钉档位（那是 ui_check Z10 的事），
            // 只钉"有这么个控件、默认停在全部那一档"）。
            QComboBox *levelCombo = levelWin->getLineLimitCombo();
            check(levelCombo != nullptr, "AE6 顶上那排右边有一颗行数上限下拉框（换掉了「显示全部」）");
            if (levelCombo != nullptr) {
                check(levelCombo->count() == GameLogWindow::lineLimitChoices().size() &&
                          levelCombo->currentData().toInt() == GameLogWindow::defaultLineLimit(),
                      "AE6 它的默认档就是「全部」（开窗那一眼看到的是全部的日志）",
                      levelCombo->currentText());
            }

            // ③ 点的是**真按钮**（不是直接调 setLevelVisible）—— 用户能碰到的就是这一颗。
            // NOTE: 量"关掉之后看不见"必须同时量"账没变小、按钮上的数也没变" —— 只量前者的话，
            // 把计数做成"数当前显示着的行"也一样绿。
            QToolButton *errButton = levelWin->getFilterButton(GameLogWindow::LogLevel::Error);
            errButton->click();
            QApplication::processEvents();
            check(!errButton->isChecked() && !levelWin->levelVisible(GameLogWindow::LogLevel::Error),
                  "AE6 点一下那颗按钮就把这一档关掉了");
            check(!levelWin->logText().contains(QStringLiteral("出错一条")),
                  "AE6 关掉的那一档在正文里真的没了");
            check(levelWin->lineCountFor(GameLogWindow::LogLevel::Error) == 1 &&
                      errButton->text() ==
                          GameLogWindow::filterButtonText(GameLogWindow::LogLevel::Error, 1),
                  "AE6 但这一档的账没变小、按钮上的数字也没变（关掉显示不是丢掉）",
                  errButton->text());
            check(levelWin->lineCount() == 6, "AE6 模型里六行都还在",
                  QString::number(levelWin->lineCount()));

            // ④ 五档全关：正文该说的是「都被筛掉了」，不是「还没有输出」——两句话不是一回事。
            for (int i = 0; i < GameLogWindow::levelCount(); ++i) {
                QToolButton *button = levelWin->getFilterButton(GameLogWindow::levelAt(i));
                if (button->isChecked()) {
                    button->click();
                }
            }
            QApplication::processEvents();
            check(levelWin->logText().isEmpty(), "AE6 五档全关 → 正文一行不剩",
                  levelWin->logText().left(80));
            check(levelWin->getLogView()->placeholderText() ==
                      GameLogWindow::filteredOutPlaceholderText(),
                  "AE6 那时候说的是「都被筛掉了」（不是「还没有输出」）",
                  levelWin->getLogView()->placeholderText());

            // ⑤ 五档一颗一颗点回来（也是**真按钮**）。
            // NOTE: 「显示全部」那颗已经换成行数下拉框，所以"全部勾回来"这条路
            // **只剩手点五颗按钮**这一条 —— 这一节照这条路走，同时把"顶上的
            // 下拉框跟筛选是两件事"钉住：五颗都点回来之后，正文该一字不差地
            // 回来（下拉框那档没动过）。
            for (int i = 0; i < GameLogWindow::levelCount(); ++i) {
                QToolButton *button = levelWin->getFilterButton(GameLogWindow::levelAt(i));
                if (!button->isChecked()) {
                    button->click();
                }
            }
            QApplication::processEvents();
            bool allOn = true;
            for (int i = 0; i < GameLogWindow::levelCount(); ++i) {
                if (!levelWin->levelVisible(GameLogWindow::levelAt(i))) {
                    allOn = false;
                }
            }
            check(allOn, "AE6 五颗一颗一颗点回来，五档又都勾上了");
            check(levelWin->logText().contains(QStringLiteral("出错一条")) &&
                      levelWin->logText().contains(QStringLiteral("全完了")),
                  "AE6 字都回来了");
            check(levelWin->getLogView()->placeholderText() == GameLogWindow::emptyPlaceholderText(),
                  "AE6 占位那句也回到「还没有输出」",
                  levelWin->getLogView()->placeholderText());
            check(levelWin->lineCount() == 6 && levelWin->displayLineLimit() ==
                                                    GameLogWindow::defaultLineLimit(),
                  "AE6 这一路一个模型行都没丢、行数上限也还在「全部」那档");

            // 这一节自己开的窗自己收：⑧ 收尾不关窗，留着会让后面的节数出多余的窗。
            levelWin->close();
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            check(openLogWindows().isEmpty(),
                  "AE6 这一节开的窗也收干净了",
                  QString::number(openLogWindows().size()));
        }

        // ---- ⑦b 游戏退出之后这个窗怎么办（喂真槽 onGameFinished）----
        //
        // 走的是真槽（onGameFinished 是私有槽，靠 invokeMethod 打进去，和上面一个路子）。
        {
            const auto feedLine = [&window](int sessionId, const QString &text) {
                QMetaObject::invokeMethod(&window, "onProcessOutput", Qt::DirectConnection,
                                          Q_ARG(int, sessionId), Q_ARG(QString, text));
                QApplication::processEvents();
            };
            const auto exitGame = [&window](int sessionId, int code, QProcess::ExitStatus status) {
                QMetaObject::invokeMethod(&window, "onGameFinished", Qt::DirectConnection,
                                          Q_ARG(int, sessionId), Q_ARG(int, code),
                                          Q_ARG(QProcess::ExitStatus, status));
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            };
            // 按**窗里那句话**找窗，不按 `topLevelWidgets()` 的顺序：那个顺序没有保证，
            // 而下面几条要的正是"哪一扇是哪一份会话的"（几扇窗的标题一模一样 —— 这些
            // 假 sessionId 没有实例名，标题都是「游戏日志」）。
            const auto findLogWindow = [&openLogWindows](const QString &needle) {
                for (GameLogWindow *candidate : openLogWindows()) {
                    if (candidate->logText().contains(needle)) {
                        return candidate;
                    }
                }
                return static_cast<GameLogWindow *>(nullptr);
            };

            // (a) 正常以 0 退出 → 窗自己关掉。
            const int cleanSession = fakeSession + 21;
            feedLine(cleanSession, QStringLiteral("正常跑完的那一行\n"));
            const QList<GameLogWindow *> beforeClean = openLogWindows();
            check(beforeClean.size() == 1,
                  "AE7 前提：这一份会话的窗建出来了（不然下面那条量的是「本来就没有」）",
                  QString::number(beforeClean.size()));
            exitGame(cleanSession, 0, QProcess::NormalExit);
            check(openLogWindows().isEmpty(),
                  "AE7 游戏正常以 0 退出 → 日志窗自动关掉、而且真的销毁了",
                  QString::number(openLogWindows().size()));
            // 关窗走的是 finished 那条路，它会把这一份记进"用户关过的不再自动开"。产品那边
            // 紧接着要把它摘掉（那一笔是"用户自己关的"才该留着）—— 摘晚了的话摘的就是空的，
            // 于是这一份留在名单里。再喂一次同一份就能看出来：真摘掉了才会重新建窗。
            feedLine(cleanSession, QStringLiteral("退出之后又冒出来的一行\n"));
            check(openLogWindows().size() == 1,
                  "AE7 自动关窗**不算**「用户关过」（关窗 → 摘名单的顺序反了这条就红）",
                  QString::number(openLogWindows().size()));
            for (GameLogWindow *left : openLogWindows()) {
                left->close();
            }
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

            // (b) 正常退出但码不是 0 → 留着，而且末行补上那句"到此为止"。
            const int failedSession = fakeSession + 22;
            feedLine(failedSession, QStringLiteral("出事前的那一行\n"));
            exitGame(failedSession, 1, QProcess::NormalExit);
            GameLogWindow *failedWin = findLogWindow(QStringLiteral("出事前的那一行"));
            check(failedWin != nullptr && openLogWindows().size() == 1,
                  "AE7 码不是 0 → 窗留着（那正是用户要逐行看的时候）",
                  QString::number(openLogWindows().size()));
            check(failedWin != nullptr &&
                      failedWin->logText().contains(QStringLiteral("[游戏已退出，退出码：1]")),
                  "AE7 而且末行补上了「游戏已退出，退出码：1」",
                  failedWin != nullptr ? failedWin->logText().right(60) : QString());

            // (c) 崩溃 → 留着，末行是"已崩溃退出"那句。
            const int crashSession = fakeSession + 23;
            feedLine(crashSession, QStringLiteral("崩之前的那一行\n"));
            exitGame(crashSession, -1, QProcess::CrashExit);
            GameLogWindow *crashWin = findLogWindow(QStringLiteral("崩之前的那一行"));
            check(crashWin != nullptr,
                  "AE7 崩溃 → 窗留着（崩栈那几行正是用户要看的）",
                  QString::number(openLogWindows().size()));
            check(crashWin != nullptr &&
                      crashWin->logText().contains(QStringLiteral("[游戏已崩溃退出，退出码：-1]")),
                  "AE7 而且说的是「已崩溃退出」不是「已退出」（两句不同的话）",
                  crashWin != nullptr ? crashWin->logText().right(60) : QString());
            // (d) 崩溃 + 码正好是 0（Windows 上杀进程拿到的就是 0）→ **不关**。
            // 这一条是"只看码"那种写法的照妖镜：把 autoCloseOnExit 写成 `exitCode == 0`
            // 的话，(a) 照样绿，这一条红。
            const int killedSession = fakeSession + 24;
            feedLine(killedSession, QStringLiteral("被杀掉之前的最后一行\n"));
            exitGame(killedSession, 0, QProcess::CrashExit);
            check(openLogWindows().size() == 3,
                  "AE7 崩溃退出、码正好是 0 → 也不关（只看码会把它误判成「正常退出」）",
                  QString::number(openLogWindows().size()));

            for (GameLogWindow *left : openLogWindows()) {
                left->close();
            }
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            check(openLogWindows().isEmpty(),
                  "AE7 这一节开的窗也收干净了",
                  QString::number(openLogWindows().size()));

            // (e) #151：退出码 0 但日志含 Forge 加载失败特征 → 日志窗不能自动关掉。
            const int modFailSession = fakeSession + 25;
            feedLine(modFailSession,
                     QStringLiteral("[15:50:52] [main/ERROR]: Missing or unsupported mandatory dependencies:\n"));
            exitGame(modFailSession, 0, QProcess::NormalExit);
            GameLogWindow *modFailWin =
                findLogWindow(QStringLiteral("Missing or unsupported mandatory dependencies:"));
            check(modFailWin != nullptr && openLogWindows().size() == 1,
                  "★★ AE7 退出码 0 但含模组加载失败特征 → 日志窗保留不自动关闭",
                  QString::number(openLogWindows().size()));
            check(modFailWin != nullptr &&
                      modFailWin->logText().contains(QStringLiteral("[游戏异常退出，退出码：0]")),
                  "AE7 而且末行补上了「游戏异常退出，退出码：0」",
                  modFailWin != nullptr ? modFailWin->logText().right(60) : QString());
            for (GameLogWindow *left : openLogWindows()) {
                left->close();
            }
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }

        // ---- ⑧ 收尾：这一节改过的三项要恢复原样 ----
        // 后面还有几节要接着跑（O 节数版本、Z 节底栏那几行），一份被改过的全局配置留在
        // 沙盒里迟早会被谁撞上。
        showLogCheck->setChecked(false);
        priorityCombo->setCurrentIndex(priorityCombo->findData(int(ProcessPriority::Normal)));
        windowWidthSpin->setValue(854);
        windowHeightSpin->setValue(480);
        QApplication::processEvents();
        {
            LauncherSettings probe;
            check(!probe.showGameLog() && probe.processPriority() == ProcessPriority::Normal &&
                      probe.gameWindowWidth() == 854 && probe.gameWindowHeight() == 480,
                  "AE8 三项都恢复成默认了",
                  QString("log=%1 prio=%2 %3×%4")
                      .arg(probe.showGameLog() ? 1 : 0)
                      .arg(int(probe.processPriority()))
                      .arg(probe.gameWindowWidth())
                      .arg(probe.gameWindowHeight()));
        }
    }
return 0;
}



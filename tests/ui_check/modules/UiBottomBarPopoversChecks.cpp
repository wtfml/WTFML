#include "Support.hpp"
int UiBottomBarPopoversChecks(QMainWindow &window, MainWindowUI &ui, QTabWidget * &tabs) {

    {
        // 要求：「这个文本这里和另一边头像那里都做成个按钮，点击以后能向上展开更多信息
        // 的那种（像官启那样的）」； 又定下两块面板装什么：「左边那个列出进行中
        // 的实例…右边一个电源图标一个齿轮图标，分别是关闭实例和进入实例设置，右边是快速
        // 切换账号，列出最后登录的三个账户，离线的右侧按钮写 选择，正版的右侧写 登录」。
        // 这一节量的是**面板机制**（谁装在谁里面、弹在哪、怎么关、点了行尾按钮回来什么）
        // 加上**一行长什么样**（图标 + 两行字 + 行尾几颗按钮），内容由这里自己喂进去 ——
        // ui/ 不许 include src/，真数据（哪个实例、最近哪三个账号）是 MainWindow 的事，
        // 那几条在 mw_check 里量。
        QPushButton *runBtn = ui.getRunStateButton();
        QPushButton *accBtn = ui.getAccountButton();
        // 账号弹层的锚点是**主页右上那颗账号胶囊**（`accCard`，11-12 用户反馈）。
        // `accBtn` 是老那一格账号，一直收着，留着只量"底栏那组兼容控件还在"。
        QPushButton *accCard = ui.getAccountSkinButton();
        QLabel *runState = ui.getRunStateLabel();
        QLabel *runDetail = ui.getRunStateDetailLabel();
        QLabel *avatar = ui.getAvatarLabel();
        QProgressBar *progress = ui.getProgressBar();
        check(runBtn != nullptr && accBtn != nullptr && accCard != nullptr,
              "I4 底栏那两颗按钮 + 主页那张账号卡片都在");
        if (runBtn != nullptr && accBtn != nullptr && runState != nullptr &&
            runDetail != nullptr && avatar != nullptr && progress != nullptr) {
            // ① 整格是**一颗按钮**：那两行字（右边是头像那组）都装在按钮里面。
            // 这条同时是"挤短进度条"那个 bug 的护栏 —— 宽度钉在**按钮**上，
            // 里面有没有字都一样宽（隐藏的控件在布局里宽度是 0）。
            // 文字装在按钮里的 runStateText 格子里（图标轨整块收起），所以认「按钮是祖先」。
            check(runBtn->isAncestorOf(runState) && runBtn->isAncestorOf(runDetail),
                  "I4 左边那两行字装在那颗按钮里",
                  runState->parentWidget() != nullptr ? runState->parentWidget()->objectName()
                                                      : QStringLiteral("<null>"));
            check(avatar->parentWidget() == accBtn, "I4 头像装在那颗按钮里",
                  avatar->parentWidget() != nullptr ? avatar->parentWidget()->objectName()
                                                    : QStringLiteral("<null>"));
            // 是**贴着按钮左边缘**摆的：靠右摆会在头像左边空出一大段（实测 115 px ——
            // 那一格宽 208、内容只有九十来 px），「头像那里的左侧有一段空白」就是它。
            check(avatar->x() == 0, "I4 头像贴着按钮左边缘（左边不留空）",
                  QStringLiteral("头像 x=%1 / 按钮宽 %2").arg(avatar->x()).arg(accBtn->width()));
            // ② 体验反馈 8：底栏按钮右侧不再挂朝上的小箭头（原 objectName `bottomBarChevron`）。
            check(window.findChildren<QLabel *>(QStringLiteral("bottomBarChevron")).isEmpty(),
                  "I4 底栏按钮右侧没有小箭头",
                  QStringLiteral("找到 %1 个")
                      .arg(window.findChildren<QLabel *>(QStringLiteral("bottomBarChevron")).size()));

            // ③ 点一下 —— 向上弹出一块面板。先把进度条亮出来，好量"弹面板不动底栏"。
            //
            // 面板**吃的是外面喂进来的行**（ui/ 不许 include src/，数据在 MainWindow 手里，
            // 见 setBottomBarPanelContent()）。所以这一节自己装一个假 provider 喂两行：
            // 第 0 行是"一份实例"（图标 + 两行字 + 行尾两颗图标按钮，其中一颗灰掉），
            // 第 1 行是"什么都没有"那种说明行（没图标、没按钮）。
            // 真数据（哪个实例在跑、最近哪三个账号、当前账号那颗写什么）是 MainWindow 的事，
            // 在 mw_check 里量 —— 那边有真会话和真账号。
            QList<bool> firedPanels;
            QList<QString> firedKeys;
            QList<int> firedActions;
            ui.setBottomBarPanelContent(
                [](bool account) -> QList<MainWindowUI::PanelRow> {
                    if (account) {
                        // 账号弹层：当前账号那一行打勾（checked），另一个账号整行可点；
                        // 末尾两行菜单项（menuItem）。
                        QList<MainWindowUI::PanelRow> accounts;
                        MainWindowUI::PanelRow mine;
                        mine.key = QStringLiteral("uuid-mine");
                        mine.title = QStringLiteral("LoserKugua");
                        mine.subtitle = MainWindowUI::bottomBarPanelOnlineAccountText();
                        mine.checked = true;
                        accounts << mine;

                        MainWindowUI::PanelRow other;
                        other.key = QStringLiteral("uuid-other");
                        other.title = QStringLiteral("选手二号");
                        other.subtitle = MainWindowUI::bottomBarPanelOfflineAccountText();
                        accounts << other;

                        MainWindowUI::PanelRow add;
                        add.key = QStringLiteral("menu-add");
                        add.title = MainWindowUI::accountAddText();
                        add.menuItem = true;
                        accounts << add;
                        MainWindowUI::PanelRow manage;
                        manage.key = QStringLiteral("menu-manage");
                        manage.title = MainWindowUI::accountSkinButtonText();
                        manage.menuItem = true;
                        accounts << manage;
                        return accounts;
                    }
                    QList<MainWindowUI::PanelRow> rows;
                    MainWindowUI::PanelRow game;
                    game.key = QStringLiteral("sess-7");
                    game.icon = VersionIcons::versionChannelMark(
                        0, MainWindowUI::bottomBarPanelIconSize());
                    game.title = QStringLiteral("我的整合包-1.20.1-NeoForge");
                    game.subtitle = QStringLiteral("已运行 12 分钟");
                    const QColor ink(Qt::black);
                    MainWindowUI::PanelAction power;
                    power.icon = VersionIcons::power(MainWindowUI::bottomBarPanelActionIconSize(),
                                                     ink);
                    power.tooltip = QStringLiteral("关闭这个实例");
                    MainWindowUI::PanelAction gear;
                    gear.icon = VersionIcons::gear(MainWindowUI::bottomBarPanelActionIconSize(),
                                                   ink);
                    gear.tooltip = QStringLiteral("实例设置");
                    gear.enabled = false;  // 灰掉一颗，"灰的那颗点不动"才有得量
                    // 第三颗是**字的**（运行中实例弹层"打开日志/停止"那一套，
                    // 见 MainWindowUI::bottomBarPanelLogText()）。摆成和产品里一样的
                    // 「图标 / 图标 / 字」三颗，那条"文字那颗按字撑宽、图标那两颗是正方形"
                    // 才有得量。
                    MainWindowUI::PanelAction log;
                    log.text = MainWindowUI::bottomBarPanelLogText();
                    log.tooltip = QStringLiteral("打开这一场游戏的日志窗");
                    game.actions << power << gear << log;
                    rows << game;

                    MainWindowUI::PanelRow note;
                    note.title = MainWindowUI::bottomBarPanelNoRunningTitle();
                    note.subtitle = QStringLiteral("一句说明");
                    note.placeholder = true;
                    rows << note;
                    return rows;
                },
                [&firedPanels, &firedKeys, &firedActions](bool account, const QString &key,
                                                          int action) {
                    firedPanels << account;
                    firedKeys << key;
                    firedActions << action;
                });
            progress->show();
            ui.getStatusLabel()->show();
            QApplication::processEvents();
            const int barWidthBefore = progress->width();
            runBtn->click();
            QApplication::processEvents();
            QWidget *panel = window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
            check(panel != nullptr && panel->isVisible(), "I4 点左边那颗按钮弹出了一块面板");
            if (panel != nullptr) {
                check(panel->windowFlags().testFlag(Qt::Popup),
                      "I4 它是 Qt::Popup（点界面别处自动收回去、不抢主窗口焦点）",
                      QStringLiteral("flags = 0x%1").arg(uint(panel->windowFlags()), 0, 16));
                // 行控件按**行号**找得回来（objectName 由 bottomBarPanelRowName() 给），
                // 行里的两行字各有各的名字 —— 验证工程不用数"有几个 QLabel"这种脆弱判据。
                QWidget *rowWidget0 =
                    panel->findChild<QWidget *>(MainWindowUI::bottomBarPanelRowName(0));
                QWidget *rowWidget1 =
                    panel->findChild<QWidget *>(MainWindowUI::bottomBarPanelRowName(1));
                QLabel *title0 =
                    panel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowTitleName(0));
                QLabel *subtitle0 =
                    panel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowSubtitleName(0));
                QLabel *title1 =
                    panel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowTitleName(1));
                QLabel *subtitle1 =
                    panel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowSubtitleName(1));
                check(rowWidget0 != nullptr && rowWidget1 != nullptr,
                      "I4 喂进去两行，面板里就两行（行控件按行号找得回来）",
                      QStringLiteral("第 0 行 %1 / 第 1 行 %2")
                          .arg(rowWidget0 != nullptr ? QStringLiteral("在") : QStringLiteral("没找到"),
                               rowWidget1 != nullptr ? QStringLiteral("在") : QStringLiteral("没找到")));
                // 「一行一个会话」：主行字是实例名，次行字是**这一份跑了多久**（用户
                // 原话：「一行一个会话，有时长是好主意，做进去」）。
                check(title0 != nullptr &&
                          title0->text() == QStringLiteral("我的整合包-1.20.1-NeoForge"),
                      "I4 第一行的主行字就是喂进去的实例名",
                      title0 != nullptr ? title0->text() : QStringLiteral("<没找到>"));
                check(subtitle0 != nullptr && subtitle0->text() == QStringLiteral("已运行 12 分钟"),
                      "I4 第一行的次行字是那一份的已运行时长（一行一个会话，各带各的时长）",
                      subtitle0 != nullptr ? subtitle0->text() : QStringLiteral("<没找到>"));
                check(title1 != nullptr &&
                          title1->text() == MainWindowUI::bottomBarPanelNoRunningTitle() &&
                          subtitle1 != nullptr &&
                          subtitle1->text() == QStringLiteral("一句说明"),
                      "I4 第二行是喂进去的那行说明（标题 + 次行字原样画出来）",
                      title1 != nullptr && subtitle1 != nullptr
                          ? title1->text() + " / " + subtitle1->text()
                          : QStringLiteral("<没找到>"));
                // 每一行都得**装得下自己那行字**：面板要是只有一行那么高（用户报过
                // 「左右上拉只有一行的高度 文字…显示不全」），这些字会被压成半个字高。
                int squashed = 0;
                const QList<QLabel *> texts = {title0, subtitle0, title1, subtitle1};
                for (QLabel *line : texts) {
                    if (line != nullptr && line->height() < line->fontMetrics().height()) {
                        ++squashed;
                    }
                }
                check(squashed == 0 && title0 != nullptr && subtitle0 != nullptr &&
                          title1 != nullptr && subtitle1 != nullptr,
                      "I4 面板里每一行都装得下自己那行字（没被压扁）",
                      QStringLiteral("压扁了 %1 行").arg(squashed));
                // 那一列字**宽度钉死**：面板的宽度是"内容多宽就多宽"（popUp() 里
                // adjustSize()），跟着长实例名走的话，长名字会把面板顶出窗口。
                check(title0 != nullptr &&
                          title0->width() == MainWindowUI::bottomBarPanelTextWidth(),
                      "I4 那一列字的宽度钉死在 bottomBarPanelTextWidth()",
                      title0 != nullptr
                          ? QStringLiteral("宽 %1 / 该占 %2")
                                .arg(title0->width())
                                .arg(MainWindowUI::bottomBarPanelTextWidth())
                          : QStringLiteral("<没找到>"));
                // 行尾那几颗按钮：实例那一行是**两颗图标**（电源 = 关闭这一份实例、
                // 齿轮 = 进它的实例设置），说明那一行一颗都没有。
                const int actionSize = MainWindowUI::bottomBarPanelActionSize();
                QPushButton *powerBtn = panel->findChild<QPushButton *>(
                    MainWindowUI::bottomBarPanelRowActionName(0, 0));
                QPushButton *gearBtn = panel->findChild<QPushButton *>(
                    MainWindowUI::bottomBarPanelRowActionName(0, 1));
                QPushButton *logBtn = panel->findChild<QPushButton *>(
                    MainWindowUI::bottomBarPanelRowActionName(0, 2));
                check(powerBtn != nullptr && gearBtn != nullptr && logBtn != nullptr,
                      "I4 实例那一行行尾有三颗按钮（第 0 = 关闭 / 第 1 = 实例设置 / "
                      "第 2 = 打开日志窗）");
                check(powerBtn != nullptr && powerBtn->size() == QSize(actionSize, actionSize),
                      "I4 行尾按钮的大小钉死在 bottomBarPanelActionSize()",
                      powerBtn != nullptr
                          ? QStringLiteral("%1x%2 / 该是 %3x%3")
                                .arg(powerBtn->width())
                                .arg(powerBtn->height())
                                .arg(actionSize)
                          : QStringLiteral("<没找到>"));
                check(powerBtn != nullptr && powerBtn->text().isEmpty() &&
                          !powerBtn->icon().isNull(),
                      "I4 行尾画的是**图标**不是字（给了 icon 就不画 text）");
                check(powerBtn != nullptr && !powerBtn->toolTip().isEmpty(),
                      "I4 电源那颗带一句能看懂的说明（一个电源符号说不清点了会把游戏杀掉）",
                      powerBtn != nullptr ? powerBtn->toolTip() : QStringLiteral("<没找到>"));
                check(gearBtn != nullptr && !gearBtn->isEnabled(),
                      "I4 灰掉的那一颗真的点不动（照 PanelAction::enabled 走，右面板"
                      "「当前」那颗也靠它）");
                // 「打开日志」那颗：**没给图标就给字**，而且按字撑宽 ——
                // 图标那两颗是钉死的正方形（22），文字这颗要是也被压成 22 宽，「日志」两个字
                // 就只剩半个。两条合起来钉住 "PanelAction 的两种形态各走各的那一支"。
                check(logBtn != nullptr && logBtn->icon().isNull() && !logBtn->text().isEmpty(),
                      "I4 第三颗画的是**字**不是图标（没给 icon 就走 text 那一支）",
                      logBtn != nullptr
                          ? QStringLiteral("text =「%1」/ icon 空 = %2")
                                .arg(logBtn->text(),
                                     logBtn->icon().isNull() ? QStringLiteral("是")
                                                             : QStringLiteral("不是"))
                          : QStringLiteral("<没找到>"));
                check(logBtn != nullptr && logBtn->text() == MainWindowUI::bottomBarPanelLogText(),
                      "I4 那颗上的字就是 bottomBarPanelLogText()（产品里那颗和它同一个出处）",
                      logBtn != nullptr ? logBtn->text() : QStringLiteral("<没找到>"));
                check(logBtn != nullptr && logBtn->width() > actionSize &&
                          logBtn->width() >= logBtn->sizeHint().width(),
                      "I4 文字那颗比图标那颗宽（按字撑开，没被压成图标那个正方形）",
                      logBtn != nullptr
                          ? QStringLiteral("宽 %1 / 图标那颗 %2 / 想要 %3")
                                .arg(logBtn->width())
                                .arg(actionSize)
                                .arg(logBtn->sizeHint().width())
                          : QStringLiteral("<没找到>"));
                // 三颗的**左右次序就是编号次序**（0 → 1 → 2）。面板是"编号 = 第几颗"认人的
                // （动作回调里那个下标），摆错位置的话用户按下去的是另一件事。
                check(powerBtn != nullptr && gearBtn != nullptr && logBtn != nullptr &&
                          powerBtn->x() < gearBtn->x() && gearBtn->x() < logBtn->x(),
                      "I4 三颗按钮从左到右就是 0/1/2（编号认人，摆错了按下去做的是别的事）",
                      logBtn != nullptr
                          ? QStringLiteral("x = %1 / %2 / %3")
                                .arg(powerBtn != nullptr ? powerBtn->x() : -1)
                                .arg(gearBtn != nullptr ? gearBtn->x() : -1)
                                .arg(logBtn->x())
                          : QStringLiteral("<没找到>"));
                check(panel->findChild<QPushButton *>(
                          MainWindowUI::bottomBarPanelRowActionName(1, 0)) == nullptr,
                      "I4 说明那一行行尾一颗按钮都没有");
                // 行首那块方块：给了图标的那一行画出来、而且是**正好那么大**（按
                // bottomBarPanelIconSize() 摆，尺寸不对就是被拉伸了或者面板被撑高）。
                // 没给图标的那一行（这里是"没有实例在运行"那句，真面板上还有账号那几行）
                // **一个图标控件都不该有** —— 留一格空白的话，两行字的左边缘就对不齐了。
                QLabel *icon0 =
                    panel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowIconName(0));
                check(icon0 != nullptr && !icon0->pixmap().isNull() &&
                          icon0->size() == QSize(MainWindowUI::bottomBarPanelIconSize(),
                                                 MainWindowUI::bottomBarPanelIconSize()),
                      "I4 行首那块方块画出来了、边长是 bottomBarPanelIconSize()",
                      icon0 != nullptr
                          ? QStringLiteral("%1x%2").arg(icon0->width()).arg(icon0->height())
                          : QStringLiteral("<没找到>"));
                check(panel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowIconName(1)) ==
                          nullptr,
                      "I4 没给图标的那一行连图标控件都没有（那一格不是「留白」）");
                // 那句"跑了多久"的四档话术。**纯函数**（MainWindowUI::runningDurationText），
                // 所以四档都能在这儿钉住 —— "已经跑了 3 天"那一档在真界面上等不出来。
                // 要求：「一行一个会话，有时长是好主意，做进去」。
                const qint64 base = QDateTime::currentMSecsSinceEpoch();
                check(MainWindowUI::runningDurationText(base, base + 59 * 1000) ==
                          QStringLiteral("已运行不到 1 分钟") &&
                          MainWindowUI::runningDurationText(base, base + 12 * 60 * 1000) ==
                              QStringLiteral("已运行 12 分钟") &&
                          MainWindowUI::runningDurationText(base, base + (2 * 60 + 7) * 60 * 1000) ==
                              QStringLiteral("已运行 2 小时 7 分钟") &&
                          MainWindowUI::runningDurationText(base,
                                                            base + (3 * 24 + 5) * 60 * 60 * 1000) ==
                              QStringLiteral("已运行 3 天 5 小时"),
                      "I4 时长四档话术：不到 1 分钟 / N 分钟 / N 小时 M 分钟 / N 天 M 小时",
                      QStringLiteral("%1 | %2 | %3 | %4")
                          .arg(MainWindowUI::runningDurationText(base, base + 59 * 1000),
                               MainWindowUI::runningDurationText(base, base + 12 * 60 * 1000),
                               MainWindowUI::runningDurationText(
                                   base, base + (2 * 60 + 7) * 60 * 1000),
                               MainWindowUI::runningDurationText(
                                   base, base + (3 * 24 + 5) * 60 * 60 * 1000)));
                // 整点那半句不缀：2 小时整就说"2 小时"，不是"2 小时 0 分钟"。
                check(MainWindowUI::runningDurationText(base, base + 120 * 60 * 1000) ==
                          QStringLiteral("已运行 2 小时"),
                      "I4 整点分钟不缀那半句（读起来不像机器写的）",
                      MainWindowUI::runningDurationText(base, base + 120 * 60 * 1000));
                // 几何：面板整个在按钮**上面**，左边缘贴按钮左边缘
                const QPoint runAnchor = runBtn->mapToGlobal(QPoint(0, 0));
                check(panel->y() + panel->height() <= runAnchor.y(),
                      "I4 面板在按钮上方（不是压在底栏上）",
                      QStringLiteral("面板底 %1 / 按钮顶 %2")
                          .arg(panel->y() + panel->height()).arg(runAnchor.y()));
                check(panel->x() == runAnchor.x(), "I4 左边那块左边缘贴按钮左边缘",
                      QStringLiteral("面板 x %1 / 按钮 x %2").arg(panel->x()).arg(runAnchor.x()));
                check(panel->width() >= runBtn->width(), "I4 面板不比按钮窄",
                      QStringLiteral("面板宽 %1 / 按钮宽 %2")
                          .arg(panel->width()).arg(runBtn->width()));
                // 弹面板**不许动底栏** —— 这一轮修的就是"弹出来把中间那根条挤动"。
                check(progress->width() == barWidthBefore,
                      "I4 弹出面板前后，中间那根进度条一点没动",
                      QStringLiteral("%1 px → %2 px").arg(barWidthBefore).arg(progress->width()));
                // Esc 关掉 + WA_DeleteOnClose：关完在窗口里就找不到了（不是藏起来）
                QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(panel, &esc);
                QApplication::processEvents();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                check(window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName()) == nullptr,
                      "I4 Esc 关掉之后面板就没了（WA_DeleteOnClose 真删）");
            }
            // ④ 行尾那颗按钮：点一下要**先把面板收回去**，再把「哪一行是谁 + 第几颗」交回来。
            //
            // NOTE: 先收回去是硬要求 —— 这两个动作里有一个要弹模态窗（实例设置那个对话框），
            // 而面板是个 Qt::Popup，它按着鼠标键盘的 grab；模态窗开在一层抓着 grab
            // 的东西底下，在 Windows 上就是"窗出来了但点不动"。所以回调是**排到
            // 下一轮事件循环**去的（close() 只发了个关闭事件，面板要等这一轮走完
            // 才真的没了，当轮就弹模态窗等于没关）。
            // NOTE: 回来的身份是 PanelRow::key（左面板 = 会话号），**不是行号** ——
            // 面板弹出来到按钮被点之间隔着用户看那两眼，这中间列表是会变的，
            // 认行号就会"关掉另一份游戏"。
            firedPanels.clear();
            firedKeys.clear();
            firedActions.clear();
            runBtn->click();
            QApplication::processEvents();
            QWidget *panelAgain = window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
            QPushButton *powerAgain =
                panelAgain != nullptr
                    ? panelAgain->findChild<QPushButton *>(
                          MainWindowUI::bottomBarPanelRowActionName(0, 0))
                    : nullptr;
            check(powerAgain != nullptr, "I4 再开一次，量「点行尾按钮」那条的前提还在");
            if (powerAgain != nullptr) {
                powerAgain->click();
                check(firedKeys.isEmpty(),
                      "I4 点行尾那颗按钮：动作不**当场**回来（面板得先真的收回去）",
                      QStringLiteral("当场就回来了 %1 次").arg(firedKeys.size()));
                QApplication::processEvents();
                check(firedPanels == QList<bool>{false} &&
                          firedKeys == QList<QString>{QStringLiteral("sess-7")} &&
                          firedActions == QList<int>{0},
                      "I4 回来的是「哪一行是谁 + 第几颗」：key = 会话号，动作号 0（左面板 "
                      "account = false）",
                      QStringLiteral("account=%1 key=%2 action=%3")
                          .arg(firedPanels.value(0))
                          .arg(firedKeys.value(0))
                          .arg(firedActions.value(0)));
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                check(window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName()) == nullptr,
                      "I4 点完行尾按钮，面板已经收回去了");
            }
            // ④b **第三颗**（那颗文字按钮「日志」）点下去报的是编号 2，不是 0 —— "第几颗"
            // 是面板唯一认得出的身份（按钮上是图标还是字、写着什么，都不参与判断）。
            // 这条和上面那条第 0 颗的合起来说明"编号 = 位置"这件事两头都成立。
            firedPanels.clear();
            firedKeys.clear();
            firedActions.clear();
            runBtn->click();
            QApplication::processEvents();
            QWidget *panelText = window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
            QPushButton *logAgain =
                panelText != nullptr
                    ? panelText->findChild<QPushButton *>(
                          MainWindowUI::bottomBarPanelRowActionName(0, 2))
                    : nullptr;
            check(logAgain != nullptr, "I4 再开一次，那颗文字按钮还在原位（第 0 行第 2 颗）");
            if (logAgain != nullptr) {
                logAgain->click();
                QApplication::processEvents();
                check(firedPanels == QList<bool>{false} &&
                          firedKeys == QList<QString>{QStringLiteral("sess-7")} &&
                          firedActions == QList<int>{2},
                      "I4 点第三颗回来的是动作号 2（文字那颗和图标那两颗一样按位置认人）",
                      QStringLiteral("account=%1 key=%2 action=%3")
                          .arg(firedPanels.value(0))
                          .arg(firedKeys.value(0))
                          .arg(firedActions.value(0)));
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            // ⑤ 开关：开着的时候再点**同一颗**按钮要**收回**（要求：
            // 「那个按钮，点开以后处于展开状态时再次点击按钮收回」）。
            // NOTE: 这一下 Qt 会把它重放给按钮（Qt::Popup 点外面自动关的时候专门补的，
            // 不然"菜单关掉之后再点不开"），所以按钮照旧收到 clicked —— 收回去
            // 靠的是接线那道"已经开着就不许再开"，不是靠 Qt 帮忙。
            runBtn->click();
            QApplication::processEvents();
            check(window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName()) != nullptr,
                  "I4 点一下弹出来（开关的第一下）");
            runBtn->click();
            QApplication::processEvents();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            const QWidget *stillOpen =
                window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
            check(stillOpen == nullptr,
                  "I4 展开状态下再点同一颗按钮 = 收回（不是关了又立刻开一块新的）",
                  stillOpen != nullptr ? QStringLiteral("还留着一块 %1x%2，visible=%3")
                                             .arg(stillOpen->width())
                                             .arg(stillOpen->height())
                                             .arg(stillOpen->isVisible())
                                       : QString());
            runBtn->click();
            QApplication::processEvents();
            check(window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName()) != nullptr,
                  "I4 收回之后再点一下还能打开（开关是双向的）");
            // ⑥ 账号弹层（锚点是主页右上那颗账号胶囊，从它**下面**弹出、右边对齐）。
            // 这会儿左边那块是开着的：点账号胶囊要**这块收、那块开**，不能两块一起挂着。
            // 胶囊在主页上：先切到主页，量的才是它真摆着的几何。
            tabs->setCurrentIndex(0);
            QApplication::processEvents();
            QWidget *leftOpen = window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
            check(leftOpen != nullptr && leftOpen->isVisible(),
                  "I4 换面之前左边那块开着（量下面那条的前提）");
            const auto accountPopover = [&window]() -> QWidget * {
                QWidget *pop = window.findChild<QWidget *>(MainWindowUI::accountPopoverName());
                return pop != nullptr && pop->isVisible() ? pop : nullptr;
            };
            const auto settle = []() {
                QApplication::processEvents();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QApplication::processEvents();
            };
            accCard->click();
            settle();
            QWidget *pop = accountPopover();
            check(pop != nullptr, "I4 点账号按钮：开账号弹层");
            {
                QWidget *still = window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
                check(still == nullptr || !still->isVisible(),
                      "I4 换面之后左边那块收掉了（同一时间只挂一块弹层）");
            }
            check(accCard->property("wtfmlOpen").toBool(),
                  "I4 弹层开着时账号按钮保持按住的底色（wtfmlOpen）");
            if (pop != nullptr) {
                const QPoint a = accCard->mapTo(pop->parentWidget(), QPoint(0, 0));
                check(pop->x() + pop->width() == a.x() + accCard->width() || pop->x() == 8 ||
                          pop->x() + pop->width() == pop->parentWidget()->width() - 8,
                      "I4 账号弹层右边和账号胶囊右边对齐（窄窗时夹在窗口里）",
                      QStringLiteral("弹层右 %1 / 胶囊右 %2")
                          .arg(pop->x() + pop->width()).arg(a.x() + accCard->width()));
                check(pop->y() == a.y() + accCard->height() + 8,
                      "I4 账号弹层从胶囊下面 8 弹出（往下长）",
                      QStringLiteral("弹层顶 %1 / 胶囊底 %2").arg(pop->y()).arg(a.y() + accCard->height()));
                auto *row0 = pop->findChild<QAbstractButton *>(QStringLiteral("accountPopoverRow0"));
                auto *row1 = pop->findChild<QAbstractButton *>(QStringLiteral("accountPopoverRow1"));
                auto *row2 = pop->findChild<QAbstractButton *>(QStringLiteral("accountPopoverRow2"));
                auto *row3 = pop->findChild<QAbstractButton *>(QStringLiteral("accountPopoverRow3"));
                check(row0 != nullptr && row1 != nullptr && row2 != nullptr && row3 != nullptr,
                      "I4 喂进去四行（两个账号 + 两行菜单项），弹层里就四行");
                QWidget *tick = pop->findChild<QWidget *>(QStringLiteral("accountPopoverCheck"));
                check(tick != nullptr && row0 != nullptr && tick->parentWidget() == row0,
                      "I4 当前账号那一行打勾（只有那一行）");
                check(pop->findChildren<QPushButton *>().isEmpty() ||
                          std::none_of(pop->findChildren<QPushButton *>().cbegin(),
                                       pop->findChildren<QPushButton *>().cend(),
                                       [](QPushButton *b) {
                                           return b->text() == MainWindowUI::bottomBarPanelLogoutText();
                                       }),
                      "I4 弹层里没有「登出」（登出在「管理账号与皮肤」里）");
                check(row2 != nullptr && row0 != nullptr && row2->height() < row0->height(),
                      "I4 菜单项那两行比账号行矮（36 vs 52）",
                      row2 != nullptr && row0 != nullptr
                          ? QStringLiteral("%1 / %2").arg(row2->height()).arg(row0->height())
                          : QString());
                // 点另一个账号：弹层收回去，回调带 account = true + 那一行的 uuid + 动作 0。
                firedPanels.clear();
                firedKeys.clear();
                firedActions.clear();
                if (row1 != nullptr) row1->click();
                settle();
                check(firedPanels == QList<bool>{true} &&
                          firedKeys == QList<QString>{QStringLiteral("uuid-other")} &&
                          firedActions == QList<int>{0},
                      "I4 点别的账号回来的是 account = true、key = 那一行的 uuid、动作 0",
                      QStringLiteral("account=%1 key=%2 action=%3")
                          .arg(firedPanels.value(0))
                          .arg(firedKeys.value(0))
                          .arg(firedActions.value(0)));
                check(accountPopover() == nullptr && !accCard->property("wtfmlOpen").toBool(),
                      "I4 点了之后弹层收回去、按钮底色也复原");
            }
            // 菜单项：「管理账号与皮肤」那一行回来的是它自己的 key。
            firedKeys.clear();
            accCard->click();
            settle();
            pop = accountPopover();
            if (auto *manageRow = pop != nullptr
                                      ? pop->findChild<QAbstractButton *>(QStringLiteral("accountPopoverRow3"))
                                      : nullptr) {
                manageRow->click();
                settle();
            }
            check(firedKeys == QList<QString>{QStringLiteral("menu-manage")},
                  "I4 点「管理账号与皮肤」回来的 key 就是那一行的 key",
                  firedKeys.value(0));
            // 开关：开着再点同一颗 = 收回。
            accCard->click();
            settle();
            check(accountPopover() != nullptr, "I4 点一下弹出来（开关的第一下）");
            accCard->click();
            settle();
            check(accountPopover() == nullptr, "I4 展开状态下再点同一颗按钮 = 收回");
            // Esc 收。
            accCard->click();
            settle();
            if (QWidget *open = accountPopover()) {
                QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(open, &esc);
                settle();
            }
            check(accountPopover() == nullptr, "I4 Esc 收起账号弹层");

            // ⑥b 未登录：一个账号都没有 ⇒ 头像占位 +「未登录」+ 一颗「登录」。
            {
                ui.setBottomBarPanelContent(
                    [](bool account) -> QList<MainWindowUI::PanelRow> {
                        MainWindowUI::PanelRow none;
                        none.placeholder = true;
                        none.title = account ? MainWindowUI::bottomBarPanelNoAccountTitle()
                                             : MainWindowUI::bottomBarPanelNoRunningTitle();
                        if (!account) return {none};
                        MainWindowUI::PanelRow login;
                        login.key = QStringLiteral("menu-login");
                        login.title = QStringLiteral("登录");
                        login.menuItem = true;
                        return {none, login};
                    },
                    [&firedKeys](bool, const QString &key, int) { firedKeys << key; });
                firedKeys.clear();
                accCard->click();
                settle();
                QWidget *out = accountPopover();
                auto *state = out != nullptr ? out->findChild<QLabel *>(QStringLiteral("accountPopoverState")) : nullptr;
                auto *login = out != nullptr ? out->findChild<QPushButton *>(QStringLiteral("accountPopoverRow1")) : nullptr;
                check(state != nullptr && state->text() == MainWindowUI::bottomBarPanelNoAccountTitle() &&
                          login != nullptr && login->text() == QStringLiteral("登录"),
                      "I4 未登录：弹层写「未登录」+ 一颗「登录」",
                      state != nullptr ? state->text() : QStringLiteral("<没找到>"));
                if (login != nullptr) login->click();
                settle();
                check(firedKeys == QList<QString>{QStringLiteral("menu-login")} && accountPopover() == nullptr,
                      "I4 点「登录」：弹层收回、回调带登录那一行的 key",
                      firedKeys.value(0));
            }

            // ⑥c 一个账号时弹层紧凑，三个账号时跟着长高。
            {
                int wantedRows = 1;
                ui.setBottomBarPanelContent(
                    [&wantedRows](bool account) -> QList<MainWindowUI::PanelRow> {
                        QList<MainWindowUI::PanelRow> rows;
                        if (!account) return rows;
                        for (int i = 0; i < wantedRows; ++i) {
                            MainWindowUI::PanelRow one;
                            one.key = QStringLiteral("uuid-%1").arg(i);
                            one.title = QStringLiteral("账号 %1").arg(i + 1);
                            one.subtitle = MainWindowUI::bottomBarPanelOnlineAccountText();
                            one.checked = i == 0;
                            rows << one;
                        }
                        return rows;
                    },
                    nullptr);
                const auto measure = [&]() {
                    accCard->click();
                    settle();
                    QWidget *open = accountPopover();
                    const int height = open != nullptr ? open->height() : -1;
                    if (open != nullptr) open->close();
                    settle();
                    return height;
                };
                const int heightOne = measure();
                wantedRows = 3;
                const int heightThree = measure();
                check(heightOne > 0 && heightOne < 2 * 52,
                      "I4 只有一个账号时弹层**不到两行高**（不被撑成大盒子）",
                      QStringLiteral("弹层 %1").arg(heightOne));
                check(heightThree > heightOne,
                      "I4 账号多起来（三行）弹层跟着长高",
                      QStringLiteral("三行 %1 / 一行 %2").arg(heightThree).arg(heightOne));
                check(accountPopover() == nullptr, "I4 量完那一块收回去了（这一节的收尾不留给下一节）");
                ui.setBottomBarPanelContent(nullptr, nullptr);
            }
            // ⑦ 兜底：没装 provider（或它给了个空列表）时，面板里留**一句话**而不是一块
            // 空白 —— 空白一块比一句话更难查（用户分不清"没弹出来"和"弹出来是空的"）。
            ui.setBottomBarPanelContent(nullptr, nullptr);
            runBtn->click();
            QApplication::processEvents();
            QWidget *emptyPanel = window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
            QLabel *emptyTitle =
                emptyPanel != nullptr
                    ? emptyPanel->findChild<QLabel *>(MainWindowUI::bottomBarPanelRowTitleName(0))
                    : nullptr;
            check(emptyTitle != nullptr && !emptyTitle->text().isEmpty() &&
                      emptyPanel->findChild<QPushButton *>(
                          MainWindowUI::bottomBarPanelRowActionName(0, 0)) == nullptr,
                  "I4 没内容时面板里留一句话（不是一块空白、行尾也没有按钮）",
                  emptyTitle != nullptr ? emptyTitle->text() : QStringLiteral("<没找到>"));
            if (emptyPanel != nullptr) {
                emptyPanel->close();
                QApplication::processEvents();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            // 收尾：那个假 provider 捕的是这一节里的局部量，用完就卸掉（留着的话，
            // 后面哪一节再点开面板就是拿着悬空引用在跑）。
            ui.setBottomBarPanelContent(nullptr, nullptr);
            progress->hide();
            // 那一格还成开局那一份 —— 不能再 `status->hide()`：它开局就是亮的
            // （理由见 E4/I 那两节）。
            ui.setBottomBarTaskSummary(MainWindowUI::BottomBarTaskSummary());
            QApplication::processEvents();

            // ⑧ 左下那一格**点不点得开**。判据在 MainWindow 手里
            // （`runningGames()` 空不空），界面这一头只管把"点得开 / 点不开"画出来
            // （见 setRunPanelAvailable()）—— 所以这里量两件事：**初值**必须是点得开
            // （谁都没摆过的时候不该一上来就点不动，独立构造 MainWindowUI 的地方吃这个
            // 初值），以及摆下去之后**真的**点不开（不是只把箭头藏起来做样子）。
            //
            // NOTE: 这一节用 `click()` 点，不用真鼠标事件：`QAbstractButton::click()`
            // 在 disabled 的时候会**直接返回**（那样断言就白绿了），而这一格用的
            // 是 toggle 里把住、按钮本身是 enabled 的 —— 所以
            // `click()` 照样发 clicked()，能不能弹出来全看 toggle 里那道闸。
            {
                const auto describe = [runBtn, &ui]() {
                    return QStringLiteral("收鼠标 %1 / tooltip「%2」")
                        .arg(!ui.runPanelAvailable()
                                 ? QStringLiteral("是")
                                 : QStringLiteral("不是"))
                        .arg(runBtn->toolTip());
                };
                check(ui.runPanelAvailable() &&
                          runBtn->toolTip() == MainWindowUI::bottomBarRunPanelTip(),
                      "I4 没被告知「没有实例在跑」时，这一格照旧点得开（鼠标收得到、"
                      "悬停那句话也挂着）",
                      describe());

                ui.setRunPanelAvailable(false);
                QApplication::processEvents();
                check(!ui.runPanelAvailable() &&
                          runBtn->toolTip() == MainWindowUI::runStateNotRunningText(),
                      "I4 被告知「没有实例在跑」之后：这一格点不开、"
                      "悬停提示换成「未运行」",
                      describe());

                runBtn->click();
                QApplication::processEvents();
                check(window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName()) == nullptr,
                      "I4 点不开的时候点它**什么都不弹**（不是弹一块空的出来）");

                ui.setRunPanelAvailable(true);
                QApplication::processEvents();
                runBtn->click();
                QApplication::processEvents();
                QWidget *backPanel =
                    window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName());
                check(backPanel != nullptr && backPanel->isVisible(),
                      "I4 摆回「点得开」之后，点它照旧弹得出来",
                      describe());
                if (backPanel != nullptr) {
                    backPanel->close();
                    QApplication::processEvents();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                }
            }
        }
    }
 return 0;
}



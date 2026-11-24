#include "Support.hpp"

// U10L：「服务器」那一页的"现在什么样"（图标 / MOTD / 延迟）。
//
// 用户问的「服务器列表能实际 ping 服务器并显示服务器图标和 motd 吗」：**连上去问**那一半
// （握手、超时、拆包、图标那串 data URL）在 build/verify 的 ZH 节里真连一次验完了；
// 这一节验的是另一半 —— 问回来的东西画在哪儿：图标进哪一格、MOTD 进哪一行、状态那一列
// 写什么、那几颗按钮按下去发不发信。
//
// 这一段原来住在 `UiContentPagesChecks.cpp` 那一大段里、跟着同一个 `dialog` 活几页。
// 分出来以后夹具是**显式形参**（`dialog` 由调用方持有，生命周期见 docs/dev/TESTING.md
// 「夹具生命周期（ui_check 的大模块）」），这个模块不再假装自己有一份。
int UiServerPageChecks(InstanceSettingsDialog &dialog) {
    using Dlg = InstanceSettingsDialog;
    const auto showPage = [](Dlg &d, const QString &key) { d.showPage(key); };
    // ---- U10L 「服务器」那一页的"现在什么样"（：图标 / MOTD / 延迟）----
    //
    // 用户 问的「服务器列表能实际 ping 服务器并显示服务器图标和 motd 吗」。
    // **连上去问**那一半（握手、超时、拆包、图标那串 data URL）在 build/verify 的 ZH
    // 节里真连一次验完了；这一节验的是另一半 —— 问回来的东西画在哪儿：图标进哪一格、
    // MOTD 进哪一行、状态那一列写什么、那几颗按钮按下去发不发信。
    {
        showPage(dialog, QStringLiteral("server"));
        auto *tree = dialog.serverTree();
        auto *refresh = dialog.serverRefreshButton();
        auto *add = dialog.serverAddButton();
        auto *remove = dialog.serverDeleteButton();
        check(tree != nullptr && refresh != nullptr && add != nullptr && remove != nullptr &&
                  dialog.serverEditButton() != nullptr,
              "U10L 「刷新」「编辑」「添加」「删除」四颗按钮和那张表都在");

        // ---- U10L-1 三列 + 每一行一开始就写着「还没测」 ----
        dialog.setServerList({qMakePair(QStringLiteral("我的服"),
                                        QStringLiteral("mc.example.com")),
                              qMakePair(QStringLiteral("朋友的服"),
                                        QStringLiteral("1.2.3.4:25566")),
                              qMakePair(QStringLiteral("挂了的服"),
                                        QStringLiteral("5.6.7.8"))},
                             QString());
        check(tree->topLevelItemCount() == 3, "U10L 三台服务器摆了三行");
        // 多选：删除按选中的全部发一条批量信号，只选一台时仍发原来的单条信号；编辑 / 加入只认恰好一行。
        {
            QList<QPair<QString, QString>> batch;
            QString singleAddress;
            int singles = 0;
            const auto many = QObject::connect(&dialog, &Dlg::deleteServersRequested, &dialog,
                [&](const QList<QPair<QString, QString>> &entries) { batch = entries; });
            const auto one = QObject::connect(&dialog, &Dlg::deleteServerRequested, &dialog,
                [&](const QString &, const QString &address) { singleAddress = address; ++singles; });
            check(tree->selectionMode() == QAbstractItemView::ExtendedSelection, "服务器列表允许多选");
            tree->topLevelItem(0)->setSelected(true);
            tree->topLevelItem(2)->setSelected(true);
            check(remove->isEnabled() && !dialog.serverEditButton()->isEnabled(),
                  "多选时「删除」可点、「编辑」灰着");
            remove->click();
            check(batch.size() == 2 && batch.at(0).second == QStringLiteral("mc.example.com")
                      && batch.at(1).second == QStringLiteral("5.6.7.8") && singles == 0,
                  "多选按「删除」发一条带全部选中项的批量信号");
            tree->clearSelection();
            tree->topLevelItem(1)->setSelected(true);
            check(dialog.serverEditButton()->isEnabled(), "只选一台时「编辑」可点");
            remove->click();
            check(singles == 1 && singleAddress == QStringLiteral("1.2.3.4:25566") && batch.size() == 2,
                  "只选一台时仍发单条删除信号");
            tree->clearSelection();
            QObject::disconnect(many);
            QObject::disconnect(one);
        }
        QStringList headers;
        for (int i = 0; i < tree->columnCount(); ++i) {
            headers.append(tree->headerItem() != nullptr ? tree->headerItem()->text(i)
                                                         : QString());
        }
        check(headers == QStringList({Dlg::serverNameColumnText(), Dlg::serverAddressColumnText(),
                                      Dlg::serverStatusColumnText()}),
              "★ U10L 多出来的是**第三列**「状态」（名称 / 地址 / 状态）", headers.join('/'));
        // 空白和"测了没通"在屏幕上是同一个样子，而这两件事对用户完全不是一回事。
        check(tree->topLevelItem(0)->text(2) == Dlg::serverUntestedText() &&
                  tree->topLevelItem(2)->text(2) == Dlg::serverUntestedText(),
              "★ U10L 刚读出来时每一行的状态那一格写的是「还没测」（不是留一栏空白）",
              tree->topLevelItem(0)->text(2));

        // ---- U10L-2 名字那一格挂的是那个两行式的代理（图标 + 名字 + MOTD）----
        auto *delegate = qobject_cast<VersionTreeDelegate *>(tree->itemDelegateForColumn(0));
        check(delegate != nullptr,
              "★ U10L 第 0 列挂的是 VersionTreeDelegate（和版本树 / 下载页同一份实现）");
        check(delegate != nullptr && !delegate->leafArrow(),
              "★ U10L 而且叶子那一层**不画**右边那个 `>` —— 服务器不是「点进去还有东西」",
              delegate != nullptr ? QStringLiteral("leafArrow=%1").arg(delegate->leafArrow())
                                  : QStringLiteral("<null>"));
        check(delegate != nullptr && delegate->iconSize() == MainWindowUI::serverIconSize() &&
                  tree->iconSize() == QSize(MainWindowUI::serverIconSize(),
                                            MainWindowUI::serverIconSize()),
              "★ U10L 图标那一格是 serverIconSize() 那么大（控件和代理两处得是同一个数）",
              QStringLiteral("代理 %1、控件 %2x%3、期望 %4")
                  .arg(delegate != nullptr ? delegate->iconSize() : -1)
                  .arg(tree->iconSize().width())
                  .arg(tree->iconSize().height())
                  .arg(MainWindowUI::serverIconSize()));
        check(tree->itemDelegateForColumn(1) != delegate,
              "U10L 地址那一列没挂它（那一列是给外人看的地址，不需要两行式的行）");

        // ---- U10L-3 状态那一格写什么（纯函数，四档）----
        {
            Dlg::ServerRowStatus untested;
            check(Dlg::serverStatusText(untested) == Dlg::serverUntestedText(),
                  "U10L 没测过 ⇒「还没测」", Dlg::serverStatusText(untested));

            Dlg::ServerRowStatus dead;
            dead.tested = true;
            dead.online = false;
            dead.error = QStringLiteral("没有回应（对面没开，或者端口不对）");
            check(Dlg::serverStatusText(dead) == dead.error,
                  "★ U10L 没通 ⇒ 直接把**它没通的那句话**摆出来（用户要的就是那个理由）",
                  Dlg::serverStatusText(dead));

            // 超时那一档**只写两个字**（要求：「超时的只写超时」）。
            // `error` 那两句仍是全的（两句都在这儿钉一遍）—— 判据是 timedOut 那一位，
            // 不是这句话的前缀：界面上**不许**拿字符串去认。
            Dlg::ServerRowStatus timedOut;
            timedOut.tested = true;
            timedOut.online = false;
            timedOut.timedOut = true;
            timedOut.error = QStringLiteral("已连接，但服务器没有响应（等待超时）");
            check(Dlg::serverStatusLines(timedOut) == QStringList({Dlg::serverTimeoutText()}),
                  "★ U10L 超时 ⇒ 那一格只写「超时」两个字（理由是给排查看的，不进这一格）",
                  Dlg::serverStatusLines(timedOut).join(QStringLiteral("︱")));
            Dlg::ServerRowStatus timedOutNet = timedOut;
            timedOutNet.error = QStringLiteral("超时（网络不通）");
            check(Dlg::serverStatusLines(timedOutNet) ==
                      QStringList({Dlg::serverTimeoutText()}),
                  "★ U10L 两种超时（网络不通 / 它没答话）在这一格上是同一句「超时」",
                  Dlg::serverStatusLines(timedOutNet).join(QStringLiteral("︱")));
            // 反过来那条同样要钉：**不是**超时的那些必须照旧把那句话摆出来（判据要是写成
            // "前缀是超时"之外的东西，这一条是唯一能拦住"全都缩成两个字"的断言）。
            Dlg::ServerRowStatus refused = timedOut;
            refused.timedOut = false;
            refused.error = QStringLiteral("没有回应（对面没开，或者端口不对）");
            check(Dlg::serverStatusLines(refused) == QStringList({refused.error}),
                  "U10L 不是超时的没通照旧写那句话（别把整档都缩成两个字）",
                  Dlg::serverStatusLines(refused).join(QStringLiteral("︱")));

            // 真要是空的也别留一格空白 —— 那就是"看着像没测过"。
            Dlg::ServerRowStatus blank;
            blank.tested = true;
            check(!Dlg::serverStatusText(blank).isEmpty() &&
                      Dlg::serverStatusText(blank) != Dlg::serverUntestedText(),
                  "★ U10L 通没通都没说、又没带理由时兜一句（空着和没测过长得一样）",
                  Dlg::serverStatusText(blank));

            Dlg::ServerRowStatus live;
            live.tested = true;
            live.online = true;
            live.latencyMs = 42;
            live.playersOnline = 7;
            live.playersMax = 20;
            // 「在线」那两个字 傍晚删了（要求：「把服务器那个 在线
            // 文本删了，只保留延迟和人数」）—— 通了的这一档第一行**就是数字**。
            check(Dlg::serverStatusText(live) == QStringLiteral("42 ms / 7/20 人"),
                  "★ U10L 通了 ⇒「延迟 / 人数」（没有「在线」两个字）",
                  Dlg::serverStatusText(live));

            // 服务器自报的版本名（非常用属性）：只进悬停提示末尾，不进格子。
            check(Dlg::serverStatusTooltip(live) == Dlg::serverStatusText(live),
                  "U10L 没问到版本名时悬停提示就是状态那句话（不多一格空尾巴）",
                  Dlg::serverStatusTooltip(live));
            Dlg::ServerRowStatus versioned = live;
            versioned.versionName = QStringLiteral("  Paper 1.21.4  ");
            check(Dlg::serverStatusTooltip(versioned) ==
                      QStringLiteral("42 ms / 7/20 人 / Paper 1.21.4") &&
                      Dlg::serverStatusText(versioned) == QStringLiteral("42 ms / 7/20 人"),
                  "★ U10L 服务器报了版本名 ⇒ 悬停提示末尾挂上它（两头空白先去掉），"
                  "状态那格**一个字都不多**",
                  Dlg::serverStatusTooltip(versioned));
            Dlg::ServerRowStatus blankVersion = live;
            blankVersion.versionName = QStringLiteral("   ");
            check(Dlg::serverStatusTooltip(blankVersion) == Dlg::serverStatusText(live),
                  "U10L 版本名只有空白 ⇒ 当没问到（不写一个空尾巴「 / 」）",
                  Dlg::serverStatusTooltip(blankVersion));

            // 在线玩家名单（同一档非常用属性）：只进提示、最多列五个。
            Dlg::ServerRowStatus sampled = versioned;
            sampled.samplePlayers = {QStringLiteral("Steve"), QStringLiteral("Alex")};
            check(Dlg::serverStatusTooltip(sampled) ==
                      QStringLiteral("42 ms / 7/20 人 / Paper 1.21.4 / 在线：Steve、Alex 等"),
                  "★★ U10L 服务器给了名单 ⇒ 提示末尾挂「在线：…」（版本名仍在），"
                  "状态那格一个字都不多",
                  Dlg::serverStatusTooltip(sampled));
            Dlg::ServerRowStatus manyNames = live;
            manyNames.playersOnline = 7;
            manyNames.samplePlayers = {QStringLiteral("A"), QStringLiteral("B"),
                                       QStringLiteral("C"), QStringLiteral("D"),
                                       QStringLiteral("E"), QStringLiteral("F"),
                                       QStringLiteral("G")};
            check(Dlg::serverStatusTooltip(manyNames) ==
                      QStringLiteral("42 ms / 7/20 人 / 在线：A、B、C、D、E 等"),
                  "★ U10L 名单超过五个 ⇒ 只列前五个，后面加「等」（提示框不跟着无限长）",
                  Dlg::serverStatusTooltip(manyNames));
            Dlg::ServerRowStatus partialNames = live;
            partialNames.samplePlayers = {QStringLiteral("Steve"), QStringLiteral("Alex")};
            check(Dlg::serverStatusTooltip(partialNames) ==
                      QStringLiteral("42 ms / 7/20 人 / 在线：Steve、Alex 等"),
                  "★ U10L 服务器只列了一部分（在线 7 人、名单 2 个）⇒ 也补「等」，"
                  "不假装名单就是全部",
                  Dlg::serverStatusTooltip(partialNames));
            Dlg::ServerRowStatus blankNames = live;
            blankNames.samplePlayers = {QStringLiteral("   "), QString()};
            check(Dlg::serverStatusTooltip(blankNames) == Dlg::serverStatusText(live),
                  "U10L 名单里只有空白格子 ⇒ 当没给（不写一个空尾巴「 / 」）",
                  Dlg::serverStatusTooltip(blankNames));

            // 服务器没说人数（-1）和"一个人都没有"（0）不是一回事：前者不许写出人数，
            // 后者要写出 0/20 —— 混起来的话"它没说"会被显示成"这服空着"。
            Dlg::ServerRowStatus noPlayers = live;
            noPlayers.playersOnline = -1;
            noPlayers.playersMax = -1;
            check(Dlg::serverStatusText(noPlayers) == QStringLiteral("42 ms"),
                  "U10L 服务器没说人数时不写那一截（不是写成 0/0 人）",
                  Dlg::serverStatusText(noPlayers));
            Dlg::ServerRowStatus zero = live;
            zero.playersOnline = 0;
            zero.playersMax = 0;
            check(Dlg::serverStatusText(zero) == QStringLiteral("42 ms / 0/0 人"),
                  "U10L 0/0 是**它说了**「一个人都没有」，照写", Dlg::serverStatusText(zero));
            // 延迟量不出（-1）那一档：人数照写，只少一行。**不许**因此冒出「在线」。
            Dlg::ServerRowStatus noLatency = live;
            noLatency.latencyMs = -1;
            check(Dlg::serverStatusLines(noLatency) == QStringList({QStringLiteral("7/20 人")}),
                  "U10L 没量到延迟时只剩人数那一行（不写「在线」占位）",
                  Dlg::serverStatusLines(noLatency).join(QStringLiteral("︱")));
            // 两个都没有 = 通了但一个数都没量到 ⇒ 兜一句「在线」（不然这一格空着，和
            // "还没测"在屏幕上是同一个样子）。生产里到不了，见 serverStatusLines 那段。
            Dlg::ServerRowStatus bare = noLatency;
            bare.playersOnline = -1;
            bare.playersMax = -1;
            check(Dlg::serverStatusLines(bare) == QStringList({QStringLiteral("在线")}),
                  "U10L 一个数都没有时才兜「在线」两个字（空着和没测过长得一样）",
                  Dlg::serverStatusLines(bare).join(QStringLiteral("︱")));
        }

        // ---- U10L-4 灌一茬结果进去：图标 / MOTD / 状态 / tooltip 各落各的格 ----
        QPixmap iconPixmap(MainWindowUI::serverIconSize(), MainWindowUI::serverIconSize());
        iconPixmap.fill(QColor(0x11, 0x88, 0x44));
        const QIcon icon(iconPixmap);
        const QString motd = QStringLiteral("欢迎来到我们的服 第二行");

        QList<Dlg::ServerRowStatus> statuses;
        Dlg::ServerRowStatus live;
        live.tested = true;
        live.online = true;
        live.latencyMs = 42;
        live.playersOnline = 7;
        live.playersMax = 20;
        live.motd = motd;
        live.icon = icon;
        live.versionName = QStringLiteral("Paper 1.21.4");
        live.samplePlayers = {QStringLiteral("Steve"), QStringLiteral("Alex")};
        statuses.append(live);
        Dlg::ServerRowStatus dead;
        dead.tested = true;
        dead.online = false;
        dead.error = QStringLiteral("没有回应（对面没开，或者端口不对）");
        statuses.append(dead);

        dialog.setServerStatuses(statuses);
        QTreeWidgetItem *liveItem = tree->topLevelItem(0);
        QTreeWidgetItem *deadItem = tree->topLevelItem(1);
        QTreeWidgetItem *untestedItem = tree->topLevelItem(2);

        check(!liveItem->icon(0).isNull() &&
                  liveItem->icon(0).pixmap(MainWindowUI::serverIconSize(),
                                           MainWindowUI::serverIconSize())
                          .toImage()
                          .pixelColor(MainWindowUI::serverIconSize() / 2,
                                      MainWindowUI::serverIconSize() / 2) ==
                      QColor(0x11, 0x88, 0x44),
              "★ U10L 服务器图标落在 `Qt::DecorationRole` 上、而且还是**那一枚**"
              "（代理就是读这一格画的）");
        check(deadItem->icon(0).isNull(),
              "U10L 没通的那一台没有图标（不是留着上一次那一枚）");
        check(liveItem->data(0, MainWindowUI::rowSubTextRole()).toString() == motd,
              "★ U10L MOTD 落在第二行小字那个角色位上（版本树放发布时间的那一格）",
              liveItem->data(0, MainWindowUI::rowSubTextRole()).toString());
        check(liveItem->toolTip(0) == motd,
              "U10L 名字那一格挂一句 tooltip：MOTD 装不下时是被省略号截掉的，"
              "整行只有这一处能看全",
              liveItem->toolTip(0));
        check(deadItem->data(0, MainWindowUI::rowSubTextRole()).toString().isEmpty() &&
                  deadItem->toolTip(0).isEmpty(),
              "★ U10L 没通的那一台 MOTD 那一格和 tooltip 都是空的（不留别人的话）");
        // 状态那一格是**两行**（用户 ：「在线延迟和人数换到两行来」），
        // 而且通的那一档**不带「在线」两个字**（当天傍晚的要求）。
        check(liveItem->text(2) == QStringLiteral("42 ms\n7/20 人") &&
                  deadItem->text(2) == dead.error &&
                  untestedItem->text(2) == Dlg::serverUntestedText(),
              "★ U10L 三行的状态那一格各是各的（通了是两行、没通一行、还没测一行）",
              QStringLiteral("「%1」「%2」「%3」")
                  .arg(liveItem->text(2), deadItem->text(2), untestedItem->text(2)));
        check(liveItem->toolTip(2) ==
                  QStringLiteral("42 ms / 7/20 人 / Paper 1.21.4 / 在线：Steve、Alex 等") &&
                  liveItem->text(2) == QStringLiteral("42 ms\n7/20 人"),
              "★ U10L 状态那一格的 tooltip 还是那一句**单行**的（版本名、名单挂在末尾），"
              "格子里仍是两行、一个字都不多（提示框里没有「两行」这回事；"
              "而且版本名和名单不是「现在几个人」）",
              liveItem->toolTip(2));

        // ---- U10L-5 行数和状态数对不上时：多的丢掉、缺的回到「还没测」----
        dialog.setServerStatuses(statuses);  // 先把三行都写成有内容的样子
        QList<Dlg::ServerRowStatus> shorter;
        shorter.append(live);  // 只给第一台
        dialog.setServerStatuses(shorter);
        check(tree->topLevelItem(0)->text(2) == QStringLiteral("42 ms\n7/20 人") &&
                  tree->topLevelItem(1)->text(2) == Dlg::serverUntestedText() &&
                  tree->topLevelItem(2)->text(2) == Dlg::serverUntestedText(),
              "★ U10L 状态比行**少**时，多出来的那几行回到「还没测」"
              "（不是留着上一趟那几句 —— 那些话配的是上一份列表的行）",
              QStringLiteral("「%1」「%2」「%3」")
                  .arg(tree->topLevelItem(0)->text(2), tree->topLevelItem(1)->text(2),
                       tree->topLevelItem(2)->text(2)));
        check(tree->topLevelItem(1)->data(0, MainWindowUI::rowSubTextRole()).toString().isEmpty(),
              "U10L 而且那些行的 MOTD 那一格也一起清掉了");

        QList<Dlg::ServerRowStatus> longer = statuses;
        longer.append(live);
        longer.append(live);
        dialog.setServerStatuses(longer);
        check(tree->topLevelItemCount() == 3 &&
                  tree->topLevelItem(2)->text(2) == QStringLiteral("42 ms\n7/20 人"),
              "★ U10L 状态比行**多**时多出来的直接丢掉（不越界、也不凭空长出一行）",
              QStringLiteral("%1 行").arg(tree->topLevelItemCount()));

        // ---- U10L-6 换一份列表 ⇒ 上一份那些"现在什么样"一个字都不留 ----
        dialog.setServerList({qMakePair(QStringLiteral("换了一份"),
                                        QStringLiteral("mc.example.com"))},
                             QString());
        check(tree->topLevelItemCount() == 1 &&
                  tree->topLevelItem(0)->text(2) == Dlg::serverUntestedText() &&
                  tree->topLevelItem(0)->icon(0).isNull() &&
                  tree->topLevelItem(0)->data(0, MainWindowUI::rowSubTextRole())
                      .toString()
                      .isEmpty(),
              "★ U10L 「刷新」（重读 servers.dat）之后状态全回到「还没测」"
              "—— 旧的那几句配的是旧的那些行");

        // ---- U10L-7 「刷新」发它那一条信号；重测那期间点不动 ----
        int refreshes = 0;
        int adds = 0;
        QObject::connect(&dialog, &Dlg::refreshServerListRequested, &dialog,
                         [&refreshes]() { ++refreshes; });
        QObject::connect(&dialog, &Dlg::addServerRequested, &dialog,
                         [&adds]() { ++adds; });

        check(refresh->text() == Dlg::serverRefreshText(), "U10L 「刷新」上写的是「刷新」",
              refresh->text());
        refresh->click();
        check(refreshes == 1,
              "★ U10L 点「刷新」发 refreshServerListRequested（那条路上重读 + 重测）",
              QStringLiteral("refresh=%1").arg(refreshes));

        // 重读那一半是 MainWindow 的事、要等它回来；这一层手上还是**一行都没选中**
        // ⇒ 「删除」灰着，而「添加」照旧点得动（它不依赖选中行）。
        check(tree->currentItem() == nullptr && !remove->isEnabled() && add->isEnabled(),
              "★ U10L 一行都没选中时：「删除」灰着、「添加」照旧点得动"
              "（要加的那一台还没出现在表里，它没有「先选中」这一步）");
        add->click();
        check(adds == 1, "★ U10L 点「添加」发一条 addServerRequested（**不带参数**）",
              QStringLiteral("add=%1").arg(adds));

        dialog.setServerPinging(true);
        check(refresh->text() == Dlg::serverPingingText() && !refresh->isEnabled(),
              "★ U10L 重测期间「刷新」换成「刷新中…」而且**点不动**"
              "（连点三下发三遍请求是用户没要的，每一遍都要等满超时）",
              QStringLiteral("「%1」enabled=%2")
                  .arg(refresh->text())
                  .arg(refresh->isEnabled() ? 1 : 0));
        const int refreshesBefore = refreshes;
        refresh->click();
        check(refreshes == refreshesBefore, "U10L 灰着的时候点了也没反应");
        dialog.setServerPinging(false);
        check(refresh->text() == Dlg::serverRefreshText() && refresh->isEnabled(),
              "U10L 重测完了按钮变回「刷新」、也点得动了", refresh->text());
        refresh->click();
        check(refreshes == refreshesBefore + 1, "U10L 变回来之后点得动");

        // ---- U10L-7b 那五颗各是哪一档（颜色就落在这儿，）----
        //
        // 「这个画出来是什么色」在**别处**量（`widgetStyleSheet()` 那几条选择器 ⇒
        // W12 那一段、以及主题那一节）。这一层该钉的是**这一页的哪一颗挂了哪一档**：
        // 挂错了不会报错、不会崩，只体现在"用户按下去之前心里那一句判断"上 ——
        // 「删除」要是没红，用户就分不出它和「刷新」有什么不一样。
        //
        // NOTE: 没挂属性 = `Accent`（见 `Theme::ButtonKind` 那条 NOTE：界面上一百来颗
        // 按钮绝大多数就是普通按钮，犯不着每建一颗都喊一遍）。
        {
            const auto kindOf = [](QPushButton *button) {
                const QVariant value = button->property("wtfmlButtonKind");
                return value.isValid() ? value.toString()
                                       : Theme::buttonKindKey(Theme::ButtonKind::Accent);
            };
            const QString neutral = Theme::buttonKindKey(Theme::ButtonKind::Neutral);
            const QString danger = Theme::buttonKindKey(Theme::ButtonKind::Danger);
            check(kindOf(refresh) == neutral && kindOf(dialog.serverEditButton()) == neutral &&
                      kindOf(add) == neutral,
                  "★ U10L 「刷新」「编辑」「添加」都是**一般性**那一档"
                  "（黑字黑边：看一眼 / 挪个地方，不是「该按的那一颗」）",
                  QStringLiteral("%1 / %2 / %3")
                      .arg(kindOf(refresh), kindOf(dialog.serverEditButton()), kindOf(add)));
            check(kindOf(remove) == danger,
                  "★★ U10L 「删除」是**不可撤销**那一档（红字红边）—— 删掉的那一台，"
                  "用户得在按下去之前就知道这件事回不来",
                  kindOf(remove));
            check(kindOf(dialog.serverJoinButton()) != kindOf(remove) &&
                      kindOf(dialog.serverJoinButton()) ==
                          Theme::buttonKindKey(Theme::ButtonKind::Accent),
                  "★★ U10L 「加入服务器」是**主题色**那一档，而且和「删除」**不是同一档**"
                  "（这两颗在屏幕上是挨着的两颗白底按钮，就靠字色和描边分 —— "
                  "同一档的话用户分不出哪一颗是「要开游戏」、哪一颗是「删掉」）",
                  QStringLiteral("加入=%1 删除=%2")
                      .arg(kindOf(dialog.serverJoinButton()), kindOf(remove)));
        }

        // ---- U10L-8 地址那一栏：默认原文，「隐藏服务器地址」打开后遮起来，右键整行弹菜单 ----
        //
        // 要求：「域名就显示首个字母+一条横杠（如 m-.h-.c- 来表示
        // mc.hyperlands.cloud），悬停在上方时才显示完整的，ipv4 的显示为 123.45.*.*，
        // ipv6 显示为 [123:123:*:*:*:*:*:*]」。
        {
            struct MaskCase {
                const char *input;
                const char *expected;
                const char *what;
            };
            const MaskCase cases[] = {
                {"mc.hyperlands.cloud", "m-.h-.c-", "域名：每段一个首字母 + 一条横杠"},
                {"example.com", "e-.c-", "两段的域名"},
                {"play.mc.example.com:25566", "p-.m-.e-.c-:25566",
                 "端口照抄（它不是身份，而且同一台机器上好几个服就靠它分）"},
                {"123.45.67.89", "123.45.*.*", "IPv4：留头两段、后面打掉（用户给的样子）"},
                {"123.45.67.89:25566", "123.45.*.*:25566", "IPv4 带端口"},
                {"[2001:db8:1:2:3:4:5:6]", "[2001:db8:*:*:*:*:*:*]",
                 "IPv6：方括号留着、留头两段（用户给的样子）"},
                {"[2001:db8::1]:25566", "[2001:db8::*]:25566",
                 "压缩写法的 IPv6：那个空段**留着空**（它就是 `::` 本身）—— 填成 `*` 的话"
                 "遮出来是 `[2001:db8:*:*]`，四个段、一个双冒号都没有，看着像另一个地址"},
                {"[::1]", "[::*]", "回环那种 IPv6 也不会把自己露出来"},
                {"  mc.example.com  ", "m-.e-.c-", "两头空白照旧去掉"},
                // 认不出来就**原样回**：这一格只是个显示，猜出来的东西比不猜更坏。
                {"", "", "空串回空串（不编一个假名字出来）"},
                {"localhost", "l-", "没有点的名字也遮（留一个首字母）"},
                {"a..b", "a-..b-", "坏地址里空的那一段保持空（不连成 a-.b- 那种假名字）"},
            };
            for (const MaskCase &item : cases) {
                const QString got =
                    Dlg::maskedServerAddress(QString::fromUtf8(item.input));
                check(got == QString::fromUtf8(item.expected),
                      QStringLiteral("★ U10L 遮地址：%1").arg(QString::fromUtf8(item.what)),
                      QStringLiteral("「%1」⇒「%2」，期望「%3」")
                          .arg(QString::fromUtf8(item.input), got,
                               QString::fromUtf8(item.expected)));
            }
            check(Dlg::maskedServerAddress(QStringLiteral("mc.hyperlands.cloud"))
                      .contains(QStringLiteral("hyperlands")) == false,
                  "★★ U10L 遮过的写法里**不出现**原来那个域名 "
                  "（这一条是这一整段的要害：上面那些逐字对的是「遮成什么样」，"
                  "这一条对的是「真的没露」）",
                  Dlg::maskedServerAddress(QStringLiteral("mc.hyperlands.cloud")));

            // ---- 默认：显示用户填的原文（域名、IP、IPv6 方括号、端口都照原样，没填端口不补）----
            const QString hideKey = QStringLiteral("Look/HideServerAddress");
            QSettings().remove(hideKey);
            const QString address = QStringLiteral("mc.hyperlands.cloud:25566");
            const QList<QPair<QString, QString>> rows = {
                qMakePair(QStringLiteral("我的服"), address),
                qMakePair(QStringLiteral("IP"), QStringLiteral("123.45.67.89")),
                qMakePair(QStringLiteral("IPv6"), QStringLiteral("[2001:db8::1]:25566")),
                qMakePair(QStringLiteral("无端口"), QStringLiteral("play.example.net"))};
            dialog.setServerList(rows, QString());
            for (int i = 0; i < rows.size(); ++i) {
                QTreeWidgetItem *row = tree->topLevelItem(i);
                check(row->text(1) == rows.at(i).second && row->toolTip(1) == rows.at(i).second &&
                          row->data(1, MainWindowUI::serverAddressRole()).toString() == rows.at(i).second,
                      QStringLiteral("★★ U10L 默认显示原文（%1）：格子、悬停、完整地址三处都是用户填的那一串")
                          .arg(rows.at(i).first),
                      QStringLiteral("格「%1」提示「%2」").arg(row->text(1), row->toolTip(1)));
            }

            // ---- 「隐藏服务器地址」打开：遮蔽，悬停不露完整地址，完整地址仍挂在 role 上 ----
            QSettings().setValue(hideKey, true);
            dialog.setServerList(rows, QString());
            QTreeWidgetItem *item = tree->topLevelItem(0);
            check(item->text(1) == QStringLiteral("m-.h-.c-:25566") && item->toolTip(1).isEmpty(),
                  "★★ U10L 开关打开 ⇒ 格子是遮过的，悬停提示里没有完整地址",
                  QStringLiteral("格「%1」提示「%2」").arg(item->text(1), item->toolTip(1)));
            check(tree->topLevelItem(1)->text(1) == QStringLiteral("123.45.*.*") &&
                      tree->topLevelItem(2)->text(1) == QStringLiteral("[2001:db8::*]:25566"),
                  "U10L 开关打开 ⇒ IP 留前两段、IPv6 方括号和端口留着",
                  tree->topLevelItem(1)->text(1) + QLatin1Char('/') + tree->topLevelItem(2)->text(1));
            check(item->data(1, MainWindowUI::serverAddressRole()).toString() == address,
                  "★★ U10L 开关打开时完整地址仍挂在第 1 列的 serverAddressRole 上（编辑、加入、复制都取它）",
                  item->data(1, MainWindowUI::serverAddressRole()).toString());
            QSettings().remove(hideKey);

            // ---- 过长省略中间：开头和端口都留着，放得下的原样 ----
            {
                const QFontMetrics metrics(tree->font());
                const QString longAddress = QStringLiteral("very-long-subdomain.of.some.example.network.com:25566");
                const int width = metrics.horizontalAdvance(QStringLiteral("very-long-sub…:25566"));
                const QString shown = Dlg::elidedServerAddress(longAddress, metrics, width);
                check(shown.startsWith(QStringLiteral("very")) && shown.endsWith(QStringLiteral(":25566")) &&
                          shown.contains(QChar(0x2026)) && metrics.horizontalAdvance(shown) <= width,
                      "★★ U10L 长地址省略中间：开头和 :端口 都在，宽度不超", shown);
                check(Dlg::elidedServerAddress(address, metrics, 4000) == address,
                      "U10L 放得下的地址原样返回");
                const QString noPort = Dlg::elidedServerAddress(
                    QStringLiteral("very-long-subdomain.of.some.example.network.com"), metrics, width);
                check(noPort.startsWith(QStringLiteral("very")) && noPort.endsWith(QStringLiteral(".com")) &&
                          noPort.contains(QChar(0x2026)),
                      "U10L 没端口的长地址：中间省略、两头留着", noPort);
            }

            // ---- 右键整行 ⇒ 手搭浮层「加入 / 编辑 / 复制地址」----
            {
                dialog.setServerJoinText(Dlg::serverJoinText(), Dlg::serverJoinTip());
                dialog.setServerList(rows, QString());
                QString copied;
                QString editedName;
                QString editedAddress;
                QString joinedAddress;
                const QMetaObject::Connection links[] = {
                    QObject::connect(&dialog, &Dlg::copyServerAddressRequested,
                                     [&copied](const QString &text) { copied = text; }),
                    QObject::connect(&dialog, &Dlg::editServerRequested,
                                     [&](const QString &name, const QString &text) {
                                         editedName = name;
                                         editedAddress = text;
                                     }),
                    QObject::connect(&dialog, &Dlg::joinServerRequested,
                                     [&joinedAddress](const QString &, const QString &text) {
                                         joinedAddress = text;
                                     })};
                const auto rightClick = [&](int row) {
                    const QRect cell = tree->visualRect(tree->model()->index(row, 0));
                    QContextMenuEvent event(QContextMenuEvent::Mouse, cell.center(),
                                            tree->viewport()->mapToGlobal(cell.center()));
                    QApplication::sendEvent(tree->viewport(), &event);
                };
                const auto findMenu = [&dialog] { return dialog.findChild<QWidget *>(Dlg::serverMenuName()); };
                const auto menuEntry = [&](const QString &name) -> QPushButton * {
                    auto *popup = findMenu();
                    return popup != nullptr ? popup->findChild<QPushButton *>(name) : nullptr;
                };

                tree->clearSelection();
                rightClick(0);
                check(findMenu() != nullptr && findMenu()->isVisible() && findMenu()->windowFlags().testFlag(Qt::Popup),
                      "★★ U10L 右键一行 ⇒ 弹出手搭的 Qt::Popup 浮层");
                check(tree->selectedItems().size() == 1 && tree->selectedItems().first() == tree->topLevelItem(0),
                      "★★ U10L 右键时先选中那一行");
                auto *join = menuEntry(Dlg::serverMenuJoinName());
                auto *edit = menuEntry(Dlg::serverMenuEditName());
                auto *copy = menuEntry(Dlg::serverMenuCopyName());
                check(join != nullptr && edit != nullptr && copy != nullptr &&
                          join->text() == Dlg::serverMenuJoinText() && edit->text() == Dlg::serverEditText() &&
                          copy->text() == Dlg::serverMenuCopyText() && join->isEnabled() && edit->isEnabled() &&
                          copy->isEnabled(),
                      "★★ U10L 菜单三项「加入 / 编辑 / 复制地址」都在、都可点");
                if (edit != nullptr) edit->click();
                check(editedName == QStringLiteral("我的服") && editedAddress == address,
                      "★★ U10L 菜单「编辑」和下方按钮同一处理：发出原名 + 完整地址",
                      editedName + QLatin1Char('/') + editedAddress);
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                check(findMenu() == nullptr, "U10L 点完一项菜单收起");

                // 开关打开时，复制的仍是完整地址；「加入」同样走下方那颗的处理。
                QSettings().setValue(hideKey, true);
                dialog.setServerList(rows, QString());
                rightClick(1);
                if (auto *entry = menuEntry(Dlg::serverMenuCopyName())) entry->click();
                check(copied == rows.at(1).second,
                      "★★ U10L 开关打开时「复制地址」复制的是完整地址", copied);
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                rightClick(2);
                if (auto *entry = menuEntry(Dlg::serverMenuJoinName())) entry->click();
                check(joinedAddress == rows.at(2).second,
                      "U10L 菜单「加入」和下方按钮同一处理：发出完整地址", joinedAddress);
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QSettings().remove(hideKey);

                // 版本没有快速加入参数（下方那颗收着）时，菜单里的「加入」灰着。
                dialog.setServerJoinText(QString(), QString());
                rightClick(0);
                check(menuEntry(Dlg::serverMenuJoinName()) != nullptr && !menuEntry(Dlg::serverMenuJoinName())->isEnabled(),
                      "U10L 下方「加入」收着时菜单里的「加入」灰着");
                if (auto *popup = findMenu()) popup->close();
                QApplication::processEvents();
                for (const auto &link : links) QObject::disconnect(link);
            }
        }

        // ---- U10L-9 状态那一格：两行是怎么拆的（纯函数）----
        {
            Dlg::ServerRowStatus live;
            live.tested = true;
            live.online = true;
            live.latencyMs = 42;
            live.playersOnline = 7;
            live.playersMax = 20;
            const QStringList lines = Dlg::serverStatusLines(live);
            check(lines == QStringList({QStringLiteral("42 ms"),
                                        QStringLiteral("7/20 人")}),
                  "★ U10L 通了 ⇒ 两行：第一行延迟、第二行人数（没有「在线」两个字）",
                  lines.join(QStringLiteral("︱")));
            check(Dlg::serverStatusText(live) == lines.join(QStringLiteral(" / ")),
                  "★ U10L 单行那份就是这两行用「 / 」拼起来的"
                  "（所以格子里拆成两行之后，悬停那句一个字都没变）",
                  Dlg::serverStatusText(live));

            Dlg::ServerRowStatus noPlayers = live;
            noPlayers.playersOnline = -1;
            noPlayers.playersMax = -1;
            check(Dlg::serverStatusLines(noPlayers).size() == 1,
                  "★ U10L 服务器没说人数时**只有一行**（不是空着第二行、也不是写成 0/0 人）",
                  Dlg::serverStatusLines(noPlayers).join(QStringLiteral("︱")));

            Dlg::ServerRowStatus dead;
            dead.tested = true;
            dead.online = false;
            dead.error = QStringLiteral("没有回应（对面没开，或者端口不对）");
            check(Dlg::serverStatusLines(dead) == QStringList({dead.error}),
                  "★ U10L 没通那一档只有**一行**（那句话本来就长，拆两行更难读）",
                  Dlg::serverStatusLines(dead).join(QStringLiteral("︱")));

            // 要求：「‘上次’两个字也不要」——缓存那一份和当场测出来
            // 那一份现在**逐字相同**，一个前缀都没有。
            Dlg::ServerRowStatus cached = live;
            cached.fromCache = true;
            check(Dlg::serverStatusLines(cached) == Dlg::serverStatusLines(live) &&
                      Dlg::serverStatusLines(cached) ==
                          QStringList({QStringLiteral("42 ms"), QStringLiteral("7/20 人")}),
                  "★★ U10L 缓存那一份通了 ⇒ 和当场测出来的**逐字相同**"
                  "（没有「上次：」那种过去时的前缀了）",
                  Dlg::serverStatusLines(cached).join(QStringLiteral("︱")));

            // 补，要求：「服务器那个会显示上次:没连上，也改成超时」，
            // 接着又说「‘上次’两个字也不要」。盘上那份"上次问到"的没通也带着"是不是
            // 超时"这一位（见 ServerStatusCache::Entry::timedOut），所以这一格和刚测完
            // 那一格**同一个字**。
            Dlg::ServerRowStatus cachedTimeout;
            cachedTimeout.tested = true;
            cachedTimeout.online = false;
            cachedTimeout.timedOut = true;
            cachedTimeout.fromCache = true;
            check(Dlg::serverStatusLines(cachedTimeout) ==
                      QStringList({Dlg::serverTimeoutText()}),
                  "★★ U10L 缓存里那一份「上次是超时」⇒ 写两个字「超时」"
                  "（和刚测完那一格一模一样）",
                  Dlg::serverStatusLines(cachedTimeout).join(QStringLiteral("︱")));
            // 反过来那条同样要钉：老缓存（没存这一位的那一份）读出来 timedOut 是
            // false ⇒ 那个词还是「没连上」，不是一律写「超时」。
            Dlg::ServerRowStatus cachedRefused = cachedTimeout;
            cachedRefused.timedOut = false;
            check(Dlg::serverStatusLines(cachedRefused) ==
                      QStringList({QStringLiteral("没连上")}),
                  "★ U10L 而老缓存（不知道上一次是怎么没通的）写「没连上」"
                  "—— 不是替它编一句「超时」",
                  Dlg::serverStatusLines(cachedRefused).join(QStringLiteral("︱")));
            // 最后一条：这一格上**任何时候**都不许再冒出那两个字（前缀是从这儿删的，
            // 别处又加回来就是同一句话复活）。
            check(!Dlg::serverStatusText(cachedTimeout).contains(QStringLiteral("上次")) &&
                      !Dlg::serverStatusText(cached).contains(QStringLiteral("上次")),
                  "★★ U10L 状态那一格从此不出现「上次」两个字（缓存那一份也不）",
                  Dlg::serverStatusText(cachedTimeout));
        }

        // ---- U10L-10 三栏的宽度是这么分的 ----
        //
        // 要求「三栏，让最右边（在线）那个固定靠右，保证左侧的 motd 能显示全」。
        // 默认那套是"最后一列拉伸"，多出来的宽度全给了状态那一栏 —— 左边两行 MOTD
        // 会被挤掉一大截。所以这里逐条钉住三列各自的伸缩方式。
        {
            QHeaderView *header = tree->header();
            check(header != nullptr, "U10L 那张表有表头");
            if (header != nullptr) {
                check(!header->stretchLastSection(),
                      "★★ U10L 关掉了「最后一列拉伸」（不关的话多出来的宽度全归状态那一栏）");
                check(header->sectionResizeMode(0) == QHeaderView::Stretch,
                      "★★ U10L 第 0 列（名字 + MOTD）**吃掉剩下的全部宽度** —— "
                      "这一条就是「保证左侧的 motd 能显示全」的落法");
                check(header->sectionResizeMode(1) == QHeaderView::Interactive &&
                          header->sectionResizeMode(2) == QHeaderView::Interactive,
                      "U10L 地址和状态那两栏按内容定宽（地址是遮过的、本来就窄）");
            }
            check(tree->topLevelItemCount() > 0 &&
                      tree->topLevelItem(0)->textAlignment(2) ==
                          (Qt::AlignRight | Qt::AlignVCenter),
                  "★ U10L 状态那一格**靠右**（表头那一格也是，见下面那条）",
                  QStringLiteral("%1").arg(int(tree->topLevelItem(0)->textAlignment(2))));
            check(tree->headerItem() != nullptr &&
                      tree->headerItem()->textAlignment(2) ==
                          (Qt::AlignRight | Qt::AlignVCenter),
                  "★ U10L 表头的「状态」两个字也跟着靠右（不然它和底下的数字错开一个身位）");
        }

        // ---- U10L-11 两行**真的画成了两行**（量像素）----
        //
        // 上面那些只证明了"格子里那个字符串里有一个 `\\n`"。它在屏幕上会不会真换行，
        // 取决于控件开没开 wordWrap + 条目代理怎么摆 —— 不开的话那个 `\\n` 是一个
        // **看不见的字符**，两行会挤成一行，而这件事**一声不响**。
        {
            dialog.setServerList({qMakePair(QStringLiteral("我的服"),
                                            QStringLiteral("mc.example.com"))},
                                 QString());
            QList<Dlg::ServerRowStatus> rows;
            Dlg::ServerRowStatus live;
            live.tested = true;
            live.online = true;
            live.latencyMs = 42;
            live.playersOnline = 7;
            live.playersMax = 20;
            rows.append(live);
            dialog.setServerStatuses(rows);

            check(tree->wordWrap(),
                  "★ U10L 那张表开着 wordWrap（不开的话下面那条量出来会是一行）");

            const QModelIndex index = tree->model()->index(0, 2);
            const QRect cell = tree->visualRect(index);
            check(cell.isValid() && cell.width() > 4 && cell.height() > 4,
                  "U10L 状态那一格量得出来",
                  QStringLiteral("%1x%2").arg(cell.width()).arg(cell.height()));
            if (cell.isValid() && !cell.isEmpty()) {
                // 量的是**控件自己画出来的样子**（`captureWidget` → `render()`），
                // 不是我们自己手搓一个 QStyleOptionViewItem 去调代理 —— 后者会把
                // "控件到底给代理喂了哪些位"这件事跳过去，而这一节要验的恰恰是那个
                // （wordWrap 有没有传下去、对齐有没有传下去）。
                const QImage shot = captureWidget(tree);
                const QRect inShot = cell.translated(tree->viewport()->mapTo(tree, QPoint(0, 0)));

                // 一行一行扫"这一行有没有墨"（字是深的、底是浅的）。
                QVector<int> inkRows;
                for (int y = inShot.top(); y <= inShot.bottom(); ++y) {
                    for (int x = inShot.left(); x <= inShot.right(); ++x) {
                        const QColor pixel = shot.pixelColor(x, y);
                        if (pixel.red() < 160 && pixel.green() < 160 && pixel.blue() < 160) {
                            inkRows.append(y - inShot.top());
                            break;
                        }
                    }
                }
                // 把连着的行并成"带"：两行字就是两条带，中间隔着一段空白。
                QVector<QPair<int, int>> bands;
                for (int y : inkRows) {
                    if (!bands.isEmpty() && y - bands.last().second <= 1) {
                        bands.last().second = y;
                    } else {
                        bands.append(qMakePair(y, y));
                    }
                }
                QStringList described;
                for (const QPair<int, int> &band : bands) {
                    described.append(QStringLiteral("%1~%2").arg(band.first).arg(band.second));
                }
                check(bands.size() == 2,
                      "★★ U10L 状态那一格在屏幕上**真的是两行**（`\\n` 不是个看不见的字符）"
                      " —— 扫出来是两条墨迹带：一条「42 ms」、一条「7/20 人」",
                      QStringLiteral("%1 条带：%2")
                          .arg(bands.size())
                          .arg(described.join(QStringLiteral("、"))));
                check(bands.size() == 2 && bands.at(0).second < bands.at(1).first,
                      "★ U10L 而且是**延迟在上、人数在下**（没有画反）",
                      described.join(QStringLiteral("、")));

                // 靠右：最下面那条带的右端得**贴着这一格的右边缘**。
                if (bands.size() == 2) {
                    int rightMost = -1;
                    for (int y = inShot.top() + bands.at(1).first;
                         y <= inShot.top() + bands.at(1).second; ++y) {
                        for (int x = inShot.right(); x >= inShot.left(); --x) {
                            const QColor pixel = shot.pixelColor(x, y);
                            if (pixel.red() < 160 && pixel.green() < 160 &&
                                pixel.blue() < 160) {
                                rightMost = qMax(rightMost, x - inShot.left());
                                break;
                            }
                        }
                    }
                    // 默认行代理在行尾留 8 的白（圆角块内缩 3 + 字离块边），靠右的字离格边约 13；左对齐会差一百多。
                    check(rightMost >= 0 && rightMost >= cell.width() - 16,
                          "★ U10L 而且那两行**贴着右边**（靠右对齐真的生效了）",
                          QStringLiteral("最右那个墨点在 x=%1，这一格宽 %2")
                              .arg(rightMost)
                              .arg(cell.width()));
                }
            }
        }
    }
    return 0;
}

#include "Support.hpp"
#include "WheelFocusFilter.hpp"
int UiWorldEditorChecks() {
    // ==================================================================
    // 要求：「世界根据其存档文件在存档设置（那支笔）里面编辑游戏规则等，打开之前
    // 弹出坏档警告并提供备份按钮」—— 这一页就是那句话里"在里面编辑"那一半。
    //
    // 这一层验的是**页面上摆的东西**：摆进去的那一份在不在格子里、改过之后收回来的是不是
    // 新的、**筛掉的那些会不会被顺手清空**（这是最容易写错的一格）、读不出来那一档藏得对
    // 不对。真读盘/真写盘那一趟不在这里（`build/verify` 的 ZL5 / ZL6）。
    {
        WorldSettingsPage page;
        page.show();

        const auto settleFilter = [] {
            QElapsedTimer span;
            span.start();
            while (span.elapsed() < 350) {
                QApplication::processEvents();
            }
        };
        settleFilter();   // 让 `page.show()` 之后还悬着的那些事件先落地

        // **还没摆过任何世界**的这一页必须是干净的。
        //
        // 这一条钉的是"那四格一律可动、`editedView()` 也一律收"：于是"难度"
        // 那一格收上来的是**下拉框的当前项**，而刚建出来的下拉框停在第一项（0 和平），
        // 基线的 `WorldView()` 里写的却是 2（普通）。两个对不上 ⇒ 这一页凭空是脏的。
        //
        // 后果不是"少问一句"，而是**反过来**：`InstanceSettingsDialog::reject()` 拿
        // `isDirty()` 挡顶上的返回箭头 ⇒ 用户开一下实例设置、什么都没改，按返回**关不掉**
        // （那一头接到的是"改过还没存，丢不丢"）。所以这一条钉的是"构造完就是干净的"。
        check(!page.isDirty(),
              "★★ Z16a 刚 new 出来、还没 setWorld 的那一页是**干净的**（脏的话，实例设置"
              "顶上那颗返回箭头会被一句没影的「改过还没存」挡下来 —— 界面上就是按了没反应）");

        WorldSettingsPage::WorldView view;
        view.dirName = QStringLiteral("我的世界");
        view.hasLevelName = true;
        view.levelName = QStringLiteral("我的生存");
        view.hasVersionName = true;
        view.versionName = QStringLiteral("1.20.1");
        view.hasFormat = true;
        view.formatText = QStringLiteral("3465");
        view.hasLastPlayed = true;
        view.lastPlayedMs = 1758960000000LL;
        // 只读概况：大小 / 种子 / 玩家位置 / 已生成区域。
        view.hasSize = true;
        view.sizeText = QStringLiteral("123.4 MB");
        view.hasSeed = true;
        view.seedText = QStringLiteral("1234567890123");
        view.hasPlayer = true;
        view.playerText = QStringLiteral("12.5 / 64.0 / -30.2（主世界）");
        view.hasRegions = true;
        view.regionsText = QStringLiteral("3 个区域文件 / 已生成 1234 个区块 / 范围 x 0 ~ 511、z 0 ~ 511");
        view.mapNote = QStringLiteral("这份统计只看主世界 region/ 里的 .mca。");
        view.gzipped = true;
        view.hasAllowCommands = true;
        view.allowCommands = false;
        view.hasHardcore = true;
        view.hardcore = false;
        view.hasDifficulty = true;
        view.difficulty = 2;
        view.hasRules = true;
        view.rules = {
            {QStringLiteral("keepInventory"), QStringLiteral("true"), WorldSettingsPage::Kind::Bool},
            {QStringLiteral("randomTickSpeed"), QStringLiteral("3"), WorldSettingsPage::Kind::Int},
            // 第三条 `inFile = false`：**盘上没有这一条**（主程序摆进来的是并过全集的
            // 那一列），屏幕上摆的是游戏默认值 —— 界面上要挂一枚「默认」小标记。
            {QStringLiteral("motdStyle"), QStringLiteral("花哨"), WorldSettingsPage::Kind::Text,
             /*inFile=*/false},
        };
        page.setWorld(view);

        {
            // 行宽必须**跟着视口走**（用户那句"排版"要的就是这个）。
            //
            // 根因是 Qt 布局那一趟的次序：这一列的行是**在折叠框还收着的时候**建的（那时
            // 列表根本没有大小 —— 探针量到的是 100x0），摆开之后视图先按"还没有滚动条"的
            // 宽度摆了一遍（行 635、视口 621，差 14 = 滚动条宽），等滚动条露出来、视口缩窄，
            // **没有第二趟**回来改行的宽度。
            //
            // 所以这一条走的是**那条真路**：另起干干净净的一页 ⇒ 造行（框还收着）⇒ 第一次
            // 摆开 ⇒ 马上量。用外面那一页量不出来 —— 那一页已经摆开又收起过一次，
            // 多的那几趟布局把宽度自己捯饬整齐了，量出来恒绿（这就是"假绿"）。
            WorldSettingsPage probe;
            probe.show();
            probe.resize(900, 420);   // 挤到规则表非滚不可
            probe.setWorld(view);
            QApplication::processEvents();
            probe.setFileEditingOpen(true);
            QApplication::processEvents();
            QListWidget *plist = probe.ruleList();
            auto *pscroll = probe.findChild<QScrollArea *>();
            check(pscroll != nullptr && pscroll->verticalScrollBar()->maximum() > 0 &&
                      plist->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff &&
                      !plist->verticalScrollBar()->isVisible(),
                  "（前提）世界设置外层滚动区接管滚动，规则表随条目撑高且关掉自身竖滑条",
                  QStringLiteral("外层可滚 %1、表高 %2、行高 %3、行数 %4")
                      .arg(pscroll != nullptr ? pscroll->verticalScrollBar()->maximum() : -1)
                      .arg(plist->height())
                      .arg(plist->sizeHintForRow(0))
                      .arg(plist->count()));
            int bad = 0;
            QString worst;
            for (int row = 0; row < plist->count(); ++row) {
                QWidget *host = plist->itemWidget(plist->item(row));
                if (host == nullptr) {
                    continue;
                }
                if (host->width() != plist->viewport()->width()) {
                    ++bad;
                    if (worst.isEmpty()) {
                        worst = QStringLiteral("第 %1 行 %2、视口 %3")
                                    .arg(row)
                                    .arg(host->width())
                                    .arg(plist->viewport()->width());
                    }
                }
            }
            check(bad == 0,
                  "★★ Z16a 每一行的宽度都等于**视口宽**（宽一条滚动条的话，行尾那颗开关会被"
                  "视口切掉半个 —— 放大截图看得清清楚楚，看着像这一页坏了）",
                  worst.isEmpty()
                      ? QStringLiteral("全部对齐（视口 %1）").arg(plist->viewport()->width())
                      : worst);
        }

        // 要求：「下面的 gamerules 不会改坏吗，如果可能会的话也放到那个框里去」。
        // 游戏规则改的也是 `<存档目录>/level.dat`（`Data/GameRules` 那一格），和「世界名 /
        // 允许作弊 / 极限模式 / 难度」是**同一份文件**、同一档风险、同一句备份警告 ——
        // 所以它们现在一起收在「修改存档文件」那个框里（一起摊在屏幕上才是错的切法：
        // 那个切法按的是"游戏里能不能用 /gamerule 改"，而安全该按**动的是哪份文件**分）。
        //
        // 判据用 `parentWidget()`（**结构**：它归不归那个框管）+ `isHidden()`（那句
        // `setVisible(false)` 摆没摆），**不用** `isVisible()` —— 后者在"整层都还没露面"
        // 的场合恒假，量出来的是"这台机器上露没露"，不是"它在不在框里"。
        check(page.ruleList()->parentWidget() == page.fileBody() &&
                  page.ruleFilter()->parentWidget() == page.fileBody(),
              "★★ Z16a 规则表（连它的搜索框）是**那个折叠框的子控件** —— 规则也写在 level.dat "
              "里，和上面那四格同一份文件、同一档风险");
        check(page.fileBody()->isHidden() && page.fileToggle() != nullptr &&
                  !page.fileToggle()->isChecked(),
              "★★ Z16a 所以那个框默认收起时**一整列规则也看不见**（一进来不该有几十条规则"
              "摊在屏幕上）");

        check(page.breadcrumbTitle() == QStringLiteral("世界设置：我的世界"),
              "Z16a 面包屑上写的是**哪一个**存档（不看这个的话，改完了根本不知道改的是哪个世界）",
              page.breadcrumbTitle());
        check(page.nameEdit()->text() == QStringLiteral("我的生存"),
              "Z16a 世界名摆进格子里了");
        QLabel *infoLabel = page.findChild<QLabel *>(WorldSettingsPage::infoLabelName());
        check(infoLabel != nullptr &&
                  infoLabel->text().contains(QStringLiteral("游戏版本 1.20.1")),
              "Z16a 那行只读的事实里有游戏版本（缺的那几位不写，不拿 0 / 空串顶替）",
              infoLabel == nullptr ? QStringLiteral("找不到那行字") : infoLabel->text());
        // 那行事实多了四件只读的 —— 大小 / 种子 / 玩家位置 / 已生成区域。
        check(infoLabel != nullptr && infoLabel->text().contains(QStringLiteral("大小 123.4 MB")),
              "★★ Z16a 那行事实里有这个世界的**大小**（只读摘要）",
              infoLabel == nullptr ? QStringLiteral("找不到那行字") : infoLabel->text());
        check(infoLabel != nullptr && infoLabel->text().contains(QStringLiteral("种子 1234567890123")),
              "★★ Z16a 那行事实里有**种子**");
        check(infoLabel != nullptr &&
                  infoLabel->text().contains(QStringLiteral("玩家位置 12.5 / 64.0 / -30.2")),
              "★★ Z16a 那行事实里有**玩家位置**（世界地图第一阶段要的那一件）");
        check(infoLabel != nullptr &&
                  infoLabel->text().contains(QStringLiteral("已生成区域 3 个区域文件")),
              "★★ Z16a 那行事实里有**已生成区域数**");
        check(infoLabel != nullptr &&
                  infoLabel->text().contains(QStringLiteral("已生成 1234 个区块")),
              "★★ Z16a 已生成区域那句里还有区块数与范围");
        check(infoLabel != nullptr && infoLabel->toolTip() == view.mapNote,
              "★ Z16a 「结果可能不完整」那句说明挂在 tooltip 上（模组维度 / 坏的区域文件）",
              infoLabel == nullptr ? QString() : infoLabel->toolTip());
        {
            // 缺的那几件**整条不写**：从没进过的世界没有玩家位置，老档可能没有种子。
            WorldSettingsPage bare;
            WorldSettingsPage::WorldView sparse;
            sparse.dirName = QStringLiteral("空");
            sparse.hasVersionName = true;
            sparse.versionName = QStringLiteral("1.20.1");
            bare.setWorld(sparse);
            QLabel *bareInfo = bare.findChild<QLabel *>(WorldSettingsPage::infoLabelName());
            check(bareInfo != nullptr
                      && !bareInfo->text().contains(QStringLiteral("种子"))
                      && !bareInfo->text().contains(QStringLiteral("玩家位置"))
                      && !bareInfo->text().contains(QStringLiteral("已生成区域"))
                      && !bareInfo->text().contains(QStringLiteral("大小"))
                      && bareInfo->toolTip().isEmpty(),
                  "★★ Z16a 盘上没有的那几件**整条不出现**（不拿 0 / 空串顶替，也不留一句空说明）",
                  bareInfo == nullptr ? QStringLiteral("找不到那行字") : bareInfo->text());
        }
        check(!page.allowCommandsSwitch()->isChecked() && page.allowCommandsSwitch()->isEnabled(),
              "Z16a 「允许作弊」是**关**的，而且是能点的（盘上有这一项）");
        check(page.difficultyCombo()->currentData().toInt() == 2 &&
                  page.difficultyCombo()->currentText() ==
                      WorldSettingsPage::difficultyOptionText(2),
              "Z16a 难度摆的是盘上那个（2 = 普通），不是下拉框第一项",
              page.difficultyCombo()->currentText());
        check(!page.isDirty(),
              "★★ Z16a 刚摆进去是**干净的** —— 不干净的话，用户进来什么都没干就按返回箭头，"
              "会被问一句「改过还没存」");
        check(page.ruleList()->count() == 3, "Z16a 三条规则三条行",
              QString::number(page.ruleList()->count()));
        {
            // 症状（10j2 那张图）：那行只读的事实**悬在页面中间**、底下那颗「修改存档文件」
            // 被甩到页底。根因不是这一页写了什么字，是 `QVBoxLayout` 的默认规矩：**场上没有
            // 任何一项带 stretch 时，多余的高度平分给所有能长高的项** —— 框收着的时候那个
            // 折叠框里一列规则是空的（藏着的项在布局里直接跳过），场上就只剩那行字和这颗钮。
            // 修法见产品里 `m_bodyTail` 那一段（收尾挂一根弹簧，收着时 stretch = 1）。
            //
            // 判据量两样、都量**几何**：那行字不比自己需要的高（`heightForWidth()` 那一份
            // —— 它开着 `setWordWrap(true)`，按 900 宽算出来的可能是两行，`sizeHint()`
            // 只给一行），以及那颗钮紧贴在它下面（同父，y 可比）。
            page.resize(900, 700);
            QApplication::processEvents();
            QLabel *info = page.infoLabel();
            QAbstractButton *toggle = page.fileToggle();
            const int wrapH = info == nullptr ? -1 : info->heightForWidth(info->width());
            const int need = info == nullptr ? -1
                                             : (wrapH > 0 ? wrapH : info->sizeHint().height());
            check(info != nullptr && toggle != nullptr && info->height() <= need + 2,
                  "★★ Z16a 框收着时那行事实**不拉长**（拉长了就是多余高度被平分了 —— 屏幕上"
                  "是「字悬在页面中间、钮甩到页底」，看着像这一页坏了）",
                  info == nullptr ? QStringLiteral("找不到那行字")
                                  : QStringLiteral("高 %1、需要 %2（视口宽 %3）")
                                        .arg(info->height())
                                        .arg(need)
                                        .arg(info->width()));
            check(info != nullptr && toggle != nullptr &&
                      toggle->y() - (info->y() + info->height()) <= 12,
                  "★ Z16a 那颗「修改存档文件」紧贴在那行字下面（中间那一段空白就是用户说的"
                  "「诡异」）",
                  (info == nullptr || toggle == nullptr)
                      ? QStringLiteral("找不到那两样")
                      : QStringLiteral("字底 %1、钮顶 %2")
                            .arg(info->y() + info->height())
                            .arg(toggle->y()));
        }
        check(page.ruleList()->currentRow() < 0 && !page.ruleList()->hasFocus(),
              "★ Z16a 规则表里没有「当前项」（不然第 0 行会被整行染一遍，看着像被选中了）",
              QStringLiteral("currentRow = %1").arg(page.ruleList()->currentRow()));
        {
            // 这张表现在收在折叠框里，**必须先把它摆开**才量得到像素 ——
            // 藏着的控件没有过布局（视口大小是建出来那个默认值），`grab()` 出来的东西
            // 不反映真实摆法。量完再收回去（下面那一段量的是"默认收起"）。
            page.setFileEditingOpen(true);
            page.resize(900, 700);
            QApplication::processEvents();
            const QImage shot = page.ruleList()->viewport()->grab().toImage();
            const int h = page.ruleList()->sizeHintForRow(0);
            const QColor first = shot.pixelColor(shot.width() / 2, qMax(1, h / 2));
            const QColor second = shot.pixelColor(shot.width() / 2, qMax(1, h + h / 2));
            // NOTE: 根因是 `addItem()` 之后、视图**第一次布局**那一下 Qt 会把第 0 行设成
            // **当前项**（`setWorld()` 那一刻 `currentRow()` 还是 -1，`resize()`
            // 之后就变成 0），而 Fusion 画当前项用的是一道**竖渐变**；
            // `setSelectionMode(NoSelection)` 拦不住它（那一位管的是"选中"，管不着
            // "当前项"）。产品在**画的这一侧**掐掉（见 RuleItemDelegate —— 摆完内容
            // 清一下当前项没用：下一次布局它又回来）。
            check(first == second,
                  "★★ Z16a 第 0 行的底色和第 1 行**一模一样**（不一样的底色看着像"
                  "「这一条被选中了」）",
                  QStringLiteral("%1 vs %2（视口 %3x%4、行高 %5）")
                      .arg(first.name(), second.name())
                      .arg(shot.width())
                      .arg(shot.height())
                      .arg(h));
            page.setFileEditingOpen(false);  // 量完收回去（下一段量的是"默认收起"）
        }
        check(page.ruleList()->item(0)->data(WorldSettingsPage::ruleNameRole()).toString() ==
                  QStringLiteral("keepInventory"),
              "Z16a 那一行是哪一条看的是 item 上那个角色（**不是**行上写着的那串字 —— "
              "行上盖的是官方中文名）");
        {
            // 盖的这一层是 `GameRuleNames`（官方 `zh_cn.json` 里 `gamerule.<名字>` 那一条，
            // 出处见那个文件头）——**认不出来就原样回英文**，所以模组写的规则、比那张表还新
            // 的规则最坏也只是回到英文原样。英文名**没有丢**：它退到 tooltip 里了
            // （它才是写进 `level.dat` 的那个键，也是游戏里 `/gamerule` 要打的那个词）。
            QWidget *row0 = page.ruleList()->itemWidget(page.ruleList()->item(0));
            QLabel *name = row0 == nullptr
                               ? nullptr
                               : row0->findChild<QLabel *>(WorldSettingsPage::ruleNameLabelName());
            check(name != nullptr && name->text() == QStringLiteral("死亡后保留物品栏"),
                  "★★ Z16a 行上那一格盖的是**官方中文名**（英文原样摆着的话，普通玩家认不出"
                  "哪一条是哪一条）",
                  name == nullptr ? QStringLiteral("找不到那一格")
                                  : QStringLiteral("写的是「%1」").arg(name->text()));
            check(name != nullptr && name->toolTip().contains(QStringLiteral("keepInventory")),
                  "★ Z16a 英文原名**没丢**，退到 tooltip 里了（它是写进存档的键，也是游戏里"
                  "`/gamerule` 要打的那个词）",
                  name == nullptr ? QStringLiteral("找不到那一格") : name->toolTip());
            check(GameRuleNames::displayName(QStringLiteral("keepInventory")) ==
                          QStringLiteral("死亡后保留物品栏") &&
                      // 表里没有的（模组写的、或者比这张表还新的规则）**原样回它自己**：
                      // 屏幕上最坏也只是英文原样，不会变成空白。
                      GameRuleNames::displayName(QStringLiteral("someModRule")) ==
                          QStringLiteral("someModRule"),
                  "★★ Z16a 认不出来的规则名原样回它自己（不许变成空串 —— 那一行就没人认得出"
                  "是哪一条了）",
                  GameRuleNames::displayName(QStringLiteral("someModRule")));
            check(GameRuleNames::count() > 40,
                  "★ Z16a 那张官方中文表真被读进来了（空表的话上面几条会全变成英文原样）",
                  QString::number(GameRuleNames::count()));
        }
        check(page.ruleToggleAt(0) != nullptr && page.ruleToggleAt(0)->isChecked() &&
                  page.ruleEditorAt(0) == nullptr,
              "★ Z16a 值是 true/false 的那一条画的是**开关**（不是让人手打 true）");
        check(page.ruleEditorAt(1) != nullptr &&
                  page.ruleEditorAt(1)->text() == QStringLiteral("3") &&
                  page.ruleToggleAt(1) == nullptr,
              "★ Z16a 值是整数的那一条画的是**输入框**");
        check(page.ruleEditorAt(1)->validator() != nullptr,
              "★ Z16a 整数那一条带着**校验器**（填不进去 'abc'）—— 游戏那边对一个读不成数字的"
              "gamerule 是用默认值，屏幕上一点异常都没有，所以只能在这儿拦住");
        check(page.ruleEditorAt(2) != nullptr &&
                  page.ruleEditorAt(2)->text() == QStringLiteral("花哨") &&
                  page.ruleEditorAt(2)->validator() == nullptr,
              "Z16a 别的那种值是随便填的（不校验）");

        // ---- ：那一列是**全集**，盘上没有的那几条要看得出来 ----
        //
        // 要求「下面的世界设置不要要求进了世界才显示而是把所有的都列出来」。主程序摆
        // 进来的是并过全集的那一列（盘上那几条 + 启动器认识的那些，缺的用游戏默认值），
        // 所以屏幕上会混着两种行 —— 不标出来的话，用户会以为"这个世界本来就是这样"。
        {
            const auto hasDefaultMark = [&page](int row) {
                QWidget *host = page.ruleList()->itemWidget(page.ruleList()->item(row));
                const auto *mark =
                    host == nullptr ? nullptr
                                    : host->findChild<QLabel *>(WorldSettingsPage::ruleDefaultMarkName());
                return mark != nullptr && mark->text() == QStringLiteral("未写入");
            };
            check(!hasDefaultMark(0) && !hasDefaultMark(1) && hasDefaultMark(2),
                  "★★ Z16a 盘上有的那几条**不挂**「未写入」小标记，盘上没有的那一条挂上 —— "
                  "两种行混在一列里，不标出来用户会以为「屏幕上摆的就是这个世界本来的样子」");
            WorldSettingsPage raw;
            raw.setWorld(view);
            // 那一列现在收在折叠框里，所以"这一列是怎么来的"这句小字**只在框
            // 开着的时候说** —— 收着的时候屏幕上没有那个列表，说它等于让用户去找一样看不
            // 见的东西。这两条一句盯半边。
            check(!raw.noteLabel()->text().contains(WorldSettingsPage::rulesSourceHint(true)),
                  "★ Z16a 框收着时底下不说「那一列」是怎么来的（屏幕上根本没有那一列）",
                  raw.noteLabel()->text());
            raw.setFileEditingOpen(true);
            check(raw.noteLabel()->text().contains(WorldSettingsPage::rulesSourceHint(true)),
                  "★ Z16a 展开之后那行小字说清了这一列是怎么来的（哪几条是存档里的、哪几条是"
                  "游戏默认值、改过才会写进去）",
                  raw.noteLabel()->text());
        }

        // ---- ：「修改存档文件」那个折叠框，**默认收起** ----
        //
        // 要求「修改文件的设置放在一个类似版本下载那里的收起来的框里，默认收起，展开是
        // 弹出备份警告」。这一层量的是"收没收"和"点了发不发信号"；**那一句警告在
        // MainWindow 那一层弹**（见 mw_check 的 T27.7），这一页只负责发
        // `fileEditingRequested()`。
        {
            check(page.fileToggle() != nullptr && !page.fileToggle()->isChecked() &&
                      !page.fileBody()->isVisible(),
                  "★★ Z16a 那个折叠框**默认收起**（世界名 / 允许作弊 / 极限模式 / 难度这四格，"
                  "加上那一整列游戏规则，一个都不该一进来就摊在屏幕上）");
            check(page.fileToggle()->toolTip().contains(QStringLiteral("游戏规则")),
                  "★ Z16a 那颗钮的 tooltip 里点到了游戏规则（框里现在装的是那两拨东西）",
                  page.fileToggle()->toolTip());
            check(page.fileToggle()->text() == WorldSettingsPage::fileToggleText(),
                  "Z16a 那颗钮上写的就是「修改存档文件」", page.fileToggle()->text());
            check(page.renameButton()->isVisible() && page.deleteButton()->isVisible() &&
                      page.openFolderButton()->isVisible(),
                  "★ Z16a 「重命名…」「打开文件夹」「删除世界…」三颗都在**上面**（用户原话"
                  "「改文件的设置上方是重命名 打开文件夹 删除世界」）");
            int editingCount = 0;
            bool lastWanted = false;
            QObject::connect(&page, &WorldSettingsPage::fileEditingRequested, &page,
                             [&editingCount, &lastWanted](bool wanted) {
                                 ++editingCount;
                                 lastWanted = wanted;
                             });
            page.fileToggle()->click();
            check(editingCount == 1 && lastWanted,
                  "★★ Z16a 点那颗钮发的是「用户想改文件了」（`fileEditingRequested(true)`）"
                  "—— 那一句备份警告在主程序那一层弹",
                  QStringLiteral("发了 %1 次").arg(editingCount));
            // 这条盯的是 `setFileEditingOpen()` 里那句 `QSignalBlocker`：不挡的话，
            // 主程序答完之后拿这个函数把框摆开，会**顺着 `toggled` 再发一次**信号 ——
            // 用户点一下、警告弹两次，第二次是在他刚答应完之后弹的。
            const auto stateText = [&page] {
                const auto labels = page.fileToggle()->findChildren<QLabel *>();
                return labels.isEmpty() ? QString() : labels.first()->text();
            };
            check(stateText() == QStringLiteral("收起"), "Z16a 展开后折叠行右侧写「收起」", stateText());
            page.setFileEditingOpen(false);
            check(!page.fileToggle()->isChecked() && !page.fileBody()->isVisible(),
                  "★ Z16a `setFileEditingOpen(false)` 把框收回去（用户选了「取消」那一档）");
            check(stateText() == QStringLiteral("展开"),
                  "★★ #74 弹窗里选「取消」把框收回去之后，右侧文字变回「展开」", stateText());
            page.setFileEditingOpen(true);
            check(editingCount == 1,
                  "★★ Z16a `setFileEditingOpen(true)` **不再发一次** `fileEditingRequested()` "
                  "—— 不挡的话主程序答完之后会顺着 `toggled` 再弹一问，而那一下是在用户刚"
                  "答应完之后弹的",
                  QStringLiteral("一共发了 %1 次").arg(editingCount));
            check(page.fileBody()->isVisible() && page.fileToggle()->isChecked(),
                  "Z16a 主程序说「继续修改」之后框真的开着");
            check(page.saveButton()->parentWidget() == page.fileBody() ||
                      page.fileBody()->isAncestorOf(page.saveButton()),
                  "★★ #74 「保存」在「修改存档文件」这一组里（框收着时不露面）");
            check(page.nameEdit()->width() > 340,
                  "★ #74 展开后世界名输入框占满一行，不再被短值上限（340）截短",
                  QString::number(page.nameEdit()->width()));
            page.setFileEditingOpen(false);
            check(!page.fileBody()->isVisible(), "Z16a 收回来之后里头那四格又看不见了");
        }

        // ---- 改了之后收回来的是新的 ----
        page.nameEdit()->setText(QStringLiteral("改过的名字"));
        page.allowCommandsSwitch()->setChecked(true);
        page.difficultyCombo()->setCurrentIndex(
            page.difficultyCombo()->findData(3));
        page.ruleToggleAt(0)->setChecked(false);
        page.ruleEditorAt(1)->setText(QStringLiteral("42"));
        page.ruleEditorAt(2)->setText(QStringLiteral("朴素"));
        check(page.isDirty(), "Z16b 动过就是脏的（那枚返回箭头靠它拦一下）");

        const WorldSettingsPage::WorldView edited = page.editedView();
        check(edited.levelName == QStringLiteral("改过的名字"), "Z16b 改过的世界名收了回来");
        check(edited.allowCommands, "Z16b 改过的作弊开关收了回来");
        check(edited.difficulty == 3, "Z16b 改过的难度收了回来",
              QString::number(edited.difficulty));
        check(edited.rules.size() == 3 && edited.rules[0].value == QStringLiteral("false") &&
                  edited.rules[1].value == QStringLiteral("42") &&
                  edited.rules[2].value == QStringLiteral("朴素"),
              "Z16b 三条改过的规则值都收了回来");

        // ---- 筛掉的那些**不许被顺手清空**（最容易写错的一格）----
        {
            // 屏幕上那一格现在盖的是**官方中文名**，所以用户记着的那个词很
            // 可能也是中文（「物品栏」）。筛的时候**两个名字都得认**：中文那一侧要能搜到，
            // 英文那一侧（`/gamerule` 要打的那个词、也是存档里的键）也不能搜不到。
            page.ruleFilter()->setText(QStringLiteral("物品栏"));
            settleFilter();   // 防抖（见 setWorld 上头那一段 ★）
            check(page.ruleList()->count() == 1 && page.ruleToggleAt(0) != nullptr,
                  "★★ Z16c 拿**中文名**（屏幕上写着的那个词）也筛得出来",
                  QString::number(page.ruleList()->count()));
        }
        page.ruleFilter()->setText(QStringLiteral("inventory"));
        settleFilter();   // 防抖
        check(page.ruleList()->count() == 1,
              "Z16c 筛一下只剩一条", QString::number(page.ruleList()->count()));
        check(page.ruleToggleAt(0) != nullptr,
              "Z16c 筛出来的那一条还是能改的");
        page.ruleToggleAt(0)->setChecked(true);
        const WorldSettingsPage::WorldView filtered = page.editedView();
        check(filtered.rules.size() == 3,
              "★★ Z16c 筛着的时候收回来的是**整整三条**（不是筛出来的那一条）",
              QString::number(filtered.rules.size()));
        check(filtered.rules[1].value == QStringLiteral("42") &&
                  filtered.rules[2].value == QStringLiteral("朴素"),
              "★★ Z16c 被筛掉的那两条**一个字节都没动** —— 用户搜三个字、改一条就存，"
              "不该把另外几十条一起变成「筛掉时读到的空串」");
        check(filtered.rules[0].value == QStringLiteral("true"),
              "Z16c 而筛出来那一条改的就是改过的值");

        page.ruleFilter()->setText(QStringLiteral("没有这条规则"));
        settleFilter();   // 防抖
        check(page.ruleList()->count() == 1 &&
                  page.ruleList()->item(0)->data(WorldSettingsPage::ruleNameRole()).toString().isEmpty() &&
                  page.ruleList()->item(0)->text() == WorldSettingsPage::noMatchText(),
              "★ Z16c 一条都没筛出来时摆**一行不能点的提示**（不是留一片空白 —— 空白看着像"
              "还没读完，而这是读完了就是这个结果）");
        check((page.ruleList()->item(0)->flags() & Qt::ItemIsEnabled) == 0,
              "Z16c 那一行是点不动的");
        page.ruleFilter()->setText(QString());
        settleFilter();   // 防抖
        check(page.ruleList()->count() == 3, "Z16c 清掉搜索框三条都回来了");
        check(page.ruleEditorAt(1) != nullptr &&
                  page.ruleEditorAt(1)->text() == QStringLiteral("42"),
              "★★ Z16c 筛回来之后，刚才改的那条**屏幕上还是改过的值** —— 筛选只是换个看法，"
              "不是「重新读一遍盘」",
              page.ruleEditorAt(1) == nullptr ? QStringLiteral("没有那一行") : page.ruleEditorAt(1)->text());

        // ---- 空值不许存（把一条规则写成一个空串 = 游戏那边当坏值）----
        page.ruleEditorAt(1)->setText(QString());
        check(!page.saveButton()->isEnabled(),
              "★★ Z16d 有一条规则的值被清空了 ⇒ 「保存」点不动（用户多半是手滑，"
              "不是想把那条规则写成一个空串）");
        page.ruleEditorAt(1)->setText(QStringLiteral("7"));
        check(page.saveButton()->isEnabled(), "Z16d 填回去就又点得动了");

        page.ruleEditorAt(1)->setText(QString());
        page.ruleFilter()->setText(QStringLiteral("motd"));
        settleFilter();
        check(!page.saveButton()->isEnabled(),
              "★★ Z16d 清空的那一条被搜索框筛掉之后，「保存」照样点不动（判据走整张规则表）");
        page.ruleFilter()->setText(QString());
        settleFilter();
        // 编辑器那个正则挡得住字母，挡不住**单独一个 `-`**（"还没打完"的中间态）——
        // 存下去游戏读不成数字，静悄悄按默认值来。
        page.ruleEditorAt(1)->setText(QStringLiteral("-"));
        check(!page.saveButton()->isEnabled(),
              "★★ Z16d 只填了一个 `-` 也算没填完 ⇒ 「保存」点不动");
        page.ruleEditorAt(1)->setText(QStringLiteral("7"));
        check(page.saveButton()->isEnabled(), "Z16d 改回一个整数又点得动了");

        // ---- 存下去之后：基线往前挪，值还是页面上这一份 ----
        page.markSaved();
        check(!page.isDirty(),
              "★★ Z16d markSaved() 之后不脏了 —— 基线跟着挪到刚写下去那一份。不挪的话，"
              "用户存完再按返回箭头还会被问一句「改了没保存」，而它明明刚存过");
        check(page.editedView().rules[1].value == QStringLiteral("7"),
              "Z16d 而且页面上的值还是刚存下去那一份（基线挪了，值没被退回去）");
        check(page.noteLabel()->text().contains(QStringLiteral("已保存")),
              "Z16d 底下那行字改口成「已保存」", page.noteLabel()->text());

        // ---- 备份那一句：有备份时说明改口 ----
        page.setBackupAvailable(true);
        check(page.noteLabel()->text().contains(QStringLiteral("备份")),
              "Z16d 已经留过备份时底下那行字说得出这件事", page.noteLabel()->text());

        // ---- 不是 gzip 的那份：底下多一句（不替用户改形状）----
        {
            WorldSettingsPage raw;
            raw.show();
            WorldSettingsPage::WorldView unwrapped = view;
            unwrapped.gzipped = false;
            unwrapped.rules.clear();
            raw.setWorld(unwrapped);
            check(raw.noteLabel()->text().contains(WorldSettingsPage::notGzippedText()),
                  "★ Z16e 盘上那份不是 gzip 包着的时候，底下多一句实话（我们按原样写回去，"
                  "不替你改形状）");
            check(raw.ruleList()->count() == 1 &&
                      raw.ruleList()->item(0)->text() == WorldSettingsPage::emptyRulesText(),
                  "★ Z16e 一条规则都没有 ⇒ 摆一句实话那一行（**产品里走不到这一档** —— "
                  "主程序摆进来的是并过全集的那一列，好几十条；留着是给「只摆了个空 "
                  "WorldView」那种场面一句话，而不是一片看着像「还没读完」的空白）");
            // 这一句是"这一列是怎么来的"的说明书。
            // `unwrapped` 是 `view` 拷来的，`hasRules` 还是 true ⇒ 走"有那一格"那一档。
            // 那句话只在那个折叠框**开着**的时候说（上面 Z16a 那两条盯的就是这件事）。
            raw.setFileEditingOpen(true);
            check(raw.noteLabel()->text().contains(WorldSettingsPage::rulesSourceHint(true)),
                  "★ Z16e 底下那行小字说的是「这一列是启动器认识的全部规则 + 盘上的值，"
                  "改过哪一条才写哪一条」—— 用户得知道为什么列表里有他没见过的规则",
                  raw.noteLabel()->text());
            raw.hide();
        }

        // ---- 读不出来那一档：整块正文藏起来，只留能做事的三颗 ----
        {
            WorldSettingsPage broken;
            broken.show();
            broken.setWorld(view);
            broken.setUnreadable(QStringLiteral("坏档"), QStringLiteral("level.dat 里的内容读不懂"));
            check(broken.isUnreadable(), "Z16f 认得出自己在「读不出来」那一档");
            check(broken.findChild<QWidget *>(WorldSettingsPage::bodyName()) != nullptr &&
                      !broken.findChild<QWidget *>(WorldSettingsPage::bodyName())->isVisible(),
                  "★★ Z16f 读不出来时**整块正文藏起来**（它摆的那些值一个都不作数）");
            check(!broken.saveButton()->isVisible(),
                  "★★ Z16f 「保存」整个不画 —— 读不出存档内容就不该往上写任何东西");
            check(broken.fullBackupButton()->isVisible() && broken.openFolderButton()->isVisible(),
                  "★★ Z16f 但「立即备份」「打开文件夹」都还在 —— 这一档最该做的就是"
                  "这两件事，不能是个死胡同");
            check(broken.renameButton()->isVisible() && broken.deleteButton()->isVisible(),
                  "★★ Z16f 「重命名…」「删除世界…」也还在 —— 读不出存档内容，但这个世界还在"
                  "盘上，该能改名、该能删");
            check(!broken.fileToggle()->isVisible() && !broken.fileToggle()->isChecked(),
                  "★★ Z16f 「修改存档文件」那颗折叠钮跟着正文一起藏起来（读不出内容，"
                  "改文件这件事无从谈起）");
            check(broken.noteLabel()->text().contains(QStringLiteral("读不懂")),
                  "Z16f 底下那行字说的就是**为什么**读不出来", broken.noteLabel()->text());
            check(!broken.isDirty(),
                  "★★ Z16f 这一档**永远不算改过** —— 一个格子都动不了，按返回箭头不该被拦");
            broken.hide();
        }

        // ---- 四条信号 + 覆盖层 Esc 统一走页眉返回 ----
        int saveCount = 0;
        int openCount = 0;
        int backCount = 0;
        int renameCount = 0;
        int deleteCount = 0;
        QObject::connect(&page, &WorldSettingsPage::saveRequested, &page,
                         [&saveCount] { ++saveCount; });
        QObject::connect(&page, &WorldSettingsPage::openFolderRequested, &page,
                         [&openCount] { ++openCount; });
        QObject::connect(&page, &WorldSettingsPage::renameRequested, &page,
                         [&renameCount] { ++renameCount; });
        QObject::connect(&page, &WorldSettingsPage::deleteRequested, &page,
                         [&deleteCount] { ++deleteCount; });
        page.saveButton()->click();
        page.openFolderButton()->click();
        page.renameButton()->click();
        page.deleteButton()->click();
        check(saveCount == 1 && openCount == 1 && renameCount == 1 && deleteCount == 1,
              "Z16g 四颗按钮各发各的信号（这一层不自己写盘、也不自己切格子 / 不自己弹框；"
              "页内没有「返回」，返回在覆盖层页眉的面包屑上）",
              QStringLiteral("%1/%2/%3/%4")
                  .arg(saveCount)
                  .arg(openCount)
                  .arg(renameCount)
                  .arg(deleteCount));
        check(page.renameButton()->text() == WorldSettingsPage::renameText() &&
                  page.deleteButton()->text() == WorldSettingsPage::deleteText(),
              "Z16g 那两颗的字就是「重命名…」「删除世界…」", page.deleteButton()->text());

        // Esc 统一由 OverlayHost 接管，按 Esc 等同于点页眉「返回」（先走 setBackOverride，
        // 不直接关掉外层 QDialog）。
        {
            QWidget shell;
            shell.resize(700, 500);
            auto *anchor = new QWidget(&shell);
            anchor->setGeometry(0, 0, 700, 500);
            OverlayHost host(anchor, &shell);
            QDialog outerDialog;
            auto *outerLayout = new QVBoxLayout(&outerDialog);
            page.setParent(&outerDialog);
            outerLayout->addWidget(&page);
            shell.show();
            host.showPage(&outerDialog, QStringLiteral("世界设置：测试"));
            host.setBackOverride([&backCount] {
                ++backCount;
                return true;
            });
            QApplication::processEvents();
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(page.nameEdit(), &esc);
            QApplication::processEvents();
            check(backCount == 1 && esc.isAccepted() && host.pageOpen() && outerDialog.isVisible(),
                  "★★ Z16g 世界设置页里按 Esc = 点页眉返回（走 setBackOverride，不关掉外层对话框）",
                  QStringLiteral("收到 %1 次 / pageOpen=%2").arg(backCount).arg(host.pageOpen()));
            host.closeOverlay();
            QApplication::processEvents();
            page.setParent(nullptr);
        }

        // ---- ：`deltaFromOpened()` —— "用户到底动了哪几格" ----
        //
        // 这一段盯的是**产品纪律**：那四格现在一律可动（盘上没有的项在屏幕上摆的是游戏
        // 默认值），而"用户没碰的那几格一个字节都不许写进存档"这条纪律全靠这一份差集兜着。
        // 漏出去一位的症状是：用户只想改难度，主程序顺手把世界名 / 作弊 / 极限模式也按
        // 屏幕上的值写了一遍 —— 屏幕上一点异常都没有。
        {
            WorldSettingsPage fresh;
            fresh.setWorld(view);
            const WorldSettingsPage::Delta untouched = fresh.deltaFromOpened();
            check(!untouched.nameChanged && !untouched.allowCommandsChanged &&
                      !untouched.hardcoreChanged && !untouched.difficultyChanged &&
                      untouched.rules.isEmpty(),
                  "★★ Z16h 刚摆进去**一位都不算改过**（一进来就交出一堆「要写」的话，"
                  "主程序会顺手把几十个默认值塞进用户的存档）");

            fresh.nameEdit()->setText(QStringLiteral("另一个名字"));
            fresh.allowCommandsSwitch()->setChecked(true);
            const WorldSettingsPage::Delta changed = fresh.deltaFromOpened();
            check(changed.nameChanged && changed.nameValue == QStringLiteral("另一个名字"),
                  "★ Z16h 改过的世界名在差集里（带着新值）", changed.nameValue);
            check(changed.allowCommandsChanged && changed.allowCommandsValue,
                  "★ Z16h 拨过的作弊开关在差集里");
            check(!changed.hardcoreChanged && !changed.difficultyChanged,
                  "★★ Z16h **没碰的那两格不在差集里** —— 它们在屏幕上也有值，但那不是"
                  "「用户要改」");
            check(changed.rules.isEmpty(),
                  "★★ Z16h 一条规则都没动 ⇒ 差集里一条都没有（盘上没有的那条 `motdStyle` "
                  "屏幕上摆着游戏默认值，用户没碰它就**不算**要写）",
                  QStringLiteral("差集里 %1 条").arg(changed.rules.size()));

            fresh.ruleEditorAt(2)->setText(QStringLiteral("更花哨"));
            const WorldSettingsPage::Delta withRule = fresh.deltaFromOpened();
            check(withRule.rules.size() == 1 &&
                      withRule.rules[0].first == QStringLiteral("motdStyle") &&
                      withRule.rules[0].second == QStringLiteral("更花哨"),
                  "★ Z16h 改过的那一条规则在差集里（名字 + 新值）",
                  QStringLiteral("差集里 %1 条").arg(withRule.rules.size()));

            // 筛一下再收——差集**不看筛选框**（它比的是值和值，不是"这一行现在露没露面"）。
            fresh.ruleFilter()->setText(QStringLiteral("motd"));
            settleFilter();   // 防抖：**先让它真筛下去**，再量差集 —— 不然量的是"筛还没生效"
            const WorldSettingsPage::Delta filtered = fresh.deltaFromOpened();
            check(filtered.rules.size() == 1,
                  "★★ Z16h 筛着的时候差集照样把改过的那一条交出来（差集比的是值，"
                  "不是「这一行现在露没露面」）",
                  QStringLiteral("差集里 %1 条").arg(filtered.rules.size()));
            fresh.ruleFilter()->clear();
            settleFilter();   // 让他悬着的那一发落完，别漏到下一段去

            fresh.markSaved();
            const WorldSettingsPage::Delta afterSave = fresh.deltaFromOpened();
            check(!afterSave.nameChanged && !afterSave.allowCommandsChanged &&
                      !afterSave.hardcoreChanged && !afterSave.difficultyChanged &&
                      afterSave.rules.isEmpty(),
                  "★★ Z16h 存过之后差集**清空** —— 刚写下去的就是页面上这一份（不清的话，"
                  "用户存完接着改一条别的规则、再存一次，会把刚存过的那几格又写一遍，"
                  "白吃掉一格备份代数）");
        }

        {
            WorldSettingsPage spaced;
            WorldSettingsPage::WorldView spacedView = view;
            spacedView.rules[1].value = QStringLiteral("42 ");
            spaced.setWorld(spacedView);
            check(!spaced.isDirty(), "★★ Z16h' 盘上那一格带个尾空格，一打开不算改过");
            check(spaced.editedView().rules.value(1).value == QStringLiteral("42 "),
                  "★ Z16h' 原样交出去（不替用户 trim）",
                  spaced.editedView().rules.value(1).value);
            spaced.hide();
        }

        page.hide();
    }

    // ---- #74：全局滚轮规矩（没有焦点的下拉框、数字框不认滚轮，滚轮交给外层滚动区）----
    {
        WheelFocusFilter::install(*qApp);
        QWidget host;
        auto *area = new QScrollArea(&host);
        area->setWidgetResizable(true);
        auto *content = new QWidget(area);
        content->setMinimumHeight(1200);
        // 没有逐个装 WheelGuard：全靠应用级过滤器。
        auto *combo = new QComboBox(content);
        combo->addItems({QStringLiteral("和平"), QStringLiteral("简单"), QStringLiteral("普通")});
        combo->setGeometry(10, 10, 140, 32);
        auto *spin = new QSpinBox(content);
        spin->setRange(0, 99999);
        spin->setValue(123);
        spin->setGeometry(10, 60, 140, 32);
        auto *other = new QLineEdit(content);
        other->setGeometry(10, 110, 140, 32);
        area->setWidget(content);
        area->resize(240, 160);
        host.resize(240, 160);
        host.show();
        host.activateWindow();
        QApplication::processEvents();
        other->setFocus();
        QApplication::processEvents();
        check((combo->focusPolicy() & Qt::WheelFocus) != Qt::WheelFocus &&
                  (spin->focusPolicy() & Qt::WheelFocus) != Qt::WheelFocus,
              "★★ #74 下拉框、数字框的焦点策略里没有 WheelFocus（滚轮抢不到焦点）");
        const auto wheel = [](QWidget *target) {
            QWheelEvent event(QPointF(5, 5), target->mapToGlobal(QPointF(5, 5)), QPoint(0, 0),
                              QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                              false);
            QApplication::sendEvent(target, &event);
        };
        QScrollBar *bar = area->verticalScrollBar();
        const int before = bar->value();
        wheel(combo);
        wheel(spin);
        QApplication::processEvents();
        check(combo->currentIndex() == 0 && spin->value() == 123 && bar->value() > before,
              "★★ #74 没有焦点时滚轮不改下拉框、数字框的值，滚轮转给外层滚动区",
              QStringLiteral("下拉框 %1 / 数字框 %2 / 滚动 %3→%4")
                  .arg(combo->currentIndex()).arg(spin->value()).arg(before).arg(bar->value()));
        combo->setFocus();
        QApplication::processEvents();
        wheel(combo);
        check(combo->hasFocus() && combo->currentIndex() != 0,
              "#74 有焦点时滚轮照常改值", QString::number(combo->currentIndex()));
    }
    return 0;
}



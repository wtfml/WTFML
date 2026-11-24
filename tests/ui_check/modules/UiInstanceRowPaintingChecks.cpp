#include "Support.hpp"
int UiInstanceRowPaintingChecks() {
    {
        // ---- a) 版本行左边那枚字母方块 ----
        //
        // 用户点过这条：「像素断言要量"颜色由我们自己决定"的那一格」。这一枚整块（底色 +
        // 那个字母）都是我们定的，所以量得到 —— 而它又是这一页跟下载页那四张表唯一不像
        // 的一处（那边画缩略图、这边画一枚标记），所以值得单独钉住。
        const int badge = ResourceDetailMarks::badgeSize();
        check(badge == MainWindowUI::resourceTreeIconSize(),
              "Z14 这枚方块的边长就是下载页那四张表的缩略图那一格（并列看过去一样大）",
              QStringLiteral("%1 px").arg(badge));

        const QColor release = ResourceDetailMarks::channelColor(0);
        const QColor beta = ResourceDetailMarks::channelColor(1);
        const QColor alpha = ResourceDetailMarks::channelColor(2);
        const QColor unknown = ResourceDetailMarks::channelColor(-1);
        check(release != beta && beta != alpha && release != alpha,
              "Z14 三档底色互不相同",
              QStringLiteral("%1 / %2 / %3").arg(release.name(), beta.name(), alpha.name()));
        check(release != unknown && beta != unknown && alpha != unknown,
              "Z14 三档都不是「认不出来」那一档的灰（不然三档和认不出在屏幕上一个样）");
        check(ResourceDetailMarks::channelColor(-1) == ResourceDetailMarks::channelColor(99),
              "Z14 认不出来的档一律灰 —— 不是「不画」：同组别的行前面都有方块，"
              "这一行没有的话它的正文会往左挪一截");
        check(ResourceDetailMarks::channelLetter(0) == QStringLiteral("R") &&
                  ResourceDetailMarks::channelLetter(1) == QStringLiteral("B") &&
                  ResourceDetailMarks::channelLetter(2) == QStringLiteral("A"),
              "Z14 三档的字母是 R / B / A（和 release / beta / alpha 那三个词的次序对齐）");
        check(ResourceDetailMarks::channelLetter(-1).isEmpty() &&
                  ResourceDetailMarks::channelLetter(99).isEmpty(),
              "Z14 认不出来的档不写字（只画那块灰底）");
        check(ResourceDetailMarks::channelLetterColor() == QColor(0xff, 0xff, 0xff) &&
                  ResourceDetailMarks::channelLetterColor().alpha() == 255,
              "Z14 字母是白的不透明（三档底色上都读得出来）");
        // 那一格画出来：底色在、字母在、圆角真的圆、字母不贴边。
        {
            const QPixmap pixmap = ResourceDetailMarks::channelBadge(0, badge);
            check(!pixmap.isNull() && pixmap.size() == QSize(badge, badge),
                  "Z14 channelBadge() 给出的是 badge×badge 的图",
                  QStringLiteral("%1x%2").arg(pixmap.width()).arg(pixmap.height()));
            const QImage image = pixmap.toImage();
            const QRgb letterInk = ResourceDetailMarks::channelLetterColor().rgb();
            int fillPixels = 0;
            int letterPixels = 0;
            QRect letterBox;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QColor color = image.pixelColor(x, y);
                    if (color.alpha() == 0) {
                        continue;
                    }
                    if (color.rgb() == release.rgb()) {
                        ++fillPixels;
                    }
                    if (color.rgb() == letterInk) {
                        ++letterPixels;
                        const QRect px(x, y, 1, 1);
                        letterBox = letterBox.isNull() ? px : letterBox.united(px);
                    }
                }
            }
            check(fillPixels > 0 && letterPixels > 0,
                  "★ Z14 画出来的方块：底色铺开了、那个字母也画上了",
                  QStringLiteral("底色 %1 px / 字母 %2 px").arg(fillPixels).arg(letterPixels));
            check(image.pixelColor(0, 0).alpha() == 0 &&
                      image.pixelColor(0, badge / 2).alpha() > 0,
                  "Z14 四个角是**圆**的（角上那一格不画，边中间照画）");
            check(!letterBox.isNull() && QRect(2, 2, badge - 4, badge - 4).contains(letterBox),
                  "Z14 字母不贴边（四周留得下一点）",
                  QStringLiteral("%1,%2-%3,%4")
                      .arg(letterBox.left())
                      .arg(letterBox.top())
                      .arg(letterBox.right())
                      .arg(letterBox.bottom()));
            // 认不出来的那一档：只有灰底、一个白像素都没有。
            const QImage grey = ResourceDetailMarks::channelBadge(-1, badge).toImage();
            int greyFill = 0;
            int greyLetter = 0;
            for (int y = 0; y < grey.height(); ++y) {
                for (int x = 0; x < grey.width(); ++x) {
                    const QColor color = grey.pixelColor(x, y);
                    if (color.alpha() == 0) {
                        continue;
                    }
                    if (color.rgb() == unknown.rgb()) {
                        ++greyFill;
                    }
                    if (color.rgb() == letterInk) {
                        ++greyLetter;
                    }
                }
            }
            check(greyFill > 0 && greyLetter == 0,
                  "★ Z14 认不出来的档：灰底在、字母一个像素都不画",
                  QStringLiteral("灰底 %1 px / 白字 %2 px").arg(greyFill).arg(greyLetter));
            check(ResourceDetailMarks::channelBadge(0, 0).isNull(),
                  "Z14 边长给 0 时回空图（不是画一张 0×0 的、更不是崩）");
        }

        // ---- b) 头卡：那三行字 + 那三颗按钮 ----
        ResourceDetailPage page;
        QString longDescription;
        while (longDescription.size() < 320) {
            longDescription += QStringLiteral("钠是一个为现代渲染管线重写的渲染优化模组。");
        }
        ResourceDetail::Header header;
        header.title = QStringLiteral("Sodium");
        header.description = longDescription;
        header.metaLine = QStringLiteral("Fabric · 下载 2.3 亿次 · 更新于 5 天前 · Modrinth");
        header.openLabel = QStringLiteral("Modrinth");
        header.openUrl = QStringLiteral("https://modrinth.com/mod/sodium");
        header.mcmodUrl = QStringLiteral("https://www.mcmod.cn/s?key=sodium");
        header.nameToCopy = QStringLiteral("Sodium");
        page.setHeader(header);
        // 窄一点（360）：这一页的量要的是"放不下的时候怎么画"。窄到比控件自己的最小宽度还窄
        // 也**没关系** —— Qt 会钳到最小宽度，而下面那两条断言对这两种宽度都成立。
        page.resize(360, 560);
        page.show();
        QApplication::processEvents();

        check(page.titleText() == header.title, "Z14 页眉那行字就是资源名", page.titleText());
        check(page.openButtonText() == QStringLiteral("转到 Modrinth"),
              "Z14 「转到 XXX」那颗按钮上的字是按来源拼的（不是写死 Modrinth）",
              page.openButtonText());
        auto *openButton = page.findChild<QPushButton *>(ResourceDetailPage::openButtonName());
        auto *mcmodButton = page.findChild<QPushButton *>(ResourceDetailPage::mcmodButtonName());
        auto *copyButton = page.findChild<QPushButton *>(ResourceDetailPage::copyNameButtonName());
        check(openButton != nullptr && mcmodButton != nullptr && copyButton != nullptr,
              "Z14 头卡上那三颗按钮都在（按 objectName 找得到）");
        check(mcmodButton != nullptr && mcmodButton->text() == QStringLiteral("在 MC 百科搜索"),
              "Z14 MC 百科那一颗写的是「在 MC 百科搜索」", mcmodButton ? mcmodButton->text() : QString());
        check(openButton != nullptr && openButton->isVisible() && mcmodButton != nullptr &&
                  mcmodButton->isVisible() && copyButton != nullptr && copyButton->isVisible(),
              "Z14 地址拼得出、名字拿得到 ⇒ 三颗都画出来");
        auto *metaLabel = page.findChild<QLabel *>(ResourceDetailPage::metaLabelName());
        check(metaLabel != nullptr && metaLabel->text() == header.metaLine,
              "Z14 元信息那一行就是 MainWindow 拼好的整串（这一页一个字都不拼）",
              metaLabel != nullptr ? metaLabel->text() : QString());
        auto *titleLabel = page.findChild<QLabel *>(ResourceDetailPage::titleLabelName());
        check(metaLabel != nullptr && titleLabel != nullptr &&
                  metaLabel->palette().color(QPalette::WindowText) !=
                      titleLabel->palette().color(QPalette::WindowText),
              "Z14 元信息那一行是次要那一档颜色（不是正文那个黑）");
        check(page.shownDescription().size() < header.description.size() &&
                  page.shownDescription().endsWith(QString(QChar(0x2026))),
              "★★ Z14 简介**只画一行**、放不下从尾巴截成省略号（不是硬裁成半个字）",
              page.shownDescription());
        check(page.descriptionToolTip() == header.description,
              "Z14 截掉的那半句在鼠标提示里有全文（不然用户永远看不到它）");

        // 头卡那一块反过来：拼不出地址 / 没名字可复制的那几颗**一颗都不画**。
        ResourceDetail::Header bare;
        bare.title = QStringLiteral("某某");
        page.setHeader(bare);
        check(openButton != nullptr && !openButton->isVisible() && mcmodButton != nullptr &&
                  !mcmodButton->isVisible() && copyButton != nullptr && !copyButton->isVisible(),
              "★★ Z14 一颗都拼不出时那三颗都不画（点了没反应的按钮比没有这颗更糟）");
        check(page.titleText() == bare.title, "Z14 换了资源之后页眉那行字跟着换");
        {
            ResourceDetailPage fresh;
            check(fresh.titleText() == QStringLiteral("资源详情"),
                  "Z14 还没摆过资源时页眉是「资源详情」（不是空串）");
        }

        // 那两颗「转到」+「复制名称」都**只发信号**：开浏览器 / 写剪贴板是 MainWindow 的事
        // （开浏览器那条路还要能被验证工程拦住）。
        QStringList opened;
        QStringList copied;
        QObject::connect(&page, &ResourceDetailPage::openExternalRequested,
                         [&opened](const QString &url) { opened.append(url); });
        QObject::connect(&page, &ResourceDetailPage::copyNameRequested,
                         [&copied](const QString &text) { copied.append(text); });
        page.setHeader(header);
        openButton->click();
        mcmodButton->click();
        copyButton->click();
        check(opened.size() == 2 && opened.at(0) == header.openUrl &&
                  opened.at(1) == header.mcmodUrl,
              "★ Z14 两颗「转到」各自发**自己那条**地址（顺序对得上：左边那颗先发）",
              opened.join(QStringLiteral(" / ")));
        check(copied.size() == 1 && copied.first() == header.nameToCopy,
              "Z14 「复制名称」发的是名字原样（这一页不碰剪贴板 —— 那是 MainWindow 的事）",
              copied.join(QStringLiteral(" / ")));

        // ---- c) 那一排游戏版本 chip ----
        QList<ResourceDetail::Chip> chips;
        chips.append({QString(), QStringLiteral("全部")});
        chips.append({QStringLiteral("1.21.1"), QStringLiteral("1.21.1")});
        chips.append({QStringLiteral("1.21"), QStringLiteral("1.21")});
        page.setChips(chips, QString());
        check(page.chips().size() == 3, "Z14 chip 摆出来的颗数就是给的那几颗",
              QStringLiteral("%1 颗").arg(page.chips().size()));
        check(page.chipTextAt(0) == QStringLiteral("全部") && page.chipKeyAt(0).isEmpty() &&
                  page.chipKeyAt(1) == QStringLiteral("1.21.1") &&
                  page.chipKeyAt(2) == QStringLiteral("1.21"),
              "Z14 三颗的文字与 key 一一对上（「全部」的 key 是**空串**）",
              QStringLiteral("%1 / %2 / %3")
                  .arg(page.chipTextAt(0), page.chipTextAt(1), page.chipTextAt(2)));
        check(page.chipTextAt(99).isEmpty() && page.chipKeyAt(-1).isEmpty(),
              "Z14 越界问 chip 回空串（不崩、也不编一个）");
        check(page.chips().at(0)->isChecked() && !page.chips().at(1)->isChecked() &&
                  !page.chips().at(2)->isChecked(),
              "Z14 一开始按着的是调用方指定的那一颗（这儿是「全部」）");
        check(page.currentChip().isEmpty(), "Z14 currentChip() 一开始也说「全部」");

        QStringList chipFired;
        QObject::connect(&page, &ResourceDetailPage::chipChanged,
                         [&chipFired](const QString &key) { chipFired.append(key); });
        page.chips().at(1)->click();
        check(chipFired.size() == 1 && chipFired.first() == QStringLiteral("1.21.1"),
              "★ Z14 点一颗 chip 发的是它的 **key**（不是按钮上那行字）",
              chipFired.join(QStringLiteral(" / ")));
        check(page.currentChip() == QStringLiteral("1.21.1") && page.chips().at(1)->isChecked() &&
                  !page.chips().at(0)->isChecked(),
              "★ Z14 一排 chip 是**互斥**的：按下一颗，上一颗自己弹起来");
        page.chips().at(1)->click();
        check(chipFired.size() == 1 && page.chips().at(1)->isChecked(),
              "★ Z14 点已经按着的那一颗：不再发一次 chipChanged（重算分组会让下面整份列表"
              "闪一下），但它还按着（Qt 那一下会把它翻成没按，得摆回去）",
              QStringLiteral("又发了 %1 次").arg(chipFired.size() - 1));
        page.chips().at(0)->click();
        check(chipFired.size() == 2 && page.currentChip().isEmpty() &&
                  page.chips().at(0)->isChecked(),
              "Z14 点「全部」= 发一个**空 key**（那一路就是「不筛」）");
        // 调用方给了一个表里没有的 key（比如资源刚换、chip 还没重摆）：按「全部」那一颗，
        // 而且 currentChip() 也得说「全部」—— 不然界面按着一颗、读到的却是另一颗，
        // 下游会按一个屏幕上没按着的版本去筛（画面上就是"列表少了几版"）。
        page.setChips(chips, QStringLiteral("1.20.1"));
        check(page.chips().at(0)->isChecked() && page.currentChip().isEmpty(),
              "★★ Z14 给了一个表里没有的 key：按「全部」，currentChip() 也说「全部」"
              "（不是那个界面上没人按着的 key）",
              QStringLiteral("currentChip = 「%1」").arg(page.currentChip()));

        // ---- c1b) 顶排是大版本组，下面的文件分组默认收起（issue #20）----
        //
        // 顶排只有「全部」和各个大版本（key 就是大版本号，「其他」放最后）；点一颗 = 按这个
        // 大版本筛（筛的是业务层的事，这一页只发 key）；展开、收起下面的文件分组不动选中的那一颗。
        {
            QList<ResourceDetail::Chip> series;
            series.append({QString(), QStringLiteral("全部")});
            series.append({QStringLiteral("1.21"), QStringLiteral("1.21")});
            series.append({QStringLiteral("1.20"), QStringLiteral("1.20")});
            series.append({QStringLiteral("other"), QStringLiteral("其他")});
            page.setChips(series, QString());
            QApplication::processEvents();
            check(page.chips().size() == 4 && page.chipKeyAt(1) == QStringLiteral("1.21") &&
                      page.chipTextAt(3) == QStringLiteral("其他") &&
                      page.chipKeyAt(3) == QStringLiteral("other") && page.currentChip().isEmpty(),
                  "★★ Z14c 顶排只有「全部」+ 大版本组（1.21 / 1.20）+ 最后的「其他」，没有细版本",
                  QString::number(page.chips().size()));
            const int firedBefore = chipFired.size();
            page.chips().at(1)->click();
            check(chipFired.size() == firedBefore + 1 && chipFired.last() == QStringLiteral("1.21") &&
                      page.currentChip() == QStringLiteral("1.21") && page.chips().at(1)->isChecked() &&
                      !page.chips().at(0)->isChecked(),
                  "★★ Z14c 点 1.21 = 发 key=1.21，选中的组高亮（互斥）",
                  chipFired.join(QLatin1Char(',')));

            // 下面的文件分组：默认收起，展开 / 收起都不改顶排。
            QList<ResourceDetail::Group> fileGroups;
            for (const QString &title : {QStringLiteral("Fabric 1.21.4"), QStringLiteral("Fabric 1.21.1"),
                                         QStringLiteral("NeoForge 1.21.1")}) {
                ResourceDetail::Group group;
                group.title = title;
                ResourceDetail::Row row;
                row.key = title;
                row.title = QStringLiteral("1.0");
                group.rows.append(row);
                fileGroups.append(group);
            }
            page.setGroups(fileGroups);
            check(page.groupCount() == 3 && !page.groupExpandedAt(0) && !page.groupExpandedAt(1) &&
                      !page.groupExpandedAt(2),
                  "★★ Z14c 文件分组默认全部收起");
            page.versionList()->topLevelItem(1)->setExpanded(true);
            check(page.groupExpandedAt(1) && !page.groupExpandedAt(0) &&
                      page.currentChip() == QStringLiteral("1.21") && page.chips().at(1)->isChecked() &&
                      chipFired.size() == firedBefore + 1,
                  "★★ Z14c 展开一个文件分组：顶排选中的 1.21 不变、不发 chipChanged");
            page.versionList()->topLevelItem(1)->setExpanded(false);
            check(!page.groupExpandedAt(1) && page.currentChip() == QStringLiteral("1.21") &&
                      page.chips().at(1)->isChecked() && chipFired.size() == firedBefore + 1,
                  "★★ Z14c 再收起：筛选条件仍不变");

            // 预选落在某个大版本上：按下去的是那一颗，不发信号。
            page.setChips(series, QStringLiteral("1.20"));
            check(page.currentChip() == QStringLiteral("1.20") && page.chips().at(2)->isChecked() &&
                      chipFired.size() == firedBefore + 1,
                  "Z14c 预选 1.20：那一颗按下，开页那一下不发信号");

            // 窄页面 + 很多大版本：折行，不出横向滚动条，每一颗都完整落在页面里。
            QList<ResourceDetail::Chip> many;
            many.append({QString(), QStringLiteral("全部")});
            for (int minor = 21; minor >= 1; --minor) {
                const QString key = QStringLiteral("1.%1").arg(minor);
                many.append({key, key});
            }
            page.setChips(many, QString());
            QApplication::processEvents();
            auto *chipRow = page.findChild<QWidget *>(ResourceDetailPage::chipRowName());
            bool allInside = chipRow != nullptr;
            int maxBottom = 0;
            for (QPushButton *button : page.chips()) {
                allInside = allInside && chipRow != nullptr &&
                            button->geometry().right() <= chipRow->width() &&
                            button->geometry().left() >= 0;
                maxBottom = qMax(maxBottom, button->geometry().bottom());
            }
            check(chipRow != nullptr && chipRow->findChildren<QAbstractScrollArea *>().isEmpty() &&
                      allInside && maxBottom > page.chips().first()->geometry().bottom(),
                  "★★ Z14c 22 颗 chip 在 360 宽的页面里折成多行：每颗都完整落在页内、chip 区里没有滚动区",
                  QStringLiteral("顶排宽 %1 / 最低一颗下沿 %2")
                      .arg(chipRow != nullptr ? chipRow->width() : -1)
                      .arg(maxBottom));
            // 窗口拉宽：同一份表排得下更多，仍然没有滚动区，也不再折行。
            page.resize(1400, 560);
            QApplication::processEvents();
            int wideMaxBottom = 0;
            for (QPushButton *button : page.chips()) {
                wideMaxBottom = qMax(wideMaxBottom, button->geometry().bottom());
            }
            check(chipRow != nullptr && chipRow->findChildren<QAbstractScrollArea *>().isEmpty() &&
                      chipRow->width() > 900 && wideMaxBottom < maxBottom,
                  "Z14c 拉宽到 1400 后顶排跟着变宽、行数变少，仍没有滚动区",
                  QString::number(chipRow != nullptr ? chipRow->width() : -1));
            page.resize(360, 560);
            QApplication::processEvents();
            // 还回原样，后面几节照旧用三颗那一份。
            page.setChips(chips, QString());
            page.setGroups({});
        }

        // ---- c2) chip 那一排右边那颗「显示快照版」勾选框 ----
        //
        // 要求：「就是点开具体的页面（上次的那张图的那个页面里，上方右侧放一个勾选框，
        // 默认不勾选，按 alt 键可以切换其显示（鼠标悬停里提到这点，记得顺便刷新）」。
        //
        // 这一节钉的是**这一页自己做得了的那一半**：勾在哪、默认哪一档、提示里写着什么键、
        // 拨它发什么、裸 Alt 那一发算不算、以及"摆初始值"那一下许不许发信号。另一半（拨了
        // 之后 chip 重摆、列表重算）是 MainWindow 的事，在 mw_check 的 ZJ 那一节里端到端钉。
        auto *snapBox = page.findChild<QCheckBox *>(ResourceDetailPage::showSnapshotsCheckBoxName());
        check(snapBox != nullptr, "★ Z14 「显示快照版」那颗勾选框按 objectName 找得到");
        check(snapBox != nullptr && snapBox->text() == QStringLiteral("显示快照版"),
              "Z14 那颗勾上写着「显示快照版」",
              snapBox != nullptr ? snapBox->text() : QString());
        check(snapBox != nullptr && !snapBox->isChecked() && !page.showSnapshots(),
              "★ Z14 默认**不勾**（用户原话「默认不勾选」—— 不勾看的是归好类的那份版本表）");
        check(snapBox != nullptr && snapBox->toolTip().contains(QStringLiteral("Alt")),
              "★ Z14 鼠标提示里点明「按 Alt 键也能切换」（用户原话「鼠标悬停里提到这点」）",
              snapBox != nullptr ? snapBox->toolTip() : QString());

        // 位置：在那一排 chip 的**右边**，而且在 chip 区**外面**。放里面的话 chip 折行时
        // 这颗勾会跟着被挤走 —— 而它是"默认不勾"的那一颗，用户得找得到。
        {
            auto *chipArea = page.findChild<QWidget *>(ResourceDetailPage::chipRowName());
            check(chipArea != nullptr && snapBox != nullptr && !chipArea->isAncestorOf(snapBox) &&
                      snapBox->mapTo(&page, QPoint()).x() >=
                          chipArea->mapTo(&page, QPoint()).x() + chipArea->width(),
                  "★★ Z14 那颗勾选框在那一排 chip 的**右边**、并且不在 chip 区里",
                  chipArea == nullptr
                      ? QStringLiteral("没找到 chip 区")
                      : QStringLiteral("chip 区右边缘 %1 / 勾左边 %2")
                            .arg(chipArea->mapTo(&page, QPoint()).x() + chipArea->width())
                            .arg(snapBox->mapTo(&page, QPoint()).x()));
        }

        QList<bool> snapFired;
        QObject::connect(&page, &ResourceDetailPage::showSnapshotsChanged,
                         [&snapFired](bool show) { snapFired.append(show); });
        snapBox->click();
        check(snapFired == QList<bool>({true}) && page.showSnapshots() && snapBox->isChecked(),
              "★ Z14 点它 = 发 showSnapshotsChanged(true)（这一页自己不重摆任何东西）",
              QStringLiteral("发了 %1 次").arg(snapFired.size()));
        snapBox->click();
        check(snapFired == QList<bool>({true, false}) && !page.showSnapshots(),
              "Z14 再点一下 = 发 false（这一位是**两档**、不是「开一次就完」）",
              QStringLiteral("发了 %1 次").arg(snapFired.size()));

        // **摆初始值不发信号**（和 setChips 同一条规矩）：开页那一下 MainWindow 会自己摆
        // 一次 chip，要是这一下也算"用户拨了"，它就在开页那半秒里把整份列表重算一遍。
        snapFired.clear();
        page.setShowSnapshots(true);
        check(snapFired.isEmpty() && snapBox->isChecked() && page.showSnapshots(),
              "★★ Z14 setShowSnapshots() 是「调用方在摆初始值」：一位都发不出去",
              QStringLiteral("发了 %1 次").arg(snapFired.size()));
        page.setShowSnapshots(false);
        check(snapFired.isEmpty() && !snapBox->isChecked(),
              "Z14 摆回不勾那一下同样一声不响");
        page.setShowSnapshots(true);
        snapFired.clear();

        // 裸 Alt 那一发。**只认裸的**（Ctrl/Shift/Meta 同按一律放过去）—— 别人家的快捷键
        // 组合里带 Alt 的多得是，吞掉它们等于把这一页变成一个"按键黑洞"。
        {
            QKeyEvent alt(QEvent::KeyPress, Qt::Key_Alt, Qt::NoModifier);
            QApplication::sendEvent(&page, &alt);
            check(snapFired == QList<bool>({false}) && !snapBox->isChecked(),
                  "★ Z14 裸 Alt 一发 = 拨一下那颗勾（用户原话「按 alt 键可以切换其显示」）",
                  QStringLiteral("发了 %1 次").arg(snapFired.size()));
            // 小键盘那一档：Qt 在某些键盘布局下会把裸 Alt 报成 Alt+Keypad，掩掉它才是
            // "裸"的正确判据（漏掉的话这些键盘上快捷键直接哑掉）。
            QKeyEvent altPad(QEvent::KeyPress, Qt::Key_Alt, Qt::KeypadModifier);
            QApplication::sendEvent(&page, &altPad);
            check(snapFired == QList<bool>({false, true}) && snapBox->isChecked(),
                  "Z14 Alt（带小键盘那一档修饰）照样算裸 Alt",
                  QStringLiteral("发了 %1 次").arg(snapFired.size()));
            snapFired.clear();
            QKeyEvent ctrlAlt(QEvent::KeyPress, Qt::Key_Alt, Qt::ControlModifier);
            QApplication::sendEvent(&page, &ctrlAlt);
            QKeyEvent shiftAlt(QEvent::KeyPress, Qt::Key_Alt, Qt::ShiftModifier);
            QApplication::sendEvent(&page, &shiftAlt);
            check(snapFired.isEmpty() && snapBox->isChecked(),
                  "★★ Z14 Ctrl+Alt / Shift+Alt **不许**拨它（只认裸 Alt 那一发）",
                  QStringLiteral("发了 %1 次").arg(snapFired.size()));
            // 松手那一发不是"再拨一下"。
            QKeyEvent altUp(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
            QApplication::sendEvent(&page, &altUp);
            check(snapFired.isEmpty() && snapBox->isChecked(),
                  "Z14 松手（KeyRelease）不拨第二下（一次手势 = 一次切换）");
        }

        // ---- c3) 同一格上的「模组设置」----
        //
        // 要求：「对于模组，点进去和资源包差不多，但是右上角显示快照版那里替换为
        // 模组设置（先占位），而且和下载页有点区别，不要不小心把下载页改了」；形态那一问
        // 选的是「画成灰的、点不动」，替换范围选的是「只有实例设置那条路换」。
        //
        // 这一节钉的是**这一页自己做得了的那一半**：那一格默认摆谁、换过去之后那颗勾选框
        // 收没收、那颗按钮灰不灰、以及"摆着它的时候裸 Alt 拨不动那一颗看不见的勾"。
        // 「什么时候摆它」是 MainWindow 的事（按来路 + 种类），在 mw_check 的 T24.7 / ZJ9
        // 那两条端到端钉。
        {
            // 摆回干净的一档再量（上面 Alt 那一节把勾拨上去了，而这里要量的是"默认那一格"）。
            page.setShowSnapshots(false);
            snapFired.clear();

            auto *modButton = page.modSettingsButton();
            check(modButton != nullptr, "★ Z14 那颗「模组设置」按 modSettingsButton() 拿得到"
                                        "（它是**建一次留着**的，只是默认收着）");
            check(!page.modSettingsSlotShown() && modButton != nullptr &&
                      !modButton->isVisible() && snapBox->isVisible(),
                  "★★ Z14 默认那一格是「显示快照版」：那颗「模组设置」连画都不画",
                  QStringLiteral("槽位=%1 模组设置露着=%2")
                      .arg(page.modSettingsSlotShown() ? QStringLiteral("模组设置")
                                                       : QStringLiteral("快照版"),
                           modButton != nullptr && modButton->isVisible() ? QStringLiteral("是")
                                                                         : QStringLiteral("不是")));
            check(modButton != nullptr &&
                      modButton->text() == ResourceDetailPage::modSettingsText() &&
                      modButton->text() == QStringLiteral("模组设置"),
                  "★ Z14 那四个字就是「模组设置」（唯一出处 ResourceDetailPage::modSettingsText()）",
                  modButton != nullptr ? modButton->text() : QString());

            page.setModSettingsSlot(true);
            check(page.modSettingsSlotShown() && modButton != nullptr &&
                      modButton->isVisible() && snapBox->isVisible(),
                  "★★ Z14 摆上「模组设置」时「显示快照版」仍然露着（两者不再共用一格）",
                  QStringLiteral("勾选框露着=%1")
                      .arg(snapBox->isVisible() ? QStringLiteral("是") : QStringLiteral("不是")));
            check(modButton != nullptr && modButton->isEnabled(),
                  "★★ Z14 模组设置入口可用，打开当前实例配置文件管理");

            // 两者都在时裸 Alt 照旧拨「显示快照版」。
            snapFired.clear();
            {
                QKeyEvent altWhileMod(QEvent::KeyPress, Qt::Key_Alt, Qt::NoModifier);
                QApplication::sendEvent(&page, &altWhileMod);
            }
            check(snapFired.size() == 1 && snapBox->isChecked() && page.showSnapshots(),
                  "★★ Z14 摆着「模组设置」时裸 Alt 仍然拨「显示快照版」",
                  QStringLiteral("发了 %1 次").arg(snapFired.size()));
            page.setShowSnapshots(false);
            // 而 Esc 照旧是"离开这一页"：**摆着它的那一档**只多挡了裸 Alt 那一发，别的照旧
            // 交给基类（吞掉的话这一页就再也退不出去了）。用一张一次性的页量 —— 别把上面
            // 那一张关掉，后面还有别的量要它露着。
            {
                ResourceDetailPage escWithMod;
                escWithMod.setModSettingsSlot(true);
                escWithMod.show();
                QApplication::processEvents();
                const bool escWasVisible = escWithMod.isVisible();
                QKeyEvent escUnderMod(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(&escWithMod, &escUnderMod);
                QApplication::processEvents();
                check(escWasVisible && !escWithMod.isVisible(),
                      "★★ Z14 摆着「模组设置」时 Esc 照旧交给基类 ⇒ 还是「离开这一页」"
                      "（这一档只多挡裸 Alt 那一发）");
            }

            page.setModSettingsSlot(false);
            check(!page.modSettingsSlotShown() && snapBox->isVisible() &&
                      modButton != nullptr && !modButton->isVisible(),
                  "★★ Z14 摆回去之后「显示快照版」又露出来、那颗「模组设置」收回去"
                  "（这一位是**两档**，而且每一次开页都要显式摆一次 —— 页面是共用对象）");
        }

        // Esc 是"离开这一页"的通用键（覆盖层那边也这么说的）。新接的 keyPressEvent 必须把
        // **不是裸 Alt** 的都交给基类 —— 吞掉的话这一页就再也退不出去了。
        {
            ResourceDetailPage escProbe;
            escProbe.show();
            QApplication::processEvents();
            const bool wasVisible = escProbe.isVisible();
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&escProbe, &esc);
            QApplication::processEvents();
            check(wasVisible && !escProbe.isVisible(),
                  "★★ Z14 Esc 照旧交给基类 ⇒ 还是「离开这一页」（新那段没把它吞掉）");
        }

        // 焦点：这一页在覆盖层里是**塞进内容区的一个孩子**，而顶上那颗「返回」按钮是它的
        // **兄弟**、还先拿着焦点 —— 键盘事件不会从兄弟冒泡过来，这一页不在 showEvent 里
        // 自己抢一手就一个 Alt 都收不到（这条路上的症状是"快捷键按了没反应"，静默的）。
        {
            // 这个 host 必须是**真窗口**（QDialog 那种，show() 那一下才成为"激活窗口"）：
            // 焦点只挂在激活窗口上，而这一页自己得是**孩子控件**（不是另一个窗口）——
            // 那是覆盖层里的形状，也正是"键盘事件到不了它手上"的原因。
            QDialog host;
            host.resize(600, 400);
            auto *sibling = new QPushButton(QStringLiteral("返回"), &host);
            sibling->move(8, 8);
            auto *child = new ResourceDetailPage;
            child->setParent(&host, Qt::Widget);  // 和 execEmbedded 里那一下同一个意思
            child->move(0, 40);
            child->resize(500, 320);
            host.show();
            QApplication::processEvents();
            sibling->setFocus();
            QApplication::processEvents();
            const bool siblingHasFocus = QApplication::focusWidget() == sibling;
            check(siblingHasFocus, "Z14 前提：焦点先摆在那一页的兄弟控件上",
                  QApplication::focusWidget() != nullptr
                      ? QApplication::focusWidget()->metaObject()->className()
                      : QStringLiteral("没有焦点控件"));
            child->hide();
            child->show();  // 这一下才是"覆盖层把它摆上来"
            QApplication::processEvents();
            QWidget *holder = QApplication::focusWidget();
            check(holder == child || child->isAncestorOf(holder),
                  "★ Z14 摆上来那一下自己把焦点收回来（焦点在兄弟身上 ⇒ 裸 Alt 到不了它手上）",
                  holder != nullptr ? holder->metaObject()->className()
                                    : QStringLiteral("没有焦点控件"));
            // 走一遍真路径：把 Alt 发给**当下拿着焦点的那个控件**，看它落不落到这一页上。
            auto *childBox =
                child->findChild<QCheckBox *>(ResourceDetailPage::showSnapshotsCheckBoxName());
            const bool before = childBox != nullptr && childBox->isChecked();
            QKeyEvent alt(QEvent::KeyPress, Qt::Key_Alt, Qt::NoModifier);
            if (holder != nullptr) {
                QApplication::sendEvent(holder, &alt);
            }
            QApplication::processEvents();
            check(childBox != nullptr && childBox->isChecked() != before,
                  "★ Z14 焦点在它身上时，Alt 真能拨到那颗勾（不是只有直发才管用）");
        }

        // ---- d) 版本列表：分组、行里的东西、空表那两种 ----
        QList<ResourceDetail::Group> groups;
        ResourceDetail::Group stable;
        stable.title = QStringLiteral("Fabric 1.21.1");
        ResourceDetail::Row rowStable;
        rowStable.key = QStringLiteral("iris-1.8.8");
        rowStable.title = QStringLiteral("1.8.8+1.21.1");
        rowStable.subText = QStringLiteral("Fabric · 1.8.8+1.21.1-fabric.jar");
        rowStable.channel = 0;  // 正式版
        rowStable.tooltip = QStringLiteral("1.8.8+1.21.1-fabric.jar");
        ResourceDetail::Row rowBeta;
        rowBeta.key = QStringLiteral("iris-beta-1.8.14");
        rowBeta.title = QStringLiteral("1.8.14-beta.1+1.21.1");
        rowBeta.subText = QStringLiteral("Fabric · 1.8.14-beta.1+1.21.1-fabric.jar · 测试版");
        rowBeta.channel = 1;  // 测试版
        stable.rows.append(rowStable);
        stable.rows.append(rowBeta);
        ResourceDetail::Group older;
        older.title = QStringLiteral("Fabric 1.21");
        ResourceDetail::Row rowOld;
        rowOld.key = QStringLiteral("iris-1.7.3");
        rowOld.title = QStringLiteral("1.7.3+1.21");
        rowOld.subText = QStringLiteral("Fabric · 1.7.3+1.21-fabric.jar");
        rowOld.channel = 0;
        older.rows.append(rowOld);
        // 默认收起（issue #20）；这一节后面要点行、点组头，所以这两组显式摊开。
        stable.expanded = true;
        older.expanded = true;
        groups.append(stable);
        groups.append(older);

        // 先按默认摆一遍：组头没标 expanded 就是**收起**的。
        {
            QList<ResourceDetail::Group> collapsed = groups;
            collapsed[0].expanded = false;
            collapsed[1].expanded = false;
            page.setGroups(collapsed);
            check(page.groupCount() == 2 && !page.groupExpandedAt(0) && !page.groupExpandedAt(1),
                  "★★ Z14 组头默认**收起**（点标题行才展开）");
            collapsed[1].expanded = true;
            page.setGroups(collapsed);
            check(!page.groupExpandedAt(0) && page.groupExpandedAt(1),
                  "★★ Z14 只有标了 expanded 的那一组摊开，其余收起");
        }
        page.setGroups(groups);
        QTreeWidget *list = page.versionList();
        check(page.groupCount() == 2 && page.versionRowCount() == 3,
              "Z14 摆出来的是 2 个组头 + 3 行版本",
              QStringLiteral("%1 组 / %2 行").arg(page.groupCount()).arg(page.versionRowCount()));
        check(page.groupTitleAt(0) == stable.title && page.groupTitleAt(1) == older.title,
              "Z14 组头写的就是 MainWindow 给的那两截（这一页不自己拼）",
              QStringLiteral("%1 / %2").arg(page.groupTitleAt(0), page.groupTitleAt(1)));
        check(page.groupExpandedAt(0) && page.groupExpandedAt(1),
              "Z14 标了 expanded 的组头摊开");
        check(page.placeholderText().isEmpty(),
              "Z14 摆上真内容之后那句说明就撤了（不是叠在列表上）");
        auto *delegate = qobject_cast<VersionTreeDelegate *>(list->itemDelegate());
        check(delegate != nullptr && !delegate->leafArrow(),
              "★ Z14 版本那一层**不画**右边那个展开箭头（点它是「下它」这个动作，"
              "不是「里面还有一页」—— 挂个箭头等于骗人说里面有东西）");
        check(delegate != nullptr && delegate->iconSize() == badge &&
                  list->iconSize() == QSize(badge, badge),
              "Z14 列表里那一格图标就是那一枚字母方块的大小",
              QStringLiteral("代理 %1 / 表 %2")
                  .arg(delegate != nullptr ? delegate->iconSize() : -1)
                  .arg(list->iconSize().width()));
        check(!list->rootIsDecorated() && !list->expandsOnDoubleClick(),
              "Z14 和下载页那四张表**逐项相同**：根上那个小三角不画、双击不自己展开");

        QTreeWidgetItem *rowItem = list->topLevelItem(0)->child(0);
        check(rowItem != nullptr &&
                  rowItem->data(0, ResourceDetailPage::rowKeyRole()).toString() == rowStable.key,
              "★ Z14 行的身份挂在 rowKeyRole() 里（这一页只原样带回来、一个字都不解释）",
              rowItem != nullptr
                  ? rowItem->data(0, ResourceDetailPage::rowKeyRole()).toString()
                  : QString());
        check(rowItem != nullptr &&
                  rowItem->data(0, MainWindowUI::rowSubTextRole()).toString() == rowStable.subText,
              "Z14 第二行小字的出处是 MainWindow（rowSubTextRole）",
              rowItem != nullptr
                  ? rowItem->data(0, MainWindowUI::rowSubTextRole()).toString()
                  : QString());
        const QPixmap rowBadge =
            rowItem != nullptr ? rowItem->data(0, MainWindowUI::rowIconRole()).value<QPixmap>()
                               : QPixmap();
        check(!rowBadge.isNull() && rowBadge.size() == QSize(badge, badge),
              "★ Z14 行前面那枚就是**那一档**的字母方块（尺寸 = badgeSize）",
              QStringLiteral("%1x%2").arg(rowBadge.width()).arg(rowBadge.height()));
        check(rowItem != nullptr && rowItem->toolTip(0) == rowStable.tooltip,
              "Z14 文件名那种长正文挂在鼠标提示上（行里那两行画不下它）");
        check(list->topLevelItem(0) != nullptr && list->topLevelItem(0)->child(1) != nullptr &&
                  list->topLevelItem(0)
                          ->child(1)
                          ->data(0, ResourceDetailPage::rowKeyRole())
                          .toString() == rowBeta.key,
              "Z14 组里第二行是给的那个第二行（顺序原样，不重排）");

        // 点一个版本 = 要下它；点组头 = 开/关（**两档的分别**就在这儿）。真发鼠标事件
        // （和 E3 那一段同一个写法）：走的是这一页自己那条接线，不是绕过去调处理函数。
        QStringList activated;
        QObject::connect(&page, &ResourceDetailPage::rowActivated,
                         [&activated](const QString &key) { activated.append(key); });
        const auto clickRow = [&page](QTreeWidgetItem *item) {
            QTreeWidget *tree = page.versionList();
            tree->scrollToItem(item);
            const QPoint pos = tree->visualItemRect(item).center();
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos),
                              QPointF(tree->viewport()->mapToGlobal(pos)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos),
                                QPointF(tree->viewport()->mapToGlobal(pos)), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &release);
            QApplication::processEvents();
        };
        clickRow(rowItem);
        check(activated.size() == 1 && activated.first() == rowStable.key,
              "★ Z14 点一个版本 = 发 rowActivated(那一行的 key)（= 要下它）",
              activated.join(QStringLiteral(" / ")));
        activated.clear();
        QTreeWidgetItem *headerItem = list->topLevelItem(0);
        const bool wasExpanded = headerItem->isExpanded();
        clickRow(headerItem);
        check(activated.isEmpty() && headerItem->isExpanded() != wasExpanded,
              "★ Z14 点组头 = 自己开/关（那不是「下一个版本」，一个信号都不发）",
              QStringLiteral("展开状态 %1 → %2")
                  .arg(wasExpanded ? QStringLiteral("开着") : QStringLiteral("收着"),
                       headerItem->isExpanded() ? QStringLiteral("开着") : QStringLiteral("收着")));
        activated.clear();
        clickRow(headerItem);
        check(activated.isEmpty() && headerItem->isExpanded() == wasExpanded,
              "Z14 再点一下又摆回去（这一行是个开关，两下都算）");

        // 空表那两种「空」：**一个版本都没发过**（真的没内容）和**正在读 / 没取到**
        // （内容还在路上）。两条走的是同一条路（顶层条目 + 粗体），但说的话不一样。
        page.setGroups({});
        check(page.groupCount() == 1 && page.versionRowCount() == 0 &&
                  page.placeholderText() == QStringLiteral("这个资源一个版本都没发过"),
              "★ Z14 空表**不摆一片空白**：摆一句「这个资源一个版本都没发过」"
              "（那条说明自己算一条顶层条目）",
              page.placeholderText());
        page.setPlaceholder(QStringLiteral("正在读版本列表…"));
        check(page.placeholderText() == QStringLiteral("正在读版本列表…") &&
                  page.groupTitleAt(0) == QStringLiteral("正在读版本列表…"),
              "★ Z14 「正在读…」那句是同一条路摆上去的（代理给顶层画粗体，"
              "和下载页那四张表里「没取到」那句一个形状）",
              page.groupTitleAt(0));
        page.setPlaceholder(QString());
        check(page.placeholderText().isEmpty() && page.groupCount() == 0,
              "Z14 传空串 = 把列表清空（什么都不摆）");

        // ---- e) 行尾那个箭头的命中格：画的和点的是同一处 ----
        {
            QTreeWidget arrowProbe;
            arrowProbe.setItemDelegate(new VersionTreeDelegate(&arrowProbe));
            auto *longItem = new QTreeWidgetItem(&arrowProbe);
            longItem->setText(0, QStringLiteral("钠 Sodium 这是一个很长的分类名，长到足够顶到右边"));
            // 那一笔箭头**只画给真有孩子的行**（`isChannel && !hasChildren` 那一条直接
            // return，见代理里那段）—— 所以探针这条顶层行得挂一个孩子，不然它一格都不画。
            longItem->addChild(new QTreeWidgetItem(QStringList{QStringLiteral("1.2.3")}));
            const QModelIndex index = arrowProbe.model()->index(0, 0);
            const QRect rowBox(0, 0, 400, MainWindowUI::versionTreeRowHeight());
            const int trail = MainWindowUI::versionTreeTrailWidth();
            const QRect hit = VersionTreeDelegate::arrowRect(rowBox);
            // 画出来那格「展开 / 收起」= 命中格四周各收 3 px（行尾不再画箭头，体验反馈 8）。
            const QRect glyph = hit.adjusted(3, 3, -3, -3);
            check(glyph.width() == trail && hit.contains(glyph) &&
                      hit.width() - glyph.width() == 6 && hit.height() - glyph.height() == 6 &&
                      glyph.right() == rowBox.right() - MainWindowUI::versionTreeRowPadding(),
                  "★ Z14 命中格 = 行尾那格文字 + 四周各 3 px（画与点同一个出处）",
                  QStringLiteral("命中 %1x%2 / 画 %3x%4")
                      .arg(hit.width())
                      .arg(hit.height())
                      .arg(glyph.width())
                      .arg(glyph.height()));
            check(rowBox.contains(hit),
                  "★ Z14 命中格整个装在行里（12 px 的点击目标太小，四周各宽 3 px 是手感余量）");

            // 真画一遍这一行，把**墨迹**量出来：箭头的墨（正文色那一路）必须落在命中格里，
            // 而正文的墨必须停在命中格左边 —— 这就是"点那一格不会同时点到正文"。
            QImage canvas(rowBox.size(), QImage::Format_ARGB32_Premultiplied);
            canvas.fill(Qt::white);
            QPainter painter(&canvas);
            QStyleOptionViewItem opt;
            opt.rect = rowBox;
            opt.state = QStyle::State_Enabled;
            opt.font = arrowProbe.font();
            // 要的就是那棵树自己：代理判"有没有孩子"是 `qobject_cast<QTreeView *>(option.widget)`
            // 之后问它的 model（给视口不行 —— 视口不是 QTreeView）。
            opt.widget = &arrowProbe;
            // 正文色改成一种一眼认得出的颜色（顶层那一行的箭头用的就是这一档），
            // 于是图里那种颜色的像素 = 这一行画的字和箭头。
            const QColor marker(255, 0, 255);
            opt.palette.setColor(QPalette::WindowText, marker);
            opt.palette.setColor(QPalette::Text, marker);
            opt.palette.setColor(QPalette::PlaceholderText, marker);  // 「展开」用次要色画
            opt.palette.setColor(QPalette::Base, QColor(255, 255, 255));
            opt.palette.setColor(QPalette::Highlight, QColor(255, 255, 255));
            arrowProbe.itemDelegate()->paint(&painter, opt, index);
            painter.end();

            QRect arrowInk;
            QRect textInk;
            for (int y = 0; y < canvas.height(); ++y) {
                for (int x = 0; x < canvas.width(); ++x) {
                    const QColor color = canvas.pixelColor(x, y);
                    if (color.red() < 200 || color.green() > 80 || color.blue() < 200) {
                        continue;
                    }
                    const QRect px(x, y, 1, 1);
                    if (x >= hit.left()) {
                        arrowInk = arrowInk.isNull() ? px : arrowInk.united(px);
                    } else {
                        textInk = textInk.isNull() ? px : textInk.united(px);
                    }
                }
            }
            check(!arrowInk.isNull() && hit.contains(arrowInk),
                  "★ Z14 真画出来那格「展开」落在命中格里（点那一格点得到它）",
                  QStringLiteral("箭头墨迹 %1,%2-%3,%4 / 命中格 %5,%6-%7,%8")
                      .arg(arrowInk.left())
                      .arg(arrowInk.top())
                      .arg(arrowInk.right())
                      .arg(arrowInk.bottom())
                      .arg(hit.left())
                      .arg(hit.top())
                      .arg(hit.right())
                      .arg(hit.bottom()));
            check(!textInk.isNull() && textInk.right() < hit.left(),
                  "★ Z14 而正文的墨正好停在命中格左边（那一格上按下去只可能是箭头）",
                  QStringLiteral("正文到 x = %1，命中格从 x = %2 起")
                      .arg(textInk.right())
                      .arg(hit.left()));
            check(arrowInk.width() <= trail && arrowInk.width() > MainWindowUI::versionTreeArrowSize(),
                  "Z14 行尾墨迹是一行字（宽于 12 px 的旧箭头、装在 trail 那一格里）",
                  QStringLiteral("%1x%2").arg(arrowInk.width()).arg(arrowInk.height()));
        }

        page.hide();
    }
    return 0;
}




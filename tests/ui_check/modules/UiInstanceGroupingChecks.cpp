#include "Support.hpp"
int UiInstanceGroupingChecks(MainWindowUI &ui, QTreeWidget * &instanceList) {

    {
        // 实例列表从 QListWidget 换成 QTreeWidget 是分组要求的（组是父节点、实例是
        // 子节点）。这几条钉住树的**配置**：开了哪些开关、缩进多少 —— 它们都是量出来的
        // 取舍（见 MainWindowUI.cpp 里那几行注释），改动了就该有人看一眼。
        check(instanceList != nullptr, "实例列表按 objectName 找得到");
        if (instanceList != nullptr) {
            check(instanceList->topLevelItemCount() == 0 && instanceList->currentItem() == nullptr,
                  "界面刚搭好时是空的（数据由 MainWindow 灌）");
            check(instanceList->isHeaderHidden(), "没有表头（这里只有一列：名字）");
            // 那个展开/折叠的小三角**是我们自己画的**（组行行尾那颗箭头，
            // 见 InstanceGroupDelegate），所以原生的那个必须关掉 —— 要求「每一个组和
            // 版本下载几乎一致，点一下打开/关闭，右侧有个箭头和齿轮」，版本树那边也是
            // setRootIsDecorated(false)（两棵树同一个做法）。
            // NOTE: 关它要走 setRootIsDecorated(false)，**不能**用 DontShowIndicator ——
            // 后者会让整条分类的子条目静默消失（见版本树那几条注释）。
            check(!instanceList->rootIsDecorated(),
                  "不画原生那个小三角（展开/折叠的箭头是组行里自己画的，和版本树一个做法）");
            check(!instanceList->uniformRowHeights(),
                  "组头 36 / 实例行 60 两档高度 ⇒ 不开 uniformRowHeights（开了会全按第一行组头算）");
            check(instanceList->indentation() == 0,
                  "组下面的实例不缩进（设计稿：实例行和组头左缘对齐）",
                  QString::number(instanceList->indentation()));
        }

        // ---- 设计依赖的两条 Qt 行为（拿独立控件验，不经过产品代码） ----
        QTreeWidget probe;
        auto *header = new QTreeWidgetItem(&probe);
        header->setText(0, MainWindowUI::groupItemText(QStringLiteral("整合包"), 3));
        header->setData(0, MainWindowUI::groupIdRole(), QStringLiteral("g1"));
        header->setFlags(header->flags() & ~Qt::ItemIsSelectable);
        auto *child = new QTreeWidgetItem(header);
        child->setText(0, QStringLiteral("1.20.1"));
        child->setData(0, MainWindowUI::instanceIdRole(), QStringLiteral("1.20.1"));

        // 组头**必须点不亮**：它是"一堆东西的标题"，不是"一个能启动的实例"。做法是
        // 清掉 ItemIsSelectable。这一段**真的发一次鼠标事件**验它，而不是去问
        // selectionCommand() —— 后者只答"选择命令是什么"，答不了"点完屏幕上什么样"。
        //
        // NOTE: 这里量出来的是**两条不同的事实**，它们决定了整套守卫的形状：
        // ① 组头**不会进"被选中"的集合** → 点它不会亮（清 flag 有用）；
        // ② 但它**仍然会变成 currentItem()** → 光靠清 flag 兜不住"当前选中的
        // 是哪个实例"，每个读 currentItem() 的地方都必须自己过一遍
        // instanceIdOf()。②这条要是没了（换了 Qt、或者谁把 flag 加回去），
        // 下面那条断言会红，而红的地方正是"为什么需要 instanceIdOf()"。
        probe.setCurrentItem(child);
        probe.resize(240, 120);
        probe.show();
        QApplication::processEvents();
        const auto clickAt = [&probe](const QPoint &pos) {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos),
                              QPointF(probe.viewport()->mapToGlobal(pos)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(probe.viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos),
                                QPointF(probe.viewport()->mapToGlobal(pos)), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(probe.viewport(), &release);
            QApplication::processEvents();
        };

        clickAt(probe.visualItemRect(header).center());
        check(!probe.selectedItems().contains(header),
              "① 点组头，它不会进被选中的集合（那一行不会亮）");
        check(probe.currentItem() == header,
              "② 但它照样变成 currentItem() —— 所以读的时候必须自己守卫",
              probe.currentItem() != nullptr ? probe.currentItem()->text(0)
                                             : QStringLiteral("<null>"));

        clickAt(probe.visualItemRect(child).center());
        check(probe.currentItem() == child && probe.selectedItems().contains(child),
              "点实例条目照常能选中（这条防护没有误伤实例）");

        // 反向的两条：**setCurrentItem() 不看 ItemIsSelectable**，方向键也只跳过
        // !ItemIsEnabled。所以每个读 currentItem() 的地方都要自己判"这条是不是组头"，
        // 不能指望控件挡着 —— 这就是 MainWindow::instanceIdOf() 存在的理由。
        probe.setCurrentItem(header);
        check(probe.currentItem() == header,
              "setCurrentItem() 能停在被禁选的组头上（所以读的时候必须自己守卫）");
        check(probe.currentItem() != nullptr &&
                  probe.currentItem()->data(0, MainWindowUI::groupIdRole()).isValid(),
              "组头/实例的区分办法：组头挂着组 id（groupIdRole）");
        probe.setCurrentItem(child);
        check(probe.currentItem() != nullptr &&
                  !probe.currentItem()->data(0, MainWindowUI::groupIdRole()).isValid() &&
                  probe.currentItem()->data(0, MainWindowUI::instanceIdRole()).toString() ==
                      QStringLiteral("1.20.1"),
              "实例条目没有组 id 那个 role，实例 id 在 instanceIdRole 里");

        // ---- 组行行尾那两格的几何（用户「右侧有个箭头和齿轮」） ----
        //
        // NOTE: 拿**合成出来的行**量，不靠树上那一行 —— 这台机器上 offscreen 的字体库是空的
        // （行高只有十几像素），"行够高时齿轮不缩"那一档在这儿永远走不到，而那一档
        // 正是真机上的样子（行高 ≈ 一行字 + 上下留白）。合成一个高行就能两档都钉住。
        {
            const int tallH = MainWindowUI::instanceTreeRowHeight();
            const int shortH = MainWindowUI::instanceGroupGearSize() - 4;  // 12：比齿轮还矮
            const QRect tallRow(0, 0, 400, tallH);
            const QRect shortRow(0, 0, 400, shortH);

            // ① 行够高：画出来的那一圈就是 instanceGroupGearSize()（16），位置正好是命中格
            // 往里收 3 px —— 和 VersionTreeDelegate 那几个行内按钮同一个做法。
            check(InstanceGroupDelegate::gearGlyphRect(tallRow) ==
                      InstanceGroupDelegate::gearRect(tallRow).adjusted(3, 3, -3, -3),
                  "行够高时齿轮画的就是那 16 px（命中格四周那 3 px 是手感余量，不画）",
                  QString::number(InstanceGroupDelegate::gearGlyphRect(tallRow).width()));
            // ② 行矮：画出来的那一圈缩到行高。不缩的话它上下各冒出去一半 —— 屏幕上就是
            // "齿轮被上下各切掉一条"（整屏重绘压到隔壁那两条行上，局部重绘被切掉）。
            check(InstanceGroupDelegate::gearGlyphRect(shortRow).height() == shortH &&
                      tallRow.contains(InstanceGroupDelegate::gearGlyphRect(tallRow)) &&
                      shortRow.contains(InstanceGroupDelegate::gearGlyphRect(shortRow)),
                  "画出来的齿轮一定装在行里（行矮就跟着缩，不留半颗在外面）",
                  QStringLiteral("行高 %1 → 齿轮高 %2")
                      .arg(shortH)
                      .arg(InstanceGroupDelegate::gearGlyphRect(shortRow).height()));
            check(tallRow.contains(InstanceGroupDelegate::chevronRect(tallRow)) &&
                      shortRow.contains(InstanceGroupDelegate::chevronRect(shortRow)),
                  "箭头也一样装在行里（它是「点一下打开/关闭」那一格，被切掉一条就看不清了）");
            check(InstanceGroupDelegate::gearGlyphRect(shortRow).right() <
                          InstanceGroupDelegate::chevronRect(shortRow).left() &&
                      InstanceGroupDelegate::gearGlyphRect(tallRow).right() <
                          InstanceGroupDelegate::chevronRect(tallRow).left(),
                  "两档里齿轮都排在箭头左边（行尾那两格，和版本树那几个行内按钮一样靠右）");
        }

        // ---- 组行行尾**只剩那颗齿轮** ----
        //
        // 要求：「这个点击一下就同时展开和调整了，其次这个箭头不应该在这里，而应该在
        // 分组设置里面，这里的删了，去分组设置里面加上，设置那个框下方的上移下移的按钮就
        // 删了」。上下移那两颗三角连同 `rowGroupMoveRole()` / 那两个掩码位一起从这棵树上
        // 删掉了，整个搬进「分组设置」那张组清单 —— 它长什么样、点一下管不管用钉在下面 N
        // 节（那边有真的 GroupDialog、真的行矩形）。
        //
        // NOTE: "那两个几何函数没了"编译器替我们钉住了（谁把 InstanceGroupDelegate::moveUpRect()
        // 加回来，这个工程当场编不过）。这里补的是**屏幕上那一格真的空着** —— "把画
        // 它们的那几行删了、几何留着"照样编得过，可用户看见的还是那两颗箭头。量的是
        // **墨**（和行底色不一样的像素），不是"整行有没有变"，理由见上面那几条。
        {
            const int tallH = MainWindowUI::instanceTreeRowHeight();
            QTreeWidget paintProbe;
            paintProbe.setRootIsDecorated(false);
            paintProbe.setUniformRowHeights(true);
            paintProbe.setItemDelegate(new InstanceGroupDelegate(&paintProbe));
            // 行尾那颗齿轮现在**只在鼠标停在那一行时才画**（用户那份改版文档：
            // 实例页"分组齿轮只在悬停时出现"）⇒ 量它之前得先把悬停那一位填上，而
            // `State_MouseOver` 是**视图**填的：不开 mouseTracking，Qt 一个字都不改它，
            // 下面那发合成的鼠标移动就推不动（这个坑上面 R 节那个刷新圈探针踩过，同一句话）。
            paintProbe.setMouseTracking(true);
            auto *paintItem = new QTreeWidgetItem(&paintProbe);
            paintItem->setText(0, MainWindowUI::groupItemText(QStringLiteral("整合包"), 3));
            paintItem->setData(0, MainWindowUI::groupIdRole(), QStringLiteral("g1"));
            paintItem->setData(0, MainWindowUI::rowGroupGearRole(), true);
            paintProbe.resize(400, tallH);
            paintProbe.show();
            QApplication::processEvents();
            // 量下面那些"墨"之前，必须先把**当前项摘掉**。
            //
            // 不摘的话量出来的是 Fusion 当前项那一块渐变，单点取样必然错（同一个坑在
            // "量像素的基线要按画出来的那一块取"那条里也踩过）。这是判据的前提，不是
            // 产品的事：探针和代理都不看产品改了什么。
            paintProbe.clearSelection();
            paintProbe.setCurrentItem(nullptr);
            QApplication::processEvents();
            const QRect paintRow = paintProbe.visualItemRect(paintItem);
            // 悬停：那颗齿轮只在悬停时才画（见上面 setMouseTracking 那条），所以先把
            // 光标挪进这一行正中，再截图（合成一发 MouseMove，和 R 节那个悬停探针同一个做法）。
            {
                const QPoint cursor = paintRow.center();
                QMouseEvent move(QEvent::MouseMove, QPointF(cursor),
                                 QPointF(paintProbe.viewport()->mapToGlobal(cursor)),
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(paintProbe.viewport(), &move);
                QApplication::processEvents();
            }
            const QImage shot = captureWidget(paintProbe.viewport());
            // 行底色：最右边那一列一定是留白（齿轮和展开箭头都在它左边，各留了一圈版面）。
            // 悬停之后这一行的底色是**悬停那一档**，它只铺在 `cardRect()` 里（上下各收掉
            // 半道空），所以这两个采样点的 y 都收进卡片里取 —— 有一个落在卡片外的话，
            // 取到的是行的原底色，下面"底色是个定值"那条哨兵会当场红（而且红得像是产品坏了）。
            const QRect hoverCardRect = InstanceGroupDelegate::cardRect(paintRow);
            const QRgb bg = shot.pixel(paintRow.right() - 1, hoverCardRect.center().y());
            const auto inkIn = [&shot, bg](const QRect &cell) {
                const QRect clip = cell.intersected(shot.rect());
                int n = 0;
                for (int y = clip.top(); y <= clip.bottom(); ++y) {
                    for (int x = clip.left(); x <= clip.right(); ++x) {
                        if (shot.pixel(x, y) != bg) {
                            ++n;
                        }
                    }
                }
                return n;
            };
            const QRect gearCell = InstanceGroupDelegate::gearGlyphRect(paintRow);
            // 那两颗三角原本占的正是齿轮左边那一带：两个 18 px 宽的命中格、紧挨着齿轮
            // （`box = 箭头边长 + 6`）。
            // 取齿轮命中格左边 54 px 起、48 px 宽的一段，刚好把那一带整个盖住。
            const QRect oldArrowBand(InstanceGroupDelegate::gearRect(paintRow).left() - 54,
                                     paintRow.top(), 48, paintRow.height());
            // 哨兵：行底色得是个**定值**（当前项被摘掉之后就不会再有渐变 —— 上面那条
            // NOTE 说的就是"没摘掉的时候这一条自己会把整片渐变算成墨"）。做法是同一行里
            // 换个采样点再取一次，两次不一样就说明这一行还在渐变里，下面的判据不能信。
            const QRgb bgProbe2 = shot.pixel(paintRow.right() - 1, hoverCardRect.top() + 1);
            check(bg == bgProbe2,
                  "哨兵：这一行的底色是个定值（还在渐变里的话，下面那条量的是「渐变的行数」）",
                  QStringLiteral("中段 %1 / 顶端 %2")
                      .arg(bg, 8, 16, QLatin1Char('0'))
                      .arg(bgProbe2, 8, 16, QLatin1Char('0')));
            check(oldArrowBand.width() == 48 && paintRow.contains(oldArrowBand),
                  "哨兵：下面量的是**行内**齿轮左边那一段（不是一段落在行外的空矩形）",
                  QStringLiteral("band=%1,%2 %3x%4 / row=%5,%6 %7x%8")
                      .arg(oldArrowBand.x())
                      .arg(oldArrowBand.y())
                      .arg(oldArrowBand.width())
                      .arg(oldArrowBand.height())
                      .arg(paintRow.x())
                      .arg(paintRow.y())
                      .arg(paintRow.width())
                      .arg(paintRow.height()));
            check(inkIn(gearCell) > 0,
                  "哨兵：齿轮那一格里真的画了东西（下面那个「这一带没墨」的量法本身有效）",
                  QStringLiteral("齿轮格 %1 px 墨").arg(inkIn(gearCell)));
            // 反面：光标**不在**这一行上时，齿轮那一格一个像素的墨都没有 —— 这就是用户那份
            // 改版文档点名的"分组齿轮只在悬停时出现"。少了这一条，上面那条哨兵在"齿轮常显"
            // 的那一版上也照样绿：它只证明"量法有效"，不证明"只在悬停时才画"。
            // 挪出去用的是把光标发到**视口外面**那一发（和上面 R 节那个刷新圈探针逐字一样）。
            {
                QMouseEvent leave(QEvent::MouseMove, QPointF(QPoint(2, -6)),
                                  QPointF(paintProbe.viewport()->mapToGlobal(QPoint(2, -6))),
                                  Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(paintProbe.viewport(), &leave);
                QApplication::processEvents();
                const QImage plainShot = captureWidget(paintProbe.viewport());
                const QRgb plainBg =
                    plainShot.pixel(paintRow.right() - 1, hoverCardRect.center().y());
                const auto inkInPlain = [&plainShot, plainBg](const QRect &cell) {
                    const QRect clip = cell.intersected(plainShot.rect());
                    int n = 0;
                    for (int y = clip.top(); y <= clip.bottom(); ++y) {
                        for (int x = clip.left(); x <= clip.right(); ++x) {
                            if (plainShot.pixel(x, y) != plainBg) {
                                ++n;
                            }
                        }
                    }
                    return n;
                };
                check(inkInPlain(gearCell) == 0,
                      "★ 光标不在这一行上时，齿轮那一格一个像素的墨都没有"
                      "（「分组齿轮只在悬停时出现」—— 用户那份改版文档点名的那条）",
                      QStringLiteral("齿轮格 %1 px 墨").arg(inkInPlain(gearCell)));
                // 挪出去之后底色也得回到**行的原底色**（悬停那一档没跟出去）——不然上面那条
                // 「一个像素都没有」可能只是"整行都还是悬停色、齿轮恰好没画"这种假绿。
                // UI 7b：组头是一行手写小标题（设计稿 manage），悬停**不铺底色** ⇒ 挪出去前后底色一样。
                check(plainShot.pixel(paintRow.right() - 1, hoverCardRect.center().y()) == bg,
                      "★ 组头悬停不铺底色：光标挪出去前后这一行底色一样（UI 7b 组头是小标题）",
                      QStringLiteral("悬停 %1 / 挪出去 %2")
                          .arg(QColor::fromRgb(bg).name(), QColor::fromRgb(plainBg).name()));
            }
            // 量的是那一带**落在卡片里**的那一块：悬停之后行的底色只在 `cardRect()` 里
            // （上下各收掉半道空），拿整条 58 px 去比的话，上下那两道空自己就是"不是悬停色"
            // —— 量出来 144 px 全是那两条缝，和"箭头回没回来"一点关系都没有（加悬停
            // 那一层之后当场踩到）。两道空里本来也没人画箭头。
            const QRect arrowBandInCard = oldArrowBand.intersected(hoverCardRect);
            check(inkIn(arrowBandInCard) == 0,
                  "★ 组行行尾那两颗上下移箭头搬走之后，齿轮**左边**那一带一个像素的墨都没有"
                  "（谁把它们加回这棵树，这一条就红）",
                  QStringLiteral("那一带 %1 px 不是底色（量的是卡片里那一块 %2x%3）")
                      .arg(inkIn(arrowBandInCard))
                      .arg(arrowBandInCard.width())
                      .arg(arrowBandInCard.height()));

            // ⑧ 行高：**每一行**都是版本树那个常数（要求「每个版本啥的上下高一点，像
            // 下载页那样」）—— 组行和实例行都要。只管组行的话，平铺那一档（没分过组、
            // 第一行就是实例）会矮回去，而 uniformRowHeights 是拿第一行当整棵树的行高的
            // ⇒ "有没有分组"会让同一张树换个高矮。
            auto *plainItem = new QTreeWidgetItem(&paintProbe);
            plainItem->setText(0, QStringLiteral("1.20.1"));
            plainItem->setData(0, MainWindowUI::instanceIdRole(), QStringLiteral("1.20.1"));
            QApplication::processEvents();
            QStyleOptionViewItem heightOpt;
            heightOpt.font = paintProbe.font();
            const int groupH = paintProbe.itemDelegate()
                                   ->sizeHint(heightOpt, paintProbe.model()->index(0, 0))
                                   .height();
            const int itemH = paintProbe.itemDelegate()
                                  ->sizeHint(heightOpt, paintProbe.model()->index(1, 0))
                                  .height();
            // UI 7b（设计稿 manage）：组头 36、实例行 60（ListRowDelegate 那一档）。
            check(groupH == InstanceGroupDelegate::groupRowHeight() && itemH == ListRowDelegate::rowHeight(),
                  "组行 36 / 实例行 60（设计稿 manage 的两档）",
                  QStringLiteral("组 %1 / 实例 %2").arg(groupH).arg(itemH));
            check(paintRow.height() == InstanceGroupDelegate::groupRowHeight(),
                  "真的挂进一棵树里，那一行也是这么高",
                  QStringLiteral("visualItemRect 高 %1").arg(paintRow.height()));
        }

        // ---- ⑨ 行与行之间那道空 ----
        //
        // 要求「实例列表里每一个之间留一点空的距离」。做法是**画的那一圈上下各收掉半个
        // instanceTreeRowGap()**（`InstanceGroupDelegate::cardRect()`），收掉的那一带谁也不画，
        // 露出来的是树自己的底色。两件事分开钉：
        // ① 算术：卡片矩形 = 整条行减掉那道空、和整条行**同中心**，而且画出来的东西
        // （行尾那三颗按钮、组行那颗齿轮和箭头）一个不落全装在它里面；
        // ② 屏幕上真的收了：真树、真渲染，量**选中那一行**的底色带两头缩进去了多少。
        // ② 是这条改动的**唯一**判据：只留①的话，谁把那句 `opt.rect = cardRect(...)`
        // 删掉照样全绿（画还是按整条行画的），而屏幕上那道空又没了 —— 正是这个仓库
        // 栽过好几次的"假绿"形状。
        {
            const int h = MainWindowUI::instanceTreeRowHeight();
            const int gap = MainWindowUI::instanceTreeRowGap();
            const int pad = gap / 2;             // 上面那半个缺口
            const int padBottom = gap - pad;     // 底下的半个（奇数时多出来的 1 px 落在底下）
            const QRect row(0, 0, 400, h);
            const QRect card = InstanceGroupDelegate::cardRect(row);
            check(gap > 0 && card.height() == h - gap && card.width() == row.width() &&
                      card.top() == row.top() + pad && card.bottom() == row.bottom() - padBottom,
                  "卡片矩形 = 整条行上下各收掉半个那道空（左右一个像素都不动）",
                  QStringLiteral("行 %1 → 卡片 %2（空 %3）").arg(h).arg(card.height()).arg(gap));
            check(card.center() == row.center(),
                  "★ 收的是**上下各一半**：卡片和整条行**同中心** —— 不同中心的话，行尾那三颗"
                  "按钮（按整条行算的几何）会整体偏出去半格，看着就是「点得中、画得歪」",
                  QStringLiteral("行中心 %1,%2 / 卡片中心 %3,%4")
                      .arg(row.center().x()).arg(row.center().y())
                      .arg(card.center().x()).arg(card.center().y()));
            check(card.contains(InstanceGroupDelegate::gearGlyphRect(row)) &&
                      card.contains(InstanceGroupDelegate::chevronRect(row)),
                  "画出来的东西一个不落全装在卡片里（组行那颗齿轮和箭头、实例行那三颗）——"
                  "装不进去的话，收掉的那一带会把按钮上下切掉一条",
                  QStringLiteral("卡片 %1 高 / 行 %2 高").arg(card.height()).arg(h));

            // ② 真渲染：一棵只有实例行的树，选中它，量那一列"空白处"（文字右边、行尾三颗
            // 按钮左边）上下各有多少像素还是底色。带子两头各缩 pad / padBottom 才对。
            QTreeWidget gapProbe;
            gapProbe.setObjectName(QStringLiteral("wtfml_gap_probe"));
            gapProbe.setHeaderHidden(true);
            gapProbe.setRootIsDecorated(false);
            gapProbe.setUniformRowHeights(true);
            gapProbe.setItemDelegate(new InstanceGroupDelegate(&gapProbe));
            auto *gapItem = new QTreeWidgetItem(&gapProbe);
            gapItem->setText(0, QStringLiteral("1.20.1"));
            gapItem->setData(0, MainWindowUI::instanceIdRole(), QStringLiteral("1.20.1"));
            gapProbe.resize(400, h * 3);
            gapProbe.show();
            QApplication::processEvents();
            // 顺序不能换：**先摘当前项，再选**。
            gapProbe.setCurrentItem(nullptr);
            gapItem->setSelected(true);
            QApplication::processEvents();
            const QImage gapShot = captureWidget(gapProbe.viewport());
            const QRect gapRow = gapProbe.visualItemRect(gapItem);
            // 树底色：取视口最下面那一带（那里一条行都没有）。
            const QRgb gapBg = gapShot.pixel(2, gapShot.height() - 2);
            // 那一列：文字右边、行尾三颗按钮左边 —— 一段真的空白，那里有色只可能是底色带。
            const int blankX = gapRow.right() - 160;
            int firstInk = -1;
            int lastInk = -1;
            for (int y = gapRow.top(); y <= gapRow.bottom(); ++y) {
                for (int dx = 0; dx < 8; ++dx) {
                    if (gapShot.pixel(blankX + dx, y) != gapBg) {
                        if (firstInk < 0) {
                            firstInk = y;
                        }
                        lastInk = y;
                        break;
                    }
                }
            }
            check(firstInk >= 0,
                  "哨兵：选中那一行的底色带真的画出来了（没画出来的话，下面那条是恒绿的假断言）",
                  QStringLiteral("那一列 %1..%2 一个有色像素都没有").arg(gapRow.top()).arg(gapRow.bottom()));
            // UI 7b：实例行改由 ListRowDelegate 画，选中卡片铺满 60px 整行（设计稿行与行紧挨着）。
            Q_UNUSED(pad); Q_UNUSED(padBottom);
            check(firstInk == gapRow.top() && lastInk == gapRow.bottom(),
                  "★ 选中那一行的底色带铺满整行（ListRowDelegate 的选中卡片，设计稿 manage）",
                  QStringLiteral("行 %1..%2，带子 %3..%4（该是 %5..%6）")
                      .arg(gapRow.top()).arg(gapRow.bottom()).arg(firstInk).arg(lastInk)
                      .arg(gapRow.top()).arg(gapRow.bottom()));
            gapProbe.hide();
        }

        // ---- ⑩ 左边那截"分支区" ----
        //
        // 要求：「注意蓝色部分，左边那里长了，改一下」—— 实例是组的**子条目**，
        // 视图按 setIndentation(14) 在这一行的左边留出一截"分支区"，而
        // `QTreeView::drawRow()` 会**先**用选中的底色把这整截涂满（`PE_PanelItemViewRow`，
        // 矩形 `QRect(position, y, indentation, height)` —— **整条行高**），**然后**才轮到
        // 代理画卡片（卡片上下各收掉半个缺口）。于是选中一行时左边多出一根 14 px 宽、
        // 比卡片上下各冒出去半个缺口的竖条。
        //
        // 上面那段 ⑨ 为什么是绿的：它那棵树是**平铺**的（只有顶层条目、还没设
        // setIndentation），顶层条目的 indentation 是 `(0 + (rootDecoration ? 1 : 0)) * 14`
        // 而产品里 setRootIsDecorated(false) ⇒ 0 ⇒ 那一截宽度是 0、视图根本不涂。
        // 所以这条断言必须**自己搭一棵有父子的树**，不能复用 ⑨ 那棵。
        {
            const int h = MainWindowUI::instanceTreeRowHeight();
            QTreeWidget stripProbe;
            stripProbe.setObjectName(QStringLiteral("wtfml_branch_strip_probe"));
            stripProbe.setHeaderHidden(true);
            // 这三句和产品里逐字一样（见 MainWindowUI.cpp 里那句 setIndentation(14) 的注释）。
            stripProbe.setRootIsDecorated(false);
            stripProbe.setUniformRowHeights(true);
            stripProbe.setIndentation(14);
            stripProbe.setItemDelegate(new InstanceGroupDelegate(&stripProbe));
            auto *stripGroup = new QTreeWidgetItem(&stripProbe);
            stripGroup->setText(0, QStringLiteral("原版"));
            stripGroup->setData(0, MainWindowUI::groupIdRole(), QStringLiteral("default-vanilla"));
            auto *stripChild = new QTreeWidgetItem(stripGroup);
            stripChild->setText(0, QStringLiteral("1.20.1"));
            stripChild->setData(0, MainWindowUI::instanceIdRole(), QStringLiteral("1.20.1"));
            stripProbe.expandAll();
            stripProbe.resize(400, h * 4);
            stripProbe.show();
            QApplication::processEvents();
            // 顺序和 ⑨ 一样：`setCurrentItem(nullptr)` 会把选中一起清掉 ⇒ 先摘当前项、再选。
            stripProbe.setCurrentItem(nullptr);
            stripChild->setSelected(true);
            QApplication::processEvents();
            const QImage stripShot = captureWidget(stripProbe.viewport());
            const QRect stripRow = stripProbe.visualItemRect(stripChild);
            const QRect stripCard = InstanceGroupDelegate::cardRect(stripRow);
            // 底色取视口最下面那一带（那里一条行都没有）。
            const QRgb stripBg = stripShot.pixel(2, stripShot.height() - 2);
            // 哨兵①：子条目真的缩进了。不缩进的话（比如谁把 setIndentation 挪走）下面那条
            // 是恒绿的假断言 —— "左边没有那一截"当然也就"那一截是干净的"。
            check(stripRow.left() > 0,
                  "哨兵：实例（组的子条目）真的被缩进了 —— 没有这一截就谈不上「左边长了一根」",
                  QStringLiteral("子条目 %1 / 组行 %2")
                      .arg(stripRow.left())
                      .arg(stripProbe.visualItemRect(stripGroup).left()));
            // 哨兵②：选中那一行的底色带真的画出来了。
            int stripFirstInk = -1;
            const int stripMidY = stripCard.top() + stripCard.height() / 2;
            for (int x = 0; x < stripShot.width(); ++x) {
                if (stripShot.pixel(x, stripMidY) != stripBg) {
                    stripFirstInk = x;
                    break;
                }
            }
            check(stripFirstInk >= 0,
                  "哨兵：选中那一行的底色带真的画出来了（没画出来的话，下面那条是恒绿的假断言）",
                  QStringLiteral("y=%1 那一横扫过去一个有色像素都没有").arg(stripMidY));
            check(stripFirstInk == stripCard.left(),
                  "★ 底色带**从卡片的左边缘开始** —— 左边那一截分支区是树的底色，不是选中色",
                  QStringLiteral("带子左边缘 %1 / 卡片左边缘 %2")
                      .arg(stripFirstInk).arg(stripCard.left()));
            // 上面那条只看了一行像素。这一条管整块：卡片左边那一片（**整条行高** ——
            // "长了"就是长在上下那两条里）一个有色像素都不许有。
            int stripDirtyX = -1;
            int stripDirtyY = -1;
            for (int y = stripRow.top(); y <= stripRow.bottom() && stripDirtyX < 0; ++y) {
                for (int x = 0; x < stripCard.left(); ++x) {
                    if (stripShot.pixel(x, y) != stripBg) {
                        stripDirtyX = x;
                        stripDirtyY = y;
                        break;
                    }
                }
            }
            check(stripDirtyX < 0,
                  "★ 卡片左边那一片（x < 卡片左边缘、**整条行高**）一个有色像素都没有 ——"
                  "上下各冒出去的那两条就是这么来的",
                  QStringLiteral("(%1,%2) 有色；行 %3..%4，卡片 %5..%6，卡片左边 %7")
                      .arg(stripDirtyX).arg(stripDirtyY)
                      .arg(stripRow.top()).arg(stripRow.bottom())
                      .arg(stripCard.top()).arg(stripCard.bottom()).arg(stripCard.left()));
            stripProbe.hide();
        }

        // ---- ⑪ 悬停那一层的加深底色 ----
        //
        // 要求：「把所有悬停在版本/按钮上都加一个颜色加深的效果」。
        {
            // ---- ⑪a 色本身：一档只有一个出处 ----
            const QColor white(0xff, 0xff, 0xff);
            const auto rgbDist = [](const QColor &a, const QColor &b) {
                return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green())
                       + qAbs(a.blue() - b.blue());
            };
            // 7a 令牌里悬停 / 选中底是**半透明**的：屏幕上看到的是它叠在下面那一层上的结果。
            // 期望值 = 这一档按 source-over 叠在「同一点没悬停 / 没选中时」那个像素上。
            const auto over = [](const QColor &top, const QColor &under) {
                const qreal a = top.alphaF();
                return QColor(qRound(top.red() * a + under.red() * (1 - a)),
                              qRound(top.green() * a + under.green() * (1 - a)),
                              qRound(top.blue() * a + under.blue() * (1 - a)));
            };
            const auto near = [&rgbDist](const QColor &a, const QColor &b) { return rgbDist(a, b) <= 4; };
            // 认得出的预设（出厂是暮色雪山）**一档只有一个色**：不管这一行自己的底是什么，
            // 悬停都铺 `tokens().hover` ——「悬停全应用只有一种深浅」就是靠这一条。
            const QColor darkBase(0x32, 0x32, 0x32);
            const QColor presetHover = Theme::tokens().hover;
            check(Theme::rowHoverFill(white) == presetHover &&
                      Theme::rowHoverFill(darkBase) == presetHover,
                  "★ 预设自带的悬停档：底换了一种，铺的还是 `tokens().hover`（一档一个色）",
                  QStringLiteral("白底 %1 / 深底 %2 / 该是 %3")
                      .arg(Theme::rowHoverFill(white).name(),
                           Theme::rowHoverFill(darkBase).name(), presetHover.name()));
            check(presetHover != white,
                  "★ 而且它真的和白底不一样（一样的话等于「加了悬停」但屏幕上什么都没发生）",
                  presetHover.name());
            // 主题色被用户改过（不再等于预设的 `primary`）时走**混色**那一支：从这一行
            // 自己的底色出发往主题色那头混。浅色下底就是白 ⇒ 和按钮 `:hover` 逐字节相等；
            // 深色底上必须**从自己的底色出发**，不能照抄"罩在白上"的那个近白色 ——
            // 照抄的话深色主题下整行悬停会闪成一片白。
            // 这一段临时换一份自定义主题色，量完逐字还原（后面的节假设默认那身）。
            {
                const Theme::Spec savedSpec = Theme::current();
                Theme::Spec custom = Theme::presetSpec(QStringLiteral("emerald"));
                custom.accent = QColor(QStringLiteral("#336699"));
                Theme::setSpec(custom);
                Theme::apply(*qApp);
                const QColor hoverOnWhite = Theme::rowHoverFill(white);
                const QColor buttonHover = Theme::accentShade(Theme::AccentShade::Hover);
                check(hoverOnWhite == buttonHover,
                      "★ 自定义主题色下白底的行悬停色 == 按钮 `:hover` 那一档（逐字节）",
                      QStringLiteral("行 %1 / 按钮 %2").arg(hoverOnWhite.name(), buttonHover.name()));
                const QColor hoverOnDark = Theme::rowHoverFill(darkBase);
                check(rgbDist(hoverOnDark, darkBase) < rgbDist(buttonHover, darkBase),
                      "★ 深底上它是「从自己的底色加深一档」，离底色**比**按钮那个近白色更近",
                      QStringLiteral("从底色出发 %1（距离 %2）/ 近白那一档 距离 %3")
                          .arg(hoverOnDark.name())
                          .arg(rgbDist(hoverOnDark, darkBase))
                          .arg(rgbDist(buttonHover, darkBase)));
                Theme::setSpec(savedSpec);
                Theme::apply(*qApp);
            }
            check(Theme::rowHoverAccentPercent() > 0 && Theme::rowHoverAccentPercent() < 100,
                  "★「加深一档」那个数落在 0~100 之间（0 = 没效果，100 = 整行变成纯主题色）",
                  QString::number(Theme::rowHoverAccentPercent()));

            // ---- ⑪b 真上屏：实例树（两条分支 + 选中优先）----
            const int hoverRowH = MainWindowUI::instanceTreeRowHeight();
            QTreeWidget instanceProbe;
            instanceProbe.setObjectName(QStringLiteral("wtfml_hover_instance_probe"));
            instanceProbe.setHeaderHidden(true);
            // 这三句和产品里逐字一样（见 ⑩ 那段）。
            instanceProbe.setRootIsDecorated(false);
            instanceProbe.setUniformRowHeights(true);
            instanceProbe.setIndentation(14);
            instanceProbe.setItemDelegate(new InstanceGroupDelegate(&instanceProbe));
            // 这一句是那个前提 —— 下面那些是**手动**发事件，开不开都推得动它；真鼠标那一档
            // 只有这一句能救（⑪d 才量产品里那几棵树开没开）。
            instanceProbe.setMouseTracking(true);
            auto *hoverGroupRow = new QTreeWidgetItem(&instanceProbe);
            hoverGroupRow->setText(0, QStringLiteral("原版"));
            hoverGroupRow->setData(0, MainWindowUI::groupIdRole(),
                                   QStringLiteral("default-vanilla"));
            auto *hoverChildRow = new QTreeWidgetItem(hoverGroupRow);
            hoverChildRow->setText(0, QStringLiteral("1.20.1"));
            hoverChildRow->setData(0, MainWindowUI::instanceIdRole(), QStringLiteral("1.20.1"));
            instanceProbe.expandAll();
            instanceProbe.resize(420, hoverRowH * 4);
            instanceProbe.show();
            QApplication::processEvents();
            instanceProbe.setCurrentItem(nullptr);
            QApplication::processEvents();
            const QImage plainInstance = captureWidget(instanceProbe.viewport());

            const QColor hoverFill =
                Theme::rowHoverFill(instanceProbe.palette().color(QPalette::Base));
            const QRect childRowRect = instanceProbe.visualItemRect(hoverChildRow);
            const QRect childCardRect = InstanceGroupDelegate::cardRect(childRowRect);
            const QRect groupRowRect = instanceProbe.visualItemRect(hoverGroupRow);
            const QRect groupCardRect = InstanceGroupDelegate::cardRect(groupRowRect);
            // 取色那一点：卡片里靠左上角一点点 —— 那儿一定是底色。图标是从"卡片竖直正中
            // 往上半个图标"才开始的、正文也在那一条横带上，所以左上角那两个点是干净的。
            const QPoint childSpot(childCardRect.left() + 3, childCardRect.top() + 2);
            const QPoint groupSpot(groupCardRect.left() + 3, groupCardRect.top() + 2);
            const auto hoverShotAt = [&instanceProbe](const QPoint &cursor) {
                QMouseEvent move(QEvent::MouseMove, QPointF(cursor),
                                 QPointF(instanceProbe.viewport()->mapToGlobal(cursor)),
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(instanceProbe.viewport(), &move);
                QApplication::processEvents();
                return captureWidget(instanceProbe.viewport());
            };
            check(instanceProbe.viewport()->hasMouseTracking(),
                  "★ 探针自己开着 mouseTracking（真鼠标那一档只有它会填 State_MouseOver）");

            const QImage shotOnChild = hoverShotAt(childRowRect.center());
            const QColor childHover = over(hoverFill, plainInstance.pixelColor(childSpot));
            const QColor groupHover = over(hoverFill, plainInstance.pixelColor(groupSpot));
            check(near(shotOnChild.pixelColor(childSpot), childHover),
                  "★ 光标停在实例那一行：卡片上的底色正是 `Theme::rowHoverFill()` 那一档 ——"
                  " 这一条同时证明它**真的上了屏**（被样式盖掉的话这儿就不是这个色）",
                  QStringLiteral("量到 %1 / 该是 %2")
                      .arg(shotOnChild.pixelColor(childSpot).name(), childHover.name()));
            check(shotOnChild.pixelColor(groupSpot) == plainInstance.pixelColor(groupSpot),
                  "★ 而**没**悬停的组行没被染（对照组 —— 少了它，「整棵树都染上」也能过）",
                  shotOnChild.pixelColor(groupSpot).name());
            int branchStained = 0;
            for (int y = childRowRect.top(); y <= childRowRect.bottom(); ++y) {
                for (int x = 0; x < childCardRect.left(); ++x) {
                    if (shotOnChild.pixelColor(x, y) != plainInstance.pixelColor(x, y)) {
                        ++branchStained;
                    }
                }
            }
            check(branchStained == 0,
                  "★ 实例行悬停时左边那一截分支区**没有**被染 —— 悬停带和选中带是同一个形状"
                  "（染成整条行的话，悬停带会比选中带宽出去一截）",
                  QStringLiteral("分支区里被染的像素 = %1").arg(branchStained));

            const QImage shotOnGroup = hoverShotAt(groupRowRect.center());
            check(shotOnGroup.pixelColor(groupSpot) == plainInstance.pixelColor(groupSpot),
                  "★ 光标停在组行上：组头不染色（UI 7b 组头是一行小标题，不是可点的卡片）",
                  shotOnGroup.pixelColor(groupSpot).name());
            check(shotOnGroup.pixelColor(childSpot) == plainInstance.pixelColor(childSpot),
                  "★ 而组行悬停时实例那一行没被染（对照组）",
                  shotOnGroup.pixelColor(childSpot).name());

            // 选中压得住悬停：又选中又悬停的那一行，屏幕上必须是**选中色**。
            // 代理把悬停那层画在 `CE_ItemViewItem` 之前，所以这里本来就该是选中色。
            instanceProbe.setCurrentItem(hoverChildRow);
            QApplication::processEvents();
            const QImage shotOnSelected = hoverShotAt(childRowRect.center());
            const QColor selectedPixel = shotOnSelected.pixelColor(childSpot);
            // 选中那一档从 Fusion 那个蓝**换成了我们自己画的一块灰**（要求
            // 「选中的时候那个蓝色的换成深灰色」，随后「再浅一点，#cccccc」）⇒ 这里的
            // "选中色"不再是 `palette().color(Highlight)`，而是 `Theme::selectionFill()`
            // （唯一出处见 ui/Theme.hpp）。Fusion 画当前项是**竖渐变**，所以不能写
            // `== highlight` —— 仍然判"离选中色近得多、且绝不是悬停色"。
            const QColor selectionGray = Theme::selectionFill();
            const QColor highlight = instanceProbe.palette().color(QPalette::Highlight);
            const int distToSelection =
                rgbDist(selectedPixel, over(selectionGray, plainInstance.pixelColor(childSpot)));
            const int distToHover = rgbDist(selectedPixel, childHover);
            check(selectedPixel != childHover && distToSelection < distToHover,
                  "★ 又选中又悬停的那一行：屏幕上仍然是**选中色**那一档（悬停压不过选中）"
                  " —— 代理把那层悬停画在 `CE_ItemViewItem` **之前**就是为了这个",
                  QStringLiteral("量到 %1 / 离选中色 %2 / 离悬停色 %3（选中色 %4，悬停色 %5）")
                      .arg(selectedPixel.name())
                      .arg(distToSelection)
                      .arg(distToHover)
                      .arg(selectionGray.name(), hoverFill.name()));
            check(rgbDist(highlight, selectionGray) == 0,
                  "★ 调色板里的 `Highlight` 就是我们这一档（Fusion 那个蓝在应用调色板里"
                  "已经不在了）",
                  QStringLiteral("调色板 Highlight %1 / 我们这一档 %2")
                      .arg(highlight.name(), selectionGray.name()));
            // 左边那一截分支区**还是白的**（196b5cf 专门补的）：实例行选中时的带子只有
            // 卡片那一圈，不跟着那根竖条走 —— 顺手把它涂灰就不再是"卡片"了。
            // 这一条钉在**又选中又悬停**那一刻：两种底色都铺上的时候才量得出"谁没被涂"。
            const QPoint instanceStripSpot(2, childRowRect.top() + childRowRect.height() / 2);
            const QColor instanceBase = plainInstance.pixelColor(instanceStripSpot);
            check(childRowRect.left() > 2 &&
                      shotOnSelected.pixelColor(instanceStripSpot) == instanceBase,
                  "★ 而实例行左边那一截分支区**没跟着变**（还是底色，不是深灰）—— "
                  "实例行的选中带是「卡片」，版本树那一支才是「整条行」",
                  QStringLiteral("x=2 那一列量到 %1 / 该是 %2 / 卡片从 x=%3 起")
                      .arg(shotOnSelected.pixelColor(instanceStripSpot).name(),
                           instanceBase.name())
                      .arg(childRowRect.left()));
            instanceProbe.hide();

            // ---- ⑪c 真上屏：版本树那一类（"分类 → 版本号"）----
            //
            // 这一支和实例树有两处**故意不一样**（两边都按自己的"选中带"取形状）：
            // · 版本树的行是**挨着**的（没有那道空）⇒ 它的"行矩形"就是"卡片"；
            // · 它左边那一截分支区，**选中**时是被 `drawRow()` 连着一块涂的 ⇒ 悬停也得
            // 涂上去。不涂的话同一行上"选中带"和"悬停带"的左边会差一个缩进 —— 看着
            // 像是悬停没铺到头。
            const int versionRowH = MainWindowUI::versionTreeRowHeight();
            QTreeWidget versionProbe;
            // 退出设计默认：下面拿「没悬停那张」逐点当底色对照，要视口自己铺 Base。
            DesignDefaults::optOut(&versionProbe);
            versionProbe.setObjectName(QStringLiteral("wtfml_hover_version_probe"));
            versionProbe.setHeaderHidden(true);
            versionProbe.setRootIsDecorated(false);
            versionProbe.setUniformRowHeights(true);
            // 产品里版本树**没有**设过 indentation（用的是样式默认值），这里写死一个只为了
            // 让"左边那一截"有确定的宽度可比。
            versionProbe.setIndentation(20);
            versionProbe.setItemDelegate(new VersionTreeDelegate(&versionProbe));
            versionProbe.setMouseTracking(true);
            auto *channelRow = new QTreeWidgetItem(&versionProbe);
            channelRow->setText(0, QStringLiteral("正式版"));
            auto *versionRow = new QTreeWidgetItem(channelRow);
            versionRow->setText(0, QStringLiteral("1.20.1"));
            auto *disabledRow = new QTreeWidgetItem(channelRow);
            disabledRow->setText(0, QStringLiteral("1.20.2"));
            // 不可用的那一行**不染**（判据和 rowOpacity 那条一样）：淡到 0.5 的行再铺一层
            // 底色，看着就像是"点得动"。
            disabledRow->setFlags(disabledRow->flags() & ~Qt::ItemIsEnabled);
            versionProbe.expandAll();
            versionProbe.resize(420, versionRowH * 5);
            versionProbe.show();
            QApplication::processEvents();
            versionProbe.setCurrentItem(nullptr);
            QApplication::processEvents();
            const QImage plainVersion = captureWidget(versionProbe.viewport());

            const QColor versionHoverFill =
                Theme::rowHoverFill(versionProbe.palette().color(QPalette::Base));
            const QRect versionRowRect = versionProbe.visualItemRect(versionRow);
            const QRect channelRowRect = versionProbe.visualItemRect(channelRow);
            const QRect disabledRowRect = versionProbe.visualItemRect(disabledRow);
            const auto versionShotAt = [&versionProbe](const QPoint &cursor) {
                QMouseEvent move(QEvent::MouseMove, QPointF(cursor),
                                 QPointF(versionProbe.viewport()->mapToGlobal(cursor)),
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(versionProbe.viewport(), &move);
                QApplication::processEvents();
                return captureWidget(versionProbe.viewport());
            };
            // 取色那一点取行的**最上面那一条**：图标从"行竖直正中"才开始的，顶边上这一条
            // 是干净的底色（版本树这一行是两行字，正文更低）。
            check(versionRowRect.left() > 0,
                  "哨兵：版本条目（分类的子条目）真的被缩进了 —— 不缩进就谈不上「左边那一截」",
                  QStringLiteral("条目 %1 / 分类 %2")
                      .arg(versionRowRect.left())
                      .arg(channelRowRect.left()));
            const QImage shotOnVersion = versionShotAt(versionRowRect.center());
            const QPoint versionSpot(versionRowRect.left() + 2, versionRowRect.top() + 1);
            const QColor versionHover = over(versionHoverFill, plainVersion.pixelColor(versionSpot));
            check(near(shotOnVersion.pixelColor(versionSpot), versionHover),
                  "★ 版本树这一类：光标停在版本号那一行，那一行的底色就是悬停色",
                  QStringLiteral("量到 %1 / 该是 %2")
                      .arg(shotOnVersion.pixelColor(versionSpot).name(),
                           versionHover.name()));
            // 左边那一截**也要染**：选中时它是连着一块涂的，悬停不补这一截的话
            // "悬停带"和"选中带"的左边就差一个缩进。
            const QPoint versionStripSpot(2, versionRowRect.top() + 1);
            check(near(shotOnVersion.pixelColor(versionStripSpot),
                       over(versionHoverFill, plainVersion.pixelColor(versionStripSpot))),
                  "★ 而且左边那一截分支区**也**染上了 —— 版本树这一支要和它自己的选中带同形状",
                  QStringLiteral("x=2 那一列量到 %1")
                      .arg(shotOnVersion.pixelColor(versionStripSpot).name()));
            const QPoint channelSpot(channelRowRect.left() + 2, channelRowRect.top() + 1);
            check(shotOnVersion.pixelColor(channelSpot) == plainVersion.pixelColor(channelSpot),
                  "★ 而**没**悬停的分类行没被染（对照组）",
                  shotOnVersion.pixelColor(channelSpot).name());

            const QImage shotOnDisabled = versionShotAt(disabledRowRect.center());
            const QPoint disabledSpot(disabledRowRect.left() + 2, disabledRowRect.top() + 1);
            check(shotOnDisabled.pixelColor(disabledSpot) == plainVersion.pixelColor(disabledSpot),
                  "★ 不可用的那一行不染 —— 整行已经淡到 0.5 了，再铺一层底色像是「点得动」",
                  shotOnDisabled.pixelColor(disabledSpot).name());
            versionProbe.hide();

            // ---- ⑪d 产品里挂着这两个代理的表，真的都开着 mouseTracking ----
            //
            // 这是**真鼠标**那一档唯一的守卫：上面所有断言都是手动发事件的，开不开
            // mouseTracking 都量得动。而用户用真鼠标 —— 少一句 setMouseTracking(true)，
            // 屏幕上的症状是"悬停没反应"，不报错、上面每条断言还照样绿的。
            // （这两个视图都必须显式开这一位。）
            const auto viewTracksMouse = [](QAbstractScrollArea *view) {
                return view != nullptr && view->viewport()->hasMouseTracking();
            };
            check(viewTracksMouse(ui.getInstanceList()),
                  "★ 产品·实例列表开着 mouseTracking（悬停底色靠视图填这一位）");
            check(viewTracksMouse(ui.getDownloadCategoryList()),
                  "★ 产品·下载页左栏那份分类列表开着 mouseTracking");
            check(viewTracksMouse(ui.getVersionTree()),
                  "★ 产品·版本树开着 mouseTracking");
            check(viewTracksMouse(ui.getLoaderTree()),
                  "★ 产品·加载器那棵树开着 mouseTracking");
            // 第 0 个「游戏」那一页**没有资源表**（`getDownloadResourceTree("game")`
            // 回 nullptr —— 它有它自己那棵树），所以这里按"四个资源页"来数，跳过它。
            const QStringList allCategoryKeys = MainWindowUI::downloadCategoryKeys();
            const int gameIndex = MainWindowUI::downloadCategoryGameIndex();
            int resourceTreesTracked = 0;
            int resourceTreesExpected = 0;
            for (int i = 0; i < allCategoryKeys.size(); ++i) {
                // 「游戏」和「服务端」都没有资源表（前者有版本树，后者只有一句说明 +
                // 一颗「新建服务端…」），跳过它们。
                if (i == gameIndex || i == MainWindowUI::downloadCategoryServerIndex()) {
                    continue;
                }
                ++resourceTreesExpected;
                if (viewTracksMouse(ui.getDownloadResourceTree(allCategoryKeys.at(i)))) {
                    ++resourceTreesTracked;
                }
            }
            check(resourceTreesTracked == resourceTreesExpected && resourceTreesExpected > 0,
                  "★ 产品·下载页那几张资源表（整合包/模组/数据包/资源包/光影）每一张都开着 "
                  "mouseTracking",
                  QStringLiteral("开着的有 %1 / 一共 %2")
                      .arg(resourceTreesTracked).arg(resourceTreesExpected));

            // ---- ⑪e 挂在 VersionTreeDelegate 上的另外两张表 ----
            // 左边那两张列表（实例设置左栏、资源详情页）不在 `ui` 里，但它们在**产品**里
            // 就是同一句 —— 这里现搭一份量一次，免得漏掉。
            QListWidget hoverListProbe;
            hoverListProbe.setItemDelegate(new VersionTreeDelegate(&hoverListProbe));
            hoverListProbe.setMouseTracking(true);
            hoverListProbe.addItem(QStringLiteral("常规"));
            hoverListProbe.resize(240, versionRowH * 3);
            hoverListProbe.show();
            QApplication::processEvents();
            check(viewTracksMouse(&hoverListProbe),
                  "★ 探针那份列表也开着（产品里实例设置左栏 / 资源详情页那两张是同一句）");
            hoverListProbe.hide();

            // ===== ⑫ 选中那一档：Fusion 那个蓝 → 我们自己画的一块灰 =====
            //
            // 要求：「选中的时候那个蓝色的换成深灰色」，紧接着
            // 「再浅一点，#cccccc」。
            //
            // 为什么不是改调色板：`QPalette::Highlight` 只要被 `setPalette()` 动一下，
            // `Theme::apply()` 里那个 `touchesPalette` 就置位 ⇒ 默认那份「跟着系统」的预设
            // 会**钉死**调色板，系统深浅主题从此不跟了（W1 那条断言钉的就是这个）。
            // ⇒ 这块灰是**我们画**的：唯一出处 `ui/Theme.cpp` 的 `selectionFill()` /
            // `selectionInk()` / `selectionSubInk()` / `selectionPalette()`，落点是
            // `WtfmlProxyStyle`（= `Theme::createStyle()`）里那三处原语覆写：
            // `PE_PanelItemViewItem` / `PE_PanelItemViewRow` / `CE_ItemViewItem`。
            // 手画正文的三个代理（VersionTreeDelegate / InstanceGroupDelegate /
            // GroupListDelegate）读的是 `selectionPalette()` —— 它们抄不到样式那一半。

            // ---- ⑫a 那一档底：预设自带一档；自定义主题色时由**这一行自己的底色**混出来 ----
            const QColor selectionFill = Theme::selectionFill();
            check(selectionFill == Theme::selectionFill(QColor(Qt::white)) &&
                      selectionFill == Theme::selectionFill(QColor(0x32, 0x32, 0x32)) &&
                      selectionFill == Theme::tokens().selected,
                  "★ 预设自带的选中档：底换了一种，铺的还是 `tokens().selected`（一档一个色）",
                  selectionFill.name());
            const QColor selectionOnCard = over(selectionFill, Theme::tokens().card);
            check(selectionOnCard.lightness() > 200 && selectionOnCard.hsvSaturation() < 40 &&
                      selectionOnCard.green() > selectionOnCard.red(),
                  "★ 那一档底是**低饱和的浅主题色**：够浅（明度 > 200）、够淡（饱和度 < 40）、"
                  "方向对（这一支主题色是绿的）—— 既不是灰、也不是主题色本色",
                  QStringLiteral("叠在卡片上 %1（明度 %2 / 饱和 %3）")
                      .arg(selectionOnCard.name())
                      .arg(selectionOnCard.lightness())
                      .arg(selectionOnCard.hsvSaturation()));
            const QColor selectionDarkBase(0x32, 0x32, 0x32);
            // 自定义主题色那一支（主题色不再等于预设的 `primary`）：底由**这一行自己的
            // 底色**混一档主题色，不是写死的色号 —— 深色主题的底混出来仍然是「底色上浮
            // 一点、方向跟着这一支主题色」，不会闪成一片近白。量完逐字还原。
            {
                const Theme::Spec savedSpec = Theme::current();
                Theme::Spec custom = Theme::presetSpec(QStringLiteral("emerald"));
                custom.accent = QColor(QStringLiteral("#336699"));
                Theme::setSpec(custom);
                Theme::apply(*qApp);
                const QColor onWhite = Theme::selectionFill(QColor(Qt::white));
                const QColor onDarkBase = Theme::selectionFill(selectionDarkBase);
                check(onWhite != onDarkBase &&
                          onDarkBase.lightness() < onWhite.lightness() &&
                          onDarkBase.lightness() > selectionDarkBase.lightness() &&
                          onDarkBase.blue() > onDarkBase.red(),
                      "★ 自定义主题色下底**跟着这一行自己的底色走**：深底混出来是「底色上浮"
                      "一点、方向跟着这一支主题色」，不会闪成一片近白",
                      QStringLiteral("白底混出 %1 / 深底混出 %2 / 深底本色 %3")
                          .arg(onWhite.name(), onDarkBase.name(), selectionDarkBase.name()));
                Theme::setSpec(savedSpec);
                Theme::apply(*qApp);
            }
            const QColor appHighlight =
                QGuiApplication::palette().color(QPalette::Active, QPalette::Highlight);
            check(selectionFill.rgb() == appHighlight.rgb(),
                  "★ 调色板里的 `Highlight` 也换成了**我们这一档**（Fusion 那个蓝在整个应用上"
                  "已经不在了）—— 凡是从调色板取选中色的地方都不会再长回蓝色",
                  QStringLiteral("我们这一档 %1 / 调色板 Highlight %2")
                      .arg(selectionFill.name(), appHighlight.name()));

            // ---- ⑫b 墨：浅底上必须是**暗**字，深底上必须是**亮**字 ----
            //
            // 方向是这一节的要害：亮墨压浅底 = 等于没字，反过来暗墨压深底也一样。
            // 所以这里不只量"差得够远"，还钉住"底是亮的、墨是暗的"这两条方向；底
            // **跟着这一行自己的底色走**的那一支上，方向**还得会翻回来**（深底换亮墨）。
            const QColor selectionInk = Theme::selectionInk();
            const QColor selectionSubInk = Theme::selectionSubInk();
            check(selectionOnCard.lightness() > 128 && selectionInk.lightness() < 128,
                  "★ 底是**浅**的、墨是**暗**的（方向反了 = 白字压浅底 = 等于没字）",
                  QStringLiteral("底明度 %1 / 墨明度 %2")
                      .arg(selectionOnCard.lightness())
                      .arg(selectionInk.lightness()));
            check(rgbDist(selectionInk, selectionOnCard) > 300,
                  "★ 选中那一档的墨和底（叠在卡片上、屏幕上那个色）拉开得够远",
                  QStringLiteral("墨 %1 / 底 %2 / 差 %3")
                      .arg(selectionInk.name(), selectionOnCard.name())
                      .arg(rgbDist(selectionInk, selectionOnCard)));
            check(rgbDist(selectionSubInk, selectionOnCard) > 240,
                  "★ 次要那一档墨（次要正文 / 行里那些小图标）也压得住这一块底 —— 不够的话"
                  "图标看着就是「没了」",
                  QStringLiteral("次要墨 %1 / 差 %2")
                      .arg(selectionSubInk.name())
                      .arg(rgbDist(selectionSubInk, selectionOnCard)));
            // 「底一暗、墨翻亮」这一条只在底**跟着这一行走**的那一支上成立（自定义主题色 /
            // 跟着系统）。预设自带一档的时候深浅由预设定，换底不换墨 —— 也就不用翻。
            {
                const Theme::Spec savedSpec = Theme::current();
                Theme::Spec custom = Theme::presetSpec(QStringLiteral("emerald"));
                custom.accent = QColor(QStringLiteral("#336699"));
                Theme::setSpec(custom);
                Theme::apply(*qApp);
                const QColor inkOnDark = Theme::selectionInk(selectionDarkBase);
                const QColor subInkOnDark = Theme::selectionSubInk(selectionDarkBase);
                check(inkOnDark.lightness() > 128 && subInkOnDark.lightness() > 128 &&
                          inkOnDark.lightness() > subInkOnDark.lightness() &&
                          rgbDist(inkOnDark, Theme::selectionFill(selectionDarkBase)) > 300,
                      "★ 底一暗下去，墨**自己翻回亮的那一支**（写死一支暗墨的话深底上"
                      "整行就是「没有字」）",
                      QStringLiteral("深底上的墨 %1（明度 %2）/ 次要墨 %3（明度 %4）")
                          .arg(inkOnDark.name())
                          .arg(inkOnDark.lightness())
                          .arg(subInkOnDark.name())
                          .arg(subInkOnDark.lightness()));
                Theme::setSpec(savedSpec);
                Theme::apply(*qApp);
            }
            const QPalette unselectedPalette = QGuiApplication::palette();
            check(Theme::selectionPalette(unselectedPalette, /*selected=*/false) == unselectedPalette,
                  "★ 没选中时 `selectionPalette()` **一个字都不动**（是不是选中只看调用方传进来"
                  "那一位 —— 顺手改一份没选中的调色板会让整棵树都不是系统色）");
            const QPalette selectedPalette = Theme::selectionPalette(unselectedPalette, true);
            check(selectedPalette.color(QPalette::Highlight).rgb() ==
                          Theme::selectionFill().rgb() &&
                      selectedPalette.color(QPalette::HighlightedText).rgb() == selectionInk.rgb() &&
                      selectedPalette.color(QPalette::WindowText).rgb() == selectionInk.rgb() &&
                      selectedPalette.color(QPalette::PlaceholderText).rgb() == selectionSubInk.rgb(),
                  "★ 选中那一份把**底和墨一起**换了（三个手画正文的代理读的就是这一份；"
                  "少换一档的后果是同一行上一暗一白）",
                  QStringLiteral("底 %1 / 正文墨 %2 / 次要墨 %3")
                      .arg(selectedPalette.color(QPalette::Highlight).name(),
                           selectedPalette.color(QPalette::WindowText).name(),
                           selectedPalette.color(QPalette::PlaceholderText).name()));

            // ---- ⑫c 真上屏：版本树那一支（"整条行连着左边分支区"都归选中色）----
            QTreeWidget selectProbe;
            selectProbe.setObjectName(QStringLiteral("wtfml_selection_version_probe"));
            selectProbe.setHeaderHidden(true);
            selectProbe.setRootIsDecorated(false);
            selectProbe.setUniformRowHeights(true);
            selectProbe.setIndentation(20);
            selectProbe.setItemDelegate(new VersionTreeDelegate(&selectProbe));
            auto *selectChannel = new QTreeWidgetItem(&selectProbe);
            selectChannel->setText(0, QStringLiteral("正式版"));
            auto *selectRow = new QTreeWidgetItem(selectChannel);
            selectRow->setText(0, QStringLiteral("1.20.1"));
            selectProbe.expandAll();
            selectProbe.resize(420, versionRowH * 4);
            selectProbe.show();
            QApplication::processEvents();
            selectProbe.setCurrentItem(nullptr);
            QApplication::processEvents();
            const QImage plainSelect = captureWidget(selectProbe.viewport());
            selectProbe.setCurrentItem(selectRow);
            QApplication::processEvents();
            const QRect selectRowRect = selectProbe.visualItemRect(selectRow);
            const QImage selectShot = captureWidget(selectProbe.viewport());
            const QColor probeBase = selectProbe.palette().color(QPalette::Base);
            const QColor probeHighlight = selectProbe.palette().color(QPalette::Highlight);
            const QColor probeSelection = over(Theme::selectionFill(),
                                               plainSelect.pixelColor(selectRowRect.left() + 2,
                                                                      selectRowRect.center().y()));
            // **判据扫一片取多数，别拿单点逐字节比**：这一行是"当前项"，样式会在条目边界上
            // 描焦点框、边上还有几个抗锯齿的像素 —— 贴着边界取样量到的是混色。
            // 取 `top()+1` 会量到 `#4c4c4c`（该有 `#4a4a4a`）、取竖直正中
            // 会量到 `#cdcdcd`（该有 `#cccccc`）—— 每次都只差一两个数位，判"接近"就混过去了，
            // 判"相等"当场红。所以扫**一整片**（左边那圈留白里、够不着正文和图标），数
            // "有多少个像素就是我们这一档、有几个还是那个蓝"。
            check(selectRowRect.left() > 2, "哨兵：这一行真的被缩进了（不缩进就谈不上「左边那一截」）",
                  QStringLiteral("左边 = %1").arg(selectRowRect.left()));
            // 为什么不是"某一点逐字节等于我们那一档"：**当前项**那一行的底是 Fusion 自己
            // 画的**竖渐变**（把这一列逐行打印出来看：上沿一条 `#b8b8b8`、
            // 紧跟着 `#d2d2d2`、往下平滑收到下沿那条 `#b4b4b4`），我们换掉的只是它用的那份
            // 调色板 —— 整条渐变**就是**我们这一档底调制出来的，但只有中间几行逐字节等于
            // 它。所以判据是"这一列**平均**下来就是这一档、最偏的一个像素也在同一族里" +
            // "那个蓝一个像素都不剩"。**平**的地方有：分支区那一截（下面那条逐字节判据量
            // 得到）、以及不在焦点上的那些选中行（走 `PE_PanelItemViewItem`，是我们自己
            // `fillRect` 的）。
            int bodyPixels = 0;
            int bodyWorst = 0;
            int bodyTint = 0;
            int sumR = 0;
            int sumG = 0;
            int sumB = 0;
            QColor bodyWorstPixel;
            for (int y = selectRowRect.top(); y <= selectRowRect.bottom(); ++y) {
                const QColor px = selectShot.pixelColor(selectRowRect.left() + 2, y);
                ++bodyPixels;
                sumR += px.red();
                sumG += px.green();
                sumB += px.blue();
                // 这一档底是**带方向**的（这一支主题色是绿的）：渐变再怎么调，绿的通道
                // 都得最高。写死"逐字节等于某一档"是不行的（当前项那层渐变），而"
                // 整列平均下来对得上"又太松（从绿退回灰也能凑出同一个平均）—— 这两条
                // 合起来才是"屏幕上真的铺的是我们这一档"。允许差一个数位（四舍五入）。
                if (px.green() + 1 < px.red() || px.green() + 1 < px.blue()) {
                    ++bodyTint;
                }
                const int d = rgbDist(px, probeSelection);
                if (d > bodyWorst) {
                    bodyWorst = d;
                    bodyWorstPixel = px;
                }
            }
            // 平均色要**逐通道**算，不能拿"每像素的通道平均值"再合成一个灰 —— 那一手在
            // 底色是灰的时候看不出区别，换成带方向的这一档就成了"拿灰去比绿"（离目标
            // 22 全是这么来的，屏幕上其实只有 11）。
            const QColor bodyAverage(bodyPixels > 0 ? sumR / bodyPixels : 0,
                                     bodyPixels > 0 ? sumG / bodyPixels : 0,
                                     bodyPixels > 0 ? sumB / bodyPixels : 0);
            check(bodyPixels > 0 && bodyTint == 0 &&
                      rgbDist(bodyAverage, probeSelection) <= 12 && bodyWorst <= 120 &&
                      probeHighlight.rgb() == Theme::selectionFill().rgb(),
                  "★ 选中一行：屏幕上铺的就是我们这一档（这一列**平均**下来正落在它上面、"
                  "**每一个**像素都还带着这一支主题色的方向、最偏的那个也还在同一族里，"
                  "调色板里的 `Highlight` 也是这一档）",
                  QStringLiteral("这一列 %1 个像素：平均 %2（离目标 %3）/ 最差 %4 那个像素是 %5"
                                 " / 方向不对的 %6 个 / 调色板 Highlight %7 / 底色 %8")
                      .arg(bodyPixels)
                      .arg(bodyAverage.name())
                      .arg(rgbDist(bodyAverage, probeSelection))
                      .arg(bodyWorst)
                      .arg(bodyWorstPixel.name())
                      .arg(bodyTint)
                      .arg(probeHighlight.name(), probeBase.name()));
            // tokens-r2 §6：选中 = 淡底 + 强调字，左缘不再有 3 px 指示条 ⇒ 原来那根条
            // 所在的那一列（x = 4）量到的也该是同一档选中底。
            const QPoint railSpot(4, selectRowRect.center().y());
            const QColor railPixel = selectShot.pixelColor(railSpot);
            check(near(railPixel, probeSelection),
                  "★ 选中那一行最左边没有指示条，铺的是同一档选中淡底",
                  QStringLiteral("x=4 量到 %1（该是选中底 %2）")
                      .arg(railPixel.name(), probeSelection.name()));
            const QPoint selectStripSpot(selectRowRect.left() - 2, selectRowRect.top() + 1);
            const QColor selectStrip = selectShot.pixelColor(selectStripSpot);
            check(near(selectStrip, probeSelection),
                  "★ 指示条右边那一截（还是分支区）铺的**也是**这一档底 —— 版本树这一支的选中带"
                  "是「整条行」，左边那一截是 `QTreeView::drawRow()` 先涂的，代理排在它后面、"
                  "得自己再补一遍",
                  QStringLiteral("x=%1 那一列量到 %2").arg(selectStripSpot.x()).arg(selectStrip.name()));
            selectProbe.hide();

            // ---- ⑫d 一张**裸**表（没挂我们那三个代理）：只看"不再是那个蓝" ----
            QListWidget plainProbe;
            plainProbe.setObjectName(QStringLiteral("wtfml_selection_plain_probe"));
            plainProbe.addItem(QStringLiteral("常规"));
            plainProbe.resize(240, 40);
            plainProbe.show();
            QApplication::processEvents();
            const QImage plainUnselected = captureWidget(plainProbe.viewport());
            plainProbe.setCurrentRow(0);
            QApplication::processEvents();
            const QRect plainRect = plainProbe.visualItemRect(plainProbe.item(0));
            const QImage plainShot = captureWidget(plainProbe.viewport());
            // 裸表（没挂我们那三个代理）铺的是 Fusion 自己画的竖渐变 —— 取正中那一格，
            // 它应当正落在**调色板里的选中色**上（那一份已经是我们这一档，Fusion 那个蓝
            // 在应用上不存在了）。
            // 取这一行右端（字是左对齐的，右端是干净的底）。
            const QPoint plainSpot(plainRect.right() - 4, plainRect.center().y());
            const QColor plainInkPixel = plainShot.pixelColor(plainSpot);
            const QColor plainExpected =
                over(probeHighlight, plainUnselected.pixelColor(plainSpot));
            check(rgbDist(plainInkPixel, plainExpected) <= 60,
                  "★ 裸列表里选中一行铺的就是调色板里那一档（它已经是我们这一档，"
                  "Fusion 那个蓝在应用上不存在了）",
                  QStringLiteral("量到 %1（调色板选中色叠上底 = %2，差 %3）")
                      .arg(plainInkPixel.name(), plainExpected.name())
                      .arg(rgbDist(plainInkPixel, plainExpected)));
            plainProbe.hide();

            // ---- ⑫e 那条闸：`Highlight` 被换过的表**不许**被接管 ----
            //
            // 下拉框弹出层里的选中色是**主题色**（用户点名，走 QSS
            // `QComboBox QAbstractItemView { selection-background-color: … }`）。那条 QSS 是
            // 逐控件的，我们这份样式看不到是哪张表在画，只能看 `option.palette`：这一份的
            // `Highlight` 跟应用调色板里那个不一样 ⇒ 不是"我们自己的选中带" ⇒ 原样交回去。
            // 少了这条闸，下拉框里选中的那一项会跟着变成我们那块灰，用户点名要的主题色就没了。
            QImage takeoverCanvas(24, 12, QImage::Format_ARGB32_Premultiplied);
            const auto renderItemPanel = [&takeoverCanvas](const QPalette &palette) {
                takeoverCanvas.fill(Qt::transparent);
                QPainter p(&takeoverCanvas);
                QStyleOptionViewItem opt;
                opt.rect = QRect(0, 0, takeoverCanvas.width(), takeoverCanvas.height());
                opt.state = QStyle::State_Enabled | QStyle::State_Selected | QStyle::State_Active;
                opt.palette = palette;
                opt.showDecorationSelected = true;
                QApplication::style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, &p, nullptr);
                p.end();
                return takeoverCanvas.pixelColor(takeoverCanvas.width() / 2,
                                                 takeoverCanvas.height() / 2);
            };
            const QColor controlledPixel = renderItemPanel(unselectedPalette);
            // 画布是透明的，半透明那一档存成预乘再取回来会差一两个数位。
            check(rgbDist(controlledPixel, Theme::selectionFill()) <= 8,
                  "★ 对照组：`Highlight` 就是应用调色板那一份 ⇒ **上面那条路我们接管**，"
                  "画出来的是我们那一档底",
                  QStringLiteral("量到 %1 / 该是 %2")
                      .arg(controlledPixel.name(), Theme::selectionFill().name()));
            QPalette accentPalette = unselectedPalette;
            accentPalette.setColor(QPalette::Active, QPalette::Highlight,
                                   Theme::accentShade(Theme::AccentShade::Pure));
            const QColor accentPixel = renderItemPanel(accentPalette);
            const QColor accentPure = Theme::accentShade(Theme::AccentShade::Pure);
            check(accentPixel.rgb() != Theme::selectionFill().rgb() &&
                      rgbDist(accentPixel, accentPure) <
                          rgbDist(accentPixel, Theme::selectionFill()),
                  "★ 而 `Highlight` 换成主题色的那一份**没被接管**（就是下拉框弹出层那条路）",
                  QStringLiteral("量到 %1 / 主题色 %2 / 我们那一档底 %3")
                      .arg(accentPixel.name(), accentPure.name(),
                           Theme::selectionFill().name()));
        }
    }
 return 0;
}



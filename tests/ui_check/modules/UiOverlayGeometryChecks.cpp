#include "Support.hpp"
int UiOverlayGeometryChecks(QMainWindow &window, MainWindowUI &ui, QTabWidget * &tabs, QFrame * &bar) {
    if (OverlayHost *overlay = ui.overlay()) {
        // 内容区 = 标签页**当前那一页**占的那块。期望值**不复用 contentRect()** —— 拿被测
        // 代码自己的算法去算期望，等于"它算错也算对"（这个仓库已经在"两端读同一个常数"
        // 上吃过一次亏）。这里走 mapTo()，是另一条路。
        const QPoint pageTopLeft = tabs->currentWidget()->mapTo(&window, QPoint(0, 0));
        const QSize pageSize = tabs->currentWidget()->size();
        // 顶上**不许被盖住的东西**： 起是那条自己画的标题栏（四个功能在它上面），
        // **不是标签栏** —— 标签栏已经藏起来了（见 Z11 那一节）。所以这里量的是条。
        TitleBar *strip = ui.titleBar();
        const QRect stripInWindow(strip != nullptr ? strip->mapTo(&window, QPoint(0, 0)) : QPoint(),
                                  strip != nullptr ? strip->size() : QSize());
        const QRect barInWindow(bar->mapTo(&window, QPoint(0, 0)), bar->size());
        // 开覆盖层**之前**先抓一张：下面量"四周变暗"要拿同一格的像素比。
        const QImage plainPage = captureWidget(tabs->currentWidget());

        QWidget *probe = new QWidget;  // addWidget 会认领它，所以不给父控件也没事
        probe->setObjectName(QStringLiteral("overlayProbePage"));
        overlay->openPage(QStringLiteral("实例设置"))->layout()->addWidget(probe);
        QApplication::processEvents();

        check(overlay->isOpen() && overlay->pageOpen(), "打开之后是「整页」那一档");
        const QPoint overlayTopLeft = overlay->mapTo(&window, QPoint(0, 0));
        check(overlayTopLeft == pageTopLeft && overlay->size() == pageSize,
              "盖住的正好是内容区（和标签页当前页是同一块）",
              QStringLiteral("覆盖层 %1,%2 %3x%4 ／ 内容区 %5,%6 %7x%8")
                  .arg(overlayTopLeft.x())
                  .arg(overlayTopLeft.y())
                  .arg(overlay->width())
                  .arg(overlay->height())
                  .arg(pageTopLeft.x())
                  .arg(pageTopLeft.y())
                  .arg(pageSize.width())
                  .arg(pageSize.height()));
        // 顶上那一条和底栏必须露着（要求「底下那一条不用覆盖，**顶上四个选项卡也不覆盖**」
        // —— 那四个选项卡 起就在这条上，所以"露着"这一条现在由条来满足）。
        // 壳换成左导航轨后：功能入口和原底栏都在轨上，覆盖层得整个落在轨右边、窗口按钮条下面。
        check(stripInWindow.isValid() && stripInWindow.right() < overlayTopLeft.x() &&
                  barInWindow.right() < overlayTopLeft.x(),
              "没盖住左边的导航轨（功能入口和轨底任务卡都在那上面）",
              QStringLiteral("轨右 %1 ／ 轨底卡右 %2 ／ 覆盖层左 %3")
                  .arg(stripInWindow.right()).arg(barInWindow.right()).arg(overlayTopLeft.x()));
        check(overlayTopLeft.y() >= TitleBar::barHeight(), "没盖住顶上的窗口按钮条",
              QStringLiteral("覆盖层顶 %1").arg(overlayTopLeft.y()));

        // 返回箭头：用户点名的「左上角有返回箭头」。
        auto *backButton = overlay->findChild<QPushButton *>(OverlayHost::overlayBackButtonName());
        check(backButton != nullptr && backButton->isVisible(), "左上角那颗返回箭头在",
              backButton != nullptr ? backButton->objectName() : QStringLiteral("<null>"));
        // 7c 起是设计稿面包屑：箭头是 chevL 线条图标，字是「返回」。
        check(backButton != nullptr && !backButton->icon().isNull() &&
                  backButton->text() == OverlayHost::backButtonText(),
              "它带一个向左的箭头图标 + 「返回」",
              backButton != nullptr ? backButton->text() : QString());
        if (backButton != nullptr) {
            const QPoint at = backButton->mapTo(overlay, QPoint(0, 0));
            check(at.x() < overlay->width() / 3 && at.y() < OverlayHost::pageHeaderHeight(),
                  "它在左上角（贴着左边、纵向在页眉那一行里）",
                  QStringLiteral("%1,%2").arg(at.x()).arg(at.y()));
        }
        QLabel *overlayTitle = overlay->findChild<QLabel *>(OverlayHost::overlayTitleLabelName());
        check(overlayTitle != nullptr && overlayTitle->text() == QStringLiteral("实例设置"),
              "页眉上写着这一页是什么",
              overlayTitle != nullptr ? overlayTitle->text() : QStringLiteral("<null>"));

        // 被盖住的那一页要**点不着**。覆盖层能吃掉鼠标，但**键盘吃不下** —— Tab 照样能把
        // 焦点挪到下面那些控件上，接着按空格就等于在用户看不见的地方点了一下。
        check(!tabs->currentWidget()->isEnabled(), "被盖住的那一页被按住了");

        // 返回箭头点一下 = 发一次 backPressed。offscreen 下 activeModalWidget() 恒空，
        // 所以"那一发到没到"只能数信号。
        int backCount = 0;
        auto backConnection = QObject::connect(overlay, &OverlayHost::backPressed, overlay,
                                               [&backCount] { ++backCount; });
        backButton->click();
        QApplication::processEvents();
        check(backCount == 1, "点一下返回箭头 = 发一次 backPressed", QString::number(backCount));
        QObject::disconnect(backConnection);

        overlay->closeOverlay();
        QApplication::processEvents();
        check(!overlay->isOpen() && !overlay->isVisible(), "收掉之后覆盖层不见了");
        check(tabs->currentWidget()->isEnabled(), "收掉之后下面那一页又是活的");

        // 整页那一档外面得套一层滚动区。设置页是按它自己的尺寸设计的（实例设置要
        // 600x480），而内容区多大是主窗口说了算 —— 窗口一小，页脚那排「取消 / 确定」
        // 就被切在下面，用户怎么点都点不着，那正是"这一页根本没法用"。
        QWidget *tallArea = overlay->openPage(QStringLiteral("很高的页面"));
        probe->setFixedHeight(4000);
        tallArea->layout()->addWidget(probe);
        QApplication::processEvents();
        auto *pageScroll = overlay->findChild<QScrollArea *>(OverlayHost::overlayPageScrollName());
        check(pageScroll != nullptr && pageScroll->widget() == tallArea,
              "整页那一档里的东西是装在滚动区里的",
              pageScroll != nullptr ? pageScroll->objectName() : QStringLiteral("<null>"));
        if (pageScroll != nullptr && pageScroll->widget() == tallArea) {
            auto *vbar = pageScroll->verticalScrollBar();
            check(vbar->maximum() > 0 && tallArea->height() > pageScroll->viewport()->height(),
                  "内容比内容区高的时候，容器自己长高（不是把下面切掉）",
                  QStringLiteral("容器高 %1 ／ 视口高 %2 ／ 可滚 %3")
                      .arg(tallArea->height())
                      .arg(pageScroll->viewport()->height())
                      .arg(vbar->maximum()));
            const int beforeTop = tallArea->mapTo(pageScroll->viewport(), QPoint(0, 0)).y();
            vbar->setValue(vbar->maximum());
            QApplication::processEvents();
            const int afterTop = tallArea->mapTo(pageScroll->viewport(), QPoint(0, 0)).y();
            check(afterTop < beforeTop, "滚到底之后内容真的往上走了（最下面那排能滚到眼前）",
                  QStringLiteral("%1 → %2").arg(beforeTop).arg(afterTop));
        }
        overlay->closeOverlay();
        QApplication::processEvents();
        delete probe;

        // ---------- 提示型：中间一块 + 四周变暗 ----------
        auto cardOf = [overlay]() -> QWidget * {
            return overlay->findChild<QWidget *>(OverlayHost::overlayPromptHostName());
        };
        // ① 短句那条。直接开（不卡调用方）—— 要看的那些东西得在它开着的时候量。
        InlineMessageBox shortBox(MessageKind::Information, QStringLiteral("下载完成"),
                                  QStringLiteral("装好了。"), QStringList(), 0, -1);
        overlay->openPrompt(false)->layout()->addWidget(&shortBox);
        QApplication::processEvents();

        QWidget *card = cardOf();
        check(overlay->isOpen() && overlay->promptOpen(), "第二条开出来是「提示」那一档");
        check(card != nullptr && card->isVisible(), "中间那块框在");
        const int shortWidth = card != nullptr ? card->width() : -1;
        const int shortHeight = card != nullptr ? card->height() : -1;
        check(shortWidth == OverlayHost::promptWidth(), "框的宽度就是那个唯一出处",
              QString::number(shortWidth));
        if (card != nullptr) {
            check(qAbs(card->x() - (overlay->width() - card->width()) / 2) <= 1 &&
                      qAbs(card->y() - (overlay->height() - card->height()) / 2) <= 1,
                  "框摆在内容区中间",
                  QStringLiteral("%1,%2").arg(card->x()).arg(card->y()));
        }
        // 四周变暗：同一格像素，开了提示之后必须**更深**（"看得见下面、但被压暗了"）。
        const QImage scrimmed = captureWidget(overlay);
        if (scrimmed.size() == plainPage.size()) {
            const QColor before = plainPage.pixelColor(6, 6);
            const QColor after = scrimmed.pixelColor(6, 6);
            check(after.lightness() < before.lightness(), "四周变暗了（同一格比没有提示时更深）",
                  QStringLiteral("%1 → %2").arg(before.name(), after.name()));
        } else {
            check(false, "四周变暗那一条量得出来（抓图尺寸要和内容区一致）",
                  QStringLiteral("%1x%2 vs %3x%4")
                      .arg(scrimmed.width())
                      .arg(scrimmed.height())
                      .arg(plainPage.width())
                      .arg(plainPage.height()));
        }

        // 点框外面：默认**不关**。"要不要停游戏"这种问话误点一下外面就当没问过，
        // 是个危险的默认值。
        int dismissCount = 0;
        auto dismissConnection = QObject::connect(overlay, &OverlayHost::promptDismissed, overlay,
                                                  [&dismissCount] { ++dismissCount; });
        // 先确认这一格真的在框外面（不然下面那两条量的是别的东西）。
        check(card != nullptr && !card->geometry().contains(QPoint(4, 4)),
              "拿来当「外面」的那一格确实在框外面（不然下面两条量的是别的东西）");
        QMouseEvent outsidePress(QEvent::MouseButtonPress, QPointF(4, 4), QPointF(4, 4),
                                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(overlay, &outsidePress);
        QApplication::processEvents();
        check(dismissCount == 0 && overlay->isOpen(),
              "默认点框外面不关它（问话误点一下外面就当没问过是危险的）",
              QStringLiteral("发了 %1 次").arg(dismissCount));

        // Esc = 离开，**不问** dismissOnOutsideClick —— 按 Esc 是明确表态。
        QKeyEvent escapePress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(overlay, &escapePress);
        QApplication::processEvents();
        check(dismissCount == 1, "按 Esc 算关掉（发一次 promptDismissed）",
              QStringLiteral("发了 %1 次").arg(dismissCount));
        QObject::disconnect(dismissConnection);
        overlay->closeOverlay();
        QApplication::processEvents();

        // ② 换成一句长的：**宽度一点不能变**（用户要的「不会让界面出现尺寸不一样的弹窗」），
        // 高矮可以随字数走。
        InlineMessageBox longBox(
            MessageKind::Warning, QStringLiteral("下载失败"),
            QStringLiteral("从 cdn.modrinth.com 拉 https://cdn.modrinth.com/data/abcdefgh/"
                           "versions/12345678/SomeVeryLongFileName-1.20.1-forge-47.2.0.jar 的"
                           "时候，对方在传了 43% 之后把连接掐断了。重试三次都是同一个位置，"
                           "看起来是那条线路自己的问题，不是这一版的文件坏了。"),
            QStringList(), 0, -1);
        overlay->openPrompt(false)->layout()->addWidget(&longBox);
        QApplication::processEvents();
        QWidget *longCard = cardOf();
        check(longCard != nullptr && longCard->width() == shortWidth,
              "换成一句长的，宽度**一点没变**",
              QStringLiteral("%1 → %2")
                  .arg(shortWidth)
                  .arg(longCard != nullptr ? longCard->width() : -1));
        check(longCard != nullptr && longCard->height() > shortHeight,
              "长的那句确实更高（宽度钉住、高矮随字数）",
              QStringLiteral("%1 → %2")
                  .arg(shortHeight)
                  .arg(longCard != nullptr ? longCard->height() : -1));
        overlay->closeOverlay();
        QApplication::processEvents();

        // ③ `execPrompt` 是**同步**的（和 QMessageBox::exec() 一个形状）—— 九十来处调用点
        // 就是照这个形状换过去的。等真框开出来时用 QTimer 点 —— 在自己的 processEvents
        // 里点 = 死锁（这条坑 mw_check 里踩过）。
        InlineMessageBox askBox(MessageKind::Question, QStringLiteral("删除账号"),
                                QStringLiteral("删了就拿不回来了。"),
                                QStringList{QStringLiteral("删除"), QStringLiteral("取消")}, 1, 0);
        QTimer::singleShot(0, &askBox, [&askBox] {
            if (auto *first = askBox.findChild<QPushButton *>(InlineMessageBox::buttonName(0))) {
                first->click();
            }
        });
        const int askCode = overlay->execPrompt(&askBox, false);
        check(askCode == QDialog::Accepted && askBox.clickedIndex() == 0,
              "execPrompt 卡到用户点了为止，并返回被点中的那一颗",
              QStringLiteral("code=%1 index=%2").arg(askCode).arg(askBox.clickedIndex()));
        check(!overlay->isOpen(), "execPrompt 回来之后覆盖层自己收掉了");

        {
            auto *destroyedPrompt = new QDialog;
            QTimer::singleShot(0, destroyedPrompt, &QObject::deleteLater);
            const int destroyedCode = overlay->execPrompt(destroyedPrompt, false);
            check(destroyedCode == QDialog::Rejected && !overlay->isOpen(),
                  "提示被直接销毁也退出等待并清理覆盖层");
        }

        // ④ Esc 那一档：什么都没选 = **-1**（"关掉 / Esc / 点外面"一律算没选）。
        InlineMessageBox escBox(MessageKind::Question, QStringLiteral("删除账号"),
                                QStringLiteral("删了就拿不回来了。"),
                                QStringList{QStringLiteral("删除"), QStringLiteral("取消")}, 1, 0);
        QTimer::singleShot(0, &escBox, [&escBox] {
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&escBox, &esc);
        });
        const int escCode = overlay->execPrompt(&escBox, false);
        check(escCode == QDialog::Rejected && escBox.clickedIndex() == -1,
              "按 Esc = 拒绝，而且没选中任何一颗（-1）",
              QStringLiteral("code=%1 index=%2").arg(escCode).arg(escBox.clickedIndex()));

        // ⑤ 「问一句名字」那一档（输入框）：标题 + 正文 + **一行输入框** +
        // 右边那一排按钮。这一档要钉两件事：
        // - 卡片宽度**照旧是那个唯一出处** —— 多插一行不许多出一个宽度。这一档是
        // 最容易破的那一个（输入框天生想长），破了就正好撞上用户那句
        // 「不会让界面出现尺寸不一样的弹窗」；
        // - 输入框和**正文同宽**（左边对齐），不是"比正文窄一点/宽出去一点"。
        {
            InlineMessageBox inputBox(
                MessageKind::Question, QStringLiteral("重命名实例"),
                QStringLiteral("新名字会成为 versions 下面那个目录的名字，也就是这个实例的"
                               "版本号。里面的 jar 和 json 会一起改。"),
                QStringList{QStringLiteral("确定"), QStringLiteral("取消")}, 0, -1);
            QLineEdit *edit = inputBox.setLineEdit(QStringLiteral("26.3"));
            check(edit != nullptr && edit->objectName() == InlineMessageBox::lineEditName(),
                  "问名字那一档有一行按 objectName 找得到的输入框");
            check(inputBox.lineEditText() == QStringLiteral("26.3"),
                  "预填的内容读得出来（调用点先看 ok、再看它）", inputBox.lineEditText());

            overlay->openPrompt(false)->layout()->addWidget(&inputBox);
            for (int i = 0; i < 3; ++i) {
                QApplication::processEvents();
            }
            QWidget *inputCard = cardOf();
            check(inputCard != nullptr && inputCard->width() == shortWidth,
                  "多插了一行输入框，卡片的宽度**一点没变**",
                  QStringLiteral("%1 → %2")
                      .arg(shortWidth)
                      .arg(inputCard != nullptr ? inputCard->width() : -1));
            check(inputCard != nullptr && inputCard->height() > shortHeight,
                  "它确实比没有输入框的那条更高（高度随内容走）",
                  QStringLiteral("%1 → %2")
                      .arg(shortHeight)
                      .arg(inputCard != nullptr ? inputCard->height() : -1));
            check(edit != nullptr && edit->width() == OverlayHost::promptContentWidth(),
                  "输入框和正文同宽（左边对齐）",
                  QStringLiteral("%1 vs %2")
                      .arg(edit != nullptr ? edit->width() : -1)
                      .arg(OverlayHost::promptContentWidth()));
            // 光标落点：`QDialog::focusWidget()` 是"最后一个被 setFocus() 过的子控件"，
            // 和窗口激不激活无关 —— offscreen 下 `hasFocus()` 那种断言会跟着平台跑。
            // 这一条钉的是"摆上来之后还认得到那一格"（setParent 会把焦点清掉，
            // 所以真正管用的是 showEvent 里那一下，见 InlineMessageBox::showEvent）。
            check(inputBox.focusWidget() == edit,
                  "光标落在输入框上（点开这一档就是为了打字）",
                  inputBox.focusWidget() != nullptr ? inputBox.focusWidget()->objectName()
                                                    : QStringLiteral("<null>"));

            // 上面那一下只是"摆出来量尺寸"，得先收干净 —— 不收的话下面 `execPrompt()`
            // 里的 `openPrompt()` 会因为"提示已经开着"而返回 nullptr，那一发直接当成
            // 拒绝（实测就是这样：code=0 / index=-1，而按钮一次都没点到）。
            overlay->closeOverlay();
            QApplication::processEvents();

            // 点「取消」= 没选（和 QInputDialog 的 ok == false 同一件事）。
            QTimer::singleShot(0, &inputBox, [&inputBox] {
                if (auto *cancel =
                        inputBox.findChild<QPushButton *>(InlineMessageBox::buttonName(1))) {
                    cancel->click();
                }
            });
            const int inputCode = overlay->execPrompt(&inputBox, false);
            // NOTE: 这里**不能**照 QMessageBox 的直觉写成 `Rejected` —— 这一层里每一颗
            // 按钮都是 accept()，"选了什么"由**下标**说（见 InlineMessageBox 的 ctor）。
            // `getText()` 判的就是"是不是第 0 颗"，所以点取消拿到的 ok 是 false。
            // 真"什么都没选"（Esc / 点外面 / 关掉）才是 Rejected + 下标 -1，那一条在
            // 上面 ④ 里单独钉着。
            check(inputCode == QDialog::Accepted && inputBox.clickedIndex() == 1,
                  "点「取消」= 被点中的是第二颗（getText 拿它判 ok，于是 ok == false）",
                  QStringLiteral("code=%1 index=%2").arg(inputCode).arg(inputBox.clickedIndex()));
            check(inputBox.lineEditText() == QStringLiteral("26.3"),
                  "取消也不会把那一格清掉（和 QInputDialog 一样：文本照旧读得出来）",
                  inputBox.lineEditText());
        }

        // ---------- 「挑一个」那一档：卡片里多一张列表 ----------
        //
        // 要求：「在一些其它的地方拖入模组等时选择要安装在的目标实例」。这一档和上面那
        // 个输入框是**同一条规矩**：多插一行**不许多出一个宽度**（那句"不会让界面出现尺寸
        // 不一样的弹窗"），而且插在**按钮行前面**。
        //
        // 高度这一格是这张列表自己的规格：**最多露 `listVisibleRows()` 行**，再多就在列表
        // 内部滚 —— 一个装着七十个实例的列表要是照条数长，卡片会长到把按钮行挤出屏幕。
        {
            InlineMessageBox listBox(
                MessageKind::Question, QStringLiteral("装到哪个实例里"),
                QStringLiteral("这几个文件装进哪一个实例？"),
                QStringList{QStringLiteral("确定"), QStringLiteral("取消")}, 0, -1);
            const QStringList two{QStringLiteral("1.20.1"), QStringLiteral("1.19.4")};
            QListWidget *list = listBox.setList(two, 1);
            check(list != nullptr && list->objectName() == InlineMessageBox::listName(),
                  "「挑一个」那一档有一张按 objectName 找得到的列表");
            check(listBox.listRow() == 1 && list->currentRow() == 1,
                  "预选那一行真的选上了（调用方给的那一格就是用户看到的光标）",
                  QString::number(listBox.listRow()));
            check(list->count() == 2 && list->item(0)->text() == QStringLiteral("1.20.1"),
                  "两行都在，顺序就是传进来的顺序", QString::number(list->count()));
            check(list->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
                  "不挂横向滚动条（实例名长了让它自己截，横着滚会给卡片多出一条从来没用的东西）");

            // 预选下标越界 / 不给（-1）时**不是**"替用户挑第 0 行"：一行都不选。
            InlineMessageBox noSel(MessageKind::Question, QStringLiteral("x"),
                                   QStringLiteral("y"),
                                   QStringList{QStringLiteral("确定")}, 0, -1);
            noSel.setList(two, -1);
            check(noSel.listRow() == -1, "不给预选（-1）时一行都不选，不是默默选第 0 行",
                  QString::number(noSel.listRow()));

            // 插在按钮行前面：布局里的下标必须**小于**按钮行那一个。
            auto *layout = qobject_cast<QVBoxLayout *>(listBox.layout());
            QWidget *buttonRow = listBox.findChild<QWidget *>(InlineMessageBox::buttonRowName());
            check(layout != nullptr && buttonRow != nullptr &&
                      layout->indexOf(list) >= 0 && layout->indexOf(buttonRow) >= 0 &&
                      layout->indexOf(list) < layout->indexOf(buttonRow),
                  "列表排在按钮行**前面**（不然按钮会被挤到列表上面去）",
                  QStringLiteral("list=%1 buttons=%2")
                      .arg(layout != nullptr ? layout->indexOf(list) : -1)
                      .arg(layout != nullptr ? layout->indexOf(buttonRow) : -1));

            // 再调一次 setList()：**换内容，不插第二张**（和 setLineEdit() 同一个规矩）。
            listBox.setList(QStringList({QStringLiteral("a"), QStringLiteral("b"),
                                         QStringLiteral("c")}),
                            2);
            check(listBox.listWidget() == list &&
                      listBox.findChildren<QListWidget *>(InlineMessageBox::listName()).size() == 1,
                  "再调一次 setList() 是换内容，不是插第二张列表",
                  QString::number(
                      listBox.findChildren<QListWidget *>(InlineMessageBox::listName()).size()));
            check(listBox.listRow() == 2, "第二次也把预选摆上了", QString::number(listBox.listRow()));

            // 摆上去量：宽度**照旧是那个唯一出处**。
            overlay->openPrompt(false)->layout()->addWidget(&listBox);
            for (int i = 0; i < 3; ++i) {
                QApplication::processEvents();
            }
            QWidget *listCard = cardOf();
            check(listCard != nullptr && listCard->width() == shortWidth,
                  "多插了一张列表，卡片的宽度**一点没变**",
                  QStringLiteral("%1 → %2")
                      .arg(shortWidth)
                      .arg(listCard != nullptr ? listCard->width() : -1));
            check(list != nullptr && list->width() == OverlayHost::promptContentWidth(),
                  "列表和正文同宽（左边对齐）",
                  QStringLiteral("%1 vs %2")
                      .arg(list != nullptr ? list->width() : -1)
                      .arg(OverlayHost::promptContentWidth()));

            // 高度：**跟着条数长到某个数为止**，超过它就不长了（那一段靠列表内部滚）。
            // 这个"某个数"只有一处出处（`InlineMessageBox::listVisibleRows()`），不写死。
            const auto heightFor = [&listBox](int count) {
                QStringList items;
                for (int i = 0; i < count; ++i) {
                    items.append(QStringLiteral("实例 %1").arg(i + 1));
                }
                listBox.setList(items, -1);
                listBox.adjustSize();
                return listBox.listWidget() != nullptr ? listBox.listWidget()->height() : -1;
            };
            const int h1 = heightFor(1);
            const int h2 = heightFor(2);
            const int cap = InlineMessageBox::listVisibleRows();
            const int hCap = heightFor(cap);
            const int hOver = heightFor(cap + 4);
            check(h1 > 0 && h2 > h1 && hCap > h2,
                  "列表越高行数越多就越高（1 行 < 2 行 < 刚好那么多行）",
                  QStringLiteral("%1 < %2 < %3").arg(h1).arg(h2).arg(hCap));
            check(hOver == hCap,
                  QStringLiteral("超过 %1 行之后高度**就不再长了**（多出来的在列表内部滚）")
                      .arg(cap),
                  QStringLiteral("6 行 %1 / %2 行 %3").arg(hCap).arg(cap + 4).arg(hOver));

            overlay->closeOverlay();
            QApplication::processEvents();

            // ---------- `pickFromList()`：把上面那张列表接到"问一句"上 ----------
            //
            // 摆出来量过的那一下得先收干净（不收的话 `execPrompt()` 里的 `openPrompt()`
            // 会因为"提示已经开着"直接返回 nullptr，那一发就成了拒绝）。
            const QStringList targets{QStringLiteral("第一个"), QStringLiteral("第二个"),
                                      QStringLiteral("第三个")};

            // ① 没有覆盖层可摆 = 不弹、也不静默相信什么。
            int rowOut = 99;
            check(!InlineMessage::pickFromList(nullptr, QStringLiteral("装到哪"),
                                               QStringLiteral("哪一份？"), targets, 0, &rowOut) &&
                      rowOut == -1,
                  "没有 OverlayHost 时 pickFromList 回 false、rowOut 归 -1（调用方据此当"
                  "「用户取消了」）",
                  QString::number(rowOut));
            // ② 一张空表：**不弹**（长得和"有东西可挑"一模一样的空框是骗人）。
            check(!InlineMessage::pickFromList(overlay, QStringLiteral("装到哪"),
                                               QStringLiteral("哪一份？"), QStringList(), 0,
                                               &rowOut),
                  "清单是空的时候一个框都不弹");

            // ③④⑤ 下面这三发都得**在那一发卡住的嵌套事件循环里**去点真框上那颗按钮：
            // `pickFromList()` 自己 new 一个 box 再 `execPrompt()`，外面拿不到它 ——
            // 所以只能从覆盖层上按 objectName 找回来（和 mw_check 里 `currentPrompt()`
            // 同一个手法）。`clearRow` 那一档还要先替用户把选中取消掉。
            const auto answerPick = [overlay](int buttonIndex, bool clearRowFirst) {
                QTimer::singleShot(0, overlay, [overlay, buttonIndex, clearRowFirst] {
                    // 不能用光秃秃的 `findChild<InlineMessageBox *>()`：上面那几个量过尺寸
                    // 的 box 收页之后**仍然挂在覆盖层底下**（`setParent()` 只把它挪回寄存处，
                    // 父链没断），`findChild` 是"第一个"，找回来说不定是那一个 —— 于是一颗
                    // **看不见的**按钮被点了，真提示一动不动，测试挂在嵌套事件循环里
                    // （实测：整趟跑不完、一行日志都不多）。
                    // 判据取"看得见 + 身上真有那张列表"：真提示是唯一一个同时满足的。
                    InlineMessageBox *box = nullptr;
                    const QList<InlineMessageBox *> boxes =
                        overlay->findChildren<InlineMessageBox *>();
                    for (InlineMessageBox *candidate : boxes) {
                        if (candidate->isVisible() && candidate->listWidget() != nullptr) {
                            box = candidate;
                            break;
                        }
                    }
                    if (box == nullptr) {
                        return;
                    }
                    if (clearRowFirst) {
                        if (auto *list = box->listWidget()) {
                            // 连选中一起清（见"setCurrentItem(nullptr) 是连选中一起清"那条）
                            list->setCurrentItem(nullptr);
                        }
                    }
                    if (auto *button =
                            box->findChild<QPushButton *>(InlineMessageBox::buttonName(buttonIndex))) {
                        button->click();
                    }
                });
            };

            // ③ 正常一档：预选第 2 行、点「确定」⇒ true + 那一行。
            answerPick(0, /*clearRowFirst=*/false);
            int picked = -1;
            const bool pickOk = InlineMessage::pickFromList(overlay, QStringLiteral("装到哪个实例里"),
                                                            QStringLiteral("这几个文件装进哪一个实例？"),
                                                            targets, 2, &picked);
            check(pickOk && picked == 2, "点「确定」⇒ true，挑中的就是预选那一行",
                  QStringLiteral("ok=%1 row=%2").arg(pickOk).arg(picked));
            // 那一发之后提示自己收干净了（不收的话下一发 `openPrompt()` 会拿 nullptr，
            // 于是下面两条会变成"什么都没弹"，红得莫名其妙）。
            check(!overlay->promptOpen() && !overlay->isOpen(),
                  "答完那一发，提示自己收干净了（下一发才有地方可弹）");

            // ④ 点「取消」⇒ false（和"什么都没选"同一个结果）。
            answerPick(1, /*clearRowFirst=*/false);
            int cancelledRow = 7;
            check(!InlineMessage::pickFromList(overlay, QStringLiteral("装到哪个实例里"),
                                               QStringLiteral("这几个文件装进哪一个实例？"), targets,
                                               0, &cancelledRow) &&
                      cancelledRow == -1,
                  "点「取消」⇒ false + rowOut 归 -1", QString::number(cancelledRow));

            // ⑤ ★ 点了「确定」但一行都没选中（用户可以 Ctrl+点一下把选中取消掉）⇒ **不算**
            // 选了第 0 行。替用户挑一个，比让他再选一次糟得多。
            answerPick(0, /*clearRowFirst=*/true);
            int noneRow = 5;
            check(!InlineMessage::pickFromList(overlay, QStringLiteral("装到哪个实例里"),
                                               QStringLiteral("这几个文件装进哪一个实例？"), targets,
                                               1, &noneRow) &&
                      noneRow == -1,
                  "★ 点了「确定」但一行都没选中 ⇒ false（不替用户挑第 0 行）",
                  QString::number(noneRow));
        }

        // ---------- 提示压在页面上面（两层） ----------
        // 这一条必须成立：设置页开着的时候，后台一样会冒出来"下载失败"这种话，而真窗口
        // 那会儿它是一个模态窗叠在另一个上面。收掉提示之后**底下那一页还得在**。
        auto *pageContent = overlay->findChild<QWidget *>(OverlayHost::overlayPageContentName());
        QWidget *pageProbe = new QWidget;
        pageProbe->setObjectName(QStringLiteral("overlayProbeInner"));
        QWidget *outerArea = overlay->openPage(QStringLiteral("实例设置"));
        outerArea->layout()->addWidget(pageProbe);
        QApplication::processEvents();

        // 页面开着的时候再要一页 —— 得被拒（见 OverlayHost.hpp「一层只能有一个页面」）。
        check(overlay->openPage(QStringLiteral("另一页")) == nullptr && overlay->pageOpen(),
              "一层只放得下一个页面：页面开着的时候再开一页会被拒（不把它顶掉）");

        InlineMessageBox inner(MessageKind::Warning, QStringLiteral("下载失败"),
                               QStringLiteral("对方把连接掐断了。"), QStringList(), 0, -1);
        QWidget *innerArea = overlay->openPrompt(false);
        check(innerArea != nullptr && overlay->promptOpen() && !overlay->pageOpen(),
              "提示开在页面上面（现在最上面那一层是提示）");
        if (innerArea != nullptr) {
            innerArea->layout()->addWidget(&inner);
            QApplication::processEvents();
        }
        check(!overlay->isOpen() || overlay->promptOpen(), "提示那一层是开着的");
        // 页面那一层的东西**一个都没被搬走**（搬走的话收掉提示之后那一页就空了）。
        check(pageContent != nullptr && pageContent->layout()->count() == 1 &&
                  pageProbe->parentWidget() == pageContent,
              "底下一层还留着它自己的内容（提示没有把页面的东西搬走）",
              QStringLiteral("页面内容容器里有 %1 个控件")
                  .arg(pageContent != nullptr ? pageContent->layout()->count() : -1));
        QLabel *stillThere = overlay->findChild<QLabel *>(OverlayHost::overlayTitleLabelName());
        check(stillThere != nullptr && stillThere->text() == QStringLiteral("实例设置"),
              "底下一层那行标题也还在（页眉是同一套控件）",
              stillThere != nullptr ? stillThere->text() : QStringLiteral("<null>"));
        auto *pageScrollIn = overlay->findChild<QScrollArea *>(OverlayHost::overlayPageScrollName());
        QWidget *pageFrame = pageScrollIn != nullptr ? pageScrollIn->parentWidget() : nullptr;
        check(pageFrame != nullptr && !pageFrame->isVisible(), "提示开着的时候整页那一层收起来了");
        // 逐层收：先收提示，底下的页面**回到屏幕上**。
        overlay->closeOverlay();
        QApplication::processEvents();
        check(overlay->pageOpen() && overlay->isOpen(), "收掉提示之后回到那一页（不是全关掉）");
        check(pageFrame != nullptr && pageFrame->isVisible(), "回到那一页时它又露出来了");
        check(pageContent != nullptr && pageContent->layout()->count() == 1,
              "回到那一页时它的内容原样还在",
              QStringLiteral("%1 个")
                  .arg(pageContent != nullptr ? pageContent->layout()->count() : -1));
        // 再收一层：这次是全关掉。
        overlay->closeOverlay();
        QApplication::processEvents();
        check(!overlay->isOpen() && !overlay->isVisible(), "再收一层就真的全关掉了");
        check(pageContent != nullptr && pageContent->layout()->count() == 0,
              "全关掉之后两个内容容器都清空了",
              QStringLiteral("页面 %1 个")
                  .arg(pageContent != nullptr ? pageContent->layout()->count() : -1));
        delete pageProbe;

        // 提示叠提示：不做（外面那一层正卡在自己的事件循环里，把它顶掉就是死等）。
        InlineMessageBox first(MessageKind::Information, QStringLiteral("第一句"),
                               QStringLiteral("先来的那一句。"), QStringList(), 0, -1);
        InlineMessageBox second(MessageKind::Information, QStringLiteral("第二句"),
                                QStringLiteral("后来的那一句。"), QStringList(), 0, -1);
        overlay->openPrompt(true)->layout()->addWidget(&first);
        QApplication::processEvents();
        check(overlay->openPrompt(true) == nullptr && overlay->promptOpen(),
              "提示叠提示会被拒（不把外面那一层顶掉）");
        overlay->closeOverlay();
        QApplication::processEvents();
        check(!overlay->isOpen(), "拒绝之后外面那条提示照旧收得掉");

        {
            InlineMessageBox holder(MessageKind::Information, QStringLiteral("先来的"),
                                    QStringLiteral("上一条还开着。"), QStringList(), 0, -1);
            overlay->openPrompt(true)->layout()->addWidget(&holder);
            QApplication::processEvents();
            // 这一条不该把上一条顶掉、也不该当场丢：它被排着。
            InlineMessage::warning(overlay, QStringLiteral("下载失败"),
                                   QStringLiteral("后台收尾的那一句。"));
            check(overlay->promptOpen(), "纯报信提示不顶掉已经开着的那一条");
            bool sawQueued = false;
            QTimer::singleShot(100, overlay, [&] {
                if (overlay->promptOpen()) {
                    sawQueued = true;
                    // 和用户按 Esc 同一发：`execPrompt()` 的循环靠它退出（`closeOverlay()`
                    // 只收外壳、不 reject 对话框）。
                    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                    QApplication::sendEvent(overlay, &esc);
                }
            });
            overlay->closeOverlay();  // 收掉上一条 → 排着的那一条下一轮摆出来
            QElapsedTimer wait;
            wait.start();
            while (!sawQueued && wait.elapsed() < 3000) {
                QApplication::processEvents();
            }
            check(sawQueued, "上一条提示收掉之后，排着的那条纯报信提示真的摆出来了");
            QApplication::processEvents();
            check(!overlay->isOpen(), "排队那条收掉之后覆盖层回到全关");
        }

        // ---------- ★ 提示叠在**产品那条路**上：点掉提示不能把底下那一页一起拒掉 ----------
        //
        // 上面那一段走的是 `openPage` / `openPrompt` —— 这两个函数**不卡调用方**，所以
        // 它们量不到 `execEmbedded` 里那条连接；而「点一下提示框外面，底下那一页跟着没了」
        // 这个 bug 只在 `execEmbedded` 的嵌套事件循环里才成立（页面那一次的 `loop.exec()`
        // 还卡着、它那条连接还活着）。这一段走产品那条路（阻塞的 `execPage`），复现的是
        // 那一串：「分组设置」那一页 → 改完名字点确定 → 叠出一条重名警告 →
        // 点警告框外面 → 整页跟着没了、改的全丢（GroupDialog 判的是 `!= Accepted`）。
        //
        // 判据放在**返回值**上：那一页是被自己 accept 掉的还是被上面那条提示连坐 reject 掉的，
        // 从 `execPage` 返回什么就看得出来（连坐的话返回的就是 Rejected）。
        {
            QDialog pageDialog;
            int promptDismissals = 0;
            const QMetaObject::Connection dismissSpy = QObject::connect(
                overlay, &OverlayHost::promptDismissed, overlay,
                [&promptDismissals] { ++promptDismissals; });
            // 「底下那一页被连坐拒掉了没有」量的是**它自己发没发 finished** —— 不能用
            // `isVisible()`：提示压在页上的时候整页那一层是收着的（那是设计要求，
            // 上面那一段还专门钉过），那会儿连子控件的 `isVisible()` 都是假，两件事分不开。
            int pageFinishedCount = 0;
            int pageFinishedCode = 0;
            const QMetaObject::Connection pageSpy = QObject::connect(
                &pageDialog, &QDialog::finished, &pageDialog, [&](int code) {
                    ++pageFinishedCount;
                    pageFinishedCode = code;
                });

            QTimer::singleShot(0, overlay, [&] {
                // 进到这里的时候页面那一层已经开着、`execPage` 卡在它自己的事件循环里。
                check(overlay->pageOpen() && pageDialog.isVisible(),
                      "★ 前提：页面那一层先开着（这一段量的是叠在上面那一发）");
                InlineMessageBox warning(MessageKind::Warning, QStringLiteral("分组名重复"),
                                         QStringLiteral("已经有一个同名分组了。"),
                                         QStringList(), 0, -1);
                QWidget *area = overlay->openPrompt(true); // 这一档：点框外面算关掉
                check(area != nullptr && overlay->promptOpen() && !overlay->pageOpen(),
                      "★ 提示真的叠在那一页上面（现在最上面那一层是提示）");
                if (area != nullptr) {
                    area->layout()->addWidget(&warning);
                    QApplication::processEvents();
                }
                // 点框外面 —— 用户报的那一下。
                QMouseEvent outsidePress(QEvent::MouseButtonPress, QPointF(4, 4), QPointF(4, 4),
                                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(overlay, &outsidePress);
                QApplication::processEvents();
                check(promptDismissals == 1, "★ 点框外面发出了一次 promptDismissed",
                      QStringLiteral("发了 %1 次").arg(promptDismissals));
                // 这一条就是那个 bug 的形状：点框外面这一发如果连坐，`pageDialog` 会当场
                // 收掉、`finished(Rejected)` 立刻发出去、`execPage` 返回 Rejected。用户
                // 那边看到的就是"我只点关了一条提示，刚编辑的分组设置全丢了"。
                check(pageFinishedCount == 0,
                      "★ 那一发没有连坐到底下的页（它一次 finished 都没发）",
                      QStringLiteral("finished %1 次，code=%2")
                          .arg(pageFinishedCount)
                          .arg(pageFinishedCode));
                // 非阻塞开出来的提示没人替它收（产品那条路是 `execPrompt` 的循环收的）。
                overlay->closeOverlay();
                QApplication::processEvents();
                check(overlay->pageOpen() && pageDialog.isVisible() &&
                          pageFinishedCount == 0,
                      "★ 收掉提示之后回到那一页（页和它的对话框都还在）");
                // 页面自己走完（用户点了「确定」）。
                pageDialog.accept();
            });

            const int pageCode = overlay->execPage(&pageDialog, QStringLiteral("分组设置"));
            check(pageCode == QDialog::Accepted,
                  "★ 那一页返回的是 Accepted（不是被上面那条提示连坐的 Rejected）",
                  QStringLiteral("code=%1").arg(pageCode));
            check(promptDismissals == 1 && !overlay->isOpen(),
                  "★ 收尾：提示那一层关掉了、页面那一层自己也收掉了");
            QObject::disconnect(dismissSpy);
        }
    }
    return 0;
}



#include "Support.hpp"
int UiLaunchPromptsChecks(MainWindowUI &ui) {
    if (OverlayHost *overlay = ui.overlay()) {
        // ---- (a) 小字那一行 ----
        {
            const QString noteText =
                QStringLiteral("可以在设置中关闭此提示（设置页「启动提醒」→「重复启动」）。");
            InlineMessageBox box(
                MessageKind::Warning, QStringLiteral("已经有一个相同的运行中游戏进程了！"),
                QStringLiteral("这个实例上已经有一份游戏在跑了，开第二份会同时打开同一份存档。"),
                QStringList{QStringLiteral("取消"), QStringLiteral("仍要启动")}, -1, -1);
            box.setNote(noteText);
            check(box.noteText() == noteText, "setNote 之后那一行读得回来", box.noteText());

            overlay->openPrompt(false)->layout()->addWidget(&box);
            // 多转两圈（理由同上面输入框那一档：LayoutRequest 在下一轮才轮到）。
            for (int i = 0; i < 3; ++i) {
                QApplication::processEvents();
            }
            QWidget *card = overlay->findChild<QWidget *>(OverlayHost::overlayPromptHostName());
            auto *note = box.findChild<QLabel *>(InlineMessageBox::noteLabelName());
            auto *text = box.findChild<QLabel *>(InlineMessageBox::textLabelName());
            auto *row = box.findChild<QWidget *>(InlineMessageBox::buttonRowName());
            check(note != nullptr && note->text() == noteText,
                  "小字按 objectName 找得到、写的就是那一句",
                  note != nullptr ? note->text() : QStringLiteral("<null>"));
            check(card != nullptr && card->width() == OverlayHost::promptWidth(),
                  "多了一行小字，卡片宽度**一点没变**（所有提示一样宽那条铁律）",
                  QStringLiteral("%1 vs %2")
                      .arg(card != nullptr ? card->width() : -1)
                      .arg(OverlayHost::promptWidth()));
            check(note != nullptr && note->width() == OverlayHost::promptContentWidth(),
                  "小字和正文同宽（左边对齐）",
                  QStringLiteral("%1 vs %2")
                      .arg(note != nullptr ? note->width() : -1)
                      .arg(OverlayHost::promptContentWidth()));
            // 摆位：正文之上、按钮行之下。比的是**窗口坐标**（三个控件不在同一个父上）。
            if (note != nullptr && text != nullptr && row != nullptr) {
                const int textY = text->mapTo(&box, QPoint(0, 0)).y();
                const int noteY = note->mapTo(&box, QPoint(0, 0)).y();
                const int rowY = row->mapTo(&box, QPoint(0, 0)).y();
                check(textY < noteY && noteY < rowY,
                      "它在正文下面、按钮行上面（不是钻到按钮底下去了）",
                      QStringLiteral("正文 %1 ／ 小字 %2 ／ 按钮行 %3")
                          .arg(textY)
                          .arg(noteY)
                          .arg(rowY));
                // 小一号：字号是**在同一个调色板的字体上算**的（不写死点数），所以这里
                // 只比"它比正文小"，不钉具体点数。
                const qreal textPt = text->font().pixelSize();
                const qreal notePt = note->font().pixelSize();
                check(notePt > 0 && textPt > 0 && notePt < textPt,
                      "小字确实比正文小一号",
                      QStringLiteral("正文 %1pt ／ 小字 %2pt").arg(textPt).arg(notePt));
                // 浅一档：全仓"比正文浅一档"就是 QPalette::PlaceholderText 那一格。
                check(note->palette().color(QPalette::WindowText) ==
                          note->palette().color(QPalette::PlaceholderText),
                      "小字的颜色取自 PlaceholderText（比正文浅一档那个唯一去处）",
                      note->palette().color(QPalette::WindowText).name());
            }

            const int withNote = card != nullptr ? card->height() : -1;
            overlay->closeOverlay();
            QApplication::processEvents();

            // 传空串 = **不补那一行**（不是"补一行空的"）。同一个卡片上撤掉它，高度得掉回去。
            box.setNote(QString());
            check(box.noteText().isEmpty() &&
                      box.findChild<QLabel *>(InlineMessageBox::noteLabelName()) == nullptr,
                  "传空串 = 那一行整个撤掉（不是留一个空标签占着高度）");
            overlay->openPrompt(false)->layout()->addWidget(&box);
            for (int i = 0; i < 3; ++i) {
                QApplication::processEvents();
            }
            QWidget *card2 = overlay->findChild<QWidget *>(OverlayHost::overlayPromptHostName());
            const int withoutNote = card2 != nullptr ? card2->height() : -1;
            check(withNote > 0 && withoutNote > 0 && withNote > withoutNote,
                  "有小字的那一张确实更高（高度随内容走）",
                  QStringLiteral("%1 → %2").arg(withNote).arg(withoutNote));
            check(card2 != nullptr && card2->width() == OverlayHost::promptWidth(),
                  "撤掉那一行之后宽度照旧是那个唯一出处",
                  QString::number(card2 != nullptr ? card2->width() : -1));
            // 按钮还是那两颗（要求写的就是「取消 / 仍要启动」）。
            check(box.findChild<QPushButton *>(InlineMessageBox::buttonName(0)) != nullptr &&
                      box.findChild<QPushButton *>(InlineMessageBox::buttonName(1)) != nullptr &&
                      box.findChild<QPushButton *>(InlineMessageBox::buttonName(2)) == nullptr,
                  "下面还是「取消 / 仍要启动」两颗（多一颗少一颗都会被这条抓住）");
            // Esc = 没选（`-1`）：这两条确认框都是"点外面不算数"的那种，所以这里只量 Esc
            // 这一档 —— "点外面不关"那一条在上面整页那一节里钉过。
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&box, &esc);
            QApplication::processEvents();
            check(box.clickedIndex() == -1, "按 Esc = 什么都没选（-1）",
                  QString::number(box.clickedIndex()));
            overlay->closeOverlay();
            QApplication::processEvents();
        }

        // ---- (b) 首字母头像（什么都没有的时候那块兜底） ----
        {
            // NOTE: 这里用 `==` 比颜色是**对的**，别照着"看着不一样"去把它改成 `.name()`：
            // `avatarFallbackColor()` 是先算 HSV 再**按 8 位量化**回来的，
            // 而图里存的、`pixelColor()` 取回来的也是 8 位精确值 —— 两边恒等。
            // （Qt 6 的 QColor 内部是 16 位的，少了那一步量化就会"名字、rgba() 全一样、
            // 只有 == 说不相等"，这个坑 在 mw_check 的 AH7b 上吃过一整轮。）
            const auto hasWhitePixel = [](const QImage &image) {
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        // 「近白」：问号这种细笔画在文楷字体下全是抗锯齿过渡色，没有一个纯白像素。
                        const QColor c = image.pixelColor(x, y);
                        if (qMin(c.red(), qMin(c.green(), c.blue())) >= 225) {
                            return true;
                        }
                    }
                }
                return false;
            };

            const QString bob = QStringLiteral("Bob");
            const QImage bobAvatar = SkinModel::letterAvatar(bob, 24);
            check(bobAvatar.size() == QSize(24, 24), "边长就是要的那个数（面板里是 24）",
                  QStringLiteral("%1x%2").arg(bobAvatar.width()).arg(bobAvatar.height()));
            check(bobAvatar.pixelColor(0, 0) == SkinModel::avatarFallbackColor(bob),
                  "底色由名字决定（同一个名字永远同一个色）",
                  bobAvatar.pixelColor(0, 0).name());
            check(hasWhitePixel(bobAvatar),
                  "上面那个字母是纯白的（底色是算出来的，字钉成白的对比度才有下限）");
            check(SkinModel::letterAvatar(bob, 24) == bobAvatar,
                  "同一个名字画两遍一模一样（不是每次重画一个色）");
            check(SkinModel::avatarFallbackColor(bob) !=
                      SkinModel::avatarFallbackColor(QStringLiteral("Carol")),
                  "不同名字的底色不一样（三行都是同一个灰、要靠读字母就分不开了）",
                  QStringLiteral("%1 vs %2")
                      .arg(SkinModel::avatarFallbackColor(bob).name(),
                           SkinModel::avatarFallbackColor(QStringLiteral("Carol")).name()));
            check(SkinModel::letterAvatar(bob, 24) !=
                      SkinModel::letterAvatar(QStringLiteral("Carol"), 24),
                  "不同名字画出来是两张不同的图（字母 + 底色都不同）");

            // 边长变大，方块和字**一起**长（字号是按边长算的，不是写死的点数）。
            const QImage big = SkinModel::letterAvatar(bob, 48);
            check(big.size() == QSize(48, 48), "边长给多大就画多大");
            check(hasWhitePixel(big) && !hasWhitePixel(SkinModel::letterAvatar(bob, 0)),
                  "边长给 0 是一张空图（不是 0×0 的怪东西、也不该崩）",
                  QStringLiteral("48 档有字=%1").arg(hasWhitePixel(big)));

            // 名字是汉字/数字/符号也得画得出来（字母是**用字体画的**，所以任意一个字符都行
            // —— 手摆像素那套（兜底皮肤上那个问号）在这里覆盖不了）。
            const QImage han = SkinModel::letterAvatar(QStringLiteral("咱俩的存档"), 24);
            const QImage digit = SkinModel::letterAvatar(QStringLiteral("7f"), 24);
            check(hasWhitePixel(han) && hasWhitePixel(digit),
                  "汉字开头、数字开头的名字都画得出那一个字");
            check(han.pixelColor(0, 0) ==
                      SkinModel::avatarFallbackColor(QStringLiteral("咱俩的存档")),
                  "底色照旧跟着名字走（和是不是 ASCII 无关）");
            // 空名字：一块**有色的**方块 + 一个问号（"什么都没有"和"有个占位"在界面上
            // 分不出来，而后者至少说明这一行加载过了）。
            const QImage empty = SkinModel::letterAvatar(QString(), 24);
            check(empty.size() == QSize(24, 24) && hasWhitePixel(empty),
                  "空名字也给一块有色方块 + 一个问号（不是一块空白）",
                  empty.pixelColor(0, 0).name());
        }
    } else {
        check(false, "Z12 前提：找得到那个内容区覆盖层");
    }
    return 0;
}



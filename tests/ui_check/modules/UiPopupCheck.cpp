#include "Support.hpp"
#include "Popups.hpp"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QHelpEvent>
#include <QPushButton>
#include <QToolButton>
#include <QTimer>
#include <QComboBox>
#include <QCompleter>
#include <QListView>
#include <QLineEdit>
#include <QMenu>
#include <QScreen>
#include <QScrollBar>
#include <QStyle>
#include <QFrame>
#include <QVBoxLayout>

namespace {
constexpr int kPopupRowHeight = 30;  // Popups.cpp kRowHeight
constexpr int kPopupGap = 4;         // Popups.cpp kPopupGap：弹层顶边离框底边
}

namespace {

/// 浮层这一层现在的样子：磨砂玻璃底 + 1px 亮边，**不再有**系统那个黄底方角气泡。
/// 判据取"整块主色离 tokens().glass 很近、且一个黄底像素都没有"。
bool looksLikeGlass(const QImage &shot) {
    const QColor glass = overOpaque(Theme::tokens().glass, Theme::tokens().page);
    const QColor fill = dominantColor(shot, shot.rect().adjusted(4, 4, -4, -4));
    // 系统那条是黄底（#ffffe1 一类）；我们这条是玻璃压在页面色上，两者差得开。
    return colorDistance(fill, glass) <= 24 &&
           colorDistance(fill, QColor(0xff, 0xff, 0xe1)) > 12;
}

/// 把一张带透明底的浮层图垫到页面色上，存成验收图。
bool saveShot(const QWidget *widget, const QString &path) {
    const QImage shot = captureWidgetOn(const_cast<QWidget *>(widget), Theme::tokens().page);
    QImage canvas(shot.size() + QSize(40, 40), QImage::Format_ARGB32);
    canvas.fill(Theme::tokens().page);
    QPainter painter(&canvas);
    painter.drawImage(QPoint(20, 20), shot);
    painter.end();
    return canvas.save(path);
}

}  // namespace

/// 浮层那一层：tooltip / 下拉弹层 / 菜单。验收图写进 `WTFML_POPUP_SHOTS` 指的那个目录。
int UiPopupCheck(QApplication &app) {
    const int fontId = QFontDatabase::addApplicationFont(
        QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc")));
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty()) {
        app.setFont(QFont(families.first(), 10));
    }
    app.setStyle(Theme::createStyle());
    Theme::setSpec(Theme::defaultSpec());
    Theme::apply(app);
    Anim::setEnabled(false);  // 验收图不靠动效，断言也不该等它

    int failures = 0;
    const auto expect = [&failures](bool ok, const QString &text,
                                    const QString &detail = QString()) {
        const QByteArray utf8 = text.toUtf8();
        const QByteArray extra = detail.toUtf8();
        printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", utf8.constData(),
               extra.isEmpty() ? "" : "   ", extra.constData());
        fflush(stdout);
        if (!ok) ++failures;
    };

    const QString shots = qEnvironmentVariable("WTFML_POPUP_SHOTS");
    if (!shots.isEmpty()) {
        QDir().mkpath(shots);
    }

    // ---------- ① tooltip：自绘那一条 ----------
    {
        QWidget host;
        auto *button = new QPushButton(QStringLiteral("文件夹"), &host);
        button->setIcon(QIcon());
        Q_UNUSED(button);
        host.resize(240, 120);
        host.show();
        QApplication::processEvents();

        Popups::showTooltip(QStringLiteral("打开实例文件夹"), host.mapToGlobal(QPoint(40, 40)), nullptr);
        QApplication::processEvents();
        expect(Popups::tooltipVisible() && Popups::tooltipText() == QStringLiteral("打开实例文件夹"),
               "tooltip 是自绘的那一条（内容对得上，系统那条不出现）");
        QWidget *bubble = nullptr;
        for (QWidget *widget : QApplication::allWidgets()) {
            if (widget->objectName() == QStringLiteral("wtfmlTooltip")) {
                bubble = widget;
                break;
            }
        }
        expect(bubble != nullptr && bubble->testAttribute(Qt::WA_TranslucentBackground),
               "tooltip 那个窗口是透明底的（磨砂才透得出来）");
        if (bubble != nullptr) {
            const QImage shot = captureWidgetOn(bubble, Theme::tokens().page);
            expect(looksLikeGlass(shot),
                   "tooltip 是磨砂玻璃、不是系统那个黄底方框",
                   QStringLiteral("主色 %1（该是 %2）")
                       .arg(dominantColor(shot, shot.rect().adjusted(4, 4, -4, -4)).name(),
                            overOpaque(Theme::tokens().glass, Theme::tokens().page).name()));
        }
        Popups::hideTooltip();
        QApplication::processEvents();
        expect(!Popups::tooltipVisible(), "hideTooltip() 能收回去");
        host.hide();
    }

    // ---------- ①a UI问题 #10：提示落在鼠标右下方、宽度有上限、字少框就窄 ----------
    {
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        const QPoint mouse = area.topLeft() + QPoint(100, 80);
        Popups::showTooltip(QStringLiteral("打开文件夹"), mouse, nullptr);
        QApplication::processEvents();
        const QRect shortTip = Popups::tooltipGeometry();
        expect(shortTip.left() > mouse.x() && shortTip.top() > mouse.y() && shortTip.left() - mouse.x() < 40 &&
                   shortTip.top() - mouse.y() < 40,
               "UI问题 #10 提示出现在鼠标右下方附近", QStringLiteral("鼠标 %1,%2 提示 %3,%4")
                   .arg(mouse.x()).arg(mouse.y()).arg(shortTip.left()).arg(shortTip.top()));
        Popups::showTooltip(QStringLiteral("打开文件夹"), mouse + QPoint(150, 90), nullptr);
        expect(Popups::tooltipGeometry().topLeft() - shortTip.topLeft() == QPoint(150, 90),
               "UI问题 #10 鼠标换到别处，提示跟着换位置（不钉在控件上）");
        Popups::moveTooltipTo(mouse);
        expect(Popups::tooltipGeometry().topLeft() == shortTip.topLeft(), "UI问题 #10 提示亮着时跟着鼠标走");

        const QString medium = QStringLiteral("打开实例所在的文件夹，里面有存档、模组和配置。");
        QString longText;
        for (int i = 0; i < 6; ++i) longText += medium;
        Popups::showTooltip(medium, mouse, nullptr);
        const QRect mediumTip = Popups::tooltipGeometry();
        Popups::showTooltip(longText, mouse, nullptr);
        const QRect longTip = Popups::tooltipGeometry();
        expect(shortTip.width() < mediumTip.width() && shortTip.height() == mediumTip.height(),
               "UI问题 #10 字少框就窄，一行时高度不变",
               QStringLiteral("短 %1x%2 / 中 %3x%4").arg(shortTip.width()).arg(shortTip.height())
                   .arg(mediumTip.width()).arg(mediumTip.height()));
        expect(longTip.width() <= 420 + 20 && longTip.height() > mediumTip.height() * 2 && longTip.width() > 300,
               "UI问题 #10 长提示宽度到上限就换行（不是又窄又高）",
               QStringLiteral("长 %1x%2").arg(longTip.width()).arg(longTip.height()));
        // 整段没有空格也能换行（路径、长英文串）。
        Popups::showTooltip(QString(200, QLatin1Char('W')), mouse, nullptr);
        expect(Popups::tooltipGeometry().width() <= 420 + 20 && Popups::tooltipGeometry().height() > mediumTip.height(),
               "UI问题 #10 没有空格的长串也在宽度上限处断行");

        const QPoint corner = area.bottomRight() - QPoint(6, 6);
        Popups::showTooltip(longText, corner, nullptr);
        const QRect cornerTip = Popups::tooltipGeometry();
        expect(area.contains(cornerTip) && cornerTip.right() < corner.x() && cornerTip.bottom() < corner.y(),
               "UI问题 #10 鼠标在屏幕右下角：提示翻到鼠标左上方、不出屏", QStringLiteral("提示 %1,%2 %3x%4 / 可用区 %5x%6")
                   .arg(cornerTip.left()).arg(cornerTip.top()).arg(cornerTip.width()).arg(cornerTip.height())
                   .arg(area.width()).arg(area.height()));
        Popups::hideTooltip();
        QApplication::processEvents();
    }

    // ---------- ①b 500ms 才出、1 秒内换控件立刻出 ----------
    {
        QWidget host;
        auto *iconA = new QPushButton(&host);
        auto *iconB = new QPushButton(&host);
        iconA->setToolTip(QStringLiteral("重新扫描 Java"));   // 没有 text() ⇒ 图标钮，出提示
        iconB->setToolTip(QStringLiteral("打开实例文件夹"));
        auto *row = new QHBoxLayout(&host);
        row->addWidget(iconA);
        row->addWidget(iconB);
        host.resize(240, 80);
        host.show();
        QApplication::processEvents();
        // 先等过「刚收起 1 秒内立刻出」那个窗口，不然这一条量的是那条规则（不是 500ms）。
        {
            QElapsedTimer warm;
            warm.start();
            while (warm.elapsed() < 1100) {
                QApplication::processEvents(QEventLoop::AllEvents, 50);
            }
        }

        // 进去之后马上查：还没到 500ms，什么都不该出。
        QEvent enter(QEvent::Enter);
        QApplication::sendEvent(iconA, &enter);
        QApplication::processEvents();
        const bool early = Popups::tooltipVisible();
        // 再等够 500ms（跑事件循环让它到点）。
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 700) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        expect(!early && Popups::tooltipVisible() &&
                   Popups::tooltipText() == QStringLiteral("重新扫描 Java"),
               "悬停 500ms 才出提示（更早的时候不出）",
               QStringLiteral("早查 %1 / 到点 %2").arg(early ? 1 : 0).arg(Popups::tooltipVisible() ? 1 : 0));

        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(iconA, &leave);
        QApplication::processEvents();
        // 刚收起，马上移到相邻那一颗：不等，立刻出。
        QApplication::sendEvent(iconB, &enter);
        QApplication::processEvents();
        expect(Popups::tooltipVisible() &&
                   Popups::tooltipText() == QStringLiteral("打开实例文件夹"),
               "收起后 1 秒内换到相邻控件：立刻出，不再等 500ms",
               Popups::tooltipText());
        QApplication::sendEvent(iconB, &leave);
        QApplication::processEvents();
        host.hide();
    }

    // ---------- ② 文字按钮不出提示 ----------
    {
        QPushButton textButton(QStringLiteral("浏览…"));
        QPushButton iconButton;
        iconButton.setIcon(QIcon());
        QComboBox combo;
        QLineEdit edit;
        expect(Popups::isTextButton(&textButton) && !Popups::isTextButton(&iconButton) &&
                   !Popups::isTextButton(&combo) && !Popups::isTextButton(&edit),
               "判据：只有带 text() 的按钮算「文字按钮」，图标钮 / 下拉 / 输入框不算");
        // 实例文件那排 ⓘ / ✕：屏幕上是图标，text() 只是给无障碍与验证工程读的 —— 它们仍该有提示。
        QToolButton glyphButton;
        glyphButton.setToolButtonStyle(Qt::ToolButtonIconOnly);
        glyphButton.setText(QStringLiteral("详情"));
        QPushButton markedIcon;
        markedIcon.setText(QStringLiteral("详情"));
        Theme::markIconButton(&markedIcon);
        expect(!Popups::isTextButton(&glyphButton) && !Popups::isTextButton(&markedIcon),
               "只画图标的钮（`ToolButtonIconOnly` / `markIconButton()` 过）不算文字按钮，提示照出");
        // 真发一条 ToolTip 事件：文字按钮那一条要被吃掉，而且不许弹出自绘气泡。
        textButton.setToolTip(QStringLiteral("浏览…"));
        QHelpEvent help(QEvent::ToolTip, QPoint(2, 2), textButton.mapToGlobal(QPoint(2, 2)));
        QApplication::sendEvent(&textButton, &help);
        QApplication::processEvents();
        expect(!Popups::tooltipVisible(), "文字按钮的悬停提示不出（事件被吃掉）");
    }

    // ---------- ③ 下拉弹层 ----------
    {
        const auto makeCombo = [](QWidget *parent, int items) {
            auto *combo = new QComboBox(parent);
            for (int i = 0; i < items; ++i) {
                combo->addItem(QStringLiteral("选项 %1").arg(i + 1));
            }
            return combo;
        };
        QWidget host;
        auto *shortCombo = makeCombo(&host, 5);
        auto *longCombo = makeCombo(&host, 45);
        auto *row = new QHBoxLayout(&host);
        row->addWidget(shortCombo);
        row->addWidget(longCombo);
        host.resize(420, 120);
        host.show();
        shortCombo->showPopup();
        QApplication::processEvents();

        expect(shortCombo->maxVisibleItems() == 8, "下拉最多露 8 项（限高，不再整屏铺开）");
        QAbstractItemView *view = shortCombo->view();
        QWidget *popup = view != nullptr ? view->window() : nullptr;
        expect(popup != nullptr && popup != &host && popup->testAttribute(Qt::WA_TranslucentBackground),
               "下拉弹层是透明底的独立窗口（磨砂才透得出来）");
        expect(view != nullptr && view->itemDelegate() != nullptr &&
                   view->itemDelegate()->property("wtfmlPopupDelegate").toBool(),
               "下拉那一列换成了自绘代理（项高 30 / 悬停淡底 / 当前项强调淡底）");
        if (view != nullptr) {
            const QRect rowRect = view->visualRect(view->model()->index(0, 0));
            expect(rowRect.height() == kPopupRowHeight, "项高 30（照 PCL 收紧）",
                   QStringLiteral("量到 %1").arg(rowRect.height()));
        }
        expect(popup != nullptr && popup->height() <= kPopupRowHeight * 8 + 12 + 4,
               "弹层高度压在 8 项以内",
               QStringLiteral("%1 px").arg(popup != nullptr ? popup->height() : -1));
        expect(popup != nullptr &&
                   popup->findChild<QLineEdit *>(QStringLiteral("wtfmlPopupSearch")) == nullptr,
               "短列表（5 项）没有搜索框");
        // #337：弹层**挂在框正下方**（不是 Fusion 那种当前项对齐、整张表盖在框上）。
        if (popup != nullptr) {
            const QRect frame = popup->frameGeometry();
            const QPoint comboBottomLeft = shortCombo->mapToGlobal(QPoint(0, shortCombo->height()));
            // ±1：`wtfml.popups-scale`（1.25 倍）下逻辑坐标来回换算会差一像素。
            expect(qAbs(frame.top() - (comboBottomLeft.y() + kPopupGap)) <= 1 &&
                       qAbs(frame.left() - comboBottomLeft.x()) <= 1 &&
                       frame.width() >= shortCombo->width(),
                   "弹层顶边 = 框底边 + 4、左缘对齐框左缘、宽度不小于框宽（不盖住框）",
                   QStringLiteral("弹层 (%1,%2) 宽 %3 / 框底左 (%4,%5) 宽 %6")
                       .arg(frame.left()).arg(frame.top()).arg(frame.width())
                       .arg(comboBottomLeft.x()).arg(comboBottomLeft.y()).arg(shortCombo->width()));
        }
        expect(shortCombo->style()->styleHint(QStyle::SH_ComboBox_Popup, nullptr, shortCombo) == 0,
               "样式不再要求「当前项盖在框上」那种弹法（SH_ComboBox_Popup = 0）");
        // 5 项装得下：这里**不该**有滚动（以前弹层矮一截，靠"内容溢出"才意外滚得起来）。
        const int shortScrollMax = view != nullptr ? view->verticalScrollBar()->maximum() : 0;
        expect(view != nullptr && shortScrollMax == 0, "5 项装得下就不滚（滚动上限 0）",
               QStringLiteral("滚动上限 %1").arg(shortScrollMax));
        if (view != nullptr) {
            // 细条：常驻的是 3px 一根，但常态透明度是 0（滚动时才露、停 600ms 淡出）。
            auto *effect = qobject_cast<QGraphicsOpacityEffect *>(view->verticalScrollBar()->graphicsEffect());
            expect(Theme::widgetStyleSheet().contains(
                       QLatin1String("QComboBox QAbstractItemView QScrollBar:vertical { width: 3px")) &&
                       effect != nullptr && effect->opacity() < 0.5,
                   "滚动条是 3px 细条、常态不显（S2）",
                   QStringLiteral("3px 规则 %1 / 透明度 %2")
                       .arg(Theme::widgetStyleSheet().contains(
                                QLatin1String("QComboBox QAbstractItemView QScrollBar:vertical { width: 3px"))
                                ? 1 : 0)
                       .arg(effect != nullptr ? effect->opacity() : -1.0));
            // Qt 自带那两块上下翻页箭头也不许在（用户不要任何小箭头）。
            int scrollers = 0;
            if (popup != nullptr) {
                for (QWidget *child : popup->findChildren<QWidget *>()) {
                    if (QString::fromLatin1(child->metaObject()->className())
                            .contains(QLatin1String("QComboBoxPrivateScroller")) &&
                        child->isVisible()) {
                        ++scrollers;
                    }
                }
            }
            expect(scrollers == 0, "弹层上下那两块系统翻页箭头不在了",
                   QStringLiteral("还露着 %1 块").arg(scrollers));
        }
        // 弹层宽度按**最长那一项**撑开：Qt 只照外面那个下拉框的宽摆弹层，而项里文字左右
        // 各让出 12px、弹层自己还有 6px 内边距 —— 不撑开就切字，而且不带省略号（任务中心
        // 那个「全部任务」筛选就是这么被切掉的）。
        auto *wideCombo = new QComboBox(&host);
        wideCombo->addItems({QStringLiteral("全部任务"), QStringLiteral("正在进行"),
                             QStringLiteral("已结束"), QStringLiteral("需要处理")});
        wideCombo->setFixedWidth(60);  // 比最长那一项窄：模拟被工具栏挤窄的下拉框
        wideCombo->show();
        QApplication::processEvents();
        wideCombo->showPopup();
        QApplication::processEvents();
        QWidget *widePopup = wideCombo->view() != nullptr ? wideCombo->view()->window() : nullptr;
        const int widestRow = wideCombo->view() != nullptr
                                  ? wideCombo->view()->sizeHintForColumn(0)
                                  : 0;
        expect(widePopup != nullptr && widestRow > 0 && widePopup->width() >= widestRow + 12,
               "弹层宽度按最长那一项撑开（项里左右各让 12px，字不被切）",
               QStringLiteral("弹层 %1 / 最宽一项 %2 / 下拉框 %3")
                   .arg(widePopup != nullptr ? widePopup->width() : -1)
                   .arg(widestRow)
                   .arg(wideCombo->width()));
        wideCombo->hidePopup();
        QApplication::processEvents();

        shortCombo->hidePopup();
        QApplication::processEvents();

        QWidget edgeHost;
        auto *edgeCombo = makeCombo(&edgeHost, 12);
        const QString longOption = QStringLiteral("这是横向空间不足时仍应完整显示的选项");
        const int lastRow = edgeCombo->count() - 1;
        edgeCombo->setItemText(lastRow, longOption);
        edgeCombo->setFixedWidth(60);
        edgeHost.resize(60, 40);
        const QRect screenArea = host.screen()->availableGeometry();
        edgeHost.move(screenArea.right() - edgeHost.width() + 1, screenArea.top() + 50);
        edgeHost.show();
        QApplication::processEvents();
        edgeCombo->showPopup();
        QApplication::processEvents();
        auto *edgeView = edgeCombo->view();
        const QRect lastOption = edgeView->visualRect(edgeCombo->model()->index(lastRow, edgeCombo->modelColumn()));
        const int required = QFontMetrics(Theme::textFont(edgeView->font(), Theme::TextRole::Body))
                                 .horizontalAdvance(longOption) + 24;
        expect(edgeView->isVisible() && screenArea.contains(edgeView->window()->frameGeometry()) &&
                   edgeView->verticalScrollBar()->maximum() > 0 && lastOption.width() >= required,
               "屏幕右侧有滚动条的弹层仍完整显示最长项",
               QStringLiteral("行宽 %1 / 所需 %2 / 弹层右缘 %3 / 屏幕右缘 %4")
                   .arg(lastOption.width()).arg(required).arg(edgeView->window()->frameGeometry().right()).arg(screenArea.right()));
        edgeCombo->hidePopup();
        QApplication::processEvents();

        // 框贴在屏幕底边：下面放不下 ⇒ 翻到框**上方**，弹层底边 = 框顶边 − 4，仍不盖框。
        QWidget bottomHost;
        auto *bottomCombo = makeCombo(&bottomHost, 6);
        bottomCombo->setFixedWidth(160);
        bottomHost.resize(160, 40);
        bottomHost.move(screenArea.left() + 40, screenArea.bottom() - bottomHost.height() + 1);
        bottomHost.show();
        QApplication::processEvents();
        bottomCombo->showPopup();
        QApplication::processEvents();
        if (QWidget *bottomPopup = bottomCombo->view() != nullptr ? bottomCombo->view()->window() : nullptr) {
            const QRect frame = bottomPopup->frameGeometry();
            const QPoint comboTopLeft = bottomCombo->mapToGlobal(QPoint(0, 0));
            expect(qAbs(frame.bottom() + 1 + kPopupGap - comboTopLeft.y()) <= 1 &&
                       qAbs(frame.left() - comboTopLeft.x()) <= 1,
                   "下方放不下才翻到框上方：弹层底边 = 框顶边 − 4、左缘对齐",
                   QStringLiteral("弹层 %1..%2 / 框顶 %3 / 左 %4 vs %5")
                       .arg(frame.top()).arg(frame.bottom()).arg(comboTopLeft.y())
                       .arg(frame.left()).arg(comboTopLeft.x()));
        }
        bottomCombo->hidePopup();
        bottomHost.hide();
        QApplication::processEvents();

        // 长列表：顶上多一条搜索框，键入只留匹配项。
        longCombo->showPopup();
        QApplication::processEvents();
        QAbstractItemView *longView = longCombo->view();
        QWidget *longPopup = longView != nullptr ? longView->window() : nullptr;
        auto *search = longPopup != nullptr
                           ? longPopup->findChild<QLineEdit *>(QStringLiteral("wtfmlPopupSearch"))
                           : nullptr;
        expect(search != nullptr, "长列表（45 项）顶上有一条搜索框");
        // 45 项 > 8 项：弹层里必须能滚（滚轮 / 拖动 / 键盘），露出来的正好 8 行。
        const int longScrollMax =
            longView != nullptr ? longView->verticalScrollBar()->maximum() : 0;
        expect(longView != nullptr && longScrollMax > 0,
               "内容超出 8 项时弹层里能滚（滚轮 / 拖动 / 键盘）",
               QStringLiteral("45 项滚动上限 %1").arg(longScrollMax));
        // 限高按**项数**算：8 项 × 30 + 内边距，再给顶上那条搜索框留出自己那一截。
        const int longHeight = longPopup != nullptr ? longPopup->height() : -1;
        const int designHeight = kPopupRowHeight * 8 + 12;  // 项 8 个 + 上下内边距 6
        expect(longHeight <= designHeight + 48,
               "长列表也只露 8 项（弹层高度压在 8 项 + 搜索框以内，不再整屏铺开）",
               QStringLiteral("弹层 %1 px（8 项配比 %2 px）").arg(longHeight).arg(designHeight));
        if (search != nullptr && longView != nullptr) {
            search->setText(QStringLiteral("选项 4"));
            QApplication::processEvents();
            auto *longList = qobject_cast<QListView *>(longView);
            int visible = 0;
            if (longList != nullptr) {
                for (int i = 0; i < longCombo->count(); ++i) {
                    visible += longList->isRowHidden(i) ? 0 : 1;
                }
            }
            expect(visible > 0 && visible < longCombo->count(),
                   "搜索框键入之后只剩匹配项（跳转不靠滚）",
                   QStringLiteral("剩 %1 项").arg(visible));
            search->clear();
            QApplication::processEvents();
        }
        longCombo->hidePopup();
        QApplication::processEvents();
        host.hide();
    }

    // ---------- ③b 弹层高度**恰好**装下露出来的那几项（2 / 4 / 8 / 45 项） ----------
    // 用户报的是「最后一项被裁掉一截」：125% 下弹层比「项数 × 项高」少 15 物理像素
    // （= 12 逻辑像素），末项下沿压在底部渐隐上。这里量的就是那条线：
    // ① 弹层高度 ≥ 可见项数 × 项高 + 上下内边距（有搜索框再加它那一截）；
    // ② 视图高度 ≥ 可见项数 × 项高；
    // ③ 凡是露在视口里的行都**整行**在里面，不许有半行。
    // 同一段在两个 CTest 条目里跑：`wtfml.popups`（本机 100%）和 `wtfml.popups-scale`
    // （`QT_SCALE_FACTOR=1.25`）—— 用户那台就是这个缩放。
    {
        QWidget host;
        auto *column = new QVBoxLayout(&host);
        host.resize(520, 200);
        host.show();
        QApplication::processEvents();
        const qreal dpr = host.devicePixelRatioF();
        QList<QComboBox *> combos;
        for (int items : {2, 4, 8, 45}) {
            auto *combo = new QComboBox(&host);
            for (int i = 0; i < items; ++i) {
                combo->addItem(QStringLiteral("选项 %1").arg(i + 1));
            }
            column->addWidget(combo);
            combos.append(combo);
        }
        // 先让这一列排完，把 Polish 跑过（`ComboWatcher` 靠 Polish 挂自绘代理和弹层装饰器），
        // 再一个个弹 —— 否则量到的是"还没装装饰器"的裸弹层，测不到内边距那一层。
        QApplication::processEvents();
        for (QComboBox *combo : combos) {
            const int items = combo->count();
            combo->showPopup();
            QApplication::processEvents();
            QAbstractItemView *view = combo->view();
            QWidget *popup = view != nullptr ? view->window() : nullptr;
            const int rows = qMin(items, 8);
            auto *search = popup != nullptr
                               ? popup->findChild<QLineEdit *>(QStringLiteral("wtfmlPopupSearch"))
                               : nullptr;
            const int searchHeight = search != nullptr && search->isVisible() ? search->height() : 0;
            const int needView = rows * kPopupRowHeight;
            const int needPopup = needView + 12 + searchHeight;  // 上下各 6px 内边距

            int clipped = 0;      // 露了一截、下沿被切掉的行
            int fullyVisible = 0; // 整行都在视口里的行
            if (view != nullptr) {
                auto *listView = qobject_cast<QListView *>(view);
                const int viewportHeight = view->viewport()->height();
                for (int i = 0; i < items; ++i) {
                    if (listView != nullptr && listView->isRowHidden(i)) {
                        continue;
                    }
                    const QRect rect = view->visualRect(view->model()->index(i, 0));
                    if (rect.isEmpty() || rect.top() >= viewportHeight) {
                        continue;
                    }
                    if (rect.bottom() >= viewportHeight) {
                        ++clipped;
                    } else {
                        ++fullyVisible;
                    }
                }
            }

            const QString measured =
                QStringLiteral("%1 项：弹层 %2 / 视图 %3 / 视口 %4 / 该有 %5+%6 / DPR %7")
                    .arg(items)
                    .arg(popup != nullptr ? popup->height() : -1)
                    .arg(view != nullptr ? view->height() : -1)
                    .arg(view != nullptr ? view->viewport()->height() : -1)
                    .arg(needPopup)
                    .arg(searchHeight > 0 ? QStringLiteral("（含搜索框 %1）").arg(searchHeight)
                                          : QStringLiteral(""))
                    .arg(dpr);
                   (qobject_cast<QFrame *>(popup) ? qobject_cast<QFrame *>(popup)->frameWidth() : -1), view ? view->maximumHeight() : -1,
            expect(popup != nullptr && popup->height() >= needPopup,
                   "弹层高度装得下露出来的项（项高 30 × 可见项数 + 内边距）", measured);
            expect(view != nullptr && view->height() >= needView,
                   "视图高度装得下露出来的项（每一行都有 30）", measured);
            expect(view != nullptr && clipped == 0 && fullyVisible >= rows,
                   "露在视口里的行都是整行（末项不被裁、不被底部渐隐盖住）",
                   QStringLiteral("%1 项：整行 %2 行 / 被裁 %3 行 / 视口 %4")
                       .arg(items)
                       .arg(fullyVisible)
                       .arg(clipped)
                       .arg(view != nullptr ? view->viewport()->height() : -1));
            combo->hidePopup();
            QApplication::processEvents();
        }
        host.hide();
        QApplication::processEvents();
    }


    // ---------- ③c 弹层窗口：无边框 + 半透明要在 HWND 建出来之前就挂上 ----------
    // 这一条决定真机上四角透不透明：Windows 上 Qt 只给「无边框**且**半透明」的窗口建
    // WS_EX_LAYERED（逐像素 alpha）。下拉弹层是 Qt 自己造的 `QComboBoxPrivateContainer`，
    // 带 Qt::Popup 但不带 FramelessWindowHint；属性要是等到 Show 才挂，窗口早按不透明造成，
    // 四角那块没人画的像素就是黑的（风景底 / 纯色下打开设置就能看见）。
    {
        QWidget host;
        auto *column = new QVBoxLayout(&host);
        host.resize(320, 120);
        host.show();
        auto *combo = new QComboBox(&host);
        for (int i = 0; i < 3; ++i) {
            combo->addItem(QStringLiteral("选项 %1").arg(i + 1));
        }
        column->addWidget(combo);
        QApplication::processEvents();  // 走一遍 Polish：弹层装饰器在那一步挂
        QWidget *popup = combo->view() != nullptr ? combo->view()->window() : nullptr;
        expect(popup != nullptr && popup != combo, "下拉弹层是独立顶层窗口");
        if (popup != nullptr) {
            expect(popup->windowHandle() == nullptr,
                   "断言这一刻弹层的原生窗口还没建出来（属性还来得及）");
            expect(popup->testAttribute(Qt::WA_TranslucentBackground),
                   "弹层窗口在 Show 之前就挂上了半透明（四角才不露黑）");
            expect(popup->windowFlags().testFlag(Qt::FramelessWindowHint),
                   "弹层窗口无边框（无边框 + 半透明才拿得到逐像素 alpha）");
        }
        host.hide();
        QApplication::processEvents();
    }

    // ---------- ③d 禁用下拉外面不画框 ----------
    // 旧白框时代那条 `QComboBox:disabled { border-color: palette(mid) }` 带伪类，
    // 优先级压得过后面那条 3px 透明边 —— 真机上禁用下拉外面就多出一圈灰边框。
    {
        QWidget host;
        auto *column = new QVBoxLayout(&host);
        host.resize(320, 120);
        host.show();
        auto *combo = new QComboBox(&host);
        combo->addItem(QStringLiteral("填充"));
        combo->setEnabled(false);
        column->addWidget(combo);
        QApplication::processEvents();
        const QImage shot = combo->grab().toImage();
        const qreal dpr = shot.devicePixelRatio();
        const QColor fill = shot.pixelColor(shot.width() / 2, shot.height() / 2);
        const QColor edge = shot.pixelColor(shot.width() / 2, qMax(1, qRound(1.5 * dpr)));
        expect(colorDistance(edge, fill) <= 12,
               "禁用下拉外圈和它自己的底色一致（没有那圈灰边框）",
               QStringLiteral("芯 %1 / 边 %2 / 差 %3")
                   .arg(fill.name(), edge.name())
                   .arg(colorDistance(edge, fill)));
        // #337：可用的下拉框外圈有一圈 1px 细边（和不描线的输入框分开），禁用那一档才不画。
        auto *live = new QComboBox(&host);
        live->addItem(QStringLiteral("填充"));
        column->addWidget(live);
        QApplication::processEvents();
        // 垫在页面底上量（底和细边都是半透明的，不垫的话深色那档两者同为白、只差 alpha）。
        // 顶边那一行在 QSS 3px 透明边的最外侧，底只铺到边框中线 ⇒ 那一行底下是页面底：
        // 有细边 = 「stroke 叠在页面底上」那一色；没画边就是页面底本身（所以不拿芯的底色比，那样没边也过）。
        const QColor page = Theme::tokens().page;
        const QImage liveShot = captureWidgetOn(live, page);
        const QColor liveEdge = liveShot.pixelColor(liveShot.width() / 2, 0);
        const QColor edgeInk = overOpaque(Theme::tokens().stroke, page);
        expect(colorDistance(liveEdge, edgeInk) <= 12 && colorDistance(edgeInk, page) > 24,
               "可用下拉框最外一圈有细边（收起态和输入框分得开）",
               QStringLiteral("顶边 %1 该是 %2（页面底 %3）")
                   .arg(liveEdge.name(), edgeInk.name(), page.name()));
        host.hide();
        QApplication::processEvents();
    }

    // ---------- ④ 菜单：同一个外壳 ----------
    {
        QMenu menu;
        menu.addAction(QStringLiteral("打开实例文件夹"));
        menu.addAction(QStringLiteral("复制路径"));
        menu.show();
        QApplication::processEvents();
        expect(menu.testAttribute(Qt::WA_TranslucentBackground),
               "菜单是透明底的（底由那层磨砂玻璃画）");
        expect(Theme::widgetStyleSheet().contains(
                   QLatin1String("QMenu { background-color: transparent;")),
               "菜单样式表里不再铺不透明底（否则会把玻璃整块盖掉）");
        menu.hide();
        QApplication::processEvents();
    }

    // ---------- ⑤ 样式表里那几条规格还在 ----------
    {
        const QString sheet = Theme::widgetStyleSheet();
        expect(sheet.contains(QLatin1String("QComboBox QAbstractItemView { background-color: transparent;")),
               "下拉弹层那张表不再自己铺不透明底（玻璃由弹层窗口画）");
        expect(!sheet.contains(QLatin1String("#ffffe1")),
               "样式表里没有系统 tooltip 那个黄底");
    }

    // ---------- ⑥ 验收图：深色 + 青瓷各一遍（`grab()` 拍不到 DWM 合成那层，图里是自绘玻璃） ----------
    if (!shots.isEmpty()) {
        for (const QString &preset : {QStringLiteral("dark"), QStringLiteral("emerald")}) {
            Theme::setSpec(Theme::presetSpec(preset));
            Theme::apply(app);
            QApplication::processEvents();

            QWidget host;
            host.resize(220, 90);
            host.show();
            QApplication::processEvents();
            Popups::showTooltip(QStringLiteral("打开实例文件夹"), host.mapToGlobal(QPoint(30, 30)), nullptr);
            QApplication::processEvents();
            for (QWidget *widget : QApplication::allWidgets()) {
                if (widget->objectName() == QStringLiteral("wtfmlTooltip") && widget->isVisible()) {
                    expect(saveShot(widget, QDir(shots).filePath(preset + QStringLiteral("-tooltip.png"))),
                           QStringLiteral("%1 tooltip 验收图写出来了").arg(preset));
                    break;
                }
            }
            Popups::hideTooltip();

            auto *shortCombo = new QComboBox(&host);
            for (int i = 0; i < 5; ++i) {
                shortCombo->addItem(QStringLiteral("选项 %1").arg(i + 1));
            }
            shortCombo->setGeometry(10, 10, 200, 32);
            shortCombo->show();  // 先 show 才会走 Polish，装饰器（弹层外壳）是在那一步装的
            shortCombo->showPopup();
            QApplication::processEvents();
            if (QWidget *popup = shortCombo->view() != nullptr ? shortCombo->view()->window() : nullptr) {
                expect(saveShot(popup, QDir(shots).filePath(preset + QStringLiteral("-dropdown-short.png"))),
                       QStringLiteral("%1 短列表验收图写出来了").arg(preset));
            }
            shortCombo->hidePopup();

            auto *longCombo = new QComboBox(&host);
            for (int i = 0; i < 45; ++i) {
                longCombo->addItem(QStringLiteral("选项 %1").arg(i + 1));
            }
            longCombo->setGeometry(10, 10, 200, 32);
            longCombo->show();  // 先 show 才会走 Polish，装饰器（弹层外壳）是在那一步装的
            longCombo->showPopup();
            QApplication::processEvents();
            if (QWidget *popup = longCombo->view() != nullptr ? longCombo->view()->window() : nullptr) {
                expect(popup->findChild<QLineEdit *>(QStringLiteral("wtfmlPopupSearch")) != nullptr,
                       QStringLiteral("%1 长列表顶上那条搜索框在").arg(preset));
                expect(saveShot(popup, QDir(shots).filePath(preset + QStringLiteral("-dropdown-long.png"))),
                       QStringLiteral("%1 长列表 + 搜索验收图写出来了").arg(preset));
                // 只看细条那一档：把透明度钉到 1 再拍一张（S2 的常态是 0，屏幕上要滚才露）
                if (auto *bar = longCombo->view()->verticalScrollBar()) {
                    if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(bar->graphicsEffect())) {
                        effect->setOpacity(1.0);
                        QApplication::processEvents();
                        expect(saveShot(popup, QDir(shots).filePath(
                                            preset + QStringLiteral("-dropdown-thumb.png"))),
                               QStringLiteral("%1 细条（3px）验收图写出来了").arg(preset));
                        effect->setOpacity(0.0);
                    }
                }
            }
            longCombo->hidePopup();

            QMenu menu;
            menu.addAction(QStringLiteral("打开实例文件夹"));
            menu.addAction(QStringLiteral("复制路径"));
            menu.addSeparator();
            menu.addAction(QStringLiteral("删除实例"));
            menu.show();
            QApplication::processEvents();
            expect(saveShot(&menu, QDir(shots).filePath(preset + QStringLiteral("-menu.png"))),
                   QStringLiteral("%1 菜单验收图写出来了").arg(preset));
            menu.hide();
            host.hide();
            QApplication::processEvents();
        }
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
    }

    // ---------- ⑦ 防漏：Qt 自建的几种弹层也都是无边框 + 半透明 ----------
    // 第三轮反馈第 4 条（圆角面外套直角黑框）。每块浮层上屏时进程级的 PopupWindowGuard 还会再验一遍。
    {
        QWidget host;
        host.resize(320, 120);
        host.show();
        Popups::showTooltip(QStringLiteral("防漏检查"), host.mapToGlobal(QPoint(20, 20)), nullptr);
        auto *probe = new QComboBox(&host);
        probe->addItems({QStringLiteral("一"), QStringLiteral("二")});
        probe->show();
        probe->showPopup();
        QApplication::processEvents();
        const QWidget *comboPopup = probe->view()->window();
        const QString comboProblem = popupWindowProblem(comboPopup);
        probe->hidePopup();
        QMenu probeMenu;
        probeMenu.addAction(QStringLiteral("一项"));
        probeMenu.show();
        QApplication::processEvents();
        const QString menuProblem = popupWindowProblem(&probeMenu);
        probeMenu.hide();
        // 可编辑下拉框的补全列表（实例设置「目标版本」、更新实例那两处）。
        auto *editable = new QComboBox(&host);
        editable->setEditable(true);
        editable->addItems({QStringLiteral("1.20.1"), QStringLiteral("1.20.4"), QStringLiteral("1.21")});
        editable->completer()->setCompletionMode(QCompleter::PopupCompletion);
        editable->show();
        editable->completer()->setCompletionPrefix(QStringLiteral("1.2"));
        editable->completer()->complete();
        QApplication::processEvents();
        QWidget *completerPopup = editable->completer()->popup();
        const QString completerProblem = completerPopup->isVisible() ? popupWindowProblem(completerPopup)
                                                                     : QStringLiteral("补全列表没弹出来");
        completerPopup->hide();
        QString tipProblem = QStringLiteral("tooltip 窗口没找到");
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("wtfmlTooltip")) tipProblem = popupWindowProblem(widget);
        }
        expect(tipProblem.isEmpty() && comboProblem.isEmpty() && menuProblem.isEmpty() &&
                   completerProblem.isEmpty(),
               "tooltip / 下拉弹层 / 菜单 / 补全列表都是无边框 + 半透明",
               QStringList({tipProblem, comboProblem, menuProblem, completerProblem}).join(QLatin1Char(' ')));
        Popups::hideTooltip();
        host.hide();
        QApplication::processEvents();
    }

    printf("%s %d 项失败\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}



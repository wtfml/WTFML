#include "Support.hpp"
#include "MainWindowPageLayout.hpp"

#include <QKeyEvent>
#include <QPushButton>
// Z11：自绘外框 + 左导航轨（ui/TitleBar）。这里验接线与摆位：轨在左边、宽 208 / 折叠 64、
// 启动先是图标轨、点品牌标 W 收展（155）、窄窗（< 1024）强制折、右栏顶上那条 40 高的窗口按钮条（最小化 / 最大化 / 关闭）、
// 导航的颗数与字、点下去发什么信号、品牌字。"系统那条白条到底去掉了没有"是 Win32 那一层
// 的事（ui/WindowChrome），offscreen 里只验它认领哪些原生消息。
int UiTitleBarChecks(QMainWindow &window, MainWindowUI &ui) {
    TitleBar *rail = ui.titleBar();
    WindowChrome *chrome = ui.chrome();
    check(rail != nullptr && chrome != nullptr, "Z11 导航轨、外框挂件都搭出来了");
    if (rail == nullptr || chrome == nullptr) return 0;
    // 拖窗区两条：右上那条窗口按钮条（主条）+ 导航轨的空白处（addDragStrip）。
    check(chrome->strip() == rail->captionStrip(), "Z11 外框认的主拖窗条就是右上那条窗口按钮条");
    check((window.windowFlags() & Qt::WindowMaximizeButtonHint) == 0,
          "★ UI问题 #2 主窗口不给最大化（没有 WS_MAXIMIZEBOX：Win+↑、拖到屏顶、双击标题栏都最大化不了）");
    check(window.findChild<QWidget *>(QStringLiteral("titleBarMaximize")) == nullptr,
          "★ UI问题 #2 标题栏上没有最大化按钮");
    check(window.minimumSize() == QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight) &&
              window.minimumSize() == QSize(900, 590),
          "★ 155 主窗口最小 900×590",
          QStringLiteral("%1x%2").arg(window.minimumWidth()).arg(window.minimumHeight()));
    {
        TitleBar fresh;
        check(fresh.isCollapsed() && fresh.width() == TitleBar::collapsedRailWidth(),
              "★ 155 导航轨一建好就是图标轨（每次启动都收起）", QString::number(fresh.width()));
    }

    // ---------- 宽窗：轨展开 208，贴在左边整高 ----------
    window.resize(1280, 800);
    rail->setCollapsed(false);
    QApplication::processEvents();

    // ---------- 手写体只留四处（tokens-r2 §1）：用了手写体的控件都得挂 wtfmlDisplay ----------
    {
        const QStringList hand{Theme::handFamily(Theme::HandFont::Kai), Theme::handFamily(Theme::HandFont::KaiLight),
                               Theme::handFamily(Theme::HandFont::Script)};
        const QByteArray mark = Theme::displayPropertyName().toUtf8();
        QStringList strays;
        int marked = 0;
        for (QWidget *w : window.findChildren<QWidget *>()) {
            if (qobject_cast<QLabel *>(w) == nullptr && qobject_cast<QAbstractButton *>(w) == nullptr) continue;
            w->ensurePolished();
            const QStringList families = w->font().families();
            if (families.isEmpty() || hand.first().isEmpty() || !hand.contains(families.first())) continue;
            if (w->property(mark.constData()).toBool()) { ++marked; continue; }
            const auto *label = qobject_cast<QLabel *>(w);
            const auto *button = qobject_cast<QAbstractButton *>(w);
            strays << QStringLiteral("%1「%2」").arg(w->objectName(), label != nullptr ? label->text() : button->text());
        }
        check(strays.isEmpty() && marked > 0, "★ r2 手写体只在品牌 / 主页英雄区 / 页标题 / 空状态（其余控件都是系统字）",
              QStringLiteral("标了 %1 处；漏网：%2").arg(marked).arg(strays.mid(0, 6).join(QStringLiteral("、"))));
    }
    check(!rail->isCollapsed() && rail->width() == TitleBar::railWidth() && TitleBar::railWidth() == 208,
          "★ Z11 宽窗下轨展开、宽 208",
          QStringLiteral("轨 %1 / 窗 %2").arg(rail->width()).arg(window.width()));
    check(rail->x() == 0 && rail->y() == 0 && rail->height() == window.centralWidget()->height(),
          "Z11 轨贴在左边、上下顶满",
          QStringLiteral("%1,%2 %3 / %4").arg(rail->x()).arg(rail->y()).arg(rail->height())
              .arg(window.centralWidget()->height()));

    // ---------- 右栏顶上那条窗口按钮条 ----------
    QWidget *caption = rail->captionStrip();
    QAbstractButton *minBtn = rail->minimizeButton();
    QAbstractButton *closeBtn = rail->closeButton();
    check(caption != nullptr && caption->height() == TitleBar::barHeight() && TitleBar::barHeight() == 40,
          "★ Z11 顶部拖动区高 40", caption != nullptr ? QString::number(caption->height()) : QString());
    check(minBtn && closeBtn, "Z11 最小化 / 关闭两颗都找得到");
    if (caption && minBtn && closeBtn) {
        const QSize cell(TitleBar::windowButtonWidth(), TitleBar::buttonSize());
        check(minBtn->size() == cell && closeBtn->size() == cell && cell == QSize(46, 40),
              "★ Z11 两颗窗口按钮都是 46×40");
        const QPoint closeTopRight = closeBtn->mapTo(&window, closeBtn->rect().topRight());
        check(closeTopRight.y() == 0 && closeTopRight.x() == window.width() - 1,
              "★ Z11 关闭那颗贴在窗口右上角",
              QStringLiteral("%1,%2 / 窗宽 %3").arg(closeTopRight.x()).arg(closeTopRight.y()).arg(window.width()));
        check(minBtn->geometry().right() + 1 == closeBtn->x(), "Z11 最小化、关闭从左到右一个挨一个");
        check(minBtn->focusPolicy() == Qt::NoFocus && closeBtn->focusPolicy() == Qt::NoFocus,
              "Z11 窗口按钮不抢键盘焦点");
        check(caption->mapTo(&window, QPoint(0, 0)).x() == rail->width(),
              "Z11 按钮条摆在右栏最上面（从轨的右边开始）");
    }

    // ---------- 导航：颗数 / 字 / 摆位 / 点击 ----------
    QTabWidget *tabs = ui.getTabWidget();
    check(tabs != nullptr && !tabs->tabBar()->isVisible(), "★ Z11 标签页自己那条标签栏藏起来了（导航在轨上）");
    check(TitleBar::defaultBrandText() == QStringLiteral("WTFML"),
          "★ Z11 品牌固定写那五个字母（UI 问题 #3：不给自定义）",
          TitleBar::defaultBrandText());
    // 五个功能 = 五颗导航；第六个选项卡「工具箱」不上导航（入口是导航轨底部那颗）。
    const int tabCount = tabs != nullptr ? tabs->count() - 1 : -1;
    check(tabCount == 5 && tabs->count() == MainWindowUI::toolboxTabIndex() + 1 && rail->navCount() == tabCount,
          "★ Z11 五个功能 = 五颗导航，工具箱是第六个选项卡不上导航",
          QStringLiteral("%1 / %2").arg(tabCount).arg(rail->navCount()));
    if (tabs != nullptr && tabCount == rail->navCount() && tabCount > 0) {
        bool labelsOk = true, fontOk = true, sizeOk = true, orderOk = true;
        for (int i = 0; i < tabCount; ++i) {
            QAbstractButton *button = rail->navButton(i);
            labelsOk = labelsOk && button->text() == tabs->tabText(i);
            fontOk = fontOk && button->font().pixelSize() == TitleBar::navFontPixelSize();
            sizeOk = sizeOk && button->size() == QSize(TitleBar::navItemWidth(), TitleBar::navItemHeight());
            orderOk = orderOk && (i == 0 || button->y() > rail->navButton(i - 1)->geometry().bottom());
        }
        check(labelsOk, "★ Z11 导航的字就是标签页的字（唯一出处 tabText()）");
        check(fontOk && TitleBar::navFontPixelSize() == 14, "★ Z11 导航字号 14（系统字）");
        check(sizeOk && TitleBar::navItemHeight() == 42, "★ Z11 每颗导航 navItemWidth()×42",
              QStringLiteral("%1x%2").arg(rail->navButton(0)->width()).arg(rail->navButton(0)->height()));
        check(orderOk, "Z11 导航从上到下排、不叠");

        int lastNav = -1;
        const auto spy = QObject::connect(rail, &TitleBar::navRequested, rail, [&lastNav](int i) { lastNav = i; });
        const int before = tabs->currentIndex();
        const int target = (before + 1) % tabCount;
        rail->navButton(target)->click();
        QApplication::processEvents();
        QObject::disconnect(spy);
        check(lastNav == target, "Z11 点第几颗就发 navRequested(几)", QString::number(lastNav));
        check(tabs->currentIndex() == target && rail->currentNav() == target,
              "★ Z11 点了真的翻页，当前那一颗跟着走");
        tabs->setCurrentIndex(before);
        QApplication::processEvents();
    }

    // ---------- 折叠：点品牌标 W 收展（155），窄窗强制折、点 W 不动 ----------
    int lastCollapsed = -1;
    const auto collapseSpy = QObject::connect(rail, &TitleBar::collapsedChanged, rail,
                                              [&lastCollapsed](bool c) { lastCollapsed = c ? 1 : 0; });
    QAbstractButton *logo = rail->logoButton();
    check(window.findChild<QWidget *>(QStringLiteral("titleBarFold")) == nullptr && logo != nullptr &&
              logo->objectName() == QStringLiteral("titleBarLogo"),
          "★ 155 品牌旁的收起钮删了，收展只走品牌标 W");
    if (logo != nullptr) {
        check(logo->isVisible() && logo->cursor().shape() == Qt::PointingHandCursor &&
                  logo->toolTip() == QStringLiteral("收起导航") && logo->accessibleName() == QStringLiteral("收起导航") &&
                  logo->focusPolicy() == Qt::TabFocus,
              "★ 155 展开时 W：手型光标、提示与无障碍名都是「收起导航」、能 Tab 聚焦",
              QStringLiteral("提示「%1」名「%2」光标 %3 焦点 %4").arg(logo->toolTip(), logo->accessibleName())
                  .arg(int(logo->cursor().shape())).arg(int(logo->focusPolicy())));
        logo->click();
        QApplication::processEvents();
        check(rail->isCollapsed() && rail->width() == TitleBar::collapsedRailWidth() && TitleBar::collapsedRailWidth() == 64,
              "★ 155 点 W 折成 64 的图标轨", QString::number(rail->width()));
        check(lastCollapsed == 1, "Z11 折叠时发 collapsedChanged(true)");
        check(logo->toolTip() == QStringLiteral("展开导航") && logo->accessibleName() == QStringLiteral("展开导航") &&
                  logo->cursor().shape() == Qt::PointingHandCursor,
              "★ 155 收起时 W 提示「展开导航」", logo->toolTip());
        // ---- 焦点框：窗口激活时 Qt 自动把焦点落到 W（链上第一个能 Tab 的）——不画框、不接键 ----
        window.activateWindow();
        QApplication::processEvents();
        logo->clearFocus();
        QApplication::processEvents();
        const QImage noFocusShot = captureWidget(logo);
        for (const Qt::FocusReason reason : {Qt::ActiveWindowFocusReason, Qt::MouseFocusReason, Qt::OtherFocusReason}) {
            logo->setFocus(reason);
            QApplication::processEvents();
            check(logo->hasFocus() && !static_cast<TitleBarButton *>(logo)->hasKeyboardFocus() &&
                      captureWidget(logo) == noFocusShot,
                  QStringLiteral("★ 155 窗口激活 / 鼠标给的焦点（原因 %1）：W 有焦点但不画框").arg(int(reason)),
                  QStringLiteral("有焦点 %1").arg(logo->hasFocus()));
            QKeyEvent stray(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QKeyEvent strayUp(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
            QKeyEvent strayDown(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QApplication::sendEvent(logo, &stray);
            QApplication::sendEvent(logo, &strayDown);
            QApplication::sendEvent(logo, &strayUp);
            QApplication::processEvents();
            check(rail->isCollapsed(), QStringLiteral("★ 155 自动落上来的焦点（原因 %1）：回车 / 空格不收放导航").arg(int(reason)));
        }
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(logo, &tab);
        QApplication::processEvents();
        check(logo->hasFocus() && static_cast<TitleBarButton *>(logo)->hasKeyboardFocus() &&
                  captureWidget(logo) != noFocusShot,
              "★ 155 焦点自动落在 W 上时按 Tab：留在 W、亮出焦点下划线");
        logo->clearFocus();
        QApplication::processEvents();
        check(!static_cast<TitleBarButton *>(logo)->hasKeyboardFocus() && captureWidget(logo) == noFocusShot,
              "155 W 失去焦点：下划线收掉");
        logo->setFocus(Qt::TabFocusReason);
        QApplication::processEvents();
        check(logo->hasFocus() && static_cast<TitleBarButton *>(logo)->hasKeyboardFocus() &&
                  captureWidget(logo) != noFocusShot,
              "★ 155 用 Tab 选中 W：画焦点下划线");
        {
            // UI问题 #16：焦点提示只在 W 底下那一笔，不再框住 W —— 上半格和左右两列与没焦点时一模一样。
            const QImage focused = captureWidget(logo);
            const int h = focused.height(), w = focused.width();
            const bool topSame = focused.copy(0, 0, w, h / 2) == noFocusShot.copy(0, 0, w, h / 2);
            const bool sidesSame = focused.copy(0, 0, 1, h) == noFocusShot.copy(0, 0, 1, h) &&
                                   focused.copy(w - 1, 0, 1, h) == noFocusShot.copy(w - 1, 0, 1, h);
            check(topSame && sidesSame, "UI问题 #16 W 的焦点提示不是框：上半格、左右边缘不变");
        }
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(logo, &enter);
        QApplication::processEvents();
        check(!rail->isCollapsed() && rail->width() == TitleBar::railWidth(), "★ 155 W 上按回车展开回 208",
              QString::number(rail->width()));
        QKeyEvent spaceDown(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent spaceUp(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(logo, &spaceDown);
        QApplication::sendEvent(logo, &spaceUp);
        QApplication::processEvents();
        check(rail->isCollapsed(), "★ 155 W 上按空格再收起");
        logo->click();
        QApplication::processEvents();
        check(!rail->isCollapsed() && rail->width() == TitleBar::railWidth(), "Z11 再点 W 展开回 208");
    }

    window.resize(1000, 700);
    QApplication::processEvents();
    check(TitleBar::narrowBreakpoint() == 1024 && rail->isCollapsed() && rail->width() == TitleBar::collapsedRailWidth(),
          "★ Z11 窗口宽 < 1024 时轨自动折叠", QString::number(rail->width()));
    if (logo != nullptr) {
        logo->click();
        QApplication::processEvents();
        check(rail->isCollapsed() && rail->width() == TitleBar::collapsedRailWidth() &&
                  logo->toolTip() == QStringLiteral("窗口加宽后才能展开") && logo->cursor().shape() == Qt::ArrowCursor,
              "★ 155 窄窗点 W 不展开，提示「窗口加宽后才能展开」",
              QStringLiteral("轨 %1 提示「%2」").arg(rail->width()).arg(logo->toolTip()));
    }
    window.resize(1280, 800);
    QApplication::processEvents();
    check(!rail->isCollapsed() && rail->width() == TitleBar::railWidth(), "★ Z11 窗口放宽回去轨自动展开");
    QObject::disconnect(collapseSpy);

    // ---------- W 的焦点下划线判据：理由是 Tab 还不够，还要最后一次输入是键盘 ----------
    // 鼠标关浮层 / 换页时 Qt 把焦点按 TabFocusReason 交给链上下一个（W 排在后面就接到），没人按过 Tab。
    {
        QWidget host;
        host.resize(420, 240);
        auto *overlay = new QPushButton(QStringLiteral("浮层"), &host);
        auto *probeRail = new TitleBar(&host);
        host.show();
        host.activateWindow();
        QApplication::processEvents();
        auto *probeLogo = static_cast<TitleBarButton *>(probeRail->logoButton());
        int logoClicks = 0;
        QObject::connect(probeLogo, &QAbstractButton::clicked, probeLogo, [&logoClicks] { ++logoClicks; });
        Qt::FocusReason landedReason = Qt::OtherFocusReason;
        struct ReasonProbe : QObject {
            Qt::FocusReason *out;
            bool eventFilter(QObject *, QEvent *e) override {
                if (e->type() == QEvent::FocusIn) *out = static_cast<QFocusEvent *>(e)->reason();
                return false;
            }
        } reasonProbe;
        reasonProbe.out = &landedReason;
        probeLogo->installEventFilter(&reasonProbe);
        const auto press = [](QWidget *target, Qt::Key key) {
            QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
            QApplication::sendEvent(target, &down);
            QApplication::processEvents();
        };
        probeLogo->clearFocus();
        QApplication::processEvents();
        const QImage plain = captureWidget(probeLogo);

        // ① 鼠标点过浮层，再把它藏起来：焦点交给 W（理由 Tab），不画下划线、回车也不收放。
        overlay->setFocusPolicy(Qt::StrongFocus);
        QMouseEvent click(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(overlay->mapToGlobal(QPoint(5, 5))),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(overlay, &click);
        overlay->setFocus(Qt::MouseFocusReason);
        QApplication::processEvents();
        overlay->hide();
        QApplication::processEvents();
        check(probeLogo->hasFocus() &&
                  (landedReason == Qt::TabFocusReason || landedReason == Qt::BacktabFocusReason),
              "★ 藏起有焦点的控件后 Qt 把焦点按 Tab 理由交给 W（复现前提）",
              QStringLiteral("有焦点 %1 理由 %2").arg(probeLogo->hasFocus()).arg(int(landedReason)));
        check(!probeLogo->hasKeyboardFocus() && captureWidget(probeLogo) == plain,
              "★ 鼠标关浮层 / 换页把焦点交给 W：不画下划线");
        press(probeLogo, Qt::Key_Return);
        check(logoClicks == 0 && !probeLogo->hasKeyboardFocus(),
              "★ 交接来的焦点按回车不收放导航，也不因按键点亮下划线");

        // ② 真按 Tab 从前一个控件选到 W：画下划线，回车收放。
        probeLogo->clearFocus();
        overlay->show();
        overlay->setFocus(Qt::MouseFocusReason);
        QApplication::processEvents();
        press(overlay, Qt::Key_Tab);
        check(probeLogo->hasFocus() && probeLogo->hasKeyboardFocus() && captureWidget(probeLogo) != plain,
              "★ 真按 Tab 选到 W：画下划线");
        press(probeLogo, Qt::Key_Return);
        check(logoClicks == 1, "★ 真按 Tab 选到 W 后回车收放导航");

        // ③ 窗口激活自动落上来的焦点：回车不收放（过滤器此时已因按键把属性置真，守卫不能读它）。
        probeLogo->clearFocus();
        probeLogo->setFocus(Qt::ActiveWindowFocusReason);
        QApplication::processEvents();
        press(probeLogo, Qt::Key_Return);
        check(probeLogo->hasFocus() && logoClicks == 1 && !probeLogo->hasKeyboardFocus(),
              "★ 窗口激活后直接按回车不收放导航");
    }

    // ---------- 窗口按钮的信号（单独一份轨，不去真的收主窗口） ----------
    {
        TitleBar probe;
        int minimized = 0, closed = 0;
        QObject::connect(&probe, &TitleBar::minimizeRequested, [&] { ++minimized; });
        QObject::connect(&probe, &TitleBar::closeRequested, [&] { ++closed; });
        probe.minimizeButton()->click();
        check(minimized == 1 && closed == 0, "Z11 点「—」只发最小化那一条");
        probe.closeButton()->click();
        check(minimized == 1 && closed == 1, "Z11 点「×」只发关闭那一条");
    }

    // ---------- 品牌字：固定是商标，跟窗口标题无关、也不给改（UI 问题 #3） ----------
    // 条上那行字没有"现在写着什么"的 getter 了（牌子固定），所以这里量**画出来的像素**：
    // 换窗口标题前后一模一样，说明它不从窗口标题、也不从任何设置里来。
    const QImage brandShot = captureWidget(rail);
    const QString longTitle(300, QLatin1Char('W'));
    window.setWindowTitle(longTitle);
    QApplication::processEvents();
    check(captureWidget(rail) == brandShot && rail->width() == TitleBar::railWidth(),
          "★ Z11 窗口标题再长，轨上照旧那五个字母、轨宽不变");
    window.setWindowTitle(QStringLiteral("WTF Minecraft Launcher"));
    QApplication::processEvents();
    check(captureWidget(rail) == brandShot, "Z11 品牌那行字固定画商标，换窗口标题也不变");

    // ---------- 外框认领的原生消息（系统背板那一版只认 WM_NCHITTEST，非客户区交给系统） ----------
    qintptr ncResult = -1;
    check(!chromeHandlesNcCalcsize(chrome, &ncResult), "Z11 WM_NCCALCSIZE 交给系统（系统背板要它）");
    qintptr otherResult = -1;
    check(!chromeHandlesPlainMessage(chrome, chromeLeftButtonDownMessage(), &otherResult),
          "Z11 别的原生消息一个字都不碰（只认 WM_NCHITTEST）");
    // UI问题 #2：主窗口自己拖窗，键盘贴靠（带 0x80000 标志位、改大小）被按住；别的改大小一律放行。
    {
        qintptr ignored = 0;
        const unsigned hold = chromeSwpNoSizeNoMove();
        check(chrome->ownMove(), "★ UI问题 #2 主窗口的外框是自己拖窗模式");
        check((chromeWindowPosChangingFlags(chrome, 0x180040) & hold) == hold,
              "★ UI问题 #2 键盘贴靠的改大小（标志位带 0x80000）被按住：大小和位置都不动");
        check(chromeWindowPosChangingFlags(chrome, 0x14) == 0x14 && chromeWindowPosChangingFlags(chrome, 0x1001837) == 0x1001837,
              "UI问题 #2 Qt 自己的 resize / 显示（没有 0x80000）原样放行");
        chromeHandlesPlainMessage(chrome, chromeEnterSizeMoveMessage(), &ignored);
        check(chromeWindowPosChangingFlags(chrome, 0x180040) == 0x180040,
              "UI问题 #2 拖边缩放的模态循环里不拦（拖边照旧能用）");
        chromeHandlesPlainMessage(chrome, chromeExitSizeMoveMessage(), &ignored);
        check((chromeWindowPosChangingFlags(chrome, 0x180040) & hold) == hold, "UI问题 #2 缩放循环结束后又拦");
    }
    {
        GameLogWindow log(QStringLiteral("例外那一扇"));
        check((log.windowFlags() & Qt::FramelessWindowHint) == 0, "★ Z11 「游戏日志」窗照旧带系统标题栏");
    }

    check(window.isVisible(), "Z11 前提：主窗口这会儿还开着");
    if (closeBtn != nullptr) {
        closeBtn->click();
        QApplication::processEvents();
    }
    check(!window.isVisible(), "★ Z11 主窗口上那颗「×」接在窗口上（点它窗口真的关）");
    return 0;
}

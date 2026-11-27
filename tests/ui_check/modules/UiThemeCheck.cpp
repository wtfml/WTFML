#include "Support.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "Glass.hpp"
#include "InlineMessage.hpp"
#include "TitleBar.hpp"
#include "ItemViewContextMenu.hpp"
#include "ListRowDelegate.hpp"
#include "MotdText.hpp"
#include <QMouseEvent>
#include <QToolButton>
#include <QSpinBox>
#include <QTreeWidget>
int UiThemeCheck(QApplication &app) {

        Anim::setEnabled(false);
        QTemporaryDir settingsDir(QDir::current().filePath(QStringLiteral(".theme-check-XXXXXX")));
        if (!settingsDir.isValid()) return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        app.setOrganizationName(QStringLiteral("WTFMLThemeCheck"));
        app.setApplicationName(QStringLiteral("ThemeCheck"));
        app.setStyle(Theme::createStyle());
        const QString fontPath = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
        const int fontId = QFile::exists(fontPath) ? QFontDatabase::addApplicationFont(fontPath) : -1;
        const QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
        app.setFont(QFont(fontFamilies.isEmpty() ? QStringLiteral("Microsoft YaHei UI") : fontFamilies.first(), 9));
        int failures = 0;
        auto expect = [&failures](bool ok, const char *name) {
            printf("%s %s\n", ok ? "PASS" : "FAIL", name);
            if (!ok) ++failures;
        };
        // `Theme::save()` 在「没有基线」那一支只清**主题自己的键**。Glass 的材质 / 着色 / 模糊
        // 也在 `Theme/` 下（`Theme/Backdrop`、`Theme/BackdropTint`、`Theme/BackdropBlur`，
        // 见 ui/Glass.cpp），整组删会把用户挑的那三项抹回出厂。
        {
            // 换一个应用名 = 换一份配置文件 ⇒ 这一支必然按「没有基线」走（不看本进程之前 load 过没有）。
            app.setApplicationName(QStringLiteral("ThemeSaveCheck"));
            {
                QSettings glass;
                glass.setValue(QStringLiteral("Theme/Backdrop"), QStringLiteral("mica"));
                glass.setValue(QStringLiteral("Theme/BackdropTint"), 37);
                glass.setValue(QStringLiteral("Theme/BackdropBlur"), 61);
                // 主题自己的旧键：没基线那一支要清掉（「上一次设过、这一次没设」的项不许复活）。
                glass.setValue(QStringLiteral("Theme/Parts/card/Image"), QStringLiteral("C:/old.png"));
                glass.sync();
            }
            Theme::save(Theme::presetSpec(QStringLiteral("dark")));
            QSettings after;
            expect(after.value(QStringLiteral("Theme/Backdrop")).toString() == QStringLiteral("mica") &&
                       after.value(QStringLiteral("Theme/BackdropTint")).toInt() == 37 &&
                       after.value(QStringLiteral("Theme/BackdropBlur")).toInt() == 61,
                   "Theme::save() 没有基线时一根手指都不碰 Glass 的材质三项（材质 / 着色 / 模糊）");
            expect(after.value(QStringLiteral("Theme/Preset")).toString() == QStringLiteral("dark"),
                   "…主题自己的键照旧写回去");
            expect(!after.contains(QStringLiteral("Theme/Parts/card/Image")),
                   "…上一次设过、这一次没设的主题项照旧清掉");
            app.setApplicationName(QStringLiteral("ThemeCheck"));
        }
        // 出厂材质：盘上没写过 `Theme/Backdrop` 时用风景底（≤ 2 线程的机器、旧版关过磨砂的是纯色）。
        expect(Glass::factoryMode(8, false) == Glass::Mode::Scene &&
                   Glass::factoryMode(2, false) == Glass::Mode::Solid &&
                   Glass::factoryMode(8, true) == Glass::Mode::Solid,
               "factory backdrop is the scene; few-thread machines and old 'glass off' stay solid");
        // `enabled()` 的四档判据现在缓存起来了（每次重绘都要问它，原来每次都读注册表 +
        // 问系统）。写设置之后**同一帧里**就得反映新值，不能等某个事件来清缓存。
        {
            const Glass::Mode before = Glass::mode();
            // 系统那三位（高对比度 / 透明效果 / 系统背板）在测试机上什么样都有可能，先钉死。
            Glass::setSystemFlagsForTest(false, false, true);
            Glass::setMode(Glass::Mode::Solid);
            expect(!Glass::enabled(), "写 solid 之后 enabled() 立刻是 false（写路径清了缓存）");
            Glass::setMode(Glass::Mode::Scene);
            expect(Glass::enabled(), "写 scene 之后 enabled() 立刻是 true");
            // 绕过 setMode、直接往盘上写（外部改设置）那条：用公开的失效入口。
            QSettings().setValue(Glass::settingsKey(), Glass::modeKey(Glass::Mode::Solid));
            Glass::invalidateSettingsCache();
            expect(!Glass::enabled(), "直接写盘 + invalidateSettingsCache() 之后立刻生效");
            Glass::setMode(before);
            Glass::clearSystemFlagsForTest();
            expect(Glass::enabled() == (Glass::effectiveMode() != Glass::Mode::Solid),
                   "撤掉测试用的系统标志之后回到现读系统（判据一致）");
        }
        // #143：换过出厂主色之后的一次性迁移。
        {
            app.setApplicationName(QStringLiteral("ThemeAccentCheck"));  // 单独一份配置文件
            const auto loadAccent = [](const char *preset, const char *accent) {
                QSettings disk;
                disk.setValue(QStringLiteral("Theme/Preset"), QLatin1String(preset));
                disk.setValue(QStringLiteral("Theme/Accent"), QLatin1String(accent));
                disk.sync();
                return Theme::load().accent.rgba();
            };
            const QRgb oldEmerald = 0xff087a68;
            const QRgb factory = Theme::tokens(QStringLiteral("emerald")).primary.rgba();
            QSettings().clear();
            expect(loadAccent("emerald", "#087a68") == factory &&
                       QColor::fromString(QSettings().value(QStringLiteral("Theme/Accent")).toString())
                               .rgba() == factory,
                   "冰川碧湖存着旧出厂主色：读回来是现在的默认主色，盘上也改写了");
            expect(loadAccent("emerald", "#ff087a68") == oldEmerald, "只迁一次：迁过之后再选回旧色就保留");
            QSettings().clear();
            expect(loadAccent("emerald", "#ff336699") == 0xff336699, "自己挑的别的颜色不动");
            QSettings().clear();
            expect(loadAccent("ocean", "#ff087a68") == oldEmerald, "别的预设存着这个值（不是它的旧出厂值）不动");
            QSettings().clear();
            app.setApplicationName(QStringLiteral("ThemeCheck"));
        }
        // #143：挂 `background: transparent` 的列表（实例设置左栏）`Base` 是全透明黑，
        // 自定义主题色下浅色界面的选中行仍须是深字。
        {
            const Theme::Spec savedSpec = Theme::current();
            Theme::Spec custom = Theme::presetSpec(QStringLiteral("emerald"));
            custom.accent = QColor(QStringLiteral("#336699"));
            Theme::setSpec(custom);
            QPalette seeThrough;
            seeThrough.setColor(QPalette::Base, Qt::transparent);
            expect(Theme::selectionPalette(seeThrough, true).color(QPalette::Text).lightness() < 128 &&
                       Theme::selectionFill(Qt::transparent) == Theme::selectionFill(Theme::tokens().page) &&
                       Theme::rowHoverFill(Qt::transparent) == Theme::rowHoverFill(Theme::tokens().page),
                   "透明底的列表按页面底判明暗，浅色下选中行是深字");
            Theme::setSpec(savedSpec);
        }
        // #143 返工：真挂上那句样式表的树，Qt 给的 `Base` 是 NoBrush（颜色是不透明黑），上面手造的全透明测不到。
        // 实例设置左栏原样：旧出厂主色 #087A68；浅色、深色下选中项的字对 `paintRowBlock` 那块选中底都要读得清，
        // 半透明 `Base`（令牌 `input`）的普通列表也一起量。
        {
            const Theme::Spec savedSpec = Theme::current();
            QTreeWidget nav;
            nav.setStyleSheet(QStringLiteral("QTreeWidget { background: transparent; }"));
            nav.ensurePolished();
            expect(nav.palette().brush(QPalette::Base).style() == Qt::NoBrush &&
                       Theme::rowBase(nav.palette()).alpha() == 0,
                   "透明样式表的树 Base 是 NoBrush，rowBase 按全透明给");
            for (const char *preset : {"emerald", "dark"}) {
                Theme::Spec oldFactory = Theme::presetSpec(QLatin1String(preset));
                oldFactory.accent = QColor(QStringLiteral("#087A68"));
                Theme::setSpec(oldFactory);
                const Theme::Tokens t = Theme::tokens();
                const bool light = t.page.lightness() >= 128;
                QColor fill = t.focus;
                fill.setAlpha(light ? 30 : 34);  // 与 DesignDefaults::paintRowBlock 同一档
                const QColor block = Glass::over(fill, t.page);
                QPalette plain;
                plain.setColor(QPalette::Base, t.input);
                for (const QPalette &palette : {nav.palette(), plain}) {
                    const QColor ink = Theme::selectionPalette(palette, true).color(QPalette::Text);
                    expect(Glass::contrast(ink, block) >= 4.5 &&
                               (light ? ink.lightness() < block.lightness() : ink.lightness() > block.lightness()),
                           light ? "旧出厂主色 + 浅色：左栏选中项是比底深的字，对比度够"
                                 : "旧出厂主色 + 深色：选中项是比底浅的字，对比度够");
                }
            }
            // MOTD 按「露出来的底」重算字色：深色下半透明 `input` 底（7% 白）不能被当成白底把白字 / 黄字压暗；
            // 浅色下白字照旧压暗。透明样式表的树（NoBrush）也按页面底算。
            const QColor motdWhite(0xFF, 0xFF, 0xFF), motdYellow(0xFF, 0xFF, 0x55);
            const auto sum = [](const QColor &c) { return c.red() + c.green() + c.blue(); };
            for (const char *preset : {"emerald", "dark"}) {
                Theme::setSpec(Theme::presetSpec(QLatin1String(preset)));
                const Theme::Tokens t = Theme::tokens();
                const bool light = t.page.lightness() >= 128;
                QPalette plain;
                plain.setColor(QPalette::Base, t.input);
                for (const QPalette &palette : {nav.palette(), plain}) {
                    const QColor shown = Theme::shownRowBase(palette);
                    const QColor onWhite = MotdText::readableOn(motdWhite, shown);
                    const QColor onYellow = MotdText::readableOn(motdYellow, shown);
                    if (light)
                        expect(shown.alpha() == 255 && sum(onWhite) < sum(motdWhite), "浅色主题：MOTD 白字在露出来的浅底上照旧压暗");
                    else
                        expect(shown.alpha() == 255 && shown.lightness() < 128 && sum(onWhite) >= sum(motdWhite) &&
                                   sum(onYellow) >= sum(motdYellow) &&
                                   Theme::shownRowBase(Theme::selectionPalette(palette, true), true).lightness() < 128,
                               "深色主题：半透明 / 透明底（含选中那层半透明 Highlight）按露出来的深底算，MOTD 白字黄字不被压暗");
                }
            }
            Theme::setSpec(savedSpec);
        }
        // 主题显示名照着三张内置风景起；内部 id 与「配色」下拉的顺序不动（盘上存的是 id）。
        expect(Theme::presetName(QStringLiteral("dark")) == QStringLiteral("暮色雪山") &&
                   Theme::presetName(QStringLiteral("emerald")) == QStringLiteral("冰川碧湖") &&
                   Theme::presetName(QStringLiteral("light")) == QStringLiteral("冰川碧湖") &&
                   Theme::presetName(QStringLiteral("ocean")) == QStringLiteral("落日海崖") &&
                   Theme::presetName(QStringLiteral("system")) == QStringLiteral("跟随系统") &&
                   Theme::presetIds().value(0) == QStringLiteral("dark"),
               "preset names follow the three scenes / ids and order untouched");
        // 换配色不再重挂整份应用样式表 —— 样式表字符串必须与主题无关。
        // 这一条直接钉住那个代价（6800 个控件重挂一次要 1.9 秒）：三套预设铺完，
        // 应用身上挂的那一份必须还是同一串。
        const QString themeIndependentSheet = Theme::widgetStyleSheet();
        QString installedSheet;
        for (const QString &id : {QStringLiteral("emerald"), QStringLiteral("ocean"), QStringLiteral("dark")}) {
            Theme::setSpec(Theme::presetSpec(id));
            Theme::apply(app);
            {
                InlineMessageBox box(MessageKind::Question, QStringLiteral("删除原理图"), QStringLiteral("删除选中的原理图？"),
                                     {QStringLiteral("删除"), QStringLiteral("取消删除")}, 1, 0);
                box.show();
                app.processEvents();
                auto *remove = box.findChild<QPushButton *>(InlineMessageBox::buttonName(0));
                auto *cancel = box.findChild<QPushButton *>(InlineMessageBox::buttonName(1));
                expect(remove && cancel && remove->size() == cancel->size() &&
                           remove->width() >= qMax(remove->sizeHint().width(), cancel->sizeHint().width()),
                       "destructive confirmation actions have equal sizes and fit both labels");
                if (remove && cancel) {
                    expect(!remove->isDefault() && cancel->isDefault() && remove->x() < cancel->x() &&
                               remove->property("wtfmlButtonKind").toString() == Theme::buttonKindKey(Theme::ButtonKind::Danger),
                           "destructive action stays red and cancel stays the rightmost default action");
                    cancel->click();
                    expect(box.clickedIndex() == 1, "cancel retains its action index");
                    box.show();
                    remove->click();
                    expect(box.clickedIndex() == 0, "delete retains its action index");
                }
            }
            expect(app.font().pixelSize() == 14, "application body text uses the shared 14 px scale");
            QFont bold = app.font(); bold.setBold(true);
            expect(Theme::textFont(bold, Theme::TextRole::Caption).pixelSize() == 13 &&
                   Theme::textFont(bold, Theme::TextRole::Body).pixelSize() == 14 &&
                   Theme::textFont(bold, Theme::TextRole::Heading).pixelSize() == 16 &&
                   Theme::textFont(bold, Theme::TextRole::Display).pixelSize() == 26 &&
                   Theme::textFont(bold, Theme::TextRole::Empty).pixelSize() == 22 &&
                   Theme::textFont(bold, Theme::TextRole::Caption).bold(),
                   "tokens-r2 type scale: aux 13 / body 14 / section 16 / title 26 (+ empty 22)");
            {
                // 手写体只给页标题与空状态大字；小节标题是系统字半粗。
                const QStringList hand{Theme::handFamily(Theme::HandFont::Kai), Theme::handFamily(Theme::HandFont::KaiLight),
                                       Theme::handFamily(Theme::HandFont::Script)};
                const auto isHand = [&hand](const QFont &f) { return !f.families().isEmpty() && hand.contains(f.families().first()); };
                const QFont plain = app.font();
                expect(!isHand(Theme::textFont(plain, Theme::TextRole::Caption)) && !isHand(Theme::textFont(plain, Theme::TextRole::Body)) &&
                           !isHand(Theme::textFont(plain, Theme::TextRole::Heading)) &&
                           Theme::textFont(plain, Theme::TextRole::Heading).weight() == QFont::DemiBold &&
                           (hand.first().isEmpty() || (isHand(Theme::textFont(plain, Theme::TextRole::Display)) &&
                                                       isHand(Theme::textFont(plain, Theme::TextRole::Empty)))),
                       "hand font only on Display / Empty roles; section headings are system semibold");
                QWidget host;
                QPushButton plainButton(QStringLiteral("Enter 发送 Fabric 1.21.4"), &host);
                plainButton.ensurePolished();
                expect(!isHand(plainButton.font()) && plainButton.font().pixelSize() == 14,
                       "buttons are polished to the system font at 14 px");
            }
            {
                // 着色 alpha 自检：出厂浓度下，亚克力 / 云母（系统背板区间）每一档都落在对比度要的最薄着色，
                // 正文、次要字在背板最暗、最亮两端都 ≥ 4.5:1。
                const auto inks = Glass::bodyInks();
                const QList<QColor> range = Glass::systemBackdropRange();
                const qreal need = Glass::minimumTintAlpha(Theme::tokens().page, inks, range, 0.0);
                const qreal got = Glass::minimumTintAlpha(Theme::tokens().page, inks, range, Glass::tintStrength() / 100.0);
                QColor layer = Theme::tokens().page;
                layer.setAlphaF(got);
                double worst = 21.0;
                for (const QColor &bg : range)
                    for (const auto &ink : inks)
                        worst = qMin(worst, Glass::contrast(Glass::over(ink.first, Glass::over(layer, bg)), Glass::over(layer, bg)));
                printf("INFO tint %s alpha=%.2f need=%.2f worst=%.2f\n", qPrintable(id), got, need, worst);
                expect(qFuzzyCompare(got, need) && worst >= 4.5,
                       "default tint strength never binds: system backdrop tint is the thinnest readable alpha");
            }
            if (installedSheet.isEmpty()) {
                installedSheet = app.styleSheet();
            }
            expect(app.styleSheet() == installedSheet, "theme switch does not reinstall the app stylesheet");
            expect(Theme::widgetStyleSheet() == themeIndependentSheet,
                   "widget style sheet is independent of the current preset");
            const auto t = Theme::tokens();
            expect(app.palette().color(QPalette::Window) == t.page, "page palette follows preset");
            expect(Theme::current().accent == t.primary, "brand follows preset");
            expect(Theme::partBackground(Theme::Part::TitleBar) == t.deep, "title uses deep brand");
            expect(Theme::partBackground(Theme::Part::BottomBar) == t.subtle, "bottom bar uses subtle surface");
            expect(Theme::selectionFill(t.card) == t.selected, "selection uses specified color");
            expect(Theme::widgetStyleSheet().contains(QStringLiteral("palette(base)")) &&
                       app.palette().color(QPalette::Base).name() == t.input.name(),
                   "inputs use theme colors");
            {
                QWidget focusHost;
                focusHost.resize(180, 80);
                QPushButton button(QStringLiteral("任务中心"), &focusHost);
                button.setGeometry(20, 20, 140, 40);
                Theme::markButton(&button, Theme::ButtonKind::Neutral);
                focusHost.show();
                focusHost.activateWindow();
                app.processEvents();
                button.clearFocus();
                QKeyEvent firstTab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                QApplication::sendEvent(&focusHost, &firstTab);
                expect(button.hasFocus() && button.property("wtfmlKeyboardFocus").toBool(),
                       "first Tab enters a visible control and displays focus from an empty focus state");
                button.clearFocus();
                QKeyEvent firstBacktab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
                QApplication::sendEvent(&focusHost, &firstBacktab);
                expect(button.hasFocus(), "first Shift-Tab enters the reverse focus chain");
                button.setFocus(Qt::TabFocusReason);
                // 「键盘在用」这一档现在只认**真的按键**（见 Theme.cpp 里
                // `ButtonFocusVisibility` 那段 —— 藏起焦点控件时 Qt 也报 TabFocusReason，
                // 不能拿它当"用户按了 Tab"）。这里先送一发普通按键把这一档摆上；
                // 不送 Tab 键：那一发会真的触发焦点链移动，把被测按钮换掉。
                QKeyEvent keyboardInput(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
                QApplication::sendEvent(&button, &keyboardInput);
                QFocusEvent keyboardFocus(QEvent::FocusIn, Qt::TabFocusReason);
                QApplication::sendEvent(&button, &keyboardFocus);
                expect(button.property("wtfmlKeyboardFocus").toBool(), "keyboard focus shows button focus ring");
                const QImage keyboardShot = button.grab().toImage();
                const QPointF position(button.rect().center());
                const QPointF global(button.mapToGlobal(button.rect().center()));
                QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(&button, &press);
                QApplication::sendEvent(&button, &release);
                const QImage mouseShot = button.grab().toImage();
                expect(!button.property("wtfmlKeyboardFocus").toBool(), "mouse activation clears focus ring even on an already focused button");
                expect(keyboardShot.pixelColor(0, keyboardShot.height() / 2) !=
                           mouseShot.pixelColor(0, mouseShot.height() / 2),
                       "mouse click removes the rendered focus border");
                expect(button.hasFocus() && (button.focusPolicy() & Qt::TabFocus),
                       "hiding mouse focus ring preserves real focus and keyboard accessibility");
                QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                QApplication::sendEvent(&button, &tab);
                expect(button.property("wtfmlKeyboardFocus").toBool(), "Tab restores keyboard focus feedback after mouse use");

                // 设置页分段页签（自绘焦点环）：真机反馈"点完页签整条外面一圈绿框"。鼠标点完后焦点由 Qt
                // 从藏起来那一页挪过来，理由同样是 TabFocusReason —— 判据得是"最后一次输入是不是键盘"。
                SettingsParts::SlidingTabs segments(&focusHost);
                segments.addTab(QStringLiteral("启动"));
                segments.addTab(QStringLiteral("下载"));
                segments.move(20, 70);
                segments.show();
                app.processEvents();
                const QPoint at = segments.tabRect(1).center();
                QMouseEvent tabPress(QEvent::MouseButtonPress, QPointF(at), QPointF(segments.mapToGlobal(at)),
                                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(&segments, &tabPress);
                segments.setFocus(Qt::TabFocusReason);
                app.processEvents();
                const QImage segMouse = segments.grab().toImage();
                expect(segments.hasFocus() && !segments.property("wtfmlKeyboardFocus").toBool(),
                       "segmented tabs: focus moved in after a mouse click draws no outer ring");
                QKeyEvent segKey(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
                QApplication::sendEvent(&segments, &segKey);
                QFocusEvent segTabIn(QEvent::FocusIn, Qt::TabFocusReason);
                QApplication::sendEvent(&segments, &segTabIn);
                const QImage segKeyboard = segments.grab().toImage();
                expect(segments.property("wtfmlKeyboardFocus").toBool() &&
                           segKeyboard.pixelColor(2, segKeyboard.height() / 2) != segMouse.pixelColor(2, segMouse.height() / 2),
                       "segmented tabs: keyboard arrival still draws the focus ring (F1)");
            }
            // ---- 键盘走到的那一行露出行尾按钮（#70 第二批 ①）：判据 = 悬停，或（当前行且列表带键盘焦点）----
            {
                QListWidget keyed;
                keyed.setFrameShape(QFrame::NoFrame);
                keyed.resize(520, 3 * ListRowDelegate::rowHeight() + 4);
                ListRowDelegate::install(&keyed);
                for (int i = 0; i < 3; ++i) {
                    auto *item = new QListWidgetItem(QStringLiteral("实例 %1").arg(i), &keyed);
                    item->setData(ListRowDelegate::SubtitleRole, QStringLiteral("Fabric 0.16.9 · 1.21.4"));
                }
                keyed.move(420, 260);  // 离开屏幕左上角：免得假光标停在第一行上、算成悬停
                keyed.show();
                keyed.activateWindow();
                keyed.setFocus(Qt::OtherFocusReason);
                app.processEvents();
                keyed.setCurrentRow(1);
                app.processEvents();
                const QColor primary = Theme::tokens().primary;
                const auto launchProbe = [&keyed](int row) {
                    const QRect launch = ListRowDelegate::actionRect(keyed.visualItemRect(keyed.item(row)),
                                                                     ListRowDelegate::Action::Launch);
                    return QPoint(launch.left() + 4, launch.center().y());
                };
                const auto pillAt = [&keyed, &launchProbe](int row) {
                    return keyed.viewport()->grab().toImage().pixelColor(launchProbe(row));
                };
                // 鼠标点进来：当前行不露按钮。
                const QRect row1 = keyed.visualItemRect(keyed.item(1));
                const QPoint edge(row1.left() + 2, row1.top() + 2);
                QMouseEvent mouseDown(QEvent::MouseButtonPress, edge, keyed.viewport()->mapToGlobal(edge),
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(keyed.viewport(), &mouseDown);
                app.processEvents();
                expect(keyed.hasFocus() && keyed.currentRow() == 1 && !keyed.property("wtfmlKeyboardFocus").toBool() &&
                           pillAt(1) != primary,
                       "★ LR-K1 鼠标点到的当前行不露行尾按钮（没有悬停、也不是键盘焦点）");
                // 键盘到达：当前行露出主色启动胶囊，别的行不露。
                QKeyEvent anyKey(QEvent::KeyPress, Qt::Key_F13, Qt::NoModifier);
                QApplication::sendEvent(&keyed, &anyKey);
                app.processEvents();
                expect(keyed.property("wtfmlKeyboardFocus").toBool() && pillAt(1) == primary,
                       "★ LR-K2 键盘走到的当前行露出行尾按钮（主色启动胶囊）");
                expect(pillAt(0) != primary && pillAt(2) != primary, "LR-K3 键盘焦点只让当前行露行尾按钮，别的行不露");
                // 再点一下鼠标：收回。
                QApplication::sendEvent(keyed.viewport(), &mouseDown);
                app.processEvents();
                expect(!keyed.property("wtfmlKeyboardFocus").toBool() && pillAt(1) != primary,
                       "★ LR-K4 鼠标一点，当前行的行尾按钮收回去");
            }
            // ---- 菜单键落点（#70 第二批 ②）：键盘来源的 ContextMenu 统一换算成「当前行附近」的视口坐标 ----
            {
                QListWidget menuList;
                menuList.setFrameShape(QFrame::NoFrame);
                menuList.resize(300, 200);
                for (int i = 0; i < 40; ++i) new QListWidgetItem(QStringLiteral("条目 %1").arg(i), &menuList);
                menuList.setContextMenuPolicy(Qt::CustomContextMenu);
                ItemViewContextMenu::install(&menuList);
                int requested = 0;
                QPoint requestedAt(-1, -1);
                QObject::connect(&menuList, &QListWidget::customContextMenuRequested, &menuList,
                                 [&](const QPoint &pos) { ++requested; requestedAt = pos; });
                menuList.show();
                menuList.setFocus(Qt::OtherFocusReason);
                app.processEvents();
                menuList.setCurrentRow(30);  // 视口外：菜单键要先把它滚进来
                app.processEvents();
                QContextMenuEvent keyMenu(QContextMenuEvent::Keyboard, QPoint(0, 0), menuList.mapToGlobal(QPoint(0, 0)));
                QApplication::sendEvent(&menuList, &keyMenu);
                app.processEvents();
                const QRect currentRow = menuList.visualItemRect(menuList.item(30));
                expect(requested == 1 && currentRow.isValid() && currentRow.contains(requestedAt),
                       "★ 菜单键：只发一次，落点在当前行里（视口坐标），视口外的当前行先滚进来");
                menuList.setCurrentRow(-1);
                menuList.setCurrentItem(nullptr);
                requested = 0;
                QApplication::sendEvent(&menuList, &keyMenu);
                expect(requested == 1 && menuList.viewport()->rect().contains(requestedAt),
                       "菜单键：没有当前行时也只发一次，落在视口里");
            }
            // ---- 列表的键盘焦点标记（#70 第二批 ③）：键盘焦点时当前行一圈焦点环色，鼠标时不画 ----
            {
                QListWidget marked;  // 默认代理：条目的画法不管标记
                marked.setFrameShape(QFrame::NoFrame);
                marked.resize(260, 200);
                for (int i = 0; i < 5; ++i) {
                    auto *item = new QListWidgetItem(QStringLiteral("条目 %1").arg(i), &marked);
                    item->setSizeHint(QSize(0, 44));  // 够高：圆角之间才有一段直边可量
                }
                marked.move(420, 260);
                marked.show();
                marked.activateWindow();
                marked.setFocus(Qt::OtherFocusReason);
                app.processEvents();
                marked.setCurrentRow(2);
                const auto settle = [&app] { for (int i = 0; i < 4; ++i) app.processEvents(); };
                settle();
                const auto markerOf = [&marked]() {
                    return marked.viewport()->findChild<QWidget *>(QStringLiteral("wtfmlItemViewFocusMarker"));
                };
                const QRect row = marked.visualItemRect(marked.item(2));
                // 焦点环色带透明度：量「左缘那一列和没有标记时不同」，不量等于令牌原值。
                QImage plain;
                const auto ringHits = [&marked, row, &plain]() {
                    const QImage shot = marked.viewport()->grab().toImage();
                    int hits = 0;
                    for (int y = row.top() + 10; y < row.bottom() - 10; ++y) {
                        if (plain.isNull() || shot.pixelColor(row.left() + 2, y) != plain.pixelColor(row.left() + 2, y)) ++hits;
                    }
                    return hits;
                };
                // 鼠标点进来：没有标记。
                const QPoint edge(row.left() + 2, row.top() + 2);
                QMouseEvent press(QEvent::MouseButtonPress, edge, marked.viewport()->mapToGlobal(edge), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(marked.viewport(), &press);
                settle();
                plain = marked.viewport()->grab().toImage();
                expect(marked.currentRow() == 2 && (markerOf() == nullptr || !markerOf()->isVisible()) && ringHits() == 0,
                       "★ 列表标记：鼠标点到的当前行没有键盘焦点标记");
                // 键盘到达：当前行左缘一整段是焦点环色。
                QKeyEvent anyKey(QEvent::KeyPress, Qt::Key_F13, Qt::NoModifier);
                QApplication::sendEvent(&marked, &anyKey);
                settle();
                expect(markerOf() != nullptr && markerOf()->isVisible() && row.contains(markerOf()->geometry()) &&
                           ringHits() >= (row.height() - 20) * 8 / 10,
                       "★ 列表标记：键盘焦点时当前行有一圈焦点环色的标记");
                // 换当前行：标记跟着走。
                marked.setCurrentRow(3);
                settle();
                expect(markerOf() != nullptr && markerOf()->isVisible() &&
                           marked.visualItemRect(marked.item(3)).contains(markerOf()->geometry()),
                       "列表标记：当前行换了，标记跟着挪过去");
                // 鼠标一点：收回。
                QApplication::sendEvent(marked.viewport(), &press);
                settle();
                expect(markerOf() == nullptr || !markerOf()->isVisible(), "★ 列表标记：鼠标一点就收回");
            }
            // ---- 开关控件能用 Tab 到达，只在键盘焦点时画环（#70 第二批 ④）----
            {
                QWidget switchHost;
                switchHost.resize(220, 90);
                auto *before = new QLineEdit(&switchHost);
                before->setGeometry(10, 8, 190, 30);
                auto *tabSwitch = new ToggleSwitch(&switchHost);
                tabSwitch->move(10, 50);
                tabSwitch->resize(tabSwitch->sizeHint());
                switchHost.move(420, 260);
                switchHost.show();
                switchHost.activateWindow();
                before->setFocus(Qt::OtherFocusReason);
                app.processEvents();
                expect((tabSwitch->focusPolicy() & Qt::TabFocus) != 0 && (tabSwitch->focusPolicy() & Qt::ClickFocus) == 0,
                       "★ 开关：能用 Tab 到达（鼠标点它不抢焦点）");
                const QImage unfocused = tabSwitch->grab().toImage();
                // 真按 Tab：键盘到达 → 画环。
                QKeyEvent tabKey(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                QApplication::sendEvent(before, &tabKey);
                app.processEvents();
                const QImage ringed = tabSwitch->grab().toImage();
                expect(tabSwitch->hasFocus() && tabSwitch->property("wtfmlKeyboardFocus").toBool() && ringed != unfocused,
                       "★ 开关：Tab 到达后画键盘焦点环");
                // 空格切换（QAbstractButton 自带）。
                QKeyEvent spaceDown(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
                QKeyEvent spaceUp(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
                QApplication::sendEvent(tabSwitch, &spaceDown);
                QApplication::sendEvent(tabSwitch, &spaceUp);
                app.processEvents();
                expect(tabSwitch->isChecked(), "开关：焦点在上面按空格切换");
                // 鼠标按下：环收掉（焦点仍在，不再画）。
                const QPoint at = tabSwitch->rect().center();
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), QPointF(tabSwitch->mapToGlobal(at)),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(tabSwitch, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), QPointF(tabSwitch->mapToGlobal(at)),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(tabSwitch, &release);
                app.processEvents();
                expect(!tabSwitch->property("wtfmlKeyboardFocus").toBool(), "★ 开关：鼠标点过之后不再画键盘焦点环");
            }
            ToggleSwitch toggle;
            toggle.resize(toggle.sizeHint());
            toggle.setChecked(true);
            toggle.show();
            app.processEvents();
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&toggle, &leave);
            const QPoint trackPoint(3, toggle.height() / 2);
            expect(toggle.grab().toImage().pixelColor(trackPoint) ==
                       t.primary,
                   "switch on state uses the solid accent fill in every theme");
            if (id == QLatin1String("emerald")) {
                QEnterEvent enter(QPointF(3, 3), QPointF(3, 3), QPointF(3, 3));
                QApplication::sendEvent(&toggle, &enter);
                expect(toggle.grab().toImage().pixelColor(trackPoint) == t.primaryHover,
                       "emerald switch hover uses palette brand hover");
            }
            toggle.hide();
            QSettings().setValue(QStringLiteral("Look/LogColors"), true);
            GameLogWindow log(QStringLiteral("Theme check"));
            log.appendText(QStringLiteral("[main/INFO]: info\n[main/WARN]: warn\n[main/ERROR]: error\n[main/FATAL]: fatal\n"));
            auto *view = log.findChild<QPlainTextEdit *>();
            expect(view != nullptr && view->palette().color(QPalette::Base) == t.logBase, "log body follows preset");
            if (view) {
                QTextCursor cursor(view->document()->firstBlock());
                cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                expect(cursor.charFormat().foreground().color() ==
                       (id == QLatin1String("dark") ? QColor("#79D4A5") : QColor("#202020")), "INFO text color");
                const QTextBlock warning = view->document()->firstBlock().next();
                expect(warning.blockFormat().background().color() ==
                       (id == QLatin1String("dark") ? QColor("#171E23") : QColor("#FFF0BF")), "WARN background color");
                QTextCursor error(warning.next());
                error.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                expect(error.charFormat().foreground().color() ==
                       (id == QLatin1String("dark") ? QColor("#FFB8BD") : QColor("#202020")), "ERROR text color");
                QTextCursor fatal(warning.next().next());
                fatal.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                expect(fatal.charFormat().foreground().color() ==
                       (id == QLatin1String("dark") ? QColor("#FF858B") : QColor("#202020")), "FATAL text color");
            }
            QSettings().setValue(QStringLiteral("Look/LogColors"), false);
            GameLogWindow mono(QStringLiteral("Monochrome"));
            mono.appendText(QStringLiteral("[main/ERROR]: error\n"));
            auto *monoView = mono.findChild<QPlainTextEdit *>();
            if (monoView) {
                QTextCursor cursor(monoView->document()->firstBlock());
                cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                expect(cursor.charFormat().foreground().color() ==
                       (id == QLatin1String("dark") ? QColor("#D5DEE3") : QColor("#202020")), "monochrome text color");
                expect(monoView->document()->firstBlock().blockFormat().background().style() == Qt::NoBrush,
                       "monochrome clears level backgrounds");
            } else expect(false, "monochrome log view exists");
            Theme::save(Theme::current());
            expect(!QSettings().value(QStringLiteral("Look/LogColors"), true).toBool(),
                   "saving theme preserves log color preference");
            expect(Theme::load().presetId == id, "theme selection survives save/load");
            if (app.arguments().contains(QStringLiteral("--theme-preview"))) {
                const QString directory = QDir::current().filePath(QStringLiteral("build/theme-preview"));
                QDir().mkpath(directory);
                QMainWindow preview;
                MainWindowUI previewUi(&preview);
                MainWindowUI::FolderRow currentFolder;
                currentFolder.path = QStringLiteral("D:/Games/Minecraft");
                currentFolder.title = QStringLiteral("当前文件夹");
                currentFolder.subtitle = currentFolder.path;
                currentFolder.assistant = true;
                currentFolder.current = true;
                MainWindowUI::FolderRow moddedFolder;
                moddedFolder.path = QStringLiteral("E:/Minecraft/Modpacks");
                moddedFolder.title = QStringLiteral("模组整合包");
                moddedFolder.subtitle = moddedFolder.path;
                previewUi.setFolderRows({currentFolder, moddedFolder});
                preview.resize(1000, 650);
                preview.show();
                app.processEvents();
                expect(preview.grab().save(directory + QLatin1Char('/') + id + QStringLiteral("-home.png")),
                       "save home preview");
                previewUi.getTabWidget()->setCurrentIndex(3);
                app.processEvents();
                expect(preview.grab().save(directory + QLatin1Char('/') + id + QStringLiteral("-settings.png")),
                       "save settings preview");
                expect(preview.findChild<QListWidget *>(QStringLiteral("settingsCategoryList")) == nullptr &&
                           preview.findChild<QFrame *>(QStringLiteral("settingsSidebarSurface")) == nullptr &&
                           preview.findChild<SettingsParts::SlidingTabs *>(MainWindowUI::settingsTabsName()) != nullptr,
                       "settings has top tabs and no left index sidebar");
                for (const auto &entry : {qMakePair(MainWindowUI::instancesTabIndex(), QStringLiteral("-management.png")),
                                          qMakePair(MainWindowUI::downloadTabIndex(), QStringLiteral("-download.png"))}) {
                    previewUi.getTabWidget()->setCurrentIndex(entry.first);
                    app.processEvents();
                    for (auto *list : {preview.findChild<QListWidget *>(MainWindowUI::folderListName()),
                                       preview.findChild<QListWidget *>(MainWindowUI::downloadCategoryListName())}) {
                        expect(list && list->frameShape() == QFrame::NoFrame
                                   && list->palette().color(QPalette::Base) == t.main,
                               "sidebar list uses seamless theme surface without inner frame");
                    }
                    expect(preview.grab().save(directory + QLatin1Char('/') + id + entry.second), "save sidebar preview");
                }
                log.show();
                app.processEvents();
                expect(view && view->viewport()->palette().color(QPalette::Base) == t.logBase,
                       "visible log viewport keeps specified background");
                expect(log.grab().save(directory + QLatin1Char('/') + id + QStringLiteral("-log.png")),
                       "save log preview");
            }
        }
        // ---- 换页那一下（F1）：鼠标点导航换页后，新页第一个控件不该带焦点环 ----
        //
        // 判据只能有一条：「最后一次输入是不是键盘」。`QFocusEvent::reason()` 不算数 ——
        // 代码里 `setFocus(Qt::TabFocusReason)`（换页交接、分段页签那一路）和用户真按 Tab
        // 报的是同一个理由，只按理由判就会凭空亮一圈（分段页签那次就是这么亮的，见 00c25a0）。
        // 按钮和自绘环的控件早就按这一条判了，**输入类控件还留在理由上**：这里把两条路一起钉住。
        {
            QWidget host;
            host.resize(360, 200);
            auto *nav = new QPushButton(QStringLiteral("管理"), &host);
            nav->setObjectName(QStringLiteral("navProbeButton"));
            nav->setFocusPolicy(Qt::NoFocus);
            nav->setGeometry(8, 8, 120, 32);
            auto *stack = new QStackedWidget(&host);
            stack->setGeometry(8, 48, 340, 140);
            auto *oldPage = new QWidget;
            auto *oldButton = new QPushButton(QStringLiteral("旧页那颗"), oldPage);
            oldButton->setGeometry(8, 8, 140, 32);
            auto *newPage = new QWidget;
            auto *newEdit = new QLineEdit(newPage);
            newEdit->setObjectName(QStringLiteral("newPageFirstControl"));
            newEdit->setGeometry(8, 8, 180, 32);
            auto *newButton = new QPushButton(QStringLiteral("新页第二颗"), newPage);
            newButton->setGeometry(8, 52, 140, 32);
            stack->addWidget(oldPage);
            stack->addWidget(newPage);
            QObject::connect(nav, &QPushButton::clicked, stack, [stack]() { stack->setCurrentIndex(1); });
            host.show();
            host.activateWindow();
            app.processEvents();
            oldButton->setFocus(Qt::TabFocusReason);
            QKeyEvent warm(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
            QApplication::sendEvent(oldButton, &warm);
            app.processEvents();
            expect(oldButton->hasFocus() && oldButton->property("wtfmlKeyboardFocus").toBool(),
                   "换页前：键盘到达的那颗按钮带着焦点环（判据的正面那一半）");
            const QPoint navAt = nav->rect().center();
            QMouseEvent navPress(QEvent::MouseButtonPress, QPointF(navAt), QPointF(nav->mapToGlobal(navAt)),
                                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(nav, &navPress);
            QMouseEvent navRelease(QEvent::MouseButtonRelease, QPointF(navAt), QPointF(nav->mapToGlobal(navAt)),
                                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(nav, &navRelease);
            app.processEvents();
            expect(stack->currentIndex() == 1 && newEdit->hasFocus(),
                   "鼠标点导航真的翻页，焦点交接给新页第一个控件");
            const bool ringedAfterMouse = newEdit->property("wtfmlKeyboardFocus").toBool();
            const QImage afterMouse = newEdit->grab().toImage();
            // 只看最上面那 3 px（输入框常驻的那圈透明边就是环带）。两版都**有焦点**，
            // 底那一档一样，所以这个带子里唯一的差别就是那圈环本身。
            const auto topBand = [](const QImage &image) {
                return image.copy(image.width() / 5, 0, image.width() * 3 / 5, 3);
            };
            expect(!ringedAfterMouse, "★ 鼠标点导航换页后，新页第一个控件不挂键盘焦点那一档");
            // 反方向：焦点拿掉再放回来、理由写成 TabFocusReason（"交接"和"真按 Tab"本来就是同一个
            // 理由）—— 最后一次输入还是鼠标，就不该点亮。这一条钉的是判据本身，和 Qt 交接时到底
            // 报哪个理由无关。
            newEdit->clearFocus();
            app.processEvents();
            newEdit->setFocus(Qt::TabFocusReason);
            app.processEvents();
            expect(newEdit->hasFocus() && !newEdit->property("wtfmlKeyboardFocus").toBool(),
                   "★ 只凭 FocusIn 那个理由（TabFocusReason）点不亮环（交接与真按 Tab 用的是同一个理由）");
            const QImage focusedNoRing = newEdit->grab().toImage();
            expect(topBand(afterMouse) == topBand(focusedNoRing),
                   "…换页交接过来的那一版和「有焦点、没环」那一版，环带上一模一样");
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
            QApplication::sendEvent(newEdit, &tab);
            app.processEvents();
            expect(newButton->hasFocus() && newButton->property("wtfmlKeyboardFocus").toBool(),
                   "★ 按 Tab 到达新页那颗控件之后焦点环照旧出来");
            const QImage ringShot = newButton->grab().toImage();
            const QPoint buttonAt = newButton->rect().center();
            QMouseEvent buttonPress(QEvent::MouseButtonPress, QPointF(buttonAt),
                                    QPointF(newButton->mapToGlobal(buttonAt)), Qt::LeftButton, Qt::LeftButton,
                                    Qt::NoModifier);
            QApplication::sendEvent(newButton, &buttonPress);
            app.processEvents();
            const QImage plainShot = newButton->grab().toImage();
            expect(!newButton->property("wtfmlKeyboardFocus").toBool() && ringShot != plainShot,
                   "…鼠标按下那一下把环收掉（画出来的像素真的变了）");
            // 键盘到达的那个输入框：环带上真多出那圈环 —— 证明上面"没挂那一档"确实等于"没画环"，
            // 不是这套判据根本画不出来。
            QKeyEvent warmAgain(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier);
            QApplication::sendEvent(newButton, &warmAgain);
            newEdit->setFocus(Qt::TabFocusReason);
            app.processEvents();
            expect(newEdit->property("wtfmlKeyboardFocus").toBool() &&
                       topBand(newEdit->grab().toImage()) != topBand(focusedNoRing),
                   "★ 键盘到达的输入框画得出那一圈环（同一套判据的正面）");
        }
        // ---- 真窗口那一份：导航点上五颗都走一遍 ----
        {
            QMainWindow window;
            MainWindowUI windowUi(&window);
            window.resize(1000, 720);
            window.show();
            window.activateWindow();
            app.processEvents();
            QTabWidget *tabs = windowUi.getTabWidget();
            TitleBar *rail = windowUi.titleBar();
            const QList<int> targets{MainWindowUI::instancesTabIndex(), MainWindowUI::downloadTabIndex(),
                                     MainWindowUI::agentTabIndex(), MainWindowUI::settingsTabIndex(),
                                     MainWindowUI::homeTabIndex()};
            bool clean = true, moved = false;
            QString detail;
            for (int target : targets) {
                // 先在当前页上用键盘到达一颗控件，把「键盘在用」那一档摆上（用户报的那一幕就是
                // 这样来的：之前按过 Tab，再拿鼠标点导航）。
                QWidget *warm = nullptr;
                for (QWidget *w : tabs->widget(tabs->currentIndex())->findChildren<QWidget *>()) {
                    if (w->isVisible() && w->isEnabled() && (w->focusPolicy() & Qt::TabFocus)) {
                        warm = w;
                        break;
                    }
                }
                if (warm != nullptr) {
                    warm->setFocus(Qt::TabFocusReason);
                    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
                    QApplication::sendEvent(warm, &key);
                    app.processEvents();
                }
                QAbstractButton *navButton = rail->navButton(target);
                if (navButton == nullptr) {
                    clean = false;
                    detail = QStringLiteral("导航轨上找不到第 %1 颗").arg(target);
                    break;
                }
                const QPoint at = navButton->rect().center();
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), QPointF(navButton->mapToGlobal(at)),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(navButton, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at),
                                    QPointF(navButton->mapToGlobal(at)), Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(navButton, &release);
                app.processEvents();
                if (tabs->currentIndex() != target) {
                    clean = false;
                    detail = QStringLiteral("点了第 %1 颗没翻过去").arg(target);
                    break;
                }
                QWidget *focus = QApplication::focusWidget();
                if (focus != nullptr) {
                    moved = true;
                    if (focus->property("wtfmlKeyboardFocus").toBool()) {
                        clean = false;
                        detail = QStringLiteral("第 %1 页焦点控件 %2 带着环").arg(target).arg(focus->objectName());
                        break;
                    }
                }
                for (QWidget *w : tabs->widget(target)->findChildren<QWidget *>()) {
                    if (w->isVisible() && w->property("wtfmlKeyboardFocus").toBool()) {
                        clean = false;
                        detail = QStringLiteral("第 %1 页的 %2 带着环").arg(target).arg(w->objectName());
                        break;
                    }
                }
                if (!clean) break;
            }
            if (!clean && !detail.isEmpty()) {
                std::printf("  导航焦点那一项没过的细节：%s\n", qUtf8Printable(detail));
            }
            expect(clean, "★ 真窗口：鼠标点导航换页后，新页上没有任何控件带着键盘焦点环");
            expect(moved, "…换页后焦点确实交接到了新页的控件上（上面那条不是靠「没焦点」过的）");
        }
        // ---- 左上角品牌标 W：换页不变色（步骤 121），主页风景 / 别的页磨砂底上都有 3:1 ----
        // 三套预设各一份，再加暮色雪山配一张很亮的自定义背景图（最难兜对比度的那一档）。
        {
            const QString brightPath = QDir(settingsDir.path()).filePath(QStringLiteral("bright-bg.png"));
            QImage bright(640, 400, QImage::Format_RGB32);
            bright.fill(QColor(246, 246, 248));
            bright.save(brightPath);
            struct LogoCase {
                QString preset;
                bool brightImage;
            };
            const QList<LogoCase> cases{{QStringLiteral("dark"), false}, {QStringLiteral("emerald"), false},
                                        {QStringLiteral("ocean"), false}, {QStringLiteral("dark"), true}};
            const Theme::Spec restore = Theme::current();
            bool same = true, readable = true, fades = true, captionReadable = true, captionKept = true;
            for (const LogoCase &c : cases) {
                Theme::Spec spec = Theme::presetSpec(c.preset);
                if (c.brightImage) {
                    Theme::PartStyle &page = spec.parts[int(Theme::Part::PageBackground)];
                    page.hasImage = true;
                    page.imagePath = brightPath;
                }
                Theme::setSpec(spec);
                Theme::apply(app);
                QMainWindow window;
                MainWindowUI windowUi(&window);
                window.resize(1280, 800);
                window.show();
                app.processEvents();
                QWidget *logo = window.findChild<QWidget *>(QStringLiteral("titleBarLogo"));
                Glass::Backdrop *backdrop = Glass::Backdrop::of(logo);
                QTabWidget *tabs = windowUi.getTabWidget();
                if (logo == nullptr || backdrop == nullptr) {
                    same = readable = false;
                    std::printf("  品牌标 %s：找不到 W 或整窗底\n", qUtf8Printable(c.preset));
                    continue;
                }
                const auto shoot = [&](int tab) {
                    tabs->setCurrentIndex(tab);
                    app.processEvents();
                    backdrop->rebuildNow();
                    app.processEvents();
                    QImage img(logo->size(), QImage::Format_ARGB32);
                    img.fill(Qt::transparent);
                    logo->render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
                    return img;
                };
                const QImage home = shoot(MainWindowUI::homeTabIndex());
                const QList<QColor> homeUnder = backdrop->colorsUnder(logo);  // 主页此刻真正的底（清晰风景）
                // 主页两颗窗口按钮共用一份墨色：取两颗字形里最实的那一点（最小化那一横落在半像素上，
                // 单看它会被抗锯齿压淡），叠到每颗按钮底下实测的两端上量对比度。
                // 亮背景图要过 3:1；默认风景墨色原样是主页字色 heroInk2（补对比度那一步不许动它）。
                {
                    QColor core(0, 0, 0, 0);
                    QList<QColor> under;
                    for (const char *name : {"titleBarMinimize", "titleBarClose"}) {
                        auto *button = window.findChild<QWidget *>(QString::fromLatin1(name));
                        if (button == nullptr) continue;
                        QImage img(button->size(), QImage::Format_ARGB32);
                        img.fill(Qt::transparent);
                        button->render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
                        for (int y = 0; y < img.height(); ++y)
                            for (int x = 0; x < img.width(); ++x)
                                if (img.pixelColor(x, y).alpha() > core.alpha()) core = img.pixelColor(x, y);
                        under += backdrop->colorsUnder(button);
                    }
                    double low = under.isEmpty() ? 0.0 : 21.0;
                    for (const QColor &bg : under) low = qMin(low, Glass::contrast(Glass::over(core, bg), bg));
                    const QColor expected = Theme::tokens().heroInk2;
                    const bool kept = qAbs(core.red() - expected.red()) <= 2 && qAbs(core.green() - expected.green()) <= 2 &&
                                      qAbs(core.blue() - expected.blue()) <= 2 && qAbs(core.alpha() - expected.alpha()) <= 3;
                    if (c.brightImage) captionReadable = captionReadable && low >= 3.0;
                    else captionKept = captionKept && kept;
                    std::printf("  窗口按钮 %s%s：墨 %s α%d（主页字色 %s α%d），最差对比度 %.2f\n", qUtf8Printable(c.preset),
                                c.brightImage ? "+亮背景图" : "", qUtf8Printable(core.name()), core.alpha(),
                                qUtf8Printable(expected.name()), expected.alpha(), low);
                }
                const QImage manage = shoot(MainWindowUI::instancesTabIndex());
                // 别的页此刻真正的底：W 那一块的磨砂底（风景淡出后只剩它）+ 主页那一份
                const QList<QColor> under = homeUnder + backdrop->colorsUnderAnyPage(logo);
                double worst = 21.0;
                int opaque = 0;
                QColor first, last;
                for (int y = 0; y < home.height(); ++y) {
                    for (int x = 0; x < home.width(); ++x) {
                        const QColor px = home.pixelColor(x, y);
                        if (px.alpha() < 255) continue;  // 只量实心那部分（边缘抗锯齿是半透明）
                        if (opaque++ == 0) first = px;
                        last = px;
                        for (const QColor &bg : under) worst = qMin(worst, Glass::contrast(px, bg));
                    }
                }
                const bool caseSame = home == manage && opaque > 20;
                const bool caseReadable = worst >= 3.0;
                same = same && caseSame;
                readable = readable && caseReadable;
                std::printf("  品牌标 %s%s：主页 / 管理页%s，W 实心 %d 像素，上 %s → 下 %s，最差对比度 %.2f\n",
                            qUtf8Printable(c.preset), c.brightImage ? "+亮背景图" : "",
                            caseSame ? "一致" : "不一致", opaque, qUtf8Printable(first.name()),
                            qUtf8Printable(last.name()), worst);
                // 窗口按钮墨色跟风景不透明度渐变：一半时既不是普通字色也不是主页字色。
                if (!c.brightImage) {
                    TitleBar *rail = windowUi.titleBar();
                    QAbstractButton *close = rail->closeButton();
                    const auto glyph = [&](qreal opacity) {
                        rail->setHeroOpacity(opacity);
                        QImage img(close->size(), QImage::Format_ARGB32);
                        img.fill(Qt::transparent);
                        close->render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
                        return img;
                    };
                    const QImage normal = glyph(0.0), mid = glyph(0.5), hero = glyph(1.0);
                    fades = fades && normal != hero && mid != normal && mid != hero;
                    rail->setHeroOpacity(backdrop->heroOpacity());
                }
            }
            Theme::setSpec(restore);
            Theme::apply(app);
            expect(same, "★ 品牌标 W 在主页和管理页画出来一模一样（三套预设 + 亮背景图）");
            expect(readable, "★ 品牌标 W 在主页风景和别的页磨砂底上都有 3:1（三套预设 + 亮背景图）");
            expect(fades, "窗口按钮墨色按风景不透明度在普通 / 主页字色之间渐变（一半时介于两者）");
            expect(captionReadable, "★ 亮背景图下主页三颗窗口按钮对实测底色有 3:1");
            expect(captionKept, "★ 默认风景三套预设下主页窗口按钮墨色仍是主页字色（补对比度不改默认观感）");
        }
        {
            Theme::Spec edited;
            int emitted = 0;
            ThemeAdvancedEditor editor;
            QObject::connect(&editor, &ThemeAdvancedEditor::specEdited, &editor,
                [&](const Theme::Spec &spec) { edited = spec; ++emitted; });
            auto *roles = editor.findChild<QComboBox *>("themeRoleCombo");
            auto *parts = editor.findChild<QComboBox *>("themePartCombo");
            auto *opacity = editor.findChild<QSpinBox *>("themePartTextOpacity");
            auto *body = editor.findChild<QWidget *>("themeAdvancedBody");
            expect(roles && roles->count() == Theme::roleCount() && parts && parts->count() == Theme::partCount(),
                   "advanced editor exposes every supported role and part");
            expect(body && body->isHidden(), "advanced editor starts collapsed");
            auto *fold = editor.findChild<QAbstractButton *>("themeAdvancedToggle");
            // 折叠行（8 号反馈）：整行一颗按钮、标题在 text()、右边那颗小字写「展开 / 收起」，
            // 不再有 ▸ / ▾ 箭头，也不再是 QToolButton 那套系统外观。
            const auto foldState = [fold]() {
                const auto labels = fold->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly);
                for (auto *label : labels) {
                    if (label->text() == QStringLiteral("展开") || label->text() == QStringLiteral("收起")) {
                        return label->text();
                    }
                }
                return QString();
            };
            expect(fold && qobject_cast<QToolButton *>(fold) == nullptr &&
                       fold->text() == QStringLiteral("细分区域与控件颜色") && foldState() == QStringLiteral("展开"),
                   "fold row is one plain button with a 展开/收起 word and no arrow");
            fold->click();
            expect(!body->isHidden(), "advanced editor expands without opening a new page");
            expect(foldState() == QStringLiteral("收起"), "fold row switches its word to 收起 once open");
            editor.setSpec(Theme::defaultSpec());
            expect(emitted == 0, "refreshing advanced editor never writes theme settings");
            editor.setRoleColor(int(Theme::Role::Base), QColor("#123456"));
            expect(edited.roles.value(int(Theme::Role::Base)) == QColor("#123456"), "advanced role color edited");
            editor.setRoleColor(int(Theme::Role::Base), {});
            expect(!edited.roles.contains(int(Theme::Role::Base)), "role follows preset after reset");
            auto spec = Theme::defaultSpec();
            auto &style = spec.parts[int(Theme::Part::PageBackground)];
            style.hasImage = true; style.imagePath = "C:/preview/background.png";
            style.imageName = "background.png"; style.imageOpacity = 61;
            editor.setSpec(spec);
            parts->setCurrentIndex(parts->findData(int(Theme::Part::PageBackground)));
            editor.setPartColor(int(Theme::Part::PageBackground), false, QColor("#abcdef"));
            opacity->setValue(42);
            expect(edited.parts.value(int(Theme::Part::PageBackground)).textOpacity == 42, "part text opacity edited");
            editor.resetPart(int(Theme::Part::PageBackground));
            const auto preserved = edited.parts.value(int(Theme::Part::PageBackground));
            expect(!preserved.hasBackground && !preserved.hasForeground && preserved.textOpacity == -1 &&
                       preserved.hasImage && preserved.imageOpacity == 61 && preserved.imageName == "background.png",
                   "resetting part colors preserves background image and image opacity");
            parts->setCurrentIndex(parts->findData(int(Theme::Part::Scrim)));
            expect(!opacity->isEnabled() && !editor.findChild<QPushButton *>("themePartForeground")->isEnabled(),
                   "scrim has no misleading text controls");
            const int before = emitted;
            editor.setRoleColor(-1, Qt::red);
            editor.setPartColor(Theme::partCount(), false, Qt::red);
            expect(emitted == before, "invalid role and part cannot alter spec");
        }
        printf("Theme check: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    
 return 0;
}


#include "Support.hpp"
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
int UiThemePaletteChecks(QApplication &app, QTabWidget * &tabs, QFrame * &bar) {
    {
        // 比色一律拿 `name(HexArgb)` 比字符串，不用 `QColor::operator==` —— 后者在 Qt 6 里
        // 比的是内部那 16 位分量（同一个八位色名构造两次也可能因为路径不同而不相等），
        // 这条坑 在别处踩过（见「启动提醒」那一段）。
        const auto sameColor = [](const QColor &a, const QColor &b) {
            return a.isValid() && b.isValid() && a.name(QColor::HexArgb) == b.name(QColor::HexArgb);
        };

        // ---------- W1 出厂那一份 = 暮色雪山，一项覆盖都没有 ----------
        // 出厂预设不再是"跟着系统"、也不再是冰川碧湖：新安装铺的就是**暮色雪山**那一套
        // tokens（见 `Theme::defaultSpec()`），主题色是它自己的 primary。所以判据是
        // "铺下去之后应用调色板就是这一支 tokens"，而不是"和平台给的那一份一个字节不差"。
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        const Theme::Tokens factoryTokens = Theme::tokens();
        check(app.palette().color(QPalette::Window).rgb() == factoryTokens.page.rgb() &&
                  app.palette().color(QPalette::WindowText).rgb() == factoryTokens.text.rgb(),
              "★★ W1 出厂那一份铺下去 = 应用调色板就是暮色雪山那一支 tokens",
              QStringLiteral("量到 %1 / %2，该是 %3 / %4")
                  .arg(app.palette().color(QPalette::Window).name(),
                       app.palette().color(QPalette::WindowText).name(),
                       factoryTokens.page.name(), factoryTokens.text.name()));
        check(Theme::current().presetId == QStringLiteral("dark") &&
                  Theme::current().roles.isEmpty() && Theme::current().parts.isEmpty() &&
                  Theme::current().textOpacity == 100,
              "★ W1 出厂那一份就是「暮色雪山 + 一项覆盖都没有」",
              Theme::current().presetId);
        check(sameColor(Theme::current().accent, Theme::tokens(QStringLiteral("dark")).primary) &&
                  !sameColor(Theme::current().accent, Theme::defaultAccentColor()),
              "★★ W1 出厂主题色 = 暮色雪山那一支 primary（不是 `defaultAccentColor()` 那一支冰川碧湖）",
              QStringLiteral("%1 / 预设那一支 %2 / 冰川碧湖 %3")
                  .arg(Theme::current().accent.name(),
                       Theme::tokens(QStringLiteral("dark")).primary.name(),
                       Theme::defaultAccentColor().name()));

        // ---------- W2 深色 / 浅色两个预设 ----------
        // 预设现在**自带颜色表**（`tokens(id)`）：`dark` 是暮色雪山那一套、`emerald` 是冰川碧湖
        // 那一套，和平台插件认不认 `ColorScheme` 没关系（`Theme::apply()` 里那次
        // `setColorScheme()` 只是告诉平台"想要哪一档"）。所以这里直接量两份调色板：
        // 一深一浅，而且深的那份真的是深色。
        //
        // NOTE: `"light"` 是**旧名字**，`normalize()` 会把它认成 `emerald`（老盘上的预设
        // 不许一读回来就变回出厂）—— W7 钉着那条往返。
        Theme::setSpec(Theme::presetSpec(QStringLiteral("dark")));
        Theme::apply(app);
        const QPalette darkPalette = app.palette();
        Theme::setSpec(Theme::presetSpec(QStringLiteral("emerald")));
        Theme::apply(app);
        const QPalette lightPalette = app.palette();
        check(Theme::isDarkPalette(darkPalette) && !Theme::isDarkPalette(lightPalette),
              "★★ W2 深色 / 浅色两个预设铺出来的窗口底色真的一深一浅",
              QStringLiteral("深 %1（明度 %2）/ 浅 %3（明度 %4）")
                  .arg(darkPalette.color(QPalette::Window).name())
                  .arg(darkPalette.color(QPalette::Window).lightness())
                  .arg(lightPalette.color(QPalette::Window).name())
                  .arg(lightPalette.color(QPalette::Window).lightness()));
        check(darkPalette.color(QPalette::Window).rgb() ==
                  Theme::tokens(QStringLiteral("dark")).page.rgb(),
              "★ W2 深色预设的窗口底色就是它自己那一支 tokens（不跟平台那一份走）",
              darkPalette.color(QPalette::Window).name());

        // 语义色**按当前这一档现推**（用户没点名要一个色的时候）：深底上用浅红，不然那处红
        // 会糊在深底里。两个调色板是自己捏的 —— 要量的就是"它看的是底色的明度"，不是
        // "系统这一档叫什么"。
        QPalette artificialDark;
        artificialDark.setColor(QPalette::Window, QColor(0x20, 0x20, 0x20));
        QPalette artificialLight;
        artificialLight.setColor(QPalette::Window, QColor(0xef, 0xef, 0xef));
        const QColor darkDanger = Theme::dangerColor(artificialDark);
        const QColor lightDanger = Theme::dangerColor(artificialLight);
        check(Theme::isDarkPalette(artificialDark) && !Theme::isDarkPalette(artificialLight) &&
                  colorDistance(darkDanger, lightDanger) > 0 &&
                  darkDanger.lightness() > lightDanger.lightness(),
              "★★ W2 「算不算深色」看的是窗口底色的**明度**（不是哪个标志位），语义色按它现推："
              "深底上用浅红、浅底上用深红",
              QStringLiteral("深 %1 / 浅 %2").arg(darkDanger.name(), lightDanger.name()));

        // 用户点名要一个语义色时，一律用他那个（预设怎么推都不管）。
        Theme::Spec accentSpec = Theme::presetSpec(QStringLiteral("dark"));
        accentSpec.danger = QColor(0x00, 0x88, 0xff);
        Theme::setSpec(accentSpec);
        check(sameColor(Theme::dangerColor(artificialDark), QColor(0x00, 0x88, 0xff)) &&
                  sameColor(Theme::dangerColor(artificialLight), QColor(0x00, 0x88, 0xff)),
              "★ W2 单独指定过一个语义色之后，深浅两档用的都是**他指定的那个**",
              Theme::dangerColor(artificialLight).name());

        // ---------- W3 角色覆盖落到应用调色板上 ----------
        // 这一拨是"调色板里本来就有位置"的那些，所以判据就是"应用调色板上那个角色变了没有"
        // —— 界面里所有读 `palette()` 的地方自动跟着走，不用挨个去改。
        Theme::Spec roleSpec = Theme::presetSpec(QStringLiteral("light"));
        roleSpec.roles.insert(int(Theme::Role::Window), QColor(0x12, 0x34, 0x56));
        roleSpec.roles.insert(int(Theme::Role::Highlight), QColor(0x65, 0x43, 0x21));
        Theme::setSpec(roleSpec);
        Theme::apply(app);
        check(app.palette().color(QPalette::Window).name() == QLatin1String("#123456") &&
                  app.palette().color(QPalette::Highlight).name() == QLatin1String("#654321"),
              "★★ W3 单独改一个**角色**（内容区底色 / 选中那一块）落到应用调色板上",
              QStringLiteral("底色 %1，选中色 %2")
                  .arg(app.palette().color(QPalette::Window).name(),
                       app.palette().color(QPalette::Highlight).name()));

        // ---------- W4 文字不透明度是**乘**上去的 ----------
        // 次要文字的出厂值现在是**不透明**的（tokens 里那一支），所以要量"乘"这件事
        // 得先给它一份**带透明度**的角色覆盖（用户自己挑的那一档），再看 50% 之后
        // 是不是**再乘一半**（128 → 64）。直接赋值那一版会给出 127 —— 两下就分得开。
        Theme::Spec opaqueSpec = Theme::presetSpec(QStringLiteral("emerald"));
        opaqueSpec.roles.insert(int(Theme::Role::PlaceholderText), QColor(0x30, 0x40, 0x50, 128));
        Theme::setSpec(opaqueSpec);
        Theme::apply(app);
        const QColor textFull = app.palette().color(QPalette::WindowText);
        const QColor placeholderFull = app.palette().color(QPalette::PlaceholderText);
        Theme::Spec halfSpec = opaqueSpec;
        halfSpec.textOpacity = 50;
        Theme::setSpec(halfSpec);
        Theme::apply(app);
        const QColor textHalf = app.palette().color(QPalette::WindowText);
        const QColor placeholderHalf = app.palette().color(QPalette::PlaceholderText);
        check(textFull.alpha() == 255 && textHalf.alpha() > 0 && textHalf.alpha() < textFull.alpha(),
              "★★ W4 「文字透明度 50」真的把正文压淡了（不是只写在规格里）",
              QStringLiteral("正文 alpha %1 → %2").arg(textFull.alpha()).arg(textHalf.alpha()));
        // 这一条是"乘"和"盖"的分界：`PlaceholderText` 已经被用户挑成半透明了，
        // 直接赋值等于把它从 128 变成 127（"和正文一样清楚"那一档），乘出来则是 64。
        check(placeholderFull.alpha() == 128 && placeholderHalf.alpha() == 64,
              "★★ W4 不透明度是**乘**上去的：本来就半透明的次要文字跟着一起变淡"
              "（直接赋值会把它变得和正文一样清楚）",
              QStringLiteral("次要文字 alpha %1 → %2")
                  .arg(placeholderFull.alpha())
                  .arg(placeholderHalf.alpha()));

        // ---------- W5 每一块骨架的出厂值 = tokens 里那一支 ----------
        // 这一拨（标题栏 / 底栏 / 浮层面板 / 卡片 / 四周变暗）在调色板里**没有位置**，
        // 它们的出厂值由当前预设那一支 tokens 决定（见 `Theme::partBackground()`）：
        // 标题栏 = 主题色加深一档（`deep`）、底栏 = `subtle`、浮层面板/卡片 = `card`。
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        const QColor factoryDeep = Theme::tokens().deep;
        check(sameColor(Theme::partBackground(Theme::Part::TitleBar), factoryDeep),
              "★★ W5 标题栏底色（没覆盖过）= **tokens 的 deep 那一档**（和主题色同一支，"
              "暮色雪山下就是本色）",
              Theme::partBackground(Theme::Part::TitleBar).name());
        check(sameColor(Theme::partBackground(Theme::Part::BottomBar), Theme::tokens().subtle),
              "★ W5 底栏 = `subtle`（它装的是启动/日志那一排控件）",
              Theme::partBackground(Theme::Part::BottomBar).name());
        check(!sameColor(Theme::partBackground(Theme::Part::TitleBar),
                         Theme::partBackground(Theme::Part::BottomBar)),
              "★ W5 所以上下两条**不再是同一个色**（这一条钉的是「改一处不会被另一处悄悄跟走」）");
        check(Theme::partLightInk(Theme::Part::TitleBar) &&
                  !Theme::partLightInk(Theme::Part::BottomBar),
              "★★ W5 只有标题栏的出厂字色是**白墨**（这一块压着主题色那一支，只有白读得出来；"
              "底栏不是那一支，字色跟调色板走）");
        check(sameColor(Theme::partForeground(Theme::Part::TitleBar, QColor(0x11, 0x22, 0x33)),
                        QColor(Qt::white)),
              "★★ W5 没人覆盖过时标题栏的字色就是白的（递进去那个兜底色**被忽略**）",
              Theme::partForeground(Theme::Part::TitleBar, QColor(0x11, 0x22, 0x33)).name());
        check(sameColor(Theme::partForeground(Theme::Part::BottomBar, QColor(0x11, 0x22, 0x33)),
                        QColor(0x11, 0x22, 0x33)),
              "★ W5 别的块照旧：没人覆盖过时就是调用方递进来那个兜底色");
        check(sameColor(Theme::partBackground(Theme::Part::Panel), Theme::tokens().card),
              "★ W5 浮层面板 = `card`",
              Theme::partBackground(Theme::Part::Panel).name());
        check(sameColor(Theme::partBackground(Theme::Part::Card), Theme::tokens().card),
              "★ W5 卡片 = `card`（压在内容区上那张卡片和列表底同一档）",
              Theme::partBackground(Theme::Part::Card).name());
        check(Theme::partBackground(Theme::Part::Scrim).rgb() == Theme::tokens().scrim.rgb() &&
                  qAbs(Theme::partBackground(Theme::Part::Scrim).alphaF() -
                       Theme::tokens().scrim.alphaF() * (Theme::isDark() ? 0.6 : 1.0)) < 0.01,
              "★★ W5 四周变暗那一层 = 预设 scrim 色、深色不透明度 ×0.6、浅色照原值（轻压一档，不是黑块）",
              Theme::partBackground(Theme::Part::Scrim).name(QColor::HexArgb));
        check(sameColor(OverlayHost::scrimColor(), Theme::partBackground(Theme::Part::Scrim)),
              "★★ W5 覆盖层问到的「四周变暗」和这一层是**同一个**色（不是两份各写一遍）");

        // ---------- W5b 一个**已经立着的**真控件会不会自己重算 ----------
        // 这一条才是这一层真正的难点：条自己持有一份算好的色（`setPalette()`），"应用调色板
        // 变了"这件事不会自动改它 —— 得靠 `changeEvent()` 里那一挂。不挂就是"换了主题之后
        // 顶上那一条还停在旧色上"，而那正是这个功能最容易被骂的地方。
        // 7a：标题栏换成左导航轨，轨本身是磨砂玻璃（不铺自己的底，`autoFillBackground` 关着），
        // 用户在个性化里给「标题栏」单独设过底色时才铺那一层（见 TitleBar::paintEvent）。
        // 这里量的是"字色跟着预设重算"。
        TitleBar bar;
        bar.setFixedSize(TitleBar::railWidth(), 400);
        bar.show();
        QApplication::processEvents();
        check(!bar.autoFillBackground() &&
                  sameColor(bar.palette().color(QPalette::WindowText), Theme::tokens().text),
              "★ W5b 导航轨立起来时不铺自己的底（磨砂），字色就是当前预设的正文色",
              bar.palette().color(QPalette::WindowText).name());
        // 这一条才是这一节的重点：换一个**主题色**（不是换预设）之后，已经立着的那条
        // 得自己把底色重算一遍。它是新加的一位，正好拿来把"重算"这件事
        // 从"换个预设试试"升级成"换一个色也管用"。
        Theme::Spec recolor = Theme::defaultSpec();
        recolor.accent = QColor(0x33, 0x66, 0x99);
        Theme::setSpec(recolor);
        Theme::apply(app);
        QApplication::processEvents();
        check(!bar.autoFillBackground() &&
                  sameColor(bar.palette().color(QPalette::WindowText), Theme::tokens().text),
              "★★ W5b 换一个**主题色**之后，已经立着的导航轨照旧不铺底、字色跟着重算",
              QStringLiteral("字 %1 / 正文色 %2")
                  .arg(bar.palette().color(QPalette::WindowText).name(), Theme::tokens().text.name()));
        const QString sheetBefore = Theme::widgetStyleSheet();
        Theme::setSpec(Theme::presetSpec(QStringLiteral("dark")));
        Theme::apply(app);
        QApplication::processEvents();
        // 应用级样式表换主题时必须一字不差：变了就会在换调色板**之前**重挂一遍，
        // 跟主题算色的控件拿到旧色（实例页那排 chip 曾因菜单样式写死颜色停在深色上）。
        check(Theme::widgetStyleSheet() == sheetBefore,
              "★★ W5c 换主题前后应用级样式表一字不差（颜色一律走 palette(...)，不写死 token）");
        check(sameColor(bar.palette().color(QPalette::WindowText), Theme::tokens().text),
              "★★ W5b 换主题（预设）之后**已经立着的**导航轨自己把字色重算了一遍"
              "（它持有一份算好的色，不重算就停在旧色上）",
              QStringLiteral("字 %1，该是 %2")
                  .arg(bar.palette().color(QPalette::WindowText).name(),
                       Theme::tokens().text.name()));
        bar.hide();
        // 回出厂那一份：上面那一段是**改过主题色**的，不还回去的话后面每一条量的都是
        // 那个 #336699（这一节的收尾处也会铺回出厂那一份，但中间那些断言量的是"当前这一份"）。
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        QApplication::processEvents();

        // ---------- W6 `follow()` 只跑一遍，而且不会自己咬自己 ----------
        QWidget probe;
        int hits = 0;
        Theme::follow(&probe, [&hits]() { ++hits; });
        QEvent paletteEvent(QEvent::PaletteChange);
        QApplication::sendEvent(&probe, &paletteEvent);
        check(hits == 1, "★★ W6 `follow()` 登记之后收到「调色板变了」就跑一遍", QString::number(hits));
        QEvent styleEvent(QEvent::StyleChange);
        QApplication::sendEvent(&probe, &styleEvent);
        check(hits == 2,
              "★ W6 `StyleChange` 也算数（QSS 里嵌死的颜色是跟着样式重新解析的）",
              QString::number(hits));

        // 防自咬那一位（`PaletteFollower::m_running`）：回调里那句 `setPalette()` 自己也会
        // 发一次 `PaletteChange`，没有那一位就是"重算 → 又收到 → 再重算"。
        // NOTE: 兜底那一句（跑过 5 趟就不设了）是给"这一位失效"这种情况留的：真失效时这里
        // 会是一条 FAIL，而不是把这个一万九千行的验证跑成栈溢出、什么都查不出来。
        QWidget biter;
        int bites = 0;
        QPalette altPalette = QApplication::palette();
        altPalette.setColor(QPalette::Window, QColor(1, 2, 3));
        Theme::follow(&biter, [&bites, &biter, &altPalette]() {
            if (++bites > 5) {
                return;
            }
            biter.setPalette(altPalette);
        });
        QApplication::sendEvent(&biter, &paletteEvent);
        check(bites == 1,
              "★★ W6 回调里再 `setPalette()` 不会把这一条自己再咬起来（防自咬那一位真的在挡）",
              QStringLiteral("跑了几趟 %1").arg(bites));

        // ---------- W7 落盘往返 ----------
        // NOTE: 这一段**要写盘**，所以先把这个进程的 QSettings 窝换成一对专供验证的名字，
        // 量完清干净再换回来 —— 绝不能碰产品那一份（`WTFML/WTFML` 底下是用户真在用的
        // 设置，验证跑一趟把主题改了是绝对不能出的错）。
        const QString savedOrg = QCoreApplication::organizationName();
        const QString savedAppName = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("WTFML-verify"));
        QCoreApplication::setApplicationName(QStringLiteral("ui_check"));
        QSettings().clear();

        Theme::Spec written = Theme::presetSpec(QStringLiteral("dark"));
        written.textOpacity = 70;
        written.accent = QColor(0x33, 0x66, 0x99);   // 主题色也过一趟盘
        written.roles.insert(int(Theme::Role::Window), QColor(0x0a, 0x0b, 0x0c));
        Theme::PartStyle panelStyle;
        panelStyle.hasBackground = true;
        panelStyle.background = QColor(0x11, 0x22, 0x33);
        panelStyle.hasForeground = true;
        panelStyle.foreground = QColor(0x44, 0x55, 0x66);
        panelStyle.textOpacity = 40;
        written.parts.insert(int(Theme::Part::Panel), panelStyle);
        written.danger = QColor(0xaa, 0x00, 0x11);
        Theme::save(written);
        const Theme::Spec readBack = Theme::load();
        const Theme::PartStyle panelBack = readBack.parts.value(int(Theme::Part::Panel));
        check(readBack.presetId == written.presetId && readBack.textOpacity == 70 &&
                  sameColor(readBack.roles.value(int(Theme::Role::Window)),
                            written.roles.value(int(Theme::Role::Window))) &&
                  panelBack.hasBackground && sameColor(panelBack.background, panelStyle.background) &&
                  panelBack.hasForeground && sameColor(panelBack.foreground, panelStyle.foreground) &&
                  panelBack.textOpacity == 40 && readBack.danger.isValid() &&
                  sameColor(readBack.danger, written.danger) &&
                  sameColor(readBack.accent, written.accent),
              "★★ W7 写进盘上再读回来是同一份（预设 / 不透明度 / 主题色 / 角色 / 骨架三项 / 语义色）",
              QStringLiteral("预设 %1，不透明度 %2，角色 %3 项，骨架 %4 项")
                  .arg(readBack.presetId)
                  .arg(readBack.textOpacity)
                  .arg(readBack.roles.size())
                  .arg(readBack.parts.size()));

        // 第二次存的是一份**没有覆盖项**的：上一次那几项不许"复活"（`save()` 先把整组
        // `Theme` 清掉再写）。不这么做的话用户"把标题栏改回默认"重开一次又变回去。
        //
        // NOTE: 这里故意喂**旧名字** `"light"`：`normalize()` 把它认成 `emerald`，
        // 所以读回来是 `emerald` 而不是 `light` —— 老盘上存的浅色预设不会一读回来就
        // 变回出厂，也不会变成"第四个预设"。
        Theme::save(Theme::presetSpec(QStringLiteral("light")));
        const Theme::Spec afterClear = Theme::load();
        check(afterClear.roles.isEmpty() && afterClear.parts.isEmpty() &&
                  !afterClear.danger.isValid() &&
                  afterClear.presetId == QStringLiteral("emerald"),
              "★★ W7 第二次存一份**没有覆盖项**的，上一次那几项不会复活"
              "（不然「改回默认」重开一次又变回去）；旧名字 `light` 读回来就是 `emerald`",
              QStringLiteral("角色 %1 项，骨架 %2 项，预设 %3")
                  .arg(afterClear.roles.size())
                  .arg(afterClear.parts.size())
                  .arg(afterClear.presetId));

        // 出厂默认换成暮色雪山之后，**盘上存过预设的人不受影响**：存的是冰川碧湖就读回冰川碧湖，
        // 主题色也还是那一支（新默认只落在 `Theme/Preset` 这一项从没写过的人身上）。
        Theme::save(Theme::presetSpec(QStringLiteral("emerald")));
        const Theme::Spec storedEmerald = Theme::load();
        check(storedEmerald.presetId == QStringLiteral("emerald") &&
                  sameColor(storedEmerald.accent, Theme::tokens(QStringLiteral("emerald")).primary),
              "★★ W7 盘上存过「冰川碧湖」的人读回来还是冰川碧湖（新默认只影响没存过预设的人）",
              QStringLiteral("预设 %1 / 主题色 %2 / 那一支 %3")
                  .arg(storedEmerald.presetId, storedEmerald.accent.name(),
                       Theme::tokens(QStringLiteral("emerald")).primary.name()));

        // ---------- W8 盘上是垃圾（注册表能手改）⇒ 一律退回出厂 ----------
        QSettings raw;
        raw.setValue(QStringLiteral("Theme/Preset"), QStringLiteral("no-such-preset"));
        raw.setValue(QStringLiteral("Theme/TextOpacity"), 999);
        raw.setValue(QStringLiteral("Theme/Roles/window"), QStringLiteral("not-a-color"));
        raw.setValue(QStringLiteral("Theme/Parts/panel/Background"), QStringLiteral("also-not-a-color"));
        raw.setValue(QStringLiteral("Theme/Parts/titleBar/TextOpacity"), -7);
        raw.sync();
        const Theme::Spec salvaged = Theme::load();
        check(salvaged.presetId == QStringLiteral("dark") && salvaged.textOpacity == 100 &&
                  salvaged.roles.isEmpty() && salvaged.parts.isEmpty(),
              "★★ W8 盘上手改出来的垃圾一律退回出厂那一档（暮色雪山）"
              "（认不出的预设 / 越界的不透明度 / 不是颜色的字符串 / 负数的不透明度）",
              QStringLiteral("预设 %1，不透明度 %2，角色 %3 项，骨架 %4 项")
                  .arg(salvaged.presetId)
                  .arg(salvaged.textOpacity)
                  .arg(salvaged.roles.size())
                  .arg(salvaged.parts.size()));

        {
            QSettings().clear();
            QSettings junk;
            junk.setValue(QStringLiteral("Theme/TextOpacity"), QStringLiteral("abc"));
            junk.setValue(QStringLiteral("Theme/Parts/panel/Image"), QStringLiteral("C:/tmp/pic.png"));
            junk.setValue(QStringLiteral("Theme/Parts/panel/ImageOpacity"), QStringLiteral("abc"));
            junk.setValue(QStringLiteral("Theme/Parts/panel/TextOpacity"), QStringLiteral("oops"));
            junk.sync();
            const Theme::Spec salvagedNumbers = Theme::load();
            const Theme::PartStyle panelNumbers =
                salvagedNumbers.parts.value(int(Theme::Part::Panel));
            check(salvagedNumbers.textOpacity == 100 && panelNumbers.imageOpacity == 100 &&
                      panelNumbers.textOpacity < 0,
                  "★★ W8 手改出来的「不是数字」的不透明度不许被读成 0"
                  "（0% 的文字 = 界面一片空白、0% 的图 = 看不见的背景；认不出就退回出厂那一档）",
                  QStringLiteral("全局 %1，图 %2，这一块的字 %3")
                      .arg(salvagedNumbers.textOpacity)
                      .arg(panelNumbers.imageOpacity)
                      .arg(panelNumbers.textOpacity));
        }

        QSettings().clear();
        QCoreApplication::setOrganizationName(savedOrg);
        QCoreApplication::setApplicationName(savedAppName);

        // ---------- W9 每一块的键唯一、存下去能认回来 ----------
        // 这就是"复制粘贴出来的重复键"的照妖镜：两张表里键一模一样的话，盘上那一项会被后来
        // 那个盖掉，而界面上是"改了这一块、那一块跟着变"。
        QSet<QString> partKeys;
        bool partsRoundTrip = true;
        for (int i = 0; i < Theme::partCount(); ++i) {
            const Theme::Part part = static_cast<Theme::Part>(i);
            const QString key = Theme::partKey(part);
            Theme::Part back = Theme::Part::Count;
            if (key.isEmpty() || partKeys.contains(key) || !Theme::partFromKey(key, &back) ||
                back != part) {
                partsRoundTrip = false;
            }
            partKeys.insert(key);
        }
        Theme::Part bogus;
        check(partsRoundTrip && partKeys.size() == Theme::partCount() &&
                  !Theme::partFromKey(QStringLiteral("no-such-part"), &bogus),
              "★★ W9 每一块骨架的存盘键唯一、且存下去能原样认回来（认不出的返回假）",
              QStringLiteral("%1 块 / %2 个键").arg(Theme::partCount()).arg(partKeys.size()));

        QSet<QString> roleKeys;
        bool rolesRoundTrip = true;
        for (int i = 0; i < Theme::roleCount(); ++i) {
            const Theme::Role role = static_cast<Theme::Role>(i);
            const QString key = Theme::roleKey(role);
            Theme::Role back = Theme::Role::Count;
            if (key.isEmpty() || roleKeys.contains(key) || !Theme::roleFromKey(key, &back) ||
                back != role) {
                rolesRoundTrip = false;
            }
            roleKeys.insert(key);
        }
        Theme::Role bogusRole;
        check(rolesRoundTrip && roleKeys.size() == Theme::roleCount() &&
                  !Theme::roleFromKey(QStringLiteral("no-such-role"), &bogusRole),
              "★★ W9 每一个能改的角色的存盘键唯一、且存下去能原样认回来",
              QStringLiteral("%1 个角色 / %2 个键").arg(Theme::roleCount()).arg(roleKeys.size()));

        check(Theme::presetIds().size() == 4 &&
                  Theme::presetIds().value(0) == QStringLiteral("dark") &&
                  Theme::presetIds().contains(QStringLiteral("emerald")) &&
                  Theme::presetIds().contains(QStringLiteral("ocean")) &&
                  Theme::presetIds().contains(QStringLiteral("dark")) &&
                  Theme::isPresetId(QStringLiteral("system")) &&
                  // 旧名字还认（盘上存过 `light` 的老用户）
                  Theme::isPresetId(QStringLiteral("light")) &&
                  Theme::isPresetId(QStringLiteral("dark")) &&
                  !Theme::isPresetId(QStringLiteral("custom")) &&
                  !Theme::presetName(QStringLiteral("dark")).isEmpty(),
              "★ W9 四个预设「暮色雪山 / 冰川碧湖 / 落日海崖 / 跟着系统」，暮色雪山排第一（出厂默认）"
              "（**故意没有『自定义』**：覆盖项是叠在预设上面的，换了覆盖项预设还是那一个）；"
              "旧名字 `light` 仍然认",
              Theme::presetIds().join(QLatin1Char('/')));

        // ---------- W10 只改一块骨架 ⇒ 不碰应用调色板 ----------
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        const QPalette beforePartOnly = app.palette();
        Theme::Spec scrimSpec = Theme::defaultSpec();
        Theme::PartStyle scrimStyle;
        scrimStyle.hasBackground = true;
        scrimStyle.background = QColor(0, 0, 0, 200);
        scrimSpec.parts.insert(int(Theme::Part::Scrim), scrimStyle);
        Theme::setSpec(scrimSpec);
        Theme::apply(app);
        check(app.palette() == beforePartOnly,
              "★★ W10 只改一块骨架时**不碰调色板**（只有那一块重算，其余一个字节不动）",
              QStringLiteral("窗口底色 %1").arg(app.palette().color(QPalette::Window).name()));
        check(sameColor(Theme::partBackground(Theme::Part::Scrim), QColor(0, 0, 0, 200)) &&
                  sameColor(OverlayHost::scrimColor(), QColor(0, 0, 0, 200)),
              "★★ W10 四周变暗那一层单独改过之后，覆盖层问到的就是改过的那个"
              "（四周变暗的深浅是能单独调的）",
              Theme::partBackground(Theme::Part::Scrim).name(QColor::HexArgb));

        // ---------- W11 背景图（主页左栏 / 页面背景能铺一张图） ----------
        // 要求是「能给主页左侧自定义图像吗？其他页面的背景能换图吗」。这一节量五件事：
        // ① 四种摆法的键 / 名字 / 认回来；
        // ② `normalize()` 对"路径 / 摆法 / 不透明度"这三位怎么收拾；
        // ③ 存盘往返（含注册表里那串是 `\`、读回来是 `/` 这一层故意的来回）；
        // ④ `partHasBackground()` —— 这两块出厂时**一项都没设过**；
        // ⑤ ★ **没设图时那一块一个像素都不画**。
        QSet<QString> fitKeys;
        bool fitsRoundTrip = true;
        for (int i = 0; i < Theme::imageFitCount(); ++i) {
            const auto fit = static_cast<Theme::ImageFit>(i);
            const QString key = Theme::imageFitKey(fit);
            Theme::ImageFit back = Theme::ImageFit::Count;
            if (key.isEmpty() || fitKeys.contains(key) || !Theme::imageFitFromKey(key, &back) ||
                back != fit) {
                fitsRoundTrip = false;
            }
            fitKeys.insert(key);
        }
        Theme::ImageFit bogusFit;
        check(fitsRoundTrip && fitKeys.size() == Theme::imageFitCount() &&
                  !Theme::imageFitFromKey(QStringLiteral("no-such-fit"), &bogusFit),
              "★★ W11 四种摆法的存盘键唯一、且存下去能原样认回来（认不出的返回假）",
              QStringLiteral("%1 种 / %2 个键").arg(Theme::imageFitCount()).arg(fitKeys.size()));

        Theme::Spec messy = Theme::defaultSpec();
        Theme::PartStyle messyStyle;
        messyStyle.imagePath = QStringLiteral("  C:/tmp/pic.png  ");
        messyStyle.fit = static_cast<Theme::ImageFit>(99);  // 盘上手改歪了的那种
        messyStyle.imageOpacity = 250;
        messy.parts.insert(int(Theme::Part::PageBackground), messyStyle);
        const Theme::PartStyle tidy = Theme::normalize(messy).parts.value(
            int(Theme::Part::PageBackground));
        check(tidy.hasImage && tidy.imagePath == QStringLiteral("C:/tmp/pic.png") &&
                  tidy.fit == Theme::ImageFit::Cover && tidy.imageOpacity == 100,
              "★★ W11 路径去两头空白、摆法认不出退回「铺满」、不透明度钳到 0~100"
              "（**图还在**，只是那两位回到默认档 —— 为了一个摆法把用户挑的图整个忽略掉更糟）",
              QStringLiteral("路径 %1，摆法 %2，不透明度 %3")
                  .arg(tidy.imagePath)
                  .arg(static_cast<int>(tidy.fit))
                  .arg(tidy.imageOpacity));

        Theme::Spec blank = Theme::defaultSpec();
        Theme::PartStyle blankStyle;
        blankStyle.imagePath = QStringLiteral("   ");
        blank.parts.insert(int(Theme::Part::Card), blankStyle);
        check(Theme::normalize(blank).parts.isEmpty(),
              "★★ W11 只设过图、而路径是空白 ⇒ 这一块整条丢掉"
              "（「这一块设过」在盘上不许是假的，见 Theme.cpp 里那段）");

        // ④ 出厂那一份里这两块**一项都没设过**。这一位就是"没东西可画"的判据。
        // （卡片是这一节拿来量 `ThemeBackground` 画法的那一块，页面背景是产品里真正铺图的那一块。）
        Theme::setSpec(Theme::defaultSpec());
        check(!Theme::partHasBackground(Theme::Part::Card) &&
                  !Theme::partHasBackground(Theme::Part::PageBackground) &&
                  !Theme::partImage(Theme::Part::Card).isSet() &&
                  !Theme::partImage(Theme::Part::PageBackground).isSet(),
              "★★ W11 出厂那一份里卡片和页面背景**一项都没设过**"
              "（`partHasBackground()` 为假 ⇒ `ThemeBackground` 一笔都不画）",
              QStringLiteral("设过底色：卡片 %1 / 页面背景 %2")
                  .arg(Theme::partHasBackground(Theme::Part::Card) ? 1 : 0)
                  .arg(Theme::partHasBackground(Theme::Part::PageBackground) ? 1 : 0));

        // ⑤ 一像素一像素地量"画不画"。量的是卡片那一块 —— `ThemeBackground` 对每一块都是同一套
        // 画法（页面背景那一块另外有一条：磨砂开着时那张图由场景底铺在整窗最底下，不在这里量）。
        // 父控件刷成一个一眼认得出的色，子控件那一块就靠它判。
        {
            QTemporaryDir scratch;
            const QString picturePath = scratch.filePath(QStringLiteral("pic.png"));
            QImage picture(8, 4, QImage::Format_ARGB32);
            picture.fill(QColor(0xff, 0x00, 0xff));  // 洋红：界面上一个像素都不会有它
            const bool pictureWritten = picture.save(picturePath, "PNG");

            QWidget host;
            host.resize(40, 40);
            QPalette hostPalette = host.palette();
            const QColor hostColor(0x12, 0x34, 0x56);
            hostPalette.setColor(QPalette::Window, hostColor);
            host.setPalette(hostPalette);
            host.setAutoFillBackground(true);  // 父控件那一层得真的刷上去，不然量的是别的
            auto *background = new ThemeBackground(Theme::Part::Card, &host);
            background->setGeometry(0, 0, 40, 40);

            const QImage untouched = host.grab().toImage();
            const QColor cornerPlain = untouched.pixelColor(20, 20);
            check(pictureWritten && cornerPlain == hostColor,
                  "★★ W11 没设图的那一块**一个像素都不画**：整块原样透出底下那一层"
                  "（画一层「窗口底色」上去的话，选项卡那层底就被顶掉了 —— 屏幕上颜色悄悄变一档）",
                  QStringLiteral("里头那一点量到 %1，该是父控件那个 %2")
                      .arg(cornerPlain.name(), hostColor.name()));

            Theme::Spec withPicture = Theme::defaultSpec();
            Theme::PartStyle stretchStyle;
            stretchStyle.imagePath = picturePath;
            stretchStyle.fit = Theme::ImageFit::Stretch;
            stretchStyle.imageOpacity = 100;
            withPicture.parts.insert(int(Theme::Part::Card), stretchStyle);
            Theme::setSpec(withPicture);
            const QImage stretched = host.grab().toImage();
            check(stretched.pixelColor(20, 20) == QColor(0xff, 0x00, 0xff) &&
                      stretched.pixelColor(1, 1) == QColor(0xff, 0x00, 0xff),
              "★★ W11 设了图之后那一块真的铺满了（拉伸这种摆法连四个角都是图）",
              QStringLiteral("中间量到 %1").arg(stretched.pixelColor(20, 20).name()));

            // 「完整放下」：8×4 的图放进 40×40 的块里 ⇒ 上下各留一条底，中间那条才是图。
            Theme::PartStyle containStyle = stretchStyle;
            containStyle.fit = Theme::ImageFit::Contain;
            Theme::Spec withContain = Theme::defaultSpec();
            withContain.parts.insert(int(Theme::Part::Card), containStyle);
            Theme::setSpec(withContain);
            const QImage contained = host.grab().toImage();
            check(contained.pixelColor(20, 20) == QColor(0xff, 0x00, 0xff) &&
                      contained.pixelColor(20, 2) == hostColor,
              "★★ W11 「完整放下」四周留的是**底下那一层的底色**（不是我们替它算的一个色）",
              QStringLiteral("中间 %1 / 上边 %2")
                  .arg(contained.pixelColor(20, 20).name(),
                       contained.pixelColor(20, 2).name()));

            // 不透明度 0 = 一点都看不见 ⇒ 又回到"底下那一层"（这一位是用户让字读得清的旋钮）。
            Theme::PartStyle invisible = stretchStyle;
            invisible.imageOpacity = 0;
            Theme::Spec withInvisible = Theme::defaultSpec();
            withInvisible.parts.insert(int(Theme::Part::Card), invisible);
            Theme::setSpec(withInvisible);
            check(host.grab().toImage().pixelColor(20, 20) == hostColor,
                  "★ W11 不透明度调到 0 ⇒ 图上不来，底下那一层原样透出来");

            // ⑥ ★ ：换了一张图之后要**当场**重画。
            // 不然在设置页换图 / 清除 / 改摆法 / 改不透明度，屏幕上还是
            // 旧图 —— 得切到别的选项卡再切回来、或者拖一下窗口改尺寸，才看得见新的。
            // 根子：`ThemeBackground` 只靠 `Theme::follow()` 那条"调色板变了"重算，而
            // `apply()` 在调色板**没变**的时候（只换了图 / 摆法 / 不透明度 / 底色，
            // 这几样都不在 `buildPalette()` 里）一个通知都不发。
            // 这里用一份"碰调色板"的预设（emerald）走两趟：第一趟铺预设，第二趟只换摆法。
            host.show();
            QApplication::processEvents();

            class PaintCounter : public QObject {
            public:
                int paints = 0;
                bool eventFilter(QObject *, QEvent *event) override {
                    if (event->type() == QEvent::Paint) {
                        ++paints;
                    }
                    return false;
                }
            };
            PaintCounter counter;
            background->installEventFilter(&counter);

            Theme::PartStyle counterStyle = stretchStyle;
            Theme::Spec emeraldPicture = Theme::presetSpec(QStringLiteral("emerald"));
            emeraldPicture.parts.insert(int(Theme::Part::Card), counterStyle);
            Theme::setSpec(emeraldPicture);
            Theme::apply(*qApp);
            QApplication::processEvents();
            const int baselinePaints = counter.paints;

            // 第二趟：只把摆法从「拉伸」换成「完整放下」。调色板一个字节都不变。
            counterStyle.fit = Theme::ImageFit::Contain;
            Theme::Spec emeraldContain = emeraldPicture;
            emeraldContain.parts.insert(int(Theme::Part::Card), counterStyle);
            Theme::setSpec(emeraldContain);
            Theme::apply(*qApp);
            QApplication::processEvents();
            check(counter.paints > baselinePaints,
                  "★★ W11 只换图 / 摆法 / 不透明度（调色板没有变）也要**当场**重画，"
                  "不许等用户切选项卡或拖窗口才生效",
                  QStringLiteral("第二趟画了 %1 次（第一趟为止 %2 次）")
                      .arg(counter.paints - baselinePaints)
                      .arg(baselinePaints));

            background->removeEventFilter(&counter);
            Theme::setSpec(Theme::defaultSpec());
            Theme::apply(*qApp);
            QApplication::processEvents();
        }

        // ③ 存盘往返。和 W7 一样的规矩：这一段要写盘，先把 QSettings 窝换成一对专供验证的
        // 名字，量完清干净再换回来（绝不能碰产品那一份）。
        {
            const QString imageOrg = QCoreApplication::organizationName();
            const QString imageAppName = QCoreApplication::applicationName();
            QCoreApplication::setOrganizationName(QStringLiteral("WTFML-verify"));
            QCoreApplication::setApplicationName(QStringLiteral("ui_check"));
            QSettings().clear();

            Theme::Spec pictureSpec = Theme::defaultSpec();
            Theme::PartStyle style;
            style.imagePath = QStringLiteral("C:/tmp/pic.png");
            // 用户挑的那张原来叫什么（盘上那份叫 `card.png`，是槽位名）——设置页那一行
            // 「当前图片：…」写的就是这一位，所以它必须跟着图一起存回来。
            style.imageName = QStringLiteral("我挑的那张.png");
            style.fit = Theme::ImageFit::Tile;
            style.imageOpacity = 45;
            pictureSpec.parts.insert(int(Theme::Part::Card), style);
            Theme::save(pictureSpec);
            // 读回来那一步得**真的走一趟盘**：`save()` 只写盘，不碰内存里那一份
            // （产品里那一条路是 save 完再 apply）。拿内存里那份量就成了"自己问自己"。
            Theme::setSpec(Theme::load());

            const Theme::PartImage back = Theme::partImage(Theme::Part::Card);
            // 注册表里那一串**得是反斜杠**（用户在 regedit 里一眼认得出那是自己盘上的哪一条），
            // 读回来一律是 `/`（内部只认这一种）。这一来一回是故意的，见 `Theme::save()`。
            const QString onDisk = QSettings()
                                       .value(QStringLiteral("Theme/Parts/card/Image"))
                                       .toString();
            check(back.isSet() && back.path == QStringLiteral("C:/tmp/pic.png") &&
                      back.fit == Theme::ImageFit::Tile && back.opacity == 45 &&
                      onDisk == QDir::toNativeSeparators(QStringLiteral("C:/tmp/pic.png")),
                  "★★ W11 存进盘上再读回来是同一张图（路径 / 摆法 / 不透明度三位都回来了）",
                  QStringLiteral("注册表里是 %1，读回来是 %2").arg(onDisk, back.path));
            check(back.name == QStringLiteral("我挑的那张.png"),
                  "★★ W11 「用户挑的那张原来叫什么」也回来了（设置页那一行写的是它，"
                  "不是盘上那个槽位名）",
                  back.name);

            // 清掉（空路径）之后再存一遍：上一次那条**不许复活**（`save()` 先整组清掉再写）。
            Theme::Spec cleared = Theme::defaultSpec();
            Theme::PartStyle clearedStyle;
            clearedStyle.imagePath = QString();
            cleared.parts.insert(int(Theme::Part::Card), clearedStyle);
            Theme::save(cleared);
            Theme::setSpec(Theme::load());
            check(!Theme::partImage(Theme::Part::Card).isSet() &&
                      !Theme::load().parts.contains(int(Theme::Part::Card)),
                  "★★ W11 清了图再存 ⇒ 这一块整条没了（旧的那条不会自己复活）",
                  QStringLiteral("读回来还有 %1 块").arg(Theme::load().parts.size()));

            // 名字那一位**可以不在盘上**（手改注册表删掉了、或者以后降级回退留下的）：
            // 读回来是空的，界面那一侧退回落盘那个文件的名字（见 refreshThemeImageRows()）
            // —— 不许因为少一位就读不出话。
            {
                Theme::Spec nameless = Theme::defaultSpec();
                Theme::PartStyle namelessStyle;
                namelessStyle.imagePath = QStringLiteral("C:/tmp/pic.png");
                nameless.parts.insert(int(Theme::Part::Card), namelessStyle);
                Theme::save(nameless);
                Theme::setSpec(Theme::load());
                const Theme::PartImage back = Theme::partImage(Theme::Part::Card);
                check(back.isSet() && back.name.isEmpty(),
                      "★ W11 盘上没有那个名字时读回来是空的（界面那一侧要退回盘上那个名字）",
                      back.name);
            }
            // 反过来：**只有名字、没有图**的那一块不留（`normalize()` 在没图那一档把名字
            // 一起清掉，整块就没有"一项覆盖"了）。
            {
                Theme::Spec orphan = Theme::defaultSpec();
                Theme::PartStyle orphanStyle;
                orphanStyle.imageName = QStringLiteral("孤儿名字.png");
                orphan.parts.insert(int(Theme::Part::Card), orphanStyle);
                check(!Theme::normalize(orphan).parts.contains(int(Theme::Part::Card)),
                      "★ W11 只留着一个名字、没有图的那一块整条丢掉（名字不是一项覆盖）");
            }

            QSettings().clear();
            QCoreApplication::setOrganizationName(imageOrg);
            QCoreApplication::setApplicationName(imageAppName);
        }

        // ---------- W12 主题色那一支 + 按钮那一套外观 ----------
        // 要求：「使用 #56eb6c 作为默认主题颜色（顶栏用，顺便那个最小化和 x 都换成白的，
        // 合适的地方可以放稍微深/浅一点，按钮中心白色，按钮文本和边框使用这个（或者深一点
        // 浅一点）的颜色/黑色/红色）」。这一节量的是：
        //
        // ① 那几个"深一点浅一点"的档**各自是什么**（它们是算出来的，不是各写一遍）；
        // ② 按钮那一套外观**真的铺到了**一颗真按钮上（不是只写在样式表字符串里）；
        // ③ 主题色是**一位**（换一个色，顶栏 + 按钮 + 那几档一起跟着走）。
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        {
            // WCAG 的相对亮度 / 对比度 —— 那几档"深一点浅一点"存在的理由就是它，
            // 所以这儿自己算一遍，别拿"看着挺深"当判据。
            const auto luminance = [](const QColor &c) {
                const auto channel = [](double v) {
                    v /= 255.0;
                    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
                };
                return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) +
                       0.0722 * channel(c.blue());
            };
            const auto contrast = [&luminance](const QColor &a, const QColor &b) {
                const double la = luminance(a);
                const double lb = luminance(b);
                return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
            };
            const QColor pure = Theme::accentShade(Theme::AccentShade::Pure);
            const QColor ink = Theme::accentShade(Theme::AccentShade::Ink);
            const QColor edge = Theme::accentShade(Theme::AccentShade::Border);
            const QColor hover = Theme::accentShade(Theme::AccentShade::Hover);
            const QColor pressed = Theme::accentShade(Theme::AccentShade::Pressed);
            const Theme::Tokens factory = Theme::tokens();

            check(sameColor(pure, factory.primary) &&
                      !sameColor(pure, Theme::tokens(QStringLiteral("emerald")).primary),
                  "★★ W12 出厂主题色就是暮色雪山那一支 primary（不再是 `defaultAccentColor()`"
                  " 那一支冰川碧湖）",
                  QStringLiteral("%1 / 预设那一支 %2 / 冰川碧湖 %3")
                      .arg(pure.name(), factory.primary.name(),
                           Theme::tokens(QStringLiteral("emerald")).primary.name()));
            check(sameColor(Theme::partBackground(Theme::Part::TitleBar), factory.deep),
                  "★★ W12 顶栏铺的是 `deep` 那一档（暮色雪山下这一档就是主题色本色）",
                  Theme::partBackground(Theme::Part::TitleBar).name());
            check(ink.value() < pure.value() && edge.value() < pure.value() &&
                      edge.value() > ink.value(),
                  "★★ W12 「深一点」那两档真的更深、而且**边比字浅**（边只要看得见，字要读得清）",
                  QStringLiteral("纯 %1 / 边 %2 / 字 %3")
                      .arg(pure.value()).arg(edge.value()).arg(ink.value()));
            // 主按钮：底 = 主题色本色，悬停 / 按下是 tokens 里另写的两档，字是 `onPrimary`
            // （暮色雪山下是深字压浅紫）。三层底都得让那行字够正文那条 4.5:1。
            check(contrast(factory.onPrimary, factory.primary) >= 4.5 &&
                      contrast(factory.onPrimary, factory.primaryHover) >= 4.5 &&
                      contrast(factory.onPrimary, factory.primaryPressed) >= 4.5,
                  "★★ W12 主按钮那行字（`onPrimary`）压在本色 / 悬停 / 按下三层上都够 4.5:1",
                  QStringLiteral("字 %1 对本色 %2:1 / 悬停 %3:1 / 按下 %4:1")
                      .arg(factory.onPrimary.name())
                      .arg(contrast(factory.onPrimary, factory.primary), 0, 'f', 2)
                      .arg(contrast(factory.onPrimary, factory.primaryHover), 0, 'f', 2)
                      .arg(contrast(factory.onPrimary, factory.primaryPressed), 0, 'f', 2));
            check(factory.primaryHover.value() > factory.primary.value() &&
                      factory.primaryPressed.value() < factory.primary.value(),
                  "★★ W12 按钮悬停亮一档、按下暗一档（两档都在 tokens 里，不是滤镜现算）",
                  QStringLiteral("悬停 %1 / 本色 %2 / 按下 %3")
                      .arg(factory.primaryHover.name(), factory.primary.name(),
                           factory.primaryPressed.name()));
            check(hover.value() > pure.value() && pressed.value() > pure.value(),
                  "★★ W12 悬停 / 按下那两层是**往白里混**出来的淡底（都比本色浅，不靠"
                  "调亮 / 调暗滤镜）",
                  QStringLiteral("本色 %1 / 悬停 %2 / 按下 %3")
                      .arg(pure.name(), hover.name(), pressed.name()));

            // 换一个主题色 ⇒ 几档**一起**跟着走（唯一出处）。这一条钉的是"别把这几个色
            // 各自写死在按钮样式表里"。
            Theme::Spec other = Theme::defaultSpec();
            other.accent = QColor(0x88, 0x44, 0xcc);
            Theme::setSpec(other);
            const QColor otherInk = Theme::accentShade(Theme::AccentShade::Ink);
            check(!sameColor(Theme::partBackground(Theme::Part::TitleBar), pure) &&
                      !sameColor(otherInk, ink) && otherInk.value() < 0x88,
                  "★★ W12 换一个主题色：顶栏和那几档**一起**跟着走（不是各自写死一份）",
                  QStringLiteral("顶栏 %1 / 字 %2")
                      .arg(Theme::partBackground(Theme::Part::TitleBar).name(), otherInk.name()));

            // 盘上那个主题色认不出来（手改歪了）⇒ 退回当前预设那一支（和 `normalize()` 同一个
            // 规矩）。它没有"空"这个状态：界面上随时都有顶栏那一块要画。
            Theme::Spec broken = Theme::defaultSpec();
            broken.accent = QColor();
            check(sameColor(Theme::normalize(broken).accent,
                            Theme::tokens(Theme::defaultSpec().presetId).primary),
                  "★★ W12 主题色认不出（无效色）⇒ 退回预设那一支（顶栏不会画成空白）",
                  Theme::normalize(broken).accent.name());

            Theme::setSpec(Theme::defaultSpec());
            Theme::apply(app);

            // ---------- 按钮：属性 + 真的铺到那颗按钮上 ----------
            check(Theme::buttonKindKey(Theme::ButtonKind::Accent).isEmpty() &&
                      !Theme::buttonKindKey(Theme::ButtonKind::Primary).isEmpty() &&
                      !Theme::buttonKindKey(Theme::ButtonKind::Danger).isEmpty() &&
                      !Theme::buttonKindKey(Theme::ButtonKind::Neutral).isEmpty(),
                  "★ W12 普通按钮**不挂属性**（挂一个恒真的值只是让每颗按钮都带一位垃圾）");
            QPushButton plainButton;
            Theme::markButton(&plainButton, Theme::ButtonKind::Danger);
            check(plainButton.property("wtfmlButtonKind").toString() ==
                      Theme::buttonKindKey(Theme::ButtonKind::Danger),
                  "★ W12 markButton() 挂上去的属性名和样式表里选择器用的是**同一个**",
                  plainButton.property("wtfmlButtonKind").toString());
            QPushButton keepButton;
            Theme::markButton(&keepButton, Theme::ButtonKind::Accent);
            check(!keepButton.property("wtfmlButtonKind").isValid(),
                  "★ W12 把一颗按钮改回普通那一档 = 把那一位**抹掉**（不是写一个 'accent'）");

            // ---------- 动作高度那一档 ----------
            // 量的是**整颗控件**的高度，不是样式表里那个数：QSS 的 `min-height` 量的是
            // **内容**那一格 —— 这一套样式（6px 内边距 + 1px 边）配 `min-height: 32px`
            // 整颗量出来是 **46**（探针实测）。所以样式表里写的是
            // `actionHeight()` 减掉那一圈，而这一条盯的是最后那个数。
            // 三条一起量：普通 32 / 主动作 36 / **图标按钮按自己那一档**（写死 22 的那颗
            // 在 `min-height: 18px` 下会量成 32 —— `setFixedSize()` 拦不住 QSS 的
            // `min-height`，只有 `markIconButton()` 那一句能把它按回去）。
            {
                QWidget heightHost;
                auto *heightLayout = new QVBoxLayout(&heightHost);
                heightLayout->setContentsMargins(0, 0, 0, 0);
                auto *tallPlain = new QPushButton(QStringLiteral("确定"), &heightHost);
                auto *tallPrimary = new QPushButton(QStringLiteral("启动游戏"), &heightHost);
                Theme::markButton(tallPrimary, Theme::ButtonKind::Primary);
                auto *tallIcon = new QPushButton(&heightHost);
                tallIcon->setFixedSize(22, 22);
                Theme::markIconButton(tallIcon);
                heightLayout->addWidget(tallPlain);
                heightLayout->addWidget(tallPrimary);
                heightLayout->addWidget(tallIcon);
                heightHost.show();
                QApplication::processEvents();
                check(tallPlain->height() == Theme::actionHeight() &&
                          tallPrimary->height() == Theme::primaryActionHeight() &&
                          tallIcon->height() == 22,
                      "★★ 动作按钮 `actionHeight()` / 主动作 `primaryActionHeight()`"
                      "（量整颗高度）/ 图标按钮按自己钉死的边长来",
                      QStringLiteral("普通 %1px（要 %2）/ 主动作 %3px（要 %4）/ 图标 %5px（要 22）")
                          .arg(tallPlain->height())
                          .arg(Theme::actionHeight())
                          .arg(tallPrimary->height())
                          .arg(Theme::primaryActionHeight())
                          .arg(tallIcon->height()));
                check(tallIcon->property("wtfmlButtonSize").toString() ==
                          Theme::iconButtonSizeValue(),
                      "★ `markIconButton()` 挂上去的属性名和样式表里那条复位规则用的是"
                      "**同一个**");
            }

            // 真按钮真渲染：中心是轻主题填充、四周是边那一档、字是字那一档。
            // 这一条是"样式表写出来了"和"样式表生效了"的分界 —— 只比字符串的话，
            // 选择器写错一个字母照样通过。
            QWidget buttonHost;
            buttonHost.resize(240, 80);
            auto *realButton = new QPushButton(QStringLiteral("启动游戏"), &buttonHost);
            realButton->setGeometry(20, 20, 160, 40);
            realButton->setFocusPolicy(Qt::NoFocus);
            buttonHost.show();
            QApplication::processEvents();
            // 按钮底是半透明的：垫一层页面底再抓，期望值 = `Button` 叠在这层底上。
            const QColor buttonBase = Theme::tokens().page;
            const QImage buttonShot = captureWidgetOn(realButton, buttonBase);
            const QColor buttonFill = dominantColor(buttonShot, realButton->rect());
            const QColor plainFill = overOpaque(app.palette().color(QPalette::Button), buttonBase);
            const QColor plainInk = app.palette().color(QPalette::ButtonText);
            int borderHits = 0;
            int inkHits = 0;
            for (int y = 0; y < buttonShot.height(); ++y) {
                for (int x = 0; x < buttonShot.width(); ++x) {
                    const QColor c = buttonShot.pixelColor(x, y);
                    if (colorDistance(c, edge) <= 6) {
                        ++borderHits;
                    }
                    if (colorDistance(c, plainInk) <= 30) {
                        ++inkHits;
                    }
                }
            }
            // 一般按钮（不挂属性那一档）现在用的是**调色板里那几支**：底 = `Button`
            // （tokens 的 subtle）、字 = `ButtonText`（tokens 的 text）、边是**透明的**
            // （只占 1px 保证几何不变，键盘焦点那一圈才换成主题色）。所以这里量的是
            // 调色板上那两个角色 —— 样式表里写的就是 `palette(button)` / `palette(button-text)`。
            check(sameColor(buttonFill, plainFill),
                  "★★ W12 一般按钮中心铺的就是调色板的 `Button` 那一支",
                  QStringLiteral("量到 %1 该是 %2").arg(buttonFill.name(), plainFill.name()));
            check(borderHits == 0 && inkHits > 0,
                  "★★ W12 那一圈边是**透明的**（焦点才换主题色）、那行字铺的就是正文那一支",
                  QStringLiteral("主题色边 %1 个像素（要 0）/ 字 %2 个").arg(borderHits).arg(inkHits));

            // 危险那一档：同一颗按钮挂上属性之后，字和边换成红的。
            Theme::markButton(realButton, Theme::ButtonKind::Danger);
            // 动态属性是**下一次 polish 时**才被选择器看见的，改完得手动让它重来一趟
            // （Qt 不会因为属性变了就自己重算样式）。
            realButton->style()->unpolish(realButton);
            realButton->style()->polish(realButton);
            QApplication::processEvents();
            const QImage dangerShot = captureWidgetOn(realButton, buttonBase);
            const QColor dangerInk = Theme::tokens().danger;
            int dangerHits = 0;
            int dangerInkHits = 0;
            for (int y = 0; y < dangerShot.height(); ++y) {
                for (int x = 0; x < dangerShot.width(); ++x) {
                    const QColor c = dangerShot.pixelColor(x, y);
                    if (colorDistance(c, dangerInk) <= 30) {
                        ++dangerHits;
                    }
                    if (colorDistance(c, ink) <= 6) {
                        ++dangerInkHits;
                    }
                }
            }
            const QColor dangerFill = overOpaque(Theme::tokens().dangerSoft, buttonBase);
            const QColor dangerDominant = dominantColor(dangerShot, realButton->rect());
            // 「红底」不拿一个固定色值判：这一档的底是半透明的 `dangerSoft` 叠在页面底上，
            // 深色主题下它本来就很接近页面底（0.14 的红罩在深底上只偏红一点点）。判据取
            // "比页面底更红、不是主题色那一支、那行字是红的"。
            check(dangerDominant.red() > dangerDominant.green() &&
                      colorDistance(dangerDominant, buttonBase) > 6 &&
                      !sameColor(dangerDominant, Theme::tokens().primary) &&
                      dangerHits > 0 && dangerInkHits == 0,
                  "★★ W12 挂上「危险」后：红底、红字（主题色那一支一个像素都不剩）",
                  QStringLiteral("底 %1（该是 %2）/ 红字 %3 个 / 主题色 %4 个")
                      .arg(dangerDominant.name(), dangerFill.name())
                      .arg(dangerHits).arg(dangerInkHits));

            // ---------- 按钮状态体系：Ghost 那一档 + 「灰掉」那一档 ----------
            //
            // 「统一按钮视觉层级 / 单独设计 Disabled 状态」。前三档是用户
            // 点名的那三句（确认性 = 主题色 / 一般性 = 黑 / 不可撤销 = 红），
            // 这一轮补的是第四档 `Ghost`（**低频辅助动作**）和「灰掉」那一档的收口。
            //
            // Ghost 和 `Neutral` 的分别**不在颜色，在形状**：它没底、没边，于是能贴在
            // 卡片角上不抢眼（"复制到剪贴板"那种一年按不了几次的动作）。这一段量三样：
            // ① 它是不是真的一档（属性值不和别档撞）；② 它的**几何和普通那档一模一样**
            // （最容易写坏的地方）；③ 屏幕上真的**透出底下的东西**。
            {
                const QString ghostKey = Theme::buttonKindKey(Theme::ButtonKind::Ghost);
                check(!ghostKey.isEmpty() &&
                          ghostKey != Theme::buttonKindKey(Theme::ButtonKind::Primary) &&
                          ghostKey != Theme::buttonKindKey(Theme::ButtonKind::Danger) &&
                          ghostKey != Theme::buttonKindKey(Theme::ButtonKind::Neutral) &&
                          ghostKey != Theme::buttonKindKey(Theme::ButtonKind::Accent),
                      "★ 「Ghost」（低频辅助动作）是**独立的一档**：属性和另外三档都不同",
                      ghostKey);

                // ① 几何：换一档 kind，**整颗按钮一寸都不动**。
                // 这一条钉的是这一档最容易写坏的那一处：把 Ghost 写成 `border: none`
                // （看着"更干净"）会让控件**矮 2px、窄 2px** —— 于是同一个布局里换一档
                // kind，行里所有东西都跟着挪一格。边是**留成透明的**，不是删掉。
                QWidget kindHost;
                auto *kindLayout = new QHBoxLayout(&kindHost);
                kindLayout->setContentsMargins(0, 0, 0, 0);
                kindLayout->setSpacing(0);
                const QString kindText = QStringLiteral("复制到剪贴板");
                auto *ghostButton = new QPushButton(kindText, &kindHost);
                Theme::markButton(ghostButton, Theme::ButtonKind::Ghost);
                auto *kindPeer = new QPushButton(kindText, &kindHost);
                Theme::markButton(kindPeer, Theme::ButtonKind::Neutral);
                kindLayout->addWidget(ghostButton);
                kindLayout->addWidget(kindPeer);
                kindHost.show();
                QApplication::processEvents();
                check(ghostButton->sizeHint() == kindPeer->sizeHint() &&
                          ghostButton->height() == kindPeer->height() &&
                          ghostButton->height() == Theme::actionHeight(),
                      "★★ Ghost 的**几何和普通那档一模一样**（同一行字、同一个高度和宽度）："
                      "差别只在画出来的底和边 —— 边是留成透明的，不是删掉",
                      QStringLiteral("Ghost %1×%2 / 普通 %3×%4（要 %5 高）")
                          .arg(ghostButton->sizeHint().width()).arg(ghostButton->height())
                          .arg(kindPeer->sizeHint().width()).arg(kindPeer->height())
                          .arg(Theme::actionHeight()));

                // ② 真渲染：把一颗 Ghost 和一颗普通按钮摆在一块**底色明确不是白**的板上，
                // 抓整块板。Ghost 那一格的中心跟**板子的底色是同一个色**（它就是"看不见
                // 那块底"），普通那一格的中心是白的 —— 两条一起量才排得掉"整块板都白"
                // 这种假绿（只量"Ghost 那儿不是白"，把按钮画成一个黑洞也照样过）。
                QWidget paintHost;
                paintHost.resize(240, 104);
                paintHost.setAutoFillBackground(true);
                QPalette paintPalette = paintHost.palette();
                paintPalette.setColor(QPalette::Window, QColor(0xd0, 0xd0, 0xd0));
                paintHost.setPalette(paintPalette);
                auto *paintGhost = new QPushButton(kindText, &paintHost);
                Theme::markButton(paintGhost, Theme::ButtonKind::Ghost);
                paintGhost->setFocusPolicy(Qt::NoFocus);
                paintGhost->setGeometry(20, 8, 160, Theme::actionHeight());
                auto *paintSolid = new QPushButton(kindText, &paintHost);
                Theme::markButton(paintSolid, Theme::ButtonKind::Neutral);
                paintSolid->setFocusPolicy(Qt::NoFocus);
                paintSolid->setGeometry(20, 58, 160, Theme::actionHeight());
                paintHost.show();
                QApplication::processEvents();
                const QImage paintShot = captureWidget(&paintHost);
                const QColor boardColor = QColor(paintShot.pixel(228, 4));
                const QColor ghostArea = dominantColor(paintShot, paintGhost->geometry());
                const QColor solidArea = dominantColor(paintShot, paintSolid->geometry());
                // 普通那一档铺的是调色板的 `Button`（这一套界面里"白纸"就是这一支，
                // 不是纯白 —— 见 W12 那一条，两处同一个判据）。
                // 7a 设计稿「文字按钮」：无底、强调色字；普通那一档是 `Button` 叠在板上。
                const QColor solidFill =
                    overOpaque(paintSolid->palette().color(QPalette::Button), boardColor);
                check(sameColor(ghostArea, boardColor) &&
                          !sameColor(boardColor, solidFill) && sameColor(solidArea, solidFill),
                      "文字按钮（Ghost）无底、透出底下那块板；普通按钮铺 `Button` 那一档",
                      QStringLiteral("板 %1 / Ghost %2 / 普通 %3（要 %4）")
                          .arg(boardColor.name(), ghostArea.name(), solidArea.name(),
                               solidFill.name()));
                // 那行字照旧铺上（只是没有底和边）—— "没底"不等于"没字"。
                // 墨色问**这一颗按钮自己的调色板**（`QPalette::Text`，Ghost 那一档 QSS 里
                // `color: palette(text)` 就是它）；不写死 `#202020` —— 换主题之后墨色
                // 是跟着变的，写死的那一支会把"字画上去了"读成"一个深像素都没有"。
                const QColor ghostInk = paintGhost->palette().color(QPalette::Link);
                int ghostInkHits = 0;
                const QRect ghostBox = paintGhost->geometry();
                for (int y = ghostBox.top(); y <= ghostBox.bottom(); ++y) {
                    for (int x = ghostBox.left(); x <= ghostBox.right(); ++x) {
                        if (colorDistance(QColor(paintShot.pixel(x, y)), ghostInk) <= 32) {
                            ++ghostInkHits;
                        }
                    }
                }
                check(ghostInkHits > 0,
                      "★ Ghost 只是没有底和边，**那行字照旧铺上**（就是普通那一档的墨）——"
                      "「没底」不等于「没字」",
                      QStringLiteral("那一格上 %1 个墨色像素（墨 %2）")
                          .arg(ghostInkHits)
                          .arg(ghostInk.name()));

                // ③ 灰掉那一档**还得读得出来**。它是最容易被做成"淡淡的"的那一档：用户要
                // 看出"这颗不能按"，但字的内容仍然要说出口（"启用"两个字灰到看不见，
                // 用户就不知道这颗按钮是干什么的）。判据取 WCAG 那 3:1（次要信息那一档），
                // 对的是**禁用时真正的底** `disabledFillColor()`。
                const double disabledContrast =
                    contrast(Theme::disabledInkColor(), Theme::disabledFillColor());
                check(disabledContrast >= 3.0,
                      "★★ 灰掉那一档的字对底 ≥ 3:1（灰归灰，「这颗按钮叫什么」还得读得出来）",
                      QStringLiteral("%1 对 %2 = %3:1")
                          .arg(Theme::disabledInkColor().name(), Theme::disabledFillColor().name())
                          .arg(disabledContrast, 0, 'f', 2));
                // 而它和「一般性（黑）」那一档**不是同一个色**：同色的话，一颗"不能按"的
                // 按钮和一颗"能按、只是没那么要紧"的按钮，屏幕上分不出来。
                check(colorDistance(Theme::disabledInkColor(), Theme::neutralInkColor()) > 100,
                      "★ 「灰掉」和「一般性」是**两档两色**：灰的是「这颗不能按」，"
                      "黑的是「这颗能按、只是没那么要紧」",
                      QStringLiteral("灰 %1 / 黑 %2")
                          .arg(Theme::disabledInkColor().name(), Theme::neutralInkColor().name()));
                kindHost.hide();
                paintHost.hide();
            }

            // ---------- 焦点环 ----------
            //
            // 「全部关键操作具备键盘焦点、**可见焦点环**和合理 Tab 顺序」。
            // 环是 `WtfmlProxyStyle` 在 `PE_FrameFocusRect` 上自己画的 —— 基样式（Fusion）
            // 画的是**一圈虚线**，我们的按钮挂了应用级样式表之后它上屏看不见，等于"按 Tab
            // 跳到哪儿了"说不出口。这里量的是**真的画出来了、而且是那一档色**：只比
            // `focusRingColor()` 的返回值是不够的（环根本没上屏照样过）。
            {
                QWidget ringHost;
                ringHost.resize(160, 72);
                auto *ringButton = new QPushButton(QStringLiteral("确定"), &ringHost);
                ringButton->setGeometry(24, 18, 112, 36);
                ringHost.show();
                // 用 Tab 进来的焦点（真按 Tab 键也是这一档）——`setFocusPolicy` 没动过，
                // 所以这一句同时也盯着"按钮默认可聚焦"这件事。
                ringButton->setFocus(Qt::TabFocusReason);
                ringHost.activateWindow();
                // 这一档现在只认**真的按键**（藏起焦点控件时 Qt 交接焦点也报
                // TabFocusReason ⇒ 不能拿那个理由当"用户按了 Tab"，见 Theme.cpp 那段）。
                // 所以照真路先送一发普通按键，再送 FocusIn。
                QKeyEvent keyboardInput(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
                QApplication::sendEvent(ringButton, &keyboardInput);
                QFocusEvent keyboardFocus(QEvent::FocusIn, Qt::TabFocusReason);
                QApplication::sendEvent(ringButton, &keyboardFocus);
                QApplication::processEvents();
                check(ringButton->hasFocus() &&
                          ringButton->property("wtfmlKeyboardFocus").toBool(),
                      "★★ 键盘焦点进来了 ⇒ `wtfmlKeyboardFocus` 属性摆上"
                      "（环画不画就看它这一格）",
                      QStringLiteral("焦点 %1 / 属性 %2")
                          .arg(ringButton->hasFocus() ? QStringLiteral("有")
                                                      : QStringLiteral("无"),
                               ringButton->property("wtfmlKeyboardFocus").toBool()
                                   ? QStringLiteral("开")
                                   : QStringLiteral("关")));

                const QImage ringShot = captureWidget(ringButton);
                const QColor ringInk = Theme::focusRingColor(ringButton->palette());
                int ringHits = 0;
                for (int y = 0; y < ringShot.height(); ++y) {
                    for (int x = 0; x < ringShot.width(); ++x) {
                        if (colorDistance(ringShot.pixelColor(x, y), ringInk) <= 40) {
                            ++ringHits;
                        }
                    }
                }
                check(ringButton->hasFocus() && ringInk.isValid() && ringHits > 0,
                      "★★ 拿着键盘焦点的控件四周真画了一圈主题色（基样式那圈虚线上屏之后"
                      "看不见，等于没有）",
                      QStringLiteral("环 %1 个像素 / 要的是 %2").arg(ringHits).arg(ringInk.name()));

                // 环色取**当前预设那一支**（`tokens().focus`）：每个预设的 focus 已经按
                // 自己的底色挑好了（深色预设那一支就是浅的），不必运行时再算一次。
                const QColor lightRing = Theme::focusRingColor(ringButton->palette());
                check(sameColor(lightRing, Theme::tokens().focus),
                      "★★ 焦点环用的是当前预设的 `tokens().focus` 那一支",
                      QStringLiteral("环 %1 / 令牌 %2")
                          .arg(lightRing.name(), Theme::tokens().focus.name()));

                // 深浅两档**只有 `system` 需要**：同一份 tokens 要铺在两种底上，
                // "深底上用浅的那一支"才成立。固定预设自带的那一支是给自家底色的，
                // 不分叉（见 `focusRingColor()`）。
                const Theme::Spec specBeforeRings = Theme::current();
                Theme::setSpec(Theme::presetSpec(QStringLiteral("system")));
                Theme::apply(app);
                QApplication::processEvents();
                QPalette darkPalette = ringButton->palette();
                darkPalette.setColor(QPalette::Window, QColor(0x1e, 0x1e, 0x1e));
                const QColor systemLightRing = Theme::focusRingColor(ringButton->palette());
                const QColor systemDarkRing = Theme::focusRingColor(darkPalette);
                check(!sameColor(systemLightRing, systemDarkRing) && systemLightRing.isValid() &&
                          systemDarkRing.isValid(),
                      "★★ `system` 预设下焦点环深浅两档是**两支**（浅底用深的那一档、深底用浅的"
                      "那一档）",
                      QStringLiteral("浅底 %1 / 深底 %2")
                          .arg(systemLightRing.name(), systemDarkRing.name()));
                Theme::setSpec(specBeforeRings);
                Theme::apply(app);
                QApplication::processEvents();

                {
                    // ① 鼠标按下先到（真点击就是这一路）：键盘那一档收掉。
                    const QPoint center = ringButton->rect().center();
                    QMouseEvent mousePress(QEvent::MouseButtonPress, QPointF(center),
                                           QPointF(ringButton->mapToGlobal(center)),
                                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(ringButton, &mousePress);
                    check(!ringButton->property("wtfmlKeyboardFocus").toBool(),
                          "★ 鼠标按下就把键盘那一档收掉（上面那一圈环是给键盘的）");

                    // ② 切页交接过来的焦点：理由写着 TabFocusReason，前面**没有任何按键**。
                    QFocusEvent handoff(QEvent::FocusIn, Qt::TabFocusReason);
                    QApplication::sendEvent(ringButton, &handoff);
                    check(!ringButton->property("wtfmlKeyboardFocus").toBool(),
                          "★★ 切页时 Qt 交接焦点（`TabFocusReason`）**不算键盘操作** —— "
                          "新页面上那颗页面级按钮不会凭空亮一圈绿框（用户报的那一幕）");

                    // ③ 反方向：真按键之后那一发照旧点亮（别把键盘用户一起挡了）。
                    QKeyEvent realKey(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
                    QApplication::sendEvent(ringButton, &realKey);
                    QApplication::sendEvent(ringButton, &handoff);
                    check(ringButton->property("wtfmlKeyboardFocus").toBool(),
                          "★★ 真按键之后焦点进来照旧点亮（键盘那一档没被一起挡掉）");
                }
            }

            // ---------- W12 灰掉那一档：**中性 / 危险那两种也得跟着灰** ----------
            //
            // 症状（真机像素实测）：一颗**禁用着**的中性按钮，底确实是 `#F7F7F7`（说明
            // disabled 那一档生效了），**字和边却还是 `#202020`** —— 屏幕上看着就是一颗
            // 按得动的按钮。主题色那一颗（Accent，不挂 kind 属性、不参与下面这场平手）
            // 灰得是对的，所以只有这两种"看着没灰"。
            //
            // 根子：样式表里 `QPushButton:disabled` 和
            // `QPushButton[wtfmlButtonKind="neutral"]` **优先级一模一样**（都是"元素 +
            // 一个属性/伪类"），平手时**后写的赢** —— 所以 `:disabled` 必须写在两种 kind
            // 后面（见 `Theme::widgetStyleSheet()` 里那段）。
            //
            // 量法：**数深像素**。禁用那一档的那几个色（底 `disabledFillColor()` /
            // 字 `disabledInkColor()` / 边 `disabledBorderColor()`）一个都不比 `#808080`
            // 深；带病那一版字是 `#202020`，一数就是一大片。**量的是渲染出来的像素**，
            // 不是样式表字符串 —— 字符串那一条会跟着同一个错误一起写错（那正是这个 bug
            // 当初能过掉的原因）。
            const auto disabledShot = [](QPushButton *button) {
                button->setEnabled(false);
                button->style()->unpolish(button);
                button->style()->polish(button);
                QApplication::processEvents();
                const QImage shot = captureWidgetOn(button, Theme::tokens().page);
                button->setEnabled(true);
                button->style()->unpolish(button);
                button->style()->polish(button);
                return shot;
            };
            const auto countPixelsIn = [](const QImage &image, const QColor &want, int tolerance) {
                int hits = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        if (colorDistance(image.pixelColor(x, y), want) <= tolerance) {
                            ++hits;
                        }
                    }
                }
                return hits;
            };
            // 此刻挂着「危险」⟶ 先量它，再换成「中性」量第二趟。
            const QImage dangerDisabledShot = disabledShot(realButton);
            Theme::markButton(realButton, Theme::ButtonKind::Neutral);
            realButton->style()->unpolish(realButton);
            realButton->style()->polish(realButton);
            const QImage neutralDisabledShot = disabledShot(realButton);
            // 两样都要为 0：
            // · **那一档自己的墨** —— 带病那版中性按钮的字和边还是正文色，危险按钮的
            //   字和边还是红的；量法改成"和那一档的墨有多近"，不再数"深像素"（深色主题
            //   下整块底本来就是深的，数深像素等于把底也数进去）；
            // · 同时灰字得**还在**（不然"把字删掉"也能让上面那条变 0）。
            const int neutralDisabledKindInk =
                countPixelsIn(neutralDisabledShot, Theme::neutralInkColor(), 30);
            const int dangerDisabledKindInk = countPixelsIn(dangerDisabledShot, dangerInk, 30);
            const int neutralDisabledInk =
                countPixelsIn(neutralDisabledShot, Theme::disabledInkColor(), 100);
            const int dangerDisabledInk =
                countPixelsIn(dangerDisabledShot, Theme::disabledInkColor(), 100);
            check(neutralDisabledKindInk == 0 && dangerDisabledKindInk == 0 &&
                      neutralDisabledInk > 0 && dangerDisabledInk > 0,
                  "★★ W12 灰掉那一档把**中性 / 危险**那两种也按下去（字和边一起换成 "
                  "`disabledInkColor()` / `disabledBorderColor()`：两档自己的墨一个都不剩、"
                  "灰字还在）—— 平手时"
                  "后写的赢，所以那条 `:disabled` 必须摆在两种 kind 之后",
                  QStringLiteral("中性档自己的墨 %1 个 / 危险档红 %2 个 / 灰字 %3+%4 个")
                      .arg(neutralDisabledKindInk)
                      .arg(dangerDisabledKindInk)
                      .arg(neutralDisabledInk)
                      .arg(dangerDisabledInk));

            // 灰掉那一档对 **Ghost** 一视同仁：它是一档**四档共用**的
            // 状态，不是"主题色那一颗才有"。容易漏掉的原因很实在 —— Ghost 平时没有底，
            // 灰掉的时候那条 `:disabled` 才第一次给它铺上底，用同一把尺子量一遍。
            // NOTE: "勾着又灰掉"的 Ghost 是**淡底 + 灰字**（`[ghost]:checked` 那条的优先级
            // 比 `QPushButton:disabled` 高一位，只吃底不吃字）：这是故意的 ——
            // 一颗灰掉的开关也得说得出自己是开是关。字色归 `:disabled` 管。
            {
                QWidget ghostDisabledHost;
                auto *ghostDisabledButton =
                    new QPushButton(QStringLiteral("查看报告"), &ghostDisabledHost);
                Theme::markButton(ghostDisabledButton, Theme::ButtonKind::Ghost);
                ghostDisabledButton->setFocusPolicy(Qt::NoFocus);
                auto *ghostDisabledRow = new QVBoxLayout(&ghostDisabledHost);
                ghostDisabledRow->setContentsMargins(0, 0, 0, 0);
                ghostDisabledRow->addWidget(ghostDisabledButton);
                ghostDisabledHost.show();
                QApplication::processEvents();
                const QImage ghostDisabledShot = disabledShot(ghostDisabledButton);
                const int ghostDisabledGrey =
                    countPixelsIn(ghostDisabledShot, Theme::disabledInkColor(), 100);
                // Ghost 平时那行字是调色板的 `Link`（强调色写出来的那一支）。灰掉之后
                // 它不该还在 —— 判据取"还有没有那一支的像素"，不数深像素（深色主题下
                // 整块底都是深的）。
                const QColor ghostInk = ghostDisabledButton->palette().color(QPalette::Link);
                const int ghostDisabledKindInk =
                    countPixelsIn(ghostDisabledShot, ghostInk, 30);
                check(ghostDisabledGrey > 0 && ghostDisabledKindInk == 0,
                      "★★ Ghost 灰掉之后那行字也跟着灰（`disabledInkColor()` 那一档，"
                      "平时那支强调字一个都不剩）—— 灰掉是四档共用的一件事",
                      QStringLiteral("灰 %1 个 / 强调字 %2 个（%3）")
                          .arg(ghostDisabledGrey).arg(ghostDisabledKindInk).arg(ghostInk.name()));
                ghostDisabledHost.hide();
            }
            buttonHost.hide();
            Theme::setSpec(Theme::defaultSpec());
            Theme::apply(app);

            Theme::setSpec(Theme::defaultSpec());
            Theme::apply(app);

            // ---------- W13 按钮分三档 / 输入框 + 勾选框那一套 ----------
            // 要求：「对于按钮，一些确认性的（如启动，下载，搜索）做成主题色，一些一般性的
            // （设置，清空搜索）做成黑色，一些不可撤销（如修改存档，删除实例）做成红色
            // …… 输入文本的搜索框的框框跟随主题色，文本保留黑色，勾选框未勾选的时候框和文本
            // 做成黑色，勾选了两者都变成主题色」。
            //
            // 这一节量的是"**屏幕上的样子**"，不是"样式表字符串里有那几个词"：
            // ① 「一般性」那一档的黑**不是纯黑**（理由和数在下面那条断言里）；
            // ② 三档底色/字色**两两看得出不一样**（不然"分了三档"是句空话）；
            // ③ 一颗真 QLineEdit：中心白、字黑、那一圈边 = 主题色的「边」那一档；
            // ④ 一颗真 QCheckBox：没勾时框和字都是那一档黑，勾上之后**两样都**变成主题色
            // （这一条最容易写成"只变了一样"，用户那句原话是"两者都"）。
            const QColor neutralInk = Theme::neutralInkColor();
            const QColor accentPure2 = Theme::accentShade(Theme::AccentShade::Pure);
            // 勾上那一格的**字**走的是调色板 `Link`（各预设都把它备成"压在自家底色上
            // 读得清的主题色"），方框那一块走 `Accent`（纯色）—— 两样不是同一个地方取的，
            // 所以分别量。
            // 7b：调色板 `Link` 改成 accentInk（浅色预设下压到近黑、专给小字用），拿它当「绿」
            // 会和那一档黑撞在 30 的容差里 —— 「有没有主题色」改用 tokens().deep 量。
            const QColor checkInk = Theme::tokens().deep;
            // 输入框 / 下拉框**默认不描线**（「去硬描边」）；拿调色板 `Dark`（tokens 的 stroke）
            // 那一档当「旧底线」的色，量它**不在**最后一行上。
            const QColor inputBorderToken = app.palette().color(QPalette::Dark);
            // 7a：输入框 / 下拉框的底和那条底线都是半透明的，垫在页面底上抓、按叠出来的色量。
            const QColor formBase = Theme::tokens().page;
            const QColor inputFill = overOpaque(app.palette().color(QPalette::Base), formBase);
            const QColor inputBorder = overOpaque(inputBorderToken, inputFill);

            // 「一般性」那一档的墨就是正文那一支：不是纯黑、也不是纯白（两侧都太重），
            // 但压在**这一档自己的底**上够 WCAG 正文那条 4.5:1。
            check(!sameColor(neutralInk, QColor(Qt::black)) &&
                      !sameColor(neutralInk, QColor(Qt::white)) &&
                      contrast(neutralInk, Theme::tokens().page) >= 4.5,
                  "★★ W13 「一般性」那一档用的是正文墨（不是纯黑也不是纯白，一颗「取消」"
                  "不该比旁边的「确定」抢眼），压在页面底上够 4.5:1",
                  QStringLiteral("%1 对页面底 %2 是 %3:1")
                      .arg(neutralInk.name(), Theme::tokens().page.name())
                      .arg(contrast(neutralInk, Theme::tokens().page), 0, 'f', 2));
            check(colorDistance(accentPure2, neutralInk) > 60 &&
                      colorDistance(accentPure2, QColor(0xc0, 0x28, 0x28)) > 60 &&
                      colorDistance(neutralInk, QColor(0xc0, 0x28, 0x28)) > 60,
                  "★★ W13 三档真的看得出不一样（主题色 / 一般性那一档黑 / 不可撤销那一档红）",
                  QStringLiteral("主题色 %1 / 黑 %2 / 红 #c02828")
                      .arg(accentPure2.name(), neutralInk.name()));

            // 输入框 / 勾选框 / 下拉框都长在一层白底上，量的是"这一块里某个色出现了多少像素"。
            QWidget formHost;
            formHost.resize(320, 170);
            auto *searchEdit = new QLineEdit(QStringLiteral("搜索"), &formHost);
            searchEdit->setGeometry(12, 12, 200, 28);
            // 焦点策略钉成 NoFocus：`QLineEdit:focus` 那一档量到的边是**字**那一档（更深），
            // 不钉的话这一条量到哪一档取决于"这一刻谁拿着焦点"，是一条看运气的断言。
            searchEdit->setFocusPolicy(Qt::NoFocus);
            auto *uncheckedBox = new QCheckBox(QStringLiteral("启用"), &formHost);
            uncheckedBox->setGeometry(12, 52, 160, 24);
            uncheckedBox->setFocusPolicy(Qt::NoFocus);
            auto *checkedBox = new QCheckBox(QStringLiteral("启用"), &formHost);
            checkedBox->setGeometry(12, 84, 160, 24);
            checkedBox->setFocusPolicy(Qt::NoFocus);
            checkedBox->setChecked(true);
            // 加的下拉框（要求「所有单选框换成下拉的」带来的那一套样式）：
            // 两项，量和上面那颗 QLineEdit **用同一把尺子**（"和输入框一样"这句话只有
            // 同样量法才说得清）。焦点策略同样钉成 NoFocus，理由和那颗输入框一字不差
            // （`:focus` 那一档的边更深，不钉就看运气）。
            auto *plainCombo = new QComboBox(&formHost);
            plainCombo->setGeometry(12, 120, 220, 30);
            plainCombo->addItem(QStringLiteral("跟随全局"));
            plainCombo->addItem(QStringLiteral("自定义"));
            plainCombo->setFocusPolicy(Qt::NoFocus);
            formHost.show();
            QApplication::processEvents();

            // 数一个色在某一块里有多少个像素（三个控件共用这一套）。
            const auto countColorIn = [](const QImage &image, const QRect &area,
                                         const QColor &want, int tolerance) {
                int hits = 0;
                for (int y = area.top(); y <= area.bottom(); ++y) {
                    for (int x = area.left(); x <= area.right(); ++x) {
                        if (colorDistance(QColor(image.pixel(x, y)), want) <= tolerance) {
                            ++hits;
                        }
                    }
                }
                return hits;
            };

            const QImage editShot = captureWidgetOn(searchEdit, formBase);
            const QColor editText = app.palette().color(QPalette::Text);
            check(colorDistance(dominantColor(editShot, searchEdit->rect()), inputFill) <= 3 &&
                      // 文楷笔画细，抗锯齿后没有一个像素正好是字色 ⇒ 放一点容差（三通道差之和 12）。
                      countColorIn(editShot, searchEdit->rect(), editText, 12) > 0,
                  "★★ W13 输入框：框里面是 `Base` 那一档（叠在页面底上）、字是调色板 `Text` 那一支（不是纯黑）",
                  QStringLiteral("底色 %1 该是 %2 / 字 %3")
                      .arg(dominantColor(editShot, searchEdit->rect()).name(), inputFill.name(),
                           editText.name()));
            // 最后一行上「旧底线」那一档的像素数（四边 1px 都是透明边，只为和数字框一样高）。
            const auto bottomLineHits = [&countColorIn, &inputBorder](const QImage &image, const QRect &area) {
                return countColorIn(image, QRect(area.left(), area.bottom(), area.width(), 1), inputBorder, 12);
            };
            // 最后一行是圆角那圈的抗锯齿半格（底和页面之间），不该比页面离底更远。
            const auto bottomIsFill = [&inputFill, &formBase](const QImage &image, const QRect &area) {
                return colorDistance(QColor(image.pixel(area.center().x(), area.bottom())), inputFill) <=
                       colorDistance(formBase, inputFill) + 3;
            };
            const int editBorderHits = bottomLineHits(editShot, searchEdit->rect());
            check(editBorderHits <= searchEdit->width() / 10 && bottomIsFill(editShot, searchEdit->rect()),
                  "★★ W13 输入框默认**没有底线**：最后一行和框里一样是 `Base` 底（「去硬描边」）",
                  QStringLiteral("最后一行有 %1 个 `Dark` 像素 / 正中 %2 该是 %3")
                      .arg(editBorderHits)
                      .arg(QColor(editShot.pixel(searchEdit->rect().center().x(), searchEdit->rect().bottom())).name(),
                           inputFill.name()));
            {   // 报错：那**一圈**换成危险色（12 号反馈之后没有底部那条线了，报错也走整圈）。
                const auto repolish = [searchEdit] {
                    searchEdit->style()->unpolish(searchEdit);
                    searchEdit->style()->polish(searchEdit);
                };
                searchEdit->setProperty("wtfmlError", true);
                repolish();
                const QImage errorShot = captureWidgetOn(searchEdit, formBase);
                const QColor danger = app.palette().color(QPalette::BrightText);
                const QRect topBand(searchEdit->width() / 5, 0, searchEdit->width() * 3 / 5, 3);
                const QColor errorRing = overOpaque(danger, inputFill);
                const int errorHits = countColorIn(errorShot, topBand, errorRing, 24);
                check(errorHits >= topBand.width() * 3 * 6 / 10,
                      "★★ W13 输入框报错时**顶边那 3px 换成危险色**（整圈淡环，不是底部一条线）",
                      QStringLiteral("危险色 %1 个像素 / 那一带 %2 个（该是 %3）")
                          .arg(errorHits).arg(topBand.width() * 3).arg(errorRing.name()));
                searchEdit->setProperty("wtfmlError", QVariant());
                repolish();
            }
            // 焦点（F1）：鼠标点进来只把底提亮一档、**不画线**；键盘 Tab / Backtab 到达才加 3px 淡环。
            // 这一条量两件：样式表里旧底线规则一条都不许留；环真的画出来了（而且只在键盘那一档）。
            {
                const QString sheetNow = Theme::widgetStyleSheet();
                check(!sheetNow.contains(QLatin1String("border-bottom")),
                      "★★ W13 焦点态 QSS 里**不再出现底部边线规则**（`border-bottom` 一条都没有）",
                      QStringLiteral("样式表里 %1 处 `border-bottom`")
                          .arg(sheetNow.count(QLatin1String("border-bottom"))));
                check(sheetNow.contains(QLatin1String("QLineEdit[wtfmlKeyboardFocus=\"true\"]:focus")) &&
                          sheetNow.contains(QLatin1String("border-color: palette(highlight);")),
                      "★★ W13 键盘焦点那条 3px 淡环规则在：带 `wtfmlKeyboardFocus` 的输入框聚焦时换成 "
                      "`palette(highlight)`（强调色淡底那一档，三套预设都跟着走）");

                QWidget ringHost;
                auto *ringEdit = new QLineEdit(QStringLiteral("1280"), &ringHost);
                auto *ringRow = new QHBoxLayout(&ringHost);
                ringRow->setContentsMargins(8, 8, 8, 8);
                ringRow->addWidget(ringEdit);
                ringHost.show();
                ringHost.activateWindow();
                ringEdit->setFocus();
                QApplication::processEvents();
                const auto repolish = [ringEdit] {
                    ringEdit->style()->unpolish(ringEdit);
                    ringEdit->style()->polish(ringEdit);
                };
                const QRect ringBand(ringEdit->width() / 5, 0, ringEdit->width() * 3 / 5, 3);
                const QColor ring = overOpaque(app.palette().color(QPalette::Highlight), inputFill);
                // ① 鼠标焦点：只有底提亮一档，顶边那一带还是 `Base` 底（一个环像素都不许有）。
                ringEdit->setProperty("wtfmlKeyboardFocus", false);
                repolish();
                const QImage mouseShot = captureWidgetOn(ringEdit, formBase);
                const int mouseRing = countColorIn(mouseShot, ringBand, ring, 24);
                check(mouseRing == 0,
                      "★★ W13 鼠标焦点**不画线**：顶边那一带还是 `Base` 底（一个环像素都没有）",
                      QStringLiteral("环像素 %1 个").arg(mouseRing));
                // ② 键盘焦点：同一格挂上属性就是 3px 淡环。
                ringEdit->setProperty("wtfmlKeyboardFocus", true);
                repolish();
                const QImage keyboardShot = captureWidgetOn(ringEdit, formBase);
                const int keyboardRing = countColorIn(keyboardShot, ringBand, ring, 24);
                check(keyboardRing >= ringBand.width() * 3 * 6 / 10,
                      "★★ W13 键盘焦点加那一圈 3px 淡环（顶边那一带是强调淡色，不是页底色）",
                      QStringLiteral("环像素 %1 个 / 那一带 %2 个（该是 %3）")
                          .arg(keyboardRing).arg(ringBand.width() * 3).arg(ring.name()));
                ringEdit->setProperty("wtfmlKeyboardFocus", QVariant());
                ringHost.hide();
            }

            // 勾选框没勾：框和字**都是**那一档黑；勾上：**两样都**变成主题色。
            // 「两样都」是这一条的重点 —— 只写 `QCheckBox::indicator` 的话字还是黑的。
            const QImage uncheckedShot = captureWidgetOn(uncheckedBox, formBase);
            const int uncheckedBlack = countColorIn(uncheckedShot, uncheckedBox->rect(),
                                                     neutralInk, 30);
            const int uncheckedGreen = countColorIn(uncheckedShot, uncheckedBox->rect(),
                                                     checkInk, 30);
            check(uncheckedBlack > 0 && uncheckedGreen == 0,
                  "★★ W13 勾选框没勾：那一圈框和那行字**都是**「一般性」那一档黑（一个绿像素都"
                  "没有）",
                  QStringLiteral("黑 %1 个 / 绿 %2 个").arg(uncheckedBlack).arg(uncheckedGreen));

            const QImage checkedShot = captureWidgetOn(checkedBox, formBase);
            const int checkedBlack = countColorIn(checkedShot, checkedBox->rect(), neutralInk, 30);
            const int checkedGreen = countColorIn(checkedShot, checkedBox->rect(), checkInk, 30);
            const int checkedPure = countColorIn(checkedShot, checkedBox->rect(), accentPure2, 6);
            // 7a 组件样张：勾上 = 主题色实心方框 + 白勾，**字仍是正文色**（不再跟着变绿）。
            const int checkedTextInk =
                countColorIn(checkedShot, checkedBox->rect(), Theme::tokens().text, 30);
            check(checkedPure > 0 && checkedTextInk > 0,
                  "W13 勾选框勾上：方框是主题色（白勾），字仍是正文色（7a 样张）",
                  QStringLiteral("主题色方框 %1 个 / 正文色字 %2 个 / 近主题色 %3 个")
                      .arg(checkedPure).arg(checkedTextInk).arg(checkedGreen));
            Q_UNUSED(checkedBlack);

            // 下拉框：**照抄上面那颗输入框的断言形态**，同一把尺子（同样的容差、同样的
            // 阈值）—— 用户那句要求是"下拉框和输入框那一套同一组颜色"，两种控件各量一套
            // 数字的话，"一样"这个词就说不清了。
            const QImage comboShot = captureWidgetOn(plainCombo, formBase);
            check(colorDistance(dominantColor(comboShot, plainCombo->rect()), inputFill) <= 3 &&
                      countColorIn(comboShot, plainCombo->rect(), editText, 12) > 0,
                  "★★ W13 下拉框：框里面是 `Base` 那一档、字是调色板 `Text` 那一支（和输入框同一组颜色）",
                  QStringLiteral("底色 %1")
                      .arg(dominantColor(comboShot, plainCombo->rect()).name()));
            const int comboBorderHits = bottomLineHits(comboShot, plainCombo->rect());
            check(comboBorderHits <= plainCombo->width() / 10 && bottomIsFill(comboShot, plainCombo->rect()),
                  "★★ W13 下拉框默认也没有底线（判据和阈值跟输入框一模一样）",
                  QStringLiteral("最后一行 %1 个 `Dark` 像素（输入框 %2 个 / 宽度 %3）")
                      .arg(comboBorderHits)
                      .arg(editBorderHits)
                      .arg(plainCombo->width()));
            formHost.hide();

            // 数字框 / 下拉框里面**也**有一颗 QLineEdit（`QAbstractSpinBox` 的编辑区、
            // `QComboBox` 可编辑时那一格）。上面 `QLineEdit {...}` 那条不加限定就是通配，
            // 会给它们**套上第二圈边**（外面已经有自己的框了）—— 这一条钉的就是那句复位
            // 规则还在。为什么量字符串不量像素：那一圈边只有 1px，又压在同样的白底上，
            // "多了一圈"在像素上和一个正常的框分不开；选择器在不在是能直接读的。
            const QString sheet = Theme::widgetStyleSheet();
            check(sheet.contains(QLatin1String("QFrame[wtfmlSurface=\"workspace\"]")) &&
                      sheet.contains(QLatin1String("QFrame[wtfmlSurface=\"footer\"]")) &&
                      !sheet.contains(QLatin1String("border-top: 1px")),
                  "★★ 页面工作区和页脚不再画装饰性分隔线（层级只靠底色与留白）");
            {
                // 真机回归：去掉 workspace 的边框后，格式串跳过了 `%4`，
                // 但参数表仍把 QPalette::Mid 塞在第 4 位。QString::arg() 会按“下一个现存
                // 占位符”顺延，于是 Mid（Windows 浅色下恰好约 #a0a0a0）误填给 section，
                // 设置页和下载安装页的卡片整块变成深灰。这里不只搜选择器，而是真画一张
                // section，盯住它不能再退回分隔线色。
                QFrame sectionProbe;
                sectionProbe.setProperty("wtfmlSurface", Theme::sectionSurfaceValue());
                sectionProbe.setFixedSize(80, 48);
                sectionProbe.show();
                QApplication::processEvents();
                const QImage sectionShot = captureWidget(&sectionProbe);
                const QColor sectionCenter(sectionShot.pixel(sectionShot.width() / 2,
                                                              sectionShot.height() / 2));
                const QColor mid = QApplication::palette().color(QPalette::Mid);
                check(!sameColor(sectionCenter, mid),
                      "★★ 小节卡片没有被 QPalette::Mid（Windows 浅色下的 #a0a0a0）误填",
                      QStringLiteral("卡片 %1 / Mid %2").arg(sectionCenter.name(), mid.name()));
            }
            check(sheet.contains(QLatin1String("QPushButton { background-color:")) &&
                      sheet.contains(QLatin1String("border: 1px solid transparent")) &&
                      sheet.contains(QLatin1String("QPushButton[wtfmlKeyboardFocus=\"true\"]:focus { border-color:")),
                  "★★ 普通按钮默认没有深色框，透明边保住几何，只有键盘焦点显示主题色边界");
            check(sheet.contains(QLatin1String("QAbstractSpinBox QLineEdit")) &&
                      sheet.contains(QLatin1String("QComboBox QLineEdit")) &&
                      sheet.contains(QLatin1String("border: none")),
                  "★★ W13 数字框/下拉框里那颗输入框被**复位**掉了（不给它们套第二圈边）");

            // ---------- 按钮状态体系：四档 × 状态，一条条点名 ----------
            //
            // 上面那一段量的是**像素**（真按钮、真渲染），这一段量的是**表**：四档层级各自
            // 把状态写全了没有。两样都要 —— 像素那条钉的是"照这个样子画出来了"，这一条钉的
            // 是"四档是**一整套**"：漏写一个状态，屏幕上就是"按下去什么都不动"，而那种毛病
            // 单看任何一颗按钮都是"看起来正常"。
            {
                const struct {
                    Theme::ButtonKind kind;
                    const char *key;
                } tiers[] = {
                    {Theme::ButtonKind::Primary, "primary"},
                    {Theme::ButtonKind::Danger, "danger"},
                    {Theme::ButtonKind::Neutral, "neutral"},
                    {Theme::ButtonKind::Ghost, "ghost"},
                };
                QString missingStates;
                for (const auto &tier : tiers) {
                    const QString selector =
                        QStringLiteral("QPushButton[wtfmlButtonKind=\"%1\"]")
                            .arg(QLatin1String(tier.key));
                    // 悬停 / 按下那两条**都带 `:enabled`**：不带的话，一颗**禁用着**的按钮
                    // 鼠标划过去底色照样翻上来（`:hover` 是"鼠标在不在上面"，不问能不能按）。
                    if (!sheet.contains(selector + QStringLiteral(":hover:enabled")) ||
                        !sheet.contains(selector + QStringLiteral(":pressed:enabled"))) {
                        missingStates += QLatin1String(tier.key) + QLatin1Char(' ');
                    }
                }
                check(missingStates.isEmpty(),
                      "★★ 四档层级（主动作 / 危险 / 一般性 / Ghost）各自把**悬停 + 按下**"
                      "两个状态都写全了（而且都带 `:enabled` —— 不带的话灰掉的按钮悬停时底色"
                      "会翻上来）",
                      missingStates.isEmpty() ? QStringLiteral("四档齐全")
                                              : QStringLiteral("缺状态的是：%1").arg(missingStates));

                // 那一档的写法：底和边**都是透明的**，而边那 1px **还在**。
                // 写 `border: none` 看着"更干净"，可它会让控件矮 2px、窄 2px —— 于是同一个
                // 布局里换一档 kind，行里所有东西都跟着挪一格（上面那段量的就是它）。
                const QString ghostRule =
                    QStringLiteral("QPushButton[wtfmlButtonKind=\"ghost\"] {");
                const int ghostAt = sheet.indexOf(ghostRule);
                const QString ghostBody =
                    ghostAt < 0 ? QString() : sheet.mid(ghostAt, ghostRule.size() + 90);
                check(ghostAt >= 0 &&
                          ghostBody.contains(QLatin1String("background-color: transparent")) &&
                          ghostBody.contains(QLatin1String("border-color: transparent")) &&
                          !ghostBody.contains(QLatin1String("border: none")),
                      "★★ Ghost 那一档的写法：底和边**都是透明的**，边那 1px **还在**"
                      "（写成 `border: none` 就矮 2px 窄 2px，换一档整行挪位）");
                // 勾上那一档也要有：一颗 checkable 的 Ghost（"只看选中的"那种小开关）得让人
                // 看出它现在是亮着的。
                check(sheet.contains(ghostRule.left(ghostRule.size() - 2) +
                                     QStringLiteral(":checked")),
                      "★ Ghost 多一条**勾上**那一档（否则一颗 checkable 的 Ghost 勾上之后"
                      "屏幕上什么都不变）");

                // 那条共用的 `QPushButton:disabled` 必须摆在**四档之后**（坑在这里）：它和 `[wtfmlButtonKind="..."]` 优先级一样，平手时**后写的赢** ——
                // 摆在前面的话，灰掉的按钮底灰了、字和边却还是原来那一档。
                // 上面那几条像素断言量的是"中性 / 危险真的灰了"，这一条量的是**位置**：
                // 后面新加一档（Ghost 就是新加的）时，这条规矩得跟着守住。
                const int disabledAt = sheet.indexOf(QLatin1String("QPushButton:disabled"));
                check(disabledAt > ghostAt && ghostAt >= 0,
                      "★★ 那条共用的 `QPushButton:disabled` 摆在**四档之后**（Ghost 也在它"
                      "前面）—— 平手时后写的赢，摆在前面的话新加的那一档灰不干净",
                      QStringLiteral("`:disabled` 在第 %1 个字符 / Ghost 那一档在第 %2 个")
                          .arg(disabledAt).arg(ghostAt));
            }

            check(sheet.contains(QLatin1String("QComboBox {")) &&
                      sheet.contains(QLatin1String("QComboBox:focus")) &&
                      sheet.contains(QLatin1String("QComboBox::drop-down")) &&
                      sheet.contains(QLatin1String("QComboBox QAbstractItemView")),
                  "★★ W13 下拉框那一套样式是**完整的一份**（本体 / 聚焦那一档 / 那颗箭头 / "
                  "弹出来的那张表，四条都在）",
                  QStringLiteral("本体 %1 / 聚焦 %2 / 箭头 %3 / 那张表 %4")
                      .arg(sheet.contains(QLatin1String("QComboBox {")) ? 1 : 0)
                      .arg(sheet.contains(QLatin1String("QComboBox:focus")) ? 1 : 0)
                      .arg(sheet.contains(QLatin1String("QComboBox::drop-down")) ? 1 : 0)
                      .arg(sheet.contains(QLatin1String("QComboBox QAbstractItemView")) ? 1 : 0));

            // ---------- 「小箭头一个都不许有」 ----------
            //
            // 8 号反馈：「这种右边的小箭头不要再出现」。旧规格（`::drop-down` 宽 20 +
            // `ComboChevronPainter` 在下拉框画完之后补一个 ∨）整条撤掉：那一格宽度 0、
            // 什么都不画（整块仍可点）；数字框的上下钮、QToolButton 的菜单三角同样归零。
            const QString dropDownRule =
                QStringLiteral("QComboBox::drop-down { width: 0px; border: none; }");
            const QString spinArrowRule = QStringLiteral(
                "QAbstractSpinBox::up-button, QAbstractSpinBox::down-button"
                " { width: 0px; height: 0px; border: none; background: transparent; }");
            const QString menuArrowRule =
                QStringLiteral("QToolButton::menu-indicator { width: 0px; height: 0px; image: none; }");
            check(sheet.contains(dropDownRule) && sheet.contains(spinArrowRule) &&
                      sheet.contains(menuArrowRule) &&
                      !sheet.contains(QLatin1String("width: 20px; border: none; }")) &&
                      !sheet.contains(QLatin1String("width: 28px; border: none; }")),
                  "★★ W13 三处小箭头全部归零：下拉框 `::drop-down`、数字框 `::up-button/::down-button`、"
                  "`QToolButton::menu-indicator`（旧的 20 / 28px 那一档一个字都不留）",
                  QStringLiteral("下拉 %1 / 数字 %2 / 菜单 %3")
                      .arg(sheet.contains(dropDownRule) ? 1 : 0)
                      .arg(sheet.contains(spinArrowRule) ? 1 : 0)
                      .arg(sheet.contains(menuArrowRule) ? 1 : 0));

            // 高度：三样**一样高**（并排摆着差 1px 就看得出来）。
            //
            // 量的是**摆进布局之后的高度**（`QWidget::height()`），**不是 `sizeHint()`**。
            // 只量 `sizeHint()` 的话三个数都报 24 —— 可数字框实际占的是 27：
            // 竖向 `Fixed` 的控件过布局时还要取一次"最小高度"（`min-height` 那位设的就是
            // 它），而数字框偏偏只有这一个数能动。**屏幕上那个高度才是判据。**
            const auto spinEditorHeight = [](QSpinBox *spin) {
                auto *edit = spin->findChild<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly);
                if (edit == nullptr) return -1;
                return edit->height() * 1000 + edit->minimumHeight() * 10 + edit->sizeHint().height();
            };
            QWidget sizeHost;
            sizeHost.resize(320, 60);
            auto *sizeEdit = new QLineEdit(QStringLiteral("1280"), &sizeHost);
            sizeEdit->setFocusPolicy(Qt::NoFocus);
            auto *sizeCombo = new QComboBox(&sizeHost);
            sizeCombo->addItem(QStringLiteral("跟随全局"));
            sizeCombo->addItem(QStringLiteral("自定义"));
            sizeCombo->setFocusPolicy(Qt::NoFocus);
            auto *sizeSpin = new QSpinBox(&sizeHost);
            sizeSpin->setValue(6144);
            sizeSpin->setFocusPolicy(Qt::NoFocus);
            auto *sizeRow = new QHBoxLayout(&sizeHost);
            sizeRow->setContentsMargins(8, 8, 8, 8);
            sizeRow->addWidget(sizeEdit);
            sizeRow->addWidget(sizeCombo);
            sizeRow->addWidget(sizeSpin);
            sizeHost.show();
            QApplication::processEvents();
            const int editHeight = sizeEdit->height();
            const int comboHeight = sizeCombo->height();
            const int spinHeight = sizeSpin->height();
            check(editHeight == comboHeight && comboHeight == spinHeight &&
                      editHeight == Theme::inputHeight(),
                  "★★ W13 输入框 / 下拉框 / 数字框**三个一样高**，都是设计稿令牌的常规控件高 36",
                  QStringLiteral("输入框 %1 / 下拉框 %2 / 数字框 %3（`sizeHint()` 分别是 %4 / %5 / %6；"
                                 "数字框 minH %7 / minSizeHint %8 / 内层编辑框 %9）")
                      .arg(editHeight).arg(comboHeight).arg(spinHeight)
                      .arg(sizeEdit->sizeHint().height()).arg(sizeCombo->sizeHint().height())
                      .arg(sizeSpin->sizeHint().height())
                      .arg(sizeSpin->minimumHeight()).arg(sizeSpin->minimumSizeHint().height())
                      .arg(spinEditorHeight(sizeSpin)));
            QApplication::processEvents();
            // 落成图上也得是这么高（`sizeHint()` 是"算出来的"，这一条是"画出来的"）：没有描线了，
            // 数每一行上 `Base` 底那一档有多少个像素，过半的第一行到最后一行就是控件的高度。
            const auto hasInk = [](const QImage &image, int x, int y) {
                return colorDistance(QColor(image.pixel(x, y)), Qt::white) > 200;
            };
            const QImage comboSizeShot = captureWidgetOn(sizeCombo, formBase);
            int borderTop = -1;
            int borderBottom = -1;
            for (int y = 0; y < comboSizeShot.height(); ++y) {
                int hits = 0;
                for (int x = 0; x < comboSizeShot.width(); ++x) {
                    if (colorDistance(QColor(comboSizeShot.pixel(x, y)), inputFill) <= 3) {
                        ++hits;
                    }
                }
                if (hits >= comboSizeShot.width() / 2) {
                    if (borderTop < 0) {
                        borderTop = y;
                    }
                    borderBottom = y;
                }
            }
            // 首末两行是圆角抗锯齿的半格，量到的整格底从第 1 行到倒数第 2 行。
            // 3px 透明边（那条预留的焦点环槽）不铺底：整格底从第 3 行起，到倒数第 4 行止。
            check(comboSizeShot.height() == comboHeight && borderTop >= 0 && borderTop <= 3 &&
                      borderBottom >= comboHeight - 4,
                  "★★ W13 那个高度**真的画出来了**（量的是图上 `Base` 底那一块，从控件第一行铺到"
                  "最后一行，首末行是抗锯齿）",
                  QStringLiteral("边在第 %1 行到第 %2 行（%3 高，`sizeHint()` 说 %4）")
                      .arg(borderTop).arg(borderBottom).arg(borderBottom - borderTop + 1)
                      .arg(comboHeight));

            // 右边那一格**没有箭头**：整块可点，8 号反馈之后不再画任何指示记号。
            // 判据：右侧 18px 宽的一条里，除了 `Base` 底什么都不许有（字在左边，够不到这里）。
            const auto inkIn = [&inputFill](const QImage &image, const QRect &cell) {
                int hits = 0;
                for (int y = cell.top(); y <= cell.bottom(); ++y) {
                    for (int x = cell.left(); x <= cell.right(); ++x) {
                        if (colorDistance(QColor(image.pixel(x, y)), inputFill) > 40) {
                            ++hits;
                        }
                    }
                }
                return hits;
            };
            // NOTE: 用一个**空条目**的下拉框量这一条：`sizeCombo` 上有「跟随全局」四个字，
            // 框只有 79px 宽，字会伸进右边那一带（那不是箭头，量出来是假红）。
            QWidget arrowHost;
            auto *arrowCombo = new QComboBox(&arrowHost);
            arrowCombo->addItem(QString());
            arrowCombo->setFocusPolicy(Qt::NoFocus);
            arrowCombo->setMinimumWidth(140);
            auto *arrowRow = new QHBoxLayout(&arrowHost);
            arrowRow->addWidget(arrowCombo);
            arrowHost.show();
            QApplication::processEvents();
            const QImage arrowShot = captureWidgetOn(arrowCombo, formBase);
            const QRect arrowBand(arrowCombo->width() - 26, 6, 18, arrowCombo->height() - 12);
            const int comboArrowInk = inkIn(arrowShot, arrowBand);
            check(comboArrowInk == 0,
                  "★★ W13 下拉框右边**一个箭头都没有**（那一格里除了 `Base` 底什么都不画）",
                  QStringLiteral("那一格 %1×%2 里有 %3 个不是底色的像素（框宽 %4）")
                      .arg(arrowBand.width()).arg(arrowBand.height()).arg(comboArrowInk)
                      .arg(arrowCombo->width()));
            arrowHost.hide();

            // 数字框「越界回最近合法值」：往 0–5 的框里打进 99，焦点一走就落到 5（不是退回旧值）。
            {
                QWidget clampHost;
                auto *clampSpin = new QSpinBox(&clampHost);
                clampSpin->setRange(0, 5);
                clampSpin->setValue(3);
                auto *clampEdit = new QLineEdit(&clampHost);
                auto *clampRow = new QHBoxLayout(&clampHost);
                clampRow->addWidget(clampSpin);
                clampRow->addWidget(clampEdit);
                clampHost.show();
                clampHost.activateWindow();
                clampSpin->setFocus();
                QApplication::processEvents();
                if (auto *clampLine = clampSpin->findChild<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly)) {
                    clampLine->setText(QStringLiteral("99"));
                }
                clampEdit->setFocus();  // 焦点走开 ⇒ 过滤器钳到边界
                QApplication::processEvents();
                check(clampSpin->value() == 5,
                      "★★ W13 数字框越界键入**钳到最近合法值**（0–5 的框里打 99 ⇒ 5，不是退回旧值）",
                      QStringLiteral("值 %1").arg(clampSpin->value()));
                clampHost.hide();
            }

            // 数字框同理：上下箭头不画（滚轮 / ↑↓ / PgUp / PgDn 照旧，那是控件自己的键盘处理）。
            const QImage spinSizeShot = captureWidgetOn(sizeSpin, formBase);
            const QRect spinArrowCell(sizeSpin->width() - 26, 6, 18, sizeSpin->height() - 12);
            const int spinArrowInk = inkIn(spinSizeShot, spinArrowCell);
            check(spinArrowInk == 0,
                  "★★ W13 数字框没有上下箭头（右边那一格里除了 `Base` 底什么都不画）",
                  QStringLiteral("那一格 %1×%2 里有 %3 个不是底色的像素")
                      .arg(spinArrowCell.width()).arg(spinArrowCell.height()).arg(spinArrowInk));

            // 基样式那圈 95 风格的立体边（`qDrawWinButton`）也不许漏在右边缘里。
            const QRect comboInner = sizeCombo->rect().adjusted(1, 1, -1, -1);
            const auto bevelHits = [&comboSizeShot, sizeCombo, &comboInner]() {
                int hits = 0;
                for (int y = comboInner.top(); y <= comboInner.bottom(); ++y) {
                    if (colorDistance(QColor(comboSizeShot.pixel(sizeCombo->width() - 2, y)),
                                      QColor(0x55, 0x55, 0x55)) <= 30) {
                        ++hits;
                    }
                }
                return hits;
            }();
            check(bevelHits == 0,
                  "★★ W13 右边缘里没有基样式那圈立体边（一个 #555555 都没有）",
                  QStringLiteral("那一列上 %1 个 #555555").arg(bevelHits));

            // ---------- 源码扫描：箭头不许再回来 ----------
            // 像素断言盯的是"今天画出来的样子"；这一条盯的是**别处又写一遍**：
            // 折叠钮再挂 `setArrowType()`、下拉框再搞一个自绘 ∨ 的过滤器，都在这儿红。
            {
                const QDir root = repoRoot();
                const QRegularExpression comments(QStringLiteral(R"(/\*.*?\*/|//[^\n]*)"),
                                                  QRegularExpression::DotMatchesEverythingOption);
                QStringList offenders;
                for (const QString &dir : {QStringLiteral("ui"), QStringLiteral("src")}) {
                    QDirIterator it(root.absoluteFilePath(dir),
                                    {QStringLiteral("*.hpp"), QStringLiteral("*.cpp")},
                                    QDir::Files, QDirIterator::Subdirectories);
                    while (it.hasNext()) {
                        const QString path = it.next();
                        QFile file(path);
                        if (!file.open(QIODevice::ReadOnly)) continue;
                        const QString text = QString::fromUtf8(file.readAll()).remove(comments);
                        for (const QString &needle : {QStringLiteral("setArrowType(Qt::RightArrow"),
                                                      QStringLiteral("setArrowType(Qt::DownArrow"),
                                                      QStringLiteral("paintComboChevron"),
                                                      QStringLiteral("ComboChevronPainter")}) {
                            if (text.contains(needle)) {
                                offenders << root.relativeFilePath(path) + QStringLiteral(" → ") + needle;
                            }
                        }
                    }
                }
                check(offenders.isEmpty(),
                      "★★ W13 全仓不再有折叠箭头 / 自绘 ∨（扫 ui/ 与 src/，去掉注释之后一处都不许留）",
                      offenders.isEmpty() ? QStringLiteral("干净") : offenders.join(QStringLiteral("、")));
            }

            sizeHost.hide();

            // ---------- 「问一句」那三颗按钮按位置分档（不是按文字） ----------
            // 这一段钉的是 `InlineMessageBox` 的 ctor：`destructiveIndex` 那颗按红、默认那颗
            // 是主动作（实心主题色，设计稿 7c）、剩下的按中性。`InlineMessage::question(destructive:true)`
            // 走的就是同一条路（它只是把客户给的那一位转手递给 ctor）。
            // NOTE: 为什么按位置不按文字 —— 按钮表是调用方给的，同一张表里可能出现四颗都叫
            // "做什么"的（世界编辑菜单那张就是），认文字是认不住的。
            InlineMessageBox triBox(
                MessageKind::Question, QStringLiteral("删除实例"),
                QStringLiteral("实例目录和里面的存档都会没。"),
                QStringList{QStringLiteral("删除"), QStringLiteral("导出备份"),
                            QStringLiteral("取消")},
                1, 0);
            const auto kindOf = [&triBox](int index) {
                auto *button = triBox.findChild<QPushButton *>(InlineMessageBox::buttonName(index));
                return button == nullptr ? QStringLiteral("<没有这颗按钮>")
                                         : button->property("wtfmlButtonKind").toString();
            };
            check(kindOf(0) == Theme::buttonKindKey(Theme::ButtonKind::Danger) &&
                      kindOf(1) == Theme::buttonKindKey(Theme::ButtonKind::Primary) &&
                      kindOf(2) == Theme::buttonKindKey(Theme::ButtonKind::Neutral),
                  "★★ W13 三颗按钮的分档：危险那一颗按红、**默认那一颗是主动作**（Primary）、"
                  "剩下那颗（既不是默认也不是危险）才是黑的",
                  QStringLiteral("0=%1 / 1=%2 / 2=%3").arg(kindOf(0), kindOf(1), kindOf(2)));

            InlineMessageBox plainBox(
                MessageKind::Question, QStringLiteral("清空搜索"),
                QStringLiteral("把搜索框里的字去掉。"),
                QStringList{QStringLiteral("清空"), QStringLiteral("取消")}, 1, -1);
            const auto plainKindOf = [&plainBox](int index) {
                auto *button =
                    plainBox.findChild<QPushButton *>(InlineMessageBox::buttonName(index));
                return button == nullptr ? QStringLiteral("<没有这颗按钮>")
                                         : button->property("wtfmlButtonKind").toString();
            };
            check(plainKindOf(1) == Theme::buttonKindKey(Theme::ButtonKind::Primary) &&
                      plainKindOf(0) == Theme::buttonKindKey(Theme::ButtonKind::Neutral),
                  "★★ W13 没有危险动作时（`destructiveIndex = -1`）：默认那颗是主动作，"
                  "另一颗是黑的",
                  QStringLiteral("0=%1 / 1=%2").arg(plainKindOf(0), plainKindOf(1)));
        }

        // ---------- W14 颜色入口 / 无障碍 ----------
        //
        // 要求逐字：「浅色/深色/跟随系统、主题色、文字不透明度、语义色的**实时预览**、
        // **恢复默认**、**导入/导出**；……全部关键操作具备键盘焦点、**可见焦点环**和合理
        // Tab 顺序；动画总开关继续保留，并尊重系统'减少动态效果'」。
        {
            // ① 导入/导出：一份主题进得去、出得来。
            //
            // 这里量的**不是"JSON 写得对不对"**，而是"往返一趟会不会掉东西" —— 掉一样就是
            // "换台机器导进来之后某个地方悄悄变了"，而屏幕上只是那个地方颜色不对。
            Theme::Spec round = Theme::defaultSpec();
            round.presetId = QStringLiteral("dark");
            round.textOpacity = 72;
            round.accent = QColor(0x11, 0x22, 0x33);
            round.danger = QColor(0xaa, 0x00, 0x00);
            round.roles.insert(int(Theme::Role::Window), QColor(0x12, 0x34, 0x56));
            Theme::PartStyle roundPart;
            roundPart.background = QColor(0x0a, 0x0b, 0x0c);
            roundPart.hasBackground = true;
            roundPart.foreground = QColor(0xee, 0xee, 0xee);
            roundPart.hasForeground = true;
            roundPart.textOpacity = 80;
            roundPart.imagePath = QStringLiteral("C:/tmp/wtfml-round.png");
            roundPart.imageName = QStringLiteral("我的图.png");
            roundPart.hasImage = true;
            roundPart.fit = Theme::ImageFit::Contain;
            roundPart.imageOpacity = 40;
            round.parts.insert(int(Theme::Part::Card), roundPart);

            const QJsonObject roundJson = Theme::specToJson(round);
            Theme::Spec back;
            QString roundError;
            const bool roundOk = Theme::specFromJson(roundJson, &back, &roundError);
            const Theme::Spec cleanRound = Theme::normalize(round);
            const Theme::PartStyle backPart =
                back.parts.value(int(Theme::Part::Card));
            check(roundOk && back.presetId == cleanRound.presetId &&
                      back.textOpacity == cleanRound.textOpacity &&
                      sameColor(back.accent, cleanRound.accent) &&
                      sameColor(back.danger, cleanRound.danger) &&
                      back.roles.size() == cleanRound.roles.size() &&
                      sameColor(back.roles.value(int(Theme::Role::Window)),
                                cleanRound.roles.value(int(Theme::Role::Window))) &&
                      back.parts.size() == cleanRound.parts.size() &&
                      sameColor(backPart.background, roundPart.background) &&
                      backPart.hasBackground && backPart.hasForeground &&
                      sameColor(backPart.foreground, roundPart.foreground) &&
                      backPart.textOpacity == roundPart.textOpacity &&
                      backPart.imagePath == roundPart.imagePath &&
                      backPart.imageName == roundPart.imageName &&
                      backPart.fit == roundPart.fit &&
                      backPart.imageOpacity == roundPart.imageOpacity,
                  "★★ W14 一份主题导出再导入，**一样东西都不掉**（配色 / 主题色 / 语义色 / "
                  "角色覆盖 / 骨架覆盖带图带摆法带不透明度）",
                  QStringLiteral("往返 %1；预设 %2→%3，不透明度 %4→%5，骨架 %6→%7")
                      .arg(roundOk ? QStringLiteral("成功") : QStringLiteral("**失败**"))
                      .arg(cleanRound.presetId, back.presetId)
                      .arg(cleanRound.textOpacity)
                      .arg(back.textOpacity)
                      .arg(cleanRound.parts.size())
                      .arg(back.parts.size()));

            // ② 认不出来的一份文件要**被拒**，而且不许把外面那一份改坏（"导入了别的
            // JSON"这件事必须在颜色铺上界面之前就挡下来）。
            Theme::Spec rejected = Theme::defaultSpec();
            rejected.accent = QColor(0x99, 0x88, 0x77);
            const QColor beforeReject = rejected.accent;
            QString rejectError;
            const bool rejectEmpty =
                !Theme::specFromJson(QJsonObject(), &rejected, &rejectError);
            const QString firstError = rejectError;
            rejectError.clear();
            QJsonObject alien;
            alien.insert(QStringLiteral("hello"), QStringLiteral("world"));
            const bool rejectAlien = !Theme::specFromJson(alien, &rejected, &rejectError);
            check(rejectEmpty && rejectAlien && !firstError.isEmpty() &&
                      !rejectError.isEmpty() && sameColor(rejected.accent, beforeReject),
                  "★★ W14 不是一份主题文件（没有 preset / accent / roles 里的任何一条）⇒ "
                  "拒收、说一句为什么、**外面那一份原样不动**",
                  QStringLiteral("空对象 %1 / 别的 JSON %2；accent 现在是 %3")
                      .arg(rejectEmpty ? QStringLiteral("拒了") : QStringLiteral("**收了**"))
                      .arg(rejectAlien ? QStringLiteral("拒了") : QStringLiteral("**收了**"))
                      .arg(rejected.accent.name()));

            // ③ 只写一样也得能用（手写一份 `{"accent": "…"}` 是这一层的设计用法），
            // 缺的那几项是**出厂**而不是"没有"。
            QJsonObject minimal;
            minimal.insert(QStringLiteral("accent"), QStringLiteral("#ff3d8068"));
            Theme::Spec fromMinimal;
            QString minimalError;
            const bool minimalOk = Theme::specFromJson(minimal, &fromMinimal, &minimalError);
            const Theme::Spec factory = Theme::defaultSpec();
            check(minimalOk && sameColor(fromMinimal.accent, QColor(0x3d, 0x80, 0x68)) &&
                      fromMinimal.presetId == factory.presetId &&
                      fromMinimal.textOpacity == factory.textOpacity &&
                      fromMinimal.roles.isEmpty() && fromMinimal.parts.isEmpty(),
                  "★★ W14 手写一份只写主题色的 JSON 收得下：缺的那几项 = **出厂那一份**"
                  "（不是「没有」、也不是「上一次导入留下的」）",
                  QStringLiteral("预设 %1 / 不透明度 %2 / 覆盖 %3+%4")
                      .arg(fromMinimal.presetId)
                      .arg(fromMinimal.textOpacity)
                      .arg(fromMinimal.roles.size())
                      .arg(fromMinimal.parts.size()));

            // ④ 色块那一圈边：深色块给白边、浅色块给黑边。判据取的是**这块色自己的明度**
            // （不跟主题走）—— 一块近黑的主题色搁在浅色页面上，跟着主题走照样是"一个没有
            // 形状的洞"。
            const QColor darkEdge = Theme::swatchBorder(QColor(0x10, 0x10, 0x10));
            const QColor lightEdge = Theme::swatchBorder(QColor(0xf0, 0xf0, 0xf0));
            check(darkEdge.isValid() && lightEdge.isValid() && darkEdge.lightness() > 128 &&
                      lightEdge.lightness() < 128 && darkEdge != lightEdge,
                  "★★ W14 色块那一圈边按**块自己的明度**给（深色块白边 / 浅色块黑边）",
                  QStringLiteral("深块 %1 / 浅块 %2")
                      .arg(darkEdge.name(QColor::HexArgb), lightEdge.name(QColor::HexArgb)));

            // ⑤ 取色卡片里那张色板：块数、名字唯一、而且和预览块 / 输入框**不是同一个名字**
            // （重名的话验证工程与将来的调用点都会认错人）。
            QSet<QString> swatchNames;
            bool swatchNamesOk = InlineMessage::colorSwatchCount() > 0;
            for (int i = 0; i < InlineMessage::colorSwatchCount(); ++i) {
                const QString name = InlineMessage::colorSwatchName(i);
                swatchNamesOk = swatchNamesOk && !name.isEmpty();
                swatchNames.insert(name);
            }
            check(swatchNamesOk && swatchNames.size() == InlineMessage::colorSwatchCount() &&
                      !InlineMessage::colorEditName().isEmpty() &&
                      !InlineMessage::colorPreviewName().isEmpty() &&
                      !swatchNames.contains(InlineMessage::colorEditName()) &&
                      !swatchNames.contains(InlineMessage::colorPreviewName()) &&
                      InlineMessage::colorEditName() != InlineMessage::colorPreviewName(),
                  "★★ W14 取色卡片：色板每一块的名字唯一、和那行十六进制输入框 / 预览块都不重名"
                  "（重名 = 验证工程和调用点都会认错人）",
                  QStringLiteral("%1 块 / 唯一 %2 个")
                      .arg(InlineMessage::colorSwatchCount())
                      .arg(swatchNames.size()));

            // ⑥ 动效开关：产品里始终开，不跟随 Windows 的「动画效果」；测试 / 验收截图可通过
            // `Anim::setEnabled(false)` 跳到终态（没有第二把闸，源码里也不许再出现
            // SystemParametersInfo，见 `UiAnimationsChecks` ⑬ 那一条扫描）。
            const bool userWas = Anim::enabled();
            Anim::setEnabled(true);
            const bool onWhenSwitchedOn = Anim::enabled();
            Anim::setEnabled(false);
            const bool offWhenSwitchedOff = !Anim::enabled();
            Anim::setEnabled(userWas);
            check(onWhenSwitchedOn && offWhenSwitchedOff,
                  "★★ W14 动效默认开、不看系统开关；测试可通过 `Anim::setEnabled()` 切到终态",
                  QStringLiteral("开着 %1 / 关掉 %2")
                      .arg(onWhenSwitchedOn ? QStringLiteral("对") : QStringLiteral("**不对**"))
                      .arg(offWhenSwitchedOff ? QStringLiteral("对") : QStringLiteral("**不对**")));

            // ⑦ 设置页「个性化」那一栏里，这一节该有的控件**都在、都挂对了名字**。
            //
            // 这里量的是"摆出来了"（objectName / 下拉框里那三项 / 滑块的范围）：真按下去
            // 之后发生什么由 `MainWindow` 那十个槽管，在 mw_check。
            //
            // NOTE: 下面量到的是**几何**（四个色块的实际宽高）。这一页此刻不一定在最前面，
            // 藏在栈里的页留着的是**上一次布局**算出来的尺寸 —— 那一次可能发生在主题铺上去
            // 之前（`QSS` 那条 `min-height` 还没生效 / 色块自己那份样式表还没挂），量出来就是
            // 旧值。按仓库那条规矩：读某一页的几何之前先把它翻出来，量完翻回去。
            const int tabBeforeW14 = tabs->currentIndex();
            tabs->setCurrentIndex(MainWindowUI::settingsTabIndex());
            QApplication::processEvents();
            QWidget *settingsPage = tabs->widget(MainWindowUI::settingsTabIndex());
            const auto byName = [settingsPage](const QString &name) {
                return settingsPage == nullptr ? nullptr : settingsPage->findChild<QWidget *>(name);
            };
            auto *presetCombo =
                qobject_cast<QComboBox *>(byName(MainWindowUI::themePresetComboName()));
            const QStringList presetIds = Theme::presetIds();
            bool presetItemsOk = presetCombo != nullptr && presetCombo->count() == presetIds.size();
            for (int i = 0; presetItemsOk && i < presetIds.size(); ++i) {
                presetItemsOk = presetCombo->itemData(i).toString() == presetIds.at(i) &&
                                !presetCombo->itemText(i).isEmpty();
            }
            check(presetItemsOk,
                  "★★ W14 「配色」那一格就是 `Theme::presetIds()` 那三档（认的是 itemData 里"
                  "那个 id，不是那行字）",
                  QStringLiteral("%1 项 / 表里 %2 项")
                      .arg(presetCombo != nullptr ? presetCombo->count() : -1)
                      .arg(presetIds.size()));

            auto *opacitySlider =
                qobject_cast<QSlider *>(byName(MainWindowUI::themeTextOpacitySliderName()));
            auto *opacityPercent = qobject_cast<QLabel *>(
                byName(MainWindowUI::themeTextOpacityPercentName()));
            check(opacitySlider != nullptr && opacityPercent != nullptr &&
                      opacitySlider->minimum() == 0 && opacitySlider->maximum() == 100 &&
                      opacityPercent->text() == MainWindowUI::themePercentText(
                                                    opacitySlider->value()),
                  "★★ W14 「文字不透明度」那根条是 0~100，右边那个百分比**照着条子上的数写**"
                  "（不是另算一遍）",
                  QStringLiteral("%1~%2 / 「%3」")
                      .arg(opacitySlider != nullptr ? opacitySlider->minimum() : -1)
                      .arg(opacitySlider != nullptr ? opacitySlider->maximum() : -1)
                      .arg(opacityPercent != nullptr ? opacityPercent->text() : QString()));

            // 色块那种"钉死的边长"要能在 QSS polish 之后自己回来。
            //
            // 这一条钉的是 W14 偶发（四个色块量到 64x19、min 14 / max 24）的机制：应用级
            // QSS 的 `min-height` 在 polish（第一次 Show、换主题重铺）那一趟会把
            // `setFixedSize()` 钉的最小高度改回 QSS 那一档，而 polish **不发** StyleChange
            // ⇒ `Theme::follow()` 收不到，只能靠 `markIconButton()` 挂的那个补钉
            // （Show / StyleChange / Polish / PolishRequest / 调色板变化 + 一发 0 毫秒收尾）。
            // 这里摆两颗对照：一颗只挂属性（会被打回去）、一颗走 `markIconButton()`。
            {
                QWidget keeperHost;
                const int swatchWidth = MainWindowUI::themeSwatchWidth();
                const int swatchHeight = MainWindowUI::themeSwatchHeight();
                // ① 只挂图标那一档属性、不走 `markIconButton()` 的那一颗：QSS 的
                //    `min-height` 在 polish 里就把 `setFixedSize()` 钉的最小高度打回去。
                QPushButton bare(&keeperHost);
                bare.setProperty("wtfmlButtonSize", Theme::iconButtonSizeValue());
                bare.setFixedSize(swatchWidth, swatchHeight);
                bare.style()->polish(&bare);
                const int clobberedMinimum = bare.minimumHeight();
                // ② 走 `markIconButton()` 的同一颗：Show（QSS 重铺就走在它前后）之后
                //    补钉自己把它钉回原来那一对。
                auto *pinnedButton = new QPushButton(&keeperHost);
                pinnedButton->setFixedSize(swatchWidth, swatchHeight);
                Theme::markIconButton(pinnedButton);
                pinnedButton->style()->polish(pinnedButton);
                keeperHost.show();
                QApplication::processEvents();
                check(clobberedMinimum < swatchHeight &&
                          pinnedButton->minimumHeight() == swatchHeight &&
                          pinnedButton->maximumHeight() == swatchHeight,
                      "★★ W14 色块那种「钉死的边长」被 QSS polish 打回去之后，Show 上会自己钉回来",
                      QStringLiteral("没补钉那颗 polish 后 min %1 / 有补钉那颗 Show 后 min %2 max %3"
                                     "（该是 %4）")
                          .arg(clobberedMinimum)
                          .arg(pinnedButton->minimumHeight())
                          .arg(pinnedButton->maximumHeight())
                          .arg(swatchHeight));
                keeperHost.hide();
            }

            int swatchesFound = 0;
            int swatchesShaped = 0;
            int swatchesFocusable = 0;
            QStringList swatchSizes;
            for (int i = 0; i < MainWindowUI::themeSemanticCount(); ++i) {
                auto *swatch = qobject_cast<QPushButton *>(
                    byName(MainWindowUI::themeSemanticButtonName(i)));
                if (swatch == nullptr) {
                    continue;
                }
                ++swatchesFound;
                swatchSizes << QStringLiteral("%1x%2(min %3 max %4)")
                                   .arg(swatch->width())
                                   .arg(swatch->height())
                                   .arg(swatch->minimumHeight())
                                   .arg(swatch->maximumHeight());
                // 尺寸是唯一出处给的那一对（四个色块一块大小，而且比一颗普通按钮矮得多）。
                if (swatch->width() == MainWindowUI::themeSwatchWidth() &&
                    swatch->height() == MainWindowUI::themeSwatchHeight()) {
                    ++swatchesShaped;
                }
                // 那一句「全部关键操作具备**键盘焦点**」落到色板上就是"Tab 够得着"：
                // 色块是 QPushButton（不用自绘控件就是为了白拿焦点 / 回车 / Tab）。
                if (swatch->focusPolicy() & Qt::TabFocus) {
                    ++swatchesFocusable;
                }
            }
            auto *accentButton =
                qobject_cast<QPushButton *>(byName(MainWindowUI::themeAccentButtonName()));
            check(swatchesFound == MainWindowUI::themeSemanticCount() &&
                      swatchesShaped == MainWindowUI::themeSemanticCount() &&
                      swatchesFocusable == MainWindowUI::themeSemanticCount() &&
                      accentButton != nullptr &&
                      // 主题色那一块**在这一层就涂好了**（它不需要 MainWindow 那一路推语义色），
                      // 所以这里量得到"面是那个色"：块自己的色存在这条动态属性上。
                      accentButton->property("wtfmlSwatchColor").value<QColor>().isValid() &&
                      byName(MainWindowUI::themeSemanticResetName()) != nullptr &&
                      byName(MainWindowUI::themeResetAllButtonName()) != nullptr &&
                      byName(MainWindowUI::themeExportButtonName()) != nullptr &&
                      byName(MainWindowUI::themeImportButtonName()) != nullptr,
                  "★★ W14 那一节摆齐了：四个语义色块（大小一致、**Tab 够得着**）+ 主题色那块"
                  "（这一层就涂上了色）+ 「跟随预设」+ 「恢复全部默认」+ 导入 / 导出",
                  QStringLiteral("语义块 %1 个（对尺寸 %2 / 可聚焦 %3）%4")
                      .arg(swatchesFound)
                      .arg(swatchesShaped)
                      .arg(swatchesFocusable)
                      .arg(swatchSizes.join(QStringLiteral(" "))));

            // ⑧ 那一句「合理 Tab 顺序」：这一节从上到下、从左到右走一遍。
            //
            // 走的是**真的那条焦点链**（`nextInFocusChain()`），不是"我们以为的列表" ——
            // `MainWindowUI::setupSettingsLookPanel()` 末尾那一段 `setTabOrder()` 改的就是
            // 它。只挑**能 Tab 到的**那些（链上还夹着每行那个说明标签，它们不参与 Tab）。
            //
            // NOTE: 这一条**不挑可见性**：链上有几颗在别的分类页上，屏幕上此刻是哪一页都不
            // 该影响"顺序对不对"这件事（真正按 Tab 时 Qt 自己会跳掉不显示的那些）。
            {
                QWidget *cursor = presetCombo;
                QVector<QWidget *> reached;
                for (int step = 0; cursor != nullptr && step < 64 && reached.size() < 12; ++step) {
                    cursor = cursor->nextInFocusChain();
                    if (cursor == nullptr || cursor == presetCombo) {
                        break;
                    }
                    if (cursor->focusPolicy() & Qt::TabFocus) {
                        reached.append(cursor);
                    }
                }
                const QVector<QWidget *> expected = {
                    byName(MainWindowUI::themeAccentButtonName()),
                    byName(MainWindowUI::themeAccentResetName()),
                    byName(MainWindowUI::themeTextOpacitySliderName()),
                    byName(MainWindowUI::themeSemanticButtonName(0)),
                    byName(MainWindowUI::themeSemanticButtonName(1)),
                    byName(MainWindowUI::themeSemanticButtonName(2)),
                    byName(MainWindowUI::themeSemanticButtonName(3)),
                    byName(MainWindowUI::themeSemanticResetName()),
                    byName(MainWindowUI::themeResetAllButtonName()),
                    byName(MainWindowUI::themeExportButtonName()),
                    byName(MainWindowUI::themeImportButtonName()),
                };
                int firstWrong = -1;
                for (int i = 0; i < expected.size(); ++i) {
                    if (firstWrong < 0 && reached.value(i) != expected.at(i)) {
                        firstWrong = i;
                    }
                }
                check(firstWrong < 0 && reached.size() >= expected.size(),
                      "★★ W14 Tab 顺序：按 Tab 从「配色」那格出发，依次是主题色那块 → "
                      "「恢复默认」→ 不透明度那根条 → 四个语义色块 → 「跟随预设」→ "
                      "「恢复全部默认」→ 导出 → 导入（从上到下、从左到右）",
                      QStringLiteral("走到第 %1 颗对不上（走到 %2 颗，该有 %3 颗）")
                          .arg(firstWrong < 0 ? QStringLiteral("没有") : QString::number(firstWrong))
                          .arg(reached.size())
                          .arg(expected.size()));
            }
            // 量完了，把页翻回去（这个进程后面还有别的模块，别让它们从一页没打算打开的
            // 设置页上起步）。
            tabs->setCurrentIndex(tabBeforeW14);
            QApplication::processEvents();
        }

        // ---------- W15 加载器标志在深色底上认得出 ----------
        //
        // Forge 那张是**透明底 + 深藏青铁砧**（`assets/loader/README.md`）：浅色主题下压在白底
        // 列表上看得很清楚，深色主题下就是一团和底色差不多的剪影（r81 的 `dark-1280` 主页
        // 当前实例那一行、管理列表都看不出来）。所以 `loaderMark()` 在深色主题下把它按文字色
        // 重描一遍。这里量的是**结果**：深色那一份的墨要亮（压在深底上看得见），浅色那一份
        // 还是原来那张深色图（不许一刀切成永远浅色）；另外三张自带浅色内容 / 底色，两种主题
        // 下应该一个像素都不变。
        {
            const auto inkLuminance = [](const QImage &image) {
                double sum = 0.0;
                int count = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const QColor pixel = image.pixelColor(x, y);
                        if (pixel.alpha() < 200) continue;
                        sum += 0.299 * pixel.red() + 0.587 * pixel.green() + 0.114 * pixel.blue();
                        ++count;
                    }
                }
                return count == 0 ? -1.0 : sum / count;
            };

            Theme::setSpec(Theme::presetSpec(QStringLiteral("emerald")));
            Theme::apply(app);
            const QImage lightForge = VersionIcons::loaderMark(1, 32).toImage();
            const QImage lightFabric = VersionIcons::loaderMark(0, 32).toImage();
            const QImage lightNeo = VersionIcons::loaderMark(2, 32).toImage();
            const QImage lightOpti = VersionIcons::loaderMark(3, 32).toImage();

            Theme::setSpec(Theme::presetSpec(QStringLiteral("dark")));
            Theme::apply(app);
            const QImage darkForge = VersionIcons::loaderMark(1, 32).toImage();
            const QImage darkFabric = VersionIcons::loaderMark(0, 32).toImage();
            const QImage darkNeo = VersionIcons::loaderMark(2, 32).toImage();
            const QImage darkOpti = VersionIcons::loaderMark(3, 32).toImage();

            check(inkLuminance(lightForge) >= 0 && inkLuminance(lightForge) < 90,
                  "★ W15 浅色主题下 Forge 还是原来那把深色铁砧（不是一路浅色）",
                  QStringLiteral("墨的明度 %1").arg(inkLuminance(lightForge)));
            check(inkLuminance(darkForge) > 128,
                  "★★ W15 深色主题下 Forge 的铁砧按文字色重描过（压在深底上看得见）",
                  QStringLiteral("墨的明度 %1").arg(inkLuminance(darkForge)));
            check(darkFabric == lightFabric && darkNeo == lightNeo && darkOpti == lightOpti,
                  "★ W15 另外三张自带浅色内容 / 底色，两种主题下一个像素都不动");
            check(darkForge != lightForge,
                  "W15 Forge 的那两份不是同一张图（换主题真的会换样子）");

            // 铺回浅色：后面那几段（还有别的模块）都按浅色那一身拿像素。
            Theme::setSpec(Theme::presetSpec(QStringLiteral("emerald")));
            Theme::apply(app);
        }

        // ---------- 收尾：铺回出厂那一份 ----------
        // 这一段动过全局主题，收尾要铺回去：一是"换过深色预设之后能原样收回出厂那一份"
        // （光"什么都不做"是收不回的，深色那份还挂在应用上 —— 这条断言量的就是那个收回
        // 的岔路）；二是别让这个进程停在一个半路的状态上。
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        check(app.palette().color(QPalette::Window).rgb() == factoryTokens.page.rgb() &&
                  app.palette().color(QPalette::WindowText).rgb() == factoryTokens.text.rgb(),
              "★★ W 收尾 从深色预设铺回出厂那一份之后，调色板又回到**出厂那一份**"
              "（暮色雪山那一支收得回来，而且是原样收回来的）",
              QStringLiteral("量到 %1 / %2，该是 %3 / %4")
                  .arg(app.palette().color(QPalette::Window).name(),
                       app.palette().color(QPalette::WindowText).name(),
                       factoryTokens.page.name(), factoryTokens.text.name()));
    }
    return 0;
}



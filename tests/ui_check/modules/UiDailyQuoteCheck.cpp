#include "Support.hpp"

#include "MainWindowPageLayout.hpp"
#include "MainWindowUI.hpp"
#include "Theme.hpp"

#include <QFontMetrics>

// 主页「每日一言」那一行：在有 / 没有之间来回摆，量的是**版面和字体**，不是网络。
//
// 位置就是被删掉的英文副题那一格（8fc9f5e 的反向 diff）：大标题下面、启动区上面，
// 单行小字。这一层要证的三件事：
// 1. 有句子时它在、没句子/关掉时它**不占位**（启动区那一块回到原位）；
// 2. 中文 / 英文 / 日文各自选的 family 对得上（不靠像素，靠 family 列表）；
// 3. 字号随句长在区间里取放得下整句的最大一号（宽窗 18–24、窄窗 16–20），到下限还放不下就收省略号。
int UiDailyQuoteCheck(QApplication &app) {
    app.setStyle(Theme::createStyle());
    Theme::setSpec(Theme::defaultSpec());
    Theme::apply(app);
    Anim::setEnabled(false);

    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(1280, 800);
    window.show();
    app.processEvents();

    QLabel *quote = ui.getHomeHeroQuote();
    check(quote != nullptr, "DQ-UI1 主页建出了「每日一言」那一行");
    if (quote == nullptr) return 1;
    check(quote->objectName() == MainWindowUI::homeHeroQuoteName(), "DQ-UI2 它的 objectName 是 homeHeroQuote");
    check(quote->isHidden(), "DQ-UI3 还没句子时它是隐藏的（大标题下面什么都不占）");
    check(!quote->wordWrap(), "DQ-UI4 不换行（单行）");
    check(quote->property("elideMode").toInt() == int(Qt::ElideRight),
          "DQ-UI5 是 ElidedLabel，装不下从尾巴收省略号");

    QLabel *title = window.findChild<QLabel *>(QStringLiteral("homeHeroTitle"));
    check(title != nullptr, "DQ-UI6 大标题在（拿它的纵向位置当版面基准）");
    const int titleY = title != nullptr ? title->mapTo(&window, QPoint(0, 0)).y() : -1;

    // 字号判据：在 [lo, hi] 里；整句放得下（除非已到下限、交给省略号）；且再大一号就放不下（除非已到上限）。
    const auto advanceAt = [quote](int px) {
        QFont f = quote->font();
        f.setPixelSize(px);
        return QFontMetrics(f).horizontalAdvance(quote->text());
    };
    const auto sizeOk = [&](int lo, int hi) {
        const int px = quote->font().pixelSize();
        const int room = quote->contentsRect().width();
        const bool inRange = px >= lo && px <= hi;
        const bool fits = advanceAt(px) <= room || px == lo;
        const bool largest = px == hi || advanceAt(px + 1) > room;
        return inRange && fits && largest;
    };
    const auto sizeDetail = [&] {
        return QStringLiteral("%1px，句宽 %2 / 可用 %3")
            .arg(quote->font().pixelSize())
            .arg(advanceAt(quote->font().pixelSize()))
            .arg(quote->contentsRect().width());
    };

    // ---- 中文：文楷 ----
    ui.setHomeHeroQuote(QStringLiteral("长河千嶂，大荒孤城，历历在目。"),
                        QStringLiteral("— 令《明日方舟》"), Theme::QuoteScript::Han);
    app.processEvents();
    check(quote->isVisible() && quote->text() == QStringLiteral("长河千嶂，大荒孤城，历历在目。"),
          "DQ-UI7 有句子时显示出来，文字就是那一句");
    check(quote->font().families().value(0).contains(QStringLiteral("WenKai")),
          "DQ-UI8 中文选的是文楷（family 列表第一档）", quote->font().families().join(QStringLiteral(" / ")));
    check(sizeOk(MainWindowPageLayout::kHomeQuoteMinPixelSize, MainWindowPageLayout::kHomeQuoteMaxPixelSize),
          "DQ-UI9 宽窗字号在 18–24 里，是放得下整句的最大一号", sizeDetail());
    check(quote->toolTip() == QStringLiteral("— 令《明日方舟》"), "DQ-UI10 悬停提示是「— 作者《出处》」",
          quote->toolTip());
    check(quote->height() > 0 && quote->mapTo(&window, QPoint(0, 0)).y() > titleY,
          "DQ-UI11 它在大标题下方、自己占得到高度（不是压在标题上）",
          QStringLiteral("titleY=%1 quoteY=%2 h=%3")
              .arg(titleY)
              .arg(quote->mapTo(&window, QPoint(0, 0)).y())
              .arg(quote->height()));

    // ---- 英文：Dancing Script ----
    ui.setHomeHeroQuote(QStringLiteral("The significance of life is living."),
                        QStringLiteral("— Jiddu Krishnamurti"), Theme::QuoteScript::Latin);
    app.processEvents();
    check(quote->font().families().value(0).contains(QStringLiteral("Dancing")),
          "DQ-UI12 纯拉丁选的是 Dancing Script", quote->font().families().join(QStringLiteral(" / ")));

    // ---- 日文：系统日文字体链（文楷没有假名） ----
    ui.setHomeHeroQuote(QStringLiteral("希望とシェイクハンドして、真新しい自分。"),
                        QStringLiteral("— 大森元貴《Brand New》"), Theme::QuoteScript::Kana);
    app.processEvents();
    const QStringList jp = quote->font().families();
    check(jp.value(0) == QStringLiteral("Yu Mincho") && jp.contains(QStringLiteral("Meiryo UI")),
          "DQ-UI13 日文排的是 Yu Mincho → Yu Gothic UI → Meiryo UI", jp.join(QStringLiteral(" / ")));
    check(!quote->font().families().value(0).contains(QStringLiteral("WenKai")),
          "DQ-UI14 日文不把文楷排第一（文楷没有假名，混排会一行两种字形）");
    check(!quote->property("wtfmlDisplay").toBool(),
          "DQ-UI15 日文不挂 wtfmlDisplay（系统字体不算手写体）");

    // ---- 窄窗换区间 ----
    window.resize(900, 700);
    app.processEvents();
    check(sizeOk(MainWindowPageLayout::kHomeQuoteNarrowMinPixelSize, MainWindowPageLayout::kHomeQuoteNarrowMaxPixelSize),
          "DQ-UI16 窄窗（<1024）字号在 16–20 里，是放得下整句的最大一号", sizeDetail());
    window.resize(1280, 800);
    app.processEvents();
    check(sizeOk(MainWindowPageLayout::kHomeQuoteMinPixelSize, MainWindowPageLayout::kHomeQuoteMaxPixelSize), "DQ-UI17 回到宽窗按 18–24 重挑",
          sizeDetail());

    // ---- 短句字号大于长句；换句不挪下面的版面 ----
    for (const QSize size : {QSize(1280, 800), QSize(900, 620)}) {
        window.resize(size);
        app.processEvents();
        const QString where = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
        ui.setHomeHeroQuote(QStringLiteral("Be confident, not certain."), QStringLiteral("— Eleanor Roosevelt"),
                            Theme::QuoteScript::Latin);
        app.processEvents();
        const int shortPx = quote->font().pixelSize();
        const int shortBottom = quote->mapTo(&window, QPoint(0, quote->height())).y();
        // 宽窗下句库里最长那句（80 字符）在 24px 也放得下；这里用贴着 128 字硬上限的长句，两档都放不下上限。
        ui.setHomeHeroQuote(
            QStringLiteral("Supreme excellence consists of breaking the enemy's resistance without fighting, "
                           "and the wise win before the battle ever begins."),
            QStringLiteral("— Sun Tzu"), Theme::QuoteScript::Latin);
        app.processEvents();
        const int longPx = quote->font().pixelSize();
        const int longBottom = quote->mapTo(&window, QPoint(0, quote->height())).y();
        check(shortPx > longPx, QStringLiteral("DQ-UI26 %1：短句字号大于长句").arg(where),
              QStringLiteral("短 %1px / 长 %2px").arg(shortPx).arg(longPx));
        ui.setHomeHeroQuote(QStringLiteral("以有涯随无涯，殆已！"), QStringLiteral("— 庄周《庄子》"),
                            Theme::QuoteScript::Han);
        app.processEvents();
        const int hanBottom = quote->mapTo(&window, QPoint(0, quote->height())).y();
        check(shortBottom == longBottom && longBottom == hanBottom,
              QStringLiteral("DQ-UI27 %1：换句（含换语种）这一行底边不动，行高按上限预留").arg(where),
              QStringLiteral("短 %1 / 长 %2 / 中文 %3").arg(shortBottom).arg(longBottom).arg(hanBottom));
    }
    window.resize(1280, 800);
    app.processEvents();

    // ---- 收起来：不占位 ----
    ui.setHomeHeroQuote(QString(), QString(), Theme::QuoteScript::Han);
    app.processEvents();
    check(quote->isHidden(), "DQ-UI18 空句子 → 隐藏");
    check(title != nullptr && title->mapTo(&window, QPoint(0, 0)).y() == titleY,
          "DQ-UI19 收起之后版面回到原来那一格（大标题位置不变、那一行不占位）");
    check(quote->toolTip().isEmpty(), "DQ-UI20 收起时不留悬停提示");

    // ---- 引文只吃标题下面那块空白：开关前后「当前实例」以下整块不动、最近条不被挤出底边 ----
    //
    // 这一行是最容易出回归的一处：加高度谁都会加，但主页竖着已经不剩多少空档了 ——
    // 只有它**占的是标题与启动区之间那块空白**，开关才不该把下面的东西推下去。
    MainWindowUI::HomeQuickItem item;
    item.key = QStringLiteral("demo");
    item.name = QStringLiteral("1.21.4 Fabric");
    item.detail = QStringLiteral("Fabric 0.16.9 · 1.21.4");
    item.current = true;
    ui.setHomeQuickRows({item}, {item}, {item});
    ui.setHomeQuickMode(MainWindowUI::HomeQuickMode::Current);
    // 「当前实例」那行字是灌了实例之后才建出来的，所以先灌再看它在不在。
    auto *eyebrow = window.findChild<QWidget *>(QStringLiteral("homeCurrentEyebrow"));
    check(eyebrow != nullptr, "DQ-UI23 「当前实例」那一行字在（量它的纵向位置当基准）");
    if (eyebrow != nullptr) {
        const auto pump = [] {
            // 实例带 / 每日一言都往父链上排队 LayoutRequest；只 processEvents 两次时，
            // 首次量到的可能还是“卡片还没露出来”的上一版版面（DQ-UI24/25）。
            for (int pass = 0; pass < 4; ++pass) {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                QApplication::processEvents();
            }
        };
        const auto yOf = [&](QWidget *w) { return w->mapTo(&window, QPoint(0, 0)).y(); };
        for (const QSize size : {QSize(1280, 800), QSize(900, 620)}) {
            window.resize(size);
            pump();
            ui.setHomeHeroQuote(QString(), QString(), Theme::QuoteScript::Han);
            pump();
            const int off = yOf(eyebrow);
            auto *card = window.findChild<QWidget *>(QStringLiteral("homeQuickCard_demo"));
            const int cardBottomOff = card == nullptr ? -1 : yOf(card) + card->height();
            ui.setHomeHeroQuote(QStringLiteral("长河千嶂，大荒孤城，历历在目。"),
                                QStringLiteral("— 令《明日方舟》"), Theme::QuoteScript::Han);
            pump();
            const int on = yOf(eyebrow);
            const int cardBottomOn = card == nullptr ? -1 : yOf(card) + card->height();
            const QString where = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
            check(qAbs(on - off) <= 4,
                  QStringLiteral("DQ-UI24 %1：开关每日一言前后「当前实例」不动（最多差 4 px）").arg(where),
                  QStringLiteral("关 %1 / 开 %2，差 %3").arg(off).arg(on).arg(on - off));
            check(card != nullptr && cardBottomOn <= window.height() && cardBottomOff <= window.height(),
                  QStringLiteral("DQ-UI25 %1：最近条那张卡整张都在窗口里（开关前后都一样）").arg(where),
                  card == nullptr ? QStringLiteral("没找到卡片")
                                  : QStringLiteral("卡片底 关 %1 / 开 %2，窗口高 %3")
                                        .arg(cardBottomOff)
                                        .arg(cardBottomOn)
                                        .arg(window.height()));
        }
    }

    // ---- 大标题问候：单行、随名字变、长名字只截名字 ----
    if (title != nullptr) {
        window.resize(1280, 800);
        app.processEvents();
        check(!title->wordWrap(), "HG-UI1 大标题不折行（单行）");
        const QStringList families = title->font().families();
        check(families.value(0).contains(QStringLiteral("WenKai")) && families.value(1) == QStringLiteral("KaiTi"),
              "HG-UI2 文楷 Light 打头，文楷没有的字先落系统楷体", families.join(QStringLiteral(" / ")));
        // 名字字体按整段判：纯英文 → Dancing Script ×1.2、字距 0；带中文（纯中文或中英混）→ 整段文楷。
        const auto nameFontIs = [&](const QString &name) {
            ui.setHomeHeroTitle(QStringLiteral("晚上好 ") + name, name);
            return ui.homeHeroTitleNameFont();
        };
        const QFont steve = nameFontIs(QStringLiteral("Steve"));
        check(steve.families().value(0).contains(QStringLiteral("Dancing")) &&
                  steve.pixelSize() == qRound(title->font().pixelSize() * 1.2) && steve.letterSpacing() == 0,
              "HG-UI2b 纯英文名字整段用 Dancing Script，放大 1.2 倍、字距 0（连笔不断）",
              QStringLiteral("%1 %2px").arg(steve.families().join(QStringLiteral(" / "))).arg(steve.pixelSize()));
        for (const QString &name : {QStringLiteral("小明"), QStringLiteral("Steve小明")}) {
            const QFont f = nameFontIs(name);
            check(f.families() == title->font().families() && f.pixelSize() == title->font().pixelSize(),
                  QStringLiteral("HG-UI2c 「%1」带中文：整个名字用文楷，不拆成两种字").arg(name),
                  f.families().join(QStringLiteral(" / ")));
        }
        ui.setHomeHeroTitle(QStringLiteral("欢迎来到 WTFML"), QString());
        check(ui.homeHeroTitleNameFont().families().value(0).contains(QStringLiteral("Dancing")),
              "HG-UI2d 「欢迎来到 WTFML」里的 WTFML 用 Dancing Script");
        ui.setHomeHeroTitle(QStringLiteral("晚上好 Steve"), QStringLiteral("Steve"));
        app.processEvents();
        check(title->text() == QStringLiteral("晚上好 Steve") &&
                  ui.homeHeroTitleShown() == QStringLiteral("晚上好 Steve"),
              "HG-UI3 短名字整句照写", ui.homeHeroTitleShown());
        ui.setHomeHeroTitle(QStringLiteral("晚上好 Alex"), QStringLiteral("Alex"));
        app.processEvents();
        check(title->text() == QStringLiteral("晚上好 Alex"), "HG-UI4 换账号后标题跟着换名字",
              title->text());
        auto *pill = window.findChild<QWidget *>(MainWindowUI::accountSkinButtonName());
        for (const QSize size : {QSize(1280, 800), QSize(900, 620)}) {
            window.resize(size);
            ui.setHomeHeroTitle(QStringLiteral("晚上好 Steve"), QStringLiteral("Steve"));
            for (int i = 0; i < 4; ++i) app.processEvents();
            const int shortWidth = title->width();
            const int lineHeight = title->height();
            // 可用宽度先量：大标题的宽度交给布局（Ignored），不随文字长短变，所以下面
            // 按它定名字该有多长。
            const int room = title->contentsRect().width();
            // 名字要长到**确实装不下**。名字那一段按整段英文换成 Dancing Script（1.2 倍
            // 字号、字距 0），"ThisIsAVeryLongName_16" 在 60 px 下连问候语一共 792 px，
            // 而 1280x800 这一栏的可用宽度是 892 px —— 写死的一串装得下，"必须截"的断言
            // 就跟着字体度量翻脸（HG-UI5 2026-11-18 就是这么红的）。这里按实测字宽把名字
            // 接到可用宽度的两倍，任何字体度量下都一定溢出。
            QString longName = QStringLiteral("ThisIsAVeryLongName_16");
            const QFont nameFont = ui.homeHeroTitleNameFont();
            while (QFontMetrics(nameFont).horizontalAdvance(longName) < room * 2 &&
                   longName.size() < 512) {
                longName += QStringLiteral("ThisIsAVeryLongName_16");
            }
            ui.setHomeHeroTitle(QStringLiteral("晚上好 ") + longName, longName);
            for (int i = 0; i < 4; ++i) app.processEvents();
            const QString shown = ui.homeHeroTitleShown();
            const QString where = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
            check(shown.startsWith(QStringLiteral("晚上好 This")) && shown.endsWith(QStringLiteral("…")) &&
                      ui.homeHeroTitleShownWidth() <= room,
                  QStringLiteral("HG-UI5 %1：长名字只截名字，问候留着、省略号收尾，整句装得下").arg(where),
                  QStringLiteral("%1（%2 / %3 px）")
                      .arg(shown)
                      .arg(ui.homeHeroTitleShownWidth())
                      .arg(room));
            check(title->width() == shortWidth && title->height() == lineHeight,
                  QStringLiteral("HG-UI6 %1：长名字不把左栏抻宽、不折成两行").arg(where),
                  QStringLiteral("短 %1x%2 / 长 %3x%4")
                      .arg(shortWidth).arg(lineHeight).arg(title->width()).arg(title->height()));
            const QRect titleRect(title->mapTo(&window, QPoint(0, 0)), title->size());
            check(pill == nullptr || !titleRect.intersects(QRect(pill->mapTo(&window, QPoint(0, 0)), pill->size())),
                  QStringLiteral("HG-UI7 %1：长名字的标题不压右上账号胶囊").arg(where));
        }
        window.resize(1280, 800);
        app.processEvents();
    }

    // ---- 切主题：样式重刷不许把后来 setFont() 的字体还原 ----
    // 根因：`Theme::apply()` → `repolishWidgets()` 里 `style()->polish()`，QStyleSheetStyle 会把控件的字体
    // 还原成样式表第一次落字体时存下的那一份（`_q_styleSheetWidgetFont`），后来 setFont() 的全丢。
    // 症状：主页实例名从 40 缩到下限、英文每日一言从 Dancing Script 退成文楷。
    {
        window.resize(900, 619);
        MainWindowUI::HomeQuickItem item;
        item.key = item.name = QStringLiteral("Shaders 1.21.1");
        item.detail = QStringLiteral("Minecraft 1.21.1 / Fabric 0.16.9");
        item.current = item.recent = true;
        ui.setHomeQuickRows({item}, {}, {item});
        ui.setHomeQuickMode(MainWindowUI::HomeQuickMode::Current);
        ui.setHomeHeroQuote(QStringLiteral("Imagination is more important than knowledge."),
                            QStringLiteral("— Albert Einstein"), Theme::QuoteScript::Latin);
        for (int i = 0; i < 4; ++i) app.processEvents();
        QLabel *name = window.findChild<QLabel *>(QStringLiteral("homeCurrentName"));
        check(name != nullptr, "DQ-UI28 主页有当前实例名（切主题字体断言的对象）");
        if (name != nullptr) {
            const int namePx = name->font().pixelSize();
            const QString quoteFamily = quote->font().families().value(0);
            const int quotePx = quote->font().pixelSize();
            for (const QString &id : {QStringLiteral("emerald"), QStringLiteral("ocean"), QStringLiteral("dark")}) {
                Theme::setSpec(Theme::presetSpec(id));
                Theme::apply(app);
                for (int i = 0; i < 4; ++i) app.processEvents();
                check(name->font().pixelSize() == namePx,
                      QStringLiteral("DQ-UI29 切到 %1 后实例名字号不变").arg(id),
                      QStringLiteral("%1px → %2px").arg(namePx).arg(name->font().pixelSize()));
                check(quote->font().families().value(0) == quoteFamily && quote->font().pixelSize() == quotePx,
                      QStringLiteral("DQ-UI30 切到 %1 后每日一言字体族与字号不变").arg(id),
                      QStringLiteral("%1 %2px → %3 %4px")
                          .arg(quoteFamily).arg(quotePx)
                          .arg(quote->font().families().value(0)).arg(quote->font().pixelSize()));
            }
        }
        window.resize(1280, 800);
        app.processEvents();
    }

    // ---- 设置页那个开关 ----
    auto *dailyQuote = window.findChild<QCheckBox *>(MainWindowUI::dailyQuoteCheckName());
    check(dailyQuote != nullptr, "DQ-UI21 设置 → 个性化 → 外观 有「每日一言」开关");
    if (dailyQuote != nullptr) {
        check(dailyQuote->accessibleDescription().contains(QStringLiteral("hitokoto")) &&
                  dailyQuote->accessibleDescription().contains(QStringLiteral("zenquotes.io")),
              "DQ-UI22 说明文字写明发往哪几个站、给 ZenQuotes 署名",
              dailyQuote->accessibleDescription());
    }
    return 0;
}

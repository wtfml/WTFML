#include "Support.hpp"
#include "OverlayHost.hpp"
#include "PresetModReview.hpp"
#include "PresetOptionCatalog.hpp"
#include "Theme.hpp"
#include "TitleBar.hpp"
#include "UiLanguage.hpp"
#include "WorldSettingsPage.hpp"
#include "settings/LauncherSettings.hpp"

#include <QLocale>
#include <QSettings>
#include <QTranslator>

// 界面语言那一条链路：解析（纯函数）→ 装翻译 → 导航轨真的变英文。
// 产品的接线是 src/main.cpp 里那三行；这里验的是同一套函数和同一份资源。
int UiLanguageChecks(MainWindowUI &ui, QTabWidget *tabs) {
    std::printf("===== 界面语言 =====\n");
    using UiLanguage::resolve;

    // ---- 1. 解析：system 看系统 UI 语言，显式档不跟随系统 ----
    check(resolve(QStringLiteral("system"), {QStringLiteral("zh-CN")}) == QStringLiteral("zh_CN"),
          "system + 简中系统 → zh_CN");
    check(resolve(QStringLiteral("system"), {QStringLiteral("zh-TW")}) == QStringLiteral("zh_CN"),
          "system + 繁体系统 → zh_CN");
    check(resolve(QStringLiteral("system"), {QStringLiteral("en-US")}) == QStringLiteral("en"),
          "system + 英文系统 → en");
    check(resolve(QStringLiteral("system"), {}) == QStringLiteral("en"),
          "system + 系统没报语言 → en");
    check(resolve(QStringLiteral("en"), {QStringLiteral("zh-CN")}) == QStringLiteral("en"),
          "显式 en 不跟随中文系统");
    check(resolve(QStringLiteral("zh_CN"), {QStringLiteral("en-US")}) == QStringLiteral("zh_CN"),
          "显式 zh_CN 不跟随英文系统");
    check(resolve(QStringLiteral("乱写"), {QStringLiteral("en-US")}) == QStringLiteral("en"),
          "认不出来的档位按默认档算：跟随英文系统");

    // ---- 1b. 首次启动与设置页共用语言默认值 ----
    {
        QSettings probe;
        const bool had = probe.contains(QStringLiteral("General/Language"));
        const QVariant keep = probe.value(QStringLiteral("General/Language"));
        probe.remove(QStringLiteral("General/Language"));
        check(UiLanguage::readSetting() == QStringLiteral("system"),
              "没设过 General/Language 时读出来是跟随系统",
              UiLanguage::readSetting());
        check(resolve(UiLanguage::readSetting(), {QStringLiteral("en-US")}) == QStringLiteral("en"),
              "没设过时跟随系统：英文系统用英文");
        check(LauncherSettings().language() == UiLanguage::readSetting(),
              "没设过语言时启动器设置与首次启动使用同一档位");
        probe.setValue(QStringLiteral("General/Language"), QStringLiteral("zh_CN"));
        check(LauncherSettings().language() == QStringLiteral("zh_CN"),
              "启动器设置保留用户显式选择的简体中文");
        if (had) probe.setValue(QStringLiteral("General/Language"), keep);
        else probe.remove(QStringLiteral("General/Language"));
        probe.sync();
    }

    // ---- 2. 中文这一档：导航轨就是源文（没装翻译）----
    check(tabs != nullptr, "选项卡在，能读导航文字");
    const auto navTexts = [](QTabWidget *tabWidget) {
        QStringList out;
        if (tabWidget == nullptr) return out;
        for (int i = 0; i < 5; ++i) out << tabWidget->tabText(i);
        return out;
    };
    check(navTexts(tabs) == QStringList({QStringLiteral("主页"), QStringLiteral("管理"),
                                        QStringLiteral("下载"), QStringLiteral("设置"),
                                        QStringLiteral("智能体")}),
          "zh_CN（默认、测试钉住的那一档）导航轨是中文", navTexts(tabs).join(QLatin1Char('/')));

    // ---- 3. 「语言」那一行：三个选项，itemData 就是落盘的记号 ----
    QComboBox *language = ui.getLanguageCombo();
    check(language != nullptr && language->objectName() == MainWindowUI::languageComboName(),
          "设置页有「语言」下拉框（按 objectName 取得回）");
    check(language != nullptr && language->count() == 3 &&
              language->itemData(0).toString() == QStringLiteral("system") &&
              language->itemData(1).toString() == QStringLiteral("zh_CN") &&
              language->itemData(2).toString() == QStringLiteral("en"),
          "三档顺序是 跟随系统 / zh_CN / en");
    check(language != nullptr && language->itemText(0) == QStringLiteral("跟随系统"),
          "中文界面下「跟随系统」写中文", language != nullptr ? language->itemText(0) : QString());
    check(language != nullptr && language->itemText(1) == QStringLiteral("简体中文") &&
              language->itemText(2) == QStringLiteral("English"),
          "另外两档各写各自语言的名字（不翻）",
          language != nullptr ? language->itemText(2) : QString());

    // ---- 4. 设成 en：读设置 → 解析 → 装翻译 → 新建的界面是英文 ----
    QSettings().setValue(QStringLiteral("General/Language"), QStringLiteral("en"));
    const QString resolved = resolve(UiLanguage::readSetting(), QLocale::system().uiLanguages());
    check(resolved == QStringLiteral("en"), "盘上设成 en 时解析结果是 en（不看系统语言）");

    QTranslator translator;
    check(UiLanguage::loadEnglish(&translator),
          "英文翻译在 exe 的资源里（:/i18n/wtfml_en.qm）");
    check(!translator.isEmpty(), "那一份 .qm 不是空的");
    qApp->installTranslator(&translator);
    {
        QMainWindow englishWindow;
        MainWindowUI englishUi(&englishWindow);
        QTabWidget *englishTabs = englishUi.getTabWidget();
        check(navTexts(englishTabs) == QStringList({QStringLiteral("Home"), QStringLiteral("Library"),
                                                    QStringLiteral("Download"), QStringLiteral("Settings"),
                                                    QStringLiteral("Agent")}),
              "装了英文翻译之后导航轨是 Home / Library / Download / Settings / Agent",
              navTexts(englishTabs).join(QLatin1Char('/')));
        check(englishUi.getLanguageCombo() != nullptr &&
                  englishUi.getLanguageCombo()->itemText(0) == QStringLiteral("Follow system"),
              "英文界面下「跟随系统」跟着翻成 Follow system");
        check(englishUi.getLanguageCombo() != nullptr &&
                  englishUi.getLanguageCombo()->itemText(2) == QStringLiteral("English"),
              "英文界面下 English 那一档仍写作 English",
              englishUi.getLanguageCombo() != nullptr ? englishUi.getLanguageCombo()->itemText(2) : QString());
        // 图标按**序号**认（见 ui/TitleBar.cpp 的 iconForIndex）：英文下五颗导航照样有图标。
        // 以前按标签文字认，每加一门语言都得去 TitleBar 加一列，漏了就是几个没图标的按钮。
        bool iconsOk = englishUi.titleBar() != nullptr && englishUi.titleBar()->navCount() == 5;
        QStringList iconDetail;
        if (englishUi.titleBar() != nullptr) {
            for (int i = 0; i < englishUi.titleBar()->navCount(); ++i) {
                auto *button = static_cast<TitleBarButton *>(englishUi.titleBar()->navButton(i));
                const bool hasIcon =
                    button != nullptr && button->navIcon() != TitleBarButton::NavIcon::None;
                iconsOk = iconsOk && hasIcon;
                iconDetail << (hasIcon ? QStringLiteral("ok") : QStringLiteral("none"));
            }
        }
        check(iconsOk, "英文界面下五个导航按钮都有图标（按序号认，不认标签文字）",
              iconDetail.join(QLatin1Char('/')));

        MainWindowUI::HomeQuickItem instance;
        instance.name = QStringLiteral("Example instance");
        instance.key = QStringLiteral("example");
        englishUi.setHomeQuickRows({instance}, {}, {});
        auto *launch = englishWindow.findChild<QPushButton *>(QStringLiteral("homeCurrentLaunch"));
        check(launch != nullptr && launch->text() == QStringLiteral("Play"), "英文主页启动按钮显示 Play");
        englishUi.setHomeLaunchRunning(true);
        check(launch != nullptr && launch->text() == QStringLiteral("Stop game"), "英文主页运行时显示 Stop game");

        check(OverlayHost::backButtonText() == QStringLiteral("Back"), "英文内嵌页面显示 Back");
        check(Theme::presetName(QStringLiteral("dark")) == QStringLiteral("Alpine Twilight"),
              "英文主题下拉框显示本地化预设名");
        check(PresetModReview::tr("确认模组预设") == QStringLiteral("Confirm mod preset"),
              "英文模组预设确认框使用自身翻译上下文");
        const auto *clouds = PresetOptionCatalog::find(QStringLiteral("renderClouds"));
        const auto *forward = PresetOptionCatalog::find(QStringLiteral("key_key.forward"));
        check(clouds != nullptr && clouds->choices.size() == 3 &&
                  QString::fromLatin1(clouds->choices[0].value) == QStringLiteral("false") &&
                  QString::fromLatin1(clouds->choices[1].value) == QStringLiteral("fast") &&
                  QString::fromLatin1(clouds->choices[2].value) == QStringLiteral("true") &&
                  forward != nullptr && forward->initial == QStringLiteral("key.keyboard.w"),
              "英文预设保留 Minecraft 配置值和按键标识");
        check(QCoreApplication::translate("PresetOptionCatalog", "精细") == QStringLiteral("Fancy"),
              "英文预设翻译云效果选项");

        WorldSettingsPage world;
        auto *difficulty = world.findChild<QComboBox *>(WorldSettingsPage::difficultyName());
        const QStringList labels{QStringLiteral("Peaceful"), QStringLiteral("Easy"),
                                 QStringLiteral("Normal"), QStringLiteral("Hard")};
        QStringList options;
        if (difficulty != nullptr) {
            for (int index = 0; index < difficulty->count(); ++index) options << difficulty->itemText(index);
        }
        check(options == labels, "英文世界难度使用 Minecraft 原版术语", options.join(QLatin1Char('/')));
    }
    qApp->removeTranslator(&translator);

    // 收尾：把沙盒里那一格改回测试钉的 zh_CN，别把后面的模块带到英文。
    QSettings().setValue(QStringLiteral("General/Language"), QStringLiteral("zh_CN"));
    QSettings().sync();
    return 0;
}



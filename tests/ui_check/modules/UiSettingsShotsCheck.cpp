// 设置页验收截图：offscreen 下用 QWidget::grab() 出 深色 / 青瓷 × 最小窗口 / 1280×800 × 4 个页签，
// 另出 stack 态（表单收窄到 560，行上下叠放）和「高级」展开态。不弹真窗口。
// 用法：QT_QPA_PLATFORM=offscreen ui_check_settings_shots  [输出目录，默认 build/settings-shots]
#include "Support.hpp"
#include "MainWindowPageLayout.hpp"
#include "SettingsPageParts.hpp"

#include <QDir>
#include <QFontDatabase>
#include <QMainWindow>
#include <QAbstractItemView>
#include <QComboBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>

int UiSettingsShotsCheck(QApplication &app) {
    Anim::setEnabled(false);
    QTemporaryDir settingsDir(QDir::current().filePath(QStringLiteral(".settings-shots-XXXXXX")));
    if (!settingsDir.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("WTFMLSettingsShots"));
    app.setApplicationName(QStringLiteral("SettingsShots"));
    app.setStyle(Theme::createStyle());
    const QString fontPath = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
    const int fontId = QFile::exists(fontPath) ? QFontDatabase::addApplicationFont(fontPath) : -1;
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    app.setFont(QFont(families.isEmpty() ? QStringLiteral("Microsoft YaHei UI") : families.first(), 9));

    const QString out = app.arguments().size() > 1 ? app.arguments().at(1)
                                                   : QDir::current().filePath(QStringLiteral("build/settings-shots"));
    QDir().mkpath(out);
    int saved = 0;
    for (const QString &id : {QStringLiteral("dark"), QStringLiteral("emerald")}) {
        Theme::setSpec(Theme::presetSpec(id));
        Theme::apply(app);
        QMainWindow window;
        MainWindowUI ui(&window);
        window.show();
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
        QWidget *page = ui.getTabWidget()->widget(MainWindowUI::settingsTabIndex());
        auto *tabs = ui.getSettingsTabs();
        // 下拉项与动态文字平时由 MainWindow 灌；这里灌一份样例，截图才接近真实界面。
        const auto fill = [](QComboBox *combo, const QStringList &items, int current = 0) {
            if (combo == nullptr) return;
            combo->clear();
            combo->addItems(items);
            combo->setCurrentIndex(current);
        };
        fill(ui.getGameWindowModeCombo(), {QStringLiteral("窗口化"), QStringLiteral("最大化"), QStringLiteral("全屏")});
        fill(ui.getLauncherBehaviourCombo(), {QStringLiteral("收起启动器"), QStringLiteral("保持打开"), QStringLiteral("退出启动器")});
        fill(ui.getProcessPriorityCombo(), {QStringLiteral("正常"), QStringLiteral("较高"), QStringLiteral("较低")});
        fill(ui.getDownloadSourceCombo(), {QStringLiteral("优先官方源"), QStringLiteral("优先镜像"), QStringLiteral("仅官方")});
        fill(ui.getDownloadConcurrencyCombo(), {QStringLiteral("8 个"), QStringLiteral("16 个"), QStringLiteral("32 个")});
        fill(ui.getDownloadSpeedLimitCombo(), {QStringLiteral("不限速"), QStringLiteral("10 MB/s"), QStringLiteral("5 MB/s")});
        fill(ui.getDownloadProxyCombo(), {QStringLiteral("跟随系统"), QStringLiteral("直连"), QStringLiteral("手动")});
        if (ui.getJavaPathLabel() != nullptr) ui.getJavaPathLabel()->setText(QStringLiteral("自动选择　按游戏版本"));
        if (ui.getMemoryHint() != nullptr) ui.getMemoryHint()->setText(QStringLiteral("本机 16 GB，当前可用 9.2 GB。"));
        if (ui.getMemoryMaxSpin() != nullptr) ui.getMemoryMaxSpin()->setValue(4096);
        if (ui.getMemoryMaxSlider() != nullptr) ui.getMemoryMaxSlider()->setValueMb(4096);
        if (ui.getGameWindowWidthSpin() != nullptr) ui.getGameWindowWidthSpin()->setValue(1280);
        if (ui.getGameWindowHeightSpin() != nullptr) ui.getGameWindowHeightSpin()->setValue(720);
        if (ui.getShowGameLogCheck() != nullptr) ui.getShowGameLogCheck()->setChecked(true);
        if (ui.getWarnDuplicateLaunchCheck() != nullptr) ui.getWarnDuplicateLaunchCheck()->setChecked(true);
        if (ui.getWarnWorldLossCheck() != nullptr) ui.getWarnWorldLossCheck()->setChecked(true);
        if (ui.getJavaWarnMismatchCheck() != nullptr) ui.getJavaWarnMismatchCheck()->setChecked(true);
        if (ui.getWarnMissingDependenciesCheck() != nullptr) ui.getWarnMissingDependenciesCheck()->setChecked(true);
        if (ui.getDownloadAutoIntoInstanceCheck() != nullptr) ui.getDownloadAutoIntoInstanceCheck()->setChecked(true);
        if (ui.getHomeFavoriteCountSpin() != nullptr) ui.getHomeFavoriteCountSpin()->setValue(3);
        if (ui.getHomeRecentCountSpin() != nullptr) ui.getHomeRecentCountSpin()->setValue(0);
        const QStringList keys = MainWindowUI::settingsCategoryKeys();
        const auto pump = [&app] {
            app.processEvents();
            app.processEvents();
        };
        const auto shot = [&](const QString &name) {
            pump();
            ui.getSettingsSavedToast()->ping();  // 让「已保存」出现在图里（关动效时直接 1.0）
            pump();
            const QString file = QDir(out).filePath(QStringLiteral("settings-%1-%2.png").arg(name, id));
            if (window.grab().save(file)) ++saved;
            else std::printf("FAIL save %s\n", qPrintable(file));
        };
        for (const QSize size : {QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight), QSize(1280, 800)}) {
            window.resize(size);
            const QString sizeText = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
            for (int i = 0; i < keys.size(); ++i) {
                tabs->setCurrentIndex(i);
                shot(QStringLiteral("%1-%2").arg(keys.at(i), sizeText));
                // 滚到底再截一张（看每页底部的「高级」行）。
                if (auto *scroll = qobject_cast<QScrollArea *>(ui.getSettingsCategoryStack()->widget(i))) {
                    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                    shot(QStringLiteral("%1-%2-end").arg(keys.at(i), sizeText));
                    scroll->verticalScrollBar()->setValue(0);
                }
            }
            // 「配色」下拉：闭合态上面已拍；把弹层拉开再拍一张 —— 四字名在最小窗口与
            // 1280×800 下要整条显示。顺带把「字宽 / 控件宽」印出来，图之外还留一个数。
            tabs->setCurrentIndex(keys.indexOf(QStringLiteral("look")));
            if (auto *preset = page->findChild<QComboBox *>(MainWindowUI::themePresetComboName())) {
                int need = 0;
                for (const QString &id : Theme::presetIds())
                    need = qMax(need, preset->fontMetrics().horizontalAdvance(Theme::presetName(id)));
                std::printf("  preset combo %s：控件宽 %d / 最宽的名字 %d px\n",
                            qPrintable(sizeText), preset->width(), need);
                preset->showPopup();
                pump();
                if (QWidget *popup = preset->view() != nullptr ? preset->view()->window() : nullptr) {
                    const QString file = QDir(out).filePath(QStringLiteral("look-preset-popup-%1.png").arg(sizeText));
                    if (popup->grab().save(file)) ++saved;
                    else std::printf("FAIL save %s\n", qPrintable(file));
                }
                preset->hidePopup();
                pump();
            }
            // 「个性化」页展开「高级」，再收回。
            for (auto *section : page->findChildren<SettingsParts::ExpandSection *>()) section->setExpanded(true, false);
            if (auto *scroll = qobject_cast<QScrollArea *>(ui.getSettingsCategoryStack()->widget(keys.indexOf(QStringLiteral("look"))))) {
                pump();
                scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            }
            shot(QStringLiteral("look-advanced-%1").arg(sizeText));
            if (auto *scroll = qobject_cast<QScrollArea *>(ui.getSettingsCategoryStack()->widget(keys.indexOf(QStringLiteral("look")))))
                scroll->verticalScrollBar()->setValue(0);
            for (auto *section : page->findChildren<SettingsParts::ExpandSection *>()) section->setExpanded(false, false);
        }
        // stack 态：表单收窄到 560（内容宽 < 600），行上下叠放。
        window.resize(QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight));
        for (const QString &key : {QStringLiteral("launch"), QStringLiteral("look")}) {
            tabs->setCurrentIndex(keys.indexOf(key));
            for (auto *form : page->findChildren<QWidget *>(MainWindowUI::settingsFormName(key))) form->setMaximumWidth(560);
            shot(QStringLiteral("stack-%1").arg(key));
            for (auto *form : page->findChildren<QWidget *>(MainWindowUI::settingsFormName(key)))
                form->setMaximumWidth(MainWindowUI::settingsFormMaxWidth());
        }
    }
    std::printf("saved %d shots to %s\n", saved, qPrintable(out));
    return saved > 0 ? 0 : 1;
}

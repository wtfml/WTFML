// 输入框宽度检查：内容很短的输入框不许拉满整行，窄窗 / 宽窗都一样。
//
// 规则（ui/InputSizing.hpp）：
//   · 登记为 short 的输入框：宽度 ≤ 按它最多字符数算出来的上限（数字框 ≤ 自己的 sizeHint）。
//   · 登记为 long 的输入框：必须写了理由。
//   · 没登记的输入框：宽度 ≤ InputSizing::kUnregisteredMaxWidth。新加的短值输入框忘了登记，
//     在宽窗里被拉满整行，这一条会把它抓出来。
// 扫描范围：主窗口各页（实例 / 下载 / 设置各分类 / 智能体 / 工具箱每个工具）、账号与外观、
// 实例设置各页、新建服务端、服务端设置、存档设置、智能体设置、预设编辑、实例升级等覆盖层页面，
// 在 1280×800 与最小窗口两种窗口下各扫一遍。
// 设 WTFML_INPUT_WIDTH_DUMP=1 时把每个输入框打印出来（页面 / 类型 / objectName / 宽度 / 登记）。
#include "Support.hpp"
#include "MainWindowPageLayout.hpp"

#include "InputWidthScan.hpp"
#include "InstanceUpgradePage.hpp"
#include "PresetEditorPage.hpp"
#include "AgentSettingsPage.hpp"
#include "ThemeAdvancedEditor.hpp"

#include <QDir>
#include <QAbstractItemView>
#include <QFontDatabase>
#include <QMainWindow>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>

namespace {

struct Counters {
    int surfaces = 0;
    int inputs = 0;
    int shortOnes = 0;
    int longOnes = 0;
    int unregistered = 0;
};

/// 短值输入框的宽度上限：数字框取它自己的 sizeHint，其余取登记时算出来的那一个。
int shortLimit(const InputProbe &p) {
    // 数字框：登记时按字符数钉过宽（同组统一宽）就认那一个，否则取它自己的 sizeHint。
    if (qobject_cast<QAbstractSpinBox *>(p.widget) != nullptr)
        return p.limit > 0 ? p.limit : p.widget->sizeHint().width() + 2;
    return p.limit + 2;
}

// 出样张：WTFML_INPUT_WIDTH_SHOTS=<目录> 给改过的几页各拍一张；再设 WTFML_INPUT_WIDTH_BEFORE=1 先把所有
// 短值登记撤掉（最大宽度还原、数字框策略还原）再拍，得到「修前」，这一趟不做断言。
const QHash<QString, QString> &shotKeys() {
    static const QHash<QString, QString> keys = {
        {QStringLiteral("账号与外观"), QStringLiteral("account-offline")},
        {QStringLiteral("下载·安装页"), QStringLiteral("install-name")},
        {QStringLiteral("新建服务端"), QStringLiteral("new-server")},
        {QStringLiteral("智能体设置"), QStringLiteral("agent-settings")},
        {QStringLiteral("工具箱·server"), QStringLiteral("toolbox-server")},
        {QStringLiteral("实例升级"), QStringLiteral("instance-upgrade")},
        {QStringLiteral("存档设置"), QStringLiteral("world-settings")},
        {QStringLiteral("设置·look"), QStringLiteral("settings-look")},
    };
    return keys;
}

void scanSurface(QWidget *root, const QString &surface, const QString &sizeTag, Counters *counters,
                 QWidget *grabTarget = nullptr) {
    ++counters->surfaces;
    const bool dump = qEnvironmentVariableIsSet("WTFML_INPUT_WIDTH_DUMP");
    const QString shotDir = qEnvironmentVariable("WTFML_INPUT_WIDTH_SHOTS");
    const bool before = qEnvironmentVariableIsSet("WTFML_INPUT_WIDTH_BEFORE");
    if (!shotDir.isEmpty() && shotKeys().contains(surface)) {
        if (before) {
            for (const InputProbe &p : scanInputs(root)) {
                if (p.kind != QLatin1String("short")) continue;
                p.widget->setMaximumWidth(QWIDGETSIZE_MAX);
                if (qobject_cast<QAbstractSpinBox *>(p.widget) != nullptr) {
                    QSizePolicy policy = p.widget->sizePolicy();
                    policy.setHorizontalPolicy(QSizePolicy::Minimum);
                    p.widget->setSizePolicy(policy);
                }
            }
        }
        for (int i = 0; i < 4; ++i) QApplication::processEvents();
        QDir().mkpath(shotDir);
        const QString name = QStringLiteral("%1-%2-%3.png")
                                 .arg(shotKeys().value(surface), QString(sizeTag).replace(QLatin1Char(' '), QLatin1Char('-')),
                                      before ? QStringLiteral("before") : QStringLiteral("after"));
        (grabTarget != nullptr ? grabTarget : root)->grab().save(QDir(shotDir).filePath(name));
    }
    if (before) return;
    for (const InputProbe &p : scanInputs(root)) {
        ++counters->inputs;
        const QString who = QStringLiteral("[%1 %2] %3 %4").arg(sizeTag, surface, p.cls, p.name.isEmpty() ? p.hint : p.name);
        if (dump)
            std::printf("  INPUT %s w=%d max=%d kind=%s chars=%d\n", qPrintable(who), p.width, p.maxWidth,
                        qPrintable(p.kind), p.limit);
        if (p.kind == QLatin1String("short")) {
            ++counters->shortOnes;
            check(p.width <= shortLimit(p), QStringLiteral("短值输入框不拉满：%1 宽 %2 ≤ %3").arg(who).arg(p.width).arg(shortLimit(p)));
            if (!qobject_cast<QAbstractSpinBox *>(p.widget))
                check(p.limit > 0 && p.limit <= InputSizing::kShortMaxWidth,
                      QStringLiteral("短值上限不超过 %1：%2 登记了 %3").arg(InputSizing::kShortMaxWidth).arg(who).arg(p.limit));
        } else if (p.kind == QLatin1String("long")) {
            ++counters->longOnes;
            check(!p.reason.isEmpty(), QStringLiteral("长值输入框写了理由：%1").arg(who));
        } else {
            ++counters->unregistered;
            check(p.width <= InputSizing::kUnregisteredMaxWidth,
                  QStringLiteral("未登记的输入框不超过 %1：%2 宽 %3（短值用 InputSizing::shortValue，长值用 longValue 写理由）")
                      .arg(InputSizing::kUnregisteredMaxWidth)
                      .arg(who)
                      .arg(p.width));
        }
    }
}

/// 把 w 所在的各层 QStackedWidget 翻到能看见它的那一页（生产里是用户点出来的）。
void reveal(QWidget *w) {
    for (QWidget *child = w; child != nullptr && child->parentWidget() != nullptr; child = child->parentWidget()) {
        if (auto *stack = qobject_cast<QStackedWidget *>(child->parentWidget())) stack->setCurrentWidget(child);
    }
    w->show();
}

}  // namespace

int UiInputWidthCheck(QApplication &app) {
    Anim::setEnabled(false);
    QTemporaryDir settingsDir(QDir::current().filePath(QStringLiteral(".input-width-XXXXXX")));
    if (!settingsDir.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("WTFMLInputWidth"));
    app.setApplicationName(QStringLiteral("InputWidth"));
    app.setStyle(Theme::createStyle());
    const QString fontPath = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
    const int fontId = QFile::exists(fontPath) ? QFontDatabase::addApplicationFont(fontPath) : -1;
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    app.setFont(QFont(families.isEmpty() ? QStringLiteral("Microsoft YaHei UI") : families.first(), 9));
    const auto pump = [&app] {
        for (int i = 0; i < 3; ++i) app.processEvents();
    };

    Counters counters;
    const QStringList themes = {QStringLiteral("dark"), QStringLiteral("emerald")};
    for (const QString &themeId : themes) {
        Theme::setSpec(Theme::presetSpec(themeId));
        Theme::apply(app);
        for (const QSize size : {QSize(1280, 800), QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight)}) {
            const QString tag = QStringLiteral("%1 %2x%3").arg(themeId).arg(size.width()).arg(size.height());
            QMainWindow window;
            MainWindowUI ui(&window);
            window.resize(size);
            window.show();
            pump();
            const auto scanMain = [&](const QString &surface) {
                pump();
                scanSurface(&window, surface, tag, &counters, &window);
            };

            // ---- 主窗口各页 ----
            ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
            scanMain(QStringLiteral("实例"));
            ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
            for (int row : {MainWindowUI::downloadCategoryGameIndex(), MainWindowUI::downloadCategoryModIndex()}) {
                ui.getDownloadCategoryList()->setCurrentRow(row);
                ui.setDownloadHeaderRow(row);
                ui.getDownloadStack()->setCurrentIndex(row);
                scanMain(QStringLiteral("下载 %1").arg(row));
            }
            if (QLineEdit *installName = ui.getInstallNameEdit()) {
                reveal(installName);
                scanMain(QStringLiteral("下载·安装页"));
            }
            ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
            const QStringList keys = MainWindowUI::settingsCategoryKeys();
            for (int i = 0; i < keys.size(); ++i) {
                ui.getSettingsTabs()->setCurrentIndex(i);
                scanMain(QStringLiteral("设置·%1").arg(keys.at(i)));
            }
            ui.getTabWidget()->setCurrentIndex(MainWindowUI::agentTabIndex());
            scanMain(QStringLiteral("智能体"));

            ui.openToolboxTab();
            if (QWidget *host = ui.toolboxTab(); host != nullptr && host->layout() != nullptr) {
                auto *toolbox = new ToolboxPage(host);
                host->layout()->addWidget(toolbox);
                for (ToolboxPage::Tool tool : ToolboxPage::tools()) {
                    toolbox->showCatalog();
                    toolbox->showTool(tool);
                    scanMain(QStringLiteral("工具箱·%1").arg(ToolboxPage::toolKey(tool)));
                    if (ToolboxPage::toolKey(tool) == QLatin1String("server")) {
                        // 地址框是短值：「检测」按钮紧跟在框后面，不被拉成一长条。
                        auto *edit = toolbox->findChild<QLineEdit *>(QStringLiteral("serverAddress"));
                        auto *go = toolbox->findChild<QPushButton *>(QStringLiteral("serverCheck"));
                        check(edit != nullptr && go != nullptr, "延迟测试：地址框与「检测」都在", tag);
                        if (edit != nullptr && go != nullptr) {
                            const int gap = go->mapTo(&window, QPoint(0, 0)).x() -
                                            (edit->mapTo(&window, QPoint(0, 0)).x() + edit->width());
                            check(go->width() <= 160, "「检测」按钮不被拉宽", QStringLiteral("%1 宽 %2").arg(tag).arg(go->width()));
                            check(gap >= 0 && gap <= 16, "「检测」紧跟在地址框右边", QStringLiteral("%1 间距 %2").arg(tag).arg(gap));
                        }
                    }
                }
            }

            // ---- 覆盖层里的页（QDialog 一族） ----
            const auto inOverlay = [&](QDialog *page, const QString &title, const QString &surface,
                                       const std::function<void()> &prepare = {},
                                       const std::function<void()> &after = {}) {
                ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
                pump();
                if (!ui.overlay()->showPage(page, title, MainWindowUI::instancesTabIndex())) return;
                for (int i = 0; i < 8; ++i) pump();
                if (prepare) prepare();
                for (int i = 0; i < 8; ++i) pump();
                if (!page->isVisible()) std::printf("  SKIP %s：页没显示出来\n", qPrintable(surface));
                scanSurface(page, surface, tag, &counters, &window);
                if (after) after();
                ui.overlay()->closeOverlay();
                pump();
            };

            {
                AccountSkinDialog account;
                inOverlay(
                    &account, AccountSkinDialog::windowTitle(), QStringLiteral("账号与外观"),
                    [&account] { account.showOfflineAddRow(); },  // 展开离线输入行，名字框才露出来
                    [&account, &tag] {
                        // 用户原话：离线账号名字最多 16 个字符，框不该横跨整个内容区；「添加」紧跟在框后面。
                        auto *edit = account.findChild<QLineEdit *>(AccountSkinDialog::offlineNameEditName());
                        auto *add = account.findChild<QPushButton *>(AccountSkinDialog::addOfflineButtonName());
                        check(edit != nullptr && add != nullptr && edit->isVisible(), "离线名字框与「添加」都显示着", tag);
                        if (edit == nullptr || add == nullptr) return;
                        check(edit->property(InputSizing::kKindProp).toString() == QLatin1String("short"),
                              "离线账号名字框登记为短值", tag);
                        check(edit->width() <= 260, "离线名字框不超过 260 px", QStringLiteral("%1 %2").arg(tag).arg(edit->width()));
                        check(edit->width() >= 200, "离线名字框装得下 16 个字符", QStringLiteral("%1 %2").arg(tag).arg(edit->width()));
                        const int gap = add->mapTo(&account, QPoint(0, 0)).x() -
                                        (edit->mapTo(&account, QPoint(0, 0)).x() + edit->width());
                        check(gap >= 0 && gap <= 16, "「添加」紧跟在名字框右边", QStringLiteral("%1 间距 %2").arg(tag).arg(gap));
                    });
            }
            {
                NewServerPage page;
                inOverlay(&page, QStringLiteral("新建服务端"), QStringLiteral("新建服务端"));
            }
            {
                AgentSettingsPage page;
                inOverlay(&page, QStringLiteral("API 与模型设置"), QStringLiteral("智能体设置"));
            }
            {
                PresetEditorPage page(QJsonObject{{QStringLiteral("name"), QStringLiteral("示例预设")},
                                                  {QStringLiteral("icon"), QStringLiteral("icon.png")}});
                inOverlay(&page, QStringLiteral("编辑预设"), QStringLiteral("预设编辑"));
            }
            {
                ServerConsolePage page;
                inOverlay(&page, QStringLiteral("控制台"), QStringLiteral("服务端控制台"));
            }
            {
                ServerSettingsPage page;
                QList<ServerSettingsPage::FieldView> fields;
                ServerSettingsPage::FieldView name;
                name.key = QStringLiteral("level-name");
                name.label = QStringLiteral("世界名");
                fields.append(name);
                ServerSettingsPage::FieldView port;
                port.key = QStringLiteral("server-port");
                port.label = QStringLiteral("端口");
                port.kind = ServerSettingsPage::Kind::Number;
                port.min = 1;
                port.max = 65535;
                fields.append(port);
                page.setFields(fields);
                inOverlay(&page, QStringLiteral("服务端设置"), QStringLiteral("服务端设置"));
            }
            {
                InstanceSettingsDialog::GlobalValues global;
                global.javaPath = QStringLiteral("C:/Program Files/Java/jdk-21/bin/java.exe");
                global.xmx = 4096;
                global.xms = 1024;
                global.autoMemory = false;
                InstanceSettingsDialog dialog(QStringLiteral("1.20.1"), QStringLiteral("1.20.1-Forge"),
                                              InstanceSettingsDialog::Override(), global, QStringList(), false);
                inOverlay(&dialog, QStringLiteral("实例设置"), QStringLiteral("实例设置·概览"), [&dialog] {
                    dialog.showPage(InstanceSettingsDialog::overviewCategoryKey());
                });
                inOverlay(&dialog, QStringLiteral("实例设置"), QStringLiteral("实例设置·启动"), [&dialog] {
                    dialog.showPage(InstanceSettingsDialog::launchCategoryKey());
                });
            }

            // ---- 单独的 QWidget 页：放进一个与内容区同宽的容器 ----
            const int contentW = size.width() - 120;
            const auto standalone = [&](QWidget *page, const QString &surface,
                                        const std::function<void()> &after = {}) {
                QWidget host;
                auto *layout = new QVBoxLayout(&host);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->addWidget(page);
                host.resize(contentW, size.height() - 100);
                host.show();
                pump();
                scanSurface(&host, surface, tag, &counters, &host);
                if (after) after();
            };
            {
                auto *page = new InstanceUpgradePage;
                standalone(page, QStringLiteral("实例升级"));
            }
            {
                auto *page = new WorldSettingsPage;
                WorldSettingsPage::WorldView view;
                view.dirName = QStringLiteral("New World");
                view.hasLevelName = true;
                view.levelName = QStringLiteral("New World");
                view.hasRules = true;
                view.rules = {{QStringLiteral("randomTickSpeed"), QStringLiteral("3"), WorldSettingsPage::Kind::Int, true},
                              {QStringLiteral("commandBlockOutput"), QStringLiteral("true"), WorldSettingsPage::Kind::Bool, true}};
                page->setWorld(view);
                page->setFileEditingOpen(true);
                standalone(page, QStringLiteral("存档设置"), [&] {
                    const auto *rename = page->findChild<QPushButton *>(WorldSettingsPage::renameButtonName());
                    const auto *openFolder = page->findChild<QPushButton *>(WorldSettingsPage::openFolderButtonName());
                    const auto *backupFolder = page->findChild<QPushButton *>(WorldSettingsPage::backupFolderButtonName());
                    check(rename && openFolder && backupFolder && rename->y() == openFolder->y() &&
                              openFolder->y() == backupFolder->y() &&
                              backupFolder->geometry().right() < page->width(),
                          "存档设置：操作按钮同一排且不越界", tag);
                    auto *difficulty = page->findChild<QComboBox *>(WorldSettingsPage::difficultyName());
                    check(difficulty && difficulty->count() == 4, "存档设置：难度有四个选项", tag);
                    if (!difficulty) return;
                    difficulty->showPopup();
                    pump();
                    QAbstractItemView *popup = difficulty->view();
                    const QFontMetrics metrics(Theme::textFont(popup->font(), Theme::TextRole::Body));
                    for (int i = 0; i < difficulty->count(); ++i) {
                        const QString label = difficulty->itemText(i);
                        const int rowWidth = popup->visualRect(difficulty->model()->index(i, difficulty->modelColumn())).width();
                        check(popup->isVisible() && rowWidth >= metrics.horizontalAdvance(label) + 24,
                              "存档设置：难度选项容纳完整文字与两侧留白",
                              QStringLiteral("%1 %2（行宽 %3）").arg(tag, label).arg(rowWidth));
                    }
                    difficulty->hidePopup();
                    pump();
                });
            }
            {
                auto *page = new ThemeAdvancedEditor;
                if (auto *toggle = page->findChild<QAbstractButton *>(QStringLiteral("themeAdvancedToggle")))
                    if (toggle->isCheckable() && !toggle->isChecked()) toggle->click();
                standalone(page, QStringLiteral("主题高级编辑"));
            }
        }
    }

    std::printf("input-width: %d surfaces, %d inputs (short %d / long %d / unregistered %d)\n", counters.surfaces,
                counters.inputs, counters.shortOnes, counters.longOnes, counters.unregistered);
    if (!qEnvironmentVariableIsSet("WTFML_INPUT_WIDTH_BEFORE"))
        check(counters.surfaces >= 60 && counters.inputs >= 200, "扫描覆盖面没有缩水",
              QStringLiteral("%1 页 / %2 个输入框").arg(counters.surfaces).arg(counters.inputs));
    return 0;
}

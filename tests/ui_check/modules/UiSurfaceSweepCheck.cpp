// 子页面 / 弹框 / 浮层的「漏接设计系统」巡检：把每个界面表面真的打开，扫一遍控件树。
//
// 为什么要有这一层：设计系统是逐个控件「选择加入」的 —— 列表要挂自家代理、换页要调
// `Anim::pageAppear()`、开关要 `Theme::markSwitch()`。直接 new 原生控件的地方就落回
// Qt 默认外观，而且已有巡检（input-widths / review-shots）的页面清单是手写的，由按钮
// 再弹出来的提示卡、对话框、向导步骤、浮层大多不在清单里。
//
// 本模块是守卫：扫出「计入的残留」就判失败（退出码 1），并在输出里点名表面、控件、类别。
// 例外只有 `SurfaceScan.hpp` 的 `checkBoxException()` 一处，每条带一句理由。
//   · 每个表面拍一张 900×620、出厂默认主题（暮色雪山 dark）的样张；
//   · 扫出「还长着 Qt 默认样子」的控件，按类别计数；
//   · 设 WTFML_SURFACE_SHOTS=<目录> 时写样张与 surfaces.tsv / findings.tsv。
//   · 设 WTFML_SURFACE_THEME=emerald|ocean 时换主题（默认出厂暮色雪山）。
//
// 表面清单（哪个入口算什么键）在 `tests/ui_check/surface-registry.txt`，由
// `tools/check-surface-registry.ps1` 静态核对新入口有没有登记。
//
// 采样口径见 `SurfaceScan.hpp`。
#include "Support.hpp"
#include "UiLanguage.hpp"
#include "SurfaceScan.hpp"

#include "AccountSkinDialog.hpp"
#include "ActivityPopover.hpp"
#include "AgentMcpPage.hpp"
#include "AgentSettingsPage.hpp"
#include "ChromeDialog.hpp"
#include "ConfigFileEditorPage.hpp"
#include "GlassPopover.hpp"
#include "GroupDialog.hpp"
#include "GameLogWindow.hpp"
#include "InlineMessage.hpp"
#include "InstanceSettingsDialog.hpp"
#include "InstanceUpdateWizard.hpp"
#include "InstanceUpgradePage.hpp"
#include "IntegrityReportPage.hpp"
#include "JavaManageDialog.hpp"
#include "JavaMismatchDialog.hpp"
#include "NewServerPage.hpp"
#include "PresetEditorPage.hpp"
#include "PresetInstallPicker.hpp"
#include "PresetManagePage.hpp"
#include "PresetModEditor.hpp"
#include "PresetModReview.hpp"
#include "PresetOptionsEditor.hpp"
#include "ResourceDetailPage.hpp"
#include "ResourcePackPage.hpp"
#include "SchematicInfoDialog.hpp"
#include "ServerConsolePage.hpp"
#include "ServerSettingsPage.hpp"
#include "TaskDetailPage.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "ToolboxPage.hpp"
#include "UpdateCenterPage.hpp"
#include "UpgradeHistoryDialog.hpp"
#include "UpgradeReportDialog.hpp"
#include "WorldSettingsPage.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTranslator>
#include <QStackedWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>

namespace {

constexpr QSize kSurfaceSize(900, 620);

struct SurfaceRecord {
    QString key;
    QString name;
    QString entry;
    QString source;
    QString covered;  ///< 已在哪个巡检清单里（空 = 没人看过）
    QString shotFile;
    bool shotOk = false;
    QList<SurfaceFinding> findings;
    /// 强制画一遍这一整块（grab）的耗时。这是 offscreen `ui_check`（-O0）里的绘制成本，
    /// 不是产品 Release 的首帧；只用来在 95 个表面之间横向比，超过 50 ms 的进 B 类候选。
    double paintMs = -1.0;
    /// 调用方从"打开这一页 / 这一个弹框"开始计的耗时（不传 = 没量）。同样只做横向比。
    double openMs = -1.0;
};

class Sweep {
public:
    Sweep(QApplication &app, QMainWindow &window, MainWindowUI &ui, const QString &shotDir)
        : m_app(app), m_window(window), m_ui(ui), m_shotDir(shotDir) {}

    const QList<SurfaceRecord> &records() const { return m_records; }

    void pump() {
        for (int i = 0; i < 4; ++i) m_app.processEvents();
    }

    /// 扫一个表面：控件树 + 换页动效；顺手拍一张。
    /// @param openMs 调用方从"打开"到"这一页摆好"的耗时（不传 = 没量）。
    void record(const QString &key, const QString &name, const QString &entry, const QString &source,
                const QString &covered, QWidget *root, QWidget *shotTarget = nullptr,
                double openMs = -1.0) {
        SurfaceRecord rec;
        rec.key = key;
        rec.name = name;
        rec.entry = entry;
        rec.source = source;
        rec.covered = covered;
        rec.openMs = openMs;
        if (root == nullptr) {
            std::printf("== %-38s SKIP：表面没建出来\n", qPrintable(key));
            m_records.append(rec);
            return;
        }
        pump();
        // 首帧：强制把这一整块画出来（离屏）。量的是控件树 + 代理 + 图标的绘制成本。
        {
            QElapsedTimer paintClock;
            paintClock.start();
            const QPixmap warm = root->grab();
            rec.paintMs = double(paintClock.nsecsElapsed()) / 1.0e6;
            (void)warm;
        }
        rec.findings = SurfaceScan::scanWidgets(root);
        rec.findings += SurfaceScan::probePageTransitions(root, m_app);
        pump();
        rec.shotOk = capture(rec, shotTarget != nullptr ? shotTarget : root);
        m_records.append(rec);
        std::printf("== %-38s %-3d 处  paint %6.1f ms%s  %s\n", qPrintable(key),
                    int(rec.findings.size()), rec.paintMs,
                    rec.openMs >= 0 ? qPrintable(QStringLiteral("  open %1 ms").arg(rec.openMs, 0, 'f', 1)) : "",
                    rec.shotOk ? qPrintable(rec.shotFile) : "无样张");
        for (const SurfaceFinding &finding : rec.findings) {
            std::printf("     - %-18s %-20s %-28s %s\n", qPrintable(finding.kind), qPrintable(finding.cls),
                        qPrintable(finding.name), qPrintable(finding.detail));
        }
    }

    void writeReports() {
        if (m_shotDir.isEmpty()) return;
        QFile surfaces(QDir(m_shotDir).filePath(QStringLiteral("surfaces.tsv")));
        if (surfaces.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&surfaces);
            out.setEncoding(QStringConverter::Utf8);
            out << "key\tname\tentry\tsource\tcovered\tshot\tfindings\tkinds\tpaintMs\topenMs\n";
            for (const SurfaceRecord &rec : m_records) {
                out << rec.key << '\t' << rec.name << '\t' << rec.entry << '\t' << rec.source << '\t'
                    << rec.covered << '\t' << (rec.shotOk ? rec.shotFile : QString()) << '\t'
                    << rec.findings.size() << '\t' << SurfaceScan::findingSummary(rec.findings) << '\t'
                    << (rec.paintMs >= 0 ? QString::number(rec.paintMs, 'f', 1) : QString()) << '\t'
                    << (rec.openMs >= 0 ? QString::number(rec.openMs, 'f', 1) : QString()) << '\n';
            }
        }
        QFile findings(QDir(m_shotDir).filePath(QStringLiteral("findings.tsv")));
        if (findings.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&findings);
            out.setEncoding(QStringConverter::Utf8);
            out << "surface\tkind\tclass\twidget\tdetail\n";
            for (const SurfaceRecord &rec : m_records) {
                for (const SurfaceFinding &finding : rec.findings) {
                    out << rec.key << '\t' << finding.kind << '\t' << finding.cls << '\t' << finding.name
                        << '\t' << finding.detail << '\n';
                }
            }
        }
    }

private:
    /// 主窗口 + 此刻开着的顶层浮层叠成一张（浮层是 Qt::Popup，不在窗口那张图里）。
    bool capture(SurfaceRecord &rec, QWidget *target) {
        if (m_shotDir.isEmpty()) return false;
        QDir().mkpath(m_shotDir);
        QPixmap shot = target->grab();
        if (target == &m_window) {
            QPainter painter(&shot);
            for (QWidget *top : QApplication::topLevelWidgets()) {
                const Qt::WindowType type = top->windowType();
                if ((type != Qt::Popup && type != Qt::ToolTip) || !top->isVisible()) continue;
                const QRect at(m_window.mapFromGlobal(top->mapToGlobal(QPoint(0, 0))), top->size());
                painter.drawPixmap(at.topLeft(), top->grab());
            }
        }
        const QString file = QDir(m_shotDir).filePath(rec.key + QStringLiteral(".png"));
        rec.shotFile = file;
        return shot.save(file);
    }

    QApplication &m_app;
    QMainWindow &m_window;
    MainWindowUI &m_ui;
    QString m_shotDir;
    QList<SurfaceRecord> m_records;
};

/// 一个页面类没有自己的窗口外壳时，套一层空 QDialog 塞进覆盖层（和产品里那些「整页」同一条路）。
QDialog *embedPage(QWidget *page, const QString &objectName) {
    auto *dialog = new QDialog;
    dialog->setObjectName(objectName);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(page);
    return dialog;
}

void fillHome(MainWindowUI &ui);
void fillInstanceTree(MainWindowUI &ui);

}  // namespace

int UiSurfaceSweepCheck(QApplication &app) {
    Anim::setEnabled(false);
    // WTFML_SURFACE_LANGUAGE=en：英文样本巡检（样张与中文一一对应）。
    QTranslator surfaceTranslator;
    if (qEnvironmentVariable("WTFML_SURFACE_LANGUAGE") == QLatin1String(UiLanguage::kEnglish) &&
        (!UiLanguage::loadEnglish(&surfaceTranslator) || !app.installTranslator(&surfaceTranslator))) {
        qCritical("WTFML_SURFACE_LANGUAGE=en requires an English translation resource");
        return 2;
    }
    QTemporaryDir settingsDir(QDir::current().filePath(QStringLiteral(".surface-sweep-XXXXXX")));
    if (!settingsDir.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("WTFMLSurfaceSweep"));
    app.setApplicationName(QStringLiteral("SurfaceSweep"));
    app.setStyle(Theme::createStyle());
    const QString fontPath = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
    const int fontId = QFile::exists(fontPath) ? QFontDatabase::addApplicationFont(fontPath) : -1;
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    app.setFont(QFont(families.isEmpty() ? QStringLiteral("Microsoft YaHei UI") : families.first(), 9));

    // 出厂默认主题：设置是刚建的空沙盒，Theme::load() 拿到的就是暮色雪山（dark）。
    // WTFML_SURFACE_THEME=emerald / ocean 换成别的预设（只为出三主题样张，判据不随主题变）。
    const QString themeId = qEnvironmentVariable("WTFML_SURFACE_THEME");
    Theme::setSpec(Theme::isPresetId(themeId) ? Theme::presetSpec(themeId) : Theme::load());
    Theme::apply(app);

    const QString shotDir = qEnvironmentVariable("WTFML_SURFACE_SHOTS");
    if (!shotDir.isEmpty()) QDir().mkpath(shotDir);

    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(kSurfaceSize);
    window.show();
    for (int i = 0; i < 6; ++i) app.processEvents();

    Sweep sweep(app, window, ui, shotDir);
    const auto pump = [&sweep] { sweep.pump(); };

    // 覆盖层整页：开 → 摆 → 扫 → 收。
    const auto overlayDialog = [&](const QString &key, const QString &name, const QString &entry,
                                   const QString &source, const QString &covered, QDialog *page,
                                   const QString &title, const std::function<void()> &prepare = {},
                                   QWidget *shotTarget = nullptr) {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
        pump();
        QElapsedTimer openClock;
        openClock.start();
        if (!ui.overlay()->showPage(page, title, MainWindowUI::instancesTabIndex())) {
            std::printf("  SKIP %s：覆盖层没开出来（%s）\n", qPrintable(key), qPrintable(title));
            sweep.record(key, name, entry, source, covered, nullptr);
            return;
        }
        for (int i = 0; i < 4; ++i) pump();
        if (prepare) prepare();
        for (int i = 0; i < 4; ++i) pump();
        const double openMs = double(openClock.nsecsElapsed()) / 1.0e6;
        sweep.record(key, name, entry, source, covered, page, shotTarget != nullptr ? shotTarget : &window,
                     openMs);
        ui.overlay()->closeOverlay();
        pump();
    };

    // 提示卡（InlineMessageBox）：走真正的 execPrompt（居中卡片 + 背景压暗），
    // 开在嵌套事件循环里，用一发 0ms 定时器拍照、扫完自己 reject。
    const auto promptSurface = [&](const QString &key, const QString &name, const QString &entry,
                                   const QString &source, const QString &covered, const QString &title,
                                   const std::function<void(InlineMessageBox &)> &fill, MessageKind kind) {
        InlineMessageBox box(kind, title, QString(), {QStringLiteral("确定"), QStringLiteral("取消")}, 0, -1,
                             &window);
        fill(box);
        QElapsedTimer openClock;
        openClock.start();
        QTimer::singleShot(0, &app, [&]() {
            pump();
            const double openMs = double(openClock.nsecsElapsed()) / 1.0e6;
            sweep.record(key, name, entry, source, covered, &box, &window, openMs);
            box.reject();
        });
        ui.overlay()->execPrompt(&box, false);
        pump();
    };

    // ---------------------------------------------------------------- 主窗口各页
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    fillHome(ui);
    pump();
    sweep.record(QStringLiteral("home"), QStringLiteral("主页"), QStringLiteral("主窗口 → 左侧导航「主页」"),
                 QStringLiteral("ui/MainWindowUI.cpp"), QStringLiteral("ui-review-shots"),
                 ui.getTabWidget()->widget(MainWindowUI::homeTabIndex()), &window);

    ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
    fillInstanceTree(ui);
    pump();
    sweep.record(QStringLiteral("instances"), QStringLiteral("实例管理"),
                 QStringLiteral("主窗口 → 左侧导航「实例管理」"), QStringLiteral("ui/MainWindowUI.cpp"),
                 QStringLiteral("ui-review-shots"), ui.getTabWidget()->widget(MainWindowUI::instancesTabIndex()),
                 &window);

    ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
    for (int row : {MainWindowUI::downloadCategoryGameIndex(), MainWindowUI::downloadCategoryModIndex(),
                    MainWindowUI::downloadCategoryModpackIndex()}) {
        ui.getDownloadCategoryList()->setCurrentRow(row);
        ui.setDownloadHeaderRow(row);
        ui.getDownloadStack()->setCurrentIndex(row);
        pump();
        sweep.record(QStringLiteral("download-%1").arg(MainWindowUI::downloadCategoryKeys().value(row)),
                     QStringLiteral("下载·%1").arg(MainWindowUI::downloadCategoryTexts().value(row)),
                     QStringLiteral("主窗口 → 左侧导航「下载」→ 分类第 %1 项").arg(row + 1),
                     QStringLiteral("ui/MainWindowDownloadPage.cpp"),
                     row == MainWindowUI::downloadCategoryGameIndex() ? QStringLiteral("ui-review-shots / input-widths")
                     : row == MainWindowUI::downloadCategoryModIndex()  ? QStringLiteral("ui-review-shots")
                                                                        : QString(),
                     ui.getTabWidget()->widget(MainWindowUI::downloadTabIndex()), &window);
    }
    {
        ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryGameIndex());
        ui.setDownloadHeaderRow(MainWindowUI::downloadCategoryGameIndex());
        ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryGameIndex());
        ui.getGameStack()->setCurrentIndex(MainWindowUI::downloadGameInstallIndex());
        pump();
        sweep.record(QStringLiteral("download-install"), QStringLiteral("下载·安装页"),
                     QStringLiteral("主窗口 → 下载 → 游戏 → 选中一个版本后的安装表单"),
                     QStringLiteral("ui/MainWindowDownloadPage.cpp"), QStringLiteral("input-widths"),
                     ui.getTabWidget()->widget(MainWindowUI::downloadTabIndex()), &window);
        ui.getGameStack()->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
        pump();
    }

    ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
    const QStringList settingsKeys = MainWindowUI::settingsCategoryKeys();
    for (int i = 0; i < settingsKeys.size(); ++i) {
        ui.getSettingsTabs()->setCurrentIndex(i);
        pump();
        sweep.record(QStringLiteral("settings-%1").arg(settingsKeys.at(i)),
                     QStringLiteral("设置·%1").arg(settingsKeys.at(i)),
                     QStringLiteral("主窗口 → 左侧导航「设置」→ 第 %1 个分类").arg(i + 1),
                     QStringLiteral("ui/MainWindowSettingsPage.cpp"),
                     QStringLiteral("ui-review-shots / input-widths"),
                     ui.getTabWidget()->widget(MainWindowUI::settingsTabIndex()), &window);
    }

    ui.getTabWidget()->setCurrentIndex(MainWindowUI::agentTabIndex());
    pump();
    sweep.record(QStringLiteral("agent"), QStringLiteral("智能体"), QStringLiteral("主窗口 → 左侧导航「智能体」"),
                 QStringLiteral("ui/AgentPage.cpp"), QStringLiteral("ui-review-shots"),
                 ui.getTabWidget()->widget(MainWindowUI::agentTabIndex()), &window);

    ui.openToolboxTab();
    if (QWidget *host = ui.toolboxTab(); host != nullptr && host->layout() != nullptr) {
        auto *toolbox = new ToolboxPage(host);
        host->layout()->addWidget(toolbox);
        pump();
        toolbox->showCatalog();
        pump();
        sweep.record(QStringLiteral("toolbox-catalog"), QStringLiteral("工具箱·目录"),
                     QStringLiteral("主窗口 → 左侧导航「工具箱」"), QStringLiteral("ui/ToolboxPage.cpp"),
                     QStringLiteral("ui-review-shots / input-widths"), host, &window);
        for (ToolboxPage::Tool tool : ToolboxPage::tools()) {
            toolbox->showTool(tool);
            pump();
            sweep.record(QStringLiteral("toolbox-%1").arg(ToolboxPage::toolKey(tool)),
                         QStringLiteral("工具箱·%1").arg(ToolboxPage::toolName(tool)),
                         QStringLiteral("主窗口 → 工具箱 → 目录里的「%1」").arg(ToolboxPage::toolName(tool)),
                         QStringLiteral("ui/ToolboxPage.cpp"), QStringLiteral("toolbox-layout / input-widths"), host,
                         &window);
        }
        // 面包屑上的工具名点开的切换浮层。
        toolbox->showTool(ToolboxPage::Tool::Anvil);
        pump();
        if (auto *crumb = toolbox->findChild<QPushButton *>(ToolboxPage::crumbToolName())) {
            crumb->click();
            pump();
            sweep.record(QStringLiteral("toolbox-switch-popover"), QStringLiteral("工具箱·工具切换浮层"),
                         QStringLiteral("主窗口 → 工具箱 → 任一工具 → 面包屑上的工具名"),
                         QStringLiteral("ui/ToolboxPage.cpp"), QString(),
                         window.findChild<QWidget *>(ToolboxPage::switchPopoverName()), &window);
            GlassPopover::closeAllIn(&window);
            // 兜底：离屏下有的浮层 closeAllIn 收不掉，留着会叠进后面每一张样张。
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->windowType() == Qt::Popup && top->isVisible()) top->close();
            pump();
        }
        toolbox->showCatalog();
    }

    // 主页实例切换浮层（GlassPopover 那一族）。
    {
        QList<MainWindowUI::HomePickerFolder> folders;
        MainWindowUI::HomePickerFolder folder;
        folder.path = QStringLiteral("D:/Games/.minecraft");
        folder.title = QStringLiteral("官方启动器");
        folder.subtitle = folder.path;
        folder.current = true;
        folders.append(folder);
        MainWindowUI::HomePickerFolder modded;
        modded.path = QStringLiteral("E:/MC/modpacks");
        modded.title = QStringLiteral("整合包");
        modded.subtitle = modded.path;
        folders.append(modded);
        QList<MainWindowUI::HomePickerInstance> instances;
        MainWindowUI::HomePickerInstance one;
        one.folderPath = folder.path;
        one.id = QStringLiteral("1.20.1-Forge");
        one.title = QStringLiteral("1.20.1-Forge 47.2.0");
        one.subtitle = QStringLiteral("上次启动 2 天前");
        one.loaderKey = QStringLiteral("forge");
        one.current = true;
        one.recentRank = 0;
        instances.append(one);
        MainWindowUI::HomePickerInstance two;
        two.folderPath = modded.path;
        two.id = QStringLiteral("1.21.4-Fabric");
        two.title = QStringLiteral("1.21.4-Fabric 0.16.9");
        two.subtitle = QStringLiteral("上次启动 5 小时前");
        two.loaderKey = QStringLiteral("fabric");
        two.recentRank = 1;
        instances.append(two);
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
        pump();
        ui.openHomeInstancePicker(folders, instances, [](const QString &) {}, [](const QString &, const QString &) {});
        pump();
        sweep.record(QStringLiteral("home-instance-picker"), QStringLiteral("主页·实例切换浮层"),
                     QStringLiteral("主窗口 → 主页 → 启动区右侧「切换实例」"),
                     QStringLiteral("ui/MainWindowHomePopovers.cpp"), QString(),
                     window.findChild<QWidget *>(MainWindowUI::homeInstancePickerName()), &window);
        ui.closeHomeInstancePicker();
        GlassPopover::closeAllIn(&window);
        pump();
    }

    // 主页右上账号胶囊弹出的账号浮层。
    {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
        pump();
        ui.toggleAccountsPanel();
        pump();
        QWidget *popover = window.findChild<QWidget *>(MainWindowUI::accountPopoverName());
        sweep.record(QStringLiteral("account-popover"), QStringLiteral("主页·账号浮层"),
                     QStringLiteral("主窗口 → 主页右上账号胶囊"),
                     QStringLiteral("ui/MainWindowHomePopovers.cpp"), QString(), popover, &window);
        ui.toggleAccountsPanel();
        pump();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // 底栏中间那格：任务中心浮层 →「全部任务」→ 任务中心整页。
    {
        QList<TaskCenter::Item> items;
        TaskCenter::Item running;
        running.key = QStringLiteral("42");
        running.title = QStringLiteral("下载 1.21.4-Fabric");
        running.detail = QStringLiteral("已下载 12.3 MB / 45.6 MB");
        running.percent = 27;
        running.state = TaskCenter::State::Running;
        running.capabilities.cancel = true;
        running.capabilities.pause = true;
        items.append(running);
        TaskCenter::Item done;
        done.key = QStringLiteral("41");
        done.title = QStringLiteral("安装 Forge 47.2.0");
        done.state = TaskCenter::State::Succeeded;
        done.finishedAt = QDateTime::currentDateTime().addSecs(-600);
        items.append(done);
        MainWindowUI::BottomBarTaskSummary summary;
        summary.hasTask = true;
        summary.state = TaskCenter::State::Running;
        summary.percent = 27;
        summary.title = QStringLiteral("下载 1.21.4-Fabric");
        summary.extraCount = 1;
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
        ui.setTaskCenterContent([items]() { return items; }, []() {});
        ui.setBottomBarTaskSummary(summary);
        pump();
        QPushButton *cell = ui.getTaskSummaryButton();
        if (cell != nullptr) {
            cell->click();
            pump();
            QWidget *popover = window.findChild<QWidget *>(ActivityPopover::objectNameValue());
            sweep.record(QStringLiteral("activity-popover"), QStringLiteral("底栏·任务中心浮层"),
                         QStringLiteral("主窗口 → 底栏中间那格「任务」"),
                         QStringLiteral("ui/ActivityPopover.cpp"), QStringLiteral("ui-review-shots"), popover,
                         &window);
            if (auto *openAll =
                    popover != nullptr ? popover->findChild<QPushButton *>(ActivityPopover::openAllButtonName())
                                       : nullptr) {
                openAll->click();
                for (int i = 0; i < 6; ++i) pump();
                QWidget *panel = window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName());
                sweep.record(QStringLiteral("task-center"), QStringLiteral("任务中心（整页）"),
                             QStringLiteral("底栏任务那格 → 任务中心浮层 → 「全部任务」"),
                             QStringLiteral("ui/TaskCenterPanel.cpp"), QStringLiteral("ui-review-shots"), panel,
                             &window);
                ui.overlay()->closeOverlay();
                pump();
            }
            if (popover != nullptr && popover->isVisible()) cell->click();
            pump();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    // 实例页「文件夹」浮层与分组齿轮菜单。
    {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
        ui.setFolderRows({{QStringLiteral("D:/Games/.minecraft"), QStringLiteral("当前文件夹"),
                           QStringLiteral("D:/Games/.minecraft"), true, true},
                          {QStringLiteral("E:/MC/modpacks"), QStringLiteral("整合包"),
                           QStringLiteral("E:/MC/modpacks"), false, false}});
        pump();
        if (auto *pill = window.findChild<QPushButton *>(MainWindowUI::instanceFolderPillName())) {
            pill->click();
            pump();
            QWidget *surface = nullptr;
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->windowType() == Qt::Popup && top->isVisible()) {
                    surface = top;
                    break;
                }
            sweep.record(QStringLiteral("instance-folder-popup"), QStringLiteral("实例页·文件夹浮层"),
                         QStringLiteral("主窗口 → 实例管理 → 版本列表顶上那排「文件夹」"),
                         QStringLiteral("ui/MainWindowUI.cpp"), QStringLiteral("ui-review-shots"), surface, &window);
            GlassPopover::closeAllIn(&window);
            // 兜底：离屏下有的浮层 closeAllIn 收不掉，留着会叠进后面每一张样张。
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->windowType() == Qt::Popup && top->isVisible()) top->close();
            pump();
        }
        if (QTreeWidget *tree = ui.getInstanceList(); tree != nullptr && tree->topLevelItemCount() > 0) {
            MainWindowUI::GroupRow row;
            row.groupId = QStringLiteral("g-modded");
            row.name = QStringLiteral("1.20.1 整合包");
            row.memberCount = 2;
            row.hasSelection = true;
            const QRect head = tree->visualItemRect(tree->topLevelItem(0));
            const QRect gear(tree->viewport()->mapToGlobal(QPoint(head.center().x(), head.top())), QSize(28, head.height()));
            ui.openGroupMenu(row, gear);
            pump();
            QWidget *surface = nullptr;
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->windowType() == Qt::Popup && top->isVisible()) {
                    surface = top;
                    break;
                }
            sweep.record(QStringLiteral("instance-group-menu"), QStringLiteral("实例页·分组齿轮菜单"),
                         QStringLiteral("主窗口 → 实例管理 → 分组行上的齿轮"),
                         QStringLiteral("ui/MainWindowUI.cpp"), QStringLiteral("ui-review-shots"), surface, &window);
            GlassPopover::closeAllIn(&window);
            // 兜底：离屏下有的浮层 closeAllIn 收不掉，留着会叠进后面每一张样张。
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->windowType() == Qt::Popup && top->isVisible()) top->close();
            pump();
        }
    }

    // ---------------------------------------------------------------- 覆盖层整页
    {
        AccountSkinDialog page;
        page.setAccounts({{QStringLiteral("uuid-1"), QStringLiteral("Steve"), QStringLiteral("2026-11-01"), true,
                           false},
                          {QStringLiteral("uuid-2"), QStringLiteral("Alex"), QStringLiteral("2026-10-20"), false,
                           true}});
        overlayDialog(QStringLiteral("account-skin"), QStringLiteral("账号与外观"),
                      QStringLiteral("主窗口 → 主页左下账号 → 「账号与外观」"),
                      QStringLiteral("ui/AccountSkinDialog.cpp"), QStringLiteral("input-widths / ui-review-shots"),
                      &page, AccountSkinDialog::windowTitle());
        overlayDialog(QStringLiteral("account-offline"), QStringLiteral("账号与外观·添加离线账号"),
                      QStringLiteral("账号与外观 → 添加账号 → 离线账号"), QStringLiteral("ui/AccountSkinDialog.cpp"),
                      QString(), &page, AccountSkinDialog::windowTitle(),
                      [&page] { page.showOfflineAddRow(); });
    }
    {
        QList<GroupDialog::Group> groups;
        GroupDialog::Group one;
        one.id = QStringLiteral("default-forge");
        one.name = QStringLiteral("Forge");
        groups.append(one);
        GroupDialog::Group two;
        two.id = QStringLiteral("g-modded");
        two.name = QStringLiteral("1.20.1 整合包");
        groups.append(two);
        GroupDialog page(groups, {{QStringLiteral("default-forge"), 3}, {QStringLiteral("g-modded"), 2}}, groups,
                         {{QStringLiteral("default-forge"), 3}});
        overlayDialog(QStringLiteral("group-settings"), QStringLiteral("分组设置"),
                      QStringLiteral("实例管理 → 分组视图 → 「分组设置」"),
                      QStringLiteral("ui/GroupDialog.cpp"), QString(), &page, GroupDialog::windowTitle());
    }
    {
        JavaManageDialog page;
        page.setRuntime(21, QStringLiteral("C:/Program Files/Java/jdk-21/bin/java.exe"));
        page.setSelection(false, QStringLiteral("C:/Program Files/Java/jdk-21/bin/java.exe"));
        overlayDialog(QStringLiteral("java-manage"), QStringLiteral("Java 管理"),
                      QStringLiteral("设置 → 启动 → 「Java 管理…」"), QStringLiteral("ui/JavaManageDialog.cpp"),
                      QString(), &page, page.titleText());
    }
    {
        JavaMismatchDialog page(QStringLiteral("1.20.1-Forge"), 21, 8,
                                QStringLiteral("1.8.0_301"),
                                QStringLiteral("C:/Program Files/Java/jre8/bin/java.exe"));
        overlayDialog(QStringLiteral("java-mismatch"), QStringLiteral("Java 版本不符"),
                      QStringLiteral("启动实例时 Java 与版本要求不符 → 提示框"),
                      QStringLiteral("ui/JavaMismatchDialog.cpp"), QString(), &page,
                      QStringLiteral("Java 版本不符"));
    }
    {
        NewServerPage page;
        QList<NewServerPage::FlavorOption> flavors;
        NewServerPage::FlavorOption vanilla;
        vanilla.token = QStringLiteral("vanilla");
        vanilla.name = QStringLiteral("原版（Vanilla）");
        vanilla.note = QStringLiteral("官方服务端");
        flavors.append(vanilla);
        NewServerPage::FlavorOption fabric;
        fabric.token = QStringLiteral("fabric");
        fabric.name = QStringLiteral("Fabric");
        fabric.note = QStringLiteral("轻量模组加载器");
        fabric.needsLoader = true;
        fabric.needsInstaller = true;
        fabric.needsLoaderGame = true;
        flavors.append(fabric);
        page.setFlavors(flavors, QStringLiteral("fabric"));
        page.setDefaults(QStringLiteral("我的服务端"), QStringLiteral("1.21.4"));
        overlayDialog(QStringLiteral("new-server"), QStringLiteral("新建服务端"),
                      QStringLiteral("主窗口 → 实例管理 → 服务器 → 新建服务端"),
                      QStringLiteral("ui/NewServerPage.cpp"), QStringLiteral("input-widths / ui-review-shots"), &page,
                      NewServerPage::windowTitle());
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
        page.setValues({{QStringLiteral("level-name"), QStringLiteral("world")},
                        {QStringLiteral("server-port"), QStringLiteral("25565")}});
        overlayDialog(QStringLiteral("server-settings"), QStringLiteral("服务端设置"),
                      QStringLiteral("实例管理 → 服务器 → 齿轮「设置」"),
                      QStringLiteral("ui/ServerSettingsPage.cpp"), QStringLiteral("input-widths"), &page,
                      ServerSettingsPage::windowTitle());
    }
    {
        ServerConsolePage page;
        page.setServerName(QStringLiteral("我的服务端"));
        page.setStatusText(QStringLiteral("运行中 · 已开 12 分钟"));
        page.setLog({QStringLiteral("[12:00:01] [Server thread/INFO]: Starting minecraft server version 1.21.4"),
                     QStringLiteral("[12:00:03] [Server thread/INFO]: Preparing level \"world\""),
                     QStringLiteral("[12:00:09] [Server thread/INFO]: Done (6.412s)! For help, type \"help\"")});
        overlayDialog(QStringLiteral("server-console"), QStringLiteral("服务端控制台"),
                      QStringLiteral("实例管理 → 服务器 → 「控制台」"),
                      QStringLiteral("ui/ServerConsolePage.cpp"), QStringLiteral("input-widths"), &page,
                      ServerConsolePage::windowTitle());
    }
    {
        AgentSettingsPage page;
        AgentView::ProviderDraft profile;
        profile.name = QStringLiteral("OpenAI 兼容");
        profile.baseUrl = QStringLiteral("https://api.example.com/v1");
        profile.model = QStringLiteral("gpt-4o-mini");
        page.setProfile(profile, true, QStringLiteral("sk-…（已保存）"));
        overlayDialog(QStringLiteral("agent-settings"), QStringLiteral("API 与模型设置"),
                      QStringLiteral("主窗口 → 智能体 → 右上「设置」"),
                      QStringLiteral("ui/AgentSettingsPage.cpp"), QStringLiteral("input-widths / ui-review-shots"),
                      &page, QStringLiteral("API 与模型设置"));
    }
    {
        AgentMcpPage page;
        AgentView::McpServerRow row;
        row.id = QStringLiteral("srv-1");
        row.name = QStringLiteral("本地 MCP");
        row.command = QStringLiteral("C:/Servers/mcp/server.exe");
        row.identityShort = QStringLiteral("abcdef123456");
        row.state = QStringLiteral("ready");
        row.enabled = true;
        row.toolCount = 3;
        page.setServers({row});
        overlayDialog(QStringLiteral("agent-mcp"), QStringLiteral("MCP 服务器"),
                      QStringLiteral("主窗口 → 智能体 → 右上「设置」→ 「MCP 服务器…」"),
                      QStringLiteral("ui/AgentMcpPage.cpp"), QString(), &page,
                      QStringLiteral("MCP 服务器"));
    }
    {
        // ui/AgentMcpPage.cpp 的「添加 / 编辑 MCP 服务器」：ChromeDialog + 表单 + 确定/取消。
        ChromeDialog page(&window);
        page.setObjectName(QStringLiteral("agentMcpEditDialog"));
        page.setWindowTitle(QStringLiteral("编辑 MCP 服务器"));
        page.resize(600, 420);
        auto *layout = new QVBoxLayout(page.body());
        auto *form = new QFormLayout;
        auto *name = new QLineEdit(QStringLiteral("本地 MCP"), &page);
        name->setObjectName(QStringLiteral("agentMcpNameEdit"));
        auto *command = new QLineEdit(QStringLiteral("C:/Servers/mcp/server.exe"), &page);
        command->setObjectName(QStringLiteral("agentMcpCommandEdit"));
        command->setPlaceholderText(QStringLiteral("绝对路径，例如 C:\\Servers\\mcp\\server.exe"));
        auto *arguments = new QPlainTextEdit(QStringLiteral("--stdio"), &page);
        arguments->setObjectName(QStringLiteral("agentMcpArgumentsEdit"));
        arguments->setMaximumHeight(90);
        arguments->setPlaceholderText(QStringLiteral("一行一个参数，不经过 shell"));
        auto *workingDirectory = new QLineEdit(QStringLiteral("C:/Servers/mcp"), &page);
        workingDirectory->setObjectName(QStringLiteral("agentMcpWorkdirEdit"));
        auto *environment = new QLineEdit(QStringLiteral("MCP_TOKEN"), &page);
        environment->setObjectName(QStringLiteral("agentMcpEnvEdit"));
        environment->setPlaceholderText(QStringLiteral("要透传的环境变量名，逗号分隔（只记名字）"));
        auto *enabled = new QCheckBox(QStringLiteral("启用这个服务器"), &page);
        enabled->setObjectName(QStringLiteral("agentMcpEnabledEdit"));
        enabled->setChecked(true);
        form->addRow(QStringLiteral("名称"), name);
        form->addRow(QStringLiteral("可执行文件"), command);
        form->addRow(QStringLiteral("参数"), arguments);
        form->addRow(QStringLiteral("工作目录"), workingDirectory);
        form->addRow(QStringLiteral("环境变量"), environment);
        form->addRow(QString(), enabled);
        layout->addLayout(form);
        auto *note = new QLabel(
            QStringLiteral("保存配置后需要重新确认（首次启动或配置变化都会要求确认）。不会自动下载或安装任何 MCP 服务器。"),
            &page);
        note->setObjectName(QStringLiteral("agentMcpEditNote"));
        note->setWordWrap(true);
        layout->addWidget(note);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &page);
        layout->addWidget(buttons);
        overlayDialog(QStringLiteral("agent-mcp-edit"), QStringLiteral("MCP 服务器·编辑"),
                      QStringLiteral("主窗口 → 智能体 → 右上「设置」→ 「MCP 服务器…」→ 某一行「编辑…」"),
                      QStringLiteral("ui/AgentMcpPage.cpp"), QString(), &page,
                      QStringLiteral("编辑 MCP 服务器"));
    }
    {
        PresetEditorPage page(QJsonObject{{QStringLiteral("name"), QStringLiteral("性能整合包")},
                                          {QStringLiteral("icon"), QStringLiteral("icon.png")},
                                          {QStringLiteral("description"), QStringLiteral("给 1.20.1 用的轻量整合包")}});
        overlayDialog(QStringLiteral("preset-editor"), QStringLiteral("编辑预设"),
                      QStringLiteral("实例管理 → 预设 → 「编辑」"), QStringLiteral("ui/PresetEditorPage.cpp"),
                      QStringLiteral("input-widths"), &page, QStringLiteral("编辑预设"));
    }
    {
        PresetManagePage page;
        QList<PresetManagePage::PresetItem> presets;
        PresetManagePage::PresetItem one;
        one.id = QStringLiteral("p-perf");
        one.name = QStringLiteral("性能整合包");
        one.description = QStringLiteral("给 1.20.1 用的轻量整合包");
        one.categoryKey = QStringLiteral("mods");
        one.categoryText = QStringLiteral("模组");
        one.sectionCount = 3;
        one.isDefault = true;
        presets.append(one);
        page.setPresets(presets, {}, {});
        overlayDialog(QStringLiteral("preset-manage"), QStringLiteral("预设管理"),
                      QStringLiteral("主窗口 → 实例管理 → 「预设管理」"),
                      QStringLiteral("ui/PresetManagePage.cpp"), QString(), &page, QStringLiteral("预设管理"));
    }
    {
        TaskDetailPage page;
        TaskCenter::Item item;
        item.key = QStringLiteral("42");
        item.title = QStringLiteral("下载 1.21.4-Fabric");
        item.detail = QStringLiteral("已下载 12.3 MB / 45.6 MB");
        item.bytesText = QStringLiteral("12.3 MB / 45.6 MB");
        item.percent = 27;
        item.state = TaskCenter::State::Running;
        item.startedAt = QDateTime::currentDateTime().addSecs(-180);
        item.capabilities.cancel = true;
        item.capabilities.pause = true;
        TaskCenter::Step step;
        step.name = QStringLiteral("下载主文件");
        step.state = TaskCenter::StepState::Running;
        item.steps.append(step);
        page.setTask(item);
        overlayDialog(QStringLiteral("task-detail"), QStringLiteral("任务详情"),
                      QStringLiteral("任务中心 → 点某一行"), QStringLiteral("ui/TaskDetailPage.cpp"), QString(),
                      &page, TaskDetailPage::windowTitle());
    }
    {
        UpdateCenterPage page;
        UpdateCenterPage::Report report;
        report.instanceName = QStringLiteral("1.20.1-Forge");
        report.summary = QStringLiteral("查了 41 个文件，3 个有新版本。");
        UpdateCenterPage::Row row;
        row.path = QStringLiteral("D:/Games/modpack/mods/sodium.jar");
        row.name = QStringLiteral("Sodium");
        row.fileName = QStringLiteral("sodium-fabric-0.6.9.jar");
        row.currentText = QStringLiteral("0.6.9");
        row.targetText = QStringLiteral("0.6.13");
        row.state = UpdateCenterPage::State::Outdated;
        row.targetVersionId = QStringLiteral("abc123");
        report.rows.append(row);
        page.setReport(report);
        overlayDialog(QStringLiteral("update-center"), QStringLiteral("更新中心"),
                      QStringLiteral("实例管理 → 「检查更新」"), QStringLiteral("ui/UpdateCenterPage.cpp"),
                      QString(), &page, UpdateCenterPage::windowTitle());
    }
    {
        IntegrityReportPage page;
        IntegrityReportPage::Report report;
        report.instanceName = QStringLiteral("1.20.1-Forge");
        report.summary = QStringLiteral("3120 个文件里 2 个要修：客户端 jar 缺失，1 个库损坏。");
        IntegrityReportPage::Group group;
        group.name = QStringLiteral("客户端");
        group.missing = 1;
        IntegrityReportPage::Row row;
        row.label = QStringLiteral("客户端 jar");
        row.detail = QStringLiteral("缺少文件");
        row.path = QStringLiteral("D:/Games/versions/1.20.1/1.20.1.jar");
        group.rows.append(row);
        report.groups.append(group);
        page.setReport(report);
        overlayDialog(QStringLiteral("integrity-report"), QStringLiteral("完整性检查"),
                      QStringLiteral("实例管理 → 「完整性检查」"),
                      QStringLiteral("ui/IntegrityReportPage.cpp"), QString(), &page,
                      IntegrityReportPage::windowTitle());
    }
    {
        ResourceDetailPage page;
        ResourceDetail::Header header;
        header.title = QStringLiteral("Sodium");
        header.description = QStringLiteral("现代渲染优化模组，显著提升帧率。");
        header.metaLine = QStringLiteral("Fabric / 下载 2.31 亿次 / 更新于 5 天前 / Modrinth");
        header.openLabel = QStringLiteral("转到 Modrinth");
        header.openUrl = QStringLiteral("https://modrinth.com/mod/sodium");
        header.nameToCopy = QStringLiteral("Sodium");
        page.setHeader(header);
        page.setChips({{QString(), QStringLiteral("全部")}, {QStringLiteral("1.21.4"), QStringLiteral("1.21.4")},
                       {QStringLiteral("1.20.1"), QStringLiteral("1.20.1")}},
                      QString());
        ResourceDetail::Group group;
        group.title = QStringLiteral("Fabric 1.21.4");
        ResourceDetail::Row row;
        row.key = QStringLiteral("mc1.21.4-0.6.13");
        row.title = QStringLiteral("sodium-fabric-0.6.13.jar");
        row.subText = QStringLiteral("2026-11-02 · 1.2 MB");
        group.rows.append(row);
        page.setGroups({group});
        overlayDialog(QStringLiteral("resource-detail"), QStringLiteral("资源详情"),
                      QStringLiteral("下载 → 资源列表 → 点某一条"), QStringLiteral("ui/ResourceDetailPage.cpp"),
                      QString(), &page, page.titleText());
    }
    {
        InstanceUpgradePage page;
        page.setVersionOptions({QStringLiteral("1.20.1"), QStringLiteral("1.21.4")});
        page.setLoaderOptions({{QStringLiteral("fabric"), QStringLiteral("Fabric")}},
                              {QStringLiteral("0.16.9"), QStringLiteral("0.16.7")});
        overlayDialog(QStringLiteral("instance-upgrade"), QStringLiteral("实例升级"),
                      QStringLiteral("实例管理 → 右键实例 → 「升级版本」"),
                      QStringLiteral("ui/InstanceUpgradePage.cpp"), QStringLiteral("input-widths"),
                      embedPage(&page, QStringLiteral("surfaceInstanceUpgrade")), QStringLiteral("实例升级"));
    }
    {
        UpgradeReportDialog page;
        QList<UpgradeReport::Row> rows;
        UpgradeReport::Row ok;
        ok.name = QStringLiteral("Sodium");
        ok.detail = QStringLiteral("0.6.13 支持 Fabric 1.21.4");
        ok.bucket = UpgradeReport::Bucket::Compatible;
        rows.append(ok);
        UpgradeReport::Row bad;
        bad.name = QStringLiteral("OptiFine");
        bad.detail = QStringLiteral("不支持 Fabric");
        bad.bucket = UpgradeReport::Bucket::Incompatible;
        bad.required = true;
        rows.append(bad);
        page.setRows(rows);
        overlayDialog(QStringLiteral("upgrade-report"), QStringLiteral("升级兼容报告"),
                      QStringLiteral("实例升级 → 「兼容报告」"), QStringLiteral("ui/UpgradeReportDialog.cpp"),
                      QString(), &page, QStringLiteral("兼容报告"));
    }
    {
        UpgradeHistoryDialog page;
        QList<UpgradeHistory::Entry> entries;
        UpgradeHistory::Entry entry;
        entry.transactionId = QStringLiteral("t-20261110-1");
        entry.sourceName = QStringLiteral("1.20.1-Forge");
        entry.targetName = QStringLiteral("1.21.4-Fabric");
        entry.fromGameVersion = QStringLiteral("1.20.1");
        entry.toGameVersion = QStringLiteral("1.21.4");
        entry.startedAt = QStringLiteral("2026-11-10T09:00:00Z");
        entry.finishedAt = QStringLiteral("2026-11-10T09:03:20Z");
        entry.ok = true;
        entry.copiedFiles = 42;
        entry.snapshotName = QStringLiteral("snapshot-1");
        entries.append(entry);
        page.setEntries(entries);
        overlayDialog(QStringLiteral("upgrade-history"), QStringLiteral("升级历史"),
                      QStringLiteral("实例升级 → 「历史」"), QStringLiteral("ui/UpgradeHistoryDialog.cpp"),
                      QString(), &page, QStringLiteral("升级历史"));
    }
    {
        QTemporaryDir schem(QDir::current().filePath(QStringLiteral(".surface-schem-XXXXXX")));
        const QString path = QDir(schem.path()).filePath(QStringLiteral("demo.schem"));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write("WTFML surface sweep placeholder");
        SchematicInfoDialog page(path);
        overlayDialog(QStringLiteral("schematic-info"), QStringLiteral("原理图信息"),
                      QStringLiteral("实例管理 → 结构/原理图 → 点某个 .schem"),
                      QStringLiteral("ui/SchematicInfoDialog.cpp"), QString(), &page, QStringLiteral("原理图信息"));
    }
    {
        PresetModEditor page({{QStringLiteral("modrinth"), QStringLiteral("sodium"), QStringLiteral("Sodium"),
                               QStringLiteral("1.20.1"), QStringLiteral("fabric"), true},
                              {QStringLiteral("curseforge"), QStringLiteral("jei"), QStringLiteral("JEI"),
                               QStringLiteral("1.20.1"), QStringLiteral("forge"), false}});
        overlayDialog(QStringLiteral("preset-mod-editor"), QStringLiteral("预设·模组编辑"),
                      QStringLiteral("预设管理 → 「编辑」→ 模组段"), QStringLiteral("ui/PresetModEditor.cpp"),
                      QString(), &page, QStringLiteral("模组"));
    }
    {
        PresetOptionsEditor page({{QStringLiteral("level-name"), QStringLiteral("world")},
                                  {QStringLiteral("server-port"), QStringLiteral("25565")}});
        overlayDialog(QStringLiteral("preset-options-editor"), QStringLiteral("预设·选项编辑"),
                      QStringLiteral("预设管理 → 「编辑」→ 选项段"),
                      QStringLiteral("ui/PresetOptionsEditor.cpp"), QString(), &page, QStringLiteral("选项"));
    }
    {
        // 预设应用前那颗「确认模组预设」：有缺失项时要让用户填替代项目，是最宽的一档。
        Presets::ModInstallPlan plan;
        plan.ok = false;
        plan.missing = 1;
        Presets::ModInstallRow downloadRow;
        downloadRow.key = QStringLiteral("modrinth:sodium");
        downloadRow.title = QStringLiteral("Sodium");
        downloadRow.status = Presets::ModInstallStatus::Download;
        downloadRow.version.id = QStringLiteral("mc1.21.4-0.6.13");
        downloadRow.version.versionNumber = QStringLiteral("0.6.13");
        downloadRow.file.filename = QStringLiteral("sodium-fabric-0.6.13.jar");
        downloadRow.file.size = 1234567;
        downloadRow.file.sha1 = QStringLiteral("0000000000000000000000000000000000000000");
        plan.rows.append(downloadRow);
        Presets::ModInstallRow missingRow;
        missingRow.key = QStringLiteral("modrinth:iris");
        missingRow.title = QStringLiteral("Iris");
        missingRow.status = Presets::ModInstallStatus::Missing;
        missingRow.problem = QStringLiteral("没有兼容 1.21.4 的发行版");
        missingRow.details = {QStringLiteral("modrinth:iris 只支持到 1.20.6")};
        plan.rows.append(missingRow);
        auto *page = new PresetModReview(plan, &window);
        page->show();
        pump();
        sweep.record(QStringLiteral("preset-mod-review"), QStringLiteral("预设·模组确认"),
                     QStringLiteral("实例管理 → 预设 → 应用 → 有缺失项时「确认安装」"),
                     QStringLiteral("ui/PresetModReview.cpp"), QString(), page, page);
        page->hide();
        pump();
        page->setAttribute(Qt::WA_DeleteOnClose, false);
        page->deleteLater();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    {
        // src/controllers/InstanceDialogController.cpp:63 那一块「方块统计」：ChromeDialog + 原生 QTableWidget。
        ChromeDialog page(&window, true);
        page.setWindowTitle(QStringLiteral("方块统计"));
        page.resize(720, 500);
        auto *layout = new QVBoxLayout(page.body());
        layout->addWidget(new QLabel(QStringLiteral("共 3 种方块，合计 4,182 个"), &page));
        auto *search = new QLineEdit(&page);
        search->setPlaceholderText(QStringLiteral("搜索方块 ID，例如 minecraft:stone"));
        layout->addWidget(search);
        auto *table = new QTableWidget(0, 2, &page);
        table->setObjectName(QStringLiteral("schematicBlockCounts"));
        table->setHorizontalHeaderLabels({QStringLiteral("方块 ID"), QStringLiteral("数量")});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->verticalHeader()->hide();
        // 和产品那段一样的列宽（早先夹具漏了这两行，第一列被挤窄、方块 ID 被省略）。
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->setRowCount(3);
        table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("minecraft:stone")));
        table->setItem(0, 1, new QTableWidgetItem(QStringLiteral("2048")));
        table->setItem(1, 0, new QTableWidgetItem(QStringLiteral("minecraft:oak_planks")));
        table->setItem(1, 1, new QTableWidgetItem(QStringLiteral("1536")));
        table->setItem(2, 0, new QTableWidgetItem(QStringLiteral("minecraft:glass")));
        table->setItem(2, 1, new QTableWidgetItem(QStringLiteral("598")));
        layout->addWidget(table, 1);
        auto *buttons = new QDialogButtonBox(&page);
        buttons->addButton(QStringLiteral("复制清单"), QDialogButtonBox::ActionRole);
        buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
        layout->addWidget(buttons);
        overlayDialog(QStringLiteral("schematic-block-counts"), QStringLiteral("原理图·方块统计"),
                      QStringLiteral("实例管理 → 结构/原理图 → 点某一行 → 「方块统计」"),
                      QStringLiteral("src/controllers/InstanceDialogController.cpp"), QString(), &page,
                      QStringLiteral("方块统计"));
    }
    {
        // ui/PresetInstallPicker.cpp:380 的「兼容性详情」：ChromeDialog + 只读 QPlainTextEdit。
        ChromeDialog page(&window, false);
        page.setWindowTitle(QStringLiteral("兼容性详情"));
        page.resize(560, 420);
        auto *layout = new QVBoxLayout(page.body());
        auto *text = new QPlainTextEdit(&page);
        text->setReadOnly(true);
        text->setPlainText(QStringLiteral("性能整合包\n\n缺失：sodium（Modrinth: sodium）\n"
                                          "暂无法确认：iris（本地没有可用的模组信息）\n"));
        layout->addWidget(text, 1);
        auto *buttons = new QDialogButtonBox(&page);
        buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
        layout->addWidget(buttons);
        overlayDialog(QStringLiteral("preset-reasons"), QStringLiteral("实例预设·兼容性详情"),
                      QStringLiteral("下载 → 安装页 → 「选择预设」→ 「兼容性详情」"),
                      QStringLiteral("ui/PresetInstallPicker.cpp"), QString(), &page,
                      QStringLiteral("兼容性详情"));
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
        std::unique_ptr<QDialog> wrapper(embedPage(page, QStringLiteral("surfaceWorldSettings")));
        overlayDialog(QStringLiteral("world-settings"), QStringLiteral("存档设置"),
                      QStringLiteral("实例管理 → 设置 → 存档 → 点某个存档"),
                      QStringLiteral("ui/WorldSettingsPage.cpp"), QStringLiteral("input-widths"), wrapper.get(),
                      QStringLiteral("存档设置"));
    }
    {
        GameLogWindow page(QStringLiteral("1.20.1-Forge"));
        page.appendText(QStringLiteral("[12:00:01] [main/INFO]: ModLauncher running: args [--gameDir, .]\n"));
        page.appendText(QStringLiteral("[12:00:04] [main/INFO]: Loading 42 mods\n"));
        page.appendText(QStringLiteral("[12:00:09] [Render thread/ERROR]: Failed to load shader\n"));
        page.resize(kSurfaceSize);
        page.show();
        pump();
        sweep.record(QStringLiteral("game-log"), QStringLiteral("游戏日志窗"),
                     QStringLiteral("实例管理 → 「日志」→ 游戏日志"),
                     QStringLiteral("ui/GameLogWindow.cpp"), QString(), &page, &page);
        page.hide();
        pump();
    }
    {
        // 更新向导：四步各是一个真页面，这里按产品的形状把四块拼进去。
        auto *target = new QWidget;
        auto *targetLayout = new QVBoxLayout(target);
        targetLayout->addWidget(new QLabel(QStringLiteral("目标版本：1.21.4 / Fabric 0.16.9")));
        targetLayout->addWidget(new QLineEdit(QStringLiteral("1.21.4-Fabric")));
        auto *mods = new QWidget;
        auto *modsLayout = new QVBoxLayout(mods);
        modsLayout->addWidget(new QLabel(QStringLiteral("将迁移 42 个模组")));
        auto *confirmation = new QWidget;
        auto *confirmationLayout = new QVBoxLayout(confirmation);
        confirmationLayout->addWidget(new QLabel(QStringLiteral("确认迁移以下内容：模组、配置、存档")));
        auto *progress = new QWidget;
        auto *progressLayout = new QVBoxLayout(progress);
        progressLayout->addWidget(new QLabel(QStringLiteral("正在复制文件… 12 / 42")));
        InstanceUpdateWizard wizard(target, mods, confirmation, progress);
        wizard.resize(kSurfaceSize);
        wizard.show();
        pump();
        for (int step = 0; step < 4; ++step) {
            wizard.setCurrentId(step);
            pump();
            sweep.record(QStringLiteral("instance-update-wizard-%1").arg(step + 1),
                         QStringLiteral("更新向导·第 %1 步").arg(step + 1),
                         QStringLiteral("实例管理 → 右键实例 → 「更新实例」→ 第 %1 步").arg(step + 1),
                         QStringLiteral("ui/InstanceUpdateWizard.cpp"), QString(), &wizard, &wizard);
        }
        wizard.hide();
        pump();
    }
    {
        auto *page = new ThemeAdvancedEditor;
        if (auto *toggle = page->findChild<QAbstractButton *>(QStringLiteral("themeAdvancedToggle")))
            if (toggle->isCheckable() && !toggle->isChecked()) toggle->click();
        std::unique_ptr<QDialog> wrapper(embedPage(page, QStringLiteral("surfaceThemeAdvanced")));
        overlayDialog(QStringLiteral("theme-advanced"), QStringLiteral("个性化·高级编辑"),
                      QStringLiteral("设置 → 个性化 → 「高级」"),
                      QStringLiteral("ui/ThemeAdvancedEditor.cpp"), QStringLiteral("input-widths"), wrapper.get(),
                      QStringLiteral("高级"));
    }
    {
        auto *page = new ResourcePackPage;
        std::unique_ptr<QDialog> wrapper(embedPage(page, QStringLiteral("surfaceResourcePack")));
        overlayDialog(QStringLiteral("resource-pack-page"), QStringLiteral("资源包页"),
                      QStringLiteral("实例管理 → 资源包"), QStringLiteral("ui/ResourcePackPage.cpp"),
                      QStringLiteral("resource-packs"), wrapper.get(), QStringLiteral("资源包"));
    }

    // ---------------------------------------------------------------- 实例设置：逐页
    {
        QTemporaryDir instanceRoot(QDir::current().filePath(QStringLiteral(".surface-instance-XXXXXX")));
        if (!instanceRoot.isValid()) return 2;
        const QString root = instanceRoot.path();
        for (const QString &sub : {QStringLiteral("mods"), QStringLiteral("resourcepacks"), QStringLiteral("datapacks"),
                                   QStringLiteral("shaderpacks"), QStringLiteral("saves"), QStringLiteral("logs"),
                                   QStringLiteral("screenshots"), QStringLiteral("config"), QStringLiteral("game")})
            QDir().mkpath(QDir(root).filePath(sub));
        QFile mod(QDir(root).filePath(QStringLiteral("mods/sodium-fabric-0.6.13.jar")));
        if (mod.open(QIODevice::WriteOnly)) mod.write("placeholder");
        QFile config(QDir(root).filePath(QStringLiteral("config/sodium-options.json")));
        if (config.open(QIODevice::WriteOnly))
            config.write("{\n  \"quality\": \"high\",\n  \"maxFps\": 120\n}\n");

        using Dlg = InstanceSettingsDialog;
        Dlg::GlobalValues global;
        global.javaPath = QStringLiteral("C:/Program Files/Java/jdk-21/bin/java.exe");
        global.xmx = 4096;
        global.xms = 1024;
        global.autoMemory = false;
        global.extraJvmArgs = QStringLiteral("-XX:+UseG1GC");
        Dlg dialog(QStringLiteral("1.20.1 整合包"), QStringLiteral("1.20.1-Forge"), Dlg::Override(), global,
                   QStringList(), false);
        QHash<QString, QString> dirs;
        dirs.insert(QStringLiteral("mod"), QDir(root).filePath(QStringLiteral("mods")));
        dirs.insert(QStringLiteral("resourcepack"), QDir(root).filePath(QStringLiteral("resourcepacks")));
        dirs.insert(QStringLiteral("datapack"), QDir(root).filePath(QStringLiteral("datapacks")));
        dirs.insert(QStringLiteral("shader"), QDir(root).filePath(QStringLiteral("shaderpacks")));
        dirs.insert(QStringLiteral("schematic"), QDir(root).filePath(QStringLiteral("schematics")));
        dirs.insert(QStringLiteral("config"), QDir(root).filePath(QStringLiteral("config")));
        dirs.insert(QStringLiteral("save"), QDir(root).filePath(QStringLiteral("saves")));
        dirs.insert(QStringLiteral("logs"), QDir(root).filePath(QStringLiteral("logs")));
        dirs.insert(QStringLiteral("screenshots"), QDir(root).filePath(QStringLiteral("screenshots")));
        dirs.insert(QStringLiteral("game"), QDir(root).filePath(QStringLiteral("game")));
        dialog.setInstanceDir(root);
        dialog.setFileDirs(dirs);
        dialog.setLastLaunch(QDateTime::currentDateTime().addSecs(-3600 * 26));
        QStringList keys = Dlg::pinnedPageKeys();
        keys += Dlg::filePageKeys();
        keys += Dlg::lifecyclePageKeys();
        for (const QString &key : keys) {
            overlayDialog(QStringLiteral("instance-settings-%1").arg(key),
                          QStringLiteral("实例设置·%1").arg(Dlg::pageText(key)),
                          QStringLiteral("实例管理 → 右键实例 → 「设置」→ 左栏「%1」").arg(Dlg::pageText(key)),
                          QStringLiteral("ui/InstanceSettingsDialog.cpp"),
                          key == QLatin1String("overview") ? QStringLiteral("input-widths / ui-review-shots")
                          : key == QLatin1String("launch") ? QStringLiteral("input-widths")
                                                           : QString(),
                          &dialog, QStringLiteral("实例设置"), [&dialog, key] { dialog.showPage(key); });
        }
    }

    // ---------------------------------------------------------------- 提示卡（InlineMessageBox 一族）
    {
        QStringList items;
        for (int i = 0; i < 6; ++i) items.append(QStringLiteral("1.20.1-Forge 实例 %1").arg(i + 1));
        promptSurface(QStringLiteral("inline-confirm"), QStringLiteral("提示卡·确认"),
                      QStringLiteral("任何一次 InlineMessage::confirm 类调用"),
                      QStringLiteral("ui/InlineMessage.cpp"), QString(), QStringLiteral("删除实例"),
                      [](InlineMessageBox &) {}, MessageKind::Question);
        promptSurface(QStringLiteral("inline-input"), QStringLiteral("提示卡·输入名字"),
                      QStringLiteral("实例管理 → 右键实例 → 「重命名」"),
                      QStringLiteral("ui/InlineMessage.cpp"), QString(), QStringLiteral("重命名实例"),
                      [](InlineMessageBox &box) { box.setLineEdit(QStringLiteral("1.20.1-Forge")); },
                      MessageKind::Question);
        promptSurface(QStringLiteral("inline-list"), QStringLiteral("提示卡·列表选择"),
                      QStringLiteral("实例管理 → 右键实例 → 「复制到…」"),
                      QStringLiteral("ui/InlineMessage.cpp"), QString(), QStringLiteral("复制到哪个实例"),
                      [&items](InlineMessageBox &box) { box.setList(items, 0); }, MessageKind::Question);
        promptSurface(QStringLiteral("inline-report"), QStringLiteral("提示卡·长文本报告"),
                      QStringLiteral("实例升级 / 安装页 → 「详细信息」"),
                      QStringLiteral("src/controllers/InstanceDialogController.cpp"), QString(),
                      QStringLiteral("检查报告"),
                      [](InlineMessageBox &box) {
                          auto *report = new QPlainTextEdit;
                          report->setReadOnly(true);
                          report->setMinimumHeight(180);
                          report->setMaximumHeight(300);
                          report->setPlainText(QStringLiteral(
                              "客户端 jar：缺少文件\n库 com.example:1.0：大小 0 字节，应为 12345\n"
                              "资源对象 abc：校验失败\n"));
                          box.setContent(report);
                      },
                      MessageKind::Warning);
        promptSurface(QStringLiteral("inline-preview"), QStringLiteral("提示卡·文本预览"),
                      QStringLiteral("安装页 → 版本详情 → 「预览」"),
                      QStringLiteral("src/controllers/InstanceDialogController.cpp"), QString(),
                      QStringLiteral("版本详情"),
                      [](InlineMessageBox &box) {
                          auto *text = new QPlainTextEdit;
                          text->setReadOnly(true);
                          text->setPlainText(QStringLiteral("版本：1.21.4\n类型：正式版\n发布时间：2026-10-30\n"
                                                            "加载器：Fabric 0.16.9\n"));
                          box.setContent(text);
                      },
                      MessageKind::Information);
    }

    // 安装页的「实例预设」弹框：宿主是一个 InlineMessageBox，setContent() 塞进来的
    // PresetInstallPicker。空数据 / 有数据各一份，另外单拍一张「综合预设」那一页。
    {
        promptSurface(QStringLiteral("install-preset-empty"), QStringLiteral("安装页·实例预设（空）"),
                      QStringLiteral("下载 → 安装页 → 「选择预设」"), QStringLiteral("ui/PresetInstallPicker.cpp"),
                      QString(), QStringLiteral("实例预设"),
                      [](InlineMessageBox &box) {
                          auto *picker = new PresetInstallPicker;
                          picker->setContext(QStringLiteral("1.20.1"), QStringLiteral("Forge"));
                          QList<PresetInstallPicker::Category> categories;
                          // 分类与名字照 PresetController::installCategories()（早先夹具把 id 当名字喂进来）。
                          const QList<QPair<QString, QString>> kinds = {
                              {QStringLiteral("gameSettings"), QStringLiteral("游戏设置")},
                              {QStringLiteral("mods"), QStringLiteral("模组")},
                              {QStringLiteral("resourcePacks"), QStringLiteral("资源包")},
                              {QStringLiteral("dataPacks"), QStringLiteral("数据包")},
                              {QStringLiteral("shaderPacks"), QStringLiteral("光影包")}};
                          for (const auto &[key, title] : kinds) {
                              PresetInstallPicker::Category category;
                              category.key = key;
                              category.title = title;
                              categories.append(category);
                          }
                          picker->setCategories(categories);
                          picker->setComposites({});
                          box.setContent(picker, 800);
                      },
                      MessageKind::Question);

        // 有数据的三档共用一份：五类照产品，模组选着一份（兼容检查报缺失 2 项），
        // 游戏设置选着一份，数据包 / 光影包没有可选项；另有两份综合预设。
        const auto fillPicker = [](PresetInstallPicker *picker, bool withComposites) {
            picker->setContext(QStringLiteral("1.20.1"), QStringLiteral("Forge"));
            PresetInstallPicker::Category game;
            game.key = QStringLiteral("gameSettings");
            game.title = QStringLiteral("游戏设置");
            game.selectedId = QStringLiteral("g-pvp");
            game.status = QStringLiteral("应用 6 项");
            game.options = {{QStringLiteral("g-pvp"), QStringLiteral("PVP 键位"), true, false, 6},
                            {QStringLiteral("g-rec"), QStringLiteral("录屏画质"), true, false, 4}};
            PresetInstallPicker::Category mods;
            mods.key = QStringLiteral("mods");
            mods.title = QStringLiteral("模组");
            mods.selectedId = QStringLiteral("p-tech");
            mods.options = {{QStringLiteral("p-perf"), QStringLiteral("性能优化"), true, false, 12},
                            {QStringLiteral("p-tech"), QStringLiteral("生电基础包"), true, false, 47},
                            {QStringLiteral("all-in-one"), QStringLiteral("整合包同款"), true, true, 31}};
            PresetInstallPicker::Category packs;
            packs.key = QStringLiteral("resourcePacks");
            packs.title = QStringLiteral("资源包");
            packs.options = {{QStringLiteral("r-plus"), QStringLiteral("原版增强"), true, false, 3}};
            PresetInstallPicker::Category data;
            data.key = QStringLiteral("dataPacks");
            data.title = QStringLiteral("数据包");
            PresetInstallPicker::Category shaders;
            shaders.key = QStringLiteral("shaderPacks");
            shaders.title = QStringLiteral("光影包");
            picker->setCategories({game, mods, packs, data, shaders});
            picker->setStatus(QStringLiteral("mods"), QStringLiteral("缺失 2 项"), {QStringLiteral("sodium（缺失）")},
                              true, 47, 2, 0);
            QList<PresetInstallPicker::Composite> composites;
            if (withComposites) {
                PresetInstallPicker::Composite pack;
                pack.id = QStringLiteral("all-in-one");
                pack.name = QStringLiteral("整合包同款");
                pack.parts = {{QStringLiteral("mods"), QStringLiteral("p-tech")},
                              {QStringLiteral("gameSettings"), QStringLiteral("g-pvp")},
                              {QStringLiteral("resourcePacks"), QStringLiteral("r-plus")}};
                pack.itemCount = 31;
                pack.counts = {{QStringLiteral("mods"), 24}, {QStringLiteral("gameSettings"), 6},
                               {QStringLiteral("resourcePacks"), 1}};
                PresetInstallPicker::Composite friends;
                friends.id = QStringLiteral("friends");
                friends.name = QStringLiteral("朋友服一键");
                friends.parts = {{QStringLiteral("mods"), QStringLiteral("p-perf")},
                                 {QStringLiteral("gameSettings"), QStringLiteral("g-rec")}};
                friends.itemCount = 18;
                friends.counts = {{QStringLiteral("mods"), 15}, {QStringLiteral("gameSettings"), 3}};
                composites = {pack, friends};
            }
            picker->setComposites(composites);
        };
        const auto showLastPage = [](PresetInstallPicker *picker) {
            if (auto *nav = picker->findChild<QListWidget *>(PresetInstallPicker::tabBarName()))
                nav->setCurrentRow(nav->count() - 1);
        };
        promptSurface(QStringLiteral("install-preset-full"), QStringLiteral("安装页·实例预设（有数据）"),
                      QStringLiteral("下载 → 安装页 → 「选择预设」"), QStringLiteral("ui/PresetInstallPicker.cpp"),
                      QString(), QStringLiteral("实例预设"),
                      [fillPicker](InlineMessageBox &box) {
                          auto *picker = new PresetInstallPicker;
                          fillPicker(picker, true);
                          box.setContent(picker, 800);
                          picker->setCurrentTab(1);  // 模组页：设计稿 v=pick
                      },
                      MessageKind::Question);
        promptSurface(QStringLiteral("install-preset-composite"), QStringLiteral("安装页·实例预设·综合预设页"),
                      QStringLiteral("下载 → 安装页 → 「选择预设」→ 左栏「综合预设」"),
                      QStringLiteral("ui/PresetInstallPicker.cpp"), QString(), QStringLiteral("实例预设"),
                      [fillPicker, showLastPage](InlineMessageBox &box) {
                          auto *picker = new PresetInstallPicker;
                          fillPicker(picker, true);
                          box.setContent(picker, 800);
                          showLastPage(picker);
                          if (auto *list = picker->findChild<QListWidget *>(QStringLiteral("presetCompositeList")))
                              list->setCurrentRow(0);
                      },
                      MessageKind::Question);
        promptSurface(QStringLiteral("install-preset-composite-empty"),
                      QStringLiteral("安装页·实例预设·综合预设页（空）"),
                      QStringLiteral("下载 → 安装页 → 「选择预设」→ 左栏「综合预设」"),
                      QStringLiteral("ui/PresetInstallPicker.cpp"), QString(), QStringLiteral("实例预设"),
                      [fillPicker, showLastPage](InlineMessageBox &box) {
                          auto *picker = new PresetInstallPicker;
                          fillPicker(picker, false);
                          box.setContent(picker, 800);
                          showLastPage(picker);
                      },
                      MessageKind::Question);
    }

    sweep.writeReports();

    // 守卫：计入的残留必须是 0。例外只认 SurfaceScan.hpp 的 checkBoxException()，
    // 那里每条都带一句理由；kind = exempt 的条目不计数。
    QMap<QString, int> kindCounts;
    QList<SurfaceRecord> withFindings;
    QList<QPair<QString, SurfaceFinding>> residue;
    QList<QPair<QString, SurfaceFinding>> exceptions;
    for (const SurfaceRecord &rec : sweep.records()) {
        bool counted = false;
        for (const SurfaceFinding &finding : rec.findings) {
            if (finding.kind == QStringLiteral("exempt")) {
                exceptions.append({rec.key, finding});
                continue;
            }
            ++kindCounts[finding.kind];
            residue.append({rec.key, finding});
            counted = true;
        }
        if (counted) withFindings.append(rec);
    }
    std::printf("\nsurface-sweep: %d 个表面，%d 个扫出问题，%d 处（另有例外 %d 处）\n", int(sweep.records().size()),
                int(withFindings.size()), int(residue.size()), int(exceptions.size()));
    for (auto it = kindCounts.constBegin(); it != kindCounts.constEnd(); ++it)
        std::printf("  %-22s %d\n", qPrintable(it.key()), it.value());
    for (const auto &[surface, finding] : exceptions)
        std::printf("  例外（不计数）%-24s %-18s %-28s %s\n", qPrintable(surface), qPrintable(finding.cls),
                    qPrintable(finding.name), qPrintable(finding.detail));
    // 首帧分布：>50 ms 的进 B 类候选（口径见 SurfaceRecord::paintMs 的注释）。
    QList<SurfaceRecord> slow;
    for (const SurfaceRecord &rec : sweep.records()) {
        if (rec.paintMs > 50.0 || rec.openMs > 50.0) slow.append(rec);
    }
    std::sort(slow.begin(), slow.end(), [](const SurfaceRecord &a, const SurfaceRecord &b) {
        return std::max(a.paintMs, a.openMs) > std::max(b.paintMs, b.openMs);
    });
    std::printf("\n首帧 > 50 ms 的表面：%d 个\n", int(slow.size()));
    for (const SurfaceRecord &rec : slow) {
        std::printf("  %-40s paint %6.1f ms   open %6.1f ms\n", qPrintable(rec.key), rec.paintMs,
                    rec.openMs);
    }
    if (!shotDir.isEmpty()) std::printf("样张与清单：%s\n", qPrintable(shotDir));

    // 有新残留就红。例外不能在这里长：只认 SurfaceScan::checkBoxException()，每条带理由。
    if (!residue.isEmpty()) {
        std::printf("\nFAIL 计入的残留 %d 处（修掉，或在 SurfaceScan::checkBoxException() 里登记例外并写理由）：\n",
                    int(residue.size()));
        for (const auto &[surface, finding] : residue)
            std::printf("  %-30s %-20s %-22s %-28s %s\n", qPrintable(surface), qPrintable(finding.kind),
                        qPrintable(finding.cls), qPrintable(finding.name), qPrintable(finding.detail));
        std::printf("surface-sweep: FAILED（%d 处计入的残留）\n", int(residue.size()));
        return 1;
    }
    std::printf("surface-sweep: PASS（0 处计入，%d 条例外）\n", int(exceptions.size()));
    return 0;
}

namespace {

void fillHome(MainWindowUI &ui) {
    MainWindowUI::HomeQuickItem current;
    current.key = QStringLiteral("1.20.1-Forge");
    current.name = QStringLiteral("1.20.1-Forge 47.2.0");
    current.detail = QStringLiteral("上次启动 2 天前");
    current.current = true;
    MainWindowUI::HomeQuickItem favorite;
    favorite.key = QStringLiteral("1.21.4-Fabric");
    favorite.name = QStringLiteral("1.21.4-Fabric 0.16.9");
    favorite.detail = QStringLiteral("上次启动 5 小时前");
    favorite.favorite = true;
    MainWindowUI::HomeQuickItem recent;
    recent.key = QStringLiteral("1.20.4");
    recent.name = QStringLiteral("1.20.4 原版");
    recent.detail = QStringLiteral("上周");
    recent.recent = true;
    ui.setHomeQuickRows({current}, {favorite}, {recent});
}

void fillInstanceTree(MainWindowUI &ui) {
    QTreeWidget *tree = ui.getInstanceList();
    if (tree == nullptr) return;
    tree->clear();
    auto *group = new QTreeWidgetItem(tree);
    group->setText(0, QStringLiteral("1.20.1 整合包"));
    group->setData(0, Qt::UserRole, QStringLiteral("g-modded"));
    for (const QString &name : {QStringLiteral("1.20.1-Forge 47.2.0"), QStringLiteral("1.20.1-Fabric 0.16.9")}) {
        auto *item = new QTreeWidgetItem(group);
        item->setText(0, name);
        item->setData(0, Qt::UserRole, name);
    }
    auto *solo = new QTreeWidgetItem(tree);
    solo->setText(0, QStringLiteral("1.21.4 原版"));
    solo->setData(0, Qt::UserRole, QStringLiteral("1.21.4"));
    tree->expandAll();
}

}  // namespace



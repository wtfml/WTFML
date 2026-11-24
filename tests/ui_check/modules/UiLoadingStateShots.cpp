// 步骤 126 加载态接入的离屏样张：资源详情、更新向导的加载器版本框、新建服务端、文件详情、预设计划、
// 下载页「游戏」刷新，以及主页外观预览上没采用的那种叠法（留图说明）。
// 步骤 136：安装页预设弹框（点开瞬间右栏加载态 / 读不了 / 填好后模组那一行兼容检查中），文件名 r136-*。
// 每处一张「改前」（照旧写法摆出来）和一张「加载中」，能出错的再一张「出错」。
// 主题暮色雪山（dark）。用法：QT_QPA_PLATFORM=offscreen ui_check_loading_state_shots <输出目录>（紧凑运行器：ui_check_modules ui_check_loading_state_shots <输出目录>）
// 不进 ctest：只出图，不做断言。
#include "Support.hpp"

#include "InstanceFilesPage.hpp"
#include "LoadingState.hpp"
#include "MainWindowUI.hpp"
#include "NewServerPage.hpp"
#include "PresetInstallPicker.hpp"
#include "PresetManagePage.hpp"
#include "ResourceDetailPage.hpp"
#include "SkinViewer.hpp"
#include "Theme.hpp"

#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

void pump(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// 过了 150ms 门槛、淡入走完（点停在一个看得清的相位上）。
void shown() { pump(Anim::loadingDelayMs() + Anim::appearMs() + 120); }

}  // namespace

int UiLoadingStateShots(QApplication &app) {
    const QString fontPath = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
    const int fontId = QFile::exists(fontPath) ? QFontDatabase::addApplicationFont(fontPath) : -1;
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    app.setFont(QFont(families.isEmpty() ? QStringLiteral("Microsoft YaHei UI") : families.first(), 9));
    // 输出目录 = 带路径分隔符的那个参数（紧凑运行器的第一个参数是模块名）。
    QString out = QDir::current().filePath(QStringLiteral("build/r126-shots"));
    for (const QString &arg : app.arguments().mid(1))
        if (arg.contains(QLatin1Char('/')) || arg.contains(QLatin1Char('\\'))) out = arg;
    QDir().mkpath(out);
    Theme::setSpec(Theme::presetSpec(QStringLiteral("dark")));
    Theme::apply(app);
    Anim::setEnabled(true);

    int saved = 0;
    const auto save = [&](QWidget *w, const QString &name) {
        if (w->grab().save(QDir(out).filePath(QStringLiteral("r126-%1.png").arg(name)))) ++saved;
    };

    // ---- 1 资源详情页 ----
    {
        ResourceDetailPage page;
        ResourceDetail::Header header;
        header.title = QStringLiteral("钠");
        header.description = QStringLiteral("现代化的渲染引擎，大幅提升帧率");
        header.metaLine = QStringLiteral("Fabric / NeoForge / 下载 2.31 亿次 / Modrinth");
        page.setHeader(header);
        page.setChips({{QString(), QStringLiteral("全部")}, {QStringLiteral("1.21.4"), QStringLiteral("1.21.4")}},
                      QString());
        page.resize(900, 620);
        page.show();
        pump(50);
        page.setPlaceholder(QStringLiteral("正在读版本列表…"));
        pump(50);
        save(&page, QStringLiteral("1-resource-detail-before"));
        page.setLoading(QStringLiteral("正在读版本列表…"));
        shown();
        save(&page, QStringLiteral("1-resource-detail-loading"));
        page.setFailed(QStringLiteral("没取到版本列表：连接超时"), [] {});
        pump(Anim::appearMs() + 80);
        save(&page, QStringLiteral("1-resource-detail-error"));
    }

    // ---- 2 更新向导的加载器版本框（框由 src/InstanceUpdateFlow 搭，这里照它的形状摆一行） ----
    {
        QWidget card;
        card.setAttribute(Qt::WA_StyledBackground, true);
        card.setStyleSheet(QStringLiteral("QWidget#r126card{background:%1;}").arg(Theme::tokens().page.name()));
        card.setObjectName(QStringLiteral("r126card"));
        auto *form = new QFormLayout(&card);
        form->setContentsMargins(24, 24, 24, 24);
        form->setSpacing(12);
        auto *game = new QComboBox(&card);
        game->addItem(QStringLiteral("1.21.4"));
        auto *loader = new QComboBox(&card);
        loader->addItem(QStringLiteral("Fabric"));
        auto *version = new QComboBox(&card);
        version->setMinimumWidth(260);
        form->addRow(QStringLiteral("目标 Minecraft"), game);
        form->addRow(QStringLiteral("加载器"), loader);
        form->addRow(QStringLiteral("加载器版本"), version);
        card.resize(520, 190);
        card.show();
        pump(50);
        version->addItem(QStringLiteral("正在获取版本…"));
        pump(30);
        save(&card, QStringLiteral("2-update-loader-before"));
        version->clear();
        LoadingState::begin(version, QStringLiteral("正在获取版本…"));
        shown();
        save(&card, QStringLiteral("2-update-loader-loading"));
        LoadingState::fail(version, MainWindowUI::loaderVersionFailedText(), [] {});
        pump(Anim::appearMs() + 80);
        save(&card, QStringLiteral("2-update-loader-error"));
    }

    // ---- 3 新建服务端（Fabric 档：加载器 / 安装器两格都在等） ----
    {
        NewServerPage page;
        NewServerPage::FlavorOption vanilla;
        vanilla.token = QStringLiteral("vanilla");
        vanilla.name = QStringLiteral("原版");
        NewServerPage::FlavorOption fabric;
        fabric.token = QStringLiteral("fabric");
        fabric.name = QStringLiteral("Fabric");
        fabric.needsLoader = true;
        fabric.needsInstaller = true;
        page.setFlavors({vanilla, fabric}, QStringLiteral("fabric"));
        page.resize(900, 700);
        page.show();
        pump(50);
        // 改前：建框时写死的占位「正在获取…」。
        for (const QString &name : {MainWindowUI::newServerLoaderEditName(), MainWindowUI::newServerInstallerEditName()}) {
            if (auto *combo = page.findChild<QComboBox *>(name + "Combo")) {
                combo->setPlaceholderText(QStringLiteral("正在获取…"));
                combo->lineEdit()->setPlaceholderText(QStringLiteral("正在获取…"));
            }
        }
        pump(30);
        save(&page, QStringLiteral("3-new-server-before"));
        page.setVersionsLoading(false);
        page.setVersionsLoading(true);
        shown();
        save(&page, QStringLiteral("3-new-server-loading"));
    }

    // ---- 4 文件详情那一行 ----
    {
        QTemporaryDir dir;
        for (const char *name : {"城堡.litematic", "地铁站.schem", "仓库.nbt"}) {
            QFile file(dir.filePath(QString::fromUtf8(name)));
            if (file.open(QIODevice::WriteOnly)) file.write("x");
        }
        InstanceFilesPage page;
        page.resize(760, 420);
        page.show();
        page.setDirectory(dir.path());
        page.setInfoPanelEnabled(true);
        pump(50);
        page.list()->setCurrentRow(0);
        auto *info = page.findChild<QLabel *>(InstanceFilesPage::infoLabelName());
        // 改前：直接写「正在读取…」。
        LoadingState::finish(info);
        info->setText(InstanceFilesPage::infoPendingText());
        pump(30);
        save(&page, QStringLiteral("4-file-info-before"));
        info->clear();
        LoadingState::begin(info, InstanceFilesPage::infoPendingText());
        shown();
        save(&page, QStringLiteral("4-file-info-loading"));
    }

    // ---- 5 预设应用：生成计划 ----
    {
        PresetManagePage page;
        page.resize(1000, 760);
        page.show();
        pump(50);
        auto *plan = page.findChild<QListWidget *>(PresetManagePage::planListName());
        QWidget *area = plan != nullptr ? plan->parentWidget() : &page;
        page.setPlanLoading(true);
        // 改前：计划那格空着，底下一行「正在读取实例并生成计划…」。
        LoadingState::finish(plan);
        const QList<QLabel *> labels = area->findChildren<QLabel *>(Qt::FindDirectChildrenOnly);
        QLabel *hint = nullptr;
        for (QLabel *label : labels)
            if (plan != nullptr && label->y() > plan->y() && label->text().isEmpty()) hint = label;
        if (hint != nullptr) hint->setText(QStringLiteral("正在读取实例并生成计划…"));
        pump(30);
        save(area, QStringLiteral("5-preset-plan-before"));
        if (hint != nullptr) hint->clear();
        LoadingState::begin(plan, QStringLiteral("正在读取实例并生成计划…"));
        shown();
        save(area, QStringLiteral("5-preset-plan-loading"));
    }

    // ---- 6 下载页「游戏」页头刷新 / 7 主页外观预览 ----
    {
        QMainWindow window;
        MainWindowUI ui(&window);
        window.resize(1280, 800);
        window.show();
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
        if (QListWidget *categories = ui.getDownloadCategoryList()) categories->setCurrentRow(MainWindowUI::downloadCategoryGameIndex());
        if (QTreeWidget *tree = ui.getVersionTree()) {
            for (const char *group : {"正式版", "快照", "远古版", "愚人节版"}) {
                auto *item = new QTreeWidgetItem(tree);
                item->setText(0, QString::fromUtf8(group));
            }
        }
        pump(300);
        save(&window, QStringLiteral("6-game-refresh-before"));
        LoadingState::begin(ui.getVersionRefreshStatus(), MainWindowUI::versionRefreshingText());
        shown();
        save(&window, QStringLiteral("6-game-refresh-loading"));
        LoadingState::finish(ui.getVersionRefreshStatus());

        ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
        pump(300);
        save(&window, QStringLiteral("7-skin-before"));
        LoadingState::begin(ui.getSkinViewer(),
                            MainWindowUI::skinHintLoadingText(QStringLiteral("Steve")));
        shown();
        save(&window, QStringLiteral("7-skin-overlay-rejected"));  // 没采用：字压在人物身上看不清
    }

    // ---- r136 预设弹框 ----
    {
        const auto save136 = [&](QWidget *w, const QString &name) {
            if (w->grab().save(QDir(out).filePath(QStringLiteral("r136-%1.png").arg(name)))) ++saved;
        };
        QWidget card;
        card.setObjectName(QStringLiteral("r136card"));
        card.setAttribute(Qt::WA_StyledBackground, true);
        card.setStyleSheet(QStringLiteral("QWidget#r136card{background:%1;}").arg(Theme::tokens().page.name()));
        auto *layout = new QVBoxLayout(&card);
        layout->setContentsMargins(24, 24, 24, 24);
        auto *picker = new PresetInstallPicker(&card);
        layout->addWidget(picker);
        picker->setContext(QStringLiteral("1.21.4"), QStringLiteral("Fabric"));
        card.resize(848, 520);
        card.show();
        picker->beginLoading();
        shown();
        save136(&card, QStringLiteral("1-preset-picker-loading"));
        picker->failLoading(QStringLiteral("预设文件夹读不了：D:\\Games\\.minecraft\\wtfml\\presets"), [] {});
        pump(Anim::appearMs() + 80);
        save136(&card, QStringLiteral("2-preset-picker-error"));
        PresetInstallPicker::Category mods;
        mods.key = QStringLiteral("mods");
        mods.title = QStringLiteral("模组");
        mods.options.append({QStringLiteral("daily"), QStringLiteral("日常配置"), true, true, 42});
        mods.options.append({QStringLiteral("build"), QStringLiteral("建筑辅助"), true, false, 18});
        mods.options.append({QStringLiteral("perf"), QStringLiteral("性能优化"), true, false, 9});
        mods.selectedId = QStringLiteral("build");
        mods.status = QStringLiteral("检查中 6/18");
        PresetInstallPicker::Category game;
        game.key = QStringLiteral("gameSettings");
        game.title = QStringLiteral("游戏设置");
        game.options.append({QStringLiteral("daily-game"), QStringLiteral("日常配置"), true, true, 12});
        PresetInstallPicker::Category packs;
        packs.key = QStringLiteral("resourcePacks");
        packs.title = QStringLiteral("资源包");
        picker->setCategories({game, mods, packs});
        picker->setComposites({});
        if (auto *nav = picker->findChild<QListWidget *>(PresetInstallPicker::tabBarName())) nav->setCurrentRow(1);
        picker->setChecking(QStringLiteral("mods"), true);
        shown();
        save136(&card, QStringLiteral("3-preset-picker-checking"));
        picker->setStatus(QStringLiteral("mods"), QStringLiteral("缺失 2 项"), {}, true, 18, 2, 0);
        picker->setChecking(QStringLiteral("mods"), false);
        pump(80);
        save136(&card, QStringLiteral("4-preset-picker-filled"));
    }

    std::printf("saved %d shots to %s\n", saved, qPrintable(QDir::toNativeSeparators(out)));
    return saved > 0 ? 0 : 1;
}

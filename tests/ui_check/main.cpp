#include "modules/Support.hpp"
#include "support/SettingsIsolation.hpp"
#include <QFile>
#include <QIcon>
#include "AppIcon.hpp" // 产品和验证工程用的是同一个函数，见 ui/AppIcon.hpp


int main(int argc, char *argv[]) {
    // 输出不缓冲：这个工程是靠 printf 报进度的，一旦中间崩了（段错误 / 断言失败被
    // 平台层接手），缓冲区里那几 KB 会跟着进程一起没，于是"崩在哪一条"就查不出来了。
    // 关掉缓冲之后崩溃点前面那条 PASS 就是最后一条。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QApplication app(argc, argv);
    PopupWindowGuard popupGuard(app);  // 防漏：每块顶层浮层一上屏就验无边框 + 半透明
    app.setFont(Theme::textFont(app.font(), Theme::TextRole::Body));
    Q_INIT_RESOURCE(icons);
    QTemporaryDir processSandbox;
    if (!processSandbox.isValid()) return 2;
    app.setProperty("wtfml.testDefaultMinecraftDir", processSandbox.filePath(".minecraft"));
    app.setProperty("wtfml.testAgentDataDir", processSandbox.filePath("agent"));

    // ---------- 设置沙盒：必须在**第一个 QSettings 对象出现之前**重定向 ----------
    // 这个工程量的是**一套固定的基线外观**（预设见下面 `Theme::setSpec` 那一句、一项
    // 覆盖都没有），
    // 而 Theme / 样式表那一层读的正是 QSettings。不重定向的话，跑这一趟的人自己在启动器里
    // 挑过的主题（预设 / 主题色 / 背景图）会原样灌进来 —— 于是同一个 HEAD 在开发机上
    // 一片红、在 CI 上全绿，红的还偏偏是"出厂色对不对"那一类断言（见 W1 / W12 / Z11）。
    // org/app 两个都要设：INI 后端也会因 org 为空而 setStatus(AccessError)。
    {
        const QDir root(processSandbox.path());
        const QString settingsDir = root.filePath(QStringLiteral("ui-check-settings"));
        QDir(settingsDir).removeRecursively();
        QDir().mkpath(settingsDir);
        QApplication::setOrganizationName(QStringLiteral("WTFML-check"));
        QApplication::setApplicationName(QStringLiteral("ui_check"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root.filePath("system-settings"));
        // 每趟都从"从没打开过启动器"开始（和 mw_check 那份沙盒同一个形状）：
        // 上一趟 W 段自己存进去的预设不许活到这一趟，否则第二次跑就又是一片红。
        QFile::remove(settingsDir + QStringLiteral("/WTFML-check/ui_check.ini"));
        requireIsolatedSettings(processSandbox.path(), "ui_check");
    }
    if (app.arguments().contains(QStringLiteral("--bootstrap-check"))) {
        const QStringList icons{QStringLiteral("fabric"), QStringLiteral("forge"),
                                QStringLiteral("neoforge"), QStringLiteral("optifine")};
        for (const QString &icon : icons) {
            if (QPixmap(QStringLiteral(":/assets/loader/%1.png").arg(icon)).isNull()) return 1;
        }
        // 应用图标（#74）：四档 PNG 都在 qrc 里，同一个函数建出来的 QIcon 四档都报得出来。
        // 运行时窗口用的就是这一份；exe 自己那份（assets/app/wtfml.ico）由 res/wtfml.rc 编进
        // 资源，验证工程不链它 —— 那一份靠对构建出来的 exe 做 ExtractAssociatedIcon 看。
        const QIcon appIcon = wtfmlWindowIcon();
        if (appIcon.isNull()) return 1;
        const QList<QSize> appIconSizes = appIcon.availableSizes();
        for (const int size : {16, 32, 48, 256}) {
            if (!QFile::exists(QStringLiteral(":/assets/app/wtfml-%1.png").arg(size))) return 1;
            if (!appIconSizes.contains(QSize(size, size))) return 1;
        }
        std::puts("PASS historical UI driver links, starts, and loads shared icon resources and the app icon (16/32/48/256)");
        return 0;
    }

    if (app.arguments().contains(QStringLiteral("--language-check"))) {
        QMainWindow window;
        MainWindowUI ui(&window);
        UiLanguageChecks(ui, ui.getTabWidget());
        return g_fail == 0 ? 0 : 1;
    }

    if (app.arguments().contains(QStringLiteral("--install-layout-check"))) { return UiInstallLayoutCheck(app); }
    if (app.arguments().contains(QStringLiteral("--armor-trim-check"))) return runArmorTrimChecks(app);

    if (app.arguments().contains(QStringLiteral("--toolbox-polish-check"))) { return UiToolboxPolishCheck(app); }

    if (app.arguments().contains(QStringLiteral("--resource-packs-check"))) { return UiResourcePacksCheck(app); }

    if (app.arguments().contains(QStringLiteral("--lifecycle-check"))) { return UiLifecycleCheck(app); }

    if (app.arguments().contains(QStringLiteral("--ui-refresh-check"))) { return UiUiRefreshCheck(app); }

    if (app.arguments().contains(QStringLiteral("--theme-check"))) { return UiThemeCheck(app); }
    if (app.arguments().contains(QStringLiteral("--popup-check"))) { return UiPopupCheck(app); }

    // 把 Qt 的无障碍那一层**关掉**：offscreen 插件下它是恒开的，于是每个控件被藏起来 /
    // 析构时都要走一趟 `QAccessible::updateAccessibility`，而这个工程建/销的控件比产品
    // 一整个会话还多。产品跑的是 windows 平台插件、A11y 只有读屏软件连着才开，所以这是
    // **验证环境专属**的一段路。
    // 2026-10-24 复核：这**就是**历史 `c0000374` 的隔离层，不是“靠重链接翻身”。把这一句
    // 改成 `setActive(true)`，同一条 exe 连续 10/10 崩在
    // `InstanceSettingsDialog::~InstanceSettingsDialog()` 的 `QHash<QString, QString>`
    // 释放（`QArrayData::deref()`）；保持 `false` 连跑 50/50 全绿。崩溃现场见
    // `tests/ui_check/modules/Support.hpp` 文件头与 `tools/stress-ui-offscreen.ps1`。
    // 这一句不影响任何一条断言（这个工程量的是布局 / 几何 / 颜色 / 话术）。
    QAccessible::setActive(false);

    // 动效：**先开着把界面搭出来**。理由是有一条判据只有这么量得到 ——
    // "产品有没有把该接的地方接上"（展开折叠那几棵树），而漏了那一处的症状是"不报错、
    // 不崩、界面上看着也正常，只是那一棵树展开时没有动画"。搭完立刻关掉：下面整段量的
    // 都是界面本身长什么样，不是它动的时候长什么样（规矩见 ui/Animations.hpp）。
    // NOTE: 搭界面的这一路**不会**因此冒出动画来：`pageAppear()` 只在"那一页已经露着头"
    // 的时候才动，而这一行到 `window.show()` 之间那些页都还没上屏。
    Anim::setEnabled(true);

    // 必须和 src/main.cpp 钉同一个样式。下面几条断言比的是 sizeHint()——那是**样式度量**
    // 算出来的（PM_ComboBoxFrameWidth、箭头宽度等），换个样式数值就变。不在这里钉住的话，
    // 验证跑的是系统原生样式、产品跑的是 Fusion，测的就不是要发的那份东西了。
    QApplication::setStyle(Theme::createStyle());

    // 这一套像素基线的底：**冰川碧湖那一支**（浅色）。历史断言量的是浅色那身 —— 白底上的墨、
    // 深像素扫描、日志窗的浅色表 —— 出厂默认换成暮色雪山之后它们不该跟着 `Theme::load()` 走。
    // 出厂那一份（暮色雪山）由 W1 / W8 / W9 / 收尾 和 `ui_check_review_shots` 的 default
    // 那一趟单独验。只设规格、不 `apply()`：和改默认之前一样，调色板仍是 Fusion 那份浅色，
    // 换掉它会把整份基线的像素重新洗一遍。
    Theme::setSpec(Theme::presetSpec(QStringLiteral("emerald")));

    QMainWindow window;
    MainWindowUI ui(&window);

    // ---------- ⑬ 动效那一层：产品有没有真的接上 ----------
    // 这一条**只能在这儿量**（搭完、`show()` 之前）。症状很轻：漏掉一处不报错、不崩、
    // 界面上一个像素都不差，只有"那一棵树展开的时候没有动画"。量的就是展开折叠那一个
    // 开关 —— 它是 `Anim::animateExpansion()` 唯一动的那一位。
    {
        const QStringList resourceKeys = MainWindowUI::downloadCategoryKeys();
        const int treesAnimated =
            (ui.getInstanceList() != nullptr && ui.getInstanceList()->isAnimated() ? 1 : 0) +
            (ui.getVersionTree() != nullptr && ui.getVersionTree()->isAnimated() ? 1 : 0) +
            (ui.getLoaderTree() != nullptr && ui.getLoaderTree()->isAnimated() ? 1 : 0);
        check(treesAnimated == 3,
              "★ ⑬ 界面里那三棵会展开折叠的树都接上了展开动画（组行 / 版本树 / 加载器树）",
              QStringLiteral("接上了 %1 棵（该是 3 棵）").arg(treesAnimated));

        int resourceTreesAnimated = 0;
        for (const QString &key : resourceKeys) {
            QTreeWidget *tree = ui.getDownloadResourceTree(key);
            if (tree != nullptr && tree->isAnimated()) {
                ++resourceTreesAnimated;
            }
        }
        check(resourceTreesAnimated > 0,
              "★ ⑬ 下载页那几张资源表也接上了（同一条路，见 MainWindowUI 里那个循环）",
              QStringLiteral("接上了 %1 张").arg(resourceTreesAnimated));
    }

    // 量完了，关掉：下面整段都要量"界面本身长什么样"。
    Anim::setEnabled(false);

    // 几何只有在真正布局之后才有效
    window.resize(900, 620);
    window.show();
    // 155 起每次启动导航先收起；这一串历史检查按「用户没收起」写（窄窗照样强制折，放宽就展开）。
    ui.titleBar()->setCollapsed(false);
    QApplication::processEvents();
    QComboBox *installed = ui.getInstalledVersionCombo();
    QPushButton *startBtn = ui.getStartButton();
    // 下载页那两层：[0] 版本树（四个分类 + 底下的版本号）→ [1] 安装页。
    // 原来那对「可安装版本」下拉框 + 「版本下载」列表 + 「下载版本」按钮整组去掉了 ——
    // 要求是"点开后展开对应版本，再点击具体版本后进入一个新页面"。
    QStackedWidget *gameStack = ui.getGameStack();
    QTreeWidget *versionTree = ui.getVersionTree();
    QLineEdit *installNameEdit = ui.getInstallNameEdit();
    // 名字不能用时那行**红字**。名字框底下那句灰色说明已经删了（用户点名是废话）。
    QLabel *installNameError = ui.getInstallNameError();
    QLabel *installTargetLabel = ui.getInstallTargetLabel();
    QPushButton *installBtn = ui.getInstallButton();
    QPushButton *installBackBtn = ui.getInstallBackButton();
    // 安装页左栏那棵加载器树（ 起是树，不再是五个单选钮 + 四个组合框）。
    QTreeWidget *loaderTree = ui.getLoaderTree();
    QListWidget *folderList = ui.getFolderList();
    // 实例列表是**树**（分组要求两层）；可下载版本列表仍是列表。两个类型不一样是
    // 刻意的，别为了"统一"把它们塞进同一个变量。
    QTreeWidget *instanceList = ui.getInstanceList();
    QLabel *instanceFolderLabel = ui.getInstanceFolderLabel();
    QPushButton *addFolderBtn = ui.getAddFolderButton();
    QTabWidget *tabs = ui.getTabWidget();
    QFrame *bar = ui.getBottomBar();

    std::printf("===== A. 选项卡结构 =====\n");
    check(installed != nullptr, "「已安装版本」那个下拉框还在");
    // objectName 是**给验证工程的接口**：mw_check 靠它认控件。按占位文案
    // 「暂无已安装版本」认不行 —— 那个文案会随上下文在两种之间切换，那种测试会在
    // 产品改动时以"控件找不到、后面全不跑"的方式挂掉。
    check(installed->objectName() == MainWindowUI::installedVersionComboName(),
          "已安装下拉框的 objectName 是约定的那个", installed->objectName());
    // 下载页那一组新控件：名字找得回来，才谈得上后面的结构断言。
    check(gameStack != nullptr && gameStack->objectName() == MainWindowUI::downloadGameStackName(),
          "游戏页那两层（版本树/安装页）找得回来（按 objectName）",
          gameStack != nullptr ? gameStack->objectName() : QStringLiteral("<null>"));
    check(versionTree != nullptr && versionTree->objectName() == MainWindowUI::versionTreeName(),
          "版本树找得回来（按 objectName）",
          versionTree != nullptr ? versionTree->objectName() : QStringLiteral("<null>"));
    check(installNameEdit != nullptr &&
              installNameEdit->objectName() == MainWindowUI::installNameEditName(),
          "版本名称输入框找得回来（按 objectName）",
          installNameEdit != nullptr ? installNameEdit->objectName() : QStringLiteral("<null>"));
    check(installTargetLabel != nullptr &&
              installTargetLabel->objectName() == MainWindowUI::installTargetLabelName(),
          "「将基于原版 X 安装」那个标签找得回来（按 objectName）",
          installTargetLabel != nullptr ? installTargetLabel->objectName()
                                        : QStringLiteral("<null>"));
    check(installBtn != nullptr && installBtn->objectName() == MainWindowUI::installButtonName(),
          "「安装」按钮找得回来（按 objectName）",
          installBtn != nullptr ? installBtn->objectName() : QStringLiteral("<null>"));
    check(installBackBtn != nullptr &&
              installBackBtn->objectName() == MainWindowUI::installBackButtonName(),
          "「← 返回版本列表」找得回来（按 objectName）",
          installBackBtn != nullptr ? installBackBtn->objectName() : QStringLiteral("<null>"));

    check(tabs != nullptr, "主窗口里有选项卡控件");

    if (const int result = UiNavigationChecks(window, ui, installed, startBtn, gameStack, versionTree, installNameEdit, installBtn, installBackBtn, loaderTree, folderList, instanceList, instanceFolderLabel, tabs); result != 0) return result;
    if (const int result = UiLanguageChecks(ui, tabs); result != 0) return result;

    std::printf("===== B. 分组归属 =====\n");
    // 往上找最近的那个 QGroupBox：下拉框和按钮是不是同一组，看的就得是这个。
    // nearestGroupBox lives in the shared fixture.
    // ---- 页面骨架那两个"面"（ 改版）----
    //
    // 骨架是 `buildTwoColumnPage()` 搭的：侧栏一个面（`wtfmlSurface="sidebar"`）、正文一个面
    // （`"workspace"`），小节卡片是第三个值（`"section"`），安装页底下那条固定页脚是
    // 第四个（`"footer"`）。属性名和值都只有 `ui/Theme.*` 一处出处 —— 验证工程这边
    // **不写字面量**，理由和颜色一个规矩（写死的话改一处就会漏一处）。
    const std::function<QString(QWidget *)> surfaceValueOf = [](QWidget *w) -> QString {
        const QByteArray key = Theme::surfacePropertyName().toUtf8();
        for (QWidget *p = w; p != nullptr; p = p->parentWidget()) {
            const QVariant value = p->property(key.constData());
            if (value.isValid() && !value.toString().isEmpty()) {
                return value.toString();
            }
        }
        return QString();
    };
    // 这一支上最近的**那个面**是不是 @p value 那一档（"它长在侧栏面上 / 正文面上"）。
    const std::function<bool(QWidget *, const QString &)> inSurface = [&surfaceValueOf](QWidget *w, const QString &value) {
        return w != nullptr && surfaceValueOf(w) == value;
    };
    // 这一页里那个面（侧栏 / 正文）：按属性找，找到的第一个。
    const std::function<QFrame *(QWidget *, const QString &)> surfaceOf = [](QWidget *root, const QString &value) -> QFrame * {
        const QByteArray key = Theme::surfacePropertyName().toUtf8();
        for (QFrame *frame : root->findChildren<QFrame *>()) {
            if (frame->property(key.constData()).toString() == value) {
                return frame;
            }
        }
        return nullptr;
    };
    // 「版本下载」那个组随着它那对控件（列表 + 下载按钮）一起没了：现在这一页是
    // 版本树 → 安装页两层，安装页上唯一的组是「模组加载器」（四个加载器各一条）。
    // 这里改钉**那个组**的归属：四个钮 + 四条版本组合框都该在同一条里、同处这一组。
    QGroupBox *downloadGroup = nearestGroupBox(installBtn);
    check(downloadGroup == nullptr, "「安装」按钮不在任何组里（它作用于整页，不属于某一组）");
    check(nearestGroupBox(installed) == nullptr, "已安装下拉框也不在组里");
    {
        check(nearestGroupBox(loaderTree) == nullptr,
              "加载器那棵树**不再**长在任何 QGroupBox 里（那一组整块换成小节卡片了）");
        check(inSurface(loaderTree, Theme::sectionSurfaceValue()),
              "它长在一个 `wtfmlSurface=\"section\"` 的小节面上",
              surfaceValueOf(loaderTree));
        QWidget *loaderCard = loaderTree != nullptr ? loaderTree->parentWidget() : nullptr;
        // 卡片顶上那一行标题（粗体 + `sectionTitle` 那一档角色）就是当年那组的标题。
        const auto *cardTitle = loaderCard != nullptr
                                    ? loaderCard->findChild<QLabel *>(QString(), Qt::FindDirectChildrenOnly)
                                    : nullptr;
        check(cardTitle != nullptr && cardTitle->text() == MainWindowUI::modLoaderLabelText() &&
                  cardTitle->font().bold(),
              "卡片顶上那行小标题就是「模组加载器」（粗体，和设置页那些小节一个形状）",
              cardTitle != nullptr ? cardTitle->text() : QStringLiteral("<null>"));
        // 那一节里**只有这一棵树**：原来那五个钮、四个组合框连它们各自的行容器一起没了。
        // 这条是"旧控件真的删干净了"的凭据 —— 留着哪怕一个空壳，用户也可能点到它。
        check(loaderCard != nullptr &&
                  loaderCard->findChildren<QRadioButton *>().isEmpty() &&
                  loaderCard->findChildren<QComboBox *>().isEmpty() &&
                  loaderCard->findChildren<QTreeWidget *>().size() == 1,
              "这一节里没有单选钮、没有组合框，只有一棵树",
              loaderCard != nullptr
                  ? QString("radio=%1 combo=%2 tree=%3")
                        .arg(loaderCard->findChildren<QRadioButton *>().size())
                        .arg(loaderCard->findChildren<QComboBox *>().size())
                        .arg(loaderCard->findChildren<QTreeWidget *>().size())
                  : QStringLiteral("<null>"));
        // 树**自己不画那圈边**（用户那份文档第一条要减掉的"框套框"）：它四周已经是
        // 那张卡片了，再套一圈就是两条平行的框。
        check(loaderTree != nullptr && loaderTree->frameShape() == QFrame::NoFrame,
              "这棵树自己不画边框（四周已经是那张卡片）",
              loaderTree != nullptr ? QString::number(int(loaderTree->frameShape()))
                                    : QStringLiteral("<null>"));
        // 五条各带一张标志图（用户要的"对应的 forge neo... 也加上其标志"）：
        // 第 i 条上那张必须**正好是**第 i 个加载器的标志 —— 差一位的表现是
        // "Forge 那条上画着 NeoForge 的狐狸"，图标这东西没人会去逐条核对，只能靠这条钉住。
        const int iconSize = MainWindowUI::versionTreeIconSize();
        bool iconsWired = loaderTree != nullptr &&
                          loaderTree->topLevelItemCount() == 7;
        for (int i = 0; loaderTree != nullptr && i < loaderTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *row = loaderTree->topLevelItem(i);
            const int id = row->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt();
            if (id < 0 || id == MainWindowUI::modLoaderNoneIndex()) {
                continue;  // 「预设」和「不装」各自有自己的图（下面分开验）
            }
            const QIcon icon = row->icon(MainWindowUI::loaderTreeKeyColumn());
            iconsWired = iconsWired && !icon.isNull() &&
                         icon.pixmap(iconSize, iconSize).toImage() ==
                             VersionIcons::loaderMark(id, iconSize).toImage();
        }
        check(iconsWired, "五条加载器上各是自己那个加载器的标志（图标和下标一一对应，没串行）",
              QStringLiteral("size=%1").arg(iconSize));
        const QIcon noneIcon =
            loaderTree != nullptr && loaderTree->topLevelItemCount() == 7
                ? loaderTree->topLevelItem(MainWindowUI::modLoaderNoneIndex() + 1)
                      ->icon(MainWindowUI::loaderTreeKeyColumn())
                : QIcon();
        check(!noneIcon.isNull() &&
                  noneIcon.pixmap(iconSize, iconSize).toImage() ==
                      VersionIcons::grassBlock(iconSize).toImage(),
              "「不装（纯原版）」那条画的是草方块（它装出来的就是原版，和版本号前面那个一样）");
    }
    QGroupBox *folderGroup = nearestGroupBox(folderList);
    // ---- 实例页：侧栏（ 起这一栏不再有 QGroupBox）----
    //
    // 用户那份改版文档第一条就是「减少框」：「页面本身不画边框」「左侧栏只使用轻微底色
    // 区分（不画完整矩形边框）」。于是「游戏文件夹」那个组整块撤掉：
    // · 标题变成侧栏顶上**一行普通的粗体字**（`folderSectionTitleName()`，sectionTitle
    // 那一档角色）—— 不再有缺口式标题、不再有那圈边；
    // · 列表和两颗按钮直接落在侧栏那个"面"（`wtfmlSurface="sidebar"`）上。
    // 下面这几条钉的就是这个形状：**一整个面上有什么**，而不是"某一组里有什么"。
    // UI 7b（设计稿 manage-*）：左侧文件夹栏收起，文件夹列表 + 小标题 +「添加文件夹…」
    // 搬进页头那颗胶囊点开的浮层（instanceFolderPopup）；「导入整合包…」挪到页头右上。
    QWidget *instancePageForSidebar = tabs->widget(MainWindowUI::instancesTabIndex());
    QWidget *instanceSidebar = instancePageForSidebar != nullptr
                                   ? instancePageForSidebar->findChild<QWidget *>(MainWindowUI::instanceFolderPopupName())
                                   : nullptr;
    check(folderGroup == nullptr && instanceSidebar != nullptr && isDescendantOf(folderList, instanceSidebar),
          "文件夹列表在页头胶囊点开的浮层里（不再是「游戏文件夹」那一组）");
    check(instanceSidebar != nullptr && isDescendantOf(addFolderBtn, instanceSidebar),
          "「添加文件夹…」和列表同处那个浮层");
    {
        auto *title = instanceSidebar != nullptr ? instanceSidebar->findChild<QLabel *>(
                                                       MainWindowUI::folderSectionTitleName())
                                                : nullptr;
        check(title != nullptr && title->text() == MainWindowUI::folderSectionTitleText() &&
                  title->font().bold() &&
                  title->property(Theme::textRolePropertyName().toUtf8().constData())
                          .toString() == Theme::sectionTitleTextRoleValue(),
              "浮层顶上那行小标题是「游戏文件夹」（粗体 + sectionTitle 那一档角色）",
              title != nullptr ? title->text() : QStringLiteral("<null>"));
        QLayout *leftLayout = instanceSidebar != nullptr ? instanceSidebar->layout() : nullptr;
        check(leftLayout != nullptr && title != nullptr && folderList != nullptr &&
                  leftLayout->indexOf(title) >= 0 && leftLayout->indexOf(folderList) >= 0 &&
                  leftLayout->indexOf(title) < leftLayout->indexOf(folderList),
              "那行小标题在列表**上面**");
    }
    check(instanceSidebar != nullptr &&
              instanceSidebar->findChildren<QPushButton *>().size() == 1,
          "浮层里只有「添加文件夹…」一颗按钮（「移除文件夹…」已经删掉，「导入整合包…」去了页头）",
          QString::number(instanceSidebar != nullptr
                              ? instanceSidebar->findChildren<QPushButton *>().size()
                              : -1));
    check(addFolderBtn != nullptr && addFolderBtn->text() == MainWindowUI::addFolderText(),
          "那个按钮是「添加文件夹…」",
          addFolderBtn != nullptr ? addFolderBtn->text() : QStringLiteral("<null>"));
    check(addFolderBtn != nullptr &&
              addFolderBtn->objectName() == MainWindowUI::addFolderButtonName(),
          "「添加文件夹…」挂着自己的 objectName",
          addFolderBtn != nullptr ? addFolderBtn->objectName() : QStringLiteral("<null>"));
    // 「删干净了没有」光靠上面那句"按钮数 == 2"是不够的（以后那一栏里加别的按钮它就假了）。
    // 这里按 objectName **反着找**：产品那头这两个名字已经删掉了，所以判据里只能写字面量
    // —— 而这两条要钉的正是"它们找不回来了"。
    check(instanceSidebar != nullptr &&
              instanceSidebar->findChild<QPushButton *>(
                  QStringLiteral("removeFolderButton")) == nullptr,
          "「移除文件夹…」那个按钮真的不在了（按 objectName 找不到）");
    check(instanceSidebar != nullptr &&
              instanceSidebar->findChild<QLabel *>(QStringLiteral("folderHintLabel")) == nullptr,
          "列表下面那段提示也真的不在了（按 objectName 找不到）");
    // 但这一栏**又有了**一段说明：「导入整合包…」下面那句。它和上面那句
    // 被删掉的话是两回事（那句讲的是"列表里这份文件夹"，这句讲的是那颗按钮收什么）。
    {
        const auto *hint = instanceSidebar != nullptr
                               ? instanceSidebar->findChild<QLabel *>(
                                     MainWindowUI::importHintLabelName())
                               : nullptr;
        check(hint == nullptr, "导入说明不再常驻侧栏");
    }
    // ---- 右栏（正文面）：就只有那一棵实例树 ----
    check(nearestGroupBox(instanceList) == nullptr &&
              inSurface(instanceList, Theme::workspaceSurfaceValue()),
          "实例列表在**正文面**上（「实例列表」那一组整块没了）",
          surfaceValueOf(instanceList));
    // 讲「导入整合包…」的那段话是**从左栏搬过来的**（不是又抄了一份）：正文面里的 QLabel
    // 不该再有那四个字 —— 两边都写一遍的话，改一处就会漏一处。
    {
        QFrame *instanceWorkspace =
            surfaceOf(instancePageForSidebar, Theme::workspaceSurfaceValue());
        bool rightStillTalksAboutImport = false;
        if (instanceWorkspace != nullptr) {
            for (const QLabel *l : instanceWorkspace->findChildren<QLabel *>()) {
                if (l->text().contains(QStringLiteral("导入整合包"))) {
                    rightStillTalksAboutImport = true;
                }
            }
        }
        check(!rightStillTalksAboutImport,
              "正文面里不再讲「导入整合包」（那段话跟着按钮留在左栏里）");
    }
    // 「当前文件夹：…」那句话**升级成了页头那行说明**：它原来在「实例列表」
    // 那一组里当第一行，现在挂在页头标题「实例」下面（用户文档：`页面说明 / 路径 / 当前
    // 状态`）。objectName 没换（`instanceFolderLabelName()`）—— MainWindow 灌数据那条路
    // 一个字符都没动，只是它现在长在页头里、不在正文面里。
    check(instanceFolderLabel != nullptr &&
              instanceFolderLabel->objectName() == MainWindowUI::instanceFolderLabelName() &&
              instanceFolderLabel->parentWidget() != nullptr &&
              !isDescendantOf(instanceFolderLabel, instanceList),
          "「当前文件夹」那句话不再挂在实例列表底下（它现在是页头那行说明）");
    // 「导入整合包…」**不在**右栏（ 之前它在）：要求「导入整合包放在
    // 左边添加文件夹下面」，所以它在左栏、紧挨着「添加文件夹…」。
    // 这里把那条老断言翻过来钉住 —— 挪回去了（或者被谁顺手加回右栏）要在这里红。
    check(isDescendantOf(ui.getImportModpackButton(),
                         instancePageForSidebar->findChild<QWidget *>(
                             MainWindowUI::rightColumnName(QStringLiteral("instance")))) &&
              !isDescendantOf(ui.getImportModpackButton(), instanceList),
          "「导入整合包…」在右栏页头（UI 7b 设计稿右上角）");
    // NOTE: 主页左栏只有那两件 —— 离线身份由"当前账号是什么"决定
    // （MainWindow::offlineIdentity()），没有「离线模式」勾选框。
    check(nearestGroupBox(startBtn) == nullptr &&
              nearestGroupBox(ui.getInstalledVersionCombo()) == nullptr,
          "启动按钮和版本下拉框都不在组里（主页是裸的）");

    std::printf("===== C. 下载页几何（900x620 下实测） =====\n");
    // 下载页不是当前页，几何要等它被显示过才有效 —— 先切过去再量，量完切回主页
    // （底栏那条规则依赖"打开就在主页"，后面的断言还要用）。
    tabs->setCurrentIndex(MainWindowUI::downloadTabIndex());
    QApplication::processEvents();
    // NOTE: 这些控件分属不同父控件（左栏、树的页、安装页、组里的布局），拿各自的
    // geometry() 直接比等于在比几套坐标系。一律换算到页面坐标上再比。
    QWidget *downloadPage = tabs->widget(MainWindowUI::downloadTabIndex());
    const std::function<QRect(QWidget *)> rectInPage = [downloadPage](QWidget *w) {
        return w != nullptr ? QRect(w->mapTo(downloadPage, QPoint(0, 0)), w->size()) : QRect();
    };

    // ---- 左栏顶上那颗「← 返回实例设置」 ----
    //
    // 要求（先）：「点击下载新模组/资源包/...时左上角保留一个返回键，返回到实例设置
    // 里面，和资源包详情啥的一致」；（后改口）「返回按钮也是放在左上角（就是左边栏的顶上）」。
    // 于是它从"五栏各一颗、摆在右栏那行筛子里"变成**整个下载页一颗、摆在左栏顶上**。
    // 这一节钉三条：**只有一颗** / **在左栏顶上** / **默认收着**。
    {
        // NOTE: 这里量的是 `isHidden()`（"有没有被显式收起来"）**不是** `isVisible()`。
        // 下载页是一个堆叠页，此刻台上站着的是「游戏」那一栏 —— 别栏的**每一个**
        // 子控件 `isVisible()` 都是假，拿它当判据等于在量"这一栏是不是当前页"，
        // 而不是量"那颗键收没收"。`isHidden()` 只说这一颗自己的显隐状态，
        // 正是"默认收着 / 从实例进来的那一次才露面"这件事的判据。
        QPushButton *back = ui.getDownloadBackButton();
        check(back != nullptr, "下载页有一颗「← 返回实例设置」");
        if (back != nullptr) {
            // **只有一颗**：`findChildren` 按 objectName 捞，捞出来必须正好一条。
            // 名字只可能有一份（downloadBackButtonName() 没有参数了），所以这条也是在钉
            // "那颗键不再按分类复制"。
            const int count = downloadPage == nullptr
                                  ? -1
                                  : downloadPage->findChildren<QPushButton *>(
                                        MainWindowUI::downloadBackButtonName())
                                        .size();
            check(count == 1, "整个下载页**只有一颗**（不再是一栏一颗）",
                  QStringLiteral("找到 %1 颗").arg(count));
            check(back->objectName() == MainWindowUI::downloadBackButtonName(),
                  "它认得出自己", back->objectName());
            check(back->text() == MainWindowUI::downloadBackButtonText() &&
                      !back->text().isEmpty(),
                  "那颗键上有字", back->text());
            check(!back->toolTip().isEmpty(), "那颗键有话说明白它回哪儿");
            check(back->isHidden(),
                  "刚搭好时它是**收着**的（只有从实例设置点进来的那一次才露面）");
            check(back->isEnabled(), "收着归收着，它没被禁用");
        }

        QWidget *leftColumn =
            downloadPage == nullptr
                ? nullptr
                : downloadPage->findChild<QWidget *>(
                      MainWindowUI::leftColumnName(QStringLiteral("download")));
        QListWidget *categories = ui.getDownloadCategoryList();
        QFrame *leftSurface =
            downloadPage == nullptr ? nullptr : surfaceOf(downloadPage, Theme::sidebarSurfaceValue());
        // UI 7b（设计稿 download-*）：左栏收起，分类成了页头下面一排 chip，这颗键跟着搬到那一排最前面。
        QWidget *chipRow = downloadPage == nullptr ? nullptr
                                                   : downloadPage->findChild<QWidget *>(MainWindowUI::downloadCategoryChipsName());
        Q_UNUSED(leftColumn);
        check(chipRow != nullptr && back != nullptr && chipRow->isAncestorOf(back),
              "它长在分类 chip 那一排里（左栏收起之后，那一排就是原来「左边栏的顶上」）");
        check(leftSurface != nullptr && categories != nullptr &&
                  inSurface(categories, Theme::sidebarSurfaceValue()),
              "分类那一列也在左栏那个**侧栏面**上（不再画自己那圈边）",
              surfaceValueOf(categories));
        check(categories != nullptr && categories->frameShape() == QFrame::NoFrame,
              "分类那一列自己不画边框（四周已经是侧栏面）",
              categories != nullptr ? QString::number(int(categories->frameShape()))
                                    : QStringLiteral("<null>"));
        // 在分类那一列**上面**。量的是布局里的**次序**，不是 y 坐标 —— 这一颗此刻收着，
        // 收着的控件布局不给它分配几何，拿 y 去比得到的是"两个都没摆过"的假数据。
        // NOTE: 那一对里的布局是**侧栏面自己**那条布局（`buildTwoColumnPage()` 把
        // `columns.left.layout` 指过去的就是它），不是栏控件那条 —— 栏控件那条
        // 里此刻只有侧栏面一个东西。
        QLayout *leftLayout = leftSurface == nullptr ? nullptr : leftSurface->layout();
        Q_UNUSED(leftLayout);
        QLayout *chipLayout = chipRow == nullptr ? nullptr : chipRow->layout();
        QWidget *firstChip = chipRow == nullptr ? nullptr : chipRow->findChild<QWidget *>(MainWindowUI::downloadCategoryChipName(0));
        check(chipLayout != nullptr && back != nullptr && firstChip != nullptr &&
                  chipLayout->indexOf(back) >= 0 && chipLayout->indexOf(back) < chipLayout->indexOf(firstChip),
              "它在那一排分类 chip 的**最前面**");
        // 它**不在**右栏那几页里：那几页跟着分类翻，摆进去就是"翻一栏这颗键也跟着滚没了"。
        QStringList inside;
        QStackedWidget *stack = ui.getDownloadStack();
        for (int i = 0; stack != nullptr && i < stack->count(); ++i) {
            QWidget *pageOfStack = stack->widget(i);
            if (pageOfStack != nullptr && back != nullptr && pageOfStack->isAncestorOf(back)) {
                inside << MainWindowUI::downloadCategoryKeys().value(i);
            }
        }
        check(inside.isEmpty(),
              "它不在右栏那几页里的任何一页上（翻栏时它不跟着动）", inside.join(", "));

        // 露面 / 收回去：收的是 `setVisible(false)`，**位置也不占** —— 不是变灰。
        ui.setDownloadBackVisible(true);
        QApplication::processEvents();
        check(back != nullptr && !back->isHidden() && back->isEnabled(),
              "点名让它露面，它就在那儿（不是「设了也不亮」）");
        ui.setDownloadBackVisible(false);
        QApplication::processEvents();
        check(back != nullptr && back->isHidden() && back->isEnabled(),
              "收回去是整颗不见（不是变灰留在那儿占位置）");
    }

    // ---- 版本树那一层 ----
    check(gameStack != nullptr && gameStack->count() == 2,
          "游戏页正好两层：版本树 / 安装页",
          gameStack != nullptr ? QString::number(gameStack->count()) : QString());
    check(gameStack != nullptr &&
              gameStack->currentIndex() == MainWindowUI::downloadGameTreeIndex(),
          "下载页一打开停在版本树那一层（不是直接跳进安装页）",
          gameStack != nullptr ? QString::number(gameStack->currentIndex()) : QString());
    check(versionTree != nullptr && versionTree->parentWidget() != nullptr &&
              gameStack != nullptr &&
              gameStack->widget(MainWindowUI::downloadGameTreeIndex()) ==
                  versionTree->parentWidget(),
          "树在版本树那一层里（就是 gameStack 的第 0 页）");
    // 树只有一列、表头藏起来：那一列全是版本号，表头写「1」纯属噪音；而 `setHeaderHidden`
    // 漏了的表现是顶上凭空多一条空表头，占掉一行高度。
    check(versionTree != nullptr && versionTree->columnCount() == 1,
          "版本树只有一列",
          versionTree != nullptr ? QString::number(versionTree->columnCount()) : QString());
    check(versionTree != nullptr && versionTree->isHeaderHidden(), "版本树的表头是藏起来的");
    check(versionTree != nullptr && versionTree->uniformRowHeights(),
          "版本树按统一行高算（742 条快照省的是滚动时的重算）");

    // ---- 版本树长什么样（用户 点名要的那几样） ----
    // 文本放大一点 / 上下间距放大一点 / 版本号下面一行小字写发布日期 /
    // 版本名字前面一个草方块 / 四个加载器各带自己的标志 / 行最右那个能转的箭头。
    //
    // 前几样是**常数**，直接比常数；后面几样是**画上去的**，常数是对的也可能画歪了、
    // 画没了、画到别的格子里 —— 那只有像素知道。所以下面自己起一棵小树挂**真**代理，
    // 抓一张图按像素认（窗口上那棵一条数据都没有：数据要联网才来）。
    check(versionTree != nullptr &&
              versionTree->font().pixelSize() == 14,
          "版本树采用正文 14px 字号",
          versionTree != nullptr ? QString::number(versionTree->font().pixelSize()) : QString());
    check(versionTree != nullptr && !versionTree->expandsOnDoubleClick(),
          "双击不再自己展开一遍（单击分类就切换展开，两次叠起来等于没动）");
    check(versionTree != nullptr && !versionTree->rootIsDecorated(),
          "树上左边那个系统三角不画（展开/收起的提示只在右边那个箭头上）");
    // 这条是**踩过的坑**：想去掉左边那个三角，一股脑写成
    // setChildIndicatorPolicy(DontShowIndicator) 的话，QTreeModel::hasChildren()
    // 会对那个分类直接 return false —— 它的版本号一条都排不进布局，树上只剩四行空的
    // 分类、点开也是空的，而"有没有孩子"这件事在模型层就已经是假的了。
    // 这里钉住的是产品那棵树的**构造方式**（走 rootIsDecorated 那条路）；
    // "子条目真的排进布局了"由 mw_check 拿真数据量（那边才有 900 多条真版本号）。
    auto *treeDelegate =
        versionTree != nullptr ? qobject_cast<VersionTreeDelegate *>(versionTree->itemDelegate())
                               : nullptr;
    check(treeDelegate != nullptr,
          "树上挂的是 VersionTreeDelegate（不是默认那个「一行字一个图标」的代理）",
          versionTree != nullptr && versionTree->itemDelegate() != nullptr
              ? QString::fromLatin1(versionTree->itemDelegate()->metaObject()->className())
              : QStringLiteral("<null>"));
    check(MainWindowUI::versionTreeRowHeight() > 34,
          "行高比一行字高出一截（「上下间距放大一点」）",
          QString::number(MainWindowUI::versionTreeRowHeight()));
    check(MainWindowUI::versionTreeFontStep() > 0 && MainWindowUI::versionDateFontDelta() < 0,
          "树上的字是加大的、日期那行是缩小的（两个方向相反才对）",
          QStringLiteral("%1 / %2")
              .arg(MainWindowUI::versionTreeFontStep())
              .arg(MainWindowUI::versionDateFontDelta()));


    if (const int result = UiVersionTreeGeometryChecks(window, versionTree); result != 0) return result;

    // ---- 五个加载器的标志 ----
    // 五张图要**互不相同**（且不同于草方块），全画成同一张就等于没加标志。
    {
        QVector<QImage> marks;
        for (int i = 0; i < 5; ++i) {
            marks.append(VersionIcons::loaderMark(i, MainWindowUI::versionTreeIconSize())
                             .toImage());
        }
        bool allNonNull = true;
        for (const QImage &image : marks) {
            allNonNull = allNonNull && !image.isNull() && image.width() > 0;
        }
        check(allNonNull, "五个加载器各有一张标志图（大小不为零）");
        bool allDistinct = true;
        for (int i = 0; i < marks.size(); ++i) {
            for (int j = i + 1; j < marks.size(); ++j) {
                allDistinct = allDistinct && marks.at(i) != marks.at(j);
            }
        }
        check(allDistinct, "五张标志互不相同（加载器标志不能混用）");
        check(marks.at(4) != VersionIcons::grassBlock(MainWindowUI::versionTreeIconSize()).toImage(),
              "Quilt 标志不是兜底草方块");
        check(VersionIcons::loaderMark(99, 18).toImage() == VersionIcons::grassBlock(18).toImage(),
              "不认识的加载器下标退回草方块（总比画个空白强）");
        check(VersionIcons::chevron(true, 12, Qt::black).toImage() !=
                  VersionIcons::chevron(false, 12, Qt::black).toImage(),
              "chevron(展开) 和 chevron(收起) 不是同一张图");
        check(VersionIcons::grassBlock(0).isNull() && VersionIcons::loaderMark(0, 0).isNull() &&
                  VersionIcons::chevron(true, 0, Qt::black).isNull(),
              "尺寸给 0 时返回空图、不崩（尺寸算错时至少不会画出一张乱七八糟的）");

        // ---- 哪几张是**透明底**、哪几张自带底色 ----
        //
        // 四张里现在**两张是透明底、两张自带底色**：
        //
        // fabric 透明（用户 换成一张自带透明底的像素画叶子）
        // forge 透明（同一批：那张白底铁砧把白抠了）
        // neoforge 自带一块灰底 + 圆角透明（官方那张 `neoforged_background_128x128.png`）
        // optifine 自带一块圆角底
        //
        // 底色全是**烤进 PNG** 的 ⇒ 这一节量的是**资源文件本身**，不是画的代码 —— 谁哪天
        // 把带底的图换回来（或者反过来把带底的抠了），这里当场红。
        //
        // 判据挑的四个点是**四张共用**的：(10,10) 左上里侧 / (10,55) 左下 / (55,50) 右下
        // 三处都在"透明的那两张"里一定空着、在"有底的那两张"里一定是底；(32,32) 是正中，
        // 四张都该有东西。**"有底"那两张必须留一条对照** —— 不然"透明"那几条在
        // "四张全透明"（等于四张全废）时也绿。
        // NOTE: 四个角 (0,0)/(63,0)/(0,63)/(63,63) 全透明是**四张共有**的（圆角底，
        // 半径 3/16），拿它当"底没了"的判据会四张都绿 —— 别用。
        {
            const int side = 128;  // 64 那张的 2 倍：走最近邻放大，采样点乘 2 就是原图那个像素
            const QImage fabric = VersionIcons::loaderMark(0, side).toImage();
            const QImage forge = VersionIcons::loaderMark(1, side).toImage();
            const QImage neo = VersionIcons::loaderMark(2, side).toImage();
            const QImage opti = VersionIcons::loaderMark(3, side).toImage();
            const auto alphaAt = [&](const QImage &image, int x, int y) {
                const QPoint p(x * side / 64, y * side / 64);
                return image.rect().contains(p) ? qAlpha(image.pixel(p)) : -1;
            };
            const auto emptyAt = [&](const QImage &image) {
                return alphaAt(image, 10, 10) == 0 && alphaAt(image, 10, 55) == 0 &&
                       alphaAt(image, 55, 50) == 0;
            };
            const auto solidAt = [&](const QImage &image) {
                return alphaAt(image, 10, 10) > 0 && alphaAt(image, 10, 55) > 0 &&
                       alphaAt(image, 55, 50) > 0;
            };
            check(emptyAt(fabric),
                  "Fabric 那张是透明底（像素画叶子自带透明）",
                  QStringLiteral("左上 %1 / 左下 %2 / 右下 %3")
                      .arg(alphaAt(fabric, 10, 10))
                      .arg(alphaAt(fabric, 10, 55))
                      .arg(alphaAt(fabric, 55, 50)));
            check(alphaAt(fabric, 32, 32) > 0,
                  "★ Fabric 的叶子本身还在（抠底别把整张图抠没了）",
                  QStringLiteral("正中 alpha %1").arg(alphaAt(fabric, 32, 32)));
            check(emptyAt(forge),
                  "Forge 那张也是透明底（白底那张的铁砧白会被抠掉）",
                  QStringLiteral("左上 %1 / 左下 %2 / 右下 %3")
                      .arg(alphaAt(forge, 10, 10))
                      .arg(alphaAt(forge, 10, 55))
                      .arg(alphaAt(forge, 55, 50)));
            check(alphaAt(forge, 32, 32) > 0,
                  "★ Forge 那把铁砧还在（抠白别把铁砧也抠了）",
                  QStringLiteral("正中 alpha %1").arg(alphaAt(forge, 32, 32)));
            check(solidAt(neo) && solidAt(opti),
                  "对照：NeoForge / OptiFine 那两张的底色**还在**"
                  "（这一条保证上面「透明」那几条不是在「四张全透明」时也绿）",
                  QStringLiteral("NeoForge 左上 %1 正中 %2 / OptiFine 左上 %3 正中 %4")
                      .arg(alphaAt(neo, 10, 10))
                      .arg(alphaAt(neo, 32, 32))
                      .arg(alphaAt(opti, 10, 10))
                      .arg(alphaAt(opti, 32, 32)));
        }
    }

    // ---- 版本树上那四栏各自的小图标（用户 定：「快照版的草用白色（覆雪草块），
    // 远古版就土块，愚人节版用幸运方块，原版不变」）----
    //
    // 这一段量的是**颜色**，而颜色是我们自己写死在 VersionIcons.cpp 里的常数 —— 正是
    // "颜色由我们自己的代码决定"那种该拿像素当证据的格子（拿整行/样式调色板量会验错东西，
    // 见 [[wtfml-build-and-harness]] 里那条）。判据一律写成**相互关系**（比谁亮、是不是绿、
    // 亮像素有多少），不抄那九个色值：抄色值的话，改一个近似色就要同步两处，而它并不是
    // 我们想钉的那件事。
    {
        const int side = 32;  // 16 格的整数倍，格子坐标和像素对得上（产品用的是 18）
        QVector<QImage> marks;
        for (int i = 0; i < 4; ++i) {
            marks.append(VersionIcons::versionChannelMark(i, side).toImage());
        }
        bool allNonNull = true;
        for (const QImage &image : marks) {
            allNonNull = allNonNull && !image.isNull() && image.width() == side;
        }
        check(allNonNull, "四栏各有一张图标（大小不为零）");
        bool allDistinct = true;
        for (int i = 0; i < marks.size(); ++i) {
            for (int j = i + 1; j < marks.size(); ++j) {
                allDistinct = allDistinct && marks.at(i) != marks.at(j);
            }
        }
        check(allDistinct, "四张图标互不相同（四栏长得一样就等于没加）");

        // 16 格坐标系里某一格 → 图上那个像素（side 取 16 的倍数时是整数倍放大）。
        const auto cell = [side](const QImage &image, int cx, int cy) {
            return image.pixelColor(cx * side / 16, cy * side / 16);
        };
        const QImage plain = VersionIcons::grassBlock(side).toImage();
        // 「原版不变」= 正式版那一栏和 grassBlock() **逐像素完全相同**。
        check(marks.at(0) == plain, "正式版那一栏还是那个草方块（原版不变）");

        // 快照版：上面那层是白的（三个通道都比绿的那层高，而且绿不再是主色），
        // 下面那截是土 —— 和正式版那格**一样**（土是几张共用的底图）。
        const QColor snapTop = cell(marks.at(1), 8, 1);
        const QColor plainTop = cell(plain, 8, 1);
        check(snapTop.red() > plainTop.red() && snapTop.green() > plainTop.green() &&
                  snapTop.blue() > plainTop.blue() && snapTop.green() <= snapTop.blue(),
              "快照版上面那层是白的（比绿的那层亮，而且不再偏绿）",
              QStringLiteral("快照 %1 / 正式 %2")
                  .arg(snapTop.name())
                  .arg(plainTop.name()));
        check(cell(marks.at(1), 8, 12) == cell(plain, 8, 12),
              "快照版下面那截和草方块是同一份土（共用底图）");

        // 远古版：**整张图里一格绿的都没有**。判据是"绿是三个通道里最大的那个" ——
        // 注意土自己就是 g > b（0x9c704a 这种），所以**不能**写成 `g > b` 就算偏绿：
        // 那样整块土每一格都会被数进来，一条永远红的断言。草的三个色（0x8cc85a /
        // 0x5d9c3f / 0x4a8230）全都是 g 最大，这一条抓的正是"草那一层没去掉 / 颜色写错"。
        int greenish = 0;
        for (int y = 0; y < marks.at(2).height(); ++y) {
            for (int x = 0; x < marks.at(2).width(); ++x) {
                const QColor c = marks.at(2).pixelColor(x, y);
                if (c.green() > c.red() && c.green() > c.blue()) {
                    ++greenish;
                }
            }
        }
        check(greenish == 0, "远古版整张图一点绿都没有（就是一块土）",
              QStringLiteral("%1 格偏绿").arg(greenish));

        // 愚人节版：金底 + 中间那簇奶油色的问号。两条都不抄色值：
        // · 底色得"偏黄"（红的比蓝的高出一大截）；
        // · 问号认**蓝通道高的那一簇**。金的那几档蓝都很低（0x4c / 0x2a / 0x16），
        // 整个底图上没有第二个地方会是高蓝 —— 所以这条就把"问号画上去了没有"
        // 和"底色画对没画对"两件事分开了。（顺带：写成"三个通道都
        // >= 0xf0"是不对的，而那个奶油色是 0xf8f2d2、蓝只有 0xd2 —— 一条永远红的断言。）
        long sumR = 0;
        long sumB = 0;
        int inkCells = 0;
        int notCream = 0;
        for (int y = 0; y < marks.at(3).height(); ++y) {
            for (int x = 0; x < marks.at(3).width(); ++x) {
                const QColor c = marks.at(3).pixelColor(x, y);
                sumR += c.red();
                sumB += c.blue();
                if (c.blue() >= 0xc0) {
                    ++inkCells;
                    if (c.red() < 0xc0 || c.green() < 0xc0) {
                        ++notCream;
                    }
                }
            }
        }
        check(sumR > sumB * 3 / 2, "愚人节版那块是金的（整张显著偏黄）",
              QStringLiteral("红 %1 / 蓝 %2").arg(sumR).arg(sumB));
        check(inkCells >= 20 && notCream == 0,
              "愚人节版中间那个问号真的画上去了（一簇奶油色——蓝通道高的那一片）",
              QStringLiteral("%1 格高蓝（其中 %2 格不是奶油色）").arg(inkCells).arg(notCream));

        // 认不出来的下标 = 当原版（和 loaderMark 一个规矩）：越界不许画个空白出来。
        check(VersionIcons::versionChannelMark(4, side).toImage() == plain &&
                  VersionIcons::versionChannelMark(-1, side).toImage() == plain,
              "认不出来的分类下标退回草方块");
        check(VersionIcons::versionChannelMark(1, 0).isNull(),
              "尺寸给 0 时返回空图、不崩");

        // 产品用的是 versionTreeIconSize() 那一档：四张在**那个**尺寸下也得都画得出来、
        // 而且互不相同（上面那几条都跑在 32 上）。
        QVector<QImage> realSize;
        for (int i = 0; i < 4; ++i) {
            realSize.append(
                VersionIcons::versionChannelMark(i, MainWindowUI::versionTreeIconSize()).toImage());
        }
        bool realOk = true;
        for (int i = 0; i < realSize.size(); ++i) {
            realOk = realOk && !realSize.at(i).isNull();
            for (int j = i + 1; j < realSize.size(); ++j) {
                realOk = realOk && realSize.at(i) != realSize.at(j);
            }
        }
        check(realOk, "树里那个尺寸（versionTreeIconSize）下四张也画得出来、也互不相同",
              QStringLiteral("边长 %1").arg(MainWindowUI::versionTreeIconSize()));
    }

    // ---- 那四张图**真的画到行上了**没有 ----
    //
    // 上面那一段量的是"画出来是什么颜色"，这一段量的是**代理到底画不画**：版本号那些行
    // 原来靠代理的兜底（没给图标就画草方块）出图，现在改成了由条目自己带一张 QPixmap ——
    // 代理那条 `canConvert<QPixmap>()` 的支路在版本树上**这是第一次真的被用上**。它要是
    // 没走过去，行上会静静退回草方块：四栏全绿，而上面每一句断言照样全绿（这正是这个
    // 仓库栽过好几次的"假绿"形状）。所以这里搭一棵探针树真渲染一张图、按像素认。
    {
        const int iconSize = MainWindowUI::versionTreeIconSize();
        const int rowHeight = MainWindowUI::versionTreeRowHeight();
        const int pad = MainWindowUI::versionTreeRowPadding();

        QTreeWidget probe;
        probe.setColumnCount(1);
        probe.setHeaderHidden(true);
        probe.setUniformRowHeights(true);
        probe.setExpandsOnDoubleClick(false);
        probe.setRootIsDecorated(false);
        probe.setItemDelegate(new VersionTreeDelegate(&probe));
        QVector<QTreeWidgetItem *> leaves;
        for (int i = 0; i < 4; ++i) {
            auto *top = new QTreeWidgetItem(&probe);
            top->setText(MainWindowUI::versionTreeKeyColumn(), QStringLiteral("分类"));
            auto *leaf = new QTreeWidgetItem(top);
            leaf->setText(MainWindowUI::versionTreeKeyColumn(), QStringLiteral("1.0"));
            leaf->setData(MainWindowUI::versionTreeKeyColumn(), MainWindowUI::rowIconRole(),
                          VersionIcons::versionChannelMark(i, iconSize));
            leaves.append(leaf);
        }
        probe.resize(320, rowHeight * 8 + 60);
        // 必须**先展开**：收着的父条目，它的子条目根本没排进布局，`visualItemRect()` 给的是
        // 一个空矩形 —— 空矩形上算出来的那一格落在别处，量出来的四个数会一模一样（都是背
        // 景色），四条断言就一起红，而且红得看不出是哪来的。
        probe.expandAll();
        probe.show();
        QApplication::processEvents();
        const QImage shot = captureWidget(probe.viewport());

        // 四种颜色各自的判据。**互相不许重叠**是这里的关键（不然"别人的颜色"会被算进来，
        // 那几个"必须为 0"的半边就永远绿）：
        // · 草：绿是主色，而且要**高出红一截** —— 覆雪草块那层雪是 0xf4f9fc，绿 249 确实
        // 比红 244 大，只判 `g > r` 的话雪会被算成草，那条"快照版一格绿都没有"就废了；
        // · 雪：三个通道都很高；
        // · 土：红 > 绿 > 蓝，而且**红不能高出蓝太多**（r <= b*3）。土是 0x9c704a 这种
        // 红/蓝 ≈ 2.1，而幸运方块那圈边 0xa57a16 的红/蓝是 7.5 —— 不这么分的话
        // 愚人节版那一行会被数出"土"来；
        // · 金：红很高同时蓝很低。
        const auto isGreen = [](const QColor &c) {
            return c.green() > c.red() + 15 && c.green() > c.blue();
        };
        const auto isSnow = [](const QColor &c) {
            return c.red() >= 0xc0 && c.green() >= 0xc0 && c.blue() >= 0xc0;
        };
        const auto isDirt = [](const QColor &c) {
            return c.red() > c.green() && c.green() > c.blue() && c.red() < 0xd0 &&
                   c.red() <= c.blue() * 3;
        };
        const auto isGold = [](const QColor &c) { return c.red() > 0xc0 && c.blue() < 0xa0; };

        const char *const names[4] = {"正式版（草方块）", "快照版（覆雪草块）", "远古版（土块）",
                                     "愚人节版（幸运方块）"};
        for (int i = 0; i < leaves.size(); ++i) {
            const QRect row = probe.visualItemRect(leaves.at(i));
            // 代理把图标画在 row.left()+pad 那 iconSize×iconSize 里（见 VersionTreeDelegate
            // 里算 iconRect 那几行）—— 这里就量那一格，不量整行（文字、箭头、小字的颜色都
            // 会掺进来，那是样式说了算的格子，不是我们决定的那一格）。
            const QRect cell(row.left() + pad, row.center().y() - iconSize / 2, iconSize, iconSize);
            int green = 0;
            int snow = 0;
            int dirt = 0;
            int gold = 0;
            for (int y = cell.top(); y <= cell.bottom(); ++y) {
                for (int x = cell.left(); x <= cell.right(); ++x) {
                    if (x < 0 || y < 0 || x >= shot.width() || y >= shot.height()) {
                        continue;
                    }
                    const QColor c = shot.pixelColor(x, y);
                    green += isGreen(c) ? 1 : 0;
                    snow += isSnow(c) ? 1 : 0;
                    dirt += isDirt(c) ? 1 : 0;
                    gold += isGold(c) ? 1 : 0;
                }
            }
            // 每一栏：自己那张图的主色得够多，**别人家的主色一个都不许有**（后半句才是抓
            // "代理没画我们给的那张、静静退回草方块"的那一半）。
            // 例外两条：草方块和覆雪草块下面那截都是土（两张都是"上面一层 + 土"），
            // 幸运方块那个奶油色问号会被"雪"那条判据统计进去（0xf8f2d2 三个通道都高）。
            bool ok = false;
            switch (i) {
                case 0:
                    ok = green >= 20 && dirt >= 20 && snow == 0 && gold == 0;
                    break;
                case 1:
                    ok = snow >= 20 && dirt >= 20 && green == 0 && gold == 0;
                    break;
                case 2:
                    ok = dirt >= 20 && green == 0 && snow == 0 && gold == 0;
                    break;
                default:
                    ok = gold >= 20 && green == 0 && dirt == 0;
                    break;
            }
            check(ok,
                  QStringLiteral("树上那一行画的是它自己那张图（%1）")
                      .arg(QString::fromUtf8(names[i])),
                  QStringLiteral("绿 %1 / 白 %2 / 土 %3 / 金 %4")
                      .arg(green)
                      .arg(snow)
                      .arg(dirt)
                      .arg(gold));
        }
        probe.hide();
        QApplication::processEvents();
    }


    // ---- 安装页那一层 ----
    // 几何只在"这一层正在显示"时有意义：先翻过去再量。
    if (gameStack != nullptr) {
        gameStack->setCurrentIndex(MainWindowUI::downloadGameInstallIndex());
        QApplication::processEvents();
    }
    check(gameStack != nullptr &&
              gameStack->currentIndex() == MainWindowUI::downloadGameInstallIndex(),
          "能翻到安装页那一层",
          gameStack != nullptr ? QString::number(gameStack->currentIndex()) : QString());

    // （要求：「那个页面顶上的大标题和刷新键不要（点进去就隐藏，外面
    // 还保留）」）：进安装页那一层，下载页页头**整块**收起来 —— 不是只收标题、
    // 留一颗刷新孤零零在外面，也不是只藏文字、那一行的高度还占着。
    {
        QWidget *header = ui.getDownloadHeader();
        QPushButton *refresh = ui.getDownloadHeaderRefreshButton();
        check(header != nullptr &&
                  header->objectName() ==
                      MainWindowUI::pageHeaderName(QStringLiteral("download")),
              "找得到下载页页头那一整块（标题 + 刷新都在里面，按 objectName）",
              header != nullptr ? header->objectName() : QStringLiteral("<null>"));
        check(header != nullptr && !header->isVisible(),
              "★ 进了安装页，页头**整块收着**（大标题不在、刷新键也不在）",
              header != nullptr
                  ? (header->isHidden() ? QStringLiteral("整块收着")
                                        : QStringLiteral("还露着"))
                  : QStringLiteral("<null>"));
        check(header != nullptr && refresh != nullptr && header->isAncestorOf(refresh) &&
                  !refresh->isVisible(),
              "那颗刷新跟着页头一起收了（不是只收标题、把它一个人留在外面）",
              refresh != nullptr ? refresh->objectName() : QStringLiteral("<null>"));
    }

    const QRect rBack = rectInPage(installBackBtn);
    const QRect rTarget = rectInPage(installTargetLabel);
    const QRect rEdit = rectInPage(installNameEdit);
    const QRect rInstall = rectInPage(installBtn);

    // 「← 返回版本列表」和「将基于原版 X 安装」同一行（返回按钮在前、标签在后）。
    check(!rBack.isNull() && !rTarget.isNull() && rBack.width() < downloadPage->width() / 4,
          "返回按钮只吃自己那点宽度（没被拉满一整行）",
          QString::number(rBack.width()));
    check(!rBack.isNull() && !rTarget.isNull() &&
              rBack.x() + rBack.width() <= rTarget.x() &&
              qAbs(rBack.y() - rTarget.y()) < rBack.height(),
          "「← 返回版本列表」和「将基于原版 X 安装」在同一行（返回在前）",
          QString("back=[%1,%2] target=[%3,%4]").arg(rBack.x()).arg(rBack.y())
              .arg(rTarget.x()).arg(rTarget.y()));

    // 名称框那一块：标签在上面、输入框居中、那句小字在**输入框下面**（用户要求的
    // 形状就是"下方小字提示"）。小字在框右边或者上面都算做错了。
    {
        QLabel *nameLabel = nullptr;
        if (installNameEdit != nullptr && installNameEdit->parentWidget() != nullptr) {
            for (QLabel *label : installNameEdit->parentWidget()->findChildren<QLabel *>()) {
                if (label->text() == MainWindowUI::installNameLabelText()) {
                    nameLabel = label;
                    break;
                }
            }
        }
        const QRect rLabel = rectInPage(nameLabel);
        check(nameLabel != nullptr && !rLabel.isNull() &&
                  rLabel.y() + rLabel.height() <= rEdit.y(),
              "「版本名称」那个标签在输入框上面",
              QString("label.bottom=%1 edit.y=%2")
                  .arg(rLabel.y() + rLabel.height()).arg(rEdit.y()));
    }

    // 加载器那一卡、名称框、底栏自上而下排开。这几条是"用户描述的那个形状"
    // （名称框 → 加载器 → 底栏里那颗安装键）在几何上的落点。
    QFrame *loaderCard = nullptr;
    QFrame *installFooter = nullptr;
    const QByteArray surfaceKey = Theme::surfacePropertyName().toUtf8();

    if (const int result = UiInstallFormLayoutChecks(ui, installNameEdit, installNameError, installBtn, loaderTree, downloadPage, rEdit, rInstall, loaderCard, installFooter, surfaceKey, rectInPage); result != 0) return result;

    // ---- 页头从安装页退出来之后要**回来**（"点进去就隐藏，外面还保留"的后半句）----
    {
        QWidget *header = ui.getDownloadHeader();
        QStackedWidget *stack = ui.getDownloadStack();
        if (header != nullptr && stack != nullptr && gameStack != nullptr) {
            gameStack->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
            QApplication::processEvents();
            check(header->isVisible(), "退回版本树那一层，页头又露出来");

            stack->setCurrentIndex(MainWindowUI::downloadCategoryGameIndex() + 1);
            QApplication::processEvents();
            check(header->isVisible(), "翻到别的分类，页头照旧露着（外面那几页不受影响）");

            stack->setCurrentIndex(MainWindowUI::downloadCategoryGameIndex());
            QApplication::processEvents();
            check(header->isVisible(), "翻回「游戏」、停在版本树，页头还在");

            gameStack->setCurrentIndex(MainWindowUI::downloadGameInstallIndex());
            QApplication::processEvents();
            check(!header->isVisible(), "再进安装页，页头再次收起来（两发信号都接上了）");

            // 收尾：回到版本树；下面 C2 那节钉的是"打开下载页时看到的第一眼"。
            gameStack->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
            QApplication::processEvents();
        }
    }

    // 量完翻回树那一层：下面 C2 那节钉的是"打开下载页时看到的第一眼"。
    if (gameStack != nullptr) {
        gameStack->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
        QApplication::processEvents();
    }

    std::printf("===== C2. 下载页左栏那份分类（仿 PCL） =====\n");

    if (const int result = UiDownloadCategoriesChecks(ui, gameStack, downloadPage, rectInPage); result != 0) return result;

    std::printf("===== C2b. 左栏那五条「做大点 + 行尾一个刷新按钮」 =====\n");
    // 要求："下载界面左边栏每一项做大点，每一项的右边放一个刷新按钮"。
    // 这一节钉三件事：
    // ① **做大**真的发生了 —— 行高和字号都比"没动过的列表"大一档，量的是数值不是代码；
    // ② 每一条都挂着 rowRefreshRole()，而且那一格**真的画出了东西**（量像素，不看代码）；
    // ③ 那一格的位置**就是** VersionTreeDelegate::refreshRect() 说的那一格 ——
    // MainWindow::eventFilter 按它判断"这一下点中的是不是那个按钮"，两边必须同出一处
    // （分开算的话，按钮会画在 A、点的却是 B，而两边各自的断言都是绿的）。
    //
    // NOTE: "点一下真的会重搜 / 重拉清单"是接线的活，钉在 mw_check 那边（那边才搭 MainWindow）。
    // 这里只管"这一栏长什么样、按钮在哪儿"。图标缓存那条也在 mw_check（那里有真 store）。
    //
    // ①′ **号码不能撞** —— 这一节的另一半，而且它才是这一节存在的理由：
    // 用户报的是"这个刷新图标会在模组/…的右侧那个 > 的地方重叠一份"。查下来不是左栏
    // 的问题，是 `rowRefreshRole()` 和 `resourceKindRole()` 都是 `Qt::UserRole + 3`，
    // 而**资源表每一行都写着 resourceKindRole()** —— 于是模组/资源包/光影/整合包那三张
    // 表的每一行右端都冒出一个刷新圈压住 `>`。两边各自编得过、跑得动，谁也不报错，
    // 症状只在屏幕上。号码是纯 int，编译器帮不上忙，所以这条只能靠断言守着。
    {
        const QList<QPair<QString, int>> roles = {
            {QStringLiteral("rowSubTextRole"), MainWindowUI::rowSubTextRole()},
            {QStringLiteral("rowClearRole"), MainWindowUI::rowClearRole()},
            {QStringLiteral("rowRefreshRole"), MainWindowUI::rowRefreshRole()},
            // 实例页组行行尾那颗齿轮（ 加的）。它和上面那三个是**同一档**的
            // "行装饰"，撞号的症状也一样（整张表多画一笔/多认一下），所以必须一起数 ——
            // 新加一个角色就往这张表里补一笔，这是它存在的理由。
            //
            // NOTE: 这一档现在**只剩四个**了（ 傍晚）：原来还有第 5 个
            // `rowGroupMoveRole`（UserRole + 12，存的是"这一行挪得动哪一边"那两个位），
            // 它跟着行尾那两颗上下移三角一起搬进了「分组设置」那张组清单 —— 那张表
            // 里"挪不挪得动"是算出来的、不需要角色（见 GroupListDelegate::canMoveUp()）。
            // 那个号码（+12）当天晚上就被**实例行**拿去用了（rowPinRole()，见下面
            // 那一对）：号码复用是故意的，搬走之后一个使用者都没有了。
            {QStringLiteral("rowGroupGearRole"), MainWindowUI::rowGroupGearRole()},
            // 这一批是"条目身份"那边，各张表各用各的。resourceKindRole() 就是当年被撞上的
            // 那一个（资源表每一行都写它），所以它必须在场。
            //
            // NOTE: 起资源表那一行的身份**是三个号码**（+3 / +4 / +5）：kind 管
            // "点进去问哪一类"、source 管"去哪一侧问"、iconId 管"图标回来认哪一行"。
            // 这三个是同一批东西（都写着这一行的身份），所以一起进这张表 —— 少写一个
            // 就等于"那个号码随便谁拿去用都不报错"。
            {QStringLiteral("resourceKindRole"), MainWindowUI::resourceKindRole()},
            {QStringLiteral("resourceSourceRole"), MainWindowUI::resourceSourceRole()},
            {QStringLiteral("resourceIconIdRole"), MainWindowUI::resourceIconIdRole()},
            {QStringLiteral("instanceIdRole"), MainWindowUI::instanceIdRole()},
            {QStringLiteral("groupIdRole"), MainWindowUI::groupIdRole()},
            // 实例行行尾那一格的两个角色（ 晚）：`rowPinRole()` = 还能置顶
            // （+12）、`rowUnpinRole()` = 已经在置顶组里（+13）。它们**不是**上面那一档
            // （8/9/10/11 那四个是"画在哪一格"的行装饰，这两个是"这一行挂哪一颗箭头"），
            // 但一样是"号码撞了只会让屏幕上多出一笔"的那类东西，所以一起进这张表。
            {QStringLiteral("rowPinRole"), MainWindowUI::rowPinRole()},
            {QStringLiteral("rowUnpinRole"), MainWindowUI::rowUnpinRole()},
            {QStringLiteral("rowMotdRawRole"), MainWindowUI::rowMotdRawRole()},
            {QStringLiteral("serverAddressRole"), MainWindowUI::serverAddressRole()},
        };
        QString clash;
        for (int i = 0; i < roles.size(); ++i) {
            for (int j = i + 1; j < roles.size(); ++j) {
                if (roles.at(i).second == roles.at(j).second && clash.isEmpty()) {
                    clash = roles.at(i).first + QStringLiteral(" == ") + roles.at(j).first;
                }
            }
        }
        check(roles.size() == 13 && clash.isEmpty(),
              "四个行装饰角色和「条目身份」那几个号码两两不同（撞了就是整张表多画一笔）", clash);
        // 那两个"挂哪一颗箭头"的角色是**挨着的 12 / 13**，而且**互斥地**用在同一格上
        // （见 MainWindowUI::rowUnpinRole() 那段）。这一条钉号码，T17/13e 那边钉互斥。
        check(MainWindowUI::rowPinRole() == Qt::UserRole + 12 &&
                  MainWindowUI::rowUnpinRole() == Qt::UserRole + 13,
              "实例行那一格的两个角色就是 12 / 13（一个是 ↑ 置顶、一个是 ↓ 取消置顶）");
        // 光"两两不同"还不够：全挪回 UserRole + 0…5 也两两不同，可那就又回到身份那一批里
        // 去挨着了。行装饰那几个**整档**待在 8 / 9 / 10 / 11（见 MainWindowUI.hpp 那段
        // 说明），中间空着不占 —— 这一条管"以后有人顺手改小一个数"。
        //
        // NOTE: 原来那一档的尾巴是 +12（上下移那两个位的掩码）， 傍晚跟着那两颗
        // 三角一起删了。这一条**不**去管 +12 —— 它今天是空的，"空的"这件事由下面那条
        // 注释和 MainWindowUI.hpp 那段说，而"它不许被别的东西占了"没有可断言的形状
        // （任何号码都可能被将来某个角色用上，真正的判据是上面那张表要一起补一笔）。
        check(MainWindowUI::rowSubTextRole() == Qt::UserRole + 8 &&
                  MainWindowUI::rowClearRole() == Qt::UserRole + 9 &&
                  MainWindowUI::rowRefreshRole() == Qt::UserRole + 10 &&
                  MainWindowUI::rowGroupGearRole() == Qt::UserRole + 11,
              "行装饰角色就是约定的 8 / 9 / 10 / 11 那一档");
        // 同一件事的另一半：资源表那一行的三个身份号码挨着排（3 / 4 / 5）。它们必须连着 ——
        // 上面那条只说"两两不同"，而"source 顺手写成 +6、把 +4 空出来"这种改法它照样绿。
        check(MainWindowUI::resourceKindRole() == Qt::UserRole + 3 &&
                  MainWindowUI::resourceSourceRole() == Qt::UserRole + 4 &&
                  MainWindowUI::resourceIconIdRole() == Qt::UserRole + 5,
              "资源表那一行的三个身份号码就是挨着的 3 / 4 / 5（kind / 来源 / 图标身份）");
        // 服务器那一页那两个也是挨着的（一个给代理画 MOTD、一个给右键复制用）。上面那条
        // "两两不同"管不住"顺手写成 +16、把 +15 空出来"这种改法，这一条管。
        check(MainWindowUI::rowMotdRawRole() == Qt::UserRole + 14 &&
                  MainWindowUI::serverAddressRole() == Qt::UserRole + 15,
              "服务器那一页的两个角色就是挨着的 14 / 15（MOTD 原串 / 完整地址）");
    }

    if (const int result = UiDownloadTypographyChecks(window, ui); result != 0) return result;

    std::printf("===== D. 主布局与底栏位置 =====\n");
    // 壳：左导航轨 + 右栏（顶上 40 高的窗口按钮条 + 选项卡）。原来通栏的底栏（后台任务 /
    // 工具箱）收进了导航轨底部（设计稿 tokens.md 第 7 节）。
    QLayout *mainLayout = window.centralWidget() != nullptr ? window.centralWidget()->layout()
                                                            : nullptr;
    auto *shell = qobject_cast<QHBoxLayout *>(mainLayout);
    check(shell != nullptr, "主布局是 QHBoxLayout（左轨 + 右栏）");
    TitleBar *layoutRail = ui.titleBar();
    if (shell != nullptr && layoutRail != nullptr) {
        check(shell->contentsMargins().isNull() && shell->spacing() == 0, "主布局四边不留边距、间距 0");
        check(shell->indexOf(layoutRail) == 0 && shell->stretch(0) == 0, "导航轨在最左、不参与拉伸");
        const QPoint tabsAt = tabs->mapTo(&window, QPoint(0, 0));
        check(tabsAt.x() == layoutRail->width() && tabsAt.y() == TitleBar::barHeight(),
              "选项卡紧挨在轨右边、窗口按钮条下面", QStringLiteral("%1,%2").arg(tabsAt.x()).arg(tabsAt.y()));
    }
    check(bar != nullptr, "底栏是个独立控件");
    if (bar != nullptr && layoutRail != nullptr) {
        check(bar->objectName() == MainWindowUI::bottomBarName(), "底栏的 objectName 是约定的那个",
              bar->objectName());
        check(layoutRail->isAncestorOf(bar), "底栏挂在导航轨底部");
        const QRect br(bar->mapTo(layoutRail, QPoint(0, 0)), bar->size());
        check(br.bottom() < layoutRail->height() && layoutRail->height() - br.bottom() <= 24,
              "底栏贴在轨的最下面",
              QStringLiteral("底栏下沿 %1 / 轨高 %2").arg(br.bottom()).arg(layoutRail->height()));
    }
    tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
    QApplication::processEvents();

    std::printf("===== E. 空状态与占位文案 =====\n");
    check(!installed->placeholderText().isEmpty(), "已安装下拉框设了占位文案",
          installed->placeholderText());
    // 那棵树刚搭好时**一个子条目都没有**：版本列表要点开某一条加载器才去拉
    // （拉的过程和结果都由 MainWindow 灌，见 mw_check 那几节）。
    {
        bool noChildren = loaderTree != nullptr && loaderTree->topLevelItemCount() == 7;
        for (int i = 0; loaderTree != nullptr && i < loaderTree->topLevelItemCount(); ++i) {
            noChildren = noChildren && loaderTree->topLevelItem(i)->childCount() == 0;
        }
        check(noChildren, "开局七条（五条加载器 + 预设行 + 隐藏的不装行）底下都是空的"
                          "（版本列表要点开那条才去拉）");
    }
    // 两个占位串必须**都非空**：空串会让 QComboBox 在往空模型里插第一批数据时
    // 静默选中第 0 项（qcombobox.cpp:1753-1759）。界面初始值用"一个都没装"那一个。
    check(installed->placeholderText() == MainWindowUI::noInstalledVersionPlaceholder(),
          "初始占位文案是『暂无已安装版本』", installed->placeholderText());
    check(!MainWindowUI::noInstalledVersionPlaceholder().isEmpty() &&
              !MainWindowUI::chooseVersionPlaceholder().isEmpty() &&
              MainWindowUI::noInstalledVersionPlaceholder() !=
                  MainWindowUI::chooseVersionPlaceholder(),
          "两个空状态文案都非空且不同");
    check(installed->count() == 0, "UI 刚搭好时已安装那个下拉框是空的（数据由 MainWindow 灌）");
    check(installed->currentIndex() < 0, "空下拉框 currentIndex() == -1");

    // 模组加载器那棵树**没有"界面自己记住选了哪一条"这件事**：选中的那条住在
    // InstallPageController::m_selectedLoader 里，界面这一层只搭条目、不置任何状态
    // （所以这里没有什么"初始勾中的是哪个"可钉 —— 那条不变量在 mw_check 那边，
    // 那边才搭得出 MainWindow）。
    //
    // NOTE: 原来这里钉的是"初始勾的是「不装（纯原版）」"。换成树之后这条**更强了**：
    // 树上的条目本来就没有"勾"这个状态，产品里"现在要装哪一个"只有
    // m_selectedLoader 一处（InstallPageController 的构造函数把它置成
    // modLoaderNoneIndex()）。这里改钉"树上不带任何勾选状态"——它就是
    // "选中只住在一个地方"这件事在界面上的凭据。
    {
        check(loaderTree != nullptr && loaderTree->selectedItems().isEmpty() &&
                  loaderTree->currentItem() == nullptr,
              "树上没有任何条目是选中/当前状态（选中是 MainWindow 那一处的事）",
              loaderTree != nullptr ? QString::number(loaderTree->selectedItems().size())
                                    : QStringLiteral("<null>"));
        // 六条都点得动：这一组里没有"灰着不让人点"的条目 —— 用户点到哪条
        // 就是改用哪条（本启动器一次只装一个加载器，"不兼容"说的是不能同时装，
        // 不是这一条点不得）。「预设」那一行点它开的是弹窗（P1），一样要可点。
        bool allEnabled = loaderTree != nullptr && loaderTree->topLevelItemCount() == 7;
        for (int i = 0; loaderTree != nullptr && i < loaderTree->topLevelItemCount(); ++i) {
            allEnabled = allEnabled && loaderTree->topLevelItem(i)->flags().testFlag(Qt::ItemIsEnabled);
        }
        check(allEnabled, "七条顶层条目一律可点（没有灰着的那一条）");
    }

    std::printf("===== E2. 滚轮防护（行为验证） =====\n");
    {
        // eventFilters() 是 protected，查不了，直接按行为验：
        // 没有焦点时滚轮必须被吃掉，有焦点时照常放行。
        installed->addItems({QStringLiteral("1.18.2"), QStringLiteral("26.3")});
        installed->setCurrentIndex(0);

        // NOTE: 这个下拉框在主页上是**藏着的**（挑实例改从底栏那块面板进，见
        // MainWindowUI 里 `installedVersionCombo->hide()`）。藏着的控件 `setFocus()`
        // 不生效、`hasFocus()` 永远是 false —— 下面"有焦点时照常切换"那一条会以
        // `hasFocus=0` 的样子红，而产品那条路是好的。这一节量的是 WheelGuard 的判断
        // 本身，所以先把它摆出来（它没进布局，`show()` 只是让它可聚焦），量完收回去。
        installed->show();
        QApplication::processEvents();

        const auto sendWheel = [](QWidget *target) {
            QWheelEvent event(QPointF(5, 5), QPointF(5, 5), QPoint(0, 0), QPoint(0, -120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(target, &event);
        };

        // 焦点放到别的控件上，模拟"鼠标划过下拉框但没点它"
        // NOTE: offscreen 平台不保证窗口一 show 就是**活动窗口**，而 `hasFocus()` 只在
        // 活动窗口里才为真。这一节量的正是产品那个"看 hasFocus"的滚轮过滤器，所以先把
        // 窗口真的叫到前面来（真机上是用户点一下窗口这回事）。
        window.activateWindow();
        window.raise();
        QApplication::processEvents();
        startBtn->setFocus();
        QApplication::processEvents();
        const bool focusedBefore = installed->hasFocus();
        sendWheel(installed);
        QApplication::processEvents();
        check(!focusedBefore && installed->currentIndex() == 0,
              "无焦点时滚轮不改选中项（防护生效）",
              QString("hasFocus=%1 idx=%2")
                  .arg(focusedBefore)
                  .arg(installed->currentIndex()));

        // 焦点在下拉框自己身上时，滚轮应该照常切换
        installed->setFocus();
        QApplication::processEvents();
        sendWheel(installed);
        QApplication::processEvents();
        check(installed->hasFocus() && installed->currentIndex() == 1,
              "有焦点时滚轮照常切换（防护没有误伤正常操作）",
              QString("hasFocus=%1 idx=%2")
                  .arg(installed->hasFocus())
                  .arg(installed->currentIndex()));

        // 没焦点时那一发滚轮**不能就这么没了**：得转给外面那层滚动区 —— 用户报的就是
        // 鼠标停在数字框 / 下拉框上时页面滚不动（值不改是对的，页面不滚是错的）。
        {
            QWidget scrollHost;
            auto *area = new QScrollArea(&scrollHost);
            area->setWidgetResizable(true);
            auto *content = new QWidget(area);
            content->setMinimumHeight(1200);
            auto *spin = new QSpinBox(content);
            spin->setGeometry(10, 10, 140, 32);
            spin->setRange(0, 99999);
            spin->setValue(123);
            WheelGuard::install(spin);
            area->setWidget(content);
            area->resize(240, 160);
            scrollHost.resize(240, 160);
            scrollHost.show();
            QApplication::processEvents();
            startBtn->setFocus();
            QApplication::processEvents();
            QScrollBar *bar = area->verticalScrollBar();
            const int before = bar->value();
            QWheelEvent overSpin(QPointF(5, 5), spin->mapToGlobal(QPoint(5, 5)), QPoint(0, 0),
                                 QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                                 Qt::NoScrollPhase, false);
            QApplication::sendEvent(spin, &overSpin);
            QApplication::processEvents();
            check(!spin->hasFocus() && spin->value() == 123 && bar->value() > before,
                  "鼠标停在没有焦点的数字框上滚轮：值不动，页面照样滚（转给外层滚动区）",
                  QString("hasFocus=%1 值 %2 / 滚动 %3→%4")
                      .arg(spin->hasFocus())
                      .arg(spin->value())
                      .arg(before)
                      .arg(bar->value()));
        }

        installed->clear();
        check(installed->currentIndex() < 0, "测试用的条目已清干净");
        installed->hide();
        QApplication::processEvents();
    }

    std::printf("===== E2b. 滚轮防护的**前提**：焦点策略（真滚轮那条路） =====\n");
    {
        // 上面 E2 是行为验证，但它用的是合成事件 —— 照不出"真滚轮先抢焦点"这个病
        // （理由见 wheelGuardProblem() 上面那段）。这里把**装过防护的那 10 个控件**逐个
        // 按前提验一遍：焦点策略里不许含 WheelFocus，且 Tab / 点选照常。
        struct Guarded {
            QWidget *widget;
            const char *what;
        };
        const Guarded guarded[] = {
            {installed, "主页那个「已安装版本」下拉框"},
            {window.findChild<QSpinBox *>(MainWindowUI::memoryMaxSpinName()),
             "设置页「最大内存」数字框"},
            {window.findChild<QSlider *>(MainWindowUI::memoryMaxSliderName()),
             "设置页「最大内存」那根条"},
            {window.findChild<QComboBox *>(MainWindowUI::memoryModeComboName()),
             "设置页「内存分配」下拉框"},
            {window.findChild<QComboBox *>(MainWindowUI::launcherBehaviourComboName()),
             "设置页「启动后」下拉框"},
            {window.findChild<QComboBox *>(MainWindowUI::downloadSourceComboName()),
             "设置页「下载源」下拉框"},
            // 下面三个是 「启动选项」那一组加的。数字框尤其要装：滚轮在
            // QSpinBox 上是**改值**（加一步），而它和下拉框一样在没焦点时看不出变化 ——
            // 用户滚过去扫一眼，窗口大小就从 854 变成 870 了。
            {window.findChild<QComboBox *>(MainWindowUI::processPriorityComboName()),
             "设置页「进程优先级」下拉框"},
            {window.findChild<QSpinBox *>(MainWindowUI::gameWindowWidthSpinName()),
             "设置页「游戏窗口大小」的宽度数字框"},
            {window.findChild<QSpinBox *>(MainWindowUI::gameWindowHeightSpinName()),
             "设置页「游戏窗口大小」的高度数字框"},
            {window.findChild<QComboBox *>(MainWindowUI::gameWindowModeComboName()),
             "设置页「窗口模式」下拉框"},
        };
        constexpr int kGuardedCount = 10;
        int guardedCount = 0;
        for (const Guarded &item : guarded) {
            const QString problem = wheelGuardProblem(item.widget);
            check(problem.isEmpty(), QStringLiteral("E2b %1：防护装对了").arg(item.what), problem);
            guardedCount += problem.isEmpty() ? 1 : 0;
        }
        check(guardedCount == kGuardedCount,
              QStringLiteral("E2b %1 个控件一个不落（少一个说明有人漏装了）").arg(kGuardedCount),
              QStringLiteral("%1/%2").arg(guardedCount).arg(kGuardedCount));

        // 反面对照：**没装防护**的下拉框应该是原始策略（WheelFocus）—— 这一条是上面那些
        // 断言"真的在验东西"的证据：判据能认出一个没装的控件。用安装页那棵树不顶用
        // （它没有焦点策略这回事），所以这里临时造一个空的 QComboBox。
        QComboBox untouched;
        const QString problem = wheelGuardProblem(&untouched);
        check(!problem.isEmpty(), "E2b 反面：没装防护的下拉框会被这条判据认出来",
              QStringLiteral("默认策略 = %1").arg(static_cast<int>(untouched.focusPolicy())));
    }

    std::printf("===== E3. 一次手势只处理一次（RowActivationRelay） =====\n");
    {
        // H3：三棵树原来各接了 itemClicked + itemActivated 两条路，而 Windows 上**双击
        // 恰好各触发一次**（qabstractitemview.cpp：双击那一下 emit 的是 doubleClicked +
        // activated，随后的 release 被 releaseFromDoubleClick 挡掉、不再发 clicked）→
        // 槽跑两遍：「下载失败」假弹窗 + 整合包导入被静默吃掉。
        // 现在只有 itemClicked 一条路，键盘那两个键由 ui/RowActivation.hpp 里那个过滤器
        // 自己认。这里把三种手势各打一遍，数它进来几次。
        QTreeWidget probe;
        probe.setHeaderHidden(true);
        probe.setRootIsDecorated(false);
        auto *category = new QTreeWidgetItem(&probe);
        category->setText(0, QStringLiteral("模组"));
        auto *version = new QTreeWidgetItem(category);
        version->setText(0, QStringLiteral("1.2.3"));
        probe.expandAll();
        probe.resize(240, 160);
        probe.show();
        QApplication::processEvents();

        QStringList fired;
        connectRowActivation(&probe, [&fired](QTreeWidgetItem *item) {
            fired.append(item != nullptr ? item->text(0) : QStringLiteral("<空>"));
        });

        // 一次双击 = 展开又收起：靠的是 expandsOnDoubleClick 关掉（三棵树都是自己在处理
        // 展开），这一条直接钉住那个开关。
        check(!probe.expandsOnDoubleClick(),
              "E3 关掉了 expandsOnDoubleClick（双击分类行不会再展开又收起）");

        const auto sendMouse = [&probe](QEvent::Type type, const QPoint &pos,
                                        Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(pos), QPointF(probe.viewport()->mapToGlobal(pos)),
                              Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(probe.viewport(), &event);
        };
        const auto clickOnce = [&](QTreeWidgetItem *item) {
            const QPoint pos = probe.visualItemRect(item).center();
            sendMouse(QEvent::MouseButtonPress, pos, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
            QApplication::processEvents();
        };
        const auto doubleClick = [&](QTreeWidgetItem *item) {
            const QPoint pos = probe.visualItemRect(item).center();
            sendMouse(QEvent::MouseButtonPress, pos, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
            sendMouse(QEvent::MouseButtonDblClick, pos, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
            QApplication::processEvents();
        };

        clickOnce(version);
        check(fired.size() == 1 && fired.last() == QStringLiteral("1.2.3"),
              "E3 单击 = 处理一次", QStringLiteral("进来 %1 次").arg(fired.size()));

        fired.clear();
        doubleClick(version);
        check(fired.size() == 1, "E3 双击 = 处理**一次**（不是两次）",
              QStringLiteral("进来 %1 次：%2").arg(fired.size()).arg(fired.join(QStringLiteral("+"))));

        fired.clear();
        probe.setCurrentItem(version);
        QApplication::processEvents();
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&probe, &enter);
        QApplication::processEvents();
        check(fired.size() == 1, "E3 回车 = 处理一次（键盘那条路还在）",
              QStringLiteral("进来 %1 次").arg(fired.size()));

        // 长按重复（自动重复）不该被当成又一次操作：按住回车不放时 Qt 会连着发
        // isAutoRepeat() 的事件，据此过滤掉了。
        fired.clear();
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QString(), true);
        QApplication::sendEvent(&probe, &repeat);
        QApplication::processEvents();
        check(fired.isEmpty(), "E3 自动重复的回车不算又一次操作",
              QStringLiteral("进来 %1 次").arg(fired.size()));

        // 没有当前条目时按回车：什么都不该发生（当前条目为空时也不许崩）。
        fired.clear();
        probe.setCurrentItem(nullptr);
        QApplication::processEvents();
        QKeyEvent enterNoItem(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&probe, &enterNoItem);
        QApplication::processEvents();
        check(fired.isEmpty(), "E3 没选中任何条目时按回车不炸也不乱调");

        // 上面这一整段用的是**缺省**判据（isToggleRow 没给）：那就一行都不算"开关行"，
        // 于是双击的第二发被 Qt 压掉，一次手势只来一发 —— 这是现有行为，也就是下面
        // E3b 要区别对待的那一档的"反面"。这里先用顶层那一行把它钉住。
        fired.clear();
        doubleClick(category);
        check(fired.size() == 1, "E3 缺省判据（不给 isToggleRow）时，顶层行也还是只来一次",
              QStringLiteral("进来 %1 次").arg(fired.size()));
    }

    std::printf("===== E3b. 开关行：点得快也要每一下都算（isToggleRow） =====\n");
    {
        // 要求：「还有下载版本那里如果一直点那个展开关闭会有几次点击失效」。
        // 真因：Windows 上**点得快的时候第二下（以及后面每一发）到的是 MouseButtonDblClick**，
        // 而视图的 itemClicked 只在"按下+松开都到手"时才发（双击那一发的松开被
        // releaseFromDoubleClick 挡掉）→ 那几发点击**全丢**。
        // 判据因此交给调用方：isToggleRow() 说"这一行的激活是开/关"（那种行每一下都该算），
        // 动作行（进安装页、"下它"）不给它 → 仍然一次手势一发。
        QTreeWidget probe;
        probe.setHeaderHidden(true);
        probe.setRootIsDecorated(false);
        auto *category = new QTreeWidgetItem(&probe);
        category->setText(0, QStringLiteral("模组"));
        auto *version = new QTreeWidgetItem(category);
        version->setText(0, QStringLiteral("1.2.3"));
        probe.expandAll();
        probe.resize(240, 160);
        probe.show();
        QApplication::processEvents();

        QStringList fired;
        connectRowActivation(
            &probe,
            [&fired](QTreeWidgetItem *item) {
                fired.append(item != nullptr ? item->text(0) : QStringLiteral("<空>"));
                // 和产品里那三个 handler 一样：展开/收起是**自己**做的（Qt 那份双击展开
                // 已经被接线口关掉了）。
                if (item != nullptr && item->childCount() > 0) {
                    item->setExpanded(!item->isExpanded());
                }
            },
            [](QTreeWidgetItem *item) { return item->parent() == nullptr; });

        const auto sendMouse = [&probe](QEvent::Type type, const QPoint &pos,
                                        Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(pos), QPointF(probe.viewport()->mapToGlobal(pos)),
                              Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(probe.viewport(), &event);
        };
        const auto clickOnce = [&](QTreeWidgetItem *item) {
            const QPoint pos = probe.visualItemRect(item).center();
            sendMouse(QEvent::MouseButtonPress, pos, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
            QApplication::processEvents();
        };
        const auto doubleClick = [&](QTreeWidgetItem *item) {
            const QPoint pos = probe.visualItemRect(item).center();
            sendMouse(QEvent::MouseButtonPress, pos, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
            sendMouse(QEvent::MouseButtonDblClick, pos, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, pos, Qt::NoButton);
            QApplication::processEvents();
        };

        fired.clear();
        category->setExpanded(true);
        clickOnce(category);
        check(fired.size() == 1, "E3b 开关行：单击 = 一次（补发那一路不该多来一发）",
              QStringLiteral("进来 %1 次").arg(fired.size()));

        // 双击 = **两下**物理点击 = 翻两次（翻回原样）。这一条就是"一直点不会再失效"
        // 的判据：第二发是 MouseButtonDblClick，只有补上它才对得上"两下"。
        fired.clear();
        category->setExpanded(true);
        doubleClick(category);
        check(fired.size() == 2, "E3b 开关行：双击 = 两次（两下物理点击都算数）",
              QStringLiteral("进来 %1 次：%2").arg(fired.size()).arg(fired.join(QStringLiteral("+"))));
        check(category->isExpanded(), "E3b 开关行：翻两次回到原样（展开状态没被翻乱）");

        // 动作行（子条目）：**不**给判据，照样一次手势一发 —— 双击不该把安装页开两遍。
        fired.clear();
        doubleClick(version);
        check(fired.size() == 1, "E3b 动作行（子条目）：双击还是只来一次",
              QStringLiteral("进来 %1 次：%2").arg(fired.size()).arg(fired.join(QStringLiteral("+"))));

        // 连点四下（点得快：1 单击 + 3 双击）：开关行要正好翻 4 次。
        fired.clear();
        category->setExpanded(true);
        clickOnce(category);
        doubleClick(category);
        doubleClick(category);
        check(fired.size() == 5, "E3b 连点五下 = 五发（单击 1 发 + 两个双击各 2 发）",
              QStringLiteral("进来 %1 次").arg(fired.size()));
        // 五下 = 翻五次：从"展开"起翻奇数次 ⇒ 最后是**收起**（这就是"每一下都算"的样子）。
        check(!category->isExpanded(), "E3b 连点五下之后：翻了五次（不是三次、也不是一次）");
    }

    std::printf("===== E4. 底栏中间那格：任务摘要 =====\n");

    if (const int result = UiTaskSummaryChecks(ui); result != 0) return result;

    std::printf("===== E5. 底栏中间那格：长文件名不许把窗口撑宽（sizeHint 得有上限） =====\n");
    {
        QLabel *status = ui.getStatusLabel();
        check(status != nullptr &&
                  status->maximumWidth() == MainWindowUI::bottomBarStatusTextWidth(),
              "E5 那格设了最大宽度（等于 bottomBarStatusTextWidth()）",
              status != nullptr ? QStringLiteral("maximumWidth=%1 期望=%2")
                                      .arg(status->maximumWidth())
                                      .arg(MainWindowUI::bottomBarStatusTextWidth())
                                : QStringLiteral("控件没找到"));

        // 顶宽这件事分两半，**只有一半是真的**：
        // ① sizeHint —— **会被顶大**。ElidedLabel::sizeHint() 本来就是拿
        // maximumWidth() 当上限的（见 MainWindowUI.cpp 里那个类）。
        // 不设上限时，300 个字符的文件名把这一格顶到 3912、把底栏顶到 4338、
        // 把**主窗口**顶到 4338 —— 而默认窗口只有 900 宽。任何走 sizeHint 的路径
        // （adjustSize()、平台给初次尺寸、以后哪天加的"记住窗口大小"）都会拿到一个
        // 四倍宽的窗口。所以这个上限该有。
        // ② **最小宽度** —— **不会**被顶大，这条与上限无关：那格还有
        // setMinimumWidth(kStatusLabelMinWidth)，而 qSmartMinSize() 在
        // `minSize.width() > 0` 时直接把最小尺寸**钉成** minSize.width()
        // （qlayoutengine.cpp:331）—— 所以文字再长，这一格的最小贡献就是 260，
        // 整窗最小宽度实测两态都是 686。
        // 原报告里"整窗最小宽度被顶到 900+、窗口缩不回去"那半句因此**不成立**，
        // 下面的断言照这个事实写，别照着那句话改。
        //
        // NOTE: 底下那条"敲一下嵌套布局"是必须的 —— setText() 之后的 updateGeometry()
        // 只会把**控件所在的**那层布局标脏，而这一格在底栏里那个竖着排的嵌套布局
        // 里，它那份缓存不会跟着失效（不敲的话量到的还是改文字之前那个数）。
        //
        // NOTE: 起这一格**开局就是亮的**（任务中心那一次改的：中间那格平时
        // 也写一句「暂无后台任务」，不再是"没在下载就整格藏起来"）。所以不再需要
        // "量之前 show()、量完 hide() 还回去"那一套 —— 藏着的控件在布局里算 0，
        // 而它现在不藏了。`bar` 取**底栏本体**：它的爹经过这次改动已经是中间那颗
        // 按钮了（`taskSummaryButton`），拿它当"底下那层布局"会敲不到底栏的 layout。
        QWidget *bar = ui.getBottomBar();
        const QString shortText = status->text();
        const int shortWindowHint = window.sizeHint().width();
        const int shortWindowMinHint = window.minimumSizeHint().width();

        status->setText(QStringLiteral("已下载 1234 / 5678 ")
                        + QString(300, QLatin1Char('x')) + QStringLiteral("-超长文件名.jar"));
        const QList<QLayout *> nested = bar->findChildren<QLayout *>();
        for (QLayout *sub : nested) {
            sub->invalidate();
        }
        if (bar->layout() != nullptr) {
            bar->layout()->invalidate();
            bar->layout()->activate();
        }
        QApplication::processEvents();

        check(status->sizeHint().width() <= MainWindowUI::bottomBarStatusTextWidth(),
              "★ E5 300 字符的文件名也顶不大那一格的 sizeHint（上限真的管住了它）",
              QStringLiteral("%1（上限 %2）")
                  .arg(status->sizeHint().width())
                  .arg(MainWindowUI::bottomBarStatusTextWidth()));
        // NOTE: 这一条量的**不只是**底栏那一格 —— 整窗的 sizeHint 是各页里所有控件一起
        // 顶出来的：一段折行的提示文字加长到 1260 px 宽，它自己的 sizeHint 从 240
        // 跳到 480、**整窗**从 818 顶到 1042，而底下的文件名一个字都没动。
        // 要留下的是**这件事本身**：**折行 QLabel 的 sizeHint 宽度是横向 advance
        // 的分档（1/4），不是线性的** —— 随便写长一句话就可能把整窗顶过 900。红了
        // 先去看实例页有没有新加的折行文字，别去查下载那条路。
        // （为什么 900 这个上限仍然成立：窗口宽度是 setupUi() 里写死的
        // resize(900, 620)，sizeHint 只在"有人 adjustSize()"时才生效。）
        // 壳换成「左轨 + 右栏」之后整窗 sizeHint 本身就比 900 宽（轨 64/208 + 各页），这一条
        // 只钉「长文件名不让它变宽」。900 宽下下载页被压的事记在 7a 交回的遗留里（7b/7c 重排）。
        check(window.sizeHint().width() <= shortWindowHint,
              "★ E5 长文件名不会把主窗口的 sizeHint 顶宽",
              QStringLiteral("窗口 sizeHint %1 → %2")
                  .arg(shortWindowHint)
                  .arg(window.sizeHint().width()));
        if (window.minimumSizeHint().width() > 900) {
            for (auto *child : window.findChildren<QWidget *>()) {
                if (child->minimumSizeHint().width() > 500)
                    std::printf("LAYOUT-WIDTH %s %s min=%d hint=%d\n", child->metaObject()->className(),
                        qPrintable(child->objectName()), child->minimumSizeHint().width(), child->sizeHint().width());
            }
        }
        // 这一条钉的是上面 ② 那半句事实：最小宽度**没有**跟着长。哪天有人把
        // setMinimumWidth(kStatusLabelMinWidth) 拿掉，这一条会红 —— 那时候"文字顶宽最小
        // 宽度"才真的会成立，也才轮到这个上限去挡。
        check(window.minimumSizeHint().width() <= shortWindowMinHint,
              "E5 主窗口的最小宽度也不受影响（它是被那格的 setMinimumWidth 钉住的）",
              QStringLiteral("%1").arg(window.minimumSizeHint().width()));

        status->setText(shortText);
        QApplication::processEvents();
    }

    std::printf("===== F. 本次设计依赖的 Qt 行为（用独立控件验） =====\n");
    {
        // 这是整套设计最吃紧的一条假设：设了非空 placeholderText 之后，
        // 往空下拉框里 addItems() 不会自动选中第 0 项。如果不成立，
        // 用户会在没选任何版本的情况下"被动"选中一个版本。
        QComboBox probe;
        probe.setPlaceholderText(QStringLiteral("占位"));
        probe.addItems({QStringLiteral("1.18.2"), QStringLiteral("26.3")});
        check(probe.currentIndex() < 0,
              "设了占位文案时，灌入数据不会自动选中第 0 项（设计前提）",
              QString::number(probe.currentIndex()));

        // fillVersionCombo 的"保住选中项"就靠 findText + setCurrentIndex
        probe.setCurrentIndex(1);
        check(probe.currentText() == QStringLiteral("26.3"), "能选中并读回版本号",
              probe.currentText());
        probe.clear();
        check(probe.currentIndex() < 0 && probe.currentText().isEmpty(),
              QStringLiteral("clear() 之后回到「未选中」且 currentText 为空串（判空依据）"),
              QString("idx=%1 text='%2'").arg(probe.currentIndex()).arg(probe.currentText()));

        probe.addItems({QStringLiteral("1.18.2"), QStringLiteral("26.3")});
        const int restored = probe.findText(QStringLiteral("26.3"));
        probe.setCurrentIndex(restored);
        check(restored == 1 && probe.currentText() == QStringLiteral("26.3"),
              "重灌数据后能按文本把原选中项找回来",
              QString::number(restored));

        // 反向：原来选的版本已经不在清单里，必须停在"未选中"而不是乱选一个
        probe.clear();
        probe.addItems({QStringLiteral("1.20.1")});
        check(probe.findText(QStringLiteral("26.3")) < 0 && probe.currentIndex() < 0,
              "原选中项消失时停在未选中（不回退到第 0 项）",
              QString::number(probe.currentIndex()));

        // ---------------- 显示名和 id 分家（这次重构的最小复现） ----------------
        // 上面那半段（按文本找）在新设计里是**错的**写法，这里把"错了会怎样"钉住：
        // 条目文本是显示名、版本号在 Qt::UserRole 里，按文本找 id 找不到。
        // 这两行不链任何新代码就能抓住"退回按文本查找"的回归。
        probe.clear();
        probe.addItem(QStringLiteral("我的整合包-26.3"), QStringLiteral("26.3"));
        check(probe.currentIndex() < 0,
              "addItem(显示名, id) 之后不会自动选中第 0 项（占位文案仍非空）",
              QString::number(probe.currentIndex()));
        check(probe.findText(QStringLiteral("26.3")) < 0,
              "按裸 id 用 findText 找不到（显示文本是『我的整合包-26.3』）");
        check(probe.findData(QStringLiteral("26.3")) == 0, "按裸 id 用 findData 找得到",
              QString::number(probe.findData(QStringLiteral("26.3"))));
        probe.setCurrentIndex(0);
        check(probe.currentData().toString() == QStringLiteral("26.3") &&
                  probe.currentText() == QStringLiteral("我的整合包-26.3"),
              "选中后 currentText 是显示名、currentData 是裸 id",
              probe.currentText() + " / " + probe.currentData().toString());

        // addItems(QStringList) 是**不设** Qt::UserRole 的 —— 生产代码里若有人
        // 图省事把逐条 addItem 换回 addItems，所有条目的 currentData() 都会是
        // 无效 QVariant，selectedVersionId() 一路返回空串，启动按钮就再也启不动。
        probe.clear();
        probe.addItems({QStringLiteral("26.3")});
        check(probe.itemData(0).isValid() == false,
              "addItems(QStringList) 不设 Qt::UserRole（所以生产代码不能用它）",
              probe.itemData(0).toString());
    }

    std::printf("===== F2. 在两个非空占位文案之间来回切换（本次新引入的 Qt 依赖） =====\n");
    {
        // MainWindow::refreshInstalledVersions() 会按"有没有可选版本"在两个
        // 占位串之间切换。这条 Qt 依赖本身要有断言盖着。
        QComboBox probe;
        probe.setPlaceholderText(MainWindowUI::noInstalledVersionPlaceholder());
        probe.addItems({QStringLiteral("1.18.2"), QStringLiteral("26.3")});
        check(probe.currentIndex() < 0, "起始状态：没选中");

        const QSize before = probe.sizeHint();
        probe.setPlaceholderText(MainWindowUI::chooseVersionPlaceholder());
        check(probe.currentIndex() < 0, "换成『请选择一个版本』后仍然没选中",
              QString::number(probe.currentIndex()));
        probe.setPlaceholderText(MainWindowUI::noInstalledVersionPlaceholder());
        check(probe.currentIndex() < 0, "换回『暂无已安装版本』后仍然没选中",
              QString::number(probe.currentIndex()));

        // 反向再来一次：先选上，再切占位文案，选中项不能被影响
        probe.setCurrentIndex(1);
        probe.setPlaceholderText(MainWindowUI::chooseVersionPlaceholder());
        check(probe.currentIndex() == 1 && probe.currentText() == QStringLiteral("26.3"),
              "已有选中项时换占位文案不改动选中项",
              QString("idx=%1 text=%2").arg(probe.currentIndex()).arg(probe.currentText()));

        // 宽度不许跳：两个占位串都很短（6/7 字），远低于 minimumContentsLength(16)，
        // 所以 sizeHint 不该动。这条守住"窗口没有因为换文案抖一下"。
        probe.setCurrentIndex(-1);
        probe.setPlaceholderText(MainWindowUI::noInstalledVersionPlaceholder());
        const QSize a = probe.sizeHint();
        probe.setPlaceholderText(MainWindowUI::chooseVersionPlaceholder());
        const QSize b = probe.sizeHint();
        check(a.width() == b.width(), "两个占位文案之间切换时宽度不变",
              QString("%1 vs %2").arg(a.width()).arg(b.width()));
        check(before.width() == b.width(), "和初始那次量到的宽度也一致",
              QString("%1 vs %2").arg(before.width()).arg(b.width()));

        // 顺手把那个坑编成特征测试：**空**占位文案在有内容的框上会跳到第 0 项。
        // 这条断言存在的意义是让"绝不要 setPlaceholderText(QString())"成为可执行的
        // 规矩 —— 它一旦在某台 Qt 上不再成立，说明我们对 qcombobox.cpp 的理解过期了。
        probe.setCurrentIndex(-1);
        probe.setPlaceholderText(QString());
        check(probe.currentIndex() == 0,
              "特征测试：空占位文案会让下拉框静默跳到第 0 项（所以绝不能这么做）",
              QString::number(probe.currentIndex()));
    }

    std::printf("===== G. 「启动游戏」两行文字（本次新增） =====\n");
    {
        const int oneLine = startBtn->sizeHint().height();
        startBtn->setText(QStringLiteral("启动游戏\n26.2-snapshot-1"));
        startBtn->updateGeometry();
        QApplication::processEvents();
        const int twoLines = startBtn->sizeHint().height();
        check(twoLines > oneLine, "两行文字的 sizeHint 比一行高（QPushButton 认了这个换行）",
              QString("1 行=%1px 2 行=%2px").arg(oneLine).arg(twoLines));
        check(startBtn->height() >= twoLines,
              "实际高度容得下两行（布局没把它压扁）",
              QString("h=%1 need=%2").arg(startBtn->height()).arg(twoLines));
        check(startBtn->text().contains(QLatin1Char('\n')), "换行符保留在 text() 里");

        // 版本号很长时第一行"启动游戏"必须仍然完整在第一行
        check(startBtn->text().startsWith(QStringLiteral("启动游戏\n")),
              "第一行固定是「启动游戏」", startBtn->text());

        // 显示名可能比版本号长得多（"我的整合包-1.20.1-Forge-47.2.0" 之类），
        // 而且 wtfml.meta 是用户手写的、长度不设防（formatVersionName 里封顶 64）。
        // 这一段守住"无论多长，按钮都还是两行"—— 一旦变成三行，按钮会突然变高，
        // 挤压下面版本区的空间；mw_check 的 buttonVersionLine() 也会取错行。
        startBtn->setText(QStringLiteral("启动游戏\n") +
                          QString(40, QLatin1Char('x')) + QStringLiteral("-1.20.1-Forge"));
        startBtn->updateGeometry();
        QApplication::processEvents();
        check(startBtn->text().count(QLatin1Char('\n')) == 1,
              "40 字符长的显示名仍是正好两行（第一行固定，超出部分归第二行）",
              QString::number(startBtn->text().count(QLatin1Char('\n'))));
        check(startBtn->height() >= startBtn->sizeHint().height(),
              "按钮实际高度容得下这个长度的两行",
              QString("h=%1 need=%2")
                  .arg(startBtn->height())
                  .arg(startBtn->sizeHint().height()));
        check(startBtn->text().section(QLatin1Char('\n'), 0, 0) == QStringLiteral("启动游戏"),
              "第二行再长也不影响第一行");

        startBtn->setText(QStringLiteral("启动游戏"));
        startBtn->updateGeometry();
        QApplication::processEvents();
    }

    std::printf("===== H. 下拉框的宽度都不随占位文案变 =====\n");
    {
        // 下拉框都用 minimumContentsLength 把内容长度钉死了 —— 长显示名只会在框内被
        // 省略号截断（这是刻意的：不能让一个长名字把窗口顶宽）。这条断言守住"换占位
        // 文案时窗口不抖"。
        const QSize base = installed->sizeHint();
        installed->setPlaceholderText(MainWindowUI::chooseVersionPlaceholder());
        const QSize swapped = installed->sizeHint();
        installed->setPlaceholderText(MainWindowUI::noInstalledVersionPlaceholder());
        const QSize back = installed->sizeHint();
        check(base.width() == swapped.width() && swapped.width() == back.width(),
              "两个占位文案之间来回切换，宽度不变",
              QString("%1 / %2 / %3").arg(base.width()).arg(swapped.width()).arg(back.width()));

        // 加载器那棵树：会被写进**条目**的那些字必须都非空**而且互不相同**。 起
        // 那一格从一条「自动（推荐）」变成两条（最新版 / 稳定版）+ 两种第二行小字
        // （「没有正式版」「测试版」），再加上原来的「正在获取…」「列表没取到」——
        // 空的那句是一个看不见的空行（用户以为那底下什么都没有）；两句一样的则分不出
        // "还在拉"和"拉失败了"，也分不出用户点的是哪一档。
        // NOTE: 判据是**逐对**比，不是 join 出来比 —— 全部非空时 join 必然非空，
        // 那样写这条会永远绿（loaderViewer 那段就是这么被坑过一次的）。
        const QString latestText = MainWindowUI::loaderVersionLatestText();
        const QString stableText = MainWindowUI::loaderVersionStableText();
        const QString noStableText = MainWindowUI::loaderVersionStableMissingText();
        const QString betaText = MainWindowUI::loaderVersionBetaSubText();
        const QString shortLatestText = MainWindowUI::loaderVersionChannelShortText(true);
        const QString shortStableText = MainWindowUI::loaderVersionChannelShortText(false);
        const QString fetching = MainWindowUI::loaderVersionFetchingText();
        const QString failed = MainWindowUI::loaderVersionFailedText();
        const QStringList rowTexts{latestText,  stableText,      noStableText,   betaText,
                                   shortLatestText, shortStableText, fetching, failed};
        int emptyTexts = 0;
        int samePairs = 0;
        for (int i = 0; i < rowTexts.size(); ++i) {
            emptyTexts += rowTexts.at(i).isEmpty() ? 1 : 0;
            for (int j = i + 1; j < rowTexts.size(); ++j) {
                samePairs += rowTexts.at(i) == rowTexts.at(j) ? 1 : 0;
            }
        }
        // 那两行**不带括号**：要求"那个稳定版（release）那个括号啥的是给你看的，
        // 不用实际放在界面里面" —— 括号里那些是解释"最新版连 alpha/beta 一起算""稳定版就是
        // release"，写给读代码的人看，界面上就四个字 / 五个字。这一条钉的是"别哪天又顺手加回来"。
        const QStringList bracketShapes{QStringLiteral("（"), QStringLiteral("(")};
        int bracketedRows = 0;
        for (const QString &shape : bracketShapes) {
            bracketedRows += latestText.contains(shape) ? 1 : 0;
            bracketedRows += stableText.contains(shape) ? 1 : 0;
        }
        check(bracketedRows == 0,
              "★ 那两行界面上不带括号（alpha/beta / release 那些话是给读代码的人看的）",
              QStringLiteral("「%1」/「%2」").arg(latestText, stableText));
        check(emptyTexts == 0 && samePairs == 0,
              "那八句会被写进条目的字（两档 + 两种小字 + 两种档名 + 正在获取 + 列表没取到）"
              "都非空且两两不同",
              rowTexts.join(QStringLiteral(" / ")));
        // 而且**长的那些不许把那棵树（连带整个窗口）撑宽**：这几句是运行时才出现的，
        // 撑宽的表现是"点了某条加载器，整个下载页横向抽一下"。树是横向铺满的，
        // 决定这件事的是它的最小宽度 —— 那就量这个：往底下塞一条最长的子条目，
        // 最小宽度不许动。
        if (loaderTree != nullptr && loaderTree->topLevelItemCount() > 0) {
            const int before = loaderTree->minimumSizeHint().width();
            // 拿**最长**的那句去顶（现在就数「下载最新版」最长 —— 那两档是
            // 这次新加进来的，所以这条顺便替它们守着"页面不横向抽"）。
            QString longest = failed;
            for (const QString &text : rowTexts) {
                if (text.size() > longest.size()) {
                    longest = text;
                }
            }
            auto *probe = new QTreeWidgetItem(loaderTree->topLevelItem(0));
            probe->setText(MainWindowUI::loaderTreeKeyColumn(), longest);
            loaderTree->topLevelItem(0)->setExpanded(true);
            QApplication::processEvents();
            const int after = loaderTree->minimumSizeHint().width();
            delete probe;
            loaderTree->topLevelItem(0)->setExpanded(false);
            QApplication::processEvents();
            check(after <= before,
                  "那几句长文案不会把那棵树的最小宽度顶大（点了加载器页面不横向抽）",
                  QStringLiteral("%1 → %2").arg(before).arg(after));
        }
        // 顺便钉一句：那棵树横向是**铺满那一栏**的（不是"有多长就多宽"），
        // 上面那条"最小宽度不变"才是真的有用。
        check(loaderTree != nullptr &&
                  loaderTree->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding,
              "那棵树横向铺满那一栏",
              loaderTree != nullptr
                  ? QString::number(int(loaderTree->sizePolicy().horizontalPolicy()))
                  : QStringLiteral("<null>"));
    }

    std::printf("===== I. 底栏两格（左：运行状态，中：任务摘要/进度；右：工具箱。账号那格"
                "收进主页卡片） =====\n");

    if (const int result = UiBottomBarGeometryChecks(ui, bar); result != 0) return result;

    std::printf("===== I2. 底栏中间那颗按钮 = 任务中心的入口 =====\n");

    if (const int result = UiTaskCenterChecks(window, ui, tabs); result != 0) return result;

    std::printf("===== I5. 取消 / 重试那两个入口 + 任务详情页 =====\n");

    if (const int result = UiTaskActionsChecks(window, ui); result != 0) return result;

    std::printf("===== I3. 左下角运行状态文案（未运行 / %1 个进程运行中） =====\n");
    {
        // 左下角那一格只留「未运行」和「%1 个进程运行中」（客户端和服务端合计），
        // 启动中和异常退出只在任务中心提示。
        check(MainWindowUI::runStateRunningCountText(1) == QStringLiteral("1 个进程运行中")
                  && MainWindowUI::runStateRunningCountText(2) == QStringLiteral("2 个进程运行中"),
              "★ 运行进程数文案统一用「%1 个进程运行中」（客户端与服务端合计）",
              MainWindowUI::runStateRunningCountText(2));
        check(MainWindowUI::runStateRunningCountText(2) !=
                  MainWindowUI::runStateRunningCountText(3),
              "个数不一样，那句话就不一样（不是写死的常量）");

        check(MainWindowUI::runStateNotRunningText() == QStringLiteral("未运行"),
              "一个都没在跑时那句是「未运行」（服务端也算）："
              "字宽还要落在 bottomBarRunStateTextWidth() 那一档里，再长就被截成省略号",
              MainWindowUI::runStateNotRunningText());
        check(MainWindowUI::runStateNotRunningText() != MainWindowUI::runStateRunningCountText(1)
                  && MainWindowUI::runStateNotRunningText() !=
                         MainWindowUI::runStateRunningCountText(2),
              "「未运行」和「%1 个进程运行中」是不同的话");
        check(MainWindowUI::runStateNotRunningText() != QStringLiteral("实例未运行"),
              "（不再是「实例未运行」）",
              MainWindowUI::runStateNotRunningText());

        // 多开最要紧的一件事是"哪个实例是哪个账号"，所以第二行里每个实例都要带身份。
        check(MainWindowUI::runStateDetailWithAccount(QStringLiteral("我的包"),
                                                      QStringLiteral("Alice")) ==
                  QStringLiteral("我的包（Alice）"),
              "第二行里每个实例 = 显示名 +（账号）",
              MainWindowUI::runStateDetailWithAccount(QStringLiteral("我的包"),
                                                      QStringLiteral("Alice")));
        check(MainWindowUI::runStateDetailWithAccount(QStringLiteral("A"),
                                                      QStringLiteral("B")) !=
                  MainWindowUI::runStateDetailWithAccount(QStringLiteral("B"),
                                                          QStringLiteral("A")),
              "名字和账号的位置不能互换（换了就读成另一个实例了）");

        // 实例页那棵树上"这个实例正在跑"的后缀。
        check(!MainWindowUI::instanceRunningSuffixText().isEmpty(),
              "树上的「（运行中）」后缀非空", MainWindowUI::instanceRunningSuffixText());
        check(!MainWindowUI::instanceRunningSuffixText().contains(QLatin1Char(' ')),
              "后缀里没有空格（它是直接贴在显示名后面的，不是另起一段）",
              MainWindowUI::instanceRunningSuffixText());
        check(!MainWindowUI::instanceRunningSuffixText().contains(
                  QLatin1Char('(')) &&
                  !MainWindowUI::instanceRunningSuffixText().contains(QLatin1Char(')')),
              "用的是全角括号（半角括号在列表里看着像代码）",
              MainWindowUI::instanceRunningSuffixText());
    }

    std::printf("===== I4. 底栏两头那两颗按钮：点一下**向上**弹一块面板"
                " =====\n");

    if (const int result = UiBottomBarPopoversChecks(window, ui, tabs); result != 0) return result;

    std::printf("===== I5. 两颗按钮的高度、中间那根条的宽度 =====\n");
    {
        // 要求：「左右那个…只有一行的高度 文字和图像只能挤着（显示不全）」
        // 「中间进度条现在奇小无比」，还猜了一句「估计是控件类型的问题」——
        // 猜对了：QPushButton 竖直方向的 sizePolicy 默认是 Fixed，而它这会儿的
        // 高度提示只有一行字（QSS 把 padding/border 清零了），于是那一格只有
        // 12 px 高，两行字被压成各 6 px。
        // 这两条都是**量出来的**结论，所以钉进测试：再有人把按钮的高度提示当回事、
        // 或者把两头那两格加宽回去，这里当场红。
        QPushButton *runBtn = ui.getRunStateButton();
        QPushButton *accBtn = ui.getToolboxButton();
        QLabel *runState = ui.getRunStateLabel();
        QLabel *runDetail = ui.getRunStateDetailLabel();
        QLabel *mark = ui.getRunStateMarkLabel();
        QProgressBar *progress = ui.getProgressBar();
        QLabel *status = ui.getStatusLabel();
        // 轨底的文字只在展开的轨（窗宽 ≥ 1024）里显示，这一节量展开那一档。
        const QSize i5Size = window.size();
        window.resize(1280, 800);
        QApplication::processEvents();
        if (runBtn != nullptr && accBtn != nullptr && runState != nullptr &&
            runDetail != nullptr && progress != nullptr && status != nullptr) {
            // 布景摆成"正在下载 + 有两个实例在跑"（两行字都在、进度条也在）
            progress->show();
            status->show();
            runState->show();
            runDetail->show();
            // 方块也摆上：它和第一行是**同一个横排**，不显示的话那一行只剩文字、
            // 量出来的行高是"没有方块"那一档（这一节量的正是"字有没有被压扁"）。
            if (mark != nullptr) {
                mark->show();
            }
            runState->setText(MainWindowUI::runStateRunningCountText(1));
            runDetail->setText(QStringLiteral("我的整合包-1.20.1-NeoForge"));
            status->setText(QStringLiteral("已下载 2814 / 5123  client.jar"));
            progress->setRange(0, 5123);
            progress->setValue(2814);
            QApplication::processEvents();

            // ① 那两行字**没被压扁**：每一行的高度都不小于它自己要的高度。
            // （12 px 的格子里这两行只有 6 px 高 —— 就是用户看到的"挤着、显示不全"。）
            check(runState->height() >= runState->sizeHint().height() &&
                      runDetail->height() >= runDetail->sizeHint().height(),
                  "左边那两行字没被压扁（各行的高 ≥ 它自己的高）",
                  QStringLiteral("实际 %1+%2 / 需要 %3+%4")
                      .arg(runState->height()).arg(runDetail->height())
                      .arg(runState->sizeHint().height()).arg(runDetail->sizeHint().height()));
            check(runState->height() >= runState->fontMetrics().height(),
                  "第一行至少装得下一个字高（不然字会被切掉半行）",
                  QStringLiteral("%1 px / 字高 %2 px")
                      .arg(runState->height()).arg(runState->fontMetrics().height()));

            // ② 轨底两颗按钮（运行状态 / 工具箱）横向铺满轨底、竖直方向装得下两行字。
            check(runBtn->height() >= runState->sizeHint().height() + runDetail->sizeHint().height(),
                  "运行状态那颗有两行字那么高（不是一行）",
                  QStringLiteral("按钮高 %1 / 两行字 %2+%3")
                      .arg(runBtn->height()).arg(runState->sizeHint().height())
                      .arg(runDetail->sizeHint().height()));
            check(qAbs(runBtn->width() - accBtn->width()) <= 1 && runBtn->width() * 10 >= bar->width() * 8,
                  "两颗按钮同宽、基本铺满轨底",
                  QStringLiteral("运行 %1 / 工具箱 %2 / 轨底 %3")
                      .arg(runBtn->width()).arg(accBtn->width()).arg(bar->width()));

            progress->hide();
            // 那一格（和那句占位话）开局就是亮的 ⇒ 还成开局那一份，不能 hide。
            ui.setBottomBarTaskSummary(MainWindowUI::BottomBarTaskSummary());
            status->setText(MainWindowUI::taskSummaryNoTaskText());
            runState->hide();
            runDetail->hide();
            if (mark != nullptr) {
                mark->hide();
            }
            QApplication::processEvents();
        }
        window.resize(i5Size);
        QApplication::processEvents();
    }

    std::printf("===== J.「第二行要不要显示实例名」的真值表 =====\n");
    {
        // 规则：**只有**"停在主页、且下拉框选中的就是正在跑的那个实例"这一种情况
        // 不显示第二行（眼前这个界面说的就是它）；其余情况都要靠名字说清楚跑的是谁。
        const auto shows = [](bool home, const QString &selected, const QString &running) {
            return MainWindowUI::runStateShowsDetail(home, selected, running, false);
        };
        // 「正在启动」那一档单独一个判据（第四个参数），**和上面这些条件全都无关**：
        // 第一行那时候是「<实例名>启动中...」，名字已经在里面了。
        const auto launching = [](bool home, const QString &selected, const QString &running) {
            return MainWindowUI::runStateShowsDetail(home, selected, running, true);
        };
        check(!shows(true, QStringLiteral("1.20.1"), QStringLiteral("1.20.1")),
              "主页 + 选中的就是在跑的那个 → 只显示一行");
        check(shows(true, QStringLiteral("1.18.2"), QStringLiteral("1.20.1")),
              "主页 + 选中的是另一个版本 → 要显示第二行（说清跑的是谁）");
        check(shows(true, QString(), QStringLiteral("1.20.1")),
              "主页 + 什么都没选（跑的是别的）→ 要显示第二行");
        check(shows(false, QStringLiteral("1.20.1"), QStringLiteral("1.20.1")),
              "别的选项卡 + 选中的就是在跑的那个 → 要显示第二行（用户看不见那个下拉框了）");
        check(shows(false, QString(), QStringLiteral("1.20.1")),
              "别的选项卡 + 没选 → 要显示第二行");
        check(!shows(true, QString(), QString()),
              "两边都空（没在跑）也不会因此多画一行（整格本来就是藏的）");
        // 启动中：四种组合**一律** false（这一档压根不看那三个参数）
        check(!launching(true, QStringLiteral("1.20.1"), QStringLiteral("1.20.1")) &&
                  !launching(true, QStringLiteral("1.18.2"), QStringLiteral("1.20.1")) &&
                  !launching(true, QString(), QStringLiteral("1.20.1")) &&
                  !launching(false, QStringLiteral("1.20.1"), QStringLiteral("1.20.1")) &&
                  !launching(false, QString(), QStringLiteral("1.20.1")) &&
                  !launching(true, QString(), QString()),
              "正在启动时第二行**一律不显示**（名字在第一行那句话里）");
    }

    std::printf("===== J2. 底栏那颗状态方块的四档 =====\n");

    if (const int result = UiRunStateColorsChecks(); result != 0) return result;

    std::printf("===== J3. 列表行（设计稿 .row） =====\n");
    if (const int result = UiListRowDelegateChecks(); result != 0) return result;

    std::printf("===== K. 每页的左右两栏（主页右栏定宽、左栏吃剩下的；另三页是固定宽度的"
                "侧栏） =====\n");

    if (const int result = UiColumnProportionsChecks(installed, startBtn, folderList, instanceList, tabs, surfaceValueOf, inSurface, surfaceOf); result != 0) return result;

    std::printf("\n===== K2. 页面骨架：页头 / 正文面 / 侧栏面（四页一颗 QGroupBox 都不许有） =====\n");
    {
        // 用户 那份改版文档给的是**同一个骨架**（原图那两块）：
        // 侧栏 | 页面标题 + 操作
        // | 页面说明 / 路径 / 当前状态
        // |------------------------------ ← 这一条之下才是正文
        // | 主内容
        // 这一节量的就是这几层**在树上的位置关系**（页头在外面、正文面在页头下面、
        // 标题/说明挂的是哪一档文字角色），以及那条"减框"规矩的最后一道闸：**产品那四页
        // 里一颗 `QGroupBox` 都不许有**（要求：「右侧只有一个主要内容表面（不再用多个
        // QGroupBox 套起来）」）。几何类的那几条（谁在上面、谁多宽）在上面 K 节。
        struct SkeletonCase {
            const char *key;
            int index;
            QString title;
            /// 说明那一行的 objectName。★ 实例页**不是** `pageSubtitleName()`：那一页的
            /// 说明就是原来那个「当前文件夹：…」标签（`instanceFolderLabel`），改版时把页头
            /// 那个说明标签**接过来用**了（文案、灌数据那条路、验证工程找它的名字一处都没动，
            /// 见 MainWindowUI.cpp 里那段）—— 只换 objectName 会牵连一串别处的 findChild。
            QString subtitleName;
        };
        const QList<SkeletonCase> skeleton = {
            {"instance", MainWindowUI::instancesTabIndex(), MainWindowUI::instancePageTitle(),
             MainWindowUI::instanceFolderLabelName()},
            {"download", MainWindowUI::downloadTabIndex(), MainWindowUI::downloadPageTitle(),
             MainWindowUI::pageSubtitleName(QStringLiteral("download"))},
        };
        const QByteArray textRoleKey = Theme::textRolePropertyName().toUtf8();
        int groupsLeft = 0;
        for (const SkeletonCase &kase : skeleton) {
            tabs->setCurrentIndex(kase.index);
            QApplication::processEvents();
            QWidget *page = tabs->widget(kase.index);
            const QString key = QString::fromUtf8(kase.key);
            QWidget *header = page->findChild<QWidget *>(MainWindowUI::pageHeaderName(key));
            QLabel *title = page->findChild<QLabel *>(MainWindowUI::pageTitleName(key));
            QLabel *subtitle = page->findChild<QLabel *>(kase.subtitleName);
            QFrame *workspace = surfaceOf(page, Theme::workspaceSurfaceValue());
            QFrame *sidebar = surfaceOf(page, Theme::sidebarSurfaceValue());
            check(header != nullptr && title != nullptr && subtitle != nullptr &&
                      workspace != nullptr && sidebar != nullptr,
                  QStringLiteral("%1 页：页头 / 标题 / 说明 / 正文面 / 侧栏面五样都在").arg(key),
                  QStringLiteral("header=%1 title=%2 subtitle=%3 workspace=%4 sidebar=%5")
                      .arg(header != nullptr ? 1 : 0)
                      .arg(title != nullptr ? 1 : 0)
                      .arg(subtitle != nullptr ? 1 : 0)
                      .arg(workspace != nullptr ? 1 : 0)
                      .arg(sidebar != nullptr ? 1 : 0));
            // 标题就是这一页的名字，而且挂的是「页面标题」那一档文字角色 —— 样式表按这个
            // 属性排字号/颜色（产品那边 `markTextRole()` 写的就是同一对字符串）。
            check(title != nullptr && title->text() == kase.title &&
                      title->property(textRoleKey.constData()).toString() ==
                          Theme::pageTitleTextRoleValue(),
                  QStringLiteral("%1 页：标题写的就是这一页的名字，且挂着「页面标题」那一档角色")
                      .arg(key),
                  title != nullptr ? QStringLiteral("「%1」角色「%2」")
                                         .arg(title->text())
                                         .arg(title->property(textRoleKey.constData()).toString())
                                   : QStringLiteral("<null>"));
            check(subtitle != nullptr &&
                      subtitle->property(textRoleKey.constData()).toString() ==
                          Theme::secondaryTextRoleValue(),
                  QStringLiteral("%1 页：说明那一行挂着「次要」那一档角色").arg(key),
                  subtitle != nullptr
                      ? subtitle->property(textRoleKey.constData()).toString()
                      : QStringLiteral("<null>"));
            // 页头**在正文面外面**（面里只有内容）。装进面里的话，那一圈底就把标题和
            // 内容一起框住了 —— 用户文档第一条要减掉的正是它。
            check(header != nullptr && workspace != nullptr && !workspace->isAncestorOf(header),
                  QStringLiteral("%1 页：页头长在正文面**外面**（那一圈底不框标题）").arg(key));
            // 页头在正文面**上面**：两个都换算到页面坐标上比下边缘/上边缘。
            if (header != nullptr && workspace != nullptr) {
                const QRect headerBox(header->mapTo(page, QPoint(0, 0)), header->size());
                const QRect workspaceBox(workspace->mapTo(page, QPoint(0, 0)), workspace->size());
                check(headerBox.y() + headerBox.height() <= workspaceBox.y(),
                      QStringLiteral("%1 页：页头在正文面上面（标题先出来、内容在后）").arg(key),
                      QStringLiteral("header.bottom=%1 workspace.y=%2")
                          .arg(headerBox.y() + headerBox.height())
                          .arg(workspaceBox.y()));
            }
            // 侧栏和正文面是**同一层**上的两栏：谁也不许套着谁。
            check(!sidebar->isAncestorOf(workspace) && !workspace->isAncestorOf(sidebar),
                  QStringLiteral("%1 页：侧栏和正文是两个并排的面（不是谁套着谁）").arg(key));
            groupsLeft += page->findChildren<QGroupBox *>().size();
        }
        {
            // 设置页是单栏：页头（标题 + 页签 + 搜索）+ 滚动表单，没有侧栏和正文面。
            tabs->setCurrentIndex(MainWindowUI::settingsTabIndex());
            QApplication::processEvents();
            QWidget *page = tabs->widget(MainWindowUI::settingsTabIndex());
            QLabel *title = page->findChild<QLabel *>(MainWindowUI::pageTitleName(QStringLiteral("settings")));
            check(page->findChild<QWidget *>(MainWindowUI::pageHeaderName(QStringLiteral("settings"))) != nullptr &&
                      title != nullptr && title->text() == MainWindowUI::settingsPageTitle() &&
                      title->property(textRoleKey.constData()).toString() == Theme::pageTitleTextRoleValue(),
                  "settings 页：页头标题「设置」挂着「页面标题」角色");
            groupsLeft += page->findChildren<QGroupBox *>().size();
        }
        check(groupsLeft == 0,
              "★ 产品那四页里一颗 QGroupBox 都没有了（用户那份文档第一条「减少框」：整页只留"
              "侧栏和正文两个面，小节改用「标题 + 内容卡片」）",
              QStringLiteral("还剩下 %1 颗").arg(groupsLeft));

        // 侧栏那一列导航的**选中样式是代理画的**（浅主题色底 + 左边一条 3 px 主题色指示条 +
        // 主题色深色字），所以那两列必须是带 `navRow` 的那一档代理 —— 换回普通代理的话，
        // 选中项退回 Fusion 那个蓝、指示条整个消失，而样式表里没有一条 `::item` 规则能兜住。
        // 两棵树的代理必须是**没开**这一档的（它们不是导航，选中色走另一套）。
        auto *downloadCategories = ui.getDownloadCategoryList();
        const auto navRowOf = [](QAbstractItemView *view) {
            return view != nullptr ? qobject_cast<VersionTreeDelegate *>(view->itemDelegate()) !=
                                         nullptr &&
                                         qobject_cast<VersionTreeDelegate *>(view->itemDelegate())->navRow()
                                   : false;
        };
        check(navRowOf(downloadCategories),
              "★ 下载页左栏那列导航挂的是「侧栏导航」那一档代理（选中 = 浅主题色底，不是 Fusion 那个蓝）",
              QStringLiteral("download=%1").arg(navRowOf(downloadCategories) ? 1 : 0));
        check(!navRowOf(ui.getVersionTree()) && !navRowOf(ui.getLoaderTree()),
              "而两棵树（版本树 / 加载器树）**没开**这一档（它们的选中色不是导航那一套）",
              QStringLiteral("version=%1 loader=%2")
                  .arg(navRowOf(ui.getVersionTree()) ? 1 : 0)
                  .arg(navRowOf(ui.getLoaderTree()) ? 1 : 0));
    }

    std::printf("===== L. 设置页（左：一列分类，右：堆叠页；栏内才是 左名右控件 3:7） =====\n");

    if (const int result = UiSettingsLayoutChecks(ui, tabs); result != 0) return result;

    std::printf("===== M. 实例列表（树：组 → 实例） =====\n");

    if (const int result = UiInstanceGroupingChecks(ui, instanceList); result != 0) return result;

    // N. 分组设置对话框拆成了单独的 ui_check_group_editor（ctest: wtfml.group-editor）。

    std::printf("===== U. 实例设置对话框（控件 → 值，且不碰磁盘） =====\n");

    if (const int result = UiInstanceOverridesChecks(); result != 0) return result;

    if (const int result = UiContentPagesChecks(); result != 0) return result;

    if (const int result = UiConfigEditorChecks(); result != 0) return result;


    // ---- V. JavaMismatchDialog：启动前那个三岔口 ----
    // **直接构造**它，不走 exec()：产品路径上它是模态的，离屏驱动不了（会一直等），
    // 于是"按钮点下去落到哪个分支"这段判断就永远没有断言盯着 —— 构造就崩的
    // 问题只有直接构造才量得到。
    {
        using Dlg2 = JavaMismatchDialog;

        Dlg2 dialog(QStringLiteral("我的整合包"), /*expectedMajor=*/21, /*actualMajor=*/8,
                    QStringLiteral("1.8.0_301"),
                    QStringLiteral("D:/Java/jdk8/bin/java.exe"));

        // ---- V1 三个控件 + 正文 ----
        auto *text = dialog.findChild<QLabel *>(Dlg2::textLabelName());
        auto *cont = dialog.findChild<QPushButton *>(Dlg2::continueButtonName());
        auto *useMatching = dialog.findChild<QPushButton *>(Dlg2::useMatchingButtonName());
        auto *cancel = dialog.findChild<QPushButton *>(Dlg2::cancelButtonName());
        auto *dontRemind = dialog.findChild<QCheckBox *>(Dlg2::dontRemindCheckName());
        check(text != nullptr && cont != nullptr && useMatching != nullptr && cancel != nullptr &&
                  dontRemind != nullptr,
              "V1 正文/两个动作/取消/勾选框都按 objectName 找得回来");
        check(text != nullptr && text->text().contains(QStringLiteral("我的整合包")),
              "V1 正文里有**实例名**（用户得知道是哪个实例出事了）",
              text != nullptr ? text->text() : QString());
        check(text != nullptr &&
                  text->text().contains(QStringLiteral("Java 8（1.8.0_301）")) &&
                  text->text().contains(QStringLiteral("Java 21")),
              "V1 正文里同时有实例指定的版本和这个版本要求的版本");
        check(text != nullptr && text->text().contains(QStringLiteral("D:/Java/jdk8/bin/java.exe")),
              "V1 正文里露出那条路径（用户要去改的就是它）");

        // release 快路径读出版本号时**不回填 versionOutput**，所以版本串是空的这一种
        // 必须单独拼一次：否则正文里会冒出「Java 」。
        Dlg2 noText(QStringLiteral("我的整合包"), 21, 8, QString(),
                    QStringLiteral("D:/Java/jdk8/bin/java.exe"));
        auto *noTextLabel = noText.findChild<QLabel *>(Dlg2::textLabelName());
        check(noTextLabel != nullptr && noTextLabel->text().contains(QStringLiteral("Java 8")) &&
                  !noTextLabel->text().contains(QStringLiteral("（）")),
              "V1 没有版本串时只写主版本号，不拼出「Java （）」",
              noTextLabel != nullptr ? noTextLabel->text() : QString());

        // ---- V2 初值：没点任何按钮 = 不启动；勾选框默认不勾 ----
        check(dialog.choice() == Dlg2::Choice::Abort, "V2 刚构造出来是 Abort（= 不启动）");
        check(!dialog.dontRemindAgain(),
              "V2 「以后不再提醒」默认**不勾**（一个回车不该顺手改掉一个长期偏好）");

        // ---- V3 两个动作各落到哪个 Choice ----
        // 默认按钮是「仍然用这个 Java 启动」：误按回车只该继续启动，不该改配置。
        check(cont != nullptr && cont->isDefault(),
              "V3 「仍然用这个 Java 启动」是默认按钮（回车不该改写用户的配置）");
        check(useMatching != nullptr && !useMatching->isDefault(),
              "V3 「改用匹配的 Java 启动」不是默认按钮");
        useMatching->click();
        check(dialog.choice() == Dlg2::Choice::UseMatchingJava,
              "V3 点「改用匹配的」→ UseMatchingJava");
        check(dialog.result() == QDialog::Accepted, "V3 而且对话框是正常收场（Accepted）");

        {
            Dlg2 other(QStringLiteral("我的整合包"), 21, 8, QStringLiteral("1.8.0_301"),
                       QStringLiteral("D:/Java/jdk8/bin/java.exe"));
            other.findChild<QCheckBox *>(Dlg2::dontRemindCheckName())->setChecked(true);
            check(other.dontRemindAgain(), "V3 勾上之后读出来是 true");
            other.findChild<QPushButton *>(Dlg2::continueButtonName())->click();
            check(other.choice() == Dlg2::Choice::UseInstanceJava,
                  "V3 点「仍然用这个 Java 启动」→ UseInstanceJava");
        }
        {
            // 取消 / 关窗：**不启动**，而且不逼用户二选一 —— 他可能只是想先去把那个
            // Java 装好再回来。
            Dlg2 third(QStringLiteral("我的整合包"), 21, 8, QStringLiteral("1.8.0_301"),
                       QStringLiteral("D:/Java/jdk8/bin/java.exe"));
            third.findChild<QPushButton *>(Dlg2::cancelButtonName())->click();
            check(third.choice() == Dlg2::Choice::Abort, "V3 点取消 → Abort（不启动）");
            check(third.result() == QDialog::Rejected, "V3 而且对话框是 Rejected 收场");
        }
        {
            Dlg2 fourth(QStringLiteral("我的整合包"), 21, 8, QStringLiteral("1.8.0_301"),
                        QStringLiteral("D:/Java/jdk8/bin/java.exe"));
            fourth.reject();
            check(fourth.choice() == Dlg2::Choice::Abort, "V3 Esc/关窗（reject）也是 Abort");
        }
    }

    std::printf("\n===== W. 皮肤模型（主页右栏那个 3D 小人） =====\n");
    if (const int result = UiSkinMeshChecks(); result != 0) return result;


    std::printf("\n===== X. 「账号与外观」对话框（账号列表 / 披风列表 / 忙碌态） =====\n");
    if (const int result = UiAccountSkinChecks(); result != 0) return result;

    std::printf("\n===== Y. 主页右栏那个入口 + 底栏名字下面那一行 =====\n");
    {
        // 主页得在最前面 —— 藏在别的选项卡后面的控件几何停在旧值，量它等于没量。
        tabs->setCurrentIndex(0);
        QApplication::processEvents();

        // ---- Y1 账号入口：主页右上那颗胶囊（11-12 用户反馈，取代导航轨底部那一格）----
        // 圆头像 + 名字 + 一行小状态，极淡的底、不描边，点开账号弹层。
        QPushButton *entry = ui.getAccountSkinButton();
        SkinViewer *homeViewer = ui.getSkinViewer();
        QLabel *hint = ui.getSkinHintLabel();
        check(entry != nullptr && homeViewer != nullptr && hint != nullptr,
              "账号入口、3D 预览、说明行三块控件都在");
        if (entry != nullptr && homeViewer != nullptr && hint != nullptr) {
            check(entry->objectName() == MainWindowUI::accountSkinButtonName(),
                  "objectName 对得上（验证工程和 mw_check 都按它认控件）");
            QWidget *profileCard = window.findChild<QWidget *>(QStringLiteral("homeProfileCard"));
            check(profileCard != nullptr && isDescendantOf(entry, profileCard) && entry->isVisibleTo(&window),
                  "账号胶囊在主页右栏（立绘那一栏）里、可见");
            check(bar->findChild<QPushButton *>(MainWindowUI::accountSkinButtonName()) == nullptr &&
                      bar->findChild<QLabel *>(QStringLiteral("homeAvatarLabel")) == nullptr &&
                      bar->findChild<QLabel *>(QStringLiteral("homeAccountLabel")) == nullptr &&
                      ui.titleBar()->findChild<QPushButton *>(MainWindowUI::accountSkinButtonName()) == nullptr,
                  "导航轨底部没有账号格（头像 / 名字都不在轨上）");
            check(!entry->accessibleName().isEmpty(), "无障碍名不空", entry->accessibleName());
            check(entry->height() == 44, "胶囊高 44（全局 QSS 的 min-height 没把它压扁）",
                  QString::number(entry->height()));
            QLabel *pillName = ui.getHomeAccountLabel();
            QLabel *pillType = ui.getHomeAccountTypeLabel();
            QLabel *pillFace = ui.getHomeAvatarLabel();
            check(pillName != nullptr && pillType != nullptr && pillFace != nullptr &&
                      pillName->parentWidget() == entry && pillType->parentWidget() == entry &&
                      pillFace->parentWidget() == entry,
                  "圆头像、名字、小状态都装在胶囊里");
            if (pillName != nullptr && pillType != nullptr && pillFace != nullptr) {
                check(pillName->text() == MainWindowUI::homeAccountSignInText() && !pillType->isVisibleTo(&window),
                      "没登录（还没人灌数据）：胶囊写「登录」、小状态收起", pillName->text());
                check(pillName->font().pixelSize() == Theme::textSize(Theme::TextRole::Body) &&
                          pillType->font().pixelSize() == Theme::textSize(Theme::TextRole::Caption),
                      "名字正文 14、小状态说明 13（26/16/14/13 四档里的两档）",
                      QStringLiteral("%1 / %2").arg(pillName->font().pixelSize()).arg(pillType->font().pixelSize()));
                check(pillFace->width() == 32 && pillFace->height() == 32, "头像 32 见方（画成圆）");
            }
            check(window.findChild<QWidget *>(QStringLiteral("homeHeroSubtitle")) == nullptr,
                  "主页没有英文副题「Your next world awaits.」");
            // 两档窗口：胶囊整颗在窗里、不压拖动条（窗口按钮在那条上）、贴右上、不挤大标题。
            {
                const QSize before = window.size();
                QWidget *caption = ui.titleBar()->captionStrip();
                QWidget *title = window.findChild<QWidget *>(QStringLiteral("homeHeroTitle"));
                for (const QSize size : {QSize(1280, 800), QSize(900, 620)}) {
                    window.resize(size);
                    QApplication::processEvents();
                    QApplication::processEvents();
                    const QRect r(entry->mapTo(&window, QPoint(0, 0)), entry->size());
                    const QRect cap(caption->mapTo(&window, QPoint(0, 0)), caption->size());
                    const QString where = QStringLiteral("%1x%2 胶囊 %3,%4 %5x%6 / 拖动条底 %7")
                                              .arg(size.width()).arg(size.height())
                                              .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                                              .arg(cap.bottom());
                    check(window.rect().contains(r), "胶囊整颗在窗口里（没被裁）", where);
                    check(!r.intersects(cap), "胶囊不压窗口按钮那条拖动条", where);
                    check(r.right() >= window.width() - 48 && r.top() <= cap.bottom() + 24,
                          "胶囊贴着右上角", where);
                    check(title == nullptr || !r.intersects(QRect(title->mapTo(&window, QPoint(0, 0)), title->size())),
                          "胶囊不挤大标题", where);
                }
                window.resize(before);
                QApplication::processEvents();
            }
            // 底色浓度：名字（heroInk）压在「胶囊底 over 花背景」上要有 4.5:1。
            // 暗色主题按灰 160 的亮块、浅色主题按灰 96 的暗块量（中间调的风景是最难压的那种）。
            {
                const auto lum = [](const QColor &c) {
                    const auto ch = [](double v) {
                        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
                    };
                    return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
                };
                for (const QString &id : Theme::presetIds()) {
                    const Theme::Tokens t = Theme::tokens(id);
                    const bool dark = t.heroVeil.lightness() < 128;
                    const double a = MainWindowUI::homeAccountPillVeilAlpha(dark);
                    const double bg = (dark ? 160 : 96) / 255.0;
                    const QColor under = QColor::fromRgbF(float(a * t.heroVeil.redF() + (1 - a) * bg),
                                                          float(a * t.heroVeil.greenF() + (1 - a) * bg),
                                                          float(a * t.heroVeil.blueF() + (1 - a) * bg));
                    const double l1 = lum(t.heroInk), l2 = lum(under);
                    const double ratio = (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
                    check(ratio >= 4.5 && a <= 0.40, "胶囊底极淡（≤ 0.40）但名字在花背景上仍有 4.5:1",
                          QStringLiteral("%1 α=%2 对比 %3").arg(id).arg(a).arg(ratio, 0, 'f', 2));
                }
            }
            check(!hint->isVisibleTo(&window),
                  "主页不再显示那句重复解释外观状态的文本");
            check(homeViewer->isVisibleTo(&window), "角色预览仍然完整显示");
            check(ui.getAccountButton() != nullptr &&
                      !ui.getAccountButton()->isVisibleTo(&window),
                  "老那一格账号已经收起来了");
            entry->click();
            QApplication::processEvents();
            QWidget *pop = window.findChild<QWidget *>(MainWindowUI::accountPopoverName());
            check(pop != nullptr && pop->isVisible(), "点账号入口 ⇒ 开账号弹层");
            entry->click();
            QApplication::processEvents();
            pop = window.findChild<QWidget *>(MainWindowUI::accountPopoverName());
            check(pop == nullptr || !pop->isVisible(), "再点一下 ⇒ 收起（toggle）");
        }

        // ---- Y2 底栏名字下面那一行 ----
        QLabel *accountName = ui.getAccountLabel();
        QLabel *accountType = ui.getAccountTypeLabel();
        check(accountName != nullptr && accountType != nullptr,
              "底栏账户那格的两行文字都在");
        if (accountName != nullptr && accountType != nullptr) {
            check(isDescendantOf(accountType, bar),
                  "第二行在底栏里（跟着底栏一起显示/隐藏）");
            check(accountType->objectName() == MainWindowUI::accountTypeLabelName(),
                  "objectName 对得上");
            check(accountType->text() == MainWindowUI::accountTypeNotLoggedInText(),
                  "界面刚搭完、还没人来刷的时候写的是「未登录」"
                  "（**不是**离线登录 —— 那种状态点开始会被「请先登录」挡回来）",
                  accountType->text());
            // 三种说法本身钉一遍：**三种而不是两种**是这次特意定的（见 .hpp 那段 NOTE）。
            check(MainWindowUI::accountTypeOnlineText() == QStringLiteral("正版") &&
                      MainWindowUI::accountTypeOfflineText() == QStringLiteral("离线") &&
                      MainWindowUI::accountTypeNotLoggedInText() == QStringLiteral("未登录"),
                  "三种说法分别是 正版 / 离线 / 未登录（说明删减清单 §4）");
            check(accountType->mapTo(bar, QPoint(0, 0)).y() >
                      accountName->mapTo(bar, QPoint(0, 0)).y(),
                  "它在名字**下面**");
            // 做法照抄左上角那一格：小一号 + PlaceholderText 颜色。
            check(accountType->font().pixelSize() == Theme::textSize(Theme::TextRole::Caption) &&
                      accountName->font().pixelSize() == Theme::textSize(Theme::TextRole::Body),
                  "字号比名字小一档（它是注释，不是内容）",
                  QString("%1 vs %2")
                      .arg(accountType->font().pixelSize())
                      .arg(accountName->font().pixelSize()));
            check(accountType->palette().color(QPalette::WindowText) ==
                      accountType->palette().color(QPalette::PlaceholderText),
                  "颜色是 PlaceholderText（和登录页那几句、说明行一个口径）",
                  QStringLiteral("字 %1 / 占位 %2")
                      .arg(accountType->palette().color(QPalette::WindowText).name(QColor::HexArgb),
                           accountType->palette()
                               .color(QPalette::PlaceholderText)
                               .name(QColor::HexArgb)));
        }
    }

    std::printf("\n===== Z. 实例页「导入整合包…」的位置（900x620 下实测） =====\n");
    {
        // 和 C 节同一个道理：这一页不是当前页时几何停在旧值，先切过去再量。
        tabs->setCurrentIndex(1);
        QApplication::processEvents();

        QWidget *ipage = tabs->widget(1);
        QPushButton *importBtn =
            ipage->findChild<QPushButton *>(MainWindowUI::importModpackButtonName());
        QTreeWidget *ilist = ipage->findChild<QTreeWidget *>(MainWindowUI::instanceListName());
        // 实例页那两个 `QGroupBox` 拆掉了（用户那份改版文档第一条「减少框」：
        // "右侧只有一个主要内容表面"）。这两颗按钮现在长在**左栏那个侧栏面**上
        // （`wtfmlSurface="sidebar"`），所以"那个组"换成"那张面"。
        // UI 7b（设计稿 manage-*）：它在页头右上角、实例列表的上方偏右。
        check(importBtn != nullptr && ilist != nullptr, "量之前先确认两个控件都在（导入 / 实例列表）");
        if (importBtn != nullptr && ilist != nullptr) {
            const auto boxIn = [ipage](QWidget *w) {
                return QRect(w->mapTo(ipage, QPoint(0, 0)), w->size());
            };
            const QRect importBox = boxIn(importBtn);
            const QRect listBox = boxIn(ilist);
            check(importBtn->isVisibleTo(ipage) && importBox.bottom() < listBox.top() &&
                      importBox.center().x() > listBox.center().x(),
                  "「导入整合包…」整颗在实例列表上方、偏右（页头右上角）",
                  QString("import=%1,%2 list.top=%3 list.cx=%4")
                      .arg(importBox.center().x()).arg(importBox.bottom())
                      .arg(listBox.top()).arg(listBox.center().x()));
        }

        // 量完把状态收回去：Y 节和"打开就停在主页"的约定都依赖停在主页。
        tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
        QApplication::processEvents();
    }

    std::printf("\n===== Z2. 设置页「启动选项」那一组 =====\n");
    if (const int result = UiWindowPreferencesChecks(tabs); result != 0) return result;

    std::printf("\n===== Z3. 「游戏日志」窗（ui/GameLogWindow） =====\n");
    if (const int result = UiGameLogActionsChecks(bar); result != 0) return result;

    std::printf("\n===== Z3b. 新窗摆哪儿：不许遮住启动器（喂假矩形，纯几何） =====\n");
    if (const int result = UiGameLogPlacementChecks(); result != 0) return result;

    std::printf("\n===== Z4. 「这一行是哪一档」：classifyLine 的真值表（纯函数） =====\n");
    if (const int result = UiLogClassificationChecks(); result != 0) return result;

    std::printf("\n===== Z5. 级别筛选 + 计数（模型那一层） =====\n");
    if (const int result = UiLogFilteringChecks(); result != 0) return result;

    std::printf("\n===== Z6. 一行一个底色（量像素） =====\n");
    if (const int result = UiLogColorsChecks(); result != 0) return result;

    std::printf("\n===== Z7. 顶上那排开关：颜色跟着那一档走（量像素） =====\n");
    if (const int result = UiLogTogglesChecks(); result != 0) return result;

    std::printf("\n===== Z6b. 短行也铺满整行 + 那半行跟着上一行（量像素） =====\n");
    {
        // 这一节的夹具**故意全是短行** —— 它量的正是"短行也要铺满整行"。
        //
        // 改之前底色挂在**逐字**的字符格式上，只染"字占的那一截"：一行比视口短时后半截
        // 是白的。用户看到的「最后一行不会被渲染颜色」一半出在这里（末尾那几行、以及我们
        // 自己补的「游戏已退出，退出码：-1」都是短的），另一半出在"末尾那半行不染"。
        // 现在两件都改了（块格式铺满整行 + 半行按上一行那一档先猜一个），这一节钉的就是它们。
        auto *log = new GameLogWindow(QStringLiteral("短行这一节"));
        QPointer<GameLogWindow> guard(log);
        log->show();
        QApplication::processEvents();
        QPlainTextEdit *view = log->getLogView();
        using L = GameLogWindow::LogLevel;
        if (view != nullptr) {
            log->appendText(QStringLiteral("[12:34:56] [main/INFO]: 短的一行\n"));
            log->appendText(QStringLiteral("[12:34:56] [main/ERROR]: 短的一行\n"));
            // 第三条没有换行 = 末尾那半行（它的级别还不知道，按上一行那一档猜）。
            log->appendText(QStringLiteral("[12:34:56] [main/ERROR]: 还没换行的那半行"));
            QApplication::processEvents();

            const QImage shot = captureWidget(view->viewport());
            const int viewportWidth = view->viewport()->width();
            const int viewportHeight = view->viewport()->height();
            QRect rows[3];
            QRect textRects[3];
            for (int i = 0; i < 3; ++i) {
                const QTextCursor cursor(view->document()->findBlockByNumber(i));
                textRects[i] = view->cursorRect(cursor);
                rows[i] = QRect(0, textRects[i].top(), viewportWidth, textRects[i].height());
            }
            // 正文底下那块空白（"一行都没有"的那片）：它是**量出来的**窗口底色，后面的
            // 比较都对着它 —— 不拿 `palette()` 那种"代码里写的"值当参照。
            const QRect blank(0, rows[2].bottom() + 4, viewportWidth, 40);
            bool premises = rows[0].height() > 0 && viewportWidth > 200 &&
                            blank.bottom() < viewportHeight;
            for (int i = 0; i < 3; ++i) {
                premises = premises && rows[i].height() == rows[0].height();
                if (i > 0) {
                    premises = premises && rows[i].top() > rows[i - 1].top();
                }
            }
            // **前两行的字要比视口窄得多** —— "短行"这件事要成立，不然这一节量的是长行、
            // 根本没在量它该量的东西（这条也顺带把"视口够宽"钉住）。
            premises = premises && textRects[0].width() < viewportWidth / 2 &&
                       textRects[1].width() < viewportWidth / 2 &&
                       textRects[2].width() < viewportWidth / 2;
            check(premises,
                  "Z6b 前提：三格都排过布局、自上而下，前两行的字**比视口窄**，底下还留着一块空白",
                  QStringLiteral("视口 %1x%2 文字宽 %3/%4/%5 空白 y=%6")
                      .arg(viewportWidth)
                      .arg(viewportHeight)
                      .arg(textRects[0].width())
                      .arg(textRects[1].width())
                      .arg(textRects[2].width())
                      .arg(blank.y()));

            const QColor base = dominantColor(shot, blank);
            // 行**最右边**那 60 px：短行量那儿，才分得出"铺满整行"和"只染字那一截"。
            const QColor infoEnd = dominantColor(shot, QRect(viewportWidth - 60, rows[0].top(), 60, rows[0].height()));
            const QColor errorEnd = dominantColor(shot, QRect(viewportWidth - 60, rows[1].top(), 60, rows[1].height()));
            const QColor pendingEnd = dominantColor(shot, QRect(viewportWidth - 60, rows[2].top(), 60, rows[2].height()));
            const QString ends = QStringLiteral("底=%1 INFO=%2 ERROR=%3 半行=%4")
                                     .arg(base.name(), infoEnd.name(), errorEnd.name(), pendingEnd.name());

            // ① 短行最右边那 60 px 是这一档的底色（字早就不在那儿了）——**这一条就是
            // "最后一行没有颜色"那条抱怨的解药**：改之前这里量出来是窗口底色。
            check(colorDistance(errorEnd, GameLogWindow::levelBackground(L::Error)) <= 6,
                  "Z6b 短行的底色也**铺满整行**（量的是行最右边那一格，那里没有字）", ends);
            // ② 末尾那半行跟着上一行那一档，而且同样铺满整行。
            check(colorDistance(pendingEnd, GameLogWindow::levelBackground(L::Error)) <= 6,
                  "Z6b 末尾那半行按上一行（ERROR）染，也铺满整行", ends);
            // ③ 反过来的那个点：INFO 那一行整行都**不染**（它右边那一格 == 底下那块空白）。
            // 少了这一条的话，"所有行一律刷成深红"也能把上面两条混过去。
            check(colorDistance(infoEnd, base) <= 6,
                  "Z6b INFO 那一行整行都不染（它最右边那格 == 正文底下那块空白的颜色）", ends);
            // ④ 相邻两行的右边**不是同一个色**（差得开）：这才说明铺的是"一行一色"，
            // 不是把整块视口刷了一遍。
            check(colorDistance(infoEnd, errorEnd) >= 20,
                  "Z6b 上下两行的右边差得开（染的是这一行，不是整块视口）", ends);

            log->close();
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            check(guard.isNull(), "Z6b 收尾：短行这一节那个窗关掉就销毁（WA_DeleteOnClose）", QString());
        }
    }

    std::printf("\n===== Z6c. 行与行之间那条细线（量像素） =====\n");
    if (const int result = UiLogSeparatorsChecks(); result != 0) return result;

    std::printf("\n===== Z9. 游戏退出之后这个窗怎么办（纯函数） =====\n");
    {
        // 要求：「如果游戏正常以0退出就自动关闭日志界面」。判据抽成了一个纯函数，
        // 所以这里直接喂真值表 —— 关键是**四个格的分工**：
        // 正常 + 0 → 关（那一趟没有任何可看的东西）
        // 正常 + 非 0 → 不关（那正是用户要逐行看的时候）
        // 崩溃 + 任何码 → 不关
        // 崩溃 + 码是 0 → **也不关**（Windows 上杀进程拿到的码就是 0；只看码会误判）
        check(GameLogWindow::autoCloseOnExit(0, false),
              "Z9 正常退出、码是 0 → 自动关掉日志窗");
        check(!GameLogWindow::autoCloseOnExit(1, false),
              "Z9 正常退出但码不是 0 → 留着（那正是要看的时候）");
        check(!GameLogWindow::autoCloseOnExit(-1, false), "Z9 码是 -1 → 留着");
        check(!GameLogWindow::autoCloseOnExit(0, true),
              "Z9 崩溃退出、码正好是 0 → **不关**（只看码会把它误判成「正常退出」）");
        check(!GameLogWindow::autoCloseOnExit(1, true) &&
                  !GameLogWindow::autoCloseOnExit(-1, true),
              "Z9 崩溃退出 → 一律不关（崩栈那几行正是用户要看的）");
        // 上面五条是逐格念的，这一条把"**只有**一种情况会关"钉住：八种组合里只有一个真。
        // 少了它，把函数写成 `return exitCode >= 0;` 那种也躲得过逐格念的那几条。
        int closing = 0;
        for (const int code : {0, 1, -1, 2}) {
            for (const bool crashed : {false, true}) {
                if (GameLogWindow::autoCloseOnExit(code, crashed)) {
                    ++closing;
                }
            }
        }
        check(closing == 1,
              "Z9 八种组合里**只有**「正常 + 0」那一种会关窗（不是「码够小就关」之类）",
              QString::number(closing));
    }

    std::printf("\n===== Z8. 切一次筛选花了多少次文档操作（= 卡不卡） =====\n");
    {
        // 判据不是墙上的钟（那跟机器有关、会飘），而是**文档改动了几次** —— 那就是「卡不卡」
        // 的机制本身：文档操作按**次数**收钱。改之前是「一行摆两次」（一次写字、一次起一格），
        // 两万行点一下就 559 ms；现在连续同一档的行攒成一段一次写完，操作次数从「行数」降到
        // 「档数变化的次数」。所以这里数 `contentsChange`（格式改动也会发一次，见 Qt 的
        // QTextDocumentPrivate::documentChange —— 正好把「盖底色」那几次也算进来）。
        //
        // 夹具的形状是**故意像真日志的**：一档连着两百来行再换档（原版/模组的日志就是这样，
        // INFO 连着几百行中间夹几条 WARN）。夹具里让档数变化次数在两种行数下**一模一样**，
        // 于是「行数涨十倍、改动次数不变」这句话可以直接拿来当断言。
        const auto toggleChanges = [](int lines, qint64 *ms) {
            auto *log = new GameLogWindow(QStringLiteral("改几次"));
            log->show();
            QApplication::processEvents();
            QString chunk;
            const int runLength = qMax(1, lines / 10);  // 十段：段数固定，行数变
            for (int i = 0; i < lines; ++i) {
                const int which = (i / runLength) % 3;
                const char *level = which == 0 ? "INFO" : (which == 1 ? "WARN" : "ERROR");
                chunk += QStringLiteral("[12:34:56] [main/%1]: 第 %2 条\n").arg(level).arg(i);
            }
            log->appendText(chunk);
            QApplication::processEvents();
            int changes = 0;
            const QMetaObject::Connection conn = QObject::connect(
                log->getLogView()->document(), &QTextDocument::contentsChange,
                [&changes](int, int, int) { ++changes; });
            QElapsedTimer timer;
            timer.start();
            log->setLevelVisible(GameLogWindow::LogLevel::Warn, false);
            *ms = timer.elapsed();
            QObject::disconnect(conn);
            QApplication::processEvents();
            log->close();
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return changes;
        };
        qint64 msSmall = 0;
        qint64 msBig = 0;
        const int changesSmall = toggleChanges(2000, &msSmall);
        const int changesBig = toggleChanges(20000, &msBig);
        const QString detail = QStringLiteral("2000 行：%1 次 / %2 ms；20000 行：%3 次 / %4 ms")
                                   .arg(changesSmall)
                                   .arg(static_cast<long long>(msSmall))
                                   .arg(changesBig)
                                   .arg(static_cast<long long>(msBig));
        // ① 夹具得是「真改过东西」的（关掉 WARN 那一档确实动了一半的段）—— 次数至少要有几个。
        check(changesSmall > 0 && changesBig > 0,
              "Z8 前提：切这一下确实动了文档（不是「什么都没干所以很快」）", detail);
        check(changesSmall == changesBig,
              "Z8 行数涨十倍，文档改动次数**不变**（代价跟「档数变化了几次」走，不跟行数走）",
              detail);
        // ③ 次数和行数不是一个量级（差着一个数量级以上）—— 上一条可能被「两次都很小」蒙过去，
        // 这一条盯的是「确实远小于行数」。十段夹具下量到的是三十几次。
        check(changesBig * 20 < 20000,
              "Z8 那几次改动远少于行数（两万行是三十几次，不是两万次）", detail);
        check(msBig < 250, "Z8 两万行点一下不超过 250 ms", detail);
    }

    std::printf("\n===== Z10. 行数上限那颗下拉框 + 清空/关闭摆哪儿 =====\n");
    if (const int result = UiLogRetentionChecks(); result != 0) return result;

    std::printf("===== V. 游戏文件夹那一列：两行式的行 + 行尾齿轮那五项 =====\n");
    if (const int result = UiFolderDisplayChecks(ui, folderList, tabs); result != 0) return result;

    // =====================================================================
    // O. 内容区覆盖层
    // =====================================================================
    // 用户给的方向：最终**不出现独立窗口**（日志窗除外）—— 设置界面在内容区整块盖掉
    // （左上角一颗返回箭头），小提示在中间开一块、四周变暗，而且不许出现尺寸不一样的弹窗。
    // 这一节钉的就是"盖在哪儿、盖住谁、长什么样"。
    //
    // NOTE: 覆盖层和内容页是**兄弟**（它不吃布局，几何靠盯住 tabWidget 现算），所以
    // "盖住的正好是内容区"这句得在**窗口坐标系**里比 —— 两边坐标原点都不一样。
    std::printf("\n===== O. 内容区覆盖层（不再开独立窗口）=====\n");
    {
        OverlayHost *overlay = ui.overlay();
        check(overlay != nullptr && overlay->objectName() == OverlayHost::overlayHostName(),
              "覆盖层建出来了（按 objectName 找得回来）",
              overlay != nullptr ? overlay->objectName() : QStringLiteral("<null>"));
        check(overlay != nullptr && !overlay->isOpen(), "刚搭好的时候它是关着的");
    }
    if (const int result = UiOverlayGeometryChecks(window, ui, tabs, bar); result != 0) return result;

    // =====================================================================
    // O2. 切选项卡 = 这一页"收着"（顶上那四颗导航按钮，）
    // =====================================================================
    // 要求：「顶端四个按钮点击以后自动到达对应选项卡的首页（比如说我现在正点开 sodium
    // 的下载页面，现在我点击启动选项卡，直接切到那个页面，下载这个"弹窗"自动收起）」+
    // 「保留原来那个子页的状态，比如说我正在看 sodium 的下载，现在点击 启动，切到启动页，
    // 再点击 下载，那就回到下载 sodium 界面，在下载页点击 下载（顶上那个，不是下载确认键），
    // 那就回到下载页的首页（四个都适用）（包括向下滑动了多少也保留）」。
    //
    // 这一节在**一套最小环境**里量这台状态机（一个真的 QTabWidget 当锚点 + 两页 + 一个
    // 覆盖层）：翻页那两颗导航按钮长什么样、点了之后产品里走哪一条路，那是 MainWindowUI 的
    // 事（mw_check 的 ZJ10 走真按钮），这里只管"锚点换了一页，这一层该怎么办"。
    //
    // "收着"和"关掉"是两件事，这一节钉的就是这个区别：收着 = `pageOpen()` 还是真、页对象
    // 和它的内容一个不少（收掉了"切回来还是原来那一页"就没了），只是不露面。判据走
    // `parked()` + `isVisible()`。
    std::printf("\n===== O2. 切选项卡 = 这一页收着（不是关掉） =====\n");
    if (const int result = UiOverlayNavigationChecks(); result != 0) return result;

    // =====================================================================
    // Z12. 「可以在设置中关闭此提示」那行小字 + 首字母头像
    // =====================================================================
    // 要求里那两条确认各自带着一句「可以在设置中关闭此提示」——那行字比正文小一号、
    // 浅一档，**在正文下面、按钮行上面**。它是个新控件（`InlineMessageBox::setNote()`），
    // 这一节钉它的形状；"哪一句提示写了哪一行字、指的是哪一个开关"是产品那一侧的事
    // （mw_check 的 AJ 节按真会话量）。
    //
    // 后半截是底栏账号面板行首那块头像的兜底（`SkinModel::letterAvatar`）：那个面板里
    // 三行的人有一半是本地一张图都没有的（列表里那条只是个凭据副本），兜底图长什么样
    // 得有个说法 —— 一个按名字算色的方块 + 首字母。
    std::printf("\n===== Z12. 「可以在设置中关闭此提示」那行小字 + 首字母头像 =====\n");
    if (const int result = UiLaunchPromptsChecks(ui); result != 0) return result;

    std::printf("\n===== Z11. 顶上那一条自己画的标题栏（外框那一份的「脸」） =====\n");
    if (const int result = UiTitleBarChecks(window, ui); result != 0) return result;

    std::printf("\n===== Z13. 实例行行尾那三颗按钮 =====\n");
    if (const int result = UiInstanceRowActionsChecks(); result != 0) return result;

    std::printf("\n===== Z14. 资源详情页（下载页点开一个资源之后那一整页） =====\n");
    if (const int result = UiInstanceRowPaintingChecks(); result != 0) return result;

    std::printf("\n===== Z15. 「启用 / 禁用」那枚开关 + MOTD 照游戏的样子 + 行首那枚资源图标"
                "=====\n");
    if (const int result = UiContentRowsChecks(); result != 0) return result;

    std::printf("\n===== Z16. 「世界设置」那一页（点存档行尾那支笔之后） =====\n");
    if (const int result = UiWorldEditorChecks(); result != 0) return result;

    // =====================================================================
    // W. 主题那一层：背景 / 文字不透明度 / 每一块骨架能不能整套也能单独换
    // =====================================================================
    //
    // 要求是「各个地方的背景等允许切换，允许各个框架结构进行一个主题的切换（比如文本
    // 透明度等），先不急着做入口，把 ui 框架改成能高度个性化定义的状态」。这一层没有入口，
    // 界面上**看不出**它做没做对 —— 于是只能在这儿量。它要同时成立两件互相顶的事：
    //
    // ① 出厂那一份 = 今天的样子，**一个字节都不许动**（不然"加了一层框架"就变成了
    // "顺手改了配色"，而且跟着系统深浅走这件事会被钉死）；
    // ② 改过的那些项**真的**落到界面上（角色覆盖落到应用调色板、骨架覆盖落到每一处）。
    //
    // NOTE: 这一段会**动全局那一份主题**（`qApp->palette()` 也跟着动），所以摆在最后 ——
    // 前面每一条断言量的都是"出厂那一份"下的几何与像素。收尾处会铺回出厂那一份。
    std::printf("===== W. 主题那一层（背景 / 文字不透明度 / 每一块骨架） =====\n");
    if (const int result = UiThemePaletteChecks(app, tabs, bar); result != 0) return result;
    if (const int result = UiUpgradePageChecks(app); result != 0) return result;
    if (const int result = UiUpgradeReportChecks(app); result != 0) return result;
    if (const int result = UiUpgradeHistoryChecks(app); result != 0) return result;
    if (const int result = UiTopLevelDialogChecks(app); result != 0) return result;

    if (const int result = UiAnimationsChecks(app, bar); result != 0) return result;

    if (const int result = UiScrollBarsChecks(app, bar); result != 0) return result;

    std::printf("\n===== U28. 完整性检查报告页（「检查并修复」跑完之后那整页）=====\n");
    if (const int result = UiIntegrityReportChecks(); result != 0) return result;

    std::printf("\n===== U29. 「游戏异常退出」那一项在详情页上多露什么 =====\n");
    if (const int result = UiCrashReportChecks(); result != 0) return result;

    std::printf("\n===== U30. 世界与实例备份那几颗按钮 =====\n");
    if (const int result = UiBackupActionsChecks(); result != 0) return result;

    if (const int result = UiInstanceSnapshotsChecks(); result != 0) return result;

#if 0
    // 退役：旧的单张「上次游玩」卡。新主页由下面 U31b 的收藏/最近两排覆盖。
    // ---------- U31 主页那张「上次游玩」卡 ----------
    //
    // 这一节量的是**屏幕上那一张卡长什么样**：它在主页左栏落哪儿、三行字怎么摆、那一行
    // 灰字空着的时候卡片是不是**真的矮一截**（不摆空壳）、两颗按钮哪一颗是主题色那一档。
    // 判据层（账怎么写 / 时间怎么说 / 目标没了怎么降级）在 verify 的 ZR 段；**接线**
    // （点下去走哪条路）在 mw_check 的 T36 —— 这里只管"看得见的那一份"，一件事只验一遍。
    //
    // 全程走 `MainWindowUI::setHomeRecent()` 这一个入口：那一份 `HomeRecent` 就是产品
    // （`MainWindow::updateHomeRecent()`）给界面的唯一一份值，手工 setText 摆出来的卡片
    // 不是产品会画出来的那张 —— 那正是这一节要防的"看着对、产品却不是这么画的"。
    // （：这一节的兄弟 T36 抓到的就是这一类形状 —— 卡片压根没被存成成员，
    // 界面上一片空白，而两颗按钮照旧写得进字。）
    {
        window.show();
        window.resize(900, 760);
        if (tabs != nullptr) {
            tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
        }
        QApplication::processEvents();
        QFrame *recentCard = ui.getHomeRecentCard();
        QLabel *recentName = ui.getHomeRecentNameLabel();
        QLabel *recentDetail = ui.getHomeRecentDetailLabel();
        QLabel *recentWhen = ui.getHomeRecentWhenLabel();
        QPushButton *recentGo = ui.getHomeRecentContinueButton();
        QPushButton *recentSettings = ui.getHomeRecentSettingsButton();
        check(recentCard != nullptr && recentName != nullptr && recentDetail != nullptr &&
                  recentWhen != nullptr && recentGo != nullptr && recentSettings != nullptr,
              "U31.0 那张卡的卡片本体 / 三行字 / 两颗按钮都找得到（按 objectName）");
        QWidget *launchCard = startBtn != nullptr ? startBtn->parentWidget() : nullptr;
        check(recentCard != nullptr && launchCard != nullptr &&
                  recentCard->parentWidget() == launchCard->parentWidget(),
              "U31.0 它就是同一栏（主页左栏）里的一张卡，不是另开在别处的");
        if (recentCard != nullptr && launchCard != nullptr &&
            recentCard->parentWidget() == launchCard->parentWidget()) {
            // ① 一次都没启动过：**整张卡收起来**（不摆一张空的等数据）。
            check(!recentCard->isVisibleTo(recentCard->parentWidget()),
                  "★★ U31.1 一次都没启动过 ⇒ 整张卡收起来（这一节开头 nobody 调过 "
                  "setHomeRecent）");
            check(recentCard->isVisibleTo(recentCard->parentWidget()) == false &&
                      recentGo->isEnabled(),
                  "U31.1 收起来的只是那张卡，它那两颗按钮没被顺手禁用（下一次摆出来照旧能点）");

            // ② 摆一条"接着玩得起来"的：三行字各就各位。
            MainWindowUI::HomeRecent recent;
            recent.visible = true;
            recent.instanceLine = QStringLiteral("基地周目-1.20.1-forge-47.2.0");
            recent.detailLine = QStringLiteral("单人世界：基地周目 · 玩了 1 小时 12 分（还在玩）");
            recent.whenLine = QStringLiteral("昨天 22:14");
            recent.canContinue = true;
            ui.setHomeRecent(recent);
            QApplication::processEvents();
            check(recentCard->isVisibleTo(recentCard->parentWidget()),
                  "★★ U31.2 有记录了 ⇒ 那张卡露出来");
            check(recentName->text() == recent.instanceLine &&
                      recentDetail->text() == recent.detailLine &&
                      recentWhen->text() == recent.whenLine,
                  "★★ U31.2 三行字就是喂进去的那三句（一行一个字都不许另起炉灶）",
                  QStringLiteral("%1｜%2｜%3")
                      .arg(recentName->text(), recentDetail->text(), recentWhen->text()));
            check(recentGo->text() == MainWindowUI::homeRecentContinueText(),
                  "U31.2 接着玩得起来 ⇒ 那颗主动作写「继续游玩」", recentGo->text());
            check(recentSettings->text() == MainWindowUI::homeRecentSettingsText(),
                  "U31.2 旁边那颗永远写「实例设置」", recentSettings->text());

            // ③ 摆在哪儿：**启动卡下面**，和它一样宽（同一栏里两张卡，7:3 那份宽度是共用
            // 的）；名字在标题下面、详情在名字下面 —— 那一份次序是那张草图的。
            check(recentCard->y() >= launchCard->y() + launchCard->height(),
                  "★★ U31.3 那张卡在**启动卡下面**（主页左栏是「启动 + 最近活动」两"
                  "件事，次序就是它们的重要程度）",
                  QStringLiteral("启动卡底 %1 / 这张卡顶 %2")
                      .arg(launchCard->y() + launchCard->height())
                      .arg(recentCard->y()));
            check(recentCard->width() == launchCard->width(),
                  "★ U31.3 两张卡同宽（同一栏里由同一个布局铺满，不是各自算一份）",
                  QStringLiteral("%1 vs %2").arg(recentCard->width()).arg(launchCard->width()));
            check(recentName->y() > recentWhen->y() && recentDetail->y() > recentName->y() &&
                      recentGo->y() > recentDetail->y(),
                  "★ U31.3 摆的次序是「时间和标题那一行 → 实例名 → 灰字那行 → 两颗按钮」",
                  QStringLiteral("when %1 / name %2 / detail %3 / buttons %4")
                      .arg(recentWhen->y())
                      .arg(recentName->y())
                      .arg(recentDetail->y())
                      .arg(recentGo->y()));
            check(recentWhen->x() > recentCard->width() / 2,
                  "★★ U31.3 那句「昨天 22:14」在**右上角**（那张草图里它不是单独一行，"
                  "是这一行的右端）",
                  QStringLiteral("x %1 / 卡片宽 %2").arg(recentWhen->x()).arg(recentCard->width()));

            // ④ 灰字那一行空着时**整行收起来**，卡片跟着矮一截：空 QLabel 照样占一行字高，
            // 留着就是白留一块（那条"不摆空壳"落到了行这一级）。
            const int withDetail = recentCard->sizeHint().height();
            // 还原那一发要用**同一句**：灰字那行是会折行的（这一句在卡片宽度下正好两行），
            // 换一句短的回来高度当然对不上 —— 那不是"收起来不可逆"。
            const QString keepDetail = recent.detailLine;
            recent.detailLine.clear();
            ui.setHomeRecent(recent);
            QApplication::processEvents();
            const int withoutDetail = recentCard->sizeHint().height();
            check(!recentDetail->isVisibleTo(recentCard) && withoutDetail < withDetail,
                  "★★ U31.4 灰字那行空着 ⇒ 那一行**收起来**、卡片报出来的高度真的矮一截"
                  "（不是留一块空白）",
                  QStringLiteral("sizeHint %1px → %2px").arg(withDetail).arg(withoutDetail));
            recent.detailLine = keepDetail;
            ui.setHomeRecent(recent);
            QApplication::processEvents();
            check(recentDetail->isVisibleTo(recentCard) &&
                      recentCard->sizeHint().height() == withDetail,
                  "U31.4 那一行再有字 ⇒ 高度回到原来那一档（收起来是可逆的）",
                  QStringLiteral("sizeHint %1px").arg(recentCard->sizeHint().height()));
            // 时间那一位同理（账上一个时间都没有的老记录）。
            recent.whenLine.clear();
            ui.setHomeRecent(recent);
            QApplication::processEvents();
            check(!recentWhen->isVisibleTo(recentCard),
                  "U31.4 时间那句空着也收起来（不写一句空话占着右上角）");
            recent.whenLine = QStringLiteral("昨天 22:14");

            // ⑤ 降级那一档：目标 / 那一份实例没了 ⇒ 字变成「启动实例」，但**按钮不消失**
            // （目标被删了不该让这张卡变成死路一条）。
            recent.canContinue = false;
            ui.setHomeRecent(recent);
            QApplication::processEvents();
            check(recentGo->text() == MainWindowUI::homeRecentLaunchText() &&
                      recentGo->isVisibleTo(recentCard),
                  "★★ U31.5 降级那一档那颗按钮**照旧在**、只是换了句话（不是藏起来）",
                  recentGo->text());
            recent.canContinue = true;
            ui.setHomeRecent(recent);
            QApplication::processEvents();

            // ⑥ 一句很长的实例名**不许把卡片顶宽**（名字那一格横向是 Ignored，见
            // setHomeRecent 那一带的 NOTE）：宽度只由 7:3 那份份数决定。
            recent.instanceLine =
                QStringLiteral("超长实例名超长实例名超长实例名超长实例名超长实例名"
                               "-1.20.1-neoforge-21.1.0-再长一点");
            ui.setHomeRecent(recent);
            QApplication::processEvents();
            check(recentCard->width() == launchCard->width() &&
                      recentName->width() <= recentCard->width(),
                  "★★ U31.6 一句很长的实例名**顶不宽**这张卡（名字那一格横向摘出了度量）",
                  QStringLiteral("卡片 %1 / 启动卡 %2 / 名字 %3")
                      .arg(recentCard->width())
                      .arg(launchCard->width())
                      .arg(recentName->width()));

            // ⑦ 两颗按钮的**分档**：主动作那一颗是页面上唯一一颗**实心**的（底 = 主题色本色、
            // 字 = 白），旁边那颗是白底黑字的「一般性」那一档。量的**渲染出来的像素**，不是
            // 样式表字符串 —— 字符串那一条会跟着同一个错误一起写错（见 `Theme.cpp` 里
            // `primaryBlock` / `kindBlock` 那两段：这是"分了三档"唯一说得清的地方）。
            {
                const QColor pure = Theme::accentShade(Theme::AccentShade::Pure);
                const QColor neutralInk = Theme::neutralInkColor();
                const auto countNear = [](const QImage &image, const QColor &want, int tol) {
                    int hits = 0;
                    for (int y = 0; y < image.height(); ++y) {
                        for (int x = 0; x < image.width(); ++x) {
                            if (colorDistance(image.pixelColor(x, y), want) <= tol) {
                                ++hits;
                            }
                        }
                    }
                    return hits;
                };
                const QImage goShot = captureWidget(recentGo);
                const QImage settingsShot = captureWidget(recentSettings);
                const QRect goAll = QRect(QPoint(0, 0), goShot.size());
                const QRect settingsAll = QRect(QPoint(0, 0), settingsShot.size());

                // 「继续游玩」那一颗：**整块的多数色就是主题色本色**（实心）。这是"主动作
                // 是页面上唯一一颗实心按钮"唯一量得出来的样子 —— 中性/危险那两档全是白底。
                const QColor goFill = dominantColor(goShot, goAll);
                check(colorDistance(goFill, pure) <= 6,
                      "★★ U31.7 「继续游玩」是**实心**那一档：整块多数色 = 主题色本色",
                      QStringLiteral("多数色 %1（本色 %2）").arg(goFill.name(), pure.name()));

                // 它上面那行字是**白**的：中间那一带里"离白比离本色更近"的像素得存在。
                // （不数纯 `#ffffff`：`render()` 出来是灰度抗锯齿，字芯未必一格不差地打到
                // 纯白；这条要钉的是"墨是白的"，不是"抗锯齿打了几格"。）
                const QRect goBand(4, goShot.height() / 4, qMax(0, goShot.width() - 8),
                                   qMax(1, goShot.height() / 2));
                int goLight = 0;
                for (int y = goBand.top(); y <= goBand.bottom(); ++y) {
                    for (int x = goBand.left(); x <= goBand.right(); ++x) {
                        const QColor c = goShot.pixelColor(x, y);
                        if (colorDistance(c, QColor(Qt::white)) < colorDistance(c, pure)) {
                            ++goLight;
                        }
                    }
                }
                // 实心那一颗从里到外**一个「一般性」的黑像素都不该有**（字白、边也是本色；
                // 白绿之间怎么混都到不了 #202020 那一片）。
                const int goNeutral = countNear(goShot, neutralInk, 30);
                check(goLight > 0 && goNeutral == 0,
                      "★★ U31.7 实心那一颗的字是**白**的（一个「一般性」的黑像素都没有 —— "
                      "分档分的就是这个：底和墨一起换档）",
                      QStringLiteral("偏白的像素 %1 个 / 中性墨 %2 个").arg(goLight).arg(goNeutral));

                // 「实例设置」那一颗：白底 + 黑字黑边（`kindBlock("neutral")`）。
                const QColor settingsFill = dominantColor(settingsShot, settingsAll);
                const int settingsPure = countNear(settingsShot, pure, 30);
                const int settingsNeutral = countNear(settingsShot, neutralInk, 30);
                check(colorDistance(settingsFill, QColor(Qt::white)) <= 6 && settingsPure == 0 &&
                          settingsNeutral > 0,
                      "★★ U31.7 「实例设置」是**一般性**那一档：白底黑字黑边，主题色一个像素"
                      "都不沾（点名的分档：启动那种确认性的走主题色，设置"
                      "那种一般性的走黑）",
                      QStringLiteral("多数色 %1 / 本色 %2 个 / 中性墨 %3 个")
                          .arg(settingsFill.name())
                          .arg(settingsPure)
                          .arg(settingsNeutral));

                // 两档的**高度**也真的差一档（主动作 38 / 一般 34）。
                check(recentGo->height() > recentSettings->height(),
                      "★★ U31.7 两档高度落在屏幕上：主动作比一般性那一颗高",
                      QStringLiteral("主动作 %1px / 一般 %2px")
                          .arg(recentGo->height())
                          .arg(recentSettings->height()));
            }

            // 收尾：把这张卡摆回"一次都没启动过"那一档，后面的量（§⑬ 之后那几节）看的是
            // 一张干净的界面。
            ui.setHomeRecent(MainWindowUI::HomeRecent());
            QApplication::processEvents();
            check(!recentCard->isVisibleTo(recentCard->parentWidget()),
                  "★ U31 收尾：摆回「没有记录」那一档 ⇒ 那张卡又收起来了");
        }
    }

#endif
    // ---------- U31b 实例带：只摆真实例，点卡片 = 设为当前，点 ▶ = 启动 ----------
    {
        window.show();
        if (tabs != nullptr) {
            tabs->setCurrentIndex(MainWindowUI::homeTabIndex());
        }
        QList<MainWindowUI::HomeQuickItem> favorites;
        MainWindowUI::HomeQuickItem real;
        real.key = QStringLiteral("demo-instance");
        real.name = QStringLiteral("演示实例");
        favorites.append(real);
        for (int i = 0; i < 2; ++i) {
            MainWindowUI::HomeQuickItem empty;
            empty.empty = true;
            favorites.append(empty);
        }
        ui.setHomeQuickRows(favorites, {});
        QApplication::processEvents();

        QWidget *favoriteSection =
            window.findChild<QWidget *>(QStringLiteral("homeFavoriteSection"));
        QWidget *recentSection =
            window.findChild<QWidget *>(QStringLiteral("homeQuickRecentSection"));
        const QList<QPushButton *> cards =
            favoriteSection != nullptr
                ? favoriteSection->findChildren<QPushButton *>(
                      QRegularExpression(QStringLiteral("^homeQuickCard_")))
                : QList<QPushButton *>();
        check(favoriteSection != nullptr && favoriteSection->isVisibleTo(&window) &&
                  recentSection != nullptr && !recentSection->isVisibleTo(&window) &&
                  cards.size() == 1 &&
                  window.findChildren<QWidget *>(
                            QRegularExpression(QStringLiteral("^homeQuickEmpty_"))).isEmpty(),
              "★★ U31b 1 个实例 + 2 个空项 ⇒ 收藏排只摆 1 张卡、不补「添加实例」空槽；"
              "数量为 0 的最近排整排隐藏",
              QStringLiteral("卡 %1").arg(cards.size()));

        QList<QPair<QString, MainWindowUI::HomeQuickAction>> requests;
        ui.setHomeQuickActionHandler(
            [&requests](const QString &key, MainWindowUI::HomeQuickAction action) {
                requests.append({key, action});
            });
        if (!cards.isEmpty()) {
            cards.first()->click();
            if (auto *play = cards.first()->findChild<QAbstractButton *>(QStringLiteral("homeQuickPlay"))) {
                play->click();
            }
        }
        check(requests.size() == 2 && requests.at(0).first == real.key &&
                  requests.at(0).second == MainWindowUI::HomeQuickAction::Select &&
                  requests.at(1).first == real.key &&
                  requests.at(1).second == MainWindowUI::HomeQuickAction::Launch,
              "★★ U31b 点卡片只发「设为当前」（Select），点卡上那颗 ▶ 才发「启动」（Launch）",
              QStringLiteral("收到 %1 次").arg(requests.size()));

        // 「当前」那枚小字只给 `item.current` 那一张：一排里同时摆着"当前"和别的实例时，
        // 徽章**只能有一个**（r81 那批截图里"最近"那排两个都带着它）。
        {
            MainWindowUI::HomeQuickItem current = real;
            current.key = QStringLiteral("current-one");
            current.name = QStringLiteral("当前那一份");
            current.current = true;
            MainWindowUI::HomeQuickItem other = real;
            other.key = QStringLiteral("other-one");
            other.name = QStringLiteral("另一份");
            other.current = false;
            ui.setHomeQuickRows({current}, {}, {current, other});
            QApplication::processEvents();
            const QList<QLabel *> badges =
                window.findChildren<QLabel *>(QStringLiteral("homeQuickCurrentBadge"));
            check(badges.size() == 1 && badges.first()->parentWidget() != nullptr,
                  "★★ U31b 一排里只画一个「当前」徽章（只认 item.current，不认'排在第几个'）",
                  QStringLiteral("画了 %1 个").arg(badges.size()));
        }
        // 实例带：卡片**跟着名字定宽**（196 下限、320 上限），从这一排的左缘一张挨一张摆，
        // 剩下的宽度不分给卡片（右边空着就空着），实例名整句放得下。
        {
            const QSize before = window.size();
            window.resize(1280, 800);
            QApplication::processEvents();
            MainWindowUI::HomeQuickItem first = real;
            first.key = QStringLiteral("wide-a");
            first.name = QStringLiteral("1.20.1-Forge 47.2.0");
            first.detail = QStringLiteral("Minecraft 1.20.1 / Forge 47.2.0");
            MainWindowUI::HomeQuickItem second = real;
            second.key = QStringLiteral("wide-b");
            second.name = QStringLiteral("1.21.4-Fabric 0.16.9");
            second.detail = QStringLiteral("Minecraft 1.21.4 / NeoForge 21.4.62");
            ui.setHomeQuickRows({first}, {}, {first, second});
            // 两次：第一次摆卡片、第二次让实例带那条内层按新的自然宽重新定位（它是在
            // LayoutRequest 里改自己几何的，和窗口 resize 挤在同一轮）。
            QApplication::processEvents();
            QApplication::processEvents();
            QWidget *recent = window.findChild<QWidget *>(QStringLiteral("homeQuickRecentSection"));
            const QList<QPushButton *> wideCards =
                recent == nullptr
                    ? QList<QPushButton *>()
                    : recent->findChildren<QPushButton *>(
                          QRegularExpression(QStringLiteral("^homeQuickCard_")));
            bool cardsOk = wideCards.size() == 2;
            QStringList detail;
            int used = 0;
            for (QPushButton *card : wideCards) {
                // 名字整句放得下（这一排不截实例名）。
                QLabel *name = nullptr;
                for (QLabel *label : card->findChildren<QLabel *>()) {
                    if (label->text() == QStringLiteral("1.20.1-Forge 47.2.0") ||
                        label->text() == QStringLiteral("1.21.4-Fabric 0.16.9")) {
                        name = label;
                        break;
                    }
                }
                const int needed = name == nullptr ? -1
                                                   : name->fontMetrics().horizontalAdvance(name->text());
                cardsOk = cardsOk && name != nullptr && name->width() >= needed &&
                          card->width() >= 196 && card->width() <= 320;
                // 版本串也整句放得下（卡片宽度同时计入名字行和版本行）。
                QLabel *version = card->findChild<QLabel *>(QStringLiteral("homeQuickTileVersion"));
                cardsOk = cardsOk && version != nullptr &&
                          version->width() >= version->fontMetrics().horizontalAdvance(version->text());
                used += card->width();
                detail << QStringLiteral("卡 %1 / 名字 %2（要 %3）")
                              .arg(card->width())
                              .arg(name == nullptr ? -1 : name->width())
                              .arg(needed);
            }
            // 从这一排的左缘排起，中间只隔 6；第一张卡的**封面左缘**（卡片左缘 + 里面那 8 px）
            // 要和上面「最近」页签的左缘落在同一条线上。
            const int stripLeft = recent == nullptr ? 0 : recent->mapTo(&window, QPoint(0, 0)).x();
            const int firstLeft = wideCards.isEmpty() ? -1 : wideCards.first()->mapTo(&window, QPoint(0, 0)).x();
            const int secondLeft = wideCards.size() < 2 ? -1 : wideCards.at(1)->mapTo(&window, QPoint(0, 0)).x();
            const int gap = wideCards.size() < 2 ? -1 : secondLeft - (firstLeft + wideCards.first()->width());
            QWidget *tab = window.findChild<QWidget *>(QStringLiteral("homeQuickRecentTab"));
            const int tabLeft = tab == nullptr ? -1 : tab->mapTo(&window, QPoint(0, 0)).x();
            check(cardsOk && firstLeft >= stripLeft && firstLeft - stripLeft <= 6 &&
                      qAbs(firstLeft + 8 - tabLeft) <= 2 && gap == 6,
                  "★★ U31b 1280 宽时实例带：卡片跟名字定宽（196~320）、从左缘一张挨一张摆、"
                  "第一张卡的封面和「最近」页签左缘对齐、名字整句放得下",
                  QStringLiteral("卡片起点 %1（这一排 %2）/ 封面 %3（页签 %4）%5 间距 %6")
                      .arg(firstLeft)
                      .arg(stripLeft)
                      .arg(firstLeft + 8)
                      .arg(tabLeft)
                      .arg(detail.join(QStringLiteral("；")))
                      .arg(gap));
            // 剩下的宽度**没有**分给卡片：两张卡加起来远小于这一排的宽度。
            check(recent != nullptr && used + 100 < recent->width(),
                  "★★ U31b 实例带**不把剩下的宽度分给卡片**（右边空着就空着）",
                  QStringLiteral("两张卡 %1 / 这一排 %2").arg(used).arg(recent ? recent->width() : -1));
            // issue #41：悬停 ⇒ ▶ 露出来，名字的可用宽度不能被它挤小（平时就给它留着那一格）。
            bool hoverKeepsName = wideCards.size() == 2;
            QStringList hoverDetail;
            for (QPushButton *card : wideCards) {
                card->setAttribute(Qt::WA_UnderMouse, true);
                const QPoint c(card->width() / 2, card->height() / 2);
                QEnterEvent enter(c, c, card->mapToGlobal(c));
                QApplication::sendEvent(card, &enter);
                QApplication::processEvents();
                QLabel *name = nullptr;
                for (QLabel *label : card->findChildren<QLabel *>())
                    if (label->text().startsWith(QStringLiteral("1.2"))) name = label;
                QWidget *play = card->findChild<QWidget *>(QStringLiteral("homeQuickPlay"));
                const int needed = name == nullptr ? -1 : name->fontMetrics().horizontalAdvance(name->text());
                hoverKeepsName = hoverKeepsName && name != nullptr && play != nullptr && play->isVisible() &&
                                 name->width() >= needed;
                hoverDetail << QStringLiteral("名字 %1（要 %2）").arg(name ? name->width() : -1).arg(needed);
                card->setAttribute(Qt::WA_UnderMouse, false);
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(card, &leave);
            }
            check(hoverKeepsName,
                  "★★ issue #41 悬停（▶ 露出来）时名字整句仍放得下：▶ 那一格平时就留着，不是悬停才抢宽度",
                  hoverDetail.join(QStringLiteral("；")));
            window.resize(before);
            QApplication::processEvents();
        }
        ui.setHomeQuickRows({}, {});
        QApplication::processEvents();
    }

    if (const int result = UiInstanceFiltersChecks(window, ui, tabs); result != 0) return result;

    std::printf("\n===== U33. 更新中心（「检查更新」查出来的结论摊成能动手的一页）=====\n");
    if (const int result = UiUpdateCenterChecks(); result != 0) return result;

    if (const int result = UiServerModeChecks(window, ui, tabs); result != 0) return result;

    if (const int result = UiServerSettingsChecks(); result != 0) return result;

    std::printf("\n===== U38. 键盘焦点与 Tab 顺序 =====\n");
    if (const int result = UiAccessibilityChecks(window); result != 0) return result;

    std::printf("\n===== U39. 工具箱：信标计算 / 酿造助手 =====\n");
    if (const int result = UiBeaconBrewingChecks(); result != 0) return result;

    std::printf("\n===== U40. 工具箱：旗帜设计 / 纹饰搭配 =====\n");
    if (const int result = UiBannerArmorChecks(); result != 0) return result;

    std::printf("\n===== U40b. 文本那一片的自定颜色 / motd，旗帜的「复制指令」 =====\n");
    // ------------------------------------------------------------------
    // 补的两块：1.16 起那份 `#RRGGBB` 自定颜色（`§x…` 那串由 `TextFormat`
    // 算，判据在 verify 的 ZAA7），以及 `server.properties` 的 `motd=` 那一行（ZAA8）。
    // 这里只钉页面：两格在、按下去发的是用户敲的那串原文、插进去落在光标处。
    // ------------------------------------------------------------------
    {
        ToolboxPage page;
        // 工具页第一次进才搭：先进旗帜、文本这两片。
        page.showTool(ToolboxPage::Tool::Banner);
        page.showTool(ToolboxPage::Tool::Text);
        auto *hex = page.findChild<QLineEdit *>(QStringLiteral("textHex"));
        auto *hexInsert = page.findChild<QPushButton *>(QStringLiteral("textHexInsert"));
        auto *motd = page.findChild<QPlainTextEdit *>(QStringLiteral("textMotd"));
        auto *source = page.findChild<QPlainTextEdit *>(QStringLiteral("textSource"));
        check(hex != nullptr && hexInsert != nullptr && motd != nullptr && source != nullptr,
              "★★ U40b 文本那一片：自定颜色两格、motd 那一格、原文那格都在");
        if (hex != nullptr && hexInsert != nullptr && motd != nullptr && source != nullptr) {
            check(hex->maxLength() == 7 && hex->placeholderText() == QStringLiteral("#ff8800"),
                  "★ U40b 自定颜色那格最多七位（`#RRGGBB`），例子是 `#ff8800`",
                  QStringLiteral("%1 / %2").arg(hex->maxLength()).arg(hex->placeholderText()));

            QStringList asked;
            QObject::connect(&page, &ToolboxPage::textHexColorRequested, &page,
                             [&asked](const QString &value) { asked.append(value); });
            hex->setText(QStringLiteral("ff8800"));
            hexInsert->click();
            check(asked == QStringList{QStringLiteral("ff8800")},
                  "★★ U40b 「插自定颜色」发的是用户敲的那串原文（`#` 可写可不写，怎么认不在这一层）",
                  asked.join(QChar(',')));
            hex->setText(QStringLiteral("#00ff00"));
            emit hex->returnPressed();
            check(asked == QStringList{QStringLiteral("ff8800"), QStringLiteral("#00ff00")},
                  "★ U40b 在那格里按回车也算「插一下」", asked.join(QChar(',')));

            source->setPlainText(QStringLiteral("abcd"));
            QTextCursor cursor = source->textCursor();
            cursor.setPosition(2);
            source->setTextCursor(cursor);
            page.insertTextAtCursor(QStringLiteral("§x"));
            check(source->toPlainText() == QStringLiteral("ab§xcd"),
                  "★★ U40b `insertTextAtCursor()` 插在**光标处**，不是整份覆盖、也不是接到尾巴",
                  source->toPlainText());

            page.setTextMotd(QStringLiteral("motd=欢迎\\u6765"));
            check(motd->toPlainText() == QStringLiteral("motd=欢迎\\u6765"),
                  "★ U40b motd 那一格逐字摆进来（转义怎么写不在这一层）");
        }

        auto *bannerCopy = page.findChild<QPushButton *>(QStringLiteral("toolboxBannerCopy"));
        check(bannerCopy != nullptr, "★★ U40b 旗帜那一片的「复制指令」在");
        if (bannerCopy != nullptr) {
            check(!bannerCopy->isEnabled(),
                  "★★ U40b 还没算出指令时那颗按钮灰着（点下去不能复制一份空串）");
            page.setBannerCommand(QStringLiteral("/give @p white_banner"));
            check(bannerCopy->isEnabled(),
                  "★ U40b 算出指令 ⇒ 按钮可按");
            bannerCopy->click();
            check(QApplication::clipboard() != nullptr
                      && QApplication::clipboard()->text() == QStringLiteral("/give @p white_banner"),
                  "★★ U40b 按一下把那条指令真复制进剪贴板",
                  QApplication::clipboard() != nullptr ? QApplication::clipboard()->text() : QString());
            page.setBannerCommand(QString());
            check(!bannerCopy->isEnabled(),
                  "★ U40b 指令被撤掉（算不出来）⇒ 按钮跟着灰回去");
        }
        page.close();
        QApplication::processEvents();
    }

    std::printf("\n===== U40c. 附魔那张表：「和上一行同一件」那一列 =====\n");
    // ------------------------------------------------------------------
    // 一本书能带好几条附魔、一件装备本来也可能已经有好几条 —— 表格一行只放得下一条，
    // 所以第五列勾上 = 并进**上一行**那一件（分组在 controller，`runAnvilPlan()`）。
    // 这里钉页面：那一列在、第一行没有"上一行"所以灰着、勾完读得回来、删掉第一行之后
    // 新的第一行也跟着灰回去。
    // ------------------------------------------------------------------
    {
        ToolboxPage page;
        page.resize(900, 560);
        page.show();
        // 和 controller 一样：附魔那一片搭出来之后再灌表。
        page.showTool(ToolboxPage::Tool::Anvil);
        page.setEnchantmentChoices(
            {{QStringLiteral("sharpness"), QStringLiteral("锋利")},
             {QStringLiteral("unbreaking"), QStringLiteral("耐久")}});
        QApplication::processEvents();
        auto *rowsHost = page.findChild<QWidget *>(QStringLiteral("anvilRows"));
        auto *addRow = page.findChild<QPushButton *>(QStringLiteral("anvilAddRow"));
        auto *rename = page.findChild<QCheckBox *>(QStringLiteral("anvilRename"));
        check(rowsHost != nullptr && addRow != nullptr && rename != nullptr,
              "★★ U40c 附魔那一页：一行行输入区、「加一行」、改名开关都在");
        if (rename != nullptr) {
            check(!rename->isChecked() && !page.anvilRename(),
                  "★ U40c 「顺带改名」默认不勾（改名那 1 级默认不算）");
            rename->setChecked(true);
            check(page.anvilRename(),
                  "★★ U40c 勾上 ⇒ `anvilRename()` 读得回来（controller 拿它决定算不算那 1 级）");
        }
        if (rowsHost != nullptr && addRow != nullptr) {
            // 「同上一件」= 并进上一行那一件：它是「物品」下拉的一项，第一行没有上一行，所以第一行的下拉里没有这一项。
            const auto kindsAt = [&page](int row) -> QComboBox * {
                const auto combos = page.findChildren<QComboBox *>();
                int seen = -1;
                for (QComboBox *combo : combos) {
                    if (!combo->isVisibleTo(&page) || combo->findData(QStringLiteral("book")) < 0) continue;
                    if (++seen == row) return combo;
                }
                return nullptr;
            };
            QComboBox *first = kindsAt(0);
            check(first != nullptr && first->findData(QStringLiteral("same")) < 0 && first->count() == 2,
                  "★★ U40c 一进来那一行：物品下拉只有「附魔书 / 装备」，第一行没有「同上一件」");

            addRow->click();
            QApplication::processEvents();
            QComboBox *second = kindsAt(1);
            check(second != nullptr && second->findData(QStringLiteral("same")) >= 0,
                  "★ U40c 加一行 ⇒ 新那一行的物品下拉里多出「同上一件」");
            if (second != nullptr) {
                second->setCurrentIndex(second->findData(QStringLiteral("same")));
                const QList<ToolboxPage::AnvilRow> rows = page.anvilRows();
                check(rows.size() == 2 && rows.at(1).sameAsPrevious && !rows.at(0).sameAsPrevious,
                      "★★ U40c 选「同上一件」⇒ `anvilRows()` 读得回来（第一行恒 false）",
                      QStringLiteral("%1 行").arg(rows.size()));
                // 「已用次数」描述的是那一件东西 ⇒ 并进上一行时这一格归上一行管、灰掉。
                const auto spins = page.findChildren<QSpinBox *>();
                QList<QSpinBox *> uses;
                for (QSpinBox *spin : spins)
                    if (spin->isVisibleTo(&page) && spin->suffix() == QStringLiteral(" 次")) uses << spin;
                check(uses.size() == 2 && uses.at(0)->isEnabled() && !uses.at(1)->isEnabled(),
                      "★★ U40c 并进上一行 ⇒ 这一行自己的「已用次数」灰掉");
            }
            // 往回设置：第一行传 sameAsPrevious=true 也不能生效（第一行没有上一行）。
            page.setAnvilRows({{QStringLiteral("sharpness"), 3, true, 0, true},
                               {QStringLiteral("unbreaking"), 2, false, 1, true}});
            const QList<ToolboxPage::AnvilRow> restored = page.anvilRows();
            check(restored.size() == 2 && !restored.at(0).sameAsPrevious && restored.at(1).sameAsPrevious &&
                      !restored.at(1).isBook == false,
                  "★ U40c 程序设置整表：第一行的「同上一件」被忽略，后面的行保留");
        }
        page.close();
        QApplication::processEvents();
    }

    std::printf("\n===== U43. 版本树上面的筛选框（「版本树没有过滤/搜索」） =====\n");
    // ------------------------------------------------------------------
    // 过滤住在界面这一层（MainWindowUI::applyVersionFilter）：它只藏/露条目、撑开命中的
    // 分类，一个字节的数据都不碰。清单重灌那一半在 MainWindow（refreshAvailableVersions
    // 末尾再叫一次），这里把"重灌之后按框里现在的字再筛一遍"也走一遍。
    // ------------------------------------------------------------------
    {
        auto *filter = ui.getVersionFilterEdit();
        auto *hint = ui.getVersionFilterHint();
        check(filter != nullptr && filter->objectName() == MainWindowUI::versionFilterEditName()
                  && hint != nullptr && hint->objectName() == MainWindowUI::versionFilterHintName(),
              "★★ U43 筛选框和它右边那句结果都找得回来（按 objectName）");
        check(filter != nullptr && !filter->placeholderText().isEmpty(),
              "★ U43 框里那句提示不是空的（空框时它就是唯一的说明）",
              filter == nullptr ? QString() : filter->placeholderText());
        check(hint != nullptr && hint->isHidden(),
              "★★ U43 还没筛的时候那句结果收着（不占版面）");

        if (filter != nullptr && hint != nullptr) {
            // 造一棵小树：两个版本号的正式版、两个快照、一个远古版。产品那边是
            // MainWindow 灌的，这一层只认 `Qt::UserRole` 里那串版本号。
            versionTree->clear();
            auto *release = new QTreeWidgetItem(versionTree);
            release->setData(0, Qt::UserRole, QStringLiteral("release"));
            release->setText(0, QStringLiteral("正式版"));
            for (const QString &id : {QStringLiteral("1.20.1"), QStringLiteral("1.21.4")}) {
                auto *child = new QTreeWidgetItem(release);
                child->setData(0, Qt::UserRole, id);
                child->setText(0, id);
            }
            release->setExpanded(true);
            auto *snapshots = new QTreeWidgetItem(versionTree);
            snapshots->setData(0, Qt::UserRole, QStringLiteral("snapshot"));
            snapshots->setText(0, QStringLiteral("快照版"));
            for (const QString &id : {QStringLiteral("24w14a"), QStringLiteral("24w15a")}) {
                auto *child = new QTreeWidgetItem(snapshots);
                child->setData(0, Qt::UserRole, id);
                child->setText(0, id);
            }
            auto *ancient = new QTreeWidgetItem(versionTree);
            ancient->setData(0, Qt::UserRole, QStringLiteral("ancient"));
            ancient->setText(0, QStringLiteral("远古版"));
            auto *old = new QTreeWidgetItem(ancient);
            old->setData(0, Qt::UserRole, QStringLiteral("rd-132211"));
            old->setText(0, QStringLiteral("rd-132211"));

            filter->setText(QStringLiteral("1.2"));
            check(!release->isHidden() && release->isExpanded()
                      && !release->child(0)->isHidden() && !release->child(1)->isHidden(),
                  "★★ U43 命中的分类自动撑开、命中的版本号露出来");
            check(snapshots->isHidden() && ancient->isHidden(),
                  "★★ U43 一条都不命中的分类整条收起来（不留在结果里当空行）");
            check(!hint->isHidden() && hint->text() == MainWindowUI::versionFilterMatchText(2),
                  "★★ U43 右边那句说的是「找到 2 个版本」",
                  hint->text());

            // 空格分词：两个词都得出现（和实例搜索、文件搜索同一条规矩）。
            filter->setText(QStringLiteral("24w 15"));
            check(release->isHidden() && !snapshots->isHidden() && snapshots->isExpanded(),
                  "★★ U43 空格分词：`24w 15` 落在快照那一栏");
            check(snapshots->child(0)->isHidden() && !snapshots->child(1)->isHidden(),
                  "★★ U43 同一个分类里不命中的版本号也各藏各的");
            check(hint->text() == MainWindowUI::versionFilterMatchText(1),
                  "★ U43 计数跟着变（这一趟是 1 个）", hint->text());

            filter->setText(QStringLiteral("zzz"));
            check(release->isHidden() && snapshots->isHidden() && ancient->isHidden(),
                  "★★ U43 一个都不命中 ⇒ 三栏都收起来");
            check(!hint->isHidden()
                      && hint->text() == MainWindowUI::versionFilterNoMatchText(QStringLiteral("zzz")),
                  "★★ U43 一个都不命中时说「没有版本号匹配「…」」（不是一片空白像坏了）",
                  hint->text());

            filter->clear();
            check(!release->isHidden() && !snapshots->isHidden() && !ancient->isHidden()
                      && !release->child(0)->isHidden() && !snapshots->child(0)->isHidden(),
                  "★★ U43 清空筛选 ⇒ 条目全露出来");
            check(release->isExpanded() && !snapshots->isExpanded() && !ancient->isExpanded(),
                  "★★ U43 清空筛选 ⇒ 展开状态回到**筛之前**那样（不是筛完四栏全开）");
            check(hint->isHidden(),
                  "★ U43 不筛了那句结果收回去");

            // 重灌：框里的字留着，条目全是新摆的 ⇒ 产品在 refreshAvailableVersions 末尾
            // 再叫一次 applyVersionFilter()。这里把那一趟照走一遍。
            filter->setText(QStringLiteral("1.20"));
            versionTree->clear();
            auto *freshRelease = new QTreeWidgetItem(versionTree);
            freshRelease->setData(0, Qt::UserRole, QStringLiteral("release"));
            freshRelease->setText(0, QStringLiteral("正式版"));
            for (const QString &id : {QStringLiteral("1.20.1"), QStringLiteral("1.18.2")}) {
                auto *child = new QTreeWidgetItem(freshRelease);
                child->setData(0, Qt::UserRole, id);
                child->setText(0, id);
            }
            auto *freshSnapshot = new QTreeWidgetItem(versionTree);
            freshSnapshot->setData(0, Qt::UserRole, QStringLiteral("snapshot"));
            freshSnapshot->setText(0, QStringLiteral("快照版"));
            auto *freshSnap = new QTreeWidgetItem(freshSnapshot);
            freshSnap->setData(0, Qt::UserRole, QStringLiteral("24w14a"));
            freshSnap->setText(0, QStringLiteral("24w14a"));
            check(!freshSnapshot->isHidden(), "（重灌前：新摆的条目本该全是露着的）");
            ui.applyVersionFilter();
            check(!freshRelease->isHidden() && freshRelease->isExpanded()
                      && !freshRelease->child(0)->isHidden()
                      && freshRelease->child(1)->isHidden(),
                  "★★ U43 重灌之后再筛一遍：框里的字还算数（不是「框里写着 1.20、树里全摆着」）");
            check(freshSnapshot->isHidden(),
                  "★★ U43 重灌之后不命中的分类照样收起来");
            filter->clear();
        }
        // 收尾：后面没有别的地方读这棵树了，但把它清空、回到"没在筛"的样子。
        if (versionTree != nullptr) versionTree->clear();
    }

    popupGuard.report();
    std::printf("\n===== %s: %d/%d =====\n", g_fail == 0 ? "全部通过" : "有失败", g_pass,
                g_pass + g_fail);
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}





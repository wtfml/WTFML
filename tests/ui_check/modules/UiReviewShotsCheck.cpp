// 走查验收截图（offscreen，QWidget::grab()）。
//
// 默认这一趟（不带参数）保持原来的行为：新安装默认 + 三个具名预设 ×
// 主页零实例 / 管理页空状态 / 设置·启动 / 设置·下载 / 下载·游戏，窗口 1001×776。
//
// 带 `--extended` 时出一整套「默认窗口尺寸」参考图：指定一个主题和一个窗口尺寸，
// 按固定页面顺序把 30 张图拍下来（文件名为 <页面>-<状态>.png）：
//
//   QT_QPA_PLATFORM=offscreen ui_check_review_shots --extended \
//       --theme dark --size 1280x800 --out D:/shots/dark-1280
//
// `--background <图片绝对路径>` 把它设成自定义页面背景图（铺满整窗），用来看墨色 / 品牌标的取色。
//
// `--popups` 只拍自造顶层浮层打开时的样子（智能体实例下拉 / ⋯ 菜单、下拉弹层、右键菜单、
// tooltip、实例页文件夹浮层与分组齿轮菜单），每张另出一张 `-zoom` 的两倍放大，看四角。
// 浮层是独立的顶层窗，`window.grab()` 拍不到，这里按全局位置叠回主窗口那张图上。
//
// 尺寸只影响主窗口（离屏；出图像素 = 逻辑尺寸 × DPR，`--site` 用 QT_SCALE_FACTOR=2 拍 2 倍图）；
// 实例设置对话框是产品里那个 820×560 的浮层，
// 这里照真实样子嵌在覆盖层里拍，不跟着窗口拉伸。
//
// `--site`（步骤 174）一次拍官网「功能详解」页的候选截图：1120×760、默认暮色雪山，
// 主页按主题各一张（dark / emerald / ocean / system），其余是设置·外观、安装页·选加载器、
// 任务中心、Java 管理、导入 / 导出整合包、实例模组 / 存档 / 升级、服务端控制台 /
// server.properties / 玩家名单、智能体写文件审批卡、工具箱三页。夹具全是代码里的假数据
// （Steve、D:\Games\WTFML\… 这类通用路径），不联网、不读本机的 .minecraft 或用户设置。
#include "Support.hpp"
#include "InputWidthScan.hpp"
#include "LoadingState.hpp"
#include "PresetManagePage.hpp"
#include "InlineMessage.hpp"
#include "PresetInstallPicker.hpp"

#include "AgentPage.hpp"
#include "AgentApprovalCard.hpp"
#include "ActivityPopover.hpp"
#include "AgentSettingsPage.hpp"
#include "AgentViewTypes.hpp"
#include "agent/AgentSession.hpp"
#include "OverlayHost.hpp"
#include "MainWindowPageLayout.hpp"
#include "Popups.hpp"
#include "ToolboxPage.hpp"
#include "ToolboxSamples.hpp"
#include "VersionIcons.hpp"

// 「Java 管理」「服务端控制台」「服务端设置」三页：官网截图（--site）要单独摆出来拍。
#include "JavaManageDialog.hpp"
#include "ServerConsolePage.hpp"
#include "ServerSettingsPage.hpp"
// server.properties 那十几项的名字 / 值域只有这一处出处（产品也是拿它建表单）。
#include "server/ServerPropertiesForm.hpp"

#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFrame>
#include <QKeyEvent>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include <tuple>

namespace {

/// 给已有焦点的控件送一发 Tab：让 Theme 的焦点过滤器记下"最后一次输入是键盘"。
/// W 的焦点下划线只认键盘到达的焦点，样张里光 setFocus(TabFocusReason) 不再画。
void sendTabKey(QWidget *target) {
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(target, &tab);
    QApplication::processEvents();
}


struct ShotOptions {
    QString out;
    QString theme;
    QSize size{0, 0};
    QString background; ///< 自定义页面背景图（绝对路径），空 = 不设
    bool extended = false;
    bool popups = false;
    bool loading = false;  ///< `--loading`：只拍加载态（步骤 122）那几张
    bool agentPolish = false;
    bool files = false;  ///< `--files`：只拍实例设置·模组页 60 个模组（步骤 139），文件名 r139-<主题>-*
    /// `--files --first-disable`：40 个模组全启用 → 禁用第一个，前后各一张（看列表跳不跳），
    /// 文件名 first-disable-<before|after>-<宽>.png
    bool firstDisable = false;
    /// `--r151`：步骤 151 的四处（资源包页查完更新前后 / 预设弹窗标题图标 / 服务器表头 /
    /// 主页窗口按钮），文件名 r151-<主题>-<宽>-*；配 `--background` 拍亮背景图那一档。
    bool r151 = false;
    /// `--r155`：步骤 155（默认 / 最小窗口、点 W 收放导航、窗口大小「恢复默认」），自己换尺寸，
    /// 不看 `--size`；文件名 r155-<主题>-<宽>x<高>-<页面>-<状态>.png。
    bool r155 = false;
    /// `--r157`：步骤 157（导航收起时未读失败挂在进度环右上角），960×620 主页收起 + 展开对照 + 角标放大，
    /// 文件名 r157-<主题>-960x620-*.png。
    bool r157 = false;
    /// `--i85`：issue #85（图标轨补运行状态入口），默认窗口下图标轨一个进程、多个进程加面板、展开轨对照，
    /// 演示数据是写进界面的假进程行；开发用截图工具，不进启动器；文件名 i85-<主题>-<宽>x<高>-*.png。
    bool i85 = false;
    /// `--r179c`：步骤 179 UI 问题 C（设置页居中、实例设置最小窗、左栏字重、任务中心运行中开关、W 焦点），
    /// 自己换尺寸；文件名 r179c-<主题>-<宽>x<高>-<页面>.png。
    bool r179c = false;
    /// `--r70kbd`：#70 第二批（列表键盘可用性），实例页用键盘选中一行：行尾按钮露出、当前行有焦点标记。
    /// 开发用截图工具，不进启动器；文件名 r70kbd-<主题>-<宽>x<高>-<页面>.png。
    bool r70kbd = false;
    /// `--site`：步骤 174（官网「功能详解」页候选截图），1120×760、默认暮色雪山，
    /// 一次跑完整张清单；文件名就按官网那一列（look-home-<主题> / install-* / inst-* / …）。
    bool site = false;
};

/// 截图进程用的字体：系统那份雅黑 + 系统那份等宽。
///
/// 离屏平台**不枚举系统字体**（`QFontDatabase::families()` 是空的），而产品里凡是等宽的
/// 地方都问 `QFontDatabase::systemFont(FixedFont)`（智能体行内代码的 `<code>` 要的是
/// "monospace"，服务端控制台 / 配置编辑器 / 工具箱哈希同理）。一个等宽字体都没有时，
/// Qt 会退到某个应用字体 —— 截图里这些行内代码就成了 Dancing Script 手写体。照 msyh.ttc
/// 的办法把系统那份等宽注册进来之后，`monospace` 能落到 Courier New 上（真启动器用 windows
/// 平台插件，本来就枚举得到系统字体，不是产品的问题）。
void installShotFonts(QApplication &app) {
    const QDir fonts(QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts")));
    const QString msyh = fonts.filePath(QStringLiteral("msyh.ttc"));
    const int fontId = QFile::exists(msyh) ? QFontDatabase::addApplicationFont(msyh) : -1;
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    app.setFont(QFont(families.isEmpty() ? QStringLiteral("Microsoft YaHei UI") : families.first(), 9));
    // 真启动器用 windows 平台插件时，`systemFont(FixedFont)` 与 `<code>` 的 "monospace"
    // 都落到 Courier New（实测：字体库里 Courier New 在、Consolas 也在）。这里优先注册
    // 同一份，截图才和玩家看到的一致；后面的几档只是别的机器上没装 Courier 时的兜底。
    for (const QString &name : {QStringLiteral("cour.ttf"), QStringLiteral("consola.ttf"),
                                QStringLiteral("CascadiaMono.ttf"), QStringLiteral("lucon.ttf")}) {
        const QString path = fonts.filePath(name);
        if (!QFile::exists(path)) continue;
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) continue;
        const QStringList mono = QFontDatabase::applicationFontFamilies(id);
        std::printf("  等宽字体：%s（%s）\n", qPrintable(name),
                    qPrintable(mono.isEmpty() ? QStringLiteral("?") : mono.first()));
        break;
    }
    QFont mono;
    mono.setFamilies({QStringLiteral("monospace")});
    std::printf("  monospace 落到：%s\n", qPrintable(QFontInfo(mono).family()));
}

/// 颜色块 + 一个字母的小图标（资源行 / 账号行用；产品里那些是真图，这里只要不像空白）。
QPixmap letterMark(const QColor &color, const QString &letter, int size) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(0, 0, size, size), size * 0.24, size * 0.24);
    QFont font = QApplication::font();
    font.setPixelSize(qMax(9, size / 2));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(QColor(255, 255, 255, 235));
    painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, letter.left(1).toUpper());
    return pixmap;
}

/// 账号弹层里那张"脸"：圆底 + 首字母。
QPixmap faceMark(const QColor &color, const QString &letter, int size) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(0, 0, size, size));
    QFont font = QApplication::font();
    font.setPixelSize(qMax(9, size / 2));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(QColor(255, 255, 255, 235));
    painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, letter.left(1).toUpper());
    return pixmap;
}

/// 设置页里那些下拉框 / 勾选框 / 数值框，平时由 MainWindow 灌；截图前灌一份样例。
void fillSettingsSamples(MainWindowUI &ui) {
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
    fill(ui.getMemoryModeCombo(), {QStringLiteral("自动分配"), QStringLiteral("手动分配")}, 1);
    if (ui.getJavaPathLabel() != nullptr) ui.getJavaPathLabel()->setText(QStringLiteral("自动选择　按游戏版本"));
    if (ui.getMemoryHint() != nullptr)
        ui.getMemoryHint()->setText(QStringLiteral("系统总内存 31.5 GB，当前可用 14.5 GB；最小内存由启动器设为 1024 MB。"));
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
    if (ui.getHomeRecentCountSpin() != nullptr) ui.getHomeRecentCountSpin()->setValue(3);
}

/// 主页那几排快捷方式：4 个已安装实例 + 2 个收藏 + 2 个最近，含一个带加载器的。
///
/// NOTE: `current` 这一位**全表只有一个真**（就是下拉框里选中的那一份，见
/// `HomeQuickController::build()`）；这里 4 个都标上就会在「最近」条上摆出两个「当前」——
/// r81 那批截图正是这么错的。
void fillHomeSamples(MainWindowUI &ui) {
    const auto item = [](const QString &key, const QString &name, const QString &detail, bool current,
                         bool favorite, bool recent) {
        MainWindowUI::HomeQuickItem row;
        row.key = key;
        row.name = name;
        row.detail = detail;
        row.icon = QIcon(VersionIcons::isoBlock(VersionIcons::isoBlockKindFor(name),
                                                MainWindowUI::instanceTreeIconSize() * 2));
        row.current = current;
        row.favorite = favorite;
        row.recent = recent;
        return row;
    };
    // 第一个是当前实例（唯一一个 `current = true`）。
    const QList<MainWindowUI::HomeQuickItem> installed{
        item(QStringLiteral("1.20.1-Forge"), QStringLiteral("1.20.1-Forge 47.2.0"),
             QStringLiteral("Minecraft 1.20.1 / Forge 47.2.0"), true, false, true),
        item(QStringLiteral("1.21.4-Fabric"), QStringLiteral("1.21.4-Fabric 0.16.9"),
             QStringLiteral("Minecraft 1.21.4 / Fabric 0.16.9"), false, true, true),
        item(QStringLiteral("1.20.4"), QStringLiteral("1.20.4 原版"),
             QStringLiteral("Minecraft 1.20.4 / 原版"), false, false, false),
        item(QStringLiteral("1.19.2-NeoForge"), QStringLiteral("1.19.2-NeoForge 20.4.237"),
             QStringLiteral("Minecraft 1.19.2 / NeoForge 20.4.237"), false, true, false)};
    const QList<MainWindowUI::HomeQuickItem> favorites{
        installed.at(1), installed.at(3)};
    const QList<MainWindowUI::HomeQuickItem> recent{installed.at(0), installed.at(1)};
    ui.setHomeQuickRows({installed.at(0)}, favorites, recent);
    ui.setHomeQuickMode(MainWindowUI::HomeQuickMode::Current);
}

/// 实例页那张树：两个组、5 个实例（4 个带加载器）。行形状照 MainWindowViewData::fillInstanceTree()。
void fillInstanceTree(MainWindowUI &ui) {
    QTreeWidget *tree = ui.getInstanceList();
    if (tree == nullptr) return;
    tree->clear();
    const auto addRow = [&](QTreeWidgetItem *parent, const QString &text, const QString &id, int loader) {
        auto *row = new QTreeWidgetItem(parent);
        row->setText(0, text);
        row->setData(0, MainWindowUI::instanceIdRole(), id);
        row->setToolTip(0, QStringLiteral("D:/Games/.minecraft/versions/%1").arg(id));
        row->setIcon(0, QIcon(VersionIcons::loaderMark(loader, MainWindowUI::instanceTreeIconSize())));
        row->setData(0, MainWindowUI::rowPinRole(), true);
        return row;
    };
    const auto addGroup = [&](const QString &name, const QString &id) {
        auto *group = new QTreeWidgetItem(tree);
        group->setText(0, MainWindowUI::groupItemText(name, 0));
        group->setData(0, MainWindowUI::groupIdRole(), id);
        group->setData(0, MainWindowUI::rowGroupGearRole(), true);
        group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
        group->setExpanded(true);
        return group;
    };
    QTreeWidgetItem *modded = addGroup(QStringLiteral("1.20.1 整合包"), QStringLiteral("g-modded"));
    addRow(modded, QStringLiteral("1.20.1-Forge 47.2.0"), QStringLiteral("1.20.1-Forge"), 1);
    addRow(modded, QStringLiteral("1.20.1-Fabric 0.15.11"), QStringLiteral("1.20.1-Fabric"), 0);
    modded->setText(0, MainWindowUI::groupItemText(QStringLiteral("1.20.1 整合包"), modded->childCount()));
    QTreeWidgetItem *other = addGroup(QStringLiteral("其他"), QStringLiteral("g-other"));
    addRow(other, QStringLiteral("1.21.4-Fabric 0.16.9"), QStringLiteral("1.21.4-Fabric"), 0);
    addRow(other, QStringLiteral("1.19.2-NeoForge 20.4.237"), QStringLiteral("1.19.2-NeoForge"), 2);
    addRow(other, QStringLiteral("1.20.4 原版"), QStringLiteral("1.20.4"), 4);
    other->setText(0, MainWindowUI::groupItemText(QStringLiteral("其他"), other->childCount()));
    if (other->childCount() > 0) tree->setCurrentItem(other->child(0));
}

/// 下载·游戏：最新版本 + 四栏，正式版展开（照 MainWindow::refreshAvailableVersions() 的行形状）。
void fillVersionTree(MainWindowUI &ui) {
    QTreeWidget *tree = ui.getVersionTree();
    if (tree == nullptr) return;
    tree->clear();
    const int column = MainWindowUI::versionTreeKeyColumn();
    const auto addLeaf = [&](QTreeWidgetItem *parent, const QString &id, const QString &date, int channel) {
        auto *leaf = new QTreeWidgetItem(parent);
        leaf->setData(column, Qt::UserRole, id);
        leaf->setText(column, id);
        leaf->setData(column, MainWindowUI::rowSubTextRole(), date);
        leaf->setData(column, MainWindowUI::rowIconRole(),
                      VersionIcons::versionChannelMark(channel, MainWindowUI::versionTreeIconSize()));
        return leaf;
    };
    auto *latest = new QTreeWidgetItem(tree);
    latest->setData(column, Qt::UserRole, QStringLiteral("latest"));
    latest->setText(column, QStringLiteral("最新版本"));
    addLeaf(latest, QStringLiteral("1.21.4"), QStringLiteral("2025-12-09"), 0);
    addLeaf(latest, QStringLiteral("25w50a"), QStringLiteral("2025-12-11"), 1);
    latest->setExpanded(true);

    const QList<QPair<QString, QString>> channels{
        {QStringLiteral("release"), QStringLiteral("正式版")},
        {QStringLiteral("snapshot"), QStringLiteral("快照版")},
        {QStringLiteral("old"), QStringLiteral("远古版")},
        {QStringLiteral("april"), QStringLiteral("愚人节版")}};
    const QList<QStringList> ids{
        {QStringLiteral("1.21.4"), QStringLiteral("1.21.3"), QStringLiteral("1.21.2"),
         QStringLiteral("1.21.1"), QStringLiteral("1.21"), QStringLiteral("1.20.6"),
         QStringLiteral("1.20.4"), QStringLiteral("1.20.1"), QStringLiteral("1.19.2"),
         QStringLiteral("1.18.2"), QStringLiteral("1.16.5"), QStringLiteral("1.12.2")},
        {QStringLiteral("25w50a"), QStringLiteral("25w49a"), QStringLiteral("25w45a")},
        {QStringLiteral("b1.7.3"), QStringLiteral("a1.2.6")},
        {QStringLiteral("15w14a"), QStringLiteral("3D Shareware v1.34")}};
    for (int c = 0; c < channels.size(); ++c) {
        auto *top = new QTreeWidgetItem(tree);
        top->setData(column, Qt::UserRole, channels.at(c).first);
        top->setText(column, channels.at(c).second);
        top->setData(column, MainWindowUI::rowIconRole(),
                     VersionIcons::versionChannelMark(c, MainWindowUI::versionTreeIconSize()));
        for (const QString &id : ids.at(c)) addLeaf(top, id, QStringLiteral("2025-12-09"), c);
        top->setExpanded(c == 0);  // 只要「正式版」那一组展开
    }
}

/// 下载·模组：一页资源结果（5 条），第二条底下展开两个游戏版本。
void fillResourceTree(MainWindowUI &ui, const QString &categoryKey) {
    QTreeWidget *tree = ui.getDownloadResourceTree(categoryKey);
    if (tree == nullptr) return;
    tree->clear();
    const int column = MainWindowUI::versionTreeKeyColumn();
    const QList<QPair<QString, QString>> hits{
        {QStringLiteral("Sodium"), QStringLiteral("jellysquid3 / 42.1M")},
        {QStringLiteral("Iris Shaders"), QStringLiteral("IrisShaders / 18.7M")},
        {QStringLiteral("Fabric API"), QStringLiteral("modmuss50 / 96.3M")},
        {QStringLiteral("Lithium"), QStringLiteral("jellysquid3 / 21.5M")},
        {QStringLiteral("JEI"), QStringLiteral("mezz / 88.0M")}};
    const QList<QColor> colors{QColor(0x4f, 0x8a, 0xd8), QColor(0x7a, 0x5c, 0xc9), QColor(0x3c, 0x9e, 0x7a),
                               QColor(0xc9, 0x7a, 0x3c), QColor(0x9e, 0x4b, 0x6a)};
    for (int i = 0; i < hits.size(); ++i) {
        auto *hit = new QTreeWidgetItem(tree);
        hit->setData(column, Qt::UserRole, QStringLiteral("project-%1").arg(i + 1));
        hit->setData(column, MainWindowUI::resourceKindRole(), categoryKey);
        hit->setData(column, MainWindowUI::resourceSourceRole(), QStringLiteral("modrinth"));
        hit->setText(column, hits.at(i).first);
        hit->setData(column, MainWindowUI::rowSubTextRole(), hits.at(i).second);
        hit->setData(column, MainWindowUI::rowIconRole(),
                     letterMark(colors.at(i), hits.at(i).first, MainWindowUI::resourceTreeIconSize()));
        if (i == 1) {
            for (const QString &version : {QStringLiteral("1.21.4"), QStringLiteral("1.20.1")}) {
                auto *game = new QTreeWidgetItem(hit);
                game->setData(column, Qt::UserRole, version);
                game->setText(column, version);
            }
            hit->setExpanded(true);
        }
    }
}

/// 实例设置对话框的夹具：一个装了点东西的实例目录（模组页要有内容）。
void makeInstanceFixture(const QString &root) {
    const auto mkfile = [](const QString &path) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(QByteArray(4096, 'x'));
    };
    mkfile(root + QStringLiteral("/mods/sodium-0.6.13.jar"));
    mkfile(root + QStringLiteral("/mods/iris-1.8.1.jar"));
    mkfile(root + QStringLiteral("/mods/lithium-0.14.3.jar"));
    mkfile(root + QStringLiteral("/mods/old-fabric-api.jar.disabled"));
    mkfile(root + QStringLiteral("/resourcepacks/材质.zip"));
    mkfile(root + QStringLiteral("/datapacks/数据.zip"));
    mkfile(root + QStringLiteral("/shaderpacks/光影.zip"));
    mkfile(root + QStringLiteral("/saves/我的世界/level.dat"));
    mkfile(root + QStringLiteral("/logs/latest.log"));
    mkfile(root + QStringLiteral("/screenshots/2026-11-12_20.31.07.png"));
    QDir().mkpath(root + QStringLiteral("/game"));
}

/// 账号弹层的两种状态。
QList<MainWindowUI::PanelRow> accountRows(bool signedIn) {
    QList<MainWindowUI::PanelRow> rows;
    if (!signedIn) {
        MainWindowUI::PanelRow empty;
        empty.placeholder = true;
        empty.title = QStringLiteral("未登录");
        rows.append(empty);
        MainWindowUI::PanelRow login;
        login.menuItem = true;
        login.title = QStringLiteral("登录");
        login.key = QStringLiteral("login");
        rows.append(login);
        return rows;
    }
    MainWindowUI::PanelRow steve;
    steve.key = QStringLiteral("steve");
    steve.title = QStringLiteral("Steve");
    steve.subtitle = QStringLiteral("正版");
    steve.icon = faceMark(QColor(0x3c, 0x9e, 0x7a), QStringLiteral("S"), 32);
    steve.checked = true;
    rows.append(steve);
    MainWindowUI::PanelRow alex;
    alex.key = QStringLiteral("alex");
    alex.title = QStringLiteral("Alex");
    alex.subtitle = QStringLiteral("离线");
    alex.icon = faceMark(QColor(0xc9, 0x7a, 0x3c), QStringLiteral("A"), 32);
    rows.append(alex);
    MainWindowUI::PanelRow add;
    add.key = QStringLiteral("add");
    add.title = QStringLiteral("添加账号");
    add.menuItem = true;
    rows.append(add);
    MainWindowUI::PanelRow manage;
    manage.key = QStringLiteral("manage");
    manage.title = QStringLiteral("管理账号与皮肤");
    manage.menuItem = true;
    rows.append(manage);
    return rows;
}

/// 任务中心那两三条任务（一条在跑、一条已完成、一条失败）。
QList<TaskCenter::Item> activityItems() {
    QList<TaskCenter::Item> items;
    TaskCenter::Item download;
    download.key = QStringLiteral("1");
    download.title = QStringLiteral("下载 Sodium 0.6.13");
    download.detail = QStringLiteral("已下载 12.4 MB / 30.1 MB");
    download.percent = 41;
    download.state = TaskCenter::State::Running;
    download.steps = {{QStringLiteral("解析依赖"), TaskCenter::StepState::Done, QString()},
                      {QStringLiteral("下载文件"), TaskCenter::StepState::Running, QStringLiteral("12.4 MB / 30.1 MB")},
                      {QStringLiteral("写入实例"), TaskCenter::StepState::Pending, QString()}};
    download.capabilities.cancel = true;
    download.capabilities.pause = true;
    items.append(download);
    TaskCenter::Item install;
    install.key = QStringLiteral("2");
    install.title = QStringLiteral("安装 1.21.4-Fabric");
    install.detail = QStringLiteral("26 个文件");
    install.percent = 100;
    install.state = TaskCenter::State::Succeeded;
    install.finishedAt = QDateTime::currentDateTime().addSecs(-120);
    install.steps = {{QStringLiteral("下载游戏文件"), TaskCenter::StepState::Done, QString()},
                     {QStringLiteral("安装加载器"), TaskCenter::StepState::Done, QString()}};
    install.capabilities.openTarget = true;
    items.append(install);
    TaskCenter::Item failed;
    failed.key = QStringLiteral("3");
    failed.title = QStringLiteral("更新 Iris Shaders");
    failed.detail = QStringLiteral("连接超时");
    failed.state = TaskCenter::State::Failed;
    failed.errorSummary = QStringLiteral("连接超时（已重试 2 次）");
    failed.finishedAt = QDateTime::currentDateTime().addSecs(-300);
    failed.capabilities.retry = true;
    failed.capabilities.openLog = true;
    items.append(failed);
    return items;
}

/// 把控件"现在长什么样"抓下来（逻辑像素，DPR=1）。
bool saveShot(QApplication &app, QMainWindow &window, const QString &outDir, const QString &name,
              int *saved) {
    app.processEvents();
    app.processEvents();
    if (qEnvironmentVariableIsSet("WTFML_SCAN_INPUTS")) {
        for (const InputProbe &p : scanInputs(&window))
            std::printf("  INPUT [%s] %s %s '%s' w=%d max=%d kind=%s\n", qPrintable(name), qPrintable(p.cls),
                        qPrintable(p.name), qPrintable(p.hint.left(24)), p.width, p.maxWidth, qPrintable(p.kind));
    }
    const QString file = QDir(outDir).filePath(name + QStringLiteral(".png"));
    if (window.grab().save(file)) {
        ++(*saved);
        std::printf("  %s  %dx%d\n", qPrintable(name), window.width(), window.height());
        return true;
    }
    std::printf("  FAIL %s -> %s\n", qPrintable(name), qPrintable(file));
    return false;
}

/// `--loading`：加载态（ui/LoadingState）在真界面上的样子。动效开着、等淡入走完再拍。
/// 文件名 after-<场景>-<主题>.png。
/// `--files`：实例设置·模组页，60 个模组（每 5 个禁一个），平铺 / 悬停 + 选中 / 悬停在禁用行上。
/// 悬停用合成事件：行控件版发 Enter 给那一行，代理版发 MouseMove 给视口，两版都吃。
int shootFiles(QApplication &app, QMainWindow &window, MainWindowUI &ui, const ShotOptions &options) {
    int saved = 0;
    QTemporaryDir instanceRoot(QDir::current().filePath(QStringLiteral(".review-files-XXXXXX")));
    if (!instanceRoot.isValid()) return 2;
    const QString root = instanceRoot.path();
    makeInstanceFixture(root);
    QDir(root + QStringLiteral("/mods")).removeRecursively();
    QDir().mkpath(root + QStringLiteral("/mods"));
    for (int i = 0; i < (options.firstDisable ? 40 : 60); ++i) {
        QFile file(root + QStringLiteral("/mods/mod-%1-1.%2.%3.jar%4")
                              .arg(QChar(QLatin1Char('a' + i % 26)))
                              .arg(i)
                              .arg(i % 7)
                              .arg(i % 5 == 1 && !options.firstDisable ? QStringLiteral(".disabled") : QString()));
        if (file.open(QIODevice::WriteOnly)) file.write(QByteArray(4096 + i * 1531, 'x'));
    }
    using Dlg = InstanceSettingsDialog;
    Dlg::GlobalValues global;
    global.javaPath = QStringLiteral("C:/Program Files/Java/jdk-21/bin/java.exe");
    Dlg dialog(QStringLiteral("1.20.1 整合包"), QStringLiteral("1.20.1-Forge"), Dlg::Override(), global,
               QStringList(), false);
    QHash<QString, QString> dirs;
    dirs.insert(QStringLiteral("mod"), root + QStringLiteral("/mods"));
    dirs.insert(QStringLiteral("resourcepack"), root + QStringLiteral("/resourcepacks"));
    dirs.insert(QStringLiteral("save"), root + QStringLiteral("/saves"));
    dialog.setInstanceDir(root);
    dialog.setFileDirs(dirs);
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
    app.processEvents();
    if (!ui.overlay()->showPage(&dialog, QStringLiteral("实例设置"), MainWindowUI::instancesTabIndex())) {
        std::printf("  SKIP files：覆盖层没开出来\n");
        return 1;
    }
    app.processEvents();
    app.processEvents();
    dialog.showPage(QStringLiteral("mod"));
    for (int i = 0; i < 4; ++i) app.processEvents();
    InstanceFilesPage *page = dialog.filesPage(QStringLiteral("mod"));
    QListWidget *list = page != nullptr ? page->list() : nullptr;
    if (list == nullptr) return 1;
    if (options.firstDisable) {
        const QString width = QString::number(window.width());
        const auto report = [&](const char *when) {
            const auto *tools = page->findChild<QWidget *>(QStringLiteral("modSelectionTools"));
            std::printf("  %s: list viewport y=%d, selection row y=%d\n", when,
                        list->viewport()->mapTo(&window, QPoint()).y(),
                        tools != nullptr ? tools->mapTo(&window, QPoint()).y() : -1);
        };
        report("before");
        saveShot(app, window, options.out, QStringLiteral("first-disable-before-") + width, &saved);
        const QString first = list->item(0)->data(InstanceFilesPage::pathRole()).toString();
        QFile::rename(first, first + QStringLiteral(".disabled"));
        page->refresh();
        for (int i = 0; i < 4; ++i) app.processEvents();
        report("after");
        saveShot(app, window, options.out, QStringLiteral("first-disable-after-") + width, &saved);
        ui.overlay()->closeOverlay();
        app.processEvents();
        return saved == 2 ? 0 : 1;
    }
    const QString tag = QStringLiteral("r139-") + options.theme + QStringLiteral("-");
    saveShot(app, window, options.out, tag + QStringLiteral("mods"), &saved);
    const auto hoverRow = [&](int row) {
        QWidget *viewport = list->viewport();
        // 先把上一行的悬停撤掉（行控件版：每一行各收一发 Leave；代理版：视口收一发 Leave）。
        QEvent leave(QEvent::Leave);
        for (QObject *child : viewport->children())
            if (child->isWidgetType()) QApplication::sendEvent(child, &leave);
        QApplication::sendEvent(viewport, &leave);
        const QPoint pos = list->visualItemRect(list->item(row)).center() + QPoint(-40, 0);
        QWidget *w = viewport->childAt(pos);
        while (w != nullptr && w->parentWidget() != viewport) w = w->parentWidget();
        const QPoint global = viewport->mapToGlobal(pos);
        if (w != nullptr) {
            QEnterEvent enter(w->mapFrom(viewport, pos), w->mapFrom(viewport, pos), global);
            QApplication::sendEvent(w, &enter);
        }
        QMouseEvent move(QEvent::MouseMove, pos, pos, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        viewport->update();
    };
    list->item(2)->setSelected(true);
    hoverRow(0);
    saveShot(app, window, options.out, tag + QStringLiteral("mods-hover-selected"), &saved);
    list->item(2)->setSelected(false);
    hoverRow(3);  // mod-b-1.1.1.jar.disabled
    saveShot(app, window, options.out, tag + QStringLiteral("mods-disabled-hover"), &saved);
    ui.overlay()->closeOverlay();
    app.processEvents();
    std::printf("\nFiles shots: %d saved -> %s\n", saved, qPrintable(options.out));
    return saved == 3 ? 0 : 1;
}

/// `--r151`：步骤 151 走查。资源包页（紧凑页头 + 批量）查更新前 / 冒出「可更新」后各一张，
/// 服务器列表（自家代理的第一列 + 右对齐的状态列）一张，预设弹窗提问 ⓘ / 拼图各一张，主页一张。
int shootR151(QApplication &app, QMainWindow &window, MainWindowUI &ui, const ShotOptions &options) {
    int saved = 0;
    const QString tag = QStringLiteral("r151-%1-%2-").arg(options.theme).arg(window.width());
    const auto pump = [&app] { for (int i = 0; i < 4; ++i) app.processEvents(); };
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    pump();
    saveShot(app, window, options.out, tag + QStringLiteral("home"), &saved);
    if (!options.background.isEmpty()) return saved == 1 ? 0 : 1;  // 亮背景图那一档只看主页

    QTemporaryDir instanceRoot(QDir::current().filePath(QStringLiteral(".review-r151-XXXXXX")));
    if (!instanceRoot.isValid()) return 2;
    const QString root = instanceRoot.path();
    makeInstanceFixture(root);
    QDir().mkpath(root + QStringLiteral("/resourcepacks"));
    for (int i = 0; i < 12; ++i) {
        QFile file(root + QStringLiteral("/resourcepacks/pack-%1.zip").arg(QChar(QLatin1Char('a' + i))));
        if (file.open(QIODevice::WriteOnly)) file.write(QByteArray(2048 + i * 977, 'x'));
    }
    using Dlg = InstanceSettingsDialog;
    Dlg dialog(QStringLiteral("1.20.1 整合包"), QStringLiteral("1.20.1-Forge"), Dlg::Override(), Dlg::GlobalValues(),
               QStringList(), false);
    QHash<QString, QString> dirs;
    dirs.insert(QStringLiteral("mod"), root + QStringLiteral("/mods"));
    dirs.insert(QStringLiteral("resourcepack"), root + QStringLiteral("/resourcepacks"));
    dirs.insert(QStringLiteral("save"), root + QStringLiteral("/saves"));
    dialog.setInstanceDir(root);
    dialog.setFileDirs(dirs);
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
    pump();
    if (!ui.overlay()->showPage(&dialog, QStringLiteral("实例设置"), MainWindowUI::instancesTabIndex())) return 1;
    pump();
    dialog.showPage(QStringLiteral("resourcepack"));
    pump();
    if (InstanceFilesPage *page = dialog.filesPage(QStringLiteral("resourcepack")); page != nullptr && page->list()) {
        QListWidget *list = page->list();
        std::printf("  resourcepack before check: list viewport y=%d\n", list->viewport()->mapTo(&window, QPoint()).y());
        saveShot(app, window, options.out, tag + QStringLiteral("rp-before-check"), &saved);
        page->setUpdateResult({list->item(0)->data(InstanceFilesPage::pathRole()).toString()}, true);
        pump();
        std::printf("  resourcepack update found: list viewport y=%d\n", list->viewport()->mapTo(&window, QPoint()).y());
        saveShot(app, window, options.out, tag + QStringLiteral("rp-update-found"), &saved);
    }
    dialog.showPage(QStringLiteral("server"));
    dialog.setServerList({{QStringLiteral("甲乙服"), QStringLiteral("mc.example.org")},
                          {QStringLiteral("周末建筑"), QStringLiteral("build.example.net:25570")},
                          {QStringLiteral("老服"), QStringLiteral("old.example.com")}},
                         QString());
    Dlg::ServerRowStatus up;
    up.tested = up.online = true;
    up.latencyMs = 42;
    up.playersOnline = 7;
    up.playersMax = 20;
    up.motd = QStringLiteral("欢迎回来");
    Dlg::ServerRowStatus slow = up;
    slow.latencyMs = 318;
    slow.playersOnline = 0;
    Dlg::ServerRowStatus down;
    down.tested = true;
    down.timedOut = true;
    down.error = QStringLiteral("超时（网络不通）");
    dialog.setServerStatuses({up, slow, down});
    pump();
    saveShot(app, window, options.out, tag + QStringLiteral("server-list"), &saved);
    ui.overlay()->closeOverlay();
    pump();

    // 预设弹窗：照 InstallPageController 那样开（提问底色 + 800 宽正文），左边是改前（不换图标）。
    for (const bool puzzle : {false, true}) {
        InlineMessageBox box(MessageKind::Question, MainWindowUI::installPresetPickerTitle(), QString(),
                             {MainWindowUI::installPresetConfirmText(), MainWindowUI::installPresetCancelText()}, 0, -1,
                             &window);
        auto *picker = new PresetInstallPicker;
        picker->setContext(QStringLiteral("1.21.4"), QStringLiteral("Fabric"));
        PresetInstallPicker::Category game;
        game.key = QStringLiteral("gameSettings");
        game.title = QStringLiteral("游戏设置");
        game.options.append({QStringLiteral("daily-game"), QStringLiteral("日常配置"), true, true, 12});
        PresetInstallPicker::Category mods;
        mods.key = QStringLiteral("mods");
        mods.title = QStringLiteral("模组");
        mods.options.append({QStringLiteral("daily"), QStringLiteral("日常配置"), true, true, 42});
        mods.options.append({QStringLiteral("build"), QStringLiteral("建筑辅助"), true, false, 18});
        picker->setCategories({game, mods});
        picker->setComposites({});
        box.setContent(picker, 800);
        if (puzzle) box.setHeaderIcon(QStringLiteral("puzzle"));
        const QString name = tag + (puzzle ? QStringLiteral("preset-puzzle") : QStringLiteral("preset-question-i"));
        QTimer::singleShot(150, &box, [&] {
            saveShot(app, window, options.out, name, &saved);
            box.done(1);
        });
        ui.overlay()->execPrompt(&box);
        pump();
    }
    std::printf("\nr151 shots: %d saved -> %s\n", saved, qPrintable(options.out));
    return saved == 6 ? 0 : 1;
}

/// `--r155`：默认窗口主页收起 / 展开（各带 W 的悬停提示）；最小窗口主页（带「窗口加宽后才能展开」提示）、
/// 工具箱目录与每件工具、设置四个页签顶上；设置·外观「窗口大小」一行可点 / 置灰各一张。
/// 屏幕要比默认窗口大（offscreen 默认屏 800×600 会把「默认大小」夹小）：配 offscreen 的 configfile 给块大屏。
bool savePopupShot(QApplication &app, QMainWindow &window, const QString &outDir, const QString &name, int *saved);

int shootR155(QApplication &app, QMainWindow &window, MainWindowUI &ui, const ShotOptions &options) {
    using namespace MainWindowPageLayout;
    Popups::install(app);
    int saved = 0;
    const auto pump = [&app] { for (int i = 0; i < 4; ++i) app.processEvents(); };
    const auto name = [&](const QString &what) {
        return QStringLiteral("r155-%1-%2x%3-%4").arg(options.theme).arg(window.width()).arg(window.height()).arg(what);
    };
    const auto shot = [&](const QString &what) {
        pump();
        saveShot(app, window, options.out, name(what), &saved);
    };
    TitleBar *bar = ui.titleBar();
    QAbstractButton *logo = bar->logoButton();
    const auto shotLogoTip = [&](const QString &what) {
        const QPoint at = logo->rect().center();
        QHelpEvent help(QEvent::ToolTip, at, logo->mapToGlobal(at));
        QApplication::sendEvent(logo, &help);
        std::printf("  W tooltip「%s」\n", qPrintable(Popups::tooltipText()));
        savePopupShot(app, window, options.out, name(what), &saved);
        Popups::hideTooltip();
        pump();
    };

    fillHomeSamples(ui);
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    window.resize(kMainWindowDefaultWidth, kMainWindowDefaultHeight);
    pump();
    std::printf("  启动时导航 %s\n", bar->isCollapsed() ? "收起" : "展开");
    shot(QStringLiteral("home-nav-collapsed"));
    shotLogoTip(QStringLiteral("home-nav-collapsed-w-tooltip"));
    logo->click();
    shot(QStringLiteral("home-nav-expanded"));
    shotLogoTip(QStringLiteral("home-nav-expanded-w-tooltip"));
    logo->click();
    // 窗口激活时焦点自动落到 W：不画框；用 Tab 选中才画。
    window.activateWindow();
    pump();
    std::printf("  激活后 W %s焦点、%s框\n", logo->hasFocus() ? "有" : "没有",
                static_cast<TitleBarButton *>(logo)->hasKeyboardFocus() ? "画" : "不画");
    logo->clearFocus();
    logo->setFocus(Qt::TabFocusReason);
    sendTabKey(logo);  // W 只认真按键后的 Tab 焦点
    shot(QStringLiteral("home-nav-collapsed-w-tab-focus"));
    logo->clearFocus();

    window.resize(kMainWindowMinWidth, kMainWindowMinHeight);
    pump();
    shot(QStringLiteral("home"));
    shotLogoTip(QStringLiteral("home-narrow-w-tooltip"));

    ui.openToolboxTab();
    QWidget *host = ui.toolboxTab();
    if (host != nullptr && host->layout() != nullptr) {
        auto *toolbox = new ToolboxPage(host);
        host->layout()->addWidget(toolbox);
        wireToolboxSamples(*toolbox);
        toolbox->showCatalog();
        shot(QStringLiteral("toolbox-catalog"));
        for (ToolboxPage::Tool tool : ToolboxPage::tools()) {
            toolbox->showTool(tool);
            shot(QStringLiteral("toolbox-%1").arg(ToolboxPage::toolKey(tool)));
        }
    }

    ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
    fillSettingsSamples(ui);
    const QStringList keys = MainWindowUI::settingsCategoryKeys();
    const auto scrollOf = [&](int index) {
        return qobject_cast<QScrollArea *>(ui.getSettingsCategoryStack()->widget(index));
    };
    for (int i = 0; i < keys.size(); ++i) {
        ui.getSettingsTabs()->setCurrentIndex(i);
        if (QScrollArea *scroll = scrollOf(i)) scroll->verticalScrollBar()->setValue(0);
        shot(QStringLiteral("settings-%1").arg(keys.at(i)));
    }
    QPushButton *reset = ui.getWindowSizeResetButton();
    const int look = keys.indexOf(QStringLiteral("look"));
    const auto shotSizeRow = [&](const QString &state) {
        ui.getSettingsTabs()->setCurrentIndex(look);
        pump();
        if (QScrollArea *scroll = scrollOf(look); scroll != nullptr && scroll->widget() != nullptr && reset != nullptr)
            scroll->ensureWidgetVisible(reset, 0, scroll->viewport()->height() / 3);
        std::printf("  恢复默认 %s（窗 %dx%d）\n", reset != nullptr && reset->isEnabled() ? "可点" : "置灰", window.width(),
                    window.height());
        shot(QStringLiteral("settings-window-size-%1").arg(state));
    };
    shotSizeRow(QStringLiteral("enabled"));
    MainWindowUI::resetWindowSize(&window);
    shotSizeRow(QStringLiteral("disabled"));
    std::printf("\nr155 shots: %d saved -> %s\n", saved, qPrintable(options.out));
    return saved > 0 ? 0 : 1;
}

/// `--r157`：本机默认窗（960×620）主页，底栏有一件没看过的失败：收起（角标）一张、进度环连角标放大 8 倍一张，
/// 再在 1024×620 展开导航拍一张对照（标题前红点、无角标）。
int shootR157(QApplication &app, QMainWindow &window, MainWindowUI &ui, const ShotOptions &options) {
    int saved = 0;
    const auto pump = [&app] { for (int i = 0; i < 4; ++i) app.processEvents(); };
    const auto name = [&](const QString &what) {
        return QStringLiteral("r157-%1-%2x%3-%4").arg(options.theme).arg(window.width()).arg(window.height()).arg(what);
    };
    fillHomeSamples(ui);
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    window.resize(960, 620);
    MainWindowUI::BottomBarTaskSummary failed;
    failed.hasTask = true;
    failed.state = TaskCenter::State::Failed;
    failed.unreadFailure = true;
    failed.percent = 62;
    failed.title = QStringLiteral("下载游戏文件");
    ui.setBottomBarTaskSummary(failed);
    ui.titleBar()->setCollapsed(true);
    pump();
    saveShot(app, window, options.out, name(QStringLiteral("home-nav-collapsed-unread-failure")), &saved);
    if (QProgressBar *ring = ui.getProgressBar()) {
        const QRect crop(ring->mapTo(&window, QPoint(0, 0)) - QPoint(10, 10), ring->size() + QSize(20, 20));
        const QPixmap zoom = window.grab(crop).scaled(crop.size() * 8, Qt::KeepAspectRatio, Qt::FastTransformation);
        const QString file = QDir(options.out).filePath(name(QStringLiteral("badge-zoom")) + QStringLiteral(".png"));
        if (zoom.save(file)) ++saved;
    }
    // 960 宽窗在 1024 以下，导航是被窗宽强制收起的：展开对照得在 1024 宽拍。
    window.resize(1024, 620);
    ui.titleBar()->setCollapsed(false);
    pump();
    std::printf("  展开对照：导航 %s，角标 %s\n", ui.titleBar()->isCollapsed() ? "收起" : "展开",
                window.findChild<QLabel *>(MainWindowUI::taskSummaryBadgeName())->isVisible() ? "露" : "不露");
    saveShot(app, window, options.out, name(QStringLiteral("home-nav-expanded-unread-failure")), &saved);
    std::printf("\nr157 shots: %d saved -> %s\n", saved, qPrintable(options.out));
    return saved == 3 ? 0 : 1;
}

/// `--i85`：默认窗口（1024×620）主页，轨底的运行状态入口。图标轨一个进程 / 图标轨三个进程加面板 /
/// 展开轨对照；每张另存一张轨底放大 3 倍的局部。
int shootI85(QApplication &app, QMainWindow &window, MainWindowUI &ui, const ShotOptions &options) {
    int saved = 0;
    const auto pump = [&app] { for (int i = 0; i < 4; ++i) app.processEvents(); };
    const auto name = [&](const QString &what) {
        return QStringLiteral("i85-%1-%2x%3-%4").arg(options.theme).arg(window.width()).arg(window.height()).arg(what);
    };
    fillHomeSamples(ui);
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    window.resize(MainWindowPageLayout::kMainWindowDefaultWidth, MainWindowPageLayout::kMainWindowDefaultHeight);

    // 演示进程：同一实例多开两份 + 一个服务端（和运行面板同一份行结构）。
    const QColor ink = Theme::tokens().text;
    const int iconSize = MainWindowUI::bottomBarPanelActionIconSize();
    const auto row = [&](const QString &key, const QString &title, const QString &subtitle, const QString &block) {
        MainWindowUI::PanelRow r;
        r.key = key;
        r.icon = VersionIcons::isoBlock(VersionIcons::isoBlockKindFor(block), MainWindowUI::instanceTreeIconSize() * 2);
        r.title = title;
        r.subtitle = subtitle;
        MainWindowUI::PanelAction stop, settings, log;
        stop.icon = VersionIcons::power(iconSize, ink);
        stop.tooltip = QStringLiteral("结束");
        settings.icon = VersionIcons::gear(iconSize, Theme::tokens().primary);
        settings.tooltip = QStringLiteral("设置");
        log.icon = VersionIcons::line(QStringLiteral("log"), iconSize, ink);
        log.tooltip = QStringLiteral("日志");
        r.actions = {stop, settings, log};
        return r;
    };
    const QList<MainWindowUI::PanelRow> allRows{
        row(QStringLiteral("3"), QStringLiteral("1.20.1-Forge 47.2.0"), QStringLiteral("本次 4 分钟 · 累计 3 小时 12 分钟"), QStringLiteral("1.20.1-Forge")),
        row(QStringLiteral("2"), QStringLiteral("1.20.1-Forge 47.2.0"), QStringLiteral("本次 21 分钟 · 累计 3 小时 12 分钟"), QStringLiteral("1.20.1-Forge")),
        row(QStringLiteral("server:1"), QStringLiteral("生存服务端"), QStringLiteral("服务端 · 1 小时 5 分钟"), QStringLiteral("1.20.4"))};
    int shown = 1;
    ui.setBottomBarPanelContent([&](bool account) { return account ? QList<MainWindowUI::PanelRow>() : allRows.mid(0, shown); },
                                [](bool, const QString &, int) {});
    const auto setRunning = [&](int count) {
        shown = count;
        ui.setRunPanelAvailable(count > 0);
        ui.setRunStateCount(count);
        ui.getRunStateLabel()->setText(count > 0 ? MainWindowUI::runStateRunningCountText(count)
                                                 : MainWindowUI::runStateNotRunningText());
        ui.getRunStateLabel()->setVisible(true);
        ui.getRunStateMarkLabel()->setPixmap(
            MainWindowUI::runStateMarkPixmap(count > 0 ? MainWindowUI::RunStateMark::Running
                                                       : MainWindowUI::RunStateMark::Idle,
                                             ui.getBottomBar()->palette()));
        ui.getRunStateMarkLabel()->setVisible(true);
        pump();
    };
    const auto shotRail = [&](const QString &what) {
        saveShot(app, window, options.out, name(what), &saved);
        const QRect crop = ui.getBottomBar()->geometry().translated(ui.getBottomBar()->parentWidget()->mapTo(&window, QPoint(0, 0)));
        const QPixmap zoom = window.grab(crop.adjusted(-6, -6, 6, 6)).scaled(
            (crop.size() + QSize(12, 12)) * 3, Qt::KeepAspectRatio, Qt::FastTransformation);
        if (zoom.save(QDir(options.out).filePath(name(what) + QStringLiteral("-rail.png")))) ++saved;
    };

    ui.titleBar()->setCollapsed(true);
    setRunning(1);
    shotRail(QStringLiteral("rail-one"));

    setRunning(3);
    ui.getRunStateButton()->click();
    pump();
    savePopupShot(app, window, options.out, name(QStringLiteral("rail-three-panel")), &saved);
    for (QWidget *top : QApplication::topLevelWidgets()) {
        if (top->windowType() == Qt::Popup && top->isVisible()) top->close();
    }
    pump();

    ui.titleBar()->setCollapsed(false);
    pump();
    shotRail(QStringLiteral("expanded-three"));
    std::printf("\ni85 shots: %d saved -> %s\n", saved, qPrintable(options.out));
    return saved >= 6 ? 0 : 1;
}

QList<TaskCenter::Item> siteTaskItems();

/// `--r70kbd`：实例页用键盘（方向键）选中一行：当前行行尾按钮露出、当前行一圈焦点标记；
/// 前面先点一下鼠标再切回键盘，再拍一张鼠标时的对照（行尾按钮和标记都不在）。
int shootR70Keyboard(QApplication &app, QMainWindow &window, MainWindowUI &ui, const ShotOptions &options) {
    int saved = 0;
    const auto pump = [&app] { for (int i = 0; i < 4; ++i) app.processEvents(); };
    const auto name = [&](const QString &what) {
        return QStringLiteral("r70kbd-%1-%2x%3-%4").arg(options.theme).arg(window.width()).arg(window.height()).arg(what);
    };
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
    fillInstanceTree(ui);
    ui.setInstanceMode(MainWindowUI::InstanceMode::Client);
    QTreeWidget *tree = ui.getInstanceList();
    pump();
    window.activateWindow();
    tree->setFocus(Qt::OtherFocusReason);
    pump();
    // 鼠标对照：点一下当前行（按下事件送进视口），不露按钮、没有标记。
    if (tree->currentItem() != nullptr) {
        const QPoint at = tree->visualItemRect(tree->currentItem()).center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), QPointF(tree->viewport()->mapToGlobal(at)), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &press);
        pump();
    }
    pump();
    saveShot(app, window, options.out, name(QStringLiteral("mouse")), &saved);
    // 键盘：向下选一行（真按键，Theme 的焦点过滤器据此记「键盘在用」）。
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(tree, &down);
    pump();
    std::printf("  键盘焦点属性 %d，当前行 %s\n", tree->property("wtfmlKeyboardFocus").toBool() ? 1 : 0,
                tree->currentItem() != nullptr ? qPrintable(tree->currentItem()->text(0)) : "无");
    saveShot(app, window, options.out, name(QStringLiteral("keyboard")), &saved);
    std::printf("\nr70kbd shots: %d saved -> %s\n", saved, qPrintable(options.out));
    return saved > 0 ? 0 : 1;
}

/// `--r179c`：步骤 179 UI 问题 C（#5 #6 #7 #24 #16），自己换尺寸，不看 `--size`；
/// 文件名 r179c-<主题>-<宽>x<高>-<页面>.png。实例设置每一页顺带打印露着的滚动区里横向 / 纵向可滚多少（#6 判据）。
int shootR179c(QApplication &app, QMainWindow &window, MainWindowUI &ui, const ShotOptions &options) {
    int saved = 0;
    const auto pump = [&app] { for (int i = 0; i < 4; ++i) app.processEvents(); };
    const auto name = [&](const QString &what) {
        return QStringLiteral("r179c-%1-%2x%3-%4").arg(options.theme).arg(window.width()).arg(window.height()).arg(what);
    };
    const auto shot = [&](const QString &what) {
        pump();
        saveShot(app, window, options.out, name(what), &saved);
    };

    // ---- #5：设置页 / 工具箱 / 智能体设置，默认窗与拉宽 ----
    fillSettingsSamples(ui);
    ui.openToolboxTab();
    ToolboxPage *toolbox = nullptr;
    if (QWidget *host = ui.toolboxTab(); host != nullptr && host->layout() != nullptr) {
        toolbox = new ToolboxPage(host);
        host->layout()->addWidget(toolbox);
        wireToolboxSamples(*toolbox);
    }
    for (const QSize size : {QSize(960, 620), QSize(1600, 800)}) {
        window.resize(size);
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
        ui.getSettingsTabs()->setCurrentIndex(0);
        shot(QStringLiteral("settings-launch"));
        if (toolbox != nullptr) {
            ui.openToolboxTab();
            toolbox->showCatalog();
            shot(QStringLiteral("toolbox-catalog"));
            toolbox->showTool(ToolboxPage::tools().value(0));
            shot(QStringLiteral("toolbox-tool"));
        }
        AgentSettingsPage agentSettings;
        AgentView::ProviderDraft draft;
        draft.id = QStringLiteral("default");
        draft.name = QStringLiteral("DeepSeek");
        draft.baseUrl = QStringLiteral("https://api.deepseek.com/v1");
        draft.model = QStringLiteral("deepseek-chat");
        agentSettings.setProfile(draft, true, QStringLiteral("credential-manager"));
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::agentTabIndex());
        pump();
        if (ui.overlay()->showPage(&agentSettings, QStringLiteral("API 与模型设置"), MainWindowUI::agentTabIndex())) {
            shot(QStringLiteral("agent-settings"));
            ui.overlay()->closeOverlay();
            pump();
        }
    }

    // ---- #6 #7：实例设置每一页，最小窗与默认窗 ----
    {
        QTemporaryDir instanceRoot(QDir::current().filePath(QStringLiteral(".review-r179c-XXXXXX")));
        if (!instanceRoot.isValid()) return 2;
        const QString root = instanceRoot.path();
        makeInstanceFixture(root);
        using Dlg = InstanceSettingsDialog;
        Dlg::GlobalValues global;
        global.javaPath = QStringLiteral("C:/Program Files/Java/jdk-21/bin/java.exe");
        global.xmx = 4096;
        global.xms = 1024;
        global.autoMemory = false;
        Dlg dialog(QStringLiteral("Hyperlands 26.2"), QStringLiteral("Hyperlands-26.2"), Dlg::Override(), global,
                   QStringList(), false);
        QHash<QString, QString> dirs;
        for (const auto &[key, sub] : {std::pair{"mod", "mods"}, std::pair{"resourcepack", "resourcepacks"},
                                       std::pair{"datapack", "datapacks"}, std::pair{"shader", "shaderpacks"},
                                       std::pair{"save", "saves"}, std::pair{"logs", "logs"},
                                       std::pair{"screenshots", "screenshots"}, std::pair{"game", "game"}})
            dirs.insert(QString::fromLatin1(key), root + QLatin1Char('/') + QString::fromLatin1(sub));
        dialog.setInstanceDir(root);
        dialog.setFileDirs(dirs);
        dialog.setLastLaunch(QDateTime::currentDateTime().addSecs(-3600 * 26));
        dialog.setIcon(VersionIcons::iconCandidateKeys().value(0), VersionIcons::grassBlock(48));
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
        if (ui.overlay()->showPage(&dialog, QStringLiteral("实例设置"), MainWindowUI::instancesTabIndex())) {
            const QStringList keys = Dlg::pinnedPageKeys() + Dlg::filePageKeys() + Dlg::lifecyclePageKeys();
            int overflowPages = 0;
            for (const QSize size : {QSize(900, 590), QSize(1024, 620)}) {
                window.resize(size);
                for (const QString &key : keys) {
                    dialog.showPage(key);
                    pump();
                    if (dialog.currentPageKey() != key) continue;  // 这个实例没有这一页
                    QStringList horizontal;
                    QStringList vertical;
                    for (QAbstractScrollArea *area : dialog.findChildren<QAbstractScrollArea *>()) {
                        if (!area->isVisible()) continue;
                        const QString who = area->objectName().isEmpty()
                                                ? QString::fromLatin1(area->metaObject()->className())
                                                : area->objectName();
                        if (area->horizontalScrollBar()->maximum() > 0 && area->horizontalScrollBar()->isVisible())
                            horizontal << QStringLiteral("%1(+%2)").arg(who).arg(area->horizontalScrollBar()->maximum());
                        if (area->verticalScrollBar()->maximum() > 0)
                            vertical << QStringLiteral("%1(+%2)").arg(who).arg(area->verticalScrollBar()->maximum());
                    }
                    overflowPages += horizontal.isEmpty() ? 0 : 1;
                    std::printf("  %dx%d %-16s 横向 %s｜纵向 %s\n", size.width(), size.height(), qPrintable(key),
                                horizontal.isEmpty() ? "无" : qPrintable(horizontal.join(QLatin1Char(' '))),
                                vertical.isEmpty() ? "无" : qPrintable(vertical.join(QLatin1Char(' '))));
                    shot(QStringLiteral("inst-%1").arg(key));
                }
            }
            std::printf("  实例设置：出横向滚动条的页 %d 个\n", overflowPages);
            ui.overlay()->closeOverlay();
            pump();
        }
    }

    // ---- 任务中心（运行中的进程那一栏已去掉，统一看轨底入口） ----
    ui.setTaskCenterContent([] { return siteTaskItems(); }, [] {});
    for (const QSize size : {QSize(1600, 800), QSize(1024, 620)}) {
        window.resize(size);
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
        pump();
        if (QPushButton *summary = ui.getTaskSummaryButton()) summary->click();
        pump();
        ActivityPopover::openAllIn(&window);
        pump();
        if (QWidget *popover = window.findChild<QWidget *>(QStringLiteral("activityPopover"));
            popover != nullptr && popover->isVisible())
            popover->close();
        shot(QStringLiteral("tasks-center"));
        if (ui.overlay() != nullptr && ui.overlay()->isOpen()) ui.overlay()->dismiss();
        pump();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ---- #16：Tab 选中 W ----
    fillHomeSamples(ui);
    window.resize(1024, 620);
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    QAbstractButton *logo = ui.titleBar()->logoButton();
    logo->setFocus(Qt::TabFocusReason);
    sendTabKey(logo);
    shot(QStringLiteral("home-w-tab-focus"));
    for (const bool focused : {true, false}) {
        if (!focused) logo->clearFocus();
        pump();
        const QRect crop(logo->mapTo(&window, QPoint(0, 0)) - QPoint(12, 12), logo->size() + QSize(24, 24));
        const QPixmap zoom = window.grab(crop).scaled(crop.size() * 8, Qt::KeepAspectRatio, Qt::FastTransformation);
        const QString what = focused ? QStringLiteral("w-zoom-tab-focus") : QStringLiteral("w-zoom-no-focus");
        if (zoom.save(QDir(options.out).filePath(name(what) + QStringLiteral(".png")))) ++saved;
    }
    std::printf("\nr179c shots: %d saved -> %s\n", saved, qPrintable(options.out));
    return saved > 0 ? 0 : 1;
}

int shootLoading(QApplication &app, QMainWindow &window, MainWindowUI &ui, const ShotOptions &options) {
    Anim::setEnabled(true);
    int saved = 0;
    const auto settle = [&app](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
        app.processEvents();
    };
    // 拍在三颗点有明有暗的那一刻（第一颗刚过峰、第二颗快到峰），别碰巧拍到三颗都歇着。
    const auto toPhase = [&app] {
        while (LoadingState::clockMs() % Anim::loadingDotPeriodMs() < 430 ||
               LoadingState::clockMs() % Anim::loadingDotPeriodMs() > 470) {
            QEventLoop loop;
            QTimer::singleShot(4, &loop, &QEventLoop::quit);
            loop.exec();
        }
        app.processEvents();
    };
    const QString tag = options.theme;
    const QString modKey = QStringLiteral("mod");

    // ---- 下载·模组：整块加载（树清空，盖一层「正在获取最热门的模组…」）----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
    ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryModIndex());
    ui.setDownloadHeaderRow(MainWindowUI::downloadCategoryModIndex());
    ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryModIndex());
    QTreeWidget *modTree = ui.getDownloadResourceTree(modKey);
    if (modTree != nullptr) {
        modTree->clear();
        LoadingState::begin(modTree, QStringLiteral("正在获取最热门的模组…"));
        settle(Anim::loadingDelayMs() + Anim::appearMs() + 120);
        toPhase();
        saveShot(app, window, options.out, QStringLiteral("after-block-%1").arg(tag), &saved);
        // ---- 整块出错 ----
        LoadingState::fail(modTree, QStringLiteral("没取到模组列表：连接 Modrinth 超时"), [] {});
        settle(Anim::overlayMs() + 120);
        saveShot(app, window, options.out, QStringLiteral("after-error-%1").arg(tag), &saved);
        LoadingState::finish(modTree);
        settle(Anim::exitMs() + 80);
    }

    // ---- 下载·模组：第三层行内加载（Iris Shaders → 1.21.4 展开在等）----
    fillResourceTree(ui, modKey);
    if (modTree != nullptr && modTree->topLevelItemCount() > 1 && modTree->topLevelItem(1)->childCount() > 0) {
        QTreeWidgetItem *game = modTree->topLevelItem(1)->child(0);
        LoadingState::beginRow(game, QStringLiteral("正在获取…"));
        game->setExpanded(true);
        settle(Anim::loadingDelayMs() + Anim::appearMs() + 120);
        toPhase();
        saveShot(app, window, options.out, QStringLiteral("after-inline-%1").arg(tag), &saved);
    }

    // ---- 安装页加载器树：一条在等、一条没取到（带「重试」）----
    if (QTreeWidget *loaders = ui.getLoaderTree(); loaders != nullptr && ui.getInstallNameEdit() != nullptr) {
        ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryGameIndex());
        ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryGameIndex());
        for (QWidget *child = ui.getInstallNameEdit(); child != nullptr && child->parentWidget() != nullptr;
             child = child->parentWidget()) {
            if (auto *stack = qobject_cast<QStackedWidget *>(child->parentWidget())) stack->setCurrentWidget(child);
        }
        if (loaders->topLevelItemCount() > 2) {
            QTreeWidgetItem *waiting = loaders->topLevelItem(1);
            QTreeWidgetItem *failed = loaders->topLevelItem(2);
            qDeleteAll(waiting->takeChildren());
            qDeleteAll(failed->takeChildren());
            LoadingState::beginRow(waiting, MainWindowUI::loaderVersionFetchingText());
            for (const QString &text :
                 {MainWindowUI::loaderVersionLatestText(), MainWindowUI::loaderVersionStableText()})
                new QTreeWidgetItem(failed, {text});
            LoadingState::failRow(LoadingState::beginRow(failed), MainWindowUI::loaderVersionFailedText(), [] {});
            waiting->setExpanded(true);
            failed->setExpanded(true);
        }
        // 页面滚到底：出错那一行在最下面。
        for (QWidget *w = loaders->parentWidget(); w != nullptr; w = w->parentWidget()) {
            if (auto *area = qobject_cast<QScrollArea *>(w)) {
                area->widget()->adjustSize();
                app.processEvents();
                area->verticalScrollBar()->setValue(area->verticalScrollBar()->maximum());
                break;
            }
        }
        settle(Anim::loadingDelayMs() + Anim::appearMs() + 120);
        toPhase();
        saveShot(app, window, options.out, QStringLiteral("after-loader-%1").arg(tag), &saved);
    }

    // ---- 实例页「预设」那一档：点进一份，「预设内容」那一格在读 ----
    // 照产品的嵌法（PresetController::buildPage）：塞进 getInstancePresetPage() 那个宿主。
    if (QWidget *host = ui.getInstancePresetPage(); host != nullptr && host->layout() != nullptr) {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
        ui.setInstanceMode(MainWindowUI::InstanceMode::Preset);
        auto *page = new PresetManagePage(host);
        page->setWindowFlags(Qt::Widget);
        page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        host->layout()->addWidget(page);
        page->setCategories({{QString(), QStringLiteral("全部"), true},
                             {QStringLiteral("settings"), QStringLiteral("游戏设置"), false},
                             {QStringLiteral("mods"), QStringLiteral("模组"), false}});
        QList<PresetManagePage::PresetItem> presets;
        for (const auto &[id, name, category] :
             {std::tuple{QStringLiteral("p1"), QStringLiteral("生存常用"), QStringLiteral("settings")},
              std::tuple{QStringLiteral("p2"), QStringLiteral("光影与性能"), QStringLiteral("mods")},
              std::tuple{QStringLiteral("p3"), QStringLiteral("建筑按键"), QStringLiteral("settings")}}) {
            PresetManagePage::PresetItem item;
            item.id = id;
            item.name = name;
            item.categoryKey = category;
            item.categoryText = category == QStringLiteral("mods") ? QStringLiteral("模组") : QStringLiteral("游戏设置");
            item.sectionCount = 2;
            presets.append(item);
        }
        page->setPresets(presets, {}, QStringLiteral("p2"));
        page->show();
        // 点中那一份 = 进细节那一层（产品里那条 itemClicked 连接）。
        if (auto *list = page->findChild<QTreeWidget *>(PresetManagePage::presetListName());
            list != nullptr && list->currentItem() != nullptr)
            emit list->itemClicked(list->currentItem(), 0);
        app.processEvents();
        page->showPresetLoading(QStringLiteral("p2"));
        settle(Anim::loadingDelayMs() + Anim::appearMs() + 120);
        toPhase();
        saveShot(app, window, options.out, QStringLiteral("after-preset-%1").arg(tag), &saved);
        page->showPresetReadError(QStringLiteral("preset.json 不是合法的 JSON"));
        settle(Anim::overlayMs() + 120);
        saveShot(app, window, options.out, QStringLiteral("after-preset-error-%1").arg(tag), &saved);
        delete page;
    }
    Anim::setEnabled(false);
    std::printf("\nLoading shots: %d saved -> %s\n", saved, qPrintable(options.out));
    return saved > 0 ? 0 : 1;
}

int runExtended(QApplication &app, const ShotOptions &options) {
    Anim::setEnabled(false);
    QTemporaryDir settingsDir(QDir::current().filePath(QStringLiteral(".review-shots-XXXXXX")));
    if (!settingsDir.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("WTFMLReviewShots"));
    app.setApplicationName(QStringLiteral("ReviewShots"));
    app.setStyle(Theme::createStyle());
    installShotFonts(app);

    Theme::Spec spec = options.theme == QLatin1String("default") ? Theme::load() : Theme::presetSpec(options.theme);
    if (!options.background.isEmpty()) {
        Theme::PartStyle &page = spec.parts[int(Theme::Part::PageBackground)];
        page.hasImage = true;
        page.imagePath = options.background;
    }
    Theme::setSpec(spec);
    Theme::apply(app);

    QDir().mkpath(options.out);
    int saved = 0;
    const auto pump = [&app] {
        app.processEvents();
        app.processEvents();
    };

    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(options.size);
    window.show();
    pump();
    if (options.loading) return shootLoading(app, window, ui, options);
    if (options.files) return shootFiles(app, window, ui, options);
    if (options.r151) return shootR151(app, window, ui, options);
    if (options.r155) return shootR155(app, window, ui, options);
    if (options.r157) return shootR157(app, window, ui, options);
    if (options.i85) return shootI85(app, window, ui, options);
    if (options.r179c) return shootR179c(app, window, ui, options);
    if (options.r70kbd) return shootR70Keyboard(app, window, ui, options);

    // ---- 主页：零实例 / 有实例 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    MainWindowUI::HomeQuickItem none;
    none.key = QStringLiteral("none");
    none.name = QStringLiteral("还没有实例");
    none.empty = true;
    ui.setHomeQuickRows({none}, {}, {});
    saveShot(app, window, options.out, QStringLiteral("home-empty"), &saved);

    fillHomeSamples(ui);
    saveShot(app, window, options.out, QStringLiteral("home-instances"), &saved);
    ui.titleBar()->setCollapsed(true);
    saveShot(app, window, options.out, QStringLiteral("home-rail-collapsed"), &saved);
    ui.titleBar()->setCollapsed(false);

    // ---- 实例管理：空状态 / 列表 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
    if (auto *list = window.findChild<QTreeWidget *>(MainWindowUI::instanceListName())) list->hide();
    if (auto *empty = window.findChild<QWidget *>(MainWindowUI::instanceEmptyStateName())) empty->show();
    saveShot(app, window, options.out, QStringLiteral("manage-empty"), &saved);

    fillInstanceTree(ui);
    if (auto *list = window.findChild<QTreeWidget *>(MainWindowUI::instanceListName())) list->show();
    if (auto *empty = window.findChild<QWidget *>(MainWindowUI::instanceEmptyStateName())) empty->hide();
    saveShot(app, window, options.out, QStringLiteral("manage-list"), &saved);

    // ---- 实例二级设置：总览 / 模组（浮层里，对话框本身 820×560） ----
    {
        QTemporaryDir instanceRoot(QDir::current().filePath(QStringLiteral(".review-instance-XXXXXX")));
        if (!instanceRoot.isValid()) return 2;
        const QString root = instanceRoot.path();
        makeInstanceFixture(root);
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
        dirs.insert(QStringLiteral("mod"), root + QStringLiteral("/mods"));
        dirs.insert(QStringLiteral("resourcepack"), root + QStringLiteral("/resourcepacks"));
        dirs.insert(QStringLiteral("datapack"), root + QStringLiteral("/datapacks"));
        dirs.insert(QStringLiteral("shader"), root + QStringLiteral("/shaderpacks"));
        dirs.insert(QStringLiteral("save"), root + QStringLiteral("/saves"));
        dirs.insert(QStringLiteral("logs"), root + QStringLiteral("/logs"));
        dirs.insert(QStringLiteral("screenshots"), root + QStringLiteral("/screenshots"));
        dirs.insert(QStringLiteral("game"), root + QStringLiteral("/game"));
        dialog.setInstanceDir(root);
        dialog.setFileDirs(dirs);
        dialog.setLastLaunch(QDateTime::currentDateTime().addSecs(-3600 * 26));
        dialog.setIcon(VersionIcons::iconCandidateKeys().value(0), VersionIcons::grassBlock(48));
        if (ui.overlay()->showPage(&dialog, QStringLiteral("实例设置"), MainWindowUI::instancesTabIndex())) {
            dialog.showPage(Dlg::overviewCategoryKey());
            pump();
            std::printf("  instance dialog %dx%d at (%d,%d)\n", dialog.width(), dialog.height(),
                        dialog.mapTo(&window, QPoint(0, 0)).x(), dialog.mapTo(&window, QPoint(0, 0)).y());
            saveShot(app, window, options.out, QStringLiteral("instance-settings-overview"), &saved);
            dialog.showPage(QStringLiteral("mod"));
            saveShot(app, window, options.out, QStringLiteral("instance-settings-mods"), &saved);
            ui.overlay()->closeOverlay();
            pump();
        } else {
            std::printf("  SKIP instance-settings：覆盖层没开出来\n");
        }
    }

    // ---- 下载：游戏（正式版展开）/ 模组 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
    ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryGameIndex());
    ui.setDownloadHeaderRow(MainWindowUI::downloadCategoryGameIndex());
    ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryGameIndex());
    ui.getGameStack()->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
    fillVersionTree(ui);
    pump();
    saveShot(app, window, options.out, QStringLiteral("download-game-release"), &saved);

    ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryModIndex());
    ui.setDownloadHeaderRow(MainWindowUI::downloadCategoryModIndex());
    ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryModIndex());
    const auto fillFilter = [](QComboBox *combo, const QStringList &items) {
        if (combo == nullptr) return;
        QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItems(items);
        combo->setCurrentIndex(0);
    };
    const QString modKey = QStringLiteral("mod");
    fillFilter(ui.getDownloadFilterVersionCombo(modKey),
               {QStringLiteral("全部版本"), QStringLiteral("1.21.4"), QStringLiteral("1.20.1")});
    fillFilter(ui.getDownloadFilterLoaderCombo(modKey),
               {QStringLiteral("全部加载器"), QStringLiteral("Fabric"), QStringLiteral("Forge")});
    fillFilter(ui.getDownloadFilterSourceCombo(modKey),
               {QStringLiteral("Modrinth"), QStringLiteral("CurseForge")});
    fillFilter(ui.getDownloadFilterTypeCombo(modKey),
               {QStringLiteral("全部类型"), QStringLiteral("模组"), QStringLiteral("整合包")});
    if (auto *search = ui.getDownloadSearchEdit(modKey)) search->setText(QStringLiteral("sodium"));
    fillResourceTree(ui, modKey);
    pump();
    saveShot(app, window, options.out, QStringLiteral("download-mod"), &saved);

    // ---- 设置：启动 / 提示 / 个性化（顶、背景图）/ 下载 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
    fillSettingsSamples(ui);
    const QStringList settingsKeys = MainWindowUI::settingsCategoryKeys();
    QWidget *settingsPage = ui.getTabWidget()->widget(MainWindowUI::settingsTabIndex());
    const auto settingsScroll = [&](int index) -> QScrollArea * {
        return qobject_cast<QScrollArea *>(ui.getSettingsCategoryStack()->widget(index));
    };
    const auto shotSettings = [&](int index, const QString &name) {
        ui.getSettingsTabs()->setCurrentIndex(index);
        if (QScrollArea *scroll = settingsScroll(index)) scroll->verticalScrollBar()->setValue(0);
        saveShot(app, window, options.out, name, &saved);
    };
    shotSettings(settingsKeys.indexOf(QStringLiteral("launch")), QStringLiteral("settings-launch"));
    shotSettings(settingsKeys.indexOf(QStringLiteral("warn")), QStringLiteral("settings-warn"));
    const int lookIndex = settingsKeys.indexOf(QStringLiteral("look"));
    shotSettings(lookIndex, QStringLiteral("settings-look-top"));
    if (QScrollArea *scroll = settingsScroll(lookIndex)) {
        QLabel *anchor = nullptr;
        if (scroll->widget() != nullptr) {
            for (QLabel *label : scroll->widget()->findChildren<QLabel *>()) {
                if (label->text() == QStringLiteral("背景图")) {
                    anchor = label;
                    break;
                }
            }
        }
        if (anchor != nullptr) {
            const int y = anchor->mapTo(scroll->widget(), QPoint(0, 0)).y();
            scroll->verticalScrollBar()->setValue(qMax(0, y - 16));
            std::printf("  settings-look anchor y=%d scroll=%d/%d\n", y,
                        scroll->verticalScrollBar()->value(), scroll->verticalScrollBar()->maximum());
        } else {
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum() / 3);
        }
        saveShot(app, window, options.out, QStringLiteral("settings-look-background"), &saved);
    }
    shotSettings(settingsKeys.indexOf(QStringLiteral("download")), QStringLiteral("settings-download"));
    if (settingsPage != nullptr) settingsPage->update();

    // ---- 智能体：未配置 / 有会话 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::agentTabIndex());
    AgentPage *agent = ui.getAgentPage();
    if (agent != nullptr) {
        agent->setConfigured(false, QString());
        agent->showEmptyState(true);
        saveShot(app, window, options.out, QStringLiteral("agent-unconfigured"), &saved);

        agent->setConfigured(true, QStringLiteral("已连接 · OpenAI 兼容"));
        saveShot(app, window, options.out, QStringLiteral("agent-configured-empty"), &saved);
        agent->setModelLabel(QStringLiteral("gpt-4o"));
        agent->setEncryptionChecked(true);
        agent->setInstanceChip(QStringLiteral("1.20.1-Forge"), false);
        QList<AgentView::SessionMeta> sessions;
        AgentView::SessionMeta first;
        first.id = QStringLiteral("s1");
        first.title = QStringLiteral("查一下崩溃原因");
        first.updatedAt = QDateTime::currentDateTime().addSecs(-600);
        first.encrypted = true;
        sessions.append(first);
        AgentView::SessionMeta second;
        second.id = QStringLiteral("s2");
        second.title = QStringLiteral("帮我写一条启动参数");
        second.updatedAt = QDateTime::currentDateTime().addSecs(-86400);
        sessions.append(second);
        AgentView::SessionMeta third;
        third.id = QStringLiteral("s3");
        third.title = QStringLiteral("模组冲突排查");
        third.updatedAt = QDateTime::currentDateTime().addSecs(-86400 * 3);
        third.pinned = true;
        sessions.append(third);
        agent->setSessions(sessions);
        agent->setCurrentSession(QStringLiteral("s1"));
        agent->setTitle(QStringLiteral("查一下崩溃原因"));
        QList<AgentView::Message> messages;
        AgentView::Message ask;
        ask.id = QStringLiteral("m1");
        ask.user = true;
        ask.content = QStringLiteral("游戏一进世界就崩，日志里最后几行是 java.lang.NullPointerException。帮我看看。");
        messages.append(ask);
        AgentView::Message answer;
        answer.id = QStringLiteral("m2");
        answer.content = QStringLiteral(
            "日志里 NPE 出现在 `net.minecraft.client.renderer` 那一层，最常见的原因是某个渲染模组和当前版本不匹配。\n\n"
            "先按这个顺序试：\n1. 把 `sodium` 和 `iris` 一起停用，只留一个；\n"
            "2. 确认 Fabric API 的版本跟 1.20.1 对得上；\n3. 再启动一次，看 `latest.log` 里第一个 ERROR 出现在哪。");
        messages.append(answer);
        AgentView::Message toolMsg;
        toolMsg.id = QStringLiteral("m3");
        toolMsg.content = QStringLiteral("已读取实例日志：`logs/latest.log`（2.1 MB）。");
        messages.append(toolMsg);
        agent->setMessages(messages);
        AgentView::ToolCard card;
        card.id = QStringLiteral("t1");
        card.title = QStringLiteral("读取 logs/latest.log");
        card.summary = QStringLiteral("找到 1 处 ERROR、3 处 WARN");
        card.detail = QStringLiteral("ERROR 23:41:07 java.lang.NullPointerException");
        card.status = AgentView::ToolCardStatus::Done;
        agent->setToolCards({card});
        agent->showEmptyState(false);
        saveShot(app, window, options.out, QStringLiteral("agent-session"), &saved);

        // 就地改名：顶行标题点一下 / 列表双击，各拍一张编辑态，再按 Esc 收掉。
        const auto escape = [agent](const char *name) {
            if (QWidget *edit = agent->findChild<QWidget *>(QString::fromLatin1(name))) {
                QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(edit, &esc);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
        };
        if (auto *title = agent->findChild<QLabel *>(QStringLiteral("agentSessionTitle"))) {
            const QPointF at(4, title->height() / 2.0);
            QMouseEvent release(QEvent::MouseButtonRelease, at, title->mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QApplication::sendEvent(title, &release);
            saveShot(app, window, options.out, QStringLiteral("agent-title-rename"), &saved);
            escape("agentTitleRenameEdit");
        }
        QListWidget *list = agent->sessionList();
        for (int i = 0; list != nullptr && i < list->count(); ++i) {
            if (list->item(i)->text() == QStringLiteral("帮我写一条启动参数")) {
                emit list->itemDoubleClicked(list->item(i));
                saveShot(app, window, options.out, QStringLiteral("agent-list-rename"), &saved);
                escape("agentSessionRenameEdit");
                break;
            }
        }
    } else {
        std::printf("  SKIP agent：拿不到 AgentPage\n");
    }

    // ---- 智能体：API 与模型设置（覆盖层整页；顶部 / 滚到底） ----
    {
        AgentSettingsPage settingsPage;
        AgentView::ProviderDraft draft;
        draft.id = QStringLiteral("default");
        draft.name = QStringLiteral("DeepSeek");
        draft.baseUrl = QStringLiteral("https://api.deepseek.com/v1");
        draft.model = QStringLiteral("deepseek-chat");
        settingsPage.setProfile(draft, true, QStringLiteral("credential-manager"));
        if (ui.overlay()->showPage(&settingsPage, QStringLiteral("API 与模型设置"), MainWindowUI::agentTabIndex())) {
            pump();
            saveShot(app, window, options.out, QStringLiteral("agent-settings-top"), &saved);
            if (auto *scroll = settingsPage.findChild<QScrollArea *>()) {
                scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                pump();
            }
            saveShot(app, window, options.out, QStringLiteral("agent-settings-bottom"), &saved);
            if (auto *advanced = settingsPage.findChild<SettingsParts::ExpandSection *>()) {
                advanced->setExpanded(true, false);
                pump();
                if (auto *scroll = settingsPage.findChild<QScrollArea *>())
                    scroll->ensureWidgetVisible(advanced->body(), 0, 0);
                pump();
                saveShot(app, window, options.out, QStringLiteral("agent-settings-advanced"), &saved);
            }
            ui.overlay()->closeOverlay();
            pump();
        } else {
            std::printf("  SKIP agent-settings：覆盖层没开出来\n");
        }
    }

    // ---- 工具箱：目录页 + 十个工具页 ----
    ui.openToolboxTab();
    QWidget *host = ui.toolboxTab();
    ToolboxPage *toolbox = nullptr;
    if (host != nullptr && host->layout() != nullptr) {
        toolbox = new ToolboxPage(host);
        host->layout()->addWidget(toolbox);
        wireToolboxSamples(*toolbox);
        toolbox->showCatalog();
        pump();
        saveShot(app, window, options.out, QStringLiteral("toolbox-catalog"), &saved);
        for (ToolboxPage::Tool tool : ToolboxPage::tools()) {
            toolbox->showTool(tool);
            pump();
            saveShot(app, window, options.out,
                     QStringLiteral("toolbox-%1").arg(ToolboxPage::toolKey(tool)), &saved);
        }
    } else {
        std::printf("  SKIP toolbox：工具箱页挂不上\n");
    }

    // ---- 下载 → 游戏 → 安装页（版本名称 + 模组加载器卡片 + 底下的主按钮）----
    if (QLineEdit *installName = ui.getInstallNameEdit()) {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
        ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryGameIndex());
        ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryGameIndex());
        for (QWidget *child = installName; child != nullptr && child->parentWidget() != nullptr; child = child->parentWidget()) {
            if (auto *stack = qobject_cast<QStackedWidget *>(child->parentWidget())) stack->setCurrentWidget(child);
        }
        installName->show();
        pump();
        if (QPushButton *button = ui.getInstallButton())
            std::printf("  install button center x=%d (window %d)\n", button->mapTo(&window, button->rect().center()).x(), window.width());
        saveShot(app, window, options.out, QStringLiteral("download-install"), &saved);
    } else {
        std::printf("  SKIP download-install：拿不到安装页\n");
    }

    // ---- 下载 → 服务端 → 新建服务端：照 MainWindow::openNewServerPage() 嵌进表单宿主，八档照 ServerInstall::allFlavors() ----
    if (QWidget *formHost = ui.getServerFormHost(); formHost != nullptr && formHost->layout() != nullptr) {
        auto *page = new NewServerPage(formHost);
        page->setWindowFlags(Qt::Widget);
        page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        formHost->layout()->addWidget(page);
        QList<NewServerPage::FlavorOption> flavors;
        const auto flavor = [&](const char *token, const QString &name, bool loader, bool installer, bool build) {
            NewServerPage::FlavorOption option;
            option.token = QString::fromLatin1(token);
            option.name = name;
            option.needsLoader = loader;
            option.needsInstaller = installer;
            option.needsBuild = build;
            option.needsLoaderGame = true;
            flavors.append(option);
        };
        flavor("vanilla", QStringLiteral("原版"), false, false, false);
        flavor("forge", QStringLiteral("Forge"), true, false, false);
        flavor("neoforge", QStringLiteral("NeoForge"), true, false, false);
        flavor("fabric", QStringLiteral("Fabric"), true, true, false);
        flavor("quilt", QStringLiteral("Quilt"), false, true, false);
        flavor("paper", QStringLiteral("Paper"), false, false, true);
        flavor("purpur", QStringLiteral("Purpur"), false, false, true);
        flavor("spigot", QStringLiteral("Spigot"), false, false, false);
        page->setFlavors(flavors, QStringLiteral("vanilla"));
        page->setDefaults(QStringLiteral("Server1.21.4"), QStringLiteral("1.21.4"));
        ui.setServerFormIntro(QStringLiteral("将安装 1.21.4 服务端"));
        page->show();
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
        ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryServerIndex());
        ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryServerIndex());
        ui.getServerStack()->setCurrentIndex(MainWindowUI::downloadServerFormIndex());
        pump();
        {
            const auto yOf = [&](QWidget *w) { return w != nullptr ? w->mapTo(&window, QPoint(0, 0)).y() : -1; };
            QWidget *nameEdit = page->findChild<QWidget *>(MainWindowUI::newServerNameEditName());
            QWidget *card = page->findChild<QWidget *>(QStringLiteral("newServerIdentity"));
            QWidget *create = page->findChild<QWidget *>(MainWindowUI::newServerCreateButtonName());
            std::printf("  new-server: name edit y=%d h=%d, identity y=%d h=%d, create center x=%d (window %d)\n",
                        yOf(nameEdit), nameEdit != nullptr ? nameEdit->height() : -1, yOf(card),
                        card != nullptr ? card->height() : -1,
                        create != nullptr ? create->mapTo(&window, create->rect().center()).x() : -1, window.width());
            for (QLabel *label : page->findChildren<QLabel *>())
                if (label->text() == QStringLiteral("服务端"))
                    std::printf("  new-server: section title y=%d\n", yOf(label));
        }
        saveShot(app, window, options.out, QStringLiteral("new-server"), &saved);
    } else {
        std::printf("  SKIP new-server：表单宿主挂不上\n");
    }

    // ---- 任务中心：两三条任务 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    ui.setTaskCenterContent([] { return activityItems(); }, [] {});
    MainWindowUI::BottomBarTaskSummary summary;
    summary.hasTask = true;
    summary.state = TaskCenter::State::Running;
    summary.percent = 41;
    summary.extraCount = 2;
    summary.title = QStringLiteral("下载 Sodium 0.6.13");
    ui.setBottomBarTaskSummary(summary);
    // 「任务中心」的公开入口就是底栏中间那一格（点一下开、再点一下收）。
    if (QPushButton *summaryButton = ui.getTaskSummaryButton()) {
        summaryButton->click();
        pump();
        if (QFrame *pop = window.findChild<QFrame *>(QStringLiteral("activityPopover"))) {
            std::printf("  activity popover %dx%d at (%d,%d) visible=%d\n", pop->width(), pop->height(),
                        pop->mapTo(&window, QPoint(0, 0)).x(), pop->mapTo(&window, QPoint(0, 0)).y(),
                        int(pop->isVisible()));
        }
        saveShot(app, window, options.out, QStringLiteral("activity-center"), &saved);
        summaryButton->click();
        pump();
    } else {
        std::printf("  SKIP activity-center：拿不到底栏任务按钮\n");
    }

    // ---- 账号弹层：未登录 / 已登录 ----
    ui.setBottomBarPanelContent(
        [](bool account) {
            return account ? accountRows(false) : QList<MainWindowUI::PanelRow>();
        },
        [](bool, const QString &, int) {});
    ui.toggleAccountsPanel();
    pump();
    if (QWidget *pop = window.findChild<QWidget *>(MainWindowUI::accountPopoverName())) {
        std::printf("  account popover %dx%d at (%d,%d) visible=%d\n", pop->width(), pop->height(),
                    pop->mapTo(&window, QPoint(0, 0)).x(), pop->mapTo(&window, QPoint(0, 0)).y(),
                    int(pop->isVisible()));
    }
    saveShot(app, window, options.out, QStringLiteral("account-signed-out"), &saved);
    ui.toggleAccountsPanel();
    pump();

    if (ui.getHomeAccountLabel() != nullptr) ui.getHomeAccountLabel()->setText(QStringLiteral("Steve"));
    if (ui.getHomeAccountTypeLabel() != nullptr) {
        ui.getHomeAccountTypeLabel()->setText(MainWindowUI::accountTypeOnlineText());
        ui.getHomeAccountTypeLabel()->show();
    }
    if (ui.getHomeAvatarLabel() != nullptr)
        ui.getHomeAvatarLabel()->setPixmap(faceMark(QColor(0x3c, 0x9e, 0x7a), QStringLiteral("S"), 32));
    ui.setBottomBarPanelContent(
        [](bool account) {
            return account ? accountRows(true) : QList<MainWindowUI::PanelRow>();
        },
        [](bool, const QString &, int) {});
    ui.toggleAccountsPanel();
    pump();
    saveShot(app, window, options.out, QStringLiteral("account-signed-in"), &saved);
    ui.toggleAccountsPanel();
    pump();

    std::printf("saved %d shots to %s (%dx%d, theme=%s)\n", saved, qPrintable(options.out),
                options.size.width(), options.size.height(), qPrintable(options.theme));
    return saved > 0 ? 0 : 1;
}

/// 原有的那五张 × 四个主题（窗口 1001×776）—— 不带参数时的默认行为，保持不动。
/// 主窗口 + 此刻开着的顶层浮层叠成一张；`-zoom` 那张是浮层四周留 24px、放大两倍。
bool savePopupShot(QApplication &app, QMainWindow &window, const QString &outDir, const QString &name, int *saved) {
    app.processEvents();
    app.processEvents();
    QPixmap shot = window.grab();
    QRect popupArea;
    {
        QPainter painter(&shot);
        for (QWidget *top : QApplication::topLevelWidgets()) {
            const Qt::WindowType type = top->windowType();
            if ((type != Qt::Popup && type != Qt::ToolTip) || !top->isVisible()) continue;
            const QRect at(window.mapFromGlobal(top->mapToGlobal(QPoint(0, 0))), top->size());
            painter.drawPixmap(at.topLeft(), top->grab());
            popupArea = popupArea.united(at);
        }
    }
    if (popupArea.isEmpty()) {
        std::printf("  FAIL %s：没有开着的浮层\n", qPrintable(name));
        return false;
    }
    const QDir dir(outDir);
    const QPixmap zoom = shot.copy(popupArea.adjusted(-24, -24, 24, 24).intersected(shot.rect()));
    const QPixmap zoomed = zoom.scaled(zoom.size() * 2, Qt::KeepAspectRatio, Qt::FastTransformation);
    if (!shot.save(dir.filePath(name + QStringLiteral(".png"))) ||
        !zoomed.save(dir.filePath(name + QStringLiteral("-zoom.png")))) {
        std::printf("  FAIL %s -> %s\n", qPrintable(name), qPrintable(outDir));
        return false;
    }
    ++(*saved);
    std::printf("  %s  %dx%d\n", qPrintable(name), window.width(), window.height());
    return true;
}

int runPopups(QApplication &app, const ShotOptions &options) {
    Theme::Spec spec = options.theme == QLatin1String("default") ? Theme::load() : Theme::presetSpec(options.theme);
    if (!options.background.isEmpty()) {
        Theme::PartStyle &page = spec.parts[int(Theme::Part::PageBackground)];
        page.hasImage = true;
        page.imagePath = options.background;
    }
    Theme::setSpec(spec);
    Theme::apply(app);
    QDir().mkpath(options.out);
    int saved = 0;
    const auto pump = [&app] {
        app.processEvents();
        app.processEvents();
    };
    const auto closePopups = [&pump] {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (top->windowType() == Qt::Popup && top->isVisible()) top->close();
        }
        Popups::hideTooltip();
        pump();
    };

    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(options.size);
    window.show();
    pump();

    // ---- 主页风景底上：右键菜单 + tooltip ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    fillHomeSamples(ui);
    pump();
    {
        QMenu menu(&window);
        menu.addAction(QStringLiteral("打开实例文件夹"));
        menu.addAction(QStringLiteral("复制路径"));
        menu.addSeparator();
        menu.addAction(QStringLiteral("删除实例"));
        menu.popup(window.mapToGlobal(QPoint(window.width() / 2, window.height() / 3)));
        savePopupShot(app, window, options.out, QStringLiteral("home-context-menu"), &saved);
        menu.close();
        pump();
    }
    Popups::showTooltip(QStringLiteral("打开实例文件夹"), window.mapToGlobal(QPoint(window.width() / 3, window.height() / 2)));
    savePopupShot(app, window, options.out, QStringLiteral("home-tooltip"), &saved);
    closePopups();

    // ---- 智能体：实例下拉（用户报黑框的那一块）/ ⋯ 菜单 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::agentTabIndex());
    if (AgentPage *agent = ui.getAgentPage()) {
        agent->setConfigured(true, QStringLiteral("已连接 · OpenAI 兼容"));
        agent->setInstanceChip(QStringLiteral("跟随主页"), false);
        AgentView::SessionMeta session;
        session.id = QStringLiteral("s1");
        session.title = QStringLiteral("查一下崩溃原因");
        agent->setSessions({session});
        agent->setCurrentSession(QStringLiteral("s1"));
        agent->setTitle(session.title);
        agent->showEmptyState(false);
        pump();
        agent->showInstanceMenu({{QStringLiteral("a"), QStringLiteral("1.20.1-Forge 47.2.0")},
                                 {QStringLiteral("b"), QStringLiteral("1.21.4-Fabric 0.16.9")},
                                 {QStringLiteral("c"), QStringLiteral("1.20.4 原版")}},
                                QString());
        savePopupShot(app, window, options.out, QStringLiteral("agent-instance-menu"), &saved);
        closePopups();
        agent->openMoreMenu();
        savePopupShot(app, window, options.out, QStringLiteral("agent-more-menu"), &saved);
        closePopups();
    }

    // ---- 设置页：下拉弹层 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
    fillSettingsSamples(ui);
    pump();
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        if (!combo->isVisible() || combo->count() < 3) continue;
        combo->showPopup();
        savePopupShot(app, window, options.out, QStringLiteral("settings-combo"), &saved);
        combo->hidePopup();
        pump();
        break;
    }
    {
        // 补全列表（实例设置「目标版本」、更新实例那两处的可编辑下拉框）：借设置页当底。
        QComboBox editable(ui.getTabWidget()->currentWidget());
        editable.setEditable(true);
        editable.addItems({QStringLiteral("1.20.1"), QStringLiteral("1.20.4"), QStringLiteral("1.21"),
                           QStringLiteral("1.21.4"), QStringLiteral("24w14a")});
        editable.completer()->setFilterMode(Qt::MatchContains);
        editable.completer()->setCompletionMode(QCompleter::PopupCompletion);
        // 摆在「运行时」小标题那一行的空档里（标题和「Java 管理…」之间），不压任何字。
        editable.setGeometry(120, 82, 320, 32);
        editable.show();
        editable.lineEdit()->setText(QStringLiteral("1.2"));
        editable.completer()->setCompletionPrefix(QStringLiteral("1.2"));
        editable.completer()->complete();
        savePopupShot(app, window, options.out, QStringLiteral("settings-completer"), &saved);
        editable.completer()->popup()->hide();
        pump();
    }

    // ---- 实例页：文件夹浮层 / 分组齿轮菜单 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
    fillInstanceTree(ui);
    ui.setFolderRows({{QStringLiteral("D:/Games/.minecraft"), QStringLiteral("官方启动器"), QStringLiteral("D:/Games/.minecraft"), false, true},
                      {QStringLiteral("E:/MC/modpacks"), QStringLiteral("整合包"), QStringLiteral("E:/MC/modpacks"), false, false}});
    pump();
    // 三个标签各一张：路径行写法统一，「导入整合包…」只在预设标签收起（issue #18）。
    {
        const QString base = QStringLiteral("D:\\Games\\Minecraft\\Instances\\Season Five Survival Server Network\\.minecraft");
        ui.setInstanceFolderText(QStringLiteral("%1（12 个实例）").arg(base));
        ui.setServerFolderText(QStringLiteral("%1\\Servers（2 个服务端）").arg(base));
        ui.setPresetSubtitle(QStringLiteral("%1\\wtfml-presets（5 份预设）").arg(base));
        const std::pair<MainWindowUI::InstanceMode, const char *> tabs[] = {
            {MainWindowUI::InstanceMode::Client, "instance-tab-client"},
            {MainWindowUI::InstanceMode::Server, "instance-tab-server"},
            {MainWindowUI::InstanceMode::Preset, "instance-tab-preset"}};
        for (const auto &[mode, name] : tabs) {
            ui.setInstanceMode(mode);
            saveShot(app, window, options.out, QString::fromLatin1(name), &saved);
        }
        ui.setInstanceMode(MainWindowUI::InstanceMode::Client);
    }
    if (auto *pill = window.findChild<QPushButton *>(MainWindowUI::instanceFolderPillName())) {
        pill->click();
        savePopupShot(app, window, options.out, QStringLiteral("instance-folder-popup"), &saved);
        closePopups();
    }
    if (QTreeWidget *tree = ui.getInstanceList(); tree != nullptr && tree->topLevelItemCount() > 0) {
        MainWindowUI::GroupRow row;
        row.groupId = QStringLiteral("g-modded");
        row.name = QStringLiteral("1.20.1 整合包");
        row.memberCount = 2;
        row.hasSelection = true;
        const QRect head = tree->visualItemRect(tree->topLevelItem(0));
        // 锚点放在行中间：只为看面，贴行尾时 offscreen 那块屏比窗口宽，菜单会伸到窗外拍不全。
        const QRect gear(tree->viewport()->mapToGlobal(QPoint(head.center().x(), head.top())), QSize(28, head.height()));
        ui.openGroupMenu(row, gear);
        savePopupShot(app, window, options.out, QStringLiteral("instance-group-menu"), &saved);
        closePopups();
    }
    // ---- 实例设置·概览：「更改图标」浮层（勾着第 4 颗「幸运方块」）----
    {
        using Dlg = InstanceSettingsDialog;
        Dlg dialog(QStringLiteral("1.20.1 整合包"), QStringLiteral("1.20.1-Forge"), Dlg::Override(),
                   Dlg::GlobalValues(), QStringList(), false);
        dialog.setIcon(VersionIcons::iconValueForKey(VersionIcons::iconCandidateKeys().value(3)), QPixmap(), QPixmap());
        if (ui.overlay()->showPage(&dialog, QStringLiteral("实例设置"), MainWindowUI::instancesTabIndex())) {
            dialog.showPage(Dlg::overviewCategoryKey());
            pump();
            if (auto *change = dialog.findChild<QPushButton *>(Dlg::changeIconButtonName())) {
                change->click();
                // 浮层左缘对齐按钮，按钮贴着页右缘 ⇒ 离屏那张图里会伸出窗口；拍图时往左挪回窗内。
                if (auto *popup = dialog.findChild<QWidget *>(Dlg::iconMenuName())) {
                    const int overflow = popup->frameGeometry().right() - window.mapToGlobal(QPoint(window.width() - 16, 0)).x();
                    if (overflow > 0) popup->move(popup->pos() - QPoint(overflow, 0));
                }
                savePopupShot(app, window, options.out, QStringLiteral("instance-icon-popup"), &saved);
                closePopups();
            }
            ui.overlay()->closeOverlay();
            pump();
        }
    }
    std::printf("popups: %d 张\n", saved);
    return saved > 0 ? 0 : 1;
}

/// `--agent-polish`（步骤 108）：发送钮禁用 / 可用（各带一张放大）、长标题 + 失败行、
/// API 设置页「不认证」态（顶部 + 高级展开）。
/// 离屏没有真鼠标：挂悬停位还不够，按钮自己记着「鼠标在不在形状里」，得喂一次悬停进入 + 移动才认
/// （别 repolish，会把 36 的定高冲掉）。on=false 撤掉。
void fakeHover(QWidget *w, bool on) {
    w->setAttribute(Qt::WA_UnderMouse, on);
    const QPointF at(w->rect().center());
    QHoverEvent hover(on ? QEvent::HoverEnter : QEvent::HoverLeave, at, w->mapToGlobal(at), QPointF(-1, -1));
    QApplication::sendEvent(w, &hover);
    if (!on) return;
    QMouseEvent move(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
}

int runAgentPolish(QApplication &app, const ShotOptions &options) {
    Theme::setSpec(Theme::presetSpec(options.theme));
    Theme::apply(app);
    QDir().mkpath(options.out);
    int saved = 0;
    const auto pump = [&app] {
        app.processEvents();
        app.processEvents();
    };
    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(options.size);
    window.show();
    pump();
    const QString prefix = options.theme + QLatin1Char('-');
    const auto zoomOf = [&](QWidget *target, const QString &name, const std::function<void()> &prep = {}) {
        pump();
        if (prep) prep();  // 抓图前最后一刻挂状态：pump 里的合成进出事件会清掉悬停位
        const QPixmap shot = window.grab();
        const QRect at(target->mapTo(&window, QPoint(0, 0)), target->size());
        const QPixmap piece = shot.copy(at.adjusted(-40, -28, 16, 16).intersected(shot.rect()));
        if (piece.scaled(piece.size() * 4, Qt::KeepAspectRatio, Qt::FastTransformation)
                .save(QDir(options.out).filePath(name + QStringLiteral(".png"))))
            ++saved;
    };

    ui.getTabWidget()->setCurrentIndex(MainWindowUI::agentTabIndex());
    if (AgentPage *agent = ui.getAgentPage()) {
        agent->setConfigured(true, QStringLiteral("已连接 · OpenAI 兼容"));
        agent->setInstanceChip(QStringLiteral("跟随主页"), false);
        const QString longAsk = QStringLiteral("Forge 1.20.1 启动就崩，日志里说 mixin 冲突");
        AgentView::SessionMeta session;
        session.id = QStringLiteral("s1");
        session.title = AgentSessionTitle::fromMessage(longAsk);
        session.updatedAt = QDateTime::currentDateTime();
        AgentView::SessionMeta blank;
        blank.id = QStringLiteral("s0");
        blank.title = QStringLiteral("新对话");
        blank.updatedAt = QDateTime::currentDateTime().addSecs(-60);
        agent->setSessions({session, blank});

        // 空会话：发送钮禁用 / 打字后可用。
        agent->setCurrentSession(QStringLiteral("s0"));
        agent->showEmptyState(true);
        pump();
        QWidget *send = agent->findChild<QWidget *>(QStringLiteral("agentSendButton"));
        saveShot(app, window, options.out, prefix + QStringLiteral("send-disabled"), &saved);
        if (send != nullptr) zoomOf(send, prefix + QStringLiteral("send-disabled-zoom"));
        auto *composer = agent->findChild<QPlainTextEdit *>();
        if (composer != nullptr) composer->setPlainText(QStringLiteral("为什么启动就崩？"));
        pump();
        saveShot(app, window, options.out, prefix + QStringLiteral("send-enabled"), &saved);
        if (send != nullptr) {
            zoomOf(send, prefix + QStringLiteral("send-enabled-zoom"));
            zoomOf(send, prefix + QStringLiteral("send-hover-zoom"), [send] { fakeHover(send, true); });
            if (auto *button = qobject_cast<QPushButton *>(send)) {
                button->setDown(true);
                zoomOf(send, prefix + QStringLiteral("send-pressed-zoom"));
                button->setDown(false);
            }
            fakeHover(send, false);
        }
        if (composer != nullptr) composer->clear();

        // 长标题 + 发送失败那一行（只在消息行上出一次，带「重试」）。
        agent->setCurrentSession(QStringLiteral("s1"));
        agent->setTitle(session.title);
        AgentView::Message ask;
        ask.id = QStringLiteral("m1");
        ask.user = true;
        ask.content = longAsk;
        AgentView::Message failed;
        failed.id = QStringLiteral("m2");
        failed.status = AgentView::MessageStatus::Error;
        failed.errorText = QStringLiteral("模型服务拒绝连接。");
        agent->setMessages({ask, failed});
        agent->showEmptyState(false);
        saveShot(app, window, options.out, prefix + QStringLiteral("long-title-failed"), &saved);

        // 生成中（步骤 110）：停止钮常态 / 悬停 / 按下。
        AgentView::Message streaming;
        streaming.id = QStringLiteral("m2");
        streaming.status = AgentView::MessageStatus::Streaming;
        streaming.content = QStringLiteral("先看日志里 mixin 冲突那几行：");
        agent->setMessages({ask, streaming});
        agent->setStreaming(true);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // 旧行 deleteLater，离屏手动收
        pump();
        saveShot(app, window, options.out, prefix + QStringLiteral("streaming"), &saved);
        if (auto *stop = agent->findChild<QPushButton *>(QStringLiteral("agentStopButton"))) {
            zoomOf(stop, prefix + QStringLiteral("stop-zoom"));
            zoomOf(stop, prefix + QStringLiteral("stop-hover-zoom"), [stop] { fakeHover(stop, true); });
            stop->setDown(true);
            zoomOf(stop, prefix + QStringLiteral("stop-pressed-zoom"));
            stop->setDown(false);
            fakeHover(stop, false);
        }
        agent->setStreaming(false);
    }

    // API 与模型设置：认证方式「不认证」、没存密钥。
    {
        AgentSettingsPage settingsPage;
        AgentView::ProviderDraft draft;
        draft.id = QStringLiteral("default");
        draft.name = QStringLiteral("Local test");
        draft.baseUrl = QStringLiteral("http://127.0.0.1:9/v1");
        draft.model = QStringLiteral("test-model");
        draft.authMode = QStringLiteral("none");
        draft.deployment = QStringLiteral("local");
        settingsPage.setProfile(draft, false, QString());
        if (ui.overlay()->showPage(&settingsPage, QStringLiteral("API 与模型设置"), MainWindowUI::agentTabIndex())) {
            pump();
            saveShot(app, window, options.out, prefix + QStringLiteral("api-noauth-top"), &saved);
            if (auto *advanced = settingsPage.findChild<SettingsParts::ExpandSection *>()) {
                advanced->setExpanded(true, false);
                pump();
                if (auto *scroll = settingsPage.findChild<QScrollArea *>())
                    scroll->ensureWidgetVisible(advanced->body(), 0, 0);
                saveShot(app, window, options.out, prefix + QStringLiteral("api-noauth-advanced"), &saved);
            }
            ui.overlay()->closeOverlay();
            pump();
        }
    }
    std::printf("agent-polish: %d 张\n", saved);
    return saved > 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// `--site`（步骤 174）：官网「功能详解」页候选截图
//
// 这一趟只摆夹具，不联网、不读本机真实的 .minecraft / 用户配置；路径一律写成
// D:\Games\WTFML\… 这种通用形式，玩家名固定 Steve。清单、文件名见任务单与
// `docs/archive/` 那一条记录；拍完另用 Pillow 拼 contact.png（不在这个进程里）。
// ---------------------------------------------------------------------------

/// 一个演示实例的「当前 / 收藏 / 最近」行（照 fillHomeSamples 的形状）。
MainWindowUI::HomeQuickItem siteHomeItem(const QString &key, const QString &name, const QString &detail,
                                         bool current, bool favorite, bool recent) {
    MainWindowUI::HomeQuickItem row;
    row.key = key;
    row.name = name;
    row.detail = detail;
    row.icon = QIcon(VersionIcons::isoBlock(VersionIcons::isoBlockKindFor(name),
                                            MainWindowUI::instanceTreeIconSize() * 2));
    row.current = current;
    row.favorite = favorite;
    row.recent = recent;
    return row;
}

/// 官网那五份实例（名字、版本、加载器照任务单）。
void fillSiteHome(MainWindowUI &ui) {
    const QList<MainWindowUI::HomeQuickItem> all{
        siteHomeItem(QStringLiteral("光影与优化-1.21.1"), QStringLiteral("光影与优化-1.21.1"),
                     QStringLiteral("Minecraft 1.21.1 / Fabric 0.16.9"), true, true, true),
        siteHomeItem(QStringLiteral("机械动力-1.20.1"), QStringLiteral("机械动力-1.20.1"),
                     QStringLiteral("Minecraft 1.20.1 / Forge 47.2.0"), false, true, true),
        siteHomeItem(QStringLiteral("科技整合-1.21.1"), QStringLiteral("科技整合-1.21.1"),
                     QStringLiteral("Minecraft 1.21.1 / NeoForge 21.1.77"), false, true, false),
        siteHomeItem(QStringLiteral("原版生存-1.21.4"), QStringLiteral("原版生存-1.21.4"),
                     QStringLiteral("Minecraft 1.21.4 / 原版"), false, false, true),
        siteHomeItem(QStringLiteral("空岛-1.20.4"), QStringLiteral("空岛-1.20.4"),
                     QStringLiteral("Minecraft 1.20.4 / Fabric 0.15.11"), false, false, true)};
    ui.setHomeQuickRows({all.at(0)}, {all.at(1), all.at(2)}, {all.at(0), all.at(1), all.at(3)});
    ui.setHomeQuickMode(MainWindowUI::HomeQuickMode::Current);
    if (ui.getHomeAccountLabel() != nullptr) ui.getHomeAccountLabel()->setText(QStringLiteral("Steve"));
    if (ui.getHomeAccountTypeLabel() != nullptr) {
        ui.getHomeAccountTypeLabel()->setText(MainWindowUI::accountTypeOnlineText());
        ui.getHomeAccountTypeLabel()->show();
    }
    if (ui.getHomeAvatarLabel() != nullptr)
        ui.getHomeAvatarLabel()->setPixmap(faceMark(QColor(0x3c, 0x9e, 0x7a), QStringLiteral("S"), 32));
    if (ui.getAccountLabel() != nullptr) ui.getAccountLabel()->setText(QStringLiteral("Steve"));
    if (ui.getAccountTypeLabel() != nullptr) ui.getAccountTypeLabel()->setText(MainWindowUI::accountTypeOnlineText());
    if (ui.getAvatarLabel() != nullptr)
        ui.getAvatarLabel()->setPixmap(faceMark(QColor(0x3c, 0x9e, 0x7a), QStringLiteral("S"), 32));
}

/// 一台像真玩家在用的实例：17 个常见模组（一份禁用）、三个存档、资源包 / 光影各两份。
void makeSiteInstanceFixture(const QString &root) {
    const auto mkfile = [](const QString &path, qint64 bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.resize(bytes);
    };
    for (const auto &mod : {qMakePair(QStringLiteral("fabric-api-0.115.0+1.21.1.jar"), qint64(2211840)),
                            qMakePair(QStringLiteral("sodium-fabric-0.6.13+mc1.21.1.jar"), qint64(892928)),
                            qMakePair(QStringLiteral("iris-fabric-1.8.4+mc1.21.1.jar"), qint64(1245184)),
                            qMakePair(QStringLiteral("lithium-fabric-0.15.1+mc1.21.1.jar"), qint64(645120)),
                            qMakePair(QStringLiteral("ferritecore-7.0.2-fabric.jar"), qint64(181248)),
                            qMakePair(QStringLiteral("entityculling-fabric-1.7.3.jar"), qint64(96256)),
                            qMakePair(QStringLiteral("indium-1.0.35+mc1.21.jar"), qint64(128000)),
                            qMakePair(QStringLiteral("continuity-3.0.0+1.21.jar"), qint64(512000)),
                            qMakePair(QStringLiteral("modmenu-11.0.3.jar"), qint64(460800)),
                            qMakePair(QStringLiteral("cloth-config-15.0.140-fabric.jar"), qint64(1177600)),
                            qMakePair(QStringLiteral("architectury-13.0.8-fabric.jar"), qint64(512000)),
                            qMakePair(QStringLiteral("xaeros-minimap-fabric-25.2.0.jar"), qint64(2293760)),
                            qMakePair(QStringLiteral("xaeros-world-map-1.39.2.jar"), qint64(3407872)),
                            qMakePair(QStringLiteral("appleskin-fabric-3.0.5+mc1.21.jar"), qint64(61440)),
                            qMakePair(QStringLiteral("jade-15.10.1+fabric.jar"), qint64(256000)),
                            qMakePair(QStringLiteral("jei-1.21.1-fabric-19.27.0.340.jar"), qint64(1638400)),
                            qMakePair(QStringLiteral("create-1.21.1-6.0.6.jar"), qint64(14680064)),
                            qMakePair(QStringLiteral("old-fabric-api-0.15.11.jar.disabled"), qint64(2100000))})
        mkfile(root + QStringLiteral("/mods/") + mod.first, mod.second);
    for (const QString &world : {QStringLiteral("生存世界"), QStringLiteral("空岛生存"),
                                 QStringLiteral("红石试验场")}) {
        mkfile(root + QStringLiteral("/saves/") + world + QStringLiteral("/level.dat"), 9216);
        mkfile(root + QStringLiteral("/saves/") + world + QStringLiteral("/region/r.0.0.mca"), 1048576);
    }
    mkfile(root + QStringLiteral("/resourcepacks/清新材质 1.3.zip"), 8650752);
    mkfile(root + QStringLiteral("/resourcepacks/低火把.zip"), 262144);
    mkfile(root + QStringLiteral("/shaderpacks/Complementary 5.3.zip"), 4718592);
    mkfile(root + QStringLiteral("/shaderpacks/BSL 8.4.zip"), 3670016);
    mkfile(root + QStringLiteral("/options.txt"), 4096);
    mkfile(root + QStringLiteral("/servers.dat"), 2048);
    mkfile(root + QStringLiteral("/logs/latest.log"), 524288);
    mkfile(root + QStringLiteral("/screenshots/2026-11-12_20.31.07.png"), 180224);
    QDir().mkpath(root + QStringLiteral("/game"));
}

/// 下载 › 整合包那一页的资源表：几个常见的包（导入确认框就是从这一页弹出来的）。
void fillSiteModpackTree(MainWindowUI &ui) {
    QTreeWidget *tree = ui.getDownloadResourceTree(QStringLiteral("modpack"));
    if (tree == nullptr) return;
    tree->clear();
    const int column = MainWindowUI::versionTreeKeyColumn();
    const QList<std::tuple<QString, QString, QColor>> packs{
        {QStringLiteral("Fabulously Optimized"), QStringLiteral("FabulouslyOptimized / 18.4M"),
         QColor(0x4f, 0x8a, 0xd8)},
        {QStringLiteral("Create: Above and Beyond"), QStringLiteral("simibubi / 9.2M"),
         QColor(0xc9, 0x7a, 0x3c)},
        {QStringLiteral("Better MC [FABRIC]"), QStringLiteral("LunaPixelStudios / 24.1M"),
         QColor(0x3c, 0x9e, 0x7a)},
        {QStringLiteral("All the Mods 9"), QStringLiteral("ATMTeam / 31.7M"), QColor(0x7a, 0x5c, 0xc9)},
        {QStringLiteral("DawnCraft"), QStringLiteral("broot44 / 12.8M"), QColor(0x9e, 0x4b, 0x6a)},
        {QStringLiteral("RLCraft"), QStringLiteral("Shivaxi / 41.6M"), QColor(0x4b, 0x7a, 0x9e)}};
    for (int i = 0; i < packs.size(); ++i) {
        const auto &[name, detail, color] = packs.at(i);
        auto *hit = new QTreeWidgetItem(tree);
        hit->setData(column, Qt::UserRole, QStringLiteral("modpack-%1").arg(i + 1));
        hit->setData(column, MainWindowUI::resourceKindRole(), QStringLiteral("modpack"));
        hit->setData(column, MainWindowUI::resourceSourceRole(), QStringLiteral("modrinth"));
        hit->setText(column, name);
        hit->setData(column, MainWindowUI::rowSubTextRole(), detail);
        hit->setData(column, MainWindowUI::rowIconRole(),
                     letterMark(color, name, MainWindowUI::resourceTreeIconSize()));
    }
}

/// 服务端那一页的字段表：名字 / 值域照产品那张唯一出处。
QList<ServerSettingsPage::FieldView> siteServerFields() {
    QList<ServerSettingsPage::FieldView> views;
    for (const ServerPropertiesForm::Field &field : ServerPropertiesForm::commonFields()) {
        ServerSettingsPage::FieldView view;
        view.key = field.key;
        view.label = field.label;
        view.hint = field.hint;
        switch (field.kind) {
        case ServerPropertiesForm::Kind::Text: view.kind = ServerSettingsPage::Kind::Text; break;
        case ServerPropertiesForm::Kind::Number: view.kind = ServerSettingsPage::Kind::Number; break;
        case ServerPropertiesForm::Kind::Switch: view.kind = ServerSettingsPage::Kind::Switch; break;
        case ServerPropertiesForm::Kind::Choice: view.kind = ServerSettingsPage::Kind::Choice; break;
        }
        view.min = field.min;
        view.max = field.max;
        view.preview = field.preview;
        view.zipPick = field.zipPick;
        for (const ServerPropertiesForm::Choice &choice : field.choices) {
            view.choiceTokens.append(choice.token);
            view.choiceNames.append(choice.name);
        }
        views.append(view);
    }
    return views;
}

/// Paper 1.21.1 的一段正常启动日志 + 两个玩家加入（任务单里那一段）。
QStringList siteServerLog() {
    return {
        QStringLiteral("[12:00:01 INFO]: Starting minecraft server version 1.21.1"),
        QStringLiteral("[12:00:01 INFO]: Loading properties"),
        QStringLiteral("[12:00:01 INFO]: Default game type: SURVIVAL"),
        QStringLiteral("[12:00:02 INFO]: Generating keypair"),
        QStringLiteral("[12:00:02 INFO]: Starting Minecraft server on *:25565"),
        QStringLiteral("[12:00:03 INFO]: Using epoll channel type"),
        QStringLiteral("[12:00:03 INFO]: Paper: Using Java 21, this is the recommended version"),
        QStringLiteral("[12:00:03 INFO]: Preparing level \"world\""),
        QStringLiteral("[12:00:04 INFO]: Preparing start region for dimension minecraft:overworld"),
        QStringLiteral("[12:00:05 INFO]: Preparing spawn area: 39%"),
        QStringLiteral("[12:00:07 INFO]: Preparing spawn area: 84%"),
        QStringLiteral("[12:00:08 INFO]: Time elapsed: 4211 ms"),
        QStringLiteral("[12:00:08 INFO]: Done (6.842s)! For help, type \"help\""),
        QStringLiteral("[12:00:09 INFO]: Steve joined the game"),
        QStringLiteral("[12:00:12 INFO]: Steve[/10.0.0.21:52144] logged in with entity id 214 at "
                       "[world, -123.5, 71.0, 208.2]"),
        QStringLiteral("[12:01:37 INFO]: Alex joined the game"),
        QStringLiteral("[12:01:41 INFO]: Alex[/10.0.0.18:51872] logged in with entity id 431 at "
                       "[world, -96.2, 68.0, 231.7]")};
}

/// 任务中心那三件下载任务：一件在跑、两件已完成。
QList<TaskCenter::Item> siteTaskItems() {
    QList<TaskCenter::Item> items;
    TaskCenter::Item running;
    running.key = QStringLiteral("1");
    running.title = QStringLiteral("下载 Iris Shaders 1.8.4");
    running.detail = QStringLiteral("已下载 21.7 MB / 47.2 MB");
    running.percent = 46;
    running.state = TaskCenter::State::Running;
    running.steps = {{QStringLiteral("解析依赖"), TaskCenter::StepState::Done, QString()},
                     {QStringLiteral("下载文件"), TaskCenter::StepState::Running,
                      QStringLiteral("21.7 MB / 47.2 MB")},
                     {QStringLiteral("写入实例"), TaskCenter::StepState::Pending, QString()}};
    running.capabilities.cancel = true;
    running.capabilities.pause = true;
    items.append(running);

    TaskCenter::Item install;
    install.key = QStringLiteral("2");
    install.title = QStringLiteral("安装 Minecraft 1.21.1");
    install.detail = QStringLiteral("26 个文件");
    install.percent = 100;
    install.state = TaskCenter::State::Succeeded;
    install.finishedAt = QDateTime::currentDateTime().addSecs(-420);
    install.steps = {{QStringLiteral("下载游戏文件"), TaskCenter::StepState::Done, QString()},
                     {QStringLiteral("解压 natives"), TaskCenter::StepState::Done, QString()}};
    install.capabilities.openTarget = true;
    items.append(install);

    TaskCenter::Item loader;
    loader.key = QStringLiteral("3");
    loader.title = QStringLiteral("安装 Fabric 0.16.9");
    loader.detail = QStringLiteral("已写入 1.21.1-Fabric");
    loader.percent = 100;
    loader.state = TaskCenter::State::Succeeded;
    loader.finishedAt = QDateTime::currentDateTime().addSecs(-360);
    loader.capabilities.openTarget = true;
    items.append(loader);
    return items;
}

/// 一串「在实例目录里写个文件、等你批准」的审批请求（智能体确认卡）。
QJsonObject siteApprovalRequest() {
    return QJsonObject{
        {QStringLiteral("approvalId"), QStringLiteral("a-site-1")},
        {QStringLiteral("tool"), QStringLiteral("fs.write")},
        {QStringLiteral("displayName"), QStringLiteral("写入视频设置")},
        {QStringLiteral("target"), QStringLiteral("D:/Games/WTFML/instances/光影与优化-1.21.1/options.txt")},
        {QStringLiteral("summary"), QStringLiteral("修改 2 行：renderDistance 8 → 12，ao false → true。")},
        {QStringLiteral("diff"),
         QStringLiteral("- renderDistance:8\n- ao:false\n+ renderDistance:12\n+ ao:true")},
        {QStringLiteral("backupHint"), QStringLiteral("写之前会把原文件另存为 options.txt.bak。")},
        {QStringLiteral("sessionAllowAvailable"), false},
        {QStringLiteral("egress"),
         QJsonObject{{QStringLiteral("kind"), QStringLiteral("本机文件")},
                     {QStringLiteral("reason"), QStringLiteral("不经过网络")}}}};
}

int runSite(QApplication &app, const ShotOptions &options) {
    QDir().mkpath(options.out);
    // 先钉主题再建窗口：MainWindowUI 的控件是在构造里按当时那份调色板摆的。
    Theme::setSpec(Theme::presetSpec(options.theme.isEmpty() ? QStringLiteral("dark") : options.theme));
    Theme::apply(app);
    int saved = 0;
    const auto pump = [&app] {
        for (int i = 0; i < 6; ++i) app.processEvents();
    };

    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(options.size.isValid() && options.size.width() > 0 ? options.size : QSize(1120, 760));
    window.show();
    pump();
    const auto shot = [&](const QString &name) {
        pump();
        // 截图前先扫一遍可见文字：夹具里不该出现本机真实路径 / 用户名（任务单那一条）。
        const QStringList bad{QStringLiteral("WTFML-develop"), QStringLiteral("dsh-win-a7d60cfd"),
                              QStringLiteral("C:\\Users"), QStringLiteral("24346"),
                              QStringLiteral(".review-site")};
        const auto check = [&](const QString &text, const char *where) {
            for (const QString &needle : bad) {
                if (!text.contains(needle, Qt::CaseInsensitive)) continue;
                std::printf("  LEAK[%s] %s\n", where, qPrintable(text.left(140)));
                return;
            }
        };
        for (QLabel *label : window.findChildren<QLabel *>())
            if (label->isVisible()) check(label->text(), "label");
        for (QLineEdit *edit : window.findChildren<QLineEdit *>())
            if (edit->isVisible()) check(edit->text(), "edit");
        for (QPlainTextEdit *edit : window.findChildren<QPlainTextEdit *>())
            if (edit->isVisible()) check(edit->toPlainText().left(4000), "plain");
        for (QListWidget *list : window.findChildren<QListWidget *>()) {
            if (!list->isVisible()) continue;
            for (int i = 0; i < list->count(); ++i) check(list->item(i)->text(), "list");
        }
        for (QTreeWidget *tree : window.findChildren<QTreeWidget *>()) {
            if (!tree->isVisible()) continue;
            for (int i = 0; i < tree->topLevelItemCount(); ++i) check(tree->topLevelItem(i)->text(0), "tree");
        }
        saveShot(app, window, options.out, name, &saved);
    };

    const auto applyTheme = [&](const QString &id) {
        Theme::setSpec(Theme::presetSpec(id));
        Theme::apply(app);
        pump();
        std::printf("  主题 %s（%s，%s）\n", qPrintable(id), qPrintable(Theme::presetName(id)),
                    Theme::isDark() ? "深色" : "浅色");
    };
    const auto inWindow = [&](QWidget *w) {
        if (w == nullptr) return QRect();
        return QRect(w->mapTo(&window, QPoint(0, 0)), w->size());
    };
    const auto reportLabel = [&](const QString &text, const QString &what) {
        for (QLabel *label : window.findChildren<QLabel *>()) {
            if (label->text() != text || !label->isVisible()) continue;
            const QRect at = inWindow(label);
            std::printf("  %s「%s」(%d,%d) %dx%d，在窗内 %s\n", qPrintable(what), qPrintable(text.left(24)), at.x(),
                        at.y(), at.width(), at.height(), window.rect().contains(at) ? "是" : "**否**");
            return;
        }
        std::printf("  %s「%s」没找到\n", qPrintable(what), qPrintable(text.left(24)));
    };
    // 结果区不一定是个 QLabel（可能是只读文本框），按"哪一块里出现了这句话"找。
    const auto reportText = [&](const QString &needle, const QString &what) {
        for (QLabel *label : window.findChildren<QLabel *>()) {
            if (!label->isVisible() || !label->text().contains(needle)) continue;
            const QRect at = inWindow(label);
            std::printf("  %s：含「%s」的标签 (%d,%d) %dx%d，在窗内 %s\n", qPrintable(what),
                        qPrintable(needle.left(20)), at.x(), at.y(), at.width(), at.height(),
                        window.rect().contains(at) ? "是" : "**否**");
            return;
        }
        for (QPlainTextEdit *edit : window.findChildren<QPlainTextEdit *>()) {
            if (!edit->isVisible() || !edit->toPlainText().contains(needle)) continue;
            const QRect at = inWindow(edit);
            std::printf("  %s：只读框里有「%s」(%d,%d) %dx%d\n", qPrintable(what), qPrintable(needle.left(20)), at.x(),
                        at.y(), at.width(), at.height());
            return;
        }
        std::printf("  %s：「%s」没找到\n", qPrintable(what), qPrintable(needle.left(20)));
    };

    // ---- 主页：三套具名主题各一张 ----
    ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    fillSiteHome(ui);
    for (const QString &id : {QStringLiteral("dark"), QStringLiteral("emerald"), QStringLiteral("ocean")}) {
        applyTheme(id);
        shot(QStringLiteral("look-home-") + id);
    }
    applyTheme(QStringLiteral("dark"));
    reportLabel(QStringLiteral("光影与优化-1.21.1"), QStringLiteral("主页当前实例"));
    reportLabel(QStringLiteral("机械动力-1.20.1"), QStringLiteral("主页收藏实例"));
    reportLabel(QStringLiteral("原版生存-1.21.4"), QStringLiteral("主页最近实例"));
    reportLabel(QStringLiteral("Steve"), QStringLiteral("主页账号"));

    // ---- 设置 › 个性化 › 外观：背景图 / 主题色 / 文字不透明度 ----
    {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
        fillSettingsSamples(ui);
        const QStringList keys = MainWindowUI::settingsCategoryKeys();
        const int look = keys.indexOf(QStringLiteral("look"));
        ui.getSettingsTabs()->setCurrentIndex(look);
        // 背景图：铺内置那张风景（走 qrc，不写本机路径），这样「背景图」那一组是有状态的。
        {
            Theme::Spec spec = Theme::current();
            Theme::PartStyle &page = spec.parts[int(Theme::Part::PageBackground)];
            page.hasImage = true;
            page.imagePath = QStringLiteral(":/assets/scenes/dark.jpg");
            page.imageName = QStringLiteral("暮色雪山.jpg");
            Theme::setSpec(spec);
            Theme::apply(app);
            pump();
            const MainWindowUI::ThemeImageRow &row = ui.getThemeImageRow(0);
            if (row.state != nullptr) row.state->setText(QStringLiteral("当前图片：暮色雪山.jpg"));
            if (row.fit != nullptr) row.fit->setEnabled(true);
            if (row.opacity != nullptr) {
                row.opacity->setEnabled(true);
                row.opacity->setValue(96);
            }
            if (row.percent != nullptr) {
                row.percent->setEnabled(true);
                row.percent->setText(MainWindowUI::themeImagePercentText(96));
            }
            if (row.clear != nullptr) row.clear->setEnabled(true);
        }
        QScrollArea *scroll = qobject_cast<QScrollArea *>(ui.getSettingsCategoryStack()->widget(look));
        if (scroll != nullptr && scroll->widget() != nullptr) {
            QLabel *anchor = nullptr;
            for (QLabel *label : scroll->widget()->findChildren<QLabel *>()) {
                if (label->text() == MainWindowUI::themeAccentRowText()) {
                    anchor = label;
                    break;
                }
            }
            if (anchor != nullptr) {
                const int y = anchor->mapTo(scroll->widget(), QPoint(0, 0)).y();
                scroll->verticalScrollBar()->setValue(qMax(0, y - 24));
            }
            std::printf("  外观：滚动 %d/%d，主题色 %s，文字不透明度 %s，背景图「%s」\n",
                        scroll->verticalScrollBar()->value(), scroll->verticalScrollBar()->maximum(),
                        qPrintable(ui.getThemeAccentButton() != nullptr
                                       ? Theme::current().accent.name()
                                       : QStringLiteral("?")),
                        qPrintable(ui.getThemeTextOpacityPercent() != nullptr
                                       ? ui.getThemeTextOpacityPercent()->text()
                                       : QStringLiteral("?")),
                        qPrintable(ui.getThemeImageRow(0).state != nullptr
                                       ? ui.getThemeImageRow(0).state->text()
                                       : QStringLiteral("?")));
        }
        reportLabel(MainWindowUI::themeAccentRowText(), QStringLiteral("设置行"));
        reportLabel(MainWindowUI::themeTextOpacityRowText(), QStringLiteral("设置行"));
        reportLabel(QStringLiteral("背景图"), QStringLiteral("设置分组"));
        shot(QStringLiteral("look-settings"));
        // 这张拍完把背景图撤回去：后面几页照产品默认（内置风景底）看，而不是一块自定义图。
        {
            Theme::Spec spec = Theme::current();
            Theme::PartStyle &page = spec.parts[int(Theme::Part::PageBackground)];
            page.hasImage = false;
            page.imagePath.clear();
            page.imageName.clear();
            Theme::setSpec(spec);
            Theme::apply(app);
            pump();
        }
    }

    // ---- 安装页：停在「选加载器」那一屏（Fabric 展开、其余灰掉）----
    {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
        ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryGameIndex());
        ui.setDownloadHeaderRow(MainWindowUI::downloadCategoryGameIndex());
        ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryGameIndex());
        ui.getGameStack()->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
        QLineEdit *installName = ui.getInstallNameEdit();
        if (installName != nullptr) {
            for (QWidget *child = installName; child != nullptr && child->parentWidget() != nullptr;
                 child = child->parentWidget()) {
                if (auto *stack = qobject_cast<QStackedWidget *>(child->parentWidget()))
                    stack->setCurrentWidget(child);
            }
            installName->show();
        }
        if (ui.getInstallTargetLabel() != nullptr)
            ui.getInstallTargetLabel()->setText(MainWindowUI::installTargetText(QStringLiteral("1.21.1")));
        if (installName != nullptr) installName->setText(QStringLiteral("1.21.1-Fabric"));
        // 安装页整块是滚区里的一页：换了字之后要让内容重新量一遍，否则那两行还按空串的宽度摆着。
        if (ui.getInstallScroll() != nullptr && ui.getInstallScroll()->widget() != nullptr) {
            ui.getInstallScroll()->widget()->adjustSize();
            pump();
        }
        reportLabel(MainWindowUI::installTargetText(QStringLiteral("1.21.1")), QStringLiteral("安装目标"));
        if (QTreeWidget *loaders = ui.getLoaderTree()) {
            QTreeWidgetItem *fabric = nullptr;
            for (int i = 0; i < loaders->topLevelItemCount(); ++i) {
                QTreeWidgetItem *item = loaders->topLevelItem(i);
                if (item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt() == 0) fabric = item;
            }
            if (fabric != nullptr) {
                qDeleteAll(fabric->takeChildren());
                const auto addVersion = [&](const QString &text) {
                    auto *child = new QTreeWidgetItem(fabric);
                    child->setText(MainWindowUI::loaderTreeKeyColumn(), text);
                    child->setData(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole, text);
                    child->setIcon(MainWindowUI::loaderTreeKeyColumn(),
                                   QIcon(VersionIcons::loaderMark(0, MainWindowUI::versionTreeIconSize())));
                };
                addVersion(MainWindowUI::loaderVersionLatestText());
                addVersion(MainWindowUI::loaderVersionStableText());
                addVersion(QStringLiteral("0.16.9"));
                fabric->setDisabled(false);
                fabric->setExpanded(true);
                fabric->setData(MainWindowUI::loaderTreeKeyColumn(), MainWindowUI::rowSubTextRole(),
                                MainWindowUI::loaderVersionChannelShortText(false));
                fabric->setData(MainWindowUI::loaderTreeKeyColumn(), MainWindowUI::rowClearRole(), true);
                loaders->setCurrentItem(fabric->child(1));
            }
            for (int i = 0; i < loaders->topLevelItemCount(); ++i) {
                QTreeWidgetItem *item = loaders->topLevelItem(i);
                const int index = item->data(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole).toInt();
                if (index <= 0 || index >= MainWindowUI::modLoaderNoneIndex()) continue;
                item->setData(MainWindowUI::loaderTreeKeyColumn(), MainWindowUI::rowSubTextRole(),
                              MainWindowUI::loaderConflictText(QStringLiteral("Fabric")));
                item->setDisabled(true);
                item->setExpanded(false);
            }
            std::printf("  安装页：加载器 %d 条，Fabric 底下 %d 个版本，选中「%s」\n",
                        loaders->topLevelItemCount(), fabric != nullptr ? fabric->childCount() : -1,
                        loaders->currentItem() != nullptr ? qPrintable(loaders->currentItem()->text(0)) : "无");
            const QRect treeAt = inWindow(loaders);
            std::printf("  安装页：加载器树 (%d,%d) %dx%d（可见 %d），安装钮 (%d,%d)\n", treeAt.x(), treeAt.y(),
                        treeAt.width(), treeAt.height(), int(loaders->isVisible()),
                        ui.getInstallButton() != nullptr ? inWindow(ui.getInstallButton()).center().x() : -1,
                        ui.getInstallButton() != nullptr ? inWindow(ui.getInstallButton()).center().y() : -1);
            if (ui.getInstallScroll() != nullptr)
                ui.getInstallScroll()->verticalScrollBar()->setValue(0);
        }
        shot(QStringLiteral("install-loader"));
    }

    // ---- 任务中心：两件完成 + 一件在跑 ----
    {
        ui.setTaskCenterContent([] { return siteTaskItems(); }, [] {});
        if (QPushButton *summary = ui.getTaskSummaryButton()) {
            summary->click();  // 底栏中间那格 → 任务中心浮层
            pump();
            // 浮层右上那颗「全部任务」才是任务中心整页（ActivityPopover::openAllIn 就是点它）。
            const bool opened = ActivityPopover::openAllIn(&window);
            pump();
            QWidget *panel = window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName());
            int rows = 0;
            while (window.findChild<QWidget *>(MainWindowUI::taskCenterPanelRowName(rows)) != nullptr) ++rows;
            const QRect at = inWindow(panel);
            std::printf("  任务中心：全部任务入口 %s，面板 %s (%d,%d) %dx%d，行 %d，%d 件任务\n",
                        opened ? "点到了" : "没找到",
                        panel != nullptr && panel->isVisible() ? "可见" : "没开", at.x(), at.y(), at.width(),
                        at.height(), rows, siteTaskItems().size());
            shot(QStringLiteral("install-tasks"));
            if (QWidget *open = window.findChild<QWidget *>(QStringLiteral("activityPopover"));
                open != nullptr && open->isVisible())
                open->close();
            if (ui.overlay() != nullptr && ui.overlay()->isOpen()) ui.overlay()->dismiss();
            pump();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // 收掉的浮层走 deleteLater
        }
    }

    // ---- Java 管理：识别到 3 个 Java ----
    {
        JavaManageDialog page;
        page.setRuntime(21, QStringLiteral("D:\\Games\\WTFML\\java\\jdk-21.0.5\\bin\\java.exe"));
        page.setRuntime(17, QStringLiteral("D:\\Games\\WTFML\\java\\jdk-17.0.13\\bin\\java.exe"));
        page.setRuntime(11, QStringLiteral("D:\\Games\\WTFML\\java\\jdk-11.0.25\\bin\\java.exe"));
        page.setSelection(true, QString());
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
        if (ui.overlay()->showPage(&page, page.titleText(), MainWindowUI::settingsTabIndex())) {
            int installed = 0;
            for (int major : JavaManageDialog::managedMajors())
                if (page.isInstalled(major)) ++installed;
            std::printf("  Java 管理：%d/%d 行有路径，页 (%d,%d) %dx%d\n", installed,
                        int(JavaManageDialog::managedMajors().size()), inWindow(&page).x(), inWindow(&page).y(),
                        page.width(), page.height());
            shot(QStringLiteral("install-java"));
            ui.overlay()->closeOverlay();
            pump();
        }
    }

    // ---- 导入整合包：检查页（版本 / 加载器 / 文件数 / 装到哪 + 一条冲突提示）----
    // 真启动器里这颗按钮在下载 › 整合包（和实例页左栏）上，点了之后确认框仍压在**当前那一页**上。
    {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
        ui.getDownloadCategoryList()->setCurrentRow(MainWindowUI::downloadCategoryModpackIndex());
        ui.setDownloadHeaderRow(MainWindowUI::downloadCategoryModpackIndex());
        ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryModpackIndex());
        fillSiteModpackTree(ui);
        pump();
        std::printf("  导入底页：下载 › 整合包，%d 个包\n",
                    ui.getDownloadResourceTree(QStringLiteral("modpack")) != nullptr
                        ? ui.getDownloadResourceTree(QStringLiteral("modpack"))->topLevelItemCount()
                        : -1);
        InlineMessageBox box(
            MessageKind::Question, QStringLiteral("开始导入"),
            QStringLiteral("整合包：Fabulously Optimized 6.1\nMinecraft：1.21.1 + Fabric 0.16.9\n"
                           "要下载的文件：87 个\n装到：D:\\Games\\WTFML\\instances\\光影与优化-1.21.1\n\n"
                           "开始导入吗？"),
            {QStringLiteral("是"), QStringLiteral("否")}, 0, -1, &window);
        box.setNote(QStringLiteral("冲突：包里自带 OptiFine 兼容补丁，与 Sodium 不能同时启用 —— "
                                   "导入后会在「模组」页把冲突的那一份标出来，不会删除。"));
        QTimer::singleShot(200, &box, [&] {
            pump();
            std::printf("  导入检查：提示「%s」%dx%d，备注 %d 字\n",
                        qPrintable(box.findChild<QLabel *>(InlineMessageBox::titleLabelName()) != nullptr
                                       ? box.findChild<QLabel *>(InlineMessageBox::titleLabelName())->text()
                                       : QStringLiteral("?")),
                        box.width(), box.height(), box.noteText().size());
            if (QTreeWidget *packs = ui.getDownloadResourceTree(QStringLiteral("modpack")))
                std::printf("  pack-import 底页：整合包表可见 %d，%d 行\n", int(packs->isVisible()),
                            packs->topLevelItemCount());
            shot(QStringLiteral("pack-import"));
            box.done(1);
        });
        ui.overlay()->execPrompt(&box);
        pump();
    }

    // ---- 实例设置：模组 / 存档 / 版本升级 ----
    {
        // 实例目录用**通用演示路径**（D:\Games\WTFML\…）：存档页页头会把这条路径原样写出来，
        // 用临时目录的话截图里会出现本机工作树的路径。D 盘上没有 Games 才这么摆，收工删掉。
        QTemporaryDir fallbackRoot(QDir::current().filePath(QStringLiteral(".review-site-XXXXXX")));
        const bool genericRoot = !QFileInfo::exists(QStringLiteral("D:/Games"));
        const QString root = genericRoot ? QStringLiteral("D:/Games/WTFML/instances/光影与优化-1.21.1")
                                         : fallbackRoot.path();
        if (!genericRoot && !fallbackRoot.isValid()) return 2;
        QDir().mkpath(root);
        makeSiteInstanceFixture(root);
        using Dlg = InstanceSettingsDialog;
        Dlg::GlobalValues global;
        global.javaPath = QStringLiteral("D:\\Games\\WTFML\\java\\jdk-21.0.5\\bin\\java.exe");
        global.xmx = 4096;
        global.xms = 1024;
        global.autoMemory = false;
        Dlg dialog(QStringLiteral("光影与优化-1.21.1"), QStringLiteral("光影与优化-1.21.1"), Dlg::Override(),
                   global, QStringList(), false);
        QHash<QString, QString> dirs;
        dirs.insert(QStringLiteral("mod"), root + QStringLiteral("/mods"));
        dirs.insert(QStringLiteral("resourcepack"), root + QStringLiteral("/resourcepacks"));
        dirs.insert(QStringLiteral("shader"), root + QStringLiteral("/shaderpacks"));
        dirs.insert(QStringLiteral("save"), root + QStringLiteral("/saves"));
        dirs.insert(QStringLiteral("logs"), root + QStringLiteral("/logs"));
        dirs.insert(QStringLiteral("screenshots"), root + QStringLiteral("/screenshots"));
        dirs.insert(QStringLiteral("game"), root + QStringLiteral("/game"));
        dialog.setInstanceDir(root);
        dialog.setFileDirs(dirs);
        dialog.setLastLaunch(QDateTime::currentDateTime().addSecs(-3600 * 5));
        dialog.setIcon(VersionIcons::iconCandidateKeys().value(0), VersionIcons::grassBlock(48));

        ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
        if (ui.overlay()->showPage(&dialog, QStringLiteral("实例设置"), MainWindowUI::instancesTabIndex())) {
            pump();
            std::printf("  实例设置：对话框 (%d,%d) %dx%d\n", inWindow(&dialog).x(), inWindow(&dialog).y(),
                        dialog.width(), dialog.height());
            dialog.showPage(QStringLiteral("mod"));
            pump();
            if (InstanceFilesPage *page = dialog.filesPage(QStringLiteral("mod")); page != nullptr) {
                std::printf("  实例·模组：%d 行\n", page->list() != nullptr ? page->list()->count() : -1);
            }
            shot(QStringLiteral("inst-mods"));

            dialog.showPage(QStringLiteral("save"));
            pump();
            if (InstanceFilesPage *page = dialog.filesPage(QStringLiteral("save")); page != nullptr) {
                std::printf("  实例·存档：%d 行\n", page->list() != nullptr ? page->list()->count() : -1);
            }
            for (QLabel *label : window.findChildren<QLabel *>())
                if (label->isVisible() && label->text().contains(QStringLiteral("Games")))
                    std::printf("  存档页路径：%s\n", qPrintable(label->text()));
            shot(QStringLiteral("inst-saves"));

            // 版本升级：另存副本 / 原地升级两颗按钮 + 快照与校验的说明。
            dialog.setUpgradeContext(QStringLiteral("当前实例：光影与优化-1.21.1\nMinecraft 1.21.1 / Fabric 0.16.9"));
            dialog.setTransferTargets(QStringLiteral("upgrade"),
                                      {{QStringLiteral("Minecraft 1.21.4（未安装）"), QStringLiteral("1.21.4")},
                                       {QStringLiteral("Minecraft 1.21.1（当前）"), QStringLiteral("1.21.1")}});
            dialog.setTransferPlan(
                QStringLiteral("upgrade"),
                QStringLiteral("升级副本计划：游戏版本不同\n\n"
                               "将创建一份升级副本，并把选中的 8 项（1,284 个文件 / 2.6 GB）搬进去。\n\n"
                               "只增加文件，不删除、不覆盖目标里已有的同名文件。\n\n"
                               "兼容性结论：游戏版本不同\n\n"
                               "原地升级会先创建完整 ZIP 快照并逐文件校验，确认后才替换；"
                               "原目录另行保留，可回滚。"));
            dialog.setTransferItems(
                QStringLiteral("upgrade"),
                {{QStringLiteral("存档 · 3 个世界"), QStringLiteral("saves")},
                 {QStringLiteral("模组 · 16 个"), QStringLiteral("mods")},
                 {QStringLiteral("config 与 defaultconfigs"), QStringLiteral("config")},
                 {QStringLiteral("键位、视频、声音"), QStringLiteral("options")},
                 {QStringLiteral("资源包 · 2 份"), QStringLiteral("resourcepacks")},
                 {QStringLiteral("光影 · 2 份"), QStringLiteral("shaderpacks")},
                 {QStringLiteral("截图 · 1 张"), QStringLiteral("screenshots")},
                 {QStringLiteral("结构文件"), QStringLiteral("schematics")},
                 {QStringLiteral("服务器列表 · 3 条"), QStringLiteral("servers")}},
                {QStringLiteral("saves"), QStringLiteral("mods"), QStringLiteral("config"),
                 QStringLiteral("options"), QStringLiteral("resourcepacks"), QStringLiteral("shaderpacks"),
                 QStringLiteral("servers")});
            dialog.setUpgradeModChecks(
                {{QStringLiteral("Sodium 0.6.13"), QStringLiteral("可沿用"), QStringLiteral("声明支持 1.21.4")},
                 {QStringLiteral("Iris 1.8.4"), QStringLiteral("需要替换"), QStringLiteral("平台提供 1.21.4 适配版"),
                  QStringLiteral("iris-fabric-1.8.4.jar"), QString(), QStringLiteral("replaceable"),
                  QStringLiteral("1.8.6")},
                 {QStringLiteral("Create 6.0.6"), QStringLiteral("不兼容"), QStringLiteral("Forge 专用"),
                  QStringLiteral("create-6.0.6.jar"), QString(), QStringLiteral("incompatible")},
                 {QStringLiteral("JEI 19.27.0"), QStringLiteral("未知"), QStringLiteral("模组未声明支持的版本"),
                  QStringLiteral("jei.jar"), QString(), QStringLiteral("unknown")}},
                QStringLiteral("模组声明：11 个可沿用，1 个需要替换，1 个不兼容，1 个未知。"));
            dialog.showPage(QStringLiteral("upgrade"));
            pump();
            if (auto *combo = dialog.findChild<QComboBox *>(Dlg::transferTargetComboName(QStringLiteral("upgrade")))) {
                combo->setCurrentIndex(1);
            }
            pump();
            std::printf("  实例·升级：目标下拉 %d 项，搬运清单 %d 项\n",
                        dialog.findChild<QComboBox *>(Dlg::transferTargetComboName(QStringLiteral("upgrade"))) != nullptr
                            ? dialog.findChild<QComboBox *>(Dlg::transferTargetComboName(QStringLiteral("upgrade")))
                                  ->count()
                            : -1,
                        dialog.findChild<QListWidget *>(Dlg::transferItemsListName(QStringLiteral("upgrade"))) != nullptr
                            ? dialog.findChild<QListWidget *>(Dlg::transferItemsListName(QStringLiteral("upgrade")))
                                  ->count()
                            : -1);
            shot(QStringLiteral("inst-upgrade"));

            // 导出整合包：产品里从实例设置 ›「备份与操作」那颗「导出整合包…」发起，选完文件才出
            // 这一张确认 —— 底页就是那一页（不是主页 / 设置页）。
            dialog.showPage(QStringLiteral("operationsmenu"));
            pump();
            if (QPushButton *exportButton = dialog.findChild<QPushButton *>(Dlg::exportButtonName())) {
                const QRect at = inWindow(exportButton);
                std::printf("  导出底页：实例设置 › 备份与操作，导出钮 (%d,%d) %dx%d，可见 %d\n", at.x(), at.y(),
                            at.width(), at.height(), int(exportButton->isVisible()));
            }
            {
                InlineMessageBox box(
                    MessageKind::Question, QStringLiteral("继续导出吗"),
                    QStringLiteral("3 个文件在 Modrinth 与 CurseForge 都没查到出处：\n"
                                   "mods/optifine-compat-1.0.3.jar\nmods/自定义合成表.jar\nconfig/rare-recipes.json\n\n"
                                   "继续的话，认不出出处的文件会原样打进包里：别人装上照样能用，"
                                   "只是启动器不能替他单独更新或者去重。"),
                    {QStringLiteral("继续导出"), QStringLiteral("取消")}, 0, -1, &window);
                QTimer::singleShot(200, &box, [&] {
                    pump();
                    std::printf("  导出确认：提示「%s」%dx%d\n",
                                qPrintable(box.findChild<QLabel *>(InlineMessageBox::titleLabelName()) != nullptr
                                               ? box.findChild<QLabel *>(InlineMessageBox::titleLabelName())->text()
                                               : QStringLiteral("?")),
                                box.width(), box.height());
                    if (QPushButton *exportButton =
                            dialog.findChild<QPushButton *>(Dlg::exportButtonName())) {
                        const QRect at = inWindow(exportButton);
                        std::printf("  pack-export 底页：导出钮 (%d,%d) %dx%d，可见 %d\n", at.x(), at.y(),
                                    at.width(), at.height(), int(exportButton->isVisible()));
                    }
                    shot(QStringLiteral("pack-export"));
                    box.done(1);
                });
                ui.overlay()->execPrompt(&box);
                pump();
            }

            ui.overlay()->closeOverlay();
            pump();
        } else {
            std::printf("  SKIP 实例设置：覆盖层没开出来\n");
        }
        if (genericRoot) {
            // 只收自己摆的这一份，再把空出来的三级目录顺手 rmdir 掉（非空时 rmdir 什么都不做）。
            QDir(root).removeRecursively();
            QDir().rmdir(QStringLiteral("D:/Games/WTFML/instances"));
            QDir().rmdir(QStringLiteral("D:/Games/WTFML"));
            QDir().rmdir(QStringLiteral("D:/Games"));
        }
    }

    // ---- 服务端：控制台 / server.properties / 玩家名单 ----
    {
        const QString raw = QStringLiteral(
            "#Minecraft server properties\n"
            "motd=欢迎来到 Steve 的周末生存服\n"
            "server-port=25565\n"
            "max-players=20\n"
            "online-mode=true\n"
            "white-list=true\n"
            "difficulty=easy\n"
            "gamemode=survival\n"
            "pvp=true\n"
            "level-name=world\n"
            "level-seed=\n"
            "spawn-protection=16\n"
            "view-distance=10\n"
            "simulation-distance=8\n"
            "player-idle-timeout=0\n");
        const auto properties = ServerModel::parseProperties(raw);

        ServerConsolePage console;
        console.setServerName(QStringLiteral("周末生存服"));
        console.setStatusText(QStringLiteral("运行中"));
        console.setStopAvailable(true);
        console.setForceKillAvailable(false);
        console.setCommandAvailable(true);
        console.setMaxLines(2000);
        console.setLog(siteServerLog());
        if (ui.overlay()->showPage(&console, ServerConsolePage::windowTitle(),
                                   MainWindowUI::instancesTabIndex())) {
            pump();
            std::printf("  服务端控制台：日志 %d 行（可见 %d 行），页 (%d,%d) %dx%d\n", console.logLines().size(),
                        console.visibleLogLines().size(), inWindow(&console).x(), inWindow(&console).y(),
                        console.width(), console.height());
            shot(QStringLiteral("server-console"));
            ui.overlay()->closeOverlay();
            pump();
        }

        ServerSettingsPage settingsPage;
        settingsPage.setServerName(QStringLiteral("周末生存服"));
        settingsPage.setStatusText(QStringLiteral("已停止"));
        settingsPage.setEditable(true, QString());
        settingsPage.setFields(siteServerFields());
        settingsPage.setValues(ServerPropertiesForm::valuesOf(properties, ServerPropertiesForm::commonFields()));
        settingsPage.setRawText(raw);
        settingsPage.setSection(ServerSettingsPage::Section::Common);
        if (ui.overlay()->showPage(&settingsPage, ServerSettingsPage::windowTitle(),
                                   MainWindowUI::instancesTabIndex())) {
            pump();
            std::printf("  server.properties：常用字段 %d 项，原文 %d 字符，页 (%d,%d) %dx%d\n",
                        ServerPropertiesForm::commonFields().size(), settingsPage.rawText().size(),
                        inWindow(&settingsPage).x(), inWindow(&settingsPage).y(), settingsPage.width(),
                        settingsPage.height());
            shot(QStringLiteral("server-props"));

            settingsPage.setSection(ServerSettingsPage::Section::Players);
            settingsPage.setPlayerList(0);
            settingsPage.setPlayerRows(
                {{QStringLiteral("069a79f4-44e9-4726-a5be-fca90e38aaf5"), QStringLiteral("Steve"),
                  QStringLiteral("069a79f4-44e9-4726-a5be-fca90e38aaf5"), QStringLiteral("正版账号"),
                  QStringLiteral("Steve")},
                 {QStringLiteral("ec561538-f3fd-461d-aff5-086b22154bce"), QStringLiteral("Alex"),
                  QStringLiteral("ec561538-f3fd-461d-aff5-086b22154bce"), QStringLiteral("正版账号"),
                  QStringLiteral("Alex")},
                 {QStringLiteral("b7c4a1de-7c1b-3c1e-9a0f-2a5e6b9d4c11"), QStringLiteral("建筑小工"),
                  QStringLiteral("b7c4a1de-7c1b-3c1e-9a0f-2a5e6b9d4c11"), QStringLiteral("离线账号"),
                  QStringLiteral("建筑小工")}});
            settingsPage.setPlayerNote(
                MainWindowUI::serverSettingsPlayerNoteText(0, /*running=*/false));
            pump();
            if (auto *tree = settingsPage.findChild<QTreeWidget *>(
                    MainWindowUI::serverSettingsPlayerTreeName())) {
                std::printf("  玩家名单：%d 行\n", tree->topLevelItemCount());
            }
            shot(QStringLiteral("server-players"));
            ui.overlay()->closeOverlay();
            pump();
        }
    }

    // ---- 智能体：会话关联「光影与优化-1.21.1」，写文件等你批准 ----
    {
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::agentTabIndex());
        if (AgentPage *agent = ui.getAgentPage()) {
            agent->setConfigured(true, QStringLiteral("已连接 · OpenAI 兼容"));
            agent->setInstanceChip(QStringLiteral("光影与优化-1.21.1"), false);
            AgentView::SessionMeta session;
            session.id = QStringLiteral("s-site");
            session.title = QStringLiteral("把渲染距离调到 12");
            session.updatedAt = QDateTime::currentDateTime().addSecs(-120);
            session.encrypted = true;
            agent->setSessions({session});
            agent->setCurrentSession(QStringLiteral("s-site"));
            agent->setTitle(QStringLiteral("把渲染距离调到 12"));
            AgentView::Message ask;
            ask.id = QStringLiteral("m1");
            ask.user = true;
            ask.content = QStringLiteral("光影与优化-1.21.1 有点卡，帮我把渲染距离调到 12，顺便开平滑光照。");
            AgentView::Message answer;
            answer.id = QStringLiteral("m2");
            answer.content = QStringLiteral(
                "先看看 `options.txt` 现在写的是什么。\n"
                "里面 `renderDistance:8`、`ao:false`，改这两项就行；写入前会备份成 `options.txt.bak`。");
            agent->setMessages({ask, answer});
            AgentView::ToolCard card;
            card.id = QStringLiteral("t1");
            card.title = QStringLiteral("读取 options.txt");
            card.summary = QStringLiteral("读到 42 项设置");
            card.detail = QStringLiteral("renderDistance=8　ao=false");
            card.status = AgentView::ToolCardStatus::Done;
            agent->setToolCards({card});
            agent->showEmptyState(false);
            agent->showApproval(siteApprovalRequest());
            pump();
            // 审批卡插在对话流末尾，而 `scrollToLatest()` 是 singleShot(0) 排的：排上那一刻
            // 布局还没把卡片量完，滚动条最大值是旧的。这里等布局落定后再贴到底，多发几次
            // 是让最后一行的边距也进来。
            QScrollArea *scroll = window.findChild<QScrollArea *>(QStringLiteral("agentMessageScroll"));
            if (scroll != nullptr) {
                for (int i = 0; i < 4; ++i) {
                    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                    pump();
                }
            }
            AgentApprovalCard *approval = agent->approvalCard();
            if (scroll != nullptr) {
                const QRect viewport = inWindow(scroll->viewport());
                std::printf("  对话滚动：viewport (%d,%d) %dx%d，滚动 %d/%d\n", viewport.x(), viewport.y(),
                            viewport.width(), viewport.height(), scroll->verticalScrollBar()->value(),
                            scroll->verticalScrollBar()->maximum());
            }
            if (QWidget *composer = window.findChild<QWidget *>(QStringLiteral("agentComposerRow"))) {
                const QRect at = inWindow(composer);
                std::printf("  输入岛：(%d,%d) %dx%d\n", at.x(), at.y(), at.width(), at.height());
            }
            std::printf("  智能体：审批卡 %s (%d,%d) %dx%d\n",
                        approval != nullptr && approval->isVisible() ? "可见" : "没露",
                        approval != nullptr ? inWindow(approval).x() : -1,
                        approval != nullptr ? inWindow(approval).y() : -1,
                        approval != nullptr ? approval->width() : -1,
                        approval != nullptr ? approval->height() : -1);
            if (approval != nullptr) {
                if (auto *preview = approval->findChild<QPlainTextEdit *>(
                        QStringLiteral("agentEgressPreviewCard"))) {
                    std::printf("  审批卡 diff 第一行：%s\n",
                                qPrintable(preview->toPlainText().section(QLatin1Char('\n'), 0, 0)));
                }
                for (const char *name : {"agentApprovalDeny", "agentApprovalAllow"}) {
                    if (auto *button = approval->findChild<QPushButton *>(QString::fromLatin1(name))) {
                        const QRect at = inWindow(button);
                        std::printf("  审批钮 %s：(%d,%d) %dx%d，可见 %d\n", name, at.x(), at.y(), at.width(),
                                    at.height(), int(button->isVisible()));
                    }
                }
            }
            shot(QStringLiteral("agent-confirm"));
            agent->hideApproval();
            pump();
        }
    }

    // ---- 工具箱：附魔顺序 / 旗帜设计 / 坐标换算 ----
    {
        ui.openToolboxTab();
        QWidget *host = ui.toolboxTab();
        if (host != nullptr && host->layout() != nullptr) {
            auto *toolbox = new ToolboxPage(host);
            host->layout()->addWidget(toolbox);
            wireToolboxSamples(*toolbox);
            toolbox->showTool(ToolboxPage::Tool::Anvil);
            pump();
            std::printf("  附魔顺序：%d 行输入，结果区 %s\n", toolbox->anvilRows().size(),
                        toolbox->hasTool(ToolboxPage::Tool::Anvil) ? "有" : "无");
            reportText(QStringLiteral("合计 14 级"), QStringLiteral("附魔结果"));
            shot(QStringLiteral("tool-enchant"));

            toolbox->showTool(ToolboxPage::Tool::Banner);
            pump();
            reportText(QStringLiteral("白色羊毛"), QStringLiteral("旗帜结果"));
            shot(QStringLiteral("tool-banner"));

            toolbox->showTool(ToolboxPage::Tool::Coord);
            toolbox->setCoordinates(0, -123.5, 71.0, 208.25);
            toolbox->setTeleportParse(true, QStringLiteral("已识别：主世界 (-123.5, 71, 208.25)"));
            toolbox->setTeleportCommand(QStringLiteral("/tp Steve -123.5 71 208.25"));
            pump();
            reportText(QStringLiteral("下界：x"), QStringLiteral("坐标结果"));
            shot(QStringLiteral("tool-coords"));
        }
    }

    std::printf("\nsite shots: %d saved -> %s（窗 %dx%d，DPR %.0f）\n", saved, qPrintable(options.out),
                window.width(), window.height(), window.devicePixelRatioF());
    return saved > 0 ? 0 : 1;
}

int runLegacy(QApplication &app, const QString &out) {
    Theme::setSpec(Theme::load());
    QDir().mkpath(out);
    int saved = 0;
    // 第一趟是**新安装**：这个进程用的是临时设置目录（见调用方），盘上一项主题都没有，
    // 所以 `Theme::load()` 拿到的就是出厂那一份 —— 顺带把预设 / 主题色 / 窗口底色打出来。
    for (const QString &id : {QStringLiteral("default"), QStringLiteral("dark"),
                              QStringLiteral("emerald"), QStringLiteral("ocean")}) {
        if (id == QLatin1String("default")) {
            Theme::setSpec(Theme::load());
        } else {
            Theme::setSpec(Theme::presetSpec(id));
        }
        Theme::apply(app);
        if (id == QLatin1String("default")) {
            std::printf("new install (no settings): preset=%s accent=%s window-bg=%s\n",
                        qPrintable(Theme::current().presetId),
                        qPrintable(Theme::current().accent.name()),
                        qPrintable(app.palette().color(QPalette::Window).name()));
        }
        QMainWindow window;
        MainWindowUI ui(&window);
        window.resize(1127 * 4 / 5 + 100, 776);
        window.show();

        const auto fill = [](QComboBox *combo, const QStringList &items, int current = 0) {
            if (combo == nullptr) return;
            combo->clear();
            combo->addItems(items);
            combo->setCurrentIndex(current);
        };
        fill(ui.getDownloadSourceCombo(), {QStringLiteral("优先官方源"), QStringLiteral("优先国内源")});
        fill(ui.getMemoryModeCombo(), {QStringLiteral("自动分配"), QStringLiteral("手动分配")}, 1);
        fill(ui.getGameWindowModeCombo(), {QStringLiteral("窗口"), QStringLiteral("最大化"), QStringLiteral("全屏")});
        if (ui.getJavaPathLabel() != nullptr) ui.getJavaPathLabel()->setText(QStringLiteral("自动选择　按游戏版本"));
        if (ui.getMemoryHint() != nullptr)
            ui.getMemoryHint()->setText(QStringLiteral("系统总内存 31.5 GB，当前可用 14.5 GB；最小内存由启动器设为 1024 MB。"));
        if (ui.getMemoryMaxSpin() != nullptr) ui.getMemoryMaxSpin()->setValue(4096);
        if (ui.getMemoryMaxSlider() != nullptr) ui.getMemoryMaxSlider()->setValueMb(4096);

        MainWindowUI::HomeQuickItem none;
        none.key = QStringLiteral("none");
        none.name = QStringLiteral("还没有实例");
        none.empty = true;
        ui.setHomeQuickRows({none}, {}, {});

        const auto shot = [&](const QString &name) {
            app.processEvents();
            app.processEvents();
            if (window.grab().save(QDir(out).filePath(QStringLiteral("%1-%2.png").arg(name, id)))) ++saved;
        };
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
        shot(QStringLiteral("home-empty"));
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
        if (auto *list = window.findChild<QTreeWidget *>(MainWindowUI::instanceListName())) list->hide();
        if (auto *empty = window.findChild<QWidget *>(MainWindowUI::instanceEmptyStateName())) empty->show();
        shot(QStringLiteral("manage-empty"));
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
        ui.getSettingsTabs()->setCurrentIndex(0);
        shot(QStringLiteral("settings-launch"));
        ui.getSettingsTabs()->setCurrentIndex(
            MainWindowUI::settingsCategoryKeys().indexOf(QStringLiteral("download")));
        shot(QStringLiteral("settings-download"));
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
        shot(QStringLiteral("download-source"));
    }
    std::printf("saved %d shots to %s\n", saved, qPrintable(out));
    return saved > 0 ? 0 : 1;
}

}  // namespace

int UiReviewShotsCheck(QApplication &app) {
    Anim::setEnabled(false);
    QTemporaryDir settingsDir(QDir::current().filePath(QStringLiteral(".review-shots-XXXXXX")));
    if (!settingsDir.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("WTFMLReviewShots"));
    app.setApplicationName(QStringLiteral("ReviewShots"));
    app.setStyle(Theme::createStyle());
    installShotFonts(app);

    ShotOptions options;
    options.out = QDir::current().filePath(QStringLiteral("build/review-shots"));
    QStringList positional;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        const auto valueOf = [&](const QString &prefix) {
            return arg.startsWith(prefix) ? arg.mid(prefix.size()) : QString();
        };
        if (arg == QLatin1String("--extended")) {
            options.extended = true;
        } else if (arg == QLatin1String("--popups")) {
            options.popups = true;
        } else if (arg == QLatin1String("--loading")) {
            options.loading = true;
            options.extended = true;
        } else if (arg == QLatin1String("--files")) {
            options.files = true;
            options.extended = true;
        } else if (arg == QLatin1String("--r151")) {
            options.r151 = true;
            options.extended = true;
        } else if (arg == QLatin1String("--r155")) {
            options.r155 = true;
            options.extended = true;
        } else if (arg == QLatin1String("--r157")) {
            options.r157 = true;
            options.extended = true;
        } else if (arg == QLatin1String("--i85")) {
            options.i85 = true;
            options.extended = true;
        } else if (arg == QLatin1String("--r179c")) {
            options.r179c = true;
            options.extended = true;
        } else if (arg == QLatin1String("--r70kbd")) {
            options.r70kbd = true;
            options.extended = true;
        } else if (arg == QLatin1String("--site")) {
            options.site = true;
            options.extended = true;
        } else if (arg == QLatin1String("--first-disable")) {
            options.firstDisable = true;
        } else if (arg == QLatin1String("--agent-polish")) {
            options.agentPolish = true;
        } else if (arg.startsWith(QLatin1String("--theme="))) {
            options.theme = valueOf(QStringLiteral("--theme="));
        } else if (arg == QLatin1String("--theme") && i + 1 < args.size()) {
            options.theme = args.at(++i);
        } else if (arg.startsWith(QLatin1String("--size="))) {
            const QStringList parts = valueOf(QStringLiteral("--size=")).split(QLatin1Char('x'));
            if (parts.size() == 2) options.size = QSize(parts.at(0).toInt(), parts.at(1).toInt());
        } else if (arg == QLatin1String("--size") && i + 1 < args.size()) {
            const QStringList parts = args.at(++i).split(QLatin1Char('x'));
            if (parts.size() == 2) options.size = QSize(parts.at(0).toInt(), parts.at(1).toInt());
        } else if (arg.startsWith(QLatin1String("--background="))) {
            options.background = valueOf(QStringLiteral("--background="));
        } else if (arg == QLatin1String("--background") && i + 1 < args.size()) {
            options.background = args.at(++i);
        } else if (arg.startsWith(QLatin1String("--out="))) {
            options.out = valueOf(QStringLiteral("--out="));
        } else if (arg == QLatin1String("--out") && i + 1 < args.size()) {
            options.out = args.at(++i);
        } else if (!arg.startsWith(QLatin1Char('-'))) {
            positional.append(arg);
        }
    }
    // 不带参数时仍是老行为：第一个位置参数是输出目录。
    if (!positional.isEmpty() && options.out.endsWith(QStringLiteral("build/review-shots"))) {
        options.out = positional.first();
    }
    if (options.agentPolish) {
        if (options.theme.isEmpty()) options.theme = QStringLiteral("dark");
        if (!options.size.isValid() || options.size.width() <= 0) options.size = QSize(1280, 800);
        return runAgentPolish(app, options);
    }
    if (options.popups) {
        if (options.theme.isEmpty()) options.theme = QStringLiteral("dark");
        if (!options.size.isValid() || options.size.width() <= 0) options.size = QSize(1280, 800);
        return runPopups(app, options);
    }
    if (options.site) {
        if (options.theme.isEmpty()) options.theme = QStringLiteral("dark");
        if (!options.size.isValid() || options.size.width() <= 0) options.size = QSize(1120, 760);
        return runSite(app, options);
    }
    if (options.extended) {
        if (options.theme.isEmpty()) options.theme = QStringLiteral("dark");
        if (!options.size.isValid() || options.size.width() <= 0 || options.size.height() <= 0)
            options.size = QSize(1280, 800);
        return runExtended(app, options);
    }
    return runLegacy(app, options.out);
}

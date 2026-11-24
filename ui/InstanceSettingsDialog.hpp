/**
 * @file InstanceSettingsDialog.hpp
 * @brief 「实例设置…」对话框。**左栏是一列按实例现算的页表**（概览 / 启动
 * ＋ 存档 / 服务器列表 / 资源包 / 数据包 / 模组 / 光影包 / 原理图里"这个实例用得上
 * 且用户没勾掉"的那些，见 pinnedPageKeys() / optionalPageKeys()），右边一块堆叠页；
 * 要显示哪几页在概览页最下面那张勾选表里调（`settingsPages`，落在 wtfml.meta）。
 * @note 进参出参都是**值**，本文件不 include 任何 src/ 下的头 —— 和 GroupDialog 同一条
 * 约定（ui/ 不依赖业务层）。所以这里的 Override 是它自己的一份，和
 * src/settings/EffectiveJavaSettings.hpp 里的 JavaSettingsOverride 逐字段相同，
 * 由 MainWindow 在两边转换。
 *
 * 【哪五项能覆盖，别的为什么不能】
 *
 * 只做**启动五项**。其余设置项各有各的归属，混进来只会让"这个实例"变成一个说不清的概念：
 * - 下载源（Download/Source）：下载那一刻的决定，和"启动这个实例"无关；
 * - 启动后（Launcher/AfterLaunch）：那是启动器自己的行为，不是这个实例的属性；
 * - 显示名拼法：固定一份（`formatVersionName()`），不是这个实例的属性。
 *
 * 【约定：对话框只改自己手里那份值，不碰磁盘】
 *
 * **这一层一个字节都不写盘**：它把控件读成一份值（override() / appendJvmArgs() /
 * iconChoice() …）交给调用方。落盘那一步**只有一条路**：`MainWindow::applyInstanceSettings()`
 * 把这些值读成一份 `InstanceSettingsController::Edit`，再交给
 * `InstanceSettingsController::apply()`（主窗口那一层只剩
 * "读对话框"这个适配器和一个按 `Notice::Kind` 挑中文的地方）。
 *
 * 唯一的例外是**改名**：那一格动的是磁盘上的目录名，它走自己那条信号
 * （renameRequested()，回车 / 失焦才发），理由写在那个信号上面。
 *
 * 【约定：控件的值就是结果（没有内部影子状态）】
 *
 * override() **现场读控件**，不缓存一份 m_override。这样"摆好控件 → 读结果"在验证
 * 工程里可以被直接驱动，而不用先走一遍 exec()（模态窗在 offscreen 下驱动不了）。
 */
#pragma once

#include <QDialog>
#include <QCoreApplication>
#include <QDateTime>
#include <QFontMetrics>  // elidedServerAddress()
#include <QHash>
#include <QIcon>  // ServerRowStatus 里那一枚（服务器图标）
#include <QImage>  // 截图缩略图缓存（m_lifecycleThumbCache）
#include <QJsonObject>  // pageChoices() / setPageChoices()（"藏起来的那几页"）
#include <QList>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QSet>
#include <optional>

// `openConfigEditor()` 那个进参是**按值**要一份 `ConfigFileText::Loaded`（见 .cpp 里
// 那段说明：`encode()` 要靠它还原行尾和 BOM），所以这儿不能只前置声明。
#include "ConfigFileText.hpp"
// `openWorldSettings()` 的进参是 `WorldSettingsPage::WorldView` —— **嵌套类型**，
// 前置声明一个 `class WorldSettingsPage;` 是说不清它的，得把定义拿进来。
#include "WorldSettingsPage.hpp"
// `actionButtonHeight()` 报的是全应用动作按钮那一档（见那条注释）。
#include "Theme.hpp"

class ConfigFileEditorPage;
class InstanceFilesPage;
class MemorySlider;

class ElidedLabel;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QMimeData;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;
class ResourcePackPage;

class InstanceSettingsDialog : public QDialog {
    Q_OBJECT

public:
    ResourcePackPage *resourcePackPage() const {
        const_cast<InstanceSettingsDialog *>(this)->ensureFilePage(QStringLiteral("resourcepack"));
        return m_resourcePackPage;
    }
    /// 这个实例覆盖的那几项。没有值 = 跟随全局（空串也算有值，见 EffectiveJavaSettings.hpp）。
    ///
    /// NOTE: 「窗口模式」这一项存的是**代号字符串**（`windowMode` 那一格），不是枚举 ——
    /// 档位那个枚举（GameWindowMode）在 `src/settings/GameWindowSettings.hpp` 里，
    /// 而 `ui/` 不许 include `src/`（分层铁律）。这一页只认"标签 + 代号"这一对，
    /// 代号长什么样它不管；代号表由调用方喂进来（见构造函数那个 @c windowModes）。
    /// 于是这里既不需要一份枚举的拷贝，也不用在 ui/ 里再写一遍那三个串。
    struct Override {
        std::optional<QString> javaPath;
        std::optional<int> xmx;
        std::optional<int> xms;
        std::optional<bool> autoMemory;
        std::optional<QString> extraJvmArgs;
        std::optional<QString> windowMode;
        std::optional<int> windowWidth;
        std::optional<int> windowHeight;
    };

    /// 「窗口模式」那个下拉框里的一项：显示的字 + 存进 meta 的代号。
    ///
    /// 为什么是调用方喂而不是这一页自己写：那三个代号是**持久化格式的一部分**，唯一出处是
    /// `src/settings/GameWindowSettings.cpp` 里的 gameWindowModeToken()。在 ui/ 里再写一遍
    /// 就等于开第二个出处 —— 改了一处对不上时的症状是"选了档位、盘上没变"，不报任何错。
    struct WindowModeChoice {
        QString label;  ///< 下拉框上显示的字（调用方 tr 过）。
        QString token;  ///< 存进 wtfml.meta 的代号。
    };

    /// 全局那一层的值。只用来写「跟随全局（当前 …）」那句话 —— 用户要能看见自己
    /// 跟随的到底是什么数，否则"跟随全局"就是一格写不出内容的空白。
    struct GlobalValues {
        QString javaPath;
        int xmx = 0;
        int xms = 0;
        bool autoMemory = true;
        QString extraJvmArgs;
        /// 「最大内存」那一格允许拖到的上限（MB）—— 这台机器的物理内存，**由调用方算好
        /// 喂进来**。`ui/` 不许依赖 `src/`，这一页自己也读不到系统内存。
        ///
        /// NOTE: <= 0 表示调用方没给（验证工程里那些只验结构的构造点），退回内置的
        /// MainWindowUI::memoryLimitMaxMb()。**别**把 0 当成"这台机器没有内存"。
        int memoryCapMb = 0;
        /// 全局那一档「窗口模式」的**代号**（和 WindowModeChoice::token 同一个出处）。
        /// 它是这一项「跟随全局」时下拉框要摆出来的那一项 —— 不给的话摆第 0 项。
        QString windowMode;
        /// 全局那一档「窗口模式」的**显示名**（"小窗"/"最大化"/"全屏"）。只用来写
        /// 「跟随全局（当前 …）」那句话 —— 和 javaPath 那几项同一个道理：用户得能看见
        /// 自己跟随的到底是什么。空串 = 调用方没给（那句话说成"跟随全局"不带括号）。
        QString windowModeLabel;
        /// 全局的窗口大小。**它是"跟随全局"那一档下面那两个数字框的禁用值来源**吗？不是 ——
        /// 那两个框在「跟随全局」时被整格禁掉，值由全局那份决定；这两个数只用来填
        /// 「跟随全局（当前 854×480）」那一句。
        int windowWidth = 0;
        int windowHeight = 0;
    };

    /**
     * @brief 构造对话框。
     * @param instanceName 显示在标题里的实例名（用户认得出改的是哪一个）。
     * @param instanceId 这个实例**现在的目录名**（`versions/<它>`），也是「实例名」那一格
     * 的初值 —— 改了它就是要改名（见 instanceName()）。
     *
     * NOTE: 它和 instanceName 是**两个不同的东西**，都要传：显示名可能被用户改过、
     * 也可能被自定义名 / 加载器名拼成了别的东西，而目录名是磁盘上真正的身份。
     * 拿显示名当初值的话，用户点进设置页什么都没改、顺手改了别的某一格，实例就被改成了
     * 一个奇怪的名字。
     * @param current 这个实例**现在**的覆盖（wtfml.meta 里读出来的）。
     * @param global 全局有效值（GlobalJavaSettings::effective()），用来写「当前 …」。
     * @param appendJvmArgs 这个实例现有的「追加参数」（wtfml.meta 的 jvmArgs 数组）。
     * 它不是覆盖层的一项，和全局没有"跟随"关系，所以单独一行、没有档位下拉框。
     * @param warnJavaMismatch 这个实例的「Java 版本不符时提醒」。和 appendJvmArgs 一样
     * 不是覆盖层的一项（存 meta 的另一个键），所以也没有档位下拉框。
     * @param windowModes 「窗口模式」那个下拉框里的档位表（显示的字 + 存进 meta 的代号，
     * **顺序就是界面上的顺序**）。由调用方喂是分层的要求，理由见 WindowModeChoice
     * 上面那一段。空表 = 不建「窗口模式」这一行（验证工程里那些只验别的结构的构造点
     * 就传空表）—— 那时「窗口大小」那一行照样建，它不依赖这份表。
     * @param parent 父窗口。
     * @param embedded 作为内嵌页预构造时启用，parent 应为最终内容容器。
     */
    InstanceSettingsDialog(const QString &instanceName, const QString &instanceId,
                           const Override &current, const GlobalValues &global,
                           const QStringList &appendJvmArgs, bool warnJavaMismatch,
                           const QList<WindowModeChoice> &windowModes = QList<WindowModeChoice>(),
                           QWidget *parent = nullptr, bool embedded = false);

    /// 用户摆好的覆盖。每一项各自：那行选了「自定义」就有值，选了「跟随全局」就没有值。
    ///
    /// 「窗口大小」这一项有**两个**值（宽和高），但界面上是**一行**、共用
    /// 一个档位下拉框 —— 所以要么两个都有值、要么两个都没有。实现上就是"这一行选自定义时
    /// 两个框的值一起收，选跟随时两个都不收"（见 .cpp 里那一行）。**不会**出现只有宽
    /// 有值的情况。
    ///
    /// NOTE: 「内存分配」那一档整行不显示（它和最大内存说的是同一件事，只保留那个拖动的）
    /// ⇒ 这里**不产出 autoMemory** 覆盖。`Override::autoMemory` 字段还在（老数据、
    /// 老测试要用），只是这一页永远不填它；读侧 `resolveJavaSettings()` 也当它不存在。
    Override override() const;

    /// 高级选项里那两行之一：**追加**在命令行最后的 JVM 参数（写进 meta 的 jvmArgs，
    /// 是数组不是空格分隔的串 —— 参数本身可能含空格）。
    QStringList appendJvmArgs() const;

    /// 高级选项里那两行之一：这个实例的「Java 版本不符时提醒」。true = 提醒（默认）。
    ///
    /// NOTE: **刻意不进 Override**。那五项是"值覆盖"，每一项都能"跟随全局"；这是一个
    /// 行为开关，存的是 wtfml.meta 的另一个键（skipJavaVersionWarning），由
    /// MainWindow 经 updateVersionMeta() 合并写进去。混进 Override 会让
    /// isEmpty()、javaOverrideFromJson/ToJson 都跟着改，而那五项和那个键的
    /// 生命周期本来就不一样。
    bool warnJavaMismatch() const;

    /// 高级选项那一块现在是不是展开着。默认折叠。
    bool advancedExpanded() const;

    /// 用户在「实例名」那一格里现在写着什么（**没有** trim）。它和构造进来的 instanceId
    /// 不一样时 = 用户要改名；一样（或者只有空格差别）时 = 不改名。
    ///
    /// NOTE: **改名这件事不在这里做** —— 这一层只把控件读成一份值（和 override() 一样），
    /// 真的改目录在 MainWindow::openInstanceSettings() 那一段里（先改名再写设置）。
    /// 本文件不 include 任何 src/ 下的头，也就够不着 renameInstance()。
    QString instanceName() const;

    /// 自动内存是不是**有效**开着。这一页没有「内存分配」那一项，所以只看全局那份
    /// （见 .cpp 里 effectiveAutoMemory()）。
    /// 界面用它决定下面两个数字框禁不禁用；验证工程用它当"这个实例这次是自动还是手动"
    /// 的判据。
    bool effectiveAutoMemory() const;

    /// 刷新每一行下面那句话，以及两行内存的可用性（自动档下整行禁用）。
    /// 每次某个档位下拉框变化都会调它。
    void refreshHints();

    // ---------- 左栏那一列页 ----------
    //
    // 左栏只剩一列，摆的是**按这个实例现算出来**的一份页表（`shownEntryKeys()`）：
    //
    // 概览 启动 ← 钉死的那两条（`pinnedPageKeys()`），关不掉
    // 存档 服务器列表 资源包 数据包 模组 光影包 原理图 ← `optionalPageKeys()` 的顺序，
    // 可用 + 没被用户勾掉才出现
    //
    // 右栏还是那份堆叠页，顺序按 `filePageKeys()`（见 `pageCount()`）；左栏的先后与堆叠页
    // 的先后**互不相干** —— 跳页一律按**键**（`stackPageIndexOf()`），不按行号。
    //
    // 用户能调的是"显示哪些页"：概览页最下面那张勾选表（`pageCheckName()`）；勾掉哪几页记在
    // 这个实例自己的 wtfml.meta 里（`settingsPages`，见 `pageChoices()` 与 setPageChoices()）。
    //
    // 键表与 objectName 的写法照抄设置页（settingsCategoryKeys / settingsPanelName）：
    // **身份走 objectName**，验证工程不靠位置认控件。

    /// 左栏里**钉死的那两条**的键（顺序就是界面上从上到下的顺序）。
    ///
    /// 概览和启动**不能关**（「快捷方式」也是一页的时候同样不能关，但它不是一页了 ——
    /// 那几颗按钮并进了概览页，见 shortcutTexts()）。
    static QString overviewCategoryKey() { return QStringLiteral("overview"); }
    static QString launchCategoryKey() { return QStringLiteral("launch"); }
    static QStringList pinnedPageKeys() {
        return {overviewCategoryKey(), launchCategoryKey()};
    }
    static QStringList pinnedPageTexts() {
        return {QCoreApplication::translate("InstanceSettingsDialog", "概览"), QCoreApplication::translate("InstanceSettingsDialog", "启动")};
    }

    /// **能勾、能关**的那几页的键，顺序是"存档 服务器列表 资源包 数据包 …模组…光影…原理图"
    /// （默认展示的也是这个顺序）。
    ///
    /// NOTE: 这一份是 `filePageKeys()` 去掉「配置文件」之后的一个**排列**（同一批键、不同
    /// 次序），验证工程钉着这个等式。堆叠页那边的顺序（filePageKeys()）是另一件事：
    /// 它按"跳板在最前 / 往里放 / 从里面看"排过，且 `serverPageKey()` 那句"最后那一个"
    /// 还挂在它身上 —— 两边的次序**本来就该分**，别再拿其中一张去算另一张。
    static QStringList optionalPageKeys() {
        return {QStringLiteral("save"),    QStringLiteral("server"),  QStringLiteral("resourcepack"),
                QStringLiteral("datapack"), QStringLiteral("mod"),   QStringLiteral("shader"),
                QStringLiteral("schematic")};
    }
    static QStringList optionalPageTexts() {
        return {QCoreApplication::translate("InstanceSettingsDialog", "存档"), QCoreApplication::translate("InstanceSettingsDialog", "服务器列表"), QCoreApplication::translate("InstanceSettingsDialog", "资源包"),
                QCoreApplication::translate("InstanceSettingsDialog", "数据包"), QCoreApplication::translate("InstanceSettingsDialog", "模组"),   QCoreApplication::translate("InstanceSettingsDialog", "光影包"),
                QCoreApplication::translate("InstanceSettingsDialog", "结构/原理图")};
    }

    /// 第 @p key 那一页在界面上叫什么（**唯一出处**：左栏那一条的字、勾选框上的字、
    /// 报错里点名都用它）。认不出来的键回空串。
    static QString pageText(const QString &key);

    /// 堆叠页里那些**文件页**的键，**顺序就是堆叠页里的顺序**。
    ///
    /// NOTE: 左栏那一列的先后**不是**这个（见 optionalPageKeys()）：这一张是"哪几页存在、
    /// 在堆叠页里排第几"，那一张是"用户眼里从上到下怎么排"。
    ///
    /// NOTE: 「配置文件」这一页的用途是"在启动器里直接编辑模组的 config.json"。
    /// **插在原理图与存档之间**，不是为了好看：`serverPageKey()` 的
    /// 唯一出处是"最后那一个"，往尾巴上接会把它那句话变成假的。
    static QStringList filePageKeys() {
        return {QStringLiteral("mod"),    QStringLiteral("resourcepack"),
                QStringLiteral("datapack"), QStringLiteral("shader"), QStringLiteral("schematic"),
                QStringLiteral("config"), QStringLiteral("save"),   QStringLiteral("server")};
    }
    static QStringList filePageTexts() {
        return {QCoreApplication::translate("InstanceSettingsDialog", "模组"),   QCoreApplication::translate("InstanceSettingsDialog", "资源包"),
                QCoreApplication::translate("InstanceSettingsDialog", "数据包"), QCoreApplication::translate("InstanceSettingsDialog", "光影包"), QCoreApplication::translate("InstanceSettingsDialog", "结构/原理图"),
                QCoreApplication::translate("InstanceSettingsDialog", "配置文件"), QCoreApplication::translate("InstanceSettingsDialog", "存档"), QCoreApplication::translate("InstanceSettingsDialog", "服务器列表")};
    }

    /// 「配置文件」那一页的键。
    ///
    /// NOTE: 这一页**一个入口都没有** —— 它
    /// 不在 optionalPageKeys() 里、所以左栏和勾选表里都没有它，只有验证工程拿
    /// showPage() 够得着。要放出来：把它接进 optionalPageKeys()（并给一条默认关的
    /// 选择），别的地方一个字都不用改。
    static QString configPageKey() { return QStringLiteral("config"); }

    /// 「服务器列表」那一页的键（也就是 filePageKeys() 里最后那一个）。
    ///
    /// NOTE: **唯一出处** —— MainWindow 那两条接线（自动测速、以及"这一页里那两颗按钮分别
    /// 干什么"）都按它认页。写第二遍字面量的症状是**静默失效**：自动测速那一发
    /// 永远不发（少一句话、不报错），而且页还是照开的。
    static QString serverPageKey() { return QStringLiteral("server"); }

    /// 堆叠页里「文件页」那些页的起始下标（0/1 是概览与启动，排在前面）。
    /// 换言之 `stackIndex = filePageFirstIndex() + filePageKeys() 里的下标`。
    static int filePageFirstIndex() { return 2; }
    /// 堆叠页一共有几页。
    static QStringList lifecyclePageKeys() {
        return {QStringLiteral("worldbackup"), QStringLiteral("screenshots"),
                QStringLiteral("replay"), QStringLiteral("structure"),
                QStringLiteral("integrity"), QStringLiteral("repair"),
                QStringLiteral("logs"), QStringLiteral("crash"), QStringLiteral("diagnosis"),
                QStringLiteral("snapshots"), QStringLiteral("export"),
                QStringLiteral("migration"), QStringLiteral("upgrade"),
                QStringLiteral("integritymenu"), QStringLiteral("diagnosticsmenu"), QStringLiteral("operationsmenu")};
    }
    static QStringList lifecyclePageTexts() {
        return {QCoreApplication::translate("InstanceSettingsDialog", "世界备份"), QCoreApplication::translate("InstanceSettingsDialog", "截图"), QCoreApplication::translate("InstanceSettingsDialog", "回放"),
                QCoreApplication::translate("InstanceSettingsDialog", "结构"), QCoreApplication::translate("InstanceSettingsDialog", "完整性检查"), QCoreApplication::translate("InstanceSettingsDialog", "实例修复"),
                QCoreApplication::translate("InstanceSettingsDialog", "日志"), QCoreApplication::translate("InstanceSettingsDialog", "崩溃报告"), QCoreApplication::translate("InstanceSettingsDialog", "检查与诊断"),
                QCoreApplication::translate("InstanceSettingsDialog", "快照与恢复"), QCoreApplication::translate("InstanceSettingsDialog", "导出整合包"),
                QCoreApplication::translate("InstanceSettingsDialog", "实例迁移"), QCoreApplication::translate("InstanceSettingsDialog", "版本升级"),
                QCoreApplication::translate("InstanceSettingsDialog", "检查与修复"), QCoreApplication::translate("InstanceSettingsDialog", "诊断"), QCoreApplication::translate("InstanceSettingsDialog", "备份与操作")};
    }
    static int pageCount() { return filePageFirstIndex() + filePageKeys().size() + lifecyclePageKeys().size(); }

    /// @p key 那一页在堆叠页里占第几格。认不出来的键回 -1（调用方要判）。
    ///
    /// 两段：钉死的那两条（概览 / 启动）在**头**、文件那八页在 `filePageFirstIndex()` 之后
    /// —— 所以这个函数是"键 → 下标"那张表的**唯一出处**，别处的 `filePageFirstIndex() + 下标`
    /// 都该走它。（钉死那两条也得认：`showPage("overview")` 是最普通的一次调用，漏了它们
    /// 的症状是**这一页摆不动**——`showPage()` 判 -1 直接 return，界面上什么都不报。）
    ///
    /// NOTE: **按"页"认人是这一页唯一的跳页口径** —— 左栏那一列是按实例现算的，行号
    /// 随时会变（勾掉一条就少一格），拿左栏的行号去认页必然错位。
    static int stackPageIndexOf(const QString &key) {
        const int pinned = pinnedPageKeys().indexOf(key);
        if (pinned >= 0) {
            return pinned;
        }
        const int index = filePageKeys().indexOf(key);
        if (index >= 0) return filePageFirstIndex() + index;
        const int extra = lifecyclePageKeys().indexOf(key);
        return extra < 0 ? -1 : filePageFirstIndex() + filePageKeys().size() + extra;
    }

    /// 隐藏的左栏列表（`QListWidget`）。它**不是界面上的导航** ——
    /// 界面导航统一由 `navigationTreeName()` 那棵树承担，这一列在构造里 `hide()` 掉、只剩
    /// objectName 这一个身份（见 .cpp 里那句 `m_pageList->hide()`）。要量"左栏"请用树。
    static QString pageListName() { return QStringLiteral("instanceSettingsPageList"); }
    /// 左栏那棵导航树的 objectName（界面导航的唯一出处）。
    static QString navigationTreeName() { return QStringLiteral("instanceLifecycleTree"); }
    /// 左栏那一列的宽：最长的二级页名（「服务器列表」）加缩进放得下就够，多出来的都从右栏扣（UI问题 #6）。
    static int navigationTreeWidth() { return 152; }
    static QString launchInstanceButtonName() {
        return QStringLiteral("instanceSettingsLaunchInstance");
    }
    /// 概览那一页最大的那颗按钮。**不带 `▶`**：那一格默认界面字体里没有，第一次画出来
    /// 要付一次 277 ms 的字体回退（见 `ui/TextMarks.hpp` 头注释）；这颗按钮本身就是
    /// 那一页唯一的主动作，不缺一个三角来说明它。
    static QString launchInstanceText() { return QCoreApplication::translate("InstanceSettingsDialog", "启动实例"); }
    static QString launchInstanceHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "从实例设置的任意页面启动当前实例");
    }
    static QString stackName() { return QStringLiteral("instanceSettingsStack"); }
    /// 堆叠页里某一页的 objectName（@param key 见 pinnedPageKeys() / filePageKeys()）。
    static QString panelName(const QString &key) {
        return QStringLiteral("instanceSettingsPanel-") + key;
    }
    /// 包着那一页的那层滚动区的 objectName（**一页一层**，见 .cpp 里 addPage() 那段）。
    static QString scrollName(const QString &key) {
        return QStringLiteral("instanceSettingsScroll-") + key;
    }
    /// 那一页里那张表单的 objectName（概览 / 启动 各有一张；文件管理那几页还没有）。
    static QString formName(const QString &key) {
        return QStringLiteral("instanceSettingsForm-") + key;
    }

    /// 摆到 @p key 那一页上。认不出来的键 = 什么都不做。
    ///
    /// **按"左栏里有没有这一条"分两种情况** —— 有就走"选中那一条"这条路（和用户点那一条
    /// 是同一个动作）；没有（「配置文件」这种没有入口的页、或者被用户勾掉了的页）就直接
    /// 摆右栏、把左栏的选中清掉。
    ///
    /// 这正是验证工程该用的那一个入口：**要验的是那一页本身，不是"用户点得着"** ——
    /// 没有入口的「配置文件」照样得有人在盯着（它只是先不给看，不是烂在那儿）。
    void showPage(const QString &key);

    /// 关页会不会被"有没保存的改动"挡住（= `reject()` 里那两道闸）。
    ///
    /// 拖进来一个整合包就会离开实例设置，而那条路要**先问这一句**：那是程序
    /// 自己发起的关页，被闸挡住时不能当作"关掉了"继续往下走（底下那一格压着改了一半的
    /// 配置，继续走等于替用户丢掉它）。复述一遍那两条判据是错的 —— 此处**就是**它们的
    /// 定义处，`reject()` 也走这里。
    bool leaveBlockedByUnsavedEdits() const;

    /// 这一次拖放里**我们认的**本地文件（绝对路径，原样）。
    ///
    /// 认的只有"盘上存在的本地文件"（`QUrl::toLocalFile()` 非空那条路）—— 一张图片的
    /// 原始数据、一段从浏览器里拖出来的文本、一个 http 链接，一律**一个都不认**。
    /// 判断放在这一层是因为它同时是 `dragEnterEvent` 的准入判据：
    /// "哪些拖放我们接"只该有一处说法。
    static QStringList droppedFilePaths(const QMimeData *mime);

    /// 此刻摆在右栏那一页是**哪一页**（认不出来回空串）。`showPage()` 的反面。
    ///
    /// 给**"回来时还开在原来那一页上"**用：实例设置页每次都是新开一个对话框
    /// （`InstanceDialogController`），所以"上一次停在哪一页"这一层自己也记不住 ——
    /// 想记住的一方在收页之前读一次这个值（比如拖放那一路：`handleInstanceSettingsDrop()`
    /// 要判"正好停在模组那一页就直接装"），下次开页时再 `showPage()` 放回去。
    ///
    /// NOTE: 判据只有堆叠页的 `currentIndex()` 一处（和左栏反查用的是同一个函数）——
    /// 另存一份"现在在哪一页"的影子状态迟早会和右栏对不上。
    QString currentPageKey() const;

    /// 这几条给验证工程问几何/结构用（认控件主要还是按 objectName）。
    QListWidget *pageList() const { return m_pageList; }
    QTreeWidget *navigationTree() const { return m_navigationTree; }
    QStackedWidget *stack() const { return m_stack; }

    /// 左栏**此刻**摆着哪几条（键，从上到下）。**唯一出处**就是它：左栏那一列、
    /// 验证工程、以及"当前这一页掉出去之后回哪一页"都由它说了算。
    QStringList shownEntryKeys() const;

    /// 勾选表上那些"显示哪些页"的勾选框：这一刻用户勾掉的页（键 → false）。
    ///
    /// **只可能装 false，不会装 true**：勾选框只在那一页**可用**时能勾，而"可用"就是
    /// "默认显示" —— 所以能存下来的只可能是"用户把它藏起来了"。这一份不是"选择表"，
    /// 是一张**藏起来的页**的名单（读侧 = `显示 = 可用 && 名单里没写它`）。
    /// 空对象 = 一页都没藏（写盘那边按空对象**删键**，见 MainWindow::applyInstanceSettings）。
    QJsonObject pageChoices() const;

    /// 灌这个实例**存在盘上**的那份选择（键见 optionalPageKeys()）。认不出来的键丢掉。
    /// 调用方（MainWindow）从 wtfml.meta 的 `settingsPages` 里取出来递进去。
    void setPageChoices(const QJsonObject &stored);

    /// 「模组」那一页在这个实例上**用得上吗**（= 它装没装模组加载器）。
    ///
    /// NOTE: 和 setShaderUsable() / setSchematicUsable() 一样，**判据不在这一层** ——
    /// MainWindow 读 wtfml.meta 的加载器名 / 扫 mods/ 目录，算完把结论递进来。
    /// 默认是**可用**：忘了调 = 原版实例上也列出「模组」那一页。
    void setModUsable(bool usable);

    /// 「数据包」那一页在这个实例上**用得上吗**（= 这个游戏版本有没有数据包这回事）。
    /// 判据是"游戏版本 ≥ 1.13"（数据包从这个版本起才有），出处见
    /// VersionManager::instanceSupportsDatapacks()。默认可用，同上。
    void setDatapackUsable(bool usable);

    // ---------- 文件管理那八页（见上面 filePageKeys()）----------
    //
    // 八页分成三种形状：
    // - **快捷方式**：一排按钮，每颗开一个文件夹。没有目录可列。
    // - **服务器列表**：读 servers.dat，一行一个服务器。**它读不了盘**（那一层不 include
    // src/），所以列表是调用方解析好递进来的（setServerList()）。
    // - 中间那六页：同一个 InstanceFilesPage，只是喂进去的目录不同（见 setFileDirs()）。

    /// 这一页暂时用不上：**装了 Iris 或者 OptiFine 才谈得上光影包**。
    /// @param usable false 时那一页换成一句解释（"要装 Iris 或者 OptiFine"）。
    ///
    /// NOTE: 判据由调用方算 —— 它要读 wtfml.meta 的加载器名、还要扫 mods/ 里有没有
    /// `iris*.jar`，而 ui/ 够不着这两样（见 .hpp 开头那段"本文件不 include 任何
    /// src/ 下的头"）。这一层只负责把结果画出来。
    void setShaderUsable(bool usable);

    /// 同上：**装了 Litematica 才谈得上这一页**（那是 Litematica 自己的目录）。
    void setSchematicUsable(bool usable);

    /// 「加入服务器」那一颗的文字与悬停提示（**临时占位**）。
    ///
    /// @param text 空串 = 这一颗**收起来**（这个版本没有 `--quickPlayMultiplayer`）。
    ///
    /// 和 `InstanceFilesPage::setJoinText()` 是同一条规矩：**能不能用由调用方说了算** ——
    /// 「这个版本有没有那个启动参数」要读版本 json（`QuickPlay::supports()`），而 ui/ 够不着
    /// src/。这一层只负责画：文字非空就露出来、空就收起来。
    ///
    /// NOTE: 默认是**收着的**（构造里就是空文字）。所以那些不关心它的构造点（验证工程里
    /// 一大片）一个像素都不会变 —— 和 `setJoinText()` 逐字同一条理由。
    void setServerJoinText(const QString &text, const QString &tip);

    /// 服务器列表那一页的内容。
    /// @param servers 名字 → 地址，按文件里原来的顺序。
    /// @param problem 读不动的原因（空串 = 读到了）。
    ///
    /// NOTE: 「读到了但是一条都没有」和「没读到」要分开给 —— 前者那一页说"还没加过服务器"，
    /// 后者说清楚为什么没读到（像版本树那条"问到了就是没有"与"列表没取到"）。
    ///
    /// NOTE: 这个函数**把每一行的"现在什么样"清空**（图标、MOTD、状态那一格都回到
    /// 「还没测」）—— 它换掉的是整张表，旧的那些状态配的是旧的那些行。
    void setServerList(const QList<QPair<QString, QString>> &servers, const QString &problem);
    void setServerFavorites(const QStringList &addresses);
    QStringList serverAddresses() const;

    /// 一台服务器**现在**什么样（一次 ping 的结果）。
    ///
    /// NOTE: 「连上去问一句」那一半在 `src/util/ServerPing` 里 —— 这一层连 QTcpSocket 都
    /// 够不着（ui/ 不 include src/），也**不该**自己去连：一次点击发多少网络请求是
    /// 调用方说了算的事。这一层只负责把结果画出来。
    struct ServerRowStatus {
        bool tested = false;    ///< 测过没有（没测过那一格写「还没测」）
        bool online = false;    ///< 通了没有（false 时看 error）
        QString error;          ///< 没通的理由（人话，来自 ServerPing）
        /// 没通的那一档里**是不是超时**（来自 ServerPing::Status::timedOut）。
        ///
        /// 界面上**只写「超时」两个字**，而 `error` 那句话仍是全的
        /// （「超时（网络不通）」/「超时（连上了，但服务器没有回答）」—— 两者是不同的事，
        /// 留在那一份里给排查用）。不许在这里拿 error 的字符串去认前缀：判据只有
        /// ServerPing 那一处（`isTimeoutError()`）。
        ///
        /// 缓存那一份（`fromCache()`）也带着这一位（盘上存着，见 ServerStatusCache::
        /// Entry::timedOut）：没通那一档两处写**同一句字**，而这一位是**两处唯一的判据**
        /// （状态那一格里没有"上次"之类的过去时前缀）。
        bool timedOut = false;
        int latencyMs = -1;     ///< 延迟（毫秒；-1 = 不知道）
        int playersOnline = -1; ///< 在线人数（**-1 = 服务器没说**，和 0 个人不是一回事）
        int playersMax = -1;    ///< 人数上限（-1 = 服务器没说）
        QString motd;           ///< MOTD（已经洗过：颜色码去掉、多行合成一行）
        /// MOTD 的**原样**（`§` 码和换行都留着）—— 列表里那一格按游戏的样子画它
        /// （见 MainWindowUI::rowMotdRawRole()）。空串 = 没有这一份，那一格退回上面那条
        /// 洗过的路（悬停提示用的也是上面那份）。
        QString motdRaw;
        /// 这一份是**盘上缓存**里"上一次问到"的样子，不是刚刚问的。
        ///
        /// 它**不影响**状态那一格写出来的字（那一格里没有过去时前缀）。这一位留着是因为
        /// **它管别的**：没通那一档只有它分得出"理由要不要重说"（见 serverStatusLines()
        /// 里那两条支路）。一有真的结果就整条盖掉（fromCache 回到假）。
        bool fromCache = false;
        /// 服务器自报的版本名（`Paper 1.21.4` 这种，原样来自 ServerPing::Status::
        /// versionName；缓存那一份从 ServerStatusCache::Entry 里来）。
        ///
        /// **不进状态那一格**：那一格是延迟 / 人数，版本名不是"现在怎么样"的一部分
        /// （而且那一栏窄）。它只挂在悬停提示末尾（见 serverStatusTooltip()），
        /// 空串 = 没问到过（老缓存 / 没通的那一次）⇒ 那一截不写。
        QString versionName;
        /// 服务器自报的在线玩家名（已去掉 `§` 码，来自 ServerPing::Status::
        /// samplePlayers；缓存那一份从 ServerStatusCache::Entry 里来）。
        ///
        /// **不进状态那一格**（理由同版本名：那一栏窄，名单更长）。只挂在悬停提示末尾，
        /// 最多列五个（见 serverStatusTooltip()）。空表 = 服务器没给这一栏，不是"没人"。
        QStringList samplePlayers;
        QIcon icon;             ///< 服务器图标（favicon；服务器没给就是空的）
    };

    /// 把每台服务器现在的样子灌进去（**下标和 setServerList() 那一份一一对应**）。
    /// 短了的话后面那几条回到「还没测」、长了多出来的丢掉 —— 绝不按下标越界写。
    void setServerStatuses(const QList<ServerRowStatus> &statuses);

    /// 正在测速。true 时那颗「刷新」换成「刷新中…」并且点不动（连点三下发三遍请求是
    /// 用户没要的，而且每一遍都要等满超时）。
    void setServerPinging(bool pinging);

    /// 状态那一格要写的**那几行**：在线延迟和人数**分成两行**。
    ///
    /// 第一行是**延迟**（`42 ms`），第二行是人数（`7/20 人`）。没通那一档只有一行（那句话
    /// 本来就长；超时的那一档更短，只有「超时」两个字）。**唯一出处** —— 格子、悬停提示、
    /// 验证工程都看它。
    ///
    /// NOTE: 兜底那一条还留着「在线」两个字（见 .cpp）：**通了但一个数都没量到**时那一格
    /// 不能空着（空着和"还没测"长得一样），而那时候除了这两个字没别的可说。
    static QStringList serverStatusLines(const ServerRowStatus &status);

    /// 上面那几行的**单行写法**（用「 / 」串起来）—— 状态那一格的底子。验证工程也按它核对。
    ///
    /// NOTE: 它就是 `serverStatusLines().join(" / ")`（`42 ms / 7/20 人`），所以格子从
    /// 那几行变成两行之后，这里**一个字都没变**。
    static QString serverStatusText(const ServerRowStatus &status);

    /// 状态那一格的**悬停提示**：`serverStatusText()` 再挂上服务器自报的版本名与
    /// 在线玩家名（`42 ms / 7/20 人 / Paper 1.21.4 / 在线：Steve、Alex`）。
    ///
    /// **唯一出处** —— setServerStatuses() 写 tooltip 那一处和验证工程都看它。
    /// 版本名与玩家名都是**非常用属性**：不进格子（那一栏窄，而且它们不是"现在几个人"
    /// 的一部分），可又是服务器自己说的、用户偶尔真要看的。空串 / 空表 ⇒ 那一截不写；
    /// 玩家名最多列五个，后面还有（或者服务器只列了一部分）就加一个「等」。
    static QString serverStatusTooltip(const ServerRowStatus &status);

    /// 把六页目录列表**重新列一遍**。MainWindow 动过盘（删了 / 改了名）之后喊它。
    void refreshFilePages();

    // ---- 概览页那排常用目录的两段 ----
    // / 按「在盘的在外、其余收着」把那一排**重摆一次**（唯一出处，见 .cpp 里那段）。
    void relayoutShortcuts();
    /// 点「更多目录…」/「收起更多目录」。
    void toggleMoreShortcuts();

    // ---- 「配置文件」那一页的两条（见 openConfigFileRequested 那段）----
    // / 用户打开了 `config/` 底下的一条：目录 = 进去（这一层自己办），文件 = 发信号。
    void onConfigEntryActivated(const QString &path, bool isDir);
    /// 点「上级」：退到上一层（到根为止）。
    void leaveConfigDir();
    /// 按"现在在哪"决定那颗「上级」画不画。**进来、出去、换实例（setFileDirs）之后都要喊**。
    void updateConfigUpButton();

    /// 某一趟更新检测的结果（见 InstanceFilesPage::setUpdateResult）—— 转给**开得了
    /// 「可更新」那一格**的那几页（判据同构造里那一条）。
    ///
    /// 收在这一层的原因和 setFileIcon() 逐字相同（哪一页有这个路径只有这几页自己知道），
    /// 外加一条：**哪几页有「可更新」这一格也只有这一层知道**。
    ///
    /// @param outdatedPaths 查出来有新版本的那些文件的绝对路径（空表 + checked 为真
    /// = 查过了、一条都没有）。
    /// @param checked 这个实例查过没有（"没查过"和"查过、没有"是两句话）。
    ///
    /// NOTE: 这个实例正在跑一趟检测的时候用户**换了实例**的话，回来的那一份是按
    /// **那一趟的实例**递的 —— 所以调用方（MainWindow）只在自己那趟的实例上喊它
    /// （见 finishUpdateCheck 里"先记账再递"那一段）。
    void setFileUpdates(const QStringList &outdatedPaths, bool checked);

    /// 给某一条文件行换上它的**真图标**（见 InstanceFilesPage::setIconForPath）。
    ///
    /// 收在这一层是因为**路径属于哪一页只有六页自己知道** —— 调用方（MainWindow）手上
    /// 只有一串路径，挨页去问一遍的话，"哪一页有这个路径"这件事就跑到了业务层去。
    /// 转给六页各一次（不在那一页的那一条会自己说"没有"，不是错误）。
    void setFileIcon(const QString &path, const QPixmap &icon);

    /// 某一条文件**认出来了是哪个资源** ⇒ 它的名字点一下能进它自己的那一页
    /// （见 InstanceFilesPage::setResourceForPath()）。
    ///
    /// 收在这一层的原因和 setFileIcon() 逐字相同（哪一页有这个路径只有这几页自己知道），
    /// 外加一条：**哪几页接这件事也只有这一层知道**（见 filesNameOpensResource()）。
    ///
    /// @param title 那个资源的名字（接口给的那个）。**空串 = 没认出来** —— 名字点不动。
    void setFileResource(const QString &path, const QString &title);

    /// 给验证工程用的访问器：某一页那个文件列表控件（键见 filePageKeys()）。
    InstanceFilesPage *filesPage(const QString &key);
    QList<QWidget *> shortcutButtons() const { return m_shortcutButtons; }
    QTreeWidget *serverTree() const { return m_serverTree; }
    QLabel *serverNotice() const { return m_serverNotice; }
    QPushButton *serverRefreshButton() const { return m_serverRefresh; }
    /// 「编辑」：改这一台的名字和地址。
    QPushButton *serverEditButton() const { return m_serverEdit; }
    /// 「添加」：多一台；**常亮**，不跟着选中走。
    QPushButton *serverAddButton() const { return m_serverAdd; }
    /// 「删除」：拿掉选中那一台；没选中时灰着。
    QPushButton *serverDeleteButton() const { return m_serverDelete; }
    /// 「加入服务器」（临时占位；没这个参数的版本上它是收着的）。
    QPushButton *serverJoinButton() const { return m_serverJoin; }

    // ---------- 概览栏那三行（图标 / 分组 / 置顶）----------
    //
    // 它们和「实例名」是一类东西：说的都是**这个实例自己是谁**（叫什么、长什么样、摆在
    // 哪儿），不是它的启动参数。所以归「概览」，不归「启动」。
    //
    // 【出参：控件的值就是结果（图标那一行是唯一的例外）】
    // 分组和置顶照旧"现场读控件"。图标那一行有十档（默认 + 八个内置 + 本地那张），其中
    // **「本地图片」这一档没有任何一个控件能表示它**（它是一张从盘上读来的图，不是一个
    // 勾选状态），所以这一行的当前值落在 m_iconChoice 里，八个按钮只是它的一幅画。
    // 这是全页唯一一处影子状态，别的每一行都还是"控件的值就是结果"。

    /// 这个实例的目录（绝对路径，`versions/<实例名>`）。
    ///
    /// NOTE: 它和「概览」栏那行图标**没有关系** —— 那一行的预览是调用方把图解好递进来的
    /// （见 setIcon()：实例目录下那份 icon.png 由 MainWindow 读，ui/ 够不着 AppPaths）。
    /// 这一份是文件那几页用的：概览页上那排快捷按钮的「版本目录」开的就是它。
    void setInstanceDir(const QString &dir);

    /// 「文件管理」那几页各看哪个目录：**键 → 绝对路径**。
    ///
    /// NOTE: 上面那九条是**文件管理那几页**要的，一条不多。概览那排快捷按钮又多要三路
    /// 目录（`game` / `replay` / `crash`），它们**并在这同一份里一起灌**进来：
    /// 这一层只是"按某个键取一个路径"，多出来的键没有任何人当"文件管理页"读过（那几页
    /// 认的是 `filePageKeys()`），而 MainWindow 那边的 `instanceFileDirs()` 形状保持
    /// 不变（它有断言在数九条，为了这三条去动它不划算）。
    /// 去重之后：`game` / `crash` 是概览那排快捷方式要的，`replay` 是
    /// 左栏「Replay」那一页要的 —— 三条都还有读者。
    ///
    /// NOTE: 路径**由调用方算好递进来**，这一层不自己拼 `<实例目录>/mods` —— 那几个目录名
    /// 是 MC 的约定，仓库里已经有唯一一份出处（src/download/ResourceCatalog 的
    /// folderForKind()，整合包那一路也照着它写），ui/ 抄第二份迟早会跟它对不上。
    void setFileDirs(const QHash<QString, QString> &dirs);

    // ---------- 左侧那两类文件的「选中一行说一句」那一格 ----------
    //
    // 「Replay」和「结构」两页只把文件列出来；选中一行之后底下那一格要说的是：
    // 这份录像录了多久、什么版本、多少位玩家；这份结构多大、多少种方块、和这个实例的
    // 版本兼不兼容。**算的活不在这层**（读 `metaData.json` / gzip NBT 都在 src/），
    // ui/ 只负责"选中换了" → 发 `lifecycleFileInfoRequested()`，拿到答案再摆上去。
    //
    // NOTE: 和 `InstanceFilesPage` 那边同一套口子（`setInfoText(path, …)`）：答案带回
    // 它是**哪一条**的 —— 用户快速上下翻行时，前一发算得慢、后一发算得快，
    // 先回来的旧答案不能盖住新的（这里按路径比对，见 .cpp）。
    static QString lifecycleInfoLabelName(const QString &key) {
        return QStringLiteral("instanceLifecycleInfo_%1").arg(key);
    }
    /// 还没算出来时那一格摆的字（验证工程也拿它当"正在读"的判据）。
    static QString lifecycleInfoPendingText() { return QCoreApplication::translate("InstanceSettingsDialog", "正在读取…"); }

    enum class LifecycleBatchAction { Copy, Move, Remove };
    static bool isLifecycleBatchFile(const QString &key, const QString &path);
    void refreshLifecycleBatchFiles(const QString &key);

    /// 把 @p key 那一页的信息摆出来。
    /// @param path 这个答案讲的是**哪一条**（就是 `lifecycleFileInfoRequested()` 递出去的）。
    void setLifecycleInfoText(const QString &key, const QString &path, const QString &text,
                              const QString &toolTip = QString());

    static QString diagnosisReportName() { return QStringLiteral("instanceDiagnosisReport"); }
    static QString diagnosisRefreshButtonName() { return QStringLiteral("instanceDiagnosisRefresh"); }
    /// 还没算出来时摆的那一句（验证工程也拿它当"接没接上"的判据）。
    static QString diagnosisPendingText() { return QCoreApplication::translate("InstanceSettingsDialog", "正在检查…"); }
    void setDiagnosisText(const QString &text);

    // ---------- 「迁移」与「升级」那两页 ----------
    //
    // 两页共用一套控件：目标下拉框 + 计划正文 + 可勾选的范围表 + 动手那一颗。**算的活
    // 全在 `src/operations/TransferPlanner`**（扫盘、兼容结论、默认勾选都在那儿），
    // 写盘在 `TransferApply`；这一层只摆字、记勾选、发两发信号。
    //
    // NOTE: 目标那一列是**这个游戏文件夹里已经装好的其他版本**（migration 是把内容搬进
    // 其中一份，upgrade 是拿其中一份当底造升级副本）。列表由 controller 灌进来 ——
    // ui/ 不认识 VersionManager，也不知道"哪一份是当前这一份"。
    static QString transferTargetComboName(const QString &key) {
        return QStringLiteral("instanceTransferTarget_%1").arg(key);
    }
    static QString transferPlanTextName(const QString &key) {
        return QStringLiteral("instanceTransferPlan_%1").arg(key);
    }
    static QString transferItemsListName(const QString &key) {
        return QStringLiteral("instanceTransferItems_%1").arg(key);
    }
    static QString transferApplyButtonName(const QString &key) {
        return QStringLiteral("instanceTransferApply_%1").arg(key);
    }
    /// 灌目标列表（显示名, id 对）。**不发信号** —— 开页时由 controller 紧接着算第一份
    /// 计划；用户在下拉框里换目标才发 `transferTargetChosen()`。
    void setTransferTargets(const QString &key, const QList<QPair<QString, QString>> &targets);
    /// 摆计划正文（`TransferPlanner::Plan::detailText()` 那一份）。
    void setTransferPlan(const QString &key, const QString &text);
    void setUpgradeContext(const QString &text);
    QStringList upgradeKeptMods() const;
    /// 用户逐项选了「替换为平台适配版」的那些文件名。
    QStringList upgradeReplaceMods() const;
    /// 文件名 ⇒ 替换信息（`upgradeModReplacements`）。评估层填，执行层读。
    QVariantMap upgradeModReplacements() const;
    int upgradePendingModCount() const;
    void setUpgradeModChecks(const QList<QStringList> &rows, const QString &summary);
    /// 摆可勾选的范围表（显示名, 键 对 + 默认勾选的键）。
    void setTransferItems(const QString &key, const QList<QPair<QString, QString>> &items,
                          const QStringList &defaultKeys);
    /// 现在选中的目标 id（没有目标 = 空串）。
    QString transferTarget(const QString &key) const;
    /// 现在勾着的那几个键（**顺序与范围表一致**）。
    QStringList transferSelectedKeys(const QString &key) const;

    // ---------- 「配置文件」那一页的第二个格子----------
    //
    // 【为什么是"格子里再嵌一格"，而不是再开一整页覆盖层】
    //
    // 因为**开不出来**：`OverlayHost::openPage()` 在已经有人占着那一层时直接返回 nullptr
    // （一层只放一个页面，见 OverlayHost.hpp 那段），而用户点这一行的那一刻，「实例设置」
    // 自己正占着那一层。所以「配置文件」这一页是个**两格子的堆叠**：
    //
    // ① 文件列表（+「上一级」+底下那行小字） ② 编辑器
    //
    // 点一个文件 = 切到 ②，点「返回」（或按 Esc）= 切回 ①。切的事由这一层办（它俩都是
    // 它的孩子），而**盘上的事不办** —— 读、写、开目录、弹话都还是 MainWindow 的
    // （这一层没有 OverlayHost）。所以这一层只提供"把编辑器切出来 / 切回去"两个动作，
    // 外加一个把它**露出来**给 MainWindow 接线的口子。

    /// 那一格编辑器（**一次建好、一直留着**）。
    ///
    /// NOTE: 给的是裸指针是为了让 MainWindow **接一次线就完事**：它在摆好这一页的时候
    /// 连上 `saveRequested` / `backRequested` 两发，之后每次开文件只是往这一格里
    /// 灌内容（`openConfigEditor()`），不再重新连。所以**接回来的时候不要把它当
    /// 一次性的**（那会连第二遍，于是保存一次写两个盘）。
    ///
    /// NOTE: 读它之前先确认这一页开着没有 —— 没开的话它一样在（构造时就建了），只是
    /// 没露脸。这和"资源详情页那一个对象建一次留着"是同一条规矩。
    ConfigFileEditorPage *configFilePage() const {
        const_cast<InstanceSettingsDialog *>(this)->ensureFilePage(QStringLiteral("config"));
        return m_configEditor;
    }

    /// 把编辑器切出来，并摆上这一份文件。
    /// @param path 绝对路径。
    /// @param loaded 那一份字节解出来的东西（见 ConfigFileText::decode）。**由 MainWindow
    /// 读盘 + 解码之后递进来** —— 这一层不碰盘。
    /// @param stamp 读之前盘上那一份的样子（见 ConfigFileText::Stamp）。**只有读盘的人量得
    /// 出来**，所以也是递进来的；保存前拿它比对（"这中间别人动过没有"）。
    void openConfigEditor(const QString &path, const ConfigFileText::Loaded &loaded,
                          const ConfigFileText::Stamp &stamp = ConfigFileText::Stamp());

    /// 这个文件**留过备份没有**（有 ⇒ 那一格画出「还原上一次」）。和 stamp 一样是"读盘的人
    /// 才知道"的事（那儿才知道备份根底下有没有它），所以由 MainWindow 递进来。
    void setConfigBackupAvailable(bool available);

    /// 切回文件列表那一格（并让那一页重新列一遍目录）。
    ///
    /// NOTE: **调用它之前先看 `configFilePage()->isDirty()`**：改过的东西不许悄悄没掉，
    /// 那一问（"改了没保存，还返回吗"）得由 MainWindow 弹（这一层没有覆盖层），
    /// 用户答"不保存"之后要先 `discardChanges()` 再走这儿。这一层不自己问 ——
    /// 它连"现在是脏的"这件事都不看，谁调谁负责。
    void closeConfigEditor();

    // ---------- 「存档」那一页的第二个格子----------
    //
    // 和上面「配置文件」那一段**同一个做法、同一个理由**（开不出第二个整页覆盖层）：
    // ① 世界列表（+「导入世界」「批量编辑」+底下那行小字） ② 世界设置（那支笔点开的）
    //
    // NOTE: 这是**第二份**"两格子"，不是把上面那一份抽成一个通用的容器。抽的话要动的
    // 是 `m_configStack` / `m_configEditor` / `m_configUp` / `m_configRoot` 这一整套
    // （全是单份成员，且 `openConfigEditor` / `closeConfigEditor` 是公开 API，
    // MainWindow 和两个验证工程都接着它们），而两边**要摆的东西完全不一样**
    // （一个文件列表 + 上级目录，一个世界表单）—— 抽出来的那个"通用子页"只共用
    // "一个 QStackedWidget 两个格子"这一句，剩下的全是各自的。重复的那二十来行
    // 比一个漏参数的抽象便宜。

    /// 那一格世界设置（**一次建好、一直留着**，理由同 `configFilePage()`）。
    WorldSettingsPage *worldSettingsPage() const { return m_worldSettings; }

    /// 把世界设置切出来，并摆上这一个世界。
    ///
    /// @param view 摆上去的一切。**由 MainWindow 从盘上读出来、翻译过来**（这一层不碰盘，
    /// 也够不着 `WorldFiles::WorldInfo` —— `ui/` 不许 include `src/`，见
    /// WorldSettingsPage 文件头）。
    void openWorldSettings(const WorldSettingsPage::WorldView &view);

    /// 把世界设置摆成"读不出来"那一档（坏档警告之后落下来的地方）。
    void openWorldSettingsUnreadable(const QString &dirName, const QString &reason);

    /// 切回世界列表那一格（并让那一页重新列一遍目录）。
    ///
    /// NOTE: 这一条**没有**"改了没保存"要问的那种事：世界设置那一格上改的东西全在控件里，
    /// 关掉这一格就是把它们丢了 —— 所以"改过没有"得由 MainWindow 自己记（它手上
    /// 有 `editedView()`，和打开时那一份比一下就知道了）。
    void closeWorldSettings();

    /// 这个世界的存档现在**读不出来的话**，那一页上那颗「保存」本来就不该露 —— 给
    /// MainWindow 一个问的地方（它不用自己去翻那一格的状态）。
    bool worldSettingsUnreadable() const;

    /// 图标预览那一格的边长（像素）。**外面要按这个边长把图解好递进来** —— 递一张 16×16 的
    /// 进来会被放大成一团糊（那几张方块是像素画，而用户挑的照片本来就有的是像素）。
    static int iconPreviewSize();

    /// 图标那一行的初值。
    /// @param icon wtfml.meta 里那个 icon 键的原文（空 = 默认）。
    /// @param defaultMark 「默认」那一档画什么。**由调用方算好递进来**：它取决于这个实例
    /// 的版本分类 / 加载器（见 MainWindow::instanceRowMark()），而 ui/ 够不着那个
    /// 判断（本文件不 include 任何 src/ 下的头）。
    /// @param localMark 「本地图片」那一档现在长什么样（调用方把实例目录下那份 icon.png
    /// 读出来递进来；没有那份文件、或者它读不动，给一张空图 —— 那一档会自动退回
    /// defaultMark，预览是给人看的，空着一块方格比"显示上一张"更让人以为选上了）。
    ///
    /// NOTE: 三张图都由调用方解好，这一层不碰盘 —— 它只在用户**刚挑了一张新图**的时候
    /// 读那个路径（那一下它手上只有一条路径，见 pickedIconPath()）。
    void setLastLaunch(const QDateTime &at);
    void setPlayTime(const QString &total, const QString &last);
    void setIcon(const QString &icon, const QPixmap &defaultMark,
                 const QPixmap &localMark = QPixmap());

    /// 「分组…」那一格可选的那几组（**点了按钮才看得见**，见 .cpp 里那两个浮层）。
    /// @param groups **组名 → 组 id**，按用户在「分组设置」里排的顺序。
    /// 第一项「未分组」由这一层自己加在最前面（它的 id 是空串），调用方不用管。
    /// @param currentGroupId 它**老家**在哪一组（空 = 未分组）。
    ///
    /// NOTE: 传 (名字, id) 成对而不是只传一串 id：组名用户可以改、id 才是身份，而这一页
    /// 只负责**显示** —— 名字从哪来（读盘？显示格式？）是调用方的事。
    /// NOTE: 认不出来的 currentGroupId（那一组刚被删掉）落在「未分组」上 —— 和实例树那边
    /// 「组被删之后它自然落到未分组」是同一条规矩，不为它写清理代码。
    /// NOTE: **传的是「老家」不是「存的是哪一组」**。这两个对"正在置顶"的实例不是一个答案
    /// （存的那一格指着置顶组），而用户在这一格说的永远是"它本来属于哪儿"—— 两边都
    /// 显示之后，勾上置顶**不挪这一格**（见 setPinned()）。
    void setGroupChoices(const QList<QPair<QString, QString>> &groups, const QString &currentGroupId);

    /// 置顶那一行的初值。
    /// @param pinned 这个实例现在在不在置顶组里（「显示在置顶组中」勾没勾）。
    /// @param pinnedGroupId 置顶组是哪一组，只用来写进那一句解释里（"会在「置顶」里也摆
    /// 一条"）。**它不影响任何控件** —— 「分组」那一格说的永远是**老家**，「置顶」勾选框
    /// 说的是"要不要在置顶组里多摆一条"，两边都显示、互不相干。空串 = 这一份 .minecraft
    /// 里没有置顶组（用户把它删了）：勾选框照旧能勾，但勾上这一下真的什么都不会发生
    /// （MainWindow::pinInstance() 直接返回）。
    ///
    /// NOTE: 和 setGroupChoices() **没有先后要求**。
    void setPinned(bool pinned, const QString &pinnedGroupId);

    /// 图标那一行现在选的是什么：空串 = 默认、`builtin:<键>` = 内置候选、`local` = 本地那张。
    /// 形态与 VersionManager::iconField() 逐字一致（那也是写进 wtfml.meta 的东西）。
    QString iconChoice() const;

    /// 「本地图片」那一档在 wtfml.meta 里写成什么（`local`）。
    ///
    /// NOTE: 拼它的地方是这一页，认它的地方在 MainWindow::instanceRowMark() ——
    /// 这就是它有唯一出处的理由（和 VersionIcons::iconKeyFromValue() 同一个道理）。
    static QString localIconValue() { return QStringLiteral("local"); }

    /// 用户从盘上挑的那张图的路径（没挑过 = 空）。
    ///
    /// NOTE: 挑完**不在这里**拷走 —— 拷进实例目录是调用方的事
    /// （MainWindow::applyInstanceSettings()：挑完当场拷一份，见那边 `pickedIcon`
    /// 那一段）。这一层碰不了盘（ui/ 够不着 AppPaths），也**不该**碰：它手上的是
    /// "用户挑了这张"，"这张要不要真的占住那个文件名"是落盘那一侧的决定。
    QString pickedIconPath() const;

    /// 用户挑的那张图**已经真的落进实例目录了**（由调用方在写盘成功之后叫）。
    ///
    /// 忘掉源路径，否则往后每一次「改了就写」（改内存、勾开关……）都会**再
    /// 从那个源文件抄一遍**。用户把源文件挪走/删掉之后，下一次保存还会平白弹一句「图标
    /// 没保存」—— 而盘上明明已经是这张图。签名跟着挪到"本地那张"这一档（`iconChoice`
    /// 本来就是它），否则下一次任何改动都会多触发一次写入。
    void markPickedIconSaved();

    /// 分组那一行选的是哪一组（空串 = 未分组）。判据是那一格的 **userData**、不是文字 ——
    /// 组名会因为改名 / 换语言而变。
    QString chosenGroupId() const;

    /// 置顶那一行勾没勾。
    bool pinned() const;

    // ---------- 控件的 objectName ----------
    // 验证工程靠这些名字认控件，别改回按文字去认（文字会随全局值变）。
    // / 某一项的「跟随全局 / 自定义」那个下拉框。@param field 见下面那五个常量。
    // /
    // / **这一格是一个档位下拉框**（objectName 就是这一个名字）。验证工程按这个名字找、
    // / 按**下标**选（`choiceFollowIndex()` / `choiceCustomIndex()`）。
    static QString choiceComboName(const QString &field) {
        return QStringLiteral("override_%1_choiceCombo").arg(field);
    }
    /// 档位那个下拉框里两项的下标。**档位的唯一出处就是这两个数** —— 读侧
    /// （`isCustomChoice()`）、写侧（`setChoiceCustom()`）和验证工程都走它们，
    /// 别在别处再写一遍 `0` / `1`。
    static int choiceFollowIndex() { return 0; }
    static int choiceCustomIndex() { return 1; }

    // ---------- 页面按钮那一套的规格----------
    //
    // 页面按钮**恒定宽度和高度、每一行三个、间距适中且左右铺满**。排布在 .cpp 里那三个
    // `layout…` 函数里算，**量的这几个数就是它们** —— 验证工程别抄第二份（抄一份的下场
    // 是改了这里、那边还在断言旧数）。
    //
    // NOTE: **宽度不在这儿** —— 它不是钉死的像素，是"三列各一份 + 铺满整幅"算出来的
    // （见 .cpp 里 layoutActionCells() 那段）。这两个数只是"排几列"和"至少多宽"。
    /// 高度**不另立一个数**：这一排是普通的动作按钮，全局 QSS 那条
    /// `min-height: actionHeight() - 14` 本来就把它们钉在全应用那一档上
    /// （`setFixedHeight()` 写小了只会被 QSS 顶回来，量到的仍是大数）。
    static constexpr int actionButtonHeight() { return Theme::actionHeight(); }
    static constexpr int actionButtonsPerRow() { return 3; }
    static constexpr int actionButtonSpacing() { return 10; }
    static constexpr int actionButtonMinWidth() { return 110; }
    /// 某一行「跟随全局（当前 …）」那句话的 objectName。
    static QString followHintName(const QString &field) {
        return QStringLiteral("override_%1_hint").arg(field);
    }
    /// 某一行「档位那一行」的 objectName —— 里面是 [跟随全局][自定义] + 那一项的控件。
    /// 那一项的控件和它的档位是**同一个决定的两半**（选「自定义」+ 自定义成什么），
    /// 所以并排在同一行；这一整行下面才是那一句解释（followHintName）。
    static QString choiceRowName(const QString &field) {
        return QStringLiteral("override_%1_choice").arg(field);
    }
    static QString javaPathField() { return QStringLiteral("javaPath"); }
    static QString xmxField() { return QStringLiteral("xmx"); }
    static QString xmsField() { return QStringLiteral("xms"); }
    static QString extraJvmArgsField() { return QStringLiteral("extraJvmArgs"); }
    /// 「窗口模式」那两行。
    ///
    /// NOTE: 这三个串**就是 wtfml.meta 里那三个键名**（`src/settings/GameWindowSettings.cpp`
    /// 里 gameWindowModeMetaField() 那一组，那边是读侧写侧的唯一出处）。这一份是
    /// `ui/` 侧的那一份 —— ui/ 不许 include src/，所以两边各写一遍，两边各有断言
    /// 钉着（那条约定见 LauncherSettings::gameWindowMinWidth() 上面那段）。
    /// 对不上的症状是"在实例设置里改完窗口模式，启动时一点变化都没有"。
    static QString windowModeField() { return QStringLiteral("windowMode"); }
    static QString windowWidthField() { return QStringLiteral("windowWidth"); }
    static QString windowHeightField() { return QStringLiteral("windowHeight"); }

    static QString javaPathEditName() { return QStringLiteral("instanceJavaPathEdit"); }
    static QString browseJavaButtonName() {
        return QStringLiteral("instanceBrowseJavaButton");
    }
    static QString memoryMaxSpinName() { return QStringLiteral("instanceMemoryMaxSpin"); }
    /// 「最大内存」那一行左半边那根条（和设置页同一套，见 MemorySlider.hpp）。
    static QString memoryMaxSliderName() { return QStringLiteral("instanceMemoryMaxSlider"); }
    /// 对象还在、还在跟着盘上的值走，但**整行不显示**。名字留着是因为验证工程还按它找
    /// 这个控件 —— 找得到、但不该看得见（见 .cpp 里「最小内存」那一行上面那段）。
    static QString memoryMinSpinName() { return QStringLiteral("instanceMemoryMinSpin"); }
    /// 「窗口模式」那个下拉框。**项由调用方喂**（见 WindowModeChoice），这里只搭框。
    static QString windowModeComboName() { return QStringLiteral("instanceWindowModeCombo"); }
    static QString windowWidthSpinName() { return QStringLiteral("instanceWindowWidthSpin"); }
    static QString windowHeightSpinName() { return QStringLiteral("instanceWindowHeightSpin"); }
    /// 「宽 × 高」中间那个乘号（和设置页那个同名不同物，各有各的父控件）。
    static QString windowSizeTimesName() { return QStringLiteral("instanceWindowSizeTimes"); }
    static QString extraJvmArgsEditName() {
        return QStringLiteral("instanceExtraJvmArgsEdit");
    }
    static QString appendJvmArgsEditName() {
        return QStringLiteral("instanceAppendJvmArgsEdit");
    }
    static QString warnJavaMismatchCheckName() {
        return QStringLiteral("instanceWarnJavaMismatchCheck");
    }
    /// 「实例名」那一格（改名）。**它在「概览」栏的第一行**：它改的是这个实例的身份，
    /// 别的几项改的都是它的启动参数 —— 先看清改的是哪一个，再谈别的。打开对话框默认停在
    /// 「概览」，所以它还是第一眼看到的那一格。
    static QString instanceNameEditName() { return QStringLiteral("instanceNameEdit"); }
    static QString instanceNameText() { return QCoreApplication::translate("InstanceSettingsDialog", "实例名"); }

    // ---- 概览栏那四行（见上面 setIcon 那一段）----
    //
    // 排法：第一行 图标 名称 / 第二行 更改图标 分组… / 第三行 显示在置顶组中 / 第四行 导出。
    // 概览这一栏**没有"标题在左、内容在右"的行名**，那三句话变成了第一行的控件自己
    // （名称输入框）、第二行两颗按钮、第三行勾选框上的字。
    // / 第一行：图标预览（一张固定边长的图，当前选的是哪一张就画哪一张）+ 名称输入框
    // / （`instanceNameEditName()`，就是「实例名」那一格）。
    static QString iconPreviewName() { return QStringLiteral("instanceIconPreview"); }
    /// 页眉名字下面那一行（加载器 · 游戏版本，见 `setHeaderMeta()`）。验证工程要读它 ——
    /// 这一行和主页 / 管理列表是不是同一个加载器名，只有从控件上才问得出来。
    static QString headerMetaName() { return QStringLiteral("instanceHeaderMeta"); }
    /// 第二行左边那颗「更改图标」——点了在它底下开 @c iconMenuName() 那个浮层。
    static QString changeIconButtonName() { return QStringLiteral("instanceChangeIconButton"); }
    /// 那个浮层本身（**常驻**：构造函数里建好一次，以后每次点按钮只是再弹一遍 —— 见
    /// .cpp 里那段 NOTE。`isVisible()` 才表示"此刻开着没开着"）。
    static QString iconMenuName() { return QStringLiteral("instanceIconMenu"); }
    /// 浮层里第 @p index 张内置候选那颗按钮（下标见 VersionIcons::iconCandidateKeys()）。
    static QString iconCandidateButtonName(int index) {
        return QStringLiteral("instanceIconCandidate%1").arg(index);
    }
    /// 浮层里的「选一张图片…」（开的是 Windows 自己的文件选择框）。
    static QString iconPickButtonName() { return QStringLiteral("instanceIconPickButton"); }
    /// 浮层里的「恢复默认」。
    static QString iconResetButtonName() { return QStringLiteral("instanceIconResetButton"); }
    /// 第二行右边那颗「分组…」——点了在它底下开 @c groupMenuName() 那个浮层。
    static QString groupButtonName() { return QStringLiteral("instanceGroupButton"); }
    /// 「分组…」那个浮层本身（同样常驻）+ 里面第 @p index 行的 objectName。
    /// **下标 0 是「未分组」**，后面按 setGroupChoices() 递进来的顺序。
    static QString groupMenuName() { return QStringLiteral("instanceGroupMenu"); }
    static QString groupMenuRowName(int index) {
        return QStringLiteral("instanceGroupMenuRow%1").arg(index);
    }
    /// 第三行那个勾选框。
    static QString pinCheckName() { return QStringLiteral("instancePinCheck"); }
    /// 第四行那个「导出…」按钮。
    ///
    /// NOTE: 它**不叫** `exportScript*` 那一套 —— 「导出启动脚本」是同一个页面上另一件事
    /// （导出的是 .bat，和整合包规范一点关系都没有）。两个名字长得像过一次，
    /// 断言认错过控件。
    static QString exportButtonName() { return QStringLiteral("instanceExportButton"); }
    /// 概览页那颗「检查更新」的 objectName；验证工程拿它找那颗按钮。
    static QString checkUpdatesButtonName() { return QStringLiteral("instanceCheckUpdatesButton"); }
    /// 那颗按钮上的字（**唯一出处**）。
    static QString checkUpdatesText() { return QCoreApplication::translate("InstanceSettingsDialog", "检查更新"); }
    /// 那一格下面那句解释。
    ///
    /// 这一句要写出「更新检测」这四个字的含义，同时得把**我们能说什么**讲清楚：
    /// 判据是"盘上那个文件的 sha1 等于这个项目最新那一版的哪个文件"（见
    /// `ModDependencies::judgeUpdate`），所以它认不出来的那些会如实说"说不清"——
    /// 那句话不是失败，是"这一条我没法判"。**别把这句话写成"已经全部检查过了"**：
    /// 用户会以为"说不清"的那几条也是"没问题"。
    static QString checkUpdatesHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "检查资源更新，不修改文件。");
    }
    /// 那颗按钮的 tooltip（**说全的那一份**）。
    static QString checkUpdatesButtonTooltip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "按当前游戏版本和加载器检查资源更新；无法识别的标为「无法确认」。");
    }
    /// 概览页那颗「更新中心」的 objectName。
    ///
    /// NOTE: 和「检查更新」是**两颗、不是一颗**：那一颗要联网重查一遍（几十个文件、好几发
    /// 请求），而这一颗只是把**上一次查出来的**那份结论摆开看（一个网络都不发）——
    /// 它也是「撤销上一次更新」唯一进得去的地方，所以那一颗按钮**不许**被"必须联网"
    /// 卡住（更新完了发现游戏起不来，正是要退回去的那一刻）。
    static QString updateCenterButtonName() { return QStringLiteral("instanceUpdateCenterButton"); }
    static QString updateCenterText() { return QCoreApplication::translate("InstanceSettingsDialog", "更新中心"); }
    /// 那一格下面那句解释。
    static QString updateCenterHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "查看检查结果、更新资源或撤销上次更新。");
    }
    /// 那颗按钮的 tooltip（**说全的那一份**）。
    static QString updateCenterButtonTooltip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "支持更新、忽略版本、固定版本和撤销上次更新。");
    }

    // ---- 「复制实例」----
    //
    // 概览里的「复制实例」：点击以后让人选择复制到的文件夹，再给复制出来的实例起个名字。
    // / 概览页第五行那颗「复制实例」的 objectName。
    static QString copyButtonName() { return QStringLiteral("instanceCopyButton"); }
    /// 那颗按钮上的字（**唯一出处**）。
    static QString copyText() { return QCoreApplication::translate("InstanceSettingsDialog", "复制实例"); }

    /// 那一格下面那句解释。
    ///
    /// 这句话要说清**落到哪儿**："复制到另一个游戏文件夹"不是随便一个目录 ——
    /// 拷贝的落点是 `<你挑的那个文件夹>/versions/<新名字>/`（见
    /// VersionManager::copyInstance 那段）。不说清的话，用户会以为挑中的目录就是副本
    /// 本身的位置，然后在一个自己没料到的层级里找它。
    static QString copyHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "复制模组、存档和配置，创建独立实例。");
    }

    /// 那颗按钮的 tooltip（**说全的那一份**）。
    ///
    /// NOTE: 最后那句"先关掉游戏"是真的会拦（MainWindow::copyInstanceTo() 里第二道闸）。
    /// 界面不该在明着教用户做一件会被拦住的事。
    static QString copyButtonTooltip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "选择目标游戏目录和副本名称。复制保留原实例，运行中的实例不能复制。");
    }

    // ---- 「删除实例」----
    //
    // 删除这件事在实例行悬停时行尾那颗垃圾桶上已经有了，但界面上一直"看不见"它 —— 它藏在
    // 一个要悬停才出现的格子里，而用户找它的时候找的是"这个实例的设置里面有没有"。所以
    // 这一颗不是新功能，是**同一个动作的第二个入口**：真去删的那条路一个字都没改
    // （`MainWindow::confirmDeleteInstance()`，两道闸 + 问一句都在那儿）。
    //
    // NOTE: 和「复制实例」同一个形状 —— 这一层只发一句信号。理由也一样：那两步（问一句
    // "真的要删吗"、把整个目录删掉）都要覆盖层和 VersionManager，ui/ 两样都没有。
    // / 概览页那一颗「删除实例」的 objectName。
    static QString deleteButtonName() { return QStringLiteral("instanceDeleteButton"); }
    /// 那颗按钮上的字（**唯一出处**）。
    static QString deleteText() { return QCoreApplication::translate("InstanceSettingsDialog", "删除实例"); }
    /// 那一格下面那句解释。
    ///
    /// 这句话里**必须带"回收站"**：这一颗归在"危险"那一档（所以它是红的）—— 颜色只是
    /// 提醒，真正让人停下来的是字。上面那颗「复制实例」是"多出一份"，这一颗是"少掉一份"；
    /// 但它**不是**永久删除：删掉的那一整目录会进回收站，所以字也得这么说，不然用户要么
    /// 以为没法挽回、要么以为回收站永远替他留着。
    static QString deleteHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "删除实例目录，文件移入回收站。");
    }
    /// 那颗按钮的 tooltip（**说全的那一份**）。
    static QString deleteButtonTooltip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "删除实例目录，文件移入回收站。运行或安装期间不可删除；清空回收站后无法恢复。");
    }

    // ---- 存档那一页顶上那两颗----
    //
    // 存档页顶端有「导入世界」和「批量编辑」两颗按钮（后者先做占位）。
    // NOTE: 这两句和上面那些 `xxxText()` 一样是**唯一出处** —— 那一页照 setDownloadText()
    // 的约定按"文字空不空"决定露不露，所以字写在这儿、别的几页不接。
    // / 存档页顶上那颗「导入世界」。
    static QString importWorldText() { return QCoreApplication::translate("InstanceSettingsDialog", "导入世界"); }
    static QString importWorldTip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "导入本地世界文件夹。");
    }
    /// 存档页顶上那颗「批量编辑」。**它是个占位** —— 点下去只会说一句
    /// 「还没做」。字和提示都照实写，不假装它能用。
    static QString batchEditWorldsText() { return QCoreApplication::translate("InstanceSettingsDialog", "批量编辑"); }
    static QString batchEditWorldsTip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "批量编辑存档（暂不可用）。");
    }

    // ---- 启动栏那两颗「动作」按钮（导出启动脚本 / 补全版本文件）----
    //
    // 它们是**动作**不是设置项（不改 wtfml.meta 里任何一格），所以没有「跟随全局 /
    // 自定义」那一格档位。**没有单独一行行名**：按钮文本直接就是设置名称，两句话挪去
    // tooltip。
    // / 「导出启动脚本」（开的是 Windows 自己的保存框）。
    static QString exportScriptButtonName() { return QStringLiteral("instanceExportScriptButton"); }
    /// 「补全版本文件」（校验并补齐这个实例的文件）。
    static QString repairFilesButtonName() { return QStringLiteral("instanceRepairFilesButton"); }
    /// 两颗按钮上的字（= 它们的行名，也是这两件事的**唯一名字**）。
    static QString exportScriptText() { return QCoreApplication::translate("InstanceSettingsDialog", "导出启动脚本"); }
    static QString repairFilesText() { return QCoreApplication::translate("InstanceSettingsDialog", "补全版本文件"); }
    /// 两颗按钮的 tooltip，以及那一行下面那句合起来的说明。
    static QString exportScriptHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "导出独立启动脚本。正版脚本不保存令牌，运行时需提供 WTFML_MC_ACCESS_TOKEN。");
    }
    static QString repairFilesHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "校验并补齐客户端、库和资源；下载或安装期间不可执行。");
    }
    /// 两颗按钮下面那一句。**两句都要留着**：上面那一颗带着"导出的脚本里有令牌"这句
    /// 安全警告，光放 tooltip 里会有人一辈子看不到。
    static QString actionsHint();

    // ---- 概览栏那颗「检查并修复」----
    static QString integrityCheckButtonName() { return QStringLiteral("instanceIntegrityCheckButton"); }
    static QString integrityCheckText() { return QCoreApplication::translate("InstanceSettingsDialog", "检查并修复"); }
    static QString integrityCheckHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "只读检查缺失或损坏文件，确认后修复。");
    }
    static QString integrityCheckTooltip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "后台扫描，完成后显示检查报告。");
    }

    // ---- 概览栏那两颗「快照」----
    //
    // 和「检查并修复」一样**只发信号**：清单、后台线程、任务账本、挑哪一份、"还原之前先给
    // 现在这一份做安全快照"全在 MainWindow 那边（这一层连读盘都没有）。
    //
    // 为什么是"快照"而不是"备份"：概览这一栏已经有一颗「导出…」（出的是**给别人用的**
    // 整合包）。同一个实例上再来一颗叫「备份」的，用户分不清哪一颗是"我自己要回去用的"。
    // 快照 = 给我自己还原用的那一份（默认排除能重新下回来的 assets / libraries）。
    static QString createSnapshotButtonName() { return QStringLiteral("instanceSnapshotButton"); }
    static QString createSnapshotText() { return QCoreApplication::translate("InstanceSettingsDialog", "创建快照…"); }
    /// 那一格下面那句解释。**要点名默认排除的是什么** —— 那是用户唯一需要知道的分岔。
    static QString createSnapshotHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "备份模组、配置和存档，可选择是否包含游戏库和资源。");
    }
    static QString createSnapshotTooltip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "后台创建快照，保存到启动器的实例备份目录。");
    }
    static QString restoreSnapshotButtonName() { return QStringLiteral("instanceRestoreSnapshotButton"); }
    static QString restoreSnapshotText() { return QCoreApplication::translate("InstanceSettingsDialog", "从快照恢复…"); }
    static QString restoreSnapshotHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "还原前自动备份当前实例，还原后校验文件。");
    }
    static QString restoreSnapshotTooltip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "还原将覆盖当前实例文件。");
    }

    // ---- 文件管理那八页的文案与 objectName----

    /// 常用目录那一排按钮：**两张按下标对齐的表**（按钮上的字 / 路径键）。
    /// 路径键见 setFileDirs() 与 setInstanceDir()。
    ///
    /// NOTE: 它们长在**概览页**上，不是单独一页 —— 「快捷方式」那个页键和 objectName
    /// 都还留着，只是这一排按钮并进了概览。
    static QStringList shortcutTexts();
    static QStringList shortcutPathKeys();
    /// 快捷方式一排摆几颗（三条正好一排）。
    static int shortcutPerRow() { return 3; }
    /// 第 @p index 颗快捷方式按钮。
    static QString shortcutButtonName(int index) {
        return QStringLiteral("instanceShortcutButton%1").arg(index);
    }

    /// 「更多目录…」那颗按钮（收起时写「更多目录…」、展开后写「收起更多目录」）。
    ///
    /// 只在目录存在或相关模组可识别时**优先展示**，其他项收在"更多目录"里
    /// —— 它是那两段之间的**唯一开关**。一条"现在没有的"都没有时，**它自己也不摆**
    /// （不摆一颗按下去什么都不变的按钮）。
    static QString shortcutMoreText() { return QCoreApplication::translate("InstanceSettingsDialog", "更多目录…"); }
    static QString shortcutMoreLessText() { return QCoreApplication::translate("InstanceSettingsDialog", "收起更多目录"); }
    static QString shortcutMoreButtonName() { return QStringLiteral("instanceShortcutMore"); }

    /// 这一排里**现在真有东西**的那几个键（`shortcutPathKeys()` 里的值）。
    ///
    /// 判据全在调用方（MainWindow）：目录在不在盘上它是唯一知道"这个实例的根在哪儿、
    /// 装没装 ReplayMod"的地方。**默认是"全都在"**（和那四个 `set*Usable()` 同一个口径）：
    /// 直接 new 出来的对话框这五条全摆在外面、也没有「更多目录」—— 忘了调它只是"多摆了几颗
    /// 按下去会先把目录建出来的按钮"，不是把谁藏起来。
    void setFolderPresentKeys(const QStringList &keys);

    /// 那两段当前的形状（验证工程读它）：五颗按表序；「更多目录」是真有一截收着才摆。
    QPushButton *shortcutMoreButton() const { return m_shortcutMoreButton; }
    bool shortcutMoreExpanded() const { return m_shortcutMoreExpanded; }
    /// 这一刻**收在「更多目录」里**的那几颗（展开之后它们就在外面了）。
    QList<QWidget *> shortcutHiddenButtons() const;

    /// 六页目录列表控件的 objectName。**面板本身仍叫 `panelName(key)`**（那是堆叠页里
    /// 那一格的名字），这个是挂在他里面的列表控件。
    static QString filesPageName(const QString &key) {
        return QStringLiteral("instanceFilesPage-") + key;
    }
    /// 那一页底下那行说明（"数据包要自己搬进存档"这类）。
    static QString filesHintName(const QString &key) {
        return QStringLiteral("instanceFilesHint-") + key;
    }
    /// 第 @p key 那一页底下那行说明的正文。
    static QString filesHint(const QString &key);
    /// 第 @p key 那一页空着时那句话。
    static QString filesEmptyText(const QString &key);

    /// 第 @p key 那一页顶上那颗「下载新 X」上写的字（**空串 = 这一页没有那颗按钮**）。
    ///
    /// 这颗按钮按下去要导向下载页里对应的那一栏 ⇒ 只有**下载页里真有对应一栏的
    /// 那四页**才有它。
    ///
    /// NOTE: 「光影包」那一页上的字是「下载新光影」（不是「下载新光影包」）：这四个字要和
    /// **落地那一栏**说同一句话（下载页那一栏叫「光影」），而左边那一列里这一页仍旧
    /// 叫「光影包」。
    static QString filesDownloadText(const QString &key);

    /// 那颗按钮的悬停提示（说清"会离开这一页、落到哪儿"）。没有那颗按钮的页回空串。
    static QString filesDownloadTip(const QString &key);

    /// 按了第 @p key 那一页顶上那颗按钮之后，下载页该翻到**哪一栏**
    /// （`MainWindowUI::downloadCategoryKeys()` 里的一个键；没有那颗按钮的页回空串）。
    ///
    /// 两套键**现在**正好是同几个字（这四页的页键就是下载页那四栏的键），可它们是两码事：
    /// 这一张说的是"实例设置里这些页叫什么"，那一张说的是"下载页左栏有哪几栏"。所以这里
    /// **显式写一遍**，而不是把 key 直接递出去 —— 将来哪一边改了名（或者这边加了一页而
    /// 下载页里没有对应的一栏），这里是唯一的改写点，而不是一个会静默指错的地方。
    /// ui_check 钉着"回来的每一个键都在 downloadCategoryKeys() 里"。
    static QString filesDownloadCategoryKey(const QString &key);

    /// `filesDownloadCategoryKey()` 的**逆**：下载页那一栏 @p categoryKey 对应实例设置
    /// 里的**哪一页**（回程要用：从那一页点出去的，回来还开在那一页上）。
    ///
    /// 认不出来（这一栏没有对应的文件页）回空串。实现就是拿 `filePageKeys()` 逐个过一遍
    /// 上面那张表 —— 两张表仍然只有一个出处，这里不再抄一遍字面量。
    static QString filesDownloadPageKey(const QString &categoryKey);

    /// 「点一下**名字**就进它自己的那一页」这一条在这几页上开不开。
    ///
    /// 这一张表是**四页**：模组 / 资源包 / 数据包 / 光影包，判据是"名字匹配上了资源"。
    /// 要接别的页时改的是这**一处**（`setFileResource()` 那个循环和构造里那条 connect
    /// 都读它）。
    ///
    /// NOTE: 模组和另外三页**走的是同一条路**（同一个页面对象、同一个返回去处）—— 唯一的
    /// 分别落在那一页**右上角那一格**上：只有"从实例的文件管理页点一个模组进来"才摆
    /// 那颗「模组设置」（见 `ResourceDetailPage::setModSettingsSlot` 和
    /// `MainWindow::DetailSource`）。**别在这儿再加一个"模组要特殊对待"的分支**。
    ///
    /// NOTE: 和 `filesDownloadText()` 那张表**没关系**（原理图那页也没有这一条），别顺手
    /// 把两张表合成一张：那个说的是"顶上有没有那颗按钮"，这个说的是"名字点不点得动"，
    /// 而它们各自会变。
    static bool filesNameOpensResource(const QString &key);

    /// 名字那一格（认出来了的那几条）的悬停提示：说清**点下去会去哪儿**
    /// —— 这一页会关掉，所以必须说（见 InstanceFilesPage::setResourceForPath()）。
    /// @param title 那个资源在接口上的名字。空串（= 没认出来）回空串。
    static QString filesResourceTip(const QString &title);

    // 这几页底下不挂常驻小字：多选是 `MultiSelection`（点一下切换，一个修饰键都不用），
    // "要动手才发现"的事由**操作条自己**说（选中才露）。要再挂这类文字，**照着
    // `InstanceFilesPage::setBatchMode()` 里那段写**，别照抄记忆里的版本。

    /// 「配置文件」那一页上面那颗「上级」。**只有这一页有** —— 别的几页
    /// 看的是一个平铺的目录，而 `config/` 底下还有层级（`config/ftbquests/` 这种），
    /// 得能进去也得出得来。
    static QString configUpButtonName() {
        return QStringLiteral("instanceConfigUpButton");
    }
    static QString configUpText() { return QCoreApplication::translate("InstanceSettingsDialog", "上一级"); }

    static QString serverTreeName() { return QStringLiteral("instanceServerTree"); }
    static QString serverNoticeName() { return QStringLiteral("instanceServerNotice"); }
    static QString serverRefreshButtonName() {
        return QStringLiteral("instanceServerRefreshButton");
    }
    static QString serverEditButtonName() {
        return QStringLiteral("instanceServerEditButton");
    }
    static QString serverAddButtonName() {
        return QStringLiteral("instanceServerAddButton");
    }
    static QString serverDeleteButtonName() {
        return QStringLiteral("instanceServerDeleteButton");
    }
    static QString serverJoinButtonName() {
        return QStringLiteral("instanceServerJoinButton");
    }
    static QString serverRefreshText() { return QCoreApplication::translate("InstanceSettingsDialog", "刷新"); }
    /// 「编辑」：改**选中的那一台**的名字和地址。
    ///
    /// NOTE: 没选中一条时它是**灰的**（见 updateServerSelectionActions()）—— "编辑哪一台"
    /// 这个问题只有一个答案，而那个答案就是选中那一行。
    static QString serverEditText() { return QCoreApplication::translate("InstanceSettingsDialog", "编辑"); }
    static QString serverEditTip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "编辑所选服务器的名称和地址。");
    }
    /// 「添加」：往列表里加一台服务器。
    ///
    /// NOTE: 和「编辑」不同，它**不跟着选中走** —— 要加的那一台还没出现在表里，没有
    /// "先选中"这一步，所以它**常亮**（连一条都没有的时候尤其要能按）。
    static QString serverAddText() { return QCoreApplication::translate("InstanceSettingsDialog", "添加"); }
    static QString serverAddTip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "添加服务器；地址必填，名称可选。");
    }
    /// 「删除」：把选中那一台从列表里拿掉。
    ///
    /// NOTE: 它是这一页上唯一一颗**真会从盘上拿掉东西**的键，所以
    /// ① 没选中一条时灰着（同「编辑」）；② 按下去还有一道问话（`InlineMessage::question`
    /// 的 destructive 那一档，在 MainWindow 里）—— 这一层只发一条信号。
    static QString serverDeleteText() { return QCoreApplication::translate("InstanceSettingsDialog", "删除"); }
    static QString serverDeleteTip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "从服务器列表中移除所选项。");
    }
    /// 「加入服务器」（**临时占位**，见 setServerJoinText()）。
    static QString serverJoinText() { return QCoreApplication::translate("InstanceSettingsDialog", "加入服务器"); }
    static QString serverJoinTip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "启动游戏并加入所选服务器，需要游戏支持快速加入。");
    }
    /// 「编辑」「删除」「加入」三颗在**没选中一条**时按不动 —— 悬停提示里说清为什么。
    /// （「添加」也在这一组旁边，但它不跟着选中走，见 `serverAddText()` 那条 NOTE。）
    static QString serverSelectFirstHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "请先选择服务器。");
    }
    /// 「刷新」正在重读 + 重测的时候那颗按钮上的字。
    static QString serverPingingText() { return QCoreApplication::translate("InstanceSettingsDialog", "刷新中…"); }
    /// 还没测过的那一格写什么（**不是空着**：空着和"测了没通"在屏幕上是同一个样子）。
    static QString serverUntestedText() { return QCoreApplication::translate("InstanceSettingsDialog", "未检测"); }
    /// 超时那一档在界面上**就写这两个字**，不加别的解释。
    ///
    /// NOTE: 和 `Status::timedOut` 那一位配着用 —— 那一位才是判据，这一句只是写出来的字。
    /// 那一格窄（`ResizeToContents`），而「它没答话」和「网络不通」这两个理由对这个
    /// 格子的读者（想点进去玩的人）是同一件事：现在连不上。
    static QString serverTimeoutText() { return QCoreApplication::translate("InstanceSettingsDialog", "超时"); }
    /// 右键服务器行弹出的菜单：objectName 与三项的文字（验证工程按它们取控件）。
    static QString serverMenuName() { return QStringLiteral("serverMenu"); }
    static QString serverMenuJoinName() { return QStringLiteral("serverMenuJoin"); }
    static QString serverMenuEditName() { return QStringLiteral("serverMenuEdit"); }
    static QString serverMenuCopyName() { return QStringLiteral("serverMenuCopy"); }
    static QString serverMenuJoinText() { return QCoreApplication::translate("InstanceSettingsDialog", "加入"); }
    static QString serverMenuCopyText() { return QCoreApplication::translate("InstanceSettingsDialog", "复制地址"); }
    // NOTE: 状态那一格**不加任何前缀**（连「上次：」都不行）—— 缓存里那一份和当场
    // 测出来那一份写的是同一句字。要在界面上给这一格加前缀，等于重新发明一句不该出现的话。
    static QString serverNameColumnText() { return QCoreApplication::translate("InstanceSettingsDialog", "名称"); }
    static QString serverAddressColumnText() { return QCoreApplication::translate("InstanceSettingsDialog", "地址"); }
    static QString serverStatusColumnText() { return QCoreApplication::translate("InstanceSettingsDialog", "状态"); }

    /// 设置页「隐藏服务器地址」（`Look/HideServerAddress`，默认关）。每次灌列表时读一次。
    static bool hideServerAddress();
    /// 地址那一栏默认显示用户填写的**原文**；「隐藏服务器地址」打开时显示 `maskedServerAddress()`。
    static QString shownServerAddress(const QString &address, bool hide);
    /// 地址太长放不下时省略中间：保留开头和 `:端口`（没有端口就中间各留一截）。放得下就原样返回。
    static QString elidedServerAddress(const QString &text, const QFontMetrics &metrics, int width);

    /// 「隐藏服务器地址」打开时地址那一栏显示的**遮过的**写法。
    ///
    /// 规矩：
    /// - **域名** —— 每一个点分开的段取**首字母 + 一条横杠**（`mc.hyperlands.cloud`
    /// ⇒ `m-.h-.c-`）；
    /// - **数字地址**（四段全数字的 IPv4 / 带冒号的 IPv6）—— 留头两段、后面一律 `*`
    /// （`123.45.67.89` ⇒ `123.45.*.*`）；
    /// - `[...]` 那种方括号**留着**，`:` 后面那个端口**照抄**。
    ///
    /// NOTE: 认不出来（空串、光一个方括号）就**原样回**，不猜也不报错：这一格只是个显示，
    /// 真正的"这个地址能不能用"由 ServerPing::splitAddress 说了算。
    static QString maskedServerAddress(const QString &address);
    static QString serverEmptyText() { return QCoreApplication::translate("InstanceSettingsDialog", "暂无服务器"); }
    /// 服务器名字那一格是空的的时候顶上什么字。servers.dat 里的 name 真能是空串。
    static QString unnamedMark() { return QCoreApplication::translate("InstanceSettingsDialog", "（无名）"); }

    /// 「光影包」那一页用不上时那句话（唯一出处，验证工程也按它核对）。
    static QString shaderUnavailableText();
    /// 「结构/原理图」那一页用不上时那句话。
    static QString schematicUnavailableText();
    /// 「模组」那一页用不上时那句话（这个实例没有模组加载器）。
    static QString modUnavailableText();
    /// 「数据包」那一页用不上时那句话（这个游戏版本还没有数据包）。
    static QString datapackUnavailableText();
    /// 第 @p key 那一页**为什么用不上**（用得上 = 空串）。**唯一分发处** —— 页里那句
    /// 「用不上」和勾选表的悬停提示都走它，别再各写一遍 if 链。
    static QString pageUnavailableText(const QString &key);

    // ---- 「显示哪些页」那张勾选表（概览页最下面）----
    //
    // 七条一条一个勾选框（顺序见 optionalPageKeys()），**不可用的那一条不勾、灰着、
    // tooltip 说为什么**。勾/不勾直接决定左栏那一列里有没有这一页。
    //
    // / 第 @p key 条勾选框的 objectName。
    static QString pageCheckName(const QString &key) {
        return QStringLiteral("instanceSettingsShowPage-") + key;
    }
    /// 那一小块的字（**它的行名**）。
    static QString showPagesLabelText() { return QCoreApplication::translate("InstanceSettingsDialog", "显示哪些页"); }
    /// 可用那几条的 tooltip（"勾上左边那一列里就有这一页…"）。
    static QString pageShownHint();

    /// 第二行右边那颗按钮上的字（**唯一出处**：它是打开那个浮层的按钮，也是这一项的
    /// 名字 —— 概览这一栏没有单独的行名）。
    static QString groupButtonText() { return QCoreApplication::translate("InstanceSettingsDialog", "分组…"); }
    /// 第三行那个勾选框上的字。它说的是"在收藏组中另留一条快捷入口"，
    /// **不是把实例搬过去**。
    static QString pinCheckText() { return QCoreApplication::translate("InstanceSettingsDialog", "加入收藏"); }
    /// 「更改图标」/「导出…」两颗按钮上的字。
    static QString changeIconText() { return QCoreApplication::translate("InstanceSettingsDialog", "更改图标"); }
    /// 实例操作 → 导出页的主动作。
    static QString exportText() { return QCoreApplication::translate("InstanceSettingsDialog", "导出整合包…"); }
    /// 浮层里那两个动作按钮上的字。
    static QString iconPickText() { return QCoreApplication::translate("InstanceSettingsDialog", "选一张图片…"); }
    static QString iconResetText() { return QCoreApplication::translate("InstanceSettingsDialog", "恢复默认"); }
    /// 八个内置候选在界面上叫什么（顺序与 VersionIcons::iconCandidateKeys() 一一对应；
    /// 后四个故意同一个字：加载器名在设置页 / 安装页里就叫这个）。
    static QStringList iconCandidateTexts();
    /// 「分组…」那个浮层里第一项那两个字。**唯一出处是 MainWindowUI::ungroupedGroupText()**，
    /// 这里只是把它转出来（本文件不 include MainWindowUI.hpp，所以实现写在 .cpp 里）。
    static QString ungroupedGroupText();
    /// 「更改图标」那个浮层下面那句话。
    static QString iconHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "选择实例图标。");
    }
    /// 勾选框下面那句话：两边各摆一条，说明白"原组那份不动"。
    ///
    /// NOTE: 它得说清三件事：勾上之后**哪儿多出一条**、**原来那份不动**、取消勾选之后
    /// 哪一条没了 —— 少说中间那一件，用户就会以为勾上等于"搬家"。
    static QString pinHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "添加到收藏，原分组不变。主页按显示数量和最近启动时间展示。");
    }
    /// 第四行那颗按钮下面那句话（**看得见的那一行**，短）。
    ///
    /// NOTE: 一句两用的写法在这里**不成立**：那行字要短（它在这儿是**窄栏里的折行标签**，
    /// 长了会把整页的 sizeHint 顶起来 —— 页面上那条 E5 断言钉的就是它），而 tooltip
    /// 该说全（.mrpack / .zip 的区别、认不出来的东西会怎样）。所以拆成两个函数。
    static QString exportHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "导出可识别的资源清单和配置为 Modrinth 或 CurseForge 整合包。需要保留完整的实例数据时，请创建快照。");
    }
    /// 那颗按钮的 tooltip（**说全的那一份**）。
    static QString exportButtonTooltip() {
        return QCoreApplication::translate("InstanceSettingsDialog", "支持 Modrinth（.mrpack）和 CurseForge（.zip）。不含存档和截图。");
    }
    /// 那一格下面那句解释（**唯一一处**说清"改这里 = 改磁盘上的目录名"）。
    static QString instanceNameHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "重命名实例目录及核心文件。");
    }
    static QString advancedToggleName() { return QStringLiteral("instanceAdvancedToggle"); }
    static QString advancedBoxName() { return QStringLiteral("instanceAdvancedBox"); }
    // NOTE: 这一页**没有「确定」也没有「取消」**（改了就写）⇒ 覆盖层页眉上那颗返回
    // 箭头就是这一页唯一的出口（Esc 同它）。

    /// 这一页叫什么（「实例设置」）。页面型覆盖层把它写在自己那条页眉（面包屑）上。
    /// 实例名不拼进去：页内页眉那一行大字已经是实例名，拼进去就是两行重复。
    QString titleText() const;

    /// 把页内页眉那行大字换成新的实例名（@param displayName 是**显示名**，不是目录名）。
    /// 实例在这一页开着时会改名（「实例名」那一格），页眉要跟着换。
    ///
    /// NOTE: 和 `setInstanceName()` 是两件事：那个换的是**控件上那一格**和
    /// `m_instanceId`（拿的是目录名），这个换的是页眉大字（拿的是显示名）——
    /// 带自定义名的实例这两个值本来就不是一回事。
    void setInstanceDisplayName(const QString &displayName);
    /// 页眉名字下那一行（加载器 · 游戏版本）；空 = 不显示。
    void setHeaderMeta(const QString &meta);

    /// 关页之前先看一眼「配置文件」那一格里有没有没保存的改动。
    ///
    /// 走这条路的**不是**编辑器自己那颗「返回」（那颗发 `backRequested()`，由 MainWindow
    /// 接到 `leaveConfigEditor()` 上），而是**覆盖层页眉那颗返回箭头** —— 它接的是
    /// `dialog->reject()`（见 `OverlayHost::showPage` 里那段），点一下关掉的是**整个
    /// 「实例设置」**。而那一格里可能正压着改了一半的东西。
    ///
    /// 这一层没有覆盖层、弹不出"要丢掉吗"那一句，所以它只能**先不关**、把这一发交出去：
    /// 接的人问完，用户答"丢掉"之后拿 `done(QDialog::Rejected)` 关页（**不是** `reject()`
    /// —— 那会又走回这儿）。
    ///
    /// NOTE: 编辑器那一格没开着、或者它是干净的，这一条一个字都不做（照旧正常关页）。
    void reject() override;

    /// 把「实例名」那一格改成 @p name（**控件上那一格**，不碰盘）。
    ///
    /// 两个调用点都在 MainWindow 那条改名路上：改成功了要让它显示新名字（那一格从此
    /// 就是新目录名）、改失败了要**把它退回原样**（否则用户眼前那一格写着"改好了"，
    /// 而磁盘上还是老名字 —— 下一次动任何一个开关都会拿这一个不存在的名字去写 meta）。
    /// 它顺手把导出脚本那个默认文件名用的 `m_instanceId` 一起换掉。
    void setInstanceName(const QString &name);

    /// 档位下拉框里那两项的文字。**这两句是产品文案**，验证工程按它们核对而不是自己拼。
    static QString followText() { return QCoreApplication::translate("InstanceSettingsDialog", "跟随全局"); }
    static QString customText() { return QCoreApplication::translate("InstanceSettingsDialog", "自定义"); }
    /// 「跟随全局（当前 %1）」。@param current 全局那一项的值的可读写法。
    static QString followHint(const QString &current) {
        return QCoreApplication::translate("InstanceSettingsDialog", "跟随全局（当前 %1）").arg(current);
    }
    /// 自动档下两行内存数字框的提示（也是它们被禁用的原因）。
    static QString memoryAutoDisabledHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "自动分配内存时，手动设置不生效。");
    }
    /// 高级选项那个折叠按钮上的字。默认折叠 —— 里面两项都是"改错了会让游戏起不来"
    /// 的东西，不该一打开对话框就摆在眼前。
    static QString advancedToggleText() { return QCoreApplication::translate("InstanceSettingsDialog", "高级选项"); }

    /// 追加参数那行提示的正文（也是"一条一行"这条约定的唯一出处）。
    ///
    /// NOTE: 「之后」那两个字**不许写成 `**之后**`** —— 这一页上没有任何一处设过
    /// `setTextFormat(Qt::MarkdownText)`，星号会原样画在用户眼前（全项目同一条）。
    static QString appendJvmArgsHint() {
        return QCoreApplication::translate("InstanceSettingsDialog", "每行一条，追加在全局参数之后；相同参数以最后一项为准。");
    }

protected:
    void showEvent(QShowEvent *event) override;

    /// 只为一件事装在这儿：**左栏那一列不许因为"拿到焦点"就自己挑一条**（见 .cpp 里
    /// 实现上面那一整段 —— 那是 Qt `focusInEvent()` 的一个兜底行为，会让用户按一下
    /// Tab、或者窗口重新激活就凭空翻一页）。
    bool eventFilter(QObject *watched, QEvent *event) override;

    /**
     * @brief 有东西被拖到这一页上。
     *
     * 这一页**只报到"落了哪些文件"**，一个字的判断都不做：认类型要读压缩包的条目表
     * （`DroppedResource`）、装进去要知道实例目录在哪（`AppPaths`）—— 两件都在
     * `src/` 那一层，`ui/` 够不着（这一条是全仓的分层约定，见 .cpp 顶上那段）。
     * 所以它发 `filesDropped()`，接的人是 MainWindow。
     *
     * NOTE: 为什么在这一层接：拖放事件从光标底下那个控件沿着父链往上冒，**第一个**
     * `acceptDrops()` 为真的控件接住它（见 .cpp 里 `dropEvent` 那段）。这一页里
     * 绝大多数的子控件（标签、勾选框、下拉框）都不接拖放，所以拖到它们身上照样
     * 冒到这儿来。例外是那几个输入框（`QLineEdit` 自己接拖放、会把路径当文字插进去）
     * —— 拖到输入框正中间那一小块上时走的是它自己那条路，这是 Qt 本来的行为。
     */
    void dragEnterEvent(QDragEnterEvent *event) override;

    /// @copydoc dragEnterEvent
    void dropEvent(QDropEvent *event) override;

signals:
    /// @p key 那一页**刚建出来**（第一次切过去，见 ensurePage()）。
    ///
    /// 给"要在某一页的控件上接线"的调用方用：页是懒建的，开页那一刻 `findChild()` 摸到的
    /// 是空的，只能等这一发。**只在建出来那一次发**，切回来不再发。
    void pageBuilt(const QString &key);
    void resourcePackPageEntered();
    /// 左栏底部的固定入口；真正启动仍统一交给 MainWindow 的启动链。
    void launchInstanceRequested();

    /**
     * @brief 页面上的值**改完了**，该写盘了（这一页没有「确定」）。
     *
     * 这一页是"改哪一格就写哪一格"：
     *
     * - 这一发**只是"值变了"**，不含任何"该写成什么" —— 写成什么由 MainWindow 拿
     * `override()` / `appendJvmArgs()` / `iconChoice()` 那几问现读（和写整份设置读的
     * 是同一批函数，所以两边落到盘上的字节是一样的）。
     * - **值没变就不发**（见 .cpp 里 notifyEdited() 那段）：点一下已经选中的那一项、
     * 把一个数改回原样，都不该让磁盘动一次。
     * - 同一发可能由"用户操作完成"以外的原因触发（回车 / 离开那一格），这条线只认值，
     * 不认手势。
     *
     * NOTE: 改名**不走这一发**（见 renameRequested()）：那一格动的是目录名，不能和
     * 别的值挤在同一个"写一次设置"里 —— 顺序错了会在盘上留下一个空壳实例
     * （理由写在 MainWindow::openInstanceSettings() 那段）。
     */
    void settingsEdited();
    /// 用户点了概览里的「重置游玩时间」；清掉这个实例已记录的累计与上次时长。
    void resetPlayTimeRequested();

    /**
     * @brief 「实例名」那一格改完了（用户按下回车，或者离开那一格）。
     *
     * @param name 那一格里的字（**原样**，不 trim、不改大小写 —— 合不合法由
     * `InstanceFiles::validateRenameTarget()` 判，界面不抄第二份规则）。
     *
     * 这一发是**改名那条路**的入口：接了的人负责改目录名、
     * 失败时把这一格退回去（setInstanceName()）。
     *
     * NOTE: 每一格按键都发的话，用户打字打到一半目录就被改一次（"我"→"我的"→
     * "我的整合包"），中间那几次全是白改，而且失败一次就会打断打字。所以这一发
     * 挂在 `QLineEdit::editingFinished` 上（回车 / 失焦），不是 `textChanged`。
     */
    void renameRequested(const QString &name);

    /**
     * @brief 用户点了「导出…」，并且已经在 Windows 自己的保存框里挑好了一个路径。
     *
     * 这一层只**问路径**（它本来就在用 QFileDialog 问 java.exe 和图标），真正的
     * "这一次启动到底执行什么"在 MainWindow 里 —— 那边才有 authManager / versionManager
     * / launcherManager。这样切还有一条实在的好处：保存框是**驱动不了**的（见 .cpp 里
     * browseJavaButtonName 那段 NOTE），把"问"留在这一层之后，MainWindow 那一半就能被
     * 验证工程拿一条路径直接驱动。
     *
     * @param path 用户挑的完整路径（带 .bat；用户没写扩展名时保存框自己会补）。
     */
    void exportLaunchScriptRequested(const QString &path);

    /**
     * @brief 用户点了概览那一栏的「导出…」，并且已经在 Windows 自己的保存框里挑好了路径。
     *
     * 和上面 `exportLaunchScriptRequested` **同一个形状、同一套理由**（这一层只问路径，
     * 干活在 MainWindow）；区别是这一件要慢得多 —— 枚举实例目录、算哈希、联网反查、
     * 打包，都在那边，进度写底栏那一格。
     *
     * NOTE: **格式是从扩展名读出来的**，不另设一档：保存框里那两个筛选项各自带着扩展名
     * （`.mrpack` / `.zip`），用户挑了哪个、或者自己敲了哪个，选的就是哪套规范。
     * 这一层保证交出去的路径**一定**带这两个之一（见 .cpp），所以那边不用兜底。
     *
     * @param path 用户挑的完整路径（带 `.mrpack` 或 `.zip`）。
     */
    void exportModpackRequested(const QString &path);
    void exportPresetRequested();

    /**
     * @brief 用户点了概览那一栏的「检查更新」。
     *
     * NOTE: **不传实例 id** —— 这一页就是某个实例的设置页（`setInstanceDir()` / 那一份
     * wtfml.meta 都是它的），接的人从自己那边认（MainWindow 在 openInstanceSettings
     * 里记着开的是哪一个）。
     */
    void checkUpdatesRequested();

    /**
     * @brief 用户点了概览那一栏的「更新中心」。
     *
     * 和上面那一颗同一档（一颗按钮 + 底下那句解释，点了之后**会出现一个结果**），但**不联网**：
     * 只是把上一次「检查更新」查出来的那份结论摆成能动手的一页（`MainWindow::openUpdateCenter`）。
     * 那一页也是「撤销上一次更新」唯一进得去的地方 —— 所以这一发必须在"没有网络"的时候
     * 照样能用（更新完发现游戏起不来，正是要退回去的那一刻）。
     *
     * NOTE: **不传实例 id**，理由和 checkUpdatesRequested() 那条一样（这一页就是某个实例的
     * 设置页）。
     */
    void updateCenterRequested();

    /**
     * @brief 用户点了概览那一栏的「复制实例」。
     *
     * 和上面那颗一模一样：这一层**只发一句话** —— 挑目标游戏文件夹（一个 QFileDialog）、
     * 问副本叫什么（一句 InlineMessage，要覆盖层）、真去拷那一棵树（要 AppPaths）三步
     * 全在 MainWindow。这一层连"往哪儿拷"都够不着。
     *
     * NOTE: **不传实例 id、也不传目标** —— 理由和 checkUpdatesRequested() 那条一样
     * （这一页就是某个实例的设置页），而且目标和名字都要用户下一步才给。
     */
    void copyInstanceRequested();

    /**
     * @brief 用户点了概览那一栏的「删除实例」。
     *
     * 同一个动作的**第二个入口**（实例行行尾那颗垃圾桶是第一个）。这一层只发一句话：
     * 「真的要删吗」那一问、两道闸（正在跑 / 正在装）、以及真去删目录，全在
     * `MainWindow::confirmDeleteInstance()` —— 一行都没有在这儿重写一遍。
     *
     * NOTE: **这一发之后页面上那个实例就没了** ⇒ 接的人删完要**顺手把这一页收掉**
     * （`OverlayHost::dismiss()`），否则用户会停在一页写着"某个已经不存在的实例"的
     * 设置上，随便动一格都会去写一个不存在的目录。
     */
    void deleteInstanceRequested();

    /**
     * @brief 用户点了「开始检查」。
     *
     * **能不能开始、要不要先问一句，全由 MainWindow 说了算** —— 只有它知道有没有下载在跑、
     * 这个实例这会儿装没装。这一层只报"用户想干这件事"。
     */
    void repairFilesRequested();

    /**
     * @brief 用户点了概览栏那颗「检查并修复」。
     *
     * **能不能开始、扫完怎么办，全由 MainWindow 说了算** —— 只有它知道有没有下载在跑、
     * 这个实例这会儿装没装、任务账本怎么记。这一层只报"用户想做一次体检"。
     */
    void integrityCheckRequested();

    /**
     * @brief 用户点了概览栏那颗「创建快照…」。
     *
     * 按下去之后 MainWindow 先问一句"要不要连着能重新下回来的那部分一起打包"，然后起一趟
     * 后台打包、记进任务账本。**这一层不碰盘**（见 .hpp 里 createSnapshotText 那段）。
     */
    void createSnapshotRequested();

    /**
     * @brief 用户点了概览栏那颗「从快照恢复…」。
     *
     * 挑哪一份、确认、以及"还原之前先给现在这一份做安全快照"全在 MainWindow 那边。
     */
    void restoreSnapshotRequested();
    void inspectRecoveryRequested();

    /**
     * @brief 用户在一个文件夹上按了「打开」（快捷方式那九颗按钮，或者某一页的「打开文件夹」）。
     *
     * **这一层不自己开**：开目录是 QDesktopServices 的事，开不了要弹一句，而弹话的
     * InlineMessage 每一个入口都要 OverlayHost —— 那个在 MainWindow 手里。
     * @param path 要打开的目录（绝对路径）。
     */
    void openInstanceFolderRequested(const QString &path);
    void openInstanceFileRequested(const QString &path);
    /// 日志页 / 崩溃报告页双击一行：在内置查看器里打开（认 gzip 头，`.log.gz` 也行）。
    void openLogFileRequested(const QString &path);
    void backupSelectedWorldRequested(const QString &path);
    void restoreSelectedWorldRequested(const QString &path);

    /// 「Replay」/「结构」那一页里**选中的那一条换了**。
    ///
    /// 接的人负责把 `metaData.json` / 结构 NBT 读成一句话，再拿
    /// `setLifecycleInfoText(key, path, …)` 递回来。**这一层不读盘**（它连
    /// `AppPaths` 都够不着），所以选不中 / 空目录时它一个字都不发。
    void lifecycleFileInfoRequested(const QString &key, const QString &path);
    void lifecycleBatchRequested(const QString &key, const QStringList &paths, LifecycleBatchAction action);

    void diagnosisRequested();

    /// 用户在「迁移」/「升级」那一页换了一个目标。
    /// @param key `migration` / `upgrade`
    /// @param targetId 目标实例 / 目标版本的 id（下拉框的 data）。
    ///
    /// 接的人按"源实例 + 这个目标"重算一份计划，拿 `setTransferPlan()` /
    /// `setTransferItems()` 摆回来。**这一层不读盘**，换目标时也不会自己发第二发。
    void transferTargetChosen(const QString &key, const QString &targetId);
    void prepareUpgradeTargetRequested(const QString &gameVersion);
    void updateWizardRequested();
    void stopUpgradeQueryRequested();

    /// 用户按了「升级」那一页上的「重新检查」（那颗按钮在页里，见 buildLifecyclePage()）。
    ///
    /// NOTE: 转成这一发是因为**页是第一次切过去才建的**：接的人（`UpgradeAssessment`）
    /// 在开页那一刻就 attach 了，那时那颗按钮还不存在，`findChild()` 摸到的是空的 ——
    /// 直接接按钮的 `clicked` 会静默接不上（点了什么都不发生）。和 `stopUpgradeQueryRequested`
    /// 同一个做法：按钮在页里发，接的人在对话框上接。
    void upgradeRecheckRequested();

    /// 用户按了「搬过去…」/「创建升级副本…」。**只发意图，不写盘**：路径闸、运不运
    /// 行、要不要问名字都在 controller 那一层。
    /// @param key `migration` / `upgrade`
    /// @param targetId 下拉框里选中的那个（空串 = 还没选目标）
    /// @param selectedKeys 用户勾着的那些范围键
    void transferApplyRequested(const QString &key, const QString &targetId,
                                const QStringList &selectedKeys);
    void upgradeInPlaceRequested(const QString &targetId);
    void rollbackUpgradeRequested();

    /**
     * @brief 用户按了某一条行尾的「禁用」/「启用」。
     *
     * 加后缀还是去后缀**由接的人判**（看路径是不是以 `.disabled` 结尾就行，不用另一格数据）。
     * 这一层连读都没读，所以它不知道那个文件现在是什么状态 —— 只有"用户点了这一条"。
     * @param path 这一条**现在**的绝对路径。
     */
    void toggleInstanceFileRequested(const QString &path);

    /**
     * @brief 用户按了某一条行尾的「删除」。
     *
     * **"真的要删吗"那句由接的人问** —— 理由同上（这一层没有 OverlayHost）。
     */
    void deleteInstanceFileRequested(const QString &path);

    /**
     * @brief 用户在「配置文件」那一页**打开了一个文件**（双击，或者选中之后按回车）。
     *
     * 接的人负责**读那个文件**，然后拿它调 `openConfigEditor()` 把编辑器那一格切出来
     * —— 这一层手上没有 OverlayHost，也没有盘（读、解码、失败要弹的那一句，全是
     * MainWindow 的）。
     *
     * NOTE: **这一发不是"开一整页覆盖层"**。点这一行的那一刻，「实例设置」自己正占着
     * 覆盖层那唯一的一层，开不出第二个整页（见 configFilePage() 上面那一段）。
     *
     * NOTE: **只有文件走这一发**。点一个目录是"进去看"，那件事这一层自己就办完了
     * （换一个目录再列一遍，纯显示层的事，不碰盘不联网），不会绕到这儿来。
     * @param path 这一条的**绝对路径**。
     */
    void openConfigFileRequested(const QString &path);

    /**
     * @brief 用户想关掉整个「实例设置」，而**「配置文件」那一格里还有没保存的改动**。
     *
     * 见 `reject()` 上面那一段：这一层已经把那一关**挡下来**了（没关），接的人问完那一句
     * 之后自己决定 —— 用户说"丢掉"就 `done(QDialog::Rejected)`，说"留在这儿"就什么都不做
     * （页继续开着，编辑器那一格原样）。
     */
    void closeBlockedByUnsavedConfigFile();

    /**
     * @brief 用户想关掉整个「实例设置」，而**「世界设置」那一页里还有没保存的改动**。
     *
     * 和 `closeBlockedByUnsavedConfigFile()` 是同一件事的第二个实例（那一条的说明逐字
     * 适用）：那颗返回箭头走的是 `dialog->reject()`，点一下关掉的是**整个**「实例设置」，
     * 而这一格里可能正压着改了一半的游戏规则。这一层弹不出"要丢掉吗"，所以只能先挡住、
     * 把这一发交出去。
     */
    void closeBlockedByUnsavedWorldSettings();

    /**
     * @brief 世界设置那一格露出来了 / 收回去了。
     *
     * @param title 露出来时是页眉面包屑要写的那句（「世界设置：<世界名>」），收回去时是空串。
     * 这一层没有覆盖层，页眉上的字和「返回」点下去回哪儿都是覆盖层的事，所以把这一发交出去，
     * 由 InstanceDialogController 接着改页眉、设返回钮的去向。
     */
    void worldBreadcrumbChanged(const QString &title);

    /**
     * @brief 用户按了「文件管理」某几页顶上那颗「下载新 X」⇒ 请切到下载页对应的那一栏。
     *
     * @param categoryKey `MainWindowUI::downloadCategoryKeys()` 里的一个键 —— 就是那一页
     * 对应的一栏（写法见 filesDownloadCategoryKey()）。**是键不是行号**：下载页左栏
     * 的行序由界面自己摆，这一层不该知道它。
     *
     * NOTE: 这一层只发信号，自己什么都不做（不关这一页、也不碰选项卡）—— "收页之后再切到
     * 哪儿"的顺序只有 MainWindow 那边知道（见 OverlayHost::dismiss() 那段 ★）。
     */
    void browseDownloadsRequested(const QString &categoryKey);

    /**
     * @brief 用户**点了某一条行尾那颗「详情」**，而这一条认得出是哪个资源 ⇒ 请打开它自己的
     * 那一页。
     *
     * 触发点是那颗按钮，不是点一下名字（见 filesNameOpensResource()）。
     *
     * @param pageKey 哪一页发出来的（`filePageKeys()` 里的一个键 —— 也就是"这是个资源包
     * 还是光影包"，见 filesNameOpensResource()）。**是键不是下标**。
     * @param path 那一条的**绝对路径**。注意给的是路径不是资源身份：身份是 MainWindow 那张
     * 反查表里的东西（`InstanceFilesPage` 和这一层都不知道一个文件对应哪个项目）。
     *
     * NOTE: 这一层只发信号，自己什么都不做（不关这一页）—— 和 browseDownloadsRequested()
     * 同一个理由：收页和"之后开哪一页"的先后由 MainWindow 那边排。
     */
    void fileResourceRequested(const QString &pageKey, const QString &path);

    /**
     * @brief 用户在某一页的**悬浮操作条**上按了「更新」：把选中的这些升到最新版。
     *
     * @param pageKey 哪一页发出来的（和 fileResourceRequested() 同一套键）。
     * @param paths 选中的那些条目的**绝对路径**（现在盘上那个名字）。
     *
     * NOTE: 这一层不知道"最新版是哪一版" —— 那是 MainWindow 手里那张表的事（见
     * InstanceFilesPage::batchUpdateRequested 那段）。
     */
    void fileBatchUpdateRequested(const QString &pageKey, const QStringList &paths);

    /**
     * @brief 用户在悬浮操作条上按了「禁用」/「启用」：把选中的这些整体改成那个状态。
     *
     * @param pageKey 同上。
     * @param paths 选中的那些条目的绝对路径（禁用着的那些带 `.disabled`）。
     * @param disable true = 禁用（挂后缀），false = 启用（去后缀）。
     */
    void fileBatchToggleRequested(const QString &pageKey, const QStringList &paths, bool disable);

    /**
     * @brief 用户在悬浮操作条上按了「删除」。
     *
     * **"真的要删吗"那句由接的人问** —— 而且对**整批**只问一次（理由同
     * deleteInstanceFileRequested：这一层没有 OverlayHost）。
     */
    void fileBatchDeleteRequested(const QString &pageKey, const QStringList &paths);
    void modTransferRequested(const QStringList &paths, bool move);
    void modCheckSelectedUpdatesRequested(const QStringList &paths);
    void modWebsiteRequested(const QString &path);
    void modInspectDependenciesRequested(const QStringList &paths);

    /**
     * @brief 用户把几个文件**拖到这一页上**了。
     * @param paths 落下来的本地文件绝对路径（原样；至少一个 —— 一个都没有时根本不发这一发）。
     *
     * 见 `dragEnterEvent()` 那段：这一层只把"落了什么"交出去，认类型和落盘都在 MainWindow。
     */
    void filesDropped(const QStringList &paths);

    /**
     * @brief 用户按了服务器列表那一页的「刷新」。
     *
     * 那一页的内容是**解析好的结果**递进来的（setServerList()），重读一遍要碰 src/ 里的
     * NBT 解析，所以只能由 MainWindow 接。
     *
     * 这一条是**这一页上唯一一条"重新看一眼"的信号**：接的人该做满
     * 两件事：**重读一遍 servers.dat** + **把每一台重测一遍**。「刷新」的意思是"把这一页
     * 刷成现在的样子"，而这一页不只是那份文件的镜像（图标、MOTD、延迟都是刚问来的）。
     *
     * NOTE: 进这一页（`serverPageEntered()`）走的是**只重测**那一半 —— 刚打开的一页没有
     * "重读"的道理（表本来就是刚递进来的），但延迟该当场问一次。
     */
    void refreshServerListRequested();

    /**
     * @brief 用户在右键菜单里选了「复制地址」 ⇒ 请把那一串**完整的**地址放进剪贴板。
     *
     * 给出去的是**没遮过**的那一串（`host:port`，就是 servers.dat 里写的那个），和「隐藏服务器地址」
     * 开不开无关：复制是用户自己的操作。
     *
     * NOTE: 这一层**不碰剪贴板**（也不写日志、不弹提示）：和 `copyNameRequested` 那边同一条路数 ——
     * 复制出去的东西进哪儿由调用方说了算，而且这一层够不着 MainWindow 的日志。
     */
    void copyServerAddressRequested(const QString &address);
    void serverFavoriteRequested(const QString &address, bool favorite);

    /**
     * @brief 用户按了服务器列表那一页的「编辑」。
     * @param name 选中那一行在 `servers.dat` 里**真正**的名字（可以是空串）。
     * @param address 选中那一行**完整**的地址（没遮过的，和 copyServerAddressRequested 同）。
     *
     * 问那两句（名字 / 地址）、以及**写盘**都不在这一层：前者要覆盖层、后者要
     * `src/manage/ServerListFile`（那一层不 include src/）。这一层只把"要改的是哪一台"
     * 交出去 —— 而且交出去的是**改之前**的样子：落盘那边要靠这两串在 `servers.dat` 里
     * 认出那一条（**按值匹配，不按下标** —— 见 ServerListFile 那段）。
     *
     * NOTE: 名字给的是 `serverNameRole()` 里那一份，**不是**屏幕上显示的那一格 —— 空的
     * 名字屏幕上写的是「（无名）」，那是我们编的话，不能拿去当数据。
     */
    void editServerRequested(const QString &name, const QString &address);

    /**
     * @brief 用户按了服务器列表那一页的「添加」。
     *
     * 这一发**不带参数** —— 要加的那一台还没出现在表里，这一层没有它的名字和地址可给；
     * 问那两句（名字可空 / 地址必填）和写盘都在 MainWindow（前者要覆盖层，后者要
     * `src/manage/ServerListFile`，这一层一样都够不着）。
     */
    void addServerRequested();

    /**
     * @brief 用户按了服务器列表那一页的「删除」。
     * @param name 选中那一行在 `servers.dat` 里**真正**的名字（可以是空串）。
     * @param address 选中那一行**完整**的地址（没遮过的）。
     *
     * 参数和 `editServerRequested` **逐字同义**（都是"改之前的样子"，落盘那边按值在
     * `servers.dat` 里认出那一条 —— 见 ServerListFile 那段）；差别只在接的人要做的事：
     * 这一条要先问一句（不可撤销的定夺），再去删。
     *
     * NOTE: 这一层**不问话也不删** —— 覆盖层和 `src/` 它都够不着。
     */
    void deleteServerRequested(const QString &name, const QString &address);

    /// 选中了多台时按「删除」：(名字, 地址) 列表，含义同 `deleteServerRequested`。
    /// 只选一台时仍发 `deleteServerRequested`。
    void deleteServersRequested(const QList<QPair<QString, QString>> &entries);

    /**
     * @brief 用户按了服务器列表那一页的「加入服务器」（**临时占位**）。
     * @param name 选中那一台的名字（同上，可以是空串；回话里用得着）。
     * @param address 选中那一台**完整**的地址 —— 这一串就是 `--quickPlayMultiplayer` 的
     * 值（`host[:port]`，游戏自己认得这个写法）。
     *
     * 发给谁、发什么参数、游戏认不认，全在调用方（见 MainWindow::startQuickPlay()）。
     * 这一层连"这个版本有没有那个参数"都不知道（要读版本 json）。
     */
    void joinServerRequested(const QString &name, const QString &address);

    /**
     * @brief 用户**切到了服务器列表那一页**。
     *
     * 这一发是"这一页露出来了"这件事本身 —— 接到的人该去测一遍。它和上面那两条
     * **不是一回事**：那两条说的是"用户按了某颗按钮"，这一条说的是"这一页被打开了"。
     *
     * NOTE: 每一页都会发一次（切回「概览」再切回来还会再发）—— 那正是"点进去就测"该有的
     * 行为。"正在测的时候又切进来一次"不会真的并发出两趟：接的人那边有一道
     * "已经在跑就不重开"的闸（ServerPing::Pinger::start 自己那条）。
     */
    void serverPageEntered();

private:
    ResourcePackPage *m_resourcePackPage = nullptr;
    /// 「编辑」「删除」「加入服务器」三颗按钮的**可用性**照"现在选没选中一行"重算一次。
    ///
    /// 没选中任何一行时三颗都是**灰的**：这三件事都要一个明确的对象（"改哪一台"、"删哪
    /// 一台"、"连哪一台"），而那个对象只有选中那一行能给。灰着比"按下去什么也不发生"
    /// 诚实。
    /// 「添加」**不在这儿**：要加的那一台还没出现在表里，它没有"先选中"这一步，所以
    /// 常亮（连一条都没有的时候尤其要能按）。
    ///
    /// NOTE: 三个地方要喊它 —— 建完那棵树（初值）、`setServerList()`（整张表换掉 ⇒ 选中
    /// 没了）、以及 `itemSelectionChanged`（用户点了一行）。
    void updateServerSelectionActions();
    /// 右键服务器行：选中 @p item，在 @p globalPos 弹出「加入 / 编辑 / 复制地址」（手搭 Qt::Popup，不用 QMenu::exec()）。
    /// 「加入」「编辑」直接点列表下方那两颗按钮，和它们走同一处理。
    void popUpServerMenu(QTreeWidgetItem *item, const QPoint &globalPos);
    void refreshServerFavorites();

    /// 服务器列表里**现在选中的那一行**（没选中 = nullptr）。
    ///
    /// NOTE: 这一层只认"选中了哪一个条目"，**不认"选中的那个有没有地址"** —— 地址为空的
    /// 条目根本进不来（`NbtUtil::readServerList` 跳过没有可用 `ip` 的），所以条目
    /// 选得中就一定改得动、也一定连得上。那两颗按钮的灰/亮再多判一层空地址只是
    /// 把同一条规矩写两遍。
    QTreeWidgetItem *selectedServerItem() const;
    /// 当前选中且没被筛掉的所有行（按列表先后）。
    QList<QTreeWidgetItem *> selectedServerItems() const;

    /// 造一行「跟随全局 / 自定义 + 控件」。返回值是**整格**（档位行 + 提示），控件要加进
    /// 里面的「档位那一行」（`choiceRowOf()` 取），内存那两行整格禁用靠的就是这个返回值。
    QWidget *addOverrideRow(class QGridLayout *grid, int row, const QString &field,
                            const QString &title, const QString &hint);

    /// 取某一行装控件的那条横排（`choiceRowName()`）。
    ///
    /// NOTE: 控件加进这里而不是加进整格，是因为整格的布局是**竖排**（档位行 / 提示）。
    /// 写成 `qobject_cast<QHBoxLayout *>(cell->layout())` 会拿到 nullptr，
    /// 再往下就是空指针解引用 —— 这张表里每一行都写着那一句，所以收口成一个函数。
    QHBoxLayout *choiceRowOf(const QString &field) const;

    /// 某一项选的是「自定义」还是「跟随全局」。**档位的读侧唯一出处**。
    ///
    /// 读的是下拉框的 `currentIndex()`。
    /// 认不出来的 field（或那一格没建出来）**回 false = 跟随全局** —— 这是"没建出来 =
    /// 没有覆盖"那个方向，正是 override() 想要的（少写一份覆盖比多写一份安全）。
    bool isCustomChoice(const QString &field) const;

    /// 摆某一项的档位（`custom` = true 就是「自定义」）。**不发任何信号**。
    ///
    /// NOTE: 灌值时用（`setInstanceValues()` 那一串）—— 那一下说的是"打开这一页时盘上
    /// 是这么选的"，不是一次编辑，所以后面必须自己 `QSignalBlocker` 挡掉
    /// `currentIndexChanged`（挡的是 refreshHints 那一条；写盘那一条在
    /// `watchEditors()` 里）。
    void setChoiceCustom(const QString &field, bool custom);

    /// 往堆叠页里加一页（objectName = `panelName(key)`），返回那一页自己。
    /// **加进去的顺序必须就是 pageCount() 那个顺序**（有一条断言钉着）。
    ///
    /// NOTE: 现在只有 ensurePage() 跑 builder 的时候才调它 —— 堆叠页那一格（`addPageSlot()`）
    /// 在构造里就按 pageCount() 的顺序挖好了，页里的控件是第一次切过去才建的。
    QWidget *addPage(const QString &key);

    /// 在堆叠页里挖一格空位（构造里按 pageCount() 的顺序挖全部 23 格）。
    ///
    /// 为什么要空位：堆叠页的下标由 `stackPageIndexOf()` 那张表算，页得**按顺序占位**；
    /// 而页里的控件是第一次切过去才建的（见 ensurePage()）。空位自己什么都不画。
    QWidget *addPageSlot(const QString &key);

    /// 堆叠页里 @p key 那一格的空位（认不出来的键回 nullptr）。
    QWidget *pageSlot(const QString &key) const;

    /// 这一页要是有 builder 还没跑过，就跑一次（跑完从表里删掉，以后再切回来不重建）。
    /// **懒建的唯一入口就是它**：applyPageKey() 先调它 —— "切到哪一页才建哪一页"只写这一处。
    void ensurePage(const QString &key);

    /// 把新造出来的这一页上的控件接到「改了就写」那几条线上（watchEditors() 的按页版本）。
    ///
    /// NOTE: watchEditors() 是按**类型**扫子控件的，构造末尾那一次只扫得到当时已经建出来的
    /// 那些。懒建的页在造出来之后必须自己补一遍，否则就是「这一项改了不写盘」。
    void watchPage(QWidget *page);

    /// 在 @p page 里加一张「左名右控件」的两列表格（3:7，和设置页栏内那一层同一套），
    /// 返回那张表格。控件加进它自己的 `formName(key)` 子控件里。
    ///
    /// NOTE: 中间夹一层 form（而不是直接把表格挂在页上）是为了以后要往页里加表格之外的
    /// 东西（一句话说明、一颗按钮）时不用把表格拆开 —— 设置页那一层就是这个形状。
    QGridLayout *addForm(const QString &key, QWidget *page);

    /// 左栏那条导航列表的宽度：按最长的一条条目算（`sizeHintForColumn`），不写死像素
    /// —— 换字体 / 换样式就错。再加一点左右余量，条目的字离边太近看着像被切了。
    static int navListWidth(const QListWidget *list);

    /// 左栏选中第 @p row 条之后要做的事：把右栏换到那一条对应的页上。
    /// 越界 = 什么都不做（见 .cpp 里那段 NOTE）。
    void applyEntry(int row);

    /// 把右栏摆到 @p key 那一页上（不管它在不在左栏里）。applyEntry() 与 showPage() 都落到
    /// 这儿，换页之后那两件事（重列目录、发 serverPageEntered）只写一遍。
    void applyPageKey(const QString &key);

    /// 按 `shownEntryKeys()` **整份重灌左栏那一列**：重算宽高、刷新勾选框、把选中还原到
    /// 堆叠页当前那一页（那一页要是掉出去了就回概览，**但「配置文件」不算掉出去**）。
    ///
    /// 可用性一变、用户勾掉/勾上一条，都要喊它 —— 左栏那一列的内容是**算出来的**，
    /// 不是构造时摆好就完事的。
    void rebuildEntryList();

    /// 把勾选框按"可用不可用 + 用户勾没勾"重摆一遍（**压着信号**：这是画，不是用户改的）。
    void refreshPageChecks();

    /// 四个 `setXxxUsable()` 的公共落点：记进 `m_pageAvailable` + 重建左栏那一列。
    /// @param markPage 要不要把那一页**整页换成一句「用不上」**。
    ///
    /// NOTE: 「光影包 / 原理图」要（那两页里的东西在没加载器时真的一点用都没有），
    /// 「模组 / 数据包」不要 —— 低版本上的 `datapacks/`、原版实例上的 `mods/` 里
    /// 照样可能有东西，列出来是句真话（用户真想看，把这一条勾出来就看得见）。
    /// 它们"用不上"这件事由勾选框那一格的 tooltip 说（见 pageUnavailableText()）。
    void setPageUsable(const QString &key, bool usable, bool markPage = true);

    /// 把图标那一行按 m_iconChoice 重画一遍：预览换图、八颗浮层里那颗按钮的勾选状态跟着换。
    /// 唯一出处是 m_iconChoice —— 按钮和预览都是它的画（见上面那段"影子状态"）。
    /// NOTE: 浮层**没弹出来的时候也要调到它**（预览和按钮的勾选状态在关着的时候也要对，
    /// 否则弹出来那一瞬间是上一次的样子）。
    void refreshIconRow();

    /// 在 @p anchor 那颗按钮底下弹出 @p menu 那个浮层。两颗按钮（更改图标 / 分组…）共用。
    ///
    /// NOTE: 浮层是**常驻**的（构造函数里建好，这里只负责弹），所以这一个函数没有"建"
    /// 那一半。理由见 .cpp 里那段 NOTE。
    static void popUpMenuBelow(QWidget *menu, const QWidget *anchor);

    /// 「分组…」那个浮层里第 @p index 行被点了：记住选的是哪一组（下标 0 = 未分组）。
    /// 只改这一层的影子状态 `m_chosenGroupIndex` —— 落盘是 MainWindow 的事。
    void chooseGroup(int index);

    /// 把「分组…」那个浮层里每一行的可用性、以及那颗按钮上的 tooltip，按此刻的
    /// `m_chosenGroupIndex` 重摆一遍（当前那一行灰掉 + 一句「已经在这一组里了」）。
    ///
    /// NOTE: setGroupChoices() 末尾也要调它一次 —— 重建完那几行是**全亮**的，不刷的话
    /// 弹开来会看到"当前那一组也能点"（点下去什么都没发生）。和 refreshIconRow()
    /// 同一条路数：控件的样子只有一个出处，改完影子状态就调一次。
    void refreshGroupMenuState();

    // ---------- 「改了就写」那套（见 signals 里 settingsEdited() 那段）----------

    /// 把这一页上**所有能改值的控件**接到 notifyEdited() 上。构造函数最后调一次。
    ///
    /// 按控件的**类型**接，不按对象一个个接：往这一页加控件时只要它属于这几类就自动
    /// 跟上（漏接一个的表现是"这一项改了不写盘"，界面上看不出来 —— 那正是最该防的
    /// 一种坏）。四类各自的信号在函数里写着。
    ///
    /// NOTE: 「实例名」那一格**故意排除在外**（它走 renameRequested()，见 signals 那段）。
    void watchEditors();

    /// 值变了吗：变了就把这一份记下来、发一发 settingsEdited()。
    ///
    /// **判据是"和上一次发出去的那一份比"，不是"有没有人动过控件"**：
    /// - 点一下已经选中的那一项、把数字改回原样、在一个框里打一个字又删掉 ——
    /// 值没变就不该让磁盘动一次；
    /// - 反过来，`setChecked()` 这类程序化的改动也照样算数（这一页上那些只发生在
    /// 构造期，构造函数最后会把当时的签名记下来当起点，所以打开页面不会立刻写一次）。
    ///
    /// NOTE: 它**不发 settingsEdited() 的"哪一项变了"**：写盘那边读的是整份覆盖
    /// （`override()` 那一套），不关心是哪一格动的 —— 那两个问题分开问，将来
    /// 加一项就不用同时改两处。
    void notifyEdited();

    /// 这一页此刻的**值签名**（把每个会写进 wtfml.meta 的值拼成一串）。
    ///
    /// 用它而不是给那几个值类型写 `operator==`：签名的唯一用处就是"和上一次比一比"，
    /// 而这个比法**不依赖**那几个结构体将来加不加字段（加了字段、拼签名那一行忘记改的话，
    /// 漏掉的那一项会变成"改了不写盘"—— 所以它和 `override()` 摆在一起，改一处看得见另一处）。
    QStringList valueSignature() const;

    /// 上一次**发出去**（= 写盘那边看见过的）那一份签名。见 notifyEdited()。
    QStringList m_lastSignature;

    /// 全局那一层的值，用来写「跟随全局（当前 …）」。构造之后不再变。
    GlobalValues m_global;

    QLabel *m_headerIcon = nullptr;   // 页眉（UI 7b）：图标、名字、元信息
    ElidedLabel *m_headerTitle = nullptr;
    QLabel *m_headerMeta = nullptr;

    // ---------- 左栏那一列 + 右栏堆叠页（见 pinnedPageKeys() 上面那一段）----------
    // / 左栏**唯一**那一列。当前行 = 界面上当前摆着的那一页。
    QListWidget *m_pageList = nullptr;
    QTreeWidget *m_navigationTree = nullptr;
    QHash<QString, QListWidget *> m_lifecycleLists;
    /// 「Replay」/「结构」选中一行之后底下那一格（键 → 标签），以及它现在讲的是哪一条。
    /// @see lifecycleFileInfoLabelName() / setLifecycleInfoText()
    QHash<QString, QLabel *> m_lifecycleInfo;
    QHash<QString, QString> m_lifecycleInfoPath;
    QHash<QString, QTimer *> m_lifecycleInfoTimers;
    QHash<QString, std::function<void()>> m_filePageBuilders;
    /// 页键 → 造这一页的函数（第一次切过去才跑，跑完从表里删掉 —— 见 ensurePage()）。
    QHash<QString, std::function<void()>> m_pageBuilders;
    /// 已经建出来的那几页（键）。同一页只建一次。
    QSet<QString> m_builtPages;
    QHash<QString, QPixmap> m_pendingFileIcons;
    QHash<QString, QString> m_pendingFileResources;
    QStringList m_pendingOutdatedPaths;
    bool m_hasPendingUpdates = false;
    /// 首次露面时播一次换页动画（同一个对话框只播一次）。
    bool m_firstShowAnimated = false;
    bool m_closing = false;
    bool m_pendingUpdatesChecked = false;
    void ensureFilePage(const QString &key);
    /// 「文件管理」那几页还有没建出来的吗。**「要不要把这一份留给以后补灌」的判据**：
    /// 有页没建 ⇒ 存着（那一页建出来时会自己补）；全建完了 ⇒ 存的那几份可以扔了。
    ///
    /// NOTE: 别再用 `m_filePageBuilders.isEmpty()` 当这个判据 —— 页是懒建的，那个表在
    /// 打开对话框那一刻是**空的**（一页都没建），意思正好反过来。
    bool filePagesPending() const;
    /// 截图页的缩略图在**后台线程**解（`QtConcurrent`），结果按 `路径|大小|修改时间` 缓存：
    /// 一页几十张 PNG 在界面线程上逐张解（原来是排到事件循环之后），点开截图的
    /// 那一下和随后几帧都要等它。
    ///
    /// 缓存跟着这个对话框走（关掉就没了）：它只在"这一份实例的截图"这一小撮上复用，
    /// 换一页再回来、点「刷新」再列一遍都不必重解。行高在**没有图的时候**就用
    /// `setSizeHint()` 钉死，图回来只换图标，行不跳。
    QHash<QString, QImage> m_lifecycleThumbCache;
    /// 重列一次就 +1：在途的解码回调按它丢掉（不往新一页里塞旧图）。
    quint64 m_lifecycleThumbGeneration = 0;
    void requestLifecycleThumb(const QString &path);
    QPlainTextEdit *m_diagnosisReport = nullptr;
    /// 「迁移」/「升级」那两页（键 → 控件；键就是 `migration` / `upgrade`）。
    QHash<QString, QComboBox *> m_transferTargets;
    QHash<QString, QPlainTextEdit *> m_transferPlans;
    QHash<QString, QListWidget *> m_transferItems;
    void updateTransferApplyState(const QString &key);
    /// 构造函数里那三摊（原来是同一个函数里换着用一份 `grid`，现在各自 `addForm()` 一份）：
    /// 概览那一栏、启动那一栏、文件管理那八页。拆的依据是"一栏一摊"，不是按行数切。
    /// 概览那一栏只认构造时递进来的目录名（名称框的初值就是它）。
    void setupOverviewPage(const QString &instanceId);
    /// 「启动」那一栏要用到四样只有调用方才知道的：全局值、追加 JVM 参数、提醒开关、
    /// 可选的窗口档位。它们本来就只是"这一次打开对话框的现场"，不存成员。
    void setupLaunchPage(const GlobalValues &global, const QStringList &appendJvmArgs,
                         bool warnJavaMismatch, const QList<WindowModeChoice> &windowModes);
    /// 把构造函数那份「现状」灌进启动页（原构造函数里那一串 setChoiceCustom / setValue）。
    /// **跟着页走**：这一页第一次切过去才建，值就那时候灌。
    void applyLaunchValues(const Override &current, const GlobalValues &global);
    /// 造「文件管理」那八页里的**一页**（第一次切过去才跑，见 ensurePage()）。
    void buildFilePage(const QString &key);
    /// 服务器列表那一页（它不走 InstanceFilesPage，单造）。
    void buildServerPage();
    /// 造「创作工具 / 维护」那几页里的**一页**（同上）。
    void buildLifecyclePage(const QString &key);
    void addLifecycleBatchActions(const QString &key, QListWidget *list, QVBoxLayout *layout,
                                  QLineEdit *search = nullptr);
    /// 「迁移」/「升级」那两页里，把存着的那一份目标 / 计划 / 范围表摆上去。
    void replayTransferPage(const QString &key);
    /// 服务器列表那一页把存着的那一份摆上去（页可能是先收到列表、后建出来的）。
    void replayServerPage();
    void rebuildNavigationTree();
    void refreshLifecycleFiles(const QString &key);
    void filterLifecycleFiles(const QString &key);
    void updateLifecycleInfo(const QString &key);
    /// 右栏那份堆叠页，下标见 pageCount() / filePageFirstIndex()。
    QStackedWidget *m_stack = nullptr;

    /// 哪几页在这个实例上**用得上**（键 → 可用；表里没有 = 可用）。
    ///
    /// 判据全在 MainWindow（读加载器名 / 扫 mods/ / 看游戏版本），这一层只收结论：
    /// `setModUsable()` / `setShaderUsable()` / `setSchematicUsable()` / `setDatapackUsable()`
    /// 四个 setter 各自改表里一格，再喊一次 rebuildEntryList()。
    QHash<QString, bool> m_pageAvailable;
    /// 用户**勾掉的**那几页（键 → false，见 pageChoices()）。只装显式选择，不写"默认值"。
    QJsonObject m_pageOverrides;
    /// 「显示哪些页」那七条（键 → 勾选框，顺序同 optionalPageKeys()）。
    QHash<QString, QCheckBox *> m_pageChecks;

    /// 「实例名」那一格（见 instanceName()）。现在长在「概览」栏里。
    QLineEdit *m_instanceNameEdit = nullptr;

    /// 这个实例现在的目录名（构造时那个 instanceId）。留着是为了"导出启动脚本"那条路
    /// 的默认文件名 —— 它一定是合法文件名（validateDirName 那一关挡过），而显示名不一定。
    QString m_instanceId;
    /// 构造时那份「现状」（盘上的覆盖）。**启动页还没建出来时 override() 读它** ——
    /// 没建 = 这一段里用户没碰过它，读回来的就该是盘上那一份。
    Override m_currentOverride;
    /// 同上：追加参数与版本提醒那两位（它们不在 Override 里，见各自的 getter）。
    QStringList m_pendingAppendJvmArgs;
    bool m_pendingWarnJavaMismatch = true;
    bool m_launchPageBuilt = false;

    /// 「实例名」那一格接改名那条线没有（`watchPage()` 每建一页都会扫一遍整个对话框，
    /// 而那一发是 lambda —— `Qt::UniqueConnection` 管不了它，得自己记一笔）。
    bool m_renameWired = false;

    // ---- 「灌值」先到、页后建的那几页 ----
    //
    // 调用方（MainWindow / controller）开页那一刻就把服务器列表、迁移计划这些灌进来，
    // 而页是第一次切过去才建的。**这些值不会再来第二遍**，所以先存在这儿，页建出来再摆。
    QList<QPair<QString, QString>> m_serverRows;
    QString m_serverProblem;
    bool m_serverListReady = false;
    QString m_serverJoinText;
    QString m_serverJoinTipPending;
    bool m_serverPinging = false;
    QString m_diagnosisText;
    QHash<QString, QList<QPair<QString, QString>>> m_transferTargetRows;
    QHash<QString, QString> m_transferTargetId;
    QHash<QString, QString> m_transferPlanText;
    QHash<QString, QList<QPair<QString, QString>>> m_transferItemRows;
    QHash<QString, QStringList> m_transferItemDefaults;
    QString m_upgradeContextText;
    QList<QStringList> m_upgradeModRows;
    QString m_upgradeModSummary;

    // ---- 文件管理那八页（见上面 filePageKeys() / setFileDirs() 那两段）----
    // / 这个实例的目录（setInstanceDir()）：概览页上那排快捷按钮的「版本目录」开的就是它。
    QString m_instanceDir;
    /// 「文件管理」那几页各自的目录（见 setFileDirs()）：**页键 → 绝对路径**。
    QHash<QString, QString> m_fileDirs;
    /// 六页目录列表控件（键见 filePageKeys()）。refreshFilePages() 走它。
    QHash<QString, InstanceFilesPage *> m_filesPages;
    /// 概览页上那一排常用目录按钮（**按表序**，见 shortcutTexts()）。
    QList<QWidget *> m_shortcutButtons;
    /// 那一排所在的容器与网格（relayoutShortcuts() 要拿它们重摆）。
    QWidget *m_shortcutCell = nullptr;
    QGridLayout *m_shortcutGrid = nullptr;
    /// 「更多目录…」那一颗（**没有收着的东西时是 nullptr** —— 见 .hpp 里那条 NOTE）。
    QPushButton *m_shortcutMoreButton = nullptr;
    /// 用户手点开的那一截现在开没开。
    bool m_shortcutMoreExpanded = false;
    /// 这一刻**在盘上**的那几路目录（setFolderPresentKeys()）。
    ///
    /// 默认 = "全都在"（和那四个 `set*Usable()` 同一个口径）：直接 new 出来的对话框
    /// 这五条全摆在外面、也没有「更多目录」。
    QStringList m_shortcutPresentKeys = shortcutPathKeys();

    // ---- 「配置文件」那一页独有的两样----
    // / `config/` 的**根**。进去、出来都拿它当墙 —— 不许走到 `config/` 上面去（再往上是
    // / 这个实例自己、然后是整个 `.minecraft`，走到那儿"这是配置文件"这件事就走丢了）。
    // /
    // / NOTE: **不能在构造函数里从 m_fileDirs 抓** —— 那会儿 setFileDirs() 还没被调过，
    // / 抓到的永远是空串，于是"上级"那道墙形同虚设。和快捷方式那几颗按钮踩的是
    // / 同一个坑（见那边注释里那句"捕获快照的话拿到的永远是空串"）。所以它在
    // / setFileDirs() 里赋值。
    QString m_configRoot;
    /// 那一页上面那颗「上级」。**到了根就整颗不画**（不是画成灰的 —— 和"组里挪不动的
    /// 那一侧不画"同一条规矩）。
    QPushButton *m_configUp = nullptr;
    /// 那一页的两格子（0 = 列表 1 = 编辑器，见 configFilePage() 那段）。
    ///
    /// NOTE: 「上一级」和那张列表都在第 0 格里，所以 `m_configUp` 的可见性只管第 0 格 ——
    /// 切到编辑器那一格时它整格都不在画面上，不必额外藏一遍。
    QStackedWidget *m_configStack = nullptr;
    /// 第 1 格那个编辑器（**建一次留着**，见 configFilePage() 的 NOTE）。
    ConfigFileEditorPage *m_configEditor = nullptr;
    /// 「存档」那一页的两格子（见 openWorldSettings() 上面那一段）。
    QStackedWidget *m_worldStack = nullptr;
    /// 第 1 格那个世界设置（**建一次留着**，同上）。
    WorldSettingsPage *m_worldSettings = nullptr;
    QTreeWidget *m_serverTree = nullptr;
    QPushButton *m_serverFavorite = nullptr;
    QCheckBox *m_serverFavoritesOnly = nullptr;
    QSet<QString> m_serverFavorites;
    bool m_serverListReadable = true;
    QLabel *m_serverNotice = nullptr;
    /// 「刷新」（见 refreshServerListRequested()）。**正在重读 + 重测时**文字换成
    /// 「刷新中…」并按下不动（见 setServerPinging()）。
    QPushButton *m_serverRefresh = nullptr;
    /// 「编辑」（见 editServerRequested()）。
    QPushButton *m_serverEdit = nullptr;
    /// 「添加」（见 addServerRequested()）。**唯一一颗不跟着选中走的**（见 updateServerSelectionActions()）。
    QPushButton *m_serverAdd = nullptr;
    /// 「删除」（见 deleteServerRequested()）。
    QPushButton *m_serverDelete = nullptr;
    /// 「加入服务器」（见 joinServerRequested()）。**默认收着**：没有那个参数的版本上它
    /// 不该出现（见 setServerJoinText()）。
    QPushButton *m_serverJoin = nullptr;
    /// 「加入服务器」的悬停提示（setServerJoinText() 递进来的那一句）。**存在这儿**是因为
    /// 那颗按钮选中/没选中时提示要换（见 updateServerSelectionActions()）—— 换了之后还得
    /// 换得回**原来那一句**，而那句话是调用方给的，这一层编不出来。
    QString m_serverJoinTip;
    /// 每台服务器现在什么样（下标同 setServerList() 那一份）。**状态在这儿存着**：
    /// 「刷新」重读一遍 servers.dat 时那些条目是重建的，而这一份是"上一次问到的结果"，
    /// 由 setServerStatuses() 单独灌。行数对不上时以少的为准（见 setServerStatuses()）。
    QList<ServerRowStatus> m_serverStatuses;

    // ---- 概览栏那三行（见上面 setIcon 那一段）----
    // / 图标那一行**唯一的出处**：空串 / `builtin:<键>` / `local`。
    // /
    // / NOTE: 这是全页唯一一处影子状态，「本地图片」那一档没有控件能表示它（理由见上面
    // / setIcon() 上面那一段）。八颗按钮和那张预览都由 refreshIconRow()
    // / 照着它画。
    QString m_iconChoice;
    /// 用户刚挑的那张图在盘上的原处（没挑过 = 空）。拷进实例目录是调用方的事。
    QString m_pickedIconPath;
    /// 「默认」和「本地那张」两档画什么。setIcon() 灌进来，之后不变。
    QPixmap m_defaultIconMark;
    QPixmap m_localIconMark;
    /// 那一张预览。边长固定（见 .cpp 里 kIconPreviewSize）—— 换图时整行不该跟着跳。
    QLabel *m_iconPreview = nullptr;
    QLabel *m_lastLaunchLabel = nullptr;
    QLabel *m_playTimeLabel = nullptr;
    /// 八颗候选按钮，下标与 VersionIcons::iconCandidateKeys() 一一对应。
    QList<QToolButton *> m_iconButtons;
    /// 「更改图标」那颗按钮，以及它底下那个**常驻**浮层（见 .cpp 里那段 NOTE）。
    QPushButton *m_changeIconButton = nullptr;
    QWidget *m_iconMenu = nullptr;
    /// 「分组…」那颗按钮 + 它底下那个常驻浮层。
    QPushButton *m_groupButton = nullptr;
    QWidget *m_groupMenu = nullptr;
    /// 「分组…」浮层里那几行按钮（下标 0 = 未分组，其余按 setGroupChoices() 的顺序）。
    QList<QPushButton *> m_groupRows;
    /// **选的是第几行**（下标，不是组 id）。这一格和图标那一格同一类：浮层里那几行是
    /// 现场按调用方递进来的清单建的，行数会变（用户能在另一个框里加组/删组），所以值的
    /// 出处只能是这一格。`-1` = 还没灌过（没弹过任何一次）—— 那时 chosenGroupId() 回空串。
    ///
    /// NOTE: 存的是**下标**而不是 id：id 那一份在 setGroupChoices() 进的 `m_groupIds` 里，
    /// 两个成员一起改，中间不会出现"下标指着另一行"的窗口。
    int m_chosenGroupIndex = -1;
    /// 「分组…」浮层里每一行的组 id（下标 0 是空串 = 未分组）。和 m_groupRows 一一对应。
    QStringList m_groupIds;
    QCheckBox *m_pinCheck = nullptr;
    /// 置顶组是哪一组（setPinned() 灌进来）。空串 = 这一份 .minecraft 里没有置顶组。
    ///
    /// NOTE: 它**只管那一句解释怎么写**（"会在「置顶」里也摆一条"），**不影响任何控件**
    /// —— 见 .hpp 里 setPinned() 那段。
    QString m_pinnedGroupId;
    QLineEdit *m_javaPathEdit = nullptr;
    QSpinBox *m_memoryMaxSpin = nullptr;
    MemorySlider *m_memoryMaxSlider = nullptr;
    QSpinBox *m_memoryMinSpin = nullptr;
    QLineEdit *m_extraJvmArgsEdit = nullptr;
    /// 「窗口模式」那一行。**项是调用方灌的**（构造参数 windowModes），
    /// 这里只有一个框；档位那一列以 itemData 存**代号字符串**，override() 直接把它读出来。
    QComboBox *m_windowModeCombo = nullptr;
    QSpinBox *m_windowWidthSpin = nullptr;
    QSpinBox *m_windowHeightSpin = nullptr;
    /// NOTE: 分辨率那一行虽然占**两个**键（windowWidth / windowHeight），但只有**一个**
    /// 档位下拉框 —— 它俩是同一个决定的两半（见 .hpp 里 override() 那段）。所以那一个
    /// 下拉框是按 `windowWidthField()` 那个 field 存进 m_choiceCombos 的。
    /// 「跟随全局」时那一句解释要说的窗口大小（`GlobalValues::windowWidth/Height`）。
    /// 存下来是给 refreshHints() 用的 —— 它只拿到一次构造参数，之后每次都要重写那句话。
    ///
    /// NOTE: 那个 `-1` 的默认值是"调用方没给"的哨兵：<= 0 时那句话里**不写**尺寸，
    /// 只写"跟随全局"（见 .hpp 里 GlobalValues 那一段）。
    int m_globalWindowWidth = -1;
    int m_globalWindowHeight = -1;
    /// 追加参数那一行是**多行**输入（一条一行）而不是单行：meta 里的 jvmArgs 是数组，
    /// 数组存在的理由就是"参数本身可能含空格"（见 VersionManager::versionJvmArgs()）。
    /// 用单行输入就得在"读出来拼成一行"和"存回去切成数组"之间来回切，而那个来回是
    /// **有损**的：`-Dfoo=bar baz` 会被切成两条。一条一行则原样进原样出。
    QPlainTextEdit *m_appendJvmArgsEdit = nullptr;
    QCheckBox *m_warnJavaMismatchCheck = nullptr;
    QPushButton *m_advancedToggle = nullptr;
    QWidget *m_advancedBox = nullptr;
    /// **展开**「高级选项」之前这个窗有多高。折回时按它还原 —— Qt 只在"窗口比内容还小"
    /// 时把窗口撑大，内容变少时一动不动，不记这一笔的话展开一次再收起，底下就永远留着
    /// 那一截空白（见 .cpp 里那段 NOTE）。0 = 还没展开过。
    int m_heightBeforeAdvanced = 0;

    /// 每一项的档位下拉框，键是那个 field（`javaPathField()` 那一组）。
    ///
    /// 七行长得一模一样，写成表就不必再维护那条 if 链。存在成员里（而不是每次
    /// findChild）是因为 refreshHints() 每次信号都要读它们算"有效值"，而那是按键
    /// 最频繁的一条路。
    QHash<QString, QComboBox *> m_choiceCombos;
    /// 「最大内存」「最小内存」两行那一格的容器 —— 自动档下整行禁用，
    /// 禁用的是这两个容器（连同里面的档位下拉框），不是只禁数字框。
    QWidget *m_xmxFollowCell = nullptr;
    QWidget *m_xmsFollowCell = nullptr;
    /// 每一项在「自定义」状态下那句话（选「跟随全局」时说的是"当前全局是哪个值"，
    /// 两种形态都由 refreshHints() 写进同一个 QLabel）。
    QHash<QString, QString> m_customHints;
};






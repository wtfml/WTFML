/**
 * @file InstanceFilesPage.hpp
 * @brief 「实例设置 → 文件管理」那几页共用的**一个目录的列表页**（模组 / 资源包 /
 * 数据包 / 光影包 / 原理图 / 存档）。
 * @note 「文件管理」那八页里有六页是同一个形状：**列一个目录，一行一个条目，
 * 上面一排（路径 + 下载新的 / 刷新 / 打开文件夹），行尾两颗按钮（启用·禁用 / 删除）**。
 * 差别只有五条：
 * - 列出什么（存档只列文件夹，别的文件文件夹都列）；
 * - 能不能启用·禁用（只有模组 / 资源包 / 光影包 —— 游戏会自己扫这三个目录）；
 * - 能不能删（存档不行，见下面 setCanDelete() 那段）；
 * - 顶上有没有那颗「下载新 X」（只有下载页里真有对应一栏的那四页有，
 * 见 setDownloadText()）；
 * - 行尾那颗「详情」点不点得动（认得出是哪个资源的那几条才点得动，
 * 见 setResourceForPath()）。
 * 这五条都是 setter，所以六页共用这一个控件，不写六份。
 *
 * 【搜索 + 状态选择】
 *
 * 正文从上到下是：**搜索框 → 那一排按钮 → 状态选择 → 列表**（搜索在上、选择在下）。
 *
 * 两样都是**筛这一页的列表**，不改盘上任何东西：
 * - 搜索匹配的是"这一条在界面上叫什么"（以及盘上那个文件名），子串、不分大小写；
 * - 状态那一栏 = 全部 / 启用 / 禁用（+ 可更新，见 setCanFilterUpdates）。
 *
 * 【「批量管理」——只在那四页上开】
 *
 * 四件事，全由 `setBatchMode(true)` 这一只开关说了算：
 * - 行尾**详情按钮在开关前面**（见下面【行尾三格的次序】①）；
 * - **多选**（`ExtendedSelection`：单击单选、Ctrl+单击切换、Shift+单击选一段、
 *   Ctrl+A 全选）+ 列表下缘**悬浮**一条操作条（不是表尾那一行）；
 *
 * 开关的判据不在这儿：哪几页该开这套东西由 `InstanceSettingsDialog` 说
 * （`filesNameOpensResource(key)`），这一层不自己猜"哪几页算模组页"。那四页之外
 * （原理图 / 存档 / 配置文件）保持原样：单选、没有详情按钮、顶上还是那个下拉框。
 * 所以下拉框那一套控件**留着**，按 `m_batchMode` 和四颗按钮互相显隐（见 applyState）。
 *
 * 【行尾三格的次序、悬停、右键，以及选中那笔账】
 *
 * 四件事：
 * ① 行尾是 **`[详情][开关][删除]`**（layoutRow 里的次序）；
 * ② 「详情」那一格**只在鼠标悬停这一行时画**（悬停由 `eventFilter` 里视口的
 * MouseMove / Leave 记，见 m_hoverPath；**占位留着**，所以名字一列不会跟着伸缩）；
 * ③ **右键点一行 = 开这一条的详情页**（`eventFilter` 里 `MouseButtonPress` 那一支），
 * 而且那一发**被吃掉** ⇒ 视图连选中态都不会动（于是保留上一页的选择）；
 * ④ 底下那条操作条上「启用」「禁用」两颗**对称**：摆着、不适用就灰着，
 * 见 SelectionAction 那一段的 NOTE。
 *
 * 另外一条是选中账：`m_selectedPaths` 按 `identityPath()` 对（见 m_selectedPaths
 * 那段）—— 盘上还是同一个文件，用户手里选的就是"这一条"；按盘上那个名字对的话，
 * 点一下行尾的开关（禁用 / 启用 = 改名）那一行就掉出选择。
 *
 * 这些入口只有 `detailRequested(路径)` / `batchToggleRequested(…)`，
 * 这一层一个字节都不碰盘。
 *
 * 【顶上那几排「来源」下拉框 —— 只有「结构/原理图」那一页有】
 *
 * 两排下拉框**横着并排：上面一排是存档的，下面一排是投影的**。
 * 原因：结构方块存下来的那些 `.nbt` **跟着存档走**
 * （`<实例>/saves/<存档>/generated/minecraft/structures/…`），而投影（Litematica）
 * 那些 `.litematic` 是**实例级**的（`<实例>/schematics/`）—— 一句话说不清"这一页在看
 * 哪一个目录"，得同时列两个来源。见 setStructureSources() / scanDirectories()。
 *
 * 别的七页不受影响：它们不调 setStructureSources()，`scanDirectories()` 给的
 * 就是 `setDirectory()` 那一个目录，走的就是同一条路。
 *
 * 【这一层只**显示**，不碰盘】
 *
 * 和 InstanceSettingsDialog 同一条约定：它列目录（读），但**删除 / 改名一个字节都不动**
 * —— 那两件事分别发 toggleRequested() / deleteRequested()，由 MainWindow 去问一句、
 * 去写盘、再回头喊 refresh()。理由和「导出启动脚本」那条一样：弹话要走 OverlayHost
 * （InlineMessage 的每一个入口都要它），而这一层手上没有；而且 ui/ 不 include src/，
 * 它连 AppPaths 都够不着。批量那一套（更新 / 禁用 / 启用 / 删除）也一样：这一层只发
 * batchUpdateRequested() / batchToggleRequested() / batchDeleteRequested()，
 * 真正下手的是 MainWindow（见上面【批量管理】那一段）。
 *
 * 【每行是代理画的，不是一行一套控件】
 *
 * 一行一套 `QWidget` + 两个标签 + 开关 + 两三颗按钮时，60 个模组的页每次进都要 70–300 ms
 * （建控件 + 每个开关 `setChecked` 触发的无障碍事件）。现在是 `QListWidget` 的条目 +
 * `RowDelegate` 自绘：条目只带数据（见「给验证工程用的访问器」那段的角色），几何只有
 * `layoutRow()` 一处，画和点击命中都认它；行尾按钮交给样式按 QToolButton 画（借一颗藏着的
 * 探针按钮匹配样式表），开关和 `ToggleSwitch` 共用 `ToggleSwitch::paintSwitch()`。
 */
#pragma once

#include <QDateTime>
#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QElapsedTimer>
#include <QWidget>

class QAbstractButton;
class ElidedLabel;
class QComboBox;
class QLabel;
class QScrollArea;
class QLineEdit;
class QListWidget;
class QRubberBand;
class QListWidgetItem;
class QPushButton;
class QToolButton;
class QStyledItemDelegate;
class QTimer;
class QVBoxLayout;

class InstanceFilesPage : public QWidget {
    Q_OBJECT

public:
    /// 这一页列什么。
    enum class Mode {
        FilesAndDirs,  ///< 文件 + 文件夹（一个资源包 / 数据包 / 光影包都可以是一个目录）
        DirsOnly,      ///< 只列文件夹（一个存档就是一个目录，saves 底下那些文件不是存档）
        /// NOTE: 只给「模组」那一页 —— 另外三页（资源包 / 数据包 / 光影包）上一个**目录
        /// 本身就是一份资源**（游戏自己就认没解压的那种），藏掉是另一回事，
        /// 不能顺手改。
        FilesOnly,
    };

    explicit InstanceFilesPage(QWidget *parent = nullptr);

    /// 列什么（见 Mode）。默认 FilesAndDirs。
    void setMode(Mode mode);

    /// 行首那一格图标露不露。**默认露**。
    ///
    /// 关掉的是"这一页上根本不会有图"的那几页：原理图（`.litematic` 不在任何平台的
    /// 清单里）、配置文件（纯文本）。那几页上每一行都摆一张占位拼图，白占一格宽度 ——
    /// 关掉之后名字那一格直接吃掉这块地方。
    ///
    /// NOTE: 和 `setCanEdit()` 一样，建行时按这一位决定**建不建那一格**（不是建完
    /// 再藏 —— 藏起来的控件仍然占着布局里那一格）。
    void setRowIconsVisible(bool visible);

    /// 行尾要不要那颗「启用 / 禁用」。**只有游戏会自己扫的那三个目录给 true**
    /// （模组 / 资源包 / 光影包）—— 数据包里那些要自己搬进存档、原理图要 Litematica
    /// 主动去读、存档根本不是一个文件，改了名都只会让用户以为生效了。
    void setCanToggle(bool canToggle);

    /// 行尾要不要那颗「删除」。**存档那一页给 false**：存档是用户手里唯一那份，
    /// 而这一页只列条目 —— 删一个玩了几百小时的世界不该是列表行尾一颗小图标的事
    /// （世界走「世界设置」里那一颗，那里会先把名字摆出来再问）。别的几页给 true ——
    /// 删掉的东西会进回收站，那些东西也都能从下载页再下一份。
    void setCanDelete(bool canDelete);

    /// 行尾要不要那支笔（一次「编辑」）。图标是一支笔，含意就是编辑。
    /// **只有存档那一页给 true**。
    ///
    /// 给的这一页同时还多做一件事：**目录行的「大小 / 时间」是真正的统计值**（见 .cpp 里
    /// fillDirectoryStats()）。这两件事是一套的 —— 存档页要的就是"一个世界有多大、
    /// 上次什么时候玩的"，而别的几页那些目录（`mods/mcef-libraries/` 之类）算一个字节
    /// 数出来没有任何意义，还不如那个破折号。
    void setCanEdit(bool canEdit);

    /// 这套「批量管理」（详情按钮 + 多选 + 悬浮操作条 + 四颗横向筛选按钮）开不开。
    /// **默认关**；只有「模组 / 资源包 / 数据包 / 光影」那四页开
    /// （判据在 `InstanceSettingsDialog`，见文件头【批量管理】那段）。
    ///
    /// NOTE: 关了的那几页不是"这些动作不能用"，而是**这些界面元素不出现** —— 详情按钮不建、
    /// 列表是单选（Ctrl+A 自然也就不通）、顶上仍旧是那个下拉框。两套控件都在，
    /// 按这一位显隐（见 applyState）。
    void setBatchMode(bool batchMode);
    bool batchMode() const { return m_batchMode; }
    void setActionPanelEnabled(bool enabled);
    /// 存档专用布局：操作集中一排，搜索/路径一排，不提供启用状态筛选。
    void setWorldLayout(QPushButton *backupButton);
    /// 把一个控件（批量工具条）挪到列表正上方、搜索框之下。
    void placeAboveList(QWidget *widget);
    void setCompactHeader(QPushButton *leadingAction = nullptr);

    /// 顶上那颗「导入世界」（只有存档那一页给）。
    ///
    /// @param text 按钮上写的字。**空串 = 这一页没有这颗按钮**（和 setDownloadText 一样）。
    /// @param tip 悬停提示。
    ///
    /// NOTE: 和「下载新 X」一样，这一层**只发信号**：挑哪个目录、拷到哪儿、要不要问一句
    /// 重名 —— 全在 MainWindow（这一层够不着 AppPaths，也开不了系统那个选择框）。
    void setImportText(const QString &text, const QString &tip);

    /// 顶上那颗「加入选中的世界」（只有存档那一页给，见 `InstanceSettingsDialog`）。
    /// 这是一个临时占位：先看这条参数发出去游戏认不认。
    ///
    /// @param text 按钮上写的字。**空串 = 这一页没有这颗按钮**（判据和 `setImportText()`
    /// 一样是"文字空不空"）—— 那个版本的 json 里没有 `--quickPlaySingleplayer`
    /// 时调用点传空串，这一颗就整颗不露。发了游戏不认识的选项不是"被忽略"，
    /// 是游戏当场退出（见 `launcher/QuickPlay.hpp`）。
    /// @param tip 悬停提示（说清"点了会发生什么"：会**离开这一页**去把游戏开起来）。
    ///
    /// NOTE: 这一层**不知道** quick play 是什么，也不读版本 json（那一层不 include src/）：
    /// 它只发一句"用户点了这一颗"，进哪个世界、能不能进全在 MainWindow 那一层。
    void setJoinText(const QString &text, const QString &tip);

    /// 「加入选中的世界」那颗的**字**与**悬停提示**（**临时占位**）。
    ///
    /// NOTE: 唯一出处。写的是「世界」而不是「这一页选中的那一条」—— 这一颗只有存档
    /// 那一页接得起（别的几页上那一条是模组 / 资源包，`--quickPlaySingleplayer`
    /// 对它们没有意义）。真要给别的页也用，这两句得先改名。
    static QString joinText() { return QCoreApplication::translate("InstanceFilesPage", "加入选中的世界"); }
    static QString joinTip() {
        return QCoreApplication::translate("InstanceFilesPage", "启动游戏并进入所选世界，需要游戏支持快速加入。");
    }

    /// 「加入选中的世界」那颗**没选中一条世界**时的悬停提示。**唯一出处**（刷新 / 验证
    /// 工程都按它核对）。
    static QString joinSelectFirstHint() {
        return QCoreApplication::translate("InstanceFilesPage", "请先选择存档。");
    }

    /// 顶上那颗「批量编辑」。**是占位** ——
    /// 按钮摆在那儿、点下去 MainWindow 会说一句"还没做"。
    ///
    /// NOTE: 判据和 setImportText() 一样是"文字空不空"（不是另开一个 bool）—— 这一页是
    /// 同一段循环搭出来的六页，多一位状态就多一处"哪几页有"的知识。
    void setBatchText(const QString &text, const QString &tip);

    /// 目录空着的时候那一句话（每一页不一样：模组说"去下载页装一个"，
    /// 存档说"进游戏存一个"）。默认给一句通用的。
    void setEmptyText(const QString &text);

    /// 顶上那颗「下载新 X」—— 只有下载页里有对应一栏的那四页给（模组 / 资源包 / 数据包 /
    /// 光影，见 InstanceSettingsDialog::filesDownloadText()）。按下去会把用户送到下载页
    /// 对应的那一栏。
    ///
    /// @param text 按钮上写的字。**空串 = 这一页没有这颗按钮**（原理图在下载页里没有对应的
    /// 那一栏、存档不是"下载"来的、配置文件根本不在下载页里）。
    /// @param tip 悬停提示（说清按下去会去哪儿 —— 它要**离开这一页**）。
    ///
    /// NOTE: 这一层**不知道去哪儿**：它只发 downloadRequested()，切选项卡和挑那一栏是
    /// MainWindow 的事（这一层既够不着选项卡，也不该知道"下载页左栏有几栏"）。
    /// NOTE: 它和「刷新」「打开文件夹」同处顶上那一排 ⇒ 这一页「用不上」时（setUnavailable()）
    /// 跟着一起收起来。这是有意的：光影包那一页正在说"这个实例装不了光影"的时候，
    /// 把人送去下载页找光影包是**指错路**（该先装 Iris，那是模组那一栏的事）。
    void setDownloadText(const QString &text, const QString &tip);

    // ---------- 搜索 + 状态筛选（见文件头）----------

    /// 这一页每一页都有的四个状态档（那一栏里的一格）。**「可更新」要 setCanFilterUpdates(true)
    /// 才出现**（见那一处的理由）。
    enum class StateFilter {
        All,       ///< 全部
        Enabled,   ///< 启用（盘上没有 `.disabled` 后缀的那些）
        Disabled,  ///< 禁用
        Update,    ///< 可更新（这一趟更新检测查出来有新版本的那些）
    };

    /// 悬浮操作条上那七颗按钮，**顺序就是它们在条上的顺序**。前三颗是"对选中的这些
    /// 下手"，后四颗是"改选中的集合"。
    ///
    /// NOTE: 「全选 / 反选 / 取消」只作用于**当前筛出来的那些行** —— 屏幕上看得见的那几条
    /// 才能被选中（见 .cpp 里 rebuildRows 那段把选择收窄到显示集的那一句）。
    /// NOTE: 数据包那一页没有开关（`InstanceSettingsDialog::fileKeyCanToggle` 对它给 false）
    /// ⇒ 那一页的「禁用 / 启用」两颗**藏着**（不建/不摆会让那一条的宽度跟着变，
    /// 而这一条是居中的，变宽度就是整条跳一下）。
    /// 「禁用 / 启用」这两颗**对称**：
    /// **摆着，不适用就灰着**，和这条上其余每一颗逐字同一条规矩。整颗不见会让用户
    /// 以为这一页根本没有启用这个功能。
    /// 灰化的判据取的是**选中集**（`anyEnabled` / `anyDisabled`，和这两颗的灰化同一个
    /// 口径），不是整页的条数（`stateFilterCount()` 是顶上那四颗筛选按钮的口径）。
    /// 唯一"不摆"的情形是**整页没有开关**（数据包）⇒ 两颗一起藏（见上面那条 NOTE）。
    enum class SelectionAction {
        Update,      ///< 更新（把选中的那些升到最新版）
        Disable,     ///< 禁用（把选中的那些改名挂上 `.disabled`）
        Enable,      ///< 启用（把选中的那些改回来）
        SelectAll,   ///< 全选（当前筛出来的那些）
        InvertAll,   ///< 反选
        ClearAll,    ///< 取消（清空选择）
        Delete,      ///< 删除（危险那一条，落成红按钮）
        Count,
    };
    static int selectionActionCount() { return static_cast<int>(SelectionAction::Count); }

    /// 搜索框里写什么（空串 = 不筛）。**给验证工程和"把条件清回来"用** —— 用户敲字走的是
    /// 那条 QLineEdit 自己的路（见 .cpp）。
    void setSearchText(const QString &text);
    QString searchText() const { return m_searchText; }

    /// 那一栏里选哪一档。**唯一入口**（下拉框的 currentIndexChanged 也接到这儿）。
    void setStateFilter(StateFilter filter);
    StateFilter stateFilter() const { return m_stateFilter; }

    /// 状态那一栏里**要不要有「可更新」**。只有"能查更新"的那几页给 true（下载页里真有
    /// 对应一栏的那四页，见 setDownloadText）—— 原理图 / 存档 / 配置文件那几页上的文件
    /// 根本不在任何资源平台的清单里，多一格永远选不出东西的筛选项等于摆一个坏键。
    ///
    /// NOTE: 换这个开关时，如果正选着「可更新」而它要被拿掉，会**退回「全部」**（不是
    /// 留着一个已经不存在的条件 —— 那样屏幕上是一条都不剩的列表）。
    void setCanFilterUpdates(bool canFilterUpdates);

    /// 这一页能不能筛更新（`setCanFilterUpdates()` 灌进来的那一位）。
    ///
    /// 加这个读数，是因为"这一颗现在露着没有"**不等于**这一页有没有这一档：
    /// 一条都没有的档也会被收起来（见 stateFilterVisible），于是"哪几页有
    /// 「可更新」这一档"这件事在验证工程里得有一个**和显隐无关**的读数。
    bool canFilterUpdates() const { return m_canFilterUpdates; }

    /// 这一趟更新检测的结果（见 MainWindow::finishUpdateCheck / ModDependencies::judgeUpdate）。
    ///
    /// @param outdatedPaths **查出来有新版本**的那些条目的绝对路径（= `pathRole()` 那一格，
    /// 禁用着的那条带着 `.disabled`）。这一份是**整份**，不是追加。
    /// @param checked 这个实例查过没有。**「没查过」和「查过、没有」是两件事**：
    /// 前者选「可更新」时显示尚无检查结果，后者该说"都是最新的"。
    ///
    /// NOTE: 这一层只认路径 ⇒ 它在"盘上那一条现在叫什么"这件事上跟 refresh() 同一条口径
    /// （两边都是 `pathRole()`），所以禁用/启用之后路径变了、这一条也就不在表里了
    /// —— 那正是对的：改过名之后上一趟查出来的结论本来就不作数了。
    void setUpdateResult(const QStringList &outdatedPaths, bool checked);
    bool updateChecked() const { return m_updateChecked; }

    /// 这一页看哪个目录（绝对路径）。**由调用方算好递进来** —— 目录名是 MC 的约定，
    /// 仓库里已经有唯一一份（src/download/ResourceCatalog::folderForKind），
    /// 这一层不抄第二份。
    ///
    /// 灌进来的那一刻，**如果这一页不是用户正看着的那一页，就先不列**
    /// （把这一趟欠着，见 `refreshPending()`）。开一次实例设置会把七个目录一起灌进来，
    /// 而用户看的是「概览」那一页：
    /// 七页各列一遍目录、逐条 `stat`、逐条建行，全是白做的。
    /// 补上的那一刻只有一个出处：**它被摆到台前**（`InstanceSettingsDialog::applyPageKey()`
    /// 在 `setCurrentIndex()` 之后一定会喊 `refresh()`）。
    void setDirectory(const QString &dir);
    QString directory() const { return m_directory; }

    // ---------- 「结构/原理图」那一页顶上那两排「来源」----------
    //
    // 【为什么这一页要有这么两排】
    //
    // 两排下拉框**横着并排：上面一排是存档的，下面一排是投影的**。次序钉死，
    // 不随调用方换。
    //
    // 【这一层不知道什么】
    //
    // 哪些存档真有结构、装没装投影模组 —— 这两句话这一层都说不出口（它够不着 AppPaths，
    // 也不 include src/）。**由 MainWindow 算好递进来**（它手上两样都有），这一层只管
    // 摆控件、按下拉框里选中的那几个目录列文件。
    struct StructureSource {
        QString title;      ///< 这一排左边那个词（「存档」/「投影」）。
        QStringList labels; ///< 下拉框里那几行字。
        QStringList paths;  ///< 和 labels **一一对应**的目录（绝对路径）。
    };

    /// 摆上那几排来源下拉框。**一排一项，顺序就是屏幕上的次序**（第一排在最上面）。
    ///
    /// - **传空的（没调过也一样） = 这一页没有那两排**：列的还是 `setDirectory()` 给的那
    /// 一个目录 —— 别的七页因此**一个像素都不变**。
    /// - 一个来源**一行都没有**时（`labels` 空），下拉框里摆**一行灰的**
    /// `structureNoneText()`（写「无可用的结构」），选它就等于没选。
    /// - 列表列的是**各排现在选中的那几个目录合起来的东西**（不是"选中谁列谁" —— 那两排
    /// 同时在屏幕上，用户要的是同时看得见两样）。
    /// - 两排的排头占**同一个宽度**（取递进来的那些 `title` 里最宽的那个）⇒ 两个下拉框
    /// 左边对得齐。★ 这一条只有**真机**量得出来（离屏上量布局会假绿）—— 量的时候
    /// 要给两排喂**不一样长**的排头。
    void setStructureSources(const QList<StructureSource> &sources);

    /// 那一排「存档」的排头（**唯一出处**：界面和验证工程都读它）。
    static QString structureWorldTitle() { return QCoreApplication::translate("InstanceFilesPage", "存档"); }
    /// 那一排「投影」的排头。
    static QString structureSchematicTitle() { return QCoreApplication::translate("InstanceFilesPage", "投影"); }
    /// 「投影」那一排下拉框里那一行的字（这一排只有一个来源：那个文件夹本身）。
    static QString structureSchematicText() { return QCoreApplication::translate("InstanceFilesPage", "原理图文件夹"); }
    /// 一个来源一行都没有时，下拉框里那**一行灰字**。
    static QString structureNoneText() { return QCoreApplication::translate("InstanceFilesPage", "无可用的结构"); }
    /// 第 row 排那个下拉框的 objectName（验证工程按它找控件，**不解析文字**）。
    static QString structureComboName(int row) {
        return QStringLiteral("instanceFilesStructureCombo%1").arg(row);
    }
    /// 第 row 排那个排头标签的 objectName。
    static QString structureTitleName(int row) {
        return QStringLiteral("instanceFilesStructureTitle%1").arg(row);
    }

    /// 那几排来源下拉框（**给验证工程用**）。没摆过 = 空表。
    QList<QComboBox *> structureSourceCombos() const { return m_structureCombos; }

    /// 上面那条"先不列"欠着的那一趟还没补吗。（**只给判据用**：产品代码不该按它分支 ——
    /// 谁要用这一页的内容，就该先把这一页摆到台前。）
    bool refreshPending() const { return m_refreshPending; }

    /// 这一页**暂时用不上**（装了 Iris / OptiFine 才谈得上光影包，装了 Litematica 才谈得上
    /// 原理图）。非空 = 整页换成这一句解释（列表和那排按钮一起收起来 —— 那时候连
    /// 「打开文件夹」都容易让人以为里面的东西会有用）。空串 = 恢复正常。
    void setUnavailable(const QString &reason);

    // ---------- 列表下面那条"这一份是什么"（原理图 / 结构）----------
    //
    // 原理图要给出元数据与方块统计，并且和实例的版本对一句"可能不兼容"。
    // 那一份东西**不是这一层算得出来的**：要读 gzip NBT、要认识 Litematica 的位打包，
    // 而那些都在 src/ 里（这一层不 include src/，见文件头）。所以这一层只做三件事：
    // 建一格标签、在"选中的那一条换了"时说一声、以及把接的人递回来的话摆上去。
    //
    // NOTE: 默认**关着** —— 另外七页一个像素都不变；只有「原理图」那一页的调用方会开。
    void setInfoPanelEnabled(bool enabled);
    bool infoPanelEnabled() const { return m_infoPanelEnabled; }

    /// 把那一格的话摆上去（空串 = 收起来）。@p toolTip 走悬停提示（方块排行那种长表）。
    /// 读失败时也走这儿：接的人把 `SchematicInfo` 给的 `problem` 原样递进来。
    ///
    /// @param path 这个答案讲的是**哪一条**（就是 infoRequested() 递出去的那个）。
    /// 不是当前选中的那一条 ⇒ **整份丢掉**：一次读取要几百毫秒（大投影更久），
    /// 这段时间里用户完全来得及换行，摆上去就成了"选着 B、写着 A"。
    void setInfoText(const QString &path, const QString &text, const QString &toolTip = QString());

    static QString infoLabelName() { return QStringLiteral("instanceFilesInfoLabel"); }
    /// 发出请求到接的人回来之间，那一格挂的加载态写这句（读一份 gzip NBT 要几百毫秒）。
    static QString infoPendingText() { return QCoreApplication::translate("InstanceFilesPage", "正在读取…"); }

    /// 重新列一遍目录。切到这一页时、以及 MainWindow 动过盘之后都会调它。
    void refresh();

    /// 给一条行换上**真图标**（资源作者传的那张：模组图标 / 资源包封面）。
    ///
    /// 由 MainWindow 递进来（它才知道怎么按文件哈希反查出这是哪个资源、又怎么走
    /// ResourceStore 那套图标缓存）—— 这一层不认识网络、也不认识"图标是哪来的"。
    ///
    /// @param path 这一条的**绝对路径**（就是 item 上 pathRole() 那一格）。
    /// @param icon 那张图。**空 pixmap = 这条没有图标**，那一格退回占位图
    /// （不是画一个空白：空白和"还没查完"在屏幕上是同一个样子）。
    /// @return 认不认得出这一条（false = 这一页上没有这个路径 —— 用户可能刚刷新过、
    /// 或者已经删了它。**不是错误**，调用点什么都不用做）。
    ///
    /// NOTE: 递进来的每一张都**按路径记在页上**（`m_icons`），所以「刷新」之后再摆出来的
    /// 那一行还是这张图 —— 见 refresh() 里那段。空 pixmap 也记（它记的是"这一条问过
    /// 了、没有"），所以刷新之后不会又退回占位图再等一次回答。
    bool setIconForPath(const QString &path, const QPixmap &icon);

    /// 这一条**认出来了是哪个资源** ⇒ 行尾那颗「详情」按钮**点得动**（见文件头
    /// 【批量管理】）。
    ///
    /// 判据是"这一条认出来了没有"：不是每一行都能点，点了没反应的那些才是
    /// 最难用的界面。
    ///
    /// @param path 这一条的**绝对路径**（就是 item 上 pathRole() 那一格）。
    /// @param tip 悬停提示（说清点下去会去哪儿 —— 它要**离开这一页**）。
    /// **空串 = 这一条没认出来**：那颗详情按钮 `setEnabled(false)`（灰着，看得见但
    /// 按不动 —— 和"这一行没有这一格"不一样，一列下来三格要对得齐）。
    /// 空串这一支**不是"没话说"**：同一个路径上换了内容之后，上一趟认出来的那个
    /// 资源就不作数了，得把那一格**从"点得动"退回"点不动"**。所以空串这一支
    /// 只在"这一格本来记着账"的时候办事（见 .cpp 里 `hadLink` 那一段）。
    /// @return 同 setIconForPath()：这一页上现在有没有这个路径（false 不是错误 ——
    /// 用户可能刚刷新过、或者已经删了它）。
    ///
    /// NOTE: 和 setIconForPath() 一样**按 `identityPath(路径)` 记账**（`m_resourceTips`），所以「刷新」之后
    /// 重列出来的那一行照样是点得动的 —— 身份是反查来的（读盘 + 联网，几秒），
    /// 不能跟着重列再问一遍。
    /// NOTE: 这一层**不知道点了会到哪儿**，它只发 detailRequested(path)：去哪一页、
    /// 开不开得成，都由 MainWindow 说了算（"这个文件是哪个资源"那张表在它手上）。
    bool setResourceForPath(const QString &path, const QString &tip);

    // ---------- 给验证工程用的访问器 ----------
    //
    // 每一行是**代理画的**（`RowDelegate`，见 InstanceFilesPageRows.cpp），不是一行一套控件：
    // 60 个模组就是 60 个 `QListWidgetItem`，列表里没有一个行控件。条目上的东西走 item->data()：
    // pathRole() = 绝对路径（发给 MainWindow 的就是它）
    // disabledRole() = 这一条现在是不是禁用着（`X.jar.disabled`）
    // Qt::AccessibleTextRole = 名字那一行画的字（含「（已禁用）」和更新箭头，读屏读的也是它；
    //                          DisplayRole 空着，免得列表按首字母跳行）
    // Qt::CheckStateRole = 开关开着没有（只有带开关的行才有这一格，读屏据此报开 / 关）
    // Qt::DecorationRole = 行首那张图（真图标或占位图，已缩到那一格大小）
    // metaRole() = 名字底下那行小字（大小 / 时间、目录的「正在统计…」）
    // 行号会随 refresh() 变，所以别跨 refresh 存着。
    QListWidget *list() const { return m_list; }

    /// 行尾那几格（`[详情][开关][笔][垃圾桶]`，按页开关决定有哪几格）。
    enum class RowAction { None, Detail, Toggle, Edit, Delete };
    /// 第 row 行各格在**视口坐标**里的矩形（代理画和点击命中都认它）。这一行没有的格子是空矩形。
    /// 「详情」那一格**只在悬停那一行画**，但这里照样给出它的位置（藏着时留着占位，名字一列不抖）。
    struct RowGeometry {
        QRect row, icon, name, meta, detail, toggle, edit, remove;
    };
    RowGeometry rowGeometry(int row) const;
    QRect rowActionRect(int row, RowAction action) const;
    /// 那一格点不点得动：只有「详情」会灰（资源没认出来，`m_resourceTips` 里没有这一条）。
    bool rowActionEnabled(int row, RowAction action) const;
    /// 那一格的悬停提示（开关说「禁用 / 启用所选文件」、垃圾桶说删什么、详情说去哪）。
    QString rowActionToolTip(int row, RowAction action) const;
    /// 「详情」那一格此刻画没画（= 光标正压在这一行上，且这一页开了批量）。
    bool rowDetailShown(int row) const;
    /// 上一次画的时候名字那一行收没收省略号（从中间截，尾巴上的「（已禁用）」和 ↑ 留着）。
    bool rowNameElided(int row) const;
    /// 第 row 行此刻是不是正在淡进来（只有这一趟新冒出来的行才淡，见 rebuildRows）。
    bool rowFadingIn(int row) const;
    /// 照真鼠标那样点第 row 行的某一格：先移过去（悬停）、再按下、松开，走和用户同一条事件路。
    /// 那一格不在视口里时不滚动列表（滚了就改了用户看着的那一屏），直接触发同一个动作。
    /// 返回那一格有没有、点不点得动（false 时什么都没发生）。
    bool clickRowAction(int row, RowAction action);
    /// 照真鼠标那样把光标移到第 row 行上（-1 = 移出列表）。
    void hoverRow(int row);
    QPushButton *refreshButton() const { return m_refresh; }
    QPushButton *openFolderButton() const { return m_openFolder; }
    /// 顶上那两颗（只有存档那一页露着，见 setImportText / setBatchText）。
    QPushButton *importButton() const { return m_import; }
    QPushButton *batchButton() const { return m_batch; }
    /// 顶上那颗「加入选中的世界」（同上：只有存档那一页接得起 `setJoinText()`）。
    QPushButton *joinButton() const { return m_join; }

    /// 现在选中的那一条的**绝对路径**（没选中任何一条 = 空串）。
    ///
    /// NOTE: 读的是**视图里真选着的**那一行（`selectedItems()`），不是 `m_currentPath`
    /// 那种缓存 —— 重列之后行是新的，缓存就会指着一个已经不在了的位置。
    /// 和 `selectedPathList()` 同一个出处、同一把钥匙（`pathRole()`）。
    QString currentPath() const;
    /// 顶上那颗「下载新 X」。**没接过 setDownloadText() 的页上它是藏着的** —— 验证工程量
    /// 的也是它"露不露"（`isVisibleTo(这一页)`），不是"存不存在"（它每一页都有，只是空着）。
    QPushButton *downloadButton() const { return m_download; }
    QLabel *noticeLabel() const { return m_notice; }
    /// 上面那条搜索框（见 setSearchText）。验证工程靠它核"敲几个字之后列表剩几条"。
    QLineEdit *searchEdit() const { return m_search; }
    /// 那一栏（全部 / 启用 / 禁用 / 可更新）。**只有没开批量的那几页用得到它**
    /// （开了批量的页把它藏起来、摆的是下面那四颗按钮）。验证工程按 `currentText()` 核它
    /// 现在选着哪一档，以及**有「可更新」的那几页是哪几页**（`count()`）。
    QComboBox *stateCombo() const { return m_stateCombo; }
    /// 那一栏左边那个标签（写「显示」）。**没接过筛选项的页上整排都是藏着的** —— 量的
    /// 是 `isVisibleTo(这一页)`。
    QLabel *stateLabel() const { return m_stateLabel; }

    /// 顶上那**四颗横向筛选按钮**（见文件头【批量管理】）。
    ///
    /// 顺序就是 `StateFilter` 那四档；**四颗都建着**（不能筛更新的那几页把「可更新」那一颗
    /// 藏起来 —— 和"下载新 X"、"导入世界"同一套做法：建/不建会让"这一页该有什么"散成两处
    /// 知识）。**一条都没有的那一档也收起来**（「全部」恒在）—— 判据只有 `stateFilterVisible()`
    /// 一处，验证工程量"露没露"**取 `isVisibleTo()`**，别拿"建没建"当判据。
    /// 验证工程一律按 `stateButtonName(filter)` 找，**不解析文字**。
    /// NOTE: 文字里带着各档的条数（`全部(352)`），所以**一页上有很多个会变的字符串** ——
    /// 认控件只能认 objectName，这正是不拿文字当判据的理由。
    QPushButton *stateButtonAt(StateFilter filter) const;

    /// 底下那条**悬浮操作条**。★ 父控件是 **`list()` 本身**（理由见 setBatchMode
    /// 上面那一段：viewport 的孩子会被滚轮一起搬走）—— 它压在列表下缘**浮着**，
    /// 不是表尾的一行。
    /// 没选中任何一条时它是藏着的（见 .cpp 里 updateSelectionBar）。
    QWidget *selectionBar() const { return m_selectionBar; }
    /// 那条操作条最左边那句「已选择 N 个文件」。
    QLabel *selectionCountLabel() const { return m_selectionCount; }
    /// 那条操作条上第 @p action 颗按钮（顺序见 `SelectionAction`）。
    QPushButton *selectionButtonAt(int action) const;
    /// 那条操作条现在的 `geometry()` —— 验证工程核它"贴在下缘居中、而且滚轮搬不动它"用
    /// （在**它爹 `list()` 的坐标系**里；offscreen 下也是真的，因为位置是这一层自己算的，
    /// 不是布局给的。要换成 viewport 坐标系就减掉 `list()->viewport()->pos()`）。
    QRect selectionBarGeometry() const;
    /// 上面那一排最左边那格（写的是这一页在看哪个目录）。文字由 refresh() 灌 ——
    /// 验证工程按它核"切到哪一页、那一格就写哪个目录"这一条。
    /// NOTE: 定义在 .cpp 里（不在这儿就地写）：它那一格的真身份是 ElidedLabel，
    /// 就地写就得把 ElidedLabel.hpp 拖进每一个用到这一页的地方 —— 对外仍旧
    /// 只当它是个 `QLabel`（调用方要的是 setText / text）。
    QLabel *pathLabel() const;

    static int pathRole() { return Qt::UserRole; }
    static int disabledRole() { return Qt::UserRole + 1; }
    /// 这一条是不是文件夹（占位图形状、目录统计、行尾那支笔都看它）。
    static int isDirRole() { return Qt::UserRole + 2; }
    /// 名字底下那行小字（大小 / 时间；目录行先是「正在统计…」，算完换成真数）。
    static int metaRole() { return Qt::UserRole + 3; }

    /// 禁用的写法：把后缀**接在完整文件名后面**（`X.jar` → `X.jar.disabled`），
    /// 启用就是把它去掉。**不是**改扩展名也不是移到别的目录 —— 游戏扫 mods 的时候
    /// 只认 `.jar` 结尾，所以这么改一定扫不到；而用户自己一眼看得出原来是什么文件。
    static QString disabledSuffix() { return QStringLiteral(".disabled"); }
    /// 界面上那条被禁用的条目叫什么（原名 + 这四个字）。
    static QString disabledMark() { return QCoreApplication::translate("InstanceFilesPage", "（已禁用）"); }

    /// 为什么非要有这么一层：禁用 / 启用**只是改了个名字**（`X.jar` ⇄ `X.jar.disabled`），
    /// 盘上那份内容一个字节都没动，大小和修改时间也一样（改名不动这两个）。所以"这一条
    /// 是哪个资源"这件事在改名前后**是同一件事**。按路径记的那两份账（图标 `m_icons` /
    /// 能点开的身份 `m_resourceTips`）要是拿**盘上那个名字**当键，点一下「禁用」那一行就
    /// 整个掉出这两份账 —— 屏幕上是图标退回占位拼图、详情那格变灰。
    ///
    /// NOTE: **只有这一份账用这把钥匙**；`pathRole()` 那一格、以及 `MainWindow` 推图标 /
    /// 身份时说的那个路径，一律是**盘上真那个名字**（推给界面时用得上的正是它）。
    /// NOTE: `MainWindow::instanceIconCacheKey()` 同一件事（那边是 `路径 + 大小 + 时间` 那把
    /// 钥匙），也走这一处 —— 两份账的规矩是同一条，出处也只有这一个。
    static QString identityPath(const QString &path) {
        const QString suffix = disabledSuffix();
        return path.endsWith(suffix, Qt::CaseInsensitive) ? path.left(path.size() - suffix.size())
                                                          : path;
    }

    /// @p bytes 的可读写法（1.5 KB / 2.0 MB …）。**唯一出处** —— 验证工程也拿它核对。
    static QString formatSize(qint64 bytes);

    // ---------- objectName（验证工程靠它们认控件） ----------
    static QString listName() { return QStringLiteral("instanceFilesList"); }
    static QString pathLabelName() { return QStringLiteral("instanceFilesPath"); }
    /// 上面那条搜索框、下面那一栏状态选择（+ 它左边那个标签）。各自只有一个。
    static QString searchEditName() { return QStringLiteral("instanceFilesSearchEdit"); }
    static QString stateComboName() { return QStringLiteral("instanceFilesStateCombo"); }
    static QString stateLabelName() { return QStringLiteral("instanceFilesStateLabel"); }
    /// 顶上那四颗横向筛选按钮。一页四颗、名字各不相同 —— 验证工程按
    /// `stateButtonName(StateFilter::Update)` 这样**按名字**找，不解析文字（文字里带着条数）。
    static QString stateButtonName(StateFilter filter) {
        switch (filter) {
        case StateFilter::Enabled:
            return QStringLiteral("instanceFilesStateButtonEnabled");
        case StateFilter::Disabled:
            return QStringLiteral("instanceFilesStateButtonDisabled");
        case StateFilter::Update:
            return QStringLiteral("instanceFilesStateButtonUpdate");
        case StateFilter::All:
            break;
        }
        return QStringLiteral("instanceFilesStateButtonAll");
    }
    /// 底下那条悬浮操作条 + 它上面那几颗（一页各一颗 ⇒ 名字各不相同、可以整页 findChild）。
    static QString selectionBarName() { return QStringLiteral("instanceFilesSelectionBar"); }
    static QString selectionCountName() { return QStringLiteral("instanceFilesSelectionCount"); }
    static QString selectionButtonName(SelectionAction action) {
        switch (action) {
        case SelectionAction::Disable:
            return QStringLiteral("instanceFilesSelectionDisable");
        case SelectionAction::Enable:
            return QStringLiteral("instanceFilesSelectionEnable");
        case SelectionAction::SelectAll:
            return QStringLiteral("instanceFilesSelectionSelectAll");
        case SelectionAction::InvertAll:
            return QStringLiteral("instanceFilesSelectionInvert");
        case SelectionAction::ClearAll:
            return QStringLiteral("instanceFilesSelectionClear");
        case SelectionAction::Delete:
            return QStringLiteral("instanceFilesSelectionDelete");
        case SelectionAction::Update:
        case SelectionAction::Count:
            break;
        }
        return QStringLiteral("instanceFilesSelectionUpdate");
    }
    static QString refreshButtonName() { return QStringLiteral("instanceFilesRefreshButton"); }
    static QString openFolderButtonName() {
        return QStringLiteral("instanceFilesOpenFolderButton");
    }
    /// 顶上那颗「下载新 X」（名字和上面两颗同处一排，认法也一样是 findChild）。
    static QString downloadButtonName() { return QStringLiteral("instanceFilesDownloadButton"); }
    static QString noticeLabelName() { return QStringLiteral("instanceFilesNotice"); }
    /// 它补的是「可更新」筛子说不出口的那件事：筛子只说得出口"这个目录里有几条这样的"
    /// （而且**没查过的时候整颗收着**，见 stateFilterVisible() 里那段），"到底是哪一条"
    /// 得站在那一行上才知道 —— 那正是这一串尾巴。
    ///
    /// 它是**名字那一格字符串的一部分**，不能旁边另起一格。另起一格（一个并排的
    /// 横排布局）时，名字那一格是 `QSizePolicy::Ignored` 的（"别拿它的 sizeHint 去撑
    /// 这一行"，见 nameLabelMinWidth() 那段），它会把整行余下的宽度**全吃掉** ——
    /// 于是箭头被推到行的最右边，离名字几百像素。拼进同一个字符串里，"紧挨着名字"和
    /// "**和名字一个字号**"也就成了**结构保证**的（同一串字、同一个控件、
    /// 同一个字体），而不是调出来的。
    /// 判据只有一处：`m_outdatedPaths`（`setUpdateResult()` 喂进来的那份名单，路径逐字
    /// 相等；禁用/启用改过名的那条自然不在里面 —— 那一行本来就是关着的，见
    /// MainWindow::checkInstanceUpdates() 里"禁用着的不查"那条）。
    static QString updateArrowText() { return QStringLiteral("↑"); }
    /// 名字那一格**最终**写的那串：有新版本的在尾巴上缀一个空格 + `↑`（**唯一出处** ——
    /// 摆行的地方和验证工程都读它，别就地拼）。@p outdated 为假时原样回 @p displayName。
    static QString nameTextWithUpdateMark(const QString &displayName, bool outdated) {
        return outdated ? displayName + QStringLiteral(" ") + updateArrowText() : displayName;
    }

    /// 名字那一格**至少**要有这么宽（px）。名字那一格是这一行的主要内容，宁可让这一行
    /// 溢出被裁，也不许它缩到看不见。
    static int nameLabelMinWidth() { return 120; }

    /// 顶上那两颗（「导入世界」/「批量编辑」）。和 downloadButtonName() 同处一排、同一个
    /// 认法；**没接过 setImportText / setBatchText 的页上它们是藏着的**（和「下载新 X」
    /// 一样：每一页都建，露不露由文字说了算）。
    static QString importButtonName() { return QStringLiteral("instanceFilesImportButton"); }
    static QString batchButtonName() { return QStringLiteral("instanceFilesBatchButton"); }
    static QString joinButtonName() { return QStringLiteral("instanceFilesJoinButton"); }

    static QString refreshText() { return QCoreApplication::translate("InstanceFilesPage", "刷新"); }
    static QString openFolderText() { return QCoreApplication::translate("InstanceFilesPage", "打开文件夹"); }
    static QString toggleOnText() { return QCoreApplication::translate("InstanceFilesPage", "禁用"); }
    static QString toggleOffText() { return QCoreApplication::translate("InstanceFilesPage", "启用"); }
    static QString deleteText() { return QCoreApplication::translate("InstanceFilesPage", "删除"); }
    /// 行尾那支笔。和 deleteText() 一样**只活在 text() / 悬停提示里**（图标是画的，
    /// QToolButton 默认不画文字）—— 验证工程按它认那一格是干什么的。
    static QString editText() { return QCoreApplication::translate("InstanceFilesPage", "编辑"); }
    /// 行尾那颗「详情」（同上：字形是画的，文字只活在 `text()` / 悬停提示里）。
    static QString detailText() { return QCoreApplication::translate("InstanceFilesPage", "详情"); }
    /// 目录行的「大小 / 时间」还没算出来的那一格写什么（见 fillDirectoryStats()）。
    static QString statsPendingText() { return QCoreApplication::translate("InstanceFilesPage", "正在统计…"); }

    // ---------- 搜索 / 筛选那两排上的字----------
    //
    // 和上面那几串一样**只活在这儿**：下拉框那四格、搜索框的灰字、以及"筛完一条不剩"
    // 那三句话都由这一组说了算，验证工程也按它们核对（写死两份的话，改了字有一处对不上
    // 就是一条假绿）。
    static QString searchPlaceholder() { return QCoreApplication::translate("InstanceFilesPage", "搜索"); }
    static QString stateLabelText() { return QCoreApplication::translate("InstanceFilesPage", "显示"); }
    /// 那一栏里四格各写什么。**顺序就是下拉框里的顺序**（见 .cpp 构造那段）。
    static QString stateFilterText(StateFilter filter) {
        switch (filter) {
        case StateFilter::Enabled:
            return QCoreApplication::translate("InstanceFilesPage", "启用");
        case StateFilter::Disabled:
            return QCoreApplication::translate("InstanceFilesPage", "禁用");
        case StateFilter::Update:
            return QCoreApplication::translate("InstanceFilesPage", "可更新");
        case StateFilter::All:
            break;
        }
        return QCoreApplication::translate("InstanceFilesPage", "全部");
    }
    /// 筛完一条都不剩时那句（搜索词或状态把这一页筛空了）。**和"目录本来就是空的"那句
    /// 是两回事**（见 m_emptyText）：这一句底下那一排筛选还露着，用户正要靠它改回来。
    static QString noMatchText() { return QCoreApplication::translate("InstanceFilesPage", "没有符合条件的条目。"); }

    // ---------- 批量管理那两处的字----------

    /// 顶上那四颗按钮上写的字。**`stateFilterText()` 仍是那些字本身的唯一出处**，
    /// 这里只是把条数接在后面（`全部(352)`）。
    static QString stateFilterButtonText(StateFilter filter, int count) {
        return QStringLiteral("%1(%2)").arg(stateFilterText(filter)).arg(count);
    }
    /// 那条悬浮操作条最左边那句。
    static QString selectionCountText(int count) {
        return QCoreApplication::translate("InstanceFilesPage", "已选择 %1 个文件").arg(count);
    }
    /// 那七颗按钮上写的字。**顺序 = SelectionAction 那七档**。
    static QString selectionActionText(SelectionAction action) {
        switch (action) {
        case SelectionAction::Update:
            return QCoreApplication::translate("InstanceFilesPage", "更新");
        case SelectionAction::Disable:
            return QCoreApplication::translate("InstanceFilesPage", "禁用");
        case SelectionAction::Enable:
            return QCoreApplication::translate("InstanceFilesPage", "启用");
        case SelectionAction::SelectAll:
            return QCoreApplication::translate("InstanceFilesPage", "全选");
        case SelectionAction::InvertAll:
            return QCoreApplication::translate("InstanceFilesPage", "反选");
        case SelectionAction::ClearAll:
            return QCoreApplication::translate("InstanceFilesPage", "取消");
        case SelectionAction::Delete:
            return QCoreApplication::translate("InstanceFilesPage", "删除");
        case SelectionAction::Count:
            break;
        }
        return QString();
    }
    /// 「更新」那一颗的悬停提示（灰着的时候也说得出为什么灰 —— 那三条灰化规则）。
    static QString selectionUpdateTip() {
        return QCoreApplication::translate("InstanceFilesPage", "更新选中的可更新项。");
    }
    /// 「禁用 / 启用」两颗的悬停提示。@param disable 这一颗是「禁用」还是「启用」。
    static QString selectionToggleTip(bool disable) {
        return disable ? QCoreApplication::translate("InstanceFilesPage", "禁用选中项，保留原文件。")
                       : QCoreApplication::translate("InstanceFilesPage", "启用选中项。");
    }
    /// 「全选 / 反选 / 取消」三颗的悬停提示。三句话说的是同一件事的另一面：**只算当前
    /// 筛出来的那些行**（看不见的那些不参与）。
    static QString selectionPickTip(SelectionAction action) {
        switch (action) {
        case SelectionAction::SelectAll:
            return QCoreApplication::translate("InstanceFilesPage", "选中当前筛出来的全部条目。");
        case SelectionAction::InvertAll:
            return QCoreApplication::translate("InstanceFilesPage", "反选当前列表。");
        case SelectionAction::ClearAll:
            return QCoreApplication::translate("InstanceFilesPage", "清空选择。");
        default:
            break;
        }
        return QString();
    }
    /// 选着「可更新」、但**这个实例还没查过更新**（见 setUpdateResult 的 checked）。
    /// 这句话得说清下一步去哪儿按 —— 不说的话用户只会以为"筛坏了"。
    ///
    /// NOTE: 开了批量那四页上现在**够不着它**了（「可更新」那一颗在没查过的时候整颗收着，
    /// 条件也就落不到这一档上，见 stateFilterVisible() 里那段）；摆下拉框的那几页
    /// 还走得到（下拉框不收空档、也不写条数）。所以这句话现在照旧是"你上别处按一颗
    /// 去"那一路。
    static QString updateNotCheckedText() {
        return QCoreApplication::translate("InstanceFilesPage", "尚无更新检查结果。");
    }
    /// 选着「可更新」、查过了、这一页里一条要更新的都没有。
    static QString updateNoneText() {
        return QCoreApplication::translate("InstanceFilesPage", "暂无可用更新");
    }
    /// 目录不存在 / 是空的时候那一句（每一页都可以自己盖掉，见 setEmptyText）。
    static QString emptyText() { return QCoreApplication::translate("InstanceFilesPage", "文件夹为空"); }

    // ---------- 「这一页现在停在哪儿」（★★ ）----------
    //
    // 点进模组详情再退出来时要保留刚才的滚动位置。
    //
    // 这一层自己本来就有一份滚动位置（`m_scrollAnchor`，见下面那两个函数）—— 那是给
    // "**同一页重列**"用的（点一下开关不跳回顶端），重列时那个对象还活着，它自然记得住。
    // 点进资源详情页不一样：回来时这一页是**整个重新 new 出来的**
    // （`MainWindow::openInstanceSettingsNow()` 里那个栈上的对话框，上一趟连着一起没了），
    // 所以位置必须**在离开那一刻量下来、由外面带回来**。
    //
    // 带出来的就是这一对数字：最上面那一行的**身份**（`identityPath()` —— 和"还选择"、
    // 重列锚点用的是同一把钥匙）+ 它当时离视口顶端多少像素。
    struct ScrollPosition {
        QString identity;  ///< 那一行的身份（`identityPath()`），空 = 没有位置
        int delta = 0;     ///< 它当时离视口顶端多少像素（`visualItemRect().top()`，通常是负数）
    };

    /// 这一页**现在**停在哪儿（拿去跨"重建这一页"）。
    /// NOTE: 是**现算**的，不是读 `m_scrollAnchor` —— 那一位只在重列那一趟前后有效，
    /// 用户点详情按钮的那一刻它多半是空的（中间没发生过重列）。
    ScrollPosition scrollPosition() const;

    /// 把上一次带出去的位置**塞回来**：这一页下次真列出来时摆到那儿。
    ///
    /// 只在"刚 new 出来、还没列过"这一刻调得到（调用点是 `openInstanceSettingsNow()`
    /// 里、`showPage()` **之前**）。能活到那一趟是靠 `rememberScrollAnchor()` 那条
    /// 规矩：**看见空表就不动手** —— 空页第一趟 `refresh()` / `rebuildRows()` 两次记
    /// 之前表都是空的，塞进来的这一份因此原样活到 `restoreScrollAnchor()`。
    void setScrollPosition(const ScrollPosition &position);

signals:
    void checkUpdatesRequested();
    void checkSelectedUpdatesRequested(const QStringList &paths);
    void projectWebsiteRequested(const QString &path);
    void inspectDependenciesRequested(const QStringList &paths);
    void transferSelectionRequested(const QStringList &paths, bool move);
    /// 用户点了「打开文件夹」，或者行首那颗（两种都开同一个目录）。
    /// **这一层不自己开**：开目录是 QDesktopServices 的事，而失败要弹一句 —— 那句话
    /// 得挂在覆盖层上（见文件头那段）。
    void openFolderRequested(const QString &path);

    /// 用户点了某一条行尾那颗**「详情」**，而这一条是认得出是哪个资源的
    /// （见 setResourceForPath()）。
    /// @param path 这一条的绝对路径（**不是**资源身份 —— 身份在 MainWindow 那张表里）。
    /// **这一层不说去哪儿**：接的人负责把这一页收掉、开那个资源自己的那一页。
    /// NOTE: 认不出来的那几条**一个字都不会发**（那颗按钮是灰的，按不动）。
    void detailRequested(const QString &path);

    /// 进入模组页时由实例设置页发起后台检查。
    /// ⚠ 别加第二个入口：这一层连 AppPaths 都够不着（见文件头那段），而且那颗按钮
    /// 本来就该是纯粹的筛子 —— 点一下筛条件就走网络是两件事挤在一颗按钮上。
    /// 哪一条有新版本由**那一行自己的 ↑**说（见 makeItem 里那个箭头）。

    /// 用户点了悬浮操作条上的**「更新」**：把选中的这些升到最新版。
    /// @param paths 选中的那些条目的**绝对路径**（现在盘上那个名字）。
    ///
    /// NOTE: 这一层**不知道"最新版"是哪一版** —— 那是 MainWindow 手里那张"这一条对应哪个
    /// 资源、它的最新版是什么"的表（见那边 `m_instanceUpdateTargets` ——
    /// 它和喂给这一页 `setUpdateResult()` 的那份名单是同一趟检测写的）。
    /// 这一层只负责说"用户点了这些条"。
    /// NOTE: 接的人**并成一批下**（不是逐条走安装那条路 —— `downloadResourceFiles()` 一次
    /// 只收一批，见 MainWindow::updateInstanceFiles() 里那段）。
    void batchUpdateRequested(const QStringList &paths);

    /// 用户点了悬浮操作条上的**「禁用」/「启用」**：把选中的这些整体改成那个状态。
    /// @param paths 选中且需要改变状态的文件绝对路径（禁用着的那些带 `.disabled`）；不含目录。
    /// @param disable true = 禁用（挂后缀），false = 启用（去后缀）。
    ///
    /// NOTE: **一颗按钮一件事**（而不是"逐条翻转"）：用户点的是"禁用"，那就该全变成禁用，
    /// 已处于目标状态的条目保持不变，数量在按钮提示中说明；无适用文件时灰化。
    void batchToggleRequested(const QStringList &paths, bool disable);

    /// 用户点了悬浮操作条上的**「删除」**。**这一层不问那句"真的要删吗"**，接的人问
    /// —— 而且对**整批**只问一次（见 MainWindow 里那一处）。
    void batchDeleteRequested(const QStringList &paths);

    /// 用户点了顶上那颗「下载新 X」。**这一层不说去哪儿**（它只认得这一页的目录，
    /// 见 setDownloadText()）—— 接的人负责切到下载页对应的那一栏。
    void downloadRequested();

    /// 用户点了某一条行尾的「禁用」/「启用」。@param path 是**现在**的绝对路径
    /// （禁用着的那一条给的是带 `.disabled` 的那个）—— 加后缀还是去后缀由接的人判。
    void toggleRequested(const QString &path);

    /// 用户点了某一条行尾的「删除」。**这一层不问那句"真的要删吗"**，接的人问。
    void deleteRequested(const QString &path);

    /// 用户点了顶上那颗「导入世界」。**这一层不问"从哪儿导、导成什么名字"** —— 它连
    /// AppPaths 都够不着（见文件头那段），挑目录和拷文件全在 MainWindow。
    void importRequested();

    /// 用户点了顶上那颗「加入选中的世界」。@param path 是选中那一条的**绝对路径**
    /// （存档目录本身）—— 进游戏要的是那个目录的**名字**，由接的人自己取
    /// （这一层和 `editRequested()` 一样只报"点了哪一条"）。
    void joinRequested(const QString &path);

    /// 用户点了顶上那颗「批量编辑」。**目前是占位**：接的人负责说那句"还没做"。
    void batchEditRequested();

    /// 列表里**刚好选中了一个文件**（不是目录），而且这一页开着信息格（见
    /// setInfoPanelEnabled）。@param path 这一条的绝对路径。
    ///
    /// NOTE: 接的人要**同步**把话算出来、调 `setInfoText()` 递回来 —— 这一层不知道
    /// "原理图是什么"，也不该等一个异步答案（用户下一页就点走了）。
    /// NOTE: 选择一变就会发一次（而且合并了 150 ms 内的连点，见 .cpp 里那个节拍器）——
    /// 读一份大投影要几百毫秒，快速翻行时不该每行都读一遍。
    void infoRequested(const QString &path);
    void infoDetailsRequested(const QString &path);

    /// 用户点了某一条行尾的「编辑」（那支笔）。@param path 是这一条的绝对路径
    /// （= `item->data(pathRole())`）。
    ///
    /// NOTE: 这一层**不说"编辑"是什么**（改名？编辑游戏规则？导出？）—— "一个世界能
    /// 做哪几件事"是 MainWindow 的知识，而且它会变（目前只有两条，将来会多）。
    /// 这一层只负责说"用户点了这一条的那支笔"。
    void editRequested(const QString &path);

    /// 用户**打开了**某一条（双击，或者选中之后按回车）。@param isDir 这一条是不是目录
    /// —— 目录和文件要的东西不一样（目录是"进去看"，文件是"打开它"）。
    ///
    /// NOTE: 这一发**只有「配置文件」那一页接**。别的几页接不接都行
    /// —— 不接就是双击没反应，不是错误。
    /// NOTE: 判 `isDir` 用的是**这一条的路径现问一次 QFileInfo**，不是记在条目上的一个
    /// 角色：路径本身是权威（refresh() 也是这么判的），而且这一条随时可能刚被改过名。
    /// 真去问一次盘比多存一位可能过期的位可靠。
    void activateRequested(const QString &path, bool isDir);

protected:
    /// **列表的 viewport 被 resize 了** ⇒ 重算那条悬浮操作条的位置（见 repositionSelectionBar）。
    ///
    /// 为什么非要一个事件过滤器：那条操作条的父控件是 `m_list`，而它**不能进布局** ——
    /// 进了布局就是"列表下面另起一行"，而不是"浮在列表下缘"。位置只能
    /// 自己算，而"该重算"的时刻正是**viewport 变大小**这一发（父控件是 `m_list` 不等于
    /// 该盯 `m_list` 的 resize：贴的是**viewport** 的下缘，算的也全是 viewport 的宽高）。
    ///
    /// NOTE: 行是代理画的，**行上的鼠标全在 `m_list->viewport()` 这一处**：
    /// · `MouseMove` / `Leave` ⇒ 记下光标压着哪一行、哪一格（悬停底、「详情」露不露、
    /// 垃圾桶变不变红都看它，见 m_hoverPath）；
    /// · 左键按在行尾某一格上 ⇒ 这一发**吃掉**（不改选择、不起拖动），松开时还在同一格才算点；
    /// · 右键 ⇒ 认得出资源的那一条**开它的详情页**，而且**吃掉这一发**
    /// （不 accept 光 return true 是拦不住的：Qt 那个"被忽略就往上冒"的循环判的是
    /// `res && isAccepted()`，只 return true 的话视图照样会自己改选中态）。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// 目录里的一条（**筛之前**的那一份）。`refresh()` 从盘上读一次存这儿，建行和筛选都
    /// 只读它 —— 于是"敲一个字就重筛一遍"不用再问一次盘（见 .cpp 里 refresh /
    /// rebuildRows 那段）。
    struct Entry {
        QString displayName;  ///< 界面上那一格写的（改名之后 + 「（已禁用）」）
        QString raw;          ///< 盘上那个名字（`X.jar.disabled`）
        QString path;         ///< 绝对路径（= pathRole() 那一格）
        bool isDir = false;
        bool disabled = false;
        qint64 size = 0;
        QDateTime modified;
    };

    /// 一条条目：名字、小字、图标、开关状态都写进 item 的角色里，交给 RowDelegate 画。
    QListWidgetItem *makeItem(const Entry &entry);
    /// 那几格怎么排（paint 与命中测试共用这一处）：@p rowRect 是条目矩形。
    RowGeometry layoutRow(const QRect &rowRect, bool isDir) const;
    /// @p pos（视口坐标）落在第几行的哪一格上；不在任何一格上 = RowAction::None。
    RowAction actionAt(const QPoint &pos, int *row = nullptr) const;
    /// 某一行某一格此刻在不在（页上那几档开关 + 这一行是不是文件夹）。
    bool rowHasAction(const QListWidgetItem *item, RowAction action) const;
    QString actionToolTip(const QListWidgetItem *item, RowAction action) const;
    bool actionEnabled(const QListWidgetItem *item, RowAction action) const;
    /// 点中一格之后真办的事（发哪个信号）。
    void triggerRowAction(const QString &path, RowAction action);
    /// 按路径找条目（找不到 = 那一行已经不在了，nullptr）。
    QListWidgetItem *itemForPath(const QString &path) const;
    /// 行尾工具按钮的大小（问那颗藏着的探针，跟着样式表走）。
    /// 造那一个画行的代理（顺带造量按钮大小的探针、淡入那只定时器），构造里调一次。
    QStyledItemDelegate *createRowDelegate();
    QSize actionButtonSize() const;

    /// 四个状态（正常 / 空目录 / **筛完一条不剩** / 用不上）之间切换可见性。refresh()、
    /// rebuildRows() 和 setUnavailable() 都走这一处，免得几条路上摆得不一样。
    ///
    /// 「顺便收起来的」只有两种：**这一页用不上**、以及**目录本来就空**（那两种情况下
    /// 搜索/状态栏都没有可筛的东西）。筛完一条不剩时那两排**留着** —— 用户正要靠它们
    /// 把条件改回来。
    void applyState();

    /// 把 `m_entries` 按现在这两条筛选条件**重列一遍**（先 clear 再逐条建行）。
    ///
    /// 【为什么筛选 = 重列，而不是"把不中的那几行藏起来"】
    ///
    /// 重列和 `refresh()` 走同一条路：只换条目（模型数据），视图和代理不动 —— 每敲一个字
    /// 重排一次条目，一个目录几十上百条也就一两毫秒。**换来的是一条：屏幕上剩下的每一条都
    /// 真的过了筛选**（不必在第二处维护"哪几行藏着"）。
    ///
    /// NOTE: 只重列**不读盘**（读盘那一步在 refresh() 里，存进 m_entries）—— 敲字时
    /// "盘上有没有变"不是这一趟要回答的问题，用户按「刷新」才是。
    void rebuildRows();

    /// 这一条过不过现在这两条筛选条件（搜索词 + 状态栏）。
    bool entryMatchesFilter(const Entry &entry) const;

    /// ---------- 重列之后**滚回原来那个位置**（★★ ）----------
    ///
    /// 点开关改了启用/禁用状态之后，重列不能把整个表弹回顶端。
    ///
    /// 根子在这一页的重列是 `clear()` + 逐条重建：那一句把滚动条连同内容一起清掉，位置
    /// 自然回到 0。而"点一下开关"这一下**从头到尾都在这一页里**（行尾的开关 → MainWindow
    /// 改名 → `refreshFilePages()` → 回到这里重列），用户手还停在刚才那一行上 —— 屏幕
    /// 啪一下跳回顶端，他得重新找刚才那一条。
    ///
    /// 记的是**最上面那一行的身份**（`identityPath()`，和还选择那一段同一把钥匙 —— 点一下
    /// 开关只是把文件改了个名，那一行还是那一行），外加它当时离视口顶端多少像素。重列之后
    /// 照这一对数字把滚动条摆回去。
    ///
    /// NOTE: 找不到那一行（**筛掉了 / 删掉了 / 换了目录**）就什么都不做 —— 落回顶端是那一
    /// 刻唯一说得通的答案（用户刚把那一页整个换掉，凭什么保留上一次的位置）。
    /// NOTE: 记住的时机有讲究：`refresh()` 是"先 clear 再叫 rebuildRows()"，所以它必须
    /// **在 clear 之前**记；而 rebuildRows() 自己也要记（筛一下搜索那条路不走
    /// refresh）。两处都叫这一个函数，它**看见空表就不动手**（否则第二处会把第一处
    /// 刚记下的那一份覆盖成"没有锚点"）。
    void rememberScrollAnchor();
    /// 按 `m_scrollAnchor` 把滚动条摆回去。**必须在重列之后、排版算完的时候**调
    /// （`rebuildRows()` 末尾那个 0 毫秒定时器里，见那一段的 NOTE）。
    void restoreScrollAnchor();

    /// 现算一遍"最上面那一行是谁、它在哪儿"（`scrollPosition()` 和
    /// `rememberScrollAnchor()` 同一份算法，收在这儿只写一次）。
    /// NOTE: **没有**"空表就不动手"那条规矩 —— 表空的时候它老老实实回一份空的
    /// （`m_list == nullptr` / 一行都没露 都算空）。那条规矩是 `rememberScrollAnchor()`
    /// 自己加的，见它那段。
    ScrollPosition captureScrollPosition() const;

    /// 重列之前最上面那一行是谁、当时在哪儿（见上面两个函数）。
    /// 类型就是外面那一份 `ScrollPosition` —— 从详情页回来时带进来的正是它。
    ScrollPosition m_scrollAnchor;

    /// 状态栏里第 @p index 格对应哪一档（那一格的数据里存的就是它，`Qt::UserRole`）。
    static StateFilter filterAt(const QComboBox *combo, int index);

    /// 按 `m_canFilterUpdates` 把状态栏那几格（重新）摆好，并把"现在选着哪一档"落回
    /// `m_stateFilter`。构造函数和 setCanFilterUpdates() 都走这一处。
    void fillStateCombo();

    /// 顶上那**四颗横向筛选按钮**重摆一遍：文字里的条数（`全部(352)`）、哪一颗亮着、
    /// 以及**哪几颗露着**。`rebuildRows()` 和 `setUpdateResult()` 都走这一处（条数和
    /// "可更新"那份名单都会从那两处变）。
    ///
    /// NOTE: 哪颗露着由 `stateFilterVisible()` 一条说了算；**"条件停在了一颗摆不出来的
    /// 按钮上"那件事不在这儿做** —— 那件事会改 `m_stateFilter`、必须在**建行之前**
    /// 就落定（见 `ensureStateFilterVisible()`），在摆按钮这一趟做的话就成了
    /// "重列的过程中又改了一次条件"。
    void fillStateButtons();

    /// 这一档现在有几条 —— **筛之前**那一份 `m_entries` 的口径（"按「禁用」之后那个数会不会
    /// 变"因此有个确定的答案：不会，它说的是这个目录里有多少条是这样），「可更新」数的则是
    /// `m_outdatedPaths ∩ m_entries`（和 `entryMatchesFilter()` 同一个口径）。
    ///
    /// 读它的就两处：按钮上的文字、以及 `stateFilterVisible()` —— 别的地方再算一遍就是
    /// 第二份真相（底下那条操作条上那几颗的灰化数的是**选中的那几条**，另一件事）。
    int stateFilterCount(StateFilter filter) const;

    /// 这一档现在**摆不摆得出来**。判据三条，每一条都只有一个出处：
    /// · 「全部」恒在 —— 别的档全收起来了，剩下的那一颗也得有个东西亮着，
    /// 不然这一排是一片空白；
    /// · 「可更新」还要这一页真能筛更新（`m_canFilterUpdates`，见 setCanFilterUpdates）；
    /// · 其余档**一条都没有就收起来** —— **只管开了批量的那四页**，没开批量的那几页
    /// 摆的是下拉框，
    /// 它不写条数（理由写在 .cpp 里）。
    bool stateFilterVisible(StateFilter filter) const;

    /// 把 `m_stateFilter` 从一档**现在摆不出来**的条件上退回来（退到「全部」），顺带把
    /// 下拉框那一格摆回来。
    ///
    /// NOTE: 落点是 `rebuildRows()` 开头那一句（还有 `setCanFilterUpdates()`）——
    /// **必须在建行之前**：建行正是按 `m_stateFilter` 筛的，条件在半路上变了的话
    /// 屏幕上剩的和亮着的那一颗就对不上，而没有别的东西会自己纠正它。
    void ensureStateFilterVisible();

    /// 按 `m_selectedPaths` 把状态栏 + 那条操作条重摆一遍：条上那句「已选择 N 个文件」、
    /// 七颗按钮各自灰不灰、以及那条**露不露**（没选中任何一条就收起来）。
    ///
    /// NOTE: 灰化规则：
    /// - 更新：选中的里面**至少一条**在 `m_outdatedPaths` 里；
    /// - 禁用：至少一条**现在开着**；启用：至少一条**现在禁用着**；
    /// - 全选 / 反选 / 取消 / 删除：恒可用。
    /// 判据是 `m_entries` 里那一条的 `disabled`（不是条目上画的那颗开关 —— 那个是画出来的）。
    void updateSelectionBar();

    /// 顶上那颗「加入选中的世界」的**灰/亮**照"现在选没选中一条"重算一次。
    ///
    /// 和 `m_selectionBar` 那一套**不是一回事**：那条只在批量那四页上活着（这一颗只在
    /// 存档那一页上活着），而两条的灰化口径也不同（那条看的是"选中的那些里有没有
    /// 可更新的"，这一颗看的是"有没有选中一条"）。
    ///
    /// NOTE: 三个地方要喊它 —— `setJoinText()`（接文字的时候顺手定一次初值）、
    /// `itemSelectionChanged`（用户点了一行）、以及重列之后（行是新的一批）。
    /// 后两处**靠同一发信号**兜住：`rebuildRows()` 的 `clear()` 就会发一次
    /// `itemSelectionChanged`（见 .cpp 里那段 NOTE）。
    void updateJoinEnabled();

    /// 把那条操作条摆到"viewport 下缘、横向居中"那一格。**由 viewport 的 Resize 事件和
    /// 每一次显隐驱动**（见 eventFilter）。
    void repositionSelectionBar();
    /// 这一页可能露出的全部状态胶囊（不按此刻露着的几颗）加间距的总宽：排法按它定，
    /// 胶囊冒出来 / 收起来时才不换行。
    int possibleStatePillsWidth() const;
    /// 紧凑页头（资源包页）的状态胶囊：页宽装得下就排进搜索那一排，装不下单占一排。
    /// 排法只看页宽，胶囊出现或消失不改列表的纵向位置。挪了位置返回 true。
    bool placeCompactStateRow();

    /// 按路径取那一份"筛之前"的条目（`m_entries`）。找不到给 nullptr。
    const Entry *entryForPath(const QString &path) const;

    /// 「选中了哪几条」的账**从列表里现在真选着的那些行现问一遍**（见 m_selectedPaths 那段）。
    ///
    /// 两处调：`itemSelectionChanged` 那一支（用户点了）和 `rebuildRows()` 末尾（我们重列完
    /// 了 —— 那一段把信号挡着，所以必须自己把账摆对）。
    void syncSelectedPaths();

    /// 按光标**此刻**的位置重认悬停那一行 / 那一格（见 m_hoverPath）。
    ///
    /// 两处调，治的是同一件事：**鼠标没动过、光标底下那一行却换了**（没有 MouseMove 可等）：
    /// · `rebuildRows()` 末尾（重列换了一批条目；★ 排在 0 毫秒定时器里，等视图排完版）；
    /// · 列表滚动条 `valueChanged`（滚轮一动，光标底下换人了）。
    void syncRowHover();
    /// 把悬停记到 @p path / @p action 上，变了才重画（旧那行和新那行）。
    void setHover(const QString &path, RowAction action);

    /// 选中的那些路径，按**列表里那一行的先后**排成一份（发给 MainWindow 的形态）。
    QStringList selectedPathList() const;
    /// 只返回需要改变状态的选中文件，保持界面顺序。
    QStringList selectedTogglePaths(bool disable) const;

    /// 发「启用 / 禁用」之前**先登记**这几条（`m_pendingIdentityMigration`，见那段）。
    ///
    /// 所有会改名的地方都必须走这两句，不要直接 `emit batchToggleRequested/toggleRequested`：
    /// 少登记一处，那一处点完开关、重列之后那一行就会掉出选择。
    void requestToggle(const QString &path);
    void requestToggle(const QStringList &paths, bool disable);

    /// 这一条那一格该摆什么：真图标在 `m_icons` 里就摆它，否则摆占位图（形状按 isDir 分，
    /// 见 .cpp 里 placeholderIcon）。makeItem() 和 setIconForPath() 都走这一处 ——
    /// 两处各写一遍的话，"没查到"那一档迟早会有一处漏掉、退成空白。
    QPixmap iconForPath(const QString &path, bool isDir, const QColor &placeholderColor) const;

    /// 把「正在统计…」那一格换成真的数（一个世界的字节数 + 上次游玩时间）。
    /// **一趟只算一行**，算完还有剩的就再排一趟 —— 理由写在 .cpp 里（一个装着几百个世界
    /// 的 saves/ 一次算完会让界面卡住十几秒，而那是页面已经摆出来之后卡住）。
    void fillDirectoryStats();

    Mode m_mode = Mode::FilesAndDirs;
    /// 行首那一格图标（见 setRowIconsVisible()）。默认露。
    bool m_rowIconsVisible = true;
    bool m_canToggle = false;
    bool m_canDelete = false;
    /// 这套「批量管理」开不开（见 setBatchMode）。
    bool m_batchMode = false;
    /// 行尾那支笔（「编辑」）露不露。**只有「存档」那一页为真**（见 setCanEdit()）。
    /// NOTE: 它同时还是"这一行的小字里画真数还是画一个 『—』"的开关 —— 那一页每一行都是
    /// 一个世界目录，统计大小/最后游玩时间是那一页专有的（别的那几页全是文件，大小
    /// 本来就有）。
    bool m_canEdit = false;
    QString m_emptyText;
    QString m_directory;
    /// 顶上那几排「来源」（见 setStructureSources）。**空 = 这一页没有那两排**，列的就是
    /// m_directory 那一个目录 —— 那是别的七页走的路。
    QList<StructureSource> m_structureSources;
    /// 和 m_structureSources **逐位对齐**的那几个下拉框 / 排头标签。
    QList<QComboBox *> m_structureCombos;
    QList<QLabel *> m_structureTitles;
    /// 用不上的原因（空串 = 用得上）。见 setUnavailable()。
    QString m_unavailable;
    /// 上一次 refresh() 之后**目录里**是不是一条都没有（还没过筛选这一层）。**只由
    /// refresh() 写**（见 applyState()）。
    bool m_empty = true;
    /// 目录里有东西、但**这一趟筛完一条都不剩**（搜索词或状态栏干的）。**只由
    /// rebuildRows() 写**。和 m_empty 分开：前者底下要露着筛选那两排（用户正靠它改回来），
    /// 后者整块收起来（没东西可筛）。
    bool m_filteredEmpty = false;

    /// 目录里那些条目，**筛之前**的那一份（见 Entry / rebuildRows()）。
    QList<Entry> m_entries;

    /// 现在搜索框里写着什么（setSearchText / 用户敲字都写它）。**筛的是它**，不是控件上
    /// 此刻的文字 —— 免得"这一页在筛什么"这件事有两个出处。
    QString m_searchText;
    /// 状态栏里现在选着哪一档。默认「全部」。
    StateFilter m_stateFilter = StateFilter::All;
    /// 状态栏里要不要有「可更新」（见 setCanFilterUpdates）。**默认给**：能筛更新的那几页
    /// 是多数，不开的那三页由对话框按页关掉（宁可多一格"永远筛不出东西"的，也不要因为
    /// 忘了打开而少一格 —— 后者没人看得出来）。
    bool m_canFilterUpdates = true;

    /// 查出来**有新版本**的那些条目的路径（见 setUpdateResult）。在表里 = 这一条可更新。
    /// NOTE: 空表 + `m_updateChecked` 为假 = "还没查过"，那是两件事（见 updateNotCheckedText）。
    QSet<QString> m_outdatedPaths;
    /// 这个实例查过更新没有（见 setUpdateResult）。
    bool m_updateChecked = false;

    /// 已经拿到的**真图标**，按 `identityPath(路径)` 存（见 setIconForPath()）——
    /// 禁用/启用只改名字，图标还是那一张。
    ///
    /// 【为什么必须由这一页自己记着】
    ///
    /// `refresh()` 是 `m_list->clear()` + 重新建行 —— 挂在这一页上的图全跟着没了。而
    /// 「刷新」和「启用/禁用一条」都会走它（禁用就是改文件名，落盘之后要重列），图标却是
    /// 反查来的（读盘 + 联网，几秒），**不能跟着重来一遍**。记在这儿，重列出来的那一行
    /// 直接就是对的图。
    ///
    /// NOTE: **空的也存**（值是一张空 pixmap）：它记的是"这一条问过了、没有"，
    /// 和"还没问"是两件事（见 setIconForPath 的 NOTE）。
    /// NOTE: 这一份**跟着这一页活**（对话框一关就没了）—— 跨对话框的缓存是 MainWindow
    /// 那一份的事（按 路径+大小+时间 存，见 requestInstanceFileIcons 那段）。
    QHash<QString, QPixmap> m_icons;

    /// 认出来了是哪个资源的那几条，按 `identityPath(路径)` 存**悬停提示**（见
    /// setResourceForPath()）。在表里 = 那一行的详情按钮点得动。
    /// 和 m_icons 同一把钥匙（理由逐字相同）：禁用/启用只改名字，认出来的还是同一个资源。
    ///
    /// NOTE: 和 m_icons 同一档、同一个理由：`refresh()` 是 `m_list->clear()` + 重新建行，
    /// 而身份是反查来的（几秒），不能跟着重列再来一遍。刷新之前记着的，重列出来
    /// 的那一行直接就是能点的。
    /// NOTE: 只记**认出来了**的那些（空 tip 不记账）—— 记一条空的是"这一条点不动"，
    /// 而那本来就是默认状态。
    QHash<QString, QString> m_resourceTips;

    /// **最上面那一排**：搜索框（摆在按钮那排**上方**）。
    /// 目录本来就空 / 这一页用不上的时候整排收起来（没东西可筛）。
    QWidget *m_searchRow = nullptr;
    QLineEdit *m_search = nullptr;
    /// 上面那一排（路径 + 两颗按钮）。「用不上」的时候整排一起收起来。
    QWidget *m_pathRow = nullptr;
    bool m_worldLayout = false;
    bool m_compactHeader = false;
    /// 顶上那几排「来源」下拉框的容器（**只有「结构/原理图」那一页会露**，见
    /// setStructureSources）。摆在路径那一排**之下**、状态那一排之上。
    QWidget *m_structureRows = nullptr;
    QVBoxLayout *m_structureLayout = nullptr;
    /// **按钮那排下面那一排**：非批量页上是「显示」+ 下拉框（在三个按钮的下方），
    /// 批量页上是那四颗横向按钮。**两套都在这一排里**，
    /// 按 `m_batchMode` 互相显隐（见 applyState）。
    QWidget *m_stateRow = nullptr;
    QLabel *m_stateLabel = nullptr;
    QComboBox *m_stateCombo = nullptr;
    /// 那四颗横向筛选按钮的容器（它自己在一个小横排里）。整块和上面那个下拉框**互相显隐**。
    QWidget *m_stateButtonsRow = nullptr;
    /// 四颗横向筛选按钮，**下标就是 `static_cast<int>(StateFilter)`**（四颗都建着，
    /// 不能筛更新的页把最后一颗藏起来 —— 见 stateButtonAt 那段的 NOTE）。
    QList<QPushButton *> m_stateButtons;
    /// 是 ElidedLabel（从中间省略，见 .cpp 里构造那段的理由）；对外仍旧只当它是个 QLabel
    /// （见 pathLabel() 为什么定义在 .cpp 里）。
    ElidedLabel *m_pathLabel = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_openFolder = nullptr;
    /// 顶上那颗「下载新 X」（见 setDownloadText()）。**每一页都建**（同一个形状搭出来的），
    /// 没接文字的那几页是藏着的。
    QPushButton *m_download = nullptr;
    /// 顶上那两颗（「导入世界」/「批量编辑」）。同上：每一页都建，没接文字就藏着。
    QPushButton *m_import = nullptr;
    QPushButton *m_batch = nullptr;
    /// 顶上那颗「加入选中的世界」。**每一页都建**（和上面那几颗同一条理由：
    /// 这几页是同一段循环搭出来的），没接过 `setJoinText()` 的页上它藏着的。
    QPushButton *m_join = nullptr;
    /// 「加入选中的世界」的悬停提示（setJoinText() 递进来的那一句）。**存在这儿**是因为
    /// 那颗按钮"选中/没选中"时提示要换（见 updateJoinEnabled()）—— 换完还得换得回**原来
    /// 那一句**，而那句话是调用方给的，这一层编不出来。
    QString m_joinTip;
    QListWidget *m_list = nullptr;
    QLabel *m_notice = nullptr;

    // ---------- 列表下面那条"这一份是什么"----------
    // / 那一格标签。**每一页都建**（这几页是同一段循环搭的），只有
    // / `setInfoPanelEnabled(true)` 的页上它才可能露出来。
    QLabel *m_info = nullptr;
    QPushButton *m_infoDetails = nullptr;
    /// 这一页要不要那一格（默认关 —— 别的七页一个像素都不变）。
    bool m_infoPanelEnabled = false;
    /// 已经为哪一条发过请求（避免"选中没变、只是列表重列"时又读一遍盘）。
    QString m_infoPath;
    /// 连点合并用的单发定时器（150 ms；见 .cpp 里 updateInfoPanel()）。
    QTimer *m_infoTimer = nullptr;

    /// 按"现在选着哪一条"决定那一格露不露、写什么、要不要发 infoRequested()。
    /// 选择变化、重列、开/关信息格时都会调它。
    void updateInfoPanel();
    void stopInfoLoading();  ///< 撤掉那一格的加载态、放开为它留的行高

    // ---------- 批量管理（见文件头那段）----------

    /// 选中的那些条目的**路径**（= `pathRole()` 那一格）。**"选中了哪几条"的唯一出处。**
    ///
    /// 【为什么按路径记，而不是问 `QListWidget::selectedItems()`】
    ///
    /// `rebuildRows()` 是 `clear()` + 重建 —— 那一句会把选择整个清掉。而"筛一下搜索、
    /// 禁用一个文件、查完更新重列一遍"全都会走它，用户手上的选择不该因为其中任何一件
    /// 事消失。所以选择**记在页上、按路径**，重列的时候按它还回去。
    ///
    /// **点开关改名之后那一条照样选着**。往回收的判据是 `identityPath(路径)` ——
    /// 禁用 / 启用只是把 `X.jar`
    /// 改成 `X.jar.disabled`，盘上还是同一个文件、用户手里选的就是"这一条"，所以重列出来
    /// 的那一行还得是选着的。而**账上记的仍是盘上真那个名字**（`selectedPathList()` 报出去、
    /// 底下那条操作条拿去办事的，一律是它）。
    /// ⚠ `X.jar` 与 `X.jar.disabled` 同时在场时身份撞上 —— 那种情况**一条都不替用户
    /// 选**（见 `m_pendingIdentityMigration` 和 `rebuildRows()` 里那两份准账）。
    /// 重列之后**只认屏幕上真摆出来的那些行**（`syncSelectedPaths()` 一句从列表里现问
    /// 一遍）：选择永远只含看得见的条目，"已选择 3 个文件"说的就是那 3 个能看见的。筛一下
    /// 搜索就掉出几条，是对的行为（和"全选/反选只作用于当前筛出来的"同一个口径）。
    QSet<QString> m_selectedPaths;

    /// 登记身份是因为**删除**也会让旧路径消失：`X.jar` 与 `X.jar.disabled` 同时在场、
    /// 用户只勾了 `X.jar` 去删，删完重列时同身份只剩 `X.jar.disabled` 一条 ⇒ 若把
    /// "旧路径消失"都当成改名，它会被自动选中，用户再按一次「删除选中」就把自己
    /// **从没选过**的文件删掉了（不可逆）。
    ///
    /// 只有**从这一页发出去的启用/禁用**（唯一会改名的动作）才登记身份；删除 /
    /// 移动 / 别处改名的消失一律不还选择。登记之后下发那一趟是同步刷新（`refreshFilePages()`
    /// → `refresh()` → 这里），重列末尾会清掉它 —— 一次登记只服务一趟重列。
    QSet<QString> m_pendingIdentityMigration;

    QSet<QString> m_shownIdentities;

    bool m_rowsBuilt = false;

    // ---------- 代理画行用的状态 ----------
    class RowDelegate;
    friend class RowDelegate;
    /// 光标压着的那一行（路径）和那一格。悬停淡底、「详情」露不露、垃圾桶变不变红都看它 ——
    /// 不用视图自带的 `State_MouseOver`：重列 / 滚动之后它要等鼠标再动一下才更新。
    QString m_hoverPath;
    RowAction m_hoverAction = RowAction::None;
    /// 左键按在哪一行的哪一格上（松开时还在同一格才算点；开关按住时圆点拉长）。
    QString m_pressActionPath;
    RowAction m_pressAction = RowAction::None;
    /// 上一次画时名字收了省略号的那些路径（验证工程读它）。
    mutable QSet<QString> m_elidedNames;
    /// 「只淡新冒出来的那几行」：这一批的身份 + 起点（RowDelegate 按经过的时间算不透明度）。
    QSet<QString> m_fadeIdentities;
    QElapsedTimer m_fadeClock;
    QTimer *m_fadeTimer = nullptr;
    /// 量行尾按钮大小、交给样式画按钮用的那一颗（藏着，一页一颗，不是一行一颗）。
    QToolButton *m_buttonProbe = nullptr;

    /// 底下那条**悬浮操作条**。★ 父控件是 **`m_list`**（为什么不能用 viewport 见
    /// setBatchMode 上面那一段 —— viewport 的孩子会被滚轮一起搬走），
    /// **不进任何布局**（进了布局就变成"列表下面另起一行"，不是浮在列表上）—— 位置由
    /// repositionSelectionBar() 算（算出来的是 viewport 坐标系，摆的时候加 viewport->pos()）。
    QWidget *m_selectionBar = nullptr;
    QScrollArea *m_actionScroll = nullptr;
    bool m_actionPanelEnabled = false;
    bool m_actionPanelDismissed = false;
    QWidget *m_selectionTools = nullptr;
    QWidget *m_contentHost = nullptr;
    QPushButton *m_selectionReopen = nullptr;
    QLabel *m_selectionSummary = nullptr;
    QLabel *m_actionStatus = nullptr;
    QLabel *m_selectionCount = nullptr;
    /// 那七颗按钮，**下标就是 `static_cast<int>(SelectionAction)`**。
    QList<QPushButton *> m_selectionButtons;

    /// 等着算大小/时间的那些目录行（算完写回那一条的 metaRole()，按路径找回）。
    ///
    /// 目录的字节数要**递归遍历**整棵树（`saves/<世界>/region/*.mca` 动辄几十万个文件），
    /// 而 `refresh()` 是在"切到这一页"那一瞬间同步跑的 —— 在里面算等于每开一次实例设置都
    /// 卡住几秒到十几秒。所以建行时先摆一句「正在统计…」，真正的数由定时器在**事件循环
    /// 转起来之后**一格一格补上；补的时候那一行不在了（用户刚刷新或删了它）就跳过。
    /// 已经算出来的目录大小/时间：同一趟里重列（筛一下搜索）直接摆，不重算。
    QHash<QString, QString> m_statCache;
    /// 还没算的那些（按轮到的先后）。队列空 = 这一趟统计做完了。
    QStringList m_statQueue;
    /// 排下一行的定时器（单发、0 毫秒：下一个事件循环转过来就干一行）。
    QTimer *m_statTimer = nullptr;

    /// `setDirectory()` 把目录记下来、但**这一趟还没列**（见那个函数和 `refreshPending()`）。
    /// 只有两处会动它：`setDirectory()` 置上、`refresh()` 清掉。
    bool m_refreshPending = false;

    /// 这一页此刻是不是**堆叠页里当前摆着的那一格**（`setDirectory()` 那条判据）。
    /// 不在任何堆叠容器里（单独用这个控件的那几个验证工程）恒为 true —— 那种用法没有
    /// "当前页"这回事，照旧立刻列。
    bool isCurrentPage() const;

    /// 第 row 排「来源」下拉框**现在选着的那个目录**（没得选 / 选的是「无可用的结构」
    /// 那一行 ⇒ 空串）。见 setStructureSources()。
    QString selectedStructurePath(int row) const;

    /// 这一趟 refresh() 要读盘的那几个目录。**没有「来源」那两排时就是 `m_directory`
    /// 一个**（别的七页走的路）；有那两排时是它们**现在选中的那几个**（去重、按排的次序）。
    /// 唯一出处 —— refresh() 只读它。
    QStringList scanDirectories() const;

    // ---------- 拖出 / Ctrl 框选取反 ----------
    //
    // 项上直接拖动 = 把选中的文件拖出去（Windows 原生 `CF_HDROP`）；Ctrl+拖动 = 框选，
    // 扫过的每一条「选中↔未选中」翻转；空白处直接拖动交给 Qt 的框选。
    /// 拖动手势的判据（纯函数，验证工程直接调）：不到阈值 → None；按住 Ctrl → 框选取反；
    /// 否则只有"按在行上"才算拖出（空白处留给 Qt 的框选）。
public:
    enum class DragIntent { None, DragOut, ToggleSelection };
    static DragIntent dragIntent(int distancePx, int thresholdPx, bool onRow, bool ctrlPressed) {
        if (distancePx < thresholdPx) return DragIntent::None;
        if (ctrlPressed) return DragIntent::ToggleSelection;
        return onRow ? DragIntent::DragOut : DragIntent::None;
    }
    void beginFileDrag();
private:
    void updateToggleBand(const QPoint &viewportPos, bool firstUpdate);
    QPoint m_pressViewportPos;
    /// 按下那一刻的选择快照：Qt 会先按"单击"把多选收缩成一条，拖出要的是按下前的那些。
    QStringList m_pressSelection;
    QString m_pressPath;
    bool m_pressOnRow = false;
    bool m_dragStarted = false;
    bool m_ctrlDragActive = false;
    /// 已选行的普通点击在 release 时决定取消单选或收缩多选；拖拽保持原选择。
    bool m_swallowPress = false;
    QRubberBand *m_toggleBand = nullptr;
    QSet<QString> m_bandToggledIdentities;

};





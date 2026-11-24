/**
 * 【这一层一个字节都不上网、也不知道 Modrinth 是什么】
 *
 * 进参出参都是**值**（Header / Chip / Group / Row 都是本文件自己的），不 include 任何 src/
 * 下的头 —— 和 InstanceSettingsDialog / GroupDialog 同一条约定（ui/ 不依赖业务层）。
 * 跟网络与磁盘有关的三件事全都由 MainWindow 代办，这一页只发信号：
 * * `chipChanged()` —— 用户点了某个游戏版本，**筛和分组由 MainWindow 重算再 setGroups()**
 * （分组的规则在 ResourceCatalog::groupVersions 里，那是业务层的事）；
 * * `openExternalRequested()` —— 「转到 XXX」那一类按钮（开浏览器是 MainWindow 的事，
 * 它还负责把 QDesktopServices 换成验证工程能拦的那个 handler）；
 * * `rowActivated()` —— 点了某一个具体版本 = 要下它（下到哪儿、走哪条链路也是 MainWindow）。
 *
 * 【行里的东西全是拼好的字符串】
 *
 * `Row::title` / `Row::subText` 是**画什么就是什么**：这一页不做"下载量怎么读""没有文件名
 * 时省掉哪一截"这类判断（那些在 ResourceCatalog::versionRowSubText 里，有断言钉着）。
 * 这一页只决定"用哪个代理画、点哪儿算点了这一行"。
 *
 * 【为什么列表还是 QTreeWidget + 共用代理】
 *
 * 组头 = 顶层条目（代理给顶层画粗体 + 右边那个能转的箭头）、版本 = 子条目（代理给它画
 * 方块图标 + 正文 + 下面那行小字）。这一套和下载页那四张资源表、版本树、加载器树**是同一个
 * 代理**（VersionTreeDelegate）—— 界面样式四处一致，靠的就是这个代理已经画好的形状。
 *
 * 列表**不画叶子那一层右边那个 `>`**（构造代理时传 `leafArrow = false`）：点一个版本是
 * "下它"（一个动作），不是"点进去还有一页"，挂个 `>` 等于骗人说里面有东西 —— 和加载器树
 * 上那两档同一条理由。
 *
 * 【组头那一行是"开关"，版本那一行是"动作"】
 *
 * 点击的接线走 `connectRowActivation()`（那个文件头把"一次手势只调一次"讲透了）：组头那
 * 一层算开关（点一下展开、再点一下收起，**点得快时后面那几下也都要算**），版本那一层算
 * 动作（双击不该下两遍）。
 */
#pragma once

#include <QColor>
#include <QDialog>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QStringList>

#include <functional>

class QCheckBox;
class QKeyEvent;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

/// 这一页要的那份值。名字里的 Resource 是"资源"（模组 / 整合包 / 数据包 / 资源包 / 光影）。
namespace ResourceDetail {

/// 顶上那块卡。
struct Header {
    QString title;       ///< 资源名（接口给哪个就用哪个，没有第二份中文名可显示）
    QString description; ///< 一句话简介（**画一行**，放不下从尾巴截成省略号；全文挂在
                         /// < 鼠标提示上。空 = 这一行不占地方）
                         /// 名字底下那行元信息：「Fabric / NeoForge / 下载 2.31 亿次 / 更新于 5 天前 / Modrinth」。
                         /// 每一截在不在由 MainWindow 拼（这儿只画一行）。空串 = 这一行不画。
    QString metaLine;
    /// 那颗「转到 XXX」按钮上的字（`Modrinth` / `CurseForge`）与它的地址。
    /// 地址空 = 那颗按钮**不画**（拼不出网址时不该摆一颗点了没反应的按钮）。
    QString openLabel;
    QString openUrl;
    /// 「转到 MC 百科」那颗按钮的地址（空 = 不画）。
    QString mcmodUrl;
    /// 「复制名称」复制的那串（空 = 不画）。
    QString nameToCopy;
};

/// 那一排大版本 chip 里的一颗（「全部」、1.21、1.20……、「其他」）。**key 才是身份**（文字只是给人看的）。
struct Chip {
    QString key;  ///< 空串 = 「全部」（不筛）
    QString text; ///< 按钮上那行字
};

/// 版本列表里的一行（= 某一个具体版本）。
struct Row {
    /// 点它时要认的那一条。**唯一出处是 MainWindow**（它拿着 VersionEntry，知道该下哪个
    /// 文件）；这一页只把它原样带回来，一个字都不解释。条目上放在 `rowKeyRole()` 里。
    QString key;
    QString title;
    QString subText;
    /// 左边那个字母方块：0 正式版（R）/ 1 测试版（B）/ 2 早期测试版（A）。
    /// 值和 `VersionEntry::versionType` 那三个词的次序对齐（release / beta / alpha）。
    int channel = 0;
    /// 鼠标停上去的整行提示（正文太长时看全文）。空 = 不挂。
    QString tooltip;
};

/// 一个组（组头 + 组里那些版本）。
struct Group {
    /// 「Fabric 26.2」「通用 1.21.1」，MainWindow 那边按自己给的 chip 算好 —— 这一页
    /// 一个字都不拼（"哪几个版本算一组"是业务层的事，见 ResourceCatalog::groupVersions）。
    QString title;
    QList<Row> rows;
    /// 开页时是否摊开。默认收起。
    bool expanded = false;
};

}  // namespace ResourceDetail

class ResourceDetailPage : public QDialog {
    Q_OBJECT
    Q_PROPERTY(QString currentChip READ currentChip)

public:
    explicit ResourceDetailPage(QWidget *parent = nullptr);

    /// 页眉上那行字（覆盖层那一层显示它，见 OverlayHost::showPage）。拿的是资源名。
    QString titleText() const;

    /// 头卡那一块。**在 showPage 之前调**（页面开着的时候再换整块卡没人这么用）。
    void setHeader(const ResourceDetail::Header &header);

    /// 资源图标（异步来的，走 ResourceStore 那一套）。传空的 QPixmap = 这个资源没有图标，
    /// 那一格留白（**留着同样大的位置**，不然图回来的时候整张卡会往左跳一下）。
    void setIcon(const QPixmap &icon);

    /// 顶排的大版本 chip（「全部」、1.21、1.20……、「其他」）。`preselectedKey` 是要**按下去**的
    /// 那一个（空 = 「全部」）。表是 MainWindow 给的（`ResourceCatalog::gameVersionSeriesChoices()`
    /// 那一份），这一页不自己算。两排以上时折行，不出横向滚动条。
    void setChips(const QList<ResourceDetail::Chip> &chips, const QString &preselectedKey);

    /// 现在按下去的是哪一个的 **key**（「全部」= 空串）。验证工程和 MainWindow 都读它。
    QString currentChip() const;

    /// 那一排 chip 右边那个「显示快照版」勾选框。
    ///
    /// **它和 chip 那一排是同一件事的两半**：勾上 = 每一条声明过的版本各摆一颗（`26.2` 和
    /// `26.2-rc1` 各是各的）、不勾 = 快照归到对应的大版本（只剩一颗 `26.2`）。所以拨它
    /// **不只是换个勾** —— 调用方要重摆那一排 chip 并重算下面那份列表（见
    /// `showSnapshotsChanged`）。
    ///
    /// NOTE: 这一页自己不重摆（"哪些版本归到哪一个"是业务层的事，见
    /// ResourceCatalog::gameVersionChoices）—— 它只把新状态发出去，摆什么由 MainWindow
    /// 再 `setChips()` 回来。
    bool showSnapshots() const;
    /// 摆上/摘下那个勾。**不发信号**（和 setChips 同一条规矩：开页那一下是"调用方在摆
    /// 初始值"，不是"用户拨了"）。
    void setShowSnapshots(bool showSnapshots);

    /// 实例文件页打开模组详情时显示头卡的「模组设置」，入口通往该实例的配置管理。
    void setModSettingsSlot(bool show);
    /// 当前页面是否提供「模组设置」。
    bool modSettingsSlotShown() const { return m_modSettingsShown; }

    /// 版本列表那一块。**空表 = 摆一句「这个资源一个版本都没发过」**。
    void setGroups(const QList<ResourceDetail::Group> &groups);

    /// 夹在**游戏版本 chip 那一排**和**版本列表**中间的那一行小字。
    ///
    /// 传空串 = 这一行**整个收起来**（不占高度、也不留一条空白的缝）—— 没有前置的模组、
    /// 以及"这一页不是模组"的那些情况下都是这样。
    ///
    /// NOTE: 那句话**不是这一层写的**：它由 MainWindow 从 `ModDependencies` 取（那才是
    /// "界面上的字"的唯一出处，这一层只负责摆）。和 `setPlaceholder()` 同一个形状。
    void setDependencies(const QString &text);
    /// 这一行现在写的是什么（收着的时候是空串）。验证工程读它。
    QString dependenciesText() const;

    /// 列表里摆一句说明（「正在读版本列表…」/「没取到，%1」）。
    /// 传空串 = 把列表清空（什么都不摆）。
    void setPlaceholder(const QString &text);
    /// 现在列表上摆着的那句说明（没摆 = 空串）。
    QString placeholderText() const;
    /// 版本列表还在路上：清空列表，盖上整块加载态（LoadingState）。placeholderText() 返回 text。
    void setLoading(const QString &text);
    /// 没取到：整块出错态（原因 + 「重试」）。placeholderText() 返回 reason。
    void setFailed(const QString &reason, std::function<void()> retry);

    // ---------- 名字（验证工程按这些 objectName 找控件，别在别处再写一遍字面量）----------
    static QString headerCardName();
    static QString iconLabelName();
    static QString titleLabelName();
    static QString descriptionLabelName();
    static QString metaLabelName();
    static QString openButtonName();
    static QString mcmodButtonName();
    static QString copyNameButtonName();
    static QString chipRowName();
    /// chip 那颗按钮的 objectName 是「前缀 + 下标」（0 起）。
    static QString chipButtonNamePrefix();
    static QString showSnapshotsCheckBoxName();
    /// 右上角「模组设置」入口（见 setModSettingsSlot；默认收起）。
    static QString modSettingsButtonName();
    /// 那四个字。**唯一出处** —— 按钮和验证工程都读它，别在别处再写一遍。
    static QString modSettingsText();
    /// chip 那一排和版本列表中间那一行（见 setDependencies）。
    static QString dependenciesLabelName();
    static QString versionListName();

    /// 版本行上那一条身份（`Row::key`）挂的**角色号**。
    ///
    /// NOTE: 这一页的树是它自己的（不是 MainWindow 那几张表），所以 `Qt::UserRole` 在这儿
    /// 没被别人占着 —— 但读它的地方（验证工程、这一页自己的激活处理）必须都走这个
    /// 函数，别再写一遍字面量。
    static int rowKeyRole();

    // ---------- 验证工程用的读数（产品侧不用）----------
    QList<QPushButton *> chips() const;
    QTreeWidget *versionList() const;
    /// 组头的条数。**摆着说明时是 1** —— 那条说明自己是一条顶层条目（代理把顶层画成
    /// 粗体，和下载页那四张表里「没取到」那句是同一个形状），读它之前先看
    /// `placeholderText()` 是不是空。
    int groupCount() const;
    /// 组里版本行的总数（不含组头）。
    int versionRowCount() const;
    /// 第 @p index 颗 chip 上那行字（越界回空串）。**认身份用 key，这个是给人看的对照**。
    QString chipTextAt(int index) const;
    /// 第 @p index 颗 chip 的 key（越界回空串）。
    QString chipKeyAt(int index) const;
    /// 第 @p index 个组头那行字（越界回空串）。
    QString groupTitleAt(int index) const;
    /// 第 @p index 个组的**展开状态**（越界回 false）。
    bool groupExpandedAt(int index) const;
    /// 头卡上那颗按钮上现在写着什么（「转到 Modrinth」…；没摆 = 空串）。
    QString openButtonText() const;
    /// 右上角那颗「模组设置」（任何时候都拿得到 —— 它只是收着）。
    QPushButton *modSettingsButton() const { return m_modSettings; }
    /// 描述那一行**当下画出来的字**（截断之后的那一串）—— 验证工程拿它和全文比，量
    /// 「太长的简介真的被截了」。全文走 `descriptionToolTip()`。
    QString shownDescription() const;
    QString descriptionToolTip() const;

signals:
    /// 用户点了一颗 chip（参数是那颗的 **key**，空串 = 「全部」）。
    void chipChanged(const QString &gameVersion);
    /// 用户拨了「显示快照版」（点它、或者按 Alt 键都算 —— 见 keyPressEvent）。
    ///
    /// 收到这一发之后调用方要重摆 chip 那一排（`setChips`）再重算列表，**别只记下这一位**
    /// —— 那样界面看着像没反应（这一页不自己动手，见 showSnapshots() 那段）。
    void showSnapshotsChanged(bool showSnapshots);
    /// 头卡上那三颗按钮：要开浏览器 / 要复制名字。**都不由这一页自己动手**
    /// （开浏览器要能被验证工程拦，复制要能写日志 —— 两边都是 MainWindow 的事）。
    void openExternalRequested(const QString &url);
    void copyNameRequested(const QString &text);
    void modSettingsRequested();
    /// 用户点了某一个具体版本（`Row::key` 原样带回去）= 要下它。
    void rowActivated(const QString &key);

private:
    void rebuildChips();
    void applyChipChecked(const QString &key);
    void fillGroups(const QList<ResourceDetail::Group> &groups);
    void showPlaceholder(const QString &text);
    void onChipClicked(QPushButton *button);
    void onRowActivated(QTreeWidgetItem *item);

    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    /// 单独接**裸的 Alt**（`Qt::Key_Alt`，不带别的修饰键）= 拨一下「显示快照版」。
    ///
    /// 为什么一个勾选框还要配一个快捷键：那一排 chip 上有一颗是"整条线"，勾上/摘下看的
    /// 是同一件事的两种形状，手不用离开键盘。鼠标提示上写着这件事（见构造函数里那个
    /// setToolTip）。
    ///
    /// NOTE: 这儿**只认裸 Alt 那一发**（Ctrl/Shift/Meta 同按一律放过去），而且**必须
    /// 调一次基类** —— QDialog 的基类实现管着 Esc（和别的那几档）。
    void keyPressEvent(QKeyEvent *event) override;

    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_description = nullptr;
    QLabel *m_meta = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_mcmod = nullptr;
    QPushButton *m_copyName = nullptr;
    QWidget *m_chipRow = nullptr;
    /// chip 那一排和版本列表中间那一行（见 setDependencies）。**收着的时候不占高度**
    /// （setDependencies 传空串时整个 hide 掉）。
    QLabel *m_dependencies = nullptr;
    QCheckBox *m_showSnapshots = nullptr;
    /// 右上角「模组设置」入口，显示和启用均由 setModSettingsSlot 控制。
    QPushButton *m_modSettings = nullptr;
    /// 当前资源是否有实例配置入口。
    bool m_modSettingsShown = false;
    QTreeWidget *m_list = nullptr;

    ResourceDetail::Header m_header;
    QList<ResourceDetail::Chip> m_chips;
    QList<QPushButton *> m_chipButtons;
    QString m_currentChip;
    QString m_placeholder;
};

/// 版本行左边那个字母方块的**尺寸与配色**：`channel` 见 ResourceDetail::Row::channel。
///
/// 放在这儿（而不是 page 内部）是为了验证工程能直接量它的颜色 —— 整块方块的底色和
/// 那个字母都是我们自己定的，量得到。
namespace ResourceDetailMarks {
/// 方块边长（像素）。**和下载页那四张表的缩略图一样大**（`MainWindowUI::resourceTreeIconSize()`）
/// —— 两处都是"列表行前面那一格"，不一样大的话一眼看得出来。
int badgeSize();
/// 那一档的底色。认不出来的档给灰的（不是"不画"：那一行还是得有个方块，不然它会比
/// 同组其它行往左挪一截）。
QColor channelColor(int channel);
/// 方块里那个字母（`R` / `B` / `A`；认不出来的档回空串 = 不画字）。
QString channelLetter(int channel);
/// 字母的颜色（白）。
QColor channelLetterColor();
/// 画一个（给列表行的 DecorationRole）。
QPixmap channelBadge(int channel, int size);
}  // namespace ResourceDetailMarks

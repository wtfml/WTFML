/**
 * @file InstanceGroupDelegate.hpp
 * @brief 实例树代理：绘制分组头，以及实例行右侧的设置/置顶/删除按钮。
 *
 * 只有带 `groupIdRole()` 的行按组头绘制；其余行交给基类。行尾按钮不是控件，几何由本文件
 * 的 `...Rect()` / `...GlyphRect()` 统一给出，paint、eventFilter 和 helpEvent 共用同一份，
 * 避免点击区域与画出来的位置错位。
 *
 * 实例行（实例页）按 ListRowDelegate 画：40 封面 + 两行字 + 行尾元信息，悬停换成文件夹 / 更多 / 启动，
 * 点中由 MainWindow::eventFilter 认下后发 actionTriggered。预设页那棵树关掉行内操作，仍走旧的单行画法。
 */
#pragma once

#include "ListRowDelegate.hpp"

class QPainter;

/// 实例页那张树上挂的代理：只接管分组头那几行，实例行原样走基类。
class InstanceGroupDelegate : public ListRowDelegate {
    Q_OBJECT

public:
    explicit InstanceGroupDelegate(QObject *parent = nullptr);

    /// 组里那些行**行尾那三颗**（齿轮 / 置顶箭头 / 垃圾桶）画不画。默认画（实例页）。
    ///
    /// 预设页那棵树也挂这个代理（两处的组头、悬停底色、选中带子要和实例页逐字一样），
    /// 但那些动作长在右边那一栏的按钮上 —— 组里再挂一排，屏幕上会多出三格"点下去没有
    /// 去处"的图标，而 `MainWindow::eventFilter` 那边根本没接预设树。关掉之后正文的截断
    /// 边界跟着挪到行尾（见 paint()），不关的话名字会被一条看不见的空位压短。
    void setInlineRowActions(bool on) { m_inlineRowActions = on; }
    bool inlineRowActions() const { return m_inlineRowActions; }

    /// 行高。默认 `MainWindowUI::instanceTreeRowHeight()`（实例页那一档）。
    ///
    /// 挂在窄栏里的那棵树（预设页左栏 232 px）用不着 58 那么高 —— 一屏看不到三行就
    /// 得滚。矮的那一档按它自己给；把它留着可变而不是另写一套代理，是因为组头、悬停、
    /// 选中这一整套画法只该有一份。
    void setRowHeight(int height) { m_rowHeight = height; }
    int rowHeight() const;
    /// 实例页组头行高（设计稿 .group-label：小字标签，比实例行矮）。
    static constexpr int groupRowHeight() { return 36; }

    /// 这一行**画出来**的那一圈：整条行上下各收掉半个 `instanceTreeRowGap()`。
    ///
    /// 行与行之间那道空就是它留出来的 —— 底色、图标、文字全画在这一圈里，圈外那一带谁也不画，
    /// 露出来的是树自己的底色（见 `MainWindowUI::instanceTreeRowGap()` 那段）。
    ///
    /// NOTE: 收过的这一份**只用来画**（paint() 里 CE_ItemViewItem 那一层）。下面那些按整条行
    /// 算的几何 —— `gearRect()` / `chevronRect()` / `instanceXxxRect()` 那一整套 ——
    /// 读的仍然是**整条行**，`MainWindow::eventFilter` 认下那一下也仍然按整条行
    /// （`visualItemRect()`）。两者竖直方向**同一个中心**：那些式子在竖直方向一律是
    /// `top + (高 - 边长) / 2`，而上下各收一半之后中心不动
    /// （`row.top() + (H - s) / 2 == (row.top() + g/2) + (H - g - s) / 2`），
    /// 所以画出来的字形和命中格一格都不会错开。
    /// 反过来，把收过的那一份喂给它们（或者拿它去 `visualItemRect`）就会整体错开
    /// 半个缺口 —— 那一错的表现是"看着点中了齿轮、其实点的是那一行"，而那一行点一下
    /// 是进实例设置。
    /// NOTE: 只收**竖直**方向，左右一个像素都不动：行尾那三颗按钮是贴着行的右边缘排的
    /// （`instanceActionGlyphRects()` 里那句"用 `row.x() + row.width()` 当锚"），
    /// 横向一收整排就跟着飘。
    static QRect cardRect(const QRect &row);

    /// 行尾那颗**齿轮**的**命中格**占哪一格（`row` 是整行的矩形，视口坐标）。只有条目上
    /// `rowGroupGearRole()` 为真的行才画它、也只有那种行才有意义。
    ///
    /// NOTE: 形状和 `VersionTreeDelegate::clearRect()`（那个「取消」小叉）**逐字一样**：
    /// 命中格比画出来的字形大一圈（四周各留 3 px），位置是"紧挨着右边那个箭头
    /// 左边、中间留 6 px"。两处的差别只有边长来源（那边是 `versionTreeArrowSize()`，
    /// 这边是 `instanceGroupGearSize()`）。
    /// NOTE: 这一格**允许比一行高**（实例树的行高就是那一行字那么高）：它是"点得到的那一
    /// 圈"，不是画出来的那一圈。冒出去的那几点点不到别的东西 —— 认下那一下的是
    /// `MainWindow::eventFilter`，而它先要求 `itemAt()` 落在这条行里。
    static QRect gearRect(const QRect &row);

    /// 那颗齿轮**画出来**的那一圈（比 gearRect() 小：那个是命中格，四周各留 3 px 余量）。
    ///
    /// NOTE: 边长取 `min(instanceGroupGearSize(), 行高)`，保证装在行高里面。
    /// 不缩的话 16 px 的齿轮会上下各冒出去一半（整屏重绘压到相邻那两条上、
    /// 局部重绘被切成半颗）；行高是常数 `instanceTreeRowHeight()` = 58，
    /// 所以这一缩平时不生效，留着是兜底。
    static QRect gearGlyphRect(const QRect &row);

    /// 最右边那格**「展开 / 收起」文字**占哪一格（名字沿用旧称；体验反馈 8 之后不画箭头）。
    /// 分组头一定写它。宽 `versionTreeTrailWidth()`，高 12 px 竖着恒在行内（理由见 .cpp）。
    static QRect chevronRect(const QRect &row);

    /// 光标落在行里某颗按钮上就说那一句，没落在任何一个上就是空串。
    /// 组行：那颗齿轮（`rowGroupGearTipText()`）；实例行：悬停快捷操作文件夹 / 更多 / 启动
    /// （几何是 `ListRowDelegate::actionRect()`，文案从 MainWindowUI 取）。
    static QString rowActionTip(const QModelIndex &index, const QRect &row, const QPoint &pos);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    /// 悬停提示。基类那个只看 `Qt::ToolTipRole`（整条行一句），这里先问一声 rowActionTip()，
    /// 问不到才交给基类。
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

protected:
    /// 只有实例页的实例行交给 ListRowDelegate（悬停快捷操作）；组行和预设树走基类。
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

private:
    bool m_inlineRowActions = true;
    /// 0 = 走 `MainWindowUI::instanceTreeRowHeight()`（实例页那一档）。
    int m_rowHeight = 0;

    /// 实例行那三颗按钮的**字形**格子，从右往左**一次算完**（右起：齿轮 / 上箭头 / 垃圾桶）。
    ///
    /// 三个 `instanceXxxGlyphRect()` 和三个 `instanceXxxRect()` 都是它的薄壳（前者原样、
    /// 后者四周各加 `instanceRowActionMargin()`）—— 这就是上面那段"一次算完"说的那一处。
    ///
    /// NOTE: 右边缘取 `row.x() + row.width()`、**不用 `row.right()`** —— 后者等于
    /// `left + width - 1`，用它当锚整排会往左挪一个像素，而这个"差一个像素"正好是
    /// 调用方（`eventFilter`）与代理对不齐的那种错。
    static void instanceActionGlyphRects(const QRect &row, QRect &gear, QRect &pin, QRect &trash);
};

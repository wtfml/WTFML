#include "InstanceGroupDelegate.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include "Popups.hpp"
#include <QTreeView>

#include "MainWindowUI.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

InstanceGroupDelegate::InstanceGroupDelegate(QObject *parent) : ListRowDelegate(parent) {
    // 选中带只有卡片那一圈：告诉样式别涂左边那截分支区（见 Theme.cpp 里 PE_PanelItemViewRow 那段）。
    if (auto *view = qobject_cast<QWidget *>(parent)) view->setProperty("wtfmlCardSelection", true);
}

int InstanceGroupDelegate::rowHeight() const {
    return m_rowHeight > 0 ? m_rowHeight : MainWindowUI::instanceTreeRowHeight();
}

QRect InstanceGroupDelegate::chevronRect(const QRect &row) {
    const int pad = MainWindowUI::versionTreeRowPadding();
    // 边长按这一行的高度收一收。不缩的话，行高变矮时 12 px 的箭头会上下各冒出去
    // 一条被切掉；行高是 `instanceTreeRowHeight()` = 58 ——
    // 所以这一缩**平时不生效**，留着是兜底。
    const int arrowSize = qMin(MainWindowUI::versionTreeArrowSize(), row.height());
    // 这一格现在写「展开 / 收起」（体验反馈 8：不画箭头），宽取 versionTreeTrailWidth()，
    // 高仍按上面那档收。贴着行右边缘（和 VersionTreeDelegate 行尾那格同一个式子）。
    const int trail = MainWindowUI::versionTreeTrailWidth();
    //
    // NOTE: 竖直位置走 `top + (行高 - 边长) / 2`，**不用 `row.center().y()`** —— 后者走的是
    // QRect::center()（= top + (h-1)/2），行高是**偶数**时分母那一半少一像素：行高
    // 和边长一样时（就是上面那一档）那个箭头会往上冒一个像素出去，整屏重绘压到上面
    // 那条行、局部重绘被切掉一条。这个式子在"边长 ≤ 行高"时恒在行内。
    return QRect(row.right() - pad - trail + 1, row.top() + (row.height() - arrowSize) / 2,
                 trail, arrowSize);
}

QRect InstanceGroupDelegate::cardRect(const QRect &row) {
    // 一道空上下各分一半。缺口是**奇数**的时候多出来的那 1 px 落在底下 —— 挑哪边都行，
    // 写死一边是为了"同一个行矩形永远算出同一个卡片矩形"（随机取整会让量像素的断言
    // 两趟之间差一像素）。
    const int gap = MainWindowUI::instanceTreeRowGap();
    if (gap <= 0) {
        return row;
    }
    const int top = gap / 2;
    return row.adjusted(0, top, 0, -(gap - top));
}

QRect InstanceGroupDelegate::gearRect(const QRect &row) {
    // 命中格比画出来的那个齿轮大一圈（四周各留 3 px，和 clearRect/refreshRect 一个做法）——
    // 16 px 的点击目标太小了，手一抖就点到那一行上（而那一下是"展开/收起"）。
    const int box = MainWindowUI::instanceGroupGearSize() + 6;
    // 紧挨着箭头左边，中间留 6 px（"文字和箭头之间"也是这个数，看着才是一套的）。
    const int left = chevronRect(row).left() - 6 - box;
    // 竖直居中：和 chevronRect() 同一个式子（理由见那边）。**不按行高收** —— 这一格是
    // 命中格，比一行高是允许的（点得到的那一圈，不是画出来的那一圈，见 .hpp）。
    return QRect(left, row.top() + (row.height() - box) / 2, box, box);
}

QRect InstanceGroupDelegate::gearGlyphRect(const QRect &row) {
    const QRect box = gearRect(row);
    // 画出来的那一圈**必须装在行高里**（MainWindowUI::instanceGroupGearSize() 那段写的正是
    // 这件事）：不缩的话，一行只有十几像素时它上下各冒出去一半，屏幕上就是"齿轮被切掉
    // 上下两条"。缩法是**从命中格里对称地往回收** —— 行够高时收到的正好是那 3 px 的余量
    // （和 `adjusted(3, 3, -3, -3)` 逐像素一样），行矮时再多收一点，收到边长等于行高为止。
    const int size = qMin(MainWindowUI::instanceGroupGearSize(), row.height());
    const int inset = (box.width() - size) / 2;
    return box.adjusted(inset, inset, -inset, -inset);
}

QString InstanceGroupDelegate::rowActionTip(const QModelIndex &index, const QRect &row,
                                            const QPoint &pos) {
    // 组行那一半：这一行**只剩齿轮**一格（上下移那两颗在分组设置窗里，见 .hpp）。
    if (index.data(MainWindowUI::rowGroupGearRole()).toBool() && gearRect(row).contains(pos)) {
        return MainWindowUI::rowGroupGearTipText();
    }
    // 实例行那一半：悬停快捷操作三格（几何同 ListRowDelegate::actionRect）。
    if (!index.data(MainWindowUI::groupIdRole()).isValid()) {
        if (actionRect(row, Action::Folder).contains(pos)) return MainWindowUI::rowOpenFolderTipText();
        if (actionRect(row, Action::More).contains(pos)) return MainWindowUI::rowMoreTipText();
        if (actionRect(row, Action::Launch).contains(pos)) return MainWindowUI::rowLaunchTipText();
    }
    return QString();
}

bool InstanceGroupDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view,
                                      const QStyleOptionViewItem &option, const QModelIndex &index) {
    // 只在"光标真落在那颗齿轮那一格上"时说话（和 VersionTreeDelegate 那个箭头/叉一个规矩），
    // 别的地方一句都不弹。
    // 没画那一排的树（`setInlineRowActions(false)`，预设页）一句都不说 —— 那些格子在上面
    // 根本没画，在这儿说话等于"指着一块空白弹出了「进入实例设置」"。组头那一格（齿轮）
    // 仍然照说。
    if (m_inlineRowActions && event != nullptr && event->type() == QEvent::ToolTip) {
        const QString tip = rowActionTip(index, option.rect, event->pos());
        if (!tip.isEmpty()) {
            Popups::showTooltip(tip, event->globalPos(), view);
            return true;
        }
        // 实例名长到收了省略号：指着这一行给全名。
        if (!index.data(MainWindowUI::groupIdRole()).isValid() && titleElided(index)) {
            Popups::showTooltip(index.data(Qt::DisplayRole).toString(), event->globalPos(), view);
            return true;
        }
    }
    // 兜底：条目自己写 `Qt::ToolTipRole` 的老路（基类就干这个）。
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

void InstanceGroupDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const {
    // 实例那几行（没有组 id 那个 role 的）：背景/选中/文字/图标**仍然交给基类画**，
    // 这一层只在它画完之后，在行尾补上那三颗按钮（右起：齿轮 / 上箭头 / 垃圾桶，见文件头）。
    if (!index.data(MainWindowUI::groupIdRole()).isValid() && m_inlineRowActions) {
        ListRowDelegate::paint(painter, option, index);  // 实例页：设计稿列表行
        return;
    }
    if (!index.data(MainWindowUI::groupIdRole()).isValid()) {
        QStyleOptionViewItem opt(option);
        // 悬停那一位**先读走、再抹掉**：它是给"下面我们自己画的那两颗悬停按钮"用的，
        // 本来就不是"这一行被指到了"这件事本身。
        // 抹掉它**本来就不是**为了挡悬停底色 —— 那一层在这个仓库里一直是**我们自己画**才有的
        // （见下面那段 fillRect）。核过 Qt 源码：`QCommonStyle::drawPrimitive(
        // PE_PanelItemViewItem)` 只认 `backgroundBrush` 和 `State_Selected`，从不看
        // `State_MouseOver`；Fusion 没覆写它、`Theme::createStyle()` 也没碰它。
        // 抹掉这一位仍然要做：别让将来的样式插一层我们没要的底色，把下面那层盖掉。
        // 读的是**原样传进来**的那个 option：这一位是视图填的，它在 initStyleOption 里
        // 留没留住不该由我们猜（version 树那边也特意读了传进来那一份）。
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        // NOTE: 除 State_MouseOver 之外**别的 state 一个都不许动** —— 这一行还要靠样式
        // 画选中底色和文字/图标（选中那一位就在这个 state 里）。
        opt.state &= ~QStyle::State_MouseOver;
        // 正文不能交给样式按**整格宽度**摆 —— 实例页这一列是**拉伸**的（表头藏着、只有一列，
        // stretchLastSection 为真、columnWidth == 视口宽），sizeHint() 里"加了三颗按钮
        // 的宽度"那一段**根本不参与布局**。于是把一个实例改名成一长串之后，名字一路画到
        // 格子右边缘、被行尾常显的那颗齿轮压掉最后两三个字和省略号（悬停时还会多压上
        // 上箭头/垃圾桶两颗）—— 而那正是 sizeHint() 里那段注释说要防的事。
        //
        // 做法：**先自己 initStyleOption**（不能只改传进来那一份：基类 paint 会自己再
        // initStyleOption 一次，我们写进去的正文会被它覆盖掉），把正文换成**截断过**的
        // 那一份，然后照 VersionTreeDelegate 那一套直接 drawControl —— 背景、选中、
        // 图标、焦点框仍然全归样式画，只是文字短了、自然停在三颗按钮左边。
        // 文字区的宽度**问样式要**（SE_ItemViewItemText），不去猜它给图标和留白各让了
        // 多少 —— 猜的话换一个样式就错一截。
        initStyleOption(&opt, index);
        // 实例是组的**子条目**，视图按 `setIndentation(14)` 在这一行的左边留出一截。
        // 而 `QTreeView::drawRow()` 在喊代理之前，会先用**选中色**把那整截涂满
        // （`PE_PanelItemViewRow`，矩形是 `QRect(position, y, indentation, height)` ——
        // 注意是**整条行高**、不是卡片那一圈）。代理画卡片时上下各收掉半个缺口
        // （见 cardRect()），于是选中一行时左边就多出一根 14 px 宽、比卡片上下各冒出去
        // 3 px 的竖条 —— 那一截要擦掉。
        //
        // 代理在这条链里**排在视图后面**（drawRow 是"先涂分支区、后
        // `itemDelegateForIndex(...)->paint(...)`"），所以盖得住：拿树自己的底色
        // （`QPalette::Base` —— 视口的 backgroundRole 就是它，见 QAbstractItemView
        // 构造里那句 `viewport->setBackgroundRole(QPalette::Base)`）把那一截补回来。
        // `PE_PanelItemViewRow` 在"没选中"时**什么都不画**（底色是视口自己铺的），
        // 所以不能拿它当橡皮 —— 只能自己 fill 一遍。
        //
        // 宽度取的是 `option.rect.left()`，**不是**写死的 14：它就是"这一格从哪儿开始"
        // （= 分支区宽度）。横向滚动时它和视图那边**同时**变小（两边都以
        // `columnViewportPosition(0)` 为原点），滚到那一截整条出屏时它 ≤ 0、
        // 这一句自己什么都不画。写死 14 的话，往右滚一下就会在左边留下一根擦不掉的条。
        // 竖直方向用 `option.rect`（**整条行**）而不是卡片：要擦掉的那两条恰恰长在
        // 卡片上下**外面**。
        // 7a：这一截改由样式那边**不涂**（视图挂着 `wtfmlCardSelection`，构造时设的），
        // 这里不再拿底色擦 —— 半透明的底擦不掉半透明的选中色，只会再叠一层。
        // 选中那一行的**墨**：底已经由样式画成那块浅灰（`#cccccc`，覆写在
        // `Theme::createStyle()` 那一份代理样式里），而这一行上手画的那三颗（齿轮 / 置顶箭头 /
        // 垃圾桶）取的是正文色 —— 不换的话深色主题下它们就是一对白疙瘩压在浅灰上。正文那一半
        // 归样式管（走 `CE_ItemViewItem`）、那边也换过
        // 了同一支墨，两处必须一致。
        // NOTE: 换的是**这一份局部拷贝**，控件的调色板一点都不动。
        // NOTE: **只换墨、不换那一块的形状** —— 实例行选中时的带子只有卡片那一圈，
        // 左边那一截分支区要保持底色（见上面那条 ★★ ），
        // 所以这儿不去补涂它（版本树那一支反过来，它是整条行）。
        // 行与行之间那道空（见 cardRect()）：这一行**画**的那一圈比整条行上下各小半个缺口。
        // 摆在这儿（initStyleOption 之后、量文字之前）是因为下面那句 SE_ItemViewItemText
        // 是从 opt.rect 上算的 —— 少了这一句，量出来的宽度还是整条行的，虽然宽度只跟左右有关、
        // 结果一样，但"画在哪一圈"和"按哪一圈量"从此是两件事，读的人得自己核对一遍。
        opt.rect = cardRect(option.rect);
        // 悬停那一行的加深底色。画在 `CE_ItemViewItem` **之前**：选中那一行是样式紧接着用
        // **不透明的** `Highlight` 铺满这一圈的，所以"选中"天然压得住"悬停"，不用互斥判断。
        // 底色走 `Theme::rowHoverFill()`（唯一出处见 ui/Theme.hpp）：从**这一层自己的底色**
        // 出发往主题色混一档 ⇒ 浅色主题下和白底按钮的 `:hover` 逐字节同一个色。
        // 铺的是 `opt.rect`（= 上面那句收过缺口之后的**卡片**），不是 `option.rect`：
        // 这一行选中时的带子就是卡片这一圈（左边那截分支区在上面那句 fillRect 里被补回底色了），
        // 悬停也得是同一个形状 —— 铺整条行的话，悬停带会比选中带左右各宽出去一截。
        // 不可用的那几行不染：这张树上它们整体是半透明的（`setDisabled()` 那条路），
        // 再铺一层底色反而像是"点得动"。
        if (hovered && (index.flags() & Qt::ItemIsEnabled) != 0) {
            painter->fillRect(opt.rect, Theme::rowHoverFill(option.palette.color(QPalette::Base)));
        }
        if (QStyle *style = opt.widget != nullptr ? opt.widget->style() : QApplication::style()) {
            // 停在哪：最左那颗按钮**命中格**的左边（它已经比字形大一圈，天然留出一点空隙）。
            // 三颗无论此刻画不画都算进去 —— 和 sizeHint() 那条规矩一样（跟着悬停变的话，
            // 鼠标一扫过去正文就左右跳一下）。
            // 不画那一排的那种树（`setInlineRowActions(false)`，见 .hpp）停在行尾留白之前。
            const int stopAt = option.rect.x() + option.rect.width() - MainWindowUI::versionTreeRowPadding();
            const QRect textArea =
                style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
            const int avail = qMin(textArea.width(), stopAt - textArea.left());
            if (avail > 0) {
                const QFontMetrics metrics(opt.font);
                opt.text = metrics.elidedText(opt.text, Qt::ElideRight, avail);
            }
            style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        }

        // 三颗都用**正文色**（和实例页文件夹行那颗齿轮一个取色口径）：它们是行里的动作，
        // 但这一行的正文就是实例名，动作和正文同色才不显得"行尾挂了三个别的东西"。
        //
        // 不画那一排的树（`setInlineRowActions(false)`，见 .hpp 的预设页）到这儿就结束了：
        // 底色、选中、正文、图标已经全部画完，行尾干干净净。
        return;
    }

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    // 背景/选中/悬停那一层交给样式画，但文字先清掉 —— 位置由下面自己摆（左边留白、
    // 右边要给齿轮和箭头让出两格）。features 清空之后 CE_ItemViewItem 就只画背景那一层。
    opt.text.clear();
    opt.icon = QIcon();
    opt.features = QStyleOptionViewItem::None;
    // 组行同样收掉那道空（见 cardRect()）。这一句改不动屏幕上的东西 —— 组行没有自己的底色，
    // 露出来的那一带和行本身的底色是同一个（**悬停**那层带子除外，见下面那段）；
    // 留着是为了"这张树里每一行都是同一种画法"，将来谁给行加了底色不会只加一半。
    opt.rect = cardRect(option.rect);
    // 组行也要染悬停那一层，判据和实例行那一支逐字一样，色也是同一个
    // `Theme::rowHoverFill()` —— 组行和实例行的悬停是同一件事。
    // 两支都清 `State_MouseOver`、都由我们自己画：
    // 把这一位交给样式等于把"悬停长什么样"押在"样式永远不画"上，而那个假设没有保证。
    // 这里读的是**传进来**的那一份（和实例行那支同一个理由）：那一位是视图填的。
    // 组行**不可选中**，所以不存在"和选中带打架"的问题 —— 建那一行的
    // `src/MainWindow.cpp` 里有一句 `setFlags(flags() & ~Qt::ItemIsSelectable)`
    // （组头是"一堆东西的标题"，不是"一个能启动的实例"）。这是**唯一**一处放这一位
    // 的地方，本文件不碰 `flags()`；这条路径上"能不能点得动"仍然只看 `ItemIsEnabled`。
    // 这一位下面还要用两次（悬停底色 + 行尾那颗齿轮，见下面那两段），所以在这儿
    // 读成一个名字 —— 读的仍然是**传进来**那一份，时序和上面那句一模一样。
    const bool rowHovered = (option.state & QStyle::State_MouseOver) != 0;
    if (!m_inlineRowActions && rowHovered && (index.flags() & Qt::ItemIsEnabled) != 0) {
        painter->fillRect(opt.rect, Theme::rowHoverFill(option.palette.color(QPalette::Base)));
    }
    opt.state &= ~QStyle::State_MouseOver;
    if (QStyle *style = opt.widget != nullptr ? opt.widget->style() : QApplication::style()) {
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
    }

    const QRect row = option.rect;
    const QPalette &palette = option.palette;
    const int pad = MainWindowUI::versionTreeRowPadding();
    const bool hasGear = index.data(MainWindowUI::rowGroupGearRole()).toBool();
    // 展开着写「收起」、收着写「展开」。状态只有视图知道（`isExpanded`）——
    // 和 VersionTreeDelegate 一样从 option.widget 上问，问不到就当收着。
    const auto *view = qobject_cast<const QTreeView *>(option.widget);
    const bool expanded = view != nullptr && view->isExpanded(index);

    // 右边那几格从文字的可用宽度里扣掉 —— 不扣的话长组名会从齿轮底下穿过去。
    int textRight = chevronRect(row).left() - 6;
    if (hasGear) {
        textRight -= gearRect(row).width() + 6;
    }
    const int textLeft = row.left() + pad;
    const QRect textRect(textLeft, row.top(), qMax(0, textRight - textLeft), row.height());

    // 组名是**粗体**：它是一堆实例的标题，和底下的实例名不是一回事（版本树那几条分类
    // 也是这么画的）。
    QFont mainFont = option.font;
    mainFont.setBold(true);
    QColor groupInk = palette.color(QPalette::WindowText);
    if (m_inlineRowActions) {  // 实例页组头 = 设计稿 .group-label：辅助档 13 半粗、次要色（tokens-r2 分组头）
        mainFont = Theme::textFont(option.font, Theme::TextRole::Caption);
        mainFont.setWeight(QFont::DemiBold);
        groupInk = Theme::tokens().secondary;
    }
    const QFontMetrics mainFontMetrics(mainFont);
    painter->save();
    painter->setFont(mainFont);
    painter->setPen(groupInk);
    // 组名太长时截成省略号（和 VersionTreeDelegate 里那两行同一个理由 ——
    // `drawText(rect, 文字)` 装不下是硬裁，最后一个字被切掉半个）。组名是用户自己起的名字，
    // 比版本号更容易长过这一格。
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                      mainFontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight, textRect.width()));
    // 行尾那颗齿轮：画的是 gearGlyphRect()（比命中格小一圈、还要装进行高里），用次要那一档
    // 颜色 —— 它是行里的一个动作，不是正文。
    //
    // 它和实例行那几颗一样**只在鼠标停在这一行、或键盘走到这一行时才画**
    // （`Theme::isKeyboardCurrentRow`）。
    // · **宽度照扣**（上面 `textRight` 那一段没跟着改）：鼠标挪进来的时候正文不会左右
    // 跳一下 —— 和下载页那格刷新圈是同一条规矩（`hasRefresh` / `showRefresh` 拆开）。
    // · hovered 是**视图**填的 ⇒ 实例列表必须有 `setMouseTracking(true)`（MainWindowUI
    // 里那一句，少它的话这颗齿轮再也不出现，且不报错、断言也不一定先红）。
    // · 不可用的那几行（`setDisabled()` 那条路）不画：那一整行是半透明的，画上去像是
    // "点得动"。判据和上面那层悬停底色逐字一样。
    if (hasGear && (index.flags() & Qt::ItemIsEnabled) != 0 &&
        (rowHovered || Theme::isKeyboardCurrentRow(option, index))) {
        const QRect glyph = gearGlyphRect(row);
        painter->drawPixmap(glyph.topLeft(),
                            VersionIcons::gear(glyph.width(),
                                               Theme::tokens().primary));
    }
    // 最右边那格「展开 / 收起」：分组头**一定**写（整行点一下就是打开/关闭）。次要色、
    // 辅助档 13 —— 和版本树行尾那格同一条规矩（tokens-r2 §6）。
    painter->setFont(Theme::textFont(option.font, Theme::TextRole::Caption));
    painter->setPen(Theme::tokens().secondary);
    const QRect trailBox = chevronRect(row);
    painter->drawText(QRect(trailBox.left(), row.top(), trailBox.width(), row.height()),
                      Qt::AlignRight | Qt::AlignVCenter, expanded ? tr("收起") : tr("展开"));
    painter->restore();
}

QSize InstanceGroupDelegate::sizeHint(const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const {
    const bool isGroup = index.data(MainWindowUI::groupIdRole()).isValid();
    QStyleOptionViewItem opt(option);
    if (isGroup) {
        QFont bold = option.font;
        bold.setBold(true);
        opt.font = bold;
    }
    const QSize size = QStyledItemDelegate::sizeHint(opt, index);
    if (!isGroup && m_inlineRowActions) {
        return QSize(size.width(), ListRowDelegate::rowHeight());
    }
    if (!isGroup) {
        return QSize(size.width() + MainWindowUI::versionTreeRowPadding(), rowHeight());
    }
    int extra = MainWindowUI::versionTreeTrailWidth() + 6;
    if (index.data(MainWindowUI::rowGroupGearRole()).toBool()) {
        extra += MainWindowUI::instanceGroupGearSize() + 6 + 6;
    }
    return QSize(size.width() + extra, m_inlineRowActions ? groupRowHeight() : rowHeight());
}

bool InstanceGroupDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                        const QStyleOptionViewItem &option, const QModelIndex &index) {
    if (m_inlineRowActions && !index.data(MainWindowUI::groupIdRole()).isValid())
        return ListRowDelegate::editorEvent(event, model, option, index);
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

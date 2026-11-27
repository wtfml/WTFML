#include "VersionTreeDelegate.hpp"
#include "LoadingState.hpp"
#include "Popups.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QTreeView>

#include "MainWindowUI.hpp"
#include "MotdText.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

VersionTreeDelegate::VersionTreeDelegate(QObject *parent, bool leafArrow, int iconSize,
                                         bool navRow)
    : QStyledItemDelegate(parent), m_leafArrow(leafArrow), m_iconSize(iconSize), m_navRow(navRow),
      m_leafVerb(tr("安装")) {
}

int VersionTreeDelegate::resourceRowHeight() { return MainWindowUI::versionTreeRowHeight() + 8; }

int VersionTreeDelegate::rowIconSize() const {
    // 0 = 没传 = 用默认那一档（`MainWindowUI::versionTreeIconSize()`）。
    return m_iconSize > 0 ? m_iconSize : MainWindowUI::versionTreeIconSize();
}

QRect VersionTreeDelegate::clearRect(const QRect &row) {
    const int pad = MainWindowUI::versionTreeRowPadding();
    const int arrowSize = MainWindowUI::versionTreeArrowSize();
    // 命中格比画出来的那个叉大一圈（x 的边长是 arrowSize，格子里四周各留 3 px）——
    // 12 px 的点击目标太小了，手一抖就点到旁边的箭头或者那一行上。
    const int box = arrowSize + 6;
    // 它紧挨着行尾那格文字左边，中间留 6 px（"文字和行尾之间"也是这个数，看着才是一套的）。
    const int left = row.right() - pad - MainWindowUI::versionTreeTrailWidth() - 6 - box;
    return QRect(left, row.center().y() - box / 2, box, box);
}

QRect VersionTreeDelegate::refreshRect(const QRect &row) {
    const int pad = MainWindowUI::versionTreeRowPadding();
    // 画出来的那个圈和**正文前面的图标一样大**（versionTreeIconSize，18 px），命中格比它
    // 大一圈（四周各留 3 px，和 clearRect 同一个做法）—— 18 px 的点击目标太小了，
    // 手一抖就点到那一行上（而那一下是"换分类"）。
    const int box = MainWindowUI::versionTreeIconSize() + 6;
    // 贴着行右边缘。QRect 的 right() 是"最后一个像素的 x"，所以右边留出 pad 那几格之后，
    // 起点要 +1 才让这一格的右边缘正好落在 row.right() - pad 上。
    return QRect(row.right() - pad - box + 1, row.center().y() - box / 2, box, box);
}

QRect VersionTreeDelegate::arrowRect(const QRect &row) {
    const int pad = MainWindowUI::versionTreeRowPadding();
    const int trail = MainWindowUI::versionTreeTrailWidth();
    // 命中格比画出来的那格文字四周各大 3 px（和 clearRect / refreshRect 一个做法）。
    const int box = MainWindowUI::versionTreeArrowSize() + 6;
    // 画出来的是贴右边缘留 pad 的那一块（`row.right() - pad - trail + 1` 起、宽 trail）。
    return QRect(row.right() - pad - trail + 1 - 3, row.center().y() - box / 2, trail + 6, box);
}

QString VersionTreeDelegate::rowActionTip(const QModelIndex &index, const QRect &row,
                                          const QPoint &pos) {
    // 一条一条来：每加一个"行里的按钮"，在这儿加一行（几何仍然在 clearRect/refreshRect 里）。
    // 顺序就是"谁压在谁上面"的顺序 —— 今天两个角色互斥（一行不能同时挂），顺序无所谓，
    // 将来真叠上了，先判的那个赢。
    if (index.data(MainWindowUI::rowRefreshRole()).toBool() && refreshRect(row).contains(pos)) {
        return MainWindowUI::rowRefreshTipText();
    }
    if (index.data(MainWindowUI::rowClearRole()).toBool() && clearRect(row).contains(pos)) {
        return MainWindowUI::rowClearTipText();
    }
    return QString();
}

bool VersionTreeDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view,
                                    const QStyleOptionViewItem &option, const QModelIndex &index) {
    // 只在"光标真落在行里那个按钮那一格上"时说话，别的地方一句都不弹（包括那一行的正文）。
    if (event != nullptr && event->type() == QEvent::ToolTip) {
        const QString tip = rowActionTip(index, option.rect, event->pos());
        if (!tip.isEmpty()) {
            Popups::showTooltip(tip, event->globalPos(), view);  // 系统黄底气泡不出
            return true;
        }
    }
    // 兜底：条目自己写 `Qt::ToolTipRole` 的那条老路（基类就干这个）。这条对"整条行一句"
    // 仍然有意义（比如将来资源表上"这一行是什么"），所以不能整个吃掉。
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

void VersionTreeDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                const QModelIndex &index) const {
    // 整行是控件（加载器的「显示全部 / 收起」）：不画兜底图标和底色。
    if (const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        view != nullptr && view->indexWidget(index) != nullptr) {
        return;
    }
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    // 背景/选中/悬停那一层交给样式画，但**文字和图标先清掉** —— 这一行是"两行字 + 一个小图标"，
    // 样式按"一行文字一个图标"摆出来的那个位置不能用（版本号会在行正中间，下面那行小字
    // 根本没地方放）。features 清空之后 CE_ItemViewItem 就只画背景那一层了。
    opt.text.clear();
    opt.icon = QIcon();
    opt.features = QStyleOptionViewItem::None;
    // 悬停那一位**先读走、再抹掉**：它是给"这一行上我们自己画的那个按钮"用的（见下面那段
    // showRefresh），不是"这一行被指到了"这件事本身。
    // 抹掉它**不是**为了挡悬停底色 —— 那一层在仓库里是**我们自己画**才有的（就是下面那段
    // fillRect）。核过 Qt 源码：`QCommonStyle::drawPrimitive(PE_PanelItemViewItem)` 只认
    // `backgroundBrush` 和 `State_Selected`，**从不看 `State_MouseOver`**；Fusion 没有
    // 覆写它，而我们的 `Theme::createStyle()` 也只重写了 `PE_IndicatorArrowDown` 一个原语。
    // 悬停加深本身是这条规矩的一部分（版本与按钮上都要有效果），只是不靠这一位来画。
    // 这一位仍然要抹掉：别让**将来**某个样式插一层我们没要的底色，把我们画的那层盖掉。
    // 读的是**原样传进来**的那个 option，不是 initStyleOption 之后的那一份：这一位是视图
    // 填的，它在 initStyleOption 里留没留住不该由我们猜（这条路上我们自己说了算）。
    const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
    // 选中那一块由**样式**画成"这一行自己的底色混一档主题色"（与侧栏导航同款 —— 两条
    // 原语覆写在 `Theme::createStyle()` 那一份代理样式里，见 `ui/Theme.hpp` 的
    // `selectionFill()` 那段）。这一位在这儿用来给下面那一份**局部调色板**换墨。
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    opt.state &= ~QStyle::State_MouseOver;
    // 侧栏导航的选中底由下面那支自己画成「内缩圆角块」。这里先把 Selected 从交给
    // 平台样式的副本里拿掉，否则 Fusion 会先铺一块顶到四边的灰底，圆角外面会露出
    // 四个灰色直角。普通数据列表仍然保留平台样式负责的那块选中底。
    if (m_navRow) {
        opt.state &= ~QStyle::State_Selected;
    }
    // 悬停那一行的加深底色（见上）。
    //
    // 这一层**画在 `CE_ItemViewItem` 之前**是有意的：选中那一行是样式紧接着用**不透明的**
    // `Highlight` 铺满整格的，所以"选中"天然压得住"悬停"，两者不需要互斥判断（下面那句
    // 挪到之后去画的话，悬停就会反压选中 —— 那才是错的）。
    // 底色走 `Theme::rowHoverFill()`（唯一出处在 ui/Theme.hpp 里那段说明）：它从**这一层
    // 自己的底色**出发往主题色混一档，于是浅色主题下和白底按钮的 `:hover` 逐字节同一个色，
    // 换成深色预设也不会糊成一片近白（"按硬编码白去算"的写法在 `Base = #323232` 上会炸）。
    // 不可用的那几行不染（判据和下面 `rowOpacity` 那条逐字一样）：那一档整行淡到 0.5，
    // 再铺一层底色反而像是"点得动"。在这儿**内联**写一遍是因为 `rowEnabled` 要到下面才
    // 算得出来，而这一层必须赶在样式画底色之前。
    const bool hoverable = hovered && (index.flags() & Qt::ItemIsEnabled) != 0;
    const auto *treeView = qobject_cast<const QTreeView *>(option.widget);
    if (m_resourceRows && treeView != nullptr && treeView->model() != nullptr) {
        // 展开的那一组（顶层那条 + 它底下露出来的行）铺一块圆角淡底：首行圆上角、末行圆下角。
        const QAbstractItemModel *model = treeView->model();
        QModelIndex top = index;
        while (top.parent().isValid()) top = top.parent();
        if (treeView->isExpanded(top) && model->rowCount(top) > 0) {
            bool last = !(treeView->isExpanded(index) && model->rowCount(index) > 0);
            for (QModelIndex i = index; last && i.parent().isValid(); i = i.parent())
                last = i.row() == model->rowCount(i.parent()) - 1;
            const bool first = !index.parent().isValid();
            const QRect band(0, option.rect.top(), option.rect.right() + 1, option.rect.height());
            const qreal r = Theme::controlRadius();
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setClipRect(band);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::tokens().subtle);
            painter->drawRoundedRect(QRectF(band).adjusted(0, first ? 1 : -2 * r, 0, last ? -1 : 2 * r), r, r);
            painter->restore();
        }
    }
    if (hoverable && m_resourceRows) {
        // 搜索结果那一档：悬停是内缩圆角块（和管理页列表同一套），连分支区那一截一起盖住。
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(Theme::rowHoverFill(option.palette.color(QPalette::Base)));
        painter->drawRoundedRect(QRectF(0, option.rect.top(), option.rect.right() + 1, option.rect.height())
                                     .adjusted(0, 1, 0, -1),
                                 Theme::controlRadius(), Theme::controlRadius());
        painter->restore();
    } else if (hoverable) {
        const QColor hoverFill = Theme::rowHoverFill(option.palette.color(QPalette::Base));
        if (m_navRow) {
            const QRectF hoverRect = QRectF(option.rect.adjusted(3, 2, -3, -2));
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            QColor navHover = Theme::tokens().text;
            navHover.setAlpha(14);
            painter->setBrush(navHover);
            painter->drawRoundedRect(hoverRect, Theme::controlRadius(), Theme::controlRadius());
            painter->restore();
        } else {
            painter->fillRect(option.rect, hoverFill);
        }
        // 有子条目的那几棵树上（版本树、资源表：分类 → 版本号），条目左边留着一截**分支区**，
        // 而**选中**那一层是连这一截一起涂的 —— `QTreeView::drawRow()` 先按
        // `QRect(position, y, indentation, height)` 铺一遍选中色，**然后**才把画笔交给代理
        // （长注释在 ui/InstanceGroupDelegate.cpp 里那段 ★★ ）。
        // 悬停**不补这一截**的话，同一行上"选中带"和"悬停带"的左边就差着一个缩进（默认
        // 20 px）—— 看着像是悬停没铺到头。所以照着选中的形状补上。
        // 只在第 0 列补：这一截长在**这一格自己的左边**，别的列那一截是上一列的地盘。
        // （今天挂在这份代理上的每一张表都是单列 —— 服务器表是多列，但代理只挂在第 0 列。
        // 留着这个条件是因为"从 0 开始涂"这件事只在第 0 列上成立。）
        // 宽度取 `option.rect.left()`（= 这一格从哪儿开始），不是写死的缩进：横向滚动时
        // 它和视图那边同时变小（两边同以 `columnViewportPosition(0)` 为原点），整截滚出屏
        // 时它 ≤ 0、这一句自己什么都不画；写死的话往右滚一下左边就留一根擦不掉的条。
        if (index.column() == 0 && option.rect.left() > 0) {
            painter->fillRect(
                QRect(0, option.rect.top(), option.rect.left(), option.rect.height()), hoverFill);
        }
    }
    if (QStyle *style = opt.widget != nullptr ? opt.widget->style() : QApplication::style()) {
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
    }

    // 选中那一行的**分支区那一截**：`QTreeView::drawRow()` 在把画笔交给代理之前自己先用
    // **平台的选中色（蓝）**把 `QRect(0, y, 缩进, 行高)` 铺一遍（长注释在
    // ui/InstanceGroupDelegate.cpp 里那段 ★★ ）。样式那一层已经把
    // `PE_PanelItemViewRow` 也接过去画成我们这一档了，所以这里**不再补涂**：7a 令牌里
    // 选中底是半透明的，再涂一遍那一截就比整行深一档。
    // NOTE: 实例树那一边**故意不补**（它选中时的带子只有卡片那一圈，左边没有竖条，
    // 见那条 ★★）。两张表的选中带形状本来就不一样。

    // Sidebar selection uses a rounded inset block (no rail), matching other navigation lists.
    // The platform Selected layer is suppressed above so it cannot paint another background.
    if (m_navRow && selected) {
        const QRectF selectedRect = QRectF(option.rect.adjusted(3, 2, -3, -2));
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        // 和左导航轨同一套：半径 10 的低透明主题色底，不画指示条。
        const Theme::Tokens t = Theme::tokens();
        QColor fill = t.focus;
        fill.setAlpha(t.page.lightness() < 128 ? 34 : 30);
        painter->setBrush(fill);
        painter->drawRoundedRect(selectedRect, Theme::controlRadius(), Theme::controlRadius());
        painter->restore();
    }

    const QRect row = option.rect;
    // 选中那一行的**墨**：底已经不是调色板里那一档蓝了（是我们自己画的那一块），而这一行上
    // 手画的每一笔（正文、第二行小字、图标、右边的箭头）都从下面这份 palette 取色 ——
    // 不换的话深色主题下就是白字压浅底（等于没字）。换哪几档只有一处出处
    // （`selectionPalette()`），换的是**一份局部拷贝**：控件的调色板、别处的取色一点都不
    // 受影响。
    // 侧栏那一列**不用单独换一遍**：两条路的底是同一个混法，墨自然也是同一支 ——
    // 多留一处取色点就是"以后改一处漏一处"的形状。
    QPalette palette = Theme::selectionPalette(option.palette, selected);
    // 不可用的那几条（安装页上"选中了一个加载器之后其余几条"）**整行淡下去**：这一行
    // 的字和图标都是这儿自己画的，不处理的话"点得动"和"点了没反应"在屏幕上一模一样
    // —— 不处理就会出现"点不开了"那个形状，别再制造一次。
    //
    // 判据用 index.flags() 而不是 option.state：Qt 到底有没有替我们把 State_Enabled
    // 填进去要看版本和样式，而**这一位是我们自己 setDisabled() 写下去的**，
    // 读它就是读我们写的那一位。禁用是整行的事（图标、两行字、右边的箭头一起淡），
    // 所以按透明度来，而不是挨个换调色板里那一档颜色。
    const bool rowEnabled = (index.flags() & Qt::ItemIsEnabled) != 0;
    // 0.5：一眼看得出"这条现在点不得"，但字还读得清（那句话本身就是信息 ——
    // 「与 Forge 不兼容」正是给用户看的）。下面两处画东西的地方各用一次（第二处是
    // 右边的箭头，它画在 save/restore 之外）。
    const qreal rowOpacity = rowEnabled ? 1.0 : 0.5;
    // 顶层那四条是**分类**（一行字 + 右边那个能转的箭头），底下的子条目是**版本号**
    // （草方块 + 版本号 + 发布时间）。判据就是"有没有爹"。
    const bool isChannel = !index.parent().isValid();
    const auto *view = qobject_cast<const QTreeView *>(option.widget);
    const bool expanded = view != nullptr && view->isExpanded(index);
    const bool hasChildren =
        view != nullptr && view->model() != nullptr && view->model()->rowCount(index) > 0;

    const int pad = MainWindowUI::versionTreeRowPadding();
    const int iconSize = rowIconSize();

    // 前面那个小图标：条目自己给了 `Qt::DecorationRole` 就画它（加载器那棵树五条都是
    // 这么给的 —— 各自那个标志），没给就按版本树的规矩来（**版本号**前面是草方块、
    // 分类前面什么都没有）。给成空的 QIcon 等于没给。
    QPixmap mark;
    const QVariant decoration = index.data(MainWindowUI::rowIconRole());
    if (decoration.canConvert<QIcon>()) {
        mark = qvariant_cast<QIcon>(decoration).pixmap(iconSize, iconSize);
    } else if (decoration.canConvert<QPixmap>()) {
        mark = qvariant_cast<QPixmap>(decoration).scaled(iconSize, iconSize, Qt::KeepAspectRatio,
                                                         Qt::SmoothTransformation);
    }
    if (mark.isNull() && !isChannel && !m_resourceRows) {
        mark = VersionIcons::grassBlock(iconSize);
    }

    // 视图可以用 `wtfmlRowLeftPad` 属性把图标和文字整体再往里推一截（高亮底色仍铺满整行）：
    // 下载安装页的加载器列表左缘和上面「版本名称」框齐平，行内内容要退到框内文字那条线上。
    const int leftPad = pad + (view != nullptr ? view->property("wtfmlRowLeftPad").toInt() : 0);
    int textLeft = row.left() + leftPad;
    if (m_resourceRows && !isChannel && mark.isNull() && view != nullptr) {
        // 搜索结果底下的版本行：不画图标，文字对齐父行名字那一列，再往里缩一档（深一层缩 12）。
        int depth = 0;
        for (QModelIndex i = index; i.parent().isValid(); i = i.parent()) ++depth;
        textLeft = row.left() - depth * view->indentation() + leftPad + iconSize + 8 + (depth - 1) * 12;
    }
    QRect iconRect;
    if (!mark.isNull()) {
        iconRect = QRect(row.left() + leftPad, row.center().y() - iconSize / 2, iconSize, iconSize);
        textLeft = iconRect.right() + 1 + 8;
    }
    // 行内加载态（LoadingState::beginRow / failRow 那条占位行）：点 + 话 / 出错 + 重试，
    // 整行交给它画（不走下面那层"禁用就淡到 0.5"）。从图标那一格起手，和兄弟行同缩进。
    if (LoadingState::paintRow(painter, option, index, iconRect.isNull() ? textLeft : iconRect.left())) {
        return;
    }
    // 右边那个箭头自己占一格，文字不许压过去；选中的那条加载器右边还多一个「取消」小叉
    // （rowClearRole()，见下），那一格也要从文字的可用宽度里扣掉。
    const bool showClear = index.data(MainWindowUI::rowClearRole()).toBool();
    const QRect crossBox = clearRect(row);
    // 行尾那个刷新按钮（rowRefreshRole()）：和上面那个叉一样，它占的那一格也要从文字的
    // 可用宽度里扣掉 —— 不扣的话长条目会从它底下穿过去。
    // 这个按钮**只在鼠标悬停到这一行时才画**。扣宽度那一条**不**跟着挂 hovered ——
    // 宽度一直留着，鼠标挪进来的时候正文不会左右跳一下（那比少一个图标显眼得多）。
    // NOTE: hovered 是**视图**填的，而视图**默认不填** ——
    // 干净的 QListWidget 视口 WA_Hover=0 / mouseTracking=0，真鼠标移动一个字都不改
    // State_MouseOver；给那份列表开 setMouseTracking(true) 之后才会改（真平台的
    // windows 与 offscreen 各量一遍，同一个结果）。所以这一段搭在 MainWindowUI 里
    // 建左栏那份列表时那句 setMouseTracking 上 —— 那句被删掉的话，症状是"刷新按钮
    // 再也不出现了"，不是报错、也不是断言先红。
    //
    // 两个判据必须拆开：**这一行有没有这一格**（`hasRefresh`，一直扣宽度）、
    // **这一刻画不画它**（`showRefresh`，跟着 hovered）。合成一个的话鼠标一进这一行：
    // 图标冒出来（对的）+ 文字宽度当场变窄 ⇒ 长条目的省略号位置左右跳一下，正是上面那句
    // "那比少一个图标显眼得多"要避免的。
    const bool hasRefresh = index.data(MainWindowUI::rowRefreshRole()).toBool();
    const bool showRefresh = hasRefresh && hovered;
    const QRect refreshBox = refreshRect(row);
    int textRight = row.right() - pad - MainWindowUI::versionTreeTrailWidth() - 6;
    if (showClear) {
        textRight -= crossBox.width() + 6;
    }
    if (hasRefresh) {
        textRight -= refreshBox.width() + 6;
    }
    const QRect textRect(textLeft, row.top(), qMax(0, textRight - textLeft), row.height());
    const QString mainText = index.data(Qt::DisplayRole).toString();

    painter->save();
    painter->setOpacity(rowOpacity);
    // 第二行小字（rowSubTextRole）：版本树上是发布时间，加载器那棵树上是「与 X 不兼容」
    // 或者选中的版本号。两种行**都**看这个角色 —— 空串就是不画。
    const QString subText = index.data(MainWindowUI::rowSubTextRole()).toString();
    // 服务器那一行的 MOTD（rowMotdRawRole()）：**照游戏里的样子画** ——
    // 带颜色、带字形、还是两行。
    //
    // NOTE: **在这里现解析**，不把解析结果塞进 QVariant：`QVector<MotdPiece>` 要做成
    // 元类型得先登记（跨线程的排队连接还要 `qRegisterMetaType`），而这一串是**画**的
    // 时候才用得着的东西 —— 现解析一遍的代价是几十个字符的扫描，比那条路省事得多。
    //
    // NOTE: 和 `rowSubTextRole()` 是**二选一**：两个角色都非空时只画 MOTD 那条
    // （否则同一格上会叠两段字）。`rowSubTextRole()` 那份留着 —— 它有两个用处：
    // 悬停提示，以及"只有洗过的那一行、没有原串"的老路（缓存里没有 motdRaw 时）。
    const QString motdRaw = index.data(MainWindowUI::rowMotdRawRole()).toString();
    const QVector<MotdPiece> motdPieces =
        motdRaw.isEmpty() ? QVector<MotdPiece>() : MotdText::parse(motdRaw, 2);
    const int motdLines = motdPieces.isEmpty() ? 0 : (motdPieces.last().line + 1);
    QFont subFont = option.font;
    subFont = Theme::textFont(subFont, Theme::TextRole::Caption);
    const QFontMetrics mainMetrics(option.font);
    const QFontMetrics subMetrics(subFont);
    // 两行合成一块，整块在行里居中（行高是常数，所以"上面一行字下面一行字"看着才稳）。
    // NOTE: 底下那一块可能是**两行**（MOTD），所以高度按行数算 —— 不按行数算的话，
    // 两行的 MOTD 会被画到行外去（行高 64 是服务器那一页逐条设的，够装）。
    const int gap = 2;
    const int subLines = motdLines > 0 ? motdLines : (subText.isEmpty() ? 0 : 1);
    const int blockHeight =
        mainMetrics.height() + (subLines == 0 ? 0 : gap + subLines * subMetrics.height());
    int y = row.center().y() - blockHeight / 2;

    QFont mainFont = option.font;
    if (isChannel) {
        // 顶层那几条（分类 / 加载器）是**粗体**：它们和底下的版本号不是一回事。
        mainFont.setBold(true);
    }
    painter->setFont(mainFont);
    painter->setPen(palette.color(QPalette::WindowText));
    // 两行都要**先截成省略号再画**。`drawText(rect, 文字)` 在装不下时是**硬裁**：
    // 最后那个字被切掉半个、右边那一小截贴在图标上，看着像"字画坏了"，而不是"这里还有更多"。
    // 版本名和加载器名都能长过这一格（自定义名更长），资源表那几张也一样 ——
    // 四张表都用这一个代理，截断也走同一份 `elidedText`。
    painter->drawText(QRect(textRect.left(), y, textRect.width(), mainMetrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      mainMetrics.elidedText(mainText, Qt::ElideRight, textRect.width()));
    y += mainMetrics.height() + gap;
    if (motdLines > 0) {
        // 底下那一块按游戏那样画：一段一段接着画，每段自己的颜色和字形；换行就回到左边。
        //
        // 颜色要**按我们的底色重新算一次对比度**（MotdText::readableOn）：游戏那边
        // 底色是黑的，服务器按黑底配的 `§f` 白字 / `§e` 黄字在我们的浅色底上几乎看不见
        // —— 白字画在白底上等于没画，而这一格本来就是"要看服务器写了什么"。
        // 底色取的是**这一行真正铺的那一层**：选中时是 Highlight、平时是 Base。读的是
        // 传进来那个 `option`（上面只抹掉了 MouseOver 那一位，选中那一位原样还在）。
        const QColor rowBg = (option.state & QStyle::State_Selected)
                                 ? palette.color(QPalette::Highlight)
                                 : palette.color(QPalette::Base);
        int line = motdPieces.first().line;
        int lineY = y;
        int x = textRect.left();
        bool skipRestOfLine = false;
        for (const MotdPiece &piece : motdPieces) {
            if (piece.line != line) {
                line = piece.line;
                lineY += subMetrics.height();
                x = textRect.left();
                skipRestOfLine = false;  // 新的一行：从头照画
            }
            if (skipRestOfLine) {
                continue;
            }
            const int avail = textRect.left() + textRect.width() - x;
            if (avail <= 0) {
                // 这一行已经画满了（正好填满时会走到这儿，x 落在右边缘上）。
                // 同一件事：只作废这一行剩下的段。
                skipRestOfLine = true;
                continue;
            }
            QFont pieceFont = subFont;
            pieceFont.setBold(piece.bold);
            pieceFont.setItalic(piece.italic);
            pieceFont.setUnderline(piece.underline);
            pieceFont.setStrikeOut(piece.strike);
            const QFontMetrics pieceMetrics(pieceFont);
            // 装不下就从**这一段**的尾巴上省略（和上面那两行同一个做法：`drawText` 装不下
            // 是硬裁，最后那个字会被切掉半个）。省略了就是这一行的结尾 —— 后面那些段
            // 再画就要压到省略号上了，作废掉。第二行（如果还有）照画，见上面那一句。
            const QString shown = pieceMetrics.elidedText(piece.text, Qt::ElideRight, avail);
            painter->setFont(pieceFont);
            painter->setPen(MotdText::readableOn(MotdText::pieceColor(piece), rowBg));
            painter->drawText(QRect(x, lineY, avail, subMetrics.height()),
                              Qt::AlignLeft | Qt::AlignVCenter, shown);
            if (shown != piece.text) {
                skipRestOfLine = true;
                continue;
            }
            x += pieceMetrics.horizontalAdvance(shown);
        }
    } else if (!subText.isEmpty()) {
        painter->setFont(subFont);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(QRect(textRect.left(), y, textRect.width(), subMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          subMetrics.elidedText(subText, Qt::ElideRight, textRect.width()));
    }
    if (!mark.isNull()) {
        painter->drawPixmap(iconRect.topLeft(), mark);
    }
    // 行尾那个刷新按钮（见 refreshRect()）。画在**这个** save/restore 里（不是下面画箭头
    // 那一块）：下面那一块对"没有孩子的顶层"会整块 return 掉（分类条目正是那种行），
    // 画在那儿的话这一格根本轮不到。用次要那一档颜色 —— 它是行里的一个动作，不是正文。
    if (showRefresh) {
        // 画的比同格里其他字形大一号（20 而不是 18）：圆形的字形在同样标称尺寸下
        // 看着比方的那个小一圈（正文前面那个草方块就是方的），而这一格是纯图标、
        // 四周还有 2 px 余量，撑不满反倒显得"这一格是空的"。
        const QRect glyph = refreshBox.adjusted(2, 2, -2, -2);
        painter->drawPixmap(glyph.topLeft(),
                            VersionIcons::refresh(glyph.width(),
                                                  (index.flags() & Qt::ItemIsEnabled) ? Theme::tokens().primary : Theme::tokens().disabled));
    }
    painter->restore();

    // 行尾那格文字（体验反馈 8：右侧不再画 > / V 小箭头，tokens-r2 §6「收起 / 展开」）：
    // 能展开的行写「展开 / 收起」；叶子行若点进去有下一页（版本树进安装页、资源表下载），
    // 悬停时写那个动词。一个版本都没有的分类什么都不写 —— 写了就骗人说点得开。
    if (isChannel && !hasChildren) {
        return;
    }
    // 叶子那一层：加载器树、服务器那一列点下去是"选中它"，没有下一页（`m_leafArrow` 为假）。
    // 注意这一位只管那格文字：同一个 save/restore 里还画着那个「取消」小叉。
    QString trailText;
    if (isChannel || m_leafArrow) {
        // NOTE: 判据只看 `expanded` 与 `hasChildren`，**不能**再与 `isChannel` 相与：下载页
        // 那张资源表里"游戏版本"那一层是非顶层、又要能展开的。
        if (isChannel || hasChildren)
            trailText = expanded ? tr("收起") : tr("展开");
        else if (hovered)
            trailText = m_leafVerb;
    }
    // 几何只有 arrowRect() 一处 —— 下载页那四张表按同一个函数认"这一下点的是不是展开格"
    // （见 MainWindow::eventFilter 里那一段），画的是它去掉四周 3 px 的那一块。
    const QRect trailBox = arrowRect(row).adjusted(3, 0, -3, 0);
    painter->save();
    painter->setOpacity(rowOpacity);
    // 选中的那条加载器：行尾文字**左边**再画一个「取消」小叉（点它 = 不装这个加载器了，
    // 退回纯原版）。它比正文浅一档：那是个次要操作，别和版本号抢眼睛。
    if (showClear) {
        const QRect glyph = crossBox.adjusted(3, 3, -3, -3);
        painter->drawPixmap(glyph.topLeft(),
                            VersionIcons::cross(glyph.width(),
                                                palette.color(QPalette::PlaceholderText)));
    }
    if (!trailText.isEmpty()) {
        // 「展开 / 收起」用次要色（说明性）；悬停动词用主题色（它是这一下点下去要做的事）。
        const bool verb = !(isChannel || hasChildren);
        painter->setFont(Theme::textFont(option.font, Theme::TextRole::Caption));
        painter->setPen(verb && (index.flags() & Qt::ItemIsEnabled) ? Theme::tokens().primary
                                                                     : palette.color(QPalette::PlaceholderText));
        painter->drawText(QRect(trailBox.left(), row.top(), trailBox.width(), row.height()),
                          Qt::AlignRight | Qt::AlignVCenter, trailText);
    }
    painter->restore();
}

QSize VersionTreeDelegate::sizeHint(const QStyleOptionViewItem &option,
                                    const QModelIndex &index) const {
    // 行高是个**常数**，不走"按内容算"：树上开着 uniformRowHeights（900 多条，让它按
    // 第一行算一次就够），而第一行是分类（一行字）、底下是版本号（两行字）——
    // 两边高度不一样的话，两行那种会被按一行的行高裁掉半行。
    //
    // 条目自己给了 `Qt::SizeHintRole` 的**按它算高度**。服务器列表那一页要的
    // 就是这一条 —— 它的行里画的是**两行** MOTD（照游戏里那个样子），46 装不下，那一页
    // 逐条 `setSizeHint(QSize(0, serverRowHeight()))`。反过来写死一个常数的话，
    // `SizeHintRole` 会被**静默无视**（Qt 的规矩是代理说了算，模型那一格只是"建议"）——
    // 症状是"设了行高、屏幕上一点没变"，不报任何东西。
    // 宽度**照旧自己算**（它要按两行字、图标、行尾那几个角色逐项加起来），只在条目自己
    // 给了更宽的要求时才让路。
    const QSize hint = index.data(Qt::SizeHintRole).toSize();
    const int height = hint.height() > 0 ? hint.height()
                       : m_resourceRows ? resourceRowHeight()
                                        : MainWindowUI::versionTreeRowHeight();
    // 顶层那几条是粗体画的，宽度也得按粗体量（不然"加载器名 + 右边那个箭头"会差几个像素）。
    QFont mainFont = option.font;
    if (!index.parent().isValid()) {
        mainFont.setBold(true);
    }
    int width = QFontMetrics(mainFont).horizontalAdvance(index.data(Qt::DisplayRole).toString());
    // 第二行小字：两棵树都可能有（版本树上是发布时间，加载器那棵树上是"与 X 不兼容"
    // 或者选中的版本号），有就按它量。
    QFont subFont = option.font;
    subFont = Theme::textFont(subFont, Theme::TextRole::Caption);
    width = qMax(width, QFontMetrics(subFont).horizontalAdvance(
                            index.data(MainWindowUI::rowSubTextRole()).toString()));
    // MOTD（rowMotdRawRole()）量的是**最长的那一行**：`§` 码不是字，不能拿原串直接量
    // （量出来比画出来的宽上一大截，那一格会白白多留一块）。
    const QString motdRaw = index.data(MainWindowUI::rowMotdRawRole()).toString();
    if (!motdRaw.isEmpty()) {
        int line = -1;
        int lineWidth = 0;
        int widest = 0;
        const QFontMetrics subMetrics(subFont);
        for (const MotdPiece &piece : MotdText::parse(motdRaw, 2)) {
            if (piece.line != line) {
                line = piece.line;
                lineWidth = 0;
            }
            lineWidth += subMetrics.horizontalAdvance(piece.text);
            widest = qMax(widest, lineWidth);
        }
        width = qMax(width, widest);
    }
    // 前面那个图标的宽度：子条目一定有（草方块或条目自己给的），顶层条目**真给了才有**。
    //
    // NOTE: 认形态必须和 paint() 那条路**一模一样**：两边都认 QIcon 和 QPixmap。
    // 只认 QIcon 的话，"给的是 QPixmap"的条目画得出图标、量宽度时却没算它，右边那个
    // 箭头（有「取消」小叉时还有小叉）会压掉一截文字。产品代码全走 setIcon()（QIcon），
    // 但验证代码会走 QPixmap，这一位不能省。
    const QVariant decoration = index.data(MainWindowUI::rowIconRole());
    bool hasIcon = false;
    if (decoration.canConvert<QIcon>()) {
        hasIcon = !qvariant_cast<QIcon>(decoration).isNull();
    } else if (decoration.canConvert<QPixmap>()) {
        hasIcon = !qvariant_cast<QPixmap>(decoration).isNull();
    }
    if (index.parent().isValid() || hasIcon) {
        width += rowIconSize() + 8;
    }
    width += 2 * MainWindowUI::versionTreeRowPadding() + MainWindowUI::versionTreeTrailWidth() + 6;
    // 行尾那个「取消」小叉也占宽度（只有选中的那条加载器有，见 rowClearRole()）。
    if (index.data(MainWindowUI::rowClearRole()).toBool()) {
        width += MainWindowUI::versionTreeArrowSize() + 6 + 6;
    }
    // 行尾那个刷新按钮同理（见 rowRefreshRole()/refreshRect()）：它比那个叉大一号
    // （跟着正文图标那个 18 px 走），间距还是 6。
    // NOTE: 判据里**没有** hovered —— 那一格是"这一行有这么一个按钮"占的地方，不是"这会儿
    // 画不画"。跟着悬停变的话，鼠标一扫过去整行的宽度就跟着变（paint 那段有说明）。
    if (index.data(MainWindowUI::rowRefreshRole()).toBool()) {
        width += MainWindowUI::versionTreeIconSize() + 6 + 6;
    }
    return QSize(width, height);
}

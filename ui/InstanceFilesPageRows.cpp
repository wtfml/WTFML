/**
 * @file InstanceFilesPageRows.cpp
 * @brief `InstanceFilesPage` 里**列表那一行**这一摊：建条目、代理画行、行尾那几格的命中与点击。
 *
 * 【这一摊的状态】
 *
 * 一份「这一条是什么」的账 —— `m_icons` / `m_resourceTips`（按 `identityPath()` 存）、
 * `m_rowsBuilt` / `m_shownIdentities`，加上 `m_list` 里那些条目（数据都在角色里，见 .hpp
 * 「给验证工程用的访问器」那段）。`refresh()` 是 `clear()` + 重排条目，而图标和详情是异步问
 * 回来的（读盘 + 联网，几秒）—— 账要是跟着条目一起没了，重列出来的那一行会先退回占位图
 * 再等一次回答。
 *
 * 【行是画的】
 *
 * 一行的几何只有 `layoutRow()` 一处：`RowDelegate::paint()` 照它画，`actionAt()` 照它认
 * "点在哪一格"，验证工程照 `rowGeometry()` 量。几格的尺寸照搬原来那套行控件：左右内边距
 * 12、格间距 12、行首图标 `instanceFileIconSize()`、两行字竖直居中且间距 4、行尾按钮大小问
 * 一颗藏着的 `QToolButton`（`m_buttonProbe`，样式表照样作用在它身上），开关是
 * `ToggleSwitch::switchWidth/Height()`。
 *
 * 不在这儿：扫目录（`refresh()`）、筛选、统计、选择与批量操作、悬浮操作条、事件过滤。
 */

#include "InstanceFilesPage.hpp"

#include "Animations.hpp"
#include "InstanceFilesPageInternals.hpp"
#include "MainWindowUI.hpp"
#include "Theme.hpp"
#include "ToggleSwitch.hpp"
#include "VersionIcons.hpp"
#include "UpdateGlyph.hpp"

#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QStyleOptionToolButton>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>

using namespace InstanceFilesPageInternals;

namespace {


/// 某一条**没有真图标**时那一格摆什么（见 makeItem 里那段）。
///
/// 文件夹和文件分开：文件夹的形状（folderMark）是它**本来的**长相、不是"没查到"，
/// 而文件那一片用拼图（见 VersionIcons::puzzleMark 那段为什么是它）。
QPixmap placeholderIcon(bool isDir, const QColor &color) {
    const int size = MainWindowUI::instanceFileIconSize();
    if (isDir) {
        return VersionIcons::folderMark(size, color);
    }
    return VersionIcons::puzzleMark(size, color);
}

/// 把**接口给的那张真图**缩到这一格的大小。
///
/// `QLabel::setPixmap()` **不缩放**：它按 pixmap 本来的大小画，超出控件的那一块被裁掉。
/// 而作者传的那张图本来就不是这一格的大小 —— Modrinth 那条 `_96.webp` 是 96 px，
/// `icon.png` 那条更是**原图**（几百 px 的都有），而这一格只有
/// `MainWindowUI::instanceFileIconSize()`（32）。不缩放的话每一张真图标都只露出左上角
/// 那一小块，看着像"图裂了"。
///
/// NOTE: 这个毛病**只有真图标真的加载出来了**才看得见 —— 占位图本来就是照这一格的尺寸画的
/// （见 placeholderIcon），一个像素都不差。
///
/// NOTE: `KeepAspectRatio` 而不是拉满 —— 资源包封面不是方的，拉满会变形。
/// （`setScaledContents(true)` 那个开关的意思恰好就是"拉满"，所以不能用它。）
/// 缩放走 `SmoothTransformation`：这是给眼睛看的图，不是拿来逐像素比对的。
/// NOTE: **本来就装得下的图一个像素都不动**：放大只会糊（图标真有 16 px 的），
/// 而且小图原样交给 QLabel 时它按对齐方式摆在那一格里（见 makeRow 里的 AlignCenter）。
QPixmap scaleToIconSlot(const QPixmap &icon) {
    const int slot = MainWindowUI::instanceFileIconSize();
    if (icon.isNull() || (icon.width() <= slot && icon.height() <= slot)) {
        return icon;
    }
    return icon.scaled(slot, slot, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

/// 目录自带的 icon.png；未压缩的资源包、数据包也读取 pack.png。
///
/// NOTE: 那条文件名是**游戏的约定**，不是我们发明的：大小写按它写的 `icon.png` 认
/// （Windows 上不区分，别的系统上区分，照原样写最保险）。
/// NOTE: 不去认"这是不是一个真存档"（比如查 `level.dat` 在不在）—— 摆一张图而已，
/// 目录里有那张图就画。多一道判断就多一处"哪些目录算数"的规则要跟着游戏版本改。
/// NOTE: 只有**目录**才看这个（见 iconForPath 里 `isDir` 那个闸）：文件自己有哈希那条路。
QPixmap directoryThumbnail(const QString &dirPath) {
    QString candidate = dirPath + QStringLiteral("/icon.png");
    if (!QFileInfo::exists(candidate)) candidate = dirPath + QStringLiteral("/pack.png");
    if (!QFileInfo::exists(candidate)) {
        return QPixmap();
    }
    // `icon.png` 是**世界里带进来的**（共享存档里那一份也长这样），当外部输入看：
    // 先问头里声明的尺寸、超了不解。原版那张是 64×64，这里留到 1024 —— 再大的一律
    // 当"读不出来"，调用方退回文件夹字形。
    constexpr int kMaxSide = 1024;
    constexpr qint64 kMaxPixels = 1024LL * 1024;
    QImageReader reader(candidate);
    const QSize pixels = reader.size();
    if (!pixels.isValid() || pixels.width() <= 0 || pixels.height() <= 0
        || pixels.width() > kMaxSide || pixels.height() > kMaxSide
        || qint64(pixels.width()) * pixels.height() > kMaxPixels) {
        return QPixmap();
    }
    // 读不出来的（坏文件、不是张图）回空的那张，调用方自然退回文件夹字形 —— 不弹错、
    // 也不留一格里半张烂图。
    const QImage decoded = reader.read();
    if (decoded.isNull()) {
        return QPixmap();
    }
    return QPixmap::fromImage(decoded);
}

}  // namespace

// ============================================================================
// 代理：一行怎么画
// ============================================================================

class InstanceFilesPage::RowDelegate final : public QStyledItemDelegate {
public:
    RowDelegate(InstanceFilesPage *page, QObject *parent) : QStyledItemDelegate(parent), m_page(page) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(0, MainWindowUI::instanceFileRowHeight());
    }

    // 点击、空格、双击全由页上的事件过滤器管（见 eventFilter）；这里一律不接，免得基类按
    // 「可勾选」去改 CheckStateRole —— 开关的状态只能由盘上改名之后的那一趟刷新摆。
    bool editorEvent(QEvent *, QAbstractItemModel *, const QStyleOptionViewItem &,
                     const QModelIndex &) override {
        return false;
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &,
                   const QModelIndex &index) override {
        if (event == nullptr || event->type() != QEvent::ToolTip) return false;
        const QListWidgetItem *item = m_page->m_list->item(index.row());
        if (item == nullptr) return false;
        const QString path = item->data(pathRole()).toString();
        const RowGeometry g = m_page->layoutRow(view->visualRect(index), item->data(isDirRole()).toBool());
        const RowAction action = m_page->actionAt(event->pos());
        QString text;
        if (action != RowAction::None) {
            text = m_page->actionToolTip(item, action);
        } else if (g.icon.contains(event->pos()) || g.name.contains(event->pos())) {
            // 行首图标和名字两格挂整条路径（名字被省略掉的那一截靠它看全）。
            text = item->toolTip();
        }
        if (text.isEmpty()) {
            QToolTip::hideText();
        } else {
            QToolTip::showText(event->globalPos(), text, view->viewport());
        }
        return true;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QListWidgetItem *item = m_page->m_list->item(index.row());
        if (item == nullptr) return;
        const QString path = item->data(pathRole()).toString();
        const bool isDir = item->data(isDirRole()).toBool();
        const bool disabled = item->data(disabledRole()).toBool();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = !path.isEmpty() && path == m_page->m_hoverPath;
        const QPalette &palette = option.palette;

        // ---------- 行底：选中 = 选中色淡底，悬停 = hover 淡底，都是内缩 1 px 的圆角块 ----------
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const RowGeometry g = m_page->layoutRow(option.rect, isDir);
        if (selected || hovered) {
            const Theme::Tokens t = Theme::tokens();
            painter->setPen(Qt::NoPen);
            // 选中色和行尾按钮的底同一个出处（selectionFill），否则按钮会在行上留色块。
            painter->setBrush(selected ? Theme::selectionFill(palette.color(QPalette::Base)) : t.hover);
            painter->drawRoundedRect(QRectF(g.row).adjusted(0, 1, 0, -1), Theme::controlRadius(),
                                     Theme::controlRadius());
        }

        // 新冒出来的那几行淡进来（只淡内容，行底照常）。
        const QString identity = identityPath(path);
        if (m_page->m_fadeIdentities.contains(identity) && m_page->m_fadeClock.isValid()) {
            const qreal progress = qBound<qreal>(0.0, m_page->m_fadeClock.elapsed() / qreal(Anim::rowAppearMs()), 1.0);
            painter->setOpacity(progress);
        }

        // ---------- 行首图标：原样摆在那一格正中（缩放在存进条目之前就做完了） ----------
        if (!g.icon.isNull()) {
            const QPixmap icon = item->data(Qt::DecorationRole).value<QPixmap>();
            if (!icon.isNull()) {
                const QSize shown = icon.deviceIndependentSize().toSize();
                const QRect at(g.icon.x() + (g.icon.width() - shown.width()) / 2,
                               g.icon.y() + (g.icon.height() - shown.height()) / 2, shown.width(),
                               shown.height());
                painter->drawPixmap(at, icon);
            }
        }

        // ---------- 名字一行 + 大小 / 时间一行 ----------
        // 名字：正文墨；批量页上选中那一行换成选中墨（Theme::selectionPalette）；禁用的那条
        // 灰掉并划一道线（光靠尾巴那四个字扫一眼看不出这一条是关着的）。
        // 小字：次要墨 + Caption 字号，选中时也不换（它是这一条的规格，不随状态走）。
        QFont nameFont = option.font;
        QColor nameInk = palette.color(QPalette::WindowText);
        if (disabled) {
            nameInk = palette.color(QPalette::PlaceholderText);
            nameFont.setStrikeOut(true);
        } else if (selected && m_page->m_batchMode) {
            nameInk = Theme::selectionPalette(palette, true).color(QPalette::WindowText);
        }
        QString name = item->data(Qt::AccessibleTextRole).toString();
        const bool hasUpdate = m_page->m_outdatedPaths.contains(path);
        const QString updateSuffix = QStringLiteral(" ") + InstanceFilesPage::updateArrowText();
        if (hasUpdate && name.endsWith(updateSuffix)) name.chop(updateSuffix.size());
        painter->setFont(nameFont);
        const QFontMetrics nameMetrics = painter->fontMetrics();
        const int updateSize = hasUpdate ? 18 : 0;
        const int updateSpace = hasUpdate ? updateSize + 6 : 0;
        const QString nameShown = nameMetrics.elidedText(name, Qt::ElideMiddle, qMax(0, g.name.width() - updateSpace));
        if (nameShown != name) {
            m_page->m_elidedNames.insert(path);
        } else {
            m_page->m_elidedNames.remove(path);
        }
        painter->setPen(nameInk);
        QRect textBounds;
        const QRect textRect = g.name.adjusted(0, 0, -updateSpace, 0);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, nameShown, &textBounds);
        if (hasUpdate && g.name.width() >= updateSpace) {
            // 使用这次 drawText 的实际边界，跟随画笔解析后的字体和设备 DPI。
            const int left = qMin(textBounds.right() + 7, g.name.right() - updateSize + 1);
            const QRect mark(left,
                             g.name.center().y() - updateSize / 2 + 1, updateSize, updateSize);
            UpdateGlyph::icon(nameInk).paint(painter, mark);
        }

        const QFont metaFont = Theme::textFont(option.font, Theme::TextRole::Caption);
        const QFontMetrics metaMetrics(metaFont);
        painter->setFont(metaFont);
        painter->setPen(palette.color(QPalette::PlaceholderText));
        painter->drawText(g.meta, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          metaMetrics.elidedText(item->data(metaRole()).toString(), Qt::ElideRight, g.meta.width()));

        // ---------- 行尾那几格 ----------
        const auto pressed = [&](RowAction action) {
            return m_page->m_pressAction == action && m_page->m_pressActionPath == path;
        };
        const auto hoveredAction = [&](RowAction action) {
            return hovered && m_page->m_hoverAction == action;
        };
        const auto drawButton = [&](const QRect &rect, RowAction action, VersionIcons::Action glyph) {
            if (rect.isNull()) return;
            QStyleOptionToolButton button;
            button.initFrom(m_page->m_buttonProbe);
            button.rect = rect;
            button.palette = palette;
            button.state = QStyle::State_AutoRaise;
            if (m_page->actionEnabled(item, action)) button.state |= QStyle::State_Enabled;
            if (hoveredAction(action)) button.state |= QStyle::State_MouseOver;
            if (pressed(action) && hoveredAction(action)) button.state |= QStyle::State_Sunken;
            button.subControls = QStyle::SC_ToolButton;
            button.activeSubControls = hoveredAction(action) ? QStyle::SC_ToolButton : QStyle::SC_None;
            button.toolButtonStyle = Qt::ToolButtonIconOnly;
            button.features = QStyleOptionToolButton::None;
            const int side = MainWindowUI::instanceFileActionGlyphSize();
            button.iconSize = QSize(side, side);
            button.icon = VersionIcons::actionIcon(glyph);
            QStyle *style = m_page->m_buttonProbe->style();
            style->drawComplexControl(QStyle::CC_ToolButton, &button, painter, m_page->m_buttonProbe);
        };
        // 键盘走到这一行时同样露出（鼠标悬停之外的另一条路）。
        if (hovered || Theme::isKeyboardCurrentRow(option, index)) {
            drawButton(g.detail, RowAction::Detail, VersionIcons::Action::Details);
        }
        if (!g.toggle.isNull()) {
            ToggleSwitch::paintSwitch(painter, g.toggle, palette.color(QPalette::Accent),
                                      item->checkState() == Qt::Checked, hoveredAction(RowAction::Toggle),
                                      pressed(RowAction::Toggle) && hoveredAction(RowAction::Toggle), true);
        }
        drawButton(g.edit, RowAction::Edit, VersionIcons::Action::Edit);
        drawButton(g.remove, RowAction::Delete, VersionIcons::Action::QuietDelete);
        painter->restore();
    }

private:
    InstanceFilesPage *m_page;
};

QStyledItemDelegate *InstanceFilesPage::createRowDelegate() {
    m_buttonProbe = new QToolButton(this);
    m_buttonProbe->setAutoRaise(true);
    const int glyph = MainWindowUI::instanceFileActionGlyphSize();
    m_buttonProbe->setIconSize(QSize(glyph, glyph));
    m_buttonProbe->setIcon(VersionIcons::actionIcon(VersionIcons::Action::QuietDelete));
    m_buttonProbe->hide();
    m_fadeTimer = new QTimer(this);
    m_fadeTimer->setInterval(Anim::stepMs());
    connect(m_fadeTimer, &QTimer::timeout, this, [this] {
        if (!m_fadeClock.isValid() || m_fadeClock.elapsed() >= Anim::rowAppearMs()) {
            m_fadeTimer->stop();
            m_fadeIdentities.clear();
        }
        if (m_list != nullptr) m_list->viewport()->update();
    });
    return new RowDelegate(this, m_list);
}

// ============================================================================
// 几何与命中
// ============================================================================

QSize InstanceFilesPage::actionButtonSize() const {
    return m_buttonProbe != nullptr ? m_buttonProbe->sizeHint() : QSize(28, 28);
}

InstanceFilesPage::RowGeometry InstanceFilesPage::layoutRow(const QRect &rowRectIn, bool isDir) const {
    // 照搬原来那套行控件的 QHBoxLayout（左右内边距 12、格间距 12）：固定大小的格子在行里
    // 竖直居中，名字那一列吃掉剩下的宽度。
    constexpr int kMargin = 12;
    constexpr int kSpacing = 12;
    RowGeometry g;
    // 视口页滑条常驻：行右缘让出 wtfmlScrollContentGap（见 Theme::applyViewportScrollGutter）。
    const QRect rowRect = rowRectIn.adjusted(0, 0, -m_list->property("wtfmlScrollContentGap").toInt(), 0);
    g.row = rowRect;
    const auto centered = [&rowRect](int x, const QSize &size) {
        return QRect(x, rowRect.y() + (rowRect.height() - size.height()) / 2, size.width(), size.height());
    };
    int left = rowRect.x() + kMargin;
    int right = rowRect.x() + rowRect.width() - kMargin;  // 右边界（不含）
    if (m_rowIconsVisible) {
        const int side = MainWindowUI::instanceFileIconSize();
        g.icon = centered(left, QSize(side, side));
        left += side + kSpacing;
    }
    const QSize button = actionButtonSize();
    const auto takeRight = [&](const QSize &size) {
        right -= size.width();
        const QRect rect = centered(right, size);
        right -= kSpacing;
        return rect;
    };
    if (m_canDelete) g.remove = takeRight(button);
    if (m_canEdit && isDir) g.edit = takeRight(button);
    if (m_canToggle && !isDir) g.toggle = takeRight(QSize(ToggleSwitch::switchWidth(), ToggleSwitch::switchHeight()));
    if (m_batchMode) g.detail = takeRight(button);

    // 名字一行、小字一行，两行合成一块竖直居中，行间距 4（原来那一列 QVBoxLayout 的样子）。
    const QFont font = m_list != nullptr ? m_list->font() : QFont();
    const int nameHeight = QFontMetrics(font).height();
    const int metaHeight = QFontMetrics(Theme::textFont(font, Theme::TextRole::Caption)).height();
    const int block = nameHeight + 4 + metaHeight;
    const int top = rowRect.y() + (rowRect.height() - block) / 2;
    const int width = qMax(nameLabelMinWidth(), right + kSpacing - left - kSpacing);
    g.name = QRect(left, top, width, nameHeight);
    g.meta = QRect(left, top + nameHeight + 4, width, metaHeight);
    return g;
}

bool InstanceFilesPage::rowHasAction(const QListWidgetItem *item, RowAction action) const {
    if (item == nullptr) return false;
    const bool isDir = item->data(isDirRole()).toBool();
    switch (action) {
    case RowAction::Detail: return m_batchMode;
    case RowAction::Toggle: return m_canToggle && !isDir;
    case RowAction::Edit: return m_canEdit && isDir;
    case RowAction::Delete: return m_canDelete;
    case RowAction::None: break;
    }
    return false;
}

bool InstanceFilesPage::actionEnabled(const QListWidgetItem *item, RowAction action) const {
    if (!rowHasAction(item, action)) return false;
    // 「详情」的判据只有一处：`m_resourceTips` 里有这一条 = 认出来了（见 setResourceForPath）。
    // 取的是 `identityPath(path)` 那一格 —— 禁用/启用只改了名字，认出来的还是同一个资源。
    if (action == RowAction::Detail)
        return m_resourceTips.contains(identityPath(item->data(pathRole()).toString()));
    return true;
}

QString InstanceFilesPage::actionToolTip(const QListWidgetItem *item, RowAction action) const {
    if (!rowHasAction(item, action)) return QString();
    const bool isDir = item->data(isDirRole()).toBool();
    switch (action) {
    case RowAction::Detail: {
        const auto tip = m_resourceTips.constFind(identityPath(item->data(pathRole()).toString()));
        return tip != m_resourceTips.constEnd() ? tip.value() : tr("未识别资源，暂无详情。");
    }
    case RowAction::Toggle:
        return item->data(disabledRole()).toBool() ? tr("%1所选文件。").arg(toggleOffText())
                                                   : tr("%1所选文件，保留原文件。").arg(toggleOnText());
    case RowAction::Edit: return tr("编辑存档名称和游戏规则。");
    case RowAction::Delete: return isDir ? tr("删除文件夹及其内容。") : tr("删除文件。");
    case RowAction::None: break;
    }
    return QString();
}

InstanceFilesPage::RowAction InstanceFilesPage::actionAt(const QPoint &pos, int *rowOut) const {
    if (rowOut != nullptr) *rowOut = -1;
    if (m_list == nullptr) return RowAction::None;
    QListWidgetItem *item = m_list->itemAt(pos);
    if (item == nullptr) return RowAction::None;
    if (rowOut != nullptr) *rowOut = m_list->row(item);
    const RowGeometry g = layoutRow(m_list->visualItemRect(item), item->data(isDirRole()).toBool());
    if (g.toggle.contains(pos)) return RowAction::Toggle;
    if (g.edit.contains(pos)) return RowAction::Edit;
    if (g.remove.contains(pos)) return RowAction::Delete;
    // 「详情」藏着的时候不是鼠标目标；按得到它的时候光标一定已经压在这一行上了。
    if (g.detail.contains(pos)) return RowAction::Detail;
    return RowAction::None;
}

void InstanceFilesPage::triggerRowAction(const QString &path, RowAction action) {
    switch (action) {
    case RowAction::Detail: emit detailRequested(path); break;
    case RowAction::Toggle: requestToggle(path); break;
    case RowAction::Edit: emit editRequested(path); break;
    case RowAction::Delete: emit deleteRequested(path); break;
    case RowAction::None: break;
    }
}

QListWidgetItem *InstanceFilesPage::itemForPath(const QString &path) const {
    if (m_list == nullptr || path.isEmpty()) return nullptr;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (item != nullptr && item->data(pathRole()).toString() == path) return item;
    }
    return nullptr;
}

void InstanceFilesPage::setHover(const QString &path, RowAction action) {
    if (path == m_hoverPath && action == m_hoverAction) return;
    QListWidgetItem *before = itemForPath(m_hoverPath);
    m_hoverPath = path;
    m_hoverAction = path.isEmpty() ? RowAction::None : action;
    if (m_list == nullptr) return;
    QWidget *viewport = m_list->viewport();
    if (before != nullptr) viewport->update(m_list->visualItemRect(before));
    if (QListWidgetItem *now = itemForPath(m_hoverPath)) viewport->update(m_list->visualItemRect(now));
    // 开关那一格和原来那枚 ToggleSwitch 一样给手形光标（它是一件可以点的事，不是一行字）。
    if (m_hoverAction == RowAction::Toggle) {
        viewport->setCursor(Qt::PointingHandCursor);
    } else {
        viewport->unsetCursor();
    }
}

// ============================================================================
// 给验证工程用的访问器
// ============================================================================

InstanceFilesPage::RowGeometry InstanceFilesPage::rowGeometry(int row) const {
    QListWidgetItem *item = m_list != nullptr ? m_list->item(row) : nullptr;
    if (item == nullptr) return RowGeometry();
    RowGeometry g = layoutRow(m_list->visualItemRect(item), item->data(isDirRole()).toBool());
    return g;
}

QRect InstanceFilesPage::rowActionRect(int row, RowAction action) const {
    const RowGeometry g = rowGeometry(row);
    switch (action) {
    case RowAction::Detail: return g.detail;
    case RowAction::Toggle: return g.toggle;
    case RowAction::Edit: return g.edit;
    case RowAction::Delete: return g.remove;
    case RowAction::None: break;
    }
    return QRect();
}

bool InstanceFilesPage::rowActionEnabled(int row, RowAction action) const {
    return actionEnabled(m_list != nullptr ? m_list->item(row) : nullptr, action);
}

QString InstanceFilesPage::rowActionToolTip(int row, RowAction action) const {
    return actionToolTip(m_list != nullptr ? m_list->item(row) : nullptr, action);
}

bool InstanceFilesPage::rowDetailShown(int row) const {
    const QListWidgetItem *item = m_list != nullptr ? m_list->item(row) : nullptr;
    return m_batchMode && item != nullptr && item->data(pathRole()).toString() == m_hoverPath;
}

bool InstanceFilesPage::rowNameElided(int row) const {
    const QListWidgetItem *item = m_list != nullptr ? m_list->item(row) : nullptr;
    return item != nullptr && m_elidedNames.contains(item->data(pathRole()).toString());
}

bool InstanceFilesPage::rowFadingIn(int row) const {
    const QListWidgetItem *item = m_list != nullptr ? m_list->item(row) : nullptr;
    return item != nullptr && m_fadeClock.isValid() && m_fadeClock.elapsed() < Anim::rowAppearMs()
           && m_fadeIdentities.contains(identityPath(item->data(pathRole()).toString()));
}

void InstanceFilesPage::hoverRow(int row) {
    if (m_list == nullptr) return;
    QWidget *viewport = m_list->viewport();
    QListWidgetItem *item = m_list->item(row);
    if (item == nullptr) {
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(viewport, &leave);
        return;
    }
    m_list->scrollToItem(item);
    const QRect rect = m_list->visualItemRect(item);
    // 落在名字那一格上（不压着任何一颗按钮）。
    const QPoint pos = layoutRow(rect, item->data(isDirRole()).toBool()).name.center();
    QMouseEvent move(QEvent::MouseMove, pos, viewport->mapToGlobal(pos), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
}

bool InstanceFilesPage::clickRowAction(int row, RowAction action) {
    if (m_list == nullptr) return false;
    QListWidgetItem *item = m_list->item(row);
    if (!actionEnabled(item, action)) return false;
    QWidget *viewport = m_list->viewport();
    const QRect cell = rowActionRect(row, action);
    // 那一格不在视口里：**不替用户滚**（scrollToItem 会把这一屏挪走，接着那一发刷新又按挪过的
    // 位置还原 —— 滚动就凭空跳了几行），直接走松开鼠标那一路的同一个落点。
    if (!viewport->rect().contains(cell)) {
        triggerRowAction(item->data(pathRole()).toString(), action);
        return true;
    }
    const QPoint pos = cell.center();
    const QPoint global = viewport->mapToGlobal(pos);
    QMouseEvent move(QEvent::MouseMove, pos, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
    QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &release);
    return true;
}

// ============================================================================
// 条目
// ============================================================================

QListWidgetItem *InstanceFilesPage::makeItem(const Entry &entry) {
    // DisplayRole 故意空着：带了字的话列表会按首字母跳行（keyboardSearch），原来那套行控件
    // 没有这回事。名字放在 AccessibleTextRole —— 读屏读它，代理也画它。
    auto *item = new QListWidgetItem();
    item->setData(pathRole(), entry.path);
    item->setData(disabledRole(), entry.disabled);
    item->setData(isDirRole(), entry.isDir);
    item->setSizeHint(QSize(0, MainWindowUI::instanceFileRowHeight()));
    // 名字尾巴上那颗 ↑（"这一条查出来有新版本"）是这一串的一部分：紧挨着名字、和名字一个
    // 字号，省略从中间截，尾巴上的 ↑ 和「（已禁用）」都留着。
    item->setData(Qt::AccessibleTextRole,
                  nameTextWithUpdateMark(entry.displayName, m_outdatedPaths.contains(entry.path)));
    // 整条路径挂在悬停提示上（行首图标和名字两格，见 RowDelegate::helpEvent）。
    item->setToolTip(QDir::toNativeSeparators(entry.path));

    // 「12.3 MB / 12:00」。文件夹没有有意义的大小（Windows 上 info.size() 对目录给的是
    // 目录项自己的字节数），给一个破折号比给一个假数字好；可编辑的目录（存档）递归统计，
    // 先写「正在统计…」，由 fillDirectoryStats() 一条一条补。
    QString meta;
    if (m_canEdit && entry.isDir) {
        if (const auto cached = m_statCache.constFind(entry.path); cached != m_statCache.constEnd()) {
            meta = cached.value();
        } else {
            meta = statsPendingText();
            m_statQueue.append(entry.path);
        }
    } else {
        meta = entry.isDir ? QStringLiteral("—") : formatSize(entry.size);
        if (entry.modified.isValid()) {
            meta += QStringLiteral(" / ") + entry.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        }
    }
    item->setData(metaRole(), meta);
    item->setData(Qt::AccessibleDescriptionRole, meta);

    // 行首图标：真图标已经回来了就直接摆那一张（`m_icons`），否则摆占位图 —— 「刷新」重列出来
    // 的那一行不该先退成占位图再等一次回答。颜色取次要那一档（这一列是配角）。
    if (m_rowIconsVisible && m_list != nullptr) {
        item->setData(Qt::DecorationRole,
                      iconForPath(entry.path, entry.isDir, m_list->palette().color(QPalette::PlaceholderText)));
    }

    // 开关：**开着 = 游戏扫得到它**（正常文件名）、关着 = 名字后面挂着 `.disabled`。方向不能反。
    // 只给"游戏会自己扫的那三个目录里的文件"（见 setCanToggle）。勾选位让读屏报出开 / 关。
    if (m_canToggle && !entry.isDir) {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(entry.disabled ? Qt::Unchecked : Qt::Checked);
    }
    return item;
}

void InstanceFilesPage::rebuildRows() {
    if (m_list == nullptr) {
        return;
    }
    const bool updatesEnabled = m_list->updatesEnabled();
    m_list->setUpdatesEnabled(false);
    // 把"现在最上面那一行是谁"记下来（见 .hpp 里那两个函数那段）。排在最前面 —— 下面
    // `clear()` 一过就什么都不剩了。`refresh()` 那条路（点一下开关走的就是它）**已经先记过
    // 一遍**，它那一趟到这里时表是空的，所以这个函数看见空表就不动手。
    rememberScrollAnchor();
    ensureStateFilterVisible();
    // 这一段（clear + 逐条加条目）**把信号挡着** —— 见 .hpp 里 m_selectedPaths 那段：
    // `clear()` 会发一次 itemSelectionChanged，按路径还的选择也会发。不挡的话那一发会拿
    // "还没排完的列表"去重算 m_selectedPaths，等于**把用户的选择在自己重列的过程中抹掉**。
    // ⚠ 挡住之后，重列末尾必须**自己**把账和那条操作条摆对（下面那两句）。
    //
    // 身份**只在"那一条刚刚改过名"时才拿来还选择**。身份是把 `.disabled` 抹掉算的（见
    // `identityPath`），于是 `X.jar` 和 `X.jar.disabled` **同时在场**时两行同身份 —— 重列不能
    // 把"用户只点了一条"的选择变成两条，否则紧接着一次「删除选中项」就把用户**从来没选过**的
    // 那一份也删掉了（不可逆）。两份准账，各管一件事：
    // · `presentPaths` —— 盘上还在的所有路径（**含被筛掉的**）。某个旧路径还在
    // ⇒ 那不是改名，是它暂时被筛出去了；
    // · `visibleIdentityCounts` —— 这一趟**看得见**的条目里每个身份有几条。不止一条
    // ⇒ 身份本身分不出"是哪一条"，不许拿它替用户选。
    QSet<QString> presentPaths;
    QHash<QString, int> visibleIdentityCounts;
    presentPaths.reserve(m_entries.size());
    for (const Entry &entry : m_entries) {
        presentPaths.insert(entry.path);
        if (entryMatchesFilter(entry)) {
            ++visibleIdentityCounts[identityPath(entry.path)];
        }
    }
    QSet<QString> renamedIdentities;
    renamedIdentities.reserve(m_selectedPaths.size());
    for (const QString &path : m_selectedPaths) {
        // 只有**这一页刚发出去的启用/禁用**（`m_pendingIdentityMigration`）才把"旧路径消失"
        // 解释成改名。删除 / 移动也是消失 —— 删除 `X.jar` 之后 `X.jar.disabled` 会自己变成
        // 选中的，下一次「删除选中」就删掉用户没选过的文件。
        if (!presentPaths.contains(path) && m_pendingIdentityMigration.contains(identityPath(path))) {
            renamedIdentities.insert(identityPath(path));
        }
    }
    // · `freshIdentities` —— 这一趟**新冒出来**的那几行（上一趟没摆过它们）；
    // · `shownIdentities` —— 这一趟摆出来的**全部**身份。**不能就地往 `m_shownIdentities`
    // 里写**：它是判"新不新"的那份底账，边判边写等于每一行都判成旧的。
    QSet<QString> freshIdentities;
    QSet<QString> shownIdentities;
    {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        m_statQueue.clear();
        m_statCache.clear();
        for (const Entry &entry : m_entries) {
            if (!entryMatchesFilter(entry)) {
                continue;
            }
            QListWidgetItem *item = makeItem(entry);
            m_list->addItem(item);
            const QString identity = identityPath(entry.path);
            shownIdentities.insert(identity);
            if (!m_shownIdentities.contains(identity)) freshIdentities.insert(identity);
            const bool keptSelected =
                m_selectedPaths.contains(entry.path)
                || (renamedIdentities.contains(identity) && visibleIdentityCounts.value(identity) == 1);
            if (m_list->selectionMode() != QAbstractItemView::SingleSelection && keptSelected) {
                item->setSelected(true);
            }
        }
    }
    // 「同页筛选/列表变化：只动画**新增**、删除和状态，**不重播整页**」那一半的正面：
    // · **只淡新冒出来的那几行**（`Anim::rowAppearMs()`，只有淡没有抬）；
    // · **整批换掉就不淡**：新来的超过 `rowAppearLimit()` ⇒ 一行都不淡；
    // · **第一趟不淡**：那是这一页头一次铺满，前头正跑着换页那一发（`m_rowsBuilt`）。
    // · 「删除」这一半**没做**：删掉的条目当场就不在了。
    m_fadeIdentities.clear();
    if (m_rowsBuilt && Anim::enabled() && !freshIdentities.isEmpty()
        && freshIdentities.size() <= Anim::rowAppearLimit()) {
        m_fadeIdentities = freshIdentities;
        m_fadeClock.start();
        if (m_fadeTimer != nullptr) m_fadeTimer->start();
    }
    m_shownIdentities = shownIdentities;
    m_rowsBuilt = true;

    // 这一趟已经消费掉那几张"刚点过开关"的身份了（见 .hpp 里 `m_pendingIdentityMigration`
    // 那段）。清掉 —— 下一次重列是别的原因（删除、搜索、统计回来了…），那时一个身份都不该
    // 拿"改过名"还选择。
    m_pendingIdentityMigration.clear();

    syncSelectedPaths();

    // 「目录本来就是空的」和「筛完一条不剩」是两件事（见 m_filteredEmpty）：前者底下那
    // 两排筛选整块收起来，后者留着让用户把条件改回来。
    m_filteredEmpty = !m_empty && m_list->count() == 0;

    fillStateButtons();
    // 可见性 + 底下那条悬浮操作条的显隐/灰化（applyState 末尾会喊 updateSelectionBar）。
    applyState();
    updateInfoPanel();

    // **滚回原来那个位置**、按光标重认悬停那一行，都排到下一趟事件循环：滚动条能滚多远、
    // 条目在哪儿，要等视图那一趟欠着的排版（Qt 自己用 0 毫秒定时器补，登记在我们前面）算完。
    QTimer::singleShot(0, this, [this] {
        restoreScrollAnchor();
        syncRowHover();
    });

    // 目录行的「大小 / 时间」要递归遍历才拿得到，**排在行都摆出来之后**（见 fillDirectoryStats）。
    if (m_statTimer != nullptr && !m_statQueue.isEmpty()) {
        m_statTimer->start();
    }
    m_list->setUpdatesEnabled(updatesEnabled);
    m_list->viewport()->update();
}

QPixmap InstanceFilesPage::iconForPath(const QString &path, bool isDir,
                                       const QColor &placeholderColor) const {
    // `contains` 而不是"取出来看不看是空"：**空 pixmap 也是一条回答**（"这一条问过了、
    // 没有"），它和"还没问"在屏幕上都是占位图，但只有前者不该再问一次（见 m_icons 那段）。
    const QString key = identityPath(path);
    if (m_icons.contains(key)) {
        const QPixmap icon = m_icons.value(key);
        if (!icon.isNull()) {
            // 缩到这一格的大小（见 scaleToIconSlot 那段）。`m_icons` 里存的是**原样那张**
            // —— 这一层不篡改别人给的东西，缩放只发生在"往条目上摆"这一步。
            return scaleToIconSlot(icon);
        }
    }
    // 判据是**这个目录里有没有那张图**，不是"这一页是不是存档页"：文件夹自带缩略图这个约定
    // 本来就是按目录说的。不缓存：每一趟 refresh() 只问一次，那张图十来 KB、decode 是微秒级。
    if (isDir) {
        const QPixmap own = directoryThumbnail(path);
        if (!own.isNull()) {
            return scaleToIconSlot(own);
        }
    }
    return placeholderIcon(isDir, placeholderColor);
}

bool InstanceFilesPage::setIconForPath(const QString &path, const QPixmap &icon) {
    if (path.isEmpty()) {
        return false;
    }
    // 这一页没有图标栏（见 setRowIconsVisible）：记也不用记 —— 没人会来读那一份账。
    if (!m_rowIconsVisible) {
        return false;
    }
    // **先记账、再找行**：这一条现在在不在屏幕上不影响"这一条问过了"。顺序反过来的话，
    // 图标回来的那一刻用户正好点了「刷新」，这一条就永远回到"还没问"——而 MainWindow 那边
    // 不会再问第二遍。
    m_icons.insert(identityPath(path), icon);
    // 按**路径**找回那一行（不按行号）：图标是一张一张异步回来的，这中间用户完全可能点过
    // 「刷新」、删过一条。找不到不是错误：那一行已经不在了。
    QListWidgetItem *item = itemForPath(path);
    if (item == nullptr) {
        return false;
    }
    // 空 pixmap = 这条查不到图标 ⇒ **退回占位图**（不是清成空白）。
    item->setData(Qt::DecorationRole, iconForPath(path, item->data(isDirRole()).toBool(),
                                                  m_list->palette().color(QPalette::PlaceholderText)));
    return true;
}

bool InstanceFilesPage::setResourceForPath(const QString &path, const QString &tip) {
    if (path.isEmpty()) {
        return false;
    }
    // 空 tip 有两种意思，**两种都要办**（见 .hpp 那段）：
    // ① 这一条**从来**没认出来过 ⇒ 什么都不用做（"点不动"是默认状态）；
    // ② 上一趟认出来了、这一趟没有（同一个路径上换了内容）⇒ 「详情」那一格必须从"点得动"
    // 退回"点不动"，否则看着能按、按下去什么都不发生。
    // 两件事靠 `remove()` 的返回值分开：删掉了 = 走过 ② 那一支。账按 `identityPath(path)` 记
    // （理由同 setIconForPath）—— 不然点一下「禁用」，那一行的详情会跟着变灰。
    const QString key = identityPath(path);
    const bool hadLink = m_resourceTips.remove(key) > 0;
    if (tip.isEmpty() && !hadLink) {
        return false;
    }
    // 和 setIconForPath() 同一个顺序、同一个理由：先记账、再找行。
    if (!tip.isEmpty()) {
        m_resourceTips.insert(key, tip);
    }
    QListWidgetItem *item = itemForPath(path);
    // 这一页没开批量（没有那一格）或者那一行已经不在了 —— 账已经记下，什么都不用做。
    if (item == nullptr || !m_batchMode) {
        return false;
    }
    m_list->viewport()->update(m_list->visualItemRect(item));
    return true;
}

void InstanceFilesPage::syncRowHover() {
    // 重列 / 滚动之后鼠标**可能没动过**，没有 MouseMove 可等：当场问一句光标底下是谁。
    // 只在光标真压在这一页的视口上时才重认（`widgetAt` 走真正的命中测试，光标在那条悬浮
    // 操作条上、在另一扇窗上都会被排除）；不在的话保持原样 —— 离开视口时 Leave 已经清过了。
    if (m_list == nullptr) {
        return;
    }
    QWidget *viewport = m_list->viewport();
    if (QApplication::widgetAt(QCursor::pos()) != viewport) {
        return;
    }
    const QPoint pos = viewport->mapFromGlobal(QCursor::pos());
    const QListWidgetItem *item = m_list->itemAt(pos);
    setHover(item != nullptr ? item->data(pathRole()).toString() : QString(), actionAt(pos));
}

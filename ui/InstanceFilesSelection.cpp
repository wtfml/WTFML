/**
 * @file InstanceFilesSelection.cpp
 * @brief File selection state, bulk commands, and action panel layout.
 * Scan results and row construction remain in InstanceFilesPage.cpp.
 */
#include "InstanceFilesPage.hpp"
#include "Animations.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"
#include "UpdateGlyph.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QPainter>
#include <QPainterPath>
#include <QIconEngine>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kSelectionBarMargin = 12;

QIcon panelActionIcon(int action, const QColor &color) {
    if (action == 2) return UpdateGlyph::icon(color);
    QPixmap pixmap(54, 54);
    pixmap.setDevicePixelRatio(3);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath path;
    switch (action) {
    case 0:
        path.moveTo(3, 9); path.lineTo(7, 13); path.lineTo(15, 5);
        break;
    case 1:
        painter.drawEllipse(QRectF(2, 2, 14, 14));
        path.moveTo(4, 4); path.lineTo(14, 14);
        break;
    case 3:
        painter.drawRoundedRect(QRectF(6, 6, 10, 10), 1.5, 1.5);
        path.moveTo(3, 12); path.lineTo(2, 12); path.lineTo(2, 2); path.lineTo(12, 2); path.lineTo(12, 3);
        break;
    case 4:
        path.moveTo(9, 6); path.lineTo(9, 3); path.lineTo(2, 3);
        path.lineTo(2, 15); path.lineTo(9, 15); path.lineTo(9, 12);
        path.moveTo(6, 9); path.lineTo(16, 9);
        path.moveTo(12, 5); path.lineTo(16, 9); path.lineTo(12, 13);
        break;
    case 5:
        path.moveTo(2, 5); path.lineTo(16, 5);
        path.moveTo(6, 5); path.lineTo(6, 2); path.lineTo(12, 2); path.lineTo(12, 5);
        path.moveTo(4, 5); path.lineTo(5, 16); path.lineTo(13, 16); path.lineTo(14, 5);
        path.moveTo(7, 8); path.lineTo(7, 13); path.moveTo(11, 8); path.lineTo(11, 13);
        break;
    case 7:
        painter.drawRoundedRect(QRectF(6, 1, 6, 5), 1, 1);
        painter.drawRoundedRect(QRectF(1, 12, 6, 5), 1, 1);
        painter.drawRoundedRect(QRectF(11, 12, 6, 5), 1, 1);
        path.moveTo(9, 6); path.lineTo(9, 9);
        path.moveTo(4, 12); path.lineTo(4, 9); path.lineTo(14, 9); path.lineTo(14, 12);
        break;
    case 8:
        painter.drawEllipse(QRectF(2, 2, 14, 14));
        painter.drawEllipse(QRectF(6, 2, 6, 14));
        path.moveTo(2, 9); path.lineTo(16, 9);
        break;
    case 9:
        path.moveTo(2, 15); path.lineTo(2, 3); path.lineTo(7, 3);
        path.lineTo(9, 5); path.lineTo(16, 5); path.lineTo(16, 15); path.closeSubpath();
        break;
    case 10:
        painter.drawRoundedRect(QRectF(3, 2, 12, 14), 1, 1);
        for (int y : {6, 9, 12}) { path.moveTo(6, y); path.lineTo(12, y); }
        break;
    case 12:  // 清除选择
        path.moveTo(4, 4); path.lineTo(14, 14);
        path.moveTo(14, 4); path.lineTo(4, 14);
        break;
    case 11:
        path.moveTo(3, 5); path.lineTo(15, 5);
        path.moveTo(12, 2); path.lineTo(15, 5); path.lineTo(12, 8);
        path.moveTo(15, 13); path.lineTo(3, 13);
        path.moveTo(6, 10); path.lineTo(3, 13); path.lineTo(6, 16);
        break;
    default:
        painter.setBrush(color);
        for (int x : {3, 9, 15}) painter.drawEllipse(QPointF(x, 9), 0.7, 0.7);
        break;
    }
    painter.drawPath(path);
    return QIcon(pixmap);
}

class PanelActionIconEngine final : public QIconEngine {
public:
    explicit PanelActionIconEngine(int action) : m_action(action) {}
    QIconEngine *clone() const override { return new PanelActionIconEngine(m_action); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        const auto colors = Theme::tokens();
        const QColor ink = mode == QIcon::Disabled ? colors.disabled
            : m_action == 5 ? Theme::textTone(colors.dangerInk, colors.danger)
            : m_action == 2 ? Theme::textTone(colors.accentInk, colors.primary) : colors.text;
        panelActionIcon(m_action, ink).paint(painter, rect, Qt::AlignCenter, QIcon::Normal, state);
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
private:
    int m_action;
};

/// 按钮带上 @p text 时有多宽（照 QPushButton::sizeHint 的算法，图标另加 4 像素间隔），不动按钮本身。
int labeledButtonWidth(const QPushButton *button, const QString &text) {
    QStyleOptionButton option;
    option.initFrom(button);
    option.text = text;
    option.icon = button->icon();
    option.iconSize = button->iconSize();
    QSize content = button->fontMetrics().size(Qt::TextShowMnemonic, text);
    if (!option.icon.isNull()) content.rwidth() += option.iconSize.width() + 4;
    return button->style()->sizeFromContents(QStyle::CT_PushButton, &option, content, button).width();
}

}

void InstanceFilesPage::setActionPanelEnabled(bool enabled) {
    // Opt-in only: the other file-management pages retain the floating bar over the list.
    if (!enabled || m_actionPanelEnabled) return;
    m_actionPanelEnabled = true;
    // 照 PCL 把操作排在列表正上方一行：没选中时是常用操作，选中后原地换成批量操作条。
    // 两页装在 QStackedWidget 里，换页走全仓统一的换页动效（DesignDefaults）；
    // 整页只剩列表一条竖滑条（#136）。横向按 Ignored：这一行再长也不顶宽整页。
    m_actionStack = new QStackedWidget(m_selectionTools);
    m_actionStack->setObjectName(QStringLiteral("modActionStack"));
    m_actionStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    auto *idle = new QWidget(m_actionStack);
    idle->setObjectName(QStringLiteral("modIdleActions"));
    auto *idleRow = new QHBoxLayout(idle);
    idleRow->setContentsMargins(0, 0, 0, 0);
    idleRow->setSpacing(8);
    m_pathRow->layout()->removeWidget(m_download);
    const bool downloadVisible = !m_download->isHidden();
    idleRow->addWidget(m_download);
    m_download->setVisible(downloadVisible);
    m_pathRow->layout()->removeWidget(m_openFolder);
    m_openFolder->setText(tr("打开文件夹"));
    Theme::markButton(m_openFolder, Theme::ButtonKind::Neutral);
    idleRow->addWidget(m_openFolder);
    m_openFolder->show();
    auto *headerMore = new QPushButton(tr("更多"), idle);
    headerMore->setObjectName(QStringLiteral("modHeaderMore"));
    Theme::markButton(headerMore, Theme::ButtonKind::Neutral);
    idleRow->addWidget(headerMore);
    connect(headerMore, &QPushButton::clicked, this, [this, headerMore] {
        QMenu menu(this);
        menu.addAction(VersionIcons::actionIcon(VersionIcons::Action::Refresh), tr("刷新列表"), m_refresh, &QPushButton::click);
        auto *copy = menu.addAction(panelActionIcon(3, Theme::tokens().text), tr("复制文件夹路径"));
        connect(copy, &QAction::triggered, this, [this] {
            QApplication::clipboard()->setText(m_directory);
        });
        menu.exec(headerMore->mapToGlobal(QPoint(0, headerMore->height())));
    });
    idleRow->addStretch(1);
    m_selectionSummary = new QLabel(idle);
    idleRow->addWidget(m_selectionSummary);

    // 选中后那一页：「已选 N 项」+ 批量操作 + 「清除选择」。浮条那份底色不要了（它现在排在行里）。
    m_selectionBar->setAutoFillBackground(false);
    delete m_selectionBar->layout();
    auto *bar = new QHBoxLayout(m_selectionBar);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(8);
    bar->addWidget(m_selectionCount);
    const auto markAction = [](QPushButton *button, int icon) {
        button->setProperty("actionLabel", button->text());
        button->setIcon(QIcon(new PanelActionIconEngine(icon)));
        button->setProperty("wtfmlModAction", true);
        Theme::markButton(button, icon == 5 ? Theme::ButtonKind::Danger
                                  : icon == 2 ? Theme::ButtonKind::Accent : Theme::ButtonKind::Neutral);
        button->setAccessibleName(button->text());
        button->setIconSize(QSize(18, 18));
        button->setMinimumWidth(0);
    };
    const QList<QPair<SelectionAction, int>> batch = {
        {SelectionAction::Enable, 0}, {SelectionAction::Disable, 1},
        {SelectionAction::Update, 2}, {SelectionAction::Delete, 5}};
    for (const auto &[action, icon] : batch) {
        auto *button = selectionButtonAt(static_cast<int>(action));
        markAction(button, icon);
        bar->addWidget(button);
    }
    auto *more = new QPushButton(tr("更多"), m_selectionBar);
    more->setObjectName(QStringLiteral("modMoreActions"));
    more->setToolTip(more->text());
    markAction(more, 6);
    bar->addWidget(more);
    connect(more, &QPushButton::clicked, this, [this, more] {
        const auto paths = selectedPathList();
        QMenu menu(this);
        const auto menuIcon = [](int action) { return panelActionIcon(action, Theme::tokens().text); };
        for (bool move : {false, true}) {
            auto *transfer = menu.addAction(menuIcon(move ? 4 : 3), move ? tr("移动到其它实例") : tr("复制到其它实例"));
            transfer->setObjectName(move ? QStringLiteral("modMoveSelection") : QStringLiteral("modCopySelection"));
            connect(transfer, &QAction::triggered, this, [this, paths, move] {
                emit transferSelectionRequested(paths, move);
            });
        }
        menu.addSeparator();
        auto *inspect = menu.addAction(menuIcon(7), tr("依赖与冲突"));
        inspect->setObjectName(QStringLiteral("modInspectDependencies"));
        inspect->setEnabled(!paths.isEmpty());
        connect(inspect, &QAction::triggered, this, [this, paths] { emit inspectDependenciesRequested(paths); });
        auto *detail = menu.addAction(VersionIcons::actionIcon(VersionIcons::Action::Details), tr("查看详情"));
        detail->setEnabled(paths.size() == 1 && m_resourceTips.contains(identityPath(paths.value(0))));
        connect(detail, &QAction::triggered, this, [this, paths] { emit detailRequested(paths.first()); });
        auto *website = menu.addAction(menuIcon(8), tr("打开项目网页"));
        website->setEnabled(detail->isEnabled());
        connect(website, &QAction::triggered, this, [this, paths] { emit projectWebsiteRequested(paths.first()); });
        auto *location = menu.addAction(menuIcon(9), tr("打开文件位置"));
        connect(location, &QAction::triggered, this, [paths] {
            if (!paths.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.first()).absolutePath()));
        });
        auto *exportList = menu.addAction(menuIcon(10), tr("复制选中模组清单"));
        connect(exportList, &QAction::triggered, this, [paths] {
            QStringList names; for (const auto &path : paths) names.append(QFileInfo(path).fileName());
            QApplication::clipboard()->setText(names.join(QLatin1Char('\n')));
        });
        auto *invert = menu.addAction(menuIcon(11), tr("反转每项启用状态"));
        connect(invert, &QAction::triggered, this, [this] {
            // Snapshot both subsets before either operation refreshes/renames rows.
            const auto disable = selectedTogglePaths(true);
            const auto enable = selectedTogglePaths(false);
            if (!disable.isEmpty()) requestToggle(disable, true);
            if (!enable.isEmpty()) requestToggle(enable, false);
        });
        menu.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    bar->addStretch(1);
    auto *clear = selectionButtonAt(static_cast<int>(SelectionAction::ClearAll));
    clear->setText(tr("清除选择"));
    markAction(clear, 12);
    bar->addWidget(clear);
    m_selectionBar->setToolTip(tr("选择仅针对当前筛选结果。删除模组不清理配置。"));

    m_actionStack->addWidget(idle);
    m_actionStack->addWidget(m_selectionBar);
    auto *tools = new QHBoxLayout(m_selectionTools);
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(8);
    tools->addWidget(m_actionStack, 1);
    for (auto action : {SelectionAction::SelectAll, SelectionAction::InvertAll})
        tools->addWidget(selectionButtonAt(static_cast<int>(action)));
    // Esc 先清选择（批量条收回常用操作），没有选择时才交给外层去「返回」。
    m_list->installEventFilter(this);
    m_search->setMinimumWidth(100);
    m_pathRow->hide();
    m_selectionTools->show();
    applyState();
    updateSelectionBar();
}

QStringList InstanceFilesPage::selectedPathList() const {
    // 走**列表里那一行的先后**，不是 `m_selectedPaths` 那个 QSet 的顺序：这是一份要交给
    // MainWindow 去逐条办事的名单，按屏幕上从上到下的次序报出去，出错时日志里那一串
    // 也跟用户看到的一样。不在表里的（理论上没有——见 rebuildRows 那段收窄）跳过。
    QStringList paths;
    if (m_list != nullptr) {
        for (int row = 0; row < m_list->count(); ++row) {
            QListWidgetItem *item = m_list->item(row);
            if (item == nullptr || !item->isSelected()) {
                continue;
            }
            const QString path = item->data(pathRole()).toString();
            if (!path.isEmpty()) {
                paths.append(path);
            }
        }
    }
    return paths;
}

QStringList InstanceFilesPage::selectedTogglePaths(bool disable) const {
    QStringList paths;
    if (!m_canToggle) return paths;
    QSet<QString> eligible;
    for (const Entry &entry : m_entries) {
        if (!entry.isDir && entry.disabled != disable) eligible.insert(entry.path);
    }
    for (const QString &path : selectedPathList()) {
        if (eligible.contains(path)) paths.append(path);
    }
    return paths;
}

void InstanceFilesPage::updateSelectionBar() {
    if (m_selectionBar == nullptr) {
        return;
    }
    // 露出来的条件**三条一起**：开了批量、选中了至少一条、列表正露着。
    // 最后那一条不是多余的：目录空 / 这一页用不上时列表整块收起来了，而"选中"这件事
    // 还压在 m_selectedPaths 上（我**不清它** —— 用户切一眼目录再切回来，选择该还在）。
    const bool show = m_batchMode && !m_selectedPaths.isEmpty() && m_list->isVisibleTo(this);

    // Count both target states once. The visible selection is authoritative;
    // hidden/filtered rows and directories must never enter a bulk operation.
    int enableCount = 0;
    int disableCount = 0;
    if (m_canToggle) {
        const QStringList selected = selectedPathList();
        const QSet<QString> paths(selected.cbegin(), selected.cend());
        for (const Entry &entry : m_entries) {
            if (!entry.isDir && paths.contains(entry.path)) {
                if (entry.disabled) ++enableCount;
                else ++disableCount;
            }
        }
    }

    // ---------- 灰化规则（见 .hpp 里那段）----------
    bool anyOutdated = false;
    for (const QString &path : m_selectedPaths) {
        if (m_outdatedPaths.contains(path)) {
            anyOutdated = true;
        }
    }
    const auto buttonFor = [this](SelectionAction action) {
        return selectionButtonAt(static_cast<int>(action));
    };
    const auto setEnabled = [&buttonFor](SelectionAction action, bool on) {
        if (QPushButton *button = buttonFor(action)) {
            button->setEnabled(on);
        }
    };
    setEnabled(SelectionAction::Update, anyOutdated);
    // 「禁用 / 启用」两颗在**没有开关的那一页**（数据包）上不是灰的 —— 是**藏着**
    // （见 .hpp 里 SelectionAction 那段的 NOTE）。所以这里先按"这一页有没有开关"决定摆不摆，
    // 再决定灰不灰。
    //
    // 这里的规矩和这一条上其余每一颗**逐字同一条**：摆着，不适用就灰着。
    // 选中的里面一条禁用着的都没有时，「启用」也仍然摆着（只是灰着）—— 整颗不见会让
    // 用户以为这一页根本没有启用这个功能。
    //
    // NOTE: 判据取的是选中且确实需要改变状态的文件，而不是整页的条数
    // （`stateFilterCount`，那是顶上那四颗筛选按钮的口径）—— 这条操作条办的一直是
    // "选中的这些"，它上面每一颗的灰化也是同一个口径。
    if (QPushButton *button = buttonFor(SelectionAction::Disable)) {
        const int affected = disableCount;
        button->setVisible(m_canToggle);
        button->setEnabled(affected > 0);
        button->setToolTip(tr("禁用 %1 个文件，%2 项不变。\n%3")
            .arg(affected).arg(m_selectedPaths.size() - affected).arg(selectionToggleTip(true)));
    }
    if (QPushButton *button = buttonFor(SelectionAction::Enable)) {
        const int affected = enableCount;
        button->setVisible(m_canToggle);
        button->setEnabled(affected > 0);
        button->setToolTip(tr("启用 %1 个文件，%2 项不变。\n%3")
            .arg(affected).arg(m_selectedPaths.size() - affected).arg(selectionToggleTip(false)));
    }
    // 全选 / 反选 / 取消 / 删除四颗恒可用（它们要么只动"选中的集合"，要么在按下去之后
    // 还会问一句 —— 灰着反而让用户以为删不了）。
    // NOTE: 「删除」也**选中了才在**（它在这条上，而这条只在有选中时露出来）。

    if (m_selectionCount != nullptr) {
        m_selectionCount->setText(selectionCountText(m_selectedPaths.size()));
    }
    if (m_actionPanelEnabled) {
        const int count = m_selectedPaths.size();
        m_selectionSummary->setText(tr("共 %1 个模组").arg(m_list->count()));
        setEnabled(SelectionAction::ClearAll, count > 0);
        QWidget *target = show ? m_selectionBar : m_actionStack->widget(0);
        if (m_actionStack->currentWidget() != target) {
            // 焦点在要收起的那一页上（键盘点了「清除选择」「删除」）就交回列表，不丢到窗口上。
            QWidget *focus = QApplication::focusWidget();
            const bool refocus = focus != nullptr && m_actionStack->currentWidget()->isAncestorOf(focus);
            m_actionStack->setCurrentWidget(target);
            if (refocus) m_list->setFocus(Qt::OtherFocusReason);
        }
        repositionSelectionBar();
        return;
    }
    m_selectionBar->setVisible(show);
    if (show) {
        m_selectionBar->raise();
        repositionSelectionBar();
    }
}

void InstanceFilesPage::repositionSelectionBar() {
    if (m_actionPanelEnabled) {
        // 批量操作带字排得下就带字，排不下收成图标。按「带字时」的宽度判，和此刻是哪一档无关，
        // 改字之后再排一轮也得出同一个结论，不会来回翻。
        const int available = m_contentHost->width() - Theme::scrollBarGutterInset();
        int trailing = 0;
        for (auto action : {SelectionAction::SelectAll, SelectionAction::InvertAll})
            trailing += m_selectionTools->layout()->spacing() + selectionButtonAt(static_cast<int>(action))->sizeHint().width();
        const auto buttons = m_selectionBar->findChildren<QPushButton *>();
        int labeled = m_selectionBar->sizeHint().width();
        for (auto *button : buttons) {
            const QVariant label = button->property("actionLabel");
            if (label.isValid()) labeled += labeledButtonWidth(button, label.toString()) - button->sizeHint().width();
        }
        const bool compact = labeled + trailing > available;
        if (compact != m_actionCompact) {
            m_actionCompact = compact;
            for (auto *button : buttons) {
                const QVariant label = button->property("actionLabel");
                if (!label.isValid()) continue;
                button->setText(compact ? QString() : label.toString());
            }
        }
        // 常用操作那一页窄了先收起「共 N 个模组」（筛选胶囊上已有同一个数）。
        QWidget *idle = m_actionStack->widget(0);
        int idleNeed = idle->sizeHint().width();
        if (m_selectionSummary->isHidden())
            idleNeed += m_selectionSummary->sizeHint().width() + idle->layout()->spacing();
        m_selectionSummary->setVisible(idleNeed + trailing <= available);
        return;
    }
    if (m_selectionBar == nullptr || m_list == nullptr) {
        return;
    }
    QWidget *viewport = m_list->viewport();
    if (viewport == nullptr) {
        return;
    }
    // 位置**自己算**：这条操作条不进布局（进了布局就变成"列表下面另起一行"，而用户要
    // 的是"浮在列表上"，见 .hpp 里 eventFilter 那段）。算的是**居中 + 离下缘
    // kSelectionBarMargin**：PCL 那一条就在这个位置。
    // NOTE: 用 `sizeHint()` 而不是 `width()/height()`：它这会儿可能还没被 show 过
    // （geometry 是 0×0），而 sizeHint 是布局现算的，布局一算出来就是对的。
    // 文本刚改过时 sizeHint 可能还没按新内容算 —— 先激活内部布局再量，
    // 否则第一条那一下会先按旧高度摆一次、再被下一次重算拉回来。
    if (m_selectionBar->layout() != nullptr) m_selectionBar->layout()->activate();
    const QSize hint = m_selectionBar->sizeHint();
    const int roomiest = qMax(0, viewport->width() - 2 * kSelectionBarMargin);
    const int width = roomiest > 0 ? qMin(hint.width(), roomiest) : hint.width();
    const QPoint origin = viewport->pos();
    const int x = origin.x() + (viewport->width() - width) / 2;
    const int y = origin.y() + viewport->height() - hint.height() - kSelectionBarMargin;
    // 横竖都夹在 [0, …] 里：列表窄/矮的那一档（窗口拉到最小）会算出负数，摆到负坐标上
    // 就是"这一条被窗口左边切掉半截"。
    m_selectionBar->setGeometry(qMax(0, x), qMax(0, y), width, hint.height());
}

int InstanceFilesPage::possibleStatePillsWidth() const {
    const int spacing = m_stateButtonsRow->layout()->spacing();
    int width = 0;
    for (int i = 0; i < m_stateButtons.size(); ++i) {
        const auto filter = static_cast<StateFilter>(i);
        if (filter == StateFilter::Update && !m_canFilterUpdates) continue;
        if ((filter == StateFilter::Enabled || filter == StateFilter::Disabled) && !m_canToggle) continue;
        width += m_stateButtons.at(i)->sizeHint().width() + spacing;
    }
    return width;
}

bool InstanceFilesPage::placeCompactStateRow() {
    if (!(m_compactHeader || m_actionPanelEnabled) || !m_batchMode || m_worldLayout) return false;
    auto *search = qobject_cast<QHBoxLayout *>(m_searchRow->layout());
    auto *outer = qobject_cast<QVBoxLayout *>(layout());
    // 搜索框留出它自己 sizeHint 那么宽，剩下的装得下全部胶囊才同排（和模组页计数排同一判据）。
    const int needed = m_search->sizeHint().width() + search->spacing() + possibleStatePillsWidth() + 24;
    const bool inlineRow = m_contentHost->width() - Theme::scrollBarGutterInset() >= needed;
    if (inlineRow == (m_stateRow->parentWidget() == m_searchRow)) return false;
    if (inlineRow) {
        outer->removeWidget(m_stateRow);
        search->insertWidget(search->indexOf(m_search) + 1, m_stateRow);
    } else {
        search->removeWidget(m_stateRow);
        outer->insertWidget(outer->indexOf(m_structureRows) + 1, m_stateRow);
    }
    return true; // 换了父控件会被藏起来，显隐交给 applyState() 重摆
}

void InstanceFilesPage::alignFixedRows() {
    auto *outer = qobject_cast<QVBoxLayout *>(layout());
    if (outer == nullptr) return;
    const auto setRight = [](QLayout *row, int right) {
        if (row == nullptr || row->contentsMargins().right() == right) return;
        QMargins margins = row->contentsMargins();
        margins.setRight(right);
        row->setContentsMargins(margins);
    };
    const int inset = Theme::scrollBarGutterInset();
    for (int i = 0; i < outer->count(); ++i) {
        QLayoutItem *item = outer->itemAt(i);
        if (item->layout() != nullptr) {
            setRight(item->layout(), inset);
        } else if (QWidget *row = item->widget(); row != nullptr && row != m_contentHost) {
            if (row->layout() != nullptr) {
                setRight(row->layout(), inset);
            } else if (qobject_cast<QLabel *>(row) != nullptr && row->contentsMargins().right() != inset) {
                // 只给本身没有布局的提示文字设控件边距：还没建布局的行（如操作行）以后建了布局
                // 会再按布局让一次，这里先设就成了让两次。
                QMargins margins = row->contentsMargins();
                margins.setRight(inset);
                row->setContentsMargins(margins);
            }
        }
    }
    // 状态胶囊并进搜索行时，右边已由搜索行让过，自己不再让一遍。
    if (m_stateRow != nullptr && m_stateRow->parentWidget() == m_searchRow) setRight(m_stateRow->layout(), 0);
}

QPushButton *InstanceFilesPage::selectionButtonAt(int action) const {
    if (action < 0 || action >= m_selectionButtons.size()) {
        return nullptr;
    }
    return m_selectionButtons.at(action);
}

QRect InstanceFilesPage::selectionBarGeometry() const {
    return m_selectionBar != nullptr ? m_selectionBar->geometry() : QRect();
}

void InstanceFilesPage::requestToggle(const QString &path) {
    if (path.isEmpty()) {
        return;
    }
    // 登记要在 emit **之前**：下面那一发是直接连接，处理函数里就会同步刷新 + 重列，
    // 重列读完这份登记才轮到我们回到这儿。
    m_pendingIdentityMigration.insert(identityPath(path));
    emit toggleRequested(path);
}

void InstanceFilesPage::requestToggle(const QStringList &paths, bool disable) {
    if (paths.isEmpty()) {
        return;
    }
    for (const QString &path : paths) {
        m_pendingIdentityMigration.insert(identityPath(path));
    }
    emit batchToggleRequested(paths, disable);
}

void InstanceFilesPage::syncSelectedPaths() {
    // 「选中了哪几条」的账**只从列表里现在真选着的那些行问一遍** —— 见 .hpp 里
    // m_selectedPaths 那段：它是那一位的唯一出处（`itemSelectionChanged` 里那一支是
    // 同一个口径，只不过那一支是"用户点了"，这一支是"我们重列完了"）。
    // 重列那段把信号挡着，所以重列之后必须**自己**把账摆对 —— 就是这一句。
    m_selectedPaths.clear();
    // 没开批量管理但列表允许多选（存档 / 原理图）也记账：重列之后按路径把选择还回去。
    if (m_list == nullptr || m_list->selectionMode() == QAbstractItemView::SingleSelection) {
        return;
    }
    // 选中底和选中墨都由 RowDelegate 按 State_Selected 画；挡着信号重列时视图不一定自己重画。
    m_list->viewport()->update();
    const QList<QListWidgetItem *> selected = m_list->selectedItems();
    for (QListWidgetItem *item : selected) {
        if (item == nullptr) {
            continue;
        }
        const QString path = item->data(pathRole()).toString();
        if (!path.isEmpty()) {
            m_selectedPaths.insert(path);
        }
    }
}

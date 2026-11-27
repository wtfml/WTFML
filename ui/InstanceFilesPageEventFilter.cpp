/**
 * @file InstanceFilesPageEventFilter.cpp
 * @brief 这一页上的事件路由：浮条重定位、行上的悬停 / 点格子 / 右键进详情页、拖出与框选。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一页的行是**代理画的**（RowDelegate），行尾那几格不是控件，点不点中、悬停在哪一格都得
 * 自己按坐标算；底下那条悬浮操作条又**不在布局里**（它压在列表上）。于是有一批位置和悬停
 * 状态没有别人帮着算，全在这支过滤器里：
 *
 * - 页面 / 列表视口变大小 ⇒ 重新摆那条浮条（它在布局外，没有人会替它重排）；
 * - 鼠标在视口上移动 / 离开 ⇒ 记下压着哪一行哪一格（悬停底、「详情」露不露、垃圾桶变红）；
 * - 左键按在行尾某一格上 ⇒ 吃掉（不改选择、不起拖动），松开时还在同一格才算点；
 * - 行上右键 ⇒ 打开这一条的详情页，**且保留上一页的选择**。
 *
 * 【几条踩过的坑】
 *
 * - 右键那一发**必须吃掉、而且 `accept()` 不能省**：Qt 那个"被忽略了就往上冒"的循环判的
 *   是 `res && isAccepted()`，只 `return true` 的话这一发照样会被送到视图上；
 * - `path` 必须在 `emit` **之前**取出来：接的人会把这一页收掉去开详情页，回来时
 *   这一行说不定已经不在了。
 */

#include "InstanceFilesPage.hpp"

#include "InstanceFilesPageInternals.hpp"

#include <QEvent>
#include <QListWidget>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QApplication>
#include <QDrag>
#include <QFileInfo>
#include <QMimeData>
#include <QRubberBand>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QWidget>

bool InstanceFilesPage::eventFilter(QObject *watched, QEvent *event) {
    // 模组页：Esc 先清掉选择（批量操作条收回常用操作），没有选择时照常冒给外层去「返回」。
    if (watched == m_list && event->type() == QEvent::KeyPress && m_actionPanelEnabled &&
        !m_selectedPaths.isEmpty()) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            m_list->clearSelection();
            return true;
        }
    }
    if (watched == m_contentHost && event->type() == QEvent::Resize) {
        repositionSelectionBar();
        if (placeCompactStateRow()) applyState();
    }
    // 列表的 viewport 变大小了 ⇒ 底下那条悬浮操作条要重算位置（见 .hpp 里那段）。
    //
    // NOTE: 那条操作条**不在布局里**（在布局里就成了"列表下面另起一行"，而用户要的是
    // "浮在列表上"，见 repositionSelectionBar）—— 所以它的位置没有人帮着算，
    // 只有这一发能告诉我们"该算了"。
    // NOTE: `watched == m_list->viewport()` 那一句里 `viewport()` 可能为 null（这一页还没
    // 建到那一步），所以先判 m_list。
    if (m_list != nullptr && event->type() == QEvent::Resize &&
        watched == m_list->viewport()) {
        repositionSelectionBar();
    }

    // ---------- 行上的鼠标（行是画的，全在视口这一处） ----------
    if (m_list != nullptr && watched == m_list->viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
            const QListWidgetItem *item = m_list->itemAt(pos);
            setHover(item != nullptr ? item->data(pathRole()).toString() : QString(), actionAt(pos));
            // 按住一格拖开：不交给视图去拖选 / 起拖动（那一格按下去时本来就没改选择）。
            if (m_pressAction != RowAction::None) {
                m_list->viewport()->update();
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::Leave:
            setHover(QString(), RowAction::None);
            break;
        case QEvent::MouseButtonPress: {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const QPoint pos = mouse->position().toPoint();
            int row = -1;
            const RowAction action = actionAt(pos, &row);
            const QListWidgetItem *item = row >= 0 ? m_list->item(row) : nullptr;
            const QString path = item != nullptr ? item->data(pathRole()).toString() : QString();
            if (mouse->button() == Qt::RightButton) {
                // 右键这一发 = 开这一条的详情页，并**保留上一页的选择**。**只认"认得出资源"的
                // 那些** —— 判据和行尾「详情」那一格逐字相同（`m_resourceTips`）。
                // 必须吃掉：视图收到右键会先把鼠标底下那一条选中。别的行 / 空白处照旧给右键菜单。
                if (m_batchMode && !path.isEmpty() && m_resourceTips.contains(identityPath(path))) {
                    emit detailRequested(path);
                    event->accept();
                    return true;
                }
                break;
            }
            // 「详情」藏着的时候不算一格（只在悬停那一行画）：按在那里就是按在行身上。
            if (mouse->button() == Qt::LeftButton && action != RowAction::None
                && (action != RowAction::Detail || rowDetailShown(row))) {
                if (actionEnabled(item, action)) {
                    m_pressAction = action;
                    m_pressActionPath = path;
                    m_list->viewport()->update(m_list->visualItemRect(item));
                }
                // 点不动的那一格也吃掉：原来那颗灰按钮按下去同样什么都不发生（不改选择）。
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            if (m_pressAction == RowAction::None) break;
            const RowAction pressed = m_pressAction;
            const QString path = m_pressActionPath;
            m_pressAction = RowAction::None;
            m_pressActionPath.clear();
            m_list->viewport()->update();
            int row = -1;
            const RowAction now = actionAt(static_cast<QMouseEvent *>(event)->position().toPoint(), &row);
            const QListWidgetItem *item = row >= 0 ? m_list->item(row) : nullptr;
            event->accept();
            // 松开时还在同一行同一格才算点了一下（和真按钮一样：按下后拖出去再松 = 不算）。
            if (now == pressed && item != nullptr && item->data(pathRole()).toString() == path)
                triggerRowAction(path, pressed);
            return true;
        }
        case QEvent::MouseButtonDblClick:
            // 连点两下开关 / 垃圾桶不该顺带「打开」这一条（itemActivated）。
            if (actionAt(static_cast<QMouseEvent *>(event)->position().toPoint()) != RowAction::None) {
                event->accept();
                return true;
            }
            break;
        default:
            break;
        }
    }
    // ---------- 拖出 / Ctrl 框选取反 ----------
    {
        // 按在行尾那几格上的那一发上面已经吃掉了，走到这里的都是按在行身 / 空白处。
        const bool fromViewport = m_list != nullptr && watched == m_list->viewport();
        if (fromViewport) {
            switch (event->type()) {
            case QEvent::MouseButtonPress: {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() != Qt::LeftButton) break;
                const QPoint pos =
                    m_list->viewport()->mapFromGlobal(mouse->globalPosition().toPoint());
                QString path;
                if (const QModelIndex index = m_list->indexAt(pos); index.isValid()) {
                    if (const QListWidgetItem *item = m_list->item(index.row()))
                        path = item->data(pathRole()).toString();
                }
                m_pressViewportPos = pos;
                m_pressPath = path;
                m_pressOnRow = !path.isEmpty();
                m_dragStarted = false;
                m_ctrlDragActive = mouse->modifiers().testFlag(Qt::ControlModifier);
                if (m_ctrlDragActive && m_list != nullptr) m_list->setAutoScroll(false);
                m_bandToggledIdentities.clear();
                m_pressSelection.clear();
                for (const QListWidgetItem *selectedItem : m_list->selectedItems())
                    m_pressSelection.append(selectedItem->data(pathRole()).toString());
                if (!m_pressPath.isEmpty()) m_bandToggledIdentities.insert(identityPath(m_pressPath));
                // 已选行的普通点击延迟到 release：单选可取消，多选可收缩，拖拽则保留原选择。
                bool pathSelected = false;
                for (const QString &selectedPath : m_pressSelection)
                    if (selectedPath == path) pathSelected = true;
                m_swallowPress = pathSelected && (m_batchMode || m_pressSelection.size() > 1)
                    && !(mouse->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
                if (m_swallowPress) {
                    if (QListWidgetItem *pressedItem = m_list->itemAt(pos))
                        m_list->setCurrentItem(pressedItem, QItemSelectionModel::NoUpdate);
                }
                qInfo() << "[drag] press path=" << path << "swallow=" << m_swallowPress;
                break;
            }
            case QEvent::MouseMove: {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (!(mouse->buttons() & Qt::LeftButton)) break;
                const QPoint pos =
                    m_list->viewport()->mapFromGlobal(mouse->globalPosition().toPoint());
                const DragIntent intent = dragIntent(
                    (pos - m_pressViewportPos).manhattanLength(),
                    QApplication::startDragDistance(), m_pressOnRow, m_ctrlDragActive);
                if (!m_dragStarted && intent != DragIntent::None) {
                    m_dragStarted = true;
                    if (intent == DragIntent::ToggleSelection) updateToggleBand(pos, true);
                    else {
                        beginFileDrag();
                        // 原生拖放返回后，这一发 move 已经过期，不能再让视图据此重新拖选。
                        event->accept();
                        return true;
                    }
                } else if (m_dragStarted && m_ctrlDragActive) {
                    updateToggleBand(pos, false);
                }
                // 自定义取反框选已处理这一帧，不能再交给 Qt 的默认拖选改变选择与滚动。
                if (m_dragStarted && m_ctrlDragActive) {
                    event->accept();
                    return true;
                }
                break;
            }
            case QEvent::MouseButtonRelease: {
                if (m_toggleBand != nullptr) {
                    m_toggleBand->hide();
                    m_toggleBand->deleteLater();
                    m_toggleBand = nullptr;
                }
                const bool wasBandDrag = m_ctrlDragActive;
                if (m_list != nullptr) m_list->setAutoScroll(true);
                const bool dragged = m_dragStarted;
                const bool handledClick = m_swallowPress && !dragged && !m_pressPath.isEmpty();
                if (handledClick) {
                    {
                        const QSignalBlocker blocker(m_list);
                        m_list->clearSelection();
                        if (m_pressSelection.size() > 1) {
                            for (int row = 0; row < m_list->count(); ++row) {
                                QListWidgetItem *item = m_list->item(row);
                                if (item != nullptr && item->data(pathRole()).toString() == m_pressPath)
                                    item->setSelected(true);
                            }
                        }
                    }
                    syncSelectedPaths();
                    updateSelectionBar();
                    updateInfoPanel();
                    updateJoinEnabled();
                }
                m_dragStarted = false;
                m_ctrlDragActive = false;
                m_pressOnRow = false;
                m_bandToggledIdentities.clear();
                if (wasBandDrag && m_list != nullptr) {
                    QTimer::singleShot(0, this, [this] {
                        if (m_list == nullptr) return;
                        rebuildRows();
                        m_list->viewport()->update();
                    });
                }
                m_swallowPress = false;
                if (handledClick) {
                    event->accept();
                    return true;
                }
                break;
            }
            default:
                break;
            }
            // 延迟选择只接管鼠标输入；原生拖放期间仍必须放行 Paint、Resize 等视口事件。
            if (m_swallowPress && (event->type() == QEvent::MouseButtonPress ||
                                   event->type() == QEvent::MouseMove ||
                                   event->type() == QEvent::MouseButtonRelease)) {
                event->accept();
                return true;
            }

        }
    }
    return QWidget::eventFilter(watched, event);
}
void InstanceFilesPage::beginFileDrag() {
    // 拖哪些：按在"已经选中的那一条"上 ⇒ 拖整份多选；按在别的条上 ⇒ 只拖这一条。
    // Qt 处理 press 时会把多选收缩成单条，所以要用按下那一刻的快照（m_pressSelection）。
    QStringList paths;
    if (!m_pressPath.isEmpty() && m_pressSelection.contains(m_pressPath)) {
        paths = m_pressSelection;
    } else if (!m_pressPath.isEmpty()) {
        paths = {m_pressPath};
    } else {
        for (const QListWidgetItem *item : m_list->selectedItems())
            paths.append(item->data(pathRole()).toString());
    }
    paths.removeAll(QString());
    paths.removeDuplicates();
    QList<QUrl> urls;
    for (const QString &path : paths) {
        if (QFileInfo::exists(path)) urls.append(QUrl::fromLocalFile(path));
    }
    qInfo() << "[drag] start urls=" << urls.size() << "press=" << m_pressPath;

    if (urls.isEmpty()) return;
    const QStringList selectionBeforeDrag = m_pressSelection;
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    auto *drag = new QDrag(this);
    // 原生拖放有自己的事件循环；保留列表重绘，避免鼠标未释放时露出空白行。
    const bool autoScroll = m_list->hasAutoScroll();
    m_list->setAutoScroll(false);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction);
    m_list->setAutoScroll(autoScroll);
    {
        const QSignalBlocker blocker(m_list);
        const QSet<QString> selectedPaths(selectionBeforeDrag.cbegin(), selectionBeforeDrag.cend());
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem *item = m_list->item(i);
            item->setSelected(selectedPaths.contains(item->data(pathRole()).toString()));
        }
    }
    syncSelectedPaths();
    updateSelectionBar();
    updateInfoPanel();
    updateJoinEnabled();
    // 原生拖放可能吞掉 release；在这里清理按下状态，避免下一次点击沿用旧选择。
    m_swallowPress = false;
    m_dragStarted = false;
    m_ctrlDragActive = false;
    m_pressOnRow = false;
    m_pressSelection.clear();
    m_pressPath.clear();
    if (m_toggleBand != nullptr) {
        m_toggleBand->hide();
        m_toggleBand->deleteLater();
        m_toggleBand = nullptr;
    }
    qInfo() << "[drag] end count=" << m_list->count()
            << "current=" << m_list->currentRow() << "selected=" << m_list->selectedItems().size()
            << "listVis=" << m_list->isVisible() << "vpVis=" << m_list->viewport()->isVisible()
            << "vp=" << m_list->viewport()->size()
            << "item0=" << (m_list->item(0) != nullptr
                                ? m_list->visualItemRect(m_list->item(0))
                                : QRect());
    if (m_list != nullptr) {
        // 不要 doItemsLayout()：它会按滚动位置 0 重排、把列表跳回顶上。让 Qt 自己按当前滚动位置收敛即可。
        m_list->viewport()->update();
    }
}

void InstanceFilesPage::updateToggleBand(const QPoint &viewportPos, bool firstUpdate) {
    Q_UNUSED(firstUpdate);
    if (m_toggleBand == nullptr) {
        m_toggleBand = new QRubberBand(QRubberBand::Rectangle, m_list->viewport());
        m_toggleBand->setGeometry(QRect(m_pressViewportPos, QSize()));
        m_toggleBand->show();
    }
    // 一行只切换一次：滑回来**不恢复**（用户要的是“碰到过就算改过”）。
    const QRect rect = QRect(m_pressViewportPos, viewportPos).normalized();
    m_toggleBand->setGeometry(rect);
    // 但每帧都把已经切换过的行强制摆回切换后的状态 —— 否则滚动/自动滚动之类的
    // 视图动作会把最远那一条又选回去。
    const QSignalBlocker selectionSignals(m_list);
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (item == nullptr) continue;
        const QString path = item->data(pathRole()).toString();
        const QString identity = identityPath(path);
        if (m_list->visualItemRect(item).intersects(rect) &&
            !m_bandToggledIdentities.contains(identity)) {
            m_bandToggledIdentities.insert(identity);
        }
        if (m_bandToggledIdentities.contains(identity)) {
            const bool originallySelected = m_pressSelection.contains(path);
            item->setSelected(!originallySelected);
        }
    }
    syncSelectedPaths();
    updateSelectionBar();
    updateInfoPanel();
    updateJoinEnabled();
}

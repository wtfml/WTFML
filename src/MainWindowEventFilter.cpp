/**
 * @file MainWindowEventFilter.cpp
 * @brief 行里那些小按钮的事件路由：视口上的 `eventFilter()`。
 *
 * 【为什么单独一个翻译单元】
 *
 * 界面里有一批按钮不是 QWidget，而是代理**画在行里的一格**（加载器那行的「取消」叉、
 * 组行的齿轮、实例行的齿轮 / 置顶 / 垃圾桶、下载页分类行尾的刷新圈、资源行尾的箭头）。
 * 它们全靠这一支过滤器认"点中了没"：判据用的是代理**画它们时用的同一个矩形函数**，
 * 差一个像素就是"看着点的是垃圾桶、其实点的是那一行"。这一整族判据放在一个文件里，
 * 加一格按钮时不必在别处翻。
 *
 * 【两条反复出现的规矩】
 *
 * 一、**按下就动手**，松开与那几发双击只负责被吃掉 —— 跟普通按钮一样，按下去就该有反应；
 * 二、**双击也要接**：点得快时第二发及以后到的是 `MouseButtonDblClick`，不接就漏给
 * "点中了这一行"那条路（连点垃圾桶的第二下变成进实例设置）。
 *
 * 【判据不在这一层】
 *
 * 该画在哪一格在 `VersionTreeDelegate` / `InstanceGroupDelegate`；点了算"哪一件事"由
 * `MainWindow` 的槽做（`openInstanceSettings()` / `pinInstance()` / `unpinInstance()` /
 * `confirmDeleteInstance()` / `refreshDownloadCategory()` / `openResourceDetailPage()`）。
 */

#include "MainWindow.hpp"

#include "..\ui\InstanceGroupDelegate.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "..\ui\OverlayHost.hpp"
#include "..\ui\Theme.hpp"
#include "..\ui\VersionTreeDelegate.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/InstallPageController.hpp"
#include "download/ResourceCatalog.hpp"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QTreeWidget>

#include <limits>

namespace {

/// 焦点落在这些控件上时，回车归**它自己**（输入框提交、按钮按下、列表展开…），
/// 窗口级那一发不抢。焦点在窗口本身（或压根没有焦点）才算"没有交互焦点"。
bool keyboardFocusTakesEnter(QWidget *focus) {
    if (focus == nullptr || focus->isWindow()) {
        return false;
    }
    return qobject_cast<QAbstractButton *>(focus) != nullptr ||
           qobject_cast<QAbstractSlider *>(focus) != nullptr ||
           qobject_cast<QAbstractSpinBox *>(focus) != nullptr ||
           qobject_cast<QComboBox *>(focus) != nullptr ||
           qobject_cast<QLineEdit *>(focus) != nullptr ||
           qobject_cast<QAbstractScrollArea *>(focus) != nullptr;
}

}  // namespace

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // ---------- 主页回车 = 点「启动游戏」 ----------
    // 判据全部在 `handleHomeEnter()` 里（主页是否露着、有没有浮层、焦点在谁身上）。
    if (watched == this && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (!key->isAutoRepeat() && key->modifiers() == Qt::NoModifier) {
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && handleHomeEnter()) {
                key->accept();
                return true;
            }
            // ---------- 启动中 Esc = 取消启动 ----------
            if (key->key() == Qt::Key_Escape && handleEscCancelLaunch()) {
                key->accept();
                return true;
            }
        }
    }
    // 安装页那棵树上**唯一一个"行里的按钮"**：选中那条加载器右边的「取消」小叉
    // （谁画的、画在哪一格，见 VersionTreeDelegate::clearRect() 那段 NOTE）。
    //
    // 为什么不走 itemClicked：它只给"点中了哪个条目"、不给"点在条目里的哪儿"，而这一下
    // 点的是行里的一个按钮、**不是这一行** —— 点中叉的时候那一行不该跟着展开/收起。
    // 过滤器能把事件吃掉（返回 true 之后 Qt 那套点击处理整条不走了），两件事就都干净。
    QTreeWidget *tree = m_ui->getLoaderTree();
    if (tree != nullptr && watched == tree->viewport() &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QTreeWidgetItem *item = nullptr;
        if (mouse->button() == Qt::LeftButton) {
            item = tree->itemAt(mouse->position().toPoint());
        }
        if (item != nullptr &&
            item->data(MainWindowUI::loaderTreeKeyColumn(), MainWindowUI::rowClearRole())
                .toBool() &&
            VersionTreeDelegate::clearRect(tree->visualItemRect(item))
                .contains(mouse->position().toPoint())) {
            if (event->type() == QEvent::MouseButtonPress) {
                // 按下就动手（松开那一下只负责被吃掉）：跟普通按钮一样，按下去就该有反应。
                // 这一下取消的是"装这个加载器"这件事 —— 退回「不装（纯原版）」。
                // NOTE: **钉住的版本号留着**（不清 m_chosenVersion）：那是用户自己挑的
                // 一版，按一下叉就把它忘掉，下次再选中这条加载器还得从头找一遍。
                // 它只在"这一条被选中"的时候才显示，留着不会让人看到不该看的东西。
                m_installPage->selectLoader(MainWindowUI::modLoaderNoneIndex());
            }
            return true;
        }
    }
    // 实例页那张树上**行里的按钮**，头一个：组行行尾那颗齿轮（谁画的、画在哪一格，
    // 见 InstanceGroupDelegate::gearRect()）。判据和上面那个叉一模一样 —— 包括"吃掉
    // 那一下"：不吃的话点齿轮会顺手把这一组展开/收起（那正是"点组行"要干的事）。
    // 另外两颗（上移 / 下移）在下面那一段，判据同族，只有"按得快要不要补那一发"不同。
    //
    // NOTE: 这里**也接双击**。点得快的时候第二发及以后到的是 MouseButtonDblClick
    // （见 ui/RowActivation.hpp 那段），只接 Press/Release 的话"连点两下齿轮"
    // 的第二下会漏给下面那条"点中了这一行"的路。
    QTreeWidget *instanceTree = m_ui->getInstanceList();
    if (instanceTree != nullptr && watched == instanceTree->viewport()
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick)) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QTreeWidgetItem *item =
            mouse->button() == Qt::LeftButton ? instanceTree->itemAt(mouse->position().toPoint())
                                              : nullptr;
        if (item != nullptr && item->data(0, MainWindowUI::rowGroupGearRole()).toBool()
            && InstanceGroupDelegate::gearRect(instanceTree->visualItemRect(item))
                   .contains(mouse->position().toPoint())) {
            // 按下就动手（松开、以及那几发双击只负责被吃掉）：跟普通按钮一样，按下去就该
            // 有反应。双击那几发**不能**再弹一遍 —— 弹第二遍时上面那块框已经开着，
            // openGroupMenu() 认到"还是同一组"就把它收回去了，用户看到的是"连点两下齿轮
            // 反而没了"（那一处正是"再点同一颗齿轮 = 收回"，别在门口把它挡掉）。
            if (event->type() == QEvent::MouseButtonPress) {
                openGroupMenuFor(item);
            }
            return true;
        }
    }
    // 同一个实例树上**实例行**悬停那三格（ListRowDelegate：文件夹 / 更多 / 启动）。也接双击：
    // 点得快时第二发是双击事件，不吃掉会漏给"点中这一行"那条路（进实例设置）。
    // 服务端列表（同一个 ListRowDelegate）认同一套三格。
    QTreeWidget *rowTree = instanceTree != nullptr && watched == instanceTree->viewport() ? instanceTree : nullptr;
    if (rowTree == nullptr && m_ui->getServerList() != nullptr && watched == m_ui->getServerList()->viewport()) {
        rowTree = m_ui->getServerList();
    }
    if (rowTree != nullptr
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick)) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QTreeWidgetItem *item =
            mouse->button() == Qt::LeftButton ? rowTree->itemAt(mouse->position().toPoint())
                                              : nullptr;
        // 悬停快捷操作三格（文件夹 / 更多 / 启动，几何同 ListRowDelegate::actionRect）：
        // 按下就发 actionTriggered，松开和双击只负责被吃掉，免得漏给"点中这一行 = 进实例设置"。
        auto *rowDelegate = qobject_cast<ListRowDelegate *>(rowTree->itemDelegate());
        if (item != nullptr && rowDelegate != nullptr && !item->data(0, MainWindowUI::groupIdRole()).isValid()) {
            const QRect row = rowTree->visualItemRect(item);
            const QPoint pos = mouse->position().toPoint();
            for (const auto action : {ListRowDelegate::Action::Folder, ListRowDelegate::Action::More,
                                      ListRowDelegate::Action::Launch}) {
                if (!ListRowDelegate::actionRect(row, action).contains(pos)) continue;
                if (event->type() == QEvent::MouseButtonPress)
                    emit rowDelegate->actionTriggered(rowTree->indexAt(pos), action);
                return true;
            }
        }
    }
    // NOTE: 组行行尾那两颗「上移 / 下移」（齿轮左边那两颗实心三角）不在这棵树上 ——
    // 它们在「分组设置…」那张表里，认下那一下的是 `ui/GroupDialog::eventFilter()`
    // （挂在那张表的视口上），"每一发都算"和"Shift = 置顶/置底"那两条手势跟着它。
    // 实例行行尾那一排的接线在**上面**（就在组行齿轮那段后面）—— 它和那套是同一个
    // 位置、同一族判据，别再往下找。
    // 下载页左栏那份分类列表：每一行最右边那个刷新按钮（谁画的、画在哪一格，见
    // VersionTreeDelegate::refreshRect()）。判据和上面那个叉一模一样，只是宿主是个
    // QListWidget（它的 itemAt/visualItemRect 和 QTreeWidget 那对同名同义）。
    QListWidget *categories = m_ui->getDownloadCategoryList();
    if (categories != nullptr && watched == categories->viewport() &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QListWidgetItem *item =
            mouse->button() == Qt::LeftButton ? categories->itemAt(mouse->position().toPoint())
                                              : nullptr;
        if (item != nullptr &&
            item->data(MainWindowUI::rowRefreshRole()).toBool() &&
            VersionTreeDelegate::refreshRect(categories->visualItemRect(item))
                .contains(mouse->position().toPoint())) {
            if (event->type() == QEvent::MouseButtonPress) {
                // 按下就动手（松开那一下只负责被吃掉）—— 和那个叉一个规矩。
                refreshDownloadCategory(categories->row(item));
            }
            return true;
        }
    }
    // 下载页那四张资源表：**点行身 = 打开这一条资源的详情页**（下载页选中一个模组之后
    // 进入一个独立界面）；点行尾那个箭头仍然是原有规矩（展开 / 收起，
    // 判据是同一个 arrowRect()）—— 于是"展开这一条"这件事从此只挂在箭头上。
    //
    // NOTE: 这一块**只认第一层**（一个资源）。第二层（游戏版本）点开是"问这个资源在这个
    // 游戏版本上有哪些版本可下"、第三层（版本号）点它是"下它"，两条原有的路留着
    // （详情页是**另一条**看整份清单的路，不是把树上那条拆掉）。
    // NOTE: 三发都吃掉（按下 / 松开 / 双击）：`connectRowActivation` 那边给"开关行"补的
    // 那一发双击也会落到"展开"上 —— 而这一下已经算"进这一页"了，不该再翻一次树。
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
        || event->type() == QEvent::MouseButtonDblClick) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        for (ResourceCatalog::Kind treeKind : ResourceCatalog::allKinds()) {
            const QString treeKey = ResourceCatalog::kindKey(treeKind);
            QTreeWidget *tree = m_ui->getDownloadResourceTree(treeKey);
            if (tree == nullptr || watched != tree->viewport()) {
                continue;
            }
            const QPoint pos = mouse->position().toPoint();
            QTreeWidgetItem *item =
                mouse->button() == Qt::LeftButton ? tree->itemAt(pos) : nullptr;
            if (item == nullptr || resourceRowDepth(item) != 0) {
                continue;
            }
            // "这一行是不是我们建的"：占位行（「正在搜索…」那几行）没有这一位。
            const int column = MainWindowUI::versionTreeKeyColumn();
            const QString kindKey =
                item->data(column, MainWindowUI::resourceKindRole()).toString();
            if (kindKey.isEmpty()) {
                continue;
            }
            // 箭头那一格**交回去**（展开 / 收起还是它）：判据和代理画它用的是同一个函数，
            // 两边各算一份的话迟早错开，而错开的形状是"看着点的是箭头、其实进了一页"。
            if (VersionTreeDelegate::arrowRect(tree->visualItemRect(item)).contains(pos)) {
                continue;
            }
            if (event->type() == QEvent::MouseButtonPress) {
                // 按下就动手（松开和那几发双击只负责被吃掉）—— 和实例行上那三颗一个规矩。
                const QString iconKey =
                    item->data(column, MainWindowUI::resourceIconIdRole()).toString();
                // **排到下一轮事件循环**再开页：这一发还在视图的事件处理里（列表那一侧
                // 的状态机悬在半路），开页那一句不该在这里跑（和 `openPage()` 那条
                // "排到下一轮再开"是同一个理由）。这里传的是两个字符串**值**，不是条目
                // 指针 —— 下一轮里树可能已经被重搜清空了。
                //
                // 来路钉死是 `Download`：这条路上这一页**永远**摆「显示
                // 快照版」，哪怕点的是一个模组（不能把下载页的行为带偏）。
                // 实例号从下载页那条来路里读（`m_downloadReturn`）。
                const QString instanceId = m_downloadReturn.instanceId;
                QTimer::singleShot(0, this, [this, iconKey, kindKey, instanceId] {
                    // NOTE: 覆盖层被别的页占着时（典型场面：实例设置那一页**收着**、
                    // 用户切到下载页逛），`openResourceDetailPage()` 开头那条 `isOpen()`
                    // 会把这一下当场吃掉 —— 用户点了行身，屏幕上一个反应都没有。所以
                    // 先让位：静默收掉占着的那一层，再开（让位那一段的唯一出处是
                    // `runWhenOverlayFree()`）。
                    runWhenOverlayFree([this, iconKey, kindKey, instanceId] {
                        openResourceDetailPage(iconKey, kindKey, DetailSource::Download, instanceId);
                    });
                });
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

namespace {

/// 这个窗口里露着、点得动的那几颗「刷新」（`Theme::markRefreshButton()` 认人）。
QList<QPushButton *> visibleRefreshButtons(QWidget *host) {
    QList<QPushButton *> found;
    if (host == nullptr) {
        return found;
    }
    const QByteArray marker = Theme::refreshActionPropertyName().toUtf8();
    for (QPushButton *button : host->findChildren<QPushButton *>()) {
        if (!button->property(marker.constData()).toBool()) {
            continue;
        }
        if (!button->isVisible() || !button->isEnabled()) {
            continue;
        }
        found.append(button);
    }
    return found;
}

/// 同一窗口里露着好几颗时，取**离焦点最近**的那一颗：从焦点往上走，谁先罩住它用谁。
/// 一路没罩住（焦点不在这棵树里）就给第一颗 —— 顺序是建页顺序，够用。
QPushButton *nearestToFocus(const QList<QPushButton *> &candidates) {
    QWidget *focus = QApplication::focusWidget();
    if (focus == nullptr || candidates.size() <= 1) {
        return candidates.value(0);
    }
    QPushButton *best = candidates.first();
    int bestDistance = std::numeric_limits<int>::max();
    for (QPushButton *candidate : candidates) {
        int distance = 0;
        for (QWidget *widget = focus; widget != nullptr; widget = widget->parentWidget(), ++distance) {
            if (widget->isAncestorOf(candidate)) {
                break;
            }
        }
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

}  // namespace

/// F5：点当前最上层可见页面上那颗刷新按钮。
///
/// 只看 `Theme::markRefreshButton()` 标过的那几颗，所以"这一页有没有刷新"仍由各页自己说；
/// 没有标记、或者标记的那一颗此刻收着/灰着，就什么都不做。开着对话框时先点对话框里那颗
/// （`activeWindow()` 是它）。
void MainWindow::refreshTopmostPage() {
    QWidget *host = QApplication::activeWindow();
    if (host == nullptr) {
        host = this;
    }
    QList<QPushButton *> candidates = visibleRefreshButtons(host);
    if (candidates.isEmpty() && host != this) {
        candidates = visibleRefreshButtons(this);
    }
    QPushButton *target = nearestToFocus(candidates);
    if (target != nullptr) {
        target->click();
    }
}

/// 「主页可见、没有浮层占着、焦点不在可交互控件上」时，回车 = 点那颗「启动游戏」。
///
/// 主页可见就用那颗按钮自己的状态认（它只在主页上露着、正在启动时收起来）；
/// 不新记一份"当前在哪一页"。按钮不可用（没选版本 / 正在启动）就什么都不做。
bool MainWindow::handleHomeEnter() {
    // 主页可见 = 选项卡停在第 0 页（那颗「启动游戏」是给既有接线用的兼容控件，平时收着，
    // 不能拿它露没露当判据）。
    if (m_ui == nullptr || m_ui->getTabWidget() == nullptr ||
        m_ui->getTabWidget()->currentIndex() != MainWindowUI::homeTabIndex()) {
        return false;
    }
    QPushButton *start = m_ui->getStartButton();
    if (start == nullptr || !start->isEnabled()) {
        return false;
    }
    // 正在启动：这一发不给"再启动一次"。
    if (m_activityCenter != nullptr && m_launchCardTask != Tasks::invalidTaskId()) {
        const Tasks::TaskSnapshot task = m_activityCenter->tasks()->snapshot(m_launchCardTask);
        if (task.kind == Tasks::TaskKind::Launch && Tasks::isActiveState(task.state)) {
            return false;
        }
    }
    // 下拉、右键菜单、自己搭的 `Qt::Popup` 开着：这一发先归它们。
    if (QApplication::activePopupWidget() != nullptr) {
        return false;
    }
    OverlayHost *overlay = m_ui->overlay();
    if (overlay != nullptr && overlay->isOpen()) {
        return false;
    }
    if (keyboardFocusTakesEnter(QApplication::focusWidget())) {
        return false;
    }
    start->click();
    return true;
}

/// 启动走到一半（游戏进程还没起来）时，Esc = 点启动卡片上那颗「取消」。
///
/// 浮层 / 弹出层 / 下拉开着时先归它们（Esc 是它们收起自己的那个键），这里不发取消；
/// 游戏已经起来之后（`task.sessionId > 0`）也不碰 —— 那一段的退出走会话那一套。
bool MainWindow::handleEscCancelLaunch() {
    if (QApplication::activePopupWidget() != nullptr) {
        return false;
    }
    OverlayHost *overlay = m_ui != nullptr ? m_ui->overlay() : nullptr;
    if (overlay != nullptr && overlay->isOpen()) {
        return false;
    }
    if (m_activityCenter == nullptr || m_launchCardTask == Tasks::invalidTaskId()) {
        return false;
    }
    const Tasks::TaskSnapshot task = m_activityCenter->tasks()->snapshot(m_launchCardTask);
    if (task.sessionId > 0) {
        return false;
    }
    // 和点卡片上那颗「取消」同一条路：`cancelLaunch()` 自己会问"这会儿取消得动吗"。
    return cancelLaunch(task);
}

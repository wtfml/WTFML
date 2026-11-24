// 加载态组件（ui/LoadingState）：整块 begin / finish / fail 与行内 beginRow / failRow。
// 钉的是规矩本身：150ms 门槛、出错态「重试」接线、重复 begin 不叠层、动效关时照样露面且点不动、
// 行内只重绘登记的那几行、行删了时钟就停；矮宿主（下拉框、一行字）画成一行、出错时「重试」是文字按钮；
// 步骤 126 接上的几处页面（资源详情、新建服务端、预设计划、文件详情、下载页游戏刷新那一格）；
// 步骤 136 的行尾加载态（列表一行行尾那句状态在等：预设弹框的兼容检查）。
#include "Support.hpp"
#include "InstanceFilesPage.hpp"
#include "LoadingState.hpp"
#include "MainWindowUI.hpp"
#include "NewServerPage.hpp"
#include "PresetManagePage.hpp"
#include "ResourceDetailPage.hpp"
#include "VersionTreeDelegate.hpp"

#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QTemporaryDir>

#include <QEventLoop>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int overlayCount(QWidget *host) {
    return host->findChildren<QWidget *>(QStringLiteral("wtfmlLoadingOverlay"), Qt::FindDirectChildrenOnly).size();
}

/// 数 Show 事件：「150ms 内结束就从没露过面」要的是**一次都没 show 过**，不是结束时恰好藏着。
class ShowCounter : public QObject {
public:
    int shows = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Show) ++shows;
        return false;
    }
};

/// 记下 viewport 每次重绘的区域。
class PaintLog : public QObject {
public:
    QList<QRect> rects;

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint) rects.append(static_cast<QPaintEvent *>(event)->rect());
        return false;
    }
};

void blockChecks() {
    QWidget window;
    window.resize(600, 400);
    auto *layout = new QVBoxLayout(&window);
    auto *tree = new QTreeWidget(&window);
    tree->setHeaderHidden(true);
    layout->addWidget(tree);
    window.show();
    QApplication::processEvents();

    // ---- 150ms 内结束：从没露过面 ----
    LoadingState::begin(tree, QStringLiteral("正在获取最热门的模组…"));
    QWidget *layer = LoadingState::overlay(tree);
    check(layer != nullptr, "LS1 begin 之后宿主上挂着一层加载态");
    ShowCounter shows;
    if (layer != nullptr) layer->installEventFilter(&shows);
    const QPointer<QWidget> early(layer);
    check(layer != nullptr && !layer->isVisible(), "LS1 刚 begin 时它还藏着（门槛没到）");
    wait(70);
    LoadingState::finish(tree);
    check(LoadingState::overlay(tree) == nullptr, "LS1 150ms 内 finish：这一层立刻撤掉");
    wait(150);
    check(early.isNull() && shows.shows == 0, "★ LS1 150ms 内结束的那一发从头到尾没露过面",
          QString("还在=%1 露面=%2").arg(early.isNull() ? 0 : 1).arg(shows.shows));

    // ---- 过了 150ms：露面，写着那句话 ----
    LoadingState::begin(tree, QStringLiteral("正在搜索「钠」…"));
    layer = LoadingState::overlay(tree);
    wait(260);
    check(layer != nullptr && layer->isVisible() && layer->accessibleName() == QStringLiteral("正在搜索「钠」…") &&
              layer->property("state").toString() == QStringLiteral("loading"),
          "★ LS2 过了 150ms 加载态露面，写着这一发的那句话",
          layer != nullptr ? layer->accessibleName() : QStringLiteral("没有那一层"));
    check(layer != nullptr && layer->geometry() == tree->rect(), "LS2 它铺满宿主（跟着宿主走）");
    check(tree->childAt(tree->rect().center()) != layer, "LS2 加载中不拦鼠标（点得穿）");
    window.resize(700, 460);
    QApplication::processEvents();
    check(layer != nullptr && layer->geometry() == tree->rect(), "LS2 宿主变大，它跟着变大");

    // ---- 重复 begin：不叠第二层，只换文案 ----
    LoadingState::begin(tree, QStringLiteral("第二句"));
    LoadingState::begin(tree, QStringLiteral("第三句"));
    check(overlayCount(tree) == 1 && LoadingState::overlay(tree) == layer &&
              layer->accessibleName() == QStringLiteral("第三句"),
          "★ LS3 重复 begin 不叠第二层，只换成最后那句", QString::number(overlayCount(tree)));
    check(layer != nullptr && layer->isVisible(), "LS3 已经露着的那一层换文案时不先藏起来");

    // ---- 动效开着：点在呼吸（隔一段时间拍两张不一样） ----
    const QImage a = tree->grab().toImage();
    wait(300);
    const QImage b = tree->grab().toImage();
    check(a != b, "LS4 动效开着时三颗点在动（两次截图不一样）");

    // ---- 失败：出错态 + 「重试」，点它 = 调 retry ----
    int retried = 0;
    LoadingState::fail(tree, QStringLiteral("没取到模组列表：连接超时"), [&] {
        ++retried;
        LoadingState::begin(tree, QStringLiteral("正在获取最热门的模组…"));
    });
    layer = LoadingState::overlay(tree);
    auto *retry = layer != nullptr ? layer->findChild<QPushButton *>(LoadingState::retryButtonName()) : nullptr;
    check(layer != nullptr && layer->isVisible() && layer->property("state").toString() == QStringLiteral("error") &&
              layer->accessibleName() == QStringLiteral("没取到模组列表：连接超时"),
          "★ LS5 fail：立刻换成出错态，写着原因");
    check(retry != nullptr && retry->isVisible() && retry->text() == QStringLiteral("重试"),
          "★ LS5 出错态上有一颗「重试」");
    if (retry != nullptr) {
        const QPoint at = retry->mapTo(tree, retry->rect().center());
        check(tree->childAt(at) == retry, "LS5 「重试」点得到（出错态接鼠标）");
        retry->click();
    }
    check(retried == 1, "★ LS5 点「重试」= 调了 retry 一次", QString::number(retried));
    check(overlayCount(tree) == 1 && LoadingState::overlay(tree) != nullptr &&
              LoadingState::overlay(tree)->property("state").toString() == QStringLiteral("loading") &&
              LoadingState::overlay(tree)->isVisible() && retry != nullptr && !retry->isVisible(),
          "LS5 重试里再 begin：同一层直接换成点（不叠层、不闪、「重试」收起）");
    LoadingState::finish(tree);
    wait(Anim::exitMs() + 80);
    check(overlayCount(tree) == 0, "LS6 露过面的那一层 finish 后淡出撤掉", QString::number(overlayCount(tree)));

    // ---- 动效关着：门槛照旧，照样露面，点不动，时钟不空转 ----
    Anim::setEnabled(false);
    LoadingState::begin(tree, QStringLiteral("正在获取…"));
    layer = LoadingState::overlay(tree);
    check(layer != nullptr && !layer->isVisible(), "LS7 动效关：门槛照旧（刚 begin 时还藏着）");
    wait(220);
    check(layer != nullptr && layer->isVisible(), "★ LS7 动效关：过了门槛照样露面");
    const QImage c = tree->grab().toImage();
    wait(300);
    const QImage d = tree->grab().toImage();
    check(c == d, "★ LS7 动效关：三颗点是静的（两次截图一模一样）");
    check(!LoadingState::clockRunning(), "LS7 动效关：露面之后时钟不再空转");
    LoadingState::finish(tree);
    check(LoadingState::overlay(tree) == nullptr, "LS7 动效关：finish 立刻撤掉");
    Anim::setEnabled(true);
    QApplication::processEvents();
}

void inlineChecks() {
    QWidget window;
    window.resize(600, 400);
    auto *layout = new QVBoxLayout(&window);
    auto *tree = new QTreeWidget(&window);
    tree->setHeaderHidden(true);
    tree->setItemDelegate(new VersionTreeDelegate(tree, false));
    layout->addWidget(tree);
    auto *other = new QTreeWidgetItem(tree, {QStringLiteral("Fabric")});
    for (int i = 0; i < 3; ++i) new QTreeWidgetItem(other, {QStringLiteral("0.16.%1").arg(i)});
    auto *parent = new QTreeWidgetItem(tree, {QStringLiteral("Forge")});
    other->setExpanded(true);
    parent->setExpanded(true);
    window.show();
    QApplication::processEvents();

    QTreeWidgetItem *row = LoadingState::beginRow(parent, QStringLiteral("正在获取…"));
    check(row != nullptr && row->parent() == parent && row->isDisabled() &&
              row->text(0) == QStringLiteral("正在获取…"),
          "LI1 beginRow：在那一层底下建一条禁用的占位行，文字照旧");
    check(LoadingState::clockRunning() && LoadingState::tickingRowCount() == 1, "LI1 登记上了，共享时钟在转");

    // ---- 只重绘登记的那一行 ----
    wait(Anim::loadingDelayMs() + 60);
    PaintLog log;
    tree->viewport()->installEventFilter(&log);
    wait(200);
    tree->viewport()->removeEventFilter(&log);
    const QRect rowRect = tree->visualItemRect(row);
    bool onlyRow = !log.rects.isEmpty();
    for (const QRect &r : log.rects) onlyRow = onlyRow && rowRect.contains(r);
    check(onlyRow, "★ LI2 时钟每一跳只重绘登记的那一行，不重绘整棵树",
          QString("重绘 %1 次，行 %2,%3 %4x%5").arg(log.rects.size()).arg(rowRect.x()).arg(rowRect.y())
              .arg(rowRect.width()).arg(rowRect.height()));

    // ---- 行删了：自动注销，时钟停 ----
    delete parent->takeChild(parent->indexOfChild(row));
    wait(60);
    check(LoadingState::tickingRowCount() == 0 && !LoadingState::clockRunning(), "★ LI3 占位行删掉后时钟停了");

    // ---- 出错行：「重试」那一格点中 = 调 retry ----
    int retried = 0;
    QTreeWidgetItem *failed = LoadingState::beginRow(parent);
    LoadingState::failRow(failed, QStringLiteral("列表没取到"), [&] {
        ++retried;
        delete parent->takeChild(parent->indexOfChild(failed));
        LoadingState::beginRow(parent);
    });
    check(failed->text(0) == QStringLiteral("列表没取到") && failed->isDisabled(), "LI4 failRow：同一行改写成原因，仍是灰的");
    check(LoadingState::tickingRowCount() == 0, "LI4 出错行不再按时钟重绘");
    tree->viewport()->repaint();
    const QModelIndex failedIndex = tree->model()->index(0, 0, tree->model()->index(1, 0));
    const QRect retryRect = LoadingState::rowRetryRect(failedIndex);
    check(!retryRect.isEmpty() && tree->visualItemRect(failed).contains(retryRect),
          "LI4 「重试」那一格画出来了，落在这一行里");
    if (!retryRect.isEmpty()) {
        const QPoint at = retryRect.center();
        QMouseEvent press(QEvent::MouseButtonPress, at, tree->viewport()->mapToGlobal(at), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, tree->viewport()->mapToGlobal(at), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &release);
    }
    check(retried == 1, "★ LI4 点「重试」那一格 = 调了 retry 一次", QString::number(retried));
    check(parent->childCount() == 1 && parent->child(0)->data(0, LoadingState::rowLoadingRole()).toLongLong() >= 0 &&
              LoadingState::tickingRowCount() == 1,
          "LI4 重试里删掉出错行、重新 beginRow：又是一条加载行");
    qDeleteAll(parent->takeChildren());
    wait(60);
    check(!LoadingState::clockRunning(), "LI5 收尾：没有登记行时时钟停着");

    // ---- 动效关：行内同样露面一次后就不再跳 ----
    Anim::setEnabled(false);
    LoadingState::beginRow(parent);
    wait(Anim::loadingDelayMs() + 80);
    check(LoadingState::tickingRowCount() == 0 && !LoadingState::clockRunning(),
          "LI6 动效关：行内加载行露面那一下画完就不再按时钟重绘");
    Anim::setEnabled(true);
}

/// [x0, x1) 这几列里有没有画过东西（不透明度 > 60）：粗判"那一行摆在哪边"。
bool inkInColumns(const QImage &image, int x0, int x1) {
    for (int y = 0; y < image.height(); ++y)
        for (int x = qMax(0, x0); x < qMin(image.width(), x1); ++x)
            if (qAlpha(image.pixel(x, y)) > 60) return true;
    return false;
}

void waitShown() { wait(Anim::loadingDelayMs() + Anim::appearMs() + 60); }

/// 只画加载态那一层（透明底），拿来判"画在哪"。
QImage layerImage(QWidget *layer) {
    QImage image(layer->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    layer->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

void compactChecks() {
    QWidget window;
    window.resize(520, 160);
    auto *layout = new QVBoxLayout(&window);
    auto *combo = new QComboBox(&window);
    combo->setFixedHeight(34);
    auto *label = new QLabel(&window);
    label->setFixedHeight(20);
    layout->addWidget(combo);
    layout->addWidget(label);
    layout->addStretch(1);
    window.show();
    QApplication::processEvents();

    // ---- 下拉框：一行，点靠左（从框里字的起点开始），不叠一颗真按钮 ----
    LoadingState::begin(combo, QStringLiteral("正在获取版本…"));
    QWidget *layer = LoadingState::overlay(combo);
    waitShown();
    check(layer != nullptr && layer->isVisible() && layer->geometry() == combo->rect(),
          "LC1 矮宿主（下拉框）：过了门槛露面，铺满那个框");
    if (layer != nullptr) {
        const QImage img = layerImage(layer);
        check(inkInColumns(img, 0, img.width() / 3) && !inkInColumns(img, img.width() * 2 / 3, img.width()),
              "★ LC1 下拉框上画成一行、靠左（右边三分之一没画东西）");
    }
    int retried = 0;
    LoadingState::fail(combo, QStringLiteral("列表没取到"), [&] {
        ++retried;
        LoadingState::begin(combo, QStringLiteral("正在获取版本…"));
    });
    layer = LoadingState::overlay(combo);
    if (layer != nullptr) layerImage(layer);  // 画一遍，记下「重试」那一格
    auto *button = layer != nullptr ? layer->findChild<QPushButton *>(LoadingState::retryButtonName()) : nullptr;
    const QRect retryRect = LoadingState::inlineRetryRect(combo);
    check(layer != nullptr && layer->property("state").toString() == QStringLiteral("error") && button != nullptr &&
              !button->isVisible(),
          "★ LC2 矮宿主出错：不露那颗次按钮（放不下）");
    check(!retryRect.isEmpty() && combo->rect().contains(retryRect.center()),
          "★ LC2 矮宿主出错：「重试」是画在框里的文字按钮",
          QString("%1,%2 %3x%4").arg(retryRect.x()).arg(retryRect.y()).arg(retryRect.width()).arg(retryRect.height()));
    if (layer != nullptr && !retryRect.isEmpty()) {
        const QPointF at = retryRect.center();
        QMouseEvent press(QEvent::MouseButtonPress, at, layer->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, at, layer->mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(layer, &press);
        QApplication::sendEvent(layer, &release);
    }
    check(retried == 1 && LoadingState::overlay(combo) != nullptr &&
              LoadingState::overlay(combo)->property("state").toString() == QStringLiteral("loading") &&
              LoadingState::inlineRetryRect(combo).isEmpty(),
          "★ LC2 点那一格「重试」= 调 retry 一次，换回加载中", QString::number(retried));
    LoadingState::finish(combo);
    wait(Anim::exitMs() + 80);
    check(overlayCount(combo) == 0, "LC3 下拉框上那一层 finish 后撤掉");

    // ---- 一行字（QLabel）：照标签的对齐摆 ----
    label->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    LoadingState::begin(label, QStringLiteral("正在读取…"));
    waitShown();
    layer = LoadingState::overlay(label);
    if (layer != nullptr) {
        const QImage img = layerImage(layer);
        check(layer->isVisible() && !inkInColumns(img, 0, img.width() / 4) &&
                  !inkInColumns(img, img.width() * 3 / 4, img.width()) &&
                  inkInColumns(img, img.width() / 4, img.width() * 3 / 4),
              "★ LC4 居中的标签：那一行跟着居中（两边各四分之一是空的）");
    } else {
        check(false, "★ LC4 标签上挂得上加载态");
    }
    LoadingState::finish(label);
    QApplication::processEvents();
}

void pageChecks() {
    // ---- 资源详情页：整块盖在版本列表上；回来了撤；没取到 = 出错态 + 重试 ----
    {
        ResourceDetailPage page;
        page.resize(760, 560);
        page.show();
        QApplication::processEvents();
        page.setLoading(QStringLiteral("正在读版本列表…"));
        QTreeWidget *list = page.versionList();
        waitShown();
        check(LoadingState::overlay(list) != nullptr && LoadingState::overlay(list)->isVisible() &&
                  list->topLevelItemCount() == 0,
              "★ R126-1 资源详情：版本列表盖整块加载态（列表里不再插一行「正在读版本列表…」）");
        int retried = 0;
        page.setFailed(QStringLiteral("连接超时"), [&] { ++retried; });
        QWidget *layer = LoadingState::overlay(list);
        auto *button = layer != nullptr ? layer->findChild<QPushButton *>(LoadingState::retryButtonName()) : nullptr;
        if (button != nullptr) button->click();
        check(retried == 1 && page.placeholderText() == QStringLiteral("连接超时"),
              "★ R126-1 资源详情：没取到 = 出错态，点「重试」再问一次");
        page.setGroups({});
        check(LoadingState::overlay(list) == nullptr, "R126-1 资源详情：摆了内容 / 空表那句就撤掉加载态");
    }
    // ---- 新建服务端：建好时不挂假的「正在获取」；真等时一行加载态；回来撤 ----
    {
        NewServerPage page;
        NewServerPage::FlavorOption vanilla;
        vanilla.token = QStringLiteral("vanilla");
        vanilla.name = QStringLiteral("原版");
        NewServerPage::FlavorOption fabric;
        fabric.token = QStringLiteral("fabric");
        fabric.name = QStringLiteral("Fabric");
        fabric.needsLoader = true;
        fabric.needsInstaller = true;
        page.setFlavors({vanilla, fabric}, QStringLiteral("vanilla"));
        page.resize(900, 700);
        page.show();
        QApplication::processEvents();
        auto *loader = page.findChild<QComboBox *>(MainWindowUI::newServerLoaderEditName() + "Combo");
        auto *installer = page.findChild<QComboBox *>(MainWindowUI::newServerInstallerEditName() + "Combo");
        const bool noFake = loader != nullptr && installer != nullptr &&
                            !loader->lineEdit()->placeholderText().contains(QStringLiteral("正在")) &&
                            !installer->lineEdit()->placeholderText().contains(QStringLiteral("正在")) &&
                            LoadingState::overlay(loader) == nullptr;
        check(noFake, "★ R126-3 新建服务端：没发请求时两个版本框不写「正在获取…」（原版档没有假的加载态）");
        auto *flavors = page.findChild<QListWidget *>(MainWindowUI::newServerFlavorListName());
        if (loader != nullptr && flavors != nullptr) {
            flavors->setCurrentRow(1);
            page.setVersionsLoading(false);
            waitShown();
            check(LoadingState::overlay(loader) != nullptr && LoadingState::overlay(loader)->isVisible() &&
                      loader->count() == 0 && loader->lineEdit()->placeholderText().isEmpty(),
                  "★ R126-3 新建服务端：真在等时框清空、上面一行加载态");
            page.setVersionChoices(false, {QStringLiteral("0.16.9")});
            check(LoadingState::overlay(loader) == nullptr && loader->count() == 1,
                  "R126-3 新建服务端：版本回来就撤掉加载态");
            page.setVersionsLoading(false);
            flavors->setCurrentRow(0);
            QApplication::processEvents();
            check(LoadingState::overlay(loader) == nullptr, "R126-3 新建服务端：换回不要这一格的档，加载态一起撤");
        } else {
            check(false, "R126-3 新建服务端：找得到版本框与端类别");
        }
    }
    // ---- 预设计划：计划那一格整块加载态；生成完 / 取消就撤 ----
    {
        PresetManagePage page;
        page.resize(1000, 760);
        page.show();
        QApplication::processEvents();
        auto *plan = page.findChild<QListWidget *>(PresetManagePage::planListName());
        page.setPlanLoading(true);
        check(plan != nullptr && LoadingState::overlay(plan) != nullptr,
              "★ R126-5 预设应用：生成计划时计划那一格挂加载态");
        page.setPlanLoading(false);
        check(plan != nullptr && LoadingState::overlay(plan) == nullptr, "R126-5 预设应用：取消 / 生成完就撤");
    }
    // ---- 文件详情那一行：字清掉、行高留住、上面一行加载态；答案到了撤 ----
    {
        QTemporaryDir dir;
        QFile file(dir.filePath(QStringLiteral("a.litematic")));
        if (file.open(QIODevice::WriteOnly)) {
            file.write("x");
            file.close();
        }
        InstanceFilesPage page;
        page.resize(700, 500);
        page.show();
        page.setDirectory(dir.path());
        page.setInfoPanelEnabled(true);
        QApplication::processEvents();
        page.list()->setCurrentRow(0);
        QApplication::processEvents();
        auto *info = page.findChild<QLabel *>(InstanceFilesPage::infoLabelName());
        check(info != nullptr && info->isVisible() && info->text().isEmpty() && info->height() > 0 &&
                  LoadingState::overlay(info) != nullptr,
              "★ R126-4 文件详情：选中一份文件 ⇒ 那一行挂加载态（字清掉，行高留住）");
        if (info != nullptr && page.list()->count() > 0) {
            page.setInfoText(page.list()->item(0)->data(InstanceFilesPage::pathRole()).toString(),
                             QStringLiteral("原理图"));
        }
        check(info != nullptr && info->text() == QStringLiteral("原理图") && LoadingState::overlay(info) == nullptr,
              "R126-4 文件详情：答案到了撤掉加载态、写上说明");
    }
    // ---- 下载页「游戏」：刷新状态那一格在筛选条上，矮（画成一行） ----
    {
        QMainWindow window;
        MainWindowUI ui(&window);
        window.resize(1280, 800);
        window.show();
        QApplication::processEvents();
        QLabel *status = ui.getVersionRefreshStatus();
        check(status != nullptr && status->text().isEmpty(),
              "★ R126-6 下载页「游戏」：筛选条上有那一格刷新状态（平时是空的）");
        if (status != nullptr) {
            status->window()->show();
            status->resize(qMax(status->width(), 240), status->height());
            check(status->height() < 48, "R126-6 那一格够矮：加载态画成一行", QString::number(status->height()));
        }
    }
}

void rowStatusChecks() {
    QListWidget list;
    list.resize(400, 200);
    list.addItem(QStringLiteral("a"));
    list.addItem(QStringLiteral("b"));
    list.show();
    QApplication::processEvents();
    const QModelIndex a = list.model()->index(0, 0);
    const QModelIndex b = list.model()->index(1, 0);
    const int before = LoadingState::tickingRowCount();
    LoadingState::beginRowStatus(&list, a);
    const qint64 started = a.data(LoadingState::rowLoadingRole()).toLongLong();
    check(a.data(LoadingState::rowLoadingRole()).isValid() && !b.data(LoadingState::rowLoadingRole()).isValid() &&
              LoadingState::tickingRowCount() == before + 1 && LoadingState::clockRunning(),
          "★ RS1 行尾加载态：只挂在那一行上，登记到共享时钟");
    wait(30);
    LoadingState::beginRowStatus(&list, a);
    check(a.data(LoadingState::rowLoadingRole()).toLongLong() == started &&
              LoadingState::tickingRowCount() == before + 1,
          "★ RS2 再 begin 一次不重新计门槛、不重复登记（进度更新不闪）");
    QImage img(400, 40, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    const QRect area(200, 0, 190, 40);
    const QFont font = list.font();
    const int early = LoadingState::paintRowStatus(&painter, area, a, font, QStringLiteral("检查中 1/3"));
    const bool blankEarly = !inkInColumns(img, 0, img.width());
    wait(Anim::loadingDelayMs() + Anim::appearMs());
    const int later = LoadingState::paintRowStatus(&painter, area, a, font, QStringLiteral("检查中 1/3"));
    painter.end();
    check(early > 0 && early == later && blankEarly && inkInColumns(img, area.left(), area.right() + 1) &&
              !inkInColumns(img, 0, area.left()),
          "★ RS3 没过门槛什么都不画但宽度占着；过了门槛在那一格里画「点 + 话」",
          QString::number(early));
    check(LoadingState::paintRowStatus(&painter, area, b, font, QStringLiteral("x")) == -1,
          "RS3 不在等的行回 -1（调用方照常画字）");
    LoadingState::finishRowStatus(&list, a);
    wait(Anim::stepMs() * 3);
    check(!a.data(LoadingState::rowLoadingRole()).isValid() && LoadingState::tickingRowCount() == before,
          "★ RS4 finish 后角色清掉、登记表下一拍丢掉它");
}

}  // namespace

int UiLoadingStateChecks(QApplication &) {
    Anim::setEnabled(true);
    blockChecks();
    inlineChecks();
    compactChecks();
    pageChecks();
    rowStatusChecks();
    Anim::setEnabled(false);
    return 0;
}

#include "ModActionPanelChecks.hpp"
#include "Theme.hpp"
#include "InstanceFilesPage.hpp"
#include "Animations.hpp"
#include "Glass.hpp"
#include "download/ModDependencies.hpp"
#include "download/DependencyInspection.hpp"
#include <QMenu>
#include "operations/TransferApply.hpp"
#include "manage/InstanceIconLookup.hpp"
#include "util/ZipUtil.hpp"
#include "util/ImageDecode.hpp"
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QApplication>
#include <QTemporaryDir>
#include <QListWidget>
#include <QPushButton>
#include <QFile>
#include <QDir>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QDrag>
#include <QMouseEvent>
#include <cstdio>

int runModActionPanelChecks() {
    Anim::setEnabled(false);
    int failed = 0;
    auto check = [&](bool ok, const char *name) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failed;
    };
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    QApplication::setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
    qApp->setStyleSheet(Theme::widgetStyleSheet());
    // 不要写在 `cwd/build/...`：`mw_check.exe` 是从自己的目录跑的，
    // 那里没有 `build/` 这一层 ⇒ QTemporaryDir 无效、第一条 fixture 就 `return 1`，
    // 而且**一个字都不打印**（看起来像崩溃）。系统临时目录永远可用。
    QTemporaryDir dir;
    if (!dir.isValid()) { std::printf("FAIL temporary fixture directory unavailable\n"); return 2; }
    {
        QTemporaryDir iconFixtures;
        if (!iconFixtures.isValid()) return 2;
        const auto writeIconFixture = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return 2;
        const QString icon = iconFixtures.filePath(QStringLiteral("embedded.png"));
        if (!writeIconFixture(icon, png)) return 2;
        const QString invalidIcon = iconFixtures.filePath(QStringLiteral("invalid.png"));
        if (!writeIconFixture(invalidIcon, "invalid image")) return 2;
        QStringList archives;
        const QList<QPair<QString, QByteArray>> declarations = {
            {QStringLiteral("fabric.mod.json"), R"({"icon":{"32":"assets/custom.png"}})"},
            {QStringLiteral("quilt.mod.json"), R"({"quilt_loader":{"metadata":{"icon":"assets/custom.png"}}})"},
            {QStringLiteral("META-INF/mods.toml"), "[[mods]]\nlogoFile=\"assets/custom.png\"\n"},
            {QStringLiteral("META-INF/neoforge.mods.toml"), "[[mods]]\nlogoFile='assets/custom.png'\n"},
            {QStringLiteral("mcmod.info"), R"([{"logoFile":"assets/custom.png"}])"}
        };
        int index = 0;
        for (const auto &declaration : declarations) {
            const QString manifest = iconFixtures.filePath(QStringLiteral("manifest-%1.json").arg(index));
            if (!writeIconFixture(manifest, declaration.second)) return 2;
            const QString archive = iconFixtures.filePath(QStringLiteral("icon-%1.jar.disabled").arg(index++));
            check(ZipUtil::createZip(archive, {{declaration.first, manifest},
                  {QStringLiteral("assets/custom.png"), icon}, {QStringLiteral("pack.png"), invalidIcon}}),
                  "create embedded mod icon fixture with different fallback");
            archives.append(archive);
        }
        const QString pack = iconFixtures.filePath(QStringLiteral("texture.zip"));
        check(ZipUtil::createZip(pack, {{QStringLiteral("pack.png"), icon}}), "create pack icon fixture");
        archives.append(pack);
        const auto icons = ZipUtil::readIconsBatch(archives);
        for (const auto &archive : archives) {
            check(icons.value(archive) == png && !ImageDecode::decode(icons.value(archive)).isNull(),
                  "embedded mod and pack icons read without network including disabled archives");
        }
        check(ZipUtil::readIconsBatch({iconFixtures.filePath(QStringLiteral("missing.zip"))}).isEmpty(),
              "unreadable archive allows network fallback");
        const QString corrupt = iconFixtures.filePath(QStringLiteral("corrupt.zip"));
        check(ZipUtil::createZip(corrupt, {{QStringLiteral("pack.png"), invalidIcon}}), "create invalid image fixture");
        const auto corruptBytes = ZipUtil::readIconsBatch({corrupt});
        check(ImageDecode::decode(corruptBytes.value(corrupt)).isNull(), "invalid embedded image allows network fallback");
        const QString large = iconFixtures.filePath(QStringLiteral("large.png"));
        if (!writeIconFixture(large, QByteArray(1024 * 1024 + 1, 'x'))) return 2;
        const QString oversized = iconFixtures.filePath(QStringLiteral("oversized.zip"));
        check(ZipUtil::createZip(oversized, {{QStringLiteral("pack.png"), large}}), "create oversized icon fixture");
        check(ZipUtil::readIconsBatch({oversized}).isEmpty(), "oversized embedded icon rejected before decode");
        for (const auto &archive : archives) QFile::remove(archive);
    }
    for (const auto &name : {QStringLiteral("Sodium.jar"), QStringLiteral("Iris.jar.disabled")}) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write("fixture") < 0) return 1;
    }
    InstanceFilesPage page;
    page.setBatchMode(true); page.setCanToggle(true); page.setCanDelete(true);
    page.setActionPanelEnabled(true); page.setMode(InstanceFilesPage::Mode::FilesOnly);
    page.setDownloadText(QStringLiteral("下载新模组"), QStringLiteral("安装更多模组"));
    page.resize(1000, 640); page.show(); page.setDirectory(dir.path()); page.refresh();
    QApplication::processEvents();
    auto *list = page.findChild<QListWidget *>();
    auto *panel = page.selectionBar();
    check(list && list->count() == 2, "mod fixture displayed");
    const QString dragProbe = qEnvironmentVariable("WTFML_MOD_DRAG_PROBE");
    if (!dragProbe.isEmpty()) {
        QDir().mkpath(dragProbe);
        for (int i = 0; i < 50; ++i) {
            QFile file(dir.filePath(QStringLiteral("Drag-Fixture-%1.jar").arg(i, 2, 10, QLatin1Char('0'))));
            if (file.open(QIODevice::WriteOnly)) file.write("drag fixture");
        }
        page.refresh();
        page.resize(1100, 740);
        page.setWindowTitle(QStringLiteral("WTFML Drag Reproduction"));
        list->selectAll();
        QEventLoop interaction;
        QTimer capture;
        int frames = 0;
        QObject::connect(&capture, &QTimer::timeout, &page, [&] {
            if (!page.findChild<QDrag *>() || frames >= 120) return;
            page.screen()->grabWindow(page.winId()).save(dragProbe + QStringLiteral("/frame-%1.png").arg(frames++, 3, 10, QLatin1Char('0')));
        });
        capture.start(40);
        QTimer::singleShot(120000, &interaction, &QEventLoop::quit);
        QObject::connect(qApp, &QGuiApplication::lastWindowClosed, &interaction, &QEventLoop::quit);
        interaction.exec();
        return 0;
    }
    check(panel->isHidden(), "no selection hides action panel");
    auto *all = page.findChild<QPushButton *>(InstanceFilesPage::selectionButtonName(InstanceFilesPage::SelectionAction::SelectAll));
    auto *clear = page.findChild<QPushButton *>(InstanceFilesPage::selectionButtonName(InstanceFilesPage::SelectionAction::ClearAll));
    check(all->isVisible() && !clear->isEnabled(), "selection tools visible without selection");
    all->click(); QApplication::processEvents();
    auto *invert = page.selectionButtonAt(static_cast<int>(InstanceFilesPage::SelectionAction::InvertAll));
    int selectionChanges = 0;
    const auto selectionConnection = QObject::connect(list, &QListWidget::itemSelectionChanged,
        &page, [&] { ++selectionChanges; });
    invert->click();
    check(selectionChanges == 1 && list->selectedItems().isEmpty() && panel->isHidden(),
          "invert clears selected rows in one notification and refreshes panel state");
    invert->click();
    check(selectionChanges == 2 && list->selectedItems().size() == 2 && !panel->isHidden(),
          "invert restores visible selection in one notification");
    QObject::disconnect(selectionConnection);
    {
        QWidget *viewport = list->viewport();
        const QRect row = list->visualItemRect(list->item(0));
        const QPoint point(row.left() + 80, row.center().y());
        const QImage before = viewport->grab().toImage();
        QMouseEvent press(QEvent::MouseButtonPress, point, viewport->mapToGlobal(point),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &press);
        const QImage held = viewport->grab().toImage();
        check(before.copy(row) == held.copy(row),
              "holding a selected row allows viewport paint events and preserves its rendering");
        QMouseEvent release(QEvent::MouseButtonRelease, point, viewport->mapToGlobal(point),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &release);
        all->click();
    }
    QStringList inspectedPaths;
    QObject::connect(&page, &InstanceFilesPage::inspectDependenciesRequested, &page,
        [&](const QStringList &paths) { inspectedPaths = paths; });
    QTimer::singleShot(0, &page, [&page] {
        auto *menu = page.findChild<QMenu *>();
        if (!menu) return;
        if (auto *action = menu->findChild<QAction *>(QStringLiteral("modInspectDependencies"))) action->trigger();
        menu->close();
    });
    QTimer::singleShot(1000, &page, [&page] { if (auto *menu = page.findChild<QMenu *>()) menu->close(); });
    page.findChild<QPushButton *>(QStringLiteral("modMoreActions"))->click();
    check(inspectedPaths.size() == 2 && list->selectedItems().size() == 2,
          "dependency inspection menu forwards selected paths without clearing selection");
    check(!page.findChild<QPushButton *>(QStringLiteral("modCheckSelectedUpdates")),
          "manual update check removed from action panel");
    check(list->selectedItems().size() == 2 && !panel->isHidden(), "multi-selection opens panel");
    auto *actionScroll = page.findChild<QScrollArea *>(QStringLiteral("modActionScroll"));
    check(actionScroll && actionScroll->width() == 156 && list->width() < 850,
          "wide layout reserves panel space");
    check(all->parentWidget()->parentWidget() != &page,
          "wide header shares filter and selection tools row");
    // 紧凑的模组页头把三件事并在一排里：「下载新模组」+「打开文件夹」+「更多…」。
    // 「打开文件夹」是从原来那条路径排 / 「更多」菜单里**提上来**的（最常用的一颗不该藏在菜单里），
    // 所以它在页头上露着、而且和「更多…」同一排 —— 路径排这一档在模组页整条是收起的。
    auto *headerMore = page.findChild<QPushButton *>(QStringLiteral("modHeaderMore"));
    auto *headerFolder = page.openFolderButton();
    check(page.downloadButton()->isVisible() && headerMore->isVisible()
          && headerFolder->isVisible()
          && headerFolder->parentWidget() == headerMore->parentWidget(),
          "compact mod header pairs install, folder and maintenance actions");
    page.findChild<QPushButton *>(QStringLiteral("modActionPanelClose"))->click();
    check(panel->isHidden() && list->selectedItems().size() == 2, "close preserves selection");
    page.findChild<QPushButton *>(QStringLiteral("modSelectedCount"))->click();
    check(!panel->isHidden(), "selected count reopens panel");
    int toggles = 0;
    QObject::connect(&page, &InstanceFilesPage::batchToggleRequested, &page,
        [&](const QStringList &paths, bool) { toggles += paths.size(); });
    auto *enable = page.selectionButtonAt(static_cast<int>(InstanceFilesPage::SelectionAction::Enable));
    auto *disable = page.selectionButtonAt(static_cast<int>(InstanceFilesPage::SelectionAction::Disable));
    check(enable->isVisible() && disable->isVisible() && enable->isEnabled() && disable->isEnabled(),
          "mixed selection exposes both direct state actions");
    enable->click();
    check(toggles == 1 && list->selectedItems().size() == 2, "mixed selection enables only disabled file and keeps selection");
    const QString renamed = dir.filePath(QStringLiteral("Iris.jar"));
    QFile::rename(dir.filePath(QStringLiteral("Iris.jar.disabled")), renamed);
    page.refresh(); QApplication::processEvents();
    check(list->selectedItems().size() == 2, "refresh preserves stable file identities after rename");
    const auto preview = qEnvironmentVariable("WTFML_MOD_PREVIEW_DIR");
    if (!preview.isEmpty()) { QDir().mkpath(preview); page.grab().save(preview + QStringLiteral("/mods-wide.png")); }
    {
        const auto previousTheme = Theme::current();
        Theme::setSpec(Theme::presetSpec(QStringLiteral("emerald")));
        Theme::apply(*qApp);
        QApplication::processEvents();
        const QImage lightIcon = disable->icon().pixmap(18, 18).toImage();
        Theme::setSpec(Theme::presetSpec(QStringLiteral("dark")));
        Theme::apply(*qApp);
        QApplication::processEvents();
        auto *remove = page.selectionButtonAt(static_cast<int>(InstanceFilesPage::SelectionAction::Delete));
        const auto fill = [&page](QPushButton *button) {
            const QPoint at = button->mapTo(&page, QPoint(button->width() / 2, 3));
            return page.grab().toImage().scaled(page.size()).pixelColor(at);
        };
        check(Glass::contrast(remove->palette().color(QPalette::ButtonText), fill(remove)) >= 4.5,
              "dark-theme delete text has readable contrast against its button surface");
        check(!enable->isEnabled() && disable->isEnabled() && fill(enable) != fill(disable),
              "disabled mod action has a distinct surface from available actions in dark theme");
        check(enable->icon().pixmap(18, 18, QIcon::Disabled).toImage() != enable->icon().pixmap(18, 18).toImage(),
              "action icons distinguish disabled state after a live theme switch");
        check(lightIcon != disable->icon().pixmap(18, 18).toImage(), "enabled action icons follow a light-to-dark theme change");
        if (!preview.isEmpty()) page.grab().save(preview + QStringLiteral("/mods-dark.png"));
        Theme::setSpec(previousTheme);
        Theme::apply(*qApp);
        QApplication::processEvents();
    }
    page.resize(560, 640); QApplication::processEvents();
    check(page.width() == 560 && list->width() > 450 && actionScroll->width() == 56 && actionScroll->geometry().right() < page.width(), "narrow drawer does not force minimum width");
    check(enable->text().isEmpty() && !enable->icon().isNull() && !enable->toolTip().isEmpty(),
          "compact action rail uses labeled icons and preserves selection");
    if (!preview.isEmpty()) page.grab().save(preview + QStringLiteral("/mods-narrow.png"));
    page.resize(560, 420); QApplication::processEvents();
    check(page.height() == 420 && (actionScroll->verticalScrollBar()->maximum() > 0 ||
              panel->minimumSizeHint().height() <= actionScroll->viewport()->height()),
          "short window either fits compact commands or makes them scrollable");
    auto *moreActions = page.findChild<QPushButton *>(QStringLiteral("modMoreActions"));
    actionScroll->ensureWidgetVisible(moreActions); QApplication::processEvents();
    check(actionScroll->viewport()->rect().contains(moreActions->mapTo(actionScroll->viewport(), moreActions->rect().center())),
          "last action remains reachable in short window");
    if (!preview.isEmpty()) page.grab().save(preview + QStringLiteral("/mods-short.png"));
    page.resize(560, 640); QApplication::processEvents();
    QFile::remove(renamed);
    page.refresh(); QApplication::processEvents();
    check(list->selectedItems().size() == 1 && !panel->isHidden(), "partial removal retains remaining selection");
    page.setSearchText(QStringLiteral("no-such-mod")); QApplication::processEvents();
    check(list->selectedItems().isEmpty() && panel->isHidden(), "filter cannot leave hidden selected operations");
    page.setSearchText(QString()); all->click();
    clear->click();
    check(list->selectedItems().isEmpty() && panel->isHidden(), "clear selection closes panel");
    // 第一次禁用一项时「启用 / 禁用」两颗胶囊冒出来：计数排换不换行、列表视口落在哪，都得和禁用前一样
    // （否则列表整体下跳，指针底下换成另一行，连点开关会点错）。换行的临界宽度随字体浮动，
    // 所以 400~1120 每 40 一档逐档扫，窄档计数排另起一行、宽档同排，两种都得稳。
    {
        QStringList moved;
        bool sawTwoRows = false, sawOneRow = false;
        QTemporaryDir stableDir;
        for (int i = 0; i < 12; ++i) {
            QFile file(stableDir.filePath(QStringLiteral("stable-%1.jar").arg(i, 2, 10, QLatin1Char('0'))));
            if (!file.open(QIODevice::WriteOnly) || file.write("fixture") < 0) return 1;
        }
        for (int width = 400; width <= 1120; width += 40) {
            InstanceFilesPage stable;
            stable.setBatchMode(true); stable.setCanToggle(true); stable.setCanDelete(true);
            stable.setCanFilterUpdates(true);
            stable.setActionPanelEnabled(true); stable.setMode(InstanceFilesPage::Mode::FilesOnly);
            stable.setDownloadText(QStringLiteral("下载新模组"), QStringLiteral("安装更多模组"));
            stable.resize(width, 640); stable.show();
            const QString first = stableDir.filePath(QStringLiteral("stable-00.jar"));
            QFile::rename(first + QStringLiteral(".disabled"), first);
            stable.setDirectory(stableDir.path()); stable.refresh(); QApplication::processEvents();
            auto *stableList = stable.findChild<QListWidget *>();
            auto *tools = stable.findChild<QWidget *>(QStringLiteral("modSelectionTools"));
            const auto listY = [&] { return stableList->viewport()->mapTo(&stable, QPoint()).y(); };
            const auto toolsY = [&] { return tools->mapTo(&stable, QPoint()).y(); };
            const int listBefore = listY(), toolsBefore = toolsY();
            QFile::rename(first, first + QStringLiteral(".disabled"));
            stable.refresh(); QApplication::processEvents();
            const bool pillsShown = stable.findChild<QPushButton *>(InstanceFilesPage::stateButtonName(
                InstanceFilesPage::StateFilter::Disabled))->isVisible();
            if (!pillsShown || listY() != listBefore || toolsY() != toolsBefore)
                moved << QStringLiteral("%1: list %2->%3 row %4->%5").arg(width).arg(listBefore).arg(listY())
                             .arg(toolsBefore).arg(toolsY());
            (tools->parentWidget() == &stable ? sawTwoRows : sawOneRow) = true;
        }
        const QByteArray name = (QStringLiteral("first disable keeps list viewport and selection row in place "
            "at widths 400..1120 ") + moved.join(QStringLiteral("; "))).toUtf8();
        check(moved.isEmpty() && sawTwoRows && sawOneRow, name.constData());
    }
    // 紧凑页头（资源包页）：查完更新「可更新」冒出来、再查一趟收回去，列表视口都不许动。
    // 同样逐档扫页宽，窄档胶囊单占一排、宽档排进搜索那一排，两种都得见到。
    {
        QStringList moved;
        bool sawOwnRow = false, sawInline = false;
        QTemporaryDir packDir;
        for (int i = 0; i < 12; ++i) {
            QFile file(packDir.filePath(QStringLiteral("pack-%1.zip").arg(i, 2, 10, QLatin1Char('0'))));
            if (!file.open(QIODevice::WriteOnly) || file.write("fixture") < 0) return 1;
        }
        for (int width = 240; width <= 1120; width += 40) {
            InstanceFilesPage pack;
            pack.setCompactHeader(new QPushButton(QStringLiteral("启用与顺序"), &pack));
            pack.setMode(InstanceFilesPage::Mode::FilesAndDirs);
            pack.setCanDelete(true); pack.setCanFilterUpdates(true);
            pack.setDownloadText(QStringLiteral("下载新资源包"), QStringLiteral("安装更多资源包"));
            pack.setBatchMode(true);
            pack.resize(width, 640); pack.show();
            pack.setDirectory(packDir.path()); pack.refresh(); QApplication::processEvents();
            auto *packList = pack.findChild<QListWidget *>();
            auto *update = pack.findChild<QPushButton *>(
                InstanceFilesPage::stateButtonName(InstanceFilesPage::StateFilter::Update));
            const auto listY = [&] { return packList->viewport()->mapTo(&pack, QPoint()).y(); };
            const int before = listY();
            pack.setUpdateResult({packList->item(0)->data(InstanceFilesPage::pathRole()).toString()}, true);
            QApplication::processEvents();
            const bool shown = update->isVisible();
            const int afterShow = listY();
            pack.setUpdateResult({}, true); QApplication::processEvents();
            const int afterHide = listY();
            if (!shown || update->isVisible() || afterShow != before || afterHide != before)
                moved << QStringLiteral("%1: list %2->%3->%4").arg(width).arg(before).arg(afterShow).arg(afterHide);
            QWidget *stateRow = update->parentWidget()->parentWidget();
            (stateRow->parentWidget() == pack.findChild<QWidget *>(InstanceFilesPage::searchEditName())->parentWidget()
                 ? sawInline : sawOwnRow) = true;
        }
        const QByteArray name = (QStringLiteral("compact header keeps list viewport in place when the update pill "
            "appears and goes at widths 240..1120 ") + moved.join(QStringLiteral("; "))).toUtf8();
        std::printf("DBG own=%d inline=%d\n", sawOwnRow, sawInline);
        check(moved.isEmpty() && sawOwnRow && sawInline, name.constData());
    }
    // Opt-in real-widget comparison. Keep timing out of pass/fail thresholds:
    // the reference intentionally repeats the old row-by-row notification path.
    const int requestedRows = qEnvironmentVariableIntValue("WTFML_MOD_SELECTION_PROBE_ROWS");
    {
        QTemporaryDir marks;
        QFile file(marks.filePath(QStringLiteral("[物品管理器] jei-26.2-fabric-30.9.0.23-long-filename.jar")));
        check(file.open(QIODevice::WriteOnly), "create update marker alignment fixture");
        file.write("fixture"); file.close();
        InstanceFilesPage markedPage;
        markedPage.resize(1400, 180); markedPage.show(); markedPage.setDirectory(marks.path());
        QApplication::processEvents();
        auto *markedList = markedPage.findChild<QListWidget *>();
        const QRect name = markedPage.rowGeometry(0).name;
        const auto capture = [&] { return markedList->viewport()->grab().toImage().scaled(markedList->viewport()->size()); };
        const QImage plain = capture();
        markedPage.setUpdateResult({file.fileName()}, true);
        QApplication::processEvents();
        const QImage marked = capture();
        const QRgb background = plain.pixel(name.right() - 1, name.center().y());
        int overwritten = 0;
        for (int y = name.top(); y <= name.bottom(); ++y)
            for (int x = name.left(); x <= name.right(); ++x)
                if (plain.pixel(x, y) != background && marked.pixel(x, y) != plain.pixel(x, y)) ++overwritten;
        check(overwritten == 0 && plain != marked, "update glyph appears after filename without overwriting text pixels");
        if (!preview.isEmpty()) markedPage.grab().save(preview + QStringLiteral("/update-marker.png"));
    }
    if (requestedRows > 0) {
        const int rows = qBound(10, requestedRows, 2000);
        const QString probeRoot = dir.filePath(QStringLiteral("selection-probe"));
        if (!QDir().mkpath(probeRoot)) return 1;
        for (int i = 0; i < rows; ++i) {
            QFile file(QDir(probeRoot).filePath(QStringLiteral("mod-%1.jar").arg(i)));
            if (!file.open(QIODevice::WriteOnly) || file.write("fixture") < 0) return 1;
        }
        page.setDirectory(probeRoot); QApplication::processEvents();
        check(list->count() == rows, "selection probe displays requested fixture rows");
        int notices = 0;
        const auto connection = QObject::connect(list, &QListWidget::itemSelectionChanged,
            &page, [&] { ++notices; });
        QElapsedTimer timer; timer.start();
        for (int i = 0; i < list->count(); ++i) list->item(i)->setSelected(true);
        const qint64 referenceUs = timer.nsecsElapsed() / 1000;
        const int referenceNotices = notices;
        clear->click(); notices = 0;
        timer.restart(); invert->click();
        const qint64 bulkUs = timer.nsecsElapsed() / 1000;
        check(list->selectedItems().size() == rows && notices == 1,
              "large invert preserves the same selected set with one notification");
        std::printf("SELECTION-PROBE rows=%d reference_us=%lld reference_notices=%d bulk_us=%lld bulk_notices=%d\n",
            rows, static_cast<long long>(referenceUs), referenceNotices,
            static_cast<long long>(bulkUs), notices);
        QObject::disconnect(connection);
        clear->click();
    }
    // Opt-in 首绘 / 重列 / 滚动测量（P2「复杂预设/大列表」：先测，再决定要不要走模型委托 /
    // 虚拟化）。**只打印、不设阈值** —— 耗时跟机器走，写死数字只会变成一条噪声断言。
    // 量的三件事：① 换到这份目录（扫盘 + 逐行建控件 + 一次事件循环）；② 同一份目录再
    // refresh 一遍（重扫 + 整表重建）；③ 滚到底（视图重绘那一截）。
    const int paintRows = qEnvironmentVariableIntValue("WTFML_FILES_PAINT_PROBE_ROWS");
    if (paintRows > 0) {
        const int rows = qBound(10, paintRows, 2000);
        const QString paintRoot = dir.filePath(QStringLiteral("paint-probe"));
        if (!QDir().mkpath(paintRoot)) return 1;
        for (int i = 0; i < rows; ++i) {
            QFile file(QDir(paintRoot).filePath(QStringLiteral("paint-%1.jar").arg(i)));
            if (!file.open(QIODevice::WriteOnly) || file.write("fixture") < 0) return 1;
        }
        QElapsedTimer timer;
        timer.start();
        page.setDirectory(paintRoot); QApplication::processEvents();
        const qint64 buildUs = timer.nsecsElapsed() / 1000;
        check(list->count() == rows, "paint probe displays requested fixture rows");
        timer.restart();
        page.refresh(); QApplication::processEvents();
        const qint64 refreshUs = timer.nsecsElapsed() / 1000;
        auto *probeBar = list->verticalScrollBar();
        timer.restart();
        probeBar->setValue(probeBar->maximum()); QApplication::processEvents();
        const qint64 scrollUs = timer.nsecsElapsed() / 1000;
        timer.restart();
        page.setSearchText(QStringLiteral("paint-1"));
        QApplication::processEvents();
        const qint64 filterUs = timer.nsecsElapsed() / 1000;
        std::printf("FILES-PAINT-PROBE rows=%d build_us=%lld refresh_us=%lld scroll_us=%lld filter_us=%lld filtered=%d\n",
            rows, static_cast<long long>(buildUs), static_cast<long long>(refreshUs),
            static_cast<long long>(scrollUs), static_cast<long long>(filterUs), list->count());
        page.setSearchText(QString());
    }
    const auto emptyDirectory = dir.filePath(QStringLiteral("empty-mods"));
    QDir().mkpath(emptyDirectory);
    page.setDirectory(emptyDirectory); page.refresh(); QApplication::processEvents();
    check(page.downloadButton()->isVisible() && page.findChild<QPushButton *>(QStringLiteral("modHeaderMore"))->isVisible(),
          "empty mod directory retains install and folder maintenance actions");
    ModDependencies::UpdateVerdict oldA, oldB, newA;
    oldA.item.path = QStringLiteral("a.jar"); oldA.latestVersionId = QStringLiteral("old");
    oldB.item.path = QStringLiteral("b.jar"); newA = oldA; newA.latestVersionId = QStringLiteral("new");
    const auto merged = ModDependencies::mergeScopedUpdates({oldA, oldB}, {newA}, {QStringLiteral("a.jar")});
    check(merged.size() == 2 && merged.first().latestVersionId == QStringLiteral("new") &&
          merged.last().item.path == QStringLiteral("b.jar"), "partial check replaces selected result but preserves unrelated result");
    check(ModDependencies::mergeScopedUpdates({oldA, oldB}, {}, {QStringLiteral("a.jar")}).size() == 1,
          "unknown or removed selected file clears its old update verdict only");
    check(ModDependencies::mergeScopedUpdates({oldA, oldB}, {newA}, {}).size() == 1,
          "full-instance check replaces entire result");
    const QString sourceRoot = dir.filePath(QStringLiteral("source"));
    const QString targetRoot = dir.filePath(QStringLiteral("target"));
    QDir().mkpath(sourceRoot + QStringLiteral("/mods"));
    QDir().mkpath(targetRoot + QStringLiteral("/mods"));
    const auto sourcePath = [&](const QString &name) { return sourceRoot + QStringLiteral("/mods/") + name; };
    const auto targetPath = [&](const QString &name) { return targetRoot + QStringLiteral("/mods/") + name; };
    const auto writeFile = [](const QString &path, const QByteArray &data) {
        QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    };
    const auto readFile = [](const QString &path) {
        QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const QByteArray payload(1024 * 1024, 'm');
    if (!writeFile(sourcePath("copy.jar"), payload)) return 1;
    auto outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("copy.jar")}, false);
    if (outcome.failed) std::printf("transfer fixture: %s\n", qPrintable(outcome.problems.join(QStringLiteral("; "))));
    check(outcome.copied == 1 && outcome.failed == 0 && readFile(targetPath("copy.jar")) == payload &&
          QFileInfo::exists(sourcePath("copy.jar")), "streamed copy verifies content and preserves source");
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("copy.jar")}, true);
    check(outcome.skipped == 1 && QFileInfo::exists(sourcePath("copy.jar")), "existing destination is never overwritten or used to delete source");
    if (!writeFile(sourcePath("move.jar"), payload)) return 1;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("move.jar")}, true);
    check(outcome.moved == 1 && !QFileInfo::exists(sourcePath("move.jar")) &&
          readFile(targetPath("move.jar")) == payload, "move removes source only after verified publication");
    if (!writeFile(sourcePath("cancel.jar"), payload)) return 1;
    bool cancelFlag = true;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("cancel.jar")}, true, {}, [&] { return cancelFlag; });
    check(outcome.cancelled && QFileInfo::exists(sourcePath("cancel.jar")) &&
          !QFileInfo::exists(targetPath("cancel.jar")), "pre-cancel leaves source and target untouched");
    cancelFlag = false;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("cancel.jar")}, true,
        [&](int, int, const QString &detail) { if (detail.startsWith(QStringLiteral("校验"))) cancelFlag = true; },
        [&] { return cancelFlag; });
    check(outcome.cancelled && QFileInfo::exists(sourcePath("cancel.jar")) &&
          !QFileInfo::exists(targetPath("cancel.jar")), "cancellation before verification removes staging and keeps source");
    int polls = 0;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("cancel.jar")}, true, {},
        [&] { return ++polls >= 5; });
    check(outcome.cancelled && !QFileInfo::exists(targetPath("cancel.jar")) &&
          QFileInfo::exists(sourcePath("cancel.jar")), "mid-stream cancellation leaves no partial jar and preserves source");
    if (!writeFile(sourcePath("first.jar"), payload) || !writeFile(sourcePath("second.jar"), payload)) return 1;
    cancelFlag = false;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("first.jar"), sourcePath("second.jar")}, true,
        [&](int, int, const QString &detail) { if (detail.startsWith(QStringLiteral("完成"))) cancelFlag = true; },
        [&] { return cancelFlag; });
    check(outcome.cancelled && outcome.moved == 1 && QFileInfo::exists(targetPath("first.jar")) &&
          !QFileInfo::exists(sourcePath("first.jar")) && QFileInfo::exists(sourcePath("second.jar")),
          "partial cancellation retains completed move and untouched pending source");
    if (!writeFile(sourcePath("published.jar"), payload)) return 1;
    cancelFlag = false;
    int lastCompleted = -1;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("published.jar")}, true,
        [&](int completed, int, const QString &detail) {
            lastCompleted = completed;
            if (detail.startsWith(QStringLiteral("已复制"))) cancelFlag = true;
        },
        [&] { return cancelFlag; });
    check(outcome.cancelled && outcome.copied == 1 && outcome.moved == 0 && lastCompleted == 1 &&
          QFileInfo::exists(targetPath("published.jar")) && QFileInfo::exists(sourcePath("published.jar")),
          "cancellation between publish and source removal still counts the completed copy");
    if (!writeFile(sourcePath("changing.jar"), payload)) return 1;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("changing.jar")}, true,
        [&](int, int, const QString &detail) {
            if (detail.startsWith(QStringLiteral("校验"))) writeFile(sourcePath("changing.jar"), QByteArray("changed"));
        });
    check(outcome.failed == 1 && readFile(sourcePath("changing.jar")) == QByteArray("changed") &&
          !QFileInfo::exists(targetPath("changing.jar")), "changing source cannot be published or deleted");
    if (!writeFile(sourcePath("collision.jar"), payload)) return 1;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("collision.jar")}, true,
        [&](int, int, const QString &detail) {
            if (detail.startsWith(QStringLiteral("校验"))) writeFile(targetPath("collision.jar"), QByteArray("existing"));
        });
    check(outcome.failed == 1 && readFile(targetPath("collision.jar")) == QByteArray("existing") &&
          QFileInfo::exists(sourcePath("collision.jar")), "late destination collision never overwrites target or deletes source");
    if (!writeFile(sourcePath("disabled.jar.disabled"), payload) || !writeFile(targetPath("disabled.jar"), QByteArray("active"))) return 1;
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {sourcePath("disabled.jar.disabled")}, true);
    check(outcome.skipped == 1 && QFileInfo::exists(sourcePath("disabled.jar.disabled")), "enabled-disabled counterpart collision preserves source");
    outcome = TransferApply::transferMods(sourceRoot, sourceRoot, {sourcePath("copy.jar")}, true);
    check(outcome.failed == 1 && QFileInfo::exists(sourcePath("copy.jar")), "same-root transfer is rejected");
    outcome = TransferApply::transferMods(sourceRoot, targetRoot, {dir.filePath("Sodium.jar")}, true);
    check(outcome.failed == 1 && QFileInfo::exists(dir.filePath("Sodium.jar")), "paths outside source mods are rejected");
    check(QDir(targetRoot + QStringLiteral("/mods")).entryList({QStringLiteral(".wtfml-transfer-*")},
          QDir::Files | QDir::Hidden).isEmpty(), "cancelled and failed transfers leave no staging files");
    if (!writeFile(sourcePath("background.jar"), payload)) return 1;
    QFutureWatcher<TransferApply::ModTransferResult> watcher;
    QEventLoop loop;
    QTimer heartbeat; int beats = 0;
    QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] { ++beats; });
    QObject::connect(&watcher, &QFutureWatcher<TransferApply::ModTransferResult>::finished, &loop, &QEventLoop::quit);
    heartbeat.start(1);
    const QString backgroundFile = sourcePath("background.jar");
    watcher.setFuture(QtConcurrent::run([sourceRoot, targetRoot, backgroundFile] {
        return TransferApply::transferMods(sourceRoot, targetRoot, {backgroundFile}, false,
            [](int, int, const QString &) { QThread::msleep(15); });
    }));
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec(); heartbeat.stop();
    check(watcher.isFinished() && watcher.result().copied == 1 && beats > 0,
          "slow background transfer leaves GUI event loop responsive");
    ResourceCatalog::VersionEntry oldVersion, newVersion;
    oldVersion.id = QStringLiteral("old"); oldVersion.versionNumber = QStringLiteral("1.0");
    newVersion.id = QStringLiteral("new"); newVersion.versionNumber = QStringLiteral("2.0");
    oldVersion.dependencyMetadataKnown = newVersion.dependencyMetadataKnown = true;
    oldVersion.dependencies = {{QStringLiteral("removed"), {}, QStringLiteral("required")},
        {QStringLiteral("changed"), QStringLiteral("v1"), QStringLiteral("optional")}};
    newVersion.dependencies = {{QStringLiteral("added"), {}, QStringLiteral("required")},
        {QStringLiteral("changed"), QStringLiteral("v2"), QStringLiteral("required")},
        {QStringLiteral("conflict"), {}, QStringLiteral("incompatible")}};
    const QString diff = ModDependencies::updateDifferenceText(oldVersion, newVersion,
        {{QStringLiteral("changed"), QStringLiteral("v1")}, {QStringLiteral("conflict"), QStringLiteral("c1")}});
    check(diff.contains(QStringLiteral("1.0 → 2.0")), "preview displays current and target version");
    check(diff.contains(QStringLiteral("新增依赖：added")) && diff.contains(QStringLiteral("移除声明：removed")),
          "preview distinguishes added dependency and removed declaration");
    check(diff.contains(QStringLiteral("依赖变化：changed")) && diff.contains(QStringLiteral("固定版本 v2")),
          "preview detects dependency type and exact-version changes");
    check(diff.contains(QStringLiteral("版本不符")) && diff.contains(QStringLiteral("冲突提示")),
          "preview warns about known version mismatch and installed incompatible project");
    check(diff.contains(QStringLiteral("不代表确定未安装")) && diff.contains(QStringLiteral("不删除现有模组")),
          "incomplete inventory never claims missing dependency or deletes removed declaration");
    const QString plannedDiff = ModDependencies::updateDifferenceText(oldVersion, newVersion,
        {{QStringLiteral("changed"), QStringLiteral("v2")}});
    check(!plannedDiff.contains(QStringLiteral("版本不符")), "planned dependency version can satisfy exact pin");
    auto unknownCurrent = oldVersion; unknownCurrent.id.clear(); unknownCurrent.dependencyMetadataKnown = false;
    const QString unknownDiff = ModDependencies::updateDifferenceText(unknownCurrent, newVersion);
    check(unknownDiff.contains(QStringLiteral("当前依赖资料未知")) &&
          !unknownDiff.contains(QStringLiteral("新增依赖")) && !unknownDiff.contains(QStringLiteral("移除声明：")),
          "unknown baseline lists requirements without fabricated changes");
    auto unknownTarget = newVersion; unknownTarget.dependencyMetadataKnown = false;
    check(ModDependencies::updateDifferenceText(oldVersion, unknownTarget).contains(QStringLiteral("不能判断变化")),
          "unknown target dependencies are not treated as empty");
    auto malformed = newVersion; malformed.dependencies = {{QString(), QString(), QString()}};
    check(!ModDependencies::updateDifferenceText(malformed, malformed).contains(QStringLiteral("依赖声明未变化")),
          "malformed dependency data does not receive unchanged assurance");
    const auto explicitEmpty = ResourceCatalog::parseVersions(R"([{"id":"empty","dependencies":[]}])");
    const auto absentDeps = ResourceCatalog::parseVersions(R"([{"id":"unknown"}])");
    check(explicitEmpty.size() == 1 && explicitEmpty.first().dependencyMetadataKnown &&
          absentDeps.size() == 1 && !absentDeps.first().dependencyMetadataKnown,
          "parser distinguishes empty dependencies from missing metadata");
    oldVersion.files = {{QStringLiteral("https://example.invalid/old"), QStringLiteral("old.jar"), 1, QStringLiteral("oldhash"), true}};
    newVersion.files = {{QStringLiteral("https://example.invalid/new"), QStringLiteral("new.jar"), 1, QStringLiteral("newhash"), true}};
    ModDependencies::InstalledItem installed; installed.sha1 = QStringLiteral("oldhash"); installed.currentVersion = oldVersion;
    const auto fullVerdict = ModDependencies::judgeUpdate(installed, {newVersion, oldVersion});
    const auto batchVerdict = ModDependencies::judgeUpdateAgainstLatest(installed, newVersion);
    check(fullVerdict.currentVersion.id == QStringLiteral("old") && batchVerdict.currentVersion.id == QStringLiteral("old"),
          "full-list and batch update checks retain current metadata");
    InstanceIconLookup::Options cacheOptions;
    cacheOptions.cacheDir = dir.path(); cacheOptions.cacheKey = QStringLiteral("preview-cache");
    cacheOptions.requireVersionMetadata = true;
    const QFileInfo cachedFile(sourcePath("copy.jar"));
    ResourceCatalog::Hit hit; hit.projectId = QStringLiteral("project");
    QJsonObject cacheEntry{{"size", double(cachedFile.size())},
        {"mtime", double(cachedFile.lastModified().toMSecsSinceEpoch())}, {"sha1", "oldhash"},
        {"hit", ResourceCatalog::hitToJson(hit)},
        {"currentVersion", QJsonObject{{"id", "old"}, {"version_number", "1.0"}, {"dependencies", QJsonArray{}}}}};
    const QString cacheFile = InstanceIconLookup::cachePath(cacheOptions);
    if (!writeFile(cacheFile, QJsonDocument(QJsonObject{{"files", QJsonObject{{cachedFile.absoluteFilePath(), cacheEntry}}}}).toJson())) return 1;
    const auto cacheFirst = InstanceIconLookup::lookup({cachedFile.absoluteFilePath()}, cacheOptions);
    const auto cacheSecond = InstanceIconLookup::lookup({cachedFile.absoluteFilePath()}, cacheOptions);
    check(cacheFirst.hashed == 0 && cacheSecond.found.size() == 1 &&
          cacheSecond.found.first().currentVersion.id == QStringLiteral("old") &&
          cacheSecond.found.first().currentVersion.dependencyMetadataKnown,
          "current-version cache survives load-save-load without network or loss of known-empty dependencies");
    auto cachedMetadata = cacheEntry.value(QStringLiteral("currentVersion")).toObject();
    cachedMetadata.insert(QStringLiteral("dependencies"), QJsonArray{QJsonObject{
        {"project_id", "dep"}, {"version_id", "pin"}, {"dependency_type", "required"}}});
    cacheEntry.insert(QStringLiteral("currentVersion"), cachedMetadata);
    if (!writeFile(cacheFile, QJsonDocument(QJsonObject{{"files", QJsonObject{{cachedFile.absoluteFilePath(), cacheEntry}}}}).toJson())) return 1;
    InstanceIconLookup::lookup({cachedFile.absoluteFilePath()}, cacheOptions);
    const auto cachedDependencies = InstanceIconLookup::lookup({cachedFile.absoluteFilePath()}, cacheOptions);
    check(cachedDependencies.found.size() == 1 && cachedDependencies.found.first().currentVersion.dependencies.size() == 1 &&
          cachedDependencies.found.first().currentVersion.dependencies.first().versionId == QStringLiteral("pin"),
          "current dependency project, type and pin survive cache serialization");
    const auto cfVersions = ResourceCatalog::parseCurseForgeVersions(R"({"data":[{"id":7,"fileName":"x.jar","dependencies":[{"modId":1,"relationType":2},{"modId":2,"relationType":5},{"modId":3,"relationType":1}]}]})");
    check(cfVersions.size() == 1 && cfVersions.first().dependencyMetadataKnown && cfVersions.first().dependencies.size() == 3 &&
          cfVersions.first().dependencies.at(0).type == QStringLiteral("optional") &&
          cfVersions.first().dependencies.at(1).type == QStringLiteral("incompatible") &&
          cfVersions.first().dependencies.at(2).type == QStringLiteral("embedded"),
          "CurseForge optional, incompatible and embedded dependencies retain their semantics");
    ResourceCatalog::VersionEntry noDependencies; noDependencies.id = QStringLiteral("none"); noDependencies.dependencyMetadataKnown = true;
    check(ModDependencies::updateDifferenceText(noDependencies, noDependencies).contains(QStringLiteral("依赖声明未变化")),
          "two known-empty lists are genuinely unchanged");
    auto pinned = noDependencies; pinned.dependencies = {{QStringLiteral("dep"), QStringLiteral("pin"), QStringLiteral("required")}};
    const auto unknownPin = ModDependencies::updateDifferenceText(noDependencies, pinned, {{QStringLiteral("dep"), QString()}});
    check(unknownPin.contains(QStringLiteral("版本号未知")) && !unknownPin.contains(QStringLiteral("版本不符")),
          "unknown installed pin is not claimed as a definite mismatch");
    auto optional = noDependencies; optional.dependencies = {{QStringLiteral("extra"), {}, QStringLiteral("optional")}};
    check(!ModDependencies::updateDifferenceText(noDependencies, optional).contains(QStringLiteral("未在本次更新后的已识别清单中找到")),
          "optional dependency is not presented as mandatory missing content");
    auto versionConflict = noDependencies; versionConflict.dependencies = {{QStringLiteral("dep"), QStringLiteral("bad"), QStringLiteral("incompatible")}};
    check(!ModDependencies::updateDifferenceText(noDependencies, versionConflict, {{QStringLiteral("dep"), QStringLiteral("good")}})
          .contains(QStringLiteral("冲突提示")), "specific-version conflict does not flag a different known version");
    ModDependencies::InstalledItem inspected;
    inspected.path = QStringLiteral("/mods/owner.jar"); inspected.projectId = QStringLiteral("owner");
    inspected.title = QStringLiteral("Owner"); inspected.currentVersion = pinned;
    ModDependencies::InstalledItem dependency;
    dependency.path = QStringLiteral("/mods/dep.jar"); dependency.projectId = QStringLiteral("dep");
    dependency.title = QStringLiteral("Dependency"); dependency.currentVersion = noDependencies;
    dependency.currentVersion.id = QStringLiteral("pin");
    const auto inspection = ModDependencies::inspectInstalledDependencies({inspected}, {inspected, dependency});
    check(inspection.contains(QStringLiteral("当前版本")) && inspection.contains(QStringLiteral("声明：Dependency")) &&
          !inspection.contains(QStringLiteral("版本不符")), "read-only inspection resolves same-platform exact dependency from current inventory");
    auto disabledDependency = dependency; disabledDependency.path += QStringLiteral(".disabled");
    const auto disabledReport = ModDependencies::inspectInstalledDependencies({inspected}, {inspected, disabledDependency});
    check(disabledReport.contains(QStringLiteral("需确认")) && disabledReport.contains(QStringLiteral("不代表确定未安装")),
          "disabled dependency cannot satisfy active dependency but absence is not overstated");
    auto foreignDependency = dependency; foreignDependency.source = ResourceCatalog::Source::CurseForge;
    check(ModDependencies::inspectInstalledDependencies({inspected}, {inspected, foreignDependency})
          .contains(QStringLiteral("需确认")), "project identifiers from different platforms never satisfy each other");
    auto unknownItem = inspected; unknownItem.currentVersion = {};
    check(ModDependencies::inspectInstalledDependencies({unknownItem}, {unknownItem}).contains(QStringLiteral("资料未知")),
          "unknown current dependency metadata is not presented as no dependencies");
    auto duplicate = dependency; duplicate.path = QStringLiteral("/mods/dep-other.jar");
    const auto duplicateReport = ModDependencies::inspectInstalledDependencies({inspected}, {inspected, dependency, duplicate});
    check(duplicateReport.contains(QStringLiteral("重复启用")) && duplicateReport.contains(QStringLiteral("dep-other.jar")) &&
          duplicateReport.contains(QStringLiteral("版本号未知")), "duplicate active project files are reported without choosing a winning version");
    check(!ModDependencies::inspectInstalledDependencies({inspected}, {inspected, dependency, dependency})
          .contains(QStringLiteral("重复启用")), "same physical inventory entry is not counted twice");
    check(!ModDependencies::inspectInstalledDependencies({dependency}, {dependency, foreignDependency})
          .contains(QStringLiteral("重复启用")), "same project number across providers is not reported as a duplicate");
    auto inactive = inspected; inactive.path += QStringLiteral(".disabled");
    const auto inactiveReport = ModDependencies::inspectInstalledDependencies({inactive}, {inactive});
    check(inactiveReport.contains(QStringLiteral("当前未启用")) && !inactiveReport.contains(QStringLiteral("版本不符")),
          "selected inactive mod only displays declarations without active dependency warnings");
    auto conflictOwner = inspected; conflictOwner.currentVersion = versionConflict;
    auto badDependency = dependency; badDependency.currentVersion.id = QStringLiteral("bad");
    check(ModDependencies::inspectInstalledDependencies({conflictOwner}, {conflictOwner, badDependency})
          .contains(QStringLiteral("冲突提示")), "inspection reports explicit incompatibility against current known version");
    auto missingIdentity = dependency; missingIdentity.projectId.clear();
    check(ModDependencies::inspectInstalledDependencies({inspected}, {inspected, missingIdentity})
          .contains(QStringLiteral("未识别启用文件 1 项")), "inspection quantifies unknown active files instead of claiming a complete scan");
    cacheOptions.cacheOnly = true;
    cacheOptions.modrinthApiBaseOverride = QStringLiteral("http://127.0.0.1:1");
    cacheOptions.curseForgeApiBaseOverride = QStringLiteral("http://127.0.0.1:1");
    cacheOptions.cacheKey = QStringLiteral("missing-read-only-cache");
    const auto cacheMiss = InstanceIconLookup::lookup({cachedFile.absoluteFilePath()}, cacheOptions);
    check(cacheMiss.hashed == 0 && cacheMiss.found.isEmpty() && !QFileInfo::exists(InstanceIconLookup::cachePath(cacheOptions)),
          "read-only cache miss never hashes, contacts a provider, or creates cache files");
    cacheOptions.cacheKey = QStringLiteral("preview-cache");
    cacheEntry.remove(QStringLiteral("currentVersion"));
    const QByteArray oldCache = QJsonDocument(QJsonObject{{"files", QJsonObject{{cachedFile.absoluteFilePath(), cacheEntry}}}}).toJson();
    if (!writeFile(cacheFile, oldCache)) return 1;
    const auto oldReadOnly = InstanceIconLookup::lookup({cachedFile.absoluteFilePath()}, cacheOptions);
    check(oldReadOnly.found.size() == 1 && oldReadOnly.found.first().currentVersion.id.isEmpty() && readFile(cacheFile) == oldCache,
          "read-only inspection accepts old identity cache without metadata upgrades or writes");
    if (!writeFile(cachedFile.absoluteFilePath(), QByteArray("file changed"))) return 1;
    const auto staleReadOnly = InstanceIconLookup::lookup({cachedFile.absoluteFilePath()}, cacheOptions);
    check(staleReadOnly.found.isEmpty() && staleReadOnly.hashed == 0 && readFile(cacheFile) == oldCache,
          "read-only inspection rejects stale file metadata without rewriting cache");
    return failed ? 1 : 0;
}

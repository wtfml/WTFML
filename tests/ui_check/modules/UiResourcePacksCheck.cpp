#include "Support.hpp"
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QMimeData>
int UiResourcePacksCheck(QApplication &app) {

        const int fontId = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc")));
        const auto families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) app.setFont(QFont(families.first(), 9));
        app.setStyle(Theme::createStyle());
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        int failures = 0;
        const auto expect = [&failures](bool ok, const char *message) {
            printf("%s %s\n", ok ? "PASS" : "FAIL", message);
            if (!ok) ++failures;
        };
        using namespace ResourcePackOptions;
        const QByteArray original = QByteArray("\xef\xbb\xbf")
            + "fov:0.5\r\nresourcePacks:[\"vanilla\"]\r\nunknown:keep:me\r\n"
              "resourcePacks:[\"vanilla\",\"file/a.zip\"]\r\nincompatibleResourcePacks:[\"file/a.zip\"]";
        const auto document = parse(original);
        expect(document.problem.isEmpty() && document.selected == QStringList{"vanilla", "file/a.zip"},
               "parse final resourcePacks line, keep builtin ids");
        QByteArray output;
        QString problem;
        expect(rewrite(document, document.selected, &output, &problem) && output == original,
               "no-op preserves every byte including BOM and CRLF");
        expect(rewrite(document, {"vanilla", "file/b.zip", "file/a.zip"}, &output, &problem)
                   && output.startsWith(QByteArray("\xef\xbb\xbf") + "fov:0.5\r\nresourcePacks:[\"vanilla\"]\r\nunknown:keep:me\r\n")
                   && output.endsWith("\r\nincompatibleResourcePacks:[\"file/a.zip\"]")
                   && parse(output).selected == QStringList{"vanilla", "file/b.zip", "file/a.zip"},
               "rewrite only last resourcePacks value, preserve other settings");
        expect(!rewrite(document, {"file/a.zip"}, &output, &problem), "cannot remove builtin packs");
        expect(!rewrite(document, {"vanilla", "file/a.zip", "file/a.zip"}, &output, &problem), "reject duplicate ids");
        expect(!parse("resourcePacks:[123]").problem.isEmpty()
                   && !parse("resourcePacks:broken").problem.isEmpty(), "malformed lists are read-only");
        expect(!safeLocal("file/../evil.zip") && !safeLocal("file/C:evil.zip")
                   && !safeLocal("file/a/b.zip") && safeLocal("file/中文包.zip"), "local identifiers cannot escape directory");
        expect(rewrite(parse("fov:0.7"), {"file/a.zip"}, &output, &problem)
                   && output == "fov:0.7\nresourcePacks:[\"file/a.zip\"]\n", "missing key appends without damaging final line");
        QTemporaryDir scratch(QDir::current().filePath(QStringLiteral(".resource-packs-XXXXXX")));
        if (!scratch.isValid()) return 1;
        const QString path = scratch.path() + QStringLiteral("/options.txt");
        const QString backups = scratch.path() + QStringLiteral("/backups");
        const auto write = [&](const QByteArray &bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); };
        const auto bytesAt = [](const QString &filePath) { QFile file(filePath); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); };
        expect(write(original), "create options fixture");
        expect(save(path, original, true, {"vanilla", "file/a.zip", "file/b.zip"}, {"file/a.zip", "file/b.zip"}, backups, &problem),
               "save enabled order with atomic backup");
        const auto copies = QDir(backups).entryList(QDir::Files);
        expect(copies.size() == 1 && bytesAt(backups + QLatin1Char('/') + copies.value(0)) == original,
               "backup contains exact previous bytes");
        const QByteArray saved = bytesAt(path);
        expect(!save(path, original, true, document.selected, {"file/a.zip"}, backups, &problem) && bytesAt(path) == saved,
               "stale snapshot cannot overwrite external changes");
        expect(!save(path, saved, true, {"vanilla", "file/missing.zip"}, {}, backups, &problem) && bytesAt(path) == saved,
               "cannot enable a vanished pack");
        const QString blockedBackup = scratch.path() + QStringLiteral("/not-a-directory");
        { QFile blocker(blockedBackup); expect(blocker.open(QIODevice::WriteOnly), "create blocked backup fixture"); }
        expect(!save(path, saved, true, {"vanilla"}, {}, blockedBackup, &problem) && bytesAt(path) == saved,
               "backup failure leaves original options intact");
        {
            QLockFile lock(path + QStringLiteral(".wtfml.lock"));
            expect(lock.tryLock(0) && !save(path, saved, true, {"vanilla"}, {}, backups, &problem),
                   "another writer blocks concurrent save");
        }
        ResourcePackPage page;
        const QList<ResourcePackPage::Pack> packs{{"file/a.zip", "Alpha", "green"}, {"file/b.zip", "Beta", "blue"}};
        page.setPacks(packs, {"vanilla", "file/a.zip"}, original, true);
        auto *available = page.findChild<QListWidget *>(QStringLiteral("resourcePackAvailable"));
        auto *selected = page.findChild<QListWidget *>(QStringLiteral("resourcePackSelected"));
        auto *enable = page.findChild<QPushButton *>(QStringLiteral("resourcePackEnable"));
        auto *disable = page.findChild<QPushButton *>(QStringLiteral("resourcePackDisable"));
        auto *down = page.findChild<QPushButton *>(QStringLiteral("resourcePackDown"));
        QStringList requested;
        int requests = 0;
        QObject::connect(&page, &ResourcePackPage::selectionRequested, &page,
            [&](const QStringList &ids, const QByteArray &token, bool exists) {
                requested = ids; ++requests;
                expect(token == original && exists, "selection carries original conflict token");
            });
        expect(available->count() == 1 && selected->item(0)->text() == QStringLiteral("Alpha"),
               "two sections exclude active files from available, high priority first");
        available->setCurrentRow(0);
        enable->click();
        expect(requested == QStringList{"vanilla", "file/a.zip", "file/b.zip"}, "enable appends highest priority without file rename");
        down->click();
        expect(requested == QStringList{"vanilla", "file/b.zip", "file/a.zip"}, "priority button maps correctly to persisted order");
        page.setPacks(packs, {"vanilla", "file/a.zip", "file/b.zip"}, original, true);
        const bool moved = selected->model()->moveRows(QModelIndex(), 0, 1, QModelIndex(), 2);
        app.processEvents();
        expect(moved && requested == QStringList{"vanilla", "file/b.zip", "file/a.zip"},
               "internal drag reorder submits after model finishes moving rows");
        selected->setCurrentRow(selected->count() - 1);
        expect(!disable->isEnabled(), "builtin pack cannot be disabled");
        selected->setCurrentRow(0);
        disable->click();
        expect(requested == QStringList{"vanilla", "file/b.zip"}, "disable removes only selected local identifier");
        page.setPacks(packs, {"vanilla", "file/missing.zip"}, original, true);
        expect(selected->item(0)->text().contains(QStringLiteral("缺失")), "missing active file remains visible");
        selected->setCurrentRow(0);
        disable->click();
        expect(requested == QStringList{"vanilla"} && available->count() == 2, "missing file can be removed but cannot become available");
        page.setPacks(packs, {"vanilla"}, original, true, QStringLiteral("running"));
        available->setCurrentRow(0);
        const int before = requests;
        enable->click();
        expect(!enable->isEnabled() && requests == before, "read-only state blocks all selection changes");
        {
            // #136：「已启用」在上、「可用资源包」在下，两张表按行高摊开，整页只有外层一条竖滑条。
            QList<ResourcePackPage::Pack> many;
            QStringList active{"vanilla"};
            for (int i = 0; i < 30; ++i) {
                const QString id = QStringLiteral("file/p%1.zip").arg(i, 2, 10, QLatin1Char('0'));
                many.append({id, QStringLiteral("Pack %1").arg(i), QString()});
                if (i < 12) active.append(id);
            }
            page.setPacks(many, active, original, true);
            page.resize(960, 620);
            page.show();
            app.processEvents();
            int verticalBars = 0;
            for (auto *bar : page.findChildren<QScrollBar *>())
                if (bar->orientation() == Qt::Vertical && bar->isVisible()) ++verticalBars;
            expect(selected->count() == 13 && available->count() == 18 &&
                       selected->verticalScrollBar()->maximum() == 0 && available->verticalScrollBar()->maximum() == 0,
                   "both pack lists fit their rows without inner scrolling");
            expect(verticalBars <= 1 && selected->mapTo(&page, QPoint(0, selected->height())).y() <=
                                            available->mapTo(&page, QPoint()).y(),
                   "enabled section sits above available section with one vertical scrollbar");
            available->setFocus();
            available->setCurrentRow(0);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            app.sendEvent(available, &enter);
            expect(requested.size() == active.size() + 1 && requested.last() == QStringLiteral("file/p12.zip"),
                   "Enter on an available pack enables it");
            // 拖动排序：光标停在外层视口下边缘时外层持续往下滚，离开列表就停。
            auto *scroll = page.findChild<QScrollArea *>(QStringLiteral("resourcePackScroll"));
            QScrollBar *bar = scroll->verticalScrollBar();
            QStringList all{"vanilla"};
            for (const auto &pack : many) all.append(pack.id);
            page.setPacks(many, all, original, true);
            app.processEvents();
            bar->setValue(0);
            const QPoint edge = selected->viewport()->mapFrom(scroll->viewport(), QPoint(20, scroll->viewport()->height() - 2));
            QMimeData mime;
            QDragMoveEvent move(edge, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            app.sendEvent(selected->viewport(), &move);
            QElapsedTimer clock;
            clock.start();
            while (bar->value() == 0 && clock.elapsed() < 2000) app.processEvents(QEventLoop::AllEvents, 20);
            expect(bar->maximum() > 0 && bar->value() > 0, "dragging near the bottom edge scrolls the outer area");
            QDragLeaveEvent leave;
            app.sendEvent(selected->viewport(), &leave);
            const int stopped = bar->value();
            clock.restart();
            while (clock.elapsed() < 100) app.processEvents(QEventLoop::AllEvents, 20);
            expect(bar->value() == stopped, "leaving the list stops the outer scroll");
            page.hide();
        }
        if (app.arguments().contains(QStringLiteral("--resource-packs-preview"))) {
            page.setPacks(packs, {"vanilla", "file/a.zip"}, original, true);
            page.resize(820, 480);
            page.show();
            app.processEvents();
            expect(page.grab().save(QDir::current().filePath(QStringLiteral("build/resource-packs-preview.png"))),
                   "resource pack preview saved");
        }
        return failures == 0 ? 0 : 1;
    
 return 0;
}


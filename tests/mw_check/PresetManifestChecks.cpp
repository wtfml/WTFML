#include "PresetManifestChecks.hpp"
#include "controllers/PresetController.hpp"
#include "presets/PresetCatalog.hpp"
#include "presets/PresetManifestLoader.hpp"
#include "LoadingState.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QPushButton>
#include <QSemaphore>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <memory>

int runPresetManifestChecks() {
    int failures = 0;
    const auto check = [&](bool ok, const char *label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    const auto pump = [](const std::function<bool()> &done) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < 4000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        QApplication::processEvents();
        return done();
    };
    struct Gate {
        QSemaphore release;
        std::atomic<int> calls{0};
        std::atomic<bool> fail{false};
    };
    const auto a = std::make_shared<Gate>();
    const auto b = std::make_shared<Gate>();
    const auto c = std::make_shared<Gate>();
    const QString idA = Presets::newPresetId(), idB = Presets::newPresetId(), idC = Presets::newPresetId();
    const auto reader = [a, b, c, idA, idB](const QString &, const QString &id) {
        const auto gate = id == idA ? a : id == idB ? b : c;
        ++gate->calls;
        (void)gate->release.tryAcquire(1, 3000);
        Presets::ManifestSnapshot snapshot;
        snapshot.ok = !gate->fail;
        snapshot.problem = gate->fail ? QStringLiteral("fixture read failure") : QString();
        snapshot.manifest.id = id;
        snapshot.manifest.name = QStringLiteral("content-") + id;
        snapshot.manifest.category = Presets::PresetCategory::GameSettings;
        return snapshot;
    };
    Presets::CatalogSnapshot catalog;
    for (const auto &id : {idA, idB, idC}) {
        Presets::PresetSummary summary;
        summary.id = id; summary.name = id; summary.category = Presets::PresetCategory::GameSettings;
        catalog.presets.append(summary);
    }
    QWidget host;
    PresetController::Host callbacks;
    callbacks.readCatalog = [catalog](const QString &) { return catalog; };
    callbacks.readManifest = reader;
    PresetController controller(nullptr, &host, callbacks);
    controller.open();
    check(pump([&] { return a->calls == 1; }), "preset detail read starts in background");
    auto *page = host.findChild<PresetManagePage *>();
    if (!page) return failures + 1;
    auto *list = page->findChild<QTreeWidget *>(PresetManagePage::presetListName());
    // 左栏是**按类别分组**的一棵树（组头 + 预设行）：按 id 认行，组头不算行。
    const auto presetRow = [](QTreeWidget *tree, const QString &id) -> QTreeWidgetItem * {
        if (tree == nullptr) return nullptr;
        for (int g = 0; g < tree->topLevelItemCount(); ++g) {
            QTreeWidgetItem *group = tree->topLevelItem(g);
            for (int i = 0; i < group->childCount(); ++i) {
                if (group->child(i)->data(0, PresetManagePage::rowIdRole()).toString() == id)
                    return group->child(i);
            }
        }
        return nullptr;
    };
    check(presetRow(list, idA) && presetRow(list, idB) && presetRow(list, idC),
          "grouped preset list lists every catalog entry as a row");
    auto *name = page->findChild<QLineEdit *>(PresetManagePage::nameEditName());
    auto *save = page->findChild<QPushButton *>(PresetManagePage::saveDetailsButtonName());
    auto *create = page->findChild<QPushButton *>(PresetManagePage::newButtonName());
    bool input = false;
    QTimer::singleShot(0, page, [&] { input = true; });
    check(pump([&] { return input; }) && page->isBusy() && name->text().isEmpty() && !save->isEnabled(),
          "input remains responsive and stale detail editing is disabled during read");
    list->setCurrentItem(presetRow(list, idB));
    list->setCurrentItem(presetRow(list, idC));
    a->release.release();
    check(pump([&] { return c->calls == 1; }) && b->calls == 0 && page->isBusy() && name->text().isEmpty(),
          "rapid selection reads only latest pending manifest and discards old detail");
    c->release.release();
    check(pump([&] { return !controller.detailsLoading(); }) && !page->isBusy() && save->isEnabled() &&
          name->text() == QStringLiteral("content-") + idC, "latest detail alone restores matching editor");

    list->setCurrentItem(nullptr);
    check(page->currentPresetId().isEmpty() && name->text().isEmpty() && !save->isEnabled(),
          "clearing current index removes stale details without enabling writes");
    list->setCurrentItem(presetRow(list, idA));
    check(pump([&] { return a->calls == 2; }), "filter invalidation fixture is reading");
    page->findChild<QLineEdit *>(QStringLiteral("presetSearchEdit"))->setText(QStringLiteral("no-match"));
    check(page->currentPresetId().isEmpty() && !page->isBusy() && create->isEnabled(),
          "filtering out selection cancels requested detail and restores create action immediately");
    a->release.release();
    check(pump([&] { return !controller.detailsLoading(); }) && name->text().isEmpty() && !save->isEnabled(),
          "late detail cannot repopulate an empty filtered selection");

    check(page->captureChoice().vanillaOptions && page->captureChoice().vanillaKeys,
          "background loading preserves compatible capture checkboxes");
    c->fail = true;
    a->release.release(); c->release.release();
    page->findChild<QLineEdit *>(QStringLiteral("presetSearchEdit"))->clear();
    list->setCurrentItem(presetRow(list, idC));
    // 读不出来那句话今天摆在**内容那一格**上（`PresetManagePage::showPresetReadError()` 换成
    // 了 `LoadingState::fail(m_sectionList, …)`：原因 + 「重试」盖在内容列表上），页面底部那
    // 一行状态只剩"用户刚做了什么"的反馈。断言跟着出口走，别钉在搬过家的控件上。
    auto *sections = page->findChild<QListWidget *>(PresetManagePage::sectionListName());
    const auto readError = [sections] {
        QWidget *layer = sections != nullptr ? LoadingState::overlay(sections) : nullptr;
        return layer != nullptr && layer->property("state").toString() == QStringLiteral("error")
                   ? layer->accessibleName()
                   : QString();
    };
    check(pump([&] { return !controller.detailsLoading(); }) && !page->isBusy() && !save->isEnabled() &&
          create->isEnabled() && readError().contains(QStringLiteral("fixture read failure")) &&
          page->findChild<QLabel *>(QStringLiteral("presetMetaLabel"))->text().contains(QStringLiteral("无法读取")),
          "failed manifest read reports reason without enabling stale editor");

    const int before = a->calls;
    list->setCurrentItem(presetRow(list, idA));
    check(pump([&] { return a->calls > before; }), "page destruction fixture is reading");
    delete page;
    a->release.release();
    check(pump([&] { return !controller.detailsLoading(); }) && !host.findChild<PresetManagePage *>(),
          "manifest completion tolerates page deletion while controller survives");

    c->fail = false;
    int deliveries = 0;
    QString deliveredRoot;
    Presets::ManifestLoader loader(nullptr, reader);
    const int oldCalls = a->calls;
    loader.load(QStringLiteral("old-root"), idA, [&](const auto &) { ++deliveries; });
    check(pump([&] { return a->calls > oldCalls; }), "root switch fixture is reading");
    loader.load(QStringLiteral("new-root"), idC, [&](const auto &result) { ++deliveries; deliveredRoot = result.root; });
    a->release.release(); c->release.release();
    check(pump([&] { return !loader.isReading() && !loader.isLoading(); }) && deliveries == 1 && deliveredRoot == QStringLiteral("new-root"),
          "old data root cannot publish after a newer manifest request");
    {
        QTemporaryDir root;
        Presets::PresetManifest manifest;
        manifest.id = Presets::newPresetId();
        manifest.name = QStringLiteral("large manifest fixture");
        manifest.category = Presets::PresetCategory::GameSettings;
        const bool created = root.isValid() && Presets::saveManifest(root.path(), manifest);
        check(created, "large manifest fixture uses temporary storage");
        if (!created) return failures;
        auto object = QJsonDocument::fromJson(Presets::manifestToBytes(manifest)).object();
        object.insert(QStringLiteral("testPadding"), QString(4 * 1024 * 1024, QLatin1Char('x')));
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QFile file(Presets::manifestPath(root.path(), manifest.id));
        const bool written = file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
        file.close();
        check(written && bytes.size() > 4 * 1024 * 1024, "real large manifest bytes are written");
        if (!written) return failures;
        bool finished = false;
        Presets::ManifestSnapshot loaded;
        Presets::ManifestLoader diskLoader(nullptr);
        const auto complete = [&](const auto &result) { loaded = result; finished = true; };
        diskLoader.load(root.path(), manifest.id, complete);
        check(pump([&] { return finished; }) && loaded.ok && loaded.manifest.name == manifest.name,
              "default background reader parses a real 4 MiB manifest");
        const bool corrupted = file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write("{broken") == 7;
        file.close();
        finished = false;
        diskLoader.load(root.path(), manifest.id, complete);
        check(corrupted && pump([&] { return finished; }) && !loaded.ok && !loaded.problem.isEmpty(),
              "default reader reports corruption instead of reusing earlier success");
    }
    return failures;
}

#include "PresetPreviewChecks.hpp"
#include "controllers/PresetController.hpp"
#include "InstanceGroupDelegate.hpp"
#include "PresetEditorPage.hpp"
#include <QLineEdit>
#include <QLabel>
#include <QKeyEvent>
#include "presets/PresetCatalog.hpp"
#include "presets/PresetManifestLoader.hpp"
#include "presets/PresetPreviewLoader.hpp"
#include "util/AppPaths.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QDialog>
#include <QEventLoop>
#include <QListWidget>
#include <QPushButton>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <memory>

int runPresetPreviewChecks() {
    int failures = 0;
    const auto check = [&](bool ok, const char *text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const auto pump = [](const std::function<bool()> &done) {
        QElapsedTimer timer; timer.start();
        while (!done() && timer.elapsed() < 4000) {
            QApplication::processEvents(); QThread::msleep(1);
        }
        QApplication::processEvents();
        return done();
    };
    struct Gate { QSemaphore release; std::atomic<int> calls{0}; std::atomic<bool> fail{false}; };
    const auto one = std::make_shared<Gate>(), two = std::make_shared<Gate>();
    const QString presetA = Presets::newPresetId(), presetB = Presets::newPresetId();
    Presets::CatalogSnapshot catalog;
    for (const auto &id : {presetA, presetB}) {
        Presets::PresetSummary item;
        item.id = id; item.name = id; item.category = Presets::PresetCategory::GameSettings;
        catalog.presets.append(item);
    }
    bool running = false, busy = false;
    QWidget host;
    PresetController::Host callbacks;
    callbacks.isRunning = [&](const QString &) { return running; };
    callbacks.isInstanceBusy = [&](const QString &) { return busy; };
    callbacks.readCatalog = [catalog](const QString &) { return catalog; };
    callbacks.readManifest = [](const QString &, const QString &id) {
        Presets::ManifestSnapshot result;
        result.ok = true; result.manifest.id = id; result.manifest.name = id;
        result.manifest.category = Presets::PresetCategory::GameSettings;
        return result;
    };
    callbacks.readPreview = [one, two](const Presets::PreviewRequest &request) {
        const auto gate = request.instanceId == QLatin1String("one") ? one : two;
        ++gate->calls;
        (void)gate->release.tryAcquire(1, 3000);
        Presets::PreviewSnapshot result;
        result.ok = !gate->fail;
        result.problem = gate->fail ? QStringLiteral("fixture preview failure") : QString();
        result.plan.ok = true;
        Presets::ApplyPlanRow row;
        row.kind = Presets::SectionKind::VanillaOptions; row.count = 1;
        row.title = request.instanceId + QStringLiteral(" plan");
        result.plan.rows.append(row);
        return result;
    };
    PresetController controller(nullptr, &host, callbacks);
    controller.open();
    check(pump([&] { return !controller.catalogLoading() && !controller.detailsLoading(); }), "preview fixture catalog and detail finish");
    auto *page = host.findChild<PresetManagePage *>();
    if (!page) return failures + 1;
    page->setInstances({{QStringLiteral("one"), QStringLiteral("one"), false}, {QStringLiteral("two"), QStringLiteral("two"), false}});
    auto *target = page->findChild<QComboBox *>(PresetManagePage::applyInstanceComboName());
    auto *preview = page->findChild<QPushButton *>(PresetManagePage::previewButtonName());
    auto *apply = page->findChild<QPushButton *>(PresetManagePage::applyButtonName());
    auto *plan = page->findChild<QListWidget *>(PresetManagePage::planListName());
    auto *presets = page->findChild<QTreeWidget *>(PresetManagePage::presetListName());
    // 列表按类别分组：按 id 认行（组头不算行）。
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
    preview->click();
    bool input = false;
    QTimer::singleShot(0, page, [&] { input = true; });
    check(pump([&] { return one->calls == 1 && input; }) && page->isBusy() && preview->isEnabled() && !apply->isEnabled(),
          "preview preparation leaves GUI responsive and cancel enabled while apply is disabled");
    preview->click();
    check(!page->isBusy() && plan->count() == 0, "cancel preview immediately restores editing without retaining a plan");
    one->release.release();
    check(pump([&] { return !controller.previewLoading(); }) && plan->count() == 0 && !apply->isEnabled(), "cancelled preview cannot publish late");

    preview->click();
    check(pump([&] { return one->calls == 2; }), "target switch fixture is running");
    target->setCurrentIndex(1); preview->click();
    target->setCurrentIndex(0); target->setCurrentIndex(1); preview->click();
    one->release.release();
    check(pump([&] { return two->calls == 1; }) && plan->count() == 0 && page->isBusy(), "target switch discards old preview and coalesces pending work");
    two->release.release();
    check(pump([&] { return !controller.previewLoading(); }) && apply->isEnabled() && plan->count() == 1 &&
          plan->item(0)->text().contains(QStringLiteral("two plan")), "only matching target plan enables apply");

    page->setInstances({{QStringLiteral("one"), QStringLiteral("one"), false}, {QStringLiteral("two"), QStringLiteral("two"), false}}, QStringLiteral("two"));
    check(page->selectedApplyInstanceId() == QLatin1String("two") && plan->count() == 0 && !apply->isEnabled(),
          "instance refresh invalidates completed plan even when target id stays unchanged");
    preview->click();
    check(pump([&] { return two->calls == 2; }), "running-state change fixture is running");
    running = true; two->release.release();
    check(pump([&] { return !controller.previewLoading(); }) && !apply->isEnabled() && plan->count() == 1 &&
          plan->item(0)->text().contains(QStringLiteral("状态已改变")), "changed game state invalidates prepared plan");
    running = false;
    two->fail = true; preview->click();
    check(pump([&] { return two->calls == 3; }), "preview failure fixture is running");
    two->release.release();
    check(pump([&] { return !controller.previewLoading(); }) && !page->isBusy() && !apply->isEnabled() && plan->count() == 1 &&
          plan->item(0)->text().contains(QStringLiteral("fixture preview failure")), "preparation failure shows its reason without enabling apply");
    two->fail = false;

    preview->click();
    check(pump([&] { return two->calls == 4; }), "preset switch fixture is running");
    presets->setCurrentItem(presetRow(presets, presetB));
    check(pump([&] { return !controller.detailsLoading(); }) && !page->isBusy(), "preset switch restores new detail without waiting for discarded preview");
    two->release.release();
    check(pump([&] { return !controller.previewLoading(); }) && plan->count() == 0 && !apply->isEnabled(), "old preset preview cannot replace new selection");

    QTemporaryDir changedRoot;
    check(changedRoot.isValid(), "data-root change fixture is isolated");
    if (changedRoot.isValid()) {
        const QString previous = AppPaths::minecraftDirOverride();
        preview->click();
        check(pump([&] { return two->calls == 5; }), "data-root change fixture is running");
        AppPaths::setMinecraftDir(changedRoot.path());
        two->release.release();
        check(pump([&] { return !controller.previewLoading(); }) && plan->count() == 0 && !apply->isEnabled(), "changed target path cannot accept old plan");
        AppPaths::setMinecraftDir(previous);
    }
    busy = true;
    const int callsBeforeBusy = two->calls;
    preview->click();
    check(two->calls == callsBeforeBusy && !apply->isEnabled() && plan->count() == 1 && plan->item(0)->text().contains(QStringLiteral("正在处理文件")),
          "busy instance rejects preview before starting disk work");
    busy = false;
    const int beforeRemoval = two->calls;
    preview->click();
    check(pump([&] { return two->calls > beforeRemoval; }), "target removal fixture is running");
    page->setInstances({{QStringLiteral("one"), QStringLiteral("one"), false}});
    two->release.release();
    check(pump([&] { return !controller.previewLoading(); }) && !page->isBusy() && plan->count() == 0 && !apply->isEnabled(),
          "refresh removing target cancels old plan even when combo signals were blocked");
    const int beforeDeletion = one->calls;
    preview->click();
    check(pump([&] { return one->calls > beforeDeletion; }), "preview page destruction fixture is running");
    delete page; one->release.release();
    check(pump([&] { return !controller.previewLoading(); }) && !host.findChild<PresetManagePage *>(), "preview completion tolerates page deletion");

    // The production reader must remain read-only: no options writes, backups,
    // staging files, or changed timestamps while calculating an application.
    QTemporaryDir disk;
    check(disk.isValid(), "real preview fixture uses temporary storage");
    if (!disk.isValid()) return failures;
    Presets::PresetManifest manifest;
    manifest.id = Presets::newPresetId(); manifest.name = QStringLiteral("preview");
    manifest.category = Presets::PresetCategory::GameSettings;
    manifest.sections = Presets::sectionsFromOptions(Presets::parseOptions(QByteArrayLiteral("fov:0.5\n")), {QStringLiteral("fov")});
    const QString instance = disk.path() + QStringLiteral("/instance");
    const QString store = disk.path() + QStringLiteral("/presets");
    QDir().mkpath(instance);
    QFile options(instance + QStringLiteral("/options.txt"));
    const QByteArray original("fov:0.0\nunknown:keep\n");
    const bool written = options.open(QIODevice::WriteOnly) && options.write(original) == original.size();
    options.close();
    const bool stored = Presets::saveManifest(store, manifest);
    check(written && stored, "real preview source and target are prepared");
    if (!written || !stored) return failures;
    const auto timestamp = QFileInfo(options).lastModified();
    const auto tree = [&] {
        QStringList paths;
        QDirIterator it(disk.path(), QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) paths.append(it.next());
        paths.sort(); return paths;
    };
    const auto beforeTree = tree();
    bool done = false;
    Presets::PreviewSnapshot result;
    Presets::PreviewLoader loader(nullptr);
    loader.load({store, manifest.id, QStringLiteral("fixture"), instance, true, false}, [&](const auto &snapshot) { result = snapshot; done = true; });
    check(pump([&] { return done; }) && result.ok && result.plan.ok && result.plan.totalCount() == 1,
          "default background worker reads real target and computes the application plan");
    check(options.open(QIODevice::ReadOnly) && options.readAll() == original && QFileInfo(options).lastModified() == timestamp && tree() == beforeTree,
          "preview leaves target bytes, timestamps and directory tree unchanged");
    options.close();
    {
        QTemporaryDir editorFixture;
        const auto previousDefault=qApp->property("wtfml.testDefaultMinecraftDir");
        qApp->setProperty("wtfml.testDefaultMinecraftDir",editorFixture.path());
        QWidget host;
        // 编辑器现在是**整页**（铺在内容区上，和实例设置同一条路）。`showPage()` 摆上去
        // 就返回，收页之后才叫那条回调 —— 和真覆盖层同一个语义。验证工程没有真覆盖层，
        // 这里给它一个假的：把页塞进宿主、show()，`finished` 一响就把结果交回去。
        PresetController::Host editorCallbacks;
        editorCallbacks.showPage = [&host](QDialog *dialog, const QString &, int,
                                           std::function<void(int)> onClosed) {
            dialog->setParent(&host);
            dialog->setWindowFlags(Qt::Widget);
            dialog->show();
            QObject::connect(
                dialog, &QDialog::finished, dialog,
                [onClosed](int result) { onClosed(result); },
                Qt::SingleShotConnection);
            return true;
        };
        PresetController editorController(nullptr,&host,editorCallbacks);
        int choice=3;
        editorController.setEditorPrompt([&](const QString &,const QString &,const QStringList &){return choice;});
        const auto visibleEditor=[&host]{
            for(auto *page:host.findChildren<QWidget *>(QStringLiteral("presetEditorPage")))
                if(!page->isHidden())return static_cast<PresetEditorPage *>(page);
            return static_cast<PresetEditorPage *>(nullptr);
        };
        QString draftId;
        editorController.openEditor();
        // 开页这一句**不该**卡住：摆上去就返回，收页之后才走回调。∴ 这里直接看页面，
        // 不用再排一发定时器进嵌套循环（以前那一节靠 `execPage()` 卡着才写得出来）。
        auto *editor=visibleEditor();
        auto *name=host.findChild<QLineEdit *>("presetEditorName");
        check(editor&&name,"unified preset editor opens without creating a published preset");
        check(editor!=nullptr,"openEditor returns with the editor page still on screen");
        auto *embeddedManager=host.findChild<PresetManagePage *>();
        check(embeddedManager&&!embeddedManager->isWindow()&&embeddedManager->parentWidget()==&host,
              "preset catalog is embedded in the management host rather than a dialog window");
        check(editor&&!editor->isWindow()&&editor->parentWidget()==&host,
              "preset editor covers the host content area instead of opening a second window");
        check(embeddedManager&&embeddedManager->minimumSizeHint().height()==0,
              "embedded catalog does not force the management mode controls to collapse in a short window");
        if(editor&&name){
            name->setText("Editor leave test");
            check(!editorController.leaveEditor()&&name->text()=="Editor leave test",
                  "cancel departure retains the complete unsaved editor");
            choice=1;
            check(!editorController.leaveEditor()&&editor->isDirty(),
                  "failed publication keeps the editor and its unsaved changes");
            choice=0;
            draftId=editor->document().value("id").toString();
            check(editorController.leaveEditor(),"save-as-draft departure waits for the save and then leaves");
            check(editor->isHidden(),"saving as a draft takes the editor page down");
        }
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        check(embeddedManager&&!embeddedManager->isHidden(),"leaving the editor reveals the embedded preset catalog");
        // 左栏不再是一排筛选 chip，而是一棵**按类别分组**的树（和实例页共用同一个代理）。
        auto *presetList=embeddedManager?embeddedManager->findChild<QTreeWidget *>(PresetManagePage::presetListName()):nullptr;
        auto *groupDelegate=presetList?qobject_cast<InstanceGroupDelegate *>(presetList->itemDelegate()):nullptr;
        check(presetList&&groupDelegate&&!groupDelegate->inlineRowActions()&&groupDelegate->rowHeight()>=24,
              "embedded preset list keeps the grouped delegate and a readable row height after stylesheet polish");
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        if(embeddedManager)QApplication::sendEvent(embeddedManager,&escape);
        check(embeddedManager&&!embeddedManager->isHidden(),"Escape does not dismiss the embedded preset catalog into a blank host");
        bool saved=false;Presets::loadManifest(PresetController::presetsRoot()+"/drafts",draftId,&saved);
        check(saved,"departure draft is persisted before the editor closes");
        editorController.openEditor(draftId,true);
        auto *reopened=visibleEditor();
        const QString reopenedName=reopened?reopened->document().value("name").toString():QString();
        check(reopenedName=="Editor leave test","saved draft can be reopened as one editable document");
        choice=2;  // 不保存：这一趟只是把它打开看一眼
        check(editorController.leaveEditor(),"leaving a clean reopened draft closes it directly");
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        auto legacy=Presets::create(PresetController::presetsRoot(),"Legacy repeated sections");
        Presets::PresetSection first;first.id=Presets::newPresetId();first.kind=Presets::SectionKind::VanillaOptions;first.items=QJsonArray{QJsonObject{{"key","guiScale"},{"value","2"}}};
        auto second=first;second.id=Presets::newPresetId();second.mode=Presets::ApplyMode::Replace;
        legacy.sections={first,second};check(Presets::saveManifest(PresetController::presetsRoot(),legacy),"repeated-section editor fixture is saved");
        editorController.openEditor(legacy.id);
        auto *status=embeddedManager?embeddedManager->findChild<QLabel *>(PresetManagePage::statusLabelName()):nullptr;
        if(status&&!status->text().contains(QString::fromUtf8("多段")))std::printf("Legacy editor status: %s\n",status->text().toUtf8().constData());
        check(status&&status->text().contains(QString::fromUtf8("多段")),"legacy repeated sections are refused rather than silently changing overwrite order");
        qApp->setProperty("wtfml.testDefaultMinecraftDir",previousDefault);
    }
    return failures;
}

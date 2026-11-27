#include "Support.hpp"
#include <QScrollBar>
#include <QThread>
int UiLifecycleCheck(QApplication &app) {

        if (app.arguments().contains(QStringLiteral("--lifecycle-preview"))) {
            const int fontId = QFontDatabase::addApplicationFont(
                QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc")));
            const auto families = QFontDatabase::applicationFontFamilies(fontId);
            if (!families.isEmpty()) app.setFont(QFont(families.first(), 9));
            app.setStyle(Theme::createStyle());
            Theme::setSpec(Theme::defaultSpec());
            Theme::apply(app);
        }
        QTemporaryDir scratch(QDir::current().filePath(QStringLiteral(".lifecycle-check-XXXXXX")));
        if (!scratch.isValid()) return 1;
        int failures = 0;
        const auto expect = [&failures](bool ok, const char *message) {
            printf("%s %s\n", ok ? "PASS" : "FAIL", message);
            if (!ok) ++failures;
        };
        {
            QWidget anchor;
            QWidget home(&anchor);
            OverlayHost host(&anchor, &home);
            auto *prompt = new QDialog;
            QTimer::singleShot(0, prompt, &QObject::deleteLater);
            expect(host.execPrompt(prompt, false) == QDialog::Rejected && !host.isOpen(),
                   "destroyed prompt exits embedded loop and closes overlay");
        }
        using Dlg = InstanceSettingsDialog;
        Dlg dialog(QStringLiteral("Test"), QStringLiteral("test"), Dlg::Override(),
                   Dlg::GlobalValues(), {}, true);
        auto *lastLaunch = dialog.findChild<QLabel *>(QStringLiteral("instanceLastLaunch"));
        expect(lastLaunch && lastLaunch->text() == QStringLiteral("从未启动"),
               "overview does not invent a last launch for a new instance");
        const auto launched = QDateTime::fromString(QStringLiteral("2026-10-20T10:20:00"), Qt::ISODate);
        dialog.setLastLaunch(launched);
        expect(lastLaunch && lastLaunch->text().contains(QStringLiteral("2026-10-20 10:20")),
               "overview displays the supplied real launch timestamp");
        dialog.setLastLaunch(QDateTime());
        expect(lastLaunch && lastLaunch->text() == QStringLiteral("从未启动"),
               "overview clears a previous timestamp when the record is absent");
        QHash<QString, QString> dirs;
        const QStringList keys{QStringLiteral("screenshots"), QStringLiteral("replay"),
                               QStringLiteral("logs"), QStringLiteral("crash"), QStringLiteral("structure")};
        for (const QString &key : keys) {
            const QString root = scratch.path() + QLatin1Char('/') + key;
            dirs.insert(key == QStringLiteral("structure") ? QStringLiteral("save") : key, root);
            const QString folder = key == QStringLiteral("structure")
                ? root + QStringLiteral("/World/generated/test/structures") : root;
            expect(QDir().mkpath(folder), "create lifecycle fixture directory");
            const QString suffix = key == QStringLiteral("screenshots") ? QStringLiteral(".png")
                : key == QStringLiteral("replay") ? QStringLiteral(".mcpr")
                : key == QStringLiteral("structure") ? QStringLiteral(".nbt") : QStringLiteral(".log");
            for (const QString &name : {QStringLiteral("Alpha-2026"), QStringLiteral("Beta-2025")}) {
                const QString path = folder + QLatin1Char('/') + name + suffix;
                if (key == QStringLiteral("screenshots")) {
                    QImage image(16, 16, QImage::Format_RGB32);
                    image.fill(Qt::green);
                    expect(image.save(path), "write screenshot fixture");
                } else {
                    QFile file(path);
                    expect(file.open(QIODevice::WriteOnly) && file.write("test") == 4,
                           "write lifecycle fixture");
                }
            }
        }
        const QString schematicsDir = scratch.filePath(QStringLiteral("schematics"));
        dirs.insert(QStringLiteral("schematic"), schematicsDir);
        expect(QDir().mkpath(schematicsDir + QStringLiteral("/not-a-file.schem")),
               "create schematic batch fixtures");
        for (const QString &name : {QStringLiteral("first.litematic"), QStringLiteral("second.schem"),
                                    QStringLiteral("notes.txt")}) {
            QFile file(QDir(schematicsDir).filePath(name));
            expect(file.open(QIODevice::WriteOnly) && file.write("test") == 4,
                   "write schematic batch fixture");
        }
        dialog.setFileDirs(dirs);
        dialog.resize(1100, 720);
        dialog.show();
        dialog.showPage(Dlg::overviewCategoryKey());
        app.processEvents();
        // 「最后启动：…」那一行要**整句**摆得下：它按 sizeHint 宽摆在行里（overviewRow 的
        // AlignLeft），折行的标签会把日期折到行高看不见的第二行 —— 屏幕上只剩「最后启动：」。
        dialog.setLastLaunch(launched);
        app.processEvents();
        if (lastLaunch) {
            const int needed = lastLaunch->fontMetrics().horizontalAdvance(lastLaunch->text());
            printf("last launch geometry: width=%d needed=%d height=%d\n", lastLaunch->width(), needed,
                   lastLaunch->height());
            expect(!lastLaunch->wordWrap() && lastLaunch->width() >= needed &&
                       lastLaunch->height() >= lastLaunch->fontMetrics().height(),
                   "overview shows the whole last-launch line on one line");
        }
        dialog.setLastLaunch(QDateTime());
        const QList<QPushButton *> overviewCommands = {
            dialog.findChild<QPushButton *>(Dlg::changeIconButtonName()),
            dialog.findChild<QPushButton *>(Dlg::groupButtonName())};
        auto *firstShortcut = dialog.findChild<QPushButton *>(Dlg::shortcutButtonName(0));
        // 概览是「左标题 | 右控件」一行一行（e5a3b79）：按钮按自己的字宽，各行最右那颗收在同一条右边线上；
        // 快捷方式那排是摆在标题下面的整行，不参与对齐。
        for (auto *button : overviewCommands) {
            if (button) printf("overview geometry %s: x=%d width=%d parentWidth=%d\n", qPrintable(button->objectName()),
                               button->mapTo(&dialog, QPoint()).x(), button->width(), button->parentWidget()->width());
        }
        const auto rightEdge = [&dialog](QPushButton *b) { return b ? b->mapTo(&dialog, QPoint(b->width(), 0)).x() : -1; };
        const int edge = rightEdge(overviewCommands.at(0));
        bool sameEdge = edge > 0;
        for (int i : {1}) sameEdge &= qAbs(rightEdge(overviewCommands.at(i)) - edge) <= 1;
        expect(sameEdge && firstShortcut != nullptr,
               "overview row commands end on one right edge across independent rows");

        auto *navigation = dialog.findChild<QTreeWidget *>(Dlg::navigationTreeName());
        bool backupLeaf = false;
        int collapsedGroups = 0;
        for (QTreeWidgetItemIterator it(navigation); navigation && *it; ++it) {
            backupLeaf |= (*it)->data(0, Qt::UserRole).toString() == QStringLiteral("worldbackup");
            const QString group = (*it)->data(0, Qt::UserRole + 1).toString();
            if ((group == QStringLiteral("creative") || group == QStringLiteral("maintenance")) && !(*it)->isExpanded())
                ++collapsedGroups;
        }
        expect(navigation && !backupLeaf, "world backup is not a standalone navigation item");
        expect(collapsedGroups == 2, "creative and maintenance groups start collapsed");
        // 组头那一行：点**行身**（不是最左边那颗小箭头）也要展开 / 收起 ——
        // 截图点名要改的就是这一处（原来整行只有那颗箭头管用）。
        {
            QTreeWidgetItem *contentGroup = nullptr;
            for (QTreeWidgetItemIterator it(navigation); navigation && *it; ++it) {
                if ((*it)->data(0, Qt::UserRole + 1).toString() == QStringLiteral("content")) {
                    contentGroup = *it;
                    break;
                }
            }
            bool toggledOff = false;
            bool toggledBack = false;
            if (navigation != nullptr && contentGroup != nullptr && navigation->viewport() != nullptr) {
                auto *viewport = navigation->viewport();
                const auto clickRowBody = [navigation, viewport, contentGroup] {
                    // 落点取**行身**（离左边那颗箭头半个行宽远）：这不是"点在箭头上"的
                    // 那一发，正是要验的那件事。
                    const QRect row = navigation->visualItemRect(contentGroup);
                    const QPoint pos(row.left() + row.height(), row.center().y());
                    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos),
                                      QPointF(viewport->mapToGlobal(pos)), Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(viewport, &press);
                    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos),
                                        QPointF(viewport->mapToGlobal(pos)), Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(viewport, &release);
                };
                const bool wasExpanded = contentGroup->isExpanded();
                clickRowBody();
                toggledOff = contentGroup->isExpanded() != wasExpanded;
                clickRowBody();  // 再点一下翻回去，别把后面的断言建立在被改过的状态上
                toggledBack = contentGroup->isExpanded() == wasExpanded;
            }
            expect(toggledOff, "clicking a group row body collapses it without aiming at the arrow");
            expect(toggledBack, "clicking the same group row body again expands it back");
        }
        QStringList maintenanceKeys;
        for (QTreeWidgetItemIterator it(navigation); navigation && *it; ++it) {
            auto *parent = (*it)->parent();
            if (parent && parent->data(0, Qt::UserRole + 1).toString() == QStringLiteral("maintenance"))
                maintenanceKeys.append((*it)->data(0, Qt::UserRole).toString());
        }
        expect(maintenanceKeys == QStringList{QStringLiteral("diagnosis"), QStringLiteral("logs"),
                   QStringLiteral("operationsmenu"), QStringLiteral("migration"), QStringLiteral("upgrade")},
               "maintenance has five functional entries without redundant pages");
        dialog.showPage(QStringLiteral("diagnosticsmenu"));
        expect(dialog.currentPageKey() == QStringLiteral("diagnosis"), "legacy diagnostics route opens actual report");
        expect(navigation->currentItem() && navigation->currentItem()->data(0, Qt::UserRole).toString() == QStringLiteral("diagnosis"),
               "direct diagnostics page keeps the correct navigation selection");
        dialog.showPage(QStringLiteral("export"));
        expect(dialog.currentPageKey() == QStringLiteral("operationsmenu"), "legacy export route opens instance actions");
        int repairs = 0, inspections = 0;
        QObject::connect(&dialog, &Dlg::repairFilesRequested, [&repairs] { ++repairs; });
        QObject::connect(&dialog, &Dlg::integrityCheckRequested, [&inspections] { ++inspections; });
        auto *repair = dialog.findChild<QPushButton *>(Dlg::repairFilesButtonName());
        dialog.showPage(Dlg::launchCategoryKey());
        expect(repair && !repair->isVisibleTo(&dialog), "launch settings no longer duplicate file repair");
        dialog.showPage(QStringLiteral("repair"));
        expect(repair && repair->isVisibleTo(&dialog), "maintenance repair owns the file completion action");
        auto *inspection = dialog.findChild<QPushButton *>(Dlg::integrityCheckButtonName());
        expect(dialog.currentPageKey() == QStringLiteral("diagnosis") && inspection && inspection->isVisibleTo(&dialog),
               "repair and integrity actions share one page without another entry button");
        if (repair) repair->click();
        expect(repairs == 1 && inspections == 0, "repair page repairs instead of duplicating integrity scan");
        const QList<QPair<QString, QString>> movedActions = {
            {Dlg::exportButtonName(), QStringLiteral("export")},
            {Dlg::copyButtonName(), QStringLiteral("operationsmenu")},
            {Dlg::deleteButtonName(), QStringLiteral("operationsmenu")},
            {Dlg::integrityCheckButtonName(), QStringLiteral("integrity")},
            {Dlg::createSnapshotButtonName(), QStringLiteral("snapshots")},
            {Dlg::restoreSnapshotButtonName(), QStringLiteral("snapshots")}};
        for (const auto &entry : movedActions) {
            auto *control = dialog.findChild<QPushButton *>(entry.first);
            dialog.showPage(Dlg::overviewCategoryKey());
            expect(control && !control->isVisibleTo(&dialog), "maintenance action no longer appears on overview");
            dialog.showPage(entry.second);
            expect(control && control->isVisibleTo(&dialog), "relocated action appears on dedicated page");
            expect(dialog.findChildren<QPushButton *>(entry.first).size() == 1, "relocated action is not duplicated");
        }
        dialog.showPage(Dlg::overviewCategoryKey());
        expect(!dialog.findChild<QPushButton *>(Dlg::checkUpdatesButtonName()),
               "overview has no manual update check entry");
        {
            // #144 概览页底部的「删除…」和维护页那颗发同一个信号。
            int deleteRequests = 0;
            const auto counting = QObject::connect(&dialog, &Dlg::deleteInstanceRequested,
                                                   [&deleteRequests] { ++deleteRequests; });
            auto *overviewRemove = dialog.findChild<QPushButton *>(Dlg::overviewDeleteButtonName());
            expect(overviewRemove && overviewRemove->isVisibleTo(&dialog)
                       && overviewRemove->text() == QStringLiteral("删除…")
                       && overviewRemove->property("wtfmlButtonKind").toString()
                              == Theme::buttonKindKey(Theme::ButtonKind::Danger),
                   "overview offers a danger delete action");
            if (overviewRemove) overviewRemove->click();
            expect(deleteRequests == 1, "overview delete emits deleteInstanceRequested once");
            dialog.showPage(QStringLiteral("operationsmenu"));
            auto *remove = dialog.findChild<QPushButton *>(Dlg::deleteButtonName());
            expect(remove && remove->isVisibleTo(&dialog), "maintenance page keeps its delete action");
            if (remove) remove->click();
            expect(deleteRequests == 2, "maintenance delete emits the same signal");
            QObject::disconnect(counting);
        }
        dialog.showPage(QStringLiteral("snapshots"));
        expect(dialog.currentPageKey() == QStringLiteral("operationsmenu"), "snapshot route opens consolidated backup and operations page");
        auto *snapshot = dialog.findChild<QPushButton *>(Dlg::createSnapshotButtonName());
        auto *exportCommand = dialog.findChild<QPushButton *>(Dlg::exportButtonName());
        expect(snapshot && exportCommand && snapshot->width() == exportCommand->width() && snapshot->width() < 320,
               "backup and instance commands have consistent compact widths");
        app.processEvents();
        const auto commandsDoNotOverlap = [&dialog](const QStringList &names) {
            QRect previous;
            QWidget *row = nullptr;
            for (const QString &name : names) {
                auto *button = dialog.findChild<QPushButton *>(name);
                if (button) printf("maintenance geometry %s visible=%d parent=%s x=%d y=%d w=%d h=%d\n",
                    qPrintable(name), button->isVisibleTo(&dialog), qPrintable(button->parentWidget()->objectName()),
                    button->mapTo(&dialog,QPoint()).x(), button->mapTo(&dialog,QPoint()).y(), button->width(), button->height());
                if (!button || !button->isVisibleTo(&dialog)) return false;
                if (row && row != button->parentWidget()) return false;
                row = button->parentWidget();
                const QRect box(button->mapTo(&dialog, QPoint()), button->size());
                if (!previous.isNull() && (box.left() < previous.right() + 8 || box.top() != previous.top())) return false;
                previous = box;
            }
            return row && row->layout() && row->width() >= previous.width();
        };
        expect(commandsDoNotOverlap({Dlg::createSnapshotButtonName(), Dlg::restoreSnapshotButtonName(),
                       QStringLiteral("inspectRecoveryButton")}),
               "all three backup commands occupy separate horizontal click targets in one managed row");
        expect(commandsDoNotOverlap({Dlg::copyButtonName(), Dlg::exportButtonName()}),
               "copy and export commands do not overlap after reparenting");
        dialog.showPage(QStringLiteral("save"));
        dialog.showPage(QStringLiteral("resourcepack"));
        auto *packMode = dialog.findChild<QPushButton *>(QStringLiteral("resourcePackFileMode"));
        auto *packReturn = dialog.findChild<QPushButton *>(QStringLiteral("resourcePackMode"));
        expect(packMode && packReturn && packMode->isVisibleTo(&dialog) && !packReturn->isVisibleTo(&dialog),
               "resource pack file mode puts its switch inside the compact toolbar");
        if (packMode && packReturn) {
            packMode->click();
            expect(packReturn->isVisibleTo(&dialog), "resource pack order mode retains a visible way back");
            packReturn->click();
            expect(packMode->isVisibleTo(&dialog), "resource pack file toolbar returns after changing mode");
        }
        dialog.showPage(QStringLiteral("server"));
        auto *serverColumns = dialog.findChild<QTreeWidget *>(Dlg::serverTreeName());
        expect(serverColumns && serverColumns->columnWidth(1) == 240 && serverColumns->columnWidth(2) == 144,
               "server address and status have bounded default widths instead of following long error text");
        dialog.setUpgradeContext(QStringLiteral("当前实例：测试实例 · Minecraft 1.20.1 · Fabric"));
        dialog.setTransferTargets(QStringLiteral("upgrade"), {{QStringLiteral("1.21 尚未准备"), QStringLiteral("@download:1.21")},
                                                              {QStringLiteral("1.21 Fabric 核心已准备"), QStringLiteral("ready-core")}});
        dialog.showPage(QStringLiteral("upgrade"));
        auto *upgradeTarget = dialog.findChild<QComboBox *>(Dlg::transferTargetComboName(QStringLiteral("upgrade")));
        auto *upgradeApply = dialog.findChild<QPushButton *>(Dlg::transferApplyButtonName(QStringLiteral("upgrade")));
        auto *inPlace = dialog.findChild<QPushButton *>(QStringLiteral("instanceUpgradeInPlaceButton"));
        auto *prepare = dialog.findChild<QPushButton *>(QStringLiteral("instanceUpgradePrepareTarget"));
        expect(upgradeTarget && dialog.transferTarget(QStringLiteral("upgrade")).isEmpty() && !upgradeApply->isEnabled() && !inPlace->isEnabled(),
               "upgrade never silently chooses a different instance as a target");
        QString preparedVersion;
        QObject::connect(&dialog, &Dlg::prepareUpgradeTargetRequested, &dialog, [&](const QString &version) { preparedVersion = version; });
        upgradeTarget->setCurrentIndex(1);
        expect(prepare->isVisibleTo(&dialog) && !inPlace->isEnabled(), "unprepared core cannot trigger an in-place update");
        prepare->click();
        expect(preparedVersion == QStringLiteral("1.21"), "preparing an update carries the exact selected game version");
        upgradeTarget->setCurrentIndex(2);
        dialog.setTransferItems(QStringLiteral("upgrade"), {{QStringLiteral("存档"), QStringLiteral("saves")}}, {QStringLiteral("saves")});
        expect(upgradeApply->isEnabled() && inPlace->isEnabled() && !prepare->isVisibleTo(&dialog), "prepared target enables the existing protected update actions");
        upgradeTarget->setEditText(QStringLiteral("1.99 输入但未选中"));
        expect(!inPlace->isEnabled() && !upgradeApply->isEnabled(), "typing a search cannot execute the previously selected target");
        dialog.setUpgradeModChecks({{QStringLiteral("示例模组"), QStringLiteral("平台未找到目标发行版\n本地 Minecraft 声明符合"), QStringLiteral(">=1.20 <1.22")}}, QStringLiteral("声明不是运行保证"));
        auto *modChecks = dialog.findChild<QTreeWidget *>(QStringLiteral("instanceUpgradeModChecks"));
        expect(modChecks && modChecks->topLevelItemCount() == 1 && modChecks->topLevelItem(0)->text(1).contains(QStringLiteral("声明符合")),
               "platform release absence and local declaration support can be displayed together");
        if (app.arguments().contains(QStringLiteral("--lifecycle-preview"))) {
            upgradeTarget->setCurrentIndex(2);
            dialog.setTransferPlan(QStringLiteral("upgrade"), QStringLiteral("示例评估（测试数据）\nMinecraft 1.20.1 → 1.21\n游戏版本发生变化；请检查存档、模组与配置。\n原地更新需要完整快照；创建副本不会修改原实例。"));
            dialog.resize(1100, 650); app.processEvents();
            dialog.grab().save(QStringLiteral("build/upgrade-layout-preview.png"));
        }
        {
            InstanceFilesPage details;
            details.setDirectory(dirs.value(QStringLiteral("logs")));
            details.setInfoPanelEnabled(true);
            int requested = 0;
            QObject::connect(&details, &InstanceFilesPage::infoDetailsRequested, &details,
                [&](const QString &path) { if (path == details.currentPath()) ++requested; });
            details.list()->setCurrentRow(0);
            auto *button = details.findChild<QPushButton *>(QStringLiteral("instanceSchematicDetails"));
            expect(button && button->isEnabled(), "schematic details is available for a selected file");
            if (button) button->click();
            expect(requested == 1, "schematic details request carries the selected file path");
            details.list()->clearSelection();
            expect(button && !button->isEnabled(), "schematic details cannot act without a selection");
        }
        dialog.showPage(QStringLiteral("save"));
        auto *backups = dialog.findChild<QPushButton *>(QStringLiteral("instanceSaveBackups"));
        expect(backups != nullptr, "saves page provides world backup entry");
        auto *saves = dialog.findChild<InstanceFilesPage *>(Dlg::filesPageName(QStringLiteral("save")));
        if (saves) {
            saves->setJoinText(QStringLiteral("加入选中的世界"), QStringLiteral("启动并进入选中存档"));
            auto *join = saves->findChild<QPushButton *>(InstanceFilesPage::joinButtonName());
            auto *state = saves->findChild<QComboBox *>(InstanceFilesPage::stateComboName());
            auto *batchEdit = saves->findChild<QPushButton *>(InstanceFilesPage::batchButtonName());
            auto *search = saves->findChild<QLineEdit *>(InstanceFilesPage::searchEditName());
            auto *path = saves->findChild<QWidget *>(InstanceFilesPage::pathLabelName());
            expect(state && !state->isVisibleTo(&dialog), "worlds never expose enabled/disabled filters");
            saves->setStateFilter(InstanceFilesPage::StateFilter::Disabled);
            expect(saves->stateFilter() == InstanceFilesPage::StateFilter::All,
                   "worlds cannot retain a hidden disabled-state filter");
            expect(backups && join && backups->parentWidget() == join->parentWidget()
                   && join->text() == QStringLiteral("加入世界"), "world actions share one compact toolbar");
            expect(search && path && search->parentWidget() == path->parentWidget(),
                   "world path shares the search row instead of squeezing toolbar actions");
            expect(batchEdit && !batchEdit->isVisibleTo(&dialog), "unimplemented batch world edit does not occupy toolbar");
            expect(!dialog.findChild<QLabel *>(Dlg::filesHintName(QStringLiteral("save"))),
                   "obsolete world-management footer no longer reduces list height");
            if (app.arguments().contains(QStringLiteral("--lifecycle-preview"))) {
                dialog.resize(1100, 650);
                app.processEvents();
                dialog.grab().save(QStringLiteral("build/world-layout-preview.png"));
            }
        }
        if (backups) backups->click();
        expect(dialog.currentPageKey() == QStringLiteral("worldbackup") && navigation &&
                   navigation->currentItem() && navigation->currentItem()->data(0, Qt::UserRole).toString() == QStringLiteral("save"),
               "nested backups keep saves navigation selected");
        auto *worlds = dialog.findChild<QListWidget *>(QStringLiteral("instanceWorldBackupList"));
        expect(worlds && worlds->count() == 1 && worlds->item(0)->data(Qt::UserRole).toString() ==
                   dirs.value(QStringLiteral("save")) + QStringLiteral("/World"),
               "nested backup page still loads worlds from saves directory");
        auto *back = dialog.findChild<QPushButton *>(QStringLiteral("instanceWorldBackupBack"));
        if (back) back->click();
        expect(back && dialog.currentPageKey() == QStringLiteral("save"), "backup page returns to saves");
        dialog.showPage(QStringLiteral("screenshots"));
        expect(navigation && navigation->currentItem() && navigation->currentItem()->parent()->isExpanded(),
               "entering creative page reveals its navigation group");
        for (const QString &key : keys) {
            dialog.showPage(key);
            auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("instanceLifecycleSearch_%1").arg(key));
            auto *list = dialog.findChild<QListWidget *>(QStringLiteral("instanceLifecycleFiles_%1").arg(key));
            auto *result = dialog.findChild<QLabel *>(QStringLiteral("instanceLifecycleResult_%1").arg(key));
            expect(search && list && result, "lifecycle search controls exist");
            if (!search || !list || !result) continue;
            expect(list->count() == 2, "lifecycle files loaded");
            search->setText(QStringLiteral("ALPHA 2026"));
            const auto visibleCount = [list] {
                int count = 0;
                for (int row = 0; row < list->count(); ++row)
                    if (!list->item(row)->isHidden()) ++count;
                return count;
            };
            expect(visibleCount() == 1, "file search is case insensitive and matches all terms");
            dialog.showPage(QStringLiteral("overview"));
            dialog.showPage(key);
            expect(search->text() == QStringLiteral("ALPHA 2026") && visibleCount() == 1,
                   "refresh preserves search and reapplies filter");
            search->setText(QStringLiteral("nonexistent"));
            expect(visibleCount() == 0 && result->text().contains(QStringLiteral("未找到")),
                   "no matches has an explicit result message");
            search->clear();
            expect(visibleCount() == 2, "clearing search restores files");
        }
        {
            using Action = Dlg::LifecycleBatchAction;
            expect(Dlg::pageText(QStringLiteral("replay")) == QStringLiteral("回放"),
                   "replay page uses its Chinese name");
            QString requestedPage;
            QStringList requestedPaths;
            Action requestedAction = Action::Remove;
            int requests = 0;
            qsizetype selectedAtRequest = -1;
            bool blockedAtRequest = false;
            bool reclickBlocked = false;
            QListWidget *list = nullptr;
            // 旧断言钉的是"发请求前先把选择清掉"。那层保护本来是防"提示框的嵌套事件循环里
            // 再点一次「删除」，同一批发两遍"，代价却是用户点「取消」时选择也一起没了
            // （issue #26）。现在改成请求在飞时整排工具条灰着，选择留着，所以这一组
            // 断言量的是：发出去那一刻选择还在、按钮按不动、再点也发不出第二发。
            const auto buttonFor = [&dialog](const QString &name, const QString &pageKey) {
                return dialog.findChild<QPushButton *>(
                    QStringLiteral("instanceLifecycleBatch%1_%2").arg(name, pageKey));
            };
            const auto connection = QObject::connect(&dialog, &Dlg::lifecycleBatchRequested, &dialog,
                [&](const QString &key, const QStringList &paths, Action action) {
                    requestedPage = key;
                    requestedPaths = paths;
                    requestedAction = action;
                    selectedAtRequest = list ? list->selectedItems().size() : -1;
                    blockedAtRequest = true;
                    for (const QString &name : {QStringLiteral("All"), QStringLiteral("Invert"),
                                                QStringLiteral("Clear"), QStringLiteral("Copy"),
                                                QStringLiteral("Move"), QStringLiteral("Remove")}) {
                        auto *control = buttonFor(name, key);
                        blockedAtRequest = blockedAtRequest && control != nullptr && !control->isEnabled();
                    }
                    // 在飞的时候再点一次「删除」：灰着，发不出去第二发。
                    const int prior = requests;
                    if (auto *again = buttonFor(QStringLiteral("Remove"), key)) again->click();
                    reclickBlocked = requests == prior;
                    ++requests;
                });
            for (const QString &key : {QStringLiteral("screenshots"), QStringLiteral("schematic"),
                                       QStringLiteral("replay"), QStringLiteral("structure"),
                                       QStringLiteral("logs"), QStringLiteral("crash")}) {
                dialog.showPage(key);
                const auto button = [&](const QString &name) { return buttonFor(name, key); };
                if (key == QStringLiteral("schematic")) {
                    auto *page = dialog.filesPage(key);
                    list = page ? page->list() : nullptr;
                    expect(page && !page->batchMode() && !page->findChild<QWidget *>(QStringLiteral("modActionStack")),
                           "schematics do not use the mod action row");
                } else {
                    list = dialog.findChild<QListWidget *>(QStringLiteral("instanceLifecycleFiles_%1").arg(key));
                }
                auto *all = button(QStringLiteral("All"));
                auto *invert = button(QStringLiteral("Invert"));
                auto *clear = button(QStringLiteral("Clear"));
                auto *copy = button(QStringLiteral("Copy"));
                auto *move = button(QStringLiteral("Move"));
                auto *remove = button(QStringLiteral("Remove"));
                expect(list && all && invert && clear && copy && move && remove,
                       "all batch actions exist on the supported page");
                if (!list || !all || !invert || !clear || !copy || !move || !remove) continue;
                app.processEvents();
                expect(list->selectionMode() == QAbstractItemView::ExtendedSelection && !remove->isEnabled(),
                       "extended selection starts empty and cannot delete");
                all->click();
                expect(list->selectedItems().size() == 2, "select all excludes folders and unsupported files");
                invert->click();
                expect(list->selectedItems().isEmpty(), "invert from all deselects every file");
                auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("instanceLifecycleSearch_%1").arg(key));
                if (search) {
                    search->setText(QStringLiteral("Alpha"));
                    app.processEvents();
                    invert->click();
                    expect(list->selectedItems().size() == 1 && !list->selectedItems().first()->isHidden(),
                           "invert and batch actions are scoped to the visible search result");
                    copy->click();
                    expect(blockedAtRequest && reclickBlocked && selectedAtRequest == 1
                               && requestedPage == key && requestedAction == Action::Copy
                               && requestedPaths.size() == 1 && requestedPaths.first().contains(QStringLiteral("Alpha")),
                           "copy sends only visible paths and blocks the toolbar while the request is in flight");
                    expect(list->selectedItems().size() == 1 && copy->isEnabled(),
                           "a request that never refreshes keeps the selection (the cancel path)");
                    search->clear();
                    app.processEvents();
                }
                all->click();
                clear->click();
                expect(list->selectedItems().isEmpty(), "clear deselects every file");
                all->click();
                const int before = requests;
                move->click();
                expect(requests == before + 1 && blockedAtRequest && reclickBlocked
                           && selectedAtRequest == 2 && requestedPage == key
                           && requestedAction == Action::Move && requestedPaths.size() == 2,
                       "move sends the batch and blocks the toolbar while the request is in flight");
                expect(list->selectedItems().size() == 2,
                       "move keeps the selection when the refresh never happens");
                all->click();
                remove->click();
                expect(requests == before + 2 && blockedAtRequest && reclickBlocked
                           && selectedAtRequest == 2 && requestedAction == Action::Remove
                           && requestedPaths.size() == 2,
                       "delete blocks the toolbar and keeps the selection until the request returns");
                expect(list->selectedItems().size() == 2 && remove->isEnabled(),
                       "delete leaves the selection alone when nothing was deleted");
                list->clearSelection();
            }
            // 存档页：批量对象是世界文件夹，只有复制和删除（没有移动）；选择在重列后按路径保持。
            {
                const QString worldsDir = scratch.filePath(QStringLiteral("worlds"));
                for (const QString &name : {QStringLiteral("WorldA"), QStringLiteral("WorldB")})
                    expect(QDir().mkpath(worldsDir + QLatin1Char('/') + name), "create world batch fixtures");
                auto worldDirs = dirs;
                worldDirs.insert(QStringLiteral("save"), worldsDir);
                dialog.setFileDirs(worldDirs);
                dialog.showPage(QStringLiteral("save"));
                app.processEvents();
                auto *page = dialog.filesPage(QStringLiteral("save"));
                list = page ? page->list() : nullptr;
                const auto world = [&](const QString &name) {
                    return buttonFor(name, QStringLiteral("save"));
                };
                auto *worldAll = world(QStringLiteral("All"));
                auto *worldCopy = world(QStringLiteral("Copy"));
                auto *worldMove = world(QStringLiteral("Move"));
                auto *worldRemove = world(QStringLiteral("Remove"));
                expect(list && worldAll && worldCopy && worldMove && worldRemove
                           && !worldMove->isVisibleTo(&dialog) && worldCopy->isVisibleTo(&dialog)
                           && worldRemove->isVisibleTo(&dialog),
                       "saves page has batch copy and delete but no move");
                if (list && worldAll && worldCopy && worldRemove) {
                    expect(list->selectionMode() == QAbstractItemView::ExtendedSelection && !worldRemove->isEnabled(),
                           "saves page selects several worlds and cannot delete an empty selection");
                    worldAll->click();
                    expect(list->selectedItems().size() == 2, "saves page select all picks every world folder");
                    const int before = requests;
                    worldCopy->click();
                    expect(requests == before + 1 && requestedPage == QStringLiteral("save")
                               && requestedAction == Action::Copy && requestedPaths.size() == 2
                               && requestedPaths.first().startsWith(worldsDir),
                           "saves page copy sends the selected world folders");
                    worldRemove->click();
                    expect(requests == before + 2 && requestedAction == Action::Remove && requestedPaths.size() == 2,
                           "saves page delete sends the selected world folders");
                    // 失败后保持选中：重列后按路径把选择还回去，只剩没处理掉的那几个。
                    dialog.refreshLifecycleBatchFiles(QStringLiteral("save"));
                    app.processEvents();
                    expect(list->selectedItems().size() == 2, "saves page keeps the selection across a refresh");
                    expect(QDir(worldsDir + QStringLiteral("/WorldA")).removeRecursively(), "remove one world fixture");
                    dialog.refreshLifecycleBatchFiles(QStringLiteral("save"));
                    app.processEvents();
                    expect(list->count() == 1 && list->selectedItems().size() == 1
                               && list->selectedItems().first()->data(InstanceFilesPage::pathRole()).toString()
                                      == worldsDir + QStringLiteral("/WorldB"),
                           "saves page restores only the worlds that are still on disk");
                }
                dialog.setFileDirs(dirs);
                dialog.showPage(QStringLiteral("screenshots"));
                app.processEvents();
            }
            // 样张：WTFML_LIFECYCLE_SHOTS=<目录> 时，结构 / 存档 / 服务器列表三页各拍一张选中多项的样子
            // （最小窗口 900×590 与拉宽 1400 两档）。不设就不拍。
            if (const QString shotDir = qEnvironmentVariable("WTFML_LIFECYCLE_SHOTS"); !shotDir.isEmpty()) {
                QDir().mkpath(shotDir);
                const QString worldsDir = scratch.filePath(QStringLiteral("shot-worlds"));
                for (const QString &name : {QStringLiteral("生存档"), QStringLiteral("Creative Test"),
                                            QStringLiteral("红石实验场"), QStringLiteral("Skyblock-2")})
                    QDir().mkpath(worldsDir + QLatin1Char('/') + name);
                for (const QString &name : {QStringLiteral("house"), QStringLiteral("farm-tower"),
                                            QStringLiteral("village-hall"), QStringLiteral("bridge")}) {
                    const QString folder = worldsDir + QStringLiteral("/生存档/generated/minecraft/structures");
                    QDir().mkpath(folder);
                    QFile file(folder + QLatin1Char('/') + name + QStringLiteral(".nbt"));
                    if (file.open(QIODevice::WriteOnly)) file.write("test");
                }
                auto shotDirs = dirs;
                shotDirs.insert(QStringLiteral("save"), worldsDir);
                dialog.setFileDirs(shotDirs);
                dialog.setServerList({qMakePair(QStringLiteral("主服务器"), QStringLiteral("play.example.com")),
                                      qMakePair(QStringLiteral("朋友的服"), QStringLiteral("1.2.3.4:25566")),
                                      qMakePair(QStringLiteral("建筑服"), QStringLiteral("build.example.org")),
                                      qMakePair(QStringLiteral("旧服"), QStringLiteral("old.example.net"))},
                                     QString());
                for (const QSize &size : {QSize(900, 590), QSize(1400, 720)}) {
                    dialog.resize(size);
                    for (const QString &key : {QStringLiteral("structure"), QStringLiteral("save"),
                                               QStringLiteral("server")}) {
                        dialog.showPage(key);
                        if (key == QStringLiteral("structure")) dialog.refreshLifecycleBatchFiles(key);
                        app.processEvents();
                        if (key == QStringLiteral("server")) {
                            if (auto *tree = dialog.serverTree()) {
                                tree->clearSelection();
                                tree->topLevelItem(0)->setSelected(true);
                                tree->topLevelItem(2)->setSelected(true);
                            }
                        } else {
                            QListWidget *shotList = key == QStringLiteral("save")
                                ? (dialog.filesPage(key) ? dialog.filesPage(key)->list() : nullptr)
                                : dialog.findChild<QListWidget *>(QStringLiteral("instanceLifecycleFiles_%1").arg(key));
                            if (auto *all = buttonFor(QStringLiteral("All"), key)) all->click();
                            if (shotList && key == QStringLiteral("save") && shotList->count() > 3)
                                shotList->item(3)->setSelected(false);
                        }
                        app.processEvents();
                        for (int wait = 0; wait < 10; ++wait) {
                            QThread::msleep(20);
                            app.processEvents();
                        }
                        dialog.grab().save(QStringLiteral("%1/%2-%3.png").arg(shotDir, key).arg(size.width()));
                    }
                }
                dialog.setFileDirs(dirs);
                dialog.showPage(QStringLiteral("screenshots"));
                app.processEvents();
            }
            QObject::disconnect(connection);
            expect(Dlg::isLifecycleBatchFile(QStringLiteral("structure"), QStringLiteral("/x/a.NBT"))
                       && !Dlg::isLifecycleBatchFile(QStringLiteral("structure"), QStringLiteral("/x/a.txt")),
                   "structure page batches nbt files only");
            // 日志、崩溃报告不按后缀判断：任何文件都能批量；其余页仍按后缀。
            expect(Dlg::isLifecycleBatchFile(QStringLiteral("logs"), QStringLiteral("/x/logs/2026-11-18-1.log.gz"))
                       && Dlg::isLifecycleBatchFile(QStringLiteral("logs"), QStringLiteral("/x/logs/noext"))
                       && Dlg::isLifecycleBatchFile(QStringLiteral("crash"), QStringLiteral("/x/crash-reports/crash-1.txt"))
                       && !Dlg::isLifecycleBatchFile(QStringLiteral("logs"), QString()),
                   "log and crash pages accept any file for batch actions");
            expect(!Dlg::isLifecycleBatchFile(QStringLiteral("screenshots"), QStringLiteral("/x/a.txt"))
                       && Dlg::isLifecycleBatchFile(QStringLiteral("screenshots"), QStringLiteral("/x/a.PNG"))
                       && !Dlg::isLifecycleBatchFile(QStringLiteral("replay"), QStringLiteral("/x/a.nbt")),
                   "other pages still judge batch files by suffix");
            // 日志页：单击（恰好选中一行）要发信息请求；双击走内置查看器信号而不是外部打开。
            {
                dialog.showPage(QStringLiteral("logs"));
                auto *logList = dialog.findChild<QListWidget *>(QStringLiteral("instanceLifecycleFiles_logs"));
                auto *infoLabel = dialog.findChild<QLabel *>(Dlg::lifecycleInfoLabelName(QStringLiteral("logs")));
                expect(logList && infoLabel && !infoLabel->isVisibleTo(&dialog),
                       "log page has an info bar that starts hidden");
                if (logList && infoLabel) {
                    QString infoKey, infoPath, openedLog, openedExternal;
                    const auto c1 = QObject::connect(&dialog, &Dlg::lifecycleFileInfoRequested, &dialog,
                        [&](const QString &key, const QString &path) { infoKey = key; infoPath = path; });
                    const auto c2 = QObject::connect(&dialog, &Dlg::openLogFileRequested, &dialog,
                        [&](const QString &path) { openedLog = path; });
                    const auto c3 = QObject::connect(&dialog, &Dlg::openInstanceFileRequested, &dialog,
                        [&](const QString &path) { openedExternal = path; });
                    logList->clearSelection();
                    logList->item(0)->setSelected(true);
                    expect(infoLabel->isVisibleTo(&dialog)
                               && infoLabel->text() == Dlg::lifecycleInfoPendingText(),
                           "clicking one log shows the pending text at once");
                    QElapsedTimer waited;
                    waited.start();
                    while (infoKey.isEmpty() && waited.elapsed() < 2000) app.processEvents(QEventLoop::AllEvents, 20);
                    expect(infoKey == QStringLiteral("logs")
                               && infoPath == logList->item(0)->data(Qt::UserRole).toString(),
                           "the info request fires after the debounce with the clicked path");
                    dialog.setLifecycleInfoText(QStringLiteral("logs"), infoPath, QStringLiteral("a.log / 4 B / 1 行"));
                    expect(infoLabel->text() == QStringLiteral("a.log / 4 B / 1 行"),
                           "the answer for the current row replaces the pending text");
                    dialog.setLifecycleInfoText(QStringLiteral("logs"), QStringLiteral("/stale"), QStringLiteral("过期"));
                    expect(infoLabel->text() != QStringLiteral("过期"), "an answer for another row is dropped");
                    logList->item(1)->setSelected(true);  // 多选：信息栏收起
                    expect(!infoLabel->isVisibleTo(&dialog), "selecting several logs hides the info bar");
                    emit logList->itemActivated(logList->item(0));
                    expect(openedLog == logList->item(0)->data(Qt::UserRole).toString() && openedExternal.isEmpty(),
                           "activating a log opens the built-in viewer, not the external program");
                    dialog.showPage(QStringLiteral("screenshots"));
                    auto *shotList = dialog.findChild<QListWidget *>(QStringLiteral("instanceLifecycleFiles_screenshots"));
                    openedLog.clear();
                    if (shotList) emit shotList->itemActivated(shotList->item(0));
                    expect(openedLog.isEmpty() && !openedExternal.isEmpty(),
                           "other pages still open files with the external program");
                    QObject::disconnect(c1);
                    QObject::disconnect(c2);
                    QObject::disconnect(c3);
                    logList->clearSelection();
                }
            }
        }
        {
            // 重列之后滚动位置和选择（issue #26）。夹具本来只有两张截图，先补到能滚起来。
            const QString shotDir = dirs.value(QStringLiteral("screenshots"));
            QStringList extras;
            for (int index = 0; index < 22; ++index) {
                const QString path =
                    shotDir + QStringLiteral("/Extra-%1.png").arg(index, 2, 10, QLatin1Char('0'));
                QImage image(16, 16, QImage::Format_RGB32);
                image.fill(Qt::blue);
                expect(image.save(path), "write extra screenshot fixture");
                extras.append(path);
            }
            // 列表按**修改时间倒序**排（`InstanceSettingsLifecycle.cpp` 里那句 sort）。行号
            // 因此取决于时间戳：同一份夹具在不同机器上排出来不一样。旧写法把行号算在重列
            // 之前、重列之后照着用，换个排法就取到别的行、越界直接解引用空指针。这里把
            // 时间戳钉死成刚才崩掉的那种排法：Alpha 排最后一行，删掉一张之后它已经越界。
            const auto setModified = [](const QString &path, const QDateTime &stamp) {
                QFile file(path);
                if (!file.open(QIODevice::ReadWrite)) return false;
                const bool ok = file.setFileTime(stamp, QFileDevice::FileModificationTime);
                file.close();
                return ok;
            };
            const QDateTime now = QDateTime::currentDateTime();
            for (const QString &path : extras)
                expect(setModified(path, now.addSecs(-3600)), "age the extra screenshot fixture");
            expect(setModified(shotDir + QStringLiteral("/Alpha-2026.png"), now.addSecs(-7200)),
                   "make Alpha the oldest screenshot so it sorts to the last row");
            dialog.showPage(QStringLiteral("screenshots"));
            dialog.refreshLifecycleBatchFiles(QStringLiteral("screenshots"));
            app.processEvents();
            auto *list = dialog.findChild<QListWidget *>(QStringLiteral("instanceLifecycleFiles_screenshots"));
            auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("instanceLifecycleSearch_screenshots"));
            expect(list && search && list->count() == 24, "extra screenshots are listed");
            if (list && search) {
                search->setText(QStringLiteral("Extra"));
                app.processEvents();
                const auto rowOf = [list](const QString &name) {
                    for (int row = 0; row < list->count(); ++row) {
                        auto *item = list->item(row);
                        if (item != nullptr
                            && QFileInfo(item->data(Qt::UserRole).toString()).fileName() == name)
                            return row;
                    }
                    return -1;
                };
                expect(rowOf(QStringLiteral("Alpha-2026.png")) == list->count() - 1,
                       "the fixture puts Alpha on the last row, the order that used to crash");
                const int keepRow = rowOf(QStringLiteral("Extra-03.png"));
                const int goneRow = rowOf(QStringLiteral("Extra-04.png"));
                list->clearSelection();
                if (auto *item = keepRow >= 0 ? list->item(keepRow) : nullptr) item->setSelected(true);
                if (auto *item = goneRow >= 0 ? list->item(goneRow) : nullptr) item->setSelected(true);
                app.processEvents();
                QScrollBar *bar = list->verticalScrollBar();
                // QListWidget 默认按**行**滚（`ScrollPerItem`），所以这个数是行号，不是像素。
                const bool scrollable = bar != nullptr && bar->maximum() >= 4;
                expect(scrollable, "the screenshot list is tall enough to scroll");
                const int before = scrollable ? bar->maximum() - 2 : 0;
                if (bar) bar->setValue(before);
                expect(scrollable && before > 0 && bar->value() == before,
                       "scroll position moved away from the top");
                // 一份从盘上没了（成功的删除），另一份仍在（移入回收站失败的那一项）。
                expect(QFile::remove(extras.value(4)), "remove one selected screenshot");
                dialog.refreshLifecycleBatchFiles(QStringLiteral("screenshots"));
                app.processEvents();
                const auto selected = list->selectedItems();
                expect(selected.size() == 1
                           && QFileInfo(selected.first()->data(Qt::UserRole).toString()).fileName()
                                  == QStringLiteral("Extra-03.png"),
                       "refresh keeps the surviving selection and does not pick another row");
                // 重列把旧 item 全 `clear()` 掉了，旧行号一律作废：按文件名重找 Alpha 那一行，
                // 找不到就是断言失败，不许拿 `item()` 的空指针去解引用。
                const int alphaRow = rowOf(QStringLiteral("Alpha-2026.png"));
                auto *alphaItem = alphaRow >= 0 ? list->item(alphaRow) : nullptr;
                expect(alphaItem != nullptr && alphaItem->isHidden()
                           && search->text() == QStringLiteral("Extra"),
                       "refresh keeps the search filter and the hidden rows");
                expect(bar != nullptr && bar->maximum() >= before && bar->value() == before,
                       "refresh restores the vertical scroll position");
                // 列表缩到一屏装得下：旧的位置夹到新的最大值以内，不停在旧的数上。
                for (int index = 0; index < extras.size(); ++index)
                    if (index != 3) QFile::remove(extras.at(index));
                dialog.refreshLifecycleBatchFiles(QStringLiteral("screenshots"));
                app.processEvents();
                expect(bar != nullptr && bar->value() == 0 && bar->maximum() == 0,
                       "a shorter list clamps the restored scroll position to the new maximum");
                // 收尾：补出来的那几张删掉、搜索清掉，后面的用例还看得到原来那两张。
                QFile::remove(extras.value(3));
                search->clear();
                dialog.refreshLifecycleBatchFiles(QStringLiteral("screenshots"));
                app.processEvents();
                expect(list->count() == 2, "the screenshot fixture is back to its original two files");
            }
        }
        {
            const QString folder = scratch.path() + QStringLiteral("/mixed-mods");
            expect(QDir().mkpath(folder + QStringLiteral("/libraries")), "create mixed selection fixture");
            for (const QString &name : {QStringLiteral("active.jar"), QStringLiteral("inactive.jar.disabled")}) {
                QFile file(folder + QLatin1Char('/') + name);
                expect(file.open(QIODevice::WriteOnly), "create toggle fixture file");
            }
            InstanceFilesPage files;
            files.setCanToggle(true);
            files.setBatchMode(true);
            files.setDirectory(folder);
            files.list()->selectAll();
            QStringList affected;
            bool disabling = false;
            int requests = 0;
            QObject::connect(&files, &InstanceFilesPage::batchToggleRequested, &files,
                [&](const QStringList &paths, bool disable) { affected = paths; disabling = disable; ++requests; });
            auto *disable = files.selectionButtonAt(static_cast<int>(InstanceFilesPage::SelectionAction::Disable));
            auto *enable = files.selectionButtonAt(static_cast<int>(InstanceFilesPage::SelectionAction::Enable));
            disable->click();
            expect(disabling && affected == QStringList{folder + QStringLiteral("/active.jar")},
                   "mixed selection disables only active files, not folders or already-disabled files");
            expect(disable->toolTip().contains(QStringLiteral("禁用 1"))
                       && disable->toolTip().contains(QStringLiteral("2 项不变")),
                   "toggle tooltip explains affected and unchanged counts");
            enable->click();
            expect(!disabling && affected == QStringList{folder + QStringLiteral("/inactive.jar.disabled")},
                   "mixed selection enables only disabled files");
            files.list()->clearSelection();
            disable->click();
            enable->click();
            expect(requests == 2 && !disable->isEnabled() && !enable->isEnabled(),
                   "empty selection cannot emit a toggle request");
        }
        {
            ConfigFileEditorPage editor;
            int requests = 0;
            QObject::connect(&editor, &ConfigFileEditorPage::historyRequested, &editor,
                [&](const QString &path) { if (path == QStringLiteral("/config/test.json")) ++requests; });
            editor.setFile(QStringLiteral("/config/test.json"), ConfigFileText::decode("{}"));
            expect(editor.historyButton()->isHidden(), "config history hidden without backups");
            editor.setBackupAvailable(true);
            editor.historyButton()->click();
            expect(!editor.historyButton()->isHidden() && requests == 1,
                   "config history emits request for current file");
            editor.setFile(QStringLiteral("/config/other.json"), ConfigFileText::decode("{}"));
            expect(editor.historyButton()->isHidden() && !editor.historyButton()->isEnabled(),
                   "switching config file clears stale history availability");
        }
        ResourceDetailPage resource;
        int configRequests = 0;
        QObject::connect(&resource, &ResourceDetailPage::modSettingsRequested,
                         &resource, [&configRequests] { ++configRequests; });
        resource.modSettingsButton()->click();
        expect(configRequests == 0, "download details cannot request local config");
        resource.setModSettingsSlot(true);
        resource.modSettingsButton()->click();
        expect(configRequests == 1, "instance mod settings emits config request");
        resource.setModSettingsSlot(false);
        resource.modSettingsButton()->click();
        expect(configRequests == 1 && !resource.modSettingsButton()->isEnabled(),
               "reusing detail page for downloads resets config access");
        {
            // #136 深色下构造、再切浅色：游玩记录两行和页眉副题要跟着换色。
            const Theme::Spec before = Theme::current();
            Theme::setSpec(Theme::presetSpec(QStringLiteral("dark")));
            Theme::apply(app);
            Dlg themed(QStringLiteral("Test"), QStringLiteral("test"), Dlg::Override(),
                       Dlg::GlobalValues(), {}, true);
            themed.show();
            app.processEvents();
            Theme::setSpec(Theme::presetSpec(QStringLiteral("emerald")));
            Theme::apply(app);
            app.processEvents();
            for (const QString &name : {QStringLiteral("instanceLastLaunch"), QStringLiteral("instancePlayTime"),
                                        Dlg::headerMetaName()}) {
                auto *label = themed.findChild<QLabel *>(name);
                expect(label && label->palette().color(QPalette::WindowText) == Theme::tokens().secondary,
                       "secondary text line follows the theme switched after construction");
            }
            Theme::setSpec(before);
            Theme::apply(app);
        }
        return failures == 0 ? 0 : 1;
    
 return 0;
}


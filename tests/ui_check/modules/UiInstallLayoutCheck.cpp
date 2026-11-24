#include "Support.hpp"
#include "PresetEditorPage.hpp"
#include "PresetOptionsEditor.hpp"
#include "NewServerPage.hpp"
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include "PresetInstallPicker.hpp"
#include "LoadingState.hpp"
#include <QJsonArray>
int UiInstallLayoutCheck(QApplication &app) {

        const int fontId = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc")));
        const auto families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) app.setFont(QFont(families.first(), 10));
        app.setStyle(Theme::createStyle());
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        Anim::setEnabled(false);
        QMainWindow window;
        MainWindowUI ui(&window);
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::downloadTabIndex());
        ui.getDownloadStack()->setCurrentIndex(0);
        ui.getGameStack()->setCurrentIndex(1);
        int failures = 0;
        const auto expect = [&](bool ok, const char *label) {
            printf("%s %s\n", ok ? "PASS" : "FAIL", label);
            if (!ok) ++failures;
        };
        {
            QWidget *chipRow = window.findChild<QWidget *>(MainWindowUI::downloadCategoryChipsName());
            const int totalChips = MainWindowUI::downloadCategoryTexts().size();
            const auto visibleChipIndices = [&] {
                QList<int> shown;
                for (int i = 0; i < totalChips; ++i) {
                    if (auto *chip = chipRow ? chipRow->findChild<QPushButton *>(
                                                   MainWindowUI::downloadCategoryChipName(i))
                                             : nullptr) {
                        if (!chip->isHidden()) shown.append(i);
                    }
                }
                return shown;
            };
            const QList<int> installOnly = {MainWindowUI::downloadCategoryGameIndex(),
                                            MainWindowUI::downloadCategoryServerIndex()};
            expect(visibleChipIndices() == installOnly,
                   "game install page shows only game and server top category chips");
            ui.getGameStack()->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
            expect(visibleChipIndices().size() == totalChips,
                   "returning to game version list restores all seven top category chips");
            ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryServerIndex());
            ui.getServerStack()->setCurrentIndex(MainWindowUI::downloadServerFormIndex());
            expect(visibleChipIndices() == installOnly,
                   "server install form shows only game and server top category chips");
            ui.getServerStack()->setCurrentIndex(MainWindowUI::downloadServerListIndex());
            expect(visibleChipIndices().size() == totalChips,
                   "returning to server version list restores all seven top category chips");
            ui.getDownloadStack()->setCurrentIndex(MainWindowUI::downloadCategoryGameIndex());
            ui.getGameStack()->setCurrentIndex(MainWindowUI::downloadGameInstallIndex());
            expect(visibleChipIndices() == installOnly,
                   "re-entering game install page hides resource category chips again");
        }
        auto *tree = ui.getLoaderTree();
        int visibleRows = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *row = tree->topLevelItem(i);
            if (!row->isHidden()) ++visibleRows;
            if (row->data(0, Qt::UserRole).toInt() == MainWindowUI::modLoaderNoneIndex())
                expect(row->isHidden(), "vanilla sentinel is not a duplicate visible option");
        }
        expect(visibleRows == 6, "five loaders and one preset entry are visible");
        const QIcon themedGear = VersionIcons::actionIcon(VersionIcons::Action::Settings);
        const QIcon themedDetails = VersionIcons::actionIcon(VersionIcons::Action::Details);
        const QIcon themedDelete = VersionIcons::actionIcon(VersionIcons::Action::Delete);
        QImage lastIcon;
        for (const auto &preset : Theme::presetIds()) {
            if (preset == QStringLiteral("system")) continue; // May legitimately match a named preset.
            auto spec = Theme::defaultSpec();
            spec.presetId = preset;
            spec.accent = Theme::tokens(preset).primary;
            Theme::setSpec(spec);
            Theme::apply(app);
            const QImage normal = themedGear.pixmap(24, 24, QIcon::Normal).toImage();
            const QImage disabled = themedGear.pixmap(24, 24, QIcon::Disabled).toImage();
            expect(!normal.isNull() && normal != disabled, "action icons preserve a disabled colour");
            if (!lastIcon.isNull()) expect(normal != lastIcon, "existing icon follows theme changes");
            lastIcon = normal;
            for (const auto &icon : {themedDetails, themedDelete}) {
                const QImage enabled = icon.pixmap(24, 24, QIcon::Normal).toImage();
                expect(!enabled.isNull() && enabled != icon.pixmap(24, 24, QIcon::Disabled).toImage(),
                       "details and delete have distinct enabled and disabled icons");
            }
        }
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        for (const QSize size : {QSize(900, 620), QSize(1200, 800)}) {
            window.resize(size);
            window.show();
            for (int pass = 0; pass < 5; ++pass) app.processEvents();
            auto *button = ui.getInstallButton();
            auto *scroll = ui.getInstallScroll();
            const QPoint origin = scroll->mapFromGlobal(button->mapToGlobal(QPoint()));
            expect(qAbs(origin.x() + button->width() / 2 - scroll->width() / 2) <= 2,
                   "install action is centred in the right pane");
            expect(origin.y() > 0 && origin.y() + button->height() <= scroll->height(),
                   "floating action remains inside the content pane");
            expect(!scroll->isAncestorOf(button), "action does not scroll with the form");
            expect(scroll->height() - origin.y() - button->height() <= 2,
                   "game install action sits at the bottom of its content pane");
            auto *row = tree->topLevelItem(0);
            const int stableWidth = tree->viewport()->width();
            const QRect clearBefore = VersionTreeDelegate::clearRect(tree->visualItemRect(row));
            const QRect arrowBefore = VersionTreeDelegate::arrowRect(tree->visualItemRect(row));
            for (int i = 0; i < 20; ++i) new QTreeWidgetItem(row, {QString::number(i)});
            row->setExpanded(true);
            // Nested layouts post their own LayoutRequest events; one pass only
            // updates the inner tree, not necessarily the outer scroll range.
            for (int pass = 0; pass < 5; ++pass) app.processEvents();
            expect(tree->verticalScrollBar()->maximum() > 0, "expanded loader list exercises a real scrollbar");
            expect(tree->viewport()->width() == stableWidth &&
                       VersionTreeDelegate::clearRect(tree->visualItemRect(row)).x() == clearBefore.x() &&
                       VersionTreeDelegate::arrowRect(tree->visualItemRect(row)).x() == arrowBefore.x(),
                   "scrollbar appearance preserves text width and arrow/clear positions");
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            QTreeWidgetItem *lastVisibleItem = tree->topLevelItem(MainWindowUI::modLoaderNoneIndex());
            tree->scrollToItem(lastVisibleItem);
            for (int pass = 0; pass < 5; ++pass) app.processEvents();
            expect(scroll->mapFromGlobal(button->mapToGlobal(QPoint())) == origin, "expansion and scrolling do not move the action");
            const QPoint last = scroll->mapFromGlobal(tree->viewport()->mapToGlobal(tree->visualItemRect(lastVisibleItem).bottomLeft()));
            printf("layout last=%d action=%d outer=%d/%d tree=%d/%d\n", last.y(), origin.y(),
                   scroll->verticalScrollBar()->value(), scroll->verticalScrollBar()->maximum(),
                   tree->verticalScrollBar()->value(), tree->verticalScrollBar()->maximum());
            expect(last.y() < origin.y(), "last visible entry can scroll above the floating action");
            row->setExpanded(false);
            qDeleteAll(row->takeChildren());
            for (int pass = 0; pass < 5; ++pass) app.processEvents();
            if (app.arguments().contains(QStringLiteral("--install-layout-preview"))) {
                scroll->verticalScrollBar()->setValue(0);
                app.processEvents();
                QDir().mkpath(QStringLiteral("build/install-previews"));
                window.grab().save(QStringLiteral("build/install-previews/install-%1.png").arg(size.width()));
            }
        }
        {
            ui.setInstanceMode(MainWindowUI::InstanceMode::Preset);
            auto *instancePage=ui.getTabWidget()->widget(MainWindowUI::instancesTabIndex());
            // 管理页按设计稿没有左栏了（02ba270）：游戏文件夹是标题右边那颗胶囊，预设也属于当前文件夹，所以每一档都在。
            auto *folderPill=instancePage->findChild<QPushButton *>(MainWindowUI::instanceFolderPillName());
            expect(folderPill&&!folderPill->isHidden(),"preset mode keeps the game folder pill, same as instances");
            ui.navigationGuard=[] { return false; };
            // 点模式钮换页由 MainWindow 接（ConnectionsController → setInstanceMode）；这里只有界面层，照它接上。
            if (auto *client=ui.getInstanceModeButton(MainWindowUI::InstanceMode::Client)) {
                const auto wire=QObject::connect(client,&QPushButton::clicked,[&ui]{ui.setInstanceMode(MainWindowUI::InstanceMode::Client);});
                client->click();
                QObject::disconnect(wire);
            }
            expect(ui.getInstanceModeStack()->currentIndex()==static_cast<int>(MainWindowUI::InstanceMode::Preset) && ui.getInstanceModeButton(MainWindowUI::InstanceMode::Preset)->isChecked()
                   && !ui.getInstanceModeButton(MainWindowUI::InstanceMode::Client)->isChecked(),"cancelled preset departure keeps the active mode indicator");
            ui.navigationGuard={};
            ui.setInstanceMode(MainWindowUI::InstanceMode::Client);
            expect(folderPill&&!folderPill->isHidden(),"returning to client instances keeps the game folder pill");
            PresetEditorPage editor(QJsonObject{{"id","editor-test"},{"name","Original"}});
            if(app.arguments().contains(QStringLiteral("--install-layout-preview"))){
                editor.resize(980,640);editor.show();app.processEvents();QDir().mkpath("build/install-previews");editor.grab().save("build/install-previews/preset-editor.png");
            }
            editor.confirmReset=[](const QString &){return true;};
            auto *name=editor.findChild<QLineEdit *>("presetEditorName");
            expect(!editor.isDirty(),"new editor opens without false unsaved changes");
            name->setText("Changed");expect(editor.isDirty(),"name edits join the unsaved document");
            auto *nav=editor.findChild<QListWidget *>("presetEditorNavigation");int back=0;editor.backRequested=[&]{++back;};nav->setCurrentRow(2);
            expect(editor.isDirty()&&back==0,"internal page switches retain edits without leaving");
            editor.findChild<QPushButton *>("presetEditorRestore")->click();
            expect(!editor.isDirty()&&editor.document().value("name")=="Original","restore reloads the last saved document");
            bool draft=false;editor.saveRequested=[&](bool value){draft=value;};editor.findChild<QPushButton *>("presetEditorSaveDraft")->click();
            expect(draft,"draft button emits a draft save rather than publishing");
            editor.findChild<QPushButton *>("presetEditorClear")->click();
            expect(editor.document().value("name")=="Original","clear preserves the preset name");
            // 模组那一页在编辑器里是「嵌进来的对话框」：`embed()` 会把 QDialogButtonBox 和
            // 直属的说明文字藏掉，认不出平台的那两件不在那些里面，得照样露着。
            auto *onlyUnknown=editor.findChild<QCheckBox *>("presetModOnlyUnknown");
            auto *removeUnknown=editor.findChild<QPushButton *>("presetModRemoveUnknown");
            expect(onlyUnknown&&removeUnknown&&!onlyUnknown->isHidden()&&!removeUnknown->isHidden(),
                   "embedded mod editor keeps the unrecognized-row filter and bulk removal visible");
            QJsonObject packs{{"id","pack-test"},{"name","Packs"},{"sections",QJsonArray{QJsonObject{{"id","resourcePacks"},{"kind","genericFiles"},{"items",QJsonArray{QJsonObject{{"path","resourcepacks/low.zip"},{"enabled",true},{"priority",0}},QJsonObject{{"path","resourcepacks/high.zip"},{"enabled",true},{"priority",1}}}}}}}};
            PresetEditorPage packEditor(packs);
            auto *packTable=packEditor.findChild<QTableWidget *>("presetEditorFiles_resourcePacks");
            expect(packTable&&packTable->item(0,0)->text().endsWith("high.zip"),"resource pack editor displays highest priority first");
            const auto storedPacks=packEditor.document().value("sections").toArray().first().toObject().value("items").toArray();
            expect(storedPacks.first().toObject().value("priority").toInt()>storedPacks.last().toObject().value("priority").toInt(),"saving the resource pack editor preserves effective pack priority");
            PresetEditorPage metadataEditor(QJsonObject{{"id","metadata"},{"name","Metadata"},{"sections",QJsonArray{QJsonObject{{"id","original-section"},{"kind","vanillaOptions"},{"mode","replace"},{"items",QJsonArray{QJsonObject{{"key","guiScale"},{"value","2"},{"extra","retained"}}}}}}}});
            const auto metadata=metadataEditor.document().value("sections").toArray().first().toObject();
            expect(metadata.value("mode")=="replace"&&metadata.value("items").toArray().first().toObject().value("extra")=="retained","editing preserves existing section policy and item metadata");
            PresetInstallPicker picker;
            picker.setContext("1.21.1", "Fabric");
            PresetInstallPicker::Category game;game.key="gameSettings";game.title="游戏设置";game.options.append({"game-child","日常配置",true,true});
            PresetInstallPicker::Category mods;mods.key="mods";mods.title="模组";mods.options.append({"mod-child","日常配置",true,true}); mods.options.append({"mod-other","建筑辅助",true,false});
            PresetInstallPicker::Category packCategory; packCategory.key="resourcePacks"; packCategory.title="资源包";
            PresetInstallPicker::Category dataCategory; dataCategory.key="dataPacks"; dataCategory.title="数据包";
            PresetInstallPicker::Category shaderCategory; shaderCategory.key="shaderPacks"; shaderCategory.title="光影包";
            picker.setCategories({game,mods,packCategory,dataCategory,shaderCategory});picker.setComposites({{"parent","日常配置",{{"gameSettings","game-child"},{"mods","mod-child"}}}});
            {
                // 步骤 133：右栏加载态、综合预设空态、「显示综合预设」是开关。
                PresetInstallPicker loading;
                loading.setContext("1.21.1", "Fabric");
                loading.beginLoading();
                auto *stack = loading.findChild<QStackedWidget *>();
                expect(stack && LoadingState::overlay(stack), "preset picker right pane shows the shared loading state while data is pending");
                // 步骤 136：读不了 ⇒ 出错态 +「重试」，点了再 begin。
                int retried = 0;
                loading.failLoading("预设文件夹读不了：X", [&] { ++retried; loading.beginLoading(); });
                QWidget *layer = stack ? LoadingState::overlay(stack) : nullptr;
                auto *retry = layer ? layer->findChild<QPushButton *>(LoadingState::retryButtonName()) : nullptr;
                expect(layer && layer->property("state").toString() == "error" && retry,
                       "preset picker data failure shows the error state with a retry button");
                if (retry) retry->click();
                expect(retried == 1 && LoadingState::overlay(stack) &&
                           LoadingState::overlay(stack)->property("state").toString() == "loading",
                       "retry on the preset picker asks again and goes back to loading");
                loading.setCategories({game, mods});
                expect(!LoadingState::overlay(stack), "setCategories retires the preset picker loading state");
                // 步骤 136：兼容检查没查完时，选着那一行的行尾挂一行加载态；查完 / 换一行就撤。
                auto *modList = loading.findChild<QListWidget *>(PresetInstallPicker::listName(1));
                const auto checking = [modList](int row) {
                    return modList && modList->item(row) &&
                           modList->item(row)->data(LoadingState::rowLoadingRole()).isValid();
                };
                if (modList) {
                    modList->setCurrentRow(1);
                    loading.setChecking("mods", true);
                    const qint64 started = modList->item(1)->data(LoadingState::rowLoadingRole()).toLongLong();
                    expect(checking(1) && !checking(0) && !checking(2) && LoadingState::clockRunning(),
                           "compatibility check in progress puts the inline loading state on the selected row only");
                    loading.setStatus("mods", "检查中 1/3", {}, false);
                    loading.setChecking("mods", true);
                    expect(checking(1) &&
                               modList->item(1)->data(LoadingState::rowLoadingRole()).toLongLong() == started,
                           "progress updates keep the same loading start (no re-gated flicker)");
                    loading.setChecking("mods", false);
                    expect(!checking(1), "finished compatibility check retires the row loading state");
                    loading.setChecking("mods", true);
                    modList->setCurrentRow(0);
                    expect(!checking(1) && !checking(0) && loading.selectedId("mods").isEmpty(),
                           "switching to none drops the previous row's loading state");
                } else {
                    expect(false, "preset picker mods list exists");
                }
                loading.setComposites({});
                expect(!loading.findChild<QListWidget *>("presetCompositeList") &&
                           loading.findChild<QLabel *>(PresetInstallPicker::emptyLabelName(2)) &&
                           loading.findChild<QPushButton *>(PresetInstallPicker::manageButtonName(2)),
                       "empty composite page shows an empty state with a manage button instead of an empty list");
                auto *show = loading.findChild<QCheckBox *>("presetShowComposite1");
                expect(show && show->property("wtfmlSwitch").toBool(),
                       "show-composite toggle is a switch on pages that list composite presets");
            }
            picker.findChild<QListWidget *>("presetCompositeList")->setCurrentRow(0);
            expect(picker.selectedId("gameSettings")=="game-child"&&picker.selectedId("mods")=="mod-child","composite selection maps to independent category parts");
            picker.findChild<QAbstractButton *>("presetCompositePart_mods")->setChecked(false);
            expect(picker.selectedId("mods").isEmpty()&&picker.selectedId("gameSettings")=="game-child","unchecking a composite part leaves other parts selected");
            auto *modChoices = picker.findChild<QListWidget *>(PresetInstallPicker::listName(1));
            expect(modChoices && modChoices->count() == 3 &&
                   modChoices->selectionMode() == QAbstractItemView::SingleSelection,
                   "preset categories use a visible single-choice list including none");
            if (modChoices) {
                modChoices->setCurrentRow(1);
                expect(picker.selectedId("mods") == "mod-child", "list choice updates the applied category part");
                picker.setStatus("mods", "缺失 2 项", {"Required mod is missing"}, true, 8);
                expect(modChoices->currentItem()->toolTip().contains("Required mod"),
                       "preset compatibility reasons stay attached to the selected row");
                modChoices->setCurrentRow(0);
                expect(picker.selectedId("mods").isEmpty(), "none row explicitly clears the category selection");
            }
            picker.setComposites({{"parent","日常配置",{{"gameSettings","game-child"},{"mods","mod-child"}}}});
            expect(picker.findChildren<QWidget *>("presetCompositePicker").size() == 1,
                   "refreshing composite choices replaces the previous panel");
            picker.setCurrentTab(1);
            if (modChoices) {
                modChoices->setCurrentRow(1);
                picker.setStatus("mods", "缺失 2 项", {"一个必要模组没有适配版本", "一个可选模组没有适配版本"}, true, 8);
                picker.resize(740, 440);
                picker.show();
                app.processEvents();
                picker.findChild<QPushButton *>(PresetInstallPicker::reasonsButtonName(1))->click();
                app.processEvents();
                auto *details = picker.findChild<QDialog *>("presetCompatibilityDetails");
                expect(details && details->isVisible() && details->isWindow(),
                       "preset compatibility details open in an independent window");
                if (details) details->close();
                auto *navigation = picker.findChild<QListWidget *>(PresetInstallPicker::tabBarName());
                expect(navigation && navigation->count() == 6 && picker.findChild<QWidget *>("presetInstallSummary"),
                       "preset picker has six sidebar categories and a fixed source summary");
                const QString capture = qEnvironmentVariable("WTFML_PRESET_PICKER_PREVIEW");
                if (!capture.isEmpty()) {
                    picker.findChild<QListWidget *>("presetCompositeList")->setCurrentRow(0);
                    app.processEvents();
                    expect(picker.grab().save(capture), "preset picker preview saved");
                }
                modChoices->setCurrentRow(2);
                expect(picker.selectedId("mods") == "mod-child" &&
                       picker.findChild<QWidget *>("presetReplacementPrompt")->isVisible(),
                       "replacing a category waits for explicit confirmation");
                picker.findChild<QPushButton *>("presetReplaceCancel")->click();
                expect(picker.selectedId("mods") == "mod-child", "cancelled replacement preserves original source");
                modChoices->setCurrentRow(2);
                picker.findChild<QPushButton *>("presetReplaceConfirm")->click();
                expect(picker.selectedId("mods") == "mod-other" && picker.selectedId("gameSettings") == "game-child",
                       "confirmed mod replacement preserves other category sources");
                auto *source = picker.findChild<QLabel *>("presetSource_mods");
                expect(source && source->text().contains("建筑辅助"), "summary reflects the final per-category source");
                picker.findChild<QAbstractButton *>("presetCompositePart_gameSettings")->setChecked(false);
                expect(picker.selectedId("mods") == "mod-other", "unchecking another composite part preserves independently replaced mods");
                picker.setContext("1.21.1", QString());
                expect(picker.selectedId("mods").isEmpty() &&
                       !(modChoices->item(1)->flags() & Qt::ItemIsEnabled),
                       "vanilla context clears and disables mod presets");
                picker.findChild<QListWidget *>("presetCompositeList")->setCurrentRow(0);
                expect(picker.selectedId("mods").isEmpty() &&
                       !picker.findChild<QAbstractButton *>("presetCompositePart_mods")->isEnabled(),
                       "composite preset cannot bypass the mod loader requirement");

            }
        }
        {
            QJsonObject shader{{"id", "legacy-shader"}, {"kind", "genericFiles"},
                {"items", QJsonArray{QJsonObject{{"path", "shaderpacks/example.zip"}}}}};
            QJsonObject pack{{"id", "legacy-pack"}, {"kind", "genericFiles"},
                {"items", QJsonArray{QJsonObject{{"path", "resourcepacks/example.zip"}}}}};
            PresetEditorPage legacy(QJsonObject{{"name", "Legacy"},
                {"sections", QJsonArray{pack, shader}},
                {"enabledParts", QJsonObject{{"resourcePacks", false}}}});
            auto *shaders = legacy.findChild<QTableWidget *>("presetEditorFiles_shaderPacks");
            auto *packs = legacy.findChild<QTableWidget *>("presetEditorFiles_resourcePacks");
            auto *enabled = legacy.findChild<QCheckBox *>("presetEditorEnabled_shaderPacks");
            expect(shaders && packs && shaders->rowCount() == 1 && packs->rowCount() == 1,
                   "editor separates legacy shader files from resource pack rows");
            expect(enabled && !enabled->isChecked(), "legacy disabled resource content does not silently enable shaders");
            if (enabled) enabled->setChecked(true);
            expect(legacy.document().value("enabledParts").toObject().value("shaderPacks").toBool(),
                   "shader preset section can be enabled independently");
        }
        {
            PresetOptionsEditor detailed({}, nullptr, PresetOptionsEditor::Mode::Options);
            auto *table = detailed.findChild<QTableWidget *>("presetOptionsEditorTable");
            expect(table && table->rowCount() >= 35 && detailed.entries().isEmpty(),
                   "blank preset exposes detailed settings without silently selecting defaults");
            auto rowFor = [table](const QString &key) {
                for (int row=0;row<table->rowCount();++row) if (table->item(row,0)->text()==key) return row;
                return -1;
            };
            const int gamma = rowFor("gamma");
            auto *brightness = detailed.findChild<QDoubleSpinBox *>("presetValue_gamma");
            expect(gamma>=0 && brightness && !brightness->isEnabled(), "unselected settings cannot be edited accidentally");
            if (gamma>=0 && brightness) {
                table->item(gamma,0)->setCheckState(Qt::Checked);
                brightness->setValue(75);
                expect(detailed.entries().size()==1 && detailed.entries().first().value=="0.75",
                       "percentage control stores the game normalized value only when selected");
                table->item(gamma,0)->setCheckState(Qt::Unchecked);
                expect(detailed.entries().isEmpty(), "unchecking excludes an option without deleting its edited value");
                table->item(gamma,0)->setCheckState(Qt::Checked);
                expect(detailed.entries().first().value=="0.75", "reselecting retains the edited value");
                brightness->findChild<QLineEdit *>()->setText("86.5%");
                expect(detailed.entries().first().value=="0.865", "saving commits a pending numeric edit even before focus leaves the control");
            }
            PresetOptionsEditor imported({{"gamma","2.5"},{"futureOption","keep:raw"}}, nullptr, PresetOptionsEditor::Mode::Options);
            expect(imported.entries().size()==2 && imported.entries()[0].value=="2.5" && imported.entries()[1].value=="keep:raw",
                   "unknown options and out-of-range imported values survive opening the editor unchanged");
            PresetOptionsEditor bindings({}, nullptr, PresetOptionsEditor::Mode::Keys);
            auto *keyTable = bindings.findChild<QTableWidget *>("presetOptionsEditorTable");
            expect(keyTable->rowCount()>=25 && bindings.entries().isEmpty(), "key page exposes vanilla bindings independently of settings");
            auto *jump = bindings.findChild<QPushButton *>("presetValue_key_key.jump");
            int jumpRow=-1;
            for(int row=0;row<keyTable->rowCount();++row) if(keyTable->item(row,0)->text()=="key_key.jump") jumpRow=row;
            if(jump && jumpRow>=0) {
                keyTable->item(jumpRow,0)->setCheckState(Qt::Checked);
                jump->click();
                QKeyEvent press(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
                QApplication::sendEvent(jump,&press);
                expect(bindings.entries().size()==1 && bindings.entries().first().value=="key.keyboard.r",
                       "key capture writes Minecraft binding rather than Qt shortcut text");
                jump->click();
                QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
                QApplication::sendEvent(jump,&cancel);
                expect(bindings.entries().first().value=="key.keyboard.r", "escape cancels key recording without losing the binding");
                jump->click();
                QKeyEvent clear(QEvent::KeyPress,Qt::Key_Backspace,Qt::NoModifier);
                QApplication::sendEvent(jump,&clear);
                expect(bindings.entries().first().value=="key.keyboard.unknown", "clearing a binding uses the game unbound key value");
            } else expect(false,"jump binding control exists");
            auto *search=detailed.findChild<QLineEdit *>("presetOptionsSearch");
            search->setText("声音");
            expect(table->isRowHidden(rowFor("fov")) && !table->isRowHidden(rowFor("soundCategory_master")),
                   "settings search matches categories without changing selection");
        }
        {
            NewServerPage server;
            NewServerPage::FlavorOption fabric;
            fabric.token="fabric"; fabric.name="Fabric"; fabric.needsLoader=true; fabric.needsInstaller=true;
            server.setFlavors({fabric},"fabric");
            auto *installer=server.findChild<QComboBox *>(MainWindowUI::newServerInstallerEditName()+"Combo");
            server.setVersionChoices(true,{"1.0.0","0.9.0"});
            expect(installer && installer->lineEdit()->isReadOnly() && server.installerVersion()=="1.0.0",
                   "server installer versions are selectable without manual version entry");
            installer->setCurrentIndex(1);
            server.setVersionChoices(true,{"1.0.0","0.9.0"});
            expect(server.installerVersion()=="0.9.0","refreshing server installer versions preserves an available selection");
            server.setVersionChoices(true,{},"Retry");
            expect(!installer->isEnabled() && server.installerVersion().isEmpty(),"failed server metadata cannot retain a stale installer version");
            auto *summary=server.findChild<QLabel *>(MainWindowUI::newServerSummaryLabelName());
            expect(summary && summary->isHidden(), "empty server summary reserves no vertical space");
            server.setSummary({}, "Invalid name");
            expect(!summary->isHidden(), "server errors remain visible when present");
            server.setSummary({}, {});
            expect(summary->isHidden(), "clearing the server error removes its reserved row");
            server.resize(720,520); server.show();
            for(int pass=0;pass<5;++pass) app.processEvents();
            auto *content=server.findChild<QWidget *>("newServerContent");
            auto *action=server.findChild<QPushButton *>(MainWindowUI::newServerCreateButtonName());
            const int bottom=action->mapTo(content,QPoint(0,action->height())).y();
            expect(content->height()-bottom<=2,"server install action shares the bottom placement of game installation");
            server.close();
        }
        return failures == 0 ? 0 : 1;
    
 return 0;
}


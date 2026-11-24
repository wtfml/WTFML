/**
 * @file InstanceUpdateFlow.cpp
 * @brief 将目标核心安装与当前实例更新串联；只使用现有下载器和快照事务。
 */
#include "InstanceUpdateFlow.hpp"
#include "InputSizing.hpp"
#include "TextMarks.hpp"
#include "MainWindow.hpp"
#include "InstanceSettingsDialog.hpp"
#include "InstanceUpdateWizard.hpp"
#include "InstanceUpgradePage.hpp"
#include "UpgradeReportDialog.hpp"
#include "UpgradeHistoryDialog.hpp"
#include "MainWindowUI.hpp"
#include "InlineMessage.hpp"
#include "LoadingState.hpp"
#include "Theme.hpp"
#include "controllers/InstanceTransferController.hpp"
#include "controllers/InstancePagesController.hpp"
#include "controllers/DownloadCompletionController.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/InstallPageController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "manage/VersionManager.hpp"
#include "operations/InstanceUpgrade.hpp"
#include "operations/UpgradePageState.hpp"
#include "operations/TransferPlanner.hpp"
#include "operations/UpgradeReport.hpp"
#include "operations/UpgradeHistory.hpp"
#include "operations/OperationReport.hpp"
#include "presets/PresetModInstaller.hpp"
#include "manage/FolderManager.hpp"
#include "manage/InstanceFiles.hpp"
#include "launcher/LauncherManager.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QDirIterator>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QVBoxLayout>
#include <memory>

namespace {
struct Selection {
    QString root, source, game, loader, loaderVersion, core, problem;
    bool preparing = false;
    bool executing = false;
    bool finished = false;
    bool requiresReload = false;
    bool evaluated = false;
    Tasks::TaskId firstTask = 0;
    /// 这一趟「更新实例」的父任务：准备核心 / 复制 / 快照 / 发布都只是它的步骤。
    /// 任务中心只露这一行（子任务由 `TaskManager::beginChild()` 收在它底下）。
    Tasks::TaskId parent = 0;
    /// 准备阶段用户按了取消（与"失败"分开收尾）。
    bool prepCancelled = false;
    /// 正在下载平台适配模组（这一段可取消，取消后整趟回滚）。
    bool downloading = false;
    QPointer<Presets::PresetModInstaller> modInstaller;
    QString modStageDir;
    /// 替换阶段的报告编号与实际文件清单：引擎在**应用阶段**失败时也要写同一种原因报告。
    QString replacementReportId;
    QStringList replacementPreview;
    bool engineCancelled = false;
};
VersionManager::LoaderKind loaderKind(const QString &id) {
    if (id == QStringLiteral("forge")) return VersionManager::LoaderKind::Forge;
    if (id == QStringLiteral("neoforge")) return VersionManager::LoaderKind::NeoForge;
    if (id == QStringLiteral("optifine")) return VersionManager::LoaderKind::OptiFine;
    if (id == QStringLiteral("quilt")) return VersionManager::LoaderKind::Quilt;
    return VersionManager::LoaderKind::Fabric;
}
}

void InstanceUpdateFlow::attach(MainWindow *owner, InstanceSettingsDialog *dialog, const QString &sourceId) {
    dialog->setProperty("modernUpgradeFlow", true);
    auto *legacy = dialog->findChild<QComboBox *>(InstanceSettingsDialog::transferTargetComboName("upgrade"));
    auto *tree = dialog->findChild<QTreeWidget *>("instanceUpgradeModChecks");
    auto *plan = dialog->findChild<QPlainTextEdit *>(InstanceSettingsDialog::transferPlanTextName("upgrade"));
    auto *scopes = dialog->findChild<QListWidget *>(InstanceSettingsDialog::transferItemsListName("upgrade"));
    if (!legacy || !tree || !plan || !scopes) {
        // 「升级」那一页是**第一次切过去才建**的（见 InstanceSettingsDialog::ensurePage()），
        // 开页这一刻这一页还不存在 —— 这一趟先什么都不接，等它真的建出来再接一遍。
        // 连接挂在一个自毁的小对象上：接上一次就断（同一个对话框只该接一遍）；页一直没建
        // 就跟着对话框一起没。
        auto *once = new QObject(dialog);
        QObject::connect(dialog, &InstanceSettingsDialog::pageBuilt, once,
                         [owner, dialog, sourceId, once](const QString &key) {
                             if (key != QStringLiteral("upgrade")) {
                                 return;
                             }
                             once->deleteLater();
                             InstanceUpdateFlow::attach(owner, dialog, sourceId);
                         });
        return;
    }
    auto *page = legacy->parentWidget();
    auto *layout = qobject_cast<QVBoxLayout *>(page->layout());
    if (!layout) return;
    // Legacy transfer controls remain available to migration and compatibility tests,
    // but are not presented as a second update workflow.
    for (int i = 0; i < layout->count(); ++i) {
        if (auto *row = layout->itemAt(i)->layout()) {
            bool targetRow = false;
            for (int j = 0; j < row->count(); ++j) if (row->itemAt(j)->widget() == legacy) targetRow = true;
            if (targetRow) for (int j = 0; j < row->count(); ++j) if (auto *widget = row->itemAt(j)->widget()) widget->hide();
        }
    }
    scopes->parentWidget()->hide();
    for (const auto &name : {InstanceSettingsDialog::transferApplyButtonName("upgrade"),
            QStringLiteral("instanceUpgradeInPlaceButton"), QStringLiteral("instanceUpgradePrepareTarget"),
            QStringLiteral("instanceUpgradeRollbackButton")})
        if (auto *button = dialog->findChild<QPushButton *>(name)) button->hide();
    tree->setHeaderLabels({QObject::tr("当前模组"), QObject::tr("兼容检查"), QObject::tr("依据"), QObject::tr("更新时处理")});
    tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    tree->setColumnWidth(1, 220);
    plan->setFixedHeight(74);

    auto state = std::make_shared<Selection>();
    state->root = AppPaths::minecraftDir(); state->source = sourceId;
    // 目标 Java 主版本只问一次：模组的 Java 要求要和**真正会跑游戏的这份 Java** 比，
    // 而不是和"这个版本最低要哪一版"比。拿不到时记 0，评估层会把它写成"未确认"。
    // 探 Java 要起 java.exe（没有 release 的运行时尤其慢）；打开设置时只记路径，
    // 等真的做升级评估时再探（UpgradeAssessment 里）。
    dialog->setProperty("upgradeRuntimeJavaPath", owner->m_javaSettings->javaPath());
    dialog->setProperty("upgradeRuntimeJavaMajor", 0);
    const auto source = owner->m_versionManager->displayParts(sourceId);
    // 来源实例的加载器名 / 版本走 `InstancePagesController::loaderName()`，**不读
    // `source.loaderName`**：那一格只有我们自己的安装器会写，PCL / HMCL 合并出来的实例
    // （加载器只写在版本 json 的 `libraries` 里）在那儿是空的 —— 于是这一页说「纯原版」、
    // 默认选中「纯原版」、计划按原版评估模组兼容。
    QString sourceLoaderVersion;
    const QString sourceLoader = InstancePagesController::loaderName(
        source, owner->m_versionManager, &sourceLoaderVersion);
    for (const auto &task : owner->m_activityCenter->tasks()->all()) state->firstTask = qMax(state->firstTask, task.id);
    auto *selectors = new QWidget(page);
    selectors->setObjectName("instanceUpdateSelectors");
    auto *selectorLayout = new QVBoxLayout(selectors);
    selectorLayout->setContentsMargins(16, 12, 16, 12);
    auto *sourceContext = new QLabel(QObject::tr("当前实例：%1\nMinecraft %2 / %3\n\n请选择更新后的游戏与加载器版本。下一步检查现有模组，确认前不会修改实例。")
        .arg(sourceId, source.realVersion, sourceLoader.isEmpty() ? QObject::tr("纯原版") : sourceLoader), selectors);
    sourceContext->setWordWrap(true); selectorLayout->addWidget(sourceContext);
    selectorLayout->addSpacing(16);
    auto *row = new QHBoxLayout;
    auto *game = new QComboBox(selectors); game->setObjectName("instanceUpdateGame");
    game->setEditable(true); game->setInsertPolicy(QComboBox::NoInsert);
    game->completer()->setFilterMode(Qt::MatchContains);
    game->completer()->setCompletionMode(QCompleter::PopupCompletion);
    game->lineEdit()->setPlaceholderText(QObject::tr("搜索 Minecraft 版本"));
    auto *loader = new QComboBox(selectors); loader->setObjectName("instanceUpdateLoader");
    for (const auto &id : {QString(), QStringLiteral("fabric"), QStringLiteral("forge"),
                           QStringLiteral("neoforge"), QStringLiteral("optifine"),
                           QStringLiteral("quilt")})
        loader->addItem(id.isEmpty() ? QObject::tr("纯原版") : id, id);
    loader->setCurrentIndex(qMax(0, loader->findData(sourceLoader.toLower())));
    auto *loaderVersion = new QComboBox(selectors); loaderVersion->setObjectName("instanceUpdateLoaderVersion");
    row->addWidget(new QLabel(QObject::tr("1. 更新到"), selectors)); row->addWidget(game, 2);
    row->addWidget(loader, 1); row->addWidget(loaderVersion, 1);
    auto *retry = new QPushButton(QObject::tr("刷新"), selectors); retry->setObjectName("instanceUpdateRefresh");
    Theme::markButton(retry, Theme::ButtonKind::Neutral); row->addWidget(retry);
    selectorLayout->addLayout(row);
    selectorLayout->addStretch();
    auto *modeRow = new QHBoxLayout;
    auto *mode = new QComboBox(selectors); mode->setObjectName("instanceUpdateMode");
    mode->addItem(QObject::tr("创建更新副本（保留原实例）"), false);
    mode->addItem(QObject::tr("更新当前实例（先完整快照）"), true);
    auto *copyName = new QLineEdit(sourceId + QObject::tr("-更新副本"), selectors);
    copyName->setObjectName("instanceUpdateCopyName"); copyName->setPlaceholderText(QObject::tr("更新副本名称"));
    InputSizing::shortValue(copyName, InputSizing::kNameChars, InputSizing::Script::Han);
    modeRow->addWidget(new QLabel(QObject::tr("2. 更新方式"), selectors)); modeRow->addWidget(mode, 1); modeRow->addWidget(copyName, 1);
    modeRow->setContentsMargins(0, 0, 0, 0);
    auto *modePanel = new QWidget(page); modePanel->setLayout(modeRow);
    auto *note = new QLabel(QObject::tr("兼容声明不是运行保证。未知项必须明确选择处理方式，不会自动禁用；"
                                     "只有平台明确给出目标发行版的模组才提供「替换为平台适配版」，"
                                     "必要前置会自动补齐，替换失败会整趟回滚。"), page);
    note->setWordWrap(true);
    auto *status = new QLabel(page); status->setObjectName("instanceUpdateStatus"); status->setWordWrap(true);
    auto *start = new QPushButton(QObject::tr("开始更新"), page); start->setObjectName("instanceUpdateStart");
    Theme::markButton(start, Theme::ButtonKind::Primary); start->setAutoDefault(false);
    auto *rollback = new QPushButton(QObject::tr("回滚上次更新…"), page);
    Theme::markButton(rollback, Theme::ButtonKind::Neutral); rollback->setAutoDefault(false);
    QObject::connect(rollback, &QPushButton::clicked, dialog, [dialog] { emit dialog->rollbackUpgradeRequested(); });

    // Move the real controls into a native wizard. The wizard stays in the backing
    // dialog's QObject tree so assessment updates still find the same widgets.
    auto *review = new QWidget;
    auto *reviewLayout = new QVBoxLayout(review); reviewLayout->setContentsMargins(0, 0, 0, 0);
    reviewLayout->addWidget(note);
    auto *batchRow = new QHBoxLayout;
    for (const bool keep : {true, false}) {
        auto *button = new QPushButton(keep ? QObject::tr("未确认项全部保留") : QObject::tr("未确认项全部禁用"), review);
        Theme::markButton(button, Theme::ButtonKind::Neutral); batchRow->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, dialog, [tree, keep] {
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
                if (auto *choice = qobject_cast<QComboBox *>(tree->itemWidget(*it, 3)); choice && !choice->currentData().isValid())
                    choice->setCurrentIndex(keep ? 2 : 1);
        });
    }
    auto *stopQuery = new QPushButton(QObject::tr("停止平台查询，使用已有结果"), review);
    Theme::markButton(stopQuery, Theme::ButtonKind::Ghost); batchRow->addStretch(); batchRow->addWidget(stopQuery);
    QObject::connect(stopQuery, &QPushButton::clicked, dialog, [dialog] { emit dialog->stopUpgradeQueryRequested(); });
    reviewLayout->addLayout(batchRow);
    plan->hide();
    reviewLayout->addWidget(tree->parentWidget(), 1);
    auto *confirmation = new QWidget;
    auto *confirmLayout = new QVBoxLayout(confirmation);
    confirmLayout->addWidget(modePanel);
    confirmLayout->addSpacing(8);
    auto *changes = new QPlainTextEdit(confirmation);
    changes->setObjectName("instanceUpdateChanges");
    changes->setReadOnly(true);
    changes->setFixedHeight(132);
    confirmLayout->addWidget(new QLabel(QObject::tr("执行前的文件变更（模组适配替换）："), confirmation));
    confirmLayout->addWidget(changes);
    confirmLayout->addSpacing(8);
    auto *summary = new QLabel(confirmation); summary->setObjectName("instanceUpdateConfirmation"); summary->setWordWrap(true);
    confirmLayout->addWidget(summary); confirmLayout->addStretch();
    auto *progress = new QWidget;
    auto *progressLayout = new QVBoxLayout(progress); progressLayout->addWidget(status);
    auto *steps = new QListWidget(progress); steps->setObjectName("instanceUpdateExecutionSteps");
    steps->setSelectionMode(QAbstractItemView::NoSelection); progressLayout->addWidget(steps, 1);
    // 进度列表 / 变更清单：无硬描边的淡色圆角面板（颜色跟着主题走）。
    for (QWidget *panel : {static_cast<QWidget *>(steps), static_cast<QWidget *>(changes)}) {
        const auto paintPanel = [panel] {
            panel->setStyleSheet(QStringLiteral("#%1 { border: none; border-radius: 10px; background: %2; padding: 6px; }")
                                     .arg(panel->objectName(), Theme::tokens().hover.name(QColor::HexArgb)));
        };
        paintPanel();
        Theme::follow(panel, paintPanel);
    }
    // 空的时候在面板正中写一句灰字，不留一块空框。
    auto *stepsEmpty = new QLabel(QObject::tr("点击“开始更新”后在这里显示进度"), steps->viewport());
    stepsEmpty->setObjectName("instanceUpdateExecutionStepsEmpty");
    stepsEmpty->setForegroundRole(QPalette::PlaceholderText);
    stepsEmpty->setAlignment(Qt::AlignCenter);
    (new QVBoxLayout(steps->viewport()))->addWidget(stepsEmpty);
    const auto syncStepsEmpty = [steps, stepsEmpty] { stepsEmpty->setVisible(steps->count() == 0); };
    QObject::connect(steps->model(), &QAbstractItemModel::rowsInserted, stepsEmpty, syncStepsEmpty);
    QObject::connect(steps->model(), &QAbstractItemModel::rowsRemoved, stepsEmpty, syncStepsEmpty);
    QObject::connect(steps->model(), &QAbstractItemModel::modelReset, stepsEmpty, syncStepsEmpty);
    auto *retryFlow = new QPushButton(QObject::tr("返回目标选择，重新检查"), progress);
    Theme::markButton(retryFlow, Theme::ButtonKind::Neutral); progressLayout->addWidget(retryFlow); retryFlow->hide();
    auto *wizard = new InstanceUpdateWizard(selectors, review, confirmation, progress, dialog);
    dialog->setProperty("modernUpgradeWizard", true);
    start->setParent(wizard); start->hide();
    const auto hideLayout = [](QLayout *root, const auto &self) -> void {
        for (int i = 0; i < root->count(); ++i) {
            if (auto *widget = root->itemAt(i)->widget()) widget->hide();
            if (auto *child = root->itemAt(i)->layout()) self(child, self);
        }
    };
    hideLayout(layout, hideLayout);
    auto *entry = new QLabel(QObject::tr("在独立向导中选择目标、检查模组并确认更新。更新当前实例前会创建完整快照。"), page);
    entry->setWordWrap(true); layout->addWidget(entry);
    auto *open = new QPushButton(QObject::tr("打开实例更新向导…"), page); open->setObjectName("instanceUpdateOpenWizard");
    Theme::markButton(open, Theme::ButtonKind::Primary); layout->addWidget(open, 0, Qt::AlignLeft);
    // 新页面已经是入口：旧的"打开实例更新向导…"不再露出来（向导只在兜底分支里出现）。
    entry->hide();
    open->hide();
    rollback->show(); layout->addWidget(rollback, 0, Qt::AlignLeft); layout->addStretch();
    const auto showWizard = [wizard, dialog, state] {
        if (state->requiresReload) return;
        if (!wizard->isVisible()) { state->parent = 0; wizard->restart(); wizard->show(); }
        wizard->raise(); wizard->activateWindow();
        Q_UNUSED(dialog);
    };
    QObject::connect(open, &QPushButton::clicked, dialog, showWizard);
    QObject::connect(dialog, &InstanceSettingsDialog::updateWizardRequested, dialog, [dialog, showWizard] {
        QTimer::singleShot(0, dialog, showWizard);
    });
    QObject::connect(wizard, &QDialog::finished, dialog, [dialog, state] { if (state->requiresReload) dialog->reject(); });
    wizard->cancelRequested = [owner, state, status] {
        if (state->preparing) {
            owner->m_downloadTaskLedger->setUserCancelInFlight(true);
            const bool requested = owner->m_versionManager->cancelActiveDownload();
            if (!requested) owner->m_downloadTaskLedger->setUserCancelInFlight(false);
            else state->prepCancelled = true;
            status->setText(requested ? QObject::tr("已请求取消核心准备，原实例未改变。")
                : QObject::tr("当前安装阶段暂不能取消，请等待阶段结束。")); return false;
        }
        if (state->downloading) {
            if (state->modInstaller) state->modInstaller->cancel();
            status->setText(QObject::tr("已请求取消下载适配模组；停下后整趟更新不会执行。"));
            return false;
        }
        if (state->executing) { status->setText(QObject::tr("正在提交文件，请等待完成。")); return false; }
        return true;
    };
    QObject::connect(retryFlow, &QPushButton::clicked, wizard, [wizard, state, retryFlow] {
        state->finished = false; state->problem.clear(); state->parent = 0;
        retryFlow->hide(); wizard->restart();
    });
    QObject::connect(wizard, &QWizard::currentIdChanged, wizard, [wizard, start, state](int id) {
        if (id == 3 && !state->preparing && !state->executing && !state->finished)
            QTimer::singleShot(0, wizard, [wizard, start, state] {
                if (!wizard->isVisible() || wizard->currentId() != 3) return;
                start->click();
                if (!state->preparing && !state->executing && !state->finished) {
                    state->finished = true;
                    state->problem = QObject::tr("尚未开始更新。可以返回目标选择，重新检查并确认。");
                }
            });
    });

    // ---------- 新版紧凑页（过渡期）----------
    //
    // 位置：维护页最上面。作用：目标版本 / 加载器 / 副本名这三样在新页面上选，
    // 选完点「兼容性报告…」走现有检查（同一套向导、同一套判据），检查就绪后新页面
    // 的指纹被记下、确认按钮才亮起来。执行仍由向导完成 —— 九类"选了就生效"要等
    // U5 把类别接进复制链路，在那之前**不露**那一块（不给"勾了却不生效"的假象）。
    auto *upgradePage = new InstanceUpgradePage(page);
    layout->insertWidget(0, upgradePage);
    auto pageState = std::make_shared<UpgradePageState::State>();
    pageState->sourceId = sourceId;
    pageState->sourceGameVersion = source.realVersion;
    pageState->sourceLoaderKey = sourceLoader.toLower();
    pageState->sourceLoaderVersion = sourceLoaderVersion;
    pageState->target.instanceName = copyName->text();
    // 来源实例此刻在跑、或被别的作业压着：页面上提前说，不等点确认才报错。
    if (owner->m_launcherManager->isVersionRunning(sourceId)
        || owner->m_versionManager->busyVersionId() == sourceId) {
        pageState->instanceBusy = true;
    }
    // 默认勾选按规范：截图 / 原理图不勾，其余默认勾（盘上没有的那些执行时会自动跳过）。
    for (const UpgradeSelection::Category &category : UpgradeSelection::categories()) {
        if (category.defaultSelected) {
            pageState->selection.state.set(category.key, true);
        }
    }
    upgradePage->setVersionOptions(owner->m_versionManager->availableVersions());
    upgradePage->setLoaderOptions({{QString(), QObject::tr("纯原版")},
                                   {QStringLiteral("fabric"), QStringLiteral("Fabric")},
                                   {QStringLiteral("forge"), QStringLiteral("Forge")},
                                   {QStringLiteral("neoforge"), QStringLiteral("NeoForge")},
                                   {QStringLiteral("quilt"), QStringLiteral("Quilt")},
                                   {QStringLiteral("optifine"), QStringLiteral("OptiFine")}},
                                  {});
    upgradePage->setSelectionVisible(true);
    upgradePage->setHistoryVisible(true);
    upgradePage->setState(*pageState);
    dialog->setProperty("upgradeCopyRoots", UpgradeSelection::selectedRoots(pageState->selection));
    // 只读计划扫一遍来源（只算体积/文件数，不改盘）：九类每一行显示"多少个 / 多大"。
    {
        TransferPlanner::Inputs inputs;
        inputs.sourceDir = AppPaths::versionDir(sourceId);
        inputs.sourceGameVersion = source.realVersion;
        inputs.sourceLoaderName = sourceLoader;
        inputs.sourceLoaderVersion = sourceLoaderVersion;
        struct CategoryStats {
            bool ok = false;
            QHash<QString, QPair<int, qint64>> amounts;
            QHash<QString, int> links;
        };
        // 打开设置只挂接控件；磁盘遍历放到后台，且不捕获页面或控制器。
        auto *watcher = new QFutureWatcher<CategoryStats>(dialog);
        QObject::connect(watcher, &QFutureWatcher<CategoryStats>::finished, dialog,
                         [watcher, upgradePage] {
            const auto stats = watcher->result();
            if (stats.ok) {
                upgradePage->setCategoryAmounts(stats.amounts);
                upgradePage->setCategoryLinks(stats.links);
            }
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([inputs] {
            CategoryStats stats;
            const auto scanned = TransferPlanner::plan(inputs);
            stats.ok = scanned.ok;
            if (!stats.ok) return stats;
            for (const auto &category : UpgradeSelection::categories()) {
                const auto one = UpgradeSelection::amounts(scanned.items, {category.key});
                stats.amounts.insert(category.key, {one.files, one.bytes});
            }
            // 链接统计：一次走完整棵目录，按分类根前缀计数（原来九类各扫一遍=走九趟）。
            QHash<QString, QStringList> rootsByCategory;
            for (const auto &category : UpgradeSelection::categories())
                rootsByCategory.insert(category.key, UpgradeSelection::rootsFor(category.key));
            QDirIterator linkIt(inputs.sourceDir,
                                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                QDirIterator::Subdirectories);
            while (linkIt.hasNext()) {
                linkIt.next();
                const QFileInfo info = linkIt.fileInfo();
                if (!info.isSymLink() && info.junctionTarget().isEmpty()) continue;
                const QString relative = QDir(inputs.sourceDir).relativeFilePath(info.absoluteFilePath());
                for (auto it = rootsByCategory.constBegin(); it != rootsByCategory.constEnd(); ++it) {
                    for (const QString &root : it.value()) {
                        const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
                        if (relative == root || relative.startsWith(prefix)) {
                            ++stats.links[it.key()];
                            break;
                        }
                    }
                }
            }
            return stats;
        }));
    }

    // 检查没结束之前，报告/确认先不能点（每 300ms 跟一次状态，检查一结束就恢复）。
    {
        auto *gate = new QTimer(dialog);
        gate->setInterval(300);
        QObject::connect(gate, &QTimer::timeout, dialog, [dialog, upgradePage, tree] {
            const bool reading = dialog->property("upgradeAssessmentReading").toBool();
            const bool querying = dialog->property("upgradeAssessmentQuerying").toBool();
            const bool noRows = tree->property("upgradeModCount").toInt() == 0;
            upgradePage->setCheckPending(reading || querying || noRows);
        });
        gate->start();
    }
    QObject::connect(upgradePage, &InstanceUpgradePage::targetVersionChanged, dialog,
                     [upgradePage, pageState, game](const QString &version) {
                         UpgradePageState::setTargetVersion(*pageState, version);
                         const int index = game->findData(version);
                         if (index >= 0) game->setCurrentIndex(index);
                         upgradePage->setState(*pageState);
                     });
    QObject::connect(upgradePage, &InstanceUpgradePage::targetLoaderChanged, dialog,
                     [upgradePage, pageState, loader, loaderVersion](const QString &key,
                                                                     const QString &version) {
                         UpgradePageState::setTargetLoader(*pageState, key, version);
                         const int index = loader->findData(key);
                         if (index >= 0) loader->setCurrentIndex(index);
                         if (!version.isEmpty()) {
                             const int versionIndex = loaderVersion->findText(version);
                             if (versionIndex >= 0) loaderVersion->setCurrentIndex(versionIndex);
                         }
                         upgradePage->setState(*pageState);
                     });
    QObject::connect(upgradePage, &InstanceUpgradePage::instanceNameEdited, dialog,
                     [upgradePage, pageState, copyName](const QString &name) {
                         UpgradePageState::setInstanceName(*pageState, name, pageState->sourceId, {});
                         copyName->setText(name);
                         upgradePage->setState(*pageState);
                     });
    QObject::connect(upgradePage, &InstanceUpgradePage::categoryToggled, dialog,
                     [upgradePage, pageState, dialog](const QString &key, bool on) {
                         pageState->selection.state.set(key, on);
                         dialog->setProperty(
                             "upgradeCopyRoots",
                             UpgradeSelection::selectedRoots(pageState->selection));
                         upgradePage->setState(*pageState);
                     });
    QObject::connect(upgradePage, &InstanceUpgradePage::reportRequested, dialog,
                     [dialog] {
                         // 报告一律读评估层的原始行（含分类）；还没检查完就在这里说明，
                         // 不再从界面上那几列文字反推，也不把人扔回旧向导。
                         const auto raw = dialog->property("upgradeModRows").value<QList<QStringList>>();
                         QList<UpgradeReport::Row> rows = UpgradeReport::fromAssessmentRows(raw);
                         if (rows.isEmpty()) {
                             UpgradeReport::Row pending;
                             pending.name = QObject::tr("还没有检查结果");
                             pending.detail = QObject::tr("选好目标版本后会自动检查；这一页只显示检查过的项。");
                             pending.bucket = UpgradeReport::Bucket::Unknown;
                             pending.required = false;
                             rows.append(pending);
                         }
                         UpgradeReportDialog report(dialog);
                         report.setRows(rows);
                         report.exec();
                     });
    QObject::connect(upgradePage, &InstanceUpgradePage::historyRequested, dialog,
                     [dialog, sourceId, owner] {
                         UpgradeHistoryDialog history(dialog);
                         QString problem;
                         history.setEntries(
                             UpgradeHistory::read(AppPaths::versionDir(sourceId), &problem));
                         bool wantsRollback = false;
                         QObject::connect(&history, &UpgradeHistoryDialog::rollbackRequested,
                                          &history, [&history, &wantsRollback] {
                                              wantsRollback = true;
                                              history.accept();
                                          });
                         history.exec();
                         if (wantsRollback) {
                             owner->m_instanceTransferController->rollbackUpgrade(sourceId);
                         }
                     });
    QObject::connect(upgradePage, &InstanceUpgradePage::previewRequested, dialog,
                     [wizard, start, pageState, upgradePage, showWizard, dialog, tree, owner, sourceId] {
                         // 特殊整合包（改成自定义主类/JVM 参数，例如 lwjgl3ify）：升级后启动配置
                         // 可能对不上，先提醒一次。
                         bool customLaunch = false;
                         {
                             QFile versionJson(InstanceFiles::jsonPath(sourceId));
                             if (versionJson.open(QIODevice::ReadOnly)) {
                                 const QString mainClass = QJsonDocument::fromJson(versionJson.readAll())
                                     .object().value(QStringLiteral("mainClass")).toString();
                                 static const QSet<QString> standard{
                                     QStringLiteral("net.minecraft.client.main.Main"),
                                     QStringLiteral("net.minecraft.launchwrapper.Launch"),
                                     QStringLiteral("cpw.mods.bootstraplauncher.BootstrapLauncher"),
                                     QStringLiteral("net.fabricmc.loader.impl.launch.knot.KnotClient")};
                                 customLaunch = !mainClass.isEmpty() && !standard.contains(mainClass);
                             }
                         }
                         if (customLaunch
                             && !InlineMessage::question(
                                 owner->m_ui->overlay(),
                                 QObject::tr("这个整合包带自定义启动配置"),
                                 QObject::tr("升级后，主类、JVM 参数或专用库可能与新版本不兼容，导致实例无法启动。\n\n建议先备份实例。确认继续？"),
                                 QObject::tr("仍要继续"), QObject::tr("取消"), false)) {
                             return;
                         }
                         // 页面自己就是入口：先按页面的闸门校验，再直接走"准备 + 执行"；
                         // 只有那条路当前确实不可点时才退回旧向导。
                         if (UpgradePageState::firstBlocker(*pageState)
                             != UpgradePageState::Blocker::None) {
                             upgradePage->setState(*pageState);
                             return;
                         }
                         wizard->restart();
                         // 有"必须逐项选择"的模组：按最保守的"禁用、保留文件"先填上，
                         // 不把人扔回向导；确认框仍会列出这些项。
                         for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
                             auto *choice = qobject_cast<QComboBox *>(tree->itemWidget(*it, 3));
                             if (choice != nullptr && !choice->currentData().isValid()
                                 && choice->count() > 1) {
                                 choice->setCurrentIndex(1);
                             }
                         }
                         if (!start->isEnabled()) {
                             showWizard();
                             return;
                         }
                         start->click();
                     });

    const auto refreshEnabled = [owner, dialog, state, wizard, start, game, loaderVersion, copyName, mode, status, summary, steps, stopQuery, retryFlow, tree, changes, upgradePage, pageState] {
        const bool selectedGame = game->currentIndex() > 0 && game->currentText() == game->itemText(game->currentIndex());
        const bool ready = selectedGame && (state->loader.isEmpty() || !state->loaderVersion.isEmpty());    if (ready) {
        // 检查结果已经就绪：把这一刻的计划指纹记下来（目标或选择再改，新页面会提示重查）。
        UpgradePageState::markReportChecked(*pageState);
        upgradePage->setState(*pageState);
    }
        const bool named = mode->currentData().toBool() || !copyName->text().trimmed().isEmpty();
        const bool checksReady = !dialog->property("upgradeAssessmentReading").toBool()
            && !dialog->property("upgradeAssessmentQuerying").toBool() && dialog->upgradePendingModCount() == 0;
        const bool correctRoot = AppPaths::minecraftDir() == state->root;
        start->setEnabled(ready && named && checksReady && correctRoot && !state->preparing && !state->executing);
        wizard->setStepReady(0, ready && correctRoot);
        wizard->setStepReady(1, ready && checksReady && correctRoot);
        const QString name = copyName->text().trimmed();
        const bool copyValid = mode->currentData().toBool() || (InstanceFiles::validateDirName(name).isEmpty()
            && !QFileInfo::exists(AppPaths::versionDir(name)));
        wizard->setStepReady(2, ready && checksReady && correctRoot && named && copyValid);
        wizard->setStepReady(3, state->finished);
        wizard->setRunning(state->preparing || state->executing);
        stopQuery->setEnabled(dialog->property("upgradeAssessmentQuerying").toBool());
        const int kept = dialog->upgradeKeptMods().size();
        const int replaced = dialog->upgradeReplaceMods().size();
        const int pending = dialog->upgradePendingModCount();
        QString summaryText = QObject::tr("当前实例：%1\n目标：Minecraft %2 / %3 %4\n\n保留启用：%5　替换适配版：%6　禁用但保留文件：%7　待确认：%8\n\n%9\n\n存档跨版本后可能无法降级；执行前会检查磁盘空间。%11%10")
            .arg(state->source, state->game, state->loader.isEmpty() ? QObject::tr("纯原版") : state->loader, state->loaderVersion)
            .arg(kept).arg(replaced)
            .arg(qMax(0, tree->property("upgradeModCount").toInt() - kept - pending - replaced))
            .arg(pending)
            .arg(mode->currentData().toBool() ? QObject::tr("更新当前实例：完整快照并校验后替换，保留回滚。")
                : QObject::tr("创建更新副本：%1，原实例保持不变。").arg(name))
            .arg(!copyValid ? QObject::tr("\n副本名称无效或已经存在，请修改。") : !correctRoot ? QObject::tr("\n游戏目录已改变，请关闭向导重新进入。") : QString())
            .arg(mode->currentData().toBool()
                ? QObject::tr("更新当前实例：先创建并逐文件校验完整 ZIP 快照（约需 2 倍实例空间），原目录另行保留，可用「回滚上次更新」恢复。")
                : QObject::tr("创建独立副本并逐文件校验，约需一份实例空间；保留源实例，不重复创建 ZIP 快照。"));
        if (replaced > 0) {
            summaryText += QObject::tr("\n\n将下载并替换 %1 个模组；必要前置失败或任一替换失败时整趟回滚，并写出原因报告。")
                               .arg(replaced);
        }
        summary->setText(summaryText);
        const QString changeText = dialog->property("upgradeFileChanges").toStringList().join(QLatin1Char('\n'));
        if (changes->toPlainText() != changeText) changes->setPlainText(changeText);
        if (state->preparing || state->executing || state->finished) {
            QStringList lines;
            const auto appendSteps = [&lines](const QList<Tasks::TaskStep> &steps) {
                for (const auto &step : steps) {
                    // 记号只从 `TextMarks` 取：`✓` 那一格默认界面字体里没有，第一次画出来
                    // 要付一次 277 ms 的字体回退（见 `ui/TextMarks.hpp` 头注释）。
                    const QString mark = step.state == Tasks::TaskStepState::Done ? TextMarks::done()
                        : step.state == Tasks::TaskStepState::Running ? TextMarks::running()
                        : step.state == Tasks::TaskStepState::Failed ? TextMarks::failed() : TextMarks::pending();
                    lines.append(mark + QLatin1Char(' ') + step.name
                                 + (step.note.isEmpty() ? QString() : QStringLiteral(" — ") + step.note));
                }
            };
            for (const auto &task : owner->m_activityCenter->tasks()->all()) {
                if (task.id <= state->firstTask) continue;
                if (task.targetId != state->source && task.targetId != state->core && task.targetId != name) continue;
                lines.append(task.title);
                appendSteps(task.steps);
                // 子任务（准备核心 / 复制 / 快照的执行细节）不进任务中心，但向导这一页
                // 要让人看见它们走到了哪一步。
                for (const auto &child : owner->m_activityCenter->tasks()->childrenOf(task.id)) {
                    appendSteps(child.steps);
                }
                if (!task.detail.isEmpty()) lines.append(task.detail);
            }
            const QString joined = lines.join('\n');
            if (steps->property("snapshot").toString() != joined) {
                steps->setProperty("snapshot", joined); steps->clear(); steps->addItems(lines);
            }
        }
        retryFlow->setVisible(state->finished && !state->requiresReload);
        if (state->finished && !state->problem.isEmpty()) status->setText(state->problem);
        Q_UNUSED(loaderVersion);
        if (!state->preparing && !state->executing && !state->finished)
            status->setText(!state->problem.isEmpty() ? state->problem : ready ? QObject::tr("准备就绪") : QObject::tr("请选择目标 Minecraft 和加载器版本。"));
    };
    auto *poll = new QTimer(dialog); poll->setInterval(300);
    QObject::connect(poll, &QTimer::timeout, dialog, refreshEnabled); poll->start();
    const auto evaluate = [dialog, state, game, loader, loaderVersion, plan, refreshEnabled] {
        if (state->preparing || state->executing) return;
        const QString selectedGame = game->currentIndex() > 0 && game->currentText() == game->itemText(game->currentIndex()) ? game->currentData().toString() : QString();
        const bool changedCompatibility = selectedGame != state->game || loader->currentData().toString() != state->loader;
        if (state->evaluated && !changedCompatibility && loaderVersion->currentData().toString() == state->loaderVersion) {
            refreshEnabled(); return;
        }
        state->evaluated = true; state->game = selectedGame;
        state->loader = loader->currentData().toString(); state->loaderVersion = loaderVersion->currentData().toString();
        state->core.clear();
        state->problem.clear();
        dialog->setProperty("upgradeTargetLoader", state->loader);
        dialog->setProperty("upgradeTargetLoaderVersion", state->loaderVersion);
        if (changedCompatibility) dialog->setProperty("upgradeModOverrides", QVariantMap());
        const QString target = state->game.isEmpty() ? QString() : QStringLiteral("@download:") + state->game;
        dialog->setTransferTargets("upgrade", {{state->game, target}});
        if (!target.isEmpty()) {
            // 那一格**可能不在**（页懒建之后它随页走）：找不到就什么都不做 —— 这一发只是
            // 让旧转移控件跟上新目标，`transferTargetChosen` 那一发才是真正的驱动。
            if (auto *hidden = dialog->findChild<QComboBox *>(
                    InstanceSettingsDialog::transferTargetComboName("upgrade"))) {
                const QSignalBlocker block(hidden);
                hidden->setCurrentIndex(1);
            }
        }
        emit dialog->transferTargetChosen("upgrade", target);
        plan->setPlainText(state->game.isEmpty() ? QObject::tr("选择目标后会检查当前实例的模组。")
            : QObject::tr("Minecraft %1 / %2 %3\n创建副本保留全部用户文件；更新当前实例会先创建并校验完整快照。\n平台发行版与本地声明分别显示，旧模组按下方选择保留或禁用。")
                .arg(state->game, state->loader.isEmpty() ? QObject::tr("纯原版") : state->loader, state->loaderVersion));
        if (auto *old = dialog->findChild<QPushButton *>("instanceUpgradePrepareTarget")) old->hide();
        refreshEnabled();
    };
    const auto requestLoaders = [owner, state, game, loader, loaderVersion, evaluate] {
        if (state->preparing || state->executing) return;
        const QSignalBlocker block(loaderVersion); loaderVersion->clear();
        // 下拉框里不再放一项「正在获取」：框空着，上面画一行加载态（LoadingState，矮宿主画成一行）。
        LoadingState::finish(loaderVersion);
        if (loader->currentData().toString().isEmpty()) loaderVersion->addItem(QObject::tr("无需加载器"), QString());
        else if (game->currentIndex() > 0) {
            LoadingState::begin(loaderVersion, QObject::tr("正在获取版本…"));
            owner->m_versionManager->requestLoaderVersions(game->currentData().toString(), loaderKind(loader->currentData().toString()));
        }
        evaluate();
    };
    QObject::connect(owner->m_versionManager, &VersionManager::loaderVersionsFetched, dialog,
        [state, game, loader, loaderVersion, evaluate, requestLoaders](const QString &version, VersionManager::LoaderKind kind, bool ok,
            const LoaderMeta::LoaderVersionList &entries, const QString &problem) {
            if (state->preparing || state->executing) return;
            if (version != game->currentData().toString() || loader->currentData().toString().isEmpty()
                || kind != loaderKind(loader->currentData().toString())) return;
            const QSignalBlocker block(loaderVersion); loaderVersion->clear();
            loaderVersion->setToolTip(problem);
            if (!ok) {
                // 没取到：框上一行「列表没取到 · 重试」，重试 = 按现在的游戏版本与加载器再问一次。
                LoadingState::fail(loaderVersion, MainWindowUI::loaderVersionFailedText(), requestLoaders);
                evaluate();
                return;
            }
            LoadingState::finish(loaderVersion);
            loaderVersion->addItem(QObject::tr("请选择加载器版本"), QString());
            int recommended = -1;
            for (const auto &entry : entries) {
                loaderVersion->addItem(entry.version + (entry.stable ? QString() : QObject::tr("（测试版）")), entry.version);
                if (recommended < 0 && entry.stable) recommended = loaderVersion->count() - 1;
            }
            if (recommended > 0) loaderVersion->setCurrentIndex(recommended);
            evaluate();
        });
    const auto populate = [owner, state, game, source, requestLoaders] {
        if (state->preparing || state->executing) return;
        const QString previous = game->currentData().toString();
        const QSignalBlocker block(game); game->clear(); game->addItem(QObject::tr("请选择 Minecraft 版本"), QString());
        QStringList versions = owner->m_versionManager->availableVersions();
        if (!source.realVersion.isEmpty() && !versions.contains(source.realVersion)) versions.append(source.realVersion);
        versions.removeDuplicates(); for (const auto &version : versions) game->addItem(version, version);
        const int index = game->findData(previous); game->setCurrentIndex(index >= 0 ? index : 0);
        requestLoaders();
    };
    QObject::connect(owner->m_versionManager, &VersionManager::versionsFetched, dialog, [populate](const QStringList &) { populate(); });
    QObject::connect(game, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, requestLoaders);
    QObject::connect(game, &QComboBox::editTextChanged, dialog, evaluate);
    QObject::connect(loader, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, requestLoaders);
    QObject::connect(loaderVersion, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, evaluate);
    QObject::connect(retry, &QPushButton::clicked, dialog, [owner, populate] { populate(); owner->m_versionManager->fetchAvailableVersions(); });
    QObject::connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, [mode, copyName, refreshEnabled] {
        copyName->setVisible(!mode->currentData().toBool()); refreshEnabled();
    });
    QObject::connect(copyName, &QLineEdit::textChanged, dialog, refreshEnabled);
    // 一次「更新实例」= 一个父任务。准备核心、复制、校验、发布都挂在它底下，
    // 任务中心只出现这一行；子任务的失败/取消只改父任务那一步的记号，终态由这里收。
    const auto beginParentTask = [owner, state, mode, copyName]() -> Tasks::TaskId {
        if (state->parent != 0) return state->parent;
        const bool inPlace = mode->currentData().toBool();
        QList<Tasks::TaskStep> steps{
            {QObject::tr("准备目标核心与加载器"), Tasks::TaskStepState::Running, {}},
            {QObject::tr("下载并校验平台适配模组"), Tasks::TaskStepState::Pending, {}},
            {inPlace ? QObject::tr("准备并校验用户文件副本")
                     : QObject::tr("复制并校验更新副本"),
             Tasks::TaskStepState::Pending, {}},
            {QObject::tr("替换核心、应用模组并发布"), Tasks::TaskStepState::Pending, {}},
            {QObject::tr("完成"), Tasks::TaskStepState::Pending, {}}};
        const QString title = inPlace
            ? QObject::tr("更新实例 %1 → Minecraft %2 / %3")
                  .arg(owner->instanceDisplayName(state->source), state->game,
                       state->loader.isEmpty() ? QObject::tr("纯原版") : state->loader)
            : QObject::tr("创建更新副本 %1 → %2")
                  .arg(owner->instanceDisplayName(state->source), copyName->text().trimmed());
        state->parent = owner->m_activityCenter->begin(
            Tasks::TaskKind::Install, title, state->source, steps);
        Tasks::TaskCapabilities caps;
        caps.canCancel = true;
        caps.canOpenLog = true;
        caps.canOpenTarget = true;
        owner->m_activityCenter->tasks()->setCapabilities(state->parent, caps);
        return state->parent;
    };
    const auto execute = [owner, dialog = QPointer<InstanceSettingsDialog>(dialog), state, mode, copyName, refreshEnabled, status] {
        if (!dialog || AppPaths::minecraftDir() != state->root) return;
        state->executing = true; state->finished = false;
        const QStringList keep = dialog->upgradeKeptMods();
        const QStringList replaceFiles = dialog->upgradeReplaceMods();
        const QVariantMap replaceInfo = dialog->upgradeModReplacements();
        const bool inPlace = mode->currentData().toBool();
        const QString name = copyName->text().trimmed();
        const auto completed = [owner, state, refreshEnabled, status](bool changed, const QString &problem) {
            // 引擎一收尾就把下载暂存收掉：成功时它已经复制进候选目录，失败时更不该再占空间。
            if (!state->modStageDir.isEmpty()) {
                removeTreeWithoutFollowingLinks(state->modStageDir);
                state->modStageDir.clear();
            }
            // 替换已经在候选目录里试过了，但事务在发布前失败：也写一份统一原因报告，
            // 说明是哪一步、哪个文件失败（引擎的 problem 已点名）。
            QString applyFailurePath;
            if (!changed && !problem.isEmpty() && !state->replacementReportId.isEmpty()
                && !state->engineCancelled) {
                OperationReport::Report report;
                report.id = state->replacementReportId;
                report.kind = OperationReport::Kind::ModReplacement;
                report.title = QObject::tr("模组适配替换：%1")
                                   .arg(owner->instanceDisplayName(state->source));
                report.instanceId = state->source;
                report.sourceRoot = AppPaths::versionDir(state->source);
                report.totalFiles = qMax(1, int(state->replacementPreview.size()));
                report.failedFiles = report.totalFiles;
                report.complete = false;
                report.failureReason = problem;
                report.startedAt = QDateTime::currentDateTime();
                report.updatedAt = report.startedAt;
                report.context = QJsonObject{
                    {QStringLiteral("dataRoot"), AppPaths::minecraftDir()},
                    {QStringLiteral("instanceId"), state->source}};
                int index = 0;
                for (const QString &line : state->replacementPreview.isEmpty()
                                               ? QStringList{problem}
                                               : state->replacementPreview) {
                    report.entries.append(OperationReport::Entry{
                        QObject::tr("替换文件 %1").arg(++index), 0,
                        OperationReport::EntryState::Failed, problem + QStringLiteral("\n") + line});
                }
                const QString reportPath = OperationReport::reportPath(
                    AppPaths::minecraftDir(), report.id);
                QString saveProblem;
                if (OperationReport::saveReport(reportPath, report, &saveProblem)) {
                    applyFailurePath = OperationReport::writeFailureText(reportPath, report);
                }
            }
            state->replacementReportId.clear();
            state->replacementPreview.clear();
            state->engineCancelled = false;
            state->modInstaller = nullptr;
            state->downloading = false;
            state->executing = false;
            state->finished = true; state->requiresReload = changed;
            state->problem = !problem.isEmpty() ? problem : changed ? QObject::tr("更新已完成。点击完成返回；实例设置将重新读取新版本。")
                : QObject::tr("本次更新未执行或已取消，原实例没有发布新核心。");
            if (!applyFailurePath.isEmpty()) {
                state->problem += QObject::tr("\n\n替换失败原因报告：\n%1")
                                      .arg(QDir::toNativeSeparators(applyFailurePath));
                owner->appendLog(state->problem);
            }
            if (state->parent != 0) {
                if (changed && problem.isEmpty()) {
                    owner->m_activityCenter->tasks()->setStepState(
                        state->parent, 4, Tasks::TaskStepState::Done);
                    owner->m_activityCenter->finish(state->parent, true, {}, false);
                } else {
                    owner->m_activityCenter->finish(state->parent, false, state->problem, false);
                }
            }
            if (status) status->setText(state->problem);
            // 这一趟的父任务到此为止；下一次「开始更新」要重新开一行（复用旧的终态号会
            // 让新的子任务挂不上去，也会把旧步骤画到新一趟上）。
            state->parent = 0;
            refreshEnabled();
        };
        // 引擎的精确结局（成功 / 用户取消 / 失败）：`completed` 只拿到"要不要重新读设置"，
        // 拿不到"这是取消还是失败" —— 而取消不该亮红点、失败不该被说成成功。
        const auto settled = [owner, state](bool ok, bool cancelled) {
            if (cancelled) state->engineCancelled = true;
            if (state->parent == 0) return;
            if (ok) return;  // 成功由 `completed(true, …)` 那一支收尾。
            if (cancelled) owner->m_activityCenter->tasks()->cancel(state->parent);
        };
        // 父任务的粗粒度步骤 ⇒ 引擎阶段。原地升级与副本模式各自一套。
        const QHash<int, int> parentSteps = inPlace
            ? QHash<int, int>{{int(InstanceUpgrade::Stage::Checking), 2},
                              {int(InstanceUpgrade::Stage::Snapshot), 2},
                              {int(InstanceUpgrade::Stage::VerifySnapshot), 2},
                              {int(InstanceUpgrade::Stage::RetainOriginal), 3},
                              {int(InstanceUpgrade::Stage::Publish), 3},
                              {int(InstanceUpgrade::Stage::Verify), 3}}
            : QHash<int, int>{{int(InstanceUpgrade::Stage::Checking), 2},
                              {int(InstanceUpgrade::Stage::Copy), 2},
                              {int(InstanceUpgrade::Stage::VerifyCopy), 2},
                              {int(InstanceUpgrade::Stage::Publish), 3},
                              {int(InstanceUpgrade::Stage::Verify), 3}};
        const auto runUpgrade = [owner, state, completed, settled, parentSteps, inPlace, name, dialog](
                                    const QList<InstanceUpgrade::ModReplacement> &replacements,
                                    const QStringList &keep) {
            // 文件事务这一段停不得：把父任务的「取消」关掉并写清原因。
            if (state->parent != 0) {
                Tasks::TaskCapabilities caps;
                caps.canCancel = false;
                caps.cancelBlockedReason = QObject::tr(
                    "文件事务已经开始：复制、校验和发布要保证副本/原目录是一整份，中途停下会留下半份；"
                    "这一段不支持取消。");
                owner->m_activityCenter->tasks()->setCapabilities(state->parent, caps);
                owner->m_activityCenter->tasks()->setDetail(state->parent,
                    QObject::tr("正在执行文件事务…"));
            }
            if (inPlace) {
                owner->m_instanceTransferController->upgradeInPlace(
                    state->source, state->core, completed, keep, true, state->parent, parentSteps,
                    settled, replacements);
            } else {
                owner->m_instanceTransferController->upgradeToCopy(
                    state->source, name, state->core, completed, keep, true, state->parent,
                    parentSteps, settled, replacements,
                    dialog->property("upgradeCopyRoots").toStringList());
            }
        };
        // 没有勾选任何适配替换：直接进文件事务。
        if (replaceFiles.isEmpty()) {
            if (state->parent != 0) {
                owner->m_activityCenter->tasks()->setStepState(
                    state->parent, 1, Tasks::TaskStepState::Done,
                    QObject::tr("本次没有勾选替换，按保留/禁用处理。"));
            }
            status->setText(inPlace ? QObject::tr("正在执行完整快照更新，请等待事务结果…")
                                    : QObject::tr("正在复制更新副本并校验，请等待事务结果…"));
            runUpgrade({}, keep);
            return;
        }

        // ---------- 2.1.3 下载并校验平台适配模组 ----------
        // 这一段的产物只落在 `cache/operation-staging/<uuid>`：替换失败/前置失败时
        // 整趟回滚，源实例、原目录和候选目录都还没有被改过。
        const QString reportId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString stageDir = OperationReport::stagingDirectory(AppPaths::minecraftDir(), reportId);
        state->modStageDir = stageDir;
        QList<Presets::ModReference> references;
        QHash<QString, QVariantMap> selectedByProject;
        for (const QString &file : replaceFiles) {
            const QVariantMap info = replaceInfo.value(file).toMap();
            const QString projectKey = info.value(QStringLiteral("projectKey")).toString();
            if (projectKey.isEmpty() || info.value(QStringLiteral("platform")).toString().isEmpty()
                || info.value(QStringLiteral("projectId")).toString().isEmpty()) {
                completed(false, QObject::tr("「%1」的适配信息不完整，未开始替换。").arg(file));
                return;
            }
            Presets::ModReference reference;
            reference.platform = info.value(QStringLiteral("platform")).toString();
            reference.projectId = info.value(QStringLiteral("projectId")).toString();
            reference.name = info.value(QStringLiteral("title")).toString();
            reference.required = true;
            references.append(reference);
            selectedByProject.insert(projectKey, info);
        }
        if (state->parent != 0) {
            state->downloading = true;
            owner->m_activityCenter->tasks()->setStepState(
                state->parent, 1, Tasks::TaskStepState::Running);
            Tasks::TaskCapabilities caps;
            caps.canCancel = true;
            caps.canOpenLog = true;
            caps.canOpenTarget = true;
            owner->m_activityCenter->tasks()->setCapabilities(state->parent, caps);
            owner->m_activityCenter->tasks()->setDetail(
                state->parent, QObject::tr("正在下载并校验平台适配模组…"));
        }
        status->setText(QObject::tr("正在下载并校验 %1 个平台适配模组（必要前置会自动补齐）…")
                            .arg(replaceFiles.size()));
        auto *installer = new Presets::PresetModInstaller(owner, owner->m_resourceStore);
        state->modInstaller = installer;
        if (state->parent != 0) {
            owner->setUpdateFlowCancel(state->parent, [installer] {
                installer->cancel();
                return true;
            }, [] { return true; });
        }
        QObject::connect(installer, &QObject::destroyed, owner, [state] {
            if (state->modInstaller == nullptr) return;
        });
        installer->start(references, stageDir, state->game,
                         state->loader.isEmpty() ? QStringLiteral("vanilla") : state->loader,
                         owner->m_settings->curseForgeApiKey(),
                         [owner, state] {
                             return state->prepCancelled
                                    || (owner->m_launcherManager != nullptr
                                        && owner->m_launcherManager->isVersionRunning(state->source));
                         },
                         [owner, state, dialog, stageDir, reportId, references, selectedByProject,
                          keep, runUpgrade, completed, status, refreshEnabled, inPlace](Presets::PresetModInstaller::Result result) {
                             state->downloading = false;
                             state->modInstaller = nullptr;
                             if (state->parent != 0) owner->clearUpdateFlowCancel(state->parent);
                             // 失败（含取消、必要前置失败、下载/校验失败）：写统一报告 +
                             // 原因报告，整趟回滚（此时盘上还没有任何实例改动）。
                             const auto failReplacement = [&](const QString &why,
                                                              const QStringList &details,
                                                              bool cancelled = false) {
                                 if (!state->modStageDir.isEmpty()) {
                                     removeTreeWithoutFollowingLinks(state->modStageDir);
                                     state->modStageDir.clear();
                                 }
                                 OperationReport::Report report;
                                 report.id = reportId;
                                 report.kind = OperationReport::Kind::ModReplacement;
                                 report.title = QObject::tr("模组适配替换：%1")
                                                    .arg(owner->instanceDisplayName(state->source));
                                 report.instanceId = state->source;
                                 report.targetRoot = stageDir;
                                 report.sourceRoot = AppPaths::versionDir(state->source);
                                 report.stagingDir.clear();
                                 report.conflictPolicy = OperationReport::ConflictPolicy::SkipExisting;
                                 report.totalFiles = qMax(1, references.size());
                                 report.failedFiles = qMax(1, details.size());
                                 report.complete = false;
                                 report.failureReason = why;
                                 report.startedAt = QDateTime::currentDateTime();
                                 report.updatedAt = report.startedAt;
                                 report.context = QJsonObject{
                                     {QStringLiteral("dataRoot"), AppPaths::minecraftDir()},
                                     {QStringLiteral("instanceId"), state->source}};
                                 int index = 0;
                                 for (const QString &line : details.isEmpty() ? QStringList{why} : details) {
                                     report.entries.append(OperationReport::Entry{
                                         QObject::tr("适配替换 %1").arg(++index), 0,
                                         OperationReport::EntryState::Failed, line});
                                 }
                                 const QString path = OperationReport::reportPath(
                                     AppPaths::minecraftDir(), report.id);
                                 QString saveProblem;
                                 QString failurePath;
                                 if (OperationReport::saveReport(path, report, &saveProblem)) {
                                     failurePath = OperationReport::writeFailureText(path, report);
                                 }
                                 const QString text = why
                                     + (failurePath.isEmpty()
                                            ? QString()
                                            : QObject::tr("\n\n原因报告：\n%1").arg(
                                                  QDir::toNativeSeparators(failurePath)));
                                 state->executing = false;
                                 state->finished = true;
                                 state->requiresReload = false;
                                 state->problem = text;
                                 if (state->parent != 0) {
                                     if (cancelled) {
                                         owner->m_activityCenter->tasks()->cancel(state->parent);
                                     } else {
                                         owner->m_activityCenter->tasks()->setStepState(
                                             state->parent, 1, Tasks::TaskStepState::Failed, why);
                                         owner->m_activityCenter->finish(
                                             state->parent, false, text, false);
                                     }
                                     state->parent = 0;
                                 }
                                 if (status) status->setText(text);
                                 owner->appendLog(text);
                                 if (cancelled) {
                                     InlineMessage::information(owner->m_ui->overlay(),
                                                                QObject::tr("已取消模组适配替换"),
                                                                text);
                                 } else {
                                     InlineMessage::critical(owner->m_ui->overlay(),
                                                             QObject::tr("模组适配替换失败"), text);
                                 }
                                 refreshEnabled();
                             };
                             if (result.cancelled) {
                                 failReplacement(QObject::tr("已取消下载适配模组；整趟更新未执行，"
                                                             "源实例没有任何改动。"),
                                                 {}, true);
                                 return;
                             }
                             if (!result.ok) {
                                 QStringList details = result.lines;
                                 if (details.isEmpty()) {
                                     details.append(QObject::tr("必要前置或适配文件缺失/校验失败。"));
                                 }
                                 failReplacement(QObject::tr("模组适配下载或必要前置失败；"
                                                             "整趟更新已回滚，源实例没有任何改动。"),
                                                 details);
                                 return;
                             }
                             // 读回安装器留下的审计表：里面是**真正下好并校验过**的文件。
                             QFile audit(QDir(stageDir).filePath(
                                 QStringLiteral("wtfml.preset-mod-install.json")));
                             if (!audit.open(QIODevice::ReadOnly) || audit.size() > 8 * 1024 * 1024) {
                                 failReplacement(QObject::tr("读不到适配模组的下载清单，整趟更新未执行。"),
                                                 {});
                                 return;
                             }
                             const auto auditObject = QJsonDocument::fromJson(audit.readAll()).object();
                             audit.close();
                             QList<InstanceUpgrade::ModReplacement> replacements;
                             QStringList actualChanges;
                             QSet<QString> seenProjects;
                             for (const QJsonValue &value : auditObject.value(
                                      QStringLiteral("plannedFiles")).toArray()) {
                                 const QJsonObject entry = value.toObject();
                                 const QString project = entry.value(QStringLiteral("project")).toString();
                                 const QString versionId = entry.value(QStringLiteral("versionId")).toString();
                                 const QString filename = entry.value(QStringLiteral("filename")).toString();
                                 const QString sha1 = entry.value(QStringLiteral("sha1")).toString();
                                 // 这张审计表是**从盘上读回来的**（安装器写的，可谁都能改），
                                 // 里面的 `filename` 就当数据看：先过文件名那一套规矩
                                 // （`..` / 冒号 / 保留设备名 / 太长），再拼 `mods/<它>`。
                                 if (!filename.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive)
                                     || sha1.size() != 40
                                     || !InstanceFiles::validateFileName(filename).isEmpty()) {
                                     continue;
                                 }
                                 const QString staged = QDir(stageDir).filePath(
                                     QStringLiteral("mods/") + filename);
                                 if (!QFileInfo(staged).isFile()) continue;
                                 InstanceUpgrade::ModReplacement one;
                                 one.stagedPath = staged;
                                 one.fileName = filename;
                                 one.sha1 = sha1;
                                 one.size = QFileInfo(staged).size();
                                 one.projectKey = project;
                                 one.versionId = versionId;
                                 const auto selected = selectedByProject.constFind(project);
                                 if (selected != selectedByProject.cend()) {
                                     const QVariantMap info = selected.value();
                                     if (versionId != info.value(QStringLiteral("versionId")).toString()) {
                                         failReplacement(
                                             QObject::tr("平台发行版在确认后发生变化（预览 %1，实际 %2）；"
                                                         "更新已完整回滚，请重新检查配置后重试。")
                                                 .arg(info.value(QStringLiteral("versionId")).toString(),
                                                      versionId),
                                             {QObject::tr("项目 %1：预览与下载结果不一致").arg(project)});
                                         return;
                                     }
                                     seenProjects.insert(project);
                                     one.replaces = info.value(QStringLiteral("oldFile")).toString();
                                     one.title = info.value(QStringLiteral("title")).toString();
                                     one.fromVersion = info.value(QStringLiteral("fromVersion")).toString();
                                     one.toVersion = info.value(QStringLiteral("versionNumber")).toString();
                                     one.fromDependency = false;
                                     actualChanges.append(
                                         one.replaces.isEmpty()
                                             ? QObject::tr("新增：%1（%2 %3）")
                                                   .arg(filename, one.title, one.toVersion)
                                             : QObject::tr("替换：%1 → %2（%3 %4 → %5）")
                                                   .arg(one.replaces, filename, one.title,
                                                        one.fromVersion, one.toVersion));
                                 } else {
                                     one.replaces.clear();
                                     one.title = project;
                                     one.fromDependency = true;
                                     actualChanges.append(
                                         QObject::tr("新增必要前置：%1（%2）").arg(filename, project));
                                 }
                                 replacements.append(one);
                             }
                             for (auto it = selectedByProject.cbegin(); it != selectedByProject.cend(); ++it) {
                                 if (!seenProjects.contains(it.key())) {
                                     failReplacement(
                                         QObject::tr("没有从平台拿到「%1」的适配文件；整趟更新已回滚。")
                                             .arg(it.value().value(QStringLiteral("title")).toString()),
                                         {QObject::tr("项目 %1 没有可下载的适配文件").arg(it.key())});
                                     return;
                                 }
                             }
                             if (replacements.isEmpty()) {
                                 failReplacement(QObject::tr("适配下载完成但没有可用的文件；整趟更新未执行。"),
                                                 {});
                                 return;
                             }
                             // ---------- 替换前再列一次实际文件清单 ----------
                             QStringList preview = actualChanges;
                             while (preview.size() > 80) preview.removeLast();
                             const QString extraFiles =
                                 actualChanges.size() > preview.size()
                                     ? QObject::tr("\n…（还有 %1 个文件）")
                                           .arg(int(actualChanges.size() - preview.size()))
                                     : QString();
                             const QString question = QObject::tr(
                                 "将要替换/新增以下 %1 个模组文件；嵌套 JAR 随父 JAR 一起替换。\n\n%2%3\n\n"
                                 "替换失败或必要前置失败时整趟回滚，并写出原因报告。")
                                 .arg(int(replacements.size()))
                                 .arg(preview.join(QLatin1Char('\n')))
                                 .arg(extraFiles);
                             if (!InlineMessage::question(owner->m_ui->overlay(),
                                                          QObject::tr("确认替换模组"),
                                                          question,
                                                          QObject::tr("执行替换"),
                                                          QObject::tr("取消并回滚"), false)) {
                                 failReplacement(QObject::tr("用户取消了替换；整趟更新未执行，"
                                                             "源实例没有任何改动。"),
                                                 {}, true);
                                 return;
                             }
                             state->replacementReportId = reportId;
                             state->replacementPreview = actualChanges;
                             if (state->parent != 0) {
                                 owner->m_activityCenter->tasks()->setStepState(
                                     state->parent, 1, Tasks::TaskStepState::Done,
                                     QObject::tr("已下载并校验 %1 个文件。").arg(replacements.size()));
                                 owner->m_activityCenter->tasks()->setStepState(
                                     state->parent, 2, Tasks::TaskStepState::Running);
                             }
                             status->setText(inPlace
                                 ? QObject::tr("正在执行完整快照更新，请等待事务结果…")
                                 : QObject::tr("正在复制更新副本并校验，请等待事务结果…"));
                             runUpgrade(replacements, keep);
                         });
    };
    QObject::connect(start, &QPushButton::clicked, dialog,
        [owner, dialog, state, mode, copyName, status, selectors, start, refreshEnabled, execute,
         beginParentTask] {
            if (state->preparing || state->executing || dialog->property("upgradeAssessmentReading").toBool()
                || dialog->property("upgradeAssessmentQuerying").toBool() || dialog->upgradePendingModCount() != 0) return;
            state->finished = false; state->problem.clear();
            if (AppPaths::minecraftDir() != state->root || owner->m_downloadInFlight || owner->m_instanceTransferController->copying()
                || owner->installingVersionIds().contains(state->source)
                || owner->m_launcherManager->isVersionRunning(state->source)) {
                InlineMessage::warning(owner->m_ui->overlay(), QObject::tr("暂时不能更新"), QObject::tr("游戏目录已改变，或当前实例/下载/文件操作正在运行。请完成后重试。")); return;
            }
            if (!mode->currentData().toBool()) {
                const QString name = copyName->text().trimmed();
                const QString error = InstanceFiles::validateDirName(name);
                if (!error.isEmpty() || QFileInfo::exists(AppPaths::versionDir(name))) {
                    InlineMessage::warning(owner->m_ui->overlay(), QObject::tr("副本名称不可用"), error.isEmpty() ? QObject::tr("实例名称已存在，请更换名称。") : error); return;
                }
            }
            if (mode->currentData().toBool()
                && state->source.compare(owner->m_versionManager->instanceGameVersion(state->source), Qt::CaseInsensitive) == 0) {
                InlineMessage::warning(owner->m_ui->overlay(), QObject::tr("请创建更新副本"),
                    QObject::tr("当前实例是共享的原版核心，不能直接替换。选择创建更新副本即可保留原版和用户文件。")); return;
            }
            if (!state->core.isEmpty() && QFileInfo::exists(AppPaths::versionDir(state->core) + '/' + state->core + ".json")) {
                beginParentTask();
                owner->m_activityCenter->tasks()->setStepState(state->parent, 0,
                    Tasks::TaskStepState::Done);
                owner->m_activityCenter->tasks()->setStepState(state->parent, 1,
                    Tasks::TaskStepState::Running);
                execute(); return;
            }
            {
                const QStringList unknown = dialog->property("upgradeUnknownMods").toStringList();
                if (!unknown.isEmpty()) {
                    owner->appendLog(QObject::tr("实例更新声明检查：%1 项未知/需确认（必须逐项选择）\n%2")
                                         .arg(unknown.size())
                                         .arg(unknown.join(QLatin1Char('\n'))));
                }
            }
            QString installerJava = owner->m_javaSettings->javaPath();
            if (!state->loader.isEmpty() && !owner->m_installPage->pickInstallerJava(state->game,
                VersionManager::loaderIndexForKind(loaderKind(state->loader)), &installerJava)) return;
            if (!InlineMessage::question(owner->m_ui->overlay(), QObject::tr("确认实例更新"),
                    QObject::tr("更新到 Minecraft %1 / %2 %3。\n\n有目标发行版的模组可替换为适配版（缺的前置会自动补齐），其余按保留或禁用处理；无法确认的需先选处理方式。\n替换失败或前置失败会整趟回滚，并写出报告。\n存档跨版本后可能无法降级；请勿同时运行实例。")
                        .arg(state->game, state->loader.isEmpty() ? QObject::tr("纯原版") : state->loader, state->loaderVersion),
                    QObject::tr("准备并继续"), QObject::tr("取消"), false)) return;
            // Confirmation runs a nested event loop: recheck ownership before any write.
            if (AppPaths::minecraftDir() != state->root || owner->m_downloadInFlight
                || owner->m_instanceTransferController->copying() || owner->installingVersionIds().contains(state->source)
                || owner->m_launcherManager->isVersionRunning(state->source)) return;
            state->core = QStringLiteral("wtfml-update-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            state->prepCancelled = false;
            // 父任务必须在 `setDownloadUiBusy(true)` **之前**建好：那一下会调
            // `DownloadTaskLedgerController::begin()`，它按父任务决定核心准备是挂在这一步底下
            // 还是另起一行。
            beginParentTask();
            owner->m_downloadTaskLedger->setParent(state->parent, 0);
            owner->setUpdateFlowCancel(state->parent, [owner, state] {
                owner->m_downloadTaskLedger->setUserCancelInFlight(true);
                const bool requested = owner->m_versionManager->cancelActiveDownload();
                if (!requested) {
                    owner->m_downloadTaskLedger->setUserCancelInFlight(false);
                    return false;
                }
                state->prepCancelled = true;
                return true;
            }, [owner] {
                return owner->m_versionManager != nullptr
                       && owner->m_versionManager->canCancelDownload();
            });
            state->preparing = true; selectors->setEnabled(false); start->setEnabled(false);
            status->setText(QObject::tr("正在准备目标核心与加载器，完成后自动继续…"));
            auto *vm = owner->m_versionManager;
            vm->setProperty("instanceUpdatePreparedId", state->core);
            owner->m_installOccupancy.setBatch(InstallOccupancy::Batch::Install, state->core);
            owner->m_downloadCompletion->clear();
            owner->markFileOperationBusy(state->source);
            owner->setDownloadUiBusy(true); owner->m_downloadTaskLedger->begin();
            const auto core = state->core;
            QObject::connect(vm, &VersionManager::versionDownloadFinished, owner,
                [owner, guard = QPointer<InstanceSettingsDialog>(dialog), state, core, selectors = QPointer<QWidget>(selectors), status = QPointer<QLabel>(status), refreshEnabled, execute]
                (const QString &id, bool ok, const QString &problem) {
                    if (!state->preparing || id != core) return;
                    state->preparing = false; owner->releaseFileOperationBusy(state->source);
                    owner->m_versionManager->setProperty("instanceUpdatePreparedId", QVariant());
                    const Tasks::TaskId parent = state->parent;
                    state->parent = 0;
                    // 准备这一段的取消入口到此为止：后面是文件事务，由 `execute()` 另摆能力位。
                    owner->clearUpdateFlowCancel(parent);
                    const bool cancelled = state->prepCancelled && !ok;
                    state->prepCancelled = false;
                    const auto failParent = [owner, parent](bool cancel, const QString &detail) {
                        if (parent == 0) return;
                        if (cancel) {
                            owner->m_activityCenter->tasks()->cancel(parent);
                        } else {
                            owner->m_activityCenter->tasks()->setStepState(
                                parent, 0, Tasks::TaskStepState::Failed, detail);
                            owner->m_activityCenter->finish(parent, false, detail, false);
                        }
                    };
                    if (!guard) {
                        failParent(cancelled, problem);
                        return;
                    }
                    if (selectors) selectors->setEnabled(true);
                    if (!ok || AppPaths::minecraftDir() != state->root) {
                        if (!ok) state->core.clear();
                        state->finished = true;
                        state->problem = ok ? QObject::tr("游戏目录已改变，未继续更新。")
                            : cancelled ? QObject::tr("已取消准备核心，原实例没有改变。")
                                        : problem;
                        failParent(cancelled, state->problem);
                        if (status) status->setText(state->problem);
                        refreshEnabled(); return;
                    }
                    state->parent = parent;  // 准备成功：后面的文件事务还挂在同一行上。
                    owner->m_versionManager->clearVersionJsonCache();
                    if (state->parent != 0) {
                        owner->m_activityCenter->tasks()->setStepState(
                            state->parent, 0, Tasks::TaskStepState::Done);
                        owner->m_activityCenter->tasks()->setStepState(
                            state->parent, 1, Tasks::TaskStepState::Running);
                    }
                    if (guard->property("upgradeAssessmentReading").toBool()) {
                        state->finished = true;
                        state->problem = QObject::tr("核心已准备。模组检查完成后，点击开始更新继续。");
                        refreshEnabled(); return;
                    }
                    QTimer::singleShot(0, guard, execute);
                });
            QTimer::singleShot(900000, vm, [vm, core] {
                if (vm->property("instanceUpdatePreparedId").toString() == core) vm->cancelActiveDownload();
            });
            if (state->loader.isEmpty()) vm->installNamedVanilla(state->core, state->game);
            else vm->installNamedLoader(state->core, state->game, loaderKind(state->loader), state->loaderVersion, installerJava);
        });
    populate();
}














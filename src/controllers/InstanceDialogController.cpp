#include "controllers/PresetController.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include "InstanceDialogController.hpp"
#include "UpgradeAssessment.hpp"
#include "InstanceUpdateFlow.hpp"
#include "MainWindowViewData.hpp"
#include "download/ResourceStore.hpp"

#include "MainWindow.hpp"
#include "manage/BackupFiles.hpp"
#include "ConfigFileEditorPage.hpp"
#include "ResourcePackPage.hpp"
#include "InlineMessage.hpp"
#include "ChromeDialog.hpp"
#include "InstanceSettingsDialog.hpp"
#include "InstanceFilesPage.hpp"
#include "MainWindowUI.hpp"
#include "OverlayHost.hpp"
#include "WorldSettingsPage.hpp"
#include "VersionIcons.hpp"
#include "Theme.hpp"
#include <QEventLoop>
#include <QFutureWatcher>
#include "manage/WorldFiles.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTableWidget>
#include <QHeaderView>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QToolTip>
#include <QSet>
#include <atomic>
#include <functional>
#include <memory>

#include "controllers/BackupController.hpp"
#include "controllers/InstanceTransferController.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/InstancePagesController.hpp"
#include "controllers/InstanceEditorController.hpp"
#include "controllers/InstanceFileController.hpp"
#include "controllers/InstanceIconController.hpp"
#include "controllers/LaunchController.hpp"
#include "controllers/LaunchHistoryController.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/FolderManager.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "download/VersionCategory.hpp"
#include "manage/VersionDisplay.hpp"
#include "manage/VersionManager.hpp"
#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "settings/GameWindowSettings.hpp"
#include "settings/LauncherSettings.hpp"
#include "schematic/SchematicInfo.hpp"

namespace {
/// 取中文方块名表。只缓存读到内容的结果：资源还没下载时下一次点击要能重新找。
QJsonObject cachedChineseBlockNames(const QString &assetsRoot, const QString &assetIndexId) {
    static QMutex mutex;
    static QHash<QString, QJsonObject> cache;
    const QString key = assetsRoot + QLatin1Char('|') + assetIndexId;
    {
        const QMutexLocker locker(&mutex);
        if (const auto hit = cache.constFind(key); hit != cache.constEnd()) return *hit;
    }
    const QJsonObject names = SchematicInfo::loadChineseBlockNames(assetsRoot, assetIndexId);
    if (!names.isEmpty()) {
        const QMutexLocker locker(&mutex);
        cache.insert(key, names);
    }
    return names;
}

QString assetIndexIdOf(const QJsonObject &version) {
    return version.value(QStringLiteral("assetIndex")).toObject()
        .value(QStringLiteral("id")).toString(version.value(QStringLiteral("assets")).toString());
}

void showSchematicDetails(OverlayHost *overlay, QWidget *parent, const QString &path,
                          const QString &assetsRoot, const QString &assetIndexId) {
    ChromeDialog detail(parent, true);
    detail.setObjectName(QStringLiteral("schematicDetailsDialog"));
    detail.setWindowTitle(QObject::tr("方块统计"));
    detail.resize(720, 500);
    auto *layout = new QVBoxLayout(detail.body());
    auto *summary = new QLabel(QObject::tr("正在读取原理图方块统计…"), &detail);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *search = new QLineEdit(&detail);
    search->setPlaceholderText(QObject::tr("搜索中文名或方块 ID，例如石头 / minecraft:stone"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *table = new QTableWidget(0, 2, &detail);
    table->setObjectName(QStringLiteral("schematicBlockCounts"));
    table->setHorizontalHeaderLabels({QObject::tr("方块"), QObject::tr("数量")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(&detail);
    auto *copy = buttons->addButton(QObject::tr("复制清单"), QDialogButtonBox::ActionRole);
    auto *close = buttons->addButton(QObject::tr("关闭"), QDialogButtonBox::RejectRole);
    copy->setEnabled(false);
    QObject::connect(close, &QPushButton::clicked, &detail, &QDialog::reject);
    QObject::connect(copy, &QPushButton::clicked, &detail, [table] {
        QStringList lines{table->horizontalHeaderItem(0)->text() + QLatin1Char('\t') +
                          table->horizontalHeaderItem(1)->text()};
        for (int row = 0; row < table->rowCount(); ++row)
            if (!table->isRowHidden(row)) lines.append(table->item(row, 0)->text() + '\t' + table->item(row, 1)->text());
        QApplication::clipboard()->setText(lines.join('\n'));
    });
    QObject::connect(search, &QLineEdit::textChanged, table, [table](const QString &text) {
        for (int row = 0; row < table->rowCount(); ++row)
            table->setRowHidden(row, !table->item(row, 0)->text().contains(text.trimmed(), Qt::CaseInsensitive));
    });
    layout->addWidget(buttons);
    search->setEnabled(false);
    using Details = std::pair<SchematicInfo::Info, QJsonObject>;
    auto *watcher = new QFutureWatcher<Details>(&detail);
    QObject::connect(watcher, &QFutureWatcher<Details>::finished, &detail,
        [watcher, summary, table, search, copy] {
            const auto [info, language] = watcher->result();
            watcher->deleteLater();
            if (!info.ok || !info.countsComplete) {
                summary->setText(QObject::tr("无法给出完整方块清单：%1").arg(
                    info.problem.isEmpty() ? QObject::tr("方块数据未完整解析，不能把未知数量当成零。") : info.problem));
                return;
            }
            summary->setText(QObject::tr("%1 × %2 × %3 / %4 种方块 ID / %5 个非空气方块")
                .arg(info.width).arg(info.height).arg(info.length).arg(info.blocks.size()).arg(info.blockCount));
            table->setRowCount(info.blocks.size());
            for (int row = 0; row < info.blocks.size(); ++row) {
                const auto &block = info.blocks.at(row);
                const QString name = SchematicInfo::chineseBlockName(language, block.id);
                table->setItem(row, 0, new QTableWidgetItem(
                    name.isEmpty() ? block.id : QStringLiteral("%1 (%2)").arg(name, block.id)));
                auto *count = new QTableWidgetItem;
                count->setData(Qt::DisplayRole, QVariant::fromValue(block.count));
                count->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);  // 数字列右对齐，表头跟着它
                table->setItem(row, 1, count);
            }
            table->setSortingEnabled(true);
            table->sortItems(1, Qt::DescendingOrder);
            search->setEnabled(true);
            copy->setEnabled(true);
        });
    watcher->setFuture(QtConcurrent::run([path, assetsRoot, assetIndexId] {
        return Details{SchematicInfo::read(path), cachedChineseBlockNames(assetsRoot, assetIndexId)};
    }));
    overlay->execPrompt(&detail, false);
}
}
#include "replay/ReplayInfo.hpp"
#include "controllers/GameSessionController.hpp"
#include "tools/FileHash.hpp"
#include "util/LogFileReader.hpp"
#include "diagnose/StartupDiagnosis.hpp"
#include "operations/TransferApply.hpp"
#include "operations/OperationReport.hpp"
#include "controllers/FileOperationRecoveryController.hpp"
#include <QUuid>
#include "operations/TransferPlanner.hpp"
#include "download/DependencyInspection.hpp"
#include "manage/InstanceIconLookup.hpp"
#include "tools/FileHash.hpp"
#include "util/SystemMemory.hpp"
#include "util/AppPaths.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QTabWidget>
#include <QTimer>

namespace {

/// 迁移 / 升级副本在后台线程里跑完一趟的结果。
///
/// NOTE: 升级那一档先复制实例、再搬文件、最后校验，三步都在**同一个**后台任务里按顺序
/// 跑完 —— 分两趟的话中间那一小段里目标实例还没造出来，而任务中心已经开始报搬了。
struct MigrationOutcome {
    bool copyFailed = false;
    QString copyError;
    TransferApply::Result apply;
    TransferApply::VerifySummary verify;
};

/// 后台线程写、界面线程每 100 ms 读一次的进度账（锁只护这几个值）。
struct MigrationProgressState {
    QMutex mutex;
    int doneUnits = 0;
    QHash<QString, int> itemStates;  // 块键 → 1 成功 / 0 有失败
};

InstanceSettingsDialog::Override toDialogOverride(
    const JavaSettingsOverride &from, const GameWindowOverride &window = GameWindowOverride()) {
    InstanceSettingsDialog::Override to;
    to.javaPath = from.javaPath;
    to.xmx = from.xmx;
    to.xms = from.xms;
    to.autoMemory = from.autoMemory;
    to.extraJvmArgs = from.extraJvmArgs;
    if (window.mode.has_value()) {
        to.windowMode = gameWindowModeToken(*window.mode);
    }
    to.windowWidth = window.width;
    to.windowHeight = window.height;
    return to;
}

InstanceSettingsDialog::GlobalValues toDialogGlobal(const EffectiveJavaSettings &from) {
    InstanceSettingsDialog::GlobalValues to;
    to.javaPath = from.javaPath;
    to.xmx = from.xmx;
    to.xms = from.xms;
    to.autoMemory = from.autoMemory;
    to.extraJvmArgs = from.extraJvmArgs;
    return to;
}

QList<InstanceSettingsDialog::WindowModeChoice> windowModeChoices() {
    return {{QObject::tr("窗口"),
             gameWindowModeToken(GameWindowMode::Windowed)},
            {QObject::tr("最大化"),
             gameWindowModeToken(GameWindowMode::Maximized)},
            {QObject::tr("全屏"),
             gameWindowModeToken(GameWindowMode::Fullscreen)}};
}

QString windowModeLabel(GameWindowMode mode) {
    switch (mode) {
    case GameWindowMode::Maximized: return QObject::tr("最大化");
    case GameWindowMode::Fullscreen: return QObject::tr("全屏");
    case GameWindowMode::Windowed: break;
    }
    return QObject::tr("窗口");
}

VersionCategory::Channel versionChannelOf(const VersionManager *versions,
                                           const QString &versionId) {
    QString type = versions == nullptr
                       ? QString()
                       : versions->versionMeta(versionId).value(QStringLiteral("type")).toString();
    if (type.isEmpty() && versions != nullptr && QFile::exists(InstanceFiles::jsonPath(versionId))) {
        type = versions->getVersionJson(versionId).value(QStringLiteral("type")).toString();
    }
    return VersionCategory::channelFor(versionId,
                                       type.isEmpty() ? QStringLiteral("release") : type);
}

QSet<QString> knownGroupIdsOf(const InstanceGroupStore *groups) {
    QSet<QString> known;
    for (const InstanceGroupStore::Group &group : groups->groups()) known.insert(group.id);
    return known;
}

QString defaultGroupIdFor(const VersionDisplayParts &part, const VersionManager *versions,
                          const QSet<QString> &known) {
    const auto existing = [&known](InstanceGroupStore::DefaultKind kind) {
        const QString id = InstanceGroupStore::defaultGroupId(kind);
        return known.contains(id) ? id : QString();
    };
    if (!InstancePagesController::loaderName(part, versions).isEmpty())
        return existing(InstanceGroupStore::DefaultKind::Moddable);
    switch (versionChannelOf(versions, part.id)) {
    case VersionCategory::Channel::Release: return existing(InstanceGroupStore::DefaultKind::Vanilla);
    case VersionCategory::Channel::Snapshot: return existing(InstanceGroupStore::DefaultKind::Snapshot);
    case VersionCategory::Channel::Old: return existing(InstanceGroupStore::DefaultKind::Old);
    case VersionCategory::Channel::AprilFools: break;
    }
    return {};
}

QString displayGroupIdOf(const InstanceGroupStore *groups, const QString &key,
                         const VersionDisplayParts &part, const VersionManager *versions,
                         const QSet<QString> &known) {
    QString groupId = groups->groupIdOf(key);
    if (groupId.isEmpty() && !groups->isAssigned(key))
        groupId = defaultGroupIdFor(part, versions, known);
    return groupId;
}

QString homeGroupIdOf(const InstanceGroupStore *groups, const QString &key,
                      const VersionDisplayParts &part, const VersionManager *versions,
                      const QSet<QString> &known) {
    if (!groups->isPinned(key)) return displayGroupIdOf(groups, key, part, versions, known);
    QString origin;
    switch (groups->pinOriginOf(key, &origin)) {
    case InstanceGroupStore::PinOrigin::None: return {};
    case InstanceGroupStore::PinOrigin::Auto: return defaultGroupIdFor(part, versions, known);
    case InstanceGroupStore::PinOrigin::Group: break;
    }
    if (origin == InstanceGroupStore::ungroupedMarker()) return {};
    return known.contains(origin) ? origin : QString();
}

/**
 * @brief 「实例设置」这一趟的会话：那几个**按引用被几十条接线捕获**的局部量。
 *
 * 这一页从 2026-11-02 起走 `OverlayHost::showPage()` —— 摆上去就返回，收页在之后某个
 * 时刻（返回箭头 / Esc / 页上自己的操作 / 被别的页顶掉）。原来那些 lambda 捕获的是
 * 函数里的栈对象，靠 `execPage()` 的嵌套事件循环把它们钉住；不再阻塞之后，函数一返回
 * 栈就没了，捕获的引用全成了野指针。
 *
 * 所以把这几样搬到一个堆上的会话里，**父对象 = 对话框**：对话框 `deleteLater()` 之后
 * 会话跟着它一起没，和这一页同生共死。下面那几十处 `[&dialog]` / `[&targetId]` 一个字
 * 都不用改 —— C++ 里捕获一个引用变量，捕获的是**被引用的那个对象**，于是它们指向的
 * 已经是堆上这几份。
 *
 * `refreshInstanceDirs` / `serverEdits` 必须在这里（而不是还留在栈上）：它们本身也被
 * 别的接线按引用捕获着（改名那一条、服务器那三条），活不过函数返回就不行。
 */
struct InstanceSettingsSession : QObject {
    QString targetId;
    std::function<void(const QString &)> refreshInstanceDirs;
    ServerListController *serverEdits = nullptr;
    bool exportPreset = false;
    ~InstanceSettingsSession() override { delete serverEdits; }
};

} // namespace

namespace {

/// 日志页 / 崩溃报告页单击一行：先摆文件名、时间、大小，行数在后台数完再补。
/// 数行数要读整份文件（`.log.gz` 还要先起 PowerShell 解压），所以只对**当前选中**的那一个做，
/// 并且排队的任务开跑前先看一眼"我还是不是最新那一发"，不是就直接放弃。
void showLogFileInfo(InstanceSettingsDialog &dialog, const QString &key, const QString &path) {
    static std::atomic<int> latest{0};
    // 数好的行数按「路径 + 修改时间 + 大小」记着：来回点同两个 gz 不必反复解压。只在主线程读写。
    static QHash<QString, qint64> counted;
    const QFileInfo info(path);
    if (!info.isFile()) {
        dialog.setLifecycleInfoText(key, path, QCoreApplication::translate("InstanceDialogController", "读不出来：找不到这个文件。"));
        return;
    }
    const auto text = [info](qint64 lines) {
        const QString lineText = lines >= 0 ? QCoreApplication::translate("InstanceDialogController", "%1 行").arg(lines) : QCoreApplication::translate("InstanceDialogController", "行数统计中…");
        return QStringList{info.fileName(),
                           info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                           FileHash::fileSizeText(info.size()), lineText}
            .join(QStringLiteral(" / "));
    };
    const QString toolTip = QDir::toNativeSeparators(path);
    const QString cacheKey = QStringLiteral("%1|%2|%3").arg(
        path, QString::number(info.lastModified().toMSecsSinceEpoch()), QString::number(info.size()));
    const auto hit = counted.constFind(cacheKey);
    if (hit != counted.cend()) {
        dialog.setLifecycleInfoText(key, path, text(hit.value()), toolTip);
        return;
    }
    dialog.setLifecycleInfoText(key, path, text(-1), toolTip);
    const int token = ++latest;
    auto *watcher = new QFutureWatcher<LogFileReader::Counted>(&dialog);
    QObject::connect(watcher, &QFutureWatcher<LogFileReader::Counted>::finished, &dialog,
                     [&dialog, watcher, key, path, cacheKey, text, toolTip] {
        const LogFileReader::Counted result = watcher->result();
        watcher->deleteLater();
        if (result.ok) {
            counted.insert(cacheKey, result.lines);
            dialog.setLifecycleInfoText(key, path, text(result.lines), toolTip);
        } else if (result.lines >= 0) {  // 放弃的那发 lines 记 -1，不报错
            dialog.setLifecycleInfoText(key, path, QCoreApplication::translate("InstanceDialogController", "读不出来：%1").arg(result.problem));
        }
    });
    watcher->setFuture(QtConcurrent::run([path, token] {
        if (token != latest.load()) {
            LogFileReader::Counted skipped;
            skipped.lines = -1;
            return skipped;
        }
        return LogFileReader::countLines(path);
    }));
}

}  // namespace

void InstanceDialogController::applyWorldBatch(InstanceSettingsDialog &dialog, QString instanceId,
                                               const QStringList &paths, bool remove) {
    if (paths.isEmpty()) return;
    const QPointer<InstanceSettingsDialog> liveDialog(&dialog);
    auto *overlay = m_owner->m_ui->overlay();
    const QString title = remove ? tr("删除世界") : tr("复制世界");
    const auto running = [&] {
        if (instanceId.isEmpty() || !m_owner->m_launcherManager->isVersionRunning(instanceId)) return false;
        InlineMessage::warning(overlay, title, tr("游戏正在运行，请先关闭游戏再操作存档。"));
        return true;
    };
    // 删除还要看有没有别的操作占着这个实例（和单个世界删除同一条判据与同一句话）。
    const auto occupied = [&] {
        if (!remove || instanceId.isEmpty() || !m_owner->installingVersionIds().contains(instanceId)) return false;
        InlineMessage::warning(overlay, title,
                               tr("「%1」正在被另一项操作占用（安装、导入、复制、备份或还原），等它结束之后再删这个世界。")
                                   .arg(m_owner->instanceDisplayName(instanceId)));
        return true;
    };
    if (running() || occupied()) return;
    // 只认当前实例 saves 下的直接子目录；列表是旧的（目录已被改名 / 删除）就按新盘面重列，不动盘。
    const QString saves = InstanceFiles::canonicalPath(AppPaths::versionDir(instanceId) + QStringLiteral("/saves"));
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.isDir() || InstanceFiles::canonicalPath(info.absolutePath()) != saves) {
            dialog.refreshLifecycleBatchFiles(QStringLiteral("save"));
            return;
        }
    }
    if (remove) {
        QStringList lines{tr("以下 %n 个世界将移入回收站：", nullptr, static_cast<int>(paths.size())), QString()};
        constexpr qsizetype kListed = 12;
        for (qsizetype index = 0; index < paths.size() && index < kListed; ++index)
            lines.append(QFileInfo(paths.at(index)).fileName());
        if (paths.size() > kListed) lines.append(tr("另有 %1 个世界").arg(paths.size() - kListed));
        if (!InlineMessage::question(overlay, title, lines.join(QLatin1Char('\n')), tr("删除"), tr("取消"),
                                     /*defaultYes=*/false, /*destructive=*/true)) return;
        // 确认框期间游戏可能刚被启动，动盘前再看一遍。
        if (!liveDialog || running() || occupied()) return;
    }
    QString destination;
    if (!remove) {
        destination = QFileDialog::getExistingDirectory(&dialog, title, saves,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!liveDialog || destination.isEmpty() || running()) return;
        destination = QDir::fromNativeSeparators(destination);
    }
    // 整批在同一个嵌套事件循环里做完：这期间界面可操作，批量工具条整排灰着，不会重复提交。
    struct Outcome {
        qsizetype done = 0;
        QString problem;
    };
    Outcome outcome;
    QEventLoop loop;
    if (remove) {
        // 逐个移入回收站，遇到失败即停（失败原因控制器已经弹过），没处理的世界保持选中。
        auto next = std::make_shared<std::function<void()>>();
        *next = [this, &loop, &outcome, instanceId, paths, next] {
            if (outcome.done >= paths.size()) {
                loop.quit();
                return;
            }
            m_owner->m_instanceTransferController->deleteWorldInBackground(
                instanceId, paths.at(outcome.done), [&loop, &outcome, next](bool success, const QString &error) {
                    if (!success) {
                        outcome.problem = error;
                        loop.quit();
                        return;
                    }
                    ++outcome.done;
                    QTimer::singleShot(0, &loop, [next] { (*next)(); });
                });
        };
        // 第一发也排进事件循环：deleteWorldInBackground 的早退路径会同步回调，
        // 回调里的 loop.quit() 若发生在 exec() 之前就不生效，exec() 永不返回。
        QTimer::singleShot(0, &loop, [next] { (*next)(); });
        loop.exec();
        *next = nullptr;  // 断开 next 对自己的引用环
    } else {
        QFutureWatcher<Outcome> watcher;
        connect(&watcher, &QFutureWatcher<Outcome>::finished, &loop, &QEventLoop::quit);
        watcher.setFuture(QtConcurrent::run([paths, destination] {
            Outcome result;
            for (const QString &path : paths) {
                QString error;
                if (!WorldFiles::copyWorldInto(path, destination, QFileInfo(path).fileName(), &error)) {
                    result.problem = tr("无法复制「%1」：%2").arg(QFileInfo(path).fileName(), error);
                    break;
                }
                ++result.done;
            }
            return result;
        }));
        loop.exec();
        outcome = watcher.result();
    }
    if (!liveDialog) return;
    liveDialog->refreshLifecycleBatchFiles(QStringLiteral("save"));
    const QString summary = tr("%1：完成 %2 / %3 项。").arg(title).arg(outcome.done).arg(paths.size());
    m_owner->appendLog(summary);
    if (outcome.problem.isEmpty())
        InlineMessage::information(overlay, title, summary);
    else if (!remove)  // 删除失败时控制器已经弹过原因
        InlineMessage::warning(overlay, title, summary + QStringLiteral("\n\n") + outcome.problem);
}

InstanceDialogController::InstanceDialogController(MainWindow *owner)
    : QObject(owner), m_owner(owner) {}

void InstanceDialogController::open(const QString &versionId, const QString &pageKey) {
    if (m_owner->m_fileOperationOccupancy.contains(versionId)) {
        // 模组迁移和「迁移 / 升级副本」都会往这个实例的目录里写文件（后者连整棵目录
        // 都在拷）。这期间开设置页去改同一个目录，两边都不知道对方在写什么。
        InlineMessage::information(m_owner->m_ui->overlay(), tr("正在搬文件"),
                                   tr("这个实例正在后台复制或移动文件，操作完成后再修改实例。"));
        return;
    }
    // 主页快捷入口留在主页上，关闭设置后原来的实例选择状态仍在。
    // 从下载等其它入口返回设置时，仍归属实例管理页。
    const QTabWidget *entryTabs = m_owner->m_ui->getTabWidget();
    const int settingsTab = entryTabs != nullptr
                                   && entryTabs->currentIndex() == MainWindowUI::homeTabIndex()
                               ? MainWindowUI::homeTabIndex() : MainWindowUI::instancesTabIndex();

    // 已经写了的那部分覆盖从 meta 里读回来当起点，「当前全局是哪个值」由 global 提供。
    // 读归读，**这里一个字节都不写** —— 写盘只发生在下面那两条接线里（ 起这一页
    // 没有「确定」，改一格就写一格）。
    // NOTE: 窗口那一组覆盖读的是**同一份 meta**（一次读出来、两处用），不然这一页打开一次
    // 就要把同一个文件解析两遍。
    const QJsonObject instanceMeta = m_owner->m_versionManager->versionMeta(versionId);
    const JavaSettingsOverride current = javaOverrideFromJson(instanceMeta);
    // 第二个参数是**目录名**（改名要改的就是它），第一个是显示名（页眉上那行字）——
    // 两个值可以不一样（自定义名、拼进去的加载器名），页面上是分开的两行。
    InstanceSettingsDialog::GlobalValues global = toDialogGlobal(m_owner->m_javaSettings->effective());
    // 全局那一档窗口（模式 + 分辨率）：窗口那两行的「跟随全局（当前 …）」写的就是这一份。
    const GameWindowSettings globalWindow = m_owner->m_settings->gameWindowSettings();
    global.windowMode = gameWindowModeToken(globalWindow.mode);
    global.windowModeLabel = windowModeLabel(globalWindow.mode);
    global.windowWidth = globalWindow.width;
    global.windowHeight = globalWindow.height;
    // 「最大内存」那一格能拖到哪儿 = **设置页那一格能拖到哪儿**，取的是设置页那个数字框现在的
    // 上限（开窗时 applyMemoryLimits 已经按这台机器的物理内存收过口，见 refreshMemoryUi）。
    // 不在这儿另读一次系统内存，是为了两页**逐字同一个数**：用户刚在设置页看见"最多 16256"，
    // 切到实例页又是另一个上限的话，两页各说各的。
    global.memoryCapMb = m_owner->m_ui->getMemoryMaxSpin()->maximum();
    // 这一页**不阻塞开**（`showPage()` 摆上就返回），而整页几十条接线都按引用捕获下面
    // 这几个局部量 —— 它们得活到收页之后，所以放在堆上的会话里（见
    // `InstanceSettingsSession` 那段）。会话的父对象是对话框：这一页被 `deleteLater()`
    // 收掉时它跟着一起没，一个字节都不用另找时机释放。
    auto *session = new InstanceSettingsSession;
    session->targetId = versionId;
    QElapsedTimer settingsClock;
    settingsClock.start();
    auto *dialogPtr = new InstanceSettingsDialog(
        m_owner->instanceDisplayName(versionId), versionId,
        toDialogOverride(current, gameWindowOverrideFromJson(instanceMeta)),
        global, m_owner->m_versionManager->versionJvmArgs(versionId),
        !m_owner->m_versionManager->skipJavaVersionWarning(versionId),
        windowModeChoices(), m_owner);
    qInfo() << "[UI timing] settings dialog ctor" << settingsClock.elapsed() << "ms";
    session->setParent(dialogPtr);
    InstanceSettingsDialog &dialog = *dialogPtr;
    connect(dialogPtr, &InstanceSettingsDialog::javaRuntimeChoicesRequested, dialogPtr, [dialogPtr] {
        const QString javaDir = AppPaths::javaDir();
        auto *watcher = new QFutureWatcher<QMap<int, QStringList>>(dialogPtr);
        connect(watcher, &QFutureWatcher<QMap<int, QStringList>>::finished, dialogPtr, [dialogPtr, watcher] {
            dialogPtr->setJavaRuntimeChoices(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([javaDir] {
            return GlobalJavaSettings::scanAvailableJavaPaths(javaDir);
        }));
    });
    dialog.setLastLaunch(LaunchHistory::lastStartedAt(m_owner->m_launchHistory->records(), versionId));
    const auto playTime = m_owner->m_launchHistory->completedPlayTime(
        m_owner->m_versionManager->instanceId(versionId));
    dialog.setPlayTime(LaunchHistory::playTimeText(playTime.totalMs),
                       LaunchHistory::playTimeText(playTime.lastMs));
    connect(dialogPtr, &InstanceSettingsDialog::resetPlayTimeRequested, dialogPtr,
            [this, dialogPtr, versionId]() {
                const auto answer = InlineMessage::question(
                    m_owner->m_ui->overlay(), tr("重置游玩时间"),
                    tr("将清掉这个实例已记录的累计游玩时间和上次游玩时长，且不能恢复。确认重置？"),
                    tr("重置"), tr("取消"), false);
                if (!answer) {
                    return;
                }
                const QString stableId = m_owner->m_versionManager->instanceId(versionId);
                if (!m_owner->m_launchHistory->resetCompletedPlayTime(stableId)) {
                    return;
                }
                dialogPtr->setPlayTime(LaunchHistory::playTimeText(0), QString());
            });
    connect(&dialog, &InstanceSettingsDialog::launchInstanceRequested, m_owner,
            [this, versionId]() {
                QComboBox *combo = m_owner->m_ui->getInstalledVersionCombo();
                const int row = combo == nullptr ? -1 : combo->findData(versionId);
                if (row >= 0) {
                    combo->setCurrentIndex(row);
                    m_owner->onStartButtonClicked();
                }
            });
    // ---------- 概览栏那四行的初值----------
    //
    // 三张图标**由这里解好递进去**：ui/ 不做"这个实例默认该长什么样"那个判断（它要读版本
    // json 的 type 和加载器名，是业务层的事），也够不着实例目录下那份 icon.png 的名字
    // （AppPaths 在 src/ 里）。这一层解好、那一层只管画。
    const VersionDisplayParts parts = m_owner->m_versionManager->displayParts(versionId);
    const int previewSize = InstanceSettingsDialog::iconPreviewSize();
    QPixmap localIcon;
    if (parts.icon == InstanceSettingsDialog::localIconValue()) {
        localIcon = VersionIcons::fileMark(AppPaths::versionIconPath(versionId), previewSize);
    }
    VersionDisplayParts defaultCover = parts;
    defaultCover.icon.clear();
    dialog.setIcon(parts.icon, MainWindowViewData::instanceCoverMark(defaultCover, previewSize), localIcon);
    {
        const QString game = parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
        // 加载器名走 `InstancePagesController::loaderName()`，**不直接读 `parts.loaderName`**：
        // 那一格只有我们自己的安装器会写，PCL / HMCL 合并出来的实例（没有 inheritsFrom、
        // 加载器只写在版本 json 的 `libraries` 里）在那儿是空的。读它的话，同一个实例在
        // 主页 / 管理列表 / 上面那张图标上都写着「Fabric 0.16.14」，唯独这一行说「原版」。
        QString loaderVersion;
        const QString loaderName = InstancePagesController::loaderName(
            parts, m_owner->m_versionManager, &loaderVersion);
        const QString loader = loaderName.isEmpty()
                                   ? tr("原版")
                                   : (loaderName + QLatin1Char(' ') + loaderVersion).trimmed();
        dialog.setHeaderMeta(loader + QStringLiteral(" · Minecraft ") + game);
    }

    // 「分组…」那一格：候选项是**现在所有的组**（含空组 —— 「往空组里放第一个实例」正是用户
    // 会干的事，那是它唯一的入口之一），当前值是那个实例的**老家**。
    //
    // NOTE: 走 homeGroupIdOf() 而**不是** displayGroupIdOf()。对没置顶的实例两者逐字相同；
    // 对置顶着的那一个，displayGroupIdOf() 答的是置顶组（盘上 members 指着它），而
    // 用户在这一格里说的永远是"它本来属于哪儿" —— 摆上置顶组的话，用户点开看见
    // 「置顶」被灰着、还以为是这一格出了问题，同时"想给它换个老家"这件事**做不了**
    // （选了别的组又会被 applyInstanceSettings 当成"老家改了"，而它本来就是"老家改了"，
    // 只是起点显示错了）。
    const QSet<QString> knownGroupIds = knownGroupIdsOf(m_owner->m_groupStore);
    QList<QPair<QString, QString>> groupChoices;
    for (const InstanceGroupStore::Group &group : m_owner->m_groupStore->groups()) {
        groupChoices.append(qMakePair(group.name, group.id));
    }
    const QString key = m_owner->instanceKey(versionId);
    const QString groupAtOpen = homeGroupIdOf(m_owner->m_groupStore, key, parts, m_owner->m_versionManager,
                                              knownGroupIds);
    dialog.setGroupChoices(groupChoices, groupAtOpen);

    // 置顶那一行：判据是"它此刻在不在置顶组里"（isPinned()，不是"老家等于置顶组"——
    // 两边都显示之后那两句话不再等值：老家的的确确可以和置顶组是同一个组，而那时它
    // 也照样是置顶着的）。没有置顶组（用户把它删了）时恒假，而且那一格会灰掉
    // （见 setPinned()）。
    const QString pinnedGroupId = m_owner->m_groupStore->pinnedGroupId();
    dialog.setPinned(m_owner->m_groupStore->isPinned(key), pinnedGroupId);

    // ---------- 「文件管理」那八页的目录与可用性----------
    //
    // **所有路径都由这一层算好递进去**：那几个目录名是 MC 的约定，出处是
    // ResourceCatalog::folderForKind()（见 m_owner->instanceFileDirs()），ui/ 不抄第二份。
    //
    // 收成一个 lambda 而不是直接写四句：**改完名要整份重算**（下面那条改名线里调它）。
    // 改名会把整个实例换到另一个目录，那八条路径、那两页的可用性、"这九颗按钮开的是
    // 哪儿"全都跟着换 —— 不重算的话，用户改完名点「模组」，看的是那个**已经不在了**
    // 的老目录；再点一下「打开文件夹」，`mkpath()` 会把老目录重新建出来（一个只有几个
    // 空文件夹的空壳实例）。这是改名和"实时写"凑到一起才会露出来的一类坑。
    //
    // NOTE: 它按**引用**收 targetId（而不是收一个 QString）：改名之后那个值变了，重算
    // 必须看新值。下面那些 lambda 也是同一个理由全都按引用捕获它。
    // 这一位在会话里（不是栈上）：收页之后那些接线还得读它，见 InstanceSettingsSession。
    QString &targetId = session->targetId;
    session->refreshInstanceDirs = [this, &dialog, &targetId](const QString &id) {
        // 实例可能在设置页开着的时候被删掉（管理页的行尾删除、另一个入口的删除）。
        // 这时别再拿旧 id 去算目录并 set 回对话框 —— 更不能让后面的「打开目录」用
        // `mkpath()` 把它重新建出来。直接把这一页收掉。
        if (!QFileInfo(AppPaths::versionDir(id)).isDir()) {
            m_owner->m_ui->overlay()->dismiss();
            return;
        }
        QElapsedTimer dirClock;
        dirClock.start();
        // 这一段"算"的活全在 `InstancePagesController` 里（哪几页用得上、
        // 那几页指向哪个目录、有结构的存档是哪几个）。这一层剩下的**只有摆**：
        // 把那一份事实挨个 `set` 到对话框上。
        const QHash<QString, QString> fileDirs = InstancePagesController::fileDirs(id);
        const qint64 fileDirsMs = dirClock.restart();
        dialog.setInstanceDir(AppPaths::versionDir(id));
        QHash<QString, QString> shortcutDirs = fileDirs;
        shortcutDirs.insert(QStringLiteral("game"), AppPaths::minecraftDir());
        shortcutDirs.insert(QStringLiteral("replay"), AppPaths::replayRecordingsDir());
        shortcutDirs.insert(QStringLiteral("crash"), AppPaths::crashReportsDir());
        dialog.setFileDirs(shortcutDirs);
        // 那排按钮分两段的判据（哪几路"现在真有东西"）—— 理由与逐条判据见
        // InstancePagesController::commonFolderKeys()。**必须紧跟着 setFileDirs()**：它按
        // 这一份路径算。
        dialog.setFolderPresentKeys(InstancePagesController::commonFolderKeys(
            id, shortcutDirs, InstanceSettingsDialog::shortcutPathKeys()));
        const qint64 presentKeysMs = dirClock.restart();
        // 左栏那一列是**按实例现算**的：这几页要"用得上"才列出来。四个判据
        // 全在业务层 —— loadername / mods 目录 / 装没装那个 mod / 游戏版本够不够新，ui/
        // 一样都够不着。
        //
        // 四个 setter 的**默认都是"可用"**（直接 new 出来的对话框七条全列）：忘了调哪一个，
        // 那个实例上就会多列出一页来 —— 而多一页是"用户点进去看见一句用不上"，不是静默
        // 的错。所以宁可这样默认，也不要反过来（默认不可用 = 忘了调就把一整页藏了）。
        //
        // NOTE: 「模组」的判据是 `modsUsable()`（有模组加载器 **或** mods/ 里有 jar）
        // ⇒ **OptiFine 也算**，装 OptiFine 的实例会多一条「模组」。这是有意的：
        // OptiFine 从 1.13 起就是塞进 mods/ 的，那一页在那样的实例上真的用得上。
        // 判据**不是** `displayParts().loaderName` 非空 —— 那一格
        // 只有我们自己的安装器会写，用户盘上 PCL 装的实例会被误判成"没有模组加载器"。
        // 判据与理由见那个函数那段说明。
        const InstancePagesController::Usability usable =
            InstancePagesController::usability(m_owner->m_versionManager, id);
        const qint64 usabilityMs = dirClock.restart();
        qInfo() << "[UI timing] refreshInstanceDirs" << id << "fileDirs" << fileDirsMs
                << "keys" << presentKeysMs << "usability" << usabilityMs;
        dialog.setModUsable(usable.mods);
        dialog.setShaderUsable(usable.shader);
        dialog.setSchematicUsable(usable.schematic);
        if (InstanceFilesPage *schematics = dialog.filesPage(QStringLiteral("schematic"))) {
            // 原理图与原版结构已拆为独立页面；原理图只列实例级 schematics 目录。
            schematics->setStructureSources({});
            // 在列表里选中一份投影 ⇒ 底下那一格给出
            // 元数据、方块统计和那句版本兼容提示。**算的活全在 `SchematicInfo`**（读
            // gzip NBT、认 Litematica 的位打包都是 src/ 的事，ui/ 够不着）；这一页只
            // 负责说"选中的换了"，然后把算好的话摆上去。
            //
            // NOTE: 连接只挂一次 —— 这个 lambda 改名时还会再跑一遍，再 connect 一次
            // 就是同一份盘读两遍（目标版本也会跟着 targetId 换，所以捕获引用）。
            schematics->setInfoPanelEnabled(true);
            if (!schematics->property("wtfmlSchematicInfoWired").toBool()) {
                schematics->setProperty("wtfmlSchematicInfoWired", true);
                connect(schematics, &InstanceFilesPage::infoDetailsRequested, schematics,
                    [this, &targetId](const QString &path) {
                        const QString indexId = assetIndexIdOf(m_owner->m_versionManager->getVersionJson(targetId));
                        showSchematicDetails(m_owner->m_ui->overlay(), m_owner, path, AppPaths::assetsDir(), indexId);
                    });
                connect(
                    schematics, &InstanceFilesPage::infoRequested, schematics,
                    [this, schematics, &targetId](const QString &path) {
                        // 和这个实例**现在**的真实版本比（改名换 id 之后也跟着走）。
                        const VersionDisplayParts parts =
                            m_owner->m_versionManager->displayParts(targetId);
                        const QString targetVersion =
                            parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
                        const QString indexId = assetIndexIdOf(m_owner->m_versionManager->getVersionJson(targetId));
                        using Read = std::pair<SchematicInfo::Info, QJsonObject>;
                        auto *reader = new QFutureWatcher<Read>(schematics);
                        connect(reader, &QFutureWatcher<Read>::finished, schematics,
                            [reader, schematics, path, targetVersion] {
                        const auto [info, language] = reader->result();
                        reader->deleteLater();
                        if (!info.ok) {
                            // 读失败也占着这一格（用户刚点的那一行，总得听到一句为什么）。
                            schematics->setInfoText(
                                path, tr("读取失败：%1").arg(info.problem));
                            return;
                        }
                        schematics->setInfoText(path,
                                                SchematicInfo::summaryText(info, targetVersion),
                                                SchematicInfo::topBlocksText(info, language));
                            });
                        reader->setFuture(QtConcurrent::run([path, indexId] {
                            return Read{SchematicInfo::read(path),
                                        cachedChineseBlockNames(AppPaths::assetsDir(), indexId)};
                        }));
                    });
            }
        }
        dialog.setDatapackUsable(usable.datapack);
        // 那两颗「加入…」（**临时占位**，见 QuickPlay）。
        // **露不露** = 这个版本认不认那两条参数 —— 判据只有 `m_owner->instanceSupportsQuickPlay()`
        // 一处（和 `m_owner->startQuickPlay()` 的第一道闸是同一句话）。空串 = 那一颗整颗收起来。
        // NOTE: 收在这里（而不是摆在各页自己的构造里）的理由和上面那四个 setter 逐字
        // 相同：这一层才知道"这个实例是哪个版本"，而改名会让 id 换一个。
        // NOTE: **服务器那一页**的那颗要等 `m_owner->loadServerList()` 之后的选中状态，所以它自己
        // 那两句摆在最下面（见 `updateServerSelectionActions()`）。
        dialog.setServerJoinText(
            m_owner->instanceSupportsQuickPlay(id, QuickPlay::Kind::Multiplayer)
                ? InstanceSettingsDialog::serverJoinText()
                : QString(),
            InstanceSettingsDialog::serverJoinTip());
        if (InstanceFilesPage *saves = dialog.filesPage(QStringLiteral("save"))) {
            saves->setJoinText(m_owner->instanceSupportsQuickPlay(id, QuickPlay::Kind::Singleplayer)
                                   ? InstanceFilesPage::joinText()
                                   : QString(),
                               InstanceFilesPage::joinTip());
        }
        m_owner->loadServerList(&dialog, id);
        // 那几页里那些资源文件（模组 / 资源包 / 数据包 / 光影包）的图标：
        // 按文件内容反查，**在后台线程上跑**，回来了一个一个换上去（见那一支）。
        // 摆在这儿而不是摆在开窗那一刻：改名会让整个实例换一个目录，那批路径跟着全换，
        // 图标也得按新路径重认一遍（旧的那一页那时还画着老目录那些条的图）。
        m_owner->requestInstanceFileIcons(&dialog, id);
        m_owner->pushInstanceFileUpdates(&dialog, id);
    };
    // 下面那条改名接线**按引用**收它（改名之后要整份重算）—— 所以引用的是会话里那一份，
    // 不是这个栈上的引用变量本身。C++ 捕获引用变量时捕获的本来就是被引用的对象。
    std::function<void(const QString &)> &refreshInstanceDirs = session->refreshInstanceDirs;
    refreshInstanceDirs(targetId);

    connect(&dialog, &InstanceSettingsDialog::lifecycleBatchRequested, &dialog,
            [this, &dialog, &targetId](const QString &key, const QStringList &paths,
                                      InstanceSettingsDialog::LifecycleBatchAction requested) {
        using Request = InstanceSettingsDialog::LifecycleBatchAction;
        using Action = InstanceFileController::LifecycleAction;
        if (paths.isEmpty()) return;
        if (key == QStringLiteral("save")) {
            applyWorldBatch(dialog, targetId, paths, requested == Request::Remove);
            return;
        }
        for (const QString &path : paths)
            if (!InstanceSettingsDialog::isLifecycleBatchFile(key, path)) return;

        const QPointer<InstanceSettingsDialog> liveDialog(&dialog);
        const QString sourceId = targetId;
        const QString dataRoot = AppPaths::minecraftDir();
        const QString replayRoot = AppPaths::replayRecordingsDir();
        const bool replay = key == QStringLiteral("replay");
        // 崩溃报告页的目录不在 fileDirs() 里（见 refreshInstanceDirs()），单独认。
        // 结构页列的是各存档 generated 下的文件，来源根目录是 saves。
        const bool structure = key == QStringLiteral("structure");
        const QString root = replay ? replayRoot
                             : key == QStringLiteral("crash") ? AppPaths::crashReportsDir()
                                                              : InstancePagesController::fileDirs(sourceId).value(
                                                                    structure ? QStringLiteral("save") : key);
        const Action action = requested == Request::Copy ? Action::Copy
                            : requested == Request::Move ? Action::Move : Action::Remove;
        const QString title = action == Action::Copy ? tr("复制文件")
                            : action == Action::Move ? tr("移动文件") : tr("删除文件");
        auto *overlay = m_owner->m_ui->overlay();
        QString destination;
        if (action != Action::Remove) {
            destination = QFileDialog::getExistingDirectory(&dialog, title, root,
                QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
            if (!liveDialog || destination.isEmpty()) return;
            destination = QDir::fromNativeSeparators(destination);
        }
        if (action == Action::Remove || action == Action::Move) {
            // 删除这一问要把**文件名**摆出来：用户在批量列表里勾了几条，确认框里得能逐条
            // 核对删的是哪几份（只说"目录里 N 个文件"时，同名 / 名字相近的一勾错也看不出来）。
            // 选得多时只列前 5 个，剩下的用一行数量收口，免得确认框被文件名顶出屏幕。
            QString locations;
            if (action == Action::Remove) {
                QStringList names;
                names.reserve(paths.size());
                for (const QString &path : paths) names.append(QFileInfo(path).fileName());
                if (names.size() == 1) {
                    locations = tr("将「%1」移入回收站？").arg(names.first());
                } else {
                    constexpr qsizetype kListed = 5;
                    QStringList lines{tr("将以下 %1 个文件移入回收站？").arg(names.size()), QString()};
                    for (qsizetype index = 0; index < names.size() && index < kListed; ++index)
                        lines.append(names.at(index));
                    if (names.size() > kListed)
                        lines.append(tr("另有 %1 个文件").arg(names.size() - kListed));
                    locations = lines.join(QLatin1Char('\n'));
                }
            } else {
                locations = tr("将「%1」中的 %2 个所选文件移动到「%3」？\n仅支持同卷移动，不覆盖同名文件。")
                                .arg(QDir::toNativeSeparators(root)).arg(paths.size())
                                .arg(QDir::toNativeSeparators(destination));
            }
            const QString question = replay ? tr("回放目录由当前游戏目录内的实例共用。\n\n") + locations : locations;
            if (!InlineMessage::question(overlay, title, question,
                    action == Action::Remove ? tr("删除") : tr("移动"), tr("取消"), false, true)) return;
        }
        if (!liveDialog) return;
        if (targetId != sourceId || AppPaths::minecraftDir() != dataRoot) {
            InlineMessage::warning(overlay, title, tr("当前实例或游戏目录已切换，未改动文件。"));
            return;
        }
        if ((replay || InstanceFiles::pathIsSameOrInside(replayRoot, destination))
            && (!m_owner->runningVersionIds().isEmpty() || m_owner->m_launchController->isStarting())) {
            InlineMessage::warning(overlay, title, tr("回放目录由多个实例共用，请退出正在运行的游戏后再操作。"));
            return;
        }
        const auto result = m_owner->m_instanceFileController->applyLifecycleBatch(action, root, paths, destination, structure);
        if (!liveDialog) return;
        liveDialog->refreshLifecycleBatchFiles(key);
        const QString summary = tr("%1：完成 %2 / %3 项。").arg(title).arg(result.done).arg(paths.size());
        m_owner->appendLog(summary);
        if (result.problem.isEmpty())
            InlineMessage::information(overlay, title, summary);
        else
            InlineMessage::warning(overlay, title, summary + QStringLiteral("\n\n") + result.problem);
    });

    // 「Replay」和「结构」那两页，选中一行之后底下
    // 那一格要说这一条是什么（录了多久 / 多大的结构 / 和这个实例的版本兼不兼容）。
    // 和原理图那一页同一条分层：读盘与统计在 src/（`ReplayInfo` / `SchematicInfo`），
    // 这一层只把"用户选的是哪一条"和"这个实例是哪个版本"接起来；ui/ 只管摆字。
    //
    // NOTE: `targetId` 按引用捕获（和这个函数里别的 lambda 同一个理由）：改名之后
    // 实例 id 会换一个，结构那一句的兼容性要跟新版本走。
    connect(&dialog, &InstanceSettingsDialog::lifecycleFileInfoRequested, &dialog,
            [this, &dialog, &targetId](const QString &key, const QString &path) {
                if (key == QStringLiteral("logs") || key == QStringLiteral("crash")) {
                    showLogFileInfo(dialog, key, path);
                    return;
                }
                if (key == QStringLiteral("replay")) {
                    const ReplayInfo::Info info = ReplayInfo::read(path);
                    if (!info.ok) {
                        dialog.setLifecycleInfoText(
                            key, path, tr("读取失败：%1").arg(info.problem));
                        return;
                    }
                    dialog.setLifecycleInfoText(key, path, ReplayInfo::summaryText(info),
                                                ReplayInfo::detailText(info));
                    return;
                }
                if (key != QStringLiteral("structure")) return;
                // 结构那一页列的是各存档 `generated/**.nbt` —— 和原理图同一种格式族，
                // 元数据、方块统计、版本兼容提示都问同一个 `SchematicInfo`。
                const VersionDisplayParts parts =
                    m_owner->m_versionManager->displayParts(targetId);
                const QString targetVersion =
                    parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
                const SchematicInfo::Info info = SchematicInfo::read(path);
                if (!info.ok) {
                    dialog.setLifecycleInfoText(
                        key, path, tr("读取失败：%1").arg(info.problem));
                    return;
                }
                dialog.setLifecycleInfoText(
                    key, path, SchematicInfo::summaryText(info, targetVersion),
                    SchematicInfo::topBlocksText(
                        info, cachedChineseBlockNames(
                                  AppPaths::assetsDir(),
                                  assetIndexIdOf(m_owner->m_versionManager->getVersionJson(targetId)))));
            });

    connect(&dialog, &InstanceSettingsDialog::diagnosisRequested, &dialog,
            [this, &dialog, &targetId] {
                const StartupDiagnosis::Inputs inputs =
                    m_owner->startupDiagnosisInputs(targetId);
                dialog.setDiagnosisText(StartupDiagnosis::analyze(inputs).toText());
            });

    // ---------- 「迁移」/「升级」那两页 ----------
    //
    // 用户在这一页做的只有三步：挑目标 → 看计划 / 勾范围 → 按动手。这一层负责把三样东西
    // 接起来：
    // ① 目标列表 = **这个游戏文件夹里已经装好的其他版本**（升级副本不下载新版本：
    // 目标没装的话它不在这一列里，页面上的话也这么写）；
    // ② 计划全在 `TransferPlanner`（扫盘 + 兼容结论 + 默认勾选），这一层只把
    // `VersionDisplayParts` 那几格事实递进去；
    // ③ 真正搬文件在 `TransferApply`（只增不删、不覆盖、不跟链接）；升级那一档先走
    // `copyInstanceTo()` 拿目标版本造一份新副本，再往副本里搬 —— 原实例一个字节不动。
    //
    // NOTE: 两条同步确认（`InlineMessage::question` / `getText`）都是嵌套事件循环，所以
    // sourceId / targetId 全都**按值收**再往下走。
    // 这几处都走 `InstancePagesController::loaderName()`，**不读 `parts.loaderName`**：
    // 那一格只有我们自己的安装器会写，PCL / HMCL 合并出来的实例（加载器只写在版本 json 的
    // `libraries` 里）在那儿是空的 —— 于是目标下拉框少一段「 / Fabric」、计划把这趟迁移
    // 当成「原版 ↔ 原版」评估、目标列表每一条都写「原版」（见 InstanceUpgradeController 里
    // 同一套写法）。
    const auto transferTargetsFor = [this, &targetId]() {
        QList<QPair<QString, QString>> targets;
        for (const QString &id : m_owner->m_versionManager->installedVersions()) {
            if (id == targetId) continue;
            const VersionDisplayParts parts = m_owner->m_versionManager->displayParts(id);
            const QString gameVersion = parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
            QString label = m_owner->instanceDisplayName(id);
            if (label.isEmpty()) label = id;
            const QString loaderName =
                InstancePagesController::loaderName(parts, m_owner->m_versionManager);
            label = loaderName.isEmpty() ? tr("%1（%2）").arg(label, gameVersion)
                                         : tr("%1（%2 / %3）").arg(label, gameVersion, loaderName);
            targets.append(qMakePair(label, id));
        }
        return targets;
    };
    const auto buildTransferPlan = [this, &targetId](const QString &key, const QString &otherId) {
        const VersionDisplayParts source = m_owner->m_versionManager->displayParts(targetId);
        const VersionDisplayParts target = m_owner->m_versionManager->displayParts(otherId);
        QString sourceLoaderVersion;
        QString targetLoaderVersion;
        const QString sourceLoader = InstancePagesController::loaderName(
            source, m_owner->m_versionManager, &sourceLoaderVersion);
        const QString targetLoader = InstancePagesController::loaderName(
            target, m_owner->m_versionManager, &targetLoaderVersion);
        TransferPlanner::Inputs inputs;
        inputs.sourceDir = AppPaths::versionDir(targetId);
        inputs.sourceGameVersion = source.realVersion.isEmpty() ? source.id : source.realVersion;
        inputs.sourceLoaderName = sourceLoader;
        inputs.sourceLoaderVersion = sourceLoaderVersion;
        inputs.targetGameVersion = target.realVersion.isEmpty() ? target.id : target.realVersion;
        inputs.targetLoaderName = targetLoader;
        inputs.targetLoaderVersion = targetLoaderVersion;
        inputs.upgrade = key == QStringLiteral("upgrade");
        return TransferPlanner::plan(inputs);
    };
    const auto pushTransferPlan = [&dialog](const QString &key,
                                            const TransferPlanner::Plan &plan) {
        QList<QPair<QString, QString>> entries;
        if (plan.ok) {
            for (const TransferPlanner::Item &item : plan.items) {
                if (!item.exists) continue;  // 没有的东西不摆勾选框（正文里已经说了「没有」）
                QString label = item.label;
                if (item.fileCount > 0) {
                    label += tr(" / %1 个文件 / %2")
                                 .arg(item.fileCount)
                                 .arg(FileHash::fileSizeText(item.bytes));
                }
                entries.append(qMakePair(label, item.key));
            }
        }
        dialog.setTransferPlan(key, plan.ok ? plan.detailText() : plan.problem);
        dialog.setTransferItems(key, entries,
                                plan.ok ? plan.defaultSelectedKeys() : QStringList());
    };
    const auto refreshTransferPage = [this, &dialog, transferTargetsFor, buildTransferPlan,
                                      pushTransferPlan](const QString &key) {
        if (key == QStringLiteral("upgrade") && dialog.property("modernUpgradeFlow").toBool()) return;
        auto targets = transferTargetsFor();
        if (key == QStringLiteral("upgrade")) {
            QSet<QString> prepared;
            for (auto &entry : targets) {
                const auto parts = m_owner->m_versionManager->displayParts(entry.second);
                const QString game = parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
                prepared.insert(game);
                QString loaderVersion;
                const QString loaderName = InstancePagesController::loaderName(
                    parts, m_owner->m_versionManager, &loaderVersion);
                entry.first = game + QStringLiteral(" / ") + (loaderName.isEmpty() ? tr("原版") : loaderName + QLatin1Char(' ') + loaderVersion)
                    + tr("（核心已准备：%1）").arg(m_owner->instanceDisplayName(entry.second));
            }
            for (const auto &version : m_owner->m_versionManager->availableVersions()) if (!prepared.contains(version))
                targets.append({version + tr("（选择并准备目标核心 / 加载器）"), QStringLiteral("@download:") + version});
        }
        dialog.setTransferTargets(key, targets);
        const QString targetIdNow = dialog.transferTarget(key);
        if (key == QStringLiteral("upgrade")) emit dialog.transferTargetChosen(key, targetIdNow);
        if (targetIdNow.isEmpty()) {
            dialog.setTransferPlan(key, key == QStringLiteral("upgrade")
                ? tr("请先选择目标版本。当前实例始终作为更新来源；尚未准备的版本可先准备核心与加载器。")
                : tr("此游戏目录里没有其它可迁移的实例。"));
            dialog.setTransferItems(key, {}, {});
            return;
        }
        if (targetIdNow.startsWith(QStringLiteral("@download:"))) {
            dialog.setTransferPlan(key, tr("目标 Minecraft %1 尚未准备。先安装目标核心与加载器，再回来检查更新。").arg(targetIdNow.mid(10)));
            dialog.setTransferItems(key, {}, {});
        } else pushTransferPlan(key, buildTransferPlan(key, targetIdNow));
    };
    refreshTransferPage(QStringLiteral("migration"));
    const auto sourceParts = m_owner->m_versionManager->displayParts(targetId);
    QString sourceLoaderVersion;
    const QString sourceLoader = InstancePagesController::loaderName(
        sourceParts, m_owner->m_versionManager, &sourceLoaderVersion);
    dialog.setUpgradeContext(tr("当前实例：%1\nMinecraft %2 / %3 %4")
        .arg(m_owner->instanceDisplayName(targetId), sourceParts.realVersion.isEmpty() ? sourceParts.id : sourceParts.realVersion,
             sourceLoader.isEmpty() ? tr("原版") : sourceLoader, sourceLoaderVersion));
    auto *upgradeStore = new ResourceStore(&dialog);
    upgradeStore->setCurseForgeApiKey(m_owner->m_settings->curseForgeApiKey());
    upgradeStore->setCurseForgeApiBaseOverride(m_owner->m_settings->curseForgeApiBaseOverride());
    upgradeStore->setModrinthApiBaseOverride(m_owner->m_settings->modrinthApiBaseOverride());
    UpgradeAssessment::attach(&dialog, m_owner->m_versionManager, upgradeStore, targetId, AppPaths::versionDir(targetId), AppPaths::instanceIconCacheDir());
    InstanceUpdateFlow::attach(m_owner, &dialog, targetId);
    connect(m_owner->m_versionManager, &VersionManager::versionsFetched, &dialog, [refreshTransferPage](const QStringList &) { refreshTransferPage(QStringLiteral("upgrade")); });
    if (m_owner->m_versionManager->availableVersions().isEmpty()) m_owner->m_versionManager->fetchAvailableVersions();
    connect(&dialog, &InstanceSettingsDialog::prepareUpgradeTargetRequested, &dialog, [this, &dialog, targetId, settingsTab](const QString &version) {
        dialog.reject();
        QTimer::singleShot(0, m_owner, [owner = QPointer<MainWindow>(m_owner), targetId, version, settingsTab] {
            if (!owner) return;
            // 来路写清是**「版本升级」那一页**（不是落地那栏 `game`）：回来时用户要回到
            // 他刚才点那一版的那一页。这一份 `NavigationReturn` 就是"来路"的唯一出处 ——
            // 以前是先 `openDownloadCategory("game", …)` 再回头改一位页键，两步之间
            // 那个状态是错的。
            owner->openDownloadCategory(
                QStringLiteral("game"),
                NavigationReturn::instanceSettings(settingsTab, targetId, QStringLiteral("upgrade")));
            owner->openInstallPage(version);
        });
    });

    const auto finishUpgradePage = [guard = QPointer<InstanceSettingsDialog>(&dialog)]
        (bool changed, const QString &) {
            if (!guard) return;
            if (changed) guard->reject(); // Core/metadata changed; do not save stale controls.
            else guard->setEnabled(true);
        };
    connect(&dialog, &InstanceSettingsDialog::upgradeInPlaceRequested, &dialog,
        [this, &dialog, &targetId, finishUpgradePage](const QString &otherId) {
            dialog.setEnabled(false);
            m_owner->m_instanceTransferController->upgradeInPlace(targetId, otherId, finishUpgradePage);
        });
    connect(&dialog, &InstanceSettingsDialog::rollbackUpgradeRequested, &dialog,
        [this, &dialog, &targetId, finishUpgradePage] {
            dialog.setEnabled(false);
            m_owner->m_instanceTransferController->rollbackUpgrade(targetId, finishUpgradePage);
        });

    connect(&dialog, &InstanceSettingsDialog::transferTargetChosen, &dialog,
            [&dialog, buildTransferPlan, pushTransferPlan](const QString &key, const QString &otherId) {
                if (key == QStringLiteral("upgrade") && dialog.property("modernUpgradeFlow").toBool()) return;
                if (otherId.isEmpty() || otherId.startsWith(QStringLiteral("@download:"))) {
                    dialog.setTransferPlan(key, otherId.isEmpty() ? tr("请选择目标版本。")
                        : tr("目标 Minecraft %1 尚未准备，安装核心与加载器后才能更新。").arg(otherId.mid(10)));
                    dialog.setTransferItems(key, {}, {}); return;
                }
                pushTransferPlan(key, buildTransferPlan(key, otherId));
            });
    connect(&dialog, &InstanceSettingsDialog::transferApplyRequested, &dialog,
            [this, &dialog, &targetId, transferTargetsFor, buildTransferPlan,
             pushTransferPlan, refreshTransferPage](const QString &key, const QString &otherId,
                                                    const QStringList &keys) {
                const QString sourceId = targetId;
                const QString targetIdCopy = otherId;
                const QString title =
                    key == QStringLiteral("upgrade") ? tr("创建升级副本") : tr("迁移");
                if (targetIdCopy.isEmpty()) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), title,
                                           tr("先挑一个目标。"));
                    return;
                }
                if (targetIdCopy == sourceId) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), title,
                                           tr("源实例与目标实例不能相同。"));
                    return;
                }
                if (keys.isEmpty()) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), title,
                                           tr("未选中要迁移的项目。"));
                    return;
                }
                // 两个实例任何一个在跑都不能动：游戏开着的时候拷出来的是一份写到一半的
                // 存档，而往一个正在读配置的实例里塞文件同样不安全。
                if (m_owner->m_launcherManager != nullptr
                    && (m_owner->m_launcherManager->isVersionRunning(sourceId)
                        || m_owner->m_launcherManager->isVersionRunning(targetIdCopy))) {
                    InlineMessage::warning(
                        m_owner->m_ui->overlay(), title,
                        tr("实例正在运行，请先关闭游戏。"));
                    return;
                }
                // 正在装（下载 / 安装器在跑）的那一份也不动：它的目录此刻正被写。
                const QSet<QString> installing = m_owner->installingVersionIds();
                if (installing.contains(sourceId) || installing.contains(targetIdCopy)) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), title,
                                           tr("实例正在安装，请完成后重试。"));
                    return;
                }
                // 现算一份计划：用户勾的那些文件可能在页面停着的时候变了。
                const TransferPlanner::Plan plan = buildTransferPlan(key, targetIdCopy);
                if (!plan.ok) {
                    InlineMessage::critical(m_owner->m_ui->overlay(), title, plan.problem);
                    return;
                }
                if (!InlineMessage::question(
                        m_owner->m_ui->overlay(), title, plan.confirmText(keys),
                        key == QStringLiteral("upgrade") ? tr("创建副本并迁移") : tr("开始迁移"),
                        tr("取消"), false)) {
                    return;
                }

                QString targetDir;
                QString newName;
                if (key == QStringLiteral("upgrade")) {
                    // 升级副本 = 用**已经装好的目标版本**复制一份新实例当底。这里
                    // `copyInstanceTo()` 会把"那一份正在跑 / 正在装 / 名字不合法"再挡一遍
                    // （四道闸和「复制实例」同一个函数），挡下来时它自己已经弹过话。
                    bool nameOk = false;
                    const QString suggestion =
                        tr("%1 升级副本").arg(m_owner->instanceDisplayName(sourceId));
                    newName = InlineMessage::getText(
                                  m_owner->m_ui->overlay(), title,
                                  tr("新实例名称：\n\n目标游戏目录中的基础版本：%1")
                                      .arg(m_owner->instanceDisplayName(targetIdCopy)),
                                  suggestion, &nameOk)
                                  .trimmed();
                    if (!nameOk || newName.isEmpty()) {
                        return;
                    }
                    targetDir = AppPaths::versionDir(newName);
                } else {
                    targetDir = AppPaths::versionDir(targetIdCopy);
                }

                // 上面两句 `InlineMessage::question` / `getText` 都是**嵌套
                // 事件循环** —— 用户在这一问上停着的时候，完全可能把游戏开起来，或者
                // 别的活儿（下载、导入、另一趟搬文件）恰好占了这两份实例。旧路径靠
                // `copyInstanceTo()` 那四道闸**再挡一遍**；后台化之后直接调
                // `VersionManager::copyInstance()`，那几道闸不在了，必须在这里补上。
                // 否则升级副本会去拷一个正在写的实例目录。
                if (m_owner->m_launcherManager != nullptr
                    && (m_owner->m_launcherManager->isVersionRunning(sourceId)
                        || m_owner->m_launcherManager->isVersionRunning(targetIdCopy))) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), title,
                                           tr("实例已开始运行，请先关闭游戏。"));
                    return;
                }
                {
                    const QSet<QString> busyNow = m_owner->installingVersionIds();
                    if (busyNow.contains(sourceId) || busyNow.contains(targetIdCopy)) {
                        InlineMessage::warning(m_owner->m_ui->overlay(), title,
                                               tr("实例正在安装或迁移，请完成后重试。"));
                        return;
                    }
                }

                // ---------- 长操作进任务中心 ----------
                //
                // 一块勾选 = 一步。升级那一档前面再加一步「造出新的那一份」，因为它比后面
                // 每一块都慢，而且它是唯一可能"整件事没开始"的一步。
                //
                // 「可恢复中断」不用另存进度：`TransferApply` 只增不覆盖，重跑时已经搬过去
                // 的按"目标里已经有"跳过，只有没搬完的才真正写盘。账本本身由
                // `TaskManager` 落盘，启动器中途没了的话读回来是 `Interrupted`。
                //
                // 「完成后设为当前/启动」这一版落在两处：这一项记着目标实例 id（任务中心
                // 的「打开目标位置」直接开到那一份），以及下面收尾那一问里的快速启动入口。
                QList<Tasks::TaskStep> steps;
                QHash<QString, int> stepIndexOf;
                if (key == QStringLiteral("upgrade")) {
                    steps << Tasks::TaskStep{tr("创建目标实例"),
                                             Tasks::TaskStepState::Running, {}};
                }
                for (const TransferPlanner::Item &item : plan.items) {
                    if (!keys.contains(item.key) || !item.exists) continue;
                    stepIndexOf.insert(item.key, steps.size());
                    steps << Tasks::TaskStep{item.label, Tasks::TaskStepState::Pending, {}};
                }
                // 收尾校验——搬完对一遍账：目标里是不是点名的那几个
                // 文件**都在**。它不重读内容，只比文件数与大小，所以比搬本身快得多。
                steps << Tasks::TaskStep{tr("校验"), Tasks::TaskStepState::Pending, {}};
                const int verifyIndex = steps.size() - 1;
                const bool upgrade = key == QStringLiteral("upgrade");
                // 2.1.2：这一趟也留一份统一报告（逐项结果 / 失败原因 / 可续入口）。
                // 编号在**搬之前**就定下来：报告、暂存和恢复入口认的是同一个号。
                const QString reportId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                const QString taskTargetId = upgrade ? newName : targetIdCopy;
                const Tasks::TaskId taskId = m_owner->m_activityCenter->begin(
                    upgrade ? Tasks::TaskKind::Install : Tasks::TaskKind::Other,
                    upgrade
                        ? tr("升级副本：%1 → %2")
                              .arg(m_owner->instanceDisplayName(sourceId), newName)
                        : tr("迁移：%1 → %2").arg(m_owner->instanceDisplayName(sourceId),
                                                  m_owner->instanceDisplayName(targetIdCopy)),
                    taskTargetId, steps);
                Tasks::TaskManager *ledger = m_owner->m_activityCenter->tasks();
                // 升级副本这一条用的是 `TaskKind::Install`，它的默认能力位
                // 里 `canCancel` 是真的（下载/安装/导入默认都能掐）。但这一条**没有**取消
                // 的入口：搬文件那一段是一个没有取消令牌的 `QFuture`。能力位必须照实
                // 写，否则界面会摆一颗按了没用的「取消」；同时把原因写在账本上 ——
                // 详情页就靠这一句回答"为什么这儿没有取消"。
                if (ledger != nullptr) {
                    Tasks::TaskCapabilities caps = ActivityCenterController::defaultCapabilities(
                        upgrade ? Tasks::TaskKind::Install : Tasks::TaskKind::Other);
                    caps.canCancel = false;
                    caps.cancelBlockedReason = upgrade
                        ? tr("升级副本要先整棵复制、再逐文件搬并校验；中途停下会留下半份副本，这一项不支持取消。")
                        : tr("迁移是一边搬一边记账的；中途停下会留下半搬的文件，这一项不支持取消（可以重新执行，已经搬过去的会跳过）。");
                    ledger->setCapabilities(taskId, caps);
                }
                const auto finishTask = [this, taskId](bool success, const QString &summary) {
                    m_owner->m_activityCenter->finish(taskId, success, summary, false);
                };

                // ---------- 真正搬的那一段放在后台线程 ----------
                //
                // 这一段**必须在后台线程**跑：升级副本先整棵
                // 复制实例目录、再逐个文件搬、最后再递归校验一遍；一份多 GB 的实例若同步跑在
                // UI 线程上，整扇窗会几分钟不重绘（Windows 挂「无响应」）。同一页上的
                // 「复制实例」也走后台（`copyInstanceInBackground()`）。
                //
                // 规矩和 `transferMods` 那条一样：
                // · 后台线程只碰盘和 `MigrationProgressState`（锁住的小账本），**不碰
                // 任何 QWidget / 任务账本**；
                // · 界面线程每 100 ms 把账本抄进任务中心（步骤状态、进度、提示行）；
                // · 收尾那一发 `finished` 才弹结果、问快速启动。
                //
                // NOTE: 用户此时可能已经把实例设置页收掉了（这一页不再阻塞调用方）。
                // 所以 finished 里对对话框的引用一律先过 `QPointer`；任务账本那一侧
                // 不依赖对话框，照样收尾。
                const QString sourceDir = AppPaths::versionDir(sourceId);
                // 升级副本的"底"是**用户挑的那个目标版本**，而 `targetDir`
                // 是这一趟要造出来的**新副本目录**（`versionDir(newName)`）—— 两回事。
                // worker 里那句 `copyInstanceFrom()` 要的是"从哪拷"，不是"拷到哪"；
                // 把 `targetDir` 当源传进去的话每次升级都在拷一个还不存在的目录：
                // 复制失败、只弹三句（确认 / 新名字 / 报错），连新副本都不会出现。
                // 源目录同样必须在主线程取好（理由同上）。
                const QString upgradeBaseDir =
                    upgrade ? AppPaths::versionDir(targetIdCopy) : QString();
                const qint64 totalUnits = stepIndexOf.size();
                auto progress = std::make_shared<MigrationProgressState>();
                auto appliedSteps = std::make_shared<QSet<QString>>();
                const QPointer<InstanceSettingsDialog> dialogGuard(&dialog);
                auto *overlay = m_owner->m_ui->overlay();
                QWidget *promptContent = overlay->openPrompt(false);
                QPointer<QLabel> promptDetail;
                QPointer<QProgressBar> promptBar;
                QPointer<QWidget> prompt;
                if (promptContent != nullptr) {
                    auto *promptLayout = qobject_cast<QVBoxLayout *>(promptContent->layout());
                    if (promptLayout == nullptr) {
                        promptLayout = new QVBoxLayout(promptContent);
                    }
                    auto *heading = new QLabel(title, promptContent);
                    auto *detail = new QLabel(
                        upgrade ? tr("正在后台复制实例目录…") : tr("准备中…"), promptContent);
                    detail->setWordWrap(true);
                    auto *bar = new QProgressBar(promptContent);
                    bar->setRange(0, int(totalUnits));
                    promptLayout->addWidget(heading);
                    promptLayout->addWidget(detail);
                    promptLayout->addWidget(bar);
                    promptDetail = detail;
                    promptBar = bar;
                    prompt = promptContent;
                }
                dialog.setEnabled(false);
                auto *watcher = new QFutureWatcher<MigrationOutcome>(m_owner);
                auto *timer = new QTimer(watcher);
                connect(timer, &QTimer::timeout, watcher,
                        [this, progress, promptDetail, promptBar, ledger, taskId, stepIndexOf,
                         appliedSteps, totalUnits] {
                            int done = 0;
                            QHash<QString, int> states;
                            {
                                QMutexLocker guard(&progress->mutex);
                                done = progress->doneUnits;
                                states = progress->itemStates;
                            }
                            if (promptDetail != nullptr) {
                                promptDetail->setText(
                                    tr("已搬完 %1 / %2 项").arg(done).arg(totalUnits));
                            }
                            if (promptBar != nullptr) {
                                promptBar->setValue(done);
                            }
                            ledger->setProgress(taskId, done, totalUnits);
                            for (auto it = states.cbegin(); it != states.cend(); ++it) {
                                if (appliedSteps->contains(it.key())) {
                                    continue;
                                }
                                appliedSteps->insert(it.key());
                                const int index = stepIndexOf.value(it.key(), -1);
                                if (index >= 0) {
                                    ledger->setStepState(
                                        taskId, index,
                                        it.value() ? Tasks::TaskStepState::Done
                                                   : Tasks::TaskStepState::Failed);
                                }
                            }
                        });
                connect(watcher, &QFutureWatcher<MigrationOutcome>::finished, m_owner,
                        [this, watcher, timer, overlay, prompt, promptDetail, promptBar,
                         progress, appliedSteps, dialogGuard, title, ledger, taskId,
                         verifyIndex, stepIndexOf, totalUnits, upgrade, newName, key,
                         sourceId, targetIdCopy, plan, keys, refreshTransferPage,
                         pushTransferPlan, buildTransferPlan, finishTask, reportId,
                         sourceDir, targetDir, taskTargetId] {
                            // 先摘"忙"标记，再走下面任何一条早退路径（复制失败 / 搬失败 /
                            // 对话框已经没了）。挂在里面的话每加一条 return 就漏一次。
                            m_owner->releaseFileOperationBusy(sourceId);
                            m_owner->releaseFileOperationBusy(targetIdCopy);
                            if (upgrade && !newName.isEmpty()) {
                                m_owner->releaseFileOperationBusy(newName);
                            }
                            // 刷新跟着摘标记走：只在"升级副本复制成功"
                            // 那一条路上刷的话，复制失败 / 搬文件失败 /
                            // 普通迁移这几条早退之后，被 `installingVersionIds()` 滤掉的
                            // 那一行就留在"不在"的状态里，直到下一次碰巧有东西再刷新。
                            m_owner->refreshInstalledVersions();
                            timer->stop();
                            watcher->deleteLater();
                            if (prompt != nullptr && prompt->isVisibleTo(overlay)
                                && overlay->promptOpen()) {
                                overlay->closeOverlay();
                            }
                            if (dialogGuard != nullptr) {
                                dialogGuard->setEnabled(true);
                            }
                            const MigrationOutcome outcome = watcher->result();
                            // ---------- 统一报告 + 恢复入口（2.1.2） ----------
                            // 迁移 / 升级副本的搬文件本来就是「只增不删」、可以重跑；
                            // 报告里记下源 / 目标 / 勾选的块，续做就是再跑一遍 apply+verify。
                            const auto recordReport = [&](bool complete, const QString &problem) {
                                OperationReport::Report report;
                                report.id = reportId;
                                report.kind = OperationReport::Kind::InstanceTransfer;
                                report.title = upgrade
                                    ? tr("升级副本：%1 → %2").arg(m_owner->instanceDisplayName(sourceId), newName)
                                    : tr("迁移：%1 → %2").arg(m_owner->instanceDisplayName(sourceId),
                                                              m_owner->instanceDisplayName(targetIdCopy));
                                report.instanceId = taskTargetId;
                                report.sourceRoot = sourceDir;
                                report.targetRoot = targetDir;
                                report.conflictPolicy = OperationReport::ConflictPolicy::SkipExisting;
                                report.totalFiles = outcome.apply.copiedFiles
                                    + outcome.apply.skippedFiles + outcome.apply.failedFiles;
                                report.doneFiles = outcome.apply.copiedFiles;
                                report.skippedFiles = outcome.apply.skippedFiles;
                                report.failedFiles = outcome.apply.failedFiles;
                                report.complete = complete;
                                report.failureReason = problem;
                                report.startedAt = QDateTime::currentDateTime();
                                report.updatedAt = report.startedAt;
                                QJsonArray itemArray;
                                for (const TransferPlanner::Item &item : plan.items) {
                                    if (!keys.contains(item.key) || !item.exists) continue;
                                    report.entries.append(OperationReport::Entry{
                                        item.relativePath, item.bytes,
                                        complete ? OperationReport::EntryState::Done
                                                 : OperationReport::EntryState::Pending,
                                        complete ? QString()
                                                 : tr("可继续未完成的迁移，已完成文件将跳过。")});
                                    itemArray.append(QJsonObject{
                                        {QStringLiteral("key"), item.key},
                                        {QStringLiteral("relativePath"), item.relativePath},
                                        {QStringLiteral("isDirectory"), item.isDirectory}});
                                }
                                QJsonArray keyArray;
                                for (const QString &one : keys) keyArray.append(one);
                                report.context = QJsonObject{
                                    {QStringLiteral("dataRoot"), AppPaths::minecraftDir()},
                                    {QStringLiteral("sourceDir"), sourceDir},
                                    {QStringLiteral("targetDir"), targetDir},
                                    {QStringLiteral("keys"), keyArray},
                                    {QStringLiteral("items"), itemArray}};
                                QString token;
                                if (!complete && m_owner->m_fileRecovery
                                    && m_owner->m_fileRecovery->registerReport(report)) {
                                    token = reportId;
                                }
                                if (ledger != nullptr) {
                                    ledger->setRecovery(taskId, token, !token.isEmpty(), QString());
                                }
                                return token;
                            };
                            // 定时器那一拍可能还没来得及把最后几块抄进账本。
                            {
                                QHash<QString, int> states;
                                {
                                    QMutexLocker guard(&progress->mutex);
                                    states = progress->itemStates;
                                }
                                for (auto it = states.cbegin(); it != states.cend(); ++it) {
                                    if (appliedSteps->contains(it.key())) {
                                        continue;
                                    }
                                    appliedSteps->insert(it.key());
                                    const int index = stepIndexOf.value(it.key(), -1);
                                    if (index >= 0) {
                                        ledger->setStepState(
                                            taskId, index,
                                            it.value() ? Tasks::TaskStepState::Done
                                                       : Tasks::TaskStepState::Failed);
                                    }
                                }
                            }

                            if (upgrade) {
                                if (outcome.copyFailed) {
                                    ledger->setStepState(taskId, 0, Tasks::TaskStepState::Failed,
                                                         outcome.copyError);
                                    finishTask(false, tr("复制实例没成"));
                                    recordReport(false, outcome.copyError);
                                    InlineMessage::critical(overlay, title, outcome.copyError);
                                    return;
                                }
                                ledger->setStepState(taskId, 0, Tasks::TaskStepState::Done);
                                ledger->setDetail(
                                    taskId,
                                    tr("目标实例“%1”已创建，正在迁移文件。").arg(newName));
                                // 列表已经在本函数开头刷过一次（摘忙标记之后），这里别重复刷。
                            }

                            if (!outcome.apply.ok) {
                                finishTask(false, outcome.apply.problem);
                                recordReport(false, outcome.apply.problem);
                                InlineMessage::critical(overlay, title, outcome.apply.problem);
                                return;
                            }

                            // ---------- 校验（最后一步） ----------
                            //
                            // 只看"点名搬的那些文件，目标里在不在"：`TransferApply` 只增
                            // 不覆盖，所以目标里原有的同名文件（大小不同也算）**不是
                            // 失败**，只如实报数。
                            ledger->setStepState(taskId, verifyIndex,
                                                 Tasks::TaskStepState::Running);
                            const TransferApply::VerifySummary &checks = outcome.verify;
                            const bool verified = checks.ok && checks.missingFiles == 0;
                            ledger->setStepState(taskId, verifyIndex,
                                                 verified ? Tasks::TaskStepState::Done
                                                          : Tasks::TaskStepState::Failed);

                            ledger->setDetail(taskId,
                                              outcome.apply.text() + QStringLiteral("\n\n")
                                                  + checks.text());
                            const bool allGood = outcome.apply.failedFiles == 0 && verified;
                            finishTask(allGood,
                                       allGood ? QString()
                                               : tr("有文件没搬成，或者搬完校验没对上"));
                            // 恢复入口只挂在终态上：先收尾，再登记报告钥匙。
                            recordReport(allGood, allGood ? QString()
                                                          : tr("有文件没搬成，或者搬完校验没对上"));

                            QString text =
                                outcome.apply.text() + QStringLiteral("\n\n") + checks.text();
                            if (upgrade) {
                                text += tr("\n\n新实例「%1」已在此游戏目录中。")
                                            .arg(newName);
                            } else {
                                text += tr("\n\n目标实例：「%1」。")
                                            .arg(m_owner->instanceDisplayName(targetIdCopy));
                            }
                            if (outcome.apply.failedFiles > 0 || !verified) {
                                InlineMessage::warning(overlay, title, text);
                            } else {
                                InlineMessage::information(overlay, title, text);
                            }

                            // ---------- 收尾那一问 ----------
                            //
                            // 「快速启动」在这一版就是主页的收藏区（`pinInstance()` 把它
                            // 移进置顶组，主页快速启动区读的就是这一组；「不新增第三份
                            // 快速启动数据」）。三档都由用户点头：
                            // · 保留原入口（默认）—— 什么都不动；
                            // · 创建新入口 —— 把新的那一份收藏进来；
                            // · 不加入 —— 新的不进去，原来那条入口也收起来。
                            const QString entryId = upgrade ? newName : targetIdCopy;
                            const QString entryName =
                                upgrade ? newName
                                        : m_owner->instanceDisplayName(targetIdCopy);
                            const int launchChoice = InlineMessage::ask(
                                overlay, MessageKind::Question, tr("快速启动"),
                                tr("「%1」要不要放进主页的快速启动区？").arg(entryName),
                                {tr("保留原入口"), tr("创建新入口"), tr("不加入")}, 0, -1,
                                false,
                                tr("快速启动显示收藏的实例。「保留原入口」维持当前设置，「不加入」会移除现有入口，可随时恢复。"));
                            if (launchChoice == 1) {
                                m_owner->pinInstance(entryId);
                            } else if (launchChoice == 2) {
                                // 只有**真在快速启动区里**才收起来：`unpinInstance()` 对
                                // "根本不在里面"的那一份也会写一条「未分组」归属。
                                if (m_owner->m_groupStore->isPinned(
                                        m_owner->instanceKey(sourceId))) {
                                    m_owner->unpinInstance(sourceId);
                                }
                            }

                            // 搬过之后（或者刚多了一份副本）把目标列和计划重摆一遍。
                            // 对话框可能已经被收掉了（这一页不再阻塞调用方）——
                            // 那两个 lambda 都捕着 `&dialog`，只能在它还活着时调。
                            if (dialogGuard == nullptr) {
                                return;
                            }
                            if (upgrade) {
                                refreshTransferPage(key);
                            } else {
                                pushTransferPlan(key, buildTransferPlan(key, targetIdCopy));
                            }
                        });
                timer->start(100);
                // 游戏目录必须在**主线程**取好再交给 worker：用户可以在这趟跑着的时候
                // 换游戏文件夹（见 AppPaths::minecraftDir() 的注释），而 `QString` 的
                // 隐式共享对象在两个线程里一读一写就是数据竞争。在 worker 里
                // 现调 `AppPaths::minecraftDir()` 正是这一枪。
                const QString gameDir = AppPaths::minecraftDir();
                // 这一趟在后台写盘，必须在"忙"名单上：
                // 否则这段时间里源码实例照样能被启动（读到的是一份拷到一半的目录）、
                // 能被删、还能再开一趟迁移 —— 和模组迁移那条路的闸不对称。
                // `installingVersionIds()` 就是"谁不许启动/删除"的唯一出处，
                // 这里把三个真正会被碰到的 id 都记进去（读源码、写目标、造新副本）。
                m_owner->markFileOperationBusy(sourceId);
                m_owner->markFileOperationBusy(targetIdCopy);
                if (upgrade && !newName.isEmpty()) {
                    m_owner->markFileOperationBusy(newName);
                }
                watcher->setFuture(QtConcurrent::run(
                    [sourceId, targetIdCopy, sourceDir, targetDir, upgradeBaseDir, plan, keys,
                     progress, upgrade, newName, gameDir] {
                        MigrationOutcome outcome;
                        if (upgrade) {
                            // 后台另起一份 VersionManager：`copyInstance()` 会动那两份
                            // 版本 json 缓存，拿主线程那一份在别的线程里跑就是数据竞争
                            // （同 `InstanceTransferController::copyInstanceInBackground`）。
                            // 拷的是**用户挑的那个目标版本**（`targetIdCopy`），不是当前
                            // 实例 —— 升级副本的意思就是"拿新版本当底、把这一份里的东西
                            // 搬过去"。源传成 `sourceId` 的话造出来的
                            // 是一份当前实例的克隆，版本号根本没变。
                            // 源目录 `targetDir` 也是在**主线程**取好的（见上面那两处
                            // `AppPaths::versionDir()`）：worker 里现算就是和"切换游戏
                            // 文件夹"抢同一个 QString。
                            VersionManager worker;
                            if (!worker.copyInstanceFrom(upgradeBaseDir, targetIdCopy, gameDir,
                                                         newName, &outcome.copyError)) {
                                outcome.copyFailed = true;
                                return outcome;
                            }
                        }
                        outcome.apply = TransferApply::apply(
                            sourceDir, targetDir, plan.items, keys,
                            [progress](const QString &itemKey, bool ok) {
                                QMutexLocker guard(&progress->mutex);
                                ++progress->doneUnits;
                                progress->itemStates.insert(itemKey, ok ? 1 : 0);
                            });
                        if (outcome.apply.ok) {
                            outcome.verify = TransferApply::verify(sourceDir, targetDir,
                                                                   plan.items, keys);
                        }
                        return outcome;
                    }));
            });

    connect(&dialog, &InstanceSettingsDialog::resourcePackPageEntered, m_owner,            [this, &dialog, &targetId] {
                m_owner->m_instanceEditorController->refreshResourcePacks(&dialog, targetId);
            });
    connect(dialog.resourcePackPage(), &ResourcePackPage::selectionRequested, m_owner,
            [this, &dialog, &targetId](const QStringList &ids, const QByteArray &expected, bool exists) {
                m_owner->m_instanceEditorController->saveResourcePacks(&dialog, targetId, ids, expected, exists);
            });

    // 用户自己勾掉的那几页——**必须排在 refreshInstanceDirs() 之后**：
    // 灌进来的那份表是"藏起来的页"，而"这一条到底露不露"要拿"可不可用"一起算（见
    // InstanceSettingsDialog::shownEntryKeys()）。反过来的话，第一遍重建会先把不可用
    // 那几条全列出来、再被这一发推回去 —— 中间那一下会顺手把右栏换到某页上
    // （rebuildEntryList() 里那个"当前页没了就回第一页"）。
    dialog.setPageChoices(
        m_owner->m_versionManager->versionMeta(targetId).value(VersionManager::settingsPagesField())
            .toObject());

    // 两条**动作**信号：这一页里那两颗按钮（导出启动脚本 / 补全文件）不自己
    // 干活 —— ui/ 那边不 include src/、也不碰磁盘，"这一次到底执行什么"只有这一层知道
    // （authManager / versionManager / launcherManager 都在这儿）。
    //
    // 全部接在 `showPage()` **之前**：那一句一摆上去，用户当场就能点按钮（函数的返回和
    // 用户的下一次点击之间没有任何"还在准备中"的窗口），接晚了就一辈子收不到。
    //
    // NOTE: `dialog` 是**堆上**的一份（见本函数开头那个 `InstanceSettingsSession`），
    // 这几条 connect 的上下文一律是 `dialog` 自己 ⇒ 它一收掉，连接跟着断，轮不到
    // lambda 去碰已经析构的对象。`targetId` / `refreshInstanceDirs` / `serverEdits`
    // 都挂在那个会话上，跟着对话框一起走。
    connect(&dialog, &InstanceSettingsDialog::exportLaunchScriptRequested, m_owner,
            [this, &targetId](const QString &path) { m_owner->exportLaunchScript(targetId, path); });
    connect(&dialog, &InstanceSettingsDialog::repairFilesRequested, m_owner,
            [this, &targetId]() { m_owner->repairInstanceFiles(targetId); });
    // 第三条同类：概览那一栏的「导出…」（ 从灰占位换成真的）。**和上面第一条
    // 不是一件事** —— 那个出的是 .bat，这个出的是整合包。
    connect(&dialog, &InstanceSettingsDialog::exportModpackRequested, m_owner,
            [this, &targetId](const QString &path) { m_owner->exportModpack(targetId, path); });

    // 第四条同类：概览那一栏的「检查更新」（要求「实例设置那里做一个更新
    // 检测」）。**结果是一个弹框**（Prompt 那一档），所以这一发是**异步**的：反查要读几十兆
    // jar（后台线程），每个项目还要各问一发版本列表 —— 那几发回来的时候这一页可能还开着
    // （正常），也可能已经被用户收掉了（那时候 `m_owner->m_ui->overlay()` 上摆着的是别的页面）。
    //
    // NOTE: 收页了也**照样弹**（不判断这一页还在不在）：用户按下这一颗按钮之后，那个框
    // 就是他等的那个结果 —— 把它吞掉等于"按了没反应"。这一条和文件管理那几处
    // "动完盘得刷新那一页"不一样：那些是**改这一页的样子**，这一件是把结果**告诉人**。
    connect(&dialog, &InstanceSettingsDialog::checkUpdatesRequested, m_owner,
            [this, &targetId, &dialog]() { m_owner->checkInstanceUpdates(targetId, &dialog); });
    connect(&dialog, &InstanceSettingsDialog::modCheckSelectedUpdatesRequested, &dialog,
            [this, &targetId, &dialog](const QStringList &paths) {
                if (!paths.isEmpty()) m_owner->checkInstanceUpdates(targetId, &dialog, paths);
            });
    connect(&dialog, &InstanceSettingsDialog::modInspectDependenciesRequested, &dialog,
            [this, &targetId](const QStringList &paths) {
                const QString root = m_owner->instanceFileDirs(targetId).value(QStringLiteral("mod"));
                if (root.isEmpty() || !QFileInfo(root).isDir()) return;
                if (m_owner->installingVersionIds().contains(targetId)) {
                    InlineMessage::information(m_owner->m_ui->overlay(), tr("实例正在处理文件"),
                        tr("请等待当前文件操作结束后再检查依赖。")); return;
                }
                QStringList currentPaths;
                for (const auto &info : QDir(root).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name))
                    currentPaths.append(info.absoluteFilePath());
                InstanceIconLookup::Options options;
                options.cacheDir = AppPaths::instanceIconCacheDir(); options.cacheKey = targetId;
                options.cacheOnly = true;
                const auto cached = InstanceIconLookup::lookup(currentPaths, options);
                QHash<QString, InstanceIconLookup::Found> byPath;
                for (const auto &found : cached.found) byPath.insert(found.path, found);
                QHash<QString, ModDependencies::UpdateVerdict> checked;
                for (const auto &entry : m_owner->m_instanceUpdateResults.value(targetId).verdicts)
                    checked.insert(entry.item.path, entry);
                QList<ModDependencies::InstalledItem> inventory, selected;
                for (const auto &path : currentPaths) {
                    ModDependencies::InstalledItem item; item.path = path;
                    const auto found = byPath.constFind(path);
                    if (found != byPath.cend()) {
                        item.projectId = found->hit.projectId; item.source = found->hit.source;
                        item.title = found->hit.title; item.sha1 = found->sha1;
                        item.currentVersion = found->currentVersion;
                        const auto previous = checked.constFind(path);
                        if (!item.currentVersion.dependencyMetadataKnown && !item.sha1.isEmpty() &&
                            previous != checked.cend() && previous->item.sha1 == item.sha1 &&
                            previous->item.source == item.source && previous->item.projectId == item.projectId)
                            item.currentVersion = previous->currentVersion;
                    }
                    inventory.append(item);
                    if (paths.contains(path)) selected.append(item);
                }
                if (selected.isEmpty()) {
                    InlineMessage::information(m_owner->m_ui->overlay(), tr("选择已变化"),
                        tr("选中的文件已不存在，请刷新列表后重试。")); return;
                }
                InlineMessageBox box(MessageKind::Information, tr("依赖与冲突"),
                    tr("只读检查当前启用文件及有效缓存，不联网。资料未知时可先检查更新，再回来查看。"),
                    {tr("关闭")}, 0, -1);
                auto *report = new QPlainTextEdit;
                report->setObjectName(QStringLiteral("modDependencyInspectionText")); report->setReadOnly(true);
                report->setPlainText(ModDependencies::inspectInstalledDependencies(selected, inventory));
                report->setMinimumHeight(180); report->setMaximumHeight(300); box.setContent(report);
                m_owner->m_ui->overlay()->execPrompt(&box);
            });
    connect(&dialog, &InstanceSettingsDialog::modWebsiteRequested, &dialog,
            [this](const QString &path) {
                QString key;
                if (!m_owner->m_instanceIconController->fileResourceIconKey(path, &key)) return;
                const auto hit = m_owner->m_resourceHits.value(key);
                const QUrl url(ResourceCatalog::projectPageUrl(hit.source, ResourceCatalog::Kind::Mod, hit.slug));
                if (url.scheme() == QStringLiteral("https") && !url.host().isEmpty())
                    QDesktopServices::openUrl(url);
                else InlineMessage::information(m_owner->m_ui->overlay(), tr("项目网页"),
                                                tr("当前资源没有可用的项目网址。"));
            });

    // 「更新中心」：紧挨着「检查更新」的那一颗。
    //
    // **和上面那一条相反，这一颗不收页**：它开的那一页（更新中心）自己就是盖在内容区上
    // 的一整页，而这一颗要先把当前这一页**收掉**才开得进去（`m_owner->runWhenOverlayFree()` 会
    // 静默收掉占着的那一层 —— 用户 定的方向）。
    // 这一颗**一个网络都不发**（它读的是上一次检测留下来的那份结论）：所以断网的时候
    // 「撤销上一次更新」也按得动 —— 那正是它存在的意义（见 InstanceSettingsDialog 里
    // 那个信号的说明）。
    connect(&dialog, &InstanceSettingsDialog::updateCenterRequested, m_owner,
            [this, &targetId]() {
                const QString id = targetId;
                m_owner->runWhenOverlayFree([this, id] { m_owner->openUpdateCenter(id); });
            });

    // 「检查并修复」：概览栏那颗按钮。**和「检查更新」不是
    // 一回事** —— 那个查的是模组有没有新版本（联网），这个查的是游戏文件齐不齐（只读盘）。
    //
    // 收页再起：点下这一颗就说明用户要看的是"体检结果"，而结果会开成**另一整页**（报告页）。
    // 留着这一页的话，扫完那一下要么叠一层、要么得走"让位"（见 onIntegrityScanFinished 那段）；
    // 先把它收掉最干净。**排到下一轮再起**：体检起手那几道守卫失败时会往**同一条覆盖层**上
    // 弹提示，现在不排的话会撞上还没收干净的这一页。
    connect(&dialog, &InstanceSettingsDialog::integrityCheckRequested, m_owner,
            [this, &targetId, &dialog]() {
                dialog.reject();  // 和顶上那颗「← 返回」是同一条路
                const QString id = targetId;
                QTimer::singleShot(0, m_owner, [this, id] { m_owner->submitIntegrityCheck(id); });
            });

    // ---------- 概览那一栏那两颗「快照」 ----------
    //
    // NOTE: 「创建快照…」那一问（默认排除还是完整）在 `m_owner->snapshotInstance()` 里 —— 弹话要
    // OverlayHost，那玩意儿在 MainWindow 手里，这一层连读都没有读。
    connect(&dialog, &InstanceSettingsDialog::createSnapshotRequested, m_owner,
            [this, &targetId]() { m_owner->snapshotInstance(targetId); });
    connect(&dialog, &InstanceSettingsDialog::inspectRecoveryRequested, m_owner,
            [this] { m_owner->m_backupController->inspectRecovery(); });
    // 多捕一个 &dialog：还原会把整个实例目录换掉，那一页控件的值全是旧的 ——
    // BackupController 收尾时要能把它禁用 / 收掉（理由见 restoreInstance() 那段 ★）。
    connect(&dialog, &InstanceSettingsDialog::restoreSnapshotRequested, m_owner,
            [this, &targetId, &dialog]() {
                m_owner->restoreInstanceSnapshot(targetId, &dialog);
            });

    // 「文件管理」那三发：开文件夹 / 启用·禁用一条 / 删一条。
    //
    // NOTE: 这两条**要多捕获一个 &dialog** —— 动完盘得让那几页重列一遍（不然删掉的那一行
    // 还在屏幕上，用户会以为没删掉）。dialog 挂在那个会话上、活到收页为止，所以引用一直
    // 活着；那几发也只在页开着的时候可能发生。
    connect(&dialog, &InstanceSettingsDialog::backupSelectedWorldRequested, m_owner,
            [this](const QString &path) { m_owner->backupWorldNow(path); });
    connect(&dialog, &InstanceSettingsDialog::restoreSelectedWorldRequested, m_owner,
            [this, &dialog](const QString &path) { m_owner->m_backupController->restoreWorld(path, &dialog); });
    connect(&dialog, &InstanceSettingsDialog::openInstanceFileRequested, m_owner,
            [this](const QString &path) {
                if (!QFileInfo(path).isFile() || !QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), tr("打开文件"),
                                           tr("无法打开文件：%1").arg(QDir::toNativeSeparators(path)));
                }
            });
    connect(&dialog, &InstanceSettingsDialog::openLogFileRequested, m_owner,
            [this](const QString &path) {
                if (!QFileInfo(path).isFile()) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), tr("打开文件"),
                                           tr("无法打开文件：%1").arg(QDir::toNativeSeparators(path)));
                    return;
                }
                m_owner->m_gameSessionController->openLogFile(path);
            });
    connect(&dialog, &InstanceSettingsDialog::openInstanceFolderRequested, m_owner,
            [this, &targetId](const QString &path) {
                if (path.isEmpty()) {
                    return;
                }
                // 先认实例根目录，再决定要不要建快捷目录：实例已经被删掉、页面却还留着时，
                // 直接 `mkpath(path)` 会把 `versions/<旧实例>/saves` 这类空壳目录重新建出来，
                // 首页就多出一个“删不掉的空实例”。根目录不在就只报一句，绝不落盘。
                const QString instanceRoot = AppPaths::versionDir(targetId);
                if (!QFileInfo(instanceRoot).isDir()) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), tr("实例已不存在"),
                                           tr("这个实例的目录已经不在盘上了，无法打开或创建它的文件夹。请刷新实例列表。"));
                    m_owner->m_ui->overlay()->dismiss();
                    return;
                }
                // 先建出来再开：新实例的 saves/ screenshots/ 多半还不存在，而这九颗按钮
                // 就是给"我要往里面放东西"用的 —— 在这儿弹一句"这个文件夹不在盘上"
                // 正好挡住用户要做的事。建一个空目录也正是游戏第一次跑起来会做的事。
                QDir().mkpath(path);
                m_owner->onFolderOpenRequested(path);
            });
    connect(&dialog, &InstanceSettingsDialog::toggleInstanceFileRequested, m_owner,
            [this, &dialog](const QString &path) {
                // **成败都要重列**（和紧接着那两条批量处理器同一个做法）。
                // 行尾那颗 `ToggleSwitch` 是可 check 的 `QAbstractButton` —— Qt 在发出
                // `clicked` **之前**就把它翻过去了，而这一层的约定是"改完盘再回头摆状态、
                // 真生效了没有看下一轮刷新"（ToggleSwitch.hpp 那段）。失败那条路（同名
                // `.disabled` 已存在 / 文件被游戏占着 / 权限）不刷新的话，开关会停在
                // "翻过去"的位置，盘上名字一个字节没变，界面和盘正好相反；用户对着这份
                // 假现状按下一次，以为自己在"启用"一个其实早就启用着的文件。
                m_owner->toggleInstanceFile(path);
                dialog.refreshFilePages();
            });
    connect(&dialog, &InstanceSettingsDialog::deleteInstanceFileRequested, m_owner,
            [this, &dialog](const QString &path) {
                if (m_owner->deleteInstanceFile(path)) {
                    dialog.refreshFilePages();
                }
            });

    // ui/ 那边只管"用户要动这一批"（它不 include src/、也不碰磁盘），到底下什么、
    // 每条落哪儿、名字怎么改，全在这一层（和上面那两条同一个分工）。
    connect(&dialog, &InstanceSettingsDialog::fileBatchUpdateRequested, m_owner,
            [this, &targetId](const QString &pageKey, const QStringList &paths) {
                const QString contextId = targetId;
                if (pageKey == QStringLiteral("mod")) {
                    QStringList names;
                    const auto targets = m_owner->m_instanceUpdateTargets.value(contextId);
                    QStringList executable;
                    QHash<QString, FileHash::Stamp> previewStamps;
                    for (const auto &path : paths) {
                        const auto version = targets.value(path);
                        const auto *file = version.primaryFile();
                        if (!file || file->url.isEmpty() || !QFileInfo(path).isFile()) continue;
                        executable.append(path);
                        previewStamps.insert(path, FileHash::stampOf(path));
                    }
                    if (executable.isEmpty()) {
                        InlineMessage::information(m_owner->m_ui->overlay(), tr("没有更新"),
                                                   tr("选中项没有可安装的更新，请先检查更新。")); return;
                    }
                    const auto results = m_owner->m_instanceUpdateResults.value(contextId).verdicts;
                    QHash<QString, ModDependencies::UpdateVerdict> byPath;
                    for (const auto &entry : results) byPath.insert(entry.item.path, entry);
                    QList<ResourceCatalog::Hit> recognizedActive;
                    const QDir modDir(m_owner->instanceFileDirs(contextId).value(QStringLiteral("mod")));
                    for (const auto &info : modDir.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
                        if (info.fileName().endsWith(InstanceFilesPage::disabledSuffix(), Qt::CaseInsensitive)) continue;
                        QString key;
                        if (m_owner->m_instanceIconController->fileResourceIconKey(info.absoluteFilePath(), &key)) {
                            const auto hit = m_owner->m_resourceHits.value(key);
                            if (!hit.projectId.isEmpty()) recognizedActive.append(hit);
                        }
                    }
                    for (const auto &path : executable) {
                        const auto entry = byPath.value(path);
                        QHash<QString, QString> plannedVersions, projectNames;
                        for (const auto &hit : recognizedActive) if (hit.source == entry.item.source) {
                            plannedVersions.insert(hit.projectId, QString());
                            projectNames.insert(hit.projectId, hit.title.isEmpty() ? hit.projectId : hit.title);
                        }
                        for (const auto &known : results) {
                            if (known.item.source != entry.item.source || known.item.projectId.isEmpty()) continue;
                            if (!QFileInfo(known.item.path).isFile() || known.item.path.endsWith(
                                InstanceFilesPage::disabledSuffix(), Qt::CaseInsensitive)) continue;
                            plannedVersions.insert(known.item.projectId, known.currentVersion.id);
                            projectNames.insert(known.item.projectId, known.item.title.isEmpty() ? known.item.projectId : known.item.title);
                        }
                        for (const auto &plannedPath : executable) {
                            const auto planned = byPath.value(plannedPath);
                            if (planned.item.source == entry.item.source && !planned.item.projectId.isEmpty())
                                plannedVersions.insert(planned.item.projectId, targets.value(plannedPath).id);
                        }
                        const auto version = targets.value(path);
                        names.append(tr("%1\n%2\n目标文件：%3\n加载器：%4 / 游戏：%5")
                            .arg(QFileInfo(path).fileName(), ModDependencies::updateDifferenceText(
                                entry.currentVersion, version, plannedVersions, projectNames),
                                version.primaryFile()->filename, version.loaders.join(QStringLiteral(" / ")),
                                version.gameVersions.join(QStringLiteral(" / "))));
                    }
                    InlineMessageBox preview(MessageKind::Question, tr("模组更新差异"),
                        tr("将更新 %1 项。请查看依赖变化与需确认项；不会自动补装或删除依赖。").arg(executable.size()),
                        {tr("确认更新"), tr("取消")}, 1, 0);
                    auto *text = new QPlainTextEdit;
                    text->setObjectName(QStringLiteral("modUpdateDifferenceText"));
                    text->setReadOnly(true);
                    text->setPlainText(names.join(QStringLiteral("\n\n────────────────\n\n")));
                    text->setMinimumHeight(180); text->setMaximumHeight(300);
                    preview.setContent(text);
                    m_owner->m_ui->overlay()->execPrompt(&preview);
                    if (preview.clickedIndex() != 0) return;
                    if (targetId != contextId) return;
                    if (m_owner->installingVersionIds().contains(contextId) ||
                        (m_owner->m_launcherManager && m_owner->m_launcherManager->isVersionRunning(contextId))) {
                        InlineMessage::warning(m_owner->m_ui->overlay(), tr("暂时不能更新"),
                                               tr("先关闭游戏并等待实例安装完成。")); return;
                    }
                    const auto current = m_owner->m_instanceUpdateTargets.value(contextId);
                    for (const auto &path : executable) {
                        if (current.value(path).id != targets.value(path).id ||
                            !FileHash::sameStamp(previewStamps.value(path), FileHash::stampOf(path))) {
                            InlineMessage::information(m_owner->m_ui->overlay(), tr("更新清单已变化"),
                                tr("检测结果或本地文件在确认期间发生变化，请重新查看清单。")); return;
                        }
                    }
                    m_owner->updateInstanceFiles(contextId, pageKey, executable);
                    return;
                }
                // `targetId` **按引用**（和上面那批 lambda 同一个理由，见 refreshInstanceDirs
                // 的 NOTE）：改名之后它变了，而这一发要更新的是**改名之后**那个实例里的
                // 文件。**不刷新那一页** —— 理由见 m_owner->updateInstanceFiles() 末尾那段。
                m_owner->updateInstanceFiles(targetId, pageKey, paths);
            });
    connect(&dialog, &InstanceSettingsDialog::fileBatchToggleRequested, m_owner,
            [this, &dialog](const QString &pageKey, const QStringList &paths, bool disable) {
                Q_UNUSED(pageKey);  // 改名字跟哪一页无关：给的就是**这一批的绝对路径**
                m_owner->toggleInstanceFiles(paths, disable);
                // 一律重列：改过名的那几条路径变了（选中集里它们会掉出去，这是对的），
                // 而"现在到底是启用还是禁用"也要重新画。
                dialog.refreshFilePages();
            });
    connect(&dialog, &InstanceSettingsDialog::fileBatchDeleteRequested, m_owner,
            [this, &dialog](const QString &pageKey, const QStringList &paths) {
                Q_UNUSED(pageKey);  // 同上：删的是一批绝对路径
                m_owner->deleteInstanceFiles(paths);
                // 删掉的那几行还在屏幕上就说明没删掉 —— 重列一次。
                dialog.refreshFilePages();
            });

    connect(&dialog, &InstanceSettingsDialog::modTransferRequested, &dialog,
            [this, &dialog, &targetId, transferTargetsFor, buildTransferPlan]
            (const QStringList &paths, bool move) {
        if (paths.isEmpty()) return;
        const QString sourceId = targetId;
        const QString title = move ? tr("移动模组") : tr("复制模组");
        const auto targets = transferTargetsFor();
        QStringList labels;
        for (const auto &entry : targets) labels.append(entry.first);
        if (labels.isEmpty()) {
            InlineMessage::warning(m_owner->m_ui->overlay(), title, tr("没有其它已安装的实例。"));
            return;
        }
        int row = -1;
        if (!InlineMessage::pickFromList(m_owner->m_ui->overlay(), title,
                tr("选择目标实例（不覆盖同名文件）"), labels, 0, &row)) return;
        if (row < 0 || row >= targets.size() || targetId != sourceId) return;
        const QString destId = targets.at(row).second;
        const auto plan = buildTransferPlan(QStringLiteral("migration"), destId);
        if (!plan.ok) {
            InlineMessage::warning(m_owner->m_ui->overlay(), title, plan.problem); return;
        }
        if (!InlineMessage::question(m_owner->m_ui->overlay(), title,
                tr("%1\n\n将%2 %3 个模组到「%4」。\n不会自动适配版本，不覆盖现有文件，不迁移配置。%5")
                    .arg(plan.compatibilitySentence, move ? tr("移动") : tr("复制"))
                    .arg(paths.size()).arg(targets.at(row).first)
                    .arg(move ? tr("\n目标校验成功后才删除对应源文件。") : QString()),
                tr("继续"), tr("取消"), false, true)) return;
        // Confirmations run nested event loops; check context and busy state again.
        if (targetId != sourceId || m_owner->installingVersionIds().contains(sourceId) ||
            m_owner->installingVersionIds().contains(destId) ||
            (m_owner->m_launcherManager &&
             (m_owner->m_launcherManager->isVersionRunning(sourceId) ||
              m_owner->m_launcherManager->isVersionRunning(destId)))) {
            InlineMessage::warning(m_owner->m_ui->overlay(), title,
                                   tr("实例正在运行、安装，或当前实例已切换，请稍后重试。")); return;
        }
        auto *overlay = m_owner->m_ui->overlay();
        QWidget *content = overlay->openPrompt(false);
        if (!content) return;
        auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
        if (!layout) layout = new QVBoxLayout(content);
        auto *heading = new QLabel(title, content);
        auto *detail = new QLabel(tr("准备复制与校验…"), content);
        detail->setWordWrap(true);
        auto *bar = new QProgressBar(content);
        bar->setRange(0, paths.size());
        auto *cancel = new QPushButton(tr("取消"), content);
        Theme::markButton(cancel, Theme::ButtonKind::Neutral);
        layout->addWidget(heading); layout->addWidget(detail);
        layout->addWidget(bar); layout->addWidget(cancel);
        struct Progress { std::atomic_bool cancel{false}; QMutex mutex; int done = 0; QString detail; };
        const auto state = std::make_shared<Progress>();
        auto *watcher = new QFutureWatcher<TransferApply::ModTransferResult>(m_owner);
        auto *timer = new QTimer(watcher);
        const QPointer<QLabel> safeDetail(detail);
        const QPointer<QProgressBar> safeBar(bar);
        const QPointer<QWidget> prompt(content);
        const QPointer<InstanceSettingsDialog> safeDialog(&dialog);
        dialog.setEnabled(false);
        const auto task = m_owner->m_activityCenter->begin(Tasks::TaskKind::Other,
            title, sourceId, {Tasks::TaskStep{tr("复制并校验模组文件"), Tasks::TaskStepState::Running, {}}});
        m_owner->m_modTransferCancels.insert(task, [state] { state->cancel = true; });
        auto capabilities = m_owner->m_activityCenter->tasks()->snapshot(task).capabilities;
        capabilities.canCancel = true;
        m_owner->m_activityCenter->tasks()->setCapabilities(task, capabilities);
        m_owner->markFileOperationBusy(sourceId);
        m_owner->markFileOperationBusy(destId);
        connect(cancel, &QPushButton::clicked, watcher, [state, cancel] {
            state->cancel = true; cancel->setEnabled(false); cancel->setText(tr("正在取消…"));
        });
        const auto dismissConnection = connect(overlay, &OverlayHost::promptDismissed, watcher,
            [state] { state->cancel = true; });
        const auto quitConnection = connect(qApp, &QCoreApplication::aboutToQuit, watcher,
            [state] { state->cancel = true; });
        connect(timer, &QTimer::timeout, watcher, [this, state, safeDetail, safeBar, task, paths] {
            QMutexLocker guard(&state->mutex);
            if (safeDetail) safeDetail->setText(state->detail);
            if (safeBar) safeBar->setValue(state->done);
            m_owner->m_activityCenter->tasks()->setDetail(task, state->detail);
            m_owner->m_activityCenter->tasks()->setProgress(task, state->done, paths.size());
        });
        connect(watcher, &QFutureWatcher<TransferApply::ModTransferResult>::finished, m_owner,
            [this, watcher, timer, sourceId, destId, safeDialog, prompt, overlay, title, task,
             dismissConnection, quitConnection] {
                timer->stop(); disconnect(dismissConnection); disconnect(quitConnection);
                const auto result = watcher->result(); watcher->deleteLater();
                m_owner->releaseFileOperationBusy(sourceId);
                m_owner->releaseFileOperationBusy(destId);
                m_owner->m_modTransferCancels.remove(task);
                // 两个实例都刚被摘掉忙标记 ⇒ 实例页那一行也要立刻回来。
                // 对话框可能已经被收掉了（`safeDialog` 为空），所以这一刷不能挂在它上面。
                m_owner->refreshInstalledVersions();
                if (safeDialog) { safeDialog->setEnabled(true); safeDialog->refreshFilePages(); }
                if (prompt && prompt->isVisibleTo(overlay) && overlay->promptOpen()) overlay->closeOverlay();
                const QString summary = tr("已验证复制 %1 项，其中移动 %2 项；跳过 %3 项，失败 %4 项。%5\n%6")
                    .arg(result.copied).arg(result.moved).arg(result.skipped).arg(result.failed)
                    .arg(result.cancelled ? tr("\n已取消，未完成项的来源保留。") : QString())
                    .arg(result.problems.join(QLatin1Char('\n')));
                m_owner->m_activityCenter->tasks()->setDetail(task, summary);
                if (!result.cancelled) {
                    m_owner->m_activityCenter->tasks()->setProgress(task, 1, 1);
                    m_owner->m_activityCenter->tasks()->setStepState(task, 0,
                        result.failed == 0 ? Tasks::TaskStepState::Done : Tasks::TaskStepState::Failed);
                }
                if (result.cancelled) m_owner->m_activityCenter->tasks()->cancel(task);
                else m_owner->m_activityCenter->finish(task, result.failed == 0, summary, false);
                m_owner->appendLog(summary);
                InlineMessage::information(overlay, title, summary);
            });
        timer->start(100);
        const QString sourceDir = AppPaths::versionDir(sourceId);
        const QString destDir = AppPaths::versionDir(destId);
        watcher->setFuture(QtConcurrent::run([sourceDir, destDir, paths, move, state] {
            return TransferApply::transferMods(sourceDir, destDir, paths, move,
                [state](int done, int, const QString &detail) {
                    QMutexLocker guard(&state->mutex); state->done = done; state->detail = detail;
                }, [state] { return state->cancel.load(); });
        }));
    });

    // ---------- 「复制实例」----------
    //
    // 要求：「实例设置的概览里面加一条复制实例，点击以后让用户选择复制到的文件夹，然后
    // 给定复制后的实例名称」。三件事按顺序：
    // ① 选**游戏文件夹**（`QFileDialog::getExistingDirectory`，和「下载到哪个文件夹」那一处
    // 同一路）；② 问**新名字**（`InlineMessage::getText`，预填"<原名> 副本"）；
    // ③ 真正动手的那一半走 m_owner->copyInstanceTo()。
    //
    // ②问的是**新名字**、①问的是**游戏文件夹**，不是"拷到哪个目录底下"：副本落在
    // `<挑中的游戏文件夹>/versions/<新名字>/`。理由见 VersionManager::copyInstance 那段
    // ——"拷进一个普通目录"的话，那一份在启动器里**根本不会出现**（它只认 versions/ 下面
    // 的东西），用户会觉得按了没反应。这一条在按钮的 tooltip 里也写明了。
    //
    // NOTE: 两条原生对话框都在这一层（用户 那四条 QFileDialog 的同一个理由：
    // ui/ 不碰 AppPaths，"该默认到哪儿"只有这儿知道）。它们**驱动不了**，所以真正
    // 动手的那一半被切了出去，验证工程直接喂路径。
    connect(&dialog, &InstanceSettingsDialog::copyInstanceRequested, m_owner,
            [this, &targetId, &dialog]() {
                const QString sourceId = targetId;  // 按值收：下面那两条对话框会让出事件循环
                const QString defaultDir = AppPaths::minecraftDir();
                const QString folder = QFileDialog::getExistingDirectory(
                    m_owner, tr("复制到哪个游戏目录"), defaultDir,
                    QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
                if (folder.isEmpty()) {
                    return;  // 用户取消了
                }
                const QString suggestion = tr("%1 副本").arg(m_owner->instanceDisplayName(sourceId));
                bool ok = false;
                const QString name = InlineMessage::getText(
                    m_owner->m_ui->overlay(), tr("复制实例"), tr("副本名称："), suggestion, &ok);
                if (!ok) {
                    return;
                }
                // 后台版（「不在 UI 线程直接复制整个目录」）：拷一整个
                // 实例动辄几十秒，放在 UI 线程上会把整扇窗按住。四道闸还是同一套，只是拷树
                // 挪到后台，进度和结果落在任务中心。
                //
                // 拷树跑着的这段时间这一页**必须禁用**：用户能在这儿
                // 改设置（写的就是正在被拷的那份目录 ⇒ 副本里是一个前后不一致的中间态），
                // 也能按「从快照恢复」把源目录整个换掉。和上面「移动替换模组文件」那一趟
                // 同一个做法：期间禁用这一页，收尾（成功 / 失败 / 被挡下）再解冻。
                // 直接问 transfer controller 是为了拿到 `whenDone` —— MainWindow 那一层
                // 的薄壳是私有槽，参数里加不进 std::function。
                // QPointer：用户完全可能在复制期间把这一页收掉。
                const QPointer<InstanceSettingsDialog> safeDialog(&dialog);
                dialog.setEnabled(false);
                m_owner->m_instanceTransferController->copyInstanceInBackground(
                    sourceId, folder, name, [safeDialog](bool, const QString &) {
                        if (safeDialog) safeDialog->setEnabled(true);
                    });
            });

    // ---------- 「删除实例」----------
    //
    // 要求：「顺便把删除实例功能做了，按钮放在概览里」。**没有新写删除**：那一颗按钮
    // 走的就是实例行行尾那颗垃圾桶走的那条路（`m_owner->confirmDeleteInstance()`，两道闸 + 问一句
    // + 真删目录）。这一层只多一件事 —— **删完把这一页收掉**。
    //
    // 为什么要收：这一页说的是"某个实例的设置"，而那个实例刚被删掉。不收的话用户会停在
    // 一页对着已经不存在的目录的设置上：上面那几格还写着它的名字和分组，随便动一格都会
    // 把 wtfml.meta 写进一个**已经被删掉的目录**里（`QDir().mkpath()` 会把它重新建出来
    // —— 屏幕上就会出现一个只有 meta、没有 json 的空壳实例）。
    //
    // 收页走 `dismiss()`：它发的就是"返回箭头那一发"，`showPage()` 把它接在
    // `dialog->reject()` 上（于是和用户自己点返回箭头走同一条路）；`closeOverlay()`
    // 是另一条路（见 OverlayHost::dismiss() 那段 ★）。
    //
    // NOTE: 没删成（用户点了取消、或者被两道闸拦下）**不收页** —— 那时候实例还在，
    // 用户正该停在这一页上看着那句解释。
    connect(&dialog, &InstanceSettingsDialog::deleteInstanceRequested, m_owner,
            [this, &targetId]() {
                // 按值收："问一句"那一步会让出事件循环，删完这一页也要收掉了。
                const QString id = targetId;
                if (m_owner->confirmDeleteInstance(id)) {
                    m_owner->m_ui->overlay()->dismiss();
                }
            });

    // ---------- 「存档」那一页----------
    //
    // 这一页上有三样东西：行尾那支笔（「编辑」）、顶上「导入世界」、顶上「批量编辑」
    // （**占位**）。三发信号都是**页面自己**发的 —— 没有走 InstanceSettingsDialog 再转发
    // 一遍：那一层转发只是把同样的三行重写一次，而这几位只有这一页会发。
    //
    // NOTE: `filesPage()` 在那一页**没被列出来**时（用户在左栏把它勾掉了）返回 nullptr，
    // 所以这里必须判空 —— 不判就是空指针解引用，症状是"一开实例设置就崩"，而
    // 取消勾选这一页是用户随时会做的事。
    //
    // NOTE: 三条都接在 `showPage()` **之前**，理由和上面那一整批逐字相同（页一摆上去
    // 用户就能点，接晚了那一发就漏了）。
    if (InstanceFilesPage *saves = dialog.filesPage(QStringLiteral("save"))) {
        // 那支笔 = **直接进「世界设置」那一页**：三件事（重命名 / 打开文件夹 /
        // 删除世界）都搬进页里当按钮了，所以这一支笔点下去只剩一件事可做。
        //
        // NOTE: 点下去**一句话都不问，直接进页**：备份靠页里顶上那颗「立即备份」，以及
        // 展开「修改存档文件」那个折叠框时那一句警告（见 `fileEditingRequested` 那一段）。
        connect(saves, &InstanceFilesPage::editRequested, m_owner,
                [this, &dialog](const QString &path) { m_owner->showWorldSettingsPage(&dialog, path); });

        // 发过来的是一行存档的**绝对路径**，而 `--quickPlaySingleplayer` 要的是**存档目录
        // 名**（游戏就是按 `saves/` 底下那个目录名找世界的）⇒ 这儿摘一个 fileName() 出来。
        // NOTE: 摘名字这一步**只能在这一层**办：ui/ 那一层只知道"哪一条被选中了"，而
        // "游戏要的是名字不是路径"是那条启动参数的规矩，写在 launcher/QuickPlay.hpp 里。
        //
        // NOTE: 路径空 = 没选中（那颗按钮此刻是灰的，见 InstanceFilesPage::updateJoinEnabled()
        // —— 这是第二道闸）。
        connect(saves, &InstanceFilesPage::joinRequested, m_owner,
                [this, &targetId](const QString &path) {
                    if (path.isEmpty()) {
                        return;
                    }
                    const QString worldName = QFileInfo(path).fileName();
                    if (worldName.isEmpty()) {
                        return;
                    }
                    m_owner->appendLog(tr("试着直接进入世界：%1").arg(worldName));
                    m_owner->startQuickPlay(targetId, QuickPlay::Kind::Singleplayer, worldName);
                });

        // 顶上那颗「导入世界」：选一个世界目录 → 问它进来之后叫什么 → 拷进 saves/。
        // 两条对话框的顺序和「复制实例」一致（先选东西、再起名字，名字预填现成的那个）。
        connect(saves, &InstanceFilesPage::importRequested, m_owner,
                [this, &dialog, &targetId]() {
                    // 默认开在**当前游戏文件夹自己的 saves** 上：用户多半是从别的整合包 /
                    // 别的游戏文件夹里拿过来的那一份，从这儿起步最省事。
                    // NOTE: 这个目录可能不存在（一个没跑过游戏的游戏文件夹），
                    // QFileDialog 自己会退回"我的电脑"那一层，不用我们判。
                    const QString startDir =
                        AppPaths::minecraftDir() + QStringLiteral("/saves");
                    const QString dir = QFileDialog::getExistingDirectory(
                        m_owner, tr("选择要导入的世界"), startDir,
                        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
                    if (dir.isEmpty()) {
                        return;  // 用户取消了
                    }
                    // 预填用户挑的那个目录名：世界叫什么，目录名就是什么（见 WorldFiles.hpp
                    // 那段"两边都不读 level.dat 里的世界名"）。
                    bool ok = false;
                    const QString name = InlineMessage::getText(
                        m_owner->m_ui->overlay(), tr("导入世界"), tr("导入进来之后叫什么？"),
                        QFileInfo(dir).fileName(), &ok);
                    if (!ok) {
                        return;
                    }
                    const QPointer<InstanceSettingsDialog> page(&dialog);
                    m_owner->m_instanceTransferController->importWorldInBackground(
                        targetId, dir, name, [page](bool success, const QString &) {
                            if (success && page) page->refreshFilePages();
                        });
                });

        // 顶上那颗「批量编辑」：**今天是个占位**（要求「这个先做占位」）。
        // 那颗按钮的 tooltip 里已经写了"还没做"，这里再补一句**看得见**的话 —— 只靠
        // tooltip 的话，用户点下去屏幕上什么都不发生，那和"按钮坏了"分不出来。
        // NOTE: 用 information 而不是 warning：这不是出错，是"这一颗还没轮到"。
        connect(saves, &InstanceFilesPage::batchEditRequested, m_owner,
                [this]() {
                    InlineMessage::information(m_owner->m_ui->overlay(), tr("批量编辑"),
                                               tr("暂不支持批量编辑。"));
                });
    }

    // ---------- 「世界设置」那一页----------
    //
    // 和「配置文件」那一页一样：这一页是**构造时就建好、一直留着**的，所以这四条
    // connect 只连这一次（不是"每开一个世界连一遍"）。
    // NOTE: 依然接在 `showPage()` **之前** —— 页一摆上去用户就能按，接晚了那一发就漏了
    // （和上面那一整批逐字相同）。
    if (WorldSettingsPage *world = dialog.worldSettingsPage()) {
        connect(world, &WorldSettingsPage::saveRequested, m_owner,
                [this, &dialog]() { m_owner->saveWorldSettings(&dialog); });
        // 页眉面包屑「返回 / 世界设置：<世界名>」：世界设置那一格露出来时改页眉的字、
        // 把「返回」接到 `leaveWorldSettings()`（回存档列表，改过的要先问）；收回去时还原。
        connect(&dialog, &InstanceSettingsDialog::worldBreadcrumbChanged, m_owner,
                [this, guard = QPointer<InstanceSettingsDialog>(&dialog)](const QString &title) {
                    OverlayHost *overlay = m_owner->m_ui->overlay();
                    if (overlay == nullptr || guard == nullptr) {
                        return;
                    }
                    overlay->setPageTitle(title.isEmpty() ? guard->titleText() : title);
                    if (title.isEmpty()) {
                        overlay->setBackOverride(nullptr);
                        return;
                    }
                    overlay->setBackOverride([this, guard] {
                        if (guard != nullptr) {
                            m_owner->leaveWorldSettings(guard);
                        }
                        return true;
                    });
                });
        // 存档目录一定存在（这一格能开着就是刚从这个目录底下读出来的），所以**不 mkpath**
        // —— 和上面那九颗快捷方式按钮那一条故意不同。
        connect(world, &WorldSettingsPage::openFolderRequested, m_owner,
                [this]() {
                    if (!m_owner->m_worldDir.isEmpty()) {
                        m_owner->onFolderOpenRequested(m_owner->m_worldDir);
                    }
                });

        // ---------- 整份存档的备份 / 还原 / 打开备份目录 ----------
        //
        // 三颗都**只做、只报**（和上面那颗「备份一份」一个形状）：它们不动页面上任何一格，
        // 也不切页。还原那一颗例外 —— 它会把这一页上摆着的值全换掉（那些值就是 level.dat
        // 里读出来的），所以跑完得把这一页重读一遍，见 onBackupRestoreFinished() 那段。
        connect(world, &WorldSettingsPage::fullBackupRequested, m_owner,
                [this]() { m_owner->backupWorldNow(m_owner->m_worldDir); });
        connect(world, &WorldSettingsPage::openBackupFolderRequested, m_owner,
                [this]() { m_owner->openBackupFolder(BackupFiles::Kind::World); });
        connect(world, &WorldSettingsPage::restoreBackupRequested, m_owner,
                [this, &dialog]() { m_owner->m_backupController->restoreWorld(m_owner->m_worldDir, &dialog); });

        // 「重命名…」在页里当按钮。改完之后**页面上那个名字也得换**
        // （`setDirName()`），而且这一层记着的那条路径（`m_owner->m_worldDir`）跟着变 —— 不换的
        // 话，用户改完名接着按「保存」，`m_owner->saveWorldSettings()` 会对着一个已经不在了的
        // 目录报"这个存档不见了"，而屏幕上刚显示完"改好了"。
        connect(world, &WorldSettingsPage::renameRequested, m_owner,
                [this, &dialog, world]() {
                    if (m_owner->m_worldDir.isEmpty()) {
                        return;  // 没开着哪个世界（这一页没 setWorld 过）—— 不该发生
                    }
                    const QString worldDir = m_owner->m_worldDir;
                    const QString worldName = QFileInfo(worldDir).fileName();
                    bool ok = false;
                    const QString wanted = InlineMessage::getText(
                        m_owner->m_ui->overlay(), tr("重命名世界"), tr("这个存档以后叫什么？"), worldName,
                        &ok);
                    if (!ok) {
                        return;
                    }
                    // 实例 id 从**路径**反推（这一页记的是存档目录，不是实例 —— 见 .hpp 里
                    // `m_owner->m_worldDir` 那条 NOTE）。反推不出来（存档挂在某个实例的 saves 外面）
                    // 就是空串 —— `isVersionRunning("")` 是 false，于是"正在跑就不许改"那道
                    // 闸不管它，交给 `renameWorld()` 自己判盘上那份还在不在。
                    if (!m_owner->renameInstanceWorld(m_owner->instanceIdOfPath(worldDir), worldDir, wanted)) {
                        return;
                    }
                    // 名字一变，这一层记的那条路径**同时**跟着变。`WorldFiles::renameWorld()`
                    // 只动目录名（见它的头文件），所以 `level.dat` 的大小 / 修改时间两样都
                    // 没变 —— 打开时量的那个戳对新的那条路径照样作数，"盘上被人改过"那道
                    // 闸不会因为改了个名就误报。
                    const QString renamed =
                        QFileInfo(worldDir).absolutePath() + QLatin1Char('/') + wanted.trimmed();
                    m_owner->m_worldDir = renamed;
                    world->setDirName(wanted.trimmed());
                    // 重列一遍存档页：目录名变了，行上那行小字和那支笔挂的路径**全都得跟着
                    // 换**。
                    dialog.refreshFilePages();
                });

        // 「删除世界…」的做法照着 `m_owner->deleteInstanceFile()` 那
        // 一条：**先问一句**（红的那一档）、再由控制器把它**移进回收站**（junction 安全）、
        // 删不掉时把"多半是被游戏占着"说出来。
        //
        // NOTE: 这里不比 `m_owner->deleteInstanceFile()` 多判一条"游戏在跑"就删 —— 多判的那一条
        // 是有用的：游戏手里那份存档是它自己读进去的，它退出（或者在游戏里存一次）
        // 的时候会**把整个世界重新写一遍**，而那时候目录已经没了。
        connect(world, &WorldSettingsPage::deleteRequested, m_owner,
                [this, &dialog]() {
                    if (m_owner->m_worldDir.isEmpty()) {
                        return;
                    }
                    const QString worldDir = m_owner->m_worldDir;
                    const QString worldName = QFileInfo(worldDir).fileName();
                    const QString instanceId = m_owner->instanceIdOfPath(worldDir);
                    if (!instanceId.isEmpty() &&
                        m_owner->m_launcherManager->isVersionRunning(instanceId)) {
                        InlineMessage::warning(m_owner->m_ui->overlay(), tr("删除世界"),
                                               tr("游戏正在运行，请关闭后再删除存档。"));
                        return;
                    }
                    if (!instanceId.isEmpty() &&
                        m_owner->installingVersionIds().contains(instanceId)) {
                        InlineMessage::warning(
                            m_owner->m_ui->overlay(), tr("删除世界"),
                            tr("「%1」正在被另一项操作占用（安装、导入、复制、备份或还原），等它结束之后再删这个世界。")
                                .arg(m_owner->instanceDisplayName(instanceId)));
                        return;
                    }
                    // 默认**不**落在「删除」上（`defaultYes = false`）：误按一下回车不该把
                    // 用户几百 MB 的游玩记录删掉（和 `m_owner->deleteInstanceFile()` 一个方向）。
                    if (!InlineMessage::question(
                            m_owner->m_ui->overlay(), tr("删除世界"),
                            tr("把「%1」连同里面的东西一起删掉？它们会进回收站，要恢复可以从那里找回来。")
                                .arg(worldName),
                            tr("删除"), tr("取消"), /*defaultYes=*/false,
                            /*destructive=*/true)) {
                        return;
                    }
                    // 删整棵目录树走后台：几百 MB 的世界删起来可能好几秒，
                    // 不能把 UI 线程按住。**不能**用 `QDir::removeRecursively()`：实测它会
                    // 跟着 junction / 符号链接走进目标目录（见 FolderManager.cpp 里那段）
                    // —— 把 saves 挂到网盘是常见做法，而这一颗点的是"删掉这个世界"。
                    //
                    // NOTE: 删除**不提供取消**：删到一半停下留下的是半截世界，比删完更难
                    // 收拾。控制器那一侧按 `TaskKind::Other` 记账（默认没有取消位）。
                    // NOTE: 完成回调里的 `page` 用 `QPointer`：页面可以在这期间被关掉。
                    const QPointer<InstanceSettingsDialog> page(&dialog);
                    m_owner->m_instanceTransferController->deleteWorldInBackground(
                        instanceId, worldDir,
                        [this, page, worldDir](bool success, const QString &) {
                            if (!success) {
                                return;  // 失败提示控制器已经弹过
                            }
                            // 用户在这期间可能已经退回去、甚至点开了**别的**世界：只有
                            // 这一层记的仍是删掉的那一个时才收页、清缓存，否则只重列一遍
                            // 存档页（那一行现在指着一条不存在的目录）。
                            const bool sameWorld = m_owner->m_worldDir == worldDir;
                            if (sameWorld) {
                                m_owner->m_worldDir.clear();
                                m_owner->m_worldStampSize = 0;
                                m_owner->m_worldStampMtimeMs = 0;
                            }
                            if (page == nullptr) {
                                return;
                            }
                            if (sameWorld) {
                                page->closeWorldSettings();
                            } else {
                                page->refreshFilePages();
                            }
                        });
                });

        // 「修改存档文件」那个折叠框（ 新加）。要求：「修改文件的设置放在一个
        // 类似版本下载那里的收起来的框里，默认收起，**展开是弹出备份警告**」。
        //
        // 所以这一发信号说的**不是"框开了"**，是"用户想改文件了"（见 .hpp 里那个信号）：
        // 那几格改的是 `level.dat` 里游戏自己都改不了的东西（世界名 / 极限模式 / 难度），
        // 改坏了这个世界就变味了 —— 所以开框之前先把"备份"这件事摆到他面前。
        //
        // NOTE: 页面上那颗钮**已经**把框摆开了（它先摆样子再发信号 —— 不先摆的话，用户按
        // 下去屏幕上什么都不动，看着像按了个坏按钮）。所以这里三条出口要各自负责
        // 把样子摆回去：只有「取消」那一档要收。
        connect(world, &WorldSettingsPage::fileEditingRequested, m_owner,
                [this, world](bool wanted) {
                    if (!wanted) {
                        return;  // 用户自己把框收起来了 —— 这一趟不需要问任何事
                    }
                    const QString worldName = QFileInfo(m_owner->m_worldDir).fileName();
                    const int choice = InlineMessage::ask(
                        m_owner->m_ui->overlay(), MessageKind::Question, tr("改动存档文件"),
                        tr("将直接修改“%1”的存档名称、游戏模式、难度及规则。建议先备份存档信息。")
                            .arg(worldName),
                        {tr("取消"), tr("先创建备份"), tr("继续修改")},
                        /*defaultIndex=*/1, /*destructiveIndex=*/2,
                        /*dismissOnOutsideClick=*/false,
                        tr("备份当前 level.dat，不修改存档。"));
                    if (choice == 0 || choice == -1) {
                        // 「取消」和"点外面关掉"都算"不改了" ⇒ 把框**收回去**（不收回来的
                        // 话页面上那个框开着、而用户什么也没答应，看着像那句警告白弹了）。
                        world->setFileEditingOpen(false);
                        return;
                    }
                    if (choice == 1) {
                        QString problem;
                        if (!m_owner->backupWorldLevelData(m_owner->m_worldDir, &problem)) {
                            InlineMessage::warning(m_owner->m_ui->overlay(), tr("备份创建失败"), problem);
                            // 备份没留成 ⇒ **不许接着改**（这一整套的前提正是"改坏了能
                            // 回去"，而这四格没有撤销）—— 和 `m_owner->saveWorldSettings()` 里
                            // "留不下就不写"同一条规矩。
                            world->setFileEditingOpen(false);
                            return;
                        }
                        InlineMessage::information(
                            m_owner->m_ui->overlay(), tr("备份好了"),
                            tr("已备份“%1”的存档信息：\n%2")
                                .arg(worldName,
                                     QDir::toNativeSeparators(m_owner->latestWorldBackup(m_owner->m_worldDir))));
                    }
                    // 「继续修改」/「先备份一份」（备成了）：框**保持开着**。
                    //
                    // NOTE: 这里显式摆一次而不是"什么都不做" —— 页面上那颗钮是在**信号发
                    // 出去之前**摆的样子，中间跑了两个嵌套事件循环（那一问、以及备份
                    // 好了那一句），把这件事实打实地钉一遍比"相信中间没人动过它"稳。
                    world->setFileEditingOpen(true);
                });
    }

    // ---------- 「配置文件」那一页----------
    //
    // 这一页和别的几页不是一回事：点一行**不是"重列一遍"**，而是"读那个文件、把它摆进
    // 编辑器"。编辑器和那张列表是**同一页里的两格**（开不出第二个整页覆盖层 —— 见
    // InstanceSettingsDialog::configFilePage() 上面那一段），所以这一趟没有"开页 / 关页"，
    // 只有"读进来灌进去"和"切回第一格"。
    //
    // NOTE: `configFilePage()` 是**构造时就建好、一直留着**的那一个（那把 NOTE 说了不要
    // 把它当一次性的）—— 所以这三条 connect 只连这一次，之后每开一个文件都复用。
    connect(&dialog, &InstanceSettingsDialog::openConfigFileRequested, m_owner,
            [this, &dialog](const QString &path) { m_owner->openConfigFile(&dialog, path); });
    if (ConfigFileEditorPage *editor = dialog.configFilePage()) {
        connect(editor, &ConfigFileEditorPage::saveRequested, m_owner,
                [this, &dialog](const QString &path, const QByteArray &bytes) {
                    m_owner->saveConfigFile(&dialog, path, bytes);
                });
        // 编辑器给的是**文件**路径，开文件夹要的是它所在的那个目录。
        //
        // NOTE: 这儿**不 mkpath**（不像上面那九颗快捷方式按钮）：那个目录一定是存在的
        // ——这一格能开着，说明刚从这个目录底下读出来过一个文件。
        connect(editor, &ConfigFileEditorPage::openFolderRequested, m_owner,
                [this](const QString &path) {
                    const QString dir = QFileInfo(path).absolutePath();
                    if (!dir.isEmpty()) {
                        m_owner->onFolderOpenRequested(dir);
                    }
                });
        // 「还原上一次」：读备份是读盘（见 ConfigFileEditorPage.hpp 那段"这一层不碰盘"），
        // 所以那一格只发"用户要这个文件的上一次"，取和摆在这儿做。
        connect(editor, &ConfigFileEditorPage::restoreRequested, m_owner,
                [this, &dialog](const QString &path) { m_owner->restoreConfigFile(&dialog, path); });
        connect(editor, &ConfigFileEditorPage::historyRequested, m_owner,
                [this, &dialog](const QString &path) {
                    m_owner->m_instanceEditorController->showConfigHistory(&dialog, path);
                });
        connect(editor, &ConfigFileEditorPage::backRequested, m_owner,
                [this, &dialog]() { m_owner->leaveConfigEditor(&dialog); });
    }
    // 顶上那颗返回箭头（覆盖层页眉上那一颗）走的是 `dialog->reject()`，点一下关掉的是
    // **整个「实例设置」** —— 而编辑器那一格里可能正压着改了一半的东西。对话框把那一关
    // 挡下来了（见它那条 reject()）：这一发就是"挡下来了、你说怎么办"。
    //
    // NOTE: 用户说"丢掉"之后**重新走一遍 `dialog.reject()`** 而不是直接 `done()`：
    // 这一格是干净的，下一轮 reject() 会落到「世界设置」那道闸上。两格里同时压着
    // 东西是可能的（在世界那一页改过、切到配置文件那一页又改过），直接 done()
    // 会把世界那一页的改动静默丢掉 —— 问都没问。
    connect(&dialog, &InstanceSettingsDialog::closeBlockedByUnsavedConfigFile, m_owner,
            [this, &dialog]() {
                ConfigFileEditorPage *editor = dialog.configFilePage();
                if (editor == nullptr || !m_owner->confirmDiscardConfigEdits()) {
                    return;  // 留在这儿：页照旧开着，编辑器那一格原样。
                }
                editor->discardChanges();
                dialog.reject();
            });
    // 「世界设置」那一页上的同一件事（上面那条的第二个实例，逐字适用）。
    //
    // NOTE: 这一条**不用**先把那一页倒回去（上面那条要 `discardChanges()`）：整页马上就关
    // 了，下一次从那支笔进来又是从头 setWorld() 灌一遍（见 m_owner->showWorldSettingsPage()），
    // 留着一个"倒回原样"的动作只是给下一次开页添乱。
    connect(&dialog, &InstanceSettingsDialog::closeBlockedByUnsavedWorldSettings, m_owner,
            [this, &dialog]() {
                if (!m_owner->confirmDiscardWorldEdits()) {
                    return;  // 留在这儿：页照旧开着，那一格原样。
                }
                m_owner->m_worldDir.clear();
                m_owner->m_worldStampSize = 0;
                m_owner->m_worldStampMtimeMs = 0;
                dialog.done(QDialog::Rejected);
            });
    // 服务器列表那一页的「刷新」：重读一遍 servers.dat 再递进去，**顺手测一遍**
    // —— 「刷新」的意思是"把这一页刷新成现在的样子"，而这一页现在不只是
    // 那份文件的镜像（图标 / MOTD / 延迟都是刚问来的）。
    // 「测速」并进来了（要求「服务器那一栏测速和刷新作用重复了」），
    // 所以那一条 connect 没有第二份了 —— 这一发就是这一页"重新看一眼"的全部。
    connect(&dialog, &InstanceSettingsDialog::refreshServerListRequested, m_owner,
            [this, &dialog, &targetId]() {
                m_owner->loadServerList(&dialog, targetId);
                m_owner->pingServers(&dialog, targetId);
            });
    connect(&dialog, &InstanceSettingsDialog::serverFavoriteRequested, &dialog,
            [this, &dialog](const QString &address, bool favorite) {
                if (!m_owner->m_settings->setServerFavorite(address, favorite)) {
                    InlineMessage::warning(m_owner->m_ui->overlay(), tr("收藏未保存"),
                        tr("无法保存服务器收藏，请检查服务器地址和启动器设置的写入权限。"));
                    return;
                }
                QStringList favorites;
                for (const auto &entry : dialog.serverAddresses())
                    if (m_owner->m_settings->isServerFavorite(entry)) favorites.append(entry);
                dialog.setServerFavorites(favorites);
            });
    // 「点进去就自动测速」（要求）。这一发是**切到那一页**发出来的
    // （见 InstanceSettingsDialog::applyPageKey），和上面那两条"用户按了某颗按钮"不是
    // 一回事。它每一页都会发一次 —— 用户切走再切回来就又测一遍，而"已经在跑"那一趟
    // 不会被重开（见 pingServers 里那道闸）。
    connect(&dialog, &InstanceSettingsDialog::serverPageEntered, m_owner,
            [this, &dialog, &targetId]() { m_owner->pingServers(&dialog, targetId); });

    // 「下载新模组 / 资源包 / 数据包 / 光影」那四颗：**切到下载页对应的那一栏**。
    // 那一页和下载页是两块地方，路只有这一条 —— 收页 + 摆选项卡 + 挑那一行全在
    // m_owner->openDownloadCategory() 一处（内含"为什么要排到下一轮事件循环"那句）。
    //
    // NOTE: 接在 `showPage()` **之前**，理由同上：页一摆上去用户就能按，接晚了那一发就漏了。
    // 把**是哪个实例**、**哪颗根页**、**那一页滚到哪儿**一起带过去
    // （`NavigationReturn` 就是"来路"那一份形状）。那一栏靠它做三件事：左上角露出
    // 「← 返回实例设置」（回得来）、把「已安装版本」选到它身上（下来的东西落进"当前
    // 实例"）、以及回去时把那一页和滚动位置原样摆回来。不带的话用户点完那颗键就回不去了。
    connect(&dialog, &InstanceSettingsDialog::browseDownloadsRequested, m_owner,
            [this, &dialog, &targetId, settingsTab](const QString &categoryKey) {
                NavigationReturn origin = NavigationReturn::instanceSettings(
                    settingsTab, targetId,
                    InstanceSettingsDialog::filesDownloadPageKey(categoryKey));
                // 离开这一刻量一下文件页滚到哪儿（和 fileResourceRequested 那条同一个
                // 理由：回来时那一页是**另一份** InstanceFilesPage，位置只有这一刻还活着）。
                if (InstanceFilesPage *files =
                        dialog.filesPage(InstanceSettingsDialog::filesDownloadPageKey(categoryKey))) {
                    const InstanceFilesPage::ScrollPosition position = files->scrollPosition();
                    origin.scrollIdentity = position.identity;
                    origin.scrollDelta = position.delta;
                }
                m_owner->openDownloadCategory(categoryKey, origin);
            });

    // 行上的名字点得动：**进的是那个资源自己的那一页**
    // （和下载页里点一行开的是同一页）—— 前提是名字匹配上了，
    // 换成"切到下载页"就跟匹配不匹配没关系了。
    // 路径 → 它是哪个资源，是上面那趟反查认出来的（见 m_owner->m_instanceFileIdentities）；页面
    // 那边只会为**记过名字**的那几条发这一发（没认出来的名字压根点不动）。
    // `targetId` 按**引用**收（和上面那批 lambda 同一个理由，见 refreshInstanceDirs 的
    // NOTE）：它必须取到**改名之后**的值 —— 用户改完名再点名字，回来时那一页要开在
    // 新目录名的实例上，收一份旧名字就是"实例设置打不开"。
    // NOTE: 同样接在 `showPage()` **之前**，理由和上面那条逐字相同。
    connect(&dialog, &InstanceSettingsDialog::fileResourceRequested, m_owner,
            [this, &dialog, &targetId, settingsTab](const QString &pageKey, const QString &path) {
                // 非量不可的时机：下面那一发会把这一页收掉，而返回时开出来的是**另一个
                // 对话框、另一个 InstanceFilesPage** —— 位置只有这一刻还活在盘面上。
                // 摆回去那一段见 `openInstanceSettingsNow()` 里 `showPage()` 之前那一段。
                //
                // NOTE: 按**引用**收 `dialog`，理由和上面那批 lambda 逐字相同：它挂在
                // 那个会话上、活到收页为止（见 targetId 那一段）。右键点一行走的也是这一发
                // （见 InstanceSettingsDialog 那边这段注释），所以两条路都顾到了。
                NavigationReturn origin =
                    NavigationReturn::instanceSettings(settingsTab, targetId, pageKey);
                if (InstanceFilesPage *files = dialog.filesPage(pageKey)) {
                    const InstanceFilesPage::ScrollPosition position = files->scrollPosition();
                    origin.scrollIdentity = position.identity;
                    origin.scrollDelta = position.delta;
                }
                if (origin.hasScroll()) {
                    m_owner->m_instanceReturn = origin;
                }
                m_owner->openInstanceFileResource(targetId, pageKey, path);
            });

    // 右键菜单「复制地址」⇒ 把**完整的**地址放进剪贴板（和「隐藏服务器地址」开不开无关），
    // 在鼠标旁提示一下、写一行日志。
    connect(&dialog, &InstanceSettingsDialog::copyServerAddressRequested, m_owner,
            [this](const QString &address) {
                if (address.isEmpty()) {
                    return;
                }
                if (QClipboard *clipboard = QApplication::clipboard()) {
                    clipboard->setText(address);
                    QToolTip::showText(QCursor::pos(), tr("已复制服务器地址"), nullptr, QRect(), 1500);
                    m_owner->appendLog(tr("已复制服务器地址：%1").arg(address));
                }
            });

    // ---------- 服务器列表那三件会写盘的事（ 搬进 `ServerListController`）----------
    //
    // 改一台 / 加一台 / 删一台的**判断**都搬到那一层了（"一个字节都不写"那道闸、地址必填、
    // 「（无名）」那条显示规矩、以及"动完统一重读重测"）。这一层剩下的是**只有它办得了的
    // 两件事**：问话（`OverlayHost` 在这儿）和重读重测（`pingServers` 要那个对话框）。
    //
    // NOTE: 它是**这一趟的会话对象**（不是成员，和 `m_owner->m_instanceSettings` 那种不一样）：
    // 它那几条回调要用 `&dialog` 和 `&targetId`，而这两个都属于这一趟。它挂在会话上
    // （`InstanceSettingsSession`），和这一页同生共死 —— 这一页不再阻塞开，它要是还留在
    // 栈上，收页之后那三发信号里的 `&serverEdits` 就成了野引用。
    session->serverEdits = new ServerListController(ServerListController::Host{
        [this](ServerListController::Ask which, const QString &initial, QString *out) {
            // **四句问话的文案全在这儿** —— 只有主窗口有 OverlayHost，而且"哪一档说哪句话"
            // 正是这一层该管的事（那一层只说"要问哪一格"）。
            QString title;
            QString label;
            switch (which) {
                case ServerListController::Ask::EditName:
                    title = tr("编辑服务器");
                    label = tr("这一台叫什么？");
                    break;
                case ServerListController::Ask::EditAddress:
                    title = tr("编辑服务器");
                    label = tr("地址是什么？（`主机名:端口`，端口不写就是默认那个）");
                    break;
                case ServerListController::Ask::AddName:
                    title = tr("添加服务器");
                    // 名字可以留空：`servers.dat` 里的 `name` 真能是空串（游戏那边显示成
                    // 未命名），所以这一句得明说"留空也行"，不然用户会以为填不满就不让过。
                    label = tr("这一台叫什么？（可以留空）");
                    break;
                case ServerListController::Ask::AddAddress:
                    title = tr("添加服务器");
                    label = tr("地址是什么？（`主机名:端口`，端口不写就是默认那个）");
                    break;
            }
            bool ok = false;
            const QString answer =
                InlineMessage::getText(m_owner->m_ui->overlay(), title, label, initial, &ok);
            if (ok) {
                *out = answer;
            }
            return ok;
        },
        [this](const QString &who, const QString &address) {
            // 那一句问话里的名字已经过了 `displayName()`（空名字那一档是「（无名）」）——
            // 用户点的是哪一台，问的就是哪一台。
            return InlineMessage::question(
                m_owner->m_ui->overlay(), tr("删除服务器"),
                tr("确定从列表中移除服务器「%1」（%2）？\n\n游戏 servers.dat 中的对应记录将一并移除。")
                    .arg(who, address),
                tr("移除"), tr("取消"), /*defaultYes=*/false, /*destructive=*/true);
        },
        [this](const ServerListController::Notice &notice) { m_owner->showServerListNotice(notice); },
        [this](const QString &text) { m_owner->appendLog(text); },
        [this, &dialog](const QString &id) {
            m_owner->loadServerList(&dialog, id);
            m_owner->pingServers(&dialog, id);
        },
        [this](int count) {
            return InlineMessage::question(
                m_owner->m_ui->overlay(), tr("删除服务器"),
                tr("从列表中删除选中的 %n 个服务器？", nullptr, count),
                tr("移除"), tr("取消"), /*defaultYes=*/false, /*destructive=*/true);
        }});
    // 下面那三条接线按引用收它（和 targetId 同一个理由）—— 引用的必须是会话里那一份。
    ServerListController &serverEdits = *session->serverEdits;

    // 落盘走 `src/manage/ServerListFile`（`NbtUtil` 那一层**故意只读**，见它的文件头）。
    // 认出"要改的是哪一条"靠**按值匹配**（名字 + 地址，退一步只按地址）—— 不能按下标：
    // `NbtUtil::readServerList` 会跳过没有可用 `ip` 的条目，于是界面上第 N 行**不等于**
    // NBT 里第 N 条（见 ServerListFile.hpp 那段）。
    // 那两句话本身（标题 / 那两格问什么 / 要不要预填）在**上面那个 Host 里**
    // —— 这一层只剩"把这一发转给它"。
    connect(&dialog, &InstanceSettingsDialog::editServerRequested, m_owner,
            [&serverEdits, &targetId](const QString &oldName, const QString &oldAddress) {
                serverEdits.edit(targetId, oldName, oldAddress);
            });

    // 「添加」（要求「服务器那个页面里放一个添加服务器的按钮，允许添加」）。
    // 和「编辑」**逐字同一套**：两发 `getText()`（先说名字、再报地址）、落盘走
    // `ServerListFile`、失败用 `InlineMessage::warning` 挡住。差别只有两处：
    // ① 预填的是**空**（要加的那一台还没有名字 / 地址）；② 走的是 `addEntry()` —— 那个
    // 入口在**文件根本不存在**时也不当失败（那正是"这个实例第一次加服务器"）。
    connect(&dialog, &InstanceSettingsDialog::addServerRequested, m_owner,
            [&serverEdits, &targetId]() { serverEdits.add(targetId); });

    // 「删除」（要求「编辑要能改名字改地址以及删除」）。
    // 和「编辑」同一套，多出来的只有**中间那一道问话** —— 这是这一页上唯一一次真会从
    // 用户自己的文件里拿掉东西的操作。
    //
    // 那句问话**必须点名字**（用户点的是哪一台，问的就是哪一台）：`servers.dat` 里
    // 名字是空串是合法的，所以那一档写「（无名）」+ 地址 —— 光说"这一台"的话，两台
    // 没名字的服务器在屏幕上是同一个问题。
    connect(&dialog, &InstanceSettingsDialog::deleteServerRequested, m_owner,
            [&serverEdits, &targetId](const QString &name, const QString &address) {
                serverEdits.remove(targetId, name, address);
            });
    connect(&dialog, &InstanceSettingsDialog::deleteServersRequested, m_owner,
            [&serverEdits, &targetId](const QList<QPair<QString, QString>> &entries) {
                serverEdits.removeMany(targetId, entries);
            });

    // 「加入服务器」（**临时占位**）。和存档那一页那颗走的是同一条路
    // （`m_owner->startQuickPlay()`）—— 差别只有 Kind 和那串值（这里给的就是 `servers.dat` 里写的
    // `host:port`，游戏自己认得这个写法）。
    connect(&dialog, &InstanceSettingsDialog::joinServerRequested, m_owner,
            [this, &targetId](const QString &name, const QString &address) {
                m_owner->appendLog(tr("试着直接加入服务器：%1（%2）")
                              .arg(name.isEmpty() ? tr("（无名）") : name, address));
                m_owner->startQuickPlay(targetId, QuickPlay::Kind::Multiplayer, address);
            });

    // ---------- 「改了就写」那两条----------
    //
    // 这一页没有「取消 / 确定」：写设置全在这两条 lambda 里，
    // 时机是"值真的变了"（对话框那边保证：值没变不发，见 notifyEdited()）。
    //
    // NOTE: `targetId` 是那个会话上的成员（活到收页为止；见 InstanceSettingsSession），
    // 改名成功之后它跟着换 —— 不换的话，改完名之后动任何一个开关都会写到**老目录名**
    // 上，而 updateVersionMeta() 会照着老名字把目录重新建出来，用户手上就多一个只有
    // 一份 wtfml.meta 的空壳实例。
    connect(&dialog, &InstanceSettingsDialog::settingsEdited, m_owner,
            [this, &dialog, &targetId]() { m_owner->applyInstanceSettings(targetId, dialog); });
    connect(&dialog, &InstanceSettingsDialog::gameIconChoicesRequested, &dialog,
            [this, &dialog, &targetId] {
        QStringList jars;
        const QString gameVersion = m_owner->m_versionManager->instanceGameVersion(targetId);
        if (!gameVersion.isEmpty() && QFileInfo::exists(AppPaths::versionJarPath(gameVersion))) {
            jars.append(AppPaths::versionJarPath(gameVersion));
        }
        const QString instanceJar = InstanceFiles::jarPath(targetId);
        if (QFileInfo::exists(instanceJar) && !jars.contains(instanceJar)) jars.append(instanceJar);
        using Choices = QList<QPair<QString, QImage>>;
        auto *watcher = new QFutureWatcher<Choices>(&dialog);
        connect(watcher, &QFutureWatcher<Choices>::finished, &dialog, [&dialog, watcher] {
            const auto choices = watcher->result();
            watcher->deleteLater();
            dialog.setGameIconChoices(choices);
        });
        watcher->setFuture(QtConcurrent::run([jars] { return InstanceFiles::readGameItemIcons(jars); }));
    });

    // 改名：**排在写设置之外自己一条**。
    // 两件事不能合成一条 —— 改名的副作用是**整个实例换了个目录**，写设置必须发生在它
    // 之后（顺序反了就是上面那个空壳实例）。对话框那边也把这两件事分成了两条信号：
    // 那一格走 `editingFinished`（回车 / 失焦），正好是"这次改名想清楚了"的那一刻。
    connect(&dialog, &InstanceSettingsDialog::renameRequested, m_owner,
            [this, &dialog, &targetId, &refreshInstanceDirs](const QString &name) {
                const QString wanted = name.trimmed();
                // 空 = 用户把那一格清空了（不是"想改成没有名字"），等于没改。
                if (wanted.isEmpty() || wanted == targetId) {
                    dialog.setInstanceName(targetId);
                    return;
                }
                if (!m_owner->applyRename(targetId, wanted)) {
                    // 失败要把那一格退回原样。不退回的话，用户眼前那一格写着新名字、
                    // 而磁盘上还是老名字 —— 他接着动任何一个开关，那一次写入就落到一个
                    // **不存在的目录**上（updateVersionMeta() 会照着它把目录建出来）。
                    // m_owner->applyRename() 已经把原因弹出来了，这里只负责让界面别撒谎。
                    dialog.setInstanceName(targetId);
                    return;
                }
                // 改完名之后写的是**新目录**（m_owner->applyRename() 里那句 m_owner->refreshInstalledVersions()
                // 已经把列表和下拉框都换到新名字上了）。
                targetId = wanted;
                dialog.setInstanceName(targetId);
                // 「文件管理」那八页说的也是**这个实例的目录**，整份跟着重算 ——
                // 重算之前它们还指着那个已经不在了的老目录（见上面那个 lambda 的 NOTE）。
                refreshInstanceDirs(targetId);
                // 页内页眉那行大字也是"这个实例叫什么"，跟着换（覆盖层面包屑只写「实例设置」，不带实例名）。
                dialog.setInstanceDisplayName(m_owner->instanceDisplayName(targetId));
            });

    // ---------- 停在哪一页 ----------
    //
    // 摆在这一段**最后、`showPage()` 之前**有两条理由：
    // ① `showPage()` 换不了"左栏里没有的那几页"（被用户勾掉的、以及「配置文件」那种没
    // 入口的），而"哪几页在左栏里"是上面 `setPageChoices()` 定的 ⇒ 必须在它**之后**；
    // ② "停在哪一页"只能由摆上去那一句本身定 —— 页一摆上去用户当场就能看、能点，摆完了
    // 再补一次换页就是当着用户的面翻一次。
    // 认不出来的键 `showPage()` 自己判 -1 什么都不做（停「概览」）—— 不静默出错。
    // **回去**时把那一页的滚动位置塞回去。两条来路都量在离开那一刻：
    // ① 从资源详情页返回（`fileResourceRequested` 那一条接线）；
    // ② 从下载页按「← 返回实例设置」（那颗键把来路整个挪到 `m_instanceReturn` 上，
    //    见 `MainWindow::returnToInstanceSettingsFromDownload()`）。
    //
    // 必须排在下面那句 `showPage()` **之前**：`showPage()` 会把那一页列出来，而
    // "列出来"这一趟的收尾就是 `restoreScrollAnchor()` —— 塞晚了这一趟已经落回顶端了，
    // 而且那份锚点是**一次性的**（还完就丢，见那一支）。
    //
    // 对不上就整份作废（换了实例 / 开的是另一页 ⇒ 落回顶端才是对的）；不管用没用上，
    // 到这儿都清掉 —— 这一趟之后它的使命就结束了，留着的话下一次开实例设置会拿一份
    // 过期的位置去摆（而那时用户多半已经翻到别处去了）。
    if (m_owner->m_instanceReturn.hasScroll() &&
        m_owner->m_instanceReturn.instanceId == targetId && !pageKey.isEmpty() &&
        m_owner->m_instanceReturn.pageKey == pageKey) {
        if (InstanceFilesPage *files = dialog.filesPage(pageKey)) {
            InstanceFilesPage::ScrollPosition position;
            position.identity = m_owner->m_instanceReturn.scrollIdentity;
            position.delta = m_owner->m_instanceReturn.scrollDelta;
            files->setScrollPosition(position);
        }
    }
    m_owner->m_instanceReturn = NavigationReturn();

    if (!pageKey.isEmpty()) {
        dialog.showPage(pageKey);
    }

    // ---------- 保持入口所属的选项卡 ----------
    //
    // 主页快捷入口归主页，避免切页时清掉主页的筛选与列表状态；其它入口归实例页。
    // 先切根页再打开覆盖层，保证覆盖层与根页的归属一致。
    //
    // NOTE: "让位"那一段（收掉占着的那一层、静默继续）在 `m_owner->openInstanceSettings()` 里、
    // 这一半之前 —— 走到这儿时这一层**一定是空的**，所以切选项卡不会顺手把用户翻到
    // 别的选项卡上去（那颗闸弹话的时候就不该翻页）。
    if (QTabWidget *tabs = m_owner->m_ui->getTabWidget()) {
        if (settingsTab >= 0 && settingsTab < tabs->count()) {
            tabs->setCurrentIndex(settingsTab);
        }
    }

    // 拖进来的文件：这一页**认不出**一个 jar 是什么、
    // 也够不着盘 —— `ui/` 不 include `src/`（那条分层约定见 InstanceSettingsDialog.hpp
    // 里 dragEnterEvent 那段）。所以它只发一句"拖进来这几个路径"，认类型、问那一句
    // "是否继续"、把文件复制进目录，**全在 MainWindow 这一层**（见 handleInstanceSettingsDrop）。
    //
    // NOTE: `targetId` 按**引用**收 —— 改完名这一页还开着，那时拖进来的东西必须落进新目录
    // （同上面那些 lambda 的理由，见 targetId 那一段）。
    connect(&dialog, &InstanceSettingsDialog::filesDropped, m_owner,
            [this, &dialog, &targetId](const QStringList &paths) {
                m_owner->handleInstanceSettingsDrop(&dialog, targetId, paths);
            });

    // 这一页盖在内容区上（标签栏和底栏都留着），左上角那颗返回箭头 = 关掉这一页（见
    // OverlayHost::showPage）。    //
    // NOTE: 这一页**不再 accept**（没有「确定」了），所以这里**不看返回值** —— 关页
    // 不等于放弃（该写的早就在上面那些 lambda 里写完了），"关掉"就是"关掉"。
    // 「导出为整合包…」那颗按钮是唯一的例外：它把自己 `reject()` 掉，收页之后由下面
    // 那条钩子接过去开预设编辑器（这一位放在会话里，钩子读得到）。
    bool &exportPreset = session->exportPreset;
    connect(&dialog, &InstanceSettingsDialog::exportPresetRequested, &dialog, [&dialog, &exportPreset] { exportPreset = true; dialog.reject(); });

    // 摆上去就返回（不再是 `execPage()` 那个嵌套事件循环）。收页之后的两件事都接在
    // `pageClosed(QWidget *)` 上 —— 所有收页路（返回箭头 / Esc / 页上自己的操作 /
    // `dismiss()` / `evict()`）都汇到 `closeOverlay()`，钩子只发那一处。
    // 上下文对象用 `dialog`：这一页收掉之后它 `deleteLater()`，连接跟着一起没。
    OverlayHost *overlay = m_owner->m_ui->overlay();
    connect(overlay, &OverlayHost::pageClosed, &dialog,
            [this, &dialog, session](QWidget *closed) {
                if (closed != &dialog) {
                    return;
                }
                // 「导出为整合包…」：收页那一发是这条钩子自己排的，当场再去开预设编辑器
                // 等于在收尾途中又进一层 —— 排到下一轮（和 `openFileResourcePage` 那条
                // 回调同一个手法）。
                if (session->exportPreset) {
                    const QString id = session->targetId;
                    QTimer::singleShot(0, m_owner, [this, id] {
                        m_owner->m_ui->getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
                        m_owner->m_ui->setInstanceMode(MainWindowUI::InstanceMode::Preset);
                        m_owner->m_presetController->open();
                        m_owner->m_presetController->captureForEditor(id);
                    });
                }
                // 这一页每次开都是新的一份；收掉之后就还回去（会话和它那几个局部量挂在
                // 它身上，一起走）。
                dialog.deleteLater();
            },
            Qt::SingleShotConnection);
    // 这一页收掉之后是**我们自己** `deleteLater()`（见上面那条 `pageClosed` 钩子）⇒ 告诉
    // 覆盖层别再把它"还回寄存处"：收页时换一次父容器要让整棵控件树重算继承的样式和调色板，
    // 实测 75 ms，而这份树紧接着就要被删掉。
    dialog.setProperty(OverlayHost::disposesOnCloseProperty(), true);
    QElapsedTimer showClock;
    showClock.start();
    if (!overlay->showPage(&dialog, dialog.titleText(), settingsTab)) {
        // 摆不上去（覆盖层刚被另一页占上了；正常走不到：入口已经先
        // `runWhenOverlayFree()` 让过位）。这一趟就当没开过 —— 会话挂在对话框上，
        // 一起收掉。
        dialog.deleteLater();
    } else {
        qInfo() << "[UI timing] overlay show settings" << showClock.elapsed() << "ms";
        QTimer::singleShot(0, &dialog, [showClock]() mutable {
            qInfo() << "[UI timing] settings first idle" << showClock.elapsed() << "ms";
        });
    }
}





/**
 * @file PresetController.cpp
 * @brief 实现 PresetController.hpp。
 *
 * 这一份里只有三类句子：**读盘**（`Presets::loadManifest` / `readTargetSnapshot`）、
 * **把结果翻成页面认识的形状**、以及**把页面发上来的那一下交给 `Presets::` 那一层**。
 * 判据一条都不在这儿 —— 段怎么落、什么时候要确认、备份什么，全在 `src/presets/`。
 */

#include "controllers/PresetController.hpp"
#include "TextMarks.hpp"

#include "PresetManagePage.hpp"
#include "PresetEditorPage.hpp"
#include "Theme.hpp"
#include "presets/PresetWorkspace.hpp"
#include <QLineEdit>
#include <QJsonObject>
#include <QLabel>
#include "PresetModEditor.hpp"
#include "PresetModReview.hpp"
#include "PresetOptionsEditor.hpp"
#include "presets/PresetModInstaller.hpp"
#include "presets/PresetModTargets.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "manage/InstanceFiles.hpp"
#include "presets/PresetManifestLoader.hpp"
#include "presets/PresetPreviewLoader.hpp"
#include <QPushButton>
#include <QCryptographicHash>
#include "manage/VersionManager.hpp"
#include "presets/DataPackPreset.hpp"
#include "presets/GenericFiles.hpp"
#include "presets/MalilibKeys.hpp"
#include "presets/ModrinthCompatibility.hpp"
#include "presets/PresetApply.hpp"
#include "presets/PresetExecutionReport.hpp"
#include "presets/PresetCompatibility.hpp"
#include "presets/PresetPlanner.hpp"
#include "presets/PresetStore.hpp"
#include "presets/PresetCatalog.hpp"
#include "download/ResourceCatalog.hpp"
#include "util/AppPaths.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFutureWatcher>
#include <QLayout>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

// Embedded pages must not inherit QDialog's Escape/Enter dismissal behavior.
class EmbeddedPresetManagePage final : public PresetManagePage {  // surface-registry-ignore: 嵌在预设控制器里的副本，随 preset-manage 那个表面一起扫
public:
    explicit EmbeddedPresetManagePage(QWidget *parent) : PresetManagePage(parent) {}
    QSize minimumSizeHint() const override { return QSize(0,0); }
    void accept() override {}
    void reject() override {}
};

/**
 * @brief 「模组」这一类预设的内容：把实例里**认得出平台身份**的模组并进 @p into。
 *
 * 这一类没有可复制的文件段（`captureMaskFor(Mods)` 是 0），它的内容就是引用表。
 * 身份来自盘上已有的安装记录 + 逐件 sha1 核对（见 `captureInstalledModReferences()`），
 * 所以离线也能收，且换过内容的 jar 不会被当成原来那个项目。
 *
 * @param addedOut 这次新并进去几项（已有的按 key 去重，不算新增）。
 * @param unidentifiedOut 有 jar 但认不出身份的件数。
 * @return 失败时一句给用户看的话；成功时空串。
 */
QString mergeCapturedModReferences(const QString &instanceRoot,
                                   QList<Presets::ModReference> *into, int *addedOut,
                                   int *unidentifiedOut) {
    int unidentified = 0;
    QString problem;
    const QList<Presets::ModReference> found =
        Presets::captureInstalledModReferences(instanceRoot, &unidentified, &problem);
    if (unidentifiedOut != nullptr) {
        *unidentifiedOut = unidentified;
    }
    if (!problem.isEmpty()) {
        return problem;
    }
    QSet<QString> existing;
    for (const auto &reference : *into) {
        existing.insert(reference.key());
    }
    int added = 0;
    for (const auto &reference : found) {
        const QString key = reference.key();
        if (key.isEmpty() || existing.contains(key)) {
            continue;
        }
        existing.insert(key);
        into->append(reference);
        ++added;
    }
    if (addedOut != nullptr) {
        *addedOut = added;
    }
    return QString();
}

Presets::CaptureSelection toSelection(const PresetManagePage::CaptureChoice &choice) {
    Presets::CaptureSelection selection;
    selection.vanillaOptions = choice.vanillaOptions;
    selection.vanillaKeys = choice.vanillaKeys;
    selection.servers = choice.servers;
    selection.config = choice.config;
    selection.defaultConfigs = choice.defaultConfigs;
    selection.resourcePacks = choice.resourcePacks;
    selection.shaderPacks = choice.shaderPacks;
    selection.malilib = choice.malilib;
    selection.malilibValues = choice.malilibValues;
    selection.dataPacks = choice.dataPacks;
    return selection;
}

/// 段种类 → 显示文案。★ 只有这一处（页面那一层不认识 `SectionKind`）。
QString kindTextOf(Presets::SectionKind kind) {
    switch (kind) {
    case Presets::SectionKind::VanillaOptions:
        return PresetController::tr("游戏设置");
    case Presets::SectionKind::VanillaKeys:
        return PresetController::tr("原版按键");
    case Presets::SectionKind::ServerList:
        return PresetController::tr("服务器列表");
    case Presets::SectionKind::MalilibKeys:
        return PresetController::tr("MaLiLib 热键");
    case Presets::SectionKind::GenericFiles:
        return PresetController::tr("通用文件组");
    case Presets::SectionKind::Custom:
        break;
    }
    return PresetController::tr("自定义");
}

/// 筛选项那把钥匙。★ `Unset` 在盘上**没有**类别字段（`presetCategoryKey()` 回空串），
/// 而空串在页面上是「全部」那一档 —— 所以旧数据在筛选这一层另起一个不落盘的钥匙。
QString categoryFilterKey(Presets::PresetCategory category) {
    if (category == Presets::PresetCategory::Unset) {
        return QStringLiteral("legacy");
    }
    return Presets::presetCategoryKey(category);
}

QString sectionKindText(const Presets::PresetSection &section) {
    if (Presets::dataPackHandlesSection(section)) {
        return PresetController::tr("数据包");
    }
    if (section.kind == Presets::SectionKind::GenericFiles) {
        const QString subKey = Presets::fileGroupDirName(Presets::FileGroup::Config);
        if (section.id == Presets::sectionIdFor(section.kind, subKey)) {
            return PresetController::tr("配置文件 config/");
        }
        if (section.id == Presets::sectionIdFor(
                                section.kind,
                                Presets::fileGroupDirName(Presets::FileGroup::DefaultConfigs))) {
            return PresetController::tr("默认配置 defaultconfigs/");
        }
        if (section.id == Presets::sectionIdFor(
                                section.kind,
                                Presets::fileGroupDirName(Presets::FileGroup::ResourcePacks))) {
            return PresetController::tr("资源包");
        }
        if (section.id == Presets::sectionIdFor(
                                section.kind,
                                Presets::fileGroupDirName(Presets::FileGroup::ShaderPacks))) {
            return PresetController::tr("光影包");
        }
        return kindTextOf(section.kind);
    }
    if (section.kind == Presets::SectionKind::MalilibKeys) {
        return kindTextOf(section.kind);
    }
    return kindTextOf(section.kind);
}

}  // namespace

PresetController::PresetController(VersionManager *versions, QWidget *hostPage, Host host,
                                   QObject *parent)
    : QObject(parent), m_versions(versions), m_hostPage(hostPage), m_host(std::move(host)) {
    m_catalogLoader = new Presets::CatalogLoader(this, m_host.readCatalog ? m_host.readCatalog : Presets::CatalogLoader::Reader([](const QString &root) {
        auto catalog = Presets::readCatalog(root);
        auto drafts = Presets::list(root + QStringLiteral("/drafts"));
        for (auto draft : drafts) { draft.id.prepend(QStringLiteral("draft:")); catalog.presets.append(draft); }
        return catalog;
    }));
    m_manifestLoader = new Presets::ManifestLoader(this, m_host.readManifest);
    m_previewLoader = new Presets::PreviewLoader(this, m_host.readPreview);
    // 兼容检查那一套：真网络提供方 + 共享缓存 + 检查器。★ 缓存与 checker 都活到这一层
    // 结束，所以"同一批模组在两个弹窗里连着看两次"第二趟是缓存结论（「共享缓存」）。
    //
    // 根地址问 VersionManager（它认 `Launcher/ModrinthApiBase`）—— 原来这里写死了一份
    // 官方地址，夹具换不掉它：将来任何用 PresetController 的兼容检查测试都会静默打到真
    // Modrinth，而断言是全绿的。
    const QString modrinthRoot =
        m_versions != nullptr ? m_versions->modrinthApiRoot()
                              : ResourceCatalog::modrinthApiRoot();
    m_modProvider = new Presets::PlatformCompatibilityProvider(QUrl(modrinthRoot), this);
    m_modChecker = new Presets::CompatibilityChecker(&m_modCache, this);
    m_modChecker->setProvider(m_modProvider);
    const auto push = [this](const QList<Presets::CompatibilityResult> &results,
                             const Presets::CompatibilitySummary &summary, bool finished) {
        ModCheckUpdate update;
        update.presetId = m_modCheckPresetId;
        update.generation = m_modChecker->generation();
        update.results = results;
        update.summary = summary;
        update.finished = finished;
        emit modCheckUpdated(update);
    };
    connect(m_modChecker, &Presets::CompatibilityChecker::updated, this,
            [push](const QList<Presets::CompatibilityResult> &results,
                   const Presets::CompatibilitySummary &summary) { push(results, summary, false); });
    connect(m_modChecker, &Presets::CompatibilityChecker::finished, this,
            [push](const QList<Presets::CompatibilityResult> &results,
                   const Presets::CompatibilitySummary &summary) { push(results, summary, true); });
}

PresetController::~PresetController() = default;

QString PresetController::presetsRoot() {
    return Presets::presetsRoot();
}

// ---------------------------------------------------------------------------
// 打开
// ---------------------------------------------------------------------------

void PresetController::open() {
    if (m_hostPage == nullptr) {
        return;
    }
    buildPage();
    pushInstances();
    m_manifestLoader->invalidate();
    invalidatePreview();
    m_page->setBusy(true);
    m_page->showStatus(tr("正在读取预设…"));
    m_catalogLoader->load(presetsRoot(), [this](const Presets::CatalogSnapshot &snapshot) {
        if (!m_page) return;
        if (snapshot.root != presetsRoot()) {
            open();
            return;
        }
        updatePageBusy();
        m_page->showStatus(QString());
        pushCatalog(snapshot, m_currentId);
    });
}

bool PresetController::catalogLoading() const {
    return m_catalogLoader->isLoading();
}

bool PresetController::detailsLoading() const {
    return m_manifestLoader->isLoading() || m_manifestLoader->isReading();
}

bool PresetController::previewLoading() const {
    return m_previewLoader->isLoading() || m_previewLoader->isReading();
}

void PresetController::updatePageBusy() {
    if (m_page) m_page->setBusy(m_catalogLoader->isLoading() || m_manifestLoader->isLoading() || m_previewLoader->isLoading());
}

void PresetController::invalidatePreview() {
    m_previewLoader->invalidate();
    if (m_page) m_page->setPlanLoading(false);
    updatePageBusy();
}

void PresetController::buildPage() {
    if (m_page != nullptr) {
        return;
    }
    m_page = new EmbeddedPresetManagePage(m_hostPage);
    // QDialog remains useful to standalone callers, but the management page is
    // always a child widget here. A parent alone does not remove Qt::Dialog.
    m_page->setWindowFlags(Qt::Widget);
    m_page->setWindowModality(Qt::NonModal);
    m_page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // 页面是实例页第三档的常驻内容（不是弹出来的覆盖页）：塞进宿主里，跟着换页容器一起露/收。
    auto *hostLayout = qobject_cast<QVBoxLayout *>(m_hostPage->layout());
    if (hostLayout != nullptr) {
        hostLayout->addWidget(m_page);
    } else {
        auto *layout = new QVBoxLayout(m_hostPage);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_page);
    }

    m_page->show();
    pushCategories();

    connect(m_page, &PresetManagePage::presetSelected, this, &PresetController::selectPreset);
    connect(m_page, &QObject::destroyed, this, [this] { cancelModCheck(); });
    connect(m_page, &PresetManagePage::executionReportRequested, this, &PresetController::showExecutionReport);
    connect(m_page, &PresetManagePage::resumeRequested, this, [this](const QString &preset, const QString &instance) {
        applyPlanImpl(preset, instance, true, true);
    });
    connect(m_page, &PresetManagePage::createRequested, this, [this](const QString &) { openEditor(); });
    if (auto *combo = m_page->findChild<QComboBox *>(PresetManagePage::newCategoryComboName())) combo->hide();
    if (auto *label = m_page->findChild<QLabel *>(QStringLiteral("presetNewCategoryLabel"))) label->hide();
    auto *editDocument = m_page->findChild<QPushButton *>(PresetManagePage::saveDetailsButtonName());
    if (editDocument) {
        disconnect(editDocument,nullptr,m_page,nullptr);
        editDocument->setText(tr("编辑预设"));
        Theme::markButton(editDocument,Theme::ButtonKind::Primary);
        connect(editDocument, &QPushButton::clicked, this, [this] { if (!m_currentId.isEmpty()) openEditor(m_currentId); });
    }
    if (auto *name = m_page->findChild<QLineEdit *>(PresetManagePage::nameEditName())) name->setReadOnly(true);
    if (auto *description = m_page->findChild<QLineEdit *>(PresetManagePage::descriptionEditName())) description->setReadOnly(true);
    connect(m_page, &PresetManagePage::duplicateRequested, this,
            &PresetController::duplicatePreset);
    connect(m_page, &PresetManagePage::detailsEdited, this,
            [this](const QString &id, const QString &name, const QString &description) {
                saveDetails(id, name, description);
            });
    connect(m_page, &PresetManagePage::deleteRequested, this, &PresetController::removePreset);
    connect(m_page, &PresetManagePage::editOptionsRequested, this, [this](const QString &id) { openEditor(id); });
    connect(m_page, &PresetManagePage::editModsRequested, this, [this](const QString &id) { openEditor(id); });
    connect(m_page, &PresetManagePage::defaultRequested, this,
            &PresetController::setDefaultPreset);
    connect(m_page, &PresetManagePage::moveRequested, this, &PresetController::movePreset);
    connect(m_page, &PresetManagePage::captureRequested, this,
            [this](const QString &presetId, const QString &instanceId,
                   const PresetManagePage::CaptureChoice &choice) {
                Q_UNUSED(presetId); Q_UNUSED(choice); captureForEditor(instanceId);
            });
    connect(m_page, &PresetManagePage::captureAsNewRequested, this,
            [this](const QString &instanceId, const QString &name, const QString &categoryKey,
                   const PresetManagePage::CaptureChoice &choice) {
                Q_UNUSED(name); Q_UNUSED(categoryKey); Q_UNUSED(choice); captureForEditor(instanceId);
            });
    connect(m_page, &PresetManagePage::planRequested, this, &PresetController::previewPlan);
    connect(m_page, &PresetManagePage::applyTargetChanged, this, [this] { invalidatePreview(); });
    connect(m_page, &PresetManagePage::previewCancelRequested, this, [this] {
        invalidatePreview();
        if (m_page) m_page->showStatus(tr("已取消预览，实例未修改。"));
    });
    connect(m_page, &PresetManagePage::applyRequested, this, &PresetController::applyPlan);
}

void PresetController::pushInstances() {
    if (m_page == nullptr) {
        return;
    }
    m_page->setInstances(instanceItems());
}

void PresetController::refreshInstances() {
    pushInstances();
}

QString PresetController::categoryText(Presets::PresetCategory category) {
    switch (category) {
    case Presets::PresetCategory::Unset:
        return tr("旧版（未分类）");
    case Presets::PresetCategory::GameSettings:
        return tr("游戏设置");
    case Presets::PresetCategory::Mods:
        return tr("模组");
    case Presets::PresetCategory::ResourcePacks:
        return tr("资源包");
    case Presets::PresetCategory::DataPacks:
        return tr("数据包");
    case Presets::PresetCategory::ShaderPacks:
        return tr("光影包");
    }
    return tr("旧版（未分类）");
}

void PresetController::pushCategories() {
    if (m_page == nullptr) {
        return;
    }
    QList<PresetManagePage::CategoryOption> options;

    PresetManagePage::CategoryOption all;
    all.text = tr("全部");
    all.filterOnly = true;
    options.append(all);

    const Presets::PresetCategory creatable[] = {
        Presets::PresetCategory::GameSettings,
        Presets::PresetCategory::Mods,
        Presets::PresetCategory::ResourcePacks,
        Presets::PresetCategory::DataPacks,
        Presets::PresetCategory::ShaderPacks,
    };
    for (const Presets::PresetCategory category : creatable) {
        PresetManagePage::CategoryOption option;
        option.key = categoryFilterKey(category);
        option.text = categoryText(category);
        options.append(option);
    }

    // 旧版混合预设（没写类别）：能看、能用、能复制，但不能**新做**出这一类。
    PresetManagePage::CategoryOption legacy;
    legacy.key = categoryFilterKey(Presets::PresetCategory::Unset);
    legacy.text = tr("旧版");
    legacy.filterOnly = true;
    options.append(legacy);

    options.append({QStringLiteral("composite"), tr("综合预设"), true});
    options.append({QStringLiteral("draft"), tr("草稿"), true});
    m_page->setCategories(options);
}

quint32 PresetController::captureMaskFor(Presets::PresetCategory category) {
    using Bits = PresetManagePage;
    switch (category) {
    case Presets::PresetCategory::Unset:
        return PresetManagePage::captureMaskAll();
    case Presets::PresetCategory::GameSettings:
        return Bits::CaptureVanillaOptions | Bits::CaptureVanillaKeys | Bits::CaptureServers |
               Bits::CaptureConfig | Bits::CaptureDefaultConfigs | Bits::CaptureMalilib |
               Bits::CaptureMalilibValues;
    case Presets::PresetCategory::ResourcePacks:
        return Bits::CaptureResourcePacks;
    case Presets::PresetCategory::ShaderPacks:
        return Bits::CaptureShaderPacks;
    case Presets::PresetCategory::DataPacks:
        // 数据包只收世界里的那一坨；模组类仍然没有可提取的段。
        return Bits::CaptureDataPacks;
    case Presets::PresetCategory::Mods:
        return 0;
    }
    return PresetManagePage::captureMaskAll();
}

QString PresetController::captureNoteFor(Presets::PresetCategory category) {
    switch (category) {
    case Presets::PresetCategory::Unset:
        return tr("旧版预设没写类别，九样都能收。");
    case Presets::PresetCategory::GameSettings:
        return tr("「游戏设置」预设不收资源包 / 光影包。");
    case Presets::PresetCategory::ResourcePacks:
        return tr("仅提取资源包。");
    case Presets::PresetCategory::ShaderPacks:
        return tr("仅提取光影包。");
    case Presets::PresetCategory::Mods:
        return tr("模组预设请使用“编辑模组清单”添加平台项目，当前不从实例提取本地 JAR。");
    case Presets::PresetCategory::DataPacks:
        return tr("数据包按存档应用；目标中没有同名存档时标记为「待应用」。");
    }
    return QString();
}

Presets::CaptureSelection PresetController::filterCapture(
    Presets::PresetCategory category, const Presets::CaptureSelection &selection,
    QStringList *skipped) {
    const quint32 mask = captureMaskFor(category);
    Presets::CaptureSelection out = selection;
    auto drop = [mask, skipped](quint32 bit, bool &flag, const QString &label) {
        if ((mask & bit) == 0 && flag) {
            flag = false;
            if (skipped != nullptr) {
                skipped->append(label);
            }
        }
    };
    using Bits = PresetManagePage;
    drop(Bits::CaptureVanillaOptions, out.vanillaOptions, tr("基础设置"));
    drop(Bits::CaptureVanillaKeys, out.vanillaKeys, tr("标准按键"));
    drop(Bits::CaptureServers, out.servers, tr("服务器列表"));
    drop(Bits::CaptureConfig, out.config, QStringLiteral("config/"));
    drop(Bits::CaptureDefaultConfigs, out.defaultConfigs, QStringLiteral("defaultconfigs/"));
    drop(Bits::CaptureResourcePacks, out.resourcePacks, tr("资源包"));
    drop(Bits::CaptureShaderPacks, out.shaderPacks, tr("光影包"));
    drop(Bits::CaptureMalilib, out.malilib, tr("MaLiLib 热键"));
    drop(Bits::CaptureMalilibValues, out.malilibValues, tr("MaLiLib 值"));
    drop(Bits::CaptureDataPacks, out.dataPacks, tr("数据包"));
    return out;
}

QString PresetController::instanceRoot(const QString &instanceId) const {
    return AppPaths::versionDir(instanceId);
}

QString PresetController::displayName(const QString &instanceId) const {
    if (m_host.displayName) {
        const QString name = m_host.displayName(instanceId);
        if (!name.isEmpty()) {
            return name;
        }
    }
    return instanceId;
}

void PresetController::log(const QString &message) const {
    if (m_host.log) {
        m_host.log(message);
    }
}

QList<PresetManagePage::InstanceItem> PresetController::instanceItems() const {
    QList<PresetManagePage::InstanceItem> items;
    if (m_versions == nullptr) {
        return items;
    }
    for (const QString &id : m_versions->installedVersions()) {
        PresetManagePage::InstanceItem item;
        item.id = id;
        item.name = displayName(id);
        item.running = m_host.isRunning ? m_host.isRunning(id) : false;
        items.append(item);
    }
    return items;
}

// ---------------------------------------------------------------------------
// 列表
// ---------------------------------------------------------------------------

void PresetController::refreshList(const QString &selectId) {
    if (m_page == nullptr) {
        return;
    }
    pushListAndSelect(selectId);
}

void PresetController::pushListAndSelect(const QString &selectId) {
    m_catalogLoader->invalidate();
    m_manifestLoader->invalidate();
    invalidatePreview();
    m_page->setBusy(true);
    m_catalogLoader->load(presetsRoot(), [this, selectId](const Presets::CatalogSnapshot &snapshot) {
        if (!m_page) return;
        if (snapshot.root != presetsRoot()) {
            pushListAndSelect(selectId);
            return;
        }
        updatePageBusy();
        // A mutation may already have shown a success/warning message. Keep it.
        pushCatalog(snapshot, selectId);
    });
}

void PresetController::pushCatalog(const Presets::CatalogSnapshot &snapshot, const QString &selectId) {
    const auto &summaries = snapshot.presets;
    // 页头路径行和实例页同一个形状：「<预设目录>（N 份预设）」。
    if (m_host.presetSubtitle) {
        m_host.presetSubtitle(tr("%1（%2 份预设）")
                                  .arg(QDir::toNativeSeparators(presetsRoot()))
                                  .arg(summaries.size()));
    }
    m_catalogNames.clear();
    for (const auto &summary : summaries) m_catalogNames.insert(summary.name);

    QList<PresetManagePage::PresetItem> items;
    for (const Presets::PresetSummary &summary : summaries) {
        PresetManagePage::PresetItem item;
        item.id = summary.id;
        item.name = summary.name;
        item.description = summary.description;
        if (summary.updatedAt.isValid()) {
            item.updatedText =
                tr("最后修改：%1")
                    .arg(summary.updatedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        }
        item.sectionCount = summary.sectionCount;
        item.isDefault = summary.isDefault;
        item.categoryKey = categoryFilterKey(summary.category);
        item.categoryText = categoryText(summary.category);
        item.categoryKeys = summary.componentCategories;
        item.iconPath = summary.iconPath;
        if (!summary.componentCategories.isEmpty()) { item.categoryKey = QStringLiteral("composite"); item.categoryText = tr("综合预设"); }
        if (summary.id.startsWith(QStringLiteral("draft:"))) { item.categoryKey = QStringLiteral("draft"); item.categoryText = tr("草稿"); item.name += tr("（草稿）"); }
        items.append(item);
    }

    QList<PresetManagePage::BrokenItem> brokenItems;
    for (const Presets::BrokenPreset &one : snapshot.broken) {
        PresetManagePage::BrokenItem item;
        item.id = one.id;
        item.reason = one.reason;
        brokenItems.append(item);
    }

    QString keep = selectId;
    if (keep.isEmpty()) {
        for (const auto &item : items) {
            if (item.isDefault) { keep = item.id; break; }
        }
    }
    QString chosen;
    for (const PresetManagePage::PresetItem &item : items) {
        if (item.id == keep) {
            chosen = keep;
            break;
        }
    }
    if (chosen.isEmpty() && !items.isEmpty()) {
        chosen = items.first().id;
    }

    m_page->setPresets(items, brokenItems, chosen);
    // Search/category filtering may hide the requested id. Actions and details
    // must follow the actual visible selection, not an invisible fallback.
    chosen = m_page->currentPresetId();
    if (chosen.isEmpty()) {
        selectPreset(QString());
        return;
    }
    selectPreset(chosen);
}

void PresetController::selectPreset(const QString &id) {
    if (m_editor) return;
    if (m_page == nullptr) {
        return;
    }
    if (id.startsWith(QStringLiteral("draft:"))) {
        m_currentId=id; invalidatePreview();
        const auto page=m_page;
        m_manifestLoader->load(presetsRoot()+QStringLiteral("/drafts"),id.mid(6),[this,page,id](const Presets::ManifestSnapshot &snapshot) {
            if(!page||m_currentId!=id)return;
            updatePageBusy();
            if(snapshot.ok)page->showPreset(id,snapshot.manifest.name,QString(),tr("草稿"),sectionItems(snapshot.manifest));
            else page->showPresetReadError(snapshot.problem);
        });
        return;
    }
    m_currentId = id;
    invalidatePreview();
    if (id.isEmpty()) {
        m_manifestLoader->invalidate();
        updatePageBusy();
        return;
    }
    pushPresetToPage(id);
}

void PresetController::pushPresetToPage(const QString &id) {
    const QPointer<PresetManagePage> page = m_page;
    page->setBusy(true);
    page->showPresetLoading(id);
    m_manifestLoader->load(presetsRoot(), id, [this, page](const Presets::ManifestSnapshot &snapshot) {
        if (!page || page != m_page) return;
        if (snapshot.root != presetsRoot()) {
            open();
            return;
        }
        updatePageBusy();
        if (snapshot.id != m_currentId || snapshot.id != page->currentPresetId()) return;
        if (!snapshot.ok) {
            page->showPresetReadError(snapshot.problem);
            return;
        }
        const auto &manifest = snapshot.manifest;
        page->showPreset(snapshot.id, manifest.name, manifest.description, categoryText(manifest.category),
                       sectionItems(manifest));
        if (auto *edit = page->findChild<QPushButton *>(QStringLiteral("presetEditModsButton")))
            edit->hide();
        if (auto *edit = page->findChild<QPushButton *>(QStringLiteral("presetEditOptionsButton")))
            edit->hide();
        page->setCaptureAvailability(captureMaskFor(manifest.category) & ~(PresetManagePage::CaptureConfig | PresetManagePage::CaptureDefaultConfigs | PresetManagePage::CaptureMalilib | PresetManagePage::CaptureMalilibValues),
                                   tr("仅提取游戏设置、按键和资源清单。"));
    });
}

QList<PresetManagePage::SectionItem> PresetController::sectionItems(
    const Presets::PresetManifest &manifest) const {
    QList<PresetManagePage::SectionItem> items;
    if (!manifest.modReferences.isEmpty()) {
        PresetManagePage::SectionItem item;
        item.title = tr("平台模组清单"); item.kindText = tr("模组");
        item.modeText = tr("按目标版本解析"); item.itemCount = manifest.modReferences.size();
        items.append(item);
    }
    for (const Presets::PresetSection &section : manifest.sections) {
        PresetManagePage::SectionItem item;
        item.title = section.title;
        item.kindText = sectionKindText(section);
        item.modeText = modeText(section.mode);
        item.itemCount = static_cast<int>(section.items.size());
        if (item.title.isEmpty()) {
            item.title = item.kindText;
        }
        items.append(item);
    }
    return items;
}

QString PresetController::modeText(Presets::ApplyMode mode) {
    return Presets::applyPlanVerb(mode);
}

QString PresetController::kindText(const Presets::PresetSection &section) {
    return sectionKindText(section);
}

QString PresetController::newPresetName() const {
    // Use the complete loaded catalog, including entries hidden by UI filters.
    // Reopening/refreshing the page updates it after external edits.
    const QString base = QStringLiteral("新预设");  // i18n-ignore
    if (!m_catalogNames.contains(base)) {
        return base;
    }
    for (qsizetype index = 2; ; ++index) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(index);
        if (!m_catalogNames.contains(candidate)) {
            return candidate;
        }
    }
}

// ---------------------------------------------------------------------------
// 增删改
// ---------------------------------------------------------------------------

void PresetController::createPreset(const QString &categoryKey) {
    bool categoryOk = true;
    Presets::PresetCategory category = Presets::presetCategoryFromKey(categoryKey, &categoryOk);
    if (categoryKey.isEmpty()) {
        // 页面上第一项就是它；空串只可能来自"没摆下拉框"的测试。
        category = Presets::PresetCategory::GameSettings;
        categoryOk = true;
    }
    if (!categoryOk || category == Presets::PresetCategory::Unset) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("请选择预设类别。"));
        }
        return;
    }
    const Presets::PresetManifest created =
        Presets::create(presetsRoot(), newPresetName(), QString(), category);
    if (created.id.isEmpty()) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("预设目录不可写，无法创建预设。"));
        }
        return;
    }
    log(QStringLiteral("预设：新建了「%1」（%2）。")  // i18n-ignore
            .arg(created.name, categoryText(created.category)));
    refreshList(created.id);
    if (m_page != nullptr) {
        m_page->showStatus(tr("建好了。在右边改名字和说明，然后从实例提取内容。"));
    }
}

void PresetController::duplicatePreset(const QString &id) {
    const Presets::PresetManifest copy = Presets::duplicate(presetsRoot(), id);
    if (copy.id.isEmpty()) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("复制预设失败。请检查原预设可读且目标目录可写。"));
        }
        return;
    }
    log(QStringLiteral("预设：复制出「%1」。").arg(copy.name));  // i18n-ignore
    refreshList(copy.id);
}

void PresetController::saveDetails(const QString &id, const QString &name,
                                   const QString &description) {
    const QString root = presetsRoot();
    bool ok = false;
    QString reason;
    Presets::PresetManifest manifest = Presets::loadManifest(root, id, &ok, &reason);
    if (!ok) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("无法读取源预设：%1").arg(reason));
        }
        return;
    }
    const QString cleaned = Presets::cleanDisplayName(name);
    if (cleaned.isEmpty()) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("名字不能空着。"));
        }
        return;
    }
    const bool renamed = cleaned != manifest.name;
    if (!renamed && description == manifest.description) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("没有要保存的改动。"));
        }
        return;
    }
    manifest.name = cleaned;
    manifest.description = description.trimmed();
    manifest.updatedAt = QDateTime::currentDateTime();
    if (!Presets::saveManifest(root, manifest)) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("保存失败：无法写入预设文件。"));
        }
        return;
    }
    if (renamed) {
        log(QStringLiteral("预设：改名为「%1」。").arg(cleaned));  // i18n-ignore
    }
    refreshList(id);
    if (m_page != nullptr) {
        m_page->showStatus(tr("保存了。"));
    }
}

void PresetController::removePreset(const QString &id) {
    if(id.startsWith("draft:")){if(Presets::remove(presetsRoot()+"/drafts",id.mid(6)))refreshList();return;}
    bool ok = false;
    QString reason;
    const Presets::PresetManifest manifest =
        Presets::loadManifest(presetsRoot(), id, &ok, &reason);
    const QString name = ok ? manifest.name : displayName(id);
    if (!Presets::remove(presetsRoot(), id)) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("删除失败：无法删除预设目录。"));
        }
        return;
    }
    log(QStringLiteral("预设：删除了「%1」。").arg(name));  // i18n-ignore
    if (m_currentId == id) {
        m_currentId.clear();
    }
    refreshList(QString());
    if (m_page != nullptr) {
        m_page->showStatus(tr("删掉了「%1」。").arg(name));
    }
}

void PresetController::setDefaultPreset(const QString &id, bool makeDefault) {
    if (id.startsWith("draft:")) return;
    const QString root = presetsRoot();
    // 这一份属于自己的类别只写在它那份 manifest 里 —— 读一次（小文件），别处不存第二本账。
    bool ok = false;
    QString reason;
    const Presets::PresetManifest manifest = Presets::loadManifest(root, id, &ok, &reason);
    const bool composite = ok && !manifest.editorState.value(QStringLiteral("components")).toObject().isEmpty();
    const Presets::PresetCategory category =
        ok ? manifest.category : Presets::PresetCategory::Unset;

    bool changed = false;
    if (makeDefault) {
        // 混合 / 综合预设不设分类默认：它们没有"这一类"。界面上那颗按钮本来就按这一条灰着；
        // 走到这里说明调用方绕过了界面 —— 不做，也不偷偷退回去写旧版那个单默认项。
        changed = ok && !composite && category != Presets::PresetCategory::Unset
                  && Presets::setDefault(root, category, id);
    } else {
        // 取消：两个来源都清干净。旧版那个单默认键不清的话，旧版本启动器还会说它是默认，
        // 而本版本已经不算它了 —— 那就是两本账。
        if (Presets::defaultPresetId(root) == id) {
            changed = Presets::setDefault(root, QString());
        }
        if (ok && category != Presets::PresetCategory::Unset) {
            changed = Presets::setDefault(root, category, QString()) || changed;
        }
    }
    if (!changed) {
        if (m_page != nullptr) {
            m_page->showStatus(makeDefault && ok
                                   ? tr("综合预设和旧版混合预设不设默认：默认只对四类预设生效。")
                                   : tr("设置默认预设失败：该预设已不存在。"));
        }
        return;
    }
    log(makeDefault
            ? QStringLiteral("预设：%1那一类的默认项改成了「%2」。").arg(categoryText(category), displayName(id))  // i18n-ignore
            : QStringLiteral("预设：取消了「%1」在%2那一类的默认。").arg(displayName(id), categoryText(category)));  // i18n-ignore
    refreshList(id);
}

void PresetController::movePreset(const QString &id, int delta) {
    if (!Presets::move(presetsRoot(), id, delta)) {
        return;  // 已经在边上：不报错（按钮那一侧也是按这个判的）
    }
    refreshList(id);
}

// ---------------------------------------------------------------------------
// 提取
// ---------------------------------------------------------------------------

QString PresetController::captureSummary(const Presets::CaptureReport &report) const {
    QStringList parts;
    if (report.optionKeys > 0) {
        parts.append(tr("游戏设置 %1 项").arg(report.optionKeys));
    }
    if (report.keybindings > 0) {
        parts.append(tr("按键 %1 项").arg(report.keybindings));
    }
    if (report.servers > 0) {
        parts.append(tr("服务器 %1 个").arg(report.servers));
    }
    if (report.configItems > 0) {
        parts.append(tr("config %1 个").arg(report.configItems));
    }
    if (report.defaultConfigItems > 0) {
        parts.append(tr("defaultconfigs %1 个").arg(report.defaultConfigItems));
    }
    if (report.resourcePackItems > 0) {
        parts.append(tr("资源包 %1 个").arg(report.resourcePackItems));
    }
    if (report.shaderPackItems > 0) {
        parts.append(tr("光影包 %1 个").arg(report.shaderPackItems));
    }
    if (report.malilibHotkeys > 0) {
        parts.append(tr("MaLiLib 热键 %1 格").arg(report.malilibHotkeys));
    }
    if (report.malilibValues > 0) {
        parts.append(tr("MaLiLib 值 %1 格").arg(report.malilibValues));
    }
    if (report.dataPackItems > 0) {
        parts.append(tr("数据包 %1 个（%2 个存档）")
                         .arg(report.dataPackItems)
                         .arg(report.dataPackWorlds));
    }
    QString text = parts.isEmpty() ? tr("没有收到任何内容。")
                                   : tr("已提取：%1。").arg(parts.join(tr("，")));
    if (!report.problems.isEmpty()) {
        text += tr("\n未提取：%1")
                    .arg(report.problems.join(tr("；")));
    }
    return text;
}

void PresetController::captureInto(const QString &presetId, const QString &instanceId,
                                   const PresetManagePage::CaptureChoice &choice) {
    const QString root = presetsRoot();
    bool ok = false;
    QString reason;
    Presets::PresetManifest manifest = Presets::loadManifest(root, presetId, &ok, &reason);
    if (!ok) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("无法读取源预设：%1").arg(reason));
        }
        return;
    }

    Presets::CaptureRequest request;
    request.instanceId = instanceId;
    request.instanceRoot = instanceRoot(instanceId);
    request.presetFilesRoot = Presets::filesDir(root, presetId);
    // 「不串类别」的第二道闸（第一道在界面上把格子禁掉）：外面要是还是塞进来了
    // 不属于这一类的样，这里静默摘掉并说清楚，绝不让它落进 manifest。
    QStringList skipped;
    request.selection = filterCapture(manifest.category, toSelection(choice), &skipped);
    // 「模组」这一类**没有文件段**（`captureMaskFor(Mods)` 是 0）：它的内容就是引用表。
    // 在这里收：认得出平台身份的那几件进 `modReferences`，认不出的报个数让用户手动处理。
    if (manifest.category == Presets::PresetCategory::Mods) {
        int added = 0;
        int unidentified = 0;
        const QString problem =
            mergeCapturedModReferences(instanceRoot(instanceId), &manifest.modReferences, &added,
                                       &unidentified);
        if (!problem.isEmpty()) {
            if (m_page != nullptr) {
                m_page->showStatus(tr("提取失败：%1").arg(problem));
            }
            return;
        }
        if (added == 0 && manifest.modReferences.isEmpty()) {
            if (m_page != nullptr) {
                m_page->showStatus(
                    tr("未找到带有效平台安装记录的模组，未提取模组预设内容。"));
            }
            return;
        }
        manifest.sourceInstanceId = instanceId;
        manifest.updatedAt = QDateTime::currentDateTime();
        if (!Presets::saveManifest(root, manifest)) {
            if (m_page != nullptr) {
                m_page->showStatus(tr("内容已提取，但无法保存预设。"));
            }
            return;
        }
        log(QStringLiteral("预设「%1」：从 %2 收了 %3 条模组引用（新增 %4）。")  // i18n-ignore
                .arg(manifest.name, displayName(instanceId))
                .arg(manifest.modReferences.size())
                .arg(added));
        refreshList(presetId);
        if (m_page != nullptr) {
            QString status = tr("收到 %1 条平台模组引用").arg(manifest.modReferences.size());
            if (added > 0) {
                status += tr("（新增 %1 条）").arg(added);
            }
            if (unidentified > 0) {
                // 认不出的那几件怎么处理**要说出来**：编辑器那条「从本地模组导入」会按
                // CurseForge 指纹 + Modrinth sha1 再认一遍（这一层只认安装记录，不联网）。
                status += tr("；另有 %1 个模组的来源未知。可在「预设编辑器 → 从本地模组导入」中重新识别，或在模组页手动补充。")
                              .arg(unidentified);
            }
            status += tr("。");
            m_page->showStatus(status);
        }
        return;
    }
    if (request.selection.isEmpty()) {
        if (m_page != nullptr) {
            m_page->showStatus(
                skipped.isEmpty()
                    ? tr("未选择任何项。")
                    : tr("这类预设不收录：%1。")
                          .arg(skipped.join(tr("、"))));
        }
        return;
    }

    const Presets::CaptureReport report = Presets::captureFromInstance(request);
    if (!report.ok) {
        if (m_page != nullptr) {
            m_page->showStatus(report.problem.isEmpty()
                                   ? tr("没有收到任何内容。")
                                   : tr("提取失败：%1").arg(report.problem));
        }
        return;
    }

    manifest.sections = Presets::mergeCapturedSections(manifest.sections, report.sections);
    manifest.sourceInstanceId = instanceId;
    manifest.updatedAt = QDateTime::currentDateTime();
    if (!Presets::saveManifest(root, manifest)) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("内容已提取，但无法保存预设。"));
        }
        return;
    }

    log(QStringLiteral("预设「%1」：从 %2 收了 %3 段。")  // i18n-ignore
            .arg(manifest.name, displayName(instanceId))
            .arg(report.sectionCount()));
    refreshList(presetId);
    if (m_page != nullptr) {
        QString status = captureSummary(report);
        if (!skipped.isEmpty()) {
            status += tr("\n这一类预设不收：%1。")
                          .arg(skipped.join(tr("、")));
        }
        m_page->showStatus(status);
    }
}

void PresetController::captureAsNew(const QString &instanceId, const QString &name,
                                    const QString &categoryKey,
                                    const PresetManagePage::CaptureChoice &choice) {
    bool categoryOk = true;
    Presets::PresetCategory category = Presets::presetCategoryFromKey(categoryKey, &categoryOk);
    if (categoryKey.isEmpty()) {
        category = Presets::PresetCategory::GameSettings;
        categoryOk = true;
    }
    if (!categoryOk || category == Presets::PresetCategory::Unset) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("请选择预设类别。"));
        }
        return;
    }

    QString wanted = Presets::cleanDisplayName(name);
    if (wanted.isEmpty()) {
        wanted = QStringLiteral("%1 预设").arg(displayName(instanceId));  // i18n-ignore
    }
    const QString root = presetsRoot();
    Presets::PresetManifest created = Presets::create(root, wanted, QString(), category);
    if (created.id.isEmpty()) {
        if (m_page != nullptr) {
            m_page->showStatus(tr("预设目录不可写，无法创建预设。"));
        }
        return;
    }

    Presets::CaptureRequest request;
    request.instanceId = instanceId;
    request.instanceRoot = instanceRoot(instanceId);
    request.presetFilesRoot = Presets::filesDir(root, created.id);
    QStringList skipped;
    request.selection = filterCapture(category, toSelection(choice), &skipped);
    // 「模组」这一类没有文件段：内容 = 从实例收来的平台引用（见 mergeCapturedModReferences）。
    if (category == Presets::PresetCategory::Mods) {
        int unidentified = 0;
        QString problem;
        created.modReferences = Presets::captureInstalledModReferences(
            request.instanceRoot, &unidentified, &problem);
        if (!problem.isEmpty()) {
            Presets::remove(root, created.id);
            if (m_page != nullptr) {
                m_page->showStatus(tr("提取失败：%1").arg(problem));
            }
            return;
        }
        if (created.modReferences.isEmpty()) {
            // 空壳比"报成功"诚实（和下面那条空选择同一个立场）。
            Presets::remove(root, created.id);
            if (m_page != nullptr) {
                m_page->showStatus(
                    tr("无法确认此实例中的模组来自哪个平台。其安装记录可能缺失，或文件已被替换。预设未创建。"));
            }
            return;
        }
        created.sourceInstanceId = instanceId;
        created.updatedAt = QDateTime::currentDateTime();
        if (!Presets::saveManifest(root, created)) {
            Presets::remove(root, created.id);
            if (m_page != nullptr) {
                m_page->showStatus(tr("预设目录不可写，未创建新预设。"));
            }
            return;
        }
        log(QStringLiteral("预设「%1」：从 %2 收了 %3 条模组引用。")  // i18n-ignore
                .arg(created.name, displayName(instanceId))
                .arg(created.modReferences.size()));
        refreshList(created.id);
        if (m_page != nullptr) {
            QString status = tr("收到 %1 条平台模组引用").arg(created.modReferences.size());
            if (unidentified > 0) {
                // 认不出的那几件怎么处理**要说出来**：编辑器那条「从本地模组导入」会按
                // CurseForge 指纹 + Modrinth sha1 再认一遍（这一层只认安装记录，不联网）。
                status += tr("；另有 %1 个模组的来源未知。可在「预设编辑器 → 从本地模组导入」中重新识别，或在模组页手动补充。")
                              .arg(unidentified);
            }
            status += tr("。");
            m_page->showStatus(status);
        }
        return;
    }
    if (request.selection.isEmpty()) {
        // 这一类什么都收不了（模组 / 数据包首版就是这样）：空壳比"报成功"诚实。
        Presets::remove(root, created.id);
        if (m_page != nullptr) {
            m_page->showStatus(
                skipped.isEmpty()
                    ? tr("未选择任何项，新预设没有创建。")
                    : tr("这类预设不收录：%1；新预设没有创建。")
                          .arg(skipped.join(tr("、"))));
        }
        return;
    }

    const Presets::CaptureReport report = Presets::captureFromInstance(request);
    if (!report.ok) {
        // 什么都没收到 ⇒ 这一份空壳留着只会碍眼，撤掉（`create()` 那一步刚建的目录）。
        Presets::remove(root, created.id);
        if (m_page != nullptr) {
            m_page->showStatus(report.problem.isEmpty()
                                   ? tr("没有收到任何内容，新预设没有创建。")
                                   : tr("提取失败：%1").arg(report.problem));
        }
        return;
    }

    created.sections = report.sections;
    created.sourceInstanceId = instanceId;
    created.updatedAt = QDateTime::currentDateTime();
    if (!Presets::saveManifest(root, created)) {
        Presets::remove(root, created.id);
        if (m_page != nullptr) {
            m_page->showStatus(tr("预设目录不可写，未创建新预设。"));
        }
        return;
    }

    log(QStringLiteral("预设：从 %1 提取出「%2」（%3 段）。")  // i18n-ignore
            .arg(displayName(instanceId), created.name)
            .arg(report.sectionCount()));
    refreshList(created.id);
    if (m_page != nullptr) {
        QString status = captureSummary(report);
        if (!skipped.isEmpty()) {
            status += tr("\n这一类预设不收：%1。")
                          .arg(skipped.join(tr("、")));
        }
        m_page->showStatus(status);
    }
}

// ---------------------------------------------------------------------------
// 计划 / 应用
// ---------------------------------------------------------------------------

QString PresetController::rowText(const Presets::ApplyPlanRow &row) const {
    QString head = row.title;
    if (head.isEmpty()) {
        head = kindTextOf(row.kind);
    }
    QString text = QStringLiteral("%1  %2").arg(head, Presets::applyPlanRowText(row));
    if (!row.problem.isEmpty()) {
        text += tr("（%1）").arg(row.problem);
    }
    return text;
}

void PresetController::previewPlan(const QString &presetId, const QString &instanceId) {
    if(presetId.startsWith("draft:")){if(m_page)m_page->showStatus(tr("草稿需保存为预设后才能应用。"));return;}
    if (!m_page || presetId.isEmpty() || instanceId.isEmpty() ||
        presetId != m_page->currentPresetId() || instanceId != m_page->selectedApplyInstanceId()) return;
    if (m_host.isInstanceBusy && m_host.isInstanceBusy(instanceId)) {
        invalidatePreview();
        PresetManagePage::PlanView view;
        view.problem = tr("这个实例正在处理文件，请完成后再重新预览。");
        m_page->showPlan(view);
        return;
    }
    Presets::PreviewRequest request;
    request.root = presetsRoot();
    request.presetId = presetId;
    request.instanceId = instanceId;
    request.instanceRoot = instanceRoot(instanceId);
    request.supportsDataPacks = m_versions && m_versions->instanceSupportsDatapacks(instanceId);
    request.gameRunning = m_host.isRunning ? m_host.isRunning(instanceId) : false;
    const QPointer<PresetManagePage> page = m_page;
    page->setPlanLoading(true);
    m_previewLoader->load(request, [this, page](const Presets::PreviewSnapshot &result) {
        if (!page || page != m_page) return;
        page->setPlanLoading(false);
        updatePageBusy();
        const auto &key = result.request;
        if (key.root != presetsRoot() || key.instanceRoot != instanceRoot(key.instanceId) ||
            key.presetId != page->currentPresetId() || key.instanceId != page->selectedApplyInstanceId()) return;
        const bool runningNow = m_host.isRunning ? m_host.isRunning(key.instanceId) : false;
        if (runningNow != key.gameRunning || (m_host.isInstanceBusy && m_host.isInstanceBusy(key.instanceId))) {
            PresetManagePage::PlanView changed;
            changed.problem = tr("实例运行或文件操作状态已改变，请重新预览。");
            page->showPlan(changed);
            return;
        }
        showPreview(result);
    });
    updatePageBusy();
}

void PresetController::showPreview(const Presets::PreviewSnapshot &snapshot) {
    PresetManagePage::PlanView view;
    if (!snapshot.ok) {
        view.problem = snapshot.problem;
        m_page->showPlan(view);
        return;
    }
    const auto &manifest = snapshot.manifest;
    const auto &plan = snapshot.plan;
    view.ok = plan.ok;
    view.problem = plan.problem;
    view.blockedReason = plan.blockedReason;
    view.needsConfirm = plan.needsConfirm();
    view.replaceConfirmed = false;
    view.canApply = plan.canApply() && plan.writesAnything();
    if (manifest.category == Presets::PresetCategory::Mods && !manifest.modReferences.isEmpty() &&
        !snapshot.request.gameRunning && plan.ok) {
        view.canApply = true;
        view.rows.append({tr("平台模组 %1 项；应用时联网检查发行版本、依赖和冲突，必要项无法确认则不安装。")
                          .arg(manifest.modReferences.size()), false});
    }
    for (const Presets::ApplyPlanRow &row : plan.rows) {
        PresetManagePage::PlanLine line;
        line.text = rowText(row);
        line.risky = row.risky();
        view.rows.append(line);
    }
    for (const Presets::MalilibConflict &conflict : plan.keyConflicts) {
        view.keyConflicts.append(
            QStringLiteral("%1 ← %2").arg(conflict.keys, conflict.claims.join(QStringLiteral(" / "))));
    }
    if (plan.ok) {
        QString summary = tr("%1 段，会动 %2 项").arg(plan.rows.size()).arg(plan.totalCount());
        if (plan.blockedRowCount() > 0) {
            summary += tr("，%1 段做不了").arg(plan.blockedRowCount());
        }
        if (plan.riskyRowCount() > 0) {
            summary += tr("，%1 段要留意").arg(plan.riskyRowCount());
        }
        view.summary = summary;
    }
    if (!manifest.modReferences.isEmpty())
        view.summary += tr("；平台模组 %1 项（发行文件及前置待执行时确认）").arg(manifest.modReferences.size());
    m_page->showPlan(view);
}

QString PresetController::applyRowText(const Presets::PresetApplyRow &row) const {
    QString head = row.title;
    if (head.isEmpty()) {
        head = kindTextOf(row.kind);
    }
    QString state;
    if (row.blocked) {
        state = tr("做不了%1").arg(row.problem.isEmpty()
                                                  ? QString()
                                                  : tr("（%1）").arg(row.problem));
    } else if (row.skipped) {
        state = tr("跳过");
    } else if (!row.ok) {
        state = tr("没做成%1").arg(row.problem.isEmpty()
                                                   ? QString()
                                                   : tr("（%1）").arg(row.problem));
    } else if (row.written == 0 && row.removed == 0 && row.deferred == 0) {
        state = tr("没有变化");
    } else {
        QStringList bits;
        if (row.written > 0) {
            bits.append(tr("写入 %1").arg(row.written));
        }
        if (row.added > 0) {
            bits.append(tr("新增 %1").arg(row.added));
        }
        if (row.removed > 0) {
            bits.append(tr("删掉 %1").arg(row.removed));
        }
        if (row.deferred > 0) {
            bits.append(tr("待应用 %1").arg(row.deferred));
        }
        if (row.skippedItems > 0) {
            bits.append(tr("跳过 %1").arg(row.skippedItems));
        }
        state = bits.join(tr("，"));
    }
    return QStringLiteral("%1  %2").arg(head, state);
}

void PresetController::showExecutionReport(const QString &presetId, const QString &instanceId) {
    if (!m_page || presetId.isEmpty() || instanceId.isEmpty()) return;
    bool loaded = false;
    const auto manifest = Presets::loadManifest(presetsRoot(), presetId, &loaded);
    if (!loaded) { m_page->showStatus(tr("预设不可读，无法核对执行报告。")); return; }
    const bool mods = manifest.category == Presets::PresetCategory::Mods;
    const QString path = mods ? instanceRoot(instanceId) + QStringLiteral("/wtfml.preset-mod-install.json")
        : Presets::presetExecutionReportPath(instanceRoot(instanceId));
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
        m_page->showStatus(tr("没有可读的执行报告。")); return;
    }
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    PresetManagePage::ApplyView view;
    view.ok = object.value("success").toBool();
    view.backupDir = object.value("backupDir").toString();
    view.problem = object.value("problem").toString();
    const auto rows = object.value(mods ? "execution" : "rows").toArray();
    const QHash<QString, QString> states{{"planned", tr("计划中 / 尚未完成")}, {"downloaded", tr("已下载，未部署")},
        {"enabled", tr("已部署并启用")}, {"done", tr("已完成")}, {"failed", tr("失败")},
        {"blocked", tr("未解决")}, {"skipped", tr("已跳过")}, {"pending-world", tr("待应用：等待目标世界")}};
    for (const auto &value : rows) {
        const auto row = value.toObject(); const QString state = row.value("state").toString();
        view.lines.append(tr("%1：%2；写入 %3 项 %4")
            .arg(row.value(mods ? "filename" : "title").toString(), states.value(state, tr("状态无法确认")))
            .arg(row.value("written").toInt()).arg(row.value("reason").toString()));
    }
    view.lines.append(tr("已完成项保留；继续时重新核对预设和实际文件，只重试未完成部分。不会宣称整趟回滚。"));
    m_page->showApplyResult(view);
}

void PresetController::applyPlan(const QString &presetId, const QString &instanceId,
                                 bool replaceConfirmed) {
    applyPlanImpl(presetId, instanceId, replaceConfirmed, false);
}

void PresetController::applyPlanImpl(const QString &presetId, const QString &instanceId,
                                 bool replaceConfirmed, bool resume) {
    if (m_page == nullptr) {
        return;
    }
    PresetManagePage::ApplyView view;

    if (m_host.isInstanceBusy && m_host.isInstanceBusy(instanceId)) {
        view.problem = tr("这个实例正在安装、补全文件或后台搬文件（复制、还原、导入），等它收尾之后再应用预设。");
        m_page->showApplyResult(view);
        return;
    }

    bool ok = false;
    QString reason;
    const Presets::PresetManifest manifest =
        Presets::loadManifest(presetsRoot(), presetId, &ok, &reason);
    if (!ok) {
        view.problem = tr("无法读取源预设：%1").arg(reason);
        m_page->showApplyResult(view);
        return;
    }
    const auto componentIds=manifest.editorState.value("components").toObject();
    if (!componentIds.isEmpty() && !manifest.modReferences.isEmpty()) {
        Presets::PresetApplyRequest request;
        request.instanceId=instanceId;request.instanceRoot=instanceRoot(instanceId);
        request.gameRunning=m_host.isRunning&&m_host.isRunning(instanceId);
        request.replaceConfirmed=replaceConfirmed;request.resumeUnfinished=resume;
        request.supportsDataPacks=m_versions&&m_versions->instanceSupportsDatapacks(instanceId);
        const auto preview=planOnly(manifest,request);
        if(preview.needsConfirm||!preview.blockedReason.isEmpty()||!preview.problem.isEmpty()) {
            view.needsConfirm=preview.needsConfirm;view.problem=!preview.blockedReason.isEmpty()?preview.blockedReason:preview.problem;
            m_page->showApplyResult(view);return;
        }
        const auto root=presetsRoot();const auto page=m_page;
        installModsAfterInstall({componentIds.value("mods").toString()},instanceId,[this,root,page,manifest,request](bool success,const QString &message) {
            if (!page) { return; }
            PresetManagePage::ApplyView view;
            view.lines=message.split('\n');
            if(!success){view.problem=tr("模组未全部应用，其余部分尚未应用。");page->showApplyResult(view);return;}
            const auto result=Presets::applyPresetToInstance(root,manifest,request);view.ok=result.ok;view.needsConfirm=result.needsConfirm;
            view.problem=!result.blockedReason.isEmpty()?result.blockedReason:result.problem;view.backupDir=result.backupDir;
            for (const auto &row : result.rows) { view.lines.append(applyRowText(row)); }
            page->showApplyResult(view);
        });
        return;
    }
    if (manifest.category == Presets::PresetCategory::Mods && !manifest.modReferences.isEmpty()) {
        QStringList selected{presetId};
        if (resume) {
            const QString path = instanceRoot(instanceId) + QStringLiteral("/wtfml.preset-mod-install.json");
            QFile file(path);
            if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
                m_page->showStatus(tr("没有可读的模组执行报告，请重新预览。")); return;
            }
            const auto report = QJsonDocument::fromJson(file.readAll()).object();
            if (report.value("format").toString() != QStringLiteral("wtfml-preset-mod-result/1")) {
                m_page->showStatus(tr("报告格式不支持，未自动继续。")); return;
            }
            selected.clear();
            for (const auto &value : report.value("selection").toArray()) {
                const auto old = value.toObject(); const auto id = old.value("id").toString();
                bool loaded = false; const auto current = Presets::loadManifest(presetsRoot(), id, &loaded);
                if (!loaded || Presets::manifestToJson(current) != old) {
                    m_page->showStatus(tr("预设已变化或缺失，请重新预览；未沿用旧报告写入。")); return;
                }
                selected.append(id);
            }
            if (selected.isEmpty() || !selected.contains(presetId)) {
                m_page->showStatus(tr("报告不属于当前所选预设，未继续。")); return;
            }
        }
        m_page->showStatus(tr("正在检查并安装模组预设，必要前置将一并解析…"));
        installModsAfterInstall(selected, instanceId, [this](bool success, const QString &message) {
            if (!m_page) return;
            PresetManagePage::ApplyView report;
            report.ok = success;
            report.lines = message.split(QLatin1Char('\n'));
            if (!success) report.problem = tr("模组预设未全部应用，请查看逐项原因。");
            m_page->showApplyResult(report);
        });
        return;
    }

    Presets::PresetApplyRequest request;
    request.instanceId = instanceId;
    request.instanceRoot = instanceRoot(instanceId);
    request.gameRunning = m_host.isRunning ? m_host.isRunning(instanceId) : false;
    request.replaceConfirmed = replaceConfirmed;
    request.resumeUnfinished = resume;
    request.supportsDataPacks =
        m_versions != nullptr && m_versions->instanceSupportsDatapacks(instanceId);

    const Presets::PresetApplyResult result =
        Presets::applyPresetToInstance(presetsRoot(), manifest, request);

    view.ok = result.ok;
    view.needsConfirm = result.needsConfirm;
    view.problem = !result.blockedReason.isEmpty() ? result.blockedReason : result.problem;
    view.backupDir = result.backupDir;
    for (const Presets::PresetApplyRow &row : result.rows) {
        view.lines.append(applyRowText(row));
    }
    for (const Presets::MalilibConflict &conflict : result.keyConflicts) {
        view.lines.append(TextMarks::warning(tr("键位冲突：%1 ← %2"))
                              .arg(conflict.keys,
                                   conflict.claims.join(QStringLiteral(" / "))));
    }
    m_page->showApplyResult(view);

    if (result.needsConfirm) {
        return;  // 盘上一个字节都没动，日志里也不写"应用了"
    }
    if (!result.blockedReason.isEmpty()) {
        log(QStringLiteral("预设：%1 现在不能应用（%2）。")  // i18n-ignore
                .arg(displayName(instanceId), result.blockedReason));
        return;
    }
    if (!result.ok) {
        log(QStringLiteral("预设：应用到 %1 没有完成%2。")  // i18n-ignore
                .arg(displayName(instanceId),
                     result.problem.isEmpty() ? QString()
                                               : QStringLiteral("：%1").arg(result.problem)));  // i18n-ignore
        return;
    }
    QString line = QStringLiteral("预设「%1」：应用到 %2，写入 %3 项")  // i18n-ignore
                       .arg(manifest.name, displayName(instanceId))
                       .arg(result.written);
    if (result.removed > 0) {
        line += QStringLiteral("、删掉 %1 项").arg(result.removed);  // i18n-ignore
    }
    if (!result.backupDir.isEmpty()) {
        line += QStringLiteral("；备份在 %1").arg(QDir::toNativeSeparators(result.backupDir));  // i18n-ignore
    }
    log(line);
}

// ---------------------------------------------------------------------------
// 给安装流程的只读入口
// ---------------------------------------------------------------------------

QList<Presets::PresetSummary> PresetController::availablePresets() const {
    return Presets::list(presetsRoot());
}

QMap<QString, QString> PresetController::categoryDefaultsInEffect() const {
    const QString root = presetsRoot();
    Presets::PresetDefaults defaults = Presets::defaults(root);

    // 旧版那个单默认项**自己**属于哪一类：读它那份 manifest（一个小文件）。
    // 读不回来 / 它是混合预设（`category == Unset`）⇒ 不知道，于是不推导 ——
    // 宁可这一类没有默认，也不把一份不知类别的预设塞进去。
    Presets::PresetCategory legacyCategory = Presets::PresetCategory::Unset;
    if (!defaults.legacyId.isEmpty()) {
        bool ok = false;
        const Presets::PresetManifest manifest = Presets::loadManifest(root, defaults.legacyId, &ok);
        if (ok) {
            legacyCategory = manifest.category;
            // 综合预设（有子项的那一份）也不能拆成几类默认：它自己没有"这一类"。
            if (!manifest.editorState.value(QStringLiteral("components")).toObject().isEmpty()) {
                legacyCategory = Presets::PresetCategory::Unset;
            }
        }
    }

    QMap<QString, QString> out;
    for (const PresetController::InstallCategory &category : PresetController::installCategories()) {
        bool keyOk = false;
        const Presets::PresetCategory value = Presets::presetCategoryFromKey(category.key, &keyOk);
        if (!keyOk || value == Presets::PresetCategory::Unset) {
            continue;
        }
        const QString id = Presets::defaultFor(defaults, value, legacyCategory);
        if (id.isEmpty()) {
            continue;
        }
        // **默认缺失回退**：指的那一份已经不在 / 读不回来 ⇒ 这一类就是没有默认。
        // 不在这里报错：用户有一天删了那一份，安装页不该从那时起每次都弹一句警告。
        bool exists = false;
        Presets::loadManifest(root, id, &exists);
        if (!exists) {
            continue;
        }
        out.insert(category.key, id);
    }
    return out;
}

QList<PresetController::InstallCategory> PresetController::installCategories() {
    // 次序 = 安装页弹窗那四个页签的次序。★ 改这里的次序就是改界面 —— 页签和数据同一份，
    // 别处不要再写一遍四类的名字（`categoryText()` 是那四句话的唯一出处）。
    const Presets::PresetCategory order[] = {
        Presets::PresetCategory::GameSettings,
        Presets::PresetCategory::Mods,
        Presets::PresetCategory::ResourcePacks,
        Presets::PresetCategory::DataPacks,
        Presets::PresetCategory::ShaderPacks,
    };
    QList<InstallCategory> out;
    for (const Presets::PresetCategory category : order) {
        out.append({Presets::presetCategoryKey(category), categoryText(category)});
    }
    return out;
}

namespace {

/// 把摘要列表补成安装页弹窗要的那张表：复合预设再读一次它的 manifest（普通预设不读）。
///
/// 两条路（同步的 `installPickerData()` 和后台的 `requestInstallPickerData()`）共用这一份，
/// 免得"哪些是复合预设、子项叫什么"在两处各算一遍。
QList<PresetController::InstallPickerPreset> buildInstallPickerRows(
    const QString &root, const QList<Presets::PresetSummary> &summaries) {
    QList<PresetController::InstallPickerPreset> rows;
    rows.reserve(summaries.size());
    for (const Presets::PresetSummary &summary : summaries) {
        PresetController::InstallPickerPreset row;
        row.summary = summary;
        // 普通预设没有要读的那份 manifest；复合预设读完一次，这一整段就都用这一份
        // （别的调用点再读一遍是同一次读盘变成两次）。
        row.componentsLoaded = summary.componentCategories.isEmpty();
        if (!row.componentsLoaded) {
            bool loaded = false;
            const Presets::PresetManifest manifest = Presets::loadManifest(root, summary.id, &loaded);
            row.componentsLoaded = loaded;
            if (loaded) {
                const QJsonObject parts =
                    manifest.editorState.value(QStringLiteral("components")).toObject();
                for (auto it = parts.begin(); it != parts.end(); ++it) {
                    row.components.insert(it.key(), it.value().toString());
                }
            }
        }
        rows.append(row);
    }
    return rows;
}

}  // namespace

PresetController::InstallPickerData PresetController::installPickerData() const {
    InstallPickerData data;
    const QString root = presetsRoot();
    data.presets = buildInstallPickerRows(root, Presets::list(root));
    return data;
}

void PresetController::requestInstallPickerData(
    std::function<void(const InstallPickerData &)> done) {
    const quint64 generation = ++m_pickerDataGeneration;
    const QString root = presetsRoot();
    m_catalogLoader->load(
        root, [this, root, generation, done = std::move(done)](
                  const Presets::CatalogSnapshot &snapshot) {
            if (generation != m_pickerDataGeneration) return;
            // 文件夹不在 = 还没有预设（空表，不是出错）；在、却读不了才是出错。
            const QFileInfo rootInfo(root);
            if (rootInfo.exists() && (!rootInfo.isDir() || !rootInfo.isReadable())) {
                InstallPickerData failed;
                failed.problem = tr("预设文件夹读不了：%1").arg(QDir::toNativeSeparators(root));
                if (done) done(failed);
                return;
            }
            QList<Presets::PresetSummary> summaries;
            summaries.reserve(snapshot.presets.size());
            for (const Presets::PresetSummary &summary : snapshot.presets) {
                // 草稿属于预设页：安装页那张弹窗一直只列正式预设（`Presets::list()` 不带
                // 草稿，而预设页那份目录快照会带 —— 这里按同一个口径滤掉）。
                if (summary.id.startsWith(QStringLiteral("draft:"))) continue;
                summaries.append(summary);
            }
            // 复合预设那几份 manifest 也在后台读：这一段要是不挪走，等于把"同步读盘"
            // 从列表换到了详情上。
            auto *watcher = new QFutureWatcher<QList<InstallPickerPreset>>(this);
            connect(watcher, &QFutureWatcher<QList<InstallPickerPreset>>::finished, this,
                    [this, watcher, generation, done] {
                        const QList<InstallPickerPreset> rows = watcher->result();
                        watcher->deleteLater();
                        if (generation != m_pickerDataGeneration) return;
                        InstallPickerData data;
                        data.presets = rows;
                        if (done) done(data);
                    });
            watcher->setFuture(QtConcurrent::run(
                [root, summaries] { return buildInstallPickerRows(root, summaries); }));
        });
}

QList<Presets::PresetSummary> PresetController::InstallPickerData::optionsFor(
    const QString &categoryKey) const {
    bool ok = false;
    const Presets::PresetCategory category = Presets::presetCategoryFromKey(categoryKey, &ok);
    if (!ok || category == Presets::PresetCategory::Unset) {
        return {};
    }
    QList<Presets::PresetSummary> out;
    for (const InstallPickerPreset &row : presets) {
        const Presets::PresetSummary &one = row.summary;
        if (one.category == category) {
            out.append(one);
        } else if (one.componentCategories.contains(categoryKey)) {
            if (!row.componentsLoaded) {
                continue;  // 那份 manifest 没读回来 ⇒ 不知道这一段是哪个子预设
            }
            const QString child = row.components.value(categoryKey);
            if (Presets::isValidPresetId(child)) {
                auto part = one;
                part.id = child;
                part.category = category;
                part.itemCount = one.componentItemCounts.value(categoryKey, -1);
                out.append(part);
            }
        }
    }
    return out;
}

PresetController::InstallPreview PresetController::previewForInstall(
    const QString &presetId, const QString &instanceName) const {
    InstallPreview view;
    if (!Presets::isValidPresetId(presetId)) {
        view.problem = tr("没有选中有效的预设。");
        return view;
    }

    bool ok = false;
    QString reason;
    const Presets::PresetManifest manifest =
        Presets::loadManifest(presetsRoot(), presetId, &ok, &reason);
    if (!ok) {
        view.problem = tr("无法读取待导入的预设：%1").arg(reason);
        return view;
    }
    view.presetName = manifest.name;
    view.fingerprint = QString::fromLatin1(QCryptographicHash::hash(Presets::manifestToBytes(manifest), QCryptographicHash::Sha256).toHex());
    if (!manifest.modReferences.isEmpty()) {
        view.lines.append(tr("平台模组 %1 项：按目标版本解析发行文件与必要前置，缺失或查询失败会如实报告。")
                          .arg(manifest.modReferences.size()));
    }

    QString snapshotProblem;
    Presets::TargetSnapshot snapshot =
        Presets::readTargetSnapshot(instanceRoot(instanceName), &snapshotProblem);
    if (!snapshotProblem.isEmpty()) {
        view.problem = snapshotProblem;
        return view;
    }
    snapshot.supportsDataPacks =
        m_versions != nullptr && m_versions->instanceSupportsDatapacks(instanceName);

    Presets::PlanRequest request;
    request.presetFilesRoot = Presets::filesDir(presetsRoot(), presetId);
    request.sections = manifest.sections;
    request.target = snapshot;
    // 第一趟一律 `false`：这里就是"给用户看的第一趟"，有「覆盖」就如实报出来。
    request.replaceConfirmed = false;
    request.gameRunning = m_host.isRunning ? m_host.isRunning(instanceName) : false;

    const Presets::ApplyPlan plan = Presets::planPresetApplication(request);
    view.ok = plan.ok;
    view.problem = plan.problem;
    view.blockedReason = plan.blockedReason;
    view.needsConfirm = plan.needsConfirm();
    for (const Presets::ApplyPlanRow &row : plan.rows) {
        view.lines.append(rowText(row));
    }
    for (const Presets::MalilibConflict &conflict : plan.keyConflicts) {
        const QString line = TextMarks::warning(tr("键位冲突：%1 ← %2"))
                                 .arg(conflict.keys,
                                      conflict.claims.join(QStringLiteral(" / ")));
        view.lines.append(line);
        view.reasons.append(line);
    }
    // 弹窗状态行那几格：动几项 / 跳过几项 / 哪几段做不了。三个数都是
    // `ApplyPlan` 自己算的，这一层只做加法 —— 别在界面上拿 `lines.size()` 之类的近似。
    view.itemCount = plan.totalCount();
    for (const Presets::ApplyPlanRow &row : plan.rows) {
        view.skippedCount += row.skipped;
        if (row.blocked) {
            ++view.blockedCount;
        }
        // 「缺失、未知、跳过」那几类原因：只在**用得上**的时候才给（正常合并的那些段
        // 不进这张单子，否则展开之后是一屏"合并 18 项"的废话）。
        if (row.risky() || row.skipped > 0 || !row.problem.isEmpty()) {
            view.reasons.append(rowText(row));
        }
    }
    // 这一段和 `previewPlan()` 里那句小结**同口径**：界面里两处说"会动几项"不该有两种说法。
    if (plan.ok) {
        QString summary = tr("%1 段，会动 %2 项")
                              .arg(plan.rows.size())
                              .arg(plan.totalCount());
        if (plan.blockedRowCount() > 0) {
            summary += tr("，%1 段做不了").arg(plan.blockedRowCount());
        }
        if (plan.riskyRowCount() > 0) {
            summary += tr("，%1 段要留意").arg(plan.riskyRowCount());
        }
        view.summary = summary;
    }
    return view;
}

QString PresetController::presetFingerprint(const QString &presetId) const {
    bool ok = false;
    const auto manifest = Presets::loadManifest(presetsRoot(), presetId, &ok);
    return ok ? QString::fromLatin1(QCryptographicHash::hash(Presets::manifestToBytes(manifest), QCryptographicHash::Sha256).toHex()) : QString();
}

PresetController::InstallApply PresetController::applyAfterInstall(const QString &presetId,
                                                                   const QString &instanceName,
                                                                   bool replaceConfirmed) {
    InstallApply out;
    bool ok = false;
    QString reason;
    const Presets::PresetManifest manifest =
        Presets::loadManifest(presetsRoot(), presetId, &ok, &reason);
    if (!ok) {
        out.message = tr("预设读取失败：%1").arg(reason);
        log(QStringLiteral("预设：%1").arg(out.message));  // i18n-ignore
        return out;
    }

    Presets::PresetApplyRequest request;
    request.instanceId = instanceName;
    request.instanceRoot = instanceRoot(instanceName);
    request.gameRunning = m_host.isRunning ? m_host.isRunning(instanceName) : false;
    request.replaceConfirmed = replaceConfirmed;
    request.supportsDataPacks =
        m_versions != nullptr && m_versions->instanceSupportsDatapacks(instanceName);

    const Presets::PresetApplyResult result =
        Presets::applyPresetToInstance(presetsRoot(), manifest, request);
    out.needsConfirm = result.needsConfirm;
    out.backupDir = result.backupDir;
    out.written = result.written;
    out.removed = result.removed;

    int failed = 0;
    for (const Presets::PresetApplyRow &row : result.rows) {
        if (!row.ok && !row.skipped && !row.blocked) {
            ++failed;
        }
        if (row.blocked) {
            ++failed;
        }
    }

    if (result.needsConfirm) {
        out.message = tr("此预设包含覆盖操作。请在实例的「预设」页查看并确认应用计划。");
    } else if (!result.blockedReason.isEmpty()) {
        out.message = result.blockedReason;
    } else if (!result.ok) {
        out.message = result.problem.isEmpty()
                          ? tr("本次操作未完成。")
                          : tr("本次操作未完成：%1").arg(result.problem);
    } else {
        out.ok = failed == 0;
        out.message = tr("实例预设「%1」已应用：写入 %2 项")
                          .arg(manifest.name)
                          .arg(result.written);
        if (result.removed > 0) {
            out.message += tr("、删掉 %1 项").arg(result.removed);
        }
        if (failed > 0) {
            out.message += tr("，%1 段没做成（管理页里可以看详情）").arg(failed);
        }
        for (const auto &row : result.rows) {
            if (row.deferred > 0 || !row.problem.isEmpty())
                out.message += QLatin1Char('\n') + applyRowText(row);
        }
        if (!result.backupDir.isEmpty()) {
            out.message += tr("；备份在 %1")
                               .arg(QDir::toNativeSeparators(result.backupDir));
        }
    }

    if (out.ok) {
        log(QStringLiteral("预设：%1").arg(out.message));  // i18n-ignore
    } else {
        log(QStringLiteral("预设：「%1」没有应用到 %2：%3")  // i18n-ignore
                .arg(manifest.name, displayName(instanceName), out.message));
    }
    return out;
}

Presets::PresetApplyResult PresetController::planOnly(
    const Presets::PresetManifest &manifest, const Presets::PresetApplyRequest &request) const {
    Presets::PresetApplyResult out;

    QString snapshotProblem;
    Presets::TargetSnapshot snapshot =
        Presets::readTargetSnapshot(request.instanceRoot, &snapshotProblem);
    if (!snapshotProblem.isEmpty()) {
        out.problem = snapshotProblem;
        return out;
    }
    snapshot.supportsDataPacks = request.supportsDataPacks;

    Presets::PlanRequest planRequest;
    planRequest.presetFilesRoot = Presets::filesDir(presetsRoot(), manifest.id);
    planRequest.sections = manifest.sections;
    planRequest.target = snapshot;
    planRequest.replaceConfirmed = request.replaceConfirmed;
    planRequest.gameRunning = request.gameRunning;

    const Presets::ApplyPlan plan = Presets::planPresetApplication(planRequest);
    out.ok = plan.ok;
    out.problem = plan.problem;
    out.blockedReason = plan.blockedReason;
    out.needsConfirm = plan.needsConfirm();
    out.keyConflicts = plan.keyConflicts;
    for (const Presets::ApplyPlanRow &row : plan.rows) {
        Presets::PresetApplyRow one;
        one.sectionId = row.sectionId;
        one.kind = row.kind;
        one.mode = row.mode;
        one.title = row.title;
        one.blocked = row.blocked;
        one.skipped = row.mode == Presets::ApplyMode::Skip;
        one.ok = !row.blocked;
        one.written = row.count;
        one.removed = row.removed;
        one.skippedItems = row.skipped;
        one.deferred = row.deferred;
        one.problem = row.problem;
        out.rows.append(one);
        out.written += row.count;
        out.removed += row.removed;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 安装前的兼容检查
// ---------------------------------------------------------------------------

QList<Presets::ModReference> PresetController::modReferencesFor(const QString &presetId) const {
    if (presetId.trimmed().isEmpty()) {
        return {};
    }
    bool ok = false;
    QString reason;
    const Presets::PresetManifest manifest =
        Presets::loadManifest(presetsRoot(), presetId, &ok, &reason);
    if (!ok) {
        return {};
    }
    return manifest.modReferences;
}

void PresetController::startModCheck(const QString &presetId, const QString &gameVersion,
                                     const QString &loader, bool clientSide) {
    if (m_modChecker == nullptr) {
        return;
    }
    // 先把"这一代查的是哪一份"记下来，再 start()：`start()` 会**同步**发一发
    // `updated`（本地能判的那几条立刻有结论），那一发里必须已经带着正确的 presetId。
    m_modCheckPresetId = presetId;
    Presets::CompatibilityContext context;
    context.gameVersion = gameVersion.trimmed();
    context.loader = loader;
    context.clientSide = clientSide;
    m_modChecker->start(modReferencesFor(presetId), context);
}

void PresetController::cancelModCheck() {
    if (m_modChecker == nullptr) {
        return;
    }
    m_modChecker->cancel();
    m_modCheckPresetId.clear();
}

int PresetController::modCheckGeneration() const {
    return m_modChecker == nullptr ? 0 : m_modChecker->generation();
}

void PresetController::setCurseForgeApiKey(const QString &apiKey) {
    m_presetApiKey = apiKey;
    if (m_modProvider == nullptr) {
        return;
    }
    m_modProvider->setCurseForge(QUrl(ModpackImport::curseForgeApiBase(apiKey,
        m_versions ? m_versions->curseForgeResolveSettings().apiBaseOverride : QString())),
                                 apiKey.toUtf8());
}

void PresetController::installModsAfterInstall(const QStringList &presetIds, const QString &instanceId,
    std::function<void(bool, const QString &)> completed) {
    installModsAfterInstallImpl(presetIds, instanceId, std::move(completed), {});
}

void PresetController::installModsAfterInstallImpl(const QStringList &presetIds, const QString &instanceId,
    std::function<void(bool, const QString &)> completed, const QJsonObject &expectedModTargets) {
    if (m_modInstallTask) { completed(false, tr("另一项模组预设仍在应用，请等待或取消。")); return; }
    if (!m_versions || instanceId.isEmpty()) { completed(false, tr("目标实例不可用。")); return; }
    // 同一道闸在真正开跑之前再问一次（和 `applyPlan()` 那道是**同一个判断**）：从
    // 「应用」按下去到这儿之间可能进过嵌套提示，期间另一趟搬文件已经起来了。
    if (m_host.isFileOperationBusy && m_host.isFileOperationBusy(instanceId)) {
        completed(false, tr("这个实例正在后台被其它操作写入（复制、还原或导入），完成后再应用模组预设。"));
        return;
    }
    QList<Presets::ModReference> references;
    QJsonArray snapshot;
    QSet<QString> seen;
    for (const auto &id : presetIds) {
        bool ok = false;
        const auto manifest = Presets::loadManifest(presetsRoot(), id, &ok);
        if (!ok) { completed(false, tr("预设已删除或损坏，无法继续应用。")); return; }
        snapshot.append(Presets::manifestToJson(manifest));
        for (const auto &ref : manifest.modReferences) {
            if (seen.contains(ref.key())) {
                for (auto &existing : references) if (existing.key() == ref.key()) {
                    if (existing.gameVersion != ref.gameVersion || existing.loader.compare(ref.loader, Qt::CaseInsensitive) != 0) {
                        completed(false, tr("同一模组存在不同版本/加载器约束，未自动选择其中一项。")); return;
                    }
                    existing.required = existing.required || ref.required;
                }
                continue;
            }
            seen.insert(ref.key()); references.append(ref);
        }
    }
    if (references.isEmpty()) { completed(true, {}); return; }
    QString loader, loaderVersion;
    m_versions->instanceLoaderInfo(instanceId, &loader, &loaderVersion);
    const QString normalizedLoader = Presets::normalizeCompatibilityLoader(loader);
    if (normalizedLoader != QStringLiteral("fabric") && normalizedLoader != QStringLiteral("forge") &&
        normalizedLoader != QStringLiteral("neoforge") && normalizedLoader != QStringLiteral("quilt")) {
        completed(false, tr("应用模组预设需要先安装模组加载器。")); return;
    }
    auto *task = new Presets::PresetModInstaller(this);
    task->setSelectionSnapshot(snapshot);
    task->setInstanceIdentityKey(instanceId);
    QHash<QString, QString> fingerprints;
    for (const auto &id : presetIds) fingerprints.insert(id, presetFingerprint(id));
    const QString requestedRoot = instanceRoot(instanceId);
    const QPointer<PresetController> guard(this);
    task->setPlanCurrent([guard, fingerprints, requestedRoot, instanceId] {
        if (!guard || guard->instanceRoot(instanceId) != requestedRoot) return false;
        for (auto it = fingerprints.cbegin(); it != fingerprints.cend(); ++it)
            if (it.value().isEmpty() || guard->presetFingerprint(it.key()) != it.value()) return false;
        return true;
    });
    task->setReviewHandler([this, task](const Presets::ModInstallPlan &plan,
                                      std::function<void(Presets::PresetModInstaller::ReviewDecision)> finish) {
        auto *dialog = new PresetModReview(plan, m_hostPage);
        connect(task, &QObject::destroyed, dialog, &QDialog::reject);
        connect(dialog, &QDialog::finished, this, [dialog, finish](int result) {
            auto decision = dialog->decision();
            decision.accepted = decision.accepted && result == QDialog::Accepted;
            finish(decision);
        });
        dialog->open();
    });
    m_modInstallTask = task;
    bool ownTask = false;
    m_modTaskId = m_resumingModTask;
    m_resumingModTask = Tasks::invalidTaskId();
    if (m_activity) {
        if (m_modTaskId == Tasks::invalidTaskId() && m_activeInstallTask) {
            const auto candidate = m_activeInstallTask();
            if (m_activity->tasks()->has(candidate)) {
                const auto current = m_activity->tasks()->snapshot(candidate);
                if (current.targetId == instanceId && current.kind == Tasks::TaskKind::Install && Tasks::isActiveState(current.state)) m_modTaskId = candidate;
            }
        }
        if (m_modTaskId == Tasks::invalidTaskId()) {
            m_modTaskId = m_activity->begin(Tasks::TaskKind::Install, tr("应用模组预设：%1").arg(displayName(instanceId)), instanceId);
            ownTask = true;
        }
        // Snapshot hashing belongs to the installer worker, after the busy guard is held.
        task->setRecovery(m_activity, m_modTaskId,
            {{"kind", "preset-mods"}, {"root", AppPaths::minecraftDir()}, {"instance", instanceId},
             {"game", m_versions->instanceGameVersion(instanceId)}, {"loader", loader},
             {"selection", snapshot}, {"modTargets", expectedModTargets}});
    }
    const auto taskId = m_modTaskId;
    // 必须在 `start()` **之前**记：`start()` 在"没有版本 / 加载器对不上"那几条路上
    // 会**同步**回调 completed，先记后清才是对的顺序。
    if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(instanceId, true);
    task->start(references, instanceRoot(instanceId), m_versions->instanceGameVersion(instanceId), loader,
                m_presetApiKey, [this, instanceId] { return m_host.isRunning && m_host.isRunning(instanceId); },
                [this, instanceId, completed, taskId, ownTask](const Presets::PresetModInstaller::Result &result) {
                    m_modInstallTask.clear();
                    m_modTaskId = Tasks::invalidTaskId();
                    if (m_host.setFileOperationBusy) m_host.setFileOperationBusy(instanceId, false);
                    if (m_host.refreshInstances) m_host.refreshInstances();
                    const QString text = result.lines.join(QLatin1Char('\n'));
                    log(text);
                    if (m_activity) {
                        // A resumed/parent-owned task must also retain user cancellation.
                        // Its completion callback cannot turn that terminal state into failure.
                        if (result.cancelled) m_activity->tasks()->cancel(taskId);
                        else if (ownTask) m_activity->finish(taskId, result.ok, result.ok ? QString() : text, false);
                    }
                    completed(result.ok, text);
                });
}

bool PresetController::pauseModDownload(Tasks::TaskId id) {
    return ownsModTask(id) && static_cast<Presets::PresetModInstaller *>(m_modInstallTask.data())->pauseDownload();
}
bool PresetController::resumeModDownload(Tasks::TaskId id) {
    return ownsModTask(id) && static_cast<Presets::PresetModInstaller *>(m_modInstallTask.data())->resumeDownload();
}
bool PresetController::continueModInstallation(Tasks::TaskId id, const QJsonObject &intent) {
    if (m_modInstallTask || !m_activity || intent.value("kind") != "preset-mods" ||
        QDir::cleanPath(intent.value("root").toString()) != QDir::cleanPath(AppPaths::minecraftDir())) return false;
    const auto instance = intent.value("instance").toString();
    if (!InstanceFiles::validateDirName(instance).isEmpty() || (m_host.isInstanceBusy && m_host.isInstanceBusy(instance)) ||
        (m_host.isRunning && m_host.isRunning(instance))) return false;
    QString loader, version; m_versions->instanceLoaderInfo(instance, &loader, &version);
    const auto targets = intent.value("modTargets").toObject();
    if (m_versions->instanceGameVersion(instance) != intent.value("game").toString() || loader != intent.value("loader").toString() ||
        targets.isEmpty()) return false;
    QStringList ids;
    for (const auto &value : intent.value("selection").toArray()) {
        const auto old = value.toObject(); bool ok = false;
        const auto manifest = Presets::loadManifest(presetsRoot(), old.value("id").toString(), &ok);
        if (!ok || Presets::manifestToJson(manifest) != old) return false;
        ids.append(manifest.id);
    }
    if (ids.isEmpty()) return false;
    m_resumingModTask = id;
    installModsAfterInstallImpl(ids, instance, [this, id](bool ok, const QString &text) {
        if (m_activity) m_activity->finish(id, ok, ok ? QString() : text, false);
    }, targets);
    m_resumingModTask = Tasks::invalidTaskId();
    return true;
}

bool PresetController::cancelModInstall() {
    if (!m_modInstallTask) return false;
    static_cast<Presets::PresetModInstaller *>(m_modInstallTask.data())->cancel();
    return true;
}

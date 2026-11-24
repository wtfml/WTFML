/**
 * @file PresetController.hpp
 * @brief 「实例预设」那一页的控制器：管理页的增删改、从实例提取、以及登录安装前的应用。
 *
 * 【这一层在干什么】
 *
 * `ui/PresetManagePage` 只画外面喂进去的值、把用户按了哪一颗发出去（页面那一层的文件头
 * 写了这条约定）。「读 `manifest.json`、建目录、算计划、备份、真正落盘」全在这儿：
 *
 * · `Presets::` 那一层（`PresetStore` / `PresetCapture` / `PresetApply`）提供判据和盘；
 * · 这一层只负责把它们串成一条用户按得动的流程，以及在页面能念的形状之间来回翻
 * （`SectionKind` 的中文名、`ApplyPlanRow` 的行文本、`PresetApplyRow` 的结果行）。
 *
 * **`ui/` 不 include `src/`**：页面收的是 `PresetManagePage::SectionItem` 这样的 POD，
 * 种类 / 模式的中文全是这一层翻好的。这个方向不许反过来。
 *
 * 【页面只建一次、留着复用】
 *
 * 和 `JavaSettingsController` 同一个理由：页面里的勾选（提取那九样）和下拉框的选择是
 * 用户的上下文，关掉再开不该丢。页面对象归 controller 所有，构造一次之后 `open()` 只刷新。
 *
 * 【页面落在实例页的第三个模式页里】
 * 它不单独占主选项卡，由实例页的模式切换控制。
 */

#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>

#include <functional>
#include <QPointer>
#include "tasks/TaskId.hpp"
class ActivityCenterController;

#include "PresetManagePage.hpp"
#include "presets/PresetApply.hpp"
#include "presets/PresetCapture.hpp"
#include "presets/PresetCompatibility.hpp"
#include "presets/PresetTypes.hpp"

class PresetEditorPage;
class QDialog;
class VersionManager;
class QComboBox;
class QWidget;

namespace Presets {
class PlatformCompatibilityProvider;
class ManifestLoader;
struct ManifestSnapshot;
class PreviewLoader;
struct PreviewSnapshot;
struct PreviewRequest;
class CatalogLoader;
struct CatalogSnapshot;
}

class PresetController final : public QObject {
    Q_OBJECT

public:
    struct Host {
        /// 实例目录名 → 显示名（空串时调用方自己会退回 id）。
        std::function<QString(const QString &)> displayName;
        /// 这个实例现在在不在跑（应用那一趟整趟不写的判据）。
        std::function<bool(const QString &)> isRunning;
        /// 一句进主日志的话。
        std::function<void(const QString &)> log;
        /// 记 / 摘"这一份实例正在搬文件"（唯一出处是 `MainWindow::installingVersionIds()`）。
        /// 模组预设那一趟要联网查版本、下模组、往 `mods/` 里写文件，这期间这一份实例
        /// 不能被启动、不能被删。
        std::function<void(const QString &, bool)> setFileOperationBusy;
        /// 这一份实例**此刻**是不是已经在别的写目录作业手里（复制实例 / 还原 / 迁移 /
        /// 导入整合包）。
        /// 应用预设同样往实例目录里写（模组预设写 `mods/`，普通预设改
        /// `options.txt` / 服务器清单等），和后台拷树撞在一起时两边都不知道对方在写
        /// 所以开跑之前先问一句；别处那几条路都能查到这个名单，预设这一页也要查。
        /// 问的必须**只是搬文件那一份**，不是"不能启动"那个合集：安装向导收尾那一步
        /// 是带着「安装」那一档占用调进来的（那是它自己的标记）。
        std::function<bool(const QString &)> isFileOperationBusy;
        /// 这一份实例此刻**任何一样**写作业占着它没有（= 不能启动的那个合集：
        /// 后台搬文件 + 正在装 + 正在补全/修复）。只给 `applyPlan()` 用。
        /// 预设页那颗「应用」是用户直接发起的，不会出现"安装向导自己挡
        /// 自己"；而它要防的不只是复制 / 还原 / 导入 —— 正被安装、正被补全文件的
        /// 实例同样不能在这时候往目录里落预设。`installModsAfterInstall()` 继续问窄的
        /// `isFileOperationBusy`：安装向导收尾那一步带着自己的标记进来，宽的会把这一趟
        /// 自己拦掉。
        std::function<bool(const QString &)> isInstanceBusy;
        /// 重灌实例清单（`MainWindow::refreshInstalledVersions()`）。模组预设收尾之后要叫
        /// 一次：这一趟记着"搬文件忙"，而刷新那条路会按 `installingVersionIds()` 把忙的
        /// 实例**整份滤掉** —— 万一这期间别的事（下载收尾、改设置）已经刷过一次，用户看到
        /// 的就是"实例没了"，不重刷它就一直不在列表里。
        std::function<void()> refreshInstances;
        /// 把一页铺到内容区上（`OverlayHost::showPage`：摆上去就返回）。预设编辑器就靠
        /// 这一条走"整页覆盖内容区"那条路 —— 和实例设置那一页同一个外壳。
        ///
        /// @p onClosed 在这一页收掉时叫**一次**，参数是 `QDialog::result()`（「确定」=
        /// `Accepted`，其余几档 = `Rejected`）。**所有收页路**（返回箭头 / Esc / 这一页
        /// 自己 accept / `dismiss()` / `evict()` / `closeOverlay()`）都会叫到它 ——
        /// 宿主那一层接的是 `OverlayHost::pageClosed()`，那是收页的唯一出口。
        ///
        /// 空 = 这个宿主没有覆盖层（验证工程那种没有主窗口的宿主）：开不了页，按"没开成"算。
        std::function<bool(QDialog *, const QString &, int, std::function<void(int)>)> showPage =
            {};
        /// 预设页页头那句副标题（「N 份预设」）。空 = 这个宿主没有页头（验证工程那种宿主）。
        /// 和实例页那句「当前文件夹：…（N 个实例）」是同一条路：页头说明由这一层灌。
        std::function<void(const QString &)> presetSubtitle = {};
        /// Optional thread-safe reader for isolated tests; production reads PresetStore.
        std::function<Presets::CatalogSnapshot(const QString &)> readCatalog = {};
        std::function<Presets::ManifestSnapshot(const QString &, const QString &)> readManifest = {};
        std::function<Presets::PreviewSnapshot(const Presets::PreviewRequest &)> readPreview = {};
    };

    PresetController(VersionManager *versions, QWidget *hostPage, Host host,
                     QObject *parent = nullptr);
    ~PresetController() override;

    /// 刷新管理页（实例页切到「预设」那一档时调用；页面是常驻的，这里只重新灌数据）。
    void open();
    void openEditor(const QString &id = QString(), bool draft = false);
    bool leaveEditor();
    void captureForEditor(const QString &instanceId);
    void setEditorPrompt(std::function<int(const QString &, const QString &, const QStringList &)> prompt) { m_editorPrompt = std::move(prompt); }

    bool catalogLoading() const;
    bool detailsLoading() const;
    bool previewLoading() const;
    /// 实例清单变了（后台装完一份 / 删掉一份 / 换了游戏文件夹）⇒ 重灌那两个实例下拉框。
    ///
    /// 页面还没建出来（用户从没切到过「预设」那一档）时什么都不做；建过的话
    /// `fillInstances()` 按 id 保留当前选择。**不能只在 `open()` 里灌**：切到别的顶栏页
    /// 之后后台装好一份实例，再切回实例页时模式行还停在「预设」上，`open()` 不会被叫到。
    void refreshInstances();

    /// 预设根目录（验证工程与安装流程都问这一处，别各自拼路径）。
    static QString presetsRoot();

    // ---------- 页面发上来的那几件事（公开出来，安装流程也能调） ----------
    void refreshList(const QString &selectId = QString());
    void selectPreset(const QString &id);
    /// @param categoryKey 页面「新建为」下拉框里那一项（`presetCategoryKey()` 的串）；
    /// 空串 / 认不出 ⇒ 按「游戏设置」建（界面上第一项就是它）。
    void createPreset(const QString &categoryKey);
    void duplicatePreset(const QString &id);
    void saveDetails(const QString &id, const QString &name, const QString &description);
    void removePreset(const QString &id);
    void setDefaultPreset(const QString &id, bool makeDefault);
    void movePreset(const QString &id, int delta);

    void captureInto(const QString &presetId, const QString &instanceId,
                     const PresetManagePage::CaptureChoice &choice);
    void captureAsNew(const QString &instanceId, const QString &name,
                      const QString &categoryKey,
                      const PresetManagePage::CaptureChoice &choice);

    void previewPlan(const QString &presetId, const QString &instanceId);
    void applyPlan(const QString &presetId, const QString &instanceId, bool replaceConfirmed);
    void applyPlanImpl(const QString &presetId, const QString &instanceId, bool replaceConfirmed, bool resume);
    void showExecutionReport(const QString &presetId, const QString &instanceId);

    // ---------- 给安装流程的只读入口 ----------
    // / 可用的预设（读得回来的那些，按下标次序）。
    QList<Presets::PresetSummary> availablePresets() const;
    /**
     * @brief 四类各自的默认项，**已经核对过、可以直接用的那一份**。
     *
     * 键 = 四类的 `presetCategoryKey()`；值 = 那一类的默认预设 id（这一类没有默认 ⇒ 不出现）。
     * 三道都在这儿落下：
     * · 显式那格优先；没表过态时，旧版那个单默认项**自己属于这一类**才顶上来
     *   （`Presets::defaultFor()` 的规矩）；混合预设不推导（不静默重组）；
     * · **默认缺失回退**：指的那一份已经不在 / 读不回来 ⇒ 当这一类没有默认，不报错；
     * · 只读一次下标 + 最多读一次那份旧默认的 manifest，**不扫盘**（安装页每次刷摘要都问它）。
     */
    QMap<QString, QString> categoryDefaultsInEffect() const;

    /// 把一份预设算成计划（安装向导的「应用预览」用它，**一个字节都不写**）。
    Presets::PresetApplyResult planOnly(const Presets::PresetManifest &manifest,
                                        const Presets::PresetApplyRequest &request) const;

    // ---------- 安装页 ----------
    /// 安装页弹窗里的**一类**（次序 = 页签次序，`key` 是 `presetCategoryKey()` 的串）。
    struct InstallCategory {
        QString key;
        QString title;
    };
    /// 四类（游戏设置 / 模组 / 资源包 / 数据包）。★ 「旧版（混合）」`Unset` **不在里面**：
    /// "不静默拆分类别"在安装页这一侧同样是"不猜它属于哪一类"。
    static QList<InstallCategory> installCategories();

    /// 安装页那个弹窗要的全部预设数据 —— **只读一趟盘**。
    ///
    /// 早先的写法是「每一类各扫一遍，最后再扫一遍找复合预设」：四类就是五趟
    /// `Presets::list()`，而每一趟都要把 store 里每份 manifest 读一遍（perf 里 1000 份
    /// 同步读是 2.2 s 一趟）。20 份预设时这一下约 150 ms；1000 份时是十来秒的冻结。
    /// 后来 `Presets::list()` 有了摘要缓存（第一趟仍 2.2 s，后面几趟 10 ms 级），但
    /// 「一趟扫完」仍然是对的：冷的那一趟省不下来。
    /// 现在一趟扫完，复合预设的 manifest 也只读一次（以前「算某一类的项」和「拼复合项」
    /// 各读一次）。
    struct InstallPickerPreset {
        Presets::PresetSummary summary;
        /// 复合预设那几段（`components`：类别键 → 子预设 id）；普通预设是空的。
        QMap<QString, QString> components;
        /// 这份数据是不是从那份 manifest 真读回来的：普通预设不读、直接算 true；
        /// 复合预设读不回来 ⇒ false（那一份一个子项都不往外给，和老写法同一条）。
        bool componentsLoaded = false;
    };
    struct InstallPickerData {
        QList<InstallPickerPreset> presets;  // 按下标次序
        /// 读不了（预设文件夹在、但不是文件夹 / 没权限读）：那句原因。空 = 读到了（哪怕一份都没有）。
        QString problem;
        /// 某一类的可用项（含复合预设里的那一子项，次序跟着父项）。认不出的 key 回空表。
        QList<Presets::PresetSummary> optionsFor(const QString &categoryKey) const;
    };
    InstallPickerData installPickerData() const;
    /// 安装页那张预设弹窗要的数据，**在后台读**：列表走 `Presets::CatalogLoader` 的异步
    /// 快照（预设页正在读的话并进同一趟），复合预设那几份 manifest 也在工作线程里读。
    ///
    /// 点「预设」那一行不再同步扫盘（20 份预设原本约 150 ms，全压在点击那一帧上）。
    /// 只要最新一次请求的结果：后发的那一次把前一次作废（`m_pickerDataGeneration`）。
    void requestInstallPickerData(std::function<void(const InstallPickerData &)> done);

    /// 安装前的计划预览（`lines` 每一项是"段名 + 那一行计划"，`summary` 是一句话）。
    struct InstallPreview {
        bool ok = false;
        bool needsConfirm = false;
        QString problem;
        QString blockedReason;
        QString summary;
        QStringList lines;
        QString presetName;
        QString fingerprint;
        /// 这一趟真会动的项数（`ApplyPlan::totalCount()`）。状态行那句「会动 12 项」用它。
        int itemCount = 0;
        /// 预设里有、这一趟不动的项数（各段 `skipped` 之和）。
        int skippedCount = 0;
        /// 这一份里**做不了的段落数**（不认识的种类 / 那一段读不回来 / 目标读不动）。
        int blockedCount = 0;
        /// 「查看计划与原因」展开之后那几行：只看得出问题的段（做不了 / 要确认 / 会删 /
        /// 有冲突 / 要人选 / 跳过了东西）。
        QStringList reasons;
    };

    /**
     * @brief 算"装好「instanceName」之后把 presetId 落上去会做什么"。
     *
     * 目标目录此刻**还没有**（实例还没装出来）：`readTargetSnapshot()` 对"没有
     * `options.txt`"是当正常新实例的，所以这就是如实的那一份计划 —— 别拿去和"装完之后
     * 再算一遍"比：那两份在通用文件组那几段上本来就该不一样（缺了才拷）。
     */
    InstallPreview previewForInstall(const QString &presetId, const QString &instanceName) const;
    QString presetFingerprint(const QString &presetId) const;

    /// 安装收尾那一步的结果。
    struct InstallApply {
        bool ok = false;
        bool needsConfirm = false;
        QString message;  ///< 给日志/提示的**一句人话**（成功是一段小结，失败是原因）
        QString backupDir;
        int written = 0;
        int removed = 0;
    };

    /**
     * @brief 把一份预设落到**刚装好的**实例上（安装流程的最后一步）。
     *
     * @param replaceConfirmed 安装前那次预览里用户已经看过「覆盖」那几行并点了同意 ⇒ true。
     * 盘上一个字节都不会动之前仍然过一遍 `PresetPlanner` 的那道闸（见 PresetApply）。
     */
    InstallApply applyAfterInstall(const QString &presetId, const QString &instanceName,
                                   bool replaceConfirmed);

    // ---------- 安装前的兼容检查 ----------
    /**
     * @brief 一次兼容检查的进度快照。
     *
     * `presetId` + `generation` 一起给出来，是因为这两样任一变了，界面手上那份就过期了
     * （用户换了一份 / 又开了一次检查）。`results` 的次序 = 引用首次出现的次序。
     */
    struct ModCheckUpdate {
        QString presetId;
        int generation = 0;
        QList<Presets::CompatibilityResult> results;
        Presets::CompatibilitySummary summary;
        /// 这一代是不是已经全部有结论（`updated` 与 `finished` 共用一个载荷）。
        bool finished = false;
    };

    /// 那一份预设里的模组引用（读不出来 / 没有引用 ⇒ 空表）。
    QList<Presets::ModReference> modReferencesFor(const QString &presetId) const;

    /**
     * @brief 开始查「这一份模组预设在这个版本 / 加载器下能不能拿到」。
     *
     * 弹窗**先出来**、这里再查：本地能判的立刻有结论，剩下的按
     * 并发上限排队；网络失败 / 超时只算「暂无法确认」，**不计入缺失**。
     * 在途期间再调一次就是新一代，旧回调作废（generation token）。
     */
    void startModCheck(const QString &presetId, const QString &gameVersion, const QString &loader,
                       bool clientSide = true);
    /// 作废当前这一代（关弹窗时调；不改变已经出来的结论）。
    void cancelModCheck();
    int modCheckGeneration() const;

    /// CurseForge 那一侧用的 key（设置里用户自己填的那一个）。空 = 那一侧一律「暂无法确认」。
    void setCurseForgeApiKey(const QString &apiKey);
    void installModsAfterInstall(const QStringList &presetIds, const QString &instanceId,
                                 std::function<void(bool, const QString &)> completed);
    bool cancelModInstall();
    bool isInstallingMods() const { return !m_modInstallTask.isNull(); }
    void setRecoveryHost(ActivityCenterController *activity, std::function<Tasks::TaskId()> active) {
        m_activity = activity; m_activeInstallTask = std::move(active);
    }
    bool ownsModTask(Tasks::TaskId id) const { return m_modInstallTask && id == m_modTaskId; }
    bool pauseModDownload(Tasks::TaskId id);
    bool resumeModDownload(Tasks::TaskId id);
    // Accepts preparation; directory validation completes asynchronously before any download/deployment.
    bool continueModInstallation(Tasks::TaskId id, const QJsonObject &intent);

signals:
    /// 兼容检查每出一条 / 收尾都会发（`finished` 那一发也在这条信号上）。
    void modCheckUpdated(const PresetController::ModCheckUpdate &update);

private:
    void installModsAfterInstallImpl(const QStringList &presetIds, const QString &instanceId,
        std::function<void(bool, const QString &)> completed, const QJsonObject &expectedModTargets);
    QString instanceRoot(const QString &instanceId) const;
    QString displayName(const QString &instanceId) const;
    void log(const QString &message) const;

    void buildPage();
    void pushCategories();
    void pushInstances();
    void pushPresetToPage(const QString &id);
    void updatePageBusy();
    void invalidatePreview();
    void showPreview(const Presets::PreviewSnapshot &snapshot);
    void pushListAndSelect(const QString &selectId);
    void pushCatalog(const Presets::CatalogSnapshot &snapshot, const QString &selectId);
    /// 这一类的预设允许收哪几样（`PresetManagePage::CaptureBit` 那九位）。
    static quint32 captureMaskFor(Presets::PresetCategory category);
    /// 掩码对应的"为什么这一类不收那几样"（挂到界面上被禁掉那几格的工具提示里）。
    static QString captureNoteFor(Presets::PresetCategory category);
    /// 把页面勾的那几样按类别闸一遍；被闸掉的样名收进 @p skipped（可为空）。
    static Presets::CaptureSelection filterCapture(Presets::PresetCategory category,
                                                   const Presets::CaptureSelection &selection,
                                                   QStringList *skipped);
    /// 类别 → 界面上的中文。★ 只有这一处（`ui/` 不认识 `PresetCategory`）。
    static QString categoryText(Presets::PresetCategory category);
    QList<PresetManagePage::SectionItem> sectionItems(const Presets::PresetManifest &manifest) const;
    QList<PresetManagePage::InstanceItem> instanceItems() const;
    QString newPresetName() const;
    QString captureSummary(const Presets::CaptureReport &report) const;
    QString rowText(const Presets::ApplyPlanRow &row) const;
    QString applyRowText(const Presets::PresetApplyRow &row) const;
    static QString kindText(const Presets::PresetSection &section);
    static QString modeText(Presets::ApplyMode mode);

    VersionManager *m_versions = nullptr;
    /// 页面落进的那个宿主要器（实例页第三页，`getInstancePresetPage()`）。
    QPointer<QWidget> m_hostPage;
    Host m_host;
    QPointer<PresetManagePage> m_page;
    QPointer<PresetEditorPage> m_editor;
    bool m_editorSaving = false;
    bool m_editorPrompting = false;
    std::function<int(const QString &, const QString &, const QStringList &)> m_editorPrompt;
    void saveEditor(bool draft, const std::function<void(bool)> &done = {});
    void closeEditor();

    Presets::CatalogLoader *m_catalogLoader = nullptr;
    /// 安装页预设弹窗那张表的代数：晚到的后台结果按它丢掉。
    quint64 m_pickerDataGeneration = 0;
    Presets::ManifestLoader *m_manifestLoader = nullptr;
    Presets::PreviewLoader *m_previewLoader = nullptr;
    /// 正在显示的那一份（应用/提取都带着它出门）。
    QString m_currentId;
    QSet<QString> m_catalogNames;
    /// 兼容检查那一套（缓存跨弹窗共享；checker / provider 都归这一层所有）。
    Presets::CompatibilityCache m_modCache;
    Presets::CompatibilityChecker *m_modChecker = nullptr;
    Presets::PlatformCompatibilityProvider *m_modProvider = nullptr;
    /// 这一代查的是哪一份（信号里原样带回，界面按它判"还是不是我现在选的那一份"）。
    QString m_modCheckPresetId;
    QPointer<QObject> m_modInstallTask;
    QString m_presetApiKey;
    ActivityCenterController *m_activity = nullptr;
    std::function<Tasks::TaskId()> m_activeInstallTask;
    Tasks::TaskId m_modTaskId = Tasks::invalidTaskId(), m_resumingModTask = Tasks::invalidTaskId();
};

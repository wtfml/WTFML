/**
 * @file PresetManagePage.hpp
 * @brief 「实例预设」整页：一层是列表，一层是选中那一份的细节。
 *
 * 【这一层不认识预设的盘】
 *
 * 和 `JavaManageDialog` 同一条约定：页面只画外面喂进来的几份值、只把用户按了哪一颗发出去。
 * 读 `manifest.json`、建目录、算计划、备份、真正落盘，全在 `src/controllers/PresetController`
 * 和 `src/presets/` 那一层。界面上那些"段种类 / 模式"的中文名也是外面翻译好再喂进来的 ——
 * `ui/` 不 include `src/`，`SectionKind` 这个枚举在这一层根本不出现。
 *
 * 【两层的形状都照着这一栏里已有的样子】
 *
 * 列表那一层和「客户端实例」「服务端」是同一页形（顶上 chip 高的搜索框 + 一排类别 chip、
 * 中间一整棵不画外框、按类别分组的树、底下那颗建东西的主操作）。细节那一层和下载页那张
 * 安装页是同一页形（顶上「← 返回」+ 名字、中间滚动区里几张卡片、主操作浮在底下正中间）。
 * 点一行进细节、按返回回列表 —— 和实例页点一行进实例设置是同一条路。
 *
 * 【为什么提取和应用的入口都在这一页上】
 *
 * 这一页要的是"新建、复制、重命名、删除和从实例提取"一整套，以及"应用前
 * 看得见精确计划"。两件事共用"选一份预设"这一个上下文，拆成两页会让用户在两页之间来回
 * 对照"我刚才选的是哪一份"。所以细节那一层分三张卡片，自上而下：**预设内容**（名字、说明、
 * 段表、这一份的增删改）→ **从实例提取**（勾几样、选实例）→ **应用到实例**（预览、确认、应用）。
 *
 * 【确认一律在页内做，不开第二个框】
 *
 * 产品方向是"不出现独立窗口"（见 `OverlayHost`）：删除和应用「覆盖」都用
 * **点两次**（第一次把按钮文字改成"再点一次确认…"，四秒后自己复位）。这既避开了
 * `execPrompt()` 在页面占着覆盖层时调不动那件事，也符合"危险动作多问一次"的本意。
 */

#pragma once

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

class PresetManagePage : public QDialog {
    Q_OBJECT

public:
    // ------------------------------------------------------------------
    // 外面喂进来的值（全是这一层能直接摆到屏幕上的形状）
    // ------------------------------------------------------------------

    /// 左栏那一份预设。
    struct PresetItem {
        QString id;
        QString name;
        QString description;
        QString updatedText;   ///< 已经翻好的"最后修改：…"（空则不显示）
        /// 这一份属于哪一类。★ 这里只是一把**不透明**的钥匙：页面拿它和筛选项的
        /// `CategoryOption::key` 逐字比、原样交给外面；四类叫什么、怎么翻，全在控制器。
        QString categoryKey;
        QString categoryText;  ///< 已经翻好的类别名（工具提示与右栏摘要里显示）
        int sectionCount = 0;
        bool isDefault = false;
        QStringList categoryKeys;
        QString iconPath;
    };

    /// 类别筛选那一排 chip 里的一项，以及"新建为"那个下拉框里的一项。
    /// `key` 空串 = 「全部」（只用于筛选，不出现在新建下拉框里）。
    struct CategoryOption {
        QString key;
        QString text;
        /// 只出现在筛选项里（「全部」/「旧版（未分类）」）—— 不让它成为"新建为"的一项。
        bool filterOnly = false;
    };

    /// 左栏底下那几份**读不回来**的（`PresetStore::BrokenPreset`）。它们不进可选列表，
    /// 但必须让用户看见 —— "我昨天那份预设不见了"和"它坏了、就在这儿"是两种心情。
    struct BrokenItem {
        QString id;
        QString reason;
    };

    /// 右栏"这一份里有什么"的一行（= 一段）。
    struct SectionItem {
        QString title;      ///< 段自己的标题，空的时候外面已经按种类翻好了
        QString kindText;   ///< 种类（「游戏设置」「MaLiLib 热键」…）
        QString modeText;   ///< 落法（「合并」「覆盖」「追加」…）
        int itemCount = 0;
        QString summary;    ///< 可选的一句补充（「其中 2 项认不出来」…）
    };

    /// 两个下拉框里的一个实例。
    struct InstanceItem {
        QString id;
        QString name;
        bool running = false;
    };

    /// 提取那一趟勾了哪几样（对应 `Presets::CaptureSelection`）。
    struct CaptureChoice {
        bool vanillaOptions = true;
        bool vanillaKeys = true;
        bool servers = true;
        bool config = true;
        bool defaultConfigs = false;
        bool resourcePacks = false;
        bool shaderPacks = false;
        bool malilib = true;
        bool malilibValues = false;
        bool dataPacks = false;

        bool isEmpty() const {
            return !vanillaOptions && !vanillaKeys && !servers && !config && !defaultConfigs &&
                   !resourcePacks && !shaderPacks && !malilib && !dataPacks;
        }
    };

    /// 「从实例提取」那十样各占一位（`setCaptureAvailability()` 的掩码用）。
    /// 顺序和 `CaptureChoice` 的字段一致，页面只认位、不认位背后的意思。
    enum CaptureBit : quint32 {
        CaptureVanillaOptions = 1u << 0,
        CaptureVanillaKeys = 1u << 1,
        CaptureServers = 1u << 2,
        CaptureConfig = 1u << 3,
        CaptureDefaultConfigs = 1u << 4,
        CaptureResourcePacks = 1u << 5,
        CaptureShaderPacks = 1u << 6,
        CaptureMalilib = 1u << 7,
        CaptureMalilibValues = 1u << 8,
        CaptureDataPacks = 1u << 9,
    };
    static quint32 captureMaskAll() { return 0x3FFu; }

    /// 计划表上的一行（外面已经用 `PresetApplyRowText()` 翻好了）。
    struct PlanLine {
        QString text;
        bool risky = false;
    };

    /// 一整份计划（`PresetPlanner::ApplyPlan` 的"给人看"版本）。
    struct PlanView {
        bool ok = false;
        QString problem;        ///< 算不出来时的一句人话
        QString blockedReason;  ///< 整趟不许开始（游戏在跑）
        bool needsConfirm = false;  ///< 有「覆盖」段在等用户点头
        bool replaceConfirmed = false;  ///< 这份计划**是按"已经确认过覆盖"算的**
        bool canApply = false;
        QList<PlanLine> rows;
        QStringList keyConflicts;   ///< 整份键位里互相抢的那几条
        QString summary;            ///< 一句总账（「4 段，共 41 项，删掉 2 项」）
    };

    struct ApplyView {
        bool ok = false;
        bool needsConfirm = false;
        QString problem;
        QStringList lines;
        QString backupDir;
    };

    explicit PresetManagePage(QWidget *parent = nullptr);

    QString titleText() const;

    /// 左栏整份换掉。@param selectId 换完之后选中哪一份（不在列表里就选第一份）。
    void setPresets(const QList<PresetItem> &presets, const QList<BrokenItem> &broken,
                    const QString &selectId);
    /// 左栏那张分组列表用的**组次序与组名**，以及「新建为」下拉框的内容。
    /// `key` 空串 = 「全部」那种纯筛选项：它不是一组，也不进「新建为」。
    /// 只摆一次（外面那几类是从枚举翻过来的，运行期间不会变）；重复调会整棵重建。
    void setCategories(const QList<CategoryOption> &categories);
    /// 这一份预设能收哪几样（掩码见 `CaptureBit`）。不许收的那几格**禁掉并清勾**，
    /// `note` 挂到它们的工具提示尾巴上（一句人话：为什么这一类不收）。
    void setCaptureAvailability(quint32 mask, const QString &note);
    /// 右栏"这一份是什么"。
    void showPreset(const QString &id, const QString &name, const QString &description,
                    const QString &categoryText, const QList<SectionItem> &sections);
    void showPresetLoading(const QString &id);
    void showPresetReadError(const QString &reason);
    /// 两个下拉框里的实例列表。@param selectId 空则保留各自原来的选择。
    void setInstances(const QList<InstanceItem> &instances, const QString &selectId = QString());
    /// 计划表。`canApply` 决定「应用」亮不亮；`needsConfirm` 决定它是不是要走"点两次"。
    void showPlan(const PlanView &plan);
    void setPlanLoading(bool loading);
    void showApplyResult(const ApplyView &result);
    /// 底下那行状态（提取结果、保存成功之类）。
    void showStatus(const QString &text);
    /// 清掉计划（换了预设 / 实例 / 段表之后调用）。
    void clearPlan();

    /// 忙的时候所有会动盘的东西都禁掉（列表本身还能选着看）。
    void setBusy(bool busy);
    bool isBusy() const { return m_busy; }

    QString currentPresetId() const;
    QString selectedApplyInstanceId() const;

    /// 用户勾出来的那一份（验证工程也能直接念这一张表）。
    CaptureChoice captureChoice() const;

    // ---------- 控件的 objectName（验证工程靠这些名字认控件） ----------
    /// 左边那张**按类别分组**的列表（`QTreeWidget`：组头 + 组里的预设行）。
    static QString presetListName() { return QStringLiteral("presetList"); }
    /// 顶层那个两层栈（0 = 列表、1 = 细节），验证工程用来认"现在露着哪一层"。
    static QString layersName() { return QStringLiteral("presetLayers"); }
    /// 类别 chip 的 objectName：`presetCategoryChip_<钥匙>`（「全部」那种空钥匙写作 `all`）。
    static QString categoryChipName(const QString &key) {
        return QStringLiteral("presetCategoryChip_") + (key.isEmpty() ? QStringLiteral("all") : key);
    }
    /// 细节那一层的「← 返回」。
    static QString backButtonName() { return QStringLiteral("presetBackButton"); }
    /// 行上那两处"这一行是谁"的位置（**验证工程按 id 认行**用）：组头那几行没有 id，
    /// 读不回来的那几行 `rowBrokenRole()` 为真。
    /// ★ 号码从 `Qt::UserRole + 20` 起：`MainWindowUI` 那一批身份/装饰角色已经占到 +16，
    /// 而 `groupIdRole()` 是 `UserRole + 1` —— 撞上去的症状是每一行都被当成组头。
    static constexpr int rowIdRole() { return Qt::UserRole; }
    static constexpr int rowBrokenRole() { return Qt::UserRole + 21; }
    static QString newCategoryComboName() { return QStringLiteral("presetNewCategoryCombo"); }
    static QString newButtonName() { return QStringLiteral("presetNewButton"); }
    static QString duplicateButtonName() { return QStringLiteral("presetDuplicateButton"); }
    static QString deleteButtonName() { return QStringLiteral("presetDeleteButton"); }
    static QString defaultButtonName() { return QStringLiteral("presetDefaultButton"); }
    static QString moveUpButtonName() { return QStringLiteral("presetMoveUpButton"); }
    static QString moveDownButtonName() { return QStringLiteral("presetMoveDownButton"); }
    static QString nameEditName() { return QStringLiteral("presetNameEdit"); }
    static QString descriptionEditName() { return QStringLiteral("presetDescriptionEdit"); }
    static QString saveDetailsButtonName() { return QStringLiteral("presetSaveDetailsButton"); }
    static QString sectionListName() { return QStringLiteral("presetSectionList"); }
    static QString captureInstanceComboName() { return QStringLiteral("presetCaptureInstanceCombo"); }
    static QString captureButtonName() { return QStringLiteral("presetCaptureButton"); }
    static QString captureAsNewButtonName() { return QStringLiteral("presetCaptureAsNewButton"); }
    static QString newPresetNameEditName() { return QStringLiteral("presetNewNameEdit"); }
    static QString applyInstanceComboName() { return QStringLiteral("presetApplyInstanceCombo"); }
    static QString previewButtonName() { return QStringLiteral("presetPreviewButton"); }
    static QString planListName() { return QStringLiteral("presetPlanList"); }
    static QString applyButtonName() { return QStringLiteral("presetApplyButton"); }
    static QString statusLabelName() { return QStringLiteral("presetStatusLabel"); }

signals:
    void editModsRequested(const QString &id);
    void editOptionsRequested(const QString &id);
    /// 用户在左栏选了一份。
    void presetSelected(const QString &id);
    void applyTargetChanged(const QString &id);
    void previewCancelRequested();

    // ---------- 增删改 ----------
    // / @param categoryKey 用户在「新建为」下拉框里选的那一类（不透明钥匙，外面翻成枚举）。
    void createRequested(const QString &categoryKey);
    void duplicateRequested(const QString &id);
    /// 名字和说明一起交出去（改名 + 改说明是一颗按钮干的两件事）。
    void detailsEdited(const QString &id, const QString &name, const QString &description);
    void deleteRequested(const QString &id);
    /// @param makeDefault 真 = 设成唯一的默认项；假 = 取消它的默认位。
    void defaultRequested(const QString &id, bool makeDefault);
    void moveRequested(const QString &id, int delta);

    // ---------- 提取 ----------
    void captureRequested(const QString &presetId, const QString &instanceId,
                          const PresetManagePage::CaptureChoice &choice);
    void captureAsNewRequested(const QString &instanceId, const QString &name,
                               const QString &categoryKey,
                               const PresetManagePage::CaptureChoice &choice);

    // ---------- 应用 ----------
    void planRequested(const QString &presetId, const QString &instanceId);
    /// @param replaceConfirmed 第二趟（用户已经点过一次"确认覆盖"）为 true。
    void applyRequested(const QString &presetId, const QString &instanceId,
                        bool replaceConfirmed);
    void executionReportRequested(const QString &presetId, const QString &instanceId);
    void resumeRequested(const QString &presetId, const QString &instanceId);

private:
    void buildUi();
    /// 第 0 层：搜索 + 类别 chip + 树 + 底下那颗「新建预设…」。
    void buildListLayer();
    /// 第 1 层：「← 返回」+ 名字、滚动区里的三小节、浮在底下正中间的「应用」。
    /// 形状照实例设置那一页：顶上一条返回行，下面是整片底色上的几小节（不套卡片）。
    void buildDetailLayer();
    /// 名称 / 说明 / 一行小字（和实例设置页顶上那块"图标 + 名字"同一个位置）。
    QWidget *buildIdentityBlock(QWidget *parent);
    QWidget *buildPresetContentSection(QWidget *parent);
    QWidget *buildCaptureSection(QWidget *parent);
    QWidget *buildApplySection(QWidget *parent);
    /// 在两层之间翻（细节那一层要把"现在看的是哪一份"写进标题）。
    void showDetailLayer(bool detail);
    /// 按 `setCategories()` 灌进来的次序把类别 chip 摆一遍（「全部」= 空钥匙那一项）。
    void rebuildCategoryChips();
    /// 亮不亮只看 `m_categoryFilterKey` 这一处（摆完、点完都走它）。
    void syncCategoryChips();
    void refreshActions();
    void refreshCaptureEnabled();
    /// 整棵树按「类别分组」重建（见文件头「左边那一列」）。
    void rebuildPresetList(const QString &selectId, bool emitSelection);
    void setCaptureCheckEnabled(QCheckBox *box, quint32 bit, quint32 mask,
                                const QString &note);
    void armDelete();
    void disarmDelete();
    void armApply();
    void disarmApply();
    void onCurrentPresetChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    /// 现在选中的那一条能不能动手（不是组头、不是读不回来的那一份）。
    bool selectionValid() const;
    /// 选中的那一条能不能设/取消默认：只有**四类**那几组里的行可以。
    /// 综合预设、旧版混合预设、草稿没有"这一类"可言（默认现在是每类各一个）。
    bool defaultAppliesToSelection() const;
    /// 同一个组里往前 / 往后那一份（`delta` = -1 / +1，跳过的不可用行）。没有就是 nullptr。
    QTreeWidgetItem *siblingPreset(int delta) const;
    void fillInstances(QComboBox *combo, const QList<InstanceItem> &instances,
                       const QString &selectId);
    static QString comboInstanceId(const QComboBox *combo);

    /// 顶层那两层：0 = 列表、1 = 细节（和实例页 / 下载页那两个栈同一个用法 ——
    /// 两层都建着，只看露哪一层）。
    QStackedWidget *m_layers = nullptr;
    QWidget *m_listLayer = nullptr;
    QWidget *m_detailLayer = nullptr;
    /// 列表那一层顶上那两行里的第二行（类别 chip）。第一行是搜索框，独占一整条。
    QHBoxLayout *m_chipRow = nullptr;
    /// 「没有符合筛选条件的预设」那一句（筛空了才露）。
    QLabel *m_noMatchLabel = nullptr;
    /// 类别 chip（第一颗是「全部」，钥匙空串）。点哪一颗就把哪一颗点亮 —— 单选。
    QList<QPushButton *> m_categoryChips;
    /// 和 `m_categoryChips` **并列**的那把钥匙（chip 不带数据槽，按下标对回去）。
    QStringList m_categoryChipKeys;
    /// 外面最后灌进来的那张类别表（重摆 chip 时照它的次序来）。
    QList<CategoryOption> m_categoryOptions;
    /// 现在亮着的那一颗的钥匙（空 = 「全部」）。
    QString m_categoryFilterKey;
    QPushButton *m_backButton = nullptr;
    QLabel *m_detailTitle = nullptr;
    /// 分组列表。**按类别分组**（组头是类别名，组里那一行一份预设）—— 和实例页 /
    /// 服务端页那两张列表同一个写法（组头粗体 + 右端箭头，见 `InstanceGroupDelegate`）。
    QTreeWidget *m_presetList = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QComboBox *m_newCategoryCombo = nullptr;
    QList<PresetItem> m_presets;
    QList<BrokenItem> m_broken;
    /// 组的先后次序和组名（`setCategories()` 灌进来）。组名这一层不认识 ——
    /// 外面翻好了再给，和别的 POD 同一条规矩。
    QStringList m_groupOrder;
    QHash<QString, QString> m_groupText;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_duplicateButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_defaultButton = nullptr;
    QPushButton *m_moveUpButton = nullptr;
    QPushButton *m_moveDownButton = nullptr;

    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_descriptionEdit = nullptr;
    QPushButton *m_saveDetailsButton = nullptr;
    QLabel *m_presetMeta = nullptr;
    QListWidget *m_sectionList = nullptr;
    /// 顶上那块图标（这份预设自带的图标；没有就是拼图）——和实例设置页那块同一个位置。
    QLabel *m_identityIcon = nullptr;
    /// 细节层左栏那条导航（概览 / 从实例提取 / 应用到实例）和右栏那个页栈。
    QListWidget *m_detailNav = nullptr;
    QStackedWidget *m_detailPages = nullptr;

    QComboBox *m_captureInstanceCombo = nullptr;
    QCheckBox *m_captureOptions = nullptr;
    QCheckBox *m_captureKeys = nullptr;
    QCheckBox *m_captureServers = nullptr;
    QCheckBox *m_captureConfig = nullptr;
    QCheckBox *m_captureDefaultConfigs = nullptr;
    QCheckBox *m_captureResourcePacks = nullptr;
    QCheckBox *m_captureShaderPacks = nullptr;
    QCheckBox *m_captureMalilib = nullptr;
    QCheckBox *m_captureMalilibValues = nullptr;
    QCheckBox *m_captureDataPacks = nullptr;
    QPushButton *m_captureButton = nullptr;
    QPushButton *m_captureAsNewButton = nullptr;
    QLineEdit *m_newPresetNameEdit = nullptr;
    /// 每一格勾选框**出厂时**的那句工具提示（`setCaptureAvailability()` 会临时往后拼一句，
    /// 反复切换类别时要能还原成原来那句，不能越拼越长）。
    QHash<QCheckBox *, QString> m_captureTips;
    /// 当前这一份预设能收哪几样（`setCaptureAvailability()` 灌进来，`refreshCaptureEnabled()`
    /// 靠它保住"这一类不许收"的格子，别被忙碌状态那一趟重新点亮）。
    quint32 m_captureMask = 0x3FFu;
    QString m_captureNote;

    QComboBox *m_applyInstanceCombo = nullptr;
    QPushButton *m_previewButton = nullptr;
    QListWidget *m_planList = nullptr;
    QLabel *m_planHint = nullptr;
    QPushButton *m_applyButton = nullptr;
    QLabel *m_statusLabel = nullptr;

    bool m_busy = false;
    bool m_planLoading = false;
    bool m_detailsAvailable = true;
    bool m_deleteArmed = false;
    int m_deleteToken = 0;
    bool m_applyArmed = false;
    int m_applyToken = 0;
    /// 这一份计划「应用」那一下要不要先点一次确认（`PlanView::needsConfirm`）。
    bool m_planNeedsConfirm = false;
    bool m_planReplaceConfirmed = false;
    bool m_planCanApply = false;
    /// 正在显示的预设 id（`showPreset` 灌进来，点按钮时带出去）。
    QString m_currentId;
    /// 灌列表时挡信号用（`setCurrentRow` 不该伪装成"用户选了一份"）。
    bool m_fillingList = false;
};

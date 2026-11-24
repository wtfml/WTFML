/**
 * @file DataPackPreset.hpp
 * @brief 「数据包」这一类预设的判据层。
 *
 * 【数据包属于世界，不属于实例】
 *
 * 模组/资源包落在实例目录里，**数据包落在某个存档里**：
 * `saves/<世界>/datapacks/`。所以"把数据包预设应用到实例上"这件事，在没有对应世界时
 * **没有可落的地方**。要求是「无目标世界时保存'待新建世界使用的模板 / 待应用清单'；
 * 不强建世界」——这一层就是照那句话做的：
 *
 * 【为什么用 `SectionKind::Custom`】
 *
 * 段种类是**盘上格式的一部分**（`sectionKindKey()` 那一串词）。给数据包新立一个
 * `SectionKind` 会让**旧版启动器**在 `sectionFromJson()` 那一步整份读不回来（那里对认不
 * 出来的种类是"整份不认"）。挂在 `Custom` 底下、用稳定的段 id 认领，旧版会把它当成
 * "没有适配器接手的段"——照样能看、能存、能复制，只是应用时那一行报"做不了"。
 *
 * 【收的是什么】
 *
 * `saves/<世界>/datapacks/` 底下**第一层**的每一个东西（一个 `.zip` 或者一个目录）。
 * 不拆到包里面的文件：数据包是游戏自己读的整份东西，我们连它的格式都不解释。
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "PresetTypes.hpp"

namespace Presets {

/// 「数据包」那一段的稳定 id（`SectionKind::Custom` 底下的 `dataPacks`）。
QString dataPackSectionId();

/// 这一段是不是数据包那一段（`kind == Custom` **且** id 对得上）。
bool dataPackHandlesSection(const PresetSection &section);

/// `wtfml.datapacks` —— 实例根下那份待应用清单的文件名（**唯一出处**）。
QString dataPackPendingFileName();

/// `<实例>/datapacks-pending/` —— 没有对应世界时那些包的落点（**唯一出处**）。
QString dataPackPendingDirName();

/// 「换掉同名那一份」时暂存/备份名字里用的短标记（目标文件名算出来的 12 位 hex）。
///
/// 这一层对外只为了验证工程：崩在两段改名之间留下的现场叫 `.wtfml-old-<标记>-<uuid>`，
/// 测试按同一套名字摆现场；该标记只能定位待检查项，不能证明目录归属。
/// 没有可靠事务登记的残留一律保留并提示人工检查，不按标记自动删除或归位。
QString dataPackReplaceToken(const QString &base);

// ---------------------------------------------------------------------------
// 段 ⇄ 值
// ---------------------------------------------------------------------------

/// 段里的一项。
struct DataPackItem {
    /// 世界目录名（`saves/` 底下一层）。
    QString world;
    /// 包的名字（`datapacks/` 底下一层，可能是 `.zip`，也可能是一个目录）。
    QString name;
    /// 相对实例根的路径：`saves/<world>/datapacks/<name>`。
    QString relativePath;
};

/// 从一段预设里把数据包项读回来。任何一条不合法 ⇒ 整段不认（`problem` 说清楚是哪条）。
bool dataPackItemsFromSection(const PresetSection &section, QList<DataPackItem> *out,
                              QString *problemOut = nullptr);

/// 反过来：把这一组项做成一段（`SectionKind::Custom` + `dataPackSectionId()`）。
bool sectionsFromDataPacks(const QList<DataPackItem> &items, QList<PresetSection> *out,
                           QString *problemOut = nullptr);

// ---------------------------------------------------------------------------
// 提取
// ---------------------------------------------------------------------------

struct DataPackCaptureReport {
    bool ok = false;
    QString problem;
    QStringList notes;     ///< 哪个世界没有 datapacks/ 之类（原样能显示）
    QStringList problems;  ///< 读不动 / 是链接的那些
    int worlds = 0;        ///< 真的收到包的世界的个数
    int packs = 0;         ///< 收到的包数
    QList<PresetSection> sections;  ///< 最多一段
};

/**
 * @brief 从实例里收数据包（**会写盘**：把包拷进 `presetFilesRoot`）。
 *
 * 没有 `saves/`、或者哪个世界都没有 `datapacks/` ⇒ `ok = true` 但一段都没有，
 * `notes` 里说一句（"这个实例还没有世界"不是错误）。
 */
DataPackCaptureReport captureDataPacks(const QString &instanceRoot,
                                       const QString &presetFilesRoot);

// ---------------------------------------------------------------------------
// 计划 / 应用
// ---------------------------------------------------------------------------

struct DataPackPlanItem {
    QString world;
    QString name;
    QString relativePath;
    /// 目标上已经有这个同名世界（部署进世界）还是还没有（记为待应用）。
    bool worldExists = false;
    /// 目标上那一份已经存在。
    bool targetExists = false;
};

struct DataPackPlan {
    bool ok = false;
    QString problem;
    QList<DataPackPlanItem> items;
    int deploy = 0;       ///< 会部署进已有世界
    int deferred = 0;     ///< 没有对应世界 ⇒ 记为待应用
    int skipped = 0;      ///< 目标上已经有（`缺了才拷`）
    int needsChoice = 0;  ///< `合并` / `追加` 对这一坨字节做不了主
};

/// 算这一趟（**只读**）。
DataPackPlan planDataPacks(const QString &instanceRoot, const QList<DataPackItem> &items,
                           ApplyMode mode);

struct DataPackApplyReport {
    bool ok = false;
    QString problem;
    int deployed = 0;
    int deferred = 0;
    int skipped = 0;
    int needsChoice = 0;
    /// 待应用清单的路径（空 = 这一趟没有待应用的）。
    QString pendingFile;
    QStringList details;
};

/**
 * @brief 把数据包这一段落下去。
 *
 * @param presetFilesRoot 预设的 `files/` 根（项里的 `relativePath` 在它底下）。
 * @param instanceRoot 目标实例目录。
 *
 * `Replace` 会先删掉目标上同名的那个包再拷（**备份是调用方的事** —— 应用引擎那一层
 * 在动手之前会把它收走）；`CopyIfMissing` 目标上有就不动；`Merge` / `Append` 一个字节
 * 都不写，只把"待选择"的数报出来。
 */
DataPackApplyReport applyDataPacks(const QString &presetFilesRoot, const QString &instanceRoot,
                                   const PresetSection &section, ApplyMode mode,
                                   bool replaceConfirmed);

/// 读 `<实例>/wtfml.datapacks` 里某个世界的待应用包名（读不到 ⇒ 空）。
QStringList pendingDataPacks(const QString &instanceRoot, const QString &world);

}  // namespace Presets

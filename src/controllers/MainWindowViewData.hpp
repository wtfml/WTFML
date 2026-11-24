/**
 * @file MainWindowViewData.hpp
 * @brief 主窗口那些"控件认的形状"：把领域数据翻成下拉框 / 实例树 / 分组菜单行 / 底栏面板行。
 *
 * 【为什么单独一层】
 *
 * 这些函数一个都不碰 MainWindow 的成员，输入是 `VersionManager` / `InstanceGroupStore` /
 * `LauncherManager` 那样的事实，输出是 `QTreeWidgetItem` 要灌的那几格、`GroupDialog::Group`
 * 那一份、`MainWindowUI::PanelRow` 那一份 —— 一句话，**翻译**。它们原来和主窗口那一万三千行
 * 挤在同一个文件里（同一个匿名 namespace），改一处要在一整片里找上下文；搬出来之后这一层的
 * 读者只需要知道输入与输出。
 *
 * 【边界】
 *
 * · 在：版本下拉框（`fillVersionCombo`）、实例树两层结构（`fillInstanceTree` /
 *   `instanceIdOf` / `instanceTreeItemText`）、分组行（`groupRowsForMenu` / `groupRowFor`）、
 *   底栏两块上拉面板的行（`runningGamePanelRows` / `recentAccountPanelRows`）、以及
 *   "默认该落哪个组"那几条判据（`defaultGroupIdFor` / `homeGroupIdOf`）。
 * · 不在：任何 `MainWindow` 成员、任何落盘、任何问话。要问用户、要写盘的一律留在上层。
 * · 不在：`ui/` 的绘制。这里只产出数据与图标，摆到哪儿由 `MainWindowUI` 决定。
 *
 * NOTE: 函数名与签名逐字沿用搬迁前那一份（MainWindow.cpp 里 `using namespace` 直接接回去），
 * 这样验证工程按名字打量它们时不受影响。
 */

#pragma once

#include <QColor>
#include <QComboBox>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QPixmap>
#include <QRect>
#include <QSet>
#include <QString>
#include <QTreeWidget>

#include <functional>

#include "auth/AccountStore.hpp"
#include "launcher/LauncherManager.hpp"
#include "launcher/RunningProcesses.hpp"
#include "controllers/InstancePagesController.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/VersionDisplay.hpp"
#include "manage/VersionManager.hpp"
#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GameWindowSettings.hpp"
#include "tasks/TaskManager.hpp"
#include "util/AppPaths.hpp"
#include "GroupDialog.hpp"
#include "InstanceGroupDelegate.hpp"
#include "VersionIcons.hpp"
#include "InstanceSettingsDialog.hpp"
#include "MainWindowUI.hpp"

namespace MainWindowViewData {

/**
 * @brief 实例树里一个组节点，以及挂在它下面的那些实例。
 *
 * 「未分组」用 groupId 为空表示 —— 它是**虚拟**的，不进 wtfml.folders，而且只有存在
 * 真实分组时才会被画出来（一个组都没有时列表是平铺的，见 fillInstanceTree）。
 */
struct InstanceGroupBucket {
    QString groupId;
    QString name;
    /// 这一组在树里**画成展开的还是收着的**（默认展开/关闭）。
    ///
    /// NOTE: 它的出处是 wtfml.folders 里那一格（InstanceGroupStore::Group::expanded），
    /// 用户点一下组行（或者齿轮菜单里那一项）就翻过来并落盘 —— 于是"下次重灌还
    /// 照这样画"。虚拟的「未分组」没有记录，恒为 true（它收起来这件事不落盘）。
    bool expanded = true;
    QList<VersionDisplayParts> parts;
};

/**
 * @brief 按 itemData 里的枚举值选中一项。
 * @return 找到并选中了返回 true；没有这一项返回 false（调用方决定要不要兜底）。
 *
 * NOTE: 枚举以 int 存进 itemData。界面这边只认"存进去的那个值"，不去 include
 * src/settings 里的枚举 —— 界面层不该为了显示一个下拉框去依赖设置模块。
 */
template <typename Enum> bool selectByEnumData(QComboBox *combo, Enum value) {
    const int index = combo->findData(static_cast<int>(value));
    if (index < 0) {
        return false;
    }
    combo->setCurrentIndex(index);
    return true;
}

/// 左面板行尾那两颗按钮的**编号**（onBottomBarPanelAction() 认的就是这两个数）。
///
/// NOTE: 编号是"这一行第几颗"，不是"这颗按钮上画着什么" —— 拿图形反认的话，哪天把电源
/// 换成终止、或者两颗换个位置，那一下点下去会静悄悄地做错事（关实例变成开设置）。
inline constexpr int kRunningPanelStopAction = 0;
inline constexpr int kRunningPanelSettingsAction = 1;
/// 「看这一场游戏的输出」（运行中的实例要能直接打开日志、停止）。**摆在最后**：
/// 前两颗是"对**这一份会话**做什么"（关了它 / 改它的设置），这一颗是"看它说了什么"，
/// 最不打紧的一下摆边上。
inline constexpr int kRunningPanelLogAction = 2;

/// 右面板里那一行右边的那颗按钮的下标（onBottomBarPanelAction() 认的就是它）。
inline constexpr int kAccountPanelUseAction = 0;
/// 「管理账号与皮肤」「添加账号」「登录」那三行菜单项的身份键（不是任何账号的 uuid，面板回调靠它分叉）。
inline const QString kAccountPanelManageKey = QStringLiteral("__wtfml_manage_accounts__");
inline const QString kAccountPanelAddKey = QStringLiteral("__wtfml_add_account__");
inline const QString kAccountPanelLoginKey = QStringLiteral("__wtfml_login__");
/// 「退出登录」那一行（只在当前有登录身份时摆）：清掉盘上的登录状态，账号留在列表里。
inline const QString kAccountPanelLogoutKey = QStringLiteral("__wtfml_logout__");
/// 账号弹层最多列几个账号（最后登录的三个，用户原话；再多的去「管理账号与皮肤」里找）。
inline constexpr int kAccountPanelLimit = 3;

QString selectedVersionId(const QComboBox *combo);
QList<Tasks::TaskStep> taskStepsFromNames(const QStringList &names, int runningIndex);
/// 下载页整合包一键安装任务在任务中心的 6 步清单（初始状态：第 0 步运行中，其余待办）。
QList<Tasks::TaskStep> modpackOneClickSteps();
void fillVersionCombo(QComboBox *combo, const QList<VersionDisplayParts> &parts);
QString instanceIdOf(const QTreeWidgetItem *item);
QString instanceTreeItemText(const VersionDisplayParts &part, bool running);
void fillInstanceTree(QTreeWidget *tree, const QList<InstanceGroupBucket> &buckets,
                      const QSet<QString> &runningIds,
                      const QHash<QString, QIcon> &marks, const QString &pinnedGroupId,
                      const QSet<QString> &pinnedInstanceIds);
void syncTreeToCombo(const QComboBox *combo, QTreeWidget *tree);
/// 把实例树的滚动位置放回重建前那儿（灌树 + 选当前项之后调一次）。
void restoreTreeScroll(QTreeWidget *tree, int value);
QList<InstanceGroupStore::Group> toStoreGroups(const QList<GroupDialog::Group> &from);
QList<GroupDialog::Group> toDialogGroups(const QList<InstanceGroupStore::Group> &from);
QList<MainWindowUI::GroupRow> groupRowsForMenu(const MainWindowUI *ui,
                                              const InstanceGroupStore *store,
                                              const QString &selectedKey,
                                              const QString &selectedHomeGroupId);
bool groupRowFor(const MainWindowUI *ui, const InstanceGroupStore *store, const QString &groupId,
                 MainWindowUI::GroupRow *row, const QString &selectedKey = QString(),
                 const QString &selectedHomeGroupId = QString());
QRect gearAnchorFor(const MainWindowUI *ui, const QString &groupId);
JavaSettingsOverride toSettingsOverride(const InstanceSettingsDialog::Override &from);
GameWindowOverride toWindowOverride(const InstanceSettingsDialog::Override &from);
QString instanceLoaderName(const VersionDisplayParts &parts, const VersionManager *versions,
                           QString *versionOut = nullptr);
QString loaderGroupKey(const QString &loaderName);
QPixmap instanceRowMark(const VersionDisplayParts &part, const VersionManager *versions, int size);
QSet<QString> knownGroupIdsOf(const InstanceGroupStore *groups);
QString defaultGroupIdFor(const VersionDisplayParts &part, const VersionManager *versions,
                          const QSet<QString> &known);
QString homeGroupIdOf(const InstanceGroupStore *groups, const QString &key,
                      const VersionDisplayParts &part, const VersionManager *versions,
                      const QSet<QString> &knownGroupIds);
QList<MainWindowUI::PanelRow> runningGamePanelRows(QList<RunningProcesses::Entry> processes,
                                                  const VersionManager *versions,
                                                  const QColor &ink, qint64 nowMs,
                                                  const std::function<QString(int)> &playTimeText);
QList<MainWindowUI::PanelRow> recentAccountPanelRows(const QList<SavedAccount> &accounts, int limit,
                                                     const QHash<QString, QPixmap> &faces);

}  // namespace MainWindowViewData

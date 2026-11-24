/**
 * @file InstallPageController.hpp
 * @brief 「安装页」那一层的**选择状态**（要装哪个原版、选哪条加载器、它钉了哪一版、
 *        名字框里那份要不要跟着加载器变），以及把它画回那棵树上、把版本列表灌进去的那几条。
 *
 * 【为什么单独一层】
 *
 * 安装页有四格状态是**成套的**：这一页正对着哪个原版号、选中了哪一条加载器、每条加载器
 * 钉住的版本号、以及"用户自己改过名字没有"。它们一起决定三件看得见的事 —— 那棵树长什么样
 * （`updateLoaderRows()`）、名称框里默认写什么（`defaultInstanceName()`）、以及点「安装」
 * 时到底装什么。原来散在 `MainWindow` 上，于是"进安装页要清哪几样"这件事得靠读
 * `openInstallPage()` 全文才知道，漏一样就是"上一次顺手选的 Forge 跟着下一个版本进来了"
 * 那种说不清的症状。
 *
 * 【这一层只碰界面，不碰盘、不联网】
 *
 * * 真正发安装请求的是本层的 `installClicked()`（它读这一层的
 *   `selectedLoader()` / `pinnedLoaderVersion()`）；
 * * 版本列表的问与答在 `VersionManager`（这一层只负责摆占位、把回复灌成子条目）；
 * * 名字合不合法、盘上有没有重名，判据在 `InstanceFiles::validateDirName()` 与
 *   `AppPaths::versionDir()`（这一层只用结论）。
 *
 * 「实例预设」那一行也在这一层：四类里各选了哪一份（`m_presetChoices`）、点它开的那张
 * 弹窗、以及那一行第二行小字的摘要。下载收尾那一头（"这一次安装已经排上的预设"，
 * 跟着下载走）不在这里 —— 它在 `DownloadCompletionController`：那是另一件事，写它的是
 * 三处（安装按钮 / 断点续装 / 升级那一趟），读它的是收尾那一发。
 */

#pragma once

#include "download/LoaderMeta.hpp"
#include "PresetController.hpp"

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>

class MainWindow;
class QTreeWidgetItem;

class InstallPageController final : public QObject {
public:
    explicit InstallPageController(MainWindow *owner);

    /// 进安装页：@param gameVersion 树上点中的那个原版号（这一份要基于它装）。
    ///
    /// 每次进都**回到「不装（纯原版）」**、清空钉住的版本号 —— 上一个实例选的加载器跟
    /// 这一个没关系。名称框里那份"用户改过没有"也一起清（默认名重新跟着加载器走）。
    /// 用户已经挑好的**预设**不在这里清（见文件头最后那段）。
    void openPage(const QString &gameVersion);
    /// 「← 返回版本列表」：回树那一层。**什么都不清**。
    void backToVersionTree();

    /// 点「安装」：把这一页的选择变成一次安装请求。原来在
    /// `MainWindow::onInstallButtonClicked()`（连同 `pickInstallerJava()` 一起搬过来）。
    ///
    /// 它先问三件事（有没有别的下载在跑 / 原版号在不在 / 名字合不合法），再摊开预设计划、
    /// 挑安装器要的 Java，最后**先记「安装」那一档占用再置忙**（顺序见那段的 NOTE）。
    void installClicked();

    /// 装加载器那一路要**先**挑出跑安装器的 Java：Forge / NeoForge / OptiFine 的安装器
    /// 自己是个 Java 程序，要的环境就是"这个版本要的 Java"（判据在
    /// `JavaSettingsController::installerJavaFor()`，这里只负责问与弹窗）。
    ///
    /// 两处调用：「安装」按钮（本层），和「实例更新」那一趟准备核心的时候
    /// （`InstanceUpdateFlow`）。**不放回 MainWindow**：参数里那个加载器下标就是这一页
    /// 那棵树的下标（`loaderKindFromIndex`），摆在别处只会多一处翻译。
    bool pickInstallerJava(const QString &gameVersion, int loaderIndex, QString *out);

    /// 选中某一条加载器（传「不装（纯原版）」那个下标 = 取消加载器）。
    /// 走这一句的是**点一条版本号**（选中它爹那条加载器）和**点「不装」**那一条 ——
    /// 点一条加载器**不**选中它，那一下只展开/收起。**不发任何请求**：版本列表是进安装页
    /// 时一次性全问好的。
    void selectLoader(int loaderIndex);
    /// 刷新那棵树的样子（哪一条选中、其余几条"与 X 不兼容"并变灰、选中那条的第二行小字
    /// 写它钉的哪一版）。**那棵树的样子全在这一处决定**，忙态变化之后也必须再调一次。
    void updateLoaderRows();

    /// 名称框里的默认名（用户没自己改过时才动它），再刷下面那句小字。
    void updateInstallName();
    /// 名称框下面那句小字 + 「安装」按钮的可用性（名字合不合法、盘上有没有重名）。
    void updateInstallNameState();
    /// 名称框里每敲一个字：记下"用户动过这个名字了"，再重算那句小字。
    void noteNameEdited();

    /// 四个加载器能装哪些版本，**进安装页时一次性全问出去**（不能点开了再获取）。
    void requestAllLoaderVersions();
    /// 把一份版本列表灌成某个加载器的子条目。
    /// @param includeChannels 要不要头两条那两档（「下载最新版」/「下载稳定版」）；
    ///        **"这一版一个能装的都没有"时必须传 false**（留着等于让用户挑一条必然装不
    ///        出来的路）。列表**没取到**时传 true —— 那两档不依赖这份列表。
    void fillLoaderVersionItems(int loaderIndex, const LoaderMeta::LoaderVersionList &versions,
                                bool includeChannels);
    /// 版本列表的回复：填进**那一条**底下的子条目。讲的是别的游戏版本的回复直接丢掉。
    void onLoaderVersionsFetched(const QString &gameVersion, int loaderIndex, bool success,
                                 const LoaderMeta::LoaderVersionList &versions,
                                 const QString &errorMessage);

    /// 这一页现在对着哪个原版号（空串 = 没进过安装页）。
    const QString &gameVersion() const { return m_gameVersion; }
    /// 现在选中的是哪一条加载器（`modLoaderOptionTexts()` 的下标；「不装」也是一个合法值）。
    int selectedLoader() const { return m_selectedLoader; }
    /// 某条加载器钉住的那一格：一个具体版本号 / `@latest` / `@stable` / 空串（没挑过）。
    /// **别当成"一定是版本号"** —— 空串在数据层那边按稳定档算。
    QString pinnedLoaderVersion(int loaderIndex) const { return m_chosenVersion.value(loaderIndex); }
    /// 用户点了某条加载器底下的那一格（一个版本号 / 那两档的键）：把它钉住。
    /// 只记，**不改选中**（选中是 `selectLoader()` 的事）——
    /// 点一条版本号那一下两条都要走，顺序是先钉再选。
    void setPinnedLoaderVersion(int loaderIndex, const QString &version) {
        m_chosenVersion.insert(loaderIndex, version);
    }
    /// 点「预设」那一行：开那张紧凑弹窗（**不跳页**）。
    ///
    /// 弹窗里改的只落在本地草稿上：取消 / 「前往预设管理」都不写 `m_presetChoices`
    /// （取消不丢安装配置）。只有点「确定选择」才整张表按弹窗里那几项重建。
    void openPresetPicker();
    /// 把「预设」那一行第二行小字刷成当前的摘要（「未选择」/「已选择 N 类」）。
    /// **唯一出处** —— 进安装页、弹窗点确定、装完一单之后都走它。
    void updatePresetSummary();
    /// 现在四类里选中了哪几份（只含非空 id，按四类的次序）。
    ///
    /// **含各类的默认项**：用户没在弹窗里表过态的那一类，用 `PresetController::
    /// categoryDefaultsInEffect()` 给的默认；表过态（哪怕选的是"不使用预设"）就听用户的。
    /// 摘要那句话和点「安装」时摊开的那份计划都读这一份 —— 两处必须是同一份答案。
    QList<QPair<QString, QString>> currentPresetSelections() const;

    /// 这份要装的实例默认叫什么（目录名的形状 `1.18.2-Forge_40.3.12`）。
    QString defaultInstanceName() const;

private:
    /// 那棵树里某一条加载器的条目；找不到回 nullptr。
    QTreeWidgetItem *loaderItem(int loaderIndex) const;
    /// 那棵树里「预设」那一行（`UserRole == installPresetRowRole()`）；找不到回 nullptr。
    QTreeWidgetItem *presetRowItem() const;

    MainWindow &m_owner;
    /// 这一页正对着哪个原版号（空串 = 没进过安装页）。
    QString m_gameVersion;
    /// 选中的是哪一条（`MainWindowUI::modLoaderNoneIndex()` = 不装加载器）。
    ///
    /// NOTE: 初值在 .cpp 的构造里给 —— 这个头只前置声明了主窗口，而那个值是
    /// `MainWindowUI` 的静态函数（同主窗口原来那条 NOTE 的理由）。
    int m_selectedLoader;
    /// 每条加载器**钉住**的那一格（下标 → 版本号 / 档位键）。没进过 map 的下标 = 没挑过。
    /// 进安装页时整个清掉（换了一个游戏版本，上一份能装的版本就完全不是这一份了）。
    QHash<int, QString> m_chosenVersion;
    /// 用户**自己改过**名称框没有。改过之后默认名就不再跟着加载器变。
    bool m_nameTouched = false;
    /// 「预设」那一行：四类**各一格**（key = `Presets::presetCategoryKey()` 的串，
    /// value = 预设 id）。
    ///
    /// ★ **键在 = 用户在弹窗里表过态**：值是空串 = 他明确说"这一类不要"；
    /// 键不在 = 没表过态 ⇒ 这一类跟着它的**默认项**走（`categoryDefaultsInEffect()`）。
    /// 只有点「确定选择」时才整张表重建 —— 取消、点「前往预设管理」都不动它。
    /// **不跟着翻页重置**：换一个版本再看一眼，不该让他已经挑好的那几类悄悄没了。
    QMap<QString, QString> m_presetChoices;
};

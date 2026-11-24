/**
 * @file MainWindowInstanceEntry.cpp
 * @brief 进实例设置 / 快速游玩：那一扇窗怎么开、开在哪一页，以及快速游玩那两发。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"用户要从哪一页进那个实例"：`openInstanceSettings()` 是全窗唯一的入口
 * （空 id 先判、被占着就静默让位再排到下一轮）、`openInstanceSettingsNow()` 是让位之后的
 * 那一发、`instanceSupportsQuickPlay()` / `startQuickPlay()` / `startQuickPlayNow()` 是
 * 服务器列表与主页直达的那条路（真的起进程在 `LaunchController`）。
 *
 * 【判据不在这一层】
 *
 * "实例支持哪些快速游玩"由 `QuickPlay::` 那几家判；目录表在
 * `InstancePagesController`，界面上的几条常用目录是 `InstanceSettingsDialog` 的事。
 */

#include "MainWindow.hpp"

#include "..\ui\InlineMessage.hpp"
#include "..\ui\InstanceSettingsDialog.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "..\ui\OverlayHost.hpp"
#include "controllers/InstanceDialogController.hpp"
#include "controllers/InstancePagesController.hpp"
#include "controllers/LaunchController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "launcher/QuickPlay.hpp"
#include "manage/VersionManager.hpp"
#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "util/AppPaths.hpp"
#include "util/SystemMemory.hpp"

using namespace MainWindowViewData;

void MainWindow::openInstanceSettings(const QString &versionId, const QString &pageKey) {
    if (versionId.isEmpty()) {
        InlineMessage::information(m_ui->overlay(), tr("实例设置"), tr("请先选择一个实例。"));
        return;
    }
    // 这一层被别的页面/提示占着时：**静默把它收掉**，然后把"开"排到下一轮（见
    // runWhenOverlayFree 那段）。空着的时候同一轮就开出来。
    // NOTE: 那一句空 id 的判断必须排在**这一发之前** —— 它是"用户还没选实例"这种普通
    // 情况，不该顺手把别的页面收掉。
    runWhenOverlayFree([this, versionId, pageKey] { openInstanceSettingsNow(versionId, pageKey); });
}

bool MainWindow::instanceSupportsQuickPlay(const QString &versionId,
                                           QuickPlay::Kind kind) const {
    if (m_versionManager == nullptr) {
        return false;
    }
    return QuickPlay::supports(m_versionManager->getVersionJson(versionId), kind);
}

void MainWindow::startQuickPlay(const QString &versionId, QuickPlay::Kind kind,
                                const QString &value) {
    // 第一道闸：**这个版本认不认这一条参数**（判据见 QuickPlay，界面上那两颗按钮在支持的
    // 版本上才露，这是兜底）。发一条游戏不认识的选项不是"被忽略"，是 jopt-simple 抛
    // UnrecognizedOptionException、游戏当场退出 —— 所以宁可在这儿说一句。
    if (!instanceSupportsQuickPlay(versionId, kind)) {
        InlineMessage::warning(m_ui->overlay(), tr("当前版本不可用"),
                               QuickPlay::unsupportedText(kind));
        return;
    }

    QuickPlay::Target target;
    target.kind = kind;
    target.value = value;
    // 让位 + 排到下一轮：调用点在这个信号里，实例设置那一页正卡在它自己的嵌套事件循环里，
    // 当场去启动等于"页还开着就开始挑 Java / 下运行时"（见 runWhenOverlayFree 那段）。
    runWhenOverlayFree([this, versionId, target] { startQuickPlayNow(versionId, target); });
}

void MainWindow::startQuickPlayNow(const QString &versionId, const QuickPlay::Target &target) {
    // 启动**已经在走**的时候又来了这一发（换令牌 / 下 Java 的同步等待里，
    // 用户回到实例设置页点了「加入世界」）。这一发不能排进 `m_pendingQuickPlay` ——
    // 正在走的那一趟在开头就把待办取走了，这里排上的会一直留到**下一次**用户自己点
    // 「启动」时才被消费：游戏莫名其妙地进了一个他早就忘了的世界。直接丢掉并说一句。
    if (m_launchController->isStarting()) {
        InlineMessage::warning(m_ui->overlay(), tr("正在启动"),
                               tr("启动流程正在进行中（校验凭据与运行时），请稍候。"));
        return;
    }
    QComboBox *combo = m_ui->getInstalledVersionCombo();
    const int row = combo == nullptr ? -1 : combo->findData(versionId);
    if (row < 0) {
        // 这一页开着的时候那个实例被删了 / 改名了（下拉框按 id 找不到了）。**不启动**：
        // 启动走的是"下拉框里选中的那一个"，硬塞一个找不到的值只会开起别的实例来。
        InlineMessage::warning(m_ui->overlay(), tr("实例未找到"),
                               tr("未找到已安装实例「%1」，启动已取消。")
                                   .arg(instanceDisplayName(versionId)));
        return;
    }
    // 排上这一发，再把下拉框切到那个实例 —— 顺序不能反：`setCurrentIndex()` 那一发会同步
    // 牵动一堆界面（启动按钮、底栏、实例列表的选中），中间任何一处都不该看见"有 quick play
    // 但版本还没切过去"的状态。
    m_pendingQuickPlay = target;
    combo->setCurrentIndex(row);
    // 走**正常那条启动路**：令牌 / 挑 Java / 启动提醒 / 「正在下载的那一个不能启动」
    // 那几道闸一道都不绕过（要的是"试试看能不能生效"，而不是"绕过启动器直接塞参数"）。
    onStartButtonClicked();
}

void MainWindow::openInstanceSettingsNow(const QString &versionId, const QString &pageKey) {
    m_instanceDialogController->open(versionId, pageKey);
}

QHash<QString, QString> MainWindow::instanceFileDirs(const QString &versionId) const {
    return InstancePagesController::fileDirs(versionId);
}

QStringList MainWindow::instanceCommonFolderKeys(const QString &versionId,
                                                 const QHash<QString, QString> &dirs) const {
    // 同前：判断在 `InstancePagesController::commonFolderKeys()`。那一份"有哪几条常用目录"
    // 是界面的事（`InstanceSettingsDialog::shortcutPathKeys()`），所以从这儿递进去。
    return InstancePagesController::commonFolderKeys(versionId, dirs,
                                                     InstanceSettingsDialog::shortcutPathKeys());
}

StartupDiagnosis::Inputs MainWindow::startupDiagnosisInputs(const QString &versionId) {
    StartupDiagnosis::Inputs inputs;
    if (versionId.isEmpty()) return inputs;
    inputs.instanceName = instanceDisplayName(versionId);
    inputs.versionId = versionId;
    const VersionDisplayParts parts = m_versionManager->displayParts(versionId);
    inputs.gameVersion = parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
    // 加载器名 / 版本走 `InstancePagesController::loaderName()`，**不直接读**
    // `parts.loaderName`：那一格只有我们自己的安装器会写，合并实例（加载器只写在
    // 版本 json 的 `libraries` 里）在那儿是空的，诊断会把 Fabric 实例说成原版。
    QString loaderVersion;
    inputs.loaderName = InstancePagesController::loaderName(parts, m_versionManager, &loaderVersion);
    inputs.loaderVersion = loaderVersion;
    inputs.requiredJavaMajor = m_versionManager->getVersionJson(versionId)
                                  .value(QStringLiteral("javaVersion"))
                                  .toObject()
                                  .value(QStringLiteral("majorVersion"))
                                  .toInt(-1);

    const JavaSettingsOverride override = javaOverrideFromJson(m_versionManager->versionMeta(versionId));
    const EffectiveJavaSettings java = resolveJavaSettings(m_javaSettings->effective(), override);
    inputs.configuredJavaPath = java.javaPath;
    inputs.autoSelectJava = m_javaSettings->autoSelectJava() && !override.javaPath.has_value();
    inputs.autoMemory = java.autoMemory;
    const SystemMemory::Usage usage = SystemMemory::read();
    inputs.systemTotalMb = usage.totalMb;
    inputs.systemAvailableMb = usage.availableMb;
    if (java.autoMemory) {
        inputs.xmxMb = GlobalJavaSettings::suggestXmxMb(usage.totalMb, usage.availableMb,
                                                       instanceModCount(versionId));
        inputs.xmsMb = GlobalJavaSettings::suggestXmsMb(inputs.xmxMb);
    } else {
        inputs.xmxMb = java.xmx;
        inputs.xmsMb = java.xms;
    }

    inputs.modsDir = InstancePagesController::fileDirs(versionId).value(QStringLiteral("mod"));
    inputs.logPath = AppPaths::minecraftDir() + QStringLiteral("/logs/wtfml.log");
    inputs.crashDir = AppPaths::crashReportsDir();
    return inputs;
}

#pragma once
#include <QHash>
#include <QObject>
#include "download/ResourceCatalog.hpp"
#include <QPointer>
#include <QSet>
#include "download/ModDependencies.hpp"
#include "manage/InstanceIconLookup.hpp"

class MainWindow;
class InstanceSettingsDialog;
class QThread;

// Owns this workflow and its transient state; shared session caches stay with MainWindow.
class InstanceUpdateCheckController final : public QObject {
public:
    explicit InstanceUpdateCheckController(MainWindow *owner);
    ~InstanceUpdateCheckController() override;
    void waitForLookup();
    void checkInstanceUpdates(const QString &instanceId, InstanceSettingsDialog *dialog,
                                     const QStringList &selectedPaths);
    void onUpdateCheckLookupFinished(const InstanceIconLookup::Result &result,
                                            const QString &instanceId);
    void requestUpdateCheckVersions(const QString &requestKey);
    void onUpdateCheckLatestFinished(
    const QString &requestKey, const QHash<QString, ResourceCatalog::VersionEntry> &latest,
    const QString &problem);
    void onUpdateCheckVersionsFinished(const QString &requestKey,
                                              const QList<ResourceCatalog::VersionEntry> &versions,
                                              const QString &problem);
    void finishUpdateCheck(QString instanceId);
private:
    MainWindow &m_owner;
    struct UpdateCheckRun {
        QSet<QString> selectedScope; // Empty means a full-instance check.
        /// 这一趟查的是哪个实例（回来时按它认人：用户在等着的时候完全可能换了实例设置页）。
        QString instanceId;
        /// 还差几发没回来。**只有它到 0 才收尾** —— 少等一发的话，报告里会凭空少几条，
        /// 而屏幕上一点异常都没有（"这个模组已经是最新的"和"没查到"是两句话）。
        int pending = 0;
        /// 钥匙（`versionsRequestKey`）→ 等这个答案的那几条文件。
        ///
        /// 为什么要按钥匙分组、而不是"一个项目一个条目"：同一个资源在 mods/ 里摆两份是
        /// 常事（`X.jar` 和 `X.jar.disabled` 也在候选里），两份**同一次答案就能判**，
        /// 所以一发请求判两条。**反查那一趟回来之后才有它**（那时才知道哪个文件是哪个项目）。
        QHash<QString, QList<ModDependencies::InstalledItem>> itemsByKey;
        QSet<QString> waitingKeys;
        /// NOTE: 同一个 sha1 底下可能挂着**两条路径**（`X.jar` 和 `X.jar.disabled`）——
        /// 那一发只问一次，两条照同一份答案判。
        QHash<QString, QHash<QString, QList<ModDependencies::InstalledItem>>> batchItems;
        /// 批量那一发的钥匙 → 发那一发时要递回去的**加载器**（空串 = 不带这一格）。
        /// 刻意**不放进钥匙里再拆**：拆字符串的写法一改就静默对不上（同 `versionsKeyFor`
        /// 那段说的道理），而从 `batchItems` 里读第一条也在读同一个值 —— 存一份最省事。
        QHash<QString, QString> batchLoader;
        QSet<QString> pendingBatchKeys;
        /// 批量那几发**一共答出来过哪些 sha1**（跨所有批量钥匙攒的一份）。
        ///
        /// 只给"退回问 CurseForge"那一拨当判据用（"Modrinth 到底答没答过这一个哈希"）——
        /// 批量分了几发之后，单看某一发的回复就判不全了，见 `onUpdateCheckLatestFinished()`
        /// 里那两段。
        QSet<QString> batchAnswered;
        /// 这一个文件的路径在哪一类目录下（`kindUsesLoader()` / 后面那些按类分岔的判据的
        /// 输入）。**反查那一趟回来时按它认回类别** —— 那趟只回一个路径，而"这个路径属于
        /// 哪一类"是**收集候选文件那一步**就已经知道的事（目录就是按类走出来的）。
        QHash<QString, ResourceCatalog::Kind> pathKinds;
        /// 现在**一个文件只查一家**：两家都有时走 Modrinth（这一栏就是等它先答），只有
        /// CurseForge 认出来的照旧直接问 CurseForge。而 Modrinth 那一发**没给出
        /// 这个 sha1 的答案**时（作者只往 CF 传过那一版 / 这个哈希不在 Modrinth 的表里），
        /// 这一栏**退回问 CurseForge** —— 见 `onUpdateCheckLatestFinished()` 那一处。
        ///
        /// NOTE: 这一栏非空 ⇒ `batchItems` 非空（能押在这儿的前提就是"同一个文件 Modrinth
        /// 也认出来了"，那种文件一定在 `batchItems` 里）—— 所以"只有它非空才收尾"那句
        /// 判据（`pendingBatchKeys` 非空）一定立着。
        QHash<QString, QList<ModDependencies::InstalledItem>> fallbackItems;
        QList<ModDependencies::UpdateVerdict> verdicts;
        /// NOTE: 值就是 `judgeUpdate()` 看的那一份 `versions.first()`（判"最新"和拿"最新
        /// 那一版"必须是同一句话，不另算一份）。
        QHash<QString, ResourceCatalog::VersionEntry> latestByPath;
        QString lookupNote;
        /// 点「检查更新」那一颗按钮时开着的那一页。**弱引用** —— 结果回来的时候这一页
        /// 完全可能已经被收掉了（那几发是异步的），那时候**不碰它**（往一个已经析构了的
        /// 对话框上写是 use-after-free，不是"多画一笔"；同 ServerPingRun::dialog 那段）。
        QPointer<InstanceSettingsDialog> dialog;
        /// 跑着的时候就把重活派出去的那个线程（跑完 deleteLater，见 startInstanceFileIconLookup
        /// 那套写法）。
        QThread *thread = nullptr;
    };
    UpdateCheckRun m_updateCheck;
    QHash<QString, QPointer<InstanceSettingsDialog>> m_pendingChecks;
    QHash<QString, QString> m_lastIncompleteReports;
};

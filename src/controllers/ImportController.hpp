#pragma once
#include <QHash>
#include <QObject>
#include "download/ResourceCatalog.hpp"
#include "manage/ModpackImport.hpp"
#include "operations/OperationReport.hpp"
#include "tasks/TaskSnapshot.hpp"
class MainWindow;
class QWidget;
class ImportController final : public QObject {
    Q_OBJECT
public:
    explicit ImportController(MainWindow *owner);
    void onImportModpackClicked();
    void openImportReport(const QString &instanceId);
    void importModpackFromFile(const QString &zipPath, const QString &namePrefill);
    void importModpackOneClick(const QString &zipPath, const QString &confirmedInstanceName,
                               Tasks::TaskId taskId);
    bool applyModpackImport(const QString &zipPath, const QString &instanceName, const QByteArray &manifestJson, ModpackImport::Format format);
    bool installLocalResourceFile(const QString &versionId, ResourceCatalog::Kind kind, const QString &path, QString *problemOut);
    /// 这一批同类文件里用户按了「取消」：剩下的不再逐个问，直接停。
    bool batchCancelled() const { return m_batchCancelled; }
    /// 上一份没做成的单文件复制报告（`<报告>.failure.txt`）；空 = 没有。
    QString lastFileCopyFailureReport() const { return m_lastFileCopyFailureReport; }
    void handleInstanceSettingsDrop(QWidget *page, const QString &versionId, const QStringList &paths);
    void importDroppedModpack(const QString &zipPath, const QString &namePrefill);
    void handleWindowDrop(const QStringList &paths);
private:
    bool copyLocalResourceFile(const QString &versionId, const QString &lockRoot,
                               const QString &dir, const QString &path, QString *problemOut);
    struct PreparedModpack;
    bool prepareModpackPackage(const QString &zipPath, PreparedModpack *out);
    bool resolveCurseForgeManifestIfNeeded(ModpackImport::Pack *pack, ModpackImport::Format format,
                                           const QString &sourceName, QString *problemOut);
    bool promptCurseForgeFileIssuesIfNeeded(const ModpackImport::Pack &pack, const QString &instanceName);
    bool resolveInstallerJavaForPack(const ModpackImport::Pack &pack, bool allowInteractivePrompt,
                                     QString *installerJavaOut, bool *cancelledOut, QString *reasonOut);
    void finishAbortedImportTask(Tasks::TaskId taskId, bool cancelled, const QString &reason);
    void repairFromReport(const QString &reportPath);
    /// 把 MultiMC 包的原启动补丁留在实例目录（导出时原样带回）。失败只写日志，不影响导入。
    /// 单文件复制也走统一的 `OperationReport`：逐项结果 + 不可覆盖时的原因报告。
    void recordFileCopyReport(const QString &versionId, const QString &sourcePath,
                              const QString &targetPath,
                              OperationReport::ConflictPolicy policy, bool skipped, bool done,
                              const QString &problem);
    /// 「已经解析好的包」那半截：加载器检查、CurseForge 换地址、重名复查、挑安装器 Java、
    /// 置忙态、交给数据层。界面上那一半（问名字、确认框）和验证工程那一半都落到这里。
    bool applyResolvedModpackImport(const QString &zipPath, const QString &instanceName,
                                    ModpackImport::Pack pack,
                                    const QString &manifestFingerprint,
                                    const QString &preselectedInstallerJava = QString());
    MainWindow *m_owner = nullptr;
    /// 一条导入流程正在走（从读清单到 `applyModpackImport` 返回）。
    ///
    /// NOTE: 这不是"在下载"（`m_downloadInFlight`），而是"还在问名字/换清单"那一小段：
    /// CurseForge 预览现在已经让出 UI 线程（嵌套事件循环），那期间「导入整合包…」
    /// 和拖放**仍然点得动**。没有这道闸的话第二趟会从问名字一路走到
    /// 「导入」那一档占用 = B，两趟同时往 versions 下面写。
    bool m_importFlow = false;
    /// PCL 那种"压缩包里套一份 .mrpack"解出来的临时包：实例名 → 临时文件。
    /// 导入真的开始了才登记（提前退出那些路当场就删），结束（`versionDownloadFinished`，
    /// 成功失败都算）时删掉 —— 数据层在整段导入里还要读它。
    QHash<QString, QString> m_tempArchives;
    /// 带自定义启动补丁的 MultiMC 包：实例名 → 原包路径。导入成功后把原包的
    /// `mmc-pack.json` / `patches/` / `instance.cfg` 留在实例的 `.wtfml/pack-source/`，
    /// 导出时才能把"特殊格式"原样带回去。
    QHash<QString, QString> m_specialPackSources;
    void preservePackSource(const QString &instanceId, const QString &zipPath);
    bool m_batchCancelled = false;
    QString m_lastFileCopyFailureReport;
};




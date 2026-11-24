/**
 * @file AgentWorkspaceStoreTests.cpp
 * @brief 测试启动工作区的登记、路径校验、状态与清理。
 */

#include "Support.hpp"

#include "agent/AgentWorkspaceStore.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

void runAgentWorkspaceStoreTests() {
    QTemporaryDir temp;
    check(temp.isValid(), "工作区：临时目录可建");
    if (!temp.isValid()) return;

    const QString records = temp.filePath(QStringLiteral("records"));
    const QString roots = temp.filePath(QStringLiteral("roots"));
    AgentWorkspaceStore store(records, roots);

    const QString id = AgentWorkspaceStore::newId();
    check(AgentWorkspaceStore::validId(id), "工作区：新 id 合法", id);
    check(!AgentWorkspaceStore::newVersionId(id).isEmpty() &&
              !AgentWorkspaceStore::newVersionId(id).contains(QLatin1Char('/')),
          "工作区：版本 id 是安全目录名");
    check(!AgentWorkspaceStore::validId(QStringLiteral("../ws")),
          "工作区：路径字符被拒");
    check(store.workspaceRoot(id).startsWith(roots),
          "工作区：物理根落在配置根下");

    AgentWorkspaceRecord record;
    record.id = id;
    record.sourceInstanceId = QStringLiteral("1.20.1-forge");
    record.versionId = AgentWorkspaceStore::newVersionId(id);
    record.rootPath = store.workspaceRoot(id);
    record.status = QStringLiteral("preparing");
    QString problem;
    check(store.create(record, &problem), "工作区：登记成功", problem);
    check(QDir(store.logsDir(id)).exists(), "工作区：日志目录已建");

    AgentWorkspaceRecord loaded;
    check(store.load(id, &loaded, &problem), "工作区：登记可读", problem);
    check(loaded.sourceInstanceId == record.sourceInstanceId, "工作区：来源实例保留");
    check(loaded.versionId == record.versionId, "工作区：版本 id 保留");

    QList<AgentWorkspaceRecord> list;
    check(store.list(&list, &problem) && list.size() == 1, "工作区：列表有一项");

    check(store.updateStatus(id, QStringLiteral("ready"), &problem), "工作区：状态可更新");
    check(store.load(id, &loaded, &problem) && loaded.status == QStringLiteral("ready"),
          "工作区：状态已落盘");

    AgentWorkspaceRecord bad = record;
    bad.id = AgentWorkspaceStore::newId();
    bad.rootPath = temp.filePath(QStringLiteral("outside"));
    check(!store.create(bad, &problem), "工作区：路径与 id 不匹配时拒绝");

    QFile marker(store.workspaceRoot(id) + QStringLiteral("/marker.txt"));
    check(marker.open(QIODevice::WriteOnly), "工作区：可写测试文件");
    marker.write("x");
    marker.close();
    check(store.cleanup(id, &problem), "工作区：清理成功", problem);
    check(!QDir(store.workspaceRoot(id)).exists(), "工作区：物理目录已删");
    check(!QFile::exists(store.recordPath(id)), "工作区：登记已删");
    check(!store.load(id, &loaded, &problem), "工作区：清理后读不到登记");
    check(!store.cleanup(id, &problem), "工作区：未知 id 不删任何东西");
}

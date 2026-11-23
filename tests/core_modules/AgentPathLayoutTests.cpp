/**
 * @file AgentPathLayoutTests.cpp
 * @brief `%LOCALAPPDATA%\WTFML` 这一层的布局：智能体数据在 `agent/`、缓存根在 `cache/`，
 *        以及旧版三层路径的一次性迁移。
 */

#include "Support.hpp"

#include "agent/AgentPaths.hpp"
#include "download/DownloadCache.hpp"
#include "util/AppPaths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

void writeFile(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(content);
    file.close();
}

QString readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(file.readAll());
}

int countOccurrences(const QString &text, const QString &needle) {
    int count = 0;
    int from = 0;
    while (true) {
        const int index = text.indexOf(needle, from, Qt::CaseSensitive);
        if (index < 0) break;
        ++count;
        from = index + needle.size();
    }
    return count;
}

}  // namespace

void runAgentPathLayoutTests() {
    QTemporaryDir fixture;
    check(fixture.isValid(), "路径布局：临时目录可用");

    const QString root = fixture.path() + QStringLiteral("/WTFML");
    AppPaths::setLocalDataRootOverride(root);
    const QString savedAgentOverride =
        QCoreApplication::instance()->property("wtfml.testAgentDataDir").toString();
    QCoreApplication::instance()->setProperty("wtfml.testAgentDataDir", QString());

    // ---------- 根目录只有一层 WTFML ----------
    const QString localRoot = AppPaths::localDataRoot();
    check(localRoot == QDir::cleanPath(root), "路径布局：本地数据根就是 <localappdata>/WTFML",
          localRoot);
    // 只数路径段（`/WTFML`），不数临时目录名里可能出现的 "wtfml-tests" 之类。
    check(localRoot.endsWith(QStringLiteral("/WTFML")), "路径布局：数据根以 /WTFML 结尾",
          localRoot);
    check(countOccurrences(localRoot, QStringLiteral("/WTFML")) == 1,
          "路径布局：数据根里只有一个 WTFML 段", localRoot);
    check(!localRoot.contains(QStringLiteral("/WTFML/WTFML")),
          "路径布局：没有连续两层 WTFML", localRoot);
    check(AppPaths::localCacheRoot() == localRoot + QStringLiteral("/cache"),
          "路径布局：缓存根是 <root>/cache", AppPaths::localCacheRoot());
    check(AgentPaths::dataDir() == localRoot + QStringLiteral("/agent"),
          "路径布局：智能体数据在 <root>/agent", AgentPaths::dataDir());
    check(DownloadCache::root() == localRoot + QStringLiteral("/cache/range-downloads"),
          "路径布局：断点续传缓存在 <root>/cache/range-downloads",
          DownloadCache::root());
    check(AgentPaths::sessionsDir() == localRoot + QStringLiteral("/agent/sessions"),
          "路径布局：会话目录在 agent/sessions", AgentPaths::sessionsDir());
    check(AgentPaths::maintenanceBackupDir() ==
              localRoot + QStringLiteral("/agent/backups/maintenance"),
          "路径布局：维护备份在 agent/backups/maintenance", AgentPaths::maintenanceBackupDir());
    check(AgentPaths::mcpConfigDir() == localRoot + QStringLiteral("/agent/mcp"),
          "路径布局：MCP 配置在 agent/mcp", AgentPaths::mcpConfigDir());

    QString problem;
    check(AgentPaths::ensureDataDir(&problem), "路径布局：ensureDataDir 建得出来", problem);
    check(QFileInfo(localRoot + QStringLiteral("/agent/sessions")).isDir(),
          "路径布局：sessions 目录已建好");
    check(!QFileInfo(localRoot + QStringLiteral("/WTFML")).exists(),
          "路径布局：新装时不出现第二层 WTFML");

    // ---------- 旧版三层路径的迁移 ----------
    const QString legacyAgent = root + QStringLiteral("/WTFML/WTFML/agent");
    const QString legacyCache = root + QStringLiteral("/WTFML/cache");
    writeFile(legacyAgent + QStringLiteral("/sessions/old.json"), QByteArrayLiteral("old session"));
    writeFile(legacyCache + QStringLiteral("/range-downloads/a/part-1.bin"), QByteArrayLiteral("x"));
    // 新位置先清掉（上面 ensureDataDir 建过空目录），模拟"升级后第一次启动"。
    QDir(localRoot + QStringLiteral("/agent")).removeRecursively();
    QDir(localRoot + QStringLiteral("/cache")).removeRecursively();

    check(AgentPaths::migrateLegacyDataDir(&problem), "路径布局：旧版数据会搬一次", problem);
    check(readFile(localRoot + QStringLiteral("/agent/sessions/old.json")) ==
              QStringLiteral("old session"),
          "路径布局：旧会话搬到新 agent 目录");
    check(QFileInfo::exists(localRoot + QStringLiteral("/cache/range-downloads/a/part-1.bin")),
          "路径布局：旧缓存搬到新 cache 目录");
    check(!QFileInfo(legacyAgent).exists() && !QFileInfo(legacyCache).exists(),
          "路径布局：旧位置搬空后不再留着");
    check(!QFileInfo(root + QStringLiteral("/WTFML")).exists(),
          "路径布局：迁移后连中间的 WTFML 层也清掉");
    check(!AgentPaths::migrateLegacyDataDir(&problem), "路径布局：没有旧数据时迁移什么都不做");

    // ---------- 新位置已经有东西时不动旧的 ----------
    writeFile(legacyAgent + QStringLiteral("/sessions/another.json"), QByteArrayLiteral("legacy"));
    writeFile(localRoot + QStringLiteral("/agent/sessions/keep.json"), QByteArrayLiteral("keep"));
    AgentPaths::migrateLegacyDataDir(&problem);
    check(readFile(localRoot + QStringLiteral("/agent/sessions/keep.json")) ==
              QStringLiteral("keep"),
          "路径布局：新位置已有数据时不被覆盖");
    check(QFileInfo::exists(legacyAgent + QStringLiteral("/sessions/another.json")),
          "路径布局：新位置已有数据时旧目录原样保留");

    // ---------- 测试隔离下不搬真实目录 ----------
    QCoreApplication::instance()->setProperty("wtfml.testAgentDataDir",
                                              fixture.path() + QStringLiteral("/isolated-agent"));
    check(!AgentPaths::migrateLegacyDataDir(&problem),
          "路径布局：测试隔离（testAgentDataDir）下不迁移");
    check(QFileInfo::exists(legacyAgent + QStringLiteral("/sessions/another.json")),
          "路径布局：隔离下旧目录没被动过");

    QCoreApplication::instance()->setProperty("wtfml.testAgentDataDir", savedAgentOverride);
    AppPaths::setLocalDataRootOverride(QString());
}

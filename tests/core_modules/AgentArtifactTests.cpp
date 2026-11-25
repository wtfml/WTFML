/**
 * @file AgentArtifactTests.cpp
 * @brief 模型可读/本地原始存档分离、重新脱敏、加密落盘、不可读不泄露存在性。
 */

#include "Support.hpp"

#include "agent/AgentArtifactStore.hpp"

#include <QFile>
#include <QTemporaryDir>

void runAgentArtifactTests() {
    QTemporaryDir fixture;
    AgentArtifactStore store(fixture.path());
    QString modelId;
    QString problem;
    const QString secretText = QStringLiteral("password=super-secret-value\n普通说明");
    check(store.putModelReadable(QStringLiteral("s1"), secretText, &modelId, &problem),
          "artifact：模型可读落盘", problem);
    check(!modelId.isEmpty(), "artifact：返回模型可读 id");
    QString content;
    check(store.readModel(QStringLiteral("s1"), modelId, 0, 4096, &content, &problem),
          "artifact：模型可读 id 可读", problem);
    check(!content.contains(QStringLiteral("super-secret-value")) &&
              content.contains(QStringLiteral("普通说明")),
          "artifact：读取时重新脱敏", content);
    check(store.readModel(QStringLiteral("s1"), modelId, 0, 4, &content, &problem) &&
              content.size() <= 4,
          "artifact：支持 offset/length");

    QString localId;
    check(store.putLocalRaw(QStringLiteral("s1"), secretText, &localId, &problem),
          "artifact：本地原始存档落盘", problem);
    check(!store.readModel(QStringLiteral("s1"), localId, 0, 4096, &content, &problem),
          "artifact：模型读不到本地原始存档 id");
    check(problem.contains(QStringLiteral("不存在或不可读")) &&
              !problem.contains(localId),
          "artifact：错误不泄露本地 id 是否存在", problem);
    check(store.readLocalRaw(QStringLiteral("s1"), localId, &content, &problem) &&
              content.contains(QStringLiteral("super-secret-value")),
          "artifact：本地用户可显式读原始存档");

    bool plaintextOnDisk = false;
    QDir dir(store.sessionDir(QStringLiteral("s1")));
    for (const QFileInfo &file : dir.entryInfoList(QDir::Files)) {
        QFile f(file.absoluteFilePath());
        if (f.open(QIODevice::ReadOnly) &&
            f.readAll().contains(QStringLiteral("super-secret-value").toUtf8())) {
            plaintextOnDisk = true;
        }
    }
    check(!plaintextOnDisk, "artifact：落盘密文不含明文");
    check(!store.readModel(QStringLiteral("s1"), QStringLiteral("missing"), 0, 10, &content,
                           &problem),
          "artifact：不存在的 id 拒绝");
    check(store.removeSession(QStringLiteral("s1")) && !QDir(store.sessionDir(QStringLiteral("s1"))).exists(),
          "artifact：删除会话清理目录");

    // ---------- 单会话配额（默认 64 MB，测试压到 1 KB） ----------
    {
        AgentArtifactStore quota(fixture.path() + QStringLiteral("/quota"));
        check(quota.sessionQuotaBytes() == AgentArtifactStore::defaultSessionQuotaBytes(),
              "artifact：默认配额是 64 MB",
              QString::number(quota.sessionQuotaBytes()));
        quota.setSessionQuotaBytes(1024);
        const QString text(300, QLatin1Char('x'));
        QString id;
        int written = 0;
        QString quotaProblem;
        for (int i = 0; i < 8; ++i) {
            if (!quota.putModelReadable(QStringLiteral("sq"), text, &id, &quotaProblem)) break;
            written += 1;
        }
        check(written >= 1 && written < 8, "artifact：写到配额就停",
              QStringLiteral("written=%1").arg(written));
        check(quotaProblem.contains(QStringLiteral("上限")), "artifact：拒绝原因说明是配额",
              quotaProblem);
        check(quota.sessionBytes(QStringLiteral("sq")) <= 1024,
              "artifact：落盘字节不超过配额",
              QString::number(quota.sessionBytes(QStringLiteral("sq"))));
        QString readBack;
        QString readProblem;
        check(!id.isEmpty() &&
                  quota.readModel(QStringLiteral("sq"), id, 0, 1000, &readBack, &readProblem),
              "artifact：超出配额时新内容不落盘");
        check(quota.sessionQuotaBytes() == 1024, "artifact：配额可以按测试改小");
        quota.setSessionQuotaBytes(0);
        check(quota.putModelReadable(QStringLiteral("sq"), text, &id, &quotaProblem),
              "artifact：配额 0 表示不限", quotaProblem);
        check(quota.removeSession(QStringLiteral("sq")), "artifact：配额测试目录可清理");
    }
}

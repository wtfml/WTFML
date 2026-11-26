/**
 * @file AgentToolTests.cpp
 * @brief 文件工具引擎：目录元数据、完整清单确认、读/写审批、拒绝不改盘、对象变化拒绝、
 *        artifact 本地原始存档不可读、schema/路径攻击。
 */

#include "Support.hpp"

#include "agent/AgentArtifactStore.hpp"
#include "agent/AgentBackupIndex.hpp"
#include "agent/AgentFileToolEngine.hpp"
#include "agent/AgentInstanceLock.hpp"
#include "agent/AgentTool.hpp"
#include "agent/AgentWorkspaceStore.hpp"
#include "manage/BackupFiles.hpp"
#include "util/AppPaths.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace {
Agent::ToolCall callFor(const QString &name, const QJsonObject &arguments) {
    Agent::ToolCall call;
    call.id = QStringLiteral("call-1");
    call.name = name;
    call.arguments = arguments;
    return call;
}
}  // namespace

void runAgentToolTests() {
    QTemporaryDir fixture;
    const QString root = fixture.path() + QStringLiteral("/instance");
    QDir().mkpath(root + QStringLiteral("/config"));
    QFile config(root + QStringLiteral("/config/a.toml"));
    check(config.open(QIODevice::WriteOnly), "工具：造一个配置文件", config.fileName());
    config.write("maxMemory = 4096\n");
    config.close();
    QFile other(root + QStringLiteral("/notes.txt"));
    check(other.open(QIODevice::WriteOnly), "工具：造一个带口令的便签", other.fileName());
    other.write("password=super-secret-value\n");
    other.close();

    // 写前备份落在当前游戏文件夹的 cache/backups/files：指到临时目录，收尾还原。
    const QString originalGameDir = AppPaths::minecraftDir();
    AppPaths::setMinecraftDir(fixture.path());
    struct GameDirGuard {
        QString dir;
        ~GameDirGuard() { AppPaths::setMinecraftDir(dir); }
    } gameDirGuard{originalGameDir};
    AgentArtifactStore artifacts(fixture.path() + QStringLiteral("/artifacts"));
    AgentFileTools files(true);
    const AgentToolContext context{QStringLiteral("s1"), QStringLiteral("inst-1"),
                                   QStringLiteral("实例 1"),
                                   AgentInstanceLocks::gameRootId(fixture.path()), root,
                                   QStringLiteral("api.example.com"), false};
    AgentFileToolEngine engine(&files, &artifacts, [context](QString *) { return context; });

    check(AgentToolRegistry::builtins().size() >= 5, "工具：Stage 1B 注册了文件工具");
    check(AgentToolRegistry::wireName(QStringLiteral("fs.read")) == QStringLiteral("fs_read") &&
              AgentToolRegistry::internalName(QStringLiteral("fs_read")) ==
                  QStringLiteral("fs.read"),
          "工具：内部点号名映射成 OpenAI 允许的线上名");
    for (const AgentToolSpec &spec : AgentToolRegistry::builtins()) {
        const QString wire = AgentToolRegistry::wireName(spec.name);
        check(!wire.contains(QLatin1Char('.')) &&
                  QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]+$")).match(wire).hasMatch(),
              QStringLiteral("工具：线上 function name 合规 %1").arg(wire));
    }
    const AgentToolResult wireRead = engine.invoke(callFor(
        QStringLiteral("fs_read"),
        QJsonObject{{QStringLiteral("path"), QStringLiteral("config/a.toml")}}));
    check(wireRead.status == AgentToolResult::Status::ApprovalRequired,
          "工具：模型用线上名 fs_read 也能路由到 fs.read");
    engine.cancelPending();
    QString problem;
    const AgentToolSpec writeSpec = AgentToolRegistry::find(QStringLiteral("fs.write"));
    check(!writeSpec.name.isEmpty(), "工具：fs.write schema 存在");
    check(!AgentToolRegistry::validateArguments(
              writeSpec.schema,
              QJsonObject{{QStringLiteral("path"), QStringLiteral("config/a.toml")},
                          {QStringLiteral("content"), QStringLiteral("x")},
                          {QStringLiteral("unknown"), 1}},
              &problem),
          "工具：schema 拒绝未知字段");
    check(!AgentToolRegistry::validateArguments(
              writeSpec.schema,
              QJsonObject{{QStringLiteral("path"), QStringLiteral("config/a.toml")}},
              &problem),
          "工具：schema 拒绝缺少必填字段");
    check(engine.invoke(callFor(QStringLiteral("fs.nope"), {})).ok() == false,
          "工具：未知工具报错");

    const AgentToolResult list =
        engine.invoke(callFor(QStringLiteral("fs.list"),
                              QJsonObject{{QStringLiteral("path"), QStringLiteral(".")}}));
    check(list.status == AgentToolResult::Status::Ok, "工具：fs.list 自动执行", list.summary);
    const QByteArray listJson = QJsonDocument(list.data).toJson();
    check(!listJson.contains("a.toml") && !listJson.contains("notes.txt"),
          "工具：fs.list 不返回文件名", QString::fromUtf8(listJson));
    check(list.data.value(QStringLiteral("files")).toInt() >= 0, "工具：fs.list 返回数量");

    const AgentToolResult names = engine.invoke(callFor(
        QStringLiteral("fs.listNames"), QJsonObject{{QStringLiteral("path"), QStringLiteral(".")}}));
    check(names.status == AgentToolResult::Status::ApprovalRequired,
          "工具：fs.listNames 需要审批");
    const QString namesId = names.approval.value(QStringLiteral("approvalId")).toString();
    const AgentToolResult namesDenied = engine.resolveApproval(namesId, false);
    check(namesDenied.status == AgentToolResult::Status::Error &&
              namesDenied.error.kind == Agent::ErrorKind::ApprovalDenied,
          "工具：拒绝完整清单不发送");
    const AgentToolResult namesAgain = engine.invoke(callFor(
        QStringLiteral("fs.listNames"), QJsonObject{{QStringLiteral("path"), QStringLiteral(".")}}));
    const AgentToolResult namesAllowed = engine.resolveApproval(
        namesAgain.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(namesAllowed.ok() &&
              namesAllowed.data.value(QStringLiteral("names")).toString().contains("notes.txt"),
          "工具：允许后返回脱敏文件名清单");

    const AgentToolResult readConfig = engine.invoke(callFor(
        QStringLiteral("fs.read"), QJsonObject{{QStringLiteral("path"), QStringLiteral("config/a.toml")}}));
    check(readConfig.status == AgentToolResult::Status::ApprovalRequired,
          "工具：配置文件读取需要逐次确认");
    const AgentToolResult readConfigAllowed = engine.resolveApproval(
        readConfig.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(readConfigAllowed.ok() &&
              readConfigAllowed.data.value(QStringLiteral("content")).toString().contains("4096"),
          "工具：确认后读取配置");
    check(!readConfigAllowed.data.value(QStringLiteral("fileIdentityToken")).toString().isEmpty(),
          "工具：读结果带身份 token");

    const AgentToolResult readSecret = engine.invoke(callFor(
        QStringLiteral("fs.read"), QJsonObject{{QStringLiteral("path"), QStringLiteral("notes.txt")}}));
    check(readSecret.status == AgentToolResult::Status::ApprovalRequired,
          "工具：其他文本需要确认");
    const AgentToolResult readSecretAllowed = engine.resolveApproval(
        readSecret.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(readSecretAllowed.ok() &&
              !readSecretAllowed.data.value(QStringLiteral("content")).toString().contains(
                  QStringLiteral("super-secret-value")),
          "工具：确认后也只发送脱敏片段");

    QFile big(root + QStringLiteral("/big.txt"));
    check(big.open(QIODevice::WriteOnly), "工具：造一个大文本", big.fileName());
    big.write(QByteArray(20000, 'a'));
    big.close();
    const AgentToolResult bigRead = engine.invoke(callFor(
        QStringLiteral("fs.read"), QJsonObject{{QStringLiteral("path"), QStringLiteral("big.txt")}}));
    const AgentToolResult bigAllowed = engine.resolveApproval(
        bigRead.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(bigAllowed.ok() &&
              !bigAllowed.data.value(QStringLiteral("modelArtifact")).toString().isEmpty(),
          "工具：大文本转成模型可读 artifact，不整段内联");
    check(bigAllowed.data.value(QStringLiteral("content")).toString().toUtf8().size() <= 16 * 1024 + 100,
          "工具：大文本内联部分被截断");
    const QString bigHint = bigAllowed.data.value(QStringLiteral("content")).toString();
    check(bigHint.contains(QStringLiteral("model-readable artifact")) &&
              bigHint.contains(QStringLiteral("redacted")) &&
              !bigHint.contains(QStringLiteral("full content")),
          "工具：artifact 提示说明是本次读取内容、且同样已脱敏");

    QFile credential(root + QStringLiteral("/.env"));
    check(credential.open(QIODevice::WriteOnly), "工具：造一个凭据文件", credential.fileName());
    credential.write("API_KEY=abcdefghijklmnop");
    credential.close();
    const AgentToolResult readCredential = engine.invoke(callFor(
        QStringLiteral("fs.read"), QJsonObject{{QStringLiteral("path"), QStringLiteral(".env")}}));
    check(readCredential.status == AgentToolResult::Status::Blocked,
          "工具：凭据文件硬阻断");

    const AgentToolResult traversal = engine.invoke(callFor(
        QStringLiteral("fs.list"), QJsonObject{{QStringLiteral("path"), QStringLiteral("../")}}));
    check(!traversal.ok() && traversal.error.kind == Agent::ErrorKind::ToolError,
          "工具：路径穿越被拒绝");

    // 写：先读拿到 token/hash，再请求审批；拒绝时一个字节不变。
    const AgentFileReadResult before = files.read(root, QStringLiteral("config/a.toml"), 4096);
    Agent::ToolCall write = callFor(
        QStringLiteral("fs.write"),
        QJsonObject{{QStringLiteral("path"), QStringLiteral("config/a.toml")},
                    {QStringLiteral("content"), QStringLiteral("maxMemory = 8192\n")},
                    {QStringLiteral("expectedSha256"), before.sha256},
                    {QStringLiteral("fileIdentityToken"), before.identityToken}});
    const AgentToolResult writeApproval = engine.invoke(write);
    check(writeApproval.status == AgentToolResult::Status::ApprovalRequired,
          "工具：写操作需要审批");
    check(writeApproval.approval.value(QStringLiteral("diff")).toString().contains("8192"),
          "工具：写审批带 diff");
    check(engine.resolveApproval(
              writeApproval.approval.value(QStringLiteral("approvalId")).toString(), false)
              .status == AgentToolResult::Status::Error,
          "工具：拒绝写审批");
    QFile unchanged(root + QStringLiteral("/config/a.toml"));
    check(unchanged.open(QIODevice::ReadOnly), "工具：读回被拒写的那一份", unchanged.fileName());
    check(unchanged.readAll() == QByteArray("maxMemory = 4096\n"),
          "工具：拒绝后文件一个字节不变");
    unchanged.close();

    const AgentToolResult writeAllowed = engine.invoke(write);
    const AgentToolResult written = engine.resolveApproval(
        writeAllowed.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(written.ok(), "工具：允许后写入成功", written.summary);
    QFile after(root + QStringLiteral("/config/a.toml"));
    check(after.open(QIODevice::ReadOnly), "工具：读回写入后的文件", after.fileName());
    check(after.readAll() == QByteArray("maxMemory = 8192\n"), "工具：写入内容正确");
    after.close();
    check(!written.data.value(QStringLiteral("backupId")).toString().isEmpty(),
          "工具：写入带写前备份 id");
    check(written.data.value(QStringLiteral("backupDir")).toString().startsWith(
              BackupFiles::backupDir(BackupFiles::Kind::Files)),
          "工具：备份落在启动器备份目录（cache/backups/files）");
    {
        const QList<AgentBackups::Item> listed = AgentBackups::list(QStringLiteral("inst-1"), nullptr);
        QList<BackupFiles::FilePayload> payload;
        check(listed.size() == 1 && listed.constFirst().trigger == AgentBackups::kTriggerWrite &&
                  listed.constFirst().sessionId == QStringLiteral("s1") &&
                  AgentBackups::readFiles(listed.constFirst(), nullptr, &payload) &&
                  payload.size() == 1 &&
                  payload.constFirst().path == QStringLiteral("config/a.toml") &&
                  payload.constFirst().data == QByteArray("maxMemory = 4096\n"),
              "工具：写前备份可列出，内容是写入前的原文");
    }

    // 写进修复工作区：不备份（那是副本，备份记原实例 id 会让「恢复」写回原实例）。
    {
        AgentWorkspaceStore workspaces;
        AgentWorkspaceRecord workspace;
        workspace.id = AgentWorkspaceStore::newId();
        workspace.sourceInstanceId = context.instanceId;
        workspace.versionId = AgentWorkspaceStore::newVersionId(workspace.id);
        workspace.rootPath = workspaces.workspaceRoot(workspace.id);
        workspace.status = QStringLiteral("ready");
        const QString copyRoot =
            workspace.rootPath + QStringLiteral("/versions/") + workspace.versionId;
        QDir().mkpath(copyRoot + QStringLiteral("/config"));
        QFile copy(copyRoot + QStringLiteral("/config/a.toml"));
        check(copy.open(QIODevice::WriteOnly), "工具：造工作区里的配置", copy.fileName());
        copy.write("maxMemory = 4096\n");
        copy.close();
        QString problem;
        check(workspaces.create(workspace, &problem), "工具：建测试工作区", problem);
        const AgentFileReadResult inCopy = files.read(copyRoot, QStringLiteral("config/a.toml"), 4096);
        const AgentToolResult askCopy = engine.invoke(callFor(
            QStringLiteral("fs.write"),
            QJsonObject{{QStringLiteral("path"), QStringLiteral("config/a.toml")},
                        {QStringLiteral("content"), QStringLiteral("maxMemory = 2048\n")},
                        {QStringLiteral("expectedSha256"), inCopy.sha256},
                        {QStringLiteral("fileIdentityToken"), inCopy.identityToken},
                        {QStringLiteral("workspaceId"), workspace.id}}));
        const AgentToolResult wroteCopy = engine.resolveApproval(
            askCopy.approval.value(QStringLiteral("approvalId")).toString(), true);
        check(wroteCopy.ok() && wroteCopy.data.value(QStringLiteral("backupId")).toString().isEmpty() &&
                  AgentBackups::list(QStringLiteral("inst-1"), nullptr).size() == 1,
              "工具：写工作区成功且不留备份", wroteCopy.summary);
        QDir(workspace.rootPath).removeRecursively();
    }

    // prepare 之后对象被替换：提交拒绝，不假装成功。
    const AgentFileReadResult snapshot = files.read(root, QStringLiteral("config/a.toml"), 4096);
    const AgentToolResult pending = engine.invoke(callFor(
        QStringLiteral("fs.write"),
        QJsonObject{{QStringLiteral("path"), QStringLiteral("config/a.toml")},
                    {QStringLiteral("content"), QStringLiteral("maxMemory = 16384\n")},
                    {QStringLiteral("expectedSha256"), snapshot.sha256},
                    {QStringLiteral("fileIdentityToken"), snapshot.identityToken}}));
    QFile replaced(root + QStringLiteral("/config/a.toml"));
    check(replaced.open(QIODevice::WriteOnly | QIODevice::Truncate), "工具：外部替换掉文件",
          replaced.fileName());
    replaced.write("maxMemory = 2048\n");
    replaced.close();
    const AgentToolResult rejected = engine.resolveApproval(
        pending.approval.value(QStringLiteral("approvalId")).toString(), true);
    check(!rejected.ok() &&
              (rejected.error.message.contains(QStringLiteral("替换")) ||
               rejected.error.message.contains(QStringLiteral("变化"))),
          "工具：提交前对象变化拒绝写入", rejected.error.message);
    QFile still(root + QStringLiteral("/config/a.toml"));
    check(still.open(QIODevice::ReadOnly), "工具：读回外部写入的那一份", still.fileName());
    check(still.readAll() == QByteArray("maxMemory = 2048\n"),
          "工具：被拒后保留外部写入的内容");

    const AgentToolResult protectedWrite = engine.invoke(callFor(
        QStringLiteral("fs.write"),
        QJsonObject{{QStringLiteral("path"), QStringLiteral("versions/x/a.json")},
                    {QStringLiteral("content"), QStringLiteral("{}")}}));
    check(protectedWrite.status == AgentToolResult::Status::Blocked ||
              protectedWrite.status == AgentToolResult::Status::Error,
          "工具：保护目录写入拒绝");

    // artifact：本地原始存档 id 不能被模型读。
    QString localId;
    artifacts.putLocalRaw(QStringLiteral("s1"), QStringLiteral("secret"), &localId, nullptr);
    const AgentToolResult artifactRead = engine.invoke(callFor(
        QStringLiteral("artifact.read"), QJsonObject{{QStringLiteral("artifactId"), localId}}));
    check(artifactRead.status == AgentToolResult::Status::Blocked ||
              artifactRead.status == AgentToolResult::Status::Error,
          "工具：artifact.read 读不到本地原始存档");
    QString modelId;
    artifacts.putModelReadable(QStringLiteral("s1"), QStringLiteral("password=abc123456\nok"),
                               &modelId, nullptr);
    const AgentToolResult modelRead = engine.invoke(callFor(
        QStringLiteral("artifact.read"), QJsonObject{{QStringLiteral("artifactId"), modelId}}));
    check(modelRead.ok() &&
              !modelRead.data.value(QStringLiteral("content")).toString().contains(
                  QStringLiteral("abc123456")),
          "工具：artifact.read 每次重新脱敏");
}

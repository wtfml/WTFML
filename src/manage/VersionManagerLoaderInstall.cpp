/**
 * @file VersionManagerLoaderInstall.cpp
 * @brief `VersionManager` 的加载器那一摊：四种加载器各自的安装路、版本表询问、安装器进程，
 *        以及一次安装的收尾（`finishLoaderInstall()` / `failLoaderInstall()`）。
 *
 * 【为什么是这一摊】
 *
 * 这四家（Fabric / Forge / NeoForge / OptiFine）看着像四条路，其实共用同一台状态机
 * （`LoaderInstall`）：每一步的接续点都在别人的信号槽里，一处的相位改错，表现是"安装卡在
 * 半路"或者"重复装一遍"。把它们摆在一个文件里，改一条路的时候另外三条就摆在旁边，
 * 不用在几千行里翻。
 *
 * 【这份文件里的东西】
 *
 * * 加载器的**枚举与"装得了吗"**（`loaderKindCount()` 那一组）；
 * * 安装页那个可展开的**版本表询问**（只问不装）；
 * * Fabric：拉版本表 / 拉 profile / 落 json / 下它自己的库；
 * * Forge / NeoForge：拉安装器版本表、下安装器、探 `--help`、跑它；
 * * 老 Forge：照它自己那份安装说明装（纯逻辑在 `LegacyForgeInstall` 那一层）；
 * * OptiFine：换 APPDATA 对着临时目录装、装完把产物搬回来；
 * * 收尾：写 `wtfml.meta` 的加载器两栏、发终态信号。
 *
 * 【和另外三个 .cpp 的关系】
 *
 * 只通过 `LoaderInstall` 那份状态和 `DownloadManager` 的信号互相接。凡是"这一批下完了
 * 接着干什么"这类判断留在核心那个文件里（见 `onAllDownloadsFinished()`）。
 */

#include "net/ControlGet.hpp"
#include <QCoreApplication>
#include "launcher/ProcessPathLimit.hpp"
#include "VersionManager.hpp"
#include "VersionManagerInternals.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "../download/DownloadRetryPolicy.hpp"
#include "../download/InstallerFailure.hpp"
#include "../download/LegacyForgeInstall.hpp"
#include "../download/LoaderMeta.hpp"
#include "../download/ResourceCatalog.hpp"
#include "../launcher/VersionInheritance.hpp"
#include "../net/HttpClient.hpp"
#include "../settings/GlobalJavaSettings.hpp"
#include "../util/AppPaths.hpp"
#include "../util/StorageAvailability.hpp"
#include "../util/WriteDirectoryGuard.hpp"
#include "../util/ZipUtil.hpp"
#include "FolderManager.hpp"
#include "InstanceFiles.hpp"

using namespace VersionManagerInternals;

namespace {

/// 回复里那句原因；没有回复（排队到点 / 被掐 / 管理器先没）就用作业给的那句。
QString replyReason(QNetworkReply *reply, const QString &jobError) {
    return reply != nullptr ? reply->errorString() : jobError;
}

/// 这一发算不算"换个源再试试"：没回复（含排队到点）算，传输出错按既有判据，
/// 在飞时被作业按截止掐掉也算 —— 和版本表那条路（`onLoaderVersionsReply`）同一个口径。
bool transientJobFailure(QNetworkReply *reply) {
    return reply == nullptr || DownloadRetryPolicy::transientReply(reply) ||
           reply->property(RequestBudget::kDeadlineExpiredProperty).toBool();
}

}  // namespace

// ---------------------------------------------------------------------------
// 带模组加载器的版本（今天只有 Fabric 接通了）
// ---------------------------------------------------------------------------

bool VersionManager::loaderInstallSupported(LoaderKind kind) {
    // 四个都接通了，所以四个都返回 true（这个函数只说"能不能装"，不说"怎么装"）。
    //
    // 刻意写成**逐个 case 列出来**、末尾 `return false`，而不是一句 `return true`：将来往
    // LoaderKind 里加第五个加载器时，编译器的 -Wswitch 会提醒这里少了一手，而没写完的那个
    // 会落到 false —— 界面那句「还没做」和 installLoaderVersion() 里那道守卫正好接住它，
    // 不会出现"勾了一个没接线的加载器，装出来一个纯原版"。
    switch (kind) {
    case LoaderKind::Fabric:
    case LoaderKind::Forge:
    case LoaderKind::NeoForge:
    case LoaderKind::OptiFine:
    case LoaderKind::Quilt:
        return true;
    }
    return false;
}

QString VersionManager::loaderDisplayName(LoaderKind kind) {
    switch (kind) {
    case LoaderKind::Fabric:
        return QStringLiteral("Fabric");
    case LoaderKind::Forge:
        return QStringLiteral("Forge");
    case LoaderKind::NeoForge:
        return QStringLiteral("NeoForge");
    case LoaderKind::OptiFine:
        return QStringLiteral("OptiFine");
    case LoaderKind::Quilt:
        return QStringLiteral("Quilt");
    }
    return QString();
}

int VersionManager::loaderKindCount() { return 5; }

bool VersionManager::loaderKindFromIndex(int index, LoaderKind *out) {
    // 顺序**就是** MainWindowUI::modLoaderOptionNames() 的顺序（那张表里下标 0..4 是五个
    // 加载器）。界面上摆控件的顺序和这里翻译的顺序对不上时不会报错，只会"勾了 Fabric
    // 装出 Forge" —— 所以这张表只有这一处翻译，而且 verify 里按五个名字逐项对过
    // （名字取的是界面那两张表，不是这里抄的第二份字面量）。
    static const LoaderKind kOrder[] = {LoaderKind::Fabric, LoaderKind::Forge,
                                        LoaderKind::NeoForge, LoaderKind::OptiFine,
                                        LoaderKind::Quilt};
    constexpr int kCount = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));
    static_assert(kCount == 5, "加载器多了一个/少了一个，界面那两张表也要跟着改");  // i18n-ignore
    if (index < 0 || index >= kCount) {
        return false;
    }
    if (out != nullptr) {
        *out = kOrder[index];
    }
    return true;
}

int VersionManager::loaderIndexForKind(LoaderKind kind) {
    // 反着查同一张表（loaderKindFromIndex 是正着查）—— 反过来各写一张的话，"那张表
    // 改了、这张没改"会表现成"勾了 Fabric，拉回来的列表填进 Forge 那一条"，不报错。
    for (int index = 0; index < loaderKindCount(); ++index) {
        LoaderKind candidate = LoaderKind::Fabric;
        if (loaderKindFromIndex(index, &candidate) && candidate == kind) {
            return index;
        }
    }
    return -1;  // 枚举里将来多一个、那张表还没跟上时走这里
}

bool VersionManager::isLoaderInstalling() const { return m_loaderInstall.active(); }

// ---------------------------------------------------------------------------
// "这个加载器有哪些版本可选"：一次只读的询问（安装页那个可展开的列表用）
// ---------------------------------------------------------------------------
//
// 和上面那条安装路的关系：**这条只是问，什么都不装**。用户在安装页上把四个加载器挨个点开
// 看看，一次安装都还没发生 —— 所以它刻意不复用 m_loaderInstall 那套状态机（用的话点开一个
// 加载器就会把相位弄成"正在装"，isLoaderInstalling() 当场变 true，底栏和按钮全跟着错）。

void VersionManager::requestLoaderVersions(const QString &gameVersion, LoaderKind kind) {
    const QString wanted = gameVersion.trimmed();

    // **同一个加载器**上上一发还在飞就先掐掉（换了游戏版本、或者同一局重新问一遍）——
    // 换局之后旧那一发回来会把上一局的版本号填进这一局的列表里。abort 之后 finished()
    // 照样会来一发，回复处理函数靠"这一个指针还是我手上这个吗"把它丢掉。
    // 别的加载器在飞的那几发**不动**：四个各占一格，四条一起问是正常的（见头里那段）。
    LoaderVersionRequest &slot = m_loaderVersionRequests[int(kind)];
    ++slot.generation;
    delete slot.queued.data();
    slot.queued = nullptr;
    if (slot.reply != nullptr) {
        // **先摘指针，再 abort()** —— 顺序不能换。abort() 有可能**当场**把 finished()
        // 发出来（回复已经在手上、只是还没被处理时就是这一种）。那一发进来时这一格里
        // 若还是旧指针，onLoaderVersionsReply() 的 "还是我手上这一发吗"当场判成"是" ⇒
        // 我们自己掐掉的那一发被当成一次真回复，
        // 给用户弹一句「拿不到 X 的版本列表（Operation canceled）」。用户什么都没做错。
        QNetworkReply *stale = slot.reply;
        slot.reply = nullptr;
        stale->abort();
    }
    slot.gameVersion = wanted;

    if (wanted.isEmpty()) {
        emit loaderVersionsFetched(wanted, kind, false, LoaderMeta::LoaderVersionList(),
                                   tr("请选择要安装的版本。"));
        return;
    }
    if (!loaderInstallSupported(kind)) {
        emit loaderVersionsFetched(wanted, kind, false, LoaderMeta::LoaderVersionList(),
                                   tr("暂不支持安装 %1。").arg(loaderDisplayName(kind)));
        return;
    }

    // OptiFine 的列表**必须跟着下载源**走：列出来的版本要能用同一个源装下去。镜像那条路的
    // 地址最后两段是 type/patch（见 LoaderMeta::optifineMirrorPathFromEdition），官方那条路
    // 得先过广告页拿 token —— 两份列表混着用就会"列出来了但下不了"。
    //
    // 另外三个没这个问题（Fabric 只有官方一个源；两份 maven 清单官方就是发布处），一律走官方。
    // NOTE: 两份 maven 清单这里**不换镜像**，和安装那条路（fetchInstallerList，镜像挑不出来
    // 再退官方）不一样。那条教训是实测的：镜像上那份 Forge 版本表是旧文件（1782 B、
    // 最新只到 1.15.2），照着它列会给用户一个"1.16 以后都没有 Forge"的假结论。
    // 列表这件事对"全不全"比对"快不快"敏感得多，所以宁可直接问官方。
    QString url;
    // "这一发走没走镜像"**只在这里读一次**，然后一路带到解析那一步去（同一次读的结果）：
    // 解析那边若再读一遍 downloadManager->useMirror()，两次之间用户点一下开关就会出现
    // "正文是镜像那份、却按官方那套规矩解析"。见 LoaderVersionRequest 里那段 NOTE。
    bool fromMirror = false;
    switch (kind) {
    case LoaderKind::Fabric:
        url = LoaderMeta::fabricLoaderListUrl(wanted);
        break;
    case LoaderKind::Quilt:
        url = LoaderMeta::quiltLoaderListUrl(wanted);
        break;
    case LoaderKind::Forge:
        url = LoaderMeta::forgeMavenMetadataUrl();
        break;
    case LoaderKind::NeoForge:
        url = LoaderMeta::neoforgeMavenMetadataUrl();
        break;
    case LoaderKind::OptiFine:
        fromMirror = downloadManager->useMirror();
        url = fromMirror ? LoaderMeta::optifineMirrorListUrl(wanted)
                         : LoaderMeta::optifineDownloadsPageUrl();
        break;
    }
    // 这两格跟着**这一局**重来：上一局退到镜像没有、官方地址是哪一条。
    slot.officialUrl = url;
    slot.mirrorFallbackUsed = false;
    fetchLoaderVersions(kind, url, fromMirror);
}

QString VersionManager::loaderVersionsMirrorFallback(LoaderKind kind,
                                                     const QString &officialUrl) const {
    // 为什么只有 NeoForge 能这么退：
    // * Forge 那个同名 maven-metadata.xml 在镜像上是**旧的**（ 实测 latest 停在
    // `1.18-38.0.17`、64,569 B；官方 213,257 B），它的 promotions 表更早（停在 1.15.2）。
    // 照镜像那份列版本会得出「26.3 没有 Forge」的假结论 —— 这正是 requestLoaderVersions
    // 里那段"两份 maven 清单不换镜像"注释防的那件事。
    // * Fabric 的 host 镜像不认（DownloadManager::mirrorUrl() 原样返回）。
    // * OptiFine 两条路的正文格式不一样（镜像 JSON / 官方网页），换源要连带换解析规矩，
    // 不在这里掺和。
    if (kind != LoaderKind::NeoForge || officialUrl.trimmed().isEmpty()) {
        return QString();
    }
    const QString mirrored = DownloadManager::mirrorUrl(officialUrl);
    return mirrored == officialUrl ? QString() : mirrored;
}

QNetworkAccessManager *VersionManager::loaderNetworkManager() {
    if (loaderNetwork == nullptr) {
        loaderNetwork = new QNetworkAccessManager(this);
    }
    return loaderNetwork;
}

void VersionManager::fetchLoaderVersions(LoaderKind kind, const QString &url, bool fromMirror) {
    const QUrl target(url);
    QNetworkRequest request(target);
    request.setTransferTimeout(30000);
    auto &slot = m_loaderVersionRequests[int(kind)];
    slot.fromMirror = fromMirror;
    const auto generation = slot.generation;
    slot.queued = RequestBudget::ControlGet::start(this, loaderNetworkManager(), request, 30000,
        [this, kind, generation](QNetworkReply *reply, const QString &error) {
            auto &current = m_loaderVersionRequests[int(kind)];
            if (current.generation != generation) return;
            current.queued = nullptr;
            if (!reply) {
                emit loaderVersionsFetched(current.gameVersion, kind, false, {}, error);
                return;
            }
            current.reply = reply;
            onLoaderVersionsReply(kind, reply);
        });
}

void VersionManager::onLoaderVersionsReply(LoaderKind kind, QNetworkReply *reply) {
    reply->deleteLater();
    // 这一发还是我手上这一发吗？不是就说明调用方已经换了目标（abort 掉的那一发也会走到
    // 这里），这份回复没人要了。
    QHash<int, LoaderVersionRequest>::iterator slot = m_loaderVersionRequests.find(int(kind));
    if (slot == m_loaderVersionRequests.end() || slot->reply != reply) {
        return;
    }
    // 先把字段抄下来再清空那一格：下面几条 emit 都要用它们，清空之后就没得读了。
    const QString gameVersion = slot->gameVersion;
    // 走没走镜像读的是**这一发问出去时**记下的那一个（不是现在这个开关值）。
    const bool fromMirror = slot->fromMirror;
    slot->reply = nullptr;

    // 先判设备开没开再读：超时回来的那条已经不 open，readAll() 会往 stderr 甩 warning。
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();

    // Fabric 对不支持的游戏版本回 HTTP 400 + `[]`，Quilt 回 HTTP 404 + `{"code":"not_found"}`：
    // 二者都是"该版本确实没有可用加载器"，按空列表（success=true）交回，而不是报网络错误。
    if ((kind == LoaderKind::Fabric || kind == LoaderKind::Quilt) &&
        LoaderMeta::isUnsupportedGameVersionMetaReply(kind == LoaderKind::Quilt, httpStatus, body)) {
        emit loaderVersionsFetched(gameVersion, kind, true, LoaderMeta::LoaderVersionList(),
                                   QString());
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        // 官方那条**连不上**时往镜像退一次（只 NeoForge，理由见
        // loaderVersionsMirrorFallback()）。判"连不上"用的是下载那条路同一个
        // transientReply()：超时 / 连接被掐 / HTTP/2 断流都算，404、证书不对、
        // 用户取消不算 —— 那些换镜像也帮不上忙。只退一次，镜像再失败就照实报。
        if (!slot->mirrorFallbackUsed && (DownloadRetryPolicy::transientReply(reply) ||
            reply->property(RequestBudget::kDeadlineExpiredProperty).toBool())) {
            const QString fallback = loaderVersionsMirrorFallback(kind, slot->officialUrl);
            if (!fallback.isEmpty()) {
                slot->mirrorFallbackUsed = true;
                qInfo() << "Loader version list from" << slot->officialUrl << "failed ("
                        << reply->errorString() << "); retrying through the mirror:" << fallback;
                fetchLoaderVersions(kind, fallback, /*fromMirror=*/true);
                return;
            }
        }
        emit loaderVersionsFetched(
            gameVersion, kind, false, LoaderMeta::LoaderVersionList(),
            tr("拿不到 %1 %2 的版本列表（%3）。检查网络后重试。")
                .arg(loaderDisplayName(kind), gameVersion,
                     reply->property(RequestBudget::kDeadlineExpiredProperty).toBool() ? tr("请求超时") : reply->errorString()));
        return;
    }

    const LoaderVersionParse parsed = parseLoaderVersions(kind, gameVersion, body, fromMirror);
    if (!parsed.error.isEmpty()) {
        emit loaderVersionsFetched(gameVersion, kind, false, LoaderMeta::LoaderVersionList(),
                                   parsed.error);
        return;
    }
    emit loaderVersionsFetched(gameVersion, kind, true, parsed.versions, QString());
}

VersionManager::LoaderVersionParse VersionManager::parseLoaderVersions(
    LoaderKind kind, const QString &gameVersion, const QByteArray &body, bool fromMirror) const {
    LoaderVersionParse result;
    LoaderMeta::LoaderVersionList entries;
    switch (kind) {
    case LoaderKind::Fabric: {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (!doc.isArray()) {
            result.error = tr("Fabric 的版本列表无法解析（%1）。").arg(parseError.errorString());
            return result;
        }
        entries = LoaderMeta::listFabricLoaderVersions(doc.array());
        break;
    }
    case LoaderKind::Quilt: {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (!doc.isArray()) {
            result.error = tr("Quilt 的版本列表无法解析（%1）。").arg(parseError.errorString());
            return result;
        }
        entries = LoaderMeta::listQuiltLoaderVersions(doc.array());
        break;
    }
    case LoaderKind::Forge:
        entries = LoaderMeta::listForgeVersions(body, gameVersion);
        break;
    case LoaderKind::NeoForge:
        entries = LoaderMeta::listNeoForgeVersions(body, gameVersion);
        break;
    case LoaderKind::OptiFine:
        // 判据是**这一发问出去时**记下的那个（参数），不是现在这个开关值 —— 见 hpp 里那段
        // NOTE：两次读之间用户点一下开关，正文和解析规矩就会错配。
        if (fromMirror) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
            if (!doc.isArray()) {
                result.error =
                    tr("OptiFine 的版本列表无法解析（%1）。").arg(parseError.errorString());
                return result;
            }
            entries = LoaderMeta::listOptifineVersionsFromMirror(doc.array(), gameVersion);
        } else {
            entries = LoaderMeta::listOptifineVersionsFromDownloadsPage(QString::fromUtf8(body),
                                                                       gameVersion);
        }
        break;
    }

    // 直接送那份带 `stable` 标记的列表：界面要用它标「测试版」、也要用它决定
    // 「下载稳定版（release）」那一行能不能点（见 InstallPageController::fillLoaderVersionItems）。
    result.versions = entries;
    // NOTE: "这个游戏版本一个能装的都没有"**不算读失败**（`error` 留空、按 success 发出去，
    // 版本列表是空的）。四种加载器里都是**真结论**：比如 Forge 还没支持某个刚发的
    // 快照、OptiFine 只发了预览版 —— 不是网络问题、也不是清单读不懂，重试一百次
    // 还是这个答案。两种答案在数据上分得开：**空列表 = 就是没有**（界面写「无可用版本」、
    // 底下连那两档都不留）、**success=false = 没问到**（界面写「列表没取到」、
    // 底下留着那两档，因为"让安装器自己挑"那条路不依赖这份列表）。
    return result;
}

void VersionManager::installLoaderVersion(const QString &gameVersion, LoaderKind kind,
                                         const QString &javaPath,
                                         const QString &pinnedLoaderVersion) {
    if (m_loaderInstall.active()) {
        qWarning() << "A loader install is already running, refusing to start another.";
        emit loaderInstallMessage(tr("已经有一个加载器正在安装，等它结束再试。"));
        return;
    }
    if (!loaderInstallSupported(kind)) {
        // 界面那边也会拦下来并弹一句"还没做"，这里再兜一道：数据层不该被绕过去，
        // 否则症状是"点了某个还没接通的加载器，结果装出来一个纯原版"。
        emit loaderInstallMessage(
            tr("暂不支持安装 %1。").arg(loaderDisplayName(kind)));
        return;
    }
    if (downloadManager->isBusy()) {
        emit loaderInstallMessage(tr("已有下载任务正在进行，请等它结束。"));
        return;
    }
    QString lockProblem;
    if (!DirectorySessionLock::acquire(AppPaths::minecraftDir(), &lockProblem)) {
        emit loaderInstallMessage(lockProblem);
        return;
    }

    m_loaderInstall = LoaderInstall();
    if (installerProcess) {
        installerProcess->setProperty("wtfmlCertificateRetried", false);
        // 卡死重起只给一次（见 kInstallerStallTimeoutMs）；这两个计数每次新安装都从零开始。
        installerProcess->setProperty("wtfmlStallRestarts", 0);
        installerProcess->setProperty("wtfmlStallRestartPending", false);
    }
    m_loaderInstall.kind = kind;
    m_loaderInstall.gameVersion = gameVersion;
    m_loaderInstall.loaderName = loaderDisplayName(kind);
    m_loaderInstall.javaPath = javaPath;
    // 界面那一格可能是三种东西（见 LoaderMeta::loaderRequestFromChoice）：一个具体版本号、
    // `@latest` / `@stable` 两个频道键、或者空串（没挑过）。在这里**一次**解码成
    // （要钉的版本号 / 按哪一档挑），后面几条路都只读那两个字段，不再回头看那个字符串 ——
    // 解码散在多处的话，"选了最新版却装了稳定版"那种错就是对不出来。
    const LoaderMeta::LoaderRequest choice =
        LoaderMeta::loaderRequestFromChoice(pinnedLoaderVersion.trimmed());
    m_loaderInstall.pinnedVersion = choice.pinnedVersion;
    m_loaderInstall.channel = choice.channel;

    // 第一步就分开了：Fabric 拉的是"加载器版本列表"，Forge/NeoForge 拉的是"安装器该用
    // 哪个版本"，OptiFine 连"版本表"都和前三个不是一回事（镜像一条 JSON、官方一张 203 KB
    // 的网页），接口完全不是一回事（见 LoaderMeta.hpp 顶部那段）。
    if (kind == LoaderKind::Fabric || kind == LoaderKind::Quilt) {
        // 版本被钉死了就别去问版本表了 —— 直接拉那个版本的 profile。
        // NOTE: 这一步对整合包是**必须**的：它的依赖表里写着 `fabric-loader: 0.16.14`，
        // 包里那些模组正是按这个版本配的，换成"最新的正式版"是另一回事（多半也能跑，
        // 但那是猜）。镜像那份旧表挑不出来的情况也因此完全绕开了。
        if (!m_loaderInstall.pinnedVersion.isEmpty()) {
            m_loaderInstall.loaderVersion = m_loaderInstall.pinnedVersion;
            m_loaderInstall.loaderId =
                kind == LoaderKind::Quilt
                    ? LoaderMeta::quiltLoaderId(gameVersion, m_loaderInstall.pinnedVersion)
                    : LoaderMeta::fabricLoaderId(gameVersion, m_loaderInstall.pinnedVersion);
            m_loaderInstall.phase = LoaderInstall::Phase::FetchingProfile;
            emit loaderInstallMessage(tr("正在安装指定版本 %1 %2…")
                                          .arg(m_loaderInstall.loaderName,
                                               m_loaderInstall.loaderVersion));
            fetchFabricProfile();
            return;
        }

        m_loaderInstall.phase = LoaderInstall::Phase::FetchingList;
        emit loaderInstallMessage(tr("正在获取 %1 %2 加载器版本列表…")
                                      .arg(m_loaderInstall.loaderName, gameVersion));
        fetchFabricLoaderList();
        return;
    }

    m_loaderInstall.phase = LoaderInstall::Phase::FetchingInstallerList;
    emit loaderInstallMessage(
        tr("正在查找适用于 %1 %2 的版本…").arg(m_loaderInstall.loaderName, gameVersion));
    if (kind == LoaderKind::OptiFine) {
        // 钉死了就不用去问版本表 —— 和下面 Forge/NeoForge 那条一个道理，只是它得排在
        // "OptiFine 分支"**里面**（那一段的钉死判断在分支之后，够不着这里）。
        //
        // NOTE: 这个分支必须先判钉死。无条件去拉版本表的话，"钉死一个 OptiFine
        // 版本"会被无声忽略 —— 用户在安装页上选了 H7，装出来的是版本表里那条最新的 H9。
        // 症状还特别难认：装是装成了、版本目录也在，只是不是他选的那个。
        if (!m_loaderInstall.pinnedVersion.isEmpty()) {
            emit loaderInstallMessage(tr("正在安装预先指定的 %1 %2…")
                                          .arg(m_loaderInstall.loaderName,
                                               m_loaderInstall.pinnedVersion));
            adoptOptifineEdition(m_loaderInstall.pinnedVersion,
                                 LoaderMeta::optifineMirrorPathFromEdition(
                                     m_loaderInstall.pinnedVersion),
                                 downloadManager->mirrorPreferred());
            return;
        }
        fetchOptifineVersionList();
        return;
    }
    // 同上：钉死了就不用去问版本表（那张表在镜像上还是旧的，见 LoaderInstall::listOfficialUrl）。
    // NOTE: 相位先摆好再调 adoptInstallerVersion —— 里面那句 deferLoaderStep 的守卫要的
    // 就是 FetchingInstallerList。
    if (!m_loaderInstall.pinnedVersion.isEmpty()) {
        emit loaderInstallMessage(tr("正在安装预先指定的 %1 %2…")
                                      .arg(m_loaderInstall.loaderName,
                                           m_loaderInstall.pinnedVersion));
        adoptInstallerVersion(m_loaderInstall.pinnedVersion);
        return;
    }
    fetchInstallerList();
}

void VersionManager::fetchFabricLoaderList() {
    const QUrl url(m_loaderInstall.kind == LoaderKind::Quilt
                       ? LoaderMeta::quiltLoaderListUrl(m_loaderInstall.gameVersion)
                       : LoaderMeta::fabricLoaderListUrl(m_loaderInstall.gameVersion));
    QNetworkRequest request(url);
    // 30 秒是"排队 + 传输"的合计：卡死的连接会让界面永远停在 "下载中"（按钮是禁用的），
    // 而这个请求没有别的兜底。
    RequestBudget::ControlGet::start(this, loaderNetworkManager(), request, 30000,
        [this](QNetworkReply *reply, const QString &jobError) {
            onFabricLoaderListReply(reply, jobError);
        });
}

void VersionManager::onFabricLoaderListReply(QNetworkReply *reply, const QString &jobError) {
    if (reply != nullptr) {
        reply->deleteLater();
    }
    // 状态已经换过（上一次失败收了尾、或者用户又点了一次）：这份回复没人要了。
    if (m_loaderInstall.phase != LoaderInstall::Phase::FetchingList) {
        return;
    }

    // 读之前先看设备开没开：超时/掐断回来的那条已经不 open，readAll() 会甩
    // `QIODevice::read ... device not open`。
    // **仍然不拿 error() 当门**（理由见下面那条 ★★ 400 带正文的坑）。
    const int httpStatus =
        reply != nullptr ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
    const QByteArray body = reply != nullptr && reply->isOpen() ? reply->readAll() : QByteArray();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    // （1.12.2 实测）：**先看回复里有没有能读的东西，再看传输层报没报错** ——
    // 不能拿 `reply->error()` 当第一道门。
    //
    // Fabric 的接口遇到它**从没支持过**的游戏版本（1.12.2）回的是 `HTTP 400`、
    // **正文带着一个 `[]`**；Quilt 回的是 `HTTP 404`、`{"code":"not_found",...}`。
    // 若照 `error != NoError` 判断，用户拿到的是「检查网络后重试」——
    // 一个**永远不可能成功**的版本被报成网络抖动，连点多少次都是同一句话。
    // 正文读得出来就说明这一趟传输是通的，该走的是「这个版本还没支持」。
    if (!doc.isArray()) {
        if (m_loaderInstall.kind == LoaderKind::Quilt &&
            LoaderMeta::isUnsupportedGameVersionMetaReply(true, httpStatus, body)) {
            failLoaderInstall(tr("%1 还没有支持 %2 的加载器（拿到的列表是空的）。")
                                  .arg(m_loaderInstall.loaderName, m_loaderInstall.gameVersion));
            return;
        }
        failLoaderInstall(tr("拿不到 %1 的加载器列表（%2）。检查网络后重试。")
                              .arg(m_loaderInstall.gameVersion, replyReason(reply, jobError)));
        return;
    }

    const LoaderMeta::LoaderPick pick =
        m_loaderInstall.kind == LoaderKind::Quilt
            ? LoaderMeta::pickQuiltLoader(doc.array(), m_loaderInstall.channel)
            : LoaderMeta::pickFabricLoader(doc.array(), m_loaderInstall.channel);
    if (!pick.ok()) {
        // 快照、或者比 Fabric 还支持的最老那一档（1.14）更早的版本（1.12.2 就是）：接口
        // 回一个 `[]`（**而且带 400**，所以上一道门不能拿 `reply->error()` 判，见上面那条
        // NOTE）。这不是网络问题，也不能退回"装原版"——用户要的是带加载器的版本，装出来
        // 一个纯原版是骗人。
        // 列表非空但没有稳定版（Quilt 的 beta 列表就是这种）不能说成“还没支持”：
        // 用户会以为这个游戏版本一个都装不了。分开说清“只有测试版”。
        if (m_loaderInstall.kind == LoaderKind::Quilt && !doc.array().isEmpty()) {
            failLoaderInstall(tr("%1 暂无适用于 %2 的稳定版加载器。可选择「最新版」或指定版本进行安装。")
                                  .arg(m_loaderInstall.loaderName, m_loaderInstall.gameVersion));
            return;
        }
        failLoaderInstall(tr("%1 暂无适用于 %2 的加载器。")
                              .arg(m_loaderInstall.loaderName, m_loaderInstall.gameVersion));
        return;
    }

    m_loaderInstall.loaderVersion = pick.version;
    m_loaderInstall.loaderId =
        m_loaderInstall.kind == LoaderKind::Quilt
            ? LoaderMeta::quiltLoaderId(m_loaderInstall.gameVersion, pick.version)
            : LoaderMeta::fabricLoaderId(m_loaderInstall.gameVersion, pick.version);
    emit loaderInstallMessage(tr("挑中 %1 %2（%3）。")
                                  .arg(m_loaderInstall.loaderName, pick.version,
                                       pickChannelDetail(m_loaderInstall.channel, pick.stable)));
    m_loaderInstall.phase = LoaderInstall::Phase::FetchingProfile;
    fetchFabricProfile();
}

void VersionManager::fetchFabricProfile() {
    const QUrl url(m_loaderInstall.kind == LoaderKind::Quilt
                       ? LoaderMeta::quiltProfileUrl(m_loaderInstall.gameVersion,
                                                     m_loaderInstall.loaderVersion)
                       : LoaderMeta::fabricProfileUrl(m_loaderInstall.gameVersion,
                                                      m_loaderInstall.loaderVersion));
    QNetworkRequest request(url);
    RequestBudget::ControlGet::start(this, loaderNetworkManager(), request, 30000,
        [this](QNetworkReply *reply, const QString &jobError) {
            onFabricProfileReply(reply, jobError);
        });
}

void VersionManager::onFabricProfileReply(QNetworkReply *reply, const QString &jobError) {
    if (reply != nullptr) {
        reply->deleteLater();
    }
    if (m_loaderInstall.phase != LoaderInstall::Phase::FetchingProfile) {
        return;
    }

    const QByteArray body = reply != nullptr && reply->isOpen() ? reply->readAll() : QByteArray();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (reply == nullptr || reply->error() != QNetworkReply::NoError || !doc.isObject()) {
        failLoaderInstall(tr("拿不到 %1 %2 的 profile（%3）。")
                              .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion,
                                   replyReason(reply, jobError)));
        return;
    }

    // 这份 json 会**原样**落盘成 versions/<id>/<id>.json —— 它的 id 就是目录名、它的
    // inheritsFrom 决定启动时去哪找主类和客户端 jar。两条规则（自带注释）都在
    // LoaderMeta::checkFabricProfile() 里，那边是纯函数，验证工程拿真 profile 跑过。
    const QJsonObject profile = doc.object();
    const LoaderMeta::ProfileCheck check =
        LoaderMeta::checkFabricProfile(profile, m_loaderInstall.gameVersion);
    if (!check.ok()) {
        switch (check.problem) {
        case LoaderMeta::ProfileProblem::BadId:
            failLoaderInstall(tr("%1 %2 的 profile 里的版本 id 不能用（%3），没有安装。")
                                  .arg(m_loaderInstall.loaderName,
                                       m_loaderInstall.loaderVersion, check.id));
            return;
        case LoaderMeta::ProfileProblem::WrongParent:
            failLoaderInstall(tr("%1 %2 的 profile 是基于「%3」的，不是「%4」，没有安装。")
                                  .arg(m_loaderInstall.loaderName,
                                       m_loaderInstall.loaderVersion, check.parent,
                                       m_loaderInstall.gameVersion));
            return;
        case LoaderMeta::ProfileProblem::None:
            break;  // ok() 已经是 true，到不了这里
        }
        failLoaderInstall(tr("%1 %2 的 profile 没通过检查，没有安装。")
                              .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion));
        return;
    }
    const QString id = check.id;

    // 落盘由 downloadVersion() 做（它写的正是 versionJsons 里这一份，原样、不合并）。
    versionJsons[id] = profile;
    m_loaderInstall.loaderId = id;
    // 这一步在"原版已经装好"时**直接起加载器那个批次**，而这里还在网络回复的槽里。
    deferLoaderStep(LoaderInstall::Phase::FetchingProfile,
                    [this] { startParentOrLoaderFiles(); });
}

void VersionManager::startParentOrLoaderFiles() {
    const QString parent = m_loaderInstall.gameVersion;
    // "原版装过没有"看的是**磁盘上那两份文件**：版本 json（启动路径 getVersionJson →
    // 继承链要读的就是它）和客户端 jar（启动时真正要喂给 JVM 的那个）。两个都要：
    // - 只有目录、没有 json：老下载残骸。以为"装好了"的话，这个加载器版本启动时
    // 会报"版本信息缺失"。
    // - 有 json、没有 jar：**我们自己的下载残骸** —— downloadVersion() 是先写 json
    // 再下 jar 的，jar 那一批失败就剩这个形态。以为"装好了"的话，用户拿到的是
    // 一个启动不起来、而且看不出为什么的加载器版本。
    // 判成"没装"正好走上补齐那条路：downloadVersion() 会校验并只补缺的那些文件，
    // 不是重下一遍。
    if (QFile::exists(InstanceFiles::jsonPath(parent)) &&
        QFile::exists(InstanceFiles::jarPath(parent))) {
        emit loaderInstallMessage(
            tr("原版 %1 已安装，继续安装 %2 的文件。").arg(parent, m_loaderInstall.loaderName));
        startLoaderFiles();
        return;
    }

    emit loaderInstallMessage(
        tr("正在安装原版 %1 的客户端 JAR、运行库和资源…").arg(parent));
    m_loaderInstall.phase = LoaderInstall::Phase::WaitingParentJson;
    fetchVersionDetails(parent);
}

void VersionManager::startLoaderFiles() {
    const QString id = m_loaderInstall.loaderId;
    if (id.isEmpty() || !versionJsons.contains(id)) {
        failLoaderInstall(tr("%1 的 profile 没准备好，安装中止。").arg(m_loaderInstall.loaderName));
        return;
    }
    m_loaderInstall.phase = LoaderInstall::Phase::WaitingLoaderFiles;
    emit loaderInstallMessage(tr("正在下载 %1 %2 的 Maven 依赖…")
                                  .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion));
    downloadVersion(id, AppPaths::versionDir(id));
}

// ---------------------------------------------------------------------------
// Forge / NeoForge：跑它们自己的安装器
// ---------------------------------------------------------------------------

void VersionManager::fetchInstallerList() {
    // 两个接口一个回 JSON、一个回 XML，但都是"GET 一段文本，从里面挑一个版本号"，
    // 所以共用这一条路，分派放在回复处理里按 kind 做。
    // 版本表本身也走镜像：Forge 那份 1.7 KB 在 files.minecraftforge.net 上，NeoForge 那份
    // 63 KB 在 maven.neoforged.net 上，两个 host 都在 DownloadManager::mirrorUrl() 里认得了。
    // NOTE: 这条路**不是** downloadManager 发的（它只是 GET 一段文本、没有落盘），所以镜像
    // 得在这儿自己换一次 —— downloadFile() 那条路不会替它换。
    m_loaderInstall.listOfficialUrl =
        m_loaderInstall.kind == LoaderKind::Forge ? LoaderMeta::forgePromotionsUrl()
                                                  : LoaderMeta::neoforgeMavenMetadataUrl();
    const QString url = downloadManager->endpoint(m_loaderInstall.listOfficialUrl);
    m_loaderInstall.listRequestWasMirror = (url != m_loaderInstall.listOfficialUrl);
    m_loaderInstall.listRetriedOfficial = false;
    // 开局就落在镜像上的话，之后官方也失败时**不再**往镜像退（退过一次就够）。
    m_loaderInstall.listMirrorTried = m_loaderInstall.listRequestWasMirror;
    requestInstallerList(url);
}

void VersionManager::requestInstallerList(const QString &url) {
    // NOTE: 这里必须写成"先落到一个具名变量"，不能写成 QNetworkRequest request(QUrl(url)) ——
    // 那个形状在 C++ 里会被当成函数声明（most vexing parse），报的错还很难认
    // （"request 不是类类型"）。
    const QUrl target(url);
    QNetworkRequest request(target);
    RequestBudget::ControlGet::start(this, loaderNetworkManager(), request, 30000,
        [this](QNetworkReply *reply, const QString &jobError) {
            onInstallerListReply(reply, jobError);
        });
}

bool VersionManager::retryInstallerListFromOfficial() {
    // 只在"这一发是镜像发的、而且还没退过"的时候成立。退过一次还是挑不出来，那就是
    // 官方那份里真的没有 —— 不能再往回退，否则镜像、官方来回转。
    if (!m_loaderInstall.listRequestWasMirror || m_loaderInstall.listRetriedOfficial) {
        return false;
    }
    m_loaderInstall.listRetriedOfficial = true;
    m_loaderInstall.listRequestWasMirror = false;
    emit loaderInstallMessage(
        tr("镜像源版本列表未找到 %2，已切换至官方源重新获取。")
            .arg(m_loaderInstall.loaderName, m_loaderInstall.gameVersion));
    requestInstallerList(m_loaderInstall.listOfficialUrl);
    return true;
}

void VersionManager::onInstallerListReply(QNetworkReply *reply, const QString &jobError) {
    if (reply != nullptr) {
        reply->deleteLater();
    }
    // 状态已经换过（上一次失败收了尾、或者用户又点了一次）：这份回复没人要了。
    if (m_loaderInstall.phase != LoaderInstall::Phase::FetchingInstallerList) {
        return;
    }

    const QByteArray body = reply != nullptr && reply->isOpen() ? reply->readAll() : QByteArray();
    if (reply == nullptr || reply->error() != QNetworkReply::NoError) {
        if (retryInstallerListFromOfficial()) {
            return;
        }
        if (!m_loaderInstall.listMirrorTried && transientJobFailure(reply)) {
            const QString fallback = loaderVersionsMirrorFallback(
                m_loaderInstall.kind, m_loaderInstall.listOfficialUrl);
            if (!fallback.isEmpty()) {
                m_loaderInstall.listMirrorTried = true;
                m_loaderInstall.listRequestWasMirror = true;
                // 官方这条**已经**试过了（就是刚失败的这一发），镜像那条别再往回弹。
                m_loaderInstall.listRetriedOfficial = true;
                emit loaderInstallMessage(
                    tr("官方源连接失败（%2），已切换至镜像源重试。")
                        .arg(m_loaderInstall.loaderName, replyReason(reply, jobError)));
                requestInstallerList(fallback);
                return;
            }
        }
        failLoaderInstall(tr("拿不到 %1 %2 的版本表（%3）。检查网络后重试。")
                              .arg(m_loaderInstall.loaderName, m_loaderInstall.gameVersion,
                                   replyReason(reply, jobError)));
        return;
    }

    LoaderMeta::LoaderPick pick;
    if (m_loaderInstall.kind == LoaderKind::Forge) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (!doc.isObject()) {
            failLoaderInstall(tr("Forge 的版本表无法解析（%1）。").arg(parseError.errorString()));
            return;
        }
        pick = LoaderMeta::pickForgeVersion(doc.object(), m_loaderInstall.gameVersion,
                                            m_loaderInstall.channel);
    } else {
        pick = LoaderMeta::pickNeoForgeVersion(body, m_loaderInstall.gameVersion,
                                               m_loaderInstall.channel);
    }

    if (!pick.ok()) {
        // 挑不出来有两种可能：镜像那份是旧的（**多数情况**，见 listOfficialUrl 的注释），
        // 或者上游真的还没支持这个游戏版本。先问一次官方那份，两个都没有才是后者。
        if (retryInstallerListFromOfficial()) {
            return;
        }
        // 和 Fabric 那条一样：这不是网络问题，也**不能**退回"装原版"——用户要的是带
        // 加载器的版本，装出来一个纯原版是骗人。
        failLoaderInstall(tr("%1 暂无支持 %2 的版本。")
                              .arg(m_loaderInstall.loaderName, m_loaderInstall.gameVersion));
        return;
    }

    emit loaderInstallMessage(tr("挑中 %1 %2（%3）。")
                                  .arg(m_loaderInstall.loaderName, pick.version,
                                       pickChannelDetail(m_loaderInstall.channel, pick.stable)));
    // 剩下的（loaderId / 安装器地址 / 文件名 / 排下一轮）和"整合包钉死版本"那条路共用一份：
    // 两块地方各写一遍的话，迟早只有一边记得改（比如 installerFileName 那段注释里的坑）。
    adoptInstallerVersion(pick.version);
}

void VersionManager::startInstallerDownload() {
    // 文件名是拉版本表那一步定下来的（installerFileName），这里不再从地址里现切：两条路
    // 的来源本来就不一样——Forge/NeoForge 取地址最后一段（和上游同名），OptiFine 那边地址
    // 最后两段是 type/patch、只能用它在版本表里挑中的那个文件名。版本目录名那边不这么干
    // 是另一回事：那个名字是别的启动器认的，不能改。
    if (m_loaderInstall.installerFileName.isEmpty()) {
        failLoaderInstall(tr("没有拿到 %1 安装器的文件名，安装中止。")
                              .arg(m_loaderInstall.loaderName));
        return;
    }
    m_loaderInstall.installerPath =
        AppPaths::installerCacheDir() + QLatin1Char('/') + m_loaderInstall.installerFileName;

    emit loaderInstallMessage(tr("正在下载 %1 %2 安装器…")
                                  .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion));

    beginOwnBatch();
    // phase 放在 beginBatch() **之后**设：那个函数在"上一批还没收尾就又开一批"那条异常
    // 支路里会替上一批收尾、并发一次 allDownloadsFinished（见它的实现），那一次不该被
    // 当成「安装器下好了」。正常路径上调用方前面已经用 isBusy() 挡过，走不到那条支路；
    // 顺序仍然按这个摆（先清场、再进相位）。
    m_loaderInstall.phase = LoaderInstall::Phase::WaitingInstallerJar;
    // 安装器 jar 没有公布 sha1（Forge 只在下载页给别的东西，NeoForge 的 maven-metadata
    // 里也没有），所以这里只能"文件在就算好" —— 第二次装同一个加载器版本时省掉这 6 MB。
    //
    // NOTE: 这一条路上没有 sha1，也没有字节数 —— 一份 200 + HTML 错误页会被当成"下好了"
    // 落盘，而且因为"已存在"**再也不会重下**，症状是那个加载器版本永远装不上、
    // 每次都在同一处失败。所以这一条路上还剩的判据是 **jar 必须是 zip**（`PK` 那四个
    // 字节），判在 DownloadManager 里那两处"别的判据都没有"的支路上（落盘那一步 +
    // fileAlreadyValid）。**不是**真的完整性校验 —— 拿不到 sha1 就是拿不到，这一句
    // 别读成"已经校验过了"。
    // 安装器 jar 也走镜像（6 MB 上下，是这条路上最大的一笔）。镜像上那份和官方是同一个
    // 文件。installerUrl 本身**不改**：盘上那个文件名是从它切的，
    // 两条路上游的文件名一模一样。
    const QString installerUrl =
        downloadManager->useMirror()
            ? downloadManager->endpoint(m_loaderInstall.installerUrl)
            : m_loaderInstall.installerUrl;
    // 第六个参数是官方那条地址：安装器 jar 是这条路上最大的一笔（6 MB 上下），
    // 镜像取不回来时按官方再试一次，比整条安装从头再来划算。
    downloadManager->downloadFile(installerUrl, m_loaderInstall.installerPath,
                                  QString(), -1, VerifyMode::ExistsOnly,
                                  m_loaderInstall.installerUrl);
    endOwnBatch();
}

bool VersionManager::retryInstallerDownloadAlternate() {
    if (m_loaderInstall.kind != LoaderKind::Forge || m_loaderInstall.retriedAltCoordinate) {
        return false;
    }
    const QString alt = LoaderMeta::forgeInstallerUrlAlternate(m_loaderInstall.gameVersion,
                                                              m_loaderInstall.loaderVersion);
    if (alt.isEmpty() || alt == m_loaderInstall.installerUrl) {
        return false;
    }
    m_loaderInstall.retriedAltCoordinate = true;
    // 文件名是从地址最后一段切的、两种坐标的文件名不一样（`...11.15.1.2318-installer.jar`
    // vs `...11.15.1.2318-1.8.9-installer.jar`），所以这三样都得跟着换 —— 只换地址会让
    // 下载落到上一轮那个名字上，装完核对时对不上。
    m_loaderInstall.installerUrl = alt;
    m_loaderInstall.installerFileName = alt.section(QLatin1Char('/'), -1);
    m_loaderInstall.installerPath = AppPaths::installerCacheDir() + QLatin1Char('/') +
                                    m_loaderInstall.installerFileName;
    emit loaderInstallMessage(
        tr("未在常规位置找到 %1 %2 安装器，正在尝试旧版下载地址…")
            .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion));
    startInstallerDownload();
    return true;
}

bool VersionManager::ensureLauncherProfiles(const QString &minecraftDir, bool *createdOut,
                                            QString *errorOut) {
    if (createdOut != nullptr) {
        *createdOut = false;
    }
    const QString path =
        minecraftDir + QLatin1Char('/') + LoaderMeta::launcherProfilesFileName();
    if (QFile::exists(path)) {
        // 已经有就**一个字都不动**：这个文件是原版启动器和 PCL 都在写的东西，里面可能
        // 是用户真正的启动器档案。安装器会往里面**加**一条 profile，加完照样是有用的；
        // 我们覆盖它就是把人家的东西弄丢。
        return true;
    }

    QDir().mkpath(minecraftDir);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut != nullptr) {
            *errorOut = tr("无法在游戏目录中创建 %2 安装器所需的 %1：%3")
                            .arg(LoaderMeta::launcherProfilesFileName(),
                                 m_loaderInstall.loaderName, file.errorString());
        }
        return false;
    }
    file.write(LoaderMeta::minimalLauncherProfiles());
    if (!file.commit()) {
        if (errorOut != nullptr) {
            *errorOut = tr("%2 的安装器需要游戏目录中的 %1，但无法创建此文件（%3）。")
                            .arg(LoaderMeta::launcherProfilesFileName(),
                                 m_loaderInstall.loaderName, file.errorString());
        }
        return false;
    }
    if (createdOut != nullptr) {
        *createdOut = true;
    }
    return true;
}

VersionManager::InstallerOptions VersionManager::parseInstallerHelp(const QString &helpText) {
    InstallerOptions options;
    options.probed = true;
    // 只做"整段里有没有这个串"的判断：两个安装器的选项名互不包含（实测 1.8.9 那份帮助
    // 545 B、`--installClient` 和 `--mirror` 一个都没有；NeoForge 26.2 那份 2572 B、两个都有），
    // 不需要更细的解析 —— 真要按行解析 joptsimple 的排版反而更容易被下一个版本改坏。
    options.supportsInstallClient = helpText.contains(QStringLiteral("--installClient"));
    options.supportsMirror = helpText.contains(QStringLiteral("--mirror"));
    return options;
}

QStringList VersionManager::forgeInstallerArguments(const QString &installerPath,
                                                    const QString &targetDir,
                                                    const QString &mavenRoot,
                                                    bool supportsMirror) {
    // `--installClient` 后面那个目录**必须**给：不给的话安装器会落到 user.home/.minecraft
    // （这台机器上是 C:\Users\User\.minecraft），装到一个用户根本不知道的地方去。
    QStringList args{QStringLiteral("-jar"), installerPath, QStringLiteral("--installClient"),
                     targetDir};
    // 把安装器**自己**那条下载路钉到一个明确的根上。不给它这个参数时它不去官方 maven，
    // 而是按内置的第三方镜像列表挑（本机挑中 creeperhost）：实测慢一倍（48 个库 61 秒 vs
    // 30 秒），而且那条连接一坏它就把整次安装判死 —— 的 Forge 1.20.1 就是这样
    // 死在 `org.ow2.asm:asm:9.6` 上的。所以**两种源模式都要给**：
    // * 镜像模式 → BMCLAPI 的 /maven/；
    // * 官方模式 → 加载器自己的官方 maven 根（见 LoaderMeta::forgeMavenRoot() 那一节）。
    // 它只对库生效 —— 原版那几个库（libraries.minecraft.net）它照旧走自己那条路。
    // 探到它不认 `--mirror`（老安装器）就**不能给**：joptsimple 把不认识的选项当致命错误，
    // 当场退出码 1。宁可这一条路慢一点，也不能让整个安装死在"多给了一个参数"上。
    if (!mavenRoot.isEmpty() && supportsMirror) {
        args << QStringLiteral("--mirror") << mavenRoot;
    }
    return args;
}

void VersionManager::runInstaller() {
    const bool optifine = m_loaderInstall.kind == LoaderKind::OptiFine;

    // 往哪个目录装：Forge/NeoForge 直接装进真游戏目录（它们的安装器认 `--installClient`
    // 给的那个目录）；OptiFine 装进**我们自己建的一个临时目录**，因为它的安装器参数整个
    // 丢掉、只从 APPDATA 算（见 LoaderMeta.hpp 里 OptiFine 那一节）。
    //
    // NOTE: 换 APPDATA 这一招**只在 Windows 上成立**。安装器是按 os.name 分支算游戏目录的：
    // Windows 走 `%APPDATA%/.minecraft`，Linux 走 `user.home/.minecraft` —— 后者改
    // 环境变量根本改不动（Java 是从系统里取的，不是从环境变量里读的），于是它会跑到
    // 用户真实的 ~/.minecraft 里去动手。那种平台上宁可不装，也不能这么干。
    QString targetDir = AppPaths::minecraftDir();
    if (optifine) {
#ifdef Q_OS_WIN
        QString error;
        if (!prepareOptifineWorkDir(&error)) {
            failLoaderInstall(error);
            return;
        }
        targetDir = optifineWorkMinecraftDir();
#else
        failLoaderInstall(tr("OptiFine 的安装器需要 Windows 的 APPDATA 目录，当前平台暂不支持此安装方式。"));
        return;
#endif
    }

    bool created = false;
    QString error;
    if (!ensureLauncherProfiles(targetDir, &created, &error)) {
        failLoaderInstall(error);
        return;
    }
    // OptiFine 那一份是补给我们自己的临时目录的（每次装必然是新建），用户没必要知道。
    if (created && !optifine) {
        emit loaderInstallMessage(
            tr("正在创建 %2 安装器所需的 %1，仅写入安装所需的内容。")
                .arg(LoaderMeta::launcherProfilesFileName(), m_loaderInstall.loaderName));
    }

    const QString program = m_loaderInstall.javaPath.isEmpty() ? QStringLiteral("java")
                                                               : m_loaderInstall.javaPath;
    // 只有"看着像路径"的时候才判它在不在盘上。裸名字（`java`）走的是 PATH 查找，而
    // QFileInfo("java").exists() 一定返回 false —— 拿它当判据会把"设置里没指定、但 PATH
    // 上有 java"这种情况误杀，而那正是这条路的默认情形（javaPath 传空就是这么来的）。
    const bool looksLikePath = program.contains(QLatin1Char('/')) ||
                               program.contains(QLatin1Char('\\'));
    if (looksLikePath && !QFileInfo::exists(program)) {
        failLoaderInstall(tr("找不到 Java 可执行文件（%1），%2 的安装器起不来。\n到「设置」里指定一个 Java，或者让系统 PATH 上有 java。")
                              .arg(program, m_loaderInstall.loaderName));
        return;
    }

    if (installerProcess == nullptr) {
        installerProcess = new QProcess(this);
        connect(installerProcess, &QProcess::readyReadStandardOutput, this,
                [this] { onInstallerOutput(); });
        connect(installerProcess, &QProcess::readyReadStandardError, this,
                [this] { onInstallerOutput(); });
        connect(installerProcess, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError code) { onInstallerError(static_cast<int>(code)); });
        connect(installerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus status) {
                    onInstallerFinished(exitCode, status != QProcess::NormalExit);
                });
        installerTimer = new QTimer(this);
        installerTimer->setInterval(kInstallerTickMs);
        connect(installerTimer, &QTimer::timeout, this, &VersionManager::onInstallerTick);
    }

    // 这一次用的 Java 和目录存成成员：探完 `--help` 之后还要用同一份起真正的安装（见
    // probeInstallerOptions / startInstallerProcess）。
    m_installerProgram = program;
    m_installerTargetDir = targetDir;
    m_installerOptions = InstallerOptions();

    // 先探一次安装器的命令行开关（只有 Forge/NeoForge 探：OptiFine 那个入口是类名，
    // 压根没有 `--help` 这回事）。
    if (!optifine) {
        probeInstallerOptions();
        return;
    }
    startInstallerProcess();
}

void VersionManager::probeInstallerOptions() {
    m_probeOutput.clear();
    m_installerStartedAt = QDateTime::currentMSecsSinceEpoch();
    // phase 要在 start() **之前**设：进程起不来时 errorOccurred 会回来，那条路要靠
    // phase 认出"这次是探针"（见 onInstallerError）。
    m_loaderInstall.phase = LoaderInstall::Phase::ProbingInstallerOptions;
    emit loaderInstallMessage(tr("正在检查 %1 安装器支持的命令行选项…")
                                  .arg(m_loaderInstall.loaderName));

    installerProcess->setProgram(m_installerProgram);
    installerProcess->setArguments({QStringLiteral("-jar"), m_loaderInstall.installerPath,
                                    QStringLiteral("--help")});
    // 环境用**系统的**：探针不该带 OptiFine 那个换过 APPDATA 的环境（那条路本来也不探）。
    installerProcess->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    // 工作目录挑安装器 jar 自己所在的那个目录（cache/installers/）：老安装器起来会往 cwd
    // 里写一个 `<jar 名>.log`，扔在缓存目录里比扔进用户的游戏目录干净。
    installerProcess->setWorkingDirectory(
        QFileInfo(m_loaderInstall.installerPath).absolutePath());
    installerProcess->setProperty("wtfmlLastOutputMs", QDateTime::currentMSecsSinceEpoch());
    installerProcess->start();
    installerTimer->start();
}

void VersionManager::onProbeFinished(int exitCode, bool crashed) {
    if (installerTimer != nullptr) {
        installerTimer->stop();
    }
    onInstallerOutput();  // 收尾前把剩下的输出读干净（这一相位下它读进 m_probeOutput）

    const QString help = QString::fromLocal8Bit(m_probeOutput);
    if (crashed || exitCode != 0 || help.trimmed().isEmpty()) {
        // 探不出来（起不来 / 超时被杀 / 一声不吭）：按"宁可信其有"往下走，让真正那一次
        // 安装去撞南墙、报它自己的错 —— 探针是为了少一次莫名其妙的报错，不能反过来变成
        // 新的把能装的版本拦下来的关。
        m_installerOptions = InstallerOptions();
        qWarning() << "Installer option probe inconclusive: exit" << exitCode << "crashed" << crashed
                   << installerOutputTail(m_probeOutput);
    } else {
        m_installerOptions = parseInstallerHelp(help);
        qDebug() << "Installer options:" << m_installerOptions.probed
                 << m_installerOptions.supportsInstallClient << m_installerOptions.supportsMirror;
    }

    if (!m_installerOptions.supportsInstallClient) {
        // 老安装器把"装客户端"做成了图形界面（不给参数就弹窗等人点），命令行里没有这条路。
        // 别在这儿判死刑 —— 那个年代的安装器把自己该做的事全写在自己 jar 里那份
        // install_profile.json 里了，照着它装**既不用开它的界面、也不用起 JVM**。
        // 也不能报"本启动器替你点不了"让用户去找别的启动器：一台只有本启动器的新电脑上，
        // 默认挑的那个「稳定版」（1.12.2 就是 2847）永远装不上。见 LegacyForgeInstall.hpp 文件头。
        startLegacySelfInstall();
        return;
    }

    if (m_installerOptions.probed && !m_installerOptions.supportsMirror &&
        downloadManager->mirrorPreferred()) {
        emit loaderInstallMessage(tr("安装器不支持 --mirror，将从官方源下载其依赖。启动器负责的下载仍使用当前镜像。"));
    }
    startInstallerProcess();
}

// ---------------------------------------------------------------------------
// 老 Forge（那个年代的安装器没有命令行安装这条路）：照它自己那份安装说明装
// ---------------------------------------------------------------------------
//
// 为什么走这条路、以及它自己那三条实测结论（universal jar 不能带 url、没有 url 的库往
// Mojang 猜、原版已经带上的要丢掉）全在 LegacyForgeInstall.hpp 的文件头里。
//
// 这里只做"接线"那一半：从安装器 jar 里读出安装说明和那颗 universal jar、落盘摆好，剩下
// 的两段复用既有的路 —— 先补原版（WaitingParentJson → WaitingParentFiles），再下加载器
// 自己那几个库（startLoaderFiles()）。

void VersionManager::startLegacySelfInstall() {
    m_loaderInstall.legacySelfInstall = true;
    emit loaderInstallMessage(
        tr("%1 %2 安装器仅提供图形客户端安装入口。启动器将根据 JAR 中的 install_profile.json 安装客户端文件。")
            .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion));

    // 先取原版那份 json。它有两个用处：安装说明里那些"没有 url、但原版自己已经带着"的库
    // 要靠它认出来（认出来就不用下重），以及"这份版本基于哪个原版"要和它核对一遍。
    m_loaderInstall.phase = LoaderInstall::Phase::WaitingParentJson;
    fetchVersionDetails(m_loaderInstall.gameVersion);
}

void VersionManager::writeLegacyProfile(const QJsonObject &parentJson) {
    // 认不出来时的那句话：**别**把"读不懂它的说明"说成"这个版本装不了"—— 前者的药是
    // 换一份安装器 / 修 jar，后者是去找别的启动器。两颗参数各用一次（同一个编号写两遍
    // 会被 arg 全部替换， 那个 %1 的坑就在旁边）。
    const auto failWithProblem = [this](const QString &reason) {
        const QString namedVersion =
            m_loaderInstall.loaderName + QLatin1Char(' ') + m_loaderInstall.gameVersion;
        failLoaderInstall(
            tr("无法根据 %1 %2 的 install_profile.json 安装客户端：%3\n请检查 %4 的格式和完整性。\n可在原版启动器或 PCL 中安装 %5，再由本启动器识别。")
                .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion, reason,
                     m_loaderInstall.installerFileName, namedVersion));
    };

    // 两个条目都从 cache/installers/ 下那份安装器 jar 里读（只读，不动别的任何东西）。
    QByteArray profileBytes;
    QString error;
    const QString readWhat = tr("读 %1 的安装说明").arg(m_loaderInstall.installerFileName);
    const ZipUtil::EntryStatus profileStatus = ZipUtil::readEntry(
        m_loaderInstall.installerPath, QStringLiteral("install_profile.json"), &profileBytes,
        &error, readWhat);
    if (profileStatus != ZipUtil::EntryStatus::Ok) {
        // NotFound 那一档 errorOut 是空的（没有细节可给）—— 自己说一句。
        failWithProblem(profileStatus == ZipUtil::EntryStatus::NotFound
                            ? tr("jar 里没有 install_profile.json 这个文件")
                            : error);
        return;
    }

    const LegacyForgeInstall::Plan plan = LegacyForgeInstall::planFromInstallProfile(
        profileBytes, parentJson, m_loaderInstall.loaderId);
    if (!plan.ok) {
        using LegacyForgeInstall::Problem;
        QString reason;
        switch (plan.problem) {
        case Problem::NotJson:
            reason = tr("该文件不是一份 JSON（已损坏）");
            break;
        case Problem::ModernProfile:
            reason = tr("安装器中缺少 install 或 versionInfo 字段。此格式应通过安装器的命令行安装。");
            break;
        case Problem::MissingInstall:
            reason = tr("install 字段缺少 %1").arg(plan.detail);
            break;
        case Problem::UnknownPath:
            reason = tr("install.path「%1」拼不出一个安全的 maven 路径").arg(plan.detail);
            break;
        case Problem::NoParent:
            reason = tr("继承字段 inheritsFrom 和 install.minecraft 均为空，无法确定原版游戏版本");
            break;
        case Problem::NoLibraries:
            reason = tr("它的库表是空的");
            break;
        case Problem::None:
            break;
        }
        failWithProblem(reason);
        return;
    }

    // 说明书说要基于的原版，必须就是我们正要装的那个 —— 不是的话，照着它写出来的 json 会
    // 指向一个我们**没下过**的原版（启动时那一句 NoClassDefFoundError 就是这么来的）。
    // 实测两份 profile 里都是同一个值，这一条是防"哪天上游改了"。
    if (plan.inheritsFrom != m_loaderInstall.gameVersion) {
        failWithProblem(tr("安装包声明的原版版本为「%1」，所选版本为「%2」")
                            .arg(plan.inheritsFrom, m_loaderInstall.gameVersion));
        return;
    }

    // 它 jar 里那颗 universal jar（真实安装器会把它摆进 libraries/，我们照摆）。
    // 上限 64 MiB（R11）：老版本 Forge 的 universal jar 有十几 MB，而默认重载的上限
    // （16 MiB）对它是紧的；64 MiB 既装得下真货，也挡得住"同名条目是个几 GB 的炸弹"。
    QByteArray universal;
    const ZipUtil::EntryStatus universalStatus = ZipUtil::readEntry(
        m_loaderInstall.installerPath, plan.universalEntry, &universal, &error,
        tr("读 %1 里的 %2").arg(m_loaderInstall.installerFileName, plan.universalEntry),
        64LL * 1024 * 1024);
    if (universalStatus != ZipUtil::EntryStatus::Ok) {
        failWithProblem(universalStatus == ZipUtil::EntryStatus::NotFound
                            ? tr("jar 里没有 %1 这个文件").arg(plan.universalEntry)
                            : error);
        return;
    }

    // 摆到 libraries/<它自己的 maven 坐标算出来的位置>。这份 json 里那颗库是**只有 name、
    // 没有 url** 的写法（见 LegacyForgeInstall.hpp），所以它不会被下载 —— 就是在等这一份。
    const QString universalTarget =
        AppPaths::libraryDir() + QLatin1Char('/') + plan.universalLibraryPath;
    if (!QDir().mkpath(QFileInfo(universalTarget).absolutePath())) {
        failWithProblem(tr("创建目录失败：%1").arg(QFileInfo(universalTarget).absolutePath()));
        return;
    }
    {
        // QSaveFile：和版本 json 那边同一个理由 —— 半截的 jar 留在 libraries/ 里，症状是
        // 启动时一个莫名其妙的类找不到，而"装"的那一步一路绿灯。
        QSaveFile file(universalTarget);
        if (!file.open(QIODevice::WriteOnly)) {
            failWithProblem(tr("无法写入 %1（%2）").arg(universalTarget, file.errorString()));
            return;
        }
        file.write(universal);
        if (!file.commit()) {
            failWithProblem(tr("写 %1 的时候失败了（%2）").arg(universalTarget, file.errorString()));
            return;
        }
    }

    // 版本 json 落盘（id 是**我们的**那个，见 LegacyForgeInstall.hpp 里那条 NOTE）。
    if (!writeVersionJson(m_loaderInstall.loaderId, AppPaths::versionDir(m_loaderInstall.loaderId),
                          plan.versionJson, &error)) {
        failWithProblem(error);
        return;
    }
    // 内存里那份也要摆好：下一步 startLoaderFiles() 走的就是它（它只认 versionJsons 里那张
    // 表，不是盘上那份）。
    versionJsons[m_loaderInstall.loaderId] = plan.versionJson;

    emit loaderInstallMessage(
        tr("已保存 %1 %2 的版本清单（%3 项运行库，其中 %4 项由原版提供）。正在补齐原版 %5 及加载器的运行库…")
            .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion)
            .arg(plan.versionJson.value(QStringLiteral("libraries")).toArray().size())
            .arg(plan.droppedFromParent)
            .arg(m_loaderInstall.gameVersion));

    // 和安装器那条路一样：装完接着把原版补齐（安装器不下资源文件，我们更不会）。这一批
    // 是从"刚收到原版 json"那条栈上起的，排到下一轮（同 startParentCompletion 那边）。
    const QString parent = m_loaderInstall.gameVersion;
    m_loaderInstall.phase = LoaderInstall::Phase::WaitingParentFiles;
    deferLoaderStep(LoaderInstall::Phase::WaitingParentFiles, [this, parent] {
        downloadVersion(parent, AppPaths::versionDir(parent));
    });
}

void VersionManager::startInstallerProcess() {
    const bool optifine = m_loaderInstall.kind == LoaderKind::OptiFine;

    m_installerOutput.clear();
    m_installerTicks = 0;
    m_installerStartedAt = QDateTime::currentMSecsSinceEpoch();

    // phase 要在 start() **之前**设：进程起不来时 errorOccurred 会回来，那条路要靠
    // phase 认出"这次是安装器的事"。
    m_loaderInstall.phase = LoaderInstall::Phase::RunningInstaller;
    if (optifine) {
        emit loaderInstallMessage(
            tr("正在运行 %1 %2 的安装器...（它本身是个图形程序，我们用的是它的无界面入口；这一步最慢：它要把原版的客户端 jar 抄进新版本目录，再把两个库放好）")
                .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion));
    } else {
        emit loaderInstallMessage(
            tr("正在运行 %1 %2 的安装器...（这一步最慢：它要下十几个库，还要把原版的客户端 jar 打两遍补丁）")
                .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion));
    }

    // program 和 arguments 都是**常驻**的（和下面那个环境一样）：`start()` 不带参数走的
    // 就是上一次设的那个 program。所以这条路每次都得把它设一遍 —— 漏了它的症状是
    // "安装器起不来（那个 Java 进程没能启动）"，因为 QProcess 拿着一个空 program 去 start。
    // （探 `--help` 那一次也是同一个 QProcess：它的 program/arguments 就是在这儿被覆盖掉的。）
    installerProcess->setProgram(m_installerProgram);

    // 环境**每次都显式设一遍**，不是"只在 OptiFine 时设"：QProcess 的环境是常驻的
    // （set 一次之后每次 start 都用它），OptiFine 那条路换过 APPDATA 之后不还原的话，下一次
    // 装 Forge/NeoForge 会把 APPDATA 指到一个**已经删掉的**临时目录上。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (optifine) {
        // 只有 Windows 能走到这里（上面那道守卫）。
        //
        // * 入口是 `optifine.Installer`：jar 的 Main-Class 是 `optifine.InstallerFrame`
        // （Swing 窗口，headless 下当场 HeadlessException），真正的无界面入口是前者，
        // 而且**一个参数都不用给**（版本号它自己从自己那份 jar 里读）。
        // * -Djava.awt.headless=true 是**必须**的：它出错时会弹一个框等人点，不设这个的话
        // 进程永不退出，只能等我们那个半小时的超时把它杀掉。
        // * APPDATA 指到临时目录：它只认这一个地方，命令行里给目录它看都不看。
        env.insert(QStringLiteral("APPDATA"), optifineWorkRoot());
        installerProcess->setArguments({QStringLiteral("-Djava.awt.headless=true"),
                                        QStringLiteral("-cp"), m_loaderInstall.installerPath,
                                        QStringLiteral("optifine.Installer")});
    } else {
        // 和原版启动器一样的调用方式。命令行本体在 forgeInstallerArguments() 里（是个纯
        // 函数，`--mirror` 那一条就是靠它被钉住的，见 verify 的 V6c）。
        // 传的是 mirrorPreferred() 而不是 useMirror()：镜像正被降级时**不能**再把安装器
        // 拨到它那边去 —— 降级的判据是"这条链子慢到 10 秒给不出 64 KB"，和走的是哪条
        // 路由无关（maven 那一支实测快，但真慢起来也不该硬塞）。
        // 但**官方模式不是"不给 --mirror"**：不给它就会去 creeperhost（见函数里那条 ★）。
        const QString mavenRoot =
            downloadManager->mirrorPreferred()
                ? DownloadManager::mirrorMavenRootUrl()
                : (m_loaderInstall.kind == LoaderKind::NeoForge ? LoaderMeta::neoForgeMavenRoot()
                                                                : LoaderMeta::forgeMavenRoot());
        installerProcess->setArguments(forgeInstallerArguments(
            m_loaderInstall.installerPath, m_installerTargetDir, mavenRoot,
            m_installerOptions.supportsMirror));
    }
    installerProcess->setProcessEnvironment(env);
    installerProcess->setWorkingDirectory(m_installerTargetDir);
    installerProcess->setProperty("wtfmlLastOutputMs", QDateTime::currentMSecsSinceEpoch());
    installerProcess->start();
    installerTimer->start();
}

void VersionManager::onInstallerOutput() {
    if (installerProcess == nullptr) {
        return;
    }
    // 两个通道都读、都往一个缓冲区里放：安装器的诊断信息 stdout 和 stderr 都有，
    // 而我们要的只是"最后那几行说了什么"。混在一起的代价是万一两者交替写、行会交错，
    // 那种情况下报出来的诊断仍然够用。
    //
    // 探 `--help` 那一次是**同一个** QProcess，输出分流到另一个缓冲区（见 m_probeOutput）。
    QByteArray *sink = m_loaderInstall.phase == LoaderInstall::Phase::ProbingInstallerOptions
                           ? &m_probeOutput
                           : &m_installerOutput;
    const QByteArray standardOutput = installerProcess->readAllStandardOutput();
    const QByteArray standardError = installerProcess->readAllStandardError();
    if (!standardOutput.isEmpty() || !standardError.isEmpty()) {
        // 卡死看门狗（onInstallerTick）就问这一个时刻：有字节出来 = 它还活着。
        installerProcess->setProperty("wtfmlLastOutputMs",
                                      QDateTime::currentMSecsSinceEpoch());
    }
    *sink += standardOutput;
    *sink += standardError;
    if (sink->size() > kInstallerOutputKeepBytes) {
        *sink = sink->right(kInstallerOutputKeepBytes);
    }
}

QString VersionManager::installerLastOutputLine() const {
    const QStringList lines = installerOutputLines(m_installerOutput);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (!line.isEmpty()) {
            return line;
        }
    }
    return QString();
}

void VersionManager::onInstallerTick() {
    if (m_loaderInstall.phase == LoaderInstall::Phase::ProbingInstallerOptions) {
        const qint64 probeElapsed = QDateTime::currentMSecsSinceEpoch() - m_installerStartedAt;
        if (probeElapsed >= kInstallerProbeTimeoutMs) {
            // 探针挂住了（它本该打一段帮助就退出）：杀掉它，**不报错** —— finished 那一路会
            // 把结论落成"宁可信其有"然后接着跑真正的安装（见 onProbeFinished）。
            qWarning() << "Installer option probe timed out, going with permissive defaults";
            installerProcess->kill();
        }
        return;
    }
    if (m_loaderInstall.phase != LoaderInstall::Phase::RunningInstaller) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = now - m_installerStartedAt;

    // 先看"还活不活"，再看"跑了多久"：前者是真正的卡死（死连接），后者只是慢。
    const qint64 lastOutput = installerProcess->property("wtfmlLastOutputMs").toLongLong();
    const qint64 silentMs = lastOutput > 0 ? now - lastOutput : 0;
    if (silentMs >= kInstallerStallTimeoutMs) {
        const QString last = installerLastOutputLine();
        const int restarts = installerProcess->property("wtfmlStallRestarts").toInt();
        if (restarts < 1) {
            installerProcess->setProperty("wtfmlStallRestarts", restarts + 1);
            installerProcess->setProperty("wtfmlStallRestartPending", true);
            qWarning() << "Installer produced no output for" << silentMs / 1000
                       << "seconds; restarting it once. Last line:" << last;
            emit loaderInstallMessage(
                tr("%1 安装器已连续 %2 秒未输出，正在重启安装器。已下载的库会保留…")
                    .arg(m_loaderInstall.loaderName)
                    .arg(silentMs / 1000));
            installerTimer->stop();
            installerProcess->kill();
        } else {
            abortInstaller(tr("%1 安装器重试后仍连续 %2 秒未输出，已停止安装。\n最后输出：%3\n请检查网络后重试。若仍失败，可提供安装日志。")
                               .arg(m_loaderInstall.loaderName)
                               .arg(silentMs / 1000)
                               .arg(last));
        }
        return;
    }

    if (elapsed >= kInstallerTimeoutMs) {
        abortInstaller(tr("%1 的安装器运行超过 %2 分钟，已停止。\n安装器的最后一条输出：%3\n网络连接超时。请稍后重试，或在反复失败时导出日志。")
                           .arg(m_loaderInstall.loaderName)
                           .arg(elapsed / 60000)
                           .arg(installerLastOutputLine()));
        return;
    }

    if (++m_installerTicks % kInstallerHeartbeatSeconds != 0) {
        return;
    }
    // 心跳，不是进度：说清"还活着"和"它现在在干什么"，但绝不逐行转安装器的输出。
    const QString last = installerLastOutputLine();
    emit loaderInstallMessage(last.isEmpty()
                                  ? tr("%1 安装器已运行 %2 秒…")
                                        .arg(m_loaderInstall.loaderName)
                                        .arg(elapsed / 1000)
                                  : tr("%1 的安装器还在跑（%2 秒）：%3")
                                        .arg(m_loaderInstall.loaderName)
                                        .arg(elapsed / 1000)
                                        .arg(last));
}

void VersionManager::onInstallerError(int processError) {
    if (m_loaderInstall.phase == LoaderInstall::Phase::ProbingInstallerOptions) {
        // 探针没能起来：**不在这里报错**，按"宁可信其有"往下走 —— 真正那一次安装会撞上
        // 同一个错、报出给用户看的那句话（"安装器起不来，到设置里换一个 Java"）。
        if (static_cast<QProcess::ProcessError>(processError) != QProcess::FailedToStart) {
            return;  // 别的错误都跟着一个 finished 信号，那条路会收尾
        }
        if (installerTimer != nullptr) {
            installerTimer->stop();
        }
        m_installerOptions = InstallerOptions();
        startInstallerProcess();
        return;
    }
    if (m_loaderInstall.phase != LoaderInstall::Phase::RunningInstaller) {
        return;
    }
    // FailedToStart 是唯一**不会**跟着一个 finished 信号的错误（进程压根没起来），
    // 所以要在这里收尾。别的（Crashed / Timedout）都会走到 finished。
    if (static_cast<QProcess::ProcessError>(processError) != QProcess::FailedToStart) {
        return;
    }
    abortInstaller(tr("无法启动 %1 安装器的 Java 进程。\n请在「设置」中选择可用的 Java 后重试。")
                       .arg(m_loaderInstall.loaderName) +
                   ProcessPathLimit::tooLongHint({m_installerProgram, m_installerTargetDir}));
}

void VersionManager::abortInstaller(const QString &reason) {
    if (installerTimer != nullptr) {
        installerTimer->stop();
    }
    // 先把 phase 挪开**再**杀：kill() 会让那个进程的 finished 回来，而它一进
    // onInstallerFinished 就会看到 phase 还是 RunningInstaller —— 于是又收一次尾
    // （两个完成信号，UI 那边第二个会把刚恢复的界面再按一次）。挪开之后那条被守卫挡掉。
    m_loaderInstall.phase = LoaderInstall::Phase::Idle;
    if (installerProcess != nullptr && installerProcess->state() != QProcess::NotRunning) {
        installerProcess->kill();
        installerProcess->waitForFinished(5000);
    }
    failLoaderInstall(reason);
}

void VersionManager::onInstallerFinished(int exitCode, bool crashed) {
    if (m_loaderInstall.phase == LoaderInstall::Phase::ProbingInstallerOptions) {
        onProbeFinished(exitCode, crashed);
        return;
    }
    // 超时那一路已经收过尾了（见 abortInstaller）：这里只认还走在 RunningInstaller 上的。
    if (m_loaderInstall.phase != LoaderInstall::Phase::RunningInstaller) {
        return;
    }
    // 卡死看门狗杀掉的那一发**不算失败**：重起安装器，已经下好的库它自己会认。
    if (installerProcess->property("wtfmlStallRestartPending").toBool()) {
        installerProcess->setProperty("wtfmlStallRestartPending", false);
        if (installerTimer != nullptr) {
            installerTimer->stop();
        }
        onInstallerOutput();  // 被杀之前最后那点输出也读干净
        deferLoaderStep(LoaderInstall::Phase::RunningInstaller, [this] { runInstaller(); });
        return;
    }
    if (installerTimer != nullptr) {
        installerTimer->stop();
    }
    onInstallerOutput();  // 收尾前把剩下的输出读干净，失败报告才有东西可报

    const QString name = m_loaderInstall.loaderName;
    const QString version = m_loaderInstall.loaderVersion;

    // 一些旧 Forge 安装器遇到过期 Java 证书仍返回 0；只有没有成功产物时才恢复。
    // 仅更换本次安装器 JVM，不修改全局/实例游戏 Java，不关闭 TLS 校验，最多恢复一次。
    const bool badCertificate = InstallerFailure::certificateFailure(m_installerOutput);
    const bool missingProfile = !QFileInfo::exists(InstanceFiles::jsonPath(m_loaderInstall.loaderId));
    if (!crashed && badCertificate && (exitCode != 0 || missingProfile)) {
        QString replacement, reason;
        if (!installerProcess->property("wtfmlCertificateRetried").toBool() &&
            GlobalJavaSettings::installerJavaFor(17, m_installerProgram, &replacement, &reason) &&
            !replacement.isEmpty() && replacement.compare(m_installerProgram, Qt::CaseInsensitive) != 0) {
            installerProcess->setProperty("wtfmlCertificateRetried", true);
            m_loaderInstall.javaPath = replacement;
            emit loaderInstallMessage(tr("安装器使用的 Java 无法验证 HTTPS 证书，正在改用本机较新的 Java 重试：%1。游戏仍使用原来配置的 Java，证书校验保持开启。")
                                          .arg(replacement));
            deferLoaderStep(LoaderInstall::Phase::RunningInstaller, [this] { runInstaller(); });
            return;
        }
        failLoaderInstall(tr("%1 %2 安装失败：安装器使用的 Java 无法验证 HTTPS 证书。请更新该 Java 的证书库，或为安装器选择证书正常的较新 Java；不要关闭 HTTPS 证书校验。\n当前安装器 Java：%3\n%4")
                              .arg(name, version, m_installerProgram, installerOutputTail(m_installerOutput)));
        return;
    }

    if (crashed) {
        failLoaderInstall(tr("%1 %2 的安装器被强行结束了（不是它自己退出的）。\n它最后打出来的是：\n%3")
                              .arg(name, version, installerOutputTail(m_installerOutput)));
        return;
    }
    if (exitCode != 0) {
        failLoaderInstall(tr("%1 %2 安装失败，安装器退出码为 %3。\n安装器的最后一条输出：\n%4")
                              .arg(name, version)
                              .arg(exitCode)
                              .arg(installerOutputTail(m_installerOutput)));
        return;
    }

    // OptiFine 的版本目录**叫什么名字我们算不出来**（安装器按自己那份 jar 里的版本号拼的），
    // 所以它那一套是"先去临时目录里认、核对、搬回来"，出口和下面这段一样：成功就接着补
    // 原版，失败就收尾。核对仍然做，只是搬到了 adoptOptifineProducts() 里 —— 而且那一步
    // 做到"核对通过"之前，用户的游戏目录里一个字节都不会多。
    if (m_loaderInstall.kind == LoaderKind::OptiFine) {
        adoptOptifineProducts();
        return;
    }

    // 退出码 0 **还不算**装好：核对它真的把那份 json 放在我们算出来的目录里、而且那份
    // json 认的父版本就是我们打算装的那个。Forge 哪天改了目录名约定的话，这两条里至少
    // 有一条会亮 —— 报出来的是"装是装了、但不在预期的地方"，而不是静默把 wtfml.meta
    // 写到一个不存在的 id 上。
    const QString id = m_loaderInstall.loaderId;
    const QJsonObject produced = readVersionJsonAt(InstanceFiles::jsonPath(id));
    if (produced.isEmpty()) {
        failLoaderInstall(tr("%1 %2 的安装器跑完了（退出码 0），但 %3 里没有出现 %4，无法定位安装目录。\n它最后打出来的是：\n%5")
                              .arg(name, version, AppPaths::versionDir(id),
                                   id + QStringLiteral(".json"),
                                   installerOutputTail(m_installerOutput)));
        return;
    }
    const LoaderMeta::ProfileCheck check =
        LoaderMeta::checkInstallerResult(produced, m_loaderInstall.gameVersion, id);
    if (!check.ok()) {
        if (check.problem == LoaderMeta::ProfileProblem::BadId) {
            failLoaderInstall(tr("%1 %2 生成的版本 ID 为「%3」，预期为「%4」，未登记为已安装。")
                                  .arg(name, version, check.id, id));
            return;
        }
        failLoaderInstall(tr("%1 %2 装出来的版本是基于「%3」的，不是我们选的「%4」，没有记成装好。")
                              .arg(name, version, check.parent, m_loaderInstall.gameVersion));
        return;
    }

    emit loaderInstallMessage(tr("%1 %2 安装完成（%3）。").arg(name, version, id));
    startParentCompletion();
}

void VersionManager::startParentCompletion() {
    // 安装器**不下资源文件**（assets）：它自己那份 json 是给启动器读的，资源是启动器的事。
    // 少了那一半，用户拿到的是一个能装模组、但一进游戏就崩的版本。
    //
    // Forge 还会连原版的 json 都不写（只在 versions/<原版>/ 留下一份 sha1 与官方一致的
    // 客户端 jar），NeoForge 会留 json + jar 两份。所以这里**不看原版装没装**，一律走
    // 一遍 downloadVersion()：它写 json、校验客户端 jar 与库（已有就跳过，不是重下）、
    // 再把资源下全。NeoForge 那种"已经齐了"的情况下它也只是校验 + 补资源。
    const QString parent = m_loaderInstall.gameVersion;
    m_loaderInstall.phase = LoaderInstall::Phase::WaitingParentJson;
    emit loaderInstallMessage(tr("%1 已安装，正在补齐原版 %2 的资源文件…")
                                  .arg(m_loaderInstall.loaderName, parent));
    fetchVersionDetails(parent);
}

// ---------------------------------------------------------------------------
// OptiFine：跑它自己的安装器（无界面入口 + 换 APPDATA + 对着临时目录装）
// ---------------------------------------------------------------------------
//
// * 它的安装器**不认命令行给的目录**（参数整个丢掉，只从 APPDATA 算）→ 换子进程的
// APPDATA、让它对着一个临时目录装，装完把产物搬回真游戏目录。真游戏目录从头到尾没被
// 它碰过 —— 这比"用目录联接把假目录指过去"稳得多：那一条路要在用户的真目录上做删除，
// 错一步就是把人家的 .minecraft 删了。
// * 它**要拿原版的客户端 jar 当输入** → 所以是"先补原版、再跑安装器"（Forge/NeoForge
// 正好相反：先跑安装器、最后才补原版）。
// * 它的版本目录名**我们算不出来**（安装器按自己那份 jar 里的版本号拼的）→ 装完去盘上认。

void VersionManager::fetchOptifineVersionList() {
    // 两个源是**两份不同的数据**：镜像是一条 JSON 接口（一个游戏版本一页），官方只有那张
    // 203 KB 的下载页可读（每个游戏版本一节）。连"哪一条是正式版"的挑法都不一样，所以
    // 回复也不共用处理，各走各的（见 LoaderMeta.hpp 里那两个纯函数）。
    //
    // NOTE: 走哪条源在**发请求这里决定一次**，然后原样传进回复处理函数，那边**不再重新读**。
    // 若两边各读一次 useMirror()，这中间用户去设置页换一下「下载源」（安装卡着不动
    // 时他多半会去换），回复回来就会**按另一条源的格式去解** —— HTML 当 JSON 解，
    // 报出来是"版本表读不懂"；JSON 当下载页解，报出来是"还没有支持这个版本的正式版"
    // （而它明明有，只是我们根本没看）。两句话都在指向错误的方向。
    // 问的是 mirrorPreferred() 而不是 useMirror()：镜像被降级（太慢）那段时间里，
    // 这张表也不该再去问镜像 —— 和 Forge 安装器那条 `--mirror` 是同一个判据。
    const bool fromMirror = downloadManager->mirrorPreferred();
    const QUrl url(fromMirror ? LoaderMeta::optifineMirrorListUrl(m_loaderInstall.gameVersion)
                              : LoaderMeta::optifineDownloadsPageUrl());
    QNetworkRequest request(url);
    RequestBudget::ControlGet::start(this, loaderNetworkManager(), request, 30000,
        [this, fromMirror](QNetworkReply *reply, const QString &jobError) {
            onOptifineVersionListReply(reply, fromMirror, jobError);
        });
}

void VersionManager::onOptifineVersionListReply(QNetworkReply *reply, bool fromMirror,
                                                const QString &jobError) {
    if (reply != nullptr) {
        reply->deleteLater();
    }
    // 状态已经换过（上一次失败收了尾、或者用户又点了一次）：这份回复没人要了。
    if (m_loaderInstall.phase != LoaderInstall::Phase::FetchingInstallerList) {
        return;
    }

    const QByteArray body = reply != nullptr && reply->isOpen() ? reply->readAll() : QByteArray();
    if (reply == nullptr || reply->error() != QNetworkReply::NoError) {
        failLoaderInstall(tr("拿不到 %1 %2 的版本表（%3）。检查网络后重试。")
                              .arg(m_loaderInstall.loaderName, m_loaderInstall.gameVersion,
                                   replyReason(reply, jobError)));
        return;
    }

    LoaderMeta::OptiFinePick pick;
    if (fromMirror) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (!doc.isArray()) {
            failLoaderInstall(tr("OptiFine 的版本表无法解析（%1）。").arg(parseError.errorString()));
            return;
        }
        pick = LoaderMeta::pickOptifineFromMirror(doc.array(), m_loaderInstall.gameVersion,
                                                  m_loaderInstall.channel);
    } else {
        pick = LoaderMeta::pickOptifineFromDownloadsPage(QString::fromUtf8(body),
                                                         m_loaderInstall.gameVersion,
                                                         m_loaderInstall.channel);
    }

    if (!pick.ok()) {
        // 和另外三条一样：这不是网络问题，也**不能**退回"装原版"。NOTE: 到了这一步就只剩
        // "**稳定档**上一条正式版都没有"这一种可能（预览版在「最新版」那一档也会列、也装，
        // 所以官方页那一节里只有预览版的情况在那一档是能装下去的）—— 那句提示得把
        // 退路指出来，不能说"预览版不装"。
        // NOTE: 这句话里**不引界面那两行的原文** —— 数据层不认识 ui/MainWindowUI.hpp
        // （那一层依赖是单向的），引一次就等于为了五个字把两层拧在一起。
        failLoaderInstall(
            tr("%1 没有支持 %2 的稳定版。可在列表中选择包含预览版的最新版本。")
                .arg(m_loaderInstall.loaderName, m_loaderInstall.gameVersion));
        return;
    }

    // "这个版本叫什么"只有 LoaderMeta::optifineEditionFromFile() 那一处切法，官方/镜像两条路
    // 都得是同一个值（那是要写进 wtfml.meta 的东西）。
    const QString edition =
        LoaderMeta::optifineEditionFromFile(m_loaderInstall.gameVersion, pick.fileName);

    // 挑中预览版时 `stable` 那一栏是 false（见 LoaderMeta::isOptifinePreviewFile），
    // 于是这句说明会写成"稳定那一档……退回了最新的那个"—— 那正是官方页上那一节只有预览版
    // 的情况，是真话。两个源共用这一句，只有"哪一条"那半句分源（两份数据的挑法不一样）。
    emit loaderInstallMessage(
        tr("挑中 %1 %2（%3）。")
            .arg(m_loaderInstall.loaderName, edition,
                 m_loaderInstall.channel == LoaderMeta::LoaderChannel::Newest
                     ? (LoaderMeta::isOptifinePreviewFile(pick.fileName)
                            ? tr("所选最新版本为预览版")
                            : tr("最新版本（含预览版）"))
                     : pickChannelDetail(m_loaderInstall.channel,
                                         !LoaderMeta::isOptifinePreviewFile(pick.fileName))));

    // 镜像那份数据里带着权威的 type/patch，直接读到的就用它（**不从版本号切**：那两段的切法
    // 和文件名不一样，多一处自己算的就多一处能算错的地方）。用户钉死的版本没有这两个字段，
    // 那时才由版本号切 —— 见下面那个函数的参数说明。
    adoptOptifineEdition(edition, LoaderMeta::OptiFineMirrorPath{pick.type, pick.patch},
                         fromMirror);
}

/**
 * @brief OptiFine：版本定了之后的收尾（填版本号/文件名，然后按下载源决定下一步）。
 * @param edition 版本号（`HD_U_H9`，就是写进 wtfml.meta 的 loaderVersion）。
 * @param mirrorPath 镜像那条路的两个路径段。**镜像列表里直接读到的**就传进来（那是权威的）；
 * 用户钉死的版本没有这两个字段，由 optifineMirrorPathFromEdition() 从版本号切出来 ——
 * 那个切法对着那 11 条真实镜像数据逐条核过，和直接读到的完全一致。
 * @param fromMirror 这一次走的是镜像那条路吗。**由调用方决定好传进来，这里不重新读**
 * `downloadManager->mirrorPreferred()` —— 版本表已经按某一条源取回来了，
 * "接着按哪条源走"必须和取表时是同一个答案（重现读的话，用户在两次之间换一下
 * 「下载源」，就会出现"表是按官方页取的、文件却按镜像地址拼"这种半截组合）。
 *
 * 两条路（版本表挑出来的 / 用户钉死的）共用这一段的理由：**挑法和钉死的差别只在"版本号从
 * 哪来"**，后面填文件名、拼地址、走镜像还是广告页，一个字都不该不一样。两条路都从这里
 * 进，才不会出现"钉死"那条路没有入口、选了 H7 却装成 H9 的情况。
 *
 * NOTE: 版本目录名（= loaderId，最终 finished 信号里的 id）这里**故意不填** —— 它是安装器
 * 自己拼的，我们算不出来。装完认出来再填（见 adoptOptifineProducts），在那之前要报
 * id 的地方走 reportId() 的退化值（原版号）。
 */
void VersionManager::adoptOptifineEdition(const QString &edition,
                                         const LoaderMeta::OptiFineMirrorPath &mirrorPath,
                                         bool fromMirror) {
    m_loaderInstall.loaderVersion = edition;
    m_loaderInstall.installerFileName =
        LoaderMeta::optifineInstallerFileName(m_loaderInstall.gameVersion, edition);

    if (fromMirror) {
        if (!mirrorPath.ok()) {
            failLoaderInstall(tr("找不到 %1 版本 %2 的下载地址。请选择其他版本。")
                                  .arg(m_loaderInstall.loaderName, edition));
            return;
        }
        // 镜像那条路的地址最后两段是 type/patch（`/optifine/1.18.2/HD_U/H9`），302 到
        // maven 上那个真文件。所以 installerFileName 不能用它来切，上面先填好了。
        m_loaderInstall.installerUrl = LoaderMeta::optifineMirrorFileUrl(
            m_loaderInstall.gameVersion, mirrorPath.type, mirrorPath.patch);
        deferLoaderStep(LoaderInstall::Phase::FetchingInstallerList,
                        [this] { ensureParentForOptifine(); });
        return;
    }

    // 官方那条路还差一步：真文件在广告页后面，先把那条带 token 的地址抠出来。
    m_loaderInstall.phase = LoaderInstall::Phase::FetchingInstallerToken;
    emit loaderInstallMessage(tr("正在获取 %1 %2 下载地址…")
                                  .arg(m_loaderInstall.loaderName, edition));
    fetchOptifineDownloadToken();
}

void VersionManager::fetchOptifineDownloadToken() {
    const QUrl url(LoaderMeta::optifineAdPageUrl(m_loaderInstall.installerFileName));
    QNetworkRequest request(url);
    RequestBudget::ControlGet::start(this, loaderNetworkManager(), request, 30000,
        [this](QNetworkReply *reply, const QString &jobError) {
            onOptifineDownloadTokenReply(reply, jobError);
        });
}

void VersionManager::onOptifineDownloadTokenReply(QNetworkReply *reply, const QString &jobError) {
    if (reply != nullptr) {
        reply->deleteLater();
    }
    if (m_loaderInstall.phase != LoaderInstall::Phase::FetchingInstallerToken) {
        return;
    }

    const QByteArray body = reply != nullptr && reply->isOpen() ? reply->readAll() : QByteArray();
    if (reply == nullptr || reply->error() != QNetworkReply::NoError) {
        failLoaderInstall(tr("拿不到 %1 %2 的下载地址（%3）。检查网络后重试。")
                              .arg(m_loaderInstall.loaderName, m_loaderInstall.loaderVersion,
                                   replyReason(reply, jobError)));
        return;
    }

    const QString url = LoaderMeta::optifineJarUrlFromAdPage(QString::fromUtf8(body),
                                                             m_loaderInstall.installerFileName);
    if (url.isEmpty()) {
        // 抠不出来就**不猜**：宁可报"拿不到下载地址"，也不给用户下一个不知道是什么的文件。
        failLoaderInstall(tr("%1 的广告页上没认出 %2 那条下载地址，没有安装。过一会儿重试；若持续失败，请尝试在设置中切换下载源。")
                              .arg(m_loaderInstall.loaderName, m_loaderInstall.installerFileName));
        return;
    }

    m_loaderInstall.installerUrl = url;
    deferLoaderStep(LoaderInstall::Phase::FetchingInstallerToken,
                    [this] { ensureParentForOptifine(); });
}

void VersionManager::ensureParentForOptifine() {
    // 判据和 startParentOrLoaderFiles() 完全一样（版本 json + 客户端 jar 两份都在盘上），
    // 但**理由和方向都不一样**：那边是"原版在就直接下加载器自己的文件"，这边是"原版不在就
    // 先补 —— 安装器要拿那份客户端 jar 当输入，它不在的话安装器起不来"。
    const QString parent = m_loaderInstall.gameVersion;
    if (QFile::exists(InstanceFiles::jsonPath(parent)) &&
        QFile::exists(InstanceFiles::jarPath(parent))) {
        emit loaderInstallMessage(tr("原版 %1 已安装，直接运行 %2 的安装器。")
                                      .arg(parent, m_loaderInstall.loaderName));
        startInstallerDownload();
        return;
    }

    emit loaderInstallMessage(tr("%1 安装器需要原版 %2 的客户端 JAR，正在安装原版…")
                                  .arg(m_loaderInstall.loaderName, parent));
    m_loaderInstall.phase = LoaderInstall::Phase::WaitingParentJson;
    fetchVersionDetails(parent);
}

QString VersionManager::optifineWorkRoot() const {
    return optifineWorkDir != nullptr ? optifineWorkDir->path() : QString();
}

QString VersionManager::optifineWorkMinecraftDir() const {
    return optifineWorkRoot() + QStringLiteral("/.minecraft");
}

void VersionManager::cleanupOptifineWorkDir() {
    if (optifineWorkDir == nullptr) {
        return;
    }
    // 递归删。那底下全是我们自己刚建的东西（见 .hpp 里那个成员的 NOTE），所以这是安全的
    // —— 换来的正是"真游戏目录一个字节都不删"。
    //
    // NOTE: 删不掉（比如子进程还占着文件）也不报错、不当失败：最坏的结果是系统临时目录里
    // 多留一份几 MB 的假 .minecraft，不值得为这个去打扰用户。
    if (!optifineWorkDir->remove()) {
        qWarning() << "Could not remove the OptiFine work dir" << optifineWorkDir->path();
    }
    delete optifineWorkDir;
    optifineWorkDir = nullptr;
}

bool VersionManager::prepareOptifineWorkDir(QString *errorOut) {
    // 先清一次：正常路径下不该有残留（两条终态都会清），这里是保险 —— 万一将来某条新加的
    // 失败路径漏了清，下一次安装不该装进一个脏目录里。
    cleanupOptifineWorkDir();

    optifineWorkDir =
        new QTemporaryDir(QDir::tempPath() + QStringLiteral("/wtfml-optifine-XXXXXX"));
    if (!optifineWorkDir->isValid()) {
        if (errorOut != nullptr) {
            *errorOut = tr("无法创建临时工作目录（%1）。%2 安装器需要可用的 APPDATA 路径。")
                            .arg(optifineWorkDir->errorString(), m_loaderInstall.loaderName);
        }
        return false;
    }

    // 搬运安装器要的两样**输入**：原版的版本 json 和客户端 jar。它把那份 jar 原样抄进新
    // 版本目录当"这个版本的客户端 jar"（不往里写补丁），所以它得先在盘上、还得叫那个名字
    // —— 安装器是按 `versions/<原版>/<原版>.jar` 这个约定去找的。
    const QString parent = m_loaderInstall.gameVersion;
    const QString destDir = optifineWorkMinecraftDir() + QStringLiteral("/versions/") + parent;
    if (!QDir().mkpath(destDir)) {
        if (errorOut != nullptr) {
            *errorOut = tr("无法创建临时版本目录 versions/%1（%2）。").arg(parent, destDir);
        }
        return false;
    }

    const QString jsonFrom = InstanceFiles::jsonPath(parent);
    const QString jarFrom = InstanceFiles::jarPath(parent);
    if (!QFile::exists(jsonFrom) || !QFile::exists(jarFrom)) {
        if (errorOut != nullptr) {
            *errorOut = tr("原版 %1 的文件不全（%2 / %3 这两份要拷进临时目录给安装器用），没有安装。")
                            .arg(parent, jsonFrom, jarFrom);
        }
        return false;
    }
    if (!QFile::copy(jsonFrom, destDir + QLatin1Char('/') + parent + QStringLiteral(".json")) ||
        !QFile::copy(jarFrom, destDir + QLatin1Char('/') + parent + QStringLiteral(".jar"))) {
        if (errorOut != nullptr) {
            *errorOut = tr("把原版 %1 的版本 json / 客户端 jar 拷进临时目录时失败了，没有安装。")
                            .arg(parent);
        }
        return false;
    }
    return true;
}

void VersionManager::adoptOptifineProducts() {
    const QString name = m_loaderInstall.loaderName;
    const QString version = m_loaderInstall.loaderVersion;
    const QString prefix = m_loaderInstall.gameVersion + QStringLiteral("-OptiFine_");

    // 1. 认它装到哪去了。那个临时目录是全新的，我们只往里放了一份**原版**，所以
    // `versions/` 底下除了原版之外多出来的**那一个**目录就是它装的。多了、少了都不猜
    // —— 猜错就是把 wtfml.meta 写到一个不存在的 id 上。
    const QString versionsRoot = optifineWorkMinecraftDir() + QStringLiteral("/versions");
    QString id;
    const QFileInfoList entries =
        QDir(versionsRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (entry.fileName() == m_loaderInstall.gameVersion) {
            continue;  // 我们自己放进去的原版
        }
        if (!id.isEmpty()) {
            failLoaderInstall(tr("%1 安装器在临时目录生成多个版本（%2、%3），无法确定安装目标。\n最后输出：\n%4")
                                  .arg(name, id, entry.fileName(),
                                       installerOutputTail(m_installerOutput)));
            return;
        }
        id = entry.fileName();
    }
    if (id.isEmpty()) {
        failLoaderInstall(tr("%1 %2 的安装器已结束（退出码 0），但临时目录中未新增版本目录，无法定位安装目录。\n安装器输出：\n%3")
                              .arg(name, version, installerOutputTail(m_installerOutput)));
        return;
    }

    // 2. 名字得能当目录名（它来自安装器，我们拿它拼路径），而且得是 `<原版>-OptiFine_<版本>`
    // 这个形状 —— 那是它自己拼目录名的约定，对不上就说明装的不是我们要的那个原版。
    if (!LoaderMeta::isSafeVersionId(id) || !id.startsWith(prefix)) {
        failLoaderInstall(tr("%1 %2 的安装器生成了「%3」，不符合「%4…」的命名格式，未将其识别为已安装版本。")
                              .arg(name, version, id, prefix));
        return;
    }

    // 3. 核对那份 json。**从临时目录读**（不经过内存缓存 —— 走缓存就等于拿一份跟这次安装
    // 无关的东西去核对）。NOTE: checkInstallerResult() 的 id 那一半在这里是**退化的**
    // （id 就是我们刚从盘上认出来的那个，必然相等），真正在查的是 inheritsFrom 是不是
    // 我们选的那个原版、以及 id 能当目录名 —— 上面那两步已经把这两条查过了。
    const QJsonObject produced = readVersionJsonAt(optifineWorkMinecraftDir() +
                                                   QStringLiteral("/versions/") + id +
                                                   QLatin1Char('/') + id +
                                                   QStringLiteral(".json"));
    if (produced.isEmpty()) {
        failLoaderInstall(tr("%1 %2 的安装器装出了 %3 这个目录，但里面没有 %4.json，没有安装。")
                              .arg(name, version, id, id));
        return;
    }
    const LoaderMeta::ProfileCheck check =
        LoaderMeta::checkInstallerResult(produced, m_loaderInstall.gameVersion, id);
    if (!check.ok()) {
        if (check.problem == LoaderMeta::ProfileProblem::BadId) {
            failLoaderInstall(tr("%1 %2 生成的版本 ID 为「%3」，与目录名「%4」不一致，安装已取消。")
                                  .arg(name, version, check.id, id));
            return;
        }
        failLoaderInstall(tr("%1 %2 装出来的版本是基于「%3」的，不是我们选的「%4」，没有安装。")
                              .arg(name, version, check.parent, m_loaderInstall.gameVersion));
        return;
    }

    // 4. 搬回真游戏目录。**先核对、后搬**：上面任何一条不过，用户的游戏目录里一个字节都
    // 不会多出来 —— 这正是"对着临时目录装"换来的东西。
    // NOTE: 那句话管的是**核对**。搬回**中途**失败是另一回事：先搬成的那一半会留着、
    // 不自动回滚（下面两条失败文案各自说清了留下的是什么），而两样东西都是 maven
    // 式带版本号的子目录（`libraries/optifine/OptiFine/<版本>/…`），所以留下的
    // 那半份不会影响别的 OptiFine 版本。
    //
    // 两样都要搬，而且**先搬库、后搬版本目录**：
    // * 那份 json 里的两个库既没有 `url` 也没有 `downloads`，只能从本机那份
    // `libraries/` 里满足。只搬版本目录会得到一个启动就 NoClassDefFound 的版本。
    // * 顺序是刻意的：版本目录一出现，这个版本在启动器的版本列表里就"存在"了。库先搬
    // 的话，中途失败留下的是一份谁都看不见的库；反过来留下的会是一个半成品版本。
    const QString tmpLibs = optifineWorkMinecraftDir() + QStringLiteral("/libraries/optifine");
    if (!QDir(tmpLibs).exists()) {
        failLoaderInstall(tr("%1 %2 装出来的版本目录里少了它的库（临时目录里没有 %3），这样的版本启动时会报找不到类，没有安装。")
                              .arg(name, version, tmpLibs));
        return;
    }
    if (!copyTreeInto(tmpLibs, AppPaths::libraryDir() + QStringLiteral("/optifine"))) {
        failLoaderInstall(tr("无法将 %1 %2 的库（libraries/optifine/）移至游戏目录（%3），安装已取消。")
                              .arg(name, version, AppPaths::libraryDir() + QStringLiteral("/optifine")));
        return;
    }
    if (!copyTreeInto(QDir(versionsRoot).absoluteFilePath(id), AppPaths::versionDir(id))) {
        failLoaderInstall(tr("%1 %2 的库已复制，但版本目录无法移至 %3。请重新安装以补齐文件。")
                              .arg(name, version, AppPaths::versionDir(id)));
        return;
    }

    // 5. 版本号以**盘上那个目录名**为准：目录名是安装器拼的（我们算不出来），而 wtfml.meta
    // 里记的东西必须和真实装出来的那个版本一致。挑版本时报的那个（从安装器文件名切的）
    // 正常情况下和它一样。
    m_loaderInstall.loaderId = id;
    m_loaderInstall.loaderVersion = id.mid(prefix.size());

    emit loaderInstallMessage(
        tr("%1 %2 安装完成（%3）。").arg(name, m_loaderInstall.loaderVersion, id));
    startParentCompletion();
}

void VersionManager::finishLoaderInstall(bool success, const QString &message) {
    // 那份临时工作目录（OptiFine 那条路才有）到这里就没人要了：产物已经搬回真游戏目录，
    // 剩下的只是一份几 MB 的假 .minecraft。成功和失败两条终态都清，不留垃圾。
    cleanupOptifineWorkDir();

    // 先把要用的字段取出来：reset() 会把它们清掉，忘了取的表现是消息里那个名字变空的
    // （不崩，但话就说不清了）。
    const QString loaderName = m_loaderInstall.loaderName;
    const QString loaderVersion = m_loaderInstall.loaderVersion;
    const QString loaderId = m_loaderInstall.reportId();

    if (success) {
        writeLoaderMeta();
        // 装出去的这份 json 是**新的**：在这之前读到的任何缓存（合并结果那张表尤其）
        // 都是旧的，留着会让"刚装好的版本"用回装之前的样子启动。
        versionJsons.remove(m_loaderInstall.loaderId);
        mergedVersionJsons.remove(m_loaderInstall.loaderId);
    }

    // 这一次安装是**整合包导入的第一步**（不是用户单独要装加载器）：这里不发终态信号 ——
    // 发了界面会先弹一句"Fabric 已就绪"、按钮中途解禁，而导入那边还有上百个模组要下。
    // 成功就接着走导入的下一步，失败就把整件事按失败收掉（那句原因正是加载器这边给的）。
    if (m_modpackInstall.active() &&
        m_modpackInstall.phase == ModpackInstall::Phase::InstallingLoader) {
        m_loaderInstall.reset();
        if (!success) {
            failModpackImport(tr("%1 安装失败，整合包未导入。%2").arg(loaderName, message));
            return;
        }
        m_modpackInstall.loaderId = loaderId;
        emit modpackImportMessage(tr("%1 %2 已安装，正在写入实例文件…")
                                      .arg(loaderName, loaderVersion));
        // 这一步是从"加载器那批文件下完"的信号栈里回来的（allDownloadsFinished 一路同步
        // 到这儿）：排到下一轮，让它从干净栈上开始。
        deferNextStep([this] { startModpackFiles(); });
        return;
    }

    // 这一次加载器安装是"装一个**带名字的实例**"的第一步：加载器装完不算完，还得写实例
    // 自己那份薄 json，而终态信号报的该是**用户起的那个名字**（他等的是那个）。
    //
    // NOTE: 这一支和上面整合包那一支是**互斥**的（两条入口都会在对方 active 时拒绝开局），
    // 所以谁先谁后不影响行为；放在后面只是因为整合包那条路多一步。
    if (m_namedInstance.active() && m_namedInstance.phase == NamedInstance::Phase::WaitingLoader) {
        m_loaderInstall.reset();
        if (!success) {
            failNamedInstance(tr("实例安装失败：%1 %2 未能安装。%3")
                                  .arg(loaderName, loaderVersion, message));
            return;
        }
        // 父版本到这一刻才知道是谁（加载器那个目录名是安装器自己拼的）。
        m_namedInstance.parentId = loaderId;
        finishNamedInstance(loaderId, tr("%1 %2 已就绪。%3").arg(loaderName, loaderVersion, message));
        return;
    }

    // 收尾：既结束这次安装，也结束这次点击 —— 它是**唯一**的终态。
    m_loaderInstall.reset();
    emit versionDownloadFinished(
        loaderId, success,
        success ? tr("%1 %2 已就绪。%3").arg(loaderName, loaderVersion, message)
                : tr("%1 %2 安装失败。%3").arg(loaderName, loaderVersion, message));
}

void VersionManager::writeLoaderMeta() {
    QJsonObject fields;
    fields[QStringLiteral("realVersion")] = m_loaderInstall.gameVersion;
    fields[QStringLiteral("loaderName")] = m_loaderInstall.loaderName;
    fields[QStringLiteral("loaderVersion")] = m_loaderInstall.loaderVersion;

    QString error;
    if (!updateVersionMeta(m_loaderInstall.loaderId, fields, &error)) {
        // NOTE: 版本本身已经装好了、能启动 —— 写不进 meta 只是"显示名少一段"，
        // 所以这不是失败，不该把它算进失败里。但也不能不说（用户会发现名字不对）。
        qWarning() << "Failed to write wtfml.meta for" << m_loaderInstall.loaderId << ":" << error;
        emit loaderInstallMessage(tr("安装已完成，但无法保存 wtfml.meta（%1）。显示的版本名称将缺少「%2 %3」。")
                                      .arg(error, m_loaderInstall.loaderName,
                                           m_loaderInstall.loaderVersion));
    }
}

void VersionManager::failLoaderInstall(const QString &reason) {
    // 同上：不管是哪一步失败的，那个临时目录都该消失。
    cleanupOptifineWorkDir();

    // 那份 profile 还在内存缓存里、磁盘上却没有对应的目录：留着的话 getVersionJson()
    // 会给出一个"磁盘上并不存在"的合并结果（它先查缓存）。清掉它。
    if (!m_loaderInstall.loaderId.isEmpty()) {
        versionJsons.remove(m_loaderInstall.loaderId);
    }
    const QString reportId = m_loaderInstall.reportId();
    qWarning() << "Loader install failed for" << reportId << ":" << reason;

    // 这一次加载器安装是**整合包导入的第一步**（不是用户单独要装加载器）：整件事到这里
    // 就是失败了，收尾只能走 failModpackImport() —— 它会把加载器那半截一起收掉，而且发的
    // 是**实例名**（用户等的是那个名字）。
    //
    // NOTE: 这一支**必须**和 finishLoaderInstall 那边一样收掉导入状态，后果不是"少一句话"
    // 而是**永久卡死**：m_modpackInstall 会一直停在 InstallingLoader 相位上，而它正是
    // "谁不能开局"的判据 —— importModpack() / downloadResourceFiles() /
    // installNamedVanilla() / installNamedLoader() 四道闸全看它。用户此后导入不了
    // 整合包、装不了实例、下不了模组，只能重启；同时这一路发出去的终态信号带的是
    // **加载器目录名**，而 MainWindow 那边只认实例名，那个实例于是永远点不动。
    // 加载器安装失败的理由很多（安装器退出码非 0、产物核对不过、版本表拉不到、
    // 找不到 Java、安装器下不下来…），任何一条都会踩到这里。
    if (m_modpackInstall.active() &&
        m_modpackInstall.phase == ModpackInstall::Phase::InstallingLoader) {
        // loaderName 必须在 failModpackImport() 之前取（它会 reset 掉加载器那份状态）。
        const QString loaderName = m_loaderInstall.loaderName;
        failModpackImport(tr("未能安装 %1，整合包导入失败。%2").arg(loaderName, reason));
        return;
    }

    // 这一次加载器安装是"装一个带名字的实例"的第一步：终态信号报**用户起的那个名字**
    // （他等的是那个），而且必须把那个状态一起收掉 —— 不收的话 isNamedInstanceInstalling()
    // 永远是 true，界面就一直卡在忙态上、之后每次安装都被"已有任务正在进行"顶回来。
    //
    // NOTE: loaderName 必须在 reset() **之前**取出来（reset 会把它清掉，症状是那句话里
    // 加载器的名字变成空的 —— 同 finishLoaderInstall 开头那段）。
    if (m_namedInstance.active() && m_namedInstance.phase == NamedInstance::Phase::WaitingLoader) {
        const QString instanceId = m_namedInstance.instanceId;
        const QString instanceLoaderName = m_loaderInstall.loaderName;
        m_namedInstance.reset();
        m_loaderInstall.reset();
        emit versionDownloadFinished(
            instanceId, false,
            tr("实例「%1」安装失败：%2 未能安装。%3").arg(instanceId, instanceLoaderName, reason));
        return;
    }

    m_loaderInstall.reset();
    emit versionDownloadFinished(reportId, false, reason);
}

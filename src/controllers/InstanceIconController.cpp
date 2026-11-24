#include "InstanceIconController.hpp"
#include "InstanceSettingsDialog.hpp"
#include "InstanceFilesPage.hpp"
#include "download/ResourceStore.hpp"
#include "manage/InstanceFiles.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"
#include "util/ZipUtil.hpp"
#include "util/ImageDecode.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <QTimer>

namespace {

/// 这个实例那几个目录的**签名**（键值对排序拼出来）。用来回答"这一页认的是哪个实例" ——
/// 光比 versionId 不够，见 `InstanceIconRun::dirsSignature`。
QString fileDirsSignature(const QHash<QString, QString> &dirs) {
    if (dirs.isEmpty()) {
        return QString();
    }
    QStringList parts;
    parts.reserve(dirs.size());
    for (auto it = dirs.constBegin(); it != dirs.constEnd(); ++it) {
        parts.append(it.key() + QLatin1Char('=') + it.value());
    }
    parts.sort();
    return parts.join(QLatin1Char('\n'));
}

}  // namespace

InstanceIconController::InstanceIconController(LauncherSettings *settings, ResourceStore *resourceStore,
                                               Host host, QObject *parent)
    : QObject(parent), m_host(std::move(host)), m_settings(settings), m_resourceStore(resourceStore) {}

QString InstanceIconController::instanceIconCacheKey(const QString &path) {
    // 路径 + 大小 + 修改时间。**三样都要**（理由见声明处那条 NOTE）：只按路径键住的话，
    // "删掉一个模组、往同一个名字上放另一个"这一形态会让那一格一直画着上一个资源的图标。
    // 分隔符用 `\n`（一个文件名里不可能有它，而且它不会和路径里那些 `|` 打架）。
    //
    // **路径那一段先把「禁用」那四个字摘掉**（`InstanceFilesPage::
    // identityPath()`，同一个规矩的唯一出处）—— 用户报的「模组禁用以后图标没了」
    // 就是这条。禁用 = 把 `X.jar` 改名成 `X.jar.disabled`（见
    // `InstanceFilesPage::disabledSuffix()`）：**改的是名字，盘上那份内容一个字节都没动**
    // （大小和修改时间也一样 —— 改名不动这两个），所以"这一条是哪个资源"在改名前后
    // 就是同一件事。不摘的话这一把钥匙整个换掉：`m_instanceFileIcons` 那一份里明明有它
    // 也认不出来，于是要么白重读一遍那几十兆（重算哈希）、要么干脆没有 —— 而屏幕上
    // 两种都表现为那一格退回占位拼图。同一把钥匙还锁着 `m_fileResources`
    // （详情那一页的来路）和 `m_instanceIconMissing`（"问过了、没有"那条负缓存）。
    //
    // NOTE: 摘的只是**钥匙**。两张表里存的那些路径（`m_instanceIcons.paths` /
    // `Found::path`）照旧是**盘上真那个名字** —— 推给界面时用的正是它。
    // NOTE: 大小和修改时间是**拿盘上真那个路径**取的（上面那一句 `QFileInfo`），
    // 和钥匙上那一段无关。
    const QFileInfo info(path);
    return InstanceFilesPage::identityPath(path) + QLatin1Char('\n') +
           QString::number(info.size()) + QLatin1Char('\n') +
           QString::number(info.lastModified().toMSecsSinceEpoch());
}

QStringList InstanceIconController::instanceIconCandidatePaths(const QString &versionId) const {
    const QHash<QString, QString> dirs = m_host.fileDirs(versionId);
    // 五个目录：四个能下载的（模组 / 资源包 / 数据包 / 光影包）+ 原理图。**不含存档**
    // （那一页全是目录，而一个存档不是一个能反查的资源）。原理图那一页大多是
    // `.litematic`（`isLookupCandidate()` 会把它们筛掉），但用户往里放 `.zip` 也是常事。
    const QStringList keys = {QStringLiteral("mod"), QStringLiteral("resourcepack"),
                              QStringLiteral("datapack"), QStringLiteral("shader"),
                              QStringLiteral("schematic")};
    QStringList paths;
    for (const QString &key : keys) {
        const QDir dir(dirs.value(key));
        if (!dir.exists()) {
            continue;
        }
        const QFileInfoList entries =
            dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &info : entries) {
            // 只收 `.jar` / `.zip`（禁用着的那些也算，见 isLookupCandidate）。
            if (InstanceIconLookup::isLookupCandidate(info.absoluteFilePath())) {
                paths.append(info.absoluteFilePath());
            }
        }
    }
    return paths;
}

void InstanceIconController::requestInstanceFileIcons(InstanceSettingsDialog *dialog, const QString &versionId) {
    if (dialog == nullptr) {
        return;
    }
    // 这一页现在是谁 —— 结果回来时按它认人（见 InstanceIconRun::dialog 那段）。
    m_instanceIcons.dialog = dialog;
    m_instanceIcons.versionId = versionId;
    m_instanceIcons.dirsSignature = fileDirsSignature(m_host.fileDirs(versionId));
    m_instanceIcons.paths = instanceIconCandidatePaths(versionId);
    // 已经知道的先摆上：这一趟要问的那几个还在路上，但上一趟问到的、以及这一趟里
    // "已经问过、没有"的那些，现在就该是对的（不是等整个后台线程跑完才一起摆）。
    pushInstanceFileIcons();

    startInstanceFileIconLookup();
}

void InstanceIconController::startInstanceFileIconLookup() {
    // 已经有一趟在跑就**到此为止**：同一个实例的目录列表前后差不了多少，重开一趟等于
    // 把已经在读的那几十兆重读一遍、把已经在问的那几发重问一遍。
    // NOTE: 这一条早退**不再丢下新开的那一页** —— 那一趟收尾时会拿"这一趟是给谁问的"
    // 和"现在这一页是谁"比一比，不一样就回头替新那页再走一遍这里（见
    // onInstanceIconLookupFinished 末尾那段）。
    if (m_instanceIcons.active) {
        return;
    }

    // 还没问过的那些：会话缓存里两样都没有的。
    QStringList missing;
    for (const QString &path : m_instanceIcons.paths) {
        const QString key = instanceIconCacheKey(path);
        if (m_instanceFileIcons.contains(key) || m_instanceIconMissing.contains(key)) {
            continue;
        }
        missing.append(path);
    }
    if (missing.isEmpty()) {
        // 一个都不缺（多半是刚关掉又打开、而且上一趟已经把这一批问完了）。
        return;
    }

    InstanceIconLookup::Options options;
    options.curseForgeApiKey = m_settings->curseForgeApiKey();
    options.modrinthApiBaseOverride = m_settings->modrinthApiBaseOverride();
    options.curseForgeApiBaseOverride = m_settings->curseForgeApiBaseOverride();
    // （要求：「模组图标做本地缓存功能，不要每次都拉取」）：这一趟的结果
    // 留一份在这个实例名下（见 InstanceIconLookup 的 .hpp 里【结果留一份在盘上】那段）——
    // 第二次进这一页就**只读那一份 JSON**，几十兆 jar 不重读、接口一个都不问。
    // 改这个实例里某一个文件（甚至只是把它禁用掉）只让**那一条**重查：钥匙里带着大小和
    // 修改时间，而"禁用"只是改了个名。
    options.cacheDir = AppPaths::instanceIconCacheDir();
    options.cacheKey = m_instanceIcons.versionId;

    // 上一趟那个线程对象还挂着（跑完了，但 deletion 还排着队）：只把指针放掉 —— 它自己接好了
    // finished → deleteLater（见下面创建处）。
    m_instanceIconThread = nullptr;

    m_instanceIcons.active = true;
    // **这一趟问的是哪几个文件**记一份，回调里只认这一份（理由见
    // m_instanceIconRunPaths 声明处那段）。上面那道 `active` 闸排在"换 paths"之后，
    // 所以这一句必须放在这里、而不是跟着 paths 一起在最上面写 —— 它要写的是**这一趟**
    // 的事实，不是"最后一次请求"的事实。
    m_instanceIconRunPaths = missing;
    m_instanceIconRunVersionId = m_instanceIcons.versionId;
    m_host.log(tr("正在读取「%1」的资源图标（%2 个文件）…")
                   .arg(m_host.displayName(m_instanceIcons.versionId))
                   .arg(missing.size()));
    // `QThread::create` 那个重载收一个函数对象（Qt 5.10 起），省掉"再写一个 QThread 子类
    // 并在 .cpp 里 moc"。结果**回主线程那一句走 invokeMethod 的 functor 重载**（而不是
    // 一个带自定义类型参数的信号）：那样连元类型都不用登记 —— 那个 functor 是被拷进事件
    // 里带过去的，中间不过 QVariant（`InstanceIconLookup::Result` 里那两层容器要按
    // Q_DECLARE_METATYPE 一层层登记才对，漏一层就是**静默**收不到，见 ServerPing 里那条）。
    m_instanceIconThread = QThread::create([this, missing, options]() {
        // 先发布本地图标，再反查身份；读取和图片解码都留在后台线程。
        for (qsizetype offset = 0; offset < missing.size(); offset += 16) {
            const auto bytes = ZipUtil::readIconsBatch(missing.mid(offset, 16));
            QHash<QString, QImage> images;
            for (auto it = bytes.cbegin(); it != bytes.cend(); ++it) {
                const auto image = ImageDecode::decode(it.value());
                if (!image.isNull()) images.insert(instanceIconCacheKey(it.key()),
                    image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
            QMetaObject::invokeMethod(this, [this, images]() {
                for (auto it = images.cbegin(); it != images.cend(); ++it) {
                    m_embeddedIconKeys.insert(it.key());
                    m_instanceFileIcons.insert(it.key(), QPixmap::fromImage(it.value()));
                }
                pushInstanceFileIcons();
            }, Qt::QueuedConnection);
        }
        const InstanceIconLookup::Result result = InstanceIconLookup::lookup(missing, options);
        QMetaObject::invokeMethod(
            this, [this, result]() { onInstanceIconLookupFinished(result); }, Qt::QueuedConnection);
    });
    m_instanceIconThread->setParent(this);
    // 自己收自己；清指针排在删除之前（waitForLookup() 里那句 wait() 在析构时跑，
    // 指针不清掉就会对一块已经释放的内存调 wait()）。
    connect(m_instanceIconThread, &QThread::finished, this,
            [this, thread = m_instanceIconThread] {
                if (m_instanceIconThread == thread) m_instanceIconThread = nullptr;
            });
    connect(m_instanceIconThread, &QThread::finished, m_instanceIconThread,
            &QObject::deleteLater);
    m_instanceIconThread->start();
}

void InstanceIconController::onInstanceIconLookupFinished(const InstanceIconLookup::Result &result) {
    m_instanceIcons.active = false;
    const QStringList runPaths = m_instanceIconRunPaths;
    const QString runVersionId = m_instanceIconRunVersionId;
    m_instanceIconRunPaths.clear();
    m_instanceIconRunVersionId.clear();

    // ---------- ① 认出来的：换成一个"资源身份"，交给 ResourceStore 去取那张图 ----------
    //
    // 到这里为止拿到的是"这个文件的哈希属于哪个项目"，**图还没有**。取图的活交给
    // ResourceStore（它有一套现成的两级缓存 + 条件请求 + 挑 webp/PNG 那套判断），
    // 这一层只是把身份递给它。
    QSet<QString> foundPaths;
    QList<ResourceCatalog::Hit> iconRequests;
    for (const InstanceIconLookup::Found &found : result.found) {
        const ResourceCatalog::Hit hit = found.hit;
        // 这一条文件属于哪一格（`<来源>-<项目号>`）—— 图标回来时按它找回**行**（见
        // rememberResourceIcon）。同一个资源被反查出来两次（同一个 jar 摆了两份）时
        // 两条路径都记上：两张图本来就该是同一张。
        const QString iconKey = hit.iconCacheId();
        QStringList &paths = m_instanceIcons.pathsByIconKey[iconKey];
        if (!paths.contains(found.path)) {
            paths.append(found.path);
        }
        foundPaths.insert(found.path);
        // 这一条是哪个资源（会话缓存，按"路径 + 大小 + 修改时间"记 —— 同一个名字上换了
        // 一个文件就自然作废，见 instanceIconCacheKey）。整份 Hit 存进 m_fileResources：
        // 点名字开那一页走的就是**下载页点一行**那条路（openResourceDetailPage 按这把
        // 钥匙取标题 / 简介 / slug / 游戏版本；两处的分别只有"来路"那一位，见 DetailSource），
        // 而同一份 Hit 也交给主窗口那一份（下载页那份表才是那一条路的出处）。
        m_fileResources.insert(instanceIconCacheKey(found.path), hit);
        m_host.rememberHit(iconKey, hit);
        // 已经在问 / 已经在缓存里的话 requestIcon() 自己会处理（可能**同步**就把 iconReady
        // 发出来了 —— 那时候上面那一行必须先写完，见它那条 NOTE）。
        if (!m_embeddedIconKeys.contains(instanceIconCacheKey(found.path))) iconRequests.append(hit);
    }

    // ---------- ② 没认出来的那一批 ----------
    //
    // 判据是 `result.problem`：只有**整趟完整跑完**了才把"没有"记下来。断网那一趟
    // 要是也记，用户这一整个会话都再也看不到图标了 —— 而那本来是"网好了就会有"
    // （见 m_instanceIconMissing 那段）。problem 非空时还有一个后果：①里那些**认出来了
    // 的**照旧记账（那是真的），只是不再问第二遍。
    // 即使命中磁盘缓存，取图也包含解码与同步回调；按小批次让出事件循环，
    // 不让整包模组的图标在页面刚打开时占满一个长事件。
    if (!iconRequests.isEmpty()) {
        auto *batch = new QTimer(this);
        batch->setInterval(4);
        connect(batch, &QTimer::timeout, this,
                [batch = QPointer<QTimer>(batch), store = QPointer<ResourceStore>(m_resourceStore),
                 requests = std::move(iconRequests), next = qsizetype(0)]() mutable {
            if (!batch) return;
            QElapsedTimer budget;
            budget.start();
            while (store && next < requests.size()) {
                store->requestIcon(requests.at(next++));
                if (!batch) return;
                if (budget.elapsed() >= 6) break;
            }
            if (!store || next == requests.size()) {
                batch->stop();
                batch->deleteLater();
            }
        });
        batch->start();
    }

    const bool complete = result.problem.isEmpty();
    if (!complete) {
        // 只记一句日志、**不弹窗**：图标取不到是常态（自己改过的 jar、作者没传过的、
        // 断网的），画个占位拼图就是正确的界面 —— 它挡不住用户做任何事。
        m_host.log(tr("资源图标没查完：%1").arg(result.problem));
    }
    for (const QString &path : runPaths) {
        if (foundPaths.contains(path)) {
            continue;
        }
        if (complete) {
            m_instanceIconMissing.insert(instanceIconCacheKey(path));
        }
        // 记不上也要摆：这一条现在还是一张占位图（见 InstanceFilesPage::iconForPath），
        // 而"这一条问过了、没有"这件事在这一页活着的时候得记住 —— 不然用户点一下
        // 「刷新」，那一格又变回"还没问"的样子。
    }
    // 图标地址那一发没成 —— **和上面那句分开说**：那一刻身份都已经认出来了，缺的只是图，
    // 而那几格退回占位拼图就是正确的界面（不弹窗，理由同上）。这句话得说，是因为"认出来了
    // 但没图"和"压根没认出来"在屏幕上是一个样子，而这两件事的下一步不一样。
    if (!result.iconProblem.isEmpty()) {
        m_host.log(tr("有几个资源的图标地址没取到：%1").arg(result.iconProblem));
    }
    if (result.unreadable > 0) {
        m_host.log(tr("无法读取 %1 个文件，已跳过这些文件的图标。")
                       .arg(result.unreadable));
    }
    pushInstanceFileIcons();
    // 为什么非补不可：换页那一发请求撞在 `active` 闸上直接 return 了（见
    // startInstanceFileIconLookup 那条早退），而这里刚从回来的这一趟是**上一页**的 ——
    // 没有人替新那页问的话，它的图标要等用户再进一次文件管理页才出现。
    // 判据看的是**趟**（runVersionId）而不是 `m_instanceIcons.paths` 变没变：同一个实例
    // 反复进出这一页不该重跑一趟（那正是那条 early-return 想省的）。
    // NOTE: 这一页已经关掉了（dialog 是 QPointer，关了就是空）就什么都不做 —— 没人看。
    // NOTE: 不会再套一层循环：补跑那一趟的 runVersionId 就是**当时**的 versionId，它收尾时
    // 这一条判据必然相等（同一页期间路径表也不会自己变）。
    if (m_instanceIcons.dialog != nullptr && m_instanceIcons.versionId != runVersionId) {
        startInstanceFileIconLookup();
    }
}

void InstanceIconController::pushInstanceFileIcons(const QStringList &paths) {
    // 磁盘/内存缓存会同步连发 iconReady；每张图都重推整张列表会退化为平方级刷新。
    // 同一轮事件只合并刷新一次，并在执行时读取当前页面，避免旧页面结果回填。
    for (const auto &path : paths.isEmpty() ? m_instanceIcons.paths : paths)
        m_pendingFileIconPaths.insert(path);
    if (m_fileIconPushPending) return;
    m_fileIconPushPending = true;
    QTimer::singleShot(0, this, [this] {
        flushInstanceFileIcons();
    });
}

void InstanceIconController::flushInstanceFileIcons() {
    QPointer<InstanceSettingsDialog> dialog = m_instanceIcons.dialog;
    if (dialog == nullptr) {
        m_pendingFileIconPaths.clear();
        m_fileIconPushPending = false;
        return;
    }
    QElapsedTimer budget;
    budget.start();
    while (!m_pendingFileIconPaths.isEmpty()) {
        const QString path = *m_pendingFileIconPaths.constBegin();
        m_pendingFileIconPaths.remove(path);
        if (!m_instanceIcons.paths.contains(path)) continue;
        const QString key = instanceIconCacheKey(path);
        const auto cached = m_instanceFileIcons.constFind(key);
        if (cached != m_instanceFileIcons.constEnd()) {
            dialog->setFileIcon(path, cached.value());
        } else if (m_instanceIconMissing.contains(key)) {
            // 空 pixmap = "问过了、没有" ⇒ 那一格摆占位拼图（见 setIconForPath）。
            dialog->setFileIcon(path, QPixmap());
        }
        // 两样都没有 = 还没问回来 ⇒ **什么都不做**，那一格现在摆的就是占位图。

        // 行上的名字点不点得动：认得出来（`m_fileResources` 里有它、
        // 而且那份 Hit 带着名字）才给那一行带上链接的样子，否则给**空串**。
        // 空串这一支不是"没话说"，而是"把上一趟可能戴上的摘掉"：同一个路径换了内容
        // （大小/时间变了）时键也换了，这一趟的负结果必须覆盖上一趟的正结果，不然
        // 那一行会顶着一个已经不对的资源名字、点进去是另一个东西。
        const auto identity = m_fileResources.constFind(key);
        dialog->setFileResource(path, identity == m_fileResources.constEnd()
                                          ? QString()
                                          : identity.value().title);
        if (budget.elapsed() >= 6) break;
    }
    if (m_pendingFileIconPaths.isEmpty()) {
        m_fileIconPushPending = false;
    } else {
        QTimer::singleShot(4, this, [this] { flushInstanceFileIcons(); });
    }
}

void InstanceIconController::rememberResourceIcon(const QString &iconKey, const QPixmap &icon) {
    const QStringList filePaths = m_instanceIcons.pathsByIconKey.value(iconKey);
    if (filePaths.isEmpty()) {
        // 这一张图标不对应任何一条**盘上躺着的文件**（它可能是下载页那边问回来的）——
        // 这一层一个字都不记。
        return;
    }
    for (const QString &path : filePaths) {
        if (m_embeddedIconKeys.contains(instanceIconCacheKey(path))) continue;
        // 空的也存 —— 它记的是"这一条问过了、没有"（见 m_instanceIconMissing 那段）。
        m_instanceFileIcons.insert(instanceIconCacheKey(path), icon);
    }
    pushInstanceFileIcons(filePaths);
}

QSet<QString> InstanceIconController::recognizedResourceKeysUnder(const QString &dir) const {
    QSet<QString> keys;
    if (dir.isEmpty()) {
        return keys;
    }
    const QString prefix = dir + QLatin1Char('/');
    for (auto it = m_fileResources.constBegin(); it != m_fileResources.constEnd(); ++it) {
        // 键是 `路径\n大小\n时间`（见 instanceIconCacheKey）⇒ 前缀比得上就说明这个文件在
        // 这个目录里。**必须带那个 `/`**：不带的话 `…/mods2/x.jar` 也会算进来。
        if (it.key().startsWith(prefix)) {
            keys.insert(it.value().iconCacheId());
        }
    }
    return keys;
}

bool InstanceIconController::recognizedInstanceIs(const QString &instanceId) const {
    // 说的是"这个实例到底认过没有"，而不是"认出来几个" —— 一个都没认出（新实例、全是
    // 自己改过的 jar）和"没进过文件管理页"都表现为空表，而界面上那两句话不一样。
    if (m_instanceIcons.versionId.isEmpty() || m_instanceIcons.versionId != instanceId) {
        return false;
    }
    return m_instanceIcons.dirsSignature == fileDirsSignature(m_host.fileDirs(instanceId));
}

bool InstanceIconController::fileResourceIconKey(const QString &path, QString *iconKey) const {
    const auto identity = m_fileResources.constFind(instanceIconCacheKey(path));
    if (identity == m_fileResources.constEnd()) {
        return false;
    }
    if (iconKey != nullptr) {
        *iconKey = identity.value().iconCacheId();
    }
    return true;
}

void InstanceIconController::waitForLookup() {
    // NOTE: 这一趟**没有收手旗子**（`InstanceIconLookup::lookup()` 里头没有"看着旗子退出来"
    // 的口子），所以这里只能**等它跑完** —— 卡在超时上的话，关窗就会晚那十几秒才真的
    // 关上（同一个取舍，见 MainWindow 析构里备份那一段）。
    if (m_instanceIconThread != nullptr) {
        m_instanceIconThread->wait();
    }
    // 指针可能已经被放掉了（新的一趟把旧的顶掉，见 requestIcons 那段），
    // 而那条线程还在最后几条指令里。析构 QObject 会把子对象连同一条**仍在跑**的线程
    // 一起删掉 ⇒ qFatal。这里按子对象兜一遍底，保证关窗时它们都已经退出。
    for (QThread *thread : findChildren<QThread *>()) {
        if (thread != m_instanceIconThread) thread->wait();
    }
}

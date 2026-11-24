#include "Support.hpp"

void runIconCacheTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir rootA;
    QTemporaryDir rootB;
    check(rootA.isValid() && rootB.isValid(), "ZB0 两个临时数据根目录建出来了");

    section("ZB1. 缓存路径怎么拼（以及哪些 id 宁可不缓存）");

    AppPaths::setMinecraftDir(rootA.path());
    const QString path = ResourceStore::iconCachePath(QStringLiteral("AANobbMI"));
    check(QFileInfo(path).absolutePath() ==
              QDir::cleanPath(rootA.path() + QStringLiteral("/cache/icons")),
          "ZB1 图标缓存落在 <数据目录>/cache/icons 下",
          QFileInfo(path).absolutePath());
    // 后缀是 `.icon` 不是 `.png`：这一格里两种字节都会落进来（能解 webp 的机器走那条小的，
    // 存的就是 webp）。叫 .png 是句假话，而这个假话会害到下一个按后缀认格式的人。
    check(QFileInfo(path).fileName() == QStringLiteral("AANobbMI.icon"),
          "ZB1 文件名就是 project_id + .icon（里面可能是 png 也可能是 webp）");

    // 这个 id 是从**接口正文**里来的，按外部输入对待：能拼出路径的那些一律不接受。
    // `..` 最要命 —— 它每一个字符都在放行集里，却指向上一层目录。
    const QStringList badIds = {QStringLiteral(".."),  QStringLiteral("."),
                                QStringLiteral("../evil"), QStringLiteral("a/b"),
                                QStringLiteral("a\\b"), QStringLiteral("a:b"),
                                QStringLiteral("a b"),  QStringLiteral("模组"),
                                QString()};
    int rejected = 0;
    for (const QString &id : badIds) {
        if (ResourceStore::iconCachePath(id).isEmpty()) {
            ++rejected;
        }
    }
    check(rejected == badIds.size(), "ZB1 不安全/空的 project_id 一律不拼路径",
          QStringLiteral("%1/%2 被挡下").arg(rejected).arg(badIds.size()));

    section("ZB2. 凭据（ETag / Last-Modified）：存得住、读得回、空的不留文件");

    // 凭据是**跟着一条地址**的（ 补的第三行）。同一个 project 有两条地址
    // （接口那条 96px webp / 自己拼的 icon.png），这条测试全程用同一条，另有一段专门
    // 验"地址对不上 = 当没有凭据"。
    const QString kMetaId = QStringLiteral("EEEfff");
    const QString kMetaUrl = QStringLiteral("https://cdn.modrinth.com/data/EEEfff/icon-96.webp");
    const QString kMetaOtherUrl = QStringLiteral("https://cdn.modrinth.com/data/EEEfff/icon.png");
    check(ResourceStore::iconMetaPath(kMetaId) ==
              ResourceStore::iconCachePath(kMetaId) + QStringLiteral(".meta"),
          "ZB2 凭据就躺在图标那份旁边（同名 + .meta）");
    // 没存过 = 没有凭据。这不是错误，是"下一发老老实实要整张"。
    check(ResourceStore::loadIconValidators(kMetaId, kMetaUrl).isEmpty() &&
              ResourceStore::iconMetaPath(QStringLiteral("../evil")).isEmpty(),
          "ZB2 没存过时读回空的；不安全的 id 连路径都不拼");

    ResourceStore::IconValidators both;
    both.etag = QStringLiteral("\"5f3c1a2b-9\"");
    both.lastModified = QStringLiteral("Tue, 23 Sep 2026 04:11:07 GMT");
    check(ResourceStore::saveIconValidators(kMetaId, both, kMetaUrl), "ZB2 两条凭据都写下去了");
    const ResourceStore::IconValidators back =
        ResourceStore::loadIconValidators(kMetaId, kMetaUrl);
    // **逐字**比对：这两条是要原样塞回请求头里的，少一个引号、多一个空格，主机那边就是
    // 另一句话（ETag 是强比对，差一个字节就等于"变了"）。
    check(back.etag == both.etag && back.lastModified == both.lastModified,
          "ZB2 读回来一模一样（引号和逗号原样在）",
          back.etag + QStringLiteral(" / ") + back.lastModified);

    // 只有一条凭据也是常态（有些主机只给 ETag）—— 存下去、读回来也得对得上：
    // 少写的另一条读出来是空的，不能让上一条的残值串到这一条上。
    ResourceStore::IconValidators onlyEtag;
    onlyEtag.etag = QStringLiteral("\"only\"");
    check(ResourceStore::saveIconValidators(kMetaId, onlyEtag, kMetaUrl) &&
              ResourceStore::loadIconValidators(kMetaId, kMetaUrl).etag == onlyEtag.etag &&
              ResourceStore::loadIconValidators(kMetaId, kMetaUrl).lastModified.isEmpty(),
          "ZB2 只存 ETag 时另一条读回来是空的");

    // 两条都空：**照样写一份，不删文件**
    // （那份 meta 还装着"上一次核对是什么时候"，见 IconValidators::checkedAtMs ——
    // 主机不给凭据的那些源照样该被那道时间闸管着，删了文件这一位就没了）。
    //
    // 判据因此收成两条：① 保存成功；② **读回来还是"没有凭据"**（这一点一个字都不能变 ——
    // 空的 ETag 发出去就是一个空条件头）。
    check(ResourceStore::saveIconValidators(kMetaId, ResourceStore::IconValidators(), kMetaUrl),
          "ZB2 两条都空时保存 = 成功");
    {
        const ResourceStore::IconValidators empty =
            ResourceStore::loadIconValidators(kMetaId, kMetaUrl);
        check(empty.etag.isEmpty() && empty.lastModified.isEmpty(),
              "ZB2 ……而且读回来还是「没有凭据」（一个空条件头都不许发出去）");
        // 第四行那一位得**真的写下去了**（它是那道「攒够 7 天才核一次」的闸的判据）——
        // 不写的话，主机不给凭据的那些图标每次启动都要白敲一遍门。
        check(empty.checkedAtMs > 0, "ZB2 ……并且盖上了「刚核过」那个时间戳（第四行）",
              QStringLiteral("%1").arg(empty.checkedAtMs));
    }

    // 一份**空**的 meta（0 字节）= 没有凭据、一个条件头都不发。这是"两条都空不留文件"
    // 那条规矩的另一半 —— 盘上真躺着一个空文件时（旧版本留的、或者被别的东西写坏了），
    // 读出来必须是"没有"，不能变成"凭据是一条空串"（空串会变成一个语法不成形的条件头）。
    writeFileAt(ResourceStore::iconMetaPath(kMetaId), QByteArray());
    const ResourceStore::IconValidators emptyFile =
        ResourceStore::loadIconValidators(kMetaId, kMetaUrl);
    check(emptyFile.isEmpty() &&
              QString::fromUtf8(ResourceStore::iconRequest(kMetaId, emptyFile)
                                    .rawHeader("If-None-Match"))
                  .isEmpty(),
          "ZB2 空文件 = 没有凭据（不会发出一个空的条件头）");

    // 正文读不懂（被别的东西写过、写了一半）也**不崩**：这个格式就是两行纯文本，
    // 第一行是什么就当 ETag 是什么。一条对不上的凭据是无害的 —— 主机不认它，照旧把整张
    // 发回来，下面那条"字节一样 = 没变"照样接得住。真正危险的是**拿别人的凭据去问**
    // （那一份字节已经不在了、凭据还在），那条由 dropIconFromCache 一起删来保证，
    // 断言在 ZB4 里。
    writeFileAt(ResourceStore::iconMetaPath(kMetaId), QByteArray("这不是凭据\n"));
    const ResourceStore::IconValidators garbage =
        ResourceStore::loadIconValidators(kMetaId, kMetaUrl);
    check(garbage.etag == QStringLiteral("这不是凭据") && garbage.lastModified.isEmpty(),
          "ZB2 读不懂的正文：第一行当 ETag、其余当没有（不崩、不凭空多一条）", garbage.etag);

    section("ZB2b. 凭据跟的是**哪条地址**");

    // 缓存和凭据都只按 project_id 存，而同一个 project 有**两条**图标地址（接口那条 96px
    // 的 webp、自己拼的 icon.png，见 requestIcon）。不记"这条凭据是哪条地址给的"，换一条
    // 地址时就会**拿着 A 的 ETag 去问 B**：主机不认时白带一个条件头（还得整张重下），
    // 万一两条地址的 ETag 撞上就是一条 304 —— 这一格从此画着"另一条地址的那张图"，
    // 而且看起来一个错都没有。
    check(ResourceStore::saveIconValidators(kMetaId, both, kMetaUrl),
          "ZB2b 凭据跟着地址一起存下去");
    check(ResourceStore::loadIconValidators(kMetaId, kMetaUrl).etag == both.etag,
          "ZB2b 同一条地址读得回来");
    check(ResourceStore::loadIconValidators(kMetaId, kMetaOtherUrl).isEmpty(),
          "ZB2b 换了一条地址 = 当没有凭据（不拿 A 的 ETag 去问 B）");
    // 存下去那两行本身的形状也得看一眼：**必须**有地址那一条 —— 少了它上面那条断言
    // 之所以过只是因为"没记地址"，而不是因为"记下了 B"。这两件事差着一个 304。
    {
        QFile metaFile(ResourceStore::iconMetaPath(kMetaId));
        const bool opened = metaFile.open(QIODevice::ReadOnly);
        const QList<QByteArray> metaLines =
            opened ? metaFile.readAll().split('\n') : QList<QByteArray>();
        if (opened) {
            metaFile.close();
        }
        check(metaLines.size() > 2 &&
                  QString::fromUtf8(metaLines.at(2)).trimmed() == kMetaUrl,
              "ZB2b 第三行真的写着这条地址（不是「没记」，是「记的是它」）");
    }

    // 旧版 meta 只有两行 —— 第三行读出来是空的。那种是
    // **"没记地址"**，不是"记的是别的地址"：照样得能用。少写一行就把用户的凭据整份作废
    // （代价是所有人下次启动都把整本图标重下一遍）。
    writeFileAt(ResourceStore::iconMetaPath(kMetaId),
                QByteArray("\"old-etag\"\nTue, 23 Sep 2026 04:11:07 GMT\n"));
    const ResourceStore::IconValidators legacy = ResourceStore::loadIconValidators(kMetaId, kMetaUrl);
    check(legacy.etag == QStringLiteral("\"old-etag\"") &&
              legacy.lastModified == QStringLiteral("Tue, 23 Sep 2026 04:11:07 GMT"),
          "ZB2b 两行的老 meta（没记地址）照样用，不因为缺第三行就作废", legacy.etag);

    section("ZB3. 那一发请求到底带什么头（哪条凭据发哪个头）");

    // 这个函数是纯的（造一个 QNetworkRequest 再把头发读回来），所以"哪条凭据发哪个头"
    // 这张真值表可以在这儿逐条钉住 —— 而它是这次改动里最容易写错、错了又不报错的地方。
    const QString kUrl = QStringLiteral("https://cdn.modrinth.com/data/AANobbMI/icon.png");
    const auto headerOf = [](const QNetworkRequest &request, const char *name) {
        return QString::fromUtf8(request.rawHeader(name));
    };

    const QNetworkRequest plainRequest = ResourceStore::iconRequest(kUrl, ResourceStore::IconValidators());
    check(headerOf(plainRequest, "If-None-Match").isEmpty() &&
              headerOf(plainRequest, "If-Modified-Since").isEmpty(),
          "ZB3 没有凭据 = 一个条件头都不发（空头不是\"没有凭据\"那句话）");
    // 地址和身份这两条跟凭据无关，任何时候都得在。
    check(plainRequest.url().toString() == kUrl && !headerOf(plainRequest, "User-Agent").isEmpty(),
          "ZB3 地址原样没动、身份头照发");
    // 图标那一发**比别处短**（它是装饰，"半小时不出来"和"这一页被拖住"是两回事）——
    // 不写死 8000，钉的是那条**关系**：比默认那个短、而且是个真的数。
    check(plainRequest.transferTimeout() > 0 &&
              plainRequest.transferTimeout() < HttpClient::kDefaultTimeoutMs,
          "ZB3 图标那一发的超时比默认那个短",
          QStringLiteral("%1 ms").arg(plainRequest.transferTimeout()));

    ResourceStore::IconValidators etagOnly;
    etagOnly.etag = QStringLiteral("\"abc123\"");
    const QNetworkRequest etagRequest = ResourceStore::iconRequest(kUrl, etagOnly);
    check(headerOf(etagRequest, "If-None-Match") == etagOnly.etag &&
              headerOf(etagRequest, "If-Modified-Since").isEmpty(),
          "ZB3 只有 ETag 时只发 If-None-Match", headerOf(etagRequest, "If-None-Match"));

    ResourceStore::IconValidators timeOnly;
    timeOnly.lastModified = QStringLiteral("Tue, 23 Sep 2026 04:11:07 GMT");
    const QNetworkRequest timeRequest = ResourceStore::iconRequest(kUrl, timeOnly);
    check(headerOf(timeRequest, "If-Modified-Since") == timeOnly.lastModified &&
              headerOf(timeRequest, "If-None-Match").isEmpty(),
          "ZB3 只有 Last-Modified 时只发 If-Modified-Since");

    const QNetworkRequest bothRequest = ResourceStore::iconRequest(kUrl, both);
    check(headerOf(bothRequest, "If-None-Match") == both.etag &&
              headerOf(bothRequest, "If-Modified-Since") == both.lastModified,
          "ZB3 两条都有 = 两条都发，值一字不改");

    section("ZB3b. 问项目名字那一发的头（CurseForge 那条 400 的真因）");
    // 所以这一节钉两件事：**那一发的头表**（纯函数，一毫秒）+ **端到端跑一遍**（本地假
    // 服务器：不带那个头就回 400，带了才回 200 和一份真形状的 json —— 这一条才是"下次谁
    // 再把那个参数删了"能当场变红的那条）。
    {
        // 这一节自己带一份（`kUrl` / `headerOf` 是 ZB3 那个块里的局部量，出了那个花括号
        // 就不在了；两处同名不同块，不冲突）。
        const QString kUrl = QStringLiteral("https://api.curseforge.com/v1/mods");
        const auto headerOf = [](const QNetworkRequest &request, const char *name) {
            return QString::fromUtf8(request.rawHeader(name));
        };

        const QNetworkRequest withType = ResourceStore::resourceRequest(
            kUrl, {}, QByteArrayLiteral("application/json"));
        check(headerOf(withType, "Content-Type") == QStringLiteral("application/json"),
              "ZB3b 给了类型就一字不差地发出去", headerOf(withType, "Content-Type"));
        check(headerOf(withType, "User-Agent") == QStringLiteral("WTFML"),
              "ZB3b User-Agent 还是那一份");

        // 空 = "这一发没有正文"，于是**一个 Content-Type 都不加**。那个空头不是"没有类型"，
        // 它是一句不成形的话（有些网关会照着它回 400）——所以这里两种写法都不许有。
        const QNetworkRequest withoutType = ResourceStore::resourceRequest(kUrl);
        check(!withoutType.hasRawHeader("Content-Type") &&
                  headerOf(withoutType, "Content-Type").isEmpty(),
              "ZB3b 没给类型时一个 Content-Type 都不发（空的那个也不行）");

        // 自定义头（CurseForge 的 `x-api-key` 就走这一路）原样落上去。
        const QList<QPair<QByteArray, QByteArray>> extraHeaders = {
            {QByteArrayLiteral("x-api-key"), QByteArrayLiteral("$2a$10$pretend")}};
        const QNetworkRequest withExtra =
            ResourceStore::resourceRequest(kUrl, extraHeaders, QByteArrayLiteral("application/json"));
        check(headerOf(withExtra, "x-api-key") == QStringLiteral("$2a$10$pretend"),
              "ZB3b 自定义头原样带上");

        // 超时走 Qt 自己那条（不再另搭 QTimer），值是 HttpClient 那一份 —— 两处必须一样，
        // 不然"这一发到底几秒算超时"就有两个答案。
        check(withType.transferTimeout() == HttpClient::kDefaultTimeoutMs,
              "ZB3b transferTimeout 就是 HttpClient 那一份",
              QString::number(withType.transferTimeout()));
    }
    {
        // --- 端到端：**不带 `Content-Type` 就回 400** 的假 CurseForge ---
        //
        // 判据刻意做成"服务端真的照那条规矩判"：假服务器把收到的头原样记下来，缺那个头就
        // 答 400（真镜像就是这么干的，实测过）。于是这一条用例红了只有两个可能：要么产品
        // 那一发没带头、要么那台接口的规矩变了 —— 两种都该有人看一眼。
        struct FakeCurseForge {
            QTcpServer server;
            int requests = 0;
            QString lastContentType;
            bool sawContentType = false;
            QString lastPath;

            bool start() {
                QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
                    while (QTcpSocket *socket = server.nextPendingConnection()) {
                        socket->setParent(&server);
                        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                            if (socket->property("answered").toBool()) {
                                return;
                            }
                            socket->setProperty("answered", true);
                            // 正文一起读掉再答（不读的话那条连接上还挂着未读数据，
                            // 有些平台上写完就断了）。
                            const QByteArray request = socket->readAll();
                            ++requests;
                            const int headerEnd = request.indexOf("\r\n\r\n");
                            const QByteArray head = headerEnd > 0 ? request.left(headerEnd) : request;
                            lastContentType.clear();
                            sawContentType = false;
                            const QList<QByteArray> lines = head.split('\n');
                            for (const QByteArray &line : lines) {
                                const QByteArray trimmed = line.trimmed();
                                const int colon = trimmed.indexOf(':');
                                if (colon <= 0) {
                                    continue;
                                }
                                const QByteArray name = trimmed.left(colon).trimmed().toLower();
                                if (name == "content-type") {
                                    sawContentType = true;
                                    lastContentType =
                                        QString::fromUtf8(trimmed.mid(colon + 1)).trimmed();
                                }
                            }
                            if (!lines.isEmpty()) {
                                lastPath = QString::fromUtf8(lines.first().trimmed());
                            }
                            const bool accepted =
                                lastContentType.compare(QStringLiteral("application/json"),
                                                        Qt::CaseInsensitive) == 0;
                            const QByteArray body =
                                accepted
                                    ? QByteArrayLiteral(
                                          "{\"data\":[{\"id\":238222,\"slug\":\"jei\","
                                          "\"name\":\"Just Enough Items\","
                                          "\"summary\":\"看配方的\",\"downloadCount\":1,"
                                          "\"authors\":[{\"name\":\"mezz\"}],"
                                          "\"categories\":[],\"latestFilesIndexes\":[]}]}")
                                    : QByteArrayLiteral("{\"error\":\"Content-Type 不对\"}");
                            QByteArray response =
                                QByteArrayLiteral("HTTP/1.1 ") +
                                (accepted ? QByteArrayLiteral("200 OK\r\n")
                                          : QByteArrayLiteral("400 Bad Request\r\n"));
                            response += QByteArrayLiteral(
                                "Content-Type: application/json\r\nContent-Length: ");
                            response += QByteArray::number(body.size());
                            response += QByteArrayLiteral("\r\n\r\n");
                            response += body;
                            socket->write(response);
                            socket->flush();
                        });
                    }
                });
                return server.listen(QHostAddress::LocalHost, 0);
            }
        };

        FakeCurseForge fake;
        const bool listening = fake.start();
        check(listening, "ZB3b 本地假 CurseForge 起来了（起不来这条用例就没意义）");
        if (listening) {
            ResourceStore store;
            store.setCurseForgeApiBaseOverride(
                QStringLiteral("http://127.0.0.1:%1/v1").arg(fake.server.serverPort()));

            QEventLoop loop;
            QList<ResourceCatalog::Hit> hits;
            QString problem;
            bool answered = false;
            QObject::connect(&store, &ResourceStore::projectsFinished, &loop,
                             [&](const QString &, const QList<ResourceCatalog::Hit> &result,
                                 const QString &resultProblem) {
                                 hits = result;
                                 problem = resultProblem;
                                 answered = true;
                                 loop.quit();
                             });
            store.fetchProjects(ResourceCatalog::projectsRequestKey(QStringList{QStringLiteral("238222")},
                                                                   ResourceCatalog::Source::CurseForge),
                                QStringList{QStringLiteral("238222")},
                                ResourceCatalog::Source::CurseForge);
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);  // 卡住时别把整套挂在这儿
            loop.exec();

            check(fake.requests == 1, "ZB3b 那一发真的发出去了",
                  QString::number(fake.requests));
            check(fake.sawContentType &&
                      fake.lastContentType.compare(QStringLiteral("application/json"),
                                                   Qt::CaseInsensitive) == 0,
                  "ZB3b 那一发带着 application/json（缺了它假服务器回 400）",
                  fake.lastContentType);
            check(problem.isEmpty(),
                  "ZB3b 那一发没被拒（problem 空 = 没走到「没取到」那一路）", problem);
            check(answered && hits.size() == 1 && hits.first().title == QStringLiteral("Just Enough Items"),
                  "ZB3b 名字真的读回来了（**不是**退成项目号）",
                  hits.isEmpty() ? QStringLiteral("(一条都没有)") : hits.first().title);
            check(hits.size() == 1 &&
                      hits.first().projectId == QStringLiteral("238222"),
                  "ZB3b 回来的那条挂着的还是那个项目号",
                  hits.isEmpty() ? QStringLiteral("(一条都没有)") : hits.first().projectId);
        }
    }

    section("ZB4. 存了能不能读回来（含读到坏字节那种）");

    const QByteArray bytes = QByteArrayLiteral("\x89PNG\r\n\x1a\n-not-really-a-png");
    check(ResourceStore::saveIconToCache(QStringLiteral("AAAbbb"), bytes),
          "ZB4 写进盘上去了");
    check(QFile::exists(ResourceStore::iconCachePath(QStringLiteral("AAAbbb"))),
          "ZB4 文件真的在");

    QByteArray readBack;
    check(ResourceStore::loadIconFromCache(QStringLiteral("AAAbbb"), &readBack),
          "ZB4 读得回来");
    check(readBack == bytes, "ZB4 字节一模一样");

    // 没缓存过的那条：回 false，而且**不许动**调用方手上那个值（它可能就是靠"没动"
    // 来判断"这次没有"的）。
    readBack = QByteArrayLiteral("别动我");
    check(!ResourceStore::loadIconFromCache(QStringLiteral("没缓存过"), &readBack) &&
              readBack == QByteArrayLiteral("别动我"),
          "ZB4 没缓存过时回 false，且没动传进去的值");

    // 坏字节：缓存层**不看内容**（那是 ImageDecode / 界面那一侧的事），所以它照样回 true……
    check(ResourceStore::loadIconFromCache(QStringLiteral("AAAbbb"), &readBack),
          "ZB4 坏字节也照读（缓存层不看内容）");
    // ……而"读出来发现不是图"之后走的是删掉重来那条路。
    //
    // 这里顺手放上一条**凭据**：drop 必须把那份 meta 一起删掉。光删图不删凭据的话，
    // 下一次会拿着一条"对应着另一份字节"的 ETag 去问，主机答 304，于是这一格永远画不出
    // 东西来 —— 而且**看起来一个错都没有**（那是这次改动里最像样的一个坑）。
    check(ResourceStore::saveIconValidators(QStringLiteral("AAAbbb"), both, kUrl),
          "ZB4 给这份图标配一条凭据");
    ResourceStore::dropIconFromCache(QStringLiteral("AAAbbb"));
    check(!ResourceStore::loadIconFromCache(QStringLiteral("AAAbbb"), &readBack) &&
              !QFile::exists(ResourceStore::iconCachePath(QStringLiteral("AAAbbb"))),
          "ZB4 删掉之后读不到了、盘上也没了");
    check(!QFile::exists(ResourceStore::iconMetaPath(QStringLiteral("AAAbbb"))) &&
              ResourceStore::loadIconValidators(QStringLiteral("AAAbbb"), kUrl).isEmpty(),
          "ZB4 ……凭据跟着一起删（不删的话下次会问出一条 304 把这一格焊死）");

    // 盘上那份**存了多久都不算过期** —— 这正是用户 那条规矩。没有配套的断言
    // 的话，"哪天有人又加回一条时间线"是查不出来的（界面上只是"图标忽然又下了一遍"）。
    // 判据只能这么写：把文件的修改时间往前拨一大截，读回来照样是它、照样能画。
    check(ResourceStore::saveIconToCache(QStringLiteral("CCCDdd"), bytes), "ZB4 再存一份");
    const QString oldPath = ResourceStore::iconCachePath(QStringLiteral("CCCDdd"));
    {
        QFile backdated(oldPath);
        const bool opened = backdated.open(QIODevice::ReadOnly | QIODevice::WriteOnly);
        check(opened && backdated.setFileTime(QDateTime::currentDateTime().addYears(-5),
                                              QFileDevice::FileModificationTime),
              "ZB4 把那份的修改时间拨到五年前");
        if (opened) {
            backdated.close();
        }
    }
    readBack.clear();
    check(ResourceStore::loadIconFromCache(QStringLiteral("CCCDdd"), &readBack) &&
              readBack == bytes,
          "ZB4 五年前存的那份照样读得出来（缓存不过期）");

    section("ZB5. 缓存跟着「当前游戏文件夹」走");

    check(ResourceStore::saveIconToCache(QStringLiteral("DDDeee"), bytes), "ZB5 在 A 里存一份");
    check(ResourceStore::saveIconValidators(QStringLiteral("DDDeee"), both, kUrl),
          "ZB5 凭据也在 A 里存一份");
    AppPaths::setMinecraftDir(rootB.path());
    check(!ResourceStore::loadIconFromCache(QStringLiteral("DDDeee"), &readBack),
          "ZB5 换到 B 这份数据目录就读不到了（各存一份，不是全局共用）");
    check(ResourceStore::loadIconValidators(QStringLiteral("DDDeee"), kUrl).isEmpty(),
          "ZB5 凭据也跟着走：B 里没有 A 存的那条");
    check(ResourceStore::saveIconToCache(QStringLiteral("DDDeee"), bytes), "ZB5 在 B 里也存一份");
    AppPaths::setMinecraftDir(rootA.path());
    check(ResourceStore::loadIconFromCache(QStringLiteral("DDDeee"), &readBack) &&
              readBack == bytes,
          "ZB5 换回 A 那份还是原来那张");

    section("ZB6. 哪几种失败算「这一路整条不通」");

    // 0 = 压根没连上（断网 / DNS / 超时）—— 这才是那道闸要挡的。
    check(ResourceStore::isHostLevelIconFailure(0), "ZB6 没连上（0）= 主机级");
    check(ResourceStore::isHostLevelIconFailure(500), "ZB6 5xx = 主机级");
    check(ResourceStore::isHostLevelIconFailure(503), "ZB6 503 = 主机级");
    // 404 = 这个资源没有图标；429 = 你问得太勤了 —— 两条都说明**主机是活的**。
    check(!ResourceStore::isHostLevelIconFailure(404),
          "ZB6 404（作者没传图标）不算主机级");
    check(!ResourceStore::isHostLevelIconFailure(HttpClient::kStatusTooManyRequests),
          "ZB6 429（限流）不算主机级");
    check(!ResourceStore::isHostLevelIconFailure(200) &&
              !ResourceStore::isHostLevelIconFailure(302),
          "ZB6 成功/跳转更不算");
    // 304：**"没变"，而且主机明明答话了**。判成主机级失败的后果是它去推那道"放弃"的闸
    // （三次 304 就整页不再问）；而 304 恰恰是"一切正常、就是不用动"那个答案。
    check(!ResourceStore::isHostLevelIconFailure(HttpClient::kStatusNotModified),
          "ZB6 304（没变）不算主机级失败：它说的是一切正常");
    // 那道闸必须"连着"够数才放下来：一两张失败就放弃的话，偶尔丢一张图也会把整页的
    // 图标全掐掉。
    check(ResourceStore::kIconGiveUpAfter >= 2, "ZB6 那道闸要连着失败好几张才放下来",
          QStringLiteral("kIconGiveUpAfter = %1").arg(ResourceStore::kIconGiveUpAfter));

    AppPaths::setMinecraftDir(originalDir); // 收摊：后面那些节读的还是原来那份沙盒
    check(AppPaths::minecraftDir() == originalDir, "ZB7 数据根目录换回来了");
}


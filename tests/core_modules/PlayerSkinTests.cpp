#include "Support.hpp"

#include "net/RequestBudget.hpp"

void runPlayerSkinTests(const QString &sandbox) {
    section("T. 玩家外观（档案解析 + 内容寻址缓存）");

    const QString skinUrl = QStringLiteral("https://textures.minecraft.net/texture/1a2b3c");
    const QString capeUrl = QStringLiteral("https://textures.minecraft.net/texture/4d5e6f");

    // 一份"官方会发回来"的信封。里面那层 base64 **现场生成**，不贴长字面量：贴的话
    // 改一个字段就得重算一遍，而下一个人多半不会重算 —— 于是测试会对着一段过期的
    // base64 断言，一边改着别的、一边还以为都验到了。
    const auto envelope = [](const QString &innerJson) {
        QJsonObject entry;
        entry.insert(QStringLiteral("name"), QStringLiteral("textures"));
        entry.insert(QStringLiteral("value"), QString::fromLatin1(innerJson.toUtf8().toBase64()));
        QJsonObject root;
        root.insert(QStringLiteral("id"), QStringLiteral("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f"));
        root.insert(QStringLiteral("name"), QStringLiteral("Player"));
        root.insert(QStringLiteral("properties"), QJsonArray{entry});
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    };

    // ---- T1 解一份真形状的档案 ----
    {
        const QString inner = QStringLiteral(
                                  R"JSON({"timestamp":1700000000000,)JSON"
                                  R"JSON("profileId":"0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f",)JSON"
                                  R"JSON("profileName":"Player","textures":{)JSON"
                                  R"JSON("SKIN":{"url":"%1","metadata":{"model":"slim"}},)JSON"
                                  R"JSON("CAPE":{"url":"%2"}}})JSON")
                                  .arg(skinUrl, capeUrl);
        QString error;
        const PlayerSkinProfile profile =
            PlayerSkinStore::parseProfileJson(envelope(inner), &error);
        check(profile.skinUrl == skinUrl, "T1 解出 SKIN.url", profile.skinUrl);
        check(profile.capeUrl == capeUrl, "T1 解出 CAPE.url", profile.capeUrl);
        check(profile.variant == QStringLiteral("slim"),
              "T1 metadata.model == slim → 纤细模型", profile.variant);
        check(error.isEmpty(), "T1 解成功时不留下失败原因", error);
        check(!profile.isEmpty(), "T1 有皮肤的档案不是空的");
        check(profile.sameAs(profile), "T1 一份档案和自己逐字段相同");
    }

    // ---- T2 缺件与坏件各是什么下场 ----
    // 「解不出来」和「这个人没有披风」必须分得开：都返回空档案的话，调用方只能靠
    // 猜 —— 而猜错的那种（把一份坏档案当成"没皮肤"）正好会让用户看到默认外观。
    {
        QString error;
        const PlayerSkinProfile noProperties =
            PlayerSkinStore::parseProfileJson(QStringLiteral(R"({"id":"x","name":"Player"})")
                                                  .toUtf8(),
                                              &error);
        check(noProperties.isEmpty() && !error.isEmpty(), "T2 没有 properties → 解不出来且有原因",
              error);

        error.clear();
        QJsonObject entry;
        entry.insert(QStringLiteral("name"), QStringLiteral("textures"));
        // 不是合法 base64（`!` 不在字母表里）
        entry.insert(QStringLiteral("value"), QStringLiteral("!!!!这不是 base64!!!!"));
        QJsonObject root;
        root.insert(QStringLiteral("properties"), QJsonArray{entry});
        check(PlayerSkinStore::parseProfileJson(QJsonDocument(root).toJson(QJsonDocument::Compact),
                                                &error)
                  .isEmpty() &&
                  !error.isEmpty(),
              "T2 base64 坏掉 → 解不出来且有原因", error);

        // 只有 SKIN 没有 CAPE：这是**多数人**的档案（没披风），必须解得出来，
        // 而且 capeUrl 是空串、不是"整个档案作废"。
        const QString skinOnly =
            QStringLiteral(R"JSON({"textures":{"SKIN":{"url":"%1"}}})JSON").arg(skinUrl);
        error.clear();
        const PlayerSkinProfile noCape =
            PlayerSkinStore::parseProfileJson(envelope(skinOnly), &error);
        check(noCape.skinUrl == skinUrl && noCape.capeUrl.isEmpty(),
              "T2 只有 SKIN 没有 CAPE（没披风是常态）→ 照样解得出来，capeUrl 空", error);

        // metadata.model 缺字段 / 是个没见过的词：一律当经典。宁可画粗一点，
        // 也不要因为一个没见过的字符串把手臂画瘦（同 SkinModel::isSlimVariant）。
        const QString noModel =
            QStringLiteral(R"JSON({"textures":{"SKIN":{"url":"%1"}}})JSON").arg(skinUrl);
        check(PlayerSkinStore::parseProfileJson(envelope(noModel), &error).variant ==
                  QStringLiteral("classic"),
              "T2 metadata 缺字段 → 经典模型");
        const QString unknownModel = QStringLiteral(
                                         R"JSON({"textures":{"SKIN":{"url":"%1",)JSON"
                                         R"JSON("metadata":{"model":"wide"}}}})JSON")
                                         .arg(skinUrl);
        check(PlayerSkinStore::parseProfileJson(envelope(unknownModel), &error).variant ==
                  QStringLiteral("classic"),
              "T2 metadata.model 是个没见过的词 → 也当经典");

        // 有 textures 但没有 SKIN.url：每个账号都有皮肤，所以这是协议变了。
        const QString noSkin = QStringLiteral(R"JSON({"textures":{"CAPE":{"url":"%1"}}})JSON")
                                   .arg(capeUrl);
        error.clear();
        check(PlayerSkinStore::parseProfileJson(envelope(noSkin), &error).isEmpty() &&
                  !error.isEmpty(),
              "T2 没有 SKIN.url → 解不出来（不把一份没皮肤的档案发出去）", error);
    }

    // ---- T3 缓存路径是内容寻址的 ----
    {
        const QString first = PlayerSkinStore::cachePathForUrl(skinUrl);
        check(first == PlayerSkinStore::cachePathForUrl(skinUrl),
              "T3 同一个 URL 两次算出同一个路径");
        check(first != PlayerSkinStore::cachePathForUrl(capeUrl), "T3 不同 URL 不同路径");

        const QString name = QFileInfo(first).fileName();
        check(name.size() == 44 && name.endsWith(QStringLiteral(".png")),
              "T3 文件名是 40 位十六进制 + .png（sha1）", name);
        check(name.left(40) ==
                  QString::fromLatin1(
                      QCryptographicHash::hash(skinUrl.toUtf8(), QCryptographicHash::Sha1).toHex()),
              "T3 那 40 位真的是这个 URL 的 sha1（不是序号、不是时间戳）", name.left(40));
        // 缓存在**当前游戏目录**下（沙盒里就是沙盒）：这条同时钉住"它基于 AppPaths，
        // 不会写到 PCL 那份只读的数据目录里去"。
        check(QDir::cleanPath(first).startsWith(QDir::cleanPath(sandbox) + QLatin1Char('/')),
              "T3 缓存落在当前游戏目录下面（换目录各存一份）",
              QDir::toNativeSeparators(first));
    }

    // ---- T4 档案缓存的往返与坏件 ----
    {
        const QString uuid = QStringLiteral("0f0f0f0f-0f0f-0f0f-0f0f-0f0f0f0f0f0f");
        const QString uuidPlain = QStringLiteral("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f");
        check(PlayerSkinStore::uuidKey(uuid) == uuidPlain,
              "T4 带横线的 uuid 归一成 32 位无横线（URL 和文件名共用这一个形式）");
        check(PlayerSkinStore::uuidKey(uuidPlain.toUpper()) == uuidPlain,
              "T4 大写也归一成小写（官方一律小写，大写进去查不到人）");
        check(PlayerSkinStore::uuidKey(QStringLiteral("不是 uuid")).isEmpty(),
              "T4 认不出来 → 空串（不去拼一个野路径）");
        check(PlayerSkinStore::cachedProfilePath(QStringLiteral("不是 uuid")).isEmpty(),
              "T4 uuid 认不出来时档案路径也是空串");
        check(PlayerSkinStore::cachedProfilePath(uuid) ==
                  PlayerSkinStore::cachedProfilePath(uuidPlain),
              "T4 带不带横线的 uuid 指向同一份档案缓存");

        PlayerSkinProfile written;
        written.skinUrl = skinUrl;
        written.capeUrl = capeUrl;
        written.variant = QStringLiteral("slim");
        QString error;
        check(PlayerSkinStore::saveProfile(uuid, written, &error),
              "T4 存一份档案（目录不存在也要自己建出来）", error);

        PlayerSkinProfile loaded;
        check(PlayerSkinStore::loadCachedProfile(uuid, &loaded), "T4 读回来成功");
        check(loaded.sameAs(written), "T4 往返逐字段一致",
              QStringLiteral("%1 / %2 / %3").arg(loaded.skinUrl, loaded.capeUrl, loaded.variant));

        // 坏文件：读不懂 / 版本不认识 / 没有皮肤。三条都必须返回 false **而不是抛**，
        // 而且**不动 out**（调用方手上那份别被一个空值盖掉）。
        const QString path = PlayerSkinStore::cachedProfilePath(uuid);
        PlayerSkinProfile untouched;
        untouched.skinUrl = QStringLiteral("https://example.invalid/keep-me");
        const QList<QPair<QString, QByteArray>> broken = {
            {QStringLiteral("不是 JSON"), QByteArrayLiteral("{ 这不是 json")},
            {QStringLiteral("版本不认识"), QByteArrayLiteral(R"({"version":99,"skin":"x"})")},
            {QStringLiteral("有版本没皮肤"), QByteArrayLiteral(R"({"version":1,"skin":""})")},
        };
        for (const auto &item : broken) {
            writeFileAt(path, item.second);
            untouched.skinUrl = QStringLiteral("https://example.invalid/keep-me");
            check(!PlayerSkinStore::loadCachedProfile(uuid, &untouched),
                  QStringLiteral("T4 档案坏掉（%1）→ 返回 false 而不是抛").arg(item.first));
            check(untouched.skinUrl == QStringLiteral("https://example.invalid/keep-me"),
                  QStringLiteral("T4 失败时不动 out（%1）").arg(item.first), untouched.skinUrl);
        }
        // 手改文件会干的事：variant 写成一个没见过的词 → 当经典，而不是原样透出去
        writeFileAt(path, QByteArrayLiteral(R"({"version":1,"skin":"https://a.b/c.png",)"
                                            R"("variant":"wide"})"));
        PlayerSkinProfile odd;
        check(PlayerSkinStore::loadCachedProfile(uuid, &odd) &&
                  odd.variant == QStringLiteral("classic"),
              "T4 缓存里 variant 是个没见过的词 → 读回来当经典", odd.variant);
        QFile::remove(path);
    }

    // ---- T5 贴图字节的判据（只看文件头，不解码） ----
    {
        check(PlayerSkinStore::looksLikeSkinPng(fakeSkinPng(64, 64)), "T5 64×64 → 认");
        check(PlayerSkinStore::looksLikeSkinPng(fakeSkinPng(64, 32)),
              "T5 64×32（老格式）→ 认");
        check(!PlayerSkinStore::looksLikeSkinPng(fakeSkinPng(64, 16)), "T5 64×16 → 不认");
        check(!PlayerSkinStore::looksLikeSkinPng(fakeSkinPng(128, 128)), "T5 128×128 → 不认");
        check(!PlayerSkinStore::looksLikeSkinPng(fakeSkinPng(32, 64)), "T5 32×64 → 不认");
        check(!PlayerSkinStore::looksLikeSkinPng(QByteArrayLiteral("<html>404</html>")),
              "T5 一张网页 → 不认（下回来的经常是这个）");
        check(!PlayerSkinStore::looksLikeSkinPng(QByteArray()), "T5 空字节 → 不认");
        check(!PlayerSkinStore::looksLikeSkinPng(QByteArrayLiteral("\x89PNG\r\n\x1a\n")),
              "T5 只有签名、后面截断了 → 不认（不能读越界）");
    }

    // ---- T6 贴图地址认不认（官方给的就是 http://） ----
    {
        // 下面这个 URL 是**真档案里的形状**，不是编的：官方 sessionserver 返回的
        // SKIN.url / CAPE.url 都是 `http://textures.minecraft.net/texture/<sha>`。
        // 规矩不能写成"不是 https 就拒"——官方给的每一份真档案都是 http://，那样右栏
        // 永远画默认外观、缓存目录里只躺着一份 profile-*.json、一张 .png 都没有。
        const QUrl upgraded = PlayerSkinStore::textureRequestUrl(
            QStringLiteral("http://textures.minecraft.net/texture/cfa1f1c5ca1e"));
        check(upgraded.isValid() && upgraded.scheme() == QStringLiteral("https"),
              "T6 官方那个 http:// 地址认得出来、并升级成 https", upgraded.toString());
        check(upgraded.host() == QStringLiteral("textures.minecraft.net") &&
                  upgraded.path() == QStringLiteral("/texture/cfa1f1c5ca1e"),
              "T6 升级只换协议，host 和 path 原样不动", upgraded.toString());

        const QUrl alreadyHttps = PlayerSkinStore::textureRequestUrl(skinUrl);
        check(alreadyHttps == QUrl(skinUrl),
              "T6 本来就是 https 的一动都不动", alreadyHttps.toString());

        // 拒掉的那些：地址是**外面**（解出来的档案）给的，不能让它把请求带到别处去。
        // ftp/file 是靠"协议只认 http(s)"挡下的，别人家的域名是靠 host 挡下的 —— 两条
        // 规矩各有用处，所以两条都钉住。
        const QStringList rejects = {
            QStringLiteral("file:///C:/Windows/win.ini"),
            QStringLiteral("ftp://textures.minecraft.net/texture/x"),
            QStringLiteral("https://evil.example.com/texture/x"),
            QStringLiteral("http://textures.minecraft.net.evil.example.com/x"),
            QString(),
            QStringLiteral("这不是一个地址"),
        };
        QString firstAccepted;
        for (const QString &bad : rejects) {
            if (PlayerSkinStore::textureRequestUrl(bad).isValid() && firstAccepted.isEmpty()) {
                firstAccepted = bad;
            }
        }
        check(firstAccepted.isEmpty(),
              "T6 别的协议 / 别人家的 host / 空串 / 一句废话 → 一律认不出来", firstAccepted);
    }

    {
        PlayerSkinStore::setEndpointOverrides({}, QStringLiteral("http://127.0.0.1:12345"));
        const QString local = QStringLiteral("http://127.0.0.1:12345/texture/example");
        check(PlayerSkinStore::textureRequestUrl(local) == QUrl(local),
              "T6 本地 HTTP 贴图夹具不被错误升级为 TLS");
        check(!PlayerSkinStore::textureRequestUrl(QStringLiteral("http://127.0.0.1:12346/x")).isValid()
                  && !PlayerSkinStore::textureRequestUrl(QStringLiteral("https://127.0.0.1:12345/x")).isValid(),
              "T6 贴图覆盖不放行不同端口或协议");
        PlayerSkinStore::setEndpointOverrides({}, {});
    }

    // ---- T6b 命中缓存 → 同步发信号（一个请求都没发） ----
    {
        const QString url = QStringLiteral("https://textures.minecraft.net/texture/cached-one");
        const QString path = PlayerSkinStore::cachePathForUrl(url);
        writeFileAt(path, fakeSkinPng(64, 64));

        PlayerSkinStore store;
        QString gotUrl;
        QString gotPath;
        int readyCount = 0;
        int failedCount = 0;
        QString lastError;
        QObject::connect(&store, &PlayerSkinStore::textureReady,
                         [&](const QString &readyUrl, const QString &readyPath) {
                             ++readyCount;
                             gotUrl = readyUrl;
                             gotPath = readyPath;
                         });
        QObject::connect(&store, &PlayerSkinStore::failed, [&](const QString &message) {
            ++failedCount;
            lastError = message;
        });

        store.fetchTexture(url);
        // "同步"本身就是断言：一个真的网络请求不可能在这一行之前回来。所以这一条同时
        // 钉住了两件事 —— 命中缓存会发信号、以及**它没有去问官方**。
        check(readyCount == 1 && gotPath == path,
              "T6b 命中缓存 → 同步发 textureReady（贴图 URL 是内容哈希，缓存永不失效）",
              gotPath);
        check(gotUrl == url, "T6b 发回来的 URL 就是**问的那个**（升级只发生在发请求那一步）",
              gotUrl);

        // 空 URL = 这个人没有披风。**常态**，不是错误：一个信号都不该发。
        store.fetchTexture(QString());
        check(readyCount == 1 && failedCount == 0,
              "T6b 没有披风（空 URL）→ 什么都不做、也不报错");

        // 地址是我们从外面解出来的，取之前要过 textureRequestUrl()（判据见上一节）。
        // 这样写还有个好处：这条路**不发请求**，于是"地址不认就直接判失败"这件事能在
        // 离线的一节里钉住。
        store.fetchTexture(QStringLiteral("file:///C:/Windows/win.ini"));
        check(failedCount == 1 && lastError.contains(QStringLiteral("不可用")),
              "T6b 地址不认的贴图 → 直接当失败（不跟着一份被改过的档案去下任意地址）",
              lastError);

        // NOTE: "不命中缓存时会去请求"这条**故意不在这里断言** —— 那会真的发一个请求，
        // 而这一节是不联网的。它的两个半边都有别的落点：缓存检查发生在请求之前
        // （T6b 那条同步性），请求本身的行为留给真机验证。
        QFile::remove(path);
    }

    // ---- T7 状态码怎么读（204 是唯一反直觉的那一个） ----
    {
        using Outcome = PlayerSkinStore::ProfileFetchOutcome;
        const QString inner =
            QStringLiteral(R"JSON({"textures":{"SKIN":{"url":"%1"}}})JSON").arg(skinUrl);
        check(PlayerSkinStore::classifyProfileResponse(200, envelope(inner)) == Outcome::Ok,
              "T7 200 + 一份能解出皮肤的 body → Ok");
        check(PlayerSkinStore::classifyProfileResponse(204, QByteArray()) ==
                  Outcome::NoSuchPlayer,
              "T7 204 → 「查不到这个人」（sessionserver 也用 204 表达软限流），**不是错误**");
        // 429 必须和 204 分开：它确实是限流，而且是**明说**的那种（204 是"查不到 / 软
        // 限流"那条路），要退避。
        check(PlayerSkinStore::classifyProfileResponse(429, QByteArray()) == Outcome::Throttled,
              "T7 429 → 限流（也不是错误，但和 204 不同：它明说问得太勤，要退避）");

        // 退避窗口。这三条是"又 429"那个现象的解药：被挡之后这一分钟里不要再问。
        const QDateTime throttleNow = QDateTime::currentDateTimeUtc();
        check(!PlayerSkinStore::withinThrottleBackoff(QDateTime(), throttleNow),
              "T7 从没被限流过（无效值）→ 不跳（默认那份不许变成「永远跳过」）");
        check(PlayerSkinStore::withinThrottleBackoff(
                  throttleNow.addMSecs(PlayerSkinStore::kThrottleBackoffMs), throttleNow),
              "T7 刚被限流过 → 这一分钟里跳过（不然每次刷新都往限流窗口里撞）");
        check(!PlayerSkinStore::withinThrottleBackoff(throttleNow.addMSecs(-1), throttleNow),
              "T7 退避窗口过了 → 照常去问（限流不许把外观永久冻住）");
        check(PlayerSkinStore::classifyProfileResponse(500, QByteArray()) == Outcome::Failed,
              "T7 500 → 失败");
        check(PlayerSkinStore::classifyProfileResponse(0, QByteArray()) == Outcome::Failed,
              "T7 连状态码都没有（网络层就失败了）→ 失败");
        // 被代理塞了一张网页时很常见：状态码是 200，但 body 不是档案。
        // 这一条是**必须**的 —— 只看状态码会把一页 HTML 当成"这人没皮肤"。
        check(PlayerSkinStore::classifyProfileResponse(
                  200, QByteArrayLiteral("<html>429 Too Many Requests</html>")) ==
                  Outcome::Failed,
              "T7 200 但 body 不是档案 → 失败（不能只看状态码）");
    }

    // ---- T8 两发外观请求都走 Control 额度（本机真实 HTTP 夹具） ----
    //
    // 这一节要证的是"皮肤这条路**真的**进了共用额度"，不是复刻 ControlGet 那一层的行为
    // （排队、截止、删作业的语义已在 PH6 和 RequestBudgetChecks 里逐条钉过）。所以只量
    // 四件事：额度占满时一个字节都不发；名额回来就照常发出并收尾；收尾后不残留租约；
    // 排队中的作业跟着对象一起删掉时，名额腾出来也不发。
    {
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost), "T8 夹具端口");
        const QString hash = QStringLiteral("0123456789abcdef").repeated(4);
        const QByteArray png = fakeSkinPng(64, 64);
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        PlayerSkinStore::setEndpointOverrides(base + QStringLiteral("/profile/"), base);
        // 档案里那张贴图**指着夹具**（走 textureUrlForHash 的覆盖那条路），于是"档案到了
        // → 拿它的 skinUrl 去下贴图"这一整条链在本地闭环。
        const QString textureUrl = PlayerSkinStore::textureUrlForHash(hash);
        const QByteArray profileBody = envelope(
            QStringLiteral(R"JSON({"textures":{"SKIN":{"url":"%1"}}})JSON").arg(textureUrl));
        const QByteArray listBody =
            QStringLiteral(R"JSON({"skins":[{"texture":"%1"}]})JSON").arg(hash).toUtf8();

        int profileRequests = 0;
        int textureRequests = 0;
        int listRequests = 0;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    const QByteArray request = socket->readAll();
                    if (socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);
                    QByteArray body = png;
                    if (request.startsWith("GET /profile/")) {
                        ++profileRequests;
                        body = profileBody;
                    } else if (request.startsWith("GET /texture/")) {
                        ++textureRequests;
                        body = png;
                    } else {
                        ++listRequests;
                        body = listBody;
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                                  QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n");
                    socket->write(body);
                    socket->disconnectFromHost();
                });
            }
        });
        const auto pump = [](int ms) {
            QEventLoop loop;
            QTimer::singleShot(ms, &loop, &QEventLoop::quit);
            loop.exec();
        };
        const auto pumpUntil = [&](auto done, int ms) {
            QElapsedTimer clock;
            clock.start();
            while (!done() && clock.elapsed() < ms) pump(20);
            return done();
        };
        const auto fillControlLane = [] {
            QList<RequestBudget::Lease> holds;
            while (auto lease = RequestBudget::tryAcquire(RequestBudget::Lane::Control)) {
                holds.append(lease);
            }
            return holds;
        };
        const int baseline = RequestBudget::snapshot().control;
        const QString uuid = QStringLiteral("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f");
        const QString cached = PlayerSkinStore::cachePathForUrl(textureUrl);
        QFile::remove(cached);

        {
            PlayerSkinStore store;
            int profiles = 0;
            QString profileSkinUrl;
            QObject::connect(&store, &PlayerSkinStore::profileReady, &store,
                             [&](const QString &, const PlayerSkinProfile &profile) {
                                 ++profiles;
                                 profileSkinUrl = profile.skinUrl;
                             });

            auto holds = fillControlLane();
            store.refreshForUuid(uuid);
            pump(150);
            check(profileRequests == 0 && profiles == 0,
                  "T8 额度占满时取档案只排队：一个字节都没发、也不回调");
            holds.clear();
            check(pumpUntil([&] { return profiles > 0; }, 2000) && profileRequests == 1 &&
                      profileSkinUrl == textureUrl,
                  "T8 名额回来这一发就发出去，档案照常到达", profileSkinUrl);

            int textures = 0;
            QString gotPath;
            QObject::connect(&store, &PlayerSkinStore::textureReady, &store,
                             [&](const QString &, const QString &path) {
                                 ++textures;
                                 gotPath = path;
                             });
            holds = fillControlLane();
            store.fetchTexture(textureUrl);
            pump(150);
            check(textureRequests == 0 && textures == 0,
                  "T8 额度占满时下贴图也只排队（那一发还没发出去）");
            holds.clear();
            check(pumpUntil([&] { return textures > 0; }, 2000) && textureRequests == 1 &&
                      QFile::exists(gotPath),
                  "T8 名额回来就下贴图、过 PNG 判据、落盘",
                  QDir::toNativeSeparators(gotPath));

        }

        // 排队中的作业跟着对象一起删掉：名额腾出来也不许偷偷发。
        {
            const int before = profileRequests;
            auto *doomed = new PlayerSkinStore;
            auto holds = fillControlLane();
            doomed->refreshForUuid(uuid);
            pump(100);
            delete doomed;
            holds.clear();
            pump(300);
            check(profileRequests == before &&
                      RequestBudget::snapshot().control == baseline,
                  "★★ T8 排队中的作业跟着对象一起删掉 ⇒ 名额腾出来也不发、租约不残留");
        }

        PlayerSkinStore::setEndpointOverrides({}, {});
        QFile::remove(cached);
    }
}


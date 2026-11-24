/**
 * @file SupportFakes.hpp
 * @brief mw_check 的**假外网**：本地假接口（Mojang / Modrinth / CurseForge / 图标 / 反查 /
 *        跨卷延迟 / 接了不答话）以及预置皮肤那几个夹具。
 *
 * 它们各自**拥有一份状态**（收了几发、带了什么头、按哪条路由答），断言读的就是这份状态 ——
 * 所以归在一起，和"断言与小工具"（`SupportCore.hpp`）、"真窗口那套 UI 夹具"
 * （`Support.hpp`）分开。
 *
 * NOTE: 这些夹具都只听 127.0.0.1 的随机端口，**不碰外网** —— 默认那一跑在断网机器上
 * 也要给出同一个结果（理由见 `Support.hpp` 的文件头）。
 */
#pragma once

#include "SupportCore.hpp"
/// 一张现造的、解得出像素的 PNG（这一节要拿它当真图标字节用）。
static QByteArray solidPngBytes(const QColor &color, int side = 32) {
    QImage image(side, side, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    buffer.close();
    return bytes;
}

/// 一个只答一种话的假图标服务器（127.0.0.1、随机端口）。
///
/// 记着"来问过几次"和"那一发带了哪些条件头"—— ZB8 要验的东西全在这两件事上。用它就
/// 不碰外网，在哪台机器上跑都是同一个结果；而"核对那一发"是这次改动最要紧的那条路，
/// 光靠 icon-live 那条只报数的探针是钉不住的。
struct FakeIconServer {
    QTcpServer server;
    QByteArray body;      ///< 200 时发回去的正文
    QByteArray headers;   ///< 200 时额外带的头（ETag / Last-Modified 那两条）
    int status = 200;
    int requests = 0;
    QString lastIfNoneMatch;
    QString lastIfModifiedSince;

    bool start() {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    if (socket->property("answered").toBool()) {
                        return; // 一个连接上只答一发（这一发不带正文，读一次就够）
                    }
                    socket->setProperty("answered", true);
                    const QByteArray request = socket->readAll();
                    ++requests;
                    const int headerEnd = request.indexOf("\r\n\r\n");
                    const QByteArray head = headerEnd > 0 ? request.left(headerEnd) : request;
                    for (const QByteArray &line : head.split('\n')) {
                        const QByteArray trimmed = line.trimmed();
                        const int colon = trimmed.indexOf(':');
                        if (colon <= 0) {
                            continue;
                        }
                        const QByteArray name = trimmed.left(colon).trimmed().toLower();
                        const QString value =
                            QString::fromUtf8(trimmed.mid(colon + 1)).trimmed();
                        if (name == "if-none-match") {
                            lastIfNoneMatch = value;
                        } else if (name == "if-modified-since") {
                            lastIfModifiedSince = value;
                        }
                    }
                    QByteArray response = QByteArrayLiteral("HTTP/1.1 ") +
                                          QByteArray::number(status) +
                                          (status == 304 ? QByteArrayLiteral(" Not Modified\r\n")
                                                         : QByteArrayLiteral(" OK\r\n"));
                    if (status == 304) {
                        // 没变：一个字节的正文都没有。**这一条正是"不严重占网络"的全部根据。**
                        response += QByteArrayLiteral("Content-Length: 0\r\n\r\n");
                    } else {
                        response += headers;
                        response += QByteArrayLiteral("Content-Length: ") +
                                    QByteArray::number(body.size()) +
                                    QByteArrayLiteral("\r\n\r\n");
                        response += body;
                    }
                    socket->write(response);
                    socket->flush();
                });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }
};

/// 一个"上来就答 500"的假 CurseForge 接口（127.0.0.1、随机端口）。
///
/// AA10 要的只有两件事：**这一发确实发到了这里**（数连接），以及答复是**一句我们能认出来
/// 的错**（那句原话必须原样出现在弹窗里）。所以它比 verify 里那个真解析请求、按路径分发的
/// 版本短得多 —— 那边要验的是"正文写成什么样"，这边要验的是"到底发没发出来"。
///
/// NOTE: 得等到 `readyRead` 再答（不能 accept 了就写）：客户端那时才刚把请求送出来，
/// 早答是一场比赛，赢了也只是运气好。
struct OneShotCfErrorApi {
    QTcpServer server;
    int hits = 0;

    bool start() {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    if (socket->property("answered").toBool()) {
                        return;
                    }
                    socket->setProperty("answered", true);
                    ++hits;
                    socket->readAll();
                    const QByteArray body =
                        QByteArrayLiteral("{\"error\":\"接口说你没填 key\"}");
                    socket->write(QByteArrayLiteral("HTTP/1.1 500 Error\r\n"
                                                    "Content-Type: application/json\r\n"
                                                    "Content-Length: ") +
                                  QByteArray::number(body.size()) +
                                  QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
                    socket->flush();
                    socket->disconnectFromHost();
                });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }

    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
    }
};

// ---------------------------------------------------------------------------
// 默认那一跑的**假外网**：Mojang 清单 / 皮肤档案 / Minecraft Services /
// Modrinth / CurseForge 全部指到这一台 127.0.0.1 上。
//
// 为什么非要它：默认那一跑里第一个 MainWindow 一构造就发版本清单，下载页那四页切过去
// 又各发一次搜索，账号那几节还会问皮肤档案。真实网络在场时它们的**耗时和结果**都不
// 归这一跑管 —— 断网机器上每次都要等满 15 秒超时，整跑时长跟着网络抖；反过来，连得上
// 的机器上又会把真清单塞进树里，断言读到多少条全看那天官方发了什么版本。
//
// 所以这里**按请求目标路由**：路径认得出就答一份最小正文，认不出就 404（真服务器上
// "没有这个路由"也是这个样子）。每一发都记进 `hits`，断言看的是**请求次数**，不是
// 响应回到的次序 —— 顺序在并发下本来就不该被断言。
//
// 覆盖从哪来：`LauncherSettings` 的四条新键 + 已有的两条目录接口地址，在第一个
// MainWindow 构造之前写进沙盒 INI（见 main.cpp）。产品侧永远是空串、走官方地址。
struct FakeNetwork {
    QTcpServer server;
    /// 请求路径（去掉查询串）→ 次数。
    QHash<QString, int> hits;
    QStringList paths;  ///< 每一发的路径（带查询串），排查用
    /// 没有路由、被 404 掉的请求数。夹具没演到的路由会落在这里 —— 它比"某条断言
    /// 意外地没红"更早说明这一跑其实没测到东西。
    int notFound = 0;
    /// 被 404 的那几条路径（带查询串）。排查"为什么有 404"时直接看这个，别去翻整个 `paths`。
    QStringList unrouted;

    bool start() {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket]() { answer(socket); });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }

    QString base() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }
    QString versionManifestUrl() const {
        return base() + QStringLiteral("/version_manifest_v2.json");
    }
    /// 皮肤档案端点的**完整前缀**（`…/session/minecraft/profile`）。产品那边
    /// `PlayerSkinStore::setEndpointOverrides()` 拿到的就是这个前缀，然后直接拼 32 位 uuid ——
    /// 少写 `/minecraft/profile` 的话请求会变成 `/session/<uuid>`（真实 sessionserver 上没有
    /// 这条路由），夹具这边只会回 404，而界面看起来和"这个人没有皮肤"一模一样。
    QString sessionServerBase() const {
        return base() + QStringLiteral("/session/minecraft/profile");
    }
    QString minecraftServicesBase() const { return base(); }
    QString modrinthBase() const { return base() + QStringLiteral("/v2"); }
    QString curseForgeBase() const { return base() + QStringLiteral("/v1"); }

    /// 某条路径（不带查询串）被打了几次。**断言只认这个**，不认 `paths` 的下标。
    int count(const QString &path) const { return hits.value(path, 0); }

    void answer(QTcpSocket *socket) {
        if (socket->property("answered").toBool()) {
            return;
        }
        const QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            socket->setProperty("buffer", buffer);  // 头还没到齐
            return;
        }
        socket->setProperty("buffer", QByteArray());
        socket->setProperty("answered", true);

        QString target;
        for (const QByteArray &line : buffer.left(headerEnd).split('\n')) {
            const QList<QByteArray> parts = line.trimmed().split(' ');
            if (parts.size() >= 2 && parts.at(1).startsWith('/')) {
                target = QString::fromUtf8(parts.at(1));
                break;
            }
        }
        paths << target;
        const int query = target.indexOf(QLatin1Char('?'));
        const QString path = query < 0 ? target : target.left(query);
        ++hits[path];

        const auto send = [socket](int status, const QByteArray &body,
                                   const QByteArray &contentType = QByteArrayLiteral(
                                       "application/json")) {
            socket->write(QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' +
                          (status == 200 ? QByteArrayLiteral("OK")
                                         : QByteArrayLiteral("Not Found")) +
                          QByteArrayLiteral("\r\nContent-Type: ") + contentType +
                          QByteArrayLiteral("\r\nContent-Length: ") +
                          QByteArray::number(body.size()) +
                          QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
            socket->flush();
            socket->disconnectFromHost();
        };

        // ---- 版本清单（`Launcher/VersionManifestUrl`）----
        if (path == QLatin1String("/version_manifest_v2.json")) {
            const QString versions =
                QStringLiteral(
                    R"([{"id":"1.21.4","type":"release","url":"%1/versions/1.21.4.json",)"
                    R"("time":"2024-12-03T10:12:00+00:00","releaseTime":"2024-12-03T10:12:00+00:00"},)"
                    R"({"id":"26.3","type":"snapshot","url":"%1/versions/26.3.json",)"
                    R"("time":"2026-03-01T10:00:00+00:00","releaseTime":"2026-03-01T10:00:00+00:00"},)"
                    R"({"id":"1.20.1","type":"release","url":"%1/versions/1.20.1.json",)"
                    R"("time":"2023-06-12T10:00:00+00:00","releaseTime":"2023-06-12T10:00:00+00:00"},)"
                    R"({"id":"1.2.4","type":"release","url":"%1/versions/1.2.4.json",)"
                    R"("time":"2012-03-22T10:00:00+00:00","releaseTime":"2012-03-22T10:00:00+00:00"}])")
                    .arg(base());
            send(200, QStringLiteral(R"({"latest":{"release":"1.21.4","snapshot":"26.3"},)"
                                     R"("versions":%1})")
                          .arg(versions)
                          .toUtf8());
            return;
        }
        if (path.startsWith(QLatin1String("/versions/")) &&
            path.endsWith(QLatin1String(".json"))) {
            const QString id =
                path.mid(10, path.size() - 10 - 5);  // 去掉 `/versions/` 和 `.json`
            const bool hasServer = id != QLatin1String("1.2.4") &&
                                   !id.startsWith(QLatin1String("a")) &&
                                   !id.startsWith(QLatin1String("b"));
            const QString downloads =
                hasServer
                    ? QStringLiteral(R"("downloads":{"server":{"url":"%1/server/%2.jar","sha1":"0000","size":1024}},)")
                          .arg(base(), id)
                    : QStringLiteral(R"("downloads":{},)");
            send(200, QStringLiteral(R"({"id":"%1","type":"release",%2)"
                                     R"("mainClass":"net.minecraft.client.main.Main",)"
                                     R"("assets":"1.20","libraries":[],)"
                                     R"("arguments":{"game":[],"jvm":[]}})")
                          .arg(id, downloads)
                          .toUtf8());
            return;
        }

        // ---- 皮肤档案（`Launcher/SessionServerBase`）与 Minecraft Services ----
        // 只给一份**没有贴图**的档案：`PlayerSkinStore` 会当场判"这份不算数"并退回默认
        // 外观。这里的目的是**不联网、不挂 15 秒**，不是演一份真外观（真外观那几条在
        // MwPlayerSkinChecks 里直接喂信号，不经过这条路）。
        if (path.startsWith(QLatin1String("/session/minecraft/profile/")) ||
            path == QLatin1String("/minecraft/profile")) {
            send(200, QByteArrayLiteral(
                          R"({"id":"0123456789abcdef0123456789abcdef","name":"Fixture",)"
                          R"("skins":[],"capes":[],"properties":[]})"));
            return;
        }

        // ---- Modrinth 目录（`Launcher/ModrinthApiBase`）----
        if (path == QLatin1String("/v2/search")) {
            send(200, QByteArrayLiteral(
                          R"({"hits":[],"offset":0,"limit":20,"total_hits":0})"));
            return;
        }
        if (path.startsWith(QLatin1String("/v2/project/")) &&
            path.endsWith(QLatin1String("/version"))) {
            send(200, QByteArrayLiteral("[]"));
            return;
        }
        if (path == QLatin1String("/v2/projects")) {
            send(200, QByteArrayLiteral("[]"));
            return;
        }
        if (path == QLatin1String("/v2/version_files") ||
            path == QLatin1String("/v2/version_files/update")) {
            send(200, QByteArrayLiteral("{}"));
            return;
        }
        if (path.startsWith(QLatin1String("/v2/project/"))) {
            send(200, QByteArrayLiteral(R"({"id":"fixture","title":"Fixture"})"));
            return;
        }

        // ---- CurseForge 目录（`Launcher/CurseForgeApiBase`）----
        if (path == QLatin1String("/v1/mods/search")) {
            send(200, QByteArrayLiteral(
                          R"({"data":[],"pagination":{"index":0,"pageSize":20,)"
                          R"("resultCount":0,"totalCount":0}})"));
            return;
        }
        if (path == QLatin1String("/v1/mods/files") ||
            path == QLatin1String("/v1/mods") ||
            path == QLatin1String("/v1/fingerprints")) {
            send(200, QByteArrayLiteral(R"({"data":[]})"));
            return;
        }
        if (path.startsWith(QLatin1String("/v1/mods/")) &&
            path.endsWith(QLatin1String("/files"))) {
            send(200, QByteArrayLiteral(R"({"data":[]})"));
            return;
        }
        if (path.startsWith(QLatin1String("/v1/mods/"))) {
            send(200, QByteArrayLiteral(R"({"data":{}})"));
            return;
        }

        ++notFound;
        unrouted << target;
        send(404, QByteArrayLiteral(R"({"error":"fixture: no route"})"));
    }
};

/// 把假外网的五条地址写进**当前沙盒 INI**。必须在构造任何产品对象之前调 ——
/// `MainWindow` 一构造就发版本清单，地址读的是那一刻的设置。
///
/// 以前只有默认那一跑（`tests/mw_check/main.cpp`）这么写，独立构建的专项
/// （例如 `wtfml.workspace`）各自构造 `MainWindow` 却什么都没装 ⇒ 那一跑实际打的是
/// 真 Mojang：断网机器上等满超时，连得上时把当天官方清单塞进树里，而断言全绿。
/// 凡是 ctest 里会构造 `MainWindow` 的入口都走这里。
static void installFakeNetworkSettings(const FakeNetwork &fakeNet) {
    QSettings sandbox;
    sandbox.setValue(QStringLiteral("Launcher/VersionManifestUrl"),
                     fakeNet.versionManifestUrl());
    sandbox.setValue(QStringLiteral("Launcher/SessionServerBase"),
                     fakeNet.sessionServerBase());
    // NOTE: **不设 `Launcher/TextureBase`**。贴图地址那条路（PlayerSkinStore::
    // textureRequestUrl）把 scheme 强制升级成 https，也按覆盖后的 host 校验地址；
    // 指到一个明文 http 的 127.0.0.1 夹具上会让"官方贴图地址"一律判成不合法 ——
    // 预置皮肤那几节（AF）读回来的盘上那一行会变成空串。而假档案里 `skins`/`capes`
    // 都是空的，本来就不会去取任何贴图；真要去取也还是官方 host，不会打到别的什么地方。
    sandbox.setValue(QStringLiteral("Launcher/MinecraftServicesBase"),
                     fakeNet.minecraftServicesBase());
    sandbox.setValue(QStringLiteral("Launcher/ModrinthApiBase"), fakeNet.modrinthBase());
    sandbox.setValue(QStringLiteral("Launcher/CurseForgeApiBase"), fakeNet.curseForgeBase());
    sandbox.sync();
}

// 假服务器：GET 一发，按路径分发出两种答复，并把"请求行 + x-api-key"记下来。
// NOTE: 这里的答复只用**最小**的合成正文（真夹具那两条在 verify 的 Z7f/Z7h 里）——
// 这一节量的是"发到哪、带什么头、谁接的"，正文长得对不对不归它管。
struct FakeResourceApi {
    QTcpServer server;
    QByteArray searchBody;   ///< `/mods/search` 那一发的答复
    QByteArray filesBody;    ///< `/mods/{id}/files` 那一发的答复
    int searchStatus = 200;
    int filesStatus = 200;
    QStringList requests;    ///< 每一发的请求行（`GET /v1/mods/search?… HTTP/1.1`）
    QStringList apiKeys;     ///< 每一发带的 x-api-key（**没带 = 空串**）
    /// 按**请求目标**数的次数（不按下标、不依赖回答次序 —— 并发下先后本来就不定）。
    int searchHits = 0;      ///< `/mods/search` 那一发
    int filesHits = 0;       ///< `/mods/{id}/files` 那一发
    /// `POST /v1/mods`（批量问项目名）那一发。答的是 `{"data":[]}` —— 也就是**查询成功、
    /// 但一条都没有**：详情页"前置名字"那条路（R3）要的正是这一档，它曾经回来一次就
    /// 立刻再问一次。
    int projectBatchHits = 0;

    bool start() {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket]() { answer(socket); });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }

    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
    }

    /// 最后那一发的请求行（断言里看的就是它）。
    QString lastRequest() const { return requests.isEmpty() ? QString() : requests.last(); }

    void answer(QTcpSocket *socket) {
        if (socket->property("answered").toBool()) {
            return; // 一发只答一次（GET 没有正文，但 readyRead 可能来不止一回）
        }
        const QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            socket->setProperty("buffer", buffer);  // 头还没到齐
            return;
        }
        socket->setProperty("buffer", QByteArray());
        socket->setProperty("answered", true);
        const QList<QByteArray> headLines = buffer.left(headerEnd).split('\n');
        QString apiKey;
        QString target;
        QByteArray method = QByteArrayLiteral("GET");
        for (const QByteArray &line : headLines) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.startsWith(QByteArrayLiteral("GET "))) {
                target = QString::fromUtf8(trimmed.split(' ').value(1));
            } else if (trimmed.startsWith(QByteArrayLiteral("POST "))) {
                // 带正文的那一发（`POST /v1/mods` 批量问项目名）：只认请求行，正文不看。
                method = QByteArrayLiteral("POST");
                target = QString::fromUtf8(trimmed.split(' ').value(1));
            } else if (trimmed.toLower().startsWith(QByteArrayLiteral("x-api-key:"))) {
                apiKey = QString::fromUtf8(trimmed.mid(10)).trimmed();
            }
        }
        requests << QString::fromUtf8(method) + QLatin1Char(' ') + target +
                        QStringLiteral(" HTTP/1.1");
        apiKeys << apiKey;

        if (method == QByteArrayLiteral("POST")) {
            // 查询成功、结果为空（见 `projectBatchHits` 的注释）。
            ++projectBatchHits;
            const QByteArray empty = QByteArrayLiteral("{\"data\":[]}");
            socket->write(QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                            "Content-Length: ") +
                          QByteArray::number(empty.size()) +
                          QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + empty);
            socket->flush();
            socket->disconnectFromHost();
            return;
        }

        const bool isSearch = target.contains(QStringLiteral("/mods/search"));
        if (isSearch) {
            ++searchHits;
        } else {
            ++filesHits;
        }
        const int status = isSearch ? searchStatus : filesStatus;
        const QByteArray payload = isSearch ? searchBody : filesBody;
        socket->write(QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' +
                      (status == 200 ? QByteArrayLiteral("OK") : QByteArrayLiteral("Error")) +
                      QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ") +
                      QByteArray::number(payload.size()) +
                      QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + payload);
        socket->flush();
        socket->disconnectFromHost();
    }
};

/// 发一发、等它回来。返回 false = 超时（这一条本身就该红）。
static bool waitForSearch(ResourceStore &store, const QString &key,
                          ResourceCatalog::SearchResult *out) {
    bool done = false;
    const QMetaObject::Connection conn = QObject::connect(
        &store, &ResourceStore::searchFinished, &store,
        [&done, out](const QString &, const ResourceCatalog::SearchResult &result) {
            *out = result;
            done = true;
        });
    const bool ok = pumpUntil([&done]() { return done; }, 10000);
    QObject::disconnect(conn);
    return ok;
}

static bool waitForVersions(ResourceStore &store, const QString &key,
                            QList<ResourceCatalog::VersionEntry> *out, QString *problem) {
    bool done = false;
    const QMetaObject::Connection conn = QObject::connect(
        &store, &ResourceStore::versionsFinished, &store,
        [&done, out, problem](const QString &,
                              const QList<ResourceCatalog::VersionEntry> &versions,
                              const QString &why) {
            *out = versions;
            *problem = why;
            done = true;
        });
    const bool ok = pumpUntil([&done]() { return done; }, 10000);
    QObject::disconnect(conn);
    return ok;
}

/// 一个答"反查"那两发的假服务器（127.0.0.1、随机端口）。
///
/// NOTE: 这一台是**POST**，正文可能来得比头晚（`FakeResourceApi` 那边是 GET、没有正文，
/// 所以那边读到第一包就答了）—— 这里必须攒够 `Content-Length` 再回，否则会对着
/// 一份半截的请求体答话，而产品那边读到的是一个"人已经走了"的连接。
struct FakeLookupApi {
    QTcpServer server;
    QByteArray modrinthReply;   ///< `POST /v2/version_files` 的答复正文
    QByteArray modrinthVersionFilesUpdateReply;
    QByteArray curseForgeVersionListReply;
    /// 按项目号分叉的那一份（同一个手法、同一个理由，见 `modrinthVersionListByProject`）。
    QHash<QString, QByteArray> curseForgeVersionListByMod;
    /// `GET /v2/projects?ids=…` 的答复正文（**一个数组**）——T22 的图标地址只有这一发给
    /// 得出来（那条 `<id>/icon.png` 只是老项目的别名，见 ResourceCatalog 里那段实测）。
    QByteArray modrinthProjectsReply;
    /// `GET /v2/project/<id>/version` 的答复正文（**一个数组**）—— 详情页底下那一份版本列表
    /// 只有这一发给得出来（开页时"按实例的版本预选、把它的加载器排前面"这件事
    /// 要**有版本可排**才量得到，一列空表上什么都看不出来）。
    QByteArray modrinthVersionListReply;
    /// T31 补：**按项目号分叉**的那一份版本列表（项目号 → 答复正文）。
    ///
    /// 为什么非要它：T31 要在一个假服务器上同时给出"这个模组的版本"和"它那个前置的版本"
    /// —— 两份**不是同一份正文**（前置那一条自己不能又声明一遍前置，不然就递归了）。
    /// 只看 `modrinthVersionListReply` 的话，两发请求拿到的是同一个答案，于是要么模组那页
    /// 摆着前置的版本、要么前置解不开 —— 而屏幕上/日志里都看不出来是夹具的问题。
    /// 表里没有的项目号照旧走 `modrinthVersionListReply`（其余几节一个字都不受影响）。
    QHash<QString, QByteArray> modrinthVersionListByProject;
    QByteArray curseForgeReply; ///< `/v1/fingerprints` 的答复正文
    /// `POST /v1/mods` 的答复正文（T22 才用得上：图标反查要拿 `logo` 那两条地址）。
    QByteArray curseForgeModsReply;
    int modrinthStatus = 200;   ///< 想演"接口挂了"那一档时改成 500
    int modrinthProjectsStatus = 200;
    int modrinthVersionListStatus = 200;
    int modrinthVersionFilesUpdateStatus = 200;
    /// 为什么需要它：产品那一边"批量那一发"和"一项目一发那一拨"是**并着跑**的，谁先回来
    /// 由网络定。而有一个真出现过的坏法**只在某个次序下才露**：CurseForge 那一拨先回来 ⇒
    /// 整趟被提前收尾 ⇒ 批量那一发回来时已经没人等它了（收尾判据漏了 `pendingBatchKeys`，
    /// 见 `onUpdateCheckVersionsFinished()` 里那段）⇒ 报告里只剩单发那些。真机上真这么
    /// 发生过（用户那句「无法正常获取更新」）。
    ///
    /// 所以 T32 把这一发**按 400 毫秒压后**，让"CurseForge 先答"这件事从"看运气"变成
    /// **每次都是**。不压的话那一条断言是在赌：输的那几次照样绿，用户那边照样出错。
    int modrinthVersionFilesUpdateDelayMs = 0;
    int curseForgeStatus = 200;
    int curseForgeModsStatus = 200;
    int curseForgeVersionListStatus = 200;
    QStringList requests;       ///< 每一发的请求行（`GET`/`POST` 都在，target 带着查询串）
    QList<QByteArray> bodies;   ///< 每一发的正文（GET 是空的，仍然占一位，和 requests 对齐）
    /// **按请求目标**数的次数。断言不该看 `requests` 的下标 —— 单发和批量是并着跑的，
    /// 谁先回来由网络定（T32 那条延迟就是为了把"某个坏次序"变成每次都发生才存在的）。
    QHash<QString, int> routeHits;

    int count(const QString &route) const { return routeHits.value(route, 0); }

    bool start() {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket]() { answer(socket); });
            }
        });
        return server.listen(QHostAddress::LocalHost, 0);
    }

    /// 推进 `Launcher/ModrinthApiBase` 的那一个地址（产品会在它后面接 `/version_files`）。
    QString modrinthBase() const {
        return QStringLiteral("http://127.0.0.1:%1/v2").arg(server.serverPort());
    }
    /// 推进 `Launcher/CurseForgeApiBase` 的那一个（base 自带 `/v1`，见那个键的说明）。
    QString curseForgeBase() const {
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
    }

    /// 最后那一发的请求行（断言里看的就是它）。
    QString lastRequest() const { return requests.isEmpty() ? QString() : requests.last(); }

    void answer(QTcpSocket *socket) {
        if (socket->property("answered").toBool()) {
            return;
        }
        const QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            socket->setProperty("buffer", buffer);  // 头还没到齐
            return;
        }
        int contentLength = 0;
        QString target;
        QString verb;
        const QList<QByteArray> headLines = buffer.left(headerEnd).split('\n');
        for (const QByteArray &line : headLines) {
            const QByteArray trimmed = line.trimmed();
            // 请求行 `GET /v2/projects?ids=… HTTP/1.1` —— 头那一行（`Host: …`）第二段不以
            // `/` 开头，所以这一个判据认得出请求行。**GET 和 POST 都要认**：图标地址那一发
            // 是 GET（`VersionManager` 那两发是 POST；只认 POST 的话 GET 会被当成 404）。
            const QList<QByteArray> parts = trimmed.split(' ');
            if (parts.size() >= 2 && parts.at(1).startsWith('/')) {
                verb = QString::fromUtf8(parts.at(0));
                target = QString::fromUtf8(parts.at(1));
            } else if (trimmed.toLower().startsWith(QByteArrayLiteral("content-length:"))) {
                contentLength = trimmed.mid(15).trimmed().toInt();
            }
        }
        const QByteArray body = buffer.mid(headerEnd + 4);
        if (body.size() < contentLength) {
            socket->setProperty("buffer", buffer);  // 正文还没到齐
            return;
        }
        socket->setProperty("buffer", QByteArray());
        socket->setProperty("answered", true);
        requests << verb + QStringLiteral(" ") + target + QStringLiteral(" HTTP/1.1");
        bodies << body;

        const bool isModrinth = target.endsWith(QStringLiteral("/version_files"));
        // `POST /v2/version_files/update`（更新检测那一拨的批量那一发，见上面那段）。
        // 它**不是** `/version_files` 的尾巴对得上就完事 —— 那个后缀判断对它正好是 false，
        // 所以两件事分得开。
        const bool isModrinthUpdate = target.endsWith(QStringLiteral("/version_files/update"));
        // `GET {base}/mods/<项目号>/files`（CurseForge 的版本列表）。和下面那条 `/mods`
        // （POST，图标反查要的 `logo`）不是一回事。
        const bool isCurseForgeVersionList = target.contains(QStringLiteral("/mods/")) &&
                                             target.contains(QStringLiteral("/files"));
        // `GET /v2/projects?ids=…`（T22 的图标地址）。**带查询串**，所以按"含"认，
        // 不按后缀（`endsWith("/projects")` 对上的是 `/v2/projects`，后面还有 `?ids=`）。
        const bool isModrinthProjects = target.contains(QStringLiteral("/projects?"));
        // `GET /v2/project/<项目号>/version`（资源详情页底下那一份版本列表）。**单数**
        // `/project/`，和搜索那几条不撞；也**不是** `/version_files`（那条是 POST，见上）。
        // 尾巴那一截可能带查询串（`?game_versions=…`），所以按"含 + 尾"两样一起认。
        const bool isModrinthVersionList = target.contains(QStringLiteral("/project/")) &&
                                           target.contains(QStringLiteral("/version"));
        const bool isCurseForge = target.endsWith(QStringLiteral("/fingerprints"));
        // `POST /v1/mods`（T22 的图标反查用它换 `logo` 那两条地址）。
        const bool isCurseForgeMods = target.endsWith(QStringLiteral("/mods"));
        // 这一发认不认（认不出来一律 404 —— 那台服务器上"没有这个路由"就是这个样子）。
        const bool known = isModrinth || isModrinthUpdate || isModrinthProjects ||
                           isModrinthVersionList || isCurseForge || isCurseForgeMods ||
                           isCurseForgeVersionList;
        if (isModrinth) {
            ++routeHits[QStringLiteral("modrinth-version-files")];
        } else if (isModrinthUpdate) {
            ++routeHits[QStringLiteral("modrinth-version-files-update")];
        } else if (isModrinthProjects) {
            ++routeHits[QStringLiteral("modrinth-projects")];
        } else if (isModrinthVersionList) {
            ++routeHits[QStringLiteral("modrinth-project-version")];
        } else if (isCurseForgeVersionList) {
            ++routeHits[QStringLiteral("curseforge-mod-files")];
        } else if (isCurseForge) {
            ++routeHits[QStringLiteral("curseforge-fingerprints")];
        } else if (isCurseForgeMods) {
            ++routeHits[QStringLiteral("curseforge-mods")];
        }
        int status = 404;
        if (known) {
            status = isModrinthUpdate
                         ? modrinthVersionFilesUpdateStatus
                         : (isModrinth
                                ? modrinthStatus
                                : (isModrinthProjects
                                       ? modrinthProjectsStatus
                                       : (isModrinthVersionList
                                              ? modrinthVersionListStatus
                                              : (isCurseForgeVersionList
                                                     ? curseForgeVersionListStatus
                                                     : (isCurseForge ? curseForgeStatus
                                                                     : curseForgeModsStatus)))));
        }
        // 版本列表那一发**按项目号挑一份**（见 `modrinthVersionListByProject` 那段）：
        // 地址长这样 `/v2/project/<项目号>/version?game_versions=…`，所以夹在
        // `/project/` 和下一个 `/` 中间那一截就是项目号。表里没有就退回公共那一份。
        QByteArray versionListPayload = modrinthVersionListReply;
        if (isModrinthVersionList) {
            const int at = target.indexOf(QStringLiteral("/project/"));
            if (at >= 0) {
                const QString rest = target.mid(at + 9);
                const QString projectId = rest.left(rest.indexOf(QLatin1Char('/')));
                if (modrinthVersionListByProject.contains(projectId)) {
                    versionListPayload = modrinthVersionListByProject.value(projectId);
                }
            }
        }
        // CurseForge 那份版本列表**也按项目号挑一份**（同一个手法、同一个理由，见上面
        // Modrinth 那一段）：地址长这样 `…/mods/<项目号>/files?…`，所以夹在 `/mods/`
        // 和下一个 `/` 中间那一截就是项目号。
        QByteArray curseForgeVersionListPayload = curseForgeVersionListReply;
        if (isCurseForgeVersionList) {
            const int at = target.indexOf(QStringLiteral("/mods/"));
            if (at >= 0) {
                const QString rest = target.mid(at + 6);
                const QString modId = rest.left(rest.indexOf(QLatin1Char('/')));
                if (curseForgeVersionListByMod.contains(modId)) {
                    curseForgeVersionListPayload = curseForgeVersionListByMod.value(modId);
                }
            }
        }
        QByteArray payload;
        if (isModrinthUpdate) {
            payload = modrinthVersionFilesUpdateReply;
        } else if (isModrinth) {
            payload = modrinthReply;
        } else if (isModrinthProjects) {
            payload = modrinthProjectsReply;
        } else if (isModrinthVersionList) {
            payload = versionListPayload;
        } else if (isCurseForgeVersionList) {
            payload = curseForgeVersionListPayload;
        } else if (isCurseForge) {
            payload = curseForgeReply;
        } else if (isCurseForgeMods) {
            payload = curseForgeModsReply;
        }
        const QByteArray response =
            QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' +
            (status == 200 ? QByteArrayLiteral("OK") : QByteArrayLiteral("Error")) +
            QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ") +
            QByteArray::number(payload.size()) +
            QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + payload;
        if (isModrinthUpdate && modrinthVersionFilesUpdateDelayMs > 0) {
            QPointer<QTcpSocket> guard(socket);
            QTimer::singleShot(modrinthVersionFilesUpdateDelayMs, &server, [guard, response]() {
                if (guard.isNull()) {
                    return;
                }
                guard->write(response);
                guard->flush();
                guard->disconnectFromHost();
            });
            return;
        }
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    }
};

/// 一台「接上就不答话」的假接口，**跑在它自己的线程里**。
///
/// 用途只有一个：让 `InstanceIconLookup` 那一趟卡在网络那一发上，好让关窗那一刻它**真的
/// 还在跑**。为什么非它不可：官方地址在离线的机器上是"连不上、立刻失败"（`HttpClient`
/// 那一发几百毫秒就回来），那一趟一秒内就跑完，钉子成了空钉子；而本机一个**听着但不回话**
/// 的端口，客户端连得上、然后就一直等响应，一直到 `HttpClient` 的 15 秒。
///
/// 必须**另起线程**：`HttpClient::send()` 是同步的（自己转自己的事件循环），而 T43.3 那
/// 一段**主线程正卡在 `~MainWindow()` 里等它**、根本转不动事件循环 —— 摆在主线程这边的
/// `QTimer` 那一下**永远响不了**，"过一会儿把连接放掉"就没机会发生（和 ServerPing 一样：
/// "假服务器必须另起线程"）。这一条线程用的是 `msleep`，不靠任何事件循环。
class StallingApi : public QThread {
public:
    /// @param holdMs 收到一发连接之后**吊多久**再把它放掉。
    explicit StallingApi(int holdMs) : m_holdMs(holdMs) {}

    /// 起来，并**等到端口拿到手**（端口要写进设置键、还得赶在造窗口之前，所以必须等到）。
    bool startAndWait(int timeoutMs = 5000) {
        start();
        return m_ready.tryAcquire(1, timeoutMs);
    }
    quint16 port() const { return m_port; }
    /// 写进 `Launcher/ModrinthApiBase` 的那一份（产品会在它后面接 `/version_files` 那条尾巴）。
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_port); }
    /// 收摊：**必须**在对象出作用域之前等它自己退出来（`QThread` 析构时还在跑，照样 abort）。
    void finish() { wait(); }

protected:
    void run() override {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            m_ready.release();
            return;
        }
        // 端口这一位就绪了 —— 信号量本身就管好了两条线程之间的先后，写完了再放。
        m_port = server.serverPort();
        m_ready.release();
        // 等第一发连接上来（最多 10 秒：连不上就是夹具坏了，别把整趟吊死在这儿）。
        if (!server.waitForNewConnection(10000)) {
            return;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        if (socket == nullptr) {
            return;
        }
        // 一个字节都不回：客户端那一发就卡在"等响应"上（它自己那 15 秒超时兜底）。
        msleep(static_cast<unsigned long>(m_holdMs));
        socket->abort();  // 到点放掉 —— 客户端那一发这时才失败收手（socket 归 server 管）
    }

private:
    int m_holdMs = 0;
    quint16 m_port = 0;
    QSemaphore m_ready;
};

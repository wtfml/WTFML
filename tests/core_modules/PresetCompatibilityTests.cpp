#include "Support.hpp"
#include "net/RequestBudget.hpp"
#include "presets/ModrinthCompatibility.hpp"

void runPresetCompatibilityTests() {
    section("PH1. 状态行那句话：缺失与「暂无法确认」分开说");
    {
        Presets::CompatibilitySummary empty;
        check(empty.text() == QStringLiteral("无待检查的模组引用。"),
              "PH1 一条引用都没有 ⇒ 直说没有要检查的", empty.text());

        Presets::CompatibilitySummary allMatched;
        allMatched.total = 2;
        allMatched.matched = 2;
        check(allMatched.isFinished() && allMatched.text() == QStringLiteral("全部匹配（2）。"),
              "PH1 全部匹配只说一句（带总数）", allMatched.text());

        Presets::CompatibilitySummary running;
        running.total = 4;
        running.matched = 1;
        running.missing = 1;
        running.unknown = 1;
        running.pending = 1;
        running.requiredMissing = 1;
        const QString runningText = running.text();
        check(!running.isFinished() && runningText.contains(QStringLiteral("检查中 3/4")) &&
                  runningText.contains(QStringLiteral("已确认缺失 1")) &&
                  runningText.contains(QStringLiteral("暂无法确认 1")) &&
                  runningText.contains(QStringLiteral("必要项缺失 1")),
              "PH1 查的过程中就把已确认的那几档说出来", runningText);

        Presets::CompatibilitySummary done;
        done.total = 3;
        done.missing = 2;
        done.unknown = 1;
        done.requiredMissing = 1;
        check(done.isFinished() &&
                  done.text() ==
                      QStringLiteral("缺失 2，暂无法确认 1，必要项缺失 1。"),
              "★ PH1 缺失与暂无法确认是两个数，谁也不并进谁", done.text());

        Presets::CompatibilitySummary bad;
        bad.total = 1;
        bad.invalid = 1;
        check(bad.text().contains(QStringLiteral("引用无法识别 1")),
              "PH1 坏引用单独一档", bad.text());
    }

    section("PH2. 本地筛选：钉死的版本 / 加载器 / 坏引用都不问网络");
    {
        Presets::CompatibilityContext context;
        context.gameVersion = QStringLiteral("1.21.1");
        context.loader = QStringLiteral("Fabric");

        QString detail;
        check(Presets::localCompatibilityCheck(makeRef(QString(), QStringLiteral("abc")), context,
                                               &detail) ==
                      Presets::CompatibilityState::Invalid &&
                  !detail.isEmpty(),
              "PH2 平台认不出来 ⇒ Invalid（本地判，不联网）", detail);
        check(Presets::localCompatibilityCheck(makeRef(QStringLiteral("modrinth"), QString()),
                                               context, &detail) ==
                  Presets::CompatibilityState::Invalid,
              "PH2 没有项目 id ⇒ Invalid", detail);
        check(Presets::localCompatibilityCheck(
                  makeRef(QStringLiteral("modrinth"), QStringLiteral("abc"), true, QString(),
                          QStringLiteral("1.20.1")),
                  context, &detail) == Presets::CompatibilityState::Missing,
              "PH2 钉了另一个游戏版本 ⇒ 缺失（本地就能判）", detail);
        check(Presets::localCompatibilityCheck(
                  makeRef(QStringLiteral("modrinth"), QStringLiteral("abc"), true, QString(),
                          QString(), QStringLiteral("forge")),
                  context, &detail) == Presets::CompatibilityState::Missing,
              "PH2 钉了另一个加载器 ⇒ 缺失（本地就能判）", detail);
        check(Presets::localCompatibilityCheck(
                  makeRef(QStringLiteral("modrinth"), QStringLiteral("abc"), true, QString(),
                          QStringLiteral("1.21.1"), QStringLiteral("fabric")),
                  context, &detail) == Presets::CompatibilityState::Pending,
              "PH2 版本/加载器都对得上 ⇒ 交给平台查", detail);

        check(Presets::normalizeCompatibilityLoader(QStringLiteral("NeoForge")) ==
                      QStringLiteral("neoforge") &&
                  Presets::normalizeCompatibilityLoader(QStringLiteral("原版")).isEmpty() &&
                  Presets::normalizeCompatibilityLoader(QStringLiteral("none")).isEmpty(),
              "PH2 加载器名归一：大小写 / 原版 / none");
        check(Presets::normalizeCompatibilityLoader(QStringLiteral("Fabric-loader")) == QStringLiteral("fabric") &&
                  Presets::normalizeCompatibilityLoader(QStringLiteral("quilt-loader")) == QStringLiteral("quilt") &&
                  Presets::normalizeCompatibilityLoader(QStringLiteral("unknown-loader")) == QStringLiteral("unknown-loader"),
              "PH2 平台加载器名归一且不猜未知名称");
        check(Presets::CompatibilityContext{QStringLiteral("1.21.1"), QStringLiteral("NeoForge"),
                                            true}
                      .cacheKey() ==
                  QStringLiteral("1.21.1|neoforge|client|all|1|"),
              "PH2 缓存键的格式固定（上下文 + 运行侧）");
    }

    section("PH3. 检查器：去重 / 本地优先 / 并发上限 / 旧一代回调");
    {
        ImmediateProvider provider;
        Presets::CompatibilityChecker checker;
        checker.setProvider(&provider);
        checker.setOptions({4, 5000, 10 * 60 * 1000});

        Presets::CompatibilityContext context;
        context.gameVersion = QStringLiteral("1.21.1");
        context.loader = QStringLiteral("fabric");

        QList<Presets::ModReference> references;
        references.append(makeRef(QStringLiteral("modrinth"), QStringLiteral("aaa"), false));
        references.append(makeRef(QStringLiteral("modrinth"), QStringLiteral("aaa"), true));
        references.append(makeRef(QStringLiteral("modrinth"), QStringLiteral("bbb")));
        references.append(makeRef(QStringLiteral("modrinth"), QStringLiteral("bad"), true,
                                  QString(), QStringLiteral("1.20.1")));
        references.append(makeRef(QString(), QStringLiteral("nope")));

        checker.start(references, context);
        check(waitForChecker(checker), "PH3 这一代收得了尾");

        check(provider.asked.size() == 2 &&
                  provider.asked.contains(QStringLiteral("modrinth:aaa")) &&
                  provider.asked.contains(QStringLiteral("modrinth:bbb")),
              "★★ PH3 同一项目只问一遍；本地判死的（钉死版本 / 坏引用）一条都不问",
              provider.asked.join(QStringLiteral(", ")));
        QSet<QString> distinct;
        for (const QString &key : provider.asked) {
            distinct.insert(key);
        }
        check(provider.asked.size() == distinct.size(),
              "PH3 问题清单本身没有重复");

        const QList<Presets::CompatibilityResult> results = checker.results();
        check(results.size() == 4, "★ PH3 重复引用合并成一条（4 条 ⇒ 3 条真项目 + 1 条坏引用）",
              QString::number(results.size()));
        bool dedupRequired = false;
        for (const Presets::CompatibilityResult &result : results) {
            if (result.key == QStringLiteral("modrinth:aaa")) {
                dedupRequired = result.required;
            }
        }
        check(dedupRequired, "PH3 同一项目出现两次时，必要项取「或」");
        const Presets::CompatibilitySummary summary = checker.summary();
        check(summary.matched == 2 && summary.missing == 1 && summary.invalid == 1 &&
                  summary.requiredMissing == 1,
              "PH3 汇总：2 匹配 / 1 缺失 / 1 引用无法识别 / 必要项缺失 1",
              summary.text());
    }

    // 并发上限：同一时刻最多两条在途。
    {
        SlowProvider provider;
        provider.delayMs = 20;
        Presets::CompatibilityChecker checker;
        checker.setProvider(&provider);
        checker.setOptions({2, 5000, 0});

        QList<Presets::ModReference> references;
        for (int i = 0; i < 8; ++i) {
            references.append(makeRef(QStringLiteral("modrinth"), QStringLiteral("p%1").arg(i)));
        }
        Presets::CompatibilityContext context;
        context.gameVersion = QStringLiteral("1.21.1");

        checker.start(references, context);
        check(waitForChecker(checker), "PH3 八条引用也收得了尾");
        check(provider.askedCount == 8, "PH3 每一条都问到了",
              QString::number(provider.askedCount));
        check(provider.maxSeen == 2,
              "★ PH3 在途封顶：8 条引用、上限 2 ⇒ 任何时刻不超过 2",
              QString::number(provider.maxSeen));
        check(checker.summary().matched == 8, "PH3 八条都匹配", checker.summary().text());
    }

    // generation：旧一代的迟到回调必须被丢掉。
    {
        ControlledProvider provider;
        Presets::CompatibilityChecker checker;
        checker.setProvider(&provider);
        checker.setOptions({4, 5000, 0});

        Presets::CompatibilityContext context;
        context.gameVersion = QStringLiteral("1.21.1");
        QList<Presets::ModReference> references;
        references.append(makeRef(QStringLiteral("modrinth"), QStringLiteral("abc")));

        checker.start(references, context);
        const int firstGeneration = checker.generation();
        check(provider.pending.size() == 1, "PH3 第一代发出一条查询");
        const Presets::CompatibilityProvider::Reply staleReply = provider.pending.takeFirst();

        checker.start(references, context);
        check(checker.generation() == firstGeneration + 1, "PH3 再 start 一次就是新一代");
        check(provider.pending.size() == 1, "PH3 新一代又发出一条查询");
        const Presets::CompatibilityProvider::Reply freshReply = provider.pending.takeFirst();

        staleReply(Presets::CompatibilityState::Matched, QStringLiteral("旧的"));
        QCoreApplication::processEvents();
        check(checker.summary().matched == 0 && checker.running(),
              "★★ PH3 旧 generation 的回调一个字都不许改",
              checker.summary().text());

        freshReply(Presets::CompatibilityState::Missing, QStringLiteral("新的"));
        check(waitForChecker(checker), "PH3 新一代收得了尾");
        check(checker.summary().missing == 1 && checker.results().value(0).detail ==
                                                    QStringLiteral("新的"),
              "PH3 新一代的结论才落得下来", checker.summary().text());
    }

    section("PH4. 失败不报缺失 / 缓存只收准话");
    {
        // 没装查询实现：每一项都是「暂无法确认」，不是缺失。
        Presets::CompatibilityChecker checker;
        checker.setOptions({4, 5000, 0});
        Presets::CompatibilityContext context;
        context.gameVersion = QStringLiteral("1.21.1");
        checker.start({makeRef(QStringLiteral("modrinth"), QStringLiteral("abc"))}, context);
        check(waitForChecker(checker), "PH4 没有查询实现也能收尾（弹窗不挂住）");
        check(checker.summary().unknown == 1 && checker.summary().missing == 0 &&
                  checker.summary().requiredMissing == 0,
              "★★ PH4 没有查询实现 ⇒ 暂无法确认，绝不是缺失", checker.summary().text());

        // 平台明确答「不知道」：同样不算缺失。
        ImmediateProvider provider;
        provider.answers.insert(QStringLiteral("modrinth:abc"),
                                Presets::CompatibilityState::Unknown);
        Presets::CompatibilityChecker unknownChecker;
        unknownChecker.setProvider(&provider);
        unknownChecker.setOptions({4, 5000, 0});
        unknownChecker.start({makeRef(QStringLiteral("modrinth"), QStringLiteral("abc"))},
                             context);
        check(waitForChecker(unknownChecker), "PH4 平台答「不知道」也收得了尾");
        check(unknownChecker.summary().unknown == 1 && unknownChecker.summary().missing == 0 &&
                  unknownChecker.summary().requiredMissing == 0,
              "★★ PH4 网络失败 / 429 ⇒ 暂无法确认，不计入缺失（那条验收）",
              unknownChecker.summary().text());
    }

    // 超时：到点按「暂无法确认」报，不重试。
    {
        SilentProvider provider;
        Presets::CompatibilityChecker checker;
        checker.setProvider(&provider);
        checker.setOptions({4, 30, 0});
        Presets::CompatibilityContext context;
        context.gameVersion = QStringLiteral("1.21.1");
        checker.start({makeRef(QStringLiteral("modrinth"), QStringLiteral("abc"))}, context);
        check(waitForChecker(checker), "PH4 超时也会收尾");
        const QList<Presets::CompatibilityResult> results = checker.results();
        check(results.size() == 1 &&
                  results.at(0).state == Presets::CompatibilityState::Unknown &&
                  results.at(0).detail.contains(QStringLiteral("超时")),
              "PH4 超时 = 暂无法确认（原因写着超时）",
              results.isEmpty() ? QString() : results.at(0).detail);
        check(checker.summary().requiredMissing == 0, "★ PH4 超时的必要项不算缺失");
    }

    // 缓存：命中不重问、只收准话、过期要重问。
    {
        Presets::CompatibilityCache cache(10 * 60 * 1000);
        ImmediateProvider provider;
        provider.answers.insert(QStringLiteral("modrinth:abc"),
                                Presets::CompatibilityState::Missing);
        Presets::CompatibilityChecker checker(&cache);
        checker.setProvider(&provider);
        checker.setOptions({4, 5000, 10 * 60 * 1000});
        Presets::CompatibilityContext context;
        context.gameVersion = QStringLiteral("1.21.1");

        checker.start({makeRef(QStringLiteral("modrinth"), QStringLiteral("abc"))}, context);
        check(waitForChecker(checker), "PH4 第一趟查得完");
        check(provider.asked.size() == 1 && checker.summary().missing == 1,
              "PH4 第一趟是问出来的", checker.summary().text());

        provider.asked.clear();
        checker.start({makeRef(QStringLiteral("modrinth"), QStringLiteral("abc"))}, context);
        check(waitForChecker(checker), "PH4 第二趟也收得尾");
        check(provider.asked.isEmpty(), "★★ PH4 缓存没过期 ⇒ 一条都不重问",
              provider.asked.join(QStringLiteral(", ")));
        check(checker.results().value(0).fromCache &&
                  checker.results().value(0).detail.contains(QStringLiteral("缓存结论")),
              "PH4 缓存结论要标出来", checker.results().value(0).detail);

        // 过期的条目不删（能看见上次的结论），但 stale = true。
        Presets::CompatibilityCache::Entry entry;
        bool stale = false;
        const QString key =
            Presets::CompatibilityCache::keyFor(makeRef(QStringLiteral("modrinth"),
                                                        QStringLiteral("abc")),
                                                context);
        check(cache.lookup(key, &entry, &stale) && !stale,
              "PH4 刚写进去的缓存是新鲜的");
        cache.insert(QStringLiteral("1.21.1||client#modrinth:old"),
                     Presets::CompatibilityState::Matched, QStringLiteral("旧结论"),
                     QDateTime::currentMSecsSinceEpoch() - 11 * 60 * 1000);
        check(cache.lookup(QStringLiteral("1.21.1||client#modrinth:old"), &entry, &stale) &&
                  stale,
              "PH4 过期条目不删，但 lookup 说它过期了");

        // Unknown 一个字都不许进缓存。
        const int before = cache.size();
        cache.insert(QStringLiteral("modrinth:u"), Presets::CompatibilityState::Unknown,
                     QStringLiteral("断网"));
        check(cache.size() == before, "★★ PH4 Unknown 不进缓存（断网不许变成十分钟的假结论）");
        check(!cache.lookup(QStringLiteral("1.21.1||client#modrinth:u"), &entry, nullptr),
              "PH4 拒收的条目真的不在表里");
    }

    section("PH4b. 查询取消、总截止和缓存上下文");
    {
        struct CancellableProvider : Presets::CompatibilityProvider {
            int started = 0, stopped = 0;
            void query(const Presets::ModReference &, const Presets::CompatibilityContext &, Reply) override {}
            Cancel queryCancelable(const Presets::ModReference &, const Presets::CompatibilityContext &, Reply) override {
                ++started; return [this] { ++stopped; };
            }
        } provider;
        Presets::CompatibilityContext context{QStringLiteral("1.21.1"), QStringLiteral("fabric"), true};
        {
            Presets::CompatibilityChecker checker;
            checker.setProvider(&provider);
            checker.setOptions({1, 5000, 0, 40});
            checker.start({makeRef("modrinth", "one"), makeRef("modrinth", "two")}, context);
            check(waitForChecker(checker) && checker.summary().unknown == 2 && provider.started == 1
                      && provider.stopped == 1 && checker.summary().requiredMissing == 0,
                  "total deadline cancels active work and marks queued items unknown without starting them");
            checker.start({makeRef("modrinth", "three")}, context);
        }
        check(provider.started == 2 && provider.stopped == 2,
              "destroying the checker cancels its outstanding provider work");
        const auto key = context.cacheKey();
        context.releaseFilter = "release";
        check(context.cacheKey() != key, "release filter isolates compatibility cache entries");
        const auto releaseKey = context.cacheKey();
        ++context.dataVersion;
        check(context.cacheKey() != releaseKey, "metadata schema version isolates compatibility cache entries");
        Presets::PlatformCompatibilityProvider network(QUrl("http://127.0.0.1:1"));
        network.setCurseForge(QUrl("http://127.0.0.1:2"), "first-test-key");
        const auto scope = network.cacheScope();
        network.setCurseForge(QUrl("http://127.0.0.1:2"), "second-test-key");
        check(scope != network.cacheScope() && !scope.contains("first-test-key"),
              "changing API credentials invalidates provider cache scope without embedding the credential");
    }
    {
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost), "compatibility retry cancellation fixture listens");
        int requests = 0, replies = 0, responseMode = 0;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &requests, &responseMode] {
                    socket->readAll();
                    if (socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true); ++requests;
                    const QByteArray body = responseMode == 1 ? "[{}]" : responseMode == 2 ? "{\"data\":{}}" : "";
                    socket->write(QByteArray(responseMode == 0 ? "HTTP/1.1 503 Unavailable" : "HTTP/1.1 200 OK")
                        + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        Presets::PlatformCompatibilityProvider provider(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())));
        const auto cancel = provider.queryCancelable(makeRef("modrinth", "retry"),
            {QStringLiteral("1.21.1"), QStringLiteral("fabric"), true},
            [&](auto, const QString &) { ++replies; });
        QEventLoop first;
        QTimer::singleShot(100, &first, &QEventLoop::quit); first.exec();
        cancel();
        QEventLoop retry;
        QTimer::singleShot(650, &retry, &QEventLoop::quit); retry.exec();
        check(requests == 1 && replies == 0,
              "cancelling during real HTTP 503 backoff prevents both retry traffic and completion callback");
        const QUrl base(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        provider.setCurseForge(base, "fixture-key");
        for (responseMode = 1; responseMode <= 2; ++responseMode) {
            QEventLoop loop; bool answered = false;
            Presets::CompatibilityState state = Presets::CompatibilityState::Pending;
            auto stop = provider.queryCancelable(makeRef(responseMode == 1 ? "modrinth" : "curseforge", "123"),
                {QStringLiteral("1.21.1"), QStringLiteral("fabric"), true},
                [&](auto value, const QString &) { state = value; answered = true; loop.quit(); });
            QTimer::singleShot(1000, &loop, &QEventLoop::quit); loop.exec(); stop();
            check(answered && state == Presets::CompatibilityState::Unknown,
                  "malformed nonempty version metadata is unknown, never matched or cached as missing");
        }
    }

    section("PH5. 模组引用进 manifest：往返 / 旧版仍读得回 / 类别对不上");
    {
        Presets::PresetManifest manifest;
        manifest.id = Presets::newPresetId();
        manifest.name = QStringLiteral("整合包 A");
        manifest.category = Presets::PresetCategory::Mods;
        manifest.modReferences.append(makeRef(QStringLiteral("Modrinth"),
                                              QStringLiteral("sodium"), true,
                                              QStringLiteral("Sodium")));
        manifest.modReferences.append(makeRef(QStringLiteral("curseforge"),
                                              QStringLiteral("238222"), false,
                                              QStringLiteral("JEI"),
                                              QStringLiteral("1.21.1"),
                                              QStringLiteral("forge")));

        const QJsonObject json = Presets::manifestToJson(manifest);
        check(json.value(QStringLiteral("modReferences")).isArray() &&
                  json.value(QStringLiteral("modReferences")).toArray().size() == 2,
              "PH5 两条引用都写进 JSON");

        const Presets::PresetManifest back =
            Presets::manifestFromJson(QJsonDocument(json).toJson(QJsonDocument::Indented));
        check(back.modReferences.size() == 2, "PH5 读得回来",
              QString::number(back.modReferences.size()));
        if (back.modReferences.size() == 2) {
            check(back.modReferences.at(0).platform == QStringLiteral("modrinth") &&
                      back.modReferences.at(0).projectId == QStringLiteral("sodium") &&
                      back.modReferences.at(0).name == QStringLiteral("Sodium") &&
                      back.modReferences.at(0).required,
                  "PH5 顺序 / 平台名归一 / 名字 / 必要项都还在");
            check(back.modReferences.at(1).projectId == QStringLiteral("238222") &&
                      !back.modReferences.at(1).required &&
                      back.modReferences.at(1).gameVersion == QStringLiteral("1.21.1") &&
                      back.modReferences.at(1).loader == QStringLiteral("forge"),
                  "PH5 钉死的版本 / 加载器 / 可选项原样往返");
        }

        // 旧版本写出来的 manifest 没有这一位：照旧读得回来，引用为空。
        QJsonObject legacy = json;
        legacy.remove(QStringLiteral("modReferences"));
        const Presets::PresetManifest old =
            Presets::manifestFromJson(QJsonDocument(legacy).toJson());
        check(old.modReferences.isEmpty() && old.category == Presets::PresetCategory::Mods,
              "PH5 没有 modReferences 的旧 manifest 照旧读得回");

        // 类别对不上：模组引用只属于「模组」这一类。
        Presets::PresetManifest wrong = manifest;
        wrong.category = Presets::PresetCategory::GameSettings;
        check(!Presets::categoryMismatch(wrong).isEmpty(),
              "PH5 「游戏设置」里夹着模组引用 ⇒ categoryMismatch 说出来",
              Presets::categoryMismatch(wrong));
        Presets::PresetManifest right = manifest;
        check(Presets::categoryMismatch(right).isEmpty(),
              "PH5 「模组」类里的引用是合法的");
    }

    section("PH6. 平台兼容查询共用额度：排队、取消、截止");
    {
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost), "PH6 夹具端口");
        int requests = 0;
        bool silent = false;  // 收下请求但一个字节都不回（测截止）
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &requests, &silent] {
                    socket->readAll();
                    if (socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);
                    ++requests;
                    if (silent) return;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n[]");
                    socket->disconnectFromHost();
                });
            }
        });
        Presets::PlatformCompatibilityProvider provider(
            QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())));
        const Presets::CompatibilityContext context{QStringLiteral("1.21.1"),
                                                    QStringLiteral("fabric"), true};
        const int baseline = RequestBudget::snapshot().control;

        // 把 Control 名额占满（"占满" = tryAcquire 再也拿不到，不假设此刻还剩几个）。
        QList<RequestBudget::Lease> holds;
        while (auto lease = RequestBudget::tryAcquire(RequestBudget::Lane::Control)) holds.append(lease);

        int answered = 0;
        Presets::CompatibilityState answer = Presets::CompatibilityState::Pending;
        QString detail;
        auto cancelQueued = provider.queryCancelable(
            makeRef(QStringLiteral("modrinth"), QStringLiteral("queued")), context,
            [&](Presets::CompatibilityState value, const QString &why) {
                answer = value; detail = why; ++answered;
            });
        {
            QEventLoop blocked;
            QTimer::singleShot(150, &blocked, &QEventLoop::quit);
            blocked.exec();
        }
        check(requests == 0 && answered == 0,
              "PH6 额度占满时这一发只排队：一个字节都没发、也不回调");

        // 放弃排队中的那一发 ⇒ 之后腾出名额也不会偷偷发出去。
        cancelQueued();
        holds.clear();
        {
            QEventLoop released;
            QTimer::singleShot(300, &released, &QEventLoop::quit);
            released.exec();
        }
        check(requests == 0 && answered == 0 && RequestBudget::snapshot().control == baseline,
              "★★ PH6 删掉排队中的作业 ⇒ 名额腾出来也不发请求、不回调");

        answered = 0;
        answer = Presets::CompatibilityState::Pending;
        auto cancelSent = provider.queryCancelable(
            makeRef(QStringLiteral("modrinth"), QStringLiteral("sent")), context,
            [&](Presets::CompatibilityState value, const QString &why) {
                answer = value; detail = why; ++answered;
            });
        {
            QEventLoop sent;
            QTimer::singleShot(1000, &sent, &QEventLoop::quit);
            sent.exec();
        }
        cancelSent();
        check(requests == 1 && answered == 1 && answer == Presets::CompatibilityState::Missing,
              "PH6 名额回来了这一发就发出去，空版本表照旧判「缺失」", detail);

        // 截止（排队 + 传输）到点：报「查询超时」，不是缺失，也不重发。
        silent = true;
        answered = 0;
        answer = Presets::CompatibilityState::Pending;
        provider.setTransferTimeoutMs(250);
        auto cancelSlow = provider.queryCancelable(
            makeRef(QStringLiteral("modrinth"), QStringLiteral("slow")), context,
            [&](Presets::CompatibilityState value, const QString &why) {
                answer = value; detail = why; ++answered;
            });
        {
            QEventLoop deadline;
            QTimer::singleShot(1500, &deadline, &QEventLoop::quit);
            deadline.exec();
        }
        cancelSlow();
        provider.setTransferTimeoutMs(12000);
        check(answered == 1 && answer == Presets::CompatibilityState::Unknown &&
                  detail.contains(QStringLiteral("超时")),
              "PH6 截止到点 ⇒ 暂无法确认（原因写着超时），不是缺失", detail);
        check(RequestBudget::snapshot().control == baseline,
              "★★ PH6 超时那一发把共用名额还回去了");
    }
}


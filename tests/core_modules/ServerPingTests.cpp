#include "Support.hpp"

void runServerPingTests() {
    section("ZH1. 地址怎么拆（主机名 / :端口 / [IPv6]:端口）");

    {
        const ServerPing::Address plain = ServerPing::splitAddress(QStringLiteral("example.com"));
        check(plain.valid && plain.host == QStringLiteral("example.com") &&
                  plain.port == ServerPing::kDefaultPort,
              "ZH1 不写端口 = 默认 25565",
              QStringLiteral("%1:%2").arg(plain.host).arg(plain.port));

        const ServerPing::Address withPort =
            ServerPing::splitAddress(QStringLiteral("  mc.hypixel.net:25566  "));
        check(withPort.valid && withPort.host == QStringLiteral("mc.hypixel.net") &&
                  withPort.port == 25566,
              "ZH1 两头空白去掉、端口照写",
              QStringLiteral("%1:%2").arg(withPort.host).arg(withPort.port));

        const ServerPing::Address v6 = ServerPing::splitAddress(QStringLiteral("[::1]:25566"));
        check(v6.valid && v6.host == QStringLiteral("::1") && v6.port == 25566,
              "ZH1 [IPv6]:端口 —— 方括号**要去掉**（Qt 的 connectToHost 收的是裸地址）",
              QStringLiteral("%1:%2").arg(v6.host).arg(v6.port));

        const ServerPing::Address v6NoPort = ServerPing::splitAddress(QStringLiteral("[::1]"));
        check(v6NoPort.valid && v6NoPort.host == QStringLiteral("::1") &&
                  v6NoPort.port == ServerPing::kDefaultPort,
              "ZH1 光一个 [IPv6] = 默认端口",
              QStringLiteral("%1:%2").arg(v6NoPort.host).arg(v6NoPort.port));

        // 这一条是这一节最值钱的一条反面：裸的 IPv6 按"最后一个冒号"拆出来是主机 `::`、
        // 端口 `1` —— 一个看着完全合法、其实谁也不是的地址，而且报错会变成"这台服务器连
        // 不上"，真正的原因（这行地址少了方括号）一个字都不会出现。
        const ServerPing::Address bareV6 = ServerPing::splitAddress(QStringLiteral("::1"));
        check(!bareV6.valid, "ZH1 没加方括号的 IPv6 = 看不懂（不猜成主机 ::、端口 1）",
              QStringLiteral("拆成了「%1」:%2").arg(bareV6.host).arg(bareV6.port));

        check(!ServerPing::splitAddress(QStringLiteral("[::1")).valid,
              "ZH1 方括号没关上 = 看不懂");
        check(!ServerPing::splitAddress(QStringLiteral(":25565")).valid,
              "ZH1 只有端口没主机 = 看不懂");
        check(!ServerPing::splitAddress(QStringLiteral("example.com:65536")).valid,
              "ZH1 端口 65536 = 看不懂（上限是 65535）");
        check(!ServerPing::splitAddress(QStringLiteral("example.com:0")).valid,
              "ZH1 端口 0 = 看不懂（**不当成默认端口**）");
        check(!ServerPing::splitAddress(QStringLiteral("example.com:minecraft")).valid,
              "ZH1 端口不是数字 = 看不懂");
        check(!ServerPing::splitAddress(QString()).valid &&
                  !ServerPing::splitAddress(QStringLiteral("   ")).valid,
              "ZH1 空串 / 只有空白 = 看不懂");

        const ServerPing::Address maxPort = ServerPing::splitAddress(QStringLiteral("example.com:65535"));
        check(maxPort.valid && maxPort.port == 65535, "ZH1 65535 是合法的（边界这一头）");

        const ServerPing::Address trailingColon = ServerPing::splitAddress(QStringLiteral("example.com:"));
        check(trailingColon.valid && trailingColon.port == ServerPing::kDefaultPort,
              "ZH1 末尾一个光秃秃的冒号 = 没写端口（不是看不懂）",
              QStringLiteral("%1:%2").arg(trailingColon.host).arg(trailingColon.port));
    }

    section("ZH1b. SRV：先按优先级、再按权重挑");

    {
        using Record = ServerPing::SrvRecord;
        check(!ServerPing::splitAddress(QStringLiteral("example.com")).portExplicit &&
                  ServerPing::splitAddress(QStringLiteral("example.com:25566")).portExplicit,
              "★ ZH1b 端口是不是**用户写出来的**分得出来（SRV 只在前者为假时才查）");

        const QList<Record> many = {
            {QStringLiteral("backup.example.com"), 25566, 10, 0},
            {QStringLiteral("mc.example.com"), 25565, 0, 0},
            {QStringLiteral("other.example.com"), 25567, 0, 0},
            {QString(), 25565, 0, 0},                            // 目标空 ⇒ 丢
            {QStringLiteral("bad-port.example.com"), 0, 0, 0},   // 端口 0 ⇒ 丢
        };
        const QList<Record> best = ServerPing::bestPriorityRecords(many);
        check(best.size() == 2 && best.at(0).priority == 0 && best.at(1).priority == 0,
              "★★ ZH1b 先丢掉用不了的，再只留**最小优先级**那一组（备份记录不许先连）",
              QString::number(best.size()));

        Record picked;
        check(ServerPing::pickSrvRecord(many, 0, &picked) &&
                  picked.target == QStringLiteral("mc.example.com"),
              "ZH1b 权重全 0 = 等概率（样本 0 拿同优先级里第一条）",
              picked.target);
        check(ServerPing::pickSrvRecord(many, 1, &picked) &&
                  picked.target == QStringLiteral("other.example.com"),
              "ZH1b 样本 1 拿第二条（`sample % 条数`）", picked.target);

        const QList<Record> weighted = {
            {QStringLiteral("a.example.com"), 25565, 0, 1},
            {QStringLiteral("b.example.com"), 25565, 0, 3},
        };
        check(ServerPing::pickSrvRecord(weighted, 0, &picked) &&
                  picked.target == QStringLiteral("a.example.com") &&
                  ServerPing::pickSrvRecord(weighted, 1, &picked) &&
                  picked.target == QStringLiteral("b.example.com") &&
                  ServerPing::pickSrvRecord(weighted, 3, &picked) &&
                  picked.target == QStringLiteral("b.example.com"),
              "★★ ZH1b 权重 1:3 ⇒ [0,1) 给 a、[1,4) 给 b（按累计区间，不是四舍五入成一半）");

        const QList<Record> fqdn = {{QStringLiteral("mc.example.com."), 25565, 0, 0}};
        check(ServerPing::pickSrvRecord(fqdn, 0, &picked) &&
                  picked.target == QStringLiteral("mc.example.com"),
              "ZH1b FQDN 末尾那个点去掉（那不是名字的一部分）", picked.target);
        const QList<Record> unavailable = {{QStringLiteral("."), 25565, 0, 0}};
        check(!ServerPing::pickSrvRecord(unavailable, 0, &picked),
              "★ ZH1b 目标 `.` = RFC 2782 那句「服务不可用」⇒ 不选（退回用户写的地址）");
        check(!ServerPing::pickSrvRecord(QList<Record>{}, 0, &picked), "ZH1b 空表 ⇒ 挑不出来");
    }

    section("ZH1c. 错误分类：哪一类没通（不拿那句中文去认）");

    {
        check(ServerPing::kindForSocketError(QAbstractSocket::ConnectionRefusedError) ==
                  ServerPing::Kind::Refused &&
                  ServerPing::kindForSocketError(QAbstractSocket::HostNotFoundError) ==
                  ServerPing::Kind::HostNotFound &&
                  ServerPing::kindForSocketError(QAbstractSocket::SocketTimeoutError) ==
                  ServerPing::Kind::Unreachable &&
                  ServerPing::kindForSocketError(QAbstractSocket::RemoteHostClosedError) ==
                  ServerPing::Kind::ConnectionLost &&
                  ServerPing::kindForSocketError(QAbstractSocket::SocketAccessError) ==
                  ServerPing::Kind::Other,
              "★★ ZH1c 五种 socket 错误各归各的类（`timedOut` 那一档仍由 isTimeoutError 判）");

        // 地址看不懂那一档连 socket 都还没用上 —— 它自己的类。
        check(ServerPing::resolve(QStringLiteral("::1")).valid == false,
              "ZH1c 看不懂的地址 ⇒ resolve 说 valid=false（不会去查 SRV、也不会去连）");
        const ServerPing::Resolution direct =
            ServerPing::resolve(QStringLiteral("127.0.0.1:25566"));
        check(direct.valid && !direct.srvTried &&
                  direct.connectHost == QStringLiteral("127.0.0.1") && direct.connectPort == 25566,
              "★★ ZH1c 写了端口 / IP 字面量 ⇒ **一趟 DNS 都不查**（离线也确定）");
        const ServerPing::Resolution noPort =
            ServerPing::resolve(QStringLiteral("localhost"));
        check(noPort.valid && !noPort.srvTried && noPort.connectPort == ServerPing::kDefaultPort,
              "ZH1c localhost 也不查 SRV（本机没有域名可言）");

        // 产品默认是开着的那一档（关掉是给离线环境留的口子）。
        check(ServerPing::srvLookupEnabled(), "ZH1c 出厂是开的（和游戏一样先查 SRV）");
    }

    section("ZH2. VarInt：协议里每一个数字的写法");

    {
        check(ServerPing::varIntBytes(0) == QByteArrayLiteral("\x00") &&
                  ServerPing::varIntBytes(127) == QByteArrayLiteral("\x7f"),
              "ZH2 0 和 127 都是一个字节");
        check(ServerPing::varIntBytes(128) == QByteArrayLiteral("\x80\x01"),
              "ZH2 128 是 0x80 0x01（一个字节放不下了）",
              ServerPing::varIntBytes(128).toHex(' '));
        check(ServerPing::varIntBytes(25565) == QByteArrayLiteral("\xdd\xc7\x01"),
              "ZH2 25565（默认端口那个数）是 DD C7 01",
              ServerPing::varIntBytes(25565).toHex(' '));
        // 负数：协议里我们唯一会发的负数是"我是什么版本"那个 -1（我不说），补码编出来是
        // 5 个字节 —— 少编一个字节的话服务器读到的就是别的数。
        check(ServerPing::varIntBytes(-1) == QByteArrayLiteral("\xff\xff\xff\xff\x0f"),
              "ZH2 -1 是 5 个字节（FF FF FF FF 0F）",
              ServerPing::varIntBytes(-1).toHex(' '));

        // 编了再读回来是同一个数 —— 负的也一样（协议里的 VarInt 就是 32 位补码）。这一条
        // 顺带把"5 个字节那个上限"从两头都压住了：2097151 正好 3 个字节、2097152 要 4 个。
        const int values[] = {0,     1,       127,    128,   25565,
                              2097151, 2097152, 2147483647, -1, -2147483648};
        bool allRoundTrip = true;
        QString firstBad;
        for (const int value : values) {
            const QByteArray bytes = ServerPing::varIntBytes(value);
            int offset = 0;
            int back = 12345;
            if (!ServerPing::readVarInt(bytes, &offset, &back) || back != value ||
                offset != bytes.size()) {
                allRoundTrip = false;
                if (firstBad.isEmpty()) {
                    firstBad = QStringLiteral("%1 变成 %2").arg(value).arg(back);
                }
            }
        }
        check(allRoundTrip, "ZH2 10 个数（含两头极端值）编了再读回来都是原来那个", firstBad);

        // 读不动的两种：**字节不够一个 VarInt**，和连着 5 个字节还带着"后面还有"那一位。
        // 两种都只是"现在读不了"，所以 offset 和 out 都不许动 —— 半推半就会让调用方拿着
        // 一个"读到一半的位置"去解下一个字段，那是最难查的一类错。
        const QByteArray partial = QByteArrayLiteral("\x80\x80");
        int offset = 0;
        int value = 12345;
        check(!ServerPing::readVarInt(partial, &offset, &value) && offset == 0 && value == 12345,
              "ZH2 半截 VarInt = 读不动，而且 offset 和 out 都不动");

        offset = 0;
        check(!ServerPing::readVarInt(QByteArrayLiteral("\x80\x80\x80\x80\x80\x80"), &offset, &value),
              "ZH2 连着 6 个 0x80 = 读不动（VarInt 最多 5 字节）");

        // 从中间读：包里每个字段都紧接在前一个后面，offset 是进出自。
        const QByteArray mid = QByteArrayLiteral("\xff") + ServerPing::varIntBytes(300);
        offset = 1;
        check(ServerPing::readVarInt(mid, &offset, &value) && value == 300 && offset == mid.size(),
              "ZH2 从 offset 处读，读完成功推到最后",
              QStringLiteral("值 %1、offset %2/%3").arg(value).arg(offset).arg(mid.size()));

        offset = 0;
        check(!ServerPing::readVarInt(mid, nullptr, &value) &&
                  !ServerPing::readVarInt(mid, &offset, nullptr),
              "ZH2 指针给 nullptr = 读不到（不是崩）");
    }

    section("ZH3. MOTD：三种 JSON 形态展平成一行，再把颜色码洗掉");

    {
        check(ServerPing::motdText(QJsonValue(QStringLiteral("一句纯文本"))) ==
                  QStringLiteral("一句纯文本"),
              "ZH3 description 就是一段字符串：原样");

        // 真机上最常见的形态：{text, extra:[…]}，而且 extra 里对象和字符串混着。
        QJsonObject mixed;
        mixed[QStringLiteral("text")] = QStringLiteral("前半");
        QJsonArray extra;
        QJsonObject tail;
        tail[QStringLiteral("text")] = QStringLiteral("后半");
        tail[QStringLiteral("color")] = QStringLiteral("red");
        extra.append(tail);
        extra.append(QStringLiteral("再来一段"));
        mixed[QStringLiteral("extra")] = extra;
        check(ServerPing::motdText(QJsonValue(mixed)) == QStringLiteral("前半后半再来一段"),
              "ZH3 {text, extra:[…]} 展平成一行（extra 里对象和字符串混着）",
              ServerPing::motdText(QJsonValue(mixed)));

        // extra 里还能再嵌 extra（真机上有这种）。
        QJsonObject innermost;
        innermost[QStringLiteral("text")] = QStringLiteral("里");
        QJsonObject middle;
        middle[QStringLiteral("text")] = QStringLiteral("中");
        middle[QStringLiteral("extra")] = QJsonArray{innermost};
        QJsonObject outermost;
        outermost[QStringLiteral("text")] = QStringLiteral("外");
        outermost[QStringLiteral("extra")] = QJsonArray{middle};
        check(ServerPing::motdText(QJsonValue(outermost)) == QStringLiteral("外中里"),
              "ZH3 extra 里嵌 extra 也一路递归下去");

        // 1.20.3 之后 description 也可能是数组。
        check(ServerPing::motdText(QJsonValue(QJsonArray{QStringLiteral("一"), QStringLiteral("二")})) ==
                  QStringLiteral("一二"),
              "ZH3 description 是数组（1.20.3+ 那种）也认");

        check(ServerPing::motdText(QJsonValue()) == QString() &&
                  ServerPing::motdText(QJsonValue(12)) == QString() &&
                  ServerPing::motdText(QJsonValue(QJsonObject())) == QString() &&
                  ServerPing::motdText(QJsonValue(QJsonObject{{QStringLiteral("color"),
                                                               QStringLiteral("red")}})) == QString(),
              "ZH3 没给 description / 给了个数字 / 空对象 / 只有颜色的对象 = 空串（不崩）");

        // 颜色码：`§` 是**两字符一对**。只丢 `§` 的话，屏幕上会留下一串孤零零的 a l r
        // —— 那正是"洗过了但没洗干净"的样子。
        const QString colored = QStringLiteral("§a欢迎 §l来到 §r这里");
        check(ServerPing::cleanMotd(colored) == QStringLiteral("欢迎 来到 这里"),
              "ZH3 §a / §l / §r 连着**后面那个字符**一起丢",
              ServerPing::cleanMotd(colored));
        check(ServerPing::cleanMotd(QStringLiteral("末尾一个§")) == QStringLiteral("末尾一个"),
              "ZH3 末尾孤零零一个 § 直接丢掉（不越界）");

        const QString multiline = QStringLiteral("  前面\n\n后面\t尾巴  ");
        check(ServerPing::cleanMotd(multiline) == QStringLiteral("前面 后面 尾巴"),
              "ZH3 换行/制表压成空格、连着几个空格收成一个、两头去掉",
              ServerPing::cleanMotd(multiline));
    }

    section("ZH4. 图标那串 data URL");

    QByteArray iconPng;
    {
        // 现造一张（不落盘，走 QBuffer）—— 要证的只是"这条路通"，去 meta/ 里挂一份反而绕。
        QImage source(64, 64, QImage::Format_ARGB32);
        source.fill(QColor(0x33, 0x66, 0x99));
        QBuffer buffer(&iconPng);
        buffer.open(QIODevice::WriteOnly);
        const bool saved = source.save(&buffer, "PNG");
        buffer.close();
        check(saved && iconPng.startsWith(QByteArrayLiteral("\x89PNG")),
              "ZH4 现造一张 64x64 的 PNG 当图标",
              QStringLiteral("%1 字节").arg(iconPng.size()));

        const QString base64 = QString::fromLatin1(iconPng.toBase64());
        check(ServerPing::decodeFavicon(QStringLiteral("data:image/png;base64,") + base64) == iconPng,
              "ZH4 data:image/png;base64,… 解出来和原来那张**逐字节一样**");

        check(ServerPing::decodeFavicon(QStringLiteral("data:image/jpeg;base64,") + base64).isEmpty() &&
                  ServerPing::decodeFavicon(QStringLiteral("https://example.com/icon.png")).isEmpty() &&
                  ServerPing::decodeFavicon(QStringLiteral("data:image/png;base64,")).isEmpty() &&
                  ServerPing::decodeFavicon(QString()).isEmpty(),
              "ZH4 别的形态一律回空（不猜）：jpeg 前缀 / 一条 http 地址 / 只有前缀 / 空串");
    }

    section("ZH5. 连不上的话怎么说（自己写的那张表）");

    {
        check(ServerPing::connectionErrorText(QAbstractSocket::ConnectionRefusedError)
                  .contains(QStringLiteral("连接被拒绝")),
              "ZH5 连接被拒 = 「连接被拒绝」",
              ServerPing::connectionErrorText(QAbstractSocket::ConnectionRefusedError));
        check(ServerPing::connectionErrorText(QAbstractSocket::HostNotFoundError)
                  .contains(QStringLiteral("找不到")),
              "ZH5 域名解析不了 = 「找不到主机」",
              ServerPing::connectionErrorText(QAbstractSocket::HostNotFoundError));
        check(ServerPing::connectionErrorText(QAbstractSocket::SocketTimeoutError)
                  .contains(QStringLiteral("超时")) &&
                  ServerPing::connectionErrorText(QAbstractSocket::NetworkError)
                      .contains(QStringLiteral("超时")),
              "ZH5 超时和网络错误合一句");
        check(ServerPing::connectionErrorText(QAbstractSocket::RemoteHostClosedError)
                  .contains(QStringLiteral("断开")),
              "ZH5 对面挂了 = 「连接已断开」");
        check(ServerPing::connectionErrorText(QAbstractSocket::SocketAccessError)
                  .contains(QStringLiteral("受限")),
              "ZH5 本机不让连 = 「连接受限」");
        check(ServerPing::connectionErrorText(QAbstractSocket::UnknownSocketError) ==
                  QStringLiteral("连接失败"),
              "ZH5 认不出来的错兜到一句「连接失败」");

        // 这一条钉的是"我们没有走到 Qt 的 errorString() 上"：那是 Qt 翻译文件里的英文
        // 句子，而我们的部署**不带**那些 .qm 文件（--no-translations），真机上会是一句
        // 英文 —— 用户看到的是"Connection refused"。
        const int codes[] = {QAbstractSocket::ConnectionRefusedError,
                             QAbstractSocket::HostNotFoundError,
                             QAbstractSocket::SocketTimeoutError,
                             QAbstractSocket::NetworkError,
                             QAbstractSocket::RemoteHostClosedError,
                             QAbstractSocket::SocketAccessError,
                             QAbstractSocket::UnknownSocketError};
        QString withLatin;
        for (const int code : codes) {
            const QString text = ServerPing::connectionErrorText(code);
            if (text.isEmpty()) {
                withLatin = QStringLiteral("空串");
                break;
            }
            for (const QChar ch : text) {
                if (ch.unicode() < 0x80 && ch.isLetter()) {
                    withLatin = text;
                    break;
                }
            }
            if (!withLatin.isEmpty()) {
                break;
            }
        }
        check(withLatin.isEmpty(), "ZH5 每一条都是中文（一个 ASCII 字母都没有）", withLatin);
    }

    section("ZH6. 真连一次：我们发的、服务器答的，逐字段核对");

    FakeStatusServer answer(FakeStatusServer::Answer);
    {
        // 一份"像真服务器答的" JSON：MOTD 是 {text, extra} 那种形态、带颜色码、带换行、
        // 带图标、带人数和版本名 —— 每一项都对应下面一条断言。
        QJsonObject motdText;
        motdText[QStringLiteral("text")] = QStringLiteral("§a欢迎 §l来到 ");
        QJsonArray extra;
        QJsonObject colored;
        colored[QStringLiteral("text")] = QStringLiteral("WTFML 服");
        colored[QStringLiteral("color")] = QStringLiteral("red");
        extra.append(colored);
        extra.append(QStringLiteral("\n第二行"));
        motdText[QStringLiteral("extra")] = extra;

        QJsonObject players;
        players[QStringLiteral("online")] = 12;
        players[QStringLiteral("max")] = 100;
        // 玩家名单：带 `§` 码、带重名、带空名，再加一长串 —— 一次把洗名字 / 去重 / 上限
        // 三件事都摆在真连一次里（上限是 32 个，见 ServerPing.cpp）。
        QJsonArray sample;
        sample.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("§aSteve")}});
        sample.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Alex")}});
        sample.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Steve")}});  // 重名
        sample.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("§r§l")}});   // 洗空了
        sample.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("  Bob  ")}});
        for (int i = 0; i < 40; ++i) {
            sample.append(
                QJsonObject{{QStringLiteral("name"), QStringLiteral("p%1").arg(i)}});
        }
        players[QStringLiteral("sample")] = sample;

        QJsonObject version;
        version[QStringLiteral("name")] = QStringLiteral("Paper 1.21.4");
        version[QStringLiteral("protocol")] = 769;

        QJsonObject root;
        root[QStringLiteral("description")] = motdText;
        root[QStringLiteral("players")] = players;
        root[QStringLiteral("version")] = version;
        root[QStringLiteral("favicon")] = QStringLiteral("data:image/png;base64,") +
                                           QString::fromLatin1(iconPng.toBase64());

        answer.json = QJsonDocument(root).toJson(QJsonDocument::Compact);
        answer.startAndWait();
        check(answer.listening && answer.port != 0, "ZH6 假服务器起来了",
              QStringLiteral("127.0.0.1:%1").arg(answer.port));

        QElapsedTimer wall;
        wall.start();
        const ServerPing::Status status =
            ServerPing::ping(QStringLiteral("127.0.0.1:%1").arg(answer.port));
        const qint64 wallMs = wall.elapsed();

        check(status.ok, "ZH6 这一趟**通了**", status.error);
        check(status.error.isEmpty(), "ZH6 通了的时候 error 是空串", status.error);
        check(status.latencyMs >= 0 && status.latencyMs <= wallMs,
              "ZH6 记下的耗时在 0 和这一趟真实耗时之间",
              QStringLiteral("记 %1 ms，外圈量到 %2 ms").arg(status.latencyMs).arg(wallMs));
        check(status.motd == QStringLiteral("欢迎 来到 WTFML 服 第二行"),
              "ZH6 MOTD：展平 + 洗完之后就是那两行字",
              QStringLiteral("「%1」").arg(status.motd));
        check(status.versionName == QStringLiteral("Paper 1.21.4"),
              "ZH6 服务器自报的版本名读出来了", status.versionName);
        check(status.playersOnline == 12 && status.playersMax == 100,
              "ZH6 人数 12/100",
              QStringLiteral("%1/%2").arg(status.playersOnline).arg(status.playersMax));
        // 名单：`§` 码洗掉、重名只留一次、空名跳过，最多 32 个（40 个 p 只进 29 个）。
        check(status.samplePlayers.size() == 32 &&
                  status.samplePlayers.mid(0, 3) ==
                      QStringList{QStringLiteral("Steve"), QStringLiteral("Alex"),
                                  QStringLiteral("Bob")} &&
                  status.samplePlayers.last() == QStringLiteral("p28"),
              "★★ ZH6 玩家名单洗过、去过重、封顶 32 个（名字是服务器给的，长度不能由它决定）",
              status.samplePlayers.mid(0, 4).join(QStringLiteral("、")) + QStringLiteral(" … 共 %1")
                  .arg(status.samplePlayers.size()));
        check(status.iconPng == iconPng, "ZH6 图标逐字节一样",
              QStringLiteral("%1 字节").arg(status.iconPng.size()));
        QImage iconImage;
        const bool iconDecoded = iconImage.loadFromData(status.iconPng);
        check(iconDecoded && iconImage.size() == QSize(64, 64),
              "ZH6 那串字节真解得成一张 64x64 的图（界面那一层拿到手就是这么用的）",
              QStringLiteral("%1x%2").arg(iconImage.width()).arg(iconImage.height()));

        waitFakeServer(answer);
        check(answer.connections == 1, "ZH6 服务器那边正好收到一个连接",
              QStringLiteral("%1 个").arg(answer.connections));

        // ---- 发出去的那两个包，按字段拆开核对 ----
        const QByteArray seen = answer.seen;
        int pos = 0;
        int packetLength = 0;
        bool shapeOk = ServerPing::readVarInt(seen, &pos, &packetLength) && packetLength > 0;
        const int bodyStart = pos;
        int packetId = -1;
        int protocol = 0;
        QString hostInHandshake;
        int portInHandshake = -1;
        int nextState = -1;
        if (shapeOk) {
            shapeOk = ServerPing::readVarInt(seen, &pos, &packetId) && packetId == 0x00;
        }
        if (shapeOk) {
            shapeOk = ServerPing::readVarInt(seen, &pos, &protocol);
        }
        if (shapeOk) {
            int hostLength = 0;
            shapeOk = ServerPing::readVarInt(seen, &pos, &hostLength) && hostLength >= 0 &&
                      pos + hostLength <= seen.size();
            if (shapeOk) {
                hostInHandshake = QString::fromUtf8(seen.mid(pos, hostLength));
                pos += hostLength;
            }
        }
        if (shapeOk) {
            shapeOk = pos + 2 <= seen.size();
            if (shapeOk) {
                portInHandshake = (static_cast<unsigned char>(seen.at(pos)) << 8) |
                                  static_cast<unsigned char>(seen.at(pos + 1));
                pos += 2;
            }
        }
        if (shapeOk) {
            shapeOk = ServerPing::readVarInt(seen, &pos, &nextState);
        }
        check(shapeOk, "ZH6 握手包拆得开（长度 / id 0x00 / 版本 / 主机 / 端口 / 下一状态）",
              QStringLiteral("收到 %1 字节").arg(seen.size()));
        check(pos == bodyStart + packetLength,
              "ZH6 握手包自己那个长度 = 它正文的真实长度",
              QStringLiteral("说 %1，实际 %2").arg(packetLength).arg(pos - bodyStart));
        check(protocol == -1,
              "ZH6 协议版本发的是 -1（我不说，让它照自己的版本答）",
              QString::number(protocol));
        check(hostInHandshake == QStringLiteral("127.0.0.1"),
              "ZH6 握手里的主机名就是我们要连的那个", hostInHandshake);
        // 端口在协议里是**大端 uint16**，不是 VarInt —— 写成 VarInt 的话 25565 会变成
        // 三个字节，服务器读这个字段时把后面那些字节也吃进去，整个包就错位了。
        check(portInHandshake == answer.port, "ZH6 端口是大端 uint16（不是 VarInt）",
              QStringLiteral("收到 %1，期望 %2").arg(portInHandshake).arg(answer.port));
        check(nextState == 1,
              "ZH6 下一状态是 1（status）—— 写成 2 就是「我要进游戏」，服务器会等一个永远"
              "不来的登录包，一个字节都不答",
              QString::number(nextState));

        const int requestStart = pos;
        int requestLength = 0;
        int requestId = -1;
        const bool requestOk = ServerPing::readVarInt(seen, &pos, &requestLength) &&
                               ServerPing::readVarInt(seen, &pos, &requestId) &&
                               requestLength == 1 && requestId == 0x00;
        check(requestOk, "ZH6 紧跟着的是状态请求（长度 1、id 0x00、空正文）",
              QStringLiteral("长度 %1、id %2").arg(requestLength).arg(requestId));
        check(pos == seen.size(),
              "ZH6 两个包之后我们**一个字节都没多发**（只读，不往服务器写别的）",
              QStringLiteral("收到 %1 字节，用完 %2（状态请求从 %3 开始）")
                  .arg(seen.size())
                  .arg(pos)
                  .arg(requestStart));
    }

    section("ZH7. 连不上的样子（端口没人听 / 地址看不懂）");

    {
        quint16 freePort = 0;
        {
            QTcpServer probe;
            if (probe.listen(QHostAddress::LocalHost, 0)) {
                freePort = probe.serverPort();
                probe.close();
            }
        }
        check(freePort != 0, "ZH7 拿到一个刚放开的端口", QString::number(freePort));

        QElapsedTimer refusedWall;
        refusedWall.start();
        const ServerPing::Status refused =
            ServerPing::ping(QStringLiteral("127.0.0.1:%1").arg(freePort));
        const qint64 refusedMs = refusedWall.elapsed();

        check(!refused.ok, "ZH7 对面没开 = 没通",
              QStringLiteral("「%1」，用了 %2 ms").arg(refused.error).arg(refusedMs));
        // 这一条**只**钉"报的是一句人话"：这台机器上连一个没人听的端口回来的是「超时」而
        // 不是「连接被拒绝」（连接请求像是被本机的某个过滤器吃掉而不是被回了一个 RST，所以
        // 等满了那 4 秒）。两句都是人话，产品这一侧要保证的是"不是 ok、也不是一句英文"；
        // 具体哪个 socket 错误翻哪句话，由 ZH5 那张表逐条钉。写成非「连接被拒绝」不可的话，
        // 换一台机器跑就是红的，而那不是回归。
        check(refused.error.contains(QStringLiteral("连接被拒绝")) ||
                  refused.error.contains(QStringLiteral("超时")) ||
                  refused.error.contains(QStringLiteral("断开")),
              "ZH7 报出来的是一句人话（连接被拒绝 / 超时 / 连接已断开）",
              QStringLiteral("「%1」").arg(refused.error));
        // `timedOut` 那一位和那句话**说的是同一件事**（界面上就是照这一位把那一格缩成
        // 「超时」两个字的，见 ServerRowStatus::timedOut）。这一台机器上"连一个刚放开的
        // 端口"未必报超时（见上面那段 NOTE），所以这里钉的是**两者一致**，不是"必须报超时"。
        check(refused.timedOut == refused.error.contains(QStringLiteral("超时")),
              "★ ZH7 `timedOut` 和那句话说的是同一件事（界面上照着它缩成「超时」）",
              QStringLiteral("timedOut=%1、话是「%2」")
                  .arg(refused.timedOut ? QStringLiteral("真") : QStringLiteral("假"),
                       refused.error));
        check(refused.latencyMs < 0 && refused.motd.isEmpty() && refused.iconPng.isEmpty() &&
                  refused.playersOnline == -1,
              "ZH7 没通的时候：耗时 -1、MOTD 和图标都是空、人数还是 -1（不是半份结果）");

        // 地址看不懂那一档**一个字节都不往网上发**：不这么写的话，用户把地址打错了会得到
        // "这台服务器连不上"，而真正的原因（这行地址本身是坏的）一个字都看不见。
        const ServerPing::Status broken = ServerPing::ping(QStringLiteral("127.0.0.1:0"));
        check(!broken.ok && broken.error.contains(QStringLiteral("地址格式无效")),
              "ZH7 地址看不懂 = 直接说地址有问题", broken.error);
    }

    section("ZH8. 连上了但没人回答：超时那一档（顺带量一次那个旋钮）");

    {
        // 那个旋钮的下限：0 会让每一次 waitFor* 立刻返回，"连不上"和"连上了但还没答"就
        // 分不开了，所以钳到 1。
        const int saved = ServerPing::timeoutMs();
        ServerPing::setTimeoutMs(0);
        check(ServerPing::timeoutMs() == 1, "ZH8 超时设成 0 会被钳到 1 毫秒",
              QString::number(ServerPing::timeoutMs()));

        FakeStatusServer silent(FakeStatusServer::Silent);
        silent.startAndWait();
        check(silent.listening, "ZH8 那台「收下不回」的服务器起来了",
              QStringLiteral("127.0.0.1:%1").arg(silent.port));

        ServerPing::setTimeoutMs(200);
        QElapsedTimer wall;
        wall.start();
        const ServerPing::Status status =
            ServerPing::ping(QStringLiteral("127.0.0.1:%1").arg(silent.port));
        const qint64 wallMs = wall.elapsed();
        ServerPing::setTimeoutMs(saved);

        // 这一档必须说成"连上了但它没答话"，**不能**说成"网络不通"：连接明明建起来了，
        // 说网络不通会把人往查网线的方向带。Qt 在 waitForReadyRead 超时时也会把 error()
        // 设成 SocketTimeoutError，所以产品那一侧是**按走到哪一步**分的话术，不是照 error()
        // 翻表 —— 这条断言钉的就是那个区分。
        check(!status.ok && status.error.contains(QStringLiteral("超时")) &&
                  status.error.contains(QStringLiteral("没有响应")),
              "ZH8 没人回答 = 「已连接，但服务器没有响应（等待超时）」",
              QStringLiteral("「%1」").arg(status.error));
        // 界面上那一格对超时只写两个字（用户 ：「超时的只写超时」），判据是
        // 这一位 —— 所以它必须是**真的判定出来的**，而不是界面上拿 error 那句话的前缀猜的。
        // 上面那一句留着全话、这一位是"缩成两个字"的依据，两条缺一不可。
        check(status.timedOut,
              "★ ZH8 这一档的 `timedOut` 是**真**的（界面上据此只写「超时」两个字）");
        check(silent.connections == 1,
              "ZH8 对面**确实收到了这个连接**（不是连都没连上）",
              QStringLiteral("%1 个连接").arg(silent.connections));
        check(!silent.seen.isEmpty(),
              "ZH8 两个包在超时之前就发出去了（不是压根没发）",
              QStringLiteral("%1 字节").arg(silent.seen.size()));
        check(wallMs >= 180 && wallMs < 3000,
              "ZH8 200 毫秒那个旋钮真的管用（没等满默认的 4 秒、也不是压根没等）",
              QStringLiteral("实际等了 %1 ms").arg(wallMs));
        check(ServerPing::timeoutMs() == saved, "ZH8 旋钮换回来了（后面那些节用的是默认值）",
              QStringLiteral("%1 ms").arg(ServerPing::timeoutMs()));

        waitFakeServer(silent);
    }

    section("ZH9. 服务器答得不对的四种样子");

    {
        FakeStatusServer garbage(FakeStatusServer::Garbage);
        garbage.json = QByteArrayLiteral("<html>502 Bad Gateway</html>");
        garbage.startAndWait();
        const ServerPing::Status bad =
            ServerPing::ping(QStringLiteral("127.0.0.1:%1").arg(garbage.port));
        check(!bad.ok && bad.error.contains(QStringLiteral("不是一段 JSON")),
              "ZH9 正文不是 JSON（比如中间隔了个网关）", QStringLiteral("「%1」").arg(bad.error));
        waitFakeServer(garbage);

        // 长度前缀报一个天文数字：**看到这个数就该放弃**，不是等一堆永远不来的字节。
        FakeStatusServer huge(FakeStatusServer::HugeLength);
        huge.startAndWait();
        QElapsedTimer hugeWall;
        hugeWall.start();
        const ServerPing::Status oversized =
            ServerPing::ping(QStringLiteral("127.0.0.1:%1").arg(huge.port));
        const qint64 hugeMs = hugeWall.elapsed();
        check(!oversized.ok && oversized.error.contains(QStringLiteral("响应长度无效")),
              "ZH9 长度前缀说这个包有 4 MiB = 「长度不对」",
              QStringLiteral("「%1」").arg(oversized.error));
        check(hugeMs < 1500,
              "ZH9 而且是**当场**放弃的（没有等它 4 秒超时）",
              QStringLiteral("用了 %1 ms").arg(hugeMs));
        waitFakeServer(huge);

        FakeStatusServer notStatus(FakeStatusServer::NotStatus);
        notStatus.json = QByteArrayLiteral("{}");
        notStatus.startAndWait();
        const ServerPing::Status wrongId =
            ServerPing::ping(QStringLiteral("127.0.0.1:%1").arg(notStatus.port));
        check(!wrongId.ok && wrongId.error.contains(QStringLiteral("不是状态包")),
              "ZH9 包 id 不是 0x00 = 「不是状态包」",
              QStringLiteral("「%1」").arg(wrongId.error));
        waitFakeServer(notStatus);

        FakeStatusServer shortBody(FakeStatusServer::ShortBody);
        shortBody.json = QByteArrayLiteral("{}");
        shortBody.startAndWait();
        const ServerPing::Status truncated =
            ServerPing::ping(QStringLiteral("127.0.0.1:%1").arg(shortBody.port));
        check(!truncated.ok && truncated.error.contains(QStringLiteral("正文长度无效")),
              "ZH9 包是收全了，但里面说正文有 9999 字节 = 「正文长度不对」",
              QStringLiteral("「%1」").arg(truncated.error));
        waitFakeServer(shortBody);
    }

    section("ZH10. 几台一起问（并发）：三台假服务器互相等，串行必红");
    // 【这一节要证明的事】用户 ：「能多线程一起 ping 吗，一个个 ping 效率有点
    // 低」。判据不能是"量一遍耗时"——那是**环境说了算**的（本机连一个没人听的回环端口
    // 要等满整个超时，ZH7 实测过），机器一忙就会假红。所以这里换成一个**只能靠并发通过**
    // 的构造：三台假服务器**互相等**，每一台都要等到另外两台也连上来才回话。
    //
    // 并发跑（产品的样子）：三台几乎同时连上 → 闸门一开 → 三台一起回话 → 三条都通；
    // 串行跑（改回去的样子）：第 1 台等到闸门超时、一个字都不回 → 断在它这儿；后面两台
    // 根本没人来连。于是这一节**必红**，一点都不含糊。
    //
    // 顺带把"结果按 index 认人"也钉住：并发之后 oneFinished 回来的次序**不再等于** 0、1、2，
    // 接的人要是按"先后"去推，屏幕上会出现"这台的 MOTD 挂在那一台底下"。每台把自己的号
    // 写在 MOTD 里，于是张冠李戴是一条能红的断言。
    {
        const int savedTimeout = ServerPing::timeoutMs();
        check(ServerPing::kMaxConcurrentPings > 1,
              "★ ZH10 并发上限不止 1（不然下面那三条是白写的）",
              QStringLiteral("kMaxConcurrentPings=%1").arg(ServerPing::kMaxConcurrentPings));

        GateBarrier gate(3);
        QList<GatedStatusServer *> servers;
        QStringList addresses;
        for (int i = 0; i < 3; ++i) {
            auto *server = new GatedStatusServer(&gate, i);
            server->startAndWait();
            servers.append(server);
            addresses.append(QStringLiteral("127.0.0.1:%1").arg(server->port));
        }
        check(servers.at(0)->listening && servers.at(1)->listening && servers.at(2)->listening,
              "ZH10 三台假服务器都起来了");

        QHash<int, ServerPing::Status> results;
        bool finished = false;
        ServerPing::Pinger pinger;
        // 两个信号都是**从工作线程发出来的**，所以下面必须转主线程的事件循环才收得到
        // ——这一步顺带也就把"跨线程排队投递真的通了"验了（Status 没登记元类型的话，
        // 这里会一条都收不到，而那是**静默**的）。
        //
        // 这两个 connect **必须带第三个参数那个上下文对象**（这里就是 `&pinger`）。
        // 不带的话走的是那个三参数的 functor 重载，它内部写死的是 **Qt::DirectConnection**
        // —— 于是 lambda 会在**工作线程**上跑，往主线程这两个变量上写。那样写出来的
        // 测试是**碰运气**绿的：主线程那个 while 有时看得见、有时看不见（这里真踩到过：
        // 同一份代码一次全绿、下一次这一条红）。带上上下文之后按 AutoConnection 走，
        // 发送方在工作线程、接收方在主线程 ⇒ 排队投递，正好也就是产品那边走的那条路。
        QObject::connect(&pinger, &ServerPing::Pinger::oneFinished, &pinger,
                         [&results](int index, const ServerPing::Status &status) {
                             results.insert(index, status);
                         });
        // 收尾那一发到的时候**手上已经有几条**：这是"allFinished 真的是最后一条线程发的"
        // 那件事的判据（见下面那条 ★★）。要抓的那个方向写反是 `QAtomicInt::deref()` ——
        // 它等价于 `--i != 0`（减完**还有人在**就回真），于是"最先收摊的那条线程"抢先把
        // allFinished 发出去，接的人当这一趟完了：屏幕上只有最先回来的那一台有结果。
        int arrivedAtFinish = -1;
        int finishCount = 0;
        QObject::connect(&pinger, &ServerPing::Pinger::allFinished, &pinger,
                         [&finished, &results, &arrivedAtFinish, &finishCount]() {
                             arrivedAtFinish = results.size();
                             ++finishCount;
                             finished = true;
                         });

        QElapsedTimer clock;
        clock.start();
        pinger.start(addresses);
        while (!finished && clock.elapsed() < 20000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }

        check(finished, "★ ZH10 这一趟跑完了（allFinished 从工作线程投递到了主线程）");
        check(results.size() == 3, "ZH10 三台各回了一条结果",
              QStringLiteral("%1 条").arg(results.size()));

        QStringList failed;
        for (int i = 0; i < 3; ++i) {
            const ServerPing::Status status = results.value(i);
            if (!status.ok) {
                failed.append(QStringLiteral("第 %1 台：%2").arg(i).arg(status.error));
            }
        }
        check(failed.isEmpty(),
              "★★ ZH10 三台是**同时在问**的：每一台都拿回了答案"
              "（这三台要等到大家一起连上来才回话 —— 串行跑的话第 1 台永远等不来第 2 台，"
              "它一个字都不会回，这一条必红）",
              failed.join(QStringLiteral(" / ")));

        QStringList mismatched;
        for (int i = 0; i < 3; ++i) {
            const QString expected = QStringLiteral("gate-%1").arg(i);
            if (results.value(i).motd != expected) {
                mismatched.append(QStringLiteral("第 %1 条收到「%2」")
                                      .arg(i)
                                      .arg(results.value(i).motd));
            }
        }
        check(mismatched.isEmpty(),
              "★★ ZH10 而且**按 index 认人是对的**：第 i 条拿回的正是第 i 台那句话"
              "（并发之后回来的次序不再等于下标次序，按先后去推就会张冠李戴）",
              mismatched.join(QStringLiteral(" / ")));
        check(arrivedAtFinish == 3,
              "★★ ZH10 收尾那一发是**最后一条线程**发的：allFinished 到的时候三台的答复都"
              "已经在手上了（那一位的意思是「这一趟完了」，早发出去的话接的人以为跑完了 ——"
              " 症状是屏幕上只有最先回来的那一台有结果，其余几台永远停在「还没测」，而按钮"
              "已经变回「测速」、看着像跑完了）",
              QStringLiteral("allFinished 到的时候手上是 %1 条").arg(arrivedAtFinish));
        check(finishCount == 1,
              "★ ZH10 而 allFinished 只发一次（几条线程各发一次的话，接的人会以为跑完好几趟"
              " —— 这一次是三条线程抢着收尾）",
              QStringLiteral("发了 %1 次").arg(finishCount));
        check(gate.arrived() == 3, "ZH10 三台都真的报到了（字面意思上的「一起」）",
              QStringLiteral("到了 %1 台").arg(gate.arrived()));

        for (GatedStatusServer *server : servers) {
            if (!server->wait(8000)) {
                check(false, "ZH10 假服务器那条线程没停下来（最多等 8 秒）");
                server->terminate();
                server->wait(3000);
            }
            delete server;
        }
        ServerPing::setTimeoutMs(savedTimeout);
    }

    section("ZH11. 叫停之后马上再测一趟（上一趟的线程还在超时里）");

    {
        // 这一条钉的是一次**进程级的崩**（不是某个数字不对）：`abort()` 只放下
        // `m_running`，每一条线程手上那一台还在同步 socket 的超时里；「取消」会把
        // 「测速」那颗按钮重新点亮，用户立刻再点一次 —— 新的 `start()` 不能把这些
        // **还在跑**的 QThread `deleteLater()` 掉：主线程处理那条 DeferredDelete 时
        // Qt 直接 `qFatal("QThread: Destroyed while thread is still running")`，
        // 整个启动器当场消失。判据就是"这条测试进程还活着、第二趟照常收尾"。
        const int saved = ServerPing::timeoutMs();
        ServerPing::setTimeoutMs(600);
        FakeStatusServer silent(FakeStatusServer::Silent);
        silent.startAndWait();
        check(silent.listening, "ZH11 那台「收下不回」的服务器起来了",
              QStringLiteral("127.0.0.1:%1").arg(silent.port));

        int oneFinishedCount = 0;
        int finishCount = 0;
        bool lastTimedOut = false;
        ServerPing::Pinger pinger;
        QObject::connect(&pinger, &ServerPing::Pinger::oneFinished, &pinger,
                         [&oneFinishedCount, &lastTimedOut](int, const ServerPing::Status &status) {
                             ++oneFinishedCount;
                             lastTimedOut = status.timedOut;
                         });
        QObject::connect(&pinger, &ServerPing::Pinger::allFinished, &pinger,
                         [&finishCount] { ++finishCount; });

        const QStringList addresses{QStringLiteral("127.0.0.1:%1").arg(silent.port)};
        QElapsedTimer sinceStart;
        sinceStart.start();
        pinger.start(addresses);
        // 先等它真的进到 ping() 里（不是"还没被调度到"）：那一步要等 socket 超时。
        while (sinceStart.elapsed() < 150) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        pinger.abort();
        // 上一趟那条线程这会儿还卡在超时里。马上再开一趟（就是用户连点那一下）。
        pinger.start(addresses);

        QElapsedTimer clock;
        clock.start();
        while (finishCount == 0 && clock.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(finishCount == 1, "★ ZH11 第二趟照常跑完（测试进程没被 Qt 掐掉）",
              QStringLiteral("allFinished 发了 %1 次").arg(finishCount));
        // 再转一会儿，让上一趟那条线程真的收摊（它自己也快到 600 ms 超时了）。
        // 它收摊时**不该**再补发一条 allFinished / 报上一条作废的结果 —— 那会把新一趟
        // 的忙态和同一格结果一起打掉。
        const qint64 tail = clock.elapsed();
        while (clock.elapsed() < tail + 1200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(finishCount == 1,
              "★★ ZH11 上一趟那条线程晚到收摊时**没有**补发 allFinished"
              "（补发的话新一趟会被当成已经跑完）",
              QStringLiteral("allFinished 发了 %1 次").arg(finishCount));
        check(oneFinishedCount == 1,
              "★ ZH11 上一趟那条作废的结果也没有报上来（它会顶掉新一趟同一格）",
              QStringLiteral("oneFinished 发了 %1 次").arg(oneFinishedCount));
        check(lastTimedOut, "ZH11 新一趟拿回的仍然是「超时」那一档");

        // 假服务器那一趟会守着端口最多 4 秒（见 FakeStatusServer 里 Silent 那一段），
        // 这儿必须等它收摊 —— 一个还在跑的 QThread 被析构会把整个测试进程带崩，
        // 后面几节一条都跑不到。
        waitFakeServer(silent);
        ServerPing::setTimeoutMs(saved);
    }
}


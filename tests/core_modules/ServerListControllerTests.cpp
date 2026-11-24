#include "Support.hpp"

void runServerListControllerTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "ZK17 建得出一个临时数据根目录来摆夹具");
    if (!root.isValid()) {
        return;  // 还没切过数据根，不用还原
    }
    AppPaths::setMinecraftDir(root.path());

    // 三条夹具，和 ZK0 那份同一形状。第三条**没有 name** —— "认值不认下标"这件事在"删"
    // 这一边也照样得成立（界面上它那一行是空名字）。
    const QList<NbtServerItem> fixture{
        NbtServerItem{QStringLiteral("A服"), QStringLiteral("a.example.com:25565")},
        NbtServerItem{QStringLiteral("B服"), QStringLiteral("b.example.com")},
        NbtServerItem{QString(), QStringLiteral("c.example.com"), false},
    };

    /// 假 Host：把"只有主窗口才能做的事"换成四本账。每一节现建一个，账目不会串。
    struct FakeHost {
        QList<ServerListController::Ask> asks;  ///< 问过哪几格（按问的次序）
        QStringList initials;                   ///< 每一问的预填
        QStringList answers;                    ///< 每一问的回答（按"第几问"给，0 起）
        int cancelAt = -1;                      ///< 第几问取消；-1 = 一路答完
        bool confirmAnswer = true;              ///< 「删这一台吗」怎么答
        QStringList confirmations;              ///< 那句问话（`名字@地址`）
        QList<int> manyConfirmations;           ///< 批量删除那句问话里的台数
        QList<ServerListController::Notice> notices;
        QStringList logs;
        QStringList reloads;

        ServerListController::Host make() {
            ServerListController::Host host;
            host.ask = [this](ServerListController::Ask kind, const QString &initial,
                              QString *out) {
                const int index = asks.size();
                asks.append(kind);
                initials.append(initial);
                if (index == cancelAt) {
                    return false;
                }
                if (out != nullptr) {
                    *out = index < answers.size() ? answers.at(index) : QString();
                }
                return true;
            };
            host.confirmRemove = [this](const QString &who, const QString &address) {
                confirmations.append(who + QStringLiteral("@") + address);
                return confirmAnswer;
            };
            host.confirmRemoveMany = [this](int count) {
                manyConfirmations.append(count);
                return confirmAnswer;
            };
            host.notice = [this](const ServerListController::Notice &notice) {
                notices.append(notice);
            };
            host.log = [this](const QString &text) { logs.append(text); };
            host.changed = [this](const QString &id) { reloads.append(id); };
            return host;
        }
    };

    const QString instanceId = QStringLiteral("1.20.1-control");
    const QString datPath = ServerListFile::pathFor(instanceId);
    const auto rowsOf = [&]() {
        QList<NbtUtil::ServerEntry> rows;
        ServerListFile::read(datPath, &rows, nullptr);
        QStringList out;
        for (const NbtUtil::ServerEntry &row : rows) {
            out.append(row.name + QStringLiteral("|") + row.address);
        }
        return out;
    };
    const auto seed = [&]() { return writeBytesAt(datPath, nbtServersDat(fixture)); };

    section("ZK17. 前提：那一份在 <数据根>/versions/<id>/ 下，一开始根本没有");
    check(AppPaths::minecraftDir() == QDir::cleanPath(root.path()),
          "ZK17 数据根已切到临时目录（这一节读的写的是它，不是用户盘上那一份）",
          AppPaths::minecraftDir());
    check(datPath == AppPaths::versionDir(instanceId) + QStringLiteral("/servers.dat"),
          "ZK17 认的那一份就在这个实例自己的版本目录里（和读侧同一处拼的）", datPath);
    QDir().mkpath(QFileInfo(datPath).absolutePath());
    check(!QFileInfo::exists(datPath),
          "ZK17 这一趟说的是「这个实例第一次碰服务器」：盘上本来没有这一份");
    check(seed(), "ZK17 夹具落盘了");
    check(rowsOf() ==
              QStringList({QStringLiteral("A服|a.example.com:25565"),
                           QStringLiteral("B服|b.example.com"),
                           QStringLiteral("|c.example.com")}),
          "ZK17 三条都读得出来（第三条名字是空串）", rowsOf().join(QStringLiteral(" / ")));

    section("ZK18. 「（无名）」只有一处出处");
    check(ServerListController::displayName(QString()) == QStringLiteral("（无名）"),
          "ZK18 空名字 ⇒ 「（无名）」");
    check(ServerListController::displayName(QStringLiteral("A服")) == QStringLiteral("A服"),
          "ZK18 有名字的原样回（不套括号、不加前后缀）");
    check(ServerListController::displayName(QStringLiteral(" ")) == QStringLiteral(" "),
          "ZK18 只有空白**不算**没名字 —— 它不替用户裁那一格（裁了就写不回原样）");

    section("ZK19. 改一台：两样都没变 ⇒ 一个字都不写");
    {
        FakeHost fake;
        ServerListController controller(fake.make());
        const QByteArray before = readBytesAt(datPath);
        fake.answers = {QStringLiteral("A服"), QStringLiteral("a.example.com:25565")};
        check(!controller.edit(instanceId, QStringLiteral("A服"),
                               QStringLiteral("a.example.com:25565")),
              "ZK19 名字和地址一个字都没改 ⇒ 回头（返回 false）");
        check(readBytesAt(datPath) == before,
              "ZK19 空转那一趟盘上**逐字节**没动（mtime 都不该被推新、gzip 不该重压一遍）");
        check(fake.logs.isEmpty() && fake.notices.isEmpty() && fake.reloads.isEmpty(),
              "ZK19 没记日志、没弹、没喊重读 —— 对用户来说什么都没发生");
        check(fake.asks.size() == 2 &&
                  fake.asks.at(0) == ServerListController::Ask::EditName &&
                  fake.asks.at(1) == ServerListController::Ask::EditAddress,
              "ZK19 两句还是都问了（先说名字、再报地址）—— 挡在落盘那一步，不是挡在问话之前");
        check(fake.initials == QStringList({QStringLiteral("A服"),
                                            QStringLiteral("a.example.com:25565")}),
              "ZK19 两句的预填都是现成的值", fake.initials.join(QStringLiteral(" / ")));
    }

    section("ZK20. 改一台：第二句取消 ⇒ 还是不写");
    {
        FakeHost fake;
        ServerListController controller(fake.make());
        const QByteArray before = readBytesAt(datPath);
        fake.answers = {QStringLiteral("新名字")};
        fake.cancelAt = 1;
        check(!controller.edit(instanceId, QStringLiteral("A服"),
                               QStringLiteral("a.example.com:25565")),
              "ZK20 地址那一句取消 ⇒ 回头");
        check(readBytesAt(datPath) == before, "ZK20 取消之后盘上逐字节没动");
        check(fake.asks.size() == 2,
              "ZK20 取消发生在那两句之后（名字那一句已经问过、也已经答了）");
        check(fake.logs.isEmpty() && fake.notices.isEmpty() && fake.reloads.isEmpty(),
              "ZK20 半截操作不许留痕：没日志、没弹、没重读");
    }

    section("ZK21. 改一台：成了 —— 日志逐字、盘上真改、喊一声重读重测");
    {
        FakeHost fake;
        ServerListController controller(fake.make());
        fake.answers = {QStringLiteral("新名字"), QStringLiteral("new.example.com")};
        check(controller.edit(instanceId, QStringLiteral("A服"),
                              QStringLiteral("a.example.com:25565")),
              "ZK21 改名 + 改地址成功");
        check(fake.logs == QStringList{QStringLiteral(
                  "已改服务器：A服（a.example.com:25565）→ 新名字（new.example.com）")},
              "ZK21 日志那一行**逐字**对得上（测试按逐字文本认日志）",
              fake.logs.join(QStringLiteral(" / ")));
        check(fake.notices.isEmpty(), "ZK21 成了就不用弹");
        check(fake.reloads == QStringList{instanceId},
              "ZK21 改完喊了一声「重读重测」、带的是这个实例",
              fake.reloads.join(QStringLiteral(" / ")));
        check(rowsOf().contains(QStringLiteral("新名字|new.example.com")),
              "ZK21 盘上真改了");
    }

    section("ZK22. 加一台：地址是空白 ⇒ 在问话之后立刻挡，不碰盘");
    {
        check(seed(), "ZK22 夹具重新落盘");
        const QByteArray before = readBytesAt(datPath);
        FakeHost fake;
        ServerListController controller(fake.make());
        fake.answers = {QStringLiteral("X"), QStringLiteral("   ")};
        check(!controller.add(instanceId), "ZK22 地址是空白 ⇒ 拒绝（返回 false）");
        check(fake.notices.size() == 1 &&
                  fake.notices.at(0).kind == ServerListController::Notice::Kind::AddRejected,
              "ZK22 弹的是「地址没填」那一档");
        check(fake.notices.size() == 1 && fake.notices.at(0).detail.isEmpty(),
              "ZK22 这一档的 detail 是空的 —— 那道闸自己就知道该说什么，不向数据层要原因");
        check(readBytesAt(datPath) == before, "ZK22 没走到数据层：盘上逐字节没动");
        check(fake.logs.isEmpty() && fake.reloads.isEmpty(), "ZK22 没记日志、没喊重读");
        check(fake.asks.size() == 2 &&
                  fake.asks.at(0) == ServerListController::Ask::AddName &&
                  fake.initials == QStringList({QString(), QString()}),
              "ZK22 加一台那两句都不预填（名字那一格本来就能留空）",
              fake.initials.join(QStringLiteral(" / ")));
    }

    section("ZK23. 加一台：名字留空也行，日志里那一格是「（无名）」");
    {
        check(seed(), "ZK23 夹具重新落盘");
        FakeHost fake;
        ServerListController controller(fake.make());
        fake.answers = {QString(), QStringLiteral("d.example.com")};
        check(controller.add(instanceId), "ZK23 名字留空照样加得进去");
        check(fake.logs == QStringList{QStringLiteral("已加服务器：（无名）（d.example.com）")},
              "ZK23 日志里走的是 displayName()（不是把空串原样印进去）",
              fake.logs.join(QStringLiteral(" / ")));
        check(rowsOf().contains(QStringLiteral("|d.example.com")),
              "ZK23 盘上真多了一条（名字那格是空串）");
        check(rowsOf().size() == 4, "ZK23 只多了一条");
    }

    section("ZK24. 改一台：数据层说不 ⇒ 只负责把原因弹出去，别的一律不动");
    {
        // 一份读不懂的 servers.dat：改它 ⇒ `ServerListFile` 拒绝（见 ZK15）。
        const QByteArray garbage = QByteArrayLiteral("not nbt at all");
        check(writeBytesAt(datPath, garbage), "ZK24 摆一份坏的 servers.dat");
        FakeHost fake;
        ServerListController controller(fake.make());
        fake.answers = {QStringLiteral("新名字"), QStringLiteral("new.example.com")};
        check(!controller.edit(instanceId, QStringLiteral("A服"),
                               QStringLiteral("a.example.com:25565")),
              "ZK24 数据层拒写 ⇒ 回头");
        check(fake.notices.size() == 1 &&
                  fake.notices.at(0).kind == ServerListController::Notice::Kind::UpdateFailed &&
                  !fake.notices.at(0).detail.isEmpty(),
              "ZK24 弹的是「改不动」那一档，原因原样带出来（不是一句空的「失败了」）",
              fake.notices.isEmpty() ? QString() : fake.notices.at(0).detail);
        check(fake.logs.isEmpty() && fake.reloads.isEmpty(),
              "ZK24 没写成的事**不许**记日志、也不许喊重读（日志里有它 = 骗人）");
        check(readBytesAt(datPath) == garbage, "ZK24 坏文件逐字节没动");
        check(seed(), "ZK24 夹具还原");
    }

    section("ZK25. 删一台：那句问话**点名字**（没名字的点「（无名）」）");
    {
        FakeHost fake;
        ServerListController controller(fake.make());
        const QByteArray before = readBytesAt(datPath);
        fake.confirmAnswer = false;
        check(!controller.remove(instanceId, QString(), QStringLiteral("c.example.com")),
              "ZK25 点了「不删」⇒ 回头");
        check(fake.confirmations == QStringList{QStringLiteral("（无名）@c.example.com")},
              "ZK25 问的是「（无名）」+ 地址 —— 光说「这一台」的话，屏幕上是同一个问题",
              fake.confirmations.join(QStringLiteral(" / ")));
        check(readBytesAt(datPath) == before, "ZK25 没删 ⇒ 盘上逐字节没动");
        check(fake.logs.isEmpty() && fake.reloads.isEmpty(), "ZK25 没记日志、没喊重读");
    }

    section("ZK26. 删一台：成了 —— 日志逐字、盘上真删、也喊重读重测");
    {
        FakeHost fake;
        ServerListController controller(fake.make());
        fake.confirmAnswer = true;
        check(controller.remove(instanceId, QStringLiteral("B服"),
                                QStringLiteral("b.example.com")),
              "ZK26 删得掉");
        check(fake.confirmations == QStringList{QStringLiteral("B服@b.example.com")},
              "ZK26 那句问话里用的是这个名字", fake.confirmations.join(QStringLiteral(" / ")));
        check(fake.logs == QStringList{QStringLiteral("已删服务器：B服（b.example.com）")},
              "ZK26 日志那一行逐字对得上", fake.logs.join(QStringLiteral(" / ")));
        check(fake.reloads == QStringList{instanceId}, "ZK26 删完也喊了一声「重读重测」");
        check(rowsOf() == QStringList({QStringLiteral("A服|a.example.com:25565"),
                                       QStringLiteral("|c.example.com")}),
              "ZK26 盘上真删了那一台、剩下两条原样", rowsOf().join(QStringLiteral(" / ")));
    }

    section("ZK27. 删一台：认不出来的那一条 ⇒ 删不动，也不留痕");
    {
        const QByteArray before = readBytesAt(datPath);
        FakeHost fake;
        ServerListController controller(fake.make());
        check(!controller.remove(instanceId, QStringLiteral("不存在"),
                                 QStringLiteral("nowhere.example.com")),
              "ZK27 盘上没有那一条 ⇒ 删不动");
        check(fake.notices.size() == 1 &&
                  fake.notices.at(0).kind == ServerListController::Notice::Kind::RemoveFailed &&
                  !fake.notices.at(0).detail.isEmpty(),
              "ZK27 弹的是「删不动」那一档，原因原样带出来",
              fake.notices.isEmpty() ? QString() : fake.notices.at(0).detail);
        check(readBytesAt(datPath) == before, "ZK27 盘上逐字节没动");
        check(fake.logs.isEmpty() && fake.reloads.isEmpty(), "ZK27 没记日志、没喊重读");
    }

    section("ZK28. 批量删：问数量、整批认得出才写、一次落盘");
    {
        check(seed(), "ZK28 夹具还原");
        const QByteArray before = readBytesAt(datPath);
        const QList<QPair<QString, QString>> both{
            qMakePair(QStringLiteral("A服"), QStringLiteral("a.example.com:25565")),
            qMakePair(QString(), QStringLiteral("c.example.com"))};
        {
            FakeHost fake;
            ServerListController controller(fake.make());
            fake.confirmAnswer = false;
            check(!controller.removeMany(instanceId, both) && fake.manyConfirmations == QList<int>{2},
                  "ZK28 点了「不删」⇒ 回头，问的是台数");
            check(readBytesAt(datPath) == before && fake.logs.isEmpty() && fake.reloads.isEmpty(),
                  "ZK28 没删 ⇒ 盘上逐字节没动、没记日志");
        }
        {
            FakeHost fake;
            ServerListController controller(fake.make());
            auto withMissing = both;
            withMissing.append(qMakePair(QStringLiteral("不存在"), QStringLiteral("nowhere.example.com")));
            check(!controller.removeMany(instanceId, withMissing),
                  "ZK28 有一条认不出来 ⇒ 整批删不动");
            check(fake.notices.size() == 1 &&
                      fake.notices.at(0).kind == ServerListController::Notice::Kind::RemoveFailed,
                  "ZK28 弹的是「删不动」那一档");
            check(readBytesAt(datPath) == before && fake.logs.isEmpty() && fake.reloads.isEmpty(),
                  "ZK28 整批不写：前两条也没删");
        }
        {
            FakeHost fake;
            ServerListController controller(fake.make());
            check(controller.removeMany(instanceId, both), "ZK28 全认得出 ⇒ 删得掉");
            check(rowsOf() == QStringList{QStringLiteral("B服|b.example.com")},
                  "ZK28 盘上只剩没选的那一台", rowsOf().join(QStringLiteral(" / ")));
            check(fake.logs == QStringList{QStringLiteral("已删服务器：2 台")} && fake.reloads == QStringList{instanceId},
                  "ZK28 记一行日志、喊一次重读重测");
        }
    }

    AppPaths::setMinecraftDir(originalDir);  // 收摊：后面那些节读的还是原来那一份
}


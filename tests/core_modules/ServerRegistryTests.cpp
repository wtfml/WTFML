#include "Support.hpp"

void runServerRegistryTests() {
    section("ZX. 服务端工作区：Servers 底下谁在住");

    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir temp;
    check(temp.isValid(), "ZX0 临时数据根目录建出来了");
    if (!temp.isValid()) {
        return;
    }
    AppPaths::setMinecraftDir(temp.path());
    {
        const bool enabledBefore = DirectorySessionLock::enabled;
        DirectorySessionLock::enabled = true;
        QLockFile other(QDir::temp().filePath(QStringLiteral("wtfml-directory-%1.lock")
            .arg(DirectorySessionLock::keyFor(temp.path()))));
        other.setStaleLockTime(0);
        check(other.tryLock(0), "ZX0 服务端写入口占用夹具取得数据根锁");
        QString problem;
        check(!ServerRegistry::ensureRoot(&problem) && problem.contains(QStringLiteral("占用")),
              "ZX0 服务端根目录创建拒绝占用目录", problem);
        check(!ServerRegistry::writeMeta(QStringLiteral("locked"), {}, &problem), "ZX0 服务端元数据拒绝占用目录");
        check(!ServerRegistry::createServer(QStringLiteral("locked"), {}, &problem), "ZX0 创建服务端拒绝占用目录");
        check(!ServerRegistry::removeServer(QStringLiteral("locked"), &problem), "ZX0 删除服务端拒绝占用目录");
        check(!ServerRegistry::acceptEula(QStringLiteral("locked"), &problem), "ZX0 EULA 写入拒绝占用目录");
        check(!ServerRegistry::writeProperties(QStringLiteral("locked"), {}, &problem), "ZX0 配置写入拒绝占用目录");
        DirectorySessionLock::enabled = enabledBefore;
        check(!QFileInfo::exists(ServerRegistry::rootDir()), "ZX0 拒绝后没有创建服务端半成品目录");
    }
    {
        QString problem;
        check(!ServerRegistry::writeMeta(QStringLiteral("../escape"), {}, &problem)
                  && !ServerRegistry::acceptEula(QStringLiteral("../escape"), &problem)
                  && !ServerRegistry::writeProperties(QStringLiteral("../escape"), {}, &problem)
                  && !QFileInfo::exists(temp.path() + QStringLiteral("/escape")),
              "ZX0 服务端所有写入口拒绝越界目录名");
    }

    // ---------- ZX1. 根目录与空表 ----------
    check(ServerRegistry::rootDir() == temp.path() + QStringLiteral("/servers"),
          "ZX1 根就是 `<数据目录>/servers`", ServerRegistry::rootDir());
    check(ServerRegistry::serverIds().isEmpty(),
          "★ ZX1 根还没建出来 ⇒ 空表（不是错误）");
    QString rootProblem;
    check(ServerRegistry::ensureRoot(&rootProblem) && QDir(ServerRegistry::rootDir()).exists(),
          "ZX1 ensureRoot 把根建出来了", rootProblem);

    // ---------- ZX2. 建一个（我们记的那一份说了算） ----------
    {
        ServerModel::Info info;
        info.name = QStringLiteral("我的服");
        info.flavor = ServerInstall::Flavor::Vanilla;
        info.flavorKnown = true;
        info.gameVersion = QStringLiteral("1.20.1");
        QString problem;
        check(ServerRegistry::createServer(QStringLiteral("Srv1"), info, &problem),
              "ZX2 建出来了", problem);
        check(QFileInfo::exists(AppPaths::serverMetaPath(QStringLiteral("Srv1"))),
              "ZX2 `.wtfml-server` 落在服务端目录里");

        QString describeProblem;
        const ServerRegistry::Entry entry =
            ServerRegistry::describe(QStringLiteral("Srv1"), &describeProblem);
        check(entry.serverId == QStringLiteral("Srv1"), "ZX2 身份是目录名");
        check(entry.name == QStringLiteral("我的服"),
              "ZX2 显示名取我们记的那一份（不是目录名）", entry.name);
        check(entry.flavorKnown && entry.flavor == ServerInstall::Flavor::Vanilla,
              "★ ZX2 自己记的那一档是**已知**的（不打「反推」那句）");
        check(!entry.installed && entry.note.contains(QStringLiteral("缺")),
              "★★ ZX2 还没放 server.jar ⇒ **没装好**，而且说得出缺什么", entry.note);
        check(entry.port == ServerModel::defaultServerPort(),
              "ZX2 没有 server.properties ⇒ 端口是缺省那个", QString::number(entry.port));
        check(!entry.eulaAccepted, "ZX2 还没有 eula.txt ⇒ 没接受");
    }

    // ---------- ZX3. 放一个 server.jar 进去 ⇒ 装好了 ----------
    {
        QFile jar(AppPaths::serverDir(QStringLiteral("Srv1"))
                  + QStringLiteral("/server.jar"));
        if (jar.open(QIODevice::WriteOnly)) {
            jar.write("jar");
            jar.close();
        }
        const ServerRegistry::Entry entry = ServerRegistry::describe(QStringLiteral("Srv1"));
        check(entry.installed,
              "★★ ZX3 盘上出现了 server.jar ⇒ 判成「装好了」（判据是文件，不是我们记的那一句）");
        check(entry.note.isEmpty(), "ZX3 装好之后那句「缺 …」就没了", entry.note);
    }

    // ---------- ZX4. EULA 真存真读 ----------
    {
        QString problem;
        check(ServerRegistry::acceptEula(QStringLiteral("Srv1"), &problem),
              "ZX4 写下一份接受的 eula.txt", problem);
        check(ServerRegistry::readEula(QStringLiteral("Srv1")),
              "★ ZX4 写下去之后读回来就是「已接受」");
        check(ServerRegistry::describe(QStringLiteral("Srv1")).eulaAccepted,
              "ZX4 describe 那一行也跟着是「已接受」");
    }

    // ---------- ZX5. server.properties 真存真读 ----------
    {
        QString problem;
        ServerModel::Properties props;
        props.setValue(QStringLiteral("server-port"), QStringLiteral("25566"));
        props.setValue(QStringLiteral("motd"), QStringLiteral("我的服务器"));
        check(ServerRegistry::writeProperties(QStringLiteral("Srv1"), props, &problem),
              "ZX5 写 server.properties", problem);
        QString readProblem;
        const ServerModel::Properties back =
            ServerRegistry::readProperties(QStringLiteral("Srv1"), &readProblem);
        check(readProblem.isEmpty(), "ZX5 读回来没报毛病", readProblem);
        check(ServerModel::serverPortOf(back) == 25566 &&
                  back.value(QStringLiteral("motd")) == QStringLiteral("我的服务器"),
              "★ ZX5 端口和中文都逐字回来了（按 UTF-8 写、按 UTF-8 读）");
        check(ServerRegistry::describe(QStringLiteral("Srv1")).port == 25566,
              "ZX5 describe 那一行的端口跟着变");
    }

    // ---------- ZX6. 从外面拖进来的：没有我们记的那一份 ⇒ 从文件反推 ----------
    {
        const QString dir = AppPaths::serverDir(QStringLiteral("Paper1"));
        QDir().mkpath(dir + QStringLiteral("/libraries/net/minecraftforge/forge/1.20.1-47.2.0"));
        const auto touch = [](const QString &path) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write("x");
                file.close();
            }
        };
        touch(dir + QStringLiteral(
                       "/libraries/net/minecraftforge/forge/1.20.1-47.2.0/win_args.txt"));
        touch(dir + QStringLiteral("/user_jvm_args.txt"));

        const ServerRegistry::Entry entry = ServerRegistry::describe(QStringLiteral("Paper1"));
        check(!entry.flavorKnown,
              "★★ ZX6 没有 `.wtfml-server` ⇒ 那一档是**反推**的（标成「猜的」）");
        check(entry.flavor == ServerInstall::Flavor::Forge,
              "★★ ZX6 从 libraries 那张 win_args.txt 反推成 Forge", entry.name);
        check(entry.installed,
              "ZX6 两张参数文件都在 ⇒ 判成装好了");
        check(entry.note.contains(QStringLiteral("反推")),
              "★ ZX6 那一行写着「这一档是反推的」，让用户点一下确认");
        check(entry.name == QStringLiteral("Paper1"),
              "ZX6 没有我们记的名字 ⇒ 显示名退回目录名");
    }

    // ---------- ZX7. 名字的清洗 / 去重 ----------
    {
        check(ServerRegistry::sanitizeId(QStringLiteral("a/b:c")) == QStringLiteral("a_b_c"),
              "ZX7 路径分隔符和冒号换成下划线", ServerRegistry::sanitizeId(QStringLiteral("a/b:c")));
        check(ServerRegistry::sanitizeId(QStringLiteral("  .hidden ")) == QStringLiteral("hidden"),
              "★ ZX7 首尾的点和空白摘掉（Windows 存下去会自己吃掉，不如先摘）");
        check(ServerRegistry::sanitizeId(QStringLiteral("Server1.20.1")) ==
                  QStringLiteral("Server1.20.1"),
              "★ ZX7 名字里的点**不摘**（`Server1.20.1` 是用户真机上那种叫法）");
        // 空格必须原样留着（真机上的名字就有带空格的）。这一条专门盯"s 那串禁字符里
        // 混进一个空格"那种写法 —— 症状是每个空格都被换成下划线。
        check(ServerRegistry::sanitizeId(QStringLiteral("我的 服务端 2")) ==
                  QStringLiteral("我的 服务端 2"),
              "★★ ZX7 名字里的空格**原样留着**（不许被当成禁字符换掉）",
              ServerRegistry::sanitizeId(QStringLiteral("我的 服务端 2")));
        check(ServerRegistry::sanitizeId(QString()).isEmpty(),
              "ZX7 洗空了 ⇒ 空串（调用方得让用户重敲）");
        check(ServerRegistry::uniqueServerId(QStringLiteral("Srv1")) == QStringLiteral("Srv1-2"),
              "★★ ZX7 撞名了就加后缀（Srv1 已存在 ⇒ Srv1-2）",
              ServerRegistry::uniqueServerId(QStringLiteral("Srv1")));
        check(ServerRegistry::uniqueServerId(QStringLiteral("全新")) == QStringLiteral("全新"),
              "ZX7 没撞名的原样");
        check(ServerRegistry::defaultServerName(ServerInstall::Flavor::Vanilla,
                                                QStringLiteral("1.20.1")) ==
                  QStringLiteral("Server1.20.1"),
              "ZX7 默认名照用户真机上那份叫法（Server<版本>）");
        check(ServerRegistry::defaultServerName(ServerInstall::Flavor::Vanilla, QString()) ==
                  QStringLiteral("Server"),
              "ZX7 不知道版本就回 Server");
    }

    // ---------- ZX8. 删之前那三道闸 ----------
    {
        check(!ServerRegistry::removeServer(QString(), nullptr),
              "★★ ZX8 空 id 不许删");
        check(!ServerRegistry::removeServer(QStringLiteral("."), nullptr) &&
                  !ServerRegistry::removeServer(QStringLiteral(".."), nullptr),
              "★★ ZX8 `.` / `..` 不许删（那是根）");
        check(!ServerRegistry::removeServer(QStringLiteral("a/b"), nullptr) &&
                  !ServerRegistry::removeServer(QStringLiteral("a\\b"), nullptr),
              "★★ ZX8 带路径分隔符的不许删（别删到根外面去）");
        check(ServerRegistry::removeServer(QStringLiteral("不存在的服"), nullptr),
              "ZX8 本来就不在 = 已经达成目的（回 true）");
    }

    // ---------- ZX8.5 「异常退出后自动重启」那一格：读不懂就不写 ----------
    {
        const QString metaPath = AppPaths::serverMetaPath(QStringLiteral("Srv1"));
        QFile file(metaPath);
        check(file.open(QIODevice::ReadOnly), "ZX8.5 先拿到那一份记录");
        const QByteArray before = file.readAll();
        file.close();
        check(!before.isEmpty(), "ZX8.5 那一份不是空的");

        const QByteArray corruptBytes = QStringLiteral("{ 这不是 JSON").toUtf8();
        QFile broken(metaPath);
        check(broken.open(QIODevice::WriteOnly | QIODevice::Truncate), "ZX8.5 把它弄坏");
        broken.write(corruptBytes);
        broken.close();

        QString problem;
        check(!ServerRegistry::setRestartOnCrash(QStringLiteral("Srv1"), true, &problem),
              "★★ ZX8.5 读不懂的那一份不许被一份空记录顶掉", problem);
        check(!problem.isEmpty(), "ZX8.5 而且说得出为什么");

        QFile after(metaPath);
        check(after.open(QIODevice::ReadOnly), "ZX8.5 坏的那份还在");
        const QByteArray untouched = after.readAll();
        after.close();
        check(untouched == corruptBytes, "★★ ZX8.5 一个字节都没动", QString::fromUtf8(untouched));

        // 修回来之后照常能写：而且**别的格还在**（这一格不是整份重建）。
        QFile restore(metaPath);
        check(restore.open(QIODevice::WriteOnly | QIODevice::Truncate), "ZX8.5 先修回来");
        restore.write(before);
        restore.close();
        QString okProblem;
        check(ServerRegistry::setRestartOnCrash(QStringLiteral("Srv1"), true, &okProblem),
              "ZX8.5 好的那一份照常写进去", okProblem);
        const ServerModel::Info reread = ServerRegistry::readMeta(QStringLiteral("Srv1"));
        check(reread.restartOnCrash, "★ ZX8.5 写下去之后读回来就是「打开」");
        check(reread.gameVersion == QStringLiteral("1.20.1"),
              "★ ZX8.5 同一份记录里别的格（游戏版本）还在 —— 这一项不是整份重写", reread.gameVersion);

        // ZX8.6：**未知字段**也要活过一次读—改—写。
        // `ServerModel::toJson()` 只认识自己那几格，照它整份重建会把「更新的启动器写的」
        // 或「用户手写的」键一起抹掉；writeMeta 现在先读盘上那份当底、再盖已知字段。
        QJsonObject seeded = QJsonDocument::fromJson(before).object();
        seeded.insert(QStringLiteral("futureField"), QStringLiteral("keep-me"));
        QFile reseed(metaPath);
        check(reseed.open(QIODevice::WriteOnly | QIODevice::Truncate), "ZX8.6 摆一份带未知字段的记录");
        reseed.write(QJsonDocument(seeded).toJson());
        reseed.close();
        QString keepProblem;
        check(ServerRegistry::setRestartOnCrash(QStringLiteral("Srv1"), true, &keepProblem),
              "ZX8.6 在它上面改一格", keepProblem);
        QFile rawAfter(metaPath);
        check(rawAfter.open(QIODevice::ReadOnly), "ZX8.6 读回原始字节");
        const QJsonObject afterObject = QJsonDocument::fromJson(rawAfter.readAll()).object();
        rawAfter.close();
        check(afterObject.value(QStringLiteral("futureField")).toString() == QStringLiteral("keep-me"),
              "★★ ZX8.6 未知字段活过一次读—改—写（不是照结构体重建整份）",
              QString::fromUtf8(QJsonDocument(afterObject).toJson(QJsonDocument::Compact)));
        check(afterObject.value(QStringLiteral("restartOnCrash")).toBool(false),
              "ZX8.6 已知字段照常写进去");

        // ZX8.8：**恢复默认值也要落盘** —— 打开过之后关掉，回读必须真的是关。
        // `toJson()` 对 `restartOnCrash == false` 是省略的，只"盖"不"清"时盘上旧
        // `true` 会一直活着（这就是本轮复核确认的回归）。
        {
            QString problemOn;
            check(ServerRegistry::setRestartOnCrash(QStringLiteral("Srv1"), true, &problemOn),
                  "ZX8.8 先打开自动重启", problemOn);
            QString problemOff;
            check(ServerRegistry::setRestartOnCrash(QStringLiteral("Srv1"), false, &problemOff),
                  "★ ZX8.8 再关掉", problemOff);
            const ServerModel::Info off = ServerRegistry::readMeta(QStringLiteral("Srv1"));
            check(!off.restartOnCrash,
                  "★★ ZX8.8 关掉之后读回来就是「关」（默认值省略时也要把旧 true 清掉）");
            QFile raw(metaPath);
            check(raw.open(QIODevice::ReadOnly), "ZX8.8 读回原始字节");
            const QJsonObject object = QJsonDocument::fromJson(raw.readAll()).object();
            raw.close();
            check(!object.contains(QStringLiteral("restartOnCrash")),
                  "★ ZX8.8 盘上那格旧值也不留了（默认值不写成 false，直接把键去掉）",
                  QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
            check(object.value(QStringLiteral("futureField")).toString() == QStringLiteral("keep-me"),
                  "ZX8.8 清默认值时未知字段照样在");
        }

        // ZX8.9：非默认的重试次数回到默认，同样要清掉。
        {
            ServerModel::Info info = ServerRegistry::readMeta(QStringLiteral("Srv1"));
            info.restartMaxAttempts = 7;
            QString writeProblem;
            check(ServerRegistry::writeMeta(QStringLiteral("Srv1"), info, &writeProblem),
                  "ZX8.9 先写一个非默认次数", writeProblem);
            ServerModel::Info back = ServerRegistry::readMeta(QStringLiteral("Srv1"));
            check(back.restartMaxAttempts == 7, "ZX8.9 读回非默认次数",
                  QString::number(back.restartMaxAttempts));
            back.restartMaxAttempts = ServerRestart::kDefaultMaxAttempts;
            QString resetProblem;
            check(ServerRegistry::writeMeta(QStringLiteral("Srv1"), back, &resetProblem),
                  "ZX8.9 再写回默认", resetProblem);
            const ServerModel::Info def = ServerRegistry::readMeta(QStringLiteral("Srv1"));
            check(def.restartMaxAttempts == ServerRestart::kDefaultMaxAttempts,
                  "★★ ZX8.9 恢复默认后读回默认（旧值清掉）",
                  QString::number(def.restartMaxAttempts));
            QFile raw(metaPath);
            check(raw.open(QIODevice::ReadOnly), "ZX8.9 读回原始字节");
            const QJsonObject object = QJsonDocument::fromJson(raw.readAll()).object();
            raw.close();
            check(!object.contains(QStringLiteral("restartMaxAttempts")),
                  "★ ZX8.9 默认值不落键");
        }

        // ZX8.10：可选已知字段（javaPath）清空也是"删键"，未知字段仍然保留。
        {
            ServerModel::Info info = ServerRegistry::readMeta(QStringLiteral("Srv1"));
            info.javaPath = QStringLiteral("C:/Program Files/Java/bin/java.exe");
            QString writeProblem;
            check(ServerRegistry::writeMeta(QStringLiteral("Srv1"), info, &writeProblem),
                  "ZX8.10 先写一个指定 Java", writeProblem);
            ServerModel::Info cleared = ServerRegistry::readMeta(QStringLiteral("Srv1"));
            check(cleared.javaPath == info.javaPath, "ZX8.10 读回指定 Java", cleared.javaPath);
            cleared.javaPath.clear();
            QString clearProblem;
            check(ServerRegistry::writeMeta(QStringLiteral("Srv1"), cleared, &clearProblem),
                  "ZX8.10 再清空这一格", clearProblem);
            const ServerModel::Info after = ServerRegistry::readMeta(QStringLiteral("Srv1"));
            check(after.javaPath.isEmpty(), "★★ ZX8.10 清空之后读回来是空（旧路径不残留）",
                  after.javaPath);
            QFile raw(metaPath);
            check(raw.open(QIODevice::ReadOnly), "ZX8.10 读回原始字节");
            const QJsonObject object = QJsonDocument::fromJson(raw.readAll()).object();
            raw.close();
            check(!object.contains(QStringLiteral("javaPath")) && !object.contains(QStringLiteral("memoryMb")),
                  "★ ZX8.10 空字符串/0 不落键");
            check(object.value(QStringLiteral("futureField")).toString() == QStringLiteral("keep-me"),
                  "ZX8.10 未知字段照样保留");
        }
    }

    // ---------- ZX8.7. 大得不像一份记录的：当读不懂，不读进内存也不覆盖 ----------
    {
        const QString hugeMetaPath = AppPaths::serverMetaPath(QStringLiteral("Srv1"));
        QFile big(hugeMetaPath);
        check(big.open(QIODevice::WriteOnly | QIODevice::Truncate), "ZX8.7 摆一份超大记录");
        const QByteArray junk(5 * 1024 * 1024, 'x');
        check(big.write(junk) == junk.size(), "ZX8.7 写进去 5 MiB");
        big.close();
        QString readProblem;
        const ServerModel::Info info = ServerRegistry::readMeta(QStringLiteral("Srv1"), &readProblem);
        check(info.gameVersion.isEmpty() && readProblem.contains(QStringLiteral("大得不像")),
              "★★ ZX8.7 超过 4 MiB 的记录按读不懂处理（不先 readAll 再说）", readProblem);
        QString toggleProblem;
        check(!ServerRegistry::setRestartOnCrash(QStringLiteral("Srv1"), true, &toggleProblem),
              "ZX8.7 读不懂就不写（和 ZX8.5 同一条规矩）", toggleProblem);
        QFile verify(hugeMetaPath);
        check(verify.open(QIODevice::ReadOnly) && verify.size() == junk.size(),
              "ZX8.7 那一份原样还在、没被覆盖");
    }

    // ---------- ZX9. 真删一个 ----------
    {
        const QString dir = AppPaths::serverDir(QStringLiteral("Srv1"));
        check(QDir(dir).exists(), "ZX9 删之前那个目录在");
        check(ServerRegistry::removeServer(QStringLiteral("Srv1"), nullptr),
              "ZX9 删成功");
        check(!QDir(dir).exists(), "ZX9 目录连带里面的文件都没了");
        check(!ServerRegistry::serverIds().contains(QStringLiteral("Srv1")),
              "ZX9 列表里也不再是它");
    }

    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "ZX9 数据根目录换回来了");
}




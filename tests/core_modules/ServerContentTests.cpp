#include "Support.hpp"

void runServerContentTests() {
    section("ZAH. 服务端内容：模组 / 数据包的名字与分类");

    // ---------- ZAH0. 名字上的规矩 ----------
    {
        check(ServerContent::kindCount() == 2, "ZAH0 内容就两类（模组 / 数据包）",
              QString::number(ServerContent::kindCount()));
        check(ServerContent::kindTitle(ServerContent::Kind::Mods) == QStringLiteral("模组")
                  && ServerContent::kindTitle(ServerContent::Kind::Datapacks)
                         == QStringLiteral("数据包"),
              "ZAH0 两类的标题");
        check(ServerContent::folderName(ServerContent::Kind::Mods) == QStringLiteral("mods")
                  && ServerContent::folderName(ServerContent::Kind::Datapacks)
                         == QStringLiteral("datapacks"),
              "ZAH0 两类的文件夹名");
        check(ServerContent::fileSuffix(ServerContent::Kind::Mods) == QStringLiteral(".jar")
                  && ServerContent::fileSuffix(ServerContent::Kind::Datapacks)
                         == QStringLiteral(".zip"),
              "ZAH0 两类认的后缀");
        check(ServerContent::disabledSuffix() == QStringLiteral(".disabled"),
              "★ ZAH0 禁用那条尾巴的唯一出处");

        check(ServerContent::isContentFileName(ServerContent::Kind::Mods,
                                               QStringLiteral("a.jar"))
                  && ServerContent::isContentFileName(ServerContent::Kind::Mods,
                                                      QStringLiteral("A.JAR"))
                  && ServerContent::isContentFileName(ServerContent::Kind::Mods,
                                                      QStringLiteral("a.jar.disabled")),
              "★ ZAH0 `.jar` / 大写 / 带 `.disabled` 尾巴的都算模组");
        check(!ServerContent::isContentFileName(ServerContent::Kind::Mods,
                                                QStringLiteral("a.jar.bak"))
                  && !ServerContent::isContentFileName(ServerContent::Kind::Mods,
                                                       QStringLiteral("a.zip"))
                  && !ServerContent::isContentFileName(ServerContent::Kind::Mods,
                                                       QStringLiteral("a"))
                  && !ServerContent::isContentFileName(ServerContent::Kind::Mods,
                                                       QStringLiteral(".jar")),
              "★★ ZAH0 `.jar.bak` / `.zip` / 没后缀 / 光一条后缀都不算模组（摘不掉尾巴就落选）");
        check(ServerContent::isContentFileName(ServerContent::Kind::Datapacks,
                                               QStringLiteral("p.zip"))
                  && ServerContent::isContentFileName(ServerContent::Kind::Datapacks,
                                                      QStringLiteral("p.ZIP.DISABLED"))
                  && !ServerContent::isContentFileName(ServerContent::Kind::Datapacks,
                                                       QStringLiteral("p.jar")),
              "ZAH0 数据包认 `.zip`（大写的尾巴也认），不认 `.jar`");

        check(ServerContent::enabledName(QStringLiteral("a.jar.disabled"))
                  == QStringLiteral("a.jar")
                  && ServerContent::enabledName(QStringLiteral("a.jar")) == QStringLiteral("a.jar"),
              "ZAH0 摘尾巴只摘一次");
        check(ServerContent::isDisabledName(QStringLiteral("a.jar.DISABLED"))
                  && !ServerContent::isDisabledName(QStringLiteral("a.jar")),
              "ZAH0 禁用判据只看尾巴、不分大小写");
    }

    // ---------- ZAH1. 禁用 = 改名，不是删 ----------
    {
        check(ServerContent::toggledName(ServerContent::Kind::Mods, QStringLiteral("a.jar"), false)
                  == QStringLiteral("a.jar.disabled"),
              "★ ZAH1 禁用模组 = 加 `.disabled` 尾巴");
        check(ServerContent::toggledName(ServerContent::Kind::Mods,
                                         QStringLiteral("a.jar.disabled"), true)
                  == QStringLiteral("a.jar"),
              "★ ZAH1 启用 = 摘掉 `.disabled` 尾巴（文件内容原样）");
        check(ServerContent::toggledName(ServerContent::Kind::Datapacks,
                                         QStringLiteral("p.zip"), false)
                  == QStringLiteral("p.zip.disabled"),
              "ZAH1 数据包走同一条规矩");
        check(ServerContent::toggledName(ServerContent::Kind::Mods, QStringLiteral("a.zip"), false)
                      .isEmpty()
                  && ServerContent::toggledName(ServerContent::Kind::Datapacks,
                                                QStringLiteral("a.jar"), true)
                         .isEmpty()
                  && ServerContent::toggledName(ServerContent::Kind::Mods,
                                                QStringLiteral("readme.txt"), false)
                         .isEmpty(),
              "★★ ZAH1 名字不是这一类的 ⇒ 回空串（调用方据此说\"这一行动不了\"，不猜）");
        // 已经是想要的状态：改名前后同名（调用方据此直接跳过，不做多余的 rename）。
        check(ServerContent::toggledName(ServerContent::Kind::Mods, QStringLiteral("a.jar"), true)
                  == QStringLiteral("a.jar"),
              "ZAH1 已经启用着再要启用 ⇒ 名字不变");
    }

    // ---------- ZAH2. 文件夹 ----------
    {
        const QString serverDir = QStringLiteral("/srv/mc");
        check(ServerContent::modsDir(serverDir) == QStringLiteral("/srv/mc/mods"),
              "ZAH2 模组在服务端目录的 mods/ 里", ServerContent::modsDir(serverDir));
        const QString worldDir = QStringLiteral("/srv/mc/world");
        check(ServerContent::datapacksDir(worldDir)
                  == QStringLiteral("/srv/mc/world/datapacks"),
              "ZAH2 数据包在世界目录的 datapacks/ 里",
              ServerContent::datapacksDir(worldDir));
        check(ServerContent::modsDir(QString()).isEmpty()
                  && ServerContent::datapacksDir(QString()).isEmpty(),
              "★ ZAH2 服务端目录 / 世界目录为空 ⇒ 回空串（不编一个假路径出来）");
    }

    // ---------- ZAH3. 列出来的行 ----------
    {
        const QStringList names{QStringLiteral("a.jar"),        QStringLiteral("readme.txt"),
                                QStringLiteral("b.jar.disabled"), QStringLiteral("c.zip"),
                                QStringLiteral("d.jar.bak")};
        const QList<ServerContent::Entry> mods =
            ServerContent::entriesFromNames(names, ServerContent::Kind::Mods);
        check(mods.size() == 2 && mods.value(0).name == QStringLiteral("a.jar")
                  && !mods.value(0).disabled && mods.value(1).name == QStringLiteral("b.jar.disabled")
                  && mods.value(1).disabled,
              "★★ ZAH3 模组那一列：只出 `.jar` / 禁用的；顺序照传进来的",
              QStringLiteral("%1 行").arg(mods.size()));
        const QList<ServerContent::Entry> packs =
            ServerContent::entriesFromNames(names, ServerContent::Kind::Datapacks);
        check(packs.size() == 1 && packs.value(0).name == QStringLiteral("c.zip")
                  && !packs.value(0).disabled,
              "★ ZAH3 数据包那一列只认 `.zip`（`.jar` 不进这一列）",
              QStringLiteral("%1 行").arg(packs.size()));
        check(ServerContent::entriesFromNames({}, ServerContent::Kind::Mods).isEmpty(),
              "ZAH3 空文件夹 ⇒ 空列表（不是错误）");
    }
}


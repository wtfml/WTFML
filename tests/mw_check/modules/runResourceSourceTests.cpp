#include "Support.hpp"
void runResourceSourceTests() {
    FakeResourceApi api;
    check(api.start(), "ZH0 本地假接口起来了");
    api.searchBody = QByteArrayLiteral(
        "{\"data\":[{\"id\":1546263,\"name\":\"Arizona\",\"slug\":\"arizona\","
        "\"summary\":\"一句简介\",\"downloadCount\":642,"
        "\"authors\":[{\"name\":\"birdmuncher\"}],\"categories\":[{\"name\":\"Technology\"}],"
        "\"latestFilesIndexes\":[{\"gameVersion\":\"1.21.1\",\"modLoader\":4}],"
        "\"logo\":{\"thumbnailUrl\":\"https://media.forgecdn.net/a_256.png\","
        "\"url\":\"https://media.forgecdn.net/a.png\"}}],"
        "\"pagination\":{\"index\":0,\"pageSize\":20,\"resultCount\":1,\"totalCount\":1}}");
    api.filesBody = QByteArrayLiteral(
        "{\"data\":[{\"id\":8964987,\"displayName\":\"1.0 for Fabric 1.21.1\","
        "\"fileName\":\"arizona-1.0.jar\",\"fileLength\":1234,\"releaseType\":2,"
        "\"fileDate\":\"2026-09-20T20:00:41Z\",\"downloadUrl\":\"https://edge.forgecdn.net/a.jar\","
        "\"gameVersions\":[\"1.21\",\"1.21.1\",\"Client\"],"
        "\"hashes\":[{\"value\":\"da39a3ee5e6b4b0d3255bfef95601890afd80709\",\"algo\":1}]}]}");

    ResourceStore store;
    store.setCurseForgeApiBaseOverride(api.baseUrl());

    // ---- ZH1. 不带 key：走镜像那条形状，一个 x-api-key 都不发 ------------------
    {
        api.requests.clear();
        api.apiKeys.clear();
        ResourceCatalog::SearchResult found;
        const QString key =
            ResourceCatalog::searchRequestKey(ResourceCatalog::Kind::Mod,
                                              ResourceCatalog::Source::CurseForge);
        store.search(key, ResourceCatalog::Kind::Mod, ResourceCatalog::Source::CurseForge,
                     QStringLiteral("jei"), QStringLiteral("1.21.1"), QStringLiteral("1"),
                     QString());
        check(waitForSearch(store, key, &found), "ZH1 CurseForge 那一发回来了");
        check(api.apiKeys.size() == 1 && api.apiKeys.first().isEmpty(),
              "ZH1 没有 key 时**一个 x-api-key 都不发**（默认是社区镜像，它不要这个头）",
              api.apiKeys.join(QStringLiteral("|")));
        check(api.lastRequest().startsWith(QStringLiteral("GET /v1/mods/search?")),
              "ZH1 打的是 CurseForge 的搜索路径", api.lastRequest());
        check(api.lastRequest().contains(QStringLiteral("gameId=432")) &&
                  api.lastRequest().contains(QStringLiteral("classId=6")) &&
                  api.lastRequest().contains(QStringLiteral("modLoaderType=1")) &&
                  api.lastRequest().contains(QStringLiteral("gameVersion=1.21.1")) &&
                  api.lastRequest().contains(QStringLiteral("searchFilter=jei")),
              "ZH1 五个筛子都在地址上（游戏版本/加载器/类目/搜索词/游戏号）", api.lastRequest());
        // 反面那一条：**不许**出现裸的 `search=`。这一家的文本筛选那一段叫 `searchFilter`，
        // 发 `search=` 它一个字都不筛、也不报错（回的是"这一类最热门的"，界面上看着"搜出来了"）。
        // 正面那条（上面）挡不住这个错：把参数名改回 `search=` 之后，`searchFilter=jei`
        // 那一段就没有了，正面那条会当场红 —— 但要是有人**两个都发**，只有这条能看出来。
        check(!api.lastRequest().contains(QStringLiteral("&search=")) &&
                  !api.lastRequest().contains(QStringLiteral("?search=")),
              "ZH1 地址里**没有**裸的 `search=` 那一段（发它等于没搜，而且不报错）",
              api.lastRequest());
        check(found.hits.size() == 1 && found.hits.first().projectId == QStringLiteral("1546263"),
              "ZH1 回来的正文走的是 **CurseForge 那个解析器**（根在 data 上）",
              QString::number(found.hits.size()));
        check(found.hits.size() == 1 &&
                  found.hits.first().iconUrl == QStringLiteral("https://media.forgecdn.net/a_256.png"),
              "ZH1 图标地址也是那一条路给的（不是 Modrinth 那个自己拼的）");
    }

    // ---- ZH2. 填了 key：这一发就带上，**而且是当场读的那份** -------------------
    {
        api.requests.clear();
        api.apiKeys.clear();
        store.setCurseForgeApiKey(QStringLiteral("  一份自己填的 key  "));
        ResourceCatalog::SearchResult found;
        const QString key =
            ResourceCatalog::searchRequestKey(ResourceCatalog::Kind::Mod,
                                              ResourceCatalog::Source::CurseForge);
        store.search(key, ResourceCatalog::Kind::Mod, ResourceCatalog::Source::CurseForge,
                     QString(), QString(), QString(), QString());
        check(waitForSearch(store, key, &found), "ZH2 填了 key 的那一发回来了");
        check(api.apiKeys.size() == 1 &&
                  api.apiKeys.first() == QStringLiteral("一份自己填的 key"),
              "ZH2 填了 key 就带上（两头的空白去掉，和 VersionManager 那边同一个规矩）",
              api.apiKeys.join(QStringLiteral("|")));
        check(!api.lastRequest().contains(QStringLiteral("key")),
              "ZH2 key 只走请求头，**地址里一个字都不许出现**（地址会进日志、进历史）",
              api.lastRequest());
        store.setCurseForgeApiKey(QString());
    }

    // ---- ZH3. 版本列表：CF 要 `{"data":[…]}`、Modrinth 要数组 -----------------
    {
        api.requests.clear();
        QList<ResourceCatalog::VersionEntry> versions;
        QString problem;
        const QString cfKey = ResourceCatalog::versionsRequestKey(
            QStringLiteral("1546263"), QStringLiteral("1.21.1"), QString(),
            ResourceCatalog::Source::CurseForge);
        store.fetchVersions(cfKey, QStringLiteral("1546263"),
                            ResourceCatalog::Source::CurseForge, QStringLiteral("1.21.1"));
        check(waitForVersions(store, cfKey, &versions, &problem), "ZH3 CF 版本那一发回来了");
        check(problem.isEmpty() && versions.size() == 1 &&
                  versions.first().files.size() == 1 &&
                  versions.first().files.first().filename == QStringLiteral("arizona-1.0.jar"),
              "ZH3 **对象**形状的正文被接下来了（`data` 里那一条就是一份版本）",
              problem.isEmpty() ? QString::number(versions.size()) : problem);

        // 反面：**数组**形状（Modrinth 那一份的样子）交给 CurseForge 这条路 ——
        // 这一条就是"闸真的按来源分叉了吗"的证据。两边都接下 = 这道闸其实没分叉，
        // 而症状是"CurseForge 那一页把一份错位的东西当成版本清单画出来"。
        //
        // NOTE: 反面**只能这么测**（把 Modrinth 的正文喂给 CurseForge 那条路），不能反过来
        // 把 CurseForge 的正文喂给 Modrinth 那条路 —— Modrinth 那条路没有"接口根地址"
        // 这个口子（地址是写死的），喂不过去，会真打到 api.modrinth.com 上。
        // 这一条量的是**同一个判据的两半**，分叉的方向在这半边看得一清二楚。
        api.filesBody = QByteArrayLiteral("[{\"id\":\"aaa\",\"version_number\":\"1.0\"}]");
        versions.clear();
        problem.clear();
        const QString arrayKey = ResourceCatalog::versionsRequestKey(
            QStringLiteral("1546263"), QStringLiteral("1.21.1"), QString(),
            ResourceCatalog::Source::CurseForge);
        store.fetchVersions(arrayKey, QStringLiteral("1546263"),
                            ResourceCatalog::Source::CurseForge, QStringLiteral("1.21.1"));
        check(waitForVersions(store, arrayKey, &versions, &problem), "ZH3 第二发也回来了");
        check(!problem.isEmpty() && versions.isEmpty(),
              "ZH3 CurseForge 那条路**不许**把一整个数组当成版本清单（它要的是 data 那个对象）",
              problem.isEmpty() ? QStringLiteral("（没报错、还接了 %1 条）").arg(versions.size())
                                : problem);
    }

    // ---- ZH4. 换成官方那条（有 key）时，地址跟着换 ----------------------------
    //
    // 这一条钉的是"接口根地址只有一处出处"的另一面：key 一填，base 就从镜像变成官方，
    // 而**这一层不许自己拼域名**（上面 ZH1/ZH3 打的全是 127.0.0.1，就是因为 override
    // 优先）。改掉 override 之后必须立刻打新的那一台 —— 否则"设置里换了下载源、这一页
    // 还在问老地方"。
    {
        FakeResourceApi other;
        check(other.start(), "ZH4 第二台假接口起来了");
        other.searchBody = api.searchBody;
        api.requests.clear();  // 下面要数的是"老那一台还收不收得到东西"
        store.setCurseForgeApiBaseOverride(other.baseUrl());
        ResourceCatalog::SearchResult found;
        const QString key =
            ResourceCatalog::searchRequestKey(ResourceCatalog::Kind::Mod,
                                              ResourceCatalog::Source::CurseForge);
        store.search(key, ResourceCatalog::Kind::Mod, ResourceCatalog::Source::CurseForge,
                     QString(), QString(), QString(), QString());
        check(waitForSearch(store, key, &found), "ZH4 换过地址之后那一发回来了");
        check(other.requests.size() == 1 && api.requests.isEmpty(),
              "ZH4 这一发打到**新**那一台上了，老那一台一个字节都没再收到"
              "（地址是发请求那一刻读的，不是构造时定死的）",
              QStringLiteral("新=%1 老=%2")
                  .arg(other.requests.join(QStringLiteral("|")),
                       api.requests.join(QStringLiteral("|"))));
    }

    // ---- ZH5. 图标那一格的**身份**带来源前缀 ---------------------------------
    //
    // 这条是纯函数，但放在这里是因为它和上面那几条是同一件事的两半：图标那条频道回的
    // 就是这个 id（见 ResourceStore::iconReady），而两个来源的 id 空间**会撞**
    // （`238222` 在 Modrinth 的 base62 里也合法，CurseForge 那边真有这个号）。
    {
        ResourceCatalog::Hit modrinth;
        modrinth.projectId = QStringLiteral("238222");
        modrinth.source = ResourceCatalog::Source::Modrinth;
        ResourceCatalog::Hit curseForge;
        curseForge.projectId = QStringLiteral("238222");
        curseForge.source = ResourceCatalog::Source::CurseForge;
        check(modrinth.iconCacheId() != curseForge.iconCacheId(),
              QStringLiteral("ZH5 两个来源的同号资源**不是同一格**（撞上就是「永远画着另一个"
                             "资源的图标」）"),
              modrinth.iconCacheId() + QStringLiteral(" / ") + curseForge.iconCacheId());
        check(!ResourceStore::iconCachePath(modrinth.iconCacheId()).isEmpty() &&
                  !ResourceStore::iconCachePath(curseForge.iconCacheId()).isEmpty() &&
                  ResourceStore::iconCachePath(modrinth.iconCacheId()) !=
                      ResourceStore::iconCachePath(curseForge.iconCacheId()),
              "ZH5 两者的缓存文件也是两个（带来源前缀之后仍然是文件名安全的）");
    }
}


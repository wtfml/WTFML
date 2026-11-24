#include "Support.hpp"

void runResourceCatalogTests() {
    const QString metaDir = QStringLiteral(REPO_ROOT_PATH) + "/tests/meta";

    section("Z1. 几类资源的键 / 目标目录 / 名字");

    // NOTE: 加了「数据包」（要求「下载那一栏少了个数据包类型，把它放在模组和
    // 资源包中间加上」）—— API 那边认的词是 Modrinth 自己的 `datapack`（不是
    // `datapacks`，那个是我们落盘用的目录名，两回事）。
    check(ResourceCatalog::kindKey(ResourceCatalog::Kind::Mod) == QStringLiteral("mod") &&
              ResourceCatalog::kindKey(ResourceCatalog::Kind::Modpack) ==
                  QStringLiteral("modpack") &&
              ResourceCatalog::kindKey(ResourceCatalog::Kind::DataPack) ==
                  QStringLiteral("datapack") &&
              ResourceCatalog::kindKey(ResourceCatalog::Kind::Shader) == QStringLiteral("shader") &&
              ResourceCatalog::kindKey(ResourceCatalog::Kind::ResourcePack) ==
                  QStringLiteral("resourcepack"),
          "Z1 五个键就是 API 认的那五个词");

    // 反向要认回来（界面拿的是分类键，不是枚举）。
    int roundTrip = 0;
    for (ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
        ResourceCatalog::Kind back = ResourceCatalog::Kind::Mod;
        if (ResourceCatalog::kindFromCategoryKey(ResourceCatalog::kindKey(kind), &back) &&
            back == kind) {
            ++roundTrip;
        }
    }
    check(roundTrip == 5, "Z1 键绕一圈能认回原来的那一类",
          QStringLiteral("认回来 %1/5").arg(roundTrip));

    // 「游戏」那一页也走这条认路，它不是能搜的那几类之一 —— 认不出来时要如实回 false，
    // 不能默认成"那就按模组算"（那会变成"点游戏页却搜出模组"）。
    ResourceCatalog::Kind untouched = ResourceCatalog::Kind::Shader;
    check(!ResourceCatalog::kindFromCategoryKey(QStringLiteral("game"), &untouched) &&
              untouched == ResourceCatalog::Kind::Shader,
          "Z1 「game」认不出来，且没动传进去的那个值");

    check(ResourceCatalog::folderForKind(ResourceCatalog::Kind::Mod) == QStringLiteral("mods") &&
              ResourceCatalog::folderForKind(ResourceCatalog::Kind::DataPack) ==
                  QStringLiteral("datapacks") &&
              ResourceCatalog::folderForKind(ResourceCatalog::Kind::ResourcePack) ==
                  QStringLiteral("resourcepacks") &&
              ResourceCatalog::folderForKind(ResourceCatalog::Kind::Shader) ==
                  QStringLiteral("shaderpacks"),
          "Z1 模组/数据包/资源包/光影各自的目标目录");
    // 整合包**没有**目标目录：它下回来是个 .mrpack，接着走导入那条路，往哪放是清单说了算。
    // 回空串不是偷懒，是"这一类不走这条安装路"的信号。
    check(ResourceCatalog::folderForKind(ResourceCatalog::Kind::Modpack).isEmpty(),
          "Z1 整合包没有目标目录（它走导入那条路）");

    check(ResourceCatalog::kindLabel(ResourceCatalog::Kind::Mod) == QStringLiteral("模组") &&
              ResourceCatalog::kindLabel(ResourceCatalog::Kind::Modpack) ==
                  QStringLiteral("整合包") &&
              ResourceCatalog::kindLabel(ResourceCatalog::Kind::DataPack) ==
                  QStringLiteral("数据包") &&
              ResourceCatalog::kindLabel(ResourceCatalog::Kind::Shader) == QStringLiteral("光影") &&
              ResourceCatalog::kindLabel(ResourceCatalog::Kind::ResourcePack) ==
                  QStringLiteral("资源包"),
          "Z1 五个名字（和下载页左栏那几个字一样）");

    section("Z2. 搜索地址怎么拼（纯函数，不联网）");

    const QString plain = ResourceCatalog::searchUrl(ResourceCatalog::Kind::Mod,
                                                    QString(), 0, 20);
    check(plain.startsWith(QStringLiteral("https://api.modrinth.com/v2/search?")),
          "Z2 搜索打在 /v2/search 上");

    // 只按类型筛时 facets 是**一层**：`[["project_type:mod"]]`。
    {
        const QJsonDocument doc = jsonOf(urlParam(plain, QStringLiteral("facets")));
        const QJsonArray outer = doc.array();
        const bool shape = outer.size() == 1 && outer.at(0).isArray() &&
                           outer.at(0).toArray().size() == 1 &&
                           outer.at(0).toArray().at(0).toString() ==
                               QStringLiteral("project_type:mod");
        check(shape, "Z2 只按类型筛：facets 是 [ [ project_type:mod ] ]",
              urlParam(plain, QStringLiteral("facets")));
    }

    // 加了游戏版本和加载器就是**两层里三段**（两层 = "且"，这是 Modrinth 的语法糖）。
    {
        const QString filtered = ResourceCatalog::searchUrl(ResourceCatalog::Kind::Shader,
                                                            QString(), 0, 20,
                                                            QStringLiteral("1.21.1"),
                                                            QStringLiteral("iris"));
        const QJsonArray outer = jsonOf(urlParam(filtered, QStringLiteral("facets"))).array();
        QStringList all;
        for (const QJsonValue &group : outer) {
            for (const QJsonValue &value : group.toArray()) {
                all.append(value.toString());
            }
        }
        check(outer.size() == 3 &&
                  all.contains(QStringLiteral("project_type:shader")) &&
                  all.contains(QStringLiteral("versions:1.21.1")) &&
                  all.contains(QStringLiteral("categories:iris")),
              "Z2 类型 + 游戏版本 + 加载器 = 三段筛选",
              all.join(QLatin1Char(' ')));
    }

    check(urlParam(plain, QStringLiteral("offset")) == QStringLiteral("0") &&
              urlParam(plain, QStringLiteral("limit")) == QStringLiteral("20"),
          "Z2 offset / limit 进得去");
    // 空搜索词按下载量排（"最热门的模组"），有搜索词按相关度排 —— 不然搜「sodium」
    // 的头一条会是名字里带 sodium 但没人用的东西。
    check(urlParam(plain, QStringLiteral("index")) == QStringLiteral("downloads") &&
              urlParam(ResourceCatalog::searchUrl(ResourceCatalog::Kind::Mod,
                                                  QStringLiteral("sodium"), 0, 20),
                       QStringLiteral("index")) == QStringLiteral("relevance"),
          "Z2 空搜索按下载量排、有搜索词按相关度排");

    // 搜索词里的 `&` `=` 必须编码掉。不编码的话拼出来的地址会在第一个 `&` 那里断掉，
    // 用户以为自己搜的是 A 实际搜的是 B —— 而且**不报错**，只是结果不对。
    {
        const QString weird = ResourceCatalog::searchUrl(ResourceCatalog::Kind::Mod,
                                                        QStringLiteral("a&b=c"), 0, 20);
        check(urlParam(weird, QStringLiteral("query")) == QStringLiteral("a&b=c") &&
                  weird.contains(QStringLiteral("query=a%26b%3Dc")),
              "Z2 搜索词里的 & 和 = 编码掉了，读回来还是原样");
        // 参数个数靠 `&` 分隔：多出来一个裸 `&` 就是"搜索词把它劈成两段了"。
        int amps = 0;
        for (const QChar &ch : weird) {
            if (ch == QLatin1Char('&')) {
                ++amps;
            }
        }
        check(amps == 4, "Z2 五个参数正好四个分隔符（没有裸 & 混进来）",
              QStringLiteral("数到 %1 个 &").arg(amps));
    }

    check(ResourceCatalog::searchUrl(ResourceCatalog::Kind::Mod, QString(), 40, 20)
              .contains(QStringLiteral("offset=40")),
          "Z2 翻页的 offset 真的换了");

    section("Z3. 读真搜索响应（meta/modrinth-search-mod.json）");

    QByteArray searchJson;
    {
        QFile fixture(metaDir + "/modrinth-search-mod.json");
        const bool haveFixture = fixture.open(QIODevice::ReadOnly);
        check(haveFixture, "Z3 读到了真搜索夹具");
        if (haveFixture) {
            searchJson = fixture.readAll();
            fixture.close();
        }
    }
    ResourceCatalog::SearchResult search = ResourceCatalog::parseSearch(searchJson);

    check(search.ok && search.problem.isEmpty(), "Z3 真响应读得动",
          search.problem);
    check(search.totalHits == 105 && search.hits.size() == 3 && search.limit == 3,
          "Z3 一共多少条 / 这一页几条 都读对了",
          QStringLiteral("total %1、这一页 %2、limit %3")
              .arg(search.totalHits)
              .arg(search.hits.size())
              .arg(search.limit));

    if (search.hits.size() >= 3) {
        const ResourceCatalog::Hit &first = search.hits.at(0);
        check(first.projectId == QStringLiteral("AANobbMI") &&
                  first.slug == QStringLiteral("sodium") &&
                  first.title == QStringLiteral("Sodium") &&
                  first.author == QStringLiteral("jellysquid3"),
              "Z3 第一条的身份字段（Sodium）",
              QStringLiteral("%1 / %2 / %3").arg(first.projectId, first.slug, first.title));
        check(first.downloads == 229545638,
              "Z3 下载量没被 int 截断（2.29 亿 > 32 位）",
              QString::number(first.downloads));
        // 加载器要从那一堆分类标签里挑出来 —— Sodium 的 categories 里还混着 optimization。
        const QStringList loaders = first.loaders();
        check(loaders.contains(QStringLiteral("fabric")) &&
                  loaders.contains(QStringLiteral("neoforge")) &&
                  loaders.contains(QStringLiteral("quilt")) &&
                  !loaders.contains(QStringLiteral("optimization")),
              "Z3 加载器从 categories 里挑得出来，普通标签没混进去",
              loaders.join(QLatin1Char(' ')));
        check(first.gameVersions.contains(QStringLiteral("1.21.1")) &&
                  first.gameVersions.size() > 30,
              "Z3 它支持哪些游戏版本读到了（选游戏版本那一步要用）",
              QStringLiteral("%1 个版本").arg(first.gameVersions.size()));

        // 「选游戏版本」那一列要**新的在前**。而接口给的这份是**旧的在前**（实测 Sodium
        // 从 1.16.3 排到 26.3）—— 和版本接口那边正好相反，所以顺序必须钉住：
        // 反错方向不会报错，只会让用户在一列几十行的最底下找最新版本。
        // 这一条量的是**次序**，所以走 `showSnapshots = true` 那一档：折过之后再比"条数一样、
        // 头对尾"是量不出什么的。折叠本身在 Z8h 里单独验。
        const QStringList choices = ResourceCatalog::gameVersionChoices(first.gameVersions, true);
        check(choices.size() == first.gameVersions.size() &&
                  choices.first() == first.gameVersions.last() &&
                  choices.last() == first.gameVersions.first() &&
                  choices.first() == QStringLiteral("26.3"),
              "Z3 选游戏版本那一列是新的在前（接口给的是旧的在前）",
              choices.isEmpty() ? QString() : choices.first());
    }

    // 图标有**两条**地址：接口给的那条（大多是 webp 缩略图，小 7 倍）原样存进 hit.iconUrl，
    // 自己按 id 拼的那条 PNG 当兜底。这一层不判"这台机器解不解得动" —— 那是 ImageDecode /
    // ResourceStore 的事（见文件头那张表）。
    check(ResourceCatalog::iconUrlForProject(QStringLiteral("AANobbMI")) ==
              QStringLiteral("https://cdn.modrinth.com/data/AANobbMI/icon.png"),
          "Z3 兜底那条是按 project_id 拼出来的 PNG");
    check(ResourceCatalog::iconUrlForProject(QString()).isEmpty(),
          "Z3 没有 id 时不拼出一个空地址");

    // 这三条用的是真响应，里面**两种形状都有**：前两条是 `…_96.webp`，第三条
    // （Bh37bMuy）接口直接给的就是 `…/icon.png`。所以要**按每一条自己判** —— 假定
    // "接口给的一定是 webp"的话，第三条会被送去解 webp，而它本来就是一张 PNG
    // （这正是 isWebpUrl 存在的理由，见 ResourceCatalog.hpp）。
    if (search.hits.size() >= 3) {
        const ResourceCatalog::Hit &sodium = search.hits.at(0);
        check(sodium.iconUrl ==
                  QStringLiteral("https://cdn.modrinth.com/data/AANobbMI/"
                                 "295862f4724dc3f78df3447ad6072b2dcd3ef0c9_96.webp"),
              "Z3 接口给的图标地址**原样**存着（Sodium 那条是带 hash 的 webp 缩略图）",
              sodium.iconUrl);
        check(sodium.iconPngUrl().endsWith(QStringLiteral("/AANobbMI/icon.png")) &&
                  !sodium.iconPngUrl().contains(QStringLiteral(".webp")),
              "Z3 兜底那条仍然是 PNG", sodium.iconPngUrl());
        check(ResourceCatalog::isWebpUrl(sodium.iconUrl),
              "Z3 认出 Sodium 那条是 webp", sodium.iconUrl);
        check(!ResourceCatalog::isWebpUrl(search.hits.at(2).iconUrl) &&
                  search.hits.at(2).iconUrl.endsWith(QStringLiteral("/icon.png")),
              "Z3 第三条接口给的就是 PNG，不能也当成 webp",
              search.hits.at(2).iconUrl);
    }

    section("Z3b. 接口给的图标地址：只认我们自己那个域名");

    // 这个串是**外部输入**（从接口正文里读的），而它的下一个用途是"拿去发一个请求"。
    // 所以三件事都要过：https、host 全等 cdn.modrinth.com、以及空串一律当"没给"。
    check(ResourceCatalog::isUsableIconUrl(
              QStringLiteral("https://cdn.modrinth.com/data/AANobbMI/icon.png")),
          "Z3b 正常的一条放行");
    check(ResourceCatalog::isUsableIconUrl(
              QStringLiteral("https://cdn.modrinth.com/data/a/1_96.webp")),
          "Z3b webp 那条也放行（放行的是域名，不是格式）");
    check(!ResourceCatalog::isUsableIconUrl(QString()),
          "Z3b 没给地址 = 不能用（调用方退回自己拼的那条）");
    check(!ResourceCatalog::isUsableIconUrl(
              QStringLiteral("http://cdn.modrinth.com/data/a/icon.png")),
          "Z3b 明文 http 不放行");
    // 这一条是"看着像"的那种：光用 endsWith / contains 判域名的话它会被放行。
    check(!ResourceCatalog::isUsableIconUrl(
              QStringLiteral("https://cdn.modrinth.com.别的.com/data/a/icon.png")),
          "Z3b cdn.modrinth.com.别的.com 不放行（比的是 host 全等）");
    check(!ResourceCatalog::isUsableIconUrl(
              QStringLiteral("https://api.modrinth.com/data/a/icon.png")),
          "Z3b 别的 modrinth 子域不放行");
    check(!ResourceCatalog::isUsableIconUrl(
              QStringLiteral("https://example.com/data/a/icon.png")) &&
              !ResourceCatalog::isUsableIconUrl(
                  QStringLiteral("file:///C:/Windows/System32/calc.exe")),
          "Z3b 别的域名 / 本地文件路径都不放行");

    // 没给 icon_url 的项目（或者给了条别的域名的）→ Hit::iconUrl 是空的，界面走
    // iconPngUrl() 那条。**这一条不能把整个搜索判成失败** —— 少一张图标而已。
    {
        const QByteArray thin = QByteArrayLiteral(
            "{\"hits\":[{\"project_id\":\"AAA\",\"title\":\"x\","
            "\"icon_url\":\"https://evil.example.com/i.png\"},"
            "{\"project_id\":\"BBB\",\"title\":\"y\"}],"
            "\"total_hits\":2,\"offset\":0,\"limit\":2}");
        const ResourceCatalog::SearchResult result = ResourceCatalog::parseSearch(thin);
        check(result.ok && result.hits.size() == 2 &&
                  result.hits.at(0).iconUrl.isEmpty() && result.hits.at(1).iconUrl.isEmpty(),
              "Z3b 别的域名的 / 压根没给的 icon_url 一律当「没给」，搜索照样是成功的");
        check(result.hits.at(1).iconPngUrl().endsWith(QStringLiteral("/BBB/icon.png")),
              "Z3b 当没给之后，兜底那条还是拼得出来");
    }

    section("Z3c. 批量问图标地址（`GET /projects?ids=…`）");

    check(ResourceCatalog::projectsRequestUrl({QStringLiteral("AANobbMI"),
                                               QStringLiteral("P7dR8mSH")}) ==
              QStringLiteral("https://api.modrinth.com/v2/projects?ids="
                             "%5B%22AANobbMI%22%2C%22P7dR8mSH%22%5D"),
          "★ Z3c ids 那一段是**百分号编码过的 JSON 数组**（写成 `ids=AANobbMI,P7dR8mSH` "
          "这一家不认；这一串回 200、两个项目都在）",
          ResourceCatalog::projectsRequestUrl({QStringLiteral("AANobbMI"),
                                               QStringLiteral("P7dR8mSH")}));
    check(ResourceCatalog::projectsRequestUrl({QStringLiteral("AANobbMI")},
                                              QStringLiteral("http://127.0.0.1:9/v2")) ==
              QStringLiteral("http://127.0.0.1:9/v2/projects?ids=%5B%22AANobbMI%22%5D"),
          "Z3c 验证工程那个 override 是指根地址（和 /version_files 那一发同一条规矩）");
    check(ResourceCatalog::projectsRequestUrl({}).isEmpty(),
          "★ Z3c 一个号都没有时**不拼地址**（拼出来是个 `ids=[]`，白发一发还白等一次超时）");

    // 答复是一个**数组**（不是搜索那种 `{hits:[…]}`）——按对象读会一条都读不出来，
    // 而"没有图标"和"读不懂"在屏幕上是一个样子。
    {
        const QByteArray reply = QByteArrayLiteral(
            "[{\"id\":\"AANobbMI\",\"icon_url\":\"https://cdn.modrinth.com/data/AANobbMI/"
            "295862f4724dc3f78df3447ad6072b2dcd3ef0c9_96.webp\"},"
            "{\"id\":\"P7dR8mSH\",\"icon_url\":\"https://cdn.modrinth.com/data/P7dR8mSH/"
            "icon.png\"},"
            "{\"id\":\"NOPE0001\",\"icon_url\":\"https://evil.example.com/i.png\"},"
            "{\"id\":\"NOPE0002\"}]");
        const QHash<QString, QString> icons = ResourceCatalog::parseProjectIconUrls(reply);
        check(icons.value(QStringLiteral("AANobbMI")) ==
                  QStringLiteral("https://cdn.modrinth.com/data/AANobbMI/"
                                 "295862f4724dc3f78df3447ad6072b2dcd3ef0c9_96.webp") &&
                  icons.value(QStringLiteral("P7dR8mSH")) ==
                      QStringLiteral("https://cdn.modrinth.com/data/P7dR8mSH/icon.png"),
              "★ Z3c 接口给的那条地址原样读出来（各项目一个哈希，自己拼不出来）");
        check(icons.contains(QStringLiteral("NOPE0001")) &&
                  icons.value(QStringLiteral("NOPE0001")).isEmpty(),
              "★ Z3c 别家域名的 `icon_url` **当没给**（外部输入，过域名闸 —— 见 isUsableIconUrl）");
        check(icons.contains(QStringLiteral("NOPE0002")) &&
                  icons.value(QStringLiteral("NOPE0002")).isEmpty(),
              "Z3c 没带 `icon_url` 的项目 = 没有图标地址（那一格画占位拼图，不是错）");
        check(icons.size() == 4,
              "Z3c 四个项目都在表里（少一个 = 那个项目永远只有占位图，而找不出原因）",
              QString::number(icons.size()));
    }
    check(ResourceCatalog::parseProjectIconUrls(QByteArrayLiteral("{\"hits\":[]}")).isEmpty(),
          "★ Z3c 回了个**对象**（不是数组）时一条都不读 —— 形状不对就是没读到，不猜");
    check(ResourceCatalog::parseProjectIconUrls(QByteArrayLiteral("不是 JSON")).isEmpty() &&
              ResourceCatalog::parseProjectIconUrls(QByteArray()).isEmpty(),
          "Z3c 读不懂 / 空正文 → 空表（不是崩、也不是塞一条空的进来）");

    section("Z4. 读不动 / 没搜到 是两句话");

    // 「没搜到」不是错（下面 Z4b），但「没搜成」必须如实报，不能返回一个空列表 ——
    // 空列表到界面上就是"什么都没搜到"，用户会去改搜索词，而真正的问题在网络那头。
    {
        const ResourceCatalog::SearchResult empty =
            ResourceCatalog::parseSearch(QStringLiteral("{}").toUtf8());
        check(!empty.ok && !empty.problem.isEmpty(),
              "Z4 回了个空对象：报错，不是「没搜到」", empty.problem);

        const ResourceCatalog::SearchResult broken =
            ResourceCatalog::parseSearch(QByteArray("<html>502 Bad Gateway</html>"));
        check(!broken.ok && !broken.problem.isEmpty(),
              "Z4 回了一段 HTML（网关挡了）：报错", broken.problem);

        // 真接口出错时回的是 `{"error":..,"description":..}`。把那句话原样带给用户 ——
        // 它比我们编的"搜索失败"有用得多。
        const ResourceCatalog::SearchResult apiError = ResourceCatalog::parseSearch(
            QByteArray(R"({"error":"invalid_input","description":"Invalid facets"})"));
        check(!apiError.ok && apiError.problem.contains(QStringLiteral("Invalid facets")),
              "Z4 接口自己说的话带出来了", apiError.problem);

        // 真回了个空数组 = 问到了、就是没有。这是 ok。
        const ResourceCatalog::SearchResult none = ResourceCatalog::parseSearch(
            QByteArray(R"({"hits":[],"offset":0,"limit":20,"total_hits":0})"));
        check(none.ok && none.hits.isEmpty() && none.totalHits == 0,
              "Z4b 空 hits 算「问到了、没有」（ok=true）", none.problem);
    }

    section("Z5. 下载量那串字（唯一一处换单位）");

    check(ResourceCatalog::formatDownloads(0) == QStringLiteral("0") &&
              ResourceCatalog::formatDownloads(9999) == QStringLiteral("9999") &&
              ResourceCatalog::formatDownloads(10000) == QStringLiteral("1.0 万") &&
              ResourceCatalog::formatDownloads(1234567) == QStringLiteral("123.5 万") &&
              ResourceCatalog::formatDownloads(99990000) == QStringLiteral("9999.0 万") &&
              ResourceCatalog::formatDownloads(229545638) == QStringLiteral("2.3 亿"),
          "Z5 万 / 亿 两个坎上的输出",
          QStringLiteral("%1 | %2 | %3 | %4")
              .arg(ResourceCatalog::formatDownloads(10000),
                   ResourceCatalog::formatDownloads(1234567),
                   ResourceCatalog::formatDownloads(99990000),
                   ResourceCatalog::formatDownloads(229545638)));
    check(ResourceCatalog::formatDownloads(-1).isEmpty(),
          "Z5 负数（接口没给）回空串，不回「-1」");

    section("Z6. 版本列表地址与真版本列表（meta/modrinth-versions-iris.json）");

    {
        const QString url = ResourceCatalog::versionsUrl(QStringLiteral("YL57xq9U"),
                                                        QStringLiteral("1.21.1"),
                                                        QStringLiteral("fabric"));
        const QJsonDocument gv = jsonOf(urlParam(url, QStringLiteral("game_versions")));
        const QJsonDocument ld = jsonOf(urlParam(url, QStringLiteral("loaders")));
        check(gv.isArray() && gv.array().size() == 1 &&
                  gv.array().at(0).toString() == QStringLiteral("1.21.1") &&
                  ld.isArray() && ld.array().size() == 1 &&
                  ld.array().at(0).toString() == QStringLiteral("fabric"),
              "Z6 版本地址按游戏版本 + 加载器筛",
              url);

        // 没选游戏版本时**不问**筛条件（界面在用户还没选版本时不该把范围缩掉）。
        // 唯一常带的那条是 `include_changelog=false`（ 照 PCL 学的：这一层
        // 一个字节的 changelog 都不读，接口默认却把每个文件的更新日志原文塞回来）。
        const QString bare = ResourceCatalog::versionsUrl(QStringLiteral("YL57xq9U"),
                                                          QString());
        check(bare.contains(QStringLiteral("/project/YL57xq9U/version")),
              "Z6 不筛的时候地址仍指向这个项目的版本列表", bare);
        check(urlParam(bare, QStringLiteral("include_changelog")) == QStringLiteral("false"),
              "Z6 不问更新日志（include_changelog=false）", bare);
        check(urlParam(bare, QStringLiteral("game_versions")).isEmpty() &&
                  urlParam(bare, QStringLiteral("loaders")).isEmpty(),
              "Z6 不筛的时候不带 game_versions / loaders", bare);
    }

    QByteArray versionsJson;
    {
        QFile fixture(metaDir + "/modrinth-versions-iris.json");
        const bool haveFixture = fixture.open(QIODevice::ReadOnly);
        check(haveFixture, "Z6 读到了真版本列表夹具");
        if (haveFixture) {
            versionsJson = fixture.readAll();
            fixture.close();
        }
    }
    const QList<ResourceCatalog::VersionEntry> versions =
        ResourceCatalog::parseVersions(versionsJson);

    check(versions.size() == 12, "Z6 12 个版本一条不多一条不少",
          QStringLiteral("%1 条").arg(versions.size()));

    if (versions.size() == 12) {
        // **顺序**：接口给的是**新的在前**。一开始按"大概是从旧到新"的直觉写了个反向，
        // 那样列表头条会是最老的那个版本，而且不报错 —— 所以这条要钉住。
        bool descending = true;
        for (int i = 1; i < versions.size(); ++i) {
            if (versions.at(i).datePublished >= versions.at(i - 1).datePublished) {
                descending = false;
            }
        }
        check(descending && versions.first().datePublished > versions.last().datePublished,
              "Z6 顺序是新在前（别再反向遍历一次）",
              QStringLiteral("%1 → %2").arg(versions.first().versionNumber,
                                            versions.last().versionNumber));

        const ResourceCatalog::VersionEntry &newest = versions.first();
        check(newest.id == QStringLiteral("bAo1Qhte") &&
                  newest.versionNumber == QStringLiteral("1.8.14-beta.1+1.21.1-fabric") &&
                  newest.versionType == QStringLiteral("beta"),
              "Z6 最新那条的身份字段",
              QStringLiteral("%1 (%2)").arg(newest.versionNumber, newest.versionType));
        check(newest.gameVersions.contains(QStringLiteral("1.21.1")) &&
                  newest.loaders.contains(QStringLiteral("fabric")),
              "Z6 它自己写着支持 1.21.1 / fabric",
              newest.gameVersions.join(QLatin1Char(' ')));

        // 每一条都得有一个能下的文件 —— 界面上"点不动"比"下错了"更难查。
        int withFile = 0;
        int badSha = 0;
        for (const ResourceCatalog::VersionEntry &version : versions) {
            const ResourceCatalog::FileEntry *file = version.primaryFile();
            if (file == nullptr || file->url.isEmpty() || file->filename.isEmpty() ||
                file->size <= 0) {
                continue;
            }
            ++withFile;
            if (file->sha1.size() != 40) {
                ++badSha;
            }
        }
        check(withFile == 12, "Z6 12 条都能挑出一个要下的文件",
              QStringLiteral("%1/12").arg(withFile));
        // sha1 要留着 —— 下载那边就是靠它校验"下全了没有"（这是"重下=校验跳过"那条的判据）。
        check(badSha == 0, "Z6 每条都带 40 位的 sha1",
              QStringLiteral("%1 条不对").arg(badSha));

        // 前置模组：Iris 最新那条钉着 Sodium（required）。这一轮不出按钮，但数据得先读对 ——
        // 读错了等症状暴露的时候，是在"装完启动崩"那里，离这里很远。
        const QList<ResourceCatalog::Dependency> deps =
            ResourceCatalog::requiredDependenciesOf(versions, newest.id);
        check(deps.size() == 1 && deps.at(0).projectId == QStringLiteral("AANobbMI") &&
                  deps.at(0).required(),
              "Z6 前置模组读出来了（Iris → Sodium）",
              deps.isEmpty() ? QStringLiteral("一条都没有")
                             : deps.at(0).projectId);
        // 钉错的版本号不该"顺手返回第一条"——那会给用户装一个不匹配的前置。
        check(ResourceCatalog::requiredDependenciesOf(versions,
                                                     QStringLiteral("不存在的一版"))
                  .isEmpty(),
              "Z6 版本号对不上时回空清单，不猜一条");
    }

    // 读不动就回空列表：界面那边是"这个资源在 1.21.1 上没有版本"，不是崩。
    check(ResourceCatalog::parseVersions(QByteArray("not json")).isEmpty(),
          "Z6b 版本列表读不动时回空列表");
    {
        const auto parsed = ResourceCatalog::parseCurseForgeVersions(R"({"data":[{"id":1,"fileName":"a.jar","fileLength":1,"downloadUrl":"https://example.invalid/a.jar","dependencies":[{"modId":42,"fileId":0,"relationType":3},{"fileId":999,"relationType":3},{"modId":-5,"fileId":0,"relationType":3}]}]})");
        check(parsed.size() == 1 && parsed.first().dependencies.size() == 3
            && parsed.first().dependencies[0].projectId == QStringLiteral("42") && parsed.first().dependencies[0].versionId.isEmpty()
            && parsed.first().dependencies[1].projectId.isEmpty() && parsed.first().dependencies[1].versionId == QStringLiteral("999")
            && parsed.first().dependencies[2].projectId.isEmpty() && parsed.first().dependencies[2].versionId.isEmpty(),
            "CF dependency identities preserve explicit file pins, never interpret zero as latest, and reject negative project IDs");
    }

    // primaryFile 的两条兜底：都没有 primary 时回第一条，空清单回 nullptr。
    {
        ResourceCatalog::VersionEntry noPrimary;
        ResourceCatalog::FileEntry a;
        a.filename = QStringLiteral("a.jar");
        ResourceCatalog::FileEntry b;
        b.filename = QStringLiteral("b.jar");
        noPrimary.files = {a, b};
        const bool first = noPrimary.primaryFile() != nullptr &&
                           noPrimary.primaryFile()->filename == QStringLiteral("a.jar");
        ResourceCatalog::VersionEntry empty;
        check(first && empty.primaryFile() == nullptr,
              "Z6c 没有 primary 时回第一条；没有文件时回 nullptr");
    }

    section("Z6d. 前置模组：缺哪些 / 去哪问 / 挑哪一版（一键下载那一批）");

    // 用户 ：「在模组下载页，选择模组后显示其依赖，点击下载后弹出窗口，让用户
    // 可以点击一键下载所有缺少的依赖」。这一带钉的是**判断**那一半（纯函数），
    // 界面上那几个框怎么弹、下载批次怎么开在 MainWindow 里。
    {
        ModDependencies::Target target;
        target.instanceId = QStringLiteral("1.21.1-fabric");
        target.gameVersion = QStringLiteral("1.21.1");
        target.loader = QStringLiteral("fabric");

        // ① "装过的算不算装了"这件事的键，必须和图标缓存那张表**同一个拼法** ——
        // 对不上等于"装过的前置每次都当成没装、再下一遍"，而且不报错。
        check(target.installedKey(QStringLiteral("AANobbMI")) ==
                  ResourceCatalog::Hit{.projectId = QStringLiteral("AANobbMI"),
                                       .source = ResourceCatalog::Source::Modrinth}
                      .iconCacheId(),
              "Z6d 已装的键和 Hit::iconCacheId() 一字不差（两处同一个拼法）",
              target.installedKey(QStringLiteral("AANobbMI")));

        const ResourceCatalog::VersionEntry newest = versions.first();

        // ② 什么都没装：缺它声明的那一条（Iris 最新版钉着 Sodium）。
        const QList<ModDependencies::Lookup> missing =
            ModDependencies::missingRequiredDependencies(newest, {}, target);
        check(missing.size() == 1 &&
                  missing.first().dependency.projectId == QStringLiteral("AANobbMI") &&
                  missing.first().dependency.versionId == QStringLiteral("s7adptIg"),
              "Z6d 一条都没装时，缺的还是那一条（Sodium，且钉着版本）",
              QStringLiteral("%1 条").arg(missing.size()));
        // 地址和钥匙得**跟着目标实例筛**：带上游戏版本和加载器，否则会挑出一个别的加载器
        // 的前置装进这个实例（症状是"装上了、进去就崩"，离这里很远）。
        if (missing.size() == 1) {
            const QString url = missing.first().url;
            const QString key = missing.first().requestKey;
            const QJsonDocument gv = jsonOf(urlParam(url, QStringLiteral("game_versions")));
            const QJsonDocument ld = jsonOf(urlParam(url, QStringLiteral("loaders")));
            check(gv.isArray() && gv.array().size() == 1 &&
                      gv.array().at(0).toString() == QStringLiteral("1.21.1") &&
                      ld.isArray() && ld.array().size() == 1 &&
                      ld.array().at(0).toString() == QStringLiteral("fabric"),
                  "Z6d 去问前置版本列表时带着目标实例的游戏版本 + 加载器", url);
            // 钥匙 = `versionsRequestKey()` **再加一个消费方后缀**。后缀那一截不是装饰：
            // `ResourceStore::fetchVersions()` 按钥匙去重（同一把钥匙再来一发会把前一发
            // abort 掉），而"详情页在读这个项目的版本"和"前置检查在读它"是两件事 ——
            // 不区分的话后发那一发把先发掐掉，先发那一趟永远等不到自己的回复
            // （症状正是"按了没反应"）。
            check(key == ResourceCatalog::versionsRequestKey(QStringLiteral("AANobbMI"),
                                                            QStringLiteral("1.21.1"),
                                                            QStringLiteral("fabric")) +
                             QStringLiteral("|dependency"),
                  "Z6d 钥匙是 versionsRequestKey 加一个消费方后缀（自己拼会漏一处）", key);
            // 两个消费方**必须**是两把钥匙：同一把的话上面那个坑原样复现。
            check(ModDependencies::versionsKeyFor(QStringLiteral("dependency"),
                                                  QStringLiteral("AANobbMI"),
                                                  QStringLiteral("1.21.1"),
                                                  QStringLiteral("fabric")) !=
                      ModDependencies::versionsKeyFor(
                          QStringLiteral("update"), QStringLiteral("AANobbMI"),
                          QStringLiteral("1.21.1"), QStringLiteral("fabric")),
                  "Z6d 前置那一趟和更新检测那一趟的钥匙不是同一把（不然互相 abort）");
        }

        // ③ 已经装着了：一条都不缺。
        const QSet<QString> installed{target.installedKey(QStringLiteral("AANobbMI"))};
        check(ModDependencies::missingRequiredDependencies(newest, installed, target).isEmpty(),
              "Z6d 已经装过的前置不再下一次");

        // ④ ★ 但"装着的是**另一家**的同号项目"**不算装了**。两个来源的 id 各在自己那套里
        // 唯一，`238222` 这种纯数字 id 在 Modrinth 那条 base62 里也合法 —— 不带来源
        // 去比的话，这里会得出"已经装过了"、于是**一个前置都不下**，而用户手上那个
        // mods/ 里根本没有它。
        const QSet<QString> otherSource{
            ResourceCatalog::Hit{.projectId = QStringLiteral("AANobbMI"),
                                 .source = ResourceCatalog::Source::CurseForge}
                .iconCacheId()};
        check(ModDependencies::missingRequiredDependencies(newest, otherSource, target).size() == 1,
              "Z6d 别家同号的项目不算「已经装了」（来源要进判据）");

        // ⑤ optional / 没写项目号的都不下（后者是接口给的烂数据）。
        ResourceCatalog::VersionEntry handmade;
        ResourceCatalog::Dependency optionalDep;
        optionalDep.projectId = QStringLiteral("aaaaaaa1");
        optionalDep.type = QStringLiteral("optional");
        ResourceCatalog::Dependency blankDep;
        blankDep.type = QStringLiteral("required");
        ResourceCatalog::Dependency requiredDep;
        requiredDep.projectId = QStringLiteral("bbbbbbb2");
        requiredDep.type = QStringLiteral("required");
        handmade.dependencies = {optionalDep, blankDep, requiredDep};
        const QList<ModDependencies::Lookup> onlyReal =
            ModDependencies::missingRequiredDependencies(handmade, {}, target);
        check(onlyReal.size() == 1 &&
                  onlyReal.first().dependency.projectId == QStringLiteral("bbbbbbb2"),
              "Z6d optional 和没写项目号的都不进这一批，只留下那一条真的",
              QStringLiteral("%1 条").arg(onlyReal.size()));

        // ⑥ ★ Fabric API 不列（用户 ：「所有的 fabric 模组当然都要用 fabricAPI，
        // 这个是前置简直是废话，需要的前置列表里排除一下吧」）。两家各一个号，都要认。
        {
            ResourceCatalog::VersionEntry fabricMod;
            ResourceCatalog::Dependency mrApi;
            mrApi.projectId = QStringLiteral("P7dR8mSH");  // Modrinth: fabric-api
            mrApi.type = QStringLiteral("required");
            ResourceCatalog::Dependency cfApi;
            cfApi.projectId = QStringLiteral("306612");  // CurseForge: Fabric API
            cfApi.type = QStringLiteral("required");
            ResourceCatalog::Dependency cloth;
            cloth.projectId = QStringLiteral("9s6osm5g");  // Cloth Config：真该列出来的那种
            cloth.type = QStringLiteral("required");
            fabricMod.dependencies = {mrApi, cfApi, cloth};

            const QList<ResourceCatalog::Dependency> listed =
                ModDependencies::displayedRequiredDependencies(fabricMod);
            check(listed.size() == 1 &&
                      listed.first().projectId == QStringLiteral("9s6osm5g"),
                  "★★ Z6d Fabric API 不进「这一版要装的前置」—— Modrinth 和 CurseForge 两家的号都认",
                  QStringLiteral("%1 条").arg(listed.size()));

            // 隐藏只是展示规则：真安装判定不能跟着删。否则用户点
            // 「一起下载」也不会安装 Fabric API，最后得到一份缺真前置的实例。
            const QList<ModDependencies::Lookup> missingWithApi =
                ModDependencies::missingRequiredDependencies(fabricMod, {}, target);
            check(missingWithApi.size() == 3,
                  "★★ Z6d Fabric API 只在详情展示中隐藏，缺失判定仍保留真依赖",
                  QStringLiteral("%1 条").arg(missingWithApi.size()));

            // 是**按号**排的，不是"看着像"就排：差一位的两个号一个都不能误伤。
            ResourceCatalog::VersionEntry lookalikes;
            ResourceCatalog::Dependency nearMr;
            nearMr.projectId = QStringLiteral("P7dR8mS");  // 少一位
            nearMr.type = QStringLiteral("required");
            ResourceCatalog::Dependency nearCf;
            nearCf.projectId = QStringLiteral("3066120");  // 多一位
            nearCf.type = QStringLiteral("required");
            lookalikes.dependencies = {nearMr, nearCf};
            check(ModDependencies::displayedRequiredDependencies(lookalikes).size() == 2,
                  "★ Z6d 排的是那两个**号**，不是「名字里带 fabric 的都排」"
                  "（差一位的号一个都不能误伤）",
                  QStringLiteral("%1 条")
                      .arg(ModDependencies::requiredDependenciesOf(lookalikes).size()));
        }

        // ⑦ ★ 同一版的前置清单里**同一个项目出现两次**（R7）：只留一条。
        // 两发同钥匙的版本请求在 `ResourceStore` 那边第二发会把第一发掐掉、而且不回调；
        // 调用方按条数等回信的话 `pending` 永远停在 1，这一趟永远收不了尾 ——
        // 之后所有模组/光影安装都被挡着，直到重启。
        {
            ResourceCatalog::VersionEntry duplicated;
            ResourceCatalog::Dependency repeated;
            repeated.projectId = QStringLiteral("ccccccc3");
            repeated.type = QStringLiteral("required");
            ResourceCatalog::Dependency repeatedAgain = repeated;
            ResourceCatalog::Dependency other;
            other.projectId = QStringLiteral("ddddddd4");
            other.type = QStringLiteral("required");
            duplicated.dependencies = {repeated, repeatedAgain, other};

            const QList<ModDependencies::Lookup> deduped =
                ModDependencies::missingRequiredDependencies(duplicated, {}, target);
            check(deduped.size() == 2 &&
                      deduped.at(0).dependency.projectId == QStringLiteral("ccccccc3") &&
                      deduped.at(1).dependency.projectId == QStringLiteral("ddddddd4"),
                  "★ R7 同一项目出现两次时只留一条（顺序原样，第二发不会多发一把钥匙）",
                  QStringLiteral("%1 条").arg(deduped.size()));
            if (deduped.size() == 2) {
                check(deduped.at(0).requestKey != deduped.at(1).requestKey,
                      "★ R7 留下的两条钥匙互不相同（同一把钥匙的第二发会掐掉第一发）");
            }
        }
    }

    // 挑一版：钉了版本必须正好是那条、没钉取最新的一条；挑不出来要**说得出为什么**。
    {
        ModDependencies::Resolved picked;
        QString why;

        // 钉了版本、候选表里正好有它。
        ResourceCatalog::Dependency pinned;
        pinned.projectId = QStringLiteral("AANobbMI");
        pinned.versionId = QStringLiteral("s7adptIg");
        pinned.type = QStringLiteral("required");
        // 真列表里**没有** s7adptIg 这一版（Iris 那份是它自己的版本，不是 Sodium 的）——
        // 所以先用真列表钉"钉的版本找不到时不许退而求其次"。
        const bool found = ModDependencies::resolveDependency(pinned, versions, &picked, &why);
        check(!found && why.contains(QStringLiteral("s7adptIg")),
              "Z6d 作者钉的那一版不在列表里 -> 解不开（不许换成第一条），且理由里带着那个号",
              why);

        // 自己造一张候选表：三条，第 0 条就是钉的那一条。
        ResourceCatalog::VersionEntry v0;
        v0.id = QStringLiteral("s7adptIg");
        v0.versionNumber = QStringLiteral("0.6.13");
        ResourceCatalog::FileEntry f0;
        f0.url = QStringLiteral("https://example.invalid/sodium-0.6.13.jar");
        f0.filename = QStringLiteral("sodium-0.6.13.jar");
        f0.sha1 = QStringLiteral("1111111111111111111111111111111111111111");
        v0.files = {f0};
        ResourceCatalog::VersionEntry v1;
        v1.id = QStringLiteral("older001");
        v1.versionNumber = QStringLiteral("0.6.12");
        ResourceCatalog::FileEntry f1;
        f1.url = QStringLiteral("https://example.invalid/sodium-0.6.12.jar");
        f1.filename = QStringLiteral("sodium-0.6.12.jar");
        f1.sha1 = QStringLiteral("2222222222222222222222222222222222222222");
        v1.files = {f1};
        const QList<ResourceCatalog::VersionEntry> candidates{v0, v1};

        why.clear();
        check(ModDependencies::resolveDependency(pinned, candidates, &picked, &why) &&
                  picked.version.id == QStringLiteral("s7adptIg") &&
                  picked.file.filename == QStringLiteral("sodium-0.6.13.jar"),
              "Z6d 钉了版本且找得到 -> 就下那一版那个文件", why);

        // 没钉（"哪个版本都行"）-> 取**列表第一条**。这份列表接口给的就是新的在前。
        ResourceCatalog::Dependency loose;
        loose.projectId = QStringLiteral("AANobbMI");
        loose.type = QStringLiteral("required");
        why.clear();
        check(ModDependencies::resolveDependency(loose, candidates, &picked, &why) &&
                  picked.version.id == QStringLiteral("s7adptIg"),
              "Z6d 没钉版本 -> 拿列表第一条（接口给的就是新的在前）", why);
        // 这一条钉的是"不按版本号比大小"：把两条**倒过来**摆，挑出来的必须还是第一条
        // （`0.6.12` 那条）。按版本号挑的话这一条会红 —— 而那个比较器（"1.9 比 1.10 旧"）
        // 写错了不会有任何东西报警。
        const QList<ResourceCatalog::VersionEntry> reversed{v1, v0};
        why.clear();
        check(ModDependencies::resolveDependency(loose, reversed, &picked, &why) &&
                  picked.version.id == QStringLiteral("older001"),
              "Z6d 盯的是**次序**不是版本号大小：倒过来摆就挑另一条", why);

        // 挑出来的那一条没有可下载的文件：CurseForge 那种"作者关掉了第三方分发"的形态 ——
        // 那时候要给的是那边写好的那句话，不是"找不到版本"。
        ResourceCatalog::VersionEntry noUrl;
        noUrl.id = QStringLiteral("nofile01");
        noUrl.versionNumber = QStringLiteral("1.0");
        ResourceCatalog::FileEntry blocked;
        blocked.filename = QStringLiteral("blocked.jar");
        blocked.unavailableNote = QStringLiteral("作者关掉了这个文件的第三方分发。");
        noUrl.files = {blocked};
        why.clear();
        check(!ModDependencies::resolveDependency(loose, {noUrl}, &picked, &why) &&
                  why == blocked.unavailableNote,
              "Z6d 挑出来的那一版没有地址 -> 解不开，理由就是那边写好的那句", why);

        why.clear();
        check(!ModDependencies::resolveDependency(loose, {}, &picked, &why) && !why.isEmpty(),
              "Z6d 一条候选都没有 -> 解不开（空表不等于「已是最新」那种「没问题」）", why);
    }

    // buildPlan：一批答案 -> 要下的 + 解不开的（那几句是给用户看的，得带上是哪个前置）。
    {
        ModDependencies::Target target;
        target.gameVersion = QStringLiteral("1.21.1");
        target.loader = QStringLiteral("fabric");
        const QList<ModDependencies::Lookup> missing =
            ModDependencies::missingRequiredDependencies(versions.first(), {}, target);
        check(missing.size() == 1, "Z6d buildPlan 的输入（那条 Sodium）");

        if (missing.size() == 1) {
            const QString key = missing.first().requestKey;

            // ① 答案缺席 = 那一发没回来，和"它没有版本"**不是同一句话**。
            const ModDependencies::Plan none = ModDependencies::buildPlan(missing, {});
            check(none.downloads.isEmpty() && none.unresolved.size() == 1 &&
                      none.unresolved.first().contains(QStringLiteral("无法获取版本列表")),
                  "Z6d 请求未完成时保留正式的版本列表错误说明",
                  none.unresolved.join(QStringLiteral(" | ")));

            // ② 有答案：挑出要下的那一条，而且名字用的是接口那份（不是项目号）。
            ResourceCatalog::VersionEntry sodium;
            sodium.id = QStringLiteral("s7adptIg");
            sodium.versionNumber = QStringLiteral("0.6.13");
            ResourceCatalog::FileEntry file;
            file.url = QStringLiteral("https://example.invalid/sodium-0.6.13.jar");
            file.filename = QStringLiteral("sodium-0.6.13.jar");
            sodium.files = {file};
            const QHash<QString, QList<ResourceCatalog::VersionEntry>> answers{
                {key, {sodium}}};
            const QHash<QString, QString> titles{
                {QStringLiteral("AANobbMI"), QStringLiteral("Sodium")}};
            const ModDependencies::Plan plan =
                ModDependencies::buildPlan(missing, answers, titles);
            check(plan.downloads.size() == 1 && plan.unresolved.isEmpty() &&
                      plan.downloads.first().file.filename ==
                          QStringLiteral("sodium-0.6.13.jar") &&
                      plan.downloads.first().title == QStringLiteral("Sodium"),
                  "Z6d 有答案 -> 一版一文件，名字是接口那份",
                  plan.downloads.isEmpty() ? QString() : plan.downloads.first().title);

            // ③ 名字取不到时退成项目号（列出来总比"有一个前置没找到"强）。
            const ModDependencies::Plan unnamed = ModDependencies::buildPlan(missing, {});
            check(unnamed.unresolved.size() == 1 &&
                      unnamed.unresolved.first().startsWith(QStringLiteral("AANobbMI")),
                  "Z6d 名字取不到就用项目号，不写一个没有主语的句子",
                  unnamed.unresolved.join(QStringLiteral(" | ")));
        }
    }

    // 那几句话本身（界面和验证都读它，别在别处再写一遍）。
    {
        const QString prompt = ModDependencies::dependencyPromptText(
            ResourceCatalog::Kind::Mod,
            {QStringLiteral("Sodium"), QStringLiteral("Fabric API")});
        // 正文 = 句头 + 名单 + 「要一起下载吗？」。名单**一条一行、前面带短横**（用户
        // 原话「模组名称，一行一个」）。不写"这几个还没装"里的**计数** ——
        // 名单里几条一眼就看得见，而截断那一档写的是"还有 N 个未列出"。
        check(prompt.contains(QStringLiteral("- Sodium\n- Fabric API")) &&
                  prompt.contains(ModDependencies::dependencyPromptHead(ResourceCatalog::Kind::Mod)) &&
                  prompt.trimmed().endsWith(QStringLiteral("要一起下载吗？")),
              "Z6d 弹窗正文 = 句头 + 一条一行的名单 + 那句问句", prompt);
        check(ModDependencies::dependencyPromptText(ResourceCatalog::Kind::Mod, {}).isEmpty(),
              "Z6d 一条都不缺时那句话是空串（空表不该弹这个框）");
        check(ModDependencies::dependencyAllButtonText() == QStringLiteral("一起下载") &&
                  ModDependencies::dependencyOnlyThisButtonText() == QStringLiteral("只下这个"),
              "Z6d 两颗按钮的字（用户要的「一键下载」就在这一颗上）",
              ModDependencies::dependencyAllButtonText());
        check(ModDependencies::unresolvedNoteText({}).isEmpty() &&
                  ModDependencies::unresolvedNoteText({QStringLiteral("Sodium：没问到")})
                      .contains(QStringLiteral("Sodium：没问到")),
              "Z6d 没解开那几条的原话会被列出来；空表回空串");

        // ---- 句头认类别（：光影包也查前置）----
        check(ModDependencies::dependencyPromptHead(ResourceCatalog::Kind::Mod) ==
                      QStringLiteral("此模组缺失必要的前置：") &&
                  ModDependencies::dependencyPromptHead(ResourceCatalog::Kind::Shader) ==
                      QStringLiteral("此光影包缺失必要的前置："),
              "★ Z6d 句头分模组 / 光影包两句（说成「此模组」的框摆在一个光影包前面就是错的）",
              ModDependencies::dependencyPromptHead(ResourceCatalog::Kind::Shader));
        check(ModDependencies::dependencyPromptText(ResourceCatalog::Kind::Shader,
                                                    {QStringLiteral("Iris")})
                  .startsWith(ModDependencies::dependencyPromptHead(ResourceCatalog::Kind::Shader)),
              "★ Z6d 光影那一份正文用的就是光影的句头（正文和句头不许各写一份）",
              ModDependencies::dependencyPromptText(ResourceCatalog::Kind::Shader,
                                                    {QStringLiteral("Iris")}));

        // ---- 名单的截断规则（用户 点名，两条边界都要钉）----
        //
        // 原话：「如果多于 3 个的话写 还有 x 个未列出...（当然如果刚好还剩一个就列出来了
        // 不要搞一行显示 1 个未列出这种高血压操作）」。翻成判据：
        // · **总数 ≤ 4 ⇒ 全列**（4 条这一档就是"刚好还剩一个"，它走全列）；
        // · **总数 > 4 ⇒ 列前 3 条 + 「还有 N 个未列出…」**。
        // 所以「还有 1 个未列出」**永远画不出来** —— 下面最后一档就是拿它反着量的。
        const auto names = [](int n) {
            QStringList out;
            for (int i = 0; i < n; ++i) {
                out.append(QStringLiteral("Dep%1").arg(i));
            }
            return out;
        };
        const auto listingOf = [&names](int n) {
            return ModDependencies::dependencyPromptText(ResourceCatalog::Kind::Mod, names(n));
        };
        // 1 条：列出来，而且**不出现**「未列出」。
        check(listingOf(1).contains(QStringLiteral("- Dep0")) &&
                  !listingOf(1).contains(QStringLiteral("未列出")),
              "★ Z6d 只缺 1 条时就是那一条（1 条也不许写成「还有 1 个未列出」）", listingOf(1));
        // 4 条：全列（"刚好还剩一个"那一档走全列，这是用户点名不许省的那一侧）。
        check(listingOf(4).contains(QStringLiteral("- Dep3")) &&
                  !listingOf(4).contains(QStringLiteral("未列出")),
              "★ Z6d 缺 4 条时**一条不省**全列出来（这一档就是用户说的「刚好还剩一个」）",
              listingOf(4));
        // 5 条：前 3 条 + 「还有 2 个未列出…」，后两条**不在**名单里。
        const QString five = listingOf(5);
        check(five.contains(QStringLiteral("- Dep0")) && five.contains(QStringLiteral("- Dep2")) &&
                  !five.contains(QStringLiteral("- Dep3")) &&
                  !five.contains(QStringLiteral("- Dep4")) &&
                  five.contains(QStringLiteral("还有 2 个未列出…")),
              "★ Z6d 缺 5 条时列前 3 +「还有 2 个未列出…」（第 4、5 条不再占行）", five);
        // 反着量一次：**任何一档都不许**给出「还有 1 个未列出」（用户点名的那句高血压话）。
        for (int n = 0; n <= 8; ++n) {
            check(!listingOf(n).contains(QStringLiteral("还有 1 个未列出")),
                  QStringLiteral("★ Z6d 缺 %1 条时都不出现「还有 1 个未列出」（用户点名不要的那一行）")
                      .arg(n),
                  listingOf(n));
        }

        // ---- 弹窗里那行灰字（用户：「弹窗里灰字写可以在设置里面关掉弹窗」）----
        const QString note = ModDependencies::dependencyPromptNoteText();
        check(!note.isEmpty() && note.contains(QStringLiteral("设置")) &&
                  note.contains(QStringLiteral("检查前置模组")),
              "★ Z6d 灰字说清了「能在设置里关掉」，而且**点名了那一项**（不然用户在设置里找不着）",
              note);

        // ---- 「还没看过这个实例装了什么」那一版 ----
        // 两版的**区别只有一句话**：这一版在句头后面补上「还没看过这个实例里装了什么，
        // 不知道你有没有」—— 名字照样一条一行，截断规则同一套。空表同样回空串。
        const QString unknown =
            ModDependencies::dependencyPromptUnknownText(ResourceCatalog::Kind::Mod, names(1));
        check(ModDependencies::dependencyPromptUnknownText(ResourceCatalog::Kind::Mod, {}).isEmpty() &&
                  unknown.contains(QStringLiteral("- Dep0")) &&
                  unknown.contains(QStringLiteral("尚未核对实例中已安装的依赖")) &&
                  unknown.startsWith(
                      ModDependencies::dependencyPromptHead(ResourceCatalog::Kind::Mod)),
              "★ Z6d 不知道装没装的那一版补一句「不知道你有没有」，名单照旧（不替用户认下来）",
              unknown);
        // 名单截断那一套两版**共用**（同一支 dependencyNameLines）—— 拿 5 条在这版也量一次。
        check(ModDependencies::dependencyPromptUnknownText(ResourceCatalog::Kind::Mod, names(5))
                  .contains(QStringLiteral("还有 2 个未列出…")),
              "★ Z6d 截断规则两版共用（不知道装没装那一版同样是前 3 +「还有 2 个未列出…」）",
              ModDependencies::dependencyPromptUnknownText(ResourceCatalog::Kind::Mod, names(5)));
    }

    section("Z6e. 更新检测：盘上这一条是不是最新");

    // 用户 ：「实例设置那里做一个更新检测」。判据**只有一条**：盘上那个文件的
    // sha1 落在列表的第几条上（第 0 条 = 最新的那条）。**不许**按版本号比大小 ——
    // 那个比较器（"1.9 比 1.10 旧"）写错了不会有任何东西报警。
    {
        ModDependencies::InstalledItem item;
        item.path = QStringLiteral("D:/mc/versions/1.21.1-fabric/mods/iris.jar");
        item.iconKey = QStringLiteral("modrinth-YL57xq9U");
        item.title = QStringLiteral("Iris Shaders");

        // 盘上就是最新的那一条。
        item.sha1 = versions.first().primaryFile()->sha1;
        const ModDependencies::UpdateVerdict newest =
            ModDependencies::judgeUpdate(item, versions);
        check(newest.state == ModDependencies::UpdateState::UpToDate && newest.newerCount == 0 &&
                  newest.latestVersionId == versions.first().id,
              "Z6e 盘上就是最新的那一条 -> 已是最新", newest.reason);

        // 盘上是最老的那一条：比它新的有 11 条。
        item.sha1 = versions.last().primaryFile()->sha1;
        const ModDependencies::UpdateVerdict oldest =
            ModDependencies::judgeUpdate(item, versions);
        check(oldest.state == ModDependencies::UpdateState::Outdated &&
                  oldest.newerCount == versions.size() - 1 &&
                  oldest.latestVersionId == versions.first().id &&
                  !oldest.latestFileName.isEmpty(),
              "Z6e 盘上是最老的那一条 -> 有新版本，数出比它新的有几条",
              QStringLiteral("%1 条新 / 最新 %2")
                  .arg(oldest.newerCount)
                  .arg(oldest.latestVersionNumber));

        // 三档都**不许**混：说不清的两种绝不能被当成"已是最新"（那是"告诉用户没事、
        // 其实没查成"，比不报更糟）。
        ModDependencies::InstalledItem noHash = item;
        noHash.sha1.clear();
        const ModDependencies::UpdateVerdict unknownHash =
            ModDependencies::judgeUpdate(noHash, versions);
        check(unknownHash.state == ModDependencies::UpdateState::Unknown &&
                  !unknownHash.reason.isEmpty(),
              "Z6e 本地那个文件算不出 sha1 -> 说不清（不是「已是最新」）", unknownHash.reason);

        ModDependencies::InstalledItem stranger = item;
        stranger.sha1 = QString(40, QLatin1Char('f'));
        const ModDependencies::UpdateVerdict unknownFile =
            ModDependencies::judgeUpdate(stranger, versions);
        check(unknownFile.state == ModDependencies::UpdateState::Unknown &&
                  !unknownFile.reason.isEmpty(),
              "Z6e 这个 sha1 一条都没落上 -> 说不清（筛过 / 作者删了 / 用户改过，三合一）",
              unknownFile.reason);

        item.sha1 = versions.first().primaryFile()->sha1;
        const ModDependencies::UpdateVerdict noList =
            ModDependencies::judgeUpdate(item, {});
        check(noList.state == ModDependencies::UpdateState::Unknown && !noList.reason.isEmpty(),
              "Z6e 那一发没回来（空列表）-> 说不清", noList.reason);

        // 大小写不敏感地比：两边都是小写，但这是**外部输入**（接口正文 / 本地算的），
        // 差一个大小写就变成"说不清"，而屏幕上看不出任何异常。
        ModDependencies::InstalledItem upper = item;
        upper.sha1 = upper.sha1.toUpper();
        check(ModDependencies::judgeUpdate(upper, versions).state ==
                  ModDependencies::UpdateState::UpToDate,
              "Z6e sha1 比的时候不看大小写");

        check(ModDependencies::updateUpToDateText() == QStringLiteral("已是最新") &&
                  ModDependencies::updateOutdatedText() == QStringLiteral("有新版本") &&
                  !ModDependencies::updateUnknownText().isEmpty(),
              "Z6e 三档在报告里各是各的一句",
              ModDependencies::updateUnknownText());
        check(ModDependencies::updateSummaryText(5, 2).contains(QStringLiteral("5")) &&
                  ModDependencies::updateSummaryText(5, 2).contains(QStringLiteral("2")),
              "Z6e 报告开头那两个数（查了几个 / 几个有新版本）",
              ModDependencies::updateSummaryText(5, 2));
    }

    section("Z6f. 更新检测：批量那一发（`/version_files/update`）");
    // 改法两半：① Modrinth 那一拨换成 `POST /version_files/update`（一次一串 sha1，
    // 352 个模组 = 3 发）；② **一个文件只查一家**（两家都有时走 Modrinth，Modrinth 那一发
    // 没给出答案时才退回问 CurseForge）。②那一半的状态机在 `MainWindow` 上，由 mw_check 的
    // T28/T32 钉着；这一段钉的是**纯函数那一半**。
    {
        const QString shaA = QString(40, QLatin1Char('a'));
        const QString shaB = QString(40, QLatin1Char('b'));

        // ---------- ① 请求体 ----------
        //
        // 空哈希表：**不许**发（空表的回复必然是空表，白花一发）。
        check(ResourceCatalog::versionFilesUpdateRequestBody({}, QStringLiteral("1.20.1"),
                                                             QStringLiteral("forge"))
                  .isEmpty(),
              "Z6f 一个哈希都没有 -> 请求体是空的（那一发不该发出去）");
        check(ResourceCatalog::versionFilesUpdateRequestBody({QStringLiteral("   "),
                                                              QString()},
                                                             QStringLiteral("1.20.1"),
                                                             QStringLiteral("forge"))
                  .isEmpty(),
              "Z6f 全是空白 sha1 -> 也当成空表（发上去只是白占一格）");

        const QByteArray body = ResourceCatalog::versionFilesUpdateRequestBody(
            {shaA, QStringLiteral("  ") + shaB + QStringLiteral(" "), QString()},
            QStringLiteral("1.20.1"), QStringLiteral("Forge"));
        const QJsonObject bodyObject = QJsonDocument::fromJson(body).object();
        const QJsonArray bodyHashes = bodyObject.value(QStringLiteral("hashes")).toArray();
        check(bodyObject.value(QStringLiteral("algorithm")).toString() == QStringLiteral("sha1"),
              "Z6f 请求体里那一格写着 algorithm=sha1（少它那一发会被接口回 400）",
              QString::fromUtf8(body));
        check(bodyHashes.size() == 2 &&
                  bodyHashes.at(0).toString() == shaA && bodyHashes.at(1).toString() == shaB,
              "Z6f 两个真空哈希原样进去了、空白和空串被剔掉（顺序照给的顺序）",
              QStringLiteral("%1 个").arg(bodyHashes.size()));
        // 加载器那一格**必须翻一道**：`dependencyTargetFor()` 给的是显示名（「Forge」），
        // 而这一发只认小写那个词 —— 递错的症状是"一个哈希都匹配不上"（不是报错）：
        // 整批模组会被退回问 CurseForge，而屏幕上看像"这一批没查到"。
        // 见 `ResourceCatalog::versionFilesUpdateRequestBody()` 里那段。
        check(bodyObject.value(QStringLiteral("loaders")).toArray() ==
                  QJsonArray{QStringLiteral("forge")},
              "★★ Z6f 加载器递显示名「Forge」也被翻成 loaders=[forge] —— 这一格是"
              "**唯一**能做到「递错就一个都筛不出来」的地方",
              QString::fromUtf8(body));
        check(bodyObject.value(QStringLiteral("game_versions")).toArray() ==
                  QJsonArray{QStringLiteral("1.20.1")},
              "Z6f 游戏版本那一格是小写键 `game_versions`、值是个数组");
        // 认不出来的加载器词（用户盘上的 OptiFine 就是这一档）：**那一格整个不出现**
        // （= 不筛），别拼一个筛不出来的词上去。
        const QJsonObject optifineBody = QJsonDocument::fromJson(
            ResourceCatalog::versionFilesUpdateRequestBody({shaA}, QStringLiteral("1.20.1"),
                                                           QStringLiteral("OptiFine"))).object();
        check(!optifineBody.contains(QStringLiteral("loaders")),
              "Z6f 认不出来的加载器（「OptiFine」）-> `loaders` 那一格**整个不出现**（不筛，"
              "不是拼一个筛不出来的词上去）",
              QString::fromUtf8(QJsonDocument(optifineBody).toJson(QJsonDocument::Compact)));
        // 两个筛都不给 = 一个筛都不发（"随便哪一版"）。
        const QJsonObject bareBody = QJsonDocument::fromJson(
            ResourceCatalog::versionFilesUpdateRequestBody({shaA}, QString(), QString())).object();
        check(!bareBody.contains(QStringLiteral("game_versions")) &&
                  !bareBody.contains(QStringLiteral("loaders")),
              "Z6f 不给游戏版本/加载器 -> 那两个键都不出现（空 = 不筛，和 versionsUrl 同一条）");
        check(ResourceCatalog::versionFilesUpdateUrl(QStringLiteral("http://127.0.0.1:9/v2")) ==
                  QStringLiteral("http://127.0.0.1:9/v2/version_files/update"),
              "Z6f 地址：`<base>/version_files/update`（验证工程那个口子优先）");

        // ---------- ② 解析 ----------
        check(ResourceCatalog::parseVersionFilesUpdate(QByteArrayLiteral("not json")).isEmpty() &&
                  ResourceCatalog::parseVersionFilesUpdate(QByteArrayLiteral("[]")).isEmpty(),
              "Z6f 读不懂（不是对象）-> 空表（一条都不许当成「已是最新」）");
        const QByteArray reply = QByteArrayLiteral("{") + QByteArrayLiteral("\"") +
                                 shaA.toUpper().toUtf8() + QByteArrayLiteral("\":{\"id\":"
                                 "\"VNEW\",\"version_number\":\"2.0\",\"game_versions\":"
                                 "[\"1.20.1\"],\"loaders\":[\"forge\"],\"files\":[{\"hashes\":"
                                 "{\"sha1\":\"1111111111111111111111111111111111111111\"},"
                                 "\"url\":\"https://cdn.modrinth.com/data/mod-b-2.0.jar\","
                                 "\"filename\":\"模组乙-2.0.jar\",\"size\":10}]},") +
                                 QByteArrayLiteral("\"deadbeef\":\"这不是对象\"}");
        const QHash<QString, ResourceCatalog::VersionEntry> parsed =
            ResourceCatalog::parseVersionFilesUpdate(reply);
        check(parsed.size() == 1,
              "Z6f 回复里那一格不是对象的条目**被跳过**（不是崩、也不是当成一条空版本）",
              QStringLiteral("%1 条").arg(parsed.size()));
        const ResourceCatalog::VersionEntry entry = parsed.value(shaA);
        check(entry.id == QStringLiteral("VNEW") &&
                  entry.versionNumber == QStringLiteral("2.0") && entry.files.size() == 1 &&
                  entry.files.first().filename == QStringLiteral("模组乙-2.0.jar") &&
                  entry.files.first().sha1 == QString(40, QLatin1Char('1')),
              "Z6f 读出来的那一版字段齐（和 parseVersions 用的是**同一个读取器**）",
              QStringLiteral("%1 / %2").arg(entry.id, entry.versionNumber));
        check(parsed.contains(shaA),
              "★ Z6f 键**按小写收**：接口回的是大写哈希，查的时候用小写的也拿得到"
              "（本地那个 sha1 一律是小写，差一个大小写就变成「说不清」）");

        // ---------- ③ 判：`judgeUpdateAgainstLatest` ----------
        ModDependencies::InstalledItem item;
        item.path = QStringLiteral("D:/mc/versions/1.20.1-forge/mods/模组乙.jar");
        item.iconKey = QStringLiteral("modrinth-UPDPROJ");
        item.title = QStringLiteral("模组乙");

        // 盘上这一份的 sha1 **不在**最新那一版的文件里 -> 有新版本。
        item.sha1 = shaB;
        const ModDependencies::UpdateVerdict outdated =
            ModDependencies::judgeUpdateAgainstLatest(item, entry);
        check(outdated.state == ModDependencies::UpdateState::Outdated &&
                  outdated.newerCount >= 1 &&
                  outdated.latestVersionId == QStringLiteral("VNEW") &&
                  outdated.latestFileName == QStringLiteral("模组乙-2.0.jar"),
              "Z6f 盘上那一份不在最新那一版的文件里 -> 有新版本（「最新那一版」就是接口给的"
              "那一条）",
              QStringLiteral("%1 / %2").arg(outdated.latestVersionNumber, outdated.latestFileName));

        // 盘上这一份**就是**最新那一版的文件 -> 已是最新（逐字和 judgeUpdate 那一条同一句话）。
        item.sha1 = QStringLiteral("1111111111111111111111111111111111111111");
        const ModDependencies::UpdateVerdict newest =
            ModDependencies::judgeUpdateAgainstLatest(item, entry);
        check(newest.state == ModDependencies::UpdateState::UpToDate && newest.newerCount == 0,
              "Z6f 盘上就是最新那一版的文件 -> 已是最新");
        item.sha1 = item.sha1.toUpper();
        check(ModDependencies::judgeUpdateAgainstLatest(item, entry).state ==
                  ModDependencies::UpdateState::UpToDate,
              "Z6f sha1 比的时候不看大小写");

        ModDependencies::InstalledItem noHash = item;
        noHash.sha1.clear();
        check(ModDependencies::judgeUpdateAgainstLatest(noHash, entry).state ==
                  ModDependencies::UpdateState::Unknown,
              "Z6f 本地那个文件算不出 sha1 -> 说不清（不是「已是最新」）");

        // 接口对它**什么都没给**（那一批自己 429 了 / 这个项目在这个游戏版本+加载器下
        // 没有版本）：`problem` 里那句话就是这几条"说不清"的理由 —— 不能吞成
        // "网络或接口没成"，否则用户看到的每一条都是"网络没成"，在那儿反复重试网络，
        // 而真因是"一次问得太多"。
        const QString throttled = QStringLiteral("问得太勤了，等一会儿再试。");
        const ModDependencies::UpdateVerdict noAnswer =
            ModDependencies::judgeUpdateAgainstLatest(item, ResourceCatalog::VersionEntry(),
                                                      throttled);
        check(noAnswer.state == ModDependencies::UpdateState::Unknown &&
                  noAnswer.reason == throttled,
              "★★ Z6f 接口那一发自己报的那句话（「问得太勤了…」）就是这一条说不清的理由 "
              "—— 真因不许被吞成「没问到版本列表」",
              noAnswer.reason);
        const ModDependencies::UpdateVerdict emptyAnswer =
            ModDependencies::judgeUpdateAgainstLatest(item, ResourceCatalog::VersionEntry());
        check(emptyAnswer.state == ModDependencies::UpdateState::Unknown &&
                  !emptyAnswer.reason.isEmpty() && emptyAnswer.reason != noAnswer.reason,
              "Z6f 没报错、接口就是没给这一条 -> 另一句话（「这个项目在这个游戏版本/加载器"
              "下没有版本」），和「那一发没成」分得开",
              emptyAnswer.reason);
        const ModDependencies::UpdateVerdict singleNoAnswer =
            ModDependencies::judgeUpdate(item, {}, throttled);
        check(singleNoAnswer.state == ModDependencies::UpdateState::Unknown &&
                  singleNoAnswer.reason == throttled,
              "★★ Z6f 一项目一发那条路同样不许吞（否则这里一律写「没问到版本列表（网络或"
              "接口没成）」，429 在报告里和「网断了」长得一模一样）",
              singleNoAnswer.reason);

        // ---------- ④ 钥匙 ----------
        const QString key = ModDependencies::latestVersionsKeyFor(QStringLiteral("update"),
                                                                  QStringLiteral("1.20.1"),
                                                                  QStringLiteral("forge"));
        check(!key.isEmpty() &&
                  key != ModDependencies::latestVersionsKeyFor(QStringLiteral("update"),
                                                                QStringLiteral("1.21"),
                                                                QStringLiteral("forge")) &&
                  key != ModDependencies::latestVersionsKeyFor(QStringLiteral("update"),
                                                                QStringLiteral("1.20.1"),
                                                                QStringLiteral("fabric")) &&
                  key != ModDependencies::latestVersionsKeyFor(QStringLiteral("dependency"),
                                                                QStringLiteral("1.20.1"),
                                                                QStringLiteral("forge")),
              "Z6f 批量那一发的钥匙：游戏版本 / 加载器 / 消费方**各差一个字就是另一把**"
              "（少一样的话，前一发回来的会被当成「我现在等的那一发」）",
              key);
    }

    section("Z6g. 更新检测：**问版本列表时带不带加载器**");
    {
        check(ModDependencies::kindUsesLoader(ResourceCatalog::Kind::Mod) &&
                  ModDependencies::kindUsesLoader(ResourceCatalog::Kind::Modpack),
              "★★ Z6g 模组（和整合包）**要带**加载器 —— 同一个模组的 Fabric / Forge 两版"
              "是两条版本线，不带的话「最新」可能是另一个加载器的 jar（装进去就崩）");
        check(!ModDependencies::kindUsesLoader(ResourceCatalog::Kind::ResourcePack) &&
                  !ModDependencies::kindUsesLoader(ResourceCatalog::Kind::Shader) &&
                  !ModDependencies::kindUsesLoader(ResourceCatalog::Kind::DataPack),
              "★★ Z6g 资源包 / 光影包 / 数据包**一个都不带** —— 带上去不会报错、只是"
              "一个都不匹配（真机症状：那一类每一条都是「说不清」）");
        // 逐类过一遍 `allKinds()`：后加一类时**不许**悄悄落进「带上去」那一档
        // （带错了的症状是「这一类一条都判不出来」），这一条会当场红。
        QStringList withLoader;
        for (const ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
            if (ModDependencies::kindUsesLoader(kind)) {
                withLoader.append(ResourceCatalog::kindKey(kind));
            }
        }
        // 排序再比：`allKinds()` 的顺序是**下载页左栏那一份**（整合包在前），它是那一栏的
        // 规格、不该被这一条断言反过来钉住。
        withLoader.sort();
        check(withLoader == QStringList{QStringLiteral("mod"), QStringLiteral("modpack")},
              "★ Z6g 会带上加载器的**只有这两类**（新加一类资源时默认落到「不带」那一档，"
              "想带就得来这儿改一个字 —— 悄悄带上去会让那一类一条都判不出来）",
              withLoader.join(QStringLiteral(" / ")));

        // 那一位的**默认值**：`InstalledItem` 是各处就地拼出来的（产品的反查那一趟、
        // 前置那一趟、这一节的样本），默认 true 才不会有旧调用点静默变成「不带」。
        ModDependencies::InstalledItem item;
        check(item.loaderApplies,
              "Z6g `InstalledItem::loaderApplies` 默认是 true（旧调用点不填时按模组那一档"
              "走，不会静默变成「不带」）");
    }

    section("Z7. 第二个来源（CurseForge）：来源本身 + 那一行筛选项");

    // 用户 要的「按版本 / 模组加载器 / 来源 / 类型 / 名称搜索」里，"来源"和"类型"
    // 两栏的**所有出处**都在这儿：哪几个来源、每一栏有哪些项、以及"哪一类根本没有这一栏"
    // （Modrinth 的数据包）。界面上那四个下拉框是照这几张表填的 —— 表错了，界面上看不出
    // 任何异常，只是搜出来的东西不对。
    check(ResourceCatalog::allSources().size() == 2 &&
              ResourceCatalog::sourceKey(ResourceCatalog::Source::Modrinth) ==
                  QStringLiteral("modrinth") &&
              ResourceCatalog::sourceKey(ResourceCatalog::Source::CurseForge) ==
                  QStringLiteral("curseforge"),
          "Z7a 两个来源、两个稳定的键");
    // 这两个是专有名词，界面上不许翻译（下面这一条钉的是"没被翻过"）。
    check(ResourceCatalog::sourceLabel(ResourceCatalog::Source::Modrinth) ==
                  QStringLiteral("Modrinth") &&
              ResourceCatalog::sourceLabel(ResourceCatalog::Source::CurseForge) ==
                  QStringLiteral("CurseForge"),
          "Z7a 下拉框里那两行字就是原名");

    {
        ResourceCatalog::Source back = ResourceCatalog::Source::CurseForge;
        bool every = true;
        for (ResourceCatalog::Source source : ResourceCatalog::allSources()) {
            every = every && ResourceCatalog::sourceFromKey(ResourceCatalog::sourceKey(source), &back) &&
                    back == source;
        }
        ResourceCatalog::Source untouched = ResourceCatalog::Source::CurseForge;
        check(every && !ResourceCatalog::sourceFromKey(QStringLiteral("spigot"), &untouched) &&
                  untouched == ResourceCatalog::Source::CurseForge,
              "Z7a 键往返都认；认不出来时不动 out（和 kindFromCategoryKey 一个规矩）");
    }

    // --- Z7b. 类型那一栏的选项表 ---------------------------------------------
    {
        int emptyTables = 0;
        bool allTablesOk = true;
        QString badTable;
        for (ResourceCatalog::Source source : ResourceCatalog::allSources()) {
            for (ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
                const QList<ResourceCatalog::FilterOption> options =
                    ResourceCatalog::categoryOptions(source, kind);
                if (options.isEmpty()) {
                    ++emptyTables;
                    // **只有一种情况该是空表**：Modrinth 的数据包（它的分类表里没有
                    // datapack 那一档）。别的组合空了 = 界面上凭空少一个框。
                    if (!(source == ResourceCatalog::Source::Modrinth &&
                          kind == ResourceCatalog::Kind::DataPack)) {
                        allTablesOk = false;
                        badTable = ResourceCatalog::sourceKey(source) +
                                   QLatin1Char('/') + ResourceCatalog::kindKey(kind);
                    }
                    continue;
                }
                // 第一项**永远是「全部」**（界面上那一格直接照抄这张表，不用自己补）。
                if (!options.first().isAll()) {
                    allTablesOk = false;
                    badTable = ResourceCatalog::sourceKey(source) + QLatin1Char('/') +
                               ResourceCatalog::kindKey(kind) + QStringLiteral(" 缺「全部」");
                    continue;
                }
                // 每一栏里那些键**两两不同**（撞了的话后一个永远选不中，而界面上看着
                // 两个选项都在）。标签也得都有 —— 空标签在下来框里是一行空白。
                QSet<QString> seen;
                for (int i = 0; i < options.size(); ++i) {
                    const ResourceCatalog::FilterOption &option = options.at(i);
                    if (option.label.isEmpty() ||
                        (!option.isAll() && seen.contains(option.key))) {
                        allTablesOk = false;
                        badTable = ResourceCatalog::sourceKey(source) + QLatin1Char('/') +
                                   ResourceCatalog::kindKey(kind) + QLatin1Char(' ') + option.key;
                        break;
                    }
                    seen.insert(option.key);
                }
            }
        }
        check(allTablesOk && emptyTables == 1,
              "Z7b 每一栏的键两两不同、第一项是「全部」；空表只有 Modrinth 的数据包那一格",
              badTable.isEmpty() ? QStringLiteral("空表 %1 张").arg(emptyTables) : badTable);
        check(ResourceCatalog::categoryOptions(ResourceCatalog::Source::CurseForge,
                                               ResourceCatalog::Kind::DataPack)
                  .size() > 1,
              "Z7b CurseForge 那边的数据包**有**类型可选（两边的母集不一样）");
    }

    // --- Z7c. 模组加载器那一栏 -----------------------------------------------
    {
        const QList<ResourceCatalog::FilterOption> modrinth =
            ResourceCatalog::loaderOptions(ResourceCatalog::Source::Modrinth);
        const QList<ResourceCatalog::FilterOption> forge =
            ResourceCatalog::loaderOptions(ResourceCatalog::Source::CurseForge);
        check(modrinth.size() == 5 && forge.size() == 5 && modrinth.first().isAll() &&
                  forge.first().isAll(),
              "Z7c 两个来源各四个加载器 + 「全部」");
        // 键**不一样**（Modrinth 要词、CurseForge 要数字），标签顺序**一样** ——
        // 顺序不一样的话，用户在两个来源之间来回切时那个下拉框里的行会跳。
        QStringList modrinthLabels;
        QStringList forgeLabels;
        for (int i = 1; i < modrinth.size(); ++i) {
            modrinthLabels.append(modrinth.at(i).label);
            forgeLabels.append(forge.at(i).label);
        }
        check(modrinth.at(1).key == QStringLiteral("fabric") &&
                  modrinth.at(2).key == QStringLiteral("forge") &&
                  modrinth.at(3).key == QStringLiteral("neoforge") &&
                  modrinth.at(4).key == QStringLiteral("quilt") &&
                  forge.at(1).key == QStringLiteral("4") && forge.at(2).key == QStringLiteral("1") &&
                  forge.at(3).key == QStringLiteral("6") && forge.at(4).key == QStringLiteral("5"),
              "Z7c 两边的键各是各的（词 vs 数字）",
              forge.at(1).key + QLatin1Char('/') + forge.at(2).key + QLatin1Char('/') +
                  forge.at(3).key + QLatin1Char('/') + forge.at(4).key);
        check(modrinthLabels == forgeLabels, "Z7c 两边的行李顺序一样（切来源时那格不跳）",
              modrinthLabels.join(QLatin1Char(',')) + QStringLiteral(" vs ") +
                  forgeLabels.join(QLatin1Char(',')));
    }

    // --- Z7d. 类目号：搜的是哪一类、装的是哪个目录，两边得是同一套号 ----------
    //
    // 这两个号一旦不一致，症状是"搜出来的是模组、装进去却是资源包"——**中间不会有任何
    // 东西报警**：搜索回的是 Modrinth/CurseForge 的项目，落点是 ModpackImport 那张表。
    check(ResourceCatalog::curseForgeClassId(ResourceCatalog::Kind::Mod) == 6 &&
              ResourceCatalog::curseForgeClassId(ResourceCatalog::Kind::DataPack) == 6945 &&
              ResourceCatalog::curseForgeClassId(ResourceCatalog::Kind::ResourcePack) == 12 &&
              ResourceCatalog::curseForgeClassId(ResourceCatalog::Kind::Shader) == 6552 &&
              ResourceCatalog::curseForgeClassId(ResourceCatalog::Kind::Modpack) == 4471,
          "Z7d 五个类目号（6945 数据包那条原来在 ModpackImport 里是**缺**的）");
    check(ModpackImport::curseForgeFilePath(ResourceCatalog::curseForgeClassId(
              ResourceCatalog::Kind::Mod)) == ResourceCatalog::folderForKind(
              ResourceCatalog::Kind::Mod) + QLatin1Char('/') &&
              ModpackImport::curseForgeFilePath(ResourceCatalog::curseForgeClassId(
                  ResourceCatalog::Kind::DataPack)) ==
                  ResourceCatalog::folderForKind(ResourceCatalog::Kind::DataPack) +
                      QLatin1Char('/') &&
              ModpackImport::curseForgeFilePath(ResourceCatalog::curseForgeClassId(
                  ResourceCatalog::Kind::ResourcePack)) ==
                  ResourceCatalog::folderForKind(ResourceCatalog::Kind::ResourcePack) +
                      QLatin1Char('/') &&
              ModpackImport::curseForgeFilePath(ResourceCatalog::curseForgeClassId(
                  ResourceCatalog::Kind::Shader)) ==
                  ResourceCatalog::folderForKind(ResourceCatalog::Kind::Shader) +
                      QLatin1Char('/'),
          "Z7d 「从下载页下」和「从整合包里来」落的是同一个目录",
          ModpackImport::curseForgeFilePath(6945));
    // 嵌套的整合包不代装（那一条是"落点为空"的约定，下载页那条路也认它）。
    check(ModpackImport::curseForgeFilePath(ResourceCatalog::curseForgeClassId(
              ResourceCatalog::Kind::Modpack))
              .isEmpty(),
          "Z7d 整合包这一类没有落点（它走导入那条路）");

    section("Z7e. CurseForge 的搜索地址（纯函数，不联网）");

    const QString fakeBase = QStringLiteral("http://127.0.0.1:9/v1");
    {
        const QString url = ResourceCatalog::searchUrl(
            ResourceCatalog::Kind::Mod, QStringLiteral("jei"), 20, 20,
            QStringLiteral("1.21.1"), QStringLiteral("4"), ResourceCatalog::Source::CurseForge,
            QStringLiteral("6814"), fakeBase);
        check(url.startsWith(fakeBase + QStringLiteral("/mods/search?")),
              "Z7e 地址前缀就是我们给的那台接口（验证工程拿它指到本机）", url);
        // 一个筛子都不能白传：这一家的 `/mods/search` **没有 project_type 这个概念**，
        // 少传 classId 会把九类东西全混在一起回给你（不报错）。
        check(urlParam(url, QStringLiteral("gameId")) == QStringLiteral("432") &&
                  urlParam(url, QStringLiteral("classId")) == QStringLiteral("6") &&
                  urlParam(url, QStringLiteral("searchFilter")) == QStringLiteral("jei") &&
                  urlParam(url, QStringLiteral("gameVersion")) == QStringLiteral("1.21.1") &&
                  urlParam(url, QStringLiteral("modLoaderType")) == QStringLiteral("4") &&
                  urlParam(url, QStringLiteral("categoryId")) == QStringLiteral("6814") &&
                  urlParam(url, QStringLiteral("index")) == QStringLiteral("20") &&
                  urlParam(url, QStringLiteral("pageSize")) == QStringLiteral("20"),
              "Z7e 六个筛子 + 分页都拼进去了", url);
        // 名字那一段必须叫 `searchFilter`，**不能是 `search`**。这一条是补上去的：
        // 之前我们发的是 `search=`，这一家**一个字都不筛**、也不报错，安安静静
        // 把"这一类里最热门的"那一批回给你（`totalCount` 是整类的 10000，而不是 "jei"
        // 那 377 条）—— 界面上看起来"搜出来了"，其实搜什么都是同一批。
        // **它不报错**，所以只能靠"地址里那一段叫什么"来钉住（正面一条 + 反面一条）。
        // （更早那句"实测 Popularity 会把搜索词无视掉"是同一个错的另一副面孔：词压根没
        // 送到，换哪个排序都一样。更正之后 sortField=6 只是为了"按总下载量"。）
        check(urlParam(url, QStringLiteral("search")).isEmpty(),
              "Z7e 地址里**没有**裸的 `search=` 那一段（发它等于没搜）", url);
        // 排序：一个值就够（TotalDownloads），但**方向必须是 desc**
        // （这一家的默认方向是 asc，那样头一条是"这一类里最没人用的"）。
        check(urlParam(url, QStringLiteral("sortField")) == QStringLiteral("6") &&
                  urlParam(url, QStringLiteral("sortOrder")) == QStringLiteral("desc"),
              "Z7e 排序用 TotalDownloads + 倒序", url);

        // 每个筛子空着 = 那一段**根本不出现**（不是发一个空值上去）。加载器那一格尤其：
        // 空串传上去会变成 `modLoaderType=`，这一家怎么处理那个值不该由我们赌。
        const QString bare = ResourceCatalog::searchUrl(
            ResourceCatalog::Kind::Shader, QString(), 0, 20, QString(), QString(),
            ResourceCatalog::Source::CurseForge, QString(), fakeBase);
        check(urlParam(bare, QStringLiteral("classId")) == QStringLiteral("6552") &&
                  urlParam(bare, QStringLiteral("gameVersion")).isEmpty() &&
                  urlParam(bare, QStringLiteral("modLoaderType")).isEmpty() &&
                  urlParam(bare, QStringLiteral("categoryId")).isEmpty() &&
                  urlParam(bare, QStringLiteral("searchFilter")).isEmpty(),
              "Z7e 不筛的项一段都不拼（那一类自己的 classId 永远在）", bare);

        // 不给地址时走的是**那唯一一处默认**（社区镜像，不要 key）——这里比的是
        // ModpackImport::curseForgeApiBase() 的返回值，不是写死一个域名：写死的话，
        // 哪天默认换了地方，这条断言就成了"两边都错得一样"的假绿。
        const QString defaultUrl = ResourceCatalog::searchUrl(
            ResourceCatalog::Kind::Mod, QString(), 0, 20, QString(), QString(),
            ResourceCatalog::Source::CurseForge, QString(), QString());
        check(defaultUrl.startsWith(ModpackImport::curseForgeApiBase(QString(), QString()) +
                                    QStringLiteral("/mods/search?")),
              "Z7e 不指定接口时走 curseForgeApiBase 那一处（域名不在这个文件里）",
              defaultUrl);

        // 版本列表那一发（`/mods/{id}/files`）：参数名和搜索那一边**一样**（谁的就是谁的），
        // 加载器也同样要数字。
        const QString files = ResourceCatalog::versionsUrl(
            QStringLiteral("238222"), QStringLiteral("1.21.1"), QStringLiteral("4"),
            ResourceCatalog::Source::CurseForge, fakeBase);
        check(files.startsWith(fakeBase + QStringLiteral("/mods/238222/files?")) &&
                  urlParam(files, QStringLiteral("gameVersion")) == QStringLiteral("1.21.1") &&
                  urlParam(files, QStringLiteral("modLoaderType")) == QStringLiteral("4"),
              "Z7e 版本列表：问哪个项目的哪个游戏版本", files);
    }

    section("Z7e2. 加载器那一格：显示名 / 数字键 → 那一边要的词形");

    {
        // 这一格是**静默失灵**那一类：两边要的不是一套词（CurseForge 要 `modLoaderType`
        // 的数字、Modrinth 要 `fabric` 这个词），拼错了那边**不报错**，只是什么都筛不
        // 出来 —— 而"什么都没筛出来"和"这个资源一个版本都没有"在界面上是同一句话。
        //
        // 修之前：`instanceLoaderName()` 给的是**显示名**（「Fabric」），主程序直接把它
        // 递进了这一格 ⇒ 给一个 Fabric 实例问前置，回来的可能是任意加载器的版本。
        // 所以三个来源（数字键 / 显示名 / 小写词）都得认，认不出来必须回空串。
        check(ResourceCatalog::loaderRequestValue(QStringLiteral("4"),
                                                  ResourceCatalog::Source::CurseForge) ==
                      QStringLiteral("4") &&
                  ResourceCatalog::loaderRequestValue(QStringLiteral("Fabric"),
                                                      ResourceCatalog::Source::CurseForge) ==
                      QStringLiteral("4") &&
                  ResourceCatalog::loaderRequestValue(QStringLiteral("fabric"),
                                                      ResourceCatalog::Source::CurseForge) ==
                      QStringLiteral("4"),
              "★★ Z7e2 CurseForge：数字键 / 显示名 / 小写词**三个都翻成 `4`**"
              "（下拉框给的是键、`instanceLoaderName()` 给的是显示名，两条路都得通）");
        check(ResourceCatalog::loaderRequestValue(QStringLiteral("fabric"),
                                                  ResourceCatalog::Source::Modrinth) ==
                      QStringLiteral("fabric") &&
                  ResourceCatalog::loaderRequestValue(QStringLiteral("NeoForge"),
                                                      ResourceCatalog::Source::Modrinth) ==
                      QStringLiteral("neoforge"),
              "★ Z7e2 Modrinth：要的就是那个词，显示名也认");
        check(ResourceCatalog::loaderRequestValue(QStringLiteral("  Quilt  "),
                                                  ResourceCatalog::Source::CurseForge) ==
                      QStringLiteral("5") &&
                  ResourceCatalog::loaderRequestValue(QStringLiteral("OPTIFINE"),
                                                      ResourceCatalog::Source::CurseForge)
                      .isEmpty() &&
                  ResourceCatalog::loaderRequestValue(QString(),
                                                      ResourceCatalog::Source::CurseForge)
                      .isEmpty(),
              "★★ Z7e2 认不出来的（`OptiFine`）和空串都回**空串 = 这一段根本不拼**"
              "（拼上去那边不报错，只是变成「这个资源一个版本都没有」）；"
              "两头的空白也不影响认");
    }

    section("Z7f. 读真搜索响应（meta/curseforge-search-mod.json）");

    {
        QByteArray json;
        QFile fixture(metaDir + "/curseforge-search-mod.json");
        const bool haveFixture = fixture.open(QIODevice::ReadOnly);
        check(haveFixture, "Z7f 读到了真搜索响应夹具");
        if (haveFixture) {
            json = fixture.readAll();
            fixture.close();
        }

        const ResourceCatalog::SearchResult result = ResourceCatalog::parseCurseForgeSearch(json);
        check(result.ok && result.hits.size() == 2, "Z7f 两条都读出来了", result.problem);
        // 总数在 `pagination.totalCount`（不是 Modrinth 那个 total_hits）。
        check(result.totalHits == 10000,
              "Z7f 总数读的是 pagination.totalCount",
              QStringLiteral("%1").arg(result.totalHits));

        if (result.hits.size() == 2) {
            // 第一条（`"Arizona" Music Disc`）：一个 id、一个作者、一个加载器、一条版本。
            const ResourceCatalog::Hit &first = result.hits.at(0);
            check(first.source == ResourceCatalog::Source::CurseForge &&
                      first.projectId == QStringLiteral("1546263") &&
                      first.title == QStringLiteral("\"Arizona\" Music Disc") &&
                      first.slug == QStringLiteral("arizona-music-disc") &&
                      first.author == QStringLiteral("birdmuncher") &&
                      first.downloads == 642,
                  "Z7f 第一条的身份字段（`id` 在那边是数字，这里存成字符串）",
                  first.projectId + QStringLiteral(" / ") + first.title);
            // 名字在 `name` 里、简介在 `summary` 里 —— 两个键名和 Modrinth 都不一样，
            // 读错了的症状是**列表一整列空白**（不报错）。
            check(!first.description.isEmpty(), "Z7f 简介读的是 summary");
            // 加载器：搜索响应里根本不在 categories 里（那是「Cosmetic」这种分类标签），
            // 它在 `latestFilesIndexes[].modLoader` 那个数字上 —— 所以 `loaders()` 能给出
            // 东西，靠的是解析时把它换成了名字（explicitLoaders）。
            check(first.categories.contains(QStringLiteral("Cosmetic")) &&
                      first.loaders() == QStringList{QStringLiteral("forge")},
                  "Z7f 加载器从 modLoader 数字来，不是从 categories 现推",
                  first.categories.join(QLatin1Char(',')) + QStringLiteral(" | ") +
                      first.loaders().join(QLatin1Char(',')));
            // 图标：两条都在 media.forgecdn.net 上（放行的域名是按来源挑的），
            // 缓存 id 带来源前缀（两个 id 空间会撞：`238222` 在 Modrinth 也是合法 id）。
            check(first.iconUrl.startsWith(QStringLiteral("https://media.forgecdn.net/")) &&
                      first.iconPngUrl().startsWith(QStringLiteral("https://media.forgecdn.net/")) &&
                      first.iconCacheId() == QStringLiteral("curseforge-1546263"),
                  "Z7f 图标两条地址 + 缓存 id 带来源", first.iconCacheId());
        }

        if (result.hits.size() == 2) {
            // 第二条：`latestFilesIndexes` 里是**乱序**的 1.21 / 1.21.8 / 1.21.5 / … （真响应
            // 就这样），而且混着 1.21.10 / 1.21.11 —— 这一条正是那个比较器存在的理由：
            // 按字符串排会把 1.21.10 排到 1.21.2 前面。
            const ResourceCatalog::Hit &second = result.hits.at(1);
            const QStringList oldest = ResourceCatalog::gameVersionsOldestFirst(
                {QStringLiteral("1.21"), QStringLiteral("1.21.8"), QStringLiteral("1.21.5"),
                 QStringLiteral("1.21.1"), QStringLiteral("1.21.10"), QStringLiteral("1.21.2")});
            check(second.loaders() == QStringList{QStringLiteral("fabric")},
                  "Z7f 第二条是 Fabric 的（modLoader=4）",
                  second.loaders().join(QLatin1Char(',')));
            check(second.gameVersions.size() == 12 &&
                      second.gameVersions.first() == QStringLiteral("1.21") &&
                      second.gameVersions.last() == QStringLiteral("1.21.11"),
                  "Z7f 12 个游戏版本去重后**旧的在前**（接口给的是乱的）",
                  second.gameVersions.join(QLatin1Char(' ')));
            // 界面上第二层要的是**新的在前**：这半边靠 gameVersionChoices 统一翻一次。
            // 这一串里全是正式版（1.21.x），所以折与不折是同一份 —— Z8h 才用带快照的表验折。
            const QStringList choices = ResourceCatalog::gameVersionChoices(second.gameVersions, false);
            check(choices.first() == QStringLiteral("1.21.11") &&
                      choices.last() == QStringLiteral("1.21"),
                  "Z7f 界面上那一串是新的在前（和 Modrinth 那份落到同一个方向）",
                  choices.join(QLatin1Char(' ')));
            // 直接钉那个比较器（上面那条脏数据只是它的一个真实样本）。
            check(oldest == QStringList({QStringLiteral("1.21"), QStringLiteral("1.21.1"),
                                         QStringLiteral("1.21.2"), QStringLiteral("1.21.5"),
                                         QStringLiteral("1.21.8"), QStringLiteral("1.21.10")}),
                  "Z7g 1.10 比 1.2 新（按数字段比，不是按字符串比）",
                  oldest.join(QLatin1Char(' ')));
            check(ResourceCatalog::gameVersionsOldestFirst(
                      {QStringLiteral("1.21.1"), QStringLiteral("1.9"),
                       QStringLiteral("1.21"), QStringLiteral("1.10")}) ==
                      QStringList({QStringLiteral("1.9"), QStringLiteral("1.10"),
                                   QStringLiteral("1.21"), QStringLiteral("1.21.1")}),
                  "Z7g 1.9 < 1.10 < 1.21 < 1.21.1");
            // 补的三条（P4）。这个比较器改之前**不是全序**：
            // 1.9 < 1.10（数比）· 1.10 < 1.10-pre1（字符串比）· 1.10-pre1 < 1.9（字符串比），
            // 三条同时成立 = 一个圈，而它喂的是 std::stable_sort（未定义行为）。
            // 判据用"**六种入参次序排出来必须是同一个答案**"：比较器不是全序时，
            // stable_sort 分区时"看谁先"会随入参次序变，排出来的东西就跟着变。
            {
                const QString v19 = QStringLiteral("1.9");
                const QString v110 = QStringLiteral("1.10");
                const QString v110pre = QStringLiteral("1.10-pre1");
                const QStringList expectedOldest{v19, v110, v110pre};
                const QList<QStringList> orders{
                    {v19, v110, v110pre}, {v19, v110pre, v110}, {v110, v19, v110pre},
                    {v110, v110pre, v19}, {v110pre, v19, v110}, {v110pre, v110, v19}};
                int agreed = 0;
                for (const QStringList &order : orders) {
                    if (ResourceCatalog::gameVersionsOldestFirst(order) == expectedOldest) {
                        ++agreed;
                    }
                }
                check(agreed == orders.size(),
                      "Z7h 那三段的圈没了：六种入参次序排出同一个答案（1.9 < 1.10 < 1.10-pre1）",
                      QStringLiteral("%1/6 种一致").arg(agreed));
                check(ResourceCatalog::gameVersionsOldestFirst(
                          {QStringLiteral("1.20.6"), QStringLiteral("1.20.5-pre1"),
                           QStringLiteral("1.20.5")}) ==
                          QStringList({QStringLiteral("1.20.5"),
                                       QStringLiteral("1.20.5-pre1"),
                                       QStringLiteral("1.20.6")}),
                      "Z7h 5-pre1 和 6 按**数**比（1.20.5-pre1 在 1.20.6 前面），"
                      "次序不变");
                check(ResourceCatalog::gameVersionsOldestFirst(
                          {QStringLiteral("Forge"), QStringLiteral("1.21"),
                           QStringLiteral("1.9")}) ==
                          QStringList({QStringLiteral("1.9"), QStringLiteral("1.21"),
                                       QStringLiteral("Forge")}),
                      "Z7h 一个数字都不带的段（CurseForge 的加载器名）排在数字版本后面");
            }
            // 读不动 / 不是一份搜索响应：两条话要分得开（和 Z4 同一个规矩）。
            const ResourceCatalog::SearchResult notSearch = ResourceCatalog::parseCurseForgeSearch(
                QByteArray(R"({"error":"Forbidden","description":null})"));
            check(!notSearch.ok && notSearch.problem.contains(QStringLiteral("Forbidden")),
                  "Z7f 接口出错时把它自己的那句话带出来", notSearch.problem);
            check(!ResourceCatalog::parseCurseForgeSearch(QByteArray("<html>403</html>")).ok,
                  "Z7f 一段 HTML：报错，不是「没搜到」");
        }
    }

    section("Z7h. 读真文件列表（meta/curseforge-files-jei.json）");

    {
        QByteArray json;
        QFile fixture(metaDir + "/curseforge-files-jei.json");
        const bool haveFixture = fixture.open(QIODevice::ReadOnly);
        check(haveFixture, "Z7h 读到了真文件列表夹具");
        if (haveFixture) {
            json = fixture.readAll();
            fixture.close();
        }

        // 根是对象（`{"data":[…]}`）—— parseVersions **自己**认出来并转到那一家那半边，
        // 调用方不必先判来源（判的话，"哪个接口回哪种形状"就抄到调用点上了）。
        const QList<ResourceCatalog::VersionEntry> versions =
            ResourceCatalog::parseVersions(json);
        check(versions.size() == 2, "Z7h 两条（那边一条就是一个文件）",
              QStringLiteral("%1 条").arg(versions.size()));
        if (versions.size() == 2) {
            const ResourceCatalog::VersionEntry &first = versions.first();
            check(first.id == QStringLiteral("8964987") &&
                      first.versionType == QStringLiteral("beta") &&
                      first.datePublished.startsWith(QStringLiteral("2026-09-24")),
                  "Z7h 身份 / releaseType 翻成了和 Modrinth 一样的词",
                  first.id + QLatin1Char(' ') + first.versionType + QLatin1Char(' ') +
                      first.datePublished);
            // `gameVersions` 那一串里混着 `Client` / `Server` / `Forge`（真响应就长这样）：
            // 游戏版本挑出来、加载器翻成小写放进 loaders、另两个**两边都不进**。
            check(first.gameVersions ==
                      QStringList({QStringLiteral("1.21"), QStringLiteral("1.21.1")}) &&
                      first.loaders == QStringList{QStringLiteral("forge")},
                  "Z7h Client / Server 不许混进游戏版本，Forge 翻成小写进加载器",
                  first.gameVersions.join(QLatin1Char(' ')) + QStringLiteral(" | ") +
                      first.loaders.join(QLatin1Char(',')));
            const ResourceCatalog::FileEntry *file = first.primaryFile();
            check(file != nullptr &&
                      file->filename == QStringLiteral("jei-1.21.1-forge-19.57.0.449.jar") &&
                      file->size == 2246008 && file->sha1.size() == 40 &&
                      file->url.startsWith(QStringLiteral("https://edge.forgecdn.net/")) &&
                      file->unavailableNote.isEmpty(),
                  "Z7h 要下的那个文件：名字 / 字节数 / sha1（algo==1 那条）/ 地址",
                  file == nullptr ? QStringLiteral("没有文件") : file->filename);
            // 顺序：那一家回的也是新的在前（和 Modrinth 一样，界面不用按来源分叉）。
            check(versions.first().datePublished > versions.last().datePublished,
                  "Z7h 顺序是新的在前",
                  versions.first().datePublished + QStringLiteral(" / ") +
                      versions.last().datePublished);
        }

        // 空链接不等于明确不可用；资源页和整合包导入使用相同 CDN 候选规则。
        const QByteArray notDistributable = QByteArrayLiteral(
            "{\"data\":[{\"id\":123,\"fileName\":\"secret.jar\",\"fileLength\":10,"
            "\"downloadUrl\":null,\"gameVersions\":[\"1.21.1\",\"Forge\"],"
            "\"hashes\":[{\"value\":\"x\",\"algo\":2}],\"releaseType\":1}]}");
        const QList<ResourceCatalog::VersionEntry> locked =
            ResourceCatalog::parseCurseForgeVersions(notDistributable);
        check(locked.size() == 1 && locked.first().primaryFile() != nullptr &&
                  locked.first().primaryFile()->url == QStringLiteral("https://edge.forgecdn.net/files/0/123/secret.jar") &&
                  locked.first().primaryFile()->unavailableNote.isEmpty(),
              "Z7h downloadUrl 是 null：生成候选，不误称作者禁止分发",
              locked.isEmpty() ? QString()
                               : locked.first().primaryFile()->unavailableNote);
        QJsonObject unavailable = QJsonDocument::fromJson(notDistributable).object();
        QJsonArray unavailableFiles = unavailable.value(QStringLiteral("data")).toArray();
        QJsonObject unavailableFile = unavailableFiles.first().toObject();
        unavailableFile.insert(QStringLiteral("isAvailable"), false);
        unavailableFiles[0] = unavailableFile;
        unavailable.insert(QStringLiteral("data"), unavailableFiles);
        const auto blocked = ResourceCatalog::parseCurseForgeVersions(QJsonDocument(unavailable).toJson());
        check(blocked.size() == 1 && blocked.first().primaryFile() != nullptr &&
                  blocked.first().primaryFile()->url.isEmpty() &&
                  !blocked.first().primaryFile()->unavailableNote.isEmpty(),
              "Z7h 平台明确不可用时不构造下载地址");
        // md5 那条**不能**当 sha1 用（algo 2 是 md5）。上面那份夹具两条都有，读对了才是
        // 40 位；这一份只有 md5，读出来必须是空的（不然下载校验会一路失败）。
        check(locked.size() == 1 && locked.first().primaryFile() != nullptr &&
                  locked.first().primaryFile()->sha1.isEmpty(),
              "Z7h 只有 md5 时 sha1 留空（`algo == 1` 那条才是 sha1）");
    }
}


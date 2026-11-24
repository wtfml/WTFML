/**
 * @file ResourceCatalog.cpp
 * @brief ResourceCatalog 的实现：拼请求地址 + 读接口回的那段 JSON。**一个网络请求都不发**
 * （这个文件里没有 QNetworkAccessManager，将来也不会加 —— 发请求是 ResourceStore 的事）。
 */
#include "ResourceCatalog.hpp"
#include <QCoreApplication>
#include <cmath>

#include "manage/ModpackImport.hpp"  // curseForgeApiBase() —— CurseForge 那张接口根地址的唯一出处

#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QUrl>

namespace ResourceCatalog {

namespace {

/// 接口的根地址。Modrinth 官方那一个 —— mcimirror 那个镜像也认 `/modrinth/v2/...`，
/// 但这一轮**先不接镜像**：搜索是一次一条的小请求，不是几千个对象那种非镜像不可的量，
/// 而官方那个实测也通。将来要接镜像，改的只有这一处。
QString apiRoot() { return QStringLiteral("https://api.modrinth.com/v2"); }

/// 图标的根地址。和 apiRoot 是**两个不同的域名**（cdn.），别合并。
QString cdnRoot() { return QStringLiteral("https://cdn.modrinth.com"); }

/// 百分号编码。查询参数的值一律过它 —— 搜索词是用户手打的，`&`、`#`、空格、中文什么都有，
/// 不编码的话拼出来的地址会在第一个 `&` 那里断掉，变成"搜别的东西"。
QString enc(const QString &text) {
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

/// 把一串字符串拼成一维 JSON 数组再编码：`["1.21.1"]`。
///
/// NOTE: 用 QJsonDocument 拼，**不是手写方括号加引号**。手写的话，值里出现一个引号
/// （搜索词里就可能有）就拼出一段读不懂的 JSON，接口回 400 —— 而 400 到界面上
/// 只是一句"搜索失败"，没人会想到是引号。
QString jsonArrayParam(const QStringList &values) {
    QJsonArray array;
    for (const QString &value : values) {
        array.append(value);
    }
    return enc(QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

/// 同上，但拼的是二维数组：`[["project_type:mod"],["versions:1.21.1"]]`。
/// facets 要的就是这个形状 —— **一层数组是"或"、两层是"且"**，少一层方括号整个筛选就失效
/// （接口不报错，只是把筛选当没写，症状是"明明按 1.21.1 筛的，出来一堆别的版本"）。
QString facetsParam(const QStringList &groups) {
    QJsonArray outer;
    for (const QString &group : groups) {
        QJsonArray inner;
        inner.append(group);
        outer.append(inner);
    }
    return enc(QString::fromUtf8(QJsonDocument(outer).toJson(QJsonDocument::Compact)));
}

/// JSON 里那种"可能没有"的字符串列表。**不是数组就当空**（接口偶尔给 null）。
QStringList stringList(const QJsonValue &value) {
    QStringList out;
    const QJsonArray array = value.toArray();
    out.reserve(array.size());
    for (const QJsonValue &item : array) {
        out.append(item.toString());
    }
    return out;
}

/// 读不懂 JSON 时统一的那句话。带上 QJsonParseError 的位置 —— 报"解析失败"等于没报，
/// 而这句是要显示给用户的，得让他能判断"是网络中间被劫持了"还是"接口变了"。
QString unreadableText(const QJsonParseError &error) {
    return QCoreApplication::translate("ResourceCatalog", "接口返回的数据无法解析（第 %1 个字节：%2）")
        .arg(error.offset)
        .arg(error.errorString());
}

/// Modrinth 搜索里当"加载器"用的那几个词。
///
/// NOTE: 这张表和 ModpackImport::loaderDependencyKeys() **不是一回事**，别合并：
/// 那几个是**整合包清单**里 `dependencies` 的键（一份包里只会有一个，用来判
/// "按哪个加载器装"）；这几个是**搜索接口**的 categories 词表（一个模组的 categories
/// 里可能一个都没有 —— 比如数据包）。两边的词也不完全重合：这里有 liteloader /
/// modloader / rift，那边有 quilt。
/// CurseForge 那一家的接口根（`…/v1`，**带 v1 那一段**）。
///
/// 这里刻意**不写域名**：默认走哪一台（不要 key 的社区镜像、还是官方 + key）、用户在设置里
/// 填没填 key，全是 `ModpackImport::curseForgeApiBase()` 一处说了算（见那个函数的注释）。
/// 把 `apiBase` 当 override 传进去，三种情形就都落在那一个函数里了：
/// 传进来的非空 → 就用它（验证工程指本机假服务器）；空 → 没填 key 的那一套（社区镜像）。
///
/// 症状提醒：这里要是自己拼一个 `https://api.curseforge.com/v1`，看起来"更直接"，
/// 但那一条**不带 key 就是 403**，而界面上只会显示"搜索失败"。
QString curseForgeApiRoot(const QString &apiBase) {
    return ModpackImport::curseForgeApiBase(QString(), apiBase);
}

/// 把 `{键, 中文标签}` 那张表变成界面上那一栏的选项，并在**最前面**补一项「全部」。
/// （两张表都不含「全部」那一项 —— 它是每一栏都有的，写进每一张表里迟早会漏一处。）
QList<FilterOption> filterOptions(const QList<QPair<QString, QString>> &pairs) {
    QList<FilterOption> out;
    out.reserve(pairs.size() + 1);
    out.append(FilterOption{QString(), QCoreApplication::translate("ResourceCatalog", "全部")});
    for (const QPair<QString, QString> &pair : pairs) {
        out.append(FilterOption{pair.first, pair.second});
    }
    return out;
}

/// 版本号里的一段：**前导数字** + 后面剩下那截。`1.20.5-pre1` 的第二段 → (5, "-pre1")；
/// `Forge` / `pre1` 这种一位数字都不带的 → (没有数字, 原文)。
///
/// NOTE: 拆出"前导数字"而不是"整段是不是数"，正是为了 `5-pre1` 和 `6` 能按 **5 < 6** 比
/// （`1.20.5-pre1` 该排在 `1.20.6` 前面）。整段判类型的写法在这里会退回字符串比，
/// 于是 `5-pre1` 大于 `6` —— 那是另一套次序（见下面那段）。
struct VersionSegment {
    bool numbered = false;  ///< 有没有前导数字
    qint64 number = 0;      ///< 那个数（没有前导数字时无意义）
    QString rest;           ///< 前导数字之后剩下的那截（没有前导数字时是原文）
};

/// 前导数字最多读这么多个字符。更长的（一份 20 位的"版本号"）**当它没有前导数字**：
/// 折成 0 会让两个不同的巨数比成相等，而"按剩下的那截比"至少还是个全序。
constexpr int kMaxVersionDigits = 18;

VersionSegment splitVersionSegment(const QString &text) {
    int digits = 0;
    while (digits < text.size()) {
        const QChar ch = text.at(digits);
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) {
            break;
        }
        ++digits;
    }
    VersionSegment out;
    out.rest = text;
    if (digits > 0 && digits <= kMaxVersionDigits) {
        out.numbered = true;
        out.number = text.left(digits).toLongLong();
        out.rest = text.mid(digits);
    }
    return out;
}

/// 两段的次序：**带前导数字的在前**，然后比那个数，最后比剩下的那截。
///
/// 三个判据各自都是全序，按这个优先级拼起来就是全序 —— 这一条是硬要求，见下面那段。
int compareVersionSegment(const VersionSegment &a, const VersionSegment &b) {
    if (a.numbered != b.numbered) {
        return a.numbered ? -1 : 1;
    }
    if (a.numbered && a.number != b.number) {
        return a.number < b.number ? -1 : 1;
    }
    const int compared = QString::compare(a.rest, b.rest);
    if (compared != 0) {
        return compared < 0 ? -1 : 1;
    }
    return 0;
}

/// 「1.9 比 1.10 旧」那件事：按 `.` 切成段比。**只给 gameVersionsOldestFirst 用。**
///
/// 前面几段一样时**段数少的在前**（1.21 < 1.21.1）。段与段之间走
/// compareVersionSegment()：带前导数字的按数比（字符串比会把 1.10 判成小于 1.9），
/// 5-pre1 这种"数 + 后缀"先比数、再比后缀。
///
/// NOTE: 已知的、和改之前一致的近似：**预发布被排在它自己的正式版后面**（1.21 < 1.21-pre1，
/// 语义上该反过来）。这是原来就有的行为，这次只动"圈"，没动它。
/// 那把尺子的**预解析形**：把 `1.20.5-pre1` 拆成段（下面那个比较器只比这个）。
///
/// 拆成"拆"和"比"两步是有用意的：`groupVersions()` 要按游戏版本**组排序**，而一次排序
/// 是 N log N 次比较 —— 比较器里现拆的话，每次比较都要 split 两遍。 起那一趟
/// 先把每个组的键拆一遍（decorate-sort-undecorate），比较器只读预算好的这一份。
using VersionTextParts = QList<VersionSegment>;

static VersionTextParts splitVersionText(const QString &text) {
    const QStringList raw = text.split(QLatin1Char('.'));
    VersionTextParts parts;
    parts.reserve(raw.size());
    for (const QString &segment : raw) {
        parts.append(splitVersionSegment(segment));
    }
    return parts;
}

/// 两份**已经拆好**的版本号比大小（判据和上面那段一字不差，只是不再自己拆）。
static int compareVersionParts(const VersionTextParts &a, const VersionTextParts &b) {
    const int shared = qMin(a.size(), b.size());
    for (int i = 0; i < shared; ++i) {
        const int compared = compareVersionSegment(a.at(i), b.at(i));
        if (compared != 0) {
            return compared;
        }
    }
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return 0;
}

int compareVersionText(const QString &left, const QString &right) {
    return compareVersionParts(splitVersionText(left), splitVersionText(right));
}

/// 加载器那几个词在 CurseForge 的**答复正文**里长什么样（`gameVersions` 那一串里混着的
/// `Forge` / `NeoForge` / …）。**不是** filterOption 的键、也不是 `modLoaderType` 的数字 ——
/// 那是"问的时候"叫什么，这是"答的时候"叫什么，两套东西，别合并。
const QStringList &curseForgeLoaderWords() {
    static const QStringList words = {
        QStringLiteral("forge"),    QStringLiteral("neoforge"),
        QStringLiteral("fabric"),   QStringLiteral("quilt"),
    };
    return words;
}

/// CurseForge 的 `modLoader` / `modLoaderType` **数字** → 加载器的名字（小写）。
QString curseForgeModLoaderName(int modLoader) {
    switch (modLoader) {
    case 1:
        return QStringLiteral("forge");
    case 4:
        return QStringLiteral("fabric");
    case 5:
        return QStringLiteral("quilt");
    case 6:
        return QStringLiteral("neoforge");
    default:
        return QString();
    }
}

/// CurseForge 的 `releaseType` 数字 → Modrinth 那三个词（第三层那行小字两个来源得一样）。
/// 认不出来（0 = 没写）回空串：**不猜**，界面上那一截就不显示。
QString curseForgeReleaseType(int releaseType) {
    switch (releaseType) {
    case 1:
        return QStringLiteral("release");
    case 2:
        return QStringLiteral("beta");
    case 3:
        return QStringLiteral("alpha");
    default:
        return QString();
    }
}

/// 一条 CurseForge 的 `hashes[]` 里挑出 sha1（`algo == 1`；`2` 是 md5）。
/// 取 `hashes[0]` 是错的 —— 实测那一条**正好**是 sha1，但顺序没有保证。
QString curseForgeSha1(const QJsonObject &file) {
    const QJsonArray hashes = file.value(QStringLiteral("hashes")).toArray();
    for (const QJsonValue &hash : hashes) {
        const QJsonObject object = hash.toObject();
        if (object.value(QStringLiteral("algo")).toInt() == 1) {
            return object.value(QStringLiteral("value")).toString();
        }
    }
    return QString();
}

/// 那一串 `gameVersions` 里，**哪些是真游戏版本**、哪些不是。
bool looksLikeGameVersion(const QString &text) {
    return !text.isEmpty() && text.at(0).isDigit();
}

const QStringList &loaderFacetNames() {
    static const QStringList names = {
        QStringLiteral("fabric"),     QStringLiteral("forge"),
        QStringLiteral("neoforge"),   QStringLiteral("quilt"),
        QStringLiteral("liteloader"), QStringLiteral("modloader"),
        QStringLiteral("rift"),
    };
    return names;
}

} // namespace

// ---------- 几类资源 ----------

QList<Kind> allKinds() {
    // 顺序照下载页左栏那份表（去掉「游戏」）：整合包 / 模组 / 数据包 / 资源包 / 光影。
    return {Kind::Modpack, Kind::Mod, Kind::DataPack, Kind::ResourcePack, Kind::Shader};
}

QString kindKey(Kind kind) {
    switch (kind) {
    case Kind::Mod:
        return QStringLiteral("mod");
    case Kind::Modpack:
        return QStringLiteral("modpack");
    case Kind::DataPack:
        // Modrinth 的 project_type 里就叫 datapack（界面上那一条写「数据包」）。
        return QStringLiteral("datapack");
    case Kind::Shader:
        return QStringLiteral("shader");
    case Kind::ResourcePack:
        return QStringLiteral("resourcepack");
    }
    return QString();
}

bool kindFromCategoryKey(const QString &key, Kind *out) {
    const QList<Kind> kinds = allKinds();
    for (Kind kind : kinds) {
        if (kindKey(kind) == key) {
            if (out != nullptr) {
                *out = kind;
            }
            return true;
        }
    }
    return false;
}

QString folderForKind(Kind kind) {
    switch (kind) {
    case Kind::Mod:
        return QStringLiteral("mods");
    case Kind::DataPack:
        // 和三个兄弟类一样下进实例自己的目录。原版只认存档里那份（`<存档>/datapacks/`）——
        // 这件事写在头文件那段里，界面上那句提示也得说清楚（见 downloadSearchHints）。
        return QStringLiteral("datapacks");
    case Kind::ResourcePack:
        return QStringLiteral("resourcepacks");
    case Kind::Shader:
        return QStringLiteral("shaderpacks");
    case Kind::Modpack:
        // 整合包下回来是个 .mrpack，接着走既有的导入流程 —— 往哪个目录放是那份清单说了算，
        // 不是这里说了算。回空串，调用方看到空串就该知道"这一类不走这条安装路"。
        return QString();
    }
    return QString();
}

QString kindLabel(Kind kind) {
    switch (kind) {
    case Kind::Mod:
        return QCoreApplication::translate("ResourceCatalog", "模组");
    case Kind::Modpack:
        return QCoreApplication::translate("ResourceCatalog", "整合包");
    case Kind::DataPack:
        return QCoreApplication::translate("ResourceCatalog", "数据包");
    case Kind::Shader:
        return QCoreApplication::translate("ResourceCatalog", "光影");
    case Kind::ResourcePack:
        return QCoreApplication::translate("ResourceCatalog", "资源包");
    }
    return QString();
}

QString modrinthApiRoot() { return apiRoot(); }

// ---------- 来源 ----------

QList<Source> allSources() {
    // 顺序就是界面上那个下拉框的顺序：Modrinth 在前面（不要 key、原来就能用的那个）。
    return {Source::Modrinth, Source::CurseForge};
}

QString sourceKey(Source source) {
    switch (source) {
    case Source::Modrinth:
        return QStringLiteral("modrinth");
    case Source::CurseForge:
        return QStringLiteral("curseforge");
    }
    return QString();
}

QString sourceLabel(Source source) {
    switch (source) {
    case Source::Modrinth:
        return QStringLiteral("Modrinth");
    case Source::CurseForge:
        return QStringLiteral("CurseForge");
    }
    return QString();
}

bool sourceFromKey(const QString &key, Source *out) {
    const QList<Source> sources = allSources();
    for (Source source : sources) {
        if (sourceKey(source) == key) {
            if (out != nullptr) {
                *out = source;
            }
            return true;
        }
    }
    return false;
}

int curseForgeClassId(Kind kind) {
    // 九条 `isClass=true` 里我们用到的那五个（表在 .hpp 里）。**和 ModpackImport 那边
    // 是同一套号** —— 这边管"去哪个类目里搜"，那边管"清单里的文件落哪个目录"。
    switch (kind) {
    case Kind::Mod:
        return 6;
    case Kind::Modpack:
        return 4471;
    case Kind::DataPack:
        return 6945;
    case Kind::ResourcePack:
        return 12;
    case Kind::Shader:
        return 6552;
    }
    return 0;
}

// ---------- 那一行筛选项 ----------

QList<FilterOption> categoryOptions(Source source, Kind kind) {
    if (source == Source::CurseForge) {
        // 类目号来自 `/v1/categories?gameId=432`（ 抄下来的）。**挑的是那一类底下
        // 的常用子类**（parentCategoryId == 那个类目号），不是全抄 —— 模组那一类有 52 条，
        // 里面一半是 `Addons/<某个模组>` 那种"给 Create 做附属"的，摆进下拉框没人找得到。
        switch (kind) {
        case Kind::Mod:
            return filterOptions({
                {QStringLiteral("6814"), QCoreApplication::translate("ResourceCatalog", "优化")},
                {QStringLiteral("421"), QCoreApplication::translate("ResourceCatalog", "前置库")},
                {QStringLiteral("434"), QCoreApplication::translate("ResourceCatalog", "装备武器")},
                {QStringLiteral("424"), QCoreApplication::translate("ResourceCatalog", "装饰外观")},
                {QStringLiteral("422"), QCoreApplication::translate("ResourceCatalog", "冒险")},
                {QStringLiteral("419"), QCoreApplication::translate("ResourceCatalog", "魔法")},
                {QStringLiteral("412"), QCoreApplication::translate("ResourceCatalog", "科技")},
                {QStringLiteral("406"), QCoreApplication::translate("ResourceCatalog", "世界生成")},
                {QStringLiteral("408"), QCoreApplication::translate("ResourceCatalog", "矿物资源")},
                {QStringLiteral("407"), QCoreApplication::translate("ResourceCatalog", "生物群系")},
                {QStringLiteral("411"), QCoreApplication::translate("ResourceCatalog", "生物")},
                {QStringLiteral("409"), QCoreApplication::translate("ResourceCatalog", "结构")},
                {QStringLiteral("436"), QCoreApplication::translate("ResourceCatalog", "食物")},
                {QStringLiteral("420"), QCoreApplication::translate("ResourceCatalog", "存储")},
                {QStringLiteral("4558"), QCoreApplication::translate("ResourceCatalog", "红石")},
                {QStringLiteral("5191"), QCoreApplication::translate("ResourceCatalog", "实用与体验")},
                {QStringLiteral("435"), QCoreApplication::translate("ResourceCatalog", "服务端")},
                {QStringLiteral("423"), QCoreApplication::translate("ResourceCatalog", "地图与信息")},
                {QStringLiteral("6821"), QCoreApplication::translate("ResourceCatalog", "修复")},
                {QStringLiteral("425"), QCoreApplication::translate("ResourceCatalog", "杂项")},
            });
        case Kind::Modpack:
            return filterOptions({
                {QStringLiteral("4472"), QCoreApplication::translate("ResourceCatalog", "科技")},
                {QStringLiteral("4473"), QCoreApplication::translate("ResourceCatalog", "魔法")},
                {QStringLiteral("4475"), QCoreApplication::translate("ResourceCatalog", "冒险与 RPG")},
                {QStringLiteral("4476"), QCoreApplication::translate("ResourceCatalog", "探索")},
                {QStringLiteral("4478"), QCoreApplication::translate("ResourceCatalog", "任务")},
                {QStringLiteral("4736"), QCoreApplication::translate("ResourceCatalog", "空岛")},
                {QStringLiteral("4483"), QCoreApplication::translate("ResourceCatalog", "战斗与 PvP")},
                {QStringLiteral("4477"), QCoreApplication::translate("ResourceCatalog", "小游戏")},
                {QStringLiteral("4484"), QCoreApplication::translate("ResourceCatalog", "多人")},
                {QStringLiteral("5128"), QCoreApplication::translate("ResourceCatalog", "原版增强")},
                {QStringLiteral("4479"), QCoreApplication::translate("ResourceCatalog", "硬核")},
                {QStringLiteral("9243"), QCoreApplication::translate("ResourceCatalog", "专家")},
                {QStringLiteral("7418"), QCoreApplication::translate("ResourceCatalog", "恐怖")},
                {QStringLiteral("4481"), QCoreApplication::translate("ResourceCatalog", "轻量")},
                {QStringLiteral("4482"), QCoreApplication::translate("ResourceCatalog", "大型")},
                {QStringLiteral("4474"), QCoreApplication::translate("ResourceCatalog", "科幻")},
            });
        case Kind::DataPack:
            return filterOptions({
                {QStringLiteral("6946"), QCoreApplication::translate("ResourceCatalog", "模组支持")},
                {QStringLiteral("6951"), QCoreApplication::translate("ResourceCatalog", "科技")},
                {QStringLiteral("6952"), QCoreApplication::translate("ResourceCatalog", "魔法")},
                {QStringLiteral("6948"), QCoreApplication::translate("ResourceCatalog", "冒险")},
                {QStringLiteral("6950"), QCoreApplication::translate("ResourceCatalog", "前置库")},
                {QStringLiteral("6953"), QCoreApplication::translate("ResourceCatalog", "实用")},
                {QStringLiteral("6949"), QCoreApplication::translate("ResourceCatalog", "奇幻")},
                {QStringLiteral("6947"), QCoreApplication::translate("ResourceCatalog", "杂项")},
            });
        case Kind::ResourcePack:
            return filterOptions({
                {QStringLiteral("393"), QStringLiteral("16x")},
                {QStringLiteral("394"), QStringLiteral("32x")},
                {QStringLiteral("395"), QStringLiteral("64x")},
                {QStringLiteral("396"), QStringLiteral("128x")},
                {QStringLiteral("397"), QStringLiteral("256x")},
                {QStringLiteral("398"), QCoreApplication::translate("ResourceCatalog", "512x 及以上")},
                {QStringLiteral("403"), QCoreApplication::translate("ResourceCatalog", "传统")},
                {QStringLiteral("401"), QCoreApplication::translate("ResourceCatalog", "现代")},
                {QStringLiteral("402"), QCoreApplication::translate("ResourceCatalog", "中世纪")},
                {QStringLiteral("400"), QCoreApplication::translate("ResourceCatalog", "写实")},
                {QStringLiteral("404"), QCoreApplication::translate("ResourceCatalog", "动画")},
                {QStringLiteral("4465"), QCoreApplication::translate("ResourceCatalog", "模组支持")},
                {QStringLiteral("5244"), QCoreApplication::translate("ResourceCatalog", "字体")},
                {QStringLiteral("405"), QCoreApplication::translate("ResourceCatalog", "杂项")},
            });
        case Kind::Shader:
            return filterOptions({
                {QStringLiteral("6553"), QCoreApplication::translate("ResourceCatalog", "写实")},
                {QStringLiteral("6554"), QCoreApplication::translate("ResourceCatalog", "奇幻")},
                {QStringLiteral("6555"), QCoreApplication::translate("ResourceCatalog", "原版风")},
            });
        }
        return QList<FilterOption>();
    }

    // Modrinth：键是 slug（`/v2/tag/category` 里那些）。**母集比 CurseForge 小**，
    // 所以没有被砍掉的"附属模组"那类问题，反而要挑：资源包那 29 条里一半是
    // `resolutions` / `features` 那种"改了哪一块"的细分。
    switch (kind) {
    case Kind::Mod:
        return filterOptions({
            {QStringLiteral("optimization"), QCoreApplication::translate("ResourceCatalog", "优化")},
            {QStringLiteral("library"), QCoreApplication::translate("ResourceCatalog", "前置库")},
            {QStringLiteral("equipment"), QCoreApplication::translate("ResourceCatalog", "装备")},
            {QStringLiteral("decoration"), QCoreApplication::translate("ResourceCatalog", "装饰")},
            {QStringLiteral("adventure"), QCoreApplication::translate("ResourceCatalog", "冒险")},
            {QStringLiteral("magic"), QCoreApplication::translate("ResourceCatalog", "魔法")},
            {QStringLiteral("technology"), QCoreApplication::translate("ResourceCatalog", "科技")},
            {QStringLiteral("worldgen"), QCoreApplication::translate("ResourceCatalog", "世界生成")},
            {QStringLiteral("mobs"), QCoreApplication::translate("ResourceCatalog", "生物")},
            {QStringLiteral("food"), QCoreApplication::translate("ResourceCatalog", "食物")},
            {QStringLiteral("storage"), QCoreApplication::translate("ResourceCatalog", "存储")},
            {QStringLiteral("transportation"), QCoreApplication::translate("ResourceCatalog", "运输")},
            {QStringLiteral("game-mechanics"), QCoreApplication::translate("ResourceCatalog", "游戏机制")},
            {QStringLiteral("management"), QCoreApplication::translate("ResourceCatalog", "管理")},
            {QStringLiteral("minigame"), QCoreApplication::translate("ResourceCatalog", "小游戏")},
            {QStringLiteral("social"), QCoreApplication::translate("ResourceCatalog", "社交")},
            {QStringLiteral("utility"), QCoreApplication::translate("ResourceCatalog", "实用")},
            {QStringLiteral("cursed"), QCoreApplication::translate("ResourceCatalog", "诅咒")},
        });
    case Kind::Modpack:
        return filterOptions({
            {QStringLiteral("optimization"), QCoreApplication::translate("ResourceCatalog", "优化")},
            {QStringLiteral("technology"), QCoreApplication::translate("ResourceCatalog", "科技")},
            {QStringLiteral("magic"), QCoreApplication::translate("ResourceCatalog", "魔法")},
            {QStringLiteral("adventure"), QCoreApplication::translate("ResourceCatalog", "冒险")},
            {QStringLiteral("quests"), QCoreApplication::translate("ResourceCatalog", "任务")},
            {QStringLiteral("combat"), QCoreApplication::translate("ResourceCatalog", "战斗")},
            {QStringLiteral("multiplayer"), QCoreApplication::translate("ResourceCatalog", "多人")},
            {QStringLiteral("challenging"), QCoreApplication::translate("ResourceCatalog", "挑战")},
            {QStringLiteral("kitchen-sink"), QCoreApplication::translate("ResourceCatalog", "大杂烩")},
            {QStringLiteral("lightweight"), QCoreApplication::translate("ResourceCatalog", "轻量")},
        });
    case Kind::DataPack:
        // **空表**：Modrinth 的分类表里根本没有 datapack 这一档（实测 `/v2/tag/category`
        // 只给 mod / modpack / resourcepack / shader / 服务器）。界面上看到空表就把整个
        // 「类型」框藏起来 —— 摆一个只有「全部」的框等于骗人。
        return QList<FilterOption>();
    case Kind::ResourcePack:
        return filterOptions({
            {QStringLiteral("16x"), QStringLiteral("16x")},
            {QStringLiteral("32x"), QStringLiteral("32x")},
            {QStringLiteral("64x"), QStringLiteral("64x")},
            {QStringLiteral("128x"), QStringLiteral("128x")},
            {QStringLiteral("256x"), QStringLiteral("256x")},
            {QStringLiteral("512x+"), QCoreApplication::translate("ResourceCatalog", "512x 及以上")},
            {QStringLiteral("realistic"), QCoreApplication::translate("ResourceCatalog", "写实")},
            {QStringLiteral("simplistic"), QCoreApplication::translate("ResourceCatalog", "简约")},
            {QStringLiteral("themed"), QCoreApplication::translate("ResourceCatalog", "主题")},
            {QStringLiteral("vanilla-like"), QCoreApplication::translate("ResourceCatalog", "原版风")},
            {QStringLiteral("decoration"), QCoreApplication::translate("ResourceCatalog", "装饰")},
            {QStringLiteral("modded"), QCoreApplication::translate("ResourceCatalog", "模组支持")},
            {QStringLiteral("tweaks"), QCoreApplication::translate("ResourceCatalog", "微调")},
            {QStringLiteral("cursed"), QCoreApplication::translate("ResourceCatalog", "诅咒")},
        });
    case Kind::Shader:
        return filterOptions({
            {QStringLiteral("realistic"), QCoreApplication::translate("ResourceCatalog", "写实")},
            {QStringLiteral("semi-realistic"), QCoreApplication::translate("ResourceCatalog", "半写实")},
            {QStringLiteral("fantasy"), QCoreApplication::translate("ResourceCatalog", "奇幻")},
            {QStringLiteral("cartoon"), QCoreApplication::translate("ResourceCatalog", "卡通")},
            {QStringLiteral("vanilla-like"), QCoreApplication::translate("ResourceCatalog", "原版风")},
            {QStringLiteral("potato"), QCoreApplication::translate("ResourceCatalog", "极低配")},
            {QStringLiteral("path-tracing"), QCoreApplication::translate("ResourceCatalog", "光追")},
            {QStringLiteral("pbr"), QCoreApplication::translate("ResourceCatalog", "PBR 材质")},
            {QStringLiteral("colored-lighting"), QCoreApplication::translate("ResourceCatalog", "彩色光照")},
            {QStringLiteral("reflections"), QCoreApplication::translate("ResourceCatalog", "反射")},
            {QStringLiteral("shadows"), QCoreApplication::translate("ResourceCatalog", "阴影")},
            {QStringLiteral("atmosphere"), QCoreApplication::translate("ResourceCatalog", "大气")},
            {QStringLiteral("foliage"), QCoreApplication::translate("ResourceCatalog", "植被")},
            {QStringLiteral("bloom"), QCoreApplication::translate("ResourceCatalog", "泛光")},
        });
    }
    return QList<FilterOption>();
}

QList<FilterOption> loaderOptions(Source source) {
    if (source == Source::CurseForge) {
        return filterOptions({
            {QStringLiteral("4"), QStringLiteral("Fabric")},
            {QStringLiteral("1"), QStringLiteral("Forge")},
            {QStringLiteral("6"), QStringLiteral("NeoForge")},
            {QStringLiteral("5"), QStringLiteral("Quilt")},
        });
    }
    return filterOptions({
        {QStringLiteral("fabric"), QStringLiteral("Fabric")},
        {QStringLiteral("forge"), QStringLiteral("Forge")},
        {QStringLiteral("neoforge"), QStringLiteral("NeoForge")},
        {QStringLiteral("quilt"), QStringLiteral("Quilt")},
    });
}

QString loaderRequestValue(const QString &loader, Source source) {
    const QString key = loader.trimmed().toLower();
    if (key.isEmpty()) {
        return QString();
    }
    // 键和标签**两套都认**：那一栏给出来的是键（CurseForge 是 `4`），而
    // `instanceLoaderName()` 给的是标签（「Fabric」）—— 上面那两类调用方各走一套。
    const QList<FilterOption> options = loaderOptions(source);
    for (const FilterOption &option : options) {
        if (key == option.key.toLower() || key == option.label.toLower()) {
            return option.key;
        }
    }
    // 认不出来就回空串 = **不筛**。别把一个不在白名单里的词（`OptiFine` 这种）拼上去：
    // 那边不报错，只是什么都筛不出来，而"什么都没筛出来"和"这个资源一个版本都没有"在
    // 界面上是同一句话。
    return QString();
}

// ---------- 搜索 ----------

QStringList Hit::loaders() const {
    // 接口自己说了加载器就**只认它**（CurseForge 那条路：categories 是「Technology」那种
    // 分类标签，按现有规则去里面找 `fabric` 会一个都找不到）。
    if (!explicitLoaders.isEmpty()) {
        return explicitLoaders;
    }
    const QStringList &known = loaderFacetNames();
    QStringList out;
    for (const QString &category : categories) {
        if (known.contains(category)) {
            out.append(category);
        }
    }
    return out;
}

QString Hit::iconPngUrl() const {
    // CurseForge 那条路拼不出地址来（那边的图在一个带哈希的路径下，**只有接口知道**），
    // 所以它的"兜底"就是接口给的第二条（`logo.url`，见 iconFallbackUrl 那段）。
    if (source == Source::CurseForge) {
        return iconFallbackUrl;
    }
    return iconUrlForProject(projectId);
}

QString Hit::iconCacheId() const {
    // 两个 id 空间各在自己那套里唯一，而 `238222` 这种纯数字 id 在 Modrinth 的 base62 里
    // 也是合法的 —— 不带来源的话，撞上之后那格的图标会一直是**另一个**资源的
    // （缓存默认不过期 ⇒ 一直错下去，而且看起来一个错都没有）。
    return sourceKey(source) + QLatin1Char('-') + projectId;
}

QString searchUrl(Kind kind, const QString &query, int offset, int limit,
                  const QString &gameVersion, const QString &loader, Source source,
                  const QString &category, const QString &apiBase) {
    if (source == Source::CurseForge) {
        // 这一家的 `/mods/search` **没有 project_type 这个概念**，只有 `classId`（类目号）
        // —— 也就是说"搜模组"和"搜资源包"在这里是靠类目号分开的，少传它就会把九类东西
        // 全混在一起回给你。
        QStringList params;
        params.append(QStringLiteral("gameId=432"));  // Minecraft（写死的，这家还管别的游戏）
        params.append(QStringLiteral("classId=") + QString::number(curseForgeClassId(kind)));
        params.append(QStringLiteral("searchFilter=") + enc(query));
        if (!gameVersion.isEmpty()) {
            params.append(QStringLiteral("gameVersion=") + enc(gameVersion));
        }
        if (!loader.isEmpty()) {
            // 数字（见 loaderOptions）。**空串不拼这一段** —— 不筛就是没有这个参数，
            // 而不是发一个空值上去（两个接口对空值的处理都不一样，靠不住）。
            params.append(QStringLiteral("modLoaderType=") + enc(loader));
        }
        if (!category.isEmpty()) {
            params.append(QStringLiteral("categoryId=") + enc(category));
        }
        params.append(QStringLiteral("index=") + QString::number(qMax(0, offset)));
        params.append(QStringLiteral("pageSize=") + QString::number(qBound(1, limit, 50)));
        // 排序：按**总下载量**倒序（界面那一页默认搜的就是「最热门」，两处口径要一致）。
        // `sortOrder=desc` 不能省 —— 这一家的默认方向是 asc，那样头一条会变成"这一类里
        // 最没人用的"。
        params.append(QStringLiteral("sortField=6"));
        params.append(QStringLiteral("sortOrder=desc"));

        return curseForgeApiRoot(apiBase) + QStringLiteral("/mods/search?") +
               params.join(QLatin1Char('&'));
    }

    QStringList groups;
    groups.append(QStringLiteral("project_type:") + kindKey(kind));
    if (!gameVersion.isEmpty()) {
        groups.append(QStringLiteral("versions:") + gameVersion);
    }
    if (!loader.isEmpty()) {
        // 注意这里是 `categories:` 不是 `loaders:` —— Modrinth 的搜索把加载器当普通分类标签，
        // 写 `loaders:fabric` 实测筛不出东西（那个词只在版本接口里用）。
        groups.append(QStringLiteral("categories:") + loader);
    }
    if (!category.isEmpty()) {
        // 类型和加载器在这边**是同一种东西**（都是 categories facet）—— 所以它俩同时选上时
        // 是"且"（两层数组）而不是互相覆盖。facets 的"一层或、两层且"见 facetsParam。
        groups.append(QStringLiteral("categories:") + category);
    }

    QStringList params;
    params.append(QStringLiteral("query=") + enc(query));
    params.append(QStringLiteral("facets=") + facetsParam(groups));
    // 空搜索词按下载量排（那是"最热门的模组"），有搜索词按相关度排。不这样的话，
    // 搜「sodium」的头一条会是名字里带 sodium 但没人用的东西。
    params.append(QStringLiteral("index=") +
                  (query.isEmpty() ? QStringLiteral("downloads") : QStringLiteral("relevance")));
    params.append(QStringLiteral("offset=") + QString::number(qMax(0, offset)));
    params.append(QStringLiteral("limit=") + QString::number(qBound(1, limit, 100)));

    return (apiBase.isEmpty() ? apiRoot() : apiBase) + QStringLiteral("/search?") +
           params.join(QLatin1Char('&'));
}

SearchResult parseSearch(const QByteArray &json) {
    SearchResult result;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        result.problem = unreadableText(error);
        return result;
    }

    const QJsonObject root = document.object();
    if (!root.contains(QStringLiteral("hits"))) {
        // 少了这一项就不是搜索响应（实测 400 时回的是 `{"error":...,"description":...}`）。
        // 把接口自己的那句话带出去 —— 它比"搜索失败"有用得多。
        const QString description = root.value(QStringLiteral("description")).toString();
        result.problem = description.isEmpty()
                             ? QCoreApplication::translate("ResourceCatalog", "这不是一份搜索结果（里面没有 hits）")
                             : QCoreApplication::translate("ResourceCatalog", "接口说：%1").arg(description);
        return result;
    }

    const QJsonArray hits = root.value(QStringLiteral("hits")).toArray();
    result.totalHits = root.value(QStringLiteral("total_hits")).toInt();
    result.offset = root.value(QStringLiteral("offset")).toInt();
    result.limit = root.value(QStringLiteral("limit")).toInt();
    result.hits.reserve(hits.size());
    for (const QJsonValue &item : hits) {
        const QJsonObject object = item.toObject();
        Hit hit;
        hit.source = Source::Modrinth;  // 写出来：读的人一眼看得出这半边的来源
        hit.projectId = object.value(QStringLiteral("project_id")).toString();
        hit.slug = object.value(QStringLiteral("slug")).toString();
        hit.title = object.value(QStringLiteral("title")).toString();
        hit.description = object.value(QStringLiteral("description")).toString();
        hit.author = object.value(QStringLiteral("author")).toString();
        hit.downloads = static_cast<qint64>(object.value(QStringLiteral("downloads")).toDouble());
        // 最后一次更新（详情页头卡上那句「5 天前」）。两家名字不同，各自那一栏见
        // parseCurseForgeSearch。读不到就是空串（界面那一截不显示）。
        hit.updatedIso = object.value(QStringLiteral("date_modified")).toString();
        hit.categories = stringList(object.value(QStringLiteral("categories")));
        hit.gameVersions = stringList(object.value(QStringLiteral("versions")));
        // 图标地址：只认我们那个域名（见 isUsableIconUrl），别的当"没给"——那时界面走
        // 自己拼的 icon.png。**注意这一条不等于"能用"**：它可能是一条 webp，而这一台
        // 机器解不解得动是 ResourceStore 的事。
        const QString givenIconUrl = object.value(QStringLiteral("icon_url")).toString();
        hit.iconUrl = isUsableIconUrl(givenIconUrl) ? givenIconUrl : QString();
        result.hits.append(hit);
    }

    // 一条都没有**不算错** —— "搜不到"和"没搜成"是两句话，界面上得分开说
    // （同一个坑在加载器树那边踩过：详见 MainWindow 里「无可用版本」与「列表没取到」）。
    result.ok = true;
    return result;
}

SearchResult parseCurseForgeSearch(const QByteArray &json) {
    SearchResult result;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        result.problem = unreadableText(error);
        return result;
    }

    const QJsonObject root = document.object();
    if (!root.contains(QStringLiteral("data"))) {
        // 不是一份搜索结果。出错时那一家回的是 `{"error":"…"}`，而**社区镜像**在超时 /
        // 过载时回的可能是别的形状（比如一个 Cloudflare 的页面被当成 json 塞进来）。
        // 两条都带出去：接口自己的那句话优先，没有就如实说形状不对。
        const QString apiError = root.value(QStringLiteral("error")).toString();
        result.problem = !apiError.isEmpty()
                             ? QCoreApplication::translate("ResourceCatalog", "接口说：%1").arg(apiError)
                             : QCoreApplication::translate("ResourceCatalog", "这不是一份搜索结果（里面没有 data）");
        return result;
    }

    const QJsonArray data = root.value(QStringLiteral("data")).toArray();
    const QJsonObject pagination = root.value(QStringLiteral("pagination")).toObject();
    result.totalHits = pagination.value(QStringLiteral("totalCount")).toInt();
    result.offset = pagination.value(QStringLiteral("index")).toInt();
    result.limit = pagination.value(QStringLiteral("pageSize")).toInt();
    result.hits.reserve(data.size());
    for (const QJsonValue &item : data) {
        const QJsonObject object = item.toObject();
        Hit hit;
        hit.source = Source::CurseForge;
        // 那边的 `id` 是个**数字**（1546263），而 Modrinth 那个是 base62 字符串。这里统一
        // 存成字符串 —— 下游（图标缓存文件名、请求钥匙、问版本列表）只把它当一个不透明的
        // 标识，没有一处需要它是数字。
        hit.projectId = QString::number(object.value(QStringLiteral("id")).toDouble(), 'f', 0);
        hit.slug = object.value(QStringLiteral("slug")).toString();
        hit.title = object.value(QStringLiteral("name")).toString();   // 名字在 `name` 里
        hit.description = object.value(QStringLiteral("summary")).toString();
        // 作者：那边是个数组（实测只有一位）。取第一个 —— 这一行小字放不下第二个。
        // **必须先判空**。`QJsonArray::first()` 在空数组上是越界读（Qt 里带
        // Q_ASSERT，release 下是未定义行为）—— 而空数组在这儿是**很容易到**的：
        // `toArray()` 对"没这个键 / 值是 null / 值是字符串"一律回空数组，所以镜像那边只要
        // 漏了 `authors`（或者哪一天真给了一条没有作者的记录），搜到这一条就当场读到一个
        // 垃圾值，轻则那行小字空着，重则整个搜索结果页画不出来。
        // 这一段里别的字段（id/slug/summary/categories/…）全都写了缺省兜底，只有这一处
        // 没有兜底 —— 补上：取不到就是空作者，那行小字自己会少写一段。
        const QJsonArray authors = object.value(QStringLiteral("authors")).toArray();
        if (!authors.isEmpty()) {
            hit.author = authors.first().toObject().value(QStringLiteral("name")).toString();
        }
        hit.downloads = static_cast<qint64>(object.value(QStringLiteral("downloadCount")).toDouble());
        // 最后一次更新。**名字和 Modrinth 不一样**（这边是 dateModified，那边是
        // date_modified）—— 两处各读各的，读到同一栏里去。
        hit.updatedIso = object.value(QStringLiteral("dateModified")).toString();
        // categories 在那边是一个个**对象**（`{"name":"Technology",…}`），取名字那一项。
        // 这一串现在只在出错排查时看（界面那行小字用的是作者和下载量），存英文名最贴近原样。
        const QJsonArray categories = object.value(QStringLiteral("categories")).toArray();
        for (const QJsonValue &category : categories) {
            hit.categories.append(category.toObject().value(QStringLiteral("name")).toString());
        }
        // 游戏版本和加载器都从 `latestFilesIndexes` 来（那边一次不给整张 supported-versions
        // 表）：那是"每个游戏版本各有一条最新文件"的小表，所以**要按版本去重**，然后排成
        // **旧的在前**（和 Modrinth 那份同一个方向，见 gameVersionsOldestFirst）。
        QStringList versions;
        QStringList loaders;
        const QJsonArray indexes = object.value(QStringLiteral("latestFilesIndexes")).toArray();
        for (const QJsonValue &index : indexes) {
            const QJsonObject entry = index.toObject();
            const QString version = entry.value(QStringLiteral("gameVersion")).toString();
            if (!version.isEmpty() && !versions.contains(version)) {
                versions.append(version);
            }
            // `modLoader` 是数字（1 Forge / 4 Fabric / 5 Quilt / 6 NeoForge），0 = 没写。
            // 翻成小写的名字 —— 加载器那道闸和界面上那行小字认的都是小写。
            const QString loader = curseForgeModLoaderName(entry.value(QStringLiteral("modLoader")).toInt());
            if (!loader.isEmpty() && !loaders.contains(loader)) {
                loaders.append(loader);
            }
        }
        hit.gameVersions = gameVersionsOldestFirst(versions);
        hit.explicitLoaders = loaders;

        // 图标：接口给两条（256px 的缩略图 + 原图），一大一小。**主次关系和 Modrinth 那边
        // 一样**（先试小的），只是那边那条小的是 webp、这边两条都是 png。
        // 两条都要过 isUsableIconUrl（域名是 media.forgecdn.net）—— 那是外部输入。
        const QJsonObject logo = object.value(QStringLiteral("logo")).toObject();
        const QString thumbnail = logo.value(QStringLiteral("thumbnailUrl")).toString();
        const QString full = logo.value(QStringLiteral("url")).toString();
        hit.iconUrl = isUsableIconUrl(thumbnail, Source::CurseForge) ? thumbnail : QString();
        hit.iconFallbackUrl = isUsableIconUrl(full, Source::CurseForge) ? full : QString();
        result.hits.append(hit);
    }

    // 一条都没有**不算错**（和 Modrinth 那条一样：搜不到 ≠ 没搜成）。
    result.ok = true;
    return result;
}

QStringList gameVersionsOldestFirst(const QStringList &versions) {
    QStringList out = versions;
    // stable_sort：比不出大小的两条（同一个版本写两种法）保持接口给的次序。
    std::stable_sort(out.begin(), out.end(), [](const QString &left, const QString &right) {
        return compareVersionText(left, right) < 0;
    });
    return out;
}

namespace {

/// 老式周号快照：两位年 + 'w' + 两位周 + 一个小写字母（24w14a / 13w41b）。
/// **只认这一个长度**：`20w14infinite` 那种愚人节版是另一回事（见头文件里那段"不猜"）。
bool isWeeklySnapshotId(const QString &lower) {
    if (lower.size() != 6 || lower.at(2) != QLatin1Char('w')) {
        return false;
    }
    const bool digits = lower.at(0).isDigit() && lower.at(1).isDigit() &&
                        lower.at(3).isDigit() && lower.at(4).isDigit();
    const QChar suffix = lower.at(5);
    return digits && suffix >= QLatin1Char('a') && suffix <= QLatin1Char('z');
}

/// `1.21.4` / `26.2` 这种"纯数字加点"的前缀才认。
///
/// NOTE: **不认的就不折**。愚人节版 `1.RV-Pre1` 里那一段是 `1.RV`，折出来是一个谁也不
/// 是的大版本 —— 摆在"游戏版本"那一排里，用户点进去什么都搜不到，而且看不出哪里
/// 错了。`20w14infinite` 那一路用的是同一条规矩（见头文件那段"不猜"）。
bool isPlainVersionStem(const QString &text) {
    if (text.isEmpty() || !text.at(0).isDigit()) {
        return false;
    }
    bool lastWasDot = false;
    for (const QChar c : text) {
        if (c.isDigit()) {
            lastWasDot = false;
            continue;
        }
        if (c == QLatin1Char('.') && !lastWasDot) {
            lastWasDot = true;
            continue;
        }
        return false;
    }
    return !lastWasDot;
}

/// 快照号长什么样。两件事一起算，是因为"折出来的那一半"正是这两个公开函数唯一的区别。
struct SnapshotShape {
    bool snapshot = false;  ///< 这是不是一个快照号
    QString stem;           ///< 它属于哪个大版本（空串 = 折不出来，比如 24w14a）
};

/**
 * @brief 认四套写法，都是 Mojang 真用过的。
 *
 * ① 新式快照 `26.2-snapshot-1` / `26.1-snapshot-11` —— 之前**一条都不认**：
 * 它们既没有 `-pre`/`-rc`、长度也不是 6，于是不勾「显示快照版」时照样摆在那一排
 * chip 上，下载页那棵树的第二层也一家伙多出十来行（26.3-snapshot-1…10 各占一行）。
 * ② 预览与候选 `1.21.4-pre3` / `26.2-rc1`（新老两套命名都长这样）。
 * ③ 空格式的预览版 `1.14 Pre-Release 3`、`1.14.2 Pre-Release 4` —— 老 Mojang 的写法：
 * 分隔符是**空格**、词也写全了，也必须认。
 * ④ 老式周号 `24w14a`：认得出是快照，但读不出属于哪个大版本（`stem` 留空 = 折不了）。
 *
 * 尾巴必须是**末尾那一段**（`1.21.4-pre3`，不是 `1.21.4-pre3-1`），前缀必须是纯数字加点
 * （见 isPlainVersionStem）。这两条合起来挡掉的就是"把看不懂的号悄悄折成别的号"。
 */
SnapshotShape snapshotShapeOf(const QString &version) {
    SnapshotShape shape;
    const QString text = version.trimmed();
    if (text.isEmpty()) {
        return shape;
    }
    const QString lower = text.toLower();
    if (isWeeklySnapshotId(lower)) {
        shape.snapshot = true;
        return shape;
    }
    // **长关键词排前面**：`pre-release` 要是排在 `pre` 后面，`1.14 pre-release 3` 会先被
    // `pre` 那一支认下来，尾巴变成 `-release 3`（不是纯数字）⇒ 整条认不出来。
    static const QStringList keywords{QStringLiteral("snapshot"), QStringLiteral("pre-release"),
                                      QStringLiteral("pre"), QStringLiteral("rc")};
    for (const QString &keyword : keywords) {
        // 从**最后一次出现**取：`26.2-snapshot-1` 里 `snapshot` 只有一处，这个写法只是
        // 免得将来有个 `snapshot` 出现在大版本号里（不存在，但取最后一次总是对的）。
        const int at = lower.lastIndexOf(keyword);
        if (at <= 0) {
            continue;  // 没有这个词，或者它就在开头（前面没有"大版本"那一段）
        }
        const QString head = lower.left(at);
        const QChar separator = head.at(head.size() - 1);
        if (separator != QLatin1Char('-') && separator != QLatin1Char(' ')) {
            continue;  // `1.21.4pre3` 这种连在一块儿的不认（真没这么写的）
        }
        QString suffix = lower.mid(at + keyword.size());
        if (suffix.startsWith(QLatin1Char('-')) || suffix.startsWith(QLatin1Char(' '))) {
            suffix = suffix.mid(1);  // `-snapshot-1` / ` Pre-Release 3` 的第二个分隔符
        }
        if (suffix.isEmpty()) {
            continue;
        }
        bool allDigits = true;
        for (const QChar c : suffix) {
            allDigits = allDigits && c.isDigit();
        }
        if (!allDigits) {
            continue;
        }
        const QString stem = head.left(head.size() - 1);
        if (!isPlainVersionStem(stem)) {
            continue;  // 折出来不是个版本号（`1.RV-Pre1`）⇒ 当它不是快照
        }
        shape.snapshot = true;
        shape.stem = stem;
        return shape;
    }
    return shape;
}

}  // namespace

bool isSnapshotGameVersion(const QString &version) {
    return snapshotShapeOf(version).snapshot;
}

QString majorGameVersionOf(const QString &version) {
    const SnapshotShape shape = snapshotShapeOf(version);
    if (!shape.snapshot || shape.stem.isEmpty()) {
        // 不是快照：原样还回去（调用方因此不必先判一次"这是不是快照"）。
        // 是快照但折不出来（`24w14a`，前面那个版本号谁都猜不出来）：也原样还回去 ——
        // 调用方看它"还是快照"就知道这一条折不了（见 gameVersionChoices 里那段）。
        return version;
    }
    return shape.stem;
}

QString gameVersionSeriesOf(const QString &version) {
    const QString stem = majorGameVersionOf(version.trimmed());
    // 折完不是纯数字加点（周快照、b1.7.3、1.RV 这类）= 没有大版本可归。
    if (!isPlainVersionStem(stem)) {
        return QString();
    }
    return stem.section(QLatin1Char('.'), 0, 1);
}

QString otherSeriesKey() { return QStringLiteral("other"); }

QStringList gameVersionSeriesChoices(const QStringList &declaredGameVersions, bool showSnapshots) {
    QStringList out;
    bool hasOther = false;
    for (const QString &version : gameVersionChoices(declaredGameVersions, showSnapshots)) {
        const QString series = gameVersionSeriesOf(version);
        if (series.isEmpty()) {
            hasOther = true;
        } else if (!out.contains(series)) {
            out.append(series);
        }
    }
    if (hasOther) {
        out.append(otherSeriesKey());  // 「其他」永远在最后
    }
    return out;
}

QStringList gameVersionChoices(const QStringList &declaredGameVersions, bool showSnapshots) {
    QStringList out;
    out.reserve(declaredGameVersions.size());
    // 倒着抄一遍（接口给的是旧的在前）。**不是排序** —— 见头文件那段，这两种顺序都是
    // 接口自己定的，我们只是把要显示的那个方向挑出来。
    for (int i = declaredGameVersions.size() - 1; i >= 0; --i) {
        const QString declared = declaredGameVersions.at(i);
        if (showSnapshots) {
            out.append(declared);
            continue;
        }
        // 不显示快照版：折到大版本上。
        const QString folded = majorGameVersionOf(declared);
        // 折完**还是快照** = 这一条折不动（`24w14a` 这种读不出属于哪个大版本）⇒ 不摆它。
        // 勾选框叫「显示快照版」，不勾的时候就不该有快照版露在外面（它们照样在「全部」里
        // 看得到）。**判据就是这个**（Z8h 钉着）：写成 `folded != declared && …` 是错的 ——
        // 折不动的那一条恰恰是"折完跟没折一样"，那个写法会把它原样摆出来。
        if (isSnapshotGameVersion(folded)) {
            continue;
        }
        // 同一个大版本下声明过几个快照（26.2-rc1、26.2-rc2）时只留一颗。**去重在这里做**：
        // 上面是倒着走的（新的在前），所以留下的是先遇到的那一颗，而几颗折出来的字是同一个。
        if (!out.contains(folded)) {
            out.append(folded);
        }
    }
    return out;
}

QString gameVersionAfterToggle(const QStringList &declaredGameVersions, bool showSnapshots,
                               const QString &current) {
    // 「全部」永远是「全部」：两颗表里都有它，没有什么可换算的。
    if (current.isEmpty()) {
        return QString();
    }
    const QStringList choices = gameVersionChoices(declaredGameVersions, showSnapshots);
    // ① 这一颗在另一档里还在（正式版两边都有）—— 绝大多数情况到这儿就回去了。
    if (choices.contains(current)) {
        return current;
    }
    if (!showSnapshots) {
        // ② 勾 → 不勾：把现在这一颗折一下再找（26.2-rc1 → 26.2）。
        const QString folded = majorGameVersionOf(current);
        return choices.contains(folded) ? folded : QString();
    }
    // ③ 不勾 → 勾：这条线上**没有正式版**（只声明过 rc / pre）。挑最新的那一条快照，
    // 让用户留在同一条线上。声明表是**旧的在前**，所以从尾巴往前走，遇到的第一条
    // 就是最新的那一条。
    for (int i = declaredGameVersions.size() - 1; i >= 0; --i) {
        const QString declared = declaredGameVersions.at(i);
        if (isSnapshotGameVersion(declared) && majorGameVersionOf(declared) == current) {
            return declared;
        }
    }
    // 都对不上（调用方给了一个表里根本没有的 key）：回「全部」。这儿**不猜** —— 和
    // applyChipChecked 里那条"挑不中就按全部"是同一条规矩。
    return QString();
}

QString iconUrlForProject(const QString &projectId) {
    if (projectId.isEmpty()) {
        return QString();
    }
    // 为什么要有它：接口给的那个 icon_url 常常是一条小 7 倍的 webp，而**webp 不是每台
    // 机器都解得动**（见 ImageDecode::webpAvailable）。解不动、或者接口干脆没给（
    // hit.iconUrl 为空）、或者给了条别的域名的，都退到这一条上来。
    //
    // 实测更正：这条 `<id>/icon.png` **只是老项目的别名**，不是每个项目
    // 都有的那一条。用户真实 mods/ 里 20 个模组量下来 11 个回 404（FerriteCore /
    // Fabric Language Kotlin / Carpet / Fabric Permissions API / ArmorQuickSwap …），
    // 只有 9 个回 200（恰好是"图标当年就叫 icon.png"的那几个：Sodium / Fabric API /
    // Jade / cloth-config …）。修完在另一台盘上又量了一遍（`mw_check icon-lookup-live`
    // 打一个真实例的 mods/，51 个模组）：**33 个回 404、18 个回 200** —— 比例和上一批
    // 差不多，所以这不是"某一个实例特殊"。所以它**只配当最后一档**，谁要是拿它当
    // "自己拼得出来"的正常地址，那些项目就永远是那张占位拼图（那一发 404 被当成
    // "没传过图标"，屏幕上一个异常都没有）。真正那条只有问接口：projectsRequestUrl()。
    return cdnRoot() + QStringLiteral("/data/") + enc(projectId) + QStringLiteral("/icon.png");
}

QString projectsRequestUrl(const QStringList &projectIds, const QString &apiBase) {
    if (projectIds.isEmpty()) {
        return QString();
    }
    // ids 那一段照旧走 jsonArrayParam（百分号编码 + 用 QJsonDocument 拼，见那一处）：
    // 项目号都是 base62 / 纯数字，本来就没有要转义的字符，但**形状**必须是接口认的那个。
    return (apiBase.isEmpty() ? apiRoot() : apiBase) + QStringLiteral("/projects?ids=") +
           jsonArrayParam(projectIds);
}

QList<Hit> parseProjects(const QByteArray &json) {
    QList<Hit> hits;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    // 回复是一个**数组**（`GET /projects?ids=…` 是这个形状），不是搜索那种 `{hits:[…]}`。
    // 按对象读会一条都读不出来 —— 而"没有图标"和"读不懂"在屏幕上是一个样子。
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return hits;
    }
    for (const QJsonValue &value : document.array()) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            // 没有项目号就没有身份 —— 这一条进了界面也找不回任何一行（见 .hpp 那条 NOTE）。
            continue;
        }
        Hit hit;
        hit.source = Source::Modrinth;
        hit.projectId = id;
        hit.slug = object.value(QStringLiteral("slug")).toString();
        hit.title = object.value(QStringLiteral("title")).toString();
        hit.description = object.value(QStringLiteral("description")).toString();
        // 下载量可能大过 int（9 位数的有的是），所以走 double 再截断 —— 和 parseSearch 一样。
        hit.downloads = static_cast<qint64>(object.value(QStringLiteral("downloads")).toDouble());
        // 更新时间这一栏在这一份响应里叫 `updated`（搜索那份叫 `date_modified`，
        // CurseForge 那边叫 `dateModified`）—— 三个名字，读到**同一栏**里去。
        hit.updatedIso = object.value(QStringLiteral("updated")).toString();
        hit.categories = stringList(object.value(QStringLiteral("categories")));
        hit.gameVersions = stringList(object.value(QStringLiteral("game_versions")));
        // 加载器：这一份自己有一栏（`loaders`），不像搜索那边要按 categories 现推
        // （见 Hit::explicitLoaders 那段 —— 非空就只认它）。
        hit.explicitLoaders = stringList(object.value(QStringLiteral("loaders")));
        // 图标地址是外部输入，过一遍域名闸（见 isUsableIconUrl）；过不去就当这个项目
        // 没给图标地址 —— 和搜索那边读 icon_url 是同一个规矩。
        const QString icon = object.value(QStringLiteral("icon_url")).toString();
        hit.iconUrl = isUsableIconUrl(icon) ? icon : QString();
        hits.append(hit);
    }
    return hits;
}

QHash<QString, QString> parseProjectIconUrls(const QByteArray &json) {
    // 同一份响应（`GET /projects?ids=…`）只读第一格。**空着也插进去**（"这个项目没给
    // 图标地址"和"回复里根本没有这个项目"在调用方那边是两件事，见 .hpp 那条 NOTE）。
    QHash<QString, QString> icons;
    for (const Hit &hit : parseProjects(json)) {
        icons.insert(hit.projectId, hit.iconUrl);
    }
    return icons;
}

QList<Hit> parseCurseForgeMods(const QByteArray &json) {
    // 那一家的"按 id 问"和"搜"回的是**同一个对象形状**（`data[]`），所以不抄第二份解析
    // （见 .hpp 那条 NOTE）：`parseCurseForgeSearch()` 里那些字段名、空数组兜底、图标那两条
    // 地址的域名闸，全都在它那儿。
    return parseCurseForgeSearch(json).hits;
}

bool isWebpUrl(const QString &url) {
    if (url.isEmpty()) {
        return false;
    }
    // 走 QUrl::path() 而不是"整串 endsWith"：地址上可能挂着查询串 / 片段，那样
    // `…_96.webp?x=1` 会被判成不是 webp。取到 path 之后再比才算"看后缀"。
    return QUrl(url).path().endsWith(QStringLiteral(".webp"), Qt::CaseInsensitive);
}

bool isUsableIconUrl(const QString &url, Source source) {
    if (url.isEmpty()) {
        return false;
    }
    const QString expected = source == Source::CurseForge ? QStringLiteral("media.forgecdn.net")
                                                          : QStringLiteral("cdn.modrinth.com");
    const QUrl parsed(url);
    // 三件事都过才放行：协议是 https（明文 http 的图不要）、host **全等**（不是 endsWith
    // —— 见头文件里 `cdn.modrinth.com.别的.com` 那段）、以及解析本身没出岔子
    // （QUrl 对读不懂的串会给一个 host 为空的东西，那也过不了）。
    return parsed.isValid() && parsed.scheme() == QStringLiteral("https") && parsed.host() == expected;
}

QString formatDownloads(qint64 downloads) {
    if (downloads < 0) {
        return QString();
    }
    // 单位只在这里换一次。界面上列表那一行、详情、以及将来别处显示的下载量都读它，
    // 否则"1234567 次"和"123 万次"会在同一个界面上各说各的。
    if (downloads >= 100000000) {
        return QCoreApplication::translate("ResourceCatalog", "%1 亿").arg(downloads / 100000000.0, 0, 'f', 1);
    }
    if (downloads >= 10000) {
        // NOTE: 9999.95 万 ~ 1 亿 那一小段（不到一千万分之一的值）四舍五入后会显示成
        // 「10000.0 万」。数字是对的，只是单位没跟着进位 —— 换个单位要再切一刀，
        // 为一个这么窄的窗口不值当，先留着。
        return QCoreApplication::translate("ResourceCatalog", "%1 万").arg(downloads / 10000.0, 0, 'f', 1);
    }
    return QString::number(downloads);
}

// ---------- 资源详情页要用的那几件 ----------

/// 加载器在界面上排列的固定次序。**和 loaderOptions() 那份表同一个次序**
/// （fabric / forge / neoforge / quilt）—— 两处不一样的话，同一组版本在不同地方会换个先后。
/// 认不出来的一律排在这些后面。
int loaderOrderIndex(const QString &loader) {
    static const QStringList order = {QStringLiteral("fabric"), QStringLiteral("forge"),
                                      QStringLiteral("neoforge"), QStringLiteral("quilt")};
    const int index = order.indexOf(loader.toLower());
    return index < 0 ? order.size() : index;
}

/// 一条版本声明的**最新**那个游戏版本（组头里要用的那个）。一个都没有回空串。
///
/// 比法走 compareVersionText（`1.9` < `1.10` 那一套，也是"旧的在前"那个方向），
/// 取最大的那个。**不按接口给的顺序取第一个**：CF 那条 `gameVersions` 的顺序是随机的
/// （见文件里那段），照抄第一个会把一条 1.21 的版本分到 1.7.10 那一组去。
QString newestGameVersionOf(const VersionEntry &entry) {
    QString best;
    for (const QString &candidate : entry.gameVersions) {
        if (best.isEmpty() || compareVersionText(best, candidate) < 0) {
            best = candidate;
        }
    }
    return best;
}

/// 一条版本的**主加载器**（组头里那一个）：按固定次序取第一个认得的。
/// 一个都不认得（`optifine` 这种、或者空）就取它自己第一个原样小写，没有就回空串
/// （= 通用）。
QString primaryLoaderOf(const VersionEntry &entry) {
    for (const QString &known : {QStringLiteral("fabric"), QStringLiteral("forge"),
                                 QStringLiteral("neoforge"), QStringLiteral("quilt")}) {
        for (const QString &declared : entry.loaders) {
            if (declared.toLower() == known) {
                return known;
            }
        }
    }
    return entry.loaders.isEmpty() ? QString() : entry.loaders.first().toLower();
}

QString relativeTimeText(const QString &isoTime) {
    return relativeTimeText(isoTime, QDateTime::currentDateTime());
}

QString relativeTimeText(const QString &isoTime, const QDateTime &now) {
    if (isoTime.isEmpty()) {
        return QString();
    }
    // 两条格式都试：接口给的那一串**带毫秒**（`T12:34:56.789Z`），而 ISO 那一档
    // 读毫秒是分版本的（Qt 6 的 Qt::ISODate 认得，早先的版本要 Qt::ISODateWithMs）。
    // 两条都不认就把这一串当"读不懂"—— 那是空串，界面上不显示这一截。
    QDateTime then = QDateTime::fromString(isoTime, Qt::ISODate);
    if (!then.isValid()) {
        then = QDateTime::fromString(isoTime, Qt::ISODateWithMs);
    }
    if (!then.isValid()) {
        return QString();
    }
    const qint64 seconds = then.secsTo(now);
    if (seconds < 0) {
        // 将来时间：这一台机器的钟或者接口的时间戳有一个是错的。**空串**，不说「-3 天前」。
        return QString();
    }
    if (seconds < 60) {
        return QCoreApplication::translate("ResourceCatalog", "刚刚");
    }
    if (seconds < 3600) {
        return QCoreApplication::translate("ResourceCatalog", "%1 分钟前").arg(seconds / 60);
    }
    if (seconds < 86400) {
        return QCoreApplication::translate("ResourceCatalog", "%1 小时前").arg(seconds / 3600);
    }
    const qint64 days = seconds / 86400;
    if (days < 30) {
        return QCoreApplication::translate("ResourceCatalog", "%1 天前").arg(days);
    }
    if (days < 365) {
        return QCoreApplication::translate("ResourceCatalog", "%1 个月前").arg(days / 30);
    }
    return QCoreApplication::translate("ResourceCatalog", "%1 年前").arg(days / 365);
}

QString releaseChannelNote(const QString &versionType) {
    if (versionType == QStringLiteral("beta")) {
        return QCoreApplication::translate("ResourceCatalog", "测试版");
    }
    if (versionType == QStringLiteral("alpha")) {
        return QCoreApplication::translate("ResourceCatalog", "早期测试版");
    }
    // release 和"认不出来"都回空串：正式版是绝大多数，那三个字不是信息；认不出来的
    // 更不该编一个（见头文件）。
    return QString();
}

int releaseChannelRank(const QString &versionType) {
    if (versionType == QStringLiteral("release")) {
        return 0;
    }
    if (versionType == QStringLiteral("beta")) {
        return 1;
    }
    if (versionType == QStringLiteral("alpha")) {
        return 2;
    }
    // 认不出来回 -1，**不是** 0：把一条不知道是什么渠道的版本画成绿色的"正式版"，是把
    // "不知道"说成了"很稳"（详情页那个方块认不出这一档就不画字母，见头文件）。
    return -1;
}

QString loaderDisplayName(const QString &loader) {
    const QString lowered = loader.toLower();
    if (lowered == QStringLiteral("fabric")) {
        return QStringLiteral("Fabric");
    }
    if (lowered == QStringLiteral("forge")) {
        return QStringLiteral("Forge");
    }
    if (lowered == QStringLiteral("neoforge")) {
        return QStringLiteral("NeoForge");
    }
    if (lowered == QStringLiteral("quilt")) {
        return QStringLiteral("Quilt");
    }
    return loader;  // 认不出来原样还回去：显示 optifine 比显示空串强
}

QString versionRowSubText(const VersionEntry &version) {
    QStringList bits;
    // 加载器。一条版本可能同时支持好几个（Modrinth 那边常见 fabric + quilt），都列出来 ——
    // 组头只放得下主加载器一个（见 primaryLoaderOf），其余在这儿。
    QStringList loaders;
    for (const QString &loader : version.loaders) {
        loaders.append(loaderDisplayName(loader));
    }
    if (!loaders.isEmpty()) {
        bits.append(loaders.join(QStringLiteral(" / ")));
    }
    // 文件名：落盘的就是它，用户拿着它能在网页上认出同一个文件。
    const FileEntry *file = version.primaryFile();
    if (file != nullptr && !file->filename.isEmpty()) {
        bits.append(file->filename);
    }
    // 下载量：0 = 接口没给这一栏（见 VersionEntry::downloads），那一截省掉。
    if (version.downloads > 0) {
        bits.append(QCoreApplication::translate("ResourceCatalog", "下载 %1次").arg(formatDownloads(version.downloads)));
    }
    const QString updated = relativeTimeText(version.datePublished);
    if (!updated.isEmpty()) {
        bits.append(QCoreApplication::translate("ResourceCatalog", "更新于 %1").arg(updated));
    }
    const QString channel = releaseChannelNote(version.versionType);
    if (!channel.isEmpty()) {
        bits.append(channel);
    }
    return bits.join(QStringLiteral(", "));
}

QList<VersionEntry> versionsForGameVersion(const QList<VersionEntry> &versions,
                                           const QString &gameVersion, bool showSnapshots) {
    // 空串 = 不筛（「全部」那一颗）。**原样还回去**，不是"筛一个空串"（那会一条都不剩）。
    if (gameVersion.isEmpty()) {
        return versions;
    }
    QList<VersionEntry> out;
    for (const VersionEntry &version : versions) {
        // 看的是 `gameVersions`（这一版**声明过**的每一个游戏版本），**不是**它落在哪个
        // 组里（`groupVersions` 那个归属）。一条同时支持 1.20.1 / 1.20.2 的版本落在
        // 1.20.2 那一组，但选了 1.20.1 那一格时它照样该在 —— 这两件事在详情页里是分开
        // 的两步（先按 chip 筛、再分组），别合并成一步。
        if (version.gameVersions.contains(gameVersion)) {
            out.append(version);
            continue;
        }
        // 不勾「显示快照版」时，这一排上的 `26.2` 代表的是"26.2 那条线"，声明的快照也要算
        // 进来（勾上时 `26.2` 就只是 `26.2`，快照各去点自己那一颗 —— 见头文件那段）。
        // 折出来还是快照的那些（24w14a）在这一支里自然落空：它们没被折过。
        if (!showSnapshots) {
            for (const QString &declared : version.gameVersions) {
                if (majorGameVersionOf(declared) == gameVersion) {
                    out.append(version);
                    break;
                }
            }
        }
    }
    return out;
}

QList<VersionEntry> versionsForSeries(const QList<VersionEntry> &versions, const QString &series,
                                      bool showSnapshots) {
    if (series.isEmpty()) {
        return versions;
    }
    QList<VersionEntry> out;
    for (const VersionEntry &version : versions) {
        for (const QString &declared : version.gameVersions) {
            // 不勾时折不出大版本的周快照不算（它们不在顶排的任何一组里）。
            if (!showSnapshots && isSnapshotGameVersion(majorGameVersionOf(declared))) {
                continue;
            }
            const QString own = gameVersionSeriesOf(declared);
            if ((own.isEmpty() ? otherSeriesKey() : own) == series) {
                out.append(version);
                break;
            }
        }
    }
    return out;
}

QString hitMetaLine(const Hit &hit) {
    QStringList bits;
    // 加载器：这一条**支持哪几个**（不是某一个版本的）。和版本那一行同一个显示名函数 ——
    // `fabric` 在界面上永远是 `Fabric`。
    QStringList loaders;
    const QStringList raw = hit.loaders();
    for (const QString &loader : raw) {
        loaders.append(loaderDisplayName(loader));
    }
    if (!loaders.isEmpty()) {
        bits.append(loaders.join(QStringLiteral(" / ")));
    }
    // 下载量：0 = 接口没给这一栏（见 Hit::downloads），那一截省掉 —— 不写"下载 0 次"。
    if (hit.downloads > 0) {
        bits.append(QCoreApplication::translate("ResourceCatalog", "下载 %1次").arg(formatDownloads(hit.downloads)));
    }
    const QString updated = relativeTimeText(hit.updatedIso);
    if (!updated.isEmpty()) {
        bits.append(QCoreApplication::translate("ResourceCatalog", "更新于 %1").arg(updated));
    }
    // 来源总在：这一行是"这一条是从哪一家来的"，而图标和版本列表都跟着它走。
    bits.append(sourceLabel(hit.source));
    return bits.join(QStringLiteral(" / "));
}

QString VersionGroup::title() const {
    if (loader.isEmpty() && gameVersion.isEmpty()) {
        return QCoreApplication::translate("ResourceCatalog", "其它版本");
    }
    if (loader.isEmpty()) {
        return gameVersion;
    }
    const QString name = loaderDisplayName(loader);
    return gameVersion.isEmpty() ? name : name + QLatin1Char(' ') + gameVersion;
}

QList<VersionGroup> groupVersions(const QList<VersionEntry> &versions, bool showSnapshots,
                                  const QString &preferredLoader) {
    QList<VersionGroup> groups;
    for (const VersionEntry &version : versions) {
        // 不勾「显示快照版」时组头也归到大版本（一条只声明到 26.2-rc1 的版本落在
        // 「Fabric 26.2」那一组）—— 和那一排 chip 折的是同一个东西，见头文件那段。
        // **先取最新再折**：折完再比大小的话，"最新"就变成按折过的那串排的了。
        const QString declared = newestGameVersionOf(version);
        const QString gameVersion = showSnapshots ? declared : majorGameVersionOf(declared);
        const QString loader = primaryLoaderOf(version);
        int found = -1;
        for (int i = 0; i < groups.size(); ++i) {
            if (groups.at(i).gameVersion == gameVersion && groups.at(i).loader == loader) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            VersionGroup group;
            group.gameVersion = gameVersion;
            group.loader = loader;
            groups.append(group);
            found = groups.size() - 1;
        }
        groups[found].versions.append(version);
    }

    // 组的次序：游戏版本**新的在前**（compareVersionText 是"旧的在前"那把尺子，反着用），
    // 同一个游戏版本里按加载器的固定次序，空游戏版本（信息就是缺的）和最不认识的加载器
    // 都排到最后。stable_sort：两条完全同序的组保持它们进列表的先后。
    //
    // `preferredLoader`：同一个游戏版本里**点名的那一个加载器排最前**。
    // 放在这里、而不是在调用方排一遍：组序只有这一处出处（见 .hpp 那段)，劈成两处
    // 之后"两组各说各的"迟早错开。次序的**第一关键字仍然是游戏版本**——见 .hpp 那条
    // NOTE，不然筛了 1.21.1 之后一个老 Fabric 组会跑到 26.2 的 NeoForge 组前面。
    const QString preferred = preferredLoader.trimmed().toLower();
    const auto rankOf = [&preferred](const QString &loader) {
        // 点名的那个排在最前（-1 比谁都小），其余照旧走那把固定的尺子。
        if (!preferred.isEmpty() && loader == preferred) {
            return -1;
        }
        return loaderOrderIndex(loader);
    };
    // 排序前先把每个组要用的那两样**预算一遍**（decorate-sort-undecorate，）：
    // 一次排序是 N log N 次比较，现拆的话每次比较都要 split 两遍版本号（`1.20.5-pre1`
    // 那种还要逐段过一遍）—— 而这一趟**每按一颗 chip 都会重跑一遍**。判据一个字没改：
    // 还是 compareVersionParts() + loaderOrderIndex() 那一套，只是值提前算好了。
    struct Row {
        VersionTextParts parts;   ///< gameVersion 拆好的那一份
        int loaderRank = 0;       ///< rankOf(loader) 的预算结果
        VersionGroup group;
    };
    QList<Row> rows;
    rows.reserve(groups.size());
    for (const VersionGroup &group : groups) {
        rows.append(Row{splitVersionText(group.gameVersion), rankOf(group.loader), group});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.group.gameVersion != b.group.gameVersion) {
            if (a.group.gameVersion.isEmpty()) {
                return false;
            }
            if (b.group.gameVersion.isEmpty()) {
                return true;
            }
            return compareVersionParts(a.parts, b.parts) > 0;
        }
        return a.loaderRank < b.loaderRank;
    });
    groups.clear();
    groups.reserve(rows.size());
    for (const Row &row : rows) {
        groups.append(row.group);
    }
    return groups;
}

QString projectPageUrl(Source source, Kind kind, const QString &slug) {
    if (slug.isEmpty()) {
        return QString();
    }
    if (source == Source::CurseForge) {
        // 类名那一段是**那一家自己网页里的目录名**，不是接口的类目号、也不是我们那份
        // kindKey —— 三套东西，别互相套用。
        QString section;
        switch (kind) {
        case Kind::Mod:
            section = QStringLiteral("mc-mods");
            break;
        case Kind::Modpack:
            section = QStringLiteral("modpacks");
            break;
        case Kind::ResourcePack:
            section = QStringLiteral("texture-packs");
            break;
        case Kind::Shader:
            section = QStringLiteral("shaders");
            break;
        case Kind::DataPack:
            section = QStringLiteral("data-packs");
            break;
        }
        return QStringLiteral("https://www.curseforge.com/minecraft/") + section +
               QLatin1Char('/') + slug;
    }
    QString section;
    switch (kind) {
    case Kind::Mod:
        section = QStringLiteral("mod");
        break;
    case Kind::Modpack:
        section = QStringLiteral("modpack");
        break;
    case Kind::ResourcePack:
        section = QStringLiteral("resourcepack");
        break;
    case Kind::Shader:
        section = QStringLiteral("shader");
        break;
    case Kind::DataPack:
        section = QStringLiteral("datapack");
        break;
    }
    return QStringLiteral("https://modrinth.com/") + section + QLatin1Char('/') + slug;
}

QString mcmodSearchUrl(const QString &name) {
    if (name.isEmpty()) {
        return QString();
    }
    // 那一家没有"按对面的 id 直接跳"的路：它的项目号是另一套，和对面的 id 对不上。
    // 所以这一条是**拿名字去搜**（跳到结果页，不一定是那一页）—— 按钮上的话也照这个说。
    // 分隔符和参数名抄的是它自己搜索框提交的那一条。
    return QStringLiteral("https://www.mcmod.cn/s?key=") +
           QString::fromLatin1(QUrl::toPercentEncoding(name));
}

// ---------- 版本 ----------

const FileEntry *VersionEntry::primaryFile() const {
    for (const FileEntry &file : files) {
        if (file.primary) {
            return &file;
        }
    }
    return files.isEmpty() ? nullptr : &files.first();
}

QString versionsUrl(const QString &projectId, const QString &gameVersion,
                    const QString &loader, Source source, const QString &apiBase) {
    // 加载器**先按来源翻一遍**（`loaderRequestValue()`，唯一出处）：两边要的词形不是一套
    // （Modrinth 是小写词、CurseForge 是数字），而递一个对不上的上去**不报错、只是筛不出
    // 东西**。 之前这一层收的是**显示名**（`instanceLoaderName()` 给的「Fabric」），
    // 于是前置检查那一路两边都没筛过 —— 落到实上就是"给 Fabric 实例挑了一个 Forge 的前置"。
    const QString loaderParam = loaderRequestValue(loader, source);
    if (source == Source::CurseForge) {
        // `GET {base}/mods/{id}/files?gameVersion=&modLoaderType=`。名字和搜索那边**一样**
        // 是谁的就是谁的：那一家同一个概念在两处用同一个参数名（和 Modrinth 正好相反，
        // 那边搜索叫 categories、版本叫 loaders）。
        QStringList params;
        if (!gameVersion.isEmpty()) {
            params.append(QStringLiteral("gameVersion=") + enc(gameVersion));
        }
        if (!loaderParam.isEmpty()) {
            // 这里要的**就是数字**（`loaderRequestValue()` 已经翻好了）—— 传一个 `fabric`
            // 上去不会报错，只是筛不出东西。
            params.append(QStringLiteral("modLoaderType=") + enc(loaderParam));
        }
        QString url = curseForgeApiRoot(apiBase) + QStringLiteral("/mods/") + enc(projectId) +
                      QStringLiteral("/files");
        if (!params.isEmpty()) {
            url += QStringLiteral("?") + params.join(QLatin1Char('&'));
        }
        return url;
    }

    QStringList params{QStringLiteral("include_changelog=false")};
    if (!gameVersion.isEmpty()) {
        params.append(QStringLiteral("game_versions=") + jsonArrayParam({gameVersion}));
    }
    if (!loaderParam.isEmpty()) {
        // 这一条**确实**是 `loaders`（和搜索那边不一样，见 searchUrl 里的注），值是小写那个词。
        params.append(QStringLiteral("loaders=") + jsonArrayParam({loaderParam}));
    }

    // 根地址和 searchUrl 那一条逐字同一个理由（见那儿的 ★）：`apiBase` 两个来源都认。
    QString url = (apiBase.isEmpty() ? apiRoot() : apiBase) + QStringLiteral("/project/") +
                  enc(projectId) + QStringLiteral("/version");
    if (!params.isEmpty()) {
        url += QStringLiteral("?") + params.join(QLatin1Char('&'));
    }
    return url;
}

QString versionFilesUpdateUrl(const QString &apiBase) {
    // 根地址和「批量问项目」那一条（`projectsRequestUrl()`）逐字同一个规矩：验证工程那个
    // 口子优先，否则官方那一份（唯一出处仍是 `apiRoot()`）。
    return (apiBase.isEmpty() ? apiRoot() : apiBase) +
           QStringLiteral("/version_files/update");
}

QString searchRequestKey(Kind kind, Source source) {
    // 前缀是为了和下面那把一眼分得开（两把钥匙都会进同一张表）。来源必带：用户在一个页面上
    // 从 Modrinth 切成 CurseForge 时，前一趟回来的结果**不是**"我现在等的那一发"。
    return QStringLiteral("search|") + sourceKey(source) + QLatin1Char('|') + kindKey(kind);
}

QString versionsRequestKey(const QString &projectId, const QString &gameVersion,
                           const QString &loader, Source source) {
    // `|` 在 project_id（base62 或纯数字）、游戏版本号和加载器名里都不会出现，拿它当分隔符
    // 不用转义。来源也带上了：两边 id 空间是分开的，"238222" 在两边是两个不同的项目。
    return QStringLiteral("versions|") + sourceKey(source) + QLatin1Char('|') + projectId +
           QLatin1Char('|') + gameVersion + QLatin1Char('|') + loader;
}

QString projectsRequestKey(const QStringList &projectIds, Source source) {
    // id 先排一遍再拼：同一批号不管调用方是按什么顺序攒起来的，都是**同一把钥匙** ——
    // 不排的话，同一批号换一个顺序就成了"另一发"，前一趟回来的结果会被当成过期的丢掉，
    // 界面上的症状是"这一批名字永远填不上"。
    QStringList sorted = projectIds;
    sorted.sort();
    // `,` 在两种 id（base62 / 纯数字）里都不会出现，当分隔符不用转义。
    return QStringLiteral("projects|") + sourceKey(source) + QLatin1Char('|') +
           sorted.join(QLatin1Char(','));
}

/// `parseCurseForgeVersions()` 的本体：**已经解析好的那一份**直接收（定义在下面，
/// 声明提到这儿是因为 `parseVersions()` 也要用它 —— 形参是 QJsonDocument 而不是 QByteArray，
/// 就是为了一次回复只建一棵树）。
static QList<VersionEntry> parseCurseForgeDocument(const QJsonDocument &document);

/// 一个 Modrinth **版本对象** → 我们那一条 `VersionEntry`。
static VersionEntry entryFromModrinthObject(const QJsonObject &object) {
    VersionEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.versionNumber = object.value(QStringLiteral("version_number")).toString();
    entry.name = object.value(QStringLiteral("name")).toString();
    entry.versionType = object.value(QStringLiteral("version_type")).toString();
    entry.datePublished = object.value(QStringLiteral("date_published")).toString();
    // 这一版自己被下过多少次（详情页那行小字）。**读不到就是 0** —— 0 在界面上是
    // "这一截不显示"，不是"没人下过"（见 VersionEntry::downloads 那段）。
    entry.downloads = static_cast<qint64>(object.value(QStringLiteral("downloads")).toDouble());
    entry.gameVersions = stringList(object.value(QStringLiteral("game_versions")));
    entry.loaders = stringList(object.value(QStringLiteral("loaders")));

    const QJsonArray files = object.value(QStringLiteral("files")).toArray();
    entry.files.reserve(files.size());
    for (const QJsonValue &fileValue : files) {
        const QJsonObject fileObject = fileValue.toObject();
        FileEntry file;
        file.url = fileObject.value(QStringLiteral("url")).toString();
        file.filename = fileObject.value(QStringLiteral("filename")).toString();
        file.size = static_cast<qint64>(fileObject.value(QStringLiteral("size")).toDouble());
        file.sha1 = fileObject.value(QStringLiteral("hashes"))
                        .toObject()
                        .value(QStringLiteral("sha1"))
                        .toString();
        file.primary = fileObject.value(QStringLiteral("primary")).toBool();
        entry.files.append(file);
    }

    entry.dependencyMetadataKnown = object.value(QStringLiteral("dependencies")).isArray();
    const QJsonArray dependencies = object.value(QStringLiteral("dependencies")).toArray();
    entry.dependencies.reserve(dependencies.size());
    for (const QJsonValue &dependencyValue : dependencies) {
        const QJsonObject dependencyObject = dependencyValue.toObject();
        Dependency dependency;
        dependency.projectId = dependencyObject.value(QStringLiteral("project_id")).toString();
        dependency.versionId = dependencyObject.value(QStringLiteral("version_id")).toString();
        dependency.type = dependencyObject.value(QStringLiteral("dependency_type")).toString();
        entry.dependencies.append(dependency);
    }
    return entry;
}

QList<VersionEntry> parseVersions(const QByteArray &json) {
    QList<VersionEntry> out;

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        return out;
    }
    if (document.isObject()) {
        // 根是对象 = CurseForge 那一份（`{"data":[…]}`）。**按根分岔**而不是让调用方先判
        // 来源：调用方判的话，"哪个接口回哪种形状"这件事就抄到了调用点上。
        // 把**已经解析好的那一份**递下去：原来递的是原始 bytes，于是
        // 这一份几百到几千条 file 的回复被整棵树建了两遍。
        return parseCurseForgeDocument(document);
    }
    if (!document.isArray()) {
        return out;
    }

    const QJsonArray array = document.array();
    out.reserve(array.size());
    for (const QJsonValue &item : array) {
        out.append(entryFromModrinthObject(item.toObject()));
    }

    // 顺序**原样**返回：接口给的就是新的在前（两份真响应的 date_published 都严格递减，
    // 见头文件那段），界面直接从上往下画。这里刻意不排：写一个"1.9 比 1.10 旧"的
    // 比较器，错了不会有任何东西报警。
    return out;
}

QByteArray versionFilesUpdateRequestBody(const QStringList &sha1s, const QString &gameVersion,
                                         const QString &loader) {
    QJsonArray hashes;
    for (const QString &sha1 : sha1s) {
        const QString trimmed = sha1.trimmed();
        if (!trimmed.isEmpty()) {
            hashes.append(trimmed);
        }
    }
    if (hashes.isEmpty()) {
        return QByteArray();
    }
    QJsonObject body;
    body.insert(QStringLiteral("hashes"), hashes);
    body.insert(QStringLiteral("algorithm"), QStringLiteral("sha1"));
    // 那两个筛和 `versionsUrl()` 里**同一个规矩**：空 = 不筛（连这个键都不出现）。
    // 加载器**必须在这儿翻一道**（`loaderRequestValue()`，和 `versionsUrl()` 那一句
    // 逐字同一个出处）：调用方递进来的是 `dependencyTargetFor()` 给的那一个，也就是
    // `loaderGroupKey(instanceLoaderName(…))` —— **显示名**（「Fabric」）或者用户盘上
    // 那种别的词（「OptiFine」）。Modrinth 这一发只认**小写那个词**，递「Fabric」上去
    // 不会报错、只是**一个都不匹配** ⇒ 整批模组全被退回问 CurseForge（而屏幕上看着
    // 像"这一批没查到"）。翻不出来的词（OptiFine 这种）回空串 = 那一格**整个不发** ——
    // 这也是对的：宁可宽一点，也别拼一个筛不出来的词上去。
    // NOTE: 这一发只有 Modrinth 认这一份形状，所以它**没有 `Source` 参数**（CurseForge
    // 那边没有对应接口，调用方那边按来源分岔，见 `checkInstanceUpdates()`）；
    // 翻的那一步于是写死走 `Source::Modrinth` 这一套词。
    if (!gameVersion.isEmpty()) {
        body.insert(QStringLiteral("game_versions"),
                    QJsonArray::fromStringList(QStringList{gameVersion}));
    }
    const QString loaderParam = loaderRequestValue(loader, Source::Modrinth);
    if (!loaderParam.isEmpty()) {
        body.insert(QStringLiteral("loaders"),
                    QJsonArray::fromStringList(QStringList{loaderParam}));
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QHash<QString, VersionEntry> parseVersionFilesUpdate(const QByteArray &json) {
    QHash<QString, VersionEntry> out;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        return out;
    }
    const QJsonObject object = document.object();
    out.reserve(object.size());
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        // 键**按小写收**：本地那个 sha1 是 `hashFile()` 算的（小写），而接口回的是我们
        // 发上去的那一份（也是小写）—— 但"两边大小写一致"这件事没有谁保证，收的时候
        // 归一一次，读的人就不必再记得这件事。
        out.insert(it.key().toLower(), entryFromModrinthObject(it.value().toObject()));
    }
    return out;
}

QList<VersionEntry> parseCurseForgeVersions(const QByteArray &json) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        return QList<VersionEntry>();
    }
    return parseCurseForgeDocument(document);
}

static QList<VersionEntry> parseCurseForgeDocument(const QJsonDocument &document) {
    QList<VersionEntry> out;
    if (!document.isObject()) {
        return out;
    }
    const QJsonArray array = document.object().value(QStringLiteral("data")).toArray();
    out.reserve(array.size());
    for (const QJsonValue &item : array) {
        const QJsonObject object = item.toObject();
        VersionEntry entry;
        // 那边**一条就是一个文件**（不是"一个版本里带几个文件"）：每条有自己的 id、文件名
        // 和自己的 `gameVersions`。所以一条直接变成一个 VersionEntry、`files` 里只有它自己
        // （Modrinth 那边是"一个版本带文件数组"，形状不同但到界面上的形状是同一个）。
        const double rawFileId = object.value(QStringLiteral("id")).toDouble();
        const qint64 fileId = rawFileId > 0 && rawFileId <= 9007199254740991.0 &&
            rawFileId == static_cast<double>(static_cast<qint64>(rawFileId))
                ? static_cast<qint64>(rawFileId) : 0;
        entry.id = QString::number(fileId);
        const QString fileName = object.value(QStringLiteral("fileName")).toString();
        entry.versionNumber = object.value(QStringLiteral("displayName")).toString();
        entry.name = entry.versionNumber;
        entry.versionType =
            curseForgeReleaseType(object.value(QStringLiteral("releaseType")).toInt());
        entry.datePublished = object.value(QStringLiteral("fileDate")).toString();
        // 这一条文件被下过多少次（Modrinth 那边这一栏叫 downloads，这边叫 downloadCount）。
        entry.downloads =
            static_cast<qint64>(object.value(QStringLiteral("downloadCount")).toDouble());

        // `gameVersions` 那一串混着三样东西（游戏版本 / 哪一侧 / 哪个加载器），要挑开：
        // 游戏版本进 `gameVersions`，加载器进 `loaders`（翻成小写，加载器那道闸和小字都认
        // 小写），`Client` / `Server` 这一类**两边都不进**（界面上没有它们的位置）。
        const QStringList raw = stringList(object.value(QStringLiteral("gameVersions")));
        const QStringList &loaderWords = curseForgeLoaderWords();
        for (const QString &word : raw) {
            if (looksLikeGameVersion(word)) {
                entry.gameVersions.append(word);
                continue;
            }
            const QString lowered = word.toLower();
            if (loaderWords.contains(lowered)) {
                entry.loaders.append(lowered);
            }
        }

        FileEntry file;
        file.filename = fileName;
        file.size = static_cast<qint64>(object.value(QStringLiteral("fileLength")).toDouble());
        file.sha1 = curseForgeSha1(object);
        file.primary = true;  // 一条就是一个文件，它就是这一版的那个主文件
        const bool explicitlyUnavailable = object.value(QStringLiteral("isAvailable")).isBool() &&
            !object.value(QStringLiteral("isAvailable")).toBool();
        file.url = explicitlyUnavailable ? QString() : ModpackImport::curseForgeDownloadUrl(
            fileId, fileName, object.value(QStringLiteral("downloadUrl")).toString());
        if (file.url.isEmpty()) {
            file.unavailableNote = explicitlyUnavailable
                ? QCoreApplication::translate("ResourceCatalog", "文件不可用，请前往项目主页查看。")
                : QCoreApplication::translate("ResourceCatalog", "下载地址未公开，请在项目主页确认或手动补齐。");
        }
        entry.files.append(file);

        // 前置模组那一栏这一轮不用（`requiredDependenciesOf` 现在没有调用方），但**顺手读出来**
        // 比留着不读强：那边的 `relationType` 是个数字（3 = required、2 = optional……），
        // 和 Modrinth 的字符串不是一套，将来接的时候这里就是唯一需要翻译的地方。
        entry.dependencyMetadataKnown = object.value(QStringLiteral("dependencies")).isArray();
        const QJsonArray dependencies = object.value(QStringLiteral("dependencies")).toArray();
        for (const QJsonValue &dependency : dependencies) {
            const QJsonObject dependencyObject = dependency.toObject();
            Dependency entryDependency;
            const auto identity = [](const QJsonValue &value) {
                const double raw = value.toDouble(-1);
                return raw > 0 && raw <= 9007199254740991.0 && std::floor(raw) == raw
                    ? QString::number(static_cast<qint64>(raw)) : QString();
            };
            entryDependency.projectId = identity(dependencyObject.value(QStringLiteral("modId")));
            // Some API/SDK dependency responses additionally pin a file. Zero
            // means no exact pin; it must never be sent as a "latest" install.
            entryDependency.versionId = identity(dependencyObject.value(QStringLiteral("fileId")));
            // CurseForge FileRelationType: docs.curseforge.com/rest-api/
            switch (dependencyObject.value(QStringLiteral("relationType")).toInt()) {
            case 1: entryDependency.type = QStringLiteral("embedded"); break;
            case 2: entryDependency.type = QStringLiteral("optional"); break;
            case 3: entryDependency.type = QStringLiteral("required"); break;
            case 5: entryDependency.type = QStringLiteral("incompatible"); break;
            default: break; // Tools/unknown relations are not invented as dependencies.
            }
            entry.dependencies.append(entryDependency);
        }

        out.append(entry);
    }

    return out;
}

// ---------- 前置模组（这一轮只留接口）----------

QList<Dependency> requiredDependenciesOf(const QList<VersionEntry> &versions,
                                         const QString &versionId) {
    QList<Dependency> out;
    for (const VersionEntry &version : versions) {
        if (version.id != versionId) {
            continue;
        }
        for (const Dependency &dependency : version.dependencies) {
            if (dependency.required()) {
                out.append(dependency);
            }
        }
        break;
    }
    // NOTE: 光有 project_id 还下不了 —— 前置模组也得挑一个版本（`versionId` 常常是 null，
    // 意思是"哪个版本都行"）。那一步要问接口，得看目标实例是什么游戏版本/加载器，
    // 所以它不属于这个纯函数。用户要求这一轮**先不做**（要求「前置模组记得预留一键
    // 下载的接口，先不急」），所以这里就停在"要下谁"。
    return out;
}

// ---------- 一条 Hit 的盘上往返（见 .hpp 里那段）----------

namespace {

/// 盘上那几个键。**读和写各只出现一次**（就在下面这两个函数里）—— 两边各写一遍字符串
/// 的话，改了一处忘另一处就是"那一栏第二次启动之后没了"，而屏幕上一点异常都没有。
const char *kProjectIdKey = "projectId";
const char *kSlugKey = "slug";
const char *kTitleKey = "title";
const char *kDescriptionKey = "description";
const char *kAuthorKey = "author";
const char *kDownloadsKey = "downloads";
const char *kCategoriesKey = "categories";
const char *kGameVersionsKey = "gameVersions";
const char *kSourceKey = "source";
const char *kExplicitLoadersKey = "explicitLoaders";
const char *kIconUrlKey = "iconUrl";
const char *kIconFallbackUrlKey = "iconFallbackUrl";
const char *kUpdatedIsoKey = "updatedIso";

QJsonArray stringsToJson(const QStringList &values) {
    QJsonArray array;
    for (const QString &value : values) {
        array.append(value);
    }
    return array;
}

QStringList jsonToStrings(const QJsonValue &value) {
    QStringList out;
    const QJsonArray array = value.toArray();
    out.reserve(array.size());
    for (const QJsonValue &item : array) {
        out.append(item.toString());
    }
    return out;
}

}  // namespace

QJsonObject hitToJson(const Hit &hit) {
    QJsonObject object;
    object.insert(QLatin1String(kProjectIdKey), hit.projectId);
    object.insert(QLatin1String(kSlugKey), hit.slug);
    object.insert(QLatin1String(kTitleKey), hit.title);
    object.insert(QLatin1String(kDescriptionKey), hit.description);
    object.insert(QLatin1String(kAuthorKey), hit.author);
    // 下载量是 qint64，JSON 里只有 double —— 2^53 以内的整数原样过得去（下载量离那个数
    // 差着好几个数量级）。**别**把它写成字符串：读回来那一侧就要多一次转换。
    object.insert(QLatin1String(kDownloadsKey), static_cast<double>(hit.downloads));
    object.insert(QLatin1String(kCategoriesKey), stringsToJson(hit.categories));
    object.insert(QLatin1String(kGameVersionsKey), stringsToJson(hit.gameVersions));
    // 来源走 `sourceKey()`（`modrinth` / `curseforge`）—— 唯一出处，别在这儿写字面量。
    object.insert(QLatin1String(kSourceKey), sourceKey(hit.source));
    object.insert(QLatin1String(kExplicitLoadersKey), stringsToJson(hit.explicitLoaders));
    object.insert(QLatin1String(kIconUrlKey), hit.iconUrl);
    object.insert(QLatin1String(kIconFallbackUrlKey), hit.iconFallbackUrl);
    object.insert(QLatin1String(kUpdatedIsoKey), hit.updatedIso);
    return object;
}

Hit hitFromJson(const QJsonObject &object) {
    Hit hit;
    hit.projectId = object.value(QLatin1String(kProjectIdKey)).toString();
    hit.slug = object.value(QLatin1String(kSlugKey)).toString();
    hit.title = object.value(QLatin1String(kTitleKey)).toString();
    hit.description = object.value(QLatin1String(kDescriptionKey)).toString();
    hit.author = object.value(QLatin1String(kAuthorKey)).toString();
    hit.downloads = static_cast<qint64>(object.value(QLatin1String(kDownloadsKey)).toDouble());
    hit.categories = jsonToStrings(object.value(QLatin1String(kCategoriesKey)));
    hit.gameVersions = jsonToStrings(object.value(QLatin1String(kGameVersionsKey)));
    // 认不出来就退回 `Hit` 自己那个默认（Modrinth）—— 和 `sourceFromKey()` 那条规矩
    // 一致：读不动的一律当默认，而不是当错。
    Source source = Source::Modrinth;
    sourceFromKey(object.value(QLatin1String(kSourceKey)).toString(), &source);
    hit.source = source;
    hit.explicitLoaders = jsonToStrings(object.value(QLatin1String(kExplicitLoadersKey)));
    hit.iconUrl = object.value(QLatin1String(kIconUrlKey)).toString();
    hit.iconFallbackUrl = object.value(QLatin1String(kIconFallbackUrlKey)).toString();
    hit.updatedIso = object.value(QLatin1String(kUpdatedIsoKey)).toString();
    return hit;
}

} // namespace ResourceCatalog

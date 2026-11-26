/**
 * @file SchematicInfo.cpp
 * @brief 三种结构 / 原理图格式的元数据与方块统计。
 *
 * 逐格式的判据写在各自的 parseXxx() 头上。这一层里**没有任何界面代码**：进来的是一
 * 个路径，出去的是 `Info`；要变成屏幕上的那一行字，走 `summaryText()`。
 */

#include "SchematicInfo.hpp"
#include <QCoreApplication>
#include "TextMarks.hpp"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>

#include "util/NbtTree.hpp"
#include "util/ZipUtil.hpp"
#include "download/AssetNames.hpp"

namespace SchematicInfo {
namespace {

constexpr qint64 kMaxCompressedBytes = 96ll * 1024 * 1024;
/// 解压之后的上限。H↔B 压缩比在 NBT 上通常只有几倍，256 MB 已经非常宽。
constexpr qint64 kMaxUncompressedBytes = 256ll * 1024 * 1024;
/// 一个 region 最多解多少个位置（64M 次循环约一两秒；再大就当"只读到元数据"）。
constexpr qint64 kMaxDecodeVolume = 64ll * 1024 * 1024;

/// 整数族那几个标签都当整数读（Byte / Short / Int / Long）。
bool integerOf(const NbtTree::Node &node, qint64 *out) {
    switch (node.type) {
    case NbtTree::Type::Byte:
    case NbtTree::Type::Short:
    case NbtTree::Type::Int:
    case NbtTree::Type::Long:
        if (out != nullptr) {
            *out = node.number;
        }
        return true;
    default:
        return false;
    }
}

/// 一个复合标签里那一格是不是整数；不是 / 没有都算 false。
bool intField(const NbtTree::Node &compound, const QString &name, qint64 *out) {
    const NbtTree::Node *node = NbtTree::field(compound, name);
    return node != nullptr && integerOf(*node, out);
}

/// 同上，字符串那一格（`Name` / `Author` / `Description`）。
QString stringField(const NbtTree::Node &compound, const QString &name) {
    const NbtTree::Node *node = NbtTree::field(compound, name);
    return node != nullptr && node->type == NbtTree::Type::String ? node->text : QString();
}

/// 三个同名的坐标格（`x` / `y` / `z`）。缺一格就整组当没有。
bool xyzOf(const NbtTree::Node &compound, qint64 *x, qint64 *y, qint64 *z) {
    qint64 vx = 0;
    qint64 vy = 0;
    qint64 vz = 0;
    if (!intField(compound, QStringLiteral("x"), &vx) ||
        !intField(compound, QStringLiteral("y"), &vy) ||
        !intField(compound, QStringLiteral("z"), &vz)) {
        return false;
    }
    if (x != nullptr) *x = vx;
    if (y != nullptr) *y = vy;
    if (z != nullptr) *z = vz;
    return true;
}

/// `qAbs` 在 INT_MIN 上仍然是负的 ⇒ 统一抬到 64 位再取绝对值，再钳回 int。
int positiveSize(qint64 value) {
    const qint64 magnitude = value < 0 ? -value : value;
    constexpr qint64 kLimit = 1ll << 20;  // 一百万格已经远超任何真实结构
    if (magnitude > kLimit) {
        return static_cast<int>(kLimit);
    }
    return static_cast<int>(magnitude);
}

bool isAir(const QString &id) {
    return id == QStringLiteral("minecraft:air") || id == QStringLiteral("minecraft:cave_air") ||
           id == QStringLiteral("minecraft:void_air");
}

/// 方块状态名 → 方块 id（`minecraft:oak_stairs[facing=north]` → `minecraft:oak_stairs`）。
QString blockIdOf(const QString &stateName) {
    const int bracket = stateName.indexOf(QLatin1Char('['));
    return bracket < 0 ? stateName : stateName.left(bracket);
}

/// 记一种方块。空气不计入（包络里绝大多数格子都是空气）。
void countBlock(const QString &stateName, QHash<QString, qint64> *counts, qint64 *nonAir) {
    const QString id = blockIdOf(stateName);
    if (id.isEmpty() || isAir(id)) {
        return;
    }
    ++(*counts)[id];
    ++(*nonAir);
}

/// Litematica 的位打包读法（跨 long 的那一档也要对）。
///
/// 布局和游戏里 `PackedIntegerArray` 一样：第 i 个值从第 `i * bits` 位开始，**高位在前**；
/// 一个值跨两个字时，先取第一个字剩下的低位，再拼上第二个字的高位。
/// 越界返回 -1（调用方跳过这一格，不当成 0 号方块）。
qint64 bitArrayAt(const QList<qint64> &words, qint64 index, int bits) {
    if (index < 0 || bits <= 0) {
        return -1;
    }
    const qint64 start = index * static_cast<qint64>(bits);
    const qint64 wordIndex = start >> 6;
    if (wordIndex < 0 || wordIndex >= words.size()) {
        return -1;
    }
    const int offset = static_cast<int>(start & 63);
    const quint64 mask = (quint64(1) << bits) - 1;
    const quint64 first = static_cast<quint64>(words.at(static_cast<int>(wordIndex)));
    if (offset + bits <= 64) {
        return static_cast<qint64>((first >> (64 - offset - bits)) & mask);
    }
    const int firstBits = 64 - offset;
    if (wordIndex + 1 >= words.size()) {
        return -1;
    }
    const quint64 high = first & ((quint64(1) << firstBits) - 1);
    const int rest = bits - firstBits;
    const quint64 low = static_cast<quint64>(words.at(static_cast<int>(wordIndex + 1))) >>
                        (64 - rest);
    return static_cast<qint64>((high << rest) | low);
}

/// 把按 id 聚合的那张表排成"最多的前 kTopBlockLimit 个"。
void fillTopBlocks(Info *info, const QHash<QString, qint64> &counts) {
    info->topBlocks.clear();
    QList<BlockCount> list;
    list.reserve(counts.size());
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        list.append(BlockCount{it.key(), it.value()});
    }
    std::sort(list.begin(), list.end(), [](const BlockCount &left, const BlockCount &right) {
        if (left.count != right.count) {
            return left.count > right.count;
        }
        return left.id < right.id;  // 同数时定序，界面上不会每次刷新换一行
    });
    info->blocks = list;
    while (list.size() > kTopBlockLimit) {
        list.removeLast();
    }
    info->topBlocks = list;
}

// ---------------------------------------------------------------------------
// 三种格式
// ---------------------------------------------------------------------------

/// Litematica `.litematic`：根里有 `Regions`（一个投影可以有好几块）。
///
/// 每一块自己带调色板和 `BlockStates`（位打包的 LongArray）。**bits 是推出来的**：
/// 从 2 往上试，哪一种 bits 算出来的数组长度正好等于盘上那个长度就认哪一种
/// （Litematica 自己用的是 `max(2, ceil(log2(调色板条目数)))`，但不同版本写过不同的
/// 边角，长度对得上比猜版本可靠）。推不出来就只交元数据。
bool parseLitematic(const NbtTree::Node &root, Info *info, QString *problem) {
    info->format = Format::Litematic;

    if (const NbtTree::Node *meta = NbtTree::field(root, QStringLiteral("Metadata"));
        meta != nullptr && meta->type == NbtTree::Type::Compound) {
        info->name = stringField(*meta, QStringLiteral("Name")).trimmed();
        info->author = stringField(*meta, QStringLiteral("Author")).trimmed();
        info->description = stringField(*meta, QStringLiteral("Description")).trimmed();
        if (const NbtTree::Node *size = NbtTree::field(*meta, QStringLiteral("EnclosingSize"));
            size != nullptr && size->type == NbtTree::Type::Compound) {
            qint64 x = 0;
            qint64 y = 0;
            qint64 z = 0;
            if (xyzOf(*size, &x, &y, &z)) {
                info->width = positiveSize(x);
                info->height = positiveSize(y);
                info->length = positiveSize(z);
            }
        }
    }

    qint64 dataVersion = 0;
    if (intField(root, QStringLiteral("MinecraftDataVersion"), &dataVersion) && dataVersion > 0) {
        info->dataVersion = static_cast<int>(dataVersion);
    }

    const NbtTree::Node *regions = NbtTree::field(root, QStringLiteral("Regions"));
    if (regions == nullptr || regions->type != NbtTree::Type::Compound) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "投影文件缺少 Regions 字段，无法读取方块数据。");
        }
        return false;
    }

    QHash<QString, qint64> counts;
    qint64 nonAir = 0;
    qint64 totalVolume = 0;
    int paletteStates = 0;
    bool complete = true;
    int fallbackWidth = 0;
    int fallbackHeight = 0;
    int fallbackLength = 0;

    for (const NbtTree::Field &regionField : regions->fields) {
        const NbtTree::Node &region = regionField.second;
        if (region.type != NbtTree::Type::Compound) {
            continue;
        }
        qint64 sx = 0;
        qint64 sy = 0;
        qint64 sz = 0;
        if (const NbtTree::Node *size = NbtTree::field(region, QStringLiteral("Size"));
            size != nullptr && size->type == NbtTree::Type::Compound) {
            xyzOf(*size, &sx, &sy, &sz);
        }
        const int rx = positiveSize(sx);
        const int ry = positiveSize(sy);
        const int rz = positiveSize(sz);
        fallbackWidth = std::max(fallbackWidth, rx);
        fallbackHeight = std::max(fallbackHeight, ry);
        fallbackLength = std::max(fallbackLength, rz);

        const qint64 volume = static_cast<qint64>(rx) * ry * rz;
        totalVolume += volume;

        const NbtTree::Node *palette = NbtTree::field(region, QStringLiteral("BlockStatePalette"));
        const NbtTree::Node *states = NbtTree::field(region, QStringLiteral("BlockStates"));
        if (palette == nullptr || palette->type != NbtTree::Type::List || states == nullptr ||
            states->type != NbtTree::Type::LongArray) {
            complete = false;
            continue;
        }

        QList<QString> names;
        names.reserve(palette->items.size());
        for (const NbtTree::Node &item : palette->items) {
            names.append(item.type == NbtTree::Type::Compound
                             ? stringField(item, QStringLiteral("Name"))
                             : QString());
        }
        paletteStates += names.size();

        if (volume <= 0 || names.isEmpty()) {
            continue;
        }
        if (volume > kMaxDecodeVolume) {
            complete = false;
            continue;
        }

        // 哪个 bits 能让"算出来的长度"正好等于盘上那个长度？
        int bits = -1;
        for (int candidate = 2; candidate <= 32; ++candidate) {
            const qint64 expected = (volume * candidate + 63) / 64;
            if (expected != states->longs.size()) {
                continue;
            }
            if (static_cast<qint64>(names.size()) > (1ll << candidate)) {
                continue;  // 调色板装不进这么多位，不可能是它
            }
            bits = candidate;
            break;
        }
        if (bits < 0) {
            complete = false;
            continue;
        }

        for (qint64 index = 0; index < volume; ++index) {
            const qint64 value = bitArrayAt(states->longs, index, bits);
            if (value < 0 || value >= names.size()) {
                complete = false;  // 有一格指到调色板外面 ⇒ 这一份的表不可信
                continue;
            }
            countBlock(names.at(static_cast<int>(value)), &counts, &nonAir);
        }
    }

    // EnclosingSize 没写的时候用各 region 的最大边长兜底（多 region 真实包络还要算
    // 位置偏移，这里只给一个"大致多大"，宁可小、不编）。
    if (info->width <= 0) info->width = fallbackWidth;
    if (info->height <= 0) info->height = fallbackHeight;
    if (info->length <= 0) info->length = fallbackLength;

    info->volume = totalVolume;
    info->paletteSize = paletteStates;
    info->blockCount = nonAir;
    info->countsComplete = complete;
    fillTopBlocks(info, counts);
    return true;
}

/// Sponge `.schematic`：`Palette`（名字 → 下标）+ `BlockData`（varint 下标流）。
bool parseSponge(const NbtTree::Node &root, Info *info, QString *problem) {
    info->format = Format::SpongeSchematic;

    qint64 dataVersion = 0;
    if (intField(root, QStringLiteral("DataVersion"), &dataVersion) && dataVersion > 0) {
        info->dataVersion = static_cast<int>(dataVersion);
    }

    qint64 width = 0;
    qint64 height = 0;
    qint64 length = 0;
    intField(root, QStringLiteral("Width"), &width);
    intField(root, QStringLiteral("Height"), &height);
    intField(root, QStringLiteral("Length"), &length);
    info->width = positiveSize(width);
    info->height = positiveSize(height);
    info->length = positiveSize(length);

    if (const NbtTree::Node *meta = NbtTree::field(root, QStringLiteral("Metadata"));
        meta != nullptr && meta->type == NbtTree::Type::Compound) {
        info->name = stringField(*meta, QStringLiteral("Name")).trimmed();
        info->author = stringField(*meta, QStringLiteral("Author")).trimmed();
    }

    const NbtTree::Node *palette = NbtTree::field(root, QStringLiteral("Palette"));
    if (palette == nullptr || palette->type != NbtTree::Type::Compound) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "原理图缺少 Palette 字段，无法读取方块数据。");
        }
        return false;
    }
    QHash<qint64, QString> byIndex;
    byIndex.reserve(palette->fields.size());
    for (const NbtTree::Field &entry : palette->fields) {
        qint64 index = 0;
        if (integerOf(entry.second, &index)) {
            byIndex.insert(index, entry.first);
        }
    }
    info->paletteSize = palette->fields.size();

    if (info->width > 0 && info->height > 0 && info->length > 0) {
        info->volume = static_cast<qint64>(info->width) * info->height * info->length;
    }

    QHash<QString, qint64> counts;
    qint64 nonAir = 0;
    bool complete = true;

    const NbtTree::Node *data = NbtTree::field(root, QStringLiteral("BlockData"));
    if (data == nullptr || data->type != NbtTree::Type::ByteArray) {
        complete = false;
    } else {
        quint64 value = 0;
        int shift = 0;
        bool inNumber = false;
        for (const char raw : data->bytes) {
            const quint8 byte = static_cast<quint8>(raw);
            inNumber = true;
            if (shift > 28) {
                complete = false;  // 一个下标写到 5 个字节以上 ⇒ 表坏了，后面全不作数
                break;
            }
            value |= static_cast<quint64>(byte & 0x7F) << shift;
            if ((byte & 0x80) != 0) {
                shift += 7;
                continue;
            }
            const auto it = byIndex.constFind(static_cast<qint64>(value));
            if (it != byIndex.constEnd()) {
                countBlock(it.value(), &counts, &nonAir);
            } else {
                complete = false;
            }
            value = 0;
            shift = 0;
            inNumber = false;
        }
        if (inNumber) {
            complete = false;  // 最后一个数没写完（多半是文件被截了）
        }
    }

    info->blockCount = nonAir;
    info->countsComplete = complete;
    fillTopBlocks(info, counts);
    return true;
}

/// 原版结构方块 `.nbt`：`size`（三个 Int）+ `palette`（List）+ `blocks`（List）。
bool parseStructure(const NbtTree::Node &root, Info *info, QString *problem) {
    info->format = Format::Structure;

    qint64 dataVersion = 0;
    if (intField(root, QStringLiteral("DataVersion"), &dataVersion) && dataVersion > 0) {
        info->dataVersion = static_cast<int>(dataVersion);
    }

    const NbtTree::Node *size = NbtTree::field(root, QStringLiteral("size"));
    if (size != nullptr && size->type == NbtTree::Type::List && size->items.size() == 3) {
        qint64 dims[3] = {0, 0, 0};
        bool allInts = true;
        for (int i = 0; i < 3; ++i) {
            if (!integerOf(size->items.at(i), &dims[i])) {
                allInts = false;
                break;
            }
        }
        if (allInts) {
            info->width = positiveSize(dims[0]);
            info->height = positiveSize(dims[1]);
            info->length = positiveSize(dims[2]);
            info->volume = static_cast<qint64>(info->width) * info->height * info->length;
        }
    }

    const NbtTree::Node *palette = NbtTree::field(root, QStringLiteral("palette"));
    if (palette == nullptr || palette->type != NbtTree::Type::List) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "结构文件缺少 palette 字段，无法读取方块数据。");
        }
        return false;
    }
    QList<QString> names;
    names.reserve(palette->items.size());
    for (const NbtTree::Node &item : palette->items) {
        names.append(item.type == NbtTree::Type::Compound
                         ? stringField(item, QStringLiteral("Name"))
                         : QString());
    }
    info->paletteSize = names.size();

    QHash<QString, qint64> counts;
    qint64 nonAir = 0;
    bool complete = true;

    const NbtTree::Node *blocks = NbtTree::field(root, QStringLiteral("blocks"));
    if (blocks == nullptr || blocks->type != NbtTree::Type::List) {
        complete = false;
    } else {
        for (const NbtTree::Node &block : blocks->items) {
            if (block.type != NbtTree::Type::Compound) {
                complete = false;
                continue;
            }
            qint64 state = 0;
            if (!intField(block, QStringLiteral("state"), &state)) {
                complete = false;
                continue;
            }
            if (state < 0 || state >= names.size()) {
                complete = false;
                continue;
            }
            countBlock(names.at(static_cast<int>(state)), &counts, &nonAir);
        }
    }

    info->blockCount = nonAir;
    info->countsComplete = complete;
    fillTopBlocks(info, counts);
    return true;
}

/// 读整份文件的字节（gzip 的先解到临时目录再读回来）。失败时 `problem` 里是原因。
bool readBytes(const QString &path, QByteArray *out, QString *problem) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "未找到文件。");
        }
        return false;
    }
    if (info.size() <= 0) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "文件为空。");
        }
        return false;
    }
    if (info.size() > kMaxCompressedBytes) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "文件大小为 %1 MB，超出预览上限。")
                           .arg(QLocale(QLocale::English).toString(info.size() / (1024.0 * 1024.0), 'f', 1));
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "无法打开结构文件：%1").arg(file.errorString());
        }
        return false;
    }
    const QByteArray head = file.read(2);
    file.close();

    const bool gzip = head.size() == 2 && static_cast<quint8>(head.at(0)) == 0x1f &&
                      static_cast<quint8>(head.at(1)) == 0x8b;
    if (!gzip) {
        if (!file.open(QIODevice::ReadOnly)) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("SchematicInfo", "无法打开结构文件：%1").arg(file.errorString());
            }
            return false;
        }
        *out = file.readAll();
        file.close();
        return true;
    }

    QTemporaryDir temp;
    if (!temp.isValid()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "无法创建临时目录。");
        }
        return false;
    }
    const QString plainPath = temp.filePath(QStringLiteral("body.nbt"));
    QString error;
    if (!ZipUtil::gunzipTo(path, plainPath, &error)) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "gzip 解压失败：%1").arg(error);
        }
        return false;
    }
    const QFileInfo plainInfo(plainPath);
    if (!plainInfo.exists() || plainInfo.size() <= 0) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "解压内容为空，文件可能损坏或下载未完成。");
        }
        return false;
    }
    if (plainInfo.size() > kMaxUncompressedBytes) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "解压后大小超过 %1 MB，不展开读取。")
                           .arg(kMaxUncompressedBytes / (1024 * 1024));
        }
        return false;
    }
    QFile plain(plainPath);
    if (!plain.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("SchematicInfo", "无法打开解压后的临时文件。");
        }
        return false;
    }
    *out = plain.readAll();
    plain.close();
    return true;
}

/// 格式化数字（12345 → "12,345"）。
QString numberText(qint64 value) { return QLocale(QLocale::English).toString(value); }

struct DataVersionEntry {
    int dataVersion;
    const char *gameVersion;
};

/// 正式版的数据版本表。**只写有把握的**：认不出来就只报数字，不猜。
/// （快照的号一版一个，这里不收；1.13 之前的版本没有可靠对照，也不收。）
constexpr DataVersionEntry kDataVersions[] = {
    {1519, "1.13"},     {1628, "1.13.1"},  {1631, "1.13.2"},  {1952, "1.14"},
    {1957, "1.14.1"},   {1963, "1.14.2"},  {1968, "1.14.3"},  {1976, "1.14.4"},
    {2225, "1.15"},     {2227, "1.15.1"},  {2230, "1.15.2"},  {2566, "1.16"},
    {2567, "1.16.1"},   {2578, "1.16.2"},  {2580, "1.16.3"},  {2586, "1.16.4"},
    {2724, "1.17"},     {2730, "1.17.1"},  {2825, "1.18"},    {2847, "1.18.1"},
    {2865, "1.18.2"},   {3105, "1.19"},    {3117, "1.19.1"},  {3120, "1.19.2"},
    {3218, "1.19.3"},   {3337, "1.19.4"},  {3463, "1.20"},    {3465, "1.20.1"},
    {3578, "1.20.2"},   {3698, "1.20.3"},  {3700, "1.20.4"},  {3837, "1.20.5"},
    {3839, "1.20.6"},   {3953, "1.21"},    {3955, "1.21.1"},  {4080, "1.21.2"},
    {4082, "1.21.3"},   {4189, "1.21.4"},  {4325, "1.21.5"},  {4435, "1.21.6"},
    {4438, "1.21.7"},   {4440, "1.21.8"},
};

}  // namespace

QString formatName(Format format) {
    switch (format) {
    case Format::Litematic:
        return QCoreApplication::translate("SchematicInfo", "Litematica 投影");
    case Format::SpongeSchematic:
        return QCoreApplication::translate("SchematicInfo", "Sponge 原理图");
    case Format::Structure:
        return QCoreApplication::translate("SchematicInfo", "原版结构");
    case Format::Unknown:
        break;
    }
    return QCoreApplication::translate("SchematicInfo", "无法识别");
}

Info read(const QString &path) {
    Info info;

    QByteArray raw;
    if (!readBytes(path, &raw, &info.problem)) {
        return info;
    }

    NbtTree::Document document;
    QString problem;
    if (!NbtTree::parse(raw, &document, &problem)) {
        info.problem = QCoreApplication::translate("SchematicInfo", "无法解析 NBT 文件：%1").arg(problem);
        return info;
    }
    if (document.root.type != NbtTree::Type::Compound) {
        info.problem = QCoreApplication::translate("SchematicInfo", "根标签不是复合标签，无法作为结构或原理图读取。");
        return info;
    }

    // 判据是**字段**，不是后缀（见头文件那段）。顺序有意义：Litematica 的 Regions
    // 最强壮；Sponge 与结构各自的 Palette 大小写不同，不会互相误判。
    const NbtTree::Node &root = document.root;
    if (NbtTree::field(root, QStringLiteral("Regions")) != nullptr) {
        if (!parseLitematic(root, &info, &info.problem)) {
            return info;
        }
    } else if (NbtTree::field(root, QStringLiteral("Palette")) != nullptr) {
        if (!parseSponge(root, &info, &info.problem)) {
            return info;
        }
    } else if (NbtTree::field(root, QStringLiteral("palette")) != nullptr) {
        if (!parseStructure(root, &info, &info.problem)) {
            return info;
        }
    } else {
        info.problem = QCoreApplication::translate("SchematicInfo", "缺少 Regions、Palette 或 palette 字段，无法识别结构文件格式。");
        return info;
    }

    info.ok = true;
    info.problem.clear();
    return info;
}

QString gameVersionOfDataVersion(int dataVersion) {
    for (const DataVersionEntry &entry : kDataVersions) {
        if (entry.dataVersion == dataVersion) {
            return QString::fromLatin1(entry.gameVersion);
        }
    }
    return QString();
}

int dataVersionOfGameVersion(const QString &gameVersion) {
    // 从 "1.20.1-Forge_47.2.0" / "我的 1.20.1 整合包" 这种串里抠第一个版本号。
    static const QRegularExpression pattern(QStringLiteral("(\\d+\\.\\d+(?:\\.\\d+)?)"));
    const QRegularExpressionMatch match = pattern.match(gameVersion);
    if (!match.hasMatch()) {
        return 0;
    }
    const QString version = match.captured(1);
    for (const DataVersionEntry &entry : kDataVersions) {
        if (version == QLatin1String(entry.gameVersion)) {
            return entry.dataVersion;
        }
    }
    return 0;
}

QString compatibilityText(const Info &info, const QString &targetGameVersion) {
    if (!info.ok || info.dataVersion <= 0) {
        return QString();
    }
    const int target = dataVersionOfGameVersion(targetGameVersion);
    if (target <= 0 || target == info.dataVersion) {
        return QString();
    }
    const QString sourceName = gameVersionOfDataVersion(info.dataVersion);
    const QString targetName = gameVersionOfDataVersion(target);
    const QString sourceText = sourceName.isEmpty()
                                   ? QCoreApplication::translate("SchematicInfo", "数据版本 %1").arg(info.dataVersion)
                                   : sourceName;
    const QString targetText = targetName.isEmpty() ? targetGameVersion : targetName;
    return QCoreApplication::translate("SchematicInfo", "文件版本为 %1，当前实例版本为 %2。版本差异可能导致部分方块无法识别。")
        .arg(sourceText, targetText);
}

QString summaryText(const Info &info, const QString &targetGameVersion) {
    if (!info.ok) {
        return info.problem;
    }
    QStringList parts;
    if (!info.name.isEmpty()) {
        parts.append(QCoreApplication::translate("SchematicInfo", "「%1」").arg(info.name));
    }
    if (!info.author.isEmpty()) {
        parts.append(QCoreApplication::translate("SchematicInfo", "作者 %1").arg(info.author));
    }
    if (info.width > 0 && info.height > 0 && info.length > 0) {
        parts.append(QStringLiteral("%1 × %2 × %3")
                         .arg(info.width)
                         .arg(info.height)
                         .arg(info.length));
    }
    if (info.paletteSize > 0) {
        parts.append(QCoreApplication::translate("SchematicInfo", "%1 种方块").arg(numberText(info.paletteSize)));
    }
    if (info.countsComplete && info.blockCount > 0) {
        parts.append(QCoreApplication::translate("SchematicInfo", "%1 个方块").arg(numberText(info.blockCount)));
    }
    parts.append(formatName(info.format));
    if (info.dataVersion > 0) {
        const QString version = gameVersionOfDataVersion(info.dataVersion);
        parts.append(version.isEmpty()
                         ? QCoreApplication::translate("SchematicInfo", "数据版本 %1").arg(info.dataVersion)
                         : QCoreApplication::translate("SchematicInfo", "%1（数据版本 %2）")
                               .arg(version)
                               .arg(info.dataVersion));
    }
    QString text = parts.join(QStringLiteral(" / "));
    if (!info.countsComplete) {
        text += QCoreApplication::translate("SchematicInfo", "\n（方块数据读取失败，仅显示元数据）");
    }
    const QString compatibility = compatibilityText(info, targetGameVersion);
    if (!compatibility.isEmpty()) {
        text += QLatin1Char('\n') + TextMarks::warning(compatibility);
    }
    if (!info.description.isEmpty()) {
        // 描述常常很长、还带颜色码；只留第一行，剩下的放悬停提示。
        QString firstLine = info.description.left(info.description.indexOf(QLatin1Char('\n')) < 0
                                                      ? info.description.size()
                                                      : info.description.indexOf(QLatin1Char('\n')));
        if (!firstLine.trimmed().isEmpty()) {
            text += QStringLiteral("\n%1").arg(firstLine.trimmed());
        }
    }
    return text;
}

static bool isSafeLanguageCode(const QString &language) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9_]+$"));
    return pattern.match(language).hasMatch();
}

QJsonObject blockNamesFromJson(const QByteArray &json) {
    if (json.isEmpty() || json.size() > 8 * 1024 * 1024) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    return doc.isObject() ? doc.object() : QJsonObject();
}

QJsonObject loadBlockNames(const QString &assetsRoot, const QString &indexId,
                           const QString &language) {
    if (assetsRoot.isEmpty() || !AssetNames::isSafeIndexId(indexId) ||
        !isSafeLanguageCode(language)) {
        return {};
    }
    const auto readObject = [&assetsRoot](const QString &path, qint64 limit) {
        if (!InstanceFiles::pathIsSameOrInside(assetsRoot, path)) return QJsonObject();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return QJsonObject();
        const QByteArray bytes = file.read(limit + 1);
        return bytes.size() <= limit ? QJsonDocument::fromJson(bytes).object() : QJsonObject();
    };
    const auto index = readObject(assetsRoot + QStringLiteral("/indexes/%1.json").arg(indexId), 32 * 1024 * 1024);
    const QString entry = QStringLiteral("minecraft/lang/%1.json").arg(language);
    const QString hash = index.value(QStringLiteral("objects")).toObject()
        .value(entry).toObject().value(QStringLiteral("hash")).toString();
    if (!AssetNames::isSha1Hex(hash)) return {};
    return readObject(assetsRoot + QStringLiteral("/objects/%1/%2").arg(hash.left(2), hash), 8 * 1024 * 1024);
}

QJsonObject loadChineseBlockNames(const QString &assetsRoot, const QString &indexId) {
    return loadBlockNames(assetsRoot, indexId, QStringLiteral("zh_cn"));
}

QString chineseBlockName(const QJsonObject &language, const QString &blockId) {
    const qsizetype separator = blockId.indexOf(QLatin1Char(':'));
    const QString nameSpace = separator < 0 ? QStringLiteral("minecraft") : blockId.left(separator);
    QString path = blockId.mid(separator + 1);
    path.replace(QLatin1Char('/'), QLatin1Char('.'));
    const QString name = language.value(QStringLiteral("block.%1.%2").arg(nameSpace, path)).toString();
    return name.size() <= 256 && !name.contains(QLatin1Char('\n')) && !name.contains(QLatin1Char('\r'))
        && !name.contains(QLatin1Char('\t')) ? name : QString();
}

QString topBlocksText(const Info &info, const QJsonObject &language) {
    if (!info.ok || info.topBlocks.isEmpty()) {
        return QString();
    }
    QStringList lines;
    lines.append(QCoreApplication::translate("SchematicInfo", "数量最多的方块："));
    for (const BlockCount &block : info.topBlocks) {
        const QString name = chineseBlockName(language, block.id);
        lines.append(QCoreApplication::translate("SchematicInfo", "  %1：%2")
                         .arg(name.isEmpty() ? block.id : name, numberText(block.count)));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace SchematicInfo

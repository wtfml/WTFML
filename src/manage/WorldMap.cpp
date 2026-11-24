/**
 * @file WorldMap.cpp
 * @brief `WorldMap.hpp` 的实现：头部表那 8 KiB 的解析、`level.dat` 那几格的读取、目录扫描。
 */

#include "manage/WorldMap.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace WorldMap {
namespace {

/// 区域文件的头部固定 8 KiB（4 KiB 位置表 + 4 KiB 时间戳表）。
constexpr int kHeaderBytes = 8192;
/// 一个区域文件按 32×32 个区块切。
constexpr int kRegionSide = 32;

/// 整数族（Byte/Short/Int/Long）都收，浮点数/字符串不收。
bool readInteger(const NbtTree::Node *node, qint64 *out) {
    if (node == nullptr) {
        return false;
    }
    switch (node->type) {
    case NbtTree::Type::Byte:
    case NbtTree::Type::Short:
    case NbtTree::Type::Int:
    case NbtTree::Type::Long:
        *out = node->number;
        return true;
    default:
        return false;
    }
}

/// 变量名里那几个维度的对照（老存档 `Data/Player/Dimension` 是一个 Int）。
QString dimensionNameFromNumber(qint64 value) {
    switch (value) {
    case -1:
        return QStringLiteral("minecraft:the_nether");
    case 0:
        return QStringLiteral("minecraft:overworld");
    case 1:
        return QStringLiteral("minecraft:the_end");
    default:
        return QString();
    }
}

/// 递归加起来：**不跟 junction、不跟符号链接**（`isSymLink()` 对 junction 是 false，
/// 见 util 里那一条；`isJunction()` 才是判据）。
void addDirectorySize(const QString &dir, quint64 *bytes, int *files) {
    const QFileInfoList entries =
        QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden
                                    | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink() || entry.isJunction()) {
            continue;
        }
        if (entry.isDir()) {
            addDirectorySize(entry.absoluteFilePath(), bytes, files);
        } else {
            if (entry.size() > 0) {
                *bytes += static_cast<quint64>(entry.size());
            }
            ++(*files);
        }
    }
}

} // namespace

void readLevelFacts(const NbtTree::Document &document, Snapshot *out) {
    if (out == nullptr) {
        return;
    }

    // ---------- 种子 ----------
    // 1.16 起在 `Data/WorldGenSettings/seed`；更老的存档在 `Data/RandomSeed`。
    // 两个都在时**新那一个说了算**（老档被工具升过级时两格可能对不上，游戏自己读的是新那格）。
    qint64 number = 0;
    if (readInteger(NbtTree::descend(document.root, QStringLiteral("Data/WorldGenSettings/seed")),
                    &number)) {
        out->hasSeed = true;
        out->seed = number;
    } else if (readInteger(NbtTree::descend(document.root, QStringLiteral("Data/RandomSeed")),
                           &number)) {
        out->hasSeed = true;
        out->seed = number;
    }

    // ---------- 玩家 ----------
    // `Data/Player` 不在 = 从没进过这个世界（或者这是一份服务端存档）。
    const NbtTree::Node *player = NbtTree::descend(document.root, QStringLiteral("Data/Player"));
    if (player != nullptr) {
        if (const NbtTree::Node *pos = NbtTree::field(*player, QStringLiteral("Pos"));
            pos != nullptr && pos->type == NbtTree::Type::List && pos->items.size() >= 3) {
            const NbtTree::Node &x = pos->items.at(0);
            const NbtTree::Node &y = pos->items.at(1);
            const NbtTree::Node &z = pos->items.at(2);
            if (x.type == NbtTree::Type::Double && y.type == NbtTree::Type::Double
                && z.type == NbtTree::Type::Double) {
                out->hasPlayer = true;
                out->playerX = x.real;
                out->playerY = y.real;
                out->playerZ = z.real;
            }
        }
        if (const NbtTree::Node *dimension = NbtTree::field(*player, QStringLiteral("Dimension"));
            dimension != nullptr) {
            if (dimension->type == NbtTree::Type::String) {
                out->hasPlayerDimension = true;
                out->playerDimension = dimension->text;
            } else if (qint64 value = 0; readInteger(dimension, &value)) {
                const QString name = dimensionNameFromNumber(value);
                // 认不出来的数字（模组维度）**留空**，不编一个名字。
                if (!name.isEmpty()) {
                    out->hasPlayerDimension = true;
                    out->playerDimension = name;
                }
            }
        }
    }

    // ---------- 出生点 ----------
    qint64 spawnX = 0;
    qint64 spawnY = 0;
    qint64 spawnZ = 0;
    if (readInteger(NbtTree::descend(document.root, QStringLiteral("Data/SpawnX")), &spawnX)
        && readInteger(NbtTree::descend(document.root, QStringLiteral("Data/SpawnY")), &spawnY)
        && readInteger(NbtTree::descend(document.root, QStringLiteral("Data/SpawnZ")), &spawnZ)) {
        out->hasSpawn = true;
        out->spawnX = static_cast<int>(spawnX);
        out->spawnY = static_cast<int>(spawnY);
        out->spawnZ = static_cast<int>(spawnZ);
    }
}

bool parseRegionHeader(const QByteArray &header, int regionX, int regionZ, int *chunksOut,
                       ChunkBounds *boundsOut) {
    if (header.size() < kHeaderBytes) {
        return false;
    }
    int chunks = 0;
    ChunkBounds bounds;
    for (int index = 0; index < kRegionSide * kRegionSide; ++index) {
        const int at = index * 4;
        const quint8 b0 = static_cast<quint8>(header.at(at));
        const quint8 b1 = static_cast<quint8>(header.at(at + 1));
        const quint8 b2 = static_cast<quint8>(header.at(at + 2));
        // 前三个字节是扇区偏移。**全零 = 那个区块还没生成过**（第 4 字节的扇区数也是 0，
        // 但有些工具会留下"偏移 0、扇区数非 0"的脏项 —— 按偏移判，和游戏自己一致）。
        const quint32 sectorOffset = (static_cast<quint32>(b0) << 16)
                                     | (static_cast<quint32>(b1) << 8) | static_cast<quint32>(b2);
        if (sectorOffset == 0) {
            continue;
        }
        ++chunks;
        const int localX = index % kRegionSide;
        const int localZ = index / kRegionSide;
        const int chunkX = regionX * kRegionSide + localX;
        const int chunkZ = regionZ * kRegionSide + localZ;
        if (!bounds.ok) {
            bounds.ok = true;
            bounds.minX = bounds.maxX = chunkX;
            bounds.minZ = bounds.maxZ = chunkZ;
        } else {
            bounds.minX = qMin(bounds.minX, chunkX);
            bounds.maxX = qMax(bounds.maxX, chunkX);
            bounds.minZ = qMin(bounds.minZ, chunkZ);
            bounds.maxZ = qMax(bounds.maxZ, chunkZ);
        }
    }
    if (chunksOut != nullptr) {
        *chunksOut = chunks;
    }
    if (boundsOut != nullptr) {
        *boundsOut = bounds;
    }
    return true;
}

Snapshot scan(const QString &worldDir) {
    Snapshot out;
    const QFileInfo worldInfo(worldDir);
    if (worldDir.isEmpty() || !worldInfo.isDir()) {
        out.problem = QCoreApplication::translate("WorldMap", "这个世界的目录不在了。");
        return out;
    }

    // 下界 / 末地 / 模组维度存在与否 —— 这一版**不算它们**，但要如实报出来（见 .hpp）。
    out.hasOtherDimensions = QFileInfo(worldDir + QStringLiteral("/DIM-1")).isDir()
                             || QFileInfo(worldDir + QStringLiteral("/DIM1")).isDir()
                             || QFileInfo(worldDir + QStringLiteral("/dimensions")).isDir();

    const QString regionDir = worldDir + QStringLiteral("/region");
    out.hasRegionDir = QFileInfo(regionDir).isDir();
    if (!out.hasRegionDir) {
        // 刚建好还没存过档的世界、或者被清过的档：`region/` 本来就可以不在。
        // 这不是错误（`WorldFiles::looksLikeWorld()` 也只认 `level.dat`）。
        out.ok = true;
    } else {
        const QFileInfoList files =
            QDir(regionDir).entryInfoList(QStringList(), QDir::Files | QDir::NoDotAndDotDot);
        out.ok = true;
        for (const QFileInfo &file : files) {
            if (!file.fileName().endsWith(QStringLiteral(".mca"), Qt::CaseInsensitive)) {
                continue;
            }
            const QStringList parts = file.completeBaseName().split(QLatin1Char('.'));
            bool okX = false;
            bool okZ = false;
            const int regionX = parts.size() == 3 ? parts.at(1).toInt(&okX) : 0;
            const int regionZ = parts.size() == 3 ? parts.at(2).toInt(&okZ) : 0;
            // 名字不是 `r.<x>.<z>` 的 `.mca` 不当区域文件算 —— 这一层不去猜。
            if (parts.size() != 3 || parts.first() != QStringLiteral("r") || !okX || !okZ) {
                continue;
            }

            RegionFile region;
            region.x = regionX;
            region.z = regionZ;
            region.bytes = file.size();
            QFile handle(file.absoluteFilePath());
            if (handle.open(QIODevice::ReadOnly)) {
                const QByteArray header = handle.read(kHeaderBytes);
                ChunkBounds bounds;
                if (parseRegionHeader(header, regionX, regionZ, &region.chunks, &bounds)) {
                    region.headerRead = true;
                    out.chunkCount += region.chunks;
                    if (bounds.ok) {
                        if (!out.chunkBounds.ok) {
                            out.chunkBounds = bounds;
                        } else {
                            out.chunkBounds.minX = qMin(out.chunkBounds.minX, bounds.minX);
                            out.chunkBounds.maxX = qMax(out.chunkBounds.maxX, bounds.maxX);
                            out.chunkBounds.minZ = qMin(out.chunkBounds.minZ, bounds.minZ);
                            out.chunkBounds.maxZ = qMax(out.chunkBounds.maxZ, bounds.maxZ);
                        }
                    }
                }
            }
            if (!region.headerRead) {
                ++out.unreadableRegions;
            }
            out.regionBytes += region.bytes;
            out.regions.append(region);
        }
    }

    std::sort(out.regions.begin(), out.regions.end(),
              [](const RegionFile &a, const RegionFile &b) {
                  if (a.x != b.x) {
                      return a.x < b.x;
                  }
                  return a.z < b.z;
              });

    addDirectorySize(worldInfo.absoluteFilePath(), &out.dirBytes, &out.fileCount);
    return out;
}

} // namespace WorldMap

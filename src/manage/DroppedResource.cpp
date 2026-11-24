/**
 * @file DroppedResource.cpp
 * @brief DroppedResource 的实现（见 .hpp 里那几条判据）。
 */

#include "DroppedResource.hpp"
#include <QCoreApplication>

#include <QFileInfo>

#include "../util/ZipUtil.hpp"
#include "ModpackImport.hpp"
#include "util/SevenZip.hpp"

namespace {

/// 条目名归一到"能拿来比"的形状：`\` 当 `/`，整体小写。
///
/// 为什么要转小写：zip 的**条目名是区分大小写的**，而"这个包里有没有 pack.mcmeta"
/// 这件事不该因为作者把文件写成了 `Pack.MCMeta` 就答错 —— 那个包在游戏里照样是资源包。
/// 为什么要管 `\`：zip 规范里条目名只用 `/`，但真实世界里有一批工具（尤其是国内那些
/// "一键压缩"）会写进反斜杠。不去管它的话，一个用反斜杠写的资源包在我们眼里就是
/// "里面什么都没有"。
QString normalized(const QString &entryName) {
    QString name = entryName;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return name.toLower();
}

/// 这份（已归一的）条目表里有没有 @p segment 这一段目录（传进去的是**不带斜杠**的那一段，
/// 比如 `assets`）。
bool hasSegment(const QStringList &folded, const QString &segment) {
    const QChar slashChar = QLatin1Char('/');
    const QString slash(slashChar);
    for (const QString &entry : folded) {
        // 四种写法都算：`assets/…`、目录条目自己 `assets`（zip 里一条以 `/` 结尾的记录）、
        // 被套在一层目录里的 `MyPack/assets`，以及 `MyPack/assets/…`。
        if (entry == segment || entry.startsWith(segment + slash) || entry.endsWith(slashChar + segment)
            || entry.contains(slash + segment + slash)) {
            return true;
        }
    }
    return false;
}

/// 有没有一个**叫这个名字的文件**（允许它被套在一层目录里：`MyPack/pack.mcmeta`）。
///
/// 为什么要放宽到"套一层也行"：用户手边那种包十有八九是"整个目录一起压"的
/// （GitHub 的 zipball、`右键 → 压缩`），根目录是 `<包名>/…`。只认根目录的话，
/// 那种包会被判成"认不出来"，而它其实一点毛病都没有。
/// NOTE: 反过来（把嵌套也算进去）是安全的：这一条只在**已经排除了整合包**之后才问。
bool hasFileNamed(const QStringList &folded, const QString &fileName) {
    for (const QString &entry : folded) {
        if (entry == fileName || entry.endsWith(QLatin1Char('/') + fileName)) {
            return true;
        }
    }
    return false;
}

}  // namespace

DroppedResource::Result DroppedResource::classify(const QString &path) {
    Result result;
    result.path = path;

    const QFileInfo info(path);
    if (!info.isFile()) {
        // 拖进来的可能是个目录（有人习惯拖文件夹）或者干脆是不存在的东西。两种都算
        // "认不出来"，但**理由不一样**，detail 里说清楚。
        result.detail = info.isDir() ? QCoreApplication::translate("DroppedResource", "该路径为文件夹，不是文件")
                                     : QCoreApplication::translate("DroppedResource", "未找到文件");
        return result;
    }

    const QString suffix = info.suffix().toLower();
    if (suffix == QLatin1String("jar")) {
        // 见 .hpp 第 1 条：jar 只有一个意思，连盘都不用读。
        result.known = true;
        result.kind = ResourceCatalog::Kind::Mod;
        result.detail = QCoreApplication::translate("DroppedResource", "后缀是 .jar");
        return result;
    }
    // rar/7z：本机装了 7-Zip 就按整合包接（导入时先解压再走 zip 那条路）。
    if (SevenZip::isArchive(path)) {
        if (SevenZip::available()) {
            result.known = true;
            result.kind = ResourceCatalog::Kind::Modpack;
            result.detail = QCoreApplication::translate("DroppedResource", "需要 7-Zip 解压后导入");
        } else {
            result.detail = QCoreApplication::translate("DroppedResource", "需要 7-Zip 解压，本机未找到 7z.exe");
        }
        return result;
    }
    if (suffix != QLatin1String("zip") && suffix != QLatin1String("mrpack")) {
        result.detail = suffix.isEmpty()
                            ? QCoreApplication::translate("DroppedResource", "文件缺少扩展名，无法识别类型")
                            : QCoreApplication::translate("DroppedResource", "`.%1` 不是可安装的内容").arg(suffix);
        return result;
    }

    // ---------- 到这儿都是压缩包：只能看里面有什么 ----------
    QStringList entries;
    QString error;
    const ZipUtil::EntryStatus status =
        ZipUtil::listEntries(path, &entries, &error, QCoreApplication::translate("DroppedResource", "读取压缩包"));
    if (status == ZipUtil::EntryStatus::NotAnArchive) {
        result.detail = QCoreApplication::translate("DroppedResource", "无法打开：不是 zip 压缩包或文件已损坏");
        return result;
    }
    if (status != ZipUtil::EntryStatus::Ok) {
        result.detail = error.isEmpty() ? QCoreApplication::translate("DroppedResource", "无法读取文件") : error;
        return result;
    }

    QStringList folded;
    folded.reserve(entries.size());
    for (const QString &entry : entries) {
        folded.append(normalized(entry));
    }

    // ① 整合包：判据全在 ModpackImport::detectArchiveLayout()（清单、MultiMC 实例、直接
    // 压进来的 .minecraft、PCL 那种套着 .mrpack 的壳，还有"只有一堆模组"的反判）。
    // 概念只有一个出处，这里不再按文件名抄一遍。
    //
    // NOTE: 服务端包 / 世界存档 / "只有 mods/" 这三档 detect 会认出来并**明确排除**，
    // 所以要先判它们，别让下面资源包那两条把它们接走（服务端包里也有 mods/，世界存档里
    // 也有 data/）。
    const ModpackImport::ArchiveLayout layout = ModpackImport::detectArchiveLayout(entries);
    if (layout.serverPack || layout.worldSave || layout.modsBundle) {
        result.detail = layout.detail;
        return result;
    }
    if (layout.format != ModpackImport::Format::Unknown) {
        result.known = true;
        result.kind = ResourceCatalog::Kind::Modpack;
        result.detail = layout.detail;
        return result;
    }

    // ② 资源包 / 数据包：**先说 pack.mcmeta**，再说里面装的是 assets 还是 data。
    if (hasFileNamed(folded, QStringLiteral("pack.mcmeta"))) {
        if (hasSegment(folded, QStringLiteral("assets"))) {
            result.known = true;
            result.kind = ResourceCatalog::Kind::ResourcePack;
            result.detail = QCoreApplication::translate("DroppedResource", "包含 pack.mcmeta 与 assets/");
            return result;
        }
        if (hasSegment(folded, QStringLiteral("data"))) {
            result.known = true;
            result.kind = ResourceCatalog::Kind::DataPack;
            result.detail = QCoreApplication::translate("DroppedResource", "包含 pack.mcmeta 与 data/");
            return result;
        }
    }

    // ③ 光影：**没有 pack.mcmeta**，只有 shaders/ —— 所以它必须排在资源包那一条之后，
    // 否则资源包里那份 `assets/.../shaders/` 会把资源包认成光影（见 .hpp 那句 ★）。
    if (hasSegment(folded, QStringLiteral("shaders"))) {
        result.known = true;
        result.kind = ResourceCatalog::Kind::Shader;
        result.detail = QCoreApplication::translate("DroppedResource", "包含 shaders/");
        return result;
    }

    // 判据层认出"为什么不是"的时候（比如好几个实例打在一起），用它那句话 ——
    // 一句光秃秃的「认不出」会让用户以为包坏了。
    if (!layout.detail.isEmpty()) {
        result.detail = layout.detail;
        return result;
    }
    result.detail = entries.isEmpty()
                        ? QCoreApplication::translate("DroppedResource", "压缩包为空")
                        : QCoreApplication::translate("DroppedResource", "未找到整合包清单、pack.mcmeta 或 shaders/，无法识别压缩包类型");
    return result;
}


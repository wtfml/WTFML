#pragma once
#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QThread>
#include <QElapsedTimer>
#include <algorithm>

namespace ArmorTrimAssets {
struct Result { QHash<QString, QImage> images; QString error; QString source; };

/// 一个"可以拿来读纹理的客户端 jar"候选。
struct ArchiveCandidate {
    QString id;        ///< 版本目录名（只看给人看的来源行）
    QString jar;       ///< 客户端 jar 的绝对路径
    qint64 releasedAt = 0;  ///< 这个 jar 所代表的游戏版本的发布时间（毫秒）
};

/**
 * @brief 把 1.21.2+/26.x 的新路径折回旧名字（下游 `ArmorTrimScene` 只认旧名）。
 *
 * 1.21.2 起游戏把盔甲层和纹饰挪进了"装备层"：
 * 旧 `textures/models/armor/<材质>_layer_1.png` → `textures/entity/equipment/humanoid/<材质>.png`
 * 旧 `textures/trims/models/armor/<图案>.png` → `textures/trims/entity/humanoid/<图案>.png`
 * 26.x 又把调色板挪到 `textures/palettes/trim/`、`textures/palettes/trim_base.png`。
 * **UV 和尺寸一个像素都没变**（逐字节比对过 1.21.1 ↔ 1.21.4 的机器纹理），所以只是换名字。
 *
 * 认不出来的路径原样返回（`load()` 只按白名单尺寸收图，多出来的键没人读）。
 * 纯函数，测试直接喂路径。
 */
inline QString legacyKey(const QString &path) {
    const auto tail = [&path](const char *prefix) {
        return path.mid(static_cast<int>(qstrlen(prefix)));
    };
    if (path.startsWith(QLatin1String("entity/equipment/humanoid/"))) {
        const QString name = tail("entity/equipment/humanoid/");
        if (name == QLatin1String("leather_overlay"))
            return QStringLiteral("models/armor/leather_layer_1_overlay");
        if (name == QLatin1String("turtle_scute")) return QStringLiteral("models/armor/turtle_layer_1");
        return QStringLiteral("models/armor/%1_layer_1").arg(name);
    }
    if (path.startsWith(QLatin1String("entity/equipment/humanoid_leggings/"))) {
        const QString name = tail("entity/equipment/humanoid_leggings/");
        if (name == QLatin1String("leather_overlay"))
            return QStringLiteral("models/armor/leather_layer_2_overlay");
        return QStringLiteral("models/armor/%1_layer_2").arg(name);
    }
    if (path.startsWith(QLatin1String("trims/entity/humanoid/")))
        return QStringLiteral("trims/models/armor/%1").arg(tail("trims/entity/humanoid/"));
    if (path.startsWith(QLatin1String("trims/entity/humanoid_leggings/")))
        return QStringLiteral("trims/models/armor/%1_leggings")
            .arg(tail("trims/entity/humanoid_leggings/"));
    if (path == QLatin1String("palettes/trim_base"))
        return QStringLiteral("trims/color_palettes/trim_palette");
    if (path.startsWith(QLatin1String("palettes/trim/")))
        return QStringLiteral("trims/color_palettes/") + tail("palettes/trim/");
    return path;
}

/**
 * @brief 候选排成"新 → 旧"，同一个 jar 只留一次。
 *
 * **不能按版本目录的 mtime 排**。目录 mtime 记的是"哪天装进来的"，
 * 用户机上几十个版本全是同一天解压的，按它取前几个会挑到 1.21 以前的客户端
 * （于是 1.21 的涡流/镶铆、1.21.4 的树脂质在预览里永远缺 —— 用户实测）。
 * 判据是版本发布的 `releaseTime`，由调用方读 json 填进 releasedAt。
 *
 * 纯函数（不碰盘），测试直接喂数据。
 */
inline QList<ArchiveCandidate> newestFirst(QList<ArchiveCandidate> candidates) {
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const ArchiveCandidate &a, const ArchiveCandidate &b) {
                         return a.releasedAt > b.releasedAt;
                     });
    QList<ArchiveCandidate> ordered;
    QSet<QString> seen;
    for (const ArchiveCandidate &candidate : candidates) {
        // 继承型实例的 jar 可能和它主人那一份是同一个文件；同一份只读一次。
        const QString key = QFileInfo(candidate.jar).absoluteFilePath().toLower();
        if (seen.contains(key)) continue;
        seen.insert(key);
        ordered.append(candidate);
    }
    return ordered;
}

// Bounded, read-only extraction in a worker. No JAR execution, no filesystem extraction.
inline Result load(const QString &archive) {
    Result result; result.source = archive;
    const QString script = QStringLiteral(R"PS(
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$path=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%1'))
$zip=[IO.Compression.ZipFile]::OpenRead($path)
try {
  $rows=@(); $total=0
  foreach($entry in $zip.Entries) {
    if($entry.FullName -cnotmatch '^assets/minecraft/textures/(models/armor/[a-z0-9_]+|trims/models/armor/[a-z0-9_]+|trims/color_palettes/[a-z0-9_]+|entity/equipment/humanoid/[a-z0-9_]+|entity/equipment/humanoid_leggings/[a-z0-9_]+|trims/entity/humanoid/[a-z0-9_]+|trims/entity/humanoid_leggings/[a-z0-9_]+|palettes/trim/[a-z0-9_]+|palettes/trim_base)\.png$') { continue }
    $name=$Matches[1]
    if($rows.Count -ge 128 -or $entry.Length -gt 262144) { throw 'Texture limit exceeded' }
    $stream=$entry.Open(); $memory=New-Object IO.MemoryStream
    try {
      $buffer=New-Object byte[] 16384
      while(($n=$stream.Read($buffer,0,$buffer.Length)) -gt 0) {
        $total += $n
        if($memory.Length+$n -gt 262144 -or $total -gt 8388608) { throw 'Expansion limit exceeded' }
        $memory.Write($buffer,0,$n)
      }
      $rows+=@{name=$name;data=[Convert]::ToBase64String($memory.ToArray())}
    } finally { $stream.Dispose(); $memory.Dispose() }
  }
  ConvertTo-Json -InputObject @($rows) -Compress
} finally { $zip.Dispose() }
)PS").arg(QString::fromLatin1(archive.toUtf8().toBase64()));
    QByteArray encoded;
    for (QChar ch : script) { encoded.append(char(ch.unicode() & 255)); encoded.append(char(ch.unicode() >> 8)); }
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; });
#endif
    process.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
        QStringLiteral("-EncodedCommand"), QString::fromLatin1(encoded.toBase64())});
    bool completed = process.waitForStarted(5000);
    QElapsedTimer deadline; deadline.start();
    while (completed && !process.waitForFinished(100)) {
        if (QThread::currentThread()->isInterruptionRequested() || deadline.elapsed() >= 30000) {
            completed = false; break;
        }
    }
    if (!completed) {
        process.kill(); process.waitForFinished(3000);
        result.error = QStringLiteral("读取预览资源超时或无法启动读取器。"); return result;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        result.error = QStringLiteral("无法读取预览资源，归档损坏或资源超过安全限制。"); return result;
    }
    const QByteArray bytes = process.readAllStandardOutput();
    if (bytes.size() > 12 * 1024 * 1024) { result.error = QStringLiteral("资源输出超过安全限制。"); return result; }
    QJsonParseError error;
    const auto rows = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !rows.isArray()) {
        result.error = QStringLiteral("预览资源清单无效。"); return result;
    }
    for (const auto &row : rows.array()) {
        const auto object = row.toObject();
        // 1.21.2+/26.x 的新路径在这里折回旧名；下游（尺寸判据、ArmorTrimScene）只认旧名。
        const auto key = legacyKey(object.value("name").toString());
        QByteArray data = QByteArray::fromBase64(object.value("data").toString().toLatin1());
        QBuffer buffer(&data); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "png"); reader.setAutoTransform(false);
        const bool palette = key.startsWith(QStringLiteral("trims/color_palettes/"));
        if (reader.size() != (palette ? QSize(8, 1) : QSize(64, 32))) continue;
        const QImage image = reader.read();
        if (!image.isNull()) result.images.insert(key, image.convertToFormat(QImage::Format_ARGB32));
    }
    if (!result.images.contains(QStringLiteral("models/armor/iron_layer_1"))
        || !result.images.contains(QStringLiteral("trims/color_palettes/trim_palette"))) {
        result.images.clear();
        result.error = QStringLiteral("未找到受支持的盔甲纹理。请选择 Java 1.20 及以上、带原版纹理的客户端，或下载预览资源。");
    }
    return result;
}

}

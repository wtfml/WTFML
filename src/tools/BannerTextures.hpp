#pragma once
#include <QBuffer>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

namespace BannerTextures {
struct Result { QHash<QString, QImage> images; QString error; };
// 只读本地归档的指定 PNG，不解压到用户目录，不执行 JAR。工作线程调用。
inline Result load(const QString &archive) {
    Result result;
    const QString script = QStringLiteral(R"PS(
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$path=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%1'))
$zip=[IO.Compression.ZipFile]::OpenRead($path)
try {
  $rows=@(); $total=0
  foreach($entry in $zip.Entries) {
    if($entry.FullName -cnotmatch '^assets/minecraft/textures/entity/banner/([a-z0-9_]+)\.png$') { continue }
    $name=$Matches[1]
    if($rows.Count -ge 96 -or $entry.Length -gt 2097152) { throw 'Banner texture limit exceeded' }
    $textureStream=$entry.Open(); $memory=New-Object IO.MemoryStream
    try {
      $buffer=New-Object byte[] 65536
      while(($n=$textureStream.Read($buffer,0,$buffer.Length)) -gt 0) {
        $total += $n
        if($memory.Length+$n -gt 2097152 -or $total -gt 16777216) { throw 'Texture expansion limit exceeded' }
        $memory.Write($buffer,0,$n)
      }
      $rows+=@{name=$name;data=[Convert]::ToBase64String($memory.ToArray())}
    } finally { $textureStream.Dispose(); $memory.Dispose() }
  }
  ConvertTo-Json -InputObject @($rows) -Compress
} finally { $zip.Dispose() }
)PS").arg(QString::fromLatin1(archive.toUtf8().toBase64()));
    QByteArray encoded;
    for (QChar ch : script) { encoded.append(char(ch.unicode() & 255)); encoded.append(char(ch.unicode() >> 8)); }
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; }); // CREATE_NO_WINDOW
#endif
    process.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
        QStringLiteral("-EncodedCommand"), QString::fromLatin1(encoded.toBase64())});
    if (!process.waitForStarted(5000) || !process.waitForFinished(30000)) {
        process.kill(); process.waitForFinished(3000);
        result.error = QStringLiteral("读取旗帜贴图超时或无法启动读取器。"); return result;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        result.error = QStringLiteral("无法读取归档中的旗帜贴图，或贴图超过安全限制。"); return result;
    }
    const auto rows = QJsonDocument::fromJson(process.readAllStandardOutput()).array();
    for (const auto &row : rows) {
        const auto object = row.toObject();
        QByteArray data = QByteArray::fromBase64(object.value("data").toString().toLatin1());
        QBuffer buffer(&data); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "png");
        const QSize size = reader.size();
        if (size.width() < 64 || size.width() > 2048 || size.height() != size.width() || size.width() % 64 != 0) continue;
        const auto image = reader.read();
        if (!image.isNull()) result.images.insert(object.value("name").toString(), image.scaled(64, 64));
    }
    if (result.images.isEmpty()) result.error = QStringLiteral("没有找到受支持的 Java 旗帜贴图，请选择原版客户端 JAR 或包含旗帜贴图的资源包。");
    return result;
}
}

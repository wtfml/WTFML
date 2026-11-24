/**
 * @file ZipUtil.cpp
 * @brief ZipUtil 的实现。
 * @note 用 PowerShell + .NET System.IO.Compression 解压，一次进程调用可处理
 * 任意多个压缩包；脚本经 Base64 编码后交给 -EncodedCommand，绕开
 * cmd/PowerShell 两层的引号与转义问题。
 * @note `-EncodedCommand` 换来的是**命令行长度上限**：整个脚本只能占一万二千来个字符
 * （32767 的命令行 ÷ Base64 的 4/3 ÷ UTF-16LE 的 2）。所以脚本里**只放常量**，
 * 随数据长大的东西（打包那张条目表）一律走临时文件 —— 见 createZip 里那段。
 */
#include "ZipUtil.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include "PowerShell.hpp"
#include <QProcess>
#include <QUuid>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QSet>

namespace {

/**
 * @brief 把字符串包成 PowerShell 单引号字面量。
 * @param value 原始字符串。
 * @return 返回已转义的 PowerShell 字面量（含外层单引号）。
 */
QString psQuote(const QString &value) {
    QString escaped = value;
    // PowerShell 单引号字符串里，单引号本身要写成两个
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// PowerShell/.NET does not inherit our exe's longPathAware manifest.
// Use an absolute device path for filesystem I/O, never for ZIP entry names.
QString processIoPath(const QString &path) {
    const QString native = QDir::toNativeSeparators(path);
#ifdef Q_OS_WIN
    if (native.startsWith(QStringLiteral("\\\\?\\"))) return native;
    const QString full = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    return full.startsWith(QStringLiteral("\\\\"))
        ? QStringLiteral("\\\\?\\UNC\\") + full.mid(2) : QStringLiteral("\\\\?\\") + full;
#else
    return native;
#endif
}

QString psFilePath(const QString &path) { return psQuote(processIoPath(path)); }

QStringList processIoPaths(const QStringList &paths) {
    QStringList result;
    result.reserve(paths.size());
    for (const auto &path : paths) result.append(processIoPath(path));
    return result;
}

/**
 * @brief 把字符串列表转成 PowerShell 数组字面量。
 * @param values 原始字符串列表。
 * @return 返回形如 @('a','b') 的字面量。
 */
QString psArray(const QStringList &values) {
    QStringList quoted;
    quoted.reserve(values.size());
    for (const QString &value : values) {
        quoted << psQuote(value);
    }
    return QStringLiteral("@(") + quoted.join(QLatin1Char(',')) + QLatin1Char(')');
}

/**
 * @brief 按 -EncodedCommand 的要求编码脚本。
 * @param script 脚本正文。
 * @return 返回 UTF-16LE + Base64 的字符串。
 */
QString encodeCommand(const QString &script) {
    // Catch setup failures too (e.g. destination-directory creation before the
    // operation's own try). Otherwise CLIXML filtering hides the actual cause.
    const QString guarded = QStringLiteral("$ErrorActionPreference='Stop'\ntry {\n") + script +
        QStringLiteral("\n} catch { [Console]::Error.WriteLine($_.Exception.GetBaseException().Message); exit 1 }\n");
    // -EncodedCommand 要求 Base64(UTF-16LE)，这里手工拼一遍，不依赖主机字节序
    QByteArray utf16le;
    utf16le.reserve(guarded.size() * 2);
    for (const QChar ch : guarded) {
        utf16le.append(static_cast<char>(ch.unicode() & 0xFF));
        utf16le.append(static_cast<char>((ch.unicode() >> 8) & 0xFF));
    }
    return QString::fromLatin1(utf16le.toBase64());
}

/**
 * @brief waitForFinished() 返回 false 之后，到底发生了什么。
 * @param failure process.error()（**必须在 kill 之前读**，kill 之后它可能已经变了）。
 * @param detail process.errorString()。
 * @param what 这次动作的名字（「解压」/「读整合包清单」），拼进句子里。**由调用方给**：这几句
 * 里有一部分是直接弹给用户看的，主语写错了就是一句说不通的话（见 readEntry 的 what）。
 * @return 返回给用户看的一句话。
 *
 * NOTE: 超时**不是**唯一一种"waitForFinished() 给 false"。进程压根没起来（PowerShell
 * 不存在、被安全软件拦下）也是 false。三个调用点都要按 `error()` 分开报，不能一律报
 * 「解压超时」—— 用户和排查的人会往网络/磁盘上找，而真因在"进程没起来"那一头。
 */
QString describeProcessFailure(QProcess::ProcessError failure, const QString &detail,
                               const QString &what) {
    switch (failure) {
    case QProcess::FailedToStart:
        return QStringLiteral("%1没跑起来（%2）。").arg(what, detail);
    case QProcess::Crashed:
        return QStringLiteral("%1时进程被强行结束了（%2）。").arg(what, detail);
    default:
        return QStringLiteral("%1超时").arg(what);
    }
}

/**
 * @brief 从 PowerShell 的错误输出里挑一句能给用户看的话。
 * @param raw runPowershell() 写进 errorOut 的东西。
 * @return 挑出来的那一句；**一句都挑不出来时返回空串**，让调用方用它自己那句兜底
 * （宁可少说，也不要把 `#< CLIXML` 这种内部形状弹到用户脸上）。
 *
 * #< CLIXML
 * The compression mode specified in GZip header is unknown.
 * <Objs Version="1.1.0.1" xmlns="...">...</Objs>
 *
 * —— **人话夹在中间那两行机器话之间**。只取第一行得到的是 `#< CLIXML` 七个字符
 * （等于什么也没说）；只判「头一行是不是 CLIXML 就整段丢掉」会把真因一起丢掉
 * （脚本里加了 catch、原因也写出来了，用户看到的却还是兜底那句话，原因就在这儿）。
 * 所以按行过一遍，把机器话那两种行跳过，取第一条活下来的。
 * NOTE: 脚本 catch 里那句是 `[Console]::Error.WriteLine()` 写的，走**原生 stderr**、
 * 不经过 PowerShell 的错误流序列化，所以它自己能干净地占一行 —— 上面那个形状
 * 就是这么来的。没被 catch 住的错则整个躺在 `<Objs>` 的那条 XML 里（换行写成
 * `_x000D__x000A_` 那种转义），那种**不解**：解出来是给排查的人看的，不是给用户
 * 看的；那种形态只说明脚本自己就没跑起来，弹给用户的框里说不了。
 */
QString readProcessError(const QString &raw) {
    const QStringList lines = raw.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString text = line.trimmed();
        // 机器话那两种：CLIXML 的头，和序列化出来的那条 XML（整条就一行）。
        if (text.isEmpty() || text == QLatin1String("#< CLIXML") ||
            text.startsWith(QLatin1String("<Objs"))) {
            continue;
        }
        QString human = text;
        human.replace(QStringLiteral("\\\\?\\UNC\\"), QStringLiteral("\\\\"), Qt::CaseInsensitive);
        human.replace(QStringLiteral("\\\\?\\"), QString());
        return human;
    }
    return QString();
}

/// 起一次 PowerShell 跑这段脚本。返回 false 时 errorOut 里是原因。
///
/// NOTE: 三个 - 号开头的开关一个都不能少（-NoProfile
/// 挡用户 profile 里的别名、-ExecutionPolicy Bypass 挡执行策略、-EncodedCommand
/// 绕开两层引号转义）。四个调用点共用这一份，免得哪天只给其中一个加了新开关。
/// NOTE: `what` 是**这次动作的名字**，由调用方给 —— 这几句是要弹给用户看的，主语写错就是
/// 一句说不通的话：打包那一条若写成「**解压**没跑起来」，用户拿着这句话往解压上找，
/// 而全程根本没解压。
bool runPowershell(const QString &script, int timeoutMs, QByteArray *stdoutOut,
                   QString *errorOut, const QString &what,
                   const std::function<bool()> &cancelled = {},
                   const std::function<void(const QByteArray &)> &onStdoutLine = {}) {
    if (cancelled && cancelled()) {
        if (errorOut) *errorOut = QStringLiteral("%1已取消或超时。").arg(what);
        return false;
    }
    const QString encoded = encodeCommand(script);
    // 兜底闸：Windows 一条命令行只有 32767 个字符，超了 CreateProcess 直接失败，QProcess
    // 报的是「文件名或扩展名太长」（ERROR_FILENAME_EXCED_RANGE）—— 那句话和真正的原因
    // （脚本太长）一个字都对不上。这里自己先拦一道，
    // 把它变成一句说得通的话。
    //
    // NOTE: 撞到这一道说明**我们自己的脚本里放了跟着数据长大的东西**，那是写错了，
    // 不是用户在做什么奇怪的事：脚本正文里只该有常量，实例相关的东西（打包那张
    // 条目表）走临时文件 —— 见 createZip。3 万这个数留了七百来个字符给
    // `powershell` 那几个开关和引号 —— 编码后的正文是 ASCII，字节数就是字符数。
    if (encoded.size() > 32000) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("%1失败：递给 PowerShell 的脚本太长了（编码后 %2 个字符，"
                                       "Windows 一条命令行最多三万多）。")
                            .arg(what)
                            .arg(encoded.size());
        }
        return false;
    }
    QProcess process;
    process.start(PowerShell::executable(),
                  {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                   QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                   QStringLiteral("-EncodedCommand"), encoded});
    bool finished = false;
    bool interrupted = false;
    QByteArray stdoutBuf;
    if (!cancelled && !onStdoutLine) {
        finished = process.waitForFinished(timeoutMs);
    } else {
        const auto drainStdout = [&]() {
            if (!onStdoutLine) {
                return;
            }
            stdoutBuf += process.readAllStandardOutput();
            while (true) {
                const int nl = stdoutBuf.indexOf('\n');
                if (nl < 0) {
                    break;
                }
                const QByteArray line = stdoutBuf.left(nl).trimmed();
                stdoutBuf.remove(0, nl + 1);
                if (!line.isEmpty()) {
                    onStdoutLine(line);
                }
            }
        };
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < timeoutMs) {
            if (cancelled && cancelled()) { interrupted = true; break; }
            if (process.waitForFinished(50)) {
                finished = true;
                drainStdout();
                break;
            }
            drainStdout();
            if (process.state() == QProcess::NotRunning) break;
        }
        if (finished && onStdoutLine && !stdoutBuf.trimmed().isEmpty()) {
            onStdoutLine(stdoutBuf.trimmed());
            stdoutBuf.clear();
        }
    }
    if (!finished) {
        // error() / errorString() 要在 kill 之前读：kill 会把状态改成 Crashed。
        const QProcess::ProcessError failure = process.error();
        const QString detail = process.errorString();
        process.kill();
        process.waitForFinished(5000);
        if (errorOut != nullptr) {
            *errorOut = interrupted ? QStringLiteral("%1已取消或超时。").arg(what)
                                    : describeProcessFailure(failure, detail, what);
        }
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorOut != nullptr) {
            // NOTE: fromLocal8Bit 是对的，**不要**跟着游戏进程改成 fromUtf8 —— 这里的
            // 输出来自 PowerShell，不是 JVM，-Dstdout.encoding 管不到它（见
            // LauncherManager::decodeGameOutput 的说明）。
            //
            // 这一段**不能直接交给用户看**：stderr 被重定向时 PowerShell 会把错误流
            // 序列化成 CLIXML（见 readProcessError() 上面那段），原文的头一行是
            // `#< CLIXML` 七个字符。三个调用点全都走这里，所以过滤放在这一层 ——
            // 调用方拿到的永远是一句人话（挑不出来时才回它自己那句兜底的）。
            const QString raw = QString::fromLocal8Bit(process.readAllStandardError());
            const QString err = readProcessError(raw);
            *errorOut = err.isEmpty()
                            ? QStringLiteral("PowerShell 退出码 %1").arg(process.exitCode())
                            : err;
        }
        return false;
    }
    if (stdoutOut != nullptr) {
        *stdoutOut = process.readAllStandardOutput();
    }
    return true;
}

}  // namespace

bool ZipUtil::extractArchives(const QStringList &archives, const QString &targetDir,
                              const QStringList &excludePrefixes, QString *errorOut,
                              const std::function<void(int done, int total)> &progress) {
    if (archives.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("没有需要解压的压缩包");
        }
        return false;
    }

    if (!QDir().mkpath(targetDir)) {
        if (errorOut) {
            *errorOut = QStringLiteral("无法创建目录：%1").arg(targetDir);
        }
        return false;
    }

    QStringList skipConditions;
    // 防 zip slip。这几条和 extractSubtrees() 里那组**一样严**（三条判据不能只剩 `..`，
    // 否则漏掉"条目名本身就是绝对路径"那种 —— 见下面 NOTE）。正常 Minecraft jar 不会有
    // 这种条目，但解压工具没理由不设防。
    //
    // NOTE: 三条各自的来路：
    // - `..`：翻到 $out 上面去（`../x`、`a/../../x`，`\` 分隔也一样）；
    // - 头一个字符是 `/`、或者名字里带 `:`：.NET 的 Path.Combine 认为第二个参数已经是
    // 绝对路径（`/x`、`\x`、`C:/x` 都算）→ **把 $out 丢掉**，文件写到目标目录外面；
    // - 判的必须是**归一过 `\`→`/` 的名字**：Windows 上 `\` 同样是分隔符，不归一的话
    // `META-INF\MANIFEST.MF` 这类条目还会从 excludePrefixes 底下溜过去。
    const QString fullName = QStringLiteral("$e.FullName.Replace('\\','/')");
    // `..` 判的是**路径分量**，不是子串。`.Contains('..')` 会把名字里
    // 正好带两个点的**正常文件**一并丢掉 —— `mods/foo..bar.jar`、`assets/a..b.png` 都是
    // 合法名字，而这里的丢弃是**静默的**（`continue`，不报错、不计入任何清单），于是用户
    // 看到的是"整合包导完了但少东西"。判分量之后 `..` 仍然拦得住（`../x` 拆出来第一段
    // 就是 `..`），而 `a..b` 拆出来是一段 `a..b`、不等于 `..`，放行。
    skipConditions << QStringLiteral("(%1.Split('/') -contains '..')").arg(fullName);
    skipConditions << QStringLiteral("(%1 -match '(^|/)([^/]*[. ])?(/|$)')").arg(fullName);
    skipConditions << QStringLiteral("%1.StartsWith('/')").arg(fullName);
    skipConditions << QStringLiteral("%1.Contains(':')").arg(fullName);
    // NOTE: exclude 这几条**区分大小写**（.NET 的 StartsWith(string) 就是区分大小写的），
    // 和 extractSubtrees() 里那条**前缀**比对不一样 —— 那边是
    // `$full.ToLowerInvariant().StartsWith($r.ToLowerInvariant())`，故意不区分
    // （真实整合包里 `Overrides/` / `overrides/` 两种写法都有，认不出来就会一条都
    // 不解压，而那种失败是静默的）。这里不跟着放宽，是因为排的是 `META-INF/` 这种
    // 规范拼法：放宽只会多丢几个本来也无害的条目，收紧反而可能漏掉签名的目录。
    // 上面那句"和 extractSubtrees() 里那组一样严"说的是**三条防穿越的判据**，
    // 前缀比对这一条两边口径不同，这里写明白，别让读的人以为它们逐字一样。
    for (const QString &prefix : excludePrefixes) {
        const QString normalized = prefix.endsWith(QLatin1Char('/')) ? prefix : prefix + QLatin1Char('/');
        skipConditions << QStringLiteral("%1.StartsWith(%2)").arg(fullName, psQuote(normalized));
    }

    // 用 .NET 的 Path/Directory 而不是 PowerShell 的 Join-Path / New-Item：
    // 前者纯字符串运算，不会被 PowerShell provider 把路径"解析"成别的形状。
    //
    // ★ 解压后的大小 / 压缩比上限（R10）：这一条收的是**外部来的** zip（整合包、恢复备份），
    // 而条目声明的大小是压缩包里写着的、不可信的数。没有上限时一个几百 KB 的 zip bomb
    // 就能把盘写满（超时之后还留下半成品）。三条判据：
    // · 单条解压后 ≤ 2 GiB —— 正常模组 / 资源包 / 光影里最大的单个文件也就几百 MB；
    // · 压缩比 ≤ 1000（只判 > 1 MiB 的条目）—— 已经压缩过的游戏资源通常在 1–20 之间，
    //   1000 是以文本为主的包都给不到的倍数，而 zip bomb 动辄几万、几十万；
    // · 一批累计 ≤ 16 GiB —— 几百个模组的整合包也就几个 GB。
    const QString script =
        QStringLiteral("$ErrorActionPreference='Stop'\n"
                       "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                       "$out=%1\n"
                       "[void][System.IO.Directory]::CreateDirectory($out)\n"
                       "$total=[long]0\n"
                       "foreach($z in %2){\n"
                       "  $zip=$null\n"
                       "  try{\n"
                       "    $zip=[System.IO.Compression.ZipFile]::OpenRead($z)\n"
                       "    foreach($e in $zip.Entries){\n"
                       "      if([string]::IsNullOrEmpty($e.Name)){continue}\n"
                       "      $full=$e.FullName.Replace('\\','/')\n"
                       "      if(%3){continue}\n"
                       "      if($e.Length -gt 2147483648){throw ('「' + $full + '」解压后超过 2 GiB 上限，按压缩炸弹拦下了')}\n"
                       "      if($e.Length -gt 1048576 -and ($e.Length / [Math]::Max($e.CompressedLength,1)) -gt 10000){throw ('「' + $full + '」的压缩比超过 10000，按压缩炸弹拦下了')}\n"
                       "      if(($total + $e.Length) -gt 17179869184){throw '整包解压后累计超过 16 GiB 上限，按压缩炸弹拦下了'}\n"
                       "      $dest=[System.IO.Path]::Combine($out,$full).Replace('/','\\')\n"
                       "      [void][System.IO.Directory]::CreateDirectory"
                       "([System.IO.Path]::GetDirectoryName($dest))\n"
                       "      [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e,$dest,$true)\n"
                       "      $total=$total+$e.Length\n"
                       "    }\n"
                       "    [Console]::Out.WriteLine('__WTFML_ARCHIVE_DONE__')\n"
                       "    [Console]::Out.Flush()\n"
                       "  } catch {\n"
                       // 这一段 catch 是**必须的**，理由和 gunzipTo / createZip
                       // 里那两段一模一样（见 readProcessError 上面那段）：没兜住的话 PowerShell
                       // 会把错误流序列化成 `#< CLIXML` + 一整条 `<Objs>` XML，而真因（路径太长、
                       // 目标被占用、包是半截下载）**就埋在那条 XML 里**，readProcessError 按行
                       // 过滤时连同机器话一起跳过 ⇒ 用户拿到的是「解压失败：PowerShell 退出码 1」，
                       // 一个字都说不出是哪个包怎么了 —— 而这一条是要弹给他看的。
                       // GetBaseException() 取最里面那一层（真因在包装里，和那两段同一个理由）。
                       // 退出码 3 和 gunzipTo / createZip 保持一致（"脚本跑到了、里面出错了"，
                       // 和"进程没起来/超时"分开）。
                       "    $msg=$_.Exception.GetBaseException().Message\n"
                       "    if([string]::IsNullOrEmpty($msg)){ $msg=$_.Exception.Message }\n"
                       "    [Console]::Error.WriteLine(('解压「' + $z + '」时出错：' + $msg))\n"
                       "    exit 3\n"
                       "  } finally { if($zip){ $zip.Dispose() } }\n"
                       "}\n")
            .arg(psFilePath(QDir(targetDir).absolutePath()), psArray(processIoPaths(archives)),
                 QStringLiteral("(") + skipConditions.join(QStringLiteral(" -or ")) + QLatin1Char(')'));

    const int totalArchives = int(archives.size());
    int doneArchives = 0;
    if (progress) {
        progress(0, totalArchives);
    }
    const std::function<void(const QByteArray &)> onLine =
        progress ? std::function<void(const QByteArray &)>(
                       [&](const QByteArray &line) {
                           if (line == "__WTFML_ARCHIVE_DONE__") {
                               ++doneArchives;
                               progress(qMin(doneArchives, totalArchives), totalArchives);
                           }
                       })
                 : std::function<void(const QByteArray &)>{};

    QString failure;
    if (!runPowershell(script, 180000, nullptr, &failure, QStringLiteral("解压"), {}, onLine)) {
        if (errorOut) {
            *errorOut = failure.isEmpty()
                            ? QStringLiteral("解压失败（%1 个压缩包）").arg(archives.size())
                            : failure;
        }
        return false;
    }
    return true;
}

QHash<QString, ZipUtil::ArchiveMetadata> ZipUtil::readMetadataBatch(
    const QStringList &archives, const QStringList &entryNames, QString *problem,
    const std::function<bool()> &cancelled) {
    QHash<QString, ArchiveMetadata> result;
    if (problem) problem->clear();
    if (archives.isEmpty()) return result;
    if (archives.size() > 10000 || entryNames.size() > 16) {
        if (problem) *problem = QStringLiteral("元数据批次过大，未读取。");
        return result;
    }
    QTemporaryDir scratch;
    const QString input = scratch.filePath("input.json"), output = scratch.filePath("output.jsonl");
    QFile manifest(input);
    QJsonArray archivePaths;
    for (const auto &path : archives)
        archivePaths.append(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("ioPath"), processIoPath(path)}});
    const QByteArray bytes = QJsonDocument(QJsonObject{{"archives", archivePaths},
        {"entries", QJsonArray::fromStringList(entryNames)}}).toJson(QJsonDocument::Compact);
    if (!scratch.isValid() || !manifest.open(QIODevice::WriteOnly) || manifest.write(bytes) != bytes.size()) {
        if (problem) *problem = QStringLiteral("无法准备批量元数据读取。");
        return result;
    }
    manifest.close();
    const QString script = QStringLiteral(R"PS(
$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$inputData=[IO.File]::ReadAllText(%1,[Text.Encoding]::UTF8) | ConvertFrom-Json
$writer=[IO.StreamWriter]::new(%2,$false,[Text.UTF8Encoding]::new($false))
$watch=[Diagnostics.Stopwatch]::StartNew(); [long]$total=0
try {
 foreach($archivePath in $inputData.archives) {
  $path=$archivePath.path
  $row=@{path=$path; entries=@{}; problems=@{}; problem=''}; $zip=$null
  if($watch.Elapsed.TotalSeconds -gt 55) {$row.problem='未检查：本轮读取预算已用完'}
  else {
   try {
    $zip=[IO.Compression.ZipFile]::OpenRead($archivePath.ioPath)
    foreach($name in $inputData.entries) {
     $entry=$null
     if($name -eq '@wtfml-icon') {
      $candidates=[Collections.Generic.List[string]]::new()
      foreach($meta in @('fabric.mod.json','quilt.mod.json','mcmod.info','META-INF/neoforge.mods.toml','META-INF/mods.toml')) {
       $declaration=$zip.GetEntry($meta)
       if(!$declaration -or $declaration.Length -gt 1048576){continue}
       $reader=$null
       try {
        $reader=[IO.StreamReader]::new($declaration.Open())
        $chars=New-Object char[] 1048577; $length=$reader.ReadBlock($chars,0,$chars.Length)
        if($length -gt 1048576){continue}; $text=[string]::new($chars,0,$length)
        if($meta.EndsWith('.toml')) {
         foreach($match in [regex]::Matches($text,'(?m)^\s*logoFile\s*=\s*["'']([^"'']+)["'']')) {$candidates.Add($match.Groups[1].Value)}
        } else {
         $json=ConvertFrom-Json -InputObject $text
         $values=@()
         if($meta -eq 'fabric.mod.json') {$values=@($json.icon)}
         elseif($meta -eq 'quilt.mod.json') {$values=@($json.quilt_loader.metadata.icon)}
         else {if($json.modList){$json=$json.modList}; foreach($mod in $json){$values+=@($mod.logoFile)}}
         foreach($value in $values) {
          if($value -is [string]) {$candidates.Add($value)}
          elseif($value) {foreach($property in ($value.PSObject.Properties | Sort-Object {[int]$_.Name} -Descending)) {if($property.Value -is [string]){$candidates.Add($property.Value)}}}
         }
        }
       } catch {} finally {if($reader){$reader.Dispose()}}
      }
      $candidates.Add('pack.png'); $candidates.Add('icon.png'); $candidates.Add('logo.png')
      foreach($candidate in $candidates) {
       $candidate=$candidate.Replace('\','/').TrimStart('/')
       if($candidate.Split('/') -contains '..'){continue}
       $entry=$zip.GetEntry($candidate)
       if($entry -and $entry.Length -le 1048576){break}; $entry=$null
      }
     } else {$entry=$zip.GetEntry($name)}
     if($null -eq $entry){continue}
     $stream=$null; $memory=$null
     try {
      if($entry.Length -gt 1048576){throw '声明超过 1 MiB 限制'}
      $stream=$entry.Open(); $memory=[IO.MemoryStream]::new(); $buffer=New-Object byte[] 65536
      while(($count=$stream.Read($buffer,0,$buffer.Length)) -gt 0) {
       if(($memory.Length+$count) -gt 1048576 -or ($total+$count) -gt 16777216){throw '声明超过批量读取限制'}
       $memory.Write($buffer,0,$count); $total+=$count
      }
      $row.entries[$name]=[Convert]::ToBase64String($memory.ToArray())
     } catch {$row.problems[$name]=$_.Exception.Message}
     finally {if($stream){$stream.Dispose()}; if($memory){$memory.Dispose()}}
    }
   } catch {$row.problem=$_.Exception.Message}
   finally {if($zip){$zip.Dispose()}}
  }
  $writer.WriteLine((ConvertTo-Json -InputObject $row -Depth 5 -Compress)); $writer.Flush()
 }
} finally {$writer.Dispose()}
)PS").arg(psFilePath(input), psFilePath(output));
    QByteArray ignored;
    runPowershell(script, 60000, &ignored, problem, QStringLiteral("批量读取模组声明"), cancelled);
    QFile file(output);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) return result;
    const QSet<QString> requested(archives.cbegin(), archives.cend());
    while (!file.atEnd()) {
        QJsonParseError error;
        const auto object = QJsonDocument::fromJson(file.readLine(32 * 1024 * 1024), &error).object();
        const QString path = object.value("path").toString();
        if (error.error != QJsonParseError::NoError || !requested.contains(path)) continue;
        ArchiveMetadata metadata;
        metadata.problem = object.value("problem").toString();
        const auto entries = object.value("entries").toObject();
        const auto problems = object.value("problems").toObject();
        for (const auto &name : entryNames) {
            if (entries.contains(name)) {
                const auto decoded = QByteArray::fromBase64(entries.value(name).toString().toLatin1());
                if (decoded.size() <= 1024 * 1024) metadata.entries.insert(name, decoded);
                else metadata.entryProblems.insert(name, QStringLiteral("声明超过 1 MiB 限制"));
            }
            if (problems.contains(name)) metadata.entryProblems.insert(name, problems.value(name).toString());
        }
        result.insert(path, metadata);
    }
    return result;
}

QHash<QString, QByteArray> ZipUtil::readIconsBatch(const QStringList &archives) {
    QHash<QString, QByteArray> result;
    const auto metadata = readMetadataBatch(archives, {QStringLiteral("@wtfml-icon")});
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it) {
        const auto bytes = it.value().entries.value(QStringLiteral("@wtfml-icon"));
        if (!bytes.isEmpty()) result.insert(it.key(), bytes);
    }
    return result;
}

ZipUtil::EntryStatus ZipUtil::readEntry(const QString &archive, const QString &entryName,
                                        QByteArray *out, QString *errorOut, const QString &what) {
    // 默认重载**也带上限**（R11）：以前是 -1（不设上限），而它的调用方全是"读一份小的
    // 元数据 / 贴图 / class 头"—— 一个畸形包里放一个几 GB 的同名条目，临时目录和内存
    // 就会一起涨（每次启动扫描还会重演）。16 MiB 对这些用途绰绰有余；真需要读更大的
    // 就自己传 maximumBytes，或者用 extractEntryToFile()（那条不读进内存）。
    return readEntry(archive, entryName, out, errorOut, what, 16LL * 1024 * 1024);
}

ZipUtil::EntryStatus ZipUtil::readEntry(const QString &archive, const QString &entryName,
                                        QByteArray *out, QString *errorOut, const QString &what, qint64 maximumBytes) {
    if (out != nullptr) {
        out->clear();
    }
    if (errorOut != nullptr) {
        errorOut->clear();
    }

    // 条目先落到一个临时文件再读回来，而不是让 PowerShell 把字节打到 stdout：
    // stdout 是**文本**通道，经过一次编码转换之后，二进制内容（zip 里的 json 是 UTF-8 的
    // 字节流）就不再是原来的字节了。临时文件没有这一层。
    const QString tempPath = QDir::tempPath() + QStringLiteral("/wtfml-entry-") +
                             QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray temp = QFile::encodeName(tempPath);

    // 退出码：3 = 没有这个条目，4 = 打不开（不是 zip / 坏了）。两个都要和"别的失败"分开。
    const QString script =
        QStringLiteral("$ErrorActionPreference='Stop'\n"
                       "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                       "$target=%1\n"
                       "try{ $zip=[System.IO.Compression.ZipFile]::OpenRead(%2) }"
                       " catch { exit 4 }\n"
                       "try{\n"
                       "  $e=$zip.GetEntry($target)\n"
                       "  if($e -eq $null){\n"
                       "    $lower=$target.ToLowerInvariant()\n"
                       "    foreach($c in $zip.Entries){\n"
                       "      if($c.FullName.ToLowerInvariant() -eq $lower){ $e=$c; break }\n"
                       "    }\n"
                       "  }\n"
                       "  if($e -eq $null){ exit 3 }\n"
                       "  $limit=%4\n"
                       "  if($limit -gt 0){\n"
                       "    if($e.Length -gt $limit){ exit 5 }\n"
                       "    $inputStream=$e.Open(); $outputStream=[System.IO.File]::Create(%3)\n"
                       "    try{ $buffer=New-Object byte[] 65536; $total=0L\n"
                       "      while(($n=$inputStream.Read($buffer,0,$buffer.Length)) -gt 0){\n"
                       "        $total+=$n; if($total -gt $limit){ exit 5 }; $outputStream.Write($buffer,0,$n)\n"
                       "      }\n"
                       "    } finally { $inputStream.Dispose(); $outputStream.Dispose() }\n"
                       "  } else { [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e,%3,$true) }\n"
                       "} finally { $zip.Dispose() }\n")
            // 四个占位符**一次给全**。分两次 `.arg()` 的话，第二次会重扫已经插入进去的
            // 内容：条目名是从压缩包里读来的（第三方可控），名字里带一个 `%4` 就会被替换成
            // 字节上限，`$target` 悄悄变成别的名字，症状只是"这个条目找不到"。
            .arg(psQuote(entryName), psFilePath(archive),
                 psFilePath(QFile::decodeName(temp)), QString::number(maximumBytes));

    QProcess process;
    process.start(PowerShell::executable(),
                  {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                   QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                   QStringLiteral("-EncodedCommand"), encodeCommand(script)});
    if (!process.waitForFinished(60000)) {
        // 同上：先读 error()/errorString()，再 kill。
        const QProcess::ProcessError failure = process.error();
        const QString detail = process.errorString();
        process.kill();
        process.waitForFinished(5000);
        QFile::remove(tempPath);
        if (errorOut != nullptr) {
            *errorOut = describeProcessFailure(failure, detail, what);
        }
        return EntryStatus::Failed;
    }
    const int code = process.exitCode();
    const QByteArray stderrBytes = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit) {
        QFile::remove(tempPath);
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("%1时 PowerShell 被强行结束了").arg(what);
        }
        return EntryStatus::Failed;
    }
    if (code == 3) {
        QFile::remove(tempPath);
        return EntryStatus::NotFound;
    }
    if (code == 4) {
        QFile::remove(tempPath);
        return EntryStatus::NotAnArchive;
    }
    if (code == 5) {
        QFile::remove(tempPath);
        if (errorOut) *errorOut = QStringLiteral("%1：声明超过读取大小上限，未采用。").arg(what);
        return EntryStatus::Failed;
    }
    if (code != 0) {
        QFile::remove(tempPath);
        if (errorOut != nullptr) {
            const QString err = readProcessError(QString::fromLocal8Bit(stderrBytes));
            *errorOut = err.isEmpty() ? QStringLiteral("%1失败（退出码 %2）").arg(what).arg(code) : err;
        }
        return EntryStatus::Failed;
    }

    QFile file(tempPath);
    const bool read = file.open(QIODevice::ReadOnly);
    const QByteArray bytes = read ? file.readAll() : QByteArray();
    file.close();
    QFile::remove(tempPath);

    if (!read) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("读不出临时文件 %1").arg(tempPath);
        }
        return EntryStatus::Failed;
    }
    if (out != nullptr) {
        *out = bytes;
    }
    return EntryStatus::Ok;
}

ZipUtil::EntryStatus ZipUtil::extractEntryToFile(const QString &archive, const QString &entryName,
                                                 const QString &targetPath, QString *errorOut,
                                                 const QString &what) {
    if (errorOut != nullptr) {
        errorOut->clear();
    }

    // 走文件通道而不是 stdout，理由和 readEntry 那段一样（二进制不过文本通道）。
    // NOTE: 这里**没有**大小上限 —— 调用方（PCL 那种压缩包里套一份 .mrpack）要的是完整
    // 解出来一份真文件，不是读进内存。上限那一路留给 readEntry。
    // 退出码和 readEntry 同一套：3 = 没有这个条目，4 = 打不开。
    const QString script =
        QStringLiteral("$ErrorActionPreference='Stop'\n"
                       "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                       "$target=%1\n"
                       "try{ $zip=[System.IO.Compression.ZipFile]::OpenRead(%2) }"
                       " catch { exit 4 }\n"
                       "try{\n"
                       "  $e=$zip.GetEntry($target)\n"
                       "  if($e -eq $null){\n"
                       "    $lower=$target.ToLowerInvariant()\n"
                       "    foreach($c in $zip.Entries){\n"
                       "      if($c.FullName.ToLowerInvariant() -eq $lower){ $e=$c; break }\n"
                       "    }\n"
                       "  }\n"
                       "  if($e -eq $null){ exit 3 }\n"
                       "  $dest=%3\n"
                       "  $dir=[System.IO.Path]::GetDirectoryName($dest)\n"
                       "  if(-not [string]::IsNullOrEmpty($dir)){"
                       "[void][System.IO.Directory]::CreateDirectory($dir) }\n"
                       "  [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e,$dest,$true)\n"
                       "} catch {\n"
                       "  $msg=$_.Exception.GetBaseException().Message\n"
                       "  if([string]::IsNullOrEmpty($msg)){ $msg=$_.Exception.Message }\n"
                       "  [Console]::Error.WriteLine($msg)\n"
                       "  exit 1\n"
                       "} finally { $zip.Dispose() }\n")
            .arg(psQuote(entryName), psFilePath(archive), psFilePath(targetPath));

    QProcess process;
    process.start(PowerShell::executable(),
                  {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                   QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                   QStringLiteral("-EncodedCommand"), encodeCommand(script)});
    if (!process.waitForFinished(300000)) {
        const QProcess::ProcessError failure = process.error();
        const QString detail = process.errorString();
        process.kill();
        process.waitForFinished(5000);
        if (errorOut != nullptr) {
            *errorOut = describeProcessFailure(failure, detail, what);
        }
        return EntryStatus::Failed;
    }
    if (process.exitStatus() != QProcess::NormalExit) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("%1时 PowerShell 被强行结束了").arg(what);
        }
        return EntryStatus::Failed;
    }
    const int code = process.exitCode();
    if (code == 3) {
        return EntryStatus::NotFound;
    }
    if (code == 4) {
        return EntryStatus::NotAnArchive;
    }
    if (code != 0) {
        if (errorOut != nullptr) {
            const QString err =
                readProcessError(QString::fromLocal8Bit(process.readAllStandardError()));
            *errorOut =
                err.isEmpty() ? QStringLiteral("%1失败（退出码 %2）").arg(what).arg(code) : err;
        }
        return EntryStatus::Failed;
    }
    return EntryStatus::Ok;
}

ZipUtil::EntryStatus ZipUtil::listEntries(const QString &archive, QStringList *out,
                                          QString *errorOut, const QString &what) {
    if (out != nullptr) {
        out->clear();
    }
    if (errorOut != nullptr) {
        errorOut->clear();
    }

    // 条目名先落到一个**临时文件**再读回来，理由和 readEntry 那条一模一样：stdout 是
    // **文本**通道，过一遍编码转换之后条目名里那几个怪字符（中文、空格、`%`）就不再是
    // 原来的字节了；而条目名恰恰是这一发唯一要传回来的东西。
    //
    // NOTE: 格式用 JSON 而不是"一行一个"：**条目名里可以有换行**（zip 规范不禁），
    // 拼分隔符是给自己埋雷。JSON 由 PowerShell 那一侧写、Qt 这一侧用
    // QJsonDocument 读，两边都不用自己转义。
    //
    // 外面套一层对象（`{ entries = @(...) }`）而不是直接 ConvertTo-Json 一个数组：
    // Windows PowerShell 5.1 的 ConvertTo-Json 会把**只有一条**的数组**摊成一个标量**
    // （`-AsArray` 是 PowerShell 7 才有的开关）。套一层之后那个成员还是会被摊，
    // 但读的那一侧两种形态都接得住（见下面），比"少一条名字"（一个只有一条
    // `pack.mcmeta` 的资源包就被认成"空包"）好得多。
    const QString tempPath = QDir::tempPath() + QStringLiteral("/wtfml-entries-") +
                             QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray temp = QFile::encodeName(tempPath);

    // 退出码：4 = 打不开（不是 zip / 坏了），和 readEntry 同一个约定。
    const QString script =
        QStringLiteral("$ErrorActionPreference='Stop'\n"
                       "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                       "try{ $zip=[System.IO.Compression.ZipFile]::OpenRead(%1) }"
                       " catch { exit 4 }\n"
                       "try{\n"
                       "  $names=[System.Collections.ArrayList]::new()\n"
                       "  foreach($e in $zip.Entries){ [void]$names.Add($e.FullName) }\n"
                       "  $json=([pscustomobject]@{ entries = @($names) }) |"
                       " ConvertTo-Json -Compress -Depth 3\n"
                       "  [System.IO.File]::WriteAllText(%2,$json,"
                       "(New-Object System.Text.UTF8Encoding($false)))\n"
                       "} finally { $zip.Dispose() }\n")
            .arg(psFilePath(archive), psFilePath(QFile::decodeName(temp)));

    QProcess process;
    process.start(PowerShell::executable(),
                  {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                   QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                   QStringLiteral("-EncodedCommand"), encodeCommand(script)});
    if (!process.waitForFinished(60000)) {
        // 同上：先读 error()/errorString()，再 kill。
        const QProcess::ProcessError failure = process.error();
        const QString detail = process.errorString();
        process.kill();
        process.waitForFinished(5000);
        QFile::remove(tempPath);
        if (errorOut != nullptr) {
            *errorOut = describeProcessFailure(failure, detail, what);
        }
        return EntryStatus::Failed;
    }
    const int code = process.exitCode();
    const QByteArray stderrBytes = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit) {
        QFile::remove(tempPath);
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("%1时 PowerShell 被强行结束了").arg(what);
        }
        return EntryStatus::Failed;
    }
    if (code == 4) {
        QFile::remove(tempPath);
        return EntryStatus::NotAnArchive;
    }
    if (code != 0) {
        QFile::remove(tempPath);
        if (errorOut != nullptr) {
            // readProcessError() 的理由见它上面那一段（CLIXML 那一层）。
            const QString err = readProcessError(QString::fromLocal8Bit(stderrBytes));
            *errorOut = err.isEmpty() ? QStringLiteral("%1失败（退出码 %2）").arg(what).arg(code) : err;
        }
        return EntryStatus::Failed;
    }

    QFile file(tempPath);
    const bool read = file.open(QIODevice::ReadOnly);
    const QByteArray bytes = read ? file.readAll() : QByteArray();
    file.close();
    QFile::remove(tempPath);
    if (!read) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("读不出临时文件 %1").arg(tempPath);
        }
        return EntryStatus::Failed;
    }

    // 空压缩包：脚本写出来的是 `{"entries":null}`（`@()` 空的）或者一个空文件。两种都算
    // **成功且一条都没有**，不是失败 —— "这个包是空的"是调用方该自己判的事。
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) {
        if (bytes.trimmed().isEmpty()) {
            return EntryStatus::Ok;
        }
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("%1：条目表读不回来（临时文件里不是 JSON）").arg(what);
        }
        return EntryStatus::Failed;
    }
    const QJsonValue entries = doc.object().value(QStringLiteral("entries"));
    if (entries.isString()) {
        // 只有一条时 5.1 会摊成标量（见上面那段 NOTE）。
        if (out != nullptr) {
            out->append(entries.toString());
        }
        return EntryStatus::Ok;
    }
    if (entries.isArray()) {
        const QJsonArray array = entries.toArray();
        if (out != nullptr) {
            out->reserve(array.size());
            for (const QJsonValue &item : array) {
                out->append(item.toString());
            }
        }
        return EntryStatus::Ok;
    }
    // null / undefined = 空的（见上）。
    return EntryStatus::Ok;
}

bool ZipUtil::extractSubtrees(const QStringList &archives, const QString &targetDir,
                              const QStringList &roots, int *extractedOut, QString *errorOut,
                              const std::function<bool()> &cancelled,
                              const QStringList &excludePrefixes) {
    if (extractedOut != nullptr) {
        *extractedOut = 0;
    }
    if (archives.isEmpty() || roots.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("没有需要解压的压缩包");
        }
        return false;
    }
    if (!QDir().mkpath(targetDir)) {
        if (errorOut) {
            *errorOut = QStringLiteral("无法创建目录：%1").arg(targetDir);
        }
        return false;
    }

    // 前缀统一成"以 / 结尾"的形状：调用方写 `overrides` 还是 `overrides/` 都得认。
    // **空串是合法的**：它表示"整包摘出来"（懒人包那种直接把 .minecraft 压进来的包）。
    bool wholeArchive = false;
    QStringList normalized;
    for (const QString &root : roots) {
        if (root.isEmpty()) {
            wholeArchive = true;
            continue;
        }
        normalized << (root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/'));
    }
    if (wholeArchive) {
        // 整包 + 某个前缀同时给时，前缀那一支完全被整包包含 —— 只按整包算，
        // 否则同一份文件会被写两遍（第二遍覆盖第一遍，白花一次 IO）。
        normalized = QStringList{QString()};
    }
    if (normalized.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("没有需要解压的目录");
        }
        return false;
    }

    // 跳过的前缀同样归一：小写（PowerShell 那侧比的是小写）、目录形态以 `/` 结尾。
    // 单独一份 `foo.txt` 不能变成 `foo.txt/`，所以"是不是目录"由调用方用自己的写法表达。
    QStringList normalizedExcludes;
    for (const QString &exclude : excludePrefixes) {
        if (exclude.isEmpty()) {
            continue;
        }
        normalizedExcludes << exclude.toLower();
    }

    // NOTE: 这一段是**剥掉前缀之后**的那几条守卫，和 extractArchives 里那三条是同一个口径
    // （那边有完整来路）：剥完之后如果剩下的是 `C:/x` 或 `/x`，.NET 的 Path.Combine 会
    // **丢掉**前半截（它认为第二个参数已经是绝对路径了），于是文件被写到 $out 外面去。
    // 条目名里的 `\` 也先换成 `/` 再判 —— Windows 上它同样是分隔符。
    const QString script =
        QStringLiteral("$ErrorActionPreference='Stop'\n"
                       "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                       "$out=%1\n"
                       "[void][System.IO.Directory]::CreateDirectory($out)\n"
                       "$roots=%2\n"
                       "$excludes=%4\n"
                       "$count=0\n"
                       "$total=[long]0\n"
                       "foreach($z in %3){\n"
                       "  $zip=$null\n"
                       "  try{\n"
                       "    $zip=[System.IO.Compression.ZipFile]::OpenRead($z)\n"
                       "    foreach($r in $roots){\n"
                       "      $low=$r.ToLowerInvariant()\n"
                       "      foreach($e in $zip.Entries){\n"
                       "        if([string]::IsNullOrEmpty($e.Name)){continue}\n"
                       "        $full=$e.FullName.Replace('\\','/')\n"
                       "        if(-not $full.ToLowerInvariant().StartsWith($low)){continue}\n"
                       "        $rel=$full.Substring($r.Length)\n"
                       "        if($rel.Split('/') -contains '..'){continue}\n"
                       // 同上那条：Windows 折掉每段末尾的空格 / 点 ⇒ `.. ` / `. ` / `...`
                       // 都能折回父目录。剥完前缀之后同样得挡（`BackupFiles` 的还原走这条）。
                       "        if($rel -match '(^|/)([^/]*[. ])?(/|$)'){continue}\n"
                       "        if($rel.StartsWith('/') -or $rel.Contains(':')){continue}\n"
                       // 解压后的大小 / 压缩比上限（R10）：和 extractArchives() 里那三条同一个
                       // 口径（那边有完整来路）。恢复备份这一条收的也是外部来的 zip。
                       "        if($e.Length -gt 2147483648){throw ('「' + $full + '」解压后超过 2 GiB 上限，按压缩炸弹拦下了')}\n"
                       "        if($e.Length -gt 1048576 -and ($e.Length / [Math]::Max($e.CompressedLength,1)) -gt 10000){throw ('「' + $full + '」的压缩比超过 10000，按压缩炸弹拦下了')}\n"
                       "        if(($total + $e.Length) -gt 17179869184){throw '整包解压后累计超过 16 GiB 上限，按压缩炸弹拦下了'}\n"
                       // 跳过的前缀（剥完之后的名字，小写、`/` 分隔）。`libraries/` 这种
                       // 是整层目录；`options.txt` 这种是单独一份文件。两边都已经小写。
                       "        if($excludes.Count -gt 0){\n"
                       "          $lowrel=$rel.ToLowerInvariant(); $skip=$false\n"
                       "          foreach($x in $excludes){\n"
                       "            if($lowrel -eq $x -or $lowrel.StartsWith($x)){ $skip=$true; break }\n"
                       "          }\n"
                       "          if($skip){continue}\n"
                       "        }\n"
                       "        $dest=[System.IO.Path]::Combine($out,$rel).Replace('/','\\')\n"
                       "        [void][System.IO.Directory]::CreateDirectory"
                       "([System.IO.Path]::GetDirectoryName($dest))\n"
                       "        [System.IO.Compression.ZipFileExtensions]::ExtractToFile"
                       "($e,$dest,$true)\n"
                       "        $total=$total+$e.Length\n"
                       "        $count=$count+1\n"
                       "      }\n"
                       "    }\n"
                       "  } catch {\n"
                       // 同上（整包解压那一段的 NOTE 逐条适用）。整合包导入这条路走的**就是**
                       // 这一段：`overrides/` 里出现一个超长路径或者被占用的文件时，报出来的是
                       // 哪个包、哪句 .NET 的原话，而不是一句「PowerShell 退出码 1」。
                       "    $msg=$_.Exception.GetBaseException().Message\n"
                       "    if([string]::IsNullOrEmpty($msg)){ $msg=$_.Exception.Message }\n"
                       "    [Console]::Error.WriteLine(('解压「' + $z + '」时出错：' + $msg))\n"
                       "    exit 3\n"
                       "  } finally { if($zip){ $zip.Dispose() } }\n"
                       "}\n"
                       "[Console]::Out.Write($count)\n")
            .arg(psFilePath(QDir(targetDir).absolutePath()), psArray(normalized), psArray(processIoPaths(archives)),
                 psArray(normalizedExcludes));

    QByteArray stdoutBytes;
    QString failure;
    if (!runPowershell(script, 900 * 1000, &stdoutBytes, &failure,
                       QStringLiteral("解压"), cancelled)) {
        if (errorOut) {
            *errorOut = failure;
        }
        return false;
    }
    if (extractedOut != nullptr) {
        *extractedOut = QString::fromLatin1(stdoutBytes).trimmed().toInt();
    }
    return true;
}

bool ZipUtil::gunzipTo(const QString &sourceGz, const QString &targetPath, QString *errorOut) {
    if (errorOut != nullptr) {
        errorOut->clear();
    }

    // 解到目标路径，**不让 PowerShell 把字节打到 stdout** —— 理由和 readEntry 里那段
    // 一样（stdout 是文本通道，二进制过一趟就不再是原来的字节）。
    //
    // NOTE: 两个 Add-Type 都要有：GZipStream 与 CompressionMode 在
    // System.IO.Compression 里，只加 FileSystem 的话在只装了 .NET Framework
    // 的机器上这一步会报「找不到类型」（FileSystem 依赖它，但别指望程序集
    // 加载顺序替我们把类型也带进来）。
    // NOTE: $dest 只出现一次（先赋给变量再用），不让同一个 %2 在脚本里出现两遍 ——
    // QString::arg 的占位符替换对「同一个编号出现多次」这件事不好读，绕开它。
    // NOTE: **整段脚本**（连 Add-Type 和建目录）都包在那一个 try 里，catch 里把原因
    // 用 [Console]::Error.WriteLine() 写到**原生 stderr** 上再 exit 3。
    // 两个原因：① 不兜住的话 PowerShell 把错误流序列化成 CLIXML 丢到 stderr 上
    // （见 readProcessError 的说明），调用方拿到的是 `#< CLIXML` 这种没人看得懂的
    // 东西；② 退出码要和"进程没起来/超时"分开 —— 那两档 runPowershell 自己会说。
    const QString script =
        QStringLiteral("$ErrorActionPreference='Stop'\n"
                       "try{\n"
                       "  Add-Type -AssemblyName System.IO.Compression\n"
                       "  Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                       "  $dest=%1\n"
                       "  $dir=[System.IO.Path]::GetDirectoryName($dest)\n"
                       "  if(-not [string]::IsNullOrEmpty($dir)){\n"
                       "    [void][System.IO.Directory]::CreateDirectory($dir)\n"
                       "  }\n"
                       "  $in=[System.IO.File]::OpenRead(%2)\n"
                       "  try{\n"
                       "    $gz=New-Object System.IO.Compression.GZipStream"
                       "($in,[System.IO.Compression.CompressionMode]::Decompress)\n"
                       "    try{\n"
                       "      $out=[System.IO.File]::Create($dest)\n"
                       "      try{ $gz.CopyTo($out) } finally { $out.Dispose() }\n"
                       "    } finally { $gz.Dispose() }\n"
                       "  } finally { $in.Dispose() }\n"
                       "} catch {\n"
                       // GetBaseException() 取的是**最里面**那一层：CopyTo 抛出来的是个
                       // 包装（MethodInvocationException），真正有用的那句（比如「这不是
                       // gzip」）在它里面；只取 Message 会拿到「Exception calling CopyTo…」
                       // 这种把真因埋在引号里的话。
                       "  $msg=$_.Exception.GetBaseException().Message\n"
                       "  if([string]::IsNullOrEmpty($msg)){ $msg=$_.Exception.Message }\n"
                       "  [Console]::Error.WriteLine($msg)\n"
                       "  exit 3\n"
                       "}\n")
            .arg(psFilePath(targetPath), psFilePath(sourceGz));

    // 15 秒。这个数不是随便定的：真机上 servers.dat 是几百字节，连起进程算上也就
    // 一秒出头；给它 15 秒是为了"机器正忙"时别误判，同时"没装 PowerShell / 被安全
    // 软件拦下"那一档不会让用户干等（那一档根本不进等待，直接 FailedToStart）。
    QString failure;
    if (!runPowershell(script, 15000, nullptr, &failure, QStringLiteral("解压 gzip"))) {
        if (errorOut != nullptr) {
            const QString reason = readProcessError(failure);
            *errorOut = reason.isEmpty() ? QStringLiteral("解压 gzip 失败") : reason;
        }
        return false;
    }
    return true;
}

bool ZipUtil::gzipFrom(const QString &sourcePath, const QString &targetGz, QString *errorOut) {
    if (errorOut != nullptr) {
        errorOut->clear();
    }

    // gunzipTo() 的反方向，一个字节的结构都不多：同样**不让 PowerShell 碰 stdout**，
    // 同样把整段包在一个 try 里、catch 里写原生 stderr 再 exit 3，退出码的含义也一致
    // （3 = 脚本跑到了、里面出错了；"没起来/超时"由 runPowershell 自己那一档说）。
    //
    // NOTE: 写那一侧的顺序是**反的** —— 先开目标文件，再把 GZipStream 套在它外面，
    // 最后把源文件 CopyTo 进 gz。两个 finally 的顺序也一样重要：`$out.Dispose()`
    // 必须在 `$gz.Dispose()` **之前**（GZipStream 的 Dispose 要把压缩尾巴刷出去，
    // 底下的流已经关了的话那一段就丢了 —— 症状是一个能建出来、谁解都报错的包）。
    // NOTE: CompressionLevel 用的是 Optimal（和 createZip 那一档同一个理由：不为了几个
    // 百分点造出一个"和大家不一样"的包）。level.dat 是几十 KB，这一档和最快那一档
    // 的耗时差在人眼以下。
    const QString script =
        QStringLiteral("$ErrorActionPreference='Stop'\n"
                       "try{\n"
                       "  Add-Type -AssemblyName System.IO.Compression\n"
                       "  Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                       "  $dest=%1\n"
                       "  $dir=[System.IO.Path]::GetDirectoryName($dest)\n"
                       "  if(-not [string]::IsNullOrEmpty($dir)){\n"
                       "    [void][System.IO.Directory]::CreateDirectory($dir)\n"
                       "  }\n"
                       "  $in=[System.IO.File]::OpenRead(%2)\n"
                       "  try{\n"
                       "    $out=[System.IO.File]::Create($dest)\n"
                       "    try{\n"
                       "      $gz=New-Object System.IO.Compression.GZipStream"
                       "($out,[System.IO.Compression.CompressionMode]::Compress)\n"
                       "      try{ $in.CopyTo($gz) } finally { $gz.Dispose() }\n"
                       "    } finally { $out.Dispose() }\n"
                       "  } finally { $in.Dispose() }\n"
                       "} catch {\n"
                       "  $msg=$_.Exception.GetBaseException().Message\n"
                       "  if([string]::IsNullOrEmpty($msg)){ $msg=$_.Exception.Message }\n"
                       "  [Console]::Error.WriteLine($msg)\n"
                       "  exit 3\n"
                       "}\n")
            .arg(psFilePath(targetGz), psFilePath(sourcePath));

    QString failure;
    // 15 秒是"压几十 KB 的 level.dat"的预算，但这一发还要**起 PowerShell**（加载
    // System.IO.Compression、过安全软件的首扫）—— CI runner 上冷启动偶尔就吃掉十几秒，
    // 于是夹具超时、整个 core-instances 判死（2026-11-02 连挂两次的 `ZL6`）。
    // 60 秒和 readEntry() 那一档对齐：真出问题时仍然会失败，只是不再把"机器慢"当"文件坏"。
    if (!runPowershell(script, 60000, nullptr, &failure, QStringLiteral("压缩 gzip"))) {
        if (errorOut != nullptr) {
            const QString reason = readProcessError(failure);
            *errorOut = reason.isEmpty() ? QStringLiteral("写 gzip 失败") : reason;
        }
        return false;
    }
    return true;
}

bool ZipUtil::createZip(const QString &targetZip, const QList<ZipSource> &entries,
                        QString *errorOut, const std::function<bool()> &cancelled) {
    if (cancelled && cancelled()) {
        if (errorOut) *errorOut = QStringLiteral("打包已取消或超时。");
        return false;
    }
    if (targetZip.trimmed().isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("没有给出要写到哪去。");
        }
        return false;
    }
    // **目标已经存在就当场拒绝，不覆盖。**
    //
    // 底下 PowerShell 那一句是 `ZipFile::Open($target,'Create')` —— 那个 `Create` 不是
    // "建一个新的、没有就建"，而是**打开并清空**：目标要是本来就在，它当场被截成 0 字节。
    // 而失败那一路又会把目标删掉（"半成品不留"）⇒ 一次没打成的包会把盘上原来那份好的
    // 顺手毁掉，用户看见的是"打包失败"之后那份旧备份也没了。
    //
    // 产品里的调用方（备份、导出）都是先挑一个没人占的名字再进来，所以这一条在它们那儿
    // 永远是通的；拦在这里是为了让"传了一个已经存在的路径"这种调用**在写盘之前**就停下，
    // 而不是靠调用方各记各的。NOTE: 拒绝而不是"建个临时文件再原子替换" —— 替换会让
    // "失败时旧文件一个字节不动"这句保证也跟着变复杂，而这条路上没有谁需要覆盖语义。
    if (QFileInfo::exists(targetZip)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("目标文件已经存在，没有覆盖它：%1")
                            .arg(QDir::toNativeSeparators(targetZip));
        }
        return false;
    }
    for (const ZipSource &entry : entries) {
        if (entry.entryName.trimmed().isEmpty() || entry.sourcePath.trimmed().isEmpty()) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("打包的条目表里有空名字或者空来源。");
            }
            return false;
        }
        // 名字里带 `\` 的条目在别的启动器眼里就是一个**名字里带反斜杠的文件**（zip 规范
        // 只用 `/`），等于整层 overrides 全废 —— 而那种包我们自己读得回来（.NET 读的时候
        // 会归一），所以这种错在自测里是看不见的。在这里直接拦掉。
        if (entry.entryName.contains(QLatin1Char('\\'))) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("打包的条目名里有反斜杠：%1").arg(entry.entryName);
            }
            return false;
        }
    }

    // 条目表**写成一个临时文件，不拼进脚本正文**。
    //
    // NOTE: 这是一条硬限制：脚本是经 `-EncodedCommand`
    // 用**命令行**递给 PowerShell 的，而 Windows 给一条命令行只有 32767 个字符；
    // 脚本要先做 Base64（×4/3）再算 UTF-16LE（×2）⇒ 能塞进去的脚本大约 1.2 万字符。
    // 一个装着上百个模组的实例，光这张 `@('条目名','盘上路径')` 表就超过一万字符
    // ⇒ CreateProcess 当场失败，QProcess 报「文件名或扩展名太长」
    // （ERROR_FILENAME_EXCED_RANGE），用户看到的是「打包失败」而盘上什么都没留下。
    // **小夹具（两条三条）永远看不见这个** —— 断言得照真实例的量级摆。
    // NOTE: 表用 JSON 而不是"一行一条、TAB 分隔"：条目名和路径里什么都可能有（空格、单引号、
    // 中文、`.`），拼分隔符迟早踩雷，而 JSON 由 Qt 这一侧写、PowerShell 那一侧
    // ConvertFrom-Json 读，两边都不用自己转义。
    const QString tablePath = QDir::tempPath() + QStringLiteral("/wtfml-ziptable-") +
                              QUuid::createUuid().toString(QUuid::WithoutBraces) +
                              QStringLiteral(".json");
    {
        QJsonArray table;
        for (const ZipSource &entry : entries) {
            QJsonObject item;
            item.insert(QStringLiteral("name"), entry.entryName);
            item.insert(QStringLiteral("src"), processIoPath(entry.sourcePath));
            table.append(item);
        }
        QFile tableFile(tablePath);
        if (!tableFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("打包的条目表写不出来：%1").arg(tablePath);
            }
            return false;
        }
        // 这一发的返回值**必须查**。写不进去（磁盘满、临时目录没有写
        // 权限、被安全软件拦下）时 `write()` 回 -1，而流程照旧往下走：PowerShell 那一侧
        // 拿到一个空文件，`ConvertFrom-Json` 报「不能对 Null 值表达式调用方法」，用户看到
        // 的是这么一句和真因（盘满）毫无关系的话，而目标 zip 里**一个条目都没有**。
        // 这里当场拦下。NOTE: 短写（盘刚好在半路满）也一并算失败 —— 半截 JSON 同样过不了
        // ConvertFrom-Json，早点说清楚比让它去猜强。
        const QByteArray payload = QJsonDocument(table).toJson(QJsonDocument::Compact);
        if (tableFile.write(payload) != payload.size()) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("打包的条目表写不完整：%1（临时目录还剩多少空间？）")
                                .arg(tablePath);
            }
            tableFile.close();
            QFile::remove(tablePath);
            return false;
        }
    }

    const QString script =
        QStringLiteral("$ErrorActionPreference='Stop'\n"
                       "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
                       "$target=%1\n"
                       "$table=%2\n"
                       // 外头这层**必须是 `[array]` 而不是 `@()`**：两种形态都得接住 ——
                       // 只有一条时 ConvertFrom-Json 回的是**一个对象**（不是数组），
                       // 它要能兜成一条；而多条时 ConvertFrom-Json 是**不展开**的（整个数组
                       // 当**一个**输出写出去），`@()` 会把它包成一个"装着数组的一元数组"
                       // ⇒ foreach 只跑一圈、`$p.name` 由成员枚举变成**所有名字拼成的一串**
                       // ⇒ CreateEntryFromFile 收到一个数组当路径，报
                       // 「The given path's format is not supported.」
                       // (`[array]` 对一条和多条两种形态都是平铺的。)
                       "$pairs=[array]([System.IO.File]::ReadAllText"
                       "($table,[System.Text.Encoding]::UTF8) | ConvertFrom-Json)\n"
                       "$dir=[System.IO.Path]::GetDirectoryName($target)\n"
                       "if(-not [string]::IsNullOrEmpty($dir)){\n"
                       "  [void][System.IO.Directory]::CreateDirectory($dir)\n"
                       "}\n"
                       "$current=''\n"
                       "try{\n"
                       "  $zip=[System.IO.Compression.ZipFile]::Open($target,'Create')\n"
                       "  try{\n"
                       "    foreach($p in $pairs){\n"
                       "      $current=$p.name\n"
                       "      [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile"
                       "($zip,$p.src,$p.name,[System.IO.Compression.CompressionLevel]::Optimal)\n"
                       "    }\n"
                       "  } finally { $zip.Dispose() }\n"
                       "} catch {\n"
                       // 同上：GetBaseException() 取最里面那一层（缺文件/被占用那些真因
                       // 在包装里面）。
                       "  $msg=$_.Exception.GetBaseException().Message\n"
                       "  if([string]::IsNullOrEmpty($msg)){ $msg=$_.Exception.Message }\n"
                       // 那一句 .NET 的原文（英文、带着完整路径）**照旧带上** —— 它是唯一
                       // 说得出"哪个文件怎么了"的东西；前面这半句是给用户看的主语，因为
                       // 这一条最后是要**弹出来的**（能挡住用户操作的那种错）。
                       "  [Console]::Error.WriteLine(('打包「' + $current + '」时出错：' + $msg))\n"
                       "  exit 3\n"
                       "}\n")
            .arg(psFilePath(targetZip), psFilePath(tablePath));

    QString failure;
    // 开工之前已经确认过目标不在（见上面那段），所以走到这儿目标**只可能是我们刚建的**
    // ⇒ 失败时删掉它删的是我们自己写的半成品，不会碰到谁的文件。
    const bool ok = runPowershell(script, 1800 * 1000, nullptr, &failure, QStringLiteral("打包"), cancelled);
    // 临时表两份出路都要删（成功、失败都一样）—— 它躺在系统临时目录里，里面是用户的
    // 实例里都有哪些文件，没理由留一份。
    QFile::remove(tablePath);
    if (!ok) {
        // 半成品删掉（见 .hpp）。NOTE: 删的是**我们写的那个目标路径**，不碰别的。
        QFile::remove(targetZip);
        if (errorOut != nullptr) {
            *errorOut = failure.isEmpty() ? QStringLiteral("打包失败") : failure;
        }
        return false;
    }
    return true;
}


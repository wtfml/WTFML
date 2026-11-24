/**
 * @file ModpackImport.cpp
 * @brief ModpackImport 的实现。判据的来龙去脉写在 .hpp 里；这里只有实现。
 * @note 两种整合包：Modrinth（`modrinth.index.json`，清单里带地址）与
 * CurseForge（`manifest.json`，**清单里只有号**，要先拿去接口换文件）。
 * 后者在这里分两段：`parseCurseForgeManifest` 读号，`parseCurseForgeFileInfo`
 * 读接口的答复 —— 中间那一发网络请求在 VersionManager 里（见 resolveCurseForgeFiles）。
 */
#include "ModpackImport.hpp"
#include <QCoreApplication>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>

#include "InstanceFiles.hpp"

namespace ModpackImport {

QString modrinthIndexName() { return QStringLiteral("modrinth.index.json"); }

QString curseForgeManifestName() { return QStringLiteral("manifest.json"); }

QString fabricLoaderKey() { return QStringLiteral("fabric-loader"); }

QString forgeLoaderKey() { return QStringLiteral("forge"); }

QString neoforgeLoaderKey() { return QStringLiteral("neoforge"); }

QString quiltLoaderKey() { return QStringLiteral("quilt-loader"); }

QStringList loaderDependencyKeys() {
    // 顺序 = 认的顺序。一个包里只会声明其中一个，但表本身得有确定的次序：靠遍历
    // QJsonObject 的键来挑是"实现细节当规则用"，换一次 Qt 就可能挑到另一个。
    return {fabricLoaderKey(), forgeLoaderKey(), neoforgeLoaderKey(), quiltLoaderKey()};
}

QString unsupportedLoaderText(const QString &loaderKey, const QString &loaderVersion) {
    if (loaderKey.isEmpty()) {
        // 纯原版的整合包（依赖表里只有 minecraft）：能装，只是没有加载器要塞。
        return QString();
    }
    if (loaderKey == fabricLoaderKey() || loaderKey == forgeLoaderKey() ||
        loaderKey == neoforgeLoaderKey() || loaderKey == quiltLoaderKey()) {
        return QString();
    }

    const QString version =
        loaderVersion.isEmpty() ? QString() : QStringLiteral(" %1").arg(loaderVersion);
    // 整合包导入现在认得 fabric-loader / forge / neoforge / quilt-loader 四个键。
    // OptiFine 在整合包依赖表里没有标准键，仍然拒绝；不能因为安装页能装 OptiFine 就
    // 把这种包当成能装 —— 那会装出一个进不去的实例。
    return QCoreApplication::translate("ModpackImport", "此整合包需要加载器「%1」%2。导入整合包仅支持 fabric-loader、forge、neoforge 和 quilt-loader，导入已取消。\n\n如需单独安装 Fabric、Forge、NeoForge 或 OptiFine，请使用「安装」页。整合包的依赖清单无法指定 OptiFine。")
        .arg(loaderKey, version);
}

QString unsupportedFormatText(Format format) {
    // NOTE: 参数现在**用不上**了 —— 每种认得的格式都有自己的入口，走到这里的一律是
    // `Unknown`。留着形状是为了调用点不用改。
    Q_UNUSED(format);
    return QCoreApplication::translate("ModpackImport", "无法识别整合包格式，因为压缩包缺少有效清单。\n\n支持 Modrinth（modrinth.index.json）、CurseForge（manifest.json）、MultiMC / Prism（mmc-pack.json），以及含 mods/、config/ 或 options.txt 的 .minecraft 压缩包。");
}

bool isSafeRelativePath(const QString &path) {
    if (path.isEmpty()) {
        return false;
    }
    // 整条路径的长度上限（Windows 的 MAX_PATH 是 260，减去实例目录本身那一段留的余量）。
    // NOTE: 这个数**不是** kMaxDirNameLength —— 那个是实例名的规矩，路径段是另一回事。
    if (path.length() > 240) {
        return false;
    }
    // 反斜杠一律拒绝。Modrinth 的清单里用 `/`，出现 `\` 只可能是想借 Windows 的分隔符
    // 绕过逐段判据：`mods\..\..\x.jar` 按 `/` 切开是**一整段**，逐段看起来完全正常。
    if (path.contains(QLatin1Char('\\'))) {
        return false;
    }
    // 绝对路径（`/etc/...`）。盘符（`C:...`）不用单独判：冒号本来就在非法字符里。
    if (path.startsWith(QLatin1Char('/'))) {
        return false;
    }

    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        // 逐段的规矩和"实例名"共用一份（含空段 —— `a//b` 会被切出一个空段）。
        if (!InstanceFiles::validatePathSegment(segment).isEmpty()) {
            return false;
        }
    }
    return true;
}

namespace {

/// 这一条地址是不是 Modrinth 自己的 CDN。
/// 判 host、不判整串：清单里同一个文件的 modrinth 地址带着 `%2B` / `%2b` 两种大小写
/// （见那两个真包），拿字符串比很容易在某个包上悄悄失效。
bool isModrinthCdnUrl(const QString &url) {
    return QUrl(url).host() == QLatin1String("cdn.modrinth.com");
}

/// 清单里那条路径的**去重键**。
///
/// 为什么不能直接拿路径当键：Windows 的文件系统**不区分大小写**，
/// `mods/X.jar` 和 `mods/x.jar` 是同一个文件，而按区分大小写的键去重会给它们排出两个
/// 作业 —— 两个作业抢着写同一个路径（内容一样就白下一遍，不一样就"谁最后写完谁说了算"）。
/// 别的平台上这两个确实是两个文件，照旧各算一条，所以这一手只在 Windows 上做。
///
/// NOTE: toLower() 不是 Windows 那套精确的大小写折叠（它认的是 UpCaseTable），但对整合包
/// 里这种 ASCII 路径足够了 —— 目标是"认得出这是同一个文件"，不是逐字符等价。
QString pathDedupKey(const QString &path) {
#ifdef Q_OS_WIN
    return path.toLower();
#else
    return path;
#endif
}

}  // namespace

QStringList preferredUrlOrder(const QStringList &urls) {
    QStringList ordered;
    ordered.reserve(urls.size());
    for (const QString &url : urls) {
        if (isModrinthCdnUrl(url)) {
            ordered << url;
        }
    }
    // 一条都不匹配时，下面这个循环会把整份**原样**抄一遍 —— 次序不动，和没调过一样。
    for (const QString &url : urls) {
        if (!isModrinthCdnUrl(url)) {
            ordered << url;
        }
    }
    return ordered;
}

QString suggestInstanceName(const QString &packName) {
    QString name = packName.trimmed();

    // 建不出目录的字符换成 `-`：实例名那条路是**拒绝**（报一句"名字里不能有…"），
    // 这里是把名字修好 —— 用户还没输入任何东西，不该先撞一句错误。
    static const QString kIllegal = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < name.size(); ++i) {
        const QChar ch = name.at(i);
        if (ch.unicode() < 0x20 || kIllegal.contains(ch)) {
            name[i] = QLatin1Char('-');
        }
    }

    // 结尾的点和空格：Windows 会静默吃掉（见 InstanceFiles::validatePathSegment 里那段）。
    // 整合包的名字里出现结尾的点是常事（`xxx.1.8.4` 少见，但 `xxx.` 见过），所以这一步
    // 不是多余的。
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    name = name.trimmed();

    if (name.length() > InstanceFiles::kMaxDirNameLength) {
        name = name.left(InstanceFiles::kMaxDirNameLength).trimmed();
        while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
            name.chop(1);
        }
    }

    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return QObject::tr("整合包");
    }

    // 剩下的只有"Windows 保留设备名"这一类（`CON`、`COM1`……，见 validatePathSegment）。
    // 那种名字建不出目录来，而这里又不能报错（用户什么都没输），所以换个说法绕开它。
    // NOTE: 判据用**同一份** validateDirName()，不自己抄一遍保留名清单。
    if (!InstanceFiles::validateDirName(name).isEmpty()) {
        name = QObject::tr("整合包 %1").arg(name);
        if (name.length() > InstanceFiles::kMaxDirNameLength) {
            name = name.left(InstanceFiles::kMaxDirNameLength).trimmed();
            // 截完**还要再砍一次结尾的点**：截出来的名字可能以点结尾，那就过不了
            // validateDirName()，于是这个函数会返回一个"它自己刚拒过一次"的名字。而这里
            // 又不许报错（用户什么都没输），调用方拿它去建目录，Windows 会悄悄把结尾的
            // 点吃掉 = 名字跟返回值不一样。
            while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
                name.chop(1);
            }
        }
    }
    return name;
}

QString suggestUniqueInstanceName(const QString &packName,
                                  const std::function<bool(const QString &)> &isAvailable) {
    return suggestOneClickInstanceName(
               packName, [](const QString &) { return false; }, isAvailable)
        .name;
}

OneClickInstanceName suggestOneClickInstanceName(
    const QString &packName,
    const std::function<bool(const QString &)> &isUnfinished,
    const std::function<bool(const QString &)> &isAvailable) {
    const auto unfinished = [&](const QString &candidate) {
        if (!InstanceFiles::validateDirName(candidate).isEmpty()) {
            return false;
        }
        return isUnfinished ? isUnfinished(candidate)
                            : InstanceFiles::isUnfinishedImport(candidate);
    };
    const auto available = [&](const QString &candidate) {
        if (!InstanceFiles::validateDirName(candidate).isEmpty()) {
            return false;
        }
        return isAvailable ? isAvailable(candidate)
                           : InstanceFiles::validateRenameTarget(candidate, QString()).isEmpty();
    };

    const QString base = suggestInstanceName(packName);
    if (unfinished(base)) {
        return {base, true};
    }
    // 首个可用空位即止，避免没有冲突时继续探测近万个不存在的后缀目录。
    if (available(base)) {
        return {base, false};
    }

    for (int index = 2; index < 10000; ++index) {
        const QString suffix = QStringLiteral("-%1").arg(index);
        const int maxPrefix = InstanceFiles::kMaxDirNameLength - suffix.length();
        QString prefix = maxPrefix > 0 ? base.left(maxPrefix).trimmed() : QString();
        while (prefix.endsWith(QLatin1Char('.')) || prefix.endsWith(QLatin1Char(' '))) {
            prefix.chop(1);
        }
        const QString candidate = prefix.isEmpty() ? QObject::tr("整合包%1").arg(suffix) : prefix + suffix;
        if (unfinished(candidate)) {
            return {candidate, true};
        }
        if (available(candidate)) {
            return {candidate, false};
        }
    }
    return {base, false};
}

// ---------------------------------------------------------------------------
// 压缩包形状：MultiMC / 普通 .minecraft / PCL 包装
// ---------------------------------------------------------------------------

namespace {

QString slashedEntry(const QString &entry) {
    QString name = entry;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1Char('/'))) {
        name.remove(0, 1);
    }
    return name;
}

/// 打包工具/系统留下的垃圾条目。判断"最外层只有一层目录"时必须先剔掉它们，
/// 否则 macOS 上打的包永远判不出根前缀（根上多一条 `__MACOSX/`）。
bool isJunkEntry(const QString &lowerName) {
    return lowerName.startsWith(QLatin1String("__macosx/")) ||
           lowerName == QLatin1String(".ds_store") ||
           lowerName.endsWith(QLatin1String("/.ds_store")) ||
           lowerName == QLatin1String("thumbs.db") ||
           lowerName.endsWith(QLatin1String("/thumbs.db")) ||
           lowerName == QLatin1String("desktop.ini") ||
           lowerName.endsWith(QLatin1String("/desktop.ini"));
}

QString commonRootPrefix(const QStringList &entries) {
    QString root;
    for (const QString &raw : entries) {
        const QString name = slashedEntry(raw);
        if (name.isEmpty()) {
            continue;
        }
        const QString lower = name.toLower();
        if (isJunkEntry(lower)) {
            continue;
        }
        const int slash = name.indexOf(QLatin1Char('/'));
        if (slash < 0) {
            // 根目录直接躺着一个文件 ⇒ 没有"唯一一层目录"。
            return QString();
        }
        const QString top = name.left(slash);
        if (top.isEmpty()) {
            continue;
        }
        if (root.isEmpty()) {
            root = top;
        } else if (root.compare(top, Qt::CaseInsensitive) != 0) {
            return QString();
        }
    }
    return root.isEmpty() ? QString() : root + QLatin1Char('/');
}

QString findRelativeEntry(const QStringList &entries, const QString &prefix,
                          const QString &relative) {
    const QString target = (prefix + relative).toLower();
    for (const QString &raw : entries) {
        const QString name = slashedEntry(raw);
        if (name.endsWith(QLatin1Char('/'))) {
            continue;  // 目录条目不算文件
        }
        if (name.toLower() == target) {
            return name;
        }
    }
    return QString();
}

bool hasRelativeDir(const QStringList &entries, const QString &prefix, const QString &dir) {
    if (dir.isEmpty()) {
        return false;
    }
    QString base = (prefix + dir).toLower();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    for (const QString &raw : entries) {
        const QString name = slashedEntry(raw).toLower();
        if (name == base || name.startsWith(base + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

/// 压缩包里是不是"好几个实例打在一起"（两个以上顶层目录各自带着一个 `.minecraft/`）。
///
/// ★ 这种包**不猜**：随便挑一个导进来，剩下的那几份既不会出现、也不会被提一句 ——
/// 而用户以为"整个包都装好了"。返回那句人话（不是多实例时回空串）；判据只认
/// "各带一个 `.minecraft/`"，不带这个目录的兄弟目录（说明、启动器本体）不算。
QString multiInstanceDetail(const QStringList &entries, const QString &prefix) {
    QSet<QString> tops;
    const QString base = prefix.toLower();
    for (const QString &raw : entries) {
        const QString name = slashedEntry(raw).toLower();
        if (!name.startsWith(base)) {
            continue;
        }
        const QString rest = name.mid(base.size());
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash <= 0) {
            continue;
        }
        const QString tail = rest.mid(slash + 1);
        if (tail == QLatin1String(".minecraft") ||
            tail.startsWith(QLatin1String(".minecraft/"))) {
            tops.insert(rest.left(slash));
        }
    }
    if (tops.size() < 2) {
        return QString();
    }
    return QObject::tr("压缩包中有多个 .minecraft 目录。请解压后选择要导入的目录。");
}

bool hasLoaderKeyword(const QString &lowerId) {
    return lowerId.contains(QLatin1String("forge")) ||
           lowerId.contains(QLatin1String("fabric")) ||
           lowerId.contains(QLatin1String("neoforge")) ||
           lowerId.contains(QLatin1String("quilt"));
}

QString pickVersionJsonEntry(const QStringList &entries, const QString &prefix) {
    const QString versionsPrefix = (prefix + QStringLiteral("versions/")).toLower();
    struct Candidate {
        QString idLower;
        QString entry;
        bool strong = false;
    };
    QList<Candidate> candidates;
    for (const QString &raw : entries) {
        const QString name = slashedEntry(raw);
        if (name.endsWith(QLatin1Char('/'))) {
            continue;
        }
        const QString lower = name.toLower();
        if (!lower.startsWith(versionsPrefix)) {
            continue;
        }
        const QStringList parts = lower.mid(versionsPrefix.size()).split(QLatin1Char('/'));
        if (parts.size() != 2 || !parts.at(1).endsWith(QLatin1String(".json"))) {
            continue;
        }
        Candidate candidate;
        candidate.idLower = parts.at(0);
        candidate.entry = name;
        candidate.strong = parts.at(1) == parts.at(0) + QLatin1String(".json");
        candidates.append(candidate);
    }
    QList<Candidate> strong;
    for (const Candidate &candidate : candidates) {
        if (candidate.strong) {
            strong.append(candidate);
        }
    }
    const QList<Candidate> &pool = strong.isEmpty() ? candidates : strong;
    QString loaderEntry;
    int loaders = 0;
    for (const Candidate &candidate : pool) {
        if (hasLoaderKeyword(candidate.idLower)) {
            loaderEntry = candidate.entry;
            ++loaders;
        }
    }
    // A base version plus one loader is unambiguous; two loader instances are not.
    return loaders == 1 ? loaderEntry
                        : (pool.size() == 1 ? pool.first().entry : QString());
}

QStringList versionIdsFromEntries(const QStringList &entries, const QString &prefix) {
    const QString versionsPrefix = (prefix + QStringLiteral("versions/")).toLower();
    QStringList ids;
    QSet<QString> seen;
    for (const QString &raw : entries) {
        const QString name = slashedEntry(raw).toLower();
        if (!name.startsWith(versionsPrefix)) {
            continue;
        }
        const QStringList parts = name.mid(versionsPrefix.size()).split(QLatin1Char('/'));
        if (parts.isEmpty() || parts.first().isEmpty()) {
            continue;
        }
        if (seen.contains(parts.first())) {
            continue;
        }
        seen.insert(parts.first());
        ids.append(parts.first());
    }
    return ids;
}

bool looksLikeMinecraftVersion(const QString &text) {
    const QString value = text.trimmed();
    if (value.isEmpty()) {
        return false;
    }
    static const QRegularExpression release(QStringLiteral("^\\d+\\.\\d+(?:\\.\\d+)?$"));
    static const QRegularExpression snapshot(QStringLiteral("^\\d{2}w\\d{2}[a-z0-9]+$"),
                                             QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression oldAlpha(QStringLiteral("^[ab]\\d+\\.\\d+(?:\\.\\d+)?$"),
                                             QRegularExpression::CaseInsensitiveOption);
    return release.match(value).hasMatch() || snapshot.match(value).hasMatch() ||
           oldAlpha.match(value).hasMatch();
}

QString minecraftVersionFromId(const QString &id) {
    const QString value = id.trimmed();
    if (looksLikeMinecraftVersion(value)) {
        return value;
    }
    static const QRegularExpression separators(QStringLiteral("[-_]"));
    for (const QString &part : value.split(separators)) {
        if (looksLikeMinecraftVersion(part)) {
            return part;
        }
    }
    return QString();
}

QString stripTrailingSlashes(QString path) {
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path;
}

QString instanceCfgValue(const QByteArray &bytes, const QString &key) {
    const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int equals = trimmed.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            continue;
        }
        if (trimmed.left(equals).trimmed().compare(key, Qt::CaseInsensitive) == 0) {
            return trimmed.mid(equals + 1).trimmed();
        }
    }
    return QString();
}

QString normalizeForgeVersion(QString version, const QString &gameVersion) {
    version = version.trimmed();
    if (gameVersion.isEmpty()) {
        return version;
    }
    if (version.startsWith(gameVersion + QLatin1Char('-'))) {
        version = version.mid(gameVersion.size() + 1);
    }
    if (version.endsWith(QLatin1Char('-') + gameVersion)) {
        version.chop(gameVersion.size() + 1);
    }
    return version;
}

struct LoaderGuess {
    QString key;
    QString version;
};

LoaderGuess loaderFromLibraries(const QStringList &names, const QString &gameVersion) {
    LoaderGuess out;
    QString fmlloaderVersion;
    for (const QString &raw : names) {
        const QStringList parts = raw.split(QLatin1Char(':'));
        if (parts.size() < 3) {
            continue;
        }
        const QString group = parts.at(0).toLower();
        const QString artifact = parts.at(1).toLower();
        const QString version = parts.at(2);
        if (group == QLatin1String("net.fabricmc") &&
            artifact == QLatin1String("fabric-loader")) {
            return {fabricLoaderKey(), version};
        }
        if (group == QLatin1String("org.quiltmc") &&
            artifact == QLatin1String("quilt-loader")) {
            return {quiltLoaderKey(), version};
        }
        if (group == QLatin1String("net.neoforged") && artifact == QLatin1String("neoforge")) {
            return {neoforgeLoaderKey(), version};
        }
        if (group == QLatin1String("net.minecraftforge") && artifact == QLatin1String("forge")) {
            return {forgeLoaderKey(), normalizeForgeVersion(version, gameVersion)};
        }
        if (group == QLatin1String("net.minecraftforge") &&
            artifact == QLatin1String("fmlloader") && fmlloaderVersion.isEmpty()) {
            fmlloaderVersion = version;
        }
    }
    // 只剩 FML loader 坐标（新的 Forge 偶尔这么写）时，那个版本号不等于 Forge 版本，
    // 只当作"这是个 Forge 包"，版本留空 = 装的时候挑最新的。
    if (!fmlloaderVersion.isEmpty()) {
        return {forgeLoaderKey(), QString()};
    }
    return out;
}

LoaderGuess loaderFromId(const QString &id) {
    const QString lower = id.toLower();
    struct Keyword {
        const char *word;
        const char *key;
    };
    // neoforge 必须排在 forge 前面（它包含 "forge"）。
    static const Keyword keywords[] = {
        {"neoforge", "neoforge"}, {"fabric", "fabric-loader"},
        {"forge", "forge"},       {"quilt", "quilt-loader"},
    };
    static const QRegularExpression versionPattern(QStringLiteral("(\\d+(?:\\.\\d+)+)"));
    for (const Keyword &keyword : keywords) {
        const QString word = QString::fromLatin1(keyword.word);
        const int pos = lower.indexOf(word);
        if (pos < 0) {
            continue;
        }
        QString key = QString::fromLatin1(keyword.key);
        if (key == QLatin1String("neoforge")) {
            key = neoforgeLoaderKey();
        } else if (key == QLatin1String("fabric-loader")) {
            key = fabricLoaderKey();
        } else if (key == QLatin1String("forge")) {
            key = forgeLoaderKey();
        } else {
            key = quiltLoaderKey();
        }
        const QRegularExpressionMatch match =
            versionPattern.match(lower.mid(pos + word.size()));
        return {key, match.hasMatch() ? match.captured(1) : QString()};
    }
    return {};
}

QStringList gameFolderExtractExcludes() {
    return {QStringLiteral("versions/"),
            QStringLiteral("libraries/"),
            QStringLiteral("assets/"),
            QStringLiteral("logs/"),
            QStringLiteral("crash-reports/"),
            QStringLiteral("debug/"),
            QStringLiteral(".mixin.out/"),
            QStringLiteral("PCL/"),
            QStringLiteral(".hmcl/"),
            QStringLiteral("launcher_accounts.json"),
            QStringLiteral("launcher_profiles.json"),
            QStringLiteral("launcher_accounts_microsoft.json"),
            QStringLiteral("accounts.json"),
            QStringLiteral("credential.json"),
            QStringLiteral("credentials.json"),
            QStringLiteral("dev-login.json"),
            QStringLiteral("usercache.json"),
            QStringLiteral("usernamecache.json")};
}

}  // namespace

ArchiveLayout detectArchiveLayout(const QStringList &entries) {
    ArchiveLayout layout;
    layout.rootPrefix = commonRootPrefix(entries);
    const QString &prefix = layout.rootPrefix;

    // ① 两份清单。它们在根上（或者唯一那层目录里）就是哪种格式 —— 和以前一样，
    // 只是现在允许整个包被套在一层目录里。
    layout.manifestEntry = findRelativeEntry(entries, prefix, modrinthIndexName());
    if (!layout.manifestEntry.isEmpty()) {
        layout.format = Format::Modrinth;
        layout.gameDirPrefix = prefix;
        layout.hasOverrides = hasRelativeDir(entries, prefix, QStringLiteral("overrides"));
        layout.detail = QObject::tr("里面有一份 %1").arg(modrinthIndexName());
        return layout;
    }
    layout.manifestEntry = findRelativeEntry(entries, prefix, curseForgeManifestName());
    if (!layout.manifestEntry.isEmpty()) {
        layout.format = Format::CurseForge;
        layout.gameDirPrefix = prefix;
        layout.hasOverrides = hasRelativeDir(entries, prefix, QStringLiteral("overrides"));
        layout.detail = QObject::tr("里面有一份 %1").arg(curseForgeManifestName());
        return layout;
    }

    // ② MultiMC / Prism：`mmc-pack.json` 是身份，`instance.cfg` 或 `.minecraft/` 是包装。
    layout.mmcPackEntry = findRelativeEntry(entries, prefix, QStringLiteral("mmc-pack.json"));
    layout.instanceCfgEntry = findRelativeEntry(entries, prefix, QStringLiteral("instance.cfg"));
    const bool hasDotMinecraft = hasRelativeDir(entries, prefix, QStringLiteral(".minecraft"));
    if (!layout.mmcPackEntry.isEmpty() && (!layout.instanceCfgEntry.isEmpty() || hasDotMinecraft)) {
        layout.format = Format::MultiMC;
        layout.gameDirPrefix =
            hasDotMinecraft ? prefix + QStringLiteral(".minecraft/") : prefix;
        layout.detail = QObject::tr("这是一个 MultiMC / Prism 导出的实例");
        return layout;
    }

    // A zipped mod itself is not a game folder, even if it contains config/.
    for (const QString &marker : {QStringLiteral("fabric.mod.json"),
                                 QStringLiteral("quilt.mod.json"),
                                 QStringLiteral("mcmod.info"),
                                 QStringLiteral("META-INF/mods.toml"),
                                 QStringLiteral("META-INF/neoforge.mods.toml")}) {
        if (!findRelativeEntry(entries, prefix, marker).isEmpty()) {
            layout.modsBundle = true;
            layout.detail = QObject::tr("这是一个模组文件，不是整合包");
            return layout;
        }
    }

    // ③ PCL 那种包装：根上直接躺着 `modpack.mrpack`。
    for (const QString &raw : entries) {
        const QString name = slashedEntry(raw);
        if (!name.toLower().startsWith(prefix.toLower())) {
            continue;
        }
        const QString rest = name.mid(prefix.size());
        if (rest.isEmpty() || rest.contains(QLatin1Char('/'))) {
            continue;
        }
        if (rest.toLower().endsWith(QLatin1String(".mrpack"))) {
            layout.format = Format::Wrapper;
            layout.innerPackEntry = name;
            layout.gameDirPrefix = prefix;
            layout.detail = QObject::tr("这是一个 PCL 打包的整合包（里面有一份 %1）").arg(rest);
            return layout;
        }
    }

    // Ignore sibling launchers/readmes; only import the explicitly named game folder.
    if (hasDotMinecraft) {
        layout.rootPrefix += QStringLiteral(".minecraft/");
    }

    // ④ 服务端包。**必须排在游戏目录前面**：服务端包里同样有 `mods/` 和 `config/`，
    // 按游戏目录认的话会把它当成客户端包导入。
    static const QStringList kServerMarkers = {
        QStringLiteral("server.properties"), QStringLiteral("eula.txt"),
        QStringLiteral("bukkit.yml"),        QStringLiteral("spigot.yml"),
        QStringLiteral("paper.yml"),         QStringLiteral("purpur.yml"),
        QStringLiteral("server.jar"),
    };
    for (const QString &marker : kServerMarkers) {
        if (!findRelativeEntry(entries, prefix, marker).isEmpty()) {
            layout.serverPack = true;
            layout.gameDirPrefix = prefix;
            layout.detail = QObject::tr("这是一个服务端包（里面有 %1）").arg(marker);
            return layout;
        }
    }

    // ⑤ 世界存档：`level.dat` 就在这一层。
    if (!findRelativeEntry(entries, prefix, QStringLiteral("level.dat")).isEmpty()) {
        layout.worldSave = true;
        layout.gameDirPrefix = prefix;
        layout.detail = QObject::tr("这是一个世界存档（里面有 level.dat）");
        return layout;
    }

    // ⑥ 普通 .minecraft 懒人包。判据是"游戏目录该有的东西"：`config/`、`options.txt`、
    // `servers.dat`、资源包/光影目录——**光有 `mods/` 不算**（见下一条）。
    const bool hasMods = hasRelativeDir(entries, prefix, QStringLiteral("mods"));
    const bool hasConfig = hasRelativeDir(entries, prefix, QStringLiteral("config"));
    const bool hasOptions = !findRelativeEntry(entries, prefix, QStringLiteral("options.txt")).isEmpty();
    const bool hasServersDat =
        !findRelativeEntry(entries, prefix, QStringLiteral("servers.dat")).isEmpty();
    const bool hasPacks =
        hasRelativeDir(entries, prefix, QStringLiteral("resourcepacks")) ||
        hasRelativeDir(entries, prefix, QStringLiteral("shaderpacks"));
    // `mods/` 单独出现不算（"一堆模组合集"就是这样）；但它**和 versions/ 一起**出现时
    // 就是一个真 .minecraft 目录（只是没写 options.txt/config）。
    const bool hasVersions = !versionIdsFromEntries(entries, prefix).isEmpty();
    layout.gameFolderMarkers =
        hasConfig || hasOptions || hasServersDat || hasPacks || (hasMods && hasVersions);
    if (layout.gameFolderMarkers) {
        layout.format = Format::GameFolder;
        layout.gameDirPrefix = prefix;
        layout.versionJsonEntry = pickVersionJsonEntry(entries, prefix);
        // 旧版 Technic / 老式 Forge 包把加载器塞在 `bin/modpack.jar`（有时还有
        // `bin/minecraft.jar`）。这一层**不猜**它对应哪个 Forge 版本 —— 那串文件名里没有
        // 版本号，进 jar 里读 MANIFEST 也不可靠 —— 但必须**说出来**：不说的话用户拿到的是
        // 一个纯原版实例，点启动看到的是一堆 mod 找不到 class，而不是"加载器还没装"。
        layout.legacyLauncherJar =
            !findRelativeEntry(entries, prefix, QStringLiteral("bin/modpack.jar")).isEmpty()
            || !findRelativeEntry(entries, prefix, QStringLiteral("bin/minecraft.jar")).isEmpty();
        layout.detail = layout.legacyLauncherJar
            ? QObject::tr("检测到旧版 Technic / Forge 整合包。版本清单未声明加载器，导入后需手动安装。")
            : QObject::tr("里面是一个 .minecraft 游戏目录");
        return layout;
    }

    // ⑦ 只有一个 `mods/`，或者根上就摆着几份 `.jar`：这是"别人打包好的一堆模组"，
    // **不是整合包**。按整合包导进去会建一个空实例（没有版本、没有加载器），
    // 远不如直接告诉他"把 jar 拖到实例的模组页"。
    int rootJars = 0;
    bool rootOthers = false;
    const QString prefixLower = prefix.toLower();
    for (const QString &raw : entries) {
        const QString name = slashedEntry(raw);
        if (name.endsWith(QLatin1Char('/'))) {
            continue;
        }
        const QString lower = name.toLower();
        if (!lower.startsWith(prefixLower)) {
            continue;
        }
        const QString rest = lower.mid(prefixLower.size());
        if (rest.isEmpty() || isJunkEntry(lower)) {
            continue;
        }
        if (!rest.contains(QLatin1Char('/')) && rest.endsWith(QLatin1String(".jar"))) {
            ++rootJars;
        } else {
            rootOthers = true;
        }
    }
    if ((hasMods && !layout.gameFolderMarkers) || (rootJars > 0 && !rootOthers)) {
        layout.modsBundle = true;
        layout.gameDirPrefix = prefix;
        layout.detail = QObject::tr("里面只有一堆模组，没有整合包清单");
        return layout;
    }
    // ⑦ 最后一个仍然认不出的形状里，最常见的是"好几个实例打在一起"。它照样不导入，
    // 但要**说清为什么**：一句「认不出来」会让用户以为包坏了，或者以为拖错了文件。
    layout.detail = multiInstanceDetail(entries, prefix);
    return layout;
}

bool archiveHasEntryPrefix(const QStringList &entries, const QString &prefix) {
    QString target = slashedEntry(prefix).toLower();
    while (target.endsWith(QLatin1Char('/'))) {
        target.chop(1);
    }
    if (target.isEmpty()) {
        return false;
    }
    for (const QString &raw : entries) {
        QString name = slashedEntry(raw).toLower();
        while (name.endsWith(QLatin1Char('/'))) {
            name.chop(1);
        }
        if (name == target || name.startsWith(target + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

Result parseMultiMCInstance(const QByteArray &mmcPackJson, const QByteArray &instanceCfg,
                            const QString &gameDirPrefix, const QString &fallbackName) {
    Result result;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(mmcPackJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.problem = QObject::tr("无法解析 mmc-pack.json（%1）。请检查文件是否完整。")
                             .arg(parseError.errorString());
        return result;
    }

    const QJsonArray components = doc.object().value(QStringLiteral("components")).toArray();
    if (components.isEmpty()) {
        result.problem = QCoreApplication::translate("ModpackImport", "mmc-pack.json 缺少 components，无法确定 MultiMC / Prism 实例的 Minecraft 版本。");
        return result;
    }

    QString gameVersion;
    QString loaderKey;
    QString loaderVersion;
    QSet<QString> seenComponents;
    QStringList customComponents;
    for (const QJsonValue &value : components) {
        const QJsonObject component = value.toObject();
        const QString uid = component.value(QStringLiteral("uid")).toString().trimmed();
        if (uid.isEmpty()) {
            continue;
        }
        QString version = component.value(QStringLiteral("version")).toString().trimmed();
        if (version.isEmpty()) {
            version = component.value(QStringLiteral("cachedVersion")).toString().trimmed();
        }
        const QString uidLower = uid.toLower();
        if (seenComponents.contains(uidLower)) {
            result.problem = QObject::tr("mmc-pack.json 有重复组件 %1，无法确定启动配置，没有导入。").arg(uid);
            return result;
        }
        seenComponents.insert(uidLower);
        const bool standard = uidLower == QLatin1String("net.minecraft") ||
            uidLower == QLatin1String("org.lwjgl") ||
            uidLower == QLatin1String("net.minecraftforge") ||
            uidLower == QLatin1String("net.fabricmc.fabric-loader") ||
            uidLower == QLatin1String("net.neoforged") ||
            uidLower == QLatin1String("org.quiltmc.quilt-loader");
        if (!standard) customComponents.append(uid);
        if (standard && uidLower != QLatin1String("org.lwjgl") && version.isEmpty()) {
            result.problem = QObject::tr("组件 %1 没有明确版本，不能自动选择最新版代替，没有导入。").arg(uid);
            return result;
        }
        if (standard && uidLower != QLatin1String("net.minecraft") &&
            uidLower != QLatin1String("org.lwjgl") && !loaderKey.isEmpty()) {
            result.problem = QObject::tr("这个实例同时声明多个加载器，暂不支持，没有导入。");
            return result;
        }

        if (uidLower == QLatin1String("net.minecraft")) {
            if (gameVersion.isEmpty()) {
                gameVersion = version;
            }
        } else if (uidLower == QLatin1String("net.minecraftforge")) {
            if (loaderKey.isEmpty()) {
                loaderKey = forgeLoaderKey();
                loaderVersion = normalizeForgeVersion(version, gameVersion);
            }
        } else if (uidLower == QLatin1String("net.fabricmc.fabric-loader")) {
            if (loaderKey.isEmpty()) {
                loaderKey = fabricLoaderKey();
                loaderVersion = version;
            }
        } else if (uidLower == QLatin1String("net.neoforged")) {
            if (loaderKey.isEmpty()) {
                loaderKey = neoforgeLoaderKey();
                loaderVersion = version;
            }
        } else if (uidLower == QLatin1String("org.quiltmc.quilt-loader")) {
            if (loaderKey.isEmpty()) {
                loaderKey = quiltLoaderKey();
                loaderVersion = version;
            }
        }
    }

    if (gameVersion.isEmpty()) {
        // 有的 MultiMC 导出只在 instance.cfg 里留了 IntendedVersion。
        gameVersion = instanceCfgValue(instanceCfg, QStringLiteral("IntendedVersion"));
    }
    if (gameVersion.isEmpty()) {
        result.problem = QCoreApplication::translate("ModpackImport", "这是一个 MultiMC / Prism 的实例导出（有 mmc-pack.json），但里面没写它基于哪个 Minecraft 版本（components 里没有 net.minecraft），无法安装。");
        return result;
    }
    if (!customComponents.isEmpty()) {
        result.problem = QCoreApplication::translate("ModpackImport", "这个 MultiMC / Prism 包依赖自定义启动组件：%1。\n\n目前不能转换它的 patches、专用库和 JVM 参数，因此没有导入，避免生成无法启动的实例。GTNH Java 17+ 的 lwjgl3ify 包请暂用 Prism 导入；这不影响普通标准格式整合包。")
                             .arg(customComponents.join(QCoreApplication::translate("ModpackImport", "、")));
        return result;
    }

    Pack pack;
    pack.format = Format::MultiMC;
    pack.gameVersion = gameVersion;
    pack.loaderKey = loaderKey;
    pack.loaderVersion = loaderVersion;
    // GTNH 的 forge 组件在 net.minecraft 之前就出现了（components 的次序不保证），
    // 所以上面 normalize 时 gameVersion 可能还是空的 —— 到这里再补一次。
    if (pack.loaderKey == forgeLoaderKey() && !pack.loaderVersion.isEmpty()) {
        pack.loaderVersion = normalizeForgeVersion(pack.loaderVersion, gameVersion);
    }
    pack.overridesDir = stripTrailingSlashes(gameDirPrefix);
    pack.clientOverridesDir.clear();
    pack.declaredFileCount = 0;
    pack.extractExcludes = gameFolderExtractExcludes();

    pack.name = instanceCfgValue(instanceCfg, QStringLiteral("name"));
    if (pack.name.isEmpty()) {
        pack.name = fallbackName.trimmed();
    }
    if (pack.name.isEmpty()) {
        pack.name = QObject::tr("整合包");
    }

    QString note = QCoreApplication::translate("ModpackImport", "此整合包来自 MultiMC 或 Prism。将导入 .minecraft 中的模组、配置及资源包等内容。原启动器的 patches/、libraries/ 和启动设置不会导入。");
    pack.supportNote = note;

    result.ok = true;
    result.pack = pack;
    return result;
}

Result parseGameFolderInstance(const QStringList &entries, const QByteArray &versionJson,
                               const QString &rootPrefix, const QString &fallbackName) {
    Result result;

    QJsonObject versionObject;
    if (!versionJson.isEmpty()) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(versionJson, &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            versionObject = doc.object();
        }
        // 版本 json 读不懂**不在这里拒绝**：目录名里往往也写着版本，能推出来就照装。
    }

    const QString jsonId = versionObject.value(QStringLiteral("id")).toString().trimmed();
    const QStringList versionIds = versionIdsFromEntries(entries, rootPrefix);

    QString chosenId = jsonId;
    int loaderCandidates = 0;
    for (const QString &id : versionIds) {
        if (hasLoaderKeyword(id)) ++loaderCandidates;
    }
    if (jsonId.isEmpty() && (loaderCandidates > 1 ||
        (loaderCandidates == 0 && versionIds.size() > 1))) {
        result.problem = QObject::tr("这个游戏目录包含多个候选实例，无法确定要导入哪一个。请分别打包后导入。");
        return result;
    }
    if (chosenId.isEmpty()) {
        for (const QString &id : versionIds) {
            if (hasLoaderKeyword(id)) {
                chosenId = id;
                break;
            }
        }
        if (chosenId.isEmpty() && versionIds.size() == 1) {
            chosenId = versionIds.first();
        }
    }
    if (chosenId.isEmpty() && versionIds.size() > 1) {
        result.problem = QCoreApplication::translate("ModpackImport", "找到 %1 个版本，但缺少可确定目标版本的 JSON 清单。导入已取消。\n\n请将目标版本目录及其 JSON 清单一起打包，或改用 Modrinth / CurseForge 格式。")
                             .arg(versionIds.size());
        return result;
    }

    QString gameVersion;
    const QStringList versionFields = {
        versionObject.value(QStringLiteral("clientVersion")).toString().trimmed(),
        versionObject.value(QStringLiteral("inheritsFrom")).toString().trimmed(),
        jsonId,
    };
    for (const QString &field : versionFields) {
        if (looksLikeMinecraftVersion(field)) {
            gameVersion = field;
            break;
        }
    }
    if (gameVersion.isEmpty()) {
        gameVersion = minecraftVersionFromId(chosenId);
    }
    if (gameVersion.isEmpty()) {
        for (const QString &id : versionIds) {
            gameVersion = minecraftVersionFromId(id);
            if (!gameVersion.isEmpty()) {
                break;
            }
        }
    }
    if (gameVersion.isEmpty()) {
        result.problem = QCoreApplication::translate("ModpackImport", "压缩包中有 .minecraft 目录，但 versions/ 中缺少版本 JSON。无法确定所需的 Minecraft 版本，导入已取消。\n\n请补入版本 JSON 后重试，或使用 Modrinth / CurseForge 格式。");
        return result;
    }

    QStringList libraries;
    for (const QJsonValue &value :
         versionObject.value(QStringLiteral("libraries")).toArray()) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) {
            libraries.append(name);
        }
    }
    LoaderGuess guess = loaderFromLibraries(libraries, gameVersion);
    if (guess.key.isEmpty()) {
        guess = loaderFromId(chosenId);
        if (guess.key == forgeLoaderKey() && !guess.version.isEmpty()) {
            guess.version = normalizeForgeVersion(guess.version, gameVersion);
        }
    }
    if (guess.key.isEmpty()) {
        for (const QString &id : versionIds) {
            LoaderGuess candidate = loaderFromId(id);
            if (!candidate.key.isEmpty()) {
                if (candidate.key == forgeLoaderKey() && !candidate.version.isEmpty()) {
                    candidate.version = normalizeForgeVersion(candidate.version, gameVersion);
                }
                guess = candidate;
                break;
            }
        }
    }
    if (!guess.key.isEmpty() && guess.version.isEmpty()) {
        result.problem = QObject::tr("认出了加载器，但包里没有明确的加载器版本。请提供完整版本 JSON，不能用最新版代替。");
        return result;
    }

    Pack pack;
    pack.format = Format::GameFolder;
    pack.gameVersion = gameVersion;
    pack.loaderKey = guess.key;
    pack.loaderVersion = guess.version;
    pack.overridesDir = stripTrailingSlashes(rootPrefix);
    pack.clientOverridesDir.clear();
    pack.declaredFileCount = 0;
    pack.extractExcludes = gameFolderExtractExcludes();

    pack.name = fallbackName.trimmed();
    if (pack.name.isEmpty()) {
        pack.name = chosenId;
    }
    if (pack.name.isEmpty()) {
        pack.name = QObject::tr("整合包");
    }

    if (!versionIds.isEmpty() && !jsonId.isEmpty()) {
        pack.supportNote = QCoreApplication::translate("ModpackImport", "已按清单重新安装原版游戏和加载器，跳过包内的 versions/。mods 和 config 等内容会解压到实例目录。");
    }

    result.ok = true;
    result.pack = pack;
    return result;
}

Result parseModrinthIndex(const QByteArray &json, bool allowEmptyFiles) {
    Result result;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.problem = QObject::tr("整合包清单无法解析（%1）。这个文件可能已损坏。")
                             .arg(parseError.errorString());
        return result;
    }

    const QJsonObject root = doc.object();

    // 头两条：这份 json 得**看起来就是** modrinth.index.json。`game` 和 `formatVersion`
    // 是 Mojang 无关的格式标记，缺了它们说明拿到的不是清单（比如是个空的同名文件）。
    if (!root.value(QStringLiteral("formatVersion")).isDouble() ||
        !root.value(QStringLiteral("files")).isArray()) {
        result.problem = QCoreApplication::translate("ModpackImport", "%1 缺少 formatVersion 或 files，无法作为整合包清单读取。")
                             .arg(modrinthIndexName());
        return result;
    }

    const QString game = root.value(QStringLiteral("game")).toString();
    if (game != QLatin1String("minecraft")) {
        result.problem =
            game.isEmpty()
                ? QObject::tr("%1 缺少游戏标识，无法安装。")
                      .arg(modrinthIndexName())
                : QObject::tr("这不是 Minecraft 的整合包：清单里写的是「%1」。").arg(game);
        return result;
    }

    // 基于哪个原版。没有这一条就什么也装不了（连"装个纯原版"都不知道装哪个）。
    const QJsonObject dependencies = root.value(QStringLiteral("dependencies")).toObject();
    const QString gameVersion = dependencies.value(QStringLiteral("minecraft")).toString();
    if (gameVersion.isEmpty()) {
        result.problem = QCoreApplication::translate("ModpackImport", "整合包清单缺少 dependencies.minecraft，无法确定要安装的 Minecraft 版本。");
        return result;
    }

    Pack pack;
    pack.format = Format::Modrinth;
    pack.gameVersion = gameVersion;

    pack.name = root.value(QStringLiteral("name")).toString().trimmed();
    if (pack.name.isEmpty()) {
        // 名字只是"起实例名时的候选"，没有也能装 —— 退化成清单里的版本号，再退化成一句
        // 通用的。**不**因为缺它拒绝整个包。
        pack.name = root.value(QStringLiteral("versionId")).toString().trimmed();
    }
    pack.version = root.value(QStringLiteral("versionId")).toString().trimmed();

    for (const QString &key : loaderDependencyKeys()) {
        if (dependencies.contains(key)) {
            pack.loaderKey = key;
            pack.loaderVersion = dependencies.value(key).toString();
            break;
        }
    }
    // 认不出的加载器键**也得带出去**，不能就这么丢掉。
    // NOTE: 上面那个循环只在那四个键里挑，于是 dependencies 里写着别的加载器
    // （rift-loader / liteloader / 以后新出的……）时 loaderKey 是空串 —— 而空串在
    // unsupportedLoaderText() 眼里是"纯原版整合包，放行"，最后装出来一个**进不去**
    // 的包：一堆 mod 躺在 mods/ 里，游戏按原版起。.hpp 里规定了"认不出来的键走和
    // Quilt 同一条路、不返回空串"，这一步不能悄悄绕过去。
    // 多个认不出的键时取**排序后的第一个**：这种包本身就不合规，报哪个都算报，
    // 但得报确定的那一个 —— 靠 QJsonObject 的遍历顺序挑，换一次 Qt 就可能换一个。
    if (pack.loaderKey.isEmpty()) {
        QStringList unknownKeys;
        for (auto it = dependencies.constBegin(); it != dependencies.constEnd(); ++it) {
            if (it.key() != QLatin1String("minecraft")) {
                unknownKeys.append(it.key());
            }
        }
        unknownKeys.sort();
        if (!unknownKeys.isEmpty()) {
            pack.loaderKey = unknownKeys.first();
            pack.loaderVersion = dependencies.value(pack.loaderKey).toString();
        }
    }

    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    // 和 CurseForge 那一侧同一个意思：记"清单里点了几条"，不是"解析出几条"。
    pack.declaredFileCount = files.size();
    QHash<QString, int> indexesByPath;  // 路径 -> 它在 files 里的下标（去重用）
    for (const QJsonValue &value : files) {
        if (!value.isObject()) {
            continue;  // 不是对象的条目当没有 —— 它连路径都没法说，报也报不清
        }
        const QJsonObject entry = value.toObject();

        const QString path = entry.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            continue;
        }
        // **路径不安全 = 整包拒绝**（不是跳过这一条）。理由写在 .hpp 里。
        if (!isSafeRelativePath(path)) {
            result.problem =
                QObject::tr("整合包清单中有无效路径：\n%1\n\n路径须位于实例目录内，且名称不能以句点或空格结尾。导入已取消。")
                    .arg(path);
            return result;
        }

        // `env.client` 写着 unsupported = 这个 mod 只给服务端用。装到客户端上轻则没用，
        // 重则一进游戏就崩，所以跳过它，但**记一笔**（装在哪儿、少了什么要说清楚）。
        const QString clientEnv =
            entry.value(QStringLiteral("env")).toObject().value(QStringLiteral("client")).toString();
        if (clientEnv == QLatin1String("unsupported")) {
            ++pack.filesSkipped;
            continue;
        }

        PackFile file;
        file.path = path;
        const QJsonArray downloads = entry.value(QStringLiteral("downloads")).toArray();
        for (const QJsonValue &url : downloads) {
            const QString text = url.toString();
            if (!text.isEmpty()) {
                file.urls.append(text);
            }
        }
        file.sha1 = entry.value(QStringLiteral("hashes"))
                        .toObject()
                        .value(QStringLiteral("sha1"))
                        .toString();
        // fileSize 越界（或者干脆不是数）= **当"不知道字节数"**（-1），不强转。
        // NOTE: 不能裸 `static_cast<qint64>(size.toDouble())`：越界的 double 转
        // qint64 是**未定义行为**（x86 上给出 0x8000…，一个负数）。清单是外部输入，
        // 写个 1e30 就能撞上，而那个负数会一路走进"这份文件下完了没有"的判断里 ——
        // 变成一个永远下不完的作业。0 也留着（0 字节的文件确实存在，但那份字节数由
        // sha1 那一侧兜），这里只挡"明显不是字节数"的。
        // 2^63 这个字面量在 double 里表示得**正好**，`<` 它就一定塞得进 qint64。
        constexpr double kQint64UpperBound = 9223372036854775808.0;
        const QJsonValue size = entry.value(QStringLiteral("fileSize"));
        file.size = -1;
        if (size.isDouble() && size.toDouble() >= 0.0 && size.toDouble() < kQint64UpperBound) {
            file.size = static_cast<qint64>(size.toDouble());
        }

        if (file.urls.isEmpty()) {
            // 没有下载地址。Modrinth 的规矩是这种文件由压缩包自己带（overrides 里），
            // 所以不是错误，但它**一定**要说出来 —— 万一它并不在 overrides 里，用户得知道
            // 少了什么，而不是开游戏才发现缺 mod。
            ++pack.filesSkipped;
            continue;
        }

        // 同一个路径出现两次时以**最后**一条为准。合法的清单里不会有重复（Modrinth 自己
        // 生成的那份就没有），这里只是别让两条下载写同一个文件。
        // 键要走 pathDedupKey()：Windows 上大小写不同的两条是同一个文件，见那个函数。
        const QString dedupKey = pathDedupKey(path);
        if (indexesByPath.contains(dedupKey)) {
            pack.files[indexesByPath.value(dedupKey)] = file;
            continue;
        }
        indexesByPath.insert(dedupKey, pack.files.size());
        pack.files.append(file);
    }

    if (pack.files.isEmpty() && pack.filesSkipped == 0) {
        // 清单里一条文件都没有。**严格按格式说**这不是错误，「纯 overrides 的整合包」是
        // 存在的 —— 但只有压缩包里**真的有** overrides 那一层时才算，而这里（纯逻辑层）
        // 看不到压缩包。所以默认仍然拒；调用方确认过 overrides 有内容时传
        // `allowEmptyFiles = true` 放行。
        if (!allowEmptyFiles) {
            result.problem = QCoreApplication::translate("ModpackImport", "整合包清单未列出待下载文件，导入已取消。");
            return result;
        }
    }

    result.ok = true;
    result.pack = pack;
    return result;
}

// ---------------------------------------------------------------------------
// CurseForge
// ---------------------------------------------------------------------------

namespace {

/// `modLoaders[].id` 拆开之后的形状（喂给 curseForgeLoaderKey 那一对出参）。
struct CurseForgeLoaderId {
    bool known = false;
    QString key;
    QString version;
};

CurseForgeLoaderId splitCurseForgeLoaderId(const QString &rawId) {
    CurseForgeLoaderId out;
    const QString id = rawId.trimmed();
    if (id.isEmpty()) {
        return out;
    }

    // 拆在**第一个** `-` 上（不是最后一个）：`neoForge-20.4.1-beta` 也得把后缀留在版本里。
    const int dash = id.indexOf(QLatin1Char('-'));
    const QString name = (dash < 0 ? id : id.left(dash)).trimmed();
    out.version = (dash < 0) ? QString() : id.mid(dash + 1).trimmed();

    // 名字那一半按小写认：真清单里三种写法都见过（`forge` / `neoForge` / `NeoForge`）。
    const QString lowered = name.toLower();
    if (lowered == QLatin1String("forge")) {
        out.key = forgeLoaderKey();
        out.known = true;
    } else if (lowered == QLatin1String("neoforge")) {
        out.key = neoforgeLoaderKey();
        out.known = true;
    } else if (lowered == QLatin1String("fabric")) {
        out.key = fabricLoaderKey();
        out.known = true;
    } else if (lowered == QLatin1String("quilt")) {
        out.key = quiltLoaderKey();
        out.known = true;
    } else {
        // 认不出来**也给 key**（原样那个前缀）：空串在下游是"纯原版包，放行"，
        // 于是 `liteloader-1.12.2` 会被当成纯原版装出来 —— 一堆 mod 躺在 mods/ 里，
        // 游戏按原版起。见 .hpp 里那一条。
        out.key = name;
    }
    return out;
}

/// `hashes` 里 `algo == 1` 的那一条（CurseForge 的 SHA-1；`2` 是 MD5）。
///
/// 找不到回空串 —— 空 sha1 在下载那一段是"不核 sha1、只核字节数"，不是错误。
/// NOTE: **不能拿 `hashes[0]`**：数组的次序没有承诺，而且实际回来时它常常是 md5
/// （那样下载完每一条都会被判成"校验不过"，而字节数是对的）。
QString sha1FromCurseForgeHashes(const QJsonArray &hashes) {
    for (const QJsonValue &value : hashes) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("algo")).toInt(-1) == 1) {
            return entry.value(QStringLiteral("value")).toString();
        }
    }
    return QString();
}

/// `{"fileIds":[…]}` / `{"modIds":[…]}` 那一类请求体。键名和"号是数字不是字符串"
/// 这两件事写错时接口回 400，而 400 的正文里只有一句 `{"error":"…"}` —— 那看着和
/// "这个包坏了"一模一样（见 .hpp 里为什么把请求体做成纯函数）。
QByteArray idListRequestBody(const QString &key, const QList<int> &ids) {
    QJsonArray array;
    for (const int id : ids) {
        array.append(id);
    }
    QJsonObject body;
    body[key] = array;
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

}  // namespace

bool curseForgeLoaderKey(const QString &rawId, QString *keyOut, QString *versionOut) {
    const CurseForgeLoaderId parsed = splitCurseForgeLoaderId(rawId);
    if (keyOut != nullptr) {
        *keyOut = parsed.key;
    }
    if (versionOut != nullptr) {
        *versionOut = parsed.version;
    }
    return parsed.known;
}

Result parseCurseForgeManifest(const QByteArray &json, bool allowEmptyFiles) {
    Result result;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.problem = QObject::tr("整合包清单无法解析（%1）。这个文件可能已损坏。")
                             .arg(parseError.errorString());
        return result;
    }

    const QJsonObject root = doc.object();

    // 头两条：这份 json 得**看起来就是** manifest.json。
    // `manifestVersion` + `files` 是那份清单的骨架，缺了它们说明拿到的不是清单。
    if (!root.value(QStringLiteral("manifestVersion")).isDouble() ||
        !root.value(QStringLiteral("files")).isArray()) {
        result.problem = QCoreApplication::translate("ModpackImport", "%1 缺少 manifestVersion 或 files，无法作为整合包清单读取。")
                             .arg(curseForgeManifestName());
        return result;
    }

    // `manifestType` 只在**写着别的**时才拒。CurseForge 自己生成的包一定写着
    // "minecraftModpack"，但手改过的、以及一些很老的包缺这一条 —— 缺它不是
    // "这不是整合包"的证据（上面那两条骨架才是），为它拒一个能装的包不值得。
    const QString manifestType = root.value(QStringLiteral("manifestType")).toString();
    if (!manifestType.isEmpty() && manifestType != QLatin1String("minecraftModpack")) {
        result.problem =
            QObject::tr("这不是 Minecraft 的整合包：清单里写的类型是「%1」。").arg(manifestType);
        return result;
    }

    // 基于哪个原版。没有这一条就什么也装不了（连"装个纯原版"都不知道装哪个）。
    const QJsonObject minecraft = root.value(QStringLiteral("minecraft")).toObject();
    const QString gameVersion = minecraft.value(QStringLiteral("version")).toString().trimmed();
    if (gameVersion.isEmpty()) {
        result.problem = QCoreApplication::translate("ModpackImport", "整合包清单缺少 minecraft.version，无法确定要安装的 Minecraft 版本。");
        return result;
    }

    Pack pack;
    pack.format = Format::CurseForge;
    pack.gameVersion = gameVersion;

    pack.name = root.value(QStringLiteral("name")).toString().trimmed();
    if (pack.name.isEmpty()) {
        // 名字只是"起实例名时的候选"，缺了不拒整个包（和 Modrinth 那一条同一个取舍）。
        pack.name = root.value(QStringLiteral("version")).toString().trimmed();
    }
    pack.version = root.value(QStringLiteral("version")).toString().trimmed();

    // 加载器：`modLoaders` 里挑 `primary` 那一条。CurseForge 允许多条（真有包写了不止一条），
    // 挑不出来时取**第一条**；一条都没有 = 纯原版整合包，不是错误。
    const QJsonArray loaders = minecraft.value(QStringLiteral("modLoaders")).toArray();
    QString loaderRaw;
    for (const QJsonValue &value : loaders) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty()) {
            continue;
        }
        if (entry.value(QStringLiteral("primary")).toBool()) {
            loaderRaw = id;
            break;  // primary 优先，且它就该是唯一的那条
        }
        if (loaderRaw.isEmpty()) {
            loaderRaw = id;
        }
    }
    if (!loaderRaw.isEmpty()) {
        curseForgeLoaderKey(loaderRaw, &pack.loaderKey, &pack.loaderVersion);
    }

    // 覆盖文件那一层目录名。**这是外部输入**（它会拼成一条路径前缀），所以过一遍
    // isSafeRelativePath()：不安全就退回默认的 `overrides` —— 少解几个 config 是小事，
    // 让它拼出一条能写到实例目录外面的前缀是大事。
    const QString overrides = root.value(QStringLiteral("overrides")).toString().trimmed();
    if (!overrides.isEmpty() && isSafeRelativePath(overrides)) {
        pack.overridesDir = overrides;
    }
    // CurseForge 没有 `client-overrides` 这个规矩（空串 = 不解这一层，见 extractModpackOverrides）。
    pack.clientOverridesDir.clear();

    // 文件清单。**这里一个地址都没有** —— 只有两个号，全都要去接口换（见 .hpp 顶上那段）。
    const QJsonArray fileArray = root.value(QStringLiteral("files")).toArray();
    // `declaredFileCount` 记的是**清单里点了几条**，不是解析出几条：解析不出（号缺失、
    // 小于等于零）的条目会被剔掉，而"剔光了"和"本来就没写"必须分得开 —— 前者是坏包，
    // 后者是纯 overrides 包（有的导出工具会写一个空数组）。
    pack.declaredFileCount = fileArray.size();
    QSet<int> seenFileIds;
    for (const QJsonValue &value : fileArray) {
        if (!value.isObject()) {
            continue;  // 不是对象的条目当没有：它连号都给不出，报也报不清
        }
        const QJsonObject entry = value.toObject();
        const QJsonValue projectId = entry.value(QStringLiteral("projectID"));
        const QJsonValue fileId = entry.value(QStringLiteral("fileID"));
        if (!projectId.isDouble() || !fileId.isDouble()) {
            continue;
        }
        PackRef ref;
        ref.projectId = projectId.toInt();
        ref.fileId = fileId.toInt();
        if (ref.projectId <= 0 || ref.fileId <= 0) {
            continue;
        }
        // 同一个文件列两遍 = 同一个文件下两遍（两条作业写同一个路径）。真清单里不会有，
        // 但这一手很便宜，而踩上的症状是"两个线程抢着写同一个 jar"。
        if (seenFileIds.contains(ref.fileId)) {
            continue;
        }
        seenFileIds.insert(ref.fileId);
        // `required: false` 的文件**照样下**：主流启动器都这么做，而"这个包作者认为可选的
        // 那个 mod"对用户来说通常正是包的一部分（少装它进游戏就报错）。
        pack.pendingRefs.append(ref);
    }

    if (pack.pendingRefs.isEmpty() && pack.declaredFileCount > 0) {
        // 清单点了名，却一条都读不出号。这是坏清单，不是"纯 overrides 包"。
        result.problem = QCoreApplication::translate("ModpackImport", "整合包清单有 %1 条文件记录，但均缺少有效的 projectID 或 fileID。导入已取消。")
                             .arg(pack.declaredFileCount);
        return result;
    }
    if (pack.pendingRefs.isEmpty() && !allowEmptyFiles) {
        // 一个空数组本身是合法的「纯 overrides 包」，但只有压缩包里真有 overrides 那一层
        // 时才成立 —— 这一层看不到压缩包，所以默认仍然拒（调用方确认过再放行）。
        result.problem = QCoreApplication::translate("ModpackImport", "整合包清单未列出待下载文件，导入已取消。");
        return result;
    }

    result.ok = true;
    result.pack = pack;
    return result;
}

QByteArray curseForgeFilesRequestBody(const QList<PackRef> &refs) {
    QList<int> ids;
    ids.reserve(refs.size());
    for (const PackRef &ref : refs) {
        ids.append(ref.fileId);
    }
    return idListRequestBody(QStringLiteral("fileIds"), ids);
}

QByteArray curseForgeModsRequestBody(const QList<int> &projectIds) {
    return idListRequestBody(QStringLiteral("modIds"), projectIds);
}

QString curseForgeDownloadUrl(qint64 fileId, const QString &fileName, const QString &declaredUrl) {
    if (!declaredUrl.trimmed().isEmpty()) return declaredUrl;
    if (fileId <= 0 || fileId > 9007199254740991LL || fileName.contains(QLatin1Char('/')) ||
        fileName.contains(QLatin1Char('\\')) || !isSafeRelativePath(fileName)) return {};
    // 文件 ID 以千为界拆分，不依赖 ID 长度；文件名单独编码，避免 #、?、% 改变 URL 语义。
    return QStringLiteral("https://edge.forgecdn.net/files/%1/%2/%3")
        .arg(fileId / 1000).arg(fileId % 1000)
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(fileName)));
}

CurseForgeFiles parseCurseForgeFileInfo(const QByteArray &json, const QList<PackRef> &asked) {
    CurseForgeFiles out;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        out.problem =
            QObject::tr("CurseForge 的接口返回了无法解析的内容（%1）。").arg(parseError.errorString());
        return out;
    }

    const QJsonValue data = doc.object().value(QStringLiteral("data"));
    if (!data.isArray()) {
        // 出错时那边回的是 `{"error":"…"}`（没有 data）—— 这一档和 400 是同一个形状，
        // 得让用户看见接口说了什么，而不是我们猜。
        const QString error =
            doc.object().value(QStringLiteral("error")).toString().trimmed();
        out.problem = error.isEmpty()
                          ? QObject::tr("CurseForge 的接口回的话里没有文件表。")
                          : QObject::tr("CurseForge 的接口拒绝了这一问：%1").arg(error);
        return out;
    }

    // 正文里的条目先按 fileId 归档。**不能按位置对上**：批量回来的次序没有承诺，
    // 而且查不到的号根本不会回来 —— 按位置取会静默错位，把 A 的文件名安到 B 上
    // （症状是"装完有几个 mod 是错的"，比少装可怕得多）。
    QHash<int, QJsonObject> byFileId;
    for (const QJsonValue &value : data.toArray()) {
        const QJsonObject entry = value.toObject();
        const int id = entry.value(QStringLiteral("id")).toInt(0);
        if (id > 0) {
            byFileId.insert(id, entry);
        }
    }

    QList<int> projectIds;
    QSet<int> seenProjects;
    for (const PackRef &ref : asked) {
        const auto it = byFileId.constFind(ref.fileId);
        if (it == byFileId.constEnd()) {
            ++out.missing;  // 接口里没有这一条：号写错了 / 已删 / 没过审
            out.issues.append({ref, {}, {}, {}, -1, QObject::tr("接口未返回此文件")});
            continue;
        }
        const QJsonObject entry = it.value();

        // 文件名要拼进路径（`mods/<名字>`），所以**先过安全性**：带 `/`、`\`、`..` 的
        // 当场算"查不到"（记 missing），绝不放行到写盘那一步。名字不安全的文件
        // **整个包不拒绝** —— 和 Modrinth 那份清单里写死路径的情形不同：那边路径是
        // 作者写在清单里的，这边是一个我们控制不了的文件名。
        const QString fileName = entry.value(QStringLiteral("fileName")).toString();
        if (fileName.isEmpty() || !isSafeRelativePath(fileName)) {
            ++out.missing;
            out.issues.append({ref, fileName, {}, {}, -1, QObject::tr("文件名缺失或路径不安全")});
            continue;
        }

        // 空链接不单独推断作者意图；明确不可用则不构造候选，更不伪报已下载。
        if (entry.value(QStringLiteral("isAvailable")).isBool() &&
            !entry.value(QStringLiteral("isAvailable")).toBool()) {
            ++out.notDistributable;
            const QJsonValue size = entry.value(QStringLiteral("fileLength"));
            const double length = size.toDouble(-1);
            out.issues.append({ref, fileName, {},
                sha1FromCurseForgeHashes(entry.value(QStringLiteral("hashes")).toArray()),
                length >= 0 && length < 9223372036854775808.0 ? static_cast<qint64>(length) : -1,
                QObject::tr("平台将文件标记为不可用")});
            continue;
        }
        const QString url = curseForgeDownloadUrl(ref.fileId, fileName,
            entry.value(QStringLiteral("downloadUrl")).toString());
        if (url.isEmpty()) {
            ++out.missing;
            out.issues.append({ref, fileName, {}, {}, -1, QObject::tr("无法生成安全下载地址")});
            continue;
        }

        CurseForgeFile file;
        file.file.urls.append(url);
        file.file.sha1 = sha1FromCurseForgeHashes(entry.value(QStringLiteral("hashes")).toArray());
        // `fileLength` 越界（或者干脆不是数）= 当"不知道字节数"（-1）。和 Modrinth 那边
        // 同一个取舍：越界的 double 转 qint64 是未定义行为，而负数会变成一个永远下不完的作业。
        constexpr double kQint64UpperBound = 9223372036854775808.0;
        const QJsonValue size = entry.value(QStringLiteral("fileLength"));
        file.file.size = -1;
        if (size.isDouble() && size.toDouble() >= 0.0 && size.toDouble() < kQint64UpperBound) {
            file.file.size = static_cast<qint64>(size.toDouble());
        }
        // `path` 里此刻只有一个文件名 —— 目录要拿 modId 去问 classId，见 placeCurseForgeFiles()。
        file.file.path = fileName;
        file.modId = entry.value(QStringLiteral("modId")).toInt(0);
        file.fileId = ref.fileId;
        file.file.curseForgeProjectId = ref.projectId;
        file.file.curseForgeFileId = ref.fileId;
        out.resolved.append(file);

        if (file.modId > 0 && !seenProjects.contains(file.modId)) {
            seenProjects.insert(file.modId);
            projectIds.append(file.modId);
        }
    }
    out.projectIds = projectIds;
    return out;
}

QString curseForgeFilePath(int classId) {
    // 类目号来自 CurseForge 自己（`classId`，九条 `isClass=true` 里我们用得上的那几条）：
    // 6 模组 / 12 资源包 / 6552 光影 / 6945 数据包 / 17 存档 / 4546 Customization /
    // 4471 整合包。
    switch (classId) {
    case 12:    // Resource Packs
    case 4546:  // Customization（披风、皮肤那一类贴图包，本质就是资源包）
        return QStringLiteral("resourcepacks/");
    case 6552:  // Shaders
        return QStringLiteral("shaderpacks/");
    case 6945:  // Data Packs —— 和下载页那一类的落点**必须一致**（见
                // ResourceCatalog::folderForKind）：同一个东西两条路放进两个目录，
                // 症状是"从下载页下的能生效、从整合包里来的不生效"。
                // 注意原版只认存档里那份（`<存档>/datapacks/`），这里的产物要用户自己搬进存档 ——
                // 和 ResourceCatalog 那边同一句话，别在这儿说成"装好了"。
        return QStringLiteral("datapacks/");
    case 17:    // Worlds（存档）
        return QStringLiteral("saves/");
    case 4471:  // Modpacks —— 嵌套的整合包，**不代装**
        return QString();
    default:
        // 6（Mods）和"认不出来"合在一起，理由写在 .hpp 里。
        return QStringLiteral("mods/");
    }
}

CurseForgePlacement placeCurseForgeFiles(const QList<CurseForgeFile> &resolved,
                                         const QHash<int, int> &classIdByProject) {
    CurseForgePlacement out;

    QHash<QString, int> indexesByPath;  // 去重键 -> 它在 out.files 里的下标
    for (const CurseForgeFile &entry : resolved) {
        // 拿不到类目号（那一发没问到这一项）就按"认不出来"落 mods/ —— 不该让文件消失。
        const int classId = classIdByProject.value(entry.modId, 0);
        const QString dir = curseForgeFilePath(classId);
        if (dir.isEmpty()) {
            ++out.skipped;  // 嵌套的整合包
            continue;
        }

        PackFile file = entry.file;
        file.path = dir + entry.file.path;
        // 拼完**再判一次**：文件名单判过一次，但加上目录之后整条可能超长
        // （isSafeRelativePath 的长度上限是按整条路径算的）。
        if (!isSafeRelativePath(file.path)) {
            ++out.missing;
            out.issues.append({{entry.modId, entry.fileId}, entry.file.path, {}, file.sha1,
                file.size, QObject::tr("目标路径超长或不安全")});
            continue;
        }

        // 同一个路径出现两次时以**最后**一条为准（和 Modrinth 那一份同一个取舍）：
        // 文件名是外部输入，两个项目给出同名文件是可能的，而两条同时写一个路径
        // 就是数据竞争。键走 pathDedupKey()（Windows 上大小写不同的两条是同一个文件）。
        const QString dedupKey = pathDedupKey(file.path);
        if (indexesByPath.contains(dedupKey)) {
            out.files[indexesByPath.value(dedupKey)] = file;
            continue;
        }
        indexesByPath.insert(dedupKey, out.files.size());
        out.files.append(file);
    }
    return out;
}

QString curseForgeFileWebUrl(const FileIssue &issue) {
    const QUrl url(issue.projectWebUrl);
    static const QRegularExpression path(QStringLiteral("^/minecraft/(mc-mods|modpacks|texture-packs|shaders|worlds|customization|data-packs)/[a-zA-Z0-9_-]+/?$"));
    if (issue.ref.projectId <= 0 || issue.ref.fileId <= 0 || url.scheme() != QStringLiteral("https") ||
        url.host() != QStringLiteral("www.curseforge.com") || url.port() != -1 ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
        !path.match(url.path()).hasMatch()) return {};
    QString base = url.toString();
    if (base.endsWith(QLatin1Char('/'))) base.chop(1);
    return base + QStringLiteral("/files/%1").arg(issue.ref.fileId);
}

QHash<int, QString> parseCurseForgeProjectWebUrls(const QByteArray &json) {
    QHash<int, QString> out;
    const auto data = QJsonDocument::fromJson(json).object().value(QStringLiteral("data")).toArray();
    for (const auto &value : data) {
        const auto entry = value.toObject();
        FileIssue probe;
        probe.ref = {entry.value(QStringLiteral("id")).toInt(), 1};
        probe.projectWebUrl = entry.value(QStringLiteral("links")).toObject().value(QStringLiteral("websiteUrl")).toString();
        if (!curseForgeFileWebUrl(probe).isEmpty()) out.insert(probe.ref.projectId, probe.projectWebUrl);
    }
    return out;
}

QHash<int, int> parseCurseForgeClassIds(const QByteArray &json, QString *problem) {
    const auto fail = [problem](const QString &why) {
        if (problem != nullptr) {
            *problem = why;
        }
        return QHash<int, int>();
    };

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QObject::tr("CurseForge 的接口返回了无法解析的内容（%1）。")
                        .arg(parseError.errorString()));
    }
    const QJsonValue data = doc.object().value(QStringLiteral("data"));
    if (!data.isArray()) {
        return fail(QObject::tr("CurseForge 的接口回的话里没有项目表。"));
    }

    QHash<int, int> out;
    for (const QJsonValue &value : data.toArray()) {
        const QJsonObject entry = value.toObject();
        const int id = entry.value(QStringLiteral("id")).toInt(0);
        // `classId` 缺了就**不放进表里**（等于"认不出来"，落 mods/）——
        // 填一个 0 进去会静默落 mods/，而"表里根本没有"也是落 mods/，两件事一样，但
        // 少了这一条后面就分不出"接口说它是 0"和"接口没说"。
        if (id <= 0 || !entry.contains(QStringLiteral("classId"))) {
            continue;
        }
        out.insert(id, entry.value(QStringLiteral("classId")).toInt(0));
    }
    return out;
}

QString curseForgeApiBase(const QString &apiKey, const QString &override) {
    // 验证工程的口子（`Launcher/CurseForgeApiBase`）：产品**从不写这个键**，
    // 但填了它就得最优先 —— 那是"把接口指到本机的假服务器上"的唯一办法。
    QString base = override.trimmed();
    if (base.isEmpty()) {
        // 没填 key 时走 `mod.mcimirror.top/curseforge/v1`：它是**照着官方那份做的镜像**
        // （`{"data":[…]}`、`pagination`、`downloadUrl`、`hashes`、
        // `latestFilesIndexes` 一层不差），所以上层一个字都不用改。这条路用到的每一发
        // （搜索、版本列表、单文件、两个批量 POST、分类表）都通。
        // · ★ **结尾那个 `/v1` 一个字符都不能少**。这个函数的约定是"返回到 `/v1` 为止的
        // 那一段"，调用点只往后接 `/mods/search` / `/mods/files` / `/mods/{id}/files` ——
        // 少这一段就变成 `/curseforge/mods/search`，**每一发都是 404**。
        // 它坑人的地方在于**症状和"服务挂了"长得一模一样**（CurseForge 那一栏搜不出
        // 东西、整合包导入说换不到文件），而这条路的每一发都是拿假服务器验的 ——
        // 本机那个假接口的 base 就是 `…:/v1`，所以少一段这种事**只有真网络那一跑能看见**。
        // · **它也可能哪天就没了**：换的时候只改下面这一行。
        base = apiKey.trimmed().isEmpty()
                   ? QStringLiteral("https://mod.mcimirror.top/curseforge/v1")
                   : QStringLiteral("https://api.curseforge.com/v1");
    }
    // 结尾那个 `/` 去掉：拼 URL 的地方一律自己带上它（`%1/mods/%2/files/%3`），
    // 两种写法都收下，免得用户在设置里多打或者少打一个斜杠就 404。
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return base;
}

}  // namespace ModpackImport

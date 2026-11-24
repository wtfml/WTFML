#include "ModpackImport.hpp"
#include <QCoreApplication>
#include "InstanceFiles.hpp"
#include "launcher/LibraryRules.hpp"
#include "util/ZipUtil.hpp"
#include "util/DirectorySessionLock.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <algorithm>

namespace ModpackImport {
namespace {
bool httpsUrl(const QString &text) {
    const QUrl url(text);
    return url.isValid() && url.scheme() == QLatin1String("https") && !url.host().isEmpty()
        && url.userInfo().isEmpty() && url.fragment().isEmpty();
}
QString sha1(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}
bool sha1Valid(const QString &value) {
    static const QRegularExpression hash(QStringLiteral("^[a-fA-F0-9]{40}$"));
    return hash.match(value).hasMatch();
}
bool jvmArguments(const QJsonArray &input, QJsonArray &output) {
    if (input.size() > 256) return false;
    // A deliberately bounded subset, not a shell/JVM command passthrough.
    static const QRegularExpression module(QStringLiteral("^[A-Za-z0-9_.]+/[A-Za-z0-9_.]+=ALL-UNNAMED(?:,java.naming)?$"));
    for (int i = 0; i < input.size(); ++i) {
        if (!input.at(i).isString()) return false;
        const QString arg = input.at(i).toString();
        if (arg.size() > 1024) return false;
        if (arg == QLatin1String("-Dfile.encoding=UTF-8") ||
            arg == QLatin1String("-Djava.system.class.loader=com.gtnewhorizons.retrofuturabootstrap.RfbSystemClassLoader") ||
            arg == QLatin1String("--enable-native-access=ALL-UNNAMED")) {
            output.append(arg);
        } else if (arg == QLatin1String("--add-opens") || arg == QLatin1String("--add-exports")) {
            if (++i >= input.size() || !input.at(i).isString() || !module.match(input.at(i).toString()).hasMatch()) return false;
            output.append(arg); output.append(input.at(i));
        } else if (arg == QLatin1String("--enable-native-access")) {
            if (++i >= input.size() || input.at(i).toString() != QLatin1String("ALL-UNNAMED")) return false;
            output.append(QStringLiteral("--enable-native-access=ALL-UNNAMED"));
        } else return false;
    }
    return true;
}

/// 已知、但这一层**明确不支持**的补丁字段：回一句"它是什么、为什么不能当没看见"。
///
/// 这些字段不是"以后顺手加一个解析就能支持"的东西 —— 每一个都要求启动器侧多一种能力
/// （把 jar 合并进游戏本体、额外的 natives、行为提示），只解析不实现比拒绝更糟：包会装上、
/// 启动时才炸，而用户看不出是哪一步被忽略了。所以认得出就点名说清楚。
QString unsupportedFieldReason(const QString &field) {
    if (field == QLatin1String("+jarMods")) {
        return QObject::tr("这个包用了 +jarMods（旧式核心 mod）：它要求把 jar 合并进游戏本体，本启动器不做这一步，装上也起不来。");
    }
    if (field == QLatin1String("+files")) {
        return QObject::tr("整合包包含旧版 +files 文件表。该表依赖原启动器确定目标路径，当前导入流程仅处理 +libraries。");
    }
    if (field == QLatin1String("addnatives")) {
        return QObject::tr("这个包用了 addnatives（额外 natives）：本启动器只从版本 json 的 libraries 里取 natives。");
    }
    if (field == QLatin1String("+traits")) {
        return QObject::tr("此整合包使用了 +traits（例如 legacyLaunch）来指定启动方式。当前版本不支持导入这些启动设置。");
    }
    if (field == QLatin1String("+luac")) {
        return QObject::tr("此整合包使用当前版本不支持的 +luac 补丁（如 LiteLoader）。");
    }
    return QString();
}
}

Result parsePatchedMultiMCInstance(const QByteArray &manifest, const QByteArray &config,
    const QHash<QString, QByteArray> &patches, const QHash<QString, QByteArray> &bundledFiles,
    const QString &gameDirPrefix, const QString &fallbackName) {
    Result result;
    result.pack.format = Format::MultiMC;
    const auto fail = [&](const QString &why) { result.problem = why; return result; };
    const QJsonDocument doc = QJsonDocument::fromJson(manifest);
    if (!doc.isObject() || doc.object().value("formatVersion").toInt(-1) != 1 ||
        !doc.object().value("components").isArray()) return fail(QCoreApplication::translate("MultiMCPatchImport", "不支持的 MultiMC 清单格式。"));
    const QJsonArray components = doc.object().value("components").toArray();
    if (components.isEmpty() || components.size() > 64) return fail(QCoreApplication::translate("MultiMCPatchImport", "组件数量不合法。"));
    QHash<QString, QString> versions;
    QList<QJsonObject> ordered;
    QString gameVersion, loader, loaderVersion;
    const QSet<QString> allowedFields{"formatVersion", "uid", "version", "name", "order", "releaseTime", "type",
        "volatile", "requires", "conflicts", "compatibleJavaMajors", "libraries", "mainClass", "mainJar",
        "assetIndex", "minecraftArguments", "+tweakers", "+jvmArgs"};
    for (const auto &value : components) {
        if (!value.isObject()) return fail(QCoreApplication::translate("MultiMCPatchImport", "组件不是对象。"));
        const QJsonObject component = value.toObject();
        const QString uid = component.value("uid").toString();
        const QString version = component.value("version").toString(component.value("cachedVersion").toString());
        if (uid.isEmpty() || versions.contains(uid) || !patches.contains(uid))
            return fail(QCoreApplication::translate("MultiMCPatchImport", "组件重复或缺少本地启动补丁：%1").arg(uid));
        const QJsonDocument patchDoc = QJsonDocument::fromJson(patches.value(uid));
        if (!patchDoc.isObject()) return fail(QCoreApplication::translate("MultiMCPatchImport", "补丁 JSON 损坏：%1").arg(uid));
        const QJsonObject patch = patchDoc.object();
        if (patch.value("formatVersion").toInt(-1) != 1 || patch.value("uid").toString() != uid ||
            patch.value("version").toString().isEmpty() || (!version.isEmpty() && patch.value("version").toString() != version))
            return fail(QCoreApplication::translate("MultiMCPatchImport", "补丁身份或版本与清单不一致：%1").arg(uid));
        if (patch.contains("order") && (!patch.value("order").isDouble() ||
            patch.value("order").toDouble() != patch.value("order").toInt())) return fail(QCoreApplication::translate("MultiMCPatchImport", "补丁顺序无效。"));
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            if (allowedFields.contains(it.key())) {
                continue;
            }
            // 认得出的那几种点名说清楚（为什么不能只解析、以及它要求启动器多做什么）；
            // 认不出来的保持原话。两种都不静默忽略 —— 包会装上、启动时才炸。
            const QString reason = unsupportedFieldReason(it.key());
            return fail(reason.isEmpty()
                            ? QCoreApplication::translate("MultiMCPatchImport", "尚不支持补丁字段 %1，未忽略它。").arg(it.key())
                            : QCoreApplication::translate("MultiMCPatchImport", "%1（字段 %2，未忽略它。）").arg(reason, it.key()));
        }
        versions.insert(uid, patch.value("version").toString());
        ordered.append(patch);
        if (uid == QLatin1String("net.minecraft")) gameVersion = versions.value(uid);
        const QString key = uid == QLatin1String("net.minecraftforge") ? forgeLoaderKey() :
            uid == QLatin1String("net.fabricmc.fabric-loader") ? fabricLoaderKey() :
            uid == QLatin1String("net.neoforged") ? neoforgeLoaderKey() :
            uid == QLatin1String("org.quiltmc.quilt-loader") ? quiltLoaderKey() : QString();
        if (!key.isEmpty()) {
            if (!loader.isEmpty()) return fail(QCoreApplication::translate("MultiMCPatchImport", "不能同时导入多个加载器。"));
            loader = key; loaderVersion = versions.value(uid);
        }
    }
    if (gameVersion.isEmpty()) return fail(QCoreApplication::translate("MultiMCPatchImport", "没有 Minecraft 组件。"));
    // Preserve the ordinary parser's path exclusions/name handling without accepting custom components there.
    const QJsonObject baseComponent{{"uid", "net.minecraft"}, {"version", gameVersion}};
    result = parseMultiMCInstance(QJsonDocument(QJsonObject{{"components", QJsonArray{baseComponent}}}).toJson(),
                                 config, gameDirPrefix, fallbackName);
    if (!result.ok) return result;
    result.ok = false;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.value("order").toInt() < b.value("order").toInt(); });
    QJsonObject runtime;
    QJsonArray libraries, jvm, tweakers;
    QSet<int> compatibleJava;
    bool hasJavaDeclaration = false;
    QHash<QString, int> libraryKeys;
    const QString archiveRoot = gameDirPrefix.endsWith(QLatin1String(".minecraft/"))
        ? gameDirPrefix.left(gameDirPrefix.size() - 11) : QString();
    qint64 bundledTotal = 0;
    for (const QJsonObject &patch : ordered) {
        for (const QString &field : {QStringLiteral("requires"), QStringLiteral("conflicts")}) {
            if (patch.contains(field) && !patch.value(field).isArray()) return fail(QCoreApplication::translate("MultiMCPatchImport", "组件依赖字段不是数组。"));
            for (const auto &value : patch.value(field).toArray()) {
                const auto rule = value.toObject();
                const QString uid = rule.value("uid").toString();
                if (uid.isEmpty() || (field == QLatin1String("conflicts") && versions.contains(uid)) ||
                    (field == QLatin1String("requires") && (!versions.contains(uid) ||
                     (rule.contains("equals") && rule.value("equals").toString() != versions.value(uid)))))
                    return fail(QCoreApplication::translate("MultiMCPatchImport", "补丁组件依赖冲突：%1").arg(uid));
            }
        }
        for (const QString &key : {QStringLiteral("mainClass"), QStringLiteral("minecraftArguments"), QStringLiteral("assetIndex")}) {
            if (patch.contains(key)) runtime[key] = patch.value(key);
        }
        if (patch.contains("mainJar")) {
            const auto artifact = patch.value("mainJar").toObject().value("downloads").toObject().value("artifact").toObject();
            if (!httpsUrl(artifact.value("url").toString()) || !sha1Valid(artifact.value("sha1").toString()))
                return fail(QCoreApplication::translate("MultiMCPatchImport", "客户端来源缺少 HTTPS 或 SHA-1。"));
            // Never download a modified game client declared by an arbitrary patch.
            const QString host = QUrl(artifact.value("url").toString()).host();
            if (host != QLatin1String("launcher.mojang.com") && host != QLatin1String("piston-data.mojang.com"))
                return fail(QCoreApplication::translate("MultiMCPatchImport", "客户端必须来自 Mojang 官方文件源。"));
            runtime["downloads"] = QJsonObject{{"client", artifact}};
        }
        if (patch.contains("compatibleJavaMajors")) {
            int minimum = 100;
            QSet<int> declared;
            for (const auto &value : patch.value("compatibleJavaMajors").toArray()) {
                const int major = value.toInt(-1);
                if (major < 8 || major > 99) return fail(QCoreApplication::translate("MultiMCPatchImport", "Java 版本声明无效。"));
                minimum = qMin(minimum, major);
                declared.insert(major);
            }
            if (minimum == 100) return fail(QCoreApplication::translate("MultiMCPatchImport", "Java 版本列表为空。"));
            if (hasJavaDeclaration) compatibleJava.intersect(declared); else compatibleJava = declared;
            hasJavaDeclaration = true;
            if (compatibleJava.isEmpty()) return fail(QCoreApplication::translate("MultiMCPatchImport", "补丁之间的 Java 要求没有交集。"));
        }
        if (patch.contains("+jvmArgs") && (!patch.value("+jvmArgs").isArray() || !jvmArguments(patch.value("+jvmArgs").toArray(), jvm)))
            return fail(QCoreApplication::translate("MultiMCPatchImport", "补丁包含暂不支持或不安全的 JVM 参数，没有执行。"));
        if (patch.contains("+tweakers") && !patch.value("+tweakers").isArray()) return fail(QCoreApplication::translate("MultiMCPatchImport", "Tweaker 字段无效。"));
        for (const auto &value : patch.value("+tweakers").toArray()) {
            static const QRegularExpression className(QStringLiteral("^[A-Za-z_$][A-Za-z0-9_$.]*$"));
            if (!value.isString() || !className.match(value.toString()).hasMatch()) return fail(QCoreApplication::translate("MultiMCPatchImport", "Tweaker 类名无效。"));
            tweakers.append(QStringLiteral("--tweakClass")); tweakers.append(value);
        }
        if (patch.contains("libraries") && !patch.value("libraries").isArray()) return fail(QCoreApplication::translate("MultiMCPatchImport", "库清单无效。"));
        for (const auto &value : patch.value("libraries").toArray()) {
            if (!value.isObject() || libraries.size() >= 4096) return fail(QCoreApplication::translate("MultiMCPatchImport", "库清单过大或损坏。"));
            auto lib = value.toObject();
            QString name = lib.value("name").toString();
            QStringList coordinate = name.split(QLatin1Char(':'));
            if (coordinate.size() < 3) return fail(QCoreApplication::translate("MultiMCPatchImport", "无效 Maven 坐标：%1").arg(name));
            // Prism encodes native classifiers as artifact suffixes and adds broad OS rules.
            // Translate to Mojang coordinates with explicit architecture so x86 DLLs cannot shadow x64.
            if (coordinate.at(0) == QLatin1String("org.lwjgl") && coordinate.at(1).contains(QLatin1String("-natives-"))) {
                const int split = coordinate.at(1).indexOf(QLatin1String("-natives-"));
                const QString classifier = coordinate.at(1).mid(split + 1);
                coordinate[1] = coordinate.at(1).left(split);
                coordinate = {coordinate.at(0), coordinate.at(1), coordinate.at(2), classifier};
                name = coordinate.join(QLatin1Char(':')); lib["name"] = name;
                const QString os = classifier.contains("windows") ? "windows" : classifier.contains("macos") ? "osx" : "linux";
                const QString arch = classifier.endsWith("arm64") ? "arm64" : classifier.endsWith("arm32") ? "arm" : classifier.endsWith("x86") ? "x86" : "x86_64";
                lib["rules"] = QJsonArray{QJsonObject{{"action", "allow"}, {"os", QJsonObject{{"name", os}, {"arch", arch}}}}};
            }
            const QString path = LibraryRules::mavenPathForName(name);
            if (path.isEmpty() || !isSafeRelativePath(path)) return fail(QCoreApplication::translate("MultiMCPatchImport", "库路径不安全：%1").arg(name));
            auto downloads = lib.value("downloads").toObject();
            auto artifact = downloads.value("artifact").toObject();
            if (lib.value("MMC-hint").toString() == QLatin1String("local")) {
                const QString fileName = QFileInfo(path).fileName();
                const QString entry = archiveRoot + "libraries/" + fileName;
                const QByteArray bytes = bundledFiles.value(entry);
                bundledTotal += bytes.size();
                if (bytes.isEmpty() || bytes.size() > 64 * 1024 * 1024 || bundledTotal > 128 * 1024 * 1024)
                    return fail(QCoreApplication::translate("MultiMCPatchImport", "包内专用库缺失或超过大小限制：%1").arg(entry));
                result.pack.bundledLibraries.append({path, bytes, sha1(bytes)});
                lib.remove("downloads"); lib.remove("url"); lib.remove("MMC-hint");
                lib["sha1"] = sha1(bytes); lib["size"] = bytes.size();
            } else if (!artifact.isEmpty()) {
                if (!httpsUrl(artifact.value("url").toString()) || !sha1Valid(artifact.value("sha1").toString()))
                    return fail(QCoreApplication::translate("MultiMCPatchImport", "专用库缺少安全地址或 SHA-1：%1").arg(name));
                artifact["path"] = path; downloads["artifact"] = artifact; lib["downloads"] = downloads;
            } else if (lib.contains("natives")) {
                // Retain official classifier downloads, filling their missing Maven-relative paths.
                auto classifiers = downloads.value("classifiers").toObject();
                if (classifiers.isEmpty()) return fail(QCoreApplication::translate("MultiMCPatchImport", "Native 库没有可校验的 classifier 清单。"));
                for (auto it = classifiers.begin(); it != classifiers.end(); ++it) {
                    auto item = it.value().toObject();
                    if (!httpsUrl(item.value("url").toString()) || !sha1Valid(item.value("sha1").toString())) return fail(QCoreApplication::translate("MultiMCPatchImport", "Native 库校验信息缺失。"));
                    const QString classifierPath = LibraryRules::mavenPathForName(name + ':' + it.key());
                    if (classifierPath.isEmpty()) return fail(QCoreApplication::translate("MultiMCPatchImport", "Native 库路径无效。"));
                    item["path"] = classifierPath; it.value() = item;
                }
                downloads["classifiers"] = classifiers; lib["downloads"] = downloads;
            } else {
                const QString base = lib.value("url").toString("https://libraries.minecraft.net/");
                if (!httpsUrl(base)) return fail(QCoreApplication::translate("MultiMCPatchImport", "Maven 仓库地址不安全。"));
                lib["url"] = base;
            }
            const QString key = coordinate.at(0) + ':' + coordinate.at(1) + ':' + (coordinate.size() > 3 ? coordinate.at(3) : QString());
            if (libraryKeys.contains(key)) libraries[libraryKeys.value(key)] = lib;
            else { libraryKeys.insert(key, libraries.size()); libraries.append(lib); }
        }
    }
    static const QRegularExpression mainClass(QStringLiteral("^[A-Za-z_$][A-Za-z0-9_$.]*$"));
    if (!mainClass.match(runtime.value("mainClass").toString()).hasMatch() ||
        !runtime.contains("downloads") || !runtime.value("assetIndex").isObject()) return fail(QCoreApplication::translate("MultiMCPatchImport", "补丁缺少完整主类、官方客户端或资源索引。"));
    const auto assets = runtime.value("assetIndex").toObject();
    if (!httpsUrl(assets.value("url").toString()) || !sha1Valid(assets.value("sha1").toString())) return fail(QCoreApplication::translate("MultiMCPatchImport", "资源索引缺少安全来源或校验。"));
    runtime["libraries"] = libraries;
    if (hasJavaDeclaration) {
        QList<int> majors = compatibleJava.values(); std::sort(majors.begin(), majors.end());
        QJsonArray javaMajors; for (const int major : majors) javaMajors.append(major);
        runtime["javaVersion"] = QJsonObject{{"majorVersion", majors.first()}};
        runtime["compatibleJavaMajors"] = javaMajors;
    }
    QString gameArgs = runtime.value("minecraftArguments").toString();
    for (const auto &value : tweakers) gameArgs += ' ' + value.toString();
    runtime["minecraftArguments"] = gameArgs;
    runtime["arguments"] = QJsonObject{{"jvm", jvm}};
    result.pack.patchedRuntime = runtime;
    result.pack.loaderKey = loader; result.pack.loaderVersion = loaderVersion;
    result.pack.supportNote = QCoreApplication::translate("MultiMCPatchImport", "已转换包内声明式启动补丁、专用库、Java 要求和 JVM 参数；不执行来源启动器或前后置命令。");
    result.ok = true;
    return result;
}

bool publishBundledLibraries(const Pack &pack, const QString &minecraftRoot, QString *problem) {
    const auto fail = [&](const QString &why) { if (problem) *problem = why; return false; };
    if (!DirectorySessionLock::acquire(minecraftRoot, problem)) return false;
    for (const auto &library : pack.bundledLibraries) {
        const QString relative = "libraries/" + library.path;
        if (!isSafeRelativePath(relative) || sha1(library.bytes) != library.sha1 ||
            InstanceFiles::pathHasLinkComponent(minecraftRoot, relative)) return fail(QCoreApplication::translate("MultiMCPatchImport", "包内库路径或校验不安全。"));
        const QString destination = QDir(minecraftRoot).filePath(relative);
        if (QFileInfo::exists(destination)) {
            QFile existing(destination);
            if (!existing.open(QIODevice::ReadOnly) || existing.size() != library.bytes.size() || sha1(existing.readAll()) != library.sha1)
                return fail(QCoreApplication::translate("MultiMCPatchImport", "已有同名专用库内容不同，未覆盖：%1").arg(destination));
            continue;
        }
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || InstanceFiles::pathHasLinkComponent(minecraftRoot, relative))
            return fail(QCoreApplication::translate("MultiMCPatchImport", "无法安全创建专用库目录。"));
        QSaveFile file(destination);
        if (!file.open(QIODevice::WriteOnly) || file.write(library.bytes) != library.bytes.size() || !file.commit())
            return fail(QCoreApplication::translate("MultiMCPatchImport", "专用库保存失败：%1").arg(destination));
    }
    return true;
}

bool preserveMultiMCSource(const QString &zipPath, const QString &instanceDir) {
    QStringList entries;
    QString error;
    if (ZipUtil::listEntries(zipPath, &entries, &error) != ZipUtil::EntryStatus::Ok) return false;
    QString root;
    for (const QString &entry : entries) {
        if (entry.endsWith(QStringLiteral("mmc-pack.json"))) {
            root = entry.left(entry.size() - int(QStringLiteral("mmc-pack.json").size()));
            break;
        }
    }
    if (root.isEmpty()) return false;
    const QString target = instanceDir + QStringLiteral("/.wtfml/pack-source");
    bool wrote = false;
    for (const QString &entry : entries) {
        const bool wanted = entry.startsWith(root + QStringLiteral("patches/"))
            || entry == root + QStringLiteral("mmc-pack.json")
            || entry == root + QStringLiteral("instance.cfg");
        if (!wanted) continue;
        QByteArray bytes;
        if (ZipUtil::readEntry(zipPath, entry, &bytes, &error, QCoreApplication::translate("MultiMCPatchImport", "保存包身份"),
                               8 * 1024 * 1024) != ZipUtil::EntryStatus::Ok) {
            continue;
        }
        const QString relative = entry.mid(root.size());
        const QString out = target + QLatin1Char('/') + relative;
        if (!QDir().mkpath(QFileInfo(out).absolutePath())) continue;
        QFile file(out);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) wrote = true;
    }
    return wrote;
}

}  // namespace ModpackImport

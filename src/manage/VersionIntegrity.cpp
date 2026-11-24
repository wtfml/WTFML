/**
 * @file VersionIntegrity.cpp
 * @brief 「体检」那套判据的实现（只报不装，见头文件里那几段说明）。
 */

#include "VersionIntegrity.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>

#include "../util/AppPaths.hpp"
#include "InstanceFiles.hpp"

namespace VersionIntegrity {

QString groupName(Group group) {
    switch (group) {
    case Group::ClientJar:
        return QCoreApplication::translate("VersionIntegrity", "客户端");
    case Group::Libraries:
        return QCoreApplication::translate("VersionIntegrity", "库");
    case Group::AssetIndex:
        return QCoreApplication::translate("VersionIntegrity", "资源索引");
    case Group::AssetObjects:
        return QCoreApplication::translate("VersionIntegrity", "资源对象");
    }
    return QCoreApplication::translate("VersionIntegrity", "其它");
}

QList<Group> allGroups() {
    // 顺序 = plan() 列清单的顺序 = 报告里该出现的顺序。**唯一出处**。
    return {Group::ClientJar, Group::Libraries, Group::AssetIndex, Group::AssetObjects};
}

int groupIndex(Group group) {
    return static_cast<int>(allGroups().indexOf(group));
}

QString Verdict::detail() const {
    // 每一个分支都要有一句人话：这一段直接被界面拿去显示（或者被验证工程拿去断言），
    // 落回"未知错误"就白体检了。
    switch (status) {
    case Status::Ok:
        return QCoreApplication::translate("VersionIntegrity", "完好");
    case Status::Missing:
        return QCoreApplication::translate("VersionIntegrity", "缺少文件");
    case Status::Empty:
        return QCoreApplication::translate("VersionIntegrity", "0 字节（下载写了一半留下的）");
    case Status::WrongSize:
        return QCoreApplication::translate("VersionIntegrity", "大小 %1 字节，应为 %2").arg(actualSize).arg(expectedSize);
    case Status::WrongSha1:
        return QCoreApplication::translate("VersionIntegrity", "SHA-1 校验失败");
    case Status::Unreadable:
        return QCoreApplication::translate("VersionIntegrity", "文件无法打开或读取失败");
    }
    return QCoreApplication::translate("VersionIntegrity", "未知");
}

int Report::missing() const {
    int count = 0;
    for (const Verdict &v : problems) {
        if (v.status == Status::Missing || v.status == Status::Empty) {
            ++count;
        }
    }
    return count;
}

int Report::corrupt() const {
    // 数"缺"和"坏"这两档的办法**只有这两处**（missing() / corrupt()）。别在界面那边按
    // status 再数一遍 —— 那样以后往 Status 里加一档时，只有一个地方记得改。
    return problems.size() - missing();
}

QStringList Report::problemPaths() const {
    // 只把 path 摘出来给补全用（见头文件）。**不做去重**：problems 里同一个路径不会出现两次
    // （plan() 一段一段地列，每个文件只列一条），去重那一层留给真正在乎的人
    // （DownloadManager::fileAlreadyValid() 就是按 contains 问的，重不重复都不影响它）。
    QStringList paths;
    paths.reserve(problems.size());
    for (const Verdict &v : problems) {
        paths.append(v.path);
    }
    return paths;
}

QString Report::summary() const {
    if (!error.isEmpty()) {
        return error;
    }
    if (problems.isEmpty()) {
        return QCoreApplication::translate("VersionIntegrity", "%1 个文件都完好").arg(checked);
    }
    return QCoreApplication::translate("VersionIntegrity", "%1 个文件里 %2 个要修：缺 %3 个、坏 %4 个")
        .arg(checked)
        .arg(problems.size())
        .arg(missing())
        .arg(corrupt());
}

// ---------- 按组数（见头文件那段 NOTE：只数 problems）----------

int Report::missingIn(Group group) const {
    int count = 0;
    for (const Verdict &v : problems) {
        if (v.group == group && (v.status == Status::Missing || v.status == Status::Empty)) {
            ++count;
        }
    }
    return count;
}

int Report::corruptIn(Group group) const {
    // 和 corrupt() 一样：**别在别处按 status 再数一遍**。这里挑出这一组的条数，再减去其中
    // 算"缺"的那部分 —— "缺"的判据只有 missingIn() 一处。
    int count = 0;
    for (const Verdict &v : problems) {
        if (v.group == group) {
            ++count;
        }
    }
    return count - missingIn(group);
}

int Report::problemsIn(Group group) const {
    return missingIn(group) + corruptIn(group);
}

QList<Verdict> Report::problemsInGroup(Group group) const {
    QList<Verdict> out;
    for (const Verdict &v : problems) {
        if (v.group == group) {
            out.append(v);
        }
    }
    return out;
}

namespace {

/// 资源对象那一档的判据（见头文件）：只看在不在、字节数对不对，**不哈希**。
/// @param objects 索引 json 里那份 objects 表（`{"<逻辑名>": {"hash": …, "size": …}}`）。
void appendAssetObjects(QList<ExpectedFile> &files, const QJsonObject &objects) {
    files.reserve(files.size() + objects.size());
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const QJsonObject object = it.value().toObject();
        const QString hash = object.value(QStringLiteral("hash")).toString();
        // 长度不是 40 的条目装配侧也是直接跳过（parseAndDownloadAssets 里那一句 qWarning），
        // 这里跟着跳：体检报一个"装配时根本不会去下"的文件，只会是假的告警。
        if (hash.size() != 40) {
            continue;
        }
        // 落点和装配侧同一条规矩：objects/<hash 前两位>/<hash>。文件名就是 sha1，所以
        // 没有"名字对不上"这一档 —— label 用逻辑名，人认得出是哪个资源。
        files.append({QCoreApplication::translate("VersionIntegrity", "资源对象 %1").arg(it.key()),
                      AppPaths::assetsDir() + QStringLiteral("/objects/") + hash.left(2)
                          + QLatin1Char('/') + hash,
                      // 判据里**故意**不带 sha1：见头文件里"资源对象不做哈希"那一段。
                      // 文件路径本身就是那个 sha1，再哈希一遍等于把同一件事算两遍，
                      // 而且是这 3000 个文件里最贵的那一步。
                      QString(), object.value(QStringLiteral("size")).toVariant().toLongLong(),
                      Group::AssetObjects});
    }
}

}  // namespace

QList<ExpectedFile> plan(const QJsonObject &mergedJson, const QString &versionId,
                         const QList<Library> &libraries, const AssetIndex &assetIndex) {
    QList<ExpectedFile> files;

    // ① 客户端 jar。地址和哈希来自 downloads.client，**落点**由 InstanceFiles 说了算 ——
    // 装出来的加载器版本自己没有 jar（继承父版本那一份），jarPath() 会指到主人那边去。
    // 这里和 registerVersionFiles() 一样判"有 client 这个对象"，不判 url 空不空：
    // url 空只说明这一份下不下来，而"盘上有没有那个文件"照样是个说得出口的问题。
    const QJsonObject client =
        mergedJson.value(QStringLiteral("downloads")).toObject().value(QStringLiteral("client"))
            .toObject();
    if (!client.isEmpty()) {
        files.append({QCoreApplication::translate("VersionIntegrity", "客户端 jar"),
                      InstanceFiles::jarPath(versionId),
                      client.value(QStringLiteral("sha1")).toString(),
                      client.value(QStringLiteral("size")).toVariant().toLongLong(),
                      Group::ClientJar});
    }

    // ② 库。**全体实例共用** <数据目录>/libraries 那一份（见 registerVersionFiles 里那段
    // NOTE）。path 空的跳过 —— downloadLibraries() 也是这么跳的，那种库装配时落不了盘。
    for (const Library &lib : libraries) {
        if (lib.path.isEmpty()) {
            continue;
        }
        files.append({lib.name, AppPaths::libraryDir() + QLatin1Char('/') + lib.path, lib.sha1,
                      lib.size, Group::Libraries});
    }

    // ③ 资源索引那个 json。url / id 有一个是空的时候装配侧一句都不下（downloadAssets()
    // 开头那条早退），所以这里也不列 —— 列了就是无中生有的一条"缺文件"。
    if (!assetIndex.id.isEmpty() && !assetIndex.url.isEmpty()) {
        files.append({QCoreApplication::translate("VersionIntegrity", "资源索引 %1").arg(assetIndex.id),
                      AppPaths::assetsDir() + QStringLiteral("/indexes/") + assetIndex.id
                          + QStringLiteral(".json"),
                      assetIndex.sha1, assetIndex.size, Group::AssetIndex});
    }

    // ④ 资源对象（索引里那份 objects 表）。这是这一层**唯一**的读盘：读的就是上面那条
    // 索引文件。读不出来（不存在、或者不是 json）时一条都不列 —— 索引缺了这件事本身
    // 已经在清单上了，用户修它就够，不用再叠 3000 条"资源对象 xx 缺失"的噪声。
    if (!assetIndex.id.isEmpty()) {
        QFile indexFile(AppPaths::assetsDir() + QStringLiteral("/indexes/") + assetIndex.id
                        + QStringLiteral(".json"));
        if (indexFile.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(indexFile.readAll(), &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
                appendAssetObjects(files, doc.object().value(QStringLiteral("objects")).toObject());
            }
        }
    }

    return files;
}

Verdict check(const ExpectedFile &file) {
    Verdict verdict;
    verdict.label = file.label;
    verdict.path = file.path;
    verdict.expectedSize = file.size;
    // 分组一路带过来（plan() 在列清单那一刻就知道）：界面按组摊结果时就靠它，
    // 不用回头去认 label 字符串（见头文件 Group 那段 NOTE）。
    verdict.group = file.group;

    const QFileInfo info(file.path);
    if (!info.exists() || !info.isFile()) {
        verdict.status = Status::Missing;
        return verdict;
    }
    verdict.actualSize = info.size();
    // "0 字节永远不算数"：和 DownloadManager::fileAlreadyValid() 里那条同一个规矩、同一个
    // 理由 —— 一个被中断的下载留下的空文件，看字节数是看不出毛病的。这一条要在比字节数
    // **之前**，否则 expectedSize 恰好是 0（判据里没有这一档）的时候就漏掉了。
    if (verdict.actualSize == 0) {
        verdict.status = Status::Empty;
        return verdict;
    }
    // size <= 0 = 判据里没有字节数这一档（资源对象之外，装配侧也会给 -1）。
    if (file.size > 0 && verdict.actualSize != file.size) {
        verdict.status = Status::WrongSize;
        return verdict;
    }
    if (!file.sha1.isEmpty()) {
        const QString actual = DownloadManager::calculateFileSha1(file.path);
        if (actual.isEmpty()) {
            // 读不出来（文件被独占、读到一半出错）：**不能**当成"内容对不上" —— 那两句话
            // 给用户的下一步不一样（一个是关掉游戏再试，一个是重下）。
            verdict.status = Status::Unreadable;
            return verdict;
        }
        if (actual != file.sha1) {
            verdict.status = Status::WrongSha1;
            return verdict;
        }
    }
    return verdict;  // status 保持 Ok
}

QString plainReport(const Report &report, const QString &instanceName) {
    // 行尾统一用 \n（不走 QTextStream 的本地行尾）：这一份是往剪贴板/GitHub issue 里贴的，
    // 在两台机器之间传的时候别掺进 \r 去。
    QStringList lines;
    lines << QCoreApplication::translate("VersionIntegrity", "实例「%1」完整性检查").arg(instanceName);
    // summary() 自己会把 error 原样回出来，所以这一行就把"没跑起来"说清楚了。
    lines << report.summary();
    if (!report.error.isEmpty()) {
        return lines.join(QLatin1Char('\n'));
    }
    for (Group group : allGroups()) {
        const QList<Verdict> problems = report.problemsInGroup(group);
        lines << QCoreApplication::translate("VersionIntegrity", "%1（%2 个要修）").arg(groupName(group)).arg(problems.size());
        for (const Verdict &v : problems) {
            lines << QCoreApplication::translate("VersionIntegrity", "  %1：%2").arg(v.label, v.detail());
            lines << QStringLiteral("    %1").arg(v.path);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace VersionIntegrity

/**
 * @file ServerListFile.cpp
 * @brief 实现 ServerListFile.hpp 里那三个入口（改 / 加 / 删）和 `pathFor()`。
 * @note 见 ServerListFile.hpp 的说明。这里只留实现上的坑。
 */

#include "ServerListFile.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include "util/WriteDirectoryGuard.hpp"
#include <QUuid>

#include "util/AppPaths.hpp"
#include "util/NbtTree.hpp"
#include "util/ZipUtil.hpp"

namespace {

/// 上限和 `NbtUtil` 那一份同量级（那边 16 MB，真机上这份文件是几百字节到几十 KB ——
/// `build/tmp-run` 里最大的一份 37 KB，那个尺寸全是每台服务器一格 8 KB 的 `icon`）。
/// 写这条路多这一道是因为**它要在内存里建整棵树**：一份几百 MB 的东西进来，
/// 光是 parse 就会把内存吃光。16 MB 是同一个数，`NbtTree` 自己那一层还会再卡一次。
constexpr qint64 kMaxFileSize = 16 * 1024 * 1024;

/// gzip 的两个魔数字节（`1f 8b`）—— 和 `NbtUtil` / `WorldFiles` 认的是同两位。
constexpr quint8 kGzipMagic0 = 0x1F;
constexpr quint8 kGzipMagic1 = 0x8B;

bool looksGzipped(const QByteArray &bytes) {
    return bytes.size() >= 2 && static_cast<uchar>(bytes.at(0)) == kGzipMagic0 &&
           static_cast<uchar>(bytes.at(1)) == kGzipMagic1;
}

/// 中间产物一律摆**系统临时目录**（同一个理由见 WorldFiles.cpp 里那段：实例目录是用户
/// 看得见也备得了份的地方，往那儿摆一份中间产物只会让人以为那是游戏的东西）。
QString tempPathFor(const QString &tag) {
    return QDir::tempPath() + QStringLiteral("/.wtfml-") + tag + QLatin1Char('-') +
           QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".tmp");
}

bool readWholeFile(const QString &path, QByteArray *out, QString *problem) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *problem = QCoreApplication::translate("ServerListFile", "读不了 %1：%2").arg(path, file.errorString());
        return false;
    }
    *out = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *problem = QCoreApplication::translate("ServerListFile", "读 %1 读到一半出错了（%2）")
                       .arg(path, file.errorString());
        return false;
    }
    return true;
}

/// 把一句"为什么不写"记进 @p problemOut，并且**一律返回 false** —— 三个入口里每一条拒绝
/// 的路都是这两件事，写成一行才不容易出现"报了错却返回了 true"。
bool failWith(QString *problemOut, const QString &why) {
    if (problemOut != nullptr) {
        *problemOut = why;
    }
    return false;
}

/// 取一格字符串（那一格不在 / 不是字符串 → 空串）。
QString stringField(const NbtTree::Node &compound, const QString &name) {
    const NbtTree::Node *node = NbtTree::field(compound, name);
    if (node == nullptr || node->type != NbtTree::Type::String) {
        return QString();
    }
    return node->text;
}

/// 把一格**字符串**写成 @p text；那一格在盘上根本没有时按 `TAG_String` **补建**一格
/// （类型和位置照着原版自己写的形状来 —— 名字那一格排在 `ip` 后面）。
///
/// NOTE: `append()` 之后必须**重新取一次**那一格的地址（`QList` 可能把内存搬走）——
/// 和 `WorldFiles.cpp` 里 `writeFieldOrCreate()` 是同一个坑、同一个写法。
bool setStringField(NbtTree::Node *compound, const QString &name, const QString &text) {
    if (NbtTree::Node *node = NbtTree::field(compound, name)) {
        if (node->type != NbtTree::Type::String) {
            return false;  // 形状不对：不动它（见文件头第 2 条纪律）
        }
        node->text = text;
        return true;
    }
    NbtTree::Field fresh;
    fresh.first = name;
    fresh.second.type = NbtTree::Type::String;
    fresh.second.text = text;
    compound->fields.append(fresh);
    return true;
}

/// 把整份文件（含 gzip 那一层）落回 @p target，走 QSaveFile 原子替换。
bool commitBytes(const QString &target, const QByteArray &body, bool gzipped, QString *problem) {
    if (!WriteDirectoryGuard::acquire(target, problem)) return false;
    // 原来压着的那一份要压回去：直接落裸 NBT 的话，游戏（它读的是
    // `NbtIo.readCompressed`）会把这个文件当成坏的。
    QString tempGz;
    if (gzipped) {
        const QString tempPlain = tempPathFor(QStringLiteral("serverlist"));
        QFile plain(tempPlain);
        if (!plain.open(QIODevice::WriteOnly)) {
            *problem = QCoreApplication::translate("ServerListFile", "无法创建临时文件：%1").arg(plain.errorString());
            return false;
        }
        const qint64 written = plain.write(body);
        plain.close();
        if (written != body.size()) {
            QFile::remove(tempPlain);
            *problem = QCoreApplication::translate("ServerListFile", "临时文件没写全（盘满了？）");
            return false;
        }
        tempGz = tempPathFor(QStringLiteral("serverlist-gz"));
        QString why;
        const bool compressed = ZipUtil::gzipFrom(tempPlain, tempGz, &why);
        QFile::remove(tempPlain);
        if (!compressed) {
            QFile::remove(tempGz);
            *problem = QCoreApplication::translate("ServerListFile", "压缩 servers.dat 失败了：%1").arg(why);
            return false;
        }
    }

    // QSaveFile：在目标所在的目录里开自己的临时文件，`commit()` 一步换掉 —— 真·原子替换。
    // **不用 `QFile::rename()`**：Qt 6.10 的它走的是 `::MoveFile`，不带 REPLACE_EXISTING，
    // 目标已存在时就是失败（`WorldFiles::writeLevelData()` 那段 ★★ 里查到过底）。
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        QFile::remove(tempGz);
        *problem = QCoreApplication::translate("ServerListFile", "无法写入 servers.dat：%1").arg(out.errorString());
        return false;
    }

    bool wroteOk = false;
    if (gzipped) {
        QFile gz(tempGz);
        if (gz.open(QIODevice::ReadOnly)) {
            wroteOk = true;
            char buffer[64 * 1024];
            while (true) {
                const qint64 got = gz.read(buffer, static_cast<qint64>(sizeof(buffer)));
                // 读到尾（0）和读出错（-1）是两件事，理由见 WorldFiles.cpp 里那段 ★：
                // 并成一个出口的话，"读到一半失败"会被当成"抄完了"，一份截断的 gzip
                // 照样原子地盖上去，而界面上报成功。
                if (got < 0) {
                    wroteOk = false;
                    break;
                }
                if (got == 0) {
                    break;
                }
                if (out.write(buffer, got) != got) {
                    wroteOk = false;
                    break;
                }
            }
            gz.close();
        }
        QFile::remove(tempGz);
        if (!wroteOk) {
            out.cancelWriting();
            *problem = QCoreApplication::translate("ServerListFile", "无法用压缩文件替换 servers.dat。请检查磁盘空间和写入权限。");
            return false;
        }
    } else if (out.write(body) != body.size()) {
        out.cancelWriting();
        *problem = QCoreApplication::translate("ServerListFile", "临时文件没写全（盘满了？）");
        return false;
    }

    if (!out.commit()) {
        *problem = QCoreApplication::translate("ServerListFile", "换掉 servers.dat 失败了：%1").arg(out.errorString());
        return false;
    }
    return true;
}

/// 一份读进来、并且已经认出 `servers` 那一格的 `servers.dat`。
///
/// `servers` 是指进 `document` 里的 —— 两者**必须一起用**（别把那个指针一个人带出去，
/// 它指向的东西跟着 `document` 走）。
struct LoadedList {
    NbtTree::Document document;
    NbtTree::Node *servers = nullptr;
    /// 原来是 gzip 的就得 gzip 写回去（见文件头「写回去的形状跟着读进来那一份」）。
    bool gzipped = false;
};

/// 把 @p datPath 读进来，认出并校验 `servers` 那一格。三个入口共用的那一段开头 ——
/// 它现在只是把公开那两口（`loadDocument` / `serversListOf`）串起来，**加上自己那句提示**。
///
/// 认不出（文件不在 / 太大 / 读不动 / gzip 解不开 / parse 不过 / `servers` 那一格不是
/// 一张服务器表）一律返回 false 并在 @p problem 里留一句给人看的话式**一个字节都不写**。
/// NOTE: 文件不在这一档对"改"和"删"是失败，对"加"**不是**（见 `addEntry()`）—— 所以这一条
/// 留在这儿由 `addEntry()` 自己在调用之前先判一次，别的入口直接用返回值。
bool loadServers(const QString &datPath, LoadedList *out, QString *problem) {
    QString why;
    // 那两口在 `namespace ServerListFile` 里，而这一段在它**外面**（匿名命名空间）——
    // 所以要写全名。摆在外面是因为它是"读进来 + 认出那一格"这一段的实现，和三个入口同层。
    const ServerListFile::FileState state =
        ServerListFile::loadDocument(datPath, &out->document, &out->gzipped, &why);
    if (state == ServerListFile::FileState::Missing) {
        // 三个入口共用这一句，所以不能写成"先在游戏里加一台服务器"（那是**改**那一条路该
        // 给的提示，"删"读起来就成了让人去加一台再加了删）。只说清"为什么现在没得动"。
        *problem =
            QCoreApplication::translate("ServerListFile", "此实例尚无 servers.dat。进入游戏添加服务器后将生成该文件。");
        return false;
    }
    if (state == ServerListFile::FileState::Unreadable) {
        *problem = why;
        return false;
    }
    return ServerListFile::serversListOf(&out->document, &out->servers, problem);
}

/// 认"要动的是哪一条"，回来的是它在 `servers` 那个 list 里的**下标**。
///
/// 认的是**值不是下标**：`NbtUtil::readServerList()` 会跳过没有 `ip` 的元素，所以界面上
/// 第 N 行**未必**是盘上第 N 个元素。两趟：第一趟要名字 + 地址都逐字对上；对不上再退一步
/// 只按地址找（用户可能在游戏里顺手把名字改了）。认不准就返回 false，**一个字都不写**。
bool findEntryIndex(const NbtTree::Node &servers, const QString &name, const QString &address,
                    int *indexOut, QString *problem) {
    for (int i = 0; i < servers.items.size(); ++i) {
        const NbtTree::Node &item = servers.items.at(i);
        if (item.type == NbtTree::Type::Compound &&
            stringField(item, QStringLiteral("ip")) == address &&
            stringField(item, QStringLiteral("name")) == name) {
            *indexOut = i;
            return true;
        }
    }

    int addressOnly = 0;
    int lastIndex = -1;
    for (int i = 0; i < servers.items.size(); ++i) {
        const NbtTree::Node &item = servers.items.at(i);
        if (item.type != NbtTree::Type::Compound ||
            stringField(item, QStringLiteral("ip")) != address) {
            continue;
        }
        ++addressOnly;
        lastIndex = i;
    }
    if (addressOnly == 0) {
        *problem =
            QCoreApplication::translate("ServerListFile", "servers.dat 中找不到地址为「%1」的条目。请刷新服务器列表。")
                .arg(address);
        return false;
    }
    if (addressOnly > 1) {
        // 两台同名同地址的服务器在盘上是合法的，但"名字没对上"时挑哪一条就不再唯一。
        // 与其猜一个，不如说清楚。
        *problem =
            QCoreApplication::translate("ServerListFile", "servers.dat 中有 %1 条记录使用此地址，无法确定要修改的条目。")
                .arg(addressOnly);
        return false;
    }
    *indexOut = lastIndex;
    return true;
}

}  // namespace

namespace ServerListFile {

QString pathFor(const QString &versionId) {
    return AppPaths::versionDir(versionId) + QStringLiteral("/servers.dat");
}

NbtUtil::Status read(const QString &datPath, QList<NbtUtil::ServerEntry> *out,
                     QString *problemOut) {
    return NbtUtil::readServerList(datPath, out, problemOut);
}

// ---------------------------------------------------------------------------
// 预设那一段用的那几口（见 .hpp 里那一段的开头）
// ---------------------------------------------------------------------------

FileState loadDocument(const QString &datPath, NbtTree::Document *out, bool *gzipped,
                       QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const QFileInfo info(datPath);
    if (!info.exists()) {
        return FileState::Missing;
    }
    if (info.isDir()) {
        // 同名的一个目录摆在那儿：既不是文件也不是"还没有" —— 这一档谁都不许动它。
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerListFile", "servers.dat 所在位置是目录，未执行修改。");
        }
        return FileState::Unreadable;
    }
    if (info.size() > kMaxFileSize) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("ServerListFile", "servers.dat 大小为 %1 MB，超出处理上限，未执行修改。")
                              .arg(QString::number(info.size() / (1024.0 * 1024.0), 'f', 1));
        }
        return FileState::Unreadable;
    }

    // ---------- 读进来（gzip 那一档先解开） ----------
    QByteArray bytes;
    QString problem;
    if (!readWholeFile(datPath, &bytes, &problem)) {
        if (problemOut != nullptr) {
            *problemOut = problem;
        }
        return FileState::Unreadable;
    }
    const bool compressed = looksGzipped(bytes);
    if (compressed) {
        // 解压走 `ZipUtil::gunzipTo()`（临时文件那一路）：gzip 只有那一处实现，理由见
        // `ZipUtil.hpp` 里那一句"别把它当通用解压用"。
        const QString tempPlain = tempPathFor(QStringLiteral("serverlist-read"));
        QString why;
        if (!ZipUtil::gunzipTo(datPath, tempPlain, &why)) {
            if (problemOut != nullptr) {
                *problemOut = why.isEmpty()
                                  ? QCoreApplication::translate("ServerListFile", "无法解压 servers.dat 中的 gzip 数据。")
                                  : QCoreApplication::translate("ServerListFile", "无法读取 servers.dat：%1").arg(why);
            }
            return FileState::Unreadable;
        }
        const bool got = readWholeFile(tempPlain, &bytes, &problem);
        QFile::remove(tempPlain);
        if (!got) {
            if (problemOut != nullptr) {
                *problemOut = problem;
            }
            return FileState::Unreadable;
        }
    }

    // ---------- 解开整棵树 ----------
    if (!NbtTree::parse(bytes, out, &problem)) {
        if (problemOut != nullptr) {
            // `NbtTree::parse()` 拒绝的每一档（类型不认识 / 长度越界 / 嵌套太深 / 末尾多
            // 出来一截）都算"我们看不懂这一份" —— 不留"部分解出来"的树，也就没有下文。
            *problemOut = QCoreApplication::translate("ServerListFile", "无法读取 servers.dat：%1").arg(problem);
        }
        return FileState::Unreadable;
    }
    if (gzipped != nullptr) {
        *gzipped = compressed;
    }
    return FileState::Ok;
}

bool serversListOf(NbtTree::Document *document, NbtTree::Node **out, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    NbtTree::Node *servers = NbtTree::field(&document->root, QStringLiteral("servers"));
    if (servers == nullptr || servers->type != NbtTree::Type::List) {
        if (problemOut != nullptr) {
            *problemOut =
                QCoreApplication::translate("ServerListFile", "servers.dat 的 servers 字段类型无效，文件未修改。");
        }
        return false;
    }
    // 空表（游戏里把服务器删光之后写出来的那个形状）**元素类型是 End** —— 这是合法的
    // NBT，但它只说明"这一格现在一条都没有"，不说明以后塞进去的是什么。而写盘那一步
    // （`NbtTree::serialize()`）是按 `itemType` 落的：End 的那一张一旦被 `addEntry()` /
    // 预设往里 append 了一条，写出去的就是 `List(End, count>0)` —— 游戏和本启动器都读不
    // 回来（下一次打开就是"这份文件坏了"）。
    //
    // 所以**在原地补上元素类型**：表是空的，没有任何元素受影响，补这一下是无损的。
    if (servers->items.isEmpty()) {
        servers->itemType = NbtTree::Type::Compound;
    }
    if (servers->itemType != NbtTree::Type::Compound) {
        if (problemOut != nullptr) {
            *problemOut =
                QCoreApplication::translate("ServerListFile", "servers.dat 的 servers 字段类型无效，文件未修改。");
        }
        return false;
    }
    if (out != nullptr) {
        *out = servers;
    }
    return true;
}

NbtTree::Document emptyDocument() {
    NbtTree::Document document;
    // 无名根（`level.dat` 也是无名的；`servers.dat` 这份游戏自己写的也匿名）。
    document.rootName = QString();
    document.root.type = NbtTree::Type::Compound;

    NbtTree::Field serversField;
    serversField.first = QStringLiteral("servers");
    serversField.second.type = NbtTree::Type::List;
    // 空的表**也要带着元素类型**：少了它写回去的是一张"元素类型是 End"的表，而那是坏的
    // （`NbtTree::serialize()` 按 `itemType` 落盘）。
    serversField.second.itemType = NbtTree::Type::Compound;
    document.root.fields.append(serversField);
    return document;
}

NbtTree::Node entryNode(const QString &name, const QString &address) {
    NbtTree::Node item;
    item.type = NbtTree::Type::Compound;

    NbtTree::Field addressField;
    addressField.first = QStringLiteral("ip");
    addressField.second.type = NbtTree::Type::String;
    addressField.second.text = address;
    item.fields.append(addressField);

    NbtTree::Field nameField;
    nameField.first = QStringLiteral("name");
    nameField.second.type = NbtTree::Type::String;
    nameField.second.text = name;
    item.fields.append(nameField);

    return item;
}

bool storeDocument(const QString &datPath, const NbtTree::Document &document, bool gzipped,
                   QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    return commitBytes(datPath, NbtTree::serialize(document), gzipped, problemOut);
}

bool updateEntry(const QString &datPath, const QString &oldName, const QString &oldAddress,
                 const QString &newName, const QString &newAddress, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (datPath.trimmed().isEmpty()) {
        return failWith(problemOut, QCoreApplication::translate("ServerListFile", "没有指定要改哪个 servers.dat。"));
    }
    if (newAddress.trimmed().isEmpty()) {
        return failWith(
            problemOut,
            QCoreApplication::translate("ServerListFile", "服务器地址不能为空。"));
    }

    LoadedList loaded;
    QString problem;
    if (!loadServers(datPath, &loaded, &problem)) {
        return failWith(problemOut, problem);
    }

    // ---------- 认"要改的是哪一条"（见 findEntryIndex 的 ★） ----------
    int index = -1;
    if (!findEntryIndex(*loaded.servers, oldName, oldAddress, &index, &problem)) {
        return failWith(problemOut, problem);
    }

    // ---------- 只动这两格 ----------
    NbtTree::Node *hit = &loaded.servers->items[index];
    if (!setStringField(hit, QStringLiteral("ip"), newAddress)) {
        return failWith(problemOut,
                        QCoreApplication::translate("ServerListFile", "servers.dat 的 ip 字段不是字符串，已保留原值。"));
    }
    if (!setStringField(hit, QStringLiteral("name"), newName)) {
        return failWith(problemOut,
                        QCoreApplication::translate("ServerListFile", "servers.dat 的 name 字段不是字符串，已保留原值。"));
    }

    return commitBytes(datPath, NbtTree::serialize(loaded.document), loaded.gzipped, problemOut);
}

bool addEntry(const QString &datPath, const QString &name, const QString &address,
              QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (datPath.trimmed().isEmpty()) {
        return failWith(problemOut, QCoreApplication::translate("ServerListFile", "没有指定要加到哪个 servers.dat。"));
    }
    if (address.trimmed().isEmpty()) {
        return failWith(
            problemOut,
            QCoreApplication::translate("ServerListFile", "服务器地址不能为空。"));
    }

    const QFileInfo info(datPath);
    if (info.isDir()) {
        // 同名的一个目录摆在那儿：既不是文件也不是"还没有"，不动它。
        return failWith(problemOut,
                        QCoreApplication::translate("ServerListFile", "servers.dat 所在位置是目录，未执行修改。"));
    }

    if (!info.exists()) {
        // ---------- 第一次在这个实例里加服务器：从零建一棵游戏认得出来的树 ----------
        //
        // 形状是照着一份"游戏自己写的"对着建的（见 `emptyDocument()`）—— 那一段实现只有
        // 一处，预设那一段"目标上还没有 servers.dat"时走的也是它。
        NbtTree::Document document = emptyDocument();
        NbtTree::Node *servers = &document.root.fields.last().second;
        servers->items.append(entryNode(name, address));

        // gzip（不是跟着"原来那份"—— 没有原来那份）。见文件头那段。
        return commitBytes(datPath, NbtTree::serialize(document), kFreshFileGzipped, problemOut);
    }

    LoadedList loaded;
    QString problem;
    if (!loadServers(datPath, &loaded, &problem)) {
        return failWith(problemOut, problem);
    }
    // 接在表尾（原版在游戏里点"添加服务器"也是接到尾巴上）。这一格本身是空表也照接。
    loaded.servers->items.append(entryNode(name, address));

    return commitBytes(datPath, NbtTree::serialize(loaded.document), loaded.gzipped, problemOut);
}

bool removeEntry(const QString &datPath, const QString &name, const QString &address,
                 QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (datPath.trimmed().isEmpty()) {
        return failWith(problemOut, QCoreApplication::translate("ServerListFile", "没有指定要从哪个 servers.dat 里删。"));
    }

    LoadedList loaded;
    QString problem;
    if (!loadServers(datPath, &loaded, &problem)) {
        return failWith(problemOut, problem);
    }

    int index = -1;
    if (!findEntryIndex(*loaded.servers, name, address, &index, &problem)) {
        return failWith(problemOut, problem);
    }

    // 删到空表就留一张空表（不是把文件删掉）—— 见 .hpp 那条 NOTE。
    loaded.servers->items.removeAt(index);

    return commitBytes(datPath, NbtTree::serialize(loaded.document), loaded.gzipped, problemOut);
}

bool removeEntries(const QString &datPath, const QList<QPair<QString, QString>> &entries,
                   QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (datPath.trimmed().isEmpty() || entries.isEmpty()) {
        return failWith(problemOut, QCoreApplication::translate("ServerListFile", "没有指定要从哪个 servers.dat 里删，或者没有选中任何一条。"));
    }

    LoadedList loaded;
    QString problem;
    if (!loadServers(datPath, &loaded, &problem)) {
        return failWith(problemOut, problem);
    }
    // 每删一条树就变一次，所以逐条在**当前这棵树**上重新认；同名同地址的两条各删一个。
    for (const auto &entry : entries) {
        int index = -1;
        if (!findEntryIndex(*loaded.servers, entry.first, entry.second, &index, &problem)) {
            return failWith(problemOut, problem);
        }
        loaded.servers->items.removeAt(index);
    }
    return commitBytes(datPath, NbtTree::serialize(loaded.document), loaded.gzipped, problemOut);
}

}  // namespace ServerListFile

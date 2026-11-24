/**
 * @file NbtUtil.cpp
 * @brief NbtUtil 的实现：一个只读到够用的最小 NBT 读取器，外加 gzip 那一档的接线。
 * @note 判据与三种结果的分别写在 .hpp 的块注释里；这里只有实现，几处 NOTE 说的是
 * 「为什么不能照直觉改」。
 */
#include "NbtUtil.hpp"
#include <QCoreApplication>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstring>

#include "ZipUtil.hpp"

namespace NbtUtil {
namespace {

/// 一份 .dat 最多按几 MB 算。
///
/// NOTE: 真机上 servers.dat 是**几百字节**（几十条服务器也就几 KB），16 MB 是给
/// 「用户自己塞进来的怪文件」留的余量。这道闸在**读进来之前**就得落下：
/// NBT 里每一个长度都是 int32，先按 2 GB 分配再喊「读不动」是来不及的。
constexpr qint64 kMaxFileSize = 16LL * 1024 * 1024;

/// 复合/列表的嵌套深度上限。真实 NBT 深不过十几层；这一条挡的是**构造出来的
/// 深层自指结构** —— 没有它，下面那几个递归函数会被一份文件把栈吃光。
constexpr int kMaxDepth = 64;

/// NBT 的标签类型号（大端序文件里那个 1 字节的表）。
constexpr quint8 kTagEnd = 0;
constexpr quint8 kTagByte = 1;
constexpr quint8 kTagShort = 2;
constexpr quint8 kTagInt = 3;
constexpr quint8 kTagLong = 4;
constexpr quint8 kTagFloat = 5;
constexpr quint8 kTagDouble = 6;
constexpr quint8 kTagByteArray = 7;
constexpr quint8 kTagString = 8;
constexpr quint8 kTagList = 9;
constexpr quint8 kTagCompound = 10;
constexpr quint8 kTagIntArray = 11;
constexpr quint8 kTagLongArray = 12;

/**
 * @brief 一个只进不退的读游标，**越界一次就整体作废**。
 *
 * 所有读取都从这里走，图的是「长度离谱」只有一处判据：每一个读取动作都先拿剩余
 * 字节数卡一道，卡不住就把游标标死、返回一个零值，**不分配、不抛、不转圈**。
 * 调用方在每一层收尾处问一次 ok() 就够了。
 */
class Cursor {
public:
    explicit Cursor(const QByteArray &data) : m_data(data) {}

    /// 还没出过错。
    bool ok() const { return m_problem.isEmpty(); }
    /// 第一处出错的原因（给用户看的一句话）。没出错时是空串。
    QString problem() const { return m_problem; }
    /// 还剩下多少字节可读。**游标已经死了就是 0**（后续每一问都直接失败）。
    qint64 remaining() const { return ok() ? m_data.size() - m_pos : 0; }

    quint8 readU8() {
        if (!need(1)) {
            return 0;
        }
        return static_cast<quint8>(m_data.at(static_cast<int>(m_pos++)));
    }

    qint16 readI16() { return readBigEndian<qint16>(); }
    qint32 readI32() { return readBigEndian<qint32>(); }

    /// 读一段定长字节（**会真的拷出来** —— 只有字符串正文要它）。
    QByteArray readRaw(qint64 count) {
        if (!need(count)) {
            return QByteArray();
        }
        const QByteArray raw = m_data.mid(static_cast<int>(m_pos), static_cast<int>(count));
        m_pos += count;
        return raw;
    }

    /// 跳过一段定长字节（**一个字都不拷**）。标签正文里绝大多数都要跳过 —— 一份
    /// 8 MB 的 TAG_ByteArray 用 readRaw() 跳就是白拷 8 MB。
    bool skipRaw(qint64 count) {
        if (!need(count)) {
            return false;
        }
        m_pos += count;
        return true;
    }

    /// 把游标标死。**只有第一处原因算数**（后面那些都是它的连锁反应，报出来更绕）。
    bool fail(const QString &why) {
        if (m_problem.isEmpty()) {
            m_problem = why;
        }
        return false;
    }

private:
    bool need(qint64 count) {
        if (count < 0) {
            return fail(QCoreApplication::translate("NbtUtil", "文件中的长度值为负数"));
        }
        if (count > remaining()) {
            return fail(QCoreApplication::translate("NbtUtil", "文件在第 %1 个字节处中断").arg(m_pos));
        }
        return true;
    }

    template <typename T>
    T readBigEndian() {
        const QByteArray raw = readRaw(static_cast<qint64>(sizeof(T)));
        if (raw.size() != static_cast<int>(sizeof(T))) {
            return T();
        }
        T value = 0;
        std::memcpy(&value, raw.constData(), sizeof(T));
        return qFromBigEndian(value);
    }

    QByteArray m_data;
    qint64 m_pos = 0;
    QString m_problem;
};

/**
 * @brief 解 NBT 那套「修改过的 UTF-8」。
 * @param bytes 已经是正文那一截（长度字段不含在内）。
 * @return 解出来的字符串；坏字节换成 U+FFFD，**不让整个文件因此失败**。
 *
 * 和普通 UTF-8 的差别只有一处：U+0000 写成 `0xC0 0x80` 两字节（普通 UTF-8 里
 * 0x00 就是一个 0x00，而 NBT 的字符串里不允许出现裸的 0x00）。其余按 UTF-8 解，
 * 差别是**补充平面走 CESU-8**（一个码点拆成两个三字节的代理对）—— 这一点不用特意
 * 处理：解出来的两个 QChar 本来就该是「高代理 + 低代理」相邻躺在一个 QString 里，
 * 于是和 UTF-16 的表示正好一样。
 */
QString decodeModifiedUtf8(const QByteArray &bytes) {
    QString out;
    out.reserve(bytes.size());
    const int size = bytes.size();
    int i = 0;
    while (i < size) {
        const uchar b0 = static_cast<uchar>(bytes.at(i));
        if (b0 == 0xC0 && i + 1 < size && static_cast<uchar>(bytes.at(i + 1)) == 0x80) {
            out.append(QChar(u'\0'));
            i += 2;
            continue;
        }
        if (b0 < 0x80) {
            out.append(QChar(static_cast<char16_t>(b0)));
            i += 1;
            continue;
        }

        int extra = 0;
        uint code = 0;
        if ((b0 & 0xE0) == 0xC0) {
            extra = 1;
            code = b0 & 0x1F;
        } else if ((b0 & 0xF0) == 0xE0) {
            extra = 2;
            code = b0 & 0x0F;
        } else if ((b0 & 0xF8) == 0xF0) {
            extra = 3;
            code = b0 & 0x07;
        } else {
            out.append(QChar(QChar::ReplacementCharacter));
            i += 1;
            continue;
        }

        if (i + extra >= size) {
            out.append(QChar(QChar::ReplacementCharacter));
            break;
        }
        bool broken = false;
        for (int k = 1; k <= extra; ++k) {
            const uchar bk = static_cast<uchar>(bytes.at(i + k));
            if ((bk & 0xC0) != 0x80) {
                broken = true;
                break;
            }
            code = (code << 6) | (bk & 0x3F);
        }
        // 过长的写法（能用两字节写却写成三字节）不接受，判成坏字节：那是构造函数
        // 拿来绕判据的常见形状，认下来只会让同一段文字有两个字节表示。
        const uint minimum = (extra == 1) ? 0x80u : (extra == 2 ? 0x800u : 0x10000u);
        if (broken || code < minimum || code > 0x10FFFFu) {
            out.append(QChar(QChar::ReplacementCharacter));
            i += 1;
            continue;
        }
        i += extra + 1;
        if (code <= 0xFFFFu) {
            out.append(QChar(static_cast<char16_t>(code)));
        } else {
            const uint rest = code - 0x10000u;
            out.append(QChar(static_cast<char16_t>(0xD800u + (rest >> 10))));
            out.append(QChar(static_cast<char16_t>(0xDC00u + (rest & 0x3FFu))));
        }
    }
    return out;
}

/// 读一个 TAG_String 的正文（`quint16 长度 + 修改过的 UTF-8`）。
QString readModifiedString(Cursor &cursor) {
    const quint16 length = static_cast<quint16>(cursor.readI16());
    const QByteArray raw = cursor.readRaw(length);
    if (!cursor.ok()) {
        return QString();
    }
    return decodeModifiedUtf8(raw);
}

bool skipPayload(Cursor &cursor, quint8 type, int depth);

/**
 * @brief 跳过一个 TAG_List 的元素区（类型与个数已经读掉了）。
 *
 * NOTE: 这里那道 `count > remaining()` 是**必须的**：元素至少占 1 个字节，所以
 * count 大于剩余字节数时这份文件必然是坏的；不先卡这一下，一份写着
 * `count = 0x7FFFFFFF` 的文件会被老老实实循环二十多亿次（每次都在同一个
 * 位置失败，但它就是不停），用户看到的是启动器卡住而不是一句报错。
 */
bool skipListElements(Cursor &cursor, quint8 elementType, qint32 count, int depth) {
    if (depth >= kMaxDepth) {
        return cursor.fail(QCoreApplication::translate("NbtUtil", "NBT 嵌套层级超过 %1").arg(kMaxDepth));
    }
    if (count > cursor.remaining()) {
        return cursor.fail(QCoreApplication::translate("NbtUtil", "列表声明包含 %1 个元素，实际数据不足").arg(count));
    }
    for (qint32 i = 0; i < count; ++i) {
        if (!skipPayload(cursor, elementType, depth + 1)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 跳过一个标签的正文。
 * @param type 标签类型号。
 * @param depth 当前的嵌套深度（根那一层是 0）。
 *
 * 这里是「不糊弄」的那一半：每一种类型都按它**自己**的长度规则读过去，而不是
 * 猜「下一个字节大概是名字」。跳错一个字节，后面读到的 `servers` 就是碰运气。
 */
bool skipPayload(Cursor &cursor, quint8 type, int depth) {
    switch (type) {
    case kTagByte:
        return cursor.skipRaw(1);
    case kTagShort:
        return cursor.skipRaw(2);
    case kTagInt:
        return cursor.skipRaw(4);
    case kTagLong:
        return cursor.skipRaw(8);
    case kTagFloat:
        return cursor.skipRaw(4);
    case kTagDouble:
        return cursor.skipRaw(8);
    case kTagByteArray: {
        const qint32 length = cursor.readI32();
        return cursor.skipRaw(length);
    }
    case kTagString: {
        const quint16 length = static_cast<quint16>(cursor.readI16());
        return cursor.skipRaw(length);
    }
    case kTagList: {
        const quint8 elementType = cursor.readU8();
        const qint32 count = cursor.readI32();
        if (!cursor.ok()) {
            return false;
        }
        // 元素类型是 0（TAG_End）或者个数是 0 时后面**没有**元素数据。这两条
        // 不判的话，一个空 list 会被当成「有 count 个元素」继续往下读。
        if (count <= 0 || elementType == kTagEnd) {
            return true;
        }
        return skipListElements(cursor, elementType, count, depth);
    }
    case kTagCompound: {
        if (depth >= kMaxDepth) {
            return cursor.fail(QCoreApplication::translate("NbtUtil", "NBT 嵌套层级超过 %1").arg(kMaxDepth));
        }
        for (;;) {
            const quint8 inner = cursor.readU8();
            if (!cursor.ok()) {
                return false;
            }
            if (inner == kTagEnd) {
                return true;
            }
            readModifiedString(cursor);  // 标签名，跳过正文用不着它
            if (!cursor.ok()) {
                return false;
            }
            if (!skipPayload(cursor, inner, depth + 1)) {
                return false;
            }
        }
    }
    case kTagIntArray: {
        const qint32 count = cursor.readI32();
        return cursor.skipRaw(static_cast<qint64>(count) * 4);
    }
    case kTagLongArray: {
        const qint32 count = cursor.readI32();
        return cursor.skipRaw(static_cast<qint64>(count) * 8);
    }
    default:
        return cursor.fail(QCoreApplication::translate("NbtUtil", "未知的标签类型 %1").arg(type));
    }
}

/// 读一条服务器（一个 TAG_Compound）里的 `name` / `ip`。
///
/// NOTE: 只认 TAG_String 的那两格。同名但类型不对的（有人把 name 写成 TAG_Int）
/// 当**没有这一格**处理：值取不出来，跟着类型硬解只会得到一条乱码地址。
bool readServerEntry(Cursor &cursor, ServerEntry *entry, QString *problem) {
    for (;;) {
        const quint8 type = cursor.readU8();
        if (!cursor.ok()) {
            *problem = cursor.problem();
            return false;
        }
        if (type == kTagEnd) {
            return true;
        }
        const QString tagName = readModifiedString(cursor);
        if (!cursor.ok()) {
            *problem = cursor.problem();
            return false;
        }
        const bool wanted = type == kTagString &&
                            (tagName == QLatin1String("name") || tagName == QLatin1String("ip"));
        if (wanted) {
            const QString value = readModifiedString(cursor);
            if (!cursor.ok()) {
                *problem = cursor.problem();
                return false;
            }
            if (tagName == QLatin1String("name")) {
                entry->name = value;
            } else {
                // `ip` 就是地址那一格的名字（**不是** address）—— 这一条整个文件的
                // 意义都在这里，写错了读出来的是个空地址。
                entry->address = value;
            }
            continue;
        }
        if (!skipPayload(cursor, type, 1)) {
            *problem = cursor.problem();
            return false;
        }
    }
}

/// 读 `servers` 那一格的正文（元素类型与个数还没读）。
bool readServerListTag(Cursor &cursor, QList<ServerEntry> *out, QString *problem) {
    const quint8 elementType = cursor.readU8();
    const qint32 count = cursor.readI32();
    if (!cursor.ok()) {
        *problem = cursor.problem();
        return false;
    }
    // 空列表：以后面没有元素数据。**不是**错误（见 .hpp 里那条 NOTE）。
    if (count <= 0 || elementType == kTagEnd) {
        return true;
    }
    // 元素类型不对时**不接着往下猜**。这一档在真机上不存在（游戏不会写出
    // 这种文件），所以它只可能是「这不是我认识的那个格式」—— 报 Unreadable
    // 比硬跳过、然后交出一个空列表更接近事实（空列表会显示成「一个服务器都没有」，
    // 而这句是假话）。
    if (elementType != kTagCompound) {
        *problem = QCoreApplication::translate("NbtUtil", "servers 列表的元素类型为 %1，应为 TAG_Compound")
                       .arg(elementType);
        return false;
    }
    if (count > cursor.remaining()) {
        *problem = QCoreApplication::translate("NbtUtil", "servers 声明包含 %1 条记录，实际数据不足").arg(count);
        return false;
    }
    for (qint32 i = 0; i < count; ++i) {
        ServerEntry entry;
        if (!readServerEntry(cursor, &entry, problem)) {
            return false;
        }
        // 没地址的那条不返回，理由见 .hpp。
        if (out != nullptr && !entry.address.isEmpty()) {
            out->append(entry);
        }
    }
    return true;
}

/// 从根复合体里找 `servers` 那一格。
bool readServers(const QByteArray &bytes, QList<ServerEntry> *out, QString *problem) {
    Cursor cursor(bytes);
    const quint8 rootType = cursor.readU8();
    if (!cursor.ok()) {
        *problem = QCoreApplication::translate("NbtUtil", "文件为空，不含任何标签");
        return false;
    }
    if (rootType != kTagCompound) {
        *problem = QCoreApplication::translate("NbtUtil", "根标签应为 TAG_Compound，实际类型为 %1")
                       .arg(rootType);
        return false;
    }
    readModifiedString(cursor);  // 根标签名，实机上是个空串
    if (!cursor.ok()) {
        *problem = cursor.problem();
        return false;
    }

    for (;;) {
        const quint8 type = cursor.readU8();
        if (!cursor.ok()) {
            *problem = cursor.problem();
            return false;
        }
        if (type == kTagEnd) {
            // 根到头了，说明里面根本没有 servers 那一格。**按空列表处理**：有些
            // 工具写出来的就是这个形状，报「文件坏了」是句不实在的话。
            return true;
        }
        const QString tagName = readModifiedString(cursor);
        if (!cursor.ok()) {
            *problem = cursor.problem();
            return false;
        }
        if (tagName == QLatin1String("servers")) {
            if (type != kTagList) {
                *problem = QCoreApplication::translate("NbtUtil", "servers 字段应为 TAG_List，实际类型为 %1。").arg(type);
                return false;
            }
            return readServerListTag(cursor, out, problem);
        }
        if (!skipPayload(cursor, type, 0)) {
            *problem = cursor.problem();
            return false;
        }
    }
}

/// 文件头两个字节是不是 gzip 的魔数。
bool looksGzipped(const QByteArray &bytes) {
    return bytes.size() >= 2 && static_cast<uchar>(bytes.at(0)) == 0x1F &&
           static_cast<uchar>(bytes.at(1)) == 0x8B;
}

/// 把整个文件读进来。**读之前**按 size() 卡一次上限，理由见 kMaxFileSize。
bool readWholeFile(const QString &path, QByteArray *out, QString *problem) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *problem = QCoreApplication::translate("NbtUtil", "无法打开 %1（%2）").arg(path, file.errorString());
        return false;
    }
    const qint64 size = file.size();
    if (size > kMaxFileSize) {
        *problem = QCoreApplication::translate("NbtUtil", "%1 为 %2 MB，超过 %3 MB 上限，未读取")
                       .arg(path, QString::number(size / (1024.0 * 1024.0), 'f', 1))
                       .arg(kMaxFileSize / (1024 * 1024));
        return false;
    }
    *out = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *problem = QCoreApplication::translate("NbtUtil", "读取 %1 时中途出错（%2）").arg(path, file.errorString());
        return false;
    }
    return true;
}

}  // namespace

Status readServerList(const QString &datPath, QList<ServerEntry> *out, QString *problemOut) {
    if (out != nullptr) {
        out->clear();
    }
    if (problemOut != nullptr) {
        problemOut->clear();
    }

    // 盘上有没有这个文件，和「读的时候哪里出了问题」是两回事，先分开。
    const QFileInfo info(datPath);
    if (!info.exists() || info.isDir()) {
        return Status::Missing;
    }
    // 上限在**读进来之前**判一次（readWholeFile 里还有一道，那一份管的是解压出来的
    // 临时文件）。这里这一道是为了让问题在还没有任何内存被占住的时候就显出来。
    if (info.size() > kMaxFileSize) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("NbtUtil", "servers.dat 为 %1 MB，超过 %2 MB 上限，未读取")
                              .arg(QString::number(info.size() / (1024.0 * 1024.0), 'f', 1))
                              .arg(kMaxFileSize / (1024 * 1024));
        }
        return Status::Unreadable;
    }

    QString problem;
    QByteArray bytes;
    if (!readWholeFile(datPath, &bytes, &problem)) {
        if (problemOut != nullptr) {
            *problemOut = problem;
        }
        return Status::Unreadable;
    }

    if (looksGzipped(bytes)) {
        // 中间文件建在系统临时目录里、靠 QTemporaryDir 的析构收走。**不许**落到实例
        // 目录里：那儿是用户看得见也备得了份的地方，一份解出来的副本躺在那儿既没
        // 意义又会让人以为那是游戏的东西。
        QTemporaryDir tempDir(QDir::tempPath() + QStringLiteral("/wtfml-nbt-XXXXXX"));
        if (!tempDir.isValid()) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate("NbtUtil", "无法创建临时目录（%1）").arg(tempDir.errorString());
            }
            return Status::Unreadable;
        }
        const QString target = tempDir.filePath(QStringLiteral("servers.dat"));
        QString error;
        if (!ZipUtil::gunzipTo(datPath, target, &error)) {
            if (problemOut != nullptr) {
                // 不重试：解不动就是解不动，重试只会让用户再等 15 秒拿到同一句话。
                // 拼成「读不动：…」而不是「解压失败：…」—— gunzipTo 给回来的那几句
                // 自己就带着「解压」两个字（解压超时 / 解压没跑起来），前面再冠一个
                // 「解压失败」就成了「解压失败：解压超时」这种自己跟自己绕的话。
                *problemOut = error.isEmpty()
                                  ? QCoreApplication::translate("NbtUtil", "无法解压 servers.dat 中的 gzip 数据")
                                  : QCoreApplication::translate("NbtUtil", "无法读取 servers.dat：%1").arg(error);
            }
            return Status::Unreadable;
        }
        // NOTE: 上面那道 16 MB 的门量的是**压缩后**的尺寸，一份解出来几 GB 的压缩
        // 炸弹照样过得来 —— 所以解完按同一根尺子再量一次。这一道自己量、自己
        // 说话：readWholeFile 那句里会带出临时文件的路径，而那个路径对用户
        // 没有任何意义（他递进来的是 servers.dat）。
        const QFileInfo decompressed(target);
        if (decompressed.size() > kMaxFileSize) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate("NbtUtil", "servers.dat 解压后为 %1 MB，超过 %2 MB 上限")
                                  .arg(QString::number(decompressed.size() / (1024.0 * 1024.0), 'f', 1))
                                  .arg(kMaxFileSize / (1024 * 1024));
            }
            return Status::Unreadable;
        }
        bytes.clear();
        if (!readWholeFile(target, &bytes, &problem)) {
            if (problemOut != nullptr) {
                *problemOut = problem;
            }
            return Status::Unreadable;
        }
        if (bytes.isEmpty()) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate("NbtUtil", "servers.dat 解压后无数据，请确认文件是否完整");
            }
            return Status::Unreadable;
        }
    }

    QList<ServerEntry> servers;
    if (!readServers(bytes, &servers, &problem)) {
        if (problemOut != nullptr) {
            *problemOut = problem;
        }
        return Status::Unreadable;
    }
    if (out != nullptr) {
        *out = servers;
    }
    return Status::Ok;
}

}  // namespace NbtUtil

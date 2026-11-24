/**
 * @file NbtTree.cpp
 * @brief NbtTree 的实现：一个**保序、全类型**的 NBT 建树读取器 + 写回器，外加游戏规则那两格。
 *
 * 【为什么读写分成"两套游标"而不是一套】
 *
 * 读的时候每一格都得**先看剩多少字节**再敢按长度前缀去取（一份被截断或者被人手改过的
 * `level.dat` 会给出任意大的长度），写的时候反过来是"往一个 QByteArray 后面接"。两件事
 * 的失败方式完全不同：读会失败，写不会。硬凑成一套 `Cursor` 只会让读那半边的每一条边界
 * 检查都变得可疑，而这一份的全部价值就在那些边界检查上。
 */

#include "util/NbtTree.hpp"

#include <QCoreApplication>

#include <cstring>
#include <limits>

namespace NbtTree {
namespace {

/// 一份 NBT 的正文上限。`level.dat` 通常是几十 KB；模组世界能到几 MB，给到 64 MB 是
/// "不因为一个真存档被拒"和"一份坏文件别把内存吃掉"之间的一个数（**拍的，不是量出来的**）。
constexpr qint64 kMaxSize = 64LL * 1024 * 1024;

/// 一份 NBT 里最多能装多少个元素（列表项 + 复合项，全加起来）。
///
/// 光有 `kMaxSize` 挡不住内存放大：List 的元素**最少一个字节**（空 Compound 就只占
/// 收尾那一个 `TAG_End`），所以 64 MiB 的坏文件可以写出 6000 万项，而一个 `Node` 是
/// 几十上百字节 —— 按项数直接 `reserve` 就是十几 GB 的一次分配，线程里抛
/// `std::bad_alloc` 或者被系统直接杀掉。
///
/// 这个数按"真数据用得着多少"拍：结构文件（`blocks` 是 List(Compound)，一格一个）里
/// 50 万块的巨型建筑也才几十万项；超了就是坏文件/攻击，不猜。
constexpr qint64 kMaxNodes = 1LL << 20;

/// 列表 `reserve` 的上限次数（超出就按摊销翻倍长）。**不是**接受上限，只是"别信那个
/// 项数一次要一整片"：真正的上限是 `kMaxNodes`，而 reserve 一个不可信的 count 本来就
/// 是一次放大器。
constexpr qint64 kMaxReserve = 4096;

/// 一种元素**至少**占几个字节（拿"项数 × 最少字节"和剩余字节数对一下，不用逐个真读）。
qint64 minItemBytes(Type type) {
    switch (type) {
    case Type::End:
        // 空表（count=0）走到这儿；带项的 End 表在 List 那一段先被拒掉。
        // 给 1 而不是 0：下面拿它做除数（`count > remaining / 最少字节`）。
        return 1;
    case Type::Byte:
        return 1;
    case Type::Short:
        return 2;
    case Type::Int:
    case Type::Float:
        return 4;
    case Type::Long:
    case Type::Double:
        return 8;
    case Type::ByteArray:
    case Type::IntArray:
    case Type::LongArray:
        return 4;  // 长度前缀，正文可以是 0
    case Type::String:
        return 2;  // 长度前缀，正文可以是 0
    case Type::List:
        return 5;  // 元素类型 1 + 项数 4
    case Type::Compound:
        return 1;  // 只有收尾那一个字节
    }
    return 1;
}

/// 一个新造的 Node 的默认形状（每个 case 里只改自己那一格，别的保持默认）。
Node makeNode(Type type) {
    Node node;
    node.type = type;
    return node;
}

// ---------------------------------------------------------------------------
// 读：一个只往前走的游标（越界即死，之后一律失败）
// ---------------------------------------------------------------------------

/**
 * @brief 只往前走的读游标。**任何一次越界都让整个游标永久失效**。
 *
 * 这是这一份里最重要的一条：一次越界之后 `ok()` 变 false 且再也回不来，于是上面每一层
 * 都可以"读一读、最后统一看一次 ok()"，不用在每个函数里各写一遍错误分支 —— 而一旦哪个
 * 分支忘了看，最外层的 `parse()` 也会兜住（它最后要检查 `cursor.ok()` 和"正好读到底"）。
 */
class Reader {
public:
    explicit Reader(const QByteArray &raw) : m_raw(raw) {}

    bool ok() const { return m_ok; }
    const QString &problem() const { return m_problem; }
    qint64 remaining() const { return m_raw.size() - m_pos; }
    /// 还剩的字节数（`parse()` 用它判"末尾有没有多出一截"）。
    bool atEnd() const { return m_pos == m_raw.size(); }

    void fail(const QString &why) {
        if (m_ok) {
            m_ok = false;
            m_problem = why;
        }
    }

    /// 往后要 `count` 个字节（不取，只判够不够）。
    bool need(qint64 count) {
        if (count < 0) {
            fail(QStringLiteral("长度是负的"));
            return false;
        }
        if (remaining() < count) {
            fail(QStringLiteral("文件在这里就断了（还要 %1 个字节，只剩 %2 个）")
                     .arg(count)
                     .arg(remaining()));
            return false;
        }
        return true;
    }

    quint8 readU8() {
        if (!need(1)) {
            return 0;
        }
        return static_cast<quint8>(m_raw.at(m_pos++));
    }

    qint16 readI16() { return static_cast<qint16>(readU16()); }

    quint16 readU16() {
        if (!need(2)) {
            return 0;
        }
        const quint16 value = static_cast<quint16>(
            (static_cast<quint16>(static_cast<quint8>(m_raw.at(m_pos))) << 8) |
            static_cast<quint8>(m_raw.at(m_pos + 1)));
        m_pos += 2;
        return value;
    }

    qint32 readI32() {
        if (!need(4)) {
            return 0;
        }
        quint32 value = 0;
        for (int k = 0; k < 4; ++k) {
            value = (value << 8) | static_cast<quint8>(m_raw.at(m_pos + k));
        }
        m_pos += 4;
        return static_cast<qint32>(value);
    }

    qint64 readI64() {
        if (!need(8)) {
            return 0;
        }
        quint64 value = 0;
        for (int k = 0; k < 8; ++k) {
            value = (value << 8) | static_cast<quint8>(m_raw.at(m_pos + k));
        }
        m_pos += 8;
        return static_cast<qint64>(value);
    }

    float readF32() {
        const quint32 bits = static_cast<quint32>(readI32());
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    double readF64() {
        const quint64 bits = static_cast<quint64>(readI64());
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    QByteArray readRaw(qint64 count) {
        if (!need(count)) {
            return QByteArray();
        }
        const QByteArray out = m_raw.mid(static_cast<int>(m_pos), static_cast<int>(count));
        m_pos += count;
        return out;
    }

    /// 记一个元素，并卡 `kMaxNodes`（见那个常数：这是"别让项数变成一次十几 GB 的分配"
    /// 的最后一道闸 —— 列表那条"最少一个字节"的检查挡不住 List(Compound)）。
    bool claimNode() {
        if (++m_nodes > kMaxNodes) {
            fail(QStringLiteral("元素太多（超过 %1 个）").arg(kMaxNodes));
            return false;
        }
        return true;
    }

private:
    const QByteArray &m_raw;
    qint64 m_pos = 0;
    qint64 m_nodes = 0;
    bool m_ok = true;
    QString m_problem;
};

/**
 * @brief 解一个**修改过的 UTF-8** 串（就是 Java `DataOutputStream.writeUTF` 那一种）。
 *
 * 和普通 UTF-8 的差别只有一处：U+0000 写成 `0xC0 0x80` 两字节（NBT 的字符串里不允许
 * 出现裸的 0x00）。补充平面走 CESU-8（一个码点拆成两个三字节的代理对）—— 这一点不用特意
 * 处理，解出来的两个 QChar 本来就该是"高代理 + 低代理"相邻躺在一个 QString 里。
 *
 * NOTE: 和 `util/NbtUtil.cpp` 里那份**逐条同规矩**（那一份是匿名命名空间的，够不着，
 * 所以这里重写一遍）。两份都要改的话，得一起改 —— 好在"解出来和 Java 那边一样"
 * 这件事是死的，不会各自演化。
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
        // 过长的写法（能用两字节写却写成三字节）不接受：认下来只会让同一段文字有两个
        // 字节表示，而"我们没碰过的地方一个字节都不变"就靠这个唯一的表示。
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

/// 反过来：把一个 QString 写成**修改过的 UTF-8**（见上）。
///
/// 补一个 `QChar` 是三个字节（`0xE0|高四位` 那种）—— 代理对因此各写三个字节，也就是
/// CESU-8，正是 Java 那边 `readUTF` 期望的形状。**不能**走 `QString::toUtf8()`：那一个
/// 会把补充平面写成四字节，Java 的 `readUTF` 会把它当成两个独立的字符（乱码）。
QByteArray encodeModifiedUtf8(const QString &text) {
    QByteArray out;
    out.reserve(text.size() * 2);
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t u = text.at(i).unicode();
        if (u == 0) {
            out.append(static_cast<char>(0xC0));
            out.append(static_cast<char>(0x80));
        } else if (u < 0x80) {
            out.append(static_cast<char>(u));
        } else if (u < 0x800) {
            out.append(static_cast<char>(0xC0 | (u >> 6)));
            out.append(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.append(static_cast<char>(0xE0 | (u >> 12)));
            out.append(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.append(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

/// 类型编号 → `Type`（不认识的就是 fail）。
bool toType(quint8 raw, Type *out, QString *problemOut) {
    if (raw > static_cast<quint8>(Type::LongArray)) {
        if (problemOut != nullptr) {
            *problemOut = QStringLiteral("不认识的标签类型 %1").arg(raw);
        }
        return false;
    }
    *out = static_cast<Type>(raw);
    return true;
}

bool readString(Reader &reader, QString *out) {
    const quint16 length = reader.readU16();
    if (!reader.ok()) {
        return false;
    }
    const QByteArray raw = reader.readRaw(length);
    if (!reader.ok()) {
        return false;
    }
    *out = decodeModifiedUtf8(raw);
    return true;
}

/// 读一格**带名字的**标签（类型那个字节已经读掉了）—— 复合标签的每一项、以及文件开头那一格。
bool readNamedFromType(Reader &reader, quint8 rawType, Field *out, int depth);

/// 读一格**不带名字的**载荷（List 的元素就是它）。
bool readPayload(Reader &reader, Type type, Node *out, int depth) {
    if (depth > kMaxDepth) {
        reader.fail(QStringLiteral("嵌套太深（超过 %1 层）").arg(kMaxDepth));
        return false;
    }
    if (!reader.claimNode()) {
        return false;
    }
    Node node = makeNode(type);
    switch (type) {
    case Type::End:
        // 只有"空 List"和"复合标签的结束"会走到这儿 —— 单独一格 End 是错的。
        reader.fail(QStringLiteral("这里不该出现结束标签"));
        return false;
    case Type::Byte:
        node.number = static_cast<qint8>(reader.readU8());
        break;
    case Type::Short:
        node.number = reader.readI16();
        break;
    case Type::Int:
        node.number = reader.readI32();
        break;
    case Type::Long:
        node.number = reader.readI64();
        break;
    case Type::Float:
        node.real = static_cast<double>(reader.readF32());
        break;
    case Type::Double:
        node.real = reader.readF64();
        break;
    case Type::ByteArray: {
        const qint32 length = reader.readI32();
        // 先拿长度和剩余字节数比一下再分配：一份坏文件里那个长度可以是 20 亿。
        if (!reader.ok() || !reader.need(length)) {
            return false;
        }
        node.bytes = reader.readRaw(length);
        break;
    }
    case Type::String:
        if (!readString(reader, &node.text)) {
            return false;
        }
        break;
    case Type::List: {
        quint8 rawItemType = 0;
        rawItemType = reader.readU8();
        Type itemType = Type::End;
        if (!toType(rawItemType, &itemType, nullptr)) {
            reader.fail(QStringLiteral("列表里写着不认识的元素类型 %1").arg(rawItemType));
            return false;
        }
        const qint32 count = reader.readI32();
        if (!reader.ok()) {
            return false;
        }
        if (count < 0) {
            reader.fail(QStringLiteral("列表的项数是负的"));
            return false;
        }
        // 空表（`itemType == End`）只能有 0 项 —— 有项就表示"每个元素的类型都读不出来"，
        // 后面按 End 读第一格就报错；这里先一句话说清楚，也避开下面那个除以 0。
        if (itemType == Type::End && count != 0) {
            reader.fail(QStringLiteral("列表的元素类型是空的、却写着 %1 项").arg(count));
            return false;
        }
        // 每一项**最少**占 `minItemBytes` 个字节 ⇒ 项数比"剩余字节数 / 最少字节"还大
        // 必然是坏的。这一下既挡"按 count 一个个读成几百万次的循环"，也挡"按 count
        // 直接 reserve 一大片"（List(Byte) 的项数可以接近文件字节数）。
        if (count > reader.remaining() / minItemBytes(itemType)) {
            reader.fail(QStringLiteral("列表的项数 %1 比文件剩下的字节还多").arg(count));
            return false;
        }
        node.itemType = itemType;
        node.items.reserve(static_cast<int>(qMin<qint64>(count, kMaxReserve)));
        for (qint32 k = 0; k < count; ++k) {
            Node item;
            if (!readPayload(reader, itemType, &item, depth + 1)) {
                return false;
            }
            node.items.append(item);
        }
        break;
    }
    case Type::Compound:
        for (;;) {
            // 收尾那一格是 `TAG_End`，**不带名字**（它只占一个字节）—— 所以这里先自己读
            // 那个类型字节，读到 0 才是收尾，否则连着名字一起交给 readNamedFromType()。
            if (reader.remaining() < 1) {
                reader.fail(QStringLiteral("复合标签没有收尾"));
                return false;
            }
            const quint8 rawType = reader.readU8();
            if (rawType == static_cast<quint8>(Type::End)) {
                break;
            }
            Field item;
            if (!readNamedFromType(reader, rawType, &item, depth + 1)) {
                return false;
            }
            node.fields.append(item);
        }
        break;
    case Type::IntArray: {
        const qint32 count = reader.readI32();
        if (!reader.ok() || count < 0) {
            if (reader.ok()) {
                reader.fail(QStringLiteral("整数数组的长度是负的"));
            }
            return false;
        }
        if (!reader.need(static_cast<qint64>(count) * 4)) {
            return false;
        }
        node.ints.reserve(count);
        for (qint32 k = 0; k < count; ++k) {
            node.ints.append(reader.readI32());
        }
        break;
    }
    case Type::LongArray: {
        const qint32 count = reader.readI32();
        if (!reader.ok() || count < 0) {
            if (reader.ok()) {
                reader.fail(QStringLiteral("长整数数组的长度是负的"));
            }
            return false;
        }
        if (!reader.need(static_cast<qint64>(count) * 8)) {
            return false;
        }
        node.longs.reserve(count);
        for (qint32 k = 0; k < count; ++k) {
            node.longs.append(reader.readI64());
        }
        break;
    }
    }
    if (!reader.ok()) {
        return false;
    }
    *out = node;
    return true;
}

bool readNamedFromType(Reader &reader, quint8 rawType, Field *out, int depth) {
    if (depth > kMaxDepth) {
        reader.fail(QStringLiteral("嵌套太深（超过 %1 层）").arg(kMaxDepth));
        return false;
    }
    Type type = Type::End;
    if (!toType(rawType, &type, nullptr)) {
        reader.fail(QStringLiteral("不认识的标签类型 %1").arg(rawType));
        return false;
    }
    if (type == Type::End) {
        reader.fail(QStringLiteral("这里不该出现结束标签"));
        return false;
    }
    QString name;
    if (!readString(reader, &name)) {
        return false;
    }
    Node value;
    if (!readPayload(reader, type, &value, depth)) {
        return false;
    }
    out->first = name;
    out->second = value;
    return true;
}

/// 文件最开头那一格（自己读类型字节，然后交给上面那一个）。
bool readRootNamed(Reader &reader, Field *out) {
    const quint8 rawType = reader.readU8();
    if (!reader.ok()) {
        return false;
    }
    return readNamedFromType(reader, rawType, out, 1);
}

// ---------------------------------------------------------------------------
// 写：往一个 QByteArray 后面接（**不会失败**）
// ---------------------------------------------------------------------------

void writeU8(QByteArray *out, quint8 value) { out->append(static_cast<char>(value)); }

void writeU16(QByteArray *out, quint16 value) {
    out->append(static_cast<char>((value >> 8) & 0xFF));
    out->append(static_cast<char>(value & 0xFF));
}

void writeI32(QByteArray *out, qint32 value) {
    const quint32 bits = static_cast<quint32>(value);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out->append(static_cast<char>((bits >> shift) & 0xFF));
    }
}

void writeI64(QByteArray *out, qint64 value) {
    const quint64 bits = static_cast<quint64>(value);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out->append(static_cast<char>((bits >> shift) & 0xFF));
    }
}

/// 写一个字符串（长度前缀 + 修改过的 UTF-8）。**超长就截到 65535 字节**：调用方那一边
/// （`setValueText()`）已经把这种事挡掉了，这里只是不让一次非法写入把后面的字节冲歪。
void writeString(QByteArray *out, const QString &text) {
    QByteArray raw = encodeModifiedUtf8(text);
    if (raw.size() > 0xFFFF) {
        raw.truncate(0xFFFF);
    }
    writeU16(out, static_cast<quint16>(raw.size()));
    out->append(raw);
}

void writePayload(QByteArray *out, const Node &node);

void writeNamed(QByteArray *out, const QString &name, const Node &node) {
    writeU8(out, static_cast<quint8>(node.type));
    writeString(out, name);
    writePayload(out, node);
}

void writePayload(QByteArray *out, const Node &node) {
    switch (node.type) {
    case Type::End:
        return;  // 什么都不写（只有空 List 会走到这儿）
    case Type::Byte:
        writeU8(out, static_cast<quint8>(static_cast<qint8>(node.number)));
        return;
    case Type::Short:
        writeU16(out, static_cast<quint16>(static_cast<qint16>(node.number)));
        return;
    case Type::Int:
        writeI32(out, static_cast<qint32>(node.number));
        return;
    case Type::Long:
        writeI64(out, node.number);
        return;
    case Type::Float: {
        const float value = static_cast<float>(node.real);
        quint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        writeI32(out, static_cast<qint32>(bits));
        return;
    }
    case Type::Double: {
        quint64 bits = 0;
        std::memcpy(&bits, &node.real, sizeof(bits));
        writeI64(out, static_cast<qint64>(bits));
        return;
    }
    case Type::ByteArray:
        writeI32(out, static_cast<qint32>(node.bytes.size()));
        out->append(node.bytes);
        return;
    case Type::String:
        writeString(out, node.text);
        return;
    case Type::List:
        // **元素类型写 `itemType` 本身**（不是看第一个元素猜的）：空列表就靠它保命，
        // 而"盘上写着什么"和"里面装了什么"不一致时也照盘上那份写回去（我们没打算修它）。
        writeU8(out, static_cast<quint8>(node.itemType));
        writeI32(out, static_cast<qint32>(node.items.size()));
        for (const Node &item : node.items) {
            writePayload(out, item);
        }
        return;
    case Type::Compound:
        for (const Field &item : node.fields) {
            writeNamed(out, item.first, item.second);
        }
        writeU8(out, static_cast<quint8>(Type::End));
        return;
    case Type::IntArray:
        writeI32(out, static_cast<qint32>(node.ints.size()));
        for (const qint32 value : node.ints) {
            writeI32(out, value);
        }
        return;
    case Type::LongArray:
        writeI32(out, static_cast<qint32>(node.longs.size()));
        for (const qint64 value : node.longs) {
            writeI64(out, value);
        }
        return;
    }
}

/// 一格整数的合法范围（按类型的有符号位宽）。
void integerRange(Type type, qint64 *low, qint64 *high) {
    switch (type) {
    case Type::Byte:
        *low = -128;
        *high = 127;
        return;
    case Type::Short:
        *low = -32768;
        *high = 32767;
        return;
    case Type::Int:
        *low = -2147483648LL;
        *high = 2147483647LL;
        return;
    default:
        *low = std::numeric_limits<qint64>::min();
        *high = std::numeric_limits<qint64>::max();
        return;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 类型那一组小工具
// ---------------------------------------------------------------------------

QString typeName(Type type) {
    switch (type) {
    case Type::End:
        return QStringLiteral("结束");
    case Type::Byte:
        return QStringLiteral("字节");
    case Type::Short:
        return QStringLiteral("短整数");
    case Type::Int:
        return QStringLiteral("整数");
    case Type::Long:
        return QStringLiteral("长整数");
    case Type::Float:
        return QStringLiteral("单精度小数");
    case Type::Double:
        return QStringLiteral("双精度小数");
    case Type::ByteArray:
        return QStringLiteral("字节数组");
    case Type::String:
        return QStringLiteral("文字");
    case Type::List:
        return QStringLiteral("列表");
    case Type::Compound:
        return QStringLiteral("复合");
    case Type::IntArray:
        return QStringLiteral("整数数组");
    case Type::LongArray:
        return QStringLiteral("长整数数组");
    }
    return QStringLiteral("不认识");
}

bool isFloatingPoint(Type type) {
    return type == Type::Float || type == Type::Double;
}

int integerBits(Type type) {
    switch (type) {
    case Type::Byte:
        return 8;
    case Type::Short:
        return 16;
    case Type::Int:
        return 32;
    case Type::Long:
        return 64;
    default:
        return 0;
    }
}

QString valueText(const Node &node) {
    switch (node.type) {
    case Type::End:
        return QString();
    case Type::Byte:
    case Type::Short:
    case Type::Int:
    case Type::Long:
        return QString::number(node.number);
    case Type::Float:
        // 9 位有效数字是 float 的往返精度；双精度用 17 位。
        return QString::number(node.real, 'g', 9);
    case Type::Double:
        return QString::number(node.real, 'g', 17);
    case Type::String:
        return node.text;
    case Type::ByteArray:
        return QCoreApplication::translate("NbtTree", "%1 个字节").arg(node.bytes.size());
    case Type::List:
        return QCoreApplication::translate("NbtTree", "%1 项").arg(node.items.size());
    case Type::Compound:
        return QCoreApplication::translate("NbtTree", "%1 项").arg(node.fields.size());
    case Type::IntArray:
        return QCoreApplication::translate("NbtTree", "%1 个整数").arg(node.ints.size());
    case Type::LongArray:
        return QCoreApplication::translate("NbtTree", "%1 个长整数").arg(node.longs.size());
    }
    return QString();
}

bool setValueText(Node *node, const QString &text) {
    if (node == nullptr) {
        return false;
    }
    switch (node->type) {
    case Type::Byte:
    case Type::Short:
    case Type::Int:
    case Type::Long: {
        bool ok = false;
        const qint64 value = text.trimmed().toLongLong(&ok);
        if (!ok) {
            return false;
        }
        qint64 low = 0;
        qint64 high = 0;
        integerRange(node->type, &low, &high);
        if (value < low || value > high) {
            return false;
        }
        node->number = value;
        return true;
    }
    case Type::Float:
    case Type::Double: {
        bool ok = false;
        const double value = text.trimmed().toDouble(&ok);
        if (!ok) {
            return false;
        }
        node->real = value;
        return true;
    }
    case Type::String: {
        // 长度上限是**编码之后**的字节数（NBT 的长度前缀是 quint16）。超了就拒绝，
        // 而不是写一个会被截断的值回去 —— "写进去的和用户看见的不一样"是最坏的一种。
        if (encodeModifiedUtf8(text).size() > 0xFFFF) {
            return false;
        }
        node->text = text;
        return true;
    }
    default:
        // 数组 / 列表 / 复合不接"一行字"：它们是整棵子树，得按子树改（这一版没有这种需求）。
        return false;
    }
}

// ---------------------------------------------------------------------------
// 走路
// ---------------------------------------------------------------------------

const Node *field(const Node &compound, const QString &name) {
    if (compound.type != Type::Compound) {
        return nullptr;
    }
    for (const Field &item : compound.fields) {
        if (item.first == name) {
            return &item.second;
        }
    }
    return nullptr;
}

Node *field(Node *compound, const QString &name) {
    if (compound == nullptr || compound->type != Type::Compound) {
        return nullptr;
    }
    for (Field &item : compound->fields) {
        if (item.first == name) {
            return &item.second;
        }
    }
    return nullptr;
}

namespace {

/// 按路径切段。路径里的空段（开头/结尾/连着两个斜杠）忽略 —— `Data/GameRules` 和
/// `Data//GameRules` 是同一个意思，而调用方写错一个斜杠不该变成「这个世界没有那一格」。
QStringList pathParts(const QString &path) {
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

}  // namespace

// NOTE: `const` 与不 `const` 这两份是**分开写的两个循环**，没有合成一个模板：
// `field()` 那对重载在模板里过不了（模板里 `current` 是 `T *`，两个重载得靠
// `const` 限定去分，而 GCC 在这条路上只挑得中不带 `const` 的那个）。八行代码，
// 不值得为它绕一圈。
const Node *descend(const Node &root, const QString &path) {
    const Node *current = &root;
    for (const QString &part : pathParts(path)) {
        if (current->type != Type::Compound) {
            return nullptr;
        }
        current = field(*current, part);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

Node *descend(Node *root, const QString &path) {
    Node *current = root;
    for (const QString &part : pathParts(path)) {
        if (current == nullptr || current->type != Type::Compound) {
            return nullptr;
        }
        current = field(current, part);
    }
    return current;
}

// ---------------------------------------------------------------------------
// 解 / 写
// ---------------------------------------------------------------------------

bool parse(const QByteArray &raw, Document *out, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (out == nullptr) {
        return false;
    }
    if (raw.size() > kMaxSize) {
        if (problemOut != nullptr) {
            *problemOut = QStringLiteral("这份文件太大了（%1 字节）").arg(raw.size());
        }
        return false;
    }

    Reader reader(raw);
    Document document;
    // 开头那一格：类型 + 名字 + 载荷。`level.dat` 的根是一格**无名**的 Compound。
    Field rootField;
    if (!readRootNamed(reader, &rootField)) {
        if (problemOut != nullptr) {
            *problemOut = reader.problem();
        }
        return false;
    }
    // 末尾多出来一截也算坏（"读完了"和"正好读到底"是两件事：多出来的那一段可能正是
    // 被人塞进去的东西，认了它就是认了一份我们不认识的文件）。
    if (!reader.atEnd()) {
        if (problemOut != nullptr) {
            *problemOut = QStringLiteral("末尾还多着 %1 个字节").arg(reader.remaining());
        }
        return false;
    }
    document.rootName = rootField.first;
    document.root = rootField.second;
    *out = document;
    return true;
}

QByteArray serialize(const Document &document) {
    QByteArray out;
    out.reserve(4096);
    writeNamed(&out, document.rootName, document.root);
    return out;
}

// ---------------------------------------------------------------------------
// 游戏规则
// ---------------------------------------------------------------------------

ValueKind ruleKind(const QString &value) {
    const QString trimmed = value.trimmed();
    if (trimmed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
        trimmed.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) {
        return ValueKind::Bool;
    }
    bool ok = false;
    trimmed.toLongLong(&ok);
    if (ok) {
        return ValueKind::Int;
    }
    // `2.5` 这种也归"别的"：游戏规则没有小数（真按小数读进去，游戏那边会当成坏值）。
    return ValueKind::Text;
}

QString valueKindName(ValueKind kind) {
    switch (kind) {
    case ValueKind::Bool:
        return QCoreApplication::translate("NbtTree", "开关");
    case ValueKind::Int:
        return QCoreApplication::translate("NbtTree", "整数");
    case ValueKind::Text:
        return QCoreApplication::translate("NbtTree", "文字");
    }
    return QString();
}

bool boolValue(const QString &value, bool *out) {
    const QString trimmed = value.trimmed();
    if (trimmed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) {
        if (out != nullptr) {
            *out = true;
        }
        return true;
    }
    if (trimmed.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) {
        if (out != nullptr) {
            *out = false;
        }
        return true;
    }
    return false;
}

bool gameRulesOf(const Node &root, QList<GameRule> *out, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (out == nullptr) {
        return false;
    }
    out->clear();
    const Node *rules = descend(root, QStringLiteral("Data/GameRules"));
    if (rules == nullptr) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate(
                "NbtTree", "level.dat 缺少游戏规则数据。存档可能尚未初始化或来自旧版本。");
        }
        return false;
    }
    if (rules->type != Type::Compound) {
        if (problemOut != nullptr) {
            *problemOut = QCoreApplication::translate("NbtTree", "游戏规则数据类型无效（%1），无法读取。")
                              .arg(typeName(rules->type));
        }
        return false;
    }
    for (const Field &item : rules->fields) {
        // 值必须是字符串：不是的那一条**跳过**而不是拒绝整份文件 —— 一张表里有一条
        // 形状不对（模组写的），不该让别的几十条都改不了。跳过的那一条不报数：
        // 界面上少一条、用户看不出来，而"少的那条"在游戏里照样是那个值（我们没碰它）。
        if (item.second.type != Type::String) {
            continue;
        }
        GameRule rule;
        rule.name = item.first;
        rule.value = item.second.text;
        out->append(rule);
    }
    return true;
}

bool setGameRule(Node *root, const QString &name, const QString &value, QString *problemOut,
                 MissingRule whenMissing) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    const auto missing = [problemOut](const QString &why) {
        if (problemOut != nullptr) {
            *problemOut = why;
        }
        return false;
    };
    Node *rules = descend(root, QStringLiteral("Data/GameRules"));
    if (rules == nullptr && whenMissing == MissingRule::Create) {
        // 这一档是给"这个世界还没被游戏写过档"那种情况用的（`Data/GameRules`
        // 那一格压根不在）。**只在 `Data` 本身是复合标签时建** —— 连 `Data` 都不是的话，
        // 这份 level.dat 的形状我们看不懂，宁可一个字都不写。
        Node *data = descend(root, QStringLiteral("Data"));
        if (data != nullptr && data->type == Type::Compound) {
            Field fresh;
            fresh.first = QStringLiteral("GameRules");
            fresh.second.type = Type::Compound;
            data->fields.append(fresh);
            // `append()` 之后必须**重新取一次**：`data->fields` 是 QList，append 可能搬走
            // 那一整块内存，刚才那个指针就悬空了（这份文件里别处没有这么用的，所以这条
            // 规矩写在这儿）。
            rules = &data->fields.last().second;
        }
    }
    if (rules == nullptr || rules->type != Type::Compound) {
        return missing(QCoreApplication::translate(
            "NbtTree", "level.dat 缺少游戏规则数据，无法修改。"));
    }
    for (Field &item : rules->fields) {
        if (item.first != name) {
            continue;
        }
        if (item.second.type != Type::String) {
            return missing(QCoreApplication::translate("NbtTree", "规则「%1」的值不是文本，已跳过。")
                               .arg(name));
        }
        if (encodeModifiedUtf8(value).size() > 0xFFFF) {
            return missing(QCoreApplication::translate("NbtTree", "规则「%1」的值过长，无法保存。")
                               .arg(name));
        }
        item.second.text = value;
        return true;
    }
    if (whenMissing == MissingRule::Create) {
        // 表里没有这一条 ⇒ 往表尾接一条。名字太长的话 NBT 那一格写不下（长度前缀 2 字节），
        // 当场拒绝 —— 加一条**写不进去**的规则不该被当成成功。
        if (encodeModifiedUtf8(name).size() > 0xFFFF) {
            return missing(QCoreApplication::translate("NbtTree", "游戏规则名称过长，无法保存。"));
        }
        Field fresh;
        fresh.first = name;
        fresh.second.type = Type::String;
        fresh.second.text = value;
        rules->fields.append(fresh);
        return true;
    }
    // 默认那一档：不发明新规则（理由见 .hpp 里那一段）。
    return missing(QCoreApplication::translate(
        "NbtTree", "此存档中没有规则「%1」。可编辑已存在的规则。")
                       .arg(name));
}

}  // namespace NbtTree

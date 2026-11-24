/**
 * @file ServerPing.cpp
 * @brief ServerPing 的实现（协议说明在 .hpp 开头那一段）。
 */
#include "ServerPing.hpp"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDnsLookup>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <climits>
#include <memory>

namespace {

/// 一次 ping 的超时。见 timeoutMs()（唯一出处）。
int g_timeoutMs = 4000;

/// SRV 查询开着没有（见 srvLookupEnabled()）。**默认开** —— 游戏自己就是先查 SRV 再连。
bool g_srvLookupEnabled = true;

/// SRV 这一步最多花多少毫秒（再长也不超过整趟预算的一半，见 resolve()）。
///
/// 取 2 秒：一个正常 DNS 查询是几十毫秒，2 秒还没回来基本就是解析器坏了 —— 那不该把
/// 整趟预算（默认 4 秒）吃光，否则一台写域名、没有 SRV 的服务器会从"连了就答"变成
/// "等到超时"，看起来像服务器挂了。
constexpr int kSrvMaxBudgetMs = 2000;

/// 一个状态包最大认多大（字节）。**必须有上限**：长度前缀是服务器给的，一个坏掉的
/// （或者故意使坏的）服务器报一个 20 亿的长度，我们会一直等一堆永远不来的字节
/// —— 那正好是"超时"这条路上最难看的一种死法。真实的状态包（含图标）几 KB。
constexpr int kMaxPacketBytes = 1024 * 1024;

/// 协议里那个"我是什么版本"的数。**发 -1**（= 我不说）而不是我们自己猜一个：
/// 猜错的话，带版本检查的服务器（代理、跨版本插件）会照着我们报的那个数答一句
/// 「版本不兼容」，MOTD 就成了一行跟我们无关的话。发 -1 它照自己的版本报。
constexpr int kUnknownProtocolVersion = -1;

/// 一个字符串在协议里的写法：VarInt 长度 + UTF-8 正文。
QByteArray stringBytes(const QString &text) {
    const QByteArray utf8 = text.toUtf8();
    return ServerPing::varIntBytes(utf8.size()) + utf8;
}

/// 端口（协议里是**大端** uint16，不是 VarInt）。
QByteArray portBytes(int port) {
    QByteArray out;
    out.append(static_cast<char>((port >> 8) & 0xFF));
    out.append(static_cast<char>(port & 0xFF));
    return out;
}

/// 把"正文"套上一个 VarInt 长度前缀，就是一个包。
QByteArray packet(const QByteArray &body) {
    return ServerPing::varIntBytes(body.size()) + body;
}

/// `description` 那一格展平成**没洗过**的一行（见 motdText()）。
///
/// 这块 JSON 的形态（都不是我们编的，wiki.vg 上那几种真机上都见过）：
/// - `"description": "一句纯文本"`
/// - `"description": {"text": "前半", "extra": [{"text":"后半","color":"red"}, "再来一段"]}`
/// - `extra` 里还能再嵌 `extra`（递归下去）；`color` / `bold` 那些**不要**（我们不画富文本）
/// - 1.20.3 之后 `description` 也可能是**数组**（一段一段的），所以数组那一档也认
QString flattenMotd(const QJsonValue &value) {
    if (value.isString()) {
        return value.toString();
    }
    if (value.isArray()) {
        QString all;
        const QJsonArray parts = value.toArray();
        for (const QJsonValue &one : parts) {
            all += flattenMotd(one);
        }
        return all;
    }
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        QString all = obj.value(QStringLiteral("text")).toString();
        const QJsonValue extra = obj.value(QStringLiteral("extra"));
        if (extra.isArray()) {
            const QJsonArray parts = extra.toArray();
            for (const QJsonValue &one : parts) {
                all += flattenMotd(one);
            }
        }
        return all;
    }
    return QString();
}

/// 「富文本那一版」展平：**把颜色和字形翻译成 `§` 码**发出来（见 motdRaw()）。
///
/// 和上面那个 flattenMotd() 的差别只有一件事：那个只取字，这个还要把
/// `"color": "red"` / `"bold": true` 那些**翻回 `§c` / `§l`**。为什么绕这一圈：JSON 那一
/// 层是"结构化"的、`§` 是"一串字符"的，而**画的那一层只认后者**（`ServerPing` 这一层不
/// 碰 QtGui，见文件头那条"不开图形依赖"—— 所以颜色不能在这一层变成 QColor）。
///
/// 【继承怎么算】
///
/// 组件格式里子元素**继承**父元素没写的那几项（`{"color":"red","extra":[{"text":"x"}]}`
/// 里那个 x 也是红的），而 `§` 码是"从这里往后都算"的**增量**记号 —— 两边对不上，所以
/// 这里带着一份 `state` 往下走：进一个元素时按它自己写了哪几项改 state，退出时还原
/// （兄弟之间不继承，只有父子继承）。
///
/// NOTE: `"bold": false` 这种**明写关掉**才是关掉（不写就是继承）。要关掉一个字形只能
/// 发 `§r`，而 `§r` 连颜色一起清 —— 所以关任何一位之前先把颜色复位再补回来，
/// 不然症状是"那一小段的颜色莫名变成白的"。
struct MotdState {
    QString colorCode;  ///< Minecraft 颜色码（`c` / `x` 开头的一串）；空 = 没指定
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
};

/// 具名颜色 → `§` 后面那一串。表里没有的（包括空串）回空。
QString colorCodeForName(const QString &name) {
    static const QHash<QString, QString> table = {
        {QStringLiteral("black"), QStringLiteral("0")},
        {QStringLiteral("dark_blue"), QStringLiteral("1")},
        {QStringLiteral("dark_green"), QStringLiteral("2")},
        {QStringLiteral("dark_aqua"), QStringLiteral("3")},
        {QStringLiteral("dark_red"), QStringLiteral("4")},
        {QStringLiteral("dark_purple"), QStringLiteral("5")},
        {QStringLiteral("gold"), QStringLiteral("6")},
        {QStringLiteral("gray"), QStringLiteral("7")},
        {QStringLiteral("dark_gray"), QStringLiteral("8")},
        {QStringLiteral("blue"), QStringLiteral("9")},
        {QStringLiteral("green"), QStringLiteral("a")},
        {QStringLiteral("aqua"), QStringLiteral("b")},
        {QStringLiteral("red"), QStringLiteral("c")},
        {QStringLiteral("light_purple"), QStringLiteral("d")},
        {QStringLiteral("yellow"), QStringLiteral("e")},
        {QStringLiteral("white"), QStringLiteral("f")},
    };
    return table.value(name);
}

/// `#RRGGBB` → `x§R§R§G§G§B§B`（1.16 之后那种写法，**每个分量前面都要一个 `§`**）。
QString hexColorCode(const QString &text) {
    if (text.size() != 7 || !text.startsWith(QLatin1Char('#'))) {
        return QString();
    }
    QString digits = text.mid(1);
    bool ok = false;
    digits.toUInt(&ok, 16);
    if (!ok) {
        return QString();
    }
    QString code = QStringLiteral("x");
    for (const QChar digit : digits) {
        code += QChar(0x00A7);
        code += digit;
    }
    return code;
}

/// 从 `state` 走到 @p to，把中间要发的 `§` 码拼出来，并把 `state` 推到 @p to。
QString codesTo(const MotdState &to, bool explicitReset, MotdState *state) {
    QString out;
    MotdState cur = *state;
    // ① 有哪一位要**关掉**（或者元素自己写了 `"color":"reset"`）：只能发 `§r`，它会把颜色
    // 和字形一起清掉。
    const bool anyOff = explicitReset || (cur.bold && !to.bold) || (cur.italic && !to.italic) ||
                        (cur.underline && !to.underline) || (cur.strike && !to.strike) ||
                        (to.colorCode.isEmpty() && !cur.colorCode.isEmpty());
    if (anyOff) {
        out += QChar(0x00A7);
        out += QLatin1Char('r');
        cur = MotdState();
    }
    // ② 颜色（`§r` 之后要重新补上，不然"关粗体"会顺手把颜色变成白的）。
    if (!to.colorCode.isEmpty() && to.colorCode != cur.colorCode) {
        out += QChar(0x00A7);
        out += to.colorCode;
        cur.colorCode = to.colorCode;
    }
    // ③ 字形（只加不减 —— 减那一半在 ① 里做过了）。
    const auto turnOn = [&out](QLatin1Char code, bool want, bool *have) {
        if (want && !*have) {
            out += QChar(0x00A7);
            out += code;
            *have = true;
        }
    };
    turnOn(QLatin1Char('l'), to.bold, &cur.bold);
    turnOn(QLatin1Char('o'), to.italic, &cur.italic);
    turnOn(QLatin1Char('n'), to.underline, &cur.underline);
    turnOn(QLatin1Char('m'), to.strike, &cur.strike);
    *state = cur;
    return out;
}

QString flattenMotdRich(const QJsonValue &value, const MotdState &inherited, int depth) {
    // 深度那一道是防"服务器给我们一份自己嵌自己的 JSON"（递归下去就是个栈溢出）。
    // 组件格式真用起来不会超过几层；8 层之后不再往下走，字照收。
    if (depth > 8) {
        return value.isString() ? value.toString() : QString();
    }
    if (value.isString()) {
        // 纯字符串那一档：**里面本来就可能带着 `§` 码**（真机上大多数服务器就是这么写的），
        // 原样发出去，一个字都不动。
        return value.toString();
    }
    if (value.isArray()) {
        QString all;
        const QJsonArray parts = value.toArray();
        for (const QJsonValue &one : parts) {
            all += flattenMotdRich(one, inherited, depth + 1);
        }
        return all;
    }
    if (!value.isObject()) {
        return QString();
    }

    const QJsonObject obj = value.toObject();
    MotdState state = inherited;
    bool explicitReset = false;
    if (obj.contains(QStringLiteral("color"))) {
        const QString name = obj.value(QStringLiteral("color")).toString();
        if (name == QLatin1String("reset")) {
            explicitReset = true;
            state.colorCode.clear();
        } else if (name.startsWith(QLatin1Char('#'))) {
            state.colorCode = hexColorCode(name);
        } else {
            state.colorCode = colorCodeForName(name);
        }
    }
    const auto styleField = [&obj](const char *key, bool *out) {
        const QJsonValue flag = obj.value(QString::fromLatin1(key));
        if (flag.isBool()) {
            *out = flag.toBool();
        }
    };
    styleField("bold", &state.bold);
    styleField("italic", &state.italic);
    styleField("underlined", &state.underline);
    styleField("strikethrough", &state.strike);

    MotdState walk = inherited;
    QString out = codesTo(state, explicitReset, &walk);
    out += obj.value(QStringLiteral("text")).toString();
    const QJsonValue extra = obj.value(QStringLiteral("extra"));
    if (extra.isArray()) {
        const QJsonArray parts = extra.toArray();
        for (const QJsonValue &one : parts) {
            // 子元素继承的是**这个元素**的状态（不是上面传下来的那一份）。
            out += flattenMotdRich(one, state, depth + 1);
        }
    }
    return out;
}

}  // namespace

namespace ServerPing {

/// 把一条记录收成"能用"的样子：去掉两头空白和 FQDN 末尾那个点，其余原样。
///
/// 目标是 `.`（根）就是 RFC 2782 那句"这个服务**不可用**"—— 剥掉点之后变空，被过滤掉。
/// 那正是我们想要的：宁可不换 SRV，也不要连到一个"就叫点"的主机上去。
static bool usableSrvRecord(const SrvRecord &record, SrvRecord *out) {
    SrvRecord fixed = record;
    fixed.target = fixed.target.trimmed();
    while (fixed.target.endsWith(QLatin1Char('.'))) {
        fixed.target.chop(1);
    }
    if (fixed.target.isEmpty() || fixed.port <= 0 || fixed.port > 65535) {
        return false;
    }
    if (out != nullptr) {
        *out = fixed;
    }
    return true;
}

QList<SrvRecord> bestPriorityRecords(const QList<SrvRecord> &records) {
    QList<SrvRecord> valid;
    valid.reserve(records.size());
    int bestPriority = INT_MAX;
    for (const SrvRecord &record : records) {
        SrvRecord fixed;
        if (!usableSrvRecord(record, &fixed)) {
            continue;
        }
        valid.append(fixed);
        bestPriority = qMin(bestPriority, fixed.priority);
    }
    if (valid.isEmpty()) {
        return {};
    }
    QList<SrvRecord> best;
    best.reserve(valid.size());
    for (const SrvRecord &record : valid) {
        if (record.priority == bestPriority) {
            best.append(record);
        }
    }
    return best;
}

bool pickSrvRecord(const QList<SrvRecord> &records, quint32 sample, SrvRecord *out) {
    const QList<SrvRecord> best = bestPriorityRecords(records);
    if (best.isEmpty() || out == nullptr) {
        return false;
    }
    quint32 totalWeight = 0;
    for (const SrvRecord &record : best) {
        totalWeight += static_cast<quint32>(qMax(0, record.weight));
    }
    if (totalWeight == 0) {
        // 全是 0 权重 = 等概率（RFC 2782 明写的）。"权重为 0 就不选"会让这种表一条都
        // 挑不出来 —— 而它是合法的、真有人这么配。
        *out = best.at(static_cast<int>(sample % static_cast<quint32>(best.size())));
        return true;
    }
    const quint32 point = sample % totalWeight;
    quint32 running = 0;
    for (const SrvRecord &record : best) {
        running += static_cast<quint32>(qMax(0, record.weight));
        if (point < running) {
            *out = record;
            return true;
        }
    }
    // 走不到这儿（point < totalWeight）；留一个兜底免得"理论上不可能"变成空返回。
    *out = best.constLast();
    return true;
}

bool srvLookupEnabled() { return g_srvLookupEnabled; }

void setSrvLookupEnabled(bool on) { g_srvLookupEnabled = on; }

namespace {

/// 这个主机是不是 IP 字面量（`1.2.3.4` / `::1`）。是的话**不查 SRV**：IP 没有域名可言，
/// 对它的 SRV 查询只会白等一趟 DNS。
bool isIpLiteral(const QString &host) {
    QHostAddress address;
    return address.setAddress(host);
}

/// 查 `_minecraft._tcp.<host>` 这一步。**阻塞**（内部开一个局部事件循环），最多 @p budgetMs。
struct SrvOutcome {
    bool found = false;
    QString target;
    int port = 0;
    QString error;
};

SrvOutcome lookupSrv(const QString &host, int budgetMs) {
    SrvOutcome outcome;
    QDnsLookup lookup(QDnsLookup::SRV, QStringLiteral("_minecraft._tcp.%1").arg(host));

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&lookup, &QDnsLookup::finished, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&loop, &timedOut] {
        timedOut = true;
        loop.quit();
    });
    deadline.start(qMax(1, budgetMs));
    lookup.lookup();
    if (!lookup.isFinished()) {
        loop.exec();
    }

    if (timedOut) {
        outcome.error = QCoreApplication::translate("ServerPing", "SRV 查询超时");
        return outcome;
    }
    if (lookup.error() != QDnsLookup::NoError) {
        if (lookup.error() == QDnsLookup::NotFoundError) {
            outcome.error = QCoreApplication::translate("ServerPing", "没有 SRV 记录");
        } else {
            outcome.error = QCoreApplication::translate("ServerPing", "SRV 查询失败");
        }
        return outcome;
    }

    QList<SrvRecord> candidates;
    const QList<QDnsServiceRecord> records = lookup.serviceRecords();
    candidates.reserve(records.size());
    for (const QDnsServiceRecord &record : records) {
        SrvRecord one;
        one.target = record.target();
        one.port = static_cast<int>(record.port());
        one.priority = static_cast<int>(record.priority());
        one.weight = static_cast<int>(record.weight());
        candidates.append(one);
    }
    SrvRecord picked;
    if (!pickSrvRecord(candidates, QRandomGenerator::global()->generate(), &picked)) {
        outcome.error = QCoreApplication::translate("ServerPing", "没有能用的 SRV 记录");
        return outcome;
    }
    outcome.found = true;
    outcome.target = picked.target;
    outcome.port = picked.port;
    return outcome;
}

}  // namespace

Resolution resolve(const QString &text, int budgetMs) {
    Resolution out;
    out.address = splitAddress(text);
    out.valid = out.address.valid;
    if (!out.address.valid) {
        return out;
    }
    out.connectHost = out.address.host;
    out.connectPort = out.address.port;

    if (!g_srvLookupEnabled) {
        return out;
    }
    // 写了端口 = 用户明确指定了这一对，不许 DNS 再换（游戏自己也是这个规矩）。
    if (out.address.portExplicit) {
        return out;
    }
    if (isIpLiteral(out.address.host)) {
        return out;
    }
    if (out.address.host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        return out;
    }

    const int budget = budgetMs > 0 ? budgetMs : timeoutMs();
    const int srvBudget = qMax(1, qMin(budget / 2, kSrvMaxBudgetMs));
    out.srvTried = true;
    const SrvOutcome srv = lookupSrv(out.address.host, srvBudget);
    if (!srv.found) {
        out.srvError = srv.error;
        return out;
    }
    out.connectHost = srv.target;
    out.connectPort = srv.port;
    out.srvUsed = true;
    return out;
}

QString kindName(Kind kind) {
    switch (kind) {
    case Kind::Ok:
        return QCoreApplication::translate("ServerPing", "通了");
    case Kind::InvalidAddress:
        return QCoreApplication::translate("ServerPing", "服务器地址格式无效");
    case Kind::HostNotFound:
        return QCoreApplication::translate("ServerPing", "找不到主机");
    case Kind::Refused:
        return QCoreApplication::translate("ServerPing", "连接被拒");
    case Kind::Unreachable:
        return QCoreApplication::translate("ServerPing", "网络不通");
    case Kind::NoAnswer:
        return QCoreApplication::translate("ServerPing", "服务器没有响应");
    case Kind::ConnectionLost:
        return QCoreApplication::translate("ServerPing", "连接断了");
    case Kind::Protocol:
        return QCoreApplication::translate("ServerPing", "服务器响应格式无效");
    case Kind::Other:
        return QCoreApplication::translate("ServerPing", "其余");
    }
    return QCoreApplication::translate("ServerPing", "其余");
}

Kind kindForSocketError(int socketError) {
    switch (static_cast<QAbstractSocket::SocketError>(socketError)) {
    case QAbstractSocket::ConnectionRefusedError:
        return Kind::Refused;
    case QAbstractSocket::HostNotFoundError:
        return Kind::HostNotFound;
    case QAbstractSocket::SocketTimeoutError:
    case QAbstractSocket::NetworkError:
        return Kind::Unreachable;
    case QAbstractSocket::RemoteHostClosedError:
        return Kind::ConnectionLost;
    default:
        return Kind::Other;
    }
}

int timeoutMs() { return g_timeoutMs; }

void setTimeoutMs(int ms) {
    // 下限 1 毫秒：0 会让每一次 waitFor* 立刻返回，"连不上"和"连上了但还没答"就分不开了
    // （验证工程要压到几十毫秒，那也够）。
    g_timeoutMs = qMax(1, ms);
}

Address splitAddress(const QString &text) {
    Address out;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return out;
    }

    QString hostPart = trimmed;
    QString portPart;
    if (trimmed.startsWith(QLatin1Char('['))) {
        // IPv6 字面量：`[::1]` 或者 `[::1]:25566`。方括号是**给地址里的冒号**用的，
        // 交给我们连的时候要去掉（Qt 收的是裸地址）。
        const int close = trimmed.indexOf(QLatin1Char(']'));
        if (close < 0) {
            return out;
        }
        hostPart = trimmed.mid(1, close - 1);
        const QString rest = trimmed.mid(close + 1);
        if (!rest.isEmpty()) {
            if (!rest.startsWith(QLatin1Char(':'))) {
                return out;
            }
            portPart = rest.mid(1);
        }
    } else {
        // 裸的 IPv6（没加方括号）**不猜**：`::1` 按"最后一个冒号"拆出来是主机 `::`、端口
        // `1` —— 一个看上去完全合法、其实谁也不是的地址。那正是这个函数最不该做的事
        // （把看不懂的地址悄悄改成别的去连，用户看到的会是"这台服务器连不上"）。冒号不止
        // 一个又没方括号，就是看不懂。
        if (trimmed.count(QLatin1Char(':')) > 1) {
            return out;
        }
        const int colon = trimmed.lastIndexOf(QLatin1Char(':'));
        if (colon >= 0) {
            hostPart = trimmed.left(colon);
            portPart = trimmed.mid(colon + 1);
        }
    }

    out.host = hostPart.trimmed();
    if (out.host.isEmpty()) {
        return out;
    }
    if (portPart.isEmpty()) {
        // 没写端口 —— 那是**默认值**，不是"看不懂"。
        out.port = kDefaultPort;
    } else {
        bool numeric = false;
        const int port = portPart.trimmed().toInt(&numeric);
        if (!numeric || port <= 0 || port > 65535) {
            return out;
        }
        out.port = port;
        out.portExplicit = true;
    }
    out.valid = true;
    return out;
}

bool readVarInt(const QByteArray &data, int *offset, int *out) {
    if (offset == nullptr || out == nullptr) {
        return false;
    }
    int result = 0;
    int shift = 0;
    int pos = *offset;
    while (true) {
        // 两种情况都只是"现在读不了"：手上这些字节还不够一个完整的 VarInt，
        // 或者连着 5 个字节都还带着"后面还有"那一位（VarInt 最多 5 字节）。
        if (pos >= data.size() || shift > 28) {
            return false;
        }
        const unsigned char byte = static_cast<unsigned char>(data.at(pos));
        ++pos;
        result |= (byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            break;
        }
    }
    *offset = pos;
    *out = result;
    return true;
}

QByteArray varIntBytes(int value) {
    QByteArray out;
    quint32 rest = static_cast<quint32>(value);
    while (true) {
        unsigned char byte = static_cast<unsigned char>(rest & 0x7F);
        rest >>= 7;
        if (rest != 0) {
            byte |= 0x80;
        }
        out.append(static_cast<char>(byte));
        if (rest == 0) {
            break;
        }
    }
    return out;
}

QString cleanMotd(const QString &raw) {
    QString out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const QChar ch = raw.at(i);
        // `§` 是两字符一对（`§a` / `§l` / `§r` …），**连着后面那个字符一起丢**。
        // 只丢 `§` 的话，屏幕上会留下一串孤零零的 `a` `l` `r`。
        if (ch == QChar(0x00A7)) {
            ++i;
            continue;
        }
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch == QLatin1Char('\t')) {
            out += QLatin1Char(' ');
            continue;
        }
        out += ch;
    }
    // 制表/多行压成空格之后可能连着好几个空格，收一下（顺带把两头的空白去掉）。
    return out.simplified();
}

QString motdText(const QJsonValue &description) {
    return cleanMotd(flattenMotd(description));
}

QString motdRaw(const QJsonValue &description) {
    return flattenMotdRich(description, MotdState(), 0);
}

QByteArray decodeFavicon(const QString &dataUrl) {
    // 只认这一种形态（wiki.vg 上写死的就是它）。别的形态**不猜**：宁可不画图标，
    // 也不要画一个"猜出来的"东西 —— 那看起来像是服务器给了个坏图标。
    static const QString prefix = QStringLiteral("data:image/png;base64,");
    if (!dataUrl.startsWith(prefix)) {
        return QByteArray();
    }
    return QByteArray::fromBase64(dataUrl.mid(prefix.size()).toLatin1());
}

QString connectionErrorText(int socketError) {
    switch (static_cast<QAbstractSocket::SocketError>(socketError)) {
    case QAbstractSocket::ConnectionRefusedError:
        return QCoreApplication::translate("ServerPing", "连接被拒绝");
    case QAbstractSocket::HostNotFoundError:
        return QCoreApplication::translate("ServerPing", "找不到主机");
    case QAbstractSocket::SocketTimeoutError:
    case QAbstractSocket::NetworkError:
        return QCoreApplication::translate("ServerPing", "连接超时");
    case QAbstractSocket::RemoteHostClosedError:
        return QCoreApplication::translate("ServerPing", "连接已断开");
    case QAbstractSocket::SocketAccessError:
        return QCoreApplication::translate("ServerPing", "连接受限");
    default:
        return QCoreApplication::translate("ServerPing", "连接失败");
    }
}

namespace {
/// 这个 socket 错属于"超时"那一类吗（**唯一判据**，上面 `Status::timedOut` 那段）。
///
/// 两处都在用：连的那一步（`SocketTimeoutError` / `NetworkError` —— 后者是"线被拔了"那一档，
/// connectionErrorText() 早就把它和超时归在同一句里）和"等它答话"那一步（那里额外把
/// `UnknownSocketError` 也算进来：Qt 在 waitForReadyRead 到点时会留下它，见那段注释）。
bool isTimeoutError(QAbstractSocket::SocketError error) {
    return error == QAbstractSocket::SocketTimeoutError || error == QAbstractSocket::NetworkError;
}
}  // namespace

Status ping(const QString &address) {
    Status status;
    QElapsedTimer clock;
    clock.start();
    const int budget = timeoutMs();
    const auto left = [&clock, budget] { return qMax(1, budget - static_cast<int>(clock.elapsed())); };

    // 先按 SRV 算一遍"真正要连哪一对"（写了端口 / IP / localhost 那几种
    // 情况 resolve() 自己就不查，见它的规矩）。SRV 这一段和建连**共用同一个预算** ——
    // 否则一台写域名、又没有 SRV 的服务器会连着超时两回。
    const Resolution resolved = resolve(address, budget);
    status.connectHost = resolved.connectHost;
    status.connectPort = resolved.connectPort;
    status.srvUsed = resolved.srvUsed;
    if (!resolved.valid) {
        status.kind = Kind::InvalidAddress;
        status.error =
            QCoreApplication::translate("ServerPing", "服务器地址格式无效。请输入主机名和端口，省略端口时使用 25565。");
        return status;
    }
    const Address target = resolved.address;

    QTcpSocket socket;

    socket.connectToHost(resolved.connectHost, static_cast<quint16>(resolved.connectPort));
    if (!socket.waitForConnected(left())) {
        status.kind = kindForSocketError(socket.error());
        status.error = connectionErrorText(socket.error());
        status.timedOut = isTimeoutError(socket.error());
        return status;
    }

    // 延迟从**这儿**起算，不是从进这个函数起算：上面那一段（DNS 解析 +
    // TCP 三次握手）不算进 `latencyMs`。否则那一格写的是"解析域名 +
    // 建连 + 握手一个来回"—— 一个 RTT 的服务器在我们这儿看着像两个 RTT（见 .hpp
    // 里 latencyMs 那一段：游戏里那一列量的也是连上之后的一个来回）。
    //
    // NOTE: **两把表各管各的**。`clock` 管整趟预算（超时还得从进函数起算，否则一个连不
    // 上的地址会连着超时两回），`roundTrip` 只量延迟。
    QElapsedTimer roundTrip;
    roundTrip.start();

    // ① 握手 ② 状态请求。两发一起写出去（服务器那边一个 read 就能收全）。
    const QByteArray handshakeBody =
        varIntBytes(0x00) + varIntBytes(kUnknownProtocolVersion) + stringBytes(target.host) +
        portBytes(target.port) + varIntBytes(1);
    const QByteArray requestBody = varIntBytes(0x00);
    socket.write(packet(handshakeBody));
    socket.write(packet(requestBody));
    if (!socket.waitForBytesWritten(left())) {
        status.kind = Kind::ConnectionLost;
        status.error = QCoreApplication::translate("ServerPing", "发不出去（连接刚建立就断了）");
        return status;
    }

    // 收：一个长度前缀 + 它说那么长的正文。**按长度收全再解析**（响应可能分好几包到，
    // 尤其是带图标的那种），少一个字节都不能当成"这个包就这样"。
    QByteArray buffer;
    int bodyStart = -1;
    int need = -1;
    while (true) {
        if (need < 0 && !buffer.isEmpty()) {
            int offset = 0;
            int size = 0;
            if (readVarInt(buffer, &offset, &size)) {
                if (size < 0 || size > kMaxPacketBytes) {
                    status.kind = Kind::Protocol;
                    status.error = QCoreApplication::translate("ServerPing", "服务器响应长度无效");
                    return status;
                }
                bodyStart = offset;
                need = offset + size;
            } else if (buffer.size() > 5) {
                status.kind = Kind::Protocol;
                status.error =
                    QCoreApplication::translate("ServerPing", "服务器响应缺少有效的长度字段");
                return status;
            }
        }
        if (need >= 0 && buffer.size() >= need) {
            break;
        }
        if (!socket.waitForReadyRead(left())) {
            // 「等不到东西」有两种，而**它们不能用 error() 分**：Qt 在 waitForReadyRead 超时
            // 时也会把 error() 设成 SocketTimeoutError（"网络不通"那句），可这时候连接明明
            // 已经建起来了（上面 waitForConnected 过了）—— 该说的是"它没答话"。所以这里按
            // **走到哪一步了**分：连接这一段的错误（对面把连接断了之类）照表翻，剩下的一律
            // 是"连上了、但它一个字都没回"。
            if (socket.error() == QAbstractSocket::SocketTimeoutError ||
                socket.error() == QAbstractSocket::UnknownSocketError) {
                status.kind = Kind::NoAnswer;
                status.error = QCoreApplication::translate("ServerPing", "已连接，但服务器没有响应（等待超时）");
                status.timedOut = true;
            } else {
                status.kind = kindForSocketError(socket.error());
                status.error = connectionErrorText(socket.error());
                status.timedOut = isTimeoutError(socket.error());
            }
            return status;
        }
        buffer += socket.readAll();
    }

    int pos = bodyStart;
    int packetId = 0;
    if (!readVarInt(buffer, &pos, &packetId) || packetId != 0x00) {
        status.kind = Kind::Protocol;
        status.error = QCoreApplication::translate("ServerPing", "服务器答的不是状态包");
        return status;
    }
    int jsonLength = 0;
    // 先做减法：`pos + jsonLength` 在 jsonLength 接近 INT_MAX 时会**有符号溢出**（UB），
    // 于是这道"长度不对"的闸被绕过。`buffer.size() - pos` 一定非负（pos 由上面的
    // readVarInt 保证不超过 size），拿它去比既不会溢出，也不会放宽判据。
    if (!readVarInt(buffer, &pos, &jsonLength) || jsonLength < 0
        || jsonLength > buffer.size() - pos) {
        status.kind = Kind::Protocol;
        status.error = QCoreApplication::translate("ServerPing", "服务器响应的正文长度无效");
        return status;
    }
    const QJsonDocument document = QJsonDocument::fromJson(buffer.mid(pos, jsonLength));
    if (!document.isObject()) {
        status.kind = Kind::Protocol;
        status.error = QCoreApplication::translate("ServerPing", "服务器答的不是一段 JSON");
        return status;
    }
    const QJsonObject root = document.object();

    status.ok = true;
    status.kind = Kind::Ok;
    status.latencyMs = static_cast<int>(roundTrip.elapsed());
    status.motd = motdText(root.value(QStringLiteral("description")));
    // 同一格东西的**另一种写法**：留着 `§` 码和换行，给"照游戏里的样子画出来"用
    // （见 motdRaw 那段）。两个都留着是有意的 —— 洗过的那份进 tooltip 和日志（一行、
    // 可读），没洗的那份进列表（两行、带颜色）。
    status.motdRaw = motdRaw(root.value(QStringLiteral("description")));
    status.versionName =
        root.value(QStringLiteral("version")).toObject().value(QStringLiteral("name")).toString();
    const QJsonObject players = root.value(QStringLiteral("players")).toObject();
    if (players.contains(QStringLiteral("online"))) {
        status.playersOnline = players.value(QStringLiteral("online")).toInt(-1);
    }
    if (players.contains(QStringLiteral("max"))) {
        status.playersMax = players.value(QStringLiteral("max")).toInt(-1);
    }
    // 服务器列出的在线玩家名。只收对象里的 `name`（协议写死的那种形状）；`§` 码洗掉，
    // 空名 / 重名跳过。上限 32：这一栏是服务器给的，长度不该由对面决定。
    constexpr int kMaxSamplePlayers = 32;
    const QJsonValue sampleValue = players.value(QStringLiteral("sample"));
    if (sampleValue.isArray()) {
        for (const QJsonValue &entry : sampleValue.toArray()) {
            if (status.samplePlayers.size() >= kMaxSamplePlayers) {
                break;
            }
            const QString name =
                cleanMotd(entry.toObject().value(QStringLiteral("name")).toString());
            if (name.isEmpty() || status.samplePlayers.contains(name)) {
                continue;
            }
            status.samplePlayers.append(name);
        }
    }
    status.iconPng = decodeFavicon(root.value(QStringLiteral("favicon")).toString());
    return status;
}

void registerMetaTypes() {
    // 名字**必须和 `Q_ARG` / 信号参数里写的一字不差**（`ServerPing::Status`）——
    // 见 .hpp 末尾 Q_DECLARE_METATYPE 那条 NOTE。
    qRegisterMetaType<ServerPing::Status>("ServerPing::Status");
}

Pinger::Pinger(QObject *parent) : QObject(parent) {
    // 跨线程排队投递要先登记（见 registerMetaTypes）。构造一次就够，重复调用是幂等的。
    registerMetaTypes();
}

Pinger::~Pinger() {
    // **必须先叫停再等**：`QThread` 析构时还在跑会打一句警告，而且那些 lambda 手里攥着
    // `this` —— 线程没停就把它删了，下一句 `emit` 就是野指针。
    abort();
    for (QThread *thread : m_threads) {
        if (thread != nullptr) {
            thread->wait();
        }
    }
}

void Pinger::start(const QStringList &addresses) {
    if (running()) {
        return;
    }
    if (addresses.isEmpty()) {
        emit allFinished();
        return;
    }

    // 上一趟那些线程对象还挂着（跑完了，但 deletion 还排着队）：先放它们走。不管的话，
    // 用户每点一次「测速」就在父对象底下攒一堆僵尸 QThread（一整个会话下来能攒几十个）。
    // NOTE: 从**主线程**调 deleteLater 是安全的 —— 这些对象本来就住在主线程（它们是在
    // start() 里 new 出来的），线程跑完了不欠任何东西。
    //
    // 只放走**已经收摊**的那些。abort() 只放下 `m_running`，每条线程
    // 手上那一台还在同步 socket 的 4 秒超时里；用户取消之后立刻再点「测速」（取消会把
    // 按钮重新点亮），**不能**对一条**还在跑**的 QThread `deleteLater` 并把它从表里
    // 抹掉 —— 主线程处理那条 DeferredDelete 时 Qt 直接
    // `qFatal("QThread: Destroyed while thread is still running")`，整个启动器当场消失。
    // 还在跑的那几条留在表里：它们自己会在超时到了之后退出，下一趟 start() 再收；
    // 一直没收的由 `~Pinger()` wait 着收。
    for (int i = m_threads.size() - 1; i >= 0; --i) {
        QThread *thread = m_threads.at(i);
        if (thread == nullptr) {
            m_threads.removeAt(i);
        } else if (thread->isFinished()) {
            thread->deleteLater();
            m_threads.removeAt(i);
        }
    }

    m_running.storeRelease(1);
    // 本趟的代号（见头文件里 m_generation 那段）：abort() 之后紧接着的这一发 start()
    // 会让旧线程手上的 `m_running` 重新变成 1，号对不上才分得开两趟。
    const int generation = m_generation.fetchAndAddOrdered(1) + 1;
    const int total = addresses.size();
    // 同时开几条：**不超过台数**（三台就开三条，不开八条在那儿抢一个空号）。
    const int workers = qMin(total, kMaxConcurrentPings);
    // 这一趟"下一个该问第几号"那个号牌。几条线程一起抢它 —— 所以慢的那一台堵不住别人，
    // 也不会出现"某一条线程摊到的恰好是那三台慢的"这种事。抓在 shared_ptr 里是因为几条
    // 线程都要拿着它，而它该活到最后一条线程收摊为止（下面那几个 lambda 里没有别的东西
    // 能管这件事：它们只是各自的副本）。
    auto cursor = std::make_shared<QAtomicInt>(0);
    // 还活着几条线程。**最后一条收摊的**（`deref()` 回假）负责收尾 —— running 放下来、
    // 发 allFinished()。**不能让每条线程都发一次**：接的人按 allFinished 收掉"测速中"，
    // 收八次和收一次在屏幕上是一个样子，但那个信号的意思是"这一趟完了"，一份信号发八遍
    // 不是这个意思。
    // 这里的方向**唯一出处就是 deref() 自己的语义**：它等价于 `--i != 0`（减完还有人在
    // 就回真），所以"最后一条"是**回假**那一条。 写成"回真"了 —— 症状是
    // 最先收摊的那条线程（往往是号牌发完、一台都没轮上的那条，几乎立刻就退出）抢先把
    // allFinished 发出去，接的人当这一趟完了：**屏幕上只有最先回来的那一台有结果**，
    // 其余几台永远停在「还没测」，而按钮已经变回「测速」、看着像跑完了。
    // 验证工程 T20.6（三台：答话的/不答话的/没人听的）抓的就是这一条。
    auto live = std::make_shared<QAtomicInt>(workers);

    // `QThread::create` 那个重载收的是一个函数对象（Qt 5.10 起）—— **它省掉了"再写一个
    // QThread 子类 + 在 .cpp 里 moc"**，而这个类本来也不需要线程自己有信号：结果是从
    // 工作线程里 `emit` 主线程那个对象的信号，Qt 按接收方所在线程自动排队（这就是"跨线程
    // 发信号"唯一要守的规矩）。
    for (int w = 0; w < workers; ++w) {
        QThread *thread = QThread::create([this, addresses, total, cursor, live, generation]() {
            for (;;) {
                // 每一台之前查一次"被叫停没有"：abort() 之后每条线程最多再跑完手上这一台。
                // 号（generation）也得对得上：abort 之后紧跟着 start 会把 `m_running`
                // 重新置 1，只看它的话上一趟的线程会接着往下跑、抢上一趟的号牌。
                if (m_running.loadAcquire() == 0 || m_generation.loadAcquire() != generation) {
                    break;
                }
                const int i = cursor->fetchAndAddRelaxed(1);
                if (i >= total) {
                    break;  // 号牌发完了（别人已经把剩下的都领走了）
                }
                const ServerPing::Status status = ServerPing::ping(addresses.at(i));
                // 手上这一台问完了：没被取消就照常报（abort 的约定是"手上那一台问完"）。
                // 但如果这期间已经开了**新的一趟**，这一条结果属于上一趟的下标，报上去
                // 会被接的人当成新表的第 i 台 —— 扔掉。
                if (m_generation.loadAcquire() != generation) {
                    break;
                }
                QMetaObject::invokeMethod(this, [this, generation, i, status] {
                    if (m_generation.loadAcquire() == generation) emit oneFinished(i, status);
                }, Qt::QueuedConnection);
            }
            // `deref()` 是 `--i != 0`（**还有人在**）—— 所以"最后一条"是**回假**那一条。
            // 反着写的代价见上面 `live` 那一段。
            if (!live->deref()) {
                // 收尾也只收自己那一趟：上一趟的"最后一条"晚到时，新一趟可能正跑着，
                // 这时候清零 / 发 allFinished 会把新一趟的忙态和结果一起打掉。
                QMetaObject::invokeMethod(this, [this, generation] {
                    if (m_generation.loadAcquire() != generation) return;
                    m_running.storeRelease(0);
                    emit allFinished();
                }, Qt::QueuedConnection);
            }
        });
        thread->setParent(this);
        m_threads.append(thread);
        thread->start();
    }
}

void Pinger::abort() {
    m_running.storeRelease(0);
}

bool Pinger::running() const {
    return m_running.loadAcquire() != 0;
}

}  // namespace ServerPing

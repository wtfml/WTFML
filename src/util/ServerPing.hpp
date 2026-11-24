/**
 * @file ServerPing.hpp
 * @brief 问一台 Minecraft 服务器"你现在什么样"（Server List Ping / 状态协议）。
 * @note 用户 问的：「服务器列表能实际 ping 服务器并显示服务器图标和 motd 吗」。
 * 能 —— 就是**游戏自己那个多人列表**背后的那套东西：游戏在多人界面里给每个服务器
 * 显示的那个小图标、那两行 MOTD、在线人数和信号格，全都来自同一个协议。
 *
 * 【这个协议是什么】
 *
 * 一条普通 TCP 连接，客户端发两个包、服务器答一个包（**不登录、不进游戏**）：
 * ① 握手（packet id 0x00）：协议版本（VarInt）、主机名（String）、端口（uint16）、
 * 下一状态（VarInt，**1 = status**）；
 * ② 状态请求（packet id 0x00，空正文）；
 * ③ 服务器答一个包（id 0x00），正文是一段 **JSON**：`description`（MOTD）、
 * `favicon`（`data:image/png;base64,…`）、`players.online/max`、`version.name`。
 * 全都是 VarInt 长度前缀 + 正文，没有加密、没有压缩。README 级别的说明在
 * https://wiki.vg/Server_List_Ping （离线也能照上面那几行写出来，本条注释就是全部依据）。
 *
 * 【为什么同步】
 *
 * 和 `HttpClient` 同一条理由：调用方是界面上的一次点击，本来就等着一个结果；异步化只会让
 * 两边都多出一堆状态机。**一次问一台**，串起来问的那一段（每台之间让界面转一圈）在
 * MainWindow 里 —— 那是界面的事，不是协议的事，所以不放进这个文件。
 *
 * NOTE: 这个文件**只读**：连上去、问一句、把答案拆开，一个字节都不往下发。它不发聊天、
 * 不登录、不传账号 —— 服务器那边看到的和"有人在多人界面里刷了一下列表"一模一样。
 */

#pragma once

#include <QAtomicInt>
#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QThread;

namespace ServerPing {

/// 没写端口时用这个（游戏的默认值）。
constexpr int kDefaultPort = 25565;

/// 一次 ping 的超时（毫秒）。**唯一出处** —— 验证工程把它压到几十毫秒来验"连不上"那一档。
///
/// 4 秒是"能等、但不会让人以为界面死掉"的那一档：真连不上的服务器大多是
/// **连接被拒**（立刻回）或者 DNS 立刻失败；只有丢包的服务器才会耗满它。
int timeoutMs();
void setTimeoutMs(int ms);

/// 一趟里**同时**问几台。**唯一出处** —— `Pinger::start()` 按它决定开几条线程。
///
/// 为什么要并发（用户 ：「能多线程一起 ping 吗，一个个 ping 效率有点低」）：
/// 一次 ping 最多等满 timeoutMs()（默认 4 秒），而**等**这件事几乎全是网络往返 —— 一台
/// 一台问的话，20 台里有 3 台丢包就是 12 秒，而这三段等待本来可以互相盖住。8 路并发把
/// 最坏情况从"台数 × 4 秒"压到"台数 / 8 × 4 秒"。
///
/// 取 8：一个桌面程序同时开 8 条 TCP 出去是极轻的事（浏览器每个域名就开 6 条），而正常人
/// 手上的服务器列表远不到 8 台 —— 到不到，等的时长都一样。**别调成几百**：那是拿用户的
/// 路由器当压测目标，而且对方看到的会是"同一个 IP 一瞬间连了我几十次"。
constexpr int kMaxConcurrentPings = 8;

/// 一个地址拆出来的两半（见 splitAddress）。
struct Address {
    QString host;
    int port = kDefaultPort;
    bool valid = false;
    /// 端口是**用户写出来的**（还是落回了默认那一个）。SRV 只在"没写端口"时查 ——
    /// 游戏自己也是这个规矩：写了端口就是"我就要连这一对"，不许 SRV 再把它换掉。
    bool portExplicit = false;
};

/**
 * @brief 把 `example.com` / `example.com:25566` / `[::1]:25566` 拆成主机和端口。
 *
 * 规矩（就这几条）：
 * - 两头空白一律去掉；
 * - `[…]` 包着的是 IPv6 字面量，方括号**要去掉**（Qt 的 connectToHost 收的是裸地址），
 * 方括号后面可以再跟 `:端口`；
 * - 其余情况按**最后一个** `:` 拆 —— 主机名里本来不会有冒号，最后一个才是"端口那个"；
 * 所以没加方括号的 IPv6（`::1`，冒号不止一个）**看不懂** —— 按最后一个冒号拆会得到
 * 主机 `::`、端口 `1`，一个看着合法、其实谁也不是的地址；
 * - 没写端口 = kDefaultPort；端口不是数字、或者不在 1..65535 里 = 看不懂（valid=false）。
 *
 * NOTE: 空串 / 只有端口 / 端口写成 `0` 都算看不懂，**不当成"默认端口"** —— 把看不懂的地址
 * 悄悄改成 25565 去连，用户看到的是"这个服务器连不上"，而真正的原因是这行地址坏了。
 */
Address splitAddress(const QString &text);

// ---------- SRV（`_minecraft._tcp.<主机>`） ----------

/// 一台主机在 DNS 里自己报的服务地址（SRV 记录的四条：目标、端口、优先级、权重）。
struct SrvRecord {
    QString target;
    int port = 0;
    int priority = 0;
    int weight = 0;
};

/// 过滤掉**用不了**的记录（目标空、端口不在 1..65535），只留 priority 最小的那一组。
///
/// 为什么先挑优先级再挑权重：SRV 的规矩是"优先级是硬次序、权重只排同一优先级里的先后"。
/// 把两者混在一起算（比如按 weight/priority 排）会在有备份服务器时**先连到备份上**。
QList<SrvRecord> bestPriorityRecords(const QList<SrvRecord> &records);

/**
 * @brief 在**同一优先级**那一组里按权重挑一条。
 * @param sample 一个随机数（验证工程传定值来钉"权重怎么分"）。>= totalWeight 时从 0 算。
 * @return 挑不出来（空表 / 全不合法）回 `false`，@p out 不动。
 *
 * 权重全是 0 的合法写法是"所有记录等概率"（RFC 2782），不是"谁都不选"。
 */
bool pickSrvRecord(const QList<SrvRecord> &records, quint32 sample, SrvRecord *out);

/// SRV 查询开着没有（**默认开**，和游戏一样）。关掉之后 `resolve()` 直接回用户写的那一对 ——
/// 验证工程要离线跑、或者用户在不认 SRV 的环境里想少等一趟 DNS，都可以关。
bool srvLookupEnabled();
void setSrvLookupEnabled(bool on);

/// 一条地址**真正会连到哪**（见 resolve()）。
struct Resolution {
    /// 用户写的那一对（splitAddress 的结果）。valid=false 时后面几格没有意义。
    Address address;
    /// 真正去连的主机（SRV 命中时是记录里的 target；否则就是 address.host）。
    QString connectHost;
    int connectPort = kDefaultPort;
    /// 查了 SRV、而且命中了（connectHost/connectPort 因此和 address 那两格不同）。
    bool srvUsed = false;
    /// 查了 SRV，但没查到（NXDOMAIN / 服务器没答 / 超时）。**不是错误**：照用户写的连。
    bool srvTried = false;
    /// 为什么没查到（中文，只作说明；srvTried && !srvUsed 时才有）。
    QString srvError;
    bool valid = false;
};

/**
 * @brief 按"用户写的地址 → 实际要连的主机:端口"算一遍（含 SRV，阻塞、最多 @p budgetMs 毫秒）。
 *
 * @param budgetMs <= 0 用 timeoutMs()；SRV 这一步**最多花一半预算**（见 .cpp）。
 */
Resolution resolve(const QString &text, int budgetMs = -1);

/// 没通的原因**归成哪一类**（协议错误分类）。
///
/// 界面**不许再拿 `Status::error` 那句中文去认**（那是给人看的字，改一个字就静默失配）。
/// 要判"哪种没通"一律问 `Status::kind`。
enum class Kind {
    Ok = 0,           ///< 通了
    InvalidAddress,   ///< 地址本身就看不懂（`splitAddress` 那几条规矩）
    HostNotFound,     ///< DNS 说没有这台主机
    Refused,          ///< 对面没开 / 端口不对（TCP 被拒）
    Unreachable,      ///< 网络不通（含 connect 那一步超时）
    NoAnswer,         ///< **连上了但是一个字节都没答**（协议层面的"没回话"）
    ConnectionLost,   ///< 连上之后被对面断开 / 写不出去
    Protocol,         ///< 答了，但答的不是一个合法的状态包（长度 / 包号 / JSON）
    Other,            ///< 其余（本机不让连之类）
};

/// 这一类的机器判据（界面上要换一句话时用它，不去认中文）。
QString kindName(Kind kind);

/// 一个 socket 错误归到哪一类（超时那一档由 `Status::timedOut` 另记，见 isTimeoutError）。
Kind kindForSocketError(int socketError);

/// 一次 ping 的结果。**OK 和"为什么没 OK"两件事都在这里面**（同 HttpClient::Reply）。
struct Status {
    bool ok = false;
    /// 没通的是哪一类（机器判据；`error` 那句中文**只给人看**）。
    Kind kind = Kind::Other;
    /// 没通的原因（**中文**，按 connectionErrorText() 那张表翻；"连上了但没人答"那一档是
    /// ping() 自己写的一句）。ok 为真时是空串。
    QString error;
    /// "没通"这一档里**属于超时**的那些（`error` 是「超时（网络不通）」或者「超时（连上了，
    /// 但服务器没有回答）」两句之一）。
    ///
    /// 为什么要单独一位、而不是让界面上那句 `error` 前缀去认：界面上那一格对超时只写
    /// 两个字（用户 ：「超时的只写超时」），而 `error` 那句话要留着全的 ——
    /// "网络不通"和"它连上了却不答话"是**两件不同的事**（同一台服务器换了个端口就从
    /// 前者变成后者），排查的时候要的正是这个区别。判据在 .cpp 里只有一处
    /// （`isTimeoutError()`），界面上不许再去拿字符串认。
    ///
    /// NOTE: 缓存（ServerStatusCache::Entry）**存这一位**，照旧不存
    /// `error` 那句理由 —— 理由是当时那一刻的。盘上那份"上次问到"的没通按它写
    /// 「超时」或者「没连上」（见那边 Entry::timedOut 那条 NOTE）—— 那两句和当场
    /// 测出来写的是**同一句字**。
    bool timedOut = false;
    /// 延迟：**连接建好之后**，握手那一趟往返花了多久（毫秒）。ok 为假时是 -1。
    int latencyMs = -1;
    /// MOTD：已经把 `§` 颜色码去掉、多行合成一行（见 motdText()）。服务器没给就是空串。
    ///
    /// NOTE: 这一份是给**一行字**用的地方看的（悬停提示、日志）。列表里那一格要的是
    /// `motdRaw`（见下）—— 它按游戏的样子分色画、还是两行。
    QString motd;
    /// MOTD 的**原样**：`§` 码和换行都留着（见 motdRaw()）。服务器没给就是空串。
    QString motdRaw;
    /// 服务器自报的版本名（比如 `Paper 1.20.4`）。没给就是空串。
    QString versionName;
    /// 服务器自报的在线玩家名（`players.sample[].name`）。**已经去掉 `§` 码**、
    /// 空白收过；`id` 那一栏不要。最多 32 个（防一份超长数组把提示框撑破）。
    ///
    /// 空表 = 服务器**没给这一栏**（== 没人在线的说法不成立：很多服故意关掉或只给一部分）。
    /// 名字是服务器自己写的，界面只当"它这么说"显示。
    QStringList samplePlayers;
    /// 在线 / 上限人数。**服务器没报就是 -1**（不是 0 —— "一个人都没有"和"它没说"不同）。
    int playersOnline = -1;
    int playersMax = -1;
    /// 图标：`favicon` 那串 data URL 解出来的**原始 PNG 字节**（没给就是空）。
    /// 解成图片是界面那一层的事（这里不碰 QPixmap，util 层不开图形依赖）。
    QByteArray iconPng;
    /// 这一趟**真正连的**主机/端口（SRV 命中时和用户写的不一样；见 resolve()）。
    QString connectHost;
    int connectPort = -1;
    /// 这一趟是 SRV 换过去的（界面想写"经 SRV → xxx:yyyy"时用；不写也行）。
    bool srvUsed = false;
};

/// 问一台（同步，最多 timeoutMs()）。
Status ping(const QString &address);

/**
 * @brief 「一台一台问下去」这件事，**在后台线程上跑**。
 *
 * 线程模型：这一段**在后台线程上跑**，界面线程不阻塞。不能在界面线程里同步跑
 * （每台之间 `processEvents(ExcludeUserInputEvents)` 转一圈也不行）——那几秒里整个设置页
 * 是按不动的：想翻到别的页要等、想关掉要等，一台服务器连满 4 秒超时的时候尤其难受。
 * "边测边能点别的"就是默认行为。
 *
 * 用法：`start()` 之后一台问完发一次 `oneFinished(index, …)`（下标与传进去那份一一对应），
 * 全问完发 `allFinished()`。**结果和结束状态都排队到对象所属线程**，投递前核对请求代数；Qt 自己
 * 按队列投递（所以 `Status` 注册过元类型，见 .cpp）。
 *
 * 【几条线程一起问，结果按"第几台"认人】
 *
 * 开 `min(台数, kMaxConcurrentPings)` 条线程，它们**抢同一串下标**（谁先问完谁拿下一个号）
 * —— 所以慢的那一台堵不住别人，也不会出现"第 3 条线程摊到的恰好是那三台慢的"这种事。
 * 代价只有一个：**`oneFinished` 的次序不再是 0、1、2…**（快的那台先回来）。接的人本来就
 * 该按 `index` 认人（MainWindow 那边正是这么写的），不许按下标收到的先后去推。
 *
 * NOTE: 这个类**不认识界面**（util 层一贯如此）：它不知道"测速中…"那几个字、也不知道
 * 那颗按钮该不该灰。那些都是接的人的事。
 */
class Pinger : public QObject {
    Q_OBJECT

public:
    explicit Pinger(QObject *parent = nullptr);
    ~Pinger() override;

    /// 开始（**已经在跑就什么都不做** —— 连点两下发两遍请求是用户没要的）。
    /// 空表 = 立刻发一次 allFinished()（"一台都没有"也是走完了）。
    void start(const QStringList &addresses);

    /// 叫停：**每一条线程手上那一台会问完**（同步的 socket 等没法从外面掐断，最多
    /// timeoutMs()），还没被抢走的那些不再问，然后照常发一次 allFinished()。
    /// 并发之后"手上那一台"最多有 kMaxConcurrentPings 台 —— 这是那一句的代价。
    void abort();

    bool running() const;

signals:
    /// 第 @p index 台问完了。@param status 就是 `ping()` 那一趟的结果。
    void oneFinished(int index, const ServerPing::Status &status);
    /// 全跑完了（**含被 abort 掐掉的情况**）—— 接的人按它把"测速中"收掉。
    void allFinished();

private:
    /// 开过的那几条线程（并发之后就不是一条了）。跑完的那些留在表里，下一趟 start() 时
    /// **只把已经收摊的** deleteLater 掉；还在跑的（abort 之后手上那一台要等最多
    /// timeoutMs()）继续留在表里等它自己收摊，再由下一趟 / 析构收走 —— **不能对还在跑的
    /// QThread 调 deleteLater**：Qt 析构那条线程时直接 `qFatal`（整个进程消失）。
    QList<QThread *> m_threads;
    QAtomicInt m_running{0};
    /// 第几趟。abort() 只把 `m_running` 放下，而紧接着的 start() 又会把它置回 1；
    /// 旧线程只看 `m_running` 的话会把新一趟当成自己那一趟继续跑，收尾时还会把新一趟的
    /// `m_running` 清零、替它发 allFinished。每条线程攥着自己那一趟的号，号对不上就退。
    QAtomicInt m_generation{0};
};

/// 把异步那一支要跨线程传的 `Status` 登记进元类型系统。**构造函数里会自己调一次**，
/// 验证工程拿 `invokeMethod` 直接驱动时用得上（同 `ModpackImport::Format` 那条老账：
/// 名字必须和 `Q_ARG` 里写的一字不差）。
void registerMetaTypes();

// ---------- 下面这几个是"拆包和拆 JSON"的纯函数，验证工程直接拿它们核对 ----------

/// VarInt 编出来长什么样（协议里每个长度前缀都是它）。**负数按 5 字节**（补码）。
QByteArray varIntBytes(int value);

/**
 * @brief 从一个字节串的 **offset 处**读一个 VarInt。
 * @param data 待读的字节串。
 * @param offset 进出自：进来是起点，成功时推到 VarInt 后面那个字节。
 * @param out 成功时写入读到的值。
 * @return 读到没有：**false 有两种**（"这一段还不够一个 VarInt"和"超过了 5 字节还是没完"），
 * 两者都只是"现在读不了"，包一层判断由调用方去做 —— 这个函数不做区分，因为
 * 在那两种情况下调用方要做的事是同一件（等更多字节进来 / 判这个包坏了）。
 */
bool readVarInt(const QByteArray &data, int *offset, int *out);

/// `description` 那一格（可能是字符串，也可能是 `{text, extra:[…]}` 的对象）展平成一行。
/// 见 .cpp 里那段"这块 JSON 的形态"。
QString motdText(const QJsonValue &description);

/**
 * @brief 同一个 `description` 的**原样**版本：`§` 颜色码和换行都留着。
 *
 * @return 一串"游戏那种写法"的字（`§a第一行\n§l第二行`）。服务器没给就是空串。
 *
 * 【为什么要有两个】
 *
 * `motdText()` 是洗过的（颜色码去掉、两行并一行）—— 那是给"只有一行字的地方"用的
 * （悬停提示、日志）。而列表里那一格要**照游戏里的样子画**（用户 ：「motd按照
 * 游戏里显示的样式展现」），那就得知道哪儿是什么颜色、哪儿换的行 —— 洗掉就再也回不来了。
 *
 * NOTE: JSON 组件那一档的 `color` / `bold` 那些字段**会被翻回 `§` 码**（见 .cpp 里
 * flattenMotdRich 那段"继承怎么算"）—— 因为画的那一层只认 `§` 串，而颜色变成
 * QColor 得有 QtGui，这一层**故意没有**那个依赖（见文件头）。
 */
QString motdRaw(const QJsonValue &description);

/// `§` 颜色码去掉、换行/制表压成空格、连续空格合并成一个。motdText() 收尾会用一次。
QString cleanMotd(const QString &raw);

/// `data:image/png;base64,…` 解成原始 PNG 字节。**认不出前缀就返回空**（不猜别的形态）。
QByteArray decodeFavicon(const QString &dataUrl);

/// 这个 socket 错误对应哪句话。**自己写一张表，不用 QAbstractSocket::errorString()** ——
/// 那是 Qt 翻译文件里的英文句子，而我们的部署里没有 Qt 的 .qm（`--no-translations`）。
QString connectionErrorText(int socketError);

}  // namespace ServerPing

// 这一条**必须在命名空间外面**：
// 写在里面的话，`qRegisterMetaType<T>()` 注册出来的是**裸类型名**（`Status`），而
// `Q_ARG(ServerPing::Status, …)` 找的是修过名那个 —— 对不上等于没注册，`invokeMethod`
// **静默返回 false**（编得过、跑起来什么都不发生）。跨线程的排队连接同理。
Q_DECLARE_METATYPE(ServerPing::Status)

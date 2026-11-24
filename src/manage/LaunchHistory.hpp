/**
 * @file LaunchHistory.hpp
 * @brief 「上一次启动」那本账：主页那张卡片的唯一数据源。
 *
 * 【为什么单独一层】
 *
 * 和 `TaskHistory` / `WorldFiles` 同一个理由：读盘、写盘、判"这一条还算不算数"都是纯逻辑，
 * 收在一个不碰界面的地方，验证工程才好逐格念（时间怎么说、目标没了怎么降级、上限怎么截、
 * 坏文件读回来是什么）。界面那一层只拿 `Record` 摆字和摆按钮。
 *
 * 【"上一次"以什么为准】
 *
 * **以成功创建游戏进程为准**，不以点击启动按钮为准。点下去之后可能是登录失败、
 * Java 挑不出来、进程起不来 —— 那些都不该把"上次玩的是哪一份"改掉（`Result::StartFailed`
 * 这一档是留给"起来了又立刻死"和"没起来但用户该知道"的少数场面，主页那张卡**不认它**，
 * 见 `latestPlayable()`）。
 *
 * 【为什么时间要自己算】
 *
 * 卡片上写的是「昨天 22:14」这种**人话**，不是 `T22:14`。这一层提供
 * `whenText()`（纯函数：给两个时间，出一个字符串），所以"昨天"这条线能逐格验，而不是靠
 * 人肉盯着界面看。
 */

#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

namespace LaunchHistory {

struct PlayTime {
    qint64 totalMs = 0;
    qint64 lastMs = -1;
};

/// 同一实例从首个进程启动到最后一个进程结束，合计一次连续游玩。
class PlayTimeCounter {
public:
    explicit PlayTimeCounter(PlayTime completed = {}) : m_completed(completed) {}

    void start(int sessionId, qint64 tickMs);
    bool finish(int sessionId, qint64 tickMs);
    qint64 currentMs(qint64 tickMs) const;
    qint64 totalMs(qint64 tickMs) const;
    PlayTime completed() const { return m_completed; }
    /// 清掉这一次会话开始前记下的累计（用户手动重置游玩时长时用）。
    void resetCompleted() { m_completed = PlayTime{}; }

private:
    PlayTime m_completed;
    QSet<int> m_sessions;
    qint64 m_startedAtMs = 0;
};

PlayTime loadPlayTime(const QString &path, const QString &stableInstanceId);
QHash<QString, PlayTime> loadPlayTimes(const QString &path);
bool savePlayTime(const QString &path, const QString &stableInstanceId, const PlayTime &playTime);
QString playTimeText(qint64 milliseconds);

/// 这一趟最后怎么样了。**记的是事实**，不是"我们打算怎么样"。
enum class Result {
    Running,      ///< 还在跑（游戏正常退出的那一刻会被改写成下面那两档之一）
    NormalExit,   ///< 以 0 退出
    Crash,        ///< 异常退出（判据只有 `GameExitDiagnosis::isAbnormal()` 一处）
    StartFailed,  ///< 压根没玩成（进程没起来、或者起来就死）
};

/// 这一趟是**直接进游戏里的哪一块**，还是普通进主菜单。
///
/// 「世界 / 服务器只有通过 Quick Play 启动时才记录，普通进主菜单不能猜」——
/// 所以 `None` 不是"不知道"，是"这一趟就是普通启动"。
enum class Target {
    None,    ///< 普通启动（进主菜单，玩什么由用户在游戏里挑）
    World,   ///< 单人世界（Quick Play）
    Server,  ///< 服务器（Quick Play）
};

/// 启动一次 = 一条记录。
struct Record {
    QString instanceId;         ///< 实例的稳定 id（`versions/<id>` 那个目录名）
    /// **启动那一刻**显示的实例名（`MainWindow::instanceDisplayName()` 那一句，里面已经带着
    /// 版本和加载器）。★ 存当时那一句而不是现在去问实例：实例后来改了名 / 被删了，历史里
    /// 还是当时那个名字 —— 这正是"记录"该有的样子。
    QString instanceName;
    /// 当时用的账号（`AccountStore` 那把钥匙：正版是 uuid，离线是名字）。空 = 记不下来。
    QString accountKey;
    Target target = Target::None;
    /// 世界目录名 / 服务器地址。**稳定键** —— 显示名会改，这个不会。
    QString targetId;
    /// 给用户看的那个名字（世界名 / 服务器条目名）。可能为空（那时界面上写 `targetId`）。
    QString targetDisplayName;
    QDateTime startedAt;
    /// 这一趟玩了多久（秒）。**< 0 = 还在跑或者还没算出来**。
    qint64 durationSeconds = -1;
    Result result = Result::Running;

    /// 这一条拿得出去吗（主页那张卡要的三样：哪一份、什么时候）。
    bool isValid() const { return !instanceId.isEmpty() && startedAt.isValid(); }
};

/// 账本落在哪儿：`<当前游戏文件夹>/wtfml.history`。
///
/// **跟当前游戏文件夹走**（不是 `AppPaths::defaultMinecraftDir()`）：这张卡说的是
/// "这个文件夹里的哪一份实例上次玩过"，而实例本身就在 `versions/` 底下 —— 和
/// `wtfml.folders`（分组）同一档，换了游戏文件夹就该看见另一个文件夹的历史。
/// （任务账本反过来，它是启动器级的，理由见 `Tasks::defaultHistoryPath()`。）
QString historyPath();

/// 留几条。★ 只留最近 20 条（主页只用得上第一条，剩下的留给以后的"活动历史"）。
int historyLimit();

/// 枚举 ↔ 落盘用的那几个词。**唯一出处**（写和读都走它；改一个字要一起改）。
QString resultKey(Result result);
QString targetKey(Target target);
/// 认不出来的词回**默认那一档**（`Running` / `None`）—— 单条记录认不出来不该让整本账作废。
Result resultFromKey(const QString &key);
Target targetFromKey(const QString &key);

/// 一本账 ⇄ 一段字节。`format` 那一位认不出来时 `ok = false`（**整份不认**）。
QByteArray toJson(const QList<Record> &records);
QList<Record> fromJson(const QByteArray &bytes, bool *ok = nullptr);

/// 读盘 / 写盘。**路径是参数**（不是写死 `historyPath()`）：验证工程拿临时目录一喂就能逐格
/// 念，"读回来的和写下去的一样"这种事不该靠改全局状态来测（和 `Tasks::load` 同一套）。
/// **文件不在 = 空账**（不是错误：一次都没启动过的新用户就是这样）。
QList<Record> load(const QString &path);
/// 写盘失败（目录建不出来 / 没有写权限）回 false —— 调用方**不许静默吞掉**。
bool save(const QString &path, const QList<Record> &records);

/// 把一条新记录插到**最前面**，并截到 `historyLimit()`。
QList<Record> prepend(const QList<Record> &records, const Record &record);

/// 账本上"接着玩哪一份"的那一条：按 `startedAt` 取最新的一条可玩记录。
///
/// **跳过 `StartFailed`**（那一趟压根没玩成，拿它当"上次游玩"是撒谎）；
/// `Running` 算数（游戏这会儿正开着，主页那张卡更应该显示它）。
/// @return 一条都没有时回一个 `isValid() == false` 的空记录。
Record latestPlayable(const QList<Record> &records);

/// Latest recorded process start for this instance, across accounts. Invalid if absent.
/// Unlike latestPlayable(), a subsequently failed startup remains a real process start.
QDateTime lastStartedAt(const QList<Record> &records, const QString &instanceId);

/// 「昨天 22:14」这种写法。纯函数：给"什么时候"和"现在"，出一句人话。
///
/// - 今天 → `22:14`；昨天 → `昨天 22:14`；前天 → `前天 22:14`；再早 → ` 22:14`。
/// - 跨年也照旧写年份（"去年"要用户自己算，不如把日子写全）。
/// - 时间无效 → 空串（**不编**）。
QString whenText(const QDateTime &at, const QDateTime &now);
QString whenText(const QDateTime &at);

/// 卡片上第二行那半句：`单人世界：基地周目` / `服务器：play.example.com` / 空串。
///
/// 空串就是"这一趟是普通启动"（`Target::None`），**不写"主菜单"三个字** —— 那是句废话，
/// 卡片上少一行正是"没什么可说的"。
QString targetLine(const Record &record);

/// 卡片上那句补充（`（上次是异常退出）` / `（还在玩）` / `（上次没起来）`）。
/// 正常退出回空串 —— 绝大多数时候都正常，不必占一行。
QString resultNote(const Record &record);

/// 「玩了 1 小时 12 分」那种写法。`< 0` 回空串（还没算出来）；不到一分钟写
/// 「玩了不到 1 分钟」。
///
/// 回的是**整句**（含"玩了"两个字），不是一个数量：卡片上那一行由"目标 · 时长（结局）"
/// 拼出来，让调用方去拼"玩了 %1"会拼出「玩了 不到 1 分钟」那种话。
QString durationText(qint64 seconds);

}  // namespace LaunchHistory

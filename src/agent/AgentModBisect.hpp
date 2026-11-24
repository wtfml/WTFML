/**
 * @file AgentModBisect.hpp
 * @brief 依赖感知二分（docs/dev/AGENT_AUTONOMY.md S5.5）：找出"哪些模组一起启用才会崩"。
 *
 * 这一层是**纯状态机**：给一次启动的结果，返回下一轮要启用哪些文件。启动、重置工作区、
 * 清理日志都在工具层（`AgentModToolEngine`）。这样 delta debugging 的每一步都能用
 * 脚本化的结果在 `verify_agent` 里逐格验，不需要真游戏。
 *
 * 规则：
 * - 加载器/API 前置（Fabric API、Forge、NeoForge、Quilt 等）始终启用；任何被保留模组的
 *   required 前置跟着一起启用（`respectDependencies`）。
 * - 基线跑一次确认崩溃可复现；不能复现直接 `not_reproducible`。
 * - 用 delta debugging 缩减到 1-minimal；单个是 `single`、两个是 `pair`、更多是 `set`；
 *   停用全部候选仍崩是 `not_found`。
 * - 最小集合要再跑 `repeatPerCandidate` 次确认；出现不复现就标 `flaky` 并最多加一轮，
 *   仍不一致回 `inconclusive`。
 * - 启动次数/时间超过预算就停在当前最小集合，标 `budgetExhausted`。
 */

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace AgentModBisect {

struct Mod {
    QString fileName;          ///< `mods/` 下的文件名（不含 `.jar.disabled` 后缀）
    QString modId;             ///< 空 = 读不到声明
    QStringList requiredModIds;
    bool initiallyEnabled = true;
    bool prerequisite = false; ///< 加载器/API 前置，始终启用
    qint64 modifiedMs = 0;
};

struct Options {
    int maxLaunches = 8;
    int repeatPerCandidate = 2;
    int maxMinutes = 45;
    bool respectDependencies = true;
    QStringList prioritizedCandidates;  ///< mod id 或文件名，按顺序先试
};

struct LaunchOutcome {
    bool completed = true;   ///< false = 基础设施失败（没起来/宿主报错），不是"没崩"
    bool crashed = false;
    QString detail;
};

struct Step {
    enum class Kind { Launch, Finished };
    Kind kind = Kind::Launch;
    QStringList enabledFiles;   ///< 这一轮要启用的文件；工作区其余 `.jar` 全部停用
    QString purpose;            ///< baseline / reduce / confirm
    bool confirmation = false;
};

struct Result {
    QString result;      ///< single / pair / set / not_found / not_reproducible / inconclusive
    QStringList culprits;
    QStringList minimalSet;
    int launches = 0;
    bool budgetExhausted = false;
    bool flaky = false;
    QStringList notes;
};

class Bisect {
public:
    Bisect(const QList<Mod> &mods, const Options &options);

    Step step() const;
    void report(const LaunchOutcome &outcome, qint64 elapsedMs = 0);
    bool finished() const { return m_phase == Phase::Done; }
    Result result() const { return m_result; }

    /// 静态事实（工具层用来写盘/报告）。
    QStringList candidateFiles() const;
    QStringList initialEnabledFiles() const;
    /// 某个启用集合实际会启用哪些文件（含前置与依赖闭包）。
    QStringList configuration(const QStringList &files) const;
    /// 加载器/API 前置的 mod id 集合（唯一出处）。
    static QStringList prerequisiteModIds();
    static int maxLaunchesLimit();
    static int maxRepeatLimit();
    static int maxMinutesLimit();

private:
    enum class Phase { Baseline, Reduce, Confirm, Done };

    void beginReduce();
    void finishForBudget();
    bool prepareNextReduction();
    void beginConfirm();
    void finish(const QString &result, const QStringList &minimalSet,
                const QStringList &notes = {});
    void note(const QString &text);
    bool dependencyOfKept(const QString &file) const;

    QList<Mod> m_mods;
    Options m_options;
    QHash<QString, int> m_indexByFile;
    QStringList m_candidates;        ///< 可停用候选（按优先级排好）
    QStringList m_initialEnabled;
    Step m_current;
    Phase m_phase = Phase::Baseline;
    QStringList m_keep;              ///< 仍然启用就能复现的候选集合
    QStringList m_suspects;          ///< 优先试的嫌疑模组（来自 crashSnapshot/模型）
    QStringList m_trialRemaining;    ///< 正在试的那一轮停用后剩下的集合
    int m_granularity = 2;
    int m_subsetIndex = 0;
    int m_confirmationLeft = 0;
    bool m_extraRepeatUsed = false;
    bool m_anyNoCrash = false;
    bool m_anyCrash = false;
    int m_consecutiveFailures = 0;
    Result m_result;
};

}  // namespace AgentModBisect

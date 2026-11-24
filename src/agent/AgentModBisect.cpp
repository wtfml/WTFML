/**
 * @file AgentModBisect.cpp
 * @brief 依赖感知二分的状态机（见 .hpp）。
 */

#include "AgentModBisect.hpp"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("AgentModBisect", text);
}

QStringList subtract(const QStringList &from, const QStringList &remove) {
    const QSet<QString> removed(remove.cbegin(), remove.cend());
    QStringList out;
    for (const QString &item : from) {
        if (!removed.contains(item)) out.append(item);
    }
    return out;
}

/// 均分 @p n 组（组大小差不超过 1）。
QList<QStringList> split(const QStringList &items, int n) {
    QList<QStringList> parts;
    const int count = qMax(1, qMin(n, int(items.size())));
    const int base = items.size() / count;
    const int extra = items.size() % count;
    int offset = 0;
    for (int i = 0; i < count; ++i) {
        const int size = base + (i < extra ? 1 : 0);
        parts.append(items.mid(offset, size));
        offset += size;
    }
    return parts;
}

}  // namespace

namespace AgentModBisect {

int Bisect::maxLaunchesLimit() { return 20; }
int Bisect::maxRepeatLimit() { return 3; }
int Bisect::maxMinutesLimit() { return 45; }

QStringList Bisect::prerequisiteModIds() {
    return {QStringLiteral("minecraft"),
            QStringLiteral("forge"),
            QStringLiteral("neoforge"),
            QStringLiteral("fabricloader"),
            QStringLiteral("fabric-loader"),
            QStringLiteral("fabric"),
            QStringLiteral("fabric-api"),
            QStringLiteral("quilt_loader"),
            QStringLiteral("quilt-loader"),
            QStringLiteral("quilted_fabric_loader"),
            QStringLiteral("mixin"),
            QStringLiteral("mixinextras"),
            QStringLiteral("java")};
}

Bisect::Bisect(const QList<Mod> &mods, const Options &options)
    : m_mods(mods), m_options(options) {
    m_options.maxLaunches = qBound(1, options.maxLaunches, maxLaunchesLimit());
    m_options.repeatPerCandidate = qBound(1, options.repeatPerCandidate, maxRepeatLimit());
    m_options.maxMinutes = qBound(1, options.maxMinutes, 45);
    const QStringList prerequisites = prerequisiteModIds();
    for (int index = 0; index < m_mods.size(); ++index) {
        Mod &mod = m_mods[index];
        if (prerequisites.contains(mod.modId.toLower())) mod.prerequisite = true;
        m_indexByFile.insert(mod.fileName, index);
        if (!mod.initiallyEnabled) continue;
        m_initialEnabled.append(mod.fileName);
    }
    // 候选排序：优先项 → 最近修改 → 文件名。
    QList<Mod> candidates;
    for (const Mod &mod : m_mods) {
        if (mod.initiallyEnabled && !mod.prerequisite) candidates.append(mod);
    }
    QList<Mod> ordered;
    QSet<QString> orderedFiles;
    const auto takeMatching = [&ordered, &orderedFiles, &candidates](const QString &token) {
        for (const Mod &mod : candidates) {
            if (orderedFiles.contains(mod.fileName)) continue;
            if (mod.modId.compare(token, Qt::CaseInsensitive) == 0 ||
                mod.fileName.compare(token, Qt::CaseInsensitive) == 0) {
                ordered.append(mod);
                orderedFiles.insert(mod.fileName);
            }
        }
    };
    for (const QString &token : m_options.prioritizedCandidates) {
        const QString trimmed = token.trimmed();
        if (!trimmed.isEmpty()) takeMatching(trimmed);
    }
    QList<Mod> rest;
    for (const Mod &mod : candidates) {
        if (!orderedFiles.contains(mod.fileName)) rest.append(mod);
    }
    std::stable_sort(rest.begin(), rest.end(), [](const Mod &a, const Mod &b) {
        if (a.modifiedMs != b.modifiedMs) return a.modifiedMs > b.modifiedMs;
        return a.fileName < b.fileName;
    });
    ordered.append(rest);
    for (const Mod &mod : ordered) m_candidates.append(mod.fileName);
    for (const QString &file : m_candidates) {
        const Mod &mod = m_mods.at(m_indexByFile.value(file));
        for (const QString &token : m_options.prioritizedCandidates) {
            const QString trimmed = token.trimmed();
            if (trimmed.isEmpty()) continue;
            if (mod.modId.compare(trimmed, Qt::CaseInsensitive) == 0 ||
                mod.fileName.compare(trimmed, Qt::CaseInsensitive) == 0) {
                m_suspects.append(file);
                break;
            }
        }
    }

    m_current = Step{};
    m_current.kind = Step::Kind::Launch;
    m_current.enabledFiles = m_initialEnabled;
    m_current.purpose = QStringLiteral("baseline");
}

QStringList Bisect::candidateFiles() const { return m_candidates; }

QStringList Bisect::initialEnabledFiles() const { return m_initialEnabled; }

QStringList Bisect::configuration(const QStringList &files) const {
    QSet<QString> enabled;
    for (const Mod &mod : m_mods) {
        if (mod.initiallyEnabled && mod.prerequisite) enabled.insert(mod.fileName);
    }
    for (const QString &file : files) {
        if (m_indexByFile.contains(file)) enabled.insert(file);
    }
    if (m_options.respectDependencies) {
        bool changed = true;
        while (changed) {
            changed = false;
            const QList<QString> snapshot = enabled.values();
            for (const QString &file : snapshot) {
                const Mod &mod = m_mods.at(m_indexByFile.value(file));
                for (const QString &dependency : mod.requiredModIds) {
                    for (const Mod &provider : m_mods) {
                        if (!provider.initiallyEnabled) continue;
                        if (provider.modId.compare(dependency, Qt::CaseInsensitive) != 0) continue;
                        if (enabled.contains(provider.fileName)) continue;
                        enabled.insert(provider.fileName);
                        changed = true;
                    }
                }
            }
        }
    }
    QStringList out;
    for (const Mod &mod : m_mods) {
        if (enabled.contains(mod.fileName)) out.append(mod.fileName);
    }
    return out;
}

Step Bisect::step() const { return m_current; }

void Bisect::note(const QString &text) {
    if (text.isEmpty() || m_result.notes.contains(text)) return;
    m_result.notes.append(text);
}

void Bisect::beginReduce() {
    m_phase = Phase::Reduce;
    m_keep = m_candidates;
    m_granularity = 2;
    m_subsetIndex = 0;
    if (m_keep.isEmpty()) {
        finish(QStringLiteral("not_found"), {},
               {tr("没有可停用的候选模组：问题不来自模组集合")});
        return;
    }
    // 先试"只启用嫌疑模组"：命中就省掉大半缩减（S5.5 的优先步骤）。
    if (!m_suspects.isEmpty() && m_suspects.size() < m_keep.size()) {
        const QStringList trial = configuration(m_suspects);
        if (trial != configuration(m_keep)) {
            m_current = Step{};
            m_current.kind = Step::Kind::Launch;
            m_current.enabledFiles = trial;
            m_current.purpose = QStringLiteral("suspects");
            return;
        }
    }
    prepareNextReduction();
}

bool Bisect::prepareNextReduction() {
    while (true) {
        if (m_keep.isEmpty()) {
            finish(QStringLiteral("not_found"), {},
                   {tr("停用全部候选模组后仍能复现：问题在加载器/前置或非模组")});
            return false;
        }
        const int groups = qMin(m_granularity, int(m_keep.size()));
        const QList<QStringList> parts = split(m_keep, groups);
        if (m_subsetIndex >= parts.size()) {
            if (m_granularity >= m_keep.size()) {
                beginConfirm();
                return false;
            }
            m_granularity = qMin(int(m_keep.size()), m_granularity * 2);
            m_subsetIndex = 0;
            continue;
        }
        const QStringList part = parts.at(m_subsetIndex);
        const QStringList remaining = subtract(m_keep, part);
        const QStringList currentEnabled = configuration(m_keep);
        const QStringList trial = configuration(remaining);
        if (trial == currentEnabled) {
            // 这一组被依赖强制保留：实际什么都没停掉，换下一组。
            ++m_subsetIndex;
            continue;
        }
        m_current = Step{};
        m_current.kind = Step::Kind::Launch;
        m_current.enabledFiles = trial;
        m_current.purpose = QStringLiteral("reduce");
        m_trialRemaining = remaining;
        return true;
    }
}

void Bisect::beginConfirm() {
    m_phase = Phase::Confirm;
    m_confirmationLeft = m_options.repeatPerCandidate;
    m_anyCrash = false;
    m_anyNoCrash = false;
    m_current = Step{};
    m_current.kind = Step::Kind::Launch;
    m_current.enabledFiles = configuration(m_keep);
    m_current.purpose = QStringLiteral("confirm");
    m_current.confirmation = true;
}

void Bisect::finishForBudget() {
    if (m_phase == Phase::Done) return;
    m_result.budgetExhausted = true;
    note(tr("达到启动次数上限（%1），返回当前最小集合").arg(m_options.maxLaunches));
    if (m_phase == Phase::Baseline || m_keep.isEmpty()) {
        finish(m_phase == Phase::Baseline ? QStringLiteral("inconclusive")
                                          : QStringLiteral("not_found"),
               {});
        return;
    }
    finish(m_keep.size() == 1 ? QStringLiteral("single")
           : m_keep.size() == 2 ? QStringLiteral("pair")
                                : QStringLiteral("set"),
           configuration(m_keep));
}

/// 最小启用集合里，被别的成员依赖（required 闭包）的那些不是责任模组：
/// "app 需要 lib" 的场合 culprit 只有 app，lib 在 minimalSet 里但不背锅。
QStringList responsibility(const QList<Mod> &mods, const QHash<QString, int> &indexByFile,
                           const QStringList &minimalSet) {
    QSet<QString> dependencies;
    for (const QString &file : minimalSet) {
        const auto index = indexByFile.constFind(file);
        if (index == indexByFile.constEnd()) continue;
        QSet<QString> visited;
        QStringList stack = mods.at(index.value()).requiredModIds;
        while (!stack.isEmpty()) {
            const QString dependency = stack.takeFirst().toLower();
            for (const Mod &provider : mods) {
                if (provider.modId.toLower() != dependency) continue;
                if (!minimalSet.contains(provider.fileName)) continue;
                if (visited.contains(provider.fileName)) continue;
                visited.insert(provider.fileName);
                dependencies.insert(provider.fileName);
                stack.append(provider.requiredModIds);
            }
        }
    }
    QStringList culprits;
    for (const QString &file : minimalSet) {
        if (!dependencies.contains(file)) culprits.append(file);
    }
    if (culprits.isEmpty()) culprits = minimalSet;
    return culprits;
}

void Bisect::finish(const QString &result, const QStringList &minimalSet,
                    const QStringList &notes) {
    m_phase = Phase::Done;
    m_result.result = result;
    m_result.minimalSet = minimalSet;
    m_result.culprits.clear();
    // 责任模组只在候选里算：前置（fabric-api 之类）在 minimalSet 里但不背锅。
    QStringList candidateMinimal;
    for (const QString &file : minimalSet) {
        if (m_candidates.contains(file)) candidateMinimal.append(file);
    }
    const QStringList responsible = responsibility(m_mods, m_indexByFile, candidateMinimal);
    for (const QString &file : responsible) {
        const auto index = m_indexByFile.constFind(file);
        if (index == m_indexByFile.constEnd()) continue;
        const Mod &mod = m_mods.at(index.value());
        m_result.culprits.append(mod.modId.isEmpty() ? mod.fileName : mod.modId);
    }
    if (result == QStringLiteral("single") || result == QStringLiteral("pair") ||
        result == QStringLiteral("set")) {
        // 结果类型按**责任模组**算：依赖被闭包带进来的那几个不算。
        m_result.result = m_result.culprits.size() == 1   ? QStringLiteral("single")
                          : m_result.culprits.size() == 2 ? QStringLiteral("pair")
                                                          : QStringLiteral("set");
    }
    for (const QString &text : notes) note(text);
    m_current = Step{};
    m_current.kind = Step::Kind::Finished;
}

void Bisect::report(const LaunchOutcome &outcome, qint64 elapsedMs) {
    if (m_phase == Phase::Done) return;
    ++m_result.launches;
    if (elapsedMs > qint64(m_options.maxMinutes) * 60 * 1000) {
        m_result.budgetExhausted = true;
        note(tr("超过 %1 分钟的时间预算").arg(m_options.maxMinutes));
        if (m_phase == Phase::Baseline || m_keep.isEmpty()) {
            finish(m_phase == Phase::Baseline ? QStringLiteral("inconclusive")
                                              : QStringLiteral("not_found"),
                   {});
        } else {
            finish(m_keep.size() == 1 ? QStringLiteral("single")
                   : m_keep.size() == 2 ? QStringLiteral("pair")
                                        : QStringLiteral("set"),
                   configuration(m_keep));
        }
        return;
    }
    if (!outcome.completed) {
        ++m_consecutiveFailures;
        note(outcome.detail.isEmpty() ? tr("一次启动没有跑起来") : outcome.detail);
        if (m_consecutiveFailures >= 3) {
            finish(QStringLiteral("inconclusive"), {},
                   {tr("连续 3 次启动失败，停止二分")});
        }
        return;  // 同一步重试
    }
    m_consecutiveFailures = 0;

    if (m_phase == Phase::Baseline) {
        if (!outcome.crashed) {
            finish(QStringLiteral("not_reproducible"), {},
                   {tr("完整启用集启动后没有崩溃：这个问题不能靠二分定位")});
            return;
        }
        beginReduce();
        if (!finished() && m_result.launches >= m_options.maxLaunches) finishForBudget();
        return;
    }
    if (m_phase == Phase::Reduce) {
        if (m_current.purpose == QStringLiteral("suspects")) {
            m_keep = outcome.crashed ? m_suspects : m_candidates;
            m_granularity = 2;
            m_subsetIndex = 0;
            if (m_result.launches >= m_options.maxLaunches) {
                finishForBudget();
                return;
            }
            prepareNextReduction();
            return;
        }
        if (outcome.crashed) {
            m_keep = m_trialRemaining;
            m_granularity = 2;
            m_subsetIndex = 0;
        } else {
            ++m_subsetIndex;
        }
        if (m_result.launches >= m_options.maxLaunches) {
            finishForBudget();
            return;
        }
        prepareNextReduction();
        return;
    }
    // Confirm
    if (outcome.crashed) {
        m_anyCrash = true;
        --m_confirmationLeft;
        if (m_anyNoCrash) {
            // 确认阶段出现过一次不复现：最多加一轮，仍然不一致就判定不确定。
            if (!m_extraRepeatUsed) {
                m_extraRepeatUsed = true;
                m_result.flaky = true;
                m_confirmationLeft = 1;
                note(tr("确认时有一次没有复现，加跑一轮"));
                if (m_result.launches >= m_options.maxLaunches) finishForBudget();
                return;
            }
            finish(QStringLiteral("inconclusive"), {},
                   {tr("重复确认结果不一致（flaky），不给出责任模组")});
            return;
        }
        if (m_confirmationLeft > 0) {
            if (m_result.launches >= m_options.maxLaunches) {
                finishForBudget();
            }
            return;
        }
        const QString result = m_keep.size() == 1 ? QStringLiteral("single")
                              : m_keep.size() == 2 ? QStringLiteral("pair")
                                                   : QStringLiteral("set");
        if (m_keep.size() > 2) {
            note(tr("只在 %1 个模组同时启用时复现，没有缩到单个或两个").arg(m_keep.size()));
        }
        finish(result, configuration(m_keep));
        return;
    }
    // !crashed
    m_anyNoCrash = true;
    m_result.flaky = true;
    if (!m_extraRepeatUsed) {
        m_extraRepeatUsed = true;
        m_confirmationLeft = 1;
        note(tr("确认时没有复现，加跑一轮"));
        return;
    }
    finish(QStringLiteral("inconclusive"), {},
           {tr("重复确认结果不一致（flaky），不给出责任模组")});
}

}  // namespace AgentModBisect

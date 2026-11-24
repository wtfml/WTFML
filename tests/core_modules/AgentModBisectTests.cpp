/**
 * @file AgentModBisectTests.cpp
 * @brief S5.5 依赖感知二分：用脚本化启动结果驱动状态机（不需要真游戏）。
 */

#include "Support.hpp"

#include "agent/AgentModBisect.hpp"

#include <QHash>
#include <QSet>
#include <QStringList>

namespace {

AgentModBisect::Mod mod(const QString &fileName, const QString &modId,
                        const QStringList &required = {}) {
    AgentModBisect::Mod item;
    item.fileName = fileName;
    item.modId = modId;
    item.requiredModIds = required;
    item.initiallyEnabled = true;
    return item;
}

/// 按"崩溃判定"函数跑完整个状态机，记录每一轮启用了什么。
struct Drive {
    QList<QStringList> launched;
    QList<QString> purposes;
    int steps = 0;
};

Drive drive(AgentModBisect::Bisect *bisect,
            const std::function<bool(const QStringList &enabled)> &crashes,
            int maxSteps = 200) {
    Drive result;
    while (!bisect->finished() && result.steps < maxSteps) {
        const AgentModBisect::Step step = bisect->step();
        if (step.kind != AgentModBisect::Step::Kind::Launch) break;
        result.launched.append(step.enabledFiles);
        result.purposes.append(step.purpose);
        AgentModBisect::LaunchOutcome outcome;
        outcome.completed = true;
        outcome.crashed = crashes(step.enabledFiles);
        bisect->report(outcome);
        ++result.steps;
    }
    return result;
}

bool contains(const QStringList &list, const QString &value) {
    for (const QString &item : list) {
        if (item == value) return true;
    }
    return false;
}

}  // namespace

void runAgentModBisectTests() {
    // ---------------------------------------------------------------- 单个问题模组
    {
        QList<AgentModBisect::Mod> mods;
        for (int i = 0; i < 120; ++i) {
            mods.append(mod(QStringLiteral("mod-%1.jar").arg(i),
                            QStringLiteral("mod%1").arg(i)));
        }
        AgentModBisect::Options options;
        options.maxLaunches = 12;
        options.repeatPerCandidate = 2;
        options.prioritizedCandidates = QStringList{QStringLiteral("mod77")};
        AgentModBisect::Bisect bisect(mods, options);
        const Drive drive1 = drive(&bisect, [](const QStringList &enabled) {
            return contains(enabled, QStringLiteral("mod-77.jar"));
        });
        const AgentModBisect::Result result = bisect.result();
        check(result.result == QStringLiteral("single"), "二分：120 个模组里定位单个问题模组",
              result.result);
        check(result.minimalSet == QStringList{QStringLiteral("mod-77.jar")},
              "二分：最小集合就是那一个文件", result.minimalSet.join(QStringLiteral(",")));
        check(result.culprits == QStringList{QStringLiteral("mod77")},
              "二分：culprit 用 mod id", result.culprits.join(QStringLiteral(",")));
        check(result.launches <= 12, "二分：12 次启动以内定位",
              QString::number(result.launches));
        check(!result.budgetExhausted, "二分：没有用完预算");
        check(!result.flaky, "二分：结果稳定");
        check(drive1.launched.constFirst().size() == 120, "二分：基线跑的是完整启用集",
              QString::number(drive1.launched.constFirst().size()));
        check(drive1.purposes.contains(QStringLiteral("baseline")) &&
                  drive1.purposes.contains(QStringLiteral("reduce")) &&
                  drive1.purposes.contains(QStringLiteral("confirm")),
              "二分：走过基线/缩减/确认三段");

        // 没有嫌疑候选时仍要收敛，只是启动次数更多（ddmin 每层可能试两半）。
        AgentModBisect::Options plain;
        plain.maxLaunches = 20;
        AgentModBisect::Bisect plainBisect(mods, plain);
        drive(&plainBisect, [](const QStringList &enabled) {
            return contains(enabled, QStringLiteral("mod-77.jar"));
        });
        check(plainBisect.result().result == QStringLiteral("single") &&
                  plainBisect.result().minimalSet == QStringList{QStringLiteral("mod-77.jar")},
              "二分：没有嫌疑候选也能缩到单个",
              plainBisect.result().result);
    }

    // ---------------------------------------------------------------- 依赖保留
    {
        QList<AgentModBisect::Mod> mods;
        for (int i = 0; i < 16; ++i) {
            mods.append(mod(QStringLiteral("mod-%1.jar").arg(i),
                            QStringLiteral("mod%1").arg(i)));
        }
        mods.append(mod(QStringLiteral("app.jar"), QStringLiteral("app"),
                        {QStringLiteral("lib")}));
        mods.append(mod(QStringLiteral("lib.jar"), QStringLiteral("lib")));
        AgentModBisect::Options options;
        options.maxLaunches = 20;
        options.repeatPerCandidate = 2;
        options.prioritizedCandidates = QStringList{QStringLiteral("app")};
        AgentModBisect::Bisect bisect(mods, options);
        const Drive driven = drive(&bisect, [](const QStringList &enabled) {
            return contains(enabled, QStringLiteral("app.jar"));
        });
        const AgentModBisect::Result result = bisect.result();
        check(result.result == QStringLiteral("single"),
              "二分：依赖图的场景收敛到单个责任模组", result.result);
        check(result.culprits == QStringList{QStringLiteral("app")},
              "二分：被依赖的 lib 不背锅", result.culprits.join(QStringLiteral(",")));
        check(result.minimalSet.contains(QStringLiteral("lib.jar")),
              "二分：minimalSet 保留依赖",
              result.minimalSet.join(QStringLiteral(",")));
        bool dependencyAlwaysEnabled = true;
        bool appSeen = false;
        for (const QStringList &enabled : driven.launched) {
            if (contains(enabled, QStringLiteral("app.jar"))) {
                appSeen = true;
                if (!contains(enabled, QStringLiteral("lib.jar"))) {
                    dependencyAlwaysEnabled = false;
                }
            }
        }
        check(appSeen, "二分：有若干轮启用了 app");
        check(dependencyAlwaysEnabled, "二分：app 启用时它的前置 lib 始终启用");
        check(!driven.launched.isEmpty() &&
                  driven.launched.constFirst().contains(QStringLiteral("lib.jar")),
              "二分：基线之后的第一轮（只启用嫌疑）就带着依赖闭包");
    }

    // ---------------------------------------------------------------- 双模组组合
    {
        QList<AgentModBisect::Mod> mods;
        for (int i = 0; i < 24; ++i) {
            mods.append(mod(QStringLiteral("m%1.jar").arg(i), QStringLiteral("m%1").arg(i)));
        }
        mods.append(mod(QStringLiteral("pair-a.jar"), QStringLiteral("paira")));
        mods.append(mod(QStringLiteral("pair-b.jar"), QStringLiteral("pairb")));
        AgentModBisect::Options options;
        options.maxLaunches = 20;
        AgentModBisect::Bisect bisect(mods, options);
        drive(&bisect, [](const QStringList &enabled) {
            return contains(enabled, QStringLiteral("pair-a.jar")) &&
                   contains(enabled, QStringLiteral("pair-b.jar"));
        });
        const AgentModBisect::Result result = bisect.result();
        check(result.result == QStringLiteral("pair"), "二分：两个模组同时启用才崩报 pair",
              result.result);
        check(result.minimalSet.size() == 2 &&
                  result.minimalSet.contains(QStringLiteral("pair-a.jar")) &&
                  result.minimalSet.contains(QStringLiteral("pair-b.jar")),
              "二分：pair 的两个文件",
              result.minimalSet.join(QStringLiteral(",")));
    }

    // ---------------------------------------------------------------- 基线不复现
    {
        QList<AgentModBisect::Mod> mods{mod(QStringLiteral("a.jar"), QStringLiteral("a"))};
        AgentModBisect::Bisect bisect(mods, AgentModBisect::Options{});
        bool launchedOnce = false;
        drive(&bisect, [&launchedOnce](const QStringList &enabled) {
            Q_UNUSED(enabled);
            launchedOnce = true;
            return false;
        });
        const AgentModBisect::Result result = bisect.result();
        check(launchedOnce && result.result == QStringLiteral("not_reproducible"),
              "二分：基线不崩返回 not_reproducible", result.result);
        check(result.launches == 1, "二分：不崩时只跑一次基线",
              QString::number(result.launches));
        check(result.minimalSet.isEmpty(), "二分：not_reproducible 没有 culprit");
    }

    // ---------------------------------------------------------------- 停用全部候选仍崩
    {
        QList<AgentModBisect::Mod> mods{mod(QStringLiteral("a.jar"), QStringLiteral("a")),
                                        mod(QStringLiteral("b.jar"), QStringLiteral("b"))};
        AgentModBisect::Bisect bisect(mods, AgentModBisect::Options{});
        drive(&bisect, [](const QStringList &enabled) {
            Q_UNUSED(enabled);
            return true;  // 什么都崩：问题不在候选集合
        });
        const AgentModBisect::Result result = bisect.result();
        check(result.result == QStringLiteral("not_found"),
              "二分：停用全部候选仍崩报 not_found", result.result);
        check(result.minimalSet.isEmpty(), "二分：not_found 没有 culprit");
    }

    // ---------------------------------------------------------------- 前置始终启用
    {
        QList<AgentModBisect::Mod> mods{mod(QStringLiteral("fabric-api.jar"),
                                            QStringLiteral("fabric-api")),
                                        mod(QStringLiteral("a.jar"), QStringLiteral("a")),
                                        mod(QStringLiteral("b.jar"), QStringLiteral("b"))};
        AgentModBisect::Bisect bisect(mods, AgentModBisect::Options{});
        const Drive driven = drive(&bisect, [](const QStringList &enabled) {
            return contains(enabled, QStringLiteral("a.jar"));
        });
        bool prerequisiteKept = true;
        for (const QStringList &enabled : driven.launched) {
            if (!contains(enabled, QStringLiteral("fabric-api.jar"))) prerequisiteKept = false;
        }
        check(prerequisiteKept, "二分：加载器/API 前置始终启用");
        check(bisect.result().result == QStringLiteral("single"),
              "二分：前置不会被当成责任模组", bisect.result().result);
        check(!bisect.candidateFiles().contains(QStringLiteral("fabric-api.jar")),
              "二分：前置不在候选集合里");
    }

    // ---------------------------------------------------------------- 预算用完
    {
        QList<AgentModBisect::Mod> mods;
        for (int i = 0; i < 120; ++i) {
            mods.append(mod(QStringLiteral("m%1.jar").arg(i), QStringLiteral("m%1").arg(i)));
        }
        AgentModBisect::Options options;
        options.maxLaunches = 3;
        options.repeatPerCandidate = 2;
        AgentModBisect::Bisect bisect(mods, options);
        drive(&bisect, [](const QStringList &enabled) {
            return contains(enabled, QStringLiteral("m99.jar"));
        });
        const AgentModBisect::Result result = bisect.result();
        check(result.launches <= 3, "二分：启动次数不超过预算",
              QString::number(result.launches));
        check(result.result == QStringLiteral("set") ||
                  result.result == QStringLiteral("single") ||
                  result.result == QStringLiteral("pair"),
              "二分：预算用完时返回当前最小集合", result.result);
        check(!result.minimalSet.isEmpty(), "二分：预算用完也给出当前集合");
        check(result.budgetExhausted, "二分：标出预算用完");
    }

    // ---------------------------------------------------------------- 基础设施失败
    {
        QList<AgentModBisect::Mod> mods{mod(QStringLiteral("a.jar"), QStringLiteral("a"))};
        AgentModBisect::Bisect bisect(mods, AgentModBisect::Options{});
        int attempts = 0;
        while (!bisect.finished() && attempts < 10) {
            const AgentModBisect::Step step = bisect.step();
            if (step.kind != AgentModBisect::Step::Kind::Launch) break;
            AgentModBisect::LaunchOutcome outcome;
            outcome.completed = false;
            outcome.detail = QStringLiteral("进程没起来");
            bisect.report(outcome);
            ++attempts;
        }
        check(attempts == 3, "二分：连续 3 次启动失败就停", QString::number(attempts));
        check(bisect.result().result == QStringLiteral("inconclusive"),
              "二分：基础设施失败回 inconclusive", bisect.result().result);
    }

    // ---------------------------------------------------------------- 不一致
    {
        QList<AgentModBisect::Mod> mods{mod(QStringLiteral("a.jar"), QStringLiteral("a"))};
        AgentModBisect::Bisect bisect(mods, AgentModBisect::Options{});
        int confirmations = 0;
        while (!bisect.finished()) {
            const AgentModBisect::Step step = bisect.step();
            if (step.kind != AgentModBisect::Step::Kind::Launch) break;
            AgentModBisect::LaunchOutcome outcome;
            outcome.completed = true;
            const bool enabledA = contains(step.enabledFiles, QStringLiteral("a.jar"));
            if (step.confirmation) {
                ++confirmations;
                outcome.crashed = confirmations == 1 && enabledA;  // 确认时第二次不复现
            } else {
                outcome.crashed = enabledA;
            }
            bisect.report(outcome);
        }
        const AgentModBisect::Result result = bisect.result();
        check(result.flaky, "二分：确认阶段不一致标 flaky");
        check(result.result == QStringLiteral("inconclusive"),
              "二分：加跑一轮仍不一致回 inconclusive", result.result);
        check(result.launches >= 4, "二分：不一致时多跑了一轮",
              QString::number(result.launches));
    }

    // ---------------------------------------------------------------- 不保留依赖
    {
        QList<AgentModBisect::Mod> mods{mod(QStringLiteral("app.jar"), QStringLiteral("app"),
                                            {QStringLiteral("lib")}),
                                        mod(QStringLiteral("lib.jar"), QStringLiteral("lib")),
                                        mod(QStringLiteral("other.jar"), QStringLiteral("other"))};
        AgentModBisect::Options options;
        options.respectDependencies = false;
        AgentModBisect::Bisect bisect(mods, options);
        const Drive driven = drive(&bisect, [](const QStringList &enabled) {
            // 只有 lib 在的时候才崩：respectDependencies=false 时应当能定位到 lib。
            return contains(enabled, QStringLiteral("lib.jar"));
        });
        bool sawDependencyAlone = false;
        for (const QStringList &enabled : driven.launched) {
            if (contains(enabled, QStringLiteral("lib.jar")) &&
                !contains(enabled, QStringLiteral("app.jar"))) {
                sawDependencyAlone = true;
            }
        }
        check(sawDependencyAlone, "二分：respectDependencies=false 时允许停掉依赖它的模组");
        check(bisect.result().result == QStringLiteral("single") &&
                  bisect.result().minimalSet.contains(QStringLiteral("lib.jar")),
              "二分：不保留依赖时定位到前置本身", bisect.result().result);
    }

    // ---------------------------------------------------------------- 优先候选
    {
        QList<AgentModBisect::Mod> mods;
        for (int i = 0; i < 8; ++i) {
            mods.append(mod(QStringLiteral("m%1.jar").arg(i), QStringLiteral("m%1").arg(i)));
        }
        AgentModBisect::Options options;
        options.prioritizedCandidates = QStringList{QStringLiteral("m5")};
        AgentModBisect::Bisect bisect(mods, options);
        check(bisect.candidateFiles().constFirst() == QStringLiteral("m5.jar"),
              "二分：优先候选排在最前面",
              bisect.candidateFiles().constFirst());
    }
}

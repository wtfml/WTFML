/**
 * @file AgentRepairCaseTests.cpp
 * @brief 本地修复病例库（P2）：默认关闭、只存结构化事实、合并、本地排序、结果更新。
 */

#include "Support.hpp"

#include "agent/AgentRepairCases.hpp"
#include "agent/AgentSettings.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

void runAgentRepairCaseTests() {
    QTemporaryDir fixture;
    AgentRepairCase::Store store(fixture.path() + QStringLiteral("/cases"));

    // ---------- 默认关闭：不落盘、不参与排序 ----------
    AgentSettings::setRepairCasesEnabled(false);
    check(!store.enabled(), "病例库：默认关闭");
    AgentRepairCase::Case off;
    off.signature = QStringLiteral("crash:java.lang.OutOfMemoryError|rule:heap");
    off.actions = {QStringLiteral("setting:xmx")};
    off.outcome = QStringLiteral("verified");
    off.verified = true;
    QString problem = QStringLiteral("stale");
    check(store.record(off, &problem) && problem == QStringLiteral("stale"),
          "病例库：关闭时 record 是空转");
    check(!QDir(store.root()).exists(), "病例库：关闭时不建目录");
    check(store.ranked(off.signature, 3).isEmpty(), "病例库：关闭时排序也给空");

    // ---------- 打开后：落盘 + 读回 + 排序 ----------
    AgentSettings::setRepairCasesEnabled(true);
    check(store.enabled(), "病例库：设置能打开");

    AgentRepairCase::Case good;
    good.signature = QStringLiteral("crash:java.lang.OutOfMemoryError|phase:load|rule:heap");
    good.actions = {QStringLiteral("setting:xmx"), QStringLiteral("scope:config")};
    good.files = {QStringLiteral("config/sodium-options.json")};
    good.outcome = QStringLiteral("verified");
    good.verified = true;
    check(store.record(good, &problem), "病例库：落盘成功", problem);
    const QList<AgentRepairCase::Case> all = store.all();
    check(all.size() == 1, "病例库：读回一条");
    check(all.size() == 1 && all.constFirst().signature == good.signature &&
              all.constFirst().actions.contains(QStringLiteral("setting:xmx")),
          "病例库：签名与动作读回一致");

    // 同签名同动作再记一次失败：合并成一条，尝试 2 次成功 1 次。
    AgentRepairCase::Case again = good;
    again.outcome = QStringLiteral("not_verified");
    again.verified = false;
    check(store.record(again, &problem), "病例库：同键合并不报错", problem);
    QList<AgentRepairCase::Entry> ranked = store.ranked(good.signature, 3);
    check(ranked.size() == 1, "病例库：同签名同动作合并不新增条目");
    check(ranked.size() == 1 && ranked.constFirst().item.attempts == 2 &&
              ranked.constFirst().item.successes == 1,
          "病例库：合并后计数是 2 次尝试 / 1 次成功",
          ranked.isEmpty()
              ? QStringLiteral("empty")
              : QStringLiteral("attempts=%1 successes=%2 outcome=%3")
                    .arg(ranked.constFirst().item.attempts)
                    .arg(ranked.constFirst().item.successes)
                    .arg(ranked.constFirst().item.outcome));

    // 同签名不同动作：单独一条，成功率更高排前面。
    AgentRepairCase::Case better;
    better.signature = good.signature;
    better.actions = {QStringLiteral("file:disabled:mods")};
    better.outcome = QStringLiteral("verified");
    better.verified = true;
    check(store.record(better, &problem), "病例库：第二个动作组合落盘", problem);
    AgentRepairCase::Case betterAgain = better;
    betterAgain.outcome = QStringLiteral("verified");
    betterAgain.verified = true;
    check(store.record(betterAgain, &problem), "病例库：第二个动作组合再记一次", problem);
    ranked = store.ranked(good.signature, 3);
    check(ranked.size() == 2, "病例库：同签名两条动作组合");
    check(ranked.size() == 2 &&
              ranked.at(0).item.actions.contains(QStringLiteral("file:disabled:mods")),
          "病例库：本地排序把 2/2 排在 1/2 前面");
    check(ranked.size() == 2 && ranked.at(0).score > ranked.at(1).score,
          "病例库：排序分数确实递减");

    // 结果更新不改尝试次数。
    check(store.markOutcome(good.signature,
                            {QStringLiteral("setting:xmx"), QStringLiteral("scope:config")},
                            QStringLiteral("committed"), &problem),
          "病例库：更新结果", problem);
    ranked = store.ranked(good.signature, 3);
    bool committed = false;
    int attemptsAfter = 0;
    for (const AgentRepairCase::Entry &entry : ranked) {
        if (entry.item.actions.contains(QStringLiteral("setting:xmx"))) {
            committed = entry.item.outcome == QStringLiteral("committed");
            attemptsAfter = entry.item.attempts;
        }
    }
    check(committed && attemptsAfter == 2, "病例库：更新结果是改 outcome，不加尝试次数");

    // ---------- 进模型上下文的那一份不含路径明细 ----------
    const QJsonObject projected = ranked.constFirst().toModelJson();
    check(!projected.contains(QStringLiteral("files")), "病例库：模型看不到路径明细");
    check(projected.value(QStringLiteral("actions")).toArray().size() > 0,
          "病例库：模型看得到动作标签");

    // ---------- 键与上限 ----------
    const QString crashKey = AgentRepairCase::crashSignature(
        QStringLiteral("java.lang.NoSuchMethodError"), QStringLiteral("mixin"),
        {QStringLiteral("mixin-apply-failed"), QStringLiteral("mod-dependency")});
    check(crashKey.contains(QStringLiteral("crash:java.lang.NoSuchMethodError")) &&
              crashKey.contains(QStringLiteral("phase:mixin")) &&
              crashKey.contains(QStringLiteral("rule:mixin-apply-failed")),
          "病例库：症状键由结构字段拼出来", crashKey);
    check(AgentRepairCase::validSignature(crashKey), "病例库：症状键形状合法", crashKey);
    check(AgentRepairCase::crashSignature(QString(), QString(), {}).isEmpty(),
          "病例库：什么都没有时不给键");
    check(AgentRepairCase::repairSignature({QStringLiteral("mods"), QStringLiteral("config")}) ==
              QStringLiteral("repair:config+mods"),
          "病例库：兜底键按范围排序");
    check(!AgentRepairCase::validSignature(QStringLiteral("crash:路径")),
          "病例库：中文/空格等症状键不允许");
    check(!AgentRepairCase::validSignature(
              QString(AgentRepairCase::maxSignatureChars() + 1, QLatin1Char('a'))),
          "病例库：超长症状键不允许");

    AgentRepairCase::Case badKey;
    badKey.signature = QStringLiteral("坏 键");
    check(!store.record(badKey, &problem) && problem.contains(QStringLiteral("症状键")),
          "病例库：坏键拒绝落盘", problem);

    AgentRepairCase::Case manyActions;
    manyActions.signature = QStringLiteral("crash:java.lang.Error|rule:generic");
    for (int i = 0; i < AgentRepairCase::maxActions() + 6; ++i) {
        manyActions.actions.append(QStringLiteral("file:changed:mods-%1").arg(i));
    }
    check(store.record(manyActions, &problem), "病例库：动作很多的条目也能落盘", problem);
    ranked = store.ranked(manyActions.signature, 3);
    check(ranked.size() == 1 &&
              ranked.constFirst().item.actions.size() <= AgentRepairCase::maxActions(),
          "病例库：动作标签有上限");

    // ---------- 上限与清理 ----------
    AgentRepairCase::Case recency;
    recency.signature = QStringLiteral("crash:java.lang.Error|rule:recent");
    recency.actions = {QStringLiteral("scope:mods")};
    recency.updatedMs = QDateTime::currentMSecsSinceEpoch();
    AgentRepairCase::Case old = recency;
    old.signature = QStringLiteral("crash:java.lang.Error|rule:old");
    old.updatedMs = 0;
    const double oldScore = AgentRepairCase::rankScore(old, recency.updatedMs);
    const double newScore = AgentRepairCase::rankScore(recency, recency.updatedMs);
    check(newScore > oldScore, "病例库：最近用过的分数更高");
    AgentRepairCase::Case won = recency;
    won.attempts = 1;
    won.successes = 1;
    const double wonScore = AgentRepairCase::rankScore(won, recency.updatedMs);
    check(wonScore > 0.5 && wonScore < 1.0, "病例库：成功率底数在 0–1 之间",
          QString::number(wonScore));

    check(store.clear(&problem) && !QDir(store.root()).exists(), "病例库：清理删掉目录", problem);
    AgentSettings::setRepairCasesEnabled(false);
}

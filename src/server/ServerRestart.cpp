/**
 * @file ServerRestart.cpp
 * @brief ServerRestart.hpp 的实现（纯函数，不读盘、不看时钟）。
 */

#include "server/ServerRestart.hpp"
#include <QCoreApplication>

#include <QtGlobal>

namespace ServerRestart {

Why whyFor(bool userStopped, bool forceKilled, int exitCode) {
    // 用户停的（含强杀）优先：被强杀的服务端退出码往往不是 0，但它不是「崩了」。
    if (userStopped || forceKilled) {
        return Why::UserStop;
    }
    return exitCode == 0 ? Why::CleanExit : Why::Crash;
}

bool isAbnormal(Why why) {
    return why == Why::Crash;
}

QString whyName(Why why) {
    switch (why) {
    case Why::UserStop:
        return QCoreApplication::translate("ServerRestart", "你自己停的");
    case Why::CleanExit:
        return QCoreApplication::translate("ServerRestart", "正常退出");
    case Why::Crash:
        return QCoreApplication::translate("ServerRestart", "异常退出");
    }
    return QString();
}

int normalizeMaxAttempts(int value) {
    if (value < kMinMaxAttempts || value > kMaxMaxAttempts) {
        return kDefaultMaxAttempts;
    }
    return value;
}

Policy normalize(const Policy &policy) {
    Policy out = policy;
    out.maxAttempts = normalizeMaxAttempts(policy.maxAttempts);
    return out;
}

int delaySeconds(int attempt) {
    if (attempt <= 1) return kBaseDelaySeconds;
    int delay = kBaseDelaySeconds;
    for (int i = 2; i <= attempt && delay < kMaxDelaySeconds; ++i) {
        delay *= 2;
    }
    return qMin(delay, kMaxDelaySeconds);
}

Decision decide(const Policy &policy, Why why, int attemptsUsed) {
    Decision decision;
    if (why != Why::Crash) {
        return decision;
    }
    const Policy normalized = normalize(policy);
    if (!normalized.enabled) {
        return decision;
    }
    if (attemptsUsed >= normalized.maxAttempts) {
        decision.note = QCoreApplication::translate("ServerRestart", "连续自动重启 %1 次均失败，已停止自动重启。")
                            .arg(normalized.maxAttempts);
        return decision;
    }
    decision.restart = true;
    decision.delaySeconds = delaySeconds(attemptsUsed + 1);
    decision.note = QCoreApplication::translate("ServerRestart", "异常退出，%1 秒后自动重启（第 %2 次，最多连着 %3 次）。")
                        .arg(QString::number(decision.delaySeconds),
                             QString::number(attemptsUsed + 1),
                             QString::number(normalized.maxAttempts));
    return decision;
}

}  // namespace ServerRestart

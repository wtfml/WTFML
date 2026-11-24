/**
 * @file GameExitDiagnosis.hpp
 * @brief 游戏进程异常退出之后那几句话怎么得出来。
 *
 * 【为什么单独一个文件】
 *
 * 要求的是「仅做规则明确的提示…不做不透明的『AI 猜错』」。规则明确 = **每一条提示都能
 * 指着上面某个字段说"因为它这样，所以才说这句"**。把这件事放成一个纯函数（值进、字符串出、
 * 不看时钟、不读盘、不开进程）之后，那张表就能在验证工程里逐格念一遍 —— 而写在
 * `MainWindow::onGameFinished()` 里的话，谁也量不出来（那一段要一整个主窗、要真的起过游戏）。
 *
 * 【它不知道"日志窗"是什么】
 *
 * 那些 `errorLineCount` / `sawOutOfMemory` 由调用方从日志窗那份模型里数好、看一眼递进来。
 * 这里**不去读日志文本**：一来读不到（会话结束时窗可能已经没了，见 `GameSessionController::GameLogFacts` 那段），
 * 二来"哪些行算错误"只有日志窗那边一个出处（见 `util/ErrorReport.hpp` 的文件头）。
 *
 * 【它也不认识任务账本】
 *
 * 回出去的是两句普通的话：一句给账本的 `errorSummary`（任务详情页上「出了什么问题」那一块），
 * 一串给验证工程和以后的界面用。要不要记成一项任务、记成什么状态，是 `MainWindow` 的事。
 */

#pragma once

#include <QString>
#include <QStringList>

namespace GameExitDiagnosis {

/// 异常退出时留多少行 ERROR/Fatal（只留最后 200 行）。
///
/// 这个数是**唯一出处**：`MainWindow` 按它截、报告里那句"只留了末尾 N 行"也按它写。
constexpr int kExcerptLineLimit = 200;

constexpr int kLogTailLineLimit = 500;

/**
 * @brief 一次游戏进程退出时手上有的**事实**。
 *
 * 全部由调用方从它自己那边取（`MainWindow` 在启动那一刻存了一份会话事实，退出时补齐退出码）。
 * **每一项都可以是"不知道"**：0 / 空串 / false 一律按"不知道"处理 —— 这一层绝不去猜一个
 * 缺掉的值（那正是要避免的"不透明的猜错"）。
 */
struct Facts {
    /// 进程退出码。**Windows 上崩溃给的是 NTSTATUS**（高位是 0xC0），所以它是负的。
    int exitCode = 0;
    /// `QProcess::ExitStatus == CrashExit`。
    bool crashed = false;

    /// 这个实例的 version.json 声明的 Java 主版本（0 = 不知道）。
    int requiredJavaMajor = 0;
    /// 这一趟**实际用的**那个 Java 的主版本（0 = 不知道，比如 release 文件读不到）。
    int usedJavaMajor = 0;
    /// 实际用的那个 Java 可执行文件（空 = 不知道）。
    QString javaPath;

    /// 给 JVM 的 `-Xmx` / `-Xms`（MB，0 = 不知道）。
    int xmxMb = 0;
    int xmsMb = 0;
    /// 这台机器的物理内存总量（MB，0 = 不知道）。
    qint64 totalMemoryMb = 0;

    /// 这个实例的显示名（用户认得出的那个）。
    QString instanceName;
    /// 实例 id（= `versions/` 下面那个目录名）。
    QString instanceId;
    /// 模组加载器的显示名（"Fabric" / "Forge" / "NeoForge"…）。**空 = 原版或者不知道**。
    QString loaderName;
    /// 加载器版本（"0.15.11"）。可空。
    QString loaderVersion;

    /// 这一趟收到的 ERROR/Fatal 那几档有多少行（0 = 一行都没有）。
    int errorLineCount = 0;
    /// 那些行里**有没有** `OutOfMemoryError`（`logShowsOutOfMemory()` 认出来的）。
    bool sawOutOfMemory = false;
    /// 那些行里**有没有**"某个模组的前置没装"（`logShowsMissingDependency()` 认出来的）。
    ///
    /// NOTE: 这是**加载器自己说的那几句话**，不是我们按模组清单猜的 —— 退出这一刻去联网问
    /// 一遍每个模组的前置，用户只会看到启动器卡在那里（见 `hints()` 里那段）。
    bool sawMissingDependency = false;
    /// 这一趟到底有没有拿到游戏的输出（`显示游戏日志` 那个开关关着时是 false）。
    bool logCaptured = false;
};

/// 这次算不算"异常退出"：崩溃，或者非 0 退出码。以 0 正常退出的不算。
bool isAbnormal(const Facts &facts);

/// 同上，手上有那两个值、还没攒成一份 `Facts` 的时候用（`onGameFinished()` 那一处就是：
/// 那一刻手上只有退出码和 `QProcess::ExitStatus`）。**判据只有这一处**，上面那个重载
/// 调的就是它。
inline bool isAbnormal(int exitCode, bool crashed) {
    return crashed || exitCode != 0;
}

/// 日志里有没有"某个模组的前置没装"的迹象。
///
/// 认的是三个加载器**各自那句原文**（Forge / NeoForge 的
/// `Missing or unsupported mandatory dependencies`、Fabric 的 `requires ... which is missing`）。
/// 放在这里而不是让调用方去 `contains()` 一个写死的词：那一句话的形状要能跟着一起被断言钉住。
bool logShowsMissingDependency(const QString &logText);

/// 日志里有没有 `OutOfMemoryError`。
bool logShowsOutOfMemory(const QString &logText);

/// 退出码写成一句人话：`0xC0000005（内存访问冲突）`。认不出的原样写十进制。
///
/// NOTE: Windows 上 `QProcess` 报的崩溃码就是 NTSTATUS（不是进程的 exit code），所以
/// 那几个高位 0xC0 的值能指名道姓地说出"哪种崩"。认不出的**不编**，照原样写。
QString exitDescription(int exitCode);

/// 一句话概括这次退出（写进账本的 `errorSummary`）。
QString summary(const Facts &facts);

/**
 * @brief 规则明确的提示，逐条。
 *
 * **每一条都从上面某个字段直接推出来**，凑不出任何一条就回一个空表（那说明这次退出
 * 我们手上没有能指认的东西 —— 空着比说一句"可能有很多原因"强）。
 *
 * NOTE: 这个函数**不看时钟、不读盘**，所以同一份 Facts 永远给同一串话 —— 验证工程按它
 * 钉那张表。
 */
QStringList hints(const Facts &facts);

}  // namespace GameExitDiagnosis

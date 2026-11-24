/**
 * @file ServerRun.hpp
 * @brief 「把一个已经装好的服务端跑起来、再让它干净地停下来」这条路上的**判据层**
 * （操作那一段 + 停止时序）。
 *
 * 【它和 `ServerModel` / `ServerInstall` 差在哪】
 *
 * * `ServerModel` 说"一个服务端是什么"（状态那七个词、`eula.txt` 怎么读怎么写、
 * `server.properties`、`isInstalled()`）；
 * * `ServerInstall` 说"要下什么、装完是什么样"，并给出**装完之后启动它发什么**
 * （`Plan::launchTokens`）；
 * * 这一层说**那串 token 怎么变成一条真命令**（java 在哪、内存那几项插到第几位）、
 * **怎么让它停**（先 `stop`、等多久才允许强杀）、以及**它吐出来的那些字节在说什么**
 * （起来了没有、退了之后算哪一档状态）。
 *
 * 和 `ServerInstallRun.hpp` 同一个形状：**header-only、全 `inline`、不联网、不起进程、
 * 不看时钟** —— 喂几个数进来、回一个结论出去。所以 `verify` 能直接把它整片钉死
 * （见 tests 的 **ZZ** 一节），真的 `QProcess` / `QTimer` 留给 `ServerProcess`。
 *
 * 【★ 内存那几项插在哪儿】
 *
 * 不能接在最后：
 *
 * java @user_jvm_args.txt @libraries/…/win_args.txt nogui -Xmx4G
 * ^^^^^^^^^^^^^^^^ 这一串是**主类 + 它的参数**
 *
 * —— `win_args.txt` 不是"一堆 JVM 参数"，它**自带主类**（`--launchTarget forgeserver`
 * 前面那个 `net.minecraftforge.bootstrap.BootstrapLauncher` 就是）。java 从左往右认，
 * **第一个不是选项、也不是 `-jar` 目标的东西之后，全都算程序参数** ⇒ 接在最后那几项会被
 * 当成"给服务端的参数"，`-Xmx` 一个字都不生效（而那台服务端只会回一句
 * `Unrecognized option` 之类，或者干脆照默认内存跑）。
 *
 * 正确的位置是**两张参数文件之间**：排在 `@user_jvm_args.txt` 之后（那张文件里可能也写着
 * `-Xmx`，同一个参数以最后出现的为准 —— 接在前面会被它盖掉，界面上那根内存滑杆成了摆设），
 * 又排在 `@win_args.txt` **之前**（那之后就是主类了）。所以那个"第几位"不能靠"接哪儿"这句
 * 话表达，`ServerInstall::Plan` 上有一格 `launchJvmArgsIndex`（默认 0 = 接在最前面，
 * Forge / NeoForge 是 1）。
 *
 * 【停下来：宽限时间是**算出来的**，不是拍的】
 *
 * `stop` 发出去之后服务端要存盘、要等各个 mod 收尾，大世界 + 几百个 mod 的服可能几十秒才
 * 退。这段时间里把「强制结束」放出来只会诱导用户在存盘中途把它打死（那正是丢存档的姿势）。
 * 用户 点名：「基于模组数量和存档大小灵活调整」—— 见 `gracefulStopMs()`：
 *
 * 起手 15 s，每 32 个模组 +5 s（封顶 +45 s）、每 1 GiB 存档 +5 s（封顶 +60 s），
 * 总上限 120 s。
 *
 * 三个数都由 `MainWindow` 现算现喂（模组 = `<目录>/mods` 下 `.jar` 的个数，
 * `.jar.disabled` 不算；存档 = `<目录>/world` 那棵树的大小），所以这一层不知道盘长什么样。
 */

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "download/ServerInstall.hpp"
#include "server/ServerModel.hpp"

namespace ServerRun {

// ---------------------------------------------------------------------------
// 拼那条启动命令
// ---------------------------------------------------------------------------

/**
 * @brief 装好之后真正发给 `QProcess` 的那串参数（**不含**工作目录 —— 那是服务端目录）。
 *
 * @param plan 那一档的安装计划（要的是 `launchTokens` 和 `launchJvmArgsIndex`）。
 * @param memoryMb 内存上限，MB。**`<= 0` 就一项都不发**（"不限制"是 java 的默认档；
 * 编一个数出来比不发更危险 —— 同实例设置那条规则）。
 * @param extraJvmArgs 别的 JVM 参数（排在内存那几项后面）。空 = 不发。
 *
 * 返回值**不含** `java` 自己（那是 `QProcess::setProgram()`）；调用方拼的时候别把
 * `plan.launchTokens` 里的东西再抄一遍。
 */
inline QStringList launchArguments(const ServerInstall::Plan &plan, int memoryMb,
                                   const QStringList &extraJvmArgs = QStringList()) {
    QStringList jvmArgs;
    if (memoryMb > 0) {
        jvmArgs.append(QStringLiteral("-Xmx%1m").arg(memoryMb));
    }
    jvmArgs.append(extraJvmArgs);

    // 插到计划说的那一位（见文件头那段：Forge / NeoForge 必须夹在两张参数文件中间）。
    const int at = qBound(0, plan.launchJvmArgsIndex, plan.launchTokens.size());
    QStringList out = plan.launchTokens;
    for (int i = 0; i < jvmArgs.size(); ++i) {
        out.insert(at + i, jvmArgs.at(i));
    }
    return out;
}

/// 让它正常停下来发的那一行。**换行是必须的**（`stop` 后面不回车，服务端读到 EOF 之前
/// 一直在等下一行）。这是启动器唯一会主动写进它 stdin 的一句话。
inline QByteArray stopCommandBytes() {
    return QByteArrayLiteral("stop\n");
}

// ---------------------------------------------------------------------------
// 「起来了吗 / 退成什么样」
// ---------------------------------------------------------------------------

/**
 * @brief 这一行输出是不是"服务端起来了"那一句。
 *
 * 服务端起来时打的是 `Done (12.345s)! For help, type "help"`（原版 / Paper / Fabric /
 * Forge / NeoForge 都这一句）。★ 服务端**没有**"窗口出现了"那种信号 —— 进程一 create 出来
 * 就算"启动中"，只有这一行才是"真的在跑了"（`Status::Running`）。
 *
 * NOTE: 判据是**完整的那一段** `Done (`，不是 `Done`：mod 的日志里出现一个光秃秃的 Done
 * 太容易了（"Done loading 512 recipes" 这种一眼就是别的意思），而带左括号那一截
 * 是服务端自己那台 `Server start-up complete` 才有的形状。
 */
inline bool isReadyLine(const QString &line) {
    return line.contains(QLatin1String("Done ("));
}

// ---------------------------------------------------------------------------
// 「这一堆字节里有几整行了」
// ---------------------------------------------------------------------------

/**
 * @brief 把新到手的一截**字节**拼到 `pending` 上，切出里面**所有完整的行**，剩下的留在
 * `pending` 里等下一趟。
 *
 * 这一层为什么必须按**字节**切、切完再解码：`QProcess::readAllStandardOutput()` 到手的
 * 是**字节**，而一次 `readyRead` 只保证"有东西来了"，**不保证切在字符边界上**。
 * `QString::fromLocal8Bit(readAllStandardOutput())`（到手就整块解码）遇到一个三字节的
 * 汉字正好被两次 `readyRead` 从中间劈开时，前后各解出半个字符 —— 拼回来是两个替换字符/
 * 乱码，于是"回声回来了没有"这条判断就时红时绿（管道怎么分包看系统心情；纯 ASCII 那几条
 * 看不出来）。
 *
 * 先按字节切行就绕开了这件事：`\n`(0x0A) 和 `\r`(0x0D) 在 UTF-8 里**只可能是行尾**，
 * 多字节字符的后续字节一律 ≥ 0x80 ⇒ 按这两个字节切永远不会把字符切开，切出来的每一段
 * 都是完整的字节序列、可以放心逐行解码。
 *
 * 空行**不回来** —— 服务端自己会打空行，
 * 而且 `\r\n`（Windows 那套换行）会被切成"前一段 + 一个空段"，把空段也当一行报上去
 * 会让行数凭空多出来。这个行为是**故意的**。
 *
 * @param pending 跨趟攒着的那截（进来时是上一趟剩下的，返回时是这一趟剩下的）。
 * @param chunk 刚 `readAllStandardOutput()` 到手的一截（**不要先解码**）。
 * @return 这次能凑齐的那些行（已解码，按顺序）。
 */
inline QStringList takeLines(QByteArray &pending, const QByteArray &chunk) {
    pending += chunk;
    QStringList lines;
    int cut = 0;
    while (cut < pending.size()) {
        const char ch = pending.at(cut);
        if (ch != '\n' && ch != '\r') {
            ++cut;
            continue;
        }
        const QByteArray raw = pending.left(cut);
        pending.remove(0, cut + 1);
        if (!raw.isEmpty()) {
            lines.append(QString::fromLocal8Bit(raw));
        }
        cut = 0;
    }
    return lines;
}

/**
 * @brief 进程退了之后是哪一档状态。
 *
 * - 退出码 0 ⇒ `Stopped`（正常退出 —— 不管是我们发的 `stop` 还是它自己 `/stop`）；
 * - 非 0 ⇒ `Crashed`；
 * - ★ 我们还**没来得及**说"要停"它就 0 退出了，也算 `Stopped`：那种场面是"它在启动过程中
 * 自己关掉了"（比如 eula 没同意、端口被占，服务端自己 exit 0），把它写成「异常退出」会
 * 让用户去查一个不存在的崩溃。
 *
 * @param exitCode `QProcess::exitCode()`。
 * @param wasStopping 退出之前我们是不是已经发过 `stop` / 按过强杀。
 */
inline ServerModel::Status statusAfterExit(int exitCode, bool wasStopping) {
    Q_UNUSED(wasStopping);
    return exitCode == 0 ? ServerModel::Status::Stopped : ServerModel::Status::Crashed;
}

/**
 * @brief 「强制结束」什么时候才允许点 —— 发过 `stop` 之后要等的那个毫秒数。
 * @param modCount `<目录>/mods` 下 `.jar` 的个数（`.jar.disabled` 不算）。
 * @param worldBytes `<目录>/world` 那棵树的总字节数（没有世界 / 读不出来 = 0）。
 *
 * 表格（**规格的唯一出处**，改这里就是改产品行为）：
 *
 * | 那一头 | 加多少 | 封顶 |
 * |---|---|---|
 * | 起手 | 15 s | —— |
 * | 每 32 个模组 | +5 s | +45 s |
 * | 每 1 GiB 存档 | +5 s | +60 s |
 * | 总计 | —— | **120 s** |
 *
 * 为什么两头的封顶不一样：几百个 mod 的收尾是有上限的（多数是同步 IO，快慢差不了几十秒），
 * 而存档那一头是真没底 —— 一个 20 GiB 的服存一次盘就是分钟级。所以存档给的余量更大。
 */
inline int gracefulStopMs(int modCount, qint64 worldBytes) {
    constexpr int kBaseMs = 15'000;
    constexpr int kModulesPerStep = 32;
    constexpr int kModulesStepMs = 5'000;
    constexpr int kModulesCapMs = 45'000;
    constexpr qint64 kGiB = 1024LL * 1024LL * 1024LL;
    constexpr int kWorldStepMs = 5'000;
    constexpr int kWorldCapMs = 60'000;
    constexpr int kMaxMs = 120'000;

    int total = kBaseMs;
    if (modCount > 0) {
        const int steps = modCount / kModulesPerStep;
        total += qMin(steps * kModulesStepMs, kModulesCapMs);
    }
    if (worldBytes > 0) {
        const qint64 steps = worldBytes / kGiB;
        // 不足 1 GiB 也算一步 —— 不然 0.9 GiB 的存档和一个空目录一个待遇。
        const qint64 grown = (steps + (worldBytes % kGiB != 0 ? 1 : 0)) * qint64(kWorldStepMs);
        total += int(qMin<qint64>(grown, kWorldCapMs));
    }
    return qMin(total, kMaxMs);
}

/// 同上，秒（给界面/日志用；**判据读这一个，别在测试里写死秒数**）。
inline int gracefulStopSeconds(int modCount, qint64 worldBytes) {
    return gracefulStopMs(modCount, worldBytes) / 1000;
}

// ---------------------------------------------------------------------------
// 用户态那一句
// ---------------------------------------------------------------------------

/// 「停止中」那一行小字（界面上那句"正在正常停下来"，带上还要等多久）。
inline QString stoppingNote(int seconds) {
    return QStringLiteral("正在停止服务端，等待保存数据（最多 %1 秒）。").arg(seconds);
}

/// 宽限时间到了之后那一句 —— 说明那颗「强制结束」为什么现在亮了。
inline QString forceKillNote(int seconds) {
    return QStringLiteral("服务端在 %1 秒内未退出。可强制结束，但可能丢失未保存的存档数据。").arg(seconds);
}

/// 服务端目录里没有那个入口文件时，点「启动」看到的那句话。
inline QString notInstalledMessage() {
    return QStringLiteral("服务端安装不完整：缺少启动文件。请通过「修复」补全安装。");
}

/// EULA 那道闸上那句正文（**不替用户做决定**，见下面 `eulaPromptText()` 那段）。
inline QString eulaLinkText() {
    return QStringLiteral("打开《Minecraft 最终用户许可协议》原文");
}

/// 原文地址。★ 只在用户点了那一行链接时才会被打开，我们**不会**自己去请求它。
inline QString eulaUrl() {
    return QStringLiteral("https://aka.ms/MinecraftEULA");
}

/**
 * @brief EULA 那道闸的正文与标题（**唯一出处**，界面照抄）。
 *
 * 三条规矩写死在这儿：
 * ① **不自动接受**（要求不做）—— 只有用户自己勾了才写 `eula=true`；
 * ② 勾选框那一句只陈述"我读过并同意哪一份协议"，不替他判断"这个协议没问题"；
 * ③ 原文地址是**可点的一行**（用户 点名：「要提供通向 eula 的链接，且可以
 * 点击跳转」）—— 他把同意两个字打在屏幕上之前得能真看见那是什么。
 */
inline QString eulaPromptTitle() {
    return QStringLiteral("Minecraft 最终用户许可协议");
}

inline QString eulaPromptText() {
    return QStringLiteral("首次启动 Minecraft 服务端前，需阅读并接受最终用户许可协议。"
                          "仅在勾选协议并选择「同意并启动」后，才会在 eula.txt 中写入 eula=true。");
}

inline QString eulaCheckBoxText() {
    return QStringLiteral("我已阅读并同意《Minecraft 最终用户许可协议》");
}

}  // namespace ServerRun

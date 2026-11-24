/**
 * @file GameExitDiagnosis.cpp
 * @brief `GameExitDiagnosis` 的实现（见 .hpp 的文件头：为什么这些规则单独放一层）。
 *
 * NOTE: 这里回出去的全是普通字符串字面量（和 `util/ErrorReport` 一个路数）。整个仓库没有
 * 翻译那一层（没有 .ts、没有 `installTranslator`），所以那些 `QObject::tr(...)` 只是
 * 原样把字面量还回来 —— 在这一层里写它只会让每一行多一段噪音。
 */

#include "GameExitDiagnosis.hpp"
#include <QCoreApplication>

namespace {

/// 一条"这个退出码是什么意思"。
///
/// NOTE: 表里只放**能指名道姓**的那几个 —— Windows 上 `QProcess` 报的崩溃码是 NTSTATUS，
/// 高位 0xC0 的那一档是"内核已经知道出了什么事"的，说得出人话；剩下的一律按十进制
/// 原样写（编一个像样的名字比不写更糟：用户会拿着一个错的名字去搜）。
struct ExitCodeName {
    int code;
    const char *name;
};

constexpr ExitCodeName kExitCodeNames[] = {
    {-1073741819, QT_TRANSLATE_NOOP("GameExitDiagnosis", "内存访问冲突")},           // 0xC0000005 STATUS_ACCESS_VIOLATION
    {-1073741818, QT_TRANSLATE_NOOP("GameExitDiagnosis", "内存分页失败")},           // 0xC0000006 STATUS_IN_PAGE_ERROR
    {-1073741795, QT_TRANSLATE_NOOP("GameExitDiagnosis", "非法指令")},               // 0xC000001D STATUS_ILLEGAL_INSTRUCTION
    {-1073741676, QT_TRANSLATE_NOOP("GameExitDiagnosis", "整数除以零")},             // 0xC0000094 STATUS_INTEGER_DIVIDE_BY_ZERO
    {-1073741571, QT_TRANSLATE_NOOP("GameExitDiagnosis", "栈溢出")},                 // 0xC00000FD STATUS_STACK_OVERFLOW
    {-1073741515, QT_TRANSLATE_NOOP("GameExitDiagnosis", "缺一个系统 DLL")},         // 0xC0000135 STATUS_DLL_NOT_FOUND
    {-1073741511, QT_TRANSLATE_NOOP("GameExitDiagnosis", "DLL 里的入口点找不到")},   // 0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND
    {-1073741502, QT_TRANSLATE_NOOP("GameExitDiagnosis", "DLL 初始化失败")},         // 0xC0000142 STATUS_DLL_INIT_FAILED
    {-1073740940, QT_TRANSLATE_NOOP("GameExitDiagnosis", "堆损坏")},                 // 0xC0000374 STATUS_HEAP_CORRUPTION
    {-1073740791, QT_TRANSLATE_NOOP("GameExitDiagnosis", "栈上的缓冲区写越界")},     // 0xC0000409 STATUS_STACK_BUFFER_OVERRUN
    {-2147483645, QT_TRANSLATE_NOOP("GameExitDiagnosis", "断点")},                   // 0x80000003 STATUS_BREAKPOINT
};

const char *nameForExitCode(int code) {
    for (const ExitCodeName &entry : kExitCodeNames) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return nullptr;
}

/// 那几个"这段代码本来就不该出现在一个 Java 进程里"的崩溃码：**内核层面上**就是坏内存 /
/// 坏栈 / 坏指令，和"某个模组写坏了东西"这一类高度相关。
bool isMemoryFault(int code) {
    return code == -1073741819      // 访问冲突
           || code == -1073741795   // 非法指令
           || code == -1073740940   // 堆损坏
           || code == -1073740791;  // 栈上的缓冲区写越界
}

/// 进程**根本没起来**（加载期就死了）那一档：DLL 找不到 / 入口点找不到 / DLL 初始化失败。
bool isLoadFailure(int code) {
    return code == -1073741515 || code == -1073741511 || code == -1073741502;
}

}  // namespace

namespace GameExitDiagnosis {

bool isAbnormal(const Facts &facts) {
    return isAbnormal(facts.exitCode, facts.crashed);
}

bool logShowsMissingDependency(const QString &logText) {
    if (logText.isEmpty()) {
        return false;
    }
    // 三句**加载器自己打的**原文（大小写不敏感，因为有的加载器把整行大写）：
    // Forge / NeoForge：`Missing or unsupported mandatory dependencies:`
    // Fabric：`requires ... which is missing!`
    // Fabric（另一种收尾）：`requires any version of ...`
    // 认的是"它说了"，不是"我们按清单猜"—— 猜错了用户会去删一个没问题的模组。
    static const char *const kNeedles[] = {
        "missing or unsupported mandatory dependencies",
        "which is missing!",
        "requires any version of",
    };
    for (const char *needle : kNeedles) {
        if (logText.contains(QLatin1String(needle), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool logShowsOutOfMemory(const QString &logText) {
    return !logText.isEmpty()
           && logText.contains(QLatin1String("OutOfMemoryError"), Qt::CaseInsensitive);
}

QString exitDescription(int exitCode) {
    const char *name = nameForExitCode(exitCode);
    if (name == nullptr) {
        return QString::number(exitCode);
    }
    // 十六进制那一截**由已知的码算出来**（不另抄一张表）：同一件事写两遍迟早会有一遍写错，
    // 而"名字对、码不对"这种错最误导人 —— 用户拿着 0xC0000006 去搜"栈溢出"。
    const QString hex = QString::number(static_cast<quint32>(exitCode), 16)
                            .rightJustified(8, QLatin1Char('0'))
                            .toUpper();
    return QCoreApplication::translate("GameExitDiagnosis", "0x%1（%2）").arg(hex, QCoreApplication::translate("GameExitDiagnosis", name));
}

QString summary(const Facts &facts) {
    const QString where = facts.instanceName.isEmpty() ? facts.instanceId : facts.instanceName;
    if (facts.crashed) {
        // 「崩溃」这一档不问退出码的正负：`QProcess` 报的 CrashExit 已经是"不是它自己走的"，
        // 哪怕码看起来像个普通数字（比如 -1）也照崩溃说 —— 那是这一次退出最要紧的性质。
        return where.isEmpty() ? QCoreApplication::translate("GameExitDiagnosis", "游戏崩溃退出（%1）。").arg(exitDescription(facts.exitCode))
                               : QCoreApplication::translate("GameExitDiagnosis", "「%1」崩溃退出（%2）。")
                                     .arg(where, exitDescription(facts.exitCode));
    }
    return where.isEmpty()
               ? QCoreApplication::translate("GameExitDiagnosis", "游戏以非 0 退出码结束（%1）。").arg(exitDescription(facts.exitCode))
               : QCoreApplication::translate("GameExitDiagnosis", "「%1」以非 0 退出码结束（%2）。")
                     .arg(where, exitDescription(facts.exitCode));
}

QStringList hints(const Facts &facts) {
    QStringList out;
    const auto say = [&out](const QString &text) {
        if (!out.contains(text)) {
            out.append(text);
        }
    };
    const QString where = facts.instanceName.isEmpty() ? facts.instanceId : facts.instanceName;

    // ① Java 版本对不上。**两个数都非 0 才说话** —— 只拿到一个的时候分不出"对得上"和
    // "没测出来"，那时候说"版本不匹配"就是猜。
    if (facts.requiredJavaMajor > 0 && facts.usedJavaMajor > 0
        && facts.requiredJavaMajor != facts.usedJavaMajor) {
        QString text = QCoreApplication::translate("GameExitDiagnosis", "「%1」要求 Java %2，本次实际使用的是 Java %3。")
                           .arg(where)
                           .arg(facts.requiredJavaMajor)
                           .arg(facts.usedJavaMajor);
        if (!facts.javaPath.isEmpty()) {
            text += QCoreApplication::translate("GameExitDiagnosis", "（%1）").arg(facts.javaPath);
        }
        say(text);
    }

    // ② 内存。**超过物理内存**和**接近打满**分开说：前者是"一定分不到"，后者是"抢起来会不够"。
    if (facts.xmxMb > 0 && facts.totalMemoryMb > 0) {
        if (static_cast<qint64>(facts.xmxMb) > facts.totalMemoryMb) {
            say(QCoreApplication::translate("GameExitDiagnosis", "分配给游戏的堆内存为 %1 MB，设备总内存为 %2 MB。游戏可能无法启动或在运行中退出。请在「设置 → 启动」中调低内存分配。")
                    .arg(facts.xmxMb)
                    .arg(facts.totalMemoryMb));
        } else if (static_cast<qint64>(facts.xmxMb) * 4 > facts.totalMemoryMb * 3) {
            say(QCoreApplication::translate("GameExitDiagnosis", "游戏分配了 %1 MB 堆内存，超过设备总内存 %2 MB 的四分之三。请为系统和其他程序预留内存。")
                    .arg(facts.xmxMb)
                    .arg(facts.totalMemoryMb));
        }
    }

    // ③ 日志里那句 OutOfMemoryError。**比上面那条更硬**：上面是"看着悬"，这一条是
    // "JVM 自己说了"。两条会一起出现，用两句话分别说清"为什么"和"依据"。
    if (facts.sawOutOfMemory) {
        say(QCoreApplication::translate("GameExitDiagnosis", "日志记录了 OutOfMemoryError，游戏可用内存不足。"));
    }

    // ④ 缺前置的模组。依据是**加载器自己打的那句话**（`logShowsMissingDependency()`），
    // 不是我们按清单猜的 —— 退出这一刻去联网问一遍每个模组的前置，用户只会看到启动器
    // 卡在那里。所以这里**不报个数**：我们手上确实没有那个数。
    if (facts.sawMissingDependency) {
        say(QCoreApplication::translate("GameExitDiagnosis", "日志显示缺少模组依赖。请在「文件」页选择「检查更新」，查看所需依赖。"));
    }

    // ⑤ 崩的方式。坏内存 / 坏栈 / 坏指令这一档，在**装了模组加载器**时才指向模组；原版也崩
    // 的话更可能是驱动 / 杀软那一类，口风就不一样。
    if (facts.crashed && isMemoryFault(facts.exitCode)) {
        if (facts.loaderName.isEmpty()) {
            say(QCoreApplication::translate("GameExitDiagnosis", "游戏出现内存访问错误（%1）。请检查设备驱动和安全软件是否影响游戏进程。")
                    .arg(exitDescription(facts.exitCode)));
        } else {
            say(QCoreApplication::translate("GameExitDiagnosis", "游戏出现内存访问错误（%1）。请检查模组及其依赖是否与游戏版本兼容。")
                    .arg(exitDescription(facts.exitCode)));
        }
    }

    // ⑥ 加载期就死了：这不是"游戏跑到一半崩了"，是"进程压根没起来"，两者的排查方向差很远。
    if (isLoadFailure(facts.exitCode)) {
        say(QCoreApplication::translate("GameExitDiagnosis", "游戏在加载阶段退出（%1）。请检查系统运行库及加载器文件是否完整。")
                .arg(exitDescription(facts.exitCode)));
    }

    // ⑦ 非崩溃、也没抓到任何错误行：这时候最像"它就是自己退出的"，与其让用户去翻一份空日志，
    // 不如直说。**只在确实拿到了日志的情况下说** —— 没开日志开关时"没有错误行"什么都不代表。
    //
    // 加载期就死了那一档（⑥）**排除在外**：那两种情况都会被判成"没有错误行"（进程压根
    // 没跑到打日志那一步），可它们的原因差得远 —— 一个缺 DLL、一个可能是用户自己关的。
    // 两句一起说等于让用户二选一，那比只说一句糟。
    if (!facts.crashed && facts.exitCode != 0 && facts.logCaptured && facts.errorLineCount == 0
        && !isLoadFailure(facts.exitCode)) {
        // 这句话原来写「ERROR/WARN 都没有」，可 `errorLineCount` 数的是
        // **ERROR 及以上**（Error / Fatal，出处 `GameLogWindow::logTextFrom(Error)` 和
        // `sessionLogTailErrors()`）—— 日志里只有 WARN 时它也会照说不误，玩家照着
        // "WARN 也没有"去排除，把真正的线索漏掉。文案跟着判据写。
        say(QCoreApplication::translate("GameExitDiagnosis", "日志中未记录 ERROR 或 Fatal。请确认游戏是否正常退出，或存档是否在其他进程中打开。"));
    }

    // ⑧ 压根没拿到输出。放在最后：它是"上面那些结论的依据缺一块"的说明，不是原因本身。
    if (!facts.logCaptured) {
        // 这句**不能**把原因归到「显示游戏日志」那个开关上 ——
        // 进程输出另有一份和窗无关的底账（`GameSessionController::sessionLogTail`），
        // 开关关着也照样收得到。真的一行都没有，那是游戏自己没打到能收的地方。
        say(QCoreApplication::translate("GameExitDiagnosis", "游戏进程退出前未生成日志。请在「设置 → 启动」中确认 Java 路径及内存分配。"));
    }

    return out;
}

}  // namespace GameExitDiagnosis

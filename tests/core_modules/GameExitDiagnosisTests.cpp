#include "Support.hpp"

void runGameExitDiagnosisTests() {
    section("ZP1. 退出码写成一句人话（认不出的原样写，不编）");
    check(GameExitDiagnosis::exitDescription(-1073741819)
              == QStringLiteral("0xC0000005（内存访问冲突）"),
          "★ ZP1 访问冲突那几个字是认出来的",
          GameExitDiagnosis::exitDescription(-1073741819));
    check(GameExitDiagnosis::exitDescription(-1073740940)
              == QStringLiteral("0xC0000374（堆损坏）"),
          "ZP1 堆损坏");
    check(GameExitDiagnosis::exitDescription(-1073741515)
              == QStringLiteral("0xC0000135（缺一个系统 DLL）"),
          "ZP1 缺 DLL");
    // 十六进制那一截是**由那个已知的码算出来的**（同一件事不写两遍）：`0xC0000006` 是
    // 分页失败，而它的十进制是 -1073741818 —— 差一个数就会指到"访问冲突"上去。
    check(GameExitDiagnosis::exitDescription(-1073741818)
              == QStringLiteral("0xC0000006（内存分页失败）"),
          "★ ZP1 名字和十六进制对得上（不另抄一张表）");
    // 认不出来的**不编**：编一个像样的名字比不写更糟 —— 用户会拿着一个错的名字去搜。
    check(GameExitDiagnosis::exitDescription(1) == QStringLiteral("1"), "★ ZP1 认不出的照原样写");
    check(GameExitDiagnosis::exitDescription(0) == QStringLiteral("0"), "ZP1 零也是原样");

    section("ZP2. 哪些退出算「异常」");
    check(!GameExitDiagnosis::isAbnormal(0, false), "★ ZP2 以 0 正常退出的不算异常");
    check(GameExitDiagnosis::isAbnormal(0, true),
          "★ ZP2 崩了但码正好是 0 **也算**（那种 0 什么都不能证明）");
    check(GameExitDiagnosis::isAbnormal(1, false), "ZP2 非 0 退出码算");
    check(GameExitDiagnosis::isAbnormal(-1073741819, false), "ZP2 负的退出码算");
    // 两个重载必须是同一条判据（上面那个调的就是下面这个）。
    GameExitDiagnosis::Facts abnormalFacts;
    abnormalFacts.exitCode = 7;
    check(GameExitDiagnosis::isAbnormal(abnormalFacts)
              == GameExitDiagnosis::isAbnormal(7, false),
          "★ ZP2 两个重载说的是同一条规则");

    section("ZP3. 一句概括");
    GameExitDiagnosis::Facts facts;
    facts.instanceName = QStringLiteral("我的整合包");
    facts.instanceId = QStringLiteral("1.20.1-forge");
    facts.crashed = true;
    facts.exitCode = -1073741819;
    check(GameExitDiagnosis::summary(facts)
              == QStringLiteral("「我的整合包」崩溃退出（0xC0000005（内存访问冲突））。"),
          "★ ZP3 崩溃那一档说的是「崩溃退出」",
          GameExitDiagnosis::summary(facts));
    facts.crashed = false;
    facts.exitCode = 3;
    check(GameExitDiagnosis::summary(facts) == QStringLiteral("「我的整合包」以非 0 退出码结束（3）。"),
          "★ ZP3 非崩溃那一档说的是「以非 0 退出码结束」",
          GameExitDiagnosis::summary(facts));
    // 名字空着时不留一个空引号（那种「「」以非 0 退出码结束」看着像坏了）。
    GameExitDiagnosis::Facts noName;
    noName.instanceId = QStringLiteral("1.20.1-forge");
    noName.exitCode = 3;
    check(GameExitDiagnosis::summary(noName)
              == QStringLiteral("「1.20.1-forge」以非 0 退出码结束（3）。"),
          "ZP3 显示名空着时用实例 id 顶上");
    noName.instanceId.clear();
    check(GameExitDiagnosis::summary(noName) == QStringLiteral("游戏以非 0 退出码结束（3）。"),
          "★ ZP3 两个名字都没有时不留空引号", GameExitDiagnosis::summary(noName));

    section("ZP4. 日志里那几句话认得出来");
    check(GameExitDiagnosis::logShowsOutOfMemory(
              QStringLiteral("[21:03:11] [Render thread/ERROR] java.lang.OutOfMemoryError: Java heap space")),
          "ZP4 OutOfMemoryError");
    check(!GameExitDiagnosis::logShowsOutOfMemory(QStringLiteral("一切正常")),
          "ZP4 没有就是没有");
    check(!GameExitDiagnosis::logShowsOutOfMemory(QString()), "ZP4 空串不当成有");
    // 三个加载器各自那句原文；大小写不敏感（有的加载器把整行大写）。
    check(GameExitDiagnosis::logShowsMissingDependency(
              QStringLiteral("Missing or unsupported mandatory dependencies:\n\tMod ID: x")),
          "ZP4 NeoForge / Forge 那句");
    check(GameExitDiagnosis::logShowsMissingDependency(
              QStringLiteral("MISSING OR UNSUPPORTED MANDATORY DEPENDENCIES:")),
          "ZP4 大写也认");
    check(GameExitDiagnosis::logShowsMissingDependency(
              QStringLiteral("Mod 'a' requires any version of mod 'b', which is missing!")),
          "ZP4 Fabric 那句");
    check(!GameExitDiagnosis::logShowsMissingDependency(QStringLiteral("Loaded 42 mods")),
          "★ ZP4 没说不许猜（认的是加载器自己那句原文，不是我们按清单猜）");

    section("ZP5. 提示那张表：每一条都指着某个字段");
    // ---------------------------------------------------------------------
    // 这一节的底：**"这一趟收到了游戏的输出"**。⑧那一条（没收到输出）在 `logCaptured`
    // 为假时**必然**多说一句，而下面绝大多数判据要量的是别的规则 —— 所以先把它摆成"收到了"，
    // 想验⑧的那两处再单独翻过去（见 ZP5 ⑦/⑧）。
    GameExitDiagnosis::Facts base;
    base.logCaptured = true;

    // ① Java 版本对不上 —— **两个数都非 0 才说话**：只拿到一个的时候分不出"对得上"和
    // "没测出来"，那时候说"版本不匹配"就是猜。
    GameExitDiagnosis::Facts javaMismatch = base;
    javaMismatch.requiredJavaMajor = 21;
    javaMismatch.usedJavaMajor = 17;
    javaMismatch.javaPath = QStringLiteral("D:/Java/jdk17/bin/java.exe");
    const QStringList javaHints = GameExitDiagnosis::hints(javaMismatch);
    check(javaHints.size() == 1 && javaHints.first().contains(QStringLiteral("要求 Java 21"))
              && javaHints.first().contains(QStringLiteral("实际使用的是 Java 17"))
              && javaHints.first().contains(QStringLiteral("D:/Java/jdk17/bin/java.exe")),
          "★ ZP5 ①Java 对不上（两个数都有时说得出来）",
          javaHints.join(QStringLiteral(" | ")));
    GameExitDiagnosis::Facts onlyRequired = javaMismatch;
    onlyRequired.usedJavaMajor = 0;   // 实际用的那个没读出来
    onlyRequired.javaPath.clear();
    check(GameExitDiagnosis::hints(onlyRequired).isEmpty(),
          "★ ZP5 ①只拿到「要什么」时**闭嘴**（那正是猜）");
    GameExitDiagnosis::Facts sameMajor = javaMismatch;
    sameMajor.usedJavaMajor = 21;
    check(GameExitDiagnosis::hints(sameMajor).isEmpty(), "ZP5 ①对得上时也不说话");

    // ② 内存：**超过物理内存**和**接近打满**分开说（前者一定分不到，后者抢起来才不够）。
    GameExitDiagnosis::Facts overMemory = base;
    overMemory.xmxMb = 65536;
    overMemory.totalMemoryMb = 32768;
    const QStringList overHints = GameExitDiagnosis::hints(overMemory);
    check(overHints.size() == 1 && overHints.first().contains(QStringLiteral("65536"))
              && overHints.first().contains(QStringLiteral("设备总内存为 32768 MB")),
          "★ ZP5 ②要的比机器有的还多 ⇒ 一定分不到",
          overHints.join(QStringLiteral(" | ")));
    GameExitDiagnosis::Facts nearMemory = base;
    nearMemory.xmxMb = 28672;   // 32768 的 7/8
    nearMemory.totalMemoryMb = 32768;
    const QStringList nearHints = GameExitDiagnosis::hints(nearMemory);
    check(nearHints.size() == 1 && nearHints.first().contains(QStringLiteral("四分之三")),
          "ZP5 ②占了四分之三以上 ⇒ 另一句（口风不一样）",
          nearHints.join(QStringLiteral(" | ")));
    // 边界：判据是 `xmx * 4 > 总内存 * 3`，所以**正好四分之三不算**"以上"（那一侧是开的）。
    // 24576 / 32768 正好是四分之三 —— 这一档一个字都不该说。
    GameExitDiagnosis::Facts exactlyThreeQuarters = base;
    exactlyThreeQuarters.xmxMb = 24576;
    exactlyThreeQuarters.totalMemoryMb = 32768;
    check(GameExitDiagnosis::hints(exactlyThreeQuarters).isEmpty(),
          "★ ZP5 ②正好四分之三 ⇒ 不算「以上」（边界是开的）");
    GameExitDiagnosis::Facts smallMemory = base;
    smallMemory.xmxMb = 4096;
    smallMemory.totalMemoryMb = 32768;
    check(GameExitDiagnosis::hints(smallMemory).isEmpty(), "ZP5 ②分得很少时不说内存");

    // ③ 日志里那句 OutOfMemoryError —— 比"看着悬"那条更硬（JVM 自己说了）。
    GameExitDiagnosis::Facts oom;
    oom.sawOutOfMemory = true;
    oom.logCaptured = true;
    check(GameExitDiagnosis::hints(oom).size() == 1
              && GameExitDiagnosis::hints(oom).first().contains(QStringLiteral("OutOfMemoryError")),
          "★ ZP5 ③日志里写着 OutOfMemoryError");

    // ④ 缺前置：依据是**加载器自己打的那句**，所以**不报个数**（我们手上确实没有那个数）。
    GameExitDiagnosis::Facts missing;
    missing.sawMissingDependency = true;
    missing.logCaptured = true;
    const QStringList missingHints = GameExitDiagnosis::hints(missing);
    check(missingHints.size() == 1
              && missingHints.first().contains(QStringLiteral("缺少模组依赖"))
              && missingHints.first().contains(QStringLiteral("检查更新")),
          "★ ZP5 ④缺前置那一条 (指向「检查更新」)",
          missingHints.join(QStringLiteral(" | ")));
    check(!missingHints.first().contains(QStringLiteral("个"))
              || missingHints.first().contains(QStringLiteral("哪一个")),
          "ZP5 ④不编一个数出来");

    // ⑤ 崩的方式：同一句"坏内存"，**原版和装了加载器的口风不一样**。
    GameExitDiagnosis::Facts vanillaCrash;
    vanillaCrash.crashed = true;
    vanillaCrash.exitCode = -1073741819;
    vanillaCrash.logCaptured = true;
    const QStringList vanillaHints = GameExitDiagnosis::hints(vanillaCrash);
    check(vanillaHints.size() == 1
              && vanillaHints.first().contains(QStringLiteral("驱动")),
          "★ ZP5 ⑤原版崩在坏内存上 ⇒ 指向本机驱动/安全软件",
          vanillaHints.join(QStringLiteral(" | ")));
    GameExitDiagnosis::Facts moddedCrash = vanillaCrash;
    moddedCrash.loaderName = QStringLiteral("Fabric");
    const QStringList moddedHints = GameExitDiagnosis::hints(moddedCrash);
    check(moddedHints.size() == 1 && moddedHints.first().contains(QStringLiteral("模组"))
              && !moddedHints.first().contains(QStringLiteral("驱动")),
          "★ ZP5 ⑤装了加载器 ⇒ 指向模组/前置（不是驱动）",
          moddedHints.join(QStringLiteral(" | ")));

    // ⑥ 加载期就死了：不是"跑到一半崩了"，排查方向差很远（缺运行库 / 被安全软件拦了）。
    GameExitDiagnosis::Facts loadFailure;
    loadFailure.exitCode = -1073741515;
    loadFailure.logCaptured = true;
    const QStringList loadHints = GameExitDiagnosis::hints(loadFailure);
    check(loadHints.size() == 1 && loadHints.first().contains(QStringLiteral("加载阶段")),
          "★ ZP5 ⑥缺 DLL 那一档说的是「进程压根没起来」",
          loadHints.join(QStringLiteral(" | ")));

    // ⑦ 非崩溃、非 0、日志里一条错误都没有 ⇒ 最像"它就是自己退出的"。**只在确实拿到了
    // 日志的情况下说** —— 没开日志开关时"没有错误行"什么都不代表。
    GameExitDiagnosis::Facts quietExit;
    quietExit.exitCode = 1;
    quietExit.logCaptured = true;
    quietExit.errorLineCount = 0;
    const QStringList quietHints = GameExitDiagnosis::hints(quietExit);
    check(quietHints.size() == 1 && quietHints.first().contains(QStringLiteral("是否正常退出")),
          "★ ZP5 ⑦自己退出那一档", quietHints.join(QStringLiteral(" | ")));
    GameExitDiagnosis::Facts quietNoLog = quietExit;
    quietNoLog.logCaptured = false;
    check(GameExitDiagnosis::hints(quietNoLog).size() == 1
              && GameExitDiagnosis::hints(quietNoLog).first().contains(QStringLiteral("未生成日志")),
          "★ ZP5 ⑦没拿到日志时**不说**「自己退出」，改说「没收到输出」",
          GameExitDiagnosis::hints(quietNoLog).join(QStringLiteral(" | ")));

    // ⑧ 什么都没拿到 ⇒ **一个空表**（不是一句"可能有很多原因"）。
    GameExitDiagnosis::Facts nothing;
    nothing.crashed = true;
    nothing.exitCode = -1;   // 认不出来的码
    nothing.logCaptured = false;
    check(GameExitDiagnosis::hints(nothing).size() == 1
              && GameExitDiagnosis::hints(nothing).first().contains(QStringLiteral("未生成日志")),
          "ZP5 ⑧跑到这儿至少会有一句「没收到输出」");

    // 全都有的时候：条数 = 那几条都在，而且**不重复**。
    GameExitDiagnosis::Facts everything;
    everything.crashed = true;
    everything.exitCode = -1073741819;
    everything.requiredJavaMajor = 21;
    everything.usedJavaMajor = 8;
    everything.xmxMb = 65536;
    everything.totalMemoryMb = 16384;
    everything.loaderName = QStringLiteral("Forge");
    everything.logCaptured = true;
    everything.errorLineCount = 3;
    everything.sawOutOfMemory = true;
    everything.sawMissingDependency = true;
    const QStringList all = GameExitDiagnosis::hints(everything);
    check(all.size() == 5,
          "★ ZP5 全都凑齐时五条各说各的（Java / 内存 / OOM / 缺前置 / 崩的方式）",
          QString::number(all.size()));
    QSet<QString> unique;
    for (const QString &hint : all) {
        unique.insert(hint);
    }
    check(unique.size() == all.size(), "ZP5 一条都不重复");
    // 顺序稳定：同一个输入永远同一串话（验证工程和以后的界面都指着这一条）。
    check(GameExitDiagnosis::hints(everything) == all, "★ ZP5 同一份事实永远同一串话");
}


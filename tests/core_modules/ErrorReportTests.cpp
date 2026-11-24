#include "Support.hpp"
#include "util/SecretRedactor.hpp"
#include "util/LaunchScriptCredentials.hpp"

void runErrorReportTests() {
    section("日志凭据保护：完整值、跨块与导出");
    const QString credential = QStringLiteral("__MC_SECURITY_TEST_ABC123456789__");
    SecretRedactor::rememberArguments({QStringLiteral("--accessToken"), credential});
    const QString refresh = QStringLiteral("__AUDIT_REFRESH_SECRET_987654321__");
    const QString xbox = QStringLiteral("__AUDIT_XBOX_SECRET_987654321__");
    const QString profileName = QStringLiteral("ReadableProfileName");
    SecretRedactor::rememberCredentialFields(QJsonObject{
        {"accounts", QJsonArray{QJsonObject{{"name", profileName},
            {"microsoft", QJsonObject{{"refresh_token", refresh}}},
            {"xbox", QJsonObject{{"Token", xbox}}}}}}});
    check(SecretRedactor::redact(refresh + xbox + profileName) ==
          SecretRedactor::replacement() + SecretRedactor::replacement() + profileName,
          "嵌套账户与Xbox票据脱敏，普通档案名称仍可读");
    const QString masked = SecretRedactor::replacement();
    check(SecretRedactor::redact(credential + credential) == masked + masked,
          "重复凭据全部遮盖");
    bool splitsSafe = true;
    for (qsizetype split = 0; split <= credential.size(); ++split) {
        SecretRedactor::Stream stream;
        QString result = stream.push(QStringLiteral("log:") + credential.left(split));
        result += stream.push(credential.mid(split) + QLatin1Char('\n'));
        result += stream.push({}, true);
        splitsSafe &= result == QStringLiteral("log:") + masked + QLatin1Char('\n');
    }
    check(splitsSafe, "所有分块边界均不泄露凭据且保留普通文本");
    SecretRedactor::Stream bytes;
    QString byteResult;
    for (QChar ch : credential) byteResult += bytes.push(QString(ch));
    byteResult += bytes.push({}, true);
    check(byteResult == masked, "逐字符输出仍遮盖");
    SecretRedactor::Stream incomplete;
    QString fragment = incomplete.push(QStringLiteral("end:") + credential.left(14));
    fragment += incomplete.push({}, true);
    check(fragment == QStringLiteral("end:") + masked, "退出时未完成的凭据片段也遮盖");
    SecretRedactor::Stream plain;
    check(plain.push(QStringLiteral("ordinary log\n")) == QStringLiteral("ordinary log\n"),
          "普通日志不延迟");
    const QString longer = credential + QStringLiteral("EXT");
    SecretRedactor::remember(longer);
    SecretRedactor::Stream overlap;
    QString overlapResult = overlap.push(credential);
    overlapResult += overlap.push(QStringLiteral("EXT\n"), true);
    check(overlapResult == masked + QLatin1Char('\n'), "重叠凭据优先匹配长值");
    ErrorReport::Facts secretFacts;
    secretFacts.instanceName = credential;
    const QString report = ErrorReport::text(secretFacts, QDateTime::currentDateTime(), credential, credential);
    check(!report.contains(credential) && report.contains(masked), "报告头部与日志统一遮盖");
    const QString scriptArg = LaunchScriptCredentials::argument(
        QStringLiteral("-Dminecraft.accessToken=") + LaunchScriptCredentials::placeholder());
    check(scriptArg == QStringLiteral("\"-Dminecraft.accessToken=%WTFML_MC_ACCESS_TOKEN%\""),
          "在线脚本只保存运行时环境变量引用");
    check(LaunchScriptCredentials::preamble().contains(QStringLiteral("if not defined WTFML_MC_ACCESS_TOKEN")) &&
              LaunchScriptCredentials::preamble().contains(QStringLiteral("-notmatch")),
          "脚本拒绝缺失或不安全的运行时凭据");
    check(LaunchScriptCredentials::argument(QStringLiteral("offline")) == QStringLiteral("offline"),
          "普通脚本参数保持不变");
    section("ZI1. 头部那几行（顺序、形状、哪些空着就不写）");
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-28 21:03:11"),
                                                QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    ErrorReport::Facts full;
    full.launcherVersion = QStringLiteral("0.9.3");
    full.instanceName = QStringLiteral("我的整合包");
    full.instanceId = QStringLiteral("1.20.1-forge");
    full.accountName = QStringLiteral("Steve");
    full.systemDescription = ErrorReport::systemDescription();
    full.processId = 12345;
    full.startedAt = now.addSecs(-600);
    full.finished = true;
    full.crashed = false;
    full.exitCode = 0;

    const QString fullHeader = ErrorReport::header(full, now);
    const QStringList fullLines = fullHeader.split(QLatin1Char('\n'));
    check(fullLines.size() == 10, "ZI1 该写的行一行不多一行不少（标题 + 9 个字段）",
          QString::number(fullLines.size()));
    check(fullLines.value(0) == QStringLiteral("WTFML 错误报告"),
          "ZI1 第一行就是那句话", fullLines.value(0));
    check(fullLines.contains(QStringLiteral("生成时间：2026-09-28 21:03:11")),
          "ZI1 生成时间按外面传的那个走（这一格不读时钟）");
    check(fullLines.contains(QStringLiteral("启动器版本：0.9.3")), "ZI1 启动器版本那一行");
    check(fullHeader.contains(QStringLiteral("系统：") + ErrorReport::systemDescription()),
          "ZI1 系统那一行用的就是 systemDescription()");
    check(fullLines.contains(QStringLiteral("实例：我的整合包")), "ZI1 实例显示名那一行");
    check(fullLines.contains(QStringLiteral("实例 id：1.20.1-forge")), "ZI1 实例 id 那一行");
    check(fullLines.contains(QStringLiteral("账号：Steve")), "ZI1 账号那一行");
    check(fullLines.contains(QStringLiteral("进程 ID：12345")), "ZI1 进程 ID 那一行");
    check(fullLines.contains(QStringLiteral("启动时间：2026-09-28 20:53:11")),
          "ZI1 启动时间那一行");
    check(fullLines.contains(QStringLiteral("退出码：0")), "ZI1 正常退出就是那个数");

    // 异常终止：那个数是**负数**，光看它认不出是崩溃，所以后面必须跟一句。
    full.crashed = true;
    full.exitCode = -1073741819;
    check(ErrorReport::header(full, now).contains(
              QStringLiteral("退出码：-1073741819（异常终止）")),
          "ZI1 崩溃时退出码后面跟一句「（异常终止）」");

    full.finished = false;
    full.crashed = false;
    full.exitCode = 0;
    const QString liveHeader = ErrorReport::header(full, now);
    check(liveHeader.contains(QStringLiteral("退出码：（导出时游戏还在运行）")),
          "ZI1 还没退出时那一行明说「还在运行」");
    check(!liveHeader.contains(QStringLiteral("退出码：0")),
          "ZI1 而且**不许**顺手写个 0 出来（那是最坏的一种假话）");

    // 拿不到的那些字段：整行**不写**（留一个 `账号：` 比不写更难看，也更容易被当成"真有个
    // 空名字的账号"）。
    ErrorReport::Facts bare;
    bare.finished = true;
    const QString bareHeader = ErrorReport::header(bare, now);
    check(!bareHeader.contains(QStringLiteral("账号")) &&
              !bareHeader.contains(QStringLiteral("启动器版本")) &&
              !bareHeader.contains(QStringLiteral("系统")) &&
              !bareHeader.contains(QStringLiteral("实例")) &&
              !bareHeader.contains(QStringLiteral("进程 ID")) &&
              !bareHeader.contains(QStringLiteral("启动时间")),
          "ZI1 拿不到的字段整行不写（不是写个空的冒号在那儿）");
    check(bareHeader.contains(QStringLiteral("生成时间：")) &&
              bareHeader.contains(QStringLiteral("退出码：")),
          "ZI1 但时间和退出码那两行任何时候都在");

    section("ZI2. 整份报告：两段、分隔线、以及「一行都没有」那句");
    check(ErrorReport::sectionDivider(QStringLiteral("x")) ==
              QStringLiteral("------------------------------ x ------------------------------"),
          "ZI2 分隔线的形状");

    full.finished = true;
    full.crashed = false;
    full.exitCode = 0;
    full.errorLines = 2;
    full.totalLines = 5;
    const QString withErrors =
        ErrorReport::text(full, now,
                          QStringLiteral("[21:03:11] [Render thread/ERROR] 炸了\n"
                                         "[21:03:11] [Render thread/ERROR] 又炸了\n"),
                          QStringLiteral("[21:03:10] 起\n[21:03:11] 中\n"));
    check(withErrors.startsWith(ErrorReport::header(full, now)), "ZI2 报告从头部开始");
    check(withErrors.contains(QStringLiteral("错误与致命（共 2 行）")), "ZI2 错误那一段的标题");
    check(withErrors.contains(QStringLiteral("完整日志（共 5 行）")), "ZI2 日志那一段的标题");
    check(withErrors.contains(QLatin1Char('\n') +
                              ErrorReport::sectionDivider(QStringLiteral("完整日志（共 5 行）"))),
          "ZI2 分隔线**另起一行**（不是粘在最后一行后面）");
    check(withErrors.endsWith(QStringLiteral("中\n")), "ZI2 末尾收在一个换行上");

    // 两段文本末尾那半行可能不带换行（`logTextFrom()` 就是这么给的）—— 缺了要补一个。
    const QString halfLine = ErrorReport::text(full, now, QStringLiteral("一行错误"),
                                               QStringLiteral("一行日志"));
    check(halfLine.contains(QStringLiteral("一行错误\n")), "ZI2 摘录末尾补了换行");
    check(halfLine.endsWith(QStringLiteral("一行日志\n")), "ZI2 日志末尾也补了换行");

    // 一行错误都没有时**照样写上那一段**：空着的话看的人分不清"没出错"和"导出漏了"。
    full.errorLines = 0;
    const QString noErrors =
        ErrorReport::text(full, now, QString(), QStringLiteral("[21:03:10] 一切正常\n"));
    check(noErrors.contains(QStringLiteral("错误与致命（共 0 行）")),
          "ZI2 没错误时那一段的标题照旧（写的是 0 行）");
    check(noErrors.contains(QStringLiteral("本次没有 ERROR 及以上的输出")),
          "ZI2 而且明说「这一段里一行都没有」（不是空着）");
    check(noErrors.contains(QStringLiteral("[21:03:10] 一切正常")),
          "ZI2 日志那一段照旧在");

    // 内容截过就要**说出来**。崩溃报告正文只有末尾 200 / 500 行，而标题
    // 原来照样写「共 N 行」—— 玩家把一份八千行的日志导出、发给别人排查时，前面真正的
    // 起因早就不在文件里了，而文件自己说它是完整的。旗子由 `CrashReportController`
    // 在"记录里的总行数 > 实际留下的行数"时竖起来；日志窗那条导出是全量，两个都 false。
    {
        ErrorReport::Facts truncated = full;
        truncated.errorLines = 8000;
        truncated.totalLines = 12000;
        truncated.errorTruncated = true;
        truncated.logTruncated = true;
        const QString clipped = ErrorReport::text(
            truncated, now,
            QStringLiteral("[21:03:11] [Render thread/ERROR] 最后一条错误\n"),
            QStringLiteral("[21:03:10] 起\n[21:03:11] 中\n"));
        check(clipped.contains(QStringLiteral("错误与致命（末尾 1 行 / 原始共 8000 行）")),
              "ZI2 ★ 截过的错误摘录：标题写「末尾几行 / 原始共 N 行」");
        check(clipped.contains(
                  QStringLiteral("日志末尾（最近 2 行 / 原始共 12000 行；完整日志没有导出）")),
              "ZI2 ★ 截过的日志：标题明说完整日志不在这份文件里");
        // 标题里那个"最近几行"读的是**递进来的文本**，不是记录里的总数 —— 换一份短的
        // 文本，那个数就得跟着变（不然它写的是一句没人核对过的话）。
        check(ErrorReport::text(truncated, now, QStringLiteral("一行"), QStringLiteral("一行"))
                  .contains(QStringLiteral("日志末尾（最近 1 行 / 原始共 12000 行；完整日志没有导出）")),
              "ZI2 截过的日志标题按**实际留下来的行数**写（不是拿总数冒充）");
        // 旗子不竖 = 全量导出（日志窗那条路）—— 标题必须回到老写法。
        ErrorReport::Facts atLimit = full;
        atLimit.errorLines = 200;
        atLimit.totalLines = 500;
        check(ErrorReport::text(atLimit, now, QStringLiteral("一行"), QStringLiteral("一行"))
                  .contains(QStringLiteral("完整日志（共 500 行）")),
              "ZI2 没截（两个旗子都不竖）时标题照旧");
    }

    section("ZI3. 两种文件名的推荐值（实例名里的坏字符要换掉）");
    check(ErrorReport::suggestedFileName(QStringLiteral("我的整合包"), now) ==
              QStringLiteral("错误报告-我的整合包-20260928-210311.txt"),
          "ZI3 报告文件名",
          ErrorReport::suggestedFileName(QStringLiteral("我的整合包"), now));
    check(ErrorReport::suggestedLogFileName(QStringLiteral("我的整合包"), now) ==
              QStringLiteral("日志-我的整合包-20260928-210311.log"),
          "ZI3 日志文件名",
          ErrorReport::suggestedLogFileName(QStringLiteral("我的整合包"), now));
    // 冒号、斜杠这些在文件名里是不许出现的 —— 不换掉的话用户的感受是"导出失败了"，
    // 而原因只是名字里有个冒号。
    check(ErrorReport::suggestedFileName(QStringLiteral("a/b:c*d?e\"f<g>h|i\\j"), now) ==
              QStringLiteral("错误报告-a_b_c_d_e_f_g_h_i_j-20260928-210311.txt"),
          "ZI3 名字里的坏字符换成下划线",
          ErrorReport::suggestedFileName(QStringLiteral("a/b:c*d?e\"f<g>h|i\\j"), now));
    check(ErrorReport::suggestedFileName(QString(), now) ==
              QStringLiteral("错误报告-20260928-210311.txt"),
          "ZI3 名字空着时不留双横杠",
          ErrorReport::suggestedFileName(QString(), now));
    check(!ErrorReport::systemDescription().isEmpty() &&
              ErrorReport::systemDescription().contains(QSysInfo::currentCpuArchitecture()),
          "ZI3 系统那一行里带着架构",
          ErrorReport::systemDescription());

    section("ZI4. 「游戏异常退出」那一份多出来的三行 + 「可能的原因」那一节");
    // ---------------------------------------------------------------------
    // 这三行是要收的那几样（Java 版本 / 加载器版本 / 内存参数）。**只有那一路
    // 会填** —— 日志窗那颗「导出错误报告」手上没有这几个值，留空（上面 ZI1 那个 10 行的
    // 断言就是"留空 = 一行都不多"）。
    ErrorReport::Facts crash = full;
    crash.crashed = true;
    crash.exitCode = -1073741819;
    crash.finished = true;
    crash.javaVersion = QStringLiteral("17（D:/Java/jdk17/bin/java.exe）");
    crash.loaderVersion = QStringLiteral("Fabric 0.15.11");
    crash.memoryArgs = QStringLiteral("-Xmx 4096 MB / -Xms 512 MB（本机共 32768 MB）");
    const QString crashHeader = ErrorReport::header(crash, now);
    const QStringList crashLines = crashHeader.split(QLatin1Char('\n'));
    check(crashLines.size() == 13, "★ ZI4 填了那三样就多三行（10 + 3）",
          QString::number(crashLines.size()));
    check(crashLines.contains(QStringLiteral("加载器：Fabric 0.15.11")), "ZI4 加载器那一行");
    check(crashLines.contains(QStringLiteral("Java：17（D:/Java/jdk17/bin/java.exe）")),
          "ZI4 Java 那一行");
    check(crashLines.contains(
              QStringLiteral("内存：-Xmx 4096 MB / -Xms 512 MB（本机共 32768 MB）")),
          "ZI4 内存那一行");
    // 顺序：三行**接在账号后面**（上面那几行是人认这个实例，下面这几行才是这一趟的环境）。
    check(crashHeader.indexOf(QStringLiteral("账号：")) < crashHeader.indexOf(QStringLiteral("加载器："))
              && crashHeader.indexOf(QStringLiteral("加载器："))
                     < crashHeader.indexOf(QStringLiteral("Java："))
              && crashHeader.indexOf(QStringLiteral("Java："))
                     < crashHeader.indexOf(QStringLiteral("内存："))
              && crashHeader.indexOf(QStringLiteral("内存："))
                     < crashHeader.indexOf(QStringLiteral("进程 ID：")),
          "★ ZI4 三行接在账号后面、进程 ID 前面");

    // 「可能的原因」那一节：排在两段日志**前面**（看报告的人第一眼要知道的是"大概是什么事"），
    // 逐条前面带一个「· 」。一句都没有时**整个不写** —— 不写"暂时无法判断"那种废话。
    crash.hints = QStringList{QStringLiteral("这条是第一条。"), QStringLiteral("这条是第二条。")};
    const QString withHints =
        ErrorReport::text(crash, now, QStringLiteral("[21:03:11] 炸了\n"),
                          QStringLiteral("[21:03:10] 起\n"));
    check(withHints.contains(
              ErrorReport::sectionDivider(QStringLiteral("可能原因"))),
          "★ ZI4 有提示时才有那一节");
    check(withHints.indexOf(QStringLiteral("可能的原因")) < withHints.indexOf(QStringLiteral("错误与致命")),
          "★ ZI4 那一节排在错误摘录**前面**");
    check(withHints.contains(QStringLiteral("• 这条是第一条。\n• 这条是第二条。\n")),
          "ZI4 逐条写成「· 那句话」");
    crash.hints = QStringList();
    check(!ErrorReport::text(crash, now, QStringLiteral("x\n"), QStringLiteral("y\n"))
               .contains(QStringLiteral("可能原因")),
          "★ ZI4 一句提示都没有时那一节整个不写");
}


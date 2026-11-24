#include "Support.hpp"

void runJavaReleaseParseTests() {
    section("M. release 文件解析（majorVersionFromReleaseFile）");

    const QList<QPair<QString, int>> cases = {
        {QStringLiteral("JAVA_VERSION=\"21.0.7\"\n"), 21},
        {QStringLiteral("JAVA_VERSION=\"25.0.1\"\n"), 25},
        {QStringLiteral("JAVA_VERSION=\"17.0.15\"\n"), 17},
        {QStringLiteral("JAVA_VERSION=\"11.0.12\"\n"), 11},
        // Java 8 那代主版本在第二段
        {QStringLiteral("JAVA_VERSION=\"1.8.0_422\"\n"), 8},
        {QStringLiteral("JAVA_VERSION=\"1.8.0_51\"\n"), 8},
        // 只写了日期：不得被当成版本
        {QStringLiteral("JAVA_VERSION_DATE=\"2025-10-21\"\n"), -1},
        // 压根没有版本信息
        {QStringLiteral("MODULES=\"java.base\"\n"), -1},
        {QStringLiteral(""), -1},
        {QStringLiteral("\n\n"), -1},
        // 认不出来就不猜，返回 -1 让调用方退回真的问一次
        {QStringLiteral("JAVA_VERSION=\"not-a-version\"\n"), -1},
        {QStringLiteral("JAVA_VERSION=\"\"\n"), -1},
    };
    for (const auto &c : cases) {
        const int got = GlobalJavaSettings::majorVersionFromReleaseFile(c.first);
        const QString label =
            c.first.trimmed().isEmpty() ? QStringLiteral("<空>") : c.first.trimmed();
        check(got == c.second, QStringLiteral("release 解析 %1 -> %2").arg(label).arg(c.second),
              QStringLiteral("实际 %1").arg(got));
    }

    // 真实的 release 文件里 JAVA_VERSION 只是其中一行，后面还跟着一整行 MODULES。
    // 只认 JAVA_VERSION= 才不会撞上别的行。
    const QString realistic = QStringLiteral("JAVA_VERSION=\"25.0.1\"\n"
                                             "JAVA_VERSION_DATE=\"2025-10-21\"\n"
                                             "MODULES=\"java.base java.compiler java.xml\"\n"
                                             "IMPLEMENTOR=\"Microsoft\"\n");
    check(GlobalJavaSettings::majorVersionFromReleaseFile(realistic) == 25,
          QStringLiteral("真实 release 文件（JAVA_VERSION 后面还跟着 MODULES 等行）"),
          QString::number(GlobalJavaSettings::majorVersionFromReleaseFile(realistic)));

    // 真机对账：本机扫出来的每个运行时，两个来源必须说同一个版本。
    GlobalJavaSettings settings;
    const QMap<int, QString> found = settings.scanLocalJavaVersions();
    out() << "  本机扫到 " << found.size() << " 个运行时，逐个与 java -version 对账：\n";
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        const QString probed = javaVersionOf(it.value());
        // Java 8 问出来是 "1.8.0_51"，其余是 "25.0.1" 这种
        const QString expect = it.key() == 8 ? QStringLiteral("1.8") : QString::number(it.key());
        check(probed.startsWith(expect),
              QStringLiteral("Java %1：扫描结果与 java -version 一致").arg(it.key()),
              QStringLiteral("%1 -> %2").arg(it.value(), probed));
    }

    // pickMinimalJavaAtLeast()：安装加载器要的 Java **只要够新**（不像启动那样要精确主版本，
    // 见 GlobalJavaSettings::installerJavaFor() 的注释）。这是它的全部规则，所以整张表钉死。
    //
    // 「配的那份太老、本机恰好有够新的」那种场景在这儿造不出来（installerJavaFor() 要读环境
    // 变量、扫磁盘），所以只钉这个纯函数 —— 那才是"挑谁"这件事的唯一判据。
    {
        QMap<int, QString> candidates;
        candidates[8] = QStringLiteral("jre8");
        candidates[17] = QStringLiteral("jre17");
        candidates[21] = QStringLiteral("jre21");
        candidates[25] = QStringLiteral("jre25");
        check(GlobalJavaSettings::pickMinimalJavaAtLeast(17, candidates) == QLatin1String("jre17"),
              QStringLiteral("够新的里面取**最小**的（要 17，候选 17/21/25 → 17）"),
              GlobalJavaSettings::pickMinimalJavaAtLeast(17, candidates));
        check(GlobalJavaSettings::pickMinimalJavaAtLeast(18, candidates) == QLatin1String("jre21"),
              QStringLiteral("要 18 时取 21（17 不够新）"),
              GlobalJavaSettings::pickMinimalJavaAtLeast(18, candidates));
        check(GlobalJavaSettings::pickMinimalJavaAtLeast(8, candidates) == QLatin1String("jre8"),
              QStringLiteral("要 Java 8 时不该挑更新的那份（1.16.5 的安装器就得 Java 8）"),
              GlobalJavaSettings::pickMinimalJavaAtLeast(8, candidates));
        check(GlobalJavaSettings::pickMinimalJavaAtLeast(26, candidates).isEmpty(),
              QStringLiteral("一个都不够新 → 空（调用方据此报错，而不是随便挑一个）"));
        check(GlobalJavaSettings::pickMinimalJavaAtLeast(0, candidates).isEmpty() &&
                  GlobalJavaSettings::pickMinimalJavaAtLeast(-1, candidates).isEmpty(),
              QStringLiteral("requiredMajor <= 0（不知道要求）→ 空"));

        QMap<int, QString> withEmpty;
        withEmpty[13] = QString();
        withEmpty[17] = QStringLiteral("jre17");
        check(GlobalJavaSettings::pickMinimalJavaAtLeast(9, withEmpty) == QLatin1String("jre17"),
              QStringLiteral("路径为空的那条不算候选（主版本对得上也不行）"),
              GlobalJavaSettings::pickMinimalJavaAtLeast(9, withEmpty));

        candidates.clear();
        check(GlobalJavaSettings::pickMinimalJavaAtLeast(17, candidates).isEmpty(),
              QStringLiteral("候选是空的时候 → 空"));
    }
}


#include "Support.hpp"

void runServerRestartTests() {
    section("ZAF. 服务端异常退出后自动重启：哪一档才拉 / 连着几次收手 / 退避");

    // ---------- ZAF0. 这一次是怎么退的 ----------
    {
        check(ServerRestart::whyFor(true, false, 0) == ServerRestart::Why::UserStop,
              "★★ ZAF0 用户点了「停止」⇒ UserStop（退出码是 0 也一样）");
        check(ServerRestart::whyFor(false, true, 137) == ServerRestart::Why::UserStop,
              "★★ ZAF0 被强杀的那一趟也是 UserStop —— 强杀的退出码难看，但它不是崩了");
        check(ServerRestart::whyFor(false, false, 0) == ServerRestart::Why::CleanExit,
              "★ ZAF0 退出码 0 ⇒ CleanExit（控制台里敲了 stop 也走这一档）");
        check(ServerRestart::whyFor(false, false, 1) == ServerRestart::Why::Crash,
              "★ ZAF0 非 0 退出 ⇒ Crash");
        check(ServerRestart::whyFor(false, false, -1) == ServerRestart::Why::Crash,
              "★ ZAF0 起不来的那一档（−1）也算异常退出 —— 它下一次照样值得再试");
        check(ServerRestart::isAbnormal(ServerRestart::Why::Crash)
                  && !ServerRestart::isAbnormal(ServerRestart::Why::CleanExit)
                  && !ServerRestart::isAbnormal(ServerRestart::Why::UserStop),
              "★★ ZAF0 只有 Crash 会触发自动重启的考虑");
        check(ServerRestart::whyName(ServerRestart::Why::UserStop) == QStringLiteral("你自己停的")
                  && ServerRestart::whyName(ServerRestart::Why::CleanExit)
                         == QStringLiteral("正常退出")
                  && ServerRestart::whyName(ServerRestart::Why::Crash)
                         == QStringLiteral("异常退出"),
              "★ ZAF0 三档那三句话的唯一出处");
    }

    // ---------- ZAF1. 次数：越界回默认，不夹到边上 ----------
    {
        check(ServerRestart::normalizeMaxAttempts(0) == ServerRestart::kDefaultMaxAttempts,
              "★★ ZAF1 0 ⇒ 回默认那一档（不是夹到 1：夹到边上会让被限流看起来像用户自己选的）",
              QString::number(ServerRestart::normalizeMaxAttempts(0)));
        check(ServerRestart::normalizeMaxAttempts(99) == ServerRestart::kDefaultMaxAttempts,
              "★ ZAF1 越过大上限 ⇒ 默认",
              QString::number(ServerRestart::normalizeMaxAttempts(99)));
        check(ServerRestart::normalizeMaxAttempts(ServerRestart::kMinMaxAttempts)
                      == ServerRestart::kMinMaxAttempts
                  && ServerRestart::normalizeMaxAttempts(ServerRestart::kMaxMaxAttempts)
                         == ServerRestart::kMaxMaxAttempts,
              "ZAF1 两个边界值本身留得住");
        check(ServerRestart::normalizeMaxAttempts(5) == 5, "ZAF1 表内的合法值（5）原样留");
        ServerRestart::Policy policy;
        policy.enabled = true;
        policy.maxAttempts = 0;
        const ServerRestart::Policy normalized = ServerRestart::normalize(policy);
        check(normalized.enabled && normalized.maxAttempts == ServerRestart::kDefaultMaxAttempts,
              "ZAF1 normalize() 只动那个数，开关原样");
    }

    // ---------- ZAF2. 退避：5、10、20、40、60 封顶 ----------
    {
        check(ServerRestart::delaySeconds(1) == 5 && ServerRestart::delaySeconds(2) == 10
                  && ServerRestart::delaySeconds(3) == 20 && ServerRestart::delaySeconds(4) == 40
                  && ServerRestart::delaySeconds(5) == 60 && ServerRestart::delaySeconds(6) == 60,
              "★★ ZAF2 第一次 5 秒、之后每次翻倍、60 秒封顶（崩得越快等得越久）",
              QStringLiteral("%1/%2/%3/%4/%5")
                  .arg(ServerRestart::delaySeconds(1))
                  .arg(ServerRestart::delaySeconds(2))
                  .arg(ServerRestart::delaySeconds(3))
                  .arg(ServerRestart::delaySeconds(4))
                  .arg(ServerRestart::delaySeconds(6)));
        check(ServerRestart::delaySeconds(0) == ServerRestart::kBaseDelaySeconds
                  && ServerRestart::delaySeconds(-3) == ServerRestart::kBaseDelaySeconds,
              "ZAF2 第 0 / 负数次也回第一次那个数（调用方数错一格不至于等 0 秒）");
        check(ServerRestart::delaySeconds(100) == ServerRestart::kMaxDelaySeconds,
              "ZAF2 很大的次数也封在 60 秒");
        check(ServerRestart::kResetAfterSeconds > ServerRestart::kMaxDelaySeconds,
              "★ ZAF2 跑够久算新的一轮那个门槛比最长退避还长（不然重启循环永远等不到新一轮）",
              QString::number(ServerRestart::kResetAfterSeconds));
    }

    // ---------- ZAF3. 决定 ----------
    {
        ServerRestart::Policy policy;
        policy.enabled = true;
        policy.maxAttempts = 3;

        const ServerRestart::Decision first =
            ServerRestart::decide(policy, ServerRestart::Why::Crash, 0);
        check(first.restart && first.delaySeconds == 5
                  && first.note.contains(QStringLiteral("第 1 次"))
                  && first.note.contains(QStringLiteral("3")),
              "★★ ZAF3 第一次崩：5 秒后重启，那句话写清楚第几次 / 最多几次", first.note);

        const ServerRestart::Decision third =
            ServerRestart::decide(policy, ServerRestart::Why::Crash, 2);
        check(third.restart && third.delaySeconds == 20
                  && third.note.contains(QStringLiteral("第 3 次")),
              "★ ZAF3 已经重启过 2 次 ⇒ 第 3 次、等 20 秒", third.note);

        const ServerRestart::Decision exhausted =
            ServerRestart::decide(policy, ServerRestart::Why::Crash, 3);
        check(!exhausted.restart && exhausted.delaySeconds == 0
                  && exhausted.note.contains(QStringLiteral("已停止自动重启")),
              "★★ ZAF3 连着重启满 3 次 ⇒ 收手，而且那句话必须说出来"
              "（不然用户会一直等它自己起来）",
              exhausted.note);

        const ServerRestart::Decision clean =
            ServerRestart::decide(policy, ServerRestart::Why::CleanExit, 0);
        const ServerRestart::Decision user =
            ServerRestart::decide(policy, ServerRestart::Why::UserStop, 0);
        check(!clean.restart && clean.note.isEmpty() && !user.restart && user.note.isEmpty(),
              "★★ ZAF3 正常退出 / 用户停的 ⇒ 不重启、也不多说一句（这两档不是问题）");

        policy.enabled = false;
        const ServerRestart::Decision off =
            ServerRestart::decide(policy, ServerRestart::Why::Crash, 0);
        check(!off.restart && off.note.isEmpty(),
              "★★ ZAF3 策略关着 ⇒ 崩了也不拉（出厂就是这个状态：没问过用户的事不替他做）");

        policy.enabled = true;
        policy.maxAttempts = 0;  // 手改坏的那种
        const ServerRestart::Decision fixed =
            ServerRestart::decide(policy, ServerRestart::Why::Crash, 0);
        check(fixed.restart && fixed.note.contains(QStringLiteral("最多连着 3 次")),
              "★ ZAF3 次数写坏了 ⇒ 按默认的 3 次办事（不是一次都不拉）", fixed.note);

        const ServerRestart::Decision beyond =
            ServerRestart::decide(policy, ServerRestart::Why::Crash, 99);
        check(!beyond.restart, "ZAF3 已经重启过 99 次 ⇒ 当然不拉");
    }

    // ---------- ZAF4. 策略记在 `.wtfml-server` 里，读了要回来 ----------
    {
        ServerModel::Info info;
        info.serverId = QStringLiteral("s1");
        info.gameVersion = QStringLiteral("1.20.1");
        info.restartOnCrash = true;
        info.restartMaxAttempts = 7;
        bool ok = false;
        const ServerModel::Info back = ServerModel::fromJson(ServerModel::toJson(info), &ok);
        check(ok && back.restartOnCrash && back.restartMaxAttempts == 7,
              "★★ ZAF4 重启策略那两格写得出、读得回（记不住的设置等于没有）",
              QStringLiteral("%1/%2").arg(back.restartOnCrash).arg(back.restartMaxAttempts));

        ServerModel::Info plain;
        plain.serverId = QStringLiteral("s2");
        const QByteArray plainBytes = ServerModel::toJson(plain);
        check(!plainBytes.contains("restartOnCrash") && !plainBytes.contains("restartMaxAttempts"),
              "★ ZAF4 没动过策略（关着 + 默认 3 次）⇒ 一个字节都不多写（老文件的样子）",
              QString::fromUtf8(plainBytes));

        // 手改坏的次数：读的时候归一化成默认那一档。
        QJsonObject object;
        object.insert(QStringLiteral("format"), ServerModel::formatTag());
        object.insert(QStringLiteral("version"), ServerModel::formatVersion());
        object.insert(QStringLiteral("restartOnCrash"), true);
        object.insert(QStringLiteral("restartMaxAttempts"), 999);
        const ServerModel::Info repaired =
            ServerModel::fromJson(QJsonDocument(object).toJson());
        check(repaired.restartOnCrash
                  && repaired.restartMaxAttempts == ServerRestart::kDefaultMaxAttempts,
              "★★ ZAF4 手改成 999 ⇒ 读回来是默认那一档（normalizeMaxAttempts() 兜住）",
              QString::number(repaired.restartMaxAttempts));
    }
}


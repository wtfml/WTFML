#include "Support.hpp"

void runAccountSkinTests(const QString &sandbox) {
    section("W. 多账号存储（活动槽 ↔ 列表）");

    // 真值来源是 QSettings，照 I5 那套先把它搬进沙盒 —— **必须在第一个 QSettings 对象
    // 出现之前**。这一节的键在真机器上写一遍没有意义，还会把用户自己的登录状态覆盖掉。
    const QString settingsDir = sandbox + "/wtfml-accounts";
    QDir(settingsDir).removeRecursively();
    QDir().mkpath(settingsDir);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);

    const QString uuidA = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const QString uuidB = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    // ---- W1 空盘：什么都是空的 ----
    {
        check(!TokenStore::hasValue(TokenStore::minecraftKey()),
              "W1 前提：沙盒的活动槽本来是空的（否则说明沙盒没换成新目录）");
        check(AccountStore::accounts().isEmpty(), "W1 全新盘上没有已保存的账号");
        check(AccountStore::activeUuid().isEmpty(), "W1 全新盘上没有当前账号");
        check(!AccountStore::contains(uuidA), "W1 全新盘里查不到任何账号");
        check(AccountStore::nameOf(uuidA).isEmpty(), "W1 查一个不存在的账号，名字是空串");

        QString error;
        check(!AccountStore::captureActive(&error), "W1 没登录时收不进列表");
        check(!error.isEmpty(), "W1 而且给得出原因", error);
    }

    // ---- W2 只有 Minecraft 那一半凭据 → 不收 ----
    // 微软那份才是"下次还能不能换出新令牌"的来路。只有一半的账号收进列表，将来切过去
    // 必然换不出令牌 —— 与其那时候报一个查不明白的错，不如当场不收。
    {
        check(TokenStore::writeJson(TokenStore::minecraftKey(), stubMinecraftPayload(uuidA, "Alice")),
              "W2 前提：只写 Minecraft 那份");
        QString error;
        check(!AccountStore::captureActive(&error),
              "W2 只有 Minecraft 凭据、没有微软那份 → 不收进列表");
        check(!error.isEmpty(), "W2 而且说得出是缺了哪一半", error);
        check(AccountStore::accounts().isEmpty(), "W2 列表里一条都没多");
        TokenStore::clear(TokenStore::minecraftKey());
    }

    // ---- W3 两份都齐 → 收进列表，uuid 和名字都从凭据自己身上读 ----
    {
        putActiveCredentials(QStringLiteral("W3"), uuidA, QStringLiteral("Alice"),
                             QStringLiteral("rt-A"));
        QString error;
        check(AccountStore::captureActive(&error), "W3 两份凭据都齐 → 收进列表", error);

        const QList<SavedAccount> list = AccountStore::accounts();
        check(list.size() == 1, "W3 列表里正好一条", QString::number(list.size()));
        check(accountAt(list, 0).uuid == uuidA, "W3 那一项的 uuid 就是凭据里的 uuid");
        check(AccountStore::nameOf(uuidA) == QStringLiteral("Alice"),
              "W3 名字是从凭据自己身上读的", AccountStore::nameOf(uuidA));
        check(AccountStore::activeUuid() == uuidA, "W3 当前账号就是刚收进来的这个");
        check(AccountStore::contains(uuidA), "W3 contains() 认得出它");
        check(accountAt(list, 0).isCurrent, "W3 而且标着「当前」");
        check(!accountAt(list, 0).savedAt.isEmpty(),
              "W3 存进来的时刻有值（界面要显示「存于 …」）", accountAt(list, 0).savedAt);

        SavedAccount found;
        check(AccountStore::find(uuidA, &found) && found.uuid == uuidA,
              "W3 find() 按 uuid 找得到");
        check(AccountStore::find(uuidA, &found) && found.isCurrent,
              "W3 find() 也带着「当前」标记");
    }

    // ---- W4 同一个账号再收一次 → 就地更新，不是追加 ----
    // 令牌换新了、用户改了名字，都还是同一个人。重复追加的症状是列表里出现两条同名项，
    // 而点哪一条都"切得过去"，下次查起来很绕。
    {
        const int before = AccountStore::accounts().size();
        check(TokenStore::writeJson(TokenStore::minecraftKey(),
                                    stubMinecraftPayload(uuidA, QStringLiteral("Alice2"))),
              "W4 前提：换一份名字不同的凭据进活动槽");
        QString error;
        check(AccountStore::captureActive(&error), "W4 同一账号再收一次成功", error);
        const QList<SavedAccount> list = AccountStore::accounts();
        check(list.size() == before, "W4 列表条数没变（就地更新，不是追加）",
              QString("%1 -> %2").arg(before).arg(list.size()));
        check(AccountStore::nameOf(uuidA) == QStringLiteral("Alice2"),
              "W4 名字更新成了凭据里的新名字", AccountStore::nameOf(uuidA));
        check(accountAt(list, 0).isCurrent, "W4 更新之后「当前」标记还在它身上");
    }

    // ---- W5 第二个账号：追加到末尾，「当前」标记跟着活动槽走 ----
    {
        putActiveCredentials(QStringLiteral("W5"), uuidB, QStringLiteral("Bob"),
                             QStringLiteral("rt-B"));
        QString error;
        check(AccountStore::captureActive(&error), "W5 第二个账号也收得进来", error);

        const QList<SavedAccount> list = AccountStore::accounts();
        check(list.size() == 2, "W5 列表里有两条", QString::number(list.size()));
        check(accountAt(list, 0).uuid == uuidA && accountAt(list, 1).uuid == uuidB,
              "W5 新来的追加在末尾（顺序 = 收进来的先后）");
        check(!accountAt(list, 0).isCurrent && accountAt(list, 1).isCurrent,
              "W5 「当前」是**现算**的，不是存下来的字段");
    }

    // ---- W6 切换：活动槽的**字节**真的被换成了列表里那一份 ----
    // 这一条是整节的核心。切换不是"记一个 uuid 就完事"，而是把那个账号的两份凭据
    // 写回活动槽 —— 因为启动游戏、刷新令牌读的全是活动槽（AuthManager 一个字没改）。
    {
        QString error;
        check(AccountStore::activate(uuidA, &error), "W6 切到 Alice", error);
        check(AccountStore::activeUuid() == uuidA, "W6 当前账号换成了 Alice");

        const QJsonObject mc = TokenStore::readJson(TokenStore::minecraftKey());
        check(mc.value(QStringLiteral("name")).toString() == QStringLiteral("Alice2"),
              "W6 活动槽里 Minecraft 那份是 Alice 的**副本**（连 W4 更新过的名字一起）",
              mc.value(QStringLiteral("name")).toString());
        const QJsonObject ms = TokenStore::readJson(TokenStore::microsoftKey());
        // 切到 A 之前活动槽里放的是 **B** 的微软凭据（W5 写进去的）。这里必须是 A 的
        // rt-A —— 写回来的是"列表里收着的那一份"，不是"上一个账号留下的那一份"。
        check(ms.value(QStringLiteral("refresh_token")).toString() == QStringLiteral("rt-A"),
              "W6 微软那份也来自列表里的副本，不是上一个账号留下的",
              ms.value(QStringLiteral("refresh_token")).toString());

        const QList<SavedAccount> list = AccountStore::accounts();
        check(accountAt(list, 0).isCurrent && !accountAt(list, 1).isCurrent,
              "W6 列表上的「当前」标记跟着翻过来了");
    }

    // ---- W7 切回去：副本一个字段都没丢 ----
    {
        QString error;
        check(AccountStore::activate(uuidB, &error), "W7 切回 Bob", error);
        const QJsonObject ms = TokenStore::readJson(TokenStore::microsoftKey());
        const QJsonObject mc = TokenStore::readJson(TokenStore::minecraftKey());
        check(ms.value(QStringLiteral("refresh_token")).toString() == QStringLiteral("rt-B"),
              "W7 微软那份换成了 Bob 的（说明上一趟把 Alice 的副本完整留在列表里了）");
        check(mc.value(QStringLiteral("name")).toString() == QStringLiteral("Bob") &&
                  mc.value(QStringLiteral("uuid")).toString() == uuidB,
              "W7 Minecraft 那份也对");
    }

    // ---- W8 切不存在的 / 空 uuid → 失败，而且活动槽**不许被动过** ----
    // 失败时动了一半活动槽是最坏的下场：用户看着还是原来那个人，盘上已经是别人的令牌了。
    {
        QString error;
        check(!AccountStore::activate(uuidA.left(30), &error),
              "W8 切一个不在列表里的 uuid → 失败");
        check(!error.isEmpty(), "W8 而且说得出是「不在列表里」", error);
        check(AccountStore::activeUuid() == uuidB,
              "W8 失败的时候活动槽没被动过（还在 Bob）", AccountStore::activeUuid());
        check(!AccountStore::activate(QString(), &error), "W8 空 uuid 也失败");
    }

    // ---- W9 删非当前账号：列表少一条，活动槽不动 ----
    {
        check(AccountStore::remove(uuidA), "W9 删掉 Alice");
        check(!AccountStore::contains(uuidA), "W9 它从列表里没了");
        check(AccountStore::accounts().size() == 1, "W9 列表里剩一条");
        check(AccountStore::activeUuid() == uuidB,
              "W9 删的不是当前账号 → 活动槽原封不动（还登录着 Bob）",
              AccountStore::activeUuid());
        check(TokenStore::hasValue(TokenStore::minecraftKey()),
              "W9 活动槽里那份凭据也还在（删别人不该把我踢下线）");
        check(!AccountStore::remove(uuidA), "W9 再删一次 → 失败（列表里本来就没有它）");
    }

    // ---- W10 删**当前**账号 = 退出登录 ----
    // 留着会是"当前账号不在列表里"这种说得通但没人想要的状态，所以连活动槽一起清空。
    {
        check(AccountStore::remove(uuidB), "W10 删掉当前账号 Bob");
        check(AccountStore::accounts().isEmpty(), "W10 列表空了");
        check(AccountStore::activeUuid().isEmpty(), "W10 当前账号也空了（= 退出登录）");
        check(!TokenStore::hasValue(TokenStore::microsoftKey()) &&
                  !TokenStore::hasValue(TokenStore::minecraftKey()),
              "W10 活动槽那两份字节是**真的删掉了**，不是写成空值");
        check(!QSettings().contains(TokenStore::currentUuidKey()),
              "W10 CurrentAccountUuid 也一并清掉");
    }

    // ---- W10b 登出：只清登录状态，**账号留在列表里** ----
    //
    // 和 W10（删当前账号）只差一条：列表里那一项**不动**。所以这一节和 W10 一样要钉住
    // "盘上确实没登录状态了"，另外**必须**再钉一条"账号还在" —— 少了后一条，这个函数就
    // 退化成 W10 了，而它俩在界面上是两个按钮（「登出」和「删除所选」）。
    {
        putActiveCredentials(QStringLiteral("W10b"), uuidA, QStringLiteral("Alice"),
                             QStringLiteral("rt-A"));
        check(AccountStore::captureActive(), "W10b 先把 Alice 收进列表");
        putActiveCredentials(QStringLiteral("W10b"), uuidB, QStringLiteral("Bob"),
                             QStringLiteral("rt-B"));
        check(AccountStore::captureActive(), "W10b 再把 Bob 收进列表");
        check(AccountStore::activeUuid() == uuidB, "W10b 前提：当前账号是 Bob");

        check(AccountStore::logout(), "W10b 登出");
        check(AccountStore::activeUuid().isEmpty(), "W10b 盘上没有当前账号了（是「没登录」）");
        check(!TokenStore::hasValue(TokenStore::microsoftKey()) &&
                  !TokenStore::hasValue(TokenStore::minecraftKey()),
              "W10b 活动槽那两份凭据是**真的删掉了**，不是写成空值");
        // 这一句必须清：activeUuid() 第二档只看 CurrentAccountUuid 是不是指向一个**离线**
        // 账号 —— 登出的是离线账号时不清它，盘上就还是"当前是某某"。
        check(!QSettings().contains(TokenStore::currentUuidKey()),
              "W10b CurrentAccountUuid 也一并清掉");
        check(AccountStore::accounts().size() == 2 && AccountStore::contains(uuidA) &&
                  AccountStore::contains(uuidB),
              "W10b **两个账号都还在列表里**（这一条才是「登出」和「删除」的分界）",
              QString::number(AccountStore::accounts().size()));
        check(!accountAt(AccountStore::accounts(), 0).isCurrent &&
                  !accountAt(AccountStore::accounts(), 1).isCurrent,
              "W10b 列表上那个「当前」标记跟着没了");

        // 本来就没登录：返回 false。**顺带钉住盘上的东西一个字节没动** —— 清一遍空槽是
        // 空操作，但要是它返回 true，界面就会说一句"已退出登录"，而那句话是假的。
        check(!AccountStore::logout(), "W10b 再登出一次 → 失败（本来就没登录）");
        check(AccountStore::accounts().size() == 2,
              "W10b 那一下没把列表动掉", QString::number(AccountStore::accounts().size()));

        // 回来：这才是"登出可逆"的判据 —— 副本里该有的字段一个都没少。
        QString error;
        check(AccountStore::activate(uuidB, &error), "W10b 切回 Bob（登出之后还能回来）", error);
        check(AccountStore::activeUuid() == uuidB, "W10b 当前账号又是 Bob 了");
        const QJsonObject ms = TokenStore::readJson(TokenStore::microsoftKey());
        check(ms.value(QStringLiteral("refresh_token")).toString() == QStringLiteral("rt-B"),
              "W10b 回来的那份副本是完整的（refresh_token 还在）",
              ms.value(QStringLiteral("refresh_token")).toString());

        // 收尾：还原成 W10 结束时的样子（列表空 + 活动槽空）—— W11 的前提就是这两条，
        // 不还原的话它那句「活动槽是空的 → 谁都不标当前」会因为这儿还登录着 Bob 而红，
        // 而那不是 W11 的错。
        check(AccountStore::remove(uuidA) && AccountStore::remove(uuidB),
              "W10b 收尾：把这一节造的两个账号删干净");
        check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
              "W10b 收尾：列表和活动槽都回到空（W11 的前提）");
    }

    // ---- R8 登出时列表读不出来：不许把活动槽里唯一那份 refresh_token 删掉 ----
    //
    // 现场：活动槽里是一份**完整**的在线凭据（有 refresh_token），而 `SavedAccounts`
    // 那一格解不开（换 Windows 账户 / 换机器之后就是这样）。旧行为是 qWarning 一句就
    // 照常清活动槽 —— 盘上再没有第二份，用户只能重走浏览器登录，而界面上还会说
    // "账号还留在列表里，点一下就能回来"。
    {
        putActiveCredentials(QStringLiteral("R8"), uuidA, QStringLiteral("Alice"),
                             QStringLiteral("rt-A"));
        {
            // 直接往那一格写一串解不开的字节（不是 TokenStore::writeJson —— 那会写成
            // 一份合法列表，这一条要的正好是"读不出来"）。
            QSettings settings;
            settings.setValue(TokenStore::accountsKey(), QByteArrayLiteral("@@@@@@ 坏掉了"));
            settings.sync();
        }
        check(!AccountStore::logout(),
              "★ R8 列表读不出来、而活动槽里是一份完整的凭据 → 这次登出被拒（不清活动槽）");
        const QJsonObject kept = TokenStore::readJson(TokenStore::microsoftKey());
        check(kept.value(QStringLiteral("refresh_token")).toString() == QStringLiteral("rt-A"),
              "★★ R8 活动槽里那份 refresh_token **还在**（盘上唯一的一份，清掉只能重走"
              "浏览器登录）",
              kept.value(QStringLiteral("refresh_token")).toString());
        check(TokenStore::hasValue(TokenStore::minecraftKey()) &&
                  AccountStore::activeUuid() == uuidA,
              "★ R8 而且当前账号没变（没有「看着登出了、其实没动」那种半截状态）",
              AccountStore::activeUuid());
        // 收尾：把这一节造的坏列表和活动槽一起清掉（W11 的前提就是两处都空）。
        TokenStore::clear(TokenStore::accountsKey());
        TokenStore::clear(TokenStore::microsoftKey());
        TokenStore::clear(TokenStore::minecraftKey());
        check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
              "R8 收尾：坏列表和活动槽都清了（W11 的前提）");
    }

    // ---- R13 清凭据写不进盘：登出不许谎报成功 ----
    //
    // 现场：另一份启动器正占着数据目录那把锁（`DirectorySessionLock`），这一份拿不到锁，
    // `TokenStore::clear()` 一个字节都清不掉。它以前是 void —— 只往日志里扔一句，而
    // `logout()` 无条件返回 true，界面于是说"已退出登录"，下次启动却还是那个账号。
    {
        putActiveCredentials(QStringLiteral("R13"), uuidA, QStringLiteral("Alice"),
                             QStringLiteral("rt-A"));
        check(AccountStore::captureActive(), "R13 前提：Alice 收进列表");

        const QString lockPath = QDir::temp().filePath(
            QStringLiteral("wtfml-directory-%1.lock")
                .arg(DirectorySessionLock::keyFor(AppPaths::defaultMinecraftDir())));
        QLockFile rival(lockPath);
        rival.setStaleLockTime(0);
        DirectorySessionLock::enabled = true;
        const bool rivalHolds = rival.tryLock(0);
        check(rivalHolds, "R13 前提：模拟另一份启动器占着数据目录那把锁");
        if (rivalHolds) {
            QString clearError;
            check(!TokenStore::clear(TokenStore::minecraftKey(), &clearError),
                  "★★ R13 拿不到目录锁时 clear() 返回 false（不再只是往日志里扔一句）",
                  clearError);
            check(TokenStore::hasValue(TokenStore::minecraftKey()),
                  "★ R13 而且那格字节确实还在（返回值说的是事实）");
            check(!AccountStore::logout(),
                  "★★ R13 清不掉时登出返回 false（界面那句「失败」才和盘上一致）");
            check(AccountStore::activeUuid() == uuidA,
                  "★ R13 当前账号没变（没有「看着登出了、其实没动」）",
                  AccountStore::activeUuid());
            rival.unlock();
        }
        DirectorySessionLock::enabled = false;
        // 收尾（锁已经放开）：清掉这一节造的东西，W11 的前提是两处都空。
        check(TokenStore::clear(TokenStore::accountsKey()) &&
                  TokenStore::clear(TokenStore::microsoftKey()) &&
                  TokenStore::clear(TokenStore::minecraftKey()),
              "R13 收尾：放开锁之后都清得掉");
        check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
              "R13 收尾：列表和活动槽都空了（W11 的前提）");
    }

    // ---- R15 切换账号第二步失败时的回滚材料：原样字节放得回去 ----
    //
    // 真正的失败路径（第一步写入成功、第二步写失败）没法在正常盘上造出来，所以这里钉的是
    // 回滚那一手本身：读出来的**原样字节**放回去之后，解出来还是原来那一份 —— 不是重新
    // 保护一遍、更不是写成一个空值。
    {
        putActiveCredentials(QStringLiteral("R15"), uuidA, QStringLiteral("Alice"),
                             QStringLiteral("rt-raw"));
        const QByteArray raw = TokenStore::rawValue(TokenStore::microsoftKey());
        check(!raw.isEmpty(), "R15 前提：rawValue() 读得到盘上那份原样字节");
        check(TokenStore::clear(TokenStore::microsoftKey()), "R15 先真的清掉那一格");
        check(!TokenStore::hasValue(TokenStore::microsoftKey()), "R15 清掉了");
        check(TokenStore::writeRaw(TokenStore::microsoftKey(), raw), "R15 原样放回去");
        check(TokenStore::readJson(TokenStore::microsoftKey())
                      .value(QStringLiteral("refresh_token"))
                      .toString() == QStringLiteral("rt-raw"),
              "★★ R15 放回去的还是解密得开的那一份（回滚真的发生，不是「重写一个空值」）");
        check(TokenStore::writeRaw(TokenStore::microsoftKey(), QByteArray()),
              "★ R15 空字节放回去 = 删掉那一格（原来就没有凭据时不凭空造一格空值）");
        check(!TokenStore::hasValue(TokenStore::microsoftKey()), "R15 那一格确实没了");
        TokenStore::clear(TokenStore::minecraftKey());
        check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
              "R15 收尾：列表和活动槽都空了（W11 的前提）");
    }

    // ---- W11 盘上有坏项 / 残项时的下场 ----
    // 手写一份列表（当"外部写入者"），验产品的读。两种坏法各一条：
    // - 一项连 uuid 都没有 → 列表里**不摆**（摆一个点不动的账号比少一项更难查）；
    // - 一项缺了微软那半边 → activate 拒掉，别把半份凭据写进活动槽。
    {
        const QString uuidBad = QStringLiteral("dddddddddddddddddddddddddddddddd");
        QJsonObject noUuid;
        noUuid.insert(QStringLiteral("name"), QStringLiteral("没有 uuid"));
        QJsonObject halfBaked;
        halfBaked.insert(QStringLiteral("uuid"), uuidBad);
        halfBaked.insert(QStringLiteral("name"), QStringLiteral("HalfBaked"));
        halfBaked.insert(QStringLiteral("minecraft"), stubMinecraftPayload(uuidBad, "HalfBaked"));
        // 微软那半边故意不给
        QJsonObject root;
        root.insert(QStringLiteral("accounts"), QJsonArray{noUuid, halfBaked});
        check(TokenStore::writeJson(TokenStore::accountsKey(), root),
              "W11 前提：手写一份带坏项的列表进沙盒");

        const QList<SavedAccount> list = AccountStore::accounts();
        check(list.size() == 1, "W11 没有 uuid 的那一项被跳过了", QString::number(list.size()));
        check(!list.isEmpty() && list.at(0).uuid == uuidBad, "W11 留下来的是有 uuid 的那一项");
        check(!list.isEmpty() && !list.at(0).isCurrent,
              "W11 活动槽是空的 → 谁都不标「当前」");

        QString error;
        check(!AccountStore::activate(uuidBad, &error),
              "W11 切一份缺了微软那半的凭据 → 失败");
        check(!error.isEmpty(), "W11 而且提示重新登录一次", error);
        check(AccountStore::activeUuid().isEmpty(),
              "W11 失败之后活动槽仍然是空的（没写进半份凭据）");
    }

    // ---- W12 离线 uuid 的推法（纯函数） ----
    // 期望值是**独立算出来**的（python -c 按 md5("OfflinePlayer:"+名字) 加版本位/变体位），
    // 不是拿被测代码自己跑一遍当答案 —— 那样只能证明"它和它自己一致"。
    {
        check(AccountStore::offlineUuidFor(QStringLiteral("Notch")) ==
                  QStringLiteral("b50ad385829d3141a2167e7d7539ba7f"),
              "W12 Notch 的离线 uuid 和游戏自己那套规则算出来的一致",
              AccountStore::offlineUuidFor(QStringLiteral("Notch")));
        check(AccountStore::offlineUuidFor(QStringLiteral("Steve")) ==
                  QStringLiteral("5627dd98e6be3c21b8a8e92344183641"),
              "W12 Steve 也是",
              AccountStore::offlineUuidFor(QStringLiteral("Steve")));
        check(AccountStore::offlineUuidFor(QStringLiteral("离线测试")) ==
                  QStringLiteral("97247186eee73f638958c7740bee3775"),
              "W12 非 ASCII 名字按 UTF-8 字节算（这一步最容易写错成 QString::toLocal8Bit）",
              AccountStore::offlineUuidFor(QStringLiteral("离线测试")));

        const QString uuid = AccountStore::offlineUuidFor(QStringLiteral("Notch"));
        check(uuid.size() == 32, "W12 是 32 位不带横线的那种", QString::number(uuid.size()));
        check(uuid == uuid.toLower(), "W12 全小写（和档案里给的 profile.id 一个形态）");
        // 版本位/变体位：md5 的直接输出不是合法 uuid，得按 RFC 4122 摆过才和游戏一致。
        // 少了这一步，值看着**一样是 32 位十六进制**，只是永远对不上 —— 不报错的那种错。
        check(uuid.at(12) == QLatin1Char('3'), "W12 版本位是 3（RFC 4122 的 v3）",
              QString(uuid.at(12)));
        check(QStringLiteral("89ab").contains(uuid.at(16)),
              "W12 变体位是 RFC 4122 那一档（8/9/a/b）", QString(uuid.at(16)));
        check(AccountStore::offlineUuidFor(QStringLiteral("Notch")) == uuid,
              "W12 同一个名字推两次是同一个值（否则每次开窗都是新身份）");
        check(AccountStore::offlineUuidFor(QStringLiteral("notch")) != uuid,
              "W12 大小写算两个名字（游戏那边也这么算）");
    }

    // ---- W13 名字不合法的那几条：一条都不许进列表，也不许把人踢下线 ----
    // 现场：W11 那份手写列表里还剩一条（HalfBaked），再收一个正常的在线账号进来。
    {
        putActiveCredentials(QStringLiteral("W13"), uuidA, QStringLiteral("Alice"),
                             QStringLiteral("rt-A"));
        check(AccountStore::captureActive(), "W13 前提：收一个在线账号 Alice 进列表");
        const int before = AccountStore::accounts().size();

        const QStringList bad = {QString(),
                                 QStringLiteral("   "),
                                 QStringLiteral("有 空格"),
                                 QStringLiteral("带\t制表符"),
                                 QString(17, QLatin1Char('x')),
                                 // 破坏性：中文超长 / 控制字符 / emoji（每个算 2 个 UTF-16 单元）
                                 QString(17, QChar(0x5F20)),   // 17 个「张」
                                 QStringLiteral("带\u0001控制"),
                                 QStringLiteral("😀😀😀😀😀😀😀😀😀")};   // 9 个 emoji = 18 单元
        for (const QString &name : bad) {
            QString error;
            check(!AccountStore::addOffline(name, &error),
                  QStringLiteral("W13 「%1」加不进去（%2 个字符）")
                      .arg(name, QString::number(name.size())));
            check(!error.isEmpty(), "W13 而且给了原因，不是闷着失败", error);
        }
        check(AccountStore::accounts().size() == before,
              "W13 那些失败一条都没进列表",
              QString::number(AccountStore::accounts().size()));
        check(AccountStore::activeUuid() == uuidA,
              "W13 失败之后当前账号还是 Alice（加离线账号失败不许把人踢下线）",
              AccountStore::activeUuid());
        check(TokenStore::hasValue(TokenStore::minecraftKey()),
              "W13 活动槽里那份凭据也还在");
    }

    // ---- W13b 破坏性名字的纯函数面：中文/emoji 的离线 UUID 稳定且不与 ASCII 撞 ----
    {
        const QString chinese = QStringLiteral("史蒂夫");
        const QString chinese2 = QStringLiteral("史蒂芬");
        const QString ascii = QStringLiteral("Steve");
        check(!AccountStore::offlineUuidFor(chinese).isEmpty(),
              "W13b 中文名算得出离线 UUID");
        check(AccountStore::offlineUuidFor(chinese) == AccountStore::offlineUuidFor(chinese),
              "W13b 同一个中文名两次算出同一个 UUID");
        check(AccountStore::offlineUuidFor(chinese) != AccountStore::offlineUuidFor(chinese2),
              "W13b 不同中文名不会撞 UUID");
        check(AccountStore::offlineUuidFor(chinese) != AccountStore::offlineUuidFor(ascii),
              "W13b 中文名和 ASCII 名不会撞");
        check(AccountStore::offlineUuidFor(QStringLiteral("😀😀")).size() == 32,
              "W13b emoji 名也给 32 位十六进制 UUID");
    }

    // ---- W14 加一条离线账号：进列表、成当前账号、活动槽清空 ----
    {
        const QString offlineUuid = AccountStore::offlineUuidFor(QStringLiteral("Steve"));
        const int before = AccountStore::accounts().size();

        QString error;
        check(AccountStore::addOffline(QStringLiteral("Steve"), &error),
              "W14 Steve 加得进去", error);
        check(AccountStore::contains(offlineUuid), "W14 列表里认得出它");
        check(AccountStore::nameOf(offlineUuid) == QStringLiteral("Steve"),
              "W14 名字就是它", AccountStore::nameOf(offlineUuid));
        check(AccountStore::accounts().size() == before + 1, "W14 只多了一条",
              QString::number(AccountStore::accounts().size()));

        SavedAccount found;
        check(AccountStore::find(offlineUuid, &found) && found.offline,
              "W14 这一项带着「离线」那个标志（界面靠它挂「（离线）」）");
        check(AccountStore::find(offlineUuid, &found) && found.isCurrent,
              "W14 而且它就是当前账号（加完顺手切过去）");
        check(!AccountStore::isOffline(uuidA), "W14 在线那条不会被认成离线账号");

        // 盘上那一项**真的没有**凭据副本 —— 这是"离线账号"这句话的字面意思。
        // 走原始 JSON 看（SavedAccount 里不带副本，从 find() 看不出来）。
        QJsonArray raw = TokenStore::readJson(TokenStore::accountsKey())
                             .value(QStringLiteral("accounts"))
                             .toArray();
        bool sawOffline = false;
        for (const QJsonValue &value : raw) {
            const QJsonObject item = value.toObject();
            if (item.value(QStringLiteral("uuid")).toString() != offlineUuid) {
                continue;
            }
            sawOffline = true;
            check(item.value(QStringLiteral("offline")).toBool(),
                  "W14 盘上写着 offline = true");
            check(!item.contains(QStringLiteral("microsoft")) &&
                      !item.contains(QStringLiteral("minecraft")),
                  "W14 这一项里一个凭据字节都没有（离线账号就是只有名字）");
        }
        check(sawOffline, "W14 原始列表里确实有这一项（上面那两条才有东西可验）");

        // 加完 = 离开在线身份：活动槽两份都空掉。
        check(!TokenStore::hasValue(TokenStore::microsoftKey()) &&
                  !TokenStore::hasValue(TokenStore::minecraftKey()),
              "W14 活动槽被清空了（离线身份没有凭据那一半）");
        // 这一条钉的就是 activeUuid() 的这个点：活动槽空着、而当前是离线账号。
        check(AccountStore::activeUuid() == offlineUuid,
              "W14 活动槽空着也认得出当前是谁（离线账号的第二档）",
              AccountStore::activeUuid());
        // 但那个在线账号**没丢**：它的副本还在列表里，点一下就回来（下一节验）。
        check(AccountStore::contains(uuidA), "W14 原来那个在线账号还在列表里，没被顶掉");

        // 同名再加一次 = 就地更新，不出现第二条 Steve。
        const int afterFirst = AccountStore::accounts().size();
        check(AccountStore::addOffline(QStringLiteral("Steve"), &error),
              "W14 同一个名字再加一次也成功", error);
        check(AccountStore::accounts().size() == afterFirst,
              "W14 但列表条数没变（同一个名字 = 同一个人）",
              QString::number(AccountStore::accounts().size()));
    }

    // ---- W15 在线 ↔ 离线来回切 ----
    {
        const QString offlineUuid = AccountStore::offlineUuidFor(QStringLiteral("Steve"));

        // 切回在线：凭据被写回活动槽，当前账号跟着换。
        QString error;
        check(AccountStore::activate(uuidA, &error),
              "W15 切回在线账号 Alice", error);
        check(AccountStore::activeUuid() == uuidA, "W15 当前账号换成 Alice",
              AccountStore::activeUuid());
        check(TokenStore::hasValue(TokenStore::microsoftKey()) &&
                  TokenStore::hasValue(TokenStore::minecraftKey()),
              "W15 活动槽那两份凭据都回来了");
        check(!AccountStore::isOffline(uuidA), "W15 切回来之后它不是离线账号");

        // 再切到离线：这一档一个凭据字节都不碰，也不会失败。
        check(AccountStore::activate(offlineUuid, &error), "W15 再切到离线账号 Steve", error);
        check(AccountStore::activeUuid() == offlineUuid, "W15 当前账号换成 Steve",
              AccountStore::activeUuid());
        check(!TokenStore::hasValue(TokenStore::minecraftKey()),
              "W15 活动槽又被清空了");

        // ① 活动槽空着 + 记录键指向一个**在线**账号 → 必须是"没登录"。
        // 不卡这一条的话，删掉当前账号之后界面还会说"当前是某某"（记录键上写着它）。
        TokenStore::clear(TokenStore::microsoftKey());
        TokenStore::clear(TokenStore::minecraftKey());
        QSettings settings;
        settings.setValue(TokenStore::currentUuidKey(), uuidA);
        settings.sync();
        check(AccountStore::activeUuid().isEmpty(),
              "W15 活动槽空着时，记录键指向在线账号**不算**当前（= 没登录）",
              AccountStore::activeUuid());

        // ② 记录键指向一个**不在列表里**的 uuid → 也是"没登录"（孤儿键）。
        settings.setValue(TokenStore::currentUuidKey(),
                          QStringLiteral("cccccccccccccccccccccccccccccccc"));
        settings.sync();
        check(AccountStore::activeUuid().isEmpty(),
              "W15 记录键指向列表里没有的账号 → 也是「没登录」（孤儿键不算数）",
              AccountStore::activeUuid());

        // 收拾回"当前是离线账号"，顺便验 remove()。
        check(AccountStore::activate(offlineUuid, &error) &&
                  AccountStore::activeUuid() == offlineUuid,
              "W15 切回离线账号（上面两条钉子之后盘上还是能恢复的）");
        check(AccountStore::remove(offlineUuid), "W15 删掉离线账号 Steve");
        check(!AccountStore::contains(offlineUuid), "W15 列表里没了");
        check(AccountStore::activeUuid().isEmpty(),
              "W15 删的是当前账号 → 当前也空了（离线账号同样适用这条）",
              AccountStore::activeUuid());
        check(!QSettings().contains(TokenStore::currentUuidKey()),
              "W15 记录键也一并清掉，不留孤儿");
    }

    // ---- W16 边界：16 个字符收得下（17 个在 W13 已经拒了） ----
    {
        const QString name = QString(16, QLatin1Char('y'));
        QString error;
        check(AccountStore::addOffline(name, &error),
              "W16 16 个字符是收的（上限就是 16）", error);
        check(AccountStore::isOffline(AccountStore::offlineUuidFor(name)),
              "W16 而且它确实是一条离线账号");
        check(AccountStore::remove(AccountStore::offlineUuidFor(name)),
              "W16 收尾：把它删掉");
    }

    QSettings::setDefaultFormat(QSettings::NativeFormat);
    QDir(settingsDir).removeRecursively();

    // ============================================================================
    // X 节：皮肤/披风那几条接口的纯函数。全是纯函数 —— 不联网、不碰盘，也不碰上面的沙盒。
    // ============================================================================
    section("X. 皮肤/披风接口的纯函数（解析 / variant / multipart / 话术）");

    const QString skinClassic = QStringLiteral("https://textures.minecraft.net/texture/aaa");
    const QString skinSlim = QStringLiteral("https://textures.minecraft.net/texture/bbb");
    const QString capeUrl = QStringLiteral("https://textures.minecraft.net/texture/ccc");

    // ---- X1 解当前皮肤：有 ACTIVE 就用 ACTIVE，没有才退回第一件能用的 ----
    {
        const QByteArray twoSkins =
            QStringLiteral(R"J({"skins":[)J"
                           R"J({"id":"1","state":"INACTIVE","url":"%1","variant":"CLASSIC"},)J"
                           R"J({"id":"2","state":"ACTIVE","url":"%2","variant":"SLIM"}]})J")
                .arg(skinClassic, skinSlim)
                .toUtf8();
        const SkinSummary chosen = SkinApi::parseCurrentSkin(twoSkins);
        check(chosen.url == skinSlim, "X1 有 ACTIVE 就选 ACTIVE 那一件（哪怕它排在后面）",
              chosen.url);
        check(chosen.variant == QStringLiteral("slim"),
              "X1 顺带把小写形态也解出来了（读回来的是大写 SLIM）", chosen.variant);
        check(!chosen.isEmpty(), "X1 解出来的不是空的");

        // 旧档案里 state 可能整个字段都没有。那时退回第一件**有 url 的**。
        const QByteArray noState =
            QStringLiteral(R"J({"skins":[{"url":"%1"},{"url":"%2"}]})J")
                .arg(skinClassic, skinSlim)
                .toUtf8();
        check(SkinApi::parseCurrentSkin(noState).url == skinClassic,
              "X1 没有 state 字段 → 退回第一件");

        // 没有 url 的项不能"占住"退回的位置 —— 它连图都下不了。
        const QByteArray blankFirst =
            QStringLiteral(R"J({"skins":[{"id":"1"},{"url":"%1"}]})J").arg(skinSlim).toUtf8();
        check(SkinApi::parseCurrentSkin(blankFirst).url == skinSlim,
              "X1 url 为空的项被跳过，退回的是第一件真的有 url 的");

        check(SkinApi::parseCurrentSkin(QByteArrayLiteral(R"J({"skins":[]})J")).isEmpty(),
              "X1 skins 是空数组 → 空摘要（= 这份档案没有皮肤信息）");
        check(SkinApi::parseCurrentSkin(QByteArrayLiteral("<html>404</html>")).isEmpty(),
              "X1 body 根本不是 JSON → 空摘要（不能把一页 HTML 当皮肤）");
        check(SkinApi::parseCurrentSkin(QByteArray()).isEmpty(), "X1 空 body → 空摘要");
    }

    // ---- X2 variant 只有 slim 认 slim ----
    // 协议本身的默认值是 classic：档案里没有 variant 字段就是经典模型。所以"认不出来"
    // 必须落到 classic，**不能**落到 slim（那会让所有人的模型悄悄变成 Alex）。
    {
        check(SkinApi::normalizeVariant(QStringLiteral("SLIM")) == QStringLiteral("slim"),
              "X2 读回来的大写 SLIM → slim");
        check(SkinApi::normalizeVariant(QStringLiteral("slim")) == QStringLiteral("slim"),
              "X2 已经是小写的 slim → slim");
        check(SkinApi::normalizeVariant(QStringLiteral("Slim")) == QStringLiteral("slim"),
              "X2 大小写混着写也认");
        check(SkinApi::normalizeVariant(QStringLiteral("CLASSIC")) == QStringLiteral("classic"),
              "X2 CLASSIC → classic");
        check(SkinApi::normalizeVariant(QString()) == QStringLiteral("classic"),
              "X2 空串（档案里没这个字段）→ classic，这是协议默认值");
        check(SkinApi::normalizeVariant(QStringLiteral("Cape")) == QStringLiteral("classic"),
              "X2 认不出来的值 → classic，不回落到 slim");
    }

    // ---- X3 解披风列表 ----
    {
        const QByteArray body =
            QStringLiteral(R"J({"capes":[)J"
                           R"J({"id":"c1","alias":"Migrator","url":"%1","state":"ACTIVE"},)J"
                           R"J({"id":"c2","alias":"MineCon 2016","url":"%2","state":"inactive"},)J"
                           R"J({"id":"c3"})J"
                           R"J(]})J")
                .arg(capeUrl, capeUrl)
                .toUtf8();
        const QList<OwnedCape> capes = SkinApi::parseCapes(body);
        check(capes.size() == 3, "X3 三件都解出来了", QString::number(capes.size()));
        const auto capeAt = [&capes](int i) {
            return i >= 0 && i < capes.size() ? capes.at(i) : OwnedCape();
        };
        check(capeAt(0).id == QStringLiteral("c1") && capeAt(0).alias == QStringLiteral("Migrator"),
              "X3 id 和显示名都解出来了");
        check(capeAt(0).active, "X3 state=ACTIVE → 标成当前披风");
        check(!capeAt(1).active, "X3 state=inactive（小写）→ 不是当前披风");
        check(capeAt(2).alias == QStringLiteral("c3"),
              "X3 没带 alias → 退回 id（列表上不能显示成空白）", capeAt(2).alias);
        check(capeAt(2).url.isEmpty() && !capeAt(2).active,
              "X3 只有 id 的项照样摆出来（能点，只是没缩略图）");

        // 没有 id 的项**丢掉**：PUT 要的就是 id，摆一件点了没反应的披风不如不摆。
        const QByteArray noId =
            QStringLiteral(R"J({"capes":[{"alias":"没有 id","url":"%1","state":"ACTIVE"}]})J")
                .arg(capeUrl)
                .toUtf8();
        check(SkinApi::parseCapes(noId).isEmpty(), "X3 没有 id 的项被丢掉（点了也设不成当前）");

        check(SkinApi::parseCapes(QByteArrayLiteral(R"J({"capes":[]})J")).isEmpty(),
              "X3 一件披风都没有 → 空列表（常态化，不是错误）");
        check(SkinApi::parseCapes(QByteArrayLiteral("not json")).isEmpty(),
              "X3 body 不是 JSON → 空列表，不抛");
        check(SkinApi::parseCapes(QByteArray()).isEmpty(), "X3 空 body → 空列表");

        // **裸数组**那种形状（没有外层那个 "capes" 键）：旧的 `api.mojang.com` 那条查披风
        // 的路、以及别的启动器从 OpenAPI 那边看到的就是它。上面几条喂的全是"外层是对象"
        // 那份（**产品走的就是这份**，见 X7），只有这一组钉得住另一种。
        // NOTE: 产品**不发**一条独立的"列披风"请求 —— 那条路由不存在（ 的 404）。
        const QByteArray bareCapes =
            QStringLiteral(R"J([{"id":"c1","alias":"Migrator","url":"%1","state":"ACTIVE"},)J"
                           R"J({"id":"c2","alias":"MineCon 2016","url":"%2","state":"INACTIVE"}])J")
                .arg(capeUrl, capeUrl)
                .toUtf8();
        const QList<OwnedCape> fromBare = SkinApi::parseCapes(bareCapes);
        check(fromBare.size() == 2,
              "X3 裸数组（/capes 那种形状）也解得出来 —— 只认一种的话这里会是 0",
              QString::number(fromBare.size()));
        if (fromBare.size() == 2) {
            check(fromBare.at(0).id == QStringLiteral("c1") &&
                      fromBare.at(0).alias == QStringLiteral("Migrator") && fromBare.at(0).active,
                  "X3 裸数组里 ACTIVE 那件照样认成当前披风、alias 照样读出来");
            check(fromBare.at(1).url == capeUrl && !fromBare.at(1).active,
                  "X3 裸数组里的 url 也读出来了（INACTIVE 那件不是当前）");
        }
        check(SkinApi::parseCapes(QByteArrayLiteral("[]")).isEmpty(),
              "X3 裸空数组 → 空列表（和「解不出来」分不开，但至少不抛）");
        check(SkinApi::parseCapes(QByteArrayLiteral(R"J({"capes":null})J")).isEmpty(),
              "X3 capes 显式是 null → 空列表，不抛");
    }

    // ---- X4 皮肤尺寸只有 64×64 和 64×32 两种 ----
    // 老格式（64×32）到现在还有人在用，所以两种都收。判据只看宽高：官方那边也校验，
    // 但它在传上去之后才回一句英文 400，用户白等一圈。
    {
        check(SkinApi::isAcceptableSkinSize(64, 64), "X4 64×64（新格式）收");
        check(SkinApi::isAcceptableSkinSize(64, 32), "X4 64×32（老格式）也收");
        check(!SkinApi::isAcceptableSkinSize(128, 128), "X4 128×128 不收（那是高清皮肤，官方不收）");
        check(!SkinApi::isAcceptableSkinSize(64, 128), "X4 64×128 不收");
        check(!SkinApi::isAcceptableSkinSize(128, 64), "X4 128×64 不收");
        check(!SkinApi::isAcceptableSkinSize(63, 64), "X4 63×64 不收");
        check(!SkinApi::isAcceptableSkinSize(64, 31), "X4 64×31 不收");
        check(!SkinApi::isAcceptableSkinSize(0, 0), "X4 0×0 不收（读不出来的图）");
    }

    // ---- X5 multipart 请求体 ----
    {
        const QByteArray boundary = QByteArrayLiteral("----WTFMLBoundary1234");
        // 故意在 PNG 字节里塞 CRLF 和一个 NUL：手写 multipart 最容易折的就是这种字节
        // （按文本处理会把它们改成别的，官方收到一张坏图、回一句看不懂的错）。
        QByteArray png;
        png.append(QByteArrayLiteral("\x89PNG\r\n\x1a\n"));
        png.append(QByteArrayLiteral("\x00\x00\x00\x0dIHDR"));
        png.append(QByteArrayLiteral("\r\n\x00\xff\xfe"));

        const QByteArray body = SkinApi::skinUploadBody(png, QStringLiteral("SLIM"), boundary);
        check(body.contains(png), "X5 皮肤字节**原样**在请求体里（CRLF 和 NUL 都没被折）");
        check(body.contains(QByteArrayLiteral("name=\"variant\"\r\n\r\nslim\r\n")),
              "X5 variant 段写的是**小写**（大写进来也小写出去）",
              QString::fromLatin1(body.left(200)));
        check(body.contains(QByteArrayLiteral("name=\"file\"; filename=\"skin.png\"")),
              "X5 file 那一段带着文件名");
        check(body.contains(QByteArrayLiteral("Content-Type: image/png\r\n\r\n")),
              "X5 file 那一段写明了 Content-Type（不写官方会当文本收）");
        // 两段各自的起始 + 结尾那一条，一共三条边界。
        check(body.count(boundary) == 3, "X5 三条 boundary（两段开头 + 结尾一条）",
              QString::number(body.count(boundary)));
        check(body.startsWith(QByteArrayLiteral("--") + boundary + QByteArrayLiteral("\r\n")),
              "X5 请求体以第一段的 boundary 开头");
        check(body.endsWith(QByteArrayLiteral("--") + boundary + QByteArrayLiteral("--\r\n")),
              "X5 以结尾那条边界收尾（少了官方解不出最后一段）");
        // 段落之间是**空行**（\r\n\r\n）：头部和正文之间必须有一个，少一个整段就废了。
        check(body.contains(QByteArrayLiteral("variant\"\r\n\r\n")) &&
                  body.contains(QByteArrayLiteral("image/png\r\n\r\n")),
              "X5 头部和正文之间都有那个空行");

        check(SkinApi::skinUploadContentType(boundary) ==
                  QByteArrayLiteral("multipart/form-data; boundary=") + boundary,
              "X5 Content-Type 里带着同一个 boundary");
    }

    // ---- X6 失败话术（401 / 403 / 404 / 别的，各是各的行动） ----
    {
        const QString what = QStringLiteral("上传皮肤失败");

        const QString expired = SkinApi::failureText(httpReply(401, QByteArray()), what);
        check(expired.contains(what), "X6 401 那句话带着「在做什么」", expired);
        check(expired.contains(QStringLiteral("重新登录")),
              "X6 401 → 说清是登录态那件事（用户的动作是重新登录）", expired);

        // 403 有两种：应用没被批准（要提交申请）和权限不够。这里验的是**后者** ——
        // 正文里没有那句 "Invalid app registration" 时不能瞎指路。
        const QString forbidden = SkinApi::failureText(httpReply(403, QByteArray()), what);
        check(forbidden.contains(QStringLiteral("403")), "X6 403（通用）→ 说清是 403", forbidden);
        check(!forbidden.contains(QStringLiteral("aka.ms")),
              "X6 403 通用那种**不许**指到「提交申请」（那是另一种 403）", forbidden);

        // 正文里带了那句 → 转成能照着做的那段话（和登录那条路同一个口径）。
        const QByteArray notApproved =
            QByteArrayLiteral(R"({"errorMessage":"Invalid app registration"})");
        const QString hint = SkinApi::failureText(httpReply(403, notApproved), what);
        check(hint.contains(QStringLiteral("aka.ms/mce-reviewappid")),
              "X6 403 +「Invalid app registration」→ 给出提交申请那个地址（和登录那条路一致）",
              hint);
        check(hint.contains(QStringLiteral("同一账号重新登录")),
              "X6 应说明应用获批后可用同一账号重新登录");

        const QString missing = SkinApi::failureText(httpReply(404, QByteArray()), what);
        check(missing.contains(QStringLiteral("未找到可修改的 Minecraft 档案")),
              "X6 404 → 说清是「这个账号没有档案」（比如没买游戏）", missing);

        // 429：限流。**不许**跟下面那条通用分支走 —— 通用分支认的正是正文里那个 `error`
        // 字段（Minecraft 服务 429 时它就是 "TOO_MANY_REQUESTS"），端给用户等于说了句
        // 他照做不了的话。 真机日志里那句
        // "读取披风失败（HTTP 429）：TOO_MANY_REQUESTS" 就是从这儿来的。
        const QByteArray tooMany = QByteArrayLiteral(
            R"({"path":"/minecraft/profile","errorType":"TOO_MANY_REQUESTS","error":"TOO_MANY_REQUESTS","status":429})");
        const QString throttled = SkinApi::failureText(httpReply(429, tooMany), what);
        check(throttled.contains(what) && throttled.contains(QStringLiteral("429")),
              "X6 429 → 说清哪一步、什么状态码（日志里那行要能对得上）", throttled);
        check(!throttled.contains(QStringLiteral("TOO_MANY_REQUESTS")),
              "X6 429 **不**把官方那个错误码端给用户（它一个字都没说该干什么）", throttled);
        check(throttled.contains(QStringLiteral("等待一两分钟")),
              "X6 429 → 下一步是等一会儿（不是重新登录、也不是换账号）", throttled);
        check(throttled.contains(QStringLiteral("请求频率过高")),
              "X6 429 → 说明服务器限制了请求频率", throttled);
        check(throttled == HttpClient::throttledText(what),
              "X6 429 那句话的唯一出处是 HttpClient::throttledText()（登录链说的是同一句）");

        const QByteArray badSize = QByteArrayLiteral(
            R"({"errorMessage":"Skin must be 64x64 or 64x32","path":"/minecraft/profile/skins","traceId":"abc"})");
        const QString detailed = SkinApi::failureText(httpReply(400, badSize), what);
        check(detailed.contains(QStringLiteral("Skin must be 64x64 or 64x32")),
              "X6 别的状态码 → 把官方那句原因带出来", detailed);
        check(!detailed.contains(QStringLiteral("traceId")) &&
                  !detailed.contains(QStringLiteral("/minecraft/profile/skins")),
              "X6 但只带原因，**不把正文里别的键（path / traceId）丢给用户看**", detailed);

        const QString bare = SkinApi::failureText(httpReply(500, QByteArray()), what);
        check(bare.contains(QStringLiteral("500")) && bare.contains(what),
              "X6 什么正文都没有 → 至少说清哪一步、什么状态码", bare);
        // 正文是 HTML（代理塞网页那种）时 bodyDetail 取不到字段，要走通用那条，不能空。
        check(!SkinApi::failureText(httpReply(502, QByteArrayLiteral("<html>502 Bad Gateway</html>")), what)
                   .isEmpty(),
              "X6 body 不是 JSON 也不会返回空串");

        // 断网 / DNS 解析不了 / 超时被 abort：**没有 HTTP 状态码**（HttpClient 给的初值
        // 是 -1）。这里不许报「读取披风失败（HTTP -1）。」，用户既不知道该去查网络，
        // 还看见一个不存在的状态码。
        const QString offline =
            SkinApi::failureText(httpReplyFailed(QStringLiteral("Host not found")), what);
        check(offline.contains(what), "X6 没连上 → 也要说清是哪一步在干什么", offline);
        check(offline.contains(QStringLiteral("Host not found")),
              "X6 没连上 → 把网络那句原因带出来（用户要拿它去查网络/代理）", offline);
        check(!offline.contains(QStringLiteral("-1")) &&
                  !offline.contains(QStringLiteral("HTTP")),
              "X6 没连上 **不许**端出一个不存在的状态码（HTTP -1）", offline);
    }

    // ---- X7 披风列表来自**档案**那条地址（没有一条独立的 /capes 路由） ----
    // 这一条钉的是一个**没有纯函数可验**的东西：请求打到哪个地址。披风列表就走档案
    // 那条地址；`GET /minecraft/profile/capes` **不存在**（对过 Mojang 的 OpenAPI：那一层
    // 只有 /capes/active 两个方法），别按"设披风、取消披风都在 /capes/active 下"对称猜出
    // 一条 /capes 路由来。
    {
        check(SkinApi::profilePath() == QStringLiteral("/minecraft/profile"),
              "X7 档案那条路径是 /minecraft/profile", SkinApi::profilePath());
        check(SkinApi::baseUrl() + SkinApi::profilePath() ==
                  QStringLiteral("https://api.minecraftservices.com/minecraft/profile"),
              "X7 于是「档案 / 皮肤当前状态 / 披风列表」是同一个地址",
              SkinApi::baseUrl() + SkinApi::profilePath());
        // 反面：它**不是** `/minecraft/profile/capes` 那种独立路由。这条写成这样，是为了让
        // "又猜了一条不存在的路由"有一句断言拦着，而不是等真机上再看到一个 404。
        check(!SkinApi::profilePath().endsWith(QStringLiteral("/capes")),
              "X7 而且它不是那条不存在的 /capes（404 的真因，别再加回来）",
              SkinApi::profilePath());
    }
}


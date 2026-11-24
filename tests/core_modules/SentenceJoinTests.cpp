#include "Support.hpp"

#include "util/SentenceJoin.hpp"

// 几个完整句子接成一段：中文直接相接，英文那一个空格放在 `SentenceJoin` 上下文的译文里。
//
// 测试钉的是"不吞句子、不重复填占位符"：测试进程钉 zh_CN，`%1%2` 没有英文译文，
// 所以这里看到的就是源文形态（两句之间什么都不加）。
void runSentenceJoinTests() {
    section("ZAJ. 句子相接：空表、单句、空串、句子里的 %1");

    check(joinSentences({}).isEmpty(), "ZAJ1 空列表 ⇒ 空串");
    check(joinSentences({QString()}).isEmpty(), "ZAJ1 只有一个空串 ⇒ 空串");
    check(joinSentences({QStringLiteral("甲。")}) == QStringLiteral("甲。"), "ZAJ1 单句原样返回");
    check(joinSentences({QStringLiteral("甲。"), QStringLiteral("乙。"), QStringLiteral("丙。")})
              == QStringLiteral("甲。乙。丙。"),
          "ZAJ1 三句依次相接（中文不额外加空格）");
    check(joinSentences({QStringLiteral("甲。"), QString(), QStringLiteral("乙。")})
              == QStringLiteral("甲。乙。"),
          "ZAJ1 中间的空串跳过");
    check(joinSentences({QString(), QStringLiteral("甲。"), QString(), QStringLiteral("乙。"), QString()})
              == QStringLiteral("甲。乙。"),
          "ZAJ1 两头和中间的空串都不留空档");

    // 句子自己带占位符：用双参 arg 归并，前面那句里的 `%1` 不会在下一轮被填一次。
    check(joinSentences({QStringLiteral("通过 %1 项。"), QStringLiteral("其余失败。")})
              == QStringLiteral("通过 %1 项。其余失败。"),
          "★ ZAJ1 句子里的 %1 原样保留");
    check(joinSentences({QStringLiteral("通过 %1 项。%2"), QStringLiteral("其余失败。")})
              == QStringLiteral("通过 %1 项。%2其余失败。"),
          "★ ZAJ1 句子里的 %2 原样保留（没被下一句顶掉）");
}

/**
 * @file AgentWebPolicyTests.cpp
 * @brief 联网地址策略：scheme/userinfo/片段、公网判定（v4/v6 各档）、全地址校验、
 *        pinned-IP URL、content-type 白名单、HTML 转文本与截断。
 */

#include "Support.hpp"

#include "agent/AgentWebPolicy.hpp"

#include <QHostAddress>

void runAgentWebPolicyTests() {
    using namespace AgentWebPolicy;

    // ---------- URL ----------
    check(validateUrl(QUrl(QStringLiteral("https://example.com/a?b=1"))).ok,
          "联网策略：https 地址通过");
    check(validateUrl(QUrl(QStringLiteral("http://example.com"))).ok,
          "联网策略：http 地址通过（IP 校验另算）");
    check(!validateUrl(QUrl(QStringLiteral("file:///C:/x"))).ok, "联网策略：file:// 被拒");
    check(!validateUrl(QUrl(QStringLiteral("ftp://example.com"))).ok, "联网策略：ftp 被拒");
    check(!validateUrl(QUrl(QStringLiteral("data:text/plain,hi"))).ok,
          "联网策略：data: 被拒");
    check(!validateUrl(QUrl(QStringLiteral("https://user:pw@example.com/"))).ok,
          "联网策略：userinfo 被拒");
    check(!validateUrl(QUrl(QStringLiteral("https://example.com/#frag"))).ok,
          "联网策略：片段被拒");
    check(!validateUrl(QUrl(QStringLiteral("https://"))).ok, "联网策略：没有主机被拒");

    // ---------- IP 分类 ----------
    check(classify(QHostAddress(QStringLiteral("8.8.8.8"))) == IpClass::Public,
          "联网策略：公网 v4 是 public");
    check(classify(QHostAddress(QStringLiteral("127.0.0.1"))) == IpClass::Loopback,
          "联网策略：127.0.0.1 是 loopback");
    check(classify(QHostAddress(QStringLiteral("10.1.2.3"))) == IpClass::Private,
          "联网策略：10/8 是 private");
    check(classify(QHostAddress(QStringLiteral("172.16.0.1"))) == IpClass::Private,
          "联网策略：172.16/12 是 private");
    check(classify(QHostAddress(QStringLiteral("172.32.0.1"))) == IpClass::Public,
          "联网策略：172.32 不在私网段");
    check(classify(QHostAddress(QStringLiteral("192.168.1.1"))) == IpClass::Private,
          "联网策略：192.168/16 是 private");
    check(classify(QHostAddress(QStringLiteral("169.254.1.1"))) == IpClass::LinkLocal,
          "联网策略：169.254 是 link-local");
    check(classify(QHostAddress(QStringLiteral("100.64.0.1"))) == IpClass::Private,
          "联网策略：100.64/10 是 CGNAT");
    check(classify(QHostAddress(QStringLiteral("224.0.0.1"))) == IpClass::Multicast,
          "联网策略：224/4 是 multicast");
    check(classify(QHostAddress(QStringLiteral("240.0.0.1"))) == IpClass::Reserved,
          "联网策略：240/4 是 reserved");
    check(classify(QHostAddress(QStringLiteral("0.0.0.0"))) == IpClass::Unspecified,
          "联网策略：0.0.0.0 是 unspecified");
    check(classify(QHostAddress(QStringLiteral("192.0.2.5"))) == IpClass::Reserved,
          "联网策略：文档段 192.0.2 是 reserved");
    check(classify(QHostAddress(QStringLiteral("::1"))) == IpClass::Loopback,
          "联网策略：::1 是 loopback");
    check(classify(QHostAddress(QStringLiteral("fd00::1"))) == IpClass::Private,
          "联网策略：ULA 是 private");
    check(classify(QHostAddress(QStringLiteral("fe80::1"))) == IpClass::LinkLocal,
          "联网策略：fe80:: 是 link-local");
    check(classify(QHostAddress(QStringLiteral("ff02::1"))) == IpClass::Multicast,
          "联网策略：ff02:: 是 multicast");
    check(classify(QHostAddress(QStringLiteral("2001:4860:4860::8888"))) == IpClass::Public,
          "联网策略：公网 v6 是 public");
    check(!addressAllowed(QHostAddress(QStringLiteral("::ffff:10.0.0.1"))),
          "联网策略：v4 映射的私网地址也不许连");
    check(addressAllowed(QHostAddress(QStringLiteral("1.1.1.1"))), "联网策略：1.1.1.1 可连");

    // ---------- 全地址校验 ----------
    QString problem;
    check(allAddressesAllowed({QHostAddress(QStringLiteral("1.1.1.1")),
                              QHostAddress(QStringLiteral("8.8.8.8"))},
                             &problem),
          "联网策略：全部公网地址通过");
    check(!allAddressesAllowed({QHostAddress(QStringLiteral("1.1.1.1")),
                               QHostAddress(QStringLiteral("192.168.0.1"))},
                              &problem),
          "联网策略：多地址里混入私网就整条拒绝");
    check(problem.contains(QStringLiteral("private")), "联网策略：拒绝原因说明是哪一档");
    check(!allAddressesAllowed({}, &problem), "联网策略：解析不出地址也拒绝");

    // ---------- pinned-IP ----------
    const QUrl original(QStringLiteral("https://example.com:8443/path?q=1"));
    const QUrl pinned = pinnedUrl(original, QHostAddress(QStringLiteral("93.184.216.34")));
    check(pinned.host() == QStringLiteral("93.184.216.34"), "联网策略：pinned URL 的 host 是 IP");
    check(pinned.port() == 8443, "联网策略：pinned URL 保留端口");
    check(pinned.path() == QStringLiteral("/path"), "联网策略：pinned URL 保留路径");
    check(pinned.query() == QStringLiteral("q=1"), "联网策略：pinned URL 保留查询");

    // ---------- content-type ----------
    check(contentTypeAllowed(QStringLiteral("text/html; charset=utf-8")),
          "联网策略：text/* 允许");
    check(contentTypeAllowed(QStringLiteral("application/json")), "联网策略：json 允许");
    check(contentTypeAllowed(QString()), "联网策略：没有 Content-Type 时按文本尝试");
    check(!contentTypeAllowed(QStringLiteral("image/png")), "联网策略：图片不解析");
    check(!contentTypeAllowed(QStringLiteral("application/octet-stream")),
          "联网策略：二进制不解析");

    // ---------- HTML → 文本 ----------
    const QString html = QStringLiteral(
        "<html><head><style>body{}</style><script>var x=1;</script></head>"
        "<body><h1>标题</h1><p>第一段&amp;实体</p><div>第二段</div>"
        "<a href=\"https://x\">链接</a></body></html>");
    const QString text = htmlToText(html);
    check(text.contains(QStringLiteral("标题")), "联网策略：HTML 保留正文");
    check(!text.contains(QStringLiteral("script")), "联网策略：去掉 script");
    check(!text.contains(QStringLiteral("body{}")), "联网策略：去掉 style 内容");
    check(!text.contains(QLatin1Char('<')), "联网策略：没有残留标签");
    check(text.contains(QStringLiteral("第一段&实体")), "联网策略：解常见实体");

    QString longText(AgentWebPolicy::kMaxTextChars + 10, QChar('a'));
    check(truncateText(&longText), "联网策略：超长文本被截断");
    check(longText.contains(QStringLiteral("truncated")), "联网策略：截断处有标记");
    QString shortText = QStringLiteral("ok");
    check(!truncateText(&shortText), "联网策略：短文本不截断");
}

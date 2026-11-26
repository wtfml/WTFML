/**
 * @file AgentWebPolicy.cpp
 * @brief 地址策略与 HTML 转文本（见 .hpp）。
 */

#include "AgentWebPolicy.hpp"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QStringList>

namespace {

bool inV4Range(quint32 address, quint32 base, int bits) {
    const quint32 mask = bits == 0 ? 0u : (0xFFFFFFFFu << (32 - bits));
    return (address & mask) == (base & mask);
}

quint32 v4Value(const QHostAddress &address) {
    bool ok = false;
    const quint32 value = address.toIPv4Address(&ok);
    return ok ? value : 0;
}

/// IPv4 的特殊段（RFC 1918 / 6598 / 5737 / 2544 等）：这些都不许连。
AgentWebPolicy::IpClass classifyV4(quint32 v) {
    if (v == 0) return AgentWebPolicy::IpClass::Unspecified;
    if (inV4Range(v, 0x7F000000u, 8)) return AgentWebPolicy::IpClass::Loopback;      // 127/8
    if (inV4Range(v, 0x0A000000u, 8)) return AgentWebPolicy::IpClass::Private;       // 10/8
    if (inV4Range(v, 0xAC100000u, 12)) return AgentWebPolicy::IpClass::Private;      // 172.16/12
    if (inV4Range(v, 0xC0A80000u, 16)) return AgentWebPolicy::IpClass::Private;      // 192.168/16
    if (inV4Range(v, 0x64400000u, 10)) return AgentWebPolicy::IpClass::Private;      // 100.64/10
    if (inV4Range(v, 0xA9FE0000u, 16)) return AgentWebPolicy::IpClass::LinkLocal;    // 169.254/16
    if (inV4Range(v, 0xE0000000u, 4)) return AgentWebPolicy::IpClass::Multicast;     // 224/4
    if (inV4Range(v, 0xF0000000u, 4)) return AgentWebPolicy::IpClass::Reserved;      // 240/4
    if (v == 0xFFFFFFFFu) return AgentWebPolicy::IpClass::Reserved;                  // 广播
    if (inV4Range(v, 0xC0000000u, 24)) return AgentWebPolicy::IpClass::Reserved;     // 192.0.0/24
    if (inV4Range(v, 0xC0000200u, 24)) return AgentWebPolicy::IpClass::Reserved;     // 192.0.2/24
    if (inV4Range(v, 0xC6120000u, 15)) return AgentWebPolicy::IpClass::Reserved;     // 198.18/15
    if (inV4Range(v, 0xC6336400u, 24)) return AgentWebPolicy::IpClass::Reserved;     // 198.51.100/24
    if (inV4Range(v, 0xCB007100u, 24)) return AgentWebPolicy::IpClass::Reserved;     // 203.0.113/24
    if (inV4Range(v, 0xE0000000u, 3)) return AgentWebPolicy::IpClass::Multicast;     // 224/3
    return AgentWebPolicy::IpClass::Public;
}

AgentWebPolicy::IpClass classifyV6(const QHostAddress &address) {
    if (address.isLoopback()) return AgentWebPolicy::IpClass::Loopback;
    if (address.isNull()) return AgentWebPolicy::IpClass::Unspecified;
    if (address.isMulticast()) return AgentWebPolicy::IpClass::Multicast;
    // IPv4 映射地址：按内嵌的 v4 判。
    bool v4 = false;
    const quint32 mapped = address.toIPv4Address(&v4);
    if (v4) return classifyV4(mapped);
    if (address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7)) {
        return AgentWebPolicy::IpClass::Private;  // ULA
    }
    if (address.isInSubnet(QHostAddress(QStringLiteral("fe80::")), 10)) {
        return AgentWebPolicy::IpClass::LinkLocal;
    }
    if (address.isInSubnet(QHostAddress(QStringLiteral("2001:db8::")), 32)) {
        return AgentWebPolicy::IpClass::Reserved;  // 文档段
    }
    return AgentWebPolicy::IpClass::Public;
}

}  // namespace

namespace AgentWebPolicy {

QString ipClassName(IpClass value) {
    switch (value) {
    case IpClass::Public: return QStringLiteral("public");
    case IpClass::Loopback: return QStringLiteral("loopback");
    case IpClass::Private: return QStringLiteral("private");
    case IpClass::LinkLocal: return QStringLiteral("link-local");
    case IpClass::Multicast: return QStringLiteral("multicast");
    case IpClass::Reserved: return QStringLiteral("reserved");
    case IpClass::Unspecified: return QStringLiteral("unspecified");
    case IpClass::Invalid: return QStringLiteral("invalid");
    }
    return QStringLiteral("invalid");
}

IpClass classify(const QHostAddress &address) {
    if (address.isNull() && address.protocol() == QAbstractSocket::UnknownNetworkLayerProtocol) {
        return IpClass::Invalid;
    }
    const QAbstractSocket::NetworkLayerProtocol protocol = address.protocol();
    if (protocol == QAbstractSocket::IPv4Protocol) return classifyV4(v4Value(address));
    if (protocol == QAbstractSocket::IPv6Protocol) return classifyV6(address);
    return IpClass::Invalid;
}

bool addressAllowed(const QHostAddress &address) { return classify(address) == IpClass::Public; }

UrlCheck validateUrl(const QUrl &url) {
    UrlCheck check;
    check.host = url.host();
    check.scheme = url.scheme().toLower();
    if (!url.isValid()) {
        check.problem = QCoreApplication::translate("AgentWebPolicy", "URL 无效。");
        return check;
    }
    if (check.scheme != QStringLiteral("http") && check.scheme != QStringLiteral("https")) {
        check.problem = QCoreApplication::translate("AgentWebPolicy", "仅允许 http/https，拒绝 %1。").arg(url.scheme());
        return check;
    }
    if (!url.userInfo().isEmpty()) {
        check.problem = QCoreApplication::translate("AgentWebPolicy", "URL 不能包含用户信息。");
        return check;
    }
    if (url.hasFragment()) {
        check.problem = QCoreApplication::translate("AgentWebPolicy", "URL 不能包含片段。");
        return check;
    }
    if (check.host.isEmpty()) {
        check.problem = QCoreApplication::translate("AgentWebPolicy", "URL 缺少主机名。");
        return check;
    }
    check.ok = true;
    return check;
}

bool allAddressesAllowed(const QList<QHostAddress> &addresses, QString *problem) {
    if (addresses.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWebPolicy", "域名无法解析为任何地址。");
        return false;
    }
    for (const QHostAddress &address : addresses) {
        if (!addressAllowed(address)) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("AgentWebPolicy", "目标地址 %1 属于 %2，拒绝连接。")
                               .arg(address.toString(), ipClassName(classify(address)));
            }
            return false;
        }
    }
    return true;
}

QUrl pinnedUrl(const QUrl &original, const QHostAddress &address) {
    QUrl url = original;
    QString host = address.toString();
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        // 连接地址必须是裸 IPv6 文本；Host 头另外加方括号。
        url.setHost(host);
    } else {
        url.setHost(host);
    }
    return url;
}

bool contentTypeAllowed(const QString &contentType) {
    const QString lowered = contentType.toLower();
    if (lowered.isEmpty()) return true;  // 有的服务器不回 Content-Type，按文本尝试解析
    if (lowered.startsWith(QStringLiteral("text/"))) return true;
    static const QStringList allowed{ QStringLiteral("application/json"),
                                      QStringLiteral("application/ld+json"),
                                      QStringLiteral("application/xml"),
                                      QStringLiteral("application/xhtml+xml"),
                                      QStringLiteral("application/rss+xml"),
                                      QStringLiteral("application/atom+xml") };
    for (const QString &item : allowed) {
        if (lowered.startsWith(item)) return true;
    }
    return false;
}

QString htmlToText(const QString &html) {
    QString text = html;
    // script/style 整段丢掉，包括内容。
    static const QRegularExpression script(
        QStringLiteral("<\\s*(script|style)[^>]*>.*?<\\s*/\\s*\\1\\s*>"),
        QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::DotMatchesEverythingOption);
    text.remove(script);
    // 块级标签换成换行，其余标签直接去掉。
    static const QRegularExpression block(
        QStringLiteral("<\\s*/?\\s*(p|div|br|li|tr|h[1-6]|section|article|header|footer)[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(block, QStringLiteral("\n"));
    static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
    text.remove(tag);
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    static const QRegularExpression spaces(QStringLiteral("[ \\t\\r\\f\\v]+"));
    text.replace(spaces, QStringLiteral(" "));
    static const QRegularExpression blankLines(QStringLiteral("\\n{3,}"));
    text.replace(blankLines, QStringLiteral("\n\n"));
    return text.trimmed();
}

bool truncateText(QString *text, int maxChars) {
    if (text == nullptr) return false;
    if (text->size() <= maxChars) return false;
    *text = text->left(maxChars) + QStringLiteral("\n...(truncated)");  // i18n-ignore
    return true;
}

}  // namespace AgentWebPolicy

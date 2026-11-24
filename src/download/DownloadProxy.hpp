#pragma once
#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>

// 代理（设置 → 下载 →「代理」）。
//
// 三档：
// System —— 跟随系统（Windows 的「Internet 选项 → 连接 → 局域网设置」）。**出厂就是它**，
// 也就是"谁都不动这一项时行为一个字都不变"：之前 Qt 一直是这么走的。
// Direct —— 直连，不走任何代理（系统里配了代理但它坏了 / 不想让它管启动器时用）。
// Manual —— 用户自己填 `host:port`（可以带 `socks5://` 前缀）。
//
// 这一层是**纯判据**（没有 QtNetwork、不碰任何全局状态）：怎么解析、怎么归一、认不出来
// 算什么，唯一出处都在这里；真把它装到进程上的是 `LauncherPreferencesController::
// syncDownloadProxy()`（`QNetworkProxy::setApplicationProxy`），测试可以直接核这里。
//
// NOTE: 只认一个地址，没有"例外列表 / 用户名密码"。理由是这一项要解决的是"启动器连不上
// 网、而系统里配的代理它没走对"这件事；带认证的代理眼下用系统那一档填更省事。
namespace DownloadProxy {

enum class Mode { System, Direct, Manual };

/// 出厂档位：跟随系统（= 加这一项之前的行为）。
inline constexpr Mode kDefaultMode = Mode::System;

/// 下拉框里的三条（顺序就是展示顺序）。
inline QList<Mode> modes() {
    return {Mode::System, Mode::Direct, Mode::Manual};
}

/// @return 存进 QSettings 的字符串（和别的枚举同一个做法：写死的键，别用界面文字）。
inline QString modeKey(Mode mode) {
    switch (mode) {
    case Mode::Direct: return QStringLiteral("direct");
    case Mode::Manual: return QStringLiteral("manual");
    case Mode::System: break;
    }
    return QStringLiteral("system");
}

/// @return 认不出来的值一律当 System（= 默认行为，不会因为一行坏配置把网络掐掉）。
inline Mode modeFromKey(const QString &key) {
    const QString text = key.trimmed().toLower();
    if (text == QStringLiteral("direct")) return Mode::Direct;
    if (text == QStringLiteral("manual")) return Mode::Manual;
    return Mode::System;
}

/// 手动那一格解析出来的东西。
struct Endpoint {
    QString host;
    int port = 0;
    bool socks5 = false;
    bool operator==(const Endpoint &other) const {
        return host == other.host && port == other.port && socks5 == other.socks5;
    }
};

/// 解析手动代理地址。
///
/// 认的写法：
/// `127.0.0.1:7890`、`proxy.example.com:8080`
/// `http://127.0.0.1:7890`（和上一行同一个意思）
/// `socks5://127.0.0.1:7890`、`socks5h://...`、`socks://...`
/// `[::1]:7890`（IPv6 要用方括号）
///
/// @param raw 用户填的原串（两头的空白会修掉）。
/// @param out 解析成功时写入。
/// @param error 失败时写入一句给人看的中文原因；可以为 nullptr。
/// @return 能不能用。
inline bool parseManual(const QString &raw, Endpoint *out, QString *error = nullptr) {
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    if (out == nullptr) return fail(QCoreApplication::translate("DownloadProxy", "内部错误：未接收到解析结果"));
    QString text = raw.trimmed();
    if (text.isEmpty()) {
        return fail(QCoreApplication::translate("DownloadProxy", "手动代理须填写地址，例如 127.0.0.1:7890。"));
    }
    // 前缀（协议）。没有前缀 = HTTP 代理。
    bool socks5 = false;
    const int schemeEnd = text.indexOf(QStringLiteral("://"));
    if (schemeEnd >= 0) {
        const QString scheme = text.left(schemeEnd).trimmed().toLower();
        text = text.mid(schemeEnd + 3);
        if (scheme == QStringLiteral("socks5") || scheme == QStringLiteral("socks5h") ||
            scheme == QStringLiteral("socks")) {
            socks5 = true;
        } else if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
            return fail(QCoreApplication::translate("DownloadProxy", "仅支持 http:// 和 socks5:// "
                                                                     "两种前缀（省略前缀时按 HTTP 处理）。"));
        }
    }
    // 地址里不该再有路径 / 查询串 —— 那是把"要访问的网址"填进来了。
    if (text.contains(QLatin1Char('/'))) {
        return fail(QCoreApplication::translate("DownloadProxy", "代理地址仅填写主机和端口，不含路径。"));
    }
    QString host;
    QString portText;
    if (text.startsWith(QLatin1Char('['))) {
        // IPv6：[::1]:7890
        const int close = text.indexOf(QLatin1Char(']'));
        if (close < 0) {
            return fail(QCoreApplication::translate("DownloadProxy", "IPv6 地址须用方括号括起，例如 [::1]:7890。"));
        }
        host = text.mid(1, close - 1).trimmed();
        if (close + 1 >= text.size() || text.at(close + 1) != QLatin1Char(':')) {
            return fail(QCoreApplication::translate("DownloadProxy", "须填写端口，例如 [::1]:7890。"));
        }
        portText = text.mid(close + 2).trimmed();
    } else {
        const int colon = text.lastIndexOf(QLatin1Char(':'));
        if (colon < 0) {
            return fail(QCoreApplication::translate("DownloadProxy", "须填写端口，例如 127.0.0.1:7890。"));
        }
        host = text.left(colon).trimmed();
        portText = text.mid(colon + 1).trimmed();
    }
    if (host.isEmpty()) {
        return fail(QCoreApplication::translate("DownloadProxy", "主机部分为空。"));
    }
    for (const QChar ch : host) {
        if (ch.isSpace() || ch == QLatin1Char('@') || ch == QLatin1Char('#') ||
            ch == QLatin1Char('?') || ch == QLatin1Char('\\')) {
            return fail(QCoreApplication::translate("DownloadProxy", "主机部分含无法识别的字符，"
                                                                     "仅支持主机名或 IP。"));
        }
    }
    bool ok = false;
    const int port = portText.toInt(&ok);
    if (!ok || port < 1 || port > 65535) {
        return fail(QCoreApplication::translate("DownloadProxy", "端口须为 1–65535 之间的整数。"));
    }
    if (out) {
        out->host = host;
        out->port = port;
        out->socks5 = socks5;
    }
    return true;
}

/// 把一份地址拼回一行（存盘 / 界面回填用）。`socks5` 会带上前缀，HTTP 不带 ——
/// 于是"没写前缀"和"写了 http://"读回来长得一样，不会每次保存都多一层前缀。
inline QString endpointText(const Endpoint &endpoint) {
    QString host = endpoint.host;
    if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('['))) {
        host = QLatin1Char('[') + host + QLatin1Char(']');
    }
    return QStringLiteral("%1%2:%3")
        .arg(endpoint.socks5 ? QStringLiteral("socks5://") : QString(), host)
        .arg(endpoint.port);
}

}  // namespace DownloadProxy

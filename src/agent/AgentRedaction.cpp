/**
 * @file AgentRedaction.cpp
 * @brief 凭据形态的扫描规则（字段名优先，其次常见令牌形态）。
 */

#include "AgentRedaction.hpp"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

namespace AgentRedaction {
namespace {

QString replacementText() { return QStringLiteral("<redacted>"); }

void note(Result *result, const QString &kind) {
    result->hits += 1;
    if (!result->kinds.contains(kind)) {
        result->kinds.append(kind);
    }
}

/// 键名切成小写词根：`_`、`-`、`.` 断开，外加「小写/数字后面紧跟大写」这一刀。
///
/// 不在**全大写串内部**切（`PASSWORD` 是一个词根，切成 pass+word 就谁都不认识了），
/// 所以这里没有用「大写后面接小写就断」那条常见的驼峰收尾规则。
QStringList keyNameTokens(const QString &key) {
    QStringList tokens;
    QString current;
    for (int i = 0; i < key.size(); ++i) {
        const QChar character = key.at(i);
        if (character == QLatin1Char('_') || character == QLatin1Char('-') ||
            character == QLatin1Char('.')) {
            if (!current.isEmpty()) {
                tokens << current;
                current.clear();
            }
            continue;
        }
        if (character.isUpper() && !current.isEmpty()) {
            const QChar previous = key.at(i - 1);
            if (previous.isLower() || previous.isDigit()) {
                tokens << current;
                current.clear();
            }
        }
        current.append(character.toLower());
    }
    if (!current.isEmpty()) {
        tokens << current;
    }
    return tokens;
}

/// 键名里出现这些词根，就按凭据字段对待。
///
/// NOTE: 判据不能写成「键名等于某个完整名字」。`\b` 在下划线和连字符**中间不成立**
/// （那两个都是单词字符），所以写死名字那一版放过的是 `hypixel_api_key`、`DB_PASSWORD`、
/// `REDIS_PASSWORD`、`API_SECRET`、`JWT_SECRET` 这一整类带前后缀的名字 —— 而字段名是
/// 服务方和用户配置说了算的，我们列不完。
bool isCredentialKeyName(const QString &key) {
    static const QSet<QString> roots = {
        QStringLiteral("password"),  QStringLiteral("passwd"), QStringLiteral("pwd"),
        QStringLiteral("secret"),    QStringLiteral("token"),  QStringLiteral("credential"),
        QStringLiteral("credentials"), QStringLiteral("cookie"), QStringLiteral("csrf"),
        QStringLiteral("session"),   QStringLiteral("apikey")};
    const QStringList tokens = keyNameTokens(key);
    for (const QString &token : tokens) {
        if (roots.contains(token)) {
            return true;
        }
    }
    // 相邻两个词根合起来也是凭据字段：`api_key`、`x-api-key`、`apiKey`，
    // 以及 `private_key`、`access_key`。`public_key` 不在其中，它是公开材料。
    static const QSet<QString> credentialPairs = {
        QStringLiteral("api/key"),     QStringLiteral("key/api"),
        QStringLiteral("private/key"), QStringLiteral("access/key")};
    for (int i = 0; i + 1 < tokens.size(); ++i) {
        const QString adjacent = tokens.at(i) + QLatin1Char('/') + tokens.at(i + 1);
        if (credentialPairs.contains(adjacent)) {
            return true;
        }
    }
    return false;
}

const QRegularExpression &authorizationPattern() {
    static const QRegularExpression pattern(
        QStringLiteral(R"((?i)\b(authorization\s*[:=]\s*)(bearer\s+)?([A-Za-z0-9._~+/=-]{8,}))"));
    return pattern;
}

const QRegularExpression &fieldPattern() {
    // 只遮“字段名 = 值”里的值；值本身必须够长，避免把 `password = true` 这种说明文字遮掉。
    // 键名这里**只放宽形状**（任意 2–64 个名字字符），是不是凭据由 isCredentialKeyName() 判。
    static const QRegularExpression pattern(QStringLiteral(
        R"(\b([A-Za-z0-9_.\-]{2,64})(\s*[:=]\s*["']?)([^\s"',;]{6,}))"));
    return pattern;
}

const QRegularExpression &bearerPattern() {
    static const QRegularExpression pattern(
        QStringLiteral(R"((?i)\bbearer\s+([A-Za-z0-9._~+/=-]{8,}))"));
    return pattern;
}

const QRegularExpression &skPattern() {
    static const QRegularExpression pattern(QStringLiteral(R"(\bsk-[A-Za-z0-9_-]{12,})"));
    return pattern;
}

const QRegularExpression &jwtPattern() {
    static const QRegularExpression pattern(
        QStringLiteral(R"(\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,})"));
    return pattern;
}

const QRegularExpression &privateKeyPattern() {
    static const QRegularExpression pattern(
        QStringLiteral(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----)"));
    return pattern;
}

const QRegularExpression &awsPattern() {
    static const QRegularExpression pattern(QStringLiteral(R"(\bAKIA[0-9A-Z]{16}\b)"));
    return pattern;
}

}  // namespace

QString replacement() { return replacementText(); }

Result redact(const QString &text) {
    Result result;
    result.text = text;
    if (text.isEmpty()) {
        return result;
    }

    // 私钥块先处理：它跨行，后面几条都是单行规则。
    {
        auto match = privateKeyPattern().match(result.text);
        while (match.hasMatch()) {
            note(&result, QStringLiteral("private-key"));
            result.text.replace(match.capturedStart(), match.capturedLength(),
                                replacementText());
            match = privateKeyPattern().match(result.text, match.capturedStart() + 1);
        }
    }

    {
        auto match = authorizationPattern().match(result.text);
        while (match.hasMatch()) {
            note(&result, QStringLiteral("authorization"));
            const QString head = match.captured(1) + match.captured(2);
            result.text.replace(match.capturedStart(), match.capturedLength(),
                                head + replacementText());
            match = authorizationPattern().match(result.text, match.capturedStart() + 1);
        }
    }

    {
        int from = 0;
        while (true) {
            const auto match = fieldPattern().match(result.text, from);
            if (!match.hasMatch()) {
                break;
            }
            const QString key = match.captured(1);
            if (!isCredentialKeyName(key)) {
                // 非凭据键名（`version=1.21.1`、`username=…`）原样留着，只把扫描位置
                // 推过这一段键名。
                from = match.capturedStart() + match.capturedLength(1);
                continue;
            }
            note(&result, QStringLiteral("credential-field"));
            const QString head = key + match.captured(2);
            result.text.replace(match.capturedStart(), match.capturedLength(),
                                head + replacementText());
            from = match.capturedStart() + head.size() + replacementText().size();
        }
    }

    {
        auto match = bearerPattern().match(result.text);
        while (match.hasMatch()) {
            note(&result, QStringLiteral("bearer"));
            result.text.replace(match.capturedStart(), match.capturedLength(),
                                QStringLiteral("Bearer ") + replacementText());
            match = bearerPattern().match(result.text, match.capturedStart() + 1);
        }
    }

    {
        auto match = skPattern().match(result.text);
        while (match.hasMatch()) {
            note(&result, QStringLiteral("sk-key"));
            result.text.replace(match.capturedStart(), match.capturedLength(),
                                replacementText());
            match = skPattern().match(result.text, match.capturedStart() + 1);
        }
    }

    {
        auto match = jwtPattern().match(result.text);
        while (match.hasMatch()) {
            note(&result, QStringLiteral("jwt"));
            result.text.replace(match.capturedStart(), match.capturedLength(),
                                replacementText());
            match = jwtPattern().match(result.text, match.capturedStart() + 1);
        }
    }

    {
        auto match = awsPattern().match(result.text);
        while (match.hasMatch()) {
            note(&result, QStringLiteral("cloud-key"));
            result.text.replace(match.capturedStart(), match.capturedLength(),
                                replacementText());
            match = awsPattern().match(result.text, match.capturedStart() + 1);
        }
    }

    return result;
}

bool containsCredentials(const QString &text) { return redact(text).containsCredentials(); }

QString preview(const QString &text, int maxChars) {
    const Result result = redact(text);
    if (maxChars <= 0 || result.text.size() <= maxChars) {
        return result.text;
    }
    return result.text.left(maxChars) + QCoreApplication::translate("AgentRedaction", "\n…（已截断）");
}

}  // namespace AgentRedaction

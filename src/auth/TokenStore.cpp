#include "util/SecretRedactor.hpp"
/**
 * @file TokenStore.cpp
 * @brief 凭据落盘的加解密与读写（见 .hpp 里那段说明）。
 */

#include "TokenStore.hpp"
#include "util/AppPaths.hpp"
#include "util/SecretProtector.hpp"
#include "util/DirectorySessionLock.hpp"
#include "util/SettingsSyncRetry.hpp"

#include <QCryptographicHash>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSettings>

namespace {
// 旧格式（XOR 混淆）的密钥。只为了还能把老值读出来 —— 新写的一律走 DPAPI。
static constexpr const char kEncryptionKey[] = "WTFMLSecretKey2026";

// ───────────────────────── 令牌落盘的加解密 ─────────────────────────
//
// 盘上那份令牌是这个应用最值钱的数据 —— refresh_token 拿到手就等于拿到账号的长期访问权。
// 所以这里**要真加密**。旧的 obfuscate() 只是"密钥写在二进制里的混淆"：谁捡到那个 ini、
// 再反汇编一下把密钥读出来，令牌明文就还原了。
//
// Windows 上的正解是 DPAPI（CryptProtectData）：它用**当前登录用户**的凭据派生密钥，密文只有
// 同一个用户在这台机器上解得开。好处是密钥不归我们保管 —— 没有可泄露的密钥，也不用让用户
// 再记一个密码。代价也说明白：换用户、换机器就解不开（重登一次即可），这是设计的代价，不是 bug。
//
// NOTE: 故意**不加** optionalEntropy。熵是写死在二进制里的，挡不住任何能读到二进制的对手，
// 却会多出一个"两边必须一模一样"的失配点。真正的边界是"同一个 Windows 用户"。
//
// 盘上的格式是自描述的，为的是兼容已经存在的旧值：
// 以 kTokenMagic 开头 → 后面是 DPAPI 密文
// 其它 → 旧的 XOR 混淆（读得出来；下次写盘就自动升级成新格式）
//
// NOTE: 这一层**只管"当前用户 + 这台机器"这一条边界**。多账号列表里那些副本用的是同一个
// 格式、同一个 protect()，所以它们的强度完全一样 —— 换账号不需要用户重新输密码。

static constexpr char kTokenMagic[] = "WTFML-PROTECTED-1\n";
static constexpr int kTokenMagicLen = int(sizeof(kTokenMagic)) - 1;

/// 旧的混淆，只为了还能读出来。加密不再走它。
QByteArray legacyXor(const QByteArray &data) {
    const QByteArray key =
        QCryptographicHash::hash(QByteArray(kEncryptionKey), QCryptographicHash::Sha256);
    QByteArray result;
    result.reserve(data.size());
    for (int i = 0; i < data.size(); ++i) {
        result.append(data.at(i) ^ key.at(i % key.size()));
    }
    return result;
}

}  // namespace

QByteArray TokenStore::protect(const QByteArray &plain) {
#if defined(Q_OS_WIN)
    const QByteArray stored =
        SecretProtector::protect(plain, QByteArray(kTokenMagic, kTokenMagicLen));
    if (stored.isEmpty()) {
        // Encryption failure must never downgrade saved credentials to legacy obfuscation.
        qWarning() << "DPAPI 加密失败，拒绝写入凭据，原数据保留。";
    }
    return stored;
#else
    // Windows is the supported distribution; preserve existing compatibility elsewhere.
    return legacyXor(plain);
#endif
}

QByteArray TokenStore::unprotect(const QByteArray &stored) {
    if (stored.isEmpty()) {
        return QByteArray();
    }
#if defined(Q_OS_WIN)
    if (SecretProtector::hasMagic(stored, QByteArray(kTokenMagic, kTokenMagicLen))) {
        return SecretProtector::unprotect(stored, QByteArray(kTokenMagic, kTokenMagicLen));
    }
#endif
    return legacyXor(stored);
}

bool TokenStore::writeJson(const QString &key, const QJsonObject &json, QString *errorOut) {
    if (!DirectorySessionLock::acquire(AppPaths::defaultMinecraftDir(), errorOut)) return false;
    SecretRedactor::rememberCredentialFields(json);
    const QByteArray rawData = QJsonDocument(json).toJson(QJsonDocument::Compact);
    const QByteArray stored = protect(rawData.toBase64());
    if (stored.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("无法保护登录凭据，未写入，原数据保留。");
        return false;
    }
    QString fileName;
    int attempts = 0;
    if (!SettingsSyncRetry::write([&](QSettings &s) { s.setValue(key, stored); }, &fileName, &attempts)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("凭据写盘失败（%1，已试 %2 次）：%3")
                            .arg(key).arg(attempts).arg(fileName);
        }
        qWarning() << "凭据写盘失败：" << key << fileName << "尝试" << attempts << "次";
        return false;
    }
    return true;
}

QJsonObject TokenStore::readJson(const QString &key) {
    return readJsonChecked(key, nullptr);
}

QJsonObject TokenStore::readJsonChecked(const QString &key, bool *readableOut) {
    QSettings settings;
    const QByteArray encrypted = settings.value(key).toByteArray();
    if (encrypted.isEmpty()) {
        // 盘上没有这一格：这是"真的什么都没有"，不是"读不出来"。
        if (readableOut != nullptr) {
            *readableOut = true;
        }
        return QJsonObject();
    }

    const QByteArray rawData = QByteArray::fromBase64(unprotect(encrypted));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(rawData, &parseError);
    if (!document.isObject()) {
        // 这一格在，但解不开。最常见的原因：这份密文不是这台机器/这个 Windows 账户加密的
        // （DPAPI 的密文绑定到加密它的那个账户），换账户或换机器之后一定解不开 —— 它本来
        // 也没有"空"的意思。**专门留一行日志**：这种事从界面上看只会是"账号都没了"。
        if (readableOut != nullptr) {
            *readableOut = false;
        }
        qWarning() << "凭据读不出来（在，但解不开）：" << key << parseError.errorString();
        return QJsonObject();
    }

    if (readableOut != nullptr) {
        *readableOut = true;
    }
    SecretRedactor::rememberCredentialFields(document.object());
    return document.object();
}

bool TokenStore::hasValue(const QString &key) {
    QSettings settings;
    return !settings.value(key).toByteArray().isEmpty();
}

bool TokenStore::clear(const QString &key, QString *errorOut) {
    QString problem;
    if (!DirectorySessionLock::acquire(AppPaths::defaultMinecraftDir(), &problem)) {
        qWarning() << problem;
        if (errorOut != nullptr) {
            *errorOut = problem;
        }
        return false;
    }
    QString fileName;
    int attempts = 0;
    const bool ok = SettingsSyncRetry::write([&](QSettings &s) { s.remove(key); }, &fileName, &attempts);
    // 清不掉**不是静默的**。登出 / 切离线账号那两条路清的就是活动槽，写不
    // 进去的话用户会看到"已退出登录"，下次启动却还是那个账号 —— 而 QSettings 落盘失败
    // 本身一个字都不说。至少留一行日志（和 writeJson() 那条同一个理由）。
    // R13：调用方要拿"真的清掉了没有"去决定说不说那句"已经退出登录"，所以这里返回结果。
    if (!ok) {
        qWarning() << "凭据清不掉（QSettings 状态异常）：" << key << fileName << "尝试" << attempts << "次";
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("凭据清不掉（%1，已试 %2 次）：%3").arg(key).arg(attempts).arg(fileName);
        }
        return false;
    }
    return true;
}

QByteArray TokenStore::rawValue(const QString &key) {
    QSettings settings;
    return settings.value(key).toByteArray();
}

bool TokenStore::writeRaw(const QString &key, const QByteArray &stored, QString *errorOut) {
    // 原来那一格就不存在 ⇒ 放回去 = 删掉它（别凭空留一个空值，见 clear() 的 NOTE）。
    if (stored.isEmpty()) {
        return clear(key, errorOut);
    }
    QString problem;
    if (!DirectorySessionLock::acquire(AppPaths::defaultMinecraftDir(), &problem)) {
        qWarning() << problem;
        if (errorOut != nullptr) {
            *errorOut = problem;
        }
        return false;
    }
    QString fileName;
    int attempts = 0;
    if (!SettingsSyncRetry::write([&](QSettings &s) { s.setValue(key, stored); }, &fileName, &attempts)) {
        qWarning() << "凭据回滚写盘失败：" << key << fileName << "尝试" << attempts << "次";
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("凭据回滚写盘失败（%1，已试 %2 次）：%3").arg(key).arg(attempts).arg(fileName);
        }
        return false;
    }
    return true;
}

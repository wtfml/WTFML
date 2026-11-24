/**
 * @file AgentSecretStore.cpp
 * @brief 凭据管理器 / DPAPI 后备两条路（见 .hpp 的边界）。
 */

#include "AgentSecretStore.hpp"

#include "util/SecretProtector.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QSettings>
#include <QString>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace AgentSecretStore {
namespace {

const QByteArray &secretMagic() {
    static const QByteArray magic("WTFML-AGENT-SECRET-1\n");
    return magic;
}

bool propertyTrue(const char *name) {
    const auto app = QCoreApplication::instance();
    return app != nullptr && app->property(name).toBool();
}

QSettings fallbackSettings() {
    // 明确的 HKCU 产品位置。测试里 QSettings 的 IniFormat 路径已经被重定向到临时目录，
    // 所以同一句代码不会写进开发者的真实注册表。
    return QSettings(QStringLiteral("WTFML"), QStringLiteral("WTFML"));
}

QString fallbackKey(const QString &profileId) {
    return QStringLiteral("Agent/Secrets/") + profileId;
}

#if defined(Q_OS_WIN)
bool credRead(const QString &target, QByteArray *out) {
    PCREDENTIALW credential = nullptr;
    if (CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0,
                  &credential) == FALSE) {
        return false;
    }
    if (credential->CredentialBlobSize > 0 && credential->CredentialBlob != nullptr) {
        *out = QByteArray(reinterpret_cast<const char *>(credential->CredentialBlob),
                          int(credential->CredentialBlobSize));
    }
    CredFree(credential);
    return true;
}

bool credWrite(const QString &target, const QByteArray &secret, QString *problem) {
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(target.utf16()));
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(secret.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<LPWSTR>(L"WTFML");
    if (CredWriteW(&credential, 0) == FALSE) {
        if (problem != nullptr) {
            *problem = QStringLiteral("Windows 凭据管理器写入失败（错误码 %1）。")
                           .arg(int(GetLastError()));
        }
        return false;
    }
    return true;
}

bool credDelete(const QString &target, QString *problem) {
    if (CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0) == FALSE) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return true;
        }
        if (problem != nullptr) {
            *problem = QStringLiteral("Windows 凭据管理器删除失败（错误码 %1）。").arg(int(error));
        }
        return false;
    }
    return true;
}
#endif

}  // namespace

QString backendKey(Backend backend) {
    switch (backend) {
    case Backend::CredentialManager: return QStringLiteral("credential-manager");
    case Backend::DpapiFallback: return QStringLiteral("dpapi-fallback");
    case Backend::Unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}

QString credentialTarget(const QString &profileId) {
    return QStringLiteral("WTFML/Agent/") + profileId;
}

bool credentialManagerAvailable() {
    return !propertyTrue("wtfml.testAgentDisableCredentialManager");
}

bool write(const QString &profileId, const QString &secret, Backend *backendOut, QString *problem) {
    if (profileId.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("配置 id 为空，拒绝保存凭据。");
        return false;
    }
    if (secret.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("凭据为空，拒绝保存。");
        return false;
    }
    if (propertyTrue("wtfml.testAgentSecretProtectFail")) {
        if (problem != nullptr) *problem = QStringLiteral("模拟故障：两条凭据存储路径都不可用，拒绝保存（旧值保留）。");
        return false;
    }

    const QString target = credentialTarget(profileId);
    QString credProblem;
#if defined(Q_OS_WIN)
    if (credentialManagerAvailable() &&
        credWrite(target, secret.toUtf8(), &credProblem)) {
        // 迁移的收尾：主路径写成功后，后备里那份旧值要删掉。
        QSettings fallback = fallbackSettings();
        fallback.remove(fallbackKey(profileId));
        fallback.sync();
        if (backendOut != nullptr) *backendOut = Backend::CredentialManager;
        return true;
    }
#else
    if (credentialManagerAvailable()) {
        credProblem = QStringLiteral("当前平台没有 Windows 凭据管理器。");
    }
#endif

    const QByteArray encrypted =
        SecretProtector::protect(secret.toUtf8(), secretMagic());
    if (!encrypted.isEmpty()) {
        QSettings fallback = fallbackSettings();
        fallback.setValue(fallbackKey(profileId), QString::fromLatin1(encrypted.toBase64()));
        fallback.sync();
        if (fallback.status() == QSettings::NoError) {
            if (problem != nullptr && credentialManagerAvailable()) {
                *problem = credProblem;
            }
            if (backendOut != nullptr) *backendOut = Backend::DpapiFallback;
            return true;
        }
        if (problem != nullptr) {
            *problem = QStringLiteral("DPAPI 后备写盘失败（%1）。").arg(fallback.fileName());
        }
        return false;
    }

    if (problem != nullptr) {
        *problem = credProblem.isEmpty()
                       ? QStringLiteral("DPAPI 保护失败，拒绝保存凭据（不降级为明文）。")
                       : QStringLiteral("%1 DPAPI 后备也不可用，拒绝保存。").arg(credProblem);
    }
    return false;
}

ReadResult read(const QString &profileId) {
    ReadResult result;
    if (profileId.isEmpty()) {
        result.problem = QStringLiteral("配置 id 为空。");
        return result;
    }

    const QString target = credentialTarget(profileId);
    QByteArray blob;
#if defined(Q_OS_WIN)
    if (credentialManagerAvailable() && credRead(target, &blob)) {
        result.secret = QString::fromUtf8(blob);
        result.backend = Backend::CredentialManager;
        result.ok = true;
        return result;
    }
#endif

    QSettings fallback = fallbackSettings();
    const QString stored = fallback.value(fallbackKey(profileId)).toString();
    if (!stored.isEmpty()) {
        const QByteArray decrypted =
            SecretProtector::unprotect(QByteArray::fromBase64(stored.toLatin1()), secretMagic());
        if (decrypted.isEmpty()) {
            result.problem = QStringLiteral("DPAPI 后备里的凭据解不开（换用户/换机器或值损坏）。");
            return result;
        }
        result.secret = QString::fromUtf8(decrypted);
        result.backend = Backend::DpapiFallback;
        result.ok = true;

        // 有主路径就迁过去；迁移失败保留旧值，不丢 key。
#if defined(Q_OS_WIN)
        if (credentialManagerAvailable()) {
            QString ignored;
            if (credWrite(target, result.secret.toUtf8(), &ignored)) {
                fallback.remove(fallbackKey(profileId));
                fallback.sync();
                result.backend = Backend::CredentialManager;
            }
        }
#endif
        return result;
    }

    result.problem = QStringLiteral("没有可用的凭据，请重新输入 API Key。");
    return result;
}

bool clear(const QString &profileId, QString *problem) {
    bool ok = true;
    QString firstProblem;
#if defined(Q_OS_WIN)
    QString credProblem;
    if (!credDelete(credentialTarget(profileId), &credProblem)) {
        ok = false;
        firstProblem = credProblem;
    }
#endif
    QSettings fallback = fallbackSettings();
    fallback.remove(fallbackKey(profileId));
    fallback.sync();
    if (fallback.status() != QSettings::NoError) {
        ok = false;
        if (firstProblem.isEmpty()) {
            firstProblem = QStringLiteral("DPAPI 后备清除失败（%1）。").arg(fallback.fileName());
        }
    }
    if (!ok && problem != nullptr) {
        *problem = firstProblem;
    }
    return ok;
}

bool hasSecret(const QString &profileId) {
    return read(profileId).ok;
}

}  // namespace AgentSecretStore

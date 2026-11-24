/**
 * @file AccountStore.cpp
 * @brief 多账号列表的实现（见 .hpp 里那三段说明）。
 */

#include "AccountStore.hpp"
#include <QCoreApplication>

#include "TokenStore.hpp"
#include "util/SettingsSyncRetry.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>

namespace {
constexpr const char kAccountsField[] = "accounts";
constexpr const char kUuidField[] = "uuid";
constexpr const char kNameField[] = "name";
constexpr const char kSavedAtField[] = "saved_at";
// 那个账号两份凭据的**副本**（各是一份完整的 payload，格式和活动槽里那份一样）。
constexpr const char kMicrosoftField[] = "microsoft";
constexpr const char kMinecraftField[] = "minecraft";
// 离线账号的标志位。**只有它为 true 的项可以没有上面那两份副本** —— 见 addOffline()。
constexpr const char kOfflineField[] = "offline";
// 刷新令牌失效标记（例如微软返回 invalid_grant）。
constexpr const char kNeedsReloginField[] = "needs_relogin";

/// 读列表（整份 JSON）。
///
/// NOTE: **两种"读不到"必须分开**（所以有 @p readableOut）：盘上就没有这一格（真的还没有
/// 任何账号 —— 这是空列表）和那一格在、但解不开（DPAPI 换了 Windows 账户或换机器
/// 之后解不出来，或者值被写坏了）。不分开的话，captureActive() /
/// addOffline() 这两个"读-改-写"的写方会拿空数组当底子把整份列表覆盖成一项 ——
/// 列表里其它账号那两份 refresh_token 副本一起没了（那些副本是**唯一**能切回去的
/// 东西，抹掉就只剩重新登录一条路）。两个"读-改-写"的写方（captureActive /
/// addOffline）先看 readableOut，假就不写；remove() 走的是裸 readAccounts()，
/// 读不出来时**静默**返回 false（既不动盘、也不给用户那句话）。
/// 只当"列表里有没有这个人"用的读者（find/contains/nameOf/accounts/entryFor/activate）
/// 继续忽略它 —— 解不开的时候它们本来就查不到人。
QJsonArray readAccounts(bool *readableOut = nullptr) {
    bool readable = false;
    const QJsonObject root = TokenStore::readJsonChecked(TokenStore::accountsKey(), &readable);
    if (readableOut != nullptr) {
        *readableOut = readable;
    }
    if (!readable) {
        return QJsonArray();
    }
    return root.value(QLatin1String(kAccountsField)).toArray();
}

/// 列表读不出来时给用户看的那句话。captureActive / addOffline 两个写方共用一份，
/// 免得一处改了另一处还是旧话术。（remove() 用不上它，见上面 readAccounts() 那段。）
QString unreadableListMessage() {
    return QCoreApplication::translate("AccountStore", "无法解密已保存的账号列表。为保留原有账号，本次未写入。请在原设备上使用保存列表时的 Windows 账户重试。");
}

bool writeAccounts(const QJsonArray &accounts, QString *errorOut) {
    QJsonObject root;
    root.insert(QLatin1String(kAccountsField), accounts);
    return TokenStore::writeJson(TokenStore::accountsKey(), root, errorOut);
}

/// 盘上活动槽里 Minecraft 那份 payload —— 只有它知道"当前账号是谁"。
/// 空对象 = 没登录（或者在的这份凭据不完整）。
QJsonObject activeMinecraftPayload() {
    const QJsonObject payload = TokenStore::readJson(TokenStore::minecraftKey());
    if (payload.value(QLatin1String(kUuidField)).toString().isEmpty()) {
        return QJsonObject();
    }
    return payload;
}

/// 活动槽里那份凭据现在**清掉就再也找不回来**了没有（R8）。
///
/// `captureActive()` 失败有两种，调用方必须分开对待：
/// · 活动槽里本来就没有一份完整的凭据（没登录 / 缺了微软那半边）→ 清一遍是空操作；
/// · 凭据是完整的，只是**收不进列表**（列表那一格解不开，或者写盘失败）→ 这时候清掉
///   活动槽，就是把这台机器上唯一那份 refresh_token 删了：盘上再没有第二份，用户只能
///   重走一遍浏览器登录（这是 R8 描述的那个症状）。
///
/// 判据就是"两边都齐"：齐了还收不进去，问题在列表 / 盘，不在凭据本身。
bool activeCredentialWouldBeLost() {
    if (activeMinecraftPayload().isEmpty()) {
        return false;
    }
    const QJsonObject microsoft = TokenStore::readJson(TokenStore::microsoftKey());
    return !microsoft.value(QStringLiteral("refresh_token")).toString().isEmpty();
}

/// 列表里那一项（原始 JSON）。找不到返回空对象。
///
/// NOTE: 单独有这个函数是为了 activeUuid() —— 它需要"只看列表、不问当前是谁"，而
/// find() 自己会去问 activeUuid()（要填 isCurrent），拿 find() 取会无限递归。
QJsonObject entryFor(const QString &uuid) {
    if (uuid.isEmpty()) {
        return QJsonObject();
    }
    for (const QJsonValue &value : readAccounts()) {
        const QJsonObject item = value.toObject();
        if (item.value(QLatin1String(kUuidField)).toString() == uuid) {
            return item;
        }
    }
    return QJsonObject();
}

/// 把某个离线账号记成"当前"，并且**清空活动槽**（离开在线身份）。
/// 切换和添加两条路共用这一句，免得一处改了另一处忘。
///
/// `CurrentAccountUuid` 那一格写不进去**要让调用方知道**（@return false）。
/// 在线账号的"当前是谁"是从活动槽里那份凭据推出来的，这一格坏了不影响它；可**离线账号
/// 只有这一格**（见 activeUuid() 第二档）—— 写不进去，用户切过去之后界面会说"没登录"，
/// 而他刚刚明明选了那个账号。QSettings 写失败是**静默**的（没有组织名/权限/注册表被占），
/// 所以这一格必须自己查状态。
bool makeOfflineCurrent(const QString &uuid, QString *errorOut) {
    // **清空活动槽之前，先把里面那位在线账号收进列表**。
    // 下面两句一执行，活动槽里那份 refresh_token 就在盘上没有了 —— 而它**可能根本不在
    // 列表里**（凭据只存在活动槽、captureActive() 也只在"登录成功"那一下被调过）。
    // 少了这一句，点一下「添加离线账号」
    // 或是切到某个离线账号，那份唯一的凭据就被抹掉了，而且盘上再没有任何地方找得回来
    // （只能重走一遍浏览器登录）。文件头那句"当前那个在线账号不会丢：凭据还在列表里"
    // 靠的就是这一句成真。
    //
    // R8：收不进去**分两种**。活动槽里本来就没有一份完整的凭据时（没登录 / 缺了微软
    // 那半边），清一遍是空操作，不拦这次切换；可要是凭据是齐的、只是**列表读不出来 /
    // 写不进去**，那下面两句就是把这台机器上唯一那份 refresh_token 删掉 —— 这一种
    // 必须中止，一个字节都不动（判据在 activeCredentialWouldBeLost()）。
    QString captureError;
    if (!AccountStore::captureActive(&captureError)) {
        qWarning().noquote() << QCoreApplication::translate("AccountStore", "切换离线账号前，保存当前账号失败：") << captureError;
        if (activeCredentialWouldBeLost()) {
            if (errorOut != nullptr) {
                *errorOut = QCoreApplication::translate("AccountStore", "无法保存当前账号（%1），已取消切换。\n\n当前账号的登录凭据已保留。")
                                .arg(captureError);
            }
            return false;
        }
    }
    // R13：清不动就别往下走 —— 活动槽里那份凭据还在的话，下面这一格（`CurrentAccountUuid`）
    // 写进去也改变不了 activeUuid() 的结论（它先看活动槽），界面会显示成"切过去了"、
    // 实际还是原来那个在线账号。
    QString clearError;
    const bool microsoftCleared = TokenStore::clear(TokenStore::microsoftKey(), &clearError);
    const bool minecraftCleared = TokenStore::clear(TokenStore::minecraftKey(), &clearError);
    if (!microsoftCleared || !minecraftCleared) {
        qWarning().noquote() << QCoreApplication::translate("AccountStore", "切换离线账号时清除当前登录信息失败：") << clearError;
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("AccountStore", "无法清除当前登录信息（%1），已取消切换。\n\n请重试。若仍失败，请检查启动器设置的写入权限。")
                            .arg(clearError);
        }
        return false;
    }
    QString fileName;
    if (!SettingsSyncRetry::write([&](QSettings &s) { s.setValue(TokenStore::currentUuidKey(), uuid); },
                                  &fileName)) {
        const QString why = QCoreApplication::translate("AccountStore", "未能保存当前账号设置（%1）。")
                                .arg(fileName);
        qWarning().noquote() << QCoreApplication::translate("AccountStore", "切换离线账号时保存设置失败：") << why;
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("AccountStore", "账号已添加到列表。%1\n\n请重新选择该账号。若仍失败，请检查启动器设置的写入权限。")
                            .arg(why);
        }
        return false;
    }
    return true;
}
}  // namespace

QList<SavedAccount> AccountStore::accounts() {
    const QString current = activeUuid();
    const QJsonArray array = readAccounts();

    QList<SavedAccount> result;
    result.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject item = value.toObject();
        SavedAccount account;
        account.uuid = item.value(QLatin1String(kUuidField)).toString();
        if (account.uuid.isEmpty()) {
            // 一项连 uuid 都没有（写坏了的旧值）就当它不存在 —— 列表里摆一个点不动的账号
            // 比少一项更难查。
            continue;
        }
        account.name = item.value(QLatin1String(kNameField)).toString();
        account.savedAt = item.value(QLatin1String(kSavedAtField)).toString();
        account.offline = item.value(QLatin1String(kOfflineField)).toBool();
        account.needsRelogin = !account.offline && item.value(QLatin1String(kNeedsReloginField)).toBool();
        account.isCurrent = !current.isEmpty() && account.uuid == current;
        result.append(account);
    }
    return result;
}

bool AccountStore::contains(const QString &uuid) {
    SavedAccount account;
    return find(uuid, &account);
}

QString AccountStore::nameOf(const QString &uuid) {
    SavedAccount account;
    return find(uuid, &account) ? account.name : QString();
}

bool AccountStore::find(const QString &uuid, SavedAccount *out) {
    if (uuid.isEmpty()) {
        return false;
    }
    const QString current = activeUuid();
    for (const QJsonValue &value : readAccounts()) {
        const QJsonObject item = value.toObject();
        if (item.value(QLatin1String(kUuidField)).toString() != uuid) {
            continue;
        }
        if (out != nullptr) {
            out->uuid = uuid;
            out->name = item.value(QLatin1String(kNameField)).toString();
            out->savedAt = item.value(QLatin1String(kSavedAtField)).toString();
            out->offline = item.value(QLatin1String(kOfflineField)).toBool();
            out->needsRelogin = !out->offline && item.value(QLatin1String(kNeedsReloginField)).toBool();
            out->isCurrent = !current.isEmpty() && uuid == current;
        }
        return true;
    }
    return false;
}

bool AccountStore::needsRelogin(const QString &uuid) {
    if (uuid.isEmpty()) {
        return false;
    }
    const QJsonObject item = entryFor(uuid);
    if (item.isEmpty() || item.value(QLatin1String(kOfflineField)).toBool()) {
        return false;
    }
    return item.value(QLatin1String(kNeedsReloginField)).toBool();
}

bool AccountStore::setNeedsRelogin(const QString &uuid, bool required, QString *errorOut) {
    if (uuid.isEmpty()) {
        return false;
    }
    bool readable = false;
    QJsonArray array = readAccounts(&readable);
    if (!readable) {
        if (errorOut != nullptr) {
            *errorOut = unreadableListMessage();
        }
        return false;
    }
    for (int i = 0; i < array.size(); ++i) {
        QJsonObject item = array.at(i).toObject();
        if (item.value(QLatin1String(kUuidField)).toString() != uuid) {
            continue;
        }
        if (item.value(QLatin1String(kOfflineField)).toBool()) {
            return false;
        }
        const bool currentFlag = item.value(QLatin1String(kNeedsReloginField)).toBool();
        if (currentFlag == required) {
            return true;
        }
        if (required) {
            item.insert(QLatin1String(kNeedsReloginField), true);
        } else {
            item.remove(QLatin1String(kNeedsReloginField));
        }
        array.replace(i, item);
        return writeAccounts(array, errorOut);
    }
    return false;
}

QString AccountStore::activeUuid() {
    // 第一档：活动槽里有凭据 —— 那份凭据是谁的，当前账号就是谁。在线账号只走这一档。
    const QString fromSlot = activeMinecraftPayload().value(QLatin1String(kUuidField)).toString();
    if (!fromSlot.isEmpty()) {
        return fromSlot;
    }
    // 第二档：活动槽是空的。这时候**只有离线账号**可能还是"当前"（它本来就没有凭据，
    // 清空活动槽正是"切到它"的一部分）。两个条件都要满足才认：
    // ① `CurrentAccountUuid` 确实指向列表里的一项（不然那是个孤儿键 —— 列表都删空了
    // 它还留着）；② 那一项是离线的。
    // ② 是这里的关键：在线账号在活动槽空着的时候**必须**算"没登录"，不能因为记录键上
    // 还写着它就说"当前是某某" —— 那正好是 remove() 清空活动槽之后要避免的那个状态。
    QSettings settings;
    const QString recorded = settings.value(TokenStore::currentUuidKey()).toString();
    const QJsonObject entry = entryFor(recorded);
    if (entry.isEmpty() || !entry.value(QLatin1String(kOfflineField)).toBool()) {
        return QString();
    }
    return recorded;
}

bool AccountStore::captureActive(QString *errorOut) {
    const QJsonObject minecraft = activeMinecraftPayload();
    if (minecraft.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("AccountStore", "未找到可保存的登录凭据。");
        }
        return false;
    }
    const QJsonObject microsoft = TokenStore::readJson(TokenStore::microsoftKey());
    if (microsoft.value(QStringLiteral("refresh_token")).toString().isEmpty()) {
        // 微软那份才是"下次还能不能换出新令牌"的关键。只有一半的账号收进列表，将来切过去
        // 必然换不出令牌 —— 不如当场不收，让调用方看见这句。
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("AccountStore", "微软登录凭据不完整，无法保存该账号。");
        }
        return false;
    }

    const QString uuid = minecraft.value(QLatin1String(kUuidField)).toString();
    const QString name = minecraft.value(QLatin1String(kNameField)).toString();

    QJsonObject entry;
    entry.insert(QLatin1String(kUuidField), uuid);
    entry.insert(QLatin1String(kNameField), name);
    entry.insert(QLatin1String(kSavedAtField), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    entry.insert(QLatin1String(kMicrosoftField), microsoft);
    entry.insert(QLatin1String(kMinecraftField), minecraft);

    // 已有同一个人就**就地更新**（换新令牌、改名字都算同一个人），否则追加到末尾。
    // NOTE: 读不出来就**什么都不写**（理由见 readAccounts()）：这一句是"读-改-写"，
    // 底子读错 = 把整份列表覆盖掉，而列表里存着别的账号唯一的一份 refresh_token 副本。
    bool readable = false;
    QJsonArray array = readAccounts(&readable);
    if (!readable) {
        if (errorOut != nullptr) {
            *errorOut = unreadableListMessage();
        }
        return false;
    }
    bool replaced = false;
    for (int i = 0; i < array.size(); ++i) {
        const QJsonObject existing = array.at(i).toObject();
        if (existing.value(QLatin1String(kUuidField)).toString() == uuid) {
            // 保留已有的「需要重新登录」标记（登出 / 切离线前的保存不应把标记抹掉；
            // 成功登录或刷新成功会在写盘前显式清除该标记）。
            if (existing.value(QLatin1String(kNeedsReloginField)).toBool()) {
                entry.insert(QLatin1String(kNeedsReloginField), true);
            }
            array.replace(i, entry);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        array.append(entry);
    }

    if (!writeAccounts(array, errorOut)) {
        return false;
    }
    // 记下当前账号（`CurrentAccountUuid`）。NOTE: 它**读的时候不参与判断**（见 activeUuid()），
    // 只是留着让"上次用的是谁"在活动槽被清掉之后还查得到。
    QString fileName;
    if (!SettingsSyncRetry::write([&](QSettings &s) { s.setValue(TokenStore::currentUuidKey(), uuid); },
                                  &fileName)) {
        // 在线账号的"当前"是从活动槽里那份凭据推出来的，这一格坏了不影响它 —— 只留一行日志。
        qWarning() << QCoreApplication::translate("AccountStore", "保存当前账号设置失败：") << fileName;
    }
    return true;
}

bool AccountStore::activate(const QString &uuid, QString *errorOut) {
    QJsonObject entry;
    for (const QJsonValue &value : readAccounts()) {
        const QJsonObject item = value.toObject();
        if (item.value(QLatin1String(kUuidField)).toString() == uuid) {
            entry = item;
            break;
        }
    }
    if (entry.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("AccountStore", "此账号不在已保存的账号列表中。");
        }
        return false;
    }

    // 离线账号**没有**副本可写（它本来就没有凭据）：切过去 = 清空活动槽 + 记下当前是它。
    // 所以下面那两个"凭据不完整"的判断对它不适用，得先返回。
    if (entry.value(QLatin1String(kOfflineField)).toBool()) {
        return makeOfflineCurrent(uuid, errorOut);
    }

    const QJsonObject microsoft = entry.value(QLatin1String(kMicrosoftField)).toObject();
    const QJsonObject minecraft = entry.value(QLatin1String(kMinecraftField)).toObject();
    if (microsoft.isEmpty() || minecraft.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("AccountStore", "此账号的登录凭据不完整，请重新登录。");
        }
        return false;
    }

    // 顺序要紧：**先**写微软那份（刷新令牌的来路），再写 Minecraft 那份。反过来的话，
    // 万一第二步失败，盘上就是"新账号的启动令牌 + 旧账号的刷新令牌"—— 能启动一次，
    // 然后刷新出一个别人的令牌。宁可停在旧账号上。
    //
    // R15：光有顺序还不够 —— 第二步（Minecraft 那份）失败时，第一步已经把**新账号的
    // refresh_token** 写进活动槽了，盘上仍然是"新刷新令牌 + 旧启动令牌"。所以动手之前
    // 先把微软那一格的原样字节留一份，第二步失败就原样放回去（不是删掉：原来那格可能是
    // 一位用户的登录态，删掉等于把他登出了）。
    const QByteArray previousMicrosoft = TokenStore::rawValue(TokenStore::microsoftKey());
    if (!TokenStore::writeJson(TokenStore::microsoftKey(), microsoft, errorOut)) {
        return false;
    }
    if (!TokenStore::writeJson(TokenStore::minecraftKey(), minecraft, errorOut)) {
        QString restoreError;
        if (!TokenStore::writeRaw(TokenStore::microsoftKey(), previousMicrosoft, &restoreError)) {
            // 退不回去 = 盘上的活动槽现在是坏的（两份凭据各属一个人）。必须说出来，
            // 不能让用户以为"切换失败，什么都没变"。
            if (errorOut != nullptr) {
                *errorOut = QCoreApplication::translate("AccountStore", "切换账号时无法保存新凭据，也未能恢复原账号的凭据（%1）。\n\n请在此设备上重新登录。")
                                .arg(restoreError);
            }
        } else if (errorOut != nullptr) {
            errorOut->append(QCoreApplication::translate("AccountStore", "\n\n已恢复原账号及其登录状态。"));
        }
        return false;
    }

    QString fileName;
    if (!SettingsSyncRetry::write([&](QSettings &s) { s.setValue(TokenStore::currentUuidKey(), uuid); },
                                  &fileName)) {
        // 在线账号的"当前"是从活动槽里那份凭据推出来的，这一格坏了不影响它 —— 只留一行日志。
        qWarning() << QCoreApplication::translate("AccountStore", "保存当前账号设置失败：") << fileName;
    }
    return true;
}

bool AccountStore::remove(const QString &uuid, QString *errorOut) {
    // **先**问清"它是不是当前账号"，再动列表。顺序反过来的话，删离线账号那一条会漏掉
    // 收尾：离线账号的"当前"是靠列表里那一项证实的（见 activeUuid() 第二档），列表一改
    // 就问不出来了 —— 于是活动槽（本来就空的）和 `CurrentAccountUuid` 都不会被清，
    // 盘上留下一个指向不存在账号的孤儿键。
    const bool wasCurrent = activeUuid() == uuid;
    QJsonArray array = readAccounts();
    QJsonArray kept;
    bool found = false;
    for (const QJsonValue &value : array) {
        const QJsonObject item = value.toObject();
        if (item.value(QLatin1String(kUuidField)).toString() == uuid) {
            found = true;
            continue;
        }
        kept.append(item);
    }
    if (!found) {
        return false;
    }
    if (!writeAccounts(kept, nullptr)) {
        return false;
    }

    // 删的是当前账号 → 连活动槽一起清空（等于退出登录），理由见 .hpp 文件头。
    if (wasCurrent) {
        // R13 同一条：清不掉要说出来。列表项已经删了（上面那一步），所以这里返回 false
        // 的语义是"账号删了，但盘上的登录状态没清干净" —— 调用方照这个说。
        QString clearError;
        const bool microsoftCleared = TokenStore::clear(TokenStore::microsoftKey(), &clearError);
        const bool minecraftCleared = TokenStore::clear(TokenStore::minecraftKey(), &clearError);
        const bool currentCleared = TokenStore::clear(TokenStore::currentUuidKey(), &clearError);
        if (!microsoftCleared || !minecraftCleared || !currentCleared) {
            qWarning().noquote() << QCoreApplication::translate("AccountStore", "已移除当前账号，清除登录信息失败：") << clearError;
            if (errorOut != nullptr) {
                *errorOut = QCoreApplication::translate("AccountStore", "账号已从列表移除，但未能清除登录信息（%1）。\n\n请选择「退出登录」重试。")
                                .arg(clearError);
            }
            return false;
        }
        qInfo().noquote() << QCoreApplication::translate("AccountStore", "已移除当前账号并清除其登录信息。");
    }
    return true;
}

bool AccountStore::logout() {
    // 本来就没登录：别去动盘上那三个键（清一遍是空操作，但返回 true 会让界面说一句
    // "已退出登录"，而用户看到的界面本来就是未登录 —— 那句话是假的）。
    if (activeUuid().isEmpty()) {
        return false;
    }
    // 清活动槽之前先把里面那份收进列表，理由见 .hpp —— 和 makeOfflineCurrent() 的写法一样。
    // R8：收不进去而活动槽里那份是**完整**的（列表读不出来 / 写盘失败）→ 这次不登出。
    // 清掉的话，这台机器上唯一那份 refresh_token 就没了，而且用户点的是"登出"、本以为
    // 账号还留在列表里、点一下就能回来 —— 实际上回不来了，只能重走浏览器登录。
    QString captureError;
    if (!captureActive(&captureError)) {
        qWarning().noquote() << QCoreApplication::translate("AccountStore", "退出登录前，保存当前账号失败：") << captureError;
        if (activeCredentialWouldBeLost()) {
            return false;  // 界面那句是"盘上的登录状态没能清干净…"，与事实一致
        }
    }
    // R13：三个键都真的清掉了才返回 true。以前 `TokenStore::clear()` 是 void —— 拿不到
    // 目录锁、或者 QSettings 写失败时它只是往日志里扔一句，这里照样返回 true，界面于是
    // 说"已退出登录"，下次启动却还是那个账号。三个都试一遍（清掉一个是一个），
    // 只要有一个没清掉就如实返回 false。
    QString clearError;
    const bool microsoftCleared = TokenStore::clear(TokenStore::microsoftKey(), &clearError);
    const bool minecraftCleared = TokenStore::clear(TokenStore::minecraftKey(), &clearError);
    const bool currentCleared = TokenStore::clear(TokenStore::currentUuidKey(), &clearError);
    if (!microsoftCleared || !minecraftCleared || !currentCleared) {
        qWarning().noquote() << QCoreApplication::translate("AccountStore", "退出登录时清除登录信息失败：") << clearError;
        return false;
    }
    qInfo().noquote() << QCoreApplication::translate("AccountStore", "已退出登录，账号仍保存在列表中。");
    return true;
}

// ============================ 离线账号 ============================
//
// 一条离线账号 = 列表里的一项 + 一个名字，盘上没有它的任何凭据。"身份"就是那个 uuid，
// 而 uuid 是从名字按游戏自己那套规则推出来的（见 offlineUuidFor()）。

QString AccountStore::offlineUuidFor(const QString &name) {
    // md5("OfflinePlayer:" + 名字) → 把版本位/变体位摆成 RFC 4122 v3。
    // 摆位这一句不能省：游戏在离线模式下给玩家算 uuid 时做的就是这件事，少了它推出来的
    // 是另一个值 —— 而那个值不会报错，只会让"启动器里的身份"和"游戏里的身份"悄悄错开。
    QByteArray digest = QCryptographicHash::hash(
        QByteArrayLiteral("OfflinePlayer:") + name.toUtf8(), QCryptographicHash::Md5);
    digest[6] = char((digest.at(6) & 0x0F) | 0x30);
    digest[8] = char((digest.at(8) & 0x3F) | 0x80);
    return QString::fromLatin1(digest.toHex());
}

bool AccountStore::addOffline(const QString &name, QString *errorOut) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("AccountStore", "离线账号名称不能为空。");
        }
        return false;
    }
    // 长度按官方 ID 那条上限（16）。**下限不跟官方走**：官方要求至少 3 个字符，那是登录
    // 名的规矩；离线玩的时候叫 "ab" 完全正常，跟着卡只会拦下本来能用的人。
    if (trimmed.size() > 16) {
        if (errorOut != nullptr) {
            *errorOut = QCoreApplication::translate("AccountStore", "离线账号名称最多为 16 个字符。");
        }
        return false;
    }
    // 空白和控制字符真的会有问题：这个名字要进 --username、也要当存档和服务器列表里的
    // 标识，中间带一个空格就会在那些地方被切开或者对不上。
    for (const QChar &ch : trimmed) {
        if (ch.isSpace() || ch.isNull() || ch.category() == QChar::Other_Control) {
            if (errorOut != nullptr) {
                *errorOut = QCoreApplication::translate("AccountStore", "离线账号名称不能含空格或控制字符。");
            }
            return false;
        }
    }

    const QString uuid = offlineUuidFor(trimmed);
    QJsonObject entry;
    entry.insert(QLatin1String(kUuidField), uuid);
    entry.insert(QLatin1String(kNameField), trimmed);
    entry.insert(QLatin1String(kSavedAtField), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    entry.insert(QLatin1String(kOfflineField), true);

    // 同名 = 同一个 uuid = 同一个人，就地更新，否则追加。
    //
    // NOTE: 名字是**逐字节**比的，不是"改个大小写也算同一个"：offlineUuidFor() 是
    // md5("OfflinePlayer:" + name)，大小写不同就是
    // 不同的字节、不同的 uuid —— `Steve` 和 `steve` 在这份列表里是两条，点它们进的是
    // 两个不同的离线身份；按"大小写不敏感"写测试会写出一个跟代码相反的期望值。
    // NOTE: 同 captureActive()：列表读不出来就不写，否则这一下会把列表里其它账号抹掉。
    bool readable = false;
    QJsonArray array = readAccounts(&readable);
    if (!readable) {
        if (errorOut != nullptr) {
            *errorOut = unreadableListMessage();
        }
        return false;
    }
    bool replaced = false;
    for (int i = 0; i < array.size(); ++i) {
        if (array.at(i).toObject().value(QLatin1String(kUuidField)).toString() == uuid) {
            array.replace(i, entry);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        array.append(entry);
    }
    // 先列表后"当前"：列表没写成功就什么都不改，免得盘上记着一个列表里根本没有的账号。
    if (!writeAccounts(array, errorOut)) {
        return false;
    }
    return makeOfflineCurrent(uuid, errorOut);
}

bool AccountStore::isOffline(const QString &uuid) {
    return entryFor(uuid).value(QLatin1String(kOfflineField)).toBool();
}

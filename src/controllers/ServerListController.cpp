/**
 * @file ServerListController.cpp
 * @brief `ServerListController` 的实现。
 *
 * 三条链路在这里只保留 `Host` 这一层接口，两处**形状**：
 *
 * ① 四句问话走 `Host::ask()`（那一层才有 `OverlayHost`），这一层只说"问哪一格"。
 * ② "改完重读重测"收成一次 `Host::changed()`：`loadServerList(...)` + `pingServers(...)`
 * 两条调用**必须一起**发生（只重读不重测的话，那一台的图标 /
 * MOTD / 延迟会停在上一次的状态上，看着像"改了名之后这一台失联了"）。
 */

#include "controllers/ServerListController.hpp"

#include "manage/ServerListFile.hpp"

#include <utility>

ServerListController::ServerListController(Host host, QObject *parent)
    : QObject(parent), m_host(std::move(host)) {}

QString ServerListController::displayName(const QString &name) {
    // 「（无名）」—— 空名字不是"缺了个值"，是一条正经的显示规矩（游戏那边显示成未命名）。
    return name.isEmpty() ? tr("（无名）") : name;
}

bool ServerListController::edit(const QString &instanceId, const QString &oldName,
                                const QString &oldAddress) {
    // 两句都**预填现成的**：用户多半只改端口或者只改个名字，从原样起步最省事。
    QString newName;
    if (!m_host.ask(Ask::EditName, oldName, &newName)) {
        return false;  // 取消了 —— 地址那一句也不必问
    }
    QString newAddress;
    if (!m_host.ask(Ask::EditAddress, oldAddress, &newAddress)) {
        return false;
    }
    // 两个字都没变 ⇒ **一个字节都不写**（空转一遍还会把 mtime 推新、把 gzip 那一层重压
    // 一次，盘上那份东西凭什么变？）。
    if (newName == oldName && newAddress == oldAddress) {
        return false;
    }
    QString problem;
    // 路径从 `ServerListFile::pathFor()` 来（唯一出处）—— 读侧（loadServerList）用的也是它，
    // 两边拼错一处就是"改了这份、读的是那份"。
    if (!ServerListFile::updateEntry(ServerListFile::pathFor(instanceId), oldName, oldAddress,
                                     newName, newAddress, &problem)) {
        m_host.notice(Notice{Notice::Kind::UpdateFailed, problem});
        return false;
    }
    m_host.log(tr("已改服务器：%1（%2）→ %3（%4）")
                   .arg(displayName(oldName), oldAddress, displayName(newName), newAddress));
    // 改完重列一遍（**不新开一套"只换一行"的路**）：新表会让那一台的图标 / MOTD / 延迟回到
    // "还没测"，所以顺手照「刷新」那一路重测一遍 —— 用户看见的就是"这一台现在的样子"，
    // 而不是一个改了名、底下还挂着旧状态的半截页面。
    m_host.changed(instanceId);
    return true;
}

bool ServerListController::add(const QString &instanceId) {
    // 名字可以留空：`servers.dat` 里的 `name` 真能是空串（游戏那边显示成未命名），所以那一
    // 格得明说"留空也行"，不然用户会以为填不满就不让过。
    QString name;
    if (!m_host.ask(Ask::AddName, QString(), &name)) {
        return false;  // 取消了 —— 地址那一句也不必问
    }
    QString address;
    if (!m_host.ask(Ask::AddAddress, QString(), &address)) {
        return false;
    }
    // 地址必填。这一道**在问话之后立刻挡**，不让它一路走到落盘再报错 —— 用户刚填完的那一格
    // 是什么，就在这儿说清楚（和 `updateEntry` 那边同一句话）。
    if (address.trimmed().isEmpty()) {
        m_host.notice(Notice{Notice::Kind::AddRejected, QString()});
        return false;
    }
    QString problem;
    if (!ServerListFile::addEntry(ServerListFile::pathFor(instanceId), name, address, &problem)) {
        m_host.notice(Notice{Notice::Kind::AddFailed, problem});
        return false;
    }
    m_host.log(tr("已加服务器：%1（%2）").arg(displayName(name), address));
    m_host.changed(instanceId);
    return true;
}

bool ServerListController::remove(const QString &instanceId, const QString &name,
                                  const QString &address) {
    // 那句问话**必须点名字**（用户点的是哪一台，问的就是哪一台）：里面那台没名字的服务器
    // 光说"这一台"的话，两台没名字的在屏幕上是同一个问题。
    if (!m_host.confirmRemove(displayName(name), address)) {
        return false;
    }
    QString problem;
    // 按值认那一条（名字 + 地址，退一步只按地址）—— 见 `ServerListFile` 那段：不许按下标。
    if (!ServerListFile::removeEntry(ServerListFile::pathFor(instanceId), name, address,
                                     &problem)) {
        m_host.notice(Notice{Notice::Kind::RemoveFailed, problem});
        return false;
    }
    m_host.log(tr("已删服务器：%1（%2）").arg(displayName(name), address));
    m_host.changed(instanceId);
    return true;
}

bool ServerListController::removeMany(const QString &instanceId,
                                      const QList<QPair<QString, QString>> &entries) {
    if (entries.isEmpty() || !m_host.confirmRemoveMany(static_cast<int>(entries.size()))) {
        return false;
    }
    QString problem;
    if (!ServerListFile::removeEntries(ServerListFile::pathFor(instanceId), entries, &problem)) {
        m_host.notice(Notice{Notice::Kind::RemoveFailed, problem});
        return false;
    }
    m_host.log(tr("已删服务器：%1 台").arg(entries.size()));
    m_host.changed(instanceId);
    return true;
}

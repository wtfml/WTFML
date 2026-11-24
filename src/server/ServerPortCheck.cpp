/**
 * @file ServerPortCheck.cpp
 * @brief 端口占用那一次的落点（形状与取舍见头文件）。
 */

#include "server/ServerPortCheck.hpp"

#include <QHostAddress>
#include <QObject>
#include <QTcpServer>

namespace ServerPortCheck {

State stateOf(int port, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (port < 1 || port > 65535) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("端口 %1 超出有效范围（1–65535）。").arg(port);
        }
        return State::Unknown;
    }
    QTcpServer probe;
    // 听的是 `Any`（0.0.0.0）而不是 `LocalHost`：服务端自己听的就是 `Any`，而"127.0.0.1
    // 上没人占"和"这个端口能拿来开服"是两件事 —— 别的程序在别的网卡地址上占着同一个端口号
    // 时，服务端起手那一句 `bind` 会失败。
    const bool listening = probe.listen(QHostAddress::Any, static_cast<quint16>(port));
    if (listening) {
        probe.close();
        return State::Free;
    }
    // 没听上：**先说清是"被占"还是"不许听"**。`AddressInUseError` 才是"有人占着"；其余的
    // （权限、网卡没了、系统把它划进了保留段）一律 Unknown —— 见文件头那段。
    if (probe.serverError() == QAbstractSocket::AddressInUseError) {
        if (problemOut != nullptr) {
            *problemOut = QObject::tr("这个端口现在有别的程序在用。");
        }
        return State::InUse;
    }
    if (problemOut != nullptr) {
        *problemOut = probe.errorString().isEmpty()
                          ? QObject::tr("无法在指定端口启动测试监听。")
                          : probe.errorString();
    }
    return State::Unknown;
}

}  // namespace ServerPortCheck

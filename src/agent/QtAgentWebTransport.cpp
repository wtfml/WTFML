/**
 * @file QtAgentWebTransport.cpp
 * @brief 直连 + pinned-IP 的 HTTP 实现（见 .hpp）。
 */

#include "QtAgentWebTransport.hpp"

#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>

QString hostHeaderFor(const QUrl &url) {
    QString host = url.host();
    const int defaultPort = url.scheme() == QStringLiteral("https") ? 443 : 80;
    if (url.port() > 0 && url.port() != defaultPort) {
        host += QStringLiteral(":%1").arg(url.port());
    }
    return host;
}

QtAgentWebTransport::QtAgentWebTransport(QObject *parent) : QObject(parent) {
    m_manager = new QNetworkAccessManager(this);
    // 第一版**不继承系统代理**：代理可能自行解析目标、绕过 pinned-IP 校验（§9.5）。
    m_manager->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    m_manager->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    m_manager->setTransferTimeout(AgentWebPolicy::kTimeoutMs);
}

void QtAgentWebTransport::resolve(const QString &host, ResolveCallback done) {
    QHostInfo::lookupHost(host, this, [done](const QHostInfo &info) {
        if (info.error() != QHostInfo::NoError) {
            if (done) done(false, {}, info.errorString());
            return;
        }
        QList<QHostAddress> addresses = info.addresses();
        if (addresses.isEmpty()) {
            if (done) done(false, {}, tr("域名无法解析为有效地址。"));
            return;
        }
        if (done) done(true, addresses, QString());
    });
}

void QtAgentWebTransport::get(const AgentHttpRequest &request, ResponseCallback done) {
    QNetworkRequest networkRequest(request.pinnedUrl);
    // Host 头用原 host；证书主机名也用原 host —— 连接的是已校验的 IP（pinned-IP）。
    networkRequest.setRawHeader("Host", request.hostHeader.toUtf8());
    networkRequest.setRawHeader("User-Agent", "WTFML-Agent/1");
    if (!request.accept.isEmpty()) {
        networkRequest.setRawHeader("Accept", request.accept.toUtf8());
    }
    // TLS 主机名校验用原 host（连接的是已校验的 IP）。
    networkRequest.setPeerVerifyName(request.peerVerifyName);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                               QNetworkRequest::ManualRedirectPolicy);
    networkRequest.setTransferTimeout(request.timeoutMs);

    QNetworkReply *reply = m_manager->get(networkRequest);
    auto shared = std::make_shared<AgentHttpResponse>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, shared] {
        const QByteArray chunk = reply->readAll();
        if (shared->body.size() + chunk.size() > AgentWebPolicy::kMaxResponseBytes) {
            shared->truncated = true;
            const qint64 room = AgentWebPolicy::kMaxResponseBytes - shared->body.size();
            if (room > 0) shared->body += chunk.left(int(room));
            reply->abort();
            return;
        }
        shared->body += chunk;
    });
    connect(reply, &QNetworkReply::finished, this, [reply, shared, done, request] {
        shared->body += reply->readAll();
        if (shared->body.size() > AgentWebPolicy::kMaxResponseBytes) {
            shared->body.truncate(int(AgentWebPolicy::kMaxResponseBytes));
            shared->truncated = true;
        }
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        shared->status = status.isValid() ? status.toInt() : 0;
        shared->contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (reply->hasRawHeader("Location")) {
            shared->location = QString::fromUtf8(reply->rawHeader("Location"));
        }
        const QNetworkReply::NetworkError error = reply->error();
        if (error != QNetworkReply::NoError && error != QNetworkReply::OperationCanceledError) {
            shared->error = reply->errorString();
        } else if (error == QNetworkReply::OperationCanceledError && !shared->truncated) {
            shared->error = tr("请求超时或已取消。");
        }
        shared->ok = shared->error.isEmpty();
        reply->deleteLater();
        if (done) done(*shared);
    });
}

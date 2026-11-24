#pragma once
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

struct DirectDownloadRequest {
    QUrl url;
    QString path;
    static bool parse(const QString &text, const QString &directory, QString name,
                      DirectDownloadRequest *out, QString *error) {
        const auto reject = [error](const QString &reason) { if (error) *error = reason; return false; };
        const QUrl url(text.trimmed(), QUrl::StrictMode);
        if (!url.isValid() || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) || url.host().isEmpty())
            return reject(QStringLiteral("请输入有效的 HTTP 或 HTTPS 文件直链。"));
        if (!url.userInfo().isEmpty()) return reject(QStringLiteral("不接受地址中内嵌的用户名或密码。"));
        if (!QDir::isAbsolutePath(directory)) return reject(QStringLiteral("保存目录必须是绝对路径。"));
        if (name.isEmpty()) name = QFileInfo(url.path()).fileName();
        if (name.isEmpty()) name = QStringLiteral("download.bin");
        static const QRegularExpression invalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
        static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
        if (name == QStringLiteral(".") || name == QStringLiteral("..") || invalid.match(name).hasMatch() ||
            reserved.match(name).hasMatch() || name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
            return reject(QStringLiteral("文件名无效：不能包含路径、保留名称或特殊字符。"));
        const QString path = QDir(directory).absoluteFilePath(name);
        if (QFileInfo(path).isDir()) return reject(QStringLiteral("目标位置是文件夹，请更换文件名。"));
        if (out) *out = {url, QDir::cleanPath(path)};
        if (error) error->clear();
        return true;
    }
};

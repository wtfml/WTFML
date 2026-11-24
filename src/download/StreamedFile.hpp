#pragma once

#include <QCryptographicHash>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QNetworkReply>
#include <functional>

#include "ZipMagic.hpp"

// 一个回复一个临时文件。校验之前不替换原文件；最多读取 64 KiB，不缓存整份下载。
class StreamedFile {
public:
    explicit StreamedFile(const QString &path, bool overwrite = true) : file(path), hash(QCryptographicHash::Sha1), allowOverwrite(overwrite) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) error = file.errorString();
    }
    /// 把回复当下能读的都读进临时文件（不限量）。返回值 = 这一趟真正吃进去的字节数。
    qint64 drain(QNetworkReply *reply) { return drainUpTo(reply, -1); }

    /// 同上，但**最多读 maxBytes 个字节**（-1 = 不限）。限速泵按这一格"喂"回复。
    ///
    /// （下载限速）：抽成"限量版"之后，`drain()` 仍然是一个字节都不少
    /// 的行为 —— 收尾那一趟（`onDownloadReplyFinished`）必须用它把缓冲里剩下的读完，
    /// 否则大小/SHA1 校验会对着一个少了尾巴的文件判失败。
    qint64 drainUpTo(QNetworkReply *reply, qint64 maxBytes) {
        if (!error.isEmpty()) return 0;
        if (!reply->isReadable()) return 0;
        qint64 taken = 0;
        while (reply->bytesAvailable() > 0) {
            qint64 want = 64 * 1024;
            if (maxBytes >= 0) {
                const qint64 left = maxBytes - taken;
                if (left <= 0) break;
                want = qMin(want, left);
            }
            const QByteArray chunk = reply->read(want);
            if (chunk.isEmpty()) break;
            if (prefix.size() < 4) prefix += chunk.left(4 - prefix.size());
            if (file.write(chunk) != chunk.size()) { error = file.errorString(); break; }
            hash.addData(chunk);
            bytes += chunk.size();
            taken += chunk.size();
        }
        return taken;
    }
    QString finish(const QString &sha1, qint64 size, bool zip) {
        if (!error.isEmpty()) return error;
        if (bytes == 0) return QStringLiteral("Server returned an empty body");
        if (size > 0 && bytes != size) return QStringLiteral("Downloaded size does not match the expected size");
        if (!sha1.isEmpty() && QString::fromLatin1(hash.result().toHex()).compare(sha1, Qt::CaseInsensitive) != 0)
            return QStringLiteral("SHA1 verification failed");
        if (zip && sha1.isEmpty() && !ZipMagic::looksLikeZipHead(prefix))
            return QStringLiteral("Not a zip archive (expected a jar/zip body)");
        if (!allowOverwrite && QFileInfo::exists(file.fileName())) return QStringLiteral("目标文件已存在，未覆盖。请更换文件名或确认覆盖。");
        if (commitGuard) { const auto problem = commitGuard(); if (!problem.isEmpty()) return problem; }
        if (!file.commit()) return file.errorString();
        return {};
    }
    QSaveFile file;
    QCryptographicHash hash;
    qint64 bytes = 0;
    QByteArray prefix;
    QString error;
    bool allowOverwrite;
    std::function<QString()> commitGuard;
};

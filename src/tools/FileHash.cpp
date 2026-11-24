/**
 * @file FileHash.cpp
 * @brief FileHash 的实现（那几条规矩在 .hpp 开头那一段）。
 */
#include "FileHash.hpp"
#include <QCoreApplication>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace {

QCryptographicHash::Algorithm toQt(FileHash::Algorithm algorithm) {
    switch (algorithm) {
    case FileHash::Algorithm::Sha256:
        return QCryptographicHash::Sha256;
    case FileHash::Algorithm::Sha1:
        return QCryptographicHash::Sha1;
    case FileHash::Algorithm::Md5:
        break;
    }
    return QCryptographicHash::Md5;
}

/// 归一化：只留下十六进制字符、全部小写。
///
/// 空白和 `-` `:` 一律丢掉 —— 它们只是**分隔符**，各家写法不一样（`3f79 bb7b`、`3f-79`、
/// `sha256:3f79`），而它们指的都是同一串十六进制。不归一化的话，用户拿着一个其实正确的
/// 文件会看到"不一致"，然后去重新下载一遍。
QString normalizeHex(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.isSpace() || ch == QChar('-') || ch == QChar(':')) {
            continue;
        }
        out.append(ch.toLower());
    }
    return out;
}

/// 把输入里那个"算法那一半"读出来（`sha256:` / `SHA-256 =` / `md5 `）。
///
/// 返回 true 表示输入里确实点名了一档；@p rest 是去掉那一段之后的正文。
bool readAlgorithmPrefix(const QString &text, FileHash::Algorithm *algorithm, QString *rest) {
    const QString head = text.trimmed();
    // 几种分隔：`sha256:xxxx`、`SHA-256 = xxxx`、`md5 <hex>`。
    static const struct {
        const char *key;
        FileHash::Algorithm algorithm;
    } kKeys[] = {
        {"sha256", FileHash::Algorithm::Sha256},
        {"sha-256", FileHash::Algorithm::Sha256},
        {"sha1", FileHash::Algorithm::Sha1},
        {"sha-1", FileHash::Algorithm::Sha1},
        {"md5", FileHash::Algorithm::Md5},
    };
    for (const auto &entry : kKeys) {
        const QString key = QString::fromLatin1(entry.key);
        if (!head.startsWith(key, Qt::CaseInsensitive)) {
            continue;
        }
        // 认了前几个字还得看**后面跟着什么**：`sha256something` 不是一个前缀。
        //
        // 这一段**不能先 trimmed()**（R12）：空格本身也可能就是分隔符，先 trim 掉之后
        // `md5 <hex>` 的正文直接贴着算法名，会落到下面"不是分隔符"那一档里去 ——
        // 于是 `md5 <hex>` 这种文档写明的写法永远认不出来。
        QString tail = head.mid(key.size());
        if (tail.isEmpty()) {
            return false;  // 光写了算法、没写摘要
        }
        const QChar separator = tail.front();
        if (!(separator.isSpace() || separator == QChar(':') || separator == QChar('='))) {
            return false;
        }
        tail = tail.mid(1).trimmed();
        // `SHA-256 = xxxx`：空格之后还跟着 `=` / `:` 时一并吃掉（只吃一个）。
        if (!tail.isEmpty()
            && (tail.startsWith(QChar(':')) || tail.startsWith(QChar('=')))) {
            tail = tail.mid(1).trimmed();
        }
        if (tail.isEmpty()) {
            return false;  // 光写了算法、没写摘要
        }
        *algorithm = entry.algorithm;
        *rest = tail;
        return true;
    }
    return false;
}

}  // namespace

namespace FileHash {

QString algorithmName(Algorithm algorithm) {
    switch (algorithm) {
    case Algorithm::Sha256:
        return QStringLiteral("SHA-256");
    case Algorithm::Sha1:
        return QStringLiteral("SHA-1");
    case Algorithm::Md5:
        break;
    }
    return QStringLiteral("MD5");
}

QString algorithmKey(Algorithm algorithm) {
    switch (algorithm) {
    case Algorithm::Sha256:
        return QStringLiteral("sha256");
    case Algorithm::Sha1:
        return QStringLiteral("sha1");
    case Algorithm::Md5:
        break;
    }
    return QStringLiteral("md5");
}

QList<Algorithm> algorithms() {
    return {Algorithm::Sha256, Algorithm::Sha1, Algorithm::Md5};
}

int hexLength(Algorithm algorithm) {
    switch (algorithm) {
    case Algorithm::Sha256:
        return 64;
    case Algorithm::Sha1:
        return 40;
    case Algorithm::Md5:
        break;
    }
    return 32;
}

QString digestOf(const QByteArray &bytes, Algorithm algorithm) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, toQt(algorithm)).toHex());
}

Result digestOfFile(const QString &path, Algorithm algorithm) {
    return digestOfFile(path, algorithm, {}, {});
}

Result digestOfFile(const QString &path, Algorithm algorithm, const ProgressFn &progress,
                    const CancelFn &cancelled) {
    Result result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QCoreApplication::translate("FileHash", "无法打开文件：%1").arg(file.errorString());
        return result;
    }
    const qint64 total = file.size();
    QCryptographicHash hash(toQt(algorithm));
    qint64 done = 0;
    QByteArray buffer;
    buffer.resize(kChunkBytes);
    while (!file.atEnd()) {
        if (cancelled && cancelled()) {
            result.error = QCoreApplication::translate("FileHash", "已取消");
            return result;
        }
        const qint64 read = file.read(buffer.data(), buffer.size());
        if (read < 0) {
            result.error = QCoreApplication::translate("FileHash", "读取文件失败：%1").arg(file.errorString());
            return result;
        }
        if (read == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), read));
        done += read;
        if (progress) {
            progress(done, total);
        }
    }
    result.ok = true;
    result.hex = QString::fromLatin1(hash.result().toHex());
    result.bytes = done;
    return result;
}

Stamp stampOf(const QString &path) {
    Stamp stamp;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return stamp;
    }
    stamp.ok = true;
    stamp.bytes = info.size();
    stamp.lastModifiedMs = info.lastModified().toMSecsSinceEpoch();
    return stamp;
}

bool sameStamp(const Stamp &left, const Stamp &right) {
    return left.ok && right.ok && left.bytes == right.bytes
           && left.lastModifiedMs == right.lastModifiedMs;
}

Expected parseExpected(const QString &text) {
    Expected expected;
    QString body = text.trimmed();
    if (body.isEmpty()) {
        return expected;  // 空着是正常的（"我只想看摘要"），不算错
    }
    Algorithm prefixed = Algorithm::Sha256;
    QString rest;
    if (readAlgorithmPrefix(body, &prefixed, &rest)) {
        expected.hasAlgorithm = true;
        expected.algorithm = prefixed;
        body = rest;
    }
    const QString hex = normalizeHex(body);
    if (hex.isEmpty()) {
        expected.error = QCoreApplication::translate("FileHash", "请输入十六进制摘要。");
        return expected;
    }
    for (const QChar ch : hex) {
        if (!((ch >= QChar('0') && ch <= QChar('9')) || (ch >= QChar('a') && ch <= QChar('f')))) {
            expected.error = QCoreApplication::translate("FileHash", "`%1` 不是十六进制的字符。").arg(ch);
            return expected;
        }
    }
    // 长度自己就说明了是哪一档 —— 这一步是为了"用户没写算法时也能对上"。
    Algorithm byLength = Algorithm::Sha256;
    bool lengthKnown = false;
    for (const Algorithm algorithm : algorithms()) {
        if (hex.size() == hexLength(algorithm)) {
            byLength = algorithm;
            lengthKnown = true;
            break;
        }
    }
    if (!lengthKnown) {
        expected.error = QCoreApplication::translate("FileHash", "摘要有 %1 个字符。SHA-256、SHA-1 和 MD5 分别要求 64、40 和 32 个十六进制字符。")
                             .arg(hex.size());
        return expected;
    }
    if (expected.hasAlgorithm && expected.algorithm != byLength) {
        expected.error = QCoreApplication::translate("FileHash", "摘要长度与所选算法不匹配。%1 要求 %2 个字符，实际为 %3 个。")
                             .arg(algorithmName(expected.algorithm))
                             .arg(hexLength(expected.algorithm))
                             .arg(hex.size());
        return expected;
    }
    if (!expected.hasAlgorithm) {
        expected.algorithm = byLength;  // 长度自己说了算
    }
    expected.valid = true;
    expected.hex = hex;
    return expected;
}

Verdict compare(const QString &actualHex, const Expected &expected) {
    Verdict verdict;
    verdict.expectedAlgorithm = expected.algorithm;
    verdict.expectedHasAlgorithm = expected.hasAlgorithm;
    if (!expected.valid) {
        verdict.text = expected.error.isEmpty() ? QCoreApplication::translate("FileHash", "无已知值可比。") : expected.error;
        return verdict;
    }
    verdict.valid = true;
    const QString actual = normalizeHex(actualHex);
    verdict.matches = (actual == expected.hex);
    if (verdict.matches) {
        verdict.text = QCoreApplication::translate("FileHash", "一致。");
    } else {
        verdict.text = QCoreApplication::translate("FileHash", "不一致：实际值为 %1。").arg(actual.isEmpty()
                                                                       ? QCoreApplication::translate("FileHash", "（未计算）")
                                                                       : actual);
    }
    return verdict;
}

QString fileSizeText(qint64 bytes) {
    if (bytes < 0) {
        return QString();
    }
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 1);
    }
    if (bytes < qint64(1024) * 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(double(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

}  // namespace FileHash

#include "LogFileReader.hpp"

#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>
#include <QTemporaryDir>

#include "ZipUtil.hpp"

namespace LogFileReader {

bool isGzipHead(const QByteArray &head) {
    return head.size() >= 2 && static_cast<quint8>(head.at(0)) == 0x1f
           && static_cast<quint8>(head.at(1)) == 0x8b;
}

bool isGzipFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && isGzipHead(file.read(2));
}

namespace {

/// 要读的普通文件路径：gzip 解到 @p temp 里，其余原样返回。失败回空串并写 @p problem。
QString plainPathFor(const QString &path, QTemporaryDir *temp, QString *problem) {
    const QFileInfo info(path);
    if (!info.isFile()) {
        *problem = QStringLiteral("找不到这个文件。");
        return {};
    }
    if (!isGzipFile(path)) return path;
    if (!temp->isValid()) {
        *problem = QStringLiteral("解压这一步腾不出临时目录。");
        return {};
    }
    const QString plain = temp->filePath(QStringLiteral("log.txt"));
    QString error;
    if (!ZipUtil::gunzipTo(path, plain, &error)) {
        *problem = QStringLiteral("这份文件是 gzip 压缩的，但解不开：%1").arg(error);
        return {};
    }
    if (!QFileInfo(plain).isFile() || QFileInfo(plain).size() <= 0) {
        *problem = QStringLiteral("解压内容为空，文件可能损坏或没有写完。");
        return {};
    }
    return plain;
}

}  // namespace

QStringList splitLines(const QByteArray &bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder(bytes);
    // 老版本启动器 / 部分整合包按系统代码页写日志：不是合法 UTF-8 就退回本地编码。
    if (decoder.hasError()) text = QString::fromLocal8Bit(bytes);
    QStringList lines = text.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
    }
    return lines;
}

Loaded load(const QString &path, qint64 maxBytes) {
    Loaded result;
    QTemporaryDir temp;
    const QString plain = plainPathFor(path, &temp, &result.problem);
    if (plain.isEmpty()) return result;
    QFile file(plain);
    if (!file.open(QIODevice::ReadOnly)) {
        result.problem = QStringLiteral("打不开这个文件：%1").arg(file.errorString());
        return result;
    }
    const qint64 size = file.size();
    qint64 skipped = 0;
    if (maxBytes > 0 && size > maxBytes) {
        skipped = size - maxBytes;
        file.seek(skipped);
    }
    QByteArray bytes = file.readAll();
    if (skipped > 0) {
        // 从下一个换行之后开始，首行才是完整的；整块都没有换行（一整行超长）就原样留着。
        const qsizetype nl = bytes.indexOf('\n');
        if (nl >= 0) {
            bytes.remove(0, nl + 1);
            skipped += nl + 1;
        }
    }
    result.cutHead = skipped;
    result.lines = splitLines(bytes);
    result.ok = true;
    return result;
}

Counted countLines(const QString &path) {
    Counted result;
    QTemporaryDir temp;
    const QString plain = plainPathFor(path, &temp, &result.problem);
    if (plain.isEmpty()) return result;
    QFile file(plain);
    if (!file.open(QIODevice::ReadOnly)) {
        result.problem = QStringLiteral("打不开这个文件：%1").arg(file.errorString());
        return result;
    }
    constexpr qint64 kChunk = 1 << 20;
    char last = '\n';
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kChunk);
        if (chunk.isEmpty()) break;
        result.lines += chunk.count('\n');
        last = chunk.back();
    }
    if (last != '\n') ++result.lines;
    result.ok = true;
    return result;
}

}  // namespace LogFileReader

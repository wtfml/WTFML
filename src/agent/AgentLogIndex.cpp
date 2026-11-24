/**
 * @file AgentLogIndex.cpp
 * @brief 行索引、流式搜索与范围读取（见 .hpp）。
 */

#include "AgentLogIndex.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QVector>

#include <algorithm>

namespace {

constexpr int kMaxMatches = 200;
constexpr int kMaxLines = 500;
constexpr int kMaxChars = 256 * 1024;
constexpr int kChunkBytes = 512 * 1024;
/// 单行超过这个长度就不再等换行：日志里偶尔有一行把整个 NBT 打出来。
constexpr int kMaxLineBytes = 512 * 1024;
constexpr int kMaxStoredLineChars = 4000;
constexpr int kMaxQueryLength = 400;
constexpr int kIndexEntries = 4;
constexpr int kMaxIndexLines = 4000000;
constexpr qint64 kMaxIndexBytes = 1024ll * 1024 * 1024;

struct CacheEntry {
    qint64 size = -1;
    qint64 modifiedMs = 0;
    QVector<qint64> offsets;   ///< 每行起始字节
    quint64 lastUsed = 0;
};

QMutex &cacheMutex() {
    static QMutex mutex;
    return mutex;
}

QHash<QString, CacheEntry> &cache() {
    static QHash<QString, CacheEntry> entries;
    return entries;
}

quint64 &clockValue() {
    static quint64 value = 0;
    return value;
}

QString tr(const char *text) {
    return QCoreApplication::translate("AgentLogIndex", text);
}

QString lineText(const QByteArray &raw) {
    QByteArray bytes = raw;
    if (bytes.endsWith('\r')) bytes.chop(1);
    QString text = QString::fromUtf8(bytes);
    if (text.size() > kMaxStoredLineChars) text = text.left(kMaxStoredLineChars);
    return text;
}

/// 建索引：只记每行起始偏移，不保内容。
bool buildIndex(const QString &path, const QFileInfo &info, CacheEntry *entry, QString *problem) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = tr("日志读不动。");
        return false;
    }
    entry->offsets.clear();
    entry->offsets.append(0);
    qint64 position = 0;
    while (true) {
        const QByteArray chunk = file.read(kChunkBytes);
        if (chunk.isEmpty()) break;
        const qint64 chunkStart = position;
        position += chunk.size();
        int newline = -1;
        while ((newline = chunk.indexOf('\n', newline + 1)) >= 0) {
            entry->offsets.append(chunkStart + newline + 1);
            if (entry->offsets.size() > kMaxIndexLines) break;
        }
        if (entry->offsets.size() > kMaxIndexLines) break;
        if (position > kMaxIndexBytes) break;
    }
    // 文件以换行结尾时最后一个偏移是空行起点，不算一行。
    if (entry->offsets.size() > 1 && entry->offsets.constLast() >= info.size()) {
        entry->offsets.removeLast();
    }
    entry->size = info.size();
    entry->modifiedMs = info.lastModified().toMSecsSinceEpoch();
    entry->lastUsed = ++clockValue();
    return true;
}

/// 拿到当前文件的索引；缓存按大小 + 修改时间失效。
bool ensureIndex(const QString &path, CacheEntry *out, QString *problem) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        if (problem != nullptr) *problem = tr("日志文件不存在。");
        return false;
    }
    const qint64 size = info.size();
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    {
        QMutexLocker locker(&cacheMutex());
        auto it = cache().find(path);
        if (it != cache().end() && it->size == size && it->modifiedMs == modifiedMs) {
            it->lastUsed = ++clockValue();
            *out = it.value();
            return true;
        }
    }
    CacheEntry entry;
    if (!buildIndex(path, info, &entry, problem)) return false;
    QMutexLocker locker(&cacheMutex());
    if (cache().size() >= kIndexEntries && !cache().contains(path)) {
        QString oldestKey;
        quint64 oldest = ~quint64(0);
        for (auto it = cache().constBegin(); it != cache().constEnd(); ++it) {
            if (it->lastUsed < oldest) {
                oldest = it->lastUsed;
                oldestKey = it.key();
            }
        }
        cache().remove(oldestKey);
    }
    cache().insert(path, entry);
    *out = entry;
    return true;
}

}  // namespace

namespace AgentLogIndex {

int maxMatchesLimit() { return kMaxMatches; }
int maxLinesLimit() { return kMaxLines; }
int maxCharsLimit() { return kMaxChars; }

void clearCache() {
    QMutexLocker locker(&cacheMutex());
    cache().clear();
}

bool search(const QString &path, const SearchOptions &options, SearchResult *result,
            QString *problem) {
    if (problem != nullptr) problem->clear();
    if (result == nullptr) return false;
    *result = SearchResult{};
    const QString query = options.query.trimmed();
    if (query.isEmpty()) {
        if (problem != nullptr) *problem = tr("搜索词不能为空。");
        return false;
    }
    if (query.size() > kMaxQueryLength) {
        if (problem != nullptr) *problem = tr("搜索词超过 %1 个字符。").arg(kMaxQueryLength);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = tr("日志读不动。");
        return false;
    }
    const int before = qBound(0, options.before, 10);
    const int after = qBound(0, options.after, 10);
    const int maxMatches = qBound(1, options.maxMatches, kMaxMatches);
    const int maxChars = qBound(1000, options.maxChars, kMaxChars);
    const qint64 budgetMs = qBound<qint64>(qint64(500), options.budgetMs, qint64(15000));

    // 纯 ASCII 的搜索词直接在 UTF-8 字节上找，省掉每一行的 QString 解码
    // （100 MB / 160 万行的日志里这一步占大头，见 docs/dev/PERFORMANCE.md）。
    bool asciiQuery = true;
    for (const QChar character : query) {
        if (character.unicode() > 127) asciiQuery = false;
    }
    const QByteArray queryBytes = query.toUtf8();
    const QByteArray lowerQueryBytes = queryBytes.toLower();

    struct Pending {
        int matchIndex = 0;
        int remaining = 0;
    };
    QList<Pending> pending;
    QList<QPair<int, QByteArray>> previousRaw;
    int lineNumber = 0;
    int chars = 0;
    bool countOnly = false;   ///< 命中条数或字符预算已满：只数总数，不再收内容
    bool budgetExceeded = false;
    QElapsedTimer timer;
    timer.start();

    auto store = [&](const QByteArray &raw) {
        ++lineNumber;
        QString decoded;
        bool decodedReady = false;
        const auto decode = [&]() -> QString {
            if (!decodedReady) {
                decoded = lineText(raw);
                decodedReady = true;
            }
            return decoded;
        };
        // 先把上一条命中还欠的“后面几行”补上，再判断当前行自己是不是一条新命中。
        if (!pending.isEmpty()) {
            const QString text = decode();
            for (Pending &item : pending) {
                if (item.remaining <= 0) continue;
                if (item.matchIndex < 0 || item.matchIndex >= result->matches.size()) continue;
                result->matches[item.matchIndex].context.append(Line{lineNumber, text});
                chars += text.size();
                item.remaining -= 1;
            }
            pending.erase(std::remove_if(pending.begin(), pending.end(),
                                         [](const Pending &item) { return item.remaining <= 0; }),
                          pending.end());
        }
        const bool hit = asciiQuery
                             ? raw.toLower().contains(lowerQueryBytes)
                             : decode().contains(query, Qt::CaseInsensitive);
        if (hit) {
            ++result->totalMatches;
            if (!countOnly && result->matches.size() < maxMatches) {
                Match match;
                match.line = lineNumber;
                match.text = decode();
                for (const auto &item : previousRaw) {
                    const QString text = lineText(item.second);
                    match.context.append(Line{item.first, text});
                    chars += text.size();
                }
                chars += match.text.size();
                result->matches.append(match);
                if (after > 0) pending.append(Pending{int(result->matches.size()) - 1, after});
                if (result->matches.size() >= maxMatches) {
                    countOnly = true;
                    result->truncated = true;
                }
            } else {
                countOnly = true;
                result->truncated = true;
            }
        }
        if (chars > maxChars) {
            countOnly = true;
            result->truncated = true;
        }
        if (before > 0) {
            // 上下文行也只在真要输出时才解码：这里先留原始字节。
            previousRaw.append({lineNumber, raw});
            while (previousRaw.size() > before) previousRaw.removeFirst();
        } else if (!previousRaw.isEmpty()) {
            previousRaw.clear();
        }
    };

    QByteArray buffer;
    bool skippingLongLine = false;
    while (true) {
        if (timer.elapsed() > budgetMs) {
            budgetExceeded = true;
            result->truncated = true;
            break;
        }
        const QByteArray chunk = file.read(kChunkBytes);
        if (chunk.isEmpty()) break;
        result->bytes += chunk.size();
        if (skippingLongLine) {
            const int newline = chunk.indexOf('\n');
            if (newline < 0) continue;
            skippingLongLine = false;
            buffer = chunk.mid(newline + 1);
        } else {
            buffer.append(chunk);
        }
        int start = 0;
        while (true) {
            const int newline = buffer.indexOf('\n', start);
            if (newline < 0) break;
            store(buffer.mid(start, newline - start));
            start = newline + 1;
        }
        buffer.remove(0, start);
        if (buffer.size() > kMaxLineBytes) {
            // 超长的一行：按一行处理一次，剩下的等到下一个换行再继续，行号才不会漂。
            store(buffer.left(kMaxLineBytes));
            buffer.clear();
            skippingLongLine = true;
        }
    }
    if (!buffer.isEmpty() && !budgetExceeded && !skippingLongLine) store(buffer);
    result->scannedLines = lineNumber;
    result->budgetExceeded = budgetExceeded;
    return true;
}

bool readRange(const QString &path, int startLine, int lineCount, int maxChars, LineRange *range,
               QString *problem) {
    if (problem != nullptr) problem->clear();
    if (range == nullptr) return false;
    *range = LineRange{};
    if (startLine < 1) {
        if (problem != nullptr) *problem = tr("startLine 从 1 开始。");
        return false;
    }
    const int count = qBound(1, lineCount, kMaxLines);
    const int charBudget = qBound(1000, maxChars, kMaxChars);
    CacheEntry entry;
    if (!ensureIndex(path, &entry, problem)) return false;
    const int total = entry.offsets.size();
    range->totalLines = total;
    if (startLine > total) {
        range->truncated = true;
        return true;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = tr("日志读不动。");
        return false;
    }
    const qint64 start = entry.offsets.at(startLine - 1);
    const int lastIndex = qMin(startLine + count - 1, total);
    const qint64 end = lastIndex < total ? entry.offsets.at(lastIndex) : file.size();
    if (lastIndex < startLine + count - 1) range->truncated = true;
    if (!file.seek(start)) {
        if (problem != nullptr) *problem = tr("日志定位失败。");
        return false;
    }
    // 一行最多 4 字节/字符（UTF-8 上界），多读 4 KB 兜住最后一行。
    QByteArray buffer = file.read(qMin<qint64>(end - start, qint64(charBudget) * 4 + 4096));
    int index = 0;
    int chars = 0;
    while (range->lines.size() < count) {
        const int newline = buffer.indexOf('\n', index);
        if (newline < 0) break;
        const QString text = lineText(buffer.mid(index, newline - index));
        index = newline + 1;
        chars += text.size();
        if (chars > charBudget) {
            range->truncated = true;
            break;
        }
        range->lines.append(Line{startLine + int(range->lines.size()), text});
    }
    if (index < buffer.size() && range->lines.size() < count && !range->truncated) {
        const QString text = lineText(buffer.mid(index));
        if (chars + text.size() > charBudget) {
            range->truncated = true;
        } else {
            range->lines.append(Line{startLine + int(range->lines.size()), text});
        }
    }
    range->firstLine = startLine;
    range->lastLine = range->lines.isEmpty() ? startLine - 1 : range->lines.constLast().number;
    return true;
}

}  // namespace AgentLogIndex

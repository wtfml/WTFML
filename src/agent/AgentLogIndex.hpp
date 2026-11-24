/**
 * @file AgentLogIndex.hpp
 * @brief 游戏日志/崩溃报告的**流式行索引**：按行号搜索与按范围读取
 *        （docs/dev/AGENT_AUTONOMY.md S5.3 的 `logs.search` / `logs.readRange`）。
 *
 * 为什么不把整份日志读进内存：日志可以到几百 MB，而模型一次只要几十行。这里只建
 * 「每行起始字节」的偏移表（一份 100 MB / 100 万行的日志约 8 MB 偏移），按文件大小 +
 * 修改时间失效；搜索本身整份顺序扫，但只保留命中的那几十行。
 *
 * 这一层不知道日志是谁的、也不做脱敏：调用方给路径，拿到行号和原文，脱敏与投影在工具层。
 * 搜索首版只做字面量（大小写不敏感），不开正则 —— 没有匹配上限引擎时，一条写坏的正则
 * 能在 8 MB 日志上把界面拖死。
 */

#pragma once

#include <QList>
#include <QString>

namespace AgentLogIndex {

struct Line {
    int number = 0;
    QString text;
};

struct SearchOptions {
    QString query;
    int before = 0;      ///< 命中行前面带几行上下文（0–10）
    int after = 0;       ///< 命中行后面带几行上下文（0–10）
    int maxMatches = 50; ///< 1–200
    int maxChars = 24000;///< 结果文本预算（字符）
    qint64 budgetMs = 6000;  ///< 扫描时间预算；用尽后回部分结果并标 truncated
};

struct Match {
    int line = 0;
    QString text;
    QList<Line> context;
};

struct SearchResult {
    QList<Match> matches;
    int totalMatches = 0;
    int scannedLines = 0;
    qint64 bytes = 0;
    bool truncated = false;       ///< 条数/字符/时间任一截断
    bool budgetExceeded = false;
};

/// 按字面量搜索；`query` 为空或过长时返回 false 并给 problem。
bool search(const QString &path, const SearchOptions &options, SearchResult *result,
            QString *problem);

struct LineRange {
    QList<Line> lines;
    int firstLine = 0;
    int lastLine = 0;
    int totalLines = 0;
    bool truncated = false;   ///< 起点越界、行数或字符上限截断
};

/// 从 1 开始的行号。`lineCount` 上限 500，`maxChars` 上限 256 KB（字符）。
bool readRange(const QString &path, int startLine, int lineCount, int maxChars, LineRange *range,
               QString *problem);

/// 丢掉缓存（测试用；正常失效靠文件大小 + 修改时间）。
void clearCache();

int maxMatchesLimit();
int maxLinesLimit();
int maxCharsLimit();

}  // namespace AgentLogIndex

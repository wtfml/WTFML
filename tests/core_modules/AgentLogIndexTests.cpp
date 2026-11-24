/**
 * @file AgentLogIndexTests.cpp
 * @brief S5.3 日志行索引：按行号搜索、上下文、条数/字符上限、范围读取、缓存失效、
 *        超长行与 CRLF。
 *
 * 大文件耗时不给断言（不同盘差得太多），用环境变量开关只打印：`WTFML_AGENT_LOG_SCALE_MB=<MB>`。
 */

#include "Support.hpp"

#include "agent/AgentLogIndex.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

namespace {

bool writeLines(const QString &path, const QStringList &lines, bool trailingNewline = true) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    for (int i = 0; i < lines.size(); ++i) {
        file.write(lines.at(i).toUtf8());
        if (trailingNewline || i + 1 < lines.size()) file.write("\n");
    }
    return true;
}

QStringList numberedLines(int count, const QString &prefix) {
    QStringList lines;
    for (int i = 1; i <= count; ++i) {
        lines.append(QStringLiteral("%1 line %2").arg(prefix).arg(i));
    }
    return lines;
}

int matchLineAt(const AgentLogIndex::SearchResult &result, int index) {
    return index >= 0 && index < result.matches.size() ? result.matches.at(index).line : -1;
}

}  // namespace

void runAgentLogIndexTests() {
    QTemporaryDir dir;
    check(dir.isValid(), "日志索引：临时目录可用");
    if (!dir.isValid()) return;
    AgentLogIndex::clearCache();

    // ---------------------------------------------------------------- 搜索
    const QString path = dir.filePath(QStringLiteral("latest.log"));
    QStringList lines;
    for (int i = 1; i <= 40; ++i) {
        if (i == 10) {
            lines.append(QStringLiteral("[12:00:00] [main/ERROR]: java.lang.OutOfMemoryError: Java heap space"));
        } else if (i == 25) {
            lines.append(QStringLiteral("[12:00:01] [main/ERROR]: caused by java.lang.outofmemoryerror"));
        } else {
            lines.append(QStringLiteral("[12:00:00] [main/INFO]: tick %1").arg(i));
        }
    }
    check(writeLines(path, lines), "日志索引：写出测试日志");

    AgentLogIndex::SearchOptions options;
    options.query = QStringLiteral("OutOfMemoryError");
    AgentLogIndex::SearchResult result;
    QString problem;
    check(AgentLogIndex::search(path, options, &result, &problem), "日志索引：搜索成功", problem);
    check(result.totalMatches == 2, "日志索引：命中 2 处（大小写不敏感）",
          QString::number(result.totalMatches));
    check(matchLineAt(result, 0) == 10 && matchLineAt(result, 1) == 25,
          "日志索引：命中行号是 10 和 25",
          QStringLiteral("%1/%2").arg(matchLineAt(result, 0)).arg(matchLineAt(result, 1)));
    check(result.scannedLines == 40, "日志索引：扫了 40 行",
          QString::number(result.scannedLines));
    check(!result.truncated, "日志索引：结果没被截断");
    check(!result.budgetExceeded, "日志索引：没触发时间预算");

    // 上下文：前 2 行 + 后 1 行（不含命中行自身）。
    AgentLogIndex::SearchOptions contextOptions;
    contextOptions.query = QStringLiteral("OutOfMemoryError");
    contextOptions.before = 2;
    contextOptions.after = 1;
    AgentLogIndex::SearchResult contextResult;
    check(AgentLogIndex::search(path, contextOptions, &contextResult, &problem),
          "日志索引：带上下文的搜索成功", problem);
    check(contextResult.matches.size() == 2, "日志索引：带上下文仍是 2 处");
    if (contextResult.matches.size() == 2) {
        const QList<AgentLogIndex::Line> context = contextResult.matches.constFirst().context;
        check(context.size() == 3, "日志索引：上下文是前 2 后 1 共 3 行",
              QString::number(context.size()));
        if (context.size() == 3) {
            check(context.at(0).number == 8 && context.at(1).number == 9 &&
                      context.at(2).number == 11,
                  "日志索引：上下文行号是 8、9、11",
                  QStringLiteral("%1/%2/%3").arg(context.at(0).number).arg(context.at(1).number).arg(context.at(2).number));
            check(context.at(2).text.contains(QStringLiteral("tick 11")),
                  "日志索引：上下文文本对应第 11 行");
        }
    }

    // 条数上限：只回前 N 条，但 totalMatches 仍是全部。
    AgentLogIndex::SearchOptions limited;
    limited.query = QStringLiteral("tick");
    limited.maxMatches = 5;
    AgentLogIndex::SearchResult limitedResult;
    check(AgentLogIndex::search(path, limited, &limitedResult, &problem),
          "日志索引：带条数上限的搜索成功", problem);
    check(limitedResult.totalMatches == 38, "日志索引：条数上限不影响总数",
          QString::number(limitedResult.totalMatches));
    check(limitedResult.matches.size() == 5, "日志索引：只回 5 条",
          QString::number(limitedResult.matches.size()));
    check(limitedResult.truncated, "日志索引：截断了要标出来");

    // 空搜索词、超长搜索词都拒绝。
    AgentLogIndex::SearchOptions empty;
    AgentLogIndex::SearchResult emptyResult;
    check(!AgentLogIndex::search(path, empty, &emptyResult, &problem) && !problem.isEmpty(),
          "日志索引：空搜索词被拒");
    AgentLogIndex::SearchOptions tooLong;
    tooLong.query = QString(500, QLatin1Char('x'));
    check(!AgentLogIndex::search(path, tooLong, &emptyResult, &problem) && !problem.isEmpty(),
          "日志索引：超长搜索词被拒");

    // 文件不存在时给原因，不当成“没有命中”。
    check(!AgentLogIndex::search(dir.filePath(QStringLiteral("nope.log")), limited, &emptyResult,
                                 &problem) &&
              !problem.isEmpty(),
          "日志索引：文件不存在时报错");

    // ---------------------------------------------------------------- 范围读取
    AgentLogIndex::LineRange range;
    check(AgentLogIndex::readRange(path, 9, 3, AgentLogIndex::maxCharsLimit(), &range, &problem),
          "日志索引：读第 9–11 行成功", problem);
    check(range.totalLines == 40, "日志索引：总行数 40", QString::number(range.totalLines));
    check(range.lines.size() == 3, "日志索引：读到 3 行",
          QString::number(range.lines.size()));
    if (range.lines.size() == 3) {
        check(range.firstLine == 9 && range.lastLine == 11, "日志索引：行号从 1 开始",
              QStringLiteral("%1–%2").arg(range.firstLine).arg(range.lastLine));
        check(range.lines.at(0).text.contains(QStringLiteral("tick 9")),
              "日志索引：第 9 行内容对得上");
        check(range.lines.at(1).text.contains(QStringLiteral("OutOfMemoryError")),
              "日志索引：第 10 行是命中的那行");
    }
    check(!range.truncated, "日志索引：完整读到末尾不算截断");

    AgentLogIndex::LineRange tail;
    check(AgentLogIndex::readRange(path, 38, 10, AgentLogIndex::maxCharsLimit(), &tail, &problem),
          "日志索引：从第 38 行读 10 行成功", problem);
    check(tail.lines.size() == 3, "日志索引：末尾只剩 3 行",
          QString::number(tail.lines.size()));
    check(tail.truncated, "日志索引：请求越界标截断");

    AgentLogIndex::LineRange beyond;
    check(AgentLogIndex::readRange(path, 1000, 10, AgentLogIndex::maxCharsLimit(), &beyond,
                                   &problem),
          "日志索引：起点越界不算读失败", problem);
    check(beyond.lines.isEmpty() && beyond.truncated, "日志索引：起点越界回空表并标截断");

    check(!AgentLogIndex::readRange(path, 0, 10, AgentLogIndex::maxCharsLimit(), &beyond, &problem) &&
              !problem.isEmpty(),
          "日志索引：startLine=0 被拒");

    // 字符预算：每行 200 字符、要 100 行，预算只够一部分。
    const QString longPath = dir.filePath(QStringLiteral("wide.log"));
    QStringList wide;
    for (int i = 0; i < 100; ++i) wide.append(QString(200, QLatin1Char('a')));
    check(writeLines(longPath, wide), "日志索引：写出宽行日志");
    AgentLogIndex::LineRange capped;
    check(AgentLogIndex::readRange(longPath, 1, 100, 1000, &capped, &problem),
          "日志索引：字符预算读取成功", problem);
    check(capped.lines.size() < 100 && capped.truncated,
          "日志索引：字符预算截断要标出来",
          QString::number(capped.lines.size()));

    // CRLF 与没有结尾换行。
    const QString crlfPath = dir.filePath(QStringLiteral("crlf.log"));
    {
        QFile file(crlfPath);
        check(file.open(QIODevice::WriteOnly), "日志索引：写 CRLF 日志");
        file.write("[a] first\r\n[b] second\r\n[c] third");
        file.close();
    }
    AgentLogIndex::LineRange crlf;
    check(AgentLogIndex::readRange(crlfPath, 1, 3, AgentLogIndex::maxCharsLimit(), &crlf, &problem),
          "日志索引：CRLF 日志读得动", problem);
    check(crlf.totalLines == 3, "日志索引：CRLF 下总行数是 3",
          QString::number(crlf.totalLines));
    check(crlf.lines.size() == 3, "日志索引：没有结尾换行的最后一行也读得到",
          QString::number(crlf.lines.size()));
    if (crlf.lines.size() == 3) {
        check(!crlf.lines.at(0).text.endsWith(QLatin1Char('\r')),
              "日志索引：读出来的行不带 CR");
        check(crlf.lines.at(2).text == QStringLiteral("[c] third"),
              "日志索引：最后一行内容", crlf.lines.at(2).text);
    }
    AgentLogIndex::SearchOptions crlfSearch;
    crlfSearch.query = QStringLiteral("third");
    AgentLogIndex::SearchResult crlfResult;
    check(AgentLogIndex::search(crlfPath, crlfSearch, &crlfResult, &problem) &&
              crlfResult.totalMatches == 1 && crlfResult.matches.constFirst().line == 3,
          "日志索引：搜索也认最后一行");

    // ---------------------------------------------------------------- 缓存失效
    AgentLogIndex::LineRange before;
    check(AgentLogIndex::readRange(path, 1, 1, AgentLogIndex::maxCharsLimit(), &before, &problem),
          "日志索引：先读一次建缓存", problem);
    check(before.totalLines == 40, "日志索引：缓存里的总行数",
          QString::number(before.totalLines));
    QStringList appended = lines;
    appended.append(QStringLiteral("[12:00:02] [main/INFO]: appended"));
    check(writeLines(path, appended), "日志索引：追加一行");
    AgentLogIndex::LineRange after;
    check(AgentLogIndex::readRange(path, 1, 1, AgentLogIndex::maxCharsLimit(), &after, &problem),
          "日志索引：再读一次", problem);
    check(after.totalLines == 41, "日志索引：文件变了缓存要失效",
          QString::number(after.totalLines));

    // ---------------------------------------------------------------- 超长单行
    const QString hugePath = dir.filePath(QStringLiteral("huge-line.log"));
    {
        QFile file(hugePath);
        check(file.open(QIODevice::WriteOnly), "日志索引：写超长单行");
        file.write(QByteArray(600 * 1024, 'x'));
        file.write("\n[a] after huge line\n");
        file.close();
    }
    AgentLogIndex::SearchOptions hugeSearch;
    hugeSearch.query = QStringLiteral("after huge line");
    AgentLogIndex::SearchResult hugeResult;
    check(AgentLogIndex::search(hugePath, hugeSearch, &hugeResult, &problem),
          "日志索引：超长单行不会卡死", problem);
    check(hugeResult.matches.size() == 1 && hugeResult.matches.constFirst().line == 2,
          "日志索引：超长单行后面的行照常编号",
          QString::number(matchLineAt(hugeResult, 0)));
    check(hugeResult.matches.constFirst().text.size() <= 4000,
          "日志索引：单行文本有截断上限",
          QString::number(hugeResult.matches.constFirst().text.size()));

    // ---------------------------------------------------------------- 大文件（只打印）
    const int scaleMb = qEnvironmentVariableIntValue("WTFML_AGENT_LOG_SCALE_MB");
    if (scaleMb > 0) {
        const QString bigPath = dir.filePath(QStringLiteral("big.log"));
        const QString needle = QStringLiteral("WTFML_SCALE_NEEDLE");
        QFile big(bigPath);
        if (big.open(QIODevice::WriteOnly)) {
            const QByteArray line =
                QByteArrayLiteral("[12:00:00] [main/INFO]: vanilla tick for a perfectly normal line\n");
            const qint64 target = qint64(scaleMb) * 1024 * 1024;
            qint64 written = 0;
            int counter = 0;
            while (written < target) {
                QByteArray chunk;
                chunk.reserve(1024 * 1024);
                while (chunk.size() < 1024 * 1024) {
                    if (counter % 20000 == 19999) {
                        chunk += "[12:00:00] [main/ERROR]: ";
                        chunk += needle.toUtf8();
                        chunk += "\n";
                    } else {
                        chunk += line;
                    }
                    ++counter;
                }
                written += big.write(chunk);
            }
            big.close();
            AgentLogIndex::clearCache();
            QElapsedTimer timer;
            timer.start();
            AgentLogIndex::LineRange bigRange;
            const bool ranged =
                AgentLogIndex::readRange(bigPath, 1, 200, AgentLogIndex::maxCharsLimit(), &bigRange,
                                         &problem);
            const qint64 rangeMs = timer.restart();
            AgentLogIndex::SearchOptions bigSearch;
            bigSearch.query = needle;
            bigSearch.maxMatches = 20;
            AgentLogIndex::SearchResult bigResult;
            const bool searched = AgentLogIndex::search(bigPath, bigSearch, &bigResult, &problem);
            const qint64 searchMs = timer.elapsed();
            out() << "  INFO  " << QStringLiteral("日志规模测量：%1 MB，%2 行；首次范围读取 %3 ms，"
                                                "全文搜索 %4 ms，命中 %5 处")
                                         .arg(scaleMb)
                                         .arg(bigRange.totalLines)
                                         .arg(rangeMs)
                                         .arg(searchMs)
                                         .arg(bigResult.totalMatches)
                  << "\n";
            check(ranged && searched, "日志索引：大文件读取与搜索都成功", problem);
            check(bigResult.totalMatches > 0 && bigResult.matches.size() <= 20,
                  "日志索引：大文件搜索条数受限",
                  QString::number(bigResult.matches.size()));
            QFile::remove(bigPath);
        }
    }

    AgentLogIndex::clearCache();
}

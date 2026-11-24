/**
 * @file SentenceJoin.hpp
 * @brief 把几个完整句子接成一段。
 *
 * 面向：把"一句话 + 另一句话"直接 `+=` 起来的地方。中文标点在句尾已经收口，两句之间不加
 * 东西；英文译文需要的那一个空格放在译文里（`SentenceJoin` 上下文的 `%1%2`），
 * 不要再往源文里塞空格、也不要靠每条译文的首尾空格对位。
 */

#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

/// 把几个完整句子接成一段。中文直接相接；英文译文在两句之间加空格。
inline QString joinSentences(const QStringList &sentences) {
    QString text;
    for (const QString &sentence : sentences) {
        if (sentence.isEmpty()) continue;
        if (text.isEmpty()) {
            text = sentence;
            continue;
        }
        // `arg` 用双参重载：一次填两个占位符，前面接进去的内容里带 `%1` 也不会被再填一遍。
        text = QCoreApplication::translate("SentenceJoin", "%1%2",
                                           "two sentences; English puts a space between them")
                   .arg(text, sentence);
    }
    return text;
}

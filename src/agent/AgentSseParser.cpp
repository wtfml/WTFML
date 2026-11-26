/**
 * @file AgentSseParser.cpp
 * @brief SSE 行/事件解析（逐字节缓冲，见 .hpp 的边界）。
 */

#include "AgentSseParser.hpp"

#include <QCoreApplication>
#include <QString>

namespace {
void appendData(QByteArray *pending, bool *hasPending, const QByteArray &data) {
    if (*hasPending) {
        pending->append('\n');
    }
    pending->append(data);
    *hasPending = true;
}
}  // namespace

int AgentSseParser::maxLineBytes() { return 1024 * 1024; }

void AgentSseParser::reset() {
    m_buffer.clear();
    m_pendingData.clear();
    m_hasPendingData = false;
    m_done = false;
    m_failed = false;
}

QList<AgentSseParser::Event> AgentSseParser::feed(const QByteArray &chunk) {
    QList<Event> events;
    if (m_done || m_failed) {
        return events;
    }
    m_buffer.append(chunk);

    int consumed = 0;
    while (true) {
        const qsizetype lf = m_buffer.indexOf('\n', consumed);
        if (lf < 0) {
            break;
        }
        qsizetype lineEnd = lf;
        if (lineEnd > consumed && m_buffer.at(lineEnd - 1) == '\r') {
            --lineEnd;
        }
        const QByteArray line = m_buffer.mid(consumed, lineEnd - consumed);
        consumed = lf + 1;

        if (line.size() > maxLineBytes()) {
            m_failed = true;
            Event event;
            event.kind = Event::Kind::Error;
            event.error = QCoreApplication::translate("AgentSseParser", "SSE 单行超过 1 MB，已中止。");
            events.append(event);
            break;
        }

        if (line.isEmpty()) {
            if (m_hasPendingData) {
                Event event;
                if (m_pendingData == QByteArrayLiteral("[DONE]")) {
                    event.kind = Event::Kind::Done;
                    m_done = true;
                } else {
                    event.kind = Event::Kind::Data;
                    event.data = m_pendingData;
                }
                events.append(event);
                m_pendingData.clear();
                m_hasPendingData = false;
            }
            continue;
        }
        if (line.startsWith(':')) {
            continue;  // 注释 / keep-alive
        }
        const qsizetype colon = line.indexOf(':');
        const QByteArray field = colon < 0 ? line : line.left(colon);
        QByteArray value = colon < 0 ? QByteArray() : line.mid(colon + 1);
        if (value.startsWith(' ')) {
            value.remove(0, 1);
        }
        if (field == QByteArrayLiteral("data")) {
            appendData(&m_pendingData, &m_hasPendingData, value);
        } else if (field == QByteArrayLiteral("event") && value == QByteArrayLiteral("error")) {
            // 有些兼容层会发 event: error + data；data 仍会派发，由上层按 JSON 判错。
            continue;
        }
        // id/retry 等字段当前用不到，忽略。
    }

    if (consumed > 0) {
        m_buffer.remove(0, consumed);
    }
    if (m_buffer.size() > maxLineBytes()) {
        m_failed = true;
        Event event;
        event.kind = Event::Kind::Error;
        event.error = QCoreApplication::translate("AgentSseParser", "SSE 行缓冲超过 1 MB，已中止。");
        events.append(event);
        m_buffer.clear();
    }
    return events;
}

QList<AgentSseParser::Event> AgentSseParser::finish() {
    QList<Event> events;
    if (m_done || m_failed) {
        return events;
    }
    if (!m_buffer.isEmpty()) {
        // 没有换行收尾的最后一行：按同一套规则认一次。
        QByteArray line = m_buffer;
        if (line.endsWith('\r')) line.chop(1);
        if (line.startsWith("data:")) {
            QByteArray value = line.mid(5);
            if (value.startsWith(' ')) value.remove(0, 1);
            appendData(&m_pendingData, &m_hasPendingData, value);
        }
        m_buffer.clear();
    }
    if (m_hasPendingData) {
        Event event;
        if (m_pendingData == QByteArrayLiteral("[DONE]")) {
            event.kind = Event::Kind::Done;
            m_done = true;
        } else {
            event.kind = Event::Kind::Data;
            event.data = m_pendingData;
        }
        events.append(event);
        m_pendingData.clear();
        m_hasPendingData = false;
    }
    return events;
}

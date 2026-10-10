#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>

namespace nexa {

struct SseEvent {
    QString type;               // "event:" field, "message" when absent
    QByteArray data;            // the "data:" lines joined with '\n' (UTF-8)
    QString id;                 // last event id seen on the stream

    bool operator==(const SseEvent &) const = default;
};

// Incremental text/event-stream decoder (WHATWG HTML, server-sent events). Pure and safe against
// arbitrary chunk boundaries, including a CR/LF pair or a UTF-8 BOM split across chunks.
class SseParser {
public:
    QList<SseEvent> feed(QByteArrayView chunk);
    // End of stream: flushes a trailing line and an event not terminated by a blank line.
    QList<SseEvent> finish();
    void reset();

private:
    void processLine(QByteArrayView line, QList<SseEvent> &out);
    void dispatch(QList<SseEvent> &out);

    QByteArray buffer_;
    QByteArray data_;
    QString type_;
    QString lastId_;
    bool hasData_{false};
    bool atStreamStart_{true};
};

} // namespace nexa

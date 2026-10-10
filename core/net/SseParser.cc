#include "core/net/SseParser.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr QByteArrayView kBom{"\xEF\xBB\xBF"};

} // namespace

QList<SseEvent> SseParser::feed(QByteArrayView chunk) {
    QList<SseEvent> events;
    buffer_.append(chunk);

    if (atStreamStart_) {
        if (buffer_.size() < kBom.size() && kBom.startsWith(buffer_)) return events;  // BOM may be split
        if (buffer_.startsWith(kBom)) buffer_.remove(0, kBom.size());
        atStreamStart_ = false;
    }

    const char *bytes = buffer_.constData();
    const qsizetype size = buffer_.size();
    qsizetype lineStart = 0;
    for (qsizetype i = 0; i < size; ++i) {
        const char c = bytes[i];
        if (c != '\n' && c != '\r') continue;
        // A CR at the very end may be the first half of CRLF: wait for the next chunk.
        if (c == '\r' && i + 1 == size) break;
        processLine(QByteArrayView(bytes + lineStart, i - lineStart), events);
        if (c == '\r' && bytes[i + 1] == '\n') ++i;
        lineStart = i + 1;
    }
    buffer_.remove(0, lineStart);
    return events;
}

QList<SseEvent> SseParser::finish() {
    QList<SseEvent> events;
    if (!buffer_.isEmpty()) {
        QByteArrayView line(buffer_);
        if (line.endsWith('\r')) line.chop(1);
        processLine(line, events);
        buffer_.clear();
    }
    dispatch(events);
    return events;
}

void SseParser::reset() {
    buffer_.clear();
    data_.clear();
    type_.clear();
    lastId_.clear();
    hasData_ = false;
    atStreamStart_ = true;
}

void SseParser::processLine(QByteArrayView line, QList<SseEvent> &out) {
    if (line.isEmpty()) {
        dispatch(out);
        return;
    }
    if (line.front() == ':') return;  // comment / keep-alive

    const qsizetype colon = line.indexOf(':');
    const QByteArrayView field = colon < 0 ? line : line.first(colon);
    QByteArrayView value = colon < 0 ? QByteArrayView() : line.sliced(colon + 1);
    if (value.startsWith(' ')) value = value.sliced(1);

    if (field == QByteArrayView("data")) {
        data_.append(value);
        data_.append('\n');
        hasData_ = true;
    } else if (field == QByteArrayView("event")) {
        type_ = QString::fromUtf8(value);
    } else if (field == QByteArrayView("id")) {
        if (!value.contains('\0')) lastId_ = QString::fromUtf8(value);
    }
    // "retry" and unknown fields are ignored.
}

void SseParser::dispatch(QList<SseEvent> &out) {
    if (!hasData_) {
        type_.clear();
        return;
    }
    if (data_.endsWith('\n')) data_.chop(1);
    out.append(SseEvent{type_.isEmpty() ? u"message"_s : type_, data_, lastId_});
    data_.clear();
    type_.clear();
    hasData_ = false;
}

} // namespace nexa

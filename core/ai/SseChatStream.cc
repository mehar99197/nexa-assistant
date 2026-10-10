#include "core/ai/SseChatStream.h"

#include "core/net/HttpClient.h"

#include <QNetworkReply>

using namespace Qt::StringLiterals;

namespace nexa {

SseChatStream::SseChatStream(QNetworkReply *reply, EventDecoder decodeEvent, BodyDecoder decodeBody, QObject *parent)
    : ChatStream(parent), reply_(reply), decodeEvent_(std::move(decodeEvent)), decodeBody_(std::move(decodeBody)) {
    reply_->setParent(this);
    connect(reply_, &QNetworkReply::readyRead, this, &SseChatStream::onReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &SseChatStream::onFinished);
}

void SseChatStream::abortTransport() {
    reply_->abort();
}

bool SseChatStream::isBuffered() const {
    return HttpClient::httpStatus(reply_) >= 400
        || reply_->header(QNetworkRequest::ContentTypeHeader).toString().contains(u"application/json"_s);
}

void SseChatStream::onReadyRead() {
    const QByteArray chunk = reply_->readAll();
    if (isDone()) return;
    if (isBuffered()) {
        buffered_ += chunk;
        return;
    }
    handleEvents(parser_.feed(chunk));
}

void SseChatStream::onFinished() {
    if (isDone()) return;
    const QByteArray rest = reply_->readAll();
    if (reply_->error() != QNetworkReply::NoError || HttpClient::httpStatus(reply_) >= 400) {
        buffered_ += rest;
        fail(HttpClient::errorFor(reply_, buffered_));
        return;
    }
    if (isBuffered()) {
        buffered_ += rest;
        StreamDelta whole = decodeBody_(buffered_);
        whole.done = true;
        apply(whole);
        return;
    }
    QList<SseEvent> events = parser_.feed(rest);
    events += parser_.finish();
    handleEvents(events);
    if (isDone()) return;
    // The server closed the stream without saying the reply was complete — or, like Gemini, never
    // says so at all.
    if (finishReason_ || !text().isEmpty() || !accumulator_.isEmpty()) {
        finish();
    } else {
        fail(Error{ErrorCode::Parse, u"The server closed the stream without a reply"_s});
    }
}

void SseChatStream::handleEvents(const QList<SseEvent> &events) {
    for (const SseEvent &event : events) {
        if (isDone()) return;
        apply(decodeEvent_(event));
    }
}

void SseChatStream::apply(const StreamDelta &delta) {
    if (delta.error) {
        fail(*delta.error);
        return;
    }
    emitText(delta.text);
    for (const ToolCallFragment &fragment : delta.toolFragments) {
        const int slot = accumulator_.add(fragment);
        emitToolCallDelta(slot, fragment.name, fragment.argumentsDelta);
    }
    if (delta.finishReason) finishReason_ = delta.finishReason;
    if (delta.done) finish();
}

void SseChatStream::finish() {
    // A reply that made calls asked for tools, whatever its stop reason said: Gemini says STOP.
    FinishReason reason = finishReason_.value_or(FinishReason::Stop);
    if (!accumulator_.isEmpty() && reason == FinishReason::Stop) reason = FinishReason::ToolCalls;
    complete(reason, accumulator_.calls());
}

} // namespace nexa

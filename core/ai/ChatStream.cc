#include "core/ai/ChatStream.h"

namespace nexa {

ChatStream::ChatStream(QObject *parent) : QObject(parent) {}

void ChatStream::cancel() {
    if (done_) return;
    // Mark done first so the transport's own finished/error callbacks are ignored.
    complete(FinishReason::Cancelled, {});
    abortTransport();
}

void ChatStream::emitText(const QString &delta) {
    if (done_ || delta.isEmpty()) return;
    text_ += delta;
    emit textDelta(delta);
}

void ChatStream::emitToolCallDelta(int index, const QString &name, const QString &argumentsDelta) {
    if (done_) return;
    emit toolCallDelta(index, name, argumentsDelta);
}

void ChatStream::complete(FinishReason reason, QList<ToolCall> toolCalls) {
    if (done_) return;
    done_ = true;
    toolCalls_ = std::move(toolCalls);
    emit finished(reason);
}

void ChatStream::fail(const Error &error) {
    if (done_) return;
    done_ = true;
    emit failed(error);
}

} // namespace nexa

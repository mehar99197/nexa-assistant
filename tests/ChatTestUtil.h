#pragma once

#include "core/ai/ChatStream.h"
#include "core/ai/IAiProvider.h"

#include <QTest>

#include <memory>
#include <optional>
#include <tuple>

namespace nexa::test {

// What a streamed reply came to, for provider tests that run against FakeHttpServer.
struct ChatOutcome {
    QString text;
    std::optional<FinishReason> finish;
    std::optional<Error> error;
    QList<ToolCall> toolCalls;
};

inline ChatOutcome runChat(IAiProvider &provider, const ChatRequest &request, int timeoutMs = 5'000) {
    ChatOutcome outcome;
    std::shared_ptr<ChatStream> stream = provider.chat(request);
    bool ended = false;
    QObject::connect(stream.get(), &ChatStream::textDelta, [&](const QString &delta) { outcome.text += delta; });
    QObject::connect(stream.get(), &ChatStream::finished, [&](FinishReason reason) {
        ended = true;
        outcome.finish = reason;
        outcome.toolCalls = stream->toolCalls();
    });
    QObject::connect(stream.get(), &ChatStream::failed, [&](const Error &error) {
        ended = true;
        outcome.error = error;
    });
    // A timeout shows as an outcome with neither finish nor error, which every test checks.
    std::ignore = QTest::qWaitFor([&] { return ended; }, timeoutMs);
    return outcome;
}

// One Server-Sent Event, optionally named.
inline QByteArray sseEvent(const QByteArray &data, const QByteArray &name = {}) {
    return (name.isEmpty() ? QByteArray() : "event: " + name + "\n") + "data: " + data + "\n\n";
}

} // namespace nexa::test

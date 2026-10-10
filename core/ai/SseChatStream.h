#pragma once

#include "core/Result.h"
#include "core/ai/ChatStream.h"
#include "core/ai/ToolCallAccumulator.h"
#include "core/net/SseParser.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

class QNetworkReply;

namespace nexa {

// What one streamed event adds to a reply, once a provider has decoded its own wire format.
struct StreamDelta {
    QString text;
    QList<ToolCallFragment> toolFragments;
    std::optional<FinishReason> finishReason;
    bool done{false};               // the stream says the reply is complete
    std::optional<Error> error;
};

// Template Method over a streamed HTTP reply, shared by every provider: Server-Sent Events are
// decoded as they arrive by the provider's event decoder; error bodies — and servers that ignore the
// stream flag and answer with one JSON document — are buffered and decoded whole at the end. A
// stream that closes without saying it is done still completes, if anything arrived.
class SseChatStream : public ChatStream {
    Q_OBJECT
public:
    using EventDecoder = std::function<StreamDelta(const SseEvent &event)>;
    using BodyDecoder = std::function<StreamDelta(const QByteArray &body)>;

    SseChatStream(QNetworkReply *reply, EventDecoder decodeEvent, BodyDecoder decodeBody, QObject *parent = nullptr);

protected:
    void abortTransport() override;

private:
    bool isBuffered() const;
    void onReadyRead();
    void onFinished();
    void handleEvents(const QList<SseEvent> &events);
    void apply(const StreamDelta &delta);
    void finish();

    QNetworkReply *reply_;
    EventDecoder decodeEvent_;
    BodyDecoder decodeBody_;
    SseParser parser_;
    ToolCallAccumulator accumulator_;
    std::optional<FinishReason> finishReason_;
    QByteArray buffered_;
};

} // namespace nexa

#pragma once

// A model that answers from a script, for tests of what drives one (the agent, the controller, the mission board).
// One scripted reply per chat() call; an empty script leaves the stream hanging (for cancel tests).

#include "core/ai/ChatStream.h"
#include "core/ai/IAiProvider.h"

#include <QStringList>
#include <QTimer>

#include <memory>
#include <optional>

namespace nexa::test {

using namespace Qt::StringLiterals;

class ScriptedStream : public ChatStream {
public:
    void play(const QString &text, const QList<ToolCall> &calls, std::optional<Error> error,
              const QStringList &chunks = {}, const QString &route = {}) {
        QTimer::singleShot(0, this, [this, text, calls, error, chunks, route] {
            if (!route.isEmpty()) setRoute(route, u"One: down"_s);
            for (const QString &chunk : chunks) emitText(chunk);
            emitText(text);
            if (error) {
                fail(*error);
            } else {
                complete(calls.isEmpty() ? FinishReason::Stop : FinishReason::ToolCalls, calls);
            }
        });
    }
    bool aborted{false};

protected:
    void abortTransport() override { aborted = true; }
};

class ScriptedProvider : public IAiProvider {
public:
    struct Reply {
        QString text;
        QList<ToolCall> calls;
        std::optional<Error> error;
        QStringList chunks;     // streamed one by one before `text`
        QString route;          // a fallback answered: who
    };

    QString id() const override { return u"scripted"_s; }
    QString displayName() const override { return u"Scripted"_s; }
    Capabilities capabilities() const override { return {.nativeTools = nativeTools, .vision = vision}; }
    std::optional<bool> takesImages(const QString &) const override { return images; }
    void listModels(std::function<void(Result<QStringList>)> done) override { done(QStringList{u"m"_s}); }

    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override {
        requests.append(request);
        auto stream = std::make_shared<ScriptedStream>();
        lastStream = stream.get();
        if (!script.isEmpty()) {
            const Reply reply = script.takeFirst();
            stream->play(reply.text, reply.calls, reply.error, reply.chunks, reply.route);
        } else if (repeat) {
            stream->play(repeat->text, repeat->calls, repeat->error);
        }
        return stream;
    }

    QList<Reply> script;
    std::optional<Reply> repeat;
    bool nativeTools{true};
    bool vision{true};
    std::optional<bool> images;     // what takesImages() answers
    QList<ChatRequest> requests;
    ScriptedStream *lastStream{nullptr};
};

} // namespace nexa::test

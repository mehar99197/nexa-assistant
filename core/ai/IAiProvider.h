#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"

#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

namespace nexa {

class ChatStream;

// Strategy interface, implemented once per API family (OpenAI-compatible, Anthropic, Gemini).
class IAiProvider : public QObject {
    Q_OBJECT
public:
    struct Capabilities {
        bool streaming{true};
        bool nativeTools{true};
        bool vision{false};
        bool audioStt{false};
        bool audioTts{false};
    };

    using QObject::QObject;
    ~IAiProvider() override = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual Capabilities capabilities() const = 0;
    // `done` is dropped if the provider is destroyed first.
    virtual void listModels(std::function<void(Result<QStringList>)> done) = 0;
    // The stream is released with deleteLater(), so it is safe to drop the last reference from
    // inside one of its own signals.
    virtual std::shared_ptr<ChatStream> chat(const ChatRequest &request) = 0;
};

} // namespace nexa

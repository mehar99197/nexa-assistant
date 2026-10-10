#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

namespace nexa {

// Speech → text (Strategy): local whisper.cpp or a cloud endpoint.
class ISpeechRecognizer : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<QString>)>;

    using QObject::QObject;

    virtual QString name() const = 0;
    // Local recognisers never send audio off the machine; only they may listen continuously.
    virtual bool isLocal() const = 0;

    // Transcribes 16 kHz mono Int16 PCM. `prompt` biases the spelling of names ("Hey Nexa.").
    // `done` runs exactly once on the calling thread; after cancel() it gets ErrorCode::Cancelled.
    virtual void transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) = 0;
    virtual void cancel() = 0;

    // Gets ready for the first request (loads a model); optional.
    virtual void warmUp() {}
};

} // namespace nexa

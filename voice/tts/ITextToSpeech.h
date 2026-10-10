#pragma once

#include "core/Result.h"

#include <QObject>
#include <QString>

namespace nexa {

// Text → speech (Strategy): speech-dispatcher, Piper or a cloud endpoint. speak() queues, so a
// reply can be handed over sentence by sentence while the first one is already playing.
class ITextToSpeech : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual QString name() const = 0;
    virtual void speak(const QString &text) = 0;
    // Silences the speech now and drops the queue; emits no finished().
    virtual void stop() = 0;
    virtual bool isSpeaking() const = 0;
    // Gets ready to speak without delay (starts a local engine); optional.
    virtual void warmUp() {}

signals:
    void started();         // the first sentence of a queue starts playing
    void finished();        // everything queued has been spoken
    void failed(const nexa::Error &error);
};

} // namespace nexa

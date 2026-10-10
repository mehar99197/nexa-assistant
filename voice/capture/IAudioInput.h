#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QObject>

namespace nexa {

// Source of speech for the voice pipeline (Strategy): the microphone, or audio handed over by
// another part of Nexa. Emits 16 kHz mono Int16 PCM in small chunks while active.
class IAudioInput : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual QString name() const = 0;
    virtual Result<void> start() = 0;
    virtual void stop() = 0;
    virtual bool isActive() const = 0;

signals:
    void audio(const QByteArray &pcm16k);
    void failed(const nexa::Error &error);
};

} // namespace nexa

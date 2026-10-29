#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace nexa {

// A word and when it was said, in milliseconds from the start of the audio.
struct TimedWord {
    QString text;
    int startMs{0};
    int endMs{0};
};

// A transcript with the times of its words; `words` is empty when the recogniser cannot tell them.
struct TimedTranscript {
    QString text;
    QList<TimedWord> words;
};

// Speech → text (Strategy): local whisper.cpp or a cloud endpoint.
class ISpeechRecognizer : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<QString>)>;
    using DoneWords = std::function<void(Result<TimedTranscript>)>;

    using QObject::QObject;

    virtual QString name() const = 0;
    // Local recognisers never send audio off the machine; only they may listen continuously.
    virtual bool isLocal() const = 0;

    // Transcribes 16 kHz mono Int16 PCM. `prompt` biases the spelling of names ("Hey Nexa.").
    // `done` runs exactly once on the calling thread; after cancel() it gets ErrorCode::Cancelled.
    virtual void transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) = 0;
    virtual void cancel() = 0;

    // The same with the time of each word (whisper.cpp); by default transcribe() without word times.
    virtual void transcribeWords(const QByteArray &pcm16k, const QString &prompt, DoneWords done) {
        transcribe(pcm16k, prompt, [done = std::move(done)](Result<QString> text) {
            if (!text) {
                done(text.error());
                return;
            }
            done(TimedTranscript{.text = text.value(), .words = {}});
        });
    }

    // Gets ready for the first request (loads a model); optional.
    virtual void warmUp() {}
};

} // namespace nexa

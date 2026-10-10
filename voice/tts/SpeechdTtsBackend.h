#pragma once

#include "voice/tts/ITextToSpeech.h"

#include <QStringList>
#include <QTextToSpeech>

namespace nexa {

// Speech through speech-dispatcher (QTextToSpeech's "speechd" engine) (Strategy): installed on
// every desktop and instant, but robotic — the voice of last resort.
class SpeechdTtsBackend : public ITextToSpeech {
    Q_OBJECT
public:
    SpeechdTtsBackend(const QString &voice, double rate, QObject *parent = nullptr);

    QString name() const override;
    void speak(const QString &text) override;
    void stop() override;
    bool isSpeaking() const override { return speaking_; }

    static QStringList voiceNames();

private:
    QTextToSpeech tts_;
    bool speaking_{false};      // from speak() until the queue has been spoken
    bool announced_{false};     // started() was emitted for this queue
    bool stopping_{false};
};

} // namespace nexa

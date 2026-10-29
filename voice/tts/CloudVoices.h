#pragma once

#include <QString>
#include <QStringList>

namespace nexa {

// What the cloud speech models are (pure): which of a provider's models recognise speech and which speak, and
// the voices a speaking model offers — for Settings → Voice's lists, where typing another name stays possible.
class CloudVoices {
public:
    static QStringList voicesFor(const QString &model);
    static bool isRecognitionModel(const QString &id);     // whisper, *transcri*, audio models
    static bool isSpeechModel(const QString &id);          // tts, speech, orpheus, playai, audio models
};

} // namespace nexa

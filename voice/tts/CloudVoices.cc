#include "voice/tts/CloudVoices.h"

using namespace Qt::StringLiterals;

namespace nexa {

QStringList CloudVoices::voicesFor(const QString &model) {
    const QString name = model.toLower();
    // Groq's Orpheus voices (checked live once the account's terms are accepted).
    if (name.contains(u"orpheus"_s) && name.contains(u"english"_s)) {
        return {u"autumn"_s, u"diana"_s, u"hannah"_s, u"austin"_s, u"daniel"_s, u"troy"_s};
    }
    // OpenAI's voices.
    if (name.startsWith(u"tts-1"_s) || name.contains(u"gpt-4o-mini-tts"_s)) {
        return {u"alloy"_s, u"ash"_s, u"ballad"_s, u"coral"_s, u"echo"_s,
                u"fable"_s, u"nova"_s, u"onyx"_s, u"sage"_s, u"shimmer"_s};
    }
    return {};
}

bool CloudVoices::isRecognitionModel(const QString &id) {
    const QString name = id.toLower();
    return name.contains(u"whisper"_s) || name.contains(u"transcri"_s) || name.contains(u"audio"_s);
}

bool CloudVoices::isSpeechModel(const QString &id) {
    const QString name = id.toLower();
    for (const QString &word : {u"tts"_s, u"speech"_s, u"orpheus"_s, u"playai"_s, u"audio"_s}) {
        if (name.contains(word)) return true;
    }
    return false;
}

} // namespace nexa

#include "voice/stt/Transcript.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

QString cleanTranscript(const QString &text) {
    // Whisper marks non-speech in brackets or asterisks, music with notes.
    static const QRegularExpression annotations(u"\\[[^\\]]*\\]|\\([^)]*\\)|\\*[^*]*\\*|[♪♫]+"_s);
    static const QRegularExpression onlyPunctuation(u"^[\\s\\p{P}\\p{S}]*$"_s);

    QString cleaned = text;
    cleaned.remove(annotations);
    cleaned = cleaned.simplified();
    if (onlyPunctuation.match(cleaned).hasMatch()) return {};
    return cleaned;
}

} // namespace nexa

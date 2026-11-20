#include "voice/stt/Transcript.h"

#include <QRegularExpression>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace nexa {

bool looksMadeUp(const QString &text) {
    // What whisper writes for silence, music or a fan: the closing lines of the videos it learnt from.
    static const QStringList madeUp{u"thank you"_s, u"thanks for watching"_s, u"thank you for watching"_s,
                                    u"thank you very much"_s, u"thank you so much"_s, u"you"_s, u"bye"_s,
                                    u"bye bye"_s, u"please subscribe"_s, u"subtitles by the amaraorg community"_s,
                                    u"subtitles by the amara org community"_s, u"the end"_s};
    static const QRegularExpression notLetters(u"[^\\p{L}\\s]"_s);
    QString words = text.toLower();
    words.remove(notLetters);
    words = words.simplified();
    if (words.isEmpty()) return false;
    if (madeUp.contains(words)) return true;
    // The request prompt (VoicePipeline::requestPrompt) read back: two of its example sentences together.
    return words.contains(u"ammi ko call karo"_s) && (words.contains(u"volume kam karo"_s) || words.contains(u"pehchan"_s));
}

QString cleanTranscript(const QString &text) {
    // Whisper marks non-speech in brackets or asterisks, music with notes.
    static const QRegularExpression annotations(u"\\[[^\\]]*\\]|\\([^)]*\\)|\\*[^*]*\\*|[♪♫]+"_s);
    static const QRegularExpression onlyPunctuation(u"^[\\s\\p{P}\\p{S}]*$"_s);

    QString cleaned = text;
    cleaned.remove(annotations);
    cleaned = cleaned.simplified();
    if (onlyPunctuation.match(cleaned).hasMatch() || looksMadeUp(cleaned)) return {};
    return cleaned;
}

} // namespace nexa

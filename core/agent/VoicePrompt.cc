#include "core/agent/VoicePrompt.h"

using namespace Qt::StringLiterals;

namespace nexa {

QString voicePromptParagraph(const QString &wakeName) {
    QString paragraph =
        u"Messages that start with [voice] were spoken, and your reply to them is read aloud: answer in one to three "
        "short sentences of plain text, without Markdown, lists or code. Their words come from speech recognition and "
        "may be wrong or spelled by how they sound, and the user may mix Urdu and English: read them by sound "
        "(\"page in\" may be the song \"Pehchan\"). When a spoken request is still unclear, ask one short question "
        "instead of guessing; the user can answer without saying your name. Reply in the language the user spoke, "
        "writing Urdu in Roman letters."_s;
    const QString name = wakeName.trimmed();
    if (!name.isEmpty() && name.compare(u"Nexa"_s, Qt::CaseInsensitive) != 0) {
        paragraph += u"\n\nThe user calls you %1; answer to that name."_s.arg(name);
    }
    return paragraph;
}

} // namespace nexa

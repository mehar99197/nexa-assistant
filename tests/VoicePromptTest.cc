// What the model is told about spoken messages: short answers read aloud, words from speech recognition, Urdu
// and English, a question back when unclear — and the name the owner calls it.

#include "core/agent/VoicePrompt.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Spoken messages: what the model is told") {
    const QString spoken =
        u"Messages that start with [voice] were spoken, and your reply to them is read aloud: answer in one to three "
        "short sentences of plain text, without Markdown, lists or code. Their words come from speech recognition and "
        "may be wrong or spelled by how they sound, and the user may mix Urdu and English: read them by sound "
        "(\"page in\" may be the song \"Pehchan\"). When a spoken request is still unclear, ask one short question "
        "instead of guessing; the user can answer without saying your name. Reply in the language the user spoke, "
        "writing Urdu in Roman letters."_s;
    CHECK(voicePromptParagraph(u"Nexa"_s) == spoken);
    CHECK(voicePromptParagraph(u"Jarvis"_s) == spoken + u"\n\nThe user calls you Jarvis; answer to that name."_s);
}

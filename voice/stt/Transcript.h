#pragma once

#include <QString>

namespace nexa {

// Recogniser output → request text: drops non-speech annotations ("[BLANK_AUDIO]", "(music)",
// "*laughs*", "♪") and collapses whitespace; empty when nothing was said. Pure.
// Also empty for what whisper is known to make up from noise or silence ("Thank you.", "Thanks for watching!", a lone
// "you") and for its prompt read back (looksMadeUp).
QString cleanTranscript(const QString &text);
// The whole transcript is one of those: no one said it.
bool looksMadeUp(const QString &text);

} // namespace nexa

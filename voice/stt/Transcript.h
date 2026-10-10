#pragma once

#include <QString>

namespace nexa {

// Recogniser output → request text: drops non-speech annotations ("[BLANK_AUDIO]", "(music)",
// "*laughs*", "♪") and collapses whitespace; empty when nothing was said. Pure.
QString cleanTranscript(const QString &text);

} // namespace nexa

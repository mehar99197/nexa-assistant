#pragma once

#include <QString>

namespace nexa {

// The system prompt's paragraph on spoken messages: answers that read well aloud, words that came from speech
// recognition (and may mix Urdu and English), a question back when a request is unclear — and, when the owner
// chose another wake name, that it answers to it. Pure.
QString voicePromptParagraph(const QString &wakeName);

} // namespace nexa

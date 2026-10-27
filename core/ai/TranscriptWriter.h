#pragma once

#include "core/ai/ChatRequest.h"

#include <QList>
#include <QString>

namespace nexa {

// Builder of the text one backend turn carries, for backends that take a conversation as turns of
// plain text (agy, opencode): Nexa's instructions where there is no system prompt to put them in,
// earlier history for a fresh session, and the new turns themselves (pure).
class TranscriptWriter {
public:
    static QString instructions(const QString &system);
    static QString instructionsUpdate(const QString &system);
    // "" for no history; otherwise every earlier turn, labelled by who said it.
    static QString history(const QList<ChatMessage> &history);
    // The new turns' texts, a blank line between them (images travel separately).
    static QString turnText(const QList<ChatMessage> &turns);
};

} // namespace nexa

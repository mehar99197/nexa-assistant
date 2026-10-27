#include "core/ai/TranscriptWriter.h"

#include <QStringList>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString label(const ChatMessage &message) {
    switch (message.role) {
    case Role::System:
        return u"Instructions"_s;
    case Role::User:
        return u"User"_s;
    case Role::Assistant:
        return u"Nexa"_s;
    case Role::Tool:
        return u"Tool result (%1)"_s.arg(message.toolName);
    }
    return {};
}

} // namespace

QString TranscriptWriter::instructions(const QString &system) {
    return u"[Instructions from Nexa — follow them for the whole conversation]\n\n"_s + system;
}

QString TranscriptWriter::instructionsUpdate(const QString &system) {
    return u"[Nexa's instructions have changed; these replace the earlier ones]\n\n"_s + system;
}

QString TranscriptWriter::history(const QList<ChatMessage> &history) {
    if (history.isEmpty()) return {};
    QStringList parts{u"[Earlier in this conversation]"_s};
    for (const ChatMessage &message : history) parts.append(label(message) + u": "_s + message.text);
    return parts.join(u"\n\n"_s);
}

QString TranscriptWriter::turnText(const QList<ChatMessage> &turns) {
    QStringList parts;
    for (const ChatMessage &turn : turns) {
        if (!turn.text.isEmpty()) parts.append(turn.text);
    }
    return parts.join(u"\n\n"_s);
}

} // namespace nexa

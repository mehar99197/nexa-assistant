#pragma once

#include "core/ai/ChatRequest.h"

#include <QList>
#include <QString>

namespace nexa {

// What to send a backend that keeps the conversation itself (a warm agy process, an opencode
// session): either the new turns alone, or everything again in a fresh session.
struct TurnPlan {
    bool fresh{true};
    QList<ChatMessage> history;         // fresh only: the messages before the new turns
    QList<ChatMessage> turns;           // the new turns: no system, no assistant; images kept
    QString system;                     // the request's system prompt ("" if none)
    bool instructionsChanged{false};    // continue only: the system prompt differs from the session's
};

// Remembers what one backend session holds — the messages as Nexa sent them, then the backend's
// own reply — and plans the next request against it (pure). Messages match by role and text: images
// are not compared (the backend saw them), the note Conversation leaves for dropped images is
// ignored, and the reply matches whatever Nexa kept of it. The request may begin later than the
// session (Conversation trims old turns), as long as what it repeats is the end of what was sent.
class TurnLedger {
public:
    // `request` as the provider receives it: after TextToolParser::asText, the system message first.
    TurnPlan plan(const ChatRequest &request) const;
    // The backend took `request` and answered it.
    void commit(const ChatRequest &request);
    // The session is gone or can no longer be trusted (a failed or cancelled turn).
    void reset();
    bool isEmpty() const { return !active_; }

private:
    bool active_{false};
    QString model_;
    QString system_;
    QList<ChatMessage> sent_;           // without the system message; the reply follows it
};

} // namespace nexa

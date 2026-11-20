#pragma once

#include "core/ai/ChatRequest.h"

#include <QList>
#include <QString>

namespace nexa {

// Chat history plus system prompt. buildRequest() keeps requests small by sending only the newest
// messages that fit the character budget; the latest message is always sent, and the history sent
// always starts at a user message (so tool results never lose the call that produced them).
class Conversation {
public:
    explicit Conversation(QString systemPrompt = {});

    void setSystemPrompt(const QString &prompt) { systemPrompt_ = prompt; }
    const QString &systemPrompt() const { return systemPrompt_; }
    void setHistoryBudget(qsizetype chars) { historyBudget_ = chars; }

    void addUser(const QString &text, const QList<ImageAttachment> &images = {});
    void addAssistant(const QString &text, const QList<ToolCall> &toolCalls = {});
    void addToolResult(const QString &callId, const QString &toolName, const QString &text);
    void clear() { messages_.clear(); }

    const QList<ChatMessage> &messages() const { return messages_; }
    bool isEmpty() const { return messages_.isEmpty(); }

    // Only the newest message with images keeps them (older screenshots become a text note),
    // so a long session does not resend every image.
    ChatRequest buildRequest(const QString &model, const QList<ToolSpec> &tools = {}) const;
    // The text without the note buildRequest() leaves where it dropped an older message's images —
    // the same message, as far as a backend that already saw them is concerned.
    static QString withoutImageNotes(const QString &text);

private:
    qsizetype firstMessageInBudget() const;

    QString systemPrompt_;
    QList<ChatMessage> messages_;
    qsizetype historyBudget_{48'000};
};

} // namespace nexa

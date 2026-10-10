#pragma once

#include "core/tools/ToolSpec.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace nexa {

enum class Role { System, User, Assistant, Tool };

struct ImageAttachment {
    QByteArray data;                                    // encoded bytes (JPEG/PNG)
    QString mimeType{QStringLiteral("image/jpeg")};
};

struct ToolCall {
    QString id;
    QString name;
    QString argumentsJson;                              // JSON object text as produced by the model
};

struct ChatMessage {
    Role role{Role::User};
    QString text;
    QList<ImageAttachment> images;                      // user messages
    QList<ToolCall> toolCalls;                          // assistant messages
    QString toolCallId;                                 // tool-result messages
    QString toolName;                                   // tool-result messages
};

// Provider-neutral request; each IAiProvider maps it to its own wire format.
struct ChatRequest {
    QString model;
    QList<ChatMessage> messages;
    QList<ToolSpec> tools;                              // offered to the model; empty = plain chat
    double temperature{-1.0};                           // < 0: provider default
    int maxTokens{0};                                   // 0: provider default
};

// Builder: assembles a ChatRequest fluently (system prompt, history, attachments, tool results).
class ChatRequestBuilder {
public:
    ChatRequestBuilder &model(const QString &model);
    ChatRequestBuilder &system(const QString &text);    // skipped when blank
    ChatRequestBuilder &user(const QString &text, const QList<ImageAttachment> &images = {});
    ChatRequestBuilder &assistant(const QString &text, const QList<ToolCall> &toolCalls = {});
    ChatRequestBuilder &toolResult(const QString &callId, const QString &toolName, const QString &text);
    ChatRequestBuilder &messages(const QList<ChatMessage> &messages);
    ChatRequestBuilder &tools(const QList<ToolSpec> &tools);
    ChatRequestBuilder &temperature(double value);
    ChatRequestBuilder &maxTokens(int value);
    ChatRequest build() const { return request_; }

private:
    ChatRequest request_;
};

} // namespace nexa

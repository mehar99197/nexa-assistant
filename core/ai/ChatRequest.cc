#include "core/ai/ChatRequest.h"

namespace nexa {

ChatRequestBuilder &ChatRequestBuilder::model(const QString &model) {
    request_.model = model;
    return *this;
}

ChatRequestBuilder &ChatRequestBuilder::system(const QString &text) {
    if (!text.trimmed().isEmpty()) request_.messages.append(ChatMessage{.role = Role::System, .text = text});
    return *this;
}

ChatRequestBuilder &ChatRequestBuilder::user(const QString &text, const QList<ImageAttachment> &images) {
    request_.messages.append(ChatMessage{.role = Role::User, .text = text, .images = images});
    return *this;
}

ChatRequestBuilder &ChatRequestBuilder::assistant(const QString &text, const QList<ToolCall> &toolCalls) {
    request_.messages.append(ChatMessage{.role = Role::Assistant, .text = text, .toolCalls = toolCalls});
    return *this;
}

ChatRequestBuilder &ChatRequestBuilder::toolResult(const QString &callId, const QString &toolName,
                                                   const QString &text) {
    request_.messages.append(
        ChatMessage{.role = Role::Tool, .text = text, .toolCallId = callId, .toolName = toolName});
    return *this;
}

ChatRequestBuilder &ChatRequestBuilder::messages(const QList<ChatMessage> &messages) {
    request_.messages.append(messages);
    return *this;
}

ChatRequestBuilder &ChatRequestBuilder::tools(const QList<ToolSpec> &tools) {
    request_.tools = tools;
    return *this;
}

ChatRequestBuilder &ChatRequestBuilder::temperature(double value) {
    request_.temperature = value;
    return *this;
}

ChatRequestBuilder &ChatRequestBuilder::maxTokens(int value) {
    request_.maxTokens = value;
    return *this;
}

} // namespace nexa

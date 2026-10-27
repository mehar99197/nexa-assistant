#include "core/agent/Conversation.h"

#include <QRegularExpression>

namespace nexa {

namespace {

QString imageNote(qsizetype count) {
    return QStringLiteral("\n[%1 earlier image(s) omitted]").arg(count);
}

// Rough size estimate; images count as a fixed cost because they are sent downscaled.
constexpr qsizetype kImageCost = 4'000;

qsizetype weight(const ChatMessage &message) {
    qsizetype total = message.text.size() + message.images.size() * kImageCost;
    for (const ToolCall &call : message.toolCalls) total += call.name.size() + call.argumentsJson.size();
    return total;
}

} // namespace

Conversation::Conversation(QString systemPrompt) : systemPrompt_(std::move(systemPrompt)) {}

void Conversation::addUser(const QString &text, const QList<ImageAttachment> &images) {
    messages_.append(ChatMessage{.role = Role::User, .text = text, .images = images});
}

void Conversation::addAssistant(const QString &text, const QList<ToolCall> &toolCalls) {
    messages_.append(ChatMessage{.role = Role::Assistant, .text = text, .toolCalls = toolCalls});
}

void Conversation::addToolResult(const QString &callId, const QString &toolName, const QString &text) {
    messages_.append(ChatMessage{.role = Role::Tool, .text = text, .toolCallId = callId, .toolName = toolName});
}

ChatRequest Conversation::buildRequest(const QString &model, const QList<ToolSpec> &tools) const {
    QList<ChatMessage> history = messages_.mid(firstMessageInBudget());
    qsizetype newestWithImages = -1;
    for (qsizetype i = history.size() - 1; i >= 0 && newestWithImages < 0; --i) {
        if (!history.at(i).images.isEmpty()) newestWithImages = i;
    }
    for (qsizetype i = 0; i < history.size(); ++i) {
        ChatMessage &message = history[i];
        if (i == newestWithImages || message.images.isEmpty()) continue;
        message.text += imageNote(message.images.size());
        message.images.clear();
    }
    return ChatRequestBuilder().model(model).system(systemPrompt_).messages(history).tools(tools).build();
}

QString Conversation::withoutImageNotes(const QString &text) {
    static const QRegularExpression note(QStringLiteral("\\n\\[\\d+ earlier image\\(s\\) omitted\\]"));
    QString plain = text;
    return plain.remove(note);
}

qsizetype Conversation::firstMessageInBudget() const {
    qsizetype first = messages_.size();
    qsizetype used = 0;
    for (qsizetype i = messages_.size() - 1; i >= 0; --i) {
        used += weight(messages_.at(i));
        if (used > historyBudget_ && first < messages_.size()) break;
        first = i;
    }
    while (first > 0 && first < messages_.size() && messages_.at(first).role != Role::User) --first;
    return first;
}

} // namespace nexa

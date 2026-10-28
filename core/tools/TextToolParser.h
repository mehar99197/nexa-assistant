#pragma once

#include "core/ai/ChatRequest.h"
#include "core/tools/ToolSpec.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace nexa {

// Tool calls for models that cannot make them natively (pure). The tools are described in the
// system prompt, the model writes a call as text —
//
//     <tool_call>{"name": "open_app", "arguments": {"name": "firefox"}}</tool_call>
//
// — and parse() reads it back out of the reply. The same text is recognised from models that do
// have native tools, which some write anyway when a server forwards them badly.
class TextToolParser {
public:
    struct Parsed {
        QString text;               // the reply with the calls taken out
        QList<ToolCall> calls;      // ids "text_call_1", … — the caller may give its own
    };

    // Recognises <tool_call>…</tool_call> blocks (the convention, and Hermes/Qwen-style models),
    // and a reply that is nothing but a JSON object — bare or in a ```json block — naming a tool.
    // Only names in `known` count: prose that merely looks like JSON is left alone.
    static Parsed parse(const QString &reply, const QStringList &known);

    // The system-prompt addendum that teaches the convention and lists the tools.
    static QString instructions(const QList<ToolSpec> &tools);

    // History for a model without tools: its calls written back out as text, and the results as
    // user messages, since a server without tool support refuses "tool" messages.
    static QList<ChatMessage> asText(const QList<ChatMessage> &messages);
    // A whole request for such a model: the history asText, the convention for `request.tools` added to
    // the system prompt, and no native tools.
    static ChatRequest asTextRequest(const ChatRequest &request);

    // Lets a reply stream to the screen while it arrives without ever showing call markup: text goes
    // out up to the first "<tool_call" (a tail that may be the start of one waits for the next
    // piece), and nothing after it. A reply that opens like a bare call — "{", "[" or a ``` fence —
    // is held whole. Leading whitespace is never shown and trailing whitespace waits for what follows,
    // so a reply that stops before a call leaves no dangling line. parse() still reads the whole reply.
    class StreamFilter {
    public:
        QString feed(const QString &delta);     // what may be shown now
        const QString &shown() const { return shown_; }

    private:
        QString pending_;
        QString shown_;
        bool holding_{false};
    };
};

} // namespace nexa

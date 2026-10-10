#pragma once

#include "core/ai/ChatRequest.h"

#include <QList>
#include <QString>

#include <map>

namespace nexa {

// One streamed piece of a tool call, already extracted from the provider's wire format.
struct ToolCallFragment {
    int index{0};
    QString id;
    QString name;
    QString argumentsDelta;
};

// Merges streamed tool-call fragments into complete calls (pure). Serves every provider: OpenAI
// keys fragments by tool_calls[].index, Anthropic by content-block index, and Gemini's whole
// calls are added with a running index.
class ToolCallAccumulator {
public:
    // Returns the slot the fragment was merged into. It differs from fragment.index only when a
    // server reuses one index for several calls (a new id at an occupied index starts a new call).
    int add(const ToolCallFragment &fragment);
    // Ordered by slot; a missing id becomes "call_<slot>", empty arguments become "{}".
    QList<ToolCall> calls() const;
    bool isEmpty() const { return calls_.empty(); }
    void clear();

private:
    std::map<int, ToolCall> calls_;
    std::map<int, int> slotForIndex_;
};

} // namespace nexa

#include "core/ai/ToolCallAccumulator.h"

using namespace Qt::StringLiterals;

namespace nexa {

int ToolCallAccumulator::add(const ToolCallFragment &fragment) {
    const auto mapped = slotForIndex_.find(fragment.index);
    int slot = mapped != slotForIndex_.end() ? mapped->second : fragment.index;

    auto existing = calls_.find(slot);
    // Some OpenAI-compatible servers send every call with index 0; a different id is a new call.
    if (existing != calls_.end() && !fragment.id.isEmpty() && !existing->second.id.isEmpty()
        && fragment.id != existing->second.id) {
        slot = calls_.rbegin()->first + 1;
        existing = calls_.end();
    }
    slotForIndex_[fragment.index] = slot;

    ToolCall &call = existing != calls_.end() ? existing->second : calls_[slot];
    if (call.id.isEmpty()) call.id = fragment.id;
    if (call.name.isEmpty()) call.name = fragment.name;
    call.argumentsJson += fragment.argumentsDelta;
    return slot;
}

QList<ToolCall> ToolCallAccumulator::calls() const {
    QList<ToolCall> result;
    result.reserve(qsizetype(calls_.size()));
    for (const auto &[slot, call] : calls_) {
        ToolCall complete = call;
        if (complete.id.isEmpty()) complete.id = u"call_%1"_s.arg(slot);
        if (complete.argumentsJson.trimmed().isEmpty()) complete.argumentsJson = u"{}"_s;
        result.append(complete);
    }
    return result;
}

void ToolCallAccumulator::clear() {
    calls_.clear();
    slotForIndex_.clear();
}

} // namespace nexa

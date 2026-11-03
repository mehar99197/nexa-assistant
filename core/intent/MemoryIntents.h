#pragma once

#include "core/intent/IIntentHandler.h"

namespace nexa {

// The fast path for memory, tried right after ControlIntent (so "forget it" stays a stop):
// "remember that my office is in Multan", "remember my sister is Sara" → remember (asked);
// "forget that my office is in Multan", "forget what I said about Sara" → forget;
// "what do you know about me" → search_memory. The fact comes from IntentInput::raw, as said, and
// is a verbatim payload: "I like tea and coffee" is one fact, not a compound request. A question
// ("remember that time we met?") or a memory of an event is chat, left to the model.
class MemoryIntent : public IIntentHandler {
public:
    std::optional<IntentMatch> match(const IntentInput &input) const override;
};

} // namespace nexa

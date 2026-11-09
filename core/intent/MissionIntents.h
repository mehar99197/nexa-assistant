#pragma once

#include "core/intent/IIntentHandler.h"

namespace nexa {

// The mission board by voice or by typing (Chain of Responsibility link, after the stops): "pause task two", "resume the
// code agent", "stop browser 01", "pause all tasks", "approve" / "yes, do it", "deny" / "no, don't", "what are you
// doing?", "add a task: …", "that's all". Whole utterances only, so ordinary talk ("pause the video", "no problem")
// passes through; what a command means when nothing waits (a "yes" with no question) is AppController's call.
class TaskIntent : public IIntentHandler {
public:
    std::optional<IntentMatch> match(const IntentInput &input) const override;
};

} // namespace nexa

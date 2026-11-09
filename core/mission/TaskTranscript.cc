#include "core/mission/TaskTranscript.h"

namespace nexa {

void TaskTranscript::append(TranscriptEvent event) {
    if (event.kind == TranscriptEvent::Kind::Delta && !events_.isEmpty()
        && events_.last().kind == TranscriptEvent::Kind::Delta) {
        events_.last().text += event.text;
        return;
    }
    events_.append(std::move(event));
}

QString TaskTranscript::lastReply() const {
    for (qsizetype i = events_.size() - 1; i >= 0; --i) {
        const TranscriptEvent &event = events_.at(i);
        if (event.kind == TranscriptEvent::Kind::Delta) return event.text.trimmed();
        if (event.kind == TranscriptEvent::Kind::User) break;
    }
    return {};
}

} // namespace nexa

#include "core/ai/TurnLedger.h"

#include "core/agent/Conversation.h"

namespace nexa {

namespace {

bool same(const ChatMessage &a, const ChatMessage &b) {
    return a.role == b.role
        && Conversation::withoutImageNotes(a.text).trimmed() == Conversation::withoutImageNotes(b.text).trimmed();
}

bool endsWith(const QList<ChatMessage> &list, const QList<ChatMessage> &tail) {
    if (tail.size() > list.size()) return false;
    const qsizetype offset = list.size() - tail.size();
    for (qsizetype i = 0; i < tail.size(); ++i) {
        if (!same(list.at(offset + i), tail.at(i))) return false;
    }
    return true;
}

// The messages without a leading system message, which goes to `system`.
QList<ChatMessage> withoutSystem(const ChatRequest &request, QString *system) {
    QList<ChatMessage> messages = request.messages;
    if (!messages.isEmpty() && messages.first().role == Role::System) {
        if (system) *system = messages.first().text;
        messages.removeFirst();
    }
    return messages;
}

} // namespace

TurnPlan TurnLedger::plan(const ChatRequest &request) const {
    TurnPlan plan;
    const QList<ChatMessage> messages = withoutSystem(request, &plan.system);

    // The new turns are everything after the last assistant message, which is the backend's reply.
    qsizetype firstNew = 0;
    for (qsizetype i = messages.size() - 1; i >= 0; --i) {
        if (messages.at(i).role == Role::Assistant) {
            firstNew = i + 1;
            break;
        }
    }
    plan.turns = messages.mid(firstNew);
    plan.history = messages.first(firstNew);

    const bool continues = active_ && request.model == model_ && !plan.turns.isEmpty() && firstNew > 0
        && endsWith(sent_, messages.first(firstNew - 1));
    if (continues) {
        plan.fresh = false;
        plan.history.clear();
        plan.instructionsChanged = plan.system != system_;
    }
    return plan;
}

void TurnLedger::commit(const ChatRequest &request) {
    active_ = true;
    model_ = request.model;
    system_.clear();
    sent_ = withoutSystem(request, &system_);
}

void TurnLedger::reset() {
    active_ = false;
    model_.clear();
    system_.clear();
    sent_.clear();
}

} // namespace nexa

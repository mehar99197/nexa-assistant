#pragma once

#include "core/mission/AgentRole.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace nexa {

enum class ActivityKind { ToolStarted, ToolFinished, Notice, Rerouted, Approval, ApprovalSettled, TaskState, Voice };

// One line of the live activity: "14:37:48 WEB select option "CS-3012"".
struct ActivityEvent {
    QDateTime time;
    ActivityKind kind{ActivityKind::Notice};
    Lane lane{Lane::General};
    std::optional<quint64> task;
    QString agentId;
    QString text;
    bool ok{true};
};

// The live activity of every task and of the voice, newest last, capped (ring buffer, pure).
class ActivityFeed {
public:
    explicit ActivityFeed(int capacity = 500) : capacity_(capacity > 0 ? capacity : 1) {}

    void push(ActivityEvent event);
    // Every event, or one lane's.
    QList<ActivityEvent> events(std::optional<Lane> lane = std::nullopt) const;
    int size() const { return int(events_.size()); }
    int capacity() const { return capacity_; }

private:
    int capacity_;
    QList<ActivityEvent> events_;
};

} // namespace nexa

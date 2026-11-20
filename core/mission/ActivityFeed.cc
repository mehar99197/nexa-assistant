#include "core/mission/ActivityFeed.h"

namespace nexa {

void ActivityFeed::push(ActivityEvent event) {
    events_.append(std::move(event));
    if (events_.size() > capacity_) events_.remove(0, events_.size() - capacity_);
}

QList<ActivityEvent> ActivityFeed::events(std::optional<Lane> lane) const {
    if (!lane) return events_;
    QList<ActivityEvent> mine;
    for (const ActivityEvent &event : events_) {
        if (event.lane == *lane) mine.append(event);
    }
    return mine;
}

} // namespace nexa

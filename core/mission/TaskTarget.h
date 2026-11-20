#pragma once

#include "core/mission/Task.h"

#include <QList>
#include <QString>

#include <optional>

namespace nexa {

// Which task a spoken or typed name means (pure): a number ("2", "two" — the agent's number), a kind ("browser",
// "code": the newest of that kind not finished), an agent ("browser 01", "BROWSER-01"); nothing named: the one in view
// while it is not finished, else the newest not finished. Sub-tasks are reached by their number only.
class TaskTarget {
public:
    static std::optional<TaskId> resolve(const QString &target, const QList<Task> &tasks, std::optional<TaskId> inView);
};

} // namespace nexa

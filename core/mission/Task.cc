#include "core/mission/Task.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

QString taskStateName(TaskState state) {
    switch (state) {
    case TaskState::Queued: return u"QUEUED"_s;
    case TaskState::Running: return u"RUNNING"_s;
    case TaskState::AwaitingYou: return u"AWAITING YOU"_s;
    case TaskState::Delegating: return u"DELEGATING"_s;
    case TaskState::Paused: return u"PAUSED"_s;
    case TaskState::Done: return u"DONE"_s;
    case TaskState::Failed: return u"FAILED"_s;
    case TaskState::Stopped: return u"STOPPED"_s;
    }
    return u"QUEUED"_s;
}

bool holdsSlot(TaskState state) {
    return state == TaskState::Running || state == TaskState::AwaitingYou;
}

bool isFinished(TaskState state) {
    return state == TaskState::Done || state == TaskState::Failed || state == TaskState::Stopped;
}

QString titleFor(const QString &text) {
    constexpr qsizetype kMax = 80;
    QString line;
    for (const QString &candidate : text.split(u'\n')) {
        line = candidate.simplified();
        if (!line.isEmpty()) break;
    }
    if (line.size() > kMax) line = line.left(kMax - 1) + u"…"_s;
    return line;
}

int estimateProgress(int steps, int toolCalls, TaskState state) {
    if (state == TaskState::Done) return 100;
    if (state == TaskState::Queued) return 0;
    // Quick at first, slower as the work drags on: 100 − 90·0.85^work, so it never reaches 100 while it runs.
    const double work = std::max(0, steps) + 0.5 * std::max(0, toolCalls);
    double progress = 100.0 - 90.0 * std::pow(0.85, work);
    return std::clamp(int(progress), 5, 97);
}

} // namespace nexa

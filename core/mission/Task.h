#pragma once

#include "core/config/Config.h"
#include "core/mission/AgentRole.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace nexa {

using TaskId = quint64;

// Where a task is in its life. A task holds one of the board's slots while it runs or waits for the user's answer;
// paused, delegating (its sub-task runs instead) and queued ones hold none.
enum class TaskState { Queued, Running, AwaitingYou, Delegating, Paused, Done, Failed, Stopped };

QString taskStateName(TaskState state);     // "RUNNING", "AWAITING YOU", …
bool holdsSlot(TaskState state);
bool isFinished(TaskState state);           // done, failed or stopped

// One task on the mission board (value). Its conversation and its agent live in the TaskRunner.
struct Task {
    TaskId id{0};
    QString title;                  // titleFor(text)
    QString text;                   // what was asked
    QString source;                 // gui | voice | phone | phone-voice
    AgentRole role{AgentRole::General};
    QString agentId;                // "BROWSER-01"
    ModelChoice model;              // what it runs on (Auto: the first choice and its chain)
    QString providerName;           // in words, for its card: "opencode · gpt-x" or "Auto"
    TaskState state{TaskState::Queued};
    QString subStatus;              // its last step in a few words: "Running run_command"
    int progress{0};                // 0 … 100, an estimate
    QDateTime created;
    QDateTime started;
    QDateTime ended;
    int steps{0};
    int toolCalls{0};
    bool fastPath{false};
    std::optional<TaskId> parent;   // a sub-task's (delegate_task)
    int depth{0};
    QList<int> activity;            // tool calls per second over the last 24 s, oldest first: its sparkline
    QString lastError;
};

// What the model is given of a request: a spoken one (at the PC or into the phone) is marked [voice], so the reply is
// short plain sentences that read well aloud.
QString markedForModel(const QString &text, const QString &source);
// A task's title: its first line, simplified, shortened to 80 characters.
QString titleFor(const QString &text);
// An estimate of how far a task is, from what it has done; never 100 before it is done.
int estimateProgress(int steps, int toolCalls, TaskState state);

} // namespace nexa

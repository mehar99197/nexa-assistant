#pragma once

#include "core/Result.h"
#include "core/ai/ChatStream.h"
#include "core/config/Config.h"
#include "core/intent/IIntentHandler.h"
#include "core/mission/ActivityFeed.h"
#include "core/mission/ModelFit.h"
#include "core/mission/Task.h"
#include "core/mission/TaskRunner.h"
#include "core/mission/TaskTranscript.h"
#include "core/tools/ToolExecutor.h"

#include <QDate>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace nexa {

class IAiProvider;
class ToolRegistry;

// What the app gives every task (Strategy; AppController implements it).
class ITaskEnvironment {
public:
    virtual ~ITaskEnvironment() = default;
    // A provider instance of the task's own (a program answers one conversation at a time per instance). Auto: the
    // chain of the first choice and the providers that may answer when it cannot.
    virtual Result<std::shared_ptr<IAiProvider>> createProvider(const ModelChoice &choice) = 0;
    // The model to ask for: Auto → the toolbar's; a provider's → choice.model, or the provider's own default.
    virtual QString modelFor(const ModelChoice &choice) = 0;
    // For the cards: "opencode · gpt-x", "Auto · opencode".
    virtual QString describeChoice(const ModelChoice &choice) = 0;
    virtual QList<ProviderFacts> providerFacts() = 0;
    virtual QString systemPrompt(const QString &source, AgentRole role) = 0;
    virtual int maxSteps() = 0;
};

// One request for the board.
struct TaskRequest {
    QString text;
    QString source{QStringLiteral("gui")};      // gui | voice | phone | phone-voice | agent (a sub-task)
    QList<ImageAttachment> images;
    std::optional<IntentMatch> fastPath;        // the fast path's tool for it; none: the model's
    QString shownFirst;                         // a line the reply opens with ("Into GNOME Terminal: `ls`")
    std::optional<QString> answer;              // the reply itself, no tool and no model (a switch was flipped)
    std::optional<ModelChoice> model;           // none: what the role is given (Settings → Mission, ModelFit)
    std::optional<AgentRole> role;              // none: from the words (RoleClassifier) or the fast path's tool
    std::optional<TaskId> continues;            // the task whose conversation this one carries on
    std::optional<TaskId> parent;               // delegate_task's sub-task of this task
};

// A question a task waits on (the confirm-risky policy's), shown on its card and in the live activity.
struct Approval {
    quint64 actionId{0};
    TaskId task{0};
    QString agentId;
    QString tool;
    QString question;
    QString risk;
};

// The mission board (Mediator): every request is a task, run by a TaskRunner of its own — its agent, conversation,
// provider instance and model — up to `maxConcurrent` at once, the rest waiting in a queue (first in, first out; a
// resumed task and a parent whose sub-task ended go first). A task holds a slot while it runs or waits for the user;
// the fast path's one tool never waits for one. A lead agent hands parts of its task to agents of other roles with
// delegate_task (one level deep): the parent gives up its slot meanwhile and gets the sub-task's final answer as the
// tool's result. Questions the policy asks are routed to the task that asked (approve / deny), stopping one task stops
// only its tools, and everything that happens goes to the live activity. The policy, the tools and Share screen are
// the app's own, the same for every task.
class MissionControl : public QObject {
    Q_OBJECT
public:
    MissionControl(ToolExecutor *executor, ToolRegistry *registry, ITaskEnvironment *environment, MissionConfig config,
                   QObject *parent = nullptr);
    ~MissionControl() override;

    void setConfig(const MissionConfig &config);     // more slots start what waits
    const MissionConfig &config() const { return config_; }

    TaskId add(const TaskRequest &request);
    // A finished or paused task asked again, in its own conversation.
    void followUp(TaskId id, const QString &text, const QString &source);
    void pause(TaskId id);
    void resume(TaskId id);
    void stop(TaskId id);
    void remove(TaskId id);         // a finished or waiting task leaves the board
    void pauseAll();
    void stopAll();
    void approve(quint64 actionId);
    void deny(quint64 actionId);
    // The executor's confirmer: a task's question goes to its card; one of no task to the fallback (a dialog).
    void confirm(const ConfirmRequest &request, std::function<void(bool)> answer);
    void setFallbackConfirmer(ToolExecutor::Confirmer confirmer) { fallbackConfirmer_ = std::move(confirmer); }
    // delegate_task: a sub-task of `parent` for an agent of `role`; `done` gets its final answer.
    void delegate(TaskId parent, AgentRole role, const QString &text, const QString &context, ITool::Done done);
    // A line of the voice's for the live activity ("wake word armed").
    void noteVoice(const QString &text);

    const Task *task(TaskId id) const;
    QList<Task> tasks() const;                      // oldest first
    QList<TaskId> queue() const { return queue_; }
    int running() const;                            // tasks holding a slot
    int awaiting() const;                           // tasks waiting for the user's answer
    int slotCount() const { return config_.maxConcurrent; }
    int doneToday() const;
    bool isBusy() const { return busy_; }           // something runs, waits for a slot or for a sub-task
    QList<Approval> approvals() const;              // oldest first
    const ActivityFeed &feed() const { return feed_; }
    const TaskTranscript *transcript(TaskId id) const;
    TaskRunner::Metrics metrics(TaskId id) const;
    std::optional<TaskId> newestRunning() const;
    // A spoken summary: "Two tasks running, one needs you: BROWSER-01 wants to run browser_click."
    QString summary() const;

signals:
    void taskAdded(quint64 id);
    void taskChanged(quint64 id);
    void taskRemoved(quint64 id);
    void queueChanged();
    void countsChanged();
    void busyChanged(bool busy);
    void approvalRequested(quint64 task, quint64 actionId);
    void approvalSettled(quint64 task, quint64 actionId, bool allowed);
    void activity(const nexa::ActivityEvent &event);
    void metricsChanged(quint64 id);
    // What a task says, as the chat shows it.
    void taskReplyStarted(quint64 id, const QString &source);
    void taskReplyRoute(quint64 id, const QString &route);
    void taskReplyVia(quint64 id, const QString &route);
    void taskReplyDelta(quint64 id, const QString &delta);
    void taskReplyNotice(quint64 id, const QString &message);
    void taskReplyAction(quint64 id, const QString &tool, bool ok, const QString &summary, qint64 elapsedMs);
    void taskReplyFinished(quint64 id, nexa::FinishReason reason, bool empty);
    void taskReplyFailed(quint64 id, const nexa::Error &error);

private:
    struct Pending {                // what a queued task does once it has a slot
        enum class Kind { Start, FollowUp, Resume, ReturnToParent } kind{Kind::Start};
        QString text;
        QString source;
        QList<ImageAttachment> images;
        std::optional<IntentMatch> match;           // Start: the fast path's tool…
        std::optional<QString> answer;              // …or the reply itself
        QString shownFirst;
        std::optional<Result<ToolResult>> result;   // ReturnToParent: the sub-task's answer…
        ITool::Done done;                           // …for the lead's delegate_task call
    };
    struct Delegation {
        TaskId parent{0};
        ITool::Done done;
    };

    Task *find(TaskId id);
    void enqueue(TaskId id, Pending pending, bool first = false);
    void drain();                                   // start what waits while slots are free
    void dispatch(TaskId id, Pending pending);
    bool equip(Task &task);                         // the runner and its provider; false: failed
    TaskRunner *makeRunner(Task &task);
    void connectRunner(TaskRunner *runner);
    void onFinished(TaskId id, FinishReason reason, bool empty);
    void onFailed(TaskId id, const Error &error);
    void conclude(TaskId id);                       // a task ended: its parent, the slots, the counts
    void returnToParent(TaskId child);
    void settleApprovals(TaskId id);
    void haltChildren(TaskId id);                   // a lead paused or stopped: its sub-tasks stop
    void setState(Task &task, TaskState state);
    void note(const Task &task, ActivityKind kind, const QString &text, bool ok = true);
    void updateCounts();
    void tick();

    ToolExecutor *executor_;
    ToolRegistry *registry_;
    ITaskEnvironment *environment_;
    MissionConfig config_;
    ToolExecutor::Confirmer fallbackConfirmer_;
    QMap<TaskId, Task> tasks_;
    std::map<TaskId, std::unique_ptr<TaskRunner>> runners_;
    QHash<TaskId, TaskTranscript> transcripts_;
    QList<TaskId> queue_;
    QHash<TaskId, Pending> pending_;
    QHash<TaskId, Delegation> delegations_;         // by the sub-task
    QMap<quint64, std::pair<Approval, std::function<void(bool)>>> approvals_;
    QHash<TaskId, int> activityThisSecond_;
    std::optional<TaskId> warm_;                    // the last finished task whose provider is kept for a follow-on
    ActivityFeed feed_;
    QTimer ticker_;
    TaskId nextId_{1};
    QDate doneDate_;
    int doneCount_{0};
    bool busy_{false};
};

} // namespace nexa

#include "core/mission/MissionControl.h"

#include "core/Logging.h"
#include "core/ai/IAiProvider.h"
#include "core/mission/DelegateTool.h"
#include "core/mission/RoleClassifier.h"
#include "core/tools/ToolRegistry.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kActivityBuckets = 24;
constexpr int kTickMs = 1000;
const QString kSubTaskSource = u"agent"_s;

// A tool call in a few words for the live activity: the tool, and its first argument that says something.
QString describeCall(const QString &tool, const QJsonObject &arguments) {
    for (const QString &key : {u"url"_s, u"command"_s, u"text"_s, u"app"_s, u"name"_s, u"query"_s, u"element"_s,
                               u"path"_s, u"task"_s}) {
        const QString value = arguments.value(key).toString().simplified();
        if (!value.isEmpty()) return u"%1 → %2"_s.arg(tool, value.size() > 60 ? value.left(59) + u"…"_s : value);
    }
    return tool;
}

QString plural(int count, const QString &one, const QString &many) {
    return u"%1 %2"_s.arg(count).arg(count == 1 ? one : many);
}

} // namespace

MissionControl::MissionControl(ToolExecutor *executor, ToolRegistry *registry, ITaskEnvironment *environment,
                               MissionConfig config, QObject *parent)
    : QObject(parent),
      executor_(executor),
      registry_(registry),
      environment_(environment),
      config_(std::move(config)) {
    registry_->add(std::make_unique<DelegateTool>(this));
    ticker_.setInterval(kTickMs);
    connect(&ticker_, &QTimer::timeout, this, &MissionControl::tick);
}

MissionControl::~MissionControl() {
    ticker_.stop();
    for (auto &[id, runner] : runners_) runner->disconnect(this);
    runners_.clear();
    registry_->remove(QString(DelegateTool::kName));
}

void MissionControl::setConfig(const MissionConfig &config) {
    config_ = config;
    drain();
}

Task *MissionControl::find(TaskId id) {
    const auto it = tasks_.find(id);
    return it == tasks_.end() ? nullptr : &it.value();
}

const Task *MissionControl::task(TaskId id) const {
    const auto it = tasks_.constFind(id);
    return it == tasks_.cend() ? nullptr : &it.value();
}

QList<Task> MissionControl::tasks() const {
    return tasks_.values();
}

TaskId MissionControl::add(const TaskRequest &request) {
    const TaskId id = nextId_++;
    Task task;
    task.id = id;
    task.text = request.text.trimmed();
    task.title = titleFor(task.text);
    task.source = request.source;
    task.role = request.role               ? *request.role
                : request.fastPath ? RoleClassifier::forTool(request.fastPath->toolName)
                                   : RoleClassifier::classify(task.text);
    task.agentId = agentIdFor(task.role, int(id));
    task.hardcore = request.hardcore;
    if (request.parent) {
        if (const Task *lead = this->task(*request.parent)) {
            task.parent = lead->id;
            task.depth = lead->depth + 1;
            task.hardcore = task.hardcore || lead->hardcore;
        }
    }
    task.model = request.model ? *request.model : ModelFit::resolve(task.role, config_, environment_->providerFacts());
    task.providerName = environment_->describeChoice(task.model);
    task.fastPath = request.fastPath.has_value();
    task.created = QDateTime::currentDateTime();
    task.activity = QList<int>(kActivityBuckets, 0);
    Task &added = tasks_.insert(id, task).value();
    transcripts_[id].append({TranscriptEvent::Kind::User, added.text});
    TaskRunner *runner = makeRunner(added);

    // The conversation it carries on is copied in; the earlier task's provider is handed over while idle, so a
    // program's warm session goes on rather than starting again.
    if (request.continues) {
        if (const auto previous = runners_.find(*request.continues); previous != runners_.end()) {
            runner->conversation() = previous->second->conversation();
            const Task *before = this->task(*request.continues);
            if (before && before->model == added.model && !previous->second->isRunning()) {
                if (std::shared_ptr<IAiProvider> provider = previous->second->releaseProvider()) {
                    runner->setProvider(std::move(provider), environment_->modelFor(added.model));
                }
            }
        }
    }
    emit taskAdded(id);
    note(added, ActivityKind::TaskState, u"new task: %1"_s.arg(added.title));

    Pending pending{Pending::Kind::Start, added.text, added.source, request.images, request.fastPath, request.answer,
                    request.shownFirst, std::nullopt, {}};
    if (request.fastPath || request.answer) {
        dispatch(id, std::move(pending));      // the fast path never waits for a slot
        updateCounts();
    } else {
        enqueue(id, std::move(pending));
    }
    return id;
}

TaskRunner *MissionControl::makeRunner(Task &task) {
    auto runner = std::make_unique<TaskRunner>(task.id, task.source, executor_, registry_, this);
    runner->setMaxSteps(environment_->maxSteps());
    if (task.depth >= 1) {
        runner->setHiddenTools({QString(DelegateTool::kName)}, u"a sub-task cannot hand its work on again"_s);
    }
    TaskRunner *raw = runner.get();
    runners_[task.id] = std::move(runner);
    connectRunner(raw);
    return raw;
}

void MissionControl::connectRunner(TaskRunner *runner) {
    const TaskId id = runner->id();
    connect(runner, &TaskRunner::stepStarted, this, [this, id](int step) {
        Task *task = find(id);
        if (!task) return;
        task->steps = step;
        task->progress = std::max(task->progress, estimateProgress(task->steps, task->toolCalls, task->state));
        task->subStatus = step == 1 ? u"Thinking"_s : u"Thinking · step %1"_s.arg(step);
        emit taskChanged(id);
    });
    connect(runner, &TaskRunner::route, this, [this, id](const QString &route) {
        transcripts_[id].append({TranscriptEvent::Kind::Route, route});
        emit taskReplyRoute(id, route);
    });
    connect(runner, &TaskRunner::textDelta, this, [this, id](const QString &delta) {
        transcripts_[id].append({TranscriptEvent::Kind::Delta, delta});
        ++activityThisSecond_[id];
        if (Task *task = find(id); task && task->subStatus != u"Writing the answer"_s) {
            task->subStatus = u"Writing the answer"_s;
            emit taskChanged(id);
        }
        emit taskReplyDelta(id, delta);
    });
    connect(runner, &TaskRunner::rerouted, this, [this, id](const QString &route, const QString &reason) {
        transcripts_[id].append({TranscriptEvent::Kind::Via, route});
        if (Task *task = find(id)) {
            task->providerName = u"via %1"_s.arg(route);
            note(*task, ActivityKind::Rerouted, u"answered by %1 — %2"_s.arg(route, reason));
            emit taskChanged(id);
        }
        emit taskReplyVia(id, route);
    });
    connect(runner, &TaskRunner::toolStarted, this, [this, id](const QString &tool, const QJsonObject &arguments) {
        Task *task = find(id);
        if (!task) return;
        ++task->toolCalls;
        activityThisSecond_[id] += 3;
        task->subStatus = describeCall(tool, arguments);
        task->progress = std::max(task->progress, estimateProgress(task->steps, task->toolCalls, task->state));
        note(*task, ActivityKind::ToolStarted, describeCall(tool, arguments));
        emit taskChanged(id);
    });
    connect(runner, &TaskRunner::toolFinished, this,
            [this, id](const QString &tool, bool ok, const QString &summary, qint64 ms) {
        transcripts_[id].append({TranscriptEvent::Kind::Action, tool, summary, ok, ms});
        if (Task *task = find(id)) {
            if (task->fastPath) ++task->toolCalls;
            note(*task, ActivityKind::ToolFinished, u"%1 %2 · %3 ms"_s.arg(tool, ok ? u"done"_s : u"failed"_s).arg(ms), ok);
        }
        emit taskReplyAction(id, tool, ok, summary, ms);
    });
    connect(runner, &TaskRunner::notice, this, [this, id](const QString &message) {
        transcripts_[id].append({TranscriptEvent::Kind::Notice, message});
        if (const Task *task = find(id)) note(*task, ActivityKind::Notice, message);
        emit taskReplyNotice(id, message);
    });
    connect(runner, &TaskRunner::finished, this,
            [this, id](FinishReason reason, bool empty) { onFinished(id, reason, empty); });
    connect(runner, &TaskRunner::failed, this, [this, id](const Error &error) { onFailed(id, error); });
    connect(runner, &TaskRunner::metricsChanged, this, [this, id] { emit metricsChanged(id); });
}

void MissionControl::enqueue(TaskId id, Pending pending, bool first) {
    Task *task = find(id);
    if (!task) return;
    if (pending.kind != Pending::Kind::ReturnToParent) setState(*task, TaskState::Queued);
    pending_.insert(id, std::move(pending));
    queue_.removeAll(id);
    if (first) {
        queue_.prepend(id);
    } else {
        queue_.append(id);
    }
    emit queueChanged();
    drain();
}

void MissionControl::drain() {
    while (!queue_.isEmpty() && running() < config_.maxConcurrent) {
        const TaskId id = queue_.takeFirst();
        emit queueChanged();
        dispatch(id, pending_.take(id));
    }
    updateCounts();
}

bool MissionControl::equip(Task &task) {
    TaskRunner *runner = runners_.at(task.id).get();
    if (runner->hasProvider()) return true;
    Result<std::shared_ptr<IAiProvider>> created = environment_->createProvider(task.model);
    if (created) {
        runner->setProvider(std::move(created).value(), environment_->modelFor(task.model));
        return true;
    }
    if (task.fastPath) return true;     // the tool needs no model; the model is missed only if it hands over
    onFailed(task.id, created.error());
    return false;
}

void MissionControl::dispatch(TaskId id, Pending pending) {
    Task *task = find(id);
    if (!task || !runners_.count(id)) return;
    TaskRunner *runner = runners_.at(id).get();

    if (pending.kind == Pending::Kind::ReturnToParent) {
        // The lead takes a slot again and its delegate_task call returns the sub-task's answer.
        setState(*task, TaskState::Running);
        task->subStatus = u"Back from its sub-task"_s;
        emit taskChanged(id);
        if (pending.done && pending.result) pending.done(std::move(*pending.result));
        return;
    }
    if (!pending.answer && !equip(*task)) return;
    if (!task->started.isValid()) task->started = QDateTime::currentDateTime();
    task->ended = {};
    task->lastError.clear();
    setState(*task, TaskState::Running);
    transcripts_[id].append({TranscriptEvent::Kind::ReplyStart});
    emit taskReplyStarted(id, pending.source.isEmpty() ? task->source : pending.source);
    if (!pending.shownFirst.isEmpty()) {
        transcripts_[id].append({TranscriptEvent::Kind::Delta, pending.shownFirst});
        emit taskReplyDelta(id, pending.shownFirst);
    }
    const QString prompt = environment_->systemPrompt(task->source, task->role);
    switch (pending.kind) {
    case Pending::Kind::Start:
        if (pending.answer) {
            runner->answer(pending.text, *pending.answer);
        } else if (pending.match) {
            runner->startFastPath(prompt, pending.text, *pending.match);
        } else {
            runner->start(prompt, pending.text, pending.images);
        }
        break;
    case Pending::Kind::FollowUp: runner->start(prompt, pending.text, pending.images); break;
    case Pending::Kind::Resume: runner->resume(prompt); break;
    case Pending::Kind::ReturnToParent: break;
    }
}

void MissionControl::followUp(TaskId id, const QString &text, const QString &source) {
    Task *task = find(id);
    const QString said = text.trimmed();
    if (!task || said.isEmpty() || (!isFinished(task->state) && task->state != TaskState::Paused)) return;
    transcripts_[id].append({TranscriptEvent::Kind::User, said});
    enqueue(id, Pending{Pending::Kind::FollowUp, said, source, {}, std::nullopt, std::nullopt, {}, std::nullopt, {}});
}

void MissionControl::pause(TaskId id) {
    Task *task = find(id);
    if (!task || isFinished(task->state) || task->state == TaskState::Paused) return;
    if (task->state == TaskState::Queued) {     // what it waited to do stays pending
        queue_.removeAll(id);
        setState(*task, TaskState::Paused);
        emit queueChanged();
        updateCounts();
        return;
    }
    setState(*task, TaskState::Paused);
    task->subStatus = u"Paused"_s;
    settleApprovals(id);
    haltChildren(id);
    runners_.at(id)->pause();       // finished(Cancelled) → onFinished, which sees it paused
    drain();
}

void MissionControl::resume(TaskId id) {
    Task *task = find(id);
    if (!task || task->state != TaskState::Paused) return;
    note(*task, ActivityKind::TaskState, u"resumed"_s);
    if (pending_.contains(id)) {
        enqueue(id, pending_.take(id), true);
    } else {
        enqueue(id, Pending{Pending::Kind::Resume, {}, task->source, {}, std::nullopt, std::nullopt, {}, std::nullopt, {}},
                true);
    }
}

void MissionControl::stop(TaskId id) {
    Task *task = find(id);
    if (!task || isFinished(task->state)) return;
    const bool wasRunning = runners_.count(id) && runners_.at(id)->isRunning();
    queue_.removeAll(id);
    pending_.remove(id);
    setState(*task, TaskState::Stopped);
    task->subStatus = u"Stopped"_s;
    task->ended = QDateTime::currentDateTime();
    settleApprovals(id);
    haltChildren(id);
    emit queueChanged();
    if (wasRunning) {
        runners_.at(id)->stop();    // finished(Cancelled) → onFinished → conclude
        return;
    }
    transcripts_[id].append({TranscriptEvent::Kind::Finished});
    conclude(id);
}

void MissionControl::remove(TaskId id) {
    Task *task = find(id);
    if (!task || holdsSlot(task->state) || task->state == TaskState::Delegating) return;
    if (task->state == TaskState::Queued) stop(id);
    queue_.removeAll(id);
    pending_.remove(id);
    if (const auto it = runners_.find(id); it != runners_.end()) {
        TaskRunner *runner = it->second.release();
        runners_.erase(it);
        runner->disconnect(this);
        runner->deleteLater();
    }
    if (warm_ == id) warm_.reset();
    tasks_.remove(id);
    transcripts_.remove(id);
    activityThisSecond_.remove(id);
    emit taskRemoved(id);
    emit queueChanged();
    updateCounts();
}

void MissionControl::pauseAll() {
    for (const TaskId id : tasks_.keys()) {
        const Task *task = this->task(id);
        if (task && task->depth == 0 && (holdsSlot(task->state) || task->state == TaskState::Queued
                                          || task->state == TaskState::Delegating)) {
            pause(id);
        }
    }
}

void MissionControl::stopAll() {
    for (const TaskId id : tasks_.keys()) {
        if (const Task *task = this->task(id); task && !isFinished(task->state)) stop(id);
    }
}

void MissionControl::haltChildren(TaskId id) {
    for (auto it = delegations_.cbegin(); it != delegations_.cend(); ++it) {
        if (it.value().parent != id) continue;
        const TaskId child = it.key();
        QTimer::singleShot(0, this, [this, child] { stop(child); });    // after the lead's own state is settled
    }
}

void MissionControl::onFinished(TaskId id, FinishReason reason, bool empty) {
    Task *task = find(id);
    if (!task) return;
    transcripts_[id].append({TranscriptEvent::Kind::Finished});
    if (task->state == TaskState::Paused) {
        note(*task, ActivityKind::TaskState, u"paused"_s);
        emit taskReplyFinished(id, FinishReason::Cancelled, empty);
        emit taskChanged(id);
        updateCounts();
        drain();
        return;
    }
    if (task->state != TaskState::Stopped) {
        if (reason == FinishReason::Cancelled) {
            setState(*task, TaskState::Stopped);
            task->subStatus = u"Stopped"_s;
        } else {
            setState(*task, TaskState::Done);
            task->progress = 100;
            task->subStatus = u"Done"_s;
            const QDate today = QDate::currentDate();
            if (doneDate_ != today) {
                doneDate_ = today;
                doneCount_ = 0;
            }
            ++doneCount_;
        }
        task->ended = QDateTime::currentDateTime();
    }
    emit taskReplyFinished(id, reason, empty);
    conclude(id);
}

void MissionControl::onFailed(TaskId id, const Error &error) {
    Task *task = find(id);
    if (!task) return;
    queue_.removeAll(id);
    pending_.remove(id);
    setState(*task, TaskState::Failed);
    task->lastError = error.message;
    task->subStatus = error.message.section(u'\n', 0, 0);
    task->ended = QDateTime::currentDateTime();
    transcripts_[id].append({TranscriptEvent::Kind::Error, error.message});
    note(*task, ActivityKind::TaskState, u"failed: %1"_s.arg(task->subStatus), false);
    emit taskReplyFailed(id, error);
    conclude(id);
}

void MissionControl::conclude(TaskId id) {
    Task *task = find(id);
    if (!task) return;
    note(*task, ActivityKind::TaskState, taskStateName(task->state).toLower(), task->state != TaskState::Failed);
    // A program's warm session is kept for the conversation's next request: the last finished task's provider stays,
    // the one before it goes. A sub-task's, a stopped or a failed one's goes at once.
    if (const auto it = runners_.find(id); it != runners_.end()) {
        if (task->depth == 0 && task->state == TaskState::Done) {
            if (warm_ && *warm_ != id) {
                if (const auto old = runners_.find(*warm_); old != runners_.end()) old->second->releaseProvider();
            }
            warm_ = id;
        } else {
            it->second->releaseProvider();
        }
    }
    emit taskChanged(id);
    if (delegations_.contains(id)) returnToParent(id);
    updateCounts();
    drain();
}

void MissionControl::returnToParent(TaskId child) {
    const Delegation delegation = delegations_.take(child);
    const Task *sub = find(child);
    Result<ToolResult> result = Error{ErrorCode::Cancelled, u"The sub-task was stopped."_s};
    if (sub && sub->state == TaskState::Done) {
        const QString reply = transcripts_.value(child).lastReply();
        result = ToolResult{reply.isEmpty() ? u"The sub-task finished without an answer."_s
                                            : u"%1 answered:\n%2"_s.arg(sub->agentId, reply),
                            std::nullopt};
    } else if (sub && sub->state == TaskState::Failed) {
        result = Error{ErrorCode::Unknown, u"The sub-task failed: %1"_s.arg(sub->lastError)};
    }
    Task *lead = find(delegation.parent);
    if (!lead || lead->state != TaskState::Delegating) {
        if (delegation.done) delegation.done(std::move(result));    // the lead was stopped: the executor drops it
        return;
    }
    enqueue(delegation.parent, Pending{Pending::Kind::ReturnToParent, {}, {}, {}, std::nullopt, std::nullopt, {},
                                       std::move(result), delegation.done},
            true);
}

void MissionControl::setHardcore(TaskId id, bool hardcore) {
    Task *task = find(id);
    if (!task || task->hardcore == hardcore) return;
    task->hardcore = hardcore;
    emit taskChanged(id);
}

void MissionControl::releaseHardcore() {
    QList<TaskId> changed;
    for (Task &task : tasks_) {
        if (!task.hardcore) continue;
        task.hardcore = false;
        changed.append(task.id);
    }
    for (const TaskId id : std::as_const(changed)) emit taskChanged(id);
}

void MissionControl::delegate(TaskId parent, AgentRole role, const QString &text, const QString &context,
                              ITool::Done done) {
    Task *lead = find(parent);
    if (!lead || isFinished(lead->state)) {
        done(Error{ErrorCode::NotFound, u"The task that asked is no longer on the board"_s});
        return;
    }
    if (lead->depth >= 1) {
        done(Error{ErrorCode::NotSupported, u"delegate_task is not available in a sub-task"_s});
        return;
    }
    TaskRequest request;
    request.text = context.isEmpty() ? text : u"%1\n\nWhat the task that asked knows:\n%2"_s.arg(text, context);
    request.source = kSubTaskSource;
    request.role = role;
    request.parent = parent;
    // The lead gives its slot up while the sub-task works; its call returns when the sub-task ends.
    delegations_.insert(nextId_, Delegation{parent, std::move(done)});
    setState(*lead, TaskState::Delegating);
    const TaskId child = add(request);
    if (Task *waiting = find(parent)) {
        if (const Task *sub = task(child)) {
            waiting->subStatus = u"Waiting for %1"_s.arg(sub->agentId);
            note(*waiting, ActivityKind::TaskState, u"hands over to %1: %2"_s.arg(sub->agentId, sub->title));
        }
        emit taskChanged(parent);
    }
}

void MissionControl::confirm(const ConfirmRequest &request, std::function<void(bool)> answer) {
    Task *task = request.task ? find(request.task) : nullptr;
    if (!task || isFinished(task->state)) {
        if (fallbackConfirmer_) {
            fallbackConfirmer_(request, std::move(answer));
        } else {
            answer(false);
        }
        return;
    }
    const Approval approval{request.id, task->id, task->agentId, request.tool, request.question, request.risk};
    approvals_.insert(request.id, {approval, std::move(answer)});
    setState(*task, TaskState::AwaitingYou);
    task->subStatus = u"Waiting for your approval: %1"_s.arg(request.tool);
    note(*task, ActivityKind::Approval, u"needs you: %1"_s.arg(describeCall(request.tool, request.arguments)));
    emit taskChanged(task->id);
    emit approvalRequested(task->id, request.id);
    updateCounts();
}

void MissionControl::approve(quint64 actionId) {
    const auto it = approvals_.find(actionId);
    if (it == approvals_.end()) return;
    auto [approval, answer] = std::move(it.value());
    approvals_.erase(it);
    if (Task *task = find(approval.task)) {
        if (task->state == TaskState::AwaitingYou) setState(*task, TaskState::Running);
        task->subStatus = u"Approved: %1"_s.arg(approval.tool);
        note(*task, ActivityKind::ApprovalSettled, u"approved %1"_s.arg(approval.tool));
        emit taskChanged(approval.task);
    }
    emit approvalSettled(approval.task, actionId, true);
    updateCounts();
    answer(true);
}

void MissionControl::deny(quint64 actionId) {
    const auto it = approvals_.find(actionId);
    if (it == approvals_.end()) return;
    auto [approval, answer] = std::move(it.value());
    approvals_.erase(it);
    if (Task *task = find(approval.task)) {
        if (task->state == TaskState::AwaitingYou) setState(*task, TaskState::Running);
        task->subStatus = u"Denied: %1"_s.arg(approval.tool);
        note(*task, ActivityKind::ApprovalSettled, u"denied %1"_s.arg(approval.tool), false);
        emit taskChanged(approval.task);
    }
    emit approvalSettled(approval.task, actionId, false);
    updateCounts();
    answer(false);
}

void MissionControl::settleApprovals(TaskId id) {
    // The question itself is withdrawn by the executor when the task's tools are cancelled: no answer is given here.
    QList<quint64> settled;
    for (auto it = approvals_.cbegin(); it != approvals_.cend(); ++it) {
        if (it.value().first.task == id) settled.append(it.key());
    }
    for (const quint64 actionId : settled) {
        approvals_.remove(actionId);
        emit approvalSettled(id, actionId, false);
    }
}

QList<Approval> MissionControl::approvals() const {
    QList<Approval> list;
    for (const auto &[approval, answer] : approvals_) list.append(approval);
    return list;
}

void MissionControl::noteVoice(const QString &text) {
    const ActivityEvent event{QDateTime::currentDateTime(), ActivityKind::Voice, Lane::Voice, std::nullopt,
                              u"VOICE"_s, text, true};
    feed_.push(event);
    emit activity(event);
}

void MissionControl::note(const Task &task, ActivityKind kind, const QString &text, bool ok) {
    const ActivityEvent event{QDateTime::currentDateTime(), kind, laneFor(task.role), task.id, task.agentId, text, ok};
    feed_.push(event);
    emit activity(event);
}

void MissionControl::setState(Task &task, TaskState state) {
    if (task.state == state) return;
    task.state = state;
    task.progress = std::max(task.progress, estimateProgress(task.steps, task.toolCalls, state));
    emit taskChanged(task.id);
}

int MissionControl::running() const {
    return int(std::count_if(tasks_.cbegin(), tasks_.cend(), [](const Task &task) { return holdsSlot(task.state); }));
}

int MissionControl::awaiting() const {
    return int(std::count_if(tasks_.cbegin(), tasks_.cend(),
                             [](const Task &task) { return task.state == TaskState::AwaitingYou; }));
}

int MissionControl::doneToday() const {
    return doneDate_ == QDate::currentDate() ? doneCount_ : 0;
}

const TaskTranscript *MissionControl::transcript(TaskId id) const {
    const auto it = transcripts_.constFind(id);
    return it == transcripts_.cend() ? nullptr : &it.value();
}

TaskRunner::Metrics MissionControl::metrics(TaskId id) const {
    const auto it = runners_.find(id);
    return it == runners_.end() ? TaskRunner::Metrics{} : it->second->metrics();
}

std::optional<TaskId> MissionControl::newestRunning() const {
    for (auto it = tasks_.cend(); it != tasks_.cbegin();) {
        --it;
        if (holdsSlot(it.value().state)) return it.key();
    }
    return std::nullopt;
}

QString MissionControl::summary() const {
    const int now = running();
    const int waiting = int(queue_.size());
    const QList<Approval> asking = approvals();
    if (now == 0 && waiting == 0) return u"Nothing is running."_s;
    QString text = now == 0 ? u"Nothing is running"_s : plural(now, u"task running"_s, u"tasks running"_s);
    if (!asking.isEmpty()) {
        text += u", %1 needs you: %2 wants to use %3"_s.arg(asking.size()).arg(asking.first().agentId, asking.first().tool);
    }
    if (waiting > 0) text += u", %1 waiting"_s.arg(waiting);
    return text + u"."_s;
}

void MissionControl::updateCounts() {
    const bool active = std::any_of(tasks_.cbegin(), tasks_.cend(), [](const Task &task) {
        return holdsSlot(task.state) || task.state == TaskState::Delegating;
    });
    const bool busy = active || !queue_.isEmpty();
    if (active && !ticker_.isActive()) ticker_.start();
    if (!active && ticker_.isActive()) ticker_.stop();
    emit countsChanged();
    if (busy != busy_) {
        busy_ = busy;
        emit busyChanged(busy);
    }
}

void MissionControl::tick() {
    for (Task &task : tasks_) {
        if (!holdsSlot(task.state) && task.state != TaskState::Delegating) continue;
        if (task.activity.size() >= kActivityBuckets) task.activity.removeFirst();
        task.activity.append(activityThisSecond_.take(task.id));
        emit taskChanged(task.id);
    }
}

} // namespace nexa

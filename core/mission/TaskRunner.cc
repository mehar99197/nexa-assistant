#include "core/mission/TaskRunner.h"

#include "core/Logging.h"
#include "core/ai/IAiProvider.h"
#include "core/tools/ToolExecutor.h"

#include <QPointer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kMaxShownOutput = 6'000;
constexpr qint64 kMetricsEveryMs = 250;

} // namespace

TaskRunner::TaskRunner(TaskId id, QString source, ToolExecutor *executor, ToolRegistry *registry, QObject *parent)
    : QObject(parent), id_(id), source_(std::move(source)), executor_(executor), agent_(executor, registry) {
    agent_.setTask(id_);
    connect(&agent_, &Agent::stepStarted, this, [this](int step) {
        stepClock_.start();
        awaitingFirstWord_ = true;
        stepChars_ = 0;
        metrics_.steps = step;
        emit stepStarted(step);
        emit metricsChanged();
    });
    connect(&agent_, &Agent::textDelta, this, [this](const QString &delta) {
        hasContent_ = true;
        noteWords(delta);
        emit textDelta(delta);
    });
    connect(&agent_, &Agent::toolStarted, this, &TaskRunner::toolStarted);
    connect(&agent_, &Agent::toolFinished, this, [this](const QString &tool, bool ok, const QString &summary, qint64 ms) {
        hasContent_ = true;
        emit toolFinished(tool, ok, summary, ms);
    });
    connect(&agent_, &Agent::notice, this, &TaskRunner::notice);
    connect(&agent_, &Agent::rerouted, this, &TaskRunner::rerouted);
    connect(&agent_, &Agent::finished, this, [this](FinishReason reason) { end(reason); });
    connect(&agent_, &Agent::failed, this, [this](const Error &error) {
        running_ = false;
        emit failed(error);
    });
}

TaskRunner::~TaskRunner() {
    agent_.disconnect(this);
    agent_.cancel();
}

void TaskRunner::setProvider(std::shared_ptr<IAiProvider> provider, const QString &model) {
    provider_ = std::move(provider);
    model_ = model;
}

std::shared_ptr<IAiProvider> TaskRunner::releaseProvider() {
    if (running_) return nullptr;
    return std::exchange(provider_, nullptr);
}

void TaskRunner::start(const QString &systemPrompt, const QString &text, const QList<ImageAttachment> &images) {
    conversation_.addUser(markedForModel(text, source_), images);
    runAgent(systemPrompt);
}

void TaskRunner::runAgent(const QString &systemPrompt) {
    if (!provider_) {
        running_ = false;
        emit failed(Error{ErrorCode::NotFound, u"No AI provider is configured — open Settings."_s});
        return;
    }
    conversation_.setSystemPrompt(systemPrompt);
    running_ = true;
    fastPath_ = false;
    hasContent_ = false;
    emit route(u"agent"_s);
    agent_.run(provider_.get(), &conversation_, model_, source_);
}

void TaskRunner::startFastPath(const QString &systemPrompt, const QString &text, const IntentMatch &match) {
    running_ = true;
    fastPath_ = true;
    hasContent_ = false;
    emit route(u"fast"_s);
    const quint64 generation = ++generation_;
    QElapsedTimer timer;
    timer.start();
    qCDebug(lcApp) << "fast path:" << match.toolName << match.arguments;
    const QPointer<TaskRunner> self(this);
    executor_->execute(match.toolName, match.arguments, ToolContext{source_, id_},
                       [self, generation, timer, systemPrompt, text, match](Result<ToolResult> result) {
        if (!self || generation != self->generation_) return;     // paused or stopped meanwhile
        TaskRunner *runner = self.data();
        const qint64 elapsed = timer.elapsed();
        if (!result && result.error().code == ErrorCode::Cancelled) {
            runner->end(FinishReason::Cancelled);
            return;
        }
        // The fast path could not resolve it (an app name it does not know, say): the model tries.
        if (!result && result.error().code == ErrorCode::NotFound && !match.forwardToAgent && runner->hasProvider()) {
            qCDebug(lcApp) << "fast path missed, asking the model:" << result.error().message;
            runner->conversation_.addUser(markedForModel(text, runner->source_));
            runner->runAgent(systemPrompt);
            return;
        }

        const QString summary = result ? summarize(result.value().text) : result.error().message;
        runner->hasContent_ = true;
        emit runner->toolFinished(match.toolName, result.isOk(), summary, elapsed);
        if (!result) {
            runner->running_ = false;
            emit runner->failed(result.error());
            return;
        }
        if (match.forwardToAgent) {
            if (!runner->hasProvider()) {
                runner->end(FinishReason::Stop);
                return;
            }
            QList<ImageAttachment> images;
            if (result.value().image) images.append(*result.value().image);
            const QString question = match.agentPrompt.isEmpty() ? text : match.agentPrompt;
            runner->conversation_.addUser(
                u"%1\n\n(%2)"_s.arg(markedForModel(question, runner->source_), result.value().text), images);
            runner->runAgent(systemPrompt);
            return;
        }
        // Multi-line results (command output, …) are shown in full under the step line.
        const QString details = result.value().text.section(u'\n', 1).trimmed();
        if (!details.isEmpty()) {
            emit runner->textDelta(u"```text\n"_s
                                   + (details.size() > kMaxShownOutput ? details.left(kMaxShownOutput) + u"\n…"_s : details)
                                   + u"\n```"_s);
        }
        // The model's view of the conversation stays complete for later questions. The result stands as the reply, in
        // plain words: a bracketed note would be copied back by some models ("Done directly with …").
        runner->conversation_.addUser(markedForModel(text, runner->source_));
        runner->conversation_.addAssistant(result.value().text);
        runner->end(FinishReason::Stop);
    });
}

void TaskRunner::pause() {
    halt(u"Paused by the user."_s);
}

void TaskRunner::resume(const QString &systemPrompt) {
    conversation_.addUser(u"Continue from where you paused."_s);
    runAgent(systemPrompt);
}

void TaskRunner::stop() {
    halt({});
}

void TaskRunner::halt(const QString &note) {
    if (!running_) return;
    if (agent_.isRunning()) {
        agent_.cancel(note);        // finished(Cancelled) → end()
        return;
    }
    // The fast path's tool: its result is dropped, the tool told to stop.
    ++generation_;
    executor_->cancelTask(id_);
    end(FinishReason::Cancelled);
}

void TaskRunner::end(FinishReason reason) {
    if (!running_) return;
    running_ = false;
    emit finished(reason, !hasContent_);
}

void TaskRunner::noteWords(const QString &delta) {
    if (awaitingFirstWord_ && stepClock_.isValid()) {
        awaitingFirstWord_ = false;
        metrics_.firstTokenMs = stepClock_.elapsed();
        wordsClock_.start();
    }
    stepChars_ += delta.size();
    if (wordsClock_.isValid()) {
        const double seconds = std::max(0.25, double(wordsClock_.elapsed()) / 1000.0);
        metrics_.tokensPerSecond = double(stepChars_) / 4.0 / seconds;
    }
    if (!metricsClock_.isValid() || metricsClock_.elapsed() >= kMetricsEveryMs) {
        metricsClock_.start();
        emit metricsChanged();
    }
}

} // namespace nexa

#pragma once

#include "core/Result.h"
#include "core/agent/Agent.h"
#include "core/agent/Conversation.h"
#include "core/ai/ChatStream.h"
#include "core/intent/IIntentHandler.h"
#include "core/mission/Task.h"

#include <QElapsedTimer>
#include <QObject>
#include <QSet>

#include <memory>

namespace nexa {

class IAiProvider;
class ToolExecutor;
class ToolRegistry;

// One task's run (Facade over an Agent, its Conversation and its provider instance): a request to the model, or the
// fast path's one tool (and the model after it when the tool says so), followed by follow-ups, pauses and a stop.
// Its tools run for its task (ToolContext::task), so stopping it stops nothing else. It measures what the window
// shows: the time to the first word of each step, and the words per second after it (four characters a token).
class TaskRunner : public QObject {
    Q_OBJECT
public:
    struct Metrics {
        qint64 firstTokenMs{-1};        // the current step's time to its first word; -1 before one
        double tokensPerSecond{0};
        int steps{0};
    };

    TaskRunner(TaskId id, QString source, ToolExecutor *executor, ToolRegistry *registry, QObject *parent = nullptr);
    ~TaskRunner() override;

    TaskId id() const { return id_; }
    // What the model is asked through; none: the fast path only. `model` empty: the provider's own default.
    void setProvider(std::shared_ptr<IAiProvider> provider, const QString &model);
    bool hasProvider() const { return provider_ != nullptr; }
    // Hands the provider over (to the next task of a conversation); the runner is left without one.
    std::shared_ptr<IAiProvider> releaseProvider();
    void setMaxSteps(int steps) { agent_.setMaxSteps(steps); }
    void setHiddenTools(const QSet<QString> &names, const QString &reason) { agent_.setHiddenTools(names, reason); }
    Conversation &conversation() { return conversation_; }
    const Conversation &conversation() const { return conversation_; }
    bool isRunning() const { return running_; }
    Metrics metrics() const { return metrics_; }

    // The user's request to the model; `text` as said (the [voice] mark is added for a spoken one).
    void start(const QString &systemPrompt, const QString &text, const QList<ImageAttachment> &images = {});
    // The fast path's tool for `text`; the model takes over when the tool cannot resolve it (NotFound) or the
    // match forwards its result (a screenshot to describe).
    void startFastPath(const QString &systemPrompt, const QString &text, const IntentMatch &match);
    // A reply with no tool and no model (a switch flipped): shown, spoken and remembered like any other.
    void answer(const QString &text, const QString &reply);
    void pause();                               // stops what runs; the conversation keeps everything so far
    void resume(const QString &systemPrompt);   // goes on from where it paused
    void stop();

signals:
    void stepStarted(int step);
    void route(const QString &route);           // "fast" | "agent"
    void textDelta(const QString &delta);
    void rerouted(const QString &route, const QString &reason);
    void toolStarted(const QString &tool, const QJsonObject &arguments);
    void toolFinished(const QString &tool, bool ok, const QString &summary, qint64 elapsedMs);
    void notice(const QString &message);
    void finished(nexa::FinishReason reason, bool empty);
    void failed(const nexa::Error &error);
    void metricsChanged();                      // at most four times a second

private:
    void runAgent(const QString &systemPrompt);
    void halt(const QString &note);             // pause and stop
    void end(FinishReason reason);
    void noteWords(const QString &delta);

    TaskId id_;
    QString source_;
    ToolExecutor *executor_;
    ToolRegistry *registry_;
    Agent agent_;
    Conversation conversation_;
    std::shared_ptr<IAiProvider> provider_;
    QString model_;
    bool running_{false};
    bool hasContent_{false};
    bool fastPath_{false};
    quint64 generation_{0};                     // a fast-path result after a pause or stop is dropped
    Metrics metrics_;
    QElapsedTimer stepClock_;
    QElapsedTimer wordsClock_;
    QElapsedTimer metricsClock_;
    bool awaitingFirstWord_{false};
    qsizetype stepChars_{0};
};

} // namespace nexa

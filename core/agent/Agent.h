#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"
#include "core/ai/ChatStream.h"
#include "core/tools/TextToolParser.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>

#include <memory>
#include <optional>

namespace nexa {

class Conversation;
class IAiProvider;
class ToolExecutor;
class ToolRegistry;

// The model ↔ tools loop: stream a reply; when it asks for tools, run them in order through the
// ToolExecutor (so policy and audit apply), append the results and ask again, until the model
// answers without tool calls or the step limit is reached.
class Agent : public QObject {
    Q_OBJECT
public:
    Agent(ToolExecutor *executor, ToolRegistry *registry, QObject *parent = nullptr);

    // The conversation must already end with the user's message. It receives every assistant
    // message and tool result of the run, and must outlive it.
    void run(IAiProvider *provider, Conversation *conversation, const QString &model, const QString &source);
    // Stops the stream and the running tool; emits finished(FinishReason::Cancelled). The tool calls left open are
    // answered with `note` ("Cancelled by the user." when empty; a pause says "Paused by the user.").
    void cancel(const QString &note = {});
    bool isRunning() const { return running_; }
    void setMaxSteps(int steps) { maxSteps_ = steps; }
    // The task on the mission board this agent works for: its tools run for it (ToolContext::task), and cancelling
    // stops only its tools. 0: none — cancelling then stops every tool.
    void setTask(quint64 task) { task_ = task; }
    quint64 task() const { return task_; }
    // Tools this agent does not offer the model (delegate_task in a sub-task); a call to one is answered with
    // `reason` and nothing runs.
    void setHiddenTools(const QSet<QString> &names, const QString &reason) {
        hidden_ = names;
        hiddenReason_ = reason;
    }
    // A model that refused a request because of the images in it (a text-only model): from then
    // on its requests go without images, with a note where each one was.
    bool cannotSeeImages(const QString &model) const { return blindModels_.contains(model); }

signals:
    // A request to the model is on its way: step 1, 2, … (a retry of the same step says the same number again).
    void stepStarted(int step);
    void textDelta(const QString &delta);
    void toolStarted(const QString &tool, const QJsonObject &arguments);
    void toolFinished(const QString &tool, bool ok, const QString &summary, qint64 elapsedMs);
    void finished(nexa::FinishReason reason);
    void failed(const nexa::Error &error);
    // Something the user should know that is not the end of the reply: the model cannot see
    // the screenshot, say.
    void notice(const QString &message);
    // The first-choice provider could not answer, so `route` did (ProviderChain).
    void rerouted(const QString &route, const QString &reason);

private:
    void requestStep();
    void sendStep();                            // the request of the current step (again, after a refusal)
    void onStepFinished(ChatStream *stream, FinishReason reason);
    bool refusesImages(const Error &error) const;       // a 400-class error refusing image content
    static bool hasImages(const ChatRequest &request);
    static void stripImages(ChatRequest &request);      // images out, a note in their place
    void runNextTool();
    // Every tool call the model made needs a result, or the next request is rejected.
    void closeOpenCalls(const QString &text);
    void stop(std::optional<Error> error, FinishReason reason);
    QList<ToolSpec> offeredTools() const;       // the registry's, without the hidden ones
    QString textScreenHint() const;             // "…read as text with screen_snapshot" when that tool is offered

    ToolExecutor *executor_;
    ToolRegistry *registry_;
    IAiProvider *provider_{nullptr};
    Conversation *conversation_{nullptr};
    QString model_;
    QString source_;
    quint64 task_{0};
    QSet<QString> hidden_;
    QString hiddenReason_;
    QString cancelNote_;
    std::shared_ptr<ChatStream> stream_;
    QList<ToolCall> pendingCalls_;
    std::optional<ToolCall> currentCall_;
    QList<ImageAttachment> pendingImages_;
    int step_{0};
    int maxSteps_{8};
    quint64 generation_{0};
    bool running_{false};
    // The provider cannot call tools natively: they are described in the prompt and read back out
    // of the reply, which streams through filter_ so the call markup never reaches the screen.
    bool textTools_{false};
    TextToolParser::StreamFilter filter_;
    bool filtering_{false};         // this step's words go through filter_ (text tools, or a fallback answered)
    QSet<QString> blindModels_;
    QSet<QString> sightlessNoticed_;    // providers without vision the user has been told about
    QString pendingBlind_;          // the model whose image-free retry is on its way
    bool stepHadImages_{false};     // the request on its way carries images
};

} // namespace nexa

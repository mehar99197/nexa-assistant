#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"
#include "core/ai/ChatStream.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>

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
    // Stops the stream and the running tool; emits finished(FinishReason::Cancelled).
    void cancel();
    bool isRunning() const { return running_; }
    void setMaxSteps(int steps) { maxSteps_ = steps; }

signals:
    void textDelta(const QString &delta);
    void toolStarted(const QString &tool, const QJsonObject &arguments);
    void toolFinished(const QString &tool, bool ok, const QString &summary, qint64 elapsedMs);
    void finished(nexa::FinishReason reason);
    void failed(const nexa::Error &error);

private:
    void requestStep();
    void onStepFinished(ChatStream *stream, FinishReason reason);
    void runNextTool();
    // Every tool call the model made needs a result, or the next request is rejected.
    void closeOpenCalls(const QString &text);
    void stop(std::optional<Error> error, FinishReason reason);

    ToolExecutor *executor_;
    ToolRegistry *registry_;
    IAiProvider *provider_{nullptr};
    Conversation *conversation_{nullptr};
    QString model_;
    QString source_;
    std::shared_ptr<ChatStream> stream_;
    QList<ToolCall> pendingCalls_;
    std::optional<ToolCall> currentCall_;
    QList<ImageAttachment> pendingImages_;
    int step_{0};
    int maxSteps_{8};
    quint64 generation_{0};
    bool running_{false};
};

} // namespace nexa

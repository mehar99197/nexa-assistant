#include "core/agent/Agent.h"

#include "core/Logging.h"
#include "core/agent/Conversation.h"
#include "core/ai/IAiProvider.h"
#include "core/tools/TextToolParser.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

Agent::Agent(ToolExecutor *executor, ToolRegistry *registry, QObject *parent)
    : QObject(parent), executor_(executor), registry_(registry) {}

void Agent::run(IAiProvider *provider, Conversation *conversation, const QString &model, const QString &source) {
    if (running_) cancel();
    provider_ = provider;
    conversation_ = conversation;
    model_ = model;
    source_ = source;
    step_ = 0;
    pendingCalls_.clear();
    currentCall_.reset();
    pendingImages_.clear();
    pendingBlind_.clear();
    ++generation_;
    running_ = true;
    requestStep();
}

void Agent::cancel() {
    if (!running_) return;
    const bool toolRunning = currentCall_.has_value();
    if (stream_) {
        stream_->disconnect(this);
        stream_->cancel();
    }
    stop(std::nullopt, FinishReason::Cancelled);
    // After stop(): the generation has moved on, so a tool that reports synchronously from
    // cancel() is ignored instead of starting another round.
    if (toolRunning) executor_->cancelAll();
}

void Agent::requestStep() {
    if (step_ >= maxSteps_) {
        stop(Error{ErrorCode::Unknown,
                   u"Stopped after %1 tool rounds without a final answer (agent.maxToolRounds in config.json "
                   "allows more)."_s.arg(maxSteps_)},
             FinishReason::Other);
        return;
    }
    ++step_;
    sendStep();
}

void Agent::sendStep() {
    const QList<ToolSpec> tools = registry_->specs();
    textTools_ = !provider_->capabilities().nativeTools;
    ChatRequest request;
    if (textTools_) {
        request = conversation_->buildRequest(model_);
        request.messages = TextToolParser::asText(request.messages);
        const QString convention = TextToolParser::instructions(tools);
        if (!request.messages.isEmpty() && request.messages.first().role == Role::System) {
            request.messages.first().text += u"\n\n"_s + convention;
        } else {
            request.messages.prepend(ChatMessage{.role = Role::System, .text = convention});
        }
    } else {
        request = conversation_->buildRequest(model_, tools);
    }
    // A model known to refuse images gets the words alone, and a note where each image was.
    stepHadImages_ = hasImages(request);
    if (stepHadImages_ && (blindModels_.contains(model_) || pendingBlind_ == model_)) {
        stripImages(request);
        stepHadImages_ = false;
    }
    stream_ = provider_->chat(request);
    ChatStream *stream = stream_.get();
    filter_ = {};
    if (textTools_) {
        connect(stream, &ChatStream::textDelta, this, [this](const QString &delta) {
            if (const QString shown = filter_.feed(delta); !shown.isEmpty()) emit textDelta(shown);
        });
    } else {
        connect(stream, &ChatStream::textDelta, this, &Agent::textDelta);
    }
    connect(stream, &ChatStream::finished, this, [this, stream](FinishReason reason) { onStepFinished(stream, reason); });
    connect(stream, &ChatStream::failed, this, [this, stream](const Error &error) {
        if (stream != stream_.get()) return;
        // The model refused the images (a text-only model): the same step once more without
        // them, and the user told — otherwise every request after the screenshot would fail. The
        // model is remembered as blind only once the retry gets through: a retry that fails too
        // has taught nothing about images.
        if (stepHadImages_ && pendingBlind_.isEmpty() && refusesImages(error)) {
            pendingBlind_ = model_;
            qCInfo(lcAi) << model_ << "cannot see images:" << error.message;
            emit notice(u"%1 cannot see images, so the screenshot was left out of the request. A vision-capable "
                        "model is needed to look at the screen."_s.arg(model_));
            stream_.reset();
            stepHadImages_ = false;
            sendStep();
            return;
        }
        pendingBlind_.clear();
        if (!stream->text().isEmpty()) conversation_->addAssistant(stream->text());
        stop(error, FinishReason::Other);
    });
}

bool Agent::refusesImages(const Error &error) const {
    if (error.code != ErrorCode::Http) return false;
    // A rejected request body is a 400-class answer ("HTTP 400: …", or a gateway's "[400]: …"
    // inside the stream); a rate limit or an outage that happens to mention images is not.
    static const QRegularExpression status(u"^\\[?(?:HTTP )?(\\d{3})\\]?"_s);
    if (const QRegularExpressionMatch m = status.match(error.message); m.hasMatch()) {
        const int code = m.captured(1).toInt();
        if (code != 400 && code != 415 && code != 422) return false;
    }
    // The model's own name may say "vision"; it is not the complaint. Whole names only: the
    // letters of a short id must not be cut out of other words.
    QString text = error.message;
    if (!model_.isEmpty()) {
        text.remove(QRegularExpression(u"(?<![A-Za-z0-9])%1(?![A-Za-z0-9])"_s.arg(QRegularExpression::escape(model_)),
                                       QRegularExpression::CaseInsensitiveOption));
    }
    // "content[1].type has unsupported value \"image_url\"", "does not support image input",
    // "Image input modality is not enabled": a refusal, and the word, within one clause.
    static const QRegularExpression about(
        u"(unsupported|not support|does not accept|not enabled|not allowed|not available|invalid|cannot|can't|only)"
        "[^.;]{0,60}(image|vision|visual|multimodal|modality)"
        "|(image|vision|visual|multimodal|modality)[^.;]{0,60}(unsupported|not support|not enabled|not allowed|not available|invalid)"_s,
        QRegularExpression::CaseInsensitiveOption);
    return about.match(text).hasMatch();
}

bool Agent::hasImages(const ChatRequest &request) {
    for (const ChatMessage &message : request.messages) {
        if (!message.images.isEmpty()) return true;
    }
    return false;
}

void Agent::stripImages(ChatRequest &request) {
    for (ChatMessage &message : request.messages) {
        if (message.images.isEmpty()) continue;
        message.images.clear();
        message.text += u"\n[An image was attached here — a screenshot — but this model cannot see images, so it was "
                        "left out. If the user asked about it, say that a vision-capable model is needed to look at "
                        "the screen.]"_s;
    }
}

void Agent::onStepFinished(ChatStream *stream, FinishReason reason) {
    if (stream != stream_.get()) return;
    if (!pendingBlind_.isEmpty()) {
        blindModels_.insert(pendingBlind_);     // the retry without images got through: it was the images
        pendingBlind_.clear();
    }
    QString text = stream->text();
    QList<ToolCall> calls = stream->toolCalls();
    stream_.reset();

    // Calls written as text: the convention for providers without tools, and a habit of some
    // models even with them.
    if (calls.isEmpty()) {
        QStringList known;
        for (const ToolSpec &spec : registry_->specs()) known.append(spec.name);
        TextToolParser::Parsed parsed = TextToolParser::parse(text, known);
        if (!parsed.calls.isEmpty()) {
            for (qsizetype i = 0; i < parsed.calls.size(); ++i) parsed.calls[i].id = u"text_%1_%2"_s.arg(step_).arg(i + 1);
            text = parsed.text;
            calls = parsed.calls;
        }
    }
    // What the filter held back while the reply streamed: everything after the words already shown.
    if (textTools_) {
        const QString shown = filter_.shown();
        qsizetype start = 0;    // the filter never shows leading whitespace
        while (start < text.size() && text.at(start).isSpace()) ++start;
        qsizetype common = 0;
        while (common < shown.size() && start + common < text.size() && shown.at(common) == text.at(start + common)) {
            ++common;
        }
        if (const QString rest = text.mid(start + common); !rest.trimmed().isEmpty()) emit textDelta(rest);
    }

    if (calls.isEmpty()) {
        if (!text.isEmpty()) conversation_->addAssistant(text);
        stop(std::nullopt, reason);
        return;
    }
    conversation_->addAssistant(text, calls);
    pendingCalls_ = calls;
    runNextTool();
}

void Agent::runNextTool() {
    if (pendingCalls_.isEmpty()) {
        // Tool messages carry text only; images (screenshots) go back as a user message.
        if (!pendingImages_.isEmpty()) {
            conversation_->addUser(u"[Image returned by the tool call above]"_s, pendingImages_);
            pendingImages_.clear();
        }
        requestStep();
        return;
    }

    const ToolCall call = pendingCalls_.takeFirst();
    currentCall_ = call;
    QJsonParseError parseError;
    const QJsonDocument arguments = QJsonDocument::fromJson(call.argumentsJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !arguments.isObject()) {
        const QString message = u"Error: the arguments were not a valid JSON object (%1)."_s.arg(parseError.errorString());
        conversation_->addToolResult(call.id, call.name, message);
        currentCall_.reset();
        emit toolFinished(call.name, false, message, 0);
        runNextTool();
        return;
    }

    // Withheld for now (the screen while it is not shared): the model learns why, nothing runs.
    if (const QString reason = registry_->withheldReason(call.name); !reason.isEmpty()) {
        const QString message = u"Error: %1 cannot be used now — %2"_s.arg(call.name, reason);
        conversation_->addToolResult(call.id, call.name, message);
        currentCall_.reset();
        emit toolFinished(call.name, false, message, 0);
        runNextTool();
        return;
    }

    emit toolStarted(call.name, arguments.object());
    QElapsedTimer timer;
    timer.start();
    const quint64 generation = generation_;
    executor_->execute(call.name, arguments.object(), source_,
                       [this, generation, call, timer](Result<ToolResult> result) {
        if (generation != generation_) return;  // cancelled meanwhile
        currentCall_.reset();
        const QString text = result ? result.value().text : u"Error: "_s + result.error().message;
        conversation_->addToolResult(call.id, call.name, text);
        if (result && result.value().image) pendingImages_.append(*result.value().image);
        emit toolFinished(call.name, result.isOk(), summarize(text), timer.elapsed());
        runNextTool();
    });
}

void Agent::closeOpenCalls(const QString &text) {
    if (currentCall_) conversation_->addToolResult(currentCall_->id, currentCall_->name, text);
    for (const ToolCall &call : std::as_const(pendingCalls_)) conversation_->addToolResult(call.id, call.name, text);
    currentCall_.reset();
    pendingCalls_.clear();
    pendingImages_.clear();
}

void Agent::stop(std::optional<Error> error, FinishReason reason) {
    if (!running_) return;
    closeOpenCalls(reason == FinishReason::Cancelled ? u"Cancelled by the user."_s : u"Not run."_s);
    ++generation_;
    running_ = false;
    stream_.reset();
    if (error) {
        qCDebug(lcAi) << "Agent failed:" << error->message;
        emit failed(*error);
    } else {
        emit finished(reason);
    }
}

} // namespace nexa

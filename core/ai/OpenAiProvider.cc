#include "core/ai/OpenAiProvider.h"

#include "core/Logging.h"
#include "core/net/HttpClient.h"
#include "core/net/SseParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kModelsTimeoutMs = 15'000;
// Inactivity limit for streams; reasoning models can think for a long time before the first token.
constexpr int kStreamIdleTimeoutMs = 300'000;

FinishReason finishReasonFrom(const QString &value) {
    if (value == u"stop") return FinishReason::Stop;
    if (value == u"length") return FinishReason::Length;
    if (value == u"tool_calls" || value == u"function_call") return FinishReason::ToolCalls;
    if (value == u"content_filter") return FinishReason::ContentFilter;
    return FinishReason::Other;
}

// "content" is normally a string; some servers send an array of typed parts.
QString contentText(const QJsonValue &content) {
    if (content.isString()) return content.toString();
    QString text;
    for (const QJsonValue &part : content.toArray()) {
        const QJsonObject object = part.toObject();
        if (object.value(u"type"_s).toString() == u"text") text += object.value(u"text"_s).toString();
    }
    return text;
}

// "arguments" is a JSON string per the spec; a few servers send the object itself.
QString argumentsText(const QJsonValue &arguments) {
    if (arguments.isString()) return arguments.toString();
    if (arguments.isObject()) {
        return QString::fromUtf8(QJsonDocument(arguments.toObject()).toJson(QJsonDocument::Compact));
    }
    return {};
}

QJsonObject toWire(const ChatMessage &message) {
    switch (message.role) {
    case Role::System:
        return {{u"role"_s, u"system"_s}, {u"content"_s, message.text}};
    case Role::User: {
        if (message.images.isEmpty()) return {{u"role"_s, u"user"_s}, {u"content"_s, message.text}};
        QJsonArray parts;
        if (!message.text.isEmpty()) parts.append(QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, message.text}});
        for (const ImageAttachment &image : message.images) {
            const QString url =
                u"data:%1;base64,%2"_s.arg(image.mimeType, QString::fromLatin1(image.data.toBase64()));
            parts.append(QJsonObject{{u"type"_s, u"image_url"_s}, {u"image_url"_s, QJsonObject{{u"url"_s, url}}}});
        }
        return {{u"role"_s, u"user"_s}, {u"content"_s, parts}};
    }
    case Role::Assistant: {
        QJsonObject wire{{u"role"_s, u"assistant"_s}};
        const bool onlyToolCalls = message.text.isEmpty() && !message.toolCalls.isEmpty();
        wire.insert(u"content"_s, onlyToolCalls ? QJsonValue(QJsonValue::Null) : QJsonValue(message.text));
        if (!message.toolCalls.isEmpty()) {
            QJsonArray calls;
            for (const ToolCall &call : message.toolCalls) {
                calls.append(QJsonObject{
                    {u"id"_s, call.id},
                    {u"type"_s, u"function"_s},
                    {u"function"_s, QJsonObject{{u"name"_s, call.name}, {u"arguments"_s, call.argumentsJson}}},
                });
            }
            wire.insert(u"tool_calls"_s, calls);
        }
        return wire;
    }
    case Role::Tool:
        return {{u"role"_s, u"tool"_s}, {u"tool_call_id"_s, message.toolCallId}, {u"content"_s, message.text}};
    }
    return {};
}

// Reads one /chat/completions reply. SSE streams are decoded as they arrive; error bodies and
// servers that ignore stream=true (plain JSON) are buffered and decoded at the end.
class OpenAiChatStream : public ChatStream {
public:
    explicit OpenAiChatStream(QNetworkReply *reply) : reply_(reply) {
        reply_->setParent(this);
        connect(reply_, &QNetworkReply::readyRead, this, [this] { onReadyRead(); });
        connect(reply_, &QNetworkReply::finished, this, [this] { onFinished(); });
    }

protected:
    void abortTransport() override { reply_->abort(); }

private:
    bool isBuffered() const {
        return HttpClient::httpStatus(reply_) >= 400
            || reply_->header(QNetworkRequest::ContentTypeHeader).toString().contains(u"application/json"_s);
    }

    void onReadyRead() {
        const QByteArray chunk = reply_->readAll();
        if (isDone()) return;
        if (isBuffered()) {
            buffered_ += chunk;
            return;
        }
        handleEvents(parser_.feed(chunk));
    }

    void onFinished() {
        if (isDone()) return;
        const QByteArray rest = reply_->readAll();
        if (reply_->error() != QNetworkReply::NoError || HttpClient::httpStatus(reply_) >= 400) {
            buffered_ += rest;
            fail(HttpClient::errorFor(reply_, buffered_));
            return;
        }
        if (isBuffered()) {
            buffered_ += rest;
            OpenAiProvider::Delta whole = OpenAiProvider::decodeChunk(buffered_);
            whole.done = true;
            apply(whole);
            return;
        }
        QList<SseEvent> events = parser_.feed(rest);
        events += parser_.finish();
        handleEvents(events);
        if (isDone()) return;
        // The server closed the stream without "[DONE]".
        if (finishReason_ || !text().isEmpty() || !accumulator_.isEmpty()) {
            complete(finishReason_.value_or(FinishReason::Stop), accumulator_.calls());
        } else {
            fail(Error{ErrorCode::Parse, u"The server closed the stream without a reply"_s});
        }
    }

    void handleEvents(const QList<SseEvent> &events) {
        for (const SseEvent &event : events) {
            if (isDone()) return;
            apply(OpenAiProvider::decodeChunk(event.data));
        }
    }

    void apply(const OpenAiProvider::Delta &delta) {
        if (delta.error) {
            fail(*delta.error);
            return;
        }
        emitText(delta.text);
        for (const ToolCallFragment &fragment : delta.toolFragments) {
            const int slot = accumulator_.add(fragment);
            emitToolCallDelta(slot, fragment.name, fragment.argumentsDelta);
        }
        if (delta.finishReason) finishReason_ = delta.finishReason;
        if (delta.done) complete(finishReason_.value_or(FinishReason::Stop), accumulator_.calls());
    }

    QNetworkReply *reply_;
    SseParser parser_;
    ToolCallAccumulator accumulator_;
    std::optional<FinishReason> finishReason_;
    QByteArray buffered_;
};

} // namespace

OpenAiProvider::OpenAiProvider(ProviderConfig config, HttpClient *http, QObject *parent)
    : IAiProvider(parent), config_(std::move(config)), http_(http) {}

IAiProvider::Capabilities OpenAiProvider::capabilities() const {
    // Vision depends on the model; requests with images surface the server's error if unsupported.
    return Capabilities{.streaming = true, .nativeTools = true, .vision = true};
}

void OpenAiProvider::listModels(std::function<void(Result<QStringList>)> done) {
    http_->get(HttpClient::jsonRequest(endpoint(u"/models"_s), config_.apiKey), kModelsTimeoutMs, this,
               [done = std::move(done)](Result<QByteArray> body) {
                   if (!body) {
                       done(body.error());
                       return;
                   }
                   done(parseModels(body.value()));
               });
}

std::shared_ptr<ChatStream> OpenAiProvider::chat(const ChatRequest &request) {
    ChatRequest effective = request;
    if (effective.model.isEmpty()) effective.model = config_.defaultModel;
    const QByteArray body = QJsonDocument(buildChatBody(effective, true)).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = http_->postStreaming(
        HttpClient::jsonRequest(endpoint(u"/chat/completions"_s), config_.apiKey), body, kStreamIdleTimeoutMs);
    qCDebug(lcAi) << "chat" << config_.id << effective.model << effective.messages.size() << "messages";
    return std::shared_ptr<ChatStream>(new OpenAiChatStream(reply), [](ChatStream *stream) {
        stream->deleteLater();
    });
}

QJsonObject OpenAiProvider::buildChatBody(const ChatRequest &request, bool stream) {
    QJsonArray messages;
    for (const ChatMessage &message : request.messages) messages.append(toWire(message));
    QJsonObject body{{u"model"_s, request.model}, {u"messages"_s, messages}, {u"stream"_s, stream}};
    if (!request.tools.isEmpty()) {
        QJsonArray tools;
        for (const ToolSpec &tool : request.tools) tools.append(tool.toOpenAi());
        body.insert(u"tools"_s, tools);
        body.insert(u"tool_choice"_s, u"auto"_s);
    }
    if (request.temperature >= 0) body.insert(u"temperature"_s, request.temperature);
    if (request.maxTokens > 0) body.insert(u"max_tokens"_s, request.maxTokens);
    return body;
}

OpenAiProvider::Delta OpenAiProvider::decodeChunk(const QByteArray &data) {
    Delta delta;
    const QByteArray payload = data.trimmed();
    if (payload == "[DONE]") {
        delta.done = true;
        return delta;
    }
    if (payload.isEmpty()) return delta;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcAi) << "Skipping malformed stream chunk:" << parseError.errorString();
        return delta;
    }
    const QJsonObject root = doc.object();
    if (root.contains(u"error"_s)) {
        delta.error = Error{ErrorCode::Http, HttpClient::serverMessage(payload)};
        return delta;
    }

    const QJsonArray choices = root.value(u"choices"_s).toArray();
    if (choices.isEmpty()) return delta;  // e.g. the trailing usage chunk
    const QJsonObject choice = choices.first().toObject();
    // Streams carry "delta"; a server that ignores stream=true answers with a whole "message".
    const QJsonObject message = choice.contains(u"delta"_s) ? choice.value(u"delta"_s).toObject()
                                                            : choice.value(u"message"_s).toObject();
    delta.text = contentText(message.value(u"content"_s));

    const QJsonArray toolCalls = message.value(u"tool_calls"_s).toArray();
    for (qsizetype i = 0; i < toolCalls.size(); ++i) {
        const QJsonObject call = toolCalls.at(i).toObject();
        const QJsonObject function = call.value(u"function"_s).toObject();
        delta.toolFragments.append(ToolCallFragment{
            .index = call.contains(u"index"_s) ? call.value(u"index"_s).toInt() : int(i),
            .id = call.value(u"id"_s).toString(),
            .name = function.value(u"name"_s).toString(),
            .argumentsDelta = argumentsText(function.value(u"arguments"_s)),
        });
    }

    const QJsonValue finish = choice.value(u"finish_reason"_s);
    if (finish.isString()) delta.finishReason = finishReasonFrom(finish.toString());
    return delta;
}

Result<QStringList> OpenAiProvider::parseModels(const QByteArray &body) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return Error{ErrorCode::Parse, u"The model list is not valid JSON: %1"_s.arg(parseError.errorString())};
    }
    const QJsonArray entries = doc.isArray() ? doc.array() : doc.object().value(u"data"_s).toArray();
    QStringList ids;
    for (const QJsonValue &entry : entries) {
        const QString id = entry.isObject() ? entry.toObject().value(u"id"_s).toString() : entry.toString();
        if (!id.isEmpty()) ids.append(id);
    }
    ids.removeDuplicates();
    ids.sort(Qt::CaseInsensitive);
    return ids;
}

QUrl OpenAiProvider::endpoint(const QString &path) const {
    QString base = config_.baseUrl.trimmed();
    while (base.endsWith(u'/')) base.chop(1);
    return QUrl(base + path);
}

} // namespace nexa
